//==============================================================================
// e2e_test.cpp — 端到端音频测试
//
// 目的：验证插件真的能出声，而且起始偏移正确。
// 做法：把测试音频路径写进插件状态 → 渲染音频块 → 检查输出样本。
//
// 用法：./e2e_test <插件路径> <音频文件>
//==============================================================================
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Vst;

static int g_pass = 0, g_fail = 0;
static void check (const char* name, bool cond, const std::string& d = "")
{
    if (cond) { std::printf ("  [PASS] %s\n", name); ++g_pass; }
    else { std::printf ("  [FAIL] %s%s%s\n", name, d.empty () ? "" : " -> ", d.c_str ()); ++g_fail; }
}

//------------------------------------------------------------------------------
class MemStream : public IBStream
{
public:
    std::vector<uint8> buf;
    size_t pos = 0;
    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }
    tresult PLUGIN_API queryInterface (const TUID _iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual (_iid, FUnknown::iid)) { *obj = static_cast<IBStream*> (this); return kResultOk; }
        return kNoInterface;
    }
    tresult PLUGIN_API read (void* b, int32 n, int32* got = nullptr) override
    {
        if (!b || n < 0) return kInvalidArgument;
        const size_t avail = (pos < buf.size ()) ? buf.size () - pos : 0;
        const size_t k = (static_cast<size_t> (n) < avail) ? static_cast<size_t> (n) : avail;
        if (k) std::memcpy (b, buf.data () + pos, k);
        pos += k;
        if (got) *got = static_cast<int32> (k);
        return (k == static_cast<size_t> (n)) ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API write (void* b, int32 n, int32* put = nullptr) override
    {
        if (!b || n < 0) return kInvalidArgument;
        const uint8* p = static_cast<const uint8*> (b);
        buf.insert (buf.end (), p, p + n);
        if (put) *put = n;
        return kResultOk;
    }
    int64 PLUGIN_API getSize () { return static_cast<int64> (buf.size ()); }
    tresult PLUGIN_API seek (int64 p, int32 mode, int64* res = nullptr) override
    {
        int64 np = 0;
        if (mode == kIBSeekSet) np = p;
        else if (mode == kIBSeekCur) np = static_cast<int64> (pos) + p;
        else if (mode == kIBSeekEnd) np = static_cast<int64> (buf.size ()) + p;
        if (np < 0) np = 0;
        pos = static_cast<size_t> (np);
        if (res) *res = np;
        return kResultOk;
    }
    tresult PLUGIN_API tell (int64* p) override { if (!p) return kInvalidArgument; *p = static_cast<int64> (pos); return kResultOk; }
};

//------------------------------------------------------------------------------
// 渲染 nFrames 帧，返回峰值
//------------------------------------------------------------------------------
static float renderBlock (IAudioProcessor* proc, int32 nFrames, double sr,
                          std::vector<float>& outL, std::vector<float>& outR)
{
    outL.assign (static_cast<size_t> (nFrames), 0.f);
    outR.assign (static_cast<size_t> (nFrames), 0.f);

    Sample32* bufs[2] = {nullptr, nullptr};
    bufs[0] = outL.data ();
    bufs[1] = outR.data ();

    AudioBusBuffers bus {};
    bus.numChannels = 2;
    bus.channelBuffers32 = bufs;
    bus.silenceFlags = 0;

    ProcessData pd {};
    pd.processMode = kRealtime;
    pd.symbolicSampleSize = kSample32;
    pd.numSamples = nFrames;
    pd.numOutputs = 1;
    pd.outputs = &bus;

    proc->process (pd);

    float peak = 0.f;
    for (size_t i = 0; i < outL.size (); ++i)
    {
        const float a = std::fabs (outL[i]);
        const float b = std::fabs (outR[i]);
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    return peak;
}

//------------------------------------------------------------------------------
int main (int argc, char** argv)
{
    const char* binPath = (argc > 1) ? argv[1] : nullptr;
    const char* audioPath = (argc > 2) ? argv[2] : nullptr;
    if (!binPath || !audioPath)
    {
        std::printf ("用法: %s <插件二进制> <音频文件>\n", argv[0]);
        return 2;
    }

    std::printf ("\n============== 端到端音频测试 ==============\n");
    std::printf ("插件: %s\n音频: %s\n", binPath, audioPath);

    void* lib = dlopen (binPath, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf ("  [FAIL] dlopen: %s\n", dlerror ()); return 1; }

    auto getFactory = reinterpret_cast<IPluginFactory* (*) ()> (dlsym (lib, "GetPluginFactory"));
    if (!getFactory) { std::printf ("  [FAIL] 无工厂符号\n"); return 1; }
    IPluginFactory* factory = getFactory ();

    PClassInfo ci {};
    if (factory->getClassInfo (0, &ci) != kResultOk) { std::printf ("  [FAIL] 无类信息\n"); return 1; }

    IComponent* comp = nullptr;
    factory->createInstance (ci.cid, IComponent::iid, (void**) &comp);
    IAudioProcessor* proc = nullptr;
    comp->queryInterface (IAudioProcessor::iid, (void**) &proc);
    if (!proc) { std::printf ("  [FAIL] 无 IAudioProcessor\n"); return 1; }

    comp->initialize (nullptr);
    comp->activateBus (kAudio, kOutput, 0, true);
    comp->setActive (true);

    ProcessSetup setup {};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 44100.0;
    proc->setupProcessing (setup);
    proc->setProcessing (true);

    //---- 载入音频：写一个只含文件路径的状态流 ----
    //    格式：[int32 id=100][int32 size][路径字节]
    std::printf ("\n[1] 载入音频文件\n");
    {
        MemStream st;
        int32 id = 100;
        int32 len = static_cast<int32> (std::strlen (audioPath));
        st.write (&id, sizeof id);
        st.write (&len, sizeof len);
        st.write (const_cast<char*> (audioPath), len);
        comp->setState (&st);
    }

    // 多渲几块，确认真的出声
    std::printf ("\n[2] 渲染音频（应有输出）\n");
    {
        std::vector<float> L, R;
        float peak = 0.f;
        for (int i = 0; i < 20; ++i)
        {
            const float p = renderBlock (proc, 512, 44100.0, L, R);
            if (p > peak) peak = p;
        }
        check ("渲染 20 块后有音频输出", peak > 0.01f,
               "峰值 " + std::to_string (peak));
        std::printf ("  峰值 = %.4f\n", peak);
        check ("峰值在合理范围（未削波）", peak <= 1.0f, "峰值 " + std::to_string (peak));
    }

    //---- 偏移功能 ----
    std::printf ("\n[3] 起始偏移（核心需求）\n");
    {
        // 用状态流设置偏移 = 4 秒
        auto setOffset = [&] (float sec)
        {
            MemStream st;
            int32 idOff = 1;
            int32 sz = static_cast<int32> (sizeof (float));
            st.write (&idOff, sizeof idOff);
            st.write (&sz, sizeof sz);
            float secCopy = sec;
            st.write (&secCopy, sizeof secCopy);
            comp->setState (&st);
        };

        // 先重置为 0，确认有声
        setOffset (0.0f);
        std::vector<float> L, R;
        renderBlock (proc, 512, 44100.0, L, R);
        float p0 = 0.f;
        for (size_t i = 0; i < L.size (); ++i)
            p0 = std::max (p0, std::max (std::fabs (L[i]), std::fabs (R[i])));

        // 偏移到 4 秒，仍应有声（测试音频是 8 秒正弦）
        setOffset (4.0f);
        float p4 = 0.f;
        for (int i = 0; i < 10; ++i)
        {
            renderBlock (proc, 512, 44100.0, L, R);
            for (size_t k = 0; k < L.size (); ++k)
                p4 = std::max (p4, std::max (std::fabs (L[k]), std::fabs (R[k])));
        }
        check ("偏移 4 秒后仍有音频", p4 > 0.01f, "峰值 " + std::to_string (p4));

        // 偏移超出音频长度（8 秒）→ 应静音而非崩溃
        setOffset (100.0f);
        renderBlock (proc, 512, 44100.0, L, R);
        check ("偏移超出范围不崩溃", true);
    }

    //---- 采样率切换（重采样路径）----
    std::printf ("\n[4] 48kHz 宿主（重采样）\n");
    {
        // 先把起始偏移重置为 0。上一段故意把偏移设成了 100 秒（超出音频长度），
        // 而载入新文件后会按当前偏移重新定位 —— 那段值留着会让本段没声音。
        // 这里要单独验证 48k 重采样，必须先隔离掉偏移这个变量。
        {
            MemStream st;
            int32 idOff = 1;
            int32 sz = static_cast<int32> (sizeof (float));
            st.write (&idOff, sizeof idOff);
            st.write (&sz, sizeof sz);
            float zero = 0.0f;
            st.write (&zero, sizeof zero);
            comp->setState (&st);
        }

        // 前面已把 8 秒测试音频播完，这里先重新载入再测 48k
        {
            MemStream st;
            int32 id = 100;
            int32 len = static_cast<int32> (std::strlen (audioPath));
            st.write (&id, sizeof id);
            st.write (&len, sizeof len);
            st.write (const_cast<char*> (audioPath), len);
            comp->setState (&st);
        }

        setup.sampleRate = 48000.0;
        proc->setupProcessing (setup);

        std::vector<float> L, R;
        float peak = 0.f;
        for (int i = 0; i < 10; ++i)
        {
            const float p = renderBlock (proc, 512, 48000.0, L, R);
            if (p > peak) peak = p;
        }
        check ("48kHz 下正常输出", peak > 0.01f, "峰值 " + std::to_string (peak));
    }

    //---- 不存在的文件 ----
    std::printf ("\n[5] 错误处理\n");
    {
        MemStream st;
        const char* bad = "/nonexistent/path/to/audio.mp3";
        int32 id = 100;
        int32 len = static_cast<int32> (std::strlen (bad));
        st.write (&id, sizeof id);
        st.write (&len, sizeof len);
        st.write (const_cast<char*> (bad), len);
        const tresult r = comp->setState (&st);
        check ("不存在的文件不崩溃", r == kResultOk || r == kResultFalse);
    }

    //---- 负偏移：谱面先走，音频整体后移 ----
    // 这条专门验证「负数不再被钳成 0」—— 旧实现在 setState 里把负值吃掉了，
    // 界面上拖到负半边也不会有任何效果。
    std::printf ("\n[6] 负偏移（带符号偏移）\n");
    {
        auto setParam = [&] (int32 id, const void* data, int32 size)
        {
            MemStream st;
            st.write (&id, sizeof id);
            st.write (&size, sizeof size);
            if (size > 0) st.write (const_cast<void*> (data), size);
            comp->setState (&st);
        };
        auto loadAudio = [&]
        {
            MemStream st;
            int32 id = 100;
            int32 len = static_cast<int32> (std::strlen (audioPath));
            st.write (&id, sizeof id);
            st.write (&len, sizeof len);
            st.write (const_cast<char*> (audioPath), len);
            comp->setState (&st);
        };

        float zero = 0.0f;
        setParam (1, &zero, sizeof zero);
        loadAudio ();                       // 重新载入，让读位置从头开始

        float neg = -2.0f;                  // 偏移 -2 秒
        setParam (1, &neg, sizeof neg);

        std::vector<float> L, R;
        float peakEarly = 0.f;
        for (int i = 0; i < 129; ++i)       // 129 × 512 / 44100 ≈ 1.497 秒
        {
            const float p = renderBlock (proc, 512, 44100.0, L, R);
            if (p > peakEarly) peakEarly = p;
        }
        check ("负偏移 -2 秒：前 1.5 秒全静音", peakEarly < 1e-6f,
               "峰值 " + std::to_string (peakEarly));

        float peakLate = 0.f;
        for (int i = 0; i < 60; ++i)        // 再 0.7 秒（跨过 2 秒静音边界）
        {
            const float p = renderBlock (proc, 512, 44100.0, L, R);
            if (p > peakLate) peakLate = p;
        }
        check ("负偏移：静音结束后开始出声", peakLate > 0.01f,
               "峰值 " + std::to_string (peakLate));

        float pos = 3.0f;
        setParam (1, &pos, sizeof pos);
        float pp = 0.f;
        for (int i = 0; i < 5; ++i)
        {
            const float p = renderBlock (proc, 512, 44100.0, L, R);
            if (p > pp) pp = p;
        }
        check ("切回正偏移 3 秒仍有声", pp > 0.01f, "峰值 " + std::to_string (pp));
    }

    proc->setProcessing (false);
    comp->setActive (false);
    comp->terminate ();
    comp->release ();
    factory->release ();
    dlclose (lib);

    std::printf ("\n============================================\n");
    std::printf ("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf ("============================================\n\n");
    return g_fail > 0 ? 1 : 0;
}
