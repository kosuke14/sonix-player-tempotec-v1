/* Stubs for everything quickpanel.c and topbar.c reach into the rest of the
 * player for. Signatures follow the real headers so the compiler catches any
 * drift; no stub does more than answer neutrally -- the harness only exercises
 * hit-testing, never playback. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/audio/peqpage.h"
#include "src/gui/wireless/airplay.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/wireless/wifitransfer.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/remote/airplay.h"
#include "src/system/remote/dlna.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/eq.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/audio/usbdac.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/device/bootlogo.h"
#include "src/system/device/clock.h"
#include "src/system/device/led.h"
#include "src/system/device/power.h"
#include "src/system/device/usb.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzsync.h"
#include "src/system/streaming/radio.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalsync.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"

/* --- fonts: the faces the shell measures itself against (as the UI tests) --- */
lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;

void fonts_register_change(void (*cb)(void)) { (void)cb; }

/* --- screens the long-press handlers open; never in this harness --- */
lv_obj_t *airplay_screen;
lv_obj_t *audiobooksettings_screen;
lv_obj_t *btsettings_screen;
lv_obj_t *dlna_screen;
lv_obj_t *sonixlink_screen;
lv_obj_t *wifisettings_screen;
lv_obj_t *wifitransfer_screen;

/* --- control-centre actions --- */
void switch_screen(lv_obj_t *target_screen) { (void)target_screen; }
void switcher_set_player_return(lv_obj_t *screen) { (void)screen; }
bool gui_post(void (*cb)(void *), void *user) { (void)cb; (void)user; return true; }
void gui_notify_popup(const char *text) { (void)text; }
void confirm_show(const char *title, const char *message, const char *ok_label,
		  void (*on_ok)(void *user), void *user) {
	(void)title; (void)message; (void)ok_label; (void)on_ok; (void)user;
}
void scrolltext_set(lv_obj_t *label, const char *text) {
	(void)label;
	(void)text;
}
void scrolltext_apply(lv_obj_t *label) { (void)label; }

/* --- player / transport --- */
void player_key_play_pause(void) {}
void player_screen_next(void) {}
void player_screen_prev(void) {}
void player_refresh_now_playing(void) {}
bool player_sheet_is_open(void) { return false; }
void player_sheet_close(bool animate) { (void)animate; }
void player_output_unplugged(const char *what) { (void)what; }
bool player_current_podcast_feed(long long *id_out, podcast_feed_t *feed_out) {
	(void)id_out;
	(void)feed_out;
	return false;
}

/* --- playback state --- */
void device_state_get(device_state_t *state) { memset(state, 0, sizeof(*state)); }
void device_state_refresh_battery(void) {}
const char *metadata_shown_artist(const song_metadata_t *m) { (void)m; return ""; }
playback_mode_t playlist_cycle_mode(void) { return (playback_mode_t)0; }
playback_mode_t playlist_get_mode(void) { return (playback_mode_t)0; }
int audiobook_skip_back(void) { return 0; }
int audiobook_skip_forward(void) { return 0; }
int podcast_skip_back(void) { return 0; }
int podcast_skip_forward(void) { return 0; }

/* --- sleep timer --- */
bool sleeptimer_enabled(sleeptimer_kind_t kind) { (void)kind; return false; }
int sleeptimer_minutes(sleeptimer_kind_t kind) { (void)kind; return 0; }
void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on) { (void)kind; (void)on; }

/* --- power / backlight --- */
long power_get_max_brightness(void) { return 100; }
long power_get_min_brightness(void) { return 5; }
long power_get_brightness(void) { return 50; }
void power_set_brightness(long value) { (void)value; }
bool power_charging_held(void) { return false; }
void power_pause_in_standby(lv_timer_t *timer) { (void)timer; }
void power_slow_in_standby(lv_timer_t *timer, uint32_t standby_period_ms) {
	(void)timer;
	(void)standby_period_ms;
}

/* --- radios and switches the circle buttons read --- */
bool wifi_available(void) { return false; }
bool wifi_get_enabled(void) { return false; }
void wifi_get_status(wifi_status_t *out) { memset(out, 0, sizeof(*out)); }
void wifi_set_enabled(bool on) { (void)on; }
bool bluetooth_available(void) { return false; }
bool bluetooth_get_enabled(void) { return false; }
void bluetooth_set_enabled(bool on) { (void)on; }
bt_state_t bluetooth_get_state(void) { return (bt_state_t)0; }
int bluetooth_get_codecs(char out[][BT_CODEC_MAX], int max, char *selected, int selected_size) {
	(void)out;
	(void)max;
	(void)selected;
	(void)selected_size;
	return 0;
}
bool bluetooth_audio_active(void) { return false; }
bool bluetooth_receiver_stream(bt_stream_t *out) {
	(void)out;
	return false;
}
bool airplay_available(void) { return false; }
bool airplay_get_enabled(void) { return false; }
void airplay_set_enabled(bool on) { (void)on; }
void airplay_get_state(airplay_state_t *out) { memset(out, 0, sizeof(*out)); }
bool eq_get_enabled(void) { return false; }
bool mseb_get_enabled(void) { return false; }
bool peq_get_enabled(void) { return false; }
void peq_set_enabled(bool enabled) { (void)enabled; }
bool dlna_get_enabled(void) { return false; }
void dlna_set_enabled(bool on) { (void)on; }
bool sonixlink_get_enabled(void) { return false; }
bool sonixlink_is_connected(void) { return false; }
void sonixlink_set_enabled(bool on) { (void)on; }
bool wifitransfer_get_enabled(void) { return false; }
bool wifitransfer_available(void) { return false; }
void wifitransfer_page_open_started(void) {}
void wifitransfer_page_stop(void) {}
bool musicsettings_fade_enabled(void) { return false; }
void musicsettings_set_fade_enabled(bool enabled) { (void)enabled; }
bool musicsettings_gapless_enabled(void) { return false; }
void musicsettings_set_gapless_enabled(bool enabled) { (void)enabled; }
bool musicsettings_high_gain(void) { return false; }
void musicsettings_set_high_gain(bool enabled) { (void)enabled; }
void musicsettings_set_eq_enabled(bool enabled) { (void)enabled; }
void musicsettings_set_mseb_enabled(bool enabled) { (void)enabled; }
lv_obj_t *musicsettings_eq_screen(void) { return NULL; }
lv_obj_t *musicsettings_mseb_screen(void) { return NULL; }
lv_obj_t *musicsettings_fade_screen(void) { return NULL; }
lv_obj_t *musicsettings_playback_screen(void) { return NULL; }
lv_obj_t *peqpage_screen(void) { return NULL; }
lv_obj_t *podcastpage_settings_screen(void) { return NULL; }

/* --- audio path --- */
int get_volume_percent(void) { return 50; }
void volume_profile_persist(void) {}
int headphone_jack_state(void) { return 0; }
bool lineout_is_active(void) { return false; }
void lineout_set(bool on) { (void)on; }
void lineout_check_jack(void) {}
bool usbdac_is_active(void) { return false; }
void usbdac_get_state(usbdac_state_t *out) { memset(out, 0, sizeof(*out)); }
bool usbaudio_active(void) { return false; }
void usbaudio_poll(void) {}
bool btreceiver_is_active(void) { return false; }
bool usb_vbus_present(void) { return false; }
int audio_get_dsd_multiple(void) { return 0; }
void audio_get_output_device(char *out, size_t out_size) {
	if (out && out_size) {
		out[0] = '\0';
	}
}
int audio_get_stream_bits(void) { return 0; }
bool audio_stream_is_lossy(void) { return false; }

/* --- clock / leds --- */
bool clock_is_set(void) { return false; }
void clock_remember(void) {}
bool clock_resync(void) { return false; }
void clock_format_hm(char *out, size_t out_size, int hour, int minute) {
	(void)hour;
	(void)minute;
	if (out && out_size) {
		out[0] = '\0';
	}
}
void led_set_bt_receiver(bool active) { (void)active; }
void led_set_charging(bool charging) { (void)charging; }
void led_set_dac(bool active, bool streaming, int sample_rate) {
	(void)active;
	(void)streaming;
	(void)sample_rate;
}
void led_set_on_charger(bool present) { (void)present; }
void led_set_wifi_transfer(bool active) { (void)active; }
void led_update_playback(bool playing, int sample_rate, bool lossy, int bits, bool podcast, bool dsd) {
	(void)playing; (void)sample_rate; (void)lossy; (void)bits; (void)podcast; (void)dsd;
}
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }

/* --- radio / streaming bookkeeping --- */

bool audiobook_is_playing(void) { return false; }
bool dlna_available(void) { return false; }

bool radio_can_step(void) { return false; }
bool radio_is_connecting(void) { return false; }
bool radio_current_station(radio_station_t *out) { (void)out; return false; }
bool radio_fav_contains(const char *uuid) { (void)uuid; return false; }
void radio_fav_add(const radio_station_t *station) { (void)station; }
void radio_fav_remove(const char *uuid) { (void)uuid; }
bool podcastcache_is_episode(const char *path) { (void)path; return false; }
void podcastsubs_follow(const podcast_feed_t *feed) { (void)feed; }
void podcastsubs_unfollow(long long feed_id) { (void)feed_id; }
bool podcastsubs_is_followed(long long feed_id) { (void)feed_id; return false; }
long qobuzcache_track_id(const char *path) { (void)path; return 0; }
bool qobuzsync_is_favorite(long track_id) { (void)track_id; return false; }
void qobuzsync_favorite_toggle(long track_id, bool want, qobuzsync_done_cb done, void *user) {
	(void)track_id; (void)want; (void)done; (void)user;
}
long tidalcache_track_id(const char *path) { (void)path; return 0; }
bool tidalsync_is_favorite(long track_id) { (void)track_id; return false; }
void tidalsync_favorite_toggle(long track_id, bool want, tidalsync_done_cb done, void *user) {
	(void)track_id; (void)want; (void)done; (void)user;
}
void tidalsync_favorites_refresh(void (*done)(void)) { (void)done; }
bool library_fav_contains(const char *path) { (void)path; return false; }
bool library_fav_toggle(const char *path, const char *name, const char *artist) {
	(void)path;
	(void)name;
	(void)artist;
	return false;
}
