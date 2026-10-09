#!/bin/bash
#==============================================================================
# 清理重复插件.command — 修复「MuseScore 里出现两个一模一样的本插件」
#
# 背景：MuseScore 会同时扫描两个 VST3 目录
#     /Library/Audio/Plug-Ins/VST3/     系统域（.pkg 装这里）
#     ~/Library/Audio/Plug-Ins/VST3/    用户域（安装.command 装这里）
#   两处都有同名 bundle 时，混音器 Sound 列会出现两条【名字完全一样】的条目，
#   点开哪一条取决于列表顺序 —— 典型症状是「新版本装了，打开却是旧版」。
#
# 本脚本：列出所有副本 → 让你选保留哪一份 → 删掉其余的。
#   系统域那份是 root 所有，删除时需要输入开机密码；用户域那份不需要。
#
# 双击运行即可。（兼容 macOS 自带的 bash 3.2）
#==============================================================================

BUNDLE="大伟鼓谱MuseScore音频播放器.vst3"
EXEC="DaweiDrumScore"
VST3_REL="Library/Audio/Plug-Ins/VST3"
LEGACY_NAMES="APLAY.vst3 DaweiDrumScore.vst3"

T="$(mktemp -d /tmp/ap_cleanup.XXXXXX)"
LIST="$T/list"      # 每行：来源<TAB>路径
LEGACY="$T/legacy"
: > "$LIST" ; : > "$LEGACY"

echo "=============================================="
echo " 清理重复安装：$BUNDLE"
echo "=============================================="
echo ""
echo "正在扫描所有安装位置..."
echo ""

#---- 1. 扫描所有副本 ---------------------------------------------------------
for HOMEDIR in /Users/*; do
    [ -d "$HOMEDIR" ] || continue
    P="$HOMEDIR/$VST3_REL/$BUNDLE"
    [ -e "$P" ] && printf '用户域\t%s\n' "$P" >> "$LIST"
done
P="/Library/Audio/Plug-Ins/VST3/$BUNDLE"
[ -e "$P" ] && printf '系统域\t%s\n' "$P" >> "$LIST"

for HOMEDIR in /Users/*; do
    for OLD in $LEGACY_NAMES; do
        P="$HOMEDIR/$VST3_REL/$OLD"
        [ -e "$P" ] && printf '%s\n' "$P" >> "$LEGACY"
    done
done
for OLD in $LEGACY_NAMES; do
    P="/Library/Audio/Plug-Ins/VST3/$OLD"
    [ -e "$P" ] && printf '%s\n' "$P" >> "$LEGACY"
done

COUNT=$(wc -l < "$LIST" | tr -d ' ')
LEGACY_COUNT=$(wc -l < "$LEGACY" | tr -d ' ')

if [ "$COUNT" -eq 0 ]; then
    echo "没有找到已安装的插件（是不是还没装？）"
    rm -rf "$T"; read -r -p "按回车关闭..." _ ; exit 0
fi

#---- 2. 报告 -----------------------------------------------------------------
# 含「回到播放头」= 最新版；NSString 字面量在 Mach-O 里以 UTF-16 存储
is_new_build() {
    [ -f "$1/Contents/MacOS/$EXEC" ] || return 1
    command -v python3 >/dev/null 2>&1 || return 1
    python3 -c "
import sys
d = open(sys.argv[1], 'rb').read()
kw = '回到播放头'
sys.exit(0 if (kw.encode('utf-8') in d or kw.encode('utf-16-le') in d) else 1)
" "$1/Contents/MacOS/$EXEC" 2>/dev/null
}

echo "找到 $COUNT 份安装："
echo ""
N=0
while IFS="$(printf '\t')" read -r LABEL P; do
    N=$((N+1))
    VER="?"
    [ -f "$P/Contents/Info.plist" ] && \
        VER=$(plutil -extract CFBundleShortVersionString raw "$P/Contents/Info.plist" 2>/dev/null)
    [ -n "$VER" ] || VER="?"
    if is_new_build "$P"; then FEAT="含最新功能"; else FEAT="旧版（无「回到播放头」）"; fi
    MT=$(stat -f "%Sm" -t "%Y-%m-%d %H:%M" "$P" 2>/dev/null)
    echo "  [$N] $LABEL  版本 $VER  $FEAT  $MT"
    echo "       $P"
done < "$LIST"

if [ "$LEGACY_COUNT" -gt 0 ]; then
    echo ""
    echo "另外发现改名前遗留的旧 bundle（同样会造成重复条目，会一并清除）："
    while IFS= read -r P; do echo "       $P"; done < "$LEGACY"
fi

echo ""
if [ "$COUNT" -eq 1 ] && [ "$LEGACY_COUNT" -eq 0 ]; then
    echo "✓ 只装了一份，没有重复条目问题。"
    rm -rf "$T"; read -r -p "按回车关闭..." _ ; exit 0
fi

#---- 3. 选择保留哪一份 -------------------------------------------------------
echo "请选择【要保留】的那一份（其余会被删除）："
printf '  输入编号 1-%s，或直接回车取消: ' "$COUNT"
read -r KEEP
case "$KEEP" in
    ''|*[!0-9]*) echo "已取消，未做任何改动。"; rm -rf "$T"; read -r -p "按回车关闭..." _ ; exit 0 ;;
esac
if [ "$KEEP" -lt 1 ] || [ "$KEEP" -gt "$COUNT" ]; then
    echo "编号无效，已取消。"; rm -rf "$T"; read -r -p "按回车关闭..." _ ; exit 0
fi

KEEP_PATH=$(sed -n "${KEEP}p" "$LIST" | cut -f2)

echo ""
echo "将保留：$KEEP_PATH"
echo "将要删除："
sed -n "1,\$p" "$LIST" | cut -f2 | while IFS= read -r P; do
    [ "$P" = "$KEEP_PATH" ] && continue
    echo "  ✗ $P"
done
while IFS= read -r P; do echo "  ✗ $P"; done < "$LEGACY"
echo ""
printf '确认执行？（需要输入开机密码）[y/N] '
read -r OK
case "$OK" in y|Y|yes|YES) ;; *) echo "已取消。"; rm -rf "$T"; read -r -p "按回车关闭..." _ ; exit 0 ;; esac

#---- 4. 执行删除 -------------------------------------------------------------
# 待删清单：用户域副本 + 旧名字 + 系统域副本
TODO="$T/todo"
: > "$TODO"
cut -f2 "$LIST" | while IFS= read -r P; do
    [ "$P" = "$KEEP_PATH" ] && continue
    echo "$P" >> "$TODO"
done
cat "$LEGACY" >> "$TODO"

SUDO_FAIL=0
while IFS= read -r P; do
    [ -e "$P" ] || continue
    case "$P" in
        /Library/*)
            sudo rm -rf "$P" || SUDO_FAIL=1
            ;;
        *)
            rm -rf "$P" || { sudo rm -rf "$P" || SUDO_FAIL=1; }
            ;;
    esac
    [ -e "$P" ] && echo "  ⚠️ 未能删除：$P" || echo "  ✓ 已删除：$P"
done < "$TODO"

rm -rf "$T"

echo ""
echo "✓ 清理完成，当前保留："
echo "  $KEEP_PATH"
[ "$SUDO_FAIL" = "1" ] && echo "  （有项目未删成功，可手动执行：sudo rm -rf \"/Library/Audio/Plug-Ins/VST3/$BUNDLE\"）"
echo ""
echo "接下来："
echo "  1. ⌘Q 完全退出 MuseScore（不是关窗口），再重新打开"
echo "  2. 按 F10 打开混音器，Sound 列应该只剩一个条目"
echo ""
echo "如果重启后仍看到失效的旧条目，退出 MuseScore 后执行："
echo "  rm -f \"\$HOME/Library/Application Support/MuseScore/MuseScore4/known_audio_plugins.json\""
echo "（MuseScore 下次启动会自动重建这个文件）"

read -r -p "按回车关闭..." _
