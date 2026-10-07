#!/bin/sh
#==============================================================================
# 大伟鼓谱 · MuseScore 音频播放器 —— Linux 安装脚本（免 root）
#
# 把 .vst3 装到用户级 VST3 目录（~/.vst3），不需要 sudo，
# 也不影响系统里其它用户。~/.vst3 是 VST3 规范定义的搜索路径之一，
# 所有发行版的宿主都认 —— 所以这一个包通吃 Arch / Gentoo / NixOS 等
# 没有 .deb / .rpm 的发行版。
#
# 用法：解压本压缩包后，在本目录执行：
#     ./install.sh
#
# 另外两种装法（装到系统级 /usr/lib/vst3，需要管理员权限）：
#     Debian / Ubuntu / Mint 系： sudo apt install ./DaweiDrumScore-*-Linux-*.deb
#     Fedora / RHEL / openSUSE： sudo dnf install ./DaweiDrumScore-*-Linux-*.rpm
#
# 运行要求：glibc ≥ 2.31（Ubuntu 20.04+ / Debian 11+ / RHEL 9+）
#           插件依赖 libX11 与 libXft（桌面发行版默认都有）
# 支持格式：MP3 / WAV / FLAC
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
echo "  1. 重启 MuseScore 4"
echo "  2. 混音器 → 任一轨道 Sound 列 → 选择「大伟鼓谱MuseScore音频播放器」"
echo ""
echo "说明："
echo "  · 若 MuseScore 里看不到插件，确认其 VST3 搜索路径包含 ~/.vst3"
echo "  · 卸载：删除 $DEST/DaweiDrumScore.vst3 即可"
echo "  · 支持格式：MP3 / WAV / FLAC"
echo ""
echo "♪ B 站「大伟鼓谱」· 鼓谱 / 教学 / 伴奏持续更新"
