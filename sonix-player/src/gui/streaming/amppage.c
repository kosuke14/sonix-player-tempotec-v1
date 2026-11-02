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
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/ampapi.h"
#include "src/system/streaming/ampguest.h"
#include "src/system/streaming/ampstream.h"
#include "src/system/streaming/amplocal.h"
#include "src/system/streaming/streamturn.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lv_obj_t *amp_screen;
static lv_obj_t *settings_screen, *edit_screen, *list_screen, *login_screen;
static lv_obj_t *list_container, *list_title, *store_value;
static lv_obj_t *account_value, *guest_value;
static lv_obj_t *setup_row, *login_row, *signout_row, *test_row, *store_row, *account_row;
static lv_obj_t *quality_card, *lyrics_card, *lyrics_switch, *note_quality, *note_2fa,
	*note_settings;
static lv_obj_t *quality_pill[2], *quality_pills_parent;
static lv_obj_t *login_user_field, *login_pass_field, *login_eye_icon, *remember_switch;
static lv_obj_t *edit_field, *edit_title;
static keyboard_t *keyboard, *login_keyboard;
static gui_config_t *config;
static bool busy;
static int editing;
static bool remember_checked; // the private file exists; the switch mirrors it
static char pending_user[128], pending_pass[128]; // wiped once the outcome is in

// One page of search results, which is what the low-end screen needs.
#define AMP_PAGE 20

// The same single-mailbox worker shape the other streaming pages use: results
// own their memory until the GUI takes them, and a late worker can never
// overwrite a list the GUI is reading.
typedef enum { JOB_SEARCH, JOB_ALBUM, JOB_ME, JOB_LOGIN, JOB_2FA, JOB_LOGOUT } job_kind_t;
typedef struct {
	unsigned serial;
	job_kind_t kind;
	char term[64];
	char album_id[32]; // the catalog album a JOB_ALBUM page lists
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
			if (job.kind == JOB_LOGIN) {
				// The sign-in flow runs on the guest daemon, but every field
				// the user types comes from this device's keyboard: the
				// player is the only place the password exists, and it is
				// wiped right here.
				amplocal_login_result_t r = amplocal_signin(job.username, job.password,
															result->error,
															sizeof(result->error));
				memset(job.password, 0, sizeof(job.password));
				result->count = r == AMPLOCAL_LOGIN_OK ? 0 : r == AMPLOCAL_LOGIN_2FA ? 1 : -1;
			} else if (job.kind == JOB_2FA) {
				result->count = amplocal_2fa(job.code, result->error,
											 sizeof(result->error)) == AMPLOCAL_LOGIN_OK ? 0 : -1;
				memset(job.code, 0, sizeof(job.code));
			} else if (job.kind == JOB_LOGOUT) {
				result->count = amplocal_logout(result->error, sizeof(result->error)) ? 0 : -1;
			} else {
				amplocal_me_t me;
				// The tokens rotate whenever the daemon re-authenticates, so
				// every job reads them fresh; they are wiped before the result
				// is sent.
				if (!amplocal_me(&me, result->error, sizeof(result->error))) {
					result->count = -1;
				} else {
					result->signed_in = me.authenticated;
					snprintf(result->apple_id, sizeof(result->apple_id), "%s", me.apple_id);
					if (job.kind == JOB_ME) {
						result->count = 0; // the state snapshot is the whole answer
					} else if (!me.authenticated) {
						result->count = -1;
						snprintf(result->error, sizeof(result->error), "amp_not_authenticated");
					} else if (job.kind == JOB_ALBUM) {
						char storefront[32];
						snprintf(storefront, sizeof(storefront), "%.20s",
								 config_get("applemusic", "storefront", ampapi_default_storefront()));
						result->songs = calloc(AMP_PAGE, sizeof(*result->songs));
						result->count =
							result->songs
								? ampapi_album_tracks(me.dev_token, me.music_user_token,
													  storefront, job.album_id, result->songs,
													  AMP_PAGE, result->error,
													  sizeof(result->error))
								: -1;
					} else {
						char storefront[32];
						snprintf(storefront, sizeof(storefront), "%.20s",
								 config_get("applemusic", "storefront", ampapi_default_storefront()));
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
			// account row is refreshed from a fresh snapshot inside the job.
			if (job.kind == JOB_LOGIN || job.kind == JOB_2FA || job.kind == JOB_LOGOUT) {
				if (result->count >= 0) {
					amplocal_me_t me;
					if (amplocal_me(&me, result->error, sizeof(result->error))) {
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
	job_owner = (job->kind == JOB_SEARCH || job->kind == JOB_ALBUM) ? list_screen
																   : settings_screen;
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
// the settings draft: the guest config is saved; the credentials are only
// when the Remember-password switch says so, in a private file of their own
// ---------------------------------------------------------------------------

static char draft_store[32];
static char draft_user[128], draft_pass[128]; // the login screen's fields
static char creds_path[600];

// The remembered password lives beside the config as a 0600 JSON file, the
// subsonic page's pattern: a private file the user opted into, never a config
// key, written through a .tmp and renamed so a half-write reads as nothing.
static void creds_path_init(void) {
	creds_path[0] = '\0';
	const char *path = config_path();
	if (path && path[0] && strlen(path) < sizeof(creds_path) - 40) {
		snprintf(creds_path, sizeof(creds_path), "%s", path);
		char *slash = strrchr(creds_path, '/');
		if (slash) {
			slash[1] = '\0';
		} else {
			creds_path[0] = '\0';
		}
		strcat(creds_path, "amp-credentials.json");
	}
}

// A JSON string with the quotes escaped. False when the card refused a byte.
static bool creds_write_string(FILE *f, const char *s) {
	if (fputc('"', f) == EOF) {
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			if (fputc('\\', f) == EOF || fputc(*p, f) == EOF) {
				return false;
			}
		} else if (*p < 32) {
			if (fprintf(f, "\\u%04x", *p) < 0) {
				return false;
			}
		} else if (fputc(*p, f) == EOF) {
			return false;
		}
	}
	return fputc('"', f) != EOF;
}

static bool creds_save(const char *user, const char *pass) {
	if (!creds_path[0] || !user[0] || !pass[0]) {
		return false;
	}
	char tmp[sizeof(creds_path) + 8];
	snprintf(tmp, sizeof(tmp), "%s.tmp", creds_path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		return false;
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		return false;
	}
	bool ok = fputs("{\"username\":", f) >= 0 && creds_write_string(f, user) &&
			  fputs(",\"password\":", f) >= 0 && creds_write_string(f, pass) &&
			  fputs("}\n", f) >= 0;
	if (fflush(f) != 0 || fsync(fd) != 0) {
		ok = false;
	}
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok && rename(tmp, creds_path) != 0) {
		ok = false;
	}
	if (!ok) {
		unlink(tmp);
	}
	memset(tmp, 0, sizeof(tmp));
	return ok;
}

// Reads the file back. Anything amiss -- size, permissions, shape -- reads as
// nothing remembered, and the file is removed rather than retried.
static bool creds_load(char *user, size_t user_size, char *pass, size_t pass_size) {
	user[0] = '\0';
	pass[0] = '\0';
	if (!creds_path[0]) {
		return false;
	}
	int fd = open(creds_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	struct stat st;
	bool ok = false;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 8192 &&
		(st.st_mode & 0777) == 0600) {
		char text[8192];
		FILE *f = fdopen(fd, "r");
		if (f) {
			size_t n = fread(text, 1, sizeof(text) - 1, f);
			text[n] = '\0';
			json_doc_t doc;
			if (!ferror(f) && json_parse(text, &doc)) {
				int root = json_root(&doc);
				ok = json_str_exact(&doc, json_get(&doc, root, "username"), user, user_size) &&
					 json_str_exact(&doc, json_get(&doc, root, "password"), pass, pass_size) &&
					 user[0] && pass[0];
				json_free(&doc);
			}
			fclose(f);
			fd = -1;
			ok = ok || (unlink(creds_path) == 0);
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	if (!ok) {
		user[0] = '\0';
		pass[0] = '\0';
	}
	return ok;
}

static void creds_forget(void) {
	if (creds_path[0]) {
		unlink(creds_path);
	}
	remember_checked = false;
}

static void draft_load(void) {
	ampstream_config(draft_store, sizeof(draft_store));
	remember_checked = creds_load(draft_user, sizeof(draft_user), draft_pass, sizeof(draft_pass));
	if (!remember_checked) {
		draft_user[0] = '\0';
		draft_pass[0] = '\0';
	}
}

static void draft_save(void) {
	config_set("applemusic", "storefront", draft_store);
}

static bool configured(void) {
	// The guest tree is the whole feature: without it there is no daemon to
	// sign in on and nothing to decrypt with. The flag alone lies after the
	// tree is deleted from the card -- the setup row must come back.
	return config_get_int("applemusic", "translate", 0) != 0 && ampguest_present();
}

// The page shows only what the state can use: before the guest tree exists,
// nothing but the setup row; with the tree but no session, nothing but the
// sign-in; signed in, the settings themselves. The rows are all built once
// and the refresh decides who is on the page.
static void settings_refresh(void) {
	bool tree = configured();
	bool signed_in = session_signed_in;
	lv_obj_t *const setup_only[] = {setup_row};
	lv_obj_t *const login_only[] = {login_row, note_2fa};
	lv_obj_t *const signed_rows[] = {store_row,	 account_row,	 quality_card,
									 lyrics_card, signout_row,	 test_row,
									 note_settings, note_quality};
	for (size_t i = 0; i < sizeof(setup_only) / sizeof(*setup_only); i++) {
		if (setup_only[i]) {
			lv_obj_set_hidden(setup_only[i], tree);
		}
	}
	for (size_t i = 0; i < sizeof(login_only) / sizeof(*login_only); i++) {
		if (login_only[i]) {
			lv_obj_set_hidden(login_only[i], !tree || signed_in);
		}
	}
	for (size_t i = 0; i < sizeof(signed_rows) / sizeof(*signed_rows); i++) {
		if (signed_rows[i]) {
			lv_obj_set_hidden(signed_rows[i], !signed_in);
		}
	}
	lv_label_set_text(store_value, draft_store[0] ? draft_store : tr("amp_not_set"));
	// The guest-tree row: what the card carries right now, or the fetch in
	// progress.
	if (ampguest_busy()) {
		lv_label_set_text(guest_value, tr("amp_guest_running"));
	} else {
		lv_label_set_text(guest_value, tr(ampguest_present() ? "amp_guest_installed"
															 : "amp_not_set"));
	}
	// The account row mirrors the daemon's session as the last job saw it.
	if (session_signed_in && session_apple_id[0]) {
		lv_label_set_text(account_value, session_apple_id);
	} else {
		lv_label_set_text(account_value, tr(session_signed_in ? "amp_signed_in"
															  : "amp_not_signed_in"));
	}
	// The quality pills: the current setting is the lit one.
	bool heaac = config_get_int("applemusic", "quality", 0) == 1;
	settingsrow_pill_active(quality_pill[0], !heaac);
	settingsrow_pill_active(quality_pill[1], heaac);
	// The lyrics toggle follows the card, the switch follows the setting.
	if (lyrics_switch) {
		if (config_get_int("applemusic", "lyrics", 1) != 0) {
			lv_obj_add_state(lyrics_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(lyrics_switch, LV_STATE_CHECKED);
		}
	}
	if (remember_switch) {
		if (remember_checked) {
			lv_obj_add_state(remember_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(remember_switch, LV_STATE_CHECKED);
		}
	}
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
	lv_label_set_text(edit_title, tr("amp_storefront"));
	const char *text = draft_store;
	lv_textarea_set_accepted_chars(edit_field, NULL);
	lv_textarea_set_max_length(edit_field, 127);
	lv_textarea_set_text(edit_field, text);
	// Readable, like the subsonic page's, so a mistyped password is seen
	// before it is sent.
	lv_textarea_set_password_mode(edit_field, false);
	lv_textarea_set_placeholder_text(edit_field, tr("amp_storefront"));
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
		// The storefront is tiny ("jp", "us"): refuse overlong values rather
		// than truncate them into a different store.
		if (strlen(value) >= sizeof(draft_store)) {
			toast_error("amp_bad_storefront");
			return;
		}
		snprintf(draft_store, sizeof(draft_store), "%s", value);
	} else if (editing == 1) {
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

static void login_field_focus_cb(lv_event_t *e);

// A login field, the qobuz page's shape: one line, card-dressed, caret under
// the keyboard's control.
static lv_obj_t *login_field(lv_obj_t *parent, gui_config_t *cfg, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, cfg->screen_width < 320 ? 44 : 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, cfg->screen_width < 320 ? 8 : 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, login_field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

// The two codecs this device decodes. The master playlist also carries
// lossless and Dolby Atmos variants; the note under the pills says so rather
// than offering choices that would refuse to play.
static void quality_pill_cb(lv_event_t *e) {
	int value = (int)(intptr_t)lv_event_get_user_data(e);
	config_set_int("applemusic", "quality", value == 1 ? 1 : 0);
	config_save();
	settings_refresh();
}

static void lyrics_cb(lv_event_t *e) {
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	config_set_int("applemusic", "lyrics", on ? 1 : 0);
	config_save();
}

// ---------------------------------------------------------------------------
// the settings page: connect saves the draft, the sign-in rows drive the flow
// ---------------------------------------------------------------------------

// The guest tree, fetched by the player itself: the space check, the download
// and the unpack all happen on the fetcher's own thread; the poll below
// mirrors the progress into the row and toasts the outcome.
static void guest_download_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	if (ampguest_busy()) {
		return; // already running; the row shows the progress
	}
	// A tree that is already here does not need a second download: re-fetching
	// it would cost a hundred megabytes and several minutes to arrive at the
	// very same files. The tap itself asks to use it, which matters for a
	// tree fetched before this row switched the feature on by itself.
	if (ampguest_present()) {
		if (config_get_int("applemusic", "translate", 0) == 0) {
			config_set_int("applemusic", "translate", 1);
			config_save();
			amplocal_init();
		}
		toast_success("amp_guest_installed");
		return;
	}
	if (ampguest_fetch_start()) {
		toast_success("amp_guest_started");
	}
	settings_refresh();
}

static void test_cb(lv_event_t *e) {
	(void)e;
	if (busy) {
		return;
	}
	if (!configured()) {
		toast_error("amp_not_configured"); // no guest tree: nothing to test
		return;
	}
	draft_save();
	job_t job = {.kind = JOB_ME};
	submit(&job);
}

// The whole sign-in runs from this device: the Apple ID and password typed
// here, the 2FA code Apple sends typed here, and the daemon only doing the
// network legwork on behalf of this session.
// Sign-in happens on its own screen, the qobuz page's shape: two fields, the
// password shown as dots from the first keystroke with an eye to reveal, and
// the remember switch under them. The keyboard's ok starts the flow.
static void login_eye_refresh(void) {
	bool hidden = lv_textarea_get_password_mode(login_pass_field);
	lv_image_set_src(login_eye_icon, hidden ? &icon_eye : &icon_eye_off);
	keyboard_refresh_password(login_pass_field);
}

static void login_eye_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_password_mode(login_pass_field, !lv_textarea_get_password_mode(login_pass_field));
	login_eye_refresh();
}

static void login_focus_field(lv_obj_t *field) {
	lv_obj_t *other = field == login_user_field ? login_pass_field : login_user_field;
	keyboard_set_field(login_keyboard, field);
	lv_obj_add_state(field, LV_STATE_FOCUSED);
	lv_obj_remove_state(other, LV_STATE_FOCUSED);
	keyboard_show_caret(field, true);
	keyboard_show_caret(other, false);
}

static void login_field_focus_cb(lv_event_t *e) {
	login_focus_field(lv_event_get_target(e));
}

static void remember_maybe_save(void) {
	if (remember_checked && pending_user[0] && pending_pass[0]) {
		creds_save(pending_user, pending_pass);
	}
	memset(pending_user, 0, sizeof(pending_user));
	memset(pending_pass, 0, sizeof(pending_pass));
}

static void remember_cb(lv_event_t *e) {
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	if (!on) {
		creds_forget();
		return;
	}
	const char *user = lv_textarea_get_text(login_user_field);
	const char *pass = lv_textarea_get_text(login_pass_field);
	if (user[0] && pass[0]) {
		remember_checked = true;
		creds_save(user, pass);
		return;
	}
	// Nothing typed yet: remembering starts when a sign-in succeeds.
	remember_checked = false;
	lv_obj_remove_state(lv_event_get_target(e), LV_STATE_CHECKED);
	toast_error("amp_remember_after_signin");
}

static void open_login_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	// The remembered credentials prefill the fields; without them the sign-in
	// starts blank and the switch is off.
	draft_load();
	lv_textarea_set_text(login_user_field, draft_user);
	lv_textarea_set_text(login_pass_field, remember_checked ? draft_pass : "");
	lv_textarea_set_password_mode(login_pass_field, true);
	login_eye_refresh();
	keyboard_reset(login_keyboard);
	login_focus_field(login_user_field);
	switch_screen(login_screen);
}

static void login_accept_cb(lv_event_t *e) {
	(void)e;
	if (busy) {
		return;
	}
	const char *user = lv_textarea_get_text(login_user_field);
	const char *pass = lv_textarea_get_text(login_pass_field);
	if (!user[0] || !pass[0]) {
		toast_error("amp_missing_credentials");
		return;
	}
	if (!configured()) {
		toast_error("amp_not_configured");
		return;
	}
	remember_checked = lv_obj_has_state(remember_switch, LV_STATE_CHECKED);
	snprintf(pending_user, sizeof(pending_user), "%s", user);
	snprintf(pending_pass, sizeof(pending_pass), "%s", pass);
	job_t job = {.kind = JOB_LOGIN};
	snprintf(job.username, sizeof(job.username), "%s", user);
	snprintf(job.password, sizeof(job.password), "%s", pass);
	// The password leaves the field the moment the job takes it.
	lv_textarea_set_text(login_pass_field, "");
	lv_textarea_set_text(login_user_field, "");
	submit(&job);
	memset(&job, 0, sizeof(job));
	switch_screen(settings_screen);
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

// The album page: the catalog album's tracks in the list the search uses.
// Called from the track menu on a playing Apple Music track.
bool amppage_open_album(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || busy || !configured()) {
		return false;
	}
	free(shown);
	shown = NULL;
	shown_count = 0;
	lv_obj_clean(list_container);
	lv_label_set_text(list_title, tr("applemusic_searching"));
	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
	// Closing the player sheet comes back to the album list, the way the
	// Qobuz album page does.
	switcher_set_player_return(list_screen);
	job_t job = {.kind = JOB_ALBUM};
	snprintf(job.album_id, sizeof(job.album_id), "%s", album_id);
	snprintf(job.term, sizeof(job.term), "%s",
			 title && title[0] ? title : tr("applemusic_search"));
	submit(&job);
	return true;
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
static lv_obj_t *add_note(lv_obj_t *container, const char *key) {
	lv_obj_t *note = lv_label_create(container);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_obj_set_width(note, lv_pct(100));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_label_set_text(note, tr(key));
	return note;
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	draft_load();
	settings_refresh();
	switch_screen(settings_screen);
	// The account row reads a fresh snapshot rather than remembering last
	// time: the daemon's session can lapse, or be restored, behind our back.
	// The same job doubles as the connection test.
	if (configured() && session_signed_in) {
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

	// The settings page, in three states the refresh switches between: setup
	// (no guest tree yet), sign-in (tree but no session) and the settings
	// themselves. The decrypt port stays at its fixed 10020 -- ampmocal owns
	// it and the guest tree brings the daemon up on it.
	settings_screen = screen();
	lv_obj_t *container = settingsrow_page(settings_screen, config, "amp_wrapper_settings");
	setup_row = settingsrow_add(container, "amp_setup", &guest_value, guest_download_cb, NULL);
	settingsrow_add_icon(setup_row, &icon_import, THEME_SEMANTIC_GREEN);
	lyrics_card = NULL; // built below in signed-in order
	settingsrow_toggle(container, "amp_remember_password", &remember_switch, remember_cb);
	login_row = settingsrow_action(container, "amp_sign_in", open_login_cb, NULL);
	settingsrow_add_icon(login_row, &icon_check, THEME_SEMANTIC_GREEN);
	store_row = settingsrow_add(container, "amp_storefront", &store_value, edit_cb, (void *)(intptr_t)0);
	settingsrow_add_icon(store_row, &icon_music_note, THEME_SEMANTIC_CYAN);
	account_row = settingsrow_add(container, "amp_account", &account_value, NULL, NULL);
	settingsrow_add_icon(account_row, &icon_artist, THEME_SEMANTIC_GREEN);

	// Sound quality: the two codecs the device decodes. The master playlist
	// also carries lossless and Dolby Atmos variants, and the note says
	// plainly that this device cannot play them.
	quality_card = settingsrow_pills(container, "amp_quality", &quality_pills_parent);
	quality_pill[0] = settingsrow_pill_text(quality_pills_parent, tr("amp_quality_aac"), 0,
											quality_pill_cb);
	quality_pill[1] = settingsrow_pill_text(quality_pills_parent, tr("amp_quality_heaac"), 1,
											quality_pill_cb);
	note_quality = add_note(container, "amp_quality_note");

	lyrics_card = settingsrow_toggle(container, "amp_lyrics", &lyrics_switch, lyrics_cb);
	settingsrow_add_icon(lyrics_card, &icon_music_note, THEME_SEMANTIC_PURPLE);
	signout_row = settingsrow_action(container, "amp_sign_out", logout_cb, NULL);
	settingsrow_add_icon(signout_row, &icon_close, THEME_SEMANTIC_RED);
	test_row = settingsrow_action(container, "amp_connect_save", test_cb, NULL);
	settingsrow_add_icon(test_row, &icon_wifi_high, THEME_SEMANTIC_GREEN);
	note_settings = add_note(container, "amp_settings_note");
	note_2fa = add_note(container, "amp_2fa_note");
	player_sheet_attach_drag(container, true);

	// The sign-in screen: Apple ID and password, dots and the eye, the
	// remember switch, the keyboard's ok to start.
	login_screen = screen();
	settingsrow_title(login_screen, config, "amp_sign_in");
	int top = settingsrow_content_top(config);
	int pitch = config->screen_width < 320 ? 50 : 78;
	login_user_field = login_field(login_screen, config, "amp_username", top);
	login_pass_field = login_field(login_screen, config, "amp_password", top + pitch);
	keyboard_style_password(login_pass_field, 0);
	lv_obj_set_style_pad_right(login_pass_field, 60, 0);
	lv_obj_t *eye = lv_btn_create(login_screen);
	lv_obj_set_size(eye, config->screen_width < 320 ? 44 : 56, config->screen_width < 320 ? 44 : 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -config->padding, top + pitch);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_add_event_cb(eye, login_eye_cb, LV_EVENT_CLICKED, NULL);
	login_eye_icon = lv_image_create(eye);
	lv_obj_add_style(login_eye_icon, &theme_style_icon, 0);
	lv_obj_center(login_eye_icon);
	login_keyboard = keyboard_create(login_screen, config->screen_width,
									 config->screen_width < 320 ? 144 : 316, login_user_field,
									 NULL, "ok", login_accept_cb, NULL);
	switcher_attach_back_gesture(login_screen);

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
			} else if (result->job.kind == JOB_SEARCH || result->job.kind == JOB_ALBUM) {
				free(shown);
				shown = result->songs;
				result->songs = NULL;
				shown_count = result->count;
				if (result->job.kind == JOB_ALBUM) {
					lv_label_set_text(list_title, result->job.term);
				}
				rebuild_list();
			} else if (result->job.kind == JOB_LOGIN) {
				if (result->count == 1) {
					remember_maybe_save(); // the password was accepted; keep it if asked
					apply_state(result);
					open_2fa_box(); // Apple wants the second factor
				} else {
					remember_maybe_save();
					toast_success("amp_signed_in_ok");
				}
			} else if (result->job.kind == JOB_2FA) {
				remember_maybe_save();
				toast_success("amp_signed_in_ok");
			} else if (result->job.kind == JOB_LOGOUT) {
				creds_forget(); // a signed-out account leaves nothing behind
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
	// The guest-tree fetch reports through the same one-news-take pattern the
	// producer uses; the row follows it either way.
	char guest_error[64];
	if (ampguest_take_result(guest_error, sizeof(guest_error))) {
		toast_busy_end();
		if (guest_error[0]) {
			toast_error(guest_error);
		} else {
			// A landed tree is the whole on-device feature: from here sign-in
			// and playback are allowed, so the switch flips itself on and the
			// decrypt listener comes up now rather than at the next playback.
			// Until it did, a fresh download still ended at a refused
			// sign-in, which read like the fetch had failed.
			config_set_int("applemusic", "translate", 1);
			config_save();
			amplocal_init();
			toast_success("amp_guest_ready");
		}
	}
	if (guest_value && lv_screen_active() == settings_screen) {
		if (ampguest_busy()) {
			int percent = ampguest_progress();
			char line[16];
			if (percent >= 0) {
				snprintf(line, sizeof(line), "%d%%", percent);
			} else {
				snprintf(line, sizeof(line), "%s", tr("amp_guest_running"));
			}
			lv_label_set_text(guest_value, line);
		} else {
			lv_label_set_text(guest_value,
							  tr(ampguest_present() ? "amp_guest_installed" : "amp_not_set"));
		}
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
