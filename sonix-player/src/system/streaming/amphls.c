#include "amphls.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

// Resolves a playlist URI against the URL it came from. Apple's URLs carry
// their credentials in the path, so the join is purely lexical: everything up
// to the last '/', then the fragment, with "../" walked and absolute
// fragments taken as they are. The master's query string is deliberately
// dropped -- that is how every HLS client joins, and the segments carry their
// own parameters.
static void resolve_url(const char *base, const char *uri, char *out, size_t size) {
	if (!uri || !uri[0]) {
		out[0] = '\0';
		return;
	}
	if (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0) {
		snprintf(out, size, "%s", uri);
		return;
	}
	if (uri[0] == '/') {
		// Root-relative: scheme and authority of the base, then the fragment.
		const char *sep = strstr(base, "://");
		const char *rest = sep ? sep + 3 : base;
		const char *slash = strchr(rest, '/');
		size_t prefix = slash ? (size_t)(slash - base) : strlen(base);
		snprintf(out, size, "%.*s%s", (int)prefix, base, uri);
		return;
	}
	const char *slash = strrchr(base, '/');
	if (!slash) {
		snprintf(out, size, "%s", uri);
		return;
	}
	size_t dir = (size_t)(slash + 1 - base);
	if (dir >= size) {
		out[0] = '\0';
		return;
	}
	memcpy(out, base, dir);
	size_t at = dir;
	// Walk "../" against the base's directories; ordinary names append.
	const char *p = uri;
	while (strncmp(p, "../", 3) == 0) {
		if (at > 0 && out[at - 1] == '/') {
			at--;
		}
		while (at > 0 && out[at - 1] != '/') {
			at--;
		}
		p += 3;
	}
	snprintf(out + at, size - at, "%s", p);
}

// The attribute value of `name` inside a tag line ("URI=\"...\""), quotes
// stripped. False when the attribute is not there.
static bool tag_attribute(const char *line, const char *name, char *out, size_t size) {
	size_t name_len = strlen(name);
	for (const char *p = line; (p = strstr(p, name)) != NULL; p += name_len) {
		// A real attribute boundary: start of line, or the colon after the
		// tag name itself (#EXT-X-MAP:URI), or a comma between attributes.
		if (p != line && p[-1] != ',' && p[-1] != ':') {
			continue;
		}
		const char *v = p + name_len;
		if (*v != '=') {
			continue;
		}
		v++;
		if (*v == '"') {
			v++;
			const char *close = strchr(v, '"');
			if (!close) {
				return false;
			}
			size_t n = (size_t)(close - v) < size - 1 ? (size_t)(close - v) : size - 1;
			memcpy(out, v, n);
			out[n] = '\0';
			return true;
		}
		const char *end = strchr(v, ',');
		size_t n = end ? (size_t)(end - v) : strlen(v);
		if (n >= size) {
			n = size - 1;
		}
		memcpy(out, v, n);
		out[n] = '\0';
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// codec classification
// ---------------------------------------------------------------------------

// The AUDIO group of an enhanced-HLS variant ("audio-stereo-256") or the
// CODECS attribute of a plain one ("mp4a.40.2"). The device plays AAC-LC and
// HE-AAC; ALAC is recognised so a pick can refuse it cleanly rather than
// mistaking it for something else.
static amphls_codec_t classify(const char *value) {
	if (strncmp(value, "audio-HE-stereo", 15) == 0) {
		return AMPHLS_CODEC_HE_AAC;
	}
	if (strncmp(value, "audio-stereo", 12) == 0) {
		return AMPHLS_CODEC_AAC;
	}
	if (strncmp(value, "audio-alac", 10) == 0) {
		return AMPHLS_CODEC_ALAC;
	}
	if (strstr(value, "mp4a.40.2")) {
		return AMPHLS_CODEC_AAC;
	}
	if (strstr(value, "mp4a.40.5")) {
		return AMPHLS_CODEC_HE_AAC;
	}
	return AMPHLS_CODEC_COUNT; // everything else: atmos, ac-3, ec-3, video
}

// ---------------------------------------------------------------------------
// master playlist
// ---------------------------------------------------------------------------

bool amphls_pick_variant(const char *master_text, const char *master_url, amphls_codec_t codec,
						 char *variant_url, size_t variant_size) {
	if (!master_text || !variant_url || variant_size == 0) {
		return false;
	}
	char best[640] = "";
	long best_bw = -1;
	const char *line = master_text;
	while (line && *line) {
		const char *eol = strchr(line, '\n');
		size_t len = eol ? (size_t)(eol - line) : strlen(line);
		char tag[512];
		if (len < sizeof(tag)) {
			memcpy(tag, line, len);
			tag[len] = '\0';
			if (strncmp(tag, "#EXT-X-STREAM-INF", 17) == 0) {
				// The variant URI is the next non-comment line.
				const char *uri_line = eol ? eol + 1 : NULL;
				while (uri_line && *uri_line == '#') {
					const char *next = strchr(uri_line, '\n');
					uri_line = next ? next + 1 : NULL;
				}
				if (uri_line) {
					const char *uri_end = strchr(uri_line, '\n');
					char uri[512];
					size_t uri_len = uri_end ? (size_t)(uri_end - uri_line) : strlen(uri_line);
					while (uri_len && (uri_line[uri_len - 1] == '\r' || uri_line[uri_len - 1] == ' ')) {
						uri_len--;
					}
					if (uri_len < sizeof(uri)) {
						memcpy(uri, uri_line, uri_len);
						uri[uri_len] = '\0';
						char audio[128] = "", codecs[128] = "";
						tag_attribute(tag, "AUDIO", audio, sizeof(audio));
						tag_attribute(tag, "CODECS", codecs, sizeof(codecs));
						long bw = 0;
						{
							char bw_str[32];
							if (tag_attribute(tag, "AVERAGE-BANDWIDTH", bw_str, sizeof(bw_str))) {
								bw = atol(bw_str);
							} else if (tag_attribute(tag, "BANDWIDTH", bw_str, sizeof(bw_str))) {
								bw = atol(bw_str);
							}
						}
						amphls_codec_t got = AMPHLS_CODEC_COUNT;
						if (audio[0]) {
							got = classify(audio);
						} else if (codecs[0]) {
							got = classify(codecs);
						}
						if (got == codec && bw > best_bw) {
							best_bw = bw;
							char absolute[640];
							resolve_url(master_url, uri, absolute, sizeof(absolute));
							snprintf(best, sizeof(best), "%s", absolute);
						}
					}
				}
			}
		}
		line = eol ? eol + 1 : NULL;
	}
	if (!best[0]) {
		return false;
	}
	snprintf(variant_url, variant_size, "%s", best);
	return true;
}

// ---------------------------------------------------------------------------
// variant playlist
// ---------------------------------------------------------------------------

bool amphls_parse_variant(const char *variant_text, const char *variant_url,
						  amphls_playlist_t *out, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	memset(out, 0, sizeof(*out));
	if (!variant_text || !variant_url) {
		return false;
	}
	size_t cap = 16;
	out->segments = malloc(cap * sizeof(*out->segments));
	if (!out->segments) {
		return false;
	}

	int current_key = -1;
	const char *line = variant_text;
	while (line && *line) {
		const char *eol = strchr(line, '\n');
		size_t len = eol ? (size_t)(eol - line) : strlen(line);
		char tag[1024];
		if (len >= sizeof(tag)) {
			line = eol ? eol + 1 : NULL;
			continue;
		}
		memcpy(tag, line, len);
		tag[len] = '\0';
		// Trim the trailing CR; the URI lines below trim their own.
		size_t tlen = strlen(tag);
		while (tlen && (tag[tlen - 1] == '\r' || tag[tlen - 1] == ' ')) {
			tag[--tlen] = '\0';
		}

		if (strncmp(tag, "#EXT-X-KEY", 10) == 0) {
			char method[32] = "";
			tag_attribute(tag, "METHOD", method, sizeof(method));
			char uri[512] = "";
			tag_attribute(tag, "URI", uri, sizeof(uri));
			char keyformat[64] = "";
			tag_attribute(tag, "KEYFORMAT", keyformat, sizeof(keyformat));
			if (strcmp(method, "NONE") == 0) {
				current_key = -1; // a clear stretch: nothing here decrypts to audio
			} else if (uri[0] && (!keyformat[0] || strcmp(keyformat, "com.apple.streamingkeydelivery") == 0)) {
				if (out->key_count < (int)(sizeof(out->keys) / sizeof(out->keys[0]))) {
					snprintf(out->keys[out->key_count].uri, sizeof(out->keys[0].uri), "%s", uri);
					current_key = out->key_count;
					out->key_count++;
				}
			}
		} else if (strncmp(tag, "#EXT-X-MAP", 10) == 0) {
			char uri[512] = "";
			tag_attribute(tag, "URI", uri, sizeof(uri));
			if (uri[0]) {
				resolve_url(variant_url, uri, out->init_url, sizeof(out->init_url));
			}
		} else if (strncmp(tag, "#EXTINF", 7) == 0) {
			double duration = atof(tag + 8 > tag ? tag + 8 : tag);
			// The segment URI is the next non-comment, non-empty line.
			const char *uri_line = eol ? eol + 1 : NULL;
			while (uri_line && *uri_line == '#') {
				const char *next = strchr(uri_line, '\n');
				uri_line = next ? next + 1 : NULL;
			}
			if (uri_line) {
				const char *uri_end = strchr(uri_line, '\n');
				size_t uri_len = uri_end ? (size_t)(uri_end - uri_line) : strlen(uri_line);
				while (uri_len && (uri_line[uri_len - 1] == '\r' || uri_line[uri_len - 1] == ' ')) {
					uri_len--;
				}
				if (uri_len == 0) {
					line = eol ? eol + 1 : NULL;
					continue;
				}
				if (out->count == (int)cap) {
					cap *= 2;
					amphls_segment_t *grown =
						realloc(out->segments, cap * sizeof(*out->segments));
					if (!grown) {
						break;
					}
					out->segments = grown;
				}
				amphls_segment_t *seg = &out->segments[out->count];
				memset(seg, 0, sizeof(*seg));
				char uri[512];
				if (uri_len < sizeof(uri)) {
					memcpy(uri, uri_line, uri_len);
					uri[uri_len] = '\0';
					resolve_url(variant_url, uri, seg->url, sizeof(seg->url));
				}
				seg->duration = duration;
				seg->key_index = current_key;
				out->total_duration += duration > 0 ? duration : 0;
				out->count++;
			}
		}
		line = eol ? eol + 1 : NULL;
	}

	if (out->count == 0) {
		snprintf(error, error_size, "amphls_no_segments");
		amphls_free_playlist(out);
		return false;
	}
	return true;
}

void amphls_free_playlist(amphls_playlist_t *list) {
	if (!list) {
		return;
	}
	free(list->segments);
	list->segments = NULL;
	list->count = 0;
}

const char *amphls_key_for(const amphls_playlist_t *list, int index) {
	if (!list || index < 0 || index >= list->count) {
		return "";
	}
	int key = list->segments[index].key_index;
	if (key < 0 || key >= list->key_count) {
		return "";
	}
	return list->keys[key].uri;
}
