#ifndef AMPLOCAL_H
#define AMPLOCAL_H

#include <stdbool.h>
#include <stddef.h>

// The on-device decrypt side for Apple Music ([applemusic] translate=1).
//
// When the user has placed the guest tree on the card, this module listens
// on the configured decrypt port (127.0.0.1 only), spawns the translated
// daemon (qemu-i386 -> Android linker -> ampdaemon) behind it, and relays
// the player's WV2D batches to it. The FairPlay SPC/CKC exchange with
// Apple's key server runs here, over the player's own TLS stack -- the
// daemon inside the guest has no network at all.
//
// Everything is opt-in and lazily started: translate=0 (the default) makes
// this module a no-op and the decrypt connection goes to the remote
// wrapper exactly as before.

// Read the [applemusic] settings and open the listener socket when
// translate is on. Returns false without a listener otherwise (never an
// error for the caller: it just means "not translating").
bool amplocal_init(void);

// Called from the GUI tick: respawns the daemon after a crash, enforces the
// idle kill. Cheap; does no I/O unless work is pending.
void amplocal_tick(void);

// Stops the listener and kills the daemon (service switch / shutdown).
void amplocal_shutdown(void);

// True while the guest daemon is spawned and expected to keep running.
bool amplocal_running(void);

#endif // AMPLOCAL_H
