#include "src/system/streaming/anisette.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// The anisette assembly against a local socket that plays the v3 server: the
// client info and the get_headers answers. Provisioning itself needs a
// websocket with a real certificate behind it; what is checked here is
// everything around it -- the state the card keeps and the ten headers the
// GrandSlam calls consume.

static int failures;
#define CHECK(cond)                                                            \
	do {                                                                       \
		if (!(cond)) {                                                         \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
			failures++;                                                        \
		}                                                                      \
	} while (0)

static const char *CLIENT_INFO_JSON =
	"{\"client_info\":\"<MacBookPro13,2> <macOS;13.1;22C65> <com.apple.AuthKit/1 (com.apple.akd/1.0)>\","
	"\"user_agent\":\"akd/1.0 CFNetwork/808.1.4\"}";

static const char *HEADERS_JSON =
	"{\"result\":\"Headers\",\"X-Apple-I-MD-M\":\"B52E4F58-9C77-4F61-A0A1-C2E8A9C1D0F2\","
	"\"X-Apple-I-MD\":\"urz4f2/9Bq0AAAAAAAAAAA==\",\"X-Apple-I-MD-RINFO\":\"17106176\"}";

static int g_port;

static int listen_port(int *out) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
		close(fd);
		return -1;
	}
	socklen_t len = sizeof(addr);
	getsockname(fd, (struct sockaddr *)&addr, &len);
	*out = ntohs(addr.sin_port);
	return fd;
}

static void *http_serve(void *arg) {
	int lfd = (int)(intptr_t)arg;
	for (;;) {
		int fd = accept(lfd, NULL, NULL);
		if (fd < 0) {
			return NULL;
		}
		char req[4096];
		size_t got = 0;
		size_t want = 0;
		for (;;) {
			if (got >= sizeof(req) - 1) {
				break;
			}
			ssize_t r = read(fd, req + got, sizeof(req) - 1 - got);
			if (r <= 0) {
				break;
			}
			got += (size_t)r;
			req[got] = '\0';
			char *body_start = strstr(req, "\r\n\r\n");
			if (body_start && !want) {
				// the body follows the blank line; read to its full length
				const char *cl = strstr(req, "Content-Length: ");
				want = cl ? (size_t)atoi(cl + 16) : 0;
				if (got - (size_t)(body_start + 4 - req) >= want) {
					break;
				}
			} else if (body_start && got - (size_t)(body_start + 4 - req) >= want) {
				break;
			}
		}
		const char *answer = NULL;
		if (strstr(req, "GET /v3/client_info")) {
			answer = CLIENT_INFO_JSON;
		} else if (strstr(req, "POST /v3/get_headers")) {
			// the body must carry the identifier and the adi blob the state kept
			char *blank = strstr(req, "\r\n\r\n");
			printf("srv: get_headers body: %.200s\n", blank ? blank + 4 : "(none)");
			answer = strstr(req, "\"adi_pb\":\"AQLzVGVzdA==\"") ? HEADERS_JSON : NULL;
			printf("srv: answer set=%d\n", answer != NULL);
		} else {
			printf("srv: unknown request: %.60s\n", req);
		}
		char head[128];
		if (answer) {
			snprintf(head, sizeof(head),
					 "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
					 "Content-Length: %zu\r\n\r\n",
					 strlen(answer));
			if (write(fd, head, strlen(head)) >= 0) {
				if (write(fd, answer, strlen(answer)) < 0) {
				}
			}
		} else {
			snprintf(head, sizeof(head), "HTTP/1.0 404 Not Found\r\nContent-Length: 2\r\n\r\n");
			if (write(fd, head, strlen(head)) >= 0) {
				if (write(fd, "no", 2) < 0) {
				}
			}
		}
		close(fd);
	}
}

int main(void) {
	int lfd = listen_port(&g_port);
	CHECK(lfd >= 0);
	pthread_t server;
	pthread_create(&server, NULL, http_serve, (void *)(intptr_t)lfd);

	char server_url[64];
	snprintf(server_url, sizeof(server_url), "http://127.0.0.1:%d", g_port);
	anisette_set_server(server_url);

	char card[] = "/tmp/anisette-test-XXXXXX";
	CHECK(mkdtemp(card) != NULL);
	anisette_set_root(card);

	// Nothing on the card yet: no headers, and the message says why.
	gsa_anisette_t headers;
	char error[192] = "";
	CHECK(!anisette_headers(&headers, error, sizeof(error)));
	CHECK(strcmp(error, "anisette_not_provisioned") == 0);
	CHECK(!anisette_provisioned());

	// Seed the card the way provisioning would have: an identity and an adi
	// blob, written in the state file's own format.
	char dir[640];
	snprintf(dir, sizeof(dir), "%s/.local", card);
	CHECK(mkdir(dir, 0755) == 0 || errno == EEXIST);
	snprintf(dir, sizeof(dir), "%s/.local/amp-anisette", card);
	CHECK(mkdir(dir, 0755) == 0 || errno == EEXIST);
	char state_file[768];
	snprintf(state_file, sizeof(state_file), "%s/state.txt", dir);
	FILE *f = fopen(state_file, "w");
	CHECK(f != NULL);
	if (f) {
		fprintf(f,
				"identifier=0123456789abcdef0123456789abcdef\n"
				"serial=C02TESTSERIAL\n"
				"adi_pb=AQLzVGVzdA==\n");
		fclose(f);
	}
	CHECK(anisette_provisioned());

	// The headers come out assembled: the machine data from the server, the
	// rest derived from the state on the card.
	memset(error, 0, sizeof(error));
	bool got_headers = anisette_headers(&headers, error, sizeof(error));
	if (!got_headers) {
		printf("headers error: %s\n", error);
	}
	CHECK(got_headers);
	if (failures == 0) {
		CHECK(headers.count == 10);
		const char *want_keys[10] = {
			"X-Apple-I-Client-Time", "X-Apple-I-SRL-NO", "X-Apple-I-TimeZone",
			"X-Apple-Locale",		"X-Apple-I-MD-RINFO", "X-Apple-I-MD-LU",
			"X-Mme-Device-Id",		"X-Apple-I-MD",		"X-Apple-I-MD-M",
			"X-Mme-Client-Info",
		};
		const char *want_values[10] = {
			NULL, // the client time is the clock, only its shape is checked
			"C02TESTSERIAL",
			"UTC",
			"en_US",
			"17106176",
			// SHA-256 of the identifier 0123456789abcdef0123456789abcdef
			"223e0a160af9da0a03e6dd2c4719c56f5d66a633cbe84e78aaa9f3735865522a",
			"01234567-89ab-cdef-0123-456789abcdef",
			"urz4f2/9Bq0AAAAAAAAAAA==",
			"B52E4F58-9C77-4F61-A0A1-C2E8A9C1D0F2",
			// the identity is the server's /v3/client_info answer: the
			// OTP and the claimed machine are validated as a pair
			"<MacBookPro13,2> <macOS;13.1;22C65> <com.apple.AuthKit/1 (com.apple.akd/1.0)>",
		};
		for (int i = 0; i < 10; i++) {
			CHECK(strcmp(headers.kv[i].key, want_keys[i]) == 0);
			if (want_values[i]) {
				CHECK(strcmp(headers.kv[i].value, want_values[i]) == 0);
			}
		}
		// the client time reads like an ISO instant with a Z on the end
		size_t len = strlen(headers.kv[0].value);
		CHECK(len == 20 && headers.kv[0].value[4] == '-' && headers.kv[0].value[19] == 'Z');
	}

	shutdown(lfd, SHUT_RDWR);
	close(lfd);
	printf("anisette: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
