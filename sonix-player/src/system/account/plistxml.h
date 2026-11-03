#ifndef PLISTXML_H
#define PLISTXML_H

// The slice of Apple's XML plist that GrandSlam speaks: a dict of strings,
// integers and base64 data, nested. The writer emits what the requests need;
// the reader walks the responses into the same shapes.

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// --- writer ---------------------------------------------------------------
// A growing buffer; the caller frees .buf when done.

typedef struct {
	char *buf;
	size_t len;
	size_t cap;
	int ok;
} plistw_t;

void plistw_init(plistw_t *w);
void plistw_free(plistw_t *w);

// These emit inside an open dict: the key, then the value. Nested dicts and
// arrays are delimited by the begin/end calls.
void plistw_dict_begin(plistw_t *w, const char *key);
void plistw_dict_end(plistw_t *w);
void plistw_array_begin(plistw_t *w, const char *key);
void plistw_array_end(plistw_t *w);
void plistw_string(plistw_t *w, const char *key, const char *value);
void plistw_int(plistw_t *w, const char *key, long long value);
void plistw_data(plistw_t *w, const char *key, const uint8_t *value, size_t len);

// --- reader ---------------------------------------------------------------
// A parsed tree: dicts, strings, integers and data. Everything is owned by
// the plist_t and freed with plist_free().

#define PLIST_MAX_NODES 512

typedef enum { PLIST_DICT, PLIST_ARRAY, PLIST_STRING, PLIST_INT, PLIST_DATA } plist_kind_t;

typedef struct {
	int parent;          // -1 at the root
	char *key;           // the key under which this node sits in its parent
	plist_kind_t kind;
	char *str;           // PLIST_STRING
	long long i;         // PLIST_INT
	uint8_t *data;       // PLIST_DATA
	size_t data_len;
} plist_node_t;

typedef struct {
	plist_node_t nodes[PLIST_MAX_NODES];
	int count;
} plist_t;

// Parses one XML plist document. False on malformed input; the tree is then
// empty and needs no free.
bool plist_parse(const char *text, size_t len, plist_t *out);
void plist_free(plist_t *p);

// Lookups: the node whose key matches, searched depth-first from the root.
int plist_get(const plist_t *p, const char *key);
int plist_get_in(const plist_t *p, int parent, const char *key);
const char *plist_str(const plist_t *p, int node);
long long plist_int(const plist_t *p, int node);
const uint8_t *plist_data(const plist_t *p, int node, size_t *len);

#endif /* PLISTXML_H */
