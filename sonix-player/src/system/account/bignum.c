#include "bignum.h"

#include <string.h>

void bn_zero(bn_t *a) {
	a->n = 0;
	memset(a->w, 0, sizeof(a->w));
}

void bn_copy(bn_t *out, const bn_t *a) {
	*out = *a;
}

void bn_from_u32(bn_t *a, uint32_t v) {
	a->n = v ? 1 : 0;
	a->w[0] = v;
	memset(a->w + 1, 0, sizeof(a->w) - sizeof(uint32_t));
}

// Big-endian bytes in, little-endian words out: word wi covers the input
// bytes [len - 4*(wi+1), len - 4*wi).
void bn_from_bytes_be(bn_t *a, const uint8_t *bytes, size_t len) {
	bn_zero(a);
	size_t words = (len + 3) / 4;
	if (words > BN_WORDS) {
		words = BN_WORDS;
	}
	for (size_t wi = 0; wi < words; wi++) {
		size_t hi = len - wi * 4;
		size_t lo = hi > 4 ? hi - 4 : 0;
		uint32_t v = 0;
		for (size_t j = lo; j < hi; j++) {
			v = (v << 8) | bytes[j];
		}
		a->w[wi] = v;
	}
	a->n = 0;
	for (int i = (int)words - 1; i >= 0; i--) {
		if (a->w[i]) {
			a->n = i + 1;
			break;
		}
	}
}

void bn_to_bytes_be(const bn_t *a, uint8_t *out, size_t len) {
	memset(out, 0, len);
	for (int i = 0; i < a->n; i++) {
		size_t hi = len - (size_t)i * 4;
		size_t lo = hi > 4 ? hi - 4 : 0;
		for (size_t j = lo; j < hi; j++) {
			unsigned shift = 8 * (unsigned)(hi - 1 - j);
			out[j] = (uint8_t)(a->w[i] >> shift);
		}
	}
}

bool bn_is_zero(const bn_t *a) {
	return a->n == 0;
}

int bn_cmp(const bn_t *a, const bn_t *b) {
	if (a->n != b->n) {
		return a->n > b->n ? 1 : -1;
	}
	for (int i = a->n - 1; i >= 0; i--) {
		if (a->w[i] != b->w[i]) {
			return a->w[i] > b->w[i] ? 1 : -1;
		}
	}
	return 0;
}

void bn_add(bn_t *out, const bn_t *a, const bn_t *b) {
	int n = a->n > b->n ? a->n : b->n;
	uint64_t carry = 0;
	int i = 0;
	for (; i < n || carry; i++) {
		if (i >= BN_WORDS) {
			break;
		}
		uint64_t v = carry;
		if (i < a->n) {
			v += a->w[i];
		}
		if (i < b->n) {
			v += b->w[i];
		}
		out->w[i] = (uint32_t)v;
		carry = v >> 32;
	}
	out->n = i;
	memset(out->w + out->n, 0, sizeof(out->w) - (size_t)out->n * sizeof(uint32_t));
}

void bn_sub(bn_t *out, const bn_t *a, const bn_t *b) {
	int64_t borrow = 0;
	out->n = a->n;
	for (int i = 0; i < a->n; i++) {
		int64_t v = (int64_t)a->w[i] - borrow - (i < b->n ? (int64_t)b->w[i] : 0);
		if (v < 0) {
			v += ((int64_t)1 << 32);
			borrow = 1;
		} else {
			borrow = 0;
		}
		out->w[i] = (uint32_t)v;
	}
	while (out->n > 0 && out->w[out->n - 1] == 0) {
		out->n--;
	}
	memset(out->w + out->n, 0, sizeof(out->w) - (size_t)out->n * sizeof(uint32_t));
}

// Plain bit-by-bit reduction, top bit down: obviously correct, and only on
// the Montgomery preprocessing path (a handful of calls per init).
void bn_mod(bn_t *out, const bn_t *a, const bn_t *n) {
	bn_t cur;
	bn_zero(&cur);
	for (int bit = a->n * 32 - 1; bit >= 0; bit--) {
		bn_add(&cur, &cur, &cur); // double
		uint32_t b = (a->w[bit / 32] >> (bit % 32)) & 1;
		if (b) {
			if (cur.n == 0) {
				cur.n = 1;
			}
			cur.w[0] |= 1;
		}
		if (bn_cmp(&cur, n) >= 0) {
			bn_sub(&cur, &cur, n);
		}
	}
	*out = cur;
}

void bnmont_init(bnmont_t *m, const bn_t *n) {
	m->n = *n;
	m->words = n->n;
	// n0inv = -n^-1 mod 2^32, Newton iteration on the low word
	uint32_t inv = 1;
	for (int i = 0; i < 31; i++) {
		inv *= inv;
		inv *= n->w[0];
	}
	m->n0inv = 0u - inv;
	// R mod n, R = 2^(32*words)
	bn_t r;
	bn_zero(&r);
	r.w[m->words] = 1;
	r.n = m->words + 1;
	bn_mod(&r, &r, n);
	m->r = r;
	// R^2 mod n
	bn_t r2;
	bn_zero(&r2);
	int bits2 = 2 * m->words * 32;
	r2.w[bits2 / 32] = 1u << (bits2 % 32);
	r2.n = bits2 / 32 + 1;
	bn_mod(&r2, &r2, n);
	m->r2 = r2;
}

// One REDC step: out = a*b/R mod n. a and b must be reduced (< n).
static void mont_mul_raw(bn_t *out, const bn_t *a, const bn_t *b, const bnmont_t *m) {
	uint32_t t[BN_WORDS + 2];
	memset(t, 0, sizeof(t));
	int mw = m->words;
	for (int i = 0; i < mw; i++) {
		uint64_t carry = 0;
		uint64_t uv = 0;
		for (int j = 0; j < mw; j++) {
			uv = (uint64_t)t[j] + (uint64_t)a->w[i] * b->w[j] + carry;
			t[j] = (uint32_t)uv;
			carry = uv >> 32;
		}
		uv = (uint64_t)t[mw] + carry;
		t[mw] = (uint32_t)uv;
		t[mw + 1] = (uint32_t)(uv >> 32);
		// reduce one word: t += k*n, k = t[0]*n0inv mod 2^32, then shift right
		uint32_t k = t[0] * m->n0inv;
		uv = (uint64_t)t[0] + (uint64_t)k * m->n.w[0];
		carry = uv >> 32;
		for (int j = 1; j < mw; j++) {
			uv = (uint64_t)t[j] + (uint64_t)k * m->n.w[j] + carry;
			t[j - 1] = (uint32_t)uv;
			carry = uv >> 32;
		}
		uv = (uint64_t)t[mw] + carry;
		t[mw - 1] = (uint32_t)uv;
		t[mw] = t[mw + 1] + (uint32_t)(uv >> 32);
		t[mw + 1] = 0;
	}
	// The value after the sweep spans mw+1 words (it can reach 2n-1 when the
	// product crossed the modulus): the high word is real and dropping it
	// loses exactly one unit -- visible as a flipped LSB far into the stream.
	bn_t vnew;
	memset(&vnew, 0, sizeof(vnew));
	memcpy(vnew.w, t, (size_t)(mw + 1) * sizeof(uint32_t));
	vnew.n = mw + 1;
	while (vnew.n > 0 && vnew.w[vnew.n - 1] == 0) {
		vnew.n--;
	}
	if (bn_cmp(&vnew, &m->n) >= 0) {
		bn_sub(&vnew, &vnew, &m->n);
	}
	*out = vnew;
}

void bn_mont_mul(bn_t *out, const bn_t *a, const bn_t *b, const bnmont_t *m) {
	mont_mul_raw(out, a, b, m);
}

void bn_to_mont(bn_t *out, const bn_t *a, const bnmont_t *m) {
	bn_mont_mul(out, a, &m->r2, m);
}

void bn_from_mont(bn_t *out, const bn_t *a, const bnmont_t *m) {
	bn_t one;
	bn_from_u32(&one, 1);
	bn_mont_mul(out, a, &one, m);
}

void bn_mont_exp(bn_t *out, const bn_t *base_mont, const bn_t *exp, const bnmont_t *m) {
	bn_t result, base;
	bn_copy(&base, base_mont);
	bn_copy(&result, &m->r); // one, in Montgomery space
	for (int bit = 0; bit < exp->n * 32; bit++) {
		if ((exp->w[bit / 32] >> (bit % 32)) & 1) {
			bn_mont_mul(&result, &result, &base, m);
		}
		bn_mont_mul(&base, &base, &base, m);
	}
	*out = result;
}
