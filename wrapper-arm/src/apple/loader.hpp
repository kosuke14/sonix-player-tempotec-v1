// Apple Music 6.5.3 native-library loader (armv7, decrypt-only).
//
// dlopens the two entry libraries with RTLD_GLOBAL -- bionic resolves the
// rest of the DT_NEEDED closure transitively -- then resolves the decrypt
// path's symbols into `Symbols`.

#pragma once

#include <string>

#include "apple/abi.hpp"

namespace wrapper::apple {

struct Symbols {
    abi::fn_Data_ctor Data_ctor = nullptr;
    abi::fn_Data_dtor Data_dtor = nullptr;
    abi::fn_Data_bytes Data_bytes = nullptr;
    abi::fn_Data_length Data_length = nullptr;

    abi::fn_SVFootHillSessionCtrl_instance SVFootHillSessionCtrl_instance = nullptr;
    abi::fn_SVFootHillSessionCtrl_getSPC SVFootHillSessionCtrl_getSPC = nullptr;
    abi::fn_SVFootHillSessionCtrl_saveContextKey SVFootHillSessionCtrl_saveContextKey = nullptr;
    abi::fn_SVFootHillSessionCtrl_getExistingContextKey
        SVFootHillSessionCtrl_getExistingContextKey = nullptr;
    abi::fn_SVFootHillSessionCtrl_generateMovieId SVFootHillSessionCtrl_generateMovieId = nullptr;
    abi::fn_SVFootHillSessionCtrl_decryptContext SVFootHillSessionCtrl_decryptContext = nullptr;
    abi::fn_SVFootHillSessionCtrl_resetAllContexts SVFootHillSessionCtrl_resetAllContexts = nullptr;

    abi::fn_SVFootHillPExchange_ctor SVFootHillPExchange_ctor = nullptr;
    abi::fn_SVFootHillPExchange_dtor SVFootHillPExchange_dtor = nullptr;
    abi::fn_SVFootHillPContext_create SVFootHillPContext_create = nullptr;
    abi::fn_SVFootHillPContext_kdContext SVFootHillPContext_kdContext = nullptr;
    abi::fn_SVFootHillPContext_ckc SVFootHillPContext_ckc = nullptr;

    abi::fn_fp_sample_decrypt fp_sample_decrypt = nullptr;

    // Device binding (libstoreapi obfuscated exports, optional).
    abi::fn_device_path_init ADIInitLibrary = nullptr;
    abi::fn_device_path_init ADISetProvisioningPath = nullptr;
    abi::fn_device_id_init ADISetAndroidID = nullptr;
    abi::fn_device_id_init FairPlaySetAndroidID = nullptr;
    abi::fn_device_path_init FairPlayInitLibrary = nullptr;
    abi::fn_device_id_init KDSetAndroidID = nullptr;
    // ADIIsMachineProvisioned(storage) -- the provisioning gate probe.
    int (*ADIIsMachineProvisioned)(long storage) = nullptr;

    // Per-library JNI_OnLoad (the real app runs these at System.loadLibrary;
    // the ADI layer refuses everything until its own has run).
    int (*JNI_OnLoad_amskit)(void*, void*) = nullptr;
    int (*JNI_OnLoad_coreadi)(void*, void*) = nullptr;
    int (*JNI_OnLoad_corelskd)(void*, void*) = nullptr;
    int (*JNI_OnLoad_androidappmusic)(void*, void*) = nullptr;
    int (*JNI_OnLoad_storeapi)(void*, void*) = nullptr;
};

class Loader {
public:
    Loader() = default;
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // dlopen the Apple decrypt set from `libs_dir`. False on failure, with
    // the reason in last_error().
    bool open(const std::string& libs_dir);

    // Loads the 3.6.0 auth generation (lib36/: AuthenticateFlow, the token
    // harvest, anisette -- surfaces the 6.5.3 build only exposes through
    // JNI) and resolves its symbols into auth_symbols(). False with a
    // reason in auth_last_error(). Independent of open(): the auth set may
    // be probed even when only the decrypt generation is resident.
    bool open_auth(const std::string& libs_dir);
    void close();

    bool ok() const { return ok_; }
    // True when every decrypt-path symbol resolved.
    bool decrypt_available() const { return decrypt_available_; }
    const Symbols& sym() const { return symbols_; }
    const std::string& last_error() const { return last_error_; }
    const Symbols& auth_sym() const { return auth_symbols_; }
    const std::string& auth_last_error() const { return auth_last_error_; }

private:
    void* h_libmediaplatform_ = nullptr;
    void* h_libandroidappmusic_ = nullptr;
    void* h_libstoreapi_ = nullptr;

    Symbols symbols_;
    Symbols auth_symbols_;
    std::string auth_last_error_;
    bool ok_{false};
    bool decrypt_available_{false};
    std::string last_error_;
};

}  // namespace wrapper::apple
