/* Hit-band regression harness, run headless against the real LVGL 9.6.
 *
 * What it pins down, on both panel sizes (BOARD=tempotec_v1 / hiby_r1):
 *
 *  1. The quick panel's brightness slider claims its row -- sun icon and
 *     padding included, up to the fold handle's own band at the bottom --
 *     so a press that looks like it lands on the slider starts a scrub
 *     instead of falling through to the card, where the same movement
 *     drags the sheet. (quickpanel.c: bright_row_h, ext_click_area.)
 *     In the default layout the handle is absent and every pixel of the
 *     row is the slider. With QP_HANDLE=1 the config holds more than eight
 *     buttons, the card grows for the fold handle -- and the handle lands
 *     just below the row instead of over it, so the whole row stays the
 *     slider while the handle's own tap still folds the card.
 *
 *  2. Nothing above the row is stolen by that reach: a press on the button
 *     line still finds the buttons, and the gap between the lines still
 *     finds the card. LVGL only searches for a child inside its parent's
 *     rectangle, which is what caps the slider.
 *
 *  3. The status bar's pull-down band: on the compact panel the bar claims
 *     presses down to TOPBAR_PULL_GRAB_EXT_PX past its own height (the
 *     leather-case lip), but only while the control centre is shut -- open,
 *     a press in the band goes to the sheet as it did before the band
 *     existed; closed again, the band returns. On the wide panel there is
 *     no band at all.
 *
 *  4. The compact card's new geometry: the row is 40 px and the two button
 *     lines still fit in the 98 px left for them.
 *
 * The object tree is built by the real quickpanel_init()/topbar_init(), with
 * the rest of the player stubbed (stubs.c). Never call lv_timer_handler:
 * every position change is followed by lv_obj_update_layout() instead, the
 * same way test_main_menu keeps its tree measured without ticking.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_obj_class_private.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

/* The traversal the indev uses to find the object under a point; not in the
 * public headers, but it is what every press runs through. */
extern lv_obj_t *lv_indev_search_obj(lv_obj_t *obj, lv_point_t *point);

static int failures;

/* The same order pointer_search_obj() uses: sys, top layer, screen. */
static lv_obj_t *press_hit(int32_t x, int32_t y) {
	lv_point_t p = {.x = x, .y = y};
	lv_obj_t *got = lv_indev_search_obj(lv_display_get_layer_sys(lv_display_get_default()), &p);
	if (!got) {
		got = lv_indev_search_obj(lv_layer_top(), &p);
	}
	if (!got) {
		got = lv_indev_search_obj(lv_screen_active(), &p);
	}
	return got;
}

static void expect_hit(const char *what, lv_obj_t *want, int32_t x, int32_t y) {
	lv_obj_t *got = press_hit(x, y);
	if (got != want) {
		printf("FAIL %s: (%d,%d) hit %s, wanted %s\n", what, (int)x, (int)y,
		       got ? lv_obj_get_class(got)->name : "NULL",
		       want ? lv_obj_get_class(want)->name : "NULL");
		failures++;
	} else {
		printf("ok   %s: (%d,%d) -> %s\n", what, (int)x, (int)y,
		       got ? lv_obj_get_class(got)->name : "NULL");
	}
}

static void expect_not_hit(const char *what, lv_obj_t *not_this, int32_t x, int32_t y) {
	lv_obj_t *got = press_hit(x, y);
	if (got == not_this) {
		printf("FAIL %s: (%d,%d) hit it anyway\n", what, (int)x, (int)y);
		failures++;
	} else {
		printf("ok   %s: (%d,%d) -> %s\n", what, (int)x, (int)y,
		       got ? lv_obj_get_class(got)->name : "NULL");
	}
}

/* Find the status bar on the top layer: the short full-width clickable. */
static lv_obj_t *find_bar(int32_t bar_h) {
	uint32_t n = lv_obj_get_child_count(lv_layer_top());
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *c = lv_obj_get_child(lv_layer_top(), (int32_t)i);
		if (lv_obj_get_height(c) == bar_h && lv_obj_get_width(c) > 200 &&
		    lv_obj_is_clickable(c)) {
			return c;
		}
	}
	return NULL;
}

/* veil -> panel -> the controls card (panel children: now-playing, controls,
 * fold handle). */
static lv_obj_t *find_controls_card(int32_t screen_h) {
	uint32_t n = lv_obj_get_child_count(lv_layer_top());
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *veil = lv_obj_get_child(lv_layer_top(), (int32_t)i);
		if (lv_obj_get_height(veil) != screen_h || lv_obj_get_child_count(veil) < 1) {
			continue;
		}
		lv_obj_t *panel = lv_obj_get_child(veil, 0);
		if (lv_obj_get_child_count(panel) < 3) {
			continue;
		}
		return lv_obj_get_child(panel, 1);
	}
	return NULL;
}

static bool in_area(const lv_area_t *a, int32_t x, int32_t y) {
	return x >= a->x1 && x <= a->x2 && y >= a->y1 && y <= a->y2;
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if (!getenv("BOARD")) {
		setenv("BOARD", "tempotec_v1", 1); /* consulted on first bp_* call */
	}

	char dir[] = "/tmp/sonix-hit-XXXXXX";
	assert(mkdtemp(dir));
	char config[256];
	snprintf(config, sizeof(config), "%s/device_config.ini", dir);
	config_init(config); /* false: no file yet, defaults are fine */

	/* The second mode: more than eight buttons in the grid, so the card
	 * shows the fold handle over the bottom of the brightness row. Must be
	 * set before quickpanel_init, which reads the layout once. */
	const bool with_handle = getenv("QP_HANDLE") != NULL;
	if (with_handle) {
		config_set_int("quickpanel", "layout", 4);
	}

	/* Works from the repo's sonix-player/ directory (the usual place to run
	 * it) or from tests/ or tests/hitband/; SONIX_LANG overrides. */
	const char *lang = getenv("SONIX_LANG");
	if (!lang) {
		lang = argc > 1 ? argv[1] : "../sonix-packer/assets/R1/usr/resource/sonix/language";
	}
	if (access(lang, R_OK) != 0) {
		lang = "../../sonix-packer/assets/R1/usr/resource/sonix/language";
	}
	if (access(lang, R_OK) != 0) {
		lang = "../../../sonix-packer/assets/R1/usr/resource/sonix/language";
	}
	lang_init(lang);

	static lv_font_t *faces[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
		&font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold,
		&font_ui_26, &font_ui_28, &font_ui_32,
	};
	for (size_t i = 0; i < sizeof(faces) / sizeof(faces[0]); i++) {
		*faces[i] = lv_font_montserrat_14;
	}

	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), "/tmp"};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();

	topbar_init(&cfg);
	quickpanel_init(&cfg);

	const bool compact = cfg.screen_width < 320;
	printf("panel %ux%u %s%s\n", cfg.screen_width, cfg.screen_height,
	       compact ? "(compact)" : "(wide)", with_handle ? " [handle]" : "");

	/* Layout everything, parked state first. */
	lv_obj_update_layout(lv_layer_top());
	lv_obj_update_layout(lv_screen_active());

	lv_obj_t *bar = find_bar(cfg.top_bar_height);
	assert(bar);
	printf("bar height %d\n", (int)lv_obj_get_height(bar));

	/* --- 3a: closed, the band below the bar is the bar's (compact only) --- */
	expect_hit("bar proper", bar, cfg.screen_width / 2, cfg.top_bar_height / 2);
	if (compact) {
		expect_hit("band first row", bar, cfg.screen_width / 2, cfg.top_bar_height);
		expect_hit("band middle", bar, cfg.screen_width / 2, cfg.top_bar_height + 8);
		expect_hit("band last row", bar, cfg.screen_width / 2,
			   cfg.top_bar_height + TOPBAR_PULL_GRAB_EXT_PX - 1);
		expect_not_hit("past the band", bar, cfg.screen_width / 2,
			       cfg.top_bar_height + TOPBAR_PULL_GRAB_EXT_PX + 8);
		expect_not_hit("well below the bar", bar, cfg.screen_width / 2,
			       cfg.top_bar_height + 60);
	} else {
		expect_not_hit("no band on wide panels", bar, cfg.screen_width / 2,
			       cfg.top_bar_height + 8);
	}

	/* --- open the panel through the drag API (no animation needed; the
	 * layout pass after each step is what makes LVGL's deferred position
	 * changes visible to the next read) --- */
	quickpanel_drag_begin();
	quickpanel_drag_update(cfg.screen_height * 2);
	lv_obj_update_layout(lv_layer_top());
	quickpanel_drag_end();
	lv_obj_update_layout(lv_layer_top());
	assert(quickpanel_is_open());

	/* --- 3c: open, the band goes to the sheet, not to the bar --- */
	expect_not_hit("band over the open sheet", bar, cfg.screen_width / 2,
		       cfg.top_bar_height + 8);
	expect_hit("bar proper still the bar while open", bar, cfg.screen_width / 2,
		   cfg.top_bar_height / 2);

	/* --- 1/2/4: the brightness row --- */
	lv_obj_t *card = find_controls_card(cfg.screen_height);
	assert(card);
	lv_obj_t *button_row = lv_obj_get_child(card, 0);
	lv_obj_t *bright_row = lv_obj_get_child(card, 1);
	assert(button_row && bright_row);

	int32_t want_row_h = compact ? 40 : 44;
	printf("controls card %dx%d, button row %d, brightness row %d\n",
	       (int)lv_obj_get_width(card), (int)lv_obj_get_height(card),
	       (int)lv_obj_get_height(button_row), (int)lv_obj_get_height(bright_row));
	assert(lv_obj_get_height(bright_row) == want_row_h);
	if (compact) {
		assert(lv_obj_get_height(button_row) == 98); /* two 44 px lines + gap */
	}

	/* The two button lines still fit inside their row. */
	uint32_t nbtn = lv_obj_get_child_count(button_row);
	assert(nbtn >= 8);
	lv_obj_t *last_second_line = lv_obj_get_child(button_row, 7);
	assert(lv_obj_get_y(last_second_line) + lv_obj_get_height(last_second_line) <=
	       lv_obj_get_y(button_row) + lv_obj_get_height(button_row));

	/* The slider: last child of the row (the sun came first). */
	lv_obj_t *slider =
		lv_obj_get_child(bright_row, (int32_t)lv_obj_get_child_count(bright_row) - 1);
	assert(slider && lv_obj_get_class(slider) == &lv_slider_class);

	/* The fold handle, a later sibling of the row whose band at the bottom
	 * of the card is the one place inside the row that belongs to
	 * something else. It only exists when the grid holds more than the
	 * eight buttons the card shows. */
	lv_obj_t *handle = lv_obj_get_child(card, 3);
	assert(handle && lv_obj_get_height(handle) > 0);
	const bool handle_shown = !lv_obj_is_hidden(handle);
	printf("handle %s\n", handle_shown ? "shown" : "hidden");
	assert(handle_shown == with_handle);

	lv_area_t handle_area;
	lv_obj_get_coords(handle, &handle_area);
	lv_area_t row_area;
	lv_obj_get_coords(bright_row, &row_area);
	printf("row y %d..%d x %d..%d, handle y %d..%d\n",
	       (int)row_area.y1, (int)row_area.y2, (int)row_area.x1, (int)row_area.x2,
	       (int)handle_area.y1, (int)handle_area.y2);

	int32_t mid_x = (row_area.x1 + row_area.x2) / 2;
	int32_t mid_y = (row_area.y1 + row_area.y2) / 2;

	/* 1: the edges and interior of the row find the slider -- including over
	 * the sun icon on the left and the padding on the right, both of which
	 * used to fall through to the card and start a sheet drag. Points inside
	 * the handle's band are checked separately below. */
	int32_t gx[5] = {row_area.x1 + 1, row_area.x1 + 20, mid_x, row_area.x2 - 20, row_area.x2 - 2};
	int32_t gy[5] = {row_area.y1 + 1, mid_y - 8, mid_y, mid_y + 8, row_area.y2 - 1};
	int band_points = 0;
	for (int i = 0; i < 5; i++) {
		for (int j = 0; j < 5; j++) {
			if (handle_shown && in_area(&handle_area, gx[i], gy[j])) {
				continue;
			}
			band_points++;
			expect_hit("brightness row", slider, gx[i], gy[j]);
		}
	}
	assert(band_points >= 20);

	/* The row's last pixel line is slider in every layout: the handle,
	 * when there is one, sits below the row rather than over it. */
	expect_hit("row bottom stays slider", slider, mid_x, row_area.y2 - 1);

	/* And the handle keeps its own tap band where it is. */
	if (handle_shown) {
		expect_hit("fold handle", handle, mid_x, handle_area.y1 + 1);
	}

	/* 2: the reach stops at the row. */
	expect_not_hit("gap above the row", slider, mid_x, row_area.y1 - 1);
	lv_obj_t *wifi_btn = lv_obj_get_child(button_row, 0);
	int32_t bx = lv_obj_get_x(wifi_btn) + lv_obj_get_width(wifi_btn) / 2;
	int32_t by = lv_obj_get_y(wifi_btn) + lv_obj_get_height(wifi_btn) - 2;
	expect_hit("button line intact", wifi_btn, bx, by);

	/* --- 3d: closed again, the band belongs to the bar once more --- */
	quickpanel_close();
	lv_obj_update_layout(lv_layer_top());
	assert(!quickpanel_is_open());
	if (compact) {
		expect_hit("band back once closed", bar, cfg.screen_width / 2,
			   cfg.top_bar_height + 8);
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
