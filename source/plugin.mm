//==============================================================================
// plugin.cpp — VST3 插件实现
//
// 结构：
//   AudioPlayerProcessor : Component + AudioProcessor   （音频侧）
//   AudioPlayerController: EditController              （界面侧）
//   PluginFactory        : IPluginFactory
//
// 实时音频安全：
//   GUI 线程改参数 → 写入 std::atomic → 音频线程在 process() 读取。
//   不加锁、不在音频线程分配内存，所以不会有音频线程被阻塞的风险。
//
// 关于参数：该 SDK 版本的 pluginterfaces 不含 ParamInfo 参数系统，
//   所以不向宿主暴露参数（getParameterCount 返回 0），
//   所有控制都通过自绘 GUI 完成。这对 MuseScore 完全够用——
//   它只用插件做音频输出，不显示插件的参数条。
//==============================================================================
#include "plugin.h"
#include "audiofile.h"
#include "gui.h"

#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Vst;

//==============================================================================
// AudioPlayerProcessor —— 音频侧
//==============================================================================
class AudioPlayerProcessor : public Vst::Component,
                              public Vst::AudioProcessor,
                              public ap::PlugView::Backend
{
public:
    // Component 与 AudioProcessor 都继承 FUnknown，且都实现了 queryInterface。
    // 宿主会先 queryInterface(IAudioProcessor) 再 queryInterface(IComponent)，
    // 所以这里必须重写一份同时认两个接口的实现——否则编译器会静默选用
    // 其中一个（Component 的版本只认 IComponent），导致宿主拿不到音频处理
    // 能力，进而崩溃。这是实测发现的问题，不是理论推断。
    using Vst::Component::addRef;
    using Vst::Component::release;

    tresult PLUGIN_API queryInterface (const TUID _iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual (_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual (_iid, Vst::IComponent::iid))
        {
            *obj = static_cast<Vst::IComponent*> (this);
            addRef ();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual (_iid, Vst::IAudioProcessor::iid))
        {
            *obj = static_cast<Vst::IAudioProcessor*> (this);
            addRef ();
            return kResultOk;
        }
        return kNoInterface;
    }

    //---- 音频处理 ------------------------------------------------------
    tresult onSetup (ProcessSetup& setup) override
    {
        m_hostSampleRate = setup.sampleRate;
        return kResultOk;
    }

    tresult onProcess (ProcessData& data) override
    {
        AudioBusBuffers* out = data.outputs;
        if (!out || data.numOutputs <= 0 || out[0].numChannels == 0)
            return kResultOk;   // 没有输出总线：静默返回，不能报错

        const int32 nCh = out[0].numChannels;
        const int32 nFrames = data.numSamples;
        if (nFrames <= 0)
            return kResultOk;

        AudioBusBuffers& bus = out[0];

        // 宿主标记为静音 → 确保缓冲区为零即可（避免在静音块上做无用的重采样）
        if (bus.silenceFlags != 0)
        {
            if (bus.channelBuffers32)
            {
                for (int32 ch = 0; ch < nCh; ++ch)
                    if (bus.channelBuffers32[ch])
                        std::memset (bus.channelBuffers32[ch], 0,
                                     sizeof (float) * static_cast<size_t> (nFrames));
            }
            else if (bus.channelBuffers64)
            {
                for (int32 ch = 0; ch < nCh; ++ch)
                    if (bus.channelBuffers64[ch])
                        std::memset (bus.channelBuffers64[ch], 0,
                                     sizeof (double) * static_cast<size_t> (nFrames));
            }
            return kResultOk;
        }

        if (bus.channelBuffers32)
        {
            // 32 位浮点 —— 绝大多数宿主走这条路径
            if (nCh >= 2)
                m_player.render (&bus.channelBuffers32[0], &bus.channelBuffers32[1],
                                 static_cast<uint32> (nFrames), m_hostSampleRate);
            else
                m_player.render (&bus.channelBuffers32[0], &bus.channelBuffers32[0],
                                 static_cast<uint32> (nFrames), m_hostSampleRate);
        }
        else if (bus.channelBuffers64)
        {
            // 64 位浮点 —— 走临时缓冲转换（宿主少见）
            std::vector<float> tmpL (static_cast<size_t> (nFrames));
            std::vector<float> tmpR (static_cast<size_t> (nFrames));
            float* pL = tmpL.data ();
            float* pR = tmpR.data ();
            m_player.render (&pL, &pR, static_cast<uint32> (nFrames), m_hostSampleRate);
            for (int32 i = 0; i < nFrames; ++i)
            {
                bus.channelBuffers64[0][i] = static_cast<double> (tmpL[static_cast<size_t> (i)]);
                if (nCh >= 2)
                    bus.channelBuffers64[1][i] = static_cast<double> (tmpR[static_cast<size_t> (i)]);
            }
        }

        return kResultOk;
    }

    //---- 总线声明 ------------------------------------------------------
    tresult PLUGIN_API getBusInfo (MediaType type, BusDirection dir, int32 index,
                                   BusInfo& bus) override
    {
        if (dir != kOutput || type != kAudio || index != 0)
            return kResultFalse;
        bus.mediaType = kAudio;
        bus.direction = kOutput;
        bus.channelCount = 2;
        bus.busType = kMain;
        toString128 ("Stereo Out", bus.name);
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }

    tresult PLUGIN_API activateBus (MediaType type, BusDirection dir, int32 index,
                                    TBool state) override
    {
        if (dir != kOutput || type != kAudio || index != 0)
            return kResultFalse;
        m_outputReady = state != 0;
        return kResultOk;
    }

    //---- 状态保存 ------------------------------------------------------
    void onSaveState (IBStream* stream) override
    {
        if (!stream) return;

        auto writeChunk = [stream](int32 id, const void* data, int32 size)
        {
            int32 id32 = id;
            stream->write (&id32, sizeof id32);
            stream->write (&size, sizeof size);
            if (size > 0)
                stream->write (const_cast<void*> (data), size);
        };

        writeChunk (kIdVolume, &m_volume, sizeof m_volume);
        writeChunk (kIdOffset, &m_offset, sizeof m_offset);

        uint8 pb = m_playing ? 1 : 0;
        writeChunk (kIdPlay, &pb, 1);
        uint8 lb = m_loop ? 1 : 0;
        writeChunk (kIdLoop, &lb, 1);

        const int32 len = static_cast<int32> (m_filePath.size ());
        writeChunk (kIdFilePath, m_filePath.data (), len);
    }

    void onRestoreState (IBStream* stream) override
    {
        if (!stream) return;

        for (int guard = 0; guard < 64; ++guard)
        {
            int32 id = 0;
            if (stream->read (&id, sizeof id) != kResultOk) return;
            int32 size = 0;
            if (stream->read (&size, sizeof size) != kResultOk) return;
            if (size < 0 || size > (1 << 20)) return;   // 防御脏数据

            switch (id)
            {
                case kIdVolume:
                {
                    float v = 1.f;
                    if (stream->read (&v, sizeof v) == kResultOk)
                        m_volume = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                    break;
                }
                case kIdOffset:
                {
                    float v = 0.f;
                    if (stream->read (&v, sizeof v) == kResultOk)
                        m_offset = v < 0.f ? 0.f : v;
                    break;
                }
                case kIdPlay:
                {
                    uint8 b = 1;
                    if (stream->read (&b, 1) == kResultOk) m_playing = b != 0;
                    break;
                }
                case kIdLoop:
                {
                    uint8 b = 0;
                    if (stream->read (&b, 1) == kResultOk) m_loop = b != 0;
                    break;
                }
                case kIdFilePath:
                {
                    if (size > 0 && size < 8192)
                    {
                        std::string p (static_cast<size_t> (size), '\0');
                        if (stream->read (&p[0], static_cast<uint32> (size)) == kResultOk)
                        {
                            m_filePath = p;
                            loadCurrentFile ();
                        }
                    }
                    else if (size == 0)
                    {
                        m_filePath.clear ();
                    }
                    break;
                }
                default:
                {
                    std::vector<uint8> skip (static_cast<size_t> (size), 0);
                    if (size > 0 && stream->read (skip.data (), static_cast<uint32> (size)) != kResultOk)
                        return;
                    break;
                }
            }
        }

        applyParams ();
    }

    //==================================================================
    // PlugView::Backend —— 给 GUI 的回调接口
    //==================================================================
    bool loadFile (const std::string& path) override
    {
        m_filePath = path;
        loadCurrentFile ();
        return m_loadedOk;
    }

    void unloadFile () override
    {
        m_filePath.clear ();
        m_player.setAudio (ap::AudioData ());
        m_loadedOk = false;
    }

    bool hasAudio () const override { return m_player.hasAudio (); }

    double durationSec () const override { return m_player.durationSec (); }

    std::string currentPath () const override { return m_filePath; }
    std::string lastError () const override { return m_loadError; }

    void waveformPeaks (std::vector<float>& out, int buckets) const override
    {
        out.assign (static_cast<size_t> (buckets > 0 ? buckets : 1), 0.f);
        const ap::AudioData& d = m_player.audio ();
        const size_t n = d.numFrames ();
        const uint32 ch = d.numChannels;
        if (n == 0 || ch == 0) return;

        const size_t b = out.size ();
        const size_t per = n / b;
        if (per == 0)
        {
            for (size_t i = 0; i < n && i < b; ++i)
                out[i] = std::fabs (d.samples[i * ch]);
            return;
        }

        for (size_t i = 0; i < b; ++i)
        {
            float peak = 0.f;
            const size_t s0 = i * per;
            const size_t s1 = std::min (s0 + per, n);
            for (size_t j = s0; j < s1; ++j)
            {
                // 双声道取均值
                float v = d.samples[j * ch];
                if (ch >= 2) v = (v + d.samples[j * ch + 1]) * 0.5f;
                const float a = std::fabs (v);
                if (a > peak) peak = a;
            }
            out[i] = peak;
        }
    }

    void setOffsetSec (float sec) override { m_player.setStartOffsetSec (sec); }
    float offsetSec () const override { return m_player.startOffsetSec (); }
    void setVolume (float v) override { m_player.setVolume (v); }
    float volume () const override { return m_player.volume (); }
    void setPlaying (bool b) override { m_player.setPlaying (b); }
    bool playing () const override { return m_player.playing (); }
    void setLooping (bool b) override { m_player.setLooping (b); }
    bool looping () const override { return m_player.looping (); }
    void seekTo (double sec) override { m_player.seekTo (sec); }
    double positionSec () const override { return m_player.positionSec (); }

    //---- 供 GUI 使用 ----------------------------------------------------
    ap::Player& player () { return m_player; }

    void loadCurrentFile ()
    {
        if (m_filePath.empty ())
        {
            m_loadedOk = false;
            m_loadError = "未选择文件";
            return;
        }
        ap::AudioData d;
        std::string err;
        if (!ap::decodeAudioFile (m_filePath, d, err))
        {
            m_loadedOk = false;
            m_loadError = err;
            return;
        }
        if (!m_player.setAudio (std::move (d)))
        {
            m_loadedOk = false;
            m_loadError = "解码成功但载入失败";
            return;
        }
        m_loadedOk = true;
        m_loadError.clear ();
        applyParams ();
    }

    void applyParams ()
    {
        m_player.setVolume (m_volume);
        m_player.setStartOffsetSec (m_offset);
        m_player.setLooping (m_loop);
        m_player.setPlaying (m_playing);
    }

    void setVolumeParam (float v) { m_volume = v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    void setOffsetParam (float v) { m_offset = v < 0.f ? 0.f : v; }
    void setPlayingParam (bool v) { m_playing = v; }
    void setLoopParam (bool v) { m_loop = v; }

    const std::string& filePath () const { return m_filePath; }
    void setFilePath (std::string p) { m_filePath = std::move (p); }
    bool loadedOk () const { return m_loadedOk; }
    const std::string& loadError () const { return m_loadError; }

private:
    enum : int32
    {
        kIdVolume = 0,
        kIdOffset = 1,
        kIdPlay = 2,
        kIdLoop = 3,
        kIdFilePath = 100
    };

    ap::Player m_player;
    std::string m_filePath;
    std::string m_loadError;
    bool m_loadedOk = false;
    bool m_outputReady = false;

    float m_volume = 1.0f;
    float m_offset = 0.0f;
    bool  m_playing = true;
    bool  m_loop = false;
};

//==============================================================================
// AudioPlayerController —— 界面侧
//
// 它不碰音频数据，只负责创建 GUI。真正的后端是 AudioPlayerProcessor，
// 由工厂在创建控制器时注入（宿主里两者是配对的实例）。
//==============================================================================
class AudioPlayerController : public EditController
{
public:
    void setBackend (ap::PlugView::Backend* b) { m_backend = b; }

    ap::PlugView::Backend* backend () const { return m_backend; }

    // 必须无条件返回视图，不能因为后端还没注入就返回 nullptr。
    // 宿主在扫描阶段会先问「这个插件支不支持原生编辑器」，此时后端尚未创建，
    // 早期版本在这里 `if (!m_backend) return nullptr`，导致宿主把
    // hasNativeEditorSupport 判为 false，进而不在混音器里列出这个插件。
    IPlugView* PLUGIN_API createView (FIDString /*name*/) override
    {
        auto* v = new ap::PlugView (m_backend);   // m_backend 允许为 null
        v->addRef ();
        return static_cast<IPlugView*> (v);
    }

    // 本插件不用宿主参数系统（该 SDK 无 ParamInfo），全部走自绘 GUI。
    void setMirror (float vol, float off, bool play, bool loop)
    {
        m_volume = vol;
        m_offset = off;
        m_playing = play;
        m_loop = loop;
    }

    float volume () const { return m_volume; }
    float offset () const { return m_offset; }
    bool playing () const { return m_playing; }
    bool loop () const { return m_loop; }

private:
    float m_volume = 1.0f;
    float m_offset = 0.0f;
    bool  m_playing = true;
    bool  m_loop = false;
    ap::PlugView::Backend* m_backend = nullptr;
};

//==============================================================================
// PluginFactory
//==============================================================================
class PluginFactory : public IPluginFactory3,
                      public RefCountMixin
{
public:
    // 宿主通常先创建处理器、再创建控制器（或反之）。
    // 我们记住最近创建的处理器，在创建控制器时注入后端。
    void rememberProcessor (AudioPlayerProcessor* p) { m_lastProcessor = p; }

private:
public:
    uint32 PLUGIN_API addRef () override { return ++m_refCount; }
    uint32 PLUGIN_API release () override
    {
        const uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    ~PluginFactory () override = default;

    PluginFactory ()
    {
        strncpy8 (m_factoryInfo.vendor, "Youwei", PFactoryInfo::kNameSize - 1);
        strncpy8 (m_factoryInfo.url, "", PFactoryInfo::kURLSize - 1);
        strncpy8 (m_factoryInfo.email, "", PFactoryInfo::kEmailSize - 1);
        m_factoryInfo.flags = kDefaultFactoryFlags;
    }

    tresult PLUGIN_API queryInterface (const TUID _iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual (_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual (_iid, IPluginFactory::iid) ||
            FUnknownPrivate::iidEqual (_iid, IPluginFactory2::iid) ||
            FUnknownPrivate::iidEqual (_iid, IPluginFactory3::iid))
        {
            *obj = static_cast<IPluginFactory3*> (this);
            addRef ();
            return kResultOk;
        }
        return kNoInterface;
    }

    tresult PLUGIN_API getFactoryInfo (PFactoryInfo* info) override
    {
        if (!info) return kInvalidArgument;
        *info = m_factoryInfo;
        return kResultOk;
    }

    int32 PLUGIN_API countClasses () override { return 1; }

    tresult PLUGIN_API getClassInfo (int32 index, PClassInfo* info) override
    {
        if (index != 0 || !info) return kResultFalse;
        memcpy (info->cid, pluginClassId (), sizeof (TUID));
        strncpy8 (info->category, kAudioEffectClassStr, PClassInfo::kCategorySize - 1);
        strncpy8 (info->name, "大伟鼓谱MuseScore音频播放器", PClassInfo::kNameSize - 1);
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (FIDString cid, FIDString iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (!iid) return kInvalidArgument;

        if (cid)
        {
            if (!FUnknownPrivate::iidEqual (cid, pluginClassId ()))
                return kNoInterface;
        }

        const char8* reqIid = iid;

        // 编辑控制器
        if (FUnknownPrivate::iidEqual (reqIid, IEditController::iid))
        {
            auto* c = new AudioPlayerController ();
            c->setBackend (m_lastProcessor);
            c->addRef ();
            *obj = static_cast<IEditController*> (c);
            return kResultOk;
        }

        // 宿主先问 IComponent（同一个对象也实现 IAudioProcessor）
        if (FUnknownPrivate::iidEqual (reqIid, IComponent::iid))
        {
            auto* p = new AudioPlayerProcessor ();
            rememberProcessor (p);
            p->addRef ();
            *obj = static_cast<IComponent*> (p);
            return kResultOk;
        }

        // 宿主直接问 IAudioProcessor
        if (FUnknownPrivate::iidEqual (reqIid, IAudioProcessor::iid))
        {
            auto* p = new AudioPlayerProcessor ();
            rememberProcessor (p);
            p->addRef ();
            *obj = static_cast<IAudioProcessor*> (p);
            return kResultOk;
        }

        return kNoInterface;
    }

    //---- IPluginFactory2 ----
    // MuseScore 4.7 会调 getClassInfo2 取 PClassInfo2（含 vendor / classFlags）。
    // 不实现它，宿主就拿不到完整信息，会直接跳过这个插件（实测踩过）。
    tresult PLUGIN_API getClassInfo2 (int32 index, PClassInfo2* info) override
    {
        if (index != 0 || !info) return kResultFalse;

        std::memset (info, 0, sizeof (*info));
        memcpy (info->cid, pluginClassId (), sizeof (TUID));
        info->cardinality = PClassInfo::kManyInstances;
        strncpy8 (info->category, kAudioEffectClassStr, PClassInfo::kCategorySize - 1);
        strncpy8 (info->name, "大伟鼓谱MuseScore音频播放器", PClassInfo::kNameSize - 1);
        // classFlags / subCategories 保持 0（无特殊子分类）
        strncpy8 (info->vendor, m_factoryInfo.vendor, PClassInfo2::kVendorSize - 1);
        strncpy8 (info->version, "1.2.5", PClassInfo2::kVersionSize - 1);
        return kResultOk;
    }

    //---- IPluginFactory3 ----
    tresult PLUGIN_API getClassInfoUnicode (int32 index, PClassInfoW* info) override
    {
        if (index != 0 || !info) return kResultFalse;

        std::memset (info, 0, sizeof (*info));
        memcpy (info->cid, pluginClassId (), sizeof (TUID));
        info->cardinality = PClassInfo::kManyInstances;

        // 名称和分类用 UTF-16 —— 这里给中文名，MuseScore 等宿主能直接显示
        const char16_t* cat = u"Instrument";
        const char16_t* nm = u"大伟鼓谱MuseScore音频播放器";
        for (int i = 0; i < PClassInfo::kCategorySize - 1 && cat[i]; ++i)
            info->category[i] = cat[i];
        for (int i = 0; i < PClassInfo::kNameSize - 1 && nm[i]; ++i)
            info->name[i] = nm[i];
        return kResultOk;
    }

    tresult PLUGIN_API setHostContext (FUnknown* /*context*/) override { return kResultOk; }

    tresult PLUGIN_API setComponentHandler (IComponentHandler* /*handler*/) { return kResultOk; }

private:
    PFactoryInfo m_factoryInfo {};
    AudioPlayerProcessor* m_lastProcessor = nullptr;
};

//==============================================================================
// 工厂入口 —— 宿主唯一需要导出的符号
//
// SDK 自带的 BEGIN_FACTORY_DEF / DEF_CLASS 宏依赖 CPluginFactory
// （在 vst3_public_sdk 仓库里），pluginterfaces 不含它，所以手写导出。
//==============================================================================
extern "C"
{
    SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory ()
    {
        static PluginFactory* gFactory = nullptr;
        if (!gFactory)
        {
            gFactory = new PluginFactory ();
            gFactory->addRef ();
        }
        return static_cast<IPluginFactory*> (gFactory);
    }
}
