#include "apple/decrypt.hpp"

#include <cctype>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "apple/abi.hpp"
#include "apple/loader.hpp"
#include "apple/runtime.hpp"

namespace wrapper::apple {

namespace {

constexpr char kPrefetchAdam[] = "0";

// The context keys stay owned by this cache (stable heap vectors); Apple's
// session keeps its own internal references, and destroying Apple-side
// contexts has been observed to corrupt the FootHill state -- we never tear
// anything down (the 3.6.0 wrapper's policy, kept).
std::mutex g_kd_cache_mu;
std::unordered_map<std::string, std::vector<std::uint8_t>> g_kd_cache;

std::string kd_cache_key(const std::string& adam_id, const std::string& key_uri) {
    std::string key;
    key.reserve(adam_id.size() + 1 + key_uri.size());
    key.append(adam_id);
    key.push_back('\n');
    key.append(key_uri);
    return key;
}

const std::vector<std::uint8_t>* find_cached_kd(const std::string& adam_id,
                                                const std::string& key_uri) {
    std::lock_guard<std::mutex> lock(g_kd_cache_mu);
    auto it = g_kd_cache.find(kd_cache_key(adam_id, key_uri));
    return it == g_kd_cache.end() ? nullptr : &it->second;
}

void store_cached_kd(const std::string& adam_id, const std::string& key_uri,
                     std::vector<std::uint8_t> kd) {
    std::lock_guard<std::mutex> lock(g_kd_cache_mu);
    g_kd_cache[kd_cache_key(adam_id, key_uri)] = std::move(kd);
}

void log_hexdump(const char* what, const std::string& s) {
    std::fprintf(stderr, "decrypt: %s len=%zu head=", what, s.size());
    const std::size_t n = s.size() < 48 ? s.size() : 48;
    for (std::size_t i = 0; i < n; i++) {
        std::fprintf(stderr, "%02x", static_cast<unsigned char>(s[i]));
    }
    std::fprintf(stderr, "%s ascii=", n < s.size() ? "..." : "");
    for (std::size_t i = 0; i < n; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::fputc(std::isprint(c) ? c : '.', stderr);
    }
    std::fprintf(stderr, "\n");
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool looks_hex(const std::string& s) {
    return s.size() % 2 == 0 && !s.empty() &&
           std::all_of(s.begin(), s.end(), [](char c) { return hex_val(c) >= 0; });
}

std::vector<std::uint8_t> hex_decode(const std::string& s) {
    std::vector<std::uint8_t> out(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); i++) {
        out[i] = static_cast<std::uint8_t>((hex_val(s[i * 2]) << 4) | hex_val(s[i * 2 + 1]));
    }
    return out;
}

bool b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= '0' && c <= '9') return true;
    return c == '+' || c == '/' || c == '=' || c == '-' || c == '_';
}

bool looks_b64(const std::string& s) {
    return s.size() >= 24 && s.size() % 4 == 0 &&
           std::all_of(s.begin(), s.end(), [](char c) { return b64_val(c); });
}

int b64_sextet(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1; // '=' padding
}

std::vector<std::uint8_t> b64_decode(const std::string& s) {
    std::vector<std::uint8_t> out;
    out.reserve(s.size() / 4 * 3);
    int acc = 0, bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const int v = b64_sextet(c);
        if (v < 0) return {};
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}

// The context key string Apple returns is the key material in some encoding.
// The PoC pins the real format down; until then try the plausible shapes and
// log the raw form either way.
std::vector<std::uint8_t> decode_context_key(const std::string& keystr) {
    const std::size_t n = keystr.size();
    if (n == 16 || n == 24 || n == 32) {
        std::fprintf(stderr, "decrypt: context key used raw (%zu bytes)\n", n);
        return std::vector<std::uint8_t>(keystr.begin(), keystr.end());
    }
    if (looks_hex(keystr) && (n / 2 == 16 || n / 2 == 24 || n / 2 == 32)) {
        std::fprintf(stderr, "decrypt: context key used hex (%zu -> %zu bytes)\n", n, n / 2);
        return hex_decode(keystr);
    }
    if (looks_b64(keystr)) {
        std::vector<std::uint8_t> dec = b64_decode(keystr);
        if (dec.size() == 16 || dec.size() == 24 || dec.size() == 32) {
            std::fprintf(stderr, "decrypt: context key used base64 (%zu -> %zu bytes)\n", n,
                         dec.size());
            return dec;
        }
    }
    std::fprintf(stderr, "decrypt: context key format unknown, using raw bytes\n");
    return std::vector<std::uint8_t>(keystr.begin(), keystr.end());
}

}  // namespace

bool have_cached_key(const std::string& adam_id, const std::string& key_uri) {
    return find_cached_kd(adam_id, key_uri) != nullptr;
}

std::string build_spc(const Loader& loader, Runtime& runtime, const std::string& adam_id,
                      const std::string& key_uri, const std::string& fps_cert,
                      std::string* spc_out, std::string* error) {
    *error = "";
    spc_out->clear();
    if (!loader.ok()) {
        *error = "libs not loaded";
        return "";
    }
    void* fh = runtime.foothill_session();
    if (!fh) {
        *error = "FooHill session not initialised";
        return "";
    }
    const Symbols& s = loader.sym();

    // Arg roles from the 6.5.3 disassembly: getSPC hashes the exchange map
    // on (something derived from) arg1/arg2, then decodes arg4 as the base64
    // FPS certificate; arg3 is the key format version string.
    abi::std_string a1 = abi::make_string_view(adam_id.c_str());
    abi::std_string a2 = abi::make_string_view(key_uri.c_str());
    abi::std_string a3 = abi::make_string_view("1");
    abi::std_string a4 = abi::make_string_view(fps_cert.c_str(), fps_cert.size());

    // The return is modelled as a string sret: the hidden pointer first,
    // then `this`. On armv7 both land in consecutive registers exactly as
    // the typedef declares.
    abi::std_string spc{};
    try {
        s.SVFootHillSessionCtrl_getSPC(&spc, fh, &a1, &a2, &a3, &a4);
    } catch (const std::exception& e) {
        *error = std::string("getSPC threw: ") + e.what();
        return "";
    } catch (...) {
        *error = "getSPC threw an unknown exception";
        return "";
    }

    *spc_out = abi::read_std_string(spc);
    if (spc_out->empty()) {
        *error = "getSPC produced an empty payload";
        return "";
    }
    return "";
}

bool apply_ckc(const Loader& loader, Runtime& runtime, const std::string& adam_id,
               const std::string& key_uri, const std::uint8_t* ckc, std::size_t ckc_len,
               std::string* error) {
    *error = "";
    if (!loader.ok()) {
        *error = "libs not loaded";
        return false;
    }
    void* fh = runtime.foothill_session();
    if (!fh) {
        *error = "FooHill session not initialised";
        return false;
    }
    const Symbols& s = loader.sym();

    try {
        // Apple's own player probes the session's context store first: the
        // key for (adam, uri) survives daemon restarts in Apple's on-disk
        // store, so a CKC roundtrip is only needed on a real miss.
        abi::std_string a1 = abi::make_string_view(adam_id.c_str());
        abi::std_string a2 = abi::make_string_view(key_uri.c_str());
        const std::uint8_t offline = 0;

        std::string keystr;
        if (s.SVFootHillSessionCtrl_getExistingContextKey) {
            abi::std_string existing{};
            s.SVFootHillSessionCtrl_getExistingContextKey(&existing, fh, &a1, &a2, &offline);
            keystr = abi::read_std_string(existing);
            if (!keystr.empty()) {
                std::fprintf(stderr, "decrypt: existing context key for adam=%s uri=%.32s\n",
                             adam_id.c_str(), key_uri.c_str());
            }
        }

        if (keystr.empty()) {
            // saveContextKey(adam_id, key_uri, ckc, offline=false) -> key.
            // The CKC travels as a (binary) std::string view.
            abi::std_string ckc_view =
                abi::make_string_view(reinterpret_cast<const char*>(ckc), ckc_len);
            abi::std_string ret{};
            s.SVFootHillSessionCtrl_saveContextKey(&ret, fh, &a1, &a2, &ckc_view, &offline);
            keystr = abi::read_std_string(ret);
        }

        if (keystr.empty()) {
            *error = "saveContextKey returned no key";
            return false;
        }
        log_hexdump("context key", keystr);

        std::vector<std::uint8_t> kd = decode_context_key(keystr);
        store_cached_kd(adam_id, key_uri, std::move(kd));
        std::fprintf(stderr, "decrypt: key cached for adam=%s uri=%.32s\n", adam_id.c_str(),
                     key_uri.c_str());
    } catch (const std::exception& e) {
        *error = std::string("apply CKC threw: ") + e.what();
        return false;
    } catch (...) {
        *error = "apply CKC threw an unknown exception";
        return false;
    }
    return true;
}

DecryptResult decrypt_samples(const Loader& loader, Runtime& runtime,
                              const std::string& adam_id, const std::string& key_uri,
                              std::vector<std::vector<std::uint8_t>> ciphertexts) {
    (void)runtime;
    DecryptResult out;
    if (!loader.ok() || !loader.decrypt_available()) {
        out.error = "decrypt chain not loaded";
        return out;
    }
    if (adam_id.empty() || key_uri.empty()) {
        out.error = "adam_id and uri are required";
        return out;
    }
    if (ciphertexts.empty()) {
        out.error = "at least one sample required";
        return out;
    }

    const std::vector<std::uint8_t>* kd = find_cached_kd(adam_id, key_uri);
    if (kd == nullptr && adam_id == kPrefetchAdam) {
        out.error = "prefetch context is not cached; decrypt prefetch samples locally";
        return out;
    }
    if (kd == nullptr) {
        // The host drives the SPC/CKC exchange first; a batch arriving
        // without a key is a protocol error on the listener side.
        out.error = "key not cached; host must run getSPC/applyCKC first";
        return out;
    }

    const Symbols& s = loader.sym();
    try {
        out.plaintexts.reserve(ciphertexts.size());
        for (auto& chunk : ciphertexts) {
            if (chunk.empty()) {
                out.error = "empty sample";
                out.plaintexts.clear();
                return out;
            }
            const long status =
                s.fp_sample_decrypt(const_cast<std::uint8_t*>(kd->data()), 5u, chunk.data(),
                                    chunk.data(), chunk.size());
            if (status < 0) {
                out.error = "fp_sample_decrypt failed status=" + std::to_string(status);
                std::fprintf(stderr, "decrypt: fp_sample_decrypt failed status=%ld\n", status);
                out.plaintexts.clear();
                return out;
            }
            out.plaintexts.push_back(std::move(chunk));
        }
    } catch (const std::exception& e) {
        out.error = std::string("decrypt threw: ") + e.what();
        out.plaintexts.clear();
        return out;
    } catch (...) {
        out.error = "decrypt threw an unknown exception";
        out.plaintexts.clear();
        return out;
    }
    out.ok = true;
    return out;
}

}  // namespace wrapper::apple
