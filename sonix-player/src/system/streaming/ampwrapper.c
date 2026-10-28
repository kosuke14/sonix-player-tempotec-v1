#include "ampwrapper.h"

#include "src/system/net/http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/json.h"

// ---------------------------------------------------------------------------
// shared bits
// ---------------------------------------------------------------------------

// The daemon's HTTP answers are small; the playback dispatch for one track is
// the big one, and a few dozen flavors of metadata stay well under this.
#define AMP_HTTP_LIMIT (1024 * 1024)
#define AMP_HTTP_TIMEOUT 20

static void base_url_of(const char *base_url, char *out, size_t size) {
	// Accept with or without a scheme and with or without a trailing slash.
	const char *p = base_url ? base_url : "";
	while (*p == ' ') {
		p++;
	}
	if (strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0) {
		snprintf(out, size, "%s", p);
	} else {
		snprintf(out, size, "http://%s", p);
	}
	size_t len = strlen(out);
	while (len > 0 && out[len - 1] == '/') {
		out[--len] = '\0';
	}
}

// ---------------------------------------------------------------------------
// the HTTP side
// ---------------------------------------------------------------------------

static bool parse_me(const char *json, ampw_me_t *out, char *error, size_t error_size) {
	memset(out, 0, sizeof(*out));
	json_doc_t doc;
	if (!json_parse(json, &doc)) {
		snprintf(error, error_size, "ampw_bad_me_json");
		return false;
	}
	int root = json_root(&doc);
	json_obj_str(&doc, root, "version", out->version, sizeof(out->version));
	int auth = json_get(&doc, root, "auth");
	if (auth >= 0) {
		json_obj_str(&doc, auth, "state", out->state, sizeof(out->state));
		json_obj_str(&doc, auth, "dev_token", out->dev_token, sizeof(out->dev_token));
		json_obj_str(&doc, auth, "music_user_token", out->music_user_token,
					 sizeof(out->music_user_token));
		json_obj_str(&doc, auth, "apple_id", out->apple_id, sizeof(out->apple_id));
		out->authenticated = strcmp(out->state, "logged_out") != 0 && out->state[0] != '\0';
	}
	json_free(&doc);
	return true;
}

bool ampw_me(const char *base_url, ampw_me_t *out, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!out) {
		return false;
	}
	char base[256];
	base_url_of(base_url, base, sizeof(base));
	char url[288];
	snprintf(url, sizeof(url), "%s/me", base);

	char *body = NULL;
	size_t len = 0;
	int status = 0;
	http_req_t req = {.want_error_body = true, .allow_empty_body = true, .no_https_downgrade = true};
	if (!http_request(url, &req, &body, &len, AMP_HTTP_LIMIT, AMP_HTTP_TIMEOUT, &status)) {
		snprintf(error, error_size, "%s", http_last_error() ? http_last_error() : "ampw_unreachable");
		return false;
	}
	bool ok = parse_me(body, out, error, error_size);
	free(body);
	return ok;
}

bool ampw_playback(const char *base_url, const char *adam_id, char **body, size_t *body_len,
				   char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!body || !adam_id || !adam_id[0]) {
		return false;
	}
	char base[256];
	base_url_of(base_url, base, sizeof(base));
	char id[64];
	http_url_encode(adam_id, id, sizeof(id));
	char url[384];
	snprintf(url, sizeof(url), "%s/playback?adam_id=%s", base, id);

	int status = 0;
	http_req_t req = {.want_error_body = true, .allow_empty_body = true, .no_https_downgrade = true};
	if (!http_request(url, &req, body, body_len, AMP_HTTP_LIMIT, AMP_HTTP_TIMEOUT, &status)) {
		snprintf(error, error_size, "%s", http_last_error() ? http_last_error() : "ampw_playback_failed");
		return false;
	}
	// 401: the daemon's session lapsed; 502: Apple refused; 503: worker busy.
	if (status != 200) {
		snprintf(error, error_size, "ampw_playback_http_%d", status);
		free(*body);
		*body = NULL;
		return false;
	}
	return true;
}

static ampw_login_result_t post_json(const char *base_url, const char *path, const char *json_body,
									 char *error, size_t error_size) {
	char base[256];
	base_url_of(base_url, base, sizeof(base));
	char url[288];
	snprintf(url, sizeof(url), "%s%s", base, path);

	char *body = NULL;
	size_t len = 0;
	int status = 0;
	http_req_t req = {
		.method = "POST",
		.body = json_body,
		.content_type = "application/json",
		.want_error_body = true,
		.allow_empty_body = true, // a 202 (2FA) and a 200 answer nothing
		.no_https_downgrade = true,
	};
	if (!http_request(url, &req, &body, &len, AMP_HTTP_LIMIT, AMP_HTTP_TIMEOUT, &status)) {
		snprintf(error, error_size, "%s", http_last_error() ? http_last_error() : "ampw_unreachable");
		return AMPW_LOGIN_FAILED;
	}
	free(body);
	if (status == 200) {
		return AMPW_LOGIN_OK;
	}
	if (status == 202) {
		return AMPW_LOGIN_2FA;
	}
	snprintf(error, error_size, "ampw_login_http_%d", status);
	return AMPW_LOGIN_FAILED;
}

ampw_login_result_t ampw_login(const char *base_url, const char *username, const char *password,
							   char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	char user[256] = "", pass[256] = "";
	// The body is hand-built JSON: both fields are account strings the daemon
	// feeds to a credential dialog, and escaping the two quotables keeps the
	// document honest without carrying a JSON writer here.
	size_t w = 0;
	for (const char *p = username ? username : ""; *p && w + 8 < sizeof(user); p++) {
		w += (size_t)snprintf(user + w, sizeof(user) - w, *p == '"' || *p == '\\' ? "\\%c" : "%c", *p);
	}
	w = 0;
	for (const char *p = password ? password : ""; *p && w + 8 < sizeof(pass); p++) {
		w += (size_t)snprintf(pass + w, sizeof(pass) - w, *p == '"' || *p == '\\' ? "\\%c" : "%c", *p);
	}
	char body[640];
	snprintf(body, sizeof(body), "{\"username\":\"%s\",\"password\":\"%s\"}", user, pass);
	return post_json(base_url, "/login", body, error, error_size);
}

ampw_login_result_t ampw_login_2fa(const char *base_url, const char *code, char *error,
								   size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	char body[128];
	snprintf(body, sizeof(body), "{\"code\":\"%.32s\"}", code ? code : "");
	return post_json(base_url, "/login/2fa", body, error, error_size);
}

bool ampw_logout(const char *base_url, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	char base[256];
	base_url_of(base_url, base, sizeof(base));
	char url[288];
	snprintf(url, sizeof(url), "%s/login", base);

	char *body = NULL;
	size_t len = 0;
	int status = 0;
	http_req_t req = {
		.method = "DELETE",
		.want_error_body = true,
		.allow_empty_body = true, // the answer is a bare status
		.no_https_downgrade = true,
	};
	if (!http_request(url, &req, &body, &len, AMP_HTTP_LIMIT, AMP_HTTP_TIMEOUT, &status)) {
		snprintf(error, error_size, "%s", http_last_error() ? http_last_error() : "ampw_unreachable");
		return false;
	}
	free(body);
	if (status >= 200 && status < 300) {
		return true;
	}
	snprintf(error, error_size, "ampw_logout_http_%d", status);
	return false;
}

// ---------------------------------------------------------------------------
// the TCP decrypt side (WV2D)
// ---------------------------------------------------------------------------

#define WV2D_MAGIC 0x57563244u // "WV2D"
#define WV2D_VERSION 1
#define WV2D_KIND_BATCH 1
#define WV2D_KIND_OK 2
#define WV2D_KIND_ERROR 3
#define WV2D_KIND_CLOSE 9

#define WV2D_SOCKET_TIMEOUT 15	// seconds per blocking wait
#define WV2D_DEADLINE_SECS 120	// the whole batch, stalls included

struct ampw_decrypt {
	int fd;
	bool dead;
	uint32_t next_request_id;
	bool (*stale)(void *user);
	void *user;
};

static struct timespec deadline_after(int secs) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec += secs;
	return t;
}

static bool expired(struct timespec t) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec > t.tv_sec || (now.tv_sec == t.tv_sec && now.tv_nsec >= t.tv_nsec);
}

static void set_sock_timeouts(int fd) {
	struct timeval tv = {.tv_sec = WV2D_SOCKET_TIMEOUT, .tv_usec = 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

ampw_decrypt_t *ampw_decrypt_open(const char *host, int port, bool (*stale)(void *user),
								  void *user) {
	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", port);
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res = NULL;
	if (getaddrinfo(host ? host : "127.0.0.1", port_str, &hints, &res) != 0 || !res) {
		return NULL;
	}
	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) {
			continue;
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		return NULL;
	}
	set_sock_timeouts(fd);
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	ampw_decrypt_t *d = calloc(1, sizeof(*d));
	if (!d) {
		close(fd);
		return NULL;
	}
	d->fd = fd;
	d->next_request_id = 1;
	d->stale = stale;
	d->user = user;
	return d;
}

void ampw_decrypt_close(ampw_decrypt_t *d) {
	if (!d) {
		return;
	}
	if (d->fd >= 0) {
		// The close frame is a courtesy; the daemon also discards workers on
		// its own schedule. Never wait for a reply here.
		uint8_t frame[16] = {
			(uint8_t)(WV2D_MAGIC >> 24), (uint8_t)(WV2D_MAGIC >> 16), (uint8_t)(WV2D_MAGIC >> 8),
			(uint8_t)WV2D_MAGIC,		   0,							  1,
			0,							   (uint8_t)WV2D_KIND_CLOSE,	  0,
			0,							   0,							  0,
			0,							   0,							  0,
			0,
		};
		(void)!write(d->fd, frame, sizeof(frame));
		close(d->fd);
	}
	free(d);
}

// A blocking write that outlives socket timeouts while the session is wanted.
static bool write_all(ampw_decrypt_t *d, const uint8_t *buf, size_t len, char *error,
					  size_t error_size) {
	size_t done = 0;
	struct timespec deadline = deadline_after(WV2D_DEADLINE_SECS);
	while (done < len) {
		ssize_t w = write(d->fd, buf + done, len - done);
		if (w > 0) {
			done += (size_t)w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			if (expired(deadline) || (d->stale && d->stale(d->user))) {
				snprintf(error, error_size, "ampw_decrypt_write_stalled");
				return false;
			}
			continue;
		}
		snprintf(error, error_size, "ampw_decrypt_write_failed");
		return false;
	}
	return true;
}

// The read half of the same bargain: one recv per socket timeout, retried
// until the frame is whole, the deadline passes or the stream is stale.
static bool read_exact(ampw_decrypt_t *d, uint8_t *buf, size_t len, char *error, size_t error_size) {
	size_t done = 0;
	struct timespec deadline = deadline_after(WV2D_DEADLINE_SECS);
	while (done < len) {
		ssize_t r = read(d->fd, buf + done, len - done);
		if (r > 0) {
			done += (size_t)r;
			continue;
		}
		if (r == 0) {
			snprintf(error, error_size, "ampw_decrypt_closed");
			return false;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			if (expired(deadline) || (d->stale && d->stale(d->user))) {
				snprintf(error, error_size, "ampw_decrypt_read_stalled");
				return false;
			}
			continue;
		}
		snprintf(error, error_size, "ampw_decrypt_read_failed");
		return false;
	}
	return true;
}

// Every frame, in or out, is the same 16-byte envelope.
static bool read_frame(ampw_decrypt_t *d, uint16_t *kind, uint32_t *request_id, uint8_t **payload,
					   size_t *payload_len, char *error, size_t error_size) {
	uint8_t head[16];
	if (!read_exact(d, head, sizeof(head), error, error_size)) {
		return false;
	}
	uint32_t magic = ((uint32_t)head[0] << 24) | ((uint32_t)head[1] << 16) | ((uint32_t)head[2] << 8) |
					 head[3];
	uint16_t version = (uint16_t)((head[4] << 8) | head[5]);
	if (magic != WV2D_MAGIC || version != WV2D_VERSION) {
		snprintf(error, error_size, "ampw_decrypt_bad_frame");
		return false;
	}
	*kind = (uint16_t)((head[6] << 8) | head[7]);
	*request_id = ((uint32_t)head[8] << 24) | ((uint32_t)head[9] << 16) | ((uint32_t)head[10] << 8) |
				  head[11];
	size_t len = ((size_t)head[12] << 24) | ((size_t)head[13] << 16) | ((size_t)head[14] << 8) |
				 head[15];
	*payload_len = len;
	*payload = NULL;
	if (len > 0) {
		*payload = malloc(len);
		if (!*payload) {
			snprintf(error, error_size, "ampw_out_of_memory");
			return false;
		}
		if (!read_exact(d, *payload, len, error, error_size)) {
			free(*payload);
			*payload = NULL;
			return false;
		}
	}
	return true;
}

bool ampw_decrypt_batch(ampw_decrypt_t *d, const char *adam_id, const char *skd_uri,
						const uint8_t **samples, const size_t *lens, int count, uint8_t **plains,
						size_t *plain_lens, char *error, size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!d || d->dead || d->fd < 0 || !adam_id || !skd_uri || count <= 0) {
		snprintf(error, error_size, "ampw_decrypt_bad_args");
		return false;
	}
	size_t adam_len = strlen(adam_id), skd_len = strlen(skd_uri);
	if (!adam_len || adam_len > 0xffff || !skd_len || skd_len > 0xffff) {
		snprintf(error, error_size, "ampw_decrypt_bad_labels");
		return false;
	}

	// Payload: label lengths, the sample table, then the labels and the
	// concatenated ciphertext -- one write, so a half-sent batch is a socket
	// question, never an application-protocol one.
	size_t total = 0;
	for (int i = 0; i < count; i++) {
		if (!samples[i] || lens[i] == 0 || lens[i] > 0xffffffffu) {
			snprintf(error, error_size, "ampw_decrypt_bad_sample");
			return false;
		}
		total += lens[i];
	}
	size_t payload_len = 2 + 2 + 4 + (size_t)count * 4 + adam_len + skd_len + total;
	uint32_t request_id = d->next_request_id;
	d->next_request_id = d->next_request_id == 0xffffffffu ? 1 : d->next_request_id + 1;

	uint8_t *payload = malloc(payload_len);
	uint8_t *frame = malloc(16 + payload_len);
	if (!payload || !frame) {
		free(payload);
		free(frame);
		snprintf(error, error_size, "ampw_out_of_memory");
		return false;
	}
	uint8_t *w = payload;
	*w++ = (uint8_t)(adam_len >> 8);
	*w++ = (uint8_t)adam_len;
	*w++ = (uint8_t)(skd_len >> 8);
	*w++ = (uint8_t)skd_len;
	*w++ = (uint8_t)((uint32_t)count >> 24);
	*w++ = (uint8_t)((uint32_t)count >> 16);
	*w++ = (uint8_t)((uint32_t)count >> 8);
	*w++ = (uint8_t)(uint32_t)count;
	for (int i = 0; i < count; i++) {
		size_t n = lens[i];
		*w++ = (uint8_t)(n >> 24);
		*w++ = (uint8_t)(n >> 16);
		*w++ = (uint8_t)(n >> 8);
		*w++ = (uint8_t)n;
	}
	memcpy(w, adam_id, adam_len);
	w += adam_len;
	memcpy(w, skd_uri, skd_len);
	w += skd_len;
	for (int i = 0; i < count; i++) {
		memcpy(w, samples[i], lens[i]);
		w += lens[i];
	}

	uint8_t *f = frame;
	*f++ = (uint8_t)(WV2D_MAGIC >> 24);
	*f++ = (uint8_t)(WV2D_MAGIC >> 16);
	*f++ = (uint8_t)(WV2D_MAGIC >> 8);
	*f++ = (uint8_t)WV2D_MAGIC;
	*f++ = 0;
	*f++ = WV2D_VERSION;
	*f++ = 0;
	*f++ = WV2D_KIND_BATCH;
	*f++ = (uint8_t)(request_id >> 24);
	*f++ = (uint8_t)(request_id >> 16);
	*f++ = (uint8_t)(request_id >> 8);
	*f++ = (uint8_t)request_id;
	*f++ = (uint8_t)(payload_len >> 24);
	*f++ = (uint8_t)(payload_len >> 16);
	*f++ = (uint8_t)(payload_len >> 8);
	*f++ = (uint8_t)payload_len;
	memcpy(f, payload, payload_len);
	free(payload);

	if (!write_all(d, frame, 16 + payload_len, error, error_size)) {
		free(frame);
		d->dead = true;
		return false;
	}
	memset(frame, 0, 16 + payload_len); // the ciphertext does not linger on the heap
	free(frame);

	uint16_t kind = 0;
	uint32_t reply_id = 0;
	uint8_t *reply = NULL;
	size_t reply_len = 0;
	if (!read_frame(d, &kind, &reply_id, &reply, &reply_len, error, error_size)) {
		d->dead = true;
		return false;
	}
	if (reply_id != request_id) {
		free(reply);
		snprintf(error, error_size, "ampw_decrypt_id_mismatch");
		d->dead = true;
		return false;
	}
	if (kind == WV2D_KIND_ERROR) {
		size_t n = reply_len < 160 ? reply_len : 160;
		snprintf(error, error_size, "ampw_decrypt_refused: %.*s", (int)n,
				 reply ? (const char *)reply : "");
		free(reply);
		d->dead = true;
		return false;
	}
	if (kind != WV2D_KIND_OK) {
		free(reply);
		snprintf(error, error_size, "ampw_decrypt_bad_kind");
		d->dead = true;
		return false;
	}

	// The reply: one length table, then the plaintexts back to back.
	bool ok = false;
	memset(plains, 0, sizeof(*plains) * (size_t)count);
	memset(plain_lens, 0, sizeof(*plain_lens) * (size_t)count);
	do {
		if (reply_len < 4) {
			snprintf(error, error_size, "ampw_decrypt_short_reply");
			break;
		}
		uint32_t n = ((uint32_t)reply[0] << 24) | ((uint32_t)reply[1] << 16) |
					 ((uint32_t)reply[2] << 8) | reply[3];
		if (n != (uint32_t)count) {
			snprintf(error, error_size, "ampw_decrypt_count_mismatch");
			break;
		}
		if (4 + (size_t)n * 4 > reply_len) {
			snprintf(error, error_size, "ampw_decrypt_truncated_table");
			break;
		}
		const uint8_t *table = reply + 4;
		const uint8_t *data = table + (size_t)n * 4;
		size_t avail = reply_len - 4 - (size_t)n * 4;
		for (int i = 0; i < count; i++) {
			size_t len = ((size_t)table[i * 4] << 24) | ((size_t)table[i * 4 + 1] << 16) |
						 ((size_t)table[i * 4 + 2] << 8) | table[i * 4 + 3];
			if (len > avail) {
				snprintf(error, error_size, "ampw_decrypt_truncated_plaintext");
				ok = false;
				break;
			}
			plains[i] = malloc(len ? len : 1);
			if (!plains[i]) {
				snprintf(error, error_size, "ampw_out_of_memory");
				ok = false;
				break;
			}
			memcpy(plains[i], data, len);
			plain_lens[i] = len;
			data += len;
			avail -= len;
			ok = true;
		}
		if (!ok) {
			break;
		}
		ok = true;
	} while (0);
	free(reply);
	if (!ok) {
		for (int i = 0; i < count; i++) {
			free(plains[i]);
			plains[i] = NULL;
			plain_lens[i] = 0;
		}
		d->dead = true;
		return false;
	}
	return true;
}
