#!/bin/sh
# Builds the wrapper-arm daemon for an Android target with the user's clang
# and the NDK sysroot. The NDK's own prebuilt drivers are x86_64 binaries
# and will not run on an aarch64 development host -- the target triple +
# sysroot is all we need from the NDK, which any native clang accepts.
#
#   ANDROID_NDK_HOME=/path/to/ndk tools/../build.sh [arm|i386]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TARGET="${1:-i386}"
NDK="${ANDROID_NDK_HOME:-/tmp/android-ndk-r27c}"
SYSROOT="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

if [ ! -d "$SYSROOT" ]; then
    echo "NDK sysroot not found at $SYSROOT" >&2
    exit 1
fi

case "$TARGET" in
    i386) TRIPLE="i686-linux-android30"; ARCH="i686" ;;
    arm) TRIPLE="armv7a-linux-androideabi30"; ARCH="armv7-a" ;;
    *) echo "target must be i386 or arm" >&2; exit 1 ;;
esac

BUILTINS=$(ls "$NDK"/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/*/lib/linux/libclang_rt.builtins-"$([ "$TARGET" = i386 ] && echo i686 || echo arm)"-android.a 2>/dev/null | head -1)
if [ -z "$BUILTINS" ]; then
    echo "compiler-rt builtins for $TARGET not found in the NDK" >&2
    exit 1
fi

FLAGS="--target=$TRIPLE --sysroot=$SYSROOT -march=$ARCH -O2 -Wall -Wextra -Werror -std=c++17 -Isrc"
mkdir -p build
OBJS=""
for f in main ipc apple/loader apple/decrypt apple/runtime apple/guestelf apple/cxa_type_match; do
    obj="build/$(basename "$f").o"
    clang++ $FLAGS -c "src/$f.cpp" -o "$obj"
    OBJS="$OBJS $obj"
done

# Static archives only pull members that something references; the Apple
# libs loaded at runtime need these even though nothing in the daemon does.
# tools/force_symbols.txt is the precomputed list: Apple x86 UNDEF symbols
# that the NDK static archives define (see tools/gen_forces.py). Symbols the
# archives cannot provide (CF*, ICU*, xml*, dispatch, blocks, C++ runtime)
# are resolved at guest-load time across the loaded Apple libs instead.
FORCE_FILE="$HERE/tools/force_symbols.txt"
if [ ! -s "$FORCE_FILE" ]; then
    echo "force list missing; regenerating from Apple x86 libs..." >&2
    python3 "$HERE/tools/gen_forces.py" > "$FORCE_FILE"
fi
FORCE_SYMBOLS=$(tail -1 "$FORCE_FILE")

clang++ --target="$TRIPLE" --sysroot="$SYSROOT" -march="$ARCH" -O2 -std=c++17 -Isrc \
    -nodefaultlibs $OBJS $FORCE_SYMBOLS -o build/ampdaemon \
    -L"$SYSROOT/usr/lib/${TRIPLE%%-*}-linux-android/30" \
    -L"$SYSROOT/usr/lib/${TRIPLE%%-*}-linux-android" \
    -llog -lc++_shared -lc++abi -lm -lc -ldl "$BUILTINS"

echo "built: build/ampdaemon ($(stat -c%s build/ampdaemon) bytes)"

# The guest build: statically linked (no Android linker under qemu), so
# every libc symbol the Apple libs will look up at runtime must be forced
# into the image now.
echo "building static (guest) variant..."
UNWIND_DIR="$NDK/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/18/lib/linux"
UNWIND_PATH="$UNWIND_DIR/$([ "$TARGET" = i386 ] && echo i386 || echo arm)/libunwind.a"
clang++ --target="$TRIPLE" --sysroot="$SYSROOT" -march="$ARCH" -O2 -std=c++17 -Isrc \
    -static -nodefaultlibs $OBJS $FORCE_SYMBOLS -o build/ampdaemon-static \
    -L"$SYSROOT/usr/lib/${TRIPLE%%-*}-linux-android/30" \
    -lc++_static -lc++abi -lm -lc -ldl \
    "$SYSROOT/usr/lib/arm-linux-androideabi/libz.a" "$UNWIND_PATH" "$BUILTINS"
echo "built: build/ampdaemon-static ($(stat -c%s build/ampdaemon-static) bytes)"
