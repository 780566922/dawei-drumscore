# 大伟鼓谱 MuseScore 音频播放器 —— 技术说明与版本记录

> 安装与操作说明见 [README](../README.md) 与 [使用说明](../使用说明.md)。
> 本文面向开发者与构建者，说明项目的技术方案与各版本的功能变化。

---

## 一、项目概况

本项目是一个跨平台 **VST3 音频播放器插件**，用于给 MuseScore 4 补上「边看谱、边听伴奏」的能力。
插件挂载在 MuseScore 的混音器上，载入音频文件后播放伴奏，并在同一窗口内显示波形、
小节网格与播放位置指示线，便于对照练习。

三端共用同一份业务源码，界面层按平台分别实现：macOS 使用 AppKit、Windows 使用 Win32、Linux 使用 X11 + Xft。

---

## 二、平台支持

| | macOS | Windows | Linux |
|---|---|---|---|
| 发行包 | `.pkg` | `Setup.exe` | `.tar.gz` / `.deb` / `.rpm` |
| 安装位置 | `/Library/Audio/Plug-Ins/VST3` | `C:\Program Files\Common Files\VST3` | `~/.vst3`（tar.gz）或 `/usr/lib/vst3`（deb/rpm） |
| 界面 | AppKit | Win32（GDI 自绘） | X11 + Xft（全自绘） |
| 音频解码 | AVFoundation / CoreAudio | Media Foundation | vendored dr_libs（零系统依赖） |
| 音频格式 | mp3 / wav / m4a / aac / alac / aiff / caf / flac / ogg | mp3 / wav / m4a / aac / wma / flac / aiff / ogg | mp3 / wav / flac / ogg |
| 崩溃取证 | `signal` + `backtrace` | SEH + DbgHelp | `sigaction` + 寄存器现场 |
| 模块入口 | `bundleEntry` / `bundleExit` | `GetPluginFactory` | `ModuleEntry` / `ModuleExit` / `GetPluginFactory` |
| 安装放行 | Gatekeeper（右键打开） | SmartScreen（更多信息 → 仍要运行） | 无（未签名亦不拦截） |

**运行环境要求**

- macOS：无额外要求，音频解码走系统框架。
- Windows：Windows 10 1709 及以上（Media Foundation 自带解码器覆盖范围内的格式）。
- Linux：glibc ≥ 2.31，即 Ubuntu 20.04+ / Debian 11+ / RHEL 9+；运行时依赖 `libX11` 与 `libXft`。
  MP3 / WAV / FLAC / OGG 的解码器已编入 `.so`，不需要 FFmpeg、libsndfile 等任何额外运行时。

---

## 三、源码结构

```
source/
├── player.cpp            # 音频播放内核（平台无关）
├── audiofile.h           # 音频解码的抽象接口（平台无关）
├── aplaysdk.cpp/.h       # 插件主体：参数、状态、宿主同步、播放逻辑（平台无关）
├── gui.h                 # 界面层的后端接口与共用算法（平台无关）
├── ogg_vorbis.cpp/.h     # Ogg Vorbis 解码（平台无关，三端共用）
├── gui.mm                # macOS 界面（AppKit）
├── audiofile.mm          # macOS 音频解码（AVFoundation）
├── crashguard.mm/.h      # 崩溃取证
├── win/                  # Windows 平台实现
│   ├── gui_win.cpp
│   ├── audiofile_win.cpp
│   └── crashguard_win.cpp
└── linux/                # Linux 平台实现
    ├── gui_linux.cpp
    ├── audiofile_linux.cpp
    ├── crashguard_linux.cpp
    ├── dr_libs_impl.cpp
    └── third_party/      # dr_wav.h / dr_mp3.h / dr_flac.h
```

**跨平台约定**

- 凡「同一语义要在三端各写一遍」的逻辑，一律上提到 `gui.h`（例如小节网格的推进算法
  `ap::buildBars()`），只保留一份实现，避免三端行为漂移。
- 新增源文件需同时登记到 `CMakeLists.txt` 的 `AP_SOURCES` 与 `build.sh`。
- 版本号出现在 9 处，升级时必须同步：`CMakeLists.txt`、`source/plugin.mm`、
  `source/aplaysdk.cpp`（两处）、`build.sh`（两处）、`make_pkg.sh`、
  `installer/windows/setup.iss`、`pkg/distribution.xml`。

---

## 四、技术要点

### 4.1 音频解码

macOS 与 Windows 使用系统框架解码；Linux 没有覆盖同等格式范围、且不引入运行时依赖的系统方案，
因此将 dr_wav / dr_mp3 / dr_flac 三个单文件解码库随插件一起编译，实现「安装完即可用」。

Ogg Vorbis 是三端系统框架都覆盖不到的格式（CoreAudio 从未提供 Vorbis 解码器；
Media Foundation 将其放在默认不安装的可选包中），因此由插件自带 stb_vorbis 解码器，
三端共用同一份源码。同后缀的 Ogg Opus 属于另一套编码，不在支持范围内，加载时会给出明确提示。

### 4.2 Linux 产物与 glibc 下限

**构建底座的 glibc 版本即产物的 glibc 下限** —— 无法通过编译选项下调，因为使用的就是该版本的头文件与符号版本。

官方构建在 Ubuntu 20.04 容器内进行（glibc 2.31 / GCC 9），产物可运行于 Ubuntu 20.04+ / Debian 11+ / RHEL 9+。
若改用更新的发行版构建，产物的 `GLIBC_*` 符号需求会被顶高，导致较旧发行版的用户在动态链接阶段
就被拒绝加载、且不产生任何日志。为此 CI 中有断言检查产物的 `GLIBC_*` 需求不超过 2.31，
以防构建底座被无意升级。

同为兼容性考虑，CI 的构建容器取自固定版本镜像而非 `ubuntu-latest`。

### 4.3 线程模型与音频数据

音频数据以**不可变快照**的形式共享：`AudioData` 由 `std::shared_ptr<const AudioData>` 持有，
界面线程通过 `audioSnapshot()` 取一份引用后即可自由遍历，不再持有用于音频线程的锁。

这样做的原因是：若在持有音频锁的情况下遍历数十万采样绘制波形，音频线程的 `try_lock` 会失败并
整块丢弃音频，播放位置将永久落后于宿主。音频锁只用于保护标量状态，快照锁只用于保护指针，
两者锁序固定，新增的后端锁必须与音频线程无交集。

### 4.4 状态与持久化

宿主关闭编辑器时只销毁视图对象，音频处理器仍然存活，重开时视图与控件均为新建。
因此所有「由用户操作产生、关闭界面后仍有意义」的状态都保存在处理器中并在视图构造完成后回读，
包括 BPM、拍号、偏移、音量、视野（放大倍数与起始位置），以及拍号变化表。
状态随乐谱一并保存与恢复；更换音频文件时，与文件相关的状态会被重置。

### 4.5 界面与宿主集成

- 界面元素全部自绘，尺寸按宿主给定的客户区自适应。
- 覆盖层（使用指南、拍号设置面板）采用「隐藏全部子控件 + 父窗口整块重绘」的方式实现，
  不依赖窗口层叠顺序 —— 宿主窗口的 z 序无法假定。
- Windows 上滚轮消息发送给键盘焦点窗口而非指针所在窗口，因此主窗口按指针位置做命中判断后
  再决定自行处理或转交宿主；Linux 上 X11 只把 Button4/5 作为垂直滚轮，横向滚轮为 6/7。
- 修改界面文案时需同时核对各端的行数预算，超出会被面板底边裁切。

### 4.6 自动化验证

- `validate.cpp`：插件结构与接口断言（尺寸、类名、后端行为）。
- `gui_repro.mm`：macOS 端离屏渲染的交互用例，按退出码表示结果。
- `test_player.cpp` / `race_test.cpp` / `factory_lifecycle_test.cpp` / `ogg_vorbis_test.cpp`：
  播放内核、并发安全、工厂引用计数、Ogg 解码的单元验证。
- `build.sh`：macOS 构建并串联上述全部验证。
- GitHub Actions：Windows 与 Linux 的构建与打包，另有产物 glibc 需求的断言。

Windows 与 Linux 的界面无法在 CI 中渲染，构建通过仅代表编译与打包无误。

---

## 五、版本记录

### v1.2.8（2026-10-19）

- 修复：把网格向左移、第一小节越过音频 0 秒（起始偏移为负）时，播放中出现拖播放头或
  暂停再恢复后，音频会提前出声、与谱面错位的问题。
- 修复：同一场景下波形右上角的「分家」提示误报。

### v1.2.7（2026-10-11）

- 新增**拍号条**：波形顶部按小节显示「N/M」方块，点击可设置自该小节起生效的拍号
  （分子 2 / 3 / 4 / 5 / 6 / 7 / 9 / 12，分母 2 / 4 / 8 / 16）。
- 小节网格改为逐小节推进：拍号变化点之后的小节线与小节号按新拍号重排，不再沿用统一小节长度。
- 发生过拍号变化的小节，其方块以高亮显示；将拍号选回与继承值相同即取消该小节的变化。
- 拍号变化表随乐谱保存，关闭编辑器后重新打开仍然有效。
- 三端同步实现。

### v1.2.6（2026-10-10）

- 界面内置指南、README、使用说明与安装说明的提示语统一为「只说怎么操作、只说看到什么」。

### v1.2.5（2026-10-10）

- 编辑器面板改为按宿主给定的窗口尺寸自适应布局：宽度变化时右侧控件贴边、中间留白自动分配，
  波形区随之变宽；高度保持不变。

### v1.2.4（2026-10-09）

- 界面与文档的提示文案统一润色，行为不变。

### v1.2.3（2026-10-09）

- 关闭编辑器后重新打开，视野恢复到关闭前的位置与放大倍数，不再退回整曲全览。
- 更换音频文件时，视野重置为默认值。

### v1.2.2（2026-10-09）

- 载入音频后默认放大到前 8 秒（约 4 小节 @120BPM），取代此前的整曲全览。
- 宿主切换播放位置时波形跟随，**暂停状态下同样生效**（在乐谱中逐小节定位时不必反复播放）。
- 「？帮助」面板不再被滚轮关闭。

### v1.2.1（2026-10-09）

- 使用指南改为整块重绘的实现方式，不再依赖窗口层叠顺序。
- 面板默认宽度调整为 680，布局按实际客户区自适应。
- 滚轮按指针位置路由，支持横向滚轮与触控板左右滑动。
- 降低播放中拖动波形时的重绘开销。
- 「回到播放头」后视野跟随播放头，不再让播放头跑出画面。
- 新增「偏移归零」按钮（三端），一键将起始偏移清零。

### v1.2.0（2026-10-09）

- 关闭编辑器后重新打开，BPM、拍号、偏移、音量从处理器回读，不再被默认值覆盖。
- 波形绘制改用不可变快照，消除因界面线程抢占音频锁导致的丢块与播放位置落后。
- 视野跟随规则明确为：用户手动操作期间不干预，仅在宿主自身跳转播放头时跟随，
  停止操作后自动恢复。
- 安装包在安装新版前清理旧版本的残留。
- 界面底部署名改为可点击链接。
- 内置指南按实际操作顺序重排，快捷键提示按平台逐一校准。

### v1.1.0（2026-10-08）

- 新增 **Ogg Vorbis** 支持，三端可用，解码器由插件自带。

### v1.0.2（2026-10-08）

- 修复宿主闪退：重复索取插件工厂导致引用计数耗尽，工厂指针悬垂。
- 新增工厂引用计数的回归验证。

### v1.0.1（2026-10-08）

- Windows：修复波形区显示空白。
- Windows：适配高 DPI 显示。

### v1.0.0（2026-10-08）

- 首次发布，提供 macOS 与 Windows 版本。
- Linux 版本同期完成，提供 `.tar.gz` / `.deb` / `.rpm` 三种发行包。

---

## 六、许可证

本项目以 **GNU General Public License v3.0** 发布，见 [LICENSE](../LICENSE)。

采用 GPLv3 的原因是项目使用了 **Steinberg VST 3 SDK**，该 SDK 采用
「GPLv3 / Steinberg 专有许可」双许可模式，以 GPLv3 分发是与 SDK 许可证兼容的方式。

VST 是 Steinberg Media Technologies GmbH 的商标。本项目与 Steinberg、Muse Group 无隶属关系。
