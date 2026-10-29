#include "apple/runtime.hpp"

#include <jni.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "apple/loader.hpp"

namespace wrapper::apple {

namespace {

// The FairPlay core refuses to run until the device identity is registered:
// Apple's own startup (P7/a in the dex) sets the Android ID through the ADI,
// FootHill and KD layers before any FairPlay call. Without it KDGenMovieId
// bails with -42792 ("Error generating a movieId").
//
// The entry points are libstoreapi's obfuscated exports, recovered from the
// JNI shims (Java_..._Companion_ADISetAndroidID -> Sph98paBcz, etc.); the
// loader resolves them into Symbols as optional fields.

// c6.d.a(): Settings.Secure.ANDROID_ID, padded/truncated to 16 chars with
// '0' (hi/s.W(android_id, 16, '0')) and taken as UTF-8 bytes.
std::string device_android_id() {
    const char* env = std::getenv("AMP_ANDROID_ID");
    std::string id = env ? env : "";
    if (id.size() < 16) {
        id.insert(id.begin(), 16 - id.size(), '0');
    }
    id.resize(16);
    return id;
}

// --- the fake JVM ----------------------------------------------------------
//
// In the real app the JNI libraries initialise themselves inside
// JNI_OnLoad(JavaVM*, void*) when System.loadLibrary runs; the ADI layer in
// particular refuses everything (-45075) until that happened. We call
// JNI_OnLoad with a minimal fake JavaVM: GetEnv hands out a fake JNIEnv whose
// table is zero except for the handful of functions the loaders call before
// RegisterNatives. A crash inside one of the NULL slots is itself progress:
// the fault handler prints the table offset, which names the next function
// to implement.

using c_void = void;

jint fake_GetVersion(void*) { return 0x00010006; }  // JNI_VERSION_1_6
void* fake_FindClass(void*, const char*) { return nullptr; }
jint fake_RegisterNatives(void*, void*, void*, jint) { return 0; }
jcharArray fake_NewByteArray(void*, jsize) { return nullptr; }
jbyte* fake_GetByteArrayElements(void*, void*, void*) { return nullptr; }
jsize fake_GetArrayLength(void*, void*) { return 0; }
void fake_ReleaseByteArrayElements(void*, void*, void*, jint) {}
jint fake_ThrowNew(void*, void*, const char*) { return 0; }
jint fake_EnsureLocalCapacity(void*, jint) { return 0; }
void* fake_ExceptionOccurred(void*) { return nullptr; }
void fake_ExceptionClear(void*) {}
jboolean fake_ExceptionCheck(void*) { return 0; }
jstring fake_NewStringUTF(void*, const char*) { return nullptr; }
const char* fake_GetStringUTFChars(void*, void*, void*) { return ""; }
void fake_ReleaseStringUTFChars(void*, void*, const char*) {}
jsize fake_GetStringUTFLength(void*, void*) { return 0; }
jint fake_GetJavaVM(void*, JavaVM**) { return -1; }
jint fake_PushLocalFrame(void*, jint) { return 0; }
void* fake_PopLocalFrame(void*, void*) { return nullptr; }
void* fake_NewGlobalRef(void*, void* obj) { return obj; }
void fake_DeleteGlobalRef(void*, void*) {}
void fake_DeleteLocalRef(void*, void*) {}
jboolean fake_IsSameObject(void*, void*, void*) { return 0; }
void* fake_NewLocalRef(void*, void* obj) { return obj; }
void* fake_GetObjectClass(void*, void*) { return nullptr; }
void* fake_GetMethodID(void*, void*, const char*, const char*) { return nullptr; }
void* fake_GetStaticMethodID(void*, void*, const char*, const char*) { return nullptr; }
void* fake_GetFieldID(void*, void*, const char*, const char*) { return nullptr; }
void* fake_GetStaticFieldID(void*, void*, const char*, const char*) { return nullptr; }

// JNI function table indices (jni.h, 32-bit: entry = 4*index).
constexpr int kFnGetVersion = 4;
constexpr int kFnFindClass = 6;
constexpr int kFnThrowNew = 14;
constexpr int kFnExceptionOccurred = 15;
constexpr int kFnExceptionClear = 17;
constexpr int kFnNewStringUTF = 167;
constexpr int kFnGetStringUTFLength = 168;
constexpr int kFnGetStringUTFChars = 169;
constexpr int kFnReleaseStringUTFChars = 170;
constexpr int kFnNewByteArray = 176;
constexpr int kFnGetByteArrayElements = 184;
constexpr int kFnReleaseByteArrayElements = 192;
constexpr int kFnGetArrayLength = 171;
constexpr int kFnRegisterNatives = 215;
constexpr int kFnEnsureLocalCapacity = 220;
constexpr int kFnExceptionCheck = 228;
constexpr int kFnPushLocalFrame = 19;
constexpr int kFnPopLocalFrame = 20;
constexpr int kFnNewGlobalRef = 21;
constexpr int kFnDeleteGlobalRef = 22;
constexpr int kFnDeleteLocalRef = 23;
constexpr int kFnIsSameObject = 24;
constexpr int kFnNewLocalRef = 25;
constexpr int kFnGetObjectClass = 31;
constexpr int kFnGetMethodID = 33;
constexpr int kFnGetStaticMethodID = 113;
constexpr int kFnGetFieldID = 94;
constexpr int kFnGetStaticFieldID = 145;
constexpr int kFnGetJavaVM = 219;

jint fake_GetEnv(JavaVM*, void** env, jint) {
    // A static table of ~230 zero slots with the known functions filled.
    static void* table[230];
    static bool init = false;
    if (!init) {
        init = true;
        table[kFnGetVersion] = (void*)fake_GetVersion;
        table[kFnFindClass] = (void*)fake_FindClass;
        table[kFnThrowNew] = (void*)fake_ThrowNew;
        table[kFnExceptionOccurred] = (void*)fake_ExceptionOccurred;
        table[kFnExceptionClear] = (void*)fake_ExceptionClear;
        table[kFnExceptionCheck] = (void*)fake_ExceptionCheck;
        table[kFnPushLocalFrame] = (void*)fake_PushLocalFrame;
        table[kFnPopLocalFrame] = (void*)fake_PopLocalFrame;
        table[kFnNewGlobalRef] = (void*)fake_NewGlobalRef;
        table[kFnDeleteGlobalRef] = (void*)fake_DeleteGlobalRef;
        table[kFnDeleteLocalRef] = (void*)fake_DeleteLocalRef;
        table[kFnIsSameObject] = (void*)fake_IsSameObject;
        table[kFnNewLocalRef] = (void*)fake_NewLocalRef;
        table[kFnGetObjectClass] = (void*)fake_GetObjectClass;
        table[kFnGetMethodID] = (void*)fake_GetMethodID;
        table[kFnGetStaticMethodID] = (void*)fake_GetStaticMethodID;
        table[kFnGetFieldID] = (void*)fake_GetFieldID;
        table[kFnGetStaticFieldID] = (void*)fake_GetStaticFieldID;
        table[kFnNewStringUTF] = (void*)fake_NewStringUTF;
        table[kFnGetStringUTFLength] = (void*)fake_GetStringUTFLength;
        table[kFnGetStringUTFChars] = (void*)fake_GetStringUTFChars;
        table[kFnReleaseStringUTFChars] = (void*)fake_ReleaseStringUTFChars;
        table[kFnNewByteArray] = (void*)fake_NewByteArray;
        table[kFnGetByteArrayElements] = (void*)fake_GetByteArrayElements;
        table[kFnReleaseByteArrayElements] = (void*)fake_ReleaseByteArrayElements;
        table[kFnGetArrayLength] = (void*)fake_GetArrayLength;
        table[kFnRegisterNatives] = (void*)fake_RegisterNatives;
        table[kFnEnsureLocalCapacity] = (void*)fake_EnsureLocalCapacity;
        table[kFnGetJavaVM] = (void*)fake_GetJavaVM;
    }
    static void* env_object = table; // JNIEnv* points at the function table
    *env = &env_object;
    return 0; // JNI_OK
}

// JNIInvokeInterface: 3 reserved, DestroyJavaVM, AttachCurrentThread,
// DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon.
jint fake_AttachCurrentThread(JavaVM*, void** env, void*) {
    return fake_GetEnv(nullptr, env, 0);
}

JavaVM* g_fake_vm = nullptr;

void init_fake_jvm() {
    static void* table[8];
    table[4] = (void*)fake_AttachCurrentThread;  // AttachCurrentThread
    table[6] = (void*)fake_GetEnv;               // GetEnv
    // The JavaVM* the libraries see points at a struct whose first member is
    // the function table pointer.
    static struct { void* functions; } fake_vm_struct = { table };
    // Expose it for jni_onload callers.
    g_fake_vm = (JavaVM*)&fake_vm_struct;
}

}  // namespace

bool Runtime::initialize(const Loader& loader) {
    if (initialized_) {
        return decrypt_ready();
    }
    if (!loader.ok()) {
        std::fprintf(stderr, "runtime: cannot initialise, libs not loaded\n");
        return false;
    }

    // The environment Apple's stack assumes (mirrors the wrapper-v2
    // launcher): local DNS resolution against the values the host passes
    // in, the Android data dir, and the app's own file layout.
    setenv("ANDROID_DNS_MODE", "local", 1);
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_ROOT", "/system", 1);
    const char* base = getenv("AMP_BASE_DIR");
    if (!base || !base[0]) {
        base = "/data/data/com.apple.android.music/files";
    }
    std::string mkdir_cmd = std::string("mkdir -p ") + base + " " + base + "/mpl_db";
    (void)!system(mkdir_cmd.c_str());

    // Apple's startup order (P7/a): provisioning path, ADI library load,
    // android id through ADI, FairPlay, then KD. Every step is
    // best-effort: a missing or failing step only limits the key path,
    // the session still answers.
    const Symbols& s = loader.sym();
    const std::string base_dir = base;
    const std::string android_id = device_android_id();
    std::fprintf(stderr, "runtime: android_id=%s base=%s\n", android_id.c_str(),
                 base_dir.c_str());

    auto step = [&](const char* what, void* fn, auto... args) {
        if (!fn) {
            std::fprintf(stderr, "runtime: %s skipped (not resolved)\n", what);
            return;
        }
        const int rc = reinterpret_cast<int (*)(decltype(args)...)>(fn)(args...);
        std::fprintf(stderr, "runtime: %s -> %d\n", what, rc);
    };

    // JNI_OnLoad first: in the real app these run at System.loadLibrary and
    // initialise the obfuscated layers (the ADI one refuses everything with
    // -45075 until its own has run).
    init_fake_jvm();
    step("JNI_OnLoad(AMSKit)", (void*)s.JNI_OnLoad_amskit, (void*)g_fake_vm, (void*)nullptr);
    step("JNI_OnLoad(CoreADI)", (void*)s.JNI_OnLoad_coreadi, (void*)g_fake_vm, (void*)nullptr);
    step("JNI_OnLoad(CoreLSKD)", (void*)s.JNI_OnLoad_corelskd, (void*)g_fake_vm, (void*)nullptr);
    step("JNI_OnLoad(androidappmusic)", (void*)s.JNI_OnLoad_androidappmusic, (void*)g_fake_vm,
         (void*)nullptr);
    step("JNI_OnLoad(storeapi)", (void*)s.JNI_OnLoad_storeapi, (void*)g_fake_vm, (void*)nullptr);

    step("ADILoadLibraryWithPath", (void*)s.ADIInitLibrary, base_dir.c_str());
    step("ADISetProvisioningPath", (void*)s.ADISetProvisioningPath, base_dir.c_str());
    step("ADISetAndroidID", (void*)s.ADISetAndroidID, android_id.c_str(), 16);
    step("FairPlaySetAndroidID", (void*)s.FairPlaySetAndroidID, android_id.c_str(), 16);
    step("FairPlayLoadLibraryWithPath", (void*)s.FairPlayInitLibrary, base_dir.c_str());
    step("KDSetAndroidID", (void*)s.KDSetAndroidID, android_id.c_str(), 16);
    // The gate probe: 0 => not provisioned, 1 => provisioned, negative => the
    // ADI layer itself is refusing (storage/library state broken).
    step("ADIIsMachineProvisioned", (void*)s.ADIIsMachineProvisioned, 0L);

    foothill_ = s.SVFootHillSessionCtrl_instance();
    if (!foothill_) {
        std::fprintf(stderr, "runtime: SVFootHillSessionCtrl::instance() returned null\n");
        return false;
    }
    initialized_ = true;
    std::fprintf(stderr, "runtime: FooHill session ready (%p)\n", foothill_);
    return true;
}

}  // namespace wrapper::apple
