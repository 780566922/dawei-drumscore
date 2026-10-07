#!/bin/sh
#==============================================================================
# make_packages.sh —— 把 Linux 版 VST3 bundle 打成三种发行版安装包
#
# 用法：
#   ./make_packages.sh <DaweiDrumScore.vst3 路径> <版本号> <输出目录>
#
# 例：
#   ./make_packages.sh build/VST3/Release/DaweiDrumScore.vst3 1.0.0 dist
#
# 产物（架构后缀随构建机，如 x86_64 / aarch64）：
#   DaweiDrumScore-<ver>-Linux-<arch>.tar.gz   解压 + install.sh，所有发行版通用
#   DaweiDrumScore-<ver>-Linux-<arch>.deb      Debian / Ubuntu / Mint / Pop!_OS 系
#   DaweiDrumScore-<ver>-Linux-<arch>.rpm      Fedora / RHEL / Rocky / openSUSE 系
#
# 安装位置：
#   .deb / .rpm → /usr/lib/vst3/（VST3 在 Linux 的系统级标准路径，需管理员权限）
#   .tar.gz     → 用户级 ~/.vst3（免 sudo），见同目录 install.sh
#
# 打包工具缺失时跳过对应格式、不报错（方便在只装了 dpkg 或只装了 rpm 的机器上跑）：
#   .deb → dpkg-deb   （Debian 系：apt install dpkg-dev）
#   .rpm → rpmbuild   （Debian 系：apt install rpm；红帽系：dnf install rpm-build）
#==============================================================================
set -eu

BUNDLE=${1:-}
VERSION=${2:-}
OUTDIR=${3:-}

if [ -z "$BUNDLE" ] || [ -z "$VERSION" ] || [ -z "$OUTDIR" ]; then
    echo "用法: $0 <DaweiDrumScore.vst3 路径> <版本号> <输出目录>" >&2
    exit 2
fi
if [ ! -d "$BUNDLE" ]; then
    echo "✗ 找不到 bundle 目录: $BUNDLE" >&2
    exit 2
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUNDLE_NAME=$(basename "$BUNDLE")
mkdir -p "$OUTDIR"
OUTDIR=$(CDPATH= cd -- "$OUTDIR" && pwd)

# --- 架构（同时给出 dpkg 与 rpm 各自的叫法）---
MACHINE=$(uname -m)
case "$MACHINE" in
    x86_64|amd64)  ARCH=x86_64;  DEB_ARCH=amd64 ;;
    aarch64|arm64) ARCH=aarch64; DEB_ARCH=arm64 ;;
    *)             ARCH=$MACHINE; DEB_ARCH=$MACHINE ;;
esac

# 与产物实际要求的 glibc 下限保持一致（构建底座决定了它）：
# 官方在 Ubuntu 20.04（glibc 2.31）里构建 → 依赖写 >= 2.31。
GLIBC_MIN=2.31

BASE="DaweiDrumScore-$VERSION-Linux-$ARCH"

echo "=== 打包 $BUNDLE_NAME  version=$VERSION  arch=$ARCH ==="

#------------------------------------------------------------------ tar.gz
STAGE=$(mktemp -d)
cp -R "$BUNDLE" "$STAGE/"
cp "$HERE/install.sh" "$STAGE/"
chmod +x "$STAGE/install.sh"
tar -C "$STAGE" -czf "$OUTDIR/$BASE.tar.gz" .
rm -rf "$STAGE"
echo "✓ $BASE.tar.gz"

#--------------------------------------------------------------------- .deb
if command -v dpkg-deb >/dev/null 2>&1; then
    DEBROOT=$(mktemp -d)
    mkdir -p "$DEBROOT/DEBIAN" "$DEBROOT/usr/lib/vst3"
    cp -R "$BUNDLE" "$DEBROOT/usr/lib/vst3/"
    ISIZE=$(du -sk "$DEBROOT/usr" | cut -f1)

    cat > "$DEBROOT/DEBIAN/control" <<EOF
Package: dawei-drumscore
Version: $VERSION
Section: sound
Priority: optional
Architecture: $DEB_ARCH
Depends: libc6 (>= $GLIBC_MIN), libstdc++6, libgcc-s1, libx11-6, libxft2
Installed-Size: $ISIZE
Maintainer: Dawei DrumScore <dawei-drumscore@users.noreply.github.com>
Homepage: https://github.com/780566922/dawei-drumscore
Description: 大伟鼓谱 MuseScore 音频播放器（VST3 插件）
 面向 MuseScore 4 的鼓谱音频播放插件：把音频与谱面绑定，播放头随谱面
 位置同步移动，支持缩放、循环、倍速与波形导航。
 .
 支持 MP3 / WAV / FLAC。
 安装到 /usr/lib/vst3/，重启 MuseScore 4 后即可在
 VST -> Dawei DrumScore -> 大伟鼓谱MuseScore音频播放器 中找到。
EOF

    # 装/卸后给一句提示（MuseScore 需要重启才能扫到新插件）
    cat > "$DEBROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
echo "DaweiDrumScore 已安装到 /usr/lib/vst3/ —— 请重启 MuseScore 4 以加载插件。"
exit 0
EOF
    cat > "$DEBROOT/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
echo "正在移除 DaweiDrumScore（VST3 插件）……"
exit 0
EOF
    chmod 755 "$DEBROOT/DEBIAN/postinst" "$DEBROOT/DEBIAN/prerm"

    dpkg-deb --build --root-owner-group "$DEBROOT" "$OUTDIR/$BASE.deb" >/dev/null
    rm -rf "$DEBROOT"
    echo "✓ $BASE.deb"
else
    echo "· 跳过 .deb（未找到 dpkg-deb；Debian 系可 apt install dpkg-dev）"
fi

#--------------------------------------------------------------------- .rpm
if command -v rpmbuild >/dev/null 2>&1; then
    RPMTOP=$(mktemp -d)
    mkdir -p "$RPMTOP/BUILD" "$RPMTOP/RPMS" "$RPMTOP/SOURCES" \
             "$RPMTOP/SPECS" "$RPMTOP/SRPMS"
    cp -R "$BUNDLE" "$RPMTOP/SOURCES/"

    # 不写 Requires：交给 rpmbuild 的自动依赖生成（读 ELF 的 NEEDED 与
    # GLIBC_* 符号版本），它产出的 "libX11.so.6()(64bit)" 这类依赖在各发行版
    # 之间是通用的 —— 手写包名反而会踩到 Fedora(libX11) 与
    # openSUSE(libX11-6) 命名不一致的坑。
    cat > "$RPMTOP/SPECS/dawei-drumscore.spec" <<EOF
%global debug_package %{nil}
%global _build_id_links none

Name:           dawei-drumscore
Version:        $VERSION
Release:        1
Summary:        大伟鼓谱 MuseScore 音频播放器（VST3 插件）
License:        GPL-3.0-or-later
URL:            https://github.com/780566922/dawei-drumscore
BuildArch:      $ARCH

%description
面向 MuseScore 4 的鼓谱音频播放插件：把音频与谱面绑定，播放头随谱面
位置同步移动，支持缩放、循环、倍速与波形导航。

支持 MP3 / WAV / FLAC。
安装到 /usr/lib/vst3/，重启 MuseScore 4 后即可在
VST -> Dawei DrumScore -> 大伟鼓谱MuseScore音频播放器 中找到。

%prep
# 无需预处理：bundle 已构建好，本脚本只做打包

%build
# 无需编译：bundle 由 CMake 产出后原样打包

%install
mkdir -p %{buildroot}/usr/lib/vst3
cp -R %{_sourcedir}/$BUNDLE_NAME %{buildroot}/usr/lib/vst3/

%files
/usr/lib/vst3/$BUNDLE_NAME

%changelog
* $(date '+%a %b %d %Y') Dawei DrumScore - $VERSION-1
- 首次发布
EOF

    rpmbuild --define "_topdir $RPMTOP" -bb \
        "$RPMTOP/SPECS/dawei-drumscore.spec" >/dev/null
    RPMFILE=$(find "$RPMTOP/RPMS" -name '*.rpm' | head -1)
    if [ -z "$RPMFILE" ]; then
        echo "✗ rpmbuild 没产出 .rpm" >&2
        rm -rf "$RPMTOP"
        exit 1
    fi
    cp "$RPMFILE" "$OUTDIR/$BASE.rpm"
    rm -rf "$RPMTOP"
    echo "✓ $BASE.rpm"
else
    echo "· 跳过 .rpm（未找到 rpmbuild；Debian 系可 apt install rpm，红帽系 dnf install rpm-build）"
fi

echo
echo "=== 产物 ==="
ls -la "$OUTDIR"
