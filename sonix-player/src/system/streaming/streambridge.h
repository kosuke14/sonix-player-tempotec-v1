#ifndef STREAMBRIDGE_H
#define STREAMBRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A local, bounded byte source. One persistent producer owns every socket/TLS
// operation. The decoder copies contiguous RAM spans, at most two at a wrap.
// No temporary file, loopback HTTP server, per-frame allocation or busy loop.
#define STREAMBRIDGE_CAPACITY (1024 * 1024)
#define STREAMBRIDGE_PRECACHE (512 * 1024)
#define STREAMBRIDGE_READ_LOW (64 * 1024)

typedef struct streambridge streambridge_t;
typedef bool (*streambridge_abort_fn)(void);

// A non-HTTP byte source. One implementation fills the ring with whatever
// transformation it likes -- the Apple Music one fetches encrypted HLS,
// decrypts it through the wrapper and re-emits plain ADTS AAC -- while the
// ring, the watermarks and the abort machinery stay exactly the HTTP path's.
// All calls run on the producer thread; every callback may block on network,
// but must stay abortable: check streambridge_source_stale() between network
// steps, and hand sock_cb to any http_stream opened along the way so a cancel
// breaks a recv in progress.
typedef struct streambridge_source streambridge_source_t;

typedef struct {
	// Opens the source (resolve, connect, fetch headers). False, with a
	// message for the GUI, refuses the start. Once per start.
	bool (*open)(streambridge_source_t *src, char *error, size_t error_size);
	// Produces up to `bytes` at out. > 0 bytes produced; 0 = clean end of
	// stream; -1 = fatal, with a message. May block on network.
	long (*read)(streambridge_source_t *src, void *out, size_t bytes, char *error, size_t error_size);
	// Releases everything the source holds. Always called, after open or not.
	void (*close)(streambridge_source_t *src);
} streambridge_source_ops_t;

struct streambridge_source {
	const streambridge_source_ops_t *ops;
	void *user; // the owner's per-start state, handed back at every call
	// True once the start this source serves has been replaced or canceled.
	// Takes the ring lock briefly; call it only outside network calls.
	bool (*stale)(streambridge_source_t *src);
	// Socket observation, the same hook the HTTP path uses: a canceled
	// generation shuts the watched descriptor down out from under a recv.
	// Returns false when the observing generation has gone stale.
	bool (*sock_cb)(void *user, int fd);
	void *sock_user;
	void *impl; // streambridge-internal: generation bookkeeping
};

typedef struct {
	size_t buffered;
	size_t capacity;
	uint64_t received;
	bool opened;
	bool eof;
	bool failed;
	bool canceled;
	bool fetching;
	char error[192];
} streambridge_state_t;

// Power-of-two capacity, at most 2 MiB: wrap is a mask, not a 64-bit divide
// on the MIPS audio thread. Production uses exactly STREAMBRIDGE_CAPACITY.
streambridge_t *streambridge_create(size_t capacity);
void streambridge_destroy(streambridge_t *bridge); // join; teardown/tests only

// Nonblocking, even while the worker is in recv(). A generation counter drops
// all stale writes and wakes readers. Caller must not replace a leased source.
// `prefetch` stops at 512 KiB (or capacity if smaller) until promotion.
bool streambridge_start(streambridge_t *bridge, const char *url, bool prefetch);

// The same machinery behind a source that is not a plain HTTP GET. The ops
// and the user pointer are handed back on the producer thread; the content
// length, when known, is what ends the stream (0: the source's read returns
// 0 at its end instead). Everything else -- quotas, prefetch, abort -- is the
// URL path's. Caller must not replace a leased source.
bool streambridge_start_source(streambridge_t *bridge, const streambridge_source_ops_t *ops,
							   void *user, uint64_t content_length, bool prefetch);
void streambridge_promote(streambridge_t *bridge);
void streambridge_cancel(streambridge_t *bridge);
void streambridge_state(streambridge_t *bridge, streambridge_state_t *out);

// Register once, at audio_init. This aborts on stop/replacement/shutdown, NOT
// pause: returning EOF from inside a codec on pause would poison its state.
void streambridge_set_reader_abort(streambridge_abort_fn fn);

// Initial reserve: 512 KiB or an actually finished short file. Waits on a
// condition variable, never a socket. Abort checks only during starvation.
bool streambridge_prime(streambridge_t *bridge);

// Refill hysteresis before the next decode block: under 64 KiB wait for
// 512 KiB (or EOF), rather than trickling a few bytes per underrun. An optional
// abort includes pause/seek, since no codec is in progress at this boundary.
bool streambridge_wait_decode(streambridge_t *bridge, streambridge_abort_fn abort);

size_t streambridge_read(streambridge_t *bridge, void *out, size_t bytes);
// For native FLAC headers: forward skips, and rewinds only to bytes still in
// the ring. End-relative seeks and arbitrary playback seeks are unsupported.
bool streambridge_seek(streambridge_t *bridge, int64_t offset, bool relative);
uint64_t streambridge_tell(streambridge_t *bridge);
size_t streambridge_peek(streambridge_t *bridge, void *out, size_t bytes);
bool streambridge_failed(streambridge_t *bridge);
void streambridge_fail(streambridge_t *bridge, const char *error);

#endif
