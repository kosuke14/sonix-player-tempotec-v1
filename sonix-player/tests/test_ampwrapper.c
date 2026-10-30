// The WV2D decrypt client against a local fake: a TCP peer that speaks the
// framing back (plaintext = ciphertext + 1, which the test knows to expect).
// No network beyond loopback, no Apple anything -- what is under test is the
// client's framing, table handling and error mapping.
#include "src/system/streaming/ampwrapper.h"

#include <assert.h>
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

// The WV2D peer: reads one batch frame, answers plaintexts that are the
// ciphertexts rotated by one byte -- proof of a faithful round trip.
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
		size_t plen = ((size_t)head[12] << 24) | ((size_t)head[13] << 16) | ((size_t)head[14] << 8) |
					  head[15];
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

		// Reply: count, lengths, then each plaintext one byte over.
		size_t rlen = 4 + (size_t)count * 4;
		for (uint32_t i = 0; i < count; i++) {
			size_t n = ((size_t)table[i * 4] << 24) | ((size_t)table[i * 4 + 1] << 16) |
					   ((size_t)table[i * 4 + 2] << 8) | table[i * 4 + 3];
			rlen += n;
		}
		uint8_t *reply = malloc(16 + rlen);
		size_t at = 16;
		reply[0] = 'W';
		reply[1] = 'V';
		reply[2] = '2';
		reply[3] = 'D';
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
		// The length table is complete before any plaintext: the client reads
		// it that way, and so does the real daemon.
		for (uint32_t i = 0; i < count; i++) {
			size_t n = ((size_t)table[i * 4] << 24) | ((size_t)table[i * 4 + 1] << 16) |
					   ((size_t)table[i * 4 + 2] << 8) | table[i * 4 + 3];
			for (size_t j = 0; j < n; j++) {
				reply[at++] = (uint8_t)(data[j] + 1);
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

int main(void) {
	char error[192] = "";

	// ---- the TCP decrypt side ----
	int dec_port = 0;
	int dec_fd = listen_port(&dec_port);
	{
		struct sockaddr_in a;
		socklen_t l = sizeof(a);
		getsockname(dec_fd, (struct sockaddr *)&a, &l);
		dec_port = ntohs(a.sin_port);
	}
	pthread_t dec_thread;
	pthread_create(&dec_thread, NULL, decrypt_serve, (void *)(intptr_t)dec_fd);

	ampw_decrypt_t *dec = ampw_decrypt_open("127.0.0.1", dec_port, NULL, NULL);
	assert(dec);

	// Three samples, sizes that exercise the table and the back-to-back body:
	// 16 (one block), 1 (sub-block), 48 (three blocks).
	const uint8_t *samples[3] = {(const uint8_t *)"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10",
								 (const uint8_t *)"\x20",
								 (const uint8_t *)"0123456789abcdef0123456789abcdef0123456789abcdef"};
	size_t lens[3] = {16, 1, 48};
	uint8_t *plains[3];
	size_t plains_len[3];
	bool batched = ampw_decrypt_batch(dec, "12345", "skd://itunes.example/key", samples, lens, 3,
									  plains, plains_len, error, sizeof(error));
	if (!batched) {
		fprintf(stderr, "batch error: %s\n", error);
	}
	assert(batched);
	fprintf(stderr, "BATCH_OK\n");
	for (int i = 0; i < 3; i++) {
		assert(plains_len[i] == lens[i]);
		for (size_t j = 0; j < lens[i]; j++) {
			assert(plains[i][j] == (uint8_t)(samples[i][j] + 1));
		}
		free(plains[i]);
	}

	// The close frame goes out on close; the peer exits its loop.
	fprintf(stderr, "CLOSING\n");
	ampw_decrypt_close(dec);
	pthread_join(dec_thread, NULL);
	fprintf(stderr, "JOINED\n");

	printf("ampwrapper tests passed\n");
	return 0;
}
