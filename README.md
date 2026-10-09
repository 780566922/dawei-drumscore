# 大伟鼓谱 · MuseScore 音频播放器

> 给 MuseScore 4 补上「边看谱、边听伴奏」的能力 —— 一款跨平台 VST3 音频播放器插件（macOS / Windows / Linux）。

MuseScore 本身不支持加载外部音频伴奏。这个插件以 VST3 的形式挂进 MuseScore 的混音器，
把 mp3 / wav / m4a 等音频文件直接拖进插件窗口，就能在**谱面滚动的同时**播放伴奏，
并用波形上的小节网格和播放位置指示线对照练习。

作者 B 站：**大伟鼓谱** —— 欢迎关注，鼓谱 / 教学 / 伴奏持续更新。
插件界面底部那条粉色署名就是链接，点一下直接打开作者主页。

---

## 功能特性

- 🎵 **拖拽即播**：把音频文件拖进插件窗口即可加载
  （macOS / Windows：mp3 / wav / m4a / aac / flac / ogg 等；Linux：mp3 / wav / flac / ogg）
- 📈 **波形 + 小节网格**：显示音频波形与按拍号推算的小节线、小节号（可独立开关）
- 🔴 **双播放头指示**：红线 = 音频播到哪，绿线 = 乐谱播到哪（正常时两者重合；
  分开就是跟随出故障，点「回到播放头」修复）
- ↔️ **免裁剪对齐（偏移）**：音频与谱面起点不一致时，按住 ⌘/⌥在波形上拖动
  **网格线**即可对齐（把「1」小节线拖到音乐第一拍），无需裁剪音频；
  旁边有**「归零」**按钮，一下把偏移清零（拖歪了不用反向拖回去）
- 🪟 **窗口可拉宽**：编辑器窗口可以自由拖动边缘调整大小，波形区随宽度一起变宽
  （音频工具最需要横向分辨率）；默认宽度已从 340 加宽到 680
- ⏯️ **跟随宿主播放状态**：插件音频严格跟随 MuseScore 的播放 / 暂停 / 定位
- 🔍 **缩放 / 全览 / 回到播放头**：长音频也能快速定位；一键跳回正在播放的位置
- ❓ **内置使用指南**：点界面右上角「？帮助」即可看到完整操作说明（离线内置）
- 🚨 **分家检测**：音频与谱面播放头错位时，波形区会亮出红色提示徽标；
  点一次「回到播放头」仍错位则提示「建议重启插件」并记一条日志
- ⏱️ **BPM 手动输入**：用于小节线换算（MuseScore 不向插件提供 tempo）；
  **关掉插件界面再打开，填过的值会记住**（BPM / 拍号 / 偏移 / 音量都回读）
- 🖱️ **视野不抢手**：播放中你拖/缩波形时，播放头允许待在画面外，
  插件不会把视野拽回去（只有宿主自己跳转播放头时才跟随）；
  音频侧用不可变快照消除"画波形饿死音频线程"导致的丢块与分家
- 🎀 **页脚可点击**：底部 B 站署名是链接，点一下用默认浏览器打开作者主页
- 🧱 **跨平台**：macOS / Windows / Linux 同一份源码

---

## 安装

### macOS（推荐：PKG 安装包）

1. 到本仓库 [Releases](../../releases) 页面下载 `DaweiDrumScore-*-macOS.pkg`
2. 双击安装，按提示输入管理员密码
3. 重启 MuseScore

> **首次打开被系统拦截？** 本安装包未购买 Apple 开发者证书签名，macOS 的
> Gatekeeper 会提示"无法打开"。解决方式：**右键点击安装包 → 打开 → 在弹窗里再点"打开"**。
> 随包附有 `安装说明-请先读.txt`。

装到的是系统目录 `/Library/Audio/Plug-Ins/VST3/`，对所有账号与所有支持 VST3 的宿主生效。

### Windows（推荐：安装器）

1. 到本仓库 [Releases](../../releases) 页面下载 `DaweiDrumScore-*-Windows-Setup.exe`
2. 双击运行；若 SmartScreen 提示「Windows 已保护你的电脑」→ **更多信息 → 仍要运行**
   （安装器未做代码签名，手动放行一次即可）
3. 一路「下一步」完成安装（安装器向导为英文，插件本身是中文）
4. 重启 MuseScore

装到系统目录 `C:\Program Files\Common Files\VST3\`，对所有账号与所有支持 VST3 的宿主生效。

> 也可从仓库 [Actions](../../actions) 页面的构建产物中下载裸 bundle（`DaweiDrumScore-Windows-x64`），
> 手动把 `DaweiDrumScore.vst3` 文件夹放进上述 VST3 目录。
>
> Windows 版本目前**未经充分实机测试**，如遇问题请提 [Issue](../../issues)。

### Linux

Release 页面提供三种包，按你的发行版挑一个：

**① 通用压缩包 —— 任何发行版都能用（含 Arch / Gentoo / NixOS）**

1. 下载 `DaweiDrumScore-*-Linux-x86_64.tar.gz`
2. 解压后进入目录，执行：`./install.sh`
3. 重启 MuseScore 4

装到用户目录 `~/.vst3/`，**不需要 sudo**，也不影响系统里其它用户。

**② Debian / Ubuntu / Mint / Pop!_OS 系**

```bash
sudo apt install ./DaweiDrumScore-*-Linux-x86_64.deb
```

也可以直接在文件管理器里双击，走图形化软件中心安装。

**③ Fedora / RHEL / Rocky / openSUSE 系**

```bash
sudo dnf install ./DaweiDrumScore-*-Linux-x86_64.rpm
```

②③ 装到系统级 `/usr/lib/vst3/`（VST3 在 Linux 的标准路径），需要管理员权限。

**运行要求**：glibc ≥ 2.31，即 Ubuntu 20.04+ / Debian 11+ / RHEL 9+；
依赖 `libX11` 与 `libXft`（桌面发行版默认都装了）。

> 插件本身**零额外运行时依赖** —— MP3 / WAV / FLAC / OGG 的解码器直接编进了 .so，
> 不需要 FFmpeg、libsndfile 之类任何东西。

> **格式支持**：Linux 版支持 **MP3 / WAV / FLAC / OGG（Ogg Vorbis）**。
> m4a / aac 在 Linux 上没有零依赖的解码方案，为保「下载即用、零运行时依赖」故未支持
> —— 请先转成 MP3 或 WAV。
> ⚠️ OGG 指的是 **Ogg Vorbis**；同后缀的 **Ogg Opus** 不支持（是另一套编码），会给出明确提示。
>
> Linux 版本目前**未经实机测试**，如遇问题请提 [Issue](../../issues)。

### 在 MuseScore 里启用

混音器 → 任一轨道的 **Sound** 列 → 选择 **大伟鼓谱MuseScore音频播放器**。

> 插件菜单层级为 `VST → 厂商名 → 插件名`，插件名取自 bundle 文件名。

---

## 从源码构建

### 依赖

- **VST3 SDK 3.8.1**（Steinberg 官方）
- macOS：Xcode Command Line Tools（clang++）
- Windows：Visual Studio 2019+（MSVC）或 CMake + MSVC
- Linux：GCC / Clang + X11 / Xft 开发包（`libx11-dev libxft-dev`）
- 通用：CMake 3.25+（VST3 SDK 3.8.1 自身的要求；Ubuntu 20.04 自带的 3.16 不够，
  需另装官方 CMake）

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

产物：`build/VST3/Release/DaweiDrumScore.vst3`

可选：用 [Inno Setup](https://jrsoftware.org/isinfo.php) 编译 `installer/windows/setup.iss`
生成双击即装的安装器（CI 中已自动完成）。

### Linux

```bash
sudo apt install build-essential cmake pkg-config libx11-dev libxft-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVST3_SDK_DIR=/path/to/vst3sdk
cmake --build build --target DaweiDrumScore --parallel
```

产物：`build/VST3/Release/DaweiDrumScore.vst3`

打包成三种安装包（脚本也可在本地跑，缺 dpkg-deb / rpmbuild 时自动跳过对应格式）：

```bash
sh installer/linux/make_packages.sh build/VST3/Release/DaweiDrumScore.vst3 1.0.1 dist
```

> 解码不依赖任何音频开发库 —— 三个单文件解码库（dr_wav / dr_mp3 / dr_flac）
> 已 vendored 在 `source/linux/third_party/`，随插件一起编译。

> ⚠ **构建底座的 glibc 版本 = 产物的 glibc 下限。**
> 官方在 **Ubuntu 20.04** 容器（glibc 2.31 / GCC 9）里构建，产物可跑在
> Ubuntu 20.04+ / Debian 11+ / RHEL 9+。若在更新的发行版上构建，产物的
> `GLIBC_*` 符号需求会被顶高（例如 Ubuntu 24.04 + GCC 13 会因
> `-std=gnu++17` 隐含 `_GNU_SOURCE` → `_ISOC23_SOURCE`，导致 `sscanf` 被
> 重定向到 `__isoc23_sscanf`，硬性要求 GLIBC_2.38），于是 Ubuntu 22.04 /
> Debian 12 的用户加载插件时会被动态链接器直接拒掉。CI 里有断言守住这条线，
> 详见 `docs/overview.md`。

### 通用 CMake

`-DVST3_SDK_DIR=<SDK 路径>` 三平台通用；Windows 与 Linux 在 CI 上自动构建
（见 `.github/workflows/`）。

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
├── win/                  # Windows 平台实现
│   ├── gui_win.cpp       #   Win32 自绘界面
│   ├── audiofile_win.cpp #   Media Foundation 解码
│   └── crashguard_win.cpp#   SEH + DbgHelp 崩溃取证
└── linux/                # Linux 平台实现
    ├── gui_linux.cpp     #   X11 + Xft 自绘界面
    ├── audiofile_linux.cpp  # dr_libs 解码（零系统依赖）
    ├── crashguard_linux.cpp # signal + backtrace 崩溃取证
    ├── dr_libs_impl.cpp  #   三个单文件解码库的实现单元
    └── third_party/      #   dr_wav.h / dr_mp3.h / dr_flac.h

build.sh                  # macOS 构建脚本（含验证）
make_pkg.sh               # macOS PKG 打包脚本
installer/windows/        # Windows 安装器（Inno Setup）
installer/linux/          # Linux 安装脚本 + 打包脚本
├── install.sh            #   用户级安装到 ~/.vst3（免 sudo）
└── make_packages.sh      #   打 tar.gz / .deb / .rpm 三种包
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
已修复，而且是两层原因、两层修法：
1. 插件音频播放状态曾与宿主脱钩 → 现在严格跟随宿主的 `kPlaying` 位。
2. **播放中左右拖动波形会分家** → 旧版画波形要在锁内遍历几十万采样，音频线程
   抢不到锁就**整块丢弃音频**，每丢一块播放位置就永久落后宿主一点（不只是视觉问题）。
   现在音频数据是不可变快照（`shared_ptr<const AudioData>`），画波形完全不碰那把锁；
   若仍丢块，波形区日志会给出累计块数。

**Q：剪切板 / 拖动音频时卡顿？**
波形计算在 GUI 线程，解码在音频线程，已做锁优化；如仍卡顿请提 Issue 并附日志
（macOS：`~/Library/Logs/DaweiDrumScore.log`、Windows：`%USERPROFILE%\DaweiDrumScore.log`、
Linux：`$HOME/DaweiDrumScore.log`）。

**Q：Windows 上滚轮推不动波形？**
已修复。根因是 Windows 把滚轮消息发给**键盘焦点窗口**，而不是鼠标指针下的那个窗口 ——
消息压根没送到波形区（大多被 BPM 输入框之类的控件接走了）。现在主窗口会按**鼠标位置**
判断：指针在波形区上就自己处理滚轮（直接滚 = 平移；`Ctrl`/`Alt` + 滚轮 = 缩放），不在就原样
交给 MuseScore，不抢宿主自己的滚轮。另外横向滚轮（鼠标左右拨 / 触控板左右滑）也支持了。

**Q：编辑器窗口可以调大吗？**
可以，拖窗口边缘就行，波形区会跟着一起变宽（音频工具最需要横向分辨率）。
默认宽度已从 340 加宽到 **680**；也能拉回到 340 那么窄，布局仍然排得下。

**Q：Linux 版为什么打不开 m4a / aac？**
Linux 没有像 AVFoundation / Media Foundation 那样的系统级零依赖解码方案。
为了「下载即用、装完零运行时依赖」，Linux 版把 MP3 / WAV / FLAC 三个解码器直接编进了插件；
m4a / aac 需要额外引入 FFmpeg 一类依赖，会让用户安装变复杂，故暂不支持 —— 转成 MP3 即可。

**Q：OGG 支持到什么程度？Opus 呢？**
支持 **Ogg Vorbis**（`.ogg` / `.oga`），三个平台都能放。
这一点值得单独说明：macOS 的 CoreAudio 和 Windows 的 Media Foundation **都没有内置 Vorbis 解码器**
（微软把它放在商店的可选包「Web Media Extensions」里，默认不装），所以 OGG 不是"调用系统解码"，
而是插件**自带了一份解码器**（stb_vorbis，三端共用同一份源码）。

**Ogg Opus 不支持**（`.opus` 或后缀写成 `.ogg` 的 Opus）。Opus 是另一套编码，需要另一个解码器。
插件会识别出来并明确提示"这是 Ogg Opus"，而不是含糊地报"解码失败" —— 转成 MP3 或 OGG 即可。

**Q：Linux 版能在我的发行版上跑吗？**
能跑的条件是 **glibc ≥ 2.31**：Ubuntu 20.04+ / Debian 11+ / RHEL 9+ / Fedora 32+ 及更新的桌面发行版都满足。
运行时只依赖 `libX11` 与 `libXft`（桌面发行版默认都有），不需要 FFmpeg / libsndfile 之类任何东西。

如果 MuseScore 里根本看不到插件、且 `$HOME/DaweiDrumScore.log` 是空的，多半是动态链接器
在加载阶段就把插件拒了（`.so` 的 glibc 需求高于系统）——可以这样确认：

```bash
readelf --version-info ~/.vst3/DaweiDrumScore.vst3/Contents/x86_64-linux/*.so | grep GLIBC_ | sort -uV
ldd -r ~/.vst3/DaweiDrumScore.vst3/Contents/x86_64-linux/*.so
```

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
- [dr_libs](https://github.com/mackron/dr_libs)（Linux 版解码库，公共领域 / MIT-0）
- 献给所有边看谱边练鼓的朋友 🥁
