//==============================================================================
// plugin.h — 最小 VST3 基类实现
//
// 为什么自己写：Steinberg 的 Component / AudioProcessor / EditController
// 便捷基类在另一个仓库（vst3_public_sdk），pluginterfaces 只有纯接口。
//
// 接口清单由编译器诊断确认，不是靠阅读头文件猜测：
//   IComponent      — 15 个方法（注意：没有 getClassInfo(PClassInfo*)）
//   IAudioProcessor — 8 个方法
//   IEditController — 15 个方法（该版本仍有参数系统接口）
//
// 关键设计：FUnknown 被多个接口共同继承，所以引用计数用非继承的 mixin，
// 由每个类显式 override addRef/release，避免二义性。
//==============================================================================
#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/futils.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"

#include <cstring>

namespace Steinberg {
namespace Vst {

// Steinberg VST3 规范的插件分类字符串。
// 注意：不能用 VST2 的 "Audio Module Class" —— MuseScore 等宿主按 VST3 分类过滤，
// 分类不认识的插件会直接不显示（实测踩过）。
// 常用值："Instrument"（音源）、"Fx"（效果器）、"Instrument|Drum" 等，
// 多个分类用 | 分隔。
inline constexpr const char* kAudioEffectClassStr = "Instrument";

inline const TUID& pluginClassId ()
{
    static const TUID cid = INLINE_UID (0x41504C59, 0x41554449, 0x00000001, 0x00000000);
    return cid;
}

//------------------------------------------------------------------------------
// 引用计数 mixin（不继承 FUnknown，避免多重继承二义）
//------------------------------------------------------------------------------
class RefCountMixin
{
protected:
    RefCountMixin () : m_refCount (0) {}
    virtual ~RefCountMixin () = default;
    RefCountMixin (const RefCountMixin&) = delete;
    RefCountMixin& operator= (const RefCountMixin&) = delete;
    uint32 m_refCount = 0;
};

//------------------------------------------------------------------------------
// Component —— IComponent
//------------------------------------------------------------------------------
// 注意：IComponent 已继承 FUnknown，不能再显式继承（会造成菱形继承）。
// 引用计数由 RefCountMixin 提供，并显式 override 掉 FUnknown 的纯虚版本。
class Component : public IComponent,
                  public RefCountMixin
{
public:
    uint32 PLUGIN_API addRef () override { return ++m_refCount; }
    uint32 PLUGIN_API release () override
    {
        const uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    tresult PLUGIN_API queryInterface (const TUID _iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual (_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual (_iid, IComponent::iid))
        {
            *obj = static_cast<IComponent*> (this);
            addRef ();
            return kResultOk;
        }
        return kNoInterface;
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        if (m_initialized) return kResultFalse;
        m_initialized = true;
        return onInit (context);
    }

    virtual tresult onInit (FUnknown*) { return kResultOk; }

    tresult PLUGIN_API terminate () override
    {
        if (!m_initialized) return kResultFalse;
        m_initialized = false;
        return kResultOk;
    }

    tresult PLUGIN_API getControllerClassId (TUID classId) override
    {
        if (!classId) return kInvalidArgument;
        memcpy (classId, pluginClassId (), sizeof (TUID));
        return kResultOk;
    }

    tresult PLUGIN_API setIoMode (IoMode) override { return kResultOk; }

    int32 PLUGIN_API getBusCount (MediaType type, BusDirection dir) override
    {
        if (dir == kOutput && type == kAudio) return 1;
        return 0;
    }

    virtual tresult PLUGIN_API getBusInfo (MediaType, BusDirection, int32, BusInfo&)
    {
        return kResultFalse;
    }

    virtual tresult PLUGIN_API activateBus (MediaType, BusDirection, int32, TBool)
    {
        return kResultFalse;
    }

    tresult PLUGIN_API getRoutingInfo (RoutingInfo&, RoutingInfo&) override
    {
        return kResultFalse;
    }

    tresult PLUGIN_API setActive (TBool state) override
    {
        if (!m_initialized) return kResultFalse;
        m_active = state != 0;
        return kResultOk;
    }

    tresult PLUGIN_API setState (IBStream* state) override
    {
        if (!m_initialized) return kResultFalse;
        onRestoreState (state);
        return kResultOk;
    }

    tresult PLUGIN_API getState (IBStream* state) override
    {
        if (!m_initialized) return kResultFalse;
        onSaveState (state);
        return kResultOk;
    }

    virtual void onRestoreState (IBStream*) {}
    virtual void onSaveState (IBStream*) {}

protected:
    ~Component () override = default;
    bool m_initialized = false;
    bool m_active = false;
};

//------------------------------------------------------------------------------
// AudioProcessor —— IAudioProcessor
//------------------------------------------------------------------------------
class AudioProcessor : public IAudioProcessor,
                       public RefCountMixin
{
public:
    uint32 PLUGIN_API addRef () override { return ++m_refCount; }
    uint32 PLUGIN_API release () override
    {
        const uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    tresult PLUGIN_API queryInterface (const TUID _iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual (_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual (_iid, IAudioProcessor::iid))
        {
            *obj = static_cast<IAudioProcessor*> (this);
            addRef ();
            return kResultOk;
        }
        return kNoInterface;
    }

    tresult PLUGIN_API setupProcessing (ProcessSetup& setup) override
    {
        // 注意：kRealtime 的枚举值是 0，不能用 !setup.processMode 判断合法性
        // （那样会把最常用的实时模式误判为无效）。这里显式校验范围。
        if (setup.processMode < kRealtime || setup.processMode > kOffline)
            return kResultFalse;
        m_setup = setup;
        return onSetup (setup);
    }

    virtual tresult onSetup (ProcessSetup& setup)
    {
        m_hostSampleRate = setup.sampleRate;
        return kResultOk;
    }

    // 固定立体声输出，不接受宿主的总线重排请求
    tresult PLUGIN_API setBusArrangements (SpeakerArrangement* /*inputs*/, int32 /*numIns*/,
                                           SpeakerArrangement* /*outputs*/, int32 /*numOuts*/) override
    {
        return kResultFalse;
    }

    // 输出恒为立体声
    tresult PLUGIN_API getBusArrangement (BusDirection dir, int32 index,
                                          SpeakerArrangement& arr) override
    {
        if (dir != kOutput || index != 0)
            return kResultFalse;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize (int32 symbolicSampleSize) override
    {
        return (symbolicSampleSize == kSample32) ? kResultOk : kResultFalse;
    }

    uint32 PLUGIN_API getLatencySamples () override { return 0; }
    uint32 PLUGIN_API getTailSamples () override { return 0; }

    tresult PLUGIN_API setProcessing (TBool state) override
    {
        m_processing = state != 0;
        return kResultOk;
    }

    tresult PLUGIN_API process (ProcessData& data) override { return onProcess (data); }
    virtual tresult onProcess (ProcessData&) { return kResultFalse; }

protected:
    ~AudioProcessor () override = default;
    ProcessSetup m_setup {};
    double m_hostSampleRate = 44100.0;
    bool m_processing = false;
};

//------------------------------------------------------------------------------
// EditController —— IEditController + IComponent
//
// 参数：getParameterCount 返回 0，即不向宿主暴露参数。
// MuseScore 只把插件当音频输出，不显示插件参数条，
// 所有控制都在自绘 GUI 里完成，这样用户能拖文件、调偏移。
//------------------------------------------------------------------------------
class EditController : public IComponent,
                       public IEditController,
                       public RefCountMixin
{
public:
    uint32 PLUGIN_API addRef () override { return ++m_refCount; }
    uint32 PLUGIN_API release () override
    {
        const uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    tresult PLUGIN_API queryInterface (const TUID _iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;

        if (FUnknownPrivate::iidEqual (_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual (_iid, IEditController::iid))
        {
            *obj = static_cast<IEditController*> (this);
            addRef ();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual (_iid, IComponent::iid))
        {
            *obj = static_cast<IComponent*> (this);
            addRef ();
            return kResultOk;
        }
        return kNoInterface;
    }

    //---- IPluginBase ----
    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        if (m_initialized) return kResultFalse;
        m_initialized = true;
        return onInit (context);
    }

    virtual tresult onInit (FUnknown*) { return kResultOk; }

    tresult PLUGIN_API terminate () override
    {
        if (!m_initialized) return kResultFalse;
        m_initialized = false;
        return kResultOk;
    }

    //---- IComponent ----
    tresult PLUGIN_API getControllerClassId (TUID classId) override
    {
        if (!classId) return kInvalidArgument;
        memcpy (classId, pluginClassId (), sizeof (TUID));
        return kResultOk;
    }

    tresult PLUGIN_API setIoMode (IoMode) override { return kResultOk; }

    int32 PLUGIN_API getBusCount (MediaType type, BusDirection dir) override
    {
        if (dir == kOutput && type == kAudio) return 1;
        return 0;
    }

    tresult PLUGIN_API getBusInfo (MediaType, BusDirection, int32, BusInfo&) override
    {
        return kResultFalse;
    }

    tresult PLUGIN_API activateBus (MediaType, BusDirection, int32, TBool) override
    {
        return kResultFalse;
    }

    tresult PLUGIN_API getRoutingInfo (RoutingInfo&, RoutingInfo&) override
    {
        return kResultFalse;
    }

    tresult PLUGIN_API setActive (TBool state) override
    {
        m_active = state != 0;
        return kResultOk;
    }

    tresult PLUGIN_API setState (IBStream* stream) override
    {
        onRestoreState (stream);
        return kResultOk;
    }

    tresult PLUGIN_API getState (IBStream* stream) override
    {
        onSaveState (stream);
        return kResultOk;
    }

    virtual void onRestoreState (IBStream*) {}
    virtual void onSaveState (IBStream*) {}

    //---- IEditController ----
    tresult PLUGIN_API setComponentState (IBStream* state) override
    {
        onRestoreState (state);
        return kResultOk;
    }

    int32 PLUGIN_API getParameterCount () override { return 0; }

    tresult PLUGIN_API getParameterInfo (int32, ParameterInfo&) override { return kResultFalse; }

    ParamValue PLUGIN_API getParamNormalized (ParamID) override { return 0.0; }

    tresult PLUGIN_API setParamNormalized (ParamID, ParamValue) override { return kResultFalse; }

    tresult PLUGIN_API getParamValueByString (ParamID, TChar*, ParamValue&) override
    {
        return kResultFalse;
    }

    tresult PLUGIN_API getParamStringByValue (ParamID, ParamValue, String128) override
    {
        return kResultFalse;
    }

    ParamValue PLUGIN_API normalizedParamToPlain (ParamID, ParamValue) override { return 0.0; }

    ParamValue PLUGIN_API plainParamToNormalized (ParamID, ParamValue) override { return 0.0; }

    tresult PLUGIN_API setComponentHandler (IComponentHandler* handler) override
    {
        m_handler = handler;
        return kResultOk;
    }

    IPlugView* PLUGIN_API createView (FIDString) override { return nullptr; }

protected:
    ~EditController () override = default;
    bool m_initialized = false;
    bool m_active = false;
    IComponentHandler* m_handler = nullptr;
};

//------------------------------------------------------------------------------
// UTF-8 → String128（UTF-16LE）
//------------------------------------------------------------------------------
inline void toString128 (const char* in, String128 out, int32 maxChars = 127)
{
    if (!out) return;
    if (!in)
    {
        out[0] = 0;
        return;
    }
    int32 i = 0;
    for (; i < maxChars && in[i]; ++i)
        out[i] = static_cast<uint16> (static_cast<unsigned char> (in[i]));
    out[i] = 0;
}

}} // namespace Steinberg::Vst
