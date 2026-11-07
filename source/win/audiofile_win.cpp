//==============================================================================
// audiofile_win.cpp — 音频解码（Windows / Media Foundation）
//
// 对应 macOS 的 audiofile.mm。对外提供完全相同的两个符号：
//   ap::decodeAudioFile(path, out, err)
//   ap::isSupportedAudioExtension(path)
//
// 为什么用 Media Foundation：
//   它是 Windows 自带的媒体框架（Vista+），解码器全部内置，
//   零第三方依赖 —— 与 macOS 端用 AVFoundation 的取舍一致。
//   覆盖 MP3 / WAV / M4A(AAC) / WMA / FLAC(Win10+) 等；
//   系统装了对应 codec 的格式也能解。
//
// 例外：Ogg Vorbis 不走 MF。MF 内置解码器里【没有】Vorbis（微软把它放在
//   商店的可选包「Web Media Extensions」里，默认不装），所以这一格必须自带
//   解码器 —— 走三端共用的 ogg_vorbis.cpp（stb_vorbis）。见 ogg_vorbis.h。
//
// 路径编码：源路径是 UTF-8（std::string），MF 只认 UTF-16，
//   所以这里统一做 UTF-8 → UTF-16 转换，中文路径/文件名才能正常打开。
//==============================================================================
#include "audiofile.h"
#include "ogg_vorbis.h"
#include "crashguard.h"   // 解码阶段的阶段日志（Windows 上由 InitDll 打开日志文件）

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <objbase.h>   // CoInitializeEx：Media Foundation 的线程前提

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace ap {

namespace {

//------------------------------------------------------------------------------
// UTF-8 → UTF-16。失败返回空串。
//------------------------------------------------------------------------------
std::wstring utf8ToWide (const std::string& s)
{
    if (s.empty ())
        return std::wstring ();
    const int n = ::MultiByteToWideChar (CP_UTF8, 0, s.c_str (),
                                         static_cast<int> (s.size ()), nullptr, 0);
    if (n <= 0)
        return std::wstring ();
    std::wstring w (static_cast<size_t> (n), L'\0');
    ::MultiByteToWideChar (CP_UTF8, 0, s.c_str (), static_cast<int> (s.size ()), &w[0], n);
    return w;
}

//------------------------------------------------------------------------------
// COM：Media Foundation 只在「本线程已初始化 COM」的前提下可用。
//
// 宿主的线程不保证初始化过 COM，尤其「打开工程时恢复插件状态」走的是后台
// 加载线程。旧版完全没做这一步 —— MFStartup 在那个线程上直接失败，而且失败
// 结果被 static 缓存住，于是之后用户在正常线程里点「打开音频」也永远得到
// 「Media Foundation 初始化失败（系统组件缺失）」，插件从此再也出不了声。
//
// 用 COINIT_MULTITHREADED：MF 的原生要求是 MTA。若该线程已经是 STA，
// CoInitializeEx 返回 RPC_E_CHANGED_MODE —— 这说明 COM 已经初始化过，
// 照样可用（同步 ReadSample 不需要消息泵），只是不要再去 CoUninitialize。
//------------------------------------------------------------------------------
class ComScope
{
public:
    ComScope () : m_hr (::CoInitializeEx (nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope ()
    {
        // 只在本对象真正完成初始化时才配套反初始化（RPC_E_CHANGED_MODE 不是我们初始化的）
        if (SUCCEEDED (m_hr))
            ::CoUninitialize ();
    }
    bool usable () const
    {
        return SUCCEEDED (m_hr) || m_hr == RPC_E_CHANGED_MODE || m_hr == S_FALSE;
    }

private:
    HRESULT m_hr;
};

//------------------------------------------------------------------------------
// Media Foundation 进程内只需启动一次。
// MFSTARTUP_NOSOCKET：插件不需要网络流，少拉起一个子系统。
//
// ⚠️ 只缓存【成功】。旧写法把失败也缓存了（started = true），一次失败的
//    尝试就等于永久禁用解码 —— 而第一次尝试往往发生在最不利的时机
//    （宿主恢复状态的后台线程）。
//------------------------------------------------------------------------------
bool ensureMfStarted ()
{
    static std::atomic<int> started {0};   // 0 = 尚未成功，1 = 已成功
    if (started.load (std::memory_order_acquire) == 1)
        return true;

    const HRESULT hr = ::MFStartup (MF_VERSION, MFSTARTUP_NOSOCKET);
    if (SUCCEEDED (hr))
    {
        started.store (1, std::memory_order_release);
        return true;
    }
    ap::crashLog ("MFStartup 失败（hr=0x%08lx），下次解码时重试", static_cast<unsigned long> (hr));
    return false;
}

//------------------------------------------------------------------------------
// 打开 SourceReader。优先直接用路径；失败再试 file:/// 形式
// （个别路径形态下 MF 的 URL 解析器会挑食）。
//------------------------------------------------------------------------------
HRESULT createReader (const std::wstring& wpath, IMFSourceReader** out)
{
    // 显式允许 Source Reader 自动插入解码器 + 格式转换器
    // （MF_READWRITE_DISABLE_CONVERTERS 默认即为 FALSE，此处声明意图，便于日后排查）
    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED (::MFCreateAttributes (&attrs, 1)) && attrs)
    {
        attrs->SetUINT32 (MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    }

    HRESULT hr = ::MFCreateSourceReaderFromURL (wpath.c_str (), attrs, out);
    if (FAILED (hr) || !*out)
    {
        // 回退：拼成 file:/// 形式再试一次
        std::wstring url = L"file:///";
        for (wchar_t c : wpath)
            url.push_back (c == L'\\' ? L'/' : c);
        hr = ::MFCreateSourceReaderFromURL (url.c_str (), attrs, out);
    }

    if (attrs)
        attrs->Release ();
    return hr;
}

//------------------------------------------------------------------------------
// 把一段原始 PCM 缓冲转成「交错双声道 float」
//------------------------------------------------------------------------------
void appendSamples (const BYTE* data, DWORD bytes, bool isFloat, UINT32 bits,
                    UINT32 channels, std::vector<float>& out)
{
    if (!data || bytes == 0 || channels == 0)
        return;

    const UINT32 bytesPerSample = bits / 8;
    if (bytesPerSample == 0)
        return;
    const UINT32 frameBytes = bytesPerSample * channels;
    if (frameBytes == 0)
        return;

    const size_t frames = bytes / frameBytes;

    for (size_t i = 0; i < frames; ++i)
    {
        float l = 0.0f, r = 0.0f;

        if (isFloat && bytesPerSample == 4)
        {
            const float* f = reinterpret_cast<const float*> (data + i * frameBytes);
            l = f[0];
            r = (channels >= 2) ? f[1] : f[0];
        }
        else if (!isFloat && bits == 16)
        {
            const int16_t* s = reinterpret_cast<const int16_t*> (data + i * frameBytes);
            l = static_cast<float> (s[0]) / 32768.0f;
            r = (channels >= 2) ? static_cast<float> (s[1]) / 32768.0f : l;
        }
        else if (!isFloat && bits == 32)
        {
            const int32_t* s = reinterpret_cast<const int32_t*> (data + i * frameBytes);
            l = static_cast<float> (static_cast<double> (s[0]) / 2147483648.0);
            r = (channels >= 2) ? static_cast<float> (static_cast<double> (s[1]) / 2147483648.0) : l;
        }
        else if (!isFloat && bits == 8)
        {
            const uint8_t* s = data + i * frameBytes;
            l = (static_cast<float> (s[0]) - 128.0f) / 128.0f;
            r = (channels >= 2) ? (static_cast<float> (s[1]) - 128.0f) / 128.0f : l;
        }
        else
        {
            return;   // 未知位深，放弃（避免输出垃圾）
        }

        out.push_back (l);
        out.push_back (r);
    }
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

    ap::crashLog ("开始解码: %s", path.c_str ());

    // Ogg Vorbis 交给自带的 stb_vorbis。放在 MF 初始化之前：
    // 解 OGG 根本用不到 Media Foundation，不该为它付初始化成本
    //（更要紧的是：MF 那条路必然是失败的，白跑一遍还会覆盖掉我们
    //  更准确的错误提示，比如「这是 Opus 不是 Vorbis」）。
    if (isOggExtension (path))
    {
        const bool ok = decodeOggVorbis (path, out, err);
        ap::crashLog ("OGG 解码%s%s%s", ok ? "成功" : "失败",
                      ok ? ": " : " —— ", ok ? path.c_str () : err.c_str ());
        return ok;
    }

    // COM 必须先就绪，否则 MFStartup / SourceReader 都会失败
    //（宿主的线程不保证初始化过 COM，见 ComScope 的说明）。
    ComScope com;
    if (!com.usable ())
    {
        err = "无法初始化音频解码所需的系统组件（COM）";
        ap::crashLog ("COM 初始化失败，放弃解码");
        return false;
    }

    if (!ensureMfStarted ())
    {
        err = "Media Foundation 初始化失败（系统组件缺失）";
        return false;
    }

    const std::wstring wpath = utf8ToWide (path);
    if (wpath.empty ())
    {
        err = "路径包含非法字符";
        return false;
    }

    IMFSourceReader* reader = nullptr;
    HRESULT hr = createReader (wpath, &reader);
    if (FAILED (hr) || !reader)
    {
        err = "无法打开音频文件（格式不支持或文件损坏）";
        return false;
    }

    // 只要第一条音频流
    reader->SetStreamSelection (MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection (MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    // 请求 float32 输出，让 MF 自动插入解码器 + 格式转换器
    IMFMediaType* reqType = nullptr;
    if (FAILED (::MFCreateMediaType (&reqType)) || !reqType)
    {
        reader->Release ();
        err = "创建媒体类型失败";
        return false;
    }
    reqType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    reqType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_Float);

    hr = reader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, reqType);
    if (FAILED (hr))
    {
        // 回退到 16-bit PCM（个别解码器不吐 float）
        reqType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM);
        reqType->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = reader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, reqType);
    }
    reqType->Release ();
    if (FAILED (hr))
    {
        reader->Release ();
        err = "该音频格式无法解码（系统缺少对应解码器）";
        return false;
    }

    // 取实际生效的输出格式
    IMFMediaType* actual = nullptr;
    hr = reader->GetCurrentMediaType (MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual);
    if (FAILED (hr) || !actual)
    {
        reader->Release ();
        err = "读取音频格式失败";
        return false;
    }

    UINT32 sampleRate = 0, channels = 0, bits = 0;
    actual->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
    actual->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &channels);
    actual->GetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);

    GUID sub = {};
    actual->GetGUID (MF_MT_SUBTYPE, &sub);
    const bool isFloat = (sub == MFAudioFormat_Float);

    if (isFloat && bits == 0)
        bits = 32;

    actual->Release ();

    if (sampleRate == 0 || channels == 0 || bits == 0)
    {
        reader->Release ();
        err = "音频格式信息无效";
        return false;
    }

    out.sampleRate = sampleRate;
    out.numChannels = 2;

    // 上限保护：防止误加载超大文件把内存打爆（与 macOS 端同一阈值）
    const size_t maxFrames = 48000ull * 60ull * 60ull * 2ull;

    bool anyData = false;
    bool tooLong = false;

    while (true)
    {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample (MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                 nullptr, &flags, nullptr, &sample);
        if (FAILED (hr))
            break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            if (sample)
                sample->Release ();
            break;
        }
        if (!sample)
            continue;

        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED (sample->ConvertToContiguousBuffer (&buf)) && buf)
        {
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (SUCCEEDED (buf->Lock (&data, &maxLen, &curLen)))
            {
                if (curLen > 0)
                {
                    appendSamples (data, curLen, isFloat, bits, channels, out.samples);
                    anyData = true;
                }
                buf->Unlock ();
            }
            buf->Release ();
        }
        sample->Release ();

        // 超长则截断并停止（不直接失败，用户至少能听到前 2 小时）
        if (out.samples.size () / 2 > maxFrames)
        {
            tooLong = true;
            break;
        }
    }

    reader->Release ();

    if (!anyData)
    {
        err = "解码后没有音频数据";
        out.samples.clear ();
        return false;
    }

    // 对齐到完整帧
    if (out.samples.size () % 2 != 0)
        out.samples.pop_back ();

    if (tooLong)
        err = "音频超过 2 小时，已截断加载";

    ap::crashLog ("解码完成: %zu 帧, %u Hz, 位深 %u, 源声道 %u",
                  out.numFrames (), out.sampleRate,
                  static_cast<unsigned> (bits), static_cast<unsigned> (channels));

    return true;
}

//------------------------------------------------------------------------------
// Windows 上支持的扩展名
//   MF 解得了的那些 + 我们自己带的 Ogg Vorbis（.ogg / .oga）
bool isSupportedAudioExtension (const std::string& path)
{
    static const char* kExt[] = { ".mp3", ".wav", ".m4a", ".aac", ".wma",
                                  ".mp4", ".m4b", ".flac", ".aif", ".aiff", ".adts",
                                  ".ogg", ".oga" };
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
