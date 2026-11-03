#ifndef ANISETTE_H
#define ANISETTE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/account/gsa.h"

// The machine data Apple's sign-in servers judge the request by. The device
// cannot compute it alone -- the one-time password comes out of Apple's own
// provisioning -- so the work is split with an anisette server:
//
//   * the server holds the provisioned identity and answers get_headers with
//     the one-time password and the machine id (X-Apple-I-MD, X-Apple-I-MD-M);
//   * the device carries the rest: its own random identity, the client time,
//     the locale, and the client description the server vouches for.
//
// Provisioning, when the server offers it over its v3 websocket, runs here
// too: Apple's mid-start/mid-finish endpoints are called from the device and
// the opaque blobs relay through the session. What the device keeps on the
// card afterwards is its sixteen-byte identifier and the server's adi blob,
// both meaningless without the server that made them.

// Where the card is: the state file lives under <root>/.local/amp-anisette.
// NULL or empty turns persistence off (headers still work, nothing is kept).
void anisette_set_root(const char *sd_root);

// The anisette server, from [applemusic] anisette_url. Empty keeps the
// default. This is the same shape SideStore's servers speak: v3 client info,
// v3 get_headers, and -- when it has one -- a v3 provisioning session.
void anisette_set_server(const char *url);

// True when the card holds a provisioned identity for the current server.
bool anisette_provisioned(void);

// Runs the v3 provisioning dance against the server: the lookup that finds
// Apple's mid-start and mid-finish addresses, the websocket session that
// relays the opaque blobs, and the adi blob saved to the card. True when the
// card ends up provisioned.
bool anisette_provision(char *error, size_t error_size);

// Assembles the GrandSlam header set into `out`: ten entries, the machine
// data fresh from the server each call (the one-time password is a
// one-time password). Not provisioned is a failure with the message set.
bool anisette_headers(gsa_anisette_t *out, char *error, size_t error_size);

#endif // ANISETTE_H
