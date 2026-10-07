//==============================================================================
// decode_probe.mm — 解码路径独立探针
//
// 用真实的 decodeAudioFile 跑真实音频文件，并把可能被宿主吞掉的
// ObjC / C++ 异常显式抓出来打印。
//
// 为什么单独做这个：端到端测试只覆盖"渲染输出是否正确"，
// 覆盖不到"解码不同编码格式时会不会抛异常"。而解码正是载入音频
// 那一刻唯一会碰系统框架的地方。
//
// 用法：decode_probe <audio-file> [more...]
//==============================================================================
#include "audiofile.h"

#import <Foundation/Foundation.h>

#include <cstdio>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: decode_probe <audio-file> [more...]\n");
        return 2;
    }

    int failures = 0;

    for (int i = 1; i < argc; ++i)
    {
        const std::string path = argv[i];
        std::printf ("\n=== %s ===\n", path.c_str ());

        @try
        {
            ap::AudioData d;
            std::string err;
            const bool ok = ap::decodeAudioFile (path, d, err);
            std::printf ("result: ok=%d frames=%zu sampleRate=%u channels=%u err=\"%s\"\n",
                         ok ? 1 : 0,
                         d.numFrames (),
                         d.sampleRate,
                         d.numChannels,
                         err.c_str ());
            if (!ok)
                ++failures;
        }
        @catch (NSException* e)
        {
            std::printf ("!!! NSException: name=%s reason=%s\n",
                         e.name ? e.name.UTF8String : "(nil)",
                         e.reason ? e.reason.UTF8String : "(nil)");
            ++failures;
        }
        @catch (...)
        {
            std::printf ("!!! C++ exception escaped\n");
            ++failures;
        }
    }

    std::printf ("\n[probe done] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
