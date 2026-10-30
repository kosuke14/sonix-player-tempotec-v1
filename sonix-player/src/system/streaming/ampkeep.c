#include "ampkeep.h"

#include "amphls.h"
#include "ampfp4.h"
#include "amplocal.h"
#include "ampwrapper.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/gui/shell/gui.h"
#include "src/system/net/http.h"
#include "src/system/streaming/podcastdl.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The Apple twin of streamkeep's copy: the hard part is that there is no
// file to copy. Every piece of the finished track arrives encrypted, so the
// download is an assembly line: dispatch -> playlist -> init rewrite ->
// segment fetch -> sample decrypt (guest daemon or the public prefetch key)
// -> splice back -> append. See ampkeep.h for the destination layout.

#define AMPKEEP_PART_SUFFIX ".part"
// One batch of ciphertext runs; 128 samples is the WV2D frame budget, and a
// segment's worth of protected ranges stays far under this.
#define AMPKEEP_BATCH_MAX 128
#define AMPKEEP_CONCAT_SIZE (512 * 1024)
// A media segment is a few seconds of AAC; a four MB ceiling is generous and
// still keeps one fetch bounded.
#define AMPKEEP_SEG_MAX (4 * 1024 * 1024)

// The same public key ampstream uses for the prefetch description (index 0):
// Apple publishes it to every client, and these samples decrypt locally.
static const unsigned char AMPKEEP_PREFETCH_KEY[16] = {0x32, 0xb8, 0xad, 0xe1, 0x76, 0x9e,
													   0x26, 0xb1, 0xff, 0xb8, 0x98, 0x63,
													   0x52, 0x79, 0x3f, 0xc6};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool running;
static char last_error_text[192];

static void remember_error(const char *message) {
	pthread_mutex_lock(&lock);
	snprintf(last_error_text, sizeof(last_error_text), "%s", message ? message : "");
	pthread_mutex_unlock(&lock);
}

bool ampkeep_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = running;
	pthread_mutex_unlock(&lock);
	return busy;
}

bool ampkeep_last_error(char *out, size_t out_size) {
	pthread_mutex_lock(&lock);
	bool have = last_error_text[0] != '\0';
	if (have) {
		snprintf(out, out_size, "%s", last_error_text);
		last_error_text[0] = '\0';
	}
	pthread_mutex_unlock(&lock);
	return have;
}

// ---------------------------------------------------------------------------
// small pieces
// ---------------------------------------------------------------------------

// Every folder on the way to the path, made. The card may never have held an
// Apple Music album, so <sd>/Music/Apple Music/<artist>/<album> is four
// levels of nothing. The path itself is the file and stops one level short.
static bool make_dirs(const char *path) {
	char work[768];
	snprintf(work, sizeof(work), "%s", path);
	char *leaf = strrchr(work, '/');
	if (!leaf || leaf == work) {
		return false;
	}
	*leaf = '\0';
	for (char *p = work + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		if (mkdir(work, 0755) != 0 && errno != EEXIST) {
			return false;
		}
		*p = '/';
	}
	return mkdir(work, 0755) == 0 || errno == EEXIST;
}

// <card>/Music/Apple Music/<artist>/<album>/<nn> <title>.m4a
static bool destination_path(const ampapi_song_t *song, char *out, size_t size) {
	const char *sd = storage_sd_root();
	if (!sd || !sd[0]) {
		out[0] = '\0';
		return false;
	}
	char name_artist[160], name_album[160], name_title[160];
	podcastdl_safe_name(song->artist[0] ? song->artist : "Unknown artist", "Unknown",
						name_artist, sizeof(name_artist));
	podcastdl_safe_name(song->album[0] ? song->album : "Unknown album", "Unknown",
						name_album, sizeof(name_album));
	podcastdl_safe_name(song->title[0] ? song->title : "Track", "Track", name_title,
						sizeof(name_title));
	snprintf(out, size, "%s/Music/Apple Music/%s/%s/%s.m4a", sd, name_artist, name_album,
			 name_title);
	return out[0] != '\0';
}

// songList[0]'s master playlist URL out of the playback dispatch -- the same
// walk ampstream.c does on the streaming path.
static bool playback_url(const char *json_text, char *url, size_t size, char *error,
						 size_t error_size) {
	json_doc_t doc;
	if (!json_parse(json_text, &doc)) {
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

// One bounded HTTP fetch into a malloc'd buffer.
static bool fetch_url(const char *url, const char *content_type, size_t cap, uint8_t **out,
					  size_t *out_len, char *error, size_t error_size) {
	http_req_t req = {.method = "GET",
					  .content_type = content_type,
					  .no_https_downgrade = true};
	int status = 0;
	if (!http_request(url, &req, (char **)out, out_len, cap, 30, &status) ||
		(status != 200 && status != 206)) {
		snprintf(error, error_size, "amp_fetch_http_%d", status);
		return false;
	}
	return *out != NULL;
}

// ---------------------------------------------------------------------------
// the plain init segment: swap the encrypted sample entry for a bare mp4a
// ---------------------------------------------------------------------------

// Box walkers for the init segment. The entry we are after:
//   moov/trak/mdia/minf/stbl/stsd/<entry>  -- an 'mp4a' whose children carry
//   'sinfr' (Apple's FairPlay scheme info) instead of an 'esds'. The rewrite
//   keeps the entry's 36 fixed bytes and appends a plain esds built from the
//   AudioSpecificConfig ampfp4 already parsed. Sample sizes and offsets are
//   untouched: the media segments splice back byte-for-byte.
typedef struct {
	const uint8_t *data;
	size_t len;
} box_t;

static bool box_at(const uint8_t *p, size_t avail, box_t *out, size_t *consumed) {
	if (avail < 8) {
		return false;
	}
	uint32_t size = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
					p[3];
	if (size < 8 || size > avail) {
		return false;
	}
	out->data = p;
	out->len = size;
	*consumed = size;
	return memcmp(p + 4, "moov", 4) == 0 || memcmp(p + 4, "trak", 4) == 0 ||
		   memcmp(p + 4, "mdia", 4) == 0 || memcmp(p + 4, "minf", 4) == 0 ||
		   memcmp(p + 4, "stbl", 4) == 0 || memcmp(p + 4, "stsd", 4) == 0;
}

// Finds the first sample entry inside stsd (children start after the
// full-box header and the entry count).
static bool stsd_entry(const box_t *stsd, box_t *entry) {
	// stsd payload: version/flags (4), entry_count (4), then entries.
	if (stsd->len < 16) {
		return false;
	}
	return box_at(stsd->data + 16, stsd->len - 16, entry, &(size_t){0});
}

// Appends the esds box for `asc` to a growable buffer: the plain description
// the decoder reads once the FairPlay scheme info is gone.
static void append_esds(uint8_t **buf, size_t *len, size_t *cap, const unsigned char *asc,
						int asc_len, int track_id) {
	// DecoderSpecificInfo (tag 0x05) wraps the ASC.
	size_t dsi_len = (size_t)asc_len;
	// DecoderConfigDescriptor (tag 0x04): header fields + the DSI child.
	size_t dcd_len = 13 + 2 + dsi_len;
	// ES_Descriptor (tag 0x03): ES_ID + flags + the DCD child.
	size_t es_len = 3 + 2 + dcd_len;
	// esds box: header + version/flags + the ES descriptor.
	size_t need = 8 + 4 + 2 + es_len + 8; // +8 slack for length overflows
	if (*len + need > *cap) {
		*cap = (*len + need) * 2;
		*buf = realloc(*buf, *cap);
	}
	uint8_t *p = *buf + *len;
	uint8_t *at = p;
	uint32_t box_size = (uint32_t)(8 + 4 + 2 + es_len);
	*at++ = (uint8_t)(box_size >> 24);
	*at++ = (uint8_t)(box_size >> 16);
	*at++ = (uint8_t)(box_size >> 8);
	*at++ = (uint8_t)box_size;
	memcpy(at, "esds", 4);
	at += 4;
	*at++ = 0; // version/flags
	*at++ = 0;
	*at++ = 0;
	*at++ = 0;
	*at++ = 0x03;							  // ES_Descriptor tag
	*at++ = (uint8_t)es_len;				  // one length byte: all inside
	*at++ = (uint8_t)(track_id >> 8);		  // ES_ID
	*at++ = (uint8_t)track_id;
	*at++ = 0;								  // flags
	*at++ = 0x04;							  // DecoderConfigDescriptor tag
	*at++ = (uint8_t)dcd_len;
	*at++ = 0x40;							  // objectTypeIndication: MPEG-4 Audio
	*at++ = 0x15;							  // streamType: audio << 2 | reserved
	*at++ = 0; *at++ = 0; *at++ = 0;		  // bufferSizeDB
	*at++ = 0; *at++ = 0; *at++ = 0; *at++ = 0; // maxBitrate
	*at++ = 0; *at++ = 0; *at++ = 0; *at++ = 0; // avgBitrate
	*at++ = 0x05;							  // DecoderSpecificInfo tag
	*at++ = (uint8_t)dsi_len;
	memcpy(at, asc, (size_t)asc_len);
	*len += (size_t)(box_size);
	(void)p;
}

// Rewrites the init segment: the audio sample entry loses its FairPlay
// children and gains a plain esds. Everything else (ftyp, mvhd, the track
// header, the tfdt machinery the segments rely on) is copied verbatim.
static bool plain_init(const uint8_t *init, size_t init_len, const ampfp4_init_t *parsed,
					   const ampapi_song_t *song, uint8_t **out, size_t *out_len,
					   char *error, size_t error_size) {
	(void)song;
	box_t walk;
	box_t stsd = {0};
	bool found = false;
	size_t at = 0;
	while (at < init_len) {
		size_t consumed = 0;
		if (!box_at(init + at, init_len - at, &walk, &consumed)) {
			break;
		}
		if (memcmp(walk.data + 4, "stsd", 4) == 0) {
			stsd = walk;
			found = true;
			break;
		}
		at += consumed;
	}
	if (!found) {
		snprintf(error, error_size, "amp_init_no_stsd");
		return false;
	}
	box_t entry;
	if (!stsd_entry(&stsd, &entry) || entry.len < 36) {
		snprintf(error, error_size, "amp_init_no_entry");
		return false;
	}

	// The rewritten entry: same 8+36 header bytes, plain esds child.
	size_t entry_header = 36; // box header + AudioSampleEntry fixed fields
	uint8_t *children = NULL;
	size_t children_len = 0, children_cap = 0;
	append_esds(&children, &children_len, &children_cap, parsed->asc, parsed->asc_len,
				parsed->track_id);
	size_t new_entry_len = entry_header + children_len;

	size_t new_len = init_len - entry.len + new_entry_len;
	uint8_t *buf = malloc(new_len);
	if (!buf) {
		free(children);
		snprintf(error, error_size, "amp_out_of_memory");
		return false;
	}
	const uint8_t *entry_begin = entry.data;
	size_t before = (size_t)(entry_begin - init);
	memcpy(buf, init, before);
	// entry header: size + type + the fixed fields, copied as they are
	memcpy(buf + before, entry_begin, entry_header);
	// patch the entry size
	uint32_t be_size = (uint32_t)new_entry_len;
	buf[before + 0] = (uint8_t)(be_size >> 24);
	buf[before + 1] = (uint8_t)(be_size >> 16);
	buf[before + 2] = (uint8_t)(be_size >> 8);
	buf[before + 3] = (uint8_t)be_size;
	memcpy(buf + before + entry_header, children, children_len);
	memcpy(buf + before + new_entry_len, entry_begin + entry.len,
		   init_len - before - entry.len);
	free(children);

	// The moov/trak sizes above the entry grow by the same delta; walk the
	// container chain and patch each.
	uint32_t delta = (uint32_t)(new_entry_len - entry.len);
	size_t off = 0;
	static const char *chain[] = {"moov", "trak", "mdia", "minf", "stbl", "stsd"};
	int depth = 0;
	while (off + 8 <= new_len && depth < 6) {
		uint32_t sz = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16) |
					  ((uint32_t)buf[off + 2] << 8) | buf[off + 3];
		if (sz < 8 || off + sz > new_len) {
			break;
		}
		bool in_chain = depth == 5 ? true : memcmp(buf + off + 4, chain[depth], 4) == 0;
		if (!in_chain) {
			break;
		}
		uint32_t patched = sz + delta;
		buf[off + 0] = (uint8_t)(patched >> 24);
		buf[off + 1] = (uint8_t)(patched >> 16);
		buf[off + 2] = (uint8_t)(patched >> 8);
		buf[off + 3] = (uint8_t)patched;
		off += depth == 5 ? 8 + 8 : sz; // stsd: step over header + entry count
		depth++;
	}

	*out = buf;
	*out_len = new_len;
	return true;
}

// ---------------------------------------------------------------------------
// the segment assembly
// ---------------------------------------------------------------------------

// Decrypts one fetched media segment in place. `key_uri` is the FairPlay key
// guarding the segment; `dec` is the loopback decrypt connection (lazily
// opened by the caller's batch flushes).
static bool decrypt_segment(const char *adam_id, const char *key_uri, uint8_t *seg,
							size_t seg_len, const ampfp4_init_t *init, ampw_decrypt_t **dec,
							int port, char *error, size_t error_size) {
	ampfp4_fragment_t frag;
	if (!ampfp4_parse_fragment(seg, seg_len, init->track_id, init, &frag, error,
							   error_size)) {
		// A plain segment (no moof/senc) passes through untouched.
		return true;
	}

	// Collect the batch: everything the wrapper owes us for this segment.
	const uint8_t *bufs[AMPKEEP_BATCH_MAX];
	size_t lens[AMPKEEP_BATCH_MAX];
	uint8_t *plains[AMPKEEP_BATCH_MAX];
	size_t plain_lens[AMPKEEP_BATCH_MAX];
	ampfp4_cbcs_t cbcs[AMPKEEP_BATCH_MAX];
	ampfp4_sample_t *batch_samples[AMPKEEP_BATCH_MAX];
	uint8_t *concat = malloc(AMPKEEP_CONCAT_SIZE);
	if (!concat) {
		ampfp4_free_fragment(&frag);
		snprintf(error, error_size, "amp_out_of_memory");
		return false;
	}
	size_t concat_used = 0;
	int batch = 0;
	bool ok = true;
	for (int i = 0; ok && i < frag.count; i++) {
		ampfp4_sample_t *s = &frag.samples[i];
		if (s->desc_index == 0) {
			// The public-key prefetch sample: decrypted on this side.
			if (init->desc[0].scheme_type[0]) {
				size_t plain_len = 0;
				if (!ampfp4_decrypt_local(s, AMPKEEP_PREFETCH_KEY, init->desc[0].constant_iv,
										  init->desc[0].constant_iv_len,
										  strcmp(init->desc[0].scheme_type, "cenc") == 0,
										  init->desc[0].crypt_byte_block,
										  init->desc[0].skip_byte_block, concat,
										  AMPKEEP_CONCAT_SIZE, &plain_len) ||
					plain_len != s->size) {
					ok = false;
					snprintf(error, error_size, "amp_decrypt_local_failed");
					break;
				}
				memcpy((uint8_t *)s->data, concat, s->size);
			}
			continue;
		}
		ampfp4_cbcs_t cbc;
		if (!ampfp4_cbcs_split(s, &cbc)) {
			continue; // plain bytes already
		}
		if (batch >= AMPKEEP_BATCH_MAX) {
			snprintf(error, error_size, "amp_batch_overflow");
			ok = false;
			break;
		}
		if (!cbc.aligned) {
			// Subsampled: concatenate the protected ranges into batch storage.
			size_t total = 0;
			for (int j = 0; j < s->sub_count; j++) {
				total += (size_t)s->sub[j].enc;
			}
			if (concat_used + total > AMPKEEP_CONCAT_SIZE) {
				snprintf(error, error_size, "amp_batch_overflow");
				ok = false;
				break;
			}
			ampfp4_concatenate_protected(s, concat + concat_used, total);
			cbc.aligned = concat + concat_used;
			cbc.tail = concat + concat_used + cbc.aligned_len;
			concat_used += total;
		}
		cbcs[batch] = cbc;
		batch_samples[batch] = s;
		bufs[batch] = cbc.aligned;
		lens[batch] = cbc.aligned_len;
		batch++;
	}

	// One decrypt round trip per segment's batch.
	if (ok && batch > 0) {
		if (!*dec) {
			*dec = ampw_decrypt_open("127.0.0.1", port, NULL, NULL);
			if (!*dec) {
				snprintf(error, error_size, "amp_decrypt_connect");
				ok = false;
			}
		}
		if (ok && !ampw_decrypt_batch(*dec, adam_id, key_uri, bufs, lens, batch, plains,
									  plain_lens, error, error_size)) {
			ok = false;
		}
		if (ok) {
			for (int i = 0; i < batch; i++) {
				ampfp4_reassemble(batch_samples[i], plains[i], plain_lens[i], cbcs[i].tail,
								  cbcs[i].tail_len, (uint8_t *)batch_samples[i]->data);
				free(plains[i]);
			}
		}
	}
	free(concat);
	ampfp4_free_fragment(&frag);
	return ok;
}

// Writes the .tags sidecar and the .jpg cover the library scan reads.
static bool write_sidecars(const ampapi_song_t *song, const char *path, char *error,
						   size_t error_size) {
	char sidecar[800];
	snprintf(sidecar, sizeof(sidecar), "%.700s.tags", path);
	FILE *f = fopen(sidecar, "w");
	if (!f) {
		snprintf(error, error_size, "amp_keep_sidecar_failed");
		return false;
	}
	fprintf(f, "title=%s\n", song->title);
	fprintf(f, "artist=%s\n", song->artist);
	fprintf(f, "album=%s\n", song->album);
	fclose(f);

	if (song->artwork[0]) {
		char art_url[600];
		ampapi_artwork_url(song->artwork, 300, art_url, sizeof(art_url));
		uint8_t *jpg = NULL;
		size_t jpg_len = 0;
		char fetch_err[192] = "";
		if (fetch_url(art_url, NULL, 2 * 1024 * 1024, &jpg, &jpg_len, fetch_err,
					  sizeof(fetch_err)) &&
			jpg_len > 0) {
			char cover[800];
			char stem[700];
			snprintf(stem, sizeof(stem), "%.650s", path);
			char *dot = strrchr(stem, '.');
			if (dot) {
				*dot = '\0';
			}
			snprintf(cover, sizeof(cover), "%.650s.jpg", stem);
			FILE *c = fopen(cover, "wb");
			if (c) {
				fwrite(jpg, 1, jpg_len, c);
				fclose(c);
			}
		}
		free(jpg);
		// A missing cover is not a failed keep: the scan falls back cleanly.
	}
	return true;
}

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

// The worker's argument: the song plus whether the caller wants a toast.
typedef struct {
	ampapi_song_t song;
	bool notify_result;
} ampkeep_job_t;

static void *keep_worker(void *arg) {
	ampkeep_job_t *job = arg;
	ampapi_song_t song = job->song;
	bool notify_result = job->notify_result;
	free(job);

	char error[192] = "";
	char path[768];
	bool ok = destination_path(&song, path, sizeof(path)) && make_dirs(path);
	if (!ok) {
		snprintf(error, sizeof(error), "amp_keep_path_failed");
	}
	// Never overwrite: a finished download is the user's property now.
	char final_path[768];
	snprintf(final_path, sizeof(final_path), "%s", path);
	struct stat st;
	if (ok && stat(final_path, &st) == 0) {
		ok = false;
		snprintf(error, sizeof(error), "amp_keep_exists");
	}
	char part_path[800];
	snprintf(part_path, sizeof(part_path), "%.700s%s", path, AMPKEEP_PART_SUFFIX);

	char *dispatch = NULL;
	size_t dispatch_len = 0;
	amphls_playlist_t list = {0};
	ampfp4_init_t init;
	memset(&init, 0, sizeof(init));
	uint8_t *init_plain = NULL;
	size_t init_plain_len = 0;
	ampw_decrypt_t *dec = NULL;
	FILE *f = NULL;
	int port = (int)config_get_int("applemusic", "decrypt_port", 10020);

	// 1. the playback dispatch -> master -> variant -> parse
	if (ok) {
		ok = amplocal_playback(song.id, &dispatch, &dispatch_len, error, sizeof(error));
	}
	char master_url[640];
	if (ok) {
		ok = playback_url(dispatch, master_url, sizeof(master_url), error, sizeof(error));
	}
	free(dispatch);
	uint8_t *body = NULL;
	size_t body_len = 0;
	if (ok) {
		ok = fetch_url(master_url, NULL, AMPKEEP_SEG_MAX, &body, &body_len, error,
					   sizeof(error));
	}
	char variant_url[640];
	if (ok) {
		bool picked = amphls_pick_variant((const char *)body, master_url, AMPHLS_CODEC_AAC,
										  variant_url, sizeof(variant_url)) ||
					  amphls_pick_variant((const char *)body, master_url, AMPHLS_CODEC_HE_AAC,
										  variant_url, sizeof(variant_url));
		if (!picked) {
			snprintf(error, sizeof(error), "amp_no_variant");
			ok = false;
		}
	}
	free(body);
	body = NULL;
	if (ok) {
		ok = fetch_url(variant_url, NULL, AMPKEEP_SEG_MAX, &body, &body_len, error,
					   sizeof(error));
	}
	char variant_copy[640];
	snprintf(variant_copy, sizeof(variant_copy), "%s", variant_url);
	if (ok && (!amphls_parse_variant((const char *)body, variant_copy, &list, error,
									 sizeof(error)) ||
			   list.count == 0)) {
		ok = false;
		if (!error[0]) {
			snprintf(error, sizeof(error), "amp_no_segments");
		}
	}
	free(body);
	body = NULL;

	// 2. the init segment, rewritten to plain
	if (ok && list.init_url[0]) {
		ok = fetch_url(list.init_url, NULL, AMPKEEP_SEG_MAX, &body, &body_len, error,
					   sizeof(error));
		if (ok) {
			ok = ampfp4_parse_init(body, body_len, &init, error, sizeof(error));
		}
		if (ok) {
			ok = plain_init(body, body_len, &init, &song, &init_plain, &init_plain_len,
							error, sizeof(error));
		}
	} else if (ok) {
		snprintf(error, sizeof(error), "amp_no_init_segment");
		ok = false;
	}
	free(body);
	body = NULL;

	// 3. the file: plain init, then every segment decrypted in place
	if (ok) {
		f = fopen(part_path, "wb");
		if (!f) {
			snprintf(error, sizeof(error), "amp_keep_open_failed");
			ok = false;
		} else {
			fwrite(init_plain, 1, init_plain_len, f);
		}
	}
	for (int i = 0; ok && i < list.count; i++) {
		ok = fetch_url(list.segments[i].url, NULL, AMPKEEP_SEG_MAX, &body, &body_len, error,
					   sizeof(error)) &&
			 body_len <= AMPKEEP_SEG_MAX;
		if (!ok) {
			break;
		}
		const char *key_uri = amphls_key_for(&list, i);
		if (!key_uri[0]) {
			snprintf(error, sizeof(error), "amp_segment_unkeyed");
			ok = false;
			break;
		}
		ok = decrypt_segment(song.id, key_uri, body, body_len, &init, &dec, port, error,
							 sizeof(error));
		if (ok) {
			fwrite(body, 1, body_len, f);
			// The card is slow and the segments are small: flush each one so
			// a pulled card never leaves a corrupt-looking m4a behind.
			fflush(f);
		}
		free(body);
		body = NULL;
	}
	if (f) {
		fclose(f);
	}
	ampw_decrypt_close(dec);
	dec = NULL;
	amphls_free_playlist(&list);
	free(init_plain);

	// 4. sidecars, then the rename that makes it a finished track
	if (ok) {
		ok = write_sidecars(&song, path, error, sizeof(error));
	}
	sync();
	if (ok) {
		ok = rename(part_path, final_path) == 0;
		if (!ok) {
			snprintf(error, sizeof(error), "amp_keep_rename_failed");
		}
	} else {
		unlink(part_path);
	}

	if (!ok) {
		fprintf(stderr, "ampkeep: could not save '%s': %s\n", song.title, error);
	}
	remember_error(ok ? "" : error);
	pthread_mutex_lock(&lock);
	running = false;
	pthread_mutex_unlock(&lock);
	if (notify_result) {
		gui_notify_popup(ok ? "amp_keep_done" : (error[0] ? error : "amp_keep_failed"));
	}
	return NULL;
}

bool ampkeep_keep(const ampapi_song_t *song, bool notify_result) {
	if (!song || !song->id[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	if (running) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	running = true;
	pthread_mutex_unlock(&lock);

	ampkeep_job_t *job = malloc(sizeof(*job));
	if (!job) {
		pthread_mutex_lock(&lock);
		running = false;
		pthread_mutex_unlock(&lock);
		return false;
	}
	memset(job, 0, sizeof(*job));
	job->song = *song;
	job->notify_result = notify_result;
	pthread_t thread;
	if (pthread_create(&thread, NULL, keep_worker, job) != 0) {
		free(job);
		pthread_mutex_lock(&lock);
		running = false;
		pthread_mutex_unlock(&lock);
		return false;
	}
	pthread_detach(thread);
	return true;
}
