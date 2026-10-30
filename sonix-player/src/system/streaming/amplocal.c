#include "amplocal.h"

#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/net/http.h"

#include <arpa/inet.h>
#include <ctype.h>
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

// The translated FairPlay decrypt side plus the 3.6.0 auth side. One listener
// thread owns the decrypt port and the decrypt daemon's pipes; the player's
// WV2D client (ampwrapper.c) points at 127.0.0.1 and nothing else changes.
// The auth channel is a second, independent guest daemon: sign-in, tokens and
// the playback dispatch go straight over its pipes.
//
// The daemons run two processes deep: qemu-arm execs the guest binaries, and
// the auth one re-execs the Android dynamic linker. Both live inside the
// guest tree the user assembled; only qemu and the tree live on the card
// root. Frames (both sides) are the 20-byte WV2I envelopes; batch payloads
// are byte-identical, so the relay re-encodes only the header. The SPC/CKC
// split is the one real translation: the decrypt daemon asks (OpGetSPC), we
// POST to Apple, answer (OpApplyCKC), and the batch replays.

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

// One guest daemon: pipes, pid, health. The two Apple generations must never
// share a process (their ICU singletons fight), so each channel spawns its
// own.
typedef struct {
	int stdin_fd;  // WV2I pipe to the guest, -1 when not spawned
	int stdout_fd;
	pid_t pid;
	bool ok; // OpHealth passed after last spawn
	long used_ms;
} amp_channel_t;

typedef struct {
	int listen_fd; // decrypt-port listener, -1 when not translating
	amp_channel_t dec;
	amp_channel_t auth;
	char guest_root[300]; // where qemu-arm and the system/ tree live
	int idle_secs; // [applemusic] translate_idle_secs
	char last_error[192];
	bool error_reported;
	pthread_t thread;
	bool thread_started;
	pthread_mutex_t lock;      // state + spawn
	pthread_mutex_t auth_io;   // serializes auth-channel round trips
} amp_local_state_t;

static amp_local_state_t s_local = {
	.listen_fd = -1, .dec = {-1, -1, -1, false, 0}, .auth = {-1, -1, -1, false, 0}};

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

// A guest daemon's one-second watchdog keeps a dead guest from wedging a
// round trip: polls instead of blocking forever.
static bool wait_readable(amp_channel_t *ch, int timeout_ms) {
	struct pollfd pfd = {.fd = ch->stdout_fd, .events = POLLIN, .revents = 0};
	for (int slept = 0; slept < timeout_ms; slept += 250) {
		int r = poll(&pfd, 1, 250);
		if (r > 0) {
			return true;
		}
		if (r < 0 && errno != EINTR) {
			return false;
		}
		if (waitpid(ch->pid, NULL, WNOHANG) != 0) {
			return false; // died while we waited
		}
	}
	return (pfd.revents & POLLIN) != 0;
}

// ---------------------------------------------------------------------------
// guest daemon lifecycle
// ---------------------------------------------------------------------------

static void channel_kill_locked(amp_channel_t *ch) {
	if (ch->pid > 0) {
		// Politely: the daemon exits when stdin closes. SIGKILL only after
		// a grace beat so the FootHill state gets to flush its db.
		close(ch->stdin_fd);
		close(ch->stdout_fd);
		kill(ch->pid, SIGTERM);
		for (int i = 0; i < 10; i++) {
			if (waitpid(ch->pid, NULL, WNOHANG) != 0) {
				break;
			}
			struct timespec t = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
			nanosleep(&t, NULL);
		}
		kill(ch->pid, SIGKILL);
		waitpid(ch->pid, NULL, WNOHANG);
	}
	ch->pid = -1;
	ch->stdin_fd = -1;
	ch->stdout_fd = -1;
	ch->ok = false;
}

// Spawns qemu-arm with stdio pipes. The guest tree layout under guest_root:
//   qemu-arm             the statically linked user-mode emulator
//   system/bin/linker    Android's dynamic linker (arm)
//   system/bin/ampdaemon-auth     our auth daemon (3.6.0 set, dynamic)
//   system/bin/ampdaemon-static   our decrypt daemon (6.5.3 set, static)
//   system/lib lib36 lib65        the two Apple generations + API-24 closure
//   data/                the daemons' writable base (created here)
//
// auth mode:  ampdaemon-auth /system/lib36 auth   (AMP_DYN_LINKER=1)
// decrypt:    ampdaemon-static /system            (AMP_GUEST_LOADER=1)
static bool daemon_spawn_locked(amp_channel_t *ch, bool auth_mode) {
	char daemon_path[400], libs_arg[400], data_dir[340];
	const char *daemon_name = auth_mode ? "ampdaemon-auth" : "ampdaemon-static";
	snprintf(daemon_path, sizeof(daemon_path), "%.280s/system/bin/%.24s", s_local.guest_root,
			 daemon_name);
	if (access(daemon_path, X_OK) != 0) {
		snprintf(s_local.last_error, sizeof(s_local.last_error), "amp_guest_tree_missing");
		return false;
	}
	snprintf(libs_arg, sizeof(libs_arg), "%.280s/system/lib36", s_local.guest_root);
	snprintf(data_dir, sizeof(data_dir), "mkdir -p %.280s/data", s_local.guest_root);
	if (system(data_dir) != 0) {
		snprintf(s_local.last_error, sizeof(s_local.last_error), "amp_guest_data_failed");
		return false;
	}

	int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
	if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
		snprintf(s_local.last_error, sizeof(s_local.last_error), "amp_guest_pipe_failed");
		return false;
	}

	char qemu_path[400];
	snprintf(qemu_path, sizeof(qemu_path), "%.280s/qemu-arm", s_local.guest_root);

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
		// All guest-side paths are host paths: qemu-user passes syscalls
		// straight through, so the daemons read the tree and the data base
		// exactly as laid out on the card.
		char base[340];
		snprintf(base, sizeof(base), "%.280s/data", s_local.guest_root);
		setenv("AMP_BASE_DIR", base, 1);
		setenv("ANDROID_DATA", base, 1);
		setenv("ANDROID_ROOT", "/system", 1);
		if (auth_mode) {
			setenv("AMP_DYN_LINKER", "1", 1);
			setenv("LD_LIBRARY_PATH", "/system/lib36:/system/lib", 1);
			execl(qemu_path, "qemu-arm", "-L", s_local.guest_root, daemon_path, libs_arg,
				  "auth", (char *)NULL);
		} else {
			setenv("AMP_GUEST_LOADER", "1", 1);
			char sys_arg[400];
			snprintf(sys_arg, sizeof(sys_arg), "%.280s/system", s_local.guest_root);
			execl(qemu_path, "qemu-arm", "-L", s_local.guest_root, daemon_path, sys_arg,
				  (char *)NULL);
		}
		_exit(127);
	}
	close(in_pipe[0]);
	close(out_pipe[1]);
	ch->pid = pid;
	ch->stdin_fd = in_pipe[1];
	ch->stdout_fd = out_pipe[0];
	ch->ok = false;
	ch->used_ms = now_ms();
	return true;
}

// One WV2I round trip to a daemon. Returns the response payload or false
// (daemon dead / bad frame / timeout), filling error.
static bool daemon_round_trip(amp_channel_t *ch, int opcode, const uint8_t *payload, size_t len,
							  uint8_t **resp, size_t *resp_len, bool *ok, int timeout_ms,
							  char *error, size_t error_size) {
	if (ch->pid <= 0) {
		if (!daemon_spawn_locked(ch, opcode != 7 && opcode != 9 && opcode != 10)) {
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
	be32_put(head + 8, 1); // request id: one caller per channel at a time
	head[12] = (uint8_t)(opcode >> 8);
	head[13] = (uint8_t)opcode;
	head[14] = 0;
	head[15] = 0;
	be32_put(head + 16, (uint32_t)len);

	if (!write_exact_fd(ch->stdin_fd, head, sizeof(head)) ||
		(len && !write_exact_fd(ch->stdin_fd, payload, len))) {
		snprintf(error, error_size, "amp_guest_write_failed");
		return false;
	}
	if (!wait_readable(ch, timeout_ms)) {
		snprintf(error, error_size, "amp_guest_no_response");
		return false;
	}
	if (!read_exact_fd(ch->stdout_fd, head, sizeof(head)) ||
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
	if (rlen && !read_exact_fd(ch->stdout_fd, body, rlen)) {
		free(body);
		snprintf(error, error_size, "amp_guest_truncated");
		return false;
	}
	*ok = head[7] == 2 && rlen > 0 && body[0] == 0;
	*resp = body;
	*resp_len = rlen;
	ch->used_ms = now_ms();
	return true;
}

// ---------------------------------------------------------------------------
// the auth channel (sign-in, tokens, playback dispatch)
// ---------------------------------------------------------------------------

static const char *auth_error_text(const uint8_t *resp, size_t len) {
	static char text[192];
	if (!resp || len < 2) {
		return "amp_auth_failed";
	}
	size_t n = len - 1 < sizeof(text) - 1 ? len - 1 : sizeof(text) - 1;
	memcpy(text, resp + 1, n);
	text[n] = '\0';
	return text;
}

// Parses the OpMe field list (state=/apple_id=/storefront=/dsid=/...).
static bool me_from_payload(const uint8_t *payload, size_t len, amplocal_me_t *out) {
	memset(out, 0, sizeof(*out));
	if (len < 7 || memcmp(payload, "state=", 6) != 0) {
		return false;
	}
	const char *p = (const char *)payload;
	const char *end = p + len;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
		const char *eq = memchr(p, '=', line_len ? line_len : 1);
		if (eq && eq < p + line_len) {
			size_t key_len = (size_t)(eq - p);
			size_t val_len = line_len - key_len - 1;
			const char *val = eq + 1;
			if (key_len == 5 && !memcmp(p, "state", 5)) {
				snprintf(out->state, sizeof(out->state), "%.*s", (int)val_len, val);
				out->authenticated = strstr(out->state, "authenticated") != NULL;
			} else if (key_len == 8 && !memcmp(p, "apple_id", 8)) {
				snprintf(out->apple_id, sizeof(out->apple_id), "%.*s", (int)val_len, val);
			} else if (key_len == 9 && !memcmp(p, "storefront", 9)) {
				snprintf(out->storefront, sizeof(out->storefront), "%.*s", (int)val_len, val);
			} else if (key_len == 4 && !memcmp(p, "dsid", 4)) {
				snprintf(out->dsid, sizeof(out->dsid), "%.*s", (int)val_len, val);
			} else if (key_len == 9 && !memcmp(p, "dev_token", 9)) {
				snprintf(out->dev_token, sizeof(out->dev_token), "%.*s", (int)val_len, val);
			} else if (key_len == 16 && !memcmp(p, "music_user_token", 16)) {
				snprintf(out->music_user_token, sizeof(out->music_user_token), "%.*s",
						 (int)val_len, val);
			}
		}
		p += line_len + (nl ? 1 : 0);
	}
	return out->state[0] != '\0';
}

// Drives a started login flow to a terminal state. The flow runs at TCG
// speed and may park for a 2FA code; the cadence is gentle and the cap is
// generous but finite -- a wrong password loops inside Apple's flow forever,
// and this timeout is what turns that into an honest failure.
static amplocal_login_result_t auth_wait_settled(char *error, size_t error_size) {
	for (int i = 0; i < 800; i++) { // 800 x 300ms = 4 minutes
		uint8_t *resp = NULL;
		size_t resp_len = 0;
		bool ok = false;
		char err[192] = "";
		if (!daemon_round_trip(&s_local.auth, 13 /* OpMe */, NULL, 0, &resp, &resp_len, &ok,
							   30000, err, sizeof(err))) {
			snprintf(error, error_size, "%s", err);
			return AMPLOCAL_LOGIN_FAILED;
		}
		amplocal_me_t me;
		bool parsed = ok && me_from_payload(resp, resp_len, &me);
		free(resp);
		if (parsed) {
			if (!me.authenticated && strcmp(me.state, "in_progress") == 0) {
				struct timespec t = {.tv_sec = 0, .tv_nsec = 300 * 1000 * 1000};
				nanosleep(&t, NULL);
				continue;
			}
			if (strcmp(me.state, "awaiting_2fa") == 0) {
				return AMPLOCAL_LOGIN_2FA;
			}
			if (me.authenticated) {
				return AMPLOCAL_LOGIN_OK;
			}
			// failed: the error text came in the snapshot's error field,
			// which me_from_payload does not carry; keep it generic.
			snprintf(error, error_size, "amp_login_failed");
			return AMPLOCAL_LOGIN_FAILED;
		}
		snprintf(error, error_size, "amp_login_failed");
		return AMPLOCAL_LOGIN_FAILED;
	}
	snprintf(error, error_size, "amp_login_timeout");
	return AMPLOCAL_LOGIN_FAILED;
}

amplocal_login_result_t amplocal_signin(const char *username, const char *password,
										char *error, size_t error_size) {
	if (!username || !password || !username[0] || !password[0]) {
		snprintf(error, error_size, "amp_bad_credentials");
		return AMPLOCAL_LOGIN_FAILED;
	}
	size_t user_len = strlen(username), pass_len = strlen(password);
	size_t len = 4 + user_len + pass_len;
	uint8_t *payload = malloc(len);
	if (!payload) {
		snprintf(error, error_size, "amp_out_of_memory");
		return AMPLOCAL_LOGIN_FAILED;
	}
	payload[0] = (uint8_t)(user_len >> 8);
	payload[1] = (uint8_t)user_len;
	payload[2] = (uint8_t)(pass_len >> 8);
	payload[3] = (uint8_t)pass_len;
	memcpy(payload + 4, username, user_len);
	memcpy(payload + 4 + user_len, password, pass_len);

	pthread_mutex_lock(&s_local.auth_io);
	uint8_t *resp = NULL;
	size_t resp_len = 0;
	bool ok = false;
	char err[192] = "";
	bool sent = daemon_round_trip(&s_local.auth, 11 /* OpLogin */, payload, len, &resp,
								  &resp_len, &ok, 30000, err, sizeof(err));
	free(payload);
	if (!sent) {
		pthread_mutex_unlock(&s_local.auth_io);
		snprintf(error, error_size, "%s", err);
		return AMPLOCAL_LOGIN_FAILED;
	}
	amplocal_login_result_t r = AMPLOCAL_LOGIN_FAILED;
	if (ok) {
		r = auth_wait_settled(error, error_size);
	} else {
		snprintf(error, error_size, "%s", auth_error_text(resp, resp_len));
	}
	free(resp);
	pthread_mutex_unlock(&s_local.auth_io);
	return r;
}

amplocal_login_result_t amplocal_2fa(const char *code, char *error, size_t error_size) {
	if (!code || !code[0]) {
		snprintf(error, error_size, "amp_bad_credentials");
		return AMPLOCAL_LOGIN_FAILED;
	}
	pthread_mutex_lock(&s_local.auth_io);
	uint8_t *resp = NULL;
	size_t resp_len = 0;
	bool ok = false;
	char err[192] = "";
	if (!daemon_round_trip(&s_local.auth, 12 /* Op2FA */, (const uint8_t *)code,
						   strlen(code), &resp, &resp_len, &ok, 30000, err, sizeof(err))) {
		pthread_mutex_unlock(&s_local.auth_io);
		snprintf(error, error_size, "%s", err);
		return AMPLOCAL_LOGIN_FAILED;
	}
	amplocal_login_result_t r = AMPLOCAL_LOGIN_FAILED;
	if (ok) {
		r = auth_wait_settled(error, error_size);
	} else {
		snprintf(error, error_size, "%s", auth_error_text(resp, resp_len));
	}
	free(resp);
	pthread_mutex_unlock(&s_local.auth_io);
	return r;
}

bool amplocal_logout(char *error, size_t error_size) {
	pthread_mutex_lock(&s_local.auth_io);
	uint8_t *resp = NULL;
	size_t resp_len = 0;
	bool ok = false;
	char err[192] = "";
	bool sent = daemon_round_trip(&s_local.auth, 14 /* OpLogout */, NULL, 0, &resp, &resp_len,
								  &ok, 30000, err, sizeof(err));
	if (sent && !ok) {
		snprintf(error, error_size, "%s", auth_error_text(resp, resp_len));
	}
	pthread_mutex_unlock(&s_local.auth_io);
	free(resp);
	if (!sent) {
		snprintf(error, error_size, "%s", err);
		return false;
	}
	return ok;
}

bool amplocal_me(amplocal_me_t *out, char *error, size_t error_size) {
	pthread_mutex_lock(&s_local.auth_io);
	uint8_t *resp = NULL;
	size_t resp_len = 0;
	bool ok = false;
	char err[192] = "";
	bool sent = daemon_round_trip(&s_local.auth, 13 /* OpMe */, NULL, 0, &resp, &resp_len, &ok,
								  30000, err, sizeof(err));
	pthread_mutex_unlock(&s_local.auth_io);
	if (!sent) {
		snprintf(error, error_size, "%s", err);
		return false;
	}
	bool parsed = ok && me_from_payload(resp, resp_len, out);
	free(resp);
	if (!parsed) {
		snprintf(error, error_size, "amp_me_failed");
		return false;
	}
	return true;
}

// The playback dispatch for one track: the raw MZ dispatch JSON, malloc'd,
// owned by the caller. amphls.c picks the master playlist URL out of
// songList[0]. False on refusal, with the reason in `error`.
//
// Weak on purpose: the on-device dispatch needs the guest daemon, which no
// test environment has. test_ampstream.c overrides it with a stand-in that
// answers from a mock server, and the strong definition wins the link.
__attribute__((weak)) bool amplocal_playback(const char *adam_id, char **body, size_t *body_len,
											 char *error, size_t error_size) {
	if (!adam_id || !adam_id[0]) {
		snprintf(error, error_size, "amp_bad_adam_id");
		return false;
	}
	size_t adam_len = strlen(adam_id);
	uint8_t *payload = malloc(2 + adam_len);
	if (!payload) {
		snprintf(error, error_size, "amp_out_of_memory");
		return false;
	}
	payload[0] = (uint8_t)(adam_len >> 8);
	payload[1] = (uint8_t)adam_len;
	memcpy(payload + 2, adam_id, adam_len);

	// The dispatch is a full PurchaseRequest round trip at TCG speed: the
	// wait is minutes, not seconds.
	pthread_mutex_lock(&s_local.auth_io);
	uint8_t *resp = NULL;
	size_t resp_len = 0;
	bool ok = false;
	char err[192] = "";
	bool sent = daemon_round_trip(&s_local.auth, 15 /* OpPlayback */, payload, 2 + adam_len,
								  &resp, &resp_len, &ok, 240000, err, sizeof(err));
	free(payload);
	pthread_mutex_unlock(&s_local.auth_io);
	if (!sent) {
		snprintf(error, error_size, "%s", err);
		return false;
	}
	if (!ok) {
		snprintf(error, error_size, "%s", auth_error_text(resp, resp_len));
		free(resp);
		return false;
	}
	*body_len = resp_len - 1;
	*body = malloc(*body_len + 1);
	if (!*body) {
		free(resp);
		snprintf(error, error_size, "amp_out_of_memory");
		return false;
	}
	memcpy(*body, resp + 1, *body_len);
	(*body)[*body_len] = '\0';
	free(resp);
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
	if (!daemon_round_trip(&s_local.dec, 9 /* OpGetSPC */, payload, len, &resp, &resp_len, &ok,
						   60000, error, error_size)) {
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
	if (!daemon_round_trip(&s_local.dec, 10 /* OpApplyCKC */, payload, len, &resp, &resp_len,
						   &ok, 60000, error, error_size)) {
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

// Posts the SPC to Apple's FPS key server and hands the CKC back. The 6.5.3
// SPC request carries the key uri, format and protocol as caller arguments,
// and the endpoint itself stays the same across generations.
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
	if (!daemon_round_trip(&s_local.dec, 7 /* OpDecryptBatch */, payload, len, &out, &out_len,
						   &ok, 60000, error, error_size)) {
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
	return daemon_round_trip(&s_local.dec, 7, payload, len, resp, resp_len, &ok, 60000, error,
							 error_size) &&
		   ok;
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
		free(reply_payload);
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
		s_local.dec.used_ms = now_ms();
		pthread_mutex_unlock(&s_local.lock);
		serve_client(fd);
		close(fd);
		// The batch just finished: start the idle clock again.
		pthread_mutex_lock(&s_local.lock);
		s_local.dec.used_ms = now_ms();
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
	snprintf(s_local.guest_root, sizeof(s_local.guest_root), "%.280s",
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
	s_local.dec.used_ms = now_ms();
	s_local.auth.used_ms = now_ms();
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
	// Reap dead daemons quietly; the next use respawns them.
	if (s_local.dec.pid > 0 && waitpid(s_local.dec.pid, NULL, WNOHANG) != 0) {
		s_local.dec.pid = -1;
		s_local.dec.stdin_fd = -1;
		s_local.dec.stdout_fd = -1;
		s_local.dec.ok = false;
	}
	if (s_local.auth.pid > 0 && waitpid(s_local.auth.pid, NULL, WNOHANG) != 0) {
		s_local.auth.pid = -1;
		s_local.auth.stdin_fd = -1;
		s_local.auth.stdout_fd = -1;
		s_local.auth.ok = false;
	}
	// Idle kill: RAM is precious; after translate_idle_secs with no use,
	// close. The auth daemon holds the FootHill session cache on the card,
	// but that survives a kill -- only in-flight work is lost.
	long now = now_ms();
	if (s_local.dec.pid > 0 && s_local.idle_secs > 0 &&
		now - s_local.dec.used_ms > (long)s_local.idle_secs * 1000) {
		channel_kill_locked(&s_local.dec);
	}
	if (s_local.auth.pid > 0 && s_local.idle_secs > 0 &&
		now - s_local.auth.used_ms > (long)s_local.idle_secs * 1000) {
		channel_kill_locked(&s_local.auth);
	}
	pthread_mutex_unlock(&s_local.lock);
}

void amplocal_shutdown(void) {
	pthread_mutex_lock(&s_local.lock);
	channel_kill_locked(&s_local.dec);
	channel_kill_locked(&s_local.auth);
	if (s_local.listen_fd >= 0) {
		shutdown(s_local.listen_fd, SHUT_RDWR);
		close(s_local.listen_fd);
		s_local.listen_fd = -1;
	}
	pthread_mutex_unlock(&s_local.lock);
}

bool amplocal_running(void) {
	pthread_mutex_lock(&s_local.lock);
	bool running = s_local.listen_fd >= 0 && s_local.dec.pid > 0;
	pthread_mutex_unlock(&s_local.lock);
	return running;
}
