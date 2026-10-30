#ifndef AMPWRAPPER_H
#define AMPWRAPPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The TCP decrypt client for Apple Music playback. The Apple libraries run
// inside the on-device guest (amplocal.c spawns them under qemu-arm); this
// module is the raw transport between the player and the local listener's
// decrypt port: encrypted audio samples go in, plaintext comes out.
//
// It is deliberately thin -- one binary protocol (WV2D) -- with the stream
// assembly (HLS, fMP4, the batching policy) living in amphls.c, ampfp4.c and
// ampstream.c, and the account/playback dispatch living in amppage.c and the
// guest daemon itself.

// One open decrypt connection. Requests and replies are strictly ordered, so
// one session serves one stream at a time; a fresh track opens a fresh one.
typedef struct ampw_decrypt ampw_decrypt_t;

// Connects to the decrypt port. `stale`/`user` are polled between socket
// waits so a canceled stream never hangs the producer thread longer than one
// socket timeout.
ampw_decrypt_t *ampw_decrypt_open(const char *host, int port, bool (*stale)(void *user),
								  void *user);
void ampw_decrypt_close(ampw_decrypt_t *d);

// One round trip. `samples`/`lens` carry `count` runs of ALIGNED ciphertext
// -- the 16-byte-block parts of each sample's protected ranges, prepared by
// the caller (ampfp4.c) -- and the reply carries the same number of
// plaintexts, malloc'd, one buffer per sample, in order. The reassembly of
// clear ranges and tails around them is the caller's.
//
// Any protocol hiccup (bad magic, mismatched id, daemon error frame, closed
// socket) fails the whole session: the daemon discards workers on errors too,
// so the safe move is close + reopen, not retry-on-the-same-wire.
bool ampw_decrypt_batch(ampw_decrypt_t *d, const char *adam_id, const char *skd_uri,
						const uint8_t **samples, const size_t *lens, int count, uint8_t **plains,
						size_t *plain_lens, char *error, size_t error_size);

#endif // AMPWRAPPER_H
