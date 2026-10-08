//==============================================================================
// factory_lifecycle_test.cpp — 工厂引用计数回归测试
//
// 背景（真实用户事故）：
//   在 MuseScore 里「关掉谱子 → 导入另一个工程 → 打开插件」宿主闪退。
//   插件自己的崩溃日志显示，崩溃栈整个落在宿主自己的 VST3 模块加载路径上：
//     VstModulesRepository::addPluginModule
//       → VstPluginMetaReader::readMeta
//         → VST3::Hosting::Module::create
//
//   根因：
//   宿主每次打开插件编辑器都会重读一遍模块元数据，也就是每次都会走
//   GetPluginFactory() + release() 这一对。而我们的实现只在【首次】addRef()，
//   之后再索取就不补引用了。于是引用计数被宿主的 release 一路耗尽：
//
//     第 1 次索取：new 时 refCount=1，我们 addRef → 2；宿主 release → 1
//     第 2 次索取：不再 addRef，仍是 1；            宿主 release → 0 → delete this
//     第 3 次索取：函数内 static 仍非空 → 返回【已释放的内存】
//
//   SDK 官方宏 END_FACTORY 写的是 `else gPluginFactory->addRef();`，正是为此。
//   而且 SDK 的 ~CPluginFactory 会把它自己的全局 gPluginFactory 置空来兜底，
//   却管不到我们自己的函数内 static —— 于是那个指针永久悬垂。
//
// 本测试模拟宿主行为：同一进程内反复「索取 → 使用 → 释放」工厂。
//   修复前：第 3 轮左右进程崩溃（SIGSEGV）
//   修复后：任意轮数都稳定
//==============================================================================
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"

#include <dlfcn.h>

#include <cstdio>
#include <string>

using namespace Steinberg;

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

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("用法: factory_lifecycle_test <bundle 内可执行文件路径>\n");
        return 2;
    }

    void* lib = ::dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        std::printf ("  [FAIL] dlopen 失败: %s\n", ::dlerror ());
        return 2;
    }

    // 宿主加载模块时会先调 bundleEntry（装崩溃取证）
    auto entry = reinterpret_cast<bool (*) (void*)> (::dlsym (lib, "bundleEntry"));
    check ("导出 bundleEntry", entry != nullptr);
    if (entry)
        entry (nullptr);

    auto getFactory =
        reinterpret_cast<IPluginFactory* (*) ()> (::dlsym (lib, "GetPluginFactory"));
    check ("导出 GetPluginFactory", getFactory != nullptr);
    if (!getFactory)
        return 2;

    std::printf ("\n[1] 反复「索取 → 使用 → 释放」工厂（模拟宿主反复打开插件编辑器）\n");

    IPluginFactory* first = nullptr;
    const int kRounds = 20;
    for (int i = 0; i < kRounds; ++i)
    {
        IPluginFactory* f = getFactory ();
        if (!f)
        {
            check ("工厂指针非空", false, "第 " + std::to_string (i + 1) + " 轮返回 null");
            break;
        }
        if (i == 0)
            first = f;
        else if (f != first)
            std::printf ("  [INFO] 第 %d 轮工厂地址变了：%p → %p（可接受，但不能是野指针）\n",
                         i + 1, static_cast<void*> (first), static_cast<void*> (f));

        // 用一下：宿主读模块元数据就是这么调
        PClassInfo info {};
        const tresult r0 = f->getClassInfo (0, &info);
        const int32 n = f->countClasses ();

        // 宿主用完即释放（VST3 规范：每次索取到的引用都要配对 release）
        const uint32 left = f->release ();

        std::printf ("  第 %2d 轮: factory=%p getClassInfo=%d 类数=%d release 后计数=%u\n",
                     i + 1, static_cast<void*> (f), static_cast<int> (r0),
                     static_cast<int> (n), static_cast<unsigned> (left));
        std::fflush (stdout);

        if (r0 != kResultOk || n < 2)
        {
            check ("工厂在反复索取后仍然可用", false,
                   "第 " + std::to_string (i + 1) + " 轮已不可用");
            break;
        }
    }
    check ("反复索取 20 次，工厂始终可用（未提前销毁）", true);

    std::printf ("\n[2] 过度释放（宿主行为不可控时的兜底）\n");
    {
        IPluginFactory* f = getFactory ();
        check ("过度释放前的工厂可用", f && f->countClasses () >= 2);

        // 故意多释放几次：工厂是进程级单例，不该因此被销毁
        for (int i = 0; i < 3; ++i)
            f->release ();
        std::fflush (stdout);

        IPluginFactory* again = getFactory ();
        PClassInfo info {};
        const tresult r = again ? again->getClassInfo (0, &info) : kResultFalse;
        check ("过度释放后再次索取，工厂仍可用（没有被 delete）",
               again != nullptr && r == kResultOk);
        if (again)
            again->release ();
    }

    std::printf ("\n[3] 索取 100 次（长时间使用后的稳定性）\n");
    {
        bool ok = true;
        for (int i = 0; i < 100; ++i)
        {
            IPluginFactory* f = getFactory ();
            if (!f || f->countClasses () < 2) { ok = false; break; }
            f->release ();
        }
        check ("连续索取 100 次全部正常", ok);
    }

    std::printf ("\n结果: %d 项通过, %d 项失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
