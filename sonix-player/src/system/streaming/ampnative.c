#include "ampnative.h"

#include "src/system/account/gsa.h"
#include "anisette.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// The session on the card is four things: the identity pair, the session
// keys, and the music token. Everything but the music token is long-lived;
// the token is fetched fresh, the way a one-time password would not be.

#define AMPNATIVE_DIR ".local/amp-account"
#define MUSIC_APP_NAME "com.apple.music.gnome"

static char account_root[512];
static bool root_ready;

void ampnative_set_root(const char *sd_root) {
	snprintf(account_root, sizeof(account_root), "%s", sd_root && sd_root[0] ? sd_root : "");
	root_ready = account_root[0] != '\0';
}

static void session_path(char *out, size_t size) {
	snprintf(out, size, "%s/" AMPNATIVE_DIR "/session.txt", account_root);
}

bool ampnative_signed_in(void) {
	char path[768];
	session_path(path, sizeof(path));
	struct stat st;
	return root_ready && stat(path, &st) == 0 && st.st_size > 0;
}

// The session file, one key per line. The GsIdmsToken runs long, so the
// lines are read generously.
typedef struct {
	char adsid[128];
	char idms[1024];
	char pt[1024];
	char music[2048];
	uint8_t sk[64];
	int sk_len;
	uint8_t c[64];
	int c_len;
} native_session_t;

static void hex_encode(const uint8_t *in, size_t len, char *out, size_t out_size) {
	for (size_t i = 0; i < len && (i * 2 + 2) < out_size; i++) {
		snprintf(out + i * 2, 3, "%02x", in[i]);
	}
}

static int hex_val(char ch) {
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	return -1;
}

static bool hex_decode(const char *in, uint8_t *out, size_t out_size, size_t *out_len) {
	size_t n = 0;
	for (size_t i = 0; in[i] && in[i + 1] && n < out_size; i += 2) {
		int hi = hex_val(in[i]), lo = hex_val(in[i + 1]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		out[n++] = (uint8_t)(hi * 16 + lo);
	}
	*out_len = n;
	return n > 0;
}

static bool session_save(const native_session_t *s) {
	if (!root_ready) {
		return false;
	}
	char dir[640];
	if (mkdir(account_root, 0755) != 0 && errno != EEXIST) {
		return false;
	}
	snprintf(dir, sizeof(dir), "%s/.local", account_root);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		return false;
	}
	snprintf(dir, sizeof(dir), "%s/" AMPNATIVE_DIR, account_root);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		return false;
	}
	char path[768];
	session_path(path, sizeof(path));
	char tmp[800];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return false;
	}
	char sk_hex[129], c_hex[129];
	hex_encode(s->sk, (size_t)s->sk_len, sk_hex, sizeof(sk_hex));
	hex_encode(s->c, (size_t)s->c_len, c_hex, sizeof(c_hex));
	fprintf(f, "adsid=%s\nidms=%s\npt=%s\nmusic=%s\nsk=%s\nc=%s\n", s->adsid, s->idms, s->pt,
			s->music, sk_hex, c_hex);
	bool ok = fclose(f) == 0;
	if (ok) {
		ok = rename(tmp, path) == 0;
	}
	if (!ok) {
		remove(tmp);
	}
	return ok;
}

static bool session_load(native_session_t *s) {
	memset(s, 0, sizeof(*s));
	if (!root_ready) {
		return false;
	}
	char path[768];
	session_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[2300];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *value = eq + 1;
		if (strcmp(line, "adsid") == 0) {
			snprintf(s->adsid, sizeof(s->adsid), "%s", value);
		} else if (strcmp(line, "idms") == 0) {
			snprintf(s->idms, sizeof(s->idms), "%s", value);
		} else if (strcmp(line, "pt") == 0) {
			snprintf(s->pt, sizeof(s->pt), "%s", value);
		} else if (strcmp(line, "music") == 0) {
			snprintf(s->music, sizeof(s->music), "%s", value);
		} else if (strcmp(line, "sk") == 0) {
			size_t len = 0;
			hex_decode(value, s->sk, sizeof(s->sk), &len);
			s->sk_len = (int)len;
		} else if (strcmp(line, "c") == 0) {
			size_t len = 0;
			hex_decode(value, s->c, sizeof(s->c), &len);
			s->c_len = (int)len;
		}
	}
	fclose(f);
	return s->adsid[0] && s->idms[0];
}

// The login struct the session came in on, kept only as long as the save.
static void login_to_session(const gsa_login_t *login, native_session_t *s) {
	memset(s, 0, sizeof(*s));
	snprintf(s->adsid, sizeof(s->adsid), "%s", login->adsid);
	snprintf(s->idms, sizeof(s->idms), "%s", login->idms_token);
	snprintf(s->pt, sizeof(s->pt), "%s", login->pt_token);
	if (login->sk_len > 0 && (size_t)login->sk_len <= sizeof(s->sk)) {
		memcpy(s->sk, login->sk, (size_t)login->sk_len);
		s->sk_len = login->sk_len;
	}
	if (login->c_len > 0 && (size_t)login->c_len <= sizeof(s->c)) {
		memcpy(s->c, login->c, (size_t)login->c_len);
		s->c_len = login->c_len;
	}
}

// The machine data, provisioned on demand: the first sign-in on a card
// provisions before it signs in.
static bool anisette_ready(gsa_anisette_t *out, char *error, size_t error_size) {
	if (anisette_headers(out, error, error_size)) {
		return true;
	}
	if (strcmp(error, "anisette_not_provisioned") != 0) {
		return false;
	}
	if (!anisette_provision(error, error_size)) {
		return false;
	}
	return anisette_headers(out, error, error_size);
}

static ampnative_result_t run_login(const char *apple_id, const char *password, char *error,
									size_t error_size) {
	gsa_anisette_t anisette;
	if (!anisette_ready(&anisette, error, error_size)) {
		return AMPNATIVE_FAILED;
	}
	uint8_t a[32];
	FILE *ur = fopen("/dev/urandom", "rb");
	if (!ur || fread(a, 1, sizeof(a), ur) != sizeof(a)) {
		if (ur) {
			fclose(ur);
		}
		snprintf(error, error_size, "ampnative_entropy");
		return AMPNATIVE_FAILED;
	}
	fclose(ur);

	gsa_login_t login;
	memset(&login, 0, sizeof(login));
	if (!gsa_login_init(&anisette, apple_id, a, sizeof(a), &login, error, error_size) ||
		!gsa_login_complete(&anisette, apple_id, password, a, sizeof(a), &login, error,
							error_size)) {
		memset(a, 0, sizeof(a));
		return AMPNATIVE_FAILED;
	}
	memset(a, 0, sizeof(a));
	if (login.result == GSA_NEEDS_2FA_TOTP || login.result == GSA_NEEDS_2FA_SMS) {
		// the identity half of the session is kept: the validate hop needs it
		native_session_t s;
		login_to_session(&login, &s);
		session_save(&s);
		memset(&login, 0, sizeof(login));
		return AMPNATIVE_2FA;
	}
	if (login.result != GSA_OK) {
		return AMPNATIVE_FAILED;
	}
	// Signed in: the music token is asked for at once, while the session is
	// warm, and everything is written to the card together.
	native_session_t s;
	login_to_session(&login, &s);
	char token[2048] = "";
	if (gsa_app_token(&anisette, &login, MUSIC_APP_NAME, token, sizeof(token), error,
					  error_size)) {
		snprintf(s.music, sizeof(s.music), "%s", token);
	}
	memset(&login, 0, sizeof(login)); // the keys and tokens never linger
	bool saved = session_save(&s);
	memset(&s, 0, sizeof(s));
	if (!saved) {
		snprintf(error, error_size, "ampnative_no_card");
		return AMPNATIVE_FAILED;
	}
	return AMPNATIVE_OK;
}

ampnative_result_t ampnative_signin(const char *apple_id, const char *password, char *error,
									size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!apple_id || !apple_id[0] || !password || !password[0]) {
		snprintf(error, error_size, "ampnative_bad_args");
		return AMPNATIVE_FAILED;
	}
	ampnative_result_t r = run_login(apple_id, password, error, error_size);
	if (r == AMPNATIVE_2FA) {
		// The server wants the second factor first: validate with the code
		// the user is about to type happens in ampnative_2fa, and this run
		// simply reports the ask.
		return r;
	}
	return r;
}

ampnative_result_t ampnative_2fa(const char *apple_id, const char *password, const char *code,
								 char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	gsa_anisette_t anisette;
	if (!anisette_ready(&anisette, error, error_size)) {
		return AMPNATIVE_FAILED;
	}
	native_session_t s;
	if (!session_load(&s)) {
		snprintf(error, error_size, "ampnative_no_session");
		return AMPNATIVE_FAILED;
	}
	gsa_login_t login;
	memset(&login, 0, sizeof(login));
	snprintf(login.adsid, sizeof(login.adsid), "%s", s.adsid);
	snprintf(login.idms_token, sizeof(login.idms_token), "%s", s.idms);
	if (!gsa_2fa_totp(&anisette, &login, code, error, error_size)) {
		memset(&login, 0, sizeof(login));
		return AMPNATIVE_FAILED;
	}
	memset(&login, 0, sizeof(login));
	// The second factor is accepted; the sign-in runs again, and this time
	// the server lets it through.
	return run_login(apple_id, password, error, error_size);
}

bool ampnative_music_token(char *out, size_t out_size, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!out || !out_size) {
		return false;
	}
	out[0] = '\0';
	native_session_t s;
	if (!session_load(&s) || !s.music[0]) {
		snprintf(error, error_size, "ampnative_no_session");
		return false;
	}
	snprintf(out, out_size, "%s", s.music);
	return true;
}

void ampnative_logout(void) {
	if (!root_ready) {
		return;
	}
	char path[768];
	session_path(path, sizeof(path));
	remove(path);
}
