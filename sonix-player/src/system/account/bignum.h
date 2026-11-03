#ifndef BIGNUM_H
#define BIGNUM_H

// A small fixed-width bignum for one job: SRP-6a's 3072-bit modular
// exponentiation on the device. Little-endian 32-bit words, no dynamic
// memory, no signing -- add, sub, compare, wide multiply and a Montgomery
// modexp against an odd modulus.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BN_WORDS 200 // 6400 bits: SRP's 3072-bit N needs R^2 (6144) inside the type

typedef struct {
	int n; // words in use, 0 = zero
	uint32_t w[BN_WORDS];
} bn_t;

typedef struct {
	bn_t n;    // the odd modulus
	bn_t r;    // R mod n (the Montgomery one form)
	bn_t r2;   // R^2 mod n (to enter Montgomery space)
	uint32_t n0inv; // -n^-1 mod 2^32
	int words; // n's word count
} bnmont_t;

void bn_zero(bn_t *a);
void bn_from_u32(bn_t *a, uint32_t v);
void bn_from_bytes_be(bn_t *a, const uint8_t *bytes, size_t len);
void bn_to_bytes_be(const bn_t *a, uint8_t *out, size_t len);
bool bn_is_zero(const bn_t *a);
int bn_cmp(const bn_t *a, const bn_t *b);
void bn_copy(bn_t *out, const bn_t *a);
void bn_add(bn_t *out, const bn_t *a, const bn_t *b);
void bn_sub(bn_t *out, const bn_t *a, const bn_t *b); // requires a >= b
void bn_mod(bn_t *out, const bn_t *a, const bn_t *n); // plain division, slow

void bnmont_init(bnmont_t *m, const bn_t *n); // n must be odd
void bn_to_mont(bn_t *out, const bn_t *a, const bnmont_t *m);
void bn_from_mont(bn_t *out, const bn_t *a, const bnmont_t *m);
void bn_mont_mul(bn_t *out, const bn_t *a, const bn_t *b, const bnmont_t *m);
void bn_mont_exp(bn_t *out, const bn_t *base_mont, const bn_t *exp, const bnmont_t *m);

#endif /* BIGNUM_H */
