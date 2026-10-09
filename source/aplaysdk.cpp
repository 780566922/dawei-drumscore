//==============================================================================
// aplaysdk.cpp — 基于官方 public.sdk 的插件实现
//==============================================================================
#include "aplaysdk.h"

#include "crashguard.h"

#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"   // ProcessContext（速度/拍号/谱面位置）
#include "pluginterfaces/vst/ivstmessage.h"      // IConnectionPoint
#include "pluginterfaces/base/ipluginbase.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"
#include "pluginterfaces/base/ibstream.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// SDK 没有定义这些，按官方示例的惯例自己给。
// classFlags：标记插件可分发（宿主据此决定是否加载）
static constexpr uint32 kAPlayDistributable = 1 << 0;

// 插件版本字符串（FULL_VERSION_STR 是 CMake 生成的，这里手写）
#define APLAY_VERSION_WSTR STR16 ("1.2.2")

namespace aplay {

namespace {

//==============================================================================
// 活跃处理器注册表
//
// 为什么需要它：VST3 宿主可以在任意时机创建/销毁处理器实例，而且经常
// 先建一个临时实例用于扫描、销毁之后再建真正工作的实例。所以「记住最新
// 那个指针」这种写法必须配合同步注销 —— 否则留下的是野指针，
// 表现就是编辑器里一操作整个宿主闪退。
//==============================================================================
std::mutex g_regMutex;
std::vector<APlayProcessor*> g_liveProcessors;

void registerProcessor (APlayProcessor* p)
{
    std::lock_guard<std::mutex> lk (g_regMutex);
    g_liveProcessors.push_back (p);
}

void unregisterProcessor (APlayProcessor* p)
{
    std::lock_guard<std::mutex> lk (g_regMutex);
    auto it = std::find (g_liveProcessors.begin (), g_liveProcessors.end (), p);
    if (it != g_liveProcessors.end ())
        g_liveProcessors.erase (it);
}

APlayProcessor* currentProcessor ()
{
    std::lock_guard<std::mutex> lk (g_regMutex);
    return g_liveProcessors.empty () ? nullptr : g_liveProcessors.back ();
}

//==============================================================================
// 永生代理：无状态的纯转发器
//
// 它是编辑器视图唯一持有的后端对象，地址在进程生命周期内恒定。
// 因为没有成员状态，所以不存在跨线程数据竞争；所有真实状态都在
// 处理器里，转发前先取一次当前处理器的局部指针。
//==============================================================================
class StableBackend final : public ap::PlugView::Backend
{
public:
    bool loadFile (const std::string& path) override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->loadFile (path) : false;
    }

    void unloadFile () override
    {
        if (APlayProcessor* p = currentProcessor ())
            p->unloadFile ();
    }

    bool hasAudio () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->hasAudio () : false;
    }

    double durationSec () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->durationSec () : 0.0;
    }

    std::string currentPath () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->currentPath () : std::string ();
    }

    std::string lastError () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->lastError ()
                 : std::string ("音频引擎尚未就绪，请关闭编辑器后重新打开");
    }

    void waveformPeaks (std::vector<float>& out, int buckets,
                        double fromSec, double toSec) const override
    {
        APlayProcessor* p = currentProcessor ();
        if (p)
        {
            p->waveformPeaks (out, buckets, fromSec, toSec);
            return;
        }
        out.assign (static_cast<size_t> (buckets > 0 ? buckets : 1), 0.f);
    }

    /// 实时健康探针转发（见 gui.h 的说明）：界面线程用它把「音频丢块」写进日志。
    uint64_t lockDropCount () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->lockDropCount () : 0;
    }

    void setOffsetSec (float sec) override
    {
        if (APlayProcessor* p = currentProcessor ()) p->setOffsetSec (sec);
    }
    float offsetSec () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->offsetSec () : 0.f;
    }

    void setVolume (float v) override
    {
        if (APlayProcessor* p = currentProcessor ()) p->setVolume (v);
    }
    float volume () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->volume () : 1.f;
    }

    void setPlaying (bool b) override
    {
        if (APlayProcessor* p = currentProcessor ()) p->setPlaying (b);
    }
    bool playing () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->playing () : false;
    }

    void setLooping (bool b) override
    {
        if (APlayProcessor* p = currentProcessor ()) p->setLooping (b);
    }
    bool looping () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->looping () : false;
    }

    void seekTo (double sec) override
    {
        if (APlayProcessor* p = currentProcessor ()) p->seekTo (sec);
    }
    double positionSec () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->positionSec () : 0.0;
    }

    void setGridBPM (float bpm) override
    {
        if (APlayProcessor* p = currentProcessor ()) p->setGridBPM (bpm);
    }
    float gridBPM () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->gridBPM () : 0.f;
    }
    void setGridBeatsPerBar (int beats) override
    {
        if (APlayProcessor* p = currentProcessor ()) p->setGridBeatsPerBar (beats);
    }
    int gridBeatsPerBar () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->gridBeatsPerBar () : 4;
    }
    void setGridBeatDenominator (int denom) override
    {
        if (APlayProcessor* p = currentProcessor ()) p->setGridBeatDenominator (denom);
    }
    int gridBeatDenominator () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->gridBeatDenominator () : 4;
    }
    ap::PlugView::Backend::HostTimeline hostTimeline () const override
    {
        APlayProcessor* p = currentProcessor ();
        return p ? p->hostTimeline () : ap::PlugView::Backend::HostTimeline {};
    }
};

} // namespace

//------------------------------------------------------------------------------
ap::PlugView::Backend* stableBackend ()
{
    // 函数内 static：C++11 保证初始化线程安全，且对象活到进程退出。
    // 视图拿着这个地址永远不会失效 —— 这就是整个方案的关键。
    static StableBackend s_backend;
    return &s_backend;
}

//------------------------------------------------------------------------------
int liveProcessorCount ()
{
    std::lock_guard<std::mutex> lk (g_regMutex);
    return static_cast<int> (g_liveProcessors.size ());
}

//==============================================================================
// APlayProcessor
//==============================================================================
APlayProcessor::APlayProcessor ()
{
    // 关键：必须在构造时关联控制器 UID，否则宿主找不到对应的 EditController
    setControllerClass (FUID::fromTUID (APLayControllerUID));

    // 登记自己，供编辑器视图通过代理找到音频后端
    registerProcessor (this);
    ap::crashLog ("Processor 构造（存活数=%d）", liveProcessorCount ());
}

APlayProcessor::~APlayProcessor ()
{
    // 必须先注销再做析构：注销之后代理就再也取不到这个地址了，
    // 视图侧的定时器/按钮即便恰好在跑也只会拿到安全的空值。
    unregisterProcessor (this);
    ap::crashLog ("Processor 析构（存活数=%d）", liveProcessorCount ());
}

//------------------------------------------------------------------------------
tresult PLUGIN_API APlayProcessor::initialize (FUnknown* context)
{
    tresult result = AudioEffect::initialize (context);
    if (result == kResultTrue)
    {
        // 本插件只输出、不接收输入（用户拖入音频文件直接播放）
        addAudioOutput (STR16 ("Stereo Out"), SpeakerArr::kStereo);
    }
    return result;
}

tresult PLUGIN_API APlayProcessor::setActive (TBool state)
{
    if (state)
    {
        m_player.setPlaying (m_playing);
    }
    else
    {
        m_player.setPlaying (false);
    }
    return AudioEffect::setActive (state);
}

tresult PLUGIN_API APlayProcessor::setProcessing (TBool /*state*/)
{
    return kResultOk;   // 官方基类已处理
}

tresult PLUGIN_API APlayProcessor::setupProcessing (ProcessSetup& setup)
{
    // 注意：kRealtime 的枚举值就是 0，不能用 !setup.processMode 判断合法性
    // （那会把最常用的实时模式误判为无效）。按范围校验。
    if (setup.processMode < kRealtime || setup.processMode > kOffline)
        return kResultFalse;
    m_hostSampleRate.store (setup.sampleRate, std::memory_order_relaxed);   // 渲染要用宿主采样率做重采样
    // 官方基类要求总线全部就绪才返回 ok；本插件只有一个输出总线，
    // 若宿主尚未 activate，这里会返回 kResultFalse —— 不影响渲染。
    const tresult r = AudioEffect::setupProcessing (setup);
    if (r != kResultOk)
        m_setupPending = true;   // 记录下来，等总线就绪后重试
    return kResultOk;
}

tresult PLUGIN_API APlayProcessor::canProcessSampleSize (int32 size)
{
    return (size == kSample32) ? kResultOk : kResultFalse;
}

//------------------------------------------------------------------------------
// 状态存取
//------------------------------------------------------------------------------
tresult PLUGIN_API APlayProcessor::setState (IBStream* state)
{
    if (!state) return kResultOk;

    for (int guard = 0; guard < 64; ++guard)
    {
        int32 id = 0;
        if (state->read (&id, sizeof id) != kResultOk) break;
        int32 size = 0;
        if (state->read (&size, sizeof size) != kResultOk) break;
        if (size < 0 || size > (1 << 20)) break;

        switch (id)
        {
            case kIdVolume:
            {
                float v = 1.f;
                if (state->read (&v, sizeof v) == kResultOk)
                    m_volume = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                break;
            }
            case kIdOffset:
            {
                float v = 0.f;
                // 带符号：负偏移表示「谱面先走、音频后进」。
                // 旧代码在这里钳到 >= 0，把负数全吃成了 0。
                if (state->read (&v, sizeof v) == kResultOk)
                    m_offset = v < -3600.f ? -3600.f : (v > 3600.f ? 3600.f : v);
                break;
            }
            case kIdGridBPM:
            {
                float v = 0.f;
                if (state->read (&v, sizeof v) == kResultOk)
                    m_gridBPM = (v < 0.f) ? 0.f : (v > 400.f ? 400.f : v);
                break;
            }
            case kIdGridBeats:
            {
                int32 v = 4;
                if (state->read (&v, sizeof v) == kResultOk)
                    m_gridBeats = (v < 1) ? 4 : (v > 32 ? 32 : v);
                break;
            }
            case kIdGridBeatDenominator:
            {
                int32 v = 4;
                if (state->read (&v, sizeof v) == kResultOk)
                    m_gridBeatDenominator = (v == 2 || v == 4 || v == 8 || v == 16) ? v : 4;
                break;
            }
            case kIdPlay:
            {
                uint8 b = 1;
                if (state->read (&b, 1) == kResultOk) m_playing = b != 0;
                break;
            }
            case kIdLoop:
            {
                uint8 b = 0;
                if (state->read (&b, 1) == kResultOk) m_loop = b != 0;
                break;
            }
            case kIdFilePath:
            {
                if (size > 0 && size < 8192)
                {
                    std::string p (static_cast<size_t> (size), '\0');
                    if (state->read (&p[0], static_cast<uint32> (size)) == kResultOk)
                    {
                        m_filePath = p;
                        loadCurrentFile ();
                    }
                }
                break;
            }
            default:
            {
                std::vector<uint8> skip (static_cast<size_t> (size), 0);
                if (size > 0 && state->read (skip.data (), static_cast<uint32> (size)) != kResultOk)
                    return kResultOk;
                continue;
            }
        }
    }

    applyParams ();
    return kResultOk;
}

tresult PLUGIN_API APlayProcessor::getState (IBStream* state)
{
    if (!state) return kResultOk;

    auto writeChunk = [state] (int32 id, const void* data, int32 size)
    {
        int32 id32 = id;
        state->write (&id32, sizeof id32);
        state->write (&size, sizeof size);
        if (size > 0)
            state->write (const_cast<void*> (data), size);
    };

    writeChunk (kIdVolume, &m_volume, sizeof m_volume);
    writeChunk (kIdOffset, &m_offset, sizeof m_offset);

    uint8 pb = m_playing ? 1 : 0;
    writeChunk (kIdPlay, &pb, 1);
    uint8 lb = m_loop ? 1 : 0;
    writeChunk (kIdLoop, &lb, 1);

    // 小节的网格设置也要存 —— 用户调好的 BPM/拍号应当跟着乐谱一起保存，
    // 下次打开不用重设。
    writeChunk (kIdGridBPM, &m_gridBPM, sizeof m_gridBPM);
    int32 beats = m_gridBeats;
    writeChunk (kIdGridBeats, &beats, sizeof beats);
    int32 denom = m_gridBeatDenominator;
    writeChunk (kIdGridBeatDenominator, &denom, sizeof denom);

    const int32 len = static_cast<int32> (m_filePath.size ());
    writeChunk (kIdFilePath, m_filePath.data (), len);

    return kResultOk;
}

//------------------------------------------------------------------------------
// 采集宿主时间轴
//
// MuseScore 会在 ProcessContext 里附带速度 / 拍号 / 谱面位置。拿到了网格就能
// 自动跟随乐谱速度，谱面播放头也能画到波形上。
//
// 注意：这里跑在音频线程，只写原子量，不做任何 I/O —— 在音频线程写文件
// 可能造成爆音。日志交给界面的 20Hz 定时器去打。
//------------------------------------------------------------------------------
void APlayProcessor::captureHostTimeline (ProcessData& data)
{
    ProcessContext* pc = data.processContext;
    if (!pc)
        return;

    const uint32 st = pc->state;

    if (st & ProcessContext::kTempoValid)
        m_hostBPM.store (static_cast<float> (pc->tempo), std::memory_order_relaxed);

    if (st & ProcessContext::kTimeSigValid)
    {
        m_hostBeatsPerBar.store (pc->timeSigNumerator, std::memory_order_relaxed);
        m_hostBeatDenominator.store (pc->timeSigDenominator, std::memory_order_relaxed);
    }

    // 谱面播放头：优先用 projectTimeSamples（规范里标为 always valid，
    // 宿主一定会填），换算成秒。之前用 projectTimeMusic（四分音符）需要
    // 依赖 tempo 才能换算成秒，而 MuseScore 不填 tempo —— 结果谱面位置
    // 一直是 NaN，网格根本拿不到「谱面现在播到哪」。
    //
    // 优先级：
    //   1) projectTimeSamples —— 总是有效，采样为单位的工程位置
    //   2) projectTimeMusic（四分音符）—— 有 tempo 时更精确（能处理变速）
    //   3) continousTimeSamples —— 老宿主才用
    double pos = std::numeric_limits<double>::quiet_NaN ();
    const double sr = pc->sampleRate > 0.0
                          ? pc->sampleRate
                          : m_hostSampleRate.load (std::memory_order_relaxed);

    if (sr > 0.0)
    {
        // projectTimeSamples / sampleRate 在规范里都标为 always valid，
        // 直接读、无需 flag 判断。这是「谱面播到哪」最可靠的数据源。
        pos = static_cast<double> (pc->projectTimeSamples) / sr;

        // 有 tempo 且给了四分音符位置时，用它替换更精确（能反映变速）。
        // MuseScore 不填 tempo，这条路径基本不会走，但保留着无害。
        if (st & ProcessContext::kProjectTimeMusicValid)
        {
            const double bpm = (st & ProcessContext::kTempoValid) && pc->tempo > 1.0
                                   ? pc->tempo
                                   : 0.0;
            if (bpm > 1.0)
                pos = pc->projectTimeMusic * 60.0 / bpm;
        }
        // 老宿主连 projectTimeSamples 都不给时，退回 continousTimeSamples
        if (!(pos == pos) && (st & ProcessContext::kContTimeValid))
            pos = static_cast<double> (pc->continousTimeSamples) / sr;
    }

    // ---- 跳转跟随：宿主拖播放头 → 音频 seek 到对应位置 ----
    // 只在拿到有效位置时检测。正常播放时 projectTimeSamples 每块前进约
    // 「一块的采样数 / sampleRate」秒（几毫秒级），而手动拖拽播放头会造成
    // 大跳变或回跳。这里在音频线程检测跳变，只写原子量，不直接 seek ——
    // 真正的 seek 交给 process() 在渲染前执行（见下方 process 实现）。
    const bool hostPlayingNow = (st & ProcessContext::kPlaying) != 0;

    // ---- 「暂停 → 播放」边沿：恢复播放的瞬间，自动把音频 seek 回谱面位置 ----
    // 这是对齐的兜底：即使暂停期间音频位置因为某种原因漂了，恢复播放时也
    // 一次性拉回，杜绝「按得越多分家越远」。
    if (hostPlayingNow && !m_hostWasPlaying && (pos == pos))
    {
        const double audioPos = pos + static_cast<double> (m_player.startOffsetSec ());
        m_pendingSeekSec.store (audioPos, std::memory_order_relaxed);
    }
    m_hostWasPlaying = hostPlayingNow;

    if (pos == pos)   // 有效位置（非 NaN）
    {
        // 关键：只有「宿主正在播放」时才检测跳变。
        // 快速 play/pause 切换时，projectTimeSamples 会冻结（暂停）再跳变（恢复），
        // 这种跳变不是「用户拖播放头」，却会被下面 0.25s 阈值误判成 seek →
        // 音频被 seek 到错误位置 → 两个播放头分家。
        if (hostPlayingNow)
        {
            if (m_hostSeekSeen)
            {
                const double delta = pos - m_hostLastPlayhead;
                // 正常播放每块前进约几毫秒~几十毫秒。阈值取 0.25s：既不会把
                // 正常前进误判成 seek（除非宿主每块跳 0.25s，那已经是异常），
                // 也能抓住手动拖拽（拖拽通常跳至少 0.5s 甚至整小节）。
                const double kSeekThreshold = 0.25;
                if (delta < -kSeekThreshold || delta > kSeekThreshold)
                {
                    // 目标音频位置 = 谱面位置 + 偏移。
                    // 偏移读 player 里的原子量（startOffsetSec），避免与
                    // setState（宿主线程）写 m_offset 产生数据竞争。
                    const double audioPos = pos + static_cast<double> (m_player.startOffsetSec ());
                    m_pendingSeekSec.store (audioPos, std::memory_order_relaxed);
                }
            }
            m_hostSeekSeen = true;
            m_hostLastPlayhead = pos;
        }
        else
        {
            // 宿主暂停/停止：重置基准。下次恢复播放时，第一块不跟上一块比，
            // 避免把「暂停前后」的位置差（可能是停止归位）误判成跳变。
            m_hostSeekSeen = false;
        }
    }

    m_hostPlayheadSec.store (pos, std::memory_order_relaxed);
    m_hostPlaying.store (hostPlayingNow, std::memory_order_relaxed);
    m_hostRawState.store (st, std::memory_order_relaxed);

    // ---- 关键：插件音频播放状态必须跟随宿主的 kPlaying ----
    // 之前 player 的 playing 是独立状态（默认 true），与宿主播放/暂停完全脱钩。
    // 结果：宿主暂停时谱面播放头冻结，但音频还在继续渲染、m_readPos 继续前进
    // → 红线（音频位置）和绿线（谱面位置）越来越远 → 「按得越多分家越远」。
    // 这里每块把宿主的播放态同步给 player：宿主停，音频也停；宿主播，音频也播。
    // 用 kPlaying 位判断（st & kPlaying），比 setActive 的 m_playing 更贴近
    // 「宿主此刻到底在不在播」。
    m_player.setPlaying (hostPlayingNow);
}

//------------------------------------------------------------------------------
// 音频处理
//------------------------------------------------------------------------------
tresult PLUGIN_API APlayProcessor::process (ProcessData& data)
{
    captureHostTimeline (data);

    // ---- 消费待执行的跳转 seek ----
    // captureHostTimeline 检测到宿主播放头跳变后写入了 m_pendingSeekSec。
    // 在这里（音频线程、渲染前）安全地执行 seek。这里在 render 之前调用，
    // render 内部用 try_lock，所以此时 m_player 的锁是空闲的，seekTo 的
    // lock_guard 能立刻拿到，不会阻塞音频线程（除非 GUI 恰好正在换文件，
    // 那种情况 lock_guard 会短暂等待，但换文件极罕见且窗口极短，可接受）。
    {
        const double want = m_pendingSeekSec.load (std::memory_order_relaxed);
        if (want == want)   // 非 NaN：有跳转待执行
        {
            m_pendingSeekSec.store (std::numeric_limits<double>::quiet_NaN (),
                                    std::memory_order_relaxed);
            m_player.seekTo (want);
        }
    }

    // 官方基类已处理总线/侧链等，这里只关心自己的输出
    AudioBusBuffers* out = data.outputs;
    if (!out || data.numOutputs <= 0 || out[0].numChannels == 0)
        return kResultOk;

    const int32 nCh = out[0].numChannels;
    const int32 nFrames = data.numSamples;
    if (nFrames <= 0)
        return kResultOk;

    AudioBusBuffers& bus = out[0];

    // 取一次局部副本：本块渲染期间保持采样率一致
    const double hostSr = m_hostSampleRate.load (std::memory_order_relaxed);

    if (bus.silenceFlags != 0)
    {
        // 静音块：确保为零
        if (bus.channelBuffers32)
        {
            for (int32 ch = 0; ch < nCh; ++ch)
                if (bus.channelBuffers32[ch])
                    std::memset (bus.channelBuffers32[ch], 0, sizeof (float) * static_cast<size_t> (nFrames));
        }
        else if (bus.channelBuffers64)
        {
            for (int32 ch = 0; ch < nCh; ++ch)
                if (bus.channelBuffers64[ch])
                    std::memset (bus.channelBuffers64[ch], 0, sizeof (double) * static_cast<size_t> (nFrames));
        }
        return kResultOk;
    }

    if (bus.channelBuffers32)
    {
        if (nCh >= 2)
            m_player.render (&bus.channelBuffers32[0], &bus.channelBuffers32[1],
                             static_cast<uint32> (nFrames), hostSr);
        else
            m_player.render (&bus.channelBuffers32[0], &bus.channelBuffers32[0],
                             static_cast<uint32> (nFrames), hostSr);
    }
    else if (bus.channelBuffers64)
    {
        std::vector<float> tmpL (static_cast<size_t> (nFrames));
        std::vector<float> tmpR (static_cast<size_t> (nFrames));
        float* pL = tmpL.data ();
        float* pR = tmpR.data ();
        m_player.render (&pL, &pR, static_cast<uint32> (nFrames), hostSr);
        for (int32 i = 0; i < nFrames; ++i)
        {
            bus.channelBuffers64[0][i] = static_cast<double> (tmpL[static_cast<size_t> (i)]);
            if (nCh >= 2)
                bus.channelBuffers64[1][i] = static_cast<double> (tmpR[static_cast<size_t> (i)]);
        }
    }

    return kResultOk;
}

//==============================================================================
// PlugView::Backend 实现
//==============================================================================
bool APlayProcessor::loadFile (const std::string& path)
{
    ap::crashLog ("载入请求: %s", path.c_str ());
    m_filePath = path;
    loadCurrentFile ();

    if (m_loadedOk)
        ap::crashLog ("载入成功: 时长 %.2f 秒", m_player.durationSec ());
    else
        ap::crashLog ("载入失败: %s", m_loadError.c_str ());

    return m_loadedOk;
}

void APlayProcessor::unloadFile ()
{
    m_filePath.clear ();
    m_player.clearAudio ();   // 不能传空 AudioData 给 setAudio —— 会被校验拒掉
    m_loadedOk = false;
}

bool APlayProcessor::hasAudio () const { return m_player.hasAudio (); }
double APlayProcessor::durationSec () const { return m_player.durationSec (); }
std::string APlayProcessor::currentPath () const { return m_filePath; }
std::string APlayProcessor::lastError () const { return m_loadError; }

void APlayProcessor::waveformPeaks (std::vector<float>& out, int buckets,
                                    double fromSec, double toSec) const
{
    out.assign (static_cast<size_t> (buckets > 0 ? buckets : 1), 0.f);

    // 必须在锁内整段完成。之前是「拿到 const 引用再遍历」——
    // 音频线程可能在这中间把缓冲换掉并释放，于是读到野内存。
    m_player.withAudio ([&] (const ap::AudioData& d)
    {
        const size_t n = d.numFrames ();
        const uint32 ch = d.numChannels;
        if (n == 0 || ch == 0 || d.sampleRate == 0) return;

        // 取样区间：调用方（波形视图）给的是「当前可见的时间段」。
        // 放大之后只统计可见部分，波峰才有细节 —— 否则永远是全曲平均，看不出鼓点。
        double f0 = fromSec;
        double f1 = toSec;
        if (!(f1 > f0))
        {
            f0 = 0.0;
            f1 = d.durationSec ();
        }
        if (f0 < 0.0) f0 = 0.0;
        if (f1 > d.durationSec ()) f1 = d.durationSec ();
        if (f1 <= f0) return;

        size_t s0 = static_cast<size_t> (f0 * d.sampleRate);
        size_t s1 = static_cast<size_t> (f1 * d.sampleRate);
        if (s0 >= n) return;
        if (s1 > n) s1 = n;
        if (s1 <= s0) return;

        const size_t b = out.size ();
        const double span = static_cast<double> (s1 - s0);
        for (size_t i = 0; i < b; ++i)
        {
            const size_t a0 = s0 + static_cast<size_t> (span * static_cast<double> (i) / static_cast<double> (b));
            size_t a1 = s0 + static_cast<size_t> (span * static_cast<double> (i + 1) / static_cast<double> (b));
            if (a1 <= a0) a1 = a0 + 1;
            if (a1 > s1) a1 = s1;

            // 抽样步长：全览一首 4 分钟的歌，每个桶有上万个采样，
            // 逐个遍历要上千万次 —— 滚动时界面会卡。这里限制每桶最多看
            // 约 128 个点。放大到看得清单个鼓点的时候步长自动回到 1，不受影响。
            size_t stride = (a1 - a0) / 128;
            if (stride < 1) stride = 1;

            float peak = 0.f;
            for (size_t j = a0; j < a1; j += stride)
            {
                float v = d.samples[j * ch];
                if (ch >= 2) v = (v + d.samples[j * ch + 1]) * 0.5f;
                const float a = std::fabs (v);
                if (a > peak) peak = a;
            }
            out[i] = peak;
        }
    });
}

void APlayProcessor::setGridBPM (float bpm) { m_gridBPM = (bpm < 0.f) ? 0.f : (bpm > 400.f ? 400.f : bpm); }
float APlayProcessor::gridBPM () const { return m_gridBPM; }
uint64_t APlayProcessor::lockDropCount () const { return m_player.lockDropCount (); }
void APlayProcessor::setGridBeatsPerBar (int beats)
{
    m_gridBeats = (beats < 1) ? 4 : (beats > 32 ? 32 : beats);
}
int APlayProcessor::gridBeatsPerBar () const { return m_gridBeats; }
void APlayProcessor::setGridBeatDenominator (int denom)
{
    // 只允许 2/4/8/16（合法的拍号分母）；非法值回落 4。
    m_gridBeatDenominator = (denom == 2 || denom == 4 || denom == 8 || denom == 16) ? denom : 4;
}
int APlayProcessor::gridBeatDenominator () const { return m_gridBeatDenominator; }

ap::PlugView::Backend::HostTimeline APlayProcessor::hostTimeline () const
{
    ap::PlugView::Backend::HostTimeline t;
    const float bpm = m_hostBPM.load (std::memory_order_relaxed);
    const int bpb = m_hostBeatsPerBar.load (std::memory_order_relaxed);

    t.tempoValid = bpm > 1.0f;
    t.bpm = bpm;
    t.timeSigValid = bpb > 0;
    t.beatsPerBar = bpb;
    t.beatDenominator = m_hostBeatDenominator.load (std::memory_order_relaxed);
    t.rawState = m_hostRawState.load (std::memory_order_relaxed);

    const double ph = m_hostPlayheadSec.load (std::memory_order_relaxed);
    t.playheadValid = (ph == ph);   // NaN 判断：宿主没给位置时就是 NaN
    t.playheadSec = t.playheadValid ? ph : 0.0;
    t.playing = m_hostPlaying.load (std::memory_order_relaxed);
    return t;
}

void APlayProcessor::setOffsetSec (float sec) { m_player.setStartOffsetSec (sec); }
float APlayProcessor::offsetSec () const { return m_player.startOffsetSec (); }
void APlayProcessor::setVolume (float v) { m_player.setVolume (v); }
float APlayProcessor::volume () const { return m_player.volume (); }
void APlayProcessor::setPlaying (bool b) { m_player.setPlaying (b); }
bool APlayProcessor::playing () const { return m_player.playing (); }
void APlayProcessor::setLooping (bool b) { m_player.setLooping (b); }
bool APlayProcessor::looping () const { return m_player.looping (); }
void APlayProcessor::seekTo (double sec) { m_player.seekTo (sec); }
double APlayProcessor::positionSec () const { return m_player.positionSec (); }

void APlayProcessor::loadCurrentFile ()
{
    if (m_filePath.empty ())
    {
        m_loadedOk = false;
        m_loadError = "未选择文件";
        return;
    }
    ap::AudioData d;
    std::string err;

    ap::crashLog ("开始解码: %s", m_filePath.c_str ());
    if (!ap::decodeAudioFile (m_filePath, d, err))
    {
        m_loadedOk = false;
        m_loadError = err;
        ap::crashLog ("解码失败: %s", err.c_str ());
        return;
    }
    ap::crashLog ("解码完成: %zu 帧, 采样率 %u", d.numFrames (), d.sampleRate);

    if (!m_player.setAudio (std::move (d)))
    {
        m_loadedOk = false;
        m_loadError = "解码成功但载入失败";
        ap::crashLog ("setAudio 被拒（数据校验未通过）");
        return;
    }
    ap::crashLog ("音频已就绪");
    m_loadedOk = true;
    m_loadError.clear ();
    applyParams ();
}

void APlayProcessor::applyParams ()
{
    m_player.setVolume (m_volume);
    m_player.setStartOffsetSec (m_offset);
    m_player.setLooping (m_loop);
    m_player.setPlaying (m_playing);
}

//==============================================================================
// APlayController
//==============================================================================
tresult PLUGIN_API APlayController::initialize (FUnknown* context)
{
    return EditControllerEx1::initialize (context);
}

//------------------------------------------------------------------------------
// createView —— 永远返回非空视图，且后端恒为永生代理
//
// 两点都不能省：
//   1) 宿主在扫描阶段就会调用 createView，那时音频处理器还没创建。
//      这里返回 nullptr 会被判定 hasNativeEditorSupport = false，
//      插件直接从混音器列表里消失。
//   2) 视图拿到的是代理而不是处理器裸指针 —— 处理器随时可能被宿主销毁，
//      代理不会。这是「编辑器里一操作就闪退」的结构性修法。
//------------------------------------------------------------------------------
IPlugView* PLUGIN_API APlayController::createView (FIDString /*name*/)
{
    auto* v = new ap::PlugView (stableBackend (), nullptr);
    v->addRef ();
    ap::crashLog ("createView：新建 PlugView（后端=%p）", static_cast<void*> (stableBackend ()));
    return static_cast<IPlugView*> (v);
}

//==============================================================================
// 工厂
//
// 用官方 BEGIN_FACTORY_DEF / DEF_CLASS 宏 —— 它们会正确填好
// classFlags、categories、vendor、版本等所有字段。
// 手写工厂时漏掉这些字段，宿主会判定插件不兼容（实测踩过）。
//==============================================================================
} // namespace aplay

//------------------------------------------------------------------------------
// UTF-8 → UTF-16 解码（自己写，见下方 getClassInfoUnicode 的说明）
//------------------------------------------------------------------------------
static void utf8ToUtf16 (Steinberg::char16* dst, int32 dstCount, const char* src)
{
    if (!dst || dstCount <= 0) return;
    int32 o = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*> (src ? src : "");
    while (*p && o < dstCount - 1)
    {
        Steinberg::uint32 cp;
        if (p[0] < 0x80u)                          { cp = p[0]; p += 1; }
        else if ((p[0] & 0xE0u) == 0xC0u && p[1])  { cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); p += 2; }
        else if ((p[0] & 0xF0u) == 0xE0u && p[1] && p[2])
            { cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); p += 3; }
        else if ((p[0] & 0xF8u) == 0xF0u && p[1] && p[2] && p[3])
            { cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu); p += 4; }
        else                                       { cp = '?'; p += 1; }

        if (cp >= 0x10000u)   // 补充平面 → 代理对
        {
            cp -= 0x10000u;
            dst[o++] = static_cast<Steinberg::char16> (0xD800u + (cp >> 10));
            if (o < dstCount - 1)
                dst[o++] = static_cast<Steinberg::char16> (0xDC00u + (cp & 0x3FFu));
        }
        else
            dst[o++] = static_cast<Steinberg::char16> (cp);
    }
    dst[o] = 0;
}

//------------------------------------------------------------------------------
// 工厂定义
//
// 注意：官方 SDK 3.8.1 的 BEGIN_FACTORY/DEF_CLASS 宏与同版本头文件不同步
//（DECLARE_UID 生成的数组在宏内触发 array initializer 错误），
// 所以这里手写工厂，但【必须完整填写 PClassInfo2 的所有字段】——
// classFlags / subCategories / vendor / version / sdkVersion 缺一不可。
// 之前手写工厂漏了 vendor 和 subCategories，MuseScore 就判定插件不兼容。
//
// PClassInfo2 字段（对照 pluginterfaces/base/ipluginbase.h）：
//   cid, cardinality, category[32], name[64], classFlags,
//   subCategories[128], vendor[64], version[64], sdkVersion[64]
//------------------------------------------------------------------------------
class APlayFactory : public Steinberg::CPluginFactory
{
public:
    APlayFactory () : CPluginFactory (factoryInfo ())
    {
        Steinberg::PClassInfo2 ci {};

        // ---- 处理器 ----
        memcpy (ci.cid, aplay::APLayProcessorUID, sizeof (Steinberg::TUID));
        ci.cardinality = Steinberg::PClassInfo::kManyInstances;
        // category 必须是 kVstAudioEffectClass（= "Audio Module Class"）。
        // MuseScore 硬编码匹配这个字符串作为「是否含音频效果」的判据；
        // 写 "Instrument" 会被判为 "[1406] VST3 file contains no audio effect"。
        // "Instrument" 这类值属于 subCategories（子分类）字段的语义。
        strncpy (ci.category, kVstAudioEffectClass,
                 Steinberg::PClassInfo::kCategorySize - 1);
        // ---- 类名（插件的显示名，DAW 菜单里「VST」下一级显示的就是它）----
        // 用中文品牌。实测 MuseScore 读插件名的通道能正确解码 UTF-8 中文，
        // 显示正常（不乱码）—— 所以这里保留中文，方便宣传。
        //
        // 注意：插件名上面那一级「菜单」是【厂商名】（见下方 vendor），
        // 它走的是 PFactoryInfo / PClassInfo2.vendor 那条较弱的编码通道，
        // 中文在那里会乱码 —— 所以 vendor 必须保持纯 ASCII。
        // 一句话：插件名可以中文，厂商名必须 ASCII。
        strncpy (ci.name, "大伟鼓谱MuseScore音频播放器", Steinberg::PClassInfo::kNameSize - 1);
        ci.classFlags = 0;
        strncpy (ci.subCategories, "Instrument", Steinberg::PClassInfo2::kSubCategoriesSize - 1);
        // 厂商名：纯 ASCII。它是插件名上面那一级菜单的分组名，中文会乱码。
        strncpy (ci.vendor, "Dawei DrumScore", Steinberg::PClassInfo2::kVendorSize - 1);
        strncpy (ci.version, "1.2.2", Steinberg::PClassInfo2::kVersionSize - 1);
        strncpy (ci.sdkVersion, kVstVersionString, Steinberg::PClassInfo2::kVersionSize - 1);
        registerClass (&ci, aplay::APlayProcessor::createInstance);

        // ---- 控制器 ----
        // 控制器类的 category 用 kVstComponentControllerClass（= "Component Controller Class"），
        // 与正常插件一致。
        memcpy (ci.cid, aplay::APLayControllerUID, sizeof (Steinberg::TUID));
        strncpy (ci.category, kVstComponentControllerClass,
                 Steinberg::PClassInfo::kCategorySize - 1);
        registerClass (&ci, aplay::APlayController::createInstance);
    }

    // 重写 Unicode 类信息（IPluginFactory3::getClassInfoUnicode）。
    //
    // 背景：SDK 的 CPluginFactory 注册时用 PClassInfoW::fromAscii 填 char16 字段，
    // 而它内部是 str8ToStr16 —— 只做「逐字节拓宽」（dst[i] = (char16)src[i]），
    // 不解码 UTF-8。于是「大」这种 3 字节汉字被拆成 3 个无效 UTF-16 单元，
    // 宿主若从这条通道取名字，菜单里就是乱码（MuseScore 正是如此）。
    //
    // 基类的 getClassInfo / getClassInfo2 通道（char8、真 UTF-8）本身是对的，
    // 所以这里以它为源，重新做一次正确的 UTF-8 → UTF-16 解码。
    tresult PLUGIN_API getClassInfoUnicode (int32 index, Steinberg::PClassInfoW* info) override
    {
        if (!info) return Steinberg::kInvalidArgument;

        Steinberg::PClassInfo2 ci2 {};
        if (getClassInfo2 (index, &ci2) != Steinberg::kResultOk)
            return Steinberg::kInvalidArgument;

        memset (info, 0, sizeof (Steinberg::PClassInfoW));
        memcpy (info->cid, ci2.cid, sizeof (Steinberg::TUID));
        info->cardinality = ci2.cardinality;
        strncpy (info->category, ci2.category, Steinberg::PClassInfo::kCategorySize - 1);
        info->classFlags = ci2.classFlags;
        strncpy (info->subCategories, ci2.subCategories,
                 Steinberg::PClassInfo2::kSubCategoriesSize - 1);
        utf8ToUtf16 (info->name,       Steinberg::PClassInfo::kNameSize,     ci2.name);
        utf8ToUtf16 (info->vendor,     Steinberg::PClassInfoW::kVendorSize,  ci2.vendor);
        utf8ToUtf16 (info->version,    Steinberg::PClassInfoW::kVersionSize, ci2.version);
        utf8ToUtf16 (info->sdkVersion, Steinberg::PClassInfoW::kVersionSize, ci2.sdkVersion);
        return Steinberg::kResultOk;
    }

    //--------------------------------------------------------------------------
    // 引用计数：工厂是进程级常驻单例，计数归零也不销毁
    //
    // 【这是被一次真实闪退逼出来的】症状：宿主里「关掉谱子 → 导入另一个工程
    // → 打开插件」宿主闪退，崩溃栈落在宿主自己的模块加载路径上
    // （VstModulesRepository::addPluginModule → Module::create）。
    //
    // 根因：宿主每次打开插件编辑器都会重读一遍模块元数据，也就是每次都会走
    // 「GetPluginFactory() + release()」一对。而 SDK 的 CPluginFactory 在计数
    // 归零时 `delete this`；它的析构只把 **SDK 自己的全局 gPluginFactory**
    // 置空（见 sdk/public.sdk/source/main/pluginfactory.cpp），管不到我们在
    // 函数内 static 缓存的指针。实测计数轨迹（见 factory_lifecycle_test.cpp）：
    //   第 1 次索取：new=1 → 我们 addRef=2 → 宿主 release=1
    //   第 2 次索取：不补引用仍=1 → 宿主 release=0 → delete this
    //   第 3 次索取：static 仍非空 → 返回【已释放内存】→ 宿主当场崩溃
    //
    // 所以这里把工厂做成常驻单例：计数照常维护（宿主自查时数是对的），
    // 但归零只钉回 1，绝不 delete。模块级资源本来就该活到进程退出，
    // 这样无论宿主的引用计数行为多不规范，都不可能再悬垂。
    //--------------------------------------------------------------------------
    Steinberg::uint32 PLUGIN_API addRef () override
    {
        return m_refCount.fetch_add (1, std::memory_order_relaxed) + 1;
    }

    Steinberg::uint32 PLUGIN_API release () override
    {
        Steinberg::uint32 r = m_refCount.fetch_sub (1, std::memory_order_acq_rel) - 1;
        if (r == 0)
        {
            m_refCount.store (1, std::memory_order_relaxed);   // 常驻，不销毁
            r = 1;
        }
        return r;
    }

private:
    std::atomic<Steinberg::uint32> m_refCount { 1 };

    static Steinberg::PFactoryInfo factoryInfo ()
    {
        Steinberg::PFactoryInfo fi {};
        strncpy (fi.vendor, "Dawei DrumScore", Steinberg::PFactoryInfo::kNameSize - 1);
        strncpy (fi.url, "", Steinberg::PFactoryInfo::kURLSize - 1);
        strncpy (fi.email, "", Steinberg::PFactoryInfo::kEmailSize - 1);
        fi.flags = Steinberg::Vst::kDefaultFactoryFlags;
        return fi;
    }
};

extern "C"
{
    //------------------------------------------------------------------
    // VST3 bundle 必须导出三个符号，缺一个宿主就拒绝加载：
    //   bundleEntry / bundleExit / GetPluginFactory
    //
    // 官方 SDK 的 MacModule::load() 会依次检查这三个，找不到就报
    // "Could not create VstModule"（MuseScore 日志里的 [1401]）。
    // 之前手写工厂时只导出了 GetPluginFactory，这是插件不显示的根因。
    //------------------------------------------------------------------
    SMTG_EXPORT_SYMBOL bool PLUGIN_API bundleEntry (void*)
    {
        // 装好崩溃取证：万一后续还有闪退，~/Library/Logs/DaweiDrumScore.log
        // 会留下信号/异常类型与调用栈，不用再靠猜。
        ap::installCrashGuard ();
        return true;
    }

    SMTG_EXPORT_SYMBOL bool PLUGIN_API bundleExit (void*)
    {
        return true;
    }

#if SMTG_OS_LINUX
    //------------------------------------------------------------------
    // Linux 的 VST3 模块入口。宿主（以及 SDK 的 moduleinfotool / validator）
    // 会用 dlsym 找这三个符号：
    //   ModuleEntry / ModuleExit / GetPluginFactory
    // 缺 ModuleEntry 时 moduleinfotool 直接报
    //   "The shared library does not export the required 'ModuleEntry' function"
    // 它是 POST_BUILD 步骤，一失败就 gmake Error 1 并把 .so 删掉 ——
    // 表面看像"链接失败"，其实链接早就成功了。
    //
    // 为什么入口要自己写：SDK 的 smtg_target_add_library_main() 只在
    // public_sdk_SOURCE_DIR 变量可见时才把 linuxmain.cpp 加进 target，
    // 而那个变量定义在 SDK 自己的 directory scope，不会传回父作用域，
    // 所以在我们的 CMakeLists 里它是空的（macOS 的 bundleEntry/bundleExit
    // 同样是这个原因才自己提供）。
    //------------------------------------------------------------------
    SMTG_EXPORT_SYMBOL bool PLUGIN_API ModuleEntry (void* sharedLibraryHandle)
    {
        (void) sharedLibraryHandle;
        ap::installCrashGuard ();   // 与 macOS 的 bundleEntry 对齐
        return true;
    }

    SMTG_EXPORT_SYMBOL bool PLUGIN_API ModuleExit ()
    {
        return true;
    }
#endif // SMTG_OS_LINUX

    SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory ()
    {
        // 与 SDK 官方宏 BEGIN_FACTORY_DEF / END_FACTORY 对齐：官方在 else 分支
        // 补了一次 addRef()，因为宿主【每次】索取工厂都会配对一次 release()。
        // 我们原先只在首次 addRef，计数会被宿主耗尽 —— 详见 APlayFactory 里
        // addRef/release 的注释（那次「关谱子换工程再开插件就闪退」的事故）。
        static APlayFactory* gFactory = nullptr;
        if (!gFactory)
            gFactory = new APlayFactory ();   // 构造后引用计数 = 1
        else
            gFactory->addRef ();              // 宿主每次索取都配对一次 release
        return static_cast<Steinberg::IPluginFactory*> (gFactory);
    }

    //------------------------------------------------------------------
    // 验证器接口（宿主不会调用，只为自动化测试暴露内部状态）
    //------------------------------------------------------------------

    SMTG_EXPORT_SYMBOL int aplayLiveProcessorCount ()
    {
        return aplay::liveProcessorCount ();
    }

    // 在没有（或已销毁）处理器的情况下，把代理后端的每个方法都调一遍。
    // 视图在宿主扫描阶段就是这个状态；宿主销毁处理器之后视图也可能还活着。
    // 这条路径必须永不崩 —— 返回 1 表示全部安全。
    SMTG_EXPORT_SYMBOL int aplayStableBackendProbe ()
    {
        ap::PlugView::Backend* b = aplay::stableBackend ();
        if (!b)
            return 0;

        (void) b->hasAudio ();
        (void) b->durationSec ();
        (void) b->positionSec ();
        (void) b->playing ();
        (void) b->looping ();
        (void) b->volume ();
        (void) b->offsetSec ();
        (void) b->currentPath ();
        (void) b->lastError ();

        std::vector<float> peaks;
        b->waveformPeaks (peaks, 32, 0.0, 0.0);

        b->setVolume (0.5f);
        b->setOffsetSec (1.0f);
        b->setPlaying (true);
        b->setLooping (true);
        b->seekTo (5.0);
        b->setGridBPM (120.0f);
        b->setGridBeatsPerBar (3);
        (void) b->gridBPM ();
        (void) b->gridBeatsPerBar ();
        (void) b->hostTimeline ();
        b->unloadFile ();

        // 载入不存在的文件：应当返回 false，而不是崩
        const bool loaded = b->loadFile ("/nonexistent/APLAY_probe.mp3");

        return loaded ? 0 : 1;
    }
}
