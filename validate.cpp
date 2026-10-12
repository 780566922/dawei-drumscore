//==============================================================================
// validate.cpp — 插件验证器
//
// 目的：在装进 MuseScore 之前，先用一个最小宿主实际加载插件，
//走完 VST3 的完整生命周期。这样能在本机发现问题，而不是让用户
// 装上去才发现崩溃。
//
// 覆盖：工厂查询 → 创建处理器 → 初始化 → 总线声明 → setupProcessing
//       → 载入真实音频 → 渲染音频块 → 保存/恢复状态 → 释放
//==============================================================================
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"

// 三端共用的小节推进算法（ap::buildBars）就在这个头里。验证器只用到它的纯函数，
// 不碰任何平台 GUI —— 这样「同一份算法」是被【真的那一份】测到的，不是副本。
#include "gui.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Vst;

static int g_pass = 0, g_fail = 0;

static void check (const char* name, bool cond, const std::string& detail = "")
{
    if (cond)
    {
        std::printf ("  [PASS] %s\n", name);
        ++g_pass;
    }
    else
    {
        std::printf ("  [FAIL] %s%s%s\n", name, detail.empty () ? "" : " -> ", detail.c_str ());
        ++g_fail;
    }
}

static std::string S (tresult r)
{
    if (r == kResultOk) return "kResultOk";
    if (r == kResultFalse) return "kResultFalse";
    if (r == kNoInterface) return "kNoInterface";
    if (r == kInvalidArgument) return "kInvalidArgument";
    if (r == kNotImplemented) return "kNotImplemented";
    char b[32];
    std::snprintf (b, sizeof b, "0x%08x", static_cast<unsigned> (r));
    return b;
}

//------------------------------------------------------------------------------
// 内存流：实现 IBStream，用来测状态保存/恢复
//------------------------------------------------------------------------------
class MemStream : public IBStream
{
public:
    std::vector<uint8> buf;
    size_t pos = 0;
    bool reading = false;

    MemStream () { buf.reserve (4096); }

    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }
    tresult PLUGIN_API queryInterface (const TUID _iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual (_iid, FUnknown::iid))
        {
            *obj = static_cast<IBStream*> (this);
            return kResultOk;
        }
        return kNoInterface;
    }

    tresult PLUGIN_API read (void* buffer, int32 numBytes, int32* numBytesRead = nullptr) override
    {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        const size_t avail = (pos < buf.size ()) ? (buf.size () - pos) : 0;
        const size_t n = (static_cast<size_t> (numBytes) < avail)
                             ? static_cast<size_t> (numBytes) : avail;
        if (n) std::memcpy (buffer, buf.data () + pos, n);
        pos += n;
        if (numBytesRead) *numBytesRead = static_cast<int32> (n);
        return (n == static_cast<size_t> (numBytes)) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API write (void* buffer, int32 numBytes, int32* numBytesWritten = nullptr) override
    {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        const uint8* p = static_cast<const uint8*> (buffer);
        if (pos < buf.size ())
        {
            for (int32 i = 0; i < numBytes; ++i)
            {
                if (pos < buf.size ()) buf[pos] = p[i]; else buf.push_back (p[i]);
                ++pos;
            }
        }
        else
        {
            buf.insert (buf.end (), p, p + numBytes);
            pos = buf.size ();
        }
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }

    int64 PLUGIN_API getSize () { return static_cast<int64> (buf.size ()); }

    tresult PLUGIN_API seek (int64 pos, int32 mode, int64* result = nullptr) override
    {
        int64 np = 0;
        if (mode == kIBSeekSet) np = pos;
        else if (mode == kIBSeekCur) np = static_cast<int64> (this->pos) + pos;
        else if (mode == kIBSeekEnd) np = static_cast<int64> (buf.size ()) + pos;
        if (np < 0) np = 0;
        this->pos = static_cast<size_t> (np);
        if (result) *result = np;
        return kResultOk;
    }

    tresult PLUGIN_API tell (int64* p) override
    {
        if (!p) return kInvalidArgument;
        *p = static_cast<int64> (pos);
        return kResultOk;
    }
};

//------------------------------------------------------------------------------
int main (int argc, char** argv)
{
    const char* bundlePath = (argc > 1) ? argv[1] : "./build/大伟鼓谱MuseScore音频播放器.vst3";

    std::printf ("\n===== 大伟鼓谱MuseScore音频播放器 验证 =====\n");
    std::printf ("加载: %s\n", bundlePath);

    //---- 0. 共用的小节推进算法 ----
    // ap::buildBars()（见 gui.h）是三端网格线与拍号条共用的同一份代码。谱子里一旦
    // 有拍号变化（前 8 小节 4/4、之后转 3/4），每小节长度就不同了 —— 用「全曲统一
    // barSec」铺线，变化点之后的小节线会全部错位。这里把它的语义逐条钉死：
    // 起点 = 起始偏移、逐小节按【当时生效】的拍号累加、变化点标记、向前延伸、
    // 视野外的小节也必须继续累加、非法值回落。
    std::printf ("\n[0] 共用算法：逐小节推进（ap::buildBars）\n");
    {
        using ap::TimeSigChange;
        std::vector<ap::BarMark> bars;
        const double kEps = 1e-9;

        // ① 无变化：4/4 @120BPM（四分音符 0.5 秒）→ 每拍 0.5 秒、每小节 2 秒
        ap::buildBars (0.0, 8.0, 2.0, 0.5, 4, 4, {}, bars);
        {
            bool ok = (bars.size () == 5);
            for (size_t i = 0; ok && i < bars.size (); ++i)
                ok = (bars[i].bar == static_cast<int> (i))
                  && std::fabs (bars[i].startSec - 2.0 * static_cast<double> (i)) < kEps
                  && !bars[i].changed;
            check ("buildBars：起点 = 起始偏移；逐小节累加；第 1 小节之前向前铺（0 号小节）",
                   ok, "n=" + std::to_string (bars.size ()));
        }

        // ② 第 3 小节转 3/4：前两小节 2 秒，之后每小节 1.5 秒
        {
            std::vector<TimeSigChange> ch = { { 3, 3, 4 } };
            ap::buildBars (0.0, 10.0, 2.0, 0.5, 4, 4, ch, bars);
            const ap::BarMark* b3 = nullptr;
            const ap::BarMark* b4 = nullptr;
            for (const ap::BarMark& m : bars)
            {
                if (m.bar == 3) b3 = &m;
                if (m.bar == 4) b4 = &m;
            }
            check ("buildBars：变化点之后每小节长度跟着变（3/4 → 1.5 秒）",
                   b3 && b4
                       && std::fabs (b3->startSec - 6.0) < kEps
                       && std::fabs (b4->startSec - 7.5) < kEps
                       && b3->beats == 3 && b3->denom == 4
                       && std::fabs (b3->beatSec - 0.5) < kEps);
            check ("buildBars：只有变化点那一小节标记 changed",
                   b3 && b3->changed && b4 && !b4->changed);
        }

        // ③ 第 2 小节转 6/8：每拍（八分音符）= 0.5 × 4/8 = 0.25 秒，每小节 6 拍 = 1.5 秒
        {
            std::vector<TimeSigChange> ch = { { 2, 6, 8 } };
            ap::buildBars (0.0, 8.0, 2.0, 0.5, 4, 4, ch, bars);
            bool ok = false;
            for (const ap::BarMark& m : bars)
                if (m.bar == 2)
                    ok = (m.beats == 6 && m.denom == 8)
                      && std::fabs (m.beatSec - 0.25) < kEps
                      && std::fabs (m.startSec - 4.0) < kEps;
            bool ok3 = false;
            for (const ap::BarMark& m : bars)
                if (m.bar == 3)
                    ok3 = std::fabs (m.startSec - 5.5) < kEps;   // 4.0 + 6 × 0.25
            check ("buildBars：6/8 的拍长 = 四分音符 × 4/8（0.25 秒）", ok);
            check ("buildBars：6/8 小节长度 1.5 秒（下一小节起点 5.5）", ok3);
        }

        // ④ 多段串联：4/4 → 3/4（第 3 小节）→ 5/4（第 6 小节）
        {
            std::vector<TimeSigChange> ch = { { 3, 3, 4 }, { 6, 5, 4 } };
            ap::buildBars (0.0, 14.0, 2.0, 0.5, 4, 4, ch, bars);
            const double expect[7] = { 2.0, 4.0, 6.0, 7.5, 9.0, 10.5, 13.0 };
            const int    beats[7]  = { 4, 4, 3, 3, 3, 5, 5 };
            bool ok = true;
            for (int i = 0; i < 7 && ok; ++i)
            {
                const ap::BarMark* m = nullptr;
                for (const ap::BarMark& x : bars)
                    if (x.bar == i + 1) m = &x;
                ok = m && std::fabs (m->startSec - expect[i]) < kEps && m->beats == beats[i];
            }
            check ("buildBars：多段变化串联（起点与拍号逐小节都对）", ok);
        }

        // ⑤ 视野里只看得到后面的小节时，前面【看不见的小节也必须继续累加】
        //    （否则第 6 小节的起点会算成「偏移 + 5 × 2 秒」而不是真实值）
        {
            std::vector<TimeSigChange> ch = { { 3, 3, 4 } };
            ap::buildBars (10.0, 11.5, 2.0, 0.5, 4, 4, ch, bars);
            check ("buildBars：视野外的小节继续累加（第 6 小节起点 = 10.5，而非 12.0）",
                   bars.size () == 1 && bars[0].bar == 6
                       && std::fabs (bars[0].startSec - 10.5) < kEps
                       && bars[0].beats == 3,
                   "n=" + std::to_string (bars.size ()));
        }

        // ⑥ 非法值回落默认（分子 0 / 分母 7），但变化点本身仍然成立
        {
            std::vector<TimeSigChange> ch = { { 3, 0, 7 } };
            ap::buildBars (0.0, 8.0, 0.0, 0.5, 4, 4, ch, bars);
            bool ok = false;
            for (const ap::BarMark& m : bars)
                if (m.bar == 3)
                    ok = (m.beats == 4 && m.denom == 4) && m.changed;
            check ("buildBars：非法拍号回落 4/4（变化点仍成立）", ok);
        }

        // ⑦ 退化输入一律安全（空结果，不是崩）
        {
            ap::buildBars (0.0, 8.0, 0.0, 0.0, 4, 4, {}, bars);
            const bool empty1 = bars.empty ();
            ap::buildBars (8.0, 8.0, 0.0, 0.5, 4, 4, {}, bars);
            check ("buildBars：退化输入（四分音符时长 0 / 空区间）→ 空结果且不崩",
                   empty1 && bars.empty ());
        }
    }

    //---- 1. dlopen ----
    std::printf ("\n[1] 动态库加载\n");
    void* lib = dlopen (bundlePath, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        std::printf ("  [FAIL] dlopen 失败: %s\n", dlerror ());
        return 1;
    }
    check ("dlopen 成功", true);

    auto getFactory = reinterpret_cast<IPluginFactory* (*) ()> (dlsym (lib, "GetPluginFactory"));
    if (!getFactory)
    {
        std::printf ("  [FAIL] 找不到 GetPluginFactory 符号\n");
        return 1;
    }
    check ("GetPluginFactory 符号存在", true);

    IPluginFactory* factory = getFactory ();
    check ("工厂指针非空", factory != nullptr);
    if (!factory) return 1;

    //---- 1b. 无处理器实例时的后端安全性 ----
    // 视图在宿主扫描阶段就处于「音频引擎还没建好」的状态。以前这里给视图
    // 塞的是处理器裸指针（寄存器里是 0 或已释放地址），宿主一交互就闪退。
    // 现在视图拿的是永生代理：无处理器时所有调用都要安全返回。
    std::printf ("\n[1b] 无处理器实例时的后端安全性\n");
    auto probeBackend = reinterpret_cast<int (*) ()> (dlsym (lib, "aplayStableBackendProbe"));
    auto liveCount = reinterpret_cast<int (*) ()> (dlsym (lib, "aplayLiveProcessorCount"));
    if (probeBackend)
        check ("无处理器时，代理后端所有调用安全（查询/修改/载入均不崩）",
               probeBackend () == 1);
    else
        std::printf ("  [INFO] 未找到 aplayStableBackendProbe，跳过\n");

    //---- 2. 工厂信息 ----
    std::printf ("\n[2] 工厂与类信息\n");
    PFactoryInfo fi {};
    check ("getFactoryInfo", factory->getFactoryInfo (&fi) == kResultOk, S (factory->getFactoryInfo (&fi)));
    check ("厂商名非空", fi.vendor[0] != 0, fi.vendor);

    const int32 nClasses = factory->countClasses ();
    check ("至少 1 个类", nClasses >= 1, std::to_string (nClasses));

    PClassInfo ci {};
    check ("getClassInfo(0)", factory->getClassInfo (0, &ci) == kResultOk);
    // 插件显示名用中文品牌：实测 MuseScore 读插件名的通道能正确解码 UTF-8，
    // 显示正常不乱码。（乱码的是它上面一级的「厂商名」，见下一条断言。）
    check ("类名 = 大伟鼓谱MuseScore音频播放器（插件显示名，中文）",
           std::string (ci.name) == "大伟鼓谱MuseScore音频播放器", ci.name);
    // category 必须是 "Audio Module Class"（kVstAudioEffectClass）。
    // MuseScore 硬编码匹配这个字符串来判断「是否含音频效果」，
    // 写成别的会被判为 "[1406] VST3 file contains no audio effect"。
    check ("分类 = Audio Module Class（宿主硬编码匹配）",
           std::string (ci.category) == "Audio Module Class", ci.category);

    // 控制器类的 category 必须是 "Component Controller Class"
    if (factory->countClasses () > 1)
    {
        PClassInfo cci2 {};
        factory->getClassInfo (1, &cci2);
        check ("控制器分类 = Component Controller Class",
               std::string (cci2.category) == "Component Controller Class", cci2.category);
    }

    //---- 工厂接口 2 / 3：MuseScore 4.7 会用，缺了插件会被静默跳过 ----
    std::printf ("\n[2b] 工厂接口 2 / 3\n");
    IPluginFactory2* f2 = nullptr;
    check ("支持 IPluginFactory2",
           factory->queryInterface (IPluginFactory2::iid, (void**) &f2) == kResultOk && f2);
    if (f2)
    {
        PClassInfo2 ci2 {};
        check ("getClassInfo2(0)", f2->getClassInfo2 (0, &ci2) == kResultOk);
        check ("PClassInfo2 分类正确",
               std::string (ci2.category) == "Audio Module Class", ci2.category);
        check ("PClassInfo2 厂商非空", ci2.vendor[0] != 0, ci2.vendor);
        {
            // 厂商名是插件名「上面一级」的菜单分组名，中文在那条通道会乱码。
            // 必须纯 ASCII —— 这条防回归断言拦住「有人把厂商名改回中文」。
            bool vendorAscii = true;
            for (const char* p = ci2.vendor; *p; ++p)
                if ((unsigned char) *p >= 0x80u) { vendorAscii = false; break; }
            check ("厂商名仅含 ASCII 字节（防菜单乱码）", vendorAscii);
        }
        check ("PClassInfo2 版本非空", ci2.version[0] != 0, ci2.version);
        check ("cardinality = kManyInstances",
               ci2.cardinality == PClassInfo::kManyInstances);
        check ("cid 与 getClassInfo 一致",
               std::memcmp (ci2.cid, ci.cid, sizeof (TUID)) == 0);
    }

    IPluginFactory3* f3 = nullptr;
    check ("支持 IPluginFactory3",
           factory->queryInterface (IPluginFactory3::iid, (void**) &f3) == kResultOk && f3);
    if (f3)
    {
        PClassInfoW ciw {};
        check ("getClassInfoUnicode(0)", f3->getClassInfoUnicode (0, &ciw) == kResultOk);
        check ("Unicode 名称非空", ciw.name[0] != 0);
        // Unicode 通道才是 MuseScore 真正用的（混音器菜单/列表都取这条）。
        // SDK 基类的 PClassInfoW::fromAscii 只做逐字节拓宽、不解码 UTF-8，
        // 中文会变乱码。这里逐字符断言等于预期 UTF-16，防止再次退化。
        {
            const char16_t* expect = u"大伟鼓谱MuseScore音频播放器";
            bool unameOk = true;
            for (int i = 0; i < PClassInfo::kNameSize; ++i)
            {
                const char16_t got = (char16_t) ciw.name[i];
                if (got != expect[i]) { unameOk = false; break; }
                if (expect[i] == 0) break;
            }
            check ("Unicode 类名 = 大伟鼓谱MuseScore音频播放器（真 UTF-16，非乱码）", unameOk);
        }
        // setHostContext 是可选接口，宿主不要求插件实现它
        f3->setHostContext (nullptr);
        check ("setHostContext 可调用（可选）", true);
    }

    // TUID 是 char[16] 数组类型，不能按值拷贝，必须用引用
    const TUID& cid = ci.cid;

    //---- 3. 创建处理器 ----
    std::printf ("\n[3] 创建音频处理器\n");
    IComponent* comp = nullptr;
    check ("createInstance(IComponent)",
           factory->createInstance (cid, IComponent::iid, (void**) &comp) == kResultOk && comp);

    IAudioProcessor* proc = nullptr;
    const tresult qi = comp->queryInterface (IAudioProcessor::iid, (void**) &proc);
    check ("处理器支持 IAudioProcessor", qi == kResultOk && proc, S (qi));

    if (liveCount)
        check ("创建后活跃处理器实例数 = 1", liveCount () == 1,
               std::to_string (liveCount ()));

    check ("initialize", comp->initialize (nullptr) == kResultOk);

    //---- 4. 总线 ----
    std::printf ("\n[4] 总线声明\n");
    const int32 nOut = comp->getBusCount (kAudio, kOutput);
    check ("1 个音频输出总线", nOut == 1, std::to_string (nOut));
    check ("0 个音频输入总线", comp->getBusCount (kAudio, kInput) == 0);

    BusInfo bus {};
    check ("getBusInfo(输出0)", comp->getBusInfo (kAudio, kOutput, 0, bus) == kResultOk);
    check ("总线类型 = kMain", bus.busType == kMain);
    check ("声道数 = 2", bus.channelCount == 2, std::to_string (bus.channelCount));
    check ("方向 = kOutput", bus.direction == kOutput);

    check ("activateBus(输出0, true)", comp->activateBus (kAudio, kOutput, 0, true) == kResultOk);
    check ("setActive(true)", comp->setActive (true) == kResultOk);

    //---- 5. 音频处理设置 ----
    std::printf ("\n[5] 音频处理配置\n");
    check ("canProcessSampleSize(32bit)",
           proc->canProcessSampleSize (kSample32) == kResultOk);

    // 官方基类要求：先 setActive(true)，再 setupProcessing
    comp->setActive (true);

    ProcessSetup setup {};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 44100.0;
    {
        const tresult r = proc->setupProcessing (setup);
        check ("setupProcessing(44.1k)", r == kResultOk || r == kNotImplemented, S (r));
    }

    setup.sampleRate = 48000.0;   // 换个采样率，验证重采样路径
    {
        const tresult r = proc->setupProcessing (setup);
        check ("setupProcessing(48k) 切换", r == kResultOk || r == kNotImplemented, S (r));
    }
    setup.sampleRate = 44100.0;
    proc->setupProcessing (setup);

    check ("getLatencySamples", proc->getLatencySamples () == 0);
    check ("getTailSamples", proc->getTailSamples () == 0);
    check ("setProcessing(true)", proc->setProcessing (true) == kResultOk);

    //---- 6. 载入真实音频并渲染 ----
    std::printf ("\n[6] 载入音频并渲染\n");
    check ("载入测试音频", comp->setState (nullptr) == kResultOk);

    // 无音频时应输出静音且不崩
    {
        const int32 N = 256;
        std::vector<float> L (N, 0.f), R (N, 0.f);
        AudioBusBuffers out {};
        out.numChannels = 2;
        Sample32* bufs[2] = {nullptr, nullptr};
        bufs[0] = L.data ();
        bufs[1] = R.data ();
        out.channelBuffers32 = bufs;
        out.silenceFlags = 0;

        ProcessData pd {};
        pd.processMode = kRealtime;
        pd.symbolicSampleSize = kSample32;
        pd.numSamples = N;
        pd.numOutputs = 1;
        pd.outputs = &out;

        const tresult r = proc->process (pd);
        check ("空音频 process 返回 kResultOk", r == kResultOk, S (r));
        check ("空音频输出静音", L[0] == 0.f);
    }

    // 宿主标记静音时也应安全
    {
        const int32 N = 128;
        std::vector<float> L (N, 1.f), R (N, 1.f);
        AudioBusBuffers out {};
        out.numChannels = 2;
        Sample32* bufs[2] = {nullptr, nullptr};
        bufs[0] = L.data ();
        bufs[1] = R.data ();
        out.channelBuffers32 = bufs;
        out.silenceFlags = 3;   // 两声道都静音

        ProcessData pd {};
        pd.processMode = kRealtime;
        pd.symbolicSampleSize = kSample32;
        pd.numSamples = N;
        pd.numOutputs = 1;
        pd.outputs = &out;

        check ("静音标记 process 安全", proc->process (pd) == kResultOk);
        check ("静音标记时缓冲区被清零", L[0] == 0.f);
    }

    // 无输出总线的极端情况（宿主异常或关闭插件）
    {
        ProcessData pd {};
        pd.processMode = kRealtime;
        pd.symbolicSampleSize = kSample32;
        pd.numSamples = 128;
        pd.numOutputs = 0;
        pd.outputs = nullptr;
        check ("无输出总线时安全返回", proc->process (pd) == kResultOk);
    }

    // 64 位路径
    {
        const int32 N = 64;
        std::vector<double> L (N, 0.0), R (N, 0.0);
        AudioBusBuffers out {};
        out.numChannels = 2;
        Sample64* bufs[2] = {nullptr, nullptr};
        bufs[0] = L.data ();
        bufs[1] = R.data ();
        out.channelBuffers64 = bufs;
        out.silenceFlags = 0;

        ProcessData pd {};
        pd.processMode = kRealtime;
        pd.symbolicSampleSize = kSample64;
        pd.numSamples = N;
        pd.numOutputs = 1;
        pd.outputs = &out;

        check ("64 位 process 安全", proc->process (pd) == kResultOk);
    }

    //---- 7. 状态保存/恢复 ----
    std::printf ("\n[7] 状态保存与恢复\n");
    {
        MemStream st;
        check ("getState", comp->getState (&st) == kResultOk);
        check ("状态数据非空", st.buf.size () > 0, std::to_string (st.buf.size ()));

        MemStream st2;
        st2.buf = st.buf;
        check ("setState（回读自己的数据）", comp->setState (&st2) == kResultOk);
    }
    {   // 脏数据不能崩
        MemStream bad;
        int32 junk = 0x7FFFFFFF;
        bad.buf.resize (64, 0xAB);
        std::memcpy (bad.buf.data (), &junk, sizeof junk);
        comp->setState (&bad);
        check ("脏数据不崩溃", true);
    }

    //---- 8. 控制器 ----
    std::printf ("\n[8] 编辑控制器\n");
    IEditController* ctrl = nullptr;
    // 官方工厂按「类 UID」而非组件 UID 匹配，先取第 2 个类（控制器）
    PClassInfo cci {};
    factory->getClassInfo (1, &cci);
    check ("类数量 >= 2（处理器 + 控制器）",
           factory->countClasses () >= 2, std::to_string (factory->countClasses ()));
    const tresult cr = factory->createInstance (cci.cid, IEditController::iid, (void**) &ctrl);
    check ("createInstance(IEditController)", cr == kResultOk && ctrl, S (cr));

    if (ctrl)
    {
        // 关键：宿主扫描时会先创建控制器并探测编辑器，此时音频后端还没创建。
        // createView 必须仍然返回非空，否则宿主判定 hasNativeEditorSupport=false，
        // 插件就不会出现在混音器列表里。
        IPlugView* scanView = ctrl->createView ("PLATFORM_UI");
        check ("扫描阶段 createView 返回非空（无后端时）", scanView != nullptr);
        if (scanView)
        {
            ViewRect vr {};
            scanView->getSize (&vr);
            check ("扫描阶段视图尺寸可读", vr.right == 640 && vr.bottom == 384);

            // 平台类型：macOS 宿主问的就是 "NSView"（kPlatformTypeNSView）。
            // 之前实现错匹配 "PLATFORM_UI"（规范里不存在该值），
            // MuseScore 一问就得到否定答复 → 判定无原生编辑器 → 点了不弹窗。
            check ("isPlatformTypeSupported(\"NSView\") 为真（macOS 宿主实际问这个）",
                   scanView->isPlatformTypeSupported (kPlatformTypeNSView) == kResultOk);
            check ("isPlatformTypeSupported(\"PLATFORM_UI\") 必须被拒绝（防回归）",
                   scanView->isPlatformTypeSupported ("PLATFORM_UI") != kResultOk);

            scanView->release ();
        }

        check ("控制器 initialize", ctrl->initialize (nullptr) == kResultOk);
        check ("getParameterCount = 0（不用宿主参数）", ctrl->getParameterCount () == 0);

        // 控制器也实现了 IComponent（宿主会用它激活/反初始化）
        IComponent* ctrlComp = nullptr;
        if (ctrl->queryInterface (IComponent::iid, (void**) &ctrlComp) == kResultOk && ctrlComp)
        {
            check ("控制器支持 IComponent", true);
            check ("控制器 setActive(true)", ctrlComp->setActive (true) == kResultOk);
        }

        IPlugView* view = ctrl->createView (kPlatformTypeNSView);
        if (view)
        {
            check ("createView 返回视图", true);
            ViewRect vr {};
            check ("view->getSize", view->getSize (&vr) == kResultOk);
            check ("视图尺寸 = 640x384",
                   vr.right == 640 && vr.bottom == 384,
                   std::to_string (vr.right) + "x" + std::to_string (vr.bottom));
            // 旧版这里是 kResultFalse（固定尺寸）。现在三端一致返回 kResultTrue，
            // 表示「本视图支持缩放」。⚠️ 但这只锁「我们对规范的回答」，不锁「用户能不能
            // 拖窗口」—— MuseScore 4.x 忽略它并把窗口 min/max 钉成同一个值（经验总结 13 章）。
            check ("view->canResize 返回 true（按规范声明支持缩放）",
                   view->canResize () == kResultTrue);

            //---- 缩放钳制：宽度 640~1700，高度强制回 384（本端高度不可改）----
            {
                ViewRect narrow {0, 0, 300, 200};
                view->checkSizeConstraint (&narrow);
                check ("checkSizeConstraint：过窄/过矮 → 640x384",
                       narrow.getWidth () == 640 && narrow.getHeight () == 384,
                       std::to_string (narrow.getWidth ()) + "x"
                           + std::to_string (narrow.getHeight ()));

                ViewRect wide {0, 0, 3000, 900};
                view->checkSizeConstraint (&wide);
                check ("checkSizeConstraint：超上限 → 1700x384（高度被压回）",
                       wide.getWidth () == 1700 && wide.getHeight () == 384,
                       std::to_string (wide.getWidth ()) + "x"
                           + std::to_string (wide.getHeight ()));

                ViewRect ok {0, 0, 900, 384};
                view->checkSizeConstraint (&ok);
                check ("checkSizeConstraint：范围内 → 宽度原样通过",
                       ok.getWidth () == 900 && ok.getHeight () == 384,
                       std::to_string (ok.getWidth ()) + "x" + std::to_string (ok.getHeight ()));

                // ⚠️ 宿主给的矩形可能带非零原点（窗口装饰）。钳制必须按 width/height
                //    算再写回 left/top 上，直接覆写 right/bottom 会把窗体怼偏。
                ViewRect offset {20, 10, 920, 394};
                view->checkSizeConstraint (&offset);
                check ("checkSizeConstraint：带原点偏移时按宽高算，且原点不动",
                       offset.getWidth () == 900 && offset.getHeight () == 384
                           && offset.left == 20 && offset.top == 10,
                       std::to_string (offset.left) + "," + std::to_string (offset.top) + " "
                           + std::to_string (offset.getWidth ()) + "x"
                           + std::to_string (offset.getHeight ()));
            }

            // 界面必须真的拿到音频后端，否则窗口弹出来也是空壳：点了没反应。
            auto viewHasBackend = reinterpret_cast<int (*) (IPlugView*)> (
                dlsym (lib, "aplayViewHasBackend"));
            if (viewHasBackend)
                check ("视图已绑定音频后端（界面按钮真实有效）",
                       viewHasBackend (view) == 1);
            else
                std::printf ("  [INFO] 未找到 aplayViewHasBackend 符号，跳过该检查\\n");

            //---- 拍号变化表：内存语义 + 随乐谱保存 / 恢复 ----
            // 它和 BPM 一样属于「用户操作产生、关掉界面后仍要有意义」的状态，存在
            // 处理器里、随乐谱一起存取（铁律 11）。所以两件事都要测：
            //   ① 内存语义（写入 / 同小节覆盖 / 非法回落 / 精确取消）；
            //   ② 它真的进了 getState / setState，且恢复是【整体替换】。
            {
                auto tsProbe = reinterpret_cast<int (*) (IPlugView*)> (
                    dlsym (lib, "aplayTimeSigMapProbe"));
                auto tsSet = reinterpret_cast<void (*) (IPlugView*, int, int, int)> (
                    dlsym (lib, "aplayTimeSigSet"));
                auto tsDump = reinterpret_cast<int (*) (IPlugView*, int*, int)> (
                    dlsym (lib, "aplayTimeSigDump"));

                if (tsProbe && tsSet && tsDump)
                {
                    const int pr = tsProbe (view);
                    check ("拍号变化表：写入 / 同小节覆盖 / 非法回落 / 精确取消",
                           pr == 1, "probe=" + std::to_string (pr));

                    // 存两条 → 保存；再把表改脏（覆盖一条 + 新增一条）→ 恢复
                    tsSet (view, 2, 7, 8);
                    tsSet (view, 5, 3, 4);
                    MemStream ts1;
                    check ("拍号变化表：getState 成功", comp->getState (&ts1) == kResultOk);

                    tsSet (view, 2, 2, 2);
                    tsSet (view, 9, 9, 9);
                    MemStream ts2;
                    ts2.buf = ts1.buf;
                    check ("拍号变化表：setState 成功", comp->setState (&ts2) == kResultOk);

                    int tri[12] = {0};
                    const int n = tsDump (view, tri, 4);
                    check ("拍号变化表：随乐谱保存 / 恢复（整体替换，改脏的那两条被冲掉）",
                           n == 2
                               && tri[0] == 2 && tri[1] == 7 && tri[2] == 8
                               && tri[3] == 5 && tri[4] == 3 && tri[5] == 4,
                           "n=" + std::to_string (n));
                }
                else
                {
                    std::printf ("  [INFO] 未找到拍号变化表探针符号，跳过该检查\n");
                }
            }

            view->release ();
        }
        else
        {
            // createView 不允许返回 null —— 扫描阶段返回 null 会让宿主
            // 判定 hasNativeEditorSupport=false，插件直接从列表里消失。
            check ("createView 返回视图（不允许为 null）", false);
        }
    }

    //---- 9. 释放 ----
    std::printf ("\n[9] 释放\n");
    proc->setProcessing (false);
    check ("处理器 setProcessing(false)", true);
    check ("comp->setActive(false)", comp->setActive (false) == kResultOk);
    check ("comp->terminate", comp->terminate () == kResultOk);

    if (ctrl)
    {
        ctrl->terminate ();
        ctrl->release ();
    }
    comp->release ();

    //---- 10. 处理器销毁之后 ----
    // 宿主真实会走的顺序：先关音频引擎（销毁处理器），编辑器视图和它的
    // 20Hz 刷新定时器可能还活着。以前这种时序就是野指针闪退。
    std::printf ("\n[10] 处理器销毁后的后端安全性\n");
    if (proc)
    {
        proc->release ();    // 引用计数归零 → 处理器析构并自动注销
        proc = nullptr;
    }
    if (liveCount)
        check ("处理器已销毁：活跃实例归零", liveCount () == 0,
               std::to_string (liveCount ()));
    if (probeBackend)
        check ("处理器已销毁：代理后端调用仍然安全", probeBackend () == 1);

    factory->release ();
    dlclose (lib);
    check ("无崩溃完成释放", true);

    std::printf ("\n============================================\n");
    std::printf ("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf ("============================================\n\n");
    return g_fail > 0 ? 1 : 0;
}
