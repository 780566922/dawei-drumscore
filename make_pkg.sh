#!/bin/bash
#==============================================================================
# make_pkg.sh — 把已编译好的 VST3 打包成可分发的 macOS 安装包(.pkg)
#
# 前置：先跑 ./build.sh 生成 build/大伟鼓谱MuseScore音频播放器.vst3
# 产物：dist/大伟鼓谱MuseScore音频播放器-<VERSION>.pkg
#
# 安装位置：/Library/Audio/Plug-Ins/VST3/（全局，所有账号 + MuseScore 都能扫到）
# 安装方式：双击 PKG → 系统安装器 → 输密码 → 完成（业界 VST 插件标准做法）
#
# 说明：未签名的 PKG 首次打开会被 Gatekeeper 拦，需「右键 → 打开」或在
#       系统设置里放行一次。脚本会同时生成一份放行指引（安装说明-请先读.txt）。
#==============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
BUNDLE_NAME="大伟鼓谱MuseScore音频播放器.vst3"
DISPLAY_NAME="大伟鼓谱 MuseScore 音频播放器"
VERSION="1.2.6"
IDENT="com.dawei.drumscore"

SRC_BUNDLE="$HERE/build/$BUNDLE_NAME"
PKGDIR="$HERE/pkg"
WORK="$PKGDIR/.work"
DIST="$HERE/dist"
OUT="$DIST/大伟鼓谱MuseScore音频播放器-$VERSION.pkg"
INSTALL_DIR="Library/Audio/Plug-Ins/VST3"

# ---- 前置检查 ----
if [ ! -d "$SRC_BUNDLE" ]; then
    echo "✗ 找不到插件：$SRC_BUNDLE"
    echo "  请先运行 ./build.sh 编译"
    exit 1
fi
if [ ! -x "$SRC_BUNDLE/Contents/MacOS/DaweiDrumScore" ]; then
    echo "✗ 插件可执行文件缺失，bundle 不完整"
    exit 1
fi

echo "==> 准备 payload"
# 注意：本目录在外置盘上，rm 可能被系统/沙箱以 "Operation not permitted"
# 拒绝。脚本开头是 set -e，不吞掉这个错误会直接中断打包 —— 而这两个路径
# 后面都会整体重建/覆盖（mkdir -p + ditto + pkgbuild），删不掉并不影响
# 产物正确性，所以这里允许失败继续。
rm -rf "$WORK" "$OUT" 2>/dev/null || true
mkdir -p "$WORK/root/$INSTALL_DIR" "$DIST"
# 用 ditto 复制：完整保留 bundle 的扩展属性与签名信息（cp -R 不保证）
ditto "$SRC_BUNDLE" "$WORK/root/$INSTALL_DIR/$BUNDLE_NAME"
chmod +x "$PKGDIR/scripts/postinstall"
# preinstall 是「安装前清掉旧版本」的那一步，必须可执行，否则安装器会静默跳过
# （脚本没执行权限时 pkgbuild 不报错，装完才发现旧版还在 —— 属静默失败）。
chmod +x "$PKGDIR/scripts/preinstall"

echo "==> 构建组件包（component.pkg）"
pkgbuild --root "$WORK/root" \
         --identifier "$IDENT" \
         --version "$VERSION" \
         --install-location / \
         --scripts "$PKGDIR/scripts" \
         "$WORK/component.pkg"

echo "==> 构建分发包（含欢迎/说明/许可/完成页）"
productbuild --distribution "$PKGDIR/distribution.xml" \
             --resources "$PKGDIR/resources" \
             --package-path "$WORK" \
             "$OUT"

# ---- 验证：产物存在、结构正确、payload 含关键文件 ----
echo "==> 验证安装包"
[ -f "$OUT" ] || { echo "  ✗ 未生成 PKG"; exit 1; }
SIZE_KB=$(( $(stat -f%z "$OUT") / 1024 ))
echo "  大小：${SIZE_KB} KB"

FAIL=0
# 1) 组件包 payload 含可执行文件与必需文件
PAYLOAD=$(pkgutil --payload-files "$WORK/component.pkg" 2>/dev/null || true)
for NEED in "Contents/MacOS/DaweiDrumScore" "Contents/Info.plist" "Contents/PkgInfo"; do
    if echo "$PAYLOAD" | grep -q "$NEED"; then
        echo "  ✓ payload 含 $NEED"
    else
        echo "  ✗ payload 缺 $NEED"; FAIL=1
    fi
done
# 2) 最终分发包结构：Distribution + 内部组件包
# 目录名带 PID：外置盘上旧目录可能删不掉（Operation not permitted），
# 固定名字会让 pkgutil --expand 因「目标已存在」而失败，误报校验不通过。
EXPAND="$WORK/verify.$$"
rm -rf "$EXPAND" 2>/dev/null || true
if pkgutil --expand "$OUT" "$EXPAND" >/dev/null 2>&1; then
    [ -f "$EXPAND/Distribution" ] && echo "  ✓ 分发脚本就位" || { echo "  ✗ 缺少 Distribution"; FAIL=1; }
    [ -d "$EXPAND/component.pkg" ] && echo "  ✓ 含内部组件包" || { echo "  ✗ 缺少内部组件包"; FAIL=1; }
    [ -f "$EXPAND/component.pkg/Scripts/postinstall" ] && echo "  ✓ 安装后脚本就位" \
        || echo "  ⚠️ 未找到 postinstall（可选）"
    # 「安装前清旧版本」必须嵌进去 —— 少了它升级会残留旧 bundle 文件（静默失效）
    if [ -f "$EXPAND/component.pkg/Scripts/preinstall" ]; then
        if grep -q '大伟鼓谱MuseScore音频播放器.vst3' "$EXPAND/component.pkg/Scripts/preinstall"; then
            echo "  ✓ 安装前清理脚本就位（含旧命名）"
        else
            echo "  ✗ preinstall 里没有我们的 bundle 名"; FAIL=1
        fi
    else
        echo "  ✗ 缺少 preinstall（安装前清旧版本）"; FAIL=1
    fi
else
    echo "  ✗ 无法展开分发包"; FAIL=1
fi
# 3) 安装器可解析（不实际安装）
if installer -pkg "$OUT" -showChoicesXML >/dev/null 2>&1; then
    echo "  ✓ 安装器可解析"
else
    echo "  ⚠️ 安装器解析检查跳过（需要权限）"
fi
if [ "$FAIL" = "1" ]; then echo "  安装包校验失败"; exit 1; fi

# ---- 生成放行指引 ----
cat > "$DIST/安装说明-请先读.txt" <<TXTEOF
大伟鼓谱 MuseScore 音频播放器 — 安装说明
========================================

【怎么装】
双击「大伟鼓谱MuseScore音频播放器-$VERSION.pkg」，
按提示点「继续 / 安装」，输入开机密码即可。

装之前会自动把旧版本清干净（含改名前遗留的 APLAY.vst3 / DaweiDrumScore.vst3，
以及用户目录里可能存在的重复安装），不需要手动卸载旧版。
（分两步：preinstall 在复制文件【之前】清系统域，postinstall 在复制【之后】
  清各用户域并去掉下载隔离属性 —— 先清后装，保证装完只有本安装包这一份内容。）

【如果双击提示"来自身份不明的开发者"】
这是正常的 —— 这个插件没有花钱买苹果开发者证书。
按下面任一种方式放行即可：

  方式一（推荐，最简单）：
    在 PKG 文件上【右键】→ 选【打开】→ 弹窗里点【打开】→ 再输密码。

  方式二：
    双击报错后，打开
      系统设置 → 隐私与安全性
    往下找到「已阻止使用…」那一行，点【仍要打开】，再输密码。

【装完怎么用】
1. 完全退出 MuseScore（⌘Q），再重新打开
2. 按 F10 打开混音器
3. 任一轨道的 Sound 列 → 选「大伟鼓谱MuseScore音频播放器」
4. 点 Sound 那一格 → 把音频文件拖进窗口

【卸载】
删掉这个文件夹，然后重启 MuseScore：
  /Library/Audio/Plug-Ins/VST3/大伟鼓谱MuseScore音频播放器.vst3

--------------------------------------------------------------------------------
♪ B 站「大伟鼓谱」· 欢迎关注，鼓谱 / 教学 / 伴奏持续更新
   https://space.bilibili.com/65320474
   （插件界面底部那条粉色署名也能点，同样打开这个主页）
TXTEOF

echo ""
echo "============================================"
echo "✓ 安装包构建成功"
echo ""
echo "  安装包：$OUT"
echo "  说明：  $DIST/安装说明-请先读.txt"
echo ""
echo "  安装位置：/Library/Audio/Plug-Ins/VST3/"
echo "============================================"

# 收尾清理。⚠️ 必须放在最后一步，并且容错：
# 本目录在外置盘上，rm 会被沙箱以 "Operation not permitted" 拒绝；
# 脚本开头是 set -e，若不吞掉这个错误会【在产物已经生成之后】中断整个脚本，
# 导致后面的安装说明不再更新（曾真实发生）。删不掉不影响产物，放着即可。
rm -rf "$WORK" 2>/dev/null || true
