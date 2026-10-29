#include "ampapi.h"

#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/net/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The catalog is Apple's public MusicKit API: the developer token in the
// Authorization header, the account token in a Cookie -- exactly the pair the
// wrapper's /me exposes. One GET per call, one JSON answer, nothing cached
// here beyond what the caller keeps.

#define AMP_API_LIMIT (512 * 1024)
#define AMP_API_TIMEOUT 20

// With no storefront configured the catalog follows the player's language:
// the mapping is the device's eleven languages onto Apple's storefront codes.
const char *ampapi_default_storefront(void) {
	static const struct {
		const char *lang;
		const char *store;
	} by_lang[] = {
		{"Japanese", "jp"},	 {"Deutsch", "de"},		{"Español", "es"},
		{"Français", "fr"},	 {"Italiano", "it"},	{"Nederlands", "nl"},
		{"Polski", "pl"},	 {"Português BR", "br"}, {"Russian", "ru"},
		{"Ukrainian", "ua"},
	};
	const char *lang = lang_current();
	if (!lang || !lang[0] || strcmp(lang, "English") == 0) {
		return "us";
	}
	for (size_t i = 0; i < sizeof(by_lang) / sizeof(by_lang[0]); i++) {
		if (strcmp(lang, by_lang[i].lang) == 0) {
			return by_lang[i].store;
		}
	}
	return "us";
}

void ampapi_artwork_url(const char *template, int side, char *out, size_t size) {
	if (!template) {
		out[0] = '\0';
		return;
	}
	// The template is ".../{w}x{h}bb.jpg"; both braces mean the same size.
	char buffer[512];
	size_t w = 0;
	for (const char *p = template; *p && w + 16 < sizeof(buffer); p++) {
		if (p[0] == '{' && p[1] == 'w' && p[2] == '}') {
			w += (size_t)snprintf(buffer + w, sizeof(buffer) - w, "%d", side);
			p += 2;
		} else if (p[0] == '{' && p[1] == 'h' && p[2] == '}') {
			w += (size_t)snprintf(buffer + w, sizeof(buffer) - w, "%d", side);
			p += 2;
		} else {
			buffer[w++] = *p;
		}
	}
	buffer[w] = '\0';
	snprintf(out, size, "%s", buffer);
}

static void fill_song(const json_doc_t *doc, int obj, ampapi_song_t *out) {
	memset(out, 0, sizeof(*out));
	json_obj_str(doc, obj, "id", out->id, sizeof(out->id));
	int attrs = json_get(doc, obj, "attributes");
	if (attrs < 0) {
		return;
	}
	json_obj_str(doc, attrs, "name", out->title, sizeof(out->title));
	json_obj_str(doc, attrs, "artistName", out->artist, sizeof(out->artist));
	json_obj_str(doc, attrs, "albumName", out->album, sizeof(out->album));
	out->duration_ms = (int)json_obj_long(doc, attrs, "durationInMillis", 0);
	int art = json_get(doc, attrs, "artwork");
	if (art >= 0) {
		json_obj_str(doc, art, "url", out->artwork, sizeof(out->artwork));
	}
}

int ampapi_search_songs(const char *dev_token, const char *music_user_token,
						const char *storefront, const char *term, ampapi_song_t *out, int max,
						char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!dev_token || !dev_token[0] || !music_user_token || !music_user_token[0] || !term ||
		!term[0] || !out || max <= 0) {
		snprintf(error, error_size, "ampapi_bad_args");
		return -1;
	}
	char headers[2812];
	snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n"
									   "Cookie: media-user-token=%s\r\n"
									   "Origin: https://music.apple.com\r\n",
			 dev_token, music_user_token);

	char term_enc[512];
	http_url_encode(term, term_enc, sizeof(term_enc));
	char store[32];
	snprintf(store, sizeof(store), "%.20s",
			 storefront && storefront[0] ? storefront : ampapi_default_storefront());
	char url[640];
	snprintf(url, sizeof(url),
			 "https://amp-api.music.apple.com/v1/catalog/%s/search?term=%s&types=songs&limit=%d",
			 store, term_enc, max > 50 ? 50 : max);

	char *body = NULL;
	size_t len = 0;
	int status = 0;
	http_req_t req = {
		.extra_headers = headers, .want_error_body = true, .allow_empty_body = true};
	if (!http_request(url, &req, &body, &len, AMP_API_LIMIT, AMP_API_TIMEOUT, &status)) {
		snprintf(error, error_size, "%s", http_last_error() ? http_last_error() : "ampapi_failed");
		return -1;
	}
	if (status != 200) {
		snprintf(error, error_size, "ampapi_http_%d", status);
		free(body);
		return -2;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		snprintf(error, error_size, "ampapi_bad_json");
		return -2;
	}
	free(body);
	int root = json_root(&doc);
	int results = json_get(&doc, root, "results");
	int songs = json_path(&doc, results >= 0 ? results : root, "songs");
	int data = json_get(&doc, songs >= 0 ? songs : root, "data");
	int count = 0;
	if (data >= 0) {
		int total = json_len(&doc, data);
		for (int i = 0; i < total && count < max; i++) {
			int item = json_at(&doc, data, i);
			if (item >= 0) {
				fill_song(&doc, item, &out[count]);
				if (out[count].id[0]) {
					count++;
				}
			}
		}
	}
	json_free(&doc);
	return count;
}
