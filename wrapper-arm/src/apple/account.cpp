// The 3.6.0 account module, part 1: bootstrap + sign-in + token harvest.
// See account.hpp for the design notes. Adapted from the wrapper-v2 daemon
// (aarch64) to armv7: the sret slot is the hidden first parameter, and the
// libc++ std::function ABI matches the libs' __ndk1 runtime.

#include "apple/account.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

#include "apple/abi.hpp"
#include "apple/guestelf.h"

namespace wrapper::apple {

namespace {

// The fake "Apple Music Android client" tuple Apple's servers see. Order:
//   ClientIdentifier / VersionIdentifier / PlatformIdentifier /
//   ProductVersion   / DeviceModel       / BuildVersion       /
//   LocaleIdentifier / LanguageIdentifier / AndroidID
constexpr char kDefaultDeviceInfo[] =
    "Music/4.9/Android/10/Samsung S9/7663313/en-US/en-US/dc28071e981c439e";

constexpr int kDeviceInfoParts = 9;

std::vector<std::string> device_info_parts() {
    const char* env = std::getenv("AMP_DEVICE_INFO");
    std::string info = env && env[0] ? env : kDefaultDeviceInfo;
    std::vector<std::string> parts;
    size_t start = 0;
    while (parts.size() < (size_t)kDeviceInfoParts) {
        size_t slash = info.find('/', start);
        if (slash == std::string::npos) {
            parts.push_back(info.substr(start));
            break;
        }
        parts.push_back(info.substr(start, slash - start));
        start = slash + 1;
    }
    return parts;
}

std::string read_std_string(const abi::std_string* s) {
    if (!s) return {};
    if (s->_long.cap & 1u) {
        if (s->_long.data && s->_long.size) {
            return std::string(s->_long.data, s->_long.size);
        }
        return {};
    }
    return std::string(s->_short.str,
                       strnlen(s->_short.str, sizeof(s->_short.str)));
}

}  // namespace

const char* login_state_name(LoginState s) {
    switch (s) {
        case LoginState::LoggedOut: return "logged_out";
        case LoginState::InProgress: return "in_progress";
        case LoginState::Awaiting2FA: return "awaiting_2fa";
        case LoginState::Authenticated: return "authenticated";
        case LoginState::Failed: return "failed";
    }
    return "unknown";
}

// --- the bootstrap ----------------------------------------------------------

namespace {

// A stable storage block for the emplaced RequestContextConfig: the control
// block pointer goes in the first word, the object sits at +32. Process
// lifetime: Apple keeps shared/weak references past init.
alignas(16) std::uint8_t g_rcc_buf[2048];

// The 3.6.0 symbols, resolved once.
struct AuthSymbols {
    // bootstrap
    int (*resolv_set_nameservers_for_net)(unsigned, const char**, int, const char*);
    void (*FootHillConfig_config)(const abi::std_string*);
    void (*DeviceGUID_instance)(abi::shared_ptr*);  // sret shared_ptr
    void (*DeviceGUID_configure)(void* ret88, void* guid, const abi::std_string*,
                                 const abi::std_string*, const unsigned*, const std::uint8_t*);
    void (*make_shared_RequestContext)(abi::shared_ptr*, const abi::std_string*);
    std::uintptr_t vtable_RequestContextConfig;
    void (*RequestContextConfig_ctor)(void*);
    void (*RCC_setBaseDirectoryPath)(void*, const abi::std_string*);
    void (*RCC_setClientIdentifier)(void*, const abi::std_string*);
    void (*RCC_setVersionIdentifier)(void*, const abi::std_string*);
    void (*RCC_setPlatformIdentifier)(void*, const abi::std_string*);
    void (*RCC_setProductVersion)(void*, const abi::std_string*);
    void (*RCC_setDeviceModel)(void*, const abi::std_string*);
    void (*RCC_setBuildVersion)(void*, const abi::std_string*);
    void (*RCC_setLocaleIdentifier)(void*, const abi::std_string*);
    void (*RCC_setLanguageIdentifier)(void*, const abi::std_string*);
    void (*RequestContextManager_configure)(const abi::shared_ptr*);
    void (*RequestContext_init)(void* ret88, void* ctx, const abi::shared_ptr* rcc);
    void (*RequestContext_setFairPlayDirectoryPath)(void*, const abi::std_string*);
    void (*RequestContext_setPresentationInterface)(void*, const abi::shared_ptr*);
    void (*make_shared_AndroidPresentationInterface)(abi::shared_ptr*);
    void (*API_setCredentialsHandler)(void*, void*);
    void (*API_handleCredentialsResponse)(void*, const abi::shared_ptr*);
    std::uintptr_t vtable_CredentialsResponse;
    void (*CredentialsResponse_ctor)(void*);
    void (*CredentialsResponse_setUserName)(void*, const abi::std_string*);
    void (*CredentialsResponse_setPassword)(void*, const abi::std_string*);
    void (*CredentialsResponse_setResponseType)(void*, const int*);
    int (*CR_requiresHSA2VerificationCode)(void*);
    void (*make_shared_AuthenticateFlow)(abi::shared_ptr*, const abi::shared_ptr*);
    void (*AuthenticateFlow_run)(void*);
    abi::shared_ptr* (*AuthenticateFlow_response)(void*);
    int (*AR_responseType)(void*);
    abi::shared_ptr* (*AR_error)(void*);
    abi::std_string* (*AR_customerMessage)(void*);
    int (*SEC_errorCode)(void*);
    const char* (*SEC_what)(void*);
    abi::std_string* (*RequestContext_storeFrontIdentifier)(void*);
    // token harvest HTTP
    void (*HTTPMessage_ctor_c1)(void*, std::string, std::string);
    void (*HTTPMessage_setHeader)(void*, const abi::std_string*, const abi::std_string*);
    void (*URLRequest_ctor)(void*, const abi::shared_ptr* msg, const abi::shared_ptr* ctx);
    void (*URLRequest_run)(void*);
    abi::shared_ptr* (*URLRequest_error)(void*);
    abi::shared_ptr* (*URLRequest_response)(void*);
    void (*URLRequest_setRequestParameter)(void*, const abi::std_string*, const abi::std_string*);
    abi::shared_ptr* (*URLResponse_underlyingResponse)(void*);
    std::string base_dir;
};

AuthSymbols g_auth{};

}  // namespace

bool AuthRuntime::initialize(Loader& loader, const std::string& libs_dir) {
    if (initialized_) {
        return true;
    }
    if (!loader.auth_ready()) {
        // The auth generation loads on demand here.
        if (!loader.open_auth(libs_dir)) {
            std::fprintf(stderr, "authrt: %s\n", loader.auth_last_error().c_str());
            return false;
        }
    }

    const std::string auth_dir = libs_dir + "/lib36";
    // The 3.6.0 surface spans three libraries; probe them in turn.
    const char* auth_libs[] = {"libstoreservicescore.so", "libandroidappmusic.so",
                               "libmediaplatform.so", nullptr};
    auto resolve = [&](void** slot, const char* mangled) {
        *slot = nullptr;
        for (int i = 0; auth_libs[i]; i++) {
            *slot = guest_dlsym_in((auth_dir + "/" + auth_libs[i]).c_str(), mangled);
            if (*slot) return;
        }
        std::fprintf(stderr, "authrt: unresolved %s\n", mangled);
    };

#define AMP_AUTHSYM(field) resolve((void**)&g_auth.field, abi::mangled::auth36::field)
    // The DNS hook ships inside the 3.6.0 closure (libcurl); the three-lib
    // chain above does not carry it.
    g_auth.resolv_set_nameservers_for_net =
        (decltype(g_auth.resolv_set_nameservers_for_net))guest_dlsym(
            abi::mangled::auth36::resolv_set_nameservers_for_net);
    AMP_AUTHSYM(FootHillConfig_config);
    AMP_AUTHSYM(DeviceGUID_instance);
    AMP_AUTHSYM(DeviceGUID_configure);
    AMP_AUTHSYM(make_shared_RequestContext);
    resolve((void**)&g_auth.vtable_RequestContextConfig, abi::mangled::auth36::vtable_RequestContextConfig);
    AMP_AUTHSYM(RequestContextConfig_ctor);
    AMP_AUTHSYM(RCC_setBaseDirectoryPath);
    AMP_AUTHSYM(RCC_setClientIdentifier);
    AMP_AUTHSYM(RCC_setVersionIdentifier);
    AMP_AUTHSYM(RCC_setPlatformIdentifier);
    AMP_AUTHSYM(RCC_setProductVersion);
    AMP_AUTHSYM(RCC_setDeviceModel);
    AMP_AUTHSYM(RCC_setBuildVersion);
    AMP_AUTHSYM(RCC_setLocaleIdentifier);
    AMP_AUTHSYM(RCC_setLanguageIdentifier);
    AMP_AUTHSYM(RequestContextManager_configure);
    AMP_AUTHSYM(RequestContext_init);
    AMP_AUTHSYM(RequestContext_setFairPlayDirectoryPath);
    AMP_AUTHSYM(RequestContext_setPresentationInterface);
    AMP_AUTHSYM(make_shared_AndroidPresentationInterface);
    AMP_AUTHSYM(API_setCredentialsHandler);
    AMP_AUTHSYM(API_handleCredentialsResponse);
    resolve((void**)&g_auth.vtable_CredentialsResponse, abi::mangled::auth36::vtable_CredentialsResponse);
    AMP_AUTHSYM(CredentialsResponse_ctor);
    AMP_AUTHSYM(CredentialsResponse_setUserName);
    AMP_AUTHSYM(CredentialsResponse_setPassword);
    AMP_AUTHSYM(CredentialsResponse_setResponseType);
    AMP_AUTHSYM(CR_requiresHSA2VerificationCode);
    AMP_AUTHSYM(make_shared_AuthenticateFlow);
    AMP_AUTHSYM(AuthenticateFlow_run);
    AMP_AUTHSYM(AuthenticateFlow_response);
    AMP_AUTHSYM(AR_responseType);
    AMP_AUTHSYM(AR_error);
    AMP_AUTHSYM(AR_customerMessage);
    AMP_AUTHSYM(SEC_errorCode);
    AMP_AUTHSYM(SEC_what);
    AMP_AUTHSYM(RequestContext_storeFrontIdentifier);
    AMP_AUTHSYM(HTTPMessage_ctor_c1);
    AMP_AUTHSYM(HTTPMessage_setHeader);
    AMP_AUTHSYM(URLRequest_ctor);
    AMP_AUTHSYM(URLRequest_run);
    AMP_AUTHSYM(URLRequest_error);
    AMP_AUTHSYM(URLRequest_response);
    AMP_AUTHSYM(URLRequest_setRequestParameter);
    AMP_AUTHSYM(URLResponse_underlyingResponse);
#undef AMP_AUTHSYM

    sym_ = &loader.auth_sym();
    base_dir_ = std::getenv("AMP_BASE_DIR") ? std::getenv("AMP_BASE_DIR")
                                            : "/data/data/com.apple.android.music/files";
    std::string mkdir_cmd = std::string("mkdir -p ") + base_dir_ + "/mpl_db";
    (void)!system(mkdir_cmd.c_str());
    // Apple's libs derive the data root from ANDROID_DATA; the guest maps
    // guest paths onto host paths, so point it at the writable base.
    setenv("ANDROID_DATA", base_dir_.c_str(), 1);
    setenv("ANDROID_ROOT", "/system", 1);

    // DNS: the 3.6.0 stack carries its own resolver hook; without it the
    // bundled libcurl cannot reach the endpoints (bionic's resolv.conf is
    // elsewhere in the static guest).
    static const char* nameservers[2] = {"8.8.8.8", "8.8.4.4"};
    if (g_auth.resolv_set_nameservers_for_net) {
        g_auth.resolv_set_nameservers_for_net(0, nameservers, 2, ".");
    }

    if (!init_foothill()) {
        return false;
    }
    if (!init_request_context(libs_dir)) {
        return false;
    }
    if (!init_presentation()) {
        return false;
    }
    initialized_ = true;
    std::fprintf(stderr, "authrt: 3.6.0 runtime ready (ctx=%p)\n", request_ctx_.obj);
    return true;
}

bool AuthRuntime::init_foothill(void) {
    auto parts = device_info_parts();
    if (parts.size() < (size_t)kDeviceInfoParts) {
        std::fprintf(stderr, "authrt: device info needs 9 slash-separated parts\n");
        return false;
    }
    auto android_id = abi::make_string_view(parts[8].c_str());
    if (g_auth.FootHillConfig_config) {
        g_auth.FootHillConfig_config(&android_id);
    }
    if (g_auth.DeviceGUID_instance) {
        // DeviceGUID::instance() -> shared_ptr<DeviceGUID> (sret in r0).
        g_auth.DeviceGUID_instance(&device_guid_);
        if (!device_guid_.obj) {
            std::fprintf(stderr, "authrt: DeviceGUID::instance returned null\n");
            return false;
        }
        alignas(16) static std::uint8_t cfg_ret[88];
        static const unsigned cfg_kind = 29;
        static const std::uint8_t cfg_flag = 1;
        auto empty = abi::make_string_view("");
        g_auth.DeviceGUID_configure(cfg_ret, device_guid_.obj, &android_id, &empty, &cfg_kind,
                                    &cfg_flag);
    }
    return true;
}

bool AuthRuntime::init_request_context(const std::string& /*libs_dir*/) {
    auto parts = device_info_parts();
    std::string mpl_db = base_dir_ + "/mpl_db";
    auto mpl_db_view = abi::make_string_view(mpl_db.c_str());
    g_auth.make_shared_RequestContext(&request_ctx_, &mpl_db_view);
    if (!request_ctx_.obj) {
        std::fprintf(stderr, "authrt: make_shared<RequestContext> returned null\n");
        return false;
    }

    // The config object: emplaced with a hand-rolled vtable pointer for the
    // shared_ptr control block (the wrapper original's init_ctx pattern).
    std::memset(g_rcc_buf, 0, sizeof(g_rcc_buf));
    *reinterpret_cast<void**>(g_rcc_buf) =
        reinterpret_cast<void*>(g_auth.vtable_RequestContextConfig + 2);
    abi::shared_ptr rcc;
    rcc.obj = g_rcc_buf + 32;
    rcc.ctrl_blk = g_rcc_buf;
    g_auth.RequestContextConfig_ctor(rcc.obj);

    g_auth.RCC_setBaseDirectoryPath(rcc.obj, &mpl_db_view);
    auto s0 = abi::make_string_view(parts[0].c_str());
    g_auth.RCC_setClientIdentifier(rcc.obj, &s0);
    auto s1 = abi::make_string_view(parts[1].c_str());
    g_auth.RCC_setVersionIdentifier(rcc.obj, &s1);
    auto s2 = abi::make_string_view(parts[2].c_str());
    g_auth.RCC_setPlatformIdentifier(rcc.obj, &s2);
    auto s3 = abi::make_string_view(parts[3].c_str());
    g_auth.RCC_setProductVersion(rcc.obj, &s3);
    auto s4 = abi::make_string_view(parts[4].c_str());
    g_auth.RCC_setDeviceModel(rcc.obj, &s4);
    auto s5 = abi::make_string_view(parts[5].c_str());
    g_auth.RCC_setBuildVersion(rcc.obj, &s5);
    auto s6 = abi::make_string_view(parts[6].c_str());
    g_auth.RCC_setLocaleIdentifier(rcc.obj, &s6);
    auto s7 = abi::make_string_view(parts[7].c_str());
    g_auth.RCC_setLanguageIdentifier(rcc.obj, &s7);

    g_auth.RequestContextManager_configure(&request_ctx_);

    alignas(16) static std::uint8_t rci_ret[88];
    g_auth.RequestContext_init(rci_ret, request_ctx_.obj, &rcc);

    auto fps_dir = abi::make_string_view(base_dir_.c_str());
    g_auth.RequestContext_setFairPlayDirectoryPath(request_ctx_.obj, &fps_dir);
    return true;
}

// Defined at the end of this file (C linkage, stable address).
extern "C" void amp_credentials_entry(const abi::shared_ptr* cred_request);

bool AuthRuntime::init_presentation(void) {
    g_auth.make_shared_AndroidPresentationInterface(&presentation_);
    if (!presentation_.obj) {
        std::fprintf(stderr, "authrt: make_shared<AndroidPresentationInterface> null\n");
        return false;
    }
    // The credentials handler: Apple's AuthenticateFlow calls back through
    // this when it wants username/password (and the 2FA code, appended to
    // the password per the Android flow's quirk).
    if (g_auth.API_setCredentialsHandler) {
        g_auth.API_setCredentialsHandler(presentation_.obj, (void*)&amp_credentials_entry);
    }
    return true;
}

AuthRuntime& AuthRuntime::instance() {
    static AuthRuntime r;
    return r;
}

const abi::shared_ptr& AuthRuntime::request_ctx() const { return request_ctx_; }
const abi::shared_ptr& AuthRuntime::presentation() const { return presentation_; }

// --- the credentials bridge -------------------------------------------------


void credentials_callback(const abi::shared_ptr* cred_request) {
    AuthRuntime& rt = AuthRuntime::instance();
    if (!rt.initialized() || !rt.presentation().obj) {
        return;
    }
    const bool need_2fa = (cred_request && cred_request->obj && g_auth.CR_requiresHSA2VerificationCode)
                              ? (g_auth.CR_requiresHSA2VerificationCode(cred_request->obj) != 0)
                              : false;
    std::fprintf(stderr, "auth: CredentialsRequest HSA2=%s\n", need_2fa ? "yes" : "no");

    std::string username, password;
    Account::instance().fetch_credentials_blocking(&username, &password, need_2fa);

    // The response: emplaced 80-byte block, control block pointer in the
    // first word, object at +24 (the wrapper original's layout).
    auto* emplace = static_cast<std::uint8_t*>(::operator new(80));
    std::memset(emplace + 8, 0, 16);
    *reinterpret_cast<void**>(emplace) =
        reinterpret_cast<void*>(g_auth.vtable_CredentialsResponse + 2);
    abi::shared_ptr cred_resp;
    cred_resp.obj = emplace + 24;
    cred_resp.ctrl_blk = emplace;

    g_auth.CredentialsResponse_ctor(cred_resp.obj);
    auto user_view = abi::make_string_view(username.c_str());
    g_auth.CredentialsResponse_setUserName(cred_resp.obj, &user_view);
    auto pass_view = abi::make_string_view(password.c_str());
    g_auth.CredentialsResponse_setPassword(cred_resp.obj, &pass_view);
    static const int kSubmit = 2;
    g_auth.CredentialsResponse_setResponseType(cred_resp.obj, &kSubmit);

    g_auth.API_handleCredentialsResponse(rt.presentation().obj, &cred_resp);
}

// --- the account ------------------------------------------------------------

namespace {

std::mutex g_acct_mu;
std::condition_variable g_acct_cv;
std::condition_variable g_2fa_cv;
LoginState g_state = LoginState::LoggedOut;
std::string g_apple_id, g_password, g_2fa_code;
bool g_2fa_submitted = false;
bool g_aborted = false;
Tokens g_tokens;
std::string g_last_error;
int g_last_error_code = 0;
std::thread g_worker;

}  // namespace

Account& Account::instance() {
    static Account a;
    return a;
}

LoginState Account::state() const {
    std::lock_guard<std::mutex> g(g_acct_mu);
    return g_state;
}

AccountSnapshot Account::snapshot() const {
    std::lock_guard<std::mutex> g(g_acct_mu);
    AccountSnapshot snap;
    snap.state = g_state;
    snap.apple_id = g_apple_id;
    if (g_state == LoginState::Authenticated) {
        snap.storefront = g_tokens.storefront;
        snap.dsid = g_tokens.dsid;
        snap.dev_token = g_tokens.dev_token;
        snap.music_user_token = g_tokens.music_user_token;
    }
    if (g_state == LoginState::Failed) {
        snap.last_error = g_last_error;
        snap.last_error_code = g_last_error_code;
    }
    return snap;
}

bool Account::start_login(const std::string& apple_id, const std::string& password) {
    std::lock_guard<std::mutex> g(g_acct_mu);
    if (g_state == LoginState::InProgress || g_state == LoginState::Awaiting2FA) {
        return false;
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_apple_id = apple_id;
    g_password = password;
    g_2fa_code.clear();
    g_2fa_submitted = false;
    g_aborted = false;
    g_tokens = Tokens{};
    g_last_error.clear();
    g_last_error_code = 0;
    g_state = LoginState::InProgress;
    g_acct_cv.notify_all();
    g_worker = std::thread(&Account::worker_main, this);
    return true;
}

bool Account::submit_2fa(const std::string& code) {
    std::lock_guard<std::mutex> g(g_acct_mu);
    if (g_state != LoginState::Awaiting2FA) {
        return false;
    }
    g_2fa_code = code;
    g_2fa_submitted = true;
    g_state = LoginState::InProgress;
    g_acct_cv.notify_all();
    g_2fa_cv.notify_all();
    return true;
}

void Account::logout() {
    {
        std::lock_guard<std::mutex> g(g_acct_mu);
        g_aborted = true;
        g_2fa_cv.notify_all();
        g_acct_cv.notify_all();
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    std::lock_guard<std::mutex> g(g_acct_mu);
    g_state = LoginState::LoggedOut;
    g_apple_id.clear();
    g_password.clear();
    g_2fa_code.clear();
    g_2fa_submitted = false;
    g_aborted = false;
    g_tokens = Tokens{};
    g_acct_cv.notify_all();
}

void Account::fetch_credentials_blocking(std::string* username, std::string* password,
                                         bool needs_2fa) {
    std::unique_lock<std::mutex> g(g_acct_mu);
    if (!needs_2fa) {
        *username = g_apple_id;
        *password = g_password;
        return;
    }
    // Park as Awaiting2FA; submit_2fa()/logout() wake us.
    g_state = LoginState::Awaiting2FA;
    g_acct_cv.notify_all();
    const auto timeout = std::chrono::minutes(5);
    g_2fa_cv.wait_for(g, timeout, [] { return g_2fa_submitted || g_aborted; });
    g_state = LoginState::InProgress;
    g_acct_cv.notify_all();
    *username = g_apple_id;
    if (g_aborted || !g_2fa_submitted) {
        *password = g_password;
        return;
    }
    // Upstream quirk: the Android AuthenticateFlow expects the 2FA code
    // appended to the password field.
    *password = g_password + g_2fa_code;
}

void Account::finish_authenticated(Tokens t) {
    std::lock_guard<std::mutex> g(g_acct_mu);
    g_tokens = std::move(t);
    g_apple_id = g_tokens.apple_id;
    // Wipe the password now the flow is done.
    g_password.assign(g_password.size(), '\0');
    g_password.clear();
    g_2fa_code.assign(g_2fa_code.size(), '\0');
    g_2fa_code.clear();
    g_2fa_submitted = false;
    g_state = LoginState::Authenticated;
    g_acct_cv.notify_all();
}

void Account::finish_failed(std::string message, int code) {
    std::lock_guard<std::mutex> g(g_acct_mu);
    g_last_error = std::move(message);
    g_last_error_code = code;
    g_password.assign(g_password.size(), '\0');
    g_password.clear();
    g_2fa_code.assign(g_2fa_code.size(), '\0');
    g_2fa_code.clear();
    g_2fa_submitted = false;
    g_state = LoginState::Failed;
    g_acct_cv.notify_all();
}

// --- the token harvest ------------------------------------------------------

namespace {

// HTTPMessage: manual emplace layout; the storage must outlive the C++ stack
// frame (URLRequest/HTTPRequest can retain references past run()).
struct HTTPMessageHolder {
    static constexpr std::size_t kSize = 4096;
    std::uint8_t* buf = nullptr;
    abi::shared_ptr sp{};

    void init(void** vtable) {
        void* raw = nullptr;
        if (::posix_memalign(&raw, 16, kSize) != 0 || !raw) {
            std::fprintf(stderr, "auth: HTTPMessageHolder alloc failed\n");
            std::abort();
        }
        buf = static_cast<std::uint8_t*>(raw);
        std::memset(buf, 0, kSize);
        *reinterpret_cast<void**>(buf) = vtable + 2;
        sp.obj = buf + 32;
        sp.ctrl_blk = buf;
    }
};

std::string extract_json_field(const std::string& body, const char* field) {
    // The answers are tiny JSON ("token": "..."); a targeted scan avoids
    // pulling a parser into the guest.
    std::string key = std::string("\"") + field + "\"";
    size_t k = body.find(key);
    if (k == std::string::npos) return {};
    k = body.find(':', k + key.size());
    if (k == std::string::npos) return {};
    k++;
    while (k < body.size() && (body[k] == ' ' || body[k] == '\t')) k++;
    if (k >= body.size() || body[k] != '"') return {};
    k++;
    std::string out;
    while (k < body.size() && body[k] != '"') {
        if (body[k] == '\\' && k + 1 < body.size()) {
            k++;
        }
        out.push_back(body[k]);
        k++;
    }
    return out;
}

std::string base64url_decode(const std::string& in) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1, 52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, 15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, 41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
    std::string out;
    int bits = 0, val = 0;
    for (unsigned char c : in) {
        int v = T[c];
        if (v < 0) {
            if (c == '=' || c == '\r' || c == '\n') continue;
            return {};
        }
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((val >> bits) & 0xff));
        }
    }
    return out;
}

std::string jwt_payload_field(const std::string& jwt, const char* field) {
    // header.payload.signature
    size_t p1 = jwt.find('.');
    if (p1 == std::string::npos) return {};
    size_t p2 = jwt.find('.', p1 + 1);
    if (p2 == std::string::npos) return {};
    std::string json = base64url_decode(jwt.substr(p1 + 1, p2 - p1 - 1));
    return extract_json_field(json, field);
}

struct UrlResult {
    std::string body;
    int error_code = 0;
    std::string error_what;
    bool ok = false;
};

UrlResult run_request(const abi::shared_ptr& req_ctx, HTTPMessageHolder& msg,
                      const std::vector<std::pair<std::string, std::string>>& params) {
    UrlResult r;
    constexpr std::size_t kUrlReqSize = 8192;
    void* url_req_raw = nullptr;
    if (::posix_memalign(&url_req_raw, 16, kUrlReqSize) != 0 || !url_req_raw) {
        std::fprintf(stderr, "auth: url request alloc failed\n");
        return r;
    }
    std::uint8_t* url_req = static_cast<std::uint8_t*>(url_req_raw);
    std::memset(url_req, 0, kUrlReqSize);
    g_auth.URLRequest_ctor(url_req, &msg.sp, &req_ctx);
    for (const auto& p : params) {
        auto n = abi::make_string_view(p.first.c_str());
        auto v = abi::make_string_view(p.second.c_str());
        g_auth.URLRequest_setRequestParameter(url_req, &n, &v);
    }
    g_auth.URLRequest_run(url_req);

    abi::shared_ptr* err = g_auth.URLRequest_error(url_req);
    if (err && err->obj) {
        r.error_code = g_auth.SEC_errorCode(err->obj);
        const char* what = g_auth.SEC_what(err->obj);
        if (what) r.error_what = what;
        return r;
    }
    abi::shared_ptr* resp = g_auth.URLRequest_response(url_req);
    if (!resp || !resp->obj) {
        return r;
    }
    abi::shared_ptr* inner = g_auth.URLResponse_underlyingResponse(resp->obj);
    if (!inner || !inner->obj) {
        return r;
    }
    // The HTTPMessage body pointer sits at +48 in the 3.6.0 layout.
    void** data_slot = reinterpret_cast<void**>(static_cast<std::uint8_t*>(inner->obj) + 48);
    const char* body = data_slot ? reinterpret_cast<const char*>(*data_slot) : nullptr;
    if (body) {
        r.body = body;
        r.ok = true;
    }
    return r;
}

std::string harvest_storefront(const abi::shared_ptr& req_ctx) {
    if (g_auth.RequestContext_storeFrontIdentifier) {
        abi::std_string* sf = g_auth.RequestContext_storeFrontIdentifier(req_ctx.obj);
        std::string s = read_std_string(sf);
        if (!s.empty()) return s;
    }
    return "us";
}

bool harvest_tokens(const abi::shared_ptr& req_ctx, const std::string& guid, Tokens* out) {
    // 1. the developer ("media API") token
    HTTPMessageHolder dev_msg;
    dev_msg.init(nullptr);
    g_auth.HTTPMessage_ctor_c1(dev_msg.sp.obj, std::string("https://sf-api-token-service.itunes.apple.com/apiToken"),
                               std::string("GET"));
    UrlResult dev = run_request(req_ctx, dev_msg, {{"guid", guid}});
    if (!dev.ok || dev.error_code) {
        std::fprintf(stderr, "auth: dev token failed code=%d what=%s\n", dev.error_code,
                     dev.error_what.c_str());
        return false;
    }
    out->dev_token = extract_json_field(dev.body, "token");
    if (out->dev_token.empty()) {
        std::fprintf(stderr, "auth: dev token missing in response (%.80s)\n", dev.body.c_str());
        return false;
    }

    // 2. the per-account music user token, asserted against the dev token
    HTTPMessageHolder mus_msg;
    mus_msg.init(nullptr);
    g_auth.HTTPMessage_ctor_c1(mus_msg.sp.obj,
                               std::string("https://play.itunes.apple.com/WebObjects/MZPlay.woa/wa/createMusicToken"),
                               std::string("POST"));
    UrlResult mus = run_request(req_ctx, mus_msg,
                                {{"guid", guid}, {"assertion", out->dev_token}});
    if (!mus.ok || mus.error_code) {
        std::fprintf(stderr, "auth: music token failed code=%d what=%s\n", mus.error_code,
                     mus.error_what.c_str());
        return false;
    }
    out->music_user_token = extract_json_field(mus.body, "musicToken");
    if (out->music_user_token.empty()) {
        std::fprintf(stderr, "auth: music token missing in response (%.80s)\n", mus.body.c_str());
        return false;
    }

    if (auto d = jwt_payload_field(out->dev_token, "dsid"); !d.empty()) {
        out->dsid = std::move(d);
    }
    out->storefront = harvest_storefront(req_ctx);
    return true;
}

// The cached-session marker Apple writes on the first successful auth.
bool warm_session_present(const std::string& base_dir) {
    std::string kvs = base_dir + "/mpl_db/kvs.sqlitedb";
    struct stat st;
    if (::stat(kvs.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

}  // namespace

void Account::worker_main() {
    AuthRuntime& rt = AuthRuntime::instance();
    if (!rt.initialized() || !rt.request_ctx().obj) {
        finish_failed("auth runtime not initialized", -1);
        return;
    }
    const abi::shared_ptr req_ctx = rt.request_ctx();
    if (!req_ctx.obj) { return; }

    // Stale session files confuse the flow; Apple's own login removes them.
    if (!rt.base_dir().empty()) {
        (void)!unlink((rt.base_dir() + "/STOREFRONT_ID").c_str());
        (void)!unlink((rt.base_dir() + "/MUSIC_TOKEN").c_str());
    }

    const AuthSymbols& s = g_auth;
    abi::shared_ptr flow;
    s.make_shared_AuthenticateFlow(&flow, &req_ctx);
    if (!flow.obj) {
        finish_failed("make_shared<AuthenticateFlow> returned null", -3);
        return;
    }
    s.AuthenticateFlow_run(flow.obj);

    abi::shared_ptr* resp = s.AuthenticateFlow_response(flow.obj);
    if (!resp || !resp->obj) {
        finish_failed("AuthenticateFlow::response() returned null", -4);
        return;
    }

    int resp_type = s.AR_responseType(resp->obj);
    if (resp_type != 6) {
        std::string msg;
        int code = 0;
        abi::shared_ptr* err = s.AR_error(resp->obj);
        if (err && err->obj) {
            code = s.SEC_errorCode(err->obj);
            const char* what = s.SEC_what(err->obj);
            if (what) msg = what;
        }
        if (msg.empty()) {
            abi::std_string* customer = s.AR_customerMessage(resp->obj);
            if (customer) msg = read_std_string(customer);
        }
        if (msg.empty()) {
            msg = "Apple returned response type " + std::to_string(resp_type);
        }
        finish_failed(std::move(msg), code);
        return;
    }

    Tokens t;
    t.apple_id = g_apple_id;
    std::string guid;
    {
        // The device GUID string Apple's requests carry; fall back to the
        // android id from the tuple.
        auto parts = device_info_parts();
        guid = parts.size() >= 9 ? parts[8] : "0";
    }
    if (!harvest_tokens(req_ctx, guid, &t)) {
        finish_failed("token harvest failed after flow.run() succeeded", -5);
        return;
    }
    auth_store_tokens(t);
    finish_authenticated(std::move(t));
}

bool Account::try_restore_cached_session() {
    AuthRuntime& rt = AuthRuntime::instance();
    if (!rt.initialized() || !warm_session_present(rt.base_dir())) {
        return false;
    }
    std::lock_guard<std::mutex> g(g_acct_mu);
    if (g_state != LoginState::LoggedOut) {
        return false;
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    const abi::shared_ptr req_ctx = rt.request_ctx();
    if (!req_ctx.obj) {
        return false;
    }
    Tokens t;
    std::string guid;
    auto parts = device_info_parts();
    guid = parts.size() >= 9 ? parts[8] : "0";
    if (!harvest_tokens(req_ctx, guid, &t)) {
        std::fprintf(stderr, "auth: cached session present but token harvest failed\n");
        return false;
    }
    if (const char* label = std::getenv("AMP_APPLE_ID")) {
        if (label[0]) t.apple_id = label;
    }
    auth_store_tokens(t);
    g_tokens = std::move(t);
    g_apple_id = g_tokens.apple_id;
    g_state = LoginState::Authenticated;
    g_acct_cv.notify_all();
    std::fprintf(stderr, "auth: restored cached session (%s)\n", g_apple_id.c_str());
    return true;
}

// --- module-level glue ------------------------------------------------------

namespace {

Tokens g_stored_tokens;
std::mutex g_stored_mu;

}  // namespace

void auth_store_tokens(const Tokens& t) {
    std::lock_guard<std::mutex> g(g_stored_mu);
    g_stored_tokens = t;
}

bool auth_current_tokens(Tokens* out) {
    std::lock_guard<std::mutex> g(g_stored_mu);
    if (g_stored_tokens.dev_token.empty() && g_stored_tokens.music_user_token.empty()) {
        return false;
    }
    *out = g_stored_tokens;
    return true;
}

bool auth_bootstrap(Loader& loader, const std::string& libs_dir) {
    if (!AuthRuntime::instance().initialize(loader, libs_dir)) {
        return false;
    }
    // A warm session from a previous run means the daemon can answer /me
    // without a fresh sign-in; the account restores it in the background so
    // the first IPC answer is not delayed.
    std::thread([] { Account::instance().try_restore_cached_session(); }).detach();
    return true;
}

}  // namespace wrapper::apple

// Defined at file scope: the C entry the presentation interface holds.
extern "C" void amp_credentials_entry(const wrapper::apple::abi::shared_ptr* cred_request) {
    wrapper::apple::credentials_callback(cred_request);
}
