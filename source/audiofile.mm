//==============================================================================
// audiofile.cpp - 音频解码（AVFoundation）
//==============================================================================
#include "audiofile.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <new>

namespace ap {

//------------------------------------------------------------------------------
// 解码整个文件到交错 float 缓冲。
//
// 关键改动（原先这条路会崩宿主）：
//   旧实现先用 AVAudioFile 拿 processingFormat，再自己造一个 AVAudioConverter
//   做「源格式 → interleaved float32」的转换。这条路上有两个坑：
//     1) convertToBuffer 的输入回调是值捕获，导致每次回调都声称"还有数据"，
//        把同一个缓冲反复投喂给转换器；
//     2) 格式组合稍特殊一点，AVAudioConverter 会直接抛 ObjC 异常——而插件
//        里没人接，异常穿到宿主就成了闪退。
//
//   现在改用 AVAudioFile 的 commonFormat 初始化：直接让它以 float32
//   非交错格式读文件，压缩格式解码、位深转换、声道布局全部由它内部完成。
//   我们只管把读出来的采样交错拼起来，代码路径更短、异常面更小。
//
//   整个解码过程外面套了 @try —— 任何 ObjC 异常都转成"载入失败"，
//   绝不让它穿出去把宿主带走。
//------------------------------------------------------------------------------
bool decodeAudioFile(const std::string& path, AudioData& out, std::string& err)
{
    @autoreleasepool
    {
        if (path.empty())
        {
            err = "文件路径为空";
            return false;
        }

        NSString* nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsPath)
        {
            err = "路径包含非法字符";
            return false;
        }

        NSURL* url = [NSURL fileURLWithPath:nsPath];
        if (!url)
        {
            err = "无法构造文件 URL";
            return false;
        }

        @try
        {
            NSError* nsError = nil;

            // 直接以 float32 非交错读取：解码与格式转换都在 AVAudioFile 内部完成。
            AVAudioFile* file = [[AVAudioFile alloc] initForReading:url
                                                       commonFormat:AVAudioPCMFormatFloat32
                                                        interleaved:NO
                                                             error:&nsError];
            if (!file)
            {
                // localizedDescription 非 nil 不代表 UTF8String 非 NULL，
                // 直接 std::string(NULL) 会崩。逐层判空。
                const char* msg = nsError.localizedDescription
                                      ? [nsError.localizedDescription UTF8String] : nullptr;
                err = msg ? std::string(msg)
                          : std::string("无法打开音频文件（格式不支持或文件损坏）");
                return false;
            }

            AVAudioFormat* fmt = file.processingFormat;
            if (!fmt || fmt.channelCount == 0 || fmt.sampleRate <= 0)
            {
                err = "音频格式信息无效";
                return false;
            }

            const uint64_t totalFrames = static_cast<uint64_t>(file.length);
            if (totalFrames == 0)
            {
                err = "音频文件为空（0 帧）";
                return false;
            }

            // 上限保护：防止误加载超大文件导致内存爆掉。
            // 2 小时 48k 立体声 float ≈ 2.7GB，明显异常，直接拒绝。
            const uint64_t maxFrames = 48000ull * 60ull * 60ull * 2ull;
            if (totalFrames > maxFrames)
            {
                err = "音频文件过长（超过 2 小时），已拒绝加载";
                return false;
            }

            const uint32_t srcCh = static_cast<uint32_t>(fmt.channelCount);

            out.samples.clear();
            out.sampleRate = static_cast<uint32_t>(fmt.sampleRate + 0.5);
            out.numChannels = 2;

            // reserve 失败会抛 std::length_error / std::bad_alloc。
            // 这里就地吸收掉——异常一旦穿出到 VST3 边界就是 std::terminate。
            try
            {
                out.samples.reserve(static_cast<size_t>(totalFrames) * 2);
            }
            catch (const std::exception&)
            {
                err = "内存不足：音频文件过大";
                out.samples.clear();
                return false;
            }

            // 每次读 4096 帧，控制内存峰值
            constexpr AVAudioFrameCount kChunk = 4096;
            AVAudioPCMBuffer* buf = [[AVAudioPCMBuffer alloc] initWithPCMFormat:fmt
                                                                 frameCapacity:kChunk];
            if (!buf)
            {
                err = "内存分配失败";
                return false;
            }

            while (true)
            {
                if (![file readIntoBuffer:buf error:&nsError])
                    break;

                const AVAudioFrameCount n = buf.frameLength;
                if (n == 0)
                    break;

                float* const* chans = buf.floatChannelData;
                if (!chans || !chans[0])
                    break;

                const float* L = chans[0];
                const float* R = (srcCh >= 2 && chans[1]) ? chans[1] : nullptr;

                // 非交错 → 交错；单声道源复制成两路（渲染端只认双声道，省掉分支）
                for (AVAudioFrameCount i = 0; i < n; ++i)
                {
                    const float l = L[i];
                    out.samples.push_back(l);
                    out.samples.push_back(R ? R[i] : l);
                }
            }

            if (out.samples.empty())
            {
                err = "解码后没有音频数据";
                return false;
            }

            // 对齐到完整帧
            if (out.samples.size() % 2 != 0)
                out.samples.pop_back();

            return true;
        }
        @catch (NSException* e)
        {
            const char* name = e.name ? e.name.UTF8String : nullptr;
            const char* why  = e.reason ? e.reason.UTF8String : nullptr;
            err = std::string("解码失败：") + (name ? name : "未知异常") +
                  (why ? (std::string(" - ") + why) : std::string());
            out.samples.clear();
            return false;
        }
        @catch (...)
        {
            err = "解码失败：未预期的异常";
            out.samples.clear();
            return false;
        }
    }
}

//------------------------------------------------------------------------------
bool isSupportedAudioExtension(const std::string& path)
{
    static const char* kExt[] = { ".mp3", ".wav", ".m4a", ".aac", ".alac",
                                  ".aiff", ".aif", ".caf", ".flac", ".mp4", ".m4b" };
    const size_t n = path.size();
    std::string lower;
    lower.reserve(n);
    for (size_t i = 0; i < n; ++i)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(path[i]))));

    for (const char* e : kExt)
    {
        const size_t el = std::strlen(e);
        if (lower.size() > el && lower.compare(lower.size() - el, el, e) == 0)
            return true;
    }
    return false;
}

} // namespace ap
