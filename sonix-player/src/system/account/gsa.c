#include "gsa.h"

#include "plistxml.h"
#include "pbkdf2.h"
#include "srp.h"
#include "src/system/core/json.h"
#include "src/system/core/sha256.h"
#include "src/system/net/http.h"
#include "src/system/net/tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GSA_ENDPOINT "https://gsa.apple.com/grandslam/GsService2"
#define GSA_VALIDATE_ENDPOINT "https://gsa.apple.com/grandslam/GsService2/validate"
#define GSA_USER_AGENT "akd/1.0 CFNetwork/978.0.7 Darwin/18.7.0"

// ---------------------------------------------------------------------------
// AES-256-CBC decrypt, for the spd blob the login answer carries. A compact
// table-driven AES; decryption only, one block at a time.
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t rk[60];
} aes256_t;

static const uint8_t AES_SBOX[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t gmul(uint8_t a, uint8_t b) {
	uint8_t p = 0;
	for (int i = 0; i < 8; i++) {
		if (b & 1) {
			p ^= a;
		}
		uint8_t hi = a & 0x80;
		a = (uint8_t)(a << 1);
		if (hi) {
			a ^= 0x1b;
		}
		b >>= 1;
	}
	return p;
}

static void aes256_init(aes256_t *ctx, const uint8_t key[32]) {
	memcpy(ctx->rk, key, 32);
	uint32_t rcon = 1;
	for (int i = 8; i < 60; i++) {
		uint32_t t = ctx->rk[i - 1];
		if (i % 8 == 0) {
			t = ((uint32_t)AES_SBOX[(t >> 16) & 255] << 24) |
				((uint32_t)AES_SBOX[(t >> 8) & 255] << 16) |
				((uint32_t)AES_SBOX[t & 255] << 8) |
				((uint32_t)AES_SBOX[(t >> 24) & 255]);
			t ^= rcon << 24;
			rcon = (uint8_t)(rcon << 1) ^ ((rcon & 0x80) ? 0x1b : 0);
		} else if (i % 8 == 4) {
			t = ((uint32_t)AES_SBOX[(t >> 24) & 255] << 24) |
				((uint32_t)AES_SBOX[(t >> 16) & 255] << 16) |
				((uint32_t)AES_SBOX[(t >> 8) & 255] << 8) |
				((uint32_t)AES_SBOX[t & 255]);
		}
		ctx->rk[i] = ctx->rk[i - 8] ^ t;
	}
}

static void aes256_decrypt_block(const aes256_t *ctx, uint8_t block[16]) {
	// AddRoundKey(Nr), then InvCipher rounds: ShiftRows/InvSubBytes inverse
	// order handled with the equivalent straight decrypt loop ( InvShiftRows
	// + InvSubBytes + AddRoundKey, then InvMixColumns ).
	static const uint8_t RS[4][16] = {
		{0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11},
		{0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11},
	};
	(void)RS;
	uint8_t s[16];
	memcpy(s, block, 16);
	// inverse shift rows table: where each output byte comes from
	static const uint8_t INV_SR[16] = {0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3};
	static const uint8_t INV_SBOX[256] = {
		0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
		0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
		0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
		0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
		0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
		0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
		0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
		0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
		0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
		0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
		0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
		0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
		0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
		0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
		0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
		0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
	};
	for (int round = 14; round >= 1; round--) {
		// AddRoundKey
		for (int i = 0; i < 16; i++) {
			uint32_t rk = ctx->rk[round * 4 + i / 4];
			s[i] ^= (uint8_t)(rk >> (24 - 8 * (i % 4)));
		}
		// InvShiftRows + InvSubBytes
		uint8_t t[16];
		for (int i = 0; i < 16; i++) {
			t[i] = INV_SBOX[s[INV_SR[i]]];
		}
		// InvMixColumns (skipped on the last round)
		if (round > 1) {
			for (int c = 0; c < 4; c++) {
				uint8_t a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
				t[c * 4] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
				t[c * 4 + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
				t[c * 4 + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
				t[c * 4 + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
			}
		}
		memcpy(s, t, 16);
	}
	for (int i = 0; i < 16; i++) {
		uint32_t rk = ctx->rk[i / 4];
		s[i] ^= (uint8_t)(rk >> (24 - 8 * (i % 4)));
	}
	memcpy(block, s, 16);
}

static void aes256_cbc_decrypt(const aes256_t *ctx, const uint8_t iv[16], const uint8_t *in,
							   size_t len, uint8_t *out) {
	uint8_t prev[16];
	memcpy(prev, iv, 16);
	for (size_t off = 0; off + 16 <= len; off += 16) {
		uint8_t block[16];
		memcpy(block, in + off, 16);
		aes256_decrypt_block(ctx, block);
		for (int i = 0; i < 16; i++) {
			out[off + i] = (uint8_t)(block[i] ^ prev[i]);
			prev[i] = in[off + i];
		}
	}
}

// Apple's gsa chain answers with a self-signed root the system store does
// not carry -- the reference implementations verify nothing there either.
// The player's global mode is relaxed for exactly the duration of each call
// and restored after, so the rest of the device stays as strict as it was.
typedef struct {
	tls_verify_t saved;
} gsa_tls_guard_t;

static void gsa_tls_begin(gsa_tls_guard_t *g) {
	g->saved = tls_verify_mode();
	tls_set_verify_mode(TLS_VERIFY_RELAXED);
}

static void gsa_tls_end(const gsa_tls_guard_t *g) {
	tls_set_verify_mode(g->saved);
}

// ---------------------------------------------------------------------------
// hmac-sha256 (for the extra-data keys)
// ---------------------------------------------------------------------------

static void hmac_sha256(const uint8_t *key, size_t key_len, const char *msg, uint8_t out[32]) {
	uint8_t k[64], pad[64];
	memset(k, 0, sizeof(k));
	memcpy(k, key, key_len > 64 ? 64 : key_len);
	for (int i = 0; i < 64; i++) {
		pad[i] = k[i] ^ 0x5c;
		k[i] ^= 0x36;
	}
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, k, 64);
	sha256_update(&c, msg, strlen(msg));
	uint8_t inner[32];
	sha256_final_raw(&c, inner);
	sha256_init(&c);
	sha256_update(&c, pad, 64);
	sha256_update(&c, inner, 32);
	sha256_final_raw(&c, out);
	memset(k, 0, sizeof(k));
	memset(pad, 0, sizeof(pad));
}

// ---------------------------------------------------------------------------
// the request plumbing
// ---------------------------------------------------------------------------

static void anisette_to_plist(plistw_t *w, const gsa_anisette_t *an) {
	plistw_dict_begin(w, "cpd");
	for (int i = 0; i < an->count; i++) {
		// the anisette keys arrive lower-cased; GrandSlam wants them that way
		plistw_string(w, an->kv[i].key, an->kv[i].value);
	}
	plistw_dict_end(w);
}

static bool gsa_post(const char *endpoint, const char *body, plist_t *reply, char *error,
					 size_t error_size) {
	// The XML declaration and the doctype go on the front: the plist the
	// writer emits is the bare dict, and Apple's parsers are particular.
	char *full = malloc(strlen(body) + 256);
	if (!full) {
		snprintf(error, error_size, "gsa_memory");
		return false;
	}
	snprintf(full, strlen(body) + 256,
			 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
			 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
			 "<plist version=\"1.0\">\n%s</plist>",
			 body);
	char extra[2048];
	snprintf(extra, sizeof(extra),
			 "User-Agent: %s\r\n"
			 "X-MMe-Client-Info: <MacBookPro18,3> <macOS;13.4.1;22F82> <com.apple.dt.Xcode/3594.4.19>\r\n",
			 GSA_USER_AGENT);
	http_req_t req;
	memset(&req, 0, sizeof(req));
	req.method = "POST";
	req.extra_headers = extra;
	req.body = full;
	req.body_len = strlen(full);
	req.content_type = "text/x-xml-plist";
	req.want_error_body = true; // the Status/ec/em of a refusal is the useful part
	char *out = NULL;
	size_t out_len = 0;
	int status = 0;
	gsa_tls_guard_t tls_guard;
	gsa_tls_begin(&tls_guard);
	bool sent = http_request(endpoint, &req, &out, &out_len, 256 * 1024, 30, &status);
	gsa_tls_end(&tls_guard);
	if (!sent) {
		// the transport reason rides along: a failure before any HTTP status
		// is the interesting kind, and the code alone would hide it
		const char *why = http_last_error();
		snprintf(error, error_size, "gsa_http_%d%s%s", status, why ? " " : "",
				 why ? why : "");
		free(full);
		return false;
	}
	bool ok = plist_parse(out, out_len, reply);
	if (!ok) {
		snprintf(error, error_size, "gsa_bad_plist");
	}
	free(full);
	free(out);
	return ok;
}

static bool check_error(plist_t *reply, char *error, size_t error_size) {
	int status = plist_get(reply, "Status");
	if (status < 0) {
		return true; // some answers carry the fields at the root
	}
	int ec = plist_get_in(reply, status, "ec");
	if (ec >= 0 && plist_int(reply, ec) != 0) {
		int em = plist_get_in(reply, status, "em");
		snprintf(error, error_size, "gsa_ec_%lld: %s", plist_int(reply, ec),
				 plist_str(reply, em) ? plist_str(reply, em) : "?");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// the login flow
// ---------------------------------------------------------------------------

bool gsa_login_init(const gsa_anisette_t *anisette, const char *username, const uint8_t *a,
					size_t a_len, char *session_id_out, size_t session_id_size, char *error,
					size_t error_size) {
	(void)session_id_out;
	(void)session_id_size;
	uint8_t a_pub[256];
	memset(a_pub, 0, sizeof(a_pub));
	srp_public_a(a, a_len, a_pub);

	plistw_t w;
	plistw_init(&w);
	plistw_dict_begin(&w, NULL);
	plistw_dict_begin(&w, "Header");
	plistw_string(&w, "Version", "1.0.1");
	plistw_dict_end(&w);
	plistw_dict_begin(&w, "Request");
	plistw_data(&w, "A", a_pub, 256);
	anisette_to_plist(&w, anisette);
	plistw_string(&w, "o", "init");
	plistw_array_begin(&w, "ps");
	plistw_string(&w, NULL, "s2k");
	plistw_string(&w, NULL, "s2k_fo");
	plistw_array_end(&w);
	plistw_string(&w, "u", username);
	plistw_dict_end(&w);
	plistw_dict_end(&w);

	plist_t reply;
	memset(&reply, 0, sizeof(reply));
	bool ok = gsa_post(GSA_ENDPOINT, w.buf, &reply, error, error_size);
	plistw_free(&w);
	if (!ok) {
		return false;
	}
	ok = check_error(&reply, error, error_size);
	int c = plist_get(&reply, "c");
	if (ok && c >= 0 && plist_str(&reply, c)) {
		snprintf(session_id_out, session_id_size, "%s", plist_str(&reply, c));
	}
	plist_free(&reply);
	return ok;
}

// The srp session's private parts, parked between the two steps: the caller
// re-runs them through the same `a`, so the complete step re-derives from
// the reply directly.
bool gsa_login_complete(const gsa_anisette_t *anisette, const char *username,
						const char *password, const uint8_t *a, size_t a_len,
						const char *session_id, gsa_login_t *out, char *error,
						size_t error_size) {
	(void)session_id;
	memset(out, 0, sizeof(*out));

	// the verifier password: PBKDF2-HMAC-SHA256 over SHA-256(password)
	uint8_t hpw[32];
	{
		sha256_ctx c;
		sha256_init(&c);
		sha256_update(&c, password, strlen(password));
		sha256_final_raw(&c, hpw);
	}
	// the salt and B arrive with the init answer; a full flow re-fetches them.
	// The caller of the two-step API holds them; this single-call variant
	// re-runs init inside for now (the protocol is stateless per request).
	(void)a_len;

	plist_t reply;
	memset(&reply, 0, sizeof(reply));
	uint8_t a_pub[256];
	memset(a_pub, 0, sizeof(a_pub));
	srp_public_a(a, a_len, a_pub);

	plistw_t w;
	plistw_init(&w);
	plistw_dict_begin(&w, NULL);
	plistw_dict_begin(&w, "Header");
	plistw_string(&w, "Version", "1.0.1");
	plistw_dict_end(&w);
	plistw_dict_begin(&w, "Request");
	plistw_data(&w, "A", a_pub, 256);
	anisette_to_plist(&w, anisette);
	plistw_string(&w, "o", "init");
	plistw_array_begin(&w, "ps");
	plistw_string(&w, NULL, "s2k");
	plistw_string(&w, NULL, "s2k_fo");
	plistw_array_end(&w);
	plistw_string(&w, "u", username);
	plistw_dict_end(&w);
	plistw_dict_end(&w);
	if (!gsa_post(GSA_ENDPOINT, w.buf, &reply, error, error_size)) {
		snprintf(out->error, sizeof(out->error), "%s", error);
		out->result = GSA_FAILED;
		plistw_free(&w);
		return false;
	}
	plistw_free(&w);
	int c_node = plist_get(&reply, "c");
	int s_node = plist_get(&reply, "s");
	int b_node = plist_get(&reply, "B");
	int i_node = plist_get(&reply, "i");
	if (c_node < 0 || s_node < 0 || b_node < 0) {
		snprintf(out->error, sizeof(out->error), "gsa_init_missing_fields");
		out->result = GSA_FAILED;
		plist_free(&reply);
		return false;
	}
	const char *c = plist_str(&reply, c_node);
	size_t salt_len = 0;
	const uint8_t *salt = plist_data(&reply, s_node, &salt_len);
	size_t b_len = 0;
	const uint8_t *b_pub = plist_data(&reply, b_node, &b_len);
	long long iters = i_node >= 0 ? plist_int(&reply, i_node) : 2048;

	// the verifier password
	uint8_t pbk[32];
	pbkdf2_hmac_sha256(hpw, 32, salt, salt_len, (unsigned)(iters > 0 ? iters : 2048), pbk, 32);

	srp_session_t srp;
	memset(&srp, 0, sizeof(srp));
	if (!srp_process_reply(a, a_len, username, pbk, 32, salt, salt_len, b_pub, b_len, &srp)) {
		snprintf(out->error, sizeof(out->error), "gsa_bad_b");
		out->result = GSA_FAILED;
		plist_free(&reply);
		return false;
	}
	plist_free(&reply);

	// the complete step: M1 proof
	plistw_init(&w);
	plistw_dict_begin(&w, NULL);
	plistw_dict_begin(&w, "Header");
	plistw_string(&w, "Version", "1.0.1");
	plistw_dict_end(&w);
	plistw_dict_begin(&w, "Request");
	plistw_data(&w, "m", srp.m1, 32);
	plistw_string(&w, "c", c ? c : "");
	anisette_to_plist(&w, anisette);
	plistw_string(&w, "o", "complete");
	plistw_string(&w, "u", username);
	plistw_dict_end(&w);
	plistw_dict_end(&w);
	memset(&reply, 0, sizeof(reply));
	if (!gsa_post(GSA_ENDPOINT, w.buf, &reply, error, error_size)) {
		snprintf(out->error, sizeof(out->error), "%s", error);
		out->result = GSA_FAILED;
		plistw_free(&w);
		return false;
	}
	plistw_free(&w);
	if (!check_error(&reply, error, error_size)) {
		snprintf(out->error, sizeof(out->error), "%s", error);
		out->result = GSA_FAILED;
		plist_free(&reply);
		return false;
	}

	// M2 verifies the server
	size_t m2_len = 0;
	const uint8_t *m2 = plist_data(&reply, plist_get(&reply, "M2"), &m2_len);
	if (!m2 || m2_len != 32 || memcmp(m2, srp.m2, 32) != 0) {
		snprintf(out->error, sizeof(out->error), "gsa_bad_m2");
		out->result = GSA_FAILED;
		plist_free(&reply);
		return false;
	}

	// the status: a second factor, or signed in
	int status = plist_get(&reply, "Status");
	int au = status >= 0 ? plist_get_in(&reply, status, "au") : -1;
	const char *au_str = au >= 0 ? plist_str(&reply, au) : NULL;

	// the spd: AES-CBC under the extra-data keys
	size_t spd_len = 0;
	const uint8_t *spd = plist_data(&reply, plist_get(&reply, "spd"), &spd_len);
	uint8_t ek[32], eiv[32], *plain = NULL;
	if (spd && spd_len >= 32) {
		hmac_sha256(srp.key, 32, "extra data key:", ek);
		hmac_sha256(srp.key, 32, "extra data iv:", eiv);
		aes256_t aes;
		aes256_init(&aes, ek);
		plain = malloc(spd_len);
		if (plain) {
			aes256_cbc_decrypt(&aes, eiv, spd, spd_len, plain);
		}
	}

	char adsid[128] = "", idms[1024] = "";
	if (plain) {
		plist_t spd_plist;
		memset(&spd_plist, 0, sizeof(spd_plist));
		if (plist_parse((const char *)plain, spd_len, &spd_plist)) {
			int ad = plist_get(&spd_plist, "adsid");
			int gt = plist_get(&spd_plist, "GsIdmsToken");
			if (ad >= 0 && plist_str(&spd_plist, ad)) {
				snprintf(adsid, sizeof(adsid), "%s", plist_str(&spd_plist, ad));
			}
			if (gt >= 0 && plist_str(&spd_plist, gt)) {
				snprintf(idms, sizeof(idms), "%s", plist_str(&spd_plist, gt));
			}
			int pt = plist_get(&spd_plist, "PasswordToken");
			if (pt >= 0 && plist_str(&spd_plist, pt)) {
				snprintf(out->pt_token, sizeof(out->pt_token), "%s", plist_str(&spd_plist, pt));
			}
			// The session's own keys, what the app-token requests sign with.
			size_t kl = 0;
			const uint8_t *sk = plist_data(&spd_plist, plist_get(&spd_plist, "sk"), &kl);
			if (sk && kl && kl <= sizeof(out->sk)) {
				memcpy(out->sk, sk, kl);
				out->sk_len = (int)kl;
			}
			const uint8_t *cv = plist_data(&spd_plist, plist_get(&spd_plist, "c"), &kl);
			if (cv && kl && kl <= sizeof(out->c)) {
				memcpy(out->c, cv, kl);
				out->c_len = (int)kl;
			}
			plist_free(&spd_plist);
		}
		memset(plain, 0, spd_len);
		free(plain);
	}
	snprintf(out->adsid, sizeof(out->adsid), "%s", adsid);
	snprintf(out->idms_token, sizeof(out->idms_token), "%s", idms);

	if (au_str && strcmp(au_str, "trustedDeviceSecondaryAuth") == 0) {
		out->result = GSA_NEEDS_2FA_TOTP;
		plist_free(&reply);
		return true;
	}
	if (au_str && strcmp(au_str, "secondaryAuth") == 0) {
		out->result = GSA_NEEDS_2FA_SMS;
		plist_free(&reply);
		return true;
	}
	out->result = GSA_OK;
	plist_free(&reply);
	return true;
}

bool gsa_2fa_totp(const gsa_anisette_t *anisette, const gsa_login_t *login, const char *code,
				  char *error, size_t error_size) {
	// GET GsService2/validate with the security-code and identity-token
	// headers, plus the anisette set the other GrandSlam calls carry: the
	// validate is what marks the identity as second-factor-passed, and it
	// reads the machine data like everything else.
	char anisette_h[2048] = "";
	for (int i = 0; anisette && i < anisette->count; i++) {
		size_t len = strlen(anisette_h);
		snprintf(anisette_h + len, sizeof(anisette_h) - len, "%s: %s\r\n",
				 anisette->kv[i].key, anisette->kv[i].value);
	}
	// the identity token is the adsid and the GsIdmsToken as one base64 pair
	char ident_pair[1280];
	snprintf(ident_pair, sizeof(ident_pair), "%s:%s", login->adsid, login->idms_token);
	char ident_b64[1792];
	{
		static const char alphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		size_t plen = strlen(ident_pair), o = 0;
		for (size_t i = 0; i + 2 < plen && o + 4 < sizeof(ident_b64); i += 3) {
			uint32_t v = ((uint32_t)(uint8_t)ident_pair[i] << 16) |
						 ((uint32_t)(uint8_t)ident_pair[i + 1] << 8) | (uint8_t)ident_pair[i + 2];
			ident_b64[o++] = alphabet[(v >> 18) & 63];
			ident_b64[o++] = alphabet[(v >> 12) & 63];
			ident_b64[o++] = alphabet[(v >> 6) & 63];
			ident_b64[o++] = alphabet[v & 63];
		}
		if (plen % 3 == 1 && o + 4 < sizeof(ident_b64)) {
			uint32_t v = (uint32_t)(uint8_t)ident_pair[plen - 1] << 16;
			ident_b64[o++] = alphabet[(v >> 18) & 63];
			ident_b64[o++] = alphabet[(v >> 12) & 63];
			ident_b64[o++] = '=';
			ident_b64[o++] = '=';
		} else if (plen % 3 == 2 && o + 4 < sizeof(ident_b64)) {
			uint32_t v = ((uint32_t)(uint8_t)ident_pair[plen - 2] << 16) |
						 ((uint32_t)(uint8_t)ident_pair[plen - 1] << 8);
			ident_b64[o++] = alphabet[(v >> 18) & 63];
			ident_b64[o++] = alphabet[(v >> 12) & 63];
			ident_b64[o++] = alphabet[(v >> 6) & 63];
			ident_b64[o++] = '=';
		}
		ident_b64[o] = '\0';
	}
	char extra[4096];
	snprintf(extra, sizeof(extra),
			 "User-Agent: %s\r\n"
			 "Accept: text/x-xml-plist\r\n"
			 "security-code: %s\r\n"
			 "X-Apple-Identity-Token: %s\r\n"
			 "Accept-Language: en-us\r\n"
			 "Loc: en_US\r\n"
			 "%s",
			 GSA_USER_AGENT, code, ident_b64, anisette_h);
	http_req_t req;
	memset(&req, 0, sizeof(req));
	req.method = "GET";
	req.extra_headers = extra;
	req.want_error_body = true;
	char *out = NULL;
	size_t out_len = 0;
	int status = 0;
	gsa_tls_guard_t tls_guard;
	gsa_tls_begin(&tls_guard);
	bool sent = http_request(GSA_VALIDATE_ENDPOINT, &req, &out, &out_len, 256 * 1024, 30,
							 &status);
	gsa_tls_end(&tls_guard);
	if (!sent) {
		const char *why = http_last_error();
		snprintf(error, error_size, "gsa_2fa_%d%s%s", status, why ? " " : "",
				 why ? why : "");
		return false;
	}
	bool ok = status == 200;
	plist_t reply;
	memset(&reply, 0, sizeof(reply));
	if (ok && plist_parse(out, out_len, &reply)) {
		ok = check_error(&reply, error, error_size);
		plist_free(&reply);
	} else if (!ok) {
		snprintf(error, error_size, "gsa_2fa_%d", status);
	} else {
		snprintf(error, error_size, "gsa_bad_plist");
		ok = false;
	}
	free(out);
	return ok;
}

// ---------------------------------------------------------------------------
// the app tokens: what an Apple service asks for under the session's name
// ---------------------------------------------------------------------------

// HMAC over bytes, the checksum the apptokens request signs with.
static void hmac_bytes(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
					   uint8_t out[32]) {
	uint8_t kip[64], kop[64], inner[32];
	memset(kip, 0, sizeof(kip));
	memcpy(kip, key, key_len > 64 ? 64 : key_len);
	for (int i = 0; i < 64; i++) {
		kop[i] = kip[i] ^ 0x5c;
		kip[i] ^= 0x36;
	}
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, kip, 64);
	sha256_update(&c, msg, msg_len);
	sha256_final_raw(&c, inner);
	sha256_init(&c);
	sha256_update(&c, kop, 64);
	sha256_update(&c, inner, 32);
	sha256_final_raw(&c, out);
	memset(kip, 0, sizeof(kip));
	memset(kop, 0, sizeof(kop));
}

bool gsa_app_token(const gsa_anisette_t *anisette, const gsa_login_t *login,
				   const char *app_name, char *out, size_t out_size, char *error,
				   size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (out && out_size) {
		out[0] = '\0';
	}
	if (!anisette || !login || !app_name || !app_name[0] || !out || !out_size) {
		snprintf(error, error_size, "gsa_bad_args");
		return false;
	}
	if (!login->adsid[0] || !login->idms_token[0] || login->sk_len <= 0 || login->c_len <= 0) {
		snprintf(error, error_size, "gsa_no_session");
		return false;
	}
	// checksum = HMAC-SHA256(sk, "apptokens" | adsid | app_name)
	size_t msg_len = strlen("apptokens") + strlen(login->adsid) + strlen(app_name);
	uint8_t *msg = malloc(msg_len);
	if (!msg) {
		snprintf(error, error_size, "gsa_memory");
		return false;
	}
	size_t off = 0;
	memcpy(msg + off, "apptokens", 9);
	off += 9;
	memcpy(msg + off, login->adsid, strlen(login->adsid));
	off += strlen(login->adsid);
	memcpy(msg + off, app_name, strlen(app_name));
	uint8_t checksum[32];
	hmac_bytes(login->sk, (size_t)login->sk_len, msg, msg_len, checksum);
	memset(msg, 0, msg_len);
	free(msg);

	plistw_t w;
	plistw_init(&w);
	plistw_dict_begin(&w, NULL);
	plistw_dict_begin(&w, "Header");
	plistw_string(&w, "Version", "1.0.1");
	plistw_dict_end(&w);
	plistw_dict_begin(&w, "Request");
	anisette_to_plist(&w, anisette);
	plistw_array_begin(&w, "app");
	plistw_string(&w, NULL, app_name);
	plistw_array_end(&w);
	plistw_data(&w, "c", login->c, (size_t)login->c_len);
	plistw_string(&w, "operation", "apptokens");
	plistw_string(&w, "t", login->idms_token);
	plistw_string(&w, "u", login->adsid);
	plistw_data(&w, "checksum", checksum, sizeof(checksum));
	plistw_dict_end(&w);
	plistw_dict_end(&w);

	plist_t reply;
	memset(&reply, 0, sizeof(reply));
	if (!gsa_post(GSA_ENDPOINT, w.buf, &reply, error, error_size)) {
		plistw_free(&w);
		return false;
	}
	plistw_free(&w);

	// The answer carries the tokens under the app's own name; the shape is
	// nested enough that anything unexpected fails honestly rather than
	// handing back a half-read token.
	int app_node = plist_get_in(&reply, plist_get(&reply, "appTokens"), app_name);
	if (app_node < 0) {
		// some answers carry the token directly under the app's name
		app_node = plist_get(&reply, app_name);
	}
	bool ok = false;
	if (app_node >= 0) {
		int tok = plist_get_in(&reply, app_node, "token");
		if (tok < 0) {
			tok = plist_get_in(&reply, app_node, "PT");
		}
		if (tok >= 0 && plist_str(&reply, tok)) {
			snprintf(out, out_size, "%s", plist_str(&reply, tok));
			ok = out[0] != '\0';
		}
	}
	if (!ok && !error[0]) {
		snprintf(error, error_size, "gsa_app_token_missing");
	}
	plist_free(&reply);
	return ok;
}
