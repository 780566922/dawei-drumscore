//==============================================================================
// gui.mm — 中文界面的 AppKit 实现
//
// 界面布局（默认 640 x 384，y 轴向下；宽度可变，高度固定）：
//   ┌────────────────────────────────────────────┐
//   │  大伟鼓谱 · MuseScore 音频播放器        [打开音频…] │
//   │  ┌──────────────────────────────────────┐  │
//   │  │  波形 + 小节网格 + 播放头              │  │
//   │  └──────────────────────────────────────┘  │
//   │  0:00 / 3:45 [缩小][放大][全览][回到播放头] 网格 小节号│
//   │  起始偏移        （改偏移靠波形上 ⌘/⌥ 拖动） │
//   │  速度 [120] ▲▼  拍号 [4/4▾]  速度来源…      │
//   │  音量 [───────●───────]          80 %      │
//   │  状态文字                                   │
//   │  [▶ 播放]              拖入音频文件          │
//   ├────────────────────────────────────────────┤
//   │  ♪ B 站「大伟鼓谱」· 欢迎关注…（页脚署名）   │
//   └────────────────────────────────────────────┘
// 面板变宽时（宿主要多宽就给多宽）：波形区、分隔线、状态文字、页脚跟着变宽，
// 右上角按钮与右侧读数贴右边界。
// 各控件靠 autoresizingMask 自动重排，没有「按尺寸重算坐标」的布局函数（见
// GUIView initWithBackend: 里的三档规则）。
//
// 波形区交互（对齐时最常用）：
//   滚轮            左右平移
//   ⌥滚轮 / 双指捏合 以鼠标位置为锚点缩放
//   双击            回到全览
//   按住拖动        平移视图（抓手）—— 只动视野，不碰播放位置
//   ⌥/⌘ 拖动        拖动小节网格 = 改起始偏移（配合 ⇧ 微调 0.1 倍）
//
// 注：旧版的「点击 / 拖动 = 定位音频播放头」已移除。
//     音频位置完全由宿主驱动（谱面位置 + 偏移），手动把它拽走只会让红绿两条
//     播放头分家，用户还得再点一次按钮才能恢复 —— 纯误操作来源。
//     去掉之后，红线就只是一条只读的「音频实际播到哪」指示。
//
// 「回到播放头」（原名「回到谱面」）与「分家检测」：
//   手动定位这条误操作路径堵上了，但万一有未知 bug 让音频线程跟随失效，
//   两条线依然会悄悄分家，而用户盯着两条线是看不出来的。所以补两道保险：
//     · 波形区右上角的错位徽标 —— 播放中两者差值超过阈值就亮出来；
//     · 「回到播放头」一键修复 —— seek 回谱面位置（+偏移）并把视野也带过去。
//
// 「帮助」按钮：
//   面板只有 640x384，完整操作说明塞不进常驻布局，所以做成覆盖层，
//   点一下铺满整个面板、点任意处关闭（见 HelpOverlayView）。
//==============================================================================
#include "gui.h"

#include "crashguard.h"

#import <Cocoa/Cocoa.h>
#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Steinberg;

//------------------------------------------------------------------------------
// 分家检测阈值（秒）
//
// 正常播放时「音频实际播到哪」与「谱面位置 + 偏移」只差一个音频缓冲的量级
// （几十毫秒）。超过这个值就说明跟随出了问题，波形区右上角会亮出错位徽标。
// 取值兼顾两头：太小会被缓冲抖动误报，太大则漏掉真正的分家。
//------------------------------------------------------------------------------
static const double kDivergenceWarnSec = 0.25;

//---- 「点完『回到播放头』仍然错位」的判定窗口 ----------------------------------
// 正常情况下点了那个按钮就该对齐。若这么久之内又检测到错位，说明偏差不是一次
// 能纠正的，而是某个跟随条件**持续**为假 —— 此时再点按钮也没用，提示要升级成
// 「建议重启插件」，并落一条日志方便事后收故障报告。
// ⚠️ 这里只做「提示升级」，绝不自动 seek：0.25 秒的偏差等于十几个音频块，
//    属结构性故障，自动 seek 会下一块又错开 → 每 33ms 拉一次 → 音频发抖。
//    （跳变场景的自动硬 seek 在 aplaysdk.cpp 的 captureHostTimeline 里。）
static const double kFollowFailWindowSec = 3.0;
static const double kFollowFailLogGapSec = 5.0;   ///< 日志冷却，避免抖动着刷屏

//---- 宿主播放头「跳变」阈值（秒）----------------------------------------------
// 界面定时器 20Hz 跑一次。正常播放时相邻两次读到的谱面位置只差几十毫秒；
// 超过这个值就说明不是「播放头自己在走」，而是宿主把它挪了（点小节 / 循环回卷 /
// 拖播放头 / 播放到结尾后回卷）。只有这种跳变才允许视野跟着跳。
static const double kHostJumpSec = 0.25;

/// 新载入音频的默认视野（秒）。不默认「整曲全览」：全览时波峰糊成一片，看不清
/// 鼓点，用户每次都得先手动放大好多下。8 秒 ≈ 4 小节 @120BPM 4/4，一打开就能
/// 看清对齐状况。曲长不足 8 秒时 zoomToSpan: 会回落到全览。三端同名同值。
static const double kDefaultViewSpanSec = 8.0;

//---- 音频丢块探针的日志冷却（秒）---------------------------------------------
// 「音频线程抢不到锁 → 整块丢音频 → 播放位置永久落后宿主」是完全隐形的故障。
// 这里把它变成一条日志（同一故障 5 秒内只记一次，避免刷屏）。
static const double kLockDropLogGapSec = 5.0;

//---- 面板尺寸 ----------------------------------------------------------------
// ⚠️ 面板宽度【由宿主决定】：宿主拿 getSize() 去定窗口，而且定下之后可能再也改不了 ——
//    MuseScore 4.x 把插件编辑器窗口的 min/max 设成同一个值（钉死），既不给缩放边框、
//    也从不调用 canResize()。所以「用户拖窗口改大小」在本宿主上做不到，别再这么写文案。
//    本端只管三件事：① 按规范回答 canResize()（别的宿主会照办）；② 布局自适应任何宽度；
//    ③ checkSizeConstraint() 钳制宿主送来的尺寸。
// 高度固定 384 —— 每一行的纵向位置、以及帮助覆盖层的「行数预算」都是按 384 排的，
// 放开高度就得连带重算，收益（波形变高）远小于风险。
static const CGFloat kPanelH    = 384.0;    ///< 固定高度
static const CGFloat kPanelWDef = 640.0;    ///< 默认宽度
// ⚠️ 最小宽度 = 默认宽度。第 4 行的快捷键提示实测需 345pt（起点 x=226），
//    再窄就会被截断 —— 与其留着截断，不如不允许缩窄。
static const CGFloat kPanelWMin = 640.0;
static const CGFloat kPanelWMax = 1700.0;   ///< 与 Windows / Linux 同一上限

// 拍号条：波形视图【顶部】划出的一条窄带，按小节画「N/M」方块，点击方块
// 弹出拍号设置面板。条高从波形视图内部让出（视图 frame 不变）——
// 面板高度固定 384、每一行的纵向位置都是按它排的，动 frame 会连带动布局与验证器。
static const CGFloat kTsStripH = 22.0;      ///< 拍号条高度（pt）
static const CGFloat kTsBoxH   = 15.0;      ///< 方块高度（pt）
static const CGFloat kTsBoxGap = 4.0;       ///< 相邻方块最小间隔（太挤就跳过不画）

// ObjC++ 的 ivar 只能是指针/标量，C++ 容器要包在结构体里再放指针
struct WaveState
{
    ap::PlugView::Backend* backend = nullptr;
    std::vector<float> peaks;        ///< 当前可见区间的峰值包络

    //---- 按住拖动 = 平移视图（抓手）----
    // 旧字段 dragging 的语义是「正在拖播放头」，已废弃：
    // 播放位置不再允许手动拖动（见文件头注释）。现在拖动只改 viewStart。
    bool   panning = false;          ///< 正在按住拖动平移视图
    double panStartX = 0.0;          ///< 按下时的鼠标 x（视图坐标）
    double panStartViewStart = 0.0;  ///< 按下时的视野起点

    //---- 视野（单位：秒，音频时间轴）----
    double viewStart = 0.0;
    double viewLen   = 0.0;          ///< 0 表示全览

    //---- 峰值取样节流：只在视野真正变化时才重取样 ----
    // 手势（滚轮/缩放）是高频事件，一秒几十次。取样会遍历可见区间的采样，
    // 若这段遍历持着音频线程要用的锁，音频线程的 try_lock 就会反复失败 →
    // 整块丢音频 → 播放位置永久落后宿主（实测表现为「一边拖波形一边分家」）。
    //
    // ⭐ 根治办法在播放器侧：音频数据改成不可变快照，画波形只取一份
    //    shared_ptr 就随便扫，完全不碰那把锁（见 audiofile.h）。这里的节流
    //    仍然保留 —— 它省的是界面线程自己的 CPU，与实时安全已无关。
    double peaksViewStart = -1.0;    ///< 峰值对应的视野起点（-1 表示从未取样）
    double peaksViewLen   = -1.0;    ///< 峰值对应的视野时长
    bool   peaksDirty     = true;    ///< 需要重新取样

    bool showGrid    = true;
    bool showNumbers = true;

    //---- ⌥ 拖动网格（等价于调整起始偏移）----
    bool   draggingGrid = false;
    double dragStartX = 0.0;
    double dragStartSec = 0.0;       ///< 按下时鼠标对应的音频时间
    float  dragStartOffset = 0.0f;
    void*  panel = nullptr;          ///< GUIView*（父视图，生命期必比子视图长）

    //---- 播放头跟随（宿主播放时视野自动滚到播放头）----
    bool   followScroll = true;      ///< 是否启用「视野跟随谱面播放头」
    bool   userScrolling = false;    ///< 用户手动滚轮/拖动时暂停跟随（避免抢视图）
    double lastUserScrollAt = 0.0;   ///< 用户最后一次手动滚动的时刻（秒），用于自动恢复跟随

    //---- 「点完『回到播放头』仍错位」检测（由 tick 维护，drawRect 只读）----
    double lastBackAt = -1.0e9;      ///< 上次点「回到播放头」的时刻（systemUptime）
    bool   followFail = false;       ///< 是否处于「点了按钮仍错位」状态
    double followFailLoggedAt = -1.0e9;  ///< 上次为此写日志的时刻（做冷却）
};

@class WaveformView;
@class HelpOverlayView;   // 定义在文件后半段（用覆盖层做使用指南，见 HelpOverlayView）
@class TimeSigPopupView;  // 定义在文件后半段（拍号设置覆盖层）

// 波形视图 → 面板的回调（面板是父视图，生命期必比子视图长）
@protocol WavePanel <NSObject>
- (void)offsetChangedExternally:(double)sec;
/// 用户点了拍号条上第 bar 小节的方块 → 面板弹出该小节的拍号设置面板
- (void)openTimeSigPopupForBar:(int)bar;
@end

// 同理：GUIView 的状态也打包
struct PanelState
{
    ap::PlugView::Backend* backend = nullptr;
    WaveformView* wave = nil;
    NSSlider* volSlider = nil;
    NSTextField* offsetLabel = nil;   ///< 只读的偏移值显示（⌘/⌥拖动时实时刷新）
    NSTextField* volLabel = nil;
    NSTextField* timeLabel = nil;
    NSTextField* statusLabel = nil;
    NSTextField* bpmField = nil;      ///< BPM 输入框（手填谱面速度）
    NSStepper* bpmStepper = nil;      ///< BPM 步进器
    NSPopUpButton* beatsPopup = nil;  ///< 拍号下拉（N/M 形式）
    NSTextField* tempoSrcLabel = nil; ///< 速度来源提示
    NSButton* gridCheck = nil;
    NSButton* numCheck = nil;
    NSButton* playBtn = nil;
    HelpOverlayView* helpOverlay = nil;   ///< 覆盖整块面板的使用指南（默认隐藏）
    TimeSigPopupView* tsPopup    = nil;   ///< 拍号设置覆盖层（默认隐藏）
    NSTimer* timer = nil;
    bool timelineLogged = false;     ///< 宿主时间轴信息只记一次日志
    bool beatsUserSet = false;       ///< 用户手动改过拍号后就不再自动跟随乐谱

    //---- 宿主播放头跳变检测（区分「播放头自己在走」和「宿主把它挪走了」）----
    // 只在 GUIView::tick 里用，所以归 PanelState（⚠️ 别加进 WaveState：
    // tick 里的 `_st` 是 PanelState，加错会报 no member named ... in 'PanelState'）。
    bool   hasPrevPlayhead = false;  ///< 是否已经存过上一帧的谱面位置
    double prevPlayheadSec = 0.0;    ///< 上一帧的谱面位置（秒）

    //---- 实时健康探针：音频线程丢块（见 Backend::lockDropCount）----
    uint64_t lockDropSeen = 0;       ///< 上次记录的丢块计数（用于判断有没有增长）
    double   lockDropLoggedAt = -1.0e9;  ///< 上次为此写日志的时刻（冷却）

    //---- 视野状态回存（关掉编辑器再打开要恢复到同一段视野）----
    // 视图每次打开都是新建的，视野得存在后端里；tick 里发现视野变了就写回。
    // -1 = 「还没写过」（视野起点可以是 0，所以不能用 0 当哨兵）。
    double pushedViewStart = -1.0;
    double pushedViewLen   = -1.0;

    // 注：跳转跟随（宿主拖播放头 → 音频 seek）已下沉到音频线程，
    // 不再需要 GUI 侧保存播放头历史。
};

//------------------------------------------------------------------------------
// 网格参数：BPM 与拍号 N/M。
//   BPM  = 每分钟「四分音符」的个数（用户手填谱面速度）
//   拍号 = 每小节 N 个「M 分音符」，每拍时长 = 60/bpm × (4/M)
// 这样「每小节时长 = N × 每拍时长」才能和谱面真实小节对齐（前提是 BPM 填对）。
//------------------------------------------------------------------------------
struct GridInfo
{
    double beatSec  = 0.5;
    double quarter  = 0.5;    ///< 一个四分音符的时长 = 60/bpm（拍号变化时要按小节重算）
    int    beats    = 4;      ///< 拍号分子 N（每小节拍数）
    int    denom    = 4;      ///< 拍号分母 M（2/4/8/16）
    double bpm      = 120.0;
    bool   fromHost = false;  ///< 速度来自宿主（乐谱），而非手动
};

static GridInfo gridInfoFor (ap::PlugView::Backend* b)
{
    GridInfo g;
    if (!b) return g;

    // 拍号来自用户手选（乐谱里可读）。
    const int beats = b->gridBeatsPerBar ();
    g.beats = (beats >= 1 && beats <= 32) ? beats : 4;
    const int denom = b->gridBeatDenominator ();
    g.denom = (denom == 2 || denom == 4 || denom == 8 || denom == 16) ? denom : 4;

    // BPM：手动值优先；否则退回宿主 tempo；再退回 120。
    const float manual = b->gridBPM ();
    if (manual > 1.0f)
    {
        g.bpm = manual;
    }
    else
    {
        const ap::PlugView::Backend::HostTimeline t = b->hostTimeline ();
        if (t.tempoValid && t.bpm > 1.0f)
        {
            g.bpm = t.bpm;
            g.fromHost = true;
        }
        else
        {
            g.bpm = 120.0;
        }
    }

    // 每拍时长：BPM 定义的是「四分音符每分钟」，所以「M 分音符」的每拍
    // 时长 = (60/bpm) × (4/M)。例如 12/8 拍号下，每拍（八分音符）时长是
    // 四分音符的一半。
    g.beatSec = (60.0 / g.bpm) * (4.0 / static_cast<double> (g.denom));
    g.quarter = 60.0 / g.bpm;
    return g;
}

//------------------------------------------------------------------------------
// 取视野覆盖到的小节表（拍号条与网格线都用它）。
//
// 逐小节推进的算法本身在 gui.h 的 ap::buildBars() —— 三端共用同一份，
// 免得「同一语义各写一遍」写歪（铁律 9）。
//------------------------------------------------------------------------------
static void visibleBarsFor (ap::PlugView::Backend* b, double viewStart, double viewLen,
                            std::vector<ap::BarMark>& out)
{
    out.clear ();
    if (!b || !(viewLen > 0.0)) return;
    const GridInfo g = gridInfoFor (b);
    std::vector<ap::TimeSigChange> changes;
    b->getTimeSigChanges (changes);
    ap::buildBars (viewStart, viewStart + viewLen, (double) b->offsetSec (), g.quarter,
                   g.beats, g.denom, changes, out);
}

//------------------------------------------------------------------------------
// 拍号条上方块的布局。
//
// ⭐ 画与命中测试【共用这一份】—— 两处各算一遍迟早会写歪（点击位置与画出来的
//    方块对不上，是最难查的那类 bug）。
//
// 方块太挤时会跳过中间那些（只保证看得清）。这与参考工具的做法一致：视野很宽时
// 每个小节都画只会糊成一片。
//------------------------------------------------------------------------------
struct TsBox
{
    NSRect  rect  = NSZeroRect;
    int     bar   = 1;
    int     beats = 4;
    int     denom = 4;
    bool    changed = false;
};

static void layoutTsBoxes (const std::vector<ap::BarMark>& bars, NSRect full,
                           double v0, double v1, std::vector<TsBox>& out)
{
    out.clear ();
    if (bars.empty () || !(v1 > v0) || NSWidth (full) <= 0.0) return;

    const double  pxPerSec = (double) NSWidth (full) / (v1 - v0);
    const CGFloat boxY     = NSMinY (full) + 3.0;

    NSDictionary* attrs = @{ NSFontAttributeName : [NSFont systemFontOfSize:9] };
    CGFloat lastRight = -1.0e9;

    for (const ap::BarMark& m : bars)
    {
        if (m.startSec < v0 || m.startSec > v1) continue;
        const CGFloat x = NSMinX (full) + (CGFloat) ((m.startSec - v0) * pxPerSec);

        NSString* label = [NSString stringWithFormat:@"%d/%d", m.beats, m.denom];
        const CGFloat tw = [label sizeWithAttributes:attrs].width;
        const CGFloat w  = std::max<CGFloat> (26.0, tw + 10.0);

        TsBox b;
        b.rect    = NSMakeRect (x - w / 2.0, boxY, w, kTsBoxH);
        b.bar     = m.bar;
        b.beats   = m.beats;
        b.denom   = m.denom;
        b.changed = m.changed;

        if (b.rect.origin.x < lastRight + kTsBoxGap) continue;   // 挤不下 → 跳过
        if (b.rect.origin.x > NSMaxX (full)) break;
        lastRight = NSMaxX (b.rect);
        out.push_back (b);
    }
}

//------------------------------------------------------------------------------
// 空后端：宿主注入音频处理器之前就打开编辑器时使用。
// 有了它，界面里所有 backend->xxx() 调用都不必判空 —— 否则定时器一跑就崩。
//------------------------------------------------------------------------------
class NullBackend : public ap::PlugView::Backend
{
public:
    bool loadFile (const std::string&) override { return false; }
    void unloadFile () override {}
    bool hasAudio () const override { return false; }
    double durationSec () const override { return 0.0; }
    std::string currentPath () const override { return {}; }
    std::string lastError () const override { return "插件后端未就绪"; }
    void waveformPeaks (std::vector<float>& out, int buckets,
                        double, double) const override
    {
        out.assign (static_cast<size_t> (buckets > 0 ? buckets : 1), 0.f);
    }
    void setOffsetSec (float) override {}
    float offsetSec () const override { return 0.f; }
    void setVolume (float) override {}
    float volume () const override { return 1.f; }
    void setPlaying (bool) override {}
    bool playing () const override { return false; }
    void setLooping (bool) override {}
    bool looping () const override { return false; }
    void seekTo (double) override {}
    double positionSec () const override { return 0.0; }
    void setGridBPM (float) override {}
    float gridBPM () const override { return 0.f; }
    void setGridBeatsPerBar (int) override {}
    int gridBeatsPerBar () const override { return 4; }
    void setGridBeatDenominator (int) override {}
    int gridBeatDenominator () const override { return 4; }
    ap::PlugView::Backend::HostTimeline hostTimeline () const override { return {}; }

    // 视野状态：空后端就存着自己（等于「没设置过」），视图会走默认视野。
    void setViewState (double startSec, double spanSec) override
    {
        m_viewStart = startSec;
        m_viewSpan = spanSec;
    }
    void getViewState (double& startSec, double& spanSec) const override
    {
        startSec = m_viewStart;
        spanSec = m_viewSpan;
    }

private:
    double m_viewStart = 0.0;
    double m_viewSpan = 0.0;
};

// 保证界面拿到的 backend 永不为 null
static ap::PlugView::Backend* safeBackend (ap::PlugView::Backend* b)
{
    static NullBackend kNull;
    return b ? b : &kNull;
}

@interface WaveformView : NSView
{
    WaveState* _st;
}
- (instancetype)initWithBackend:(ap::PlugView::Backend*)b;
- (void)setPanel:(void*)panel;
- (void)setShowGrid:(BOOL)b;
- (void)setShowNumbers:(BOOL)b;
- (void)refreshPeaks;
- (void)samplePeaksIfNeeded;   ///< 仅取样不重绘（drawRect 内部用，避免重绘循环）
- (void)invalidatePeaks;
- (void)drawTimeSigStripIn:(NSRect)full bars:(std::vector<ap::BarMark>&)bars
                     view0:(double)v0 view1:(double)v1;
- (std::vector<TsBox>)timeSigBoxes;   ///< 当前视野下拍号条的方块布局（点击命中与断言用）
- (void)fitAll;
- (void)zoomBy:(double)factor aroundX:(CGFloat)x;
- (void)zoomToSpan:(double)spanSec fromStart:(double)startSec;
- (void)followPlayheadTo:(double)audioSec allowJump:(BOOL)allowJump;
- (void)recoverAutoFollow;     ///< 用户停止手动操作超时后，自动恢复跟随
- (void)centerOn:(double)audioSec;   ///< 把视野挪到 audioSec（用于「回到播放头」）
- (void)centerOnIfOffscreen:(double)audioSec;   ///< 只在播放头跑出画面时才挪（用于「偏移归零」）
- (void)noteBackToPlayhead;          ///< 记下「刚点过回到播放头」，供升级提示用
- (void)checkFollow;                 ///< 由面板 tick 调用：判定点完按钮后是否仍错位
- (BOOL)followFailActive;            ///< 是否处于「点了按钮仍错位」状态（只读，自测用）
- (void)stopDrag;
- (double)viewStartSec;        ///< 当前视野起点（秒），只读 —— 自测与状态展示用
- (double)viewLenSec;          ///< 当前视野跨度（秒），只读 —— 自测用（默认视野断言）
@end

@implementation WaveformView

- (instancetype)initWithBackend:(ap::PlugView::Backend*)b
{
    if (self = [super initWithFrame:NSMakeRect (0, 0, 412, 116)])
    {
        _st = new WaveState ();
        _st->backend = safeBackend (b);
    }
    return self;
}

- (void)dealloc
{
    delete _st;   // ARC：编译器自动补 [super dealloc]，不能也不该手写
}

- (void)setPanel:(void*)panel { _st->panel = panel; }

- (double)viewStartSec { return _st->viewStart; }
- (double)viewLenSec { return _st->viewLen; }
- (void)setShowGrid:(BOOL)b { _st->showGrid = b ? true : false; [self setNeedsDisplay:YES]; }
- (void)setShowNumbers:(BOOL)b { _st->showNumbers = b ? true : false; [self setNeedsDisplay:YES]; }

- (BOOL)isOpaque { return NO; }
- (BOOL)isFlipped { return YES; }   // 让 y 轴向下，像网页一样直观

// 触控板捏合缩放（magnify）事件沿响应链传递，只有第一响应者才能收到。
// 不返回 YES 的话，双指捏合会被宿主窗口或上层视图吞掉，表现为缩放失效。
- (BOOL)acceptsFirstResponder { return YES; }

//------------------------------------------------------------------------------
// 视野钳制：始终保证 [viewStart, viewStart+viewLen] 有内容且合法
//------------------------------------------------------------------------------
- (void)clampView
{
    const double dur = _st->backend->durationSec ();
    if (dur <= 0.0) { _st->viewStart = 0.0; _st->viewLen = 0.0; return; }

    if (!(_st->viewLen > 0.0) || _st->viewLen > dur)
    {
        _st->viewStart = 0.0;
        _st->viewLen = dur;
        return;
    }
    const double kMinSpan = 0.05;              // 最多放大到看 50ms
    if (_st->viewLen < kMinSpan) _st->viewLen = kMinSpan;

    if (_st->viewLen >= dur) { _st->viewStart = 0.0; _st->viewLen = dur; return; }
    if (_st->viewStart < 0.0) _st->viewStart = 0.0;
    if (_st->viewStart > dur - _st->viewLen) _st->viewStart = dur - _st->viewLen;
}

- (void)fitAll
{
    const double dur = _st->backend->durationSec ();
    _st->viewStart = 0.0;
    _st->viewLen = (dur > 0.0) ? dur : 0.0;
    [self refreshPeaks];
}

// 把视野挪到 audioSec 处（播放头落在画面 25% 的位置，与自动跟随同一套规则）。
//
// 用途是「回到播放头」：用户可能把视野拖到了前面或后面，点一下按钮除了
// 重新对齐，还要把画面也带回当前播放位置 —— 否则会出现「音频对齐了但看不见」
// 的困惑。同时复位 userScrolling，让自动跟随重新接管。
// 「点完『回到播放头』仍错位」的判定 —— 说明跟随机制本身失效，再点按钮也没用。
// 由面板的 tick 调用（界面线程，20Hz）：日志 I/O 只能在界面线程做，音频线程绝不碰文件。
// 判据与下面徽标完全一致，同样排除起播追赶期与音频尾端（那两种偏差天然存在）。
- (void)checkFollow
{
    const ap::PlugView::Backend::HostTimeline tl = _st->backend->hostTimeline ();
    const double audioNow = _st->backend->positionSec ();
    const double durNow   = _st->backend->durationSec ();
    const double offNow   = _st->backend->offsetSec ();
    const double diffNow  = audioNow - (tl.playheadSec + offNow);

    const bool diverging =
        tl.playing && tl.playheadValid && durNow > 0.60
        && audioNow > 0.30 && audioNow < durNow - 0.30
        && std::fabs (diffNow) > kDivergenceWarnSec;

    const double nowT = [NSProcessInfo processInfo].systemUptime;
    if (diverging && (nowT - _st->lastBackAt) < kFollowFailWindowSec)
    {
        _st->followFail = true;
        if ((nowT - _st->followFailLoggedAt) > kFollowFailLogGapSec)
        {
            _st->followFailLoggedAt = nowT;
            ap::crashLog ("跟随可能已失效：点「回到播放头」后 %.1f 秒仍错位 %.2f 秒"
                          "（音频 %.3f / 谱面 %.3f + 偏移 %.3f）",
                          nowT - _st->lastBackAt, std::fabs (diffNow),
                          audioNow, tl.playheadSec, offNow);
        }
    }
    else
    {
        _st->followFail = false;
    }
}

// 用户点了「回到播放头」：记下时刻、清掉升级状态。
- (void)noteBackToPlayhead
{
    _st->lastBackAt = [NSProcessInfo processInfo].systemUptime;
    _st->followFail = false;
}

// 只读访问器（自测用）：当前是否处于「点了按钮仍错位」状态。
- (BOOL)followFailActive
{
    return _st->followFail ? YES : NO;
}

- (void)centerOn:(double)audioSec
{
    if (!(_st->viewLen > 0.0))       // 全览状态下整段都在画面里，无需滚动
    {
        [self fitAll];
        return;
    }
    _st->viewStart = audioSec - _st->viewLen * 0.25;
    _st->userScrolling = false;
    [self clampView];
    [self refreshPeaks];
    [self setNeedsDisplay:YES];
}

// 只在播放头【跑出画面】时才把视野挪过去。
// 给「偏移归零」用：归零会让音频一下跳回「谱面位置 + 0」，播放头可能瞬间跑到画面
// 外面（那正是用户接下来想看的地方）；但用户正盯着别处时不该被抢走视野 ——
// 所以与 centerOn: 的区别就是多这一道「本来就在画面里就什么都不做」的判断。
- (void)centerOnIfOffscreen:(double)audioSec
{
    if (!(_st->viewLen > 0.0))
    {
        [self fitAll];
        return;
    }
    if (audioSec >= _st->viewStart && audioSec <= _st->viewStart + _st->viewLen)
        return;
    [self centerOn:audioSec];
}

// 直接设定视野时长与起点（用于「默认放大到前几小节」）。
- (void)zoomToSpan:(double)spanSec fromStart:(double)startSec
{
    const double dur = _st->backend->durationSec ();
    if (dur <= 0.0) return;
    if (!(spanSec > 0.0)) return;
    if (spanSec > dur) { [self fitAll]; return; }
    _st->viewStart = startSec;
    _st->viewLen = spanSec;
    [self clampView];
    [self refreshPeaks];
}

// 让视野跟随谱面播放头（audioSec 为播放头在音频时间轴上的位置）。
//
// 【反抽搐设计 · 定稿规则】画面**永远不会把播放头「拽」回来**，只有一条规则：
//   · 用户正在手动操作（userScrolling）→ 完全不干预。播放头允许呆在画面之外。
//     旧版在这里加了「播放头完全跑出画面就强制拉回」，于是用户往左拖、画面被
//     拽回右，来回拉锯 —— 视觉上就是抽搐（用户实测反馈的正是这个症状）。
//   · 播放头在画面内 / 刚擦出边缘 → 顺滑向右滚，让它落在画面 25% 处。
//   · 播放头远在画面之外（用户把视野拖到别处看）→ 什么都不做。想回来看播放头
//     就点「回到播放头」—— 该按钮存在就是为了这件事，比偷偷自动跳转可预期。
//   · 唯一例外 allowJump：**宿主自己**把播放头挪了（点小节 / 循环回卷 / 拖播放
//     头）。这种跳变若不让画面跟过去，用户会「找不到播放头」，所以允许跟随。
//     allowJump 由 tick 用「相邻两帧的谱面位置差」判定，见 kHostJumpSec。
- (void)followPlayheadTo:(double)audioSec allowJump:(BOOL)allowJump
{
    if (!_st->followScroll) return;
    if (_st->panning || _st->draggingGrid) return;
    if (!(_st->viewLen > 0.0)) return;

    // ① 用户在看别处 → 不干预（宿主主动跳转时例外：那种情况用户正等着画面跟过去）
    if (_st->userScrolling && !allowJump) return;

    const double v0 = _st->viewStart;
    const double v1 = v0 + _st->viewLen;
    const double margin = _st->viewLen * 0.25;   // 播放头离边缘多少时开始滚

    // ② 远在画面之外、且不是宿主跳转 → 不追。这是「不抢用户视野」的关键一条。
    //    容差取一个整屏：极端放大时播放头两帧之间就能移动大半屏，容差太小会漏跟。
    if (!allowJump && (audioSec < v0 - _st->viewLen || audioSec > v1 + _st->viewLen))
        return;

    // ③ 播放头落在画面左侧（循环回卷 / 向前跳转）→ 对到 25% 处，让它重新可见
    if (audioSec < v0)
    {
        _st->viewStart = audioSec - margin;
        [self clampView];
        [self setNeedsDisplay:YES];
        return;
    }

    // ④ 播放头快到右边缘 → 顺滑向右滚
    if (audioSec > v1 - margin)
    {
        _st->viewStart = audioSec - margin;
        [self clampView];
        [self setNeedsDisplay:YES];   // 重取样交给 drawRect（20Hz 触发，避免高频持锁）
    }
}

- (void)zoomBy:(double)factor aroundX:(CGFloat)x
{
    const NSRect r = [self bounds];
    if (NSWidth (r) <= 0.0 || !(_st->viewLen > 0.0)) return;
    if (factor <= 0.0) return;

    // ⭐ 缩放也算「用户在看别处」：否则点「放大/缩小」按钮时（它们不经过
    //    scrollWheel，过去不会置 userScrolling）播放头会被挤出画面，紧接着
    //    被自动跟随拽回去 → 画面抽搐。放在这里就不漏任何缩放入口。
    _st->userScrolling = true;
    _st->lastUserScrollAt = [NSProcessInfo processInfo].systemUptime;

    const double frac = std::min (1.0, std::max (0.0, (double) ((x - NSMinX (r)) / NSWidth (r))));
    const double anchor = _st->viewStart + frac * _st->viewLen;

    double newLen = _st->viewLen / factor;
    _st->viewLen = newLen;
    _st->viewStart = anchor - frac * newLen;
    [self clampView];
    // 不在这里重取样：交给 drawRect（下一帧）统一做，避免高频持锁打断音频线程。
    [self setNeedsDisplay:YES];
}

//------------------------------------------------------------------------------
// 峰值取样：只取当前可见区间。放大后必须重新取样，否则波峰糊成一片。
//
// 【节流】手势事件会高频触发重绘，但真正的重取样只在「视野变化」时发生，
// 且同一视野内不重复取。这样每个绘制帧至多持锁一次，音频线程几乎不被打断。
//------------------------------------------------------------------------------
- (void)samplePeaksIfNeeded
{
    [self clampView];

    const double v0 = _st->viewStart;
    const double vl = _st->viewLen;

    // 视野没变且已取过样 → 直接跳过（缓存命中），只保证重绘。
    if (!_st->peaksDirty &&
        _st->peaksViewStart == v0 && _st->peaksViewLen == vl)
    {
        return;
    }

    const double dur = _st->backend->durationSec ();
    if (!_st->backend->hasAudio () || dur <= 0.0 || !(vl > 0.0))
    {
        _st->peaks.clear ();
        _st->peaksViewStart = v0;
        _st->peaksViewLen = vl;
        _st->peaksDirty = false;
        return;
    }

    const NSRect r = [self bounds];
    // 桶数上限从 1400 降到 800：波形横向 612px，600+ 桶已足够细腻，
    // 少一半桶 = 持锁时间减半，进一步降低对音频线程的抢占。
    const int buckets = (int) std::max (60.0, std::min (800.0, NSWidth (r) * 1.3));
    _st->backend->waveformPeaks (_st->peaks, buckets, v0, v0 + vl);

    _st->peaksViewStart = v0;
    _st->peaksViewLen = vl;
    _st->peaksDirty = false;
}

// 对外接口：取样 + 触发重绘（fitAll / zoomToSpan 等一次性切换用）
- (void)refreshPeaks
{
    [self samplePeaksIfNeeded];
    [self setNeedsDisplay:YES];
}

// 标记峰值失效（换文件等外部事件后调用），下次 drawRect 会重取。
- (void)invalidatePeaks
{
    _st->peaksDirty = true;
}

//------------------------------------------------------------------------------
// 拍号条：布局与绘制
//
// 条在波形视图顶部，每个小节一个「N/M」方块；拍号变化点用橙色高亮。
// 点击方块 → 面板弹出该小节的拍号设置（见 TimeSigPopupView）。
//------------------------------------------------------------------------------
- (std::vector<TsBox>)timeSigBoxes
{
    std::vector<TsBox> boxes;
    const NSRect full = [self bounds];
    const double dur = _st->backend->durationSec ();
    const double vl  = (_st->viewLen > 0.0) ? _st->viewLen : dur;
    if (!(vl > 0.0)) return boxes;
    std::vector<ap::BarMark> bars;
    visibleBarsFor (_st->backend, _st->viewStart, vl, bars);
    layoutTsBoxes (bars, full, _st->viewStart, _st->viewStart + vl, boxes);
    return boxes;
}

//------------------------------------------------------------------------------
// 只读自测访问器：给 gui_repro（离屏复现程序）用。
//
// 拍号条的几何是「绘制与点击命中共用同一份」，所以复现程序只要问这几个值就能算出
// 该点哪里 —— 不用在测试里再抄一遍布局常数（抄一遍早晚跟实现漂移）。
//------------------------------------------------------------------------------
- (int)timeSigBoxCount
{
    return (int) [self timeSigBoxes].size ();
}

- (int)timeSigBoxBarAtIndex:(int)i
{
    std::vector<TsBox> b = [self timeSigBoxes];
    return (i >= 0 && i < (int) b.size ()) ? b[static_cast<size_t> (i)].bar : -1;
}

- (int)timeSigBoxBeatsAtIndex:(int)i
{
    std::vector<TsBox> b = [self timeSigBoxes];
    return (i >= 0 && i < (int) b.size ()) ? b[static_cast<size_t> (i)].beats : -1;
}

- (int)timeSigBoxDenomAtIndex:(int)i
{
    std::vector<TsBox> b = [self timeSigBoxes];
    return (i >= 0 && i < (int) b.size ()) ? b[static_cast<size_t> (i)].denom : -1;
}

- (BOOL)timeSigBoxChangedAtIndex:(int)i
{
    std::vector<TsBox> b = [self timeSigBoxes];
    return (i >= 0 && i < (int) b.size ()) && b[static_cast<size_t> (i)].changed;
}

- (CGFloat)timeSigBoxMidXAtIndex:(int)i
{
    std::vector<TsBox> b = [self timeSigBoxes];
    if (i < 0 || i >= (int) b.size ()) return -1.0;
    return NSMidX (b[static_cast<size_t> (i)].rect);
}

- (void)drawTimeSigStripIn:(NSRect)full bars:(std::vector<ap::BarMark>&)bars
                     view0:(double)v0 view1:(double)v1
{
    // 条的底色 + 下沿分隔线（把「条」和「波形区」分开）
    [[NSColor colorWithCalibratedWhite:0.085 alpha:1.0] setFill];
    NSRectFill (NSMakeRect (NSMinX (full), NSMinY (full), NSWidth (full), kTsStripH));
    [[NSColor colorWithCalibratedWhite:0.24 alpha:1.0] setFill];
    NSRectFill (NSMakeRect (NSMinX (full), NSMinY (full) + kTsStripH - 1.0, NSWidth (full), 1.0));

    std::vector<TsBox> boxes;
    layoutTsBoxes (bars, full, v0, v1, boxes);

    NSDictionary* light = @{ NSFontAttributeName : [NSFont systemFontOfSize:9],
                             NSForegroundColorAttributeName :
                                 [NSColor colorWithCalibratedWhite:0.90 alpha:1.0] };
    NSDictionary* dark  = @{ NSFontAttributeName : [NSFont systemFontOfSize:9],
                             NSForegroundColorAttributeName :
                                 [NSColor colorWithCalibratedWhite:0.10 alpha:1.0] };

    for (const TsBox& b : boxes)
    {
        if (b.changed)
        {
            [[NSColor colorWithCalibratedRed:0.98 green:0.68 blue:0.20 alpha:1.0] setFill];
            [[NSColor colorWithCalibratedRed:1.00 green:0.86 blue:0.48 alpha:1.0] setStroke];
        }
        else
        {
            [[NSColor colorWithCalibratedWhite:0.30 alpha:1.0] setFill];
            [[NSColor colorWithCalibratedWhite:0.44 alpha:1.0] setStroke];
        }
        NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:b.rect xRadius:3.0 yRadius:3.0];
        [p fill];
        [p setLineWidth:1.0];
        [p stroke];

        NSString*     label = [NSString stringWithFormat:@"%d/%d", b.beats, b.denom];
        NSDictionary* a     = b.changed ? dark : light;
        NSSize        ts    = [label sizeWithAttributes:a];
        [label drawAtPoint:NSMakePoint (NSMidX (b.rect) - ts.width / 2.0,
                                        NSMidY (b.rect) - ts.height / 2.0)
            withAttributes:a];
    }
}

- (void)drawRect:(NSRect)dirty
{
    // 整块视图 = 顶部拍号条 + 下方波形区。拍号条是从视图内部【让出来】的，
    // 视图自身的 frame 不动 —— 面板高度固定 384、每一行的纵向位置都按它排，
    // 动 frame 会连带动布局、帮助覆盖层行数预算与验证器。
    const NSRect rFull = [self bounds];
    const NSRect r = NSMakeRect (NSMinX (rFull), NSMinY (rFull) + kTsStripH,
                                 NSWidth (rFull),
                                 std::max<CGFloat> (0.0, NSHeight (rFull) - kTsStripH));
    NSColor* bg    = [NSColor colorWithCalibratedWhite:0.16 alpha:1.0];
    NSColor* wave  = [NSColor colorWithCalibratedRed:0.30 green:0.72 blue:0.98 alpha:1.0];
    NSColor* head  = [NSColor colorWithCalibratedRed:1.0 green:0.35 blue:0.30 alpha:1.0];
    NSColor* score = [NSColor colorWithCalibratedRed:0.45 green:0.95 blue:0.45 alpha:1.0];
    NSColor* midC  = [NSColor colorWithCalibratedWhite:1.0 alpha:0.12];
    NSColor* barC  = [NSColor colorWithCalibratedWhite:0.85 alpha:0.75];
    NSColor* beatC = [NSColor colorWithCalibratedWhite:0.70 alpha:0.32];
    NSColor* numC  = [NSColor colorWithCalibratedRed:0.98 green:0.86 blue:0.35 alpha:1.0];
    NSColor* txt   = [NSColor colorWithCalibratedWhite:0.65 alpha:1.0];

    [bg setFill];
    NSRectFill (rFull);   // 背景铺满整块视图（含拍号条），免得条下露出面板底色

    if (!_st->backend->hasAudio ())
    {
        NSString* msg = @"把音频文件拖到这里";
        NSDictionary* attrs = @{ NSFontAttributeName : [NSFont systemFontOfSize:13],
                                 NSForegroundColorAttributeName : txt };
        NSSize sz = [msg sizeWithAttributes:attrs];
        [msg drawAtPoint:NSMakePoint (NSMidX (r) - sz.width / 2, NSMidY (r) - sz.height / 2)
           withAttributes:attrs];
        return;
    }

    [self clampView];

    // 视野变了才重取样（内部有缓存命中判断），保证每个绘制帧至多取一次。
    [self samplePeaksIfNeeded];

    const double dur = _st->backend->durationSec ();
    const double v0  = _st->viewStart;
    const double vl  = (_st->viewLen > 0.0) ? _st->viewLen : dur;
    const double v1  = v0 + vl;
    const double off = _st->backend->offsetSec ();

    const CGFloat W = NSWidth (r);
    const CGFloat H = NSHeight (r);
    const CGFloat mid = NSMidY (r);
    const CGFloat halfH = (H - 8) / 2;

    // 时间（秒）→ 视图 x
    const double pxPerSec = (vl > 0.0) ? (double) W / vl : 0.0;
    auto X = [&] (double t) -> CGFloat
    {
        return NSMinX (r) + (CGFloat) ((t - v0) * pxPerSec);
    };

    // ---- 中线 ----
    [midC setStroke];
    NSBezierPath* midPath = [NSBezierPath bezierPath];
    [midPath moveToPoint:NSMakePoint (NSMinX (r), mid)];
    [midPath lineToPoint:NSMakePoint (NSMaxX (r), mid)];
    midPath.lineWidth = 1.0;
    [midPath stroke];

    // ---- 跳过的前奏（偏移 > 0 时音频开头这一截不会被播放）----
    if (off > 0.0)
    {
        const CGFloat x0 = X (0.0);
        const CGFloat x1 = X (off);
        const CGFloat a = std::max (NSMinX (r), std::min (x0, x1));
        const CGFloat b = std::min (NSMaxX (r), std::max (x0, x1));
        if (b > a)
        {
            [[NSColor colorWithCalibratedRed:1.0 green:0.75 blue:0.25 alpha:0.13] setFill];
            NSRectFill (NSMakeRect (a, NSMinY (r), b - a, H));
        }
    }

    // ---- 波形 ----
    const CGFloat n = (CGFloat) _st->peaks.size ();
    if (n > 0)
    {
        [wave setStroke];
        NSBezierPath* p = [NSBezierPath bezierPath];
        for (CGFloat i = 0; i < n; ++i)
        {
            const CGFloat x = NSMinX (r) + ((i + 0.5) / n) * W;
            const CGFloat a = (CGFloat) _st->peaks[(size_t) i] * halfH;
            [p moveToPoint:NSMakePoint (x, mid - a)];
            [p lineToPoint:NSMakePoint (x, mid + a)];
        }
        p.lineWidth = 1.0;
        [p stroke];
    }

    // ---- 小节网格 ----
    // 网格线（拍线/小节线）和小节号是两个独立开关：showGrid 控制线，showNumbers
    // 控制数字。两者任意一个开启就要做网格计算，但绘制各自受自己的开关约束。
    //
    // ⚠️ 小节长度【逐小节算】：谱子里有拍号变化时，从变化点起每小节的长度就
    //    不一样了（4/4 是 2 秒，转 3/4 之后是 1.5 秒）。拿一个全曲统一的 barSec
    //    去铺线，变化点之后的小节线会全部错位。推进算法见 gui.h 的 ap::buildBars()。
    std::vector<ap::BarMark> bars;
    visibleBarsFor (_st->backend, v0, vl, bars);

    // 视野里小节太多就不画（几万条线没意义还卡）
    const bool gridDrawable = (bars.size () < 4096) && ((double) W > 0.0);

    if (gridDrawable && _st->showGrid)
    {
        // 拍线（虚线）
        [beatC setStroke];
        NSBezierPath* bp = [NSBezierPath bezierPath];
        bp.lineWidth = 1.0;
        const CGFloat dash[2] = { 3.0, 3.0 };
        [bp setLineDash:dash count:2 phase:0.0];
        for (const ap::BarMark& m : bars)
        {
            if (m.beatSec * pxPerSec < 5.0) continue;   // 拍线太密 → 只留小节线，免得糊成一片
            for (int q = 1; q < m.beats; ++q)
            {
                const double t = m.startSec + q * m.beatSec;
                if (t < v0 || t > v1) continue;
                const CGFloat x = X (t);
                [bp moveToPoint:NSMakePoint (x, NSMinY (r) + 11)];
                [bp lineToPoint:NSMakePoint (x, NSMaxY (r))];
            }
        }
        [bp stroke];

        // 小节线（实线）
        [barC setStroke];
        NSBezierPath* barP = [NSBezierPath bezierPath];
        barP.lineWidth = 1.0;
        for (const ap::BarMark& m : bars)
        {
            if (m.startSec < v0 || m.startSec > v1) continue;
            const CGFloat x = X (m.startSec);
            [barP moveToPoint:NSMakePoint (x, NSMinY (r) + 11)];
            [barP lineToPoint:NSMakePoint (x, NSMaxY (r))];
        }
        [barP stroke];
    }

    // 小节号：独立开关，不依赖 showGrid。
    // ⚠️ 小节宽度可变后，「每隔 N 个标一个」的固定步长不再适用 —— 改成
    //    「与上一个标注至少隔开 42px 才标」，宽窄自动适应。
    if (gridDrawable && _st->showNumbers)
    {
        NSDictionary* attrs = @{ NSFontAttributeName : [NSFont systemFontOfSize:9],
                                 NSForegroundColorAttributeName : numC };
        CGFloat lastLabelX = -1.0e9;
        for (const ap::BarMark& m : bars)
        {
            if (m.startSec < v0 || m.startSec > v1) continue;
            const CGFloat x = X (m.startSec);
            if (x < NSMinX (r) - 2 || x > NSMaxX (r) - 8) continue;
            if (x - lastLabelX < 42.0) continue;
            lastLabelX = x;
            NSString* s = [NSString stringWithFormat:@"%d", m.bar];
            [s drawAtPoint:NSMakePoint (x + 2, NSMinY (r) + 1) withAttributes:attrs];
        }
    }

    // ---- 顶部拍号条 ----
    [self drawTimeSigStripIn:rFull bars:bars view0:v0 view1:v1];

    // ---- 谱面播放头（宿主给位置时才有）----
    // 绿线 = 谱面当前播到的位置（投影到音频时间轴，含偏移），
    // 红线 = 音频实际播到的位置。
    //
    // ⚠️ 这两条线【平时必然重合】，别拿它当「偏移调对了没」的判据：
    //    绿线画在 (谱面位置 + 偏移)、音频也被插件驱动到 (谱面位置 + 偏移)，
    //    两边同时随偏移平移 → 拖偏移时它们【各自都动，间距不动】。
    //    所以「故意把偏移调错，两线还是黏在一起」是设计使然，不是 bug。
    //    它们分开了 = 音频线程没跟上（丢块/跟随失效），是【故障指示】，
    //    不是对齐指示 —— 点「回到播放头」修复。
    //    判断偏移对不对，看【网格小节线有没有落在波形上的鼓点】。
    const ap::PlugView::Backend::HostTimeline tl = _st->backend->hostTimeline ();
    if (tl.playheadValid)
    {
        // 谱面时间 → 音频时间：谱面 0 秒 = 音频 off 秒
        const double audioT = tl.playheadSec + off;
        if (audioT >= v0 && audioT <= v1)
        {
            const CGFloat x = X (audioT);
            [score setStroke];
            NSBezierPath* sp = [NSBezierPath bezierPath];
            [sp moveToPoint:NSMakePoint (x, NSMinY (r))];
            [sp lineToPoint:NSMakePoint (x, NSMaxY (r))];
            sp.lineWidth = 1.5;
            [sp stroke];

            // 顶部小标签「谱面」，和红线（音频）区分开
            NSDictionary* attrs = @{ NSFontAttributeName : [NSFont systemFontOfSize:9],
                                     NSForegroundColorAttributeName : score };
            [@"谱面" drawAtPoint:NSMakePoint (x + 2, NSMinY (r) + 1) withAttributes:attrs];
        }
    }

    // ---- 音频播放头 ----
    const double pos = _st->backend->positionSec ();
    if (pos >= v0 && pos <= v1)
    {
        const CGFloat x = X (pos);
        [head setStroke];
        NSBezierPath* hp = [NSBezierPath bezierPath];
        [hp moveToPoint:NSMakePoint (x, NSMinY (r))];
        [hp lineToPoint:NSMakePoint (x, NSMaxY (r))];
        hp.lineWidth = 1.5;
        [hp stroke];

        NSDictionary* attrs = @{ NSFontAttributeName : [NSFont systemFontOfSize:9],
                                 NSForegroundColorAttributeName : head };
        [@"音频" drawAtPoint:NSMakePoint (x + 2, NSMinY (r) + 12) withAttributes:attrs];
    }

    // ---- 分家检测徽标 ----
    // 只在「播放中、且不在首尾过渡区」时判定：
    //   · 起播的头 0.3 秒音频还在追，差值天然偏大 → 不算；
    //   · 音频比乐谱短时，尾端必然拉开 → 不算（那不是 bug）。
    // 命中就亮一个红底徽标，把「用户根本看不出来的分家」变成一句明确指令。
    {
        const double durW  = _st->backend->durationSec ();
        const bool   midW  = (pos > 0.30 && pos < durW - 0.30);
        if (tl.playing && tl.playheadValid && durW > 0.60 && midW)
        {
            const double diff = pos - (tl.playheadSec + off);
            if (std::fabs (diff) > kDivergenceWarnSec)
            {
                // 刚点过「回到播放头」却还是错位 → 这不是一次性偏差，而是跟随失效。
                // 此时再点按钮也没用，直接告诉用户该重启插件（tick 里已记日志）。
                NSString* warn = _st->followFail
                    ? [NSString stringWithFormat:
                       @"跟随可能已失效 · 建议重启插件（错位 %.2f 秒）", std::fabs (diff)]
                    : [NSString stringWithFormat:
                       @"⚠ 与谱面错位 %.2f 秒 · 点「回到播放头」", std::fabs (diff)];
                NSDictionary* wa = @{ NSFontAttributeName : [NSFont systemFontOfSize:10],
                                      NSForegroundColorAttributeName :
                                          [NSColor colorWithCalibratedWhite:1.0 alpha:1.0] };
                const NSSize ws = [warn sizeWithAttributes:wa];
                const NSRect box = NSMakeRect (NSMaxX (r) - ws.width - 18,
                                               NSMinY (r) + 3,
                                               ws.width + 14,
                                               ws.height + 5);
                NSBezierPath* bp = [NSBezierPath bezierPathWithRoundedRect:box
                                                                   xRadius:4 yRadius:4];
                [[NSColor colorWithCalibratedRed:0.82 green:0.26 blue:0.20 alpha:0.92] setFill];
                [bp fill];
                [warn drawAtPoint:NSMakePoint (box.origin.x + 7, box.origin.y + 2.5)
                   withAttributes:wa];
            }
        }
    }
}

//------------------------------------------------------------------------------
// 交互
//
// 坐标一律用 [self convertPoint:fromView:nil] —— 之前直接用
// locationInWindow.x，只有在视图恰好位于窗口原点时才正确；
// 在 MuseScore 里编辑器被嵌进宿主窗口，位置不定，定位会偏。
//------------------------------------------------------------------------------
- (NSPoint)localPoint:(NSEvent*)e
{
    return [self convertPoint:e.locationInWindow fromView:nil];
}

- (NSEventModifierFlags)mods:(NSEvent*)e
{
    return e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
}

- (void)mouseDown:(NSEvent*)e
{
    if (!_st->backend->hasAudio ()) return;
    [[self window] makeFirstResponder:self];   // 确保后续 magnify/滚动事件能到达本视图
    const NSPoint p = [self localPoint:e];
    const NSEventModifierFlags m = [self mods:e];

    // 顶部拍号条：点中某个方块 = 弹出该小节的拍号设置。
    // 这两种情况都要把点击「吃掉」：命中方块要开面板；没命中也不能掉进下面的
    // 「平移」分支 —— 否则点一下条会把视野挪走，纯属意外。
    if (p.y < NSMinY ([self bounds]) + kTsStripH)
    {
        std::vector<TsBox> boxes = [self timeSigBoxes];
        for (const TsBox& b : boxes)
        {
            if (NSPointInRect (p, NSInsetRect (b.rect, -3.0, -3.0)))
            {
                if (_st->panel)
                    [(__bridge id<WavePanel>) _st->panel openTimeSigPopupForBar:b.bar];
                return;
            }
        }
        return;
    }

    if (e.clickCount >= 2)        // 双击 = 回到全览
    {
        [self fitAll];
        return;
    }

    // ⌥ 拖动 或 ⌘(macOS)/Ctrl(Windows) 拖动 = 改起始偏移（对齐最快的手法）。
    // NSEventModifierFlagCommand 在 macOS 是 ⌘，在 Windows 端映射为 Ctrl。
    if (m & (NSEventModifierFlagOption | NSEventModifierFlagCommand))
    {
        _st->draggingGrid = true;
        _st->dragStartX = p.x;
        _st->dragStartOffset = _st->backend->offsetSec ();
        [[NSCursor closedHandCursor] push];
        return;
    }

    // 无修饰键拖动 = 平移视图（抓手），不改变播放位置。
    _st->panning = true;
    _st->panStartX = p.x;
    _st->panStartViewStart = _st->viewStart;
    _st->userScrolling = true;   // 用户在看别处，暂停自动跟随
    _st->lastUserScrollAt = e.timestamp;
    [[NSCursor closedHandCursor] push];
}

- (void)mouseDragged:(NSEvent*)e
{
    const NSPoint p = [self localPoint:e];
    const NSRect r = [self bounds];
    const NSEventModifierFlags m = [self mods:e];

    if (_st->draggingGrid)
    {
        if (!(_st->viewLen > 0.0) || NSWidth (r) <= 0.0) return;
        const double pxPerSec = NSWidth (r) / _st->viewLen;
        double deltaSec = (p.x - _st->dragStartX) / pxPerSec;
        if (m & NSEventModifierFlagShift) deltaSec *= 0.1;   // ⇧ = 微调
        double off = (double) _st->dragStartOffset + deltaSec;
        _st->backend->setOffsetSec ((float) off);
        if (_st->panel)
            [(__bridge id<WavePanel>) _st->panel offsetChangedExternally:off];
        [self setNeedsDisplay:YES];
        return;
    }

    if (_st->panning)
    {
        if (!(_st->viewLen > 0.0) || NSWidth (r) <= 0.0) return;
        const double pxPerSec = NSWidth (r) / _st->viewLen;
        // 抓手语义：把内容往右拉 → 视野往左移，看到更早的音频。
        _st->viewStart = _st->panStartViewStart - (p.x - _st->panStartX) / pxPerSec;
        [self clampView];
        [self setNeedsDisplay:YES];   // 重取样交给 drawRect，避免高频持锁
    }
}

- (void)mouseUp:(NSEvent*)e
{
    if (_st->draggingGrid)
    {
        _st->draggingGrid = false;
        [[NSCursor closedHandCursor] pop];
    }
    if (_st->panning)
    {
        _st->panning = false;
        [[NSCursor closedHandCursor] pop];
    }
}

- (void)stopDrag
{
    // 关窗口 / 换音频时也会走到这里，光标栈必须成对回收，否则残留抓手光标。
    if (_st->panning)      { _st->panning = false;      [[NSCursor closedHandCursor] pop]; }
    if (_st->draggingGrid) { _st->draggingGrid = false; [[NSCursor closedHandCursor] pop]; }
}

// 空格键转发给宿主主窗口。
//
// 背景：MuseScore 的 VST 插件编辑器是独立浮动窗口，被点击后成为 key window，
// 键盘焦点就落在插件上，空格到不了宿主 —— 用户必须先点一下谱面小节把焦点
// 夺回宿主，空格才能触发播放/暂停，这很不符合直觉（用户以为「空格=播放」）。
//
// 插件无法通过 VST3 协议反向控制宿主播放，但「把键盘事件转发回宿主窗口」
// 是纯 AppKit 层面的事，不依赖协议。
//
// 为什么不用 NSApp.mainWindow：插件编辑器窗口在 MuseScore 里很可能就是
// mainWindow（它成为 key 后 mainWindow 也跟着变），拿它转发等于自己发给自己。
// 正确做法是遍历 NSApp.windows，排除掉「插件自身的窗口」，把空格发给其余
// 那个可接受键盘的宿主窗口（MuseScore 主文档窗口）。
- (void)keyDown:(NSEvent*)e
{
    if (e.keyCode == 49)   // 空格键
    {
        NSWindow* pluginWindow = [self window];
        // 遍历所有窗口，找宿主窗口（不是插件自身、且能成为 key window 的那个）
        for (NSWindow* w in [NSApp windows])
        {
            if (w == pluginWindow) continue;
            if (!w.isVisible) continue;
            // 只挑能成为 key window 的（宿主主文档窗口符合，工具面板/浮层不符合）
            if (![w canBecomeKeyWindow]) continue;
            // 跳过标题为空或明显是插件子窗口的
            if (w.title.length == 0) continue;

            [w makeKeyAndOrderFront:nil];   // 先激活宿主窗口，让空格落进去
            [w sendEvent:e];
            return;
        }
        // 没找到宿主窗口：退回响应链默认处理
        [super keyDown:e];
        return;
    }
    [super keyDown:e];
}

// 滚轮/双指：平移；⌥（或 ⌘）滚轮：以鼠标位置为锚点缩放
- (void)scrollWheel:(NSEvent*)e
{
    if (!_st->backend->hasAudio ()) return;
    const NSRect r = [self bounds];
    if (!(_st->viewLen > 0.0) || NSWidth (r) <= 0.0) return;
    const NSEventModifierFlags m = [self mods:e];
    const NSPoint p = [self localPoint:e];

    _st->userScrolling = true;   // 用户主动滚了，暂停舒适区内的自动跟随
    _st->lastUserScrollAt = e.timestamp;

    if (m & (NSEventModifierFlagOption | NSEventModifierFlagCommand))
    {
        const double d = (double) e.scrollingDeltaY;
        if (d == 0.0) return;
        const double factor = std::pow (1.35, d / 8.0);
        [self zoomBy:factor aroundX:p.x];
        return;
    }

    // 触控板双指捏合（magnify）在 AppKit 里，若视图没能收到 magnify 事件，
    // 会降级成「无修饰键的 scrollWheel 且 scrollingDeltaY 非零」。这里兜底：
    // 无修饰键且纵向滚动量明显大于横向（且没有 deltaX 平移意图），当作缩放。
    const double dx = (double) e.scrollingDeltaX;
    const double dy = (double) e.scrollingDeltaY;
    if (std::fabs (dy) > std::fabs (dx) * 1.5 && std::fabs (dy) > 0.5)
    {
        const double factor = std::pow (1.35, dy / 8.0);
        [self zoomBy:factor aroundX:p.x];
        return;
    }

    const double pxPerSec = NSWidth (r) / _st->viewLen;
    const double dt = -(double) e.scrollingDeltaX / pxPerSec;
    if (dt != 0.0)
    {
        _st->viewStart += dt;
        [self clampView];
        [self setNeedsDisplay:YES];   // 重取样交给 drawRect，避免高频持锁
    }
}

- (void)magnifyWithEvent:(NSEvent*)e
{
    if (!_st->backend->hasAudio ()) return;
    const double factor = 1.0 + (double) e.magnification;
    if (factor <= 0.0) return;
    const NSPoint p = [self localPoint:e];
    _st->userScrolling = true;   // 手动缩放后暂停自动跟随
    _st->lastUserScrollAt = e.timestamp;
    [self zoomBy:factor aroundX:p.x];
}

// 用户停止手动滚/拖超过 2 秒后，自动恢复「视野跟随播放头」。
// 之前 userScrolling 一旦置 true 就永久停跟随，用户滚一下之后就不再跟了。
- (void)recoverAutoFollow
{
    if (!_st->userScrolling) return;
    if (_st->panning || _st->draggingGrid) return;   // 还在拖，别打断
    const double now = [NSProcessInfo processInfo].systemUptime;
    // e.timestamp 是 NSTimeInterval（进程启动后的秒数），与 systemUptime 同源，
    // 可直接相减。不用 CACurrentMediaTime —— 那要额外链 QuartzCore 框架。
    if (now - _st->lastUserScrollAt >= 2.0)
        _st->userScrolling = false;
}

@end

//------------------------------------------------------------------------------
// 作者主页跳转（实现在文件末尾的 namespace ap 里）
//
// 这里先声明：BrandLinkField 在下面就要用它，而定义跟 PlugView 一起放在文件末尾。
//------------------------------------------------------------------------------
namespace ap { void openBrandHome (); }

//------------------------------------------------------------------------------
// 可点击的页脚宣传语（B 站署名）
//
// 需求：点一下底部那条粉色宣传语，用系统默认浏览器打开作者的 B 站主页。
//
// 为什么是 NSTextField 子类而不是 NSButton：
//   NSButton 自带边框与按下高亮背景，会把页脚那一行「顺着底边的一行粉色小字」
//   变成一枚突兀的按钮，破坏原有观感。自绘文本 + 手型光标最贴近原来的样子，
//   用户看不出区别，只有鼠标移上去才发现「咦，能点」。
//
// 为什么重写 acceptsFirstMouse: 而不是只写 mouseDown:
//   插件编辑器在 MuseScore 里是独立浮动窗口。窗口未激活时，AppKit 默认会把
//   第一次点击【只用来激活窗口】而不派发给控件 —— 用户会遇到「第一次点没反应，
//   得点第二次」。返回 YES 让第一次点击就生效。
//
// ⚠️ 别把它换成普通 NSTextField：`labelWithString:` 造出来的label 是
//    non-editable / non-selectable 的，点上去没有任何反馈，也没人会想到要
//    在父视图上做命中测试 —— 这功能会「看起来写了但其实点不动」。
//------------------------------------------------------------------------------
@interface BrandLinkField : NSTextField
@end

@implementation BrandLinkField

/// 窗口未激活时，第一次点击也要直接生效（不要被「先激活窗口」吃掉）
- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
    (void) event;
    return YES;
}

/// 鼠标移上去显示手型光标 —— 这是用户唯一能察觉「这里可以点」的线索
- (void)resetCursorRects
{
    [self addCursorRect:self.bounds cursor:[NSCursor pointingHandCursor]];
}

- (void)mouseDown:(NSEvent*)event
{
    (void) event;
    ap::openBrandHome ();
}

/// 只读（自测用）：本控件点击后会打开的地址。
/// 自测用它确认「页脚确实接到了对的主页」—— 绝不能靠真的调用 mouseDown: 来验，
/// 那会把浏览器弹出来（自动化测试里拉起浏览器是不可接受的副作用）。
- (NSString*)brandUrl
{
    return [NSString stringWithUTF8String:ap::kBrandHomeUrl];
}

@end

//------------------------------------------------------------------------------
// 使用指南覆盖层
//
// 面板只有 640x384，完整的操作说明塞不进常驻布局（挤掉的会是波形区）。
// 所以做成「帮助」按钮唤出的覆盖层：铺满整个面板、点任意处关闭，
// 既不影响常态布局，也保证第一次用的人一定能找到说明书。
//
// 实现要点：它必须作为 GUIView 的【最后一个子视图】加入，这样在子视图顺序上
// 位于所有控件之上；再把 frame 设成父视图 bounds，就能盖住整块面板并在命中
// 测试里优先接住点击（下面的控件因此不会被误触）。
//------------------------------------------------------------------------------
@interface HelpOverlayView : NSView
@end

@implementation HelpOverlayView

- (BOOL)isOpaque { return YES; }        // 铺满整块面板，可以声明不透明
- (BOOL)isFlipped { return YES; }       // 与父视图一致：y 轴向下

// 窗口不是 key window 时，第一次点击默认只用于激活窗口。这里要求接住它，
// 否则「点一下关掉指南」会出现点了没反应、要点第二次的情况。
- (BOOL)acceptsFirstMouse:(NSEvent*)e { (void) e; return YES; }

- (void)mouseDown:(NSEvent*)e       { (void) e; self.hidden = YES; }
- (void)rightMouseDown:(NSEvent*)e  { (void) e; self.hidden = YES; }

// 滚轮【只吞掉、不关闭】：指南就是固定的一屏文字，用户却习惯往下滚一滚看看
// 还有没有（Windows 端实测反馈过「怕下面还有东西，一滚帮助就没了」）。
// 不实现这条消息时，事件会沿 responder chain 上抛，可能顺带把下面的波形视野
// 也滚走 —— 用户关掉指南才发现画面不在原处。这里一次解决这两件事。
- (void)scrollWheel:(NSEvent*)e     { (void) e; }

- (void)drawRect:(NSRect)dirty
{
    (void) dirty;
    const NSRect r = [self bounds];

    [[NSColor colorWithCalibratedRed:0.09 green:0.10 blue:0.13 alpha:1.0] setFill];
    NSRectFill (r);

    NSColor* accent = [NSColor colorWithCalibratedRed:0.55 green:0.85 blue:0.55 alpha:1.0];
    NSDictionary* hAttrs = @{ NSFontAttributeName : [NSFont boldSystemFontOfSize:13],
                              NSForegroundColorAttributeName : accent };
    NSDictionary* bAttrs = @{ NSFontAttributeName : [NSFont systemFontOfSize:12],
                              NSForegroundColorAttributeName :
                                  [NSColor colorWithCalibratedWhite:0.88 alpha:1.0] };
    NSDictionary* dAttrs = @{ NSFontAttributeName : [NSFont systemFontOfSize:11],
                              NSForegroundColorAttributeName :
                                  [NSColor colorWithCalibratedWhite:0.52 alpha:1.0] };

    [@"使用指南" drawAtPoint:NSMakePoint (26, 10) withAttributes:hAttrs];
    [@"（点一下关闭）" drawAtPoint:NSMakePoint (100, 14) withAttributes:dAttrs];

    [[NSColor colorWithCalibratedWhite:0.30 alpha:1.0] setFill];
    NSRectFill (NSMakeRect (26, 32, NSWidth (r) - 52, 1));

    // 行首 "#" = 小标题（去掉井号后画）；其余为正文，统一缩进两格。
    //
    // 排序 = 【实际操作顺序】，不是功能罗列：装音频 → 填速度 → 对拍子 →
    // 波形区手势 → 出问题怎么办 → 免费/求关注。
    // 文案只讲「怎么点、怎么拖」，不讲原理 —— 目标用户是鼓手不是程序员。
    // ⚠️ 快捷键必须与本端真实实现一致（mac 有 ⌘/⌥、触摸板手势、双击全览；
    //    Win/Linux 是 Ctrl/Alt 且【Linux 没有双击全览】—— 别三端照抄）。
    // ⚠️ 绝不要写「红绿两线重合即对齐」：两条线【本来就是重合的】
    //    （音频位置 = 谱面位置 + 偏移，永远如此），重合与否跟偏移调没调对
    //    毫无关系 —— 它只在「跟随出故障」时才分开。判断是否调好偏移，
    //    唯一的依据是【网格小节线有没有落在波形上的鼓点/第一拍】。
    NSArray<NSString*>* guide = @[
        @"#1  装音频：点「打开音频」，或把音频文件直接拖进窗口。",
        @"     支持 MP3 / WAV / M4A / AAC / FLAC / OGG。装好后在乐谱里",
        @"     按空格播放。",
        @"",
        @"#2  填速度：先填 BPM 和拍号（如 120、4/4），网格小节线才会",
        @"     跟谱子的小节对上。留空 = 自动：跟着乐谱走，乐谱没给就 120。",
        @"     谱子中途换拍号时，点波形顶部的拍号块改那一小节（选回原值即取消）。",
        @"",
        @"#3  对拍子：按住 ⌘（或 ⌥）在波形上左右拖网格，把「1」那条",
        @"     小节线拖到音乐的第一拍上（对准波形里的鼓点）。⇧ 拖 = 微调，",
        @"     调坏了点右边的「归零」。开头被跳过的那段画成灰色。",
        @"",
        @"#4  波形区其它操作：直接拖 = 平移视野；滚轮 / 双指左右滑 = 平移；",
        @"     ⌘/⌥+滚轮 或 双指捏合 = 缩放；双击 = 整首全览。",
        @"",
        @"#5  在乐谱里点任意小节，画面会跟着跳过去（扒谱很方便）。",
        @"     红线=音频播到哪，绿线=乐谱播到哪 —— 平时它俩就黏在一起；",
        @"     分开或找不到播放头就点「回到播放头」，还不行 ⌘Q 重开。",
        @"",
        @"完全免费，只为方便大家制谱、练鼓。顺手的话点一下界面底部的",
        @"「大伟鼓谱」，到 B 站关注一下 —— 那就是最大的支持。",
    ];

    // 21 行 × 16px，从 y=38 起 → 末行底 ≈ 371，留 13px 余量（面板高 384）。
    // ⚠️ 文案再加长就要同步这里，否则最后一行会被面板底边裁掉。
    CGFloat y = 38.0;
    const CGFloat lh = 16.0;
    for (NSString* line in guide)
    {
        if (line.length > 0)
        {
            if ([line hasPrefix:@"#"])
                [[line substringFromIndex:1] drawAtPoint:NSMakePoint (26, y)
                                          withAttributes:hAttrs];
            else
                [line drawAtPoint:NSMakePoint (26, y) withAttributes:bAttrs];
        }
        y += lh;
    }
}

@end

//------------------------------------------------------------------------------
// 拍号设置覆盖层（点了拍号条上的方块之后弹出）
//
// 铺满整块面板的半透明遮罩 + 居中一块小面板：「分子」「分母」两排按钮，
// 外加「取消该小节的变化」。点面板外任意处关闭。
//
// 为什么做成覆盖层而不是两个常驻下拉：面板只有 640x384，常驻两个下拉要再从
// 波形区里挖地方；而「点方块 → 就地改这一小节」也比「先在别处选中再改」直观。
// 挂载方式照搬使用指南覆盖层（作为父视图【最后一个】子视图 + 显示时显式提到
// 最前），不依赖「z 序 = 创建顺序」在宿主里的表现（铁律 15）。
//------------------------------------------------------------------------------
struct TsBtn
{
    NSRect rect  = NSZeroRect;
    int    kind  = 0;   ///< 0 = 分子选项，1 = 分母选项，2 = 取消该小节的变化
    int    value = 0;   ///< 分子 / 分母的取值
};

struct TsPopupState
{
    int    bar       = 1;
    int    beats     = 4;
    int    denom     = 4;
    bool   hasChange = false;
    NSRect box       = NSZeroRect;
    std::vector<TsBtn> btns;
};

// 供面板（GUIView）实现的回调
@protocol TimeSigPopupDelegate <NSObject>
- (void)timeSigPopupApplyBar:(int)bar beats:(int)beats denom:(int)denom;
- (void)timeSigPopupRemoveBar:(int)bar;
- (void)timeSigPopupClosed;
@end

// 候选拍号（与参考工具一致）：分子取常用值，分母只留 2/4/8/16。
static const int kTsBeatChoices[]  = { 2, 3, 4, 5, 6, 7, 9, 12 };
static const int kTsDenomChoices[] = { 2, 4, 8, 16 };

static const CGFloat kTsPopupHeadH = 30.0;   ///< 标题行高
static const CGFloat kTsPopupRowH  = 30.0;   ///< 每排按钮占的行高

@interface TimeSigPopupView : NSView
{
    TsPopupState* _ps;
    __weak id<TimeSigPopupDelegate> _delegate;
}
- (instancetype)initWithDelegate:(id<TimeSigPopupDelegate>)d;
- (void)showForBar:(int)bar beats:(int)beats denom:(int)denom hasChange:(BOOL)hasChange;
@end

@implementation TimeSigPopupView

- (BOOL)isOpaque { return NO; }        // 半透明遮罩，别谎报不透明
- (BOOL)isFlipped { return YES; }       // 与父视图一致：y 轴向下

- (instancetype)initWithDelegate:(id<TimeSigPopupDelegate>)d
{
    if (self = [super initWithFrame:NSZeroRect])
    {
        _ps = new TsPopupState ();
        _delegate = d;
        self.hidden = YES;
    }
    return self;
}

- (void)dealloc
{
    delete _ps;   // ARC 不管 C++ 成员
}

// 按钮布局：绘制与命中测试共用同一份，避免两处各算一遍写歪
- (void)layoutButtons
{
    _ps->btns.clear ();

    const CGFloat padX = 14.0, btnW = 34.0, btnH = 22.0, gap = 5.0, labelW = 34.0;
    const int nBeat  = (int) (sizeof (kTsBeatChoices)  / sizeof (kTsBeatChoices[0]));
    const int nDenom = (int) (sizeof (kTsDenomChoices) / sizeof (kTsDenomChoices[0]));

    const CGFloat row1W = labelW + 6.0 + nBeat  * (btnW + gap) - gap;
    const CGFloat row2W = labelW + 6.0 + nDenom * (btnW + gap) - gap;
    const CGFloat boxW  = std::max (row1W, row2W) + padX * 2.0;
    const CGFloat boxH  = kTsPopupHeadH + kTsPopupRowH * 2.0
                        + (_ps->hasChange ? 26.0 : 0.0) + 14.0;

    const NSRect r = [self bounds];
    _ps->box = NSMakeRect (NSMidX (r) - boxW / 2.0, NSMidY (r) - boxH / 2.0, boxW, boxH);

    const CGFloat startX = _ps->box.origin.x + padX + labelW + 6.0;
    CGFloat y = _ps->box.origin.y + kTsPopupHeadH;
    CGFloat x = startX;
    for (int i = 0; i < nBeat; ++i)
    {
        TsBtn b; b.rect = NSMakeRect (x, y, btnW, btnH); b.kind = 0; b.value = kTsBeatChoices[i];
        _ps->btns.push_back (b);
        x += btnW + gap;
    }
    y += kTsPopupRowH;
    x = startX;
    for (int i = 0; i < nDenom; ++i)
    {
        TsBtn b; b.rect = NSMakeRect (x, y, btnW, btnH); b.kind = 1; b.value = kTsDenomChoices[i];
        _ps->btns.push_back (b);
        x += btnW + gap;
    }
    if (_ps->hasChange)
    {
        y += kTsPopupRowH;
        TsBtn b;
        b.rect  = NSMakeRect (_ps->box.origin.x + padX, y, boxW - padX * 2.0, 22.0);
        b.kind  = 2;
        b.value = 0;
        _ps->btns.push_back (b);
    }
}

- (void)showForBar:(int)bar beats:(int)beats denom:(int)denom hasChange:(BOOL)hasChange
{
    _ps->bar       = bar;
    _ps->beats     = beats;
    _ps->denom     = denom;
    _ps->hasChange = hasChange;
    [self layoutButtons];
    self.hidden = NO;
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent*)e
{
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    for (const TsBtn& b : _ps->btns)
    {
        if (!NSPointInRect (p, NSInsetRect (b.rect, -2.0, -2.0))) continue;
        if (b.kind == 0)
            [_delegate timeSigPopupApplyBar:_ps->bar beats:b.value denom:_ps->denom];
        else if (b.kind == 1)
            [_delegate timeSigPopupApplyBar:_ps->bar beats:_ps->beats denom:b.value];
        else
            [_delegate timeSigPopupRemoveBar:_ps->bar];
        return;
    }
    self.hidden = YES;                 // 点面板外 = 关闭
    [_delegate timeSigPopupClosed];
}

- (void)rightMouseDown:(NSEvent*)e
{
    (void) e;
    self.hidden = YES;
    [_delegate timeSigPopupClosed];
}

// 滚轮【只吞不关】：用户习惯滚一滚看还有没有，别一滚就关掉。
// 不实现这条消息时事件会沿 responder chain 上抛，顺带把下面的波形视野滚走。
- (void)scrollWheel:(NSEvent*)e { (void) e; }

// 自测访问器：某个候选按钮的中心点（弹层视图坐标 = 面板坐标）。见 WaveformView
// 那几个访问器的说明 —— 复现程序靠它算出「该点哪里」，不抄布局常数。
- (NSPoint)tsButtonMidPointForKind:(int)kind value:(int)value
{
    for (const TsBtn& b : _ps->btns)
        if (b.kind == kind && b.value == value)
            return NSMakePoint (NSMidX (b.rect), NSMidY (b.rect));
    return NSMakePoint (-1.0, -1.0);
}

- (void)drawRect:(NSRect)dirty
{
    (void) dirty;
    const NSRect r = [self bounds];

    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.55] setFill];
    NSRectFill (r);

    [[NSColor colorWithCalibratedWhite:0.16 alpha:1.0] setFill];
    [[NSColor colorWithCalibratedWhite:0.42 alpha:1.0] setStroke];
    NSBezierPath* boxPath = [NSBezierPath bezierPathWithRoundedRect:_ps->box xRadius:6.0 yRadius:6.0];
    [boxPath fill];
    [boxPath setLineWidth:1.0];
    [boxPath stroke];

    NSDictionary* titleAttrs = @{ NSFontAttributeName : [NSFont boldSystemFontOfSize:12],
                                  NSForegroundColorAttributeName :
                                      [NSColor colorWithCalibratedRed:0.98 green:0.72 blue:0.30 alpha:1.0] };
    NSString* title = [NSString stringWithFormat:@"第 %d 小节  拍号", _ps->bar];
    [title drawAtPoint:NSMakePoint (_ps->box.origin.x + 14.0, _ps->box.origin.y + 9.0)
        withAttributes:titleAttrs];

    NSDictionary* labelAttrs = @{ NSFontAttributeName : [NSFont systemFontOfSize:11],
                                  NSForegroundColorAttributeName :
                                      [NSColor colorWithCalibratedWhite:0.62 alpha:1.0] };
    NSDictionary* btnAttrs   = @{ NSFontAttributeName : [NSFont systemFontOfSize:11],
                                  NSForegroundColorAttributeName :
                                      [NSColor colorWithCalibratedWhite:0.92 alpha:1.0] };
    NSDictionary* selAttrs   = @{ NSFontAttributeName : [NSFont systemFontOfSize:11],
                                  NSForegroundColorAttributeName :
                                      [NSColor colorWithCalibratedWhite:0.10 alpha:1.0] };

    // 两排的行标签
    const CGFloat labelX = _ps->box.origin.x + 14.0;
    [@"分子" drawAtPoint:NSMakePoint (labelX, _ps->box.origin.y + kTsPopupHeadH + 4.0)
          withAttributes:labelAttrs];
    [@"分母" drawAtPoint:NSMakePoint (labelX, _ps->box.origin.y + kTsPopupHeadH + kTsPopupRowH + 4.0)
          withAttributes:labelAttrs];

    for (const TsBtn& b : _ps->btns)
    {
        const BOOL selected = (b.kind == 0 && b.value == _ps->beats)
                           || (b.kind == 1 && b.value == _ps->denom);
        if (b.kind == 2)
        {
            [[NSColor colorWithCalibratedRed:0.42 green:0.20 blue:0.20 alpha:1.0] setFill];
            [[NSColor colorWithCalibratedRed:0.62 green:0.30 blue:0.30 alpha:1.0] setStroke];
        }
        else if (selected)
        {
            [[NSColor colorWithCalibratedRed:0.98 green:0.68 blue:0.20 alpha:1.0] setFill];
            [[NSColor colorWithCalibratedRed:1.00 green:0.86 blue:0.48 alpha:1.0] setStroke];
        }
        else
        {
            [[NSColor colorWithCalibratedWhite:0.32 alpha:1.0] setFill];
            [[NSColor colorWithCalibratedWhite:0.46 alpha:1.0] setStroke];
        }
        NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:b.rect xRadius:3.0 yRadius:3.0];
        [p fill];
        [p setLineWidth:1.0];
        [p stroke];

        NSString*     t = (b.kind == 2) ? @"取消该小节的变化"
                                        : [NSString stringWithFormat:@"%d", b.value];
        NSDictionary* a = (b.kind == 2) ? btnAttrs : (selected ? selAttrs : btnAttrs);
        NSSize        sz = [t sizeWithAttributes:a];
        [t drawAtPoint:NSMakePoint (NSMidX (b.rect) - sz.width / 2.0,
                                    NSMidY (b.rect) - sz.height / 2.0)
            withAttributes:a];
    }
}

@end

//------------------------------------------------------------------------------
// 主面板
//------------------------------------------------------------------------------
@interface GUIView : NSView <NSTextFieldDelegate, TimeSigPopupDelegate>
{
    PanelState* _st;
}
- (instancetype)initWithBackend:(ap::PlugView::Backend*)b;
- (void)stopTimer;
- (void)stopDrag;
- (void)tick;
- (NSTextField*)label:(NSString*)t size:(CGFloat)s align:(NSTextAlignment)a;
- (NSTextField*)field:(NSString*)t action:(SEL)act;
- (NSButton*)button:(NSString*)t action:(SEL)act;
- (void)offsetChangedExternally:(double)sec;
- (void)syncOffsetUI:(double)sec;
- (void)syncControlsFromBackend;   ///< 视图重建后从后端回读 BPM/拍号/偏移/音量
- (double)bpmFieldValue;          ///< 只读（自测用）：BPM 输入框当前值，0 = 空白
- (NSString*)beatsSelection;      ///< 只读（自测用）：拍号下拉当前项，如 "4/4"
- (void)volChanged:(id)s;
- (void)bpmChanged:(id)s;
- (void)bpmStep:(id)s;
- (void)beatsChanged:(id)s;
- (void)gridToggled:(id)s;
- (void)numToggled:(id)s;
- (void)zoomIn:(id)s;
- (void)zoomOut:(id)s;
- (void)zoomFit:(id)s;
- (void)backToPlayhead:(id)s;
- (void)offsetZero:(id)s;     ///< 「归零」：起始偏移一键清零
- (void)helpToggle:(id)s;
- (void)openFile:(id)s;
- (void)loadPath:(NSString*)path;
- (void)openTimeSigPopupForBar:(int)bar;          ///< 拍号条上的方块被点了
- (void)timeSigAtBar:(int)bar beats:(int*)pb denom:(int*)pd hasChange:(BOOL*)ph;  ///< 该小节实际生效的拍号
- (void)inheritedTimeSigAtBar:(int)bar beats:(int*)pb denom:(int*)pd;  ///< 不含本小节自身变化时的继承值
- (void)refreshTimeSigUI;                         ///< 拍号变化后重绘波形区（拍号条 + 网格线）
- (BOOL)hasAudioURL:(id<NSDraggingInfo>)sender;
@end

@implementation GUIView

// 用翻转坐标（y 向下）：行距从上往下排，和写布局时的直觉一致
- (BOOL)isFlipped { return YES; }

// 让主面板本身能接收焦点：这样 BPM 输入框提交后把第一响应者交还给面板，
// 空格键就还给视图/宿主，而不是被输入框反复吞掉。
- (BOOL)acceptsFirstResponder { return YES; }

- (instancetype)initWithBackend:(ap::PlugView::Backend*)b
{
    if (self = [super initWithFrame:NSMakeRect (0, 0, kPanelWDef, kPanelH)])
    {
        _st = new PanelState ();
        _st->backend = safeBackend (b);

        // ---- 面板宽度可变：靠 autoresizingMask，不做手工重排 ----
        // 三档规则（⚠️ 只涉及横向，纵向全部固定，所以不用任何 Y 方向的 mask）：
        //   · 右边缘要对齐右边界  → NSViewMinXMargin（左边距吃掉宽度增量）
        //   · 横向要跟着变宽      → NSViewWidthSizable（左边距固定，宽度吃掉增量）
        //   · 其余（左对齐、定宽）→ 不设 mask。程序化创建的视图默认 mask = 0，
        //     本来就是「原地不动」，正好就是要的效果。
        // 这样宿主给出更宽的面板时 AppKit 自己就排好了，不需要（也不该有）一个
        // 按尺寸重算坐标的布局函数 —— 那种写法每加一个控件都要记得登记。

        // ---- 标题（窗口内标题保留中文：AppKit 自绘，编码完全可控）----
        NSTextField* title = [self label:@"大伟鼓谱 · MuseScore 音频播放器" size:14 align:NSTextAlignmentLeft];
        title.frame = NSMakeRect (14, 8, 400, 20);
        [title setTextColor:[NSColor colorWithCalibratedRed:0.55 green:0.85 blue:0.55 alpha:1.0]];

        NSButton* open = [self button:@"打开音频…" action:@selector (openFile:)];
        open.frame = NSMakeRect (530, 5, 96, 24);
        open.autoresizingMask = NSViewMinXMargin;   // 贴着右边界（面板变宽时右移）

        // 「帮助」：唤出使用指南覆盖层。
        // 有用户反馈「不知道怎么用」——说明书不能只躺在 README / 安装包里，
        // 得让它在这块面板上直接点得到，否则第一次打开的人只能靠猜。
        NSButton* help = [self button:@"？帮助" action:@selector (helpToggle:)];
        help.frame = NSMakeRect (452, 5, 72, 24);
        help.autoresizingMask = NSViewMinXMargin;   // 同上：跟着「打开音频」一起贴右边
        help.font = [NSFont systemFontOfSize:11];
        help.toolTip = @"使用指南：怎么对齐、怎么播放、快捷键一览";

        // ---- 波形 + 小节网格 ----
        _st->wave = [[WaveformView alloc] initWithBackend:safeBackend (b)];
        _st->wave.frame = NSMakeRect (14, 38, 612, 118);
        _st->wave.autoresizingMask = NSViewWidthSizable;   // 面板变宽 → 波形区跟着变宽
        [_st->wave setPanel:(__bridge void*) self];
        [self addSubview:_st->wave];
        _st->wave.toolTip = @"滚轮左右平移 · ⌥滚轮缩放 · 双击全览 · "
                            @"按住拖动=平移视图 · ⌥拖动或⌘拖动=改起始偏移（配合 ⇧ 微调）· "
                            @"点顶部拍号块=改该小节的拍号";

        // ---- 时间 + 缩放 + 显示开关 ----
        _st->timeLabel = [self label:@"0:00 / 0:00" size:11 align:NSTextAlignmentLeft];
        _st->timeLabel.frame = NSMakeRect (14, 160, 130, 16);

        NSButton* zOut = [self button:@"缩小" action:@selector (zoomOut:)];
        zOut.frame = NSMakeRect (150, 157, 52, 20);
        zOut.font = [NSFont systemFontOfSize:11];
        zOut.toolTip = @"缩小（看到更长时间范围）";
        NSButton* zIn = [self button:@"放大" action:@selector (zoomIn:)];
        zIn.frame = NSMakeRect (204, 157, 52, 20);
        zIn.font = [NSFont systemFontOfSize:11];
        zIn.toolTip = @"放大（看清单个鼓点）";
        NSButton* zFit = [self button:@"全览" action:@selector (zoomFit:)];
        zFit.frame = NSMakeRect (258, 157, 52, 20);
        zFit.font = [NSFont systemFontOfSize:11];
        zFit.toolTip = @"缩放到整段音频";

        // 「回到播放头」（原名「回到谱面」）—— 一键重新同步，并把视野也带回去。
        //
        // 改名理由：旧名字说的是「跳回谱面的位置」，但用户真正想干的是
        // 「我现在看不见播放头了，把我送回去」。新名字直说结果，不用理解概念。
        // 除了 seek 对齐，还会把波形视野挪到播放位置 —— 光对齐不挪视野，
        // 用户会看到「对齐了但画面上什么都没有」，比不对齐还困惑。
        NSButton* backToPlayhead = [self button:@"回到播放头" action:@selector (backToPlayhead:)];
        backToPlayhead.frame = NSMakeRect (314, 157, 86, 20);
        backToPlayhead.font = [NSFont systemFontOfSize:11];
        backToPlayhead.toolTip = @"把音频跳回谱面当前位置，\n"
                                  @"并把波形视野带回播放位置";

        _st->gridCheck = [NSButton checkboxWithTitle:@"网格" target:self action:@selector (gridToggled:)];
        _st->gridCheck.frame = NSMakeRect (406, 160, 50, 16);
        _st->gridCheck.state = NSControlStateValueOn;

        _st->numCheck = [NSButton checkboxWithTitle:@"小节号" target:self action:@selector (numToggled:)];
        _st->numCheck.frame = NSMakeRect (458, 160, 66, 16);
        _st->numCheck.state = NSControlStateValueOn;
        [self addSubview:_st->gridCheck];
        [self addSubview:_st->numCheck];

        // ---- 起始偏移（只读显示，改偏移靠波形上 ⌘/⌥拖动）----
        NSTextField* ol = [self label:@"偏移" size:12 align:NSTextAlignmentLeft];
        ol.frame = NSMakeRect (14, 187, 34, 18);

        _st->offsetLabel = [self label:@"+0.00 秒" size:12 align:NSTextAlignmentLeft];
        _st->offsetLabel.frame = NSMakeRect (52, 187, 110, 18);
        _st->offsetLabel.toolTip = @"当前起始偏移（正值=跳过前奏，负值=先垫静音）\n"
                                    @"改偏移：在波形上按住 ⌘（或 ⌥）拖动网格";

        // 「归零」：把起始偏移一键清零。
        // 之前唯一的改偏移入口是「在波形上 ⌘/⌥ 拖动」—— 手滑拖到很大的值
        // （或者只想回到初始状态）就只能反向拖回去，而拖回来的距离可能是好几个屏。
        // 用户实测点名要这个按钮。
        NSButton* offsetZero = [self button:@"归零" action:@selector (offsetZero:)];
        offsetZero.frame = NSMakeRect (168, 186, 50, 20);
        offsetZero.font = [NSFont systemFontOfSize:11];
        offsetZero.toolTip = @"把起始偏移清零（音频 0 秒 对 谱面 0 秒）";

        // ---- 快捷键提示 ----
        // ⚠️ 宽度受「归零」按钮挤压（原来从 x=170 起、450 宽）。文案里的
        //    「⇧=微调」已挪进 toolTip / 帮助覆盖层，这里只留最要紧的几条。
        NSTextField* hintKeys = [self label:@"拖动=平移视图　⌘/⌥拖动=改偏移　滚轮=平移　⌘/⌥滚轮=缩放　双击=全览"
                                     size:10 align:NSTextAlignmentLeft];
        hintKeys.frame = NSMakeRect (226, 189, 394, 16);
        hintKeys.autoresizingMask = NSViewWidthSizable;   // 起点固定，右端跟着窗口走
        [hintKeys setTextColor:[NSColor colorWithCalibratedWhite:0.55 alpha:1.0]];

        // ---- 速度（BPM）+ 拍号（N/M）----
        NSTextField* bpl = [self label:@"速度" size:12 align:NSTextAlignmentLeft];
        bpl.frame = NSMakeRect (14, 213, 34, 18);

        _st->bpmField = [self field:@"" action:@selector (bpmChanged:)];
        _st->bpmField.frame = NSMakeRect (50, 211, 56, 20);
        _st->bpmField.delegate = self;   // 失焦自动提交，避免焦点卡在输入框里吞掉空格
        _st->bpmField.toolTip = @"每分钟几拍（谱面速度）。填对了网格小节线才能对齐谱面真实小节；\n"
                                 @"留空 = 自动（默认 120）";

        _st->bpmStepper = [[NSStepper alloc] initWithFrame:NSMakeRect (108, 211, 18, 20)];
        _st->bpmStepper.minValue = 20;
        _st->bpmStepper.maxValue = 400;
        _st->bpmStepper.increment = 1;
        _st->bpmStepper.valueWraps = NO;
        _st->bpmStepper.target = self;
        _st->bpmStepper.action = @selector (bpmStep:);
        [self addSubview:_st->bpmStepper];

        NSTextField* bl = [self label:@"拍号" size:12 align:NSTextAlignmentLeft];
        bl.frame = NSMakeRect (138, 213, 34, 18);

        _st->beatsPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect (174, 210, 90, 24)
                                                     pullsDown:NO];
        [_st->beatsPopup addItemsWithTitles:@[ @"2/4", @"3/4", @"4/4", @"5/4",
                                               @"6/8", @"7/8", @"9/8", @"12/8" ]];
        [_st->beatsPopup selectItemAtIndex:2];   // 默认 4/4
        _st->beatsPopup.target = self;
        _st->beatsPopup.action = @selector (beatsChanged:);
        [self addSubview:_st->beatsPopup];

        _st->tempoSrcLabel = [self label:@"速度来源：默认 120" size:10 align:NSTextAlignmentLeft];
        _st->tempoSrcLabel.frame = NSMakeRect (270, 215, 200, 16);
        [_st->tempoSrcLabel setTextColor:[NSColor colorWithCalibratedWhite:0.55 alpha:1.0]];

        // ---- 音量 ----
        NSTextField* vl = [self label:@"音量" size:12 align:NSTextAlignmentLeft];
        vl.frame = NSMakeRect (14, 267, 34, 18);

        _st->volSlider = [NSSlider sliderWithValue:1 minValue:0 maxValue:1
                                       target:self action:@selector (volChanged:)];
        _st->volSlider.frame = NSMakeRect (74, 265, 236, 22);
        _st->volSlider.continuous = YES;
        [self addSubview:_st->volSlider];   // 必须挂进视图层级，否则自动释放后成野指针

        _st->volLabel = [self label:@"100 %" size:12 align:NSTextAlignmentLeft];
        _st->volLabel.frame = NSMakeRect (520, 267, 100, 18);
        _st->volLabel.autoresizingMask = NSViewMinXMargin;   // 音量读数贴着右边界

        // ---- 状态 ----
        _st->statusLabel = [self label:@"未载入音频" size:11 align:NSTextAlignmentLeft];
        _st->statusLabel.frame = NSMakeRect (14, 292, 612, 16);
        _st->statusLabel.autoresizingMask = NSViewWidthSizable;   // 状态文字越长越放得下

        // ---- 播放状态指示灯（纯显示，不可点击）----
        // VST3 标准里插件无法反向控制宿主播放/暂停，点这个按钮也不会真正
        // 播放谱面。所以它只做状态显示：宿主播放时亮「播放」，反之「暂停」。
        _st->playBtn = [self button:@"▶  播放" action:nil];
        _st->playBtn.frame = NSMakeRect (14, 314, 110, 26);
        _st->playBtn.enabled = NO;   // 禁用点击，仅作指示灯
        _st->playBtn.toolTip = @"只显示乐谱当前是否在播放；\n播放 / 暂停请在乐谱里操作";

        // ---- 拖放提示 ----
        // 文案收紧成不带空格的形式：加上 OGG 后原串会顶到 196px 的框外。
        // 这里不动 frame —— 布局尺寸有验证器断言盯着，改文案风险最低。
        NSTextField* hint = [self label:@"支持 MP3/WAV/M4A/AAC/FLAC/OGG" size:10
                                 align:NSTextAlignmentRight];
        hint.frame = NSMakeRect (430, 320, 196, 14);
        hint.autoresizingMask = NSViewMinXMargin;   // 右对齐（文案本来就右对齐）
        [hint setTextColor:[NSColor colorWithCalibratedWhite:0.5 alpha:1.0]];

        // ---- 底部页脚：B 站署名 + 欢迎语（作者引流，可点击跳主页）----
        // 放在面板最底部而非顶栏：不挤占主操作区，且用户每次打开界面都会看到。
        NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect (14, 352, 612, 1)];
        sep.boxType = NSBoxSeparator;
        sep.autoresizingMask = NSViewWidthSizable;   // 分隔线横贯整个面板宽度
        [self addSubview:sep];

        // ⭐ 用 BrandLinkField（NSTextField 子类）而不是 label: —— 点一下会用系统
        //    默认浏览器打开作者 B 站主页，鼠标移上去是手型光标。
        //    尺寸/颜色与原来的 label 完全一致，所以布局与验证器断言都不用改。
        BrandLinkField* footer =
            [[BrandLinkField alloc] initWithFrame:NSMakeRect (14, 360, 612, 16)];
        footer.autoresizingMask = NSViewWidthSizable;   // 与上面的分隔线同宽
        footer.stringValue = @"♪  B 站「大伟鼓谱」· 欢迎关注，鼓谱 / 教学 / 伴奏持续更新";
        footer.font        = [NSFont systemFontOfSize:11];
        footer.alignment   = NSTextAlignmentCenter;
        // ⚠️ initWithFrame: 造出来的是「可编辑输入框」原形，这几项必须显式关掉：
        //    否则页脚会变成一个带白底、能打字、会抢焦点的输入框。
        footer.bezeled         = NO;
        footer.drawsBackground = NO;
        footer.editable        = NO;
        footer.selectable      = NO;
        [footer setTextColor:[NSColor colorWithCalibratedRed:0.98 green:0.45 blue:0.60 alpha:1.0]];  // B 站粉
        footer.toolTip = @"点一下打开作者 B 站主页\n（本插件作者：大伟鼓谱 —— 鼓谱 / 教学 / 伴奏持续更新）";
        [self addSubview:footer];

        // ---- 接受拖放 ----
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];

        // ---- 使用指南覆盖层（必须最后 addSubview：子视图顺序 = 从前到后，最后加的在最上层）----
        _st->helpOverlay = [[HelpOverlayView alloc] initWithFrame:self.bounds];
        _st->helpOverlay.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _st->helpOverlay.hidden = YES;
        [self addSubview:_st->helpOverlay];

        // ---- 拍号设置覆盖层（加在帮助层之后 → 叠在它上面）----
        _st->tsPopup = [[TimeSigPopupView alloc] initWithDelegate:self];
        _st->tsPopup.frame = self.bounds;
        _st->tsPopup.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [self addSubview:_st->tsPopup];

        // ---- 从后端回读已有设置（⭐ 关掉编辑器再打开时保持用户填的值）----
        [self syncControlsFromBackend];

        // ---- 定时刷新播放头 ----
        _st->timer = [NSTimer scheduledTimerWithTimeInterval:0.05
                                                  target:self
                                                selector:@selector (tick)
                                                userInfo:nil
                                                 repeats:YES];
    }
    return self;
}

// 关闭编辑器时必须先停掉定时器：它 20Hz 访问后端，而宿主可能随后就把
// 插件实例销毁了 —— 那样定时器会打在野指针上（关窗口后闪退）。
- (void)stopTimer
{
    [_st->timer invalidate];
    _st->timer = nil;
}

// 关窗口时把拖动状态清掉：⌥拖网格会压入一个光标，不清就会残留在光标栈里
- (void)stopDrag
{
    [_st->wave stopDrag];
}

- (void)dealloc
{
    [_st->timer invalidate];
    delete _st;
}

//---- 控件工厂 --------------------------------------------------------------
- (NSTextField*)label:(NSString*)t size:(CGFloat)s align:(NSTextAlignment)a
{
    NSTextField* l = [NSTextField labelWithString:t];
    l.font = [NSFont systemFontOfSize:s];
    l.alignment = a;
    [l setTextColor:[NSColor colorWithCalibratedWhite:0.85 alpha:1.0]];
    [self addSubview:l];
    return l;
}

- (NSButton*)button:(NSString*)t action:(SEL)act
{
    NSButton* b = [NSButton buttonWithTitle:t target:self action:act];
    b.bezelStyle = NSBezelStyleTexturedRounded;   // 紧凑小按钮，中文不会被截断成「…」
    [self addSubview:b];
    return b;
}

// 可输入的数值框（回车触发 action）
- (NSTextField*)field:(NSString*)t action:(SEL)act
{
    NSTextField* f = [NSTextField textFieldWithString:t];
    f.font = [NSFont systemFontOfSize:12];
    f.alignment = NSTextAlignmentRight;
    f.target = self;
    f.action = act;
    f.continuous = NO;
    [self addSubview:f];
    return f;
}

//---- 起始偏移（带符号）------------------------------------------------------
// 偏移只读展示：改偏移的唯一入口是波形上 ⌘/⌥拖动网格。
// syncOffsetUI 只刷新只读的数值显示，不再有滑块/输入框可同步。
- (void)syncOffsetUI:(double)sec
{
    _st->offsetLabel.stringValue = [NSString stringWithFormat:@"%+.2f 秒", sec];
}

//------------------------------------------------------------------------------
// ⭐ 编辑器关闭再打开时，把后端里已有的设置回读到控件
//
// 为什么必须有：音频引擎（Processor）的寿命比编辑器视图长。在 MuseScore 里关掉
// 插件界面只销毁 PlugView —— 日志里能看到「编辑器 removed」之后紧跟一条新的
// 「PlugView 构造」，而后端指针地址不变。但视图里的控件是全新造的，若不回读：
//   · BPM 显示成默认 120（后端其实还存着用户填的值），而输入框一失焦就会
//     自动提交 → 把 120 写回后端 = 用户手填的 BPM 真的被抹掉了；
//   · 拍号下拉回到 4/4，还被当成「用户没改过」→ 之后会被宿主给的拍号覆盖。
// 所以这里把所有「存在后端里」的设置读回来，保证关掉再打开界面一模一样。
// ⚠️ 新增任何「存在后端」的控件，都必须在这里补一行回读。
//------------------------------------------------------------------------------
- (void)syncControlsFromBackend
{
    ap::PlugView::Backend* b = _st->backend;

    // 速度：只有手动值（>=1）才填进输入框；0 = 自动 → 留空（提示里写明「留空 = 自动」）。
    // 不能指望 tick 里的「跟随乐谱速度」分支来填：那要求宿主提供速度，
    // 而 MuseScore 不提供（实测 tempoN=0x0000），那条分支基本不会走。
    const float bpm = b->gridBPM ();
    if (bpm >= 1.0f)
    {
        _st->bpmField.stringValue = [NSString stringWithFormat:@"%.0f", bpm];
        _st->bpmStepper.doubleValue = bpm;
    }
    else
    {
        _st->bpmField.stringValue = @"";
        _st->bpmStepper.doubleValue = 120.0;
    }
    [self updateTempoSourceLabel];

    // 拍号：后端不是 4/4 就说明用户改过 → 回读，并标记「用户改过」，
    // 否则宿主一旦给出拍号，用户的选择会被自动覆盖。
    const int beats = b->gridBeatsPerBar ();
    const int denom = b->gridBeatDenominator ();
    NSString* want = [NSString stringWithFormat:@"%d/%d", beats, denom];
    for (NSInteger i = 0; i < _st->beatsPopup.numberOfItems; ++i)
    {
        if ([[_st->beatsPopup itemTitleAtIndex:i] isEqualToString:want])
        {
            [_st->beatsPopup selectItemAtIndex:i];
            break;
        }
    }
    if (beats != 4 || denom != 4)
        _st->beatsUserSet = true;

    // 起始偏移（只读显示）
    [self syncOffsetUI:b->offsetSec ()];

    // 音量
    const float vol = b->volume ();
    _st->volSlider.doubleValue = vol;
    _st->volLabel.stringValue = [NSString stringWithFormat:@"%d %%", (int) (vol * 100)];

    // ⭐ 视野：视图是新建的，不回读就回到「全曲全览」——用户实测原话：「把插件
    //    界面关掉再打开，波形就变成全蓝了（整曲全览），而不是之前放大的那一段」。
    //    后端存着就用它（连放大倍数一起恢复）；没存过（或刚换了音频）就按
    //    「新载入」的默认视野来，跟 loadPath: 里那条规则完全一致。
    if (b->hasAudio ())
    {
        double vs = 0.0, vlen = 0.0;
        b->getViewState (vs, vlen);
        if (vlen > 0.0)
            [_st->wave zoomToSpan:vlen fromStart:vs];
        else
            [_st->wave zoomToSpan:kDefaultViewSpanSec fromStart:b->offsetSec ()];
        // 记成「已回存」，免得 tick 第一帧把同一份值再写一遍（无害但没必要）。
        _st->pushedViewStart = [_st->wave viewStartSec];
        _st->pushedViewLen   = [_st->wave viewLenSec];
    }
}

//---- 只读访问器（自测用，见 gui_repro.mm）------------------------------------
- (double)bpmFieldValue { return _st->bpmField.stringValue.doubleValue; }
- (NSString*)beatsSelection { return _st->beatsPopup.titleOfSelectedItem; }

// 波形上 ⌘/⌥拖动网格 → 回写偏移显示，并实时提示偏移结果
- (void)offsetChangedExternally:(double)sec
{
    [self syncOffsetUI:sec];

    // 拖动网格（改偏移）时在状态栏实时显示结果，让用户一眼看到
    // 「谱面当前这一小节」被放到了音频的哪个位置，以及偏移数字是多少。
    // ⚠️ 这里【不要】说「对齐」：红绿两线本来就重合，跟偏移对不对无关（见
    //    drawRect 里那段说明）。状态栏只报「偏移数值 + 当前映射」。
    const ap::PlugView::Backend::HostTimeline tl = _st->backend->hostTimeline ();
    if (tl.playheadValid)
    {
        const double audioT = tl.playheadSec + sec;
        _st->statusLabel.stringValue =
            [NSString stringWithFormat:@"偏移 %+.2f 秒（谱面位置 %.2f 秒 = 音频 %.2f 秒）",
                                       sec, tl.playheadSec, audioT];
        [_st->statusLabel setTextColor:[NSColor colorWithCalibratedRed:0.55
                                                             green:0.85
                                                              blue:0.55
                                                             alpha:1.0]];
    }
    else
    {
        _st->statusLabel.stringValue =
            [NSString stringWithFormat:@"偏移 %+.2f 秒", sec];
        [_st->statusLabel setTextColor:[NSColor colorWithCalibratedWhite:0.6 alpha:1.0]];
    }
}

//---- 拍号变化（波形区拍号条 → 弹出设置）---------------------------------------
// 该小节【实际生效】的拍号 = 「起小节 <= bar」的最后一条变化；没有变化就用全局默认拍号。
- (void)timeSigAtBar:(int)bar beats:(int*)pb denom:(int*)pd hasChange:(BOOL*)ph
{
    int b = _st->backend->gridBeatsPerBar ();
    int d = _st->backend->gridBeatDenominator ();
    if (b < 1) b = 4;
    if (d < 1) d = 4;
    bool has = false;
    std::vector<ap::TimeSigChange> changes;
    _st->backend->getTimeSigChanges (changes);
    for (const ap::TimeSigChange& c : changes)
    {
        if (c.bar == bar) has = true;
        if (c.bar <= bar) { b = c.beats; d = c.denom; }
    }
    if (pb) *pb = b;
    if (pd) *pd = d;
    if (ph) *ph = has;
}

// 不含本小节自身变化的「继承值」。用途：用户把某小节设成跟继承值一样时，
// 等于「这一小节没有变化」→ 应当把变化点取消，而不是留一个毫无几何作用的橙块。
- (void)inheritedTimeSigAtBar:(int)bar beats:(int*)pb denom:(int*)pd
{
    int b = _st->backend->gridBeatsPerBar ();
    int d = _st->backend->gridBeatDenominator ();
    if (b < 1) b = 4;
    if (d < 1) d = 4;
    std::vector<ap::TimeSigChange> changes;
    _st->backend->getTimeSigChanges (changes);
    for (const ap::TimeSigChange& c : changes)
        if (c.bar < bar) { b = c.beats; d = c.denom; }
    if (pb) *pb = b;
    if (pd) *pd = d;
}

- (void)refreshTimeSigUI
{
    // 拍号条与网格线都在波形视图里，重绘它即可
    [_st->wave setNeedsDisplay:YES];
}

// 自测访问器：拍号设置弹层是否可见（gui_repro 用）
- (BOOL)tsPopupVisible
{
    return _st->tsPopup && !_st->tsPopup.hidden;
}

- (void)openTimeSigPopupForBar:(int)bar
{
    if (!_st->backend->hasAudio ()) return;

    int   beats = 4, denom = 4;
    BOOL  has   = NO;
    [self timeSigAtBar:bar beats:&beats denom:&denom hasChange:&has];

    _st->helpOverlay.hidden = YES;   // 别和帮助层叠在一起
    [_st->tsPopup showForBar:bar beats:beats denom:denom hasChange:has];
    [self addSubview:_st->tsPopup positioned:NSWindowAbove relativeTo:nil];
}

- (void)timeSigPopupApplyBar:(int)bar beats:(int)beats denom:(int)denom
{
    int inhB = 4, inhD = 4;
    [self inheritedTimeSigAtBar:bar beats:&inhB denom:&inhD];

    const BOOL same = (beats == inhB && denom == inhD);
    if (same)
        _st->backend->clearTimeSigChange (bar);          // 选的就是继承值 → 等于取消
    else
        _st->backend->setTimeSigChange (bar, beats, denom);

    // 面板留在屏幕上，方便连着调分子 / 分母
    [_st->tsPopup showForBar:bar beats:beats denom:denom hasChange:!same];
    [self refreshTimeSigUI];
}

- (void)timeSigPopupRemoveBar:(int)bar
{
    _st->backend->clearTimeSigChange (bar);
    int b = 4, d = 4;
    [self timeSigAtBar:bar beats:&b denom:&d hasChange:NULL];
    [_st->tsPopup showForBar:bar beats:b denom:d hasChange:NO];
    [self refreshTimeSigUI];
}

- (void)timeSigPopupClosed
{
    // 关闭本身不需要做什么：变化一按下按钮就已经写进后端并重绘了
}

//---- 网格参数 --------------------------------------------------------------
- (void)beatsChanged:(id)s
{
    _st->beatsUserSet = true;
    // 拍号标题形如 "12/8"：分子 = 每小节拍数，分母 = 以几分音符为一拍。
    NSString* title = _st->beatsPopup.titleOfSelectedItem;
    NSArray* parts = [title componentsSeparatedByString:@"/"];
    if (parts.count >= 2)
    {
        const int n = [parts[0] intValue];
        const int d = [parts[1] intValue];
        if (n >= 1 && n <= 32)
            _st->backend->setGridBeatsPerBar (n);
        if (d == 2 || d == 4 || d == 8 || d == 16)
            _st->backend->setGridBeatDenominator (d);
    }
    [_st->wave setNeedsDisplay:YES];
}

- (void)bpmChanged:(id)s
{
    const float v = (float) _st->bpmField.stringValue.doubleValue;   // 空串 → 0
    if (v < 1.0f)
    {
        _st->backend->setGridBPM (0.f);       // 自动：优先跟随宿主速度
    }
    else
    {
        _st->backend->setGridBPM (v);
        _st->bpmField.stringValue = [NSString stringWithFormat:@"%.0f", v];
        _st->bpmStepper.doubleValue = v;
    }
    [self updateTempoSourceLabel];
    [_st->wave setNeedsDisplay:YES];

    // 回车 = 提交完成，立即把第一响应者交还给面板（而非输入框）。
    // 否则焦点会卡在输入框里，之后按空格反复触发编辑、把值清回默认
    // （「按空格 BPM 乱跳」的根因）。点按钮/标签/空白处不会抢焦点，
    // 所以必须在这里主动交还，不能只依赖失焦委托。
    [[self window] makeFirstResponder:self];
}

- (void)bpmStep:(id)s
{
    float v = (float) _st->bpmStepper.doubleValue;
    if (v < 20.0f) v = 20.0f;
    if (v > 400.0f) v = 400.0f;
    _st->backend->setGridBPM (v);
    _st->bpmField.stringValue = [NSString stringWithFormat:@"%.0f", v];
    [self updateTempoSourceLabel];
    [_st->wave setNeedsDisplay:YES];
}

// BPM 输入框失焦时自动提交：用户输完点别处 / 按 Tab / 按空格，都能让值生效，
// 而不是非按回车不可。同时把第一响应者交出去，否则焦点卡在输入框里，
// 之后按空格会反复触发编辑、把值清回默认（这就是「按空格 BPM 乱跳」的根因）。
- (void)controlTextDidEndEditing:(NSNotification*)note
{
    if (note.object == _st->bpmField)
    {
        [self bpmChanged:_st->bpmField];
        [[self window] makeFirstResponder:self];   // 释放输入框焦点，让空格还给面板
    }
}

- (void)updateTempoSourceLabel
{
    const GridInfo g = gridInfoFor (_st->backend);
    const float manual = _st->backend->gridBPM ();
    NSString* s;
    if (manual > 1.0f)
        s = [NSString stringWithFormat:@"速度来源：手动 %.0f", manual];
    else if (g.fromHost)
        s = [NSString stringWithFormat:@"速度来源：乐谱 %.1f", g.bpm];
    else
        s = @"速度来源：默认 120";
    _st->tempoSrcLabel.stringValue = s;
}

- (void)gridToggled:(id)s
{
    [_st->wave setShowGrid:(_st->gridCheck.state == NSControlStateValueOn)];
}

- (void)numToggled:(id)s
{
    [_st->wave setShowNumbers:(_st->numCheck.state == NSControlStateValueOn)];
}

//---- 缩放 ------------------------------------------------------------------
- (void)zoomIn:(id)s
{
    [_st->wave zoomBy:1.6 aroundX:NSMidX (_st->wave.bounds)];
}

- (void)zoomOut:(id)s
{
    [_st->wave zoomBy:1.0 / 1.6 aroundX:NSMidX (_st->wave.bounds)];
}

- (void)zoomFit:(id)s
{
    [_st->wave fitAll];
}

// 「回到播放头」：一键重新同步 + 把视野带回播放位置。
//
// 它是「分家」的最终保险：正常操作下音频位置由宿主驱动（谱面位置 + 偏移），
// 不该分家；但万一有未知 bug 让跟随失效，用户很难自己看出来（两条线贴不贴
// 全靠肉眼）。点一下这个按钮 = 强制把音频对齐到谱面，并把画面也送回去。
//
// 视野定位直接用算出来的目标位置，不需要等 seek 生效：seekTo 是给音频线程
// 投递目标，位置会在下一块输出时到位，而下一帧画面就是按目标位置画的。
- (void)backToPlayhead:(id)s
{
    if (!_st->backend->hasAudio ())
    {
        _st->statusLabel.stringValue = @"还没载入音频 —— 点右上「打开音频」或把文件拖进来";
        [_st->statusLabel setTextColor:[NSColor colorWithCalibratedWhite:0.6 alpha:1.0]];
        return;
    }

    const ap::PlugView::Backend::HostTimeline tl = _st->backend->hostTimeline ();
    if (!tl.playheadValid)
    {
        _st->statusLabel.stringValue = @"宿主没有提供谱面位置，无法回到播放头（可直接用滚轮平移）";
        [_st->statusLabel setTextColor:[NSColor colorWithCalibratedWhite:0.6 alpha:1.0]];
        return;
    }

    const double audioPos = tl.playheadSec + _st->backend->offsetSec ();
    _st->backend->seekTo (audioPos);        // ① 重新对齐
    [_st->wave centerOn:audioPos];          // ② 视野跟着回到播放位置

    // 记下「刚刚手动对齐过」：若 3 秒内又发现错位，说明这是跟随失效而非
    // 一次性偏差 → 徽标文案升级为「建议重启插件」，同时落一条日志。
    [_st->wave noteBackToPlayhead];

    _st->statusLabel.stringValue =
        [NSString stringWithFormat:@"已回到播放头 %.2f 秒（音频已重新对齐谱面）", audioPos];
    [_st->statusLabel setTextColor:[NSColor colorWithCalibratedRed:0.55
                                                         green:0.85
                                                          blue:0.55
                                                         alpha:1.0]];
    [_st->wave setNeedsDisplay:YES];
}

// 「归零」：把起始偏移一键清零。
// 为什么要它：改偏移的唯一入口是「在波形上 ⌘/⌥ 拖动」，一旦手滑拖到很大的值
//（或只想回到初始状态），就只能反向拖回去 —— 可能要拖好几个屏。用户实测点名要。
- (void)offsetZero:(id)s
{
    (void) s;
    if (!_st->backend)
        return;

    _st->backend->setOffsetSec (0.0f);
    [self offsetChangedExternally:0.0];     // 刷新偏移数值 + 状态栏

    // 归零会让音频跳回「谱面位置 + 0」，播放头可能一下跑到画面外 —— 那种情况就
    // 把它带回画面（本来就在画面里则什么都不做，别抢用户正在看的视野）。
    const ap::PlugView::Backend::HostTimeline tl = _st->backend->hostTimeline ();
    if (tl.playheadValid)
        [_st->wave centerOnIfOffscreen:tl.playheadSec];

    [_st->wave setNeedsDisplay:YES];
}

// 「帮助」：显示/隐藏使用指南覆盖层。
// 覆盖层盖住整块面板，任意点击都会把它关掉（见 HelpOverlayView）。
- (void)helpToggle:(id)s
{
    _st->helpOverlay.hidden = !_st->helpOverlay.hidden;

    // 重新置顶：子视图顺序可能被后续操作打乱，这里显式抬到最上层，
    // 保证它一定盖住所有控件。
    if (!_st->helpOverlay.hidden)
    {
        [self addSubview:_st->helpOverlay positioned:NSWindowAbove relativeTo:nil];
        [_st->helpOverlay setNeedsDisplay:YES];
    }
}

- (void)volChanged:(id)s
{
    const float v = (float)_st->volSlider.doubleValue;
    _st->backend->setVolume (v);
    _st->volLabel.stringValue = [NSString stringWithFormat:@"%d %%", (int) (v * 100)];
}

- (void)openFile:(id)s
{
    NSOpenPanel* p = [NSOpenPanel openPanel];
    p.allowsMultipleSelection = NO;
    p.canChooseDirectories = NO;
    p.allowedFileTypes = @[ @"mp3", @"wav", @"m4a", @"aac", @"alac", @"aiff", @"aif",
                            @"caf", @"flac", @"mp4", @"ogg", @"oga" ];
    p.message = @"选择要播放的音频文件";
    if ([p runModal] != NSModalResponseOK) return;

    NSString* path = p.URL.path;
    if (!path) return;
    [self loadPath:path];
}

- (void)loadPath:(NSString*)path
{
    const bool ok = _st->backend->loadFile (path.UTF8String);
    if (ok)
    {
        _st->statusLabel.stringValue =
            [NSString stringWithFormat:@"已载入：%@", path.lastPathComponent];
        [_st->statusLabel setTextColor:[NSColor colorWithCalibratedWhite:0.6 alpha:1.0]];

        // 换了文件：偏移若超出新音频长度就归零，再同步控件（滑块范围也跟着新时长走）
        const double dur = _st->backend->durationSec ();
        double off = _st->backend->offsetSec ();
        if (dur > 0.0 && (off >= dur || off <= -dur))
        {
            off = 0.0;
            _st->backend->setOffsetSec (0.0f);
        }

        // 新文件默认放大到「前几小节」，从偏移处开始看，而不是整曲全览。
        // 全览时波峰糊成一片，用户每次都得手动放大；直接给 kDefaultViewSpanSec
        // 秒视野（约 4 小节 @120BPM 4/4），一眼看清对齐状况。
        [_st->wave zoomToSpan:kDefaultViewSpanSec fromStart:off];
        [self syncOffsetUI:off];
        [self updateTempoSourceLabel];
    }
    else
    {
        NSString* e = [NSString stringWithUTF8String:_st->backend->lastError ().c_str ()];
        _st->statusLabel.stringValue = [NSString stringWithFormat:@"载入失败：%@",
                                    e ?: @"未知错误"];
        [_st->statusLabel setTextColor:[NSColor colorWithCalibratedRed:1.0
                                                             green:0.45
                                                              blue:0.40
                                                             alpha:1.0]];
    }
    // 换文件后音频数据变了，旧峰值缓存必须失效再重取（否则视野相同时误命中旧缓存）。
    [_st->wave invalidatePeaks];
    [_st->wave refreshPeaks];
}

//---- 拖放 ----------------------------------------------------------------
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    if ([self hasAudioURL:sender]) return NSDragOperationCopy;
    return NSDragOperationNone;
}

- (BOOL)hasAudioURL:(id<NSDraggingInfo>)sender
{
    NSArray* urls = [sender.draggingPasteboard readObjectsForClasses:
                       @[ [NSURL class] ] options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    if (urls.count == 0) return NO;
    NSString* ext = ((NSURL*) urls[0]).pathExtension.lowercaseString;

    // ★ 必须是 alloc/init（引用计数 +1 且永不释放）。
    //   之前写的是 [NSSet setWithArray:]，它返回【自动释放】对象；
    //   存进 static 变量后没有任何人持有它，autorelease 池一排空
    //   这个指针就悬垂了 —— 拖拽时第二次问到这里就是「给已释放对象
    //   发消息」→ 宿主闪退（崩溃日志正是在 containsObject: 这一句）。
    static NSSet* ok = nil;
    if (!ok)
        ok = [[NSSet alloc] initWithArray:@[ @"mp3", @"wav", @"m4a", @"aac", @"alac",
                                              @"aiff", @"aif", @"caf", @"flac", @"mp4",
                                              @"ogg", @"oga" ]];
    return [ok containsObject:ext];
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray* urls = [sender.draggingPasteboard readObjectsForClasses:
                       @[ [NSURL class] ] options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    if (urls.count == 0) return NO;
    [self loadPath:((NSURL*) urls[0]).path];
    return YES;
}

//---- 定时刷新 -------------------------------------------------------------
static NSString* fmtTime (double sec)
{
    if (sec < 0 || sec != sec) sec = 0;
    const int s = (int) sec;
    return [NSString stringWithFormat:@"%d:%02d", s / 60, s % 60];
}

- (void)tick
{
    const ap::PlugView::Backend::HostTimeline tl = _st->backend->hostTimeline ();

    // 「点完『回到播放头』仍然错位」= 跟随本身失效 → 徽标文案升级并记一条日志。
    // 判定逻辑内聚在波形视图里（它同时负责画那个徽标），这里只做调用。
    [_st->wave checkFollow];

    // 宿主到底给没给时间轴信息，只记一次日志（在界面线程写文件，
    // 音频线程里绝不能做 I/O）。之后看 ~/Library/Logs/DaweiDrumScore.log 就知道
    // MuseScore 是否提供了速度/拍号/谱面位置。
    if (!_st->timelineLogged && tl.rawState != 0)
    {
        _st->timelineLogged = true;
        ap::crashLog ("宿主时间轴: state=0x%04X (tempo%c 拍号%c 谱面位置%c 播放%c)",
                      tl.rawState,
                      tl.tempoValid ? 'Y' : 'N',
                      tl.timeSigValid ? 'Y' : 'N',
                      tl.playheadValid ? 'Y' : 'N',
                      tl.playing ? 'Y' : 'N');
        if (tl.playheadValid)
            ap::crashLog ("  谱面播放头 = %.3f 秒 (projectTimeSamples/sampleRate)",
                          tl.playheadSec);
    }

    // 速度自动跟随乐谱（用户没手动填过 BPM 才跟随）
    if (tl.tempoValid && _st->backend->gridBPM () < 1.0f)
    {
        NSString* want = [NSString stringWithFormat:@"%.0f", tl.bpm];
        if (![_st->bpmField.stringValue isEqualToString:want])
        {
            _st->bpmField.stringValue = want;
            _st->bpmStepper.doubleValue = tl.bpm;
        }
    }

    // 拍号自动跟随乐谱（用户没手动改过才跟随）
    if (tl.timeSigValid && !_st->beatsUserSet && tl.beatsPerBar >= 1)
    {
        NSString* want = [NSString stringWithFormat:@"%d/%d",
                          tl.beatsPerBar,
                          tl.beatDenominator > 0 ? tl.beatDenominator : 4];
        if (![_st->beatsPopup.titleOfSelectedItem isEqualToString:want])
        {
            for (NSInteger i = 0; i < _st->beatsPopup.numberOfItems; ++i)
            {
                if ([[_st->beatsPopup itemTitleAtIndex:i] isEqualToString:want])
                {
                    [_st->beatsPopup selectItemAtIndex:i];
                    _st->backend->setGridBeatsPerBar (tl.beatsPerBar);
                    _st->backend->setGridBeatDenominator (
                        tl.beatDenominator > 0 ? tl.beatDenominator : 4);
                    break;
                }
            }
        }
    }

    // ---- 跳转跟随已下沉到音频线程（captureHostTimeline）----
    // 之前在这里（20Hz 定时器）检测宿主播放头跳变并 seek，导致 UI 关闭后
    // 定时器停止、跳转跟随失效。现在检测逻辑移到了音频线程，UI 是否显示
    // 都不影响跳转跟随。这里不再做任何跳变检测。

    // ---- 视野跟随：宿主播放时，波形视野自动滚到谱面播放头 ----
    // ⭐ 先区分「播放头自己在走」和「宿主把它挪走了」：前者画面只许顺滑滚，
    //    后者画面该跟过去。20Hz 采样下正常前进每帧只有几十毫秒；超过
    //    kHostJumpSec 就是宿主跳变（点小节 / 循环回卷 / 拖播放头）。
    bool hostJumped = false;
    if (tl.playheadValid)
    {
        if (_st->hasPrevPlayhead &&
            std::fabs (tl.playheadSec - _st->prevPlayheadSec) > kHostJumpSec)
            hostJumped = true;
        _st->prevPlayheadSec = tl.playheadSec;
        _st->hasPrevPlayhead = true;
    }

    // ---- 实时健康探针：音频线程有没有因为抢不到锁而丢块 ----
    // 丢一块就少推进一个缓冲区的时间，误差【永久累积】→ 播放位置越来越落后
    // 宿主，也就是用户看到的「一边拖波形一边分家」。音频线程绝不能做 I/O，
    // 所以在那边只做原子自增，写日志的事放到这里（界面线程）。
    // 正常情况下这个计数恒为 0；一旦增长，日志会直接点出方向。
    {
        const uint64_t drops = _st->backend->lockDropCount ();
        if (drops > _st->lockDropSeen)
        {
            _st->lockDropSeen = drops;
            const double nowT = [NSProcessInfo processInfo].systemUptime;
            if ((nowT - _st->lockDropLoggedAt) > kLockDropLogGapSec)
            {
                _st->lockDropLoggedAt = nowT;
                ap::crashLog ("⚠ 音频线程丢块：累计 %llu 块 —— 播放位置会落后宿主，"
                              "排查：谁在锁内扫大段采样（画波形应走 audioSnapshot）",
                              (unsigned long long) drops);
            }
        }
    }

    // 用户停止手动操作超时后自动恢复跟随（播放/暂停都生效）。
    [_st->wave recoverAutoFollow];

    // ⭐ 宿主自己跳的（点小节 / 循环回卷 / 拖播放头）就算停在【暂停】状态也要把
    //    画面带过去 —— 扒谱就是在暂停下一个小节一个小节地点的。以前这里写死了
    //    tl.playing，于是暂停时换位置画面纹丝不动，「空白乐谱直接定位到音乐」
    //    这件事根本做不到。自然播放推进（allowJump=NO）时仍然只许顺滑滚。
    if (tl.playheadValid && (tl.playing || hostJumped))
    {
        const double audioT = tl.playheadSec + _st->backend->offsetSec ();
        [_st->wave followPlayheadTo:audioT allowJump:(hostJumped ? YES : NO)];
    }

    if (!_st->backend->hasAudio ())
    {
        _st->timeLabel.stringValue = @"0:00 / 0:00";
        return;
    }

    // ---- 视野状态回存：关掉编辑器再打开要回到同一段视野 ----
    // ⭐ 放在 tick 里是刻意的：视野的改动入口很多（滚轮 / 拖动 / 缩放 / 全览 /
    //    回到播放头 / 自动跟随），逐个入口去写必然漏一个（见 MEMORY 铁律 9）。
    //    这里统一「发现变了就写回」，一个入口都不会漏。
    // ⚠️ viewLen == 0 表示全览，此时有效跨度是整曲长度 —— 必须换算成真实秒数
    //    再存，否则重开时后端拿到 span=0，会被当成「没设置过」。
    {
        double vs = [_st->wave viewStartSec];
        double vl = [_st->wave viewLenSec];
        const double dur = _st->backend->durationSec ();
        if (!(vl > 0.0) && dur > 0.0)
            vl = dur;
        if (vl > 0.0
            && (std::fabs (vs - _st->pushedViewStart) > 1e-6
             || std::fabs (vl - _st->pushedViewLen) > 1e-6))
        {
            _st->pushedViewStart = vs;
            _st->pushedViewLen   = vl;
            _st->backend->setViewState (vs, vl);
        }
    }

    _st->timeLabel.stringValue = [NSString stringWithFormat:@"%@ / %@",
                              fmtTime (_st->backend->positionSec ()),
                              fmtTime (_st->backend->durationSec ())];

    // 播放状态指示灯：以「宿主是否在播放」为准（tl.playing）。
    // 插件无法反向控制宿主，所以按钮只做只读状态显示，不做点击切换。
    NSString* wantTitle = tl.playing ? @"❚❚  暂停" : @"▶  播放";
    if (![_st->playBtn.title isEqualToString:wantTitle])
        _st->playBtn.title = wantTitle;

    [_st->wave setNeedsDisplay:YES];
}

@end

namespace ap {

//------------------------------------------------------------------------------
// 打开作者 B 站主页（底部宣传语被点击时调用）
//
// 【为什么必须异步】宿主是实时音频程序，GUI 线程被拖住会直接影响音频回调调度。
// LaunchServices 派发本身是异步的，这里再给个 completionHandler 只是为了在
// 打开失败时留一条日志 —— 不阻塞、不等待浏览器。
//
// ⚠️ 打不开只记日志、不弹窗：插件里弹 NSAlert 会在宿主窗口上叠一个模态框，
//    风险远大于收益（用户点个广告位不该有机会卡住宿主）。
//------------------------------------------------------------------------------
void openBrandHome ()
{
    NSString* str = [NSString stringWithUTF8String:kBrandHomeUrl];
    NSURL* url = str ? [NSURL URLWithString:str] : nil;
    if (!url)
    {
        crashLog ("作者主页地址无效，无法打开：%s", kBrandHomeUrl);
        return;
    }

    crashLog ("打开作者主页：%s", kBrandHomeUrl);
    [[NSWorkspace sharedWorkspace]
        openURL:url
        configuration:[NSWorkspaceOpenConfiguration configuration]
        completionHandler:^(NSRunningApplication* app, NSError* err) {
            (void) app;
            if (err)
                ap::crashLog ("作者主页打开失败：%s", err.localizedDescription.UTF8String);
        }];
}

//==============================================================================
// PlugView —— IPlugView 的实现
//==============================================================================
PlugView::PlugView (Backend* backend, BackendResolver resolver)
    : m_backend (backend), m_resolver (resolver)
{
    ap::crashLog ("PlugView 构造（ref=%u）", m_refCount);
}

tresult PLUGIN_API PlugView::queryInterface (const TUID _iid, void** obj)
{
    if (!obj) return kInvalidArgument;
    *obj = nullptr;
    if (FUnknownPrivate::iidEqual (_iid, FUnknown::iid) ||
        FUnknownPrivate::iidEqual (_iid, IPlugView::iid))
    {
        *obj = static_cast<IPlugView*> (this);
        addRef ();
        return kResultOk;
    }
    return kNoInterface;
}

tresult PLUGIN_API PlugView::isPlatformTypeSupported (FIDString type)
{
    if (!type) return kInvalidArgument;

    // 宿主问的是具体平台类型常量，规范里根本没有 "PLATFORM_UI" 这个值。
    // 之前匹配错字符串，MuseScore 一问「能否嵌进 NSView」就得到否定答复，
    // 直接判定插件无原生编辑器 —— 这就是「选中后没有任何界面弹出」的根因。
#if defined (__APPLE__)
    if (std::strcmp (type, kPlatformTypeNSView) == 0) return kResultTrue;
    if (std::strcmp (type, kPlatformTypeHIView) == 0) return kResultTrue;   // 兼容老宿主
#elif defined (_WIN32)
    if (std::strcmp (type, kPlatformTypeHWND) == 0)   return kResultTrue;
#endif
    return kResultFalse;
}

tresult PLUGIN_API PlugView::attached (void* parent, FIDString /*type*/)
{
    // 宿主可能先 createView（那时处理器还没接上）后 connect，
    // 所以挂载时用解析器再取一次最新后端 —— 拿不到才退回空后端。
    if (!m_backend && m_resolver)
        m_backend = m_resolver ();

    // 注意：m_backend 允许为 null —— 宿主可能在后端注入前就打开视图做探测。
    // 这种情况照样把视图挂上去，界面显示"未载入音频"，等后端就绪后再拖文件。
    if (!parent)
    {
        ap::crashLog ("编辑器 attached 被拒：parent 为空");
        return kResultFalse;
    }

    ap::crashLog ("编辑器 attached：parent=%p", parent);

    // 宿主重新挂载时先清掉旧视图，否则旧视图和它的定时器都会泄漏
    if (m_view)
    {
        ::GUIView* old = (__bridge ::GUIView*) m_view;
        m_view = nullptr;
        [old stopTimer];
        [old stopDrag];
        [old removeFromSuperview];   // ARC：父视图持有的那份引用释放即销毁，无需手动 release
    }

    NSView* parentView = (__bridge NSView*) parent;
    ::GUIView* v = [[::GUIView alloc] initWithBackend:m_backend];
    v.frame = NSMakeRect (0, 0, kPanelWDef, kPanelH);
    [v setFrameOrigin:NSZeroPoint];
    [parentView addSubview:v];
    m_view = (__bridge void*) v;
    ap::crashLog ("编辑器视图已挂载（子视图已加入 parent，window=%p）", (__bridge void*) parentView.window);
    return kResultOk;
}

tresult PLUGIN_API PlugView::removed ()
{
    ap::crashLog ("编辑器 removed 被调用（m_view=%p）", m_view);
    if (!m_view)
        return kResultOk;

    ::GUIView* v = (__bridge ::GUIView*) m_view;
    m_view = nullptr;

    // 顺序很重要：先停定时器，再摘视图。定时器以 20Hz 访问音频后端，
    // 而宿主可能紧接着就销毁插件实例 —— 旧顺序下就是「关掉窗口闪退」。
    [v stopTimer];
    [v stopDrag];      // 正在拖网格时关窗口，光标栈会残留
    [v removeFromSuperview];   // ARC：父视图持有它；摘下来引用归零即销毁
    ap::crashLog ("编辑器视图已移除");
    return kResultOk;
}

tresult PLUGIN_API PlugView::onWheel (float) { return kResultOk; }
tresult PLUGIN_API PlugView::onKeyDown (char16, int16, int16) { return kResultOk; }
tresult PLUGIN_API PlugView::onKeyUp (char16, int16, int16) { return kResultOk; }

// ⚠️ 视图已经建好时必须【如实汇报当前尺寸】—— 报默认值的话，用户刚拖好的宽度
//    会被宿主的缩放框架弹回去（规范也要求这里返回「当前」而不是「期望」尺寸）。
tresult PLUGIN_API PlugView::getSize (ViewRect* size)
{
    if (!size) return kInvalidArgument;
    size->left = 0;
    size->top = 0;

    if (m_view)
    {
        const NSSize s = ((__bridge ::GUIView*) m_view).frame.size;
        size->right  = static_cast<int32> (s.width);
        size->bottom = static_cast<int32> (s.height);
    }
    else
    {
        size->right  = static_cast<int32> (kPanelWDef);
        size->bottom = static_cast<int32> (kPanelH);
    }
    return kResultOk;
}

// 宿主在缩放过程中回调：把矩形钳制到允许范围（宽度 640~1700，高度锁死 384）。
// ⚠️ 高度必须【强制】回 384，而不是「不限制」—— 纵向位置和帮助层行数预算都是
//    按 384 排的，放任高度变化会让下半部分控件与页脚错位。
tresult PLUGIN_API PlugView::onSize (ViewRect* newSize)
{
    if (!m_view || !newSize)
        return kResultOk;

    const CGFloat w = newSize->right - newSize->left;
    const CGFloat h = newSize->bottom - newSize->top;

    ::GUIView* v = (__bridge ::GUIView*) m_view;
    // 改 frame 会触发 AppKit 按各子视图的 autoresizingMask 重排（波形区随之变宽）
    [v setFrameSize:NSMakeSize (w, h)];
    [v setNeedsDisplay:YES];
    return kResultOk;
}

tresult PLUGIN_API PlugView::onFocus (TBool /*state*/) { return kResultOk; }

tresult PLUGIN_API PlugView::setFrame (IPlugFrame* frame)
{
    // 本端不主动请求宿主改尺寸（面板宽度由宿主决定），但按规范把宿主回调存下来 ——
    // 将来若要支持「插件自己请求窗口变大」，调用的就是它。
    m_frame = frame;
    return kResultOk;
}

// 按 VST3 规范回答「本视图支持缩放」—— 别的宿主（Reaper / Cubase / Studio One …）
// 会据此开放窗口缩放边框，那时 onSize() 与 autoresizingMask 就起作用了。
// ⚠️ 但 MuseScore 4.x 完全忽略它：它把插件编辑器窗口的 min/max 设成同一个值（钉死），
//    而且从不调用本函数 —— 所以别对外承诺「可以拖窗口边缘」。
tresult PLUGIN_API PlugView::canResize () { return kResultTrue; }

tresult PLUGIN_API PlugView::checkSizeConstraint (ViewRect* rect)
{
    if (!rect) return kInvalidArgument;

    int32 w = rect->right - rect->left;
    int32 h = rect->bottom - rect->top;

    if (w < static_cast<int32> (kPanelWMin)) w = static_cast<int32> (kPanelWMin);
    if (w > static_cast<int32> (kPanelWMax)) w = static_cast<int32> (kPanelWMax);
    h = static_cast<int32> (kPanelH);   // 高度固定，不吃用户的纵向拖动

    rect->right  = rect->left + w;
    rect->bottom = rect->top  + h;
    return kResultOk;
}

} // namespace ap

//------------------------------------------------------------------------------
// C 链接薄封装：给纯 C++ 的验证器用（它不能引 Cocoa 头）。
// 返回 1 表示视图确实拿到了音频后端，界面里的按钮才真的有效。
//------------------------------------------------------------------------------
extern "C" int aplayViewHasBackend (Steinberg::IPlugView* v)
{
    auto* pv = dynamic_cast<ap::PlugView*> (v);
    return (pv && pv->backendForTest () != nullptr) ? 1 : 0;
}

//------------------------------------------------------------------------------
// 拍号变化表（后端状态）探针 —— 同上，给纯 C++ 的验证器用。
//
// 为什么要这一段：拍号变化表是「用户操作产生、关掉界面后仍要有意义」的状态，
// 存在处理器里。它的语义有四个容易写歪的点，全是纯逻辑、不需要界面就能测：
//   ① 初值必须是空的；② 同一个小节再设一次是【覆盖】不是追加；
//   ③ 非法分子 / 分母必须回落默认；④ 取消要能精确摘掉那一条。
// 返回 1 = 全部符合预期；负数 = 第几条不符（便于定位）。
//------------------------------------------------------------------------------
extern "C" int aplayTimeSigMapProbe (Steinberg::IPlugView* v)
{
    auto* pv = dynamic_cast<ap::PlugView*> (v);
    ap::PlugView::Backend* b = pv ? pv->backendForTest () : nullptr;
    if (!b) return 0;

    std::vector<ap::TimeSigChange> out;
    b->getTimeSigChanges (out);
    if (!out.empty ()) return -1;

    b->setTimeSigChange (3, 3, 4);
    b->setTimeSigChange (6, 6, 8);
    b->getTimeSigChanges (out);
    if (out.size () != 2) return -2;
    if (out[0].bar != 3 || out[0].beats != 3 || out[0].denom != 4) return -3;
    if (out[1].bar != 6 || out[1].beats != 6 || out[1].denom != 8) return -4;

    // ② 同一个小节再设一次 = 覆盖，不是追加
    b->setTimeSigChange (3, 5, 4);
    b->getTimeSigChanges (out);
    if (out.size () != 2) return -5;
    if (out[0].bar != 3 || out[0].beats != 5 || out[0].denom != 4) return -6;

    // ③ 非法值回落默认（分子 0 / 分母 7 都非法 → 4/4）
    b->setTimeSigChange (9, 0, 7);
    b->getTimeSigChanges (out);
    if (out.size () != 3) return -7;
    if (out[2].bar != 9 || out[2].beats != 4 || out[2].denom != 4) return -8;

    // bar < 1 一律归到第 1 小节
    b->setTimeSigChange (0, 3, 4);
    b->getTimeSigChanges (out);
    if (out.size () != 4) return -9;
    if (out[0].bar != 1 || out[0].beats != 3) return -10;

    // ④ 取消要精确：摘掉 1 / 3 / 9，剩下的只有 6；取消不存在的 key 也不能出问题
    b->clearTimeSigChange (1);
    b->clearTimeSigChange (3);
    b->clearTimeSigChange (9);
    b->clearTimeSigChange (77);
    b->getTimeSigChanges (out);
    if (out.size () != 1) return -11;
    if (out[0].bar != 6) return -12;

    b->clearTimeSigChange (6);
    b->getTimeSigChanges (out);
    if (!out.empty ()) return -13;

    return 1;
}

/// 给后端设一条拍号变化（验证器测「随状态保存 / 恢复」用）
extern "C" void aplayTimeSigSet (Steinberg::IPlugView* v, int bar, int beats, int denom)
{
    auto* pv = dynamic_cast<ap::PlugView*> (v);
    ap::PlugView::Backend* b = pv ? pv->backendForTest () : nullptr;
    if (b) b->setTimeSigChange (bar, beats, denom);
}

/// 回填当前拍号变化表（buf = 长度 cap 的三元组数组 [bar,beats,denom,...]），
/// 返回条数。buf 为空时只报条数。
extern "C" int aplayTimeSigDump (Steinberg::IPlugView* v, int* buf, int cap)
{
    auto* pv = dynamic_cast<ap::PlugView*> (v);
    ap::PlugView::Backend* b = pv ? pv->backendForTest () : nullptr;
    if (!b) return -1;

    std::vector<ap::TimeSigChange> out;
    b->getTimeSigChanges (out);
    const int n = static_cast<int> (out.size ());
    if (!buf) return n;
    for (int i = 0; i < n && i < cap; ++i)
    {
        const ap::TimeSigChange& c = out[static_cast<size_t> (i)];
        buf[i * 3 + 0] = c.bar;
        buf[i * 3 + 1] = c.beats;
        buf[i * 3 + 2] = c.denom;
    }
    return n;
}
