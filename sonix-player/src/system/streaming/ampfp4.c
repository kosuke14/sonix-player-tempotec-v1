#include "ampfp4.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// box walking
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t type;
	size_t box_offset; // of the box header
	size_t size;	   // whole box, header included
	size_t header_size;
} amp_box_t;

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p) {
	return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static int32_t be32s(const uint8_t *p) {
	return (int32_t)be32(p);
}

// Steps to the next box inside [offset, end). False at the end or on a box
// that overruns it.
static bool next_box(const uint8_t *data, size_t len, size_t offset, amp_box_t *out) {
	if (offset + 8 > len) {
		return false;
	}
	uint32_t raw = be32(data + offset);
	out->box_offset = offset;
	out->type = be32(data + offset + 4);
	out->header_size = 8;
	uint64_t size = raw;
	if (raw == 1) {
		if (offset + 16 > len) {
			return false;
		}
		out->header_size = 16;
		size = be64(data + offset + 8);
	} else if (raw == 0) {
		size = len - offset;
	}
	if (size < out->header_size || offset + size > len) {
		return false;
	}
	out->size = (size_t)size;
	return true;
}

// The first box named `type` inside `data` (a whole box's payload range),
// searched from `from`. Returns its payload extent, or false.
static bool find_box(const uint8_t *data, size_t len, size_t from, uint32_t type, size_t *payload,
					 size_t *payload_len) {
	size_t offset = from;
	amp_box_t box;
	while (next_box(data, len, offset, &box)) {
		if (box.type == type) {
			*payload = box.box_offset + box.header_size;
			*payload_len = box.size - box.header_size;
			return true;
		}
		offset = box.box_offset + box.size;
	}
	return false;
}

// The track whose hdlr handler_type matches, as a payload range in the moov.
static bool find_track_by_handler(const uint8_t *moov, size_t moov_len, uint32_t handler,
								  size_t *trak, size_t *trak_len) {
	size_t offset = 0;
	amp_box_t box;
	while (next_box(moov, moov_len, offset, &box)) {
		if (box.type == 0x7472616bu) { // "trak"
			const uint8_t *t = moov + box.box_offset + box.header_size;
			size_t tlen = box.size - box.header_size;
			// hdlr sits a few boxes down; a byte search then a 12-byte skip is
			// how the reference client finds it, and it has held up.
			for (size_t i = 0; i + 16 <= tlen; i++) {
				if (t[i] == 'h' && t[i + 1] == 'd' && t[i + 2] == 'l' && t[i + 3] == 'r') {
					if (i + 16 <= tlen && be32(t + i + 12) == handler) {
						*trak = box.box_offset + box.header_size;
						*trak_len = tlen;
						return true;
					}
				}
			}
		}
		offset = box.box_offset + box.size;
	}
	return false;
}

static size_t sample_entry_header_size(uint32_t type) {
	switch (type) {
	case 0x656e6376u: // encv
	case 0x61766331u: // avc1
	case 0x68766331u: // hvc1
		return 86;
	case 0x656e6361u: // enca
	case 0x6d703461u: // mp4a
	case 0x616c6163u: // alac
		return 36;
	default:
		return 36;
	}
}

// ---------------------------------------------------------------------------
// the init segment
// ---------------------------------------------------------------------------

// Reads the AudioSpecificConfig out of an esds payload: the descriptor chain
// (0x03 -> 0x04 -> 0x05) with its variable-length sizes.
static int parse_asc_from_esds(const uint8_t *esds, size_t len, unsigned char *asc, int asc_cap) {
	if (len < 4) {
		return 0;
	}
	size_t at = 4; // version/flags
	// ES_Descriptor
	if (at >= len || esds[at] != 0x03) {
		return 0;
	}
	at++;
	size_t dlen = 0;
	for (int i = 0; i < 4 && at < len; i++) {
		dlen = (dlen << 7) | (esds[at] & 0x7f);
		at++;
		if (!(esds[at - 1] & 0x80)) {
			break;
		}
	}
	if (at + 3 > len) {
		return 0;
	}
	at += 3; // ES_ID + stream priority
	// DecoderConfigDescriptor
	if (at >= len || esds[at] != 0x04) {
		return 0;
	}
	at++;
	dlen = 0;
	for (int i = 0; i < 4 && at < len; i++) {
		dlen = (dlen << 7) | (esds[at] & 0x7f);
		at++;
		if (!(esds[at - 1] & 0x80)) {
			break;
		}
	}
	if (at + 13 > len) {
		return 0;
	}
	at += 13; // objectTypeIndication + streamType/bufferSizeDB/bitrates
	// DecoderSpecificInfo -- the AudioSpecificConfig
	if (at >= len || esds[at] != 0x05) {
		return 0;
	}
	at++;
	dlen = 0;
	for (int i = 0; i < 4 && at < len; i++) {
		dlen = (dlen << 7) | (esds[at] & 0x7f);
		at++;
		if (!(esds[at - 1] & 0x80)) {
			break;
		}
	}
	if (dlen > (size_t)asc_cap) {
		dlen = (size_t)asc_cap;
	}
	if (at + dlen > len) {
		return 0;
	}
	memcpy(asc, esds + at, dlen);
	return (int)dlen;
}

bool ampfp4_parse_init(const uint8_t *data, size_t len, ampfp4_init_t *out, char *error,
					   size_t error_size) {
	memset(out, 0, sizeof(*out));
	// Top-level: ftyp, then moov (the init segment is exactly these).
	size_t moov = 0, moov_len = 0;
	if (!find_box(data, len, 0, 0x6d6f6f76u, &moov, &moov_len)) { // "moov"
		snprintf(error, error_size, "ampfp4_init_no_moov");
		return false;
	}
	size_t trak = 0, trak_len = 0;
	if (!find_track_by_handler(data + moov, moov_len, 0x736f756eu, &trak, &trak_len)) { // "soun"
		snprintf(error, error_size, "ampfp4_init_no_sound_track");
		return false;
	}
	const uint8_t *t = data + moov + trak;
	// trak -> mdia -> minf -> stbl -> stsd
	size_t mdia, mdia_len, minf, minf_len, stbl, stbl_len, stsd, stsd_len;
	if (!find_box(t, trak_len, 0, 0x6d646961u, &mdia, &mdia_len) ||
		!find_box(t + mdia, mdia_len, 0, 0x6d696e66u, &minf, &minf_len) ||
		!find_box(t + mdia + minf, minf_len, 0, 0x7374626cu, &stbl, &stbl_len) ||
		!find_box(t + mdia + minf + stbl, stbl_len, 0, 0x73747364u, &stsd, &stsd_len)) {
		snprintf(error, error_size, "ampfp4_init_no_stsd");
		return false;
	}
	const uint8_t *sd = t + mdia + minf + stbl + stsd;
	if (stsd_len < 16) {
		snprintf(error, error_size, "ampfp4_init_short_stsd");
		return false;
	}
	// The stsd payload is a fullbox: four bytes of version/flags, then the
	// entry count, then the sample entries.
	int entry_count = (int)be32(sd + 4);
	size_t at = 8;
	if (entry_count > 4) {
		entry_count = 4;
	}
	for (int i = 0; i < entry_count && at + 8 <= stsd_len; i++) {
		uint32_t esize = be32(sd + at);
		if (esize < 8 || at + esize > stsd_len) {
			break;
		}
		const uint8_t *entry = sd + at;
		uint32_t etype = be32(entry + 4);
		size_t hs = sample_entry_header_size(etype);
		// AudioSampleEntry: six reserved bytes and the data reference index,
		// then more reserved, then channelcount (2), samplesize (2) and the
		// sample rate as a 16.16 at the end of the fixed 36 bytes.
		int channels = 0, rate = 0;
		if (esize >= 36) {
			channels = (entry[24] << 8) | entry[25];
			rate = be32(entry + 32) >> 16;
		}
		// esds: wherever the entry carries it (plain mp4a: after the header;
		// encv: after the header too, beside the sinf).
		size_t esds = 0, esds_len = 0;
		if (find_box(entry, esize, hs, 0x65736473u, &esds, &esds_len)) {
			out->asc_len =
				parse_asc_from_esds(entry + esds, esds_len, out->asc, (int)sizeof(out->asc));
		}
		// The AudioSpecificConfig, decoded: 3 bits object type, 4 bits rate
		// index, 4 bits channels. HE-AAC (type 5) signals the doubled rate in
		// those 4 bits; the ADTS header carries the base rate, which the
		// decoder then doubles again, so base_rate remembers the half.
		if (out->asc_len >= 2) {
			int aot = out->asc[0] >> 3;
			static const int rates[16] = {96000, 88200, 64000, 48000, 44100, 32000,
										  24000, 22050, 16000, 12000, 11025, 8000,
										  7350,  0,	   0,	 0};
			int index = ((out->asc[0] & 0x07) << 1) | (out->asc[1] >> 7);
			int freq = (index >= 0 && index < 13) ? rates[index] : 0;
			int ch = (out->asc[1] >> 3) & 0x0f;
			out->audio_object_type = aot;
			out->sample_rate = freq;
			out->channels = ch ? ch : 2;
			out->base_rate = (aot == 5 || aot == 29) && freq >= 2 ? freq / 2 : freq;
		}
		if (channels) {
			out->channels = channels;
		}
		if (rate) {
			out->sample_rate = out->sample_rate ? out->sample_rate : rate;
		}
		// The encryption description, when the entry is an enc*.
		size_t sinf = 0, sinf_len = 0;
		if (find_box(entry, esize, hs, 0x73696e66u, &sinf, &sinf_len)) {
			const uint8_t *sf = entry + sinf;
			if (out->desc_count < (int)(sizeof(out->desc) / sizeof(out->desc[0]))) {
				ampfp4_init_t *o = out;
				int d = o->desc_count++;
				memset(&o->desc[d], 0, sizeof(o->desc[d]));
				// frma names the real codec; schm names the scheme; tenc the
				// pattern and the IV shape.
				size_t schm = 0, schm_len = 0, schi = 0, schi_len = 0, tenc = 0, tenc_len = 0;
				if (find_box(sf, sinf_len, 0, 0x7363686du, &schm, &schm_len) &&
					schm_len >= 8) {
					const uint8_t *sp = sf + schm;
					// The scheme's fullbox header sits before the scheme type.
					snprintf(o->desc[d].scheme_type, sizeof(o->desc[d].scheme_type), "%.4s",
							 (const char *)sp + 4);
				}
				if (find_box(sf, sinf_len, 0, 0x73636869u, &schi, &schi_len) &&
					find_box(sf + schi, schi_len, 0, 0x74656e63u, &tenc, &tenc_len) &&
					tenc_len >= 20) {
					const uint8_t *tp = sf + schi + tenc;
					// tenc payload: version/flags, reserved (1), then for
					// version 1 the 1:9 pattern (1) before isEncrypted (1)
					// and the IV size (1). Version 0 drops the pattern byte;
					// the key id is not needed, the wrapper keys by URI.
					int version = tp[0];
					int iv_pos = version ? 7 : 6;
					if (version > 0) {
						o->desc[d].crypt_byte_block = tp[5] >> 4;
						o->desc[d].skip_byte_block = tp[5] & 0x0f;
					}
					o->desc[d].per_sample_iv_size = tp[iv_pos];
					if (o->desc[d].per_sample_iv_size == 0) {
						int iv_len = tp[iv_pos + 1 + 16];
						if (iv_len > 16) {
							iv_len = 16;
						}
						if (iv_pos + 1 + 16 + iv_len <= (int)tenc_len) {
							memcpy(o->desc[d].constant_iv, tp + iv_pos + 1 + 16, (size_t)iv_len);
							o->desc[d].constant_iv_len = iv_len;
						}
					}
				}
			}
		} else {
			// A plain (unencrypted) description: still record it, with a zero
			// scheme, so desc indexing stays aligned.
			if (out->desc_count < (int)(sizeof(out->desc) / sizeof(out->desc[0]))) {
				memset(&out->desc[out->desc_count], 0, sizeof(out->desc[0]));
				out->desc_count++;
			}
		}
		// The track id, from tkhd, once.
		if (out->track_id == 0) {
			size_t tkhd = 0, tkhd_len = 0;
			if (find_box(t, trak_len, 0, 0x746b6864u, &tkhd, &tkhd_len) && tkhd_len >= 20) {
				const uint8_t *tp = t + tkhd;
				out->track_id = (int)be32(tp + 12);
			}
		}
		at += esize;
	}
	if (out->track_id == 0 || out->desc_count == 0) {
		snprintf(error, error_size, "ampfp4_init_incomplete");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// media segments
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t track_id;
	int desc_index;
	uint32_t default_duration;
	size_t default_size;
	uint32_t default_flags;
	int has_base_data_offset;
	uint64_t base_data_offset;
} tfhd_t;

static void parse_tfhd(const uint8_t *p, size_t len, tfhd_t *info) {
	if (len < 8) {
		return;
	}
	uint32_t flags = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
	info->track_id = (int)be32(p + 4);
	size_t at = 8;
	if (flags & 0x01 && at + 8 <= len) {
		info->has_base_data_offset = 1;
		info->base_data_offset = be64(p + at);
		at += 8;
	}
	if (flags & 0x02 && at + 4 <= len) {
		info->desc_index = (int)be32(p + at);
		at += 4;
	}
	if (flags & 0x08 && at + 4 <= len) {
		info->default_duration = be32(p + at);
		at += 4;
	}
	if (flags & 0x10 && at + 4 <= len) {
		info->default_size = be32(p + at);
		at += 4;
	}
	if (flags & 0x20 && at + 4 <= len) {
		info->default_flags = be32(p + at);
	}
}

// The samples of one trun. Sizes default from tfhd; the flags come from the
// trun's per-sample field or, for the first sample, from first_sample_flags.
typedef struct {
	int has_duration, has_size, has_flags;
	uint32_t duration, size, flags;
	int32_t cts;
} trun_entry_t;

static int parse_trun(const uint8_t *p, size_t len, trun_entry_t *out, int out_cap,
					  int64_t *data_offset, uint32_t *first_flags) {
	if (len < 8) {
		return 0;
	}
	int version = p[0];
	uint32_t flags = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
	int count = (int)be32(p + 4);
	if (count > out_cap) {
		count = out_cap;
	}
	size_t at = 8;
	if (flags & 0x01 && at + 4 <= len) {
		*data_offset = be32s(p + at);
		at += 4;
	}
	if (flags & 0x04 && at + 4 <= len) {
		*first_flags = be32(p + at);
		at += 4;
	}
	for (int i = 0; i < count; i++) {
		out[i].has_duration = out[i].has_size = out[i].has_flags = 0;
		out[i].cts = 0;
		if (flags & 0x100 && at + 4 <= len) {
			out[i].has_duration = 1;
			out[i].duration = be32(p + at);
			at += 4;
		}
		if (flags & 0x200 && at + 4 <= len) {
			out[i].has_size = 1;
			out[i].size = be32(p + at);
			at += 4;
		}
		if (flags & 0x400 && at + 4 <= len) {
			out[i].has_flags = 1;
			out[i].flags = be32(p + at);
			at += 4;
		} else if (i == 0) {
			out[i].flags = *first_flags;
		}
		if (flags & 0x800 && at + 4 <= len) {
			out[i].cts = version == 1 ? be32s(p + at) : (int32_t)be32(p + at);
			at += 4;
		}
	}
	return count;
}

// The senc payload: per-sample IVs (when the description says so) and
// subsample entries. The IV size is tried from the description first, then
// the shapes other stream variants use -- the reference client does the same
// walk, because a few streams omit what their own boxes claim.
typedef struct {
	unsigned char iv[16];
	int iv_len;
	int sub_count;
	int sub_clear[32];
	int sub_enc[32];
} senc_entry_t;

static bool parse_senc_strict(const uint8_t *p, size_t len, int iv_size,
							  const size_t *sample_sizes, int sample_count, senc_entry_t *out) {
	if (len < 8) {
		return false;
	}
	uint32_t flags = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
	int count = (int)be32(p + 4);
	if (count > sample_count) {
		return false;
	}
	size_t at = 8;
	for (int i = 0; i < count; i++) {
		memset(&out[i], 0, sizeof(out[i]));
		if (iv_size > 0) {
			if (at + (size_t)iv_size > len) {
				return false;
			}
			memcpy(out[i].iv, p + at, (size_t)iv_size);
			out[i].iv_len = iv_size;
			at += (size_t)iv_size;
		}
		if (flags & 0x02) {
			if (at + 2 > len) {
				return false;
			}
			int pairs = (p[at] << 8) | p[at + 1];
			at += 2;
			if (pairs > 32) {
				return false;
			}
			int total = 0;
			for (int j = 0; j < pairs; j++) {
				if (at + 6 > len) {
					return false;
				}
				int clear = (p[at] << 8) | p[at + 1];
				int enc = (int)be32(p + at + 2);
				at += 6;
				total += clear + enc;
				if (total > (int)sample_sizes[i]) {
					return false;
				}
				out[i].sub_clear[j] = clear;
				out[i].sub_enc[j] = enc;
			}
			out[i].sub_count = pairs;
		}
	}
	return true;
}

static int parse_senc(const uint8_t *p, size_t len, int preferred_iv_size,
					  const size_t *sample_sizes, int sample_count, senc_entry_t *out) {
	int tried[4] = {preferred_iv_size, 8, 16, 0};
	int seen = 0;
	for (int t = 0; t < 4; t++) {
		bool dup = false;
		for (int i = 0; i < t; i++) {
			if (tried[i] == tried[t]) {
				dup = true;
			}
		}
		if (dup || tried[t] < 0) {
			continue;
		}
		if (parse_senc_strict(p, len, tried[t], sample_sizes, sample_count, out)) {
			seen = 1;
			break;
		}
	}
	return seen;
}

bool ampfp4_parse_fragment(const uint8_t *seg, size_t seg_len, uint32_t track_id,
						   const ampfp4_init_t *init, ampfp4_fragment_t *out, char *error,
						   size_t error_size) {
	memset(out, 0, sizeof(*out));
	if (error && error_size) {
		error[0] = '\0';
	}

	// Walk the top level: every moof needs the mdat that follows it.
	size_t offset = 0;
	int capacity = 0;
	for (;;) {
		amp_box_t moof_box;
		if (!next_box(seg, seg_len, offset, &moof_box)) {
			break;
		}
		offset = moof_box.box_offset + moof_box.size;
		if (moof_box.type != 0x6d6f6f66u) { // "moof"
			continue;
		}
		// The mdat (or several) between this moof and the next one.
		size_t mdat_payload = 0, mdat_payload_len = 0;
		size_t scan = moof_box.box_offset + moof_box.size;
		uint64_t mdat_data_offset = 0;
		while (scan < seg_len) {
			amp_box_t b;
			if (!next_box(seg, seg_len, scan, &b)) {
				break;
			}
			if (b.type == 0x6d646174u) { // "mdat"
				mdat_payload = b.box_offset + b.header_size;
				mdat_payload_len = b.size - b.header_size;
				mdat_data_offset = mdat_payload; // file offsets == buffer offsets here
				scan = b.box_offset + b.size;
				break;
			}
			if (b.type == 0x6d6f6f66u) { // the next moof: no mdat for this one
				break;
			}
			scan = b.box_offset + b.size;
		}
		(void)mdat_data_offset;

		const uint8_t *moof = seg + moof_box.box_offset + moof_box.header_size;
		size_t moof_len = moof_box.size - moof_box.header_size;
		size_t traf = 0, traf_len = 0;
		if (!find_box(moof, moof_len, 0, 0x74726166u, &traf, &traf_len)) {
			continue;
		}

		tfhd_t info = {0};
		info.default_size = 0;
		const uint8_t *tf = moof + traf;
		size_t inner = 0;
		const uint8_t *senc_payload = NULL;
		size_t senc_len = 0;
		int64_t data_offset = -1;
		uint32_t first_flags = 0;
		trun_entry_t entries[512];
		int entry_count = 0;
		bool has_trun = false;
		bool has_senc = false;
		amp_box_t ib;
		while (next_box(tf, traf_len, inner, &ib)) {
			const uint8_t *ip = tf + ib.box_offset + ib.header_size;
			size_t ilen = ib.size - ib.header_size;
			if (ib.type == 0x74666864u) { // tfhd
				parse_tfhd(ip, ilen, &info);
			} else if (ib.type == 0x7472756eu) { // trun
				int n = parse_trun(ip, ilen, entries + entry_count,
								   (int)(sizeof(entries) / sizeof(entries[0])) - entry_count,
								   &data_offset, &first_flags);
				entry_count += n;
				has_trun = true;
			} else if (ib.type == 0x73656e63u) { // senc
				senc_payload = ip;
				senc_len = ilen;
				has_senc = true;
			}
			inner = ib.box_offset + ib.size;
		}
		if (!has_trun || (uint32_t)info.track_id != track_id) {
			continue;
		}
		// The description this fragment's samples use, 1-based in tfhd.
		int desc = info.desc_index > 0 ? info.desc_index - 1 : 0;
		const ampfp4_init_t *o = init;
		int iv_size = desc < o->desc_count ? o->desc[desc].per_sample_iv_size : 0;

		// Sizes first, for the senc walk.
		size_t sizes[512];
		int sizes_count = entry_count < 512 ? entry_count : 512;
		for (int i = 0; i < sizes_count; i++) {
			sizes[i] = entries[i].has_size ? entries[i].size : info.default_size;
		}
		senc_entry_t senc[512];
		int senc_count = 0;
		if (has_senc) {
			senc_count = parse_senc(senc_payload, senc_len, iv_size, sizes, sizes_count, senc)
							 ? sizes_count
							 : 0;
		}

		// Sample data: the trun's data_offset is relative to the default base,
		// which for these streams is the moof start (default-base-is-moof).
		int64_t pos = data_offset >= 0 ? (int64_t)info.has_base_data_offset
											 ? (int64_t)info.base_data_offset + data_offset
											 : (int64_t)moof_box.box_offset + data_offset
									   : -1;
		if (pos < 0) {
			pos = (int64_t)mdat_payload;
		}
		(void)mdat_data_offset;
		for (int i = 0; i < entry_count; i++) {
			size_t size = entries[i].has_size ? entries[i].size : info.default_size;
			uint32_t duration = entries[i].has_duration ? entries[i].duration
														: info.default_duration;
			if (size > 0 && (size_t)pos + size <= (mdat_payload ? mdat_payload + mdat_payload_len
																: seg_len)) {
				if (out->count == capacity) {
					capacity = capacity ? capacity * 2 : 64;
					ampfp4_sample_t *grown = realloc(out->samples, (size_t)capacity * sizeof(*grown));
					if (!grown) {
						snprintf(error, error_size, "ampfp4_out_of_memory");
						return false;
					}
					out->samples = grown;
				}
				ampfp4_sample_t *s = &out->samples[out->count++];
				memset(s, 0, sizeof(*s));
				s->data = seg + pos;
				s->size = size;
				s->duration = duration;
				s->desc_index = desc;
				if (senc_count > i) {
					memcpy(s->iv, senc[i].iv, 16);
					s->iv_len = senc[i].iv_len;
					int pairs = senc[i].sub_count < 32 ? senc[i].sub_count : 32;
					for (int j = 0; j < pairs; j++) {
						s->sub[j].clear = senc[i].sub_clear[j];
						s->sub[j].enc = senc[i].sub_enc[j];
					}
					s->sub_count = pairs;
				}
				pos += (int64_t)size;
			}
		}
	}
	if (out->count == 0) {
		snprintf(error, error_size, "ampfp4_fragment_no_samples");
		return false;
	}
	return true;
}

void ampfp4_free_fragment(ampfp4_fragment_t *frag) {
	if (!frag) {
		return;
	}
	free(frag->samples);
	frag->samples = NULL;
	frag->count = 0;
}

// ---------------------------------------------------------------------------
// the wrapper's view of a sample
// ---------------------------------------------------------------------------

bool ampfp4_cbcs_split(const ampfp4_sample_t *s, ampfp4_cbcs_t *out) {
	memset(out, 0, sizeof(*out));
	if (s->sub_count == 0) {
		size_t aligned = s->size & ~(size_t)0x0f;
		if (aligned == 0) {
			return false;
		}
		out->aligned = s->data;
		out->aligned_len = aligned;
		out->tail = s->data + aligned;
		out->tail_len = s->size - aligned;
		return true;
	}
	// The protected ranges concatenated; the split needs its own storage, so
	// the caller provides it through the sample's scratch -- this writes into
	// a buffer the CALLER owns. To keep the API allocation-free, the caller
	// passes the sample whose data already sits in a writable scratch and
	// reads the result from there via the lengths this returns in out.
	// (The concatenation happens in ampstream.c, which owns scratch space.)
	size_t total = 0;
	for (int i = 0; i < s->sub_count; i++) {
		total += (size_t)s->sub[i].enc;
	}
	if (total == 0) {
		return false;
	}
	out->aligned_len = total & ~(size_t)0x0f;
	out->tail_len = total - out->aligned_len;
	return out->aligned_len > 0;
}

void ampfp4_concatenate_protected(const ampfp4_sample_t *s, uint8_t *dst, size_t dst_size) {
	size_t offset = 0, at = 0;
	for (int i = 0; i < s->sub_count && at < dst_size; i++) {
		offset += (size_t)s->sub[i].clear;
		size_t n = (size_t)s->sub[i].enc;
		if (n > dst_size - at) {
			n = dst_size - at;
		}
		if (offset + n > s->size) {
			break;
		}
		memcpy(dst + at, s->data + offset, n);
		at += n;
		offset += n;
	}
}

void ampfp4_reassemble(const ampfp4_sample_t *s, const uint8_t *plain, size_t plain_len,
					   const uint8_t *tail, size_t tail_len, uint8_t *out) {
	size_t at = 0, offset = 0, dec = 0;
	if (s->sub_count == 0) {
		memcpy(out, plain, plain_len);
		memcpy(out + plain_len, tail, tail_len);
		return;
	}
	for (int i = 0; i < s->sub_count; i++) {
		size_t clear = (size_t)s->sub[i].clear;
		size_t enc = (size_t)s->sub[i].enc;
		if (clear && offset + clear <= s->size) {
			memcpy(out + at, s->data + offset, clear);
			at += clear;
			offset += clear;
		}
		if (enc && dec + enc <= plain_len && offset + enc <= s->size) {
			memcpy(out + at, plain + dec, enc);
			at += enc;
			dec += enc;
			offset += enc;
		}
	}
	if (offset < s->size) {
		// The tail the split kept: everything past the aligned region.
		size_t rest = s->size - offset;
		if (rest > tail_len) {
			rest = tail_len;
		}
		memcpy(out + at, tail, rest);
		at += rest;
	}
	(void)plain_len;
}

// ---------------------------------------------------------------------------
// the tiny AES, for the public-key prefetch samples only
// ---------------------------------------------------------------------------

// AES-128, encryption and decryption block functions with the standard
// key schedule. Compact rather than fast: the prefetch path is a handful of
// samples per track, and this runs on a 1 GHz core for microseconds.

typedef struct {
	uint32_t rk[44]; // round keys, expanded
} aes128_t;

static const uint8_t AES_SBOX[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static uint8_t xtime(uint8_t x) {
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static void aes128_expand(aes128_t *ctx, const uint8_t key[16]) {
	static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
	for (int i = 0; i < 4; i++) {
		ctx->rk[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
					 ((uint32_t)key[4 * i + 2] << 8) | key[4 * i + 3];
	}
	for (int i = 4; i < 44; i++) {
		uint32_t t = ctx->rk[i - 1];
		if (i % 4 == 0) {
			t = ((uint32_t)AES_SBOX[(t >> 16) & 0xff] << 24) |
				((uint32_t)AES_SBOX[(t >> 8) & 0xff] << 16) |
				((uint32_t)AES_SBOX[t & 0xff] << 8) | (uint32_t)AES_SBOX[(t >> 24) & 0xff];
			t ^= (uint32_t)rcon[i / 4 - 1] << 24;
		}
		ctx->rk[i] = ctx->rk[i - 4] ^ t;
	}
}

static void aes128_encrypt_block(aes128_t *ctx, const uint8_t in[16], uint8_t out[16]) {
	uint8_t s[16];
	memcpy(s, in, 16);
	for (int round = 0; round < 10; round++) {
		for (int i = 0; i < 16; i++) {
			s[i] = AES_SBOX[s[i]];
		}
		// ShiftRows: row r (bytes r, r+4, r+8, r+12) rotates left by r.
		uint8_t tmp;
		tmp = s[1];
		s[1] = s[5];
		s[5] = s[9];
		s[9] = s[13];
		s[13] = tmp;
		tmp = s[2];
		s[2] = s[10];
		s[10] = tmp;
		tmp = s[6];
		s[6] = s[14];
		s[14] = tmp;
		tmp = s[15];
		s[15] = s[11];
		s[11] = s[7];
		s[7] = s[3];
		s[3] = tmp;
		// MixColumns, skipped in the final round.
		if (round < 9) {
			for (int c = 0; c < 4; c++) {
				uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
				s[4 * c] = (uint8_t)(xtime(a0) ^ (a1 ^ xtime(a1)) ^ a2 ^ a3);
				s[4 * c + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (a2 ^ xtime(a2)) ^ a3);
				s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (a3 ^ xtime(a3)));
				s[4 * c + 3] = (uint8_t)((a0 ^ xtime(a0)) ^ a1 ^ a2 ^ xtime(a3));
			}
		}
		uint32_t *w = &ctx->rk[4 * (round + 1)];
		for (int c = 0; c < 4; c++) {
			s[4 * c] ^= (uint8_t)(w[c] >> 24);
			s[4 * c + 1] ^= (uint8_t)(w[c] >> 16);
			s[4 * c + 2] ^= (uint8_t)(w[c] >> 8);
			s[4 * c + 3] ^= (uint8_t)w[c];
		}
	}
	memcpy(out, s, 16);
}

// The inverse round, built from the forward one by mirroring: for the handful
// of prefetch samples a table-based inverse is unnecessary, but the inverse
// sbox and inverse mixcolumns are still needed exactly.
static const uint8_t AES_INV_SBOX[256] = {
	0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
	0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
	0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
	0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
	0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
	0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
	0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
	0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
	0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
	0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
	0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
	0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
	0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
	0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
	0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
	0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

static uint8_t gf_mul(uint8_t x, uint8_t y) {
	uint8_t r = 0;
	while (y) {
		if (y & 1) {
			r ^= x;
		}
		x = xtime(x);
		y >>= 1;
	}
	return r;
}

static void aes128_decrypt_block(aes128_t *ctx, const uint8_t in[16], uint8_t out[16]) {
	uint8_t s[16];
	memcpy(s, in, 16);
	// Round 10..1: addRoundKey, invShiftRows, invSubBytes, invMixColumns.
	for (int round = 10; round >= 1; round--) {
		uint32_t *w = &ctx->rk[4 * round];
		for (int c = 0; c < 4; c++) {
			s[4 * c] ^= (uint8_t)(w[c] >> 24);
			s[4 * c + 1] ^= (uint8_t)(w[c] >> 16);
			s[4 * c + 2] ^= (uint8_t)(w[c] >> 8);
			s[4 * c + 3] ^= (uint8_t)w[c];
		}
		uint8_t tmp;
		tmp = s[13];
		s[13] = s[9];
		s[9] = s[5];
		s[5] = s[1];
		s[1] = tmp;
		tmp = s[2];
		s[2] = s[10];
		s[10] = tmp;
		tmp = s[6];
		s[6] = s[14];
		s[14] = tmp;
		tmp = s[3];
		s[3] = s[7];
		s[7] = s[11];
		s[11] = s[15];
		s[15] = tmp;
		for (int i = 0; i < 16; i++) {
			s[i] = AES_INV_SBOX[s[i]];
		}
		if (round > 1) {
			for (int c = 0; c < 4; c++) {
				uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
				s[4 * c] = (uint8_t)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
				s[4 * c + 1] =
					(uint8_t)(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
				s[4 * c + 2] =
					(uint8_t)(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
				s[4 * c + 3] =
					(uint8_t)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14));
			}
		}
	}
	for (int c = 0; c < 4; c++) {
		s[4 * c] ^= (uint8_t)(ctx->rk[c] >> 24);
		s[4 * c + 1] ^= (uint8_t)(ctx->rk[c] >> 16);
		s[4 * c + 2] ^= (uint8_t)(ctx->rk[c] >> 8);
		s[4 * c + 3] ^= (uint8_t)ctx->rk[c];
	}
	memcpy(out, s, 16);
}

// AES-CBC decrypt of `len` bytes (len a multiple of 16), IV chaining in place.
static void aes_cbc_decrypt(aes128_t *ctx, const unsigned char iv[16], const uint8_t *in,
							uint8_t *out, size_t len) {
	unsigned char chain[16];
	memcpy(chain, iv, 16);
	for (size_t at = 0; at + 16 <= len; at += 16) {
		unsigned char block[16];
		aes128_decrypt_block(ctx, in + at, block);
		for (int i = 0; i < 16; i++) {
			block[i] ^= chain[i];
		}
		memcpy(out + at, block, 16);
		memcpy(chain, in + at, 16);
	}
}

// AES-CTR over one range: the keystream is the cipher of the padded IV,
// counting up. What Apple's cenc samples use when one exists at all.
static void aes_ctr_apply(aes128_t *ctx, const unsigned char iv[16], const uint8_t *in,
						  uint8_t *out, size_t len) {
	unsigned char counter[16], stream[16];
	memcpy(counter, iv, 16);
	size_t at = 0;
	while (at < len) {
		aes128_encrypt_block(ctx, counter, stream);
		size_t n = len - at < 16 ? len - at : 16;
		for (size_t i = 0; i < n; i++) {
			out[at + i] = (uint8_t)(in[at + i] ^ stream[i]);
		}
		at += n;
		for (int i = 15; i >= 0; i--) {
			if (++counter[i] != 0) {
				break;
			}
		}
	}
}

// The CBCS 1:9-style pattern: crypt `crypt_byte_block` blocks, skip
// `skip_byte_block` blocks, repeat. The chain carries across the pattern.
static void aes_cbcs_pattern(aes128_t *ctx, const unsigned char iv[16], const uint8_t *in,
							 uint8_t *out, size_t len, int crypt_byte_block, int skip_byte_block) {
	unsigned char chain[16];
	memcpy(chain, iv, 16);
	size_t at = 0;
	while (at < len) {
		for (int b = 0; b < crypt_byte_block && at < len; b++) {
			size_t n = len - at < 16 ? len - at : 16;
			if (n == 16) {
				unsigned char block[16];
				aes128_decrypt_block(ctx, in + at, block);
				for (int i = 0; i < 16; i++) {
					block[i] ^= chain[i];
				}
				memcpy(out + at, block, 16);
				memcpy(chain, in + at, 16);
			} else {
				memcpy(out + at, in + at, n); // a partial block stays clear
			}
			at += n;
		}
		size_t skip = (size_t)skip_byte_block * 16;
		if (skip > len - at) {
			skip = len - at;
		}
		memcpy(out + at, in + at, skip);
		at += skip;
	}
}

bool ampfp4_decrypt_local(const ampfp4_sample_t *s, const unsigned char *key,
						  const unsigned char *constant_iv, int constant_iv_len, int scheme_cenc,
						  int crypt_byte_block, int skip_byte_block, uint8_t *out, size_t out_size,
						  size_t *out_len) {
	if (out_size < s->size) {
		return false;
	}
	*out_len = 0;
	aes128_t ctx;
	aes128_expand(&ctx, key);
	unsigned char iv[16] = {0};
	const unsigned char *iv_src = s->iv_len ? s->iv : constant_iv;
	int iv_src_len = s->iv_len ? s->iv_len : constant_iv_len;
	if (iv_src && iv_src_len > 0) {
		memcpy(iv, iv_src, (size_t)(iv_src_len > 16 ? 16 : iv_src_len));
	}

	if (scheme_cenc) {
		if (s->sub_count == 0) {
			aes_ctr_apply(&ctx, iv, s->data, out, s->size);
			*out_len = s->size;
			return true;
		}
		size_t offset = 0, at = 0;
		for (int i = 0; i < s->sub_count; i++) {
			size_t clear = (size_t)s->sub[i].clear, enc = (size_t)s->sub[i].enc;
			if (offset + clear > s->size) {
				break;
			}
			memcpy(out + at, s->data + offset, clear);
			at += clear;
			offset += clear;
			if (offset + enc > s->size) {
				break;
			}
			aes_ctr_apply(&ctx, iv, s->data + offset, out + at, enc);
			at += enc;
			offset += enc;
		}
		if (offset < s->size) {
			memcpy(out + at, s->data + offset, s->size - offset);
			at += s->size - offset;
		}
		*out_len = at;
		return true;
	}

	if (crypt_byte_block > 0 && skip_byte_block > 0) {
		if (s->sub_count == 0) {
			aes_cbcs_pattern(&ctx, iv, s->data, out, s->size, crypt_byte_block, skip_byte_block);
			*out_len = s->size;
			return true;
		}
		size_t offset = 0, at = 0;
		for (int i = 0; i < s->sub_count; i++) {
			size_t clear = (size_t)s->sub[i].clear, enc = (size_t)s->sub[i].enc;
			if (offset + clear > s->size) {
				break;
			}
			memcpy(out + at, s->data + offset, clear);
			at += clear;
			offset += clear;
			if (offset + enc > s->size) {
				break;
			}
			aes_cbcs_pattern(&ctx, iv, s->data + offset, out + at, enc, crypt_byte_block,
							 skip_byte_block);
			at += enc;
			offset += enc;
		}
		if (offset < s->size) {
			memcpy(out + at, s->data + offset, s->size - offset);
			at += s->size - offset;
		}
		*out_len = at;
		return true;
	}

	// Plain CBC: the aligned part to AES, the tail through untouched.
	size_t aligned = s->size & ~(size_t)0x0f;
	if (aligned > 0) {
		aes_cbc_decrypt(&ctx, iv, s->data, out, aligned);
	}
	memcpy(out + aligned, s->data + aligned, s->size - aligned);
	*out_len = s->size;
	return true;
}
