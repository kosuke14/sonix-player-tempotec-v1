#!/bin/sh
# Build the hit-band regression harness: tests/hitband/test_hitband.
#
#   ./build.sh
#   BOARD=tempotec_v1  ./test_hitband              # compact panel
#   BOARD=hiby_r1      ./test_hitband              # wide panel
#   BOARD=tempotec_v1 QP_HANDLE=1 ./test_hitband   # layout with the fold handle
#
# Links the real quickpanel.c/topbar.c against LVGL and the same support
# objects tests/ui builds; everything else the shell reaches for is stubbed
# in stubs.c. Needs lvgl/ (the repo checkout) and builds tests/.build/ui
# on first run. Never ticks LVGL -- the harness re-lays out after every
# position change instead (see the comment at the top of test_hitband.c).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd) # sonix-player/
TESTS="$ROOT/tests"
cd "$ROOT"

if [ ! -d "$TESTS/.build/ui/lvgl" ]; then
	make -C "$TESTS" -j2 .build/test_main_menu
fi

FLAGS="-O1 -g -std=gnu11 -Wall -D_FILE_OFFSET_BITS=64 -DGB_CORE=1 -ffunction-sections -fdata-sections"
INC="-I. -Ilvgl"
LVCONF="-DLV_CONF_PATH=\"$TESTS/ui/lv_conf.h\""
OUT="$HERE/build"
mkdir -p "$OUT"

cc $FLAGS $INC $LVCONF -c src/gui/shell/topbar.c -o "$OUT/topbar.o"
cc $FLAGS $INC $LVCONF -c src/gui/shell/quickpanel.c -o "$OUT/quickpanel.o"
cc $FLAGS $INC $LVCONF -c "$HERE/stubs.c" -o "$OUT/stubs.o"
cc $FLAGS $INC $LVCONF -c "$HERE/test_hitband.c" -o "$OUT/test_hitband.o"

LVGL_OBJS=$(find "$TESTS/.build/ui/lvgl" -name '*.o')
cc -o "$HERE/test_hitband" "$OUT/test_hitband.o" "$OUT/topbar.o" "$OUT/quickpanel.o" "$OUT/stubs.o" \
	"$TESTS/.build/ui/src/gui/shell/theme.o" \
	"$TESTS/.build/ui/src/gui/shell/icons.o" \
	"$TESTS/.build/ui/src/gui/board_profile.o" \
	"$TESTS/.build/ui/src/system/core/config.o" \
	"$TESTS/.build/ui/src/system/core/lang.o" \
	$LVGL_OBJS -Wl,--gc-sections -lpthread -ldl -lm

echo "BUILD_OK $HERE/test_hitband"
