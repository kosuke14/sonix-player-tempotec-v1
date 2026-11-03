#include "src/system/streaming/ampitunes.h"

#include "src/system/net/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// The guest catalog against a local socket that plays the iTunes API: the
// search's answer, the lookup's collection-plus-songs answer, and a preview
// file to fetch. The card is a directory in /tmp.

static int failures;
#define CHECK(cond)                                                                 \
	do {                                                                            \
		if (!(cond)) {                                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
			failures++;                                                             \
		}                                                                           \
	} while (0)

// The fake songs: one hit, one music video (dropped), one more hit.
static const char *SEARCH_JSON =
	"{\"resultCount\":3,\"results\":["
	"{\"wrapperType\":\"track\",\"kind\":\"song\",\"trackId\":1648876333,"
	"\"collectionId\":1648875799,\"trackName\":\"Tada Kimi ni Hare\","
	"\"artistName\":\"Yorushika\",\"collectionName\":\"Hitchcock\","
	"\"previewUrl\":\"http://127.0.0.1:PORT/preview/1648876333.m4a\","
	"\"artworkUrl100\":\"https://is1.example/100x100bb.jpg\",\"trackTimeMillis\":198971},"
	"{\"wrapperType\":\"track\",\"kind\":\"music-video\",\"trackId\":99,"
	"\"collectionId\":1,\"trackName\":\"Video\",\"artistName\":\"X\","
	"\"previewUrl\":\"http://127.0.0.1:PORT/preview/99.m4a\",\"trackTimeMillis\":1000},"
	"{\"wrapperType\":\"track\",\"kind\":\"song\",\"trackId\":555,"
	"\"collectionId\":1648875799,\"trackName\":\"Hitchcock\","
	"\"artistName\":\"Yorushika\",\"collectionName\":\"Hitchcock\","
	"\"previewUrl\":\"http://127.0.0.1:PORT/preview/555.m4a\","
	"\"artworkUrl100\":\"https://is1.example/100x100bb.jpg\",\"trackTimeMillis\":210000}]}";

// The lookup answer: the collection first, the songs after it.
static const char *LOOKUP_JSON =
	"{\"resultCount\":3,\"results\":["
	"{\"wrapperType\":\"collection\",\"collectionId\":1648875799,"
	"\"collectionName\":\"Hitchcock\",\"artistName\":\"Yorushika\"},"
	"{\"wrapperType\":\"track\",\"kind\":\"song\",\"trackId\":555,"
	"\"collectionId\":1648875799,\"trackName\":\"Hitchcock\","
	"\"artistName\":\"Yorushika\",\"collectionName\":\"Hitchcock\","
	"\"previewUrl\":\"http://127.0.0.1:PORT/preview/555.m4a\",\"trackTimeMillis\":210000},"
	"{\"wrapperType\":\"track\",\"kind\":\"song\",\"trackId\":1648876333,"
	"\"collectionId\":1648875799,\"trackName\":\"Tada Kimi ni Hare\","
	"\"artistName\":\"Yorushika\",\"collectionName\":\"Hitchcock\","
	"\"previewUrl\":\"http://127.0.0.1:PORT/preview/1648876333.m4a\","
	"\"trackTimeMillis\":198971}]}";

static const char *PREVIEW_BODY = "\0\0\0\x18" "ftypM4A fake preview bytes for the test";

static int g_port;
static char g_last_path[300];
static pthread_mutex_t last_lock = PTHREAD_MUTEX_INITIALIZER;

static int listen_port(int *out) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
		close(fd);
		return -1;
	}
	socklen_t len = sizeof(addr);
	getsockname(fd, (struct sockaddr *)&addr, &len);
	*out = ntohs(addr.sin_port);
	return fd;
}

// Replaces the ":PORT/" marker with the real port, in place: the marker and
// the substitution are the same length whenever the port is five digits, and
// the page buffer has room to shift either way.
static void page_with_port(const char *src, char *out, size_t size) {
	char port[8];
	snprintf(port, sizeof(port), "%d", g_port);
	size_t o = 0;
	for (const char *p = src; *p && o + 16 < size; p++) {
		if (p[0] == ':' && strncmp(p, ":PORT/", 6) == 0) {
			o += (size_t)snprintf(out + o, size - o, ":%s/", port);
			p += 5;
		} else {
			out[o++] = *p;
		}
	}
	out[o] = '\0';
}

static void respond(int fd, const char *ctype, const char *text, size_t len) {
	bool ok_wrote = true;
	char head[256];
	snprintf(head, sizeof(head),
			 "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", ctype, len);
	if (write(fd, head, strlen(head)) < 0 || write(fd, text, len) < 0) {
		ok_wrote = false;
	}
	(void)ok_wrote;
}

// Serves whichever answer the path asks for, with the port substituted into
// the JSON so the preview addresses point back here.
static void *http_serve(void *arg) {
	int lfd = (int)(intptr_t)arg;
	char page[4096];
	for (;;) {
		int fd = accept(lfd, NULL, NULL);
		if (fd < 0) {
			return NULL;
		}
		char req[4096];
		size_t got = 0;
		bool whole = false;
		while (!whole && got < sizeof(req) - 1) {
			ssize_t r = read(fd, req + got, sizeof(req) - 1 - got);
			if (r <= 0) {
				break;
			}
			got += (size_t)r;
			req[got] = '\0';
			if (strstr(req, "\r\n\r\n")) {
				whole = true;
			}
		}
		char *line_end = strstr(req, "\r\n");
		if (line_end) {
			*line_end = '\0';
		}
		pthread_mutex_lock(&last_lock);
		snprintf(g_last_path, sizeof(g_last_path), "%.280s", req);
		pthread_mutex_unlock(&last_lock);

		if (strstr(req, "GET /search?")) {
			page_with_port(SEARCH_JSON, page, sizeof(page));
			respond(fd, "application/json", page, strlen(page));
		} else if (strstr(req, "GET /lookup?")) {
			page_with_port(LOOKUP_JSON, page, sizeof(page));
			respond(fd, "application/json", page, strlen(page));
		} else if (strstr(req, "GET /preview/")) {
			respond(fd, "audio/mp4", PREVIEW_BODY, strlen(PREVIEW_BODY) + 1);
		} else {
			const char *not_found = "no";
			char head[128];
			snprintf(head, sizeof(head), "HTTP/1.0 404 Not Found\r\nContent-Length: 2\r\n\r\n");
			if (write(fd, head, strlen(head)) < 0) {
			}
			if (write(fd, not_found, 2) < 0) {
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

	char base[64];
	snprintf(base, sizeof(base), "http://127.0.0.1:%d", g_port);
	ampitunes_set_api(base);

	char card[] = "/tmp/ampitunes-test-XXXXXX";
	CHECK(mkdtemp(card) != NULL);
	ampitunes_set_root(card);

	// --- the search -----------------------------------------------------
	ampapi_song_t songs[8];
	char error[192] = "";
	int count = ampitunes_search("yorushika", songs, 8, error, sizeof(error));
	CHECK(count == 2); // the music video is dropped, the two songs stay
	if (count == 2) {
		CHECK(strcmp(songs[0].id, "1648876333") == 0);
		CHECK(strcmp(songs[0].album_id, "1648875799") == 0);
		CHECK(strcmp(songs[0].title, "Tada Kimi ni Hare") == 0);
		CHECK(songs[0].duration_ms == 198971);
		// the artwork came back cut at the size the rows read well at
		CHECK(strstr(songs[0].artwork, "300x300bb") != NULL);
		CHECK(strstr(songs[0].artwork, "100x100bb") == NULL);
		CHECK(strstr(songs[0].preview, "/preview/1648876333.m4a") != NULL);
	}

	// --- the album lookup ------------------------------------------------
	memset(songs, 0, sizeof(songs));
	count = ampitunes_album_tracks("1648875799", songs, 8, error, sizeof(error));
	CHECK(count == 2); // the collection row is dropped on its own
	if (count == 2) {
		CHECK(strcmp(songs[0].id, "555") == 0);
		CHECK(strcmp(songs[1].id, "1648876333") == 0);
	}

	// --- the fetch and the ready check -----------------------------------
	char path[768];
	CHECK(!ampitunes_ready(&songs[0], path, sizeof(path))); // nothing on the card yet
	bool fetched = ampitunes_fetch(&songs[0], error, sizeof(error));
	if (!fetched) {
		printf("fetch error: %s\n", error);
	}
	CHECK(fetched);
	bool ready = ampitunes_ready(&songs[0], path, sizeof(path));
	if (!ready || !strstr(path, "/.local/amp-preview/555.m4a")) {
		printf("ready path: %s\n", path);
	}
	CHECK(ready);
	CHECK(strstr(path, "/.local/amp-preview/555.m4a") != NULL);
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f) {
		char buf[64] = "";
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		CHECK(n == strlen(PREVIEW_BODY) + 1);
		CHECK(strncmp(buf, "\0\0\0\x18" "ftypM4A", 11) == 0);
	}
	// A second fetch is a no-op: the file is already there in full.
	CHECK(ampitunes_fetch(&songs[0], error, sizeof(error)));

	// A bad id is refused before any request leaves.
	CHECK(ampitunes_album_tracks("12;rm", songs, 8, error, sizeof(error)) < 0);
	CHECK(strstr(error, "ampitunes_bad_args") != NULL);

	pthread_mutex_lock(&last_lock);
	CHECK(strstr(g_last_path, "/search?") != NULL || strstr(g_last_path, "/lookup?") != NULL ||
		  strstr(g_last_path, "/preview/") != NULL);
	pthread_mutex_unlock(&last_lock);

	shutdown(lfd, SHUT_RDWR);
	close(lfd);
	printf("ampitunes: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
