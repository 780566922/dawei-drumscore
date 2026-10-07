//==============================================================================
// crashguard.h — 崩溃取证与阶段日志
//
// 为什么需要它：
//   插件崩在宿主进程里时，MuseScore 自己的日志会在崩溃前一句戛然而止，
//   系统也可能什么都不落盘。没有现场就没法定位，只能一轮轮猜。
//   这里装上异常/信号处理器，把最后一刻的现场写进一个固定文件。
//
// 日志位置：~/Library/Logs/DaweiDrumScore.log
//==============================================================================
#pragma once

// MSVC 不认 __attribute__((format))，这里做个兼容宏。
// 该属性只影响编译期格式串检查，去掉不影响功能。
#if defined(__GNUC__) || defined(__clang__)
#  define AP_PRINTF_LIKE(fmtIdx, argIdx) __attribute__((format(printf, fmtIdx, argIdx)))
#else
#  define AP_PRINTF_LIKE(fmtIdx, argIdx)
#endif

namespace ap {

// 安装处理器。可重复调用，只会生效一次。在 bundleEntry 里调用。
void installCrashGuard ();

// 记一笔阶段日志（带时间戳）。用于事后确认"最后走到了哪一步"。
void crashLog (const char* fmt, ...) AP_PRINTF_LIKE (1, 2);

} // namespace ap
