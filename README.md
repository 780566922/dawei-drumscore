# 大伟鼓谱 · MuseScore 音频播放器

> 给 MuseScore 4 补上「边看谱、边听伴奏」的能力 —— 一款跨平台 VST3 音频播放器插件。

MuseScore 本身不支持加载外部音频伴奏。这个插件以 VST3 的形式挂进 MuseScore 的混音器，
把 mp3 / wav / m4a 等音频文件直接拖进插件窗口，就能在**谱面滚动的同时**播放伴奏，
并用一条红线把音频播放位置映射到谱面上，方便对照练习。

作者 B 站：**大伟鼓谱** —— 欢迎关注，鼓谱 / 教学 / 伴奏持续更新。

---

## 功能特性

- 🎵 **拖拽即播**：把音频文件拖进插件窗口即可加载，支持 mp3 / wav / m4a / aac / flac 等
- 📈 **波形 + 小节网格**：显示音频波形与按拍号推算的小节线、小节号（可独立开关）
- 🔴 **播放位置映射**：红线标记音频当前播放位置在谱面上的对应位置
- ↔️ **免裁剪对齐（偏移）**：音频与谱面起点不一致时，用偏移对齐，无需裁剪音频
- ⏯️ **跟随宿主播放状态**：插件音频严格跟随 MuseScore 的播放 / 暂停 / 定位
- 🔍 **缩放 / 全览 / 回到谱面**：长音频也能快速定位
- ⏱️ **BPM 手动输入**：用于小节线换算（MuseScore 不向插件提供 tempo）
- 🧱 **跨平台**：macOS 与 Windows 同一份源码

---

## 安装

### macOS（推荐：PKG 安装包）

1. 到本仓库 [Releases](../../releases) 页面下载 `大伟鼓谱MuseScore音频播放器-*.pkg`
2. 双击安装，按提示输入管理员密码
3. 重启 MuseScore

> **首次打开被系统拦截？** 本安装包未购买 Apple 开发者证书签名，macOS 的
> Gatekeeper 会提示"无法打开"。解决方式：**右键点击安装包 → 打开 → 在弹窗里再点"打开"**。
> 随包附有 `安装说明-请先读.txt`。

装到的是系统目录 `/Library/Audio/Plug-Ins/VST3/`，对所有账号与所有支持 VST3 的宿主生效。

### Windows

1. 到 [Releases](../../releases) 下载 Windows 版压缩包
2. 解压得到 `大伟鼓谱MuseScore音频播放器.vst3`
3. 复制到 `C:\Program Files\Common Files\VST3\`
4. 重启 MuseScore

> Windows 版本目前**未经充分实机测试**，如遇问题请提 [Issue](../../issues)。

### 在 MuseScore 里启用

混音器 → 任一轨道的 **Sound** 列 → 选择 **大伟鼓谱MuseScore音频播放器**。

> 插件菜单层级为 `VST → 厂商名 → 插件名`，插件名取自 bundle 文件名。

---

## 从源码构建

### 依赖

- **VST3 SDK 3.8.1**（Steinberg 官方）
- macOS：Xcode Command Line Tools（clang++）
- Windows：Visual Studio 2019+（MSVC）或 CMake + MSVC
- 通用：CMake 3.20+

下载 VST3 SDK 并解压到任意目录（默认约定为 `~/WorkBuddy/vst3sdk`）：

<https://www.steinberg.net/developers/>

### macOS

```bash
git clone <本仓库地址>
cd <仓库目录>

# 如果 SDK 不在默认位置，用环境变量指定：
export VST3_SDK_DIR=/path/to/vst3sdk

./build.sh          # 编译 + 6 项自动验证，全绿即成功
```

产物：`build/大伟鼓谱MuseScore音频播放器.vst3`

可选：`./make_pkg.sh` 生成可分发的 PKG（在 `dist/`）。

### Windows

```powershell
cmake -S . -B build -DVST3_SDK_DIR=C:/path/to/vst3sdk
cmake --build build --config Release --parallel
```

产物：`build/VST3/Release/大伟鼓谱MuseScore音频播放器.vst3`

### 通用 CMake

`-DVST3_SDK_DIR=<SDK 路径>` 两个平台通用；Windows 在 CI 上自动构建
（见 `.github/workflows/build-windows.yml`）。

---

## 项目结构

```
source/
├── player.cpp            # 音频播放内核（纯 C++，跨平台）
├── audiofile.h           # Player 抽象（纯 C++）
├── aplaysdk.cpp/.h       # VST3 插件主体：参数、状态、宿主同步、播放逻辑（跨平台）
├── plugin.mm             # VST3 bundle 入口
├── gui.h                 # 界面后端纯虚接口（跨平台）
├── gui.mm                # macOS 界面实现（AppKit）
├── audiofile.mm          # macOS 音频解码（AVFoundation）
├── crashguard.mm/.h      # 崩溃取证
└── win/                  # Windows 平台实现
    ├── gui_win.cpp       #   Win32 自绘界面
    ├── audiofile_win.cpp #   Media Foundation 解码
    └── crashguard_win.cpp#   SEH + DbgHelp 崩溃取证

build.sh                  # macOS 构建脚本（含验证）
make_pkg.sh               # macOS PKG 打包脚本
CMakeLists.txt            # 跨平台 CMake 构建
validate.cpp              # 插件结构与接口自动验证
```

---

## 常见问题

**Q：为什么没有 BPM 自动检测？**
MuseScore 的 VST3 接口不向插件提供 tempo / 拍号（实测 tempo 事件数为 0），插件无从得知，
只能手动输入 BPM 用于小节线换算。

**Q：插件能控制 MuseScore 的播放 / 暂停吗？**
不能。VST3 协议没有反向控制宿主传输的通道。插件播放状态严格**跟随**宿主。
空格键由 MuseScore 自身处理，如无法控制播放，请先在谱面区域点一下让宿主获得焦点。

**Q：macOS 上两个播放位置"分家"？**
已修复。根因是插件此前音频播放状态与宿主脱钩，现在严格跟随宿主的 `kPlaying` 位。

**Q：剪切板 / 拖动音频时卡顿？**
波形计算在 GUI 线程，解码在音频线程，已做锁优化；如仍卡顿请提 Issue 并附日志
（macOS：`~/Library/Logs/DaweiDrumScore.log`）。

---

## 许可证

本项目以 **GNU General Public License v3.0**（GPL-3.0）发布，见 [LICENSE](LICENSE)。

之所以采用 GPLv3，是因为本项目使用了 **Steinberg VST 3 SDK**，
而该 SDK 采用「GPLv3 / Steinberg 专有许可」双许可模式。
以 GPLv3 分发是与 SDK 许可证兼容的方式（若需闭源商用，须另行向 Steinberg 申请专有许可）。

VST 是 Steinberg Media Technologies GmbH 的商标。本项目与 Steinberg、Muse Group 无隶属关系。

---

## 致谢

- [Steinberg VST 3 SDK](https://github.com/steinbergmedia/vst3sdk)
- 献给所有边看谱边练鼓的朋友 🥁
