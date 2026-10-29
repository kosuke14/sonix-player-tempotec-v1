// The 6.5.3 FairPlay decrypt flow.
//
// Key negotiation in 6.5.3 is caller-driven (getPersistentKey is gone):
//
//   1. getSPC(...)        the library builds the SPC request payload
//   2. the HOST posts it to Apple's FPS key server (the daemon has no
//      network stack -- the mipsel side does the TLS)
//   3. the host hands the CKC reply back
//   4. SVFootHillPExchange(CKC data, key_uri, movie_id) + PContext::create
//      produce the decrypt context, whose kdContext() feeds
//      fp_sample_decrypt -- cached per (adam_id, key_uri).
//
// The IPC opcodes mirror this split: OpDecryptBatch first asks for a cached
// context; on a miss the host drives OpGetSPC / OpApplyCKC.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apple/loader.hpp"
#include "apple/runtime.hpp"

namespace wrapper::apple {

struct DecryptResult {
    bool ok = false;
    std::vector<std::vector<std::uint8_t>> plaintexts;
    std::string error;
};

// True when a key for (adam_id, key_uri) is already cached.
bool have_cached_key(const std::string& adam_id, const std::string& key_uri);

// Step 1: build the SPC payload. Empty error string on success.
// `fps_cert` is the base64 FairPlay application certificate the host
// obtained from Apple (getSPC feeds it to CryptoDataFromBase64EncodedString
// as its 4th argument -- the reverse-engineered arg roles are:
// adam_id, key_uri, key format version, fps cert).
std::string build_spc(const Loader& loader, Runtime& runtime, const std::string& adam_id,
                      const std::string& key_uri, const std::string& fps_cert,
                      std::string* spc_out, std::string* error);

// Steps 3-5: apply the CKC the host fetched, creating and caching the
// decrypt context. Empty error string on success.
bool apply_ckc(const Loader& loader, Runtime& runtime, const std::string& adam_id,
               const std::string& key_uri, const std::uint8_t* ckc, std::size_t ckc_len,
               std::string* error);

// The batch decrypt itself: every sample in-place, plaintexts replacing the
// ciphertexts in the returned vectors.
DecryptResult decrypt_samples(const Loader& loader, Runtime& runtime,
                              const std::string& adam_id, const std::string& key_uri,
                              std::vector<std::vector<std::uint8_t>> ciphertexts);

}  // namespace wrapper::apple
