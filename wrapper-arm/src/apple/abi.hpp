// Apple Music native-lib ABI bindings (armv7 / armeabi-v7a, APK 6.5.3).
//
// On armv7 (AAPCS32) C++ struct returns come back in r0, exactly like the
// x86_64 SysV layout the original wrapper targeted -- so every function
// pointer below uses the "sret as first argument" shape directly, with no
// register shuffling. The aarch64 thunks are kept only for the optional
// arm64 PoC build.
//
// Only the FairPlay decrypt path is bound here (the 6.5.3 daemon is
// decrypt-only: auth and playback dispatch stay on the remote wrapper for
// now). Everything Apple is treated as opaque `void*`; strings cross the
// boundary as pointers to the libc++ `__ndk1::basic_string` union below.
//
// Re-derive the table from an updated APK with:
//   readelf -W --dyn-syms libandroidappmusic.so | c++filt

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace wrapper::apple::abi {

// std::__ndk1::shared_ptr<T>: { T* obj; control_block* ctl; } -- 8 bytes on
// the 32-bit armv7 guest (the 64-bit PoC builders see 16; the asserts below
// pin whichever the compiler produces).
struct shared_ptr {
    void* obj = nullptr;
    void* ctrl_blk = nullptr;
};

// std::__ndk1::basic_string<char> on armv7: 12-byte long/short union; short
// mode tags with the low bit of the first byte and keeps 10 chars, long mode
// is {capacity(odd, 32-bit), size(32-bit), data}.
union std_string {
#ifdef __LP64__
    struct {
        std::uint8_t mark;
        char str[23];
    } _short;
    struct {
        std::size_t cap;
        std::size_t size;
        const char* data;
    } _long;
#else
    struct {
        std::uint8_t mark;
        char str[11];
    } _short;
    struct {
        std::uint32_t cap;
        std::uint32_t size;
        const char* data;
    } _long;
#endif
};

#if defined(__arm__) || defined(__i386__)
// 32-bit guests (armv7 and x86): pointers are 4 bytes, so shared_ptr is 8
// and the string's long form is 12.
static_assert(sizeof(std_string) == 12, "std_string ABI mismatch (32-bit)");
static_assert(sizeof(shared_ptr) == 8, "shared_ptr ABI mismatch (32-bit)");
#else
static_assert(sizeof(std_string) == 24, "std_string ABI mismatch (64-bit)");
static_assert(sizeof(shared_ptr) == 16, "shared_ptr ABI mismatch (64-bit)");
#endif

// A borrowed string view: the Apple call reads `data` directly; the buffer
// must outlive the call. We never hand Apple a string it frees.
inline std_string make_string_view(const char* s) {
    std_string out{};
    out._long.cap = 1; // odd => long mode
    out._long.size = static_cast<std::uint32_t>(std::strlen(s));
    out._long.data = s;
    return out;
}

// Same, for binary payloads (the CKC is arbitrary bytes, NULs included).
inline std_string make_string_view(const char* s, std::size_t n) {
    std_string out{};
    out._long.cap = 1; // odd => long mode
    out._long.size = static_cast<std::uint32_t>(n);
    out._long.data = s;
    return out;
}

// Copy an Apple-returned std::string out (long or short form).
inline std::string read_std_string(const std_string& s) {
    if (s._long.cap & 1u) {
        if (s._long.data && s._long.size) {
            return std::string(s._long.data, s._long.size);
        }
        return {};
    }
    return std::string(s._short.str,
                       strnlen(s._short.str, sizeof(s._short.str)));
}

// ---------------------------------------------------------------------------
// function pointer types (6.5.3 decrypt path)
//
// 6.5.3 removed the PExchange constructor export: the session controller
// builds its own contexts internally, driven by the same four entry points
// Apple's Java player uses (FootHillDecryptionKey):
//
//   getExistingContextKey(adam|server, uri, offline) -> key string (cached)
//   getSPC(a1, a2, a3, a4)                           -> SPC payload
//   [host POSTs the SPC, receives the CKC]
//   saveContextKey(a1, a2, ckc, offline)             -> key string (fresh)
//
// The returned "context key" string is the key material; the legacy
// Data/PExchange/PContext declarations below are kept only so the loader
// can still probe a 3.6.0-flavoured build if one ever appears again.
// ---------------------------------------------------------------------------

// mediaplatform::Data::Data(void const* bytes, unsigned long size, bool flag)
using fn_Data_ctor = void (*)(void* this_, const void* bytes, std::size_t size,
                              std::uint8_t flag);

// mediaplatform::Data::~Data()
using fn_Data_dtor = void (*)(void* this_);

// mediaplatform::Data::bytes() const -> const void* (borrowed)
using fn_Data_bytes = const void* (*)(const void* this_);

// mediaplatform::Data::length() const -> unsigned long
using fn_Data_length = std::size_t (*)(const void* this_);

// SVFootHillSessionCtrl::instance() -> SVFootHillSessionCtrl* (process
// singleton; never destroyed).
using fn_SVFootHillSessionCtrl_instance = void* (*)();

// SVFootHillSessionCtrl::getSPC(std::string const&, std::string const&,
//                               std::string const&, std::string const&)
// Returns the FairPlay SPC payload to POST to Apple's FPS key server.
// The exact return shape (string in sret) is confirmed at runtime by the
// decrypt layer; we model the observed shared_ptr/string sret pattern.
using fn_SVFootHillSessionCtrl_getSPC =
    void (*)(void*       hidden_return,
             void*       fh,
             std_string* arg1,
             std_string* arg2,
             std_string* arg3,
             std_string* arg4);

// SVFootHillSessionCtrl::decryptContext(std::string const&,
//                                       SVDecryptor::SVDecryptorType const&,
//                                       bool const&)
// -> shared_ptr<SVFootHillPContext>
using fn_SVFootHillSessionCtrl_decryptContext =
    void (*)(shared_ptr* ret,
             void*       fh,
             std_string* key_uri,
             const int*  decryptor_type,
             const std::uint8_t* flag);

// SVFootHillSessionCtrl::saveContextKey(std::string const&,
//                                       std::string const&,
//                                       std::string const&, bool const&)
// -> std::string  (the context key: Apple's own player stores exactly this
// string as the decryption key material). Java: saveContextKey(...)String.
using fn_SVFootHillSessionCtrl_saveContextKey =
    void (*)(std_string* ret,
             void*       fh,
             std_string* arg1,
             std_string* arg2,
             std_string* arg3,
             const std::uint8_t* flag);

// SVFootHillSessionCtrl::getExistingContextKey(std::string const&,
//                                              std::string const&,
//                                              bool const&) -> std::string
// The cached context key for (arg1, arg2), or an empty string on a miss.
using fn_SVFootHillSessionCtrl_getExistingContextKey =
    void (*)(std_string* ret,
             void*       fh,
             std_string* arg1,
             std_string* arg2,
             const std::uint8_t* flag);

// SVFootHillSessionCtrl::generateMovieId(std::string const& x5)
// Registers the movie id for a key -- only the offline/persistent path in
// Apple's own player calls this; the online path never does.
using fn_SVFootHillSessionCtrl_generateMovieId =
    void (*)(void*       fh,
             std_string* arg1,
             std_string* arg2,
             std_string* arg3,
             std_string* arg4,
             std_string* arg5);

// SVFootHillSessionCtrl::resetAllContexts()
using fn_SVFootHillSessionCtrl_resetAllContexts = void (*)(void* fh);

// SVFootHillPExchange::SVFootHillPExchange(
//     std::shared_ptr<mediaplatform::Data> const&,
//     std::string const&, unsigned long long const&)  -- x86 build
// (armv7 names the same argument `unsigned long const&`, hence the
// target-dependent width below).
#ifdef __i386__
using movie_id_t = std::uint64_t;
#else
using movie_id_t = std::uint32_t;
#endif
using fn_SVFootHillPExchange_ctor =
    void (*)(void* this_, const shared_ptr* data, std_string* key_uri,
             const movie_id_t* movie_id);

// mediaplatform::Data dtor for the CKC buffer we hand to the exchange.
using fn_SVFootHillPExchange_dtor = void (*)(void* this_);

// static SVFootHillPContext::create(
//     std::shared_ptr<SVFootHillPExchange> const&,
//     std::string const&) -> shared_ptr<SVFootHillPContext>
using fn_SVFootHillPContext_create =
    void (*)(shared_ptr* ret, const shared_ptr* exchange, std_string* key_uri);

// SVFootHillPContext::kdContext() const -> void** (double-deref, see decrypt.cpp)
using fn_SVFootHillPContext_kdContext = void** (*)(const void* pctx);

// SVFootHillPContext::ckc() const -> std::string* (borrowed)
using fn_SVFootHillPContext_ckc = std_string* (*)(const void* pctx);

// The obfuscated sample-decrypt entry (op 5 = decrypt in place).
using fn_fp_sample_decrypt = long (*)(void*       kd_context,
                                      std::uint32_t op,
                                      void*       in_out,
                                      void*       out_alias,
                                      std::size_t size);

// Device binding (libstoreapi's obfuscated exports, see runtime.cpp):
// the ADI / FairPlay / KD layers all take the 16-char Android ID, the
// library-load pair takes the provisioning directory path.
using fn_device_id_init = int (*)(const void* bytes, int len);
using fn_device_path_init = int (*)(const char* path);

// Mangled symbol names fed to dlsym.
namespace mangled {

// ---- mediaplatform ----
inline constexpr const char* Data_ctor =
    "_ZN13mediaplatform4DataC2EPKvmb";
inline constexpr const char* Data_dtor =
    "_ZN13mediaplatform4DataD2Ev";
inline constexpr const char* Data_bytes =
    "_ZNK13mediaplatform4Data5bytesEv";
inline constexpr const char* Data_length =
    "_ZNK13mediaplatform4Data6lengthEv";

// ---- FootHill (libandroidappmusic.so) ----
inline constexpr const char* SVFootHillSessionCtrl_instance =
    "_ZN21SVFootHillSessionCtrl8instanceEv";
inline constexpr const char* SVFootHillSessionCtrl_getSPC =
    "_ZN21SVFootHillSessionCtrl6getSPCERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_S8_S8_";
inline constexpr const char* SVFootHillSessionCtrl_decryptContext =
    "_ZN21SVFootHillSessionCtrl14decryptContextERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEERKN11SVDecryptor15SVDecryptorTypeERKb";
inline constexpr const char* SVFootHillSessionCtrl_saveContextKey =
    "_ZN21SVFootHillSessionCtrl14saveContextKeyERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_S8_b";
inline constexpr const char* SVFootHillSessionCtrl_getExistingContextKey =
    "_ZN21SVFootHillSessionCtrl21getExistingContextKeyERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_b";
inline constexpr const char* SVFootHillSessionCtrl_error =
    "_ZN21SVFootHillSessionCtrl5errorEv";
inline constexpr const char* SVFootHillSessionCtrl_resetAllContexts =
    "_ZN21SVFootHillSessionCtrl16resetAllContextsEv";
inline constexpr const char* SVFootHillSessionCtrl_generateMovieId =
    "_ZN21SVFootHillSessionCtrl15generateMovieIdERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_S8_S8_S8_";

inline constexpr const char* SVFootHillPExchange_ctor =
    "_ZN19SVFootHillPExchangeC2ERKNSt6__ndk110shared_ptrIN13mediaplatform4DataEEERKNS0_12basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEERKm";
inline constexpr const char* SVFootHillPExchange_dtor =
    "_ZN19SVFootHillPExchangeD2Ev";

inline constexpr const char* SVFootHillPContext_create =
    "_ZN18SVFootHillPContext6createERKNSt6__ndk110shared_ptrI19SVFootHillPExchangeEERKNS0_12basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE";
inline constexpr const char* SVFootHillPContext_kdContext =
    "_ZNK18SVFootHillPContext9kdContextEv";
inline constexpr const char* SVFootHillPContext_ckc =
    "_ZNK18SVFootHillPContext3ckcEv";

// The obfuscated sample-decrypt entry (same name in 3.6.0 and 6.5.3).
inline constexpr const char* fp_sample_decrypt =
    "NfcRKVnxuKZy04KWbdFu71Ou";

// Device binding (libstoreapi.so; recovered from the JNI shims).
inline constexpr const char* ADIInitLibrary = "kq56gsgHG6";        // ADILoadLibraryWithPath
inline constexpr const char* ADISetProvisioningPath = "nf92ngaK92";
inline constexpr const char* ADISetAndroidID = "Sph98paBcz";
inline constexpr const char* FairPlaySetAndroidID = "bsawCXd";
inline constexpr const char* FairPlayInitLibrary = "N8jdR29h";     // FairPlayLoadLibraryWithPath
inline constexpr const char* KDSetAndroidID = "hgBR8sd3n7ox0";
inline constexpr const char* ADIIsMachineProvisioned = "aslgmuibau";

// Per-library JNI_OnLoad (guest_dlsym resolves per loaded lib by the mangled
// C name; the loader resolves each with its handle below).
inline constexpr const char* JNI_OnLoad_sym = "JNI_OnLoad";

}  // namespace mangled

}  // namespace wrapper::apple::abi
