// The host-side PoC rig for the translated FairPlay daemon: spawns
// qemu-i386 with the guest's Android linker and ampdaemon, speaks the
// daemon's WV2I stdio framing, and reports what worked.
//
//   tools/wv2dtest <guest-root> [adam_id] [key_uri]
//
// <guest-root> is the assembled guest tree (tools/assemble_ampguest.sh
// layout: system/bin/linker, system/bin/ampdaemon, system/lib/*.so).
// Without adam_id the run stops after the health check and the SPC probe,
// which is the CI-able part; with a real adam_id it exercises the SPC
// build end to end (the CKC exchange itself is driven by the listener in
// the real player).
#include "src/system/streaming/amplocal_test.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t g_pid = -1;
static int g_stdin_w = -1, g_stdout_r = -1;

static uint32_t be32_get(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool read_exact(int fd, void *buf, size_t len) {
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

static bool write_exact(int fd, const void *buf, size_t len) {
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

static bool round_trip(int opcode, const uint8_t *payload, size_t len, uint8_t **out,
					   size_t *out_len, int timeout_ms) {
	uint8_t head[20];
	memcpy(head, "WV2I", 4);
	head[4] = 0;
	head[5] = 1;
	head[6] = 0;
	head[7] = 1;
	head[8] = head[9] = head[10] = head[11] = 0; // request id 1
	head[12] = (uint8_t)(opcode >> 8);
	head[13] = (uint8_t)opcode;
	head[14] = head[15] = 0;
	head[16] = (uint8_t)(len >> 24);
	head[17] = (uint8_t)(len >> 16);
	head[18] = (uint8_t)(len >> 8);
	head[19] = (uint8_t)len;
	if (!write_exact(g_stdin_w, head, sizeof(head)) || (len && !write_exact(g_stdin_w, payload, len))) {
		fprintf(stderr, "wv2dtest: daemon write failed\n");
		return false;
	}
	struct pollfd pfd = {.fd = g_stdout_r, .events = POLLIN, .revents = 0};
	int waited = 0;
	while (waited < timeout_ms) {
		int r = poll(&pfd, 1, 200);
		if (r > 0) {
			break;
		}
		if (waitpid(g_pid, NULL, WNOHANG) != 0) {
			fprintf(stderr, "wv2dtest: daemon died while waiting\n");
			return false;
		}
		waited += 200;
	}
	if (waited >= timeout_ms) {
		fprintf(stderr, "wv2dtest: daemon timed out after %d ms\n", timeout_ms);
		return false;
	}
	if (!read_exact(g_stdout_r, head, sizeof(head)) || memcmp(head, "WV2I", 4) != 0) {
		fprintf(stderr, "wv2dtest: bad frame header\n");
		return false;
	}
	uint32_t rlen = be32_get(head + 16);
	uint8_t *body = malloc(rlen ? rlen : 1);
	if (!body || (rlen && !read_exact(g_stdout_r, body, rlen))) {
		free(body);
		return false;
	}
	*out = body;
	*out_len = rlen;
	return true;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <guest-root> [adam_id] [key_uri]\n", argv[0]);
		return 2;
	}
	const char *root = argv[1];
	const char *adam = argc > 2 ? argv[2] : NULL;
	const char *uri = argc > 3 ? argv[3] : "skd://itunes.apple.com/P123456789/s1/e1";

	char linker[512], daemon[512], libs[512];
	snprintf(linker, sizeof(linker), "%s/system/bin/linker", root);
	snprintf(daemon, sizeof(daemon), "%s/system/bin/ampdaemon", root);
	snprintf(libs, sizeof(libs), "%s/system/lib", root);
	if (access(linker, X_OK) != 0) {
		fprintf(stderr, "wv2dtest: %s missing (assemble the guest tree first)\n", linker);
		return 2;
	}

	int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
	if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
		perror("pipe");
		return 2;
	}
	g_pid = fork();
	if (g_pid < 0) {
		perror("fork");
		return 2;
	}
	if (g_pid == 0) {
		dup2(in_pipe[0], STDIN_FILENO);
		dup2(out_pipe[1], STDOUT_FILENO);
		close(in_pipe[1]);
		close(out_pipe[0]);
		// stderr stays visible: the daemon's trace lines are the PoC's log.
		setenv("AMP_BASE_DIR", "/data/amp", 1);
		setenv("ANDROID_DNS_MODE", "local", 1);
		setenv("ANDROID_DATA", "/data/amp", 1);
		setenv("ANDROID_ROOT", "/system", 1);
		setenv("AMP_GUEST_LOADER", getenv("AMP_GUEST_STATIC") ? "1" : "0", 1);
        const char *bin = getenv("AMP_GUEST_STATIC") ? "ampdaemon-static" : "ampdaemon";
        char daemon_full[600];
        snprintf(daemon_full, sizeof(daemon_full), "%s/system/bin/%s", root, bin);
        // qemu-user's -L only prefixes execve/interp, not plain open(): the
        // guest loader needs the host-side path to the libraries.
        char libs_full[600];
        snprintf(libs_full, sizeof(libs_full), "%s/system/lib", root);
        if (getenv("AMP_GUEST_STATIC")) {
            // the static build carries its own loader: no Android linker hop
            execlp("qemu-i386", "qemu-i386", "-L", root, daemon_full, libs_full, (char *)NULL);
        }
        execlp("qemu-i386", "qemu-i386", "-L", root, linker, daemon, libs_full, (char *)NULL);
		perror("execlp qemu-i386");
		_exit(127);
	}
	close(in_pipe[0]);
	close(out_pipe[1]);
	g_stdin_w = in_pipe[1];
	g_stdout_r = out_pipe[0];

	// ---- health ----
	uint8_t *resp = NULL;
	size_t resp_len = 0;
	if (!round_trip(1, NULL, 0, &resp, &resp_len, 20000)) {
		fprintf(stderr, "FAIL health\n");
		return 1;
	}
	bool healthy = resp_len > 0 && resp[0] == 0;
	printf("health: %.*s\n", (int)(resp_len > 1 ? resp_len - 1 : 0),
		   healthy ? (const char *)resp + 1 : (const char *)resp);
	free(resp);
	if (!healthy) {
		fprintf(stderr, "FAIL: daemon not healthy\n");
		return 1;
	}
	printf("PASS health\n");

	if (!adam) {
		// CI-able part done: the guest boots, links bionic, loads the Apple
		// set, resolves the symbols and answers frames.
		kill(g_pid, SIGTERM);
		waitpid(g_pid, NULL, 0);
		printf("wv2dtest: guest-only checks passed\n");
		return 0;
	}

	// ---- the SPC build ----
	// getSPC's 4th argument is the base64 FPS application certificate; the
	// PoC takes it from AMP_FP_CERT (the real cert from the host playback
	// flow) or falls back to a structurally valid dummy -- enough to exercise
	// the library's request-building path, which the FPS server would reject
	// later at CKC time.
	const char *cert_env = getenv("AMP_FP_CERT");
	char dummy_cert[1024];
	if (!cert_env || !cert_env[0]) {
		// 768 bytes of 'A' = 1024 base64 chars: a well-formed (dummy) cert.
		size_t filled = 0;
		while (filled + 4 <= sizeof(dummy_cert) - 1) {
			dummy_cert[filled++] = 'A';
			dummy_cert[filled++] = 'Q';
			dummy_cert[filled++] = 'I';
			dummy_cert[filled++] = 'D';
		}
		dummy_cert[filled] = '\0';
		cert_env = dummy_cert;
	}
	size_t adam_len = strlen(adam), uri_len = strlen(uri), cert_len = strlen(cert_env);
	uint32_t total = 4 + adam_len + uri_len + 4 + (uint32_t)cert_len;
	uint8_t *payload = malloc(total);
	payload[0] = (uint8_t)(adam_len >> 8);
	payload[1] = (uint8_t)adam_len;
	payload[2] = (uint8_t)(uri_len >> 8);
	payload[3] = (uint8_t)uri_len;
	memcpy(payload + 4, adam, adam_len);
	memcpy(payload + 4 + adam_len, uri, uri_len);
	uint32_t cert_be = cert_len;
	payload[4 + adam_len + uri_len + 0] = (uint8_t)(cert_be >> 24);
	payload[4 + adam_len + uri_len + 1] = (uint8_t)(cert_be >> 16);
	payload[4 + adam_len + uri_len + 2] = (uint8_t)(cert_be >> 8);
	payload[4 + adam_len + uri_len + 3] = (uint8_t)cert_be;
	memcpy(payload + 4 + adam_len + uri_len + 4, cert_env, cert_len);
	if (!round_trip(9, payload, total, &resp, &resp_len, 60000)) {
		fprintf(stderr, "FAIL getSPC\n");
		return 1;
	}
	if (resp_len < 1 || resp[0] != 0) {
		fprintf(stderr, "getSPC error: %.*s\n", (int)(resp_len > 1 ? resp_len - 1 : 0),
				resp_len ? (const char *)resp + 1 : "");
		kill(g_pid, SIGTERM);
		waitpid(g_pid, NULL, 0);
		return 1;
	}
	printf("PASS getSPC: %zu bytes of SPC\n", resp_len - 1);
	free(resp);
	free(payload);
	kill(g_pid, SIGTERM);
	waitpid(g_pid, NULL, 0);
	printf("wv2dtest: decrypt path checks passed\n");
	return 0;
}
