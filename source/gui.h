//==============================================================================
// gui.h — 中文界面（AppKit 实现 VST3 IPlugView）
//
// 为什么用 AppKit 而不是 VSTGUI：
//   VSTGUI 在 vst3_public_sdk 仓库（没下载）。AppKit 是系统自带，
//   零依赖，中文显示天然支持。
//
// 界面功能：
//   - 拖拽音频文件进来（或点按钮选文件）
//   - 波形预览 + 可拖动的播放头
//   - 起始偏移滑块（核心需求：不用预先裁剪音频）
//   - 播放/停止、音量、循环
//==============================================================================
#pragma once

// 平台无关：本头文件被 aplaysdk（纯 C++）与各平台 GUI 实现共同包含。
// 平台框架（Cocoa / windows.h）由各自的实现文件自行引入，勿在此处 import，
// 否则会把 ObjC 依赖传染给 Windows 构建。
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ap {

//------------------------------------------------------------------------------
// IPlugView 实现
//------------------------------------------------------------------------------
using Steinberg::tresult;
using Steinberg::FIDString;
using Steinberg::TBool;
using Steinberg::char16;
using Steinberg::int16;
using Steinberg::uint32;
using Steinberg::ViewRect;
using Steinberg::IPlugFrame;
using Steinberg::IPlugView;

//------------------------------------------------------------------------------
// 作者 B 站主页 —— 底部「♪ B 站「大伟鼓谱」· 欢迎关注…」那条粉色宣传语的跳转目标。
//
// ⭐ 三端（macOS / Windows / Linux）共用这一个常量，不要在各自的 gui 文件里
//    再各写一份 URL 字符串。本项目已经反复栽在「同一语义在三端各写一遍、
//    改的时候漏掉一端」上（见 MEMORY 铁律 9：必须逐端比对）—— 地址只此一处，
//    改地址就改这一行。
//------------------------------------------------------------------------------
inline constexpr const char* kBrandHomeUrl = "https://space.bilibili.com/65320474";

// 注意：不要显式再继承 Steinberg::FUnknown —— IPlugView 已经继承它，
// 重复继承会触发 MSVC C4584（base-class already a base-class）。
class PlugView : public IPlugView
{
public:
    // 需要操作的处理器（在 plugin.cpp 里传入）
    class Backend
    {
    public:
        virtual ~Backend () = default;
        // 从文件加载音频
        virtual bool loadFile (const std::string& path) = 0;
        virtual void unloadFile () = 0;
        virtual bool hasAudio () const = 0;
        virtual double durationSec () const = 0;
        virtual std::string currentPath () const = 0;
        virtual std::string lastError () const = 0;

        // 波形数据（峰值包络），供 GUI 绘制。
        // fromSec / toSec 限定取样区间 —— 放大后必须按可见范围重新取样，
        // 否则波峰糊成一片，看不出鼓点在哪。toSec <= fromSec 表示全曲。
        virtual void waveformPeaks (std::vector<float>& out, int buckets,
                                    double fromSec, double toSec) const = 0;

        //---- 参数（原子读写，与音频线程共享）----
        virtual void setOffsetSec (float sec) = 0;
        virtual float offsetSec () const = 0;
        virtual void setVolume (float v) = 0;
        virtual float volume () const = 0;
        virtual void setPlaying (bool b) = 0;
        virtual bool playing () const = 0;
        virtual void setLooping (bool b) = 0;
        virtual bool looping () const = 0;
        virtual void seekTo (double sec) = 0;
        virtual double positionSec () const = 0;

        //---- 小节网格 ----
        // 波形上按「每分钟几拍 + 拍号 N/M」画小节线与拍线，原点就是起始偏移，
        // 于是「网格线是否落在鼓点上」直接反映当前的谱面/音频对齐状况。
        // bpm 是每分钟「四分音符」的个数；拍号 N/M 表示每小节 N 个「M 分音符」，
        // 每拍时长 = 60/bpm × (4/M)。bpm == 0 表示自动（优先用宿主速度）。
        virtual void setGridBPM (float bpm) = 0;
        virtual float gridBPM () const = 0;
        virtual void setGridBeatsPerBar (int beats) = 0;
        virtual int gridBeatsPerBar () const = 0;
        virtual void setGridBeatDenominator (int denom) = 0;   ///< 拍号分母 M（2/4/8/16）
        virtual int gridBeatDenominator () const = 0;

        //---- 宿主时间轴 ----
        // 宿主（MuseScore）会在 ProcessContext 里附带速度、拍号和谱面位置。
        // 拿到了就能让网格自动跟随乐谱速度，并把谱面播放头画在波形上 ——
        // 一眼看出「谱面第几小节正对着音频的哪一段」。
        struct HostTimeline
        {
            bool   tempoValid = false;
            float  bpm = 0.0f;
            bool   timeSigValid = false;
            int    beatsPerBar = 0;
            int    beatDenominator = 4;
            bool   playheadValid = false;
            double playheadSec = 0.0;   ///< 谱面位置（秒，自乐谱开头算）
            bool   playing = false;
            // ProcessContext.state 的原始位图。仅用于一次性诊断：
            // 让用户实测一次就能从日志看出宿主到底填了哪些字段。
            uint32 rawState = 0;
        };
        virtual HostTimeline hostTimeline () const = 0;

        //---- 视野状态（关掉编辑器再打开必须恢复）--------------------------------
        // 编辑器视图的寿命比处理器短：宿主关界面只销毁 PlugView，重开时视图是
        // 【新建】的，里面所有控件与状态都是初值。而「用户放大到多细、正看着哪
        // 一段」和 BPM 一样是用户的操作结果，必须存在后端里 —— 不存的话重开界面
        // 就回到默认视野（用户实测：「把插件界面关掉再打开，波形变成整曲全览」）。
        //
        // spanSec <= 0 表示「还没设置过」（或换了音频文件）→ 视图自己回落到
        // 新载入的默认视野（见各端 kDefaultViewSpanSec）。
        virtual void setViewState (double startSec, double spanSec) = 0;
        virtual void getViewState (double& startSec, double& spanSec) const = 0;

        //---- 实时健康探针（可选实现，默认 0）------------------------------------
        // 「音频线程因抢不到播放器的锁而整块丢弃音频」的累计次数。
        //
        // 为什么需要它：音频线程的 render() 用 try_lock，抢不到就整块输出静音
        // 并 return —— 每丢一块，播放位置就永久落后宿主约一个缓冲区的时长。
        // 表现是「播放中左右拖动波形越拖越错位 / 音频发抖」，而界面上完全看不
        // 出原因。有了这个计数，界面线程就能把这个隐形故障变成一条日志。
        // 正常情况下必须恒为 0（GUI 扫波形已改走不可变快照，不再持锁）。
        virtual uint64_t lockDropCount () const { return 0; }
    };

    // 后端解析器：宿主可能先 createView（此时后端还没接上）后 connect，
    // 所以在 attached 时再解析一次，拿最新的处理器实例。
    using BackendResolver = Backend* (*) ();

    PlugView (Backend* backend, BackendResolver resolver = nullptr);

    //---- FUnknown ----
    uint32 PLUGIN_API addRef () override { return ++m_refCount; }
    uint32 PLUGIN_API release () override
    {
        const uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
    tresult PLUGIN_API queryInterface (const Steinberg::TUID _iid, void** obj) override;

    //---- IPlugView ----
    tresult PLUGIN_API isPlatformTypeSupported (FIDString type) override;
    tresult PLUGIN_API attached (void* parent, FIDString type) override;
    tresult PLUGIN_API removed () override;
    tresult PLUGIN_API onWheel (float distance) override;
    tresult PLUGIN_API onKeyDown (char16 key, int16 keyCode, int16 modifiers) override;
    tresult PLUGIN_API onKeyUp (char16 key, int16 keyCode, int16 modifiers) override;
    tresult PLUGIN_API getSize (ViewRect* size) override;
    tresult PLUGIN_API onSize (ViewRect* newSize) override;
    tresult PLUGIN_API onFocus (Steinberg::TBool state) override;
    tresult PLUGIN_API setFrame (IPlugFrame* frame) override;
    tresult PLUGIN_API canResize () override;
    tresult PLUGIN_API checkSizeConstraint (ViewRect* rect) override;

    // 供验证器检查「视图是否真的拿到了音频后端」
    Backend* backendForTest () const { return m_backend; }

    // Windows 专用：请宿主按当前首选尺寸（含 DPI 缩放）重新调整窗口。
    // 只在 gui_win.cpp 里实现，其他平台不调用。
    void requestResizeToPreferred ();

private:
    Backend* m_backend = nullptr;
    BackendResolver m_resolver = nullptr;
    void* m_view = nullptr;   // GUIView* （ObjC 类，用 void* 跨语言边界持有）
    // 宿主回调。Windows 版用得上：DPI 缩放系数在 attached 时才确定，若与
    // getSize 之前上报的尺寸不一致，要通过 resizeView 请宿主重新调整窗口。
    // （Mac/Linux 版按规范原样存下来，但不主动请求缩放 —— 面板由用户拖窗口驱动。）
    IPlugFrame* m_frame = nullptr;
    uint32 m_refCount = 0;
};

} // namespace ap
