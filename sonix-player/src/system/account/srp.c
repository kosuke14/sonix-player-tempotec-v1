#include "srp.h"

#include "bignum.h"
#include "src/system/core/sha256.h"

#include <string.h>
#include <stdio.h>

// The RFC 5054 2048-bit group, N, and g = 2 -- the exact bytes the reference
// implementations carry.
static const uint8_t SRP_N[256] = {
	0xac, 0x6b, 0xdb, 0x41, 0x32, 0x4a, 0x9a, 0x9b, 0xf1, 0x66, 0xde, 0x5e, 0x13, 0x89, 0x58, 0x2f,
	0xaf, 0x72, 0xb6, 0x65, 0x19, 0x87, 0xee, 0x07, 0xfc, 0x31, 0x92, 0x94, 0x3d, 0xb5, 0x60, 0x50,
	0xa3, 0x73, 0x29, 0xcb, 0xb4, 0xa0, 0x99, 0xed, 0x81, 0x93, 0xe0, 0x75, 0x77, 0x67, 0xa1, 0x3d,
	0xd5, 0x23, 0x12, 0xab, 0x4b, 0x03, 0x31, 0x0d, 0xcd, 0x7f, 0x48, 0xa9, 0xda, 0x04, 0xfd, 0x50,
	0xe8, 0x08, 0x39, 0x69, 0xed, 0xb7, 0x67, 0xb0, 0xcf, 0x60, 0x95, 0x17, 0x9a, 0x16, 0x3a, 0xb3,
	0x66, 0x1a, 0x05, 0xfb, 0xd5, 0xfa, 0xaa, 0xe8, 0x29, 0x18, 0xa9, 0x96, 0x2f, 0x0b, 0x93, 0xb8,
	0x55, 0xf9, 0x79, 0x93, 0xec, 0x97, 0x5e, 0xea, 0xa8, 0x0d, 0x74, 0x0a, 0xdb, 0xf4, 0xff, 0x74,
	0x73, 0x59, 0xd0, 0x41, 0xd5, 0xc3, 0x3e, 0xa7, 0x1d, 0x28, 0x1e, 0x44, 0x6b, 0x14, 0x77, 0x3b,
	0xca, 0x97, 0xb4, 0x3a, 0x23, 0xfb, 0x80, 0x16, 0x76, 0xbd, 0x20, 0x7a, 0x43, 0x6c, 0x64, 0x81,
	0xf1, 0xd2, 0xb9, 0x07, 0x87, 0x17, 0x46, 0x1a, 0x5b, 0x9d, 0x32, 0xe6, 0x88, 0xf8, 0x77, 0x48,
	0x54, 0x45, 0x23, 0xb5, 0x24, 0xb0, 0xd5, 0x7d, 0x5e, 0xa7, 0x7a, 0x27, 0x75, 0xd2, 0xec, 0xfa,
	0x03, 0x2c, 0xfb, 0xdb, 0xf5, 0x2f, 0xb3, 0x78, 0x61, 0x60, 0x27, 0x90, 0x04, 0xe5, 0x7a, 0xe6,
	0xaf, 0x87, 0x4e, 0x73, 0x03, 0xce, 0x53, 0x29, 0x9c, 0xcc, 0x04, 0x1c, 0x7b, 0xc3, 0x08, 0xd8,
	0x2a, 0x56, 0x98, 0xf3, 0xa8, 0xd0, 0xc3, 0x82, 0x71, 0xae, 0x35, 0xf8, 0xe9, 0xdb, 0xfb, 0xb6,
	0x94, 0xb5, 0xc8, 0x03, 0xd8, 0x9f, 0x7a, 0xe4, 0x35, 0xde, 0x23, 0x6d, 0x52, 0x5f, 0x54, 0x75,
	0x9b, 0x65, 0xe3, 0x72, 0xfc, 0xd6, 0x8e, 0xf2, 0x0f, 0xa7, 0x11, 0x1f, 0x9e, 0x4a, 0xff, 0x73,
};

static void sha256_2(const void *a, size_t alen, const void *b, size_t blen, uint8_t out[32]) {
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, a, alen);
	sha256_update(&c, b, blen);
	sha256_final_raw(&c, out);
}

// x = H(salt | H(":" | password)) -- the username is empty in this flow, so
// the identity hash is the hash of the leading colon and the PBKDF2 output.
static void compute_x(const uint8_t *password, size_t password_len, const uint8_t *salt,
					  size_t salt_len, bn_t *x) {
	uint8_t colon = ':';
	uint8_t ih[32], xx[32];
	sha256_2(&colon, 1, password, password_len, ih);
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, salt, salt_len);
	sha256_update(&c, ih, 32);
	sha256_final_raw(&c, xx);
	bn_from_bytes_be(x, xx, 32);
}

// u = H(A | B), k = H(N | PAD(g))
static void compute_u(const uint8_t *a_pub, const uint8_t *b_pub, bn_t *u) {
	uint8_t h[32];
	sha256_2(a_pub, 256, b_pub, 256, h);
	bn_from_bytes_be(u, h, 32);
}

static void compute_k(bn_t *k) {
	uint8_t g_pad[256];
	memset(g_pad, 0, sizeof(g_pad));
	g_pad[255] = 2;
	sha256_2(SRP_N, 256, g_pad, 256, g_pad); // reuse the buffer for the digest
	bn_from_bytes_be(k, g_pad, 32);
}

// M1 = H(H(g_pad) XOR H(N) | H(username) | salt | A | B | K) -- the proof
// binds the account name, even though the key derivation does not.
static void compute_m1(const uint8_t *a_pub, const uint8_t *b_pub, const uint8_t *key,
					   const char *username, const uint8_t *salt, size_t salt_len,
					   uint8_t m1[32]) {
	uint8_t g_pad[256], gh[32], nh[32];
	memset(g_pad, 0, sizeof(g_pad));
	g_pad[255] = 2;
	sha256_2(g_pad, 256, NULL, 0, gh);
	sha256_2(SRP_N, 256, NULL, 0, nh);
	sha256_ctx c;
	sha256_init(&c);
	for (int i = 0; i < 32; i++) {
		uint8_t b = gh[i] ^ nh[i];
		sha256_update(&c, &b, 1);
	}
	uint8_t user_hash[32];
	{
		sha256_ctx e;
		sha256_init(&e);
		sha256_update(&e, username, strlen(username));
		sha256_final_raw(&e, user_hash);
	}
	sha256_update(&c, user_hash, 32);
	sha256_update(&c, salt, salt_len);
	sha256_update(&c, a_pub, 256);
	sha256_update(&c, b_pub, 256);
	sha256_update(&c, key, 32);
	sha256_final_raw(&c, m1);
}

static void compute_m2(const uint8_t *a_pub, const uint8_t m1[32], const uint8_t *key,
					   uint8_t m2[32]) {
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, a_pub, 256);
	sha256_update(&c, m1, 32);
	sha256_update(&c, key, 32);
	sha256_final_raw(&c, m2);
}

void srp_public_a(const uint8_t *a, size_t a_len, uint8_t out[256]) {
	bn_t N;
	bn_from_bytes_be(&N, SRP_N, 256);
	bnmont_t mont;
	bnmont_init(&mont, &N);
	bn_t av, g, r;
	bn_from_bytes_be(&av, a, a_len);
	bn_from_u32(&g, 2);
	bn_to_mont(&g, &g, &mont);
	bn_mont_exp(&r, &g, &av, &mont);
	bn_from_mont(&r, &r, &mont);
	memset(out, 0, 256);
	bn_to_bytes_be(&r, out, 256);
}

bool srp_process_reply(const uint8_t *a, size_t a_len, const char *username,
					   const uint8_t *password, size_t password_len, const uint8_t *salt,
					   size_t salt_len, const uint8_t *b_pub, size_t b_pub_len,
					   srp_session_t *out) {
	bn_t N;
	bn_from_bytes_be(&N, SRP_N, 256);
	bnmont_t mont;
	bnmont_init(&mont, &N);

	bn_t av, bv;
	bn_from_bytes_be(&av, a, a_len);
	bn_from_bytes_be(&bv, b_pub, b_pub_len);
	if (bn_is_zero(&bv)) {
		return false; // a malicious B
	}

	// u and k
	uint8_t a_pub[256];
	memset(a_pub, 0, 256);
	bn_t g, r;
	bn_from_u32(&g, 2);
	bn_to_mont(&g, &g, &mont);
	bn_mont_exp(&r, &g, &av, &mont);
	bn_from_mont(&r, &r, &mont);
	bn_to_bytes_be(&r, a_pub, 256);
	bn_t u, k;
	compute_u(a_pub, b_pub, &u);
	compute_k(&k);
#ifdef SRP_DEBUG
	{
		uint8_t db[256];
		memset(db, 0, sizeof(db));
		bn_to_bytes_be(&av, db, 256);
		fprintf(stderr, "dbg a   : ");
		for (int i = 248; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
		bn_to_bytes_be(&u, db, 256);
		fprintf(stderr, "dbg u   : ");
		for (int i = 248; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
		bn_to_bytes_be(&k, db, 256);
		fprintf(stderr, "dbg k   : ");
		for (int i = 248; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
	}
#endif

	// x = H(salt | H(":" | password))
	bn_t x;
	compute_x(password, password_len, salt, salt_len, &x);

	// base = (B - k * g^x) mod N. The group generator is already in
	// Montgomery space here (the A computation converted it in place).
	bn_t gx;
	bn_mont_exp(&gx, &g, &x, &mont);
	bn_from_mont(&gx, &gx, &mont); // g^x, plain
	bn_t kgx;
	bn_to_mont(&k, &k, &mont);
	bn_to_mont(&gx, &gx, &mont);
	bn_mont_mul(&kgx, &k, &gx, &mont);
	bn_from_mont(&kgx, &kgx, &mont); // k * g^x mod N
	bn_t base;
	bn_add(&base, &N, &bv);          // B + N (so the subtraction never underflows)
	bn_sub(&base, &base, &kgx);
	bn_mod(&base, &base, &N);

	// exp = a + u * x
	bn_t ux, exp;
#ifdef SRP_DEBUG
	{
		uint8_t db[256];
		memset(db, 0, sizeof(db));
		bn_to_bytes_be(&u, db, 256);
		fprintf(stderr, "dbg u   : ");
		for (int i = 240; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
		bn_to_bytes_be(&av, db, 256);
		fprintf(stderr, "dbg a   : ");
		for (int i = 240; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
		fprintf(stderr, "dbg A   : ");
		for (int i = 240; i < 256; i++) fprintf(stderr, "%02x", a_pub[i]);
		fprintf(stderr, "\n");
		fprintf(stderr, "dbg B   : ");
		for (int i = 240; i < 256; i++) fprintf(stderr, "%02x", b_pub[i]);
		fprintf(stderr, "\n");
		fprintf(stderr, "dbg Blen: %d\n", (int)b_pub_len);
		bn_to_bytes_be(&x, db, 256);
		fprintf(stderr, "dbg x   : ");
		for (int i = 240; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
	}
#endif
	bn_to_mont(&u, &u, &mont);
	bn_to_mont(&x, &x, &mont);
	bn_mont_mul(&ux, &u, &x, &mont);
	bn_from_mont(&ux, &ux, &mont);
#ifdef SRP_DEBUG
	{
		uint8_t db[512];
		memset(db, 0, sizeof(db));
		bn_to_bytes_be(&ux, db, 512); // ux is plain (post from_mont)
		fprintf(stderr, "dbg ux  : ");
		for (int i = 496; i < 512; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
	}
#endif
	bn_add(&exp, &ux, &av);

#ifdef SRP_DEBUG
	{
		uint8_t db[256];
		memset(db, 0, sizeof(db));
		bn_to_bytes_be(&gx, db, 256);
		fprintf(stderr, "dbg gx : ");
		for (int i = 248; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
		bn_to_bytes_be(&kgx, db, 256);
		fprintf(stderr, "dbg kgx: ");
		for (int i = 248; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
		bn_to_bytes_be(&base, db, 256);
		fprintf(stderr, "dbg base: ");
		for (int i = 248; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
		bn_to_bytes_be(&exp, db, 256);
		fprintf(stderr, "dbg exp: ");
		for (int i = 248; i < 256; i++) fprintf(stderr, "%02x", db[i]);
		fprintf(stderr, "\n");
	}
#endif
	// S = base^exp mod N
	bn_to_mont(&base, &base, &mont);
	bn_mont_exp(&r, &base, &exp, &mont);
	bn_from_mont(&r, &r, &mont);
	uint8_t sb[256];
	memset(sb, 0, sizeof(sb));
	bn_to_bytes_be(&r, sb, 256);
	// the reference hashes the MINIMAL big-endian encoding of S
	size_t first = 0;
	while (first < 255 && sb[first] == 0) {
		first++;
	}

	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, sb + first, 256 - first);
	uint8_t key[32];
	sha256_final_raw(&c, key);
	memcpy(out->key, key, 32);

	compute_m1(a_pub, b_pub, key, username, salt, salt_len, out->m1);
	compute_m2(a_pub, out->m1, key, out->m2);
	return true;
}
