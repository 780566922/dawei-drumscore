//==============================================================================
// race_test.cpp — 播放核心并发安全测试
//
// 背景（这是宿主闪退的根因）：
//   在 MuseScore 里插件始终处于激活状态，音频线程持续调用 render()。
//   用户在界面上载入音频文件时，GUI 线程会替换播放器内部的音频数据。
//   这两条路径若不同步，音频线程就会读到已经被释放的内存 → 整个宿主闪退。
//
// 这个测试让「音频线程」和「GUI 线程」真的并发跑起来互相踩。
//
// 编译（严格模式，带 ThreadSanitizer）：
//   clang++ -std=c++17 -fsanitize=thread -g -O1 \
//     race_test.cpp source/player.cpp -Isource -o build/race_test
//==============================================================================
#include "audiofile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace ap;

static std::atomic<bool> g_stop {false};
static std::atomic<uint64_t> g_blocks {0};
static std::atomic<uint64_t> g_loads {0};

//------------------------------------------------------------------------------
// 「音频线程」：像宿主那样不停要音频块
//------------------------------------------------------------------------------
static void audioThread (Player& p)
{
    constexpr uint32_t kFrames = 1024;
    std::vector<float> l (kFrames), r (kFrames);

    while (!g_stop.load (std::memory_order_relaxed))
    {
        float* pl = l.data ();
        float* pr = r.data ();
        p.render (&pl, &pr, kFrames, 44100.0);
        g_blocks.fetch_add (1, std::memory_order_relaxed);
    }
}

//------------------------------------------------------------------------------
// 「GUI 线程」：不停换文件、读波形、拖播放头
//------------------------------------------------------------------------------
static AudioData makeData (size_t frames, uint32_t sr, float freq)
{
    AudioData d;
    d.sampleRate = sr;
    d.numChannels = 2;
    d.samples.resize (frames * 2);
    for (size_t i = 0; i < frames; ++i)
    {
        const float v = std::sin (2.0f * 3.14159265f * freq * float (i) / float (sr)) * 0.5f;
        d.samples[i * 2]     = v;
        d.samples[i * 2 + 1] = v;
    }
    return d;
}

static void guiThread (Player& p)
{
    for (int round = 0; round < 400; ++round)
    {
        const size_t frames = 50000 + size_t (round % 7) * 30000;
        const uint32_t sr = (round % 2) ? 44100u : 48000u;
        p.setAudio (makeData (frames, sr, 220.0f + float (round % 100)));

        // 复刻 APlayProcessor::waveformPeaks 的做法：在锁内遍历音频缓冲。
        // 修复前这里是「拿引用再遍历」，正是踩野内存的那条路径。
        std::vector<float> peaks (256, 0.f);
        p.withAudio ([&] (const AudioData& d)
        {
            const size_t n = d.numFrames ();
            const uint32_t ch = d.numChannels;
            if (n == 0 || ch == 0) return;
            const size_t per = n / peaks.size ();
            if (per == 0) return;
            for (size_t i = 0; i < peaks.size (); ++i)
            {
                float pk = 0.f;
                const size_t s1 = std::min ((i + 1) * per, n);
                for (size_t j = i * per; j < s1; ++j)
                {
                    float v = d.samples[j * ch];
                    if (ch >= 2) v = (v + d.samples[j * ch + 1]) * 0.5f;
                    pk = std::max (pk, std::fabs (v));
                }
                peaks[i] = pk;
            }
        });

        p.seekTo (double (round % 10));
        (void) p.positionSec ();
        (void) p.durationSec ();
        (void) p.hasAudio ();

        p.setStartOffsetSec (float (round % 5));   // 会让音频线程重算读位置
        p.setVolume (0.5f);
        p.setPlaying ((round % 3) != 0);
        p.setLooping ((round % 4) == 0);

        g_loads.fetch_add (1, std::memory_order_relaxed);
        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }
    g_stop.store (true);
}

//------------------------------------------------------------------------------
int main ()
{
    std::printf ("\n=========== 播放核心并发安全测试 ===========\n");

    Player p;
    p.setAudio (makeData (44100, 44100, 440.0f));

    std::thread a (audioThread, std::ref (p));
    std::thread g (guiThread, std::ref (p));

    g.join ();
    a.join ();
    g_stop.store (true);

    const uint64_t blocks = g_blocks.load ();
    const uint64_t loads  = g_loads.load ();

    std::printf ("音频线程渲染 %llu 块，GUI 线程载入 %llu 次\n",
                 (unsigned long long) blocks, (unsigned long long) loads);

    const bool ok = (loads >= 400) && (blocks > 0);
    std::printf ("%s 并发执行期间无崩溃\n", ok ? "[PASS]" : "[FAIL]");
    std::printf ("===========================================\n\n");
    return ok ? 0 : 1;
}
