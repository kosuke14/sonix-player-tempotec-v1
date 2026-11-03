#include "plistxml.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------------
// base64, for the <data> nodes
// ---------------------------------------------------------------------------

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t len, char *out) {
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < len) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (i + 2 < len) {
			v |= in[i + 2];
		}
		out[o++] = B64[(v >> 18) & 63];
		out[o++] = B64[(v >> 12) & 63];
		out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < len ? B64[v & 63] : '=';
	}
	out[o] = '\0';
	return o;
}

static int b64_val(char c) {
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '+') {
		return 62;
	}
	if (c == '/') {
		return 63;
	}
	return -1;
}

static size_t b64_decode(const char *in, size_t len, uint8_t *out, size_t out_max) {
	size_t o = 0;
	uint32_t acc = 0;
	int bits = 0;
	for (size_t i = 0; i < len && o < out_max; i++) {
		if (isspace((unsigned char)in[i])) {
			continue;
		}
		if (in[i] == '=') {
			break;
		}
		int v = b64_val(in[i]);
		if (v < 0) {
			return 0;
		}
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (uint8_t)(acc >> bits);
		}
	}
	return o;
}

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------

void plistw_init(plistw_t *w) {
	w->cap = 4096;
	w->buf = malloc(w->cap);
	w->len = 0;
	w->ok = w->buf != NULL;
	if (w->ok) {
		w->buf[0] = '\0';
	}
}

void plistw_free(plistw_t *w) {
	free(w->buf);
	w->buf = NULL;
	w->len = w->cap = 0;
}

static void wraw(plistw_t *w, const char *s) {
	if (!w->ok) {
		return;
	}
	size_t l = strlen(s);
	if (w->len + l + 1 > w->cap) {
		size_t cap = w->cap * 2 + l + 1;
		char *nb = realloc(w->buf, cap);
		if (!nb) {
			w->ok = 0;
			return;
		}
		w->buf = nb;
		w->cap = cap;
	}
	memcpy(w->buf + w->len, s, l + 1);
	w->len += l;
}

static void wesc(plistw_t *w, const char *s) {
	// XML-escape the specials the payloads can carry
	char tmp[1024];
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p && o + 8 < sizeof(tmp); p++) {
		if (*p == '&') {
			tmp[o++] = '&';
			tmp[o++] = 'a';
			tmp[o++] = 'm';
			tmp[o++] = 'p';
			tmp[o++] = ';';
		} else if (*p == '<') {
			tmp[o++] = '&';
			tmp[o++] = 'l';
			tmp[o++] = 't';
			tmp[o++] = ';';
		} else if (*p == '>') {
			tmp[o++] = '&';
			tmp[o++] = 'g';
			tmp[o++] = 't';
			tmp[o++] = ';';
		} else {
			tmp[o++] = (char)*p;
		}
	}
	tmp[o] = '\0';
	wraw(w, tmp);
}

void plistw_dict_begin(plistw_t *w, const char *key) {
	if (key) {
		wraw(w, "<key>");
		wesc(w, key);
		wraw(w, "</key>");
	}
	wraw(w, "<dict>");
}

void plistw_dict_end(plistw_t *w) {
	wraw(w, "</dict>");
}

void plistw_array_begin(plistw_t *w, const char *key) {
	if (key) {
		wraw(w, "<key>");
		wesc(w, key);
		wraw(w, "</key>");
	}
	wraw(w, "<array>");
}

void plistw_array_end(plistw_t *w) {
	wraw(w, "</array>");
}

void plistw_string(plistw_t *w, const char *key, const char *value) {
	if (key) {
		wraw(w, "<key>");
		wesc(w, key);
		wraw(w, "</key>");
	}
	wraw(w, "<string>");
	wesc(w, value);
	wraw(w, "</string>");
}


void plistw_int(plistw_t *w, const char *key, long long value) {
	char num[32];
	snprintf(num, sizeof(num), "%lld", value);
	if (key) {
		wraw(w, "<key>");
		wesc(w, key);
		wraw(w, "</key>");
	}
	wraw(w, "<integer>");
	wraw(w, num);
	wraw(w, "</integer>");
}


void plistw_data(plistw_t *w, const char *key, const uint8_t *value, size_t len) {
	if (key) {
		wraw(w, "<key>");
		wesc(w, key);
		wraw(w, "</key>");
	}
	wraw(w, "<data>");
	char *b64 = malloc(len * 4 / 3 + 8);
	if (b64) {
		b64_encode(value, len, b64);
		wraw(w, b64);
		free(b64);
	}
	wraw(w, "</data>");
}


// ---------------------------------------------------------------------------
// reader: a small recursive-descent walk over the tags we support
// ---------------------------------------------------------------------------

static void skip_ws(const char **p, const char *end) {
	while (*p < end && isspace((unsigned char)**p)) {
		(*p)++;
	}
}

// Advances past "</name>", returns false when the tag doesn't match.
static bool expect_close(const char **p, const char *end, const char *name) {
	size_t l = strlen(name);
	if ((size_t)(end - *p) < l + 3 || (*p)[0] != '<' || (*p)[1] != '/') {
		return false;
	}
	if (strncmp(*p + 2, name, l) != 0 || (*p)[2 + l] != '>') {
		return false;
	}
	*p += l + 3;
	return true;
}

static int add_node(plist_t *pl, int parent, const char *key, plist_kind_t kind) {
	if (pl->count >= PLIST_MAX_NODES) {
		return -1;
	}
	plist_node_t *n = &pl->nodes[pl->count];
	memset(n, 0, sizeof(*n));
	n->parent = parent;
	n->key = key ? strdup(key) : NULL;
	n->kind = kind;
	return pl->count++;
}

static void xml_unescape(char *s) {
	// in-place; the escapes GrandSlam uses are short
	char *o = s;
	while (*s) {
		if (s[0] == '&') {
			if (strncmp(s, "&", 5) == 0) {
				*o++ = '&';
				s += 5;
				continue;
			}
			if (strncmp(s, "<", 4) == 0) {
				*o++ = '<';
				s += 4;
				continue;
			}
			if (strncmp(s, ">", 4) == 0) {
				*o++ = '>';
				s += 4;
				continue;
			}
		}
		*o++ = *s++;
	}
	*o = '\0';
}

// Parses the value starting at *p (after <dict>, <string> ...). Returns the
// node index or -1.
static int parse_value(plist_t *pl, const char **p, const char *end, int parent, const char *key) {
	skip_ws(p, end);
	if (*p >= end || **p != '<') {
		return -1;
	}
	const char *tag = *p + 1;
	if (strncmp(tag, "dict>", 5) == 0) {
		*p += 6;
		int node = add_node(pl, parent, key, PLIST_DICT);
		if (node < 0) {
			return -1;
		}
		for (;;) {
			skip_ws(p, end);
			if (*p + 7 <= end && strncmp(*p, "</dict>", 7) == 0) {
				*p += 7;
				return node;
			}
			if (*p >= end || **p != '<' || strncmp(*p, "<key>", 5) != 0) {
				return -1;
			}
			*p += 5;
			const char *ke = memchr(*p, '<', (size_t)(end - *p));
			if (!ke) {
				return -1;
			}
			char kbuf[256];
			size_t kl = (size_t)(ke - *p) < sizeof(kbuf) - 1 ? (size_t)(ke - *p) : sizeof(kbuf) - 1;
			memcpy(kbuf, *p, kl);
			kbuf[kl] = '\0';
			*p = ke;
			if (!expect_close(p, end, "key")) {
				return -1;
			}
			int child = parse_value(pl, p, end, node, kbuf);
			if (child < 0) {
				return -1;
			}
		}
	}
	if (strncmp(tag, "array>", 6) == 0) {
		*p += 7;
		int node = add_node(pl, parent, key, PLIST_ARRAY);
		if (node < 0) {
			return -1;
		}
		for (;;) {
			skip_ws(p, end);
			if (*p + 8 <= end && strncmp(*p, "</array>", 8) == 0) {
				*p += 8;
				return node;
			}
			if (*p >= end || **p != '<') {
				return -1;
			}
			if (parse_value(pl, p, end, node, NULL) < 0) {
				return -1;
			}
		}
	}
	if (strncmp(tag, "string>", 7) == 0) {
		*p += 8;
		const char *ve = memchr(*p, '<', (size_t)(end - *p));
		if (!ve) {
			return -1;
		}
		int node = add_node(pl, parent, key, PLIST_STRING);
		if (node < 0) {
			return -1;
		}
		size_t sl = (size_t)(ve - *p);
		pl->nodes[node].str = malloc(sl + 1);
		if (pl->nodes[node].str) {
			memcpy(pl->nodes[node].str, *p, sl);
			pl->nodes[node].str[sl] = '\0';
			xml_unescape(pl->nodes[node].str);
		}
		*p = ve;
		return expect_close(p, end, "string") ? node : -1;
	}
	// The booleans the catalog and sign-in answers carry: self-closing, no
	// body, and read as the integers 1 and 0.
	if (strncmp(tag, "true/>", 6) == 0) {
		*p += 7;
		int node = add_node(pl, parent, key, PLIST_INT);
		if (node < 0) {
			return -1;
		}
		pl->nodes[node].i = 1;
		return node;
	}
	if (strncmp(tag, "false/>", 7) == 0) {
		*p += 8;
		int node = add_node(pl, parent, key, PLIST_INT);
		if (node < 0) {
			return -1;
		}
		pl->nodes[node].i = 0;
		return node;
	}
	if (strncmp(tag, "integer>", 8) == 0) {
		*p += 9;
		char num[32];
		size_t l = 0;
		while (*p < end && l < sizeof(num) - 1 && (isdigit((unsigned char)**p) || **p == '-')) {
			num[l++] = **p;
			(*p)++;
		}
		num[l] = '\0';
		int node = add_node(pl, parent, key, PLIST_INT);
		if (node < 0) {
			return -1;
		}
		pl->nodes[node].i = strtoll(num, NULL, 10);
		return expect_close(p, end, "integer") ? node : -1;
	}
	if (strncmp(tag, "data>", 5) == 0) {
		*p += 6;
		const char *ve = strstr(*p, "</data>");
		if (!ve || (size_t)(ve - *p) > 65536) {
			return -1;
		}
		int node = add_node(pl, parent, key, PLIST_DATA);
		if (node < 0) {
			return -1;
		}
		size_t raw = (size_t)(ve - *p);
		uint8_t *dec = malloc(raw / 4 * 3 + 4);
		if (dec) {
			pl->nodes[node].data_len = b64_decode(*p, raw, dec, raw / 4 * 3 + 4);
			pl->nodes[node].data = dec;
		}
		*p = ve;
		return expect_close(p, end, "data") ? node : -1;
	}
	return -1;
}

bool plist_parse(const char *text, size_t len, plist_t *out) {
	memset(out, 0, sizeof(*out));
	const char *p = text;
	const char *end = text + len;
	// skip to the first <plist> or straight to a <dict>
	skip_ws(&p, end);
	const char *doc = strstr(p, "<plist");
	if (doc) {
		p = doc;
		const char *gt = memchr(p, '>', (size_t)(end - p));
		if (!gt) {
			return false;
		}
		p = gt + 1;
	}
	int root = parse_value(out, &p, end, -1, NULL);
	return root >= 0 && out->nodes[root].kind == PLIST_DICT;
}

void plist_free(plist_t *p) {
	for (int i = 0; i < p->count; i++) {
		free(p->nodes[i].key);
		free(p->nodes[i].str);
		free(p->nodes[i].data);
	}
	p->count = 0;
}

int plist_get_in(const plist_t *p, int parent, const char *key) {
	for (int i = 0; i < p->count; i++) {
		if (p->nodes[i].parent == parent && p->nodes[i].key && strcmp(p->nodes[i].key, key) == 0) {
			return i;
		}
	}
	return -1;
}

int plist_get(const plist_t *p, const char *key) {
	return plist_get_in(p, 0, key);
}

const char *plist_str(const plist_t *p, int node) {
	return node >= 0 && node < p->count && p->nodes[node].kind == PLIST_STRING ? p->nodes[node].str
																			   : NULL;
}

long long plist_int(const plist_t *p, int node) {
	return node >= 0 && node < p->count && p->nodes[node].kind == PLIST_INT ? p->nodes[node].i : 0;
}

const uint8_t *plist_data(const plist_t *p, int node, size_t *len) {
	if (node >= 0 && node < p->count && p->nodes[node].kind == PLIST_DATA) {
		*len = p->nodes[node].data_len;
		return p->nodes[node].data;
	}
	*len = 0;
	return NULL;
}
