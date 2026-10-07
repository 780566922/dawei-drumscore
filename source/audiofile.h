//==============================================================================
// audiofile.h - 音频文件解码与播放核心
//
// 设计要点：纯 C++，不依赖 VST 头文件，可独立用 Node DOM 桩做单元测试。
// 解码走 macOS 内置 AVFoundation（AVAudioFile），零第三方依赖，
// 天然支持 MP3 / WAV / M4A / AAC / ALAC / AIFF / CAF。
//==============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

namespace ap {

//------------------------------------------------------------------------------
// 解码后的音频：双声道交错存放的 float 采样
//------------------------------------------------------------------------------
struct AudioData
{
    std::vector<float> samples;   ///< 交错: [L0 R0 L1 R1 ...]
    uint32_t sampleRate = 44100;
    uint32_t numChannels = 2;

    size_t numFrames() const
    {
        return numChannels ? samples.size() / numChannels : 0;
    }
    double durationSec() const
    {
        return sampleRate ? static_cast<double>(numFrames()) / sampleRate : 0.0;
    }
};

//------------------------------------------------------------------------------
// 跨线程共享的参数（GUI 线程写，音频线程读）
// 用 relaxed 原子：这几个参数不需要与其他内存严格同步，
// 稍有延迟无妨，但不能出现数据竞争（UB）。
//------------------------------------------------------------------------------
struct SharedParams
{
    std::atomic<float> volume {1.0f};          ///< 0..1

    /// 起始偏移（秒，**带符号**）。含义是「谱面第 1 小节对应音频的哪一秒」：
    ///   +O → 跳过音频开头 O 秒（前奏不播），谱面第 1 小节 = 音频第 O 秒
    ///    0 → 谱面第 1 小节 = 音频第 0 秒
    ///   -O → 谱面先走 O 秒（这段插件输出静音），之后音频才从第 0 秒开始
    std::atomic<float> startOffsetSec {0.0f};
    std::atomic<bool>  playing {true};
    std::atomic<bool>  looping {false};

    void notify() const { /* 预留：将来可在此唤醒宿主 */ }
};

//------------------------------------------------------------------------------
// 播放器：把 AudioData 按宿主采样率播放出来
//
// 关键设计——重采样：
//   宿主（MuseScore）用 44100，但用户的音频可能是 44100/48000/96000。
//   这里用线性插值重采样，避免变调/变速导致"音频和谱面速度不匹配"。
//   这正是用户要"像 GP8 一样"的核心体验：音频原速播放。
//------------------------------------------------------------------------------
class Player
{
public:
    Player() = default;

    //---- 参数 ------------------------------------------------------------
    void setVolume(float v) { m_params.volume.store(clamp01(v), std::memory_order_relaxed); }
    float volume() const { return m_params.volume.load(std::memory_order_relaxed); }

    // 带符号：本来就该允许负数（谱面先走、音频后进）。
    // 只做一个很大的安全钳制，防止用户手输天文数字把读位置算到溢出。
    void setStartOffsetSec(float sec)
    {
        const float kLimit = 3600.0f;
        if (sec > kLimit)  sec = kLimit;
        if (sec < -kLimit) sec = -kLimit;
        m_params.startOffsetSec.store(sec, std::memory_order_relaxed);
    }
    float startOffsetSec() const { return m_params.startOffsetSec.load(std::memory_order_relaxed); }

    // 负偏移还欠多少秒的输出静音（0 表示当前不欠）。给测试用。
    double pendingSilenceSec() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_silenceSecLeft;
    }

    void setPlaying(bool b) { m_params.playing.store(b, std::memory_order_relaxed); }
    bool playing() const { return m_params.playing.load(std::memory_order_relaxed); }

    void setLooping(bool b) { m_params.looping.store(b, std::memory_order_relaxed); }
    bool looping() const { return m_params.looping.load(std::memory_order_relaxed); }

    SharedParams& params() { return m_params; }

    //---- 载入 ------------------------------------------------------------
    // 【线程安全】之前这里假设「只在音频线程未运行时调用」，是错的 ——
    // 在 MuseScore 里插件始终处于激活状态，音频线程一直在 render() 读
    // m_data.samples。GUI 线程载入文件时直接换掉 m_data 会释放旧缓冲，
    // 音频线程随即读到已释放内存 → 整个宿主闪退。
    // 现在所有会碰到 m_data 的路径都持同一把锁。
    bool setAudio(AudioData&& data)
    {
        if (data.samples.empty() || data.sampleRate == 0 || data.numChannels == 0)
            return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_data = std::move(data);
        m_srcSampleRate = m_data.sampleRate;
        m_offsetDirty = true;             // 强制按当前偏移重算起点
        restartFromOffsetLocked();
        return true;
    }

    // 卸载音频。注意不能靠 setAudio(AudioData()) —— 空数据会被上面的
    // 校验拒掉，结果文件根本没卸掉。
    void clearAudio()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data = AudioData();
        m_srcSampleRate = 0;
        m_readPos = 0.0;
        m_silenceSecLeft = 0.0;
        m_offsetDirty = true;
        m_appliedOffset = 0.0f;
    }

    bool hasAudio() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_data.samples.empty();
    }

    // 音频总时长（秒）
    double durationSec() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_data.durationSec();
    }

    // 在锁内访问音频数据。**绝不把引用交出去** —— 否则调用方拿着引用
    // 读的时候，音频线程可能已经把底层缓冲换掉并释放了。
    template <typename F>
    void withAudio(F&& fn) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fn(m_data);
    }

    //---- 当前播放位置（秒，源时间轴）--------------------------------------
    double positionSec() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_srcSampleRate) return 0.0;
        return static_cast<double>(m_readPos) / m_srcSampleRate;
    }

    //---- 播放 ------------------------------------------------------------
    // 输出格式：与宿主一致（hostSampleRate, 2 声道 float）
    // 返回本次实际填充的帧数。
    // 实时线程永不阻塞：锁被 GUI 占用时输出静音并立刻返回。
    uint32_t render(float** outL, float** outR, uint32_t numFrames, double hostSampleRate);

    void seekTo(double sec)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_srcSampleRate) return;
        // 手动定位就代表「从现在开始听」，负偏移欠的那段静音作废
        m_silenceSecLeft = 0.0;
        double p = sec * m_srcSampleRate;
        const double total = static_cast<double>(m_data.numFrames());
        if (p < 0.0) p = 0.0;
        if (p > total - 1.0) p = std::max(0.0, total - 1.0);
        m_readPos = p;

        // 关键：把「当前偏移」记为已生效，并清掉 dirty 标记。
        // 否则 seek 之后紧接着的 render 里 restartFromOffsetLocked() 会因为
        // offset 未同步而把读位置重置回偏移起点 —— 表现为「偶尔从头播放」。
        m_appliedOffset = m_params.startOffsetSec.load(std::memory_order_relaxed);
        m_offsetDirty = false;
    }

private:
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    //---- 偏移生效逻辑 -----------------------------------------------------
    // startOffsetSec 变化时，音频线程需要重算读位置。
    // 用「已应用的偏移」比对，避免每块都重置（否则播放位置会被反复拉回）。
    // 调用方必须已持有 m_mutex。
    void restartFromOffsetLocked()
    {
        const float target = m_params.startOffsetSec.load(std::memory_order_relaxed);
        if (m_offsetDirty || target != m_appliedOffset)
        {
            m_offsetDirty = false;
            m_appliedOffset = target;

            if (target >= 0.0f)
            {
                // 正偏移：直接从音频的第 target 秒开始读
                m_readPos = static_cast<double>(target) * m_srcSampleRate;
                m_silenceSecLeft = 0.0;
            }
            else
            {
                // 负偏移：音频从第 0 秒开始，但先垫上 |target| 秒静音。
                // 静音长度用「秒」而不是「帧」保存 —— 这里拿不到宿主采样率
                // （render 时才传进来），而且宿主切音频设备会改采样率。
                m_readPos = 0.0;
                m_silenceSecLeft = -static_cast<double>(target);
            }
        }
    }

    mutable std::mutex m_mutex;  ///< 保护 m_data / m_srcSampleRate / m_readPos / m_appliedOffset
    AudioData m_data;
    SharedParams m_params;

    uint32_t m_srcSampleRate = 0;
    double   m_readPos = 0.0;          ///< 下一个要读的源帧位置
    double   m_silenceSecLeft = 0.0;   ///< 负偏移欠的输出静音时长（秒）
    float    m_appliedOffset = 0.0f;   ///< 已生效的偏移，用于检测变化
    bool     m_offsetDirty = true;     ///< 强制重新套用偏移（换文件/卸载后）
    bool     m_wasPlaying = false;     ///< 上一块的播放状态，用于识别「恢复播放」
};

//------------------------------------------------------------------------------
// 音频文件解码（AVFoundation）
// 单独放在 .cpp 里，避免把 ObjC 头文件污染纯 C++ 头。
//------------------------------------------------------------------------------
bool decodeAudioFile(const std::string& path, AudioData& out, std::string& err);

//------------------------------------------------------------------------------
// 支持的扩展名（GUI 用）
//------------------------------------------------------------------------------
bool isSupportedAudioExtension(const std::string& path);

} // namespace ap
