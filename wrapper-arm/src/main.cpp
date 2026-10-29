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

#include "apple/loader.hpp"
#include "apple/runtime.hpp"
#include "ipc.hpp"

// liblog has no static build in the NDK, and the guest never has a logd to
// talk to anyway: the three entry points the Apple libs import become
// no-ops here.
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }
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
    // i386 mcontext: gregs[REG_ESP] is the stack pointer at fault time.
    const void* esp = (const void*)uc->uc_mcontext.gregs[REG_ESP];
    const void* eip = (const void*)uc->uc_mcontext.gregs[REG_EIP];
    wrapper::apple::guest_report_fault(info->si_addr, esp, 96);
    (void)eip;
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
    std::string libs_dir = "/system/lib";
    if (argc > 1) {
        libs_dir = argv[1];
    }

    fprintf(stderr, "boot: guest daemon starting\n");
    wrapper::apple::Loader loader;
    if (const char* env = std::getenv("WRAPPER_LIBS_DIR"); env && env[0]) {
        libs_dir = env;
    }
    fprintf(stderr, "boot: opening %s\n", libs_dir.c_str());
    if (!loader.open(libs_dir)) {
        // Stub mode: still serve OpHealth so the listener can report a
        // precise failure instead of seeing a silent death.
        std::fprintf(stderr, "wrapper-arm: libs not loaded (%s)\n",
                     loader.last_error().c_str());
    }

    fprintf(stderr, "boot: loader.ok=%d\n", loader.ok());
    wrapper::apple::Runtime rt;
    if (loader.ok()) {
        rt.initialize(loader);
    }
    fprintf(stderr, "boot: entering ipc loop\n");

    return wrapper::run_ipc_worker(rt, loader);
}
