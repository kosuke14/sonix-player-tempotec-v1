#include "ampstream.h"

#include "amplocal.h"

#include "amphls.h"
#include "ampfp4.h"
#include "ampwrapper.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/net/http.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The producer half of Apple Music playback. One run_source() behind each
// bridge owns the whole pipeline: the playback dispatch (via the on-device
// guest daemon), the HLS walk (amphls.c), the fMP4 slicing (ampfp4.c) and
// the decryption, either locally for the public-key prefetch samples or in
// batches through the local listener's TCP door (ampwrapper.c). What lands
// in the ring is plain ADTS AAC, the one thing the player's existing decoder
// accepts.

// One media segment is a few seconds of AAC -- a few hundred kilobytes. The
// same scratch carries the playlists and the init segment on the way in.
#define AMP_SCRATCH_SIZE (2 * 1024 * 1024)
// The reassembly output a batch may claim. The bridge reads from here, so a
// batch never starts unless its whole ADTS output fits. This also bounds the
// protected-range storage: the ciphertext handed to the wrapper is never
// larger than the frames it will come back as.
#define AMP_EMIT_SIZE (256 * 1024)
// The wrapper's per-request sample ceiling (the WV2D frame budget).
#define AMP_BATCH_MAX 128
// The ADTS frame length field is thirteen bits; a larger sample cannot be
// carried no matter how it was sliced.
#define AMP_FRAME_MAX 8191
#define AMP_TIMEOUT 20

// The prefetch key: description 0 of an enhanced-HLS stream is guarded with a
// key Apple publishes to every client, so those samples -- the first of the
// stream, fetched before the wrapper round-trip warms up -- decrypt here.
static const unsigned char AMP_PREFETCH_KEY[16] = {0x32, 0xb8, 0xad, 0xe1, 0x76, 0x9e,
												   0x26, 0xb1, 0xff, 0xb8, 0x98, 0x63,
												   0x52, 0x79, 0x3f, 0xc6};

// ---------------------------------------------------------------------------
// the source state: everything one start of one track owns
// ---------------------------------------------------------------------------

typedef struct {
	char adam_id[32];
	int decrypt_port;

	amphls_playlist_t list;
	ampfp4_init_t init;
	ampw_decrypt_t *dec;

	uint8_t *scratch; // one segment, and the playlists before it
	uint8_t *concat;  // the batch's protected ranges, concatenated
	uint8_t *emit;	// reassembled ADTS, drained by the bridge
	size_t emit_len;
	size_t concat_used;

	int seg_next;	  // the next segment to fetch
	char seg_key[512]; // the FairPlay key URI of the loaded segment

	ampfp4_fragment_t frag;
	int frag_index;

	struct {
		char uri[512];
		int count;
		size_t out_bytes; // the ADTS the batch will emit, for the emit bound
		struct {
			ampfp4_cbcs_t cbc;
			int sample_index;
		} items[AMP_BATCH_MAX];
	} batch;
} amp_source_t;

// ---------------------------------------------------------------------------
// small pieces
// ---------------------------------------------------------------------------

static void adts_header(unsigned char *h, int frame_len, const ampfp4_init_t *init) {
	// The seven-byte ADTS header, MPEG-4 without CRC: profile, rate index and
	// channel layout straight from the AudioSpecificConfig, this frame's
	// length, and the VBR marker as the buffer fullness.
	int profile = init->audio_object_type - 1;
	int index = ((init->asc[0] & 0x07) << 1) | (init->asc[1] >> 7);
	int ch = (init->asc[1] >> 3) & 0x0f;
	if (!ch) {
		ch = 2;
	}
	h[0] = 0xff;
	h[1] = 0xf1; // MPEG-4, layer 0, protection absent
	h[2] = (unsigned char)(((profile & 0x03) << 6) | ((index & 0x0f) << 2) | ((ch >> 2) & 0x01));
	h[3] = (unsigned char)(((ch & 0x03) << 6) | ((frame_len >> 11) & 0x03));
	h[4] = (unsigned char)((frame_len >> 3) & 0xff);
	h[5] = (unsigned char)(((frame_len & 0x07) << 5) | 0x1f); // fullness 0x7ff, high five bits
	h[6] = 0xf8; // ... low six bits, one raw data block
}

// The host part of the configured wrapper base, for the TCP decrypt door.
// The HTTP port the base carries is the daemon's own; the decrypt port is
// configured separately and may differ.
// songList[0]'s master playlist URL out of the playback dispatch.
static bool playback_url(const char *body, char *url, size_t size, char *error, size_t error_size) {
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		snprintf(error, error_size, "amp_playback_failed");
		return false;
	}
	int songs = json_path(&doc, json_root(&doc), "songList");
	int first = songs >= 0 ? json_at(&doc, songs, 0) : -1;
	bool ok = first >= 0 && json_obj_str(&doc, first, "hls-playlist-url", url, size) && url[0];
	if (!ok) {
		snprintf(error, error_size, "amp_playback_failed");
	}
	json_free(&doc);
	return ok;
}

// ---------------------------------------------------------------------------
// fetching: every network step goes through the bridge's socket hook, so a
// canceled stream breaks a recv in progress instead of waiting out a timeout
// ---------------------------------------------------------------------------

// ampw_decrypt_open polls this between socket waits.
static bool decrypt_stale(void *user) {
	streambridge_source_t *src = user;
	return src->stale(src);
}

static bool fetch_text(streambridge_source_t *src, const char *url, uint8_t *buf, size_t cap,
					   size_t *out_len, char *error, size_t error_size) {
	http_stream_t st;
	http_stream_options_t opts = {
		.socket_cb = src->sock_cb, .socket_user = src->sock_user, .no_icy = true,
		.no_https_downgrade = true};
	if (!http_stream_open_ex(&st, url, AMP_TIMEOUT, &opts)) {
		snprintf(error, error_size, "%s", http_last_error() ? http_last_error() : "amp_fetch_failed");
		return false;
	}
	if (st.content_length64 > cap - 1) {
		http_stream_close(&st);
		snprintf(error, error_size, "amp_fetch_too_large");
		return false;
	}
	size_t got = 0;
	for (;;) {
		int n = http_stream_read(&st, buf + got, (int)(cap - 1 - got));
		if (n < 0) {
			http_stream_close(&st);
			snprintf(error, error_size, "%s", http_last_error() ? http_last_error() : "amp_fetch_failed");
			return false;
		}
		if (n == 0) {
			break;
		}
		got += (size_t)n;
		if (got >= cap - 1) {
			http_stream_close(&st);
			snprintf(error, error_size, "amp_fetch_too_large");
			return false;
		}
	}
	http_stream_close(&st);
	buf[got] = '\0';
	*out_len = got;
	return true;
}

// ---------------------------------------------------------------------------
// the batch: one key URI's worth of ciphertext, sent in one WV2D round trip
// ---------------------------------------------------------------------------

// Writes the batch's reassembled ADTS frames into the emit buffer. Runs only
// between read() calls, when the bridge has drained emit: the batch bound
// (out_bytes) keeps its output inside the empty buffer's room.
static int flush_batch(amp_source_t *st, char *error, size_t error_size) {
	if (!st->batch.count) {
		return 1;
	}
	if (!st->dec) {
		snprintf(error, error_size, "amp_decrypt_closed");
		return -1;
	}
	const uint8_t *bufs[AMP_BATCH_MAX];
	size_t lens[AMP_BATCH_MAX];
	uint8_t *plains[AMP_BATCH_MAX];
	size_t plain_lens[AMP_BATCH_MAX];
	for (int i = 0; i < st->batch.count; i++) {
		bufs[i] = st->batch.items[i].cbc.aligned;
		lens[i] = st->batch.items[i].cbc.aligned_len;
	}
	if (!ampw_decrypt_batch(st->dec, st->adam_id, st->batch.uri, bufs, lens, st->batch.count,
							plains, plain_lens, error, error_size)) {
		return -1;
	}
	for (int i = 0; i < st->batch.count; i++) {
		const ampfp4_sample_t *s = &st->frag.samples[st->batch.items[i].sample_index];
		if (st->emit_len + 7 + s->size > AMP_EMIT_SIZE) { // unreachable under the batch bound
			free(plains[i]);
			snprintf(error, error_size, "amp_emit_overflow");
			return -1;
		}
		ampfp4_reassemble(s, plains[i], plain_lens[i], st->batch.items[i].cbc.tail,
						  st->batch.items[i].cbc.tail_len, st->emit + st->emit_len + 7);
		adts_header(st->emit + st->emit_len, (int)(7 + s->size), &st->init);
		st->emit_len += 7 + s->size;
		free(plains[i]);
	}
	st->batch.count = 0;
	st->batch.out_bytes = 0;
	st->concat_used = 0;
	return 1;
}

// One sample: either emitted now (locally decrypted, plain, or nothing
// protected) or folded into the open batch. Returns 1 after progress, 0 for
// the clean end of the stream, -1 on failure with the message filled.
static int process_sample(amp_source_t *st, char *error, size_t error_size) {
	ampfp4_sample_t *s = &st->frag.samples[st->frag_index];
	if (s->size > AMP_FRAME_MAX - 7) {
		snprintf(error, error_size, "amp_frame_too_large");
		return -1;
	}
	if (s->desc_index == 0) {
		// The public-key prefetch sample, decrypted on this side. A plain
		// description means the bytes already are the frame.
		const ampfp4_init_t *init = &st->init;
		if (init->desc[0].scheme_type[0]) {
			size_t plain_len = 0;
			if (!ampfp4_decrypt_local(s, AMP_PREFETCH_KEY, init->desc[0].constant_iv,
									  init->desc[0].constant_iv_len,
									  strcmp(init->desc[0].scheme_type, "cenc") == 0,
									  init->desc[0].crypt_byte_block, init->desc[0].skip_byte_block,
									  st->emit + 7, AMP_EMIT_SIZE - 7, &plain_len)) {
				snprintf(error, error_size, "amp_decrypt_local_failed");
				return -1;
			}
			adts_header(st->emit, (int)(7 + plain_len), init);
			st->emit_len = 7 + plain_len;
		} else {
			memcpy(st->emit + 7, s->data, s->size);
			adts_header(st->emit, (int)(7 + s->size), init);
			st->emit_len = 7 + s->size;
		}
		st->frag_index++;
		return 1;
	}
	// One batch speaks one key URI: a key change ends the open batch first.
	if (st->batch.count && strcmp(st->batch.uri, st->seg_key) != 0) {
		if (flush_batch(st, error, error_size) < 0) {
			return -1;
		}
		return 1; // the read loop drains, then this sample comes around again
	}
	size_t frame = 7 + s->size;
	if (st->batch.count >= AMP_BATCH_MAX || st->batch.out_bytes + frame > AMP_EMIT_SIZE ||
		st->concat_used + s->size > AMP_EMIT_SIZE) {
		if (flush_batch(st, error, error_size) < 0) {
			return -1;
		}
		return 1;
	}
	ampfp4_cbcs_t cbc;
	if (!ampfp4_cbcs_split(s, &cbc)) {
		// Nothing for the wrapper: the bytes are the frame already.
		memcpy(st->emit + 7, s->data, s->size);
		adts_header(st->emit, (int)frame, &st->init);
		st->emit_len = frame;
		st->frag_index++;
		return 1;
	}
	if (st->batch.count == 0) {
		snprintf(st->batch.uri, sizeof(st->batch.uri), "%s", st->seg_key);
	}
	if (!cbc.aligned) {
		// Subsampled: the protected ranges go to the batch concatenated, so
		// the unaligned end of the ciphertext -- this sample's tail -- rides
		// at the end of that storage.
		size_t total = 0;
		for (int i = 0; i < s->sub_count; i++) {
			total += (size_t)s->sub[i].enc;
		}
		ampfp4_concatenate_protected(s, st->concat + st->concat_used, total);
		cbc.aligned = st->concat + st->concat_used;
		cbc.tail = st->concat + st->concat_used + cbc.aligned_len;
		st->concat_used += total;
	}
	st->batch.items[st->batch.count].cbc = cbc;
	st->batch.items[st->batch.count].sample_index = st->frag_index;
	st->batch.out_bytes += frame;
	st->batch.count++;
	st->frag_index++;
	return 1; // nothing emitted yet; a later flush fills the emit buffer
}

// Loads the next segment into the scratch and slices it. True on success,
// false with the message filled on any failure.
static bool fetch_segment(amp_source_t *st, streambridge_source_t *src, char *error,
						  size_t error_size) {
	const char *key = amphls_key_for(&st->list, st->seg_next);
	if (!key || !key[0]) {
		snprintf(error, error_size, "amp_segment_unkeyed");
		return false;
	}
	if (src->stale(src)) {
		snprintf(error, error_size, "canceled");
		return false;
	}
	size_t len = 0;
	if (!fetch_text(src, st->list.segments[st->seg_next].url, st->scratch, AMP_SCRATCH_SIZE, &len,
					error, error_size)) {
		return false;
	}
	ampfp4_fragment_t frag;
	if (!ampfp4_parse_fragment(st->scratch, len, (uint32_t)st->init.track_id, &st->init, &frag,
							   error, error_size)) {
		return false;
	}
	st->frag = frag; // the previous fragment was freed before this fetch
	st->frag_index = 0;
	snprintf(st->seg_key, sizeof(st->seg_key), "%s", key);
	st->seg_next++;
	return true;
}

// One step of production: a sample, a batch flush, or a segment fetch. The
// call returns with emit either holding output or the stream at its end --
// the read loop drains emit before it comes back here.
static int produce_step(amp_source_t *st, streambridge_source_t *src, char *error,
						size_t error_size) {
	for (;;) {
		if (st->frag.count && st->frag_index < st->frag.count) {
			int r = process_sample(st, error, error_size);
			if (r <= 0) {
				return r;
			}
			return 1; // progress: emit may hold output, a batch may hold work
		}
		if (st->frag.samples) {
			// The segment is exhausted: flush what the batch holds, whose
			// pointers still live in the scratch, before the next fetch
			// reuses it.
			if (flush_batch(st, error, error_size) < 0) {
				return -1;
			}
			ampfp4_free_fragment(&st->frag);
			st->frag_index = 0;
			if (st->emit_len) {
				return 1;
			}
		}
		if (st->seg_next >= st->list.count) {
			return 0; // clean end of stream
		}
		if (!fetch_segment(st, src, error, error_size)) {
			return -1;
		}
	}
}

// ---------------------------------------------------------------------------
// the bridge's view: open, read, close
// ---------------------------------------------------------------------------

static bool amp_source_open(streambridge_source_t *src, char *error, size_t error_size) {
	amp_source_t *st = src->user;
	st->scratch = malloc(AMP_SCRATCH_SIZE);
	st->concat = malloc(AMP_EMIT_SIZE);
	st->emit = malloc(AMP_EMIT_SIZE);
	if (!st->scratch || !st->concat || !st->emit) {
		snprintf(error, error_size, "amp_out_of_memory");
		return false;
	}
	st->decrypt_port = (int)config_get_int("applemusic", "decrypt_port", 10020);
	if (config_get_int("applemusic", "translate", 0) == 0) {
		// No guest tree configured: nothing on this device can run the Apple
		// libraries, so there is no playback dispatch and no decrypt door.
		snprintf(error, error_size, "amp_not_configured");
		return false;
	}

	// The playback dispatch, then the playlist walk: master, the AAC variant
	// (HE-AAC only as the fallback), the variant itself, the init segment.
	char *body = NULL;
	size_t body_len = 0;
	if (!amplocal_playback(st->adam_id, &body, &body_len, error, error_size)) {
		return false;
	}
	char master_url[640];
	bool got_url = playback_url(body, master_url, sizeof(master_url), error, error_size);
	free(body);
	if (!got_url) {
		return false;
	}
	size_t len = 0;
	if (!fetch_text(src, master_url, st->scratch, AMP_SCRATCH_SIZE, &len, error, error_size)) {
		return false;
	}
	char variant_url[640];
	if (!amphls_pick_variant((const char *)st->scratch, master_url, AMPHLS_CODEC_AAC, variant_url,
							 sizeof(variant_url)) &&
		!amphls_pick_variant((const char *)st->scratch, master_url, AMPHLS_CODEC_HE_AAC,
							 variant_url, sizeof(variant_url))) {
		snprintf(error, error_size, "amp_no_variant");
		return false;
	}
	if (!fetch_text(src, variant_url, st->scratch, AMP_SCRATCH_SIZE, &len, error, error_size)) {
		return false;
	}
	char variant_copy[640];
	snprintf(variant_copy, sizeof(variant_copy), "%s", variant_url);
	if (!amphls_parse_variant((const char *)st->scratch, variant_copy, &st->list, error,
							  error_size) ||
		st->list.count == 0) {
		snprintf(error, error_size, "%s", error[0] ? error : "amp_no_segments");
		return false;
	}
	if (!st->list.init_url[0]) {
		snprintf(error, error_size, "amp_no_init_segment");
		return false;
	}
	if (!fetch_text(src, st->list.init_url, st->scratch, AMP_SCRATCH_SIZE, &len, error,
					error_size) ||
		!ampfp4_parse_init(st->scratch, len, &st->init, error, error_size)) {
		return false;
	}
	if (st->init.asc_len < 2 || (((st->init.asc[0] & 0x07) << 1) | (st->init.asc[1] >> 7)) >= 13) {
		snprintf(error, error_size, "amp_bad_asc");
		return false;
	}
	// A segment without a FairPlay key cannot be decrypted at all -- refuse
	// the whole stream here rather than fail two thirds in.
	for (int i = 0; i < st->list.count; i++) {
		if (!amphls_key_for(&st->list, i)[0]) {
			snprintf(error, error_size, "amp_segment_unkeyed");
			return false;
		}
	}

	// Decrypt always runs on this device: the local listener (amplocal) owns
	// the decrypt port on loopback and drives the guest daemon.
	st->dec = ampw_decrypt_open("127.0.0.1", st->decrypt_port, decrypt_stale, src);
	if (!st->dec) {
		snprintf(error, error_size, "amp_decrypt_connect");
		return false;
	}
	return true;
}

static long amp_source_read(streambridge_source_t *src, void *out, size_t bytes, char *error,
							size_t error_size) {
	amp_source_t *st = src->user;
	uint8_t *dst = out;
	size_t written = 0;
	while (written < bytes) {
		if (st->emit_len) {
			size_t n = bytes - written < st->emit_len ? bytes - written : st->emit_len;
			memcpy(dst + written, st->emit, n);
			memmove(st->emit, st->emit + n, st->emit_len - n);
			st->emit_len -= n;
			written += n;
			continue;
		}
		int r = produce_step(st, src, error, error_size);
		if (r < 0) {
			return written ? (long)written : -1;
		}
		if (r == 0) {
			break; // clean end of stream
		}
	}
	return (long)written;
}

static void amp_source_close(streambridge_source_t *src) {
	amp_source_t *st = src->user;
	if (!st) {
		return;
	}
	ampw_decrypt_close(st->dec);
	ampfp4_free_fragment(&st->frag);
	amphls_free_playlist(&st->list);
	free(st->scratch);
	free(st->concat);
	free(st->emit);
	memset(st, 0, sizeof(*st)); // no keys or tokens linger on the heap
	free(st);
}

static const streambridge_source_ops_t amp_source_ops = {
	.open = amp_source_open, .read = amp_source_read, .close = amp_source_close};

// ---------------------------------------------------------------------------
// slots and queue: the subsonicstream shape, with .aac names
// ---------------------------------------------------------------------------

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
	streambridge_t *bridge;
	char path[AMP_PATH_MAX];
	bool leased;
	bool error_reported;
} slot_t;
static slot_t slots[2];
static ampapi_song_t *queue;
static int queue_count;
static unsigned queue_generation;
static int wanted_slot = -1;
static char last_error[192];

bool ampstream_owns(const char *path) { return path && strncmp(path, AMP_PATH_PREFIX, 6) == 0; }

static int index_of(const char *path) {
	unsigned generation;
	int index, end = 0;
	if (!ampstream_owns(path) || sscanf(path, "amp://%u/%d.%*s%n", &generation, &index, &end) != 2 ||
		!end || path[end] || generation != queue_generation || index < 0 || index >= queue_count) {
		return -1;
	}
	char expected[AMP_PATH_MAX];
	snprintf(expected, sizeof(expected), "amp://%u/%d.aac", generation, index);
	return strcmp(path, expected) == 0 ? index : -1;
}

static bool allocate_slots(void) {
	for (int i = 0; i < 2; i++) {
		if (!slots[i].bridge) {
			slots[i].bridge = streambridge_create(STREAMBRIDGE_CAPACITY);
			if (!slots[i].bridge) {
				return false;
			}
		}
	}
	return true;
}

bool ampstream_queue(const ampapi_song_t *tracks, int count, int start, char paths[][AMP_PATH_MAX],
					 const char **pointers, int *queued, int *selected, char *error, size_t size) {
	if (error && size) {
		error[0] = '\0';
	}
	if (!tracks || !paths || !pointers || !queued || !selected || count <= 0 ||
		count > AMP_QUEUE_MAX || start < 0 || start >= count) {
		return false;
	}
	if (!tracks[start].id[0]) {
		if (error && size) {
			snprintf(error, size, "ampapi_bad_args");
		}
		return false;
	}
	ampapi_song_t *copy = calloc((size_t)count, sizeof(*copy));
	if (!copy) {
		if (error && size) {
			snprintf(error, size, "amp_out_of_memory");
		}
		return false;
	}
	int n = 0, chosen = 0;
	for (int i = 0; i < count; i++) {
		if (!tracks[i].id[0]) {
			continue;
		}
		if (i == start) {
			chosen = n;
		}
		copy[n++] = tracks[i];
	}
	pthread_mutex_lock(&lock);
	if (!allocate_slots()) {
		pthread_mutex_unlock(&lock);
		free(copy);
		if (error && size) {
			snprintf(error, size, "amp_out_of_memory");
		}
		return false;
	}
	for (int i = 0; i < 2; i++) {
		streambridge_cancel(slots[i].bridge);
		if (!slots[i].leased) {
			slots[i].path[0] = '\0';
		}
	}
	free(queue);
	queue = copy;
	queue_count = n;
	queue_generation++;
	if (!queue_generation) {
		queue_generation++; // no ambiguity with a never-loaded queue
	}
	wanted_slot = -1;
	last_error[0] = '\0';
	for (int i = 0; i < n; i++) {
		snprintf(paths[i], AMP_PATH_MAX, "amp://%u/%d.aac", queue_generation, i);
		pointers[i] = paths[i];
	}
	*queued = n;
	*selected = chosen;
	pthread_mutex_unlock(&lock);
	return n > 0;
}

static int choose_slot(const char *path, bool prefetch) {
	int index = index_of(path);
	if (index < 0) {
		snprintf(last_error, sizeof(last_error), "amp_invalid_id");
		return -1;
	}
	// An already leased instance of the same path is NOT a replay. Repeat-one
	// needs the other slot, holding a new stream starting at byte zero.
	for (int i = 0; i < 2; i++) {
		if (!slots[i].leased && strcmp(slots[i].path, path) == 0) {
			streambridge_state_t state;
			streambridge_state(slots[i].bridge, &state);
			if (prefetch && state.failed) {
				return -1;
			}
			if (!state.canceled && !state.failed) {
				if (!prefetch) {
					streambridge_promote(slots[i].bridge);
				}
				return i;
			}
		}
	}
	int free_slot = -1;
	for (int i = 0; i < 2; i++) {
		if (!slots[i].leased && (prefetch ? i != wanted_slot : true)) {
			free_slot = i;
			if (!slots[i].path[0]) {
				break;
			}
		}
	}
	if (free_slot < 0) {
		return -1;
	}
	amp_source_t *st = calloc(1, sizeof(*st));
	if (!st) {
		snprintf(last_error, sizeof(last_error), "amp_out_of_memory");
		return -1;
	}
	snprintf(st->adam_id, sizeof(st->adam_id), "%s", queue[index].id);
	if (!streambridge_start_source(slots[free_slot].bridge, &amp_source_ops, st, 0, prefetch)) {
		free(st);
		snprintf(last_error, sizeof(last_error), "amp_stream_failed");
		return -1;
	}
	snprintf(slots[free_slot].path, sizeof(slots[free_slot].path), "%s", path);
	slots[free_slot].error_reported = false;
	return free_slot;
}

bool ampstream_prepare(char *path, size_t path_size) {
	(void)path_size; // the stream is written under the name the queue already holds
	amplocal_init(); // no-op unless [applemusic] translate=1
	if (!ampstream_owns(path)) {
		ampstream_cancel();
		return true;
	}
	pthread_mutex_lock(&lock);
	wanted_slot = choose_slot(path, false);
	if (wanted_slot < 0 && !last_error[0]) {
		snprintf(last_error, sizeof(last_error), "amp_stream_failed");
	}
	// Any unused, stale pre-cache gives way. There is never a third stream.
	for (int i = 0; i < 2; i++) {
		if (i != wanted_slot && !slots[i].leased) {
			streambridge_cancel(slots[i].bridge);
			slots[i].path[0] = '\0';
		}
	}
	pthread_mutex_unlock(&lock);
	// The audio thread is allowed to wait for LOCAL bytes at open; it must not
	// perform HTTP itself. Failure is reported by acquire/the GUI error timer.
	return true;
}

void ampstream_prefetch(const char *path) {
	pthread_mutex_lock(&lock);
	if (!ampstream_owns(path)) {
		for (int i = 0; i < 2; i++) {
			if (i != wanted_slot && !slots[i].leased) {
				streambridge_cancel(slots[i].bridge);
				slots[i].path[0] = '\0';
			}
		}
		pthread_mutex_unlock(&lock);
		return;
	}
	if (wanted_slot >= 0) {
		streambridge_state_t current;
		streambridge_state(slots[wanted_slot].bridge, &current);
		// Current music always wins bandwidth. Retry on the next GUI tick if
		// its reserve is low; don't start a second fetch during a starvation.
		if (!current.failed && !current.canceled &&
			(current.eof || current.buffered >= STREAMBRIDGE_PRECACHE)) {
			choose_slot(path, true);
		}
	}
	pthread_mutex_unlock(&lock);
}

static void cancel_locked(void) {
	for (int i = 0; i < 2; i++) {
		streambridge_cancel(slots[i].bridge);
		if (!slots[i].leased) {
			slots[i].path[0] = '\0';
		}
	}
	wanted_slot = -1;
	last_error[0] = '\0'; // deliberate Stop/service switch discards stale feedback
}

void ampstream_cancel(void) {
	pthread_mutex_lock(&lock);
	cancel_locked();
	pthread_mutex_unlock(&lock);
}

void ampstream_reset(void) {
	pthread_mutex_lock(&lock);
	cancel_locked();
	// Adam ids only mean something to the account that supplied this page.
	// No stale playlist may silently reuse them on a new wrapper or account.
	free(queue);
	queue = NULL;
	queue_count = 0;
	pthread_mutex_unlock(&lock);
}

bool ampstream_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool wanted = false;
	for (int i = 0; i < 2; i++) {
		if (slots[i].path[0]) {
			streambridge_state_t state;
			streambridge_state(slots[i].bridge, &state);
			wanted |= !state.canceled && !state.failed && !state.eof;
		}
	}
	pthread_mutex_unlock(&lock);
	return wanted;
}

streambridge_t *ampstream_acquire(const char *path, int *duration, int *bitrate) {
	pthread_mutex_lock(&lock);
	int index = index_of(path);
	int slot = wanted_slot;
	streambridge_t *bridge = NULL;
	if (index >= 0 && slot >= 0 && !slots[slot].leased && strcmp(slots[slot].path, path) == 0) {
		slots[slot].leased = true;
		bridge = slots[slot].bridge;
		if (duration) {
			*duration = queue[index].duration_ms / 1000;
		}
		if (bitrate) {
			*bitrate = 0; // the variant is chosen by the producer, not here
		}
	}
	pthread_mutex_unlock(&lock);
	return bridge;
}

void ampstream_release(streambridge_t *bridge) {
	pthread_mutex_lock(&lock);
	for (int i = 0; i < 2; i++) {
		if (slots[i].bridge == bridge) {
			// Preserve a useful failure for the GUI even when teardown wins the
			// race with its next tick. Cancellation isn't a playback failure.
			streambridge_state_t state;
			streambridge_state(bridge, &state);
			if (state.failed && !state.canceled && !slots[i].error_reported) {
				snprintf(last_error, sizeof(last_error), "%s", state.error);
			}
			streambridge_cancel(bridge);
			slots[i].leased = false;
			slots[i].path[0] = '\0';
			// Do not clear wanted_slot here: EOF posts an advance which can
			// already have promoted the other slot before this lease closes.
			break;
		}
	}
	pthread_mutex_unlock(&lock);
}

// The queued song a path names, for the track menu's download action.
bool ampstream_song_for(const char *path, ampapi_song_t *out) {
	int index = index_of(path);
	if (index < 0 || !queue) {
		return false;
	}
	*out = queue[index];
	return true;
}

bool ampstream_metadata(const char *path, song_metadata_t *out) {
	if (!ampstream_owns(path)) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&lock);
	int index = index_of(path);
	if (index >= 0) {
		const ampapi_song_t *track = &queue[index];
		snprintf(out->title, sizeof(out->title), "%s", track->title);
		snprintf(out->artist, sizeof(out->artist), "%s", track->artist);
		snprintf(out->album, sizeof(out->album), "%s", track->album);
		out->has_tags = true;
	}
	pthread_mutex_unlock(&lock);
	return true;
}

bool ampstream_take_error(char *out, size_t size) {
	pthread_mutex_lock(&lock);
	if (!last_error[0] && wanted_slot >= 0) {
		slot_t *slot = &slots[wanted_slot];
		streambridge_state_t state;
		streambridge_state(slot->bridge, &state);
		if (state.failed && !state.canceled && !slot->error_reported) {
			snprintf(last_error, sizeof(last_error), "%s", state.error);
			slot->error_reported = true;
		}
	}
	bool have = last_error[0] != '\0';
	if (have) {
		snprintf(out, size, "%s", last_error);
		last_error[0] = '\0';
	}
	pthread_mutex_unlock(&lock);
	return have;
}

void ampstream_config(char *storefront, size_t store_size) {
	snprintf(storefront, store_size, "%s", config_get("applemusic", "storefront", "jp"));
}
