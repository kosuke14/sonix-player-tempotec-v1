#include "bootpic.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/image/jpeg_planes.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/image/png_scaled.h"
#include "src/system/library/jpeginfo.h"

// Only the JPEG writer is wanted, into memory: no stdio variants.
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
// Static, so the writers not called here are unused, which is not a fault.
#pragma GCC diagnostic ignored "-Wunused-function"
#include "src/system/image/stb_image_write.h"

// The folder's name, in the root of the card.
#define DIR_NAME "Bootlogo"

// The copy, on the writable partition: the one path the boot script can name
// without being told it. /usr/data is where the settings live, which is the
// same argument that put them there -- the rootfs is read-only and the card
// may be missing.
#define DEST_PATH "/usr/data/bootlogo_custom.jpeg"

// How large a source file is read at all. A 12-megapixel camera JPEG is four
// or five; anything past this is a file that was not prepared for a 240x320
// panel, and refusing it here costs less than discovering it during a decode.
#define MAX_SOURCE_BYTES (12u * 1024u * 1024u)

// What the progressive fallback is allowed to hold at its worst moment. The
// same order as the interface's own budget for a cover.
#define PLANES_BUDGET_BYTES (12u * 1024u * 1024u)

// The quality the copy is written at. The five pictures the firmware carries
// are 11 to 31 KB at 240x320; at this setting a photograph lands in the same
// range, so the boot script's decoder has no more to read than it has for the
// picture it ships with.
#define JPEG_QUALITY 85

static char card_root[512];

// Where the copy goes. SONIX_BOOTPIC_DEST moves it for the headless tests,
// the way SONIX_CONFIG moves the settings for them: there is no /usr/data on
// a build machine, and a test that can only be run as root is a test nobody
// runs.
static const char *dest_path(void) {
	const char *override = getenv("SONIX_BOOTPIC_DEST");
	return (override && override[0]) ? override : DEST_PATH;
}

void bootpic_set_root(const char *root) {
	snprintf(card_root, sizeof(card_root), "%s", root ? root : "");
}

const char *bootpic_path(void) { return dest_path(); }

bool bootpic_present(void) { return access(dest_path(), F_OK) == 0; }

const char *bootpic_dir(void) {
	static char dir[512];
	if (!card_root[0] || (size_t)snprintf(dir, sizeof(dir), "%s/%s", card_root, DIR_NAME) >= sizeof(dir)) {
		dir[0] = '\0';
	}
	return dir;
}

// Whether `name` ends with `ext`, either case: the way a camera names its
// files (.JPG) and the way a Mac renames one (.JPG.jpeg) are both ordinary.
static bool ends_with(const char *name, const char *ext) {
	size_t n = strlen(name);
	size_t e = strlen(ext);
	if (n < e) {
		return false;
	}
	return strcasecmp(name + n - e, ext) == 0;
}

// Whether a name is one the folder is read for: a JPEG or a PNG by its
// extension, and not hidden. The extension is the only test made here -- a
// file that turns out not to decode is listed all the same, and says so when
// it is chosen. A picture the list hides is a picture nobody can ask about.
static bool candidate_name(const char *name) {
	if (!name || name[0] == '\0' || name[0] == '.') {
		return false;
	}
	return ends_with(name, ".jpg") || ends_with(name, ".jpeg") || ends_with(name, ".png");
}

// A name that came from the page, checked before it is put behind a path: a
// '/' in it would reach another folder, and a name that is only a picture's
// extension and nothing else is not a file anybody put there.
static bool plain_name(const char *name) {
	return candidate_name(name) && !strchr(name, '/');
}

// dir + "/" + name, refused rather than truncated: a long mount point and a
// long file name together might not fit, and a cut path is a file that does
// not exist.
static bool join_path(char *out, size_t out_size, const char *dir, const char *name) {
	if ((size_t)snprintf(out, out_size, "%s/%s", dir, name) >= out_size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

int bootpic_list(char names[][BOOTPIC_NAME_MAX + 1], int max) {
	if (!names || max <= 0) {
		return 0;
	}
	const char *dir = bootpic_dir();
	DIR *d = dir[0] ? opendir(dir) : NULL;
	if (!d) {
		return 0;
	}

	int count = 0;
	struct dirent *e;
	// Read the whole folder before sorting: the page cut comes AFTER the
	// sort, or a filesystem whose readdir order is hash order (ext4, not
	// tmpfs) would hand back an arbitrary page instead of the alphabetical
	// head the page promises. A static cap keeps the buffer sane; a folder
	// past it is past any page the interface could show anyway.
	enum { BOOTPIC_SCAN_MAX = 128 };
	static char all[BOOTPIC_SCAN_MAX][BOOTPIC_NAME_MAX + 1];
	static pthread_mutex_t scan_mu = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&scan_mu);
	while ((e = readdir(d)) != NULL && count < BOOTPIC_SCAN_MAX) {
		if (!candidate_name(e->d_name)) {
			continue;
		}
		snprintf(all[count], BOOTPIC_NAME_MAX + 1, "%s", e->d_name);
		count++;
	}
	closedir(d);

	// Alphabetical, and case-insensitively so: a card written on Windows and
	// one written on a Mac do not agree about case, and the order of the
	// list should not depend on which. Insertion sort, because a folder of
	// pictures is a handful and qsort() over a two-dimensional array would
	// want a comparator carrying the row width.
	for (int i = 1; i < count; i++) {
		char held[BOOTPIC_NAME_MAX + 1];
		memcpy(held, all[i], BOOTPIC_NAME_MAX + 1);
		int j = i - 1;
		while (j >= 0 && strcasecmp(all[j], held) > 0) {
			memcpy(all[j + 1], all[j], BOOTPIC_NAME_MAX + 1);
			j--;
		}
		memcpy(all[j + 1], held, BOOTPIC_NAME_MAX + 1);
	}
	// The page cut, after the sort.
	if (count > max) {
		count = max;
	}
	for (int i = 0; i < count; i++) {
		memcpy(names[i], all[i], BOOTPIC_NAME_MAX + 1);
	}
	pthread_mutex_unlock(&scan_mu);
	return count;
}

// A PNG's width and height: the first four bytes of its IHDR, big-endian, at
// a fixed offset behind the 8-byte signature and that chunk's own header.
// Enough for a caption; the decoder is what decides whether it is a picture.
static bool png_size(const char *path, int *width, int *height) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	unsigned char head[24];
	size_t got = fread(head, 1, sizeof(head), f);
	fclose(f);
	if (got < sizeof(head) || memcmp(head, "\x89PNG\r\n\x1a\n", 8) != 0) {
		return false;
	}
	*width = (int)((unsigned long)head[16] << 24 | (unsigned long)head[17] << 16 | (unsigned long)head[18] << 8 |
				   (unsigned long)head[19]);
	*height = (int)((unsigned long)head[20] << 24 | (unsigned long)head[21] << 16 | (unsigned long)head[22] << 8 |
					(unsigned long)head[23]);
	return *width > 0 && *height > 0;
}

bool bootpic_size(const char *name, int *width, int *height) {
	if (!width || !height || !plain_name(name)) {
		return false;
	}
	const char *dir = bootpic_dir();
	if (!dir[0]) {
		return false;
	}
	char path[PATH_MAX];
	if (!join_path(path, sizeof(path), dir, name)) {
		return false;
	}
	if (ends_with(name, ".png")) {
		return png_size(path, width, height);
	}
	jpeg_info_t info;
	if (!jpeg_info_read(path, &info)) {
		return false;
	}
	*width = info.width;
	*height = info.height;
	return true;
}

/* ---- the copy ------------------------------------------------------------- */

// The whole file in memory. Both decoders read bytes rather than a path, and
// a picture is read once. `out_size` is left alone on failure.
static uint8_t *slurp(const char *path, size_t want, size_t *out_size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	uint8_t *data = malloc(want);
	if (!data) {
		fclose(f);
		return NULL;
	}
	if (fread(data, 1, want, f) != want) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*out_size = want;
	return data;
}

// The picture, decoded small: never at more resolution than the panel can
// use, and never so small that it has to be enlarged again.
//
// The two JPEG decoders reduce by 1/1, 1/2, 1/4 or 1/8, so what is asked for
// is the size the picture's own shape allows rather than the panel's: for a
// landscape photograph that is the difference between 270 rows and 135.
static uint8_t *decode_small(const uint8_t *data, size_t size, int panel_w, int panel_h, int *out_w, int *out_h) {
	int shortest = panel_w < panel_h ? panel_w : panel_h;
	int longest = panel_w > panel_h ? panel_w : panel_h;

	if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
		// The short side at or above the panel's own short side: the
		// reduction picked is then the largest one that still leaves the
		// resampler a source pixel to average.
		uint8_t *rgb = jpeg_scaled_decode(data, size, shortest / 2, out_w, out_h, NULL, NULL);
		if (rgb) {
			return rgb;
		}
		// Progressive, which the scaled decoder cannot read at all: the
		// plane decoder, at twice the panel so that the short side of a
		// landscape picture clears it too.
		return jpeg_planes_decode(data, size, longest * 2, PLANES_BUDGET_BYTES, out_w, out_h, NULL);
	}
	if (size >= 8 && data[0] == 0x89 && data[1] == 'P') {
		return png_scaled_decode(data, size, shortest, out_w, out_h);
	}
	return NULL;
}

// `src` is a cw x ch rectangle cut out of an sw x sh picture at (x0, y0); the
// result is dw x dh, every pixel the mean of the source pixels it stands for.
//
// A box filter over the rectangle rather than a walk down the source, because
// the panel is bigger than the crop as often as it is smaller -- a picture
// from a phone is reduced, one off the web is enlarged -- and a walk that
// lands one source row on each destination row only works in the first
// direction: enlarged, whole destination rows are never visited and stay
// black, which is a picture striped rather than a picture scaled.
//
// Fixed point, no libm: this runs on one core beside the interface.
static uint8_t *resample_rgb(const uint8_t *src, int sw, int x0, int y0, int cw, int ch, int dw, int dh) {
	uint8_t *dst = malloc((size_t)dw * (size_t)dh * 3);
	if (!dst) {
		return NULL;
	}

	for (int dy = 0; dy < dh; dy++) {
		int ys = dy * ch / dh;		 // the source rows this one covers ...
		int ye = (dy + 1) * ch / dh; // ... and one past the last of them
		if (ye <= ys) {
			ye = ys + 1; // an enlargement: the nearest row, rather than none
		}
		if (ye > ch) {
			ye = ch;
		}
		if (ys >= ch) {
			ys = ch - 1;
		}

		for (int dx = 0; dx < dw; dx++) {
			int xs = dx * cw / dw;
			int xe = (dx + 1) * cw / dw;
			if (xe <= xs) {
				xe = xs + 1;
			}
			if (xe > cw) {
				xe = cw;
			}
			if (xs >= cw) {
				xs = cw - 1;
			}

			uint32_t sum[3] = {0, 0, 0};
			uint32_t n = 0;
			for (int y = ys; y < ye; y++) {
				const uint8_t *line = src + ((size_t)(y0 + y) * sw + x0 + xs) * 3;
				for (int x = 0; x < xe - xs; x++) {
					sum[0] += line[x * 3];
					sum[1] += line[x * 3 + 1];
					sum[2] += line[x * 3 + 2];
					n++;
				}
			}
			uint8_t *out = dst + ((size_t)dy * dw + dx) * 3;
			out[0] = (uint8_t)(sum[0] / n);
			out[1] = (uint8_t)(sum[1] / n);
			out[2] = (uint8_t)(sum[2] / n);
		}
	}
	return dst;
}

// The rectangle of the source the panel is filled with: as much of it as
// covers the panel, centred, the rest cropped. Cutting the longer edge rather
// than padding the shorter one is what makes a landscape photograph fill a
// portrait panel instead of sitting on a band of black.
static void crop_rect(int sw, int sh, int dw, int dh, int *x0, int *y0, int *cw, int *ch) {
	if ((int64_t)sw * dh >= (int64_t)sh * dw) {
		*cw = (int)((int64_t)sh * dw / dh); // wider than the panel: cut the sides
		*ch = sh;
	} else {
		*cw = sw;
		*ch = (int)((int64_t)sw * dh / dw); // taller: cut the top and bottom
	}
	*cw = *cw < 1 ? 1 : *cw;
	*ch = *ch < 1 ? 1 : *ch;
	*x0 = (sw - *cw) / 2;
	*y0 = (sh - *ch) / 2;
}

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
	bool failed;
} sink_t;

static void sink_write(void *context, void *data, int size) {
	sink_t *s = context;
	if (s->failed || size <= 0) {
		return;
	}
	if (s->len + (size_t)size > s->cap) {
		size_t want = s->cap ? s->cap * 2 : 32 * 1024;
		while (want < s->len + (size_t)size) {
			want *= 2;
		}
		uint8_t *grown = realloc(s->data, want);
		if (!grown) {
			s->failed = true;
			return;
		}
		s->data = grown;
		s->cap = want;
	}
	memcpy(s->data + s->len, data, (size_t)size);
	s->len += (size_t)size;
}

// Written beside the real name and moved over it. The next power-on may be
// reading this file while it is being replaced, and a rename is the only way
// to replace it without there being a moment when it is half a JPEG: a boot
// screen drawn from half a JPEG is a screen of garbage, and at a power-on
// there is nobody to explain it.
static bool write_atomically(const char *path, const uint8_t *data, size_t size) {
	char tmp[600];
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
		return false;
	}

	FILE *f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, size, f) == size && fflush(f) == 0;
	if (ok && fsync(fileno(f)) != 0) {
		ok = false;
	}
	if (fclose(f) != 0) {
		ok = false;
	}
	if (!ok) {
		remove(tmp);
		return false;
	}
	if (rename(tmp, path) != 0) {
		remove(tmp);
		return false;
	}
	// Onto the flash rather than left in the page cache: the next thing this
	// device does is usually a reboot, and a copy still in the cache is a
	// copy that was never written.
	sync();
	return true;
}

bootpic_result_t bootpic_install(const char *name, int panel_w, int panel_h) {
	if (!plain_name(name) || panel_w <= 0 || panel_h <= 0) {
		return BOOTPIC_NO_FILE;
	}
	const char *dir = bootpic_dir();
	if (!dir[0]) {
		return BOOTPIC_NO_CARD;
	}
	char path[PATH_MAX];
	if (!join_path(path, sizeof(path), dir, name)) {
		return BOOTPIC_NO_FILE;
	}

	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		return BOOTPIC_NO_FILE; // taken off the card since the page listed it
	}
	if (st.st_size <= 0) {
		return BOOTPIC_UNREADABLE;
	}
	if ((unsigned long long)st.st_size > (unsigned long long)MAX_SOURCE_BYTES) {
		return BOOTPIC_TOO_LARGE;
	}

	size_t size = 0;
	uint8_t *source = slurp(path, (size_t)st.st_size, &size);
	if (!source) {
		return BOOTPIC_UNREADABLE;
	}

	int sw = 0, sh = 0;
	uint8_t *rgb = decode_small(source, size, panel_w, panel_h, &sw, &sh);
	free(source);
	if (!rgb || sw <= 0 || sh <= 0) {
		free(rgb);
		return BOOTPIC_UNREADABLE;
	}

	int cx = 0, cy = 0, cw = 0, ch = 0;
	crop_rect(sw, sh, panel_w, panel_h, &cx, &cy, &cw, &ch);
	uint8_t *fitted = resample_rgb(rgb, sw, cx, cy, cw, ch, panel_w, panel_h);
	free(rgb);
	if (!fitted) {
		return BOOTPIC_NO_MEMORY;
	}

	sink_t sink = {0};
	int encoded = stbi_write_jpg_to_func(sink_write, &sink, panel_w, panel_h, 3, fitted, JPEG_QUALITY);
	free(fitted);
	if (!encoded || sink.failed || sink.len == 0) {
		free(sink.data);
		return BOOTPIC_NO_MEMORY;
	}

	bootpic_result_t result =
		write_atomically(dest_path(), sink.data, sink.len) ? BOOTPIC_OK : BOOTPIC_WRITE_FAILED;
	free(sink.data);
	return result;
}

const char *bootpic_result_key(bootpic_result_t result) {
	switch (result) {
	case BOOTPIC_NO_CARD:
		return "bootpic_no_card";
	case BOOTPIC_NO_FILE:
		return "bootpic_gone";
	case BOOTPIC_TOO_LARGE:
		return "bootpic_too_large";
	case BOOTPIC_NO_MEMORY:
		return "bootpic_no_memory";
	case BOOTPIC_WRITE_FAILED:
		return "bootpic_write_failed";
	case BOOTPIC_UNREADABLE:
	case BOOTPIC_OK:
	default:
		return "bootpic_unreadable";
	}
}
