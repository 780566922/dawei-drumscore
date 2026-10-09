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

// WM_DPICHANGED / WM_MOUSEHWHEEL 在较老的 SDK / _WIN32_WINNT 下没有定义，兜一个。
// （WM_MOUSEHWHEEL 是 Vista 起才有的：横向滚轮 = 鼠标滚轮左右拨 / 触控板左右滑。）
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

namespace {

//---- 面板尺寸（96 DPI 下的【设计值】，统一乘 m_scale 后才是物理像素）----------
//
// ⭐ 与旧版最大的区别：面板不再固定。宿主或用户都可以把窗口拖大，
//    布局按【实际客户区】自适应 —— 波形区随宽度一起变宽（音频工具最需要的就是
//    横向分辨率：看得见鼓点，才谈得上对拍子）。
//    旧版写死 340×384，波形只有 320 px，用户实测「界面太窄」。
//
//   kPanelW/H    = 默认尺寸（宽度 340 → 680）
//   kPanel*Min   = 下限。宽度沿用旧的 340 —— 所有控件在那时刚好排得下，
//                  缩得比它还小便会出现重叠，所以作为硬下限。
//   kPanel*Max   = 上限。防止宿主报一个荒唐的尺寸（如 0 或 100000）把内存吃光。
const int kPanelW    = 680;
const int kPanelH    = 384;
const int kPanelWMin = 340;
const int kPanelHMin = 384;
const int kPanelWMax = 1700;
const int kPanelHMax = 1200;
const int kWaveW  = 320;      ///< 波形区最小宽（面板缩到下限时用它兜底）
const int kWaveH  = 118;      ///< 波形区最小高

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

//---- 视野自动跟随（与 macOS 端同一套规则，见 followPlayheadTo）--------------
// ① 用户手动操作（拖波形 / 缩放 / 滚轮 / 点缩放按钮）后暂停跟随 2 秒；
// ② 播放头远在画面之外（超一整个屏）→ 不追，绝不把用户的视野抢回来；
// ③ 唯一例外：相邻两帧的【谱面位置】差 > kHostJumpSec，即宿主自己跳转了
//    （点小节 / 循环回卷 / 拖播放头）—— 那种情况用户正等着画面跟过去。
const double kHostJumpSec       = 0.25;
const DWORD  kUserScrollHoldMs  = 2000;

//---- 新载入音频的默认视野 ----------------------------------------------------
// 不默认「整曲全览」：全览时波峰糊成一片，根本看不清鼓点，用户每次都得先手动
// 放大好几下（实测要按 11~12 次）。默认直接给 8 秒视野（≈4 小节 @120BPM 4/4），
// 一打开就能看清对齐状况。曲长不足 8 秒时仍回落到全览（见 afterLoad）。
// 三端同名同值（macOS 端在载入处用的就是这个值）。
const double kDefaultViewSpanSec = 8.0;

//---- 重绘节流 ----------------------------------------------------------------
// 拖动/滚轮时每收到一条鼠标消息就整块重绘，在虚拟机（软件 GDI + 抢占式调度）上
// 会把音频线程饿到出 crackle/变调。这里把「鼠标驱动的重绘」限到 ~28fps，
// 剩下的交给 50ms 的界面定时器兜底。
const DWORD kDragPaintGapMs = 36;

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
    IDC_OFFSET_ZERO,        // 「归零」：把起始偏移一键清零
    // ⚠️ 不能叫 IDC_HELP —— winuser.h 已经把 IDC_HELP 定义成「帮助光标」的资源 ID
    //    （MAKEINTRESOURCE(32651)）。同名枚举项会被宏展开成一串语法垃圾，
    //    MSVC 只报 "syntax error: missing '}' before '('"，很难看出是宏冲突。
    IDC_HELP_BTN,
    IDC_BRAND = 1100,
    IDC_TIMER_UI = 1200
};

const wchar_t* kMainClass = L"DaweiDrumScoreMainView";
const wchar_t* kWaveClass = L"DaweiDrumScoreWaveView";

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

int scaledOf (int designPx) { return static_cast<int> (std::lround (designPx * g_uiScale)); }
int preferredPanelW () { return scaledOf (kPanelW); }
int preferredPanelH () { return scaledOf (kPanelH); }
int minPanelW () { return scaledOf (kPanelWMin); }
int minPanelH () { return scaledOf (kPanelHMin); }
int maxPanelW () { return scaledOf (kPanelWMax); }
int maxPanelH () { return scaledOf (kPanelHMax); }

/// 把宿主/用户给的窗口尺寸钳到 [min, max]。
void clampPanelSize (int& w, int& h)
{
    if (w < minPanelW ()) w = minPanelW ();
    if (h < minPanelH ()) h = minPanelH ();
    if (w > maxPanelW ()) w = maxPanelW ();
    if (h > maxPanelH ()) h = maxPanelH ();
}

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
    /// 使用指南自绘（由【主窗口】的 WM_PAINT 调用，见 setHelp 的说明）。
    /// dc 的原点即主窗口客户区左上角。
    void paintHelp (HDC dc);
    /// 显示/隐藏使用指南。
    void toggleHelp ();
    /// 关闭使用指南（主窗口被点到时回调）。
    void hideHelp ();
    bool helpVisible () const { return m_helpVisible; }
    void onMouseDown (int x, int y, int mods);
    void onMouseMove (int x, int y, int mods);
    void onMouseUp ();
    /// 滚轮。horizontal = 横向滚轮（WM_MOUSEHWHEEL / 触控板左右滑）。
    void onWheelDelta (int delta, int mods, bool horizontal);
    /// 光标是否落在波形区上（形参是【屏幕】坐标，滚轮的 lParam 即屏幕坐标）。
    bool waveHitTest (int screenX, int screenY) const;
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
    /// 当前窗口尺寸（物理像素）—— getSize 要如实汇报，否则宿主的缩放框架会打架。
    int panelW () const { return m_panelW; }
    int panelH () const { return m_panelH; }

private:
    void layoutChildren ();
    /// 真正应用新的客户区尺寸（updateDesign = 是否把这次尺寸记为用户选择）。
    void applyPanelSize (int w, int h, bool updateDesign);
    /// 宿主容器装不下时缩回去，防止右侧控件被父窗口裁掉。
    void syncSizeToParent ();
    void createFonts ();
    void deleteFonts ();
    void applyFontsToChildren ();
    void refreshLabels ();
    void openFileDialog ();
    void applyBpmFromEdit ();
    /// ⭐ 视图重建后从后端回读 BPM / 拍号 / 音量（见实现处的说明）
    void syncControlsFromBackend ();
    /// 显示/隐藏使用指南（隐藏期间所有控件一并隐藏，靠主窗口整块自绘）。
    void setHelp (bool on);
    /// 批量显示/隐藏所有控件窗口（只有使用指南要这么做）。
    void showControls (bool show);
    /// 记下「用户正在手动看别处」→ 2 秒内不自动跟随。
    void markUserScrolling ();
    /// 用户停手超时后自动恢复跟随。
    void recoverAutoFollow ();
    /// 视野跟随播放头（规则见 kHostJumpSec 处的说明）。
    void followPlayheadTo (double audioSec, bool allowJump);
    /// 请求重绘波形区。fromMouse=true 时按 kDragPaintGapMs 节流（鼠标消息太密）。
    void requestWaveRepaint (bool fromMouse);
    void zoomBy (double factor, double centerSec);
    void zoomFit ();
    /// 把视野挪到 sec（播放头落在画面 25% 处）。用于「回到播放头」。
    void centerViewOn (double sec);
    void clampView ();
    void afterLoad ();
    /// ⭐ 视图重建后把后端里存的视野读回来（见实现处的说明）
    void restoreViewState ();
    /// 新载入音频的默认视野（afterLoad 与 restoreViewState 共用，避免两份规则漂移）
    void applyDefaultView ();
    double xToSec (int x) const;
    int    secToX (double sec) const;

    PlugView::Backend* m_backend = nullptr;

    HWND m_hwnd = nullptr;
    HWND m_wave = nullptr;
    // ⚠️ 使用指南【不是】独立窗口，而是主窗口自己画的一层（见 setHelp）。
    //    旧版把它做成最后一个创建的子窗口，指望「创建顺序 = z 序最上」；
    //    用户实测在 MuseScore 里没生效：帮助画在了主界面【下面】，
    //    「？帮助」看起来像没反应。改成「隐藏所有控件 + 父窗口整块自绘」后，
    //    层级问题从根上不存在（Linux 端一直是这么做的）。
    bool m_helpVisible = false;
    HWND m_openBtn = nullptr, m_zoomOut = nullptr, m_zoomIn = nullptr, m_zoomFit = nullptr;
    HWND m_backBtn = nullptr, m_helpBtn = nullptr, m_gridCheck = nullptr, m_numCheck = nullptr;
    HWND m_bpmEdit = nullptr, m_bpmUp = nullptr, m_bpmDown = nullptr;
    HWND m_timesig = nullptr, m_volSlider = nullptr, m_offsetZero = nullptr;
    HWND m_timeLabel = nullptr, m_offsetLabel = nullptr, m_srcLabel = nullptr;
    HWND m_volLabel = nullptr, m_fileLabel = nullptr, m_hintLabel = nullptr;
    HWND m_statusLamp = nullptr, m_brand = nullptr, m_fmtLabel = nullptr;

    HFONT m_font = nullptr, m_fontSmall = nullptr, m_fontBold = nullptr;
    HBRUSH m_bgBrush = nullptr;

    PlugView* m_owner = nullptr;      ///< 宿主侧回调（DPI 变化时请求改窗口尺寸）

    double m_viewStart = 0.0;   ///< 视野起点（秒）
    double m_viewSpan = 8.0;    ///< 视野跨度（秒）

    //---- 视野状态回存（关掉编辑器再打开要恢复到同一段视野）----
    // 视图窗口每次打开都是新建的，视野得存在后端（Processor）里；onTimer 里发现
    // 视野变了就写回。改动入口很多（滚轮/拖动/缩放/全览/回到播放头/自动跟随），
    // 集中在一处比对才不会漏（见 MEMORY 铁律 9）。-1 = 还没写过。
    double m_pushedViewStart = -1.0;
    double m_pushedViewSpan  = -1.0;

    //---- 视野自动跟随（规则与 macOS 端一致）----
    bool   m_followScroll = true;    ///< 总开关（现阶段恒为 true）
    bool   m_userScrolling = false;  ///< 用户手动操作期间暂停跟随
    DWORD  m_lastUserScrollTick = 0; ///< 用户最后一次手动操作的时刻（做超时恢复）
    bool   m_hasPrevPlayhead = false; ///< 是否已有上一帧谱面位置（判断宿主跳变）
    double m_prevPlayheadSec = 0.0;

    //---- 「点完『回到播放头』仍错位」检测（onTimer 维护，paint 只读）----
    DWORD m_lastBackTick = 0;          ///< 上次点「回到播放头」的时刻（毫秒）
    bool  m_followFail = false;        ///< 是否处于「点了按钮仍错位」状态
    DWORD m_followFailLoggedTick = 0;  ///< 上次为此写日志的时刻（做冷却）

    //---- 高 DPI ----
    double m_scale = 1.0;             ///< DPI 缩放系数（1.0 = 96 DPI）
    int    m_panelW = kPanelW;        ///< 当前窗口宽（物理像素）
    int    m_panelH = kPanelH;        ///< 当前窗口高（物理像素）
    int    m_designW = kPanelW;       ///< 用户选择的逻辑宽（设计像素，跨 DPI 时按它换算）
    int    m_designH = kPanelH;
    int    m_waveW = kWaveW;          ///< 缩放后的波形区宽（secToX 的基准）
    int    m_waveH = kWaveH;          ///< 缩放后的波形区高

    /// 离屏位图复用。旧版每次 paint 都 CreateCompatibleBitmap/DC ——
    /// 拖动时每帧一次 GDI 分配，在虚拟机上足够把音频线程饿出爆音。
    HDC     m_memDC = nullptr;
    HBITMAP m_memBmp = nullptr;
    int     m_memW = 0, m_memH = 0;

    /// 上一次真正画过的内容指纹（位置/视野）。变了才重绘，避免空转 20Hz 重画。
    double m_lastPaintKey = 1.0e30;

    /// 把「96 DPI 下的设计像素」换算成当前 DPI 下的物理像素。
    int px (int designPx) const
    {
        return static_cast<int> (std::lround (static_cast<double> (designPx) * m_scale));
    }

    bool m_dragging = false;      ///< 波形区按住鼠标中
    bool m_dragOffset = false;    ///< true = 改起始偏移；false = 平移视野
    int  m_dragStartX = 0;
    double m_dragStartVal = 0.0;  ///< 拖动开始时被改的量（改偏移时 = offset，平移时 = viewStart）
    DWORD m_lastMousePaintTick = 0;   ///< 鼠标驱动的上次重绘时刻（节流用）
    bool  m_mousePaintPending = false; ///< 有被节流掉的重绘，等定时器补

    bool m_showGrid = true;
    bool m_showNumbers = true;
};

//------------------------------------------------------------------------------
// 页脚「可点击宣传语」的子类过程 —— 前置声明
//
// ⚠ 它【定义】在文件后半（见 brandSubclassProc 的实现），但 WinView::create()
//   和 destroy() 里就要用到 → 必须在这里先声明。
//   漏掉本行 = MSVC 在 create() 里报 `error C2065: 'brandSubclassProc':
//   undeclared identifier`，Windows 端直接编不过；而 mac / Linux 本机编译
//   【永远看不到】这个问题，只能靠 CI。同理 kBrandSubclassId 也必须提前定义
//   （常量没有「先声明后定义」的写法，直接搬上来）。
//------------------------------------------------------------------------------
static LRESULT CALLBACK brandSubclassProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR id, DWORD_PTR ref);

/// SetWindowSubclass 的子类 ID（同一控件可挂多个子类，用 ID 区分；必须唯一且非 0）
static const UINT_PTR kBrandSubclassId = 1;

//------------------------------------------------------------------------------
// 窗口过程与类注册
//------------------------------------------------------------------------------
namespace {

LRESULT CALLBACK waveProc (HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK mainProc (HWND, UINT, WPARAM, LPARAM);

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
                            m_timesig, m_volSlider, m_offsetZero };
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

    // 逻辑尺寸（用户选的宽高）不变，物理尺寸按新 DPI 重算 ——
    // 跨屏拖动不该把用户拖好的窗口宽度改掉，只是整体放大。
    m_panelW = std::max (px (kPanelWMin), static_cast<int> (std::lround (m_designW * m_scale)));
    m_panelH = std::max (px (kPanelHMin), static_cast<int> (std::lround (m_designH * m_scale)));

    deleteFonts ();
    createFonts ();
    applyFontsToChildren ();
    layoutChildren ();

    if (m_memBmp) { ::DeleteObject (m_memBmp); m_memBmp = nullptr; m_memW = m_memH = 0; }

    ::SetWindowPos (m_hwnd, nullptr, 0, 0, m_panelW, m_panelH,
                    SWP_NOZORDER | SWP_NOMOVE);
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

    clampPanelSize (w, h);
    m_panelW = w;
    m_panelH = h;
    m_designW = static_cast<int> (std::lround (m_panelW / m_scale));
    m_designH = static_cast<int> (std::lround (m_panelH / m_scale));

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
    // 「归零」：一键把起始偏移清零。
    // 之前唯一的改偏移入口是「在波形上 Ctrl/Alt 拖动」，一旦手滑拖到很大的值
    // （或者只是想回到初始状态），就只能再反向拖回去 —— 拖回来的距离可能是好几个屏。
    // 用户实测明确提出「没有偏移归零的方式」，这里补上。
    m_offsetZero  = mk (L"BUTTON", L"归零", BS_PUSHBUTTON, IDC_OFFSET_ZERO);
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

    // ⚠️ 使用指南不再是独立窗口（旧版这里创建了一个铺满面板的 kHelpClass 子窗口，
    //    用户实测它被画在主界面下面）。现在指南由主窗口自己整块画，见 setHelp。

    applyFontsToChildren ();

    // ⭐ 控件回读：关掉编辑器再打开时，窗口和所有控件都会重建，但后端（Processor）
    //    一直活着。不回读的话界面显示的是「初始值」，而后端里还存着用户填的值 ——
    //    BPM 输入框一失焦就会自动提交，等于把用户填的 BPM 覆盖成 120。
    syncControlsFromBackend ();

    // ⭐ 视野也要回读：关掉编辑器再打开时后端里还存着「上次放大到多少、
    //    正看着哪一段」，不回读就退回默认视野（用户实测反馈的就是这个）。
    restoreViewState ();

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
        ::DestroyWindow (m_hwnd);    // 所有子控件随之销毁
        m_hwnd = nullptr;
    }
    m_wave = nullptr;
    m_offsetZero = nullptr;
    if (m_memDC)
    {
        ::DeleteDC (m_memDC);
        m_memDC = nullptr;
    }
    if (m_memBmp)
    {
        ::DeleteObject (m_memBmp);
        m_memBmp = nullptr;
    }
    m_memW = m_memH = 0;
    deleteFonts ();
    if (m_bgBrush)
        ::DeleteObject (m_bgBrush);
    m_bgBrush = nullptr;
}

void WinView::applyPanelSize (int w, int h, bool updateDesign)
{
    clampPanelSize (w, h);
    if (updateDesign)
    {
        // 记住「逻辑尺寸」：跨 DPI 搬窗口时按它换算物理尺寸，不再跳回默认宽度。
        m_designW = static_cast<int> (std::lround (w / m_scale));
        m_designH = static_cast<int> (std::lround (h / m_scale));
    }
    if (w == m_panelW && h == m_panelH)
        return;

    m_panelW = w;
    m_panelH = h;
    if (m_memBmp)
    {
        ::DeleteObject (m_memBmp);
        m_memBmp = nullptr;
        m_memW = m_memH = 0;
    }

    ::SetWindowPos (m_hwnd, nullptr, 0, 0, m_panelW, m_panelH, SWP_NOZORDER | SWP_NOMOVE);
    layoutChildren ();
    m_lastPaintKey = 1.0e30;      // 尺寸变了，下一帧无条件重画
    ::InvalidateRect (m_hwnd, nullptr, TRUE);
    if (m_wave)
        ::InvalidateRect (m_wave, nullptr, FALSE);
}

void WinView::resize (int w, int h)
{
    if (!m_hwnd)
        return;
    applyPanelSize (w, h, true);
}

// 自我防御：宿主给的容器窗口若比我们要的小，就缩回去 —— 否则右侧会整块被父窗口裁掉，
// 「打开音频 / ？帮助 / 回到播放头 / 归零」这些按钮直接看不见（比界面窄更糟）。
// 只有在父窗口真的装不下时才缩；父窗口够大就按我们自己的设计尺寸（用户拖过就是它）。
// 由 50ms 的界面定时器调用（每帧比对一次尺寸，代价可忽略），
// 不碰 m_design —— 父窗口恢复后还能长回用户选择的宽度。
void WinView::syncSizeToParent ()
{
    if (!m_hwnd)
        return;
    const HWND par = ::GetParent (m_hwnd);
    if (!par)
        return;
    RECT rc = {};
    if (!::GetClientRect (par, &rc))
        return;
    const int availW = rc.right - rc.left;
    const int availH = rc.bottom - rc.top;
    if (availW <= 0 || availH <= 0)
        return;

    int wantW = std::min (px (m_designW), availW);
    int wantH = std::min (px (m_designH), availH);
    if (wantW < minPanelW ()) wantW = minPanelW ();
    if (wantH < minPanelH ()) wantH = minPanelH ();

    if (wantW != m_panelW || wantH != m_panelH)
        applyPanelSize (wantW, wantH, false);
}

//------------------------------------------------------------------------------
void WinView::layoutChildren ()
{
    // 【布局基准 = 窗口实际客户区】。下面的坐标全部写 96 DPI 下的设计值，
    // 统一乘缩放系数后再交给 MoveWindow（别在这里直接写物理像素）。
    //
    // 宽度方向：大部分控件靠左固定，右侧那几个（打开音频 / 回到播放头 / 归零 /
    //   音量百分比 / 支持的格式）挂在「右边界 - 10」上，中间留白自动分配
    //   → 窗口拖大时【波形区跟着变宽】（这是用户要加宽窗口的全部意义）。
    // 高度方向：波形区吃掉上方全部余量（面板拉高 = 波形更厚），下方那一段
    //   （时间 / 缩放 / 网格 / BPM / 音量 / 文件 / 页码 / 署名）保持固定高度，
    //   整段贴着面板底部 —— 384 高时与旧版固定布局逐点吻合。
    //
    // ⚠️ 面板被宿主强行缩到比下限还小时，按【下限尺寸】排布（多余部分被裁），
    //    总比控件叠在一起好。
    const int pw  = std::max (m_panelW, px (kPanelWMin));
    const int ph  = std::max (m_panelH, px (kPanelHMin));
    const int pad = px (10);

    auto movePx = [this] (HWND h, int x, int y, int w, int hh) {
        if (h)
            ::MoveWindow (h, x, y, std::max (0, w), std::max (0, hh), TRUE);
    };

    // 底部区块的顶边（设计值 158 = 384 - 226）
    const int bottomTop = ph - px (226);

    // ---- 波形区：宽度吃掉左右各 10 外的全部，高度吃掉上下之间的一切 ----
    m_waveW = std::max (px (kWaveW), pw - pad * 2);
    m_waveH = std::max (px (kWaveH), bottomTop - px (34) - px (6));
    movePx (m_wave, pad, px (34), m_waveW, m_waveH);

    // ---- 顶栏（右对齐：打开音频、？帮助）----
    const int openW = px (90);
    movePx (m_openBtn, pw - pad - openW, px (6), openW, px (24));
    movePx (m_helpBtn, pw - pad - openW - px (6) - px (62), px (6), px (62), px (24));

    // ---- 时间 / 缩放 / 回到播放头 ----
    // ⚠️ 底部区块里所有控件的 y 都必须写成「bottomTop + 偏移」，
    //   不能写成 158 / 182 这类绝对值 —— 面板一旦被拉高，绝对值就飞了。
    movePx (m_timeLabel, px (10), bottomTop, px (92), px (18));
    movePx (m_zoomOut, px (104), bottomTop - px (3), px (38), px (22));
    movePx (m_zoomIn,  px (144), bottomTop - px (3), px (38), px (22));
    movePx (m_zoomFit, px (184), bottomTop - px (3), px (38), px (22));
    const int backW = px (80);
    movePx (m_backBtn, pw - pad - backW, bottomTop - px (3), backW, px (22));

    // ---- 网格 / 小节号 / 偏移（+ 归零）----
    movePx (m_gridCheck, px (10), bottomTop + px (24), px (62), px (20));
    movePx (m_numCheck,  px (74), bottomTop + px (24), px (72), px (20));
    const int zeroW = px (56);
    const int zeroX = pw - pad - zeroW;
    movePx (m_offsetZero, zeroX, bottomTop + px (24), zeroW, px (20));
    movePx (m_offsetLabel, px (150), bottomTop + px (24), zeroX - px (4) - px (150), px (18));

    movePx (m_hintLabel, pad, bottomTop + px (46), pw - pad * 2, px (18));

    // ---- 速度 / 拍号 ----
    movePx (m_bpmEdit, px (46), bottomTop + px (68), px (44), px (22));
    movePx (m_bpmUp,   px (91), bottomTop + px (68), px (18), px (11));
    movePx (m_bpmDown, px (91), bottomTop + px (79), px (18), px (11));
    movePx (m_timesig, px (150), bottomTop + px (66), px (70), px (200));
    movePx (m_srcLabel, px (228), bottomTop + px (68), pw - pad - px (228), px (18));

    // ---- 音量 ----
    const int volLabelW = px (52);
    const int volLabelX = pw - pad - volLabelW;
    movePx (m_volLabel, volLabelX, bottomTop + px (96), volLabelW, px (18));
    movePx (m_volSlider, px (46), bottomTop + px (94),
           volLabelX - px (6) - px (46), px (24));

    // ---- 文件名 / 状态 / 支持格式 ----
    movePx (m_fileLabel,  pad, bottomTop + px (122), pw - pad * 2, px (18));
    movePx (m_statusLamp, px (10), bottomTop + px (142), px (130), px (18));
    movePx (m_fmtLabel, px (140), bottomTop + px (142), pw - pad - px (140), px (18));

    // ---- 页脚署名（贴面板底）----
    movePx (m_brand, pad, ph - px (26), pw - pad * 2, px (18));
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

    // 离屏位图【复用】：旧版每次 paint 都 CreateCompatibleDC + CreateCompatibleBitmap，
    // 拖波形时等于每帧一次 GDI 分配 —— 在虚拟机（软件 GDI、vCPU 又少）上足够把
    // 音频线程饿出爆音（用户实测「边播放边拖波形，音频变慢」）。
    if (!m_memDC)
        m_memDC = ::CreateCompatibleDC (target);
    if (!m_memDC)
        return;
    if (!m_memBmp || m_memW != W || m_memH != H)
    {
        if (m_memBmp)
            ::DeleteObject (m_memBmp);
        m_memBmp = ::CreateCompatibleBitmap (target, W, H);
        m_memW = W;
        m_memH = H;
    }
    if (!m_memBmp)
    {
        ::DeleteDC (m_memDC);
        m_memDC = nullptr;
        return;
    }
    HDC dc = m_memDC;
    HBITMAP oldBmp = reinterpret_cast<HBITMAP> (::SelectObject (dc, m_memBmp));

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
    // 位图本体【不删】—— 它是复用的（见函数开头）。
    ::BitBlt (target, rc.left, rc.top, sw, sh, dc, sx, sy, SRCCOPY);
    ::SelectObject (dc, oldBmp);
}

//------------------------------------------------------------------------------
// 使用指南
//
// 面板最窄只有 340×384，完整说明书塞不进常驻布局（挤掉的会是波形区）。所以做成
// 「？帮助」唤出的覆盖层：铺满整块面板、点任意处关闭。
//
// ⚠️ 【为什么不是独立子窗口】旧版把指南做成一个铺满面板的 kHelpClass 子窗口，
//    依赖「最后一个创建 = z 序最上」。用户实测在 MuseScore 里失效：点「？帮助」
//    之后主界面没被盖住，指南反而画在了它【下面】——看起来像没反应。
//    根因在宿主的窗口/合成层级上，靠 SetWindowPos(HWND_TOP) 扳不动。
//    现在的做法是把这一类问题从根上删掉：显示指南时隐藏【所有】控件窗口，
//    由主窗口在自己的 WM_PAINT 里整块画 —— 层级不参与，就不会错。
//    （Linux 端本来就是这么实现的：整个界面都画在离屏 pixmap 上。）
//------------------------------------------------------------------------------
void WinView::toggleHelp ()
{
    setHelp (!m_helpVisible);
}

void WinView::hideHelp ()
{
    setHelp (false);
}

void WinView::setHelp (bool on)
{
    if (m_helpVisible == on)
        return;
    m_helpVisible = on;

    showControls (!on);

    if (on && m_hwnd)
    {
        // 打开指南时若输入框还握着焦点，先让它失焦提交（否则隐藏窗口会触发
        // EN_KILLFOCUS 在隐藏之后才提交，顺序拧着），再重画整块面板。
        if (m_backend && m_bpmEdit && ::GetFocus () == m_bpmEdit)
            applyBpmFromEdit ();
        ::SetFocus (m_hwnd);
    }

    ::InvalidateRect (m_hwnd, nullptr, TRUE);
}

void WinView::showControls (bool show)
{
    const HWND all[] = {
        m_wave, m_openBtn, m_helpBtn, m_zoomOut, m_zoomIn, m_zoomFit, m_backBtn,
        m_gridCheck, m_numCheck, m_bpmEdit, m_bpmUp, m_bpmDown, m_timesig,
        m_volSlider, m_offsetZero, m_timeLabel, m_offsetLabel, m_srcLabel,
        m_volLabel, m_fileLabel, m_hintLabel, m_statusLamp, m_fmtLabel, m_brand
    };
    for (HWND h : all)
        if (h)
            ::ShowWindow (h, show ? SW_SHOW : SW_HIDE);
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
        L"     鼓点）。按住 Shift 拖 = 微调。调坏了点右边的「归零」。",
        L"     音频开头被跳过的那段会画成灰色，是正常的。",
        L"",
        L"#4  波形区：直接拖 = 平移视野；滚轮 = 平移；",
        L"     Ctrl/Alt + 滚轮 = 缩放；双击 = 整首全览。",
        L"     窗口边缘可以拖动 —— 拉宽它，波形区更大。",
        L"",
        L"#5  在乐谱里点任意小节，画面会跟着跳过去（扒谱方便）。",
        L"     红线=音频播到哪，绿线=乐谱播到哪，平时黏在一起；分开",
        L"     了或找不到播放头，点「回到播放头」；还不行就退出宿主重开。",
        L"",
        L"完全免费，只为方便大家制谱、练鼓。顺手的话点一下",
        L"界面底部的「大伟鼓谱」，到 B 站关注一下就是支持。",
    };
    const int n = static_cast<int> (sizeof (kGuide) / sizeof (kGuide[0]));

    // 铺满【实际客户区】（面板可能被拖得比设计尺寸大/高）
    RECT client = { 0, 0, m_panelW, m_panelH };
    ::GetClientRect (m_hwnd, &client);
    HBRUSH bg = ::CreateSolidBrush (RGB (23, 25, 33));
    ::FillRect (dc, &client, bg);
    ::DeleteObject (bg);

    ::SetBkMode (dc, TRANSPARENT);

    // 标题行
    ::SelectObject (dc, m_font);
    ::SetTextColor (dc, RGB (140, 217, 140));
    ::TextOutW (dc, px (12), px (8), L"使用指南", 4);

    ::SelectObject (dc, m_fontSmall);
    ::SetTextColor (dc, RGB (133, 133, 133));
    ::TextOutW (dc, px (62), px (10), L"（点一下关闭；滚轮不关）", 12);

    RECT sep = { px (12), px (26), client.right - px (12), px (27) };
    HBRUSH sp = ::CreateSolidBrush (RGB (77, 77, 77));
    ::FillRect (dc, &sep, sp);
    ::DeleteObject (sp);

    // 23 行 × 15px，从 y=34 起 → 末行文字顶 364、底 ≈376，仍在面板高 384 之内。
    // ⚠️ 文案再加长就要同步这里（或改小 lh），否则最后一行会被面板底边裁掉。
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
void WinView::markUserScrolling ()
{
    m_userScrolling = true;
    m_lastUserScrollTick = ::GetTickCount ();
}

// 用户停手 2 秒后自动恢复跟随。
// ⚠️ 旧版（mac 端曾踩过）一旦置 true 就永久停跟随，用户滚一下之后播放头就再也
//    不跟了；这里必须有超时恢复。
void WinView::recoverAutoFollow ()
{
    if (!m_userScrolling)
        return;
    if (m_dragging)             // 还在拖，别打断
        return;
    if ((::GetTickCount () - m_lastUserScrollTick) >= kUserScrollHoldMs)
        m_userScrolling = false;
}

// 视野跟随播放头。规则（与 macOS 端逐条对齐）：
//   ① 用户手动操作期间 → 完全不干预，播放头允许跑出画面；
//   ② 播放头远在画面之外（超一整个屏）→ 不追（这是「不抢用户视野」的关键一条）；
//   ③ 唯一例外 allowJump：宿主自己把播放头挪了 → 允许跟过去。
void WinView::followPlayheadTo (double audioSec, bool allowJump)
{
    if (!m_followScroll)
        return;
    if (m_dragging || m_helpVisible)
        return;
    if (!(m_viewSpan > 0.0))
        return;

    // ① 用户在看别处 → 不干预（宿主主动跳转时例外）
    if (m_userScrolling && !allowJump)
        return;

    const double v0 = m_viewStart;
    const double v1 = v0 + m_viewSpan;
    const double margin = m_viewSpan * 0.25;   // 播放头离边缘多远开始滚

    // ② 远在画面之外 → 不追。容差取一整个屏：极端放大时播放头两帧之间就能移动
    //    大半屏，容差太小会漏跟。
    if (!allowJump && (audioSec < v0 - m_viewSpan || audioSec > v1 + m_viewSpan))
        return;

    // ③ 播放头落在画面左侧（循环回卷 / 向前跳转）→ 对到 25% 处，让它重新可见
    if (audioSec < v0)
    {
        m_viewStart = audioSec - margin;
        clampView ();
        requestWaveRepaint (false);
        return;
    }

    // ④ 播放头快到右边缘 → 顺滑向右滚
    if (audioSec > v1 - margin)
    {
        m_viewStart = audioSec - margin;
        clampView ();
        requestWaveRepaint (false);
    }
}

void WinView::requestWaveRepaint (bool fromMouse)
{
    if (!m_wave)
        return;

    if (fromMouse)
    {
        const DWORD now = ::GetTickCount ();
        if ((now - m_lastMousePaintTick) < kDragPaintGapMs)
        {
            // 节流掉：记住「欠一帧」，交给 50ms 的界面定时器补上。
            m_mousePaintPending = true;
            return;
        }
        m_lastMousePaintTick = now;
    }
    m_mousePaintPending = false;
    m_lastPaintKey = 1.0e30;      // 强制下一帧真的重画（指纹比对放行）
    ::InvalidateRect (m_wave, nullptr, FALSE);
}

/// 光标（【屏幕】坐标，滚轮的 lParam 就是屏幕坐标）是否落在波形区上。
/// 滚轮消息在 Windows 上是发给【焦点窗口】的，不是发给光标下的窗口
/// （除非开了「悬停滚动非活动窗口」），所以必须在主窗口里自己按坐标判断 ——
/// 这正是用户报的「滚轮不平移」的根因：消息压根没送到波形窗口上。
bool WinView::waveHitTest (int screenX, int screenY) const
{
    if (!m_wave)
        return false;
    RECT r = {};
    if (!::GetWindowRect (m_wave, &r))
        return false;
    return (screenX >= r.left && screenX < r.right &&
            screenY >= r.top  && screenY < r.bottom);
}

void WinView::onMouseDown (int x, int y, int mods)
{
    (void) y;
    if (!m_backend || !m_backend->hasAudio ())
        return;

    // ⭐ 让波形区拿到键盘焦点：滚轮消息是发给焦点窗口的，不点一下它，
    //    滚轮只会落到「上一次点过的控件」上（比如 BPM 输入框）。
    ::SetFocus (m_wave);

    ::SetCapture (m_wave);
    m_dragging = true;
    m_dragStartX = x;
    m_dragOffset = ((mods & MK_CONTROL) != 0) || ((mods & AP_MK_ALT) != 0);
    // 改偏移时记 offset，平移视野时记 viewStart —— 两者互斥，复用同一个字段。
    m_dragStartVal = m_dragOffset ? m_backend->offsetSec () : m_viewStart;

    // 平移视野 = 用户在主动看别处 → 暂停自动跟随 2 秒（拖完自然恢复）。
    // 改偏移【不算】：那只动网格锚点，播放头本身没被挪走。
    if (!m_dragOffset)
        markUserScrolling ();

    // 不再有「点击 / 拖动 = 定位播放头」：音频位置完全由宿主驱动，
    // 手动把它拽走只会和谱面播放头分家，还得再点一次「回到播放头」才能恢复。

    refreshLabels ();
    requestWaveRepaint (false);
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
        // 只有偏移会改变标签内容；平移时时间/偏移都没变，不必每帧刷六个 STATIC
        // （SetWindowTextW 会连带重绘控件，鼠标消息密集时是实打实的开销）。
        refreshLabels ();
    }
    else if (m_waveW > 0 && m_viewSpan > 0.0)
    {
        // 平移视野（抓手）：把内容往右拉 → 视野往左移，看到更早的音频。
        const double dxSec = (x - m_dragStartX) / static_cast<double> (m_waveW) * m_viewSpan;
        m_viewStart = m_dragStartVal - dxSec;
        clampView ();
        markUserScrolling ();
    }

    requestWaveRepaint (true);
}

void WinView::onMouseUp ()
{
    if (m_dragging)
    {
        m_dragging = false;
        m_lastUserScrollTick = ::GetTickCount ();   // 松手后再等 2 秒才恢复跟随
        ::ReleaseCapture ();
    }
}

void WinView::onWheelDelta (int delta, int mods, bool horizontal)
{
    if (!m_backend || !m_backend->hasAudio ())
        return;

    if (mods & (MK_CONTROL | AP_MK_ALT))
    {
        zoomBy (delta > 0 ? 0.8 : 1.25, m_backend->positionSec ());
    }
    else
    {
        markUserScrolling ();
        const double step = (delta / 120.0) * m_viewSpan * 0.1;
        // 纵向滚轮：向上 = 往更早看（与旧版一致）。
        // 横向滚轮（WM_MOUSEHWHEEL，delta > 0 = 向右拨）：往更晚看。
        m_viewStart += horizontal ? step : -step;
        clampView ();
    }
    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::onDoubleClick ()
{
    zoomFit ();
    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::zoomBy (double factor, double centerSec)
{
    // ⭐ 缩放也算「用户在看别处」：否则点「放大/缩小」按钮（它们不经过滚轮）
    //    时播放头会被挤出画面，紧接着被自动跟随拽回去 → 画面抽搐。
    //    放在这里就不漏任何缩放入口。
    markUserScrolling ();

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
    markUserScrolling ();
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
    // 宿主容器窗口若装不下我们（老版本可能给了个 340 宽的窗口），先缩回去，
    // 否则右侧那排按钮会被父窗口裁掉 —— 那比界面窄更糟。
    syncSizeToParent ();

    if (!m_backend)
        return;

    const PlugView::Backend::HostTimeline tl = m_backend->hostTimeline ();
    const double audioNow = m_backend->positionSec ();
    const double durNow   = m_backend->durationSec ();
    const double offNow   = static_cast<double> (m_backend->offsetSec ());
    const double diffNow  = audioNow - (tl.playheadSec + offNow);

    // ---- 「点完『回到播放头』仍然错位」= 跟随本身失效 ----
    // 正常点一下就对齐了。若 3 秒内又错位，说明有某个跟随条件持续为假 ——
    // 徽标文案升级为「建议重启插件」，并记一条日志。
    // 判据与波形区徽标完全一致（同样排除起播追赶期与音频尾端）。
    {
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

    // ---- 视野跟随 ----
    // ⭐ 先区分「播放头自己在走」和「宿主把它挪走了」：前者画面只许顺滑滚，
    //    后者画面该跟过去。20Hz 采样下正常前进每帧只有几十毫秒；
    //    超过 kHostJumpSec 就是宿主跳变（点小节 / 循环回卷 / 拖播放头）。
    bool hostJumped = false;
    if (tl.playheadValid)
    {
        if (m_hasPrevPlayhead &&
            std::fabs (tl.playheadSec - m_prevPlayheadSec) > kHostJumpSec)
            hostJumped = true;
        m_prevPlayheadSec = tl.playheadSec;
        m_hasPrevPlayhead = true;
    }

    recoverAutoFollow ();

    // ⭐ 宿主自己跳的（点小节 / 循环回卷 / 拖播放头）就算停在【暂停】状态也要把
    //    画面带过去 —— 扒谱就是在暂停下一个小节一个小节点的。以前这里写死了
    //    `tl.playing`，于是暂停时换位置画面纹丝不动，「空白乐谱直接定位到音乐」
    //    这件事根本做不到。自然播放推进（allowJump=false）时仍然只许顺滑滚。
    if (tl.playheadValid && (tl.playing || hostJumped))
        followPlayheadTo (tl.playheadSec + offNow, hostJumped);

    refreshLabels ();

    // ---- 波形区重绘：只在内容真的变了的时候 ----
    // 旧版每 50ms 无条件重画整块波形（连暂停、连没有音频时都在画）。
    // 在虚拟机上这是纯白烧 CPU，会跟音频线程抢核 → 出爆音/变速。
    // 这里用一个「内容指纹」比对：位置 / 谱面位置 / 偏移 / 视野 / 播放态 / 徽标态
    // 只要有一项变了才重画；被节流掉的鼠标重绘也在这里补上。
    const double key = audioNow * 1.0 + tl.playheadSec * 7.0 + offNow * 11.0
                     + m_viewStart * 13.0 + m_viewSpan * 17.0
                     + (tl.playing ? 3.0 : 5.0) + (m_followFail ? 19.0 : 23.0)
                     + (tl.playheadValid ? 29.0 : 31.0);
    if (m_mousePaintPending || key != m_lastPaintKey)
    {
        m_mousePaintPending = false;
        m_lastPaintKey = key;
        ::InvalidateRect (m_wave, nullptr, FALSE);
    }

    // ---- 视野状态回存：关掉编辑器再打开要回到同一段视野 ----
    // ⭐ 集中在这里比对是刻意的：视野的改动入口很多（滚轮 / 拖动 / 缩放 / 全览 /
    //    回到播放头 / 自动跟随），逐个入口去写必然漏一个（见 MEMORY 铁律 9）。
    if (m_backend->hasAudio () && m_viewSpan > 0.0
        && (std::fabs (m_viewStart - m_pushedViewStart) > 1e-6
         || std::fabs (m_viewSpan - m_pushedViewSpan) > 1e-6))
    {
        m_pushedViewStart = m_viewStart;
        m_pushedViewSpan  = m_viewSpan;
        m_backend->setViewState (m_viewStart, m_viewSpan);
    }
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

// 新载入音频的默认视野：放大到「前几小节」，从起始偏移处开始看，而不是整曲全览
// （全览时波峰糊成一片，看不清鼓点）。与 macOS 端的 `zoomToSpan:` 同一套。
// ⚠️ 这段规则被 afterLoad（用户换文件）和 restoreViewState（后端里没存过视野）
//    两处共用，所以单独成函数 —— 写两份必然漂移。
void WinView::applyDefaultView ()
{
    const double dur = m_backend->durationSec ();
    if (dur > 0.0 && dur <= kDefaultViewSpanSec)
    {
        m_viewStart = 0.0;                  // 本来就短，直接全览
        m_viewSpan  = dur;
    }
    else
    {
        m_viewSpan  = kDefaultViewSpanSec;
        m_viewStart = m_backend->offsetSec ();
        clampView ();
    }
}

// ⭐ 视图重建后把后端里存的视野读回来。
// 用户实测：「把插件界面关掉再打开，波形变成全曲全览了，而不是之前放大的那一段」。
// 根因：关掉编辑器只销毁视图窗口，后端（Processor）还活着 —— 视野是用户的操作
// 结果，跟 BPM 一样必须存在后端里，重建视图时回读。
void WinView::restoreViewState ()
{
    if (!m_backend || !m_backend->hasAudio ())
        return;

    double vs = 0.0, span = 0.0;
    m_backend->getViewState (vs, span);
    if (span > 0.0)
    {
        m_viewSpan  = span;
        m_viewStart = vs;
        clampView ();
    }
    else
    {
        applyDefaultView ();   // 后端里没存过（或刚换过音频）→ 默认视野
    }

    // 记成「已回存」，免得 onTimer 第一帧把同一份值再写一遍。
    m_pushedViewStart = m_viewStart;
    m_pushedViewSpan  = m_viewSpan;
    requestWaveRepaint (false);
}

void WinView::afterLoad ()
{
    if (!m_backend || !m_backend->hasAudio ())
        return;

    applyDefaultView ();

    // 换了音频 = 换了时间轴：上一首的谱面位置不能拿来判「宿主跳变」。
    m_hasPrevPlayhead = false;
    m_userScrolling = false;
    refreshLabels ();
    requestWaveRepaint (false);
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
    requestWaveRepaint (false);
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

/// SetWindowSubclass 的子类 ID 见文件前半的前置声明区（必须在 WinView::create 之前定义）
void WinView::onCommand (int id, int notify)
{
    switch (id)
    {
        case IDC_OPEN:
            openFileDialog ();
            break;
        case IDC_ZOOM_OUT:
            zoomBy (1.25, m_backend ? m_backend->positionSec () : 0.0);
            requestWaveRepaint (false);
            break;
        case IDC_ZOOM_IN:
            zoomBy (0.8, m_backend ? m_backend->positionSec () : 0.0);
            requestWaveRepaint (false);
            break;
        case IDC_ZOOM_FIT:
            zoomFit ();
            requestWaveRepaint (false);
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

                    // 点这个按钮 = 「我要看播放头」，所以立刻恢复自动跟随
                    //（否则刚滚过波形的人点了按钮还是 2 秒不跟，白点）。
                    m_userScrolling = false;
                    m_lastUserScrollTick = 0;

                    // 记下「刚刚手动对齐过」：若 3 秒内 onTimer 又发现错位，
                    // 说明是跟随失效而非一次性偏差 → 徽标升级提示 + 落一条日志。
                    m_lastBackTick = ::GetTickCount ();
                    m_followFail = false;
                }
            }
            refreshLabels ();
            requestWaveRepaint (false);
            break;
        case IDC_OFFSET_ZERO:
            // 「归零」：把起始偏移一键清零。
            // 之前唯一的改偏移入口是「在波形上 Ctrl/Alt 拖动」，手滑拖到很大的值
            // 之后只能反向拖回去 —— 可能要拖好几个屏。用户实测点名要这个按钮。
            if (m_backend)
            {
                m_backend->setOffsetSec (0.0f);

                // 归零会让音频跳回「谱面位置 + 0」。若播放头因此跑出画面，就顺手把
                // 它带回来（只在真跑出画面时才动 —— 用户正盯着的那段不该被抢走）。
                const PlugView::Backend::HostTimeline tlb = m_backend->hostTimeline ();
                if (tlb.playheadValid &&
                    (tlb.playheadSec < m_viewStart ||
                     tlb.playheadSec > m_viewStart + m_viewSpan))
                    centerViewOn (tlb.playheadSec);

                refreshLabels ();
                requestWaveRepaint (false);
            }
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
            requestWaveRepaint (false);
            break;
        case IDC_NUM_CHECK:
            m_showNumbers = (::SendMessageW (m_numCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            requestWaveRepaint (false);
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
                    requestWaveRepaint (false);
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
            if (v) v->onWheelDelta (GET_WHEEL_DELTA_WPARAM (wp), modsNow (), false);
            return 0;
        case WM_MOUSEHWHEEL:
            // 横向滚轮（鼠标滚轮左右拨 / 触控板左右滑）—— 与纵向一样用来平移视野。
            if (v) v->onWheelDelta (GET_WHEEL_DELTA_WPARAM (wp), modsNow (), true);
            return 0;
        // ⚠️ 波形区为了滚轮可靠会拿到键盘焦点（见 onMouseDown），但它自己不处理
        //   任何按键。这里把键盘消息原样转给宿主的窗口 —— 否则我们等于把空格
        //   （播放/暂停）、方向键统统吃掉了；用 PostMessage 异步投递，避免在宿主
        //   的消息循环里嵌套调用它的窗口过程。
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            if (v && v->hwnd ())
            {
                if (HWND host = ::GetParent (v->hwnd ()))
                {
                    ::PostMessageW (host, msg, wp, lp);
                    return 0;
                }
            }
            break;
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
    // 使用指南可见时，这一层由主窗口整块自绘（见 setHelp），
    // 所有控件都已经被隐藏，所以下面这些判断不会有「盖住控件」的副作用。
    const bool help = (v && v->helpVisible ());

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
        case WM_PAINT:
            if (help)
            {
                PAINTSTRUCT ps;
                HDC dc = ::BeginPaint (hwnd, &ps);
                v->paintHelp (dc);
                ::EndPaint (hwnd, &ps);
                // ⚠️ 不能落到 DefWindowProc：它会用类的背景刷把指南文字擦掉。
                return 0;
            }
            break;              // 平时走默认流程（用 COLOR_BTNFACE 擦背景）
        case WM_ERASEBKGND:
            if (help)
                return 1;       // 背景由 paintHelp 整块填，顺便免掉闪烁
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            // 指南铺满整块面板、没有任何可交互元素，点任意处都只该是「知道了」。
            if (help)
            {
                v->hideHelp ();
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            // ⭐ 滚轮消息在 Windows 上是发给【焦点窗口】的，不是发给光标下的窗口
            //   （除非系统开了「悬停滚动非活动窗口」）。焦点基本都在宿主/别的控件
            //   手里，所以滚轮经常被 BPM 输入框之类的控件先接走 —— 这正是用户报的
            //   「滚轮不左右移动波形」。
            //   对策：在主窗口这一层按【光标位置】命中测试，落在波形区就自己处理。
            //   （没接住的那些会经 DefWindowProc 继续上抛给宿主，不抢它的滚轮。）
            // ⚠️ 指南可见时滚轮【只吞掉、不关闭】：指南是固定的一屏文字，但用户
            //    习惯性会往下滚一滚看看还有没有，一滚就关 = 用户报的「刚进去就没了」。
            //    关闭只认鼠标按下（见下面 WM_LBUTTONDOWN 那组）。
            if (help)
                return 0;
            if (v)
            {
                const int sx = GET_X_LPARAM (lp);   // 滚轮的 lParam 是屏幕坐标
                const int sy = GET_Y_LPARAM (lp);
                if (v->waveHitTest (sx, sy))
                {
                    v->onWheelDelta (GET_WHEEL_DELTA_WPARAM (wp), modsNow (),
                                     msg == WM_MOUSEHWHEEL);
                    return 0;
                }
            }
            break;      // 不在波形区上 → 原样交给宿主（它可能要用滚轮滚乐谱）
        }
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
    if (!v->create (reinterpret_cast<HWND> (parent), preferredPanelW (), preferredPanelH ()))
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

// 请宿主按【默认尺寸】调整窗口（DPI 变化时用）。
// ⚠️ 不能拿 getSize 的结果去用 —— 它现在返回的是「当前尺寸」，
//    而这里要的是「换了 DPI 之后的默认尺寸」（WinView 内部已经按 m_design 算好了）。
void PlugView::requestResizeToPreferred ()
{
    if (!m_frame)
        return;
    ViewRect r;
    r.left = 0;
    r.top = 0;
    if (m_view)
    {
        WinView* v = static_cast<WinView*> (m_view);
        r.right  = v->panelW ();
        r.bottom = v->panelH ();
    }
    else
    {
        r.right  = preferredPanelW ();
        r.bottom = preferredPanelH ();
    }
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
    // 宿主代发的滚轮（MuseScore 目前不这么做）。没有修饰键信息，按平移处理。
    if (m_view)
        static_cast<WinView*> (m_view)->onWheelDelta (distance > 0 ? -120 : 120, 0, false);
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
    // ⭐ 视图已经建好时【如实汇报当前尺寸】：窗口可以被用户拖大，宿主随时会来问
    //   「你现在多大」。这里若还报「默认尺寸」，宿主会照着旧尺寸折腾窗口，
    //   用户刚拖好的宽度就被弹回去。
    if (m_view)
    {
        WinView* v = static_cast<WinView*> (m_view);
        size->right  = v->panelW ();
        size->bottom = v->panelH ();
    }
    else
    {
        size->right  = preferredPanelW ();
        size->bottom = preferredPanelH ();
    }
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

// 允许宿主/用户把编辑器窗口拖大（旧版返回 false = 固定尺寸）。
// 波形区随宽度自适应，面板窄到 340 也仍然排得下 —— 见 layoutChildren。
tresult PLUGIN_API PlugView::canResize ()
{
    return Steinberg::kResultTrue;
}

tresult PLUGIN_API PlugView::checkSizeConstraint (ViewRect* rect)
{
    if (rect)
    {
        int w = rect->right - rect->left;
        int h = rect->bottom - rect->top;
        clampPanelSize (w, h);
        rect->right  = rect->left + w;
        rect->bottom = rect->top  + h;
    }
    return Steinberg::kResultOk;
}

} // namespace ap
