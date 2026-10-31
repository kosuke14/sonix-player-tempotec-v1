// The guest-tree fetcher against local fakes: the "server" is a byte array
// in this file, the "card" a temp directory, and the pinned hash a compile-
// time override pointing at the fixture. What is under test is the whole
// fetch -- the space check, the sha256 gate, the gzip inflate and the tar
// walk -- plus the two ways a download can fail without touching the tree
// that is already on the card.
//
// The fixture is a real gzip -9 of a real tar: .ampguest/ with an executable
// system/bin/linker and a nested system/lib36/libfixture.so, the same shape
// the card's tree has, shrunk to a few hundred bytes.
#include "src/system/streaming/ampguest.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// the fixture: gzip -9 of the miniature tree
// ---------------------------------------------------------------------------

static const unsigned char fixture_gz[] = {
	0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xed, 0xd6, 0xeb, 0x8a, 0xc2, 0x30,
	0x10, 0x86, 0xe1, 0x5c, 0x4a, 0x6f, 0xa0, 0x87, 0x4c, 0x33, 0x9d, 0xeb, 0x69, 0x25, 0x2e, 0x45,
	0x8d, 0x92, 0x03, 0xd8, 0xbb, 0x37, 0xcb, 0xa2, 0x2c, 0x2e, 0xab, 0x20, 0xd5, 0xae, 0xec, 0xf7,
	0xfc, 0x48, 0x0a, 0xe9, 0xbf, 0x97, 0x4e, 0x5a, 0xf5, 0xbb, 0xc3, 0x47, 0xb2, 0x21, 0xd6, 0xea,
	0x69, 0x9a, 0x4c, 0x84, 0x3f, 0x77, 0x2d, 0xac, 0xbf, 0xef, 0x67, 0x4a, 0x33, 0x89, 0x6e, 0x85,
	0x99, 0x49, 0x35, 0x9a, 0x0c, 0x89, 0x2a, 0x58, 0xbd, 0x40, 0x0a, 0xb1, 0xf7, 0x45, 0xa1, 0xd2,
	0x90, 0x5c, 0x4c, 0x37, 0xde, 0xbb, 0x73, 0xfe, 0xa6, 0xaa, 0x4b, 0xff, 0x30, 0x85, 0x68, 0x77,
	0xf5, 0x9f, 0xe8, 0x9f, 0x1f, 0x5a, 0xf4, 0x5f, 0xa4, 0xff, 0x76, 0x1c, 0xda, 0xae, 0x5e, 0xb8,
	0xbf, 0xe9, 0x84, 0xd0, 0x7f, 0xc1, 0xfe, 0x79, 0x5d, 0x8f, 0xc7, 0x98, 0xbc, 0xad, 0xc2, 0x7e,
	0x8e, 0xfe, 0x9d, 0x31, 0xbf, 0xf7, 0xd7, 0x72, 0xd5, 0x5f, 0x4c, 0xd3, 0xa9, 0xa2, 0x41, 0xff,
	0xa7, 0xcb, 0xa1, 0x7d, 0xef, 0xa7, 0x72, 0xb5, 0x77, 0xd1, 0xba, 0xa8, 0xe0, 0x7f, 0xf9, 0xf1,
	0xfd, 0x0f, 0xa3, 0x9b, 0xfb, 0x1f, 0xe0, 0x81, 0xf9, 0xcf, 0x84, 0xfb, 0x7f, 0xb9, 0xfe, 0xdb,
	0xd1, 0x6d, 0xac, 0x9f, 0xb7, 0x3f, 0xdf, 0xe8, 0x4f, 0x74, 0xd5, 0x9f, 0x85, 0x18, 0xf3, 0xff,
	0x15, 0xd6, 0xfd, 0xc6, 0x96, 0x5f, 0xbd, 0xcb, 0x9c, 0x3e, 0x5f, 0x05, 0x98, 0x89, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xef, 0xec, 0x04, 0xe1, 0xb4, 0x70,
	0xa8, 0x00, 0x28, 0x00, 0x00,
};

// The fixture's own hash, compiled in so the fetcher accepts it.
#define FIXTURE_SHA256 "588a6cf87db24f5e8c8e45e70d117287e5e49051b6e7702c42a105b35efaf783"

// ---------------------------------------------------------------------------
// the fakes: a card in /tmp, a server in an array
// ---------------------------------------------------------------------------

static char card[256];
static bool corrupt_body;	 // flip one byte of the archive
static bool short_body;		 // lie about the length, send less
static int serve_fail;		 // >0: the connection never opens

const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	(void)fallback;
	static char root[300];
	snprintf(root, sizeof(root), "%s/.ampguest", card);
	return root;
}
long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	(void)fallback;
	return 0;
}
const char *storage_sd_root(void) { return card; }
void thread_be_low_priority(const char *name) { (void)name; }

bool http_stream_open_ex(http_stream_t *st, const char *url, int timeout_secs,
						 const http_stream_options_t *options) {
	(void)url;
	(void)timeout_secs;
	(void)options;
	memset(st, 0, sizeof(*st));
	st->fd = -1;
	if (serve_fail) {
		serve_fail--;
		return false;
	}
	st->content_length64 = sizeof(fixture_gz) + (short_body ? 100 : 0);
	return true;
}

static size_t served;
int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st;
	size_t left = sizeof(fixture_gz) - served;
	if (short_body && left > sizeof(fixture_gz) / 2) {
		left = sizeof(fixture_gz) / 2; // the connection dies halfway
	}
	if (left == 0) {
		return 0;
	}
	size_t take = (size_t)len < left ? (size_t)len : left;
	memcpy(buf, fixture_gz + served, take);
	if (corrupt_body && served == 0) {
		((unsigned char *)buf)[10] ^= 0xff; // the first deflate byte, so the
											// hash misses but gzip still dies
	}
	served += take;
	return (int)take;
}
void http_stream_close(http_stream_t *st) { (void)st; }

// ---------------------------------------------------------------------------
// the driver
// ---------------------------------------------------------------------------

static char pending_error[128];

// Runs a fetch to its end; the news lands in pending_error, the same way the
// GUI polls for it.
static void run_fetch(void) {
	served = 0;
	pending_error[0] = '\0';
	assert(ampguest_fetch_start());
	while (!ampguest_take_result(pending_error, sizeof(pending_error))) {
		usleep(10 * 1000);
	}
}

static bool file_has(const char *path, const char *content) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	char buf[128];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return strcmp(buf, content) == 0;
}

int main(void) {
	snprintf(card, sizeof(card), "/tmp/opencode/ampguest-test-card");
	char cmd[600];

	// A tree already on the card, the previous generation the fetch must not
	// damage unless it succeeds.
	snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s/.ampguest/system/lib65", card, card);
	assert(system(cmd) == 0);
	char path[512];
	snprintf(path, sizeof(path), "%s/.ampguest/system/lib65/libold.so", card);
	FILE *f = fopen(path, "wb");
	assert(f);
	fwrite("old", 1, 3, f);
	fclose(f);
	assert(ampguest_present() == false); // no linker marker yet

	// A corrupted archive: the hash gate refuses it, and the old tree keeps
	// living on the card.
	corrupt_body = true;
	run_fetch();
	assert(strcmp(pending_error, "amp_guest_failed") == 0);
	snprintf(path, sizeof(path), "%s/.ampguest/system/lib65/libold.so", card);
	assert(file_has(path, "old"));
	snprintf(path, sizeof(path), "%s/.ampguest.tar.gz.part", card);
	assert(access(path, F_OK) != 0); // the .part never survives a failure

	// A short download: the server promised more than it sent.
	corrupt_body = false;
	short_body = true;
	run_fetch();
	assert(strcmp(pending_error, "amp_guest_failed") == 0);
	snprintf(path, sizeof(path), "%s/.ampguest/system/lib65/libold.so", card);
	assert(file_has(path, "old")); // path still names libold.so

	// A connection that never opens.
	short_body = false;
	serve_fail = 1;
	run_fetch();
	assert(strcmp(pending_error, "amp_guest_failed") == 0);
	assert(file_has(path, "old"));

	// The good archive: the old tree is replaced wholesale, the executables
	// keep their exec bits, the marker says present.
	serve_fail = 0;
	run_fetch();
	assert(strcmp(pending_error, "") == 0);
	snprintf(path, sizeof(path), "%s/.ampguest/system/bin/linker", card);
	assert(file_has(path, "fake-linker-binary"));
	struct stat st;
	assert(stat(path, &st) == 0 && (st.st_mode & S_IXUSR)); // the exec bit
	snprintf(path, sizeof(path), "%s/.ampguest/system/lib36/libfixture.so", card);
	assert(file_has(path, "library-content"));
	snprintf(path, sizeof(path), "%s/.ampguest/system/lib65/libold.so", card);
	assert(access(path, F_OK) != 0); // the stale generation is gone
	assert(ampguest_present());

	// The second good run over an existing tree, the overwrite path.
	run_fetch();
	assert(strcmp(pending_error, "") == 0);
	assert(ampguest_present());

	printf("ampguest tests passed\n");
	return 0;
}
