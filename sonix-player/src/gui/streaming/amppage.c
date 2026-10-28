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
static lv_obj_t *account_value, *user_value, *pass_value;
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
typedef enum { JOB_SEARCH, JOB_ME, JOB_LOGIN, JOB_2FA, JOB_LOGOUT } job_kind_t;
typedef struct {
	unsigned serial;
	job_kind_t kind;
	char term[64];
	char username[128]; // the Apple ID, and its password for a sign-in
	char password[128];
	char code[8]; // the 2FA code, when the flow asks for one
} job_t;

typedef struct {
	job_t job;
	ampapi_song_t *songs;
	int count;
	bool signed_in;   // the daemon's session state, read with every job
	char apple_id[128];
	char error[192];
} result_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static bool worker_started, have_job, searching, entering_2fa;
static lv_obj_t *job_owner;
static unsigned serial, failed_serial;
static job_t pending;
static result_t *mail;
static ampapi_song_t *shown;
static int shown_count;
static bool session_signed_in;  // the account row, refreshed from /me
static char session_apple_id[128];

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
			memset(&job.password, 0, sizeof(job.password)); // credentials never linger
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
			const char *url = config_get("applemusic", "wrapper_url", "");
			if (job.kind == JOB_LOGIN) {
				// The sign-in flow runs on the daemon, but every field the user
				// types comes from this device's keyboard: the player is the
				// only place the password exists, and it is wiped right here.
				ampw_login_result_t r = ampw_login(url, job.username, job.password,
												   result->error, sizeof(result->error));
				memset(job.password, 0, sizeof(job.password));
				result->count = r == AMPW_LOGIN_OK ? 0 : r == AMPW_LOGIN_2FA ? 1 : -1;
			} else if (job.kind == JOB_2FA) {
				result->count = ampw_login_2fa(url, job.code, result->error,
											   sizeof(result->error)) == AMPW_LOGIN_OK ? 0 : -1;
				memset(job.code, 0, sizeof(job.code));
			} else if (job.kind == JOB_LOGOUT) {
				result->count = ampw_logout(url, result->error, sizeof(result->error)) ? 0 : -1;
			} else {
				ampw_me_t me;
				// The tokens rotate whenever the daemon re-authenticates, so
				// every job reads them fresh; they are wiped before the result
				// is sent.
				if (!ampw_me(url, &me, result->error, sizeof(result->error))) {
					result->count = -1;
				} else {
					result->signed_in = me.authenticated;
					snprintf(result->apple_id, sizeof(result->apple_id), "%s", me.apple_id);
					if (job.kind == JOB_ME) {
						result->count = 0; // the state snapshot is the whole answer
					} else if (!me.authenticated) {
						result->count = -1;
						snprintf(result->error, sizeof(result->error), "amp_not_authenticated");
					} else {
						char storefront[32];
						snprintf(storefront, sizeof(storefront), "%.20s",
								 config_get("applemusic", "storefront", "jp"));
						result->songs = calloc(AMP_PAGE, sizeof(*result->songs));
						result->count = result->songs
							? ampapi_search_songs(me.dev_token, me.music_user_token, storefront,
												  job.term, result->songs, AMP_PAGE,
												  result->error, sizeof(result->error))
							: -1;
					}
					memset(&me, 0, sizeof(me));
				}
			}
			// A sign-in hop changes the session behind the flow, so the
			// account row is refreshed from a fresh /me inside the same job.
			if (job.kind == JOB_LOGIN || job.kind == JOB_2FA || job.kind == JOB_LOGOUT) {
				if (result->count >= 0) {
					ampw_me_t me;
					if (ampw_me(url, &me, result->error, sizeof(result->error))) {
						result->signed_in = me.authenticated;
						snprintf(result->apple_id, sizeof(result->apple_id), "%s", me.apple_id);
						memset(&me, 0, sizeof(me));
					}
				}
			}
			if (result->count < 0 && !result->error[0]) {
				snprintf(result->error, sizeof(result->error), "ampapi_failed");
			}
		} else {
			memset(&job.password, 0, sizeof(job.password));
			memset(&job.code, 0, sizeof(job.code));
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
// the settings draft: the wrapper config is saved, the credentials are not
// ---------------------------------------------------------------------------

static char draft_url[256], draft_store[32];
static char draft_user[128], draft_pass[128]; // this session only; never written to the card
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
	// The account row mirrors the daemon's session as the last job saw it;
	// the password is shown as a marker only, since it is never stored.
	if (session_signed_in && session_apple_id[0]) {
		lv_label_set_text(account_value, session_apple_id);
	} else {
		lv_label_set_text(account_value, tr(session_signed_in ? "amp_signed_in"
															  : "amp_not_signed_in"));
	}
	lv_label_set_text(user_value, draft_user[0] ? draft_user : tr("amp_not_set"));
	lv_label_set_text(pass_value, draft_pass[0] ? "******" : tr("amp_not_set"));
}

static void apply_state(result_t *result) {
	session_signed_in = result->signed_in;
	snprintf(session_apple_id, sizeof(session_apple_id), "%s", result->apple_id);
	settings_refresh();
}

// ---------------------------------------------------------------------------
// editing: one keyboard, one field, five destinations plus the 2FA hop
// ---------------------------------------------------------------------------

static void run_search(const char *term);
static void poll_cb(lv_timer_t *timer);

static void edit_cb(lv_event_t *e) {
	if (busy || switcher_back_drag_active()) {
		return;
	}
	searching = false;
	editing = (int)(intptr_t)lv_event_get_user_data(e);
	static const char *keys[] = {"amp_wrapper_url", "amp_decrypt_port", "amp_storefront",
								 "amp_username", "password"};
	lv_label_set_text(edit_title, tr(keys[editing]));
	const char *text = editing == 0	 ? draft_url
					   : editing == 1 ? ""
					   : editing == 2 ? draft_store
					   : editing == 3 ? draft_user
									  : draft_pass;
	lv_textarea_set_accepted_chars(edit_field, NULL);
	lv_textarea_set_max_length(edit_field, 127);
	lv_textarea_set_text(edit_field, text);
	// Readable, like the subsonic page's, so a mistyped password is seen
	// before it is sent.
	lv_textarea_set_password_mode(edit_field, false);
	lv_textarea_set_placeholder_text(edit_field, editing == 1 ? "10020" : tr(keys[editing]));
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
	switch_screen(edit_screen);
}

static void accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(edit_field);
	char value[128];
	snprintf(value, sizeof(value), "%s", text);
	lv_textarea_set_text(edit_field, "");
	if (searching) {
		if (value[0]) {
			run_search(value);
		}
		return;
	}
	if (entering_2fa) {
		// The code goes straight into the sign-in job; nothing about it stays.
		char code[8];
		snprintf(code, sizeof(code), "%s", value);
		entering_2fa = false;
		lv_textarea_set_accepted_chars(edit_field, NULL);
		lv_textarea_set_max_length(edit_field, 0);
		if (!code[0]) {
			return;
		}
		job_t job = {.kind = JOB_2FA};
		snprintf(job.code, sizeof(job.code), "%s", code);
		memset(code, 0, sizeof(code));
		submit(&job);
		memset(&job, 0, sizeof(job));
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
	} else if (editing == 2) {
		// The storefront is tiny ("jp", "us"): refuse overlong values rather
		// than truncate them into a different store.
		if (strlen(value) >= sizeof(draft_store)) {
			toast_error("amp_bad_storefront");
			return;
		}
		snprintf(draft_store, sizeof(draft_store), "%s", value);
	} else if (editing == 3) {
		snprintf(draft_user, sizeof(draft_user), "%s", value);
	} else {
		snprintf(draft_pass, sizeof(draft_pass), "%s", value);
	}
	settings_refresh();
	switch_screen_return_to(settings_screen);
}

static void edit_left_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(edit_field, "");
}

// ---------------------------------------------------------------------------
// the settings page: connect saves the draft, the sign-in rows drive the flow
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

// The whole sign-in runs from this device: the Apple ID and password typed
// here, the 2FA code Apple sends typed here, and the daemon only doing the
// network legwork on behalf of this session.
static void login_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	if (!draft_url[0]) {
		toast_error("amp_not_configured");
		return;
	}
	if (!draft_user[0] || !draft_pass[0]) {
		toast_error("amp_missing_credentials");
		return;
	}
	draft_save();
	job_t job = {.kind = JOB_LOGIN};
	snprintf(job.username, sizeof(job.username), "%s", draft_user);
	snprintf(job.password, sizeof(job.password), "%s", draft_pass);
	// The password lives in the job from here; the draft forgets it.
	memset(draft_pass, 0, sizeof(draft_pass));
	settings_refresh();
	submit(&job);
	memset(&job, 0, sizeof(job));
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	job_t job = {.kind = JOB_LOGOUT};
	submit(&job);
}

// The 2FA hop: digits only, six of them, and nothing kept after the job.
static void open_2fa_box(void) {
	entering_2fa = true;
	lv_label_set_text(edit_title, tr("amp_2fa_title"));
	lv_textarea_set_text(edit_field, "");
	lv_textarea_set_placeholder_text(edit_field, "123456");
	lv_textarea_set_accepted_chars(edit_field, "0123456789");
	lv_textarea_set_max_length(edit_field, 6);
	lv_textarea_set_password_mode(edit_field, false);
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
	switch_screen(edit_screen);
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
	// The account row reads a fresh /me rather than remembering last time:
	// the daemon's session can lapse, or be restored, behind our back. The
	// same job doubles as the connection test.
	if (draft_url[0]) {
		job_t job = {.kind = JOB_ME};
		submit(&job);
	}
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
	// storefront the catalog is read from, and the account signed in here.
	settings_screen = screen();
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "amp_wrapper_settings");
	row = settingsrow_add(container, "amp_wrapper_url", &wrapper_value, edit_cb, (void *)(intptr_t)0);
	settingsrow_add_icon(row, &icon_wifi_high, THEME_SEMANTIC_BLUE);
	row = settingsrow_add(container, "amp_decrypt_port", &port_value, edit_cb, (void *)(intptr_t)1);
	settingsrow_add_icon(row, &icon_music_settings, THEME_SEMANTIC_PURPLE);
	row = settingsrow_add(container, "amp_storefront", &store_value, edit_cb, (void *)(intptr_t)2);
	settingsrow_add_icon(row, &icon_music_note, THEME_SEMANTIC_CYAN);
	row = settingsrow_add(container, "amp_account", &account_value, NULL, NULL);
	settingsrow_add_icon(row, &icon_artist, THEME_SEMANTIC_GREEN);
	row = settingsrow_add(container, "amp_username", &user_value, edit_cb, (void *)(intptr_t)3);
	settingsrow_add_icon(row, &icon_artist, THEME_SEMANTIC_BLUE);
	row = settingsrow_add(container, "amp_password", &pass_value, edit_cb, (void *)(intptr_t)4);
	settingsrow_add_icon(row, &icon_eye, THEME_SEMANTIC_PURPLE);
	row = settingsrow_action(container, "amp_sign_in", login_cb, NULL);
	settingsrow_add_icon(row, &icon_check, THEME_SEMANTIC_GREEN);
	row = settingsrow_action(container, "amp_sign_out", logout_cb, NULL);
	settingsrow_add_icon(row, &icon_close, THEME_SEMANTIC_RED);
	row = settingsrow_action(container, "amp_connect_save", test_cb, NULL);
	settingsrow_add_icon(row, &icon_wifi_high, THEME_SEMANTIC_GREEN);
	add_note(container, "amp_settings_note");
	add_note(container, "amp_2fa_note");
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
				if (entering_2fa) {
					open_2fa_box(); // a rejected code asks to be typed again
				}
			} else if (result->job.kind == JOB_SEARCH) {
				free(shown);
				shown = result->songs;
				result->songs = NULL;
				shown_count = result->count;
				rebuild_list();
			} else if (result->job.kind == JOB_LOGIN) {
				if (result->count == 1) {
					apply_state(result);
					open_2fa_box(); // Apple wants the second factor
				} else {
					toast_success("amp_signed_in_ok");
				}
			} else if (result->job.kind == JOB_2FA) {
				toast_success("amp_signed_in_ok");
			} else if (result->job.kind == JOB_LOGOUT) {
				toast_success("amp_signed_out");
			} else if (result->job.kind == JOB_ME) {
				if (result->job.serial == latest && settings_screen &&
					lv_screen_active() == settings_screen) {
					// the connection test and the account refresh
					toast_success(result->signed_in ? "amp_connected" : "amp_not_signed_in");
				}
			}
			// The account row follows the daemon's session state, whatever
			// the job was.
			apply_state(result);
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
