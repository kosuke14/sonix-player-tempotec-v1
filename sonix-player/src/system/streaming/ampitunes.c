#include "ampitunes.h"

#include "src/system/core/json.h"
#include "src/system/net/http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// The search is one GET to itunes.apple.com and one JSON answer; the fetch is
// one streamed GET written straight to the card. Nothing is held in memory
// beyond the JSON text itself.

#define AMPITUNES_API_LIMIT (512 * 1024)
#define AMPITUNES_TIMEOUT 20
#define AMPITUNES_DIR ".local/amp-preview"

static char sd_root[512];
static bool root_ready;
static char api_base[160] = "https://itunes.apple.com";

void ampitunes_set_root(const char *root) {
	snprintf(sd_root, sizeof(sd_root), "%s", root && root[0] ? root : "");
	root_ready = sd_root[0] != '\0';
}

void ampitunes_set_api(const char *base) {
	snprintf(api_base, sizeof(api_base), "%s", base && base[0] ? base : api_base);
}

static void preview_path(long track_id, char *out, size_t size) {
	snprintf(out, size, "%s/" AMPITUNES_DIR "/%ld.m4a", sd_root, track_id);
}

static bool plain_file(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool ampitunes_ready(const ampapi_song_t *song, char *out, size_t size) {
	if (!root_ready || !song || !song->preview[0] || !song->id[0]) {
		return false;
	}
	char path[768];
	preview_path(strtol(song->id, NULL, 10), path, sizeof(path));
	if (!plain_file(path)) {
		return false;
	}
	snprintf(out, size, "%s", path);
	return true;
}

static int search_body(const char *url, ampapi_song_t *out, int max, char *error,
					   size_t error_size);

int ampitunes_search(const char *term, ampapi_song_t *out, int max, char *error,
					 size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!term || !term[0] || !out || max <= 0) {
		snprintf(error, error_size, "ampitunes_bad_args");
		return -1;
	}
	char term_enc[512];
	http_url_encode(term, term_enc, sizeof(term_enc));
	char url[768];
	snprintf(url, sizeof(url),
			 "%s/search?media=music&entity=song&limit=%d&term=%s",
			 api_base, max > 50 ? 50 : max, term_enc);
	return search_body(url, out, max, error, error_size);
}

// The lookup endpoint answers with the collection itself first (a
// wrapperType of "collection", no previewUrl) and the tracks after it; only
// songs are kept, which drops the collection row on its own.
int ampitunes_album_tracks(const char *album_id, ampapi_song_t *out, int max, char *error,
						   size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!album_id || !album_id[0] || !out || max <= 0) {
		snprintf(error, error_size, "ampitunes_bad_args");
		return -1;
	}
	// The id is digits by construction, coming from the catalog itself;
	// refusing anything else keeps the path a path.
	for (const char *c = album_id; *c; c++) {
		if (*c < '0' || *c > '9') {
			snprintf(error, error_size, "ampitunes_bad_args");
			return -1;
		}
	}
	char url[512];
	snprintf(url, sizeof(url), "%s/lookup?id=%s&entity=song&limit=%d",
			 api_base, album_id, max > 100 ? 100 : max);
	return search_body(url, out, max, error, error_size);
}

// The lookup endpoint answers with the collection itself first (a
// wrapperType of "collection", no previewUrl) and the tracks after it; only
// songs are kept, which drops the collection row on its own.
// One GET and one JSON answer, shared by the search and the lookup: the
// results array carries the same song objects in both.
static int search_body(const char *url, ampapi_song_t *out, int max, char *error,
					   size_t error_size) {
	char *body = NULL;
	size_t len = 0;
	int status = 0;
	http_req_t req = {.want_error_body = true, .allow_empty_body = true};
	if (!http_request(url, &req, &body, &len, AMPITUNES_API_LIMIT, AMPITUNES_TIMEOUT, &status)) {
		const char *why = http_last_error();
		snprintf(error, error_size, "%s", why ? why : "ampitunes_failed");
		return -1;
	}
	if (status != 200) {
		snprintf(error, error_size, "ampitunes_http_%d", status);
		free(body);
		return -2;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		snprintf(error, error_size, "ampitunes_bad_json");
		return -2;
	}
	free(body);

	int results = json_get(&doc, json_root(&doc), "results");
	int count = 0;
	if (results >= 0) {
		int total = json_len(&doc, results);
		for (int i = 0; i < total && count < max; i++) {
			int item = json_at(&doc, results, i);
			if (item < 0) {
				continue;
			}
			// Only songs: the answer also carries music videos and the like.
			char kind[32];
			json_obj_str(&doc, item, "kind", kind, sizeof(kind));
			if (strcmp(kind, "song") != 0) {
				continue;
			}
			ampapi_song_t *s = &out[count];
			memset(s, 0, sizeof(*s));
			snprintf(s->id, sizeof(s->id), "%ld", json_obj_long(&doc, item, "trackId", 0));
			snprintf(s->album_id, sizeof(s->album_id), "%ld",
					 json_obj_long(&doc, item, "collectionId", 0));
			json_obj_str(&doc, item, "trackName", s->title, sizeof(s->title));
			json_obj_str(&doc, item, "artistName", s->artist, sizeof(s->artist));
			json_obj_str(&doc, item, "collectionName", s->album, sizeof(s->album));
			json_obj_str(&doc, item, "previewUrl", s->preview, sizeof(s->preview));
			s->duration_ms = (int)json_obj_long(&doc, item, "trackTimeMillis", 0);
			char art[512];
			json_obj_str(&doc, item, "artworkUrl100", art, sizeof(art));
			// The artwork server cuts any square it is asked for: the
			// 100x100 address becomes 300x300 with one substitution, which
			// is the size the player's rows read well at.
			char *cut = strstr(art, "100x100bb");
			if (cut) {
				memcpy(cut, "300x300bb", 9);
			}
			snprintf(s->artwork, sizeof(s->artwork), "%s", art);
			// A row without an id or a preview cannot be played at all.
			if (s->id[0] && s->id[0] != '0' && s->preview[0]) {
				count++;
			}
		}
	}
	json_free(&doc);
	if (count == 0 && !error[0]) {
		snprintf(error, error_size, "amp_no_results");
	}
	return count;
}

bool ampitunes_fetch(const ampapi_song_t *song, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!root_ready || !song || !song->preview[0] || !song->id[0]) {
		snprintf(error, error_size, "ampitunes_bad_args");
		return false;
	}
	long id = strtol(song->id, NULL, 10);
	char path[768];
	preview_path(id, path, sizeof(path));
	if (plain_file(path)) {
		return true; // already on the card
	}

	char dir[640];
	// The parents are made in order: the card root exists, .local may not.
	snprintf(dir, sizeof(dir), "%s/.local", sd_root);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		snprintf(error, error_size, "ampitunes_no_card");
		return false;
	}
	snprintf(dir, sizeof(dir), "%s/" AMPITUNES_DIR, sd_root);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		snprintf(error, error_size, "ampitunes_no_card");
		return false;
	}

	char tmp[800];
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		snprintf(error, error_size, "ampitunes_no_card");
		return false;
	}

	http_stream_t st;
	if (!http_stream_open(&st, song->preview, AMPITUNES_TIMEOUT)) {
		fclose(f);
		remove(tmp);
		const char *why = http_last_error();
		snprintf(error, error_size, "%s", why ? why : "ampitunes_failed");
		return false;
	}

	bool ok = true;
	char buf[8192];
	long done = 0;
	for (;;) {
		int n = http_stream_read(&st, buf, (int)sizeof(buf));
		if (n < 0) {
			ok = false;
			break;
		}
		if (n == 0) {
			break;
		}
		if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
			ok = false;
			break;
		}
		done += n;
	}
	http_stream_close(&st);
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok && done == 0) {
		ok = false; // an empty answer is not a track
	}
	if (ok) {
		if (rename(tmp, path) != 0) {
			ok = false;
		}
	}
	if (!ok) {
		remove(tmp);
		if (error && error_size && !error[0]) {
			snprintf(error, error_size, "ampitunes_failed");
		}
	}
	return ok;
}
