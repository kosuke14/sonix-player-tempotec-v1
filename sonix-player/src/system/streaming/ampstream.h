#ifndef AMPSTREAM_H
#define AMPSTREAM_H

#include <stdbool.h>
#include <stddef.h>

#include "ampapi.h"
#include "streambridge.h"
#include "src/system/library/metadata.h"

// The Apple Music streaming source: the Apple twin of subsonicstream. Tracks
// are named by pseudo-paths -- "amp://<generation>/<index>.aac" -- that name
// slots, not URLs, so the audio side never does I/O by accident; the producer
// behind the bridge fetches the track's HLS, decrypts it through wrapper-v2
// and re-emits plain ADTS AAC into the ring, where the existing ADTS decoder
// plays it. No spool file, a bounded ring, nothing on the card.

#define AMP_PATH_PREFIX "amp://"
#define AMP_PATH_MAX 96
#define AMP_QUEUE_MAX 200

bool ampstream_owns(const char *path);

// Queues a page of catalog songs for playback, the subsonic way: paths and
// pointers come back filled for the player's queue, `selected` is the index
// the user touched. `error` carries a translation tag on refusal.
bool ampstream_queue(const ampapi_song_t *tracks, int count, int start, char paths[][AMP_PATH_MAX],
					 const char **pointers, int *queued, int *selected, char *error, size_t size);

// The device_state prepare callback: pins the playing track's slot. Always
// true: the decoder is allowed to wait for LOCAL bytes at open, and failure
// surfaces through the lease or the GUI error timer, not here.
bool ampstream_prepare(char *path, size_t path_size);
// The 90%-progress prefetch, from the GUI tick.
void ampstream_prefetch(const char *path);
void ampstream_cancel(void);
void ampstream_reset(void); // also drops the queued ids: a new page owns them
bool ampstream_network_wanted(void);

// The decoder's lease on the playing track's bridge.
streambridge_t *ampstream_acquire(const char *path, int *duration, int *bitrate);
void ampstream_release(streambridge_t *bridge);

// Display metadata for the queue entry the path names.
bool ampstream_metadata(const char *path, song_metadata_t *out);

// The queued catalog song a path names, for the track menu's download action.
bool ampstream_song_for(const char *path, ampapi_song_t *out);

// One GUI error, then silence until something new fails.
bool ampstream_take_error(char *out, size_t size);

// Configuration, read from [applemusic] at open time: the storefront the
// catalog is read from. The decrypt port is ampmocal's own fixed 10020.
void ampstream_config(char *storefront, size_t store_size);

#endif // AMPSTREAM_H
