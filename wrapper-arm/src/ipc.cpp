#include "ipc.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include <unistd.h>

#include "apple/account.hpp"
#include "apple/decrypt.hpp"

namespace wrapper {

namespace {

using apple::DecryptResult;

constexpr std::uint32_t kMagic = 0x57563249u; // WV2I
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kKindRequest = 1;
constexpr std::uint16_t kKindResponse = 2;
constexpr std::size_t kMaxPayload = 256u * 1024u * 1024u;

// Decrypt-only opcodes (OpDecryptBatch keeps wrapper-v2's number).
enum Opcode : std::uint16_t {
    OpHealth = 1,
    OpDecryptBatch = 7,
    OpGetSPC = 9,     // build the FairPlay SPC request payload
    OpApplyCKC = 10,  // host fetched the CKC; create + cache the context
    OpShutdown = 8,
    OpLogin = 11,     // Apple ID sign-in (adam-style: user u16-len, pass u16-len)
    Op2FA = 12,       // submit the six-digit code
    OpMe = 13,        // account snapshot -> JSON-ish field list
    OpLogout = 14,
};

struct Frame {
    std::uint16_t kind = 0;
    std::uint32_t request_id = 0;
    std::uint16_t opcode = 0;
    std::uint16_t flags = 0;
    std::vector<std::uint8_t> payload;
};

std::uint16_t read_u16_be(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t read_u32_be(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void append_u16_be(std::vector<std::uint8_t>* out, std::uint16_t v) {
    out->push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out->push_back(static_cast<std::uint8_t>(v & 0xff));
}

void append_u32_be(std::vector<std::uint8_t>* out, std::uint32_t v) {
    out->push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    out->push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out->push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out->push_back(static_cast<std::uint8_t>(v & 0xff));
}

bool read_full(int fd, void* buf, std::size_t len) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = read(fd, p + got, len - got);
        if (n <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

bool write_full(int fd, const void* buf, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = write(fd, p + done, len - done);
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_frame(Frame* f) {
    std::uint8_t h[20];
    if (!read_full(STDIN_FILENO, h, sizeof(h))) {
        return false;
    }
    if (read_u32_be(h) != kMagic || read_u16_be(h + 4) != kVersion) {
        std::fprintf(stderr, "ipc: bad frame header\n");
        return false;
    }
    f->kind = read_u16_be(h + 6);
    f->request_id = read_u32_be(h + 8);
    f->opcode = read_u16_be(h + 12);
    f->flags = read_u16_be(h + 14);
    const std::uint32_t payload_len = read_u32_be(h + 16);
    if (payload_len > kMaxPayload) {
        std::fprintf(stderr, "ipc: payload too large: %u\n", payload_len);
        return false;
    }
    f->payload.assign(payload_len, 0);
    return payload_len == 0 || read_full(STDIN_FILENO, f->payload.data(), payload_len);
}

bool write_frame(const Frame& f) {
    std::vector<std::uint8_t> h;
    h.reserve(20);
    append_u32_be(&h, kMagic);
    append_u16_be(&h, kVersion);
    append_u16_be(&h, f.kind);
    append_u32_be(&h, f.request_id);
    append_u16_be(&h, f.opcode);
    append_u16_be(&h, f.flags);
    append_u32_be(&h, static_cast<std::uint32_t>(f.payload.size()));
    if (!write_full(STDOUT_FILENO, h.data(), h.size())) {
        return false;
    }
    return f.payload.empty() ||
           write_full(STDOUT_FILENO, f.payload.data(), f.payload.size());
}

// The reply envelope: payload[0] = 0 success / 1 error, the rest is the
// body (plaintexts, an SPC payload) or the ASCII error text.
Frame response(const Frame& req, bool ok, const std::vector<std::uint8_t>& body) {
    Frame out;
    out.kind = kKindResponse;
    out.request_id = req.request_id;
    out.opcode = req.opcode;
    out.flags = 0;
    out.payload.reserve(1 + body.size());
    out.payload.push_back(ok ? 0u : 1u);
    out.payload.insert(out.payload.end(), body.begin(), body.end());
    return out;
}

Frame error_response(const Frame& req, const std::string& text) {
    std::vector<std::uint8_t> body(text.begin(), text.end());
    return response(req, false, body);
}

// The auth surface (Apple's 3.6.0 stack) is single-threaded per process;
// every account op takes this.
std::mutex& g_auth_mutex() {
    static std::mutex m;
    return m;
}

// Shared request parse: adam_len u16, uri_len u16, [count u32], table,
// labels, bytes.
bool parse_labels(const std::vector<std::uint8_t>& payload, std::string* adam_id,
                  std::string* uri, const std::uint8_t** rest, std::size_t* rest_len) {
    if (payload.size() < 4) {
        return false;
    }
    const auto* p = payload.data();
    const std::uint16_t adam_len = read_u16_be(p);
    const std::uint16_t uri_len = read_u16_be(p + 2);
    if (adam_len == 0 || uri_len == 0 ||
        4u + static_cast<std::size_t>(adam_len) + uri_len > payload.size()) {
        return false;
    }
    adam_id->assign(reinterpret_cast<const char*>(p + 4), adam_len);
    uri->assign(reinterpret_cast<const char*>(p + 4 + adam_len), uri_len);
    *rest = p + 4 + static_cast<std::size_t>(adam_len) + uri_len;
    *rest_len = payload.size() - 4u - adam_len - uri_len;
    return true;
}

// OpDecryptBatch request: after the labels, count u32 + per-sample u32
// table + concatenated ciphertext (byte-identical to the WV2D batch).
bool parse_batch(const std::vector<std::uint8_t>& payload, std::string* adam_id,
                 std::string* uri, std::vector<std::vector<std::uint8_t>>* samples) {
    const std::uint8_t* p = nullptr;
    std::size_t rest = 0;
    if (!parse_labels(payload, adam_id, uri, &p, &rest)) {
        return false;
    }
    if (rest < 4) {
        return false;
    }
    const std::uint32_t count = read_u32_be(p);
    if (count == 0) {
        return false;
    }
    const std::size_t table_bytes = static_cast<std::size_t>(count) * 4u;
    if (rest - 4 < table_bytes) {
        return false;
    }
    const std::uint8_t* table = p + 4;
    const std::uint8_t* data = table + table_bytes;
    const std::size_t avail = rest - 4 - table_bytes;
    std::size_t total = 0;
    samples->clear();
    samples->reserve(count);
    for (std::uint32_t i = 0; i < count; i++) {
        const std::size_t n = read_u32_be(table + i * 4u);
        if (n == 0 || total + n > avail) {
            return false;
        }
        total += n;
    }
    if (total != avail) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; i++) {
        const std::size_t n = read_u32_be(table + i * 4u);
        samples->emplace_back(data, data + n);
        data += n;
    }
    return true;
}

Frame handle_health(const Frame& req, apple::Runtime& rt, apple::Loader& loader) {
    const bool ok = rt.initialized() && rt.decrypt_ready();
    std::string body = ok ? "ok" : "not_ready";
    if (!loader.ok() && !loader.last_error().empty()) {
        body = loader.last_error();
    }
    return response(req, ok, std::vector<std::uint8_t>(body.begin(), body.end()));
}

Frame handle_get_spc(const Frame& req, apple::Runtime& rt, apple::Loader& loader) {
    std::string adam_id, uri;
    const std::uint8_t* rest = nullptr;
    std::size_t rest_len = 0;
    if (!parse_labels(req.payload, &adam_id, &uri, &rest, &rest_len)) {
        return error_response(req, "invalid_frame");
    }
    // Optional trailing fps_cert: u32 length + bytes (the base64 FairPlay
    // application certificate; getSPC consumes it as its 4th argument).
    std::string fps_cert;
    if (rest_len >= 4) {
        const std::uint32_t cert_len = read_u32_be(rest);
        if (static_cast<std::size_t>(cert_len) <= rest_len - 4) {
            fps_cert.assign(reinterpret_cast<const char*>(rest + 4), cert_len);
        }
    }
    if (!rt.decrypt_ready()) {
        return error_response(req, "runtime_not_initialized");
    }
    std::string spc, error;
    {
        std::lock_guard<std::mutex> lock(rt.decrypt_mutex());
        apple::build_spc(loader, rt, adam_id, uri, fps_cert, &spc, &error);
    }
    if (!error.empty()) {
        return error_response(req, error);
    }
    return response(req, true, std::vector<std::uint8_t>(spc.begin(), spc.end()));
}

Frame handle_apply_ckc(const Frame& req, apple::Runtime& rt, apple::Loader& loader) {
    std::string adam_id, uri;
    const std::uint8_t* rest = nullptr;
    std::size_t rest_len = 0;
    if (!parse_labels(req.payload, &adam_id, &uri, &rest, &rest_len)) {
        return error_response(req, "invalid_frame");
    }
    if (rest_len < 4) {
        return error_response(req, "invalid_frame");
    }
    const std::uint32_t ckc_len = read_u32_be(rest);
    if (static_cast<std::size_t>(ckc_len) != rest_len - 4) {
        return error_response(req, "invalid_frame");
    }
    if (!rt.decrypt_ready()) {
        return error_response(req, "runtime_not_initialized");
    }
    std::string error;
    {
        std::lock_guard<std::mutex> lock(rt.decrypt_mutex());
        apple::apply_ckc(loader, rt, adam_id, uri, rest + 4, ckc_len, &error);
    }
    if (!error.empty()) {
        return error_response(req, error);
    }
    return response(req, true, {});
}

Frame handle_decrypt_batch(const Frame& req, apple::Runtime& rt, apple::Loader& loader) {
    std::string adam_id, uri;
    std::vector<std::vector<std::uint8_t>> samples;
    if (!parse_batch(req.payload, &adam_id, &uri, &samples)) {
        return error_response(req, "invalid_frame");
    }
    if (!rt.decrypt_ready()) {
        return error_response(req, "runtime_not_initialized");
    }
    const std::size_t expected = samples.size();
    DecryptResult dr;
    {
        std::lock_guard<std::mutex> lock(rt.decrypt_mutex());
        dr = apple::decrypt_samples(loader, rt, adam_id, uri, std::move(samples));
    }
    if (!dr.ok || dr.plaintexts.size() != expected) {
        return error_response(req, dr.error.empty() ? "decrypt_failed" : dr.error);
    }
    std::vector<std::uint8_t> out;
    out.reserve(4 + dr.plaintexts.size() * 4);
    append_u32_be(&out, static_cast<std::uint32_t>(dr.plaintexts.size()));
    for (const auto& plain : dr.plaintexts) {
        append_u32_be(&out, static_cast<std::uint32_t>(plain.size()));
    }
    for (const auto& plain : dr.plaintexts) {
        out.insert(out.end(), plain.begin(), plain.end());
    }
    return response(req, true, out);
}

// OpLogin payload: user u16 + pass u16 (big endian lengths).
Frame handle_login(const Frame& req) {
    const auto* p = req.payload.data();
    if (req.payload.size() < 4) {
        return error_response(req, "invalid_frame");
    }
    const std::uint16_t user_len = read_u16_be(p);
    const std::uint16_t pass_len = read_u16_be(p + 2);
    if (4u + user_len + pass_len > req.payload.size() || !user_len || !pass_len) {
        return error_response(req, "invalid_frame");
    }
    std::string user(reinterpret_cast<const char*>(p + 4), user_len);
    std::string pass(reinterpret_cast<const char*>(p + 4 + user_len), pass_len);
    std::string error;
    {
        std::lock_guard<std::mutex> lock(g_auth_mutex());
        if (!apple::Account::instance().start_login(user, pass)) {
            return error_response(req, "login_busy");
        }
    }
    return response(req, true, {});
}

Frame handle_2fa(const Frame& req) {
    std::string code(reinterpret_cast<const char*>(req.payload.data()), req.payload.size());
    std::lock_guard<std::mutex> lock(g_auth_mutex());
    if (!apple::Account::instance().submit_2fa(code)) {
        return error_response(req, "not_awaiting_2fa");
    }
    return response(req, true, {});
}

Frame handle_me(const Frame& req, apple::Runtime& rt) {
    (void)rt;
    // Poll until the flow settles (bounded; the login worker runs at TCG
    // speed, so the listener polls with its own frame cadence instead).
    auto& acct = apple::Account::instance();
    apple::LoginState st = acct.state();
    if (st == apple::LoginState::InProgress) {
        return response(req, true, {'p'});  // "poll again"
    }
    apple::AccountSnapshot snap = acct.snapshot();
    std::string body = std::string("state=") + apple::login_state_name(snap.state);
    body += "\napple_id=" + snap.apple_id;
    body += "\nstorefront=" + snap.storefront;
    body += "\ndsid=" + snap.dsid;
    if (snap.state == apple::LoginState::Authenticated) {
        body += "\ndev_token=" + snap.dev_token;
        body += "\nmusic_user_token=" + snap.music_user_token;
    }
    if (snap.state == apple::LoginState::Failed) {
        body += "\nerror=" + snap.last_error;
        body += "\nerror_code=" + std::to_string(snap.last_error_code);
    }
    return response(req, true, std::vector<std::uint8_t>(body.begin(), body.end()));
}

Frame handle_logout(const Frame& req) {
    std::lock_guard<std::mutex> lock(g_auth_mutex());
    apple::Account::instance().logout();
    return response(req, true, {});
}

}  // namespace

int run_ipc_worker(apple::Runtime& rt, apple::Loader& loader) {
    std::fprintf(stderr, "wrapper-arm: ipc worker ready\n");
    for (;;) {
        Frame req;
        if (!read_frame(&req)) {
            return 0; // the listener went away
        }
        if (req.kind != kKindRequest) {
            continue;
        }
        try {
            Frame resp;
            switch (req.opcode) {
            case OpHealth:
                resp = handle_health(req, rt, loader);
                break;
            case OpDecryptBatch:
                resp = handle_decrypt_batch(req, rt, loader);
                break;
            case OpGetSPC:
                resp = handle_get_spc(req, rt, loader);
                break;
            case OpApplyCKC:
                resp = handle_apply_ckc(req, rt, loader);
                break;
            case OpLogin:
                resp = handle_login(req);
                break;
            case Op2FA:
                resp = handle_2fa(req);
                break;
            case OpMe:
                resp = handle_me(req, rt);
                break;
            case OpLogout:
                resp = handle_logout(req);
                break;
            case OpShutdown:
                return 0;
            default:
                resp = error_response(req, "unknown_opcode");
                break;
            }
            if (!write_frame(resp)) {
                return 0;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ipc: handler threw: %s\n", e.what());
            return 1;
        } catch (...) {
            std::fprintf(stderr, "ipc: handler threw an unknown exception\n");
            return 1;
        }
    }
}

}  // namespace wrapper
