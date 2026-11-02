#ifndef AMPGUEST_H
#define AMPGUEST_H

#include <stdbool.h>
#include <stddef.h>

// The guest tree on the card, fetched by the player itself.
//
// The Apple Music translation layer runs Android binaries through qemu-arm
// from a plain directory tree on the card (tools/assemble_ampguest.sh grows
// one by hand). This module pulls a ready-made one over HTTP and unpacks it:
//
//   1. the archive's size (Content-Length, or the pinned constant) and the
//      unpacked size decide whether the card can hold it at all -- checked
//      before a single byte is written;
//   2. the archive streams to a .part file on the card, sha256'd on the way;
//   3. a hash match unpacks it -- gzip inflate through miniz's tinfl, a
//      512-byte-block tar walk -- over a fresh tree, the old one removed
//      only once the download has proven intact;
//   4. the marker binary's presence says the tree is complete.
//
// Everything blocks, so all of it runs on a worker thread; the GUI polls.

// Where the ready-made tree lives, and exactly what it must hash to. Both
// move together with tools/assemble_ampguest.sh output: bump the URL, the
// size, the hash and the unpacked figure as one. The guards exist so the
// host test can point the fetcher at a tiny fixture of its own.
#ifndef AMPGUEST_URL
#define AMPGUEST_URL "https://github.com/kosuke14/sonix-player-tempotec-v1/releases/download/guest-tree/ampguest-diag.tar.gz"
#define AMPGUEST_SHA256 "d02ebf2f96239bf3931b3c1063060ba08145e369940227b801e1508dd161e5e2"
#define AMPGUEST_ARCHIVE_BYTES 97131762ULL
#define AMPGUEST_UNPACKED_BYTES 210825328ULL
#endif

typedef enum {
	AMPGUEST_IDLE,		 // nothing running, no news
	AMPGUEST_DOWNLOADING, // the archive is coming down
	AMPGUEST_UNPACKING,	 // the tree is being written to the card
	AMPGUEST_DONE,		 // a fetch finished; take_result() hands the news over
	AMPGUEST_FAILED,		 // a fetch failed; take_result() carries the reason
} ampguest_state_t;

// The tree is complete on the card: the loader binary the guest spawn needs
// is where ampmocal looks for it.
bool ampguest_present(void);

bool ampguest_busy(void);

// 0..100 while the archive downloads, -1 the rest of the time.
int ampguest_progress(void);

// Consumes a finished fetch's news: true exactly once per DONE/FAILED. On a
// failure `error` holds the translation key to toast.
bool ampguest_take_result(char *error, size_t error_size);

// Begins a fetch on a worker thread: false when one is already running (or
// the thread could not start, which leaves the state alone).
bool ampguest_fetch_start(void);

#endif // AMPGUEST_H
