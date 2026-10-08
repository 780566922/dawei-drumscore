#!/bin/bash
# ==============================================================================
#  macOS 音频解码能力探针
#
#  回答同一个问题：本机的 AudioToolbox / AVFoundation 到底能不能解
#  Ogg Vorbis / Opus？
#
#  用法：  ./probe.sh            （自动去 ../testdata 和 ../../testdata 找 .ogg）
#          ./probe.sh a.ogg b.ogg （也可以自己指定文件）
#
#  为什么要有这个：Windows 用 Media Foundation，macOS 用 AVFoundation，
#  两边的解码器集合【不一样】，同一份 Ogg 文件在两端的结局可能不同。
#  插件里走的正是 AVAudioFile，所以这里也必须用 AVAudioFile 测，
#  不能用 afconvert —— 它可能走另一条代码路径。
# ==============================================================================
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/macprobe.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

echo "======================================================================"
echo " macOS 音频解码能力探针"
echo " $(date '+%Y-%m-%d %H:%M:%S')"
echo "======================================================================"
echo
echo "[1/4] 系统环境"
sw_vers | sed 's/^/  /'
echo "  架构: $(uname -m)"
echo

# ---------------------------------------------------------------- 找测试文件
FILES=()
if [ "$#" -gt 0 ]; then
    for f in "$@"; do [ -f "$f" ] && FILES+=("$f"); done
else
    for d in "$DIR" "$DIR/../testdata" "$DIR/../../testdata"; do
        if [ -d "$d" ]; then
            while IFS= read -r f; do FILES+=("$f"); done < <(find "$d" -maxdepth 1 -name '*.ogg' 2>/dev/null | sort)
        fi
    done
fi

echo "[2/4] 容器识别（afinfo，只读文件头，不解码）"
if [ "${#FILES[@]}" -eq 0 ]; then
    echo "  没找到 .ogg 测试文件，请在项目根目录运行，或手动传入文件路径。"
else
    for f in "${FILES[@]}"; do
        info="$(afinfo "$f" 2>&1 | grep -E 'File type ID|Data format' | head -2 | tr '\n' ' ')"
        if [ -n "$info" ]; then
            echo "  [认得] $(basename "$f")"
            echo "         $info"
        else
            echo "  [不认] $(basename "$f")  -> $(afinfo "$f" 2>&1 | head -1)"
        fi
    done
fi
echo

# --------------------------------------------------- 真解码：AVAudioFile 实测
echo "[3/4] 实解码（AVAudioFile，与插件用的完全同一套 API）"

cat > "$WORK/probe.mm" <<'OBJC'
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include <cstdio>

static int tryFile(const char* path) {
    @autoreleasepool {
        NSError* e = nil;
        NSString* nsPath = [NSString stringWithUTF8String:path];
        AVAudioFile* f = [[AVAudioFile alloc] initForReading:[NSURL fileURLWithPath:nsPath]
                                                commonFormat:AVAudioPCMFormatFloat32
                                                 interleaved:NO
                                                      error:&e];
        if (!f) {
            std::printf("  [不能解] %s\n           原因: %s\n", path,
                        e.localizedDescription ? e.localizedDescription.UTF8String : "(nil)");
            return 1;
        }
        AVAudioFormat* fmt = f.processingFormat;
        std::printf("  [能解]   %s\n", path);
        std::printf("           %.0f Hz, %u ch, length=%lld frames\n",
                    fmt.sampleRate, (unsigned)fmt.channelCount, (long long)f.length);
        if (f.length == 0)
            std::printf("           !! length 报 0 —— 插件里那句 totalFrames==0 检查会把它当成空文件拒掉\n");

        AVAudioPCMBuffer* buf = [[AVAudioPCMBuffer alloc] initWithPCMFormat:fmt frameCapacity:4096];
        NSError* re = nil;
        if (![f readIntoBuffer:buf error:&re]) {
            std::printf("           !! 读取失败: %s\n", re.localizedDescription.UTF8String);
            return 1;
        }
        std::printf("           实际读出 %u 帧 PCM\n", (unsigned)buf.frameLength);
        return 0;
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) tryFile(argv[i]);
    return 0;
}
OBJC

SDK="$(xcrun --show-sdk-path 2>/dev/null)"
if [ -z "$SDK" ]; then
    echo "  找不到 macOS SDK（需要 Xcode 或 Command Line Tools），跳过实解码测试。"
elif [ "${#FILES[@]}" -eq 0 ]; then
    echo "  没有测试文件，跳过。"
elif clang++ -fobjc-arc -fmodules -isysroot "$SDK" \
        -framework AVFoundation -framework Foundation \
        -o "$WORK/probe" "$WORK/probe.mm" 2>"$WORK/build.log"; then
    "$WORK/probe" "${FILES[@]}"
else
    echo "  编译探针失败："
    sed 's/^/    /' "$WORK/build.log" | head -10
fi
echo

# ------------------------------------------------- 顺带看有没有可 dlopen 的库
echo "[4/4] 第三方解码库（将来若要 dlopen 探测，这些是候选）"
found_any=0
for d in /opt/homebrew/lib /usr/local/lib /Library/Frameworks; do
    [ -d "$d" ] || continue
    hits="$(ls -1 "$d" 2>/dev/null | grep -Ei 'libvorbisfile|libopusfile|libavcodec' | head -6)"
    if [ -n "$hits" ]; then
        found_any=1
        echo "  $d:"
        echo "$hits" | sed 's/^/    /'
    fi
done
[ -d /Applications/VLC.app ] && { found_any=1; echo "  /Applications/VLC.app 已安装（内部自带 Vorbis/Opus 解码器）"; }
[ "$found_any" -eq 0 ] && echo "  没有任何第三方解码库（系统域 /usr/lib 里也从来没有 libvorbis）"
echo
echo "======================================================================"
echo " 说明：macOS 15.4 / Sequoia 起系统才开始带 Ogg 容器的 Vorbis/Opus 支持，"
echo "       更老的系统上只能靠插件自带解码器。"
echo "======================================================================"
