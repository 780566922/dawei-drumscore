#!/bin/bash
#==============================================================================
# 安装.command — 双击即可安装「大伟鼓谱 MuseScore 音频播放器」到 MuseScore
#
# 放在桌面上，双击运行。不需要输入密码（装到用户目录）。
#==============================================================================
set -e

BUNDLE_NAME="大伟鼓谱MuseScore音频播放器.vst3"
EXEC_NAME="DaweiDrumScore"

HERE="$(cd "$(dirname "$0")" && pwd)"
# 兼容多种位置：脚本在项目根目录，或被复制到别处
if [ -d "$HERE/build/$BUNDLE_NAME" ]; then
  PLUGIN="$HERE/build/$BUNDLE_NAME"
elif [ -d "$HERE/../build/$BUNDLE_NAME" ]; then
  PLUGIN="$(cd "$HERE/.." && pwd)/build/$BUNDLE_NAME"
elif [ -d "$HERE/vst3-audio-player/build/$BUNDLE_NAME" ]; then
  PLUGIN="$HERE/vst3-audio-player/build/$BUNDLE_NAME"
else
  echo "找不到 $BUNDLE_NAME，请先运行 build.sh 编译"
  read -r -p "按回车关闭..." _
  exit 1
fi

DEST="$HOME/Library/Audio/Plug-Ins/VST3"
mkdir -p "$DEST"

echo "正在安装 大伟鼓谱 MuseScore 音频播放器..."
echo "  从: $PLUGIN"
echo "  到: $DEST"

# 先移除旧版本（含改名前遗留的 APLAY.vst3）
if [ -d "$DEST/$BUNDLE_NAME" ]; then
  rm -rf "$DEST/$BUNDLE_NAME"
  echo "  已移除旧版本"
fi
if [ -d "$DEST/APLAY.vst3" ]; then
  rm -rf "$DEST/APLAY.vst3"
  echo "  已移除改名前遗留的 APLAY.vst3"
fi
# 清掉上一版的 ASCII bundle 名（当前菜单第二级取 bundle 文件名，已改回中文）
if [ -d "$DEST/DaweiDrumScore.vst3" ]; then
  rm -rf "$DEST/DaweiDrumScore.vst3"
  echo "  已移除上一版英文名版本"
fi

cp -R "$PLUGIN" "$DEST/"

if [ -f "$DEST/$BUNDLE_NAME/Contents/MacOS/$EXEC_NAME" ] && [ -f "$DEST/$BUNDLE_NAME/Contents/Info.plist" ]; then
  echo ""
  echo "✓ 安装成功"
  echo ""
  echo "接下来："
  echo "  1. 完全退出 MuseScore（⌘Q）"
  echo "  2. 重新打开 MuseScore"
  echo "  3. 打开乐谱 → 按 F10 打开混音器"
  echo "  4. 找一个不用的乐器轨道，Sound 列选择「大伟鼓谱MuseScore音频播放器」"
  echo "  5. 点 Sound 那一格打开界面，把音频拖进去"
  echo ""
else
  echo "✗ 安装失败：文件未正确复制"
fi

read -r -p "按回车关闭..." _
