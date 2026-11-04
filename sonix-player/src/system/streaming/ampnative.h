#ifndef AMPNATIVE_H
#define AMPNATIVE_H

#include <stdbool.h>
#include <stddef.h>

// The native sign-in: the GrandSlam flow and the anisette machine data, run
// on the player itself. No emulated daemons, nothing translated -- the same
// requests a Mac's AuthKit sends, made with the player's own sockets.
//
// What a sign-in leaves behind on the card is the session: the identity
// (adsid + GsIdmsToken), the session keys, and the music app token they
// vouch for. The music token is what the per-account catalog answers need.

typedef enum {
	AMPNATIVE_OK = 0,   // signed in, no second factor asked
	AMPNATIVE_2FA,      // a code from a trusted device is wanted next
	AMPNATIVE_FAILED,
} ampnative_result_t;

// Where the card is: the session lives under <root>/.local/amp-account.
void ampnative_set_root(const char *sd_root);

// Signs in with the typed Apple ID and password. Both are wiped by the
// caller as soon as this returns. On AMPNATIVE_2FA the session state is
// kept and ampnative_2fa finishes the flow with the code.
ampnative_result_t ampnative_signin(const char *apple_id, const char *password, char *error,
									size_t error_size);

// The six-digit code from a trusted device. The identity is validated with
// it, then the sign-in runs again -- the server remembers the second factor.
ampnative_result_t ampnative_2fa(const char *apple_id, const char *password, const char *code,
								 char *error, size_t error_size);

// True when the card holds a session.
bool ampnative_signed_in(void);

// The music app token, fetched fresh from the session (the token is what
// the per-account catalog calls carry). False when there is no session or
// the token cannot be had.
bool ampnative_music_token(char *out, size_t out_size, char *error, size_t error_size);

// Forgets the session on the card.
void ampnative_logout(void);

#endif // AMPNATIVE_H
