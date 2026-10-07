//==============================================================================
// player.cpp - 渲染逻辑（纯 C++，无 ObjC / 无 VST 依赖）
//
// 这部分单独成文件是为了能用 Node DOM 桩直接跑真实代码做回归测试，
// 而不是"写完就交付未验证的代码"。
//==============================================================================
#include "audiofile.h"

#include <algorithm>
#include <cmath>

namespace ap {

namespace {

// 输出静音。未载入音频、暂停、以及「GUI 正在换文件」三种情况共用。
uint32_t fillSilence(float** outL, float** outR, uint32_t numFrames)
{
    for (uint32_t i = 0; i < numFrames; ++i)
    {
        if (*outL) (*outL)[i] = 0.0f;
        if (*outR) (*outR)[i] = 0.0f;
    }
    return 0;
}

} // namespace

//------------------------------------------------------------------------------
// 渲染一个音频块
//
// 采样率转换：
//   源和宿主采样率不一致时用线性插值。这样音频保持原速（不会变调变快），
//   只是重采样——用户要的就是"按原曲速度播放"，和谱面对齐。
//
// 边界处理：
//   - 播放到结尾：非循环模式输出静音，并把 playing 置 false
//   - 循环模式：回到 startOffset 重新播（不是回到 0 秒，这是关键——
//     用户设了 30 秒偏移后，循环必须从 30 秒开始循环，否则体验错乱）
//   - 未载入音频：输出静音
//------------------------------------------------------------------------------
uint32_t Player::render(float** outL, float** outR, uint32_t numFrames, double hostSampleRate)
{
    if (!outL || !outR || numFrames == 0)
        return 0;

    // 【实时安全】绝不阻塞：GUI 线程正在换文件时拿不到锁，本块直接输出
    // 静音并立刻返回，下一块再继续。宁可一小段听不出来的静音，
    // 也不能把音频线程卡住（那会导致宿主爆音甚至卡死）。
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return fillSilence(outL, outR, numFrames);

    // 无音频：输出静音
    if (m_data.samples.empty() || m_srcSampleRate == 0 || hostSampleRate <= 0.0)
        return fillSilence(outL, outR, numFrames);

    // 偏移发生变化 → 重算读位置
    restartFromOffsetLocked();

    const bool playing = m_params.playing.load(std::memory_order_relaxed);

    // 播到结尾后会自动置为停止，读位置停在末尾。此时再按「播放」如果什么都不做，
    // 用户会觉得按钮坏了 —— 这里让「停止→播放」这个沿自动回到偏移起点重放。
    if (playing && !m_wasPlaying && m_readPos >= static_cast<double>(m_data.numFrames()))
    {
        m_offsetDirty = true;
        restartFromOffsetLocked();
    }
    m_wasPlaying = playing;

    if (!playing)
        return fillSilence(outL, outR, numFrames);

    const float vol = clamp01(m_params.volume.load(std::memory_order_relaxed));
    const bool loop = m_params.looping.load(std::memory_order_relaxed);
    // 每输出 1 帧，消耗多少源帧 = srcSr / hostSr
    // （44.1k 源在 48k 宿主下应放慢一点，实际消耗 0.91875 源帧/输出帧）
    const double step = static_cast<double>(m_srcSampleRate) / hostSampleRate;

    const size_t totalFrames = m_data.numFrames();
    const double loopStartFrame = static_cast<double>(m_appliedOffset) * m_srcSampleRate;
    const uint32_t ch = m_data.numChannels;

    uint32_t i = 0;

    // 负偏移：谱面先走，音频整体后移 —— 这里把欠的静音先垫掉。
    // 按「秒」记录而不是帧，所以宿主中途换采样率也不会算错。
    if (m_silenceSecLeft > 0.0)
    {
        const double needFrames = m_silenceSecLeft * hostSampleRate;
        uint32_t n = static_cast<uint32_t>(std::ceil(needFrames));
        if (n > numFrames) n = numFrames;

        for (uint32_t k = 0; k < n; ++k)
        {
            if (*outL) (*outL)[k] = 0.0f;
            if (*outR) (*outR)[k] = 0.0f;
        }
        m_silenceSecLeft -= static_cast<double>(n) / hostSampleRate;
        if (m_silenceSecLeft < 0.0) m_silenceSecLeft = 0.0;

        i = n;
    }

    while (i < numFrames)
    {
        // 读位置越界。用 totalFrames（而非 -1）作为界，
        // 让最后一帧也能被插值取到；i1 已有钳制不会越界读。
        if (m_readPos >= static_cast<double>(totalFrames))
        {
            if (loop)
            {
                // 循环回到用户设定起点；若起点非法则回到 0
                m_readPos = (loopStartFrame > 0.0 && loopStartFrame < static_cast<double>(totalFrames))
                                ? loopStartFrame
                                : 0.0;
                m_readPos = std::max(0.0, std::min(m_readPos, static_cast<double>(totalFrames)));
                continue;
            }
            break; // 结束本次渲染，余下填静音
        }

        // 线性插值
        const double p0 = m_readPos;
        const size_t i0 = static_cast<size_t>(p0);
        const double frac = p0 - static_cast<double>(i0);
        const size_t i1 = (i0 + 1 < totalFrames) ? i0 + 1 : i0;

        float l = 0.0f, r = 0.0f;
        if (ch == 2)
        {
            const float s0 = m_data.samples[i0 * 2 + 0];
            const float s1 = m_data.samples[i1 * 2 + 0];
            l = static_cast<float>(s0 + (s1 - s0) * frac);
            r = static_cast<float>(m_data.samples[i0 * 2 + 1] +
                                    (m_data.samples[i1 * 2 + 1] - m_data.samples[i0 * 2 + 1]) * frac);
        }
        else // 单声道 → 双声道
        {
            const float s0 = m_data.samples[i0];
            const float s1 = m_data.samples[(i1 < totalFrames) ? i1 : i0];
            l = r = static_cast<float>(s0 + (s1 - s0) * frac);
        }

        if (*outL) (*outL)[i] = l * vol;
        if (*outR) (*outR)[i] = r * vol;

        m_readPos += step;
        ++i;
    }

    // 剩余部分填静音（播放结束的情况）
    for (; i < numFrames; ++i)
    {
        if (*outL) (*outL)[i] = 0.0f;
        if (*outR) (*outR)[i] = 0.0f;
    }

    // 走到末尾且不循环 → 标记为停止
    if (!loop && m_readPos >= static_cast<double>(totalFrames))
        m_params.playing.store(false, std::memory_order_relaxed);

    return numFrames;
}

} // namespace ap
