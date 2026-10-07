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
