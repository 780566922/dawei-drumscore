#!/bin/bash
#==============================================================================
# build.sh — 编译并打包 大伟鼓谱MuseScore音频播放器.vst3
#
# 用法：./build.sh
# 产物：./build/大伟鼓谱MuseScore音频播放器.vst3
#==============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
# VST3 SDK 路径：可用环境变量覆盖，默认查找 ~/WorkBuddy/vst3sdk
SDK="${VST3_SDK_DIR:-$HOME/WorkBuddy/vst3sdk}"
SRC="$HERE/source"
OUT="$HERE/build"
BUNDLE="$OUT/大伟鼓谱MuseScore音频播放器.vst3"
# 关键：MuseScore 的插件菜单是 `VST → 厂商名 → bundle 文件名`，
# 第二级「插件名」显示的正是【bundle 文件名】（MuseScore 缓存
# known_audio_plugins.json 里的 id 字段 = 文件名去掉 .vst3，实测与 VST3 类名无关）。
# 所以 bundle 文件名用中文 = 菜单第二级显示中文。厂商名（第一级）仍走 ASCII。
EXEC_NAME="DaweiDrumScore"   # bundle 内可执行名保持 ASCII：避免路径层编码问题；
                             # 它不参与菜单显示（菜单第二级取的是 bundle 文件名）

# ---- 检查 SDK ----
if [ ! -d "$SDK/pluginterfaces" ]; then
  echo "✗ 找不到 VST3 SDK：$SDK"
  echo "  请重新下载：https://download.steinberg.net/sdk_downloads/vst-sdk_3.8.1_build-84_2026-08-11.zip"
  exit 1
fi

echo "==> 准备输出目录"
# 不做 rm -rf 全量清理：build 目录里上百个中间文件会触发批量删除保护。
# 所有产物文件名都是固定的（源文件名决定），重新编译直接覆盖即可。
# 仅清理最终 bundle，避免旧文件残留。
rm -rf "$BUNDLE" 2>/dev/null || true
mkdir -p "$BUNDLE/Contents/MacOS"
mkdir -p "$BUNDLE/Contents/Resources"

echo "==> 编译"
CXX=clang++
COMMON="-std=c++17 -O2 -arch x86_64 -arch arm64 -mmacosx-version-min=11.0 -fPIC -I$SRC -I$SDK"
FRAMEWORKS="-framework AVFoundation -framework Foundation -framework AppKit -framework Cocoa"

# 纯 C++ 部分
$CXX $COMMON -c "$SRC/player.cpp" -o "$OUT/player.o"

# ---- Steinberg public.sdk 官方实现（基类 + 工厂 + 工具）----
# 必须编进来：AudioEffect / EditControllerEx1 / CPluginFactory 的实现都在这里。
# 之前手写基类时缺了这些，MuseScore 判定插件不兼容。
SDKLIB="$SDK/public.sdk/source"
SDK_INC="-I$SRC -I$SDK"
SDK_FLAGS="-std=c++17 -O2 -DNDEBUG=1 -mmacosx-version-min=11.0 -fPIC -I$SRC -I$SDK"

# public.sdk 源文件位置不固定（有些在 pluginterfaces/base，有些在 public.sdk/source），
# 用 find 动态定位，避免写死路径导致 "no such file" 中断构建
find_src() { find "$SDK" -name "$1.cpp" 2>/dev/null | head -1; }

for ARCH in x86_64 arm64; do
  for NAME in ustring conststringtable coreiids commoniids funknown iupdatehandler \
              fdebug fstring fbuffer fobject fstreamer fdynlib updatehandler baseiids timer threadchecker flock \
              fdebug fstring fbuffer fobject fstreamer fdynlib updatehandler baseiids timer threadchecker flock \
              vstaudioeffect vsteditcontroller pluginterfacesupport \
              vstcomponent vstcomponentbase vstbus vstinitiids \
              vstunits vstattributes vstprocesscontext vstparameterchanges \
              vstevents vsthostapplication vstplugview pluginview \
              memorystream commonstringconvert vstparameters vsttypes \
              pluginfactory; do
    SRCFILE=$(find_src "$NAME")
    [ -z "$SRCFILE" ] && continue
    $CXX $SDK_FLAGS -arch $ARCH -c "$SRCFILE" -o "$OUT/sdk_${NAME}_$ARCH.o" 2>/dev/null
  done
done

# 合并双架构
SDK_OBJS=""
for OBJ in "$OUT"/sdk_*_x86_64.o; do
  [ -f "$OBJ" ] || continue
  N=$(basename "$OBJ" _x86_64.o)
  lipo -create "$OBJ" "$OUT/${N}_arm64.o" -output "$OUT/${N}.o" 2>/dev/null
  SDK_OBJS="$SDK_OBJS $OUT/${N}.o"
done
echo "  public.sdk 对象: $(echo $SDK_OBJS | wc -w | tr -d ' ') 个"
# 验证器是单架构的，给它 arm64 版本
SDK_OBJS_ARM64=""
for OBJ in "$OUT"/sdk_*_arm64.o; do SDK_OBJS_ARM64="$SDK_OBJS_ARM64 $OBJ"; done
# 验证器是单架构的，给它 arm64 版本
SDK_OBJS_ARM64=""
for OBJ in "$OUT"/sdk_*_arm64.o; do SDK_OBJS_ARM64="$SDK_OBJS_ARM64 $OBJ"; done
if [ -z "$SDK_OBJS" ]; then
  echo "  ✗ public.sdk 编译失败，无法构建"; exit 1
fi

# 注意：iid 实例化已由 public.sdk 的 coreiids/commoniids/vstinitiids 提供，
# 不能再编自己的 iids.cpp，否则 duplicate symbol。

# ObjC++ 部分：universal 需要逐架构编译再 lipo 合并
for ARCH in x86_64 arm64; do
  AFLAGS="-arch $ARCH"
  # audiofile.mm 与 crashguard.mm 用 ARC：它们不手工持有 ObjC 对象，
  # 旧实现是 MRC 且从不 release，每次载入都泄漏一批 AVAudioFile / buffer。
  $CXX -x objective-c++ -std=c++17 -O2 -fobjc-arc $AFLAGS -mmacosx-version-min=11.0 -fPIC \
       -I"$SRC" -I"$SDK" -c "$SRC/audiofile.mm"  -o "$OUT/audiofile_$ARCH.o"  $FRAMEWORKS
  $CXX -x objective-c++ -std=c++17 -O2 -fobjc-arc $AFLAGS -mmacosx-version-min=11.0 -fPIC \
       -I"$SRC" -I"$SDK" -c "$SRC/crashguard.mm" -o "$OUT/crashguard_$ARCH.o" $FRAMEWORKS
  # gui.mm 也改用 ARC。之前是 MRC，结果两个滑块由【自动释放】构造器创建、
  # 既没挂进视图也没人 retain —— autorelease 池一排空就成了野指针，
  # 点『打开音频』或拖入文件时给它发消息直接炸掉宿主。
  # ARC 从根上消除这一类“对象生命周期靠人脑记”的 bug。
  $CXX -x objective-c++ -std=c++17 -O2 -fobjc-arc $AFLAGS -mmacosx-version-min=11.0 -fPIC \
       -I"$SRC" -I"$SDK" -c "$SRC/gui.mm"      -o "$OUT/gui_$ARCH.o"      $FRAMEWORKS
  # aplaysdk 是纯 C++（不含 ObjC），按普通 C++ 编译 —— 与 Windows 端共用同一份源码
  $CXX -std=c++17 -O2 -DNDEBUG=1 $AFLAGS -mmacosx-version-min=11.0 -fPIC \
       -I"$SRC" -I"$SDK" -c "$SRC/aplaysdk.cpp" -o "$OUT/plugin_$ARCH.o" $FRAMEWORKS
  # Ogg Vorbis 解码：三端系统框架都没有 Vorbis 解码器（CoreAudio 没有、
  # Media Foundation 也没有），所以三端共用这一份 stb_vorbis。
  # -w：文件里 #include 了 stb_vorbis.c（约 5600 行第三方 C 代码），
  #     它自带一条无害的 -Wtautological-compare，不屏蔽会刷屏。
  $CXX -std=c++17 -O2 -DNDEBUG=1 -w $AFLAGS -mmacosx-version-min=11.0 -fPIC \
       -I"$SRC" -I"$SDK" -c "$SRC/ogg_vorbis.cpp" -o "$OUT/ogg_vorbis_$ARCH.o"
done

# 合并必须在循环外 —— 放循环内会被下一轮的单架构 .o 覆盖
lipo -create "$OUT/audiofile_x86_64.o"  "$OUT/audiofile_arm64.o"  -output "$OUT/audiofile.o"
lipo -create "$OUT/crashguard_x86_64.o" "$OUT/crashguard_arm64.o" -output "$OUT/crashguard.o"
lipo -create "$OUT/gui_x86_64.o"       "$OUT/gui_arm64.o"       -output "$OUT/gui.o"
lipo -create "$OUT/plugin_x86_64.o"    "$OUT/plugin_arm64.o"    -output "$OUT/plugin.o"
lipo -create "$OUT/ogg_vorbis_x86_64.o" "$OUT/ogg_vorbis_arm64.o" -output "$OUT/ogg_vorbis.o"

echo "==> 链接"
$CXX -bundle -arch x86_64 -arch arm64 -o "$BUNDLE/Contents/MacOS/$EXEC_NAME" \
  "$OUT/player.o" "$OUT/audiofile.o" "$OUT/gui.o" "$OUT/plugin.o" "$OUT/crashguard.o" \
  "$OUT/ogg_vorbis.o" $SDK_OBJS \
  $FRAMEWORKS

echo "==> 生成 Info.plist"
cat > "$BUNDLE/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>            <string>zh_CN</string>
    <key>CFBundleExecutable</key>                  <string>DaweiDrumScore</string>
    <key>CFBundleIdentifier</key>                  <string>com.dawei.drumscore</string>
    <key>CFBundleName</key>                        <string>大伟鼓谱MuseScore音频播放器</string>
    <key>CFBundlePackageType</key>                 <string>BNDL</string>
    <key>CFBundleSignature</key>                   <string>????</string>
    <key>CFBundleShortVersionString</key>          <string>1.2.2</string>
    <key>CFBundleVersion</key>                     <string>1.2.2</string>
    <key>NSHumanReadableCopyright</key>            <string>Free for personal use</string>
</dict>
</plist>
PLIST

# PkgInfo 是标准 macOS bundle 的一部分，VST3 插件都要有。
# 缺了可能导致宿主加载失败（MT-PowerDrumKit 等正常插件都有）。
printf 'BNDL????' > "$BUNDLE/Contents/PkgInfo"

echo "==> 签名（ad-hoc）"
# VST3 必须签名，否则宿主拒绝加载。这里用 ad-hoc 签名，不需要开发者证书。
codesign --force --deep --sign - "$BUNDLE" 2>&1 | sed 's/^/  /' || {
  echo "  ⚠️ 签名失败，但通常仍可加载"
}

echo "==> 验证"
# 1) 检查二进制架构与导出符号
ARCHS=$(lipo -archs "$BUNDLE/Contents/MacOS/$EXEC_NAME" 2>/dev/null)
echo "  架构: $ARCHS"
# 必须是 universal（x86_64 + arm64）—— MuseScore 会跳过非 universal 的插件
case "$ARCHS" in
  *x86_64*arm64*) echo "  ✓ universal 双架构" ;;
  *) echo "  ✗ 需要 universal 双架构（当前: $ARCHS）"; exit 1 ;;
esac
echo "  导出符号（VST3 要求三个，缺一不可）:"
MISSING=""
for SYM in GetPluginFactory bundleEntry bundleExit; do
  if nm -g "$BUNDLE/Contents/MacOS/$EXEC_NAME" 2>/dev/null | grep -qE "_${SYM}$"; then
    echo "    ✓ $SYM"
  else
    echo "    ✗ $SYM 缺失"
    MISSING="$MISSING $SYM"
  fi
done
if [ -n "$MISSING" ]; then
  echo "    → 宿主会报 [1401] Could not create VstModule，插件不会被加载"
  exit 1
fi

# 2) 检查 Info.plist
/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" >/dev/null 2>&1 || {
  echo "    ✗ Info.plist 有问题"
  exit 1
}

# 3) 检查 bundle 结构
if [ -x "$BUNDLE/Contents/MacOS/$EXEC_NAME" ] && [ -f "$BUNDLE/Contents/Info.plist" ] \
   && [ -f "$BUNDLE/Contents/PkgInfo" ]; then
  echo "  ✓ bundle 结构完整"
else
  echo "  ✗ bundle 结构不完整"
  exit 1
fi

# ---- 4. 自动验证（避免交付未验证的二进制）----
# 编译测试程序：输出写进 .log，失败立即中止。
#
# 为什么不用 `| head`：clang 的警告文字写满若干行后 head 会退出并关闭管道，
# clang 随即收到 SIGPIPE 被中途杀掉 —— 产物根本没生成，错误也不显眼。
# 结果是拿着【上一次的旧二进制】跑验证，报出真假不明的“假通过/假失败”。
build_test () {
  local out="$1"; shift
  if ! $CXX "$@" -o "$out" > "$out.build.log" 2>&1; then
    echo "  ✗ 测试程序编译失败: $out"
    grep -E "error" "$out.build.log" | head -5 | sed 's/^/    /'
    exit 1
  fi
}

echo "==> 编译验证器"
build_test "$OUT/validate" -std=c++17 -O1 -arch arm64 "$HERE/validate.cpp" \
           -I"$SDK" $SDK_OBJS_ARM64 $FRAMEWORKS
build_test "$OUT/e2e_test" -std=c++17 -O1 -arch arm64 "$HERE/e2e_test.cpp" \
           -I"$SDK" $SDK_OBJS_ARM64 $FRAMEWORKS
build_test "$OUT/factory_lifecycle_test" -std=c++17 -O1 -arch arm64 \
           "$HERE/factory_lifecycle_test.cpp" -I"$SDK" $SDK_OBJS_ARM64 $FRAMEWORKS

echo "==> 验证 1：VST3 接口生命周期"
if "$OUT/validate" "$BUNDLE/Contents/MacOS/$EXEC_NAME" > "$OUT/validate.log" 2>&1; then
  echo "  ✓ 接口验证通过（$(grep -c PASS "$OUT/validate.log") 项）"
else
  echo "  ✗ 接口验证失败："
  grep FAIL "$OUT/validate.log" | head -5 | sed 's/^/    /'
  exit 1
fi

# 生成测试音频做端到端验证
echo "==> 验证 2：真实音频输出"
TEST_AUDIO="/tmp/aplay_test.mp3"
if command -v ffmpeg >/dev/null 2>&1; then
  # ⚠️ 时长必须【长于】源码里的默认视野（kDefaultViewSpanSec = 8 秒），否则
  #    gui_repro 里「载入后默认视野 = 8 秒」那条断言会退化成「本来就短，全览」，
  #    永远测不到真实行为（原来恰好是 8 秒，正卡在边界上 → 每次都被跳过）。
  ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=20" -ac 2 -ar 44100 \
          -c:a libmp3lame -b:a 192k "$TEST_AUDIO" -y 2>/dev/null
else
  # 没有 ffmpeg 就用系统自带的 afconvert 生成 WAV
  TEST_AUDIO="/tmp/aplay_test.wav"
  say -o /tmp/aplay_src.aiff "test" 2>/dev/null
  afconvert -f WAVE -d LEI16@44100 /tmp/aplay_src.aiff "$TEST_AUDIO" 2>/dev/null || TEST_AUDIO=""
fi

if [ -n "$TEST_AUDIO" ] && [ -f "$TEST_AUDIO" ]; then
  if "$OUT/e2e_test" "$BUNDLE/Contents/MacOS/$EXEC_NAME" "$TEST_AUDIO" > "$OUT/e2e.log" 2>&1; then
    echo "  ✓ 音频验证通过（$(grep -c PASS "$OUT/e2e.log") 项，峰值 $(grep '峰值 =' "$OUT/e2e.log" | head -1 | awk '{print $3}')）"
  else
    echo "  ✗ 音频验证失败："
    grep FAIL "$OUT/e2e.log" | head -5 | sed 's/^/    /'
    exit 1
  fi
else
  echo "  ⚠️ 无法生成测试音频，跳过端到端验证"
fi

# 并发安全：音频线程持续渲染的同时，GUI 线程不停换文件/读波形。
# 这正是「打开音频文件瞬间宿主闪退」的复现场景。
echo "==> 验证 3：并发安全（音频线程 vs 界面线程）"
$CXX -std=c++17 -O1 -arch arm64 -o "$OUT/race_test" \
     "$HERE/race_test.cpp" "$SRC/player.cpp" -I"$SRC" 2>&1 | tail -3
if "$OUT/race_test" > "$OUT/race.log" 2>&1; then
  echo "  ✓ 并发验证通过（$(grep -o '渲染 [0-9]* 块' "$OUT/race.log" | head -1)）"
else
  echo "  ✗ 并发验证失败："
  tail -5 "$OUT/race.log" | sed 's/^/    /'
  exit 1
fi

# 解码路径：不同编码格式在 AVFoundation 里走不同解码器，个别分支会抛
# ObjC 异常——插件里没人接，异常穿到宿主就是闪退。所以逐个格式真跑一遍。
echo "==> 验证 4：多格式解码（含异常捕获）"
# 注意 audiofile.mm 会引用 OGG 那条支路，所以必须一起链上 ogg_vorbis.cpp。
# 这里的失败要当致命处理：以前用 `| tail -3` 不检查退出码，链不上时旧二进制
# 会被拿去跑，结果是「假通过」—— 明明没编出来却报验证通过。
if ! $CXX -x objective-c++ -std=c++17 -O1 -w -fobjc-arc -arch arm64 \
         -I"$SRC" -o "$OUT/decode_probe" \
         "$HERE/decode_probe.mm" "$SRC/audiofile.mm" "$SRC/ogg_vorbis.cpp" \
         -framework AVFoundation -framework Foundation > "$OUT/decode_probe.build.log" 2>&1; then
  echo "  ✗ 解码探针编译失败："
  grep -E "error" "$OUT/decode_probe.build.log" | head -5 | sed 's/^/    /'
  exit 1
fi

PROBE_ARGS=""
# 有损压缩（走音频解码器）
if [ -n "$TEST_AUDIO" ] && [ -f "$TEST_AUDIO" ]; then
  PROBE_ARGS="$PROBE_ARGS '$TEST_AUDIO'"
fi
# 未压缩 PCM（走直读）
if [ -f /tmp/aplay_src.aiff ]; then
  PROBE_ARGS="$PROBE_ARGS /tmp/aplay_src.aiff"
fi
# 系统自带音效，另一个真实样本
if [ -f /System/Library/Sounds/Ping.aiff ]; then
  PROBE_ARGS="$PROBE_ARGS /System/Library/Sounds/Ping.aiff"
fi
# 再补一条 AAC 支路
if command -v afconvert >/dev/null 2>&1 && [ -f /System/Library/Sounds/Ping.aiff ]; then
  if afconvert -f m4af -d aac /System/Library/Sounds/Ping.aiff /tmp/aplay_probe.m4a 2>/dev/null; then
    PROBE_ARGS="$PROBE_ARGS /tmp/aplay_probe.m4a"
  fi
fi

if [ -n "$PROBE_ARGS" ]; then
  eval "\"$OUT/decode_probe\" $PROBE_ARGS" > "$OUT/decode.log" 2>&1 || true
  if grep -qE "NSException|exception escaped|Segmentation|ok=0" "$OUT/decode.log"; then
    echo "  ✗ 解码验证失败："
    grep -E "NSException|exception escaped|ok=0" "$OUT/decode.log" | head -5 | sed 's/^/    /'
    exit 1
  fi
  echo "  ✓ 解码验证通过（$(grep -c 'ok=1' "$OUT/decode.log" || true) 个真实文件，无异常）"
else
  echo "  ⚠️ 无可用测试音频，跳过多格式解码验证"
fi

# 界面生命周期：ObjC 对象所有权错误（把自动释放对象存进裸指针）会在
# 「点开音频 / 拖入文件」时炸掉整个宿主，而且只在事件循环排空 autorelease
# 池之后才暴露 —— 接口验证和解码验证都抓不到。这里用真实的 GUIView，
# 按崩溃日志里的时序把两条路径各跑一遍。
echo "==> 验证 5：界面生命周期（点开音频 / 拖入文件）"
if [ -n "$TEST_AUDIO" ] && [ -f "$TEST_AUDIO" ]; then
  # audiofile_arm64.o 引用 OGG 解码支路 → 这里也得链上 ogg_vorbis（按 arm64 单独编，
  # 因为它跟 gui_repro 一样是单架构的）
  build_test "$OUT/gui_repro" -x objective-c++ -std=c++17 -O1 -DNDEBUG=1 -arch arm64 \
             -mmacosx-version-min=11.0 -I"$SRC" -I"$SDK" \
             "$HERE/gui_repro.mm" \
             -x none "$OUT/gui_arm64.o" "$OUT/player.o" "$OUT/crashguard_arm64.o" \
             "$OUT/audiofile_arm64.o" "$OUT/ogg_vorbis_arm64.o" $SDK_OBJS_ARM64 $FRAMEWORKS

  UI_FAIL=0
  for MODE in load drag grid; do
    if "$OUT/gui_repro" "$MODE" "$TEST_AUDIO" > "$OUT/gui_$MODE.log" 2>&1; then
      echo "  ✓ $MODE 路径正常"
    else
      echo "  ✗ $MODE 路径失败（退出码 $?）—— 界面对象被释放后仍在使用，或网格/交互逻辑出错"
      tail -6 "$OUT/gui_$MODE.log" | sed 's/^/    /'
      UI_FAIL=1
    fi
  done
  [ "$UI_FAIL" = "1" ] && exit 1
else
  echo "  ⚠️ 无测试音频，跳过界面生命周期验证"
fi

# 播放核心单元测试：带符号偏移的静音长度、音频是否被跳过、播完能否重播。
# 这一层跑得最快，也最容易定位 —— 上面几项失败时先看它的输出。
echo "==> 验证 6：播放核心单元测试（带符号偏移）"
build_test "$OUT/test_player" -x objective-c++ -std=c++17 -O1 -w -fobjc-arc -arch arm64 \
           "$HERE/test_player.cpp" "$SRC/player.cpp" "$SRC/audiofile.mm" \
           "$SRC/ogg_vorbis.cpp" -I"$SRC" \
           -framework AVFoundation -framework Foundation
if "$OUT/test_player" > "$OUT/player.log" 2>&1; then
  echo "  ✓ 播放核心验证通过（$(grep -c PASS "$OUT/player.log") 项）"
else
  echo "  ✗ 播放核心验证失败："
  grep FAIL "$OUT/player.log" | head -8 | sed 's/^/    /'
  exit 1
fi

# 工厂引用计数：宿主每次打开插件编辑器都会「索取工厂 → 用 → release」一轮。
# 曾经只在首次 addRef，计数被宿主耗尽 → CPluginFactory 归零即 delete this
# → 函数内 static 变野指针 → 第 3 次打开编辑器时宿主闪退。
# 触发条件很隐蔽（关谱子 → 换工程 → 再开插件），必须由这一项长期守着。
echo "==> 验证 7：工厂引用计数（反复索取/释放，防宿主闪退）"
if "$OUT/factory_lifecycle_test" "$BUNDLE/Contents/MacOS/$EXEC_NAME" \
        > "$OUT/factory.log" 2>&1; then
  echo "  ✓ 工厂生命周期验证通过（$(grep -c PASS "$OUT/factory.log") 项）"
else
  echo "  ✗ 工厂生命周期验证失败（宿主会因此闪退）："
  grep FAIL "$OUT/factory.log" | head -5 | sed 's/^/    /'
  exit 1
fi

# Ogg Vorbis：三端系统框架都没有 Vorbis 解码器（CoreAudio 没有、Media Foundation
# 也没有），这一格完全靠我们自己带进去的 stb_vorbis，所以必须真跑一遍。
# 素材是入库的小文件（testdata/），不依赖机器上装没装 ffmpeg。
echo "==> 验证 8：Ogg Vorbis 解码（含单声道合并 / Opus 提示）"
build_test "$OUT/ogg_vorbis_test" -std=c++17 -O1 -w -arch arm64 \
           "$HERE/ogg_vorbis_test.cpp" "$SRC/ogg_vorbis.cpp" -I"$SRC"
if "$OUT/ogg_vorbis_test" "$HERE/testdata" > "$OUT/ogg.log" 2>&1; then
  echo "  ✓ Ogg Vorbis 验证通过（$(grep -c PASS "$OUT/ogg.log") 项）"
else
  echo "  ✗ Ogg Vorbis 验证失败："
  grep FAIL "$OUT/ogg.log" | head -8 | sed 's/^/    /'
  exit 1
fi

echo ""
echo "============================================"
echo "✓ 构建成功（含自动验证）"
echo ""
echo "  插件位置：$BUNDLE"
echo ""
echo "  安装（复制到系统插件目录）："
echo "    cp -R \"$BUNDLE\" /Library/Audio/Plug-Ins/VST3/"
echo ""
echo "  安装（仅当前用户，无需 sudo）："
echo "    cp -R \"$BUNDLE\" ~/Library/Audio/Plug-Ins/VST3/"
echo ""
echo "  安装后重启 MuseScore，在混音器 Sound 列选择「大伟鼓谱MuseScore音频播放器」"
echo "============================================"
