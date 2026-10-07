#!/bin/sh
#==============================================================================
# 大伟鼓谱 · MuseScore 音频播放器 —— Linux 安装脚本
#
# 把 .vst3 装到用户级 VST3 目录（~/.vst3），不需要 sudo，
# 也不影响系统里其它用户。
#
# 用法：解压本压缩包后，在本目录执行：
#     ./install.sh
#==============================================================================
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BUNDLE="$HERE/DaweiDrumScore.vst3"

if [ ! -d "$BUNDLE" ]; then
    echo "✗ 同目录下找不到 DaweiDrumScore.vst3"
    echo "  请确认已完整解压压缩包，并在解压出的目录里执行本脚本。"
    exit 1
fi

DEST="$HOME/.vst3"
mkdir -p "$DEST"

# 先删旧版本，避免残留文件混在一起
rm -rf "$DEST/DaweiDrumScore.vst3"
cp -R "$BUNDLE" "$DEST/"

echo "✓ 已安装到：$DEST/DaweiDrumScore.vst3"
echo ""
echo "下一步："
echo "  1. 重启 MuseScore"
echo "  2. 混音器 → 任一轨道 Sound 列 → 选择「大伟鼓谱MuseScore音频播放器」"
echo ""
echo "说明："
echo "  · 若 MuseScore 里看不到插件，确认其 VST3 搜索路径包含 ~/.vst3"
echo "  · 卸载：删除 $DEST/DaweiDrumScore.vst3 即可"
echo ""
echo "♪ B 站「大伟鼓谱」· 鼓谱 / 教学 / 伴奏持续更新"
