#ifndef SRP_H
#define SRP_H

// The client half of Apple's GrandSlam sign-in: SRP-6a over the RFC 5054
// 2048-bit group with SHA-256, exactly as the 3.x/6.x daemons' reference
// implementations do it. The password arrives already hashed -- the caller
// runs PBKDF2-HMAC-SHA256 over SHA-256(password) first (see pbkdf2.h).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SRP_SALT_MAX 64
#define SRP_KEY_LEN 32 // the shared secret, SHA-256 sized

typedef struct {
	uint8_t m1[32];
	uint8_t m2[32];
	uint8_t key[SRP_KEY_LEN]; // session key: the spd decode and proof both use it
} srp_session_t;

// A = g^a mod N. `a` is the caller's secret (32 random bytes).
void srp_public_a(const uint8_t *a, size_t a_len, uint8_t out[256]);

// Processes the server's reply (salt, B), deriving the session key and the
// proofs. `password` is the PBKDF2 output, not the raw password. False on a
// malicious B (B ≡ 0 mod N).
bool srp_process_reply(const uint8_t *a, size_t a_len, const uint8_t *password, size_t password_len,
					   const uint8_t *salt, size_t salt_len, const uint8_t *b_pub,
					   size_t b_pub_len, srp_session_t *out);

#endif /* SRP_H */
