# 大伟鼓谱 MuseScore 音频播放器（VST3）— 交付概览

## v1.2.5（2026-10-10）

用户在 macOS 上实测反馈：说明文档写着「编辑器窗口可以拖动放大缩小」，但**鼠标放到
窗口边缘没有任何反应**，也拖不动。

### 根因：macOS 端从未实现窗口缩放，文档却按三端写

- `gui.mm` 的 `PlugView::canResize()` 一直返回 `kResultFalse`（当时注释的理由是
  「用户拖动窗口会让布局错乱，所以直接拒绝」），`getSize()` / `checkSizeConstraint()`
  把尺寸硬钉在 640x384。**只有 Windows / Linux 实现了缩放**，README / 使用说明却写成
  「三端都可以」→ 文档超出实现。
- 宿主按 VST3 规范办事：`canResize()` 为假时就不给窗口加可缩放样式，所以边缘既没有
  缩放光标、拖拽也无效。

### 修法：放开宽度，高度继续锁死

- ⭐ **不写「按尺寸重算坐标」的布局函数**，改用 AppKit 的 `autoresizingMask` 三档规则
  （右贴边 `NSViewMinXMargin` / 随宽拉伸 `NSViewWidthSizable` / 其余不设 = 原地不动）。
  程序化创建的视图默认 mask 为 0，正好就是「左对齐定宽」要的效果 ——
  实测确认过默认值与三种锚定的语义。这样以后新增控件不需要去登记坐标。
- `canResize()` → `kResultTrue`；`getSize()` 如实汇报**当前**尺寸（视图未建时回默认值）；
  `checkSizeConstraint()` 把宽度钳在 640~1700、**高度强制回 384**；
  `onSize()` 把新尺寸写进 `NSView` 的 frame，触发子视图重排。
- **只能变宽，不能变窄**：`kPanelWMin = kPanelWDef = 640`。实测第 4 行的
  快捷键提示需 345pt（起点 x=226），再窄就会被截断 —— 与其留着截断，不如不允许缩窄。
- **为什么高度不放**：每一行的纵向位置、以及帮助覆盖层的「行数预算」都是按 384 排的，
  放开高度要连带重算，收益（波形变高）远小于风险。Windows / Linux 两端本来就是可拖高的。

### 同步的三处

- `validate.cpp`：`canResize` 断言由 `kResultFalse` 改成 `kResultTrue`；新增 4 条
  `checkSizeConstraint()` 用例（过窄/超上限/范围内/带非零原点），锁住「按 width/height
  算而不是直接覆写 right/bottom」。
- `gui_repro.mm`：按宿主调用时序跑一遍 `checkSizeConstraint → onSize`，断言面板变 900 宽、
  波形区变 872 宽、帮助覆盖层同步铺满（退出码 36）。用完复原成 640，避免影响后续用例
  的鼠标坐标系。
- 文档：README / 使用说明 / 安装说明 / 本文件里「三端都可以拖动窗口边缘自由改大小」
  之类说法改成「把窗口拉宽」；使用说明补一句「macOS 高度固定」。
- 版本号 1.2.4 → 1.2.5（9 处）。

---

## v1.2.4（2026-10-09）

发布前的一遍**文案清理**：界面与文档的提示语统一成「只说怎么操作、只说看到什么」，
把解释性、防御性的措辞删掉，读起来更像人话。

- 覆盖面：三端帮助面板、macOS 的工具提示与状态栏、README / 使用说明 / 安装说明。
- **只改文字，行为一律不动**：指南不被滚轮关掉这件事本身照旧。
- 版本号 1.2.3 → 1.2.4（9 处）。

---

## v1.2.3（2026-10-09）

用户在 Windows 端实测第 4 条反馈，一条行为修复，三端同步。

### 关掉插件界面再打开 → 视野必须回到原处

**症状**：把插件界面关掉再打开，波形**变回整曲全览**（"全蓝"一片），而不是关之前
放大好的那一段；用户原话：「而不是现在播放的那一个小节的，或者说之前放大的那个
数据了」。

**根因**：这是**视图寿命 < 处理器寿命**这条老账的第二个未覆盖项。宿主关界面只销毁
`PlugView`，重开时视图是**新建**的 —— 上一轮已经修过「BPM / 拍号 / 偏移 / 音量」的
回读，但**视野（viewStart/viewSpan）没被纳入**：它只存在视图对象里，视图一销毁就
没了，新视图拿的是初值（macOS 的 `viewLen = 0` 恰好表示"全曲全览"）。
换句话说：**视野也是用户的操作结果，必须和 BPM 一样存在后端**。

**修法**（三端一致的「两半」）：

1. **后端加接口**：`PlugView::Backend` 新增 `setViewState/getViewState`；
   `APlayProcessor` 用**一把独立的小锁**（与音频线程毫无交集，不动 `m_mutex`，
   见铁律 12）存两个 double。**换音频文件时清零** = "没设置过"，避免把上一首歌的
   视野带到新歌上。
2. **视图构造时回读**：三端在视图建完、控件回读之后各读一次 —— 后端存过就用它
   （连放大倍数一起恢复），没存过就回落到「新载入」的默认视野（`kDefaultViewSpanSec`
   = 8 秒）。Windows / Linux 顺手把这段默认视野逻辑从 `afterLoad` 抽成
   `applyDefaultView()`，两处共用，避免两份规则漂移。
3. **视野变化时回存**：回存点**故意放在各自的 20Hz 刷新里**（mac `tick` /
   Win `onTimer` / Linux `runLoop`），用「发现视野变了就写回」的方式收口。
   理由：视野的改动入口有滚轮、拖动、缩放、全览、回到播放头、自动跟随六七处，
   逐个入口去写**必然漏一个**（铁律 9 踩过多次）；集中在一处比对则一个都漏不掉。

### 验证

- `gui_repro` 在已有的「重开编辑器」用例里扩了一条断言 —— 关界面前先用
  `zoomToSpan:fromStart:` 摆一个确定视野（2.0 秒跨度 / 起点 5.0 秒），跑一次 `tick`
  让它回存，再挂第二个编辑器视图，断言新视图的 `viewLenSec`/`viewStartSec`
  就是 5.0/2.0（`rc=35`）。期望值独立写死，不引用源码常量。
- ⚠️ 该断言**不能退化成"音频太短就跳过"**：build.sh 的测试音频是 20 秒，必然真跑；
  日志里若出现"跳过"，说明测试音频被改短了。
- 版本号 1.2.2 → 1.2.3（9 处）。

---

## v1.2.2（2026-10-09）

延续 Windows 端实测反馈的三条行为改动，全部三端同步。

### 一、默认视野放大到 8 秒（不再整曲全览）

**症状**：载入音频后是整曲全览，波峰糊成一片、看不清鼓点，用户每次都得先手动
放大 11~12 下才到能看清的尺度。

**修法**：载入后默认给 `kDefaultViewSpanSec`（8 秒 ≈ 4 小节 @120BPM 4/4），
从起始偏移处开始看；音频本身不足 8 秒时仍走全览。macOS 端本来就是 8 秒
（`zoomToSpan:fromStart:`），本轮把 Windows / Linux 对齐过来 —— 三端同名同值。

### 二、宿主换小节（**含暂停**）→ 波形跳到对应位置

**症状**：在乐谱里点小节换播放位置，波形视野纹丝不动。

**根因**：三端的跟随调用都被 `tl.playing` 门控，**只有播放中才跟随**。而扒谱恰恰
就是**暂停**着一个小节一个小节地点的；「乐谱空白也能直接定位到音乐」这个用法
完全落空。

**修法**：跟随条件改为 `tl.playheadValid && (tl.playing || hostJumped)`。
`hostJumped` = 相邻两帧谱面位置差 > `kHostJumpSec`(0.25s)，即**宿主自己跳的**
（点小节 / 循环回卷 / 拖播放头 / 方向键）。这条与「绝不抢用户视野」不冲突：
一条管「宿主在挪播放头」，一条管「用户在看别处」；自然播放推进仍只许顺滑滚。

### 三、「？帮助」不再被滚轮关掉

**症状**：点开指南后往下滚了一下滚轮（想看看还有没有内容），指南直接没了。

**根因**：Windows `mainProc` 的 `WM_MOUSEWHEEL`/`WM_MOUSEHWHEEL` 在 help 可见时
执行了 `hideHelp()`（当时的设计是「任意输入都表示知道了」）。

**修法**：滚轮只吞掉、不关闭。三端一起收口：
- **Windows**：滚轮分支 `return 0`，不调 `hideHelp()`；
- **Linux**：`onScroll` 开头 `if (m_helpVisible) return;` —— 原来根本没判断，滚轮会
  偷偷把视野滚走，用户点一下关掉指南才发现画面不在原处了；
- **macOS**：`HelpOverlayView` 补一个空 `scrollWheel:` —— 原来不实现，事件沿
  responder chain 上抛，可能顺带把下面的波形也滚了。

### 验证

- 本机 `build.sh` 八项全绿；`gui_repro` 新增两条断言：
  - `rc=33`：**暂停**状态下宿主跳变 → 视野必须跟过去（用新增的 `fakePlaying`
    把假宿主设成暂停）；
  - `rc=34`：载入后默认视野 = 8 秒（新增只读访问器 `viewLenSec`；期望值在测试里
    **独立写死 8.0**，故意不引用源码常量 —— 否则常量被改错了两边一起错）。
- ⚠️ Windows / Linux 仍只能靠 CI（本机既无 MSVC 也无 Linux 工具链）。
- 版本号 1.2.1 → 1.2.2（9 处）。

---

## v1.2.1 发布（2026-10-09）

本轮全部来自**用户在 Windows 端的实测反馈**（6 条），其中 3 条根因都不在 Windows
独有逻辑上，而是**布局/消息路由**这类结构性设计问题。改动范围经确认取「三端同步」。

### 一、Windows 帮助覆盖层「飘在主界面下面」

**症状**：点「？帮助」后主界面没被盖住，指南反而画在它**下面**，看起来像没反应。

**根因**：旧版把指南做成一个铺满面板的独立子窗口 `kHelpClass`，依赖「最后创建的
窗口 = z 序最上」。这条规则在 MuseScore 的窗口/合成层级下**不成立**，`SetWindowPos
(HWND_TOP)` 也扳不动 —— 整个做法从根上就不可靠。

**修法**：删掉独立子窗口，改成**隐藏所有子控件 + 由主窗口在自己的 `WM_PAINT` 里整块
自绘**（Linux 端本来就这么做）。层级不再参与，就不会错。配套三处：
`WM_ERASEBKGND` 返回 1（免闪烁）、`WM_PAINT` 处理完**不能落到 `DefWindowProc`**
（否则类的背景刷会把文字擦掉）、指南可见时任何鼠标按下都只表示「知道了」。

### 二、面板太窄 → 默认加宽到 680，并且可以手动拖大

- `kPanelW` **340 → 680**；新增 `kPanelWMin/HMin`(340/384) 与 `kPanelWMax/HMax`(1700/1200)。
- `canResize()` 改返回 `kResultTrue`，`checkSizeConstraint()` 用统一的 `clampPanelSize()`。
- **`getSize()` 视图建好后必须如实汇报【当前】尺寸** —— 若还报默认尺寸，宿主会照着旧
  尺寸折腾窗口，用户刚拖好的宽度就被弹回去。
- 布局从「写死坐标」改为**按实际客户区重算**（Windows `layoutChildren` / Linux
  `layoutAll`）：宽度方向右侧控件挂 `pw - pad - W`，中间留白自动分配 → **波形区随窗口
  变宽**；高度方向波形区吃掉上方余量，底部区块贴底（y 一律写 `bottomTop + 偏移`，
  ⚠️ 绝不写 158/182 这类绝对值）。
- `onTimer` 里加 `syncSizeToParent()`：宿主容器装不下时缩回去，防止右侧那几个按钮
  （打开音频 / 回到播放头 / 归零）被父窗口裁掉。

### 三、Windows 滚轮不左右移动波形

**根因**：`WM_MOUSEWHEEL` 在 Windows 上是发给**焦点窗口**的，不是鼠标指针下的窗口
（除非系统开了「悬停滚动非活动窗口」）。波形区几乎从不持有焦点 → 消息压根没送到。

**修法**：在主窗口按**光标屏幕坐标**（滚轮的 lParam 就是屏幕坐标）做 `waveHitTest`，
落在波形区就自己处理，**没命中则原样上抛给宿主**（不抢 MuseScore 自己的滚轮）。
另外补了 `WM_MOUSEHWHEEL`（横向滚轮 / 触控板左右滑）→ 同向平移视野；`onMouseDown`
里 `SetFocus(m_wave)` 让滚轮更可靠，⚠️ 但因此必须在 `waveProc` 里把
`WM_KEY*` 用 `PostMessageW` 转给宿主父窗口，否则**宿主空格键（播放/暂停）会被吃掉**。

### 四、播放中拖波形 → 音频变慢（重绘开销）

- 离屏位图**复用**（旧版每帧 `CreateCompatibleDC` + `CreateCompatibleBitmap`）；
  ⚠️ 双缓冲顺序仍必须是**先 `BitBlt` 再还原 `SelectObject(dc, oldBmp)`**。
- 鼠标驱动的重绘按 `kDragPaintGapMs`(36ms ≈ 28fps) 节流，被节流掉的那次交给 50ms
  定时器补。
- `onTimer` 不再无条件重绘：算一个**内容指纹**（位置 / 播放头 / 偏移 / 视野 / 播放中 /
  错位状态）比对，变了才 `InvalidateRect` —— 暂停或无音频时彻底不空转。

### 五、「回到播放头」之后播放头直接跑出画面

**根因**：Windows / Linux 端**根本没有实现自动跟随** —— `m_viewStart` 只被用户操作和
「回到播放头」改过。macOS 有、另两端没有，所以这个 bug 只在 Win 上暴露。

**修法**：按 macOS 的定稿规则逐条移植 `followPlayheadTo` / `recoverAutoFollow` /
`markUserScrolling`：① 用户手动操作期间完全不干预；② 播放头远在画面外（超一整个屏）
不追；③ 唯一例外是**宿主自己跳了**（相邻两帧谱面位置差 > `kHostJumpSec` = 0.25 秒）。
`zoomBy` / `zoomFit` / 滚轮 / 平移拖动都要置「用户在看别处」，停手约 2 秒后自动恢复。

### 六、新增「偏移归零」按钮（三端）

改偏移的唯一入口是「Ctrl/Alt + 在波形上拖动」，范围 ±3600 秒 —— 手滑拖到很大值时
反向拖回来要拖好几个屏。三端各加一个「归零」按钮；归零后若播放头因此跑到画面外，
就顺手把它带回画面（本来就在画面里则什么都不做，**不抢用户正在看的视野**）。

### 其它

- Linux 端帮助文案里的格式列表原来是错的（写了 M4A / AAC，实际不支持）——已改为
  「支持 MP3 / WAV / FLAC / OGG」。
- ⚠️ **三端帮助文案的行数预算各有上限**（mac 21 行 × 16px；Win/Linux 23 行 × 15px），
  改文案必须重算，见各端 `drawHelp` / `paintHelp` 旁的注释。
- ⚠️ **Linux 端从未实机验证**，且 CI 不渲染 GUI —— 「编译过 + CI 绿」**推不出**「界面能用」。

---

## v1.2.0 发布（2026-10-09）

本轮全部来自**用户实测**：三个交互/持久化 bug、一条写错的帮助文案、一个安装器
缺口，外加一个被 CI 拦下的 Windows 编译失败。**没有加新功能之外的东西** ——
唯一的「新功能」是底部署名可点击。

### 一、修 4 个用户实测问题

**1) 关掉插件界面再打开，手填的 BPM 变回 120**

根因不是「没保存」，而是**编辑器视图比后端短命**：宿主关界面只销毁 `IPlugView`，
后端 `Processor` 一直活着、值也一直在；重开时控件全是新建的，而 BPM 输入框
**一失焦就自动提交** → 把刚建出来的 120 写回后端 = 用户的值真被覆盖掉。
（日志铁证：`10:11:18 removed` → `10:11:21 PlugView 构造（后端=0x13b038d10）`，
后端地址与 10:06 那次完全相同。）

修法：三端各加「视图构造完成后从后端回读设置」——mac / Win `syncControlsFromBackend`
（BPM、拍号、偏移、音量），Linux `syncTimeSigFromBackend`（BPM/偏移/音量本就每帧现算）。
⚠️ **以后新增任何「存在后端」的控件，都要在回读函数里补一行。**

**2) 播放中左右拖波形会「抽搐」，两个播放头跟着分家 —— 两层原因，第二层更严重**

- **画面层**：旧 `followPlayheadTo` 在「播放头完全跑出画面」时**无条件**把视野拉回，
  与用户手上的拖动来回拉锯 = 抽搐。新规则：**手动操作期间完全不干预，播放头允许
  待在画面外**；只有**宿主自己**把播放头挪了（相邻两帧差 > `kHostJumpSec` = 0.25 秒）
  才跟随。⚠️ 缩放也会改变画面，所以 `zoomBy:` 里也要置 `userScrolling`。
- **音频层（真「分家」的根因，用户没意识到）**：旧 `withAudio()` 在**锁内**遍历几十万
  采样画波形，音频线程 `render()` 的 `try_lock` 抢不到就**整块丢音频**、`m_readPos`
  不推进 → 播放位置**永久**落后宿主。改成**不可变快照**：`AudioData` 收进
  `std::shared_ptr<const AudioData>`，读者 `audioSnapshot()` 只做一次引用计数 ++
  就拿到一份、之后随便遍历，**完全不碰音频线程的锁**；两把锁拆分
  （`m_mutex` 只护标量 / `m_snapMutex` 只护指针，锁序固定不可反向）。
  留了可见探针 `lockDropCount()`（音频线程原子自增、界面线程写日志），**正常恒为 0**。

**3) 安装器要先清旧版再装新版**

新增 `pkg/scripts/preinstall`（在 payload 复制**之前**跑，root 权限），清掉系统域三个
历史 bundle 名；Windows 侧加 Inno `[InstallDelete]` 段。`make_pkg.sh` 加了断言确认
preinstall 真的嵌进 pkg —— ⚠️ **脚本没有执行权限时 `pkgbuild` 不报错**（静默失败）。

**4) 底部署名改为可点击 → 打开作者 B 站主页**

地址收敛成 `gui.h` 的 `ap::kBrandHomeUrl`（三端共用一个常量，不再各写一份）：
- mac：`BrandLinkField : NSTextField`（`mouseDown` + `acceptsFirstMouse` 让
  **窗口未激活时第一次点击也生效** + `resetCursorRects` 手型光标）→ `NSWorkspace openURL`
- Win：STATIC 加 **`SS_NOTIFY`** 才会发 `STN_CLICKED`（不加就只是纯装饰）；
  光标必须用 `SetWindowSubclass` 改 —— **STATIC 自己会吞 `WM_SETCURSOR`**，
  在父窗口里判坐标是白写
- Linux：命中 `rBrand` → **双 fork** 拉 `xdg-open`。不用 `system()`（阻塞 GUI 线程），
  也不给宿主设 `SIGCHLD=SIG_IGN`（那会污染整个宿主进程的信号处置）

### 二、更正一条写错的帮助文案：「红绿两线重合即对齐」

用户实测推翻：**故意把偏移调错，两条线依然重合**。原因是设计使然，不是 bug ——
绿线画在 `谱面位置 + 偏移`，音频也被插件驱动到 `谱面位置 + 偏移`，所以
**改偏移时两条线一起平移、间距恒定**；它们只在音频线程没跟上（丢块 / 跟随失效）
时才分开 —— 那是**故障指示，不是对齐指示**。

**判断偏移调对了的唯一依据 = 网格小节线有没有落在波形上的鼓点/第一拍**
（网格锚点就是 `offset`，拖偏移 = 把整张谱面网格在波形上滑动）。

帮助层 6 段文案全部重写为「只讲怎么点、怎么拖」；mac 状态栏提示也从
「对齐：谱面播放头(x秒) → 音频 y 秒」改成「偏移 +z 秒（谱面位置 x 秒 = 音频 y 秒）」
（不再用被误用的「对齐」一词）。三端源码各留一条 ⚠️ 注释防复发。

### 三、帮助指南按「实际操作顺序」重排 + 快捷键逐端校准

顺序改为：① 装音频 → ② 填速度(BPM/拍号) → ③ 对拍子(拖网格) → ④ 波形区其它操作与
快捷键 → ⑤ 两条线说明 + 出问题点「回到播放头」 → 页脚「完全免费 + 求关注」。

⭐ 顺着「快捷键要按端写」，**查出并修掉一条 Linux 上的错指令**：旧帮助与 Linux 底部
提示行都写着「双击=全览」，但 Linux 的 X11 事件循环只把 Button4/5 当滚轮，
**没有任何双击判定**（mac 靠 `clickCount>=2`，Win 靠 `WM_LBUTTONDBLCLK`）。
已改成「整首全览点『全览』按钮」。

| | 改偏移 | 微调 | 平移 | 缩放 | 全览 |
|---|---|---|---|---|---|
| macOS | ⌘/⌥ 拖 | ⇧ | 滚轮 / 双指左右滑 | ⌘/⌥+滚轮 / 双指捏合 | **双击** |
| Windows | Ctrl/Alt 拖 | Shift | 滚轮 | Ctrl/Alt+滚轮 | **双击** |
| Linux | Ctrl/Alt 拖 | Shift | 滚轮 | Ctrl/Alt+滚轮 | ⚠️ **只有「全览」按钮** |

排版本着「改文案必须重算边界」：mac 21 行 × 行距 16（起点 y=38，末行底 ≈372 < 384）；
Win / Linux 各 22 行 × 行距 15（≈364 / ≈357 < 384）。三处都留了
「文案再加长就要同步此处」的 ⚠️ 注释。

### 四、Windows 端编译失败（被 CI 拦下）

```
gui_win.cpp(581,39): error C2065: 'brandSubclassProc': undeclared identifier
gui_win.cpp(581,58): error C2065: 'kBrandSubclassId': undeclared identifier
gui_win.cpp(616,46): error C2065: 'brandSubclassProc': undeclared identifier
gui_win.cpp(616,65): error C2065: 'kBrandSubclassId': undeclared identifier
```

`WinView::create()` / `destroy()` 在 581 / 616 行就要用这两个名字，但它们定义在
1444 / 1440 行。**C++ 里命名空间作用域的名字必须先声明后使用**（成员函数体不属于
「complete-class context」能救的范围）。文件顶部的前置声明区当时只补了
`waveProc` / `mainProc` / `helpProc` 三个窗口过程，**唯独漏了第四个：页脚子类过程**。

修法：补前置声明；`kBrandSubclassId` 是常量、没有「先声明后定义」的写法，
把定义整体搬上来，原位置留注释指向新位置。

> ⭐ **为什么值得单独记一笔**：这段「可点击页脚」代码一直只在本地、CI 从未见过它，
> 直到推送才被打回来。而 **mac / Linux 本机编译永远看不到这个问题** ——
> 「三端改动必须过 CI」不是形式主义。

### 五、验证与资产

- **本机** `build.sh` 8 项验证全绿；`gui_repro` 新增 6 条断言（rc=26~31）：
  重开编辑器后回读 BPM=96 / 拍号=12-8、播放中拖动视野连跑 10 次 tick 不被拉回、
  宿主跳转时画面跟到 6.45 秒、页脚是可点击链接且地址正确。
- **CI**：Windows + Linux 构建均全绿（含 Inno 安装器与 .deb/.rpm 打包）。
- 文案回归用**直接扫二进制**核对（Mach-O 里中文是 **UTF-16LE** 字面量，
  纯 `grep -a` 必然返回 0）：新文案全部命中且旧文案 0 命中。
- **资产**：`DaweiDrumScore-1.2.0-{Windows-Setup.exe, macOS.pkg, Linux-x86_64.tar.gz/.deb/.rpm}`

---

## 上一轮（2026-10-08）：三平台齐活 —— 新增 Linux 版

### 四、Linux 版 ✅ 已发布

- **同一份源码**，平台实现再开一个目录：`source/linux/*.cpp`（对 `source/*.mm` / `source/win/*.cpp`）
- 新写三件套（约 2000 行）：
  | 模块 | 文件 | 方案 |
  |---|---|---|
  | 解码 | `audiofile_linux.cpp` | **vendored dr_libs**（dr_wav / dr_mp3 / dr_flac，单文件库） |
  | 崩溃取证 | `crashguard_linux.cpp` | `sigaction(SA_SIGINFO)` + 架构寄存器现场 + `backtrace` |
  | 界面 | `gui_linux.cpp` | **X11 + Xft 全自绘**（宿主集成走 `X11EmbedWindowID`） |

- **为什么不用系统库解码**：libsndfile 1.1.0+ 才支持 MP3，而 Ubuntu 22.04 自带 1.0.31；
  各发行版版本不一，用户还得额外装运行时。改把三个单文件解码库编进插件 →
  **用户端零依赖**。代价：不支持 m4a/aac（Linux 上无对应免依赖解码器）。
- **为什么不用 GTK/zenity 选文件**：同理，避免给用户加依赖。改为自绘文件浏览器
  （`opendir`/`readdir`）+ **XDND 拖放**（`text/uri-list`）。
- **线程模型**：不依赖宿主 `IRunLoop`（Linux 宿主实现差异大），改
  `XInitThreads()` + 后台线程独占 X 事件与 30Hz 重绘；主线程只在 resize/destroy
  时短暂 `XLockDisplay`。
- 分发：`installer/linux/install.sh` 装到用户级 `~/.vst3`（免 sudo），CI 打成
  `DaweiDrumScore-Linux-x64.tar.gz`。
- **CI 第 4 轮全绿**（47s，11 步全过）。产物核对：
  - `ELF 64-bit LSB shared object, x86-64, ..., stripped`（277 KB 压缩包）
  - 导出符号齐全：`ModuleEntry` / `ModuleExit` / `GetPluginFactory`
  - `moduleinfo.json`：类名 `大伟鼓谱MuseScore音频播放器`（中文）、
    Vendor `Dawei DrumScore`（ASCII）、SDKVersion `VST 3.8.1`
  - bundle 结构 `Contents/x86_64-linux/DaweiDrumScore.so`
- 已上架 Release `v1.0.0`：`DaweiDrumScore-1.0.0-Linux-x64.tar.gz`
- 遗留：**未实机测试**（本机是 Mac）。

#### 后续补齐：glibc 兼容性 + .deb / .rpm（同日稍晚）

**发现的问题**：扒产物 ELF 符号表，最高需求是 `GLIBC_2.38`（元凶 `__isoc23_sscanf`）。
动态链接器在加载阶段就做版本校验，找不到符号直接拒绝加载 ——
**Ubuntu 22.04 / Debian 12（当前 stable）/ RHEL 9 的用户，插件在 MuseScore 里
连出现都不出现，而且没有任何提示。**

根因：CI 原来跑在 `ubuntu-latest`（＝24.04，glibc 2.39 + GCC 13）。GCC 13 配
`-std=gnu++17` 会隐含 `_GNU_SOURCE` → 打开 `_ISOC23_SOURCE` → glibc 头文件把
`sscanf` 宏重定向到 `__isoc23_sscanf`。SDK 的 header 里有 sscanf 调用，
于是这个符号被编进了产物。

> 关键认知：**构建底座的 glibc 版本 = 产物的 glibc 下限**，没法用编译选项"降级"
> （你用的就是那个版本的头文件与符号版本）。业界标准做法是钉一个老底座 ——
> Python 生态的 manylinux 就是这么干的。

修法：CI 改用 `container: ubuntu:20.04` → glibc 2.31 / GCC 9，
覆盖 **Ubuntu 20.04+ / Debian 11+ / RHEL 9+**。

| 配套踩到的坑 | 修法 |
|---|---|
| 20.04 自带 CMake 3.16 < SDK 要求的 3.25 | 下载官方 CMake 3.31.6 预编译包（静态链接，只需 glibc 2.17+） |
| 20.04 的 freetype 开发包叫 `libfreetype6-dev` | 换包名（较新发行版才叫 `libfreetype-dev`） |
| SDK 的 `module_linux.cpp` 用了 `<filesystem>`，而 GCC 9/10 的 libstdc++ 尚未把它的实现并进主库 | CMakeLists 里按编译器版本补链 `libstdc++fs` |
| 基础镜像里没有 git / curl | 第一个步骤先 apt 装 |
| `archive.ubuntu.com` 抽风：主仓库 InRelease 拉不到（updates/backports 却成功）→ 索引不全 → 依赖解析崩 | 换 Azure 官方镜像 + `Acquire::Retries=5` + update 失败自动回退原源 |

**新增 CI 断言**：产物的 `GLIBC_*` 需求必须 ≤ 2.31。它防的是"底座悄悄变新"
（哪天把 `container:` 那行去掉、或 Actions 把 `ubuntu-latest` 升到更新的镜像），
届时会明确报错，而不是在用户环境下无声失败。

**新增 .deb / .rpm**：`installer/linux/make_packages.sh` 一键出三种包，
本地可跑、CI 直接调用（缺 dpkg-deb / rpmbuild 时自动跳过对应格式）。

| 包 | 装到 | 说明 |
|---|---|---|
| `.tar.gz` | 用户级 `~/.vst3` | 免 sudo；**Arch / Gentoo / NixOS 这类没有 deb/rpm 的发行版也通吃** |
| `.deb` | 系统级 `/usr/lib/vst3` | `Depends: libc6 (>= 2.31), libstdc++6, libgcc-s1, libx11-6, libxft2`；带 postinst / prerm 提示 |
| `.rpm` | 系统级 `/usr/lib/vst3` | **不手写 Requires**，交给 rpmbuild 自动生成（读 ELF 的 NEEDED 与 GLIBC 符号版本来定），避开 Fedora(`libX11`) 与 openSUSE(`libX11-6`) 包名不一致的坑 |

**CI 结果**：全绿（1m20s，17 步）。产物核对（下载后独立解包验证，不只看 CI 日志）：

- 三个包结构标准 —— tar.gz（bundle + install.sh）、.deb（`debian-binary` + `control.tar.xz` + `data.tar.xz`）、
  .rpm（lead 魔数 `edabeedb` + gzip cpio payload）
- `.so` 的最高符号需求 **GLIBC_2.17 / GLIBCXX_3.4.21** —— 比声明的 2.31 还低，覆盖面留有富余
- `.deb` 内 `.so` 导出 `ModuleEntry` / `ModuleExit` / `GetPluginFactory` 三项齐全（共 344 个全局符号）
- `.rpm` 的 Requires 由 rpmbuild 自动生成（`libc.so.6(GLIBC_2.x)(64bit)`、`libstdc++.so.6(GLIBCXX_3.4.x)(64bit)` 等规范形式）
- Release `v1.0.0` 已换为三个 Linux 资产（旧的单个 `-Linux-x64.tar.gz` 已删除）

#### 本轮踩的坑（Linux CI 前两轮失败）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | 配置阶段报 `Package 'xcb-util' not found` | CMakeLists 里写的 SDK 开关名 `SMTG_ADD_VSTGUI` **在 SDK 3.8.1 中不存在**，CMake 对未定义变量不报错、只静默失效 → `vstgui4` 仍被 `add_subdirectory`，它强制探测 xcb-util / xcb-cursor / wayland / cairo / pango 一大堆 | 改真实开关名 `SMTG_ENABLE_VSTGUI_SUPPORT=OFF`；工作流另加**守卫断言**，日后 SDK 升级若再引入 vstgui 会明确报错 |
| 2 | 编译到 100% 报 `'uintptr_t' does not name a type` | `crashguard_linux.cpp` 用 `uintptr_t` 打印寄存器现场却没 `#include <cstdint>`（macOS 上被其它头间接带出，Linux/GCC 不成立） | 补 `<cstdint>` / `<cstddef>` |
| 3 | 表面「链接失败」，`.so` 被删掉 | 链接其实**成功**了。报错来自 POST_BUILD 的 `moduleinfotool`：它 `dlopen` 产物要求导出 `ModuleEntry`/`ModuleExit`/`GetPluginFactory`，而我们只有后者（`bundleEntry/bundleExit` 是 macOS 的名字，Linux 不认）。更深一层：`smtg_target_add_library_main()` 只在 `public_sdk_SOURCE_DIR` 可见时才加入口文件，而该变量在 SDK 自己的 directory scope，**传不回父作用域** → 三平台入口文件其实一直没被编译，全靠自己写 | `aplaysdk.cpp` 按 `#if SMTG_OS_LINUX` 补 `ModuleEntry`（装崩溃取证）/ `ModuleExit` |

> 教训：**改 CMake 开关后必须验证它真的生效**。「设了个不存在的变量」是最阴的一类错——
> 不报错、不警告，症状出现在几千行之外的第三方依赖探测里。
>
> 教训二：**别用 CMake 的报错位置推断真实故障点**。第 3 轮 gmake 明确指着 `.so` 说
> `Error 1` 还把它删了，实际是 POST_BUILD 脚本失败。看日志要往下多找几行。

#### 本机验证 Linux 代码的办法（本轮建立）

macOS 上没有 X11 头文件，但可以直接**从 Ubuntu 镜像拉 dev 包解出头文件树**，
再用 `clang++ -fsyntax-only -D__linux__` 检查：

```bash
# 1) 从 dists/noble/main/binary-amd64/Packages.xz 解析出精确 deb 路径
# 2) 下载并两步解包（.deb = ar 包着 data.tar.xz）
#    libx11-dev / libxft-dev / libxrender-dev / libfontconfig-dev /
#    libfreetype-dev / xorgproto(x11proto-dev) / libxau-dev / libxdmcp-dev / ...
# 3) 检查
clang++ -std=c++17 -fsyntax-only -D__linux__ \
  -I source -I "$SDK" \
  -I /tmp/linuxinc/usr/include -I /tmp/linuxinc/usr/include/freetype2 \
  source/linux/gui_linux.cpp
```

这套办法本轮**真的抓到了一个产品代码 bug**（Xft 的 `Visual*` 不能传 `const Visual*`），
比等 CI 迭代快得多。结果：`gui_linux.cpp` / `audiofile_linux.cpp` 0 错误；
`crashguard_linux.cpp` 的 x86_64 / aarch64 两个分支按 glibc 布局核对也是 0 错误。

### 五、三平台对照

| | macOS | Windows | Linux |
|---|---|---|---|
| 状态 | ✅ 已发布（PKG） | ✅ 已发布（Setup.exe），未实机测试 | ✅ 已发布（tar.gz + .deb + .rpm），未实机测试 |
| 本地构建 | `./build.sh`（6 项验证全绿） | 不需要 | 可 `cmake`，但本机无 X11 头 |
| 云端构建 | 不需要 | GitHub Actions（全绿） | GitHub Actions（全绿，**ubuntu:20.04 容器**） |
| **glibc 下限** | —（系统自带） | —（系统自带） | **2.31**（Ubuntu 20.04+ / Debian 11+ / RHEL 9+） |
| 解码 | AVFoundation | Media Foundation | vendored dr_libs（**零依赖**） |
| 支持格式 | mp3/wav/m4a/aac/flac… | 同 macOS | **mp3/wav/flac**（无 m4a/aac） |
| 界面 | AppKit | Win32 | X11 + Xft |
| 崩溃取证 | signal + backtrace | SEH + DbgHelp | signal + 寄存器现场 |
| 模块入口符号 | `bundleEntry`/`bundleExit` | `GetPluginFactory`（`InitDll`/`ExitDll` 可选） | `ModuleEntry`/`ModuleExit`/`GetPluginFactory` |
| 安装位置 | 系统域 `/Library/…/VST3` | `C:\Program Files\Common Files\VST3` | tar.gz→ `~/.vst3`（免 sudo）；deb/rpm→ `/usr/lib/vst3` |
| 放行 | Gatekeeper（右键打开） | SmartScreen（更多信息→仍要运行） | 无（未签名也不拦） |
| Release 资产 | `…-macOS.pkg` | `…-Windows-Setup.exe` | `…-Linux-x86_64.{tar.gz,deb,rpm}` |

---

## 上一轮（2026-10-08）：安装程序（Mac PKG + Win 安装器）+ 跨平台 + 开源上线

### 一、macOS 安装包 ✅ 已发布
- `./make_pkg.sh` → `dist/*.pkg`（约 338 KB）→ 已上传 Release 附件
- 装到**系统域** `/Library/Audio/Plug-Ins/VST3/`：双击 → 输密码 → 完成（VST 插件标准做法）
- 含欢迎页 / 使用说明 / 许可 / 完成页（B 站引流在欢迎页与页脚）
- postinstall：自动清理旧命名残留（APLAY.vst3 / DaweiDrumScore.vst3）+ 去除下载隔离属性
- **验证**：解包比对，包内二进制与源 bundle **md5 逐字节一致**；Info.plist 字段正确
- 未签名 → 首开被 Gatekeeper 拦 → 随包附 `dist/安装说明-请先读.txt`（右键 → 打开）

### 二、Windows 版 ✅ 已发布
- 架构：**同一份源码**，平台实现分目录（`source/*.mm` 对 `source/win/*.cpp`）
- 直接复用：`player.cpp`、`aplaysdk.cpp`（零 ObjC）、`PlugView::Backend` 纯虚接口
- 新写：Media Foundation 解码、SEH + DbgHelp 崩溃守卫、Win32 自绘界面（约 2600 行）
- **CI 编译成功**（第 4/5 轮全绿）：bundle 结构、VST3 导出符号、模块元数据、中文字符串均验证正确
- `installer/windows/setup.iss`（Inno Setup）→ `Setup.exe` 双击即装到 `C:\Program Files\Common Files\VST3\`
- 遗留：**未实机测试**（本机是 Mac）；安装器向导暂用英文；未签名 → SmartScreen 手动放行

### 三、开源上线
- 仓库：https://github.com/780566922/dawei-drumscore （public / C++ / GPL-3.0 / 分支 main）
- Release `v1.0.0`：`DaweiDrumScore-1.0.0-macOS.pkg` + `DaweiDrumScore-1.0.0-Windows-Setup.exe`
- README（安装 / 构建 / FAQ / 许可）+ LICENSE（GPL-3.0，因 VST3 SDK 双许可）

### 关键结论（当轮快照，此时尚未做 Linux 版）
| | macOS | Windows |
|---|---|---|
| 状态 | ✅ 已发布（PKG） | ✅ 已发布（Setup.exe），未实机测试 |
| 本地构建 | `./build.sh`（6 项验证全绿） | 不需要 |
| 云端构建 | 不需要 | GitHub Actions（全绿） |
| 解码 | AVFoundation | Media Foundation |
| 界面 | AppKit | Win32 |
| 崩溃取证 | signal + backtrace | SEH + DbgHelp |
| 放行 | Gatekeeper（右键打开） | SmartScreen（更多信息→仍要运行） |

---

## 上一轮：查清菜单显示名的真正来源 = bundle 文件名（2026-10-08 00:10）

**用户反馈**：菜单两级还都是英文。并复述层级 —— `VST` → 一级菜单（应是英文）→
二级菜单（插件本体，可以是中文）。

**取证（决定性）**：MuseScore 的插件缓存
`~/Library/Application Support/MuseScore/MuseScore4/known_audio_plugins.json`
里我们插件的条目是：

```json
"id": "DaweiDrumScore",
"vendor": "Dawei DrumScore"
```

当时 `ci.name` = `Dawei DrumScore Player`、bundle = `DaweiDrumScore.vst3`。
`id` 等于 **bundle 文件名**，**不等于** VST3 类名 → 菜单第二级显示的是
**bundle 文件名**，不是 `ci.name`。

再用排除法二次确认：更早那版 bundle 为中文、而 bundle 内**可执行名是 ASCII**，
当时菜单第二级却是中文 —— 若 `id` 取自可执行名就会是英文，所以 `id` 只能来自
**bundle 文件名**。

**结论 —— MuseScore 菜单 = `VST` → `厂商名` → `bundle 文件名`：**

| 菜单层级 | 名字来源 | 策略 |
|---|---|---|
| 第一级：厂商名 | `ci.vendor` / `PFactoryInfo.vendor` | **必须纯 ASCII**（中文在该通道会乱码） |
| 第二级：插件名 | **bundle 文件名**（`.vst3` 去掉扩展名） | **用中文命名 bundle 即显示中文** |

**最终命名配置**：

| 位置 | 内容 |
|---|---|
| bundle 文件名（= 菜单第二级显示） | `大伟鼓谱MuseScore音频播放器.vst3`（**中文**） |
| 厂商名（= 菜单第一级显示） | `Dawei DrumScore`（ASCII） |
| bundle 内可执行名 | `DaweiDrumScore`（ASCII，不参与显示，避免路径编码问题） |
| `CFBundleName` | 大伟鼓谱MuseScore音频播放器（中文） |
| `ci.name` | 大伟鼓谱MuseScore音频播放器（中文，其他宿主可能用它） |
| 窗口内标题 / 底部页脚 | `大伟鼓谱 · MuseScore 音频播放器` / B 站署名（中文，AppKit 自绘） |

**改动**：`build.sh`（bundle 名改回中文、CFBundleName 改中文、注释更正）、
`validate.cpp`（默认路径）、`安装.command`（改中文名 + 清理旧 ASCII 版）、`使用说明.md`；
并清理 MuseScore 缓存中指向已删文件的旧条目（备份 `known_audio_plugins.json.pre_cleanup`）。

**验证**：构建 6 项全绿（接口 72 / 音频 9 / 并发 478 万块 / 多格式 3 / 界面 3 路径 /
播放核心 42）。已安装 `~/Library/Audio/Plug-Ins/VST3/大伟鼓谱MuseScore音频播放器.vst3`，
实测该产物：`ci.name` = 中文、vendor = 纯 ASCII、CFBundleName = 中文、
CFBundleExecutable = `DaweiDrumScore`。

B 站署名仍在底部页脚：`♪ B 站「大伟鼓谱」· 欢迎关注，鼓谱 / 教学 / 伴奏持续更新`。

---

## 上一轮：插件名恢复中文（仅厂商名 ASCII）

---

## 上一轮：B站署名挪到底部

---

## 早前一轮：四项体验修复

### 用户反馈的四个问题

1. **触摸板双指缩放失效**
2. **偶发从头发播放**（宿主体点某小节后有时又从头放）
3. **小节号与谱面不一致**
4. **拍号分母没显示**（12 拍无法区分是 12/4 还是 12/8）

## 修复

### 1. 触摸板双指缩放（source/gui.mm）

根因：`magnifyWithEvent` 沿响应链传递，视图没声明 `acceptsFirstResponder`，
双指捏合事件被宿主窗口/上层视图吞掉，导致缩放失效。

- `WaveformView` 加 `acceptsFirstResponder` 返回 YES
- `mouseDown` 里 `makeFirstResponder:self`
- `scrollWheel` 里加兜底：无修饰键且 `|deltaY| > 1.5×|deltaX|` 时也按缩放处理
  （触控板捏合在 magnify 未被识别时降级成 scrollWheel 的 deltaY）

### 2. 偶发从头发播放（source/audiofile.h）

根因：`seekTo` 设 `m_readPos` 后**没同步 `m_appliedOffset`**。下一次 `render`
里 `restartFromOffsetLocked()` 检测到 offset「未同步/有浮点差异」→ 把读位置
重置回偏移起点 → 从头播放。

- `seekTo` 里补上 `m_appliedOffset = startOffsetSec` 并清 `m_offsetDirty`，
  这样 seek 之后紧接着的 render 不会再重置读位置。

### 3. 小节号对齐（恢复手动 BPM）

用户确认「恢复手动 BPM 输入」。根因：MuseScore 不提供 tempo，之前用固定 120 BPM
画网格，小节号是「120BPM 参考拍号」，无法等于谱面真实小节号。

- 恢复 BPM 输入框 + 步进器 + 速度来源标签
- `gridInfoFor` 改为：手动 BPM 优先，否则宿主 tempo，再退回 120
- 用户填对 BPM 后，网格小节线就能对齐谱面真实小节

### 4. 拍号分母（N/M 形式）

根因：拍号只有分子「N 拍/小节」，隐含假设「四分音符为一拍」，无法表达 12/8
这类「八分音符为一拍」的拍号。

- Backend 接口加 `setGridBeatDenominator` / `gridBeatDenominator`（M ∈ {2,4,8,16}）
- 拍号下拉改成 N/M 形式（2/4、3/4、4/4、5/4、6/8、7/8、9/8、12/8）
- `gridInfoFor` 每拍时长 = `(60/bpm) × (4/M)`，12/8 的每拍时长是四分音符的一半
- 状态存续加 `kIdGridBeatDenominator` 字段（setState/getState）

## 验证结果

```
✓ 接口生命周期        70 项
✓ 真实音频输出         9 项
✓ 并发安全             731 万块渲染
✓ 多格式解码           3 个真实文件，无异常
✓ 界面生命周期         load / drag / grid 三条路径
✓ 播放核心单元测试     42 项
```

（验证器 gui_repro.mm 同步：恢复 stepper 断言、拍号分母测试改 12/8）

插件已安装到 `~/Library/Audio/Plug-Ins/VST3/APLAY.vst3`。

## 需用户实测确认

⌘Q 重启 MuseScore 后重点测：
1. 触控板双指捏合缩放是否恢复
2. 点谱面小节后是否还会偶发从头发播放
3. 填对 BPM + 拍号后，网格小节号是否和谱面对齐

---

## v1.0.1 发版（2026-10-08）

Windows 版由用户**上机实测通过**后发版。

### 本轮修的两个 Windows Bug

| # | 现象 | 根因 | 提交 |
|---|---|---|---|
| 1 | 波形区一片空白（音频正常） | 双缓冲收尾时 `BitBlt` 写在 `SelectObject(dc, oldBmp)` **之后** —— BitBlt 读的是 DC 当前选中的位图，摘掉后挂回的是 `CreateCompatibleDC` 自带的 1×1 单色占位图 → 拷出一整块纯色 | `9f340f1` |
| 2 | 高分屏上界面太小 | 宿主是 Per-Monitor DPI Aware，窗口坐标是**物理像素**，写死的 340×384 在 150%/200% 下就真的只画那么点物理像素 | `d1cbc6a` |

高 DPI 修法要点：DPI 探测三级降级（`GetDpiForWindow` → `GetDpiForMonitor` → `GetDeviceCaps`，
全部 `GetProcAddress` 动态取，不新增链接依赖），系数夹到 [1,3] 并**量化到 0.25 档**；
布局继续写 96 DPI 设计值、由 `px()` 统一换算；⭐ `secToX()` / `xToSec()` 的基准必须改成
缩放后的 `m_waveW`，否则 DPI≠96 时点击定位整体偏移。

### 三平台检查结论

| 平台 | 构建 | 本次是否改动 | HiDPI 情况 |
|---|---|---|---|
| macOS | 本机 `./build.sh` 6 项验证全绿（universal 双架构、72 项接口、42 项播放核心） | 未改布局；`gui.h` 的新增成员不影响 AppKit 路径 | ✅ 天然安全：AppKit 用 point 坐标，Retina 由系统自动 ×2 渲染 |
| Windows | CI 全绿 + 用户上机实测通过 | 修波形 + 加 DPI 缩放 | ✅ 已修 |
| Linux | CI 全绿 | 未改 | ⚠️ **已知限制**：X11 自绘窗口尺寸是物理像素，桌面缩放 200% 时界面偏小 |

> Linux 的 HiDPI 未在本轮一并处理，理由：`gui_linux.cpp` 的布局是 6 个尺寸常量 +
> 20 个 `const Rect` 控件矩形 + 约 50 处字面量引用，要全量缩放得改 30+ 处引用点，
> 而 **Linux 版 GUI 从未实机验证过**（本机是 Mac，CI 不渲染 GUI）—— 改动风险大于收益。
> 要补的话，最小改动方案是把这些常量/矩形改成运行时按 `Xft.dpi` 重算的全局量
> （引用处不用动），默认路径 `Xft.dpi=96` → 系数 1.0，行为不变。

### Release 资产（v1.0.1，资产名全 ASCII）

- `DaweiDrumScore-1.0.1-Windows-Setup.exe`
- `DaweiDrumScore-1.0.1-macOS.pkg`
- `DaweiDrumScore-1.0.1-Linux-x86_64.tar.gz` / `.deb` / `.rpm`

版本号散落在 **9 处**，升级时必须同步：CMakeLists.txt（`project VERSION`，Linux CI 从此 grep 提取）、
installer/windows/setup.iss、make_pkg.sh、build.sh 的 CFBundle 两行、pkg/distribution.xml、
source/plugin.mm、source/aplaysdk.cpp 两处。

---

## v1.0.2 发布（2026-10-08）

修复一次宿主闪退：**关掉谱子 → 导入另一个工程 → 打开插件 → 宿主闪退**（macOS 上实测）。

### 定位过程（这次是「日志 → 假设 → 复现 → 修复」一条龙）

1. **崩溃日志给了关键信息**。插件自己的崩溃取证把宿主闪退现场写进了
   `~/Library/Logs/DaweiDrumScore.log`，栈整个落在**宿主自己的模块加载路径**上：
   ```
   VstModulesRepository::addPluginModule
     → VstPluginMetaReader::readMeta
       → VST3::Hosting::Module::create
         → Module::getModuleInfoPath      ← 崩在这里（相邻帧重复 = 栈已损坏）
   ```
   同时日志的时间线显示：`Processor 析构`（关文档）之后，每次重扫/打开编辑器
   都在消耗点什么，最终某一轮崩掉 —— 是「同一进程内**反复取工厂**」才触发。

2. **读 SDK 源码确认机制**。`CPluginFactory` 由 `FUNKNOWN_CTOR` 把引用计数初始化
   为 1，`release()` 归零时 `delete this`；而 SDK 官方宏 `END_FACTORY` 写着
   `else gPluginFactory->addRef();` —— 因为**宿主每次调用 `GetPluginFactory()`
   都会配对一次 `release()`**。我们漏了这一句。

3. **写复现程序确证**（`factory_lifecycle_test.cpp`，dlopen bundle 后反复索取/释放）：
   ```
   第 1 轮: release 后计数=1     ← new=1 → 我们 addRef=2 → 宿主 release=1
   第 2 轮: release 后计数=0     ← 不补引用仍=1 → 宿主 release=0 → delete this
   （第 3 轮直接崩溃，连输出都没有）
   ```

### 根因

```cpp
// 旧代码：只在首次 addRef，之后再也不补
static APlayFactory* gFactory = nullptr;
if (!gFactory) { gFactory = new APlayFactory (); gFactory->addRef (); }
return gFactory;              // ← 第 3 次索取返回的是【已释放内存】
```

加剧因素：SDK 的 `~CPluginFactory` 只把**它自己的**全局 `gPluginFactory` 置空来兜底，
管不到我们在函数内 static 缓存的指针 → 那个指针**永久悬垂**。

### 修法（双保险）

- `GetPluginFactory()` 对齐官方宏：`else` 分支补 `addRef()`；
- `APlayFactory` 重写 `addRef`/`release`：工厂是**进程级常驻单例**，计数归零只钉回 1，
  **绝不 delete**。模块级资源本就该活到进程退出 —— 无论宿主引用计数多不规范都不可能再悬垂。

### 三端影响

`aplaysdk.cpp` 是 **macOS / Windows / Linux 共用源码**，同一个 bug 三端都在，本次一并修复。
（macOS 的 `source/plugin.mm` 其实**不参与构建**，真正的工厂与处理器都在 `aplaysdk.cpp`。）

### 回归防线

`factory_lifecycle_test.cpp` 接入 `build.sh` 成为**验证 7**：
同一进程内反复「索取 → 使用 → 释放」20 轮 + 故意过度释放 + 连续索取 100 轮。
修复前第 3 轮必崩，修复后全绿。

### 构建健壮性顺带修复

`make_pkg.sh` 里 `rm -rf` 在**外置盘**上会被沙箱以 `Operation not permitted` 拒绝，
而脚本开头是 `set -e` → 打包中途中断。改为容忍该失败（内容后续都会整体重建/覆盖）；
展开校验目录名改用带 PID 的唯一名，避免残留目录让 `pkgutil --expand` 误报「无法展开」。

### 资产

`DaweiDrumScore-1.0.2-{Windows-Setup.exe, macOS.pkg, Linux-x86_64.tar.gz/.deb/.rpm}`

---

## v1.1.0 发布（2026-10-08）

新增 **Ogg Vorbis 解码**，三端可用。

### 为什么 OGG 需要单独写一节

插件其它格式都是「调系统解码器」：macOS 走 AVFoundation/CoreAudio，Windows 走
Media Foundation，Linux 走自家 vendored 的 dr_libs。**OGG 是唯一三端系统框架都填不上的格式**：

| 平台 | 结果 | 原因 |
|---|---|---|
| macOS | ❌ 系统没有 | CoreAudio 从未提供过 Vorbis 解码器 |
| Windows | ❌ 系统没有 | Media Foundation 内置解码器里没有；微软把它放在商店的可选包「Web Media Extensions」里，默认不装 |
| Linux | ❌ 库里没有 | dr_libs 只做 WAV / MP3 / FLAC |

> ⚠️ 容易混的一点：`dr_flac.h` 里到处是 "Ogg" 字样，但那指的是 **Ogg 封装的 FLAC**，
> 与 Ogg Vorbis 是两回事（源码里还主动 `#define DR_FLAC_NO_OGG` 关掉了它）。

### 实现

新增平台无关的共享源码 `source/ogg_vorbis.{h,cpp}`，被三端一起编译
（与 `player.cpp` / `aplaysdk.cpp` 同一层级）。解码器是 **stb_vorbis v1.22**
（公共领域 / MIT-0，`source/third_party/stb_vorbis.c`，193 KB 单文件），
按 C++ 编 —— stb 自带 `__cplusplus` 外部 "C" 保护，官方支持这么用。
它的唯一实现单元在 `ogg_vorbis.cpp` 里 `#include` 进来，避免多 TU 重复编译。

两处刻意「不用库自带能力」的地方：

1. **文件读取自己写**。`stb_vorbis_open_filename()` 内部是 `fopen(const char*)`，
   Windows 上只认 ANSI 代码页 → 中文路径打不开。现有三端解码器拿到的都是 UTF-8 路径，
   所以自己读整个文件（Windows 走 UTF-8 → UTF-16 + `_wfopen`），再交给
   `stb_vorbis_open_memory()`。
2. **声道合并自己做**。stb 在「要的声道数比源多」时会给多余声道**补 0** 而不是复制
   （见 `stb_vorbis.c` 的 `stb_vorbis_get_samples_float_interleaved`：`for (; i < channels; ++i) *buffer++ = 0;`）
   —— mono 文件若直接按 2 声道索取，**右声道就是死的**。所以始终按源声道数取，
   再自己按「mono → 左右复制；多声道 → 取前两路」合并，与其它格式行为一致。

错误提示做了区分：同样是 `.ogg` 后缀，里面装的可能是 Opus 或 FLAC，会被识别出来并
明确提示（"这是 Ogg Opus 文件"），而不是含糊地报"解码失败"；非 Ogg 内容改后缀也挡得住
（校验 `OggS` 文件头）。

**仍然不支持 Ogg Opus** —— 那是另一套编码，需要另一个解码器。

### 接入点（六处，容易漏）

- `source/audiofile.mm` / `source/win/audiofile_win.cpp` / `source/linux/audiofile_linux.cpp`：
  在扩展名分派处转到 `ap::decodeOggVorbis`，并把 `.ogg` / `.oga` 加进各自的 `kExt[]`
  （Linux 还要在 `sniffFormat` 里认 `OggS` 魔数）
- 三端 UI：macOS `gui.mm` 两处（`allowedFileTypes` + 拖放白名单）+ 提示文案；
  Windows `gui_win.cpp` 两处（文件对话框 filter + `m_fmtLabel`）；
  Linux `gui_linux.cpp` 两处（`isAudioFile()` + 状态行文案）
- 构建：`CMakeLists.txt`（`AP_SOURCES`）+ `build.sh`（逐架构编译 + lipo + 链接）

### 回归防线

新增 `testdata/`（3 个素材共 12.7 KB，入库，不依赖机器上装没装 ffmpeg）与
`ogg_vorbis_test.cpp`，接入 `build.sh` 成为**验证 8**，27 项，重点守三条：
立体声左右必须是两份**不同**数据、单声道必须**复制成两路**（不能补 0）、
Opus 必须**点名提示**。

### 顺带修掉一个构建漏洞

「验证 4：多格式解码」探针的编译是 `$CXX ... 2>&1 | tail -3`，**不检查退出码**。
一旦链不上，`$OUT/decode_probe` 会执行失败（或跑上一次的旧二进制），
而判定逻辑只 grep 日志里有没有 `NSException` / `ok=0` → 什么都没找到就算通过，
于是「明明没编出来却报验证通过」。本次新增源文件正好踩到这个坑（日志里 0 个真实文件）。
已改为编译失败即中断。

### 资产

`DaweiDrumScore-1.1.0-{Windows-Setup.exe, macOS.pkg, Linux-x86_64.tar.gz/.deb/.rpm}`

### 体积

各平台二进制约 +100 KB（stb_vorbis 的代码），换来三端一致的 OGG 支持。
