//==============================================================================
// gui_repro.mm — 复现「点开音频 / 拖入文件 → 宿主闪退」
//
// 为什么需要它：
//   MuseScore 里的崩溃日志只给出方法名和偏移，无法确定是哪一条指令、
//   更无法在改动后确认修好了。这个程序直接把真实的 GUIView 建出来，
//   按崩溃日志里的时序驱动它，用退出码给出可机读的结论。
//
// 复现的时序（与 APLAY.log 中的崩溃现场一致）：
//   A. 建视图 → 让 autorelease 池排空 → 调用 loadPath:
//      （对应 loadPath: 里给 offsetSlider 发消息的那次崩溃）
//   B. 建视图 → 池排空 → 调用 hasAudioURL: 两次
//      （对应 hasAudioURL: 里给静态 NSSet 发消息的那次崩溃）
//
// 用法: gui_repro <load|drag> <音频文件>
//   退出码 0 = 正常，非 0 = 复现/异常
//==============================================================================

#include "gui.h"
#include "audiofile.h"
#include "crashguard.h"

#import <AppKit/AppKit.h>
#import <objc/message.h>

#include <cmath>
#include <cstdio>
#include <string>

//------------------------------------------------------------------------------
// 测试后端：直接用 Player，等价于插件里 StableBackend 转发到处理器后的行为
//------------------------------------------------------------------------------
class TestBackend : public ap::PlugView::Backend
{
public:
    bool loadFile (const std::string& path) override
    {
        ap::AudioData d;
        std::string err;
        if (!ap::decodeAudioFile (path, d, err)) { m_err = err; return false; }
        return m_player.setAudio (std::move (d));
    }
    void unloadFile () override { m_player.clearAudio (); }
    bool hasAudio () const override { return m_player.hasAudio (); }
    double durationSec () const override { return m_player.durationSec (); }
    std::string currentPath () const override { return m_path; }
    std::string lastError () const override { return m_err; }

    void waveformPeaks (std::vector<float>& out, int buckets,
                        double fromSec, double toSec) const override
    {
        out.assign (static_cast<size_t> (buckets > 0 ? buckets : 1), 0.f);
        m_player.withAudio ([&] (const ap::AudioData& d)
        {
            const size_t n = d.numFrames ();
            if (n == 0 || d.sampleRate == 0) return;

            double f0 = fromSec, f1 = toSec;
            if (!(f1 > f0)) { f0 = 0.0; f1 = d.durationSec (); }
            if (f0 < 0.0) f0 = 0.0;
            if (f1 > d.durationSec ()) f1 = d.durationSec ();
            if (f1 <= f0) return;

            size_t s0 = static_cast<size_t> (f0 * d.sampleRate);
            size_t s1 = static_cast<size_t> (f1 * d.sampleRate);
            if (s0 >= n) return;
            if (s1 > n) s1 = n;
            if (s1 <= s0) return;

            const size_t b = out.size ();
            const double span = static_cast<double> (s1 - s0);
            for (size_t i = 0; i < b; ++i)
            {
                const size_t a0 = s0 + static_cast<size_t> (span * i / b);
                size_t a1 = s0 + static_cast<size_t> (span * (i + 1) / b);
                if (a1 <= a0) a1 = a0 + 1;
                if (a1 > s1) a1 = s1;
                float peak = 0.f;
                for (size_t j = a0; j < a1; ++j)
                {
                    const float a = std::fabs (d.samples[j * d.numChannels]);
                    if (a > peak) peak = a;
                }
                out[i] = peak;
            }
        });
    }

    void setOffsetSec (float s) override { m_player.setStartOffsetSec (s); }
    float offsetSec () const override { return m_player.startOffsetSec (); }
    void setVolume (float v) override { m_player.setVolume (v); }
    float volume () const override { return m_player.volume (); }
    void setPlaying (bool b) override { m_player.setPlaying (b); }
    bool playing () const override { return m_player.playing (); }
    void setLooping (bool b) override { m_player.setLooping (b); }
    bool looping () const override { return m_player.looping (); }
    void seekTo (double s) override { ++seekCalls; m_player.seekTo (s); }
    double positionSec () const override { return m_player.positionSec (); }

    void setGridBPM (float bpm) override { m_bpm = bpm; }
    float gridBPM () const override { return m_bpm; }
    void setGridBeatsPerBar (int b) override { m_beats = b; }
    int gridBeatsPerBar () const override { return m_beats; }
    void setGridBeatDenominator (int d) override { m_denom = d; }
    int gridBeatDenominator () const override { return m_denom; }

    // 假的宿主时间轴：默认「宿主没给」，测试时可按需打开，
    // 用来走通「自动跟随乐谱速度 + 画谱面播放头」这条分支。
    ap::PlugView::Backend::HostTimeline hostTimeline () const override
    {
        ap::PlugView::Backend::HostTimeline t;
        t.tempoValid = fakeTimeline;
        t.bpm = 120.0f;
        t.timeSigValid = fakeTimeline;
        t.beatsPerBar = 4;
        t.playheadValid = fakeTimeline;
        t.playheadSec = 3.5;
        t.playing = true;
        return t;
    }

    bool fakeTimeline = false;
    int  seekCalls = 0;   ///< seekTo 被调用次数 —— 回归防线：拖动波形不该 seek

private:
    mutable ap::Player m_player;
    std::string m_path;
    std::string m_err;
    float m_bpm = 0.0f;   ///< 0 = 自动
    int   m_beats = 4;
    int   m_denom = 4;
};

//------------------------------------------------------------------------------
// 假的拖放信息：只要能被问出 draggingPasteboard 就够了，
// 因为 hasAudioURL: 内部只用到这一个方法（ObjC 是动态派发，无需真实现协议）
//------------------------------------------------------------------------------
@interface FakeDrag : NSObject
{
    NSPasteboard* _pb;
}
- (instancetype)initWithPasteboard:(NSPasteboard*)pb;
- (NSPasteboard*)draggingPasteboard;
@end

@implementation FakeDrag
- (instancetype)initWithPasteboard:(NSPasteboard*)pb
{
    if (self = [super init]) _pb = [pb retain];
    return self;
}
- (void)dealloc { [_pb release]; [super dealloc]; }
- (NSPasteboard*)draggingPasteboard { return _pb; }
@end

//------------------------------------------------------------------------------
// 把 GUIView 的动作声明出来（实现体在 gui.mm 里，这里只做编译期声明）
//------------------------------------------------------------------------------
@interface NSView (APLAYRepro)
- (void)loadPath:(NSString*)path;
- (BOOL)hasAudioURL:(id<NSDraggingInfo>)sender;
- (void)tick;
- (void)stopTimer;
- (void)stopDrag;
@end

//------------------------------------------------------------------------------
// 递归找到波形视图（类名叫 WaveformView，不需要 import 就能按名字判断）
//------------------------------------------------------------------------------
static NSView* findWaveView (NSView* v)
{
    for (NSView* s in [v subviews])
    {
        if ([NSStringFromClass ([s class]) isEqualToString:@"WaveformView"])
            return s;
        NSView* r = findWaveView (s);
        if (r) return r;
    }
    return nil;
}

//------------------------------------------------------------------------------
// 离屏渲染：真的跑一遍 drawRect（网格循环、包络绘制都在里面）。
// 之前只验证「能建出视图」，绘制代码里的死循环 / 除零照样能溜过去。
//------------------------------------------------------------------------------
static int drawOffscreen (NSView* v)
{
    const NSRect b = [v bounds];
    const int w = (int) b.size.width, h = (int) b.size.height;
    if (w < 2 || h < 2) return 0;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB ();
    CGContextRef cg = CGBitmapContextCreate (nullptr, (size_t) w, (size_t) h, 8,
                                             (size_t) (w * 4), cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease (cs);
    if (!cg) return 0;

    NSGraphicsContext* ctx = [NSGraphicsContext graphicsContextWithCGContext:cg flipped:YES];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:ctx];
    [v displayRectIgnoringOpacity:b inContext:ctx];
    [NSGraphicsContext restoreGraphicsState];
    CGContextRelease (cg);
    return 1;
}

static void step (const char* what)
{
    std::printf ("  · %s\n", what);
    std::fflush (stdout);
}

// 宿主窗口：真实的 MuseScore 里编辑器是嵌在窗口里的，坐标转换依赖窗口存在。
// 这里建一个不显示的窗口，让 [view convertPoint:fromView:nil] 有确定语义
// —— 否则合成的鼠标事件位置没有意义、断言也就失去价值。
static NSWindow* g_hostWindow = nil;

int main (int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "load";
    const char* file = argc > 2 ? argv[2] : nullptr;

    @autoreleasepool { [NSApplication sharedApplication]; }

    std::printf ("[复现] 模式=%s\n", mode);
    std::fflush (stdout);

    TestBackend backend;
    NSView* guiView = nil;

    // ---- 1. 模拟宿主挂载编辑器视图（时序与 MuseScore 一致）----
    @autoreleasepool
    {
        g_hostWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect (0, 0, 620, 460)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
        NSView* container = [g_hostWindow contentView];

        auto* pv = new ap::PlugView (&backend, nullptr);
        pv->addRef ();
        const Steinberg::tresult r =
            pv->attached ((__bridge void*) container, Steinberg::kPlatformTypeNSView);
        // 由宿主持有，本进程不释放插件视图对象

        step (r == Steinberg::kResultOk ? "编辑器视图挂载成功" : "编辑器视图挂载失败");

        NSArray* subs = [container subviews];
        if (subs.count == 0)
        {
            std::printf ("[复现] 视图未被挂载，无法继续\n");
            return 2;
        }
        guiView = [[subs objectAtIndex:subs.count - 1] retain];   // MRC 文件，自己持有
    }
    // ★ 关键：这里的池排空，模拟「编辑器打开后过了几秒」
    step ("autorelease 池已排空（模拟编辑器打开数秒后）");

    int rc = 0;

    // ---- 子视图断言：音量滑块必须真的挂进视图层级 ----
    // 之前滑块既没 addSubview 也没 retain —— 结果是「界面上看不见」
    // 并且「池排空后变成野指针」。这条断言同时锁住这两件事。
    @autoreleasepool
    {
        int sliders = 0;
        NSArray* subs = [guiView subviews];
        for (NSView* sv in subs)
            if ([sv isKindOfClass:[NSSlider class]]) ++sliders;

        std::printf ("  · 界面控件: 共 %lu 个子视图，其中滑块 %d 个\n",
                     (unsigned long) subs.count, sliders);
        std::fflush (stdout);

        if (sliders < 1)
        {
            std::printf ("[复现] ✗ 滑块未挂进视图（音量滑块缺失）\n");
            rc = 4;
        }
    }

    if (std::strcmp (mode, "load") == 0)
    {
        if (!file)
        {
            std::printf ("[复现] 缺少音频文件参数\n");
            return 2;
        }
        NSString* path = [NSString stringWithUTF8String:file];
        step ("调用 loadPath:（等价于界面里点『打开音频』/拖入文件）");
        @autoreleasepool
        {
            [guiView loadPath:path];
        }
        step ("loadPath: 正常返回");
    }
    else if (std::strcmp (mode, "drag") == 0)
    {
        if (!file)
        {
            std::printf ("[复现] 缺少音频文件参数\n");
            return 2;
        }

        NSPasteboard* pb = [NSPasteboard pasteboardWithUniqueName];
        [pb declareTypes:@[ NSPasteboardTypeFileURL ] owner:nil];
        [pb setString:[NSURL fileURLWithPath:[NSString stringWithUTF8String:file]].absoluteString
              forType:NSPasteboardTypeFileURL];

        FakeDrag* drag = [[FakeDrag alloc] initWithPasteboard:pb];
        BOOL first = NO;

        // 第 1 次：拖拽刚进入视图
        @autoreleasepool
        {
            step ("第 1 次 hasAudioURL:（拖拽进入）");
            first = [guiView hasAudioURL:(id<NSDraggingInfo>) drag];
        }
        step (first ? "第 1 次返回「是音频文件」（说明这条路径真的走到了）"
                    : "第 1 次返回「否」——测试没命中目标路径，需调整");

        // 第 2 次：拖拽过程中再次询问（AppKit 会重复询问）
        @autoreleasepool
        {
            step ("第 2 次 hasAudioURL:（同一个拖拽会话中再次询问）");
            const BOOL second = [guiView hasAudioURL:(id<NSDraggingInfo>) drag];
            step (second ? "第 2 次返回「是音频文件」" : "第 2 次返回「否」");
        }

        [drag release];
        [pb release];
        if (rc == 0) rc = first ? 0 : 3;
    }
    else if (std::strcmp (mode, "grid") == 0)
    {
        if (!file)
        {
            std::printf ("[复现] 缺少音频文件参数\n");
            return 2;
        }
        NSString* path = [NSString stringWithUTF8String:file];

        // ---- 载入 ----
        @autoreleasepool { [guiView loadPath:path]; }
        step ("已载入音频");

        NSView* wave = findWaveView (guiView);
        if (!wave)
        {
            std::printf ("[复现] ✗ 找不到波形视图\n");
            return 5;
        }

        // ---- 面板控件清点（少了任何一个都是功能缺失）----
        int sliders = 0, buttons = 0, fields = 0, popups = 0, steppers = 0;
        for (NSView* sv in [guiView subviews])
        {
            if ([sv isKindOfClass:[NSSlider class]])      ++sliders;
            if ([sv isKindOfClass:[NSButton class]])      ++buttons;
            if ([sv isKindOfClass:[NSTextField class]])   ++fields;
            if ([sv isKindOfClass:[NSPopUpButton class]]) ++popups;
            if ([sv isKindOfClass:[NSStepper class]])     ++steppers;
        }
        std::printf ("  · 面板控件: 滑块 %d，按钮 %d，文本框 %d，下拉 %d，步进 %d\n",
                     sliders, buttons, fields, popups, steppers);

        // 偏移改由「波形上 ⌘/⌥拖动」操作，偏移滑块/微调按钮已删除。
        // 循环按钮已删除（「跟谱面走」模式下无实际作用）。
        // 现在：1 个音量滑块，7 个按钮（打开/？帮助/－/＋/全览/回到播放头/播放），
        // 1 个 BPM 步进器（恢复手动 BPM，拍号 N/M）。
        if (sliders < 1)   { std::printf ("[复现] ✗ 滑块缺失\n");   rc = 6; }
        if (buttons < 6)   { std::printf ("[复现] ✗ 按钮数量不足\n"); rc = 7; }
        if (fields < 5)    { std::printf ("[复现] ✗ 文本框缺失\n"); rc = 8; }
        if (popups < 1)    { std::printf ("[复现] ✗ 拍号下拉缺失\n"); rc = 9; }
        if (steppers < 1)  { std::printf ("[复现] ✗ BPM 步进缺失\n"); rc = 10; }

        // ---- 网格绘制：宿主没给时间轴（退回默认 120 BPM / 4/4）----
        backend.fakeTimeline = false;
        backend.setGridBeatsPerBar (4);
        backend.setGridBeatDenominator (4);
        if (!drawOffscreen (wave)) { std::printf ("[复现] ✗ 离屏渲染失败\n"); rc = 11; }
        else step ("绘制网格（默认 120 BPM，4/4）");

        // ---- 网格绘制：宿主给了速度/拍号/谱面位置 ----
        backend.fakeTimeline = true;
        @autoreleasepool
        {
            [guiView tick];     // 走一遍「记日志 + 画谱面播放头」
            [guiView tick];
        }
        if (!drawOffscreen (wave)) { std::printf ("[复现] ✗ 宿主时间轴下渲染失败\n"); rc = 12; }
        else step ("绘制网格（含谱面播放头）");

        // ---- 负偏移 + 12/8 拍号（验证分母参与每拍时长计算）----
        backend.setOffsetSec (-2.0f);
        backend.setGridBeatsPerBar (12);
        backend.setGridBeatDenominator (8);
        if (!drawOffscreen (wave)) { std::printf ("[复现] ✗ 负偏移渲染失败\n"); rc = 13; }
        else step ("绘制网格（偏移 -2 秒 / 12/8 拍号）");

        // ---- 缩放 / 平移 ----
        SEL selZoomBy = NSSelectorFromString (@"zoomBy:aroundX:");
        SEL selFit    = NSSelectorFromString (@"fitAll");
        if ([wave respondsToSelector:selZoomBy])
        {
            ((void (*) (id, SEL, double, CGFloat)) objc_msgSend) (wave, selZoomBy, 8.0, (CGFloat) 100.0);
            if (!drawOffscreen (wave)) { std::printf ("[复现] ✗ 放大后渲染失败\n"); rc = 14; }
            else step ("放大 8 倍后绘制（峰值按可见区间重新取样）");

            ((void (*) (id, SEL, double, CGFloat)) objc_msgSend) (wave, selZoomBy, 0.001, (CGFloat) 5.0);
            if (!drawOffscreen (wave)) { std::printf ("[复现] ✗ 极限缩小后渲染失败\n"); rc = 15; }
            else step ("极限缩小后绘制（视野钳制生效）");
        }
        else
        {
            std::printf ("[复现] ✗ 波形视图缺少 zoomBy:aroundX:\n");
            rc = 16;
        }
        if ([wave respondsToSelector:selFit])
            ((void (*) (id, SEL)) objc_msgSend) (wave, selFit);

        // ---- 无修饰键拖动 = 平移视图，且【绝不能】seek 播放位置 ----
        // 回归防线：旧版这里是「拖动 = 定位音频播放头」，会把音频拽离谱面，
        // 用户还得再点一次「回到谱面」才能恢复 —— 已改成纯视野平移。
        @autoreleasepool
        {
            backend.setOffsetSec (0.0f);
            // 先放大：全览下 viewStart 恒为 0，拖动无从体现。
            // 锚点取最左边，保证缩放后 viewStart 仍是 0，起点可预期。
            ((void (*) (id, SEL, double, CGFloat)) objc_msgSend)
                (wave, NSSelectorFromString (@"zoomBy:aroundX:"), 8.0, (CGFloat) 0.0);

            SEL selViewStart = NSSelectorFromString (@"viewStartSec");
            const double startBefore = ((double (*) (id, SEL)) objc_msgSend) (wave, selViewStart);
            const int    seeksBefore = backend.seekCalls;

            const NSInteger wn = g_hostWindow.windowNumber;
            NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                               location:NSMakePoint (300, 60)
                                          modifierFlags:0 timestamp:0 windowNumber:wn
                                                context:nil eventNumber:1 clickCount:1 pressure:1.0];
            NSEvent* drag = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                                               location:NSMakePoint (180, 60)
                                          modifierFlags:0 timestamp:0.01 windowNumber:wn
                                                context:nil eventNumber:2 clickCount:1 pressure:1.0];
            NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                             location:NSMakePoint (180, 60)
                                        modifierFlags:0 timestamp:0.02 windowNumber:wn
                                              context:nil eventNumber:3 clickCount:1 pressure:1.0];
            step ("无修饰键按下 → 左拖 120px → 松开（应平移视野、不 seek）");
            [wave mouseDown:down];
            [wave mouseDragged:drag];
            [wave mouseUp:up];

            const double startAfter = ((double (*) (id, SEL)) objc_msgSend) (wave, selViewStart);
            std::printf ("  · 视野起点 %.4f → %.4f 秒；seekTo 调用 %d → %d\n",
                         startBefore, startAfter, seeksBefore, backend.seekCalls);

            if (backend.seekCalls != seeksBefore)
            {
                std::printf ("[复现] ✗ 拖动波形竟然 seek 了播放位置（应只改视野）\n");
                rc = 18;
            }
            else if (!(startAfter > startBefore))
            {
                std::printf ("[复现] ✗ 无修饰键拖动没有平移视野\n");
                rc = 19;
            }

            // 恢复全览：紧接着的「⌥ 拖动网格」测试按 60px 位移断言偏移变化量，
            // 视野缩放级别一变那个量就不成立（实测被压到 0.098 秒 < 0.2 秒）。
            if ([wave respondsToSelector:selFit])
                ((void (*) (id, SEL)) objc_msgSend) (wave, selFit);
        }

        // ---- 「回到播放头」：seek 必须带上偏移，且视野要跟过去 ----
        // 回归防线：Windows / Linux 端曾经写成 seekTo(playheadSec)，漏掉起始偏移 ——
        // 点一次反而把两条播放头按偏移量错开，和这个按钮「修复分家」的职责完全相反。
        // 这里同时锁住两件事：① 位置算对（原文里带偏移）；② 视野真的被带回播放位置。
        @autoreleasepool
        {
            backend.fakeTimeline = true;     // 让宿主提供谱面播放头
            backend.setOffsetSec (1.5f);     // 故意给个非零偏移，才能验出「漏加偏移」

            SEL selViewStart = NSSelectorFromString (@"viewStartSec");
            SEL selFit       = NSSelectorFromString (@"fitAll");

            ((void (*) (id, SEL)) objc_msgSend) (wave, selFit);
            // 放大到 8 倍：全览时整段都在画面里，视野无需移动，测不出「跳回去」。
            ((void (*) (id, SEL, double, CGFloat)) objc_msgSend)
                (wave, NSSelectorFromString (@"zoomBy:aroundX:"), 8.0, (CGFloat) 0.0);

            const double startBefore = ((double (*) (id, SEL)) objc_msgSend) (wave, selViewStart);
            const int    seeksBefore = backend.seekCalls;

            step ("点「回到播放头」（应 seek 到 谱面位置+偏移，并把视野跳过去）");
            ((void (*) (id, SEL, id)) objc_msgSend)
                (guiView, NSSelectorFromString (@"backToPlayhead:"), nil);

            const double startAfter = ((double (*) (id, SEL)) objc_msgSend) (wave, selViewStart);
            const double posAfter   = backend.positionSec ();
            const double want       = backend.hostTimeline ().playheadSec + backend.offsetSec ();

            std::printf ("  · seekTo 调用 %d → %d；音频位置 %.4f 秒（期望 %.4f）；"
                         "视野起点 %.4f → %.4f\n",
                         seeksBefore, backend.seekCalls, posAfter, want,
                         startBefore, startAfter);

            if (backend.seekCalls <= seeksBefore)
            {
                std::printf ("[复现] ✗ 「回到播放头」没有重新对齐（根本没调用 seekTo）\n");
                rc = 20;
            }
            else if (std::fabs (posAfter - want) > 0.01)
            {
                std::printf ("[复现] ✗ 「回到播放头」seek 目标漏了起始偏移\n");
                rc = 21;
            }
            else if (std::fabs (startAfter - startBefore) < 1e-6)
            {
                std::printf ("[复现] ✗ 「回到播放头」没有把视野带回播放位置\n");
                rc = 22;
            }

            // 复位，避免影响后续「⌥ 拖动网格」的前置条件
            backend.setOffsetSec (0.0f);
            if ([wave respondsToSelector:selFit])
                ((void (*) (id, SEL)) objc_msgSend) (wave, selFit);
        }

        // ---- ⌥ 拖动网格 = 调整起始偏移 ----
        backend.setOffsetSec (0.0f);
        @autoreleasepool
        {
            const NSInteger wn = g_hostWindow.windowNumber;
            // 位置是「窗口坐标」，视图内部会用 convertPoint 转到自己的坐标系
            NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                               location:NSMakePoint (200, 60)
                                          modifierFlags:NSEventModifierFlagOption
                                              timestamp:0 windowNumber:wn context:nil
                                            eventNumber:1 clickCount:1 pressure:1.0];
            NSEvent* drag = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                                               location:NSMakePoint (260, 60)
                                          modifierFlags:NSEventModifierFlagOption
                                              timestamp:0.01 windowNumber:wn context:nil
                                            eventNumber:2 clickCount:1 pressure:1.0];
            NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                             location:NSMakePoint (260, 60)
                                        modifierFlags:NSEventModifierFlagOption
                                            timestamp:0.02 windowNumber:wn context:nil
                                          eventNumber:3 clickCount:1 pressure:1.0];
            step ("⌥ 按下 → 右拖 60px → 松开（拖动网格）");
            [wave mouseDown:down];
            [wave mouseDragged:drag];
            [wave mouseUp:up];
        }
        {
            const float off = backend.offsetSec ();
            std::printf ("  · 拖动后偏移 = %+.3f 秒\n", off);
            if (!(off > 0.2f))
            {
                std::printf ("[复现] ✗ ⌥拖动网格没有改变起始偏移\n");
                rc = 17;
            }
        }

        // ---- 关窗口不能残留拖动状态 ----
        @autoreleasepool
        {
        SEL selStopDrag = NSSelectorFromString (@"stopDrag");
        if ([guiView respondsToSelector:selStopDrag])
            ((void (*) (id, SEL)) objc_msgSend) (guiView, selStopDrag);
        step ("清理拖动状态");
        }
        step ("清理拖动状态");
    }
    else
    {
        std::printf ("[复现] 未知模式: %s\n", mode);
        return 2;
    }

    // ---- 收尾：摘掉视图，确认关闭路径也不崩 ----
    @autoreleasepool
    {
        step ("关闭编辑器（stopTimer + removeFromSuperview）");
        SEL s = NSSelectorFromString (@"stopTimer");
        if ([guiView respondsToSelector:s])
            ((void (*) (id, SEL)) objc_msgSend) (guiView, s);
        [guiView removeFromSuperview];
    }

    [guiView release];
    if (rc == 0)
        std::printf ("[复现] 全部步骤正常完成 ✓\n");
    else
        std::printf ("[复现] ✗ 存在失败项（退出码 %d）\n", rc);
    return rc;
}
