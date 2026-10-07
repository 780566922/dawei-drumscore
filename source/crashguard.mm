//==============================================================================
// crashguard.mm — 崩溃取证与阶段日志
//
// 为什么需要它：
//   插件崩在宿主进程里时，MuseScore 自己的日志会在崩溃前一句戛然而止，
//   系统也可能什么都不落盘。没有现场就只能一轮轮猜。
//   这里装上异常/信号处理器，把最后一刻的现场写进一个固定文件。
//
// 日志位置：~/Library/Logs/DaweiDrumScore.log
//
// 约束：信号处理器里只能调用 async-signal-safe 的东西
//   （write / backtrace_symbols_fd），所以下面的写日志全走裸 write。
//==============================================================================
#include "crashguard.h"

#import <Foundation/Foundation.h>

#include <csignal>
#include <signal.h>   // sigemptyset / sigaction 的全局声明（<csignal> 只保证 std:: 一份）
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
#include <sys/ucontext.h>   // ucontext_t / mcontext_t：取出崩溃时的 PC 与寄存器
#include <unistd.h>

namespace ap {

namespace {

int g_logFd = -1;

const char* logPath ()
{
    static char path[512] = {0};
    if (path[0] == '\0')
    {
        // 不用 NSHomeDirectory()：bundleEntry 时 Foundation 未必已完全就绪。
        const char* home = ::getenv ("HOME");
        if (home && *home)
            std::snprintf (path, sizeof path, "%s/Library/Logs/DaweiDrumScore.log", home);
        else
            std::snprintf (path, sizeof path, "/tmp/DaweiDrumScore.log");
    }
    return path;
}

// 信号上下文安全：单次 write，天然原子（O_APPEND + 长度远小于 PIPE_BUF）
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
// 用 sigaction + SA_SIGINFO 而不是 signal()：
//   siginfo 里有 si_addr（真正出错的地址），ucontext 里有 PC 和寄存器。
//   上一个版本只记调用栈，结果定位到具体哪条指令还得手工反汇编倒推；
//   有了这两样，一眼就能判断「是不是给已释放对象发消息」。
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
#if defined(__arm64__)
    if (uap)
    {
        const auto& ss = static_cast<const ucontext_t*> (uap)->uc_mcontext->__ss;
        m = std::snprintf (regs, sizeof regs,
            "故障地址: %p\nPC=%p  LR=%p  SP=%p  FP=%p\n"
            "x0=%p  x1=%p  x2=%p  x3=%p\n"
            "x19=%p  x20=%p  x26=%p\n",
            info ? info->si_addr : nullptr,
            reinterpret_cast<const void*> (ss.__pc), reinterpret_cast<const void*> (ss.__lr),
            reinterpret_cast<const void*> (ss.__sp), reinterpret_cast<const void*> (ss.__fp),
            reinterpret_cast<const void*> (ss.__x[0]), reinterpret_cast<const void*> (ss.__x[1]),
            reinterpret_cast<const void*> (ss.__x[2]), reinterpret_cast<const void*> (ss.__x[3]),
            reinterpret_cast<const void*> (ss.__x[19]), reinterpret_cast<const void*> (ss.__x[20]),
            reinterpret_cast<const void*> (ss.__x[26]));
    }
#elif defined(__x86_64__)
    if (uap)
    {
        const auto& ss = static_cast<const ucontext_t*> (uap)->uc_mcontext->__ss;
        m = std::snprintf (regs, sizeof regs,
            "故障地址: %p\nRIP=%p  RSP=%p  RBP=%p\nRDI=%p  RSI=%p\n",
            info ? info->si_addr : nullptr,
            reinterpret_cast<const void*> (ss.__rip), reinterpret_cast<const void*> (ss.__rsp),
            reinterpret_cast<const void*> (ss.__rbp),
            reinterpret_cast<const void*> (ss.__rdi), reinterpret_cast<const void*> (ss.__rsi));
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

    // 恢复默认处理并重发：保持系统原有的崩溃行为，避免吞掉系统报告
    // 注：不调 sigemptyset —— macOS 上它是宏（(*(set)=0,0)），而且 {} 零初始化
    //     的 sigset_t（Darwin 下是 uint32 位图）本来就等于空集。
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    ::sigaction (sig, &dfl, nullptr);
    ::raise (sig);
}

//------------------------------------------------------------------------------
// ObjC 未捕获异常（不在信号上下文，可以宽松一些）
//------------------------------------------------------------------------------
void uncaughtExceptionHandler (NSException* e)
{
    char buf[2048];
    const int n = std::snprintf (buf, sizeof buf,
                                 "\n=== 大伟鼓谱 崩溃: ObjC 未捕获异常 ===\n"
                                 "名称: %s\n原因: %s\n调用栈:\n",
                                 e.name ? e.name.UTF8String : "(无)",
                                 e.reason ? e.reason.UTF8String : "(无)");
    if (n > 0)
        writeFd (buf, static_cast<size_t> (n) < sizeof buf ? static_cast<size_t> (n)
                                                          : sizeof buf - 1);

    void* frames[96];
    const int cnt = ::backtrace (frames, 96);
    ::backtrace_symbols_fd (frames, cnt, g_logFd >= 0 ? g_logFd : STDERR_FILENO);
    writeStr ("=== 现场记录结束 ===\n");
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
    NSSetUncaughtExceptionHandler (&uncaughtExceptionHandler);

    const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };

    // SA_SIGINFO 才能拿到 si_addr 与 ucontext（寄存器现场）
    struct sigaction sa {};
    sa.sa_sigaction = &signalHandler;
    sa.sa_flags = SA_SIGINFO;   // sa_mask 已零初始化 = 空集，无需 sigemptyset
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
