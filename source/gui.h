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

private:
    Backend* m_backend = nullptr;
    BackendResolver m_resolver = nullptr;
    void* m_view = nullptr;   // GUIView* （ObjC 类，用 void* 跨语言边界持有）
    uint32 m_refCount = 0;
};

} // namespace ap
