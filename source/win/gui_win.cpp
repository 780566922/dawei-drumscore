//==============================================================================
// gui_win.cpp — 中文界面（Windows / Win32 实现 VST3 IPlugView）
//
// 对应 macOS 的 gui.mm。设计保持一致：
//   - 波形预览 + 可拖动播放头
//   - 带符号起始偏移（在波形上按住 Ctrl / Alt 拖动）
//   - 小节网格（BPM + 拍号）
//   - 播放状态灯、音量、回到谱面
//   - 拖入音频文件 / 点按钮选文件
//
// 与宿主的关系：VST3 在 Windows 上通过 kPlatformTypeHWND 传父窗口句柄，
//   这里创建一个子窗口挂上去，控件全部是它的子窗口。
//==============================================================================
#include "gui.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

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

namespace {

const int kPanelW = 340;      ///< 面板宽（与 macOS 版一致）
const int kPanelH = 384;      ///< 面板高（含底部 B 站署名）
const int kWaveW  = 320;      ///< 波形区宽
const int kWaveH  = 118;      ///< 波形区高

// 控件 ID
enum : int
{
    IDC_OPEN = 1001,
    IDC_ZOOM_OUT,
    IDC_ZOOM_IN,
    IDC_ZOOM_FIT,
    IDC_BACK_TO_SCORE,
    IDC_GRID_CHECK,
    IDC_NUM_CHECK,
    IDC_BPM_EDIT,
    IDC_BPM_UP,
    IDC_BPM_DOWN,
    IDC_TIMESIG_COMBO,
    IDC_VOLUME_SLIDER,
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

    HWND hwnd () const { return m_hwnd; }

private:
    void layoutChildren ();
    void refreshLabels ();
    void openFileDialog ();
    void applyBpmFromEdit ();
    void zoomBy (double factor, double centerSec);
    void zoomFit ();
    void clampView ();
    void afterLoad ();
    double xToSec (int x) const;
    int    secToX (double sec) const;

    PlugView::Backend* m_backend = nullptr;

    HWND m_hwnd = nullptr;
    HWND m_wave = nullptr;
    HWND m_openBtn = nullptr, m_zoomOut = nullptr, m_zoomIn = nullptr, m_zoomFit = nullptr;
    HWND m_backBtn = nullptr, m_gridCheck = nullptr, m_numCheck = nullptr;
    HWND m_bpmEdit = nullptr, m_bpmUp = nullptr, m_bpmDown = nullptr;
    HWND m_timesig = nullptr, m_volSlider = nullptr;
    HWND m_timeLabel = nullptr, m_offsetLabel = nullptr, m_srcLabel = nullptr;
    HWND m_volLabel = nullptr, m_fileLabel = nullptr, m_hintLabel = nullptr;
    HWND m_statusLamp = nullptr, m_brand = nullptr, m_fmtLabel = nullptr;

    HFONT m_font = nullptr, m_fontSmall = nullptr, m_fontBold = nullptr;
    HBRUSH m_bgBrush = nullptr;

    double m_viewStart = 0.0;   ///< 视野起点（秒）
    double m_viewSpan = 8.0;    ///< 视野跨度（秒）

    bool m_dragging = false;
    bool m_dragOffset = false;
    int  m_dragStartX = 0;
    double m_dragStartVal = 0.0;

    bool m_showGrid = true;
    bool m_showNumbers = true;
};

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
bool WinView::create (HWND parent, int w, int h)
{
    ensureClasses ();

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

    LOGFONTW lf = {};
    lf.lfHeight = -13;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    ::wcscpy_s (lf.lfFaceName, LF_FACESIZE, L"Microsoft YaHei UI");
    m_font = ::CreateFontIndirectW (&lf);

    lf.lfHeight = -11;
    m_fontSmall = ::CreateFontIndirectW (&lf);
    lf.lfHeight = -15;
    lf.lfWeight = FW_BOLD;
    m_fontBold = ::CreateFontIndirectW (&lf);

    if (!m_font)      m_font = reinterpret_cast<HFONT> (::GetStockObject (DEFAULT_GUI_FONT));
    if (!m_fontSmall) m_fontSmall = m_font;
    if (!m_fontBold)  m_fontBold = m_font;

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
    m_zoomOut   = mk (L"BUTTON", L"缩小", BS_PUSHBUTTON, IDC_ZOOM_OUT);
    m_zoomIn    = mk (L"BUTTON", L"放大", BS_PUSHBUTTON, IDC_ZOOM_IN);
    m_zoomFit   = mk (L"BUTTON", L"全览", BS_PUSHBUTTON, IDC_ZOOM_FIT);
    m_backBtn   = mk (L"BUTTON", L"回到谱面", BS_PUSHBUTTON, IDC_BACK_TO_SCORE);
    m_gridCheck = mk (L"BUTTON", L"网格", BS_AUTOCHECKBOX, IDC_GRID_CHECK);
    m_numCheck  = mk (L"BUTTON", L"小节号", BS_AUTOCHECKBOX, IDC_NUM_CHECK);
    ::SendMessageW (m_gridCheck, BM_SETCHECK, BST_CHECKED, 0);
    ::SendMessageW (m_numCheck, BM_SETCHECK, BST_CHECKED, 0);

    m_bpmEdit = mk (L"EDIT", L"120", ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER, IDC_BPM_EDIT);
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
                        L"拖动=定位　Ctrl/Alt拖动=改偏移　滚轮=平移　Ctrl/Alt滚轮=缩放　双击=全览",
                        SS_LEFT, 0);
    m_statusLamp  = mk (L"STATIC", L"○ 未载入", SS_LEFT, 0);
    m_fmtLabel    = mk (L"STATIC", L"支持 MP3/WAV/M4A/AAC/WMA/FLAC", SS_LEFT, 0);
    m_brand       = mk (L"STATIC", L"♪ B 站「大伟鼓谱」· 欢迎关注，鼓谱 / 教学 / 伴奏持续更新",
                        SS_LEFT, IDC_BRAND);

    HWND smalls[] = { m_timeLabel, m_offsetLabel, m_srcLabel, m_volLabel,
                      m_fileLabel, m_hintLabel, m_statusLamp, m_fmtLabel, m_brand };
    for (HWND c : smalls)
        if (c)
            ::SendMessageW (c, WM_SETFONT, reinterpret_cast<WPARAM> (m_fontSmall), TRUE);

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
        ::DestroyWindow (m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_font && m_font != ::GetStockObject (DEFAULT_GUI_FONT))
        ::DeleteObject (m_font);
    if (m_fontSmall && m_fontSmall != m_font)
        ::DeleteObject (m_fontSmall);
    if (m_fontBold && m_fontBold != m_font)
        ::DeleteObject (m_fontBold);
    if (m_bgBrush)
        ::DeleteObject (m_bgBrush);
    m_font = m_fontSmall = m_fontBold = nullptr;
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
    auto move = [] (HWND h, int x, int y, int w, int hh) {
        if (h)
            ::MoveWindow (h, x, y, w, hh, TRUE);
    };

    move (m_openBtn,    240,   6,  90, 24);
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
}

//------------------------------------------------------------------------------
double WinView::xToSec (int x) const
{
    return m_viewStart + (static_cast<double> (x) / kWaveW) * m_viewSpan;
}

int WinView::secToX (double sec) const
{
    if (m_viewSpan <= 0.0)
        return 0;
    return static_cast<int> ((sec - m_viewStart) / m_viewSpan * kWaveW);
}

//------------------------------------------------------------------------------
void WinView::paint (HDC target, const RECT& rc)
{
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0)
        return;

    HDC dc = ::CreateCompatibleDC (target);
    HBITMAP bmp = ::CreateCompatibleBitmap (target, W, H);
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
            HPEN wavePen = ::CreatePen (PS_SOLID, 1, RGB (90, 190, 255));
            HPEN oldPen = reinterpret_cast<HPEN> (::SelectObject (dc, wavePen));
            const int mid = H / 2;
            const int n = static_cast<int> (peaks.size ());
            for (int i = 0; i < n && i < W; ++i)
            {
                float p = peaks[static_cast<size_t> (i)];
                if (p < 0.0f) p = 0.0f;
                if (p > 1.0f) p = 1.0f;
                const int half = static_cast<int> (p * (H / 2 - 2));
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
            HPEN barPen  = ::CreatePen (PS_SOLID, 1, RGB (255, 190, 90));
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
                    if (x < -20 || x > W)
                        continue;
                    wchar_t buf[32];
                    std::swprintf (buf, 32, L"%lld", barNo + 1);
                    ::TextOutW (dc, x + 2, 2, buf, static_cast<int> (std::wcslen (buf)));
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
                HPEN p = ::CreatePen (PS_SOLID, 2, RGB (80, 220, 120));
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
                HPEN p = ::CreatePen (PS_SOLID, 2, RGB (255, 90, 90));
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
                RECT sh = { 0, 0, x < W ? x : W, H };
                HBRUSH ob = ::CreateSolidBrush (RGB (70, 40, 20));
                ::FillRect (dc, &sh, ob);
                ::DeleteObject (ob);
            }
        }
    }

    ::SelectObject (dc, oldBmp);
    ::BitBlt (target, rc.left, rc.top, W, H, dc, 0, 0, SRCCOPY);
    ::DeleteObject (bmp);
    ::DeleteDC (dc);
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
    m_dragStartVal = m_dragOffset ? m_backend->offsetSec () : 0.0;

    if (!m_dragOffset)
        m_backend->seekTo (xToSec (x));

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
        const double dxSec = (x - m_dragStartX) / static_cast<double> (kWaveW) * m_viewSpan;
        double v = m_dragStartVal + dxSec;
        if (mods & MK_SHIFT)
            v = m_dragStartVal + dxSec * 0.1;
        if (v > 3600.0)  v = 3600.0;
        if (v < -3600.0) v = -3600.0;
        m_backend->setOffsetSec (static_cast<float> (v));
    }
    else
    {
        m_backend->seekTo (xToSec (x));
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

void WinView::onTimer ()
{
    if (!m_backend)
        return;
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
    ofn.lpstrFilter = L"音频文件\0*.mp3;*.wav;*.m4a;*.aac;*.wma;*.flac;*.aif;*.aiff\0所有文件\0*.*\0";
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

void WinView::onHScroll (HWND /*slider*/)
{
    if (!m_backend || !m_volSlider)
        return;
    const int pos = static_cast<int> (::SendMessageW (m_volSlider, TBM_GETPOS, 0, 0));
    m_backend->setVolume (static_cast<float> (pos) / 100.0f);
    refreshLabels ();
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
            if (m_backend)
            {
                const PlugView::Backend::HostTimeline tl = m_backend->hostTimeline ();
                if (tl.playheadValid)
                    m_backend->seekTo (tl.playheadSec);
                refreshLabels ();
                ::InvalidateRect (m_wave, nullptr, FALSE);
            }
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
            if (m_bpmEdit)
            {
                wchar_t t[32] = {0};
                ::GetWindowTextW (m_bpmEdit, t, 32);
                double v = ::_wtof (t) + 1.0;
                std::swprintf (t, 32, L"%d", static_cast<int> (v));
                ::SetWindowTextW (m_bpmEdit, t);
                applyBpmFromEdit ();
            }
            break;
        case IDC_BPM_DOWN:
            if (m_bpmEdit)
            {
                wchar_t t[32] = {0};
                ::GetWindowTextW (m_bpmEdit, t, 32);
                double v = ::_wtof (t) - 1.0;
                if (v < 1.0) v = 1.0;
                std::swprintf (t, 32, L"%d", static_cast<int> (v));
                ::SetWindowTextW (m_bpmEdit, t);
                applyBpmFromEdit ();
            }
            break;
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

    WinView* v = new WinView (m_backend);
    if (!v->create (reinterpret_cast<HWND> (parent), kPanelW, kPanelH))
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
    size->right = kPanelW;
    size->bottom = kPanelH;
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
