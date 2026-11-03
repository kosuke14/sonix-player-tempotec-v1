#ifndef AMPITUNES_H
#define AMPITUNES_H

#include <stdbool.h>
#include <stddef.h>

#include "ampapi.h"

// The guest catalog: the iTunes Search API, reached with no tokens of any
// kind. It answers with the track's name, its artist, its album, its
// artwork and a previewUrl -- thirty seconds of DRM-free AAC in an MP4 box.
// That preview is what the guest mode plays: fetched to the card once, then
// played like any other file. The full-track FairPlay path stays with
// ampstream; this module is what makes the page usable before any sign-in.

// Where the card is. NULL or empty turns the preview cache off (the search
// still works, nothing plays).
void ampitunes_set_root(const char *sd_root);

// The API base, fixed for the device and pointed at a local socket by the
// tests, which serve the answers themselves.
void ampitunes_set_api(const char *base);

// Searches the catalog for songs. Same contract as ampapi_search_songs,
// tokens aside: the count is returned, -1 on transport failure and -2 on a
// bad body. `error` carries the message either way.
int ampitunes_search(const char *term, ampapi_song_t *out, int max, char *error,
					 size_t error_size);

// The tracks of one collection, same contract: the lookup endpoint answers
// with the album first and its songs after, and only the songs are kept.
int ampitunes_album_tracks(const char *album_id, ampapi_song_t *out, int max, char *error,
						   size_t error_size);

// True when the track's preview sits on the card in full; `out` gets its path.
bool ampitunes_ready(const ampapi_song_t *song, char *out, size_t size);

// Fetches the preview to the card if it is not there yet: a small fixed
// file, downloaded in one go -- no background machinery, the caller runs
// this on its own worker. True when the file is complete afterwards.
bool ampitunes_fetch(const ampapi_song_t *song, char *error, size_t error_size);

#endif // AMPITUNES_H
