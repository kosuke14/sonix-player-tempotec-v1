// A tiny ELF32 loader for i386 Android shared objects.
//
// The V1 guest cannot use Android's dynamic linker: it reads
// /proc/self/maps, which under qemu-user shows the *host's* address space,
// and every linker we tried (API 21/23/25) dies on that. The daemon is
// therefore a statically-linked bionic binary that loads the Apple library
// set by hand:
//
//   - each PT_LOAD goes into a fresh anonymous mapping (the .so files are
//     position-independent, the base is ours to choose),
//   - R_386_RELATIVE relocations are patched against that base,
//   - R_386_JMP_SLOT / GLOB_DAT / 32 relocations are resolved through a
//     namespace that sees (a) every previously loaded library and (b) the
//     daemon's own symbols -- the static bionic it was built with,
//   - DT_INIT / DT_INIT_ARRAY run once per library, dependencies first.
//
// No TLS: the Apple decrypt set does not use it through this path (the
// loader refuses a library that needs TLS relocations rather than
// misbehaving silently).
//
// This is deliberately minimal -- an ELF32 loader for *known, fixed input*
// (one ABI, one vendor's libraries), not a general dlopen.

#pragma once

#include <stdbool.h>
#include <stddef.h>

namespace wrapper::apple {

// Loads a library (and transitively everything it DT_NEEDs) into the
// process. Returns false with a reason in `error`. Idempotent: an already
// loaded soname is a no-op.
bool guest_load(const char *path, char *error, size_t error_size);

// dlsym-equivalent over the loaded set. NULL when nothing exports `name`.
void *guest_dlsym(const char *name);

// Same, but restricted to one library (needed when several export the same
// C symbol, e.g. every JNI library's JNI_OnLoad).
void *guest_dlsym_in(const char *soname, const char *name);

// True when `name` was already loaded.
bool guest_is_loaded(const char *soname);

// Crash forensics: on a guest SIGSEGV, print the fault address and every
// stack word that lands inside a loaded library or the daemon image as
// `lib+offset` (EIP-style return addresses, `this` pointers, vtables).
// Runs from the signal handler -- write() only, no allocation.
void guest_report_fault(void *fault_addr, const void *stack_words, int word_count);

}  // namespace wrapper::apple
