#include "amppage.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/ampapi.h"
#include "src/system/streaming/ampstream.h"
#include "src/system/streaming/ampwrapper.h"
#include "src/system/streaming/streamturn.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lv_obj_t *amp_screen;
static lv_obj_t *settings_screen, *edit_screen, *list_screen;
static lv_obj_t *list_container, *list_title, *wrapper_value, *port_value, *store_value;
static lv_obj_t *edit_field, *edit_title;
static keyboard_t *keyboard;
static gui_config_t *config;
static bool busy;
static int editing;

// One page of search results, which is what the low-end screen needs.
#define AMP_PAGE 20

// The same single-mailbox worker shape the other streaming pages use: results
// own their memory until the GUI takes them, and a late worker can never
// overwrite a list the GUI is reading.
typedef enum { JOB_SEARCH, JOB_ME } job_kind_t;
typedef struct {
	unsigned serial;
	job_kind_t kind;
	char term[64];
} job_t;

typedef struct {
	job_t job;
	ampapi_song_t *songs;
	int count;
	char error[192];
} result_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static bool worker_started, have_job, searching;
static lv_obj_t *job_owner;
static unsigned serial, failed_serial;
static job_t pending;
static result_t *mail;
static ampapi_song_t *shown;
static int shown_count;

static void free_result(result_t *result) {
	if (result) {
		free(result->songs);
		free(result);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_low_priority("Apple Music API");
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_job) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending;
		memset(&pending, 0, sizeof(pending));
		have_job = false;
		unsigned latest = serial;
		pthread_mutex_unlock(&job_lock);
		if (job.serial != latest) {
			continue;
		}
		result_t *result = calloc(1, sizeof(*result));
		if (!result) {
			// An allocation failure must still clear the GUI's wait. The tiny
			// fallback result gets a chance once the larger old mail is freed.
			pthread_mutex_lock(&job_lock);
			free_result(mail);
			mail = NULL;
			pthread_mutex_unlock(&job_lock);
			result = calloc(1, sizeof(*result));
		}
		if (result) {
			result->job = job;
			ampw_me_t me;
			// The tokens rotate whenever the daemon re-authenticates, so every
			// job reads them fresh; they are wiped before the result is sent.
			if (!ampw_me(config_get("applemusic", "wrapper_url", ""), &me, result->error,
						 sizeof(result->error))) {
				result->count = -1;
			} else if (!me.authenticated) {
				memset(&me, 0, sizeof(me)); // no tokens linger
				result->count = -1;
				snprintf(result->error, sizeof(result->error), "amp_not_authenticated");
			} else if (job.kind == JOB_ME) {
				memset(&me, 0, sizeof(me));
				result->count = 0; // the daemon answers and the session is live
			} else {
				char storefront[32];
				snprintf(storefront, sizeof(storefront), "%.20s",
						 config_get("applemusic", "storefront", "jp"));
				result->songs = calloc(AMP_PAGE, sizeof(*result->songs));
				result->count = result->songs
					? ampapi_search_songs(me.dev_token, me.music_user_token, storefront,
										  job.term, result->songs, AMP_PAGE, result->error,
										  sizeof(result->error))
					: -1;
				memset(&me, 0, sizeof(me));
			}
			if (result->count < 0 && !result->error[0]) {
				snprintf(result->error, sizeof(result->error), "ampapi_failed");
			}
		}
		pthread_mutex_lock(&job_lock);
		free_result(mail);
		mail = result;
		if (!result) {
			failed_serial = job.serial;
		}
		pthread_mutex_unlock(&job_lock);
	}
	return NULL;
}

static bool submit(job_t *job) {
	if (!worker_started) {
		pthread_t worker;
		pthread_attr_t attr;
		int err = pthread_attr_init(&attr);
		if (!err) {
			err = pthread_attr_setstacksize(&attr, 256 * 1024);
			if (!err) {
				err = pthread_create(&worker, &attr, worker_main, NULL);
			}
			pthread_attr_destroy(&attr);
		}
		if (err) {
			toast_error("amp_out_of_memory");
			return false;
		}
		pthread_detach(worker);
		worker_started = true;
	}
	pthread_mutex_lock(&job_lock);
	job->serial = ++serial;
	pending = *job;
	have_job = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
	busy = true;
	job_owner = job->kind == JOB_SEARCH ? list_screen : settings_screen;
	toast_busy("loading");
	return true;
}

static void invalidate_job(void) {
	pthread_mutex_lock(&job_lock);
	serial++;
	have_job = false;
	memset(&pending, 0, sizeof(pending));
	free_result(mail);
	mail = NULL;
	pthread_mutex_unlock(&job_lock);
	busy = false;
	job_owner = NULL;
	toast_busy_end();
}

static void page_left_cb(lv_event_t *e) {
	// Back/player navigation must not leave a global busy overlay behind or
	// let a late result rebuild a hidden list/show a toast on another page.
	if (busy && lv_event_get_target(e) == job_owner) {
		invalidate_job();
	}
}

// ---------------------------------------------------------------------------
// the settings draft: read from the config, written back on Connect
// ---------------------------------------------------------------------------

static char draft_url[256], draft_store[32];
static int draft_port;

static void draft_load(void) {
	ampstream_config(draft_url, sizeof(draft_url), &draft_port, draft_store, sizeof(draft_store));
}

static void draft_save(void) {
	config_set("applemusic", "wrapper_url", draft_url);
	config_set_int("applemusic", "decrypt_port", draft_port);
	config_set("applemusic", "storefront", draft_store);
}

static bool configured(void) {
	char url[256];
	int port;
	char store[32];
	ampstream_config(url, sizeof(url), &port, store, sizeof(store));
	return url[0] != '\0';
}

static void settings_refresh(void) {
	lv_label_set_text(wrapper_value, draft_url[0] ? draft_url : tr("amp_not_set"));
	char port[16];
	snprintf(port, sizeof(port), "%d", draft_port);
	lv_label_set_text(port_value, port);
	lv_label_set_text(store_value, draft_store[0] ? draft_store : tr("amp_not_set"));
}

// ---------------------------------------------------------------------------
// editing: one keyboard, one field, three destinations
// ---------------------------------------------------------------------------

static void run_search(const char *term);
static void poll_cb(lv_timer_t *timer);

static void edit_cb(lv_event_t *e) {
	if (busy || switcher_back_drag_active()) {
		return;
	}
	searching = false;
	editing = (int)(intptr_t)lv_event_get_user_data(e);
	static const char *keys[] = {"amp_wrapper_url", "amp_decrypt_port", "amp_storefront"};
	lv_label_set_text(edit_title, tr(keys[editing]));
	const char *text = editing == 0 ? draft_url : editing == 1 ? "" : draft_store;
	lv_textarea_set_text(edit_field, text);
	lv_textarea_set_placeholder_text(edit_field, editing == 1 ? "10020" : tr(keys[editing]));
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
	switch_screen(edit_screen);
}

static void accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(edit_field);
	char value[64];
	snprintf(value, sizeof(value), "%s", text);
	lv_textarea_set_text(edit_field, "");
	if (searching) {
		if (value[0]) {
			run_search(value);
		}
		return;
	}
	if (editing == 0) {
		snprintf(draft_url, sizeof(draft_url), "%s", value);
	} else if (editing == 1) {
		char *end = NULL;
		long port = strtol(value, &end, 10);
		if (!value[0] || (end && *end) || port < 1 || port > 65535) {
			toast_error("amp_bad_port");
			return;
		}
		draft_port = (int)port;
	} else {
		// The storefront is tiny ("jp", "us"): refuse overlong values rather
		// than truncate them into a different store.
		if (strlen(value) >= sizeof(draft_store)) {
			toast_error("amp_bad_storefront");
			return;
		}
		snprintf(draft_store, sizeof(draft_store), "%s", value);
	}
	settings_refresh();
	switch_screen_return_to(settings_screen);
}

static void edit_left_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(edit_field, "");
}

// ---------------------------------------------------------------------------
// the settings page: connect tests the daemon and saves the draft
// ---------------------------------------------------------------------------

static void test_cb(lv_event_t *e) {
	(void)e;
	if (busy) {
		return;
	}
	if (!draft_url[0]) {
		toast_error("amp_not_configured"); // nothing to test: fill in the URL
		return;
	}
	draft_save();
	job_t job = {.kind = JOB_ME};
	submit(&job);
}

// ---------------------------------------------------------------------------
// search: the term typed on the keyboard, the page of songs that came back
// ---------------------------------------------------------------------------

static void run_search(const char *term) {
	free(shown);
	shown = NULL;
	shown_count = 0;
	lv_obj_clean(list_container);
	lv_label_set_text(list_title, tr("applemusic_searching"));
	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
	job_t job = {.kind = JOB_SEARCH};
	snprintf(job.term, sizeof(job.term), "%s", term);
	submit(&job);
}

static void search_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	if (!configured()) {
		draft_load();
		settings_refresh();
		switch_screen(settings_screen);
		toast_error("amp_not_configured");
		return;
	}
	searching = true; // the shared keyboard field is the search term here
	lv_label_set_text(edit_title, tr("applemusic_search"));
	lv_textarea_set_text(edit_field, "");
	lv_textarea_set_placeholder_text(edit_field, tr("amp_search_hint"));
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
	switch_screen(edit_screen);
}

static void row_cb(lv_event_t *e) {
	if (busy || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= shown_count) {
		return;
	}
	// Paths are small, fixed and credential-free. The playlist takes copies
	// synchronously; the bridge retains only the track metadata.
	char (*paths)[AMP_PATH_MAX] = calloc(AMP_PAGE, sizeof(*paths));
	const char **pointers = calloc(AMP_PAGE, sizeof(*pointers));
	int count = 0, selected = 0;
	char error[192] = "";
	bool ok = paths && pointers && ampstream_queue(shown, shown_count, index, paths, pointers,
												   &count, &selected, error, sizeof(error));
	if (ok) {
		streamturn_abandon_all();
		device_state_play_list(pointers, count, selected);
		player_refresh_now_playing();
		player_sheet_open(true);
	} else {
		toast_error(error[0] ? error : "amp_out_of_memory");
	}
	free(pointers);
	free(paths);
}

static void rebuild_list(void) {
	lv_obj_clean(list_container);
	lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);
	if (!shown_count) {
		lv_obj_t *empty = lv_label_create(list_container);
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		lv_obj_set_width(empty, lv_pct(100));
		lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
		lv_label_set_text(empty, tr("amp_no_results"));
		return;
	}
	for (int i = 0; i < shown_count; i++) {
		lv_obj_t *row = settingsrow_add(list_container, "", NULL, row_cb, (void *)(intptr_t)i);
		char text[512];
		snprintf(text, sizeof(text), "%s\n%s", shown[i].title, shown[i].artist);
		lv_label_set_text(settingsrow_name_label(row), text); // data, not a translation key
		settingsrow_name_lines(row, 2);
		settingsrow_add_icon(row, &icon_music_note, THEME_SEMANTIC_CYAN);
	}
}

static lv_obj_t *screen(void) {
	lv_obj_t *obj = lv_obj_create(NULL);
	lv_obj_add_style(obj, &theme_style_screen, 0);
	lv_obj_add_event_cb(obj, page_left_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(obj);
	return obj;
}

// A paragraph of explanation under the row it belongs to, the way the subsonic
// settings page annotates its own account.
static void add_note(lv_obj_t *container, const char *key) {
	lv_obj_t *note = lv_label_create(container);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_obj_set_width(note, lv_pct(100));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_label_set_text(note, tr(key));
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	draft_load();
	settings_refresh();
	switch_screen(settings_screen);
}

void amppage_init(gui_config_t *cfg) {
	config = cfg;
	device_state_add_prepare_cb(ampstream_prepare);
	streamturn_register_abandon(ampstream_cancel);

	amp_screen = screen();
	lv_obj_t *home = settingsrow_page(amp_screen, cfg, "applemusic");
	lv_obj_t *row = settingsrow_add(home, "amp_wrapper_settings", NULL, open_settings_cb, NULL);
	settingsrow_add_icon(row, &icon_music_settings, THEME_SEMANTIC_BLUE);
	row = settingsrow_action(home, "applemusic_search", search_cb, NULL);
	settingsrow_add_icon(row, &icon_search, THEME_SEMANTIC_GREEN);
	player_sheet_attach_drag(home, true);

	// The settings page: where the wrapper lives, the decrypt port, the
	// storefront the catalog is read from, and the test that saves them.
	settings_screen = screen();
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "amp_wrapper_settings");
	row = settingsrow_add(container, "amp_wrapper_url", &wrapper_value, edit_cb, (void *)(intptr_t)0);
	settingsrow_add_icon(row, &icon_wifi_high, THEME_SEMANTIC_BLUE);
	row = settingsrow_add(container, "amp_decrypt_port", &port_value, edit_cb, (void *)(intptr_t)1);
	settingsrow_add_icon(row, &icon_music_settings, THEME_SEMANTIC_PURPLE);
	row = settingsrow_add(container, "amp_storefront", &store_value, edit_cb, (void *)(intptr_t)2);
	settingsrow_add_icon(row, &icon_music_note, THEME_SEMANTIC_CYAN);
	row = settingsrow_action(container, "amp_connect_save", test_cb, NULL);
	settingsrow_add_icon(row, &icon_check, THEME_SEMANTIC_GREEN);
	add_note(container, "amp_settings_note");
	player_sheet_attach_drag(container, true);

	// The results list and the shared input screen.
	list_screen = screen();
	list_container = settingsrow_page(list_screen, cfg, "applemusic");
	list_title = settingsrow_page_title(list_screen);
	player_sheet_attach_drag(list_container, true);

	edit_screen = screen();
	edit_title = settingsrow_title(edit_screen, cfg, "applemusic_search");
	edit_field = lv_textarea_create(edit_screen);
	lv_textarea_set_one_line(edit_field, true);
	lv_obj_add_style(edit_field, &theme_style_card, 0);
	lv_obj_set_style_text_font(edit_field, &font_ui_24, 0);
	lv_obj_set_style_pad_all(edit_field, 8, 0);
	lv_obj_set_size(edit_field, cfg->screen_width - 2 * cfg->padding,
					cfg->screen_width < 320 ? 44 : 64);
	lv_obj_align(edit_field, LV_ALIGN_TOP_LEFT, cfg->padding, settingsrow_content_top(cfg));
	keyboard_style_caret(edit_field);
	keyboard = keyboard_create(edit_screen, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316,
							   edit_field, NULL, "ok", accept_cb, NULL);
	lv_obj_add_event_cb(edit_screen, edit_left_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	// Piggybacks on LVGL's existing event loop. The fetch worker sleeps on a
	// condition; this tiny two-Hz check does no REST/socket I/O.
	lv_timer_create(poll_cb, 500, NULL);
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	pthread_mutex_lock(&job_lock);
	result_t *result = mail;
	mail = NULL;
	unsigned latest = serial, failed = failed_serial;
	failed_serial = 0;
	pthread_mutex_unlock(&job_lock);
	if (failed && failed == latest) {
		busy = false;
		toast_busy_end();
		toast_error("amp_out_of_memory");
	}
	if (result) {
		if (result->job.serial == latest) {
			busy = false;
			toast_busy_end();
			if (result->count < 0) {
				toast_error(result->error[0] ? result->error : "ampapi_failed");
			} else if (result->job.kind == JOB_SEARCH) {
				free(shown);
				shown = result->songs;
				result->songs = NULL;
				shown_count = result->count;
				rebuild_list();
			} else {
				toast_success("amp_connected");
			}
		}
		free_result(result);
	}

	// The same 90%-progress prefetch the subsonic page drives, and the one
	// error the producer wants the GUI to show.
	char error[192];
	if (ampstream_take_error(error, sizeof(error))) {
		toast_error(error);
	}
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		char current[512], next[512];
		audio_get_current_file(current, sizeof(current));
		if (ampstream_owns(current)) {
			double position = 0, duration = 0;
			audio_get_progress(&position, &duration);
			if (duration > 0 && position >= duration * 0.90) {
				if (playlist_peek_auto(next, sizeof(next)) && ampstream_owns(next)) {
					ampstream_prefetch(next);
				} else {
					ampstream_prefetch(NULL);
				}
			}
		}
	}
}
