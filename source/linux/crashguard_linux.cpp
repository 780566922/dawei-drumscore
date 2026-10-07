//==============================================================================
// crashguard_linux.cpp — 崩溃取证与阶段日志（Linux / signal + backtrace）
//
// 对应 macOS 的 crashguard.mm 与 Windows 的 crashguard_win.cpp。
// 对外符号保持一致：
//   ap::installCrashGuard()   —— 装信号处理器
//   ap::crashLog(fmt, ...)    —— 阶段日志
//
// 与两个兄弟版的对应关系：
//   signalHandler(sigaction)  ↔  SetUnhandledExceptionFilter（SEH）
//   backtrace_symbols_fd      ↔  CaptureStackBackTrace + DbgHelp
//   日志 ~/.local/share/...   ↔  %USERPROFILE%\DaweiDrumScore.log
//
// 日志位置：$HOME/DaweiDrumScore.log（与 Windows 版对称，一层目录好找）
//
// 约束：信号处理器里只能调 async-signal-safe 的东西（write /
//   backtrace_symbols_fd），所以下面的写日志全走裸 write。
//
// 提示：Release 构建若未带 -rdynamic，栈帧多为「模块(+0x偏移)」形式；
//   配合同一份构建产物的 nm/addr2line 仍可定位。
//==============================================================================
#include "crashguard.h"

#include <csignal>
#include <signal.h>     // sigaction / SA_SIGINFO 的全局声明
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

// ucontext_t / mcontext_t：取出崩溃时的 PC 与寄存器（Linux 各架构字段不同）
#include <ucontext.h>

namespace ap {

namespace {

int g_logFd = -1;

//------------------------------------------------------------------------------
const char* logPath ()
{
    static char path[512] = {0};
    if (path[0] == '\0')
    {
        const char* home = ::getenv ("HOME");
        if (home && *home)
            std::snprintf (path, sizeof path, "%s/DaweiDrumScore.log", home);
        else
            std::snprintf (path, sizeof path, "/tmp/DaweiDrumScore.log");
    }
    return path;
}

// 信号上下文安全：单次 write，O_APPEND 下天然原子
void writeFd (const char* s, size_t n)
{
    const int fd = (g_logFd >= 0) ? g_logFd : STDERR_FILENO;
    while (n > 0)
    {
        const ssize_t w = ::write (fd, s, n);
        if (w <= 0)
            break;
        s += w;
        n -= static_cast<size_t> (w);
    }
}

void writeStr (const char* s) { writeFd (s, std::strlen (s)); }

//------------------------------------------------------------------------------
// 信号处理器：段错误 / 总线错误 / abort 都在这里落地
//
// 用 SA_SIGINFO 拿 si_addr（真正出错的地址）与 ucontext（PC / 寄存器），
// 比只有调用栈更容易一眼看出「是不是访问了已释放对象」。
//------------------------------------------------------------------------------
void signalHandler (int sig, siginfo_t* info, void* uap)
{
    const char* name = "未知信号";
    switch (sig)
    {
        case SIGSEGV: name = "SIGSEGV 非法内存访问（野指针 / 越界）"; break;
        case SIGABRT: name = "SIGABRT abort（多为未捕获异常或断言失败）"; break;
        case SIGBUS:  name = "SIGBUS 总线错误（多与内存映射有关）"; break;
        case SIGILL:  name = "SIGILL 非法指令"; break;
        case SIGFPE:  name = "SIGFPE 算术错误"; break;
        default: break;
    }

    char hdr[256];
    const int n = std::snprintf (hdr, sizeof hdr,
                                 "\n=== 大伟鼓谱 崩溃: %s (signal %d) ===\n",
                                 name, sig);
    if (n > 0)
        writeFd (hdr, static_cast<size_t> (n) < sizeof hdr ? static_cast<size_t> (n)
                                                          : sizeof hdr - 1);

    // ---- 故障现场：出错地址 + 关键寄存器 ----
    char regs[640];
    int m = 0;
#if defined(__x86_64__)
    if (uap)
    {
        const greg_t* g = static_cast<const ucontext_t*> (uap)->uc_mcontext.gregs;
        m = std::snprintf (regs, sizeof regs,
            "故障地址: %p\nRIP=%p  RSP=%p  RBP=%p\nRDI=%p  RSI=%p\n",
            info ? info->si_addr : nullptr,
            reinterpret_cast<const void*> (static_cast<uintptr_t> (g[REG_RIP])),
            reinterpret_cast<const void*> (static_cast<uintptr_t> (g[REG_RSP])),
            reinterpret_cast<const void*> (static_cast<uintptr_t> (g[REG_RBP])),
            reinterpret_cast<const void*> (static_cast<uintptr_t> (g[REG_RDI])),
            reinterpret_cast<const void*> (static_cast<uintptr_t> (g[REG_RSI])));
    }
#elif defined(__aarch64__)
    if (uap)
    {
        const auto* mc = &static_cast<const ucontext_t*> (uap)->uc_mcontext;
        m = std::snprintf (regs, sizeof regs,
            "故障地址: %p\nPC=%p  SP=%p\nx0=%p  x1=%p  x29=%p  x30=%p\n",
            info ? info->si_addr : nullptr,
            reinterpret_cast<const void*> (static_cast<uintptr_t> (mc->pc)),
            reinterpret_cast<const void*> (static_cast<uintptr_t> (mc->sp)),
            reinterpret_cast<const void*> (static_cast<uintptr_t> (mc->regs[0])),
            reinterpret_cast<const void*> (static_cast<uintptr_t> (mc->regs[1])),
            reinterpret_cast<const void*> (static_cast<uintptr_t> (mc->regs[29])),
            reinterpret_cast<const void*> (static_cast<uintptr_t> (mc->regs[30])));
    }
#endif
    if (m > 0)
        writeFd (regs, static_cast<size_t> (m) < sizeof regs ? static_cast<size_t> (m)
                                                            : sizeof regs - 1);

    writeStr ("调用栈:\n");
    void* frames[96];
    const int cnt = ::backtrace (frames, 96);
    ::backtrace_symbols_fd (frames, cnt, g_logFd >= 0 ? g_logFd : STDERR_FILENO);
    writeStr ("=== 现场记录结束 ===\n");

    // 恢复默认处理并重发，保持系统原有的崩溃行为
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    ::sigaction (sig, &dfl, nullptr);
    ::raise (sig);
}

//------------------------------------------------------------------------------
// C++ 未捕获异常 → std::terminate
//------------------------------------------------------------------------------
void terminateHandler ()
{
    writeStr ("\n=== 大伟鼓谱 崩溃: std::terminate（未捕获的 C++ 异常）===\n调用栈:\n");
    void* frames[96];
    const int cnt = ::backtrace (frames, 96);
    ::backtrace_symbols_fd (frames, cnt, g_logFd >= 0 ? g_logFd : STDERR_FILENO);
    writeStr ("=== 现场记录结束 ===\n");
    ::abort ();
}

} // namespace

//------------------------------------------------------------------------------
void installCrashGuard ()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    const char* p = logPath ();

    // 简单轮转：超过 2MB 就重开，避免无界增长
    struct stat st {};
    if (::stat (p, &st) == 0 && st.st_size > 2 * 1024 * 1024)
        ::unlink (p);

    g_logFd = ::open (p, O_WRONLY | O_CREAT | O_APPEND, 0644);

    std::set_terminate (&terminateHandler);

    const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };

    // SA_SIGINFO 才能拿到 si_addr 与 ucontext（寄存器现场）
    struct sigaction sa {};
    sa.sa_sigaction = &signalHandler;
    sa.sa_flags = SA_SIGINFO;   // sa_mask 已零初始化 = 空集
    for (int s : sigs)
        ::sigaction (s, &sa, nullptr);

    crashLog ("--- 大伟鼓谱 插件已加载，崩溃取证已就绪（日志: %s）---", p);
}

//------------------------------------------------------------------------------
void crashLog (const char* fmt, ...)
{
    if (g_logFd < 0)
        return;

    char body[1024];
    va_list ap;
    va_start (ap, fmt);
    std::vsnprintf (body, sizeof body, fmt, ap);
    va_end (ap);

    struct timeval tv {};
    ::gettimeofday (&tv, nullptr);
    struct tm tmv {};
    ::localtime_r (&tv.tv_sec, &tmv);

    char line[1200];
    const int n = std::snprintf (line, sizeof line, "[%02d:%02d:%02d.%03d] %s\n",
                                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                                 static_cast<int> (tv.tv_usec / 1000), body);
    if (n > 0)
        writeFd (line, static_cast<size_t> (n) < sizeof line ? static_cast<size_t> (n)
                                                            : sizeof line - 1);
}

} // namespace ap
