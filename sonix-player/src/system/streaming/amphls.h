#ifndef AMPHLS_H
#define AMPHLS_H

#include <stdbool.h>
#include <stddef.h>

// The HLS half of Apple Music playback: the wrapper's dispatch hands over a
// master playlist URL, and everything this player needs afterwards is read out
// of the playlists themselves -- which variant carries the codec the device
// can play, which FairPlay key URI guards which segment, and where the
// fragments live. Parsing only; the fetching and decryption belong to
// ampstream.c.

typedef enum {
	AMPHLS_CODEC_AAC = 0,	 // audio-stereo-<rate>: AAC-LC, the device's default
	AMPHLS_CODEC_HE_AAC,	 // audio-HE-stereo-<rate>: the fallback that fits
	AMPHLS_CODEC_ALAC,		 // audio-alac-...: noted for later, not played yet
	AMPHLS_CODEC_COUNT,
} amphls_codec_t;

// One FairPlay content key, in playlist order. A track's variant may rotate
// keys; every segment carries the index of the key that guards it.
typedef struct {
	char uri[512];
} amphls_key_t;

typedef struct {
	char url[640]; // absolute, resolved against the variant URL
	double duration; // seconds, from EXTINF
	int key_index;	 // index into keys[] at this segment's position
} amphls_segment_t;

typedef struct {
	char init_url[640]; // EXT-X-MAP, the fMP4 init segment; empty when absent
	amphls_key_t keys[8];
	int key_count;
	amphls_segment_t *segments; // malloc'd, count entries
	int count;
	double total_duration; // the sum of the segment durations
} amphls_playlist_t;

// Picks a variant out of a master playlist: the one whose AUDIO group matches
// `codec` (enhanced HLS) or whose CODECS attribute matches it (the plain
// kind), taking the highest bandwidth inside the group -- the top rate of a
// codec, not a surprise one. Writes the absolute variant URL. False when the
// master has no such variant.
bool amphls_pick_variant(const char *master_text, const char *master_url, amphls_codec_t codec,
						 char *variant_url, size_t variant_size);

// Parses a variant playlist into its init segment, its keys and its segments.
// `base_url` resolves relative URIs. The playlist must be freed with
// amphls_free_playlist(). False on a playlist with no segments.
bool amphls_parse_variant(const char *variant_text, const char *variant_url,
						  amphls_playlist_t *out, char *error, size_t error_size);
void amphls_free_playlist(amphls_playlist_t *list);

// The FairPlay key URI guarding segment `index` ("" when none -- the segment
// is then not decryptable and the caller refuses it).
const char *amphls_key_for(const amphls_playlist_t *list, int index);

#endif // AMPHLS_H
