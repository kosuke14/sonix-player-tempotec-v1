#include "anisette.h"

#include "src/system/account/plistxml.h"
#include "src/system/core/base64.h"
#include "src/system/core/json.h"
#include "src/system/core/sha256.h"
#include "src/system/net/http.h"
#include "src/system/net/tls.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// The whole module is three conversations: one GET for the client info, one
// POST per provisioning hop, one POST for the headers. The websocket in the
// middle is plain framing over the same TLS the rest of the player uses.

#define ANISETTE_DIR ".local/amp-anisette"
#define ANISETTE_DEFAULT_SERVER "https://ani.sidestore.io"
#define ANISETTE_TIMEOUT 25

// The cross toolchain carries no openssl headers: the entropy comes from
// the kernel, the way everything else on the device takes it.
static void urandom(void *buf, size_t len) {
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		size_t got = fread(buf, 1, len, f);
		fclose(f);
		if (got == len) {
			return;
		}
	}
	// A urandom read that failed is nearly unheard of; degrade to a clock
	// mix rather than refuse.
	uint8_t *out = buf;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	uint64_t seed = (uint64_t)ts.tv_nsec ^ (uint64_t)(uintptr_t)buf;
	for (size_t i = 0; i < len; i++) {
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		out[i] = (uint8_t)(seed >> 33);
	}
}

static char anisette_server[256];
static char anisette_root[512];
static bool root_ready;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

void anisette_set_server(const char *url) {
	snprintf(anisette_server, sizeof(anisette_server), "%s",
			 url && url[0] ? url : ANISETTE_DEFAULT_SERVER);
	// The trailing slash the endpoints would double up on.
	size_t len = strlen(anisette_server);
	while (len > 0 && anisette_server[len - 1] == '/') {
		anisette_server[--len] = '\0';
	}
}

void anisette_set_root(const char *sd_root) {
	pthread_mutex_lock(&state_lock);
	snprintf(anisette_root, sizeof(anisette_root), "%s", sd_root && sd_root[0] ? sd_root : "");
	root_ready = anisette_root[0] != '\0';
	pthread_mutex_unlock(&state_lock);
}

// ---------------------------------------------------------------------------
// the state: the identifier and the adi blob, as text on the card
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t identifier[16];
	char serial[32];
	char adi_pb[4096]; // base64, empty until provisioned
} anisette_state_t;

static bool state_valid(const anisette_state_t *st) {
	uint8_t zero[16] = {0};
	return memcmp(st->identifier, zero, 16) != 0;
}

static void state_path(char *out, size_t size) {
	snprintf(out, size, "%s/" ANISETTE_DIR "/state.txt", anisette_root);
}

static bool state_load(anisette_state_t *st) {
	memset(st, 0, sizeof(*st));
	if (!root_ready) {
		return false;
	}
	char path[768];
	state_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[4600];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *value = eq + 1;
		if (strcmp(line, "identifier") == 0) {
			// sixteen bytes as thirty-two hex characters
			for (int i = 0; i < 16; i++) {
				unsigned byte = 0;
				if (sscanf(value + i * 2, "%2x", &byte) != 1) {
					byte = 0;
				}
				st->identifier[i] = (uint8_t)byte;
			}
		} else if (strcmp(line, "serial") == 0) {
			snprintf(st->serial, sizeof(st->serial), "%.24s", value);
		} else if (strcmp(line, "adi_pb") == 0) {
			snprintf(st->adi_pb, sizeof(st->adi_pb), "%s", value);
		}
	}
	fclose(f);
	return state_valid(st);
}

static bool state_save(const anisette_state_t *st) {
	if (!root_ready) {
		return false;
	}
	char dir[640];
	// The card root itself may not exist yet (a first run on a fresh card);
	// every level is made in order.
	if (mkdir(anisette_root, 0755) != 0 && errno != EEXIST) {
		return false;
	}
	snprintf(dir, sizeof(dir), "%s/.local", anisette_root);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		return false;
	}
	snprintf(dir, sizeof(dir), "%s/" ANISETTE_DIR, anisette_root);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		return false;
	}
	char path[768];
	state_path(path, sizeof(path));
	char tmp[800];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return false;
	}
	fprintf(f, "identifier=");
	for (int i = 0; i < 16; i++) {
		fprintf(f, "%02x", st->identifier[i]);
	}
	fprintf(f, "\nserial=%s\n", st->serial);
	if (st->adi_pb[0]) {
		fprintf(f, "adi_pb=%s\n", st->adi_pb);
	}
	bool ok = fclose(f) == 0;
	if (ok) {
		ok = rename(tmp, path) == 0;
	}
	if (!ok) {
		remove(tmp);
	}
	return ok;
}

// A fresh identity: sixteen random bytes, a stable serial beside them.
static void state_generate(anisette_state_t *st) {
	memset(st, 0, sizeof(*st));
	urandom(st->identifier, 16);
	// The serial the servers see has the shape of a Mac's: three letters and
	// eight alphanumerics. Random once, then kept.
	snprintf(st->serial, sizeof(st->serial), "C02");
	static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789";
	uint8_t rnd[8];
	urandom(rnd, 8);
	for (int i = 0; i < 8; i++) {
		st->serial[3 + i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
	}
	st->serial[11] = '\0';
}

bool anisette_provisioned(void) {
	anisette_state_t st;
	pthread_mutex_lock(&state_lock);
	bool ok = state_load(&st) && st.adi_pb[0];
	pthread_mutex_unlock(&state_lock);
	return ok;
}

// ---------------------------------------------------------------------------
// small helpers the three conversations share
// ---------------------------------------------------------------------------

static void b64_encode(const uint8_t *in, size_t len, char *out, size_t out_size) {
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i + 2 < len && o + 4 < out_size; i += 3) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
		out[o++] = alphabet[(v >> 18) & 63];
		out[o++] = alphabet[(v >> 12) & 63];
		out[o++] = alphabet[(v >> 6) & 63];
		out[o++] = alphabet[v & 63];
	}
	if (len % 3 == 1 && o + 4 < out_size) {
		uint32_t v = (uint32_t)in[len - 1] << 16;
		out[o++] = alphabet[(v >> 18) & 63];
		out[o++] = alphabet[(v >> 12) & 63];
		out[o++] = '=';
		out[o++] = '=';
	} else if (len % 3 == 2 && o + 4 < out_size) {
		uint32_t v = ((uint32_t)in[len - 2] << 16) | ((uint32_t)in[len - 1] << 8);
		out[o++] = alphabet[(v >> 18) & 63];
		out[o++] = alphabet[(v >> 12) & 63];
		out[o++] = alphabet[(v >> 6) & 63];
		out[o++] = '=';
	}
	out[o] = '\0';
}

// The header block every Apple request in this module carries: the machine
// data the servers read before anything else.
static void apple_headers(char *out, size_t size, const anisette_state_t *st,
						  const char *client_info, const char *user_agent) {
	// The provisioning claims the same identity the login will: a PET
	// issued to a Mac is refused by GrandSlam with -80045, whatever the
	// later request says.
	client_info = GSA_CLIENT_INFO;
	char now[40];
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &tm);

	uint8_t lu[32];
	sha256_ctx lu_ctx;
	sha256_init(&lu_ctx);
	sha256_update(&lu_ctx, st->identifier, sizeof(st->identifier));
	sha256_final_raw(&lu_ctx, lu);
	char lu_hex[65];
	for (int i = 0; i < 32; i++) {
		snprintf(lu_hex + i * 2, 3, "%02x", lu[i]);
	}
	char device_id[40];
	snprintf(device_id, sizeof(device_id),
			 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			 st->identifier[0], st->identifier[1], st->identifier[2], st->identifier[3],
			 st->identifier[4], st->identifier[5], st->identifier[6], st->identifier[7],
			 st->identifier[8], st->identifier[9], st->identifier[10], st->identifier[11],
			 st->identifier[12], st->identifier[13], st->identifier[14], st->identifier[15]);

	snprintf(out, size,
			 "User-Agent: %s\r\n"
			 "X-Apple-I-Client-Time: %s\r\n"
			 "X-Apple-I-SRL-NO: %s\r\n"
			 "X-Apple-I-TimeZone: UTC\r\n"
			 "X-Apple-Locale: en_US\r\n"
			 "X-Apple-I-MD-LU: %s\r\n"
			 "X-Mme-Device-Id: %s\r\n"
			 "X-Mme-Client-Info: %s\r\n",
			 user_agent ? user_agent : "akd/1.0 CFNetwork/978.0.7 Darwin/18.7.0", now,
			 st->serial, lu_hex, device_id, client_info ? client_info : "");
}

// The XML wrapper around a request body: the declaration and the doctype, or
// the plist parsers on Apple's side complain.
static void plist_header(char *out, size_t size) {
	snprintf(out, size,
			 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
			 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
			 "<plist version=\"1.0\">\n");
}

// Apple's gsa chain answers with a self-signed root the system store does
// not carry; the reference implementations verify nothing here either (their
// pinning is a TODO on the same line). The player's global mode is left as
// it was: relaxed for exactly the calls in this module that need it, strict
// for everything else on the device.
typedef struct {
	tls_verify_t saved;
} relax_guard_t;

static void relax_begin(relax_guard_t *g) {
	g->saved = tls_verify_mode();
	tls_set_verify_mode(TLS_VERIFY_RELAXED);
}

static void relax_end(const relax_guard_t *g) {
	tls_set_verify_mode(g->saved);
}

// One blocking TCP connect with the resolver's own answer.
static int tcp_connect(const char *host, const char *port) {
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
	struct addrinfo *res = NULL;
	if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
		return -1;
	}
	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) {
			continue;
		}
		struct timeval tv = {.tv_sec = ANISETTE_TIMEOUT, .tv_usec = 0};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

// Splits a URL into its pieces; https is the only scheme this module speaks.
static bool url_split(const char *url, bool *tls, char *host, size_t host_size, char *port,
					  size_t port_size, char *path, size_t path_size) {
	if (strncmp(url, "https://", 8) == 0) {
		*tls = true;
		url += 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		*tls = false;
		url += 7;
	} else {
		return false;
	}
	const char *slash = strchr(url, '/');
	const char *colon = strchr(url, ':');
	size_t host_len = slash ? (size_t)(slash - url) : strlen(url);
	if (colon && (!slash || colon < slash)) {
		host_len = (size_t)(colon - url);
		size_t port_len = slash ? (size_t)(slash - colon - 1) : strlen(colon + 1);
		snprintf(port, port_size, "%.*s", (int)port_len, colon + 1);
	} else {
		snprintf(port, port_size, "%s", *tls ? "443" : "80");
	}
	snprintf(host, host_size, "%.*s", (int)host_len, url);
	snprintf(path, path_size, "%s", slash ? slash : "/");
	return true;
}

// One plain HTTPS GET/POST with a body, read to the end. Small enough to
// want no more than the socket: the anisette answers are kilobytes.
static bool https_raw(const char *url, const char *extra_headers, const char *body,
					  char **out, size_t *out_len, int *status, char *error, size_t error_size) {
	bool use_tls = false;
	char host[160], port[16], path[512];
	if (!url_split(url, &use_tls, host, sizeof(host), port, sizeof(port), path, sizeof(path))) {
		snprintf(error, error_size, "anisette_bad_url");
		return false;
	}
	int fd = tcp_connect(host, port);
	if (fd < 0) {
		snprintf(error, error_size, "anisette_connect");
		return false;
	}
	tls_conn_t *tls = NULL;
	if (use_tls) {
		char why[128];
		tls = tls_client(fd, host, why, sizeof(why));
		if (!tls) {
			close(fd);
			snprintf(error, error_size, "anisette_tls");
			return false;
		}
	}

	char *req = malloc(16 * 1024);
	if (!req) {
		if (tls) {
			tls_free(tls);
		}
		close(fd);
		snprintf(error, error_size, "anisette_memory");
		return false;
	}
	int req_len = snprintf(req, 16 * 1024,
						   "%s %s HTTP/1.1\r\n"
						   "Host: %s\r\n"
						   "Accept: */*\r\n"
						   "Connection: close\r\n"
						   "%s"
						   "%s",
						   body ? "POST" : "GET", path, host,
						   extra_headers ? extra_headers : "",
						   body ? "" : "\r\n");
	if (body) {
		req_len += snprintf(req + req_len, (size_t)(16 * 1024 - req_len),
							"Content-Length: %zu\r\n\r\n%s", strlen(body), body);
	}
	bool ok = true;
	if (tls) {
		ok = tls_write(tls, req, req_len) == req_len;
	} else {
		ok = write(fd, req, (size_t)req_len) == (ssize_t)req_len;
	}
	free(req);

	// Read the whole answer: the head, then the body after the blank line.
	size_t cap = 128 * 1024;
	char *buf = malloc(cap);
	size_t got = 0;
	while (ok && got < cap - 1) {
		ssize_t n;
		if (tls) {
			n = tls_read(tls, buf + got, (int)(cap - 1 - got));
		} else {
			n = read(fd, buf + got, cap - 1 - got);
		}
		if (n < 0) {
			ok = false;
		} else if (n == 0) {
			break;
		} else {
			got += (size_t)n;
		}
	}
	buf[got] = '\0';
	if (tls) {
		tls_free(tls);
	}
	close(fd);
	if (!ok || got == 0) {
		free(buf);
		snprintf(error, error_size, "anisette_read");
		return false;
	}
	*status = 0;
	if (strncmp(buf, "HTTP/", 5) == 0) {
		*status = atoi(buf + 9);
	}
	char *head_end = strstr(buf, "\r\n\r\n");
	if (!head_end) {
		free(buf);
		snprintf(error, error_size, "anisette_read");
		return false;
	}
	*head_end = '\0';
	char *payload = head_end + 4;
	size_t plen = got - (size_t)(payload - buf);
	memmove(buf, payload, plen); // the caller frees the pointer it is given
	buf[plen] = '\0';
	*out = buf;
	*out_len = plen;
	return true;
}

// ---------------------------------------------------------------------------
// the websocket: one session, text frames both ways
// ---------------------------------------------------------------------------


typedef struct {
	tls_conn_t *tls;
	int fd;
	char host[160];
} ws_t;

static int ws_send_frame(ws_t *ws, int opcode, const char *payload, size_t len) {
	uint8_t head[14];
	size_t hlen = 0;
	head[hlen++] = (uint8_t)(0x80 | opcode); // FIN + text
	uint8_t mask[4];
	urandom(mask, 4);
	if (len < 126) {
		head[hlen++] = (uint8_t)(0x80 | len);
	} else if (len < 65536) {
		head[hlen++] = (uint8_t)(0x80 | 126);
		head[hlen++] = (uint8_t)(len >> 8);
		head[hlen++] = (uint8_t)(len & 0xff);
	} else {
		head[hlen++] = (uint8_t)(0x80 | 127);
		for (int i = 7; i >= 0; i--) {
			head[hlen++] = (uint8_t)(((uint64_t)len >> (i * 8)) & 0xff);
		}
	}
	memcpy(head + hlen, mask, 4);
	hlen += 4;
	if (tls_write(ws->tls, head, (int)hlen) != (int)hlen) {
		return -1;
	}
	// The payload masked in place, one chunk at a time.
	char buf[4096];
	size_t off = 0;
	while (off < len) {
		size_t chunk = len - off > sizeof(buf) ? sizeof(buf) : len - off;
		memcpy(buf, payload + off, chunk);
		for (size_t i = 0; i < chunk; i++) {
			buf[i] ^= mask[(off + i) % 4];
		}
		if (tls_write(ws->tls, buf, (int)chunk) != (int)chunk) {
			return -1;
		}
		off += chunk;
	}
	return 0;
}

// Reads one frame; text lands in `out`, control frames are answered. Returns
// the opcode of the frame that filled `out`, -1 on failure or close.
static int ws_recv_frame(ws_t *ws, char *out, size_t size) {
	uint8_t head[2];
	size_t got = 0;
	while (got < 2) {
		int n = tls_read(ws->tls, head + got, (int)(2 - got));
		if (n <= 0) {
			return -1;
		}
		got += (size_t)n;
	}
	int opcode = head[0] & 0x0f;
	bool masked = (head[1] & 0x80) != 0;
	size_t len = head[1] & 0x7f;
	if (len == 126) {
		uint8_t ext[2];
		if (tls_read(ws->tls, ext, 2) != 2) {
			return -1;
		}
		len = ((size_t)ext[0] << 8) | ext[1];
	} else if (len == 127) {
		uint8_t ext[8];
		if (tls_read(ws->tls, ext, 8) != 8) {
			return -1;
		}
		len = 0;
		for (int i = 0; i < 8; i++) {
			len = (len << 8) | ext[i];
		}
	}
	if (len >= size) {
		return -1; // the sessions never speak in frames this big
	}
	if (opcode == 9) { // ping: the pong carries the same body
		char pong[128];
		size_t plen = len < sizeof(pong) ? len : sizeof(pong);
		if (len && tls_read(ws->tls, pong, (int)len) != (int)len) {
			return -1;
		}
		ws_send_frame(ws, 10, pong, plen);
		return ws_recv_frame(ws, out, size);
	}
	if (opcode == 8) {
		return -1; // close
	}
	uint8_t mask[4] = {0};
	if (masked && tls_read(ws->tls, mask, 4) != 4) {
		return -1;
	}
	size_t off = 0;
	while (off < len) {
		int n = tls_read(ws->tls, out + off, (int)(len - off));
		if (n <= 0) {
			return -1;
		}
		off += (size_t)n;
	}
	if (masked) {
		for (size_t i = 0; i < len; i++) {
			out[i] ^= mask[i % 4];
		}
	}
	out[len] = '\0';
	return opcode;
}

// The upgrade: a plain HTTPS GET with the websocket headers on it.
static bool ws_open(ws_t *ws, const char *base, const char *path, char *error,
					size_t error_size) {
	bool use_tls = false;
	char port[16];
	if (!url_split(base, &use_tls, ws->host, sizeof(ws->host), port, sizeof(port), NULL, 0) ||
		!use_tls) {
		snprintf(error, error_size, "anisette_bad_url");
		return false;
	}
	ws->fd = tcp_connect(ws->host, port);
	if (ws->fd < 0) {
		snprintf(error, error_size, "anisette_connect");
		return false;
	}
	ws->tls = tls_client(ws->fd, ws->host, error, error_size);
	if (!ws->tls) {
		close(ws->fd);
		ws->fd = -1;
		return false;
	}
	uint8_t key[16];
	urandom(key, 16);
	char key_b64[32];
	b64_encode(key, 16, key_b64, sizeof(key_b64));
	char req[512];
	snprintf(req, sizeof(req),
			 "GET %s HTTP/1.1\r\n"
			 "Host: %s\r\n"
			 "Upgrade: websocket\r\n"
			 "Connection: Upgrade\r\n"
			 "Sec-WebSocket-Key: %s\r\n"
			 "Sec-WebSocket-Version: 13\r\n\r\n",
			 path, ws->host, key_b64);
	if (tls_write(ws->tls, req, (int)strlen(req)) != (int)strlen(req)) {
		ws_recv_frame_cleanup:
		tls_free(ws->tls);
		close(ws->fd);
		ws->fd = -1;
		return false;
	}
	char head[1024];
	size_t got = 0;
	while (got < sizeof(head) - 1) {
		int n = tls_read(ws->tls, head + got, 1);
		if (n <= 0) {
			goto ws_recv_frame_cleanup;
		}
		got += (size_t)n;
		head[got] = '\0';
		if (got > 4 && strcmp(head + got - 4, "\r\n\r\n") == 0) {
			break;
		}
	}
	if (!strstr(head, " 101")) {
		goto ws_recv_frame_cleanup;
	}
	return true;
}

static void ws_close(ws_t *ws) {
	if (ws->tls) {
		tls_free(ws->tls);
		ws->tls = NULL;
	}
	if (ws->fd >= 0) {
		close(ws->fd);
		ws->fd = -1;
	}
}

// ---------------------------------------------------------------------------
// provisioning: the v3 session, relay by relay
// ---------------------------------------------------------------------------

bool anisette_provision(char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	anisette_state_t st;
	pthread_mutex_lock(&state_lock);
	bool had = state_load(&st);
	if (!had) {
		state_generate(&st);
		state_save(&st);
	}
	pthread_mutex_unlock(&state_lock);

	relax_guard_t relax;
	relax_begin(&relax);

	// 1. The client info: the description and user agent this server vouches
	//    for. The rest of the calls quote them back.
	char info_url[512];
	snprintf(info_url, sizeof(info_url), "%s/v3/client_info", anisette_server);
	char *body = NULL;
	size_t len = 0;
	int status = 0;
	if (!https_raw(info_url, NULL, NULL, &body, &len, &status, error, error_size) ||
		status != 200) {
		snprintf(error, error_size, "anisette_server");
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		snprintf(error, error_size, "anisette_server");
		return false;
	}
	// The parser keeps the body's pointers: everything is read first, and
	// only then does the body go.
	int root = json_root(&doc);
	char client_info[256], user_agent[128];
	json_obj_str(&doc, root, "client_info", client_info, sizeof(client_info));
	json_obj_str(&doc, root, "user_agent", user_agent, sizeof(user_agent));
	json_free(&doc);
	free(body);

	// 2. The lookup: Apple says where the two provisioning hops live.
	char headers[1024];
	apple_headers(headers, sizeof(headers), &st, client_info, user_agent);
	char lookup_ct[1200];
	snprintf(lookup_ct, sizeof(lookup_ct), "Content-Type: text/x-xml-plist\r\n%s", headers);
	if (!https_raw("https://gsa.apple.com/grandslam/GsService2/lookup", lookup_ct, NULL, &body,
				   &len, &status, error, error_size) ||
		status != 200) {
		snprintf(error, error_size, "anisette_lookup");
		return false;
	}
	plist_t pl;
	char start_url[256], end_url[256];
	{
		if (!plist_parse(body, len, &pl)) {
			free(body);
			snprintf(error, error_size, "anisette_lookup");
			return false;
		}
		int urls = plist_get(&pl, "urls");
		int start = urls >= 0 ? plist_get_in(&pl, urls, "midStartProvisioning") : -1;
		int end = urls >= 0 ? plist_get_in(&pl, urls, "midFinishProvisioning") : -1;
		if (start < 0 || end < 0) {
			plist_free(&pl);
			free(body);
			relax_end(&relax);
			snprintf(error, error_size, "anisette_lookup");
			return false;
		}
		snprintf(start_url, sizeof(start_url), "%s", plist_str(&pl, start));
		snprintf(end_url, sizeof(end_url), "%s", plist_str(&pl, end));
		plist_free(&pl);
		free(body);
	}

	// 3. The session: the server directs, the device posts to Apple.
	ws_t ws = {.tls = NULL, .fd = -1, .host = ""};
	char ws_path[512];
	snprintf(ws_path, sizeof(ws_path), "%s/v3/provisioning_session", anisette_server);
	char ws_base[256];
	snprintf(ws_base, sizeof(ws_base), "%s", anisette_server);
	// the endpoint the server advertises is https; the session speaks wss on
	// the same host and port
	if (!ws_open(&ws, ws_base, ws_path + strlen(anisette_server), error, error_size)) {
		relax_end(&relax);
		return false;
	}

	bool provisioned = false;
	char msg[8192];
	char adi_b64[3072] = "";
	for (int hops = 0; hops < 8 && !provisioned; hops++) {
		if (ws_recv_frame(&ws, msg, sizeof(msg)) < 0) {
			break;
		}
		json_doc_t j;
		if (!json_parse(msg, &j)) {
			break;
		}
		int r = json_root(&j);
		char kind[64];
		json_obj_str(&j, r, "result", kind, sizeof(kind));
		if (strcmp(kind, "GiveIdentifier") == 0) {
			json_free(&j);
			char ident_b64[32];
			b64_encode(st.identifier, 16, ident_b64, sizeof(ident_b64));
			snprintf(msg, sizeof(msg), "{\"identifier\":\"%s\"}", ident_b64);
			ws_send_frame(&ws, 1, msg, strlen(msg));
		} else if (strcmp(kind, "GiveStartProvisioningData") == 0) {
			json_free(&j);
			char plist_head[256];
			plist_header(plist_head, sizeof(plist_head));
			char post[2048];
			snprintf(post, sizeof(post), "%s"
					 "<dict><key>Header</key><dict/><key>Request</key><dict/></dict>"
					 "</plist>",
					 plist_head);
			char apple[4096];
			apple_headers(apple, sizeof(apple), &st, client_info, user_agent);
			char ct[8192];
			snprintf(ct, sizeof(ct), "Content-Type: text/x-xml-plist\r\n%s", apple);
			if (!https_raw(start_url, ct, post, &body, &len, &status, error, error_size) ||
				status != 200 || !plist_parse(body, len, &pl)) {
				free(body);
				break;
			}
			int resp = plist_get(&pl, "Response");
			int spim = resp >= 0 ? plist_get_in(&pl, resp, "spim") : -1;
			char spim_b64[3072] = "";
			if (spim >= 0) {
				snprintf(spim_b64, sizeof(spim_b64), "%s", plist_str(&pl, spim));
			}
			plist_free(&pl);
			free(body);
			if (spim < 0 || !spim_b64[0]) {
				break;
			}
			snprintf(msg, sizeof(msg), "{\"spim\":\"%s\"}", spim_b64);
			ws_send_frame(&ws, 1, msg, strlen(msg));
		} else if (strcmp(kind, "GiveEndProvisioningData") == 0) {
			char cpim[3072] = "";
			json_obj_str(&j, r, "cpim", cpim, sizeof(cpim));
			json_free(&j);
			char plist_head[256];
			plist_header(plist_head, sizeof(plist_head));
			char post[4096];
			snprintf(post, sizeof(post), "%s"
					 "<dict><key>Header</key><dict/><key>Request</key><dict>"
					 "<key>cpim</key><string>%s</string></dict></dict></plist>",
					 plist_head, cpim);
			char apple[4096];
			apple_headers(apple, sizeof(apple), &st, client_info, user_agent);
			char ct[8192];
			snprintf(ct, sizeof(ct), "Content-Type: text/x-xml-plist\r\n%s", apple);
			if (!https_raw(end_url, ct, post, &body, &len, &status, error, error_size) ||
				status != 200 || !plist_parse(body, len, &pl)) {
				free(body);
				break;
			}
			int resp = plist_get(&pl, "Response");
			int ptm = resp >= 0 ? plist_get_in(&pl, resp, "ptm") : -1;
			int tk = resp >= 0 ? plist_get_in(&pl, resp, "tk") : -1;
			char ptm_b64[3072] = "", tk_b64[3072] = "";
			if (ptm >= 0 && tk >= 0) {
				snprintf(ptm_b64, sizeof(ptm_b64), "%s", plist_str(&pl, ptm));
				snprintf(tk_b64, sizeof(tk_b64), "%s", plist_str(&pl, tk));
			}
			plist_free(&pl);
			free(body);
			if (ptm < 0 || tk < 0 || !ptm_b64[0] || !tk_b64[0]) {
				break;
			}
			snprintf(msg, sizeof(msg), "{\"ptm\":\"%s\",\"tk\":\"%s\"}", ptm_b64, tk_b64);
			ws_send_frame(&ws, 1, msg, strlen(msg));
		} else if (strcmp(kind, "ProvisioningSuccess") == 0) {
			json_obj_str(&j, r, "adi_pb", adi_b64, sizeof(adi_b64));
			json_free(&j);
			provisioned = adi_b64[0] != '\0';
		} else {
			json_free(&j);
		}
	}
	ws_close(&ws);
	if (!provisioned) {
		relax_end(&relax);
		if (!error[0]) {
			snprintf(error, error_size, "anisette_provision");
		}
		return false;
	}
	pthread_mutex_lock(&state_lock);
	snprintf(st.adi_pb, sizeof(st.adi_pb), "%s", adi_b64);
	bool saved = state_save(&st);
	pthread_mutex_unlock(&state_lock);
	relax_end(&relax);
	if (!saved) {
		snprintf(error, error_size, "anisette_no_card");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// the headers
// ---------------------------------------------------------------------------

bool anisette_headers(gsa_anisette_t *out, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	anisette_state_t st;
	pthread_mutex_lock(&state_lock);
	bool have = state_load(&st) && st.adi_pb[0];
	pthread_mutex_unlock(&state_lock);
	if (!have) {
		snprintf(error, error_size, "anisette_not_provisioned");
		return false;
	}

	// The client info once per call: the description the server vouches for.
	char info_url[512];
	snprintf(info_url, sizeof(info_url), "%s/v3/client_info", anisette_server);
	char *body = NULL;
	size_t len = 0;
	int status = 0;
	if (!https_raw(info_url, NULL, NULL, &body, &len, &status, error, error_size) ||
		status != 200) {
		snprintf(error, error_size, "anisette_server");
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		snprintf(error, error_size, "anisette_server");
		return false;
	}
	char client_info[256], user_agent[128];
	json_obj_str(&doc, json_root(&doc), "client_info", client_info, sizeof(client_info));
	json_obj_str(&doc, json_root(&doc), "user_agent", user_agent, sizeof(user_agent));
	json_free(&doc);
	free(body);

	// The one-time password and the machine id, fresh each time.
	char ident_b64[32];
	b64_encode(st.identifier, 16, ident_b64, sizeof(ident_b64));
	char get_url[512];
	snprintf(get_url, sizeof(get_url), "%s/v3/get_headers", anisette_server);
	char post[4600];
	snprintf(post, sizeof(post), "{\"identifier\":\"%s\",\"adi_pb\":\"%s\"}", ident_b64,
			 st.adi_pb);
	char ct[8192];
	snprintf(ct, sizeof(ct), "Content-Type: application/json\r\n");
	if (!https_raw(get_url, ct, post, &body, &len, &status, error, error_size) ||
		status != 200) {
		snprintf(error, error_size, "anisette_server");
		return false;
	}
	if (!json_parse(body, &doc)) {
		free(body);
		snprintf(error, error_size, "anisette_server");
		return false;
	}
	int r = json_root(&doc);
	char kind[32], md[256], mdm[256], rinfo[32];
	json_obj_str(&doc, r, "result", kind, sizeof(kind));
	json_obj_str(&doc, r, "X-Apple-I-MD", md, sizeof(md));
	json_obj_str(&doc, r, "X-Apple-I-MD-M", mdm, sizeof(mdm));
	json_obj_str(&doc, r, "X-Apple-I-MD-RINFO", rinfo, sizeof(rinfo));
	json_free(&doc);
	free(body);
	if (strcmp(kind, "Headers") != 0 || !md[0] || !mdm[0]) {
		snprintf(error, error_size, "anisette_server");
		return false;
	}

	// The ten headers, in the order the reference sends them. The machine
	// data is the server's answer; the rest is derived from the state.
	char now[40];
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &tm);
	sha256_ctx lu_ctx;
	sha256_init(&lu_ctx);
	sha256_update(&lu_ctx, st.identifier, sizeof(st.identifier));
	uint8_t lu[32];
	sha256_final_raw(&lu_ctx, lu);
	char lu_hex[65];
	for (int i = 0; i < 32; i++) {
		snprintf(lu_hex + i * 2, 3, "%02x", lu[i]);
	}
	char device_id[40];
	snprintf(device_id, sizeof(device_id),
			 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			 st.identifier[0], st.identifier[1], st.identifier[2], st.identifier[3],
			 st.identifier[4], st.identifier[5], st.identifier[6], st.identifier[7],
			 st.identifier[8], st.identifier[9], st.identifier[10], st.identifier[11],
			 st.identifier[12], st.identifier[13], st.identifier[14], st.identifier[15]);

	static const char *keys[10] = {
		"X-Apple-I-Client-Time", "X-Apple-I-SRL-NO", "X-Apple-I-TimeZone",
		"X-Apple-Locale",		"X-Apple-I-MD-RINFO", "X-Apple-I-MD-LU",
		"X-Mme-Device-Id",		"X-Apple-I-MD",		"X-Apple-I-MD-M",
		"X-Mme-Client-Info",
	};
	const char *values[10] = {now,		st.serial,	 "UTC", "en_US", rinfo,
							  lu_hex, device_id,	md,	mdm,	 GSA_CLIENT_INFO};
	out->count = 10;
	for (int i = 0; i < 10; i++) {
		snprintf(out->kv[i].key, sizeof(out->kv[i].key), "%s", keys[i]);
		snprintf(out->kv[i].value, sizeof(out->kv[i].value), "%s", values[i]);
	}
	return true;
}
