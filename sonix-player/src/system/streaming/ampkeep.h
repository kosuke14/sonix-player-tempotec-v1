#ifndef AMPKEEP_H
#define AMPKEEP_H

#include <stdbool.h>
#include <stddef.h>

#include "ampapi.h"

// Apple Music downloads, the Tidal/Qobuz "keep on card" twin.
//
// An Apple track streams as encrypted HLS: fMP4 segments whose samples are
// FairPlay CBCS-protected. A download therefore cannot be a plain file copy
// like Tidal's -- this module assembles one: the playback dispatch for the
// adam id, the variant playlist, the init segment rewritten to a plain
// (non-encrypted) audio sample entry, and every media segment fetched,
// decrypted through the guest daemon and spliced back in place. What lands
// on the card is
//
//     <card>/Music/Apple Music/<artist>/<album>/<nn> <title>.m4a
//
// with a .tags sidecar and a .jpg cover beside it, so the next scan finds an
// ordinary track with an ordinary title, artist and sleeve. From then on it
// behaves like anything else on the card: no network, no account, playable
// in the file browser at once.

// Downloads one track, on a worker thread. False when another keep is
// already running or the arguments are broken. `notify_result` posts a
// result toast; bulk callers suppress it and report once.
bool ampkeep_keep(const ampapi_song_t *song, bool notify_result);

// Whether a keep is running. One at a time: they are card writes, and the
// card is also what playback reads from.
bool ampkeep_busy(void);

// Copies the latest start or completion failure into `out`; false means the
// last keep succeeded or there is no saved failure.
bool ampkeep_last_error(char *out, size_t out_size);

#endif /* AMPKEEP_H */
