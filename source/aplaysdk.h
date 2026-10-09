//==============================================================================
// aplaysdk.h — 基于 Steinberg public.sdk 官方基类的 VST3 插件
//
// 为什么改用官方基类：
//   之前手写 Component / AudioProcessor / EditController，只实现了
//   pluginterfaces 的纯接口。结果 MuseScore 扫描后判定 "incompatible"，
//   72ms 就放弃（日志证���）—— 说明它检查的环节里有手写实现没覆盖到的。
//   官方基类（AudioEffect / EditController）+ 官方工厂宏
//   （BEGIN_FACTORY_DEF / DEF_CLASS）是唯一能保证宿主认可的实现方式。
//
// 保留不变的部分：
//   - 播放核心（player.cpp，32 项测试已通过）
//   - 音频解码（audiofile.mm，AVFoundation 零依赖）
//   - 中文界面（gui.mm，AppKit）
//==============================================================================
#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"

#include "audiofile.h"
#include "gui.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <string>

// 历史上这些源码直接使用裸 uint32（原先由 macOS 的 Cocoa 头间接提供）。
// 跨平台后必须显式引入，否则 Windows 构建会报 unknown type name 'uint32'。
using Steinberg::uint32;

namespace aplay {

//------------------------------------------------------------------------------
// UID 定义（四个字节一段，必须用十六进制字面量）
//------------------------------------------------------------------------------
// 用 DECLARE_UID（工厂宏 DEF_CLASS 需要这种形式），
// 不是 DECLARE_CLASS_IID —— 两者生成的符号名不同（后者带 _iid 后缀）
DECLARE_UID (APLayProcessorUID,  0x41504C41, 0x59505031, 0x30303030, 0x00000001)
DECLARE_UID (APLayControllerUID, 0x41504C41, 0x59504354, 0x30303030, 0x00000002)

//------------------------------------------------------------------------------
// 处理器 —— 音频侧
//------------------------------------------------------------------------------
class APlayProcessor : public Steinberg::Vst::AudioEffect,
                       public ap::PlugView::Backend
{
public:
    APlayProcessor ();
    ~APlayProcessor () override;

    // 注意：这里【故意】不再暴露“最近构造的处理器实例”这类裸指针入口。
    //
    // 上一版把处理器裸指针交给编辑器视图，结果是：宿主先销毁音频处理器、
    // 后销毁编辑器视图（VST3 允许这个顺序，且 MuseScore 实际会这么干），
    // 视图里的 20Hz 刷新定时器和“打开音频”按钮就打在已释放内存上。
    // 换成 aplay::stableBackend() 返回的永生代理，见文件末尾说明。

    static Steinberg::FUnknown* createInstance (void*)
    {
        return (Steinberg::Vst::IAudioProcessor*) new APlayProcessor ();
    }

    //---- 生命周期 ----
    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setProcessing (Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setupProcessing (Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 size) override;

    //---- 状态 ----
    Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) override;

    //---- 音频 ----
    Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) override;

    //---- PlugView::Backend：给 GUI 的回调 ----
    bool loadFile (const std::string& path) override;
    void unloadFile () override;
    bool hasAudio () const override;
    double durationSec () const override;
    std::string currentPath () const override;
    std::string lastError () const override;
    void waveformPeaks (std::vector<float>& out, int buckets,
                        double fromSec, double toSec) const override;
    void setOffsetSec (float sec) override;
    float offsetSec () const override;
    void setVolume (float v) override;
    float volume () const override;
    void setPlaying (bool b) override;
    bool playing () const override;
    void setLooping (bool b) override;
    bool looping () const override;
    void seekTo (double sec) override;
    double positionSec () const override;
    void setGridBPM (float bpm) override;
    float gridBPM () const override;
    void setGridBeatsPerBar (int beats) override;
    int gridBeatsPerBar () const override;
    void setGridBeatDenominator (int denom) override;
    int gridBeatDenominator () const override;
    ap::PlugView::Backend::HostTimeline hostTimeline () const override;

    // 视野状态（见 gui.h 的说明）：编辑器视图每次打开都是新建的，所以
    // 「正看着哪一段 / 放大到多少」必须存在处理器里，重开界面才回得去。
    void setViewState (double startSec, double spanSec) override;
    void getViewState (double& startSec, double& spanSec) const override;

    /// 实时健康探针：音频线程因抢不到锁而整块丢弃音频的累计次数（见 gui.h）。
    uint64_t lockDropCount () const override;

private:
    void loadCurrentFile ();
    void applyParams ();
    void captureHostTimeline (Steinberg::Vst::ProcessData& data);

    // 宿主采样率：宿主可能在运行中重新配置（切音频设备），
    // 音频线程会读、宿主线程会写 —— 用原子量避免数据竞争。
    std::atomic<double> m_hostSampleRate {44100.0};
    bool m_setupPending = false;
    ap::Player m_player;
    std::string m_filePath;
    std::string m_loadError;
    bool m_loadedOk = false;

    // 音频后端自有状态：参数的权威副本放在这里，
    // 代理只做转发、不保存任何状态（也就没有跨线程数据竞争）。
    float m_volume = 1.0f;
    float m_offset = 0.0f;
    bool  m_playing = true;
    bool  m_loop = false;

    // 小节网格：0 = 自动（优先用宿主速度）
    float m_gridBPM = 0.0f;
    int   m_gridBeats = 4;
    int   m_gridBeatDenominator = 4;   ///< 拍号分母 M（2/4/8/16）

    //---- 视野状态（关掉编辑器再打开要恢复，见 gui.h）----
    // 单独一把小锁：它只被界面线程读写，与音频线程毫无交集 ——
    // 绝不是「音频线程要抢的锁」（对比铁律 12：持 m_mutex 扫采样会让 render() 丢块）。
    // span <= 0 = 还没设置过（或刚换了音频文件）→ 视图回落到默认视野。
    mutable std::mutex m_viewMutex;
    double m_viewStart = 0.0;
    double m_viewSpan  = 0.0;

    // 宿主时间轴缓存（音频线程写、界面线程读，所以用原子量）。
    // playheadSec 用 NaN 表示「宿主没给位置」。
    std::atomic<float>  m_hostBPM {0.0f};
    std::atomic<int>    m_hostBeatsPerBar {0};
    std::atomic<int>    m_hostBeatDenominator {4};
    std::atomic<double> m_hostPlayheadSec {std::numeric_limits<double>::quiet_NaN ()};
    std::atomic<bool>   m_hostPlaying {false};
    std::atomic<uint32> m_hostRawState {0};

    // ---- 跳转跟随（音频线程内部状态，只在 process 线程访问，无需原子）----
    // 之前在 GUI 的 20Hz 定时器里检测宿主播放头跳变，导致「UI 关闭后跳转失效」。
    // 现在下沉到音频线程：每块对比 projectTimeSamples 的位置，检测到「非连续
    // 前进/回跳」即判定为宿主手动拖了播放头，记录目标音频位置，下一块渲染前 seek。
    bool   m_hostSeekSeen = false;        ///< 是否已拿到过有效谱面位置
    double m_hostLastPlayhead = 0.0;      ///< 上一块拿到的谱面位置（秒）
    bool   m_hostWasPlaying = false;      ///< 上一块宿主是否在播放（检测播放边沿）
    // 待执行的 seek 目标（音频时间轴，秒）。NaN 表示没有待处理跳转。
    // 用原子量是因为 GUI 线程的「回到谱面」也会写入它，音频线程读。
    std::atomic<double> m_pendingSeekSec {std::numeric_limits<double>::quiet_NaN ()};

    enum : Steinberg::int32
    {
        kIdVolume = 0,
        kIdOffset = 1,
        kIdPlay = 2,
        kIdLoop = 3,
        kIdGridBPM = 4,
        kIdGridBeats = 5,
        kIdGridBeatDenominator = 6,
        kIdFilePath = 100
    };
};

//------------------------------------------------------------------------------
// 控制器 —— 界面侧
//------------------------------------------------------------------------------
class APlayController : public Steinberg::Vst::EditControllerEx1
{
public:
    APlayController () = default;
    ~APlayController () override = default;

    static Steinberg::FUnknown* createInstance (void*)
    {
        return (Steinberg::Vst::IEditController*) new APlayController ();
    }

    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) override;
    Steinberg::IPlugView* PLUGIN_API createView (Steinberg::FIDString name) override;

};

//------------------------------------------------------------------------------
// 稳定后端代理（进程生命周期内永不销毁）
//
// 解决的问题：
//   编辑器视图会被宿主长期持有，而音频处理器的销毁时机由宿主决定，
//   两者没有从属关系。只要视图里缓存了处理器裸指针，就存在
//   “宿主已销毁处理器 → 视图仍在使用”的野指针窗口，表现为闪退。
//
// 做法：
//   视图只拿这个代理（地址恒定，进程退出才消失）。代理内部通过注册表
//   解析到“当前活跃的处理器”，处理器析构时自动注销。没有处理器时
//   所有调用返回安全默认值（无音频、不播放），而不是崩溃。
//------------------------------------------------------------------------------
ap::PlugView::Backend* stableBackend ();

// 当前活跃的处理器实例数。给验证器确认「注册表真的在工作」用，
// 也用来断言「处理器销毁之后代理确实解析不到任何后端」。
int liveProcessorCount ();

} // namespace aplay
