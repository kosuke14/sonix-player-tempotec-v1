#include "apple/loader.hpp"

#include "apple/guestelf.h"
#include <cstdlib>

#include <dlfcn.h>
#include <cstdio>
#include <sys/stat.h>

// Resolve one symbol, reporting failures through the loader's error string.
// Every symbol resolved here is required: open() only succeeds when all of
// them did.
#define AMP_RESOLVE(field, member)                                                \
    do {                                                                          \
        s->field = reinterpret_cast<decltype(s->field)>(                          \
            resolve(wrapper::apple::abi::mangled::member));                       \
        if (!s->field) {                                                          \
            last_error_ = std::string("dlsym ") +                                 \
                          wrapper::apple::abi::mangled::member;                   \
            return false;                                                         \
        }                                                                         \
    } while (0)

#define AMP_RESOLVE_OPTIONAL(field, member)                                       \
    s->field = reinterpret_cast<decltype(s->field)>(                              \
        resolve(wrapper::apple::abi::mangled::member))

namespace wrapper::apple {

namespace {

// The guest tree carries two library generations that share sonames:
//   lib36/  Apple Music 3.6.0 -- the auth/token/playback surface (the 6.5.3
//           build dropped it into JNI-only obfuscation)
//   lib65/  Apple Music 6.5.3 -- the decrypt surface (getSPC/saveContextKey;
//           the 3.6.0 flow is dead server-side)
// A flat tree (the older x86 layout) still works: generation subdirectories
// are used when present, else the dir itself.
std::string gen_dir(const std::string& libs_dir, const char* gen) {
    std::string dir = libs_dir + "/" + gen;
    struct stat st;
    if (::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return dir;
    }
    return libs_dir;
}

}  // namespace

bool Loader::open(const std::string& libs_dir) {
    close();
    last_error_.clear();

    // The decrypt generation: lib65/ when the tree is split, the dir itself
    // when flat.
    const std::string dec_dir = gen_dir(libs_dir, "lib65");

    // Under the qemu guest we have no Android linker: the daemon's own
    // loader (guestelf) maps the whole Apple set and resolves it against
    // the static bionic this binary was built with. The env knob switches
    // between that and a plain dlopen (for a real chroot with a working
    // linker, e.g. the x86_64 PoC on a development host).
    const char *self_load = getenv("AMP_GUEST_LOADER");
    char err_buf[256] = "";
    if (self_load && self_load[0] == '1') {
        // Order matters only in that dependencies are walked recursively;
        // loading the two entry points pulls the whole closure in.
        if (!guest_load((dec_dir + "/libandroidappmusic.so").c_str(), err_buf,
                        sizeof(err_buf))) {
            last_error_ = err_buf;
            return false;
        }
        if (!guest_load((dec_dir + "/libmediaplatform.so").c_str(), err_buf,
                        sizeof(err_buf))) {
            last_error_ = err_buf;
            return false;
        }
    } else {
        const std::string mp_path = dec_dir + "/libmediaplatform.so";
        const std::string aam_path = dec_dir + "/libandroidappmusic.so";
        h_libmediaplatform_ = dlopen(mp_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h_libmediaplatform_) {
            last_error_ = "dlopen " + mp_path + ": " + dlerror();
            return false;
        }
        h_libandroidappmusic_ = dlopen(aam_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h_libandroidappmusic_) {
            last_error_ = "dlopen " + aam_path + ": " + dlerror();
            return false;
        }
    }

    Symbols* s = &symbols_;

    // Symbol resolution: through the guest loader's namespace, or through
    // dlsym against the RTLD_GLOBAL set.
    void* handle = h_libandroidappmusic_;
    auto resolve = [&](const char *mangled) -> void* {
        if (self_load && self_load[0] == '1') {
            return guest_dlsym(mangled);
        }
        return dlsym(handle, mangled);
    };
    // The 6.5.3 session-controller surface: the same entry points Apple's
    // own Java player drives (FootHillDecryptionKey). Everything else is
    // legacy and resolved best-effort.
    AMP_RESOLVE(SVFootHillSessionCtrl_instance, SVFootHillSessionCtrl_instance);
    AMP_RESOLVE(SVFootHillSessionCtrl_getSPC, SVFootHillSessionCtrl_getSPC);
    AMP_RESOLVE(SVFootHillSessionCtrl_saveContextKey, SVFootHillSessionCtrl_saveContextKey);
    AMP_RESOLVE(SVFootHillSessionCtrl_getExistingContextKey,
                SVFootHillSessionCtrl_getExistingContextKey);
    AMP_RESOLVE(fp_sample_decrypt, fp_sample_decrypt);

    // libstoreapi is not in any DT_NEEDED closure, but it carries the
    // device-binding entries the FairPlay core requires at startup.
    if (self_load && self_load[0] == '1') {
        if (!guest_load((dec_dir + "/libstoreapi.so").c_str(), err_buf, sizeof(err_buf))) {
            std::fprintf(stderr, "loader: libstoreapi.so: %s\n", err_buf);
        }
    } else {
        h_libstoreapi_ = dlopen((dec_dir + "/libstoreapi.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h_libstoreapi_) {
            std::fprintf(stderr, "loader: libstoreapi.so: %s\n", dlerror());
        }
    }

    // Nice-to-haves: keep optional so a future APK with them private again
    // still boots.
    AMP_RESOLVE_OPTIONAL(SVFootHillSessionCtrl_decryptContext, SVFootHillSessionCtrl_decryptContext);
    AMP_RESOLVE_OPTIONAL(SVFootHillSessionCtrl_generateMovieId, SVFootHillSessionCtrl_generateMovieId);
    AMP_RESOLVE_OPTIONAL(SVFootHillSessionCtrl_resetAllContexts, SVFootHillSessionCtrl_resetAllContexts);
    AMP_RESOLVE_OPTIONAL(SVFootHillPExchange_ctor, SVFootHillPExchange_ctor);
    AMP_RESOLVE_OPTIONAL(SVFootHillPExchange_dtor, SVFootHillPExchange_dtor);
    AMP_RESOLVE_OPTIONAL(SVFootHillPContext_create, SVFootHillPContext_create);
    AMP_RESOLVE_OPTIONAL(SVFootHillPContext_kdContext, SVFootHillPContext_kdContext);
    AMP_RESOLVE_OPTIONAL(SVFootHillPContext_ckc, SVFootHillPContext_ckc);

    // mediaplatform's Data (legacy 3.6.0 CKC wrapper).
    handle = h_libmediaplatform_;
    AMP_RESOLVE_OPTIONAL(Data_ctor, Data_ctor);
    AMP_RESOLVE_OPTIONAL(Data_dtor, Data_dtor);
    AMP_RESOLVE_OPTIONAL(Data_bytes, Data_bytes);
    AMP_RESOLVE_OPTIONAL(Data_length, Data_length);

    // Device binding (libstoreapi obfuscated exports).
    handle = h_libstoreapi_;
    AMP_RESOLVE_OPTIONAL(ADIInitLibrary, ADIInitLibrary);
    AMP_RESOLVE_OPTIONAL(ADISetProvisioningPath, ADISetProvisioningPath);
    AMP_RESOLVE_OPTIONAL(ADISetAndroidID, ADISetAndroidID);
    AMP_RESOLVE_OPTIONAL(FairPlaySetAndroidID, FairPlaySetAndroidID);
    AMP_RESOLVE_OPTIONAL(FairPlayInitLibrary, FairPlayInitLibrary);
    AMP_RESOLVE_OPTIONAL(KDSetAndroidID, KDSetAndroidID);
    AMP_RESOLVE_OPTIONAL(ADIIsMachineProvisioned, ADIIsMachineProvisioned);

    // Per-library JNI_OnLoad (each System.loadLibrary equivalent).
    auto jni_onload = [&](void* dlopen_handle, const char* soname) -> void* {
        if (self_load && self_load[0] == '1') {
            return guest_dlsym_in(soname, "JNI_OnLoad");
        }
        return dlopen_handle ? dlsym(dlopen_handle, "JNI_OnLoad") : nullptr;
    };
    s->JNI_OnLoad_amskit = (decltype(s->JNI_OnLoad_amskit))jni_onload(nullptr, "libAMSKit.so");
    s->JNI_OnLoad_coreadi = (decltype(s->JNI_OnLoad_coreadi))jni_onload(nullptr, "libCoreADI.so");
    s->JNI_OnLoad_corelskd = (decltype(s->JNI_OnLoad_corelskd))jni_onload(nullptr, "libCoreLSKD.so");
    s->JNI_OnLoad_androidappmusic =
        (decltype(s->JNI_OnLoad_androidappmusic))jni_onload(nullptr, "libandroidappmusic.so");
    s->JNI_OnLoad_storeapi = (decltype(s->JNI_OnLoad_storeapi))jni_onload(nullptr, "libstoreapi.so");

    ok_ = true;
    decrypt_available_ = true;
    return true;
}

bool Loader::open_auth(const std::string& libs_dir) {
    auth_last_error_.clear();
    const char *self_load = getenv("AMP_GUEST_LOADER");
    if (!self_load || self_load[0] != '1') {
        auth_last_error_ = "auth set requires the guest loader";
        return false;
    }

    // The 3.6.0 inits touch resources a bare guest lacks; a faulting
    // constructor is skipped rather than fatal.
    guest_set_tolerant_inits(true);
    // The 3.6.0 generation lives in lib36/ (or the dir itself when flat).
    const std::string auth_dir = gen_dir(libs_dir, "lib36");
    // guest_load keys by path, so the auth generation's libandroidappmusic.so
    // coexists with the decrypt generation's file. The decrypt set must be
    // loaded FIRST (open()) so its symbols win the global scope where the
    // two generations genuinely overlap.
    char err_buf[256] = "";
    if (!guest_load((auth_dir + "/libandroidappmusic.so").c_str(), err_buf,
                    sizeof(err_buf))) {
        auth_last_error_ = err_buf;
        return false;
    }
    if (!guest_load((auth_dir + "/libstoreservicescore.so").c_str(), err_buf,
                    sizeof(err_buf))) {
        auth_last_error_ = err_buf;
        return false;
    }

    // The auth surface lives in libstoreservicescore (the 3.6.0 build exports
    // the full RequestContext/AuthenticateFlow/URLRequest stack from it);
    // resolve through that library by path.
    Symbols* s = &auth_symbols_;
    *s = Symbols{};
    auto resolve = [&](const char* mangled) -> void* {
        return guest_dlsym_in((auth_dir + "/libstoreservicescore.so").c_str(), mangled);
    };
    auto resolve_aam = [&](const char* mangled) -> void* {
        return guest_dlsym_in((auth_dir + "/libandroidappmusic.so").c_str(), mangled);
    };
    int found = 0, total = 0;
    auto probe = [&](void** slot, const char* mangled) {
        total++;
        *slot = resolve(mangled);
        if (!*slot) {
            *slot = resolve_aam(mangled);
        }
        if (*slot) {
            found++;
        } else {
            std::fprintf(stderr, "auth: unresolved %s\n", mangled);
        }
    };
    // Core bootstrap + auth surface: resolve all and count; the account
    // module reports what it actually needs missing.
    probe((void**)&s->SVFootHillSessionCtrl_instance, "SVFootHillSessionCtrl_instance");
    probe((void**)&s->fp_sample_decrypt, "fp_sample_decrypt");

    std::fprintf(stderr, "auth: %d/%d auth-generation symbols resolved\n", found, total);
    guest_set_tolerant_inits(false);
    return true;
}

void Loader::close() {
    if (h_libandroidappmusic_) {
        dlclose(h_libandroidappmusic_);
        h_libandroidappmusic_ = nullptr;
    }
    if (h_libmediaplatform_) {
        dlclose(h_libmediaplatform_);
        h_libmediaplatform_ = nullptr;
    }
    if (h_libstoreapi_) {
        dlclose(h_libstoreapi_);
        h_libstoreapi_ = nullptr;
    }
    ok_ = false;
    decrypt_available_ = false;
}

}  // namespace wrapper::apple
