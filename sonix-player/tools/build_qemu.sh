#!/bin/sh
# Builds the statically linked qemu user-mode emulator for the V1's mipsel
# host, guest = arm (the Android armeabi-v7a ABI both Apple Music
# generations ship in -- 3.6.0 for the auth set, 6.5.3 for the decrypt set).
#
# QEMU 7.2 is pinned deliberately: it is the last series with the tcg/mips
# host backend (removed in 8.0), and 32-bit guest on 32-bit host is the only
# shape qemu-user supports here.
#
#   tools/build_qemu.sh <qemu-7.2-source-dir>
#
# Output: build/ampguest/qemu-arm (static, for /mnt/sd_0/.ampguest/)
set -e
QEMU_SRC="$1"
if [ ! -f "$QEMU_SRC/configure" ]; then
    echo "usage: $0 <qemu-7.2-source-dir>" >&2
    echo "get it: curl -LO https://download.qemu.org/qemu-7.2.0.tar.xz" >&2
    exit 1
fi
HERE=$(cd "$(dirname "$0")/.." && pwd)
TOOLCHAIN="$HERE/rockbox-toolchain/bin"
if [ ! -x "$TOOLCHAIN/mipsel-rockbox-linux-gnu-gcc" ]; then
    echo "rockbox mipsel toolchain not built yet (rockboxdev/rockboxdev.sh)" >&2
    exit 1
fi

BUILD="$QEMU_SRC/build-mipsel"
mkdir -p "$BUILD"
cd "$BUILD"
../configure \
    --static \
    --enable-user \
    --disable-system \
    --disable-tools \
    --disable-docs \
    --disable-guest-agent \
    --disable-slirp \
    --disable-capstone \
    --disable-fdt \
    --disable-seccomp \
    --disable-bochs \
    --disable-cloop \
    --disable-dmg \
    --disable-parallels \
    --disable-qed \
    --disable-vvfat \
    --disable-vdi \
    --disable-vpc \
    --disable-qom-cast-debug \
    --target-list=arm-linux-user \
    --cross-prefix="$TOOLCHAIN/mipsel-rockbox-linux-gnu-" \
    --extra-cflags="-O2 -ffunction-sections -fdata-sections" \
    --extra-ldflags="-Wl,--gc-sections"

make -j"$(nproc)" qemu-arm
armsize=$(stat -c%s arm-linux-user/qemu-arm)
echo "qemu-arm: $armsize bytes"
mkdir -p "$HERE/build/ampguest"
cp arm-linux-user/qemu-arm "$HERE/build/ampguest/qemu-arm"
echo "placed: $HERE/build/ampguest/qemu-arm"
