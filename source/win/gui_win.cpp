//==============================================================================
// gui_win.cpp — 中文界面（Windows / Win32 实现 VST3 IPlugView）
//
// 对应 macOS 的 gui.mm。设计保持一致：
//   - 波形预览 + 可拖动播放头
//   - 带符号起始偏移（在波形上按住 Ctrl / Alt 拖动）
//   - 小节网格（BPM + 拍号）
//   - 播放状态灯、音量、回到播放头、使用指南
//   - 拖入音频文件 / 点按钮选文件
//
// 与宿主的关系：VST3 在 Windows 上通过 kPlatformTypeHWND 传父窗口句柄，
//   这里创建一个子窗口挂上去，控件全部是它的子窗口。
//==============================================================================
#include "gui.h"
#include "crashguard.h"   // ap::crashLog：跟随疑似失效时落一条日志

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

// Win32 的鼠标按键掩码里【没有 Alt 位】——只有 MK_CONTROL / MK_SHIFT /
// MK_LBUTTON 等，并不存在 MK_ALT（MSVC 会报 C2065 undeclared identifier）。
// 这里自定义一个不与系统冲突的位，由 modsNow() 用 GetKeyState(VK_MENU) 填充。
#define AP_MK_ALT 0x8000

// WM_DPICHANGED 在较老的 SDK / _WIN32_WINNT 下没有定义，这里兜一个。
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace {

const int kPanelW = 340;      ///< 面板宽（与 macOS 版一致）
const int kPanelH = 384;      ///< 面板高（含底部 B 站署名）
const int kWaveW  = 320;      ///< 波形区宽
const int kWaveH  = 118;      ///< 波形区高

// 分家检测阈值（秒）：正常播放时「音频实际位置」与「谱面位置 + 偏移」只差一个
// 音频缓冲的量级（几十毫秒）。超过这个值就认为跟随出了问题，波形区右上角会亮
// 出红色错位徽标，提示用户点「回到播放头」一键修复。
// 与 macOS / Linux 端取同一个值，别各写各的。
const double kDivergenceWarnSec = 0.25;

//---- 「点完『回到播放头』仍然错位」的判定窗口 --------------------------------
// 正常点一下就对齐了。若这么久之内又错位，说明偏差不是一次能纠正的，而是某个
// 跟随条件**持续**为假 —— 再点按钮也没用，徽标文案升级为「建议重启插件」，
// 并记一条日志（带冷却，避免抖动着刷屏）。
// ⚠️ 这里只做「提示升级」，绝不自动 seek：0.25 秒的偏差等于十几个音频块，
//    属结构性故障，自动 seek 会下一块又错开 → 每几十毫秒拉一次 → 音频发抖。
//    （跳变场景的自动硬 seek 在 aplaysdk.cpp 的 captureHostTimeline 里。）
// 用 GetTickCount() 的毫秒计数：DWORD 无符号相减天然抗 49.7 天回绕。
const DWORD kFollowFailWindowMs = 3000;
const DWORD kFollowFailLogGapMs  = 5000;

// 控件 ID
enum : int
{
    IDC_OPEN = 1001,
    IDC_ZOOM_OUT,
    IDC_ZOOM_IN,
    IDC_ZOOM_FIT,
    IDC_BACK_TO_SCORE,      // 按钮文案＝「回到播放头」（ID 保留原名，避免大范围改名）
    IDC_GRID_CHECK,
    IDC_NUM_CHECK,
    IDC_BPM_EDIT,
    IDC_BPM_UP,
    IDC_BPM_DOWN,
    IDC_TIMESIG_COMBO,
    IDC_VOLUME_SLIDER,
    // ⚠️ 不能叫 IDC_HELP —— winuser.h 已经把 IDC_HELP 定义成「帮助光标」的资源 ID
    //    （MAKEINTRESOURCE(32651)）。同名枚举项会被宏展开成一串语法垃圾，
    //    MSVC 只报 "syntax error: missing '}' before '('"，很难看出是宏冲突。
    IDC_HELP_BTN,
    IDC_BRAND = 1100,
    IDC_TIMER_UI = 1200
};

const wchar_t* kMainClass = L"DaweiDrumScoreMainView";
const wchar_t* kWaveClass = L"DaweiDrumScoreWaveView";
const wchar_t* kHelpClass = L"DaweiDrumScoreHelpView";   ///< 使用指南覆盖层

// 拍号选项（与 macOS 端一致）
struct TimeSig { const wchar_t* label; int beats; int denom; };
const TimeSig kTimeSigs[] = {
    { L"4/4",  4, 4 }, { L"3/4",  3, 4 }, { L"2/4",  2, 4 }, { L"5/4",  5, 4 },
    { L"6/8",  6, 8 }, { L"7/8",  7, 8 }, { L"9/8",  9, 8 }, { L"12/8", 12, 8 },
};
const int kTimeSigCount = static_cast<int> (sizeof (kTimeSigs) / sizeof (kTimeSigs[0]));

//------------------------------------------------------------------------------
std::wstring utf8ToWide (const std::string& s)
{
    if (s.empty ())
        return std::wstring ();
    const int n = ::MultiByteToWideChar (CP_UTF8, 0, s.c_str (),
                                         static_cast<int> (s.size ()), nullptr, 0);
    if (n <= 0)
        return std::wstring ();
    std::wstring w (static_cast<size_t> (n), L'\0');
    ::MultiByteToWideChar (CP_UTF8, 0, s.c_str (), static_cast<int> (s.size ()), &w[0], n);
    return w;
}

std::string wideToUtf8 (const std::wstring& w)
{
    if (w.empty ())
        return std::string ();
    const int n = ::WideCharToMultiByte (CP_UTF8, 0, w.c_str (),
                                         static_cast<int> (w.size ()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return std::string ();
    std::string s (static_cast<size_t> (n), '\0');
    ::WideCharToMultiByte (CP_UTF8, 0, w.c_str (), static_cast<int> (w.size ()),
                           &s[0], n, nullptr, nullptr);
    return s;
}

// 统一：用 UTF-8 字符串设置控件文本，避免宽/窄格式化串混用
void setTextUtf8 (HWND h, const std::string& utf8)
{
    if (!h)
        return;
    const std::wstring w = utf8ToWide (utf8);
    ::SetWindowTextW (h, w.c_str ());
}

std::string formatTime (double sec)
{
    if (sec < 0.0)
        sec = 0.0;
    const int total = static_cast<int> (sec + 0.5);
    char buf[32];
    std::snprintf (buf, sizeof buf, "%d:%02d", total / 60, total % 60);
    return std::string (buf);
}

int modsNow ()
{
    int m = 0;
    if (::GetKeyState (VK_CONTROL) < 0) m |= MK_CONTROL;
    if (::GetKeyState (VK_MENU)    < 0) m |= AP_MK_ALT;
    if (::GetKeyState (VK_SHIFT)   < 0) m |= MK_SHIFT;
    return m;
}

//------------------------------------------------------------------------------
// 高 DPI 缩放
//
// 背景：宿主进程若是「每显示器 DPI 感知」（Per-Monitor Aware），我们拿到的窗口
// 坐标就是**物理像素**。写死的 340×384 布局在 150% / 200% 缩放的屏幕上会被画成
// 物理尺寸极小的面板 —— 这就是「界面太小」的根因。
//
// 对策：按窗口实际 DPI 把整套布局（面板尺寸、控件坐标、字号、波形区）等比放大。
//
// 宿主若是 DPI 无感知（Windows 会整窗位图拉伸放大），GetDpiForWindow 返回 96
// → 缩放系数 1.0，行为与改动前完全一致。所以这条改动对老环境是**无副作用**的。
//
// 三个 API 依次降级，保证 Win7 也能跑：
//   GetDpiForWindow (Win10 1607+) → GetDpiForMonitor (Win8.1+) → GetDeviceCaps
// 全部用 GetProcAddress 动态取，不引入新的链接期依赖。
//------------------------------------------------------------------------------
double g_uiScale = 1.0;   ///< 模块级缩放系数（getSize 在 attached 之前被调用时用它）

using GetDpiForWindowFn  = UINT    (WINAPI*) (HWND);
using GetDpiForMonitorFn = HRESULT (WINAPI*) (HMONITOR, int, UINT*, UINT*);

double rawDpiScaleFor (HWND hwnd)
{
    if (!hwnd)
        hwnd = ::GetForegroundWindow ();

    if (HMODULE user32 = ::GetModuleHandleW (L"user32.dll"))
    {
        auto p = reinterpret_cast<GetDpiForWindowFn> (
            reinterpret_cast<void*> (::GetProcAddress (user32, "GetDpiForWindow")));
        if (p && hwnd)
        {
            const UINT dpi = p (hwnd);
            if (dpi >= 96)
                return static_cast<double> (dpi) / 96.0;
        }
    }

    if (HMODULE shcore = ::LoadLibraryW (L"shcore.dll"))
    {
        auto p = reinterpret_cast<GetDpiForMonitorFn> (
            reinterpret_cast<void*> (::GetProcAddress (shcore, "GetDpiForMonitor")));
        UINT dx = 0, dy = 0;
        if (p && SUCCEEDED (p (::MonitorFromWindow (hwnd, MONITOR_DEFAULTTOPRIMARY),
                               0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx >= 96)
        {
            ::FreeLibrary (shcore);
            return static_cast<double> (dx) / 96.0;
        }
        ::FreeLibrary (shcore);
    }

    if (HDC dc = ::GetDC (nullptr))
    {
        const int dpi = ::GetDeviceCaps (dc, LOGPIXELSX);
        ::ReleaseDC (nullptr, dc);
        if (dpi >= 96)
            return static_cast<double> (dpi) / 96.0;
    }
    return 1.0;
}

double uiScaleFor (HWND hwnd)
{
    double s = rawDpiScaleFor (hwnd);
    // 夹到 [1.0, 3.0]，再量化到 0.25 的档位：避免 137/96 这类零碎比例
    // 让控件落在半像素上、文字发虚、边框粗细不一。
    if (s < 1.0) s = 1.0;
    if (s > 3.0) s = 3.0;
    s = std::round (s * 4.0) / 4.0;
    return s < 1.0 ? 1.0 : s;
}

int scaledPanelW () { return static_cast<int> (std::lround (kPanelW * g_uiScale)); }
int scaledPanelH () { return static_cast<int> (std::lround (kPanelH * g_uiScale)); }

} // namespace

namespace ap {

//------------------------------------------------------------------------------
// 内部窗口对象
//------------------------------------------------------------------------------
class WinView
{
public:
    explicit WinView (PlugView::Backend* backend) : m_backend (backend) {}

    bool create (HWND parent, int w, int h);
    void destroy ();
    void resize (int w, int h);

    void paint (HDC dc, const RECT& rc);
    /// 使用指南覆盖层自绘（由覆盖层窗口的 WM_PAINT 调用）。dc 的原点即覆盖层左上角。
    void paintHelp (HDC dc);
    /// 显示/隐藏使用指南覆盖层。
    void toggleHelp ();
    /// 关闭使用指南（覆盖层被点击时回调）。
    void hideHelp ();
    bool helpVisible () const { return m_helpVisible; }
    void onMouseDown (int x, int y, int mods);
    void onMouseMove (int x, int y, int mods);
    void onMouseUp ();
    void onMouseWheel (int delta, int mods);
    void onDoubleClick ();
    void onTimer ();
    void onDropFiles (HDROP drop);
    void onCommand (int id, int notify);
    void onHScroll (HWND slider);
    HBRUSH onCtlColor (HDC dc, HWND ctl);

    /// DPI 变了（跨屏拖动、系统缩放改动）时重算缩放系数并重建字体/布局。
    void onDpiChanged ();
    /// 绑定宿主侧回调，供 onDpiChanged 请求改窗口尺寸。
    void setOwner (PlugView* o) { m_owner = o; }

    HWND hwnd () const { return m_hwnd; }

private:
    void layoutChildren ();
    void createFonts ();
    void deleteFonts ();
    void applyFontsToChildren ();
    void refreshLabels ();
    void openFileDialog ();
    void applyBpmFromEdit ();
    /// ⭐ 视图重建后从后端回读 BPM / 拍号 / 音量（见实现处的说明）
    void syncControlsFromBackend ();
    void zoomBy (double factor, double centerSec);
    void zoomFit ();
    /// 把视野挪到 sec（播放头落在画面 25% 处）。用于「回到播放头」。
    void centerViewOn (double sec);
    void clampView ();
    void afterLoad ();
    double xToSec (int x) const;
    int    secToX (double sec) const;

    PlugView::Backend* m_backend = nullptr;

    HWND m_hwnd = nullptr;
    HWND m_wave = nullptr;
    // 使用指南覆盖层：一个铺满面板的子窗口，创建顺序在所有控件之后 → 位于 z 序
    // 最上层。做成独立窗口而不是「隐藏所有控件再在父窗口上画」，是为了不碰
    // 那二十来个控件窗口的显示状态（隐藏/恢复期间容易出现焦点和重绘的边角问题）。
    HWND m_help = nullptr;
    bool m_helpVisible = false;
    HWND m_openBtn = nullptr, m_zoomOut = nullptr, m_zoomIn = nullptr, m_zoomFit = nullptr;
    HWND m_backBtn = nullptr, m_helpBtn = nullptr, m_gridCheck = nullptr, m_numCheck = nullptr;
    HWND m_bpmEdit = nullptr, m_bpmUp = nullptr, m_bpmDown = nullptr;
    HWND m_timesig = nullptr, m_volSlider = nullptr;
    HWND m_timeLabel = nullptr, m_offsetLabel = nullptr, m_srcLabel = nullptr;
    HWND m_volLabel = nullptr, m_fileLabel = nullptr, m_hintLabel = nullptr;
    HWND m_statusLamp = nullptr, m_brand = nullptr, m_fmtLabel = nullptr;

    HFONT m_font = nullptr, m_fontSmall = nullptr, m_fontBold = nullptr;
    HBRUSH m_bgBrush = nullptr;

    PlugView* m_owner = nullptr;      ///< 宿主侧回调（DPI 变化时请求改窗口尺寸）

    double m_viewStart = 0.0;   ///< 视野起点（秒）
    double m_viewSpan = 8.0;    ///< 视野跨度（秒）

    //---- 「点完『回到播放头』仍错位」检测（onTimer 维护，paint 只读）----
    DWORD m_lastBackTick = 0;          ///< 上次点「回到播放头」的时刻（毫秒）
    bool  m_followFail = false;        ///< 是否处于「点了按钮仍错位」状态
    DWORD m_followFailLoggedTick = 0;  ///< 上次为此写日志的时刻（做冷却）

    //---- 高 DPI ----
    double m_scale = 1.0;             ///< DPI 缩放系数（1.0 = 96 DPI）
    int    m_waveW = kWaveW;          ///< 缩放后的波形区宽（secToX 的基准）
    int    m_waveH = kWaveH;          ///< 缩放后的波形区高

    /// 把「96 DPI 下的设计像素」换算成当前 DPI 下的物理像素。
    int px (int designPx) const
    {
        return static_cast<int> (std::lround (static_cast<double> (designPx) * m_scale));
    }

    bool m_dragging = false;      ///< 波形区按住鼠标中
    bool m_dragOffset = false;    ///< true = 改起始偏移；false = 平移视野
    int  m_dragStartX = 0;
    double m_dragStartVal = 0.0;  ///< 拖动开始时被改的量（改偏移时 = offset，平移时 = viewStart）

    bool m_showGrid = true;
    bool m_showNumbers = true;
};

//------------------------------------------------------------------------------
// 窗口过程与类注册
//------------------------------------------------------------------------------
namespace {

LRESULT CALLBACK waveProc (HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK mainProc (HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK helpProc (HWND, UINT, WPARAM, LPARAM);

WinView* viewOf (HWND hwnd)
{
    return reinterpret_cast<WinView*> (::GetWindowLongPtrW (hwnd, GWLP_USERDATA));
}

void ensureClasses ()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    HINSTANCE hinst = ::GetModuleHandleW (nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = mainProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW (nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH> (COLOR_BTNFACE + 1);
    wc.lpszClassName = kMainClass;
    ::RegisterClassExW (&wc);

    WNDCLASSEXW wv = {};
    wv.cbSize = sizeof wv;
    wv.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wv.lpfnWndProc = waveProc;
    wv.hInstance = hinst;
    wv.hCursor = ::LoadCursorW (nullptr, IDC_ARROW);
    wv.hbrBackground = reinterpret_cast<HBRUSH> (::GetStockObject (BLACK_BRUSH));
    wv.lpszClassName = kWaveClass;
    ::RegisterClassExW (&wv);

    // 使用指南覆盖层：背景全自绘，所以不给 hbrBackground（WM_ERASEBKGND 返回 1）。
    WNDCLASSEXW wh = {};
    wh.cbSize = sizeof wh;
    wh.style = CS_HREDRAW | CS_VREDRAW;
    wh.lpfnWndProc = helpProc;
    wh.hInstance = hinst;
    wh.hCursor = ::LoadCursorW (nullptr, IDC_ARROW);
    wh.lpszClassName = kHelpClass;
    ::RegisterClassExW (&wh);
}

} // namespace

//------------------------------------------------------------------------------
void WinView::createFonts ()
{
    // 字号同样按 DPI 放大（lfHeight 取负 = 按字符高度指定，不是行距）。
    // 下限 -8，避免极小缩放时算出 0 让系统退回默认大字。
    LOGFONTW lf = {};
    lf.lfHeight = -std::max (8L, std::lround (13.0 * m_scale));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    ::wcscpy_s (lf.lfFaceName, LF_FACESIZE, L"Microsoft YaHei UI");
    m_font = ::CreateFontIndirectW (&lf);

    lf.lfHeight = -std::max (8L, std::lround (11.0 * m_scale));
    m_fontSmall = ::CreateFontIndirectW (&lf);

    lf.lfHeight = -std::max (8L, std::lround (15.0 * m_scale));
    lf.lfWeight = FW_BOLD;
    m_fontBold = ::CreateFontIndirectW (&lf);

    if (!m_font)      m_font = reinterpret_cast<HFONT> (::GetStockObject (DEFAULT_GUI_FONT));
    if (!m_fontSmall) m_fontSmall = m_font;
    if (!m_fontBold)  m_fontBold = m_font;
}

void WinView::deleteFonts ()
{
    if (m_font && m_font != ::GetStockObject (DEFAULT_GUI_FONT))
        ::DeleteObject (m_font);
    if (m_fontSmall && m_fontSmall != m_font)
        ::DeleteObject (m_fontSmall);
    if (m_fontBold && m_fontBold != m_font)
        ::DeleteObject (m_fontBold);
    m_font = m_fontSmall = m_fontBold = nullptr;
}

void WinView::applyFontsToChildren ()
{
    const HWND normal[] = { m_openBtn, m_helpBtn, m_zoomOut, m_zoomIn, m_zoomFit, m_backBtn,
                            m_gridCheck, m_numCheck, m_bpmEdit, m_bpmUp, m_bpmDown,
                            m_timesig, m_volSlider };
    for (HWND c : normal)
        if (c)
            ::SendMessageW (c, WM_SETFONT, reinterpret_cast<WPARAM> (m_font), TRUE);

    const HWND smalls[] = { m_timeLabel, m_offsetLabel, m_srcLabel, m_volLabel,
                            m_fileLabel, m_hintLabel, m_statusLamp, m_fmtLabel, m_brand };
    for (HWND c : smalls)
        if (c)
            ::SendMessageW (c, WM_SETFONT, reinterpret_cast<WPARAM> (m_fontSmall), TRUE);
}

void WinView::onDpiChanged ()
{
    if (!m_hwnd)
        return;

    const double s = uiScaleFor (m_hwnd);
    if (std::fabs (s - m_scale) < 0.01)
        return;

    m_scale  = s;
    g_uiScale = s;
    m_waveW  = px (kWaveW);
    m_waveH  = px (kWaveH);

    deleteFonts ();
    createFonts ();
    applyFontsToChildren ();
    layoutChildren ();

    ::InvalidateRect (m_hwnd, nullptr, TRUE);
    if (m_wave)
        ::InvalidateRect (m_wave, nullptr, FALSE);

    // 面板尺寸变了，请宿主重新调整窗口（宿主不理也没关系，只是面板被裁一点）。
    if (m_owner)
        m_owner->requestResizeToPreferred ();
}

//------------------------------------------------------------------------------
bool WinView::create (HWND parent, int w, int h)
{
    ensureClasses ();

    // 先定缩放系数：后面所有尺寸（窗口、控件、字号、波形区）都按它放大。
    m_scale  = uiScaleFor (parent);
    g_uiScale = m_scale;
    m_waveW  = px (kWaveW);
    m_waveH  = px (kWaveH);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS;
    ::InitCommonControlsEx (&icc);

    m_bgBrush = ::CreateSolidBrush (RGB (240, 240, 244));

    m_hwnd = ::CreateWindowExW (0, kMainClass, L"",
                                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                0, 0, w, h, parent, nullptr,
                                ::GetModuleHandleW (nullptr), this);
    if (!m_hwnd)
        return false;

    createFonts ();

    HINSTANCE hi = ::GetModuleHandleW (nullptr);
    auto mk = [&] (const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND {
        HWND c = ::CreateWindowExW (0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                    0, 0, 10, 10, m_hwnd,
                                    reinterpret_cast<HMENU> (static_cast<INT_PTR> (id)), hi, nullptr);
        if (c)
            ::SendMessageW (c, WM_SETFONT, reinterpret_cast<WPARAM> (m_font), TRUE);
        return c;
    };

    m_wave = mk (kWaveClass, L"", 0, 0);
    if (m_wave)
        ::SetWindowLongPtrW (m_wave, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (this));

    m_openBtn   = mk (L"BUTTON", L"打开音频…", BS_PUSHBUTTON, IDC_OPEN);
    // 「？帮助」：唤出使用指南覆盖层。有用户反馈不知道怎么用，说明书必须能在
    // 界面上直接点开，不能只躺在安装目录的 readme 里。
    m_helpBtn   = mk (L"BUTTON", L"？帮助", BS_PUSHBUTTON, IDC_HELP_BTN);
    m_zoomOut   = mk (L"BUTTON", L"缩小", BS_PUSHBUTTON, IDC_ZOOM_OUT);
    m_zoomIn    = mk (L"BUTTON", L"放大", BS_PUSHBUTTON, IDC_ZOOM_IN);
    m_zoomFit   = mk (L"BUTTON", L"全览", BS_PUSHBUTTON, IDC_ZOOM_FIT);
    // 原名「回到谱面」→「回到播放头」：用户想干的是「把我送回播放位置」，
    // 新名字直说结果，不需要先理解「谱面」和「音频」的关系。
    m_backBtn   = mk (L"BUTTON", L"回到播放头", BS_PUSHBUTTON, IDC_BACK_TO_SCORE);
    m_gridCheck = mk (L"BUTTON", L"网格", BS_AUTOCHECKBOX, IDC_GRID_CHECK);
    m_numCheck  = mk (L"BUTTON", L"小节号", BS_AUTOCHECKBOX, IDC_NUM_CHECK);
    ::SendMessageW (m_gridCheck, BM_SETCHECK, BST_CHECKED, 0);
    ::SendMessageW (m_numCheck, BM_SETCHECK, BST_CHECKED, 0);

    // 初值留空 = 自动；真实值在 createControls 末尾由 syncControlsFromBackend()
    // 从后端回读（关掉编辑器再打开时后端里还存着用户填的值）。
    m_bpmEdit = mk (L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER, IDC_BPM_EDIT);
    m_bpmUp   = mk (L"BUTTON", L"▲", BS_PUSHBUTTON, IDC_BPM_UP);
    m_bpmDown = mk (L"BUTTON", L"▼", BS_PUSHBUTTON, IDC_BPM_DOWN);

    m_timesig = mk (L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_TIMESIG_COMBO);
    for (int i = 0; i < kTimeSigCount; ++i)
        ::SendMessageW (m_timesig, CB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM> (kTimeSigs[i].label));
    ::SendMessageW (m_timesig, CB_SETCURSEL, 0, 0);

    m_volSlider = mk (TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, IDC_VOLUME_SLIDER);
    if (m_volSlider)
    {
        ::SendMessageW (m_volSlider, TBM_SETRANGE, TRUE, MAKELPARAM (0, 100));
        ::SendMessageW (m_volSlider, TBM_SETPOS, TRUE, 100);
    }

    m_timeLabel   = mk (L"STATIC", L"0:00 / 0:00", SS_LEFT, 0);
    m_offsetLabel = mk (L"STATIC", L"+0.00 秒", SS_LEFT, 0);
    m_srcLabel    = mk (L"STATIC", L"默认 120", SS_LEFT, 0);
    m_volLabel    = mk (L"STATIC", L"100 %", SS_LEFT, 0);
    m_fileLabel   = mk (L"STATIC", L"未载入音频", SS_LEFT | SS_ENDELLIPSIS, 0);
    m_hintLabel   = mk (L"STATIC",
                        L"拖动=平移视图　Ctrl/Alt拖动=改偏移　滚轮=平移　Ctrl/Alt滚轮=缩放　双击=全览",
                        SS_LEFT, 0);
    m_statusLamp  = mk (L"STATIC", L"○ 未载入", SS_LEFT, 0);
    // ⚠ 标签框只有 190px（11px 字号）。加 OGG 后原串会溢出被裁，
    //   所以把 WMA 从这里拿掉腾位置 —— WMA 仍在文件对话框的筛选器和
    //   isSupportedAudioExtension 里，只是不再占用这行提示。
    m_fmtLabel    = mk (L"STATIC", L"支持 MP3/WAV/M4A/AAC/FLAC/OGG", SS_LEFT, 0);
    // ⭐ SS_NOTIFY：让静态文本在被点击时给父窗口发 STN_CLICKED。
    //    不加这个样式，STATIC 是「哑」的 —— 点它父窗口收不到任何 WM_COMMAND，
    //    页脚就只是个纯装饰（这正是「可点击宣传语」必须改样式的原因）。
    m_brand       = mk (L"STATIC", L"♪ B 站「大伟鼓谱」· 欢迎关注，鼓谱 / 教学 / 伴奏持续更新",
                        SS_LEFT | SS_NOTIFY, IDC_BRAND);
    // ⭐ 子类化只为把光标切成手型（见 brandSubclassProc 的说明：
    //    STATIC 自己会吞掉 WM_SETCURSOR，父窗口收不到，判坐标无效）。
    if (m_brand)
        ::SetWindowSubclass (m_brand, brandSubclassProc, kBrandSubclassId,
                             reinterpret_cast<DWORD_PTR> (this));

    // ⚠ 使用指南覆盖层必须【最后创建】：同层子窗口的 z 序 = 创建顺序，
    //   最后建的才盖得住前面所有控件。不加 WS_VISIBLE —— 默认隐藏。
    m_help = ::CreateWindowExW (0, kHelpClass, L"", WS_CHILD,
                                0, 0, w, h, m_hwnd, nullptr, hi, nullptr);
    if (m_help)
        ::SetWindowLongPtrW (m_help, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (this));

    applyFontsToChildren ();

    // ⭐ 控件回读：关掉编辑器再打开时，窗口和所有控件都会重建，但后端（Processor）
    //    一直活着。不回读的话界面显示的是「初始值」，而后端里还存着用户填的值 ——
    //    BPM 输入框一失焦就会自动提交，等于把用户填的 BPM 覆盖成 120。
    syncControlsFromBackend ();

    ::DragAcceptFiles (m_hwnd, TRUE);
    if (m_wave)
        ::DragAcceptFiles (m_wave, TRUE);

    layoutChildren ();
    ::SetTimer (m_hwnd, IDC_TIMER_UI, 50, nullptr);   // 20Hz
    return true;
}

void WinView::destroy ()
{
    if (m_hwnd)
    {
        ::KillTimer (m_hwnd, IDC_TIMER_UI);
        // 先摘掉页脚的子类再销毁窗口：DestroyWindow 会把子窗口一并销毁，
        // 之后再 RemoveWindowSubclass 就是在动已释放窗口的子类链了。
        if (m_brand)
        {
            ::RemoveWindowSubclass (m_brand, brandSubclassProc, kBrandSubclassId);
            m_brand = nullptr;
        }
        ::DestroyWindow (m_hwnd);    // 子窗口（含覆盖层）随之销毁
        m_hwnd = nullptr;
    }
    m_wave = nullptr;
    m_help = nullptr;
    deleteFonts ();
    if (m_bgBrush)
        ::DeleteObject (m_bgBrush);
    m_bgBrush = nullptr;
}

void WinView::resize (int w, int h)
{
    if (!m_hwnd)
        return;
    ::SetWindowPos (m_hwnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE);
    layoutChildren ();
}

//------------------------------------------------------------------------------
void WinView::layoutChildren ()
{
    // 【所有数字都是 96 DPI 下的设计值】统一乘缩放系数后再交给 MoveWindow。
    // 新增/调整控件时继续写设计值即可，别在这里直接写物理像素。
    auto move = [this] (HWND h, int x, int y, int w, int hh) {
        if (h)
            ::MoveWindow (h, px (x), px (y), px (w), px (hh), TRUE);
    };

    move (m_openBtn,    240,   6,  90, 24);
    move (m_helpBtn,    172,   6,  62, 24);
    move (m_wave,        10,  34, kWaveW, kWaveH);

    move (m_timeLabel,   10, 158,  92, 18);
    move (m_zoomOut,    104, 155,  38, 22);
    move (m_zoomIn,     144, 155,  38, 22);
    move (m_zoomFit,    184, 155,  38, 22);
    move (m_backBtn,    224, 155,  80, 22);

    move (m_gridCheck,   10, 182,  62, 20);
    move (m_numCheck,    74, 182,  72, 20);
    move (m_offsetLabel,150, 182, 126, 18);

    move (m_hintLabel,   10, 204, 320, 18);

    move (m_bpmEdit,     46, 226,  44, 22);
    move (m_bpmUp,       91, 226,  18, 11);
    move (m_bpmDown,     91, 237,  18, 11);
    move (m_timesig,    150, 224,  70, 200);
    move (m_srcLabel,   228, 226, 106, 18);

    move (m_volSlider,   46, 252, 188, 24);
    move (m_volLabel,   238, 254,  52, 18);

    move (m_fileLabel,   10, 280, 320, 18);
    move (m_statusLamp,  10, 300, 130, 18);
    move (m_fmtLabel,   140, 300, 190, 18);

    move (m_brand,       10, 358, 320, 18);

    // 使用指南覆盖层：铺满整个面板（0,0 起，整块面板尺寸），必须最后摆放。
    move (m_help,         0,   0, kPanelW, kPanelH);
}

//------------------------------------------------------------------------------
double WinView::xToSec (int x) const
{
    // 基准必须是【缩放后】的波形区宽度，否则 DPI ≠ 96 时点击定位会整体偏移。
    return m_viewStart + (static_cast<double> (x) / m_waveW) * m_viewSpan;
}

int WinView::secToX (double sec) const
{
    if (m_viewSpan <= 0.0)
        return 0;
    return static_cast<int> ((sec - m_viewStart) / m_viewSpan * m_waveW);
}

//------------------------------------------------------------------------------
void WinView::paint (HDC target, const RECT& rc)
{
    // 【恒按整块波形区绘制】secToX() / xToSec() 的基准是 m_waveW，
    // 所以离屏位图也必须恒为 m_waveW × m_waveH；最后再把 rc 与波形区的
    // 交集 Blt 上屏。若按 rc 的宽高建位图，局部重绘（rcPaint 非全窗）时
    // 波形/网格/播放头会整块错位裁切。
    const int W = m_waveW;
    const int H = m_waveH;

    const int sx = std::max (0, static_cast<int> (rc.left));
    const int sy = std::max (0, static_cast<int> (rc.top));
    const int sw = std::min (W, static_cast<int> (rc.right))  - sx;
    const int sh = std::min (H, static_cast<int> (rc.bottom)) - sy;
    if (sw <= 0 || sh <= 0)
        return;

    HDC dc = ::CreateCompatibleDC (target);
    if (!dc)
        return;
    HBITMAP bmp = ::CreateCompatibleBitmap (target, W, H);
    if (!bmp)
    {
        ::DeleteDC (dc);
        return;
    }
    HBITMAP oldBmp = reinterpret_cast<HBITMAP> (::SelectObject (dc, bmp));

    RECT full = { 0, 0, W, H };
    HBRUSH bg = ::CreateSolidBrush (RGB (24, 24, 28));
    ::FillRect (dc, &full, bg);
    ::DeleteObject (bg);

    if (m_backend && !m_backend->hasAudio ())
    {
        ::SetBkMode (dc, TRANSPARENT);
        ::SetTextColor (dc, RGB (150, 150, 160));
        ::SelectObject (dc, m_font);
        RECT tr = { 0, 0, W, H };
        ::DrawTextW (dc, L"把音频文件拖到这里", -1, &tr,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else if (m_backend)
    {
        // ---- 波形 ----
        std::vector<float> peaks;
        m_backend->waveformPeaks (peaks, W, m_viewStart, m_viewStart + m_viewSpan);
        if (!peaks.empty ())
        {
            HPEN wavePen = ::CreatePen (PS_SOLID, std::max (1, px (1)), RGB (90, 190, 255));
            HPEN oldPen = reinterpret_cast<HPEN> (::SelectObject (dc, wavePen));
            const int mid = H / 2;
            const int n = static_cast<int> (peaks.size ());
            for (int i = 0; i < n && i < W; ++i)
            {
                float p = peaks[static_cast<size_t> (i)];
                if (p < 0.0f) p = 0.0f;
                if (p > 1.0f) p = 1.0f;
                const int half = static_cast<int> (p * (H / 2 - px (2)));
                ::MoveToEx (dc, i, mid - half, nullptr);
                ::LineTo (dc, i, mid + half + 1);
            }
            ::SelectObject (dc, oldPen);
            ::DeleteObject (wavePen);
        }

        const float offset = m_backend->offsetSec ();

        // ---- 小节网格 ----
        float bpm = m_backend->gridBPM ();
        const PlugView::Backend::HostTimeline tl0 = m_backend->hostTimeline ();
        if (bpm <= 0.0f && tl0.tempoValid) bpm = tl0.bpm;
        if (bpm <= 0.0f) bpm = 120.0f;

        int beats = m_backend->gridBeatsPerBar ();
        int denom = m_backend->gridBeatDenominator ();
        if (beats <= 0) beats = 4;
        if (denom <= 0) denom = 4;

        const double beatSec = (60.0 / bpm) * (4.0 / denom);
        const double barSec = beatSec * beats;

        if (m_showGrid && beatSec > 1e-6)
        {
            // ⚠️ PS_DOT / PS_DASH 只支持宽度 1（几何画笔限制），所以拍线不跟着放大，
            // 小节线是实线、可以放大。这样高 DPI 下小节线更醒目，层次反而更清楚。
            HPEN barPen  = ::CreatePen (PS_SOLID, std::max (1, px (1)), RGB (255, 190, 90));
            HPEN beatPen = ::CreatePen (PS_DOT,   1, RGB (110, 110, 120));
            HPEN prevPen = reinterpret_cast<HPEN> (::SelectObject (dc, barPen));
            ::SetBkMode (dc, TRANSPARENT);

            // 从视野内第一条拍线开始
            double s = offset;
            if (s > m_viewStart)
                s -= std::floor ((s - m_viewStart) / beatSec) * beatSec;
            else
                s += std::ceil ((m_viewStart - s) / beatSec) * beatSec;

            for (; s <= m_viewStart + m_viewSpan; s += beatSec)
            {
                if (s < m_viewStart)
                    continue;
                const int x = secToX (s);
                if (x < 0 || x > W)
                    continue;

                const long long beatIndex =
                    static_cast<long long> (std::llround ((s - offset) / beatSec));
                const bool isBar = (beatIndex % beats) == 0;

                ::SelectObject (dc, isBar ? barPen : beatPen);
                ::MoveToEx (dc, x, 0, nullptr);
                ::LineTo (dc, x, H);
            }

            if (m_showNumbers && barSec > 1e-6)
            {
                ::SelectObject (dc, m_fontSmall);
                ::SetTextColor (dc, RGB (255, 210, 130));
                long long barNo = static_cast<long long> (std::floor ((m_viewStart - offset) / barSec));
                for (double b = offset + static_cast<double> (barNo) * barSec;
                     b <= m_viewStart + m_viewSpan; b += barSec, ++barNo)
                {
                    if (b < m_viewStart)
                        continue;
                    const int x = secToX (b);
                    if (x < -px (20) || x > W)
                        continue;
                    wchar_t buf[32];
                    std::swprintf (buf, 32, L"%lld", barNo + 1);
                    ::TextOutW (dc, x + px (2), px (2), buf, static_cast<int> (std::wcslen (buf)));
                }
            }

            ::SelectObject (dc, prevPen);
            ::DeleteObject (barPen);
            ::DeleteObject (beatPen);
        }

        // ---- 谱面播放头（绿色）----
        if (tl0.playheadValid)
        {
            const int x = secToX (tl0.playheadSec + offset);
            if (x >= 0 && x <= W)
            {
                HPEN p = ::CreatePen (PS_SOLID, std::max (1, px (2)), RGB (80, 220, 120));
                HPEN op = reinterpret_cast<HPEN> (::SelectObject (dc, p));
                ::MoveToEx (dc, x, 0, nullptr);
                ::LineTo (dc, x, H);
                ::SelectObject (dc, op);
                ::DeleteObject (p);
            }
        }

        // ---- 音频播放头（红色）----
        {
            const int x = secToX (m_backend->positionSec ());
            if (x >= 0 && x <= W)
            {
                HPEN p = ::CreatePen (PS_SOLID, std::max (1, px (2)), RGB (255, 90, 90));
                HPEN op = reinterpret_cast<HPEN> (::SelectObject (dc, p));
                ::MoveToEx (dc, x, 0, nullptr);
                ::LineTo (dc, x, H);
                ::SelectObject (dc, op);
                ::DeleteObject (p);
            }
        }

        // ---- 正偏移：左侧橙色阴影 = 被跳过的前奏 ----
        if (offset > m_viewStart)
        {
            const int x = secToX (offset);
            if (x > 0)
            {
                RECT shade = { 0, 0, x < W ? x : W, H };
                HBRUSH ob = ::CreateSolidBrush (RGB (70, 40, 20));
                ::FillRect (dc, &shade, ob);
                ::DeleteObject (ob);
            }
        }

        // ---- 分家检测徽标 ----
        // 只在「播放中、且不在首尾过渡区」判定：
        //   · 起播头 0.3 秒音频还在追，差值天然偏大 → 不算；
        //   · 音频比乐谱短时尾端必然拉开 → 不算（那不是 bug）。
        // 命中就亮一个红底徽标，把「用户肉眼看不出来的分家」变成一句明确指令。
        {
            const double dur = m_backend->durationSec ();
            const double pos = m_backend->positionSec ();
            const bool mid = (pos > 0.30 && pos < dur - 0.30);
            if (tl0.playing && tl0.playheadValid && dur > 0.60 && mid)
            {
                const double diff = pos - (tl0.playheadSec + offset);
                if (std::fabs (diff) > kDivergenceWarnSec)
                {
                    // 刚点过「回到播放头」却还是错位 → 不是一次性偏差，而是跟随失效，
                    // 此时再点按钮也没用，直接告诉用户重启插件（onTimer 里已记日志）。
                    wchar_t warn[128];
                    if (m_followFail)
                        std::swprintf (warn, 128,
                                       L"跟随可能已失效 · 建议重启插件（错位 %.2f 秒）",
                                       std::fabs (diff));
                    else
                        std::swprintf (warn, 128,
                                       L"与谱面错位 %.2f 秒 · 点「回到播放头」",
                                       std::fabs (diff));

                    ::SelectObject (dc, m_fontSmall);
                    ::SetBkMode (dc, TRANSPARENT);
                    SIZE sz = {};
                    ::GetTextExtentPoint32W (dc, warn,
                                             static_cast<int> (std::wcslen (warn)), &sz);

                    RECT box = { W - sz.cx - px (16), px (4),
                                 W - px (4), px (4) + sz.cy + px (6) };
                    if (box.left < 0) box.left = 0;
                    HBRUSH bw = ::CreateSolidBrush (RGB (208, 64, 50));
                    ::FillRect (dc, &box, bw);
                    ::DeleteObject (bw);

                    ::SetTextColor (dc, RGB (255, 255, 255));
                    RECT tr = box;
                    tr.left += px (4);
                    ::DrawTextW (dc, warn, -1, &tr,
                                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
        }
    }

    // ⚠️ 顺序绝不能反：BitBlt 读的是 DC 【当前选中】的那张位图。
    // 先 SelectObject (dc, oldBmp) 会把 CreateCompatibleDC 自带的 1×1 单色
    // 占位图挂回去 —— 拷出来的就是一整块纯色，画进 bmp 的波形/网格/
    // 播放头全部不上屏（这正是「波形区一片空白」的根因）。
    ::BitBlt (target, rc.left, rc.top, sw, sh, dc, sx, sy, SRCCOPY);
    ::SelectObject (dc, oldBmp);
    ::DeleteObject (bmp);
    ::DeleteDC (dc);
}

//------------------------------------------------------------------------------
// 使用指南覆盖层
//
// 面板只有 340x384，完整说明书塞不进常驻布局（挤掉的会是波形区）。所以做成
// 「？帮助」唤出的覆盖层：铺满整块面板、点任意处关闭。
//
// 实现上它是一个独立的子窗口（kHelpClass），而不是「隐藏所有控件 + 在父窗口上
// 画」—— 后者要来回切换二十来个控件窗口的显示状态，容易在焦点与重绘上留下
// 边角问题；独立窗口还能天然盖住所有兄弟控件（创建顺序最后 = z 序最上）。
//------------------------------------------------------------------------------
void WinView::toggleHelp ()
{
    m_helpVisible = !m_helpVisible;
    if (!m_help)
        return;

    if (m_helpVisible)
    {
        // 显式抬到 z 序最上层再显示：DPI 变化时窗口会被重排，不能只靠创建顺序。
        ::SetWindowPos (m_help, HWND_TOP, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ::ShowWindow (m_help, SW_SHOW);
        ::InvalidateRect (m_help, nullptr, TRUE);
    }
    else
    {
        ::ShowWindow (m_help, SW_HIDE);
    }
}

void WinView::hideHelp ()
{
    if (!m_helpVisible)
        return;
    m_helpVisible = false;
    if (m_help)
        ::ShowWindow (m_help, SW_HIDE);
}

void WinView::paintHelp (HDC dc)
{
    // 行首 "#" = 小标题；其余为正文。
    //
    // 排序 = 【实际操作顺序】：装音频 → 填速度 → 对拍子 → 波形区手势 →
    // 出问题怎么办 → 免费/求关注。只讲「怎么点、怎么拖」，不讲原理。
    // ⚠️ 快捷键必须与本端真实实现一致（本端 = Ctrl/Alt + Shift 微调 + 双击全览；
    //    Linux 端【没有双击全览】，mac 端是 ⌘/⌥ 且有触摸板手势 —— 别三端照抄）。
    // ⚠️ 绝不要写「红绿两线重合即对齐」：两条线本来就是重合的
    //    （音频位置 = 谱面位置 + 偏移，永远如此），重合与否跟偏移调没调对无关
    //    —— 它们只在「跟随出故障」时才分开。判断偏移有没有调好，唯一依据是
    //    【网格小节线有没有落在波形上的鼓点/第一拍】。
    static const wchar_t* kGuide[] = {
        L"#1  装音频：点「打开音频」，或把音频文件直接拖进来。",
        L"     支持 MP3 / WAV / M4A / AAC / FLAC / OGG。",
        L"     装好后在乐谱里按空格播放。",
        L"",
        L"#2  填速度：先填 BPM 和拍号（如 120、4/4）。",
        L"     填好了网格小节线才对得上谱子的小节；",
        L"     留空 = 自动，跟着乐谱走。",
        L"",
        L"#3  对拍子：按住 Ctrl（或 Alt）在波形上左右拖网格，",
        L"     把「1」那条小节线拖到音乐的第一拍上（对准波形里的",
        L"     鼓点）。按住 Shift 拖 = 微调。音频开头被跳过的",
        L"     那段会画成灰色，是正常的。",
        L"",
        L"#4  波形区其它操作：直接拖 = 平移视野；滚轮 = 平移；",
        L"     Ctrl/Alt + 滚轮 = 缩放；双击 = 整首全览。",
        L"",
        L"#5  红线=音频播到哪，绿线=乐谱播到哪 —— 平时它俩就",
        L"     黏在一起，分开或画面里找不到播放头了，点「回到",
        L"     播放头」。还不行就完全退出宿主，再打开。",
        L"",
        L"完全免费，只为方便大家制谱、练鼓。顺手的话点一下",
        L"界面底部的「大伟鼓谱」，到 B 站关注一下就是支持。",
    };
    const int n = static_cast<int> (sizeof (kGuide) / sizeof (kGuide[0]));

    RECT full = { 0, 0, px (kPanelW), px (kPanelH) };
    HBRUSH bg = ::CreateSolidBrush (RGB (23, 25, 33));
    ::FillRect (dc, &full, bg);
    ::DeleteObject (bg);

    ::SetBkMode (dc, TRANSPARENT);

    // 标题行
    ::SelectObject (dc, m_font);
    ::SetTextColor (dc, RGB (140, 217, 140));
    ::TextOutW (dc, px (12), px (8), L"使用指南", 4);

    ::SelectObject (dc, m_fontSmall);
    ::SetTextColor (dc, RGB (133, 133, 133));
    ::TextOutW (dc, px (62), px (10), L"（点任意处关闭）", 8);

    RECT sep = { px (12), px (26), px (kPanelW - 12), px (27) };
    HBRUSH sp = ::CreateSolidBrush (RGB (77, 77, 77));
    ::FillRect (dc, &sep, sp);
    ::DeleteObject (sp);

    // 22 行 × 15px，从 y=34 起 → 末行底 ≈ 362，留 22px 余量（面板高 384）。
    // ⚠️ 文案再加长就要同步这里，否则最后一行会被面板底边裁掉。
    const int lh = px (15);
    int y = px (34);

    for (int i = 0; i < n; ++i)
    {
        const wchar_t* line = kGuide[i];
        if (line[0] != L'\0')
        {
            const bool head = (line[0] == L'#');
            ::SelectObject (dc, head ? m_font : m_fontSmall);
            ::SetTextColor (dc, head ? RGB (140, 217, 140) : RGB (224, 224, 232));
            const wchar_t* text = head ? line + 1 : line;
            ::TextOutW (dc, px (12), y, text, static_cast<int> (std::wcslen (text)));
        }
        y += lh;
    }
}

//------------------------------------------------------------------------------
void WinView::onMouseDown (int x, int y, int mods)
{
    (void) y;
    if (!m_backend || !m_backend->hasAudio ())
        return;

    ::SetCapture (m_wave);
    m_dragging = true;
    m_dragStartX = x;
    m_dragOffset = ((mods & MK_CONTROL) != 0) || ((mods & AP_MK_ALT) != 0);
    // 改偏移时记 offset，平移视野时记 viewStart —— 两者互斥，复用同一个字段。
    m_dragStartVal = m_dragOffset ? m_backend->offsetSec () : m_viewStart;

    // 不再有「点击 / 拖动 = 定位播放头」：音频位置完全由宿主驱动，
    // 手动把它拽走只会和谱面播放头分家，还得再点一次「回到播放头」才能恢复。

    refreshLabels ();
    ::InvalidateRect (m_wave, nullptr, FALSE);
}

void WinView::onMouseMove (int x, int y, int mods)
{
    (void) y;
    if (!m_dragging || !m_backend)
        return;

    if (m_dragOffset)
    {
        const double dxSec = (x - m_dragStartX) / static_cast<double> (m_waveW) * m_viewSpan;
        double v = m_dragStartVal + dxSec;
        if (mods & MK_SHIFT)
            v = m_dragStartVal + dxSec * 0.1;
        if (v > 3600.0)  v = 3600.0;
        if (v < -3600.0) v = -3600.0;
        m_backend->setOffsetSec (static_cast<float> (v));
    }
    else if (m_waveW > 0 && m_viewSpan > 0.0)
    {
        // 平移视野（抓手）：把内容往右拉 → 视野往左移，看到更早的音频。
        const double dxSec = (x - m_dragStartX) / static_cast<double> (m_waveW) * m_viewSpan;
        m_viewStart = m_dragStartVal - dxSec;
        clampView ();
    }

    refreshLabels ();
    ::InvalidateRect (m_wave, nullptr, FALSE);
}

void WinView::onMouseUp ()
{
    if (m_dragging)
    {
        m_dragging = false;
        ::ReleaseCapture ();
    }
}

void WinView::onMouseWheel (int delta, int mods)
{
    if (!m_backend || !m_backend->hasAudio ())
        return;

    if (mods & (MK_CONTROL | AP_MK_ALT))
    {
        zoomBy (delta > 0 ? 0.8 : 1.25, m_backend->positionSec ());
    }
    else
    {
        m_viewStart -= (delta / 120.0) * m_viewSpan * 0.1;
        clampView ();
    }
    refreshLabels ();
    ::InvalidateRect (m_wave, nullptr, FALSE);
}

void WinView::onDoubleClick ()
{
    zoomFit ();
    refreshLabels ();
    ::InvalidateRect (m_wave, nullptr, FALSE);
}

void WinView::zoomBy (double factor, double centerSec)
{
    const double oldSpan = m_viewSpan;
    m_viewSpan *= factor;
    if (m_viewSpan < 0.05)   m_viewSpan = 0.05;
    if (m_viewSpan > 3600.0) m_viewSpan = 3600.0;
    m_viewStart = centerSec - (centerSec - m_viewStart) * (m_viewSpan / oldSpan);
    clampView ();
}

void WinView::zoomFit ()
{
    if (!m_backend)
        return;
    const double dur = m_backend->durationSec ();
    m_viewStart = 0.0;
    m_viewSpan = dur > 0.0 ? dur : 8.0;
}

void WinView::clampView ()
{
    const double dur = m_backend ? m_backend->durationSec () : 0.0;
    if (m_viewStart < 0.0)
        m_viewStart = 0.0;
    if (dur > 0.0 && m_viewStart > dur)
        m_viewStart = dur;
}

// 把视野挪到 sec 处 —— 播放头落在画面 25% 的位置（与 macOS 端同一套规则）。
// 「回到播放头」除了重新对齐，还要把画面也带回去：只对齐不挪视野，用户会看到
// 「显示已对齐，但画面里什么都没有」，比不对齐还困惑。
void WinView::centerViewOn (double sec)
{
    const double dur = m_backend ? m_backend->durationSec () : 0.0;
    if (m_viewSpan <= 0.0)
        return;
    if (dur > 0.0 && m_viewSpan >= dur)
        return;                 // 全览：整段都在画面里，无需滚动
    m_viewStart = sec - m_viewSpan * 0.25;
    clampView ();
}

void WinView::onTimer ()
{
    if (!m_backend)
        return;

    // ---- 「点完『回到播放头』仍然错位」= 跟随本身失效 ----
    // 正常点一下就对齐了。若 3 秒内又错位，说明有某个跟随条件持续为假 ——
    // 徽标文案升级为「建议重启插件」，并记一条日志。
    // 判据与波形区徽标完全一致（同样排除起播追赶期与音频尾端）。
    {
        const PlugView::Backend::HostTimeline tl = m_backend->hostTimeline ();
        const double audioNow = m_backend->positionSec ();
        const double durNow   = m_backend->durationSec ();
        const double offNow   = static_cast<double> (m_backend->offsetSec ());
        const double diffNow  = audioNow - (tl.playheadSec + offNow);

        const bool diverging =
            tl.playing && tl.playheadValid && durNow > 0.60
            && audioNow > 0.30 && audioNow < durNow - 0.30
            && std::fabs (diffNow) > kDivergenceWarnSec;

        const DWORD nowT = ::GetTickCount ();
        if (diverging && (nowT - m_lastBackTick) < kFollowFailWindowMs)
        {
            m_followFail = true;
            if ((nowT - m_followFailLoggedTick) > kFollowFailLogGapMs)
            {
                m_followFailLoggedTick = nowT;
                ap::crashLog ("跟随可能已失效：点「回到播放头」后 %.1f 秒仍错位 %.2f 秒"
                              "（音频 %.3f / 谱面 %.3f + 偏移 %.3f）",
                              static_cast<double> (nowT - m_lastBackTick) * 0.001,
                              std::fabs (diffNow), audioNow, tl.playheadSec, offNow);
            }
        }
        else
        {
            m_followFail = false;
        }
    }

    refreshLabels ();
    ::InvalidateRect (m_wave, nullptr, FALSE);
}

void WinView::refreshLabels ()
{
    if (!m_backend)
        return;

    char buf[512];

    const std::string pos = formatTime (m_backend->positionSec ());
    const std::string total = formatTime (m_backend->durationSec ());
    std::snprintf (buf, sizeof buf, "%s / %s", pos.c_str (), total.c_str ());
    setTextUtf8 (m_timeLabel, buf);

    std::snprintf (buf, sizeof buf, "偏移 %+.2f 秒", m_backend->offsetSec ());
    setTextUtf8 (m_offsetLabel, buf);

    std::snprintf (buf, sizeof buf, "%d %%",
                   static_cast<int> (m_backend->volume () * 100.0f + 0.5f));
    setTextUtf8 (m_volLabel, buf);

    const PlugView::Backend::HostTimeline tl = m_backend->hostTimeline ();
    if (m_backend->gridBPM () > 0.0f)
        ::SetWindowTextW (m_srcLabel, L"手动");
    else if (tl.tempoValid)
        ::SetWindowTextW (m_srcLabel, L"谱面速度");
    else
        ::SetWindowTextW (m_srcLabel, L"默认 120");

    const std::string path = m_backend->currentPath ();
    if (!path.empty ())
    {
        const std::wstring w = utf8ToWide (path);
        const size_t slash = w.find_last_of (L"\\/");
        const std::wstring name = (slash == std::wstring::npos) ? w : w.substr (slash + 1);
        ::SetWindowTextW (m_fileLabel, (L"已载入：" + name).c_str ());
    }
    else
    {
        ::SetWindowTextW (m_fileLabel, L"未载入音频");
    }

    if (!m_backend->hasAudio ())
        ::SetWindowTextW (m_statusLamp, L"○ 未载入");
    else
        ::SetWindowTextW (m_statusLamp, tl.playing ? L"● 跟随宿主播放" : L"○ 宿主已暂停");
}

void WinView::afterLoad ()
{
    if (!m_backend || !m_backend->hasAudio ())
        return;
    m_viewStart = 0.0;
    m_viewSpan = m_backend->durationSec ();
    if (m_viewSpan <= 0.0)
        m_viewSpan = 8.0;
    refreshLabels ();
    ::InvalidateRect (m_wave, nullptr, FALSE);
}

void WinView::onDropFiles (HDROP drop)
{
    if (m_backend)
    {
        const UINT n = ::DragQueryFileW (drop, 0xFFFFFFFF, nullptr, 0);
        if (n > 0)
        {
            wchar_t path[MAX_PATH * 2] = {0};
            if (::DragQueryFileW (drop, 0, path, MAX_PATH * 2) > 0)
            {
                m_backend->loadFile (wideToUtf8 (path));
                afterLoad ();
            }
        }
    }
    ::DragFinish (drop);
}

void WinView::openFileDialog ()
{
    wchar_t path[MAX_PATH * 2] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"音频文件\0*.mp3;*.wav;*.m4a;*.aac;*.wma;*.flac;*.aif;*.aiff;*.ogg;*.oga\0所有文件\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (::GetOpenFileNameW (&ofn) && m_backend)
    {
        m_backend->loadFile (wideToUtf8 (path));
        afterLoad ();
    }
}

void WinView::applyBpmFromEdit ()
{
    if (!m_backend || !m_bpmEdit)
        return;
    wchar_t t[32] = {0};
    ::GetWindowTextW (m_bpmEdit, t, 32);
    const double v = ::_wtof (t);
    m_backend->setGridBPM (v > 0.0 ? static_cast<float> (v) : 0.0f);
    refreshLabels ();
    ::InvalidateRect (m_wave, nullptr, FALSE);
}

void WinView::syncControlsFromBackend ()
{
    if (!m_backend)
        return;

    if (m_bpmEdit)
    {
        const float bpm = m_backend->gridBPM ();
        if (bpm >= 1.0f)
        {
            wchar_t t[32] = {0};
            std::swprintf (t, 32, L"%d", static_cast<int> (bpm + 0.5f));
            ::SetWindowTextW (m_bpmEdit, t);
        }
        else
        {
            // 空框 = 自动（优先跟随宿主速度，宿主不给就用 120）。
            // 不再预填 "120"：那会让「用户还没设过」看起来像「已经设成 120」，
            // 而且一提交就真的变成手动 120 —— 与 macOS 端的语义对齐。
            ::SetWindowTextW (m_bpmEdit, L"");
        }
    }

    if (m_timesig)
    {
        const int beats = m_backend->gridBeatsPerBar ();
        const int denom = m_backend->gridBeatDenominator ();
        for (int i = 0; i < kTimeSigCount; ++i)
        {
            if (kTimeSigs[i].beats == beats && kTimeSigs[i].denom == denom)
            {
                ::SendMessageW (m_timesig, CB_SETCURSEL, i, 0);
                break;
            }
        }
    }

    if (m_volSlider)
        ::SendMessageW (m_volSlider, TBM_SETPOS, TRUE,
                        static_cast<LPARAM> (static_cast<int> (m_backend->volume () * 100.0f + 0.5f)));
}

void WinView::onHScroll (HWND /*slider*/)
{
    if (!m_backend || !m_volSlider)
        return;
    const int pos = static_cast<int> (::SendMessageW (m_volSlider, TBM_GETPOS, 0, 0));
    m_backend->setVolume (static_cast<float> (pos) / 100.0f);
    refreshLabels ();
}

//------------------------------------------------------------------------------
// 打开作者 B 站主页（点底部页脚宣传语时调用）
//
// 用 ShellExecuteW 走系统「打开方式」关联，不引入任何 HTTP 客户端，
// 也不用起线程 —— 它只是把请求交给 Shell，同步返回、不等待浏览器。
//
// ⚠️ 失败只记日志，绝不弹 MessageBox：
//    插件跑在宿主的消息循环里，弹模态框会卡住宿主（实时音频程序卡一下就是爆音）。
//    页脚是个「顺手的广告位」，点坏了最多是没反应，不该有能力影响宿主。
// ⚠️ ShellExecuteW 的返回值 > 32 才算成功（<= 32 是错误码，这是 Win32 的老约定，
//    不能按布尔判断）。
//------------------------------------------------------------------------------
static void openBrandHome ()
{
    const std::wstring url = utf8ToWide (ap::kBrandHomeUrl);
    if (url.empty ())
        return;

    const HINSTANCE r = ::ShellExecuteW (nullptr, L"open", url.c_str (),
                                         nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR> (r) <= 32)
        ap::crashLog ("作者主页打开失败（ShellExecuteW 返回 %lld）：%s",
                      static_cast<long long> (reinterpret_cast<INT_PTR> (r)),
                      ap::kBrandHomeUrl);
}

HBRUSH WinView::onCtlColor (HDC dc, HWND ctl)
{
    if (ctl == m_brand)
    {
        ::SetTextColor (dc, RGB (214, 51, 108));   // B 站粉
        ::SetBkMode (dc, TRANSPARENT);
        return m_bgBrush;
    }
    return reinterpret_cast<HBRUSH> (nullptr);
}

// 鼠标是否停在页脚宣传语上。
//
// 【为什么需要】页脚是个 STATIC，默认光标是箭头 —— 用户看到一行小字，
// 没有任何线索表明它可点。切成手型光标是唯一的「可点」提示。
//
// 【为什么在子类里判，而不是在父窗口的 WM_SETCURSOR 里判坐标】
//   STATIC 控件自己会处理 WM_SETCURSOR（把类光标 = 箭头设上并返回 TRUE），
//   父窗口**根本收不到**这条消息 —— 在父窗口里 GetCursorPos + PtInRect 是白写。
//   所以必须用 SetWindowSubclass 把页脚截下来。子类只挂在页脚这一个控件上，
//   因此「HTCLIENT」本身就等价于「光标在页脚上」，不需要再算坐标。
static LRESULT CALLBACK brandSubclassProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR /*id*/, DWORD_PTR /*ref*/)
{
    if (msg == WM_SETCURSOR && LOWORD (lp) == HTCLIENT)
    {
        ::SetCursor (::LoadCursorW (nullptr, IDC_HAND));
        return TRUE;
    }
    return ::DefSubclassProc (hwnd, msg, wp, lp);
}

/// SetWindowSubclass 的子类 ID（同一控件可以挂多个子类，用 ID 区分；必须唯一且非 0）
static const UINT_PTR kBrandSubclassId = 1;

void WinView::onCommand (int id, int notify)
{
    switch (id)
    {
        case IDC_OPEN:
            openFileDialog ();
            break;
        case IDC_ZOOM_OUT:
            zoomBy (1.25, m_backend ? m_backend->positionSec () : 0.0);
            ::InvalidateRect (m_wave, nullptr, FALSE);
            break;
        case IDC_ZOOM_IN:
            zoomBy (0.8, m_backend ? m_backend->positionSec () : 0.0);
            ::InvalidateRect (m_wave, nullptr, FALSE);
            break;
        case IDC_ZOOM_FIT:
            zoomFit ();
            ::InvalidateRect (m_wave, nullptr, FALSE);
            break;
        case IDC_BACK_TO_SCORE:
            if (m_backend && m_backend->hasAudio ())
            {
                const PlugView::Backend::HostTimeline tl = m_backend->hostTimeline ();
                if (tl.playheadValid)
                {
                    // ⚠ 必须带上起始偏移：谱面 0 秒 = 音频 offset 秒。
                    //   旧代码写的是 seekTo (tl.playheadSec)，漏了这个偏移 ——
                    //   点一次就把两条播放头按偏移量错开，恰好和这个按钮
                    //   「修复分家」的职责相反。（macOS 端一直是带偏移的。）
                    const double target = tl.playheadSec + m_backend->offsetSec ();
                    m_backend->seekTo (target);
                    centerViewOn (target);      // 视野也回到播放位置

                    // 记下「刚刚手动对齐过」：若 3 秒内 onTimer 又发现错位，
                    // 说明是跟随失效而非一次性偏差 → 徽标升级提示 + 落一条日志。
                    m_lastBackTick = ::GetTickCount ();
                    m_followFail = false;
                }
            }
            refreshLabels ();
            ::InvalidateRect (m_wave, nullptr, FALSE);
            break;
        case IDC_HELP_BTN:
            toggleHelp ();
            break;
        case IDC_BRAND:
            // 点底部页脚宣传语 → 用系统默认浏览器打开作者 B 站主页。
            // STATIC 只在带 SS_NOTIFY 时才会发 STN_CLICKED（见 m_brand 创建处）。
            if (notify == STN_CLICKED)
                openBrandHome ();
            break;
        case IDC_GRID_CHECK:
            m_showGrid = (::SendMessageW (m_gridCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ::InvalidateRect (m_wave, nullptr, FALSE);
            break;
        case IDC_NUM_CHECK:
            m_showNumbers = (::SendMessageW (m_numCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ::InvalidateRect (m_wave, nullptr, FALSE);
            break;
        case IDC_BPM_UP:
        case IDC_BPM_DOWN:
        {
            // 两步合并：逻辑完全相同，只差符号。
            // ⚠️ 必须先做下限钳制再看符号 —— 空框（= 自动）时 _wtof("") 得 0，
            //    旧写法会算出「1 BPM」这种荒唐值。现在空框按 20 BPM 起步，
            //    与 macOS 端步进器的范围（20~400）一致。
            if (m_bpmEdit)
            {
                wchar_t t[32] = {0};
                ::GetWindowTextW (m_bpmEdit, t, 32);
                double v = ::_wtof (t);
                if (v < 20.0) v = 20.0;
                v += (id == IDC_BPM_UP) ? 1.0 : -1.0;
                if (v < 20.0)  v = 20.0;
                if (v > 400.0) v = 400.0;
                std::swprintf (t, 32, L"%d", static_cast<int> (v));
                ::SetWindowTextW (m_bpmEdit, t);
                applyBpmFromEdit ();
            }
            break;
        }
        case IDC_BPM_EDIT:
            if (notify == EN_KILLFOCUS)
                applyBpmFromEdit ();
            break;
        case IDC_TIMESIG_COMBO:
            if (notify == CBN_SELCHANGE && m_backend)
            {
                const int sel = static_cast<int> (::SendMessageW (m_timesig, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < kTimeSigCount)
                {
                    m_backend->setGridBeatsPerBar (kTimeSigs[sel].beats);
                    m_backend->setGridBeatDenominator (kTimeSigs[sel].denom);
                    ::InvalidateRect (m_wave, nullptr, FALSE);
                }
            }
            break;
        default:
            break;
    }
}

//------------------------------------------------------------------------------
// 窗口过程
//------------------------------------------------------------------------------
namespace {

LRESULT CALLBACK waveProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WinView* v = viewOf (hwnd);

    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint (hwnd, &ps);
            if (v)
                v->paint (dc, ps.rcPaint);
            ::EndPaint (hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (v) v->onMouseDown (GET_X_LPARAM (lp), GET_Y_LPARAM (lp), modsNow ());
            return 0;
        case WM_MOUSEMOVE:
            if (v) v->onMouseMove (GET_X_LPARAM (lp), GET_Y_LPARAM (lp), modsNow ());
            return 0;
        case WM_LBUTTONUP:
            if (v) v->onMouseUp ();
            return 0;
        case WM_LBUTTONDBLCLK:
            if (v) v->onDoubleClick ();
            return 0;
        case WM_MOUSEWHEEL:
            if (v) v->onMouseWheel (GET_WHEEL_DELTA_WPARAM (wp), modsNow ());
            return 0;
        case WM_DROPFILES:
            if (v) v->onDropFiles (reinterpret_cast<HDROP> (wp));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return ::DefWindowProcW (hwnd, msg, wp, lp);
}

// 使用指南覆盖层窗口过程：内容全自绘，点击任意处关闭。
LRESULT CALLBACK helpProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WinView* v = viewOf (hwnd);

    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint (hwnd, &ps);
            if (v)
                v->paintHelp (dc);
            ::EndPaint (hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            // 点任意处关闭 —— 指南没有任何可交互元素，任何点击都应当只是「知道了」。
            if (v)
                v->hideHelp ();
            return 0;
        case WM_ERASEBKGND:
            return 1;      // 背景由 WM_PAINT 整块填，避免闪烁
        case WM_SETCURSOR:
            ::SetCursor (::LoadCursorW (nullptr, IDC_ARROW));
            return TRUE;
        default:
            break;
    }
    return ::DefWindowProcW (hwnd, msg, wp, lp);
}

LRESULT CALLBACK mainProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*> (lp);
        ::SetWindowLongPtrW (hwnd, GWLP_USERDATA,
                             reinterpret_cast<LONG_PTR> (cs->lpCreateParams));
        return ::DefWindowProcW (hwnd, msg, wp, lp);
    }

    WinView* v = viewOf (hwnd);

    switch (msg)
    {
        case WM_COMMAND:
            if (v) v->onCommand (LOWORD (wp), HIWORD (wp));
            return 0;
        case WM_TIMER:
            if (wp == IDC_TIMER_UI && v) v->onTimer ();
            return 0;
        case WM_HSCROLL:
            if (v) v->onHScroll (reinterpret_cast<HWND> (lp));
            return 0;
        case WM_CTLCOLORSTATIC:
            if (v)
            {
                HBRUSH b = v->onCtlColor (reinterpret_cast<HDC> (wp),
                                          reinterpret_cast<HWND> (lp));
                if (b)
                    return reinterpret_cast<LRESULT> (b);
            }
            break;
        case WM_DROPFILES:
            if (v) v->onDropFiles (reinterpret_cast<HDROP> (wp));
            return 0;
        case WM_DPICHANGED:
            // 跨屏拖动 / 系统缩放改动时收到（宿主需为 Per-Monitor Aware）。
            // 子窗口通常收不到这条消息，收到了就按新 DPI 重建字体与布局。
            if (v) v->onDpiChanged ();
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW (hwnd, msg, wp, lp);
}

} // namespace

//------------------------------------------------------------------------------
// PlugView：VST3 侧接口实现
//------------------------------------------------------------------------------
PlugView::PlugView (Backend* backend, BackendResolver resolver)
    : m_backend (backend), m_resolver (resolver)
{
    if (!m_backend && m_resolver)
        m_backend = m_resolver ();
}

tresult PLUGIN_API PlugView::queryInterface (const Steinberg::TUID _iid, void** obj)
{
    if (!obj)
        return Steinberg::kInvalidArgument;
    *obj = nullptr;

    if (Steinberg::FUnknownPrivate::iidEqual (_iid, Steinberg::FUnknown::iid) ||
        Steinberg::FUnknownPrivate::iidEqual (_iid, IPlugView::iid))
    {
        *obj = static_cast<IPlugView*> (this);
        addRef ();
        return Steinberg::kResultOk;
    }
    return Steinberg::kNoInterface;
}

tresult PLUGIN_API PlugView::isPlatformTypeSupported (FIDString type)
{
    if (type && std::strcmp (type, Steinberg::kPlatformTypeHWND) == 0)
        return Steinberg::kResultTrue;
    return Steinberg::kResultFalse;
}

tresult PLUGIN_API PlugView::attached (void* parent, FIDString type)
{
    if (!parent || !type || std::strcmp (type, Steinberg::kPlatformTypeHWND) != 0)
        return Steinberg::kResultFalse;

    if (!m_backend && m_resolver)
        m_backend = m_resolver ();

    // 缩放系数要【在 create 之前】定下来：宿主很可能是先 attached 再 getSize，
    // 这样 getSize 报出去的就是已经放大过的尺寸。
    const double scaleBefore = g_uiScale;
    g_uiScale = uiScaleFor (reinterpret_cast<HWND> (parent));

    WinView* v = new WinView (m_backend);
    if (!v->create (reinterpret_cast<HWND> (parent), scaledPanelW (), scaledPanelH ()))
    {
        delete v;
        return Steinberg::kResultFalse;
    }
    v->setOwner (this);
    m_view = v;

    // 若宿主是在 attached 之前就调过 getSize（那时还不知道真实 DPI，报的是 96 DPI
    // 下的尺寸），这里主动请宿主按新尺寸重新调整一次窗口。
    if (g_uiScale != scaleBefore)
        requestResizeToPreferred ();
    return Steinberg::kResultOk;
}

void PlugView::requestResizeToPreferred ()
{
    if (!m_frame)
        return;
    ViewRect r;
    if (getSize (&r) == Steinberg::kResultOk)
        m_frame->resizeView (this, &r);
}

tresult PLUGIN_API PlugView::removed ()
{
    if (m_view)
    {
        WinView* v = static_cast<WinView*> (m_view);
        v->destroy ();
        delete v;
        m_view = nullptr;
    }
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onWheel (float distance)
{
    if (m_view)
        static_cast<WinView*> (m_view)->onMouseWheel (distance > 0 ? -120 : 120, 0);
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onKeyDown (char16 key, int16 keyCode, int16 modifiers)
{
    (void) key; (void) keyCode; (void) modifiers;
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onKeyUp (char16 key, int16 keyCode, int16 modifiers)
{
    (void) key; (void) keyCode; (void) modifiers;
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::getSize (ViewRect* size)
{
    if (!size)
        return Steinberg::kInvalidArgument;
    size->left = 0;
    size->top = 0;
    size->right  = scaledPanelW ();
    size->bottom = scaledPanelH ();
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onSize (ViewRect* newSize)
{
    if (m_view && newSize)
        static_cast<WinView*> (m_view)->resize (newSize->right - newSize->left,
                                                newSize->bottom - newSize->top);
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onFocus (Steinberg::TBool state)
{
    (void) state;
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::setFrame (IPlugFrame* frame)
{
    m_frame = frame;
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::canResize ()
{
    return Steinberg::kResultFalse;
}

tresult PLUGIN_API PlugView::checkSizeConstraint (ViewRect* rect)
{
    if (rect)
    {
        rect->right  = rect->left + scaledPanelW ();
        rect->bottom = rect->top  + scaledPanelH ();
    }
    return Steinberg::kResultOk;
}

} // namespace ap
