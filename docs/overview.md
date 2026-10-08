# 大伟鼓谱 MuseScore 音频播放器（VST3）— 交付概览

## 本轮（2026-10-08）：三平台齐活 —— 新增 Linux 版

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
