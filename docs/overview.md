# 大伟鼓谱 MuseScore 音频播放器（VST3）— 交付概览

## 本轮（2026-10-08）：安装程序 + Windows 跨平台

### 一、macOS 安装包（已完成，可分发）
- `./make_pkg.sh` → `dist/大伟鼓谱MuseScore音频播放器-1.0.0.pkg`（约 338 KB）
- 装到**系统域** `/Library/Audio/Plug-Ins/VST3/`：双击 → 输密码 → 完成（VST 插件标准做法）
- 含欢迎页 / 使用说明 / 许可 / 完成页（B 站引流在欢迎页与页脚）
- postinstall：自动清理旧命名残留（APLAY.vst3 / DaweiDrumScore.vst3）+ 去除下载隔离属性
- **验证**：解包比对，包内二进制与源 bundle **md5 逐字节一致**；Info.plist 字段正确
- 因未购买开发者证书，首次打开会被 Gatekeeper 拦 →
  随包附 `dist/安装说明-请先读.txt`（教「右键 → 打开」放行）

### 二、Windows 版（跨平台化已完成，待 CI 编译）
- 架构：**同一份源码**，平台实现分目录（`source/*.mm` 对 `source/win/*.cpp`）
- 直接复用：`player.cpp`、`aplaysdk.cpp`（零 ObjC）、`PlugView::Backend` 纯虚接口
- 新写：Media Foundation 解码、SEH + DbgHelp 崩溃守卫、Win32 自绘界面
- 构建：`CMakeLists.txt`（**已在本机验证 Mac 端可出包**）+ `.github/workflows/build-windows.yml`
- 操作手册：`Windows版-发布指引.md`（git push → Actions 出包 → 安装位置）

### 关键结论
| | macOS | Windows |
|---|---|---|
| 状态 | ✅ 可用（含 PKG 安装包） | ⏳ 代码完成，待 CI 编译验证 |
| 本地构建 | `./build.sh`（6 项验证全绿） | 不需要 |
| 云端构建 | 不需要 | GitHub Actions |
| 解码 | AVFoundation | Media Foundation |
| 界面 | AppKit | Win32 |
| 崩溃取证 | signal + backtrace | SEH + DbgHelp |

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
