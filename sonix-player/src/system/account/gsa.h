#ifndef GSA_H
#define GSA_H

// The GrandSlam client: Apple ID sign-in spoken natively -- SRP-6a (srp.c)
// framed in XML plist (plistxml.c) over the player's own HTTP stack. No
// Apple code runs for any of this.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The anisette headers every GrandSlam request carries. They come from the
// anisette module, which assembles the full ten the protocol wants: the
// machine data fetched from the configured anisette server, and the rest
// derived on the device.
#define GSA_ANISETTE_MAX 12

// The identity the GrandSlam route accepts: Apple's fraud detection of
// October 2026 refuses anisette provisioned through the Android ADI when
// the request claims a Mac, and GrandSlam answers -80045 to every such
// login. The Windows claim is what the Windows Apple Music's own AuthKit
// sends, and the provisioning must claim the same identity it logs in as.
#define GSA_CLIENT_INFO \
	"<PC> <Windows;6.2(0,0);9200> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"
#define GSA_CLIENT_INFO_MAC \
	"<MacBookPro13,2> <macOS;13.1;22C65> <com.apple.AuthKit/1 (com.apple.akd/1.0)>"

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
	// The init answer's pieces, what the complete step consumes: the caller
	// runs init once and the complete reuses its session -- the server
	// refuses a second init that carries the same A.
	char session[512]; // the c cookie the init hands out
	uint8_t salt[64];
	int salt_len;
	uint8_t b_pub[256];
	int b_len;
	long long iters;
	// The session's own keys from the spd, what the app-token requests sign
	// with. Zeroed after use by the caller that is done with them.
	uint8_t sk[64];
	int sk_len;
	uint8_t c[64];
	int c_len;
} gsa_login_t;

// The two sign-in steps. `a` is the caller's 32 random bytes. Init keeps
// the server's answer (the session cookie, the salt, B, the iterations) in
// `out`; complete consumes them -- the server refuses a second init with
// the same A, so the pair is one-shot.
bool gsa_login_init(const gsa_anisette_t *anisette, const char *username, const uint8_t *a,
					size_t a_len, gsa_login_t *out, char *error, size_t error_size);
bool gsa_login_complete(const gsa_anisette_t *anisette, const char *username,
						const char *password, const uint8_t *a, size_t a_len,
						gsa_login_t *out, char *error, size_t error_size);

// The second factor: the six-digit code from a trusted device.
bool gsa_2fa_totp(const gsa_anisette_t *anisette, const gsa_login_t *login, const char *code,
				  char *error, size_t error_size);

// The app token an Apple service rides on, the "apptokens" operation: asked
// with the session's identity (adsid + GsIdmsToken) and signed with the
// session key, it returns the token `app_name` recognises. The music app's
// name is what the per-account catalog token comes through.
bool gsa_app_token(const gsa_anisette_t *anisette, const gsa_login_t *login,
				   const char *app_name, char *out, size_t out_size, char *error,
				   size_t error_size);

#endif /* GSA_H */
