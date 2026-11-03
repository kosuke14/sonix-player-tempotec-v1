#include "pbkdf2.h"

#include "srp.h" // only for the salt upper bound; the salt lives with the caller
#include "src/system/core/sha256.h"

#include <string.h>

static void hmac_sha256(const uint8_t *key, size_t key_len, const void *msg, size_t msg_len,
						uint8_t out[32]) {
	uint8_t k[64], pad[64];
	memset(k, 0, sizeof(k));
	if (key_len > 64) {
		sha256_ctx c;
		sha256_init(&c);
		sha256_update(&c, key, key_len);
		sha256_final_raw(&c, k);
	} else {
		memcpy(k, key, key_len);
	}
	for (int i = 0; i < 64; i++) {
		pad[i] = k[i] ^ 0x5c;
		k[i] ^= 0x36;
	}
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, k, 64);
	sha256_update(&c, msg, msg_len);
	uint8_t inner[32];
	sha256_final_raw(&c, inner);
	sha256_init(&c);
	sha256_update(&c, pad, 64);
	sha256_update(&c, inner, 32);
	sha256_final_raw(&c, out);
	memset(k, 0, sizeof(k));
	memset(pad, 0, sizeof(pad));
}

void pbkdf2_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *salt, size_t salt_len,
						unsigned iterations, uint8_t *out, size_t out_len) {
	uint8_t block[4]; // big-endian block index, 1-based
	uint8_t msg[SRP_SALT_MAX + 4];
	uint32_t blocks = (uint32_t)((out_len + 31) / 32);
	for (uint32_t b = 1; b <= blocks; b++) {
		block[0] = (uint8_t)(b >> 24);
		block[1] = (uint8_t)(b >> 16);
		block[2] = (uint8_t)(b >> 8);
		block[3] = (uint8_t)b;
		size_t sl = salt_len > SRP_SALT_MAX ? SRP_SALT_MAX : salt_len;
		memcpy(msg, salt, sl);
		memcpy(msg + sl, block, 4);
		uint8_t u[32], acc[32];
		hmac_sha256(key, key_len, msg, sl + 4, u);
		memcpy(acc, u, 32);
		for (unsigned i = 1; i < iterations; i++) {
			hmac_sha256(key, key_len, u, 32, u);
			for (int j = 0; j < 32; j++) {
				acc[j] ^= u[j];
			}
		}
		size_t off = (size_t)(b - 1) * 32;
		size_t take = out_len - off > 32 ? 32 : out_len - off;
		memcpy(out + off, acc, take);
		memset(u, 0, sizeof(u));
		memset(acc, 0, sizeof(acc));
	}
}
