//==============================================================================
// test_player.cpp — 播放核心回归测试（编译真实源文件后运行）
//
// 为什么是 C++ 而不是 JS：Node 的 require() 不支持加载 Mach-O dylib，
// 而我们要测的是**真实编译产物**，不是重写一遍逻辑。
//==============================================================================
#include "audiofile.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace ap;

static int g_pass = 0;
static int g_fail = 0;

static void check(const char* name, bool cond, const std::string& detail = "")
{
    if (cond)
    {
        std::printf("  [PASS] %s\n", name);
        ++g_pass;
    }
    else
    {
        std::printf("  [FAIL] %s%s%s\n", name, detail.empty() ? "" : " -> ", detail.c_str());
        ++g_fail;
    }
}

static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }

static std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.5f", v); return b; }

//---- 造测试音频 -----------------------------------------------------------
static AudioData makeConst(uint32_t sr, uint32_t ch, double sec, float l, float r)
{
    AudioData d;
    d.sampleRate = sr; d.numChannels = ch;
    const size_t n = static_cast<size_t>(sr * sec);
    for (size_t i = 0; i < n; ++i)
    {
        d.samples.push_back(l);
        if (ch == 2) d.samples.push_back(r);
    }
    return d;
}

static AudioData makeRamp(uint32_t sr, uint32_t ch, double sec)
{
    AudioData d;
    d.sampleRate = sr; d.numChannels = ch;
    const size_t n = static_cast<size_t>(sr * sec);
    for (size_t i = 0; i < n; ++i)
    {
        const float v = static_cast<float>(i) / static_cast<float>(n);
        d.samples.push_back(v);
        if (ch == 2) d.samples.push_back(v);
    }
    return d;
}

static std::vector<float> renderBlock(Player& p, uint32_t n, double hostSr)
{
    std::vector<float> L(n, 0.f), R(n, 0.f);
    float* pL = L.data();
    float* pR = R.data();
    p.render(&pL, &pR, n, hostSr);
    std::vector<float> out;
    out.reserve(n * 2);
    for (uint32_t i = 0; i < n; ++i) { out.push_back(L[i]); out.push_back(R[i]); }
    return out;
}

//==============================================================================
int main()
{
    std::printf("\n================播放核心回归测试 ================\n");

    //---- 1. 基础渲染 / 音量 ----
    std::printf("\n[1] 基础渲染与音量\n");
    {
        Player p;
        p.setAudio(makeConst(44100, 2, 1.0, 0.5f, 0.25f));
        check("音频已载入", p.hasAudio());
        auto o = renderBlock(p, 256, 44100);
        check("左声道 = 0.5", near(o[0], 0.5), fmt(o[0]));
        check("右声道 = 0.25", near(o[1], 0.25), fmt(o[1]));
        p.setVolume(0.5f);
        auto o2 = renderBlock(p, 64, 44100);
        check("音量 0.5 -> L=0.25", near(o2[0], 0.25), fmt(o2[0]));
        p.setVolume(0.0f);
        auto o3 = renderBlock(p, 64, 44100);
        check("音量 0 -> 静音", o3[0] == 0.0f);
    }

    //---- 2. 采样率转换 ----
    std::printf("\n[2] 采样率转换 44.1k 源 -> 48k 宿主\n");
    {
        Player p;
        p.setAudio(makeRamp(44100, 2, 1.0));
        const uint32_t n = 48000;
        auto o = renderBlock(p, n, 48000);
        check("1 秒音频在 48k 下正好读 1 秒", near(p.positionSec(), 1.0, 0.01), fmt(p.positionSec()));
        check("首样本接近 0", std::fabs(o[0]) < 0.01, fmt(o[0]));
        check("末样本接近 1", near(o[(n - 1) * 2], 1.0, 0.02), fmt(o[(n - 1) * 2]));
        check("末样本右声道接近 1", near(o[(n - 1) * 2 + 1], 1.0, 0.02), fmt(o[(n - 1) * 2 + 1]));
        bool mono = true;
        for (uint32_t i = 1; i < n; i += 97)
            if (o[i * 2] < o[(i - 1) * 2] - 1e-5f) { mono = false; break; }
        check("重采样无跳变", mono);
    }

    //---- 3. 起始偏移（用户核心需求，带符号）----
    std::printf("\n[3] 起始偏移（带符号）\n");
    {
        // 用斜坡音频：样本值直接反映「读到了音频的第几秒」。
        // 常量音频看不出位置差异（跳没跳过前奏都是同一个值）。
        Player p;
        p.setAudio(makeRamp(44100, 2, 10.0));

        // 正偏移 = 跳过音频开头
        p.setStartOffsetSec(5.0f);
        auto o = renderBlock(p, 64, 44100);
        check("正偏移 5 秒：读到斜坡 5 秒处", near(o[0], 0.5, 0.01), fmt(o[0]));
        check("正偏移：位置约 5 秒", near(p.positionSec(), 5.0, 0.01), fmt(p.positionSec()));

        // 负偏移 = 谱面先走（先出静音），随后音频从第 0 秒进入
        p.setStartOffsetSec(-1.0f);
        auto s1 = renderBlock(p, 22050, 44100);          // 0.5 秒
        bool silent1 = true;
        for (float v : s1) if (v != 0.0f) { silent1 = false; break; }
        check("负偏移 -1 秒：前半秒全静音", silent1);
        check("负偏移：还欠 0.5 秒静音",
              near(p.pendingSilenceSec(), 0.5, 0.001), fmt(p.pendingSilenceSec()));

        auto s2 = renderBlock(p, 22050, 44100);          // 再 0.5 秒
        bool silent2 = true;
        for (float v : s2) if (v != 0.0f) { silent2 = false; break; }
        check("负偏移：第 1 秒整段静音", silent2);
        check("负偏移：静音已还清",
              near(p.pendingSilenceSec(), 0.0, 1e-9), fmt(p.pendingSilenceSec()));

        auto s3 = renderBlock(p, 44100, 44100);          // 1 秒
        // 斜坡是 10 秒从 0 涨到 1，所以「读到第 0 秒」= 0.00、「读到第 1 秒」= 0.10。
        // 首尾一起看才说明问题：起点是 0 且一路在涨，证明静音之后音频从 0 秒进入，
        // 而不是把开头 1 秒也吞掉了（那样末值会是 0.20）。
        check("负偏移：静音后从音频 0 秒开始出声", near(s3[0], 0.0, 0.001), fmt(s3[0]));
        check("负偏移：音频没有被跳过（1 秒后读到 0.10）",
              near(s3[(44100 - 1) * 2], 0.1, 0.005), fmt(s3[(44100 - 1) * 2]));

        // 正负来回切换都要立刻生效
        p.setStartOffsetSec(2.0f);
        auto s4 = renderBlock(p, 64, 44100);
        check("切回正偏移 2 秒立刻生效", near(s4[0], 0.2, 0.01), fmt(s4[0]));

        // 手动定位应当清掉「欠的静音」
        p.setStartOffsetSec(-2.0f);
        renderBlock(p, 128, 44100);
        p.seekTo(3.0);
        check("拖动定位后不再有残留静音",
              near(p.pendingSilenceSec(), 0.0, 1e-9), fmt(p.pendingSilenceSec()));
        auto s5 = renderBlock(p, 64, 44100);
        check("拖动定位后又立刻有声音", near(s5[0], 0.3, 0.01), fmt(s5[0]));

        // 超出音频长度：静音，不崩溃
        p.setStartOffsetSec(999.0f);
        auto o3 = renderBlock(p, 16, 44100);
        check("超界正偏移不崩溃", p.hasAudio() && o3.size() == 32);
        p.setStartOffsetSec(-999.0f);
        auto o4 = renderBlock(p, 16, 44100);
        check("超界负偏移不崩溃（继续静音）", p.hasAudio() && o4.size() == 32);
    }

    //---- 4. 单声道 -> 双声道 ----
    std::printf("\n[4] 单声道转双声道\n");
    {
        Player p;
        p.setAudio(makeConst(44100, 1, 1.0, 0.7f, 0.0f));
        auto o = renderBlock(p, 64, 44100);
        check("L = 0.7", near(o[0], 0.7), fmt(o[0]));
        check("R = 0.7（复制）", near(o[2], 0.7), fmt(o[2]));
    }

    //---- 5. 播放结束 ----
    std::printf("\n[5] 播放结束处理\n");
    {
        Player p;
        p.setAudio(makeConst(44100, 2, 0.05, 0.5f, 0.5f));
        renderBlock(p, 2205, 44100);
        auto o = renderBlock(p, 512, 44100);
        bool allZero = true;
        for (float v : o) if (v != 0.0f) { allZero = false; break; }
        check("结束后输出静音", allZero);
        check("结束后标记为停止", !p.playing());

        // 播到结尾自动停止后，用户再按「播放」应当从头重播；
        // 否则读位置停在末尾，按钮点了像坏了一样。
        p.setPlaying(true);
        auto r = renderBlock(p, 64, 44100);
        check("播完后再次按播放 → 从头重播", near(r[0], 0.5), fmt(r[0]));
    }

    //---- 6. 循环 ----
    std::printf("\n[6] 循环模式\n");
    {
        Player p;
        p.setAudio(makeRamp(44100, 2, 0.1));
        p.setStartOffsetSec(0.05f);
        p.setLooping(true);
        auto o = renderBlock(p, 4410, 44100);
        bool any = false;
        for (float v : o) if (v > 0.f) { any = true; break; }
        check("循环后仍有输出", any);
        check("循环回到偏移点而非 0", o[0] > 0.2f, fmt(o[0]));
    }

    //---- 7. 暂停/恢复 ----
    std::printf("\n[7] 暂停与恢复\n");
    {
        Player p;
        p.setAudio(makeConst(44100, 2, 5.0, 0.5f, 0.5f));
        p.setPlaying(false);
        auto o1 = renderBlock(p, 256, 44100);
        bool z = true; for (float v : o1) if (v != 0.f) { z = false; break; }
        check("暂停时静音", z);
        p.setPlaying(true);
        auto o2 = renderBlock(p, 256, 44100);
        check("恢复后有输出", near(o2[0], 0.5), fmt(o2[0]));
    }

    //---- 8. 空音频防护 ----
    std::printf("\n[8] 空状态防护\n");
    {
        Player p;
        auto o = renderBlock(p, 128, 44100);
        bool z = true; for (float v : o) if (v != 0.f) { z = false; break; }
        check("未载入不崩溃且静音", z);
        check("hasAudio 为假", !p.hasAudio());
        check("空指针参数安全", p.render(nullptr, nullptr, 64, 44100) == 0);
    }

    //---- 9. 扩展名 ----
    std::printf("\n[9] 支持的扩展名\n");
    {
        check(".mp3", isSupportedAudioExtension("a.mp3"));
        check(".MP3 大写", isSupportedAudioExtension("a.MP3"));
        check(".m4a", isSupportedAudioExtension("a.m4a"));
        check(".wav", isSupportedAudioExtension("a.wav"));
        check(".flac", isSupportedAudioExtension("a.flac"));
        check(".txt 不支持", !isSupportedAudioExtension("a.txt"));
        check("a.mp3.txt 不支持", !isSupportedAudioExtension("a.mp3.txt"));
    }

    std::printf("\n=================================================\n");
    std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("=================================================\n\n");
    return g_fail > 0 ? 1 : 0;
}
