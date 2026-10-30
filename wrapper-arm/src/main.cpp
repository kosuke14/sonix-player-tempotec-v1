// wrapper-arm: the FairPlay decrypt-only daemon for the V1 guest.
//
// Usage: ampdaemon <libs_dir>
//   Reads WV2I frames on stdin, answers on stdout. Spawned by the mipsel
//   listener (amplocal) inside a qemu-arm guest; all real network work
//   (the SPC -> CKC exchange, and the playback dispatch) belongs to the
//   host side.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "apple/account.hpp"
#include "apple/guestelf.h"
#include "apple/loader.hpp"
#include "apple/runtime.hpp"
#include "ipc.hpp"

// liblog has no static build in the NDK, and the guest never has a logd to
// talk to anyway: the three entry points the Apple libs import become
// no-ops here.
#include <cstdarg>
extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    if (std::getenv("AMP_APPLE_LOG")) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "apple[%d/%s]: %s\n", prio, tag ? tag : "", buf);
    }
    return 0;
}
extern "C" int __android_log_write(int, const char*, const char*) { return 0; }
extern "C" int __android_log_vprint(int, const char*, const char*, void*) { return 0; }

#include <csignal>
#include <ucontext.h>

#include "apple/guestelf.h"

// A guest fault inside the Apple set is the expected failure mode while the
// ABI bindings settle; turn the raw qemu "signal 11" into a report that
// names the faulting library and the likely return addresses. This makes
// every crash a stackful answer instead of a guessing game.
extern "C" void amp_fault_handler(int sig, siginfo_t* info, void* uctx_v) {
    ucontext_t* uc = (ucontext_t*)uctx_v;
    // Stack pointer at fault time, per guest ABI: i386 keeps the register
    // set in gregs[], armv7 names the registers arm_sp / arm_pc directly.
#if defined(__i386__)
    const void* esp = (const void*)uc->uc_mcontext.gregs[REG_ESP];
#elif defined(__arm__)
    const void* esp = (const void*)uc->uc_mcontext.arm_sp;
#else
    const void* esp = nullptr;
#endif
    wrapper::apple::guest_report_fault(info->si_addr, esp, 96);
    // Restore the default action and re-raise so qemu reports the signal.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

static void install_fault_handler() {
    struct sigaction sa;
    sa.sa_sigaction = amp_fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

int main(int argc, char** argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    install_fault_handler();
    wrapper::apple::guest_preinit(); // before any setenv (auxv walk)
    std::string libs_dir = "/system/lib";
    if (argc > 1) {
        libs_dir = argv[1];
    }

    const bool auth_mode = argc > 2 && std::strcmp(argv[2], "auth") == 0;
    fprintf(stderr, "boot: guest daemon starting (%s)\n",
            auth_mode ? "auth" : "decrypt");
    wrapper::apple::Loader loader;
    if (const char* env = std::getenv("WRAPPER_LIBS_DIR"); env && env[0]) {
        libs_dir = env;
    }
    fprintf(stderr, "boot: opening %s\n", libs_dir.c_str());

    if (auth_mode) {
        // Auth mode: the 3.6.0 generation only (see loader.cpp's MODE SPLIT
        // note). Sign-in, tokens, anisette; no decrypt session.
        if (loader.open_auth(libs_dir) && auth_bootstrap(loader, libs_dir)) {
            fprintf(stderr, "boot: auth runtime ready\n");
        } else {
            fprintf(stderr, "boot: auth runtime failed (%s)\n",
                    loader.auth_last_error().c_str());
        }
    } else if (!loader.open(libs_dir)) {
        // Stub mode: still serve OpHealth so the listener can report a
        // precise failure instead of seeing a silent death.
        std::fprintf(stderr, "wrapper-arm: libs not loaded (%s)\n",
                     loader.last_error().c_str());
    } else {
        wrapper::apple::Runtime rt;
        rt.initialize(loader);
        fprintf(stderr, "boot: entering ipc loop\n");
        return wrapper::run_ipc_worker(rt, loader);
    }

    fprintf(stderr, "boot: entering ipc loop (auth)\n");
    wrapper::apple::Runtime rt_stub;
    return wrapper::run_ipc_worker(rt_stub, loader);
}
