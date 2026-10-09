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

#------------------------------------------------------------------------------
# ⭐ 跨域重复检查
# MuseScore 会同时扫描 /Library/Audio/Plug-Ins/VST3（系统域）和
# ~/Library/Audio/Plug-Ins/VST3（用户域）。若两处都有同名 bundle，
# 混音器 Sound 列会出现【两个一模一样的条目】，点开哪一个取决于列表顺序 ——
# 典型症状就是「新版本明明装了，打开却发现是旧版」。这里主动提示并清理。
#------------------------------------------------------------------------------
SYSDEST="/Library/Audio/Plug-Ins/VST3"
SYS_FOUND=0
for NAME in "$BUNDLE_NAME" "APLAY.vst3" "DaweiDrumScore.vst3"; do
  if [ -e "$SYSDEST/$NAME" ]; then SYS_FOUND=1; fi
done

if [ "$SYS_FOUND" = "1" ]; then
  echo ""
  echo "⚠️  检测到【系统目录】里也装了一份插件："
  ls -1 "$SYSDEST" 2>/dev/null | sed 's/^/      /'
  echo ""
  echo "    这会让 MuseScore 里出现两个同名插件（其中一个是旧版）。"
  echo "    建议只保留一份 —— 删掉系统目录里的版本，只留用户目录这一份。"
  echo ""
  printf "    现在删除系统目录里的旧版本？（需要输入开机密码）[y/N] "
  read -r SYS_ANS
  case "$SYS_ANS" in
    y|Y|yes|YES)
      if sudo rm -rf "$SYSDEST/$BUNDLE_NAME" "$SYSDEST/APLAY.vst3" "$SYSDEST/DaweiDrumScore.vst3"; then
        echo "    ✓ 系统目录已清理，现在全机只剩用户目录这一份"
      else
        echo "    ✗ 清理未成功（密码错误或已取消）。请稍后手动执行："
        echo "        sudo rm -rf \"$SYSDEST/$BUNDLE_NAME\""
      fi
      ;;
    *)
      echo "    已跳过。若 MuseScore 里仍有两个条目，请手动执行："
      echo "        sudo rm -rf \"$SYSDEST/$BUNDLE_NAME\""
      ;;
  esac
fi

read -r -p "按回车关闭..." _
