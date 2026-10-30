// The Apple Music streaming source against local fakes: a canned HTTP daemon
// for the playback dispatch and the HLS pieces, and a TCP decrypt peer that
// speaks the WV2D framing back (plaintext = ciphertext ^ 0xff, which the test
// knows to expect). What is under test is the whole producer pipeline -- the
// queue's pseudo-paths, the playlist walk, the fMP4 slicing, the batching and
// the ADTS re-emission -- without any Apple anything and beyond loopback only
// in imagination.
#include "src/system/streaming/ampstream.h"
#include "src/system/streaming/ampfp4.h"
#include "src/system/core/config.h"
#include "src/system/net/http.h"

#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// box building: a tiny writer for the fMP4 fixtures
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t buf[16384];
	size_t len;
} fbuf_t;

static void fpush(fbuf_t *b, const void *data, size_t n) {
	memcpy(b->buf + b->len, data, n);
	b->len += n;
}
static void fu32(fbuf_t *b, uint32_t v) {
	uint8_t t[4] = {v >> 24, v >> 16, v >> 8, v};
	fpush(b, t, 4);
}
static void fu16(fbuf_t *b, uint16_t v) {
	uint8_t t[2] = {v >> 8, v};
	fpush(b, t, 2);
}
static void fu8(fbuf_t *b, uint8_t v) { fpush(b, &v, 1); }
static size_t fbox(fbuf_t *b, const char *type) {
	size_t at = b->len;
	fu32(b, 0);
	fpush(b, type, 4);
	return at;
}
static void fbox_end(fbuf_t *b, size_t at) {
	uint32_t sz = (uint32_t)(b->len - at);
	b->buf[at] = sz >> 24;
	b->buf[at + 1] = sz >> 16;
	b->buf[at + 2] = sz >> 8;
	b->buf[at + 3] = sz;
}
static void write32(fbuf_t *b, size_t at, uint32_t v) {
	b->buf[at] = v >> 24;
	b->buf[at + 1] = v >> 16;
	b->buf[at + 2] = v >> 8;
	b->buf[at + 3] = v;
}

// The init segment: ftyp, then moov -- tkhd (track 1), hdlr ("soun"), and a
// stsd with two descriptions: mp4a plain (the prefetch description, unencrypted
// here so the test needs no AES) and enca with a cbcs 1:9 tenc.
static void build_init(fbuf_t *b) {
	size_t ftyp = fbox(b, "ftyp");
	fpush(b, "M4A ", 4);
	fu32(b, 0);
	fbox_end(b, ftyp);
	size_t moov = fbox(b, "moov");
	{
		size_t trak = fbox(b, "trak");
		{
			size_t tkhd = fbox(b, "tkhd");
			fu32(b, 0); // version 0
			fu32(b, 0); // creation
			fu32(b, 0); // modification
			fu32(b, 1); // track id
			fu32(b, 0); // reserved
			fu32(b, 0); // duration
			fbox_end(b, tkhd);
			size_t mdia = fbox(b, "mdia");
			{
				size_t hdlr = fbox(b, "hdlr");
				fu32(b, 0);
				fu32(b, 0);
				fpush(b, "soun", 4);
				fu32(b, 0);
				fu32(b, 0);
				fu32(b, 0);
				fbox_end(b, hdlr);
				size_t minf = fbox(b, "minf");
				{
					size_t stbl = fbox(b, "stbl");
					{
						size_t stsd = fbox(b, "stsd");
						fu32(b, 0); // version 0
						fu32(b, 2); // two descriptions
						for (int e = 0; e < 2; e++) {
							size_t entry = fbox(b, e == 0 ? "mp4a" : "enca");
							// AudioSampleEntry: reserved(6), index(2),
							// reserved(8), channels(2), size(2), (2)+(2),
							// rate 16.16
							fu32(b, 0);
							fu16(b, 0);
							fu16(b, 1);
							fu32(b, 0);
							fu32(b, 0);
							fu16(b, 2);
							fu16(b, 16);
							fu16(b, 0);
							fu16(b, 0);
							fu32(b, (uint32_t)44100 << 16);
							{
								size_t esds = fbox(b, "esds");
								fu32(b, 0); // version 0
								// ES -> DecoderConfig -> ASC (AAC-LC 44100, 2ch)
								uint8_t desc[] = {0x03, 0x16, 0x00, 0x01, 0x00, 0x04, 0x11,
												  0x40, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00,
												  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
												  0x02, 0x12, 0x10};
								fpush(b, desc, sizeof(desc));
								fbox_end(b, esds);
							}
							if (e == 1) {
								size_t sinf = fbox(b, "sinf");
								{
									size_t frma = fbox(b, "frma");
									fpush(b, "mp4a", 4);
									fbox_end(b, frma);
									size_t schm = fbox(b, "schm");
									fu32(b, 0);
									fpush(b, "cbcs", 4);
									fu32(b, 0x00010000);
									fbox_end(b, schm);
									size_t schi = fbox(b, "schi");
									{
										size_t tenc = fbox(b, "tenc");
										fu32(b, 0x01000000); // version 1
										fu8(b, 0);			 // reserved
										fu8(b, 0x19);		 // pattern 1:9
										fu8(b, 1);			 // isEncrypted
										fu8(b, 8);			 // per-sample IV size
										for (int i = 0; i < 16; i++) {
											fu8(b, 0); // key id: the wrapper keys by URI
										}
										fbox_end(b, tenc);
									}
									fbox_end(b, schi);
								}
								fbox_end(b, sinf);
							}
							fbox_end(b, entry);
						}
						fbox_end(b, stsd);
					}
					fbox_end(b, stbl);
				}
				fbox_end(b, minf);
			}
			fbox_end(b, mdia);
		}
		fbox_end(b, trak);
	}
	fbox_end(b, moov);
}

// One media segment: a plain sample guarded by description 0 (20 bytes, no
// subsamples), then two samples guarded by description 1 (36 bytes each, one
// subsample of clear=4 + enc=32, one IV of 8 bytes). Two moofs, one mdat
// each, because one traf carries one description. The trun data offsets are
// filled in after the mdats they point at are in place.
static void build_segment(fbuf_t *b, const uint8_t *plain20, const uint8_t *c1,
						  const uint8_t *c2) {
	size_t do1 = 0, moof1 = 0;
	if (plain20) {
		moof1 = fbox(b, "moof");
		{
			size_t traf = fbox(b, "traf");
			{
				size_t tfhd = fbox(b, "tfhd");
				fu32(b, 0x000002); // sample_description_index follows
				fu32(b, 1);		   // track id
				fu32(b, 1);		   // description 1 (= index 0)
				fbox_end(b, tfhd);
				size_t trun = fbox(b, "trun");
				fu32(b, 0x00000301); // data offset, duration, size
				fu32(b, 1);
				do1 = b->len;
				fu32(b, 0); // data offset, patched below
				fu32(b, 1024);
				fu32(b, 20);
				fbox_end(b, trun);
			}
			fbox_end(b, traf);
		}
		fbox_end(b, moof1);
		size_t mdat = fbox(b, "mdat");
		fpush(b, plain20, 20);
		fbox_end(b, mdat);
		write32(b, do1, (uint32_t)(mdat + 8 - moof1));
	}
	size_t moof2 = fbox(b, "moof");
	{
		size_t traf = fbox(b, "traf");
		{
			size_t tfhd = fbox(b, "tfhd");
			fu32(b, 0x000002);
			fu32(b, 1);
			fu32(b, 2); // description 2 (= index 1, the cbcs one)
			fbox_end(b, tfhd);
			size_t trun = fbox(b, "trun");
			fu32(b, 0x00000305); // + first sample flags
			fu32(b, 2);
			fu32(b, 0); // data offset, patched below
			fu32(b, 0); // first sample flags
			fu32(b, 1024);
			fu32(b, 36);
			fu32(b, 1024);
			fu32(b, 36);
			fbox_end(b, trun);
			size_t senc = fbox(b, "senc");
			fu32(b, 0x000002); // subsample encrypted
			fu32(b, 2);
			for (int i = 0; i < 2; i++) {
				for (int j = 0; j < 8; j++) {
					fu8(b, (uint8_t)(0x10 * (i + 1) + j)); // an 8-byte IV
				}
				fu16(b, 1);  // one subsample
				fu16(b, 4);  // clear bytes
				fu32(b, 32); // encrypted bytes
			}
			fbox_end(b, senc);
		}
		fbox_end(b, traf);
	}
	fbox_end(b, moof2);
	size_t mdat = fbox(b, "mdat");
	fpush(b, c1, 36);
	fpush(b, c2, 36);
	fbox_end(b, mdat);
	// the cbcs data offset, found the way the parser walks: traf, tfhd, trun,
	// whose data offset field sits eight bytes into the trun payload
	{
		size_t at = moof2 + 8; // the traf box header
		size_t tfhd = at + 8;
		uint32_t fsz = ((uint32_t)b->buf[tfhd] << 24) | ((uint32_t)b->buf[tfhd + 1] << 16) |
					   ((uint32_t)b->buf[tfhd + 2] << 8) | b->buf[tfhd + 3];
		size_t trun = tfhd + fsz;
		write32(b, trun + 16, (uint32_t)(mdat + 8 - moof2));
	}
}

// ---------------------------------------------------------------------------
// the fake wrapper HTTP daemon and the fake decrypt peer
// ---------------------------------------------------------------------------

static fbuf_t g_init, g_seg1, g_seg2;
static const char *g_master = "#EXTM3U\n"
							  "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=256000,AUDIO=\"audio-stereo-256\"\n"
							  "variant.m3u8\n";
static const char *g_variant = "#EXTM3U\n"
							   "#EXT-X-MAP:URI=\"init.mp4\"\n"
							   "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://itunes.apple.com/k1\","
							   "KEYFORMAT=\"com.apple.streamingkeydelivery\"\n"
							   "#EXTINF:3.0,\n"
							   "seg1.mp4\n"
							   "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://itunes.apple.com/k2\","
							   "KEYFORMAT=\"com.apple.streamingkeydelivery\"\n"
							   "#EXTINF:3.0,\n"
							   "seg2.mp4\n";

static int g_http_port;

// ---------------------------------------------------------------------------
// the playback-dispatch stand-in
// ---------------------------------------------------------------------------

// The real amplocal_playback drives the guest daemon; the test cannot have
// one. This strong definition overrides the weak one in amplocal.c and asks
// the mock HTTP server the same question the wrapper client used to.
bool amplocal_playback(const char *adam_id, char **body, size_t *body_len, char *error,
					   size_t error_size) {
	char url[128];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/playback?adam_id=%s", g_http_port, adam_id);
	http_req_t req = {.method = "GET", .no_https_downgrade = true};
	int status = 0;
	if (!http_request(url, &req, body, body_len, 1024 * 1024, 10, &status) || status != 200) {
		snprintf(error, error_size, "amp_playback_failed");
		return false;
	}
	return true;
}

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
		bool whole = false;
		while (!whole && got < sizeof(req) - 1) {
			ssize_t r = read(fd, req + got, sizeof(req) - 1 - got);
			if (r <= 0) {
				break;
			}
			got += (size_t)r;
			req[got] = '\0';
			char *end = strstr(req, "\r\n\r\n");
			if (end) {
				whole = true; // the tests never send a body
			}
		}
		const char *text = NULL;
		const uint8_t *bin = NULL;
		size_t blen = 0;
		int status = 200;
		if (strstr(req, "GET /playback?adam_id=4401234567 ")) {
			text = "{\"songList\":[{\"songId\":\"4401234567\","
				   "\"hls-playlist-url\":\"http://127.0.0.1:PLACEHOLDER/master.m3u8\"}]}";
		} else if (strstr(req, "GET /master.m3u8 ")) {
			text = g_master;
		} else if (strstr(req, "GET /variant.m3u8 ")) {
			text = g_variant;
		} else if (strstr(req, "GET /init.mp4 ")) {
			bin = g_init.buf;
			blen = g_init.len;
		} else if (strstr(req, "GET /seg1.mp4 ")) {
			bin = g_seg1.buf;
			blen = g_seg1.len;
		} else if (strstr(req, "GET /seg2.mp4 ")) {
			bin = g_seg2.buf;
			blen = g_seg2.len;
		} else {
			status = 404;
		}
		if (!text) {
			text = "";
		}
		// the playback JSON needs the daemon's own port inside the playlist
		// URL; rewrite the placeholder from the listening socket's port
		char body[1024];
		if (strstr(text, "PLACEHOLDER")) {
			char port[8];
			snprintf(port, sizeof(port), "%d", g_http_port);
			const char *cut = strstr(text, "PLACEHOLDER");
			snprintf(body, sizeof(body), "%.*s%s%s", (int)(cut - text), text, port,
					 cut + strlen("PLACEHOLDER"));
			text = body;
		}
		size_t n = bin ? blen : strlen(text);
		char reply_head[384];
		int hn = snprintf(reply_head, sizeof(reply_head),
						  "HTTP/1.1 %d %s\r\nContent-Type: application/octet-stream\r\n"
						  "Content-Length: %zu\r\nConnection: close\r\n\r\n",
						  status, status == 200 ? "OK" : "Not Found", n);
		(void)!write(fd, reply_head, (size_t)hn);
		if (bin) {
			size_t sent = 0;
			while (sent < blen) {
				ssize_t w = write(fd, bin + sent, blen - sent);
				if (w <= 0) {
					break;
				}
				sent += (size_t)w;
			}
		} else {
			(void)!write(fd, text, strlen(text));
		}
		close(fd);
	}
	return NULL;
}

// The WV2D peer: plaintext = ciphertext ^ 0xff, one batch at a time, each
// batch's key URI and adam id remembered for the assertions below.
typedef struct {
	char uri[128];
	char adam[32];
	int count;
} batch_log_t;
static batch_log_t g_batches[8];
static int g_batch_count;

static void *decrypt_serve(void *arg) {
	int lfd = (int)(intptr_t)arg;
	int fd = accept(lfd, NULL, NULL);
	if (fd < 0) {
		return NULL;
	}
	for (;;) {
		uint8_t head[16];
		size_t got = 0;
		while (got < sizeof(head)) {
			ssize_t r = read(fd, head + got, sizeof(head) - got);
			if (r <= 0) {
				close(fd);
				return NULL;
			}
			got += (size_t)r;
		}
		uint32_t magic = ((uint32_t)head[0] << 24) | ((uint32_t)head[1] << 16) |
						 ((uint32_t)head[2] << 8) | head[3];
		uint16_t kind = (uint16_t)((head[6] << 8) | head[7]);
		uint32_t id = ((uint32_t)head[8] << 24) | ((uint32_t)head[9] << 16) |
					  ((uint32_t)head[10] << 8) | head[11];
		size_t plen = ((size_t)head[12] << 24) | ((size_t)head[13] << 16) |
					  ((size_t)head[14] << 8) | head[15];
		uint8_t *payload = malloc(plen ? plen : 1);
		size_t have = 0;
		while (have < plen) {
			ssize_t r = read(fd, payload + have, plen - have);
			if (r <= 0) {
				free(payload);
				close(fd);
				return NULL;
			}
			have += (size_t)r;
		}
		if (magic != 0x57563244u) {
			free(payload);
			close(fd);
			return NULL;
		}
		if (kind == 9) { // close
			free(payload);
			close(fd);
			return NULL;
		}
		if (kind != 1) {
			free(payload);
			continue;
		}
		uint16_t adam_len = (uint16_t)((payload[0] << 8) | payload[1]);
		uint16_t skd_len = (uint16_t)((payload[2] << 8) | payload[3]);
		uint32_t count = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
						 ((uint32_t)payload[6] << 8) | payload[7];
		const uint8_t *table = payload + 8;
		const uint8_t *labels = table + (size_t)count * 4;
		const uint8_t *data = labels + adam_len + skd_len;
		if (g_batch_count < (int)(sizeof(g_batches) / sizeof(g_batches[0]))) {
			batch_log_t *log = &g_batches[g_batch_count++];
			snprintf(log->adam, sizeof(log->adam), "%.*s", adam_len, (const char *)labels);
			snprintf(log->uri, sizeof(log->uri), "%.*s", skd_len,
					 (const char *)labels + adam_len);
			log->count = (int)count;
		}
		size_t rlen = 4 + (size_t)count * 4;
		for (uint32_t i = 0; i < count; i++) {
			size_t n = ((size_t)table[i * 4] << 24) | ((size_t)table[i * 4 + 1] << 16) |
					   ((size_t)table[i * 4 + 2] << 8) | table[i * 4 + 3];
			rlen += n;
		}
		uint8_t *reply = malloc(16 + rlen);
		size_t at = 16;
		memcpy(reply, "WV2D", 4);
		reply[4] = 0;
		reply[5] = 1;
		reply[6] = 0;
		reply[7] = 2; // ok
		reply[8] = (uint8_t)(id >> 24);
		reply[9] = (uint8_t)(id >> 16);
		reply[10] = (uint8_t)(id >> 8);
		reply[11] = (uint8_t)id;
		reply[12] = (uint8_t)(rlen >> 24);
		reply[13] = (uint8_t)(rlen >> 16);
		reply[14] = (uint8_t)(rlen >> 8);
		reply[15] = (uint8_t)rlen;
		reply[at++] = (uint8_t)(count >> 24);
		reply[at++] = (uint8_t)(count >> 16);
		reply[at++] = (uint8_t)(count >> 8);
		reply[at++] = (uint8_t)count;
		for (uint32_t i = 0; i < count; i++) {
			size_t n = ((size_t)table[i * 4] << 24) | ((size_t)table[i * 4 + 1] << 16) |
					   ((size_t)table[i * 4 + 2] << 8) | table[i * 4 + 3];
			reply[at++] = (uint8_t)(n >> 24);
			reply[at++] = (uint8_t)(n >> 16);
			reply[at++] = (uint8_t)(n >> 8);
			reply[at++] = (uint8_t)n;
		}
		for (uint32_t i = 0; i < count; i++) {
			size_t n = ((size_t)table[i * 4] << 24) | ((size_t)table[i * 4 + 1] << 16) |
					   ((size_t)table[i * 4 + 2] << 8) | table[i * 4 + 3];
			for (size_t j = 0; j < n; j++) {
				reply[at++] = (uint8_t)(data[j] ^ 0xff);
			}
			data += n;
		}
		size_t sent = 0;
		while (sent < 16 + rlen) {
			ssize_t w = write(fd, reply + sent, 16 + rlen - sent);
			if (w <= 0) {
				break;
			}
			sent += (size_t)w;
		}
		free(reply);
		free(payload);
	}
}

// ---------------------------------------------------------------------------
// the tests
// ---------------------------------------------------------------------------

static ampapi_song_t g_tracks[2];
static char g_paths[2][AMP_PATH_MAX];
static const char *g_pointers[2];

static void load_page(void) {
	memset(g_tracks, 0, sizeof(g_tracks));
	snprintf(g_tracks[0].id, sizeof(g_tracks[0].id), "4401234567");
	snprintf(g_tracks[0].title, sizeof(g_tracks[0].title), "Test Song");
	snprintf(g_tracks[0].artist, sizeof(g_tracks[0].artist), "Test Artist");
	snprintf(g_tracks[0].album, sizeof(g_tracks[0].album), "Test Album");
	g_tracks[0].duration_ms = 6100;
	snprintf(g_tracks[1].id, sizeof(g_tracks[1].id), "8800000002");
	snprintf(g_tracks[1].title, sizeof(g_tracks[1].title), "Second");
	snprintf(g_tracks[1].artist, sizeof(g_tracks[1].artist), "Second Artist");
	snprintf(g_tracks[1].album, sizeof(g_tracks[1].album), "Second Album");
	g_tracks[1].duration_ms = 200000;
	char error[192] = "";
	int queued = 0, selected = 0;
	assert(ampstream_queue(g_tracks, 2, 0, g_paths, g_pointers, &queued, &selected, error,
						   sizeof(error)));
	assert(queued == 2 && selected == 0);
}

int main(void) {
	// ---- the fixtures and the fakes ----
	build_init(&g_init);
	uint8_t d0[20], c1[36], c2[36], c3[36], c4[36];
	for (int i = 0; i < 20; i++) {
		d0[i] = (uint8_t)(0xa0 + i);
	}
	for (int i = 0; i < 36; i++) {
		c1[i] = (uint8_t)(0xb0 + i);
		c2[i] = (uint8_t)(0xc0 + i);
		c3[i] = (uint8_t)(0xd0 + i);
		c4[i] = (uint8_t)(0xe0 + i);
	}
	build_segment(&g_seg1, d0, c1, c2);
	build_segment(&g_seg2, NULL, c3, c4);
	assert(g_init.len < sizeof(g_init.buf));
	assert(g_seg1.len < sizeof(g_seg1.buf));

	int http_port = 0;
	int http_fd = listen_port(&http_port);
	assert(http_fd >= 0);
	g_http_port = http_port;
	pthread_t http_thread;
	pthread_create(&http_thread, NULL, http_serve, (void *)(intptr_t)http_fd);

	int dec_port = 0;
	int dec_fd = listen_port(&dec_port);
	assert(dec_fd >= 0);
	pthread_t dec_thread;
	pthread_create(&dec_thread, NULL, decrypt_serve, (void *)(intptr_t)dec_fd);

	// ---- the configuration the source reads at open ----
	config_set_int("applemusic", "translate", 1);
	config_set_int("applemusic", "decrypt_port", dec_port);
	config_set("applemusic", "storefront", "jp");

	// ---- the queue management ----
	assert(!ampstream_owns(NULL));
	assert(!ampstream_owns("subsonic://1/0.mp3"));
	assert(!ampstream_owns("http://example.com/x.aac"));
	assert(ampstream_owns("amp://1/0.aac"));

	load_page();
	assert(ampstream_owns(g_paths[0]));
	assert(strstr(g_paths[0], "amp://") == g_paths[0]);
	assert(strstr(g_paths[0], ".aac"));
	assert(g_pointers[0] == g_paths[0]);

	song_metadata_t md;
	assert(ampstream_metadata(g_paths[1], &md));
	assert(strcmp(md.title, "Second") == 0);
	assert(strcmp(md.artist, "Second Artist") == 0);
	assert(strcmp(md.album, "Second Album") == 0);
	assert(md.has_tags);

	// ---- the pipeline: the whole producer, end to end ----
	ampstream_prepare(g_paths[0], sizeof(g_paths[0]));
	int duration = -1, bitrate = -1;
	streambridge_t *bridge = ampstream_acquire(g_paths[0], &duration, &bitrate);
	assert(bridge);
	assert(duration == 6); // 6100 ms, in whole seconds
	assert(bitrate == 0);  // the producer picks the variant, not the queue

	if (!streambridge_prime(bridge)) {
		streambridge_state_t dbg;
		streambridge_state(bridge, &dbg);
		fprintf(stderr, "prime failed: opened=%d eof=%d failed=%d canceled=%d error=%s\n",
				dbg.opened, dbg.eof, dbg.failed, dbg.canceled, dbg.error);
		char err2[192];
		ampstream_take_error(err2, sizeof(err2));
		fprintf(stderr, "take_error: %s\n", err2);
		assert(0);
	}
	uint8_t all[65536];
	size_t have = 0;
	for (;;) {
		size_t got = streambridge_read(bridge, all + have, sizeof(all) - have);
		if (got == 0) {
			break;
		}
		have += got;
		assert(have < sizeof(all));
	}
	assert(!streambridge_failed(bridge));

	// Five ADTS frames: one plain (description 0), two wrapped batches of two
	// (description 1, one batch per key URI). Each payload is the reassembled
	// sample: the clear bytes as they were, the encrypted ones XOR-flipped by
	// the fake peer.
	size_t at = 0;
	int frames = 0;
	while (at + 7 <= have) {
		unsigned frame = ((all[at + 3] & 0x03) << 11) | ((unsigned)all[at + 4] << 3) |
						 (all[at + 5] >> 5);
		assert(all[at] == 0xff && all[at + 1] == 0xf1);
		assert(at + frame <= have);
		assert(frame > 7);
		const uint8_t *payload = all + at + 7;
		size_t plen = frame - 7;
		if (frames == 0) {
			assert(plen == 20);
			assert(memcmp(payload, d0, 20) == 0);
		} else if (frames == 1 || frames == 3) {
			const uint8_t *c = frames == 1 ? c1 : c3;
			assert(plen == 36);
			assert(memcmp(payload, c, 4) == 0);
			for (int i = 4; i < 36; i++) {
				assert(payload[i] == (uint8_t)(c[i] ^ 0xff));
			}
		} else if (frames == 2 || frames == 4) {
			const uint8_t *c = frames == 2 ? c2 : c4;
			assert(plen == 36);
			assert(memcmp(payload, c, 4) == 0);
			for (int i = 4; i < 36; i++) {
				assert(payload[i] == (uint8_t)(c[i] ^ 0xff));
			}
		} else {
			assert(0); // more frames than the segments hold
		}
		at += frame;
		frames++;
	}
	assert(frames == 5);
	assert(at == have);

	// The frames landed in the ADTS shape the init segment describes: AAC-LC,
	// 44100 Hz, two channels.
	assert(all[2] == (unsigned char)((1 << 6) | (4 << 2))); // profile 1, rate index 4
	assert((all[3] >> 6) == 2);								// channel configuration

	// Two batches went out, one per key URI, both for this adam id.
	assert(g_batch_count == 2);
	assert(strcmp(g_batches[0].uri, "skd://itunes.apple.com/k1") == 0);
	assert(strcmp(g_batches[1].uri, "skd://itunes.apple.com/k2") == 0);
	assert(strcmp(g_batches[0].adam, "4401234567") == 0);
	assert(g_batches[0].count == 2 && g_batches[1].count == 2);

	ampstream_release(bridge);

	// ---- the queue: a second page invalidates the old names ----
	load_page(); // a new generation
	char stale[AMP_PATH_MAX];
	snprintf(stale, sizeof(stale), "%s", g_paths[0]); // keep an old name
	load_page();
	song_metadata_t md2;
	assert(ampstream_metadata(stale, &md2)); // still an amp name...
	assert(!md2.title[0]);					 // ...but no track behind it

	// ---- teardown ----
	ampstream_reset();
	shutdown(http_fd, SHUT_RDWR);
	close(http_fd);
	pthread_join(http_thread, NULL);
	shutdown(dec_fd, SHUT_RDWR);
	close(dec_fd);
	pthread_join(dec_thread, NULL);
	printf("ampstream tests passed\n");
	return 0;
}
