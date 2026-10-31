#include "ampguest.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/sha256.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/image/miniz/miniz_tinfl.h"
#include "src/system/net/http.h"

// The fetch is one worker at a time, and every step blocks: the archive
// streams to a .part file on the card, the hash vouches for it, and only then
// is the old tree replaced. A failed run leaves the card exactly as it was.

#define SPACE_MARGIN (32ULL * 1024 * 1024) // slack for the FS's own bookkeeping
#define CHUNK (32 * 1024)					// the download read and the inflate feed

// ---------------------------------------------------------------------------
// state, for the GUI to poll
// ---------------------------------------------------------------------------

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static ampguest_state_t s_state = AMPGUEST_IDLE;
static char s_error[64];
static int s_progress = -1;
static pthread_t s_thread;
static bool s_thread_running;

static void set_state(ampguest_state_t state, const char *error) {
	pthread_mutex_lock(&s_lock);
	s_state = state;
	s_progress = state == AMPGUEST_DOWNLOADING ? 0 : -1;
	snprintf(s_error, sizeof(s_error), "%s", error ? error : "");
	pthread_mutex_unlock(&s_lock);
}

bool ampguest_busy(void) {
	pthread_mutex_lock(&s_lock);
	bool busy = s_state == AMPGUEST_DOWNLOADING || s_state == AMPGUEST_UNPACKING;
	pthread_mutex_unlock(&s_lock);
	return busy;
}

int ampguest_progress(void) {
	pthread_mutex_lock(&s_lock);
	int p = s_progress;
	pthread_mutex_unlock(&s_lock);
	return p;
}

bool ampguest_take_result(char *error, size_t error_size) {
	pthread_mutex_lock(&s_lock);
	bool news = s_state == AMPGUEST_DONE || s_state == AMPGUEST_FAILED;
	if (news) {
		snprintf(error, error_size, "%s", s_error);
		s_state = AMPGUEST_IDLE;
		s_error[0] = '\0';
		s_progress = -1;
	}
	pthread_mutex_unlock(&s_lock);
	return news;
}

// ---------------------------------------------------------------------------
// the tree's address on the card
// ---------------------------------------------------------------------------

// Where the tree goes: [applemusic] translate_root, the same setting ampmocal
// spawns the guest from, with its ".ampguest" tail stripped to give the root
// the archive's entries hang under.
static bool card_root(char *out, size_t out_size) {
	const char *setting = config_get("applemusic", "translate_root", "/mnt/sd_0/.ampguest");
	snprintf(out, out_size, "%s", setting);
	size_t n = strlen(out);
	const char tail[] = "/.ampguest";
	if (n > sizeof(tail) - 1 && strcmp(out + n - (sizeof(tail) - 1), tail) == 0) {
		out[n - (sizeof(tail) - 1)] = '\0';
		return true;
	}
	// A custom root without the expected tail: fall back to the card the
	// device reports, which is where such a tree is useful anyway.
	const char *sd = storage_sd_root();
	if (!sd || !*sd) {
		return false;
	}
	snprintf(out, out_size, "%s", sd);
	return true;
}

bool ampguest_present(void) {
	char root[320], marker[512];
	if (!card_root(root, sizeof(root))) {
		return false;
	}
	snprintf(marker, sizeof(marker), "%s/.ampguest/system/bin/linker", root);
	struct stat st;
	return stat(marker, &st) == 0 && S_ISREG(st.st_mode);
}

// rm -rf, bounded to the tree: a stale generation's libraries must not linger
// beside the fresh ones.
static void remove_tree(const char *path) {
	DIR *dir = opendir(path);
	if (dir) {
		struct dirent *de;
		while ((de = readdir(dir)) != NULL) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}
			char child[512];
			snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
			struct stat st;
			if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
				remove_tree(child);
			} else {
				unlink(child);
			}
		}
		closedir(dir);
	}
	rmdir(path);
}

// Every directory down to (but not including) the file at `path`.
static void mkdirs(const char *path) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	char *end = strrchr(tmp, '/');
	if (!end) {
		return;
	}
	*end = '\0';
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755); // existing pieces simply fail, harmlessly
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

// ---------------------------------------------------------------------------
// the tar walk
// ---------------------------------------------------------------------------

typedef struct {
	const char *base;	// the card root; entry names hang under it
	FILE *out;			// the file currently being written
	char name[512];		// its path, for the chmod once complete
	unsigned mode;		// the tar mode, applied when the file closes
	uint64_t body_left; // bytes of the current file still to come
	uint64_t skip_left; // padding, or a whole skipped entry, to discard
	bool skip_names;	// the skipped bytes name the next entry (GNU 'L')
	char longname[256]; // that name, handed to the next header
	int carry_len;		// bytes held back while the next header assembles
	uint8_t carry[512];
	bool done; // the two closing zero blocks were seen
	bool ok;
} tar_t;

static long header_octal(const uint8_t *field, int width) {
	long value = 0;
	for (int i = 0; i < width; i++) {
		uint8_t c = field[i];
		if (c < '0' || c > '7') {
			break;
		}
		value = value * 8 + (c - '0');
	}
	return value;
}

// Entry paths come from the archive, so they are checked, not trusted:
// nothing escapes the tree, nothing climbs out of it.
static bool entry_path(tar_t *t, const char *name, char *out, size_t out_size) {
	if (name[0] == '\0' || name[0] == '/' || strstr(name, "..") != NULL) {
		return false;
	}
	if (snprintf(out, out_size, "%s/%s", t->base, name) >= (int)out_size) {
		return false;
	}
	return true;
}

static bool tar_header(tar_t *t, const uint8_t *h) {
	if (h[0] == '\0') {
		t->done = true; // the first of the two closing zero blocks
		return true;
	}

	long size = header_octal(h + 124, 12);
	uint64_t pad = ((512 - (uint64_t)size % 512) % 512);
	char type = h[156] ? (char)h[156] : '0';

	if (type == 'L') { // GNU long name: the body names the next entry
		if (size <= 0 || (size_t)size >= sizeof(t->longname)) {
			return false;
		}
		t->longname[0] = '\0';
		t->skip_left = (uint64_t)size + pad;
		t->skip_names = true;
		return true;
	}
	t->skip_names = false;

	// Everything that carries no payload of its own to place: pax headers,
	// links, anything this archive has no reason to contain.
	if (type != '0' && type != '5') {
		t->skip_left = (uint64_t)size + pad;
		return true;
	}

	const char *name = t->longname[0] ? t->longname : (const char *)h;
	t->longname[0] = '\0';
	char path[512];
	if (!entry_path(t, name, path, sizeof(path))) {
		return false;
	}

	if (type == '5') {
		mkdirs(path);
		return true;
	}

	mkdirs(path);
	t->out = fopen(path, "wb");
	if (!t->out) {
		return false;
	}
	snprintf(t->name, sizeof(t->name), "%s", path);
	t->mode = (unsigned)header_octal(h + 100, 8) & 0777;
	t->body_left = (uint64_t)size;
	t->skip_left = pad; // the body's padding, skipped once the file closes
	return true;
}

static void tar_flush_file(tar_t *t) {
	if (t->out) {
		fclose(t->out);
		t->out = NULL;
		chmod(t->name, t->mode); // the exec bits qemu and the daemons need
	}
}

// A skipped span is usually padding, but a GNU long-name entry's payload
// names the next file and has to be kept (up to its NUL; the padding after
// it is zeros and must not overwrite the name).
static void tar_skip(tar_t *t, const uint8_t *data, size_t len) {
	if (!t->skip_names || len == 0) {
		return;
	}
	size_t have = strlen(t->longname);
	for (size_t i = 0; i < len && have + 1 < sizeof(t->longname); i++) {
		if (data[i] == '\0') {
			t->skip_names = false;
			break;
		}
		t->longname[have++] = data[i];
	}
	if (have < sizeof(t->longname)) {
		t->longname[have] = '\0';
	}
}

// Feeds the inflated stream in. Returns false only on an unusable archive;
// a complete one ends with t->done.
static bool tar_feed(tar_t *t, const uint8_t *data, size_t len) {
	while (len > 0 && t->ok && !t->done) {
		if (t->body_left > 0) {
			size_t take = len < t->body_left ? len : (size_t)t->body_left;
			if (fwrite(data, 1, take, t->out) != take) {
				t->ok = false;
				break;
			}
			data += take;
			len -= take;
			t->body_left -= take;
			if (t->body_left == 0) {
				tar_flush_file(t);
			}
		} else if (t->skip_left > 0) {
			size_t take = len < t->skip_left ? len : (size_t)t->skip_left;
			tar_skip(t, data, take);
			data += take;
			len -= take;
			t->skip_left -= take;
		} else if (t->carry_len < 512) {
			size_t want = 512 - (size_t)t->carry_len;
			size_t take = len < want ? len : want;
			memcpy(t->carry + t->carry_len, data, take);
			t->carry_len += (int)take;
			data += take;
			len -= take;
			if (t->carry_len == 512) {
				t->carry_len = 0;
				t->ok = tar_header(t, t->carry);
			}
		}
	}
	return t->ok;
}

// ---------------------------------------------------------------------------
// the gzip layer
// ---------------------------------------------------------------------------

typedef enum {
	GH_FIXED,   // the 10 fixed bytes; byte 3 carries the FLG bitmask
	GH_XLEN,	// FEXTRA's two little-endian length bytes
	GH_EXTRA,	// FEXTRA's payload, counted off and ignored
	GH_NAME,	// FNAME, up to the NUL
	GH_COMMENT, // FCOMMENT, up to the NUL
	GH_HCRC,	// FHCRC's two bytes
	GH_BODY,	// the raw deflate stream
} gh_state_t;

typedef struct {
	gh_state_t state;
	unsigned index;	  // byte position inside the current header piece
	unsigned flags;	  // the FLG bitmask, once byte 3 has passed
	unsigned xlen;	  // FEXTRA's length, once announced
	tinfl_decompressor *inflator;
	uint8_t *window;
	size_t window_pos;
	bool final_input; // no more compressed bytes will ever arrive
	tar_t tar;
} gz_t;

static void gz_start(gz_t *g, const char *base) {
	memset(g, 0, sizeof(*g));
	g->state = GH_FIXED;
	g->inflator = tinfl_decompressor_alloc();
	g->window = malloc(TINFL_LZ_DICT_SIZE);
	g->tar.base = base;
	g->tar.ok = true;
	if (g->inflator) {
		tinfl_init(g->inflator);
	}
}

static void gz_stop(gz_t *g) {
	if (g->inflator) {
		tinfl_decompressor_free(g->inflator);
	}
	free(g->window);
	tar_flush_file(&g->tar);
}

// Which header piece follows, by the FLG bits not yet accounted for. The
// fields come in a fixed order; an absent one is simply stepped over.
static gh_state_t gz_next_field(unsigned flags) {
	if (flags & 4) {
		return GH_XLEN;
	}
	if (flags & 8) {
		return GH_NAME;
	}
	if (flags & 16) {
		return GH_COMMENT;
	}
	if (flags & 2) {
		return GH_HCRC;
	}
	return GH_BODY;
}

// Hands the freshly inflated bytes to the tar walk and keeps the circular
// window bookkeeping: tinfl writes from window_pos onward without wrapping,
// so the fresh region is always one contiguous span. False only on a tar
// error; a completed tar is tar.done, which the callers check.
static bool gz_drain(gz_t *g, size_t fresh) {
	if (fresh && !tar_feed(&g->tar, g->window + g->window_pos, fresh)) {
		return false;
	}
	g->window_pos += fresh;
	if (g->window_pos == TINFL_LZ_DICT_SIZE) {
		g->window_pos = 0;
	}
	return g->tar.ok;
}

// The whole compressed input, a chunk at a time. The header walks a byte at
// a time -- it is tiny, and every optional field in it is counted off rather
// than kept. False only on an unusable archive; a complete one ends with
// tar.done.
static bool gz_feed(gz_t *g, const uint8_t *data, size_t len) {
	while (len > 0 && g->state != GH_BODY) {
		uint8_t byte = *data++;
		len--;
		switch (g->state) {
		case GH_FIXED:
			if (g->index == 3) {
				g->flags = byte; // CM is 8 for deflate; MTIME, XFL and OS are noise
			}
			if (++g->index == 10) {
				g->state = gz_next_field(g->flags);
			}
			break;
		case GH_XLEN:
			g->xlen = (g->xlen >> 8) | ((unsigned)byte << 8); // little-endian
			if (++g->index == 2) {
				g->state = g->xlen ? GH_EXTRA : gz_next_field(g->flags & ~4u);
			}
			break;
		case GH_EXTRA:
			if (++g->index == g->xlen) {
				g->state = gz_next_field(g->flags & ~4u);
			}
			break;
		case GH_NAME:
			if (byte == '\0') {
				g->state = gz_next_field(g->flags & ~8u);
			}
			break;
		case GH_COMMENT:
			if (byte == '\0') {
				g->state = gz_next_field(g->flags & ~16u);
			}
			break;
		case GH_HCRC:
			if (++g->index == 2) {
				g->state = GH_BODY;
			}
			break;
		default:
			break;
		}
	}
	if (g->state != GH_BODY) {
		return true;
	}

	for (;;) {
		if (len == 0 && !g->final_input) {
			return g->tar.ok;
		}
		size_t in_bytes = len;
		size_t out_bytes = TINFL_LZ_DICT_SIZE - g->window_pos;
		tinfl_status status =
			tinfl_decompress(g->inflator, data, &in_bytes, g->window, g->window + g->window_pos,
							 &out_bytes, g->final_input ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
		data += in_bytes;
		len -= in_bytes;

		if (status < TINFL_STATUS_DONE) {
			return false; // corrupt stream
		}
		if (status == TINFL_STATUS_NEEDS_MORE_INPUT && g->final_input) {
			return false; // the stream ended early
		}
		if (!gz_drain(g, out_bytes)) {
			return false; // tar failure, or the tar already saw its end
		}
		if (status == TINFL_STATUS_DONE) {
			return g->tar.done;
		}
		if (len == 0) {
			return g->tar.ok;
		}
	}
}

// ---------------------------------------------------------------------------
// the fetch
// ---------------------------------------------------------------------------

typedef struct {
	ampguest_state_t result;
	char error[64];
} fetch_outcome_t;

static char s_chunk[CHUNK]; // the download read and the inflate feed; the
static char s_packed[CHUNK]; // fetch is one worker at a time

static fetch_outcome_t do_fetch(void) {
	fetch_outcome_t out = {.result = AMPGUEST_FAILED, .error = "amp_guest_failed"};

	char root[320];
	if (!card_root(root, sizeof(root))) {
		return out;
	}

	http_stream_options_t opts = {.no_icy = true, .no_https_downgrade = true};
	http_stream_t stream;
	memset(&stream, 0, sizeof(stream));
	stream.fd = -1;
	if (!http_stream_open_ex(&stream, AMPGUEST_URL, 60, &opts)) {
		return out; // the network's own toast is the GUI's generic one
	}

	uint64_t archive = stream.content_length64 > 0 ? stream.content_length64 : AMPGUEST_ARCHIVE_BYTES;

	// The space question, before a single byte is written: the archive in
	// transit plus the tree it expands to, with room to spare.
	struct statvfs vfs;
	if (statvfs(root, &vfs) == 0) {
		uint64_t unit = vfs.f_frsize ? (uint64_t)vfs.f_frsize : (uint64_t)vfs.f_bsize;
		uint64_t avail = (uint64_t)vfs.f_bavail * unit;
		if (avail < archive + AMPGUEST_UNPACKED_BYTES + SPACE_MARGIN) {
			http_stream_close(&stream);
			snprintf(out.error, sizeof(out.error), "amp_guest_no_space");
			return out;
		}
	}

	char part[512];
	snprintf(part, sizeof(part), "%s/.ampguest.tar.gz.part", root);
	FILE *f = fopen(part, "wb");
	if (!f) {
		http_stream_close(&stream);
		return out;
	}

	sha256_ctx sha;
	sha256_init(&sha);
	uint64_t done = 0;
	bool connected = true;
	for (;;) {
		int n = http_stream_read(&stream, s_chunk, sizeof(s_chunk));
		if (n < 0) {
			connected = false;
			break;
		}
		if (n == 0) {
			break; // end of body
		}
		if (fwrite(s_chunk, 1, (size_t)n, f) != (size_t)n) {
			connected = false;
			break;
		}
		sha256_update(&sha, s_chunk, (size_t)n);
		done += (uint64_t)n;
		if (stream.content_length64 > 0) {
			pthread_mutex_lock(&s_lock);
			s_progress = (int)(done * 100 / stream.content_length64);
			pthread_mutex_unlock(&s_lock);
		}
	}
	http_stream_close(&stream);

	bool complete = connected && done > 0 &&
					(stream.content_length64 == 0 || done == stream.content_length64);
	if (complete && (fflush(f) != 0 || fsync(fileno(f)) != 0)) {
		complete = false;
	}
	fclose(f);
	if (!complete) {
		unlink(part);
		return out;
	}

	char hex[SHA256_HEX_LEN];
	sha256_final_hex(&sha, hex);
	if (strcasecmp(hex, AMPGUEST_SHA256) != 0) {
		unlink(part);
		return out;
	}

	set_state(AMPGUEST_UNPACKING, NULL);

	// The old tree goes only now that the replacement has proven itself, and
	// a fresh unpack must not mix generations. The .part sits beside the
	// tree, so it survives this.
	char tree[512];
	snprintf(tree, sizeof(tree), "%s/.ampguest", root);
	remove_tree(tree);

	FILE *in = fopen(part, "rb");
	if (!in) {
		return out;
	}

	gz_t gz;
	gz_start(&gz, root);
	bool ok = gz.inflator && gz.window;
	size_t n;
	while (ok && (n = fread(s_packed, 1, sizeof(s_packed), in)) > 0) {
		ok = gz_feed(&gz, (const uint8_t *)s_packed, n);
	}
	if (ok) {
		gz.final_input = true;
		ok = gz_feed(&gz, NULL, 0) && gz.tar.done;
	}
	gz_stop(&gz);
	fclose(in);

	char marker[512];
	snprintf(marker, sizeof(marker), "%s/.ampguest/system/bin/linker", root);
	struct stat st;
	ok = ok && stat(marker, &st) == 0 && S_ISREG(st.st_mode);

	unlink(part);
	sync();
	if (!ok) {
		return out;
	}
	out.result = AMPGUEST_DONE;
	out.error[0] = '\0';
	return out;
}

static void *fetch_thread(void *unused) {
	(void)unused;
	thread_be_low_priority("ampguest fetch");
	fetch_outcome_t out = do_fetch();
	set_state(out.result, out.error);
	pthread_mutex_lock(&s_lock);
	s_thread_running = false; // the result stays readable until take_result()
	pthread_mutex_unlock(&s_lock);
	return NULL;
}

bool ampguest_fetch_start(void) {
	pthread_mutex_lock(&s_lock);
	if (s_state == AMPGUEST_DOWNLOADING || s_state == AMPGUEST_UNPACKING || s_thread_running) {
		pthread_mutex_unlock(&s_lock);
		return false;
	}
	s_thread_running = true;
	pthread_mutex_unlock(&s_lock);

	if (pthread_create(&s_thread, NULL, fetch_thread, NULL) != 0) {
		pthread_mutex_lock(&s_lock);
		s_thread_running = false;
		pthread_mutex_unlock(&s_lock);
		return false;
	}
	pthread_detach(s_thread);
	set_state(AMPGUEST_DOWNLOADING, NULL);
	return true;
}
