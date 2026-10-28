#ifndef AMPAPI_H
#define AMPAPI_H

#include <stdbool.h>
#include <stddef.h>

// The Apple Music catalog, reached directly (https://amp-api.music.apple.com)
// with the tokens the wrapper's /me carries. Search and metadata only -- the
// audio always comes through the wrapper, and nothing here can play anything.

typedef struct {
	char id[32];	   // the Adam ID every playback call is keyed by
	char title[256];
	char artist[256];
	char album[256];
	char artwork[512]; // a JPEG/PNG URL template; {w}x{h} in the middle
	int duration_ms;
} ampapi_song_t;

// Searches the catalog for songs. `token` is the developer (JWT) token,
// `music_user_token` the per-account one, both from ampw_me(). Results go to
// `out` (at most `max`); the count is returned, -1 on transport failure and
// -2 on an API error body. `error` carries the message either way.
int ampapi_search_songs(const char *dev_token, const char *music_user_token,
						const char *storefront, const char *term, ampapi_song_t *out, int max,
						char *error, size_t error_size);

// The artwork URL at a concrete size, written into out: the template's {w}
// and {h} are replaced. 300x300 reads well at the player's size.
void ampapi_artwork_url(const char *template, int side, char *out, size_t size);

#endif // AMPAPI_H
