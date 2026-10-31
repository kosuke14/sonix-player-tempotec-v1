#!/bin/sh
# Assembles the FairPlay guest tree for the V1 card: a plain directory the
# player's ampmocal spawns qemu-arm from, carrying BOTH Apple generations
# split by process (their ICU singletons fight if resident together):
#
#   .ampguest/
#     qemu-arm                        mipsel-hosted user-mode emulator
#     system/bin/linker               Android's dynamic linker (arm)
#     system/bin/ampdaemon-auth       3.6.0 set: sign-in, tokens, dispatch
#     system/bin/ampdaemon-static     6.5.3 set: FairPlay sample decrypt
#     system/lib/                     the API-24 Android closure the linker needs
#     system/lib36/                   Apple Music 3.6.0 armeabi-v7a libraries
#     system/lib65/                   Apple Music 6.5.3 armeabi-v7a libraries
#
# Nothing Apple-shipped is committed anywhere; this builds the tree locally
# from files the user provides. Two ways to run it:
#
#   tools/assemble_ampguest.sh --from-tree <existing-guest-tree> [dest]
#
#       Copies a tree that was already grown (the development one) and
#       refreshes the daemons from wrapper-arm/build. The fast path; use it
#       when ~/ampguest-build/guest-arm is around.
#
#   tools/assemble_ampguest.sh --from-scratch <sdk-system.img> <apk36-dir> <apk65-dir> [dest]
#
#       Builds everything from sources: the API-24 armeabi-v7a system image
#       (debugfs extraction of the linker + the closure below), the 3.6.0 and
#       6.5.3 APK extracts (tools/extract_apkm.sh output), and the freshly
#       built daemons. The closure list is fixed -- the linker's iterative
#       "library X not found" walk was run once and its answer recorded; it
#       only changes when an Apple generation is upgraded.
#
#   dest defaults to ~/ampguest-card/.ampguest; copy that directory to the
#   card's root afterwards. qemu-arm comes from tools/build_qemu.sh.
set -e

MODE="$1"; shift
HERE=$(cd "$(dirname "$0")/.." && pwd)          # sonix-player/
PROJ=$(cd "$HERE/.." && pwd)                    # the repository root (wrapper-arm/)
DEST="$HOME/ampguest-card/.ampguest"
if [ "$1" = "-h" ] || [ "$1" = "--help" ] || [ -z "$MODE" ]; then
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
fi

# The daemons: the auth one MUST be the API-24 build (the dynamic linker
# comes from the API-24 image), the static one the API-30 build (API 24's
# bionic TLS segment is aligned to 8 and qemu refuses to load it).
refresh_daemons() {
    d="$1"
    if [ -f "$PROJ/wrapper-arm/build/ampdaemon" ]; then
        cp "$PROJ/wrapper-arm/build/ampdaemon" "$d/system/bin/ampdaemon-auth"
        chmod 755 "$d/system/bin/ampdaemon-auth"
    else
        echo "warning: wrapper-arm/build/ampdaemon missing -- build with" >&2
        echo "  (cd wrapper-arm && ANDROID_NDK_HOME=... AMP_API_LEVEL=24 sh build.sh arm)" >&2
    fi
    if [ -f "$PROJ/wrapper-arm/build/ampdaemon-static" ]; then
        cp "$PROJ/wrapper-arm/build/ampdaemon-static" "$d/system/bin/ampdaemon-static"
        chmod 755 "$d/system/bin/ampdaemon-static"
    else
        echo "warning: wrapper-arm/build/ampdaemon-static missing" >&2
    fi
}

case "$MODE" in
--from-tree)
    SRC="$1"; shift
    [ $# -gt 0 ] && DEST="$1"
    [ -d "$SRC/system/lib36" ] || { echo "$SRC does not look like a guest tree" >&2; exit 1; }
    rm -rf "$DEST"
    mkdir -p "$(dirname "$DEST")"
    cp -r "$SRC" "$DEST"
    # The development tree's scratch subdirs are not part of the card's job.
    rm -rf "$DEST/system-img" "$DEST/data"
    refresh_daemons "$DEST"
    echo "tree assembled at $DEST"
    ;;
--from-scratch)
    SYSIMG="$1"; APK36="$2"; APK65="$3"; shift 3
    [ $# -gt 0 ] && DEST="$1"
    [ -f "$SYSIMG" ] && [ -d "$APK36" ] && [ -d "$APK65" ] || {
        sed -n '17,25p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
    command -v debugfs >/dev/null || { echo "debugfs (e2fsprogs) required" >&2; exit 1; }

    rm -rf "$DEST"
    mkdir -p "$DEST/system/bin" "$DEST/system/lib36" "$DEST/system/lib65"

    # The API-24 Android closure: the linker + every library the dynamic
    # linker pulled while walking the 3.6.0 set's DT_NEEDED chains (recorded
    # from a completed closure walk; libmemtrack lives in /lib, not /system).
    CLOSURE="libEGL.so libETC1.so libGLESv1_CM.so libGLESv2.so libLLVM.so \
libOpenSLES.so libRS.so libRS_internal.so libRScpp.so libandroid.so \
libandroid_runtime.so libandroidfw.so libaudioutils.so libbacktrace.so \
libbase.so libbcinfo.so libbinary_parse.so libbinder.so libc++.so \
libc++_shared.so libc.so libcamera_client.so libcamera_metadata.so \
libcrypto.so libcutils.so libdl.so libdng_sdk.so libdrmframework.so \
libeffects.so libexpat.so libft2.so libgui.so libhardware.so \
libhardware_legacy.so libharfbuzz_ng.so libhwui.so libicui18n.so \
libicuuc.so libimage_type_recognition.so libimg_utils.so libinput.so \
libinputflinger.so libjpeg.so liblog.so liblzma.so libm.so libmedia.so \
libmediautils.so libmemtrack.so libmemunreachable.so libminikin.so \
libnativebridge.so libnativehelper.so libnativeloader.so libnbaio.so \
libnetd_client.so libnetutils.so libopus.so libpackagelistparser.so \
libpcre.so libpdfium.so libpiex.so libpng.so libpowermanager.so \
libprocessgroup.so libprotobuf-cpp-lite.so libradio.so libradio_metadata.so \
libselinux.so libskia.so libsonivox.so libsoundtrigger.so \
libspeexresampler.so libsqlite.so libssl.so libstagefright.so \
libstagefright_avc_common.so libstagefright_enc_common.so \
libstagefright_foundation.so libstagefright_http_support.so \
libstagefright_omx.so libstagefright_yuv.so libstdc++.so libsync.so \
libtiff_directory.so libui.so libunwind.so libusbhost.so libutils.so \
libvorbisidec.so libvulkan.so libwilhelm.so libz.so"
    for f in $CLOSURE; do
        debugfs -R "dump /system/lib/$f $DEST/system/lib/$f" "$SYSIMG" 2>/dev/null ||
        debugfs -R "dump /lib/$f $DEST/system/lib/$f" "$SYSIMG" 2>/dev/null || {
            echo "warning: $f not in the image" >&2; rm -f "$DEST/system/lib/$f"; }
    done
    debugfs -R "dump /system/bin/linker $DEST/system/bin/linker" "$SYSIMG" 2>/dev/null || {
        echo "the linker is not in the image; is this an armeabi-v7a API-24 image?" >&2
        exit 1; }
    chmod 755 "$DEST/system/bin/linker"

    # The Apple generations, verbatim from the user's own APK extracts.
    for f in "$APK36"/*.so; do cp "$f" "$DEST/system/lib36/"; done
    for f in "$APK65"/*.so; do cp "$f" "$DEST/system/lib65/"; done
    echo "lib36: $(ls "$DEST/system/lib36" | wc -l) libs, lib65: $(ls "$DEST/system/lib65" | wc -l) libs"

    refresh_daemons "$DEST"
    echo "tree assembled at $DEST"
    ;;
*)
    echo "unknown mode: $MODE (use --from-tree or --from-scratch)" >&2
    exit 1
    ;;
esac

# The emulator, if built. Without it the tree cannot run; build_qemu.sh
# makes it from the rockbox mipsel toolchain.
if [ -f "$HERE/build/ampguest/qemu-arm" ]; then
    cp "$HERE/build/ampguest/qemu-arm" "$DEST/qemu-arm"
    chmod 755 "$DEST/qemu-arm"
else
    echo "note: qemu-arm not built yet -- run tools/build_qemu.sh <qemu-7.2-src> and copy" >&2
    echo "  build/ampguest/qemu-arm into $DEST/" >&2
fi
echo "then: cp -r $(dirname "$DEST")/.ampguest <card-root>/ && set [applemusic] translate=1"
