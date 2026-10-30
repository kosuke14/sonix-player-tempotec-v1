// The 3.6.0 auth generation's account module: device bootstrap, Apple ID
// sign-in (AuthenticateFlow, 2FA through the credentials handler), token
// harvest and playback dispatch.
//
// Why 3.6.0: the 6.5.3 build hides its auth behind JNI obfuscation, while
// 3.6.0 exports the whole RequestContext/AuthenticateFlow/URLRequest stack
// -- and its armeabi-v7a split carries libstoreservicescore, the ADI library
// that signs every request. The two generations share sonames; the loader
// keys them by path and the account module resolves through the 3.6.0 set
// explicitly.
//
// AAPCS32 note: class returns come back through a caller-allocated slot
// passed in r0 (the hidden first argument), so every sret below is a plain
// leading parameter -- no thunks, unlike the aarch64 original.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apple/abi.hpp"
#include "apple/loader.hpp"

namespace wrapper::apple {

// The account lifecycle, mirroring the wrapper original's states.
enum class LoginState {
    LoggedOut,
    InProgress,
    Awaiting2FA,
    Authenticated,
    Failed,
};

const char* login_state_name(LoginState s);

struct Tokens {
    std::string storefront;
    std::string dsid;
    std::string dev_token;       // the "media API" JWT (base64url . . .)
    std::string music_user_token;
    std::string apple_id;
};

struct AccountSnapshot {
    LoginState state = LoginState::LoggedOut;
    std::string apple_id;
    std::string storefront;
    std::string dsid;
    std::string dev_token;
    std::string music_user_token;
    std::string last_error;
    int last_error_code = 0;
};

// The 3.6.0 bootstrap: FootHillConfig/DeviceGUID/RequestContext/presentation
// interface + the credentials handler. One instance per process; the 3.6.0
// set keeps its own singletons separate from the 6.5.3 decrypt session.
class AuthRuntime {
public:
    // Idempotent. `libs_dir` is the guest tree root that carries lib36/.
    bool initialize(Loader& loader, const std::string& libs_dir);
    bool initialized() const { return initialized_; }

    const abi::shared_ptr& request_ctx() const;
    const abi::shared_ptr& presentation() const;
    const Symbols& sym() const { return *sym_; }
    const std::string& base_dir() const { return base_dir_; }

    static AuthRuntime& instance();

private:
    bool init_foothill(void);
    bool init_request_context(const std::string& libs_dir);
    bool init_presentation(void);

    abi::shared_ptr request_ctx_;
    abi::shared_ptr presentation_;
    abi::shared_ptr device_guid_;
    const Symbols* sym_ = nullptr;
    std::string base_dir_;
    bool initialized_ = false;
};

// Sign-in / tokens. The account owns the login worker thread; the daemon's
// IPC layer polls state() and submits the 2FA code.
class Account {
public:
    static Account& instance();

    // Starts the sign-in worker. False when a flow is already running.
    bool start_login(const std::string& apple_id, const std::string& password);
    // Submits the six-digit code for the parked flow. False when not parked.
    bool submit_2fa(const std::string& code);
    // Tries the cached Apple session (mpl_db/kvs.sqlitedb) after a daemon
    // restart: harvests fresh tokens off the stored session.
    bool try_restore_cached_session();

    LoginState state() const;
    AccountSnapshot snapshot() const;
    void logout();

private:
    friend void credentials_callback(const abi::shared_ptr* cred_request);
    // The credentials handler parks here for the 2FA code; the password the
    // flow receives is password + code (Apple's Android quirk).
    void fetch_credentials_blocking(std::string* username, std::string* password,
                                    bool needs_2fa);

    void worker_main();
    void finish_authenticated(Tokens t);
    void finish_failed(std::string message, int code);
};

// The bootstrap entry the IPC layer calls once after the loader is up:
// initializes AuthRuntime and probes the cached session. Safe to re-call.
bool auth_bootstrap(Loader& loader, const std::string& libs_dir);

// The restored/last-harvested tokens, kept process-lifetime for the IPC
// layer's /me-equivalent answers.
void auth_store_tokens(const Tokens& t);
bool auth_current_tokens(Tokens* out);

}  // namespace wrapper::apple
