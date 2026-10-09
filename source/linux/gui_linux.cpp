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
//------------------------------------------------------------------------------
const int kPanelW = 340;   ///< 面板宽（与 macOS / Windows 版一致）
const int kPanelH = 384;   ///< 面板高（含底部 B 站署名）
const int kWaveX  = 10;
const int kWaveY  = 34;
const int kWaveW  = 320;
const int kWaveH  = 118;

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
//------------------------------------------------------------------------------
struct Rect
{
    int x, y, w, h;
    bool hit (int px, int py) const
    {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

const Rect rOpenBtn  = { 240,   6,  90, 24 };
const Rect rHelpBtn  = { 172,   6,  62, 24 };   ///< 「？帮助」：唤出使用指南覆盖层
const Rect rTime     = {  10, 158,  92, 18 };
const Rect rZoomOut  = { 104, 155,  38, 22 };
const Rect rZoomIn   = { 144, 155,  38, 22 };
const Rect rZoomFit  = { 184, 155,  38, 22 };
const Rect rBack     = { 224, 155,  80, 22 };
const Rect rGridChk  = {  10, 182,  62, 20 };
const Rect rNumChk   = {  74, 182,  72, 20 };
const Rect rOffset   = { 150, 182, 126, 18 };
const Rect rHint     = {  10, 204, 320, 18 };
const Rect rBpmBox   = {  46, 226,  44, 22 };
const Rect rBpmUp    = {  91, 226,  18, 11 };
const Rect rBpmDown  = {  91, 237,  18, 11 };
const Rect rTimeSig  = { 150, 224,  70, 22 };
const Rect rSrc      = { 228, 226, 106, 18 };
const Rect rVolume   = {  46, 252, 188, 24 };
const Rect rVolLabel = { 238, 254,  52, 18 };
const Rect rFile     = {  10, 280, 320, 18 };
const Rect rStatus   = {  10, 300, 130, 18 };
const Rect rFmt      = { 140, 300, 190, 18 };
const Rect rBrand    = {  10, 358, 320, 18 };

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
    }
    ~X11View () { destroy (); }

    bool create (Window parent, int w, int h);
    void destroy ();
    void resize (int w, int h);

    Window window () const { return m_win; }

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
    void onScroll (int x, int y, int dir, unsigned state);

    void actionFor (int id);
    void zoomBy (double factor, double centerSec);
    void zoomFit ();
    /// 把视野挪到 sec（播放头落在画面 25% 处）。用于「回到播放头」。
    void centerViewOn (double sec);
    /// 显示/隐藏使用指南覆盖层。
    void toggleHelp ();
    void clampView ();
    void afterLoad ();
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

    ::XLockDisplay (m_dpy);
    m_w = w;
    m_h = h;
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
                    // X11 的滚轮就是 Button4 / Button5
                    if (ev.xbutton.button == Button4)
                        onScroll (ev.xbutton.x, ev.xbutton.y,  1, ev.xbutton.state);
                    else if (ev.xbutton.button == Button5)
                        onScroll (ev.xbutton.x, ev.xbutton.y, -1, ev.xbutton.state);
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
    drawText (xd, rHint.x, rHint.y + 14,
              "拖动=平移视图 · Ctrl/Alt 拖动=改偏移 · 滚轮=平移/缩放 · 双击=全览",
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
    // 行首 "#" = 小标题。文案与 macOS / Windows 端保持一致。
    static const char* kGuide[] = {
        "#1  载入：点「打开音频」，或把文件拖进窗口。",
        "     支持 MP3 / WAV / M4A / AAC / FLAC / OGG",
        "",
        "#2  对齐：绿线「谱面」=乐谱播到哪，",
        "     红线「音频」=音频播到哪。两线重合即对齐。",
        "     不重合就按住 Ctrl/Alt 在波形上左右拖动网格，",
        "     拖到两线贴合为止。偏移值见「偏移」一栏。",
        "",
        "#3  播放：在乐谱里按空格，插件自动跟着出声。",
        "     插件不能反向控制宿主，播放/暂停请用宿主。",
        "",
        "#4  视图：滚轮/拖动=平移　Ctrl/Alt+滚轮=缩放",
        "     双击波形=全览；视野会自动跟着播放头走。",
        "",
        "#5  速度：默认自动跟随乐谱；乐谱没给速度时",
        "     手填 BPM 与拍号，网格小节线才对得上。",
        "",
        "#6  乱了：点「回到播放头」跳回播放位置并对齐。",
        "     若提示「跟随可能已失效」，请重开宿主。",
    };
    const int n = static_cast<int> (sizeof (kGuide) / sizeof (kGuide[0]));

    // 整块面板铺白，盖住底下所有内容
    fillRect (m_pixmap, Rect { 0, 0, m_w, m_h }, px (CWhite));

    drawText (xd, 12, 8 + m_font->ascent, "使用指南", xc (CCheckOn), m_font);
    drawText (xd, 62, 10 + m_fontSmall->ascent, "（点任意处关闭）",
              xc (CTextDim), m_fontSmall);

    fillRect (m_pixmap, Rect { 12, 26, m_w - 24, 1 }, px (CBtnEdge));

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

void X11View::onScroll (int x, int y, int dir, unsigned state)
{
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
        zoomBy (dir > 0 ? 0.8 : 1.25, m_backend->positionSec ());
    else
        m_viewStart -= dir * m_viewSpan * 0.1;

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

                // 记下「刚刚手动对齐过」：若 3 秒内又发现错位，说明是跟随失效而非
                // 一次性偏差 → 徽标升级提示 + 落一条日志。
                m_lastBackAt = nowSec ();
                m_followFail = false;
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

void X11View::afterLoad ()
{
    if (!m_backend || !m_backend->hasAudio ())
        return;
    m_viewStart = 0.0;
    m_viewSpan = m_backend->durationSec ();
    if (m_viewSpan <= 0.0)
        m_viewSpan = 8.0;
    m_browserOpen = false;
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
    size->right = kPanelW;
    size->bottom = kPanelH;
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
    (void) frame;
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
        rect->right = rect->left + kPanelW;
        rect->bottom = rect->top + kPanelH;
    }
    return Steinberg::kResultOk;
}

} // namespace ap
