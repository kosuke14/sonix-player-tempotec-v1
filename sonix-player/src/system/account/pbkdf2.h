#ifndef PBKDF2_H
#define PBKDF2_H

#include <stddef.h>
#include <stdint.h>

// PBKDF2-HMAC-SHA256 (RFC 2898). The GrandSlam sign-in derives its verifier
// password through this: SHA-256(apple_id_password) as the key, the server's
// salt, the server's iteration count.
void pbkdf2_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *salt, size_t salt_len,
						unsigned iterations, uint8_t *out, size_t out_len);

#endif /* PBKDF2_H */
