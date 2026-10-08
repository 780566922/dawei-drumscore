//==============================================================================
// audiofile_linux.cpp — 音频解码（Linux / dr_libs，零系统依赖）
//
// 对应 audiofile.mm（AVFoundation）与 audiofile_win.cpp（Media Foundation），
// 对外符号完全一致：
//   ap::decodeAudioFile(path, out, err)
//   ap::isSupportedAudioExtension(path)
//
// 为什么用 dr_libs（单文件公共领域库）而不是系统库：
//   Linux 没有统一的系统媒体框架。libsndfile 对 MP3 的支持要 1.1.0+，
//   而各发行版版本不一（Ubuntu 22.04 的 1.0.31 仍不支持 MP3），
//   用户端还得自行安装运行时库。把编解码器直接编进插件，用户【零依赖】，
//   下载即用 —— 与 Mac/Win 走系统原生解码一样省心。
//
// 覆盖格式：WAV / MP3 / FLAC（dr_libs）+ OGG Vorbis（自带 stb_vorbis）。
// 不支持：m4a / aac / wma（Linux 上零依赖方案覆盖不到，会给出明确提示，
//   而不是静默失败）。
//
// Ogg Vorbis 虽然也是「编进插件的编解码器」，但因为它三端系统框架都没有，
// 所以写成平台无关的共享源码 ogg_vorbis.cpp，被三端一起编译 —— 见 ogg_vorbis.h。
//==============================================================================
#include "audiofile.h"
#include "ogg_vorbis.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "third_party/dr_wav.h"
#include "third_party/dr_mp3.h"
#include "third_party/dr_flac.h"

namespace ap {

namespace {

enum class Fmt { Unknown, Wav, Mp3, Flac, Ogg };

// 每次解码 4096 帧：够大以减少循环开销，临时缓冲又不会太大
constexpr size_t kChunkFrames = 4096;

// 上限保护：与 macOS / Windows 端同一阈值（约 2 小时 @48k 立体声），
// 防止误加载超大文件把内存打爆。
constexpr size_t kMaxFrames = 48000ull * 60ull * 60ull * 2ull;

//------------------------------------------------------------------------------
// 取小写扩展名（含点）。无扩展名返回空串。
//------------------------------------------------------------------------------
std::string lowerExt (const std::string& path)
{
    const size_t dot = path.find_last_of ('.');
    const size_t slash = path.find_last_of ("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return std::string ();

    std::string e = path.substr (dot);
    for (char& c : e)
        c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return e;
}

Fmt formatOfExt (const std::string& ext)
{
    if (ext == ".wav" || ext == ".wave") return Fmt::Wav;
    if (ext == ".mp3")                   return Fmt::Mp3;
    if (ext == ".flac")                  return Fmt::Flac;
    if (ext == ".ogg" || ext == ".oga")  return Fmt::Ogg;
    return Fmt::Unknown;
}

//------------------------------------------------------------------------------
// 扩展名不可靠时（改过名 / 没有扩展名）按文件头嗅探。
// 多这一步是因为用户下载的伴奏常常被改名，扩展名和内容对不上。
//------------------------------------------------------------------------------
Fmt sniffFormat (const std::string& path)
{
    std::FILE* f = std::fopen (path.c_str (), "rb");
    if (!f)
        return Fmt::Unknown;

    unsigned char h[12] = {0};
    const size_t n = std::fread (h, 1, sizeof h, f);
    std::fclose (f);

    if (n >= 12 && std::memcmp (h, "RIFF", 4) == 0 && std::memcmp (h + 8, "WAVE", 4) == 0)
        return Fmt::Wav;
    if (n >= 4 && std::memcmp (h, "fLaC", 4) == 0)
        return Fmt::Flac;
    // Ogg 家族统一标记（Vorbis / Opus / FLAC 都是 "OggS" 开头）。
    // 具体是哪一个由 ogg_vorbis.cpp 打开后判断，认不出来时给的是
    // 「这是 Opus 不是 Vorbis」这类有信息量的提示。
    if (n >= 4 && std::memcmp (h, "OggS", 4) == 0)
        return Fmt::Ogg;
    if (n >= 3 && std::memcmp (h, "ID3", 3) == 0)
        return Fmt::Mp3;
    // MPEG 音频帧同步字：前 11 位全 1
    if (n >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0)
        return Fmt::Mp3;
    return Fmt::Unknown;
}

//------------------------------------------------------------------------------
// 交错多声道 float → 交错双声道
//   mono    → 复制到左右两路
//   >= 2 声道 → 取前两路
//------------------------------------------------------------------------------
void appendFrames (const float* src, size_t frames, unsigned ch, std::vector<float>& out)
{
    if (!src || frames == 0 || ch == 0)
        return;

    out.reserve (out.size () + frames * 2);
    for (size_t i = 0; i < frames; ++i)
    {
        const float* f = src + i * ch;
        out.push_back (f[0]);
        out.push_back (ch >= 2 ? f[1] : f[0]);
    }
}

//------------------------------------------------------------------------------
// 收尾：无数据算失败；帧对齐；超长只提示不失败
//------------------------------------------------------------------------------
bool finish (AudioData& out, bool anyData, bool tooLong, std::string& err)
{
    if (!anyData)
    {
        out.samples.clear ();
        err = "解码后没有音频数据";
        return false;
    }
    if (out.samples.size () % 2 != 0)
        out.samples.pop_back ();
    if (tooLong)
        err = "音频超过 2 小时，已截断加载";
    return true;
}

//------------------------------------------------------------------------------
bool decodeWav (const std::string& path, AudioData& out, std::string& err)
{
    drwav wav;
    if (!drwav_init_file (&wav, path.c_str (), nullptr))
    {
        err = "无法打开 WAV 文件（文件损坏或格式不受支持）";
        return false;
    }

    const unsigned ch = wav.channels;
    out.sampleRate = wav.sampleRate;
    out.numChannels = 2;

    std::vector<float> buf (kChunkFrames * (ch ? ch : 1));
    bool any = false, tooLong = false;

    for (;;)
    {
        const drwav_uint64 got =
            drwav_read_pcm_frames_f32 (&wav, static_cast<drwav_uint64> (kChunkFrames), buf.data ());
        if (got == 0)
            break;

        appendFrames (buf.data (), static_cast<size_t> (got), ch, out.samples);
        any = true;
        if (out.samples.size () / 2 > kMaxFrames) { tooLong = true; break; }
    }

    drwav_uninit (&wav);
    return finish (out, any, tooLong, err);
}

//------------------------------------------------------------------------------
bool decodeMp3 (const std::string& path, AudioData& out, std::string& err)
{
    drmp3 mp3;
    if (!drmp3_init_file (&mp3, path.c_str (), nullptr))
    {
        err = "无法打开 MP3 文件（文件损坏或格式不受支持）";
        return false;
    }

    const unsigned ch = mp3.channels;
    out.sampleRate = mp3.sampleRate;
    out.numChannels = 2;

    std::vector<float> buf (kChunkFrames * (ch ? ch : 1));
    bool any = false, tooLong = false;

    for (;;)
    {
        const drmp3_uint64 got =
            drmp3_read_pcm_frames_f32 (&mp3, static_cast<drmp3_uint64> (kChunkFrames), buf.data ());
        if (got == 0)
            break;

        appendFrames (buf.data (), static_cast<size_t> (got), ch, out.samples);
        any = true;
        if (out.samples.size () / 2 > kMaxFrames) { tooLong = true; break; }
    }

    drmp3_uninit (&mp3);
    return finish (out, any, tooLong, err);
}

//------------------------------------------------------------------------------
bool decodeFlac (const std::string& path, AudioData& out, std::string& err)
{
    drflac* flac = drflac_open_file (path.c_str (), nullptr);
    if (!flac)
    {
        err = "无法打开 FLAC 文件（文件损坏或格式不受支持）";
        return false;
    }

    const unsigned ch = flac->channels;
    out.sampleRate = flac->sampleRate;
    out.numChannels = 2;

    std::vector<float> buf (kChunkFrames * (ch ? ch : 1));
    bool any = false, tooLong = false;

    for (;;)
    {
        const drflac_uint64 got =
            drflac_read_pcm_frames_f32 (flac, static_cast<drflac_uint64> (kChunkFrames), buf.data ());
        if (got == 0)
            break;

        appendFrames (buf.data (), static_cast<size_t> (got), ch, out.samples);
        any = true;
        if (out.samples.size () / 2 > kMaxFrames) { tooLong = true; break; }
    }

    drflac_close (flac);
    return finish (out, any, tooLong, err);
}

} // namespace

//------------------------------------------------------------------------------
bool decodeAudioFile (const std::string& path, AudioData& out, std::string& err)
{
    out.samples.clear ();

    if (path.empty ())
    {
        err = "文件路径为空";
        return false;
    }

    const std::string ext = lowerExt (path);

    Fmt fmt = formatOfExt (ext);
    if (fmt == Fmt::Unknown)
        fmt = sniffFormat (path);     // 扩展名骗人时按内容判断

    switch (fmt)
    {
        case Fmt::Wav:  return decodeWav  (path, out, err);
        case Fmt::Mp3:  return decodeMp3  (path, out, err);
        case Fmt::Flac: return decodeFlac (path, out, err);
        case Fmt::Ogg:  return decodeOggVorbis (path, out, err);
        default: break;
    }

    if (ext.empty ())
        err = "无法识别的音频文件（Linux 版支持 MP3 / WAV / FLAC / OGG）";
    else
        err = "Linux 版暂不支持 " + ext + " 格式，请转换为 MP3 / WAV / FLAC / OGG";
    return false;
}

//------------------------------------------------------------------------------
bool isSupportedAudioExtension (const std::string& path)
{
    static const char* kExt[] = { ".mp3", ".wav", ".wave", ".flac", ".ogg", ".oga" };

    std::string lower;
    lower.reserve (path.size ());
    for (char c : path)
        lower.push_back (static_cast<char> (std::tolower (static_cast<unsigned char> (c))));

    for (const char* e : kExt)
    {
        const size_t el = std::strlen (e);
        if (lower.size () > el && lower.compare (lower.size () - el, el, e) == 0)
            return true;
    }
    return false;
}

} // namespace ap
