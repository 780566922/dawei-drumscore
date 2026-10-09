//==============================================================================
// gui_linux.cpp — 中文界面（Linux / X11 + Xft 实现 VST3 IPlugView）
//
// 对应 macOS 的 gui.mm（AppKit）与 Windows 的 gui_win.cpp（Win32）。
// 三平台共用同一套 ap::PlugView::Backend 接口，外观与交互保持一致：
//   - 波形预览 + 可拖动播放头
//   - 带符号起始偏移（在波形上按住 Ctrl / Alt 拖动）
//   - 小节网格（BPM + 拍号）
//   - 音量、缩放、全览、回到播放头、使用指南
//   - 拖入音频文件 / 点按钮选文件
//
// 为什么不用 GTK / VSTGUI：
//   VST3 SDK 的 VSTGUI 没下载；GTK 会给用户端带来一堆运行时依赖。
//   这里直接用 X11 自绘（Xft 负责中文与抗锯齿字体），插件本身零额外依赖。
//
// 宿主集成：VST3 在 Linux 上通过 kPlatformTypeX11EmbedWindowID 传父窗口 ID，
//   我们在自己的 X connection 上创建子窗口挂上去。
//
// 线程模型（与 Mac/Win 不同，这里需要特意设计）：
//   宿主不保证实现 IRunLoop，所以不依赖它。改为：
//     - 进程内先调 XInitThreads()，让 Xlib 支持多线程加锁；
//     - 起一个后台线程，独占处理 X 事件 + 定时重绘（约 30Hz）；
//     - 主线程（宿主的 GUI 线程）只在 attached/onSize/removed 里
//       用 XLockDisplay 短暂操作窗口尺寸。
//   X11 绘图本身不做双缓冲会闪，这里用 Pixmap 离屏绘制再整体拷贝。
//==============================================================================
#include "gui.h"
#include "crashguard.h"   // ap::crashLog：跟随疑似失效时落一条日志

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>      // size_t
#include <cstdint>      // uintptr_t（宿主传进来的 X11 Window 句柄还原）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

//------------------------------------------------------------------------------
// 尺寸与颜色
//
// ⭐ 面板不再写死：宿主或用户都可以把窗口拖大，布局由 layoutAll() 按【实际窗口
//    尺寸】重算 —— 波形区随宽度一起变宽（音频工具最需要横向分辨率）。
//    · kPanelW/H    = 默认尺寸（宽度 340 → 680）
//    · kPanel*Min   = 下限（宽度沿用旧的 340，所有控件在那时刚好排得下）
//    · kPanel*Max   = 上限（挡住宿主报来的荒唐尺寸）
//------------------------------------------------------------------------------
const int kPanelW = 680;   ///< 默认面板宽
const int kPanelH = 384;   ///< 默认面板高（含底部 B 站署名）
const int kPanelWMin = 340;
const int kPanelHMin = 384;
const int kPanelWMax = 1700;
const int kPanelHMax = 1200;

// 波形区（由 layoutAll() 重算，不是常量 —— 宽度跟着面板走）
int kWaveX  = 10;
int kWaveY  = 34;
int kWaveW  = 320;
int kWaveH  = 118;

struct RGB8 { unsigned char r, g, b; };

const RGB8 kBg        = { 240, 240, 244 };
const RGB8 kWaveBg    = {  24,  24,  28 };
const RGB8 kWaveLine  = {  90, 190, 255 };
const RGB8 kBarLine   = { 255, 190,  90 };
const RGB8 kBeatLine  = { 110, 110, 120 };
const RGB8 kNumText   = { 255, 210, 130 };
const RGB8 kScoreHead = {  80, 220, 120 };
const RGB8 kAudioHead = { 255,  90,  90 };
const RGB8 kOffsetSh  = {  70,  40,  20 };
const RGB8 kText      = {  40,  40,  48 };
const RGB8 kTextDim   = { 120, 120, 130 };
const RGB8 kBtnFace   = { 225, 225, 232 };
const RGB8 kBtnEdge   = { 165, 165, 178 };
const RGB8 kBtnDown   = { 200, 200, 212 };
const RGB8 kBtnHot    = { 236, 236, 244 };
const RGB8 kCheckOn   = {  70, 130, 220 };
const RGB8 kBiliPink  = { 214,  51, 108 };
const RGB8 kWhite     = { 255, 255, 255 };

//------------------------------------------------------------------------------
// 控件矩形（与 Windows 版一一对应，便于对照维护）
//
// ⚠️ 这些【不是常量】—— 由 X11View::layoutAll() 按当前窗口尺寸重算。
//    下面给的初值就是 340×384 下的旧布局（GUI 还没建好时的兜底值）。
//------------------------------------------------------------------------------
struct Rect
{
    int x, y, w, h;
    bool hit (int px, int py) const
    {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

Rect rOpenBtn  = { 240,   6,  90, 24 };
Rect rHelpBtn  = { 172,   6,  62, 24 };   ///< 「？帮助」：唤出使用指南覆盖层
Rect rTime     = {  10, 158,  92, 18 };
Rect rZoomOut  = { 104, 155,  38, 22 };
Rect rZoomIn   = { 144, 155,  38, 22 };
Rect rZoomFit  = { 184, 155,  38, 22 };
Rect rBack     = { 224, 155,  80, 22 };
Rect rGridChk  = {  10, 182,  62, 20 };
Rect rNumChk   = {  74, 182,  72, 20 };
Rect rOffset   = { 150, 182, 126, 18 };
Rect rOffsetZero = { 280, 182, 50, 20 };  ///< 「归零」：起始偏移一键清零
Rect rHint     = {  10, 204, 320, 18 };
Rect rBpmBox   = {  46, 226,  44, 22 };
Rect rBpmUp    = {  91, 226,  18, 11 };
Rect rBpmDown  = {  91, 237,  18, 11 };
Rect rTimeSig  = { 150, 224,  70, 22 };
Rect rSrc      = { 228, 226, 106, 18 };
Rect rVolume   = {  46, 252, 188, 24 };
Rect rVolLabel = { 238, 254,  52, 18 };
Rect rFile     = {  10, 280, 320, 18 };
Rect rStatus   = {  10, 300, 130, 18 };
Rect rFmt      = { 140, 300, 190, 18 };
Rect rBrand    = {  10, 358, 320, 18 };

//------------------------------------------------------------------------------
// 分家检测阈值（秒）
//
// 正常播放时「音频实际播到哪」与「谱面位置 + 偏移」只差一个音频缓冲的量级
// （几十毫秒）。超过这个值就认为跟随出了问题，波形区右上角会亮出红色错位徽标，
// 提示用户点「回到播放头」一键修复。
// 与 macOS / Windows 端取同一个值，别各写各的。
//------------------------------------------------------------------------------
const double kDivergenceWarnSec = 0.25;

//---- 「点完『回到播放头』仍然错位」的判定窗口 ----------------------------------
// 正常点一下就对齐了。若这么久之内又错位，说明偏差不是一次能纠正的，而是某个
// 跟随条件**持续**为假 —— 再点按钮也没用，徽标文案升级为「建议重启插件」，
// 并记一条日志（带冷却，避免抖动着刷屏）。
// ⚠️ 这里只做「提示升级」，绝不自动 seek：0.25 秒的偏差等于十几个音频块，
//    属结构性故障，自动 seek 会下一块又错开 → 每几十毫秒拉一次 → 音频发抖。
//    （跳变场景的自动硬 seek 在 aplaysdk.cpp 的 captureHostTimeline 里。）
const double kFollowFailWindowSec = 3.0;
const double kFollowFailLogGapSec = 5.0;

//---- 视野自动跟随（与 macOS / Windows 端同一套规则，见 followPlayheadTo）------
// ① 用户手动操作（拖波形 / 缩放 / 滚轮）后暂停跟随 2 秒；
// ② 播放头远在画面之外（超一整个屏）→ 不追，绝不把用户的视野抢回来；
// ③ 唯一例外：相邻两帧的【谱面位置】差 > kHostJumpSec，即宿主自己跳转了
//    （点小节 / 循环回卷 / 拖播放头）—— 那种情况用户正等着画面跟过去。
const double kHostJumpSec      = 0.25;
const double kUserScrollHoldSec = 2.0;

//---- 新载入音频的默认视野 ----------------------------------------------------
// 不默认「整曲全览」：全览时波峰糊成一片，看不清鼓点，用户每次都得先手动放大
// 好多下。默认直接给 8 秒视野（≈4 小节 @120BPM 4/4），一打开就能看清对齐状况。
// 曲长不足 8 秒时回落到全览（见 afterLoad）。三端同名同值。
const double kDefaultViewSpanSec = 8.0;

// 拍号选项（与 macOS / Windows 端一致）
struct TimeSig { const char* label; int beats; int denom; };
const TimeSig kTimeSigs[] = {
    { "4/4",  4, 4 }, { "3/4",  3, 4 }, { "2/4",  2, 4 }, { "5/4",  5, 4 },
    { "6/8",  6, 8 }, { "7/8",  7, 8 }, { "9/8",  9, 8 }, { "12/8", 12, 8 },
};
const int kTimeSigCount = static_cast<int> (sizeof (kTimeSigs) / sizeof (kTimeSigs[0]));

//------------------------------------------------------------------------------
std::string formatTime (double sec)
{
    if (sec < 0.0)
        sec = 0.0;
    const int total = static_cast<int> (sec + 0.5);
    char buf[32];
    std::snprintf (buf, sizeof buf, "%d:%02d", total / 60, total % 60);
    return std::string (buf);
}

double nowSec ()
{
    struct timeval tv {};
    ::gettimeofday (&tv, nullptr);
    return static_cast<double> (tv.tv_sec) + static_cast<double> (tv.tv_usec) / 1e6;
}

// file:///path/to/x.mp3 → /path/to/x.mp3（含 %XX 反转义）
std::string uriToPath (const std::string& uri)
{
    std::string s = uri;
    // 去掉行尾 \r
    while (!s.empty () && (s.back () == '\r' || s.back () == '\n'))
        s.pop_back ();

    const std::string prefix = "file://";
    if (s.compare (0, prefix.size (), prefix) == 0)
        s = s.substr (prefix.size ());

    std::string out;
    out.reserve (s.size ());
    for (size_t i = 0; i < s.size (); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size () &&
            std::isxdigit (static_cast<unsigned char> (s[i + 1])) &&
            std::isxdigit (static_cast<unsigned char> (s[i + 2])))
        {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
                return c - 'a' + 10;
            };
            out.push_back (static_cast<char> (hex (s[i + 1]) * 16 + hex (s[i + 2])));
            i += 2;
        }
        else
        {
            out.push_back (s[i]);
        }
    }
    return out;
}

bool isAudioFile (const std::string& name)
{
    const size_t dot = name.find_last_of ('.');
    if (dot == std::string::npos)
        return false;
    std::string e = name.substr (dot);
    for (char& c : e)
        c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return e == ".mp3" || e == ".wav" || e == ".wave" || e == ".flac"
        || e == ".ogg" || e == ".oga";
}

} // namespace

namespace ap {

//==============================================================================
// X11 视图
//==============================================================================
class X11View
{
public:
    explicit X11View (PlugView::Backend* backend) : m_backend (backend)
    {
        // ⭐ 视图是「关掉编辑器再打开」时重建的，而拍号存在后端（Processor）里。
        //    不回读就会出现「网格按 12/8 画、按钮上却写着 4/4」，而且点一下会从
        //    错误的项开始循环。BPM 不需要回读 —— 显示值是每次绘制时算的。
        syncTimeSigFromBackend ();

        // ⭐ 视野也要回读：关掉编辑器再打开时后端里还存着「上次放大到多少、
        //    正看着哪一段」，不回读就退回默认视野（用户实测反馈的就是这个）。
        restoreViewState ();
    }
    ~X11View () { destroy (); }

    bool create (Window parent, int w, int h);
    void destroy ();
    void resize (int w, int h);

    Window window () const { return m_win; }

    // 当前面板尺寸。getSize 必须如实汇报它 —— 窗口可以被用户拖大，宿主随时会来
    // 问「你现在多大」；这时若还报「默认尺寸」，宿主会照着旧尺寸折腾窗口，用户
    // 刚拖好的宽度就被弹回去。
    int panelW () const { return m_w; }
    int panelH () const { return m_h; }

private:
    //---- 线程 ----------------------------------------------------------
    static void* threadEntry (void* self);
    void runLoop ();

    //---- 绘制 ----------------------------------------------------------
    void paint ();
    void drawWaveArea (XftDraw* xd);
    void drawControls (XftDraw* xd);
    void drawBrowser (XftDraw* xd);
    void drawHelp (XftDraw* xd);

    void fillRect (Drawable d, const Rect& r, unsigned long pixel);
    void frameRect (Drawable d, const Rect& r, unsigned long pixel);
    void drawText (XftDraw* xd, int x, int baseline, const char* utf8,
                   XftColor* color, XftFont* font);
    void drawTextCentered (XftDraw* xd, const Rect& r, const char* utf8,
                           XftColor* color, XftFont* font);

    void button (XftDraw* xd, const Rect& r, const char* label, int id);
    void checkbox (XftDraw* xd, const Rect& r, const char* label, bool on, int id);

    //---- 坐标换算 ------------------------------------------------------
    double xToSec (int x) const;
    int    secToX (double sec) const;

    //---- 交互 ----------------------------------------------------------
    void onButtonPress (int x, int y, unsigned state);
    void onButtonRelease (int x, int y, unsigned state);
    void onMotion (int x, int y, unsigned state);
    void onScroll (int x, int y, int dir, unsigned state, bool horizontal = false);

    void actionFor (int id);
    void zoomBy (double factor, double centerSec);
    void zoomFit ();
    /// 把视野挪到 sec（播放头落在画面 25% 处）。用于「回到播放头」。
    void centerViewOn (double sec);
    /// 按当前窗口尺寸重算所有控件矩形（面板可由宿主/用户调整大小）。
    void layoutAll ();
    //---- 视野自动跟随（规则见 kHostJumpSec 处的说明）----
    void markUserScrolling ();
    void recoverAutoFollow (double now);
    /// 返回值 = 视野是否真的动了（动了就要重绘）。
    bool followPlayheadTo (double audioSec, bool allowJump);
    /// 显示/隐藏使用指南覆盖层。
    void toggleHelp ();
    void clampView ();
    void afterLoad ();
    /// ⭐ 视图重建后把后端里存的视野读回来（见实现处的说明）
    void restoreViewState ();
    /// 新载入音频的默认视野（afterLoad 与 restoreViewState 共用，避免两份规则漂移）
    void applyDefaultView ();
    /// 从后端回读拍号并选中对应项（见构造处的说明）
    void syncTimeSigFromBackend ();
    void refreshCache ();

    //---- 拖放（XDND）---------------------------------------------------
    void setupDnd ();
    void handleDndClientMessage (const XClientMessageEvent& cm);
    void handleSelectionNotify (const XSelectionEvent& se);
    void sendDndStatus (Window source, bool accept);
    void finishDnd (Window source, bool ok);

    //---- 文件浏览器 ----------------------------------------------------
    void browserOpen ();
    void browserClick (int x, int y);
    void browserScroll (int dir);
    void browserEnter (const std::string& path);

    //---- X 资源 --------------------------------------------------------
    Display* m_dpy = nullptr;
    int      m_screen = 0;
    Window   m_win = 0;
    GC       m_gc = nullptr;
    Pixmap   m_pixmap = 0;
    XftDraw* m_xftDraw = nullptr;

    std::vector<unsigned long> m_px;      ///< 颜色像素表（下标 = 上面 RGB8 的顺序）
    std::vector<XftColor>      m_xftCol;

    XftFont* m_font = nullptr;
    XftFont* m_fontSmall = nullptr;
    XftFont* m_fontBold = nullptr;

    PlugView::Backend* m_backend = nullptr;

    //---- 状态 ----------------------------------------------------------
    int m_w = kPanelW;
    int m_h = kPanelH;

    double m_viewStart = 0.0;   ///< 视野起点（秒）
    double m_viewSpan  = 8.0;   ///< 视野跨度（秒）

    //---- 视野状态回存（关掉编辑器再打开要恢复到同一段视野）----
    // 视图每次打开都是新建的，视野得存在后端（Processor）里；runLoop 里发现视野
    // 变了就写回。改动入口很多（滚轮/拖动/缩放/全览/回到播放头/自动跟随），集中
    // 在一处比对才不会漏（见 MEMORY 铁律 9）。-1 = 还没写过。
    double m_pushedViewStart = -1.0;
    double m_pushedViewSpan  = -1.0;

    //---- 视野自动跟随（规则与 macOS / Windows 端一致）----
    bool   m_followScroll = true;     ///< 总开关（现阶段恒为 true）
    bool   m_userScrolling = false;   ///< 用户手动操作期间暂停跟随
    double m_lastUserScrollAt = 0.0;  ///< 用户最后一次手动操作的时刻（做超时恢复）
    bool   m_hasPrevPlayhead = false; ///< 是否已有上一帧谱面位置（判断宿主跳变）
    double m_prevPlayheadSec = 0.0;

    //---- 「点完『回到播放头』仍错位」检测（渲染循环维护，paint 只读）----
    double m_lastBackAt = -1.0e9;       ///< 上次点「回到播放头」的时刻（nowSec）
    bool   m_followFail = false;        ///< 是否处于「点了按钮仍错位」状态
    double m_followFailLoggedAt = -1.0e9;  ///< 上次为此写日志的时刻（做冷却）

    bool m_dragging = false;      ///< 波形区按住鼠标中
    bool m_dragOffset = false;    ///< true = 改起始偏移；false = 平移视野
    int  m_dragStartX = 0;
    double m_dragStartVal = 0.0;  ///< 拖动开始时被改的量（改偏移时 = offset，平移时 = viewStart）

    bool m_dragVolume = false;

    bool m_showGrid = true;
    bool m_showNumbers = true;
    int  m_timeSigIndex = 0;

    /// 使用指南覆盖层是否可见。Linux 端整个界面都画在离屏 pixmap 上，
    /// 所以直接在最上层再画一遍就行，不需要 Windows 那种独立子窗口。
    bool m_helpVisible = false;

    int  m_hotId = 0;           ///< 鼠标悬停控件
    int  m_pressedId = 0;       ///< 按下的控件
    bool m_dirty = true;        ///< 需要重绘

    // 缓存（避免每帧重复取全局状态）
    double m_posSec = 0.0;
    double m_durSec = 0.0;
    std::string m_timeText = "0:00 / 0:00";
    std::string m_offsetText = "偏移 +0.00 秒";
    std::string m_volText = "100 %";
    std::string m_fileText = "未载入音频";
    std::string m_statusText = "○ 未载入";
    std::string m_srcText = "默认 120";
    std::string m_errText;

    PlugView::Backend::HostTimeline m_tl;

    //---- 文件浏览器 ----------------------------------------------------
    bool m_browserOpen = false;
    std::string m_browserDir;
    struct Entry { std::string name; bool isDir; };
    std::vector<Entry> m_entries;
    int m_browserScroll = 0;

    //---- 线程 ----------------------------------------------------------
    pthread_t m_thread = 0;
    bool m_threadRunning = false;
    volatile bool m_stopThread = false;

    //---- XDND ----------------------------------------------------------
    Atom m_aXdndAware = 0, m_aXdndEnter = 0, m_aXdndPosition = 0, m_aXdndStatus = 0;
    Atom m_aXdndDrop = 0, m_aXdndLeave = 0, m_aXdndFinished = 0;
    Atom m_aXdndSelection = 0, m_aXdndTypeList = 0, m_aTextUriList = 0;
    Atom m_aXdndActionCopy = 0;
    Window m_dndSource = 0;
    bool m_dndPending = false;

    // 颜色槽位（顺序必须与 create() 里填表的顺序一致）
    enum Col
    {
        CBg, CWaveBg, CWaveLine, CBarLine, CBeatLine, CNumText,
        CScoreHead, CAudioHead, COffsetSh, CText, CTextDim,
        CBtnFace, CBtnEdge, CBtnDown, CBtnHot, CCheckOn,
        CBiliPink, CWhite, CCount
    };
    unsigned long px (Col c) const { return m_px[static_cast<size_t> (c)]; }
    XftColor* xc (Col c) { return &m_xftCol[static_cast<size_t> (c)]; }

    // 控件 ID
    enum
    {
        ID_NONE = 0,
        ID_OPEN, ID_ZOOM_OUT, ID_ZOOM_IN, ID_ZOOM_FIT, ID_BACK, ID_HELP,
        ID_OFFSET_ZERO,        // 「归零」：把起始偏移一键清零
        ID_GRID, ID_NUM, ID_BPM_BOX, ID_BPM_UP, ID_BPM_DOWN,
        ID_TIMESIG, ID_VOLUME,
        ID_BROWSER_UP, ID_BROWSER_CANCEL, ID_BROWSER_ITEM
    };
};

//------------------------------------------------------------------------------
// X 资源初始化
//------------------------------------------------------------------------------
bool X11View::create (Window parent, int w, int h)
{
    // 多线程访问 Xlib 前必须先初始化（进程内幂等）
    ::XInitThreads ();

    m_dpy = ::XOpenDisplay (nullptr);
    if (!m_dpy)
        return false;

    m_screen = DefaultScreen (m_dpy);
    m_w = w > 0 ? w : kPanelW;
    m_h = h > 0 ? h : kPanelH;
    if (m_w < kPanelWMin) m_w = kPanelWMin;
    if (m_h < kPanelHMin) m_h = kPanelHMin;
    if (m_w > kPanelWMax) m_w = kPanelWMax;
    if (m_h > kPanelHMax) m_h = kPanelHMax;
    layoutAll ();      // 先按窗口尺寸把控件矩形算好，后面绘制/命中都用它

    const unsigned long black = BlackPixel (m_dpy, m_screen);
    m_win = ::XCreateSimpleWindow (m_dpy, parent, 0, 0,
                                   static_cast<unsigned> (m_w),
                                   static_cast<unsigned> (m_h),
                                   0, black, black);
    if (!m_win)
        return false;

    ::XSelectInput (m_dpy, m_win,
                    ExposureMask | ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask | StructureNotifyMask |
                    EnterWindowMask | LeaveWindowMask | KeyPressMask);

    m_gc = ::XCreateGC (m_dpy, m_win, 0, nullptr);

    // 颜色表
    static const RGB8* const kPalette[] = {
        &kBg, &kWaveBg, &kWaveLine, &kBarLine, &kBeatLine, &kNumText,
        &kScoreHead, &kAudioHead, &kOffsetSh, &kText, &kTextDim,
        &kBtnFace, &kBtnEdge, &kBtnDown, &kBtnHot, &kCheckOn,
        &kBiliPink, &kWhite
    };
    const Colormap cmap = DefaultColormap (m_dpy, m_screen);
    // 注意：Xft 的这几个 API 要的是非 const Visual*，不能写成 const 引用
    Visual* vis = DefaultVisual (m_dpy, m_screen);
    m_px.resize (CCount);
    m_xftCol.resize (CCount);
    for (int i = 0; i < CCount; ++i)
    {
        const RGB8& c = *kPalette[i];

        XColor xc8 {};
        xc8.red   = static_cast<unsigned short> (c.r * 257);
        xc8.green = static_cast<unsigned short> (c.g * 257);
        xc8.blue  = static_cast<unsigned short> (c.b * 257);
        xc8.flags = DoRed | DoGreen | DoBlue;
        m_px[static_cast<size_t> (i)] =
            ::XAllocColor (m_dpy, cmap, &xc8) ? xc8.pixel : black;

        XRenderColor rc {};
        rc.red   = static_cast<unsigned short> (c.r * 257);
        rc.green = static_cast<unsigned short> (c.g * 257);
        rc.blue  = static_cast<unsigned short> (c.b * 257);
        rc.alpha = 0xffff;
        ::XftColorAllocValue (m_dpy, vis, cmap, &rc, &m_xftCol[static_cast<size_t> (i)]);
    }

    // 字体：优先能显示中文的，逐级回退
    static const char* kFontPatterns[] = {
        "Noto Sans CJK SC-10",
        "Source Han Sans SC-10",
        "WenQuanYi Micro Hei-10",
        "WenQuanYi Zen Hei-10",
        "sans-10",
        "fixed-10"
    };
    for (const char* pat : kFontPatterns)
    {
        XftFont* f = ::XftFontOpenName (m_dpy, m_screen, pat);
        if (f) { m_font = f; break; }
    }
    if (!m_font)
        m_font = ::XftFontOpenName (m_dpy, m_screen, "*");

    static const char* kSmallPatterns[] = {
        "Noto Sans CJK SC-8", "WenQuanYi Micro Hei-8", "sans-8", "fixed-8"
    };
    for (const char* pat : kSmallPatterns)
    {
        XftFont* f = ::XftFontOpenName (m_dpy, m_screen, pat);
        if (f) { m_fontSmall = f; break; }
    }
    if (!m_fontSmall)
        m_fontSmall = m_font;

    static const char* kBoldPatterns[] = {
        "Noto Sans CJK SC:bold-11", "WenQuanYi Micro Hei:bold-11",
        "sans:bold-11", "sans-11"
    };
    for (const char* pat : kBoldPatterns)
    {
        XftFont* f = ::XftFontOpenName (m_dpy, m_screen, pat);
        if (f) { m_fontBold = f; break; }
    }
    if (!m_fontBold)
        m_fontBold = m_font;

    // 离屏缓冲
    m_pixmap = ::XCreatePixmap (m_dpy, m_win,
                                static_cast<unsigned> (m_w),
                                static_cast<unsigned> (m_h),
                                static_cast<unsigned> (DefaultDepth (m_dpy, m_screen)));
    m_xftDraw = ::XftDrawCreate (m_dpy, m_pixmap, vis, cmap);

    setupDnd ();

    ::XMapWindow (m_dpy, m_win);
    ::XFlush (m_dpy);

    // 后台线程：事件 + 定时重绘
    m_stopThread = false;
    if (::pthread_create (&m_thread, nullptr, &X11View::threadEntry, this) == 0)
        m_threadRunning = true;

    return true;
}

void X11View::destroy ()
{
    if (!m_dpy)
        return;

    if (m_threadRunning)
    {
        m_stopThread = true;
        ::pthread_join (m_thread, nullptr);
        m_threadRunning = false;
        m_thread = 0;
    }

    ::XLockDisplay (m_dpy);

    if (m_xftDraw) { ::XftDrawDestroy (m_xftDraw); m_xftDraw = nullptr; }
    if (m_pixmap)  { ::XFreePixmap (m_dpy, m_pixmap); m_pixmap = 0; }

    for (XftColor& c : m_xftCol)
        ::XftColorFree (m_dpy, DefaultVisual (m_dpy, m_screen),
                        DefaultColormap (m_dpy, m_screen), &c);
    m_xftCol.clear ();

    // 字体打不开时会回退成同一个指针，关闭前必须先排除别名，否则重复释放
    XftFont* fSmall = (m_fontSmall == m_font) ? nullptr : m_fontSmall;
    XftFont* fBold  = (m_fontBold == m_font || m_fontBold == m_fontSmall) ? nullptr : m_fontBold;
    if (m_font) ::XftFontClose (m_dpy, m_font);
    if (fSmall) ::XftFontClose (m_dpy, fSmall);
    if (fBold)  ::XftFontClose (m_dpy, fBold);
    m_font = m_fontSmall = m_fontBold = nullptr;

    if (m_gc)  { ::XFreeGC (m_dpy, m_gc); m_gc = nullptr; }
    if (m_win) { ::XDestroyWindow (m_dpy, m_win); m_win = 0; }

    ::XFlush (m_dpy);
    ::XUnlockDisplay (m_dpy);

    ::XCloseDisplay (m_dpy);
    m_dpy = nullptr;
}

void X11View::resize (int w, int h)
{
    if (!m_dpy || !m_win || w <= 0 || h <= 0)
        return;

    // 与 create() 一致地钳到 [min, max]：宿主/用户把窗口拖到 0 或荒唐尺寸时，
    // 布局按下限算（多余部分被裁），总比控件叠在一起好。
    if (w < kPanelWMin) w = kPanelWMin;
    if (h < kPanelHMin) h = kPanelHMin;
    if (w > kPanelWMax) w = kPanelWMax;
    if (h > kPanelHMax) h = kPanelHMax;

    ::XLockDisplay (m_dpy);
    m_w = w;
    m_h = h;
    layoutAll ();
    ::XResizeWindow (m_dpy, m_win, static_cast<unsigned> (w), static_cast<unsigned> (h));

    if (m_pixmap)
        ::XFreePixmap (m_dpy, m_pixmap);
    m_pixmap = ::XCreatePixmap (m_dpy, m_win, static_cast<unsigned> (w),
                                static_cast<unsigned> (h),
                                static_cast<unsigned> (DefaultDepth (m_dpy, m_screen)));
    if (m_xftDraw)
        ::XftDrawChange (m_xftDraw, m_pixmap);

    m_dirty = true;
    ::XFlush (m_dpy);
    ::XUnlockDisplay (m_dpy);
}

//------------------------------------------------------------------------------
// 布局：按【当前窗口尺寸】重算所有控件矩形
//
// 与 Windows 端 layoutChildren() 是同一套几何（一一对应，便于对照维护）：
//   · 宽度方向：多数控件靠左固定；右侧那几个（打开音频 / 回到播放头 / 归零 /
//     音量百分比 / 支持的格式）挂在「右边界 - 10」上，中间留白自动分配
//     → 窗口拖大时【波形区跟着变宽】。
//   · 高度方向：波形区吃掉上方全部余量，底部那一段（时间 / 缩放 / 网格 / BPM /
//     音量 / 文件 / 状态 / 署名）保持固定高度并贴着面板底部。
//   ⚠️ 底部区块里所有控件的 y 都写成「bottomTop + 偏移」，不能写绝对值 ——
//      面板一旦被拉高，绝对值就飞了。
//   ⚠️ 面板被宿主强行缩到比下限还小时，按【下限尺寸】排布（多余部分被裁）。
//   下面括号里的数字 = 340×384 下算出来的结果，与旧版固定布局逐点吻合。
//------------------------------------------------------------------------------
void X11View::layoutAll ()
{
    const int pw  = std::max (m_w, kPanelWMin);
    const int ph  = std::max (m_h, kPanelHMin);
    const int pad = 10;
    const int bottomTop = ph - 226;          // 384-226 = 158

    // ---- 波形区：宽吃掉左右各 10 外的全部，高吃掉上下之间的一切 ----
    kWaveX = pad;                            // 10
    kWaveY = 34;
    kWaveW = std::max (320, pw - pad * 2);   // 320
    kWaveH = std::max (118, bottomTop - 34 - 6);   // 118

    // ---- 顶栏（右对齐）----
    rOpenBtn = Rect { pw - pad - 90, 6, 90, 24 };                    // 240
    rHelpBtn = Rect { pw - pad - 90 - 6 - 62, 6, 62, 24 };           // 172

    // ---- 时间 / 缩放 / 回到播放头 ----
    rTime    = Rect { 10, bottomTop, 92, 18 };                       // 158
    rZoomOut = Rect { 104, bottomTop - 3, 38, 22 };                  // 155
    rZoomIn  = Rect { 144, bottomTop - 3, 38, 22 };
    rZoomFit = Rect { 184, bottomTop - 3, 38, 22 };
    rBack    = Rect { pw - pad - 80, bottomTop - 3, 80, 22 };        // 250（旧版 224）

    // ---- 网格 / 小节号 / 偏移（+ 归零）----
    rGridChk = Rect { 10, bottomTop + 24, 62, 20 };                  // 182
    rNumChk  = Rect { 74, bottomTop + 24, 72, 20 };
    const int zeroW = 56;
    const int zeroX = pw - pad - zeroW;
    rOffsetZero = Rect { zeroX, bottomTop + 24, zeroW, 20 };         // 274
    rOffset = Rect { 150, bottomTop + 24, zeroX - 4 - 150, 18 };     // 150..270

    rHint = Rect { 10, bottomTop + 46, pw - pad * 2, 18 };           // 204

    // ---- 速度 / 拍号 ----
    rBpmBox  = Rect { 46, bottomTop + 68, 44, 22 };                  // 226
    rBpmUp   = Rect { 91, bottomTop + 68, 18, 11 };
    rBpmDown = Rect { 91, bottomTop + 79, 18, 11 };                  // 237
    rTimeSig = Rect { 150, bottomTop + 66, 70, 22 };                 // 224
    rSrc     = Rect { 228, bottomTop + 68, pw - pad - 228, 18 };     // 228..330

    // ---- 音量 ----
    const int volLabelX = pw - pad - 52;
    rVolLabel = Rect { volLabelX, bottomTop + 96, 52, 18 };          // 278（旧版 238）
    rVolume   = Rect { 46, bottomTop + 94, volLabelX - 6 - 46, 24 }; // 46..272

    // ---- 文件名 / 状态 / 支持格式 ----
    rFile   = Rect { 10, bottomTop + 122, pw - pad * 2, 18 };        // 280
    rStatus = Rect { 10, bottomTop + 142, 130, 18 };                 // 300
    rFmt    = Rect { 140, bottomTop + 142, pw - pad - 140, 18 };     // 140..330

    // ---- 页脚署名（贴面板底）----
    rBrand = Rect { 10, ph - 26, pw - pad * 2, 18 };                 // 358
}

//------------------------------------------------------------------------------
// 后台线程：事件 + 重绘
//------------------------------------------------------------------------------
void* X11View::threadEntry (void* self)
{
    static_cast<X11View*> (self)->runLoop ();
    return nullptr;
}

void X11View::runLoop ()
{
    double lastPaint = 0.0;

    while (!m_stopThread)
    {
        bool needsPaint = false;

        ::XLockDisplay (m_dpy);

        while (m_dpy && ::XPending (m_dpy) > 0)
        {
            XEvent ev {};
            ::XNextEvent (m_dpy, &ev);
            needsPaint = true;

            switch (ev.type)
            {
                case Expose:
                    m_dirty = true;
                    break;
                case ConfigureNotify:
                    if (ev.xconfigure.width != m_w || ev.xconfigure.height != m_h)
                    {
                        // 此处已持有 X 锁，不能调 resize()（它会再加锁 → 死锁），就地处理
                        m_w = ev.xconfigure.width;
                        m_h = ev.xconfigure.height;
                        if (m_w < kPanelWMin) m_w = kPanelWMin;
                        if (m_h < kPanelHMin) m_h = kPanelHMin;
                        if (m_w > kPanelWMax) m_w = kPanelWMax;
                        if (m_h > kPanelHMax) m_h = kPanelHMax;
                        layoutAll ();   // 控件矩形随窗口尺寸重算
                        if (m_pixmap)
                            ::XFreePixmap (m_dpy, m_pixmap);
                        m_pixmap = ::XCreatePixmap (m_dpy, m_win,
                                                    static_cast<unsigned> (m_w),
                                                    static_cast<unsigned> (m_h),
                                                    static_cast<unsigned> (DefaultDepth (m_dpy, m_screen)));
                        if (m_xftDraw)
                            ::XftDrawChange (m_xftDraw, m_pixmap);
                        m_dirty = true;
                    }
                    break;
                case ButtonPress:
                    // X11 的滚轮就是 Button4 / Button5；横向滚轮（触控板左右滑、
                    // 部分鼠标的横滚轮）是 Button6 / Button7。
                    // ⚠️ `X.h` 只定义 `Button1`~`Button5`（没有 Button6/Button7 这两个宏，
                    //    写了会直接编译失败）→ 横向的只能用字面量 6 / 7。
                    if (ev.xbutton.button == Button4)
                        onScroll (ev.xbutton.x, ev.xbutton.y,  1, ev.xbutton.state);
                    else if (ev.xbutton.button == Button5)
                        onScroll (ev.xbutton.x, ev.xbutton.y, -1, ev.xbutton.state);
                    else if (ev.xbutton.button == 6)
                        onScroll (ev.xbutton.x, ev.xbutton.y, -1, ev.xbutton.state, true);
                    else if (ev.xbutton.button == 7)
                        onScroll (ev.xbutton.x, ev.xbutton.y,  1, ev.xbutton.state, true);
                    else
                        onButtonPress (ev.xbutton.x, ev.xbutton.y, ev.xbutton.state);
                    break;
                case ButtonRelease:
                    onButtonRelease (ev.xbutton.x, ev.xbutton.y, ev.xbutton.state);
                    break;
                case MotionNotify:
                    onMotion (ev.xmotion.x, ev.xmotion.y, ev.xmotion.state);
                    break;
                case KeyPress:
                {
                    const KeySym ks = ::XLookupKeysym (&ev.xkey, 0);
                    if (ks == XK_Escape && m_browserOpen)
                    {
                        m_browserOpen = false;
                        m_dirty = true;
                    }
                    break;
                }
                case ClientMessage:
                    if (ev.xclient.message_type == m_aXdndEnter ||
                        ev.xclient.message_type == m_aXdndPosition ||
                        ev.xclient.message_type == m_aXdndDrop ||
                        ev.xclient.message_type == m_aXdndLeave)
                        handleDndClientMessage (ev.xclient);
                    break;
                case SelectionNotify:
                    handleSelectionNotify (ev.xselection);
                    break;
                default:
                    break;
            }
        }

        ::XUnlockDisplay (m_dpy);

        // 正在播放时固定帧率刷新播放头
        const double t = nowSec ();

        // ---- 「点完『回到播放头』仍然错位」= 跟随本身失效 ----
        // 正常点一下就对齐了。若 3 秒内又错位，说明有某个跟随条件持续为假 ——
        // 徽标文案升级为「建议重启插件」，并记一条日志（带冷却）。
        // 判据与波形区徽标一致。放在 XUnlockDisplay 之后：写文件不涉及 X 锁。
        {
            const double audioNow = m_posSec;
            const double durNow   = m_durSec;
            const double offNow   = m_backend
                                        ? static_cast<double> (m_backend->offsetSec ())
                                        : 0.0;
            const double diffNow  = audioNow - (m_tl.playheadSec + offNow);
            const bool diverging =
                m_backend && m_tl.playing && m_tl.playheadValid && durNow > 0.60
                && audioNow > 0.30 && audioNow < durNow - 0.30
                && std::fabs (diffNow) > kDivergenceWarnSec;

            if (diverging && (t - m_lastBackAt) < kFollowFailWindowSec)
            {
                m_followFail = true;
                if ((t - m_followFailLoggedAt) > kFollowFailLogGapSec)
                {
                    m_followFailLoggedAt = t;
                    ap::crashLog ("跟随可能已失效：点「回到播放头」后 %.1f 秒仍错位 %.2f 秒"
                                  "（音频 %.3f / 谱面 %.3f + 偏移 %.3f）",
                                  t - m_lastBackAt, std::fabs (diffNow),
                                  audioNow, m_tl.playheadSec, offNow);
                }
            }
            else
            {
                m_followFail = false;
            }
        }

        // ---- 视野自动跟随：宿主播放时，波形视野自动滚到谱面播放头 ----
        // ⭐ 先区分「播放头自己在走」和「宿主把它挪走了」：前者画面只许顺滑滚，
        //    后者画面该跟过去。本循环约 30Hz，正常前进每帧只有几十毫秒；超过
        //    kHostJumpSec 就是宿主跳变（点小节 / 循环回卷 / 拖播放头）。
        //    本端过去完全没有这段 —— 用户实测「回到播放头后播放头直接跑到画面外」。
        bool hostJumped = false;
        if (m_tl.playheadValid)
        {
            if (m_hasPrevPlayhead &&
                std::fabs (m_tl.playheadSec - m_prevPlayheadSec) > kHostJumpSec)
                hostJumped = true;
            m_prevPlayheadSec = m_tl.playheadSec;
            m_hasPrevPlayhead = true;
        }

        // 用户停止手动操作超时后自动恢复跟随（播放 / 暂停都生效）。
        recoverAutoFollow (t);

        // ⭐ 宿主自己跳的（点小节 / 循环回卷 / 拖播放头）就算停在【暂停】状态也要
        //    把画面带过去 —— 扒谱就是在暂停下一个小节一个小节点的。以前这里写死了
        //    `m_tl.playing`，于是暂停时换位置画面纹丝不动。自然播放推进
        //    （allowJump=false）时仍然只许顺滑滚，绝不抢用户的视野。
        if (m_backend && m_tl.playheadValid && (m_tl.playing || hostJumped))
        {
            const double audioT = m_tl.playheadSec + m_backend->offsetSec ();
            if (followPlayheadTo (audioT, hostJumped))
                m_dirty = true;      // 视野动了才重绘，不动就别白画
        }

        // ---- 视野状态回存：关掉编辑器再打开要回到同一段视野 ----
        // ⭐ 集中在这里比对是刻意的：视野的改动入口很多（滚轮 / 拖动 / 缩放 /
        //    全览 / 回到播放头 / 自动跟随），逐个入口去写必然漏一个（见铁律 9）。
        // ⚠️ 必须在 XUnlockDisplay 之后调用 —— setViewState 会拿后端的小锁，
        //    不要在持有 X 显示锁时再去拿别的锁（本端对锁序历来敏感）。
        if (m_backend && m_backend->hasAudio () && m_viewSpan > 0.0
            && (std::fabs (m_viewStart - m_pushedViewStart) > 1e-6
             || std::fabs (m_viewSpan - m_pushedViewSpan) > 1e-6))
        {
            m_pushedViewStart = m_viewStart;
            m_pushedViewSpan  = m_viewSpan;
            m_backend->setViewState (m_viewStart, m_viewSpan);
        }

        if (m_backend && m_backend->hasAudio () && m_tl.playing)
        {
            if (t - lastPaint > 1.0 / 30.0)
            {
                m_dirty = true;
                lastPaint = t;
            }
        }

        if ((needsPaint || m_dirty) && !m_stopThread)
        {
            ::XLockDisplay (m_dpy);
            if (m_dirty)
            {
                refreshCache ();
                paint ();
                m_dirty = false;
            }
            ::XUnlockDisplay (m_dpy);
        }

        ::usleep (8000);   // 8ms：够 30~60Hz，又不空转
    }
}

//------------------------------------------------------------------------------
// 绘制辅助
//------------------------------------------------------------------------------
void X11View::fillRect (Drawable d, const Rect& r, unsigned long pixel)
{
    ::XSetForeground (m_dpy, m_gc, pixel);
    ::XFillRectangle (m_dpy, d, m_gc, r.x, r.y,
                      static_cast<unsigned> (r.w), static_cast<unsigned> (r.h));
}

void X11View::frameRect (Drawable d, const Rect& r, unsigned long pixel)
{
    ::XSetForeground (m_dpy, m_gc, pixel);
    ::XDrawRectangle (m_dpy, d, m_gc, r.x, r.y,
                      static_cast<unsigned> (r.w - 1), static_cast<unsigned> (r.h - 1));
}

void X11View::drawText (XftDraw* xd, int x, int baseline, const char* utf8,
                        XftColor* color, XftFont* font)
{
    if (!utf8 || !*utf8)
        return;
    ::XftDrawStringUtf8 (xd, color, font ? font : m_font, x, baseline,
                         reinterpret_cast<const FcChar8*> (utf8),
                         static_cast<int> (std::strlen (utf8)));
}

void X11View::drawTextCentered (XftDraw* xd, const Rect& r, const char* utf8,
                                XftColor* color, XftFont* font)
{
    XftFont* f = font ? font : m_font;
    XGlyphInfo gi {};
    ::XftTextExtentsUtf8 (m_dpy, f, reinterpret_cast<const FcChar8*> (utf8),
                          static_cast<int> (std::strlen (utf8)), &gi);
    const int x = r.x + (r.w - gi.xOff) / 2;
    const int baseline = r.y + (r.h + f->ascent - f->descent) / 2;
    drawText (xd, x, baseline, utf8, color, f);
}

void X11View::button (XftDraw* xd, const Rect& r, const char* label, int id)
{
    const bool down = (m_pressedId == id);
    const bool hot = (m_hotId == id);
    fillRect (m_pixmap, r, down ? px (CBtnDown) : (hot ? px (CBtnHot) : px (CBtnFace)));
    frameRect (m_pixmap, r, px (CBtnEdge));
    drawTextCentered (xd, r, label, xc (CText), m_fontSmall);
}

void X11View::checkbox (XftDraw* xd, const Rect& r, const char* label, bool on, int id)
{
    const Rect box = { r.x, r.y + (r.h - 12) / 2, 12, 12 };
    fillRect (m_pixmap, box, px (CWhite));
    frameRect (m_pixmap, box, (m_hotId == id) ? px (CCheckOn) : px (CBtnEdge));
    if (on)
    {
        // 打勾：两段线
        ::XSetForeground (m_dpy, m_gc, px (CCheckOn));
        ::XSetLineAttributes (m_dpy, m_gc, 2, LineSolid, CapRound, JoinRound);
        ::XDrawLine (m_dpy, m_pixmap, m_gc, box.x + 2, box.y + 6, box.x + 5, box.y + 9);
        ::XDrawLine (m_dpy, m_pixmap, m_gc, box.x + 5, box.y + 9, box.x + 10, box.y + 3);
        ::XSetLineAttributes (m_dpy, m_gc, 1, LineSolid, CapButt, JoinMiter);
    }
    drawText (xd, box.x + 16, r.y + r.h - 4, label, xc (CText), m_fontSmall);
}

//------------------------------------------------------------------------------
// 坐标换算
//------------------------------------------------------------------------------
double X11View::xToSec (int x) const
{
    return m_viewStart + (static_cast<double> (x - kWaveX) / kWaveW) * m_viewSpan;
}

int X11View::secToX (double sec) const
{
    if (m_viewSpan <= 0.0)
        return kWaveX;
    return kWaveX + static_cast<int> ((sec - m_viewStart) / m_viewSpan * kWaveW);
}

//------------------------------------------------------------------------------
// 缓存后端状态（每帧一次，避免重复跨接口调用）
//------------------------------------------------------------------------------
void X11View::refreshCache ()
{
    if (!m_backend)
        return;

    m_posSec = m_backend->positionSec ();
    m_durSec = m_backend->durationSec ();
    m_tl = m_backend->hostTimeline ();

    m_timeText = formatTime (m_posSec) + " / " + formatTime (m_durSec);

    char buf[512];
    std::snprintf (buf, sizeof buf, "偏移 %+.2f 秒", m_backend->offsetSec ());
    m_offsetText = buf;

    std::snprintf (buf, sizeof buf, "%d %%",
                   static_cast<int> (m_backend->volume () * 100.0f + 0.5f));
    m_volText = buf;

    if (m_backend->gridBPM () > 0.0f)
        m_srcText = "手动";
    else if (m_tl.tempoValid)
        m_srcText = "谱面速度";
    else
        m_srcText = "默认 120";

    const std::string path = m_backend->currentPath ();
    if (!path.empty ())
    {
        const size_t slash = path.find_last_of ('/');
        const std::string name = (slash == std::string::npos) ? path : path.substr (slash + 1);
        m_fileText = "已载入：" + name;
    }
    else
    {
        m_fileText = "未载入音频";
    }

    if (!m_backend->hasAudio ())
        m_statusText = "○ 未载入";
    else
        m_statusText = m_tl.playing ? "● 跟随宿主播放" : "○ 宿主已暂停";

    m_errText = m_backend->lastError ();
}

//------------------------------------------------------------------------------
// 主绘制
//------------------------------------------------------------------------------
void X11View::paint ()
{
    if (!m_dpy || !m_pixmap)
        return;

    fillRect (m_pixmap, Rect { 0, 0, m_w, m_h }, px (CBg));

    if (m_browserOpen)
    {
        drawBrowser (m_xftDraw);
    }
    else
    {
        drawWaveArea (m_xftDraw);
        drawControls (m_xftDraw);
    }

    // 使用指南画在最上层：整个界面都在离屏 pixmap 上，直接再盖一层即可。
    if (m_helpVisible)
        drawHelp (m_xftDraw);

    ::XCopyArea (m_dpy, m_pixmap, m_win, m_gc, 0, 0,
                 static_cast<unsigned> (m_w), static_cast<unsigned> (m_h), 0, 0);
    ::XFlush (m_dpy);
}

//------------------------------------------------------------------------------
void X11View::drawWaveArea (XftDraw* xd)
{
    const Rect wave { kWaveX, kWaveY, kWaveW, kWaveH };
    fillRect (m_pixmap, wave, px (CWaveBg));

    if (!m_backend)
        return;

    if (!m_backend->hasAudio ())
    {
        drawTextCentered (xd, wave, "把音频文件拖到这里，或点右上角「打开音频」",
                          xc (CTextDim), m_font);
        return;
    }

    // ---- 波形 ----
    std::vector<float> peaks;
    m_backend->waveformPeaks (peaks, kWaveW, m_viewStart, m_viewStart + m_viewSpan);

    if (!peaks.empty ())
    {
        ::XSetForeground (m_dpy, m_gc, px (CWaveLine));
        const int mid = kWaveY + kWaveH / 2;
        const int n = static_cast<int> (peaks.size ());
        for (int i = 0; i < n && i < kWaveW; ++i)
        {
            float p = peaks[static_cast<size_t> (i)];
            if (p < 0.0f) p = 0.0f;
            if (p > 1.0f) p = 1.0f;
            const int half = static_cast<int> (p * (kWaveH / 2 - 2));
            const int x = kWaveX + i;
            ::XDrawLine (m_dpy, m_pixmap, m_gc, x, mid - half, x, mid + half + 1);
        }
    }

    const float offset = m_backend->offsetSec ();

    // ---- 小节网格 ----
    float bpm = m_backend->gridBPM ();
    if (bpm <= 0.0f && m_tl.tempoValid) bpm = m_tl.bpm;
    if (bpm <= 0.0f) bpm = 120.0f;

    int beats = m_backend->gridBeatsPerBar ();
    int denom = m_backend->gridBeatDenominator ();
    if (beats <= 0) beats = 4;
    if (denom <= 0) denom = 4;

    const double beatSec = (60.0 / bpm) * (4.0 / denom);
    const double barSec  = beatSec * beats;

    if (m_showGrid && beatSec > 1e-6)
    {
        ::XSetLineAttributes (m_dpy, m_gc, 1, LineSolid, CapButt, JoinMiter);

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
            if (x < kWaveX || x > kWaveX + kWaveW)
                continue;

            const long long beatIndex =
                static_cast<long long> (std::llround ((s - offset) / beatSec));
            const bool isBar = (beatIndex % beats) == 0;

            ::XSetForeground (m_dpy, m_gc, isBar ? px (CBarLine) : px (CBeatLine));
            ::XDrawLine (m_dpy, m_pixmap, m_gc, x, kWaveY, x, kWaveY + kWaveH);
        }

        if (m_showNumbers && barSec > 1e-6)
        {
            long long barNo = static_cast<long long> (std::floor ((m_viewStart - offset) / barSec));
            for (double b = offset + static_cast<double> (barNo) * barSec;
                 b <= m_viewStart + m_viewSpan; b += barSec, ++barNo)
            {
                if (b < m_viewStart)
                    continue;
                const int x = secToX (b);
                if (x < kWaveX - 20 || x > kWaveX + kWaveW)
                    continue;
                char buf[32];
                std::snprintf (buf, sizeof buf, "%lld", barNo + 1);
                drawText (xd, x + 2, kWaveY + 12, buf, xc (CNumText), m_fontSmall);
            }
        }
    }

    // ---- 谱面播放头（绿）----
    if (m_tl.playheadValid)
    {
        const int x = secToX (m_tl.playheadSec + offset);
        if (x >= kWaveX && x <= kWaveX + kWaveW)
        {
            ::XSetForeground (m_dpy, m_gc, px (CScoreHead));
            ::XSetLineAttributes (m_dpy, m_gc, 2, LineSolid, CapButt, JoinMiter);
            ::XDrawLine (m_dpy, m_pixmap, m_gc, x, kWaveY, x, kWaveY + kWaveH);
            ::XSetLineAttributes (m_dpy, m_gc, 1, LineSolid, CapButt, JoinMiter);
        }
    }

    // ---- 音频播放头（红）----
    {
        const int x = secToX (m_posSec);
        if (x >= kWaveX && x <= kWaveX + kWaveW)
        {
            ::XSetForeground (m_dpy, m_gc, px (CAudioHead));
            ::XSetLineAttributes (m_dpy, m_gc, 2, LineSolid, CapButt, JoinMiter);
            ::XDrawLine (m_dpy, m_pixmap, m_gc, x, kWaveY, x, kWaveY + kWaveH);
            ::XSetLineAttributes (m_dpy, m_gc, 1, LineSolid, CapButt, JoinMiter);
        }
    }

    // ---- 正偏移：左侧橙色阴影 = 被跳过的前奏 ----
    if (offset > m_viewStart)
    {
        const int x = secToX (offset);
        if (x > kWaveX)
        {
            const int right = std::min (x, kWaveX + kWaveW);
            const Rect sh { kWaveX, kWaveY, right - kWaveX, kWaveH };
            fillRect (m_pixmap, sh, px (COffsetSh));
        }
    }

    // ---- 分家检测徽标 ----
    // 只在「播放中、且不在首尾过渡区」判定：
    //   · 起播头 0.3 秒音频还在追，差值天然偏大 → 不算；
    //   · 音频比乐谱短时尾端必然拉开 → 不算（那不是 bug）。
    // 命中就亮一个红底徽标，把「用户肉眼看不出来的分家」变成一句明确指令。
    {
        const bool mid = (m_posSec > 0.30 && m_durSec > 0.60 && m_posSec < m_durSec - 0.30);
        if (m_tl.playing && m_tl.playheadValid && mid)
        {
            const double diff = m_posSec - (m_tl.playheadSec + offset);
            if (std::fabs (diff) > kDivergenceWarnSec)
            {
                // 刚点过「回到播放头」却还是错位 → 不是一次性偏差，而是跟随失效，
                // 此时再点按钮也没用，直接告诉用户重启插件（渲染循环里已记日志）。
                char warn[128];
                if (m_followFail)
                    std::snprintf (warn, sizeof warn,
                                   "跟随可能已失效 · 建议重启插件（错位 %.2f 秒）",
                                   std::fabs (diff));
                else
                    std::snprintf (warn, sizeof warn,
                                   "与谱面错位 %.2f 秒 · 点「回到播放头」",
                                   std::fabs (diff));

                XGlyphInfo ext {};
                ::XftTextExtentsUtf8 (m_dpy, m_fontSmall,
                                      reinterpret_cast<const FcChar8*> (warn),
                                      static_cast<int> (std::strlen (warn)), &ext);

                const int th = m_fontSmall->ascent + m_fontSmall->descent;
                const int bw = ext.xOff + 10;
                const int bh = th + 6;
                int bx = kWaveX + kWaveW - bw - 4;
                if (bx < kWaveX + 2)
                    bx = kWaveX + 2;

                const Rect box { bx, kWaveY + 3, bw, bh };
                fillRect (m_pixmap, box, px (CAudioHead));
                drawText (xd, box.x + 5, box.y + 3 + m_fontSmall->ascent,
                          warn, xc (CWhite), m_fontSmall);
            }
        }
    }
}

//------------------------------------------------------------------------------
void X11View::drawControls (XftDraw* xd)
{
    button (xd, rOpenBtn, "打开音频", ID_OPEN);
    // 「？帮助」：唤出使用指南覆盖层。有用户反馈不知道怎么用，
    // 说明书必须能在界面上直接点开，不能只躺在安装目录的 readme 里。
    button (xd, rHelpBtn, "？帮助", ID_HELP);

    drawText (xd, rTime.x, rTime.y + 14, m_timeText.c_str (), xc (CText), m_fontSmall);

    button (xd, rZoomOut, "缩小", ID_ZOOM_OUT);
    button (xd, rZoomIn,  "放大", ID_ZOOM_IN);
    button (xd, rZoomFit, "全览", ID_ZOOM_FIT);
    // 原名「回到谱面」→「回到播放头」：用户想干的是「把我送回播放位置」，
    // 新名字直说结果，不必先理解「谱面」和「音频」的关系。
    button (xd, rBack,    "回到播放头", ID_BACK);

    checkbox (xd, rGridChk, "网格",   m_showGrid,    ID_GRID);
    checkbox (xd, rNumChk,  "小节号", m_showNumbers, ID_NUM);

    drawText (xd, rOffset.x, rOffset.y + 14, m_offsetText.c_str (), xc (CText), m_fontSmall);

    // 「归零」：把起始偏移一键清零。
    // 为什么要它：改偏移的唯一入口是「Ctrl/Alt + 在波形上拖动」，一旦手滑拖到很大
    // 的值（或只想回到初始状态），就只能反向拖回去 —— 可能要拖好几个屏。
    // 用户实测明确提出「没有偏移归零的方式」，这里补上。三端同名同位置。
    button (xd, rOffsetZero, "归零", ID_OFFSET_ZERO);

    drawText (xd, rHint.x, rHint.y + 14,
              "拖动=平移视图　Ctrl/Alt拖动=改偏移　滚轮=平移　Ctrl/Alt滚轮=缩放　「全览」=整首",
              xc (CTextDim), m_fontSmall);

    // BPM
    {
        float bpm = m_backend ? m_backend->gridBPM () : 0.0f;
        if (bpm <= 0.0f && m_tl.tempoValid) bpm = m_tl.bpm;
        if (bpm <= 0.0f) bpm = 120.0f;

        fillRect (m_pixmap, rBpmBox, px (CWhite));
        frameRect (m_pixmap, rBpmBox, px (CBtnEdge));
        char buf[32];
        std::snprintf (buf, sizeof buf, "%d", static_cast<int> (bpm + 0.5f));
        drawTextCentered (xd, rBpmBox, buf, xc (CText), m_font);

        button (xd, rBpmUp,   "▲", ID_BPM_UP);
        button (xd, rBpmDown, "▼", ID_BPM_DOWN);

        drawText (xd, rBpmBox.x - 36, rBpmBox.y + 16, "BPM", xc (CTextDim), m_fontSmall);
    }

    button (xd, rTimeSig, kTimeSigs[m_timeSigIndex].label, ID_TIMESIG);
    drawText (xd, rSrc.x, rSrc.y + 14, m_srcText.c_str (), xc (CTextDim), m_fontSmall);

    // 音量滑块
    {
        const int trackY = rVolume.y + rVolume.h / 2;
        ::XSetForeground (m_dpy, m_gc, px (CBtnEdge));
        ::XDrawLine (m_dpy, m_pixmap, m_gc, rVolume.x, trackY,
                     rVolume.x + rVolume.w - 1, trackY);

        const float v = m_backend ? m_backend->volume () : 1.0f;
        const int knobX = rVolume.x + static_cast<int> (v * (rVolume.w - 1));
        const Rect knob { knobX - 5, rVolume.y + 4, 10, rVolume.h - 8 };
        fillRect (m_pixmap, knob, px (CCheckOn));

        drawText (xd, rVolume.x - 36, rVolume.y + 16, "音量", xc (CTextDim), m_fontSmall);
        drawText (xd, rVolLabel.x, rVolLabel.y + 14, m_volText.c_str (), xc (CText), m_fontSmall);
    }

    drawText (xd, rFile.x, rFile.y + 14, m_fileText.c_str (), xc (CText), m_fontSmall);
    drawText (xd, rStatus.x, rStatus.y + 14, m_statusText.c_str (), xc (CText), m_fontSmall);
    drawText (xd, rFmt.x, rFmt.y + 14,
              "支持 MP3/WAV/FLAC/OGG",
              xc (CTextDim), m_fontSmall);

    if (!m_errText.empty ())
        drawText (xd, rStatus.x, rStatus.y + 14 - 0, m_errText.c_str (), xc (CBiliPink), m_fontSmall);

    drawText (xd, rBrand.x, rBrand.y + 14,
              "♪ B 站「大伟鼓谱」· 欢迎关注，鼓谱 / 教学 / 伴奏持续更新",
              xc (CBiliPink), m_fontSmall);
}

//------------------------------------------------------------------------------
// 使用指南覆盖层
//
// 面板只有 340x384，完整说明书塞不进常驻布局（挤掉的会是波形区）。所以做成
// 「？帮助」唤出的覆盖层：铺满整块面板、点任意处关闭。
//
// 实现比 Windows 端简单：整个界面本来就画在离屏 pixmap 上，所以在 paint() 的
// 最后再盖一层就行，不需要独立子窗口。
//
// 配色跟随本端主题 —— Linux 面板是浅色的（与 macOS / Windows 的深色面板不同），
// 所以这里用白底深字；照搬另外两端的深底浅字会在浅色面板上格格不入。
//------------------------------------------------------------------------------
void X11View::drawHelp (XftDraw* xd)
{
    // 行首 "#" = 小标题。
    //
    // 排序 = 【实际操作顺序】：装音频 → 填速度 → 对拍子 → 波形区手势 →
    // 出问题怎么办 → 免费/求关注。只讲「怎么点、怎么拖」，不讲原理。
    // ⚠️ 快捷键必须与本端真实实现一致 —— 本端【没有双击全览】（X11 侧只处理
    //    Button4/5 滚轮，没有双击判定），所以只能写「点『全览』按钮」。
    //    mac 端是 ⌘/⌥ + 触摸板手势 + 双击；Win 端是 Ctrl/Alt + 双击 —— 别照抄。
    // ⚠️ 绝不要写「红绿两线重合即对齐」：两条线本来就是重合的
    //    （音频位置 = 谱面位置 + 偏移，永远如此），重合与否跟偏移调没调对无关
    //    —— 它们只在「跟随出故障」时才分开。判断偏移有没有调好，唯一依据是
    //    【网格小节线有没有落在波形上的鼓点/第一拍】。
    static const char* kGuide[] = {
        "#1  装音频：点「打开音频」，或把音频文件直接拖进来。",
        "     支持 MP3 / WAV / FLAC / OGG。",
        "     装好后在乐谱里按空格播放。",
        "",
        "#2  填速度：先填 BPM 和拍号（如 120、4/4）。",
        "     填好了网格小节线才对得上谱子的小节；",
        "     留空 = 自动，跟着乐谱走。",
        "",
        "#3  对拍子：按住 Ctrl（或 Alt）在波形上左右拖网格，",
        "     把「1」那条小节线拖到音乐的第一拍上（对准波形里的",
        "     鼓点）。按住 Shift 拖 = 微调。调坏了点右边的「归零」。",
        "     音频开头被跳过的那段会画成灰色。",
        "",
        "#4  波形区：直接拖 = 平移视野；滚轮 = 平移；",
        "     Ctrl/Alt + 滚轮 = 缩放；整首全览点「全览」按钮。",
        "     窗口边缘可以拖动 —— 拉宽它，波形区更大。",
        "",
        "#5  在乐谱里点任意小节，画面会跟着跳过去（扒谱方便）。",
        "     红线=音频播到哪，绿线=乐谱播到哪，平时黏在一起；分开",
        "     了或找不到播放头，点「回到播放头」；还不行就退出宿主重开。",
        "",
        "完全免费，只为方便大家制谱、练鼓。顺手的话点一下",
        "界面底部的「大伟鼓谱」，到 B 站关注一下就是支持。",
    };
    const int n = static_cast<int> (sizeof (kGuide) / sizeof (kGuide[0]));

    // 整块面板铺白，盖住底下所有内容
    fillRect (m_pixmap, Rect { 0, 0, m_w, m_h }, px (CWhite));

    drawText (xd, 12, 8 + m_font->ascent, "使用指南", xc (CCheckOn), m_font);
    drawText (xd, 62, 10 + m_fontSmall->ascent, "（点一下关闭）",
              xc (CTextDim), m_fontSmall);

    fillRect (m_pixmap, Rect { 12, 26, m_w - 24, 1 }, px (CBtnEdge));

    // 23 行 × 15px，从 y=32 起 → 末行文字顶 362、底 ≈377，仍在面板【最小】高 384 之内
    //（面板可以拉更高，这里按最小高算才是紧的那一档）。
    // ⚠️ 文案再加长就要同步这里（或改小 lh），否则最后一行会被面板底边裁掉。
    const int lh = 15;
    int y = 32;
    for (int i = 0; i < n; ++i)
    {
        const char* line = kGuide[i];
        if (line[0] != '\0')
        {
            const bool head = (line[0] == '#');
            drawText (xd, 12, y + (head ? m_font->ascent : m_fontSmall->ascent),
                      head ? line + 1 : line,
                      head ? xc (CCheckOn) : xc (CText),
                      head ? m_font : m_fontSmall);
        }
        y += lh;
    }
}

// 显示/隐藏使用指南。关闭由 onButtonPress 负责（点任意处即关）。
void X11View::toggleHelp ()
{
    m_helpVisible = !m_helpVisible;
    m_hotId = ID_NONE;      // 关掉悬停高亮，避免残留按钮的「热」态
    m_dirty = true;
}

//------------------------------------------------------------------------------
// 交互
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// 打开作者 B 站主页（点底部页脚宣传语时调用）
//
// Linux 没有「用默认程序打开 URL」的系统 API，事实标准是拉 xdg-open。
//
// ⚠️ 三个必须点，缺一个都会出问题：
//  1) 不能用 system()：它同步等命令退出，GUI 线程会被拖住，直接影响音频调度
//     （宿主是实时音频程序）。所以 fork 之后父进程立刻返回。
//  2) 子进程里 exec 失败必须 _exit(127)，绝不能 return —— return 会一路退回
//     宿主的调用栈上继续执行，等于把宿主进程又跑一遍，行为完全不可预期。
//  3) 用「双 fork」而不是给 SIGCHLD 设 SIG_IGN：后者改的是**整个宿主进程**的
//     全局信号处置，会破坏宿主自己的子进程管理（它可能有音频/更新子进程）。
//     双 fork 让真正干活的孙子进程被 init 收养；父进程只回收中间那层，
//     而中间层 fork 完就 _exit，所以 waitpid 是微秒级、不会卡住界面。
//
// ⚠️ 已知差异：本端没有做「鼠标移上去变手型」。X11 要按区域换光标必须自己
//    XCreateFontCursor + 在 motion 事件里 XDefineCursor/XUndefineCursor 来回切，
//    而 Linux 端从未在真机验证过 —— 宁可不写，也不加一段没人验证过的光标逻辑。
//    点击本身可用；macOS / Windows 两端是有手型光标的。
//------------------------------------------------------------------------------
static void openBrandHome ()
{
    const pid_t mid = ::fork ();
    if (mid < 0)
    {
        ap::crashLog ("作者主页打开失败：fork 失败（%s）", ap::kBrandHomeUrl);
        return;
    }

    if (mid == 0)
    {
        // ---- 中间层子进程：只负责再 fork 一次，然后立刻退出 ----
        const pid_t child = ::fork ();
        if (child == 0)
        {
            // ---- 真正干活的孙子进程 ----
            ::setsid ();   // 脱离会话：宿主退出时不会顺手把浏览器带走
            ::execlp ("xdg-open", "xdg-open", ap::kBrandHomeUrl, nullptr);
            // 精简桌面环境里可能没有 xdg-open，退而用 GNOME 的 gio
            ::execlp ("gio", "gio", "open", ap::kBrandHomeUrl, nullptr);
            ::_exit (127);   // 两个都没有：直接死掉，别再往下走
        }
        ::_exit (0);
    }

    int status = 0;
    ::waitpid (mid, &status, 0);   // 只等中间层（它 fork 完就退了），不会阻塞
}

void X11View::onButtonPress (int x, int y, unsigned state)
{
    // 使用指南铺满整块面板，任何点击都只表示「知道了」。
    if (m_helpVisible)
    {
        m_helpVisible = false;
        m_pressedId = ID_NONE;
        m_dirty = true;
        return;
    }

    if (m_browserOpen)
    {
        browserClick (x, y);
        m_dirty = true;
        return;
    }

    m_pressedId = ID_NONE;

    // 页脚宣传语：点一下用系统默认浏览器打开作者 B 站主页。
    // ⚠️ 必须放在「波形区」之前判：rBrand 紧贴在波形区下方，命中域不能重叠。
    if (rBrand.hit (x, y))
    {
        openBrandHome ();
        return;
    }

    // 波形区
    if (Rect { kWaveX, kWaveY, kWaveW, kWaveH }.hit (x, y) && m_backend && m_backend->hasAudio ())
    {
        m_dragging = true;
        m_dragStartX = x;
        m_dragOffset = ((state & ControlMask) != 0) || ((state & Mod1Mask) != 0);
        // 改偏移时记 offset，平移视野时记 viewStart —— 两者互斥，复用同一个字段。
        m_dragStartVal = m_dragOffset ? m_backend->offsetSec () : m_viewStart;

        // 平移视野 = 用户在主动看别处 → 暂停自动跟随 2 秒（拖完自然恢复）。
        // 改偏移【不算】：那只动网格锚点，播放头本身没被挪走。
        if (!m_dragOffset)
            markUserScrolling ();

        // 不再有「点击 / 拖动 = 定位播放头」：音频位置完全由宿主驱动，
        // 手动把它拽走只会和谱面播放头分家，还得再点一次「回到播放头」才能恢复。
        m_dirty = true;
        return;
    }

    // 音量滑块
    if (rVolume.hit (x, y) && m_backend)
    {
        m_dragVolume = true;
        const float v = static_cast<float> (x - rVolume.x) /
                        static_cast<float> (rVolume.w - 1);
        m_backend->setVolume (std::max (0.0f, std::min (1.0f, v)));
        m_dirty = true;
        return;
    }

    // 按钮
    int id = ID_NONE;
    if (rOpenBtn.hit (x, y))     id = ID_OPEN;
    else if (rHelpBtn.hit (x, y)) id = ID_HELP;
    else if (rZoomOut.hit (x, y)) id = ID_ZOOM_OUT;
    else if (rZoomIn.hit (x, y))  id = ID_ZOOM_IN;
    else if (rZoomFit.hit (x, y)) id = ID_ZOOM_FIT;
    else if (rBack.hit (x, y))    id = ID_BACK;
    else if (rOffsetZero.hit (x, y)) id = ID_OFFSET_ZERO;
    else if (rGridChk.hit (x, y)) id = ID_GRID;
    else if (rNumChk.hit (x, y))  id = ID_NUM;
    else if (rBpmUp.hit (x, y))   id = ID_BPM_UP;
    else if (rBpmDown.hit (x, y)) id = ID_BPM_DOWN;
    else if (rBpmBox.hit (x, y))  id = ID_BPM_BOX;
    else if (rTimeSig.hit (x, y)) id = ID_TIMESIG;

    if (id != ID_NONE)
    {
        m_pressedId = id;
        actionFor (id);
        m_dirty = true;
    }
}

void X11View::onButtonRelease (int x, int y, unsigned /*state*/)
{
    (void) x; (void) y;
    if (m_dragging)
        m_lastUserScrollAt = nowSec ();   // 松手后再等 2 秒才恢复跟随
    m_dragging = false;
    m_dragVolume = false;
    m_pressedId = ID_NONE;
    m_dirty = true;
}

void X11View::onMotion (int x, int y, unsigned state)
{
    if (m_dragging && m_backend)
    {
        if (m_dragOffset)
        {
            const double dxSec = (x - m_dragStartX) / static_cast<double> (kWaveW) * m_viewSpan;
            double v = m_dragStartVal + dxSec;
            if (state & ShiftMask)
                v = m_dragStartVal + dxSec * 0.1;
            if (v >  3600.0) v =  3600.0;
            if (v < -3600.0) v = -3600.0;
            m_backend->setOffsetSec (static_cast<float> (v));
        }
        else if (kWaveW > 0 && m_viewSpan > 0.0)
        {
            // 平移视野（抓手）：把内容往右拉 → 视野往左移，看到更早的音频。
            const double dxSec = (x - m_dragStartX) / static_cast<double> (kWaveW) * m_viewSpan;
            m_viewStart = m_dragStartVal - dxSec;
            clampView ();
            markUserScrolling ();
        }
        m_dirty = true;
        return;
    }

    if (m_dragVolume && m_backend)
    {
        const float v = static_cast<float> (x - rVolume.x) /
                        static_cast<float> (rVolume.w - 1);
        m_backend->setVolume (std::max (0.0f, std::min (1.0f, v)));
        m_dirty = true;
        return;
    }

    // hover 高亮
    // 使用指南挡在上面时不做任何悬停高亮 —— 否则鼠标划过会看到底下的按钮
    // 隔着说明文字亮起来，像是说明本身就是个按钮。
    if (m_helpVisible)
    {
        if (m_hotId != ID_NONE)
        {
            m_hotId = ID_NONE;
            m_dirty = true;
        }
        return;
    }

    int id = ID_NONE;
    if (rOpenBtn.hit (x, y))     id = ID_OPEN;
    else if (rHelpBtn.hit (x, y)) id = ID_HELP;
    else if (rZoomOut.hit (x, y)) id = ID_ZOOM_OUT;
    else if (rZoomIn.hit (x, y))  id = ID_ZOOM_IN;
    else if (rZoomFit.hit (x, y)) id = ID_ZOOM_FIT;
    else if (rBack.hit (x, y))    id = ID_BACK;
    else if (rOffsetZero.hit (x, y)) id = ID_OFFSET_ZERO;
    else if (rGridChk.hit (x, y)) id = ID_GRID;
    else if (rNumChk.hit (x, y))  id = ID_NUM;
    else if (rBpmUp.hit (x, y))   id = ID_BPM_UP;
    else if (rBpmDown.hit (x, y)) id = ID_BPM_DOWN;
    else if (rTimeSig.hit (x, y)) id = ID_TIMESIG;

    if (id != m_hotId)
    {
        m_hotId = id;
        m_dirty = true;
    }
}

void X11View::onScroll (int x, int y, int dir, unsigned state, bool horizontal)
{
    // 使用指南铺满整块面板：滚轮在这里【什么都不做】，也【绝不能】把它关掉 ——
    // 指南是固定的一屏文字，用户却习惯往下滚一滚看看还有没有，一滚就关 =
    // 用户报的「刚进去就没了」。原来这里没有判断，滚轮会偷偷把视野滚走，
    // 等用户点一下关掉指南，波形已经不在原来的位置了。
    // 关闭只认鼠标点击（见 onButtonPress）。
    if (m_helpVisible)
        return;

    if (m_browserOpen)
    {
        browserScroll (dir);
        m_dirty = true;
        return;
    }

    if (!m_backend || !m_backend->hasAudio ())
        return;

    // BPM 区域滚轮微调
    if (rBpmBox.hit (x, y) || rBpmUp.hit (x, y) || rBpmDown.hit (x, y))
    {
        float bpm = m_backend->gridBPM ();
        if (bpm <= 0.0f) bpm = m_tl.tempoValid ? m_tl.bpm : 120.0f;
        bpm += (state & ShiftMask) ? dir * 5.0f : dir * 1.0f;
        if (bpm < 20.0f)  bpm = 20.0f;
        if (bpm > 400.0f) bpm = 400.0f;
        m_backend->setGridBPM (bpm);
        m_dirty = true;
        return;
    }

    if ((state & ControlMask) || (state & Mod1Mask))
    {
        zoomBy (dir > 0 ? 0.8 : 1.25, m_backend->positionSec ());
    }
    else
    {
        // 滚轮平移 = 用户在主动看别处 → 暂停自动跟随 2 秒。
        // 纵向滚轮（Button4/5）：向上 = 往更早看（与旧版一致）。
        // 横向滚轮（Button6/7）：往更晚看 —— 与 Windows 端 WM_MOUSEHWHEEL 同向。
        markUserScrolling ();
        m_viewStart += horizontal ? dir * m_viewSpan * 0.1
                                  : -dir * m_viewSpan * 0.1;
    }

    clampView ();
    m_dirty = true;
}

void X11View::actionFor (int id)
{
    switch (id)
    {
        case ID_OPEN:
            browserOpen ();
            break;
        case ID_ZOOM_OUT:
            zoomBy (1.25, m_backend ? m_backend->positionSec () : 0.0);
            break;
        case ID_ZOOM_IN:
            zoomBy (0.8, m_backend ? m_backend->positionSec () : 0.0);
            break;
        case ID_ZOOM_FIT:
            zoomFit ();
            break;
        case ID_BACK:
            if (m_backend && m_backend->hasAudio () && m_tl.playheadValid)
            {
                // ⚠ 必须带上起始偏移：谱面 0 秒 = 音频 offset 秒。
                //   旧代码写的是 seekTo (m_tl.playheadSec)，漏了这个偏移 ——
                //   点一次就把两条播放头按偏移量错开，恰好和这个按钮
                //   「修复分家」的职责相反。（macOS 端一直是带偏移的。）
                const double target = m_tl.playheadSec + m_backend->offsetSec ();
                m_backend->seekTo (target);
                centerViewOn (target);      // 视野也回到播放位置

                // 点这个按钮 = 「我要看播放头」，所以立刻恢复自动跟随 ——
                // 否则刚滚过波形的人点了按钮还要再等 2 秒才跟，等于白点。
                // （macOS / Windows 端同样处理，三端一致。）
                m_userScrolling = false;
                m_lastUserScrollAt = 0.0;

                // 记下「刚刚手动对齐过」：若 3 秒内又发现错位，说明是跟随失效而非
                // 一次性偏差 → 徽标升级提示 + 落一条日志。
                m_lastBackAt = nowSec ();
                m_followFail = false;
            }
            break;
        case ID_OFFSET_ZERO:
            // 「归零」：把起始偏移一键清零。
            // 为什么要它：改偏移的唯一入口是「Ctrl/Alt + 在波形上拖动」，一旦手滑
            // 拖到很大的值（或只想回到初始状态），就只能反向拖回去 —— 可能要拖好
            // 几个屏。用户实测点名要。
            if (m_backend)
            {
                m_backend->setOffsetSec (0.0f);

                // 归零会让音频跳回「谱面位置 + 0」，播放头可能一下跑到画面外 ——
                // 那种情况就把它带回画面（本来就在画面里则什么都不做，别抢用户
                // 正在看的视野）。与 macOS 端 offsetZero: 的处理一致。
                if (m_tl.playheadValid)
                {
                    const double tgt = m_tl.playheadSec;   // 偏移已归零 ⇒ 音频位置 = 谱面位置
                    if (tgt < m_viewStart || tgt > m_viewStart + m_viewSpan)
                        centerViewOn (tgt);
                }
            }
            break;
        case ID_HELP:
            toggleHelp ();
            break;
        case ID_GRID:
            m_showGrid = !m_showGrid;
            break;
        case ID_NUM:
            m_showNumbers = !m_showNumbers;
            break;
        case ID_BPM_BOX:
            // 点数字框恢复「自动（跟随谱面/默认 120）」
            if (m_backend)
                m_backend->setGridBPM (0.0f);
            break;
        case ID_BPM_UP:
        case ID_BPM_DOWN:
            if (m_backend)
            {
                float bpm = m_backend->gridBPM ();
                if (bpm <= 0.0f) bpm = m_tl.tempoValid ? m_tl.bpm : 120.0f;
                bpm += (id == ID_BPM_UP) ? 1.0f : -1.0f;
                if (bpm < 20.0f)  bpm = 20.0f;
                if (bpm > 400.0f) bpm = 400.0f;
                m_backend->setGridBPM (bpm);
            }
            break;
        case ID_TIMESIG:
            m_timeSigIndex = (m_timeSigIndex + 1) % kTimeSigCount;
            if (m_backend)
            {
                m_backend->setGridBeatsPerBar (kTimeSigs[m_timeSigIndex].beats);
                m_backend->setGridBeatDenominator (kTimeSigs[m_timeSigIndex].denom);
            }
            break;
        default:
            break;
    }
}

void X11View::zoomBy (double factor, double centerSec)
{
    // ⭐ 缩放也算「用户在看别处」：否则点「放大 / 缩小」按钮（它们不经过滚轮）
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

void X11View::zoomFit ()
{
    if (!m_backend)
        return;
    markUserScrolling ();
    const double dur = m_backend->durationSec ();
    m_viewStart = 0.0;
    m_viewSpan = dur > 0.0 ? dur : 8.0;
}

void X11View::clampView ()
{
    const double dur = m_backend ? m_backend->durationSec () : 0.0;
    if (m_viewStart < 0.0)
        m_viewStart = 0.0;
    if (dur > 0.0 && m_viewStart > dur)
        m_viewStart = dur;
}

// 把视野挪到 sec 处 —— 播放头落在画面 25% 的位置（与 macOS / Windows 端同一规则）。
// 「回到播放头」除了重新对齐，还要把画面也带回去：只对齐不挪视野，用户会看到
// 「提示已对齐，但画面里什么都没有」，比不对齐还困惑。
void X11View::centerViewOn (double sec)
{
    const double dur = m_backend ? m_backend->durationSec () : 0.0;
    if (m_viewSpan <= 0.0)
        return;
    if (dur > 0.0 && m_viewSpan >= dur)
        return;                 // 全览：整段都在画面里，无需滚动
    m_viewStart = sec - m_viewSpan * 0.25;
    clampView ();
}

//------------------------------------------------------------------------------
// 视野自动跟随（与 macOS / Windows 端同一套规则 —— 改这里就要改那两端）
//------------------------------------------------------------------------------
void X11View::markUserScrolling ()
{
    m_userScrolling = true;
    m_lastUserScrollAt = nowSec ();
}

// 用户停手 2 秒后自动恢复跟随。
// ⚠️ 旧版（macOS 端曾踩过）一旦置 true 就永久停跟随，用户滚一下之后播放头就再也
//    不跟了；这里必须有超时恢复。
void X11View::recoverAutoFollow (double now)
{
    if (!m_userScrolling)
        return;
    if (m_dragging)             // 还在拖，别打断
        return;
    if ((now - m_lastUserScrollAt) >= kUserScrollHoldSec)
        m_userScrolling = false;
}

// 让视野跟随播放头（audioSec = 播放头在【音频时间轴】上的位置）。
//
// 【反抽搐设计 · 定稿规则】画面**永远不会把播放头「拽」回来**，只有一条规则：
//   · 用户正在手动操作（m_userScrolling）→ 完全不干预，播放头允许呆在画面之外。
//     旧版曾加过「播放头完全跑出画面就强制拉回」，于是用户往左拖、画面被拽回右，
//     来回拉锯 —— 视觉上就是抽搐。
//   · 播放头远在画面之外（用户把视野拖到别处看）→ 什么都不做。想回来看播放头
//     就点「回到播放头」—— 该按钮存在就是为了这件事，比偷偷自动跳转可预期。
//   · 唯一例外 allowJump：**宿主自己**把播放头挪了（点小节 / 循环回卷 / 拖播放
//     头）。这种跳变若不让画面跟过去，用户会「找不到播放头」，所以允许跟随。
//     allowJump 由 runLoop 用「相邻两帧的谱面位置差」判定，见 kHostJumpSec。
//
// ⚠️ 本端过去**根本没有这个函数** —— 用户实测「回到播放头之后播放头直接就跑到
//    画面外面去了」，根因就是 m_viewStart 只被用户操作和「回到播放头」改过。
bool X11View::followPlayheadTo (double audioSec, bool allowJump)
{
    if (!m_followScroll)
        return false;
    if (m_dragging || m_helpVisible || m_browserOpen)
        return false;
    if (!(m_viewSpan > 0.0))
        return false;

    // ① 用户在看别处 → 不干预（宿主主动跳转时例外）
    if (m_userScrolling && !allowJump)
        return false;

    const double before = m_viewStart;
    const double v0 = m_viewStart;
    const double v1 = v0 + m_viewSpan;
    const double margin = m_viewSpan * 0.25;   // 播放头离边缘多远开始滚

    // ② 远在画面之外、且不是宿主跳转 → 不追。这是「不抢用户视野」的关键一条。
    //    容差取一个整屏：极端放大时播放头两帧之间就能移动大半屏，容差太小会漏跟。
    if (!allowJump && (audioSec < v0 - m_viewSpan || audioSec > v1 + m_viewSpan))
        return false;

    // ③ 播放头落在画面左侧（循环回卷 / 向前跳转）→ 对到 25% 处，让它重新可见
    if (audioSec < v0 || audioSec > v1 - margin)
    {
        m_viewStart = audioSec - margin;
        clampView ();
    }

    return m_viewStart != before;
}

void X11View::syncTimeSigFromBackend ()
{
    if (!m_backend)
        return;
    const int beats = m_backend->gridBeatsPerBar ();
    const int denom = m_backend->gridBeatDenominator ();
    for (int i = 0; i < kTimeSigCount; ++i)
    {
        if (kTimeSigs[i].beats == beats && kTimeSigs[i].denom == denom)
        {
            m_timeSigIndex = i;
            return;
        }
    }
    // 后端里的组合不在列表里（理论上不会）：保持默认项，不猜。
}

// 新载入音频的默认视野：放大到「前几小节」，从起始偏移处开始看，而不是整曲全览
// （全览时波峰糊成一片，看不清鼓点）。与 macOS / Windows 端同一套。
// ⚠️ 这段规则被 afterLoad（用户换文件）和 restoreViewState（后端里没存过视野）
//    两处共用，所以单独成函数 —— 写两份必然漂移。
void X11View::applyDefaultView ()
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
// 根因：关掉编辑器只销毁视图，后端（Processor）还活着 —— 视野是用户的操作结果，
// 跟 BPM 一样必须存在后端里，重建视图时回读。
void X11View::restoreViewState ()
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

    // 记成「已回存」，免得 runLoop 第一帧把同一份值再写一遍。
    m_pushedViewStart = m_viewStart;
    m_pushedViewSpan  = m_viewSpan;
    m_dirty = true;
}

void X11View::afterLoad ()
{
    if (!m_backend || !m_backend->hasAudio ())
        return;

    applyDefaultView ();
    m_browserOpen = false;

    // 换了文件 = 换了一条时间轴：上一首的播放头位置不能拿来判断「宿主跳变」，
    // 否则新文件头一帧就会被误判成一次巨大的跳变。用户的手动滚动状态也一并清掉
    // （新文件进来算是「重新开始看」）。与 macOS / Windows 端同一处理。
    m_hasPrevPlayhead = false;
    m_userScrolling = false;
    m_dirty = true;
}

//------------------------------------------------------------------------------
// 文件浏览器（自绘，零依赖；避免依赖 GTK/zenity）
//------------------------------------------------------------------------------
void X11View::browserOpen ()
{
    std::string dir;
    if (!m_browserDir.empty ())
        dir = m_browserDir;
    else
    {
        const char* home = ::getenv ("HOME");
        dir = (home && *home) ? home : "/";
    }
    browserEnter (dir);
    m_browserOpen = true;
    m_dirty = true;
}

void X11View::browserEnter (const std::string& path)
{
    DIR* d = ::opendir (path.c_str ());
    if (!d)
        return;

    m_browserDir = path;
    m_entries.clear ();
    m_browserScroll = 0;

    struct dirent* e = nullptr;
    while ((e = ::readdir (d)) != nullptr)
    {
        const std::string name = e->d_name;
        if (name == ".")
            continue;
        if (name == "..")
            continue;

        std::string full = path;
        if (!full.empty () && full.back () != '/')
            full.push_back ('/');
        full += name;

        struct stat st {};
        if (::stat (full.c_str (), &st) != 0)
            continue;

        const bool isDir = S_ISDIR (st.st_mode);
        if (isDir || isAudioFile (name))
            m_entries.push_back ({ name, isDir });
    }
    ::closedir (d);

    std::sort (m_entries.begin (), m_entries.end (),
               [] (const Entry& a, const Entry& b) {
                   if (a.isDir != b.isDir)
                       return a.isDir;          // 目录在前
                   return a.name < b.name;
               });
}

void X11View::browserScroll (int dir)
{
    const int maxScroll = std::max (0, static_cast<int> (m_entries.size ()) - 15);
    m_browserScroll = std::max (0, std::min (maxScroll, m_browserScroll - dir));
}

void X11View::browserClick (int x, int y)
{
    (void) x;

    // 顶部按钮条：y < 26
    if (y < 26)
    {
        if (x >= 8 && x < 88)                    // 上级目录
        {
            if (m_browserDir.size () > 1)
            {
                std::string up = m_browserDir;
                if (up.back () == '/')
                    up.pop_back ();
                const size_t slash = up.find_last_of ('/');
                up = (slash == std::string::npos || slash == 0) ? "/" : up.substr (0, slash);
                browserEnter (up);
            }
        }
        else if (x >= 96 && x < 150)             // 取消
        {
            m_browserOpen = false;
        }
        return;
    }

    const int rowH = 22;
    const int firstY = 30;
    const int index = m_browserScroll + (y - firstY) / rowH;
    if (index < 0 || index >= static_cast<int> (m_entries.size ()))
        return;

    const Entry& e = m_entries[static_cast<size_t> (index)];
    std::string full = m_browserDir;
    if (!full.empty () && full.back () != '/')
        full.push_back ('/');
    full += e.name;

    if (e.isDir)
    {
        browserEnter (full);
    }
    else if (m_backend)
    {
        m_backend->loadFile (full);
        afterLoad ();
    }
}

void X11View::drawBrowser (XftDraw* xd)
{
    fillRect (m_pixmap, Rect { 0, 0, m_w, m_h }, px (CBg));

    const Rect rUp   { 8,   4, 80, 20 };
    const Rect rCanc { 96,  4, 54, 20 };
    button (xd, rUp,   "↑ 上级", ID_BROWSER_UP);
    button (xd, rCanc, "取消",   ID_BROWSER_CANCEL);

    // 当前目录（过长时截断显示尾部）
    {
        std::string shown = m_browserDir;
        if (shown.size () > 40)
            shown = "…" + shown.substr (shown.size () - 40);
        drawText (xd, 158, 18, shown.c_str (), xc (CTextDim), m_fontSmall);
    }

    const int rowH = 22;
    const int firstY = 30;
    const int maxRows = (m_h - firstY - 6) / rowH;

    for (int i = 0; i < maxRows; ++i)
    {
        const int idx = m_browserScroll + i;
        if (idx < 0 || idx >= static_cast<int> (m_entries.size ()))
            break;

        const Entry& e = m_entries[static_cast<size_t> (idx)];
        const Rect row { 8, firstY + i * rowH, m_w - 16, rowH - 2 };

        fillRect (m_pixmap, row, px (CWhite));
        frameRect (m_pixmap, row, px (CBtnEdge));

        const std::string label = (e.isDir ? "[目录] " : "        ") + e.name;
        drawText (xd, row.x + 6, row.y + 15, label.c_str (),
                  e.isDir ? xc (CText) : xc (CCheckOn), m_fontSmall);
    }

    if (m_entries.empty ())
        drawText (xd, 12, 46, "（此目录没有音频文件或子目录）", xc (CTextDim), m_fontSmall);
}

//------------------------------------------------------------------------------
// XDND（X11 拖放协议）
//------------------------------------------------------------------------------
void X11View::setupDnd ()
{
    m_aXdndAware      = ::XInternAtom (m_dpy, "XdndAware", False);
    m_aXdndEnter      = ::XInternAtom (m_dpy, "XdndEnter", False);
    m_aXdndPosition   = ::XInternAtom (m_dpy, "XdndPosition", False);
    m_aXdndStatus     = ::XInternAtom (m_dpy, "XdndStatus", False);
    m_aXdndDrop       = ::XInternAtom (m_dpy, "XdndDrop", False);
    m_aXdndLeave      = ::XInternAtom (m_dpy, "XdndLeave", False);
    m_aXdndFinished   = ::XInternAtom (m_dpy, "XdndFinished", False);
    m_aXdndSelection  = ::XInternAtom (m_dpy, "XdndSelection", False);
    m_aXdndTypeList   = ::XInternAtom (m_dpy, "XdndTypeList", False);
    m_aXdndActionCopy = ::XInternAtom (m_dpy, "XdndActionCopy", False);
    m_aTextUriList    = ::XInternAtom (m_dpy, "text/uri-list", False);

    // 声明支持 XDND 版本 5
    unsigned long version = 5;
    ::XChangeProperty (m_dpy, m_win, m_aXdndAware, XA_ATOM, 32,
                       PropModeReplace, reinterpret_cast<unsigned char*> (&version), 1);
}

void X11View::sendDndStatus (Window source, bool accept)
{
    XEvent ev {};
    ev.xclient.type = ClientMessage;
    ev.xclient.display = m_dpy;
    ev.xclient.window = source;
    ev.xclient.message_type = m_aXdndStatus;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = static_cast<long> (m_win);
    ev.xclient.data.l[1] = accept ? 1 : 0;       // bit0 = 接受
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = accept ? static_cast<long> (m_aXdndActionCopy) : 0;
    ::XSendEvent (m_dpy, source, False, NoEventMask, &ev);
    ::XFlush (m_dpy);
}

void X11View::finishDnd (Window source, bool ok)
{
    XEvent ev {};
    ev.xclient.type = ClientMessage;
    ev.xclient.display = m_dpy;
    ev.xclient.window = source;
    ev.xclient.message_type = m_aXdndFinished;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = static_cast<long> (m_win);
    ev.xclient.data.l[1] = ok ? 1 : 0;
    ev.xclient.data.l[2] = ok ? static_cast<long> (m_aXdndActionCopy) : 0;
    ::XSendEvent (m_dpy, source, False, NoEventMask, &ev);
    ::XFlush (m_dpy);
}

void X11View::handleDndClientMessage (const XClientMessageEvent& cm)
{
    if (cm.message_type == m_aXdndEnter)
    {
        m_dndSource = static_cast<Window> (cm.data.l[0]);
        m_dndPending = false;
        return;
    }

    if (cm.message_type == m_aXdndPosition)
    {
        if (m_dndSource)
            sendDndStatus (m_dndSource, true);
        return;
    }

    if (cm.message_type == m_aXdndLeave)
    {
        m_dndSource = 0;
        m_dndPending = false;
        return;
    }

    if (cm.message_type == m_aXdndDrop)
    {
        const Time t = static_cast<Time> (cm.data.l[2]);
        if (!m_dndSource)
            return;

        m_dndPending = true;
        // 请求 uri-list：selection = XdndSelection, target = text/uri-list,
        // property 用 XdndSelection（XDND 惯例）
        ::XConvertSelection (m_dpy, m_aXdndSelection, m_aTextUriList,
                             m_aXdndSelection, m_win, t);
        ::XFlush (m_dpy);
    }
}

void X11View::handleSelectionNotify (const XSelectionEvent& se)
{
    if (se.property == None)
    {
        if (m_dndPending && m_dndSource)
        {
            finishDnd (m_dndSource, false);
            m_dndSource = 0;
            m_dndPending = false;
        }
        return;
    }

    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char* data = nullptr;

    if (::XGetWindowProperty (m_dpy, m_win, se.property, 0, 65536, False,
                              AnyPropertyType, &type, &format, &nitems, &bytesAfter,
                              &data) != Success || !data)
    {
        if (m_dndPending && m_dndSource)
        {
            finishDnd (m_dndSource, false);
            m_dndSource = 0;
            m_dndPending = false;
        }
        return;
    }

    std::string list (reinterpret_cast<char*> (data),
                      static_cast<size_t> (format == 8 ? nitems : nitems * format / 8));
    ::XFree (data);

    std::string first;
    {
        const size_t nl = list.find_first_of ("\r\n");
        first = (nl == std::string::npos) ? list : list.substr (0, nl);
    }

    bool ok = false;
    if (!first.empty () && m_backend)
    {
        const std::string path = uriToPath (first);
        if (m_backend->loadFile (path))
        {
            afterLoad ();
            ok = true;
        }
    }

    if (m_dndSource)
    {
        finishDnd (m_dndSource, ok);
        m_dndSource = 0;
    }
    m_dndPending = false;
    m_dirty = true;
}

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
    if (type && std::strcmp (type, Steinberg::kPlatformTypeX11EmbedWindowID) == 0)
        return Steinberg::kResultTrue;
    return Steinberg::kResultFalse;
}

tresult PLUGIN_API PlugView::attached (void* parent, FIDString type)
{
    if (!parent || !type || std::strcmp (type, Steinberg::kPlatformTypeX11EmbedWindowID) != 0)
        return Steinberg::kResultFalse;

    if (!m_backend && m_resolver)
        m_backend = m_resolver ();

    const Window parentWin = static_cast<Window> (reinterpret_cast<uintptr_t> (parent));

    X11View* v = new X11View (m_backend);
    if (!v->create (parentWin, kPanelW, kPanelH))
    {
        delete v;
        return Steinberg::kResultFalse;
    }
    m_view = v;
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::removed ()
{
    if (m_view)
    {
        delete static_cast<X11View*> (m_view);
        m_view = nullptr;
    }
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onWheel (float distance)
{
    // X11 上滚轮事件由窗口自己处理（Button4/5）；这里作为兜底
    (void) distance;
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
    // ⭐ 视图已经建好时【如实汇报当前尺寸】（与 Windows / macOS 端一致）。
    if (m_view)
    {
        const X11View* v = static_cast<const X11View*> (m_view);
        size->right  = v->panelW ();
        size->bottom = v->panelH ();
    }
    else
    {
        size->right  = kPanelW;
        size->bottom = kPanelH;
    }
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onSize (ViewRect* newSize)
{
    if (m_view && newSize)
        static_cast<X11View*> (m_view)->resize (newSize->right - newSize->left,
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
// 波形区随宽度自适应，面板窄到 340 也仍然排得下 —— 见 layoutAll()。
tresult PLUGIN_API PlugView::canResize ()
{
    return Steinberg::kResultTrue;
}

tresult PLUGIN_API PlugView::checkSizeConstraint (ViewRect* rect)
{
    if (rect)
    {
        // 与 create() / resize() 用同一套钳制范围，别两处各写一份。
        int w = rect->right - rect->left;
        int h = rect->bottom - rect->top;
        if (w < kPanelWMin) w = kPanelWMin;
        if (h < kPanelHMin) h = kPanelHMin;
        if (w > kPanelWMax) w = kPanelWMax;
        if (h > kPanelHMax) h = kPanelHMax;
        rect->right  = rect->left + w;
        rect->bottom = rect->top  + h;
    }
    return Steinberg::kResultOk;
}

} // namespace ap
