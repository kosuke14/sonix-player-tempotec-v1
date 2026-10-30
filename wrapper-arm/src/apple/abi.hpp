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

// Borrowed view of a std::__ndk1::vector<shared_ptr<T>> the Apple lib hands
// back. The 3.6.0 armv7 accessors return these as a direct pointer (same
// convention as title()/string getters), not via the Itanium sret hidden
// first argument.
struct std_vector {
    void* begin;
    void* end;
    void* cap;
};

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

// ---------------------------------------------------------------------------
// The 3.6.0 auth generation (libstoreservicescore.so / libandroidappmusic.so
// of Apple Music 3.6.0-beta): the RequestContext/AuthenticateFlow/URLRequest
// stack the 6.5.3 build only exposes through JNI. Names lifted from the
// wrapper-v2 table (aarch64 -- unchanged for armv7, the mangling is ABI-
// independent).
// ---------------------------------------------------------------------------
namespace auth36 {
inline constexpr const char* resolv_set_nameservers_for_net =
    "_resolv_set_nameservers_for_net";
inline constexpr const char* FootHillConfig_config =
    "_ZN14FootHillConfig6configERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE";
inline constexpr const char* DeviceGUID_instance =
    "_ZN17storeservicescore10DeviceGUID8instanceEv";
inline constexpr const char* DeviceGUID_configure =
    "_ZN17storeservicescore10DeviceGUID9configureERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_RKjRKb";
inline constexpr const char* make_shared_RequestContext =
    "_ZNSt6__ndk110shared_ptrIN17storeservicescore14RequestContextEE11make_sharedIJRNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEEEEES3_DpOT_";
inline constexpr const char* vtable_RequestContextConfig =
    "_ZTVNSt6__ndk120__shared_ptr_emplaceIN17storeservicescore20RequestContextConfigENS_9allocatorIS2_EEEE";
inline constexpr const char* RequestContextConfig_ctor =
    "_ZN17storeservicescore20RequestContextConfigC2Ev";
inline constexpr const char* RCC_setBaseDirectoryPath =
    "_ZN17storeservicescore20RequestContextConfig20setBaseDirectoryPathERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setClientIdentifier =
    "_ZN17storeservicescore20RequestContextConfig19setClientIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setVersionIdentifier =
    "_ZN17storeservicescore20RequestContextConfig20setVersionIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setPlatformIdentifier =
    "_ZN17storeservicescore20RequestContextConfig21setPlatformIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setProductVersion =
    "_ZN17storeservicescore20RequestContextConfig17setProductVersionERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setDeviceModel =
    "_ZN17storeservicescore20RequestContextConfig14setDeviceModelERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setBuildVersion =
    "_ZN17storeservicescore20RequestContextConfig15setBuildVersionERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setLocaleIdentifier =
    "_ZN17storeservicescore20RequestContextConfig19setLocaleIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setLanguageIdentifier =
    "_ZN17storeservicescore20RequestContextConfig21setLanguageIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RequestContextManager_configure =
    "_ZN21RequestContextManager9configureERKNSt6__ndk110shared_ptrIN17storeservicescore14RequestContextEEE";
inline constexpr const char* RequestContext_init =
    "_ZN17storeservicescore14RequestContext4initERKNSt6__ndk110shared_ptrINS_20RequestContextConfigEEE";
inline constexpr const char* RequestContext_setFairPlayDirectoryPath =
    "_ZN17storeservicescore14RequestContext24setFairPlayDirectoryPathERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RequestContext_setPresentationInterface =
    "_ZN17storeservicescore14RequestContext24setPresentationInterfaceERKNSt6__ndk110shared_ptrINS_21PresentationInterfaceEEE";
inline constexpr const char* API_setDialogHandler =
    "_ZN20androidstoreservices28AndroidPresentationInterface16setDialogHandlerEPFvxNSt6__ndk110shared_ptrIN17storeservicescore14ProtocolDialogEEENS2_INS_36AndroidProtocolDialogResponseHandlerEEEE";
inline constexpr const char* API_handleProtocolDialogResponse =
    "_ZN20androidstoreservices28AndroidPresentationInterface28handleProtocolDialogResponseERKxRKNSt6__ndk110shared_ptrIN17storeservicescore22ProtocolDialogResponseEEE";
inline constexpr const char* make_shared_AndroidPresentationInterface =
    "_ZNSt6__ndk110shared_ptrIN20androidstoreservices28AndroidPresentationInterfaceEE11make_sharedIJEEES3_DpOT_";
inline constexpr const char* API_setCredentialsHandler =
    "_ZN20androidstoreservices28AndroidPresentationInterface21setCredentialsHandlerEPFvNSt6__ndk110shared_ptrIN17storeservicescore18CredentialsRequestEEENS2_INS_33AndroidCredentialsResponseHandlerEEEE";
inline constexpr const char* API_handleCredentialsResponse =
    "_ZN20androidstoreservices28AndroidPresentationInterface25handleCredentialsResponseERKNSt6__ndk110shared_ptrIN17storeservicescore19CredentialsResponseEEE";
inline constexpr const char* vtable_CredentialsResponse =
    "_ZTVNSt6__ndk120__shared_ptr_emplaceIN17storeservicescore19CredentialsResponseENS_9allocatorIS2_EEEE";
inline constexpr const char* CredentialsResponse_ctor =
    "_ZN17storeservicescore19CredentialsResponseC1Ev";
inline constexpr const char* CredentialsResponse_setUserName =
    "_ZN17storeservicescore19CredentialsResponse11setUserNameERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* CredentialsResponse_setPassword =
    "_ZN17storeservicescore19CredentialsResponse11setPasswordERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* CredentialsResponse_setResponseType =
    "_ZN17storeservicescore19CredentialsResponse15setResponseTypeENS0_12ResponseTypeE";
inline constexpr const char* CR_requiresHSA2VerificationCode =
    "_ZNK17storeservicescore18CredentialsRequest28requiresHSA2VerificationCodeEv";
inline constexpr const char* make_shared_AuthenticateFlow =
    "_ZNSt6__ndk110shared_ptrIN17storeservicescore16AuthenticateFlowEE11make_sharedIJRNS0_INS1_14RequestContextEEEEEES3_DpOT_";
inline constexpr const char* AuthenticateFlow_run =
    "_ZN17storeservicescore16AuthenticateFlow3runEv";
inline constexpr const char* AuthenticateFlow_response =
    "_ZNK17storeservicescore16AuthenticateFlow8responseEv";
inline constexpr const char* AR_responseType =
    "_ZNK17storeservicescore20AuthenticateResponse12responseTypeEv";
inline constexpr const char* AR_error =
    "_ZNK17storeservicescore20AuthenticateResponse5errorEv";
inline constexpr const char* AR_customerMessage =
    "_ZNK17storeservicescore20AuthenticateResponse15customerMessageEv";
inline constexpr const char* SEC_errorCode =
    "_ZNK17storeservicescore19StoreErrorCondition9errorCodeEv";
inline constexpr const char* SEC_what =
    "_ZNK17storeservicescore19StoreErrorCondition4whatEv";
inline constexpr const char* RequestContext_storeFrontIdentifier =
    "_ZNK17storeservicescore14RequestContext20storeFrontIdentifierERKNSt6__ndk110shared_ptrINS_6URLBagEEE";
inline constexpr const char* HTTPMessage_ctor_c1 =
    "_ZN13mediaplatform11HTTPMessageC1ENSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES7_";
inline constexpr const char* HTTPMessage_setHeader =
    "_ZN13mediaplatform11HTTPMessage9setHeaderERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_";
inline constexpr const char* URLRequest_ctor =
    "_ZN17storeservicescore10URLRequestC2ERKNSt6__ndk110shared_ptrIN13mediaplatform11HTTPMessageEEERKNS2_INS_14RequestContextEEE";
inline constexpr const char* URLRequest_run =
    "_ZN17storeservicescore10URLRequest3runEv";
inline constexpr const char* URLRequest_error =
    "_ZNK17storeservicescore10URLRequest5errorEv";
inline constexpr const char* URLRequest_response =
    "_ZNK17storeservicescore10URLRequest8responseEv";
inline constexpr const char* URLRequest_setRequestParameter =
    "_ZN17storeservicescore10URLRequest19setRequestParameterERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_";
inline constexpr const char* URLResponse_underlyingResponse =
    "_ZNK17storeservicescore11URLResponse18underlyingResponseEv";
inline constexpr const char* URLResponse_protocolDictionary =
    "_ZN17storeservicescore11URLResponse18protocolDictionaryEv";
inline constexpr const char* URLRequest_setURLResponsePreprocessor =
    "_ZN17storeservicescore10URLRequest26setURLResponsePreprocessorERKNSt6__ndk18functionIFvRKNS1_10shared_ptrINS_11URLResponseEEEEEE";
inline constexpr const char* PurchaseRequest_ctor =
    "_ZN17storeservicescore15PurchaseRequestC2ERKNSt6__ndk110shared_ptrINS_14RequestContextEEE";
inline constexpr const char* PurchaseRequest_setBuyParameters =
    "_ZN17storeservicescore15PurchaseRequest16setBuyParametersERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* PurchaseRequest_setURLBagKey =
    "_ZN17storeservicescore15PurchaseRequest12setURLBagKeyERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* PurchaseRequest_setProcessDialogActions =
    "_ZN17storeservicescore15PurchaseRequest23setProcessDialogActionsEb";
inline constexpr const char* PurchaseRequest_run =
    "_ZN17storeservicescore15PurchaseRequest3runEv";
inline constexpr const char* PurchaseRequest_response =
    "_ZNK17storeservicescore15PurchaseRequest8responseEv";
inline constexpr const char* PurchaseResponse_items =
    "_ZNK17storeservicescore16PurchaseResponse5itemsEv";
inline constexpr const char* PurchaseResponse_error =
    "_ZN17storeservicescore16PurchaseResponse5errorEv";
inline constexpr const char* PurchaseItem_dictionary =
    "_ZNK17storeservicescore12PurchaseItem10dictionaryEv";
inline constexpr const char* SVPlaybackLeaseManager_ctor =
    "_ZN22SVPlaybackLeaseManagerC2ERKNSt6__ndk18functionIFvRKiEEERKNS1_IFvRKNS0_10shared_ptrIN17storeservicescore19StoreErrorConditionEEEEEE";
inline constexpr const char* SVPlaybackLeaseManager_refreshLeaseAutomatically =
    "_ZN22SVPlaybackLeaseManager25refreshLeaseAutomaticallyERKb";
inline constexpr const char* SVPlaybackLeaseManager_requestLease =
    "_ZN22SVPlaybackLeaseManager12requestLeaseERKb";
inline constexpr const char* SVFootHillSessionCtrl_instance =
    "_ZN21SVFootHillSessionCtrl8instanceEv";
inline constexpr const char* CFGetTypeID =
    "CFGetTypeID";
inline constexpr const char* CFStringGetTypeID =
    "CFStringGetTypeID";
inline constexpr const char* CFStringGetCString =
    "CFStringGetCString";
inline constexpr const char* CFStringGetLength =
    "CFStringGetLength";
inline constexpr const char* CFStringGetCStringPtr =
    "CFStringGetCStringPtr";
inline constexpr const char* CFStringGetMaximumSizeForEncoding =
    "CFStringGetMaximumSizeForEncoding";
inline constexpr const char* CFArrayGetTypeID =
    "CFArrayGetTypeID";
inline constexpr const char* CFArrayGetCount =
    "CFArrayGetCount";
inline constexpr const char* CFArrayGetValueAtIndex =
    "CFArrayGetValueAtIndex";
inline constexpr const char* CFNumberGetTypeID =
    "CFNumberGetTypeID";
inline constexpr const char* CFNumberIsFloatType =
    "CFNumberIsFloatType";
inline constexpr const char* CFNumberGetValue =
    "CFNumberGetValue";
inline constexpr const char* CFBooleanGetTypeID =
    "CFBooleanGetTypeID";
inline constexpr const char* CFBooleanGetValue =
    "CFBooleanGetValue";
inline constexpr const char* CFDataGetTypeID =
    "CFDataGetTypeID";
inline constexpr const char* CFDataGetBytePtr =
    "CFDataGetBytePtr";
inline constexpr const char* CFDataGetLength =
    "CFDataGetLength";
inline constexpr const char* CFDateGetTypeID =
    "CFDateGetTypeID";
inline constexpr const char* CFDateGetAbsoluteTime =
    "CFDateGetAbsoluteTime";
inline constexpr const char* CFDictionaryGetCount =
    "CFDictionaryGetCount";
inline constexpr const char* CFDictionaryGetKeysAndValues =
    "CFDictionaryGetKeysAndValues";
inline constexpr const char* CFNullGetTypeID =
    "CFNullGetTypeID";
inline constexpr const char* CFRetain =
    "CFRetain";
inline constexpr const char* CFRelease =
    "CFRelease";
// The dictionary type-id getter; _fn keeps it apart from the AuthSymbols
// field name only conceptually -- this is the dlsym name.
inline constexpr const char* CFDictionaryGetTypeID_fn =
    "CFDictionaryGetTypeID";
inline constexpr const char* CFDataGetBytes =
    "CFDataGetBytes";

}  // namespace auth36


// ---------------------------------------------------------------------------
// 3.6.0 protocol-dialog surface + the emplace vtables. The vtables are not
// exported symbols (stripped in the armv7 build): their addresses are pinned
// from the .data.rel.ro layout via the typeinfo-name scan (see account.cpp).
// ---------------------------------------------------------------------------
namespace auth36 {
inline constexpr const char* ProtocolDialog_title =
    "_ZNK17storeservicescore14ProtocolDialog5titleEv";
inline constexpr const char* ProtocolDialog_buttons =
    "_ZNK17storeservicescore14ProtocolDialog7buttonsEv";
inline constexpr const char* ProtocolButton_title =
    "_ZNK17storeservicescore14ProtocolButton5titleEv";
inline constexpr const char* ProtocolDialogResponse_ctor =
    "_ZN17storeservicescore22ProtocolDialogResponseC1Ev";
inline constexpr const char* ProtocolDialogResponse_setSelectedButton =
    "_ZN17storeservicescore22ProtocolDialogResponse17setSelectedButtonERKNSt6__ndk110shared_ptrINS_14ProtocolButtonEEE";
inline constexpr const char* Generic_setDialogHandler =
    "_ZN17storeservicescore28GenericPresentationInterface16setDialogHandlerENSt6__ndk18functionIFvRKNS1_10shared_ptrINS_14ProtocolDialogEEENS2_IFvRKNS3_INS_22ProtocolDialogResponseEEEEEEEEE";
// vtable object addresses inside libstoreservicescore (.data.rel.ro)
inline constexpr std::uintptr_t vtable_CredentialsResponse_addr = 0x1c09d0u;
inline constexpr std::uintptr_t vtable_ProtocolDialogResponse_addr = 0x1c0a38u;
}  // namespace auth36

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
