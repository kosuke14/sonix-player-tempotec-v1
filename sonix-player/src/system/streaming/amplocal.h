#ifndef AMPLOCAL_H
#define AMPLOCAL_H

#include <stdbool.h>
#include <stddef.h>

// The on-device Apple Music side ([applemusic] translate=1).
//
// When the user has placed the guest tree on the card, this module spawns
// the translated guest daemons behind the scenes -- qemu-arm running the
// Apple Music 3.6.0 auth set (sign-in, tokens, the playback dispatch) and
// the 6.5.3 static decrypt set (FairPlay sample decryption) -- and relays
// the player's requests to them. The FairPlay SPC/CKC exchange with Apple's
// key server runs here, over the player's own TLS stack -- the daemons
// inside the guest have no network at all.
//
// Two processes, never one: the two Apple generations must not be resident
// together (their ICU singletons fight), so each channel spawns its own
// guest daemon and the pair never share a process.
//
// Everything is opt-in and lazily started: translate=0 (the default) makes
// this module a no-op.

// Read the [applemusic] settings and open the listener socket when
// translate is on. Returns false without a listener otherwise (never an
// error for the caller: it just means "not translating").
bool amplocal_init(void);

// Called from the GUI tick: respawns the daemons after a crash, enforces the
// idle kill. Cheap; does no I/O unless work is pending.
void amplocal_tick(void);

// Stops the listener and kills the daemons (service switch / shutdown).
void amplocal_shutdown(void);

// True while the guest daemons are spawned and expected to keep running.
bool amplocal_running(void);

// ---------------------------------------------------------------------------
// the auth channel (the 3.6.0 set: sign-in, tokens, playback dispatch)
// ---------------------------------------------------------------------------

typedef enum {
	AMPLOCAL_LOGIN_OK = 0,
	AMPLOCAL_LOGIN_2FA,	  // the flow is parked waiting for a six-digit code
	AMPLOCAL_LOGIN_FAILED,
} amplocal_login_result_t;

// Sign-in. Runs the whole flow to a terminal state: submits the credentials,
// then polls until Apple's answer (or the 2FA park). `error` carries a
// translation-ready tag on FAILED.
amplocal_login_result_t amplocal_signin(const char *username, const char *password,
										char *error, size_t error_size);

// Submits the 2FA code for a parked flow, then waits the same way.
amplocal_login_result_t amplocal_2fa(const char *code, char *error, size_t error_size);

// Clears the daemon's in-memory tokens (the Apple-side session cache on the
// card is untouched, so a later sign-in reuses the device trust).
bool amplocal_logout(char *error, size_t error_size);

// One account snapshot. The tokens rotate whenever the daemon re-authenticates,
// so callers read them fresh rather than caching for long.
typedef struct {
	bool authenticated;
	char state[32];
	char dev_token[1536];
	char music_user_token[1536];
	char apple_id[128];
	char storefront[32];
	char dsid[64];
} amplocal_me_t;

bool amplocal_me(amplocal_me_t *out, char *error, size_t error_size);

// The playback dispatch for one track: the raw MZ dispatch JSON, malloc'd,
// owned by the caller. amphls.c picks the master playlist URL out of
// songList[0]. False on refusal, with the reason in `error`.
bool amplocal_playback(const char *adam_id, char **body, size_t *body_len, char *error,
					   size_t error_size);

#endif // AMPLOCAL_H
