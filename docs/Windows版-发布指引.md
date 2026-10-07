# Windows 版发布指引（GitHub Actions 云端编译）

插件本体已经跨平台化：**同一份源码**，macOS 用 `build.sh` 构建，Windows 交给
GitHub 的云端机器（免费的 `windows-latest`）自动编译，你不需要自己的 Windows 电脑。

---

## 一、一次性准备：把代码传到 GitHub

在项目目录里执行（把 `你的用户名` 换成你的 GitHub 用户名）：

```bash
cd /Users/youwei/WorkBuddy/2026-10-07-02-16-48/vst3-audio-player

git init
git add .
git commit -m "feat: 跨平台化 + Windows CI + macOS PKG 安装包"
git branch -M main

# 先在 GitHub 网页上新建一个空仓库（例如 dawei-drumscore-player），再执行下面两行
git remote add origin https://github.com/你的用户名/dawei-drumscore-player.git
git push -u origin main
```

> `.gitignore` 已经把 `build/`、`dist/`、`vst3sdk/` 排除掉，不会把几百 MB 的
> SDK 和中间产物传上去。CI 会自己拉取 SDK。

---

## 二、获取 Windows 版

推送完成后：

1. 打开仓库页面 → 顶部 **Actions** 标签
2. 左侧会看到 **「构建 Windows 版」**，点进去能看到正在跑的任务
3. 首次运行大约 **5～15 分钟**（要下载并编译 VST3 SDK）
4. 跑完后，在这一次运行的页面底部 **Artifacts** 里下载
   **`DaweiDrumScore-Windows-x64`**，解压得到 `DaweiDrumScore.vst3`

如果想改代码后重新出包，直接 `git push` 即可，会自动重跑。

---

## 三、Windows 用户怎么安装

把 `DaweiDrumScore.vst3` 复制到：

```
C:\Program Files\Common Files\VST3\
```

（需要管理员权限。如果这个目录不存在就手动建一个。）

然后：

1. 完全退出 MuseScore，重新打开
2. 按 `F10` 打开混音器
3. 任一轨道的 **Sound** 列 → 选 **DaweiDrumScore**
4. 点 Sound 那一格 → 把音频拖进去

> 卸载 = 删掉那个 `DaweiDrumScore.vst3` 文件，重启 MuseScore。

---

## 四、如果 CI 报错

编译日志会直接显示在 Actions 页面里。把红色报错那段发我，我来定位——
Windows 代码是在这台 Mac 上写的，**没有本地编译过**，第一次跑 CI 很可能
需要一两轮修错，这是正常的。

常见的第一轮问题类型：

| 现象 | 多半是 |
|---|---|
| `error C2065: 未声明的标识符` | 某个 Win32 头/宏漏了 |
| `error LNK2019: 无法解析的外部符号` | 某个系统库没链接（`mfplat` / `dbghelp` …） |
| `CMake Error: 找不到 sdk 目标` | SDK 版本差异，改用兜底目标定义 |
| Actions 找不到 `*.vst3` | 产物路径变了，调整上传路径 |

---

## 五、后续可选：做成 Windows 安装器

CI 跑通、`DaweiDrumScore.vst3` 能用之后，可以再加一个 Inno Setup 安装包
（双击 → 下一步 → 自动复制到 `Common Files\VST3`），做到和 Mac 的 PKG 一样傻瓜。
这一步要等插件本体在 Windows 上验证可用再做，避免白做。

---

## 附：两端构建方式对照

| | macOS | Windows |
|---|---|---|
| 本地构建 | `./build.sh`（含 6 项自动验证） | 不需要 |
| 云端构建 | 不需要 | GitHub Actions（`.github/workflows/build-windows.yml`） |
| 解码后端 | AVFoundation | Media Foundation |
| 界面 | AppKit | Win32 |
| 崩溃取证 | signal + backtrace | SEH + DbgHelp |
| 安装包 | `dist/*.pkg`（`./make_pkg.sh`） | 直接放 `Common Files\VST3`（后续可做 Inno Setup） |
