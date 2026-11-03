#ifndef GSA_H
#define GSA_H

// The GrandSlam client: Apple ID sign-in spoken natively -- SRP-6a (srp.c)
// framed in XML plist (plistxml.c) over the player's own HTTP stack. No
// Apple code runs for any of this.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The anisette headers every GrandSlam request carries. For now they come
// from a provider the caller fills (a remote anisette server's answer, or
// user-supplied machine data); the native ADI dance is a later slab.
#define GSA_ANISETTE_MAX 8

typedef struct {
	char key[64];
	char value[512];
} gsa_anisette_kv_t;

typedef struct {
	gsa_anisette_kv_t kv[GSA_ANISETTE_MAX];
	int count;
} gsa_anisette_t;

typedef enum {
	GSA_OK = 0,          // signed in, no second factor asked
	GSA_NEEDS_2FA_TOTP,  // trusted-device code
	GSA_NEEDS_2FA_SMS,   // sms code
	GSA_FAILED,
} gsa_result_t;

typedef struct {
	gsa_result_t result;
	char error[192];
	char adsid[128];
	char idms_token[1024]; // GsIdmsToken, the identity token for the 2FA hop
	char pt_token[1024];   // PasswordToken from the spd, when present
} gsa_login_t;

// The two sign-in steps. `a` is the caller's 32 random bytes; the SRP and
// PBKDF2 runs happen inside.
bool gsa_login_init(const gsa_anisette_t *anisette, const char *username, const uint8_t *a,
					size_t a_len, char *session_id_out, size_t session_id_size, char *error,
					size_t error_size);
bool gsa_login_complete(const gsa_anisette_t *anisette, const char *username,
						const char *password, const uint8_t *a, size_t a_len,
						const char *session_id, gsa_login_t *out, char *error,
						size_t error_size);

// The second factor: the six-digit code from a trusted device.
bool gsa_2fa_totp(const gsa_anisette_t *anisette, const gsa_login_t *login, const char *code,
				  char *error, size_t error_size);

#endif /* GSA_H */
