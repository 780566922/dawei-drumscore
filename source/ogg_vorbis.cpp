//==============================================================================
// ogg_vorbis.cpp — Ogg Vorbis 解码（三端共用，见 ogg_vorbis.h 的设计说明）
//
// 解码器：stb_vorbis v1.22（公共领域 / MIT-0）
//   · 唯一一处 STB_VORBIS_IMPLEMENTATION 实现单元放在这里，避免多 TU 重复编译
//   · 直接 #include .c 按 C++ 编译（stb_vorbis 自身有 __cplusplus 保护，
//     官方也支持这么用）
//
// 文件读取为什么自己写而不用 stb 自带的 stdio API：
//   stb 的 stb_vorbis_open_filename() 内部是 fopen(const char*)，Windows 上
//   只认 ANSI 代码页 —— 中文路径 / 文件名会直接打不开。现有三端解码器收到的
//   都是 UTF-8 路径，所以这里自己按平台读文件（Windows 走 UTF-8 → UTF-16 +
//   _wfopen），再交给 stb_vorbis_open_memory()。顺带也统一了错误提示。
//==============================================================================
#include "ogg_vorbis.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "third_party/stb_vorbis.c"

namespace ap {

namespace {

// 每次解码 2048 帧：够大以减少循环开销，临时缓冲又不会太大。
// （stb_vorbis 内部本身有整页缓冲，这里只是我们搬运的粒度。）
constexpr int kChunkFrames = 2048;

// 上限保护：与 macOS / Windows / Linux 其它解码路径同一阈值
// （约 2 小时 @48k 立体声），防止误加载超大文件把内存打爆。
constexpr size_t kMaxFrames = 48000ull * 60ull * 60ull * 2ull;

// 压缩文件本身的大小上限。Ogg 是压缩格式，正常伴奏几 MB 而已；
// 这里只为避免有人把几个 GB 的东西改后缀拖进来后一次性读进内存。
constexpr long kMaxFileBytes = 512L * 1024L * 1024L;

//------------------------------------------------------------------------------
// UTF-8 → UTF-16（Windows 打文件用）。失败返回空串。
//------------------------------------------------------------------------------
#ifdef _WIN32
std::wstring utf8ToWide (const std::string& s)
{
    if (s.empty ())
        return std::wstring ();

    const int need = ::MultiByteToWideChar (CP_UTF8, 0, s.c_str (),
                                            static_cast<int> (s.size ()), nullptr, 0);
    if (need <= 0)
        return std::wstring ();

    std::wstring w (static_cast<size_t> (need), L'\0');
    const int got = ::MultiByteToWideChar (CP_UTF8, 0, s.c_str (),
                                           static_cast<int> (s.size ()), &w[0], need);
    if (got <= 0)
        return std::wstring ();
    return w;
}
#endif

//------------------------------------------------------------------------------
// 读整个文件到内存。注意路径是 UTF-8。
//------------------------------------------------------------------------------
bool readWholeFile (const std::string& path, std::vector<unsigned char>& bytes, std::string& err)
{
#ifdef _WIN32
    const std::wstring w = utf8ToWide (path);
    if (w.empty ())
    {
        err = "文件路径包含非法字符";
        return false;
    }
    std::FILE* f = ::_wfopen (w.c_str (), L"rb");
#else
    std::FILE* f = std::fopen (path.c_str (), "rb");
#endif
    if (!f)
    {
        err = "无法打开文件（不存在或没有读取权限）";
        return false;
    }

    if (std::fseek (f, 0, SEEK_END) != 0)
    {
        std::fclose (f);
        err = "无法读取文件（定位末尾失败）";
        return false;
    }
    const long sz = std::ftell (f);
    std::fseek (f, 0, SEEK_SET);

    if (sz <= 0 || sz > kMaxFileBytes)
    {
        std::fclose (f);
        err = (sz <= 0) ? "文件是空的" : "文件过大（超过 512 MB），无法作为音频载入";
        return false;
    }

    bytes.resize (static_cast<size_t> (sz));
    const size_t got = std::fread (bytes.data (), 1, bytes.size (), f);
    std::fclose (f);

    if (got != bytes.size ())
    {
        bytes.clear ();
        err = "读取文件时出错（可能被其它程序占用）";
        return false;
    }
    return true;
}

//------------------------------------------------------------------------------
// 在开头一小段里找 ASCII 标记。用于给「是 Ogg，但不是 Vorbis」的情况
// 给出更准确的提示 —— 用户最常见的混淆就是 Ogg Opus 和 Ogg Vorbis。
//------------------------------------------------------------------------------
bool containsAscii (const unsigned char* data, size_t len, const char* needle)
{
    const size_t nl = std::strlen (needle);
    if (!data || len < nl)
        return false;
    for (size_t i = 0; i + nl <= len; ++i)
        if (std::memcmp (data + i, needle, nl) == 0)
            return true;
    return false;
}

//------------------------------------------------------------------------------
// stb_vorbis 的失败原因（英文常量）→ 用户能看懂的中文
//------------------------------------------------------------------------------
std::string openErrorText (int code)
{
    switch (code)
    {
        case VORBIS_need_more_data:            return "文件不完整（数据被截断）";
        case VORBIS_outofmem:                  return "解码时内存不足";
        case VORBIS_feature_not_supported:     return "该文件使用了旧版 Vorbis 特性（floor 0，2004 年前的编码器）";
        case VORBIS_too_many_channels:         return "声道数过多，超出解码器上限";
        case VORBIS_file_open_failure:         return "无法打开文件";
        case VORBIS_unexpected_eof:            return "文件意外结束（下载不完整？）";
        case VORBIS_invalid_setup:             return "Vorbis 头部数据无效（文件可能损坏）";
        case VORBIS_invalid_stream:            return "Vorbis 数据流无效（文件可能损坏）";
        case VORBIS_invalid_first_page:        return "不是有效的 Ogg 文件（首个数据页校验失败）";
        case VORBIS_bad_packet_type:           return "数据包类型异常（文件可能损坏）";
        case VORBIS_cant_find_last_page:       return "找不到最后一个数据页（文件被截断）";
        case VORBIS_seek_failed:               return "定位数据失败（文件结构异常）";
        default: break;
    }
    return "无法解析 Ogg Vorbis 数据（文件损坏或不是 Vorbis 编码）";
}

//------------------------------------------------------------------------------
// 交错多声道 float → 交错双声道
//   mono    → 复制到左右两路
//   >= 2 声道 → 取前两路
// 与 audiofile_linux.cpp 的同名逻辑保持一致（各平台行为必须一样）。
//------------------------------------------------------------------------------
void appendFrames (const float* const* chan, int ch, int frames, std::vector<float>& out)
{
    if (!chan || frames <= 0 || ch <= 0)
        return;

    out.reserve (out.size () + static_cast<size_t> (frames) * 2);
    for (int i = 0; i < frames; ++i)
    {
        const float l = chan[0][i];
        out.push_back (l);
        out.push_back (ch >= 2 ? chan[1][i] : l);
    }
}

//------------------------------------------------------------------------------
bool decodeLocked (const std::string& path, AudioData& out, std::string& err)
{
    std::vector<unsigned char> bytes;
    if (!readWholeFile (path, bytes, err))
        return false;

    // 至少要能容下 Ogg 页头，否则连"是不是 Ogg"都判断不了
    if (bytes.size () < 27 || std::memcmp (bytes.data (), "OggS", 4) != 0)
    {
        err = "不是有效的 Ogg 文件（缺少 OggS 文件头）";
        return false;
    }

    // 是 Ogg 家族但不是 Vorbis —— 给准确提示，别让用户以为是文件坏了
    const size_t probe = bytes.size () < 1024 ? bytes.size () : 1024;
    if (containsAscii (bytes.data (), probe, "OpusHead"))
    {
        err = "这是 Ogg Opus 文件（不是 Ogg Vorbis），本插件暂不支持 Opus，请转成 MP3";
        return false;
    }
    if (containsAscii (bytes.data (), probe, "fLaC"))
    {
        err = "这是 Ogg 封装的 FLAC（不是 Ogg Vorbis），请直接使用 .flac 原文件";
        return false;
    }

    int openErr = 0;
    stb_vorbis* v = stb_vorbis_open_memory (bytes.data (),
                                            static_cast<int> (bytes.size ()),
                                            &openErr, nullptr);
    if (!v)
    {
        err = openErrorText (openErr);
        return false;
    }

    const stb_vorbis_info info = stb_vorbis_get_info (v);
    const int ch = info.channels;

    if (info.sample_rate <= 0 || ch <= 0)
    {
        stb_vorbis_close (v);
        err = "Vorbis 头部信息异常（采样率或声道数为 0）";
        return false;
    }

    out.sampleRate  = static_cast<uint32_t> (info.sample_rate);
    out.numChannels = 2;

    // 平面缓冲：把一整块按声道切开，交给 stb 填。
    // 这里【始终按文件原生声道数索取】，通道合并（mono → 左右两路、
    // 多声道 → 取前两路）由上面的 appendFrames 负责 —— 因为 stb 的
    // 通道转换规则是「多余的通道补 0」，直接把 mono 当 2 声道要的话
    // 右声道会变成静音，跟我们其它格式的行为不一致。
    std::vector<float> planar (static_cast<size_t> (kChunkFrames) * static_cast<size_t> (ch));
    std::vector<float*> chanPtrs (static_cast<size_t> (ch));
    for (int i = 0; i < ch; ++i)
        chanPtrs[static_cast<size_t> (i)] = planar.data () + static_cast<size_t> (i) * kChunkFrames;

    // 估算总帧数，先留出空间减少扩容（Vorbis 头里的总样本数可能是 -1，
    // 那种情况下就不预分配）。
    const long total = stb_vorbis_stream_length_in_samples (v);
    if (total > 0)
    {
        size_t frames = static_cast<size_t> (total);
        if (frames > kMaxFrames)
            frames = kMaxFrames;
        out.samples.reserve (frames * 2);
    }

    bool any = false, tooLong = false;
    for (;;)
    {
        const int got = stb_vorbis_get_samples_float (v, ch, chanPtrs.data (), kChunkFrames);
        if (got <= 0)
            break;

        appendFrames (chanPtrs.data (), ch, got, out.samples);
        any = true;
        if (out.samples.size () / 2 > kMaxFrames) { tooLong = true; break; }
    }

    stb_vorbis_close (v);

    if (!any)
    {
        out.samples.clear ();
        err = "解码后没有音频数据（文件可能只有头部）";
        return false;
    }
    if (out.samples.size () % 2 != 0)
        out.samples.pop_back ();
    if (tooLong)
        err = "音频超过 2 小时，已截断加载";
    return true;
}

} // namespace

//------------------------------------------------------------------------------
bool isOggExtension (const std::string& path)
{
    const size_t dot = path.find_last_of ('.');
    const size_t slash = path.find_last_of ("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return false;

    std::string e = path.substr (dot);
    for (char& c : e)
        c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));

    return e == ".ogg" || e == ".oga";
}

//------------------------------------------------------------------------------
bool decodeOggVorbis (const std::string& path, AudioData& out, std::string& err)
{
    out.samples.clear ();

    if (path.empty ())
    {
        err = "文件路径为空";
        return false;
    }

    // 插件里绝不能让异常穿到宿主 —— 一条漏网的异常在 macOS/Windows 上
    // 都足以把 MuseScore 带走。这里把 bad_alloc 一类也收敛成"载入失败"。
    try
    {
        return decodeLocked (path, out, err);
    }
    catch (const std::bad_alloc&)
    {
        out.samples.clear ();
        err = "内存不足，无法解码该文件";
        return false;
    }
    catch (const std::exception& e)
    {
        out.samples.clear ();
        err = std::string ("解码时发生异常：") + e.what ();
        return false;
    }
    catch (...)
    {
        out.samples.clear ();
        err = "解码时发生未知异常";
        return false;
    }
}

} // namespace ap
