#ifndef AMPFP4_H
#define AMPFP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The fragmented-MP4 half of Apple Music playback: the init segment of an
// enhanced-HLS stream (codec, channels, the AudioSpecificConfig the ADTS
// headers are built from, the encryption description) and each media segment
// (moof/trun/senc + mdat) sliced into the individual encrypted samples that
// the wrapper decrypts. The slicing rules here are the ones gamdl's native
// ammuxer follows, reduced to what audio streaming needs.

// One parsed init segment.
typedef struct {
	int track_id;		 // the audio track's id, for the fragment filter
	int sample_rate;	 // from the AudioSampleEntry
	int channels;
	int audio_object_type; // from the AudioSpecificConfig (2 = AAC-LC, 5 = HE)
	int base_rate;		   // the rate the ADTS header carries (SBR halves it)
	int asc_len;		   // the ASC bytes, for the HE-AAC special cases
	unsigned char asc[8];
	// Encryption descriptions, in stsd order. Index 0 is the "prefetch"
	// description Apple guards with a public key; the rest go to the wrapper.
	struct {
		int crypt_byte_block, skip_byte_block; // 1:9 patterns; 0 when plain CBC
		int per_sample_iv_size;				   // 0, 8 or 16
		unsigned char constant_iv[16];
		int constant_iv_len;
		char scheme_type[8]; // "cbcs" or "cenc"
	} desc[4];
	int desc_count;
} ampfp4_init_t;

bool ampfp4_parse_init(const uint8_t *data, size_t len, ampfp4_init_t *out, char *error,
					   size_t error_size);

// One sliced sample. `data` points into the caller's segment buffer and is
// valid as long as that buffer is.
typedef struct {
	const uint8_t *data;
	size_t size;
	uint32_t duration; // timescale units
	int desc_index;
	unsigned char iv[16];
	int iv_len;
	struct {
		int clear, enc;
	} sub[32];
	int sub_count;
} ampfp4_sample_t;

typedef struct {
	ampfp4_sample_t *samples; // malloc'd, count entries
	int count;
} ampfp4_fragment_t;

// Slices one media segment (moof + mdat as downloaded, back to back) into its
// samples. `track_id` filters the traf boxes; samples of other tracks are
// skipped. Must be freed with ampfp4_free_fragment(). False when the segment
// carries no audio samples for the track.
bool ampfp4_parse_fragment(const uint8_t *seg, size_t seg_len, uint32_t track_id,
						   const ampfp4_init_t *init, ampfp4_fragment_t *out, char *error,
						   size_t error_size);
void ampfp4_free_fragment(ampfp4_fragment_t *frag);

// The wrapper batch's view of a sample: the protected ranges concatenated and
// cut to a 16-byte boundary (the part AES-CBC actually covers), with what
// remains -- the clear ranges and the sub-block tail -- kept for reassembly.
typedef struct {
	const uint8_t *aligned;
	size_t aligned_len;
	const uint8_t *tail;
	size_t tail_len;
} ampfp4_cbcs_t;

// False when the sample carries nothing for the wrapper (all clear or too
// short to hold a block): it goes to the output unchanged. When the sample
// carries subsamples, `out->aligned` is left NULL -- the protected ranges
// must first be concatenated with ampfp4_concatenate_protected() into a
// buffer the caller owns, which then becomes out->aligned.
bool ampfp4_cbcs_split(const ampfp4_sample_t *s, ampfp4_cbcs_t *out);

// Copies the protected ranges of `s`, in subsample order, into dst (at most
// dst_size bytes). The result is what the wrapper receives as this sample's
// ciphertext, and what out->aligned names after the caller assigns it.
void ampfp4_concatenate_protected(const ampfp4_sample_t *s, uint8_t *dst, size_t dst_size);

// Rebuilds a sample from its original bytes, the decrypted aligned part and
// the subsample layout -- clear ranges copied, decrypted ranges spliced in,
// tail appended. Writes `size` bytes into out.
void ampfp4_reassemble(const ampfp4_sample_t *s, const uint8_t *plain, size_t plain_len,
					   const uint8_t *tail, size_t tail_len, uint8_t *out);

// Local decryption for the samples Apple guards with the public prefetch key
// (sample description 0): AES-128 in CBC, CBCS pattern or CTR, per the
// description's scheme. key is 16 bytes.
bool ampfp4_decrypt_local(const ampfp4_sample_t *s, const unsigned char *key,
						  const unsigned char *constant_iv, int constant_iv_len, int scheme_cenc,
						  int crypt_byte_block, int skip_byte_block, uint8_t *out, size_t out_size,
						  size_t *out_len);

#endif // AMPFP4_H
