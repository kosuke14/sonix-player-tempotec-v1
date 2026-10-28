#ifndef AMPWRAPPER_H
#define AMPWRAPPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The client of wrapper-v2, the daemon that runs Apple Music's FairPlay
// libraries on a machine with room for them and offers this player two small
// doors: an HTTP side for the account and the playback dispatch, and a raw
// TCP side where encrypted audio samples go in and plaintext comes out.
//
// The daemon itself is external infrastructure -- it lives wherever the user
// runs it, in Docker on a PC, a NAS or a small server. Nothing Apple-signed
// is carried in this firmware and nothing here could run it: the libraries
// are arm64 Android builds and this player is a MIPS device. This module is
// therefore deliberately thin -- a few REST calls and one binary protocol --
// with the stream assembly (HLS, fMP4, the batching policy) living in
// amphls.c, ampfp4.c and ampstream.c.

// ---------------------------------------------------------------------------
// the HTTP side
// ---------------------------------------------------------------------------

// GET /me: who the daemon is serving right now. The tokens are what the
// catalog API (ampapi.c) authenticates with, and both can rotate whenever the
// daemon re-authenticates, so read them fresh rather than caching for long.
typedef struct {
	bool authenticated; // auth.state is anything but "logged_out"
	char state[32];
	char version[32];   // daemon API version, e.g. "0.0.2"
	char dev_token[1536];
	char music_user_token[1536];
	char apple_id[128];
} ampw_me_t;

bool ampw_me(const char *base_url, ampw_me_t *out, char *error, size_t error_size);

// GET /playback?adam_id=... -- the raw MZ dispatch for one track, JSON. The
// body is malloc'd and belongs to the caller; amphls.c picks the master
// playlist URL out of songList[0]. The daemon answers 401 when its session
// has lapsed and 502 when Apple refused, both of which surface here as false.
bool ampw_playback(const char *base_url, const char *adam_id, char **body, size_t *body_len,
				   char *error, size_t error_size);

typedef enum {
	AMPW_LOGIN_OK = 0,
	AMPW_LOGIN_2FA,	  // the daemon is waiting for POST /login/2fa
	AMPW_LOGIN_FAILED,
} ampw_login_result_t;

// Sign-in is the daemon's business (it drives Apple's own credential flow);
// this player only carries the fields and reports which kind of answer came
// back, so the settings page can put up its 2FA box. The player supplies the
// Apple ID, the password and the 2FA code from its own keyboard -- the
// daemon is just the network peer that runs the flow.
ampw_login_result_t ampw_login(const char *base_url, const char *username, const char *password,
							   char *error, size_t error_size);
ampw_login_result_t ampw_login_2fa(const char *base_url, const char *code, char *error,
								   size_t error_size);

// DELETE /login: aborts an in-flight login or clears the daemon's in-memory
// tokens. The player's Sign out row calls this; the Apple-side caches on the
// daemon's disk are untouched, so a later sign-in reuses the device trust.
bool ampw_logout(const char *base_url, char *error, size_t error_size);

// ---------------------------------------------------------------------------
// the TCP decrypt side (the WV2D protocol)
// ---------------------------------------------------------------------------

// One open decrypt connection. Requests and replies are strictly ordered, so
// one session serves one stream at a time; a fresh track opens a fresh one.
typedef struct ampw_decrypt ampw_decrypt_t;

// Connects to the decrypt port. `stale`/`user` are polled between socket
// waits so a canceled stream never hangs the producer thread longer than one
// socket timeout.
ampw_decrypt_t *ampw_decrypt_open(const char *host, int port, bool (*stale)(void *user),
								  void *user);
void ampw_decrypt_close(ampw_decrypt_t *d);

// One round trip. `samples`/`lens` carry `count` runs of ALIGNED ciphertext
// -- the 16-byte-block parts of each sample's protected ranges, prepared by
// the caller (ampfp4.c) -- and the reply carries the same number of
// plaintexts, malloc'd, one buffer per sample, in order. The reassembly of
// clear ranges and tails around them is the caller's.
//
// Any protocol hiccup (bad magic, mismatched id, daemon error frame, closed
// socket) fails the whole session: the daemon discards workers on errors too,
// so the safe move is close + reopen, not retry-on-the-same-wire.
bool ampw_decrypt_batch(ampw_decrypt_t *d, const char *adam_id, const char *skd_uri,
						const uint8_t **samples, const size_t *lens, int count, uint8_t **plains,
						size_t *plain_lens, char *error, size_t error_size);

#endif // AMPWRAPPER_H
