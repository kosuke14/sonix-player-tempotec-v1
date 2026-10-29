#!/bin/sh
# Assembles the FairPlay guest tree for the V1: one squashfs holding
# Android's bionic (from the user's SDK system image), the Apple Music
# arm/i386 libraries of the APK the user supplies, and our decrypt-only
# daemon. Nothing Apple-shipped is committed; this script builds the image
# locally from files the user provides.
#
#   tools/assemble_ampguest.sh <sdk-system.img> <apk-extract-dir>
#
#   <sdk-system.img>  the system.img of an x86 Android image
#                     (system-images;android-30;google_apis;x86), used for
#                     bionic's linker + libc + friends
#   <apk-extract-dir> the output of tools/extract_apkm.sh (its x86/ subdir
#                     is what this uses)
#
# Output: build/ampguest/guest.squashfs  (~30-40 MB)
set -e
SYSIMG="$1"
APKDIR="$2"
if [ ! -f "$SYSIMG" ] || [ ! -d "$APKDIR/x86" ]; then
    echo "usage: $0 <sdk-system.img> <apk-extract-dir>" >&2
    exit 1
fi
command -v debugfs >/dev/null || { echo "debugfs (e2fsprogs) required" >&2; exit 1; }
command -v mksquashfs >/dev/null || { echo "mksquashfs (squashfs-tools) required" >&2; exit 1; }

HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT="$HERE/build/ampguest"
GUEST="$ROOT/rootfs"
rm -rf "$GUEST"
mkdir -p "$GUEST/system/bin" "$GUEST/system/lib" "$GUEST/data/amp/mpl_db" "$GUEST/data/data/com.apple.android.music/files/mpl_db"

# --- bionic from the SDK system image ------------------------------------
# The linker, libc and friends live under /system/lib in the image. Pull
# them out with debugfs (read-only, no root needed).
for f in linker libc.so libm.so libdl.so liblog.so libz.so libstdc++.so \
         libandroid.so libOpenSLES.so libc++.so libc++_shared.so \
         libutils.so libcutils.so libbase.so; do
    if debugfs -R "dump /system/lib/$f $GUEST/system/lib/$f" "$SYSIMG" 2>/dev/null; then
        echo "bionic: $f"
    else
        echo "bionic: $f (not in image; skipped)"
    fi
done
# The linker binary sits in /system/bin in some images.
if [ ! -s "$GUEST/system/lib/linker" ]; then
    debugfs -R "dump /system/bin/linker $GUEST/system/bin/linker" "$SYSIMG" 2>/dev/null && echo "bionic: linker (from /system/bin)"
fi
mv "$GUEST/system/lib/linker" "$GUEST/system/bin/linker" 2>/dev/null || true
chmod 755 "$GUEST/system/bin/linker" 2>/dev/null || true

# --- Apple libraries (6.5.3, x86) -----------------------------------------
# libstoreapi + libFPDIFor3P are not in any DT_NEEDED closure but carry the
# device-binding entry points the FairPlay core requires at startup.
for so in libCoreFP.so libCoreLSKD.so libCoreADI.so libCoreFoundation.so \
          libandroidappmusic.so libmediaplatform.so libdispatch.so \
          libBlocksRuntime.so libdaapkit.so libmedialibrarycore.so \
          libicudata_sv_apple.so libicui18n_sv_apple.so libicuuc_sv_apple.so \
          libxml2.so libc++_shared.so libstoreapi.so libFPDIFor3P.so; do
    if [ -f "$APKDIR/x86/$so" ]; then
        cp "$APKDIR/x86/$so" "$GUEST/system/lib/$so"
    else
        echo "warning: $so missing from the APK extract" >&2
    fi
done

# --- our daemon -----------------------------------------------------------
# The static build is the one the guest actually runs: under qemu-user the
# Android linker dies on /proc/self/maps (it sees the host's maps), so the
# daemon carries its own loader (wrapper-arm/src/apple/guestelf.cpp) and its
# own bionic. The dynamic build is kept for a real-chroot PoC.
for bin in ampdaemon-static ampdaemon; do
    DAEMON="$ROOT/../../wrapper-arm/build/$bin"
    if [ -f "$DAEMON" ]; then
        cp "$DAEMON" "$GUEST/system/bin/$bin"
        chmod 755 "$GUEST/system/bin/$bin"
    else
        echo "warning: wrapper-arm/build/$bin not built yet" >&2
    fi
done
if [ ! -f "$GUEST/system/bin/ampdaemon-static" ]; then
    echo "ampdaemon-static missing: build it with wrapper-arm/build.sh" >&2
    exit 1
fi

mksquashfs "$GUEST" "$ROOT/guest.squashfs" -comp lzo -all-root -no-xattrs -no-progress
ls -la "$ROOT/guest.squashfs"
echo "guest image ready: $ROOT/guest.squashfs"
echo "place on the card under .ampguest/ together with qemu-i386 (tools/build_qemu.sh)"
