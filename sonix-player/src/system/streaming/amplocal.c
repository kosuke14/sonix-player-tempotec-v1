#include "amplocal.h"

#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/net/http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

// The translated FairPlay decrypt side. One listener thread owns the
// decrypt port and the guest daemon's pipes; the player's existing WV2D
// client (ampwrapper.c) simply points at 127.0.0.1 instead of a remote
// wrapper and nothing else changes.

// The daemon runs two processes deep: qemu-i386 execs the Android linker,
// which loads ampdaemon. Both live inside the guest squashfs the user
// assembled; only qemu and the mount point live on the card root.
//
// Frames (both sides) are the 16/20-byte envelopes ampwrapper.c and the
// daemon speak: WV2D on the outside, WV2I on the inside. Batch payloads
// are byte-identical, so the relay re-encodes only the header. The
// SPC/CKC split is the one real translation: the daemon asks (OpGetSPC),
// we POST to Apple, answer (OpApplyCKC), and the batch replays.

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

typedef struct {
	int listen_fd; // decrypt-port listener, -1 when not translating
	int daemon_stdin; // WV2I pipe to the guest, -1 when not spawned
	int daemon_stdout;
	pid_t daemon_pid;
	char translate_root[300]; // where qemu/ and guest.squashfs live
	int idle_secs; // [applemusic] translate_idle_secs
	long idle_since_ms; // last connection/daemon use
	bool daemon_ok; // OpHealth passed after last spawn
	char last_error[192];
	bool error_reported;
	pthread_t thread;
	bool thread_started;
	pthread_mutex_t lock;
} amp_local_state_t;

static amp_local_state_t s_local = {
	.listen_fd = -1, .daemon_stdin = -1, .daemon_stdout = -1, .daemon_pid = -1};

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void be32_put(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t be32_get(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool read_exact_fd(int fd, void *buf, size_t len) {
	uint8_t *p = buf;
	size_t got = 0;
	while (got < len) {
		ssize_t n = read(fd, p + got, len - got);
		if (n <= 0) {
			return false;
		}
		got += (size_t)n;
	}
	return true;
}

static bool write_exact_fd(int fd, const void *buf, size_t len) {
	const uint8_t *p = buf;
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fd, p + done, len - done);
		if (n <= 0) {
			return false;
		}
		done += (size_t)n;
	}
	return true;
}

// The daemon's one-second watchdog keeps a dead guest from wedging a batch:
// polls instead of blocking forever.
static bool wait_readable(int fd, int timeout_ms) {
	struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
	for (int slept = 0; slept < timeout_ms; slept += 250) {
		int r = poll(&pfd, 1, 250);
		if (r > 0) {
			return true;
		}
		if (r < 0 && errno != EINTR) {
			return false;
		}
		if (waitpid(s_local.daemon_pid, NULL, WNOHANG) != 0) {
			return false; // died while we waited
		}
	}
	return pfd.revents & POLLIN;
}

// ---------------------------------------------------------------------------
// guest daemon lifecycle
// ---------------------------------------------------------------------------

static void daemon_kill_locked(void) {
	if (s_local.daemon_pid > 0) {
		// Politely: the daemon exits when stdin closes. SIGKILL only after
		// a grace beat so the FootHill state gets to flush its db.
		close(s_local.daemon_stdin);
		close(s_local.daemon_stdout);
		kill(s_local.daemon_pid, SIGTERM);
		for (int i = 0; i < 10; i++) {
			if (waitpid(s_local.daemon_pid, NULL, WNOHANG) != 0) {
				break;
			}
			struct timespec t = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
			nanosleep(&t, NULL);
		}
		kill(s_local.daemon_pid, SIGKILL);
		waitpid(s_local.daemon_pid, NULL, WNOHANG);
	}
	s_local.daemon_pid = -1;
	s_local.daemon_stdin = -1;
	s_local.daemon_stdout = -1;
	s_local.daemon_ok = false;
}

// Spawns qemu-i386 -> Android linker -> ampdaemon with stdio pipes. The
// guest tree layout under translate_root:
//   qemu-i386            the statically linked user-mode emulator
//   guest.squashfs       bionic + Apple libs + ampdaemon
//   sys/                 the loop mount point (prepared here)
//   sys/bin/linker       Android's dynamic linker (i386)
//   sys/bin/ampdaemon    our daemon
//   sys/lib/             bionic + Apple arm/i386 libs
static bool daemon_spawn_locked(void) {
	char mount_cmd[768], probe[400];
	snprintf(probe, sizeof(probe), "%s/sys/bin/ampdaemon", s_local.translate_root);
	if (access(probe, X_OK) != 0) {
		snprintf(mount_cmd, sizeof(mount_cmd), "mkdir -p %s/sys && mount -t squashfs -o loop,ro %s/guest.squashfs %s/sys",
				 s_local.translate_root, s_local.translate_root, s_local.translate_root);
		if (system(mount_cmd) != 0) {
			snprintf(s_local.last_error, sizeof(s_local.last_error), "amp_guest_mount_failed");
			return false;
		}
	}

	int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
	if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
		snprintf(s_local.last_error, sizeof(s_local.last_error), "amp_guest_pipe_failed");
		return false;
	}

	char qemu_path[400], linker_path[400], daemon_path[400], base_arg[400];
	snprintf(qemu_path, sizeof(qemu_path), "%s/qemu-i386", s_local.translate_root);
	snprintf(linker_path, sizeof(linker_path), "%s/sys/bin/linker", s_local.translate_root);
	snprintf(daemon_path, sizeof(daemon_path), "%s/sys/bin/ampdaemon", s_local.translate_root);
	snprintf(base_arg, sizeof(base_arg), "AMP_BASE_DIR=/data/amp");

	pid_t pid = fork();
	if (pid < 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		snprintf(s_local.last_error, sizeof(s_local.last_error), "amp_guest_fork_failed");
		return false;
	}
	if (pid == 0) {
		// The child: translated guest. It must lose every OOM race against
		// the player, own no controlling terminal, and die with us.
		setsid();
		FILE *adj = fopen("/proc/self/oom_score_adj", "w");
		if (adj) {
			fprintf(adj, "300");
			fclose(adj);
		}
		dup2(in_pipe[0], STDIN_FILENO);
		dup2(out_pipe[1], STDOUT_FILENO);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2) {
				close(devnull);
			}
		}
		// Wrapper path envs: ANDROID_* live inside the guest image.
		setenv("ANDROID_DNS_MODE", "local", 1);
		setenv("ANDROID_DATA", "/data/amp", 1);
		setenv("ANDROID_ROOT", "/sys/system", 1);
		setenv("AMP_BASE_DIR", "/data/amp", 1);
		execl(qemu_path, "qemu-i386", "-L", s_local.translate_root, linker_path, daemon_path,
			  "/sys/lib", (char *)NULL);
		_exit(127);
	}
	close(in_pipe[0]);
	close(out_pipe[1]);
	s_local.daemon_pid = pid;
	s_local.daemon_stdin = in_pipe[1];
	s_local.daemon_stdout = out_pipe[0];
	s_local.daemon_ok = false;
	return true;
}

// One WV2I round trip to the daemon. Returns the response payload or false
// (daemon dead / bad frame), filling error.
static bool daemon_round_trip(int opcode, const uint8_t *payload, size_t len,
							  uint8_t **resp, size_t *resp_len, bool *ok,
							  char *error, size_t error_size) {
	if (s_local.daemon_pid <= 0) {
		pthread_mutex_lock(&s_local.lock);
		bool spawned = daemon_spawn_locked();
		pthread_mutex_unlock(&s_local.lock);
		if (!spawned) {
			snprintf(error, error_size, "amp_guest_spawn_failed");
			return false;
		}
	}
	if (len > 256u * 1024u * 1024u) {
		snprintf(error, error_size, "amp_frame_too_large");
		return false;
	}
	uint8_t head[20];
	memcpy(head, "WV2I", 4);
	head[4] = 0;
	head[5] = 1; // version
	head[6] = 0;
	head[7] = 1; // kind = request
	be32_put(head + 8, 1); // request id: single-threaded, always 1
	head[12] = (uint8_t)(opcode >> 8);
	head[13] = (uint8_t)opcode;
	head[14] = 0;
	head[15] = 0;
	be32_put(head + 16, (uint32_t)len);

	if (!write_exact_fd(s_local.daemon_stdin, head, sizeof(head)) ||
		(len && !write_exact_fd(s_local.daemon_stdin, payload, len))) {
		snprintf(error, error_size, "amp_guest_write_failed");
		return false;
	}
	if (!wait_readable(s_local.daemon_stdout, 30000)) {
		snprintf(error, error_size, "amp_guest_no_response");
		return false;
	}
	if (!read_exact_fd(s_local.daemon_stdout, head, sizeof(head)) ||
		memcmp(head, "WV2I", 4) != 0) {
		snprintf(error, error_size, "amp_guest_bad_frame");
		return false;
	}
	uint32_t rlen = be32_get(head + 16);
	if (rlen > 256u * 1024u * 1024u) {
		snprintf(error, error_size, "amp_guest_bad_frame");
		return false;
	}
	uint8_t *body = malloc(rlen ? rlen : 1);
	if (!body) {
		snprintf(error, error_size, "amp_out_of_memory");
		return false;
	}
	if (rlen && !read_exact_fd(s_local.daemon_stdout, body, rlen)) {
		free(body);
		snprintf(error, error_size, "amp_guest_truncated");
		return false;
	}
	*ok = head[7] == 2 && rlen > 0 && body[0] == 0;
	*resp = body;
	*resp_len = rlen;
	return true;
}

// ---------------------------------------------------------------------------
// the Apple FPS key exchange (the one thing the guest cannot do alone)
// ---------------------------------------------------------------------------

// daemon payload: adam_len u16 + uri_len u16 + labels. Response payload
// after the ok byte: the SPC bytes.
static bool fps_spc(const char *adam_id, const char *key_uri, uint8_t **spc, size_t *spc_len,
					char *error, size_t error_size) {
	size_t adam_len = strlen(adam_id), uri_len = strlen(key_uri);
	size_t len = 4 + adam_len + uri_len;
	uint8_t *payload = malloc(len);
	if (!payload) {
		snprintf(error, error_size, "amp_out_of_memory");
		return false;
	}
	payload[0] = (uint8_t)(adam_len >> 8);
	payload[1] = (uint8_t)adam_len;
	payload[2] = (uint8_t)(uri_len >> 8);
	payload[3] = (uint8_t)uri_len;
	memcpy(payload + 4, adam_id, adam_len);
	memcpy(payload + 4 + adam_len, key_uri, uri_len);

	uint8_t *resp = NULL;
	size_t resp_len = 0;
	bool ok = false;
	if (!daemon_round_trip(9 /* OpGetSPC */, payload, len, &resp, &resp_len, &ok, error,
						   error_size)) {
		free(payload);
		return false;
	}
	free(payload);
	if (!ok || resp_len < 2) {
		snprintf(error, error_size, "%.*s", (int)(resp_len > 1 ? resp_len - 1 : 0),
				 resp ? (const char *)resp + 1 : "");
		free(resp);
		return false;
	}
	*spc = malloc(resp_len - 1);
	if (!*spc) {
		free(resp);
		snprintf(error, error_size, "amp_out_of_memory");
		return false;
	}
	memcpy(*spc, resp + 1, resp_len - 1);
	*spc_len = resp_len - 1;
	free(resp);
	return true;
}

// daemon payload: adam_len u16 + uri_len u16 + ckc_len u32 + labels + ckc.
static bool fps_apply_ckc(const char *adam_id, const char *key_uri, const uint8_t *ckc,
						  size_t ckc_len, char *error, size_t error_size) {
	size_t adam_len = strlen(adam_id), uri_len = strlen(key_uri);
	size_t len = 8 + adam_len + uri_len + ckc_len;
	uint8_t *payload = malloc(len);
	if (!payload) {
		snprintf(error, error_size, "amp_out_of_memory");
		return false;
	}
	payload[0] = (uint8_t)(adam_len >> 8);
	payload[1] = (uint8_t)adam_len;
	payload[2] = (uint8_t)(uri_len >> 8);
	payload[3] = (uint8_t)uri_len;
	be32_put(payload + 4, (uint32_t)ckc_len);
	memcpy(payload + 8, adam_id, adam_len);
	memcpy(payload + 8 + adam_len, key_uri, uri_len);
	memcpy(payload + 8 + adam_len + uri_len, ckc, ckc_len);

	uint8_t *resp = NULL;
	size_t resp_len = 0;
	bool ok = false;
	if (!daemon_round_trip(10 /* OpApplyCKC */, payload, len, &resp, &resp_len, &ok, error,
						   error_size)) {
		free(payload);
		return false;
	}
	if (!ok) {
		snprintf(error, error_size, "%.*s", (int)(resp_len > 1 ? resp_len - 1 : 0),
				 resp ? (const char *)resp + 1 : "");
	}
	free(resp);
	free(payload);
	return !ok ? false : true;
}

// Posts the SPC to Apple's FPS key server and hands the CKC back. The
// daemon's stderr showed the URL the 3.6.0 build used; 6.5.3's SPC request
// carries the key uri, format and protocol as caller arguments, and the
// endpoint itself stays the same.
static bool fps_exchange(const char *adam_id, const char *key_uri, char *error,
						 size_t error_size) {
	uint8_t *spc = NULL;
	size_t spc_len = 0;
	if (!fps_spc(adam_id, key_uri, &spc, &spc_len, error, error_size)) {
		return false;
	}

	// The FPS endpoint answers a POST of the SPC bytes with the CKC.
	char url[320];
	snprintf(url, sizeof(url), "https://play.itunes.apple.com/WebObjects/MZPlay.woa/music/fps");
	http_req_t req = {.method = "POST",
					  .body = (const char *)spc,
					  .body_len = spc_len,
					  .content_type = "application/octet-stream",
					  .want_error_body = true,
					  .allow_empty_body = false,
					  .no_https_downgrade = true};
	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	bool sent = http_request(url, &req, &body, &body_len, 1024 * 1024, 30, &status);
	memset(spc, 0, spc_len); // the SPC is session material: wipe before free
	free(spc);
	if (!sent || status != 200 || body_len == 0) {
		free(body);
		snprintf(error, error_size, "amp_fps_request_failed_%d", status);
		return false;
	}

	bool applied = fps_apply_ckc(adam_id, key_uri, (const uint8_t *)body, body_len, error,
								 error_size);
	memset(body, 0, body_len);
	free(body);
	return applied;
}

// ---------------------------------------------------------------------------
// the WV2D listener: batches in, plaintexts out
// ---------------------------------------------------------------------------

// daemon request payload: adam_len u16 + uri_len u16 + count u32 + table +
// labels + concatenated ciphertext (identical to the WV2D batch).
static bool relay_batch(const uint8_t *payload, size_t len, uint8_t **resp, size_t *resp_len,
						char *error, size_t error_size) {
	uint8_t *out = NULL;
	size_t out_len = 0;
	bool ok = false;
	if (!daemon_round_trip(7 /* OpDecryptBatch */, payload, len, &out, &out_len, &ok, error,
						   error_size)) {
		return false;
	}
	// A missing key is not a failure: negotiate, then replay the batch.
	if (ok) {
		*resp = out;
		*resp_len = out_len;
		return true;
	}
	char detail[160] = "";
	if (out_len > 1) {
		size_t n = out_len - 1 < sizeof(detail) - 1 ? out_len - 1 : sizeof(detail) - 1;
		memcpy(detail, out + 1, n);
		detail[n] = '\0';
	}
	free(out);
	if (strstr(detail, "key not cached") == NULL) {
		snprintf(error, error_size, "%s", detail[0] ? detail : "amp_decrypt_failed");
		return false;
	}

	// Pull adam/uri back out of the batch payload to run the exchange.
	if (len < 4) {
		snprintf(error, error_size, "amp_invalid_batch");
		return false;
	}
	size_t adam_len = (payload[0] << 8) | payload[1];
	size_t uri_len = (payload[2] << 8) | payload[3];
	if (4 + adam_len + uri_len > len) {
		snprintf(error, error_size, "amp_invalid_batch");
		return false;
	}
	char adam_id[40], key_uri[600];
	if (adam_len >= sizeof(adam_id) || uri_len >= sizeof(key_uri)) {
		snprintf(error, error_size, "amp_invalid_batch");
		return false;
	}
	memcpy(adam_id, payload + 4, adam_len);
	adam_id[adam_len] = '\0';
	memcpy(key_uri, payload + 4 + adam_len, uri_len);
	key_uri[uri_len] = '\0';

	if (!fps_exchange(adam_id, key_uri, error, error_size)) {
		return false;
	}
	return daemon_round_trip(7, payload, len, resp, resp_len, &ok, error, error_size) && ok;
}

// One WV2D client connection (the player's ampwrapper opens one per stream).
static void serve_client(int fd) {
	for (;;) {
		uint8_t head[16];
		if (!read_exact_fd(fd, head, sizeof(head))) {
			return;
		}
		if (memcmp(head, "WV2D", 4) != 0) {
			return;
		}
		uint16_t kind = (uint16_t)((head[6] << 8) | head[7]);
		uint32_t id = be32_get(head + 8);
		size_t plen = be32_get(head + 12);
		uint8_t *payload = malloc(plen ? plen : 1);
		if (!payload) {
			return;
		}
		if (plen && !read_exact_fd(fd, payload, plen)) {
			free(payload);
			return;
		}
		if (kind == 9) { // close
			free(payload);
			return;
		}
		uint16_t reply_kind = 2; // ok
		uint8_t *reply_payload = NULL;
		size_t reply_len = 0;
		if (kind != 1) {
			free(payload);
			continue; // not a batch: ignore
		}
		char error[192] = "";
		if (!relay_batch(payload, plen, &reply_payload, &reply_len, error, sizeof(error))) {
			reply_kind = 3; // error
			reply_payload = (uint8_t *)strdup(error);
			reply_len = strlen(error);
			if (!reply_payload) {
				free(payload);
				return;
			}
		}
		free(payload);

		uint8_t out[16];
		memcpy(out, "WV2D", 4);
		out[4] = 0;
		out[5] = 1;
		out[6] = 0;
		out[7] = (uint8_t)(reply_kind >> 8);
		out[7] = (uint8_t)reply_kind;
		out[8] = (uint8_t)(id >> 24);
		out[9] = (uint8_t)(id >> 16);
		out[10] = (uint8_t)(id >> 8);
		out[11] = (uint8_t)id;
		be32_put(out + 12, (uint32_t)reply_len);
		if (!write_exact_fd(fd, out, sizeof(out)) ||
			(reply_len && !write_exact_fd(fd, reply_payload, reply_len))) {
			free(reply_payload);
			return;
		}
		if (reply_payload && reply_kind == 3) {
			free(reply_payload);
		} else {
			free(reply_payload);
		}
	}
}

static void *listener_main(void *arg) {
	(void)arg;
	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int fd = accept(s_local.listen_fd, (struct sockaddr *)&peer, &plen);
		if (fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			return NULL; // listener closed: shutting down
		}
		// Only the player itself talks to us.
		if (ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
			close(fd);
			continue;
		}
		pthread_mutex_lock(&s_local.lock);
		s_local.idle_since_ms = now_ms();
		pthread_mutex_unlock(&s_local.lock);
		serve_client(fd);
		close(fd);
		// The batch just finished: start the idle clock again.
		pthread_mutex_lock(&s_local.lock);
		s_local.idle_since_ms = now_ms();
		pthread_mutex_unlock(&s_local.lock);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

bool amplocal_init(void) {
	pthread_mutex_lock(&s_local.lock);
	if (s_local.listen_fd >= 0) {
		pthread_mutex_unlock(&s_local.lock);
		return true;
	}
	if (config_get_int("applemusic", "translate", 0) == 0) {
		pthread_mutex_unlock(&s_local.lock);
		return false;
	}
	snprintf(s_local.translate_root, sizeof(s_local.translate_root), "%.280s",
			 config_get("applemusic", "translate_root", "/mnt/sd_0/.ampguest"));
	s_local.idle_secs = (int)config_get_int("applemusic", "translate_idle_secs", 300);
	if (s_local.idle_secs <= 0) {
		s_local.idle_secs = 300;
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		pthread_mutex_unlock(&s_local.lock);
		return false;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	addr.sin_port = htons((uint16_t)config_get_int("applemusic", "decrypt_port", 10020));
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
		close(fd);
		pthread_mutex_unlock(&s_local.lock);
		return false;
	}
	s_local.listen_fd = fd;
	s_local.idle_since_ms = now_ms();
	pthread_mutex_unlock(&s_local.lock);

	if (!s_local.thread_started) {
		if (pthread_create(&s_local.thread, NULL, listener_main, NULL) == 0) {
			s_local.thread_started = true;
		}
	}
	return true;
}

void amplocal_tick(void) {
	pthread_mutex_lock(&s_local.lock);
	if (s_local.listen_fd < 0) {
		pthread_mutex_unlock(&s_local.lock);
		return;
	}
	// Reap a dead daemon quietly; the next batch respawns it.
	if (s_local.daemon_pid > 0 && waitpid(s_local.daemon_pid, NULL, WNOHANG) != 0) {
		s_local.daemon_pid = -1;
		s_local.daemon_stdin = -1;
		s_local.daemon_stdout = -1;
		s_local.daemon_ok = false;
	}
	// Idle kill: the guest holds the FootHill cache, but RAM is precious;
	// after translate_idle_secs with no connection and no open work, close.
	if (s_local.daemon_pid > 0 && s_local.idle_secs > 0 &&
		now_ms() - s_local.idle_since_ms > (long)s_local.idle_secs * 1000) {
		daemon_kill_locked();
	}
	pthread_mutex_unlock(&s_local.lock);
}

void amplocal_shutdown(void) {
	pthread_mutex_lock(&s_local.lock);
	daemon_kill_locked();
	if (s_local.listen_fd >= 0) {
		shutdown(s_local.listen_fd, SHUT_RDWR);
		close(s_local.listen_fd);
		s_local.listen_fd = -1;
	}
	pthread_mutex_unlock(&s_local.lock);
}

bool amplocal_running(void) {
	pthread_mutex_lock(&s_local.lock);
	bool running = s_local.listen_fd >= 0 && s_local.daemon_pid > 0;
	pthread_mutex_unlock(&s_local.lock);
	return running;
}
