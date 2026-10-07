//==============================================================================
// crashguard_win.cpp — 崩溃取证与阶段日志（Windows / SEH + DbgHelp）
//
// 对应 macOS 的 crashguard.mm。对外符号保持一致：
//   ap::installCrashGuard()   —— 装未处理异常过滤器
//   ap::crashLog(fmt, ...)    —— 阶段日志
//
// 与 macOS 版的对应关系：
//   signalHandler(sigaction)  ↔  SetUnhandledExceptionFilter（SEH）
//   backtrace_symbols_fd      ↔  CaptureStackBackTrace + SymFromAddr(DbgHelp)
//   日志 ~/Library/Logs/...   ↔  %USERPROFILE%\DaweiDrumScore.log
//
// 说明：Release 构建通常不带 PDB，符号解析可能只给出模块名 + 偏移，
//   地址列表本身已足够配合 map 文件定位；有 PDB 时会显示函数名。
//==============================================================================
#include "crashguard.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace ap {

namespace {

HANDLE g_logFile = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
volatile LONG g_lockReady = 0;

//------------------------------------------------------------------------------
// 日志路径：%USERPROFILE%\DaweiDrumScore.log
// 不用 SHGetFolderPath 之类：崩溃路径要尽量少依赖其它子系统。
//------------------------------------------------------------------------------
const char* logPath ()
{
    static char path[MAX_PATH * 2] = {0};
    if (path[0] == '\0')
    {
        char home[MAX_PATH] = {0};
        const DWORD n = ::GetEnvironmentVariableA ("USERPROFILE", home, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            std::snprintf (path, sizeof path, "%s\\DaweiDrumScore.log", home);
        else
            std::snprintf (path, sizeof path, "C:\\DaweiDrumScore.log");
    }
    return path;
}

// 崩溃路径下的写：直接走 Win32 文件 API，避免 CRT 状态问题
void writeRaw (const char* s, size_t n)
{
    if (n == 0)
        return;
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        ::WriteFile (g_logFile, s, static_cast<DWORD> (n), &written, nullptr);
    }
    else
    {
        ::OutputDebugStringA (s);
    }
}

//------------------------------------------------------------------------------
// 栈回溯：CaptureStackBackTrace 拿地址，DbgHelp 尽力解析符号
//------------------------------------------------------------------------------
void writeStackTrace ()
{
    HANDLE proc = ::GetCurrentProcess ();

    // 未初始化符号表时先初始化（失败也无所谓，只是没有函数名）
    static bool symInit = false;
    if (!symInit)
    {
        ::SymSetOptions (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        symInit = ::SymInitialize (proc, nullptr, TRUE) == TRUE;
    }

    void* frames[96] = {0};
    const USHORT cnt = ::CaptureStackBackTrace (0, 96, frames, nullptr);

    char line[1024];
    for (USHORT i = 0; i < cnt; ++i)
    {
        DWORD64 addr = reinterpret_cast<DWORD64> (frames[i]);

        char symName[512] = {0};
        DWORD64 disp = 0;
        bool haveName = false;

        if (symInit)
        {
            // SYMBOL_INFO 变长结构，缓冲里给名字留足空间
            char buf[sizeof (SYMBOL_INFO) + 512] = {0};
            SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*> (buf);
            si->SizeOfStruct = sizeof (SYMBOL_INFO);
            si->MaxNameLen = 511;
            if (::SymFromAddr (proc, addr, &disp, si))
            {
                std::snprintf (symName, sizeof symName, "%s", si->Name);
                haveName = true;
            }
        }

        const int k = haveName
            ? std::snprintf (line, sizeof line, "  #%02u  %p  %s+0x%llx\n",
                             i, frames[i], symName,
                             static_cast<unsigned long long> (disp))
            : std::snprintf (line, sizeof line, "  #%02u  %p\n", i, frames[i]);
        if (k > 0)
            writeRaw (line, static_cast<size_t> (k) < sizeof line ? static_cast<size_t> (k)
                                                                  : sizeof line - 1);
    }
}

//------------------------------------------------------------------------------
// 未处理异常过滤器：段错误 / 非法访问 / 除零等都在这里落地
//------------------------------------------------------------------------------
LONG WINAPI unhandledFilter (EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;

    const char* name = "未知异常";
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:          name = "非法内存访问（野指针 / 越界）"; break;
        case EXCEPTION_STACK_OVERFLOW:            name = "栈溢出（递归过深）"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:        name = "整数除零"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:       name = "非法指令"; break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:     name = "数组越界"; break;
        case EXCEPTION_PRIV_INSTRUCTION:          name = "特权指令"; break;
        case EXCEPTION_IN_PAGE_ERROR:             name = "内存页错误"; break;
        default: break;
    }

    char hdr[512];
    int n = std::snprintf (hdr, sizeof hdr,
                           "\n=== 大伟鼓谱 崩溃: %s (code 0x%08lX) ===\n故障地址: %p\n调用栈:\n",
                           name, static_cast<unsigned long> (code), addr);
    if (n > 0)
        writeRaw (hdr, static_cast<size_t> (n) < sizeof hdr ? static_cast<size_t> (n)
                                                           : sizeof hdr - 1);

    writeStackTrace ();
    writeRaw ("=== 现场记录结束 ===\n", 24);

    // 交给系统默认处理，保持原有的崩溃上报行为
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

//------------------------------------------------------------------------------
void installCrashGuard ()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    ::InitializeCriticalSection (&g_lock);
    ::InterlockedExchange (&g_lockReady, 1);

    const char* p = logPath ();

    // 简单轮转：超 2MB 就重开
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (::GetFileAttributesExA (p, GetFileExInfoStandard, &fad))
    {
        const ULONGLONG size = (static_cast<ULONGLONG> (fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (size > 2ull * 1024 * 1024)
            ::DeleteFileA (p);
    }

    g_logFile = ::CreateFileA (p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile != INVALID_HANDLE_VALUE)
        ::SetFilePointer (g_logFile, 0, nullptr, FILE_END);

    ::SetUnhandledExceptionFilter (&unhandledFilter);

    crashLog ("--- 大伟鼓谱 插件已加载，崩溃取证已就绪（日志: %s）---", p);
}

//------------------------------------------------------------------------------
void crashLog (const char* fmt, ...)
{
    if (g_logFile == INVALID_HANDLE_VALUE)
        return;

    char body[1024];
    va_list ap;
    va_start (ap, fmt);
    std::vsnprintf (body, sizeof body, fmt, ap);
    va_end (ap);

    ::time_t t = ::time (nullptr);
    ::tm tmv = {};
    ::localtime_s (&tmv, &t);

    char line[1200];
    const int n = std::snprintf (line, sizeof line, "[%02d:%02d:%02d] %s\n",
                                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, body);
    if (n <= 0)
        return;

    const size_t len = static_cast<size_t> (n) < sizeof line ? static_cast<size_t> (n)
                                                             : sizeof line - 1;
    if (g_lockReady)
        ::EnterCriticalSection (&g_lock);
    writeRaw (line, len);
    if (g_lockReady)
        ::LeaveCriticalSection (&g_lock);
}

} // namespace ap
