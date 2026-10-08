# 移交专项：Windows 版波形区一片空白（音频正常）

> 交接给下一台机器的接手人（人或 AI）。
> 开发机是 macOS，无法复现 Windows 现象，所以本文给出**根因判断 + 精确补丁 + 验证方法**，以及一份备选假设树以防主判断不成立。

---

## ✅ 状态更新（2026-10-08 10:0x）：补丁**已落地并编译通过**

- 提交 `9f340f1` `fix(win): 修正波形区空白——BitBlt 在 SelectObject(oldBmp) 之后导致整块不上屏`
- 两个 CI（Windows / Linux）全绿，Windows 安装包已产出：
  - `dist/DaweiDrumScore-Setup-1.0.0-waveform-fix.exe`（安装器，双击即装）
  - `dist/DaweiDrumScore.vst3/`（绿色版，直接拷进 `C:\Program Files\Common Files\VST3\`）
- **下方 §1 的判断已被采纳为正式补丁**；§2 之后的备选假设树保留作兜底，
  若上机后波形**仍然空白**再按它逐条排查。

改动要点（比原计划多修了一处）：
1. `BitBlt` 挪到 `SelectObject(dc, oldBmp)` **之前**（本文 §1 的根因）。
2. 离屏位图**恒按 `kWaveW × kWaveH`** 建 —— `secToX()/xToSec()` 的基准是 `kWaveW`，
   原先按 `rcPaint` 宽高建位图，局部重绘时会整块错位裁切。上屏只 Blt `rcPaint` 与波形区的交集。

---

## 0. 现场事实（已由用户实测确认）

| 观察项 | 实测结果 | 由此能推出什么 |
|---|---|---|
| 音频播放 | **正常出声** | 解码没问题，`AudioData::samples` 有真实数据 |
| 底部文件名 | **显示了文件名** | `Backend::currentPath()` 非空 → 文件确实载入了处理器 |
| 状态灯 | **「● 跟随宿主播放」** | `Backend::hasAudio()` 为 **true**，且宿主播放位正常 |
| 波形区 | **一片空白，没有网格线、没有绿/红播放头、没有波形** | `paint()` 里的**内容绘制全部没落到屏幕上**，但底色是干净的 |
| 面板其余部分（按钮/标签/下拉框） | 正常 | 那些是真正的 Win32 控件，系统自己画，与我们无关 |

四个字概括：**绘制代码跑了，像素没上屏。**

---

## 1. 根因判断（高置信）：离屏位图「先摘下来、后 BitBlt」，位图取错了

**位置：`source/win/gui_win.cpp` → `WinView::paint()` 尾部，第 597-600 行**

```cpp
    ::SelectObject (dc, oldBmp);                                  // ← 先把我们的位图摘下来
    ::BitBlt (target, rc.left, rc.top, W, H, dc, 0, 0, SRCCOPY);  // ← 再从 dc 里拷
    ::DeleteObject (bmp);
    ::DeleteDC (dc);
```

### 为什么这是错的

GDI 的 `BitBlt` **读的是 DC 当前选中的那张位图**。

- `::CreateCompatibleDC(target)` 造出来的内存 DC，默认挂着一张 **1×1 单色** 的占位位图。
- `::SelectObject(dc, bmp)` 把我们的 320×118 位图挂上去 → 后面所有 `FillRect` / `LineTo` / `TextOut` 都画进这张。
- **`::SelectObject(dc, oldBmp)` 一执行，DC 的当前位图就变回那张 1×1 单色占位图了。**
- 此时 `BitBlt(..., dc, 0, 0, ...)` 拷的是那张 1×1 单色位图 → 目标区被整体铺成**一个纯色**。

于是：底下的 `FillRect(dc, &full, bg)` 明明执行了，波形/网格/播放头明明也画进了 `bmp`，但 `BitBlt` 拷的不是 `bmp`，**屏幕上只剩一块干净的纯色**——与实测现象完全吻合。

> 现象里的「干净的纯色」本身就是证据：`WM_ERASEBKGND` 返回 1（第 963-964 行），类背景刷根本不参与擦除。如果 `BitBlt` 压根没执行或失败了，这块区域应该显示的是窗口移动后残留的脏像素/桌面残影，而不是一块均匀的纯色。**均匀纯色 = `BitBlt` 成功了，但源位图是那张 1×1。**

### 为什么一直没被发现

Windows 版**从未在真实机器上跑过**（开发机是 Mac）。CI（`.github/workflows/build-windows.yml`）只检查产物结构、导出符号、`moduleinfo.json`，**从不渲染 GUI**。这条 bug 属于「编译能过、CI 能过、一上机就露」的典型。

### 修复（就地交换两行顺序）

```cpp
    // 必须先 BitBlt，再从 DC 上摘下位图 ——
    // BitBlt 读的是 DC「当前选中」的位图，先摘就会拷到 CreateCompatibleDC
    // 自带的那张 1×1 单色占位图，结果屏幕上只剩一块纯色（波形全没了）。
    ::BitBlt (target, rc.left, rc.top, W, H, dc, 0, 0, SRCCOPY);
    ::SelectObject (dc, oldBmp);
    ::DeleteObject (bmp);
    ::DeleteDC (dc);
```

**改动量：3 行重排，零逻辑变更。**

---

## 2. 30 秒零成本确认（建议先做，再改代码）

在 Windows 上截一张插件面板的图，用画图/取色器**读波形区那块的 RGB**：

| 读到的颜色 | 结论 |
|---|---|
| **纯黑 `#000000`** | ✅ 主判断成立 —— `BitBlt` 拷的是 1×1 单色占位图。直接套第 1 节的补丁。 |
| **`#18181C`**（RGB 24,24,28） | ❌ 主判断不成立 —— 说明 `BitBlt` 拷的确实是我们的位图，底色被正确画上去了。此时转去看第 5 节的备选假设。 |

> `#18181C` 是 `paint()` 第 446 行 `CreateSolidBrush(RGB(24,24,28))` 的底色。
> 两个颜色肉眼几乎分不出，**必须取色，不要靠眼睛判断**。

---

## 3. 波形数据链路（改完之后如果还不显，照这条链往下查）

每一跳都标了 file:line，便于打断点或加日志。

```
用户拖入 / 选择音频文件
  └─ source/win/gui_win.cpp:776 onDropFiles / :794 openFileDialog
       └─ Backend::loadFile(path)
            └─ source/aplaysdk.cpp:79  StableBackend::loadFile → currentProcessor()->loadFile
                 └─ APlayProcessor::loadFile → ap::decodeAudioFile
                      └─ source/win/audiofile_win.cpp:159  Media Foundation 解码
                           ├─ :204 请求 float32 输出（失败回退 16-bit PCM，:210）
                           ├─ :251 out.sampleRate = 实际采样率
                           ├─ :252 out.numChannels = 2     ← 恒为 2，交错双声道
                           └─ :103 appendSamples()  把 PCM/float 转成 float 并 push_back

  └─ source/audiofile.h:107  Player::setAudio(std::move(data))  ← 持有 m_data（带锁）

绘图（20 Hz 定时器 source/win/gui_win.cpp:349 + :710 onTimer → InvalidateRect(m_wave)）
  └─ :930 waveProc WM_PAINT → :941 WinView::paint(dc, ps.rcPaint)
       ├─ :441-443  CreateCompatibleDC + CreateCompatibleBitmap(W×H)
       ├─ :446-448  填底色 RGB(24,24,28)
       ├─ :450-458  若 !hasAudio() → 画「把音频文件拖到这里」，然后结束
       ├─ :462      waveformPeaks(peaks, W, m_viewStart, m_viewStart+m_viewSpan)
       │              └─ source/aplaysdk.cpp:114 StableBackend::waveformPeaks
       │                   └─ :117 currentProcessor()  ← 取 g_liveProcessors.back()，见 :61-65
       │                        └─ :666 APlayProcessor::waveformPeaks
       │                             ├─ :669 out.assign(buckets, 0) ← 先铺零，绝不会是空 vector
       │                             ├─ :673 m_player.withAudio(...)  ← 全程持锁
       │                             └─ :700-722 每桶取 |max|（左+右取平均）
       ├─ :466-480  蓝色（RGB 90,190,255）1px 竖线画波形
       ├─ :499-552  小节网格（黄 RGB 255,190,90 / 灰虚线）
       ├─ :554-567  绿色谱面播放头（读 hostTimeline().playheadValid/Sec）
       ├─ :569-581  红色音频播放头（读 positionSec()）
       ├─ :583-594  正偏移时的橙色阴影
       └─ :597-600  BitBlt 上屏   ← ★ 主判断的病根在这里
```

**重要推论**：状态灯显示「● 跟随宿主播放」= `hasAudio()` 为 true = `m_data.samples` 非空 = 音频线程能从同一份数据出声。**因此 `peaks` 不可能全是 0**，波形数据一定是好的。所以问题只可能出在**绘制/上屏**这一段，不可能出在解码或峰值计算。这条推论把排查范围压缩了 80%。

---

## 4. 顺手一起修的两个小问题（非本次病因，但都是隐患）

### 4.1 坐标不一致：离屏位图用 `rcPaint` 的宽高，网格/波形却按固定 `kWaveW` 定位

`paint()` 用重绘矩形（`rc.right - rc.left`）建位图、按 `W` 取桶数；而 `secToX()`（第 426-431 行）恒定按 `kWaveW = 320` 算坐标。

- 整窗重绘时 `rcPaint = (0,0,320,118)`，两者恰好一致，所以平时看不出来。
- **一旦 Windows 送来部分重绘矩形**（别的窗口遮挡后露出来、宿主只失效了一部分），`W < 320`，波形就会按 `kWaveW` 的坐标系画进一张只有 `W` 宽的位图 → 位置错乱并被裁掉。

稳妥写法：两处都用同一个尺寸源，例如开头 `::GetClientRect(hwnd, &rc)` 并让 `secToX()` 传入宽度：

```cpp
const int W = rc.right - rc.left;   // 或干脆 GetClientRect 的宽
...
// secToX 改为按传入宽度算，或把 kWaveW 换成 W
```

### 4.2 与 macOS 版对齐守卫

macOS 版在取样前有一道守卫（`source/gui.mm:376-383`）：

```objc
if (!_st->backend->hasAudio () || dur <= 0.0 || !(vl > 0.0)) { _st->peaks.clear (); return; }
```

Windows 版没有。作用不大（`peaks` 不会是空），但**没有音频时会白扫一遍全曲**，加一道更省。另外 macOS 的桶数是 `clamp(宽度 × 1.3, 60, 800)`（`gui.mm:388`），Windows 直接等于窗口宽度（320 桶）—— 三平台行为不一致但不影响正确性，可不动。

---

## 5. 备选假设树（若第 2 节取色结果是 `#18181C`，从这里往下查）

按可能性排序，每条都给了**判定方法**和**修法方向**。

### H1 · 峰值确实为 0（可能性低，因为状态灯是 ●）
- 判定：在 `:463` 后加日志打印 `peaks` 的 max/mean（见第 6 节补丁）。
- 若真是 0：说明 `samples` 全零，但那样音频线程也该是静音 —— 与「音频正常」矛盾，所以基本可以自证排除。**这条更像是用来证伪的。**

### H2 · `currentProcessor()` 拿到了另一个处理器实例
- `source/aplaysdk.cpp:61-65`：`currentProcessor()` 返回 `g_liveProcessors.back()`（**最后创建**的那个）。
- MuseScore 某些操作（试听、轨道预览）可能额外创建处理器实例，导致编辑器连到没有载入文件的那个上。
- **但**：状态灯是 ● 且显示文件名，说明当前这个实例是有文件的 → 本条大概率不成立。留着备查。
- 判定：日志打印 `liveProcessorCount()`（声明在 `source/aplaysdk.h:201`）与 `currentPath()`。

### H3 · `paint()` 的 `else if (m_backend)` 分支没进（`m_backend` 为 null）
- 若如此，只会画底色、不画任何内容 —— 与现象吻合，但 `refreshLabels()`（`:718`）同样依赖 `m_backend`，它为 null 时标签会停在初始文案「未载入音频 / ○ 未载入」，与实测不符 → **可排除**。

### H4 · `waveProc` 里 `viewOf(hwnd)` 拿到 null
- `GWLP_USERDATA` 在第 296 行显式设置，正常不会丢。若宿主销毁并重建了子窗口而 `USERDATA` 丢失，`WM_PAINT` 会 `return 0` 什么都不画。
- 但此时 `WM_ERASEBKGND` 返回 1（`:963`）会抑制类背景擦除，画面应是**脏像素残影**而非干净纯色 → 与现象不符，**可能性低**。
- 判定：日志打印 `viewOf(hwnd)` 的指针值。

### H5 · DPI 缩放导致线条被抹掉
- 会同时让按钮文字发虚，用户应能察觉；实测其它控件正常 → **可能性低**。

### H6 · 窗口尺寸/布局异常（波形子窗口被裁）
- `layoutChildren()`（`:382-418`）把波形子窗口放到 `(10, 34, 320×118)`；`m_timesig` 下拉框被设成 `(150, 224, 70×200)`，高度 200 会伸到 y=424（超出面板 384），但**不覆盖波形区**（y 34-152），经核对无遮挡。
- 判定：日志打印波形子窗口的 `GetWindowRect`。

---

## 6. 调试补丁（一次性锁定病因，查完删除）

粘到 `source/win/gui_win.cpp` 的匿名命名空间里（例如 `modsNow()` 附近，第 130 行前后），并补 `#include <cstdarg>`：

```cpp
// —— 临时诊断：把波形相关状态写到文件，查完删掉这段 ——
void waveDbg (const char* fmt, ...)
{
    static FILE* f = nullptr;
    if (!f)
    {
        char p[MAX_PATH] = {0};
        const char* tmp = std::getenv ("TEMP");
        std::snprintf (p, sizeof p, "%s\\DaweiDrumScore-wave.log",
                       tmp ? tmp : "C:\\");
        f = std::fopen (p, "w");
        if (!f) return;
    }
    va_list ap; va_start (ap, fmt);
    std::vfprintf (f, fmt, ap);
    va_end (ap);
    std::fputc ('\n', f);
    std::fflush (f);
}
```

在 `WinView::paint()` 第 439 行（`if (W <= 0 || H <= 0) return;` 之后）插入：

```cpp
    waveDbg ("paint rc=(%ld,%ld,%ld,%ld) W=%d H=%d hasAudio=%d path=%s err=%s",
             rc.left, rc.top, rc.right, rc.bottom, W, H,
             static_cast<int> (m_backend->hasAudio ()),
             m_backend->currentPath ().c_str (),
             m_backend->lastError ().c_str ());
```

在 `:463` 之后（拿到 peaks 之后）插入：

```cpp
        {
            float mn = 1e9f, mx = -1e9f, sum = 0.f;
            for (float v : peaks) { if (v < mn) mn = v; if (v > mx) mx = v; sum += v; }
            waveDbg ("  peaks n=%zu viewStart=%.3f viewSpan=%.3f dur=%.3f "
                     "min=%.5f max=%.5f mean=%.5f",
                     peaks.size (), m_viewStart, m_viewSpan, m_backend->durationSec (),
                     mn, mx, peaks.empty () ? 0.f : sum / static_cast<float> (peaks.size ()));
        }
```

在 `waveProc` 的 `WM_PAINT` 分支（第 936 行）插入：

```cpp
            waveDbg ("WM_PAINT view=%p rcPaint=(%ld,%ld,%ld,%ld)",
                     reinterpret_cast<void*> (v),
                     ps.rcPaint.left, ps.rcPaint.top,
                     ps.rcPaint.right, ps.rcPaint.bottom);
```

**日志位置：`%TEMP%\DaweiDrumScore-wave.log`**（一般是 `C:\Users\<你>\AppData\Local\Temp\`）。

### 怎么读这份日志

| 日志表现 | 结论 |
|---|---|
| `WM_PAINT` 根本没出现 | 重绘没触发 → 查 H4 |
| `WM_PAINT` 出现但 `view=(nil)` | `GWLP_USERDATA` 丢了 → H4 |
| `hasAudio=0` | 后端没音频 → 查 H2 / 状态灯自相矛盾 |
| `hasAudio=1` 且 `peaks max > 0.01`，但屏幕空白 | **确诊绘制/上屏问题 → 第 1 节的补丁** |
| `peaks max` 接近 0 | 解码数据有问题 → 查 `audiofile_win.cpp` |

---

## 7. Windows 构建 / 安装 / 验证

### 本地构建（工作机上）

```powershell
# 1) 拉 SDK（含子模块），只需一次
git clone --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3sdk

# 2) 配置 + 编译（不要写死 Visual Studio 版本，镜像/机器差异大）
cmake -S . -B build -A x64
cmake --build build --config Release --target DaweiDrumScore --parallel
```

产物：`build\VST3\Release\DaweiDrumScore.vst3`（整个**文件夹**，不是单个文件）

### 安装（临时验证用）

把整个 `DaweiDrumScore.vst3` 文件夹复制到：

```
C:\Program Files\Common Files\VST3\
```

然后**重启 MuseScore 4**，混音器 → 任一轨道 **Sound** 列 → 选「大伟鼓谱MuseScore音频播放器」。

> 若插件不出现在列表里：MuseScore 的插件扫描缓存是
> `%APPDATA%\MuseScore\MuseScore4\known_audio_plugins.json`，
> 删掉里面指向不存在路径的条目，或整体删除该文件让它重扫。

### 云端构建（不想装 VS 也可以）

推送到 `main` 会自动跑 `.github/workflows/build-windows.yml`，也可在 Actions 页面手动 `workflow_dispatch`。
产物：`DaweiDrumScore-Windows-x64`（绿色版 bundle）+ `DaweiDrumScore-Setup-Windows-x64`（Inno Setup 安装器）。

### 日志

- 崩溃日志：**`%USERPROFILE%\DaweiDrumScore.log`**（见 `source/win/crashguard_win.cpp:38-54`）
- 调试日志（打了第 6 节补丁后）：`%TEMP%\DaweiDrumScore-wave.log`

### 改完必须确认没伤到别人

- **导出符号不能少**：Windows 上 VST3 模块**只需 `GetPluginFactory`** 必需；`InitDll`/`ExitDll` 在 SDK 里标注为 optional（见 `public.sdk/source/vst/hosting/module_win32.cpp`）。改 `aplaysdk.cpp` 的入口段时注意别破坏 `#if SMTG_OS_WINDOWS` 分支。
- **Linux 那三个符号**（`ModuleEntry`/`ModuleExit`/`GetPluginFactory`）由 `#if SMTG_OS_LINUX` 提供，改共用文件时别误删。
- **面板尺寸改了就同步 4 处**：`gui_win.cpp` 的 `kPanelW/kPanelH`、`layoutChildren()`、`getSize()`、`checkSizeConstraint()`（`:1096`/`:1132`）。

---

## 8. 验收标准

- [ ] 波形区出现蓝色波形包络（能看出鼓点位置）
- [ ] 「网格」「小节号」勾选时出现黄色小节线 + 灰色拍线 + 小节号
- [ ] 绿色谱面播放头 / 红色音频播放头随播放移动
- [ ] 拖入音频能载入、缩放（缩小/放大/全览）、拖动定位、滚轮平移、Ctrl/Alt 拖动改偏移均正常
- [ ] 窗口被别的窗口遮挡再露出来，波形**不消失、不错位**（验 4.1）
- [ ] 面板按钮/标签/中文显示无回归
- [ ] 打包出的 bundle 仍能被 MuseScore 正常扫描到（导出符号没破坏）

---

## 9. 交接时务必一并交代的上下文

**这是三平台项目，Windows 只是其中之一。改 Windows 时不要引入平台相关性到共用文件。**

| 文件 | 平台 | 备注 |
|---|---|---|
| `source/player.cpp` | **共用** | 纯 C++ 播放核心（重采样、偏移、循环） |
| `source/aplaysdk.cpp` | **共用** | VST3 处理器/控制器/工厂 + 三平台模块入口 |
| `source/audiofile.h` | **共用** | `AudioData` / `Player` / `SharedParams` 定义 |
| `source/gui.h` | **共用** | `PlugView::Backend` 抽象接口 |
| `source/audiofile.mm` `gui.mm` `crashguard.mm` | macOS | AppKit / AVFoundation |
| `source/win/*.cpp` | Windows | Win32 / Media Foundation |
| `source/linux/*.cpp` | Linux | X11+Xft 自绘 / dr_libs 解码 |

三平台通过 `ap::PlugView::Backend` 同一套纯虚接口对接，**GUI 层只管画，数据全在后端**。所以「波形不显示」这类问题优先怀疑 GUI 绘制，而不是解码。
