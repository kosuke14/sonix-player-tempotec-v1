#include "apple/guestelf.h"

#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <setjmp.h>

extern "C" void amp_fault_handler(int, siginfo_t*, void*);

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wrapper::apple {

namespace {

// The guest loads high: the daemon and its bionic live at the low end, and
// keeping the Apple set far above avoids stepping on anything the static
// image mapped by accident.
constexpr uintptr_t kLoadBase = 0x70000000u;
constexpr uintptr_t kLoadCeiling = 0xf0000000u;

struct Loaded {
    std::string soname;
    uintptr_t base;      // load bias: vaddr + base == virtual address
    uintptr_t vaddr_base; // the first PT_LOAD's p_vaddr (0 for these libs)
    Elf32_Sym *dynsym;   // .dynsym (absolute addresses already)
    uint32_t dynsym_count;
    const char *dynstr;  // .dynstr (absolute)
    void (*init)(void);  // DT_INIT, optional
    uint32_t *init_array;
    size_t init_array_count; // in entries
    bool init_ran;
};

std::unordered_map<std::string, Loaded *> g_loaded;
std::string g_search_dir;  // where DT_NEEDED names resolve first (set by guest_load)
std::string g_fallback_dir; // ...and where they fall back (the second generation)
uintptr_t g_next_base = kLoadBase;
std::vector<Loaded *> g_load_order;
// The ELF machine of the set being served (EM_386 or EM_ARM); the relocation
// type codes differ between the two.
int g_machine = EM_386;

// Global symbol scope: name -> address, filled as libraries load (insert
// order = load order, so the first definition wins, like the linker's
// breadth-first scope). A relocation pass walks tens of thousands of
// relocations; the linear dynsym scan it replaced made each lookup cost a
// full pass over every loaded library (minutes under qemu on the big libs).
std::unordered_map<std::string, void *> g_syms;

// --- the daemon's own symbols (static image) ------------------------------

struct SelfSymbols {
    Elf32_Sym *symtab = nullptr;   // into the file copy (static image: VA==file VA)
    uint32_t symtab_count = 0;
    const char *strtab = nullptr;  // into the file copy
};

SelfSymbols g_self;

void init_self_symbols(void);

// The static binary's .symtab/.strtab are file sections the loader never
// maps, so we re-open ourselves (AT_EXECFN, which qemu-user fakes to the
// guest path) and keep a private copy. The static image is non-PIE: symbol
// st_value is the virtual address directly.
void init_self_symbols(void) {
    if (g_self.symtab) {
        return;
    }
    char **e = environ;
    while (*e) {
        e++;
    }
    e++; // the auxv begins after the terminating NULL
    const char *execfn = NULL;
    for (;;) {
        Elf32_auxv_t *av = (Elf32_auxv_t *)e;
        if (av->a_type == AT_NULL) {
            break;
        }
        if (av->a_type == AT_EXECFN) {
            execfn = (const char *)av->a_un.a_val;
        }
        e += 2;
    }
    if (!execfn) {
        return;
    }
    FILE *f = fopen(execfn, "rb");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024 * 1024) {
        fclose(f);
        return;
    }
    uint8_t *file = (uint8_t *)malloc((size_t)size);
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) {
        free(file);
        fclose(f);
        return;
    }
    fclose(f);
    if (((uint32_t *)file)[0] != 0x464c457fu) {
        free(file);
        return;
    }
    Elf32_Ehdr *eh = (Elf32_Ehdr *)file;
    Elf32_Shdr *shdrs = (Elf32_Shdr *)(file + eh->e_shoff);
    if (eh->e_shoff == 0 || (long)(eh->e_shoff + eh->e_shnum * sizeof(Elf32_Shdr)) > size) {
        free(file);
        return; // stripped
    }
    Elf32_Shdr *shstr = &shdrs[eh->e_shstrndx];
    if (shstr->sh_offset + shstr->sh_size > (uint32_t)size) {
        free(file);
        return;
    }
    const char *names = (const char *)(file + shstr->sh_offset);
    for (int s = 0; s < eh->e_shnum; s++) {
        if (shdrs[s].sh_name >= shstr->sh_size) {
            continue;
        }
        const char *nm = names + shdrs[s].sh_name;
        if (strcmp(nm, ".symtab") == 0 &&
            shdrs[s].sh_offset + shdrs[s].sh_size <= (uint32_t)size) {
            g_self.symtab = (Elf32_Sym *)(file + shdrs[s].sh_offset);
            g_self.symtab_count = shdrs[s].sh_size / sizeof(Elf32_Sym);
        } else if (strcmp(nm, ".strtab") == 0 &&
                   shdrs[s].sh_offset + shdrs[s].sh_size <= (uint32_t)size) {
            g_self.strtab = (const char *)(file + shdrs[s].sh_offset);
        }
    }
    // Join the daemon's own definitions to the global scope (only functions
    // and data: st_value==0 entries are section placeholders).
    if (g_self.symtab && g_self.strtab) {
        for (uint32_t i = 1; i < g_self.symtab_count; i++) {
            Elf32_Sym *sym = &g_self.symtab[i];
            if (sym->st_value == 0 || sym->st_name == 0) {
                continue;
            }
            if (ELF32_ST_TYPE(sym->st_info) != STT_FUNC &&
                ELF32_ST_TYPE(sym->st_info) != STT_OBJECT) {
                continue;
            }
            g_syms.emplace(g_self.strtab + sym->st_name,
                           (void *)(uintptr_t)sym->st_value);
        }
        std::fprintf(stderr, "guestelf: self symbols indexed (%u)\n", g_self.symtab_count);
    }
}

void *self_resolve(const char *name) {
    init_self_symbols();
    if (!g_self.symtab || !g_self.strtab) {
        return nullptr;
    }
    for (uint32_t i = 0; i < g_self.symtab_count; i++) {
        if (g_self.symtab[i].st_name == 0 || g_self.symtab[i].st_value == 0) {
            continue;
        }
        if (strcmp(g_self.strtab + g_self.symtab[i].st_name, name) == 0) {
            // The static image is non-PIE: st_value is the virtual address.
            return (void *)(uintptr_t)g_self.symtab[i].st_value;
        }
    }
    return nullptr;
}

// --- loaded-set symbol resolution ------------------------------------------

void index_loaded(Loaded *lib) {
    if (!lib->dynsym || !lib->dynstr) {
        return;
    }
    for (uint32_t i = 1; i < lib->dynsym_count; i++) {
        Elf32_Sym *sym = &lib->dynsym[i];
        if (sym->st_value == 0 || sym->st_name == 0 ||
            sym->st_name >= 0x7fffffffu) {
            continue;
        }
        const char *name = lib->dynstr + sym->st_name;
        g_syms.emplace(name, (void *)(lib->base + sym->st_value));
    }
}

// The daemon's own symbols join the same scope once .symtab is parsed; a
// name seen in both resolves to the loaded Apple lib first (the map keeps
// the first insert), which matches the linker's search order.
void *global_resolve(const char *name) {
    auto it = g_syms.find(name);
    if (it != g_syms.end()) {
        return it->second;
    }
    return self_resolve(name);
}

// --- the loader proper ------------------------------------------------------

// Reads a whole .so and hands back its bytes. The files are tens of MB at
// most; mmap keeps the RSS at touched pages only.
uint8_t *read_whole(const char *path, size_t *out_len, char *error, size_t error_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(error, error_size, "open %s failed", path);
        return nullptr;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf32_Ehdr)) {
        close(fd);
        snprintf(error, error_size, "stat %s failed", path);
        return nullptr;
    }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        snprintf(error, error_size, "mmap %s failed", path);
        return nullptr;
    }
    *out_len = (size_t)st.st_size;
    return (uint8_t *)map;
}

Loaded *load_one(const char *path, char *error, size_t error_size);

// Applies one library's dynamic section: DT_NEEDED recursion happens before
// relocations, DT_INIT / DT_INIT_ARRAY are recorded for the post-pass.
bool apply_dynamic(Loaded *lib, uint8_t *file, size_t file_len, char *error,
                   size_t error_size) {
    Elf32_Ehdr *eh = (Elf32_Ehdr *)file;
    Elf32_Phdr *ph = (Elf32_Phdr *)(file + eh->e_phoff);
    Elf32_Dyn *dyn = nullptr;
    size_t dyn_count = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (Elf32_Dyn *)(file + ph[i].p_offset);
            dyn_count = ph[i].p_filesz / sizeof(Elf32_Dyn);
        }
    }
    if (!dyn) {
        snprintf(error, error_size, "%s: no PT_DYNAMIC", lib->soname.c_str());
        return false;
    }

    const char *strtab_file = nullptr;
    Elf32_Sym *symtab_file = nullptr;
    uint32_t symtab_count = 0;
    size_t strsz = 0;
    Elf32_Rel *rel = nullptr, *jmprel = nullptr;
    size_t rel_count = 0, jmprel_count = 0;
    uint32_t *init_array_file = nullptr;
    size_t init_array_count = 0;

    // Pass 1: the address-like entries (strtab, symtab, hash, relocs),
    // because DT_NEEDED names reference the strtab and may appear first.
    for (size_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
        Elf32_Dyn *d = &dyn[i];
        switch (d->d_tag) {
        case DT_STRTAB:
            strtab_file = (const char *)(file + (d->d_un.d_ptr - lib->vaddr_base));
            break;
        case DT_STRSZ:
            strsz = d->d_un.d_val;
            break;
        case DT_SYMTAB:
            symtab_file = (Elf32_Sym *)(file + (d->d_un.d_ptr - lib->vaddr_base));
            break;
        case DT_HASH: {
            // nchain is the second word; the symbol count follows from it.
            uint32_t *hash = (uint32_t *)(file + (d->d_un.d_ptr - lib->vaddr_base));
            symtab_count = hash[1];
            break;
        }
        case DT_GNU_HASH:
            // No DT_HASH in these libraries; derive the count lazily below.
            break;
        case DT_REL:
            rel = (Elf32_Rel *)(file + (d->d_un.d_ptr - lib->vaddr_base));
            break;
        case DT_RELSZ:
            rel_count = d->d_un.d_val / sizeof(Elf32_Rel);
            break;
        case DT_JMPREL:
            jmprel = (Elf32_Rel *)(file + (d->d_un.d_ptr - lib->vaddr_base));
            break;
        case DT_PLTRELSZ:
            jmprel_count = d->d_un.d_val / sizeof(Elf32_Rel);
            break;
        case DT_INIT:
            lib->init = (void (*)(void))(lib->base + d->d_un.d_ptr);
            break;
        case DT_INIT_ARRAY:
            init_array_file = (uint32_t *)(file + (d->d_un.d_ptr - lib->vaddr_base));
            break;
        case DT_INIT_ARRAYSZ:
            init_array_count = d->d_un.d_val / 4;
            break;
        default:
            break;
        }
    }
    // Pass 2: the string-referencing and init entries.
    for (size_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
        Elf32_Dyn *d = &dyn[i];
        switch (d->d_tag) {
        case DT_NEEDED: {
            if (!strtab_file) {
                snprintf(error, error_size, "%s: DT_STRTAB missing",
                         lib->soname.c_str());
                return false;
            }
            const char *needed = strtab_file + d->d_un.d_val;
            // The daemon is a static bionic binary: libc and friends resolve
            // against its own symbols -- mapping a second libc would fork
            // the allocator and stdio state. libc++.so is the Apple-shipped
            // libc++_shared.so under a different soname.
            static const char *kSkip[] = {"libc.so",       "libm.so",   "libdl.so",
                                          "liblog.so",     "libz.so",   "libstdc++.so",
                                          "libandroid.so", "libOpenSLES.so", NULL};
            bool skip = false;
            for (int i = 0; kSkip[i]; i++) {
                if (strcmp(needed, kSkip[i]) == 0) {
                    skip = true;
                    break;
                }
            }
            if (skip) {
                break;
            }
            const char *redirected = strcmp(needed, "libc++.so") == 0 ? "libc++_shared.so" : needed;
            // Deps resolve in the loading library's own directory first, so
            // the 3.6.0 (auth) and 6.5.3 (decrypt) generations -- which share
            // sonames -- never cross their sets.
            char dep_path[512];
            snprintf(dep_path, sizeof(dep_path), "%s/%s", g_search_dir.c_str(), redirected);
            if (access(dep_path, R_OK) != 0) {
                snprintf(dep_path, sizeof(dep_path), "%s/%s", g_fallback_dir.c_str(),
                         redirected);
            }
            if (!guest_is_loaded(dep_path) && !guest_is_loaded(redirected)) {
                if (!load_one(dep_path, error, error_size)) {
                    return false;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    (void)file_len;

    fprintf(stderr, "guestelf: %s strtab=%p symtab=%p count=%u rel=%p/%zu jmprel=%p/%zu\n",
            lib->soname.c_str(), (void*)strtab_file, (void*)symtab_file, symtab_count,
            (void*)rel, rel_count, (void*)jmprel, jmprel_count);
    fflush(stderr);
    if (symtab_file) {
        if (symtab_count == 0) {
            // GNU-hash-only libraries (no DT_HASH nchain): take the exact
            // count from the section headers -- the file is not stripped.
            // The naive strtab-minus-symtab heuristic over-counts: version
            // and hash sections sit between .dynsym and .dynstr.
            Elf32_Ehdr *eh2 = (Elf32_Ehdr *)file;
            if (eh2->e_shoff && eh2->e_shoff + (uint64_t)eh2->e_shnum * sizeof(Elf32_Shdr) <= file_len) {
                Elf32_Shdr *sh = (Elf32_Shdr *)(file + eh2->e_shoff);
                for (int s = 0; s < eh2->e_shnum; s++) {
                    if (sh[s].sh_type == SHT_DYNSYM &&
                        sh[s].sh_offset + sh[s].sh_size <= file_len) {
                        symtab_count = (uint32_t)(sh[s].sh_size / sizeof(Elf32_Sym));
                        break;
                    }
                }
            }
            if (symtab_count == 0 && (uintptr_t)strtab_file > (uintptr_t)symtab_file) {
                // Last resort: assume .dynstr directly follows .dynsym.
                symtab_count = (uint32_t)(((uintptr_t)strtab_file - (uintptr_t)symtab_file) /
                                          sizeof(Elf32_Sym));
            }
        }
        // Absolute addresses for the mapped library.
        lib->dynsym = (Elf32_Sym *)(lib->base + ((uintptr_t)symtab_file - (uintptr_t)file));
        lib->dynsym_count = symtab_count;
        lib->dynstr = (const char *)(lib->base + ((uintptr_t)strtab_file - (uintptr_t)file));
        // Index this library's definitions into the global scope before the
        // relocation pass consumes them.
        index_loaded(lib);
    }

    // Relocations: relative ones patch against the base; slots go through
    // the global namespace.
    static int g_trace = -1;
    if (g_trace < 0) {
        g_trace = getenv("AMP_GUEST_TRACE") ? 1 : 0;
    }
    auto apply = [&](Elf32_Rel *r, size_t n) {
        for (size_t i = 0; i < n; i++) {
            uint32_t type = ELF32_R_TYPE(r[i].r_info);
            uint32_t sym_idx = ELF32_R_SYM(r[i].r_info);
            void *where = (void *)(lib->base + r[i].r_offset);
            // Symbol references must stay inside .dynsym, and name offsets
            // inside .dynstr -- a corrupt index would otherwise turn into a
            // wild pointer read.
            auto sym_name = [&]() -> const char * {
                if (sym_idx >= lib->dynsym_count) {
                    return nullptr;
                }
                uint32_t noff = lib->dynsym[sym_idx].st_name;
                if (!strsz || noff >= strsz) {
                    return nullptr;
                }
                return lib->dynstr + noff;
            };
            // The two guest ABIs this loader serves: i386 and armv7. Both use
            // REL with implicit addends; only the type codes differ.
            const uint32_t rel_relative = (g_machine == EM_386) ? R_386_RELATIVE : R_ARM_RELATIVE;
            if (g_trace) {
                const char *nm = (type == rel_relative) ? "" :
                    (sym_name() ? sym_name() : "<sym-oob>");
                fprintf(stderr, "guestelf: %s rel[%zu/%zu] type=%u sym=%u %s where=%p\n",
                        lib->soname.c_str(), i, n, type, sym_idx, nm, where);
            }
            switch (type) {
            case R_386_RELATIVE:
            case R_ARM_RELATIVE:
                *(uint32_t *)where += lib->base;
                break;
            case R_386_JMP_SLOT:
            case R_386_GLOB_DAT:
            case R_ARM_JUMP_SLOT:
            case R_ARM_GLOB_DAT: {
                const char *name = sym_name();
                if (!name) {
                    snprintf(error, error_size, "%s: bad symbol index %u (type %u)",
                             lib->soname.c_str(), sym_idx, type);
                    return false;
                }
                void *addr = global_resolve(name);
                if (!addr) {
                    snprintf(error, error_size, "%s: unresolved %s", lib->soname.c_str(), name);
                    return false;
                }
                *(uint32_t *)where = (uint32_t)(uintptr_t)addr;
                break;
            }
            case R_386_32:
            case R_ARM_ABS32: {
                const char *name = sym_name();
                if (!name) {
                    snprintf(error, error_size, "%s: bad symbol index %u (type %u)",
                             lib->soname.c_str(), sym_idx, type);
                    return false;
                }
                void *addr = global_resolve(name);
                if (!addr) {
                    snprintf(error, error_size, "%s: unresolved %s", lib->soname.c_str(), name);
                    return false;
                }
                *(uint32_t *)where += (uint32_t)(uintptr_t)addr;
                break;
            }
            default:
                snprintf(error, error_size, "%s: relocation type %u unsupported",
                         lib->soname.c_str(), type);
                return false;
            }
        }
        return true;
    };

    if (rel && !apply(rel, rel_count)) {
        return false;
    }
    fprintf(stderr, "guestelf: %s rel done\n", lib->soname.c_str());
    fflush(stderr);
    if (jmprel && !apply(jmprel, jmprel_count)) {
        return false;
    }
    fprintf(stderr, "guestelf: %s jmprel done\n", lib->soname.c_str());
    fflush(stderr);

    if (init_array_file && init_array_count) {
        lib->init_array = (uint32_t *)(lib->base + ((uintptr_t)init_array_file - (uintptr_t)file));
        lib->init_array_count = init_array_count;
    }
    return true;
}

Loaded *load_one(const char *path, char *error, size_t error_size) {
    fprintf(stderr, "guestelf: load %s\n", path);
    fflush(stderr);
    size_t file_len = 0;
    uint8_t *file = read_whole(path, &file_len, error, error_size);
    if (!file) {
        return nullptr;
    }
    Elf32_Ehdr *eh = (Elf32_Ehdr *)file;
    if (memcmp(eh->e_ident, ELFMAG, 4) != 0 ||
        (eh->e_machine != EM_386 && eh->e_machine != EM_ARM)) {
        munmap(file, file_len);
        snprintf(error, error_size, "%s: not an i386/ARM ELF", path);
        return nullptr;
    }
    g_machine = (int)eh->e_machine;

    fprintf(stderr, "guestelf: mapped %s span=%zu\n", path, (size_t)eh->e_shoff);
    // Key the loaded set by full path: the 3.6.0 (auth) and 6.5.3 (decrypt)
    // generations share sonames, and both must stay resident.
    if (guest_is_loaded(path)) {
        munmap(file, file_len);
        return g_loaded[path];
    }

    // Reserve a contiguous region large enough for all PT_LOADs.
    size_t span = 0;
    Elf32_Phdr *ph = (Elf32_Phdr *)(file + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            uintptr_t end = ph[i].p_vaddr + ph[i].p_memsz;
            if (end > span) {
                span = end;
            }
        }
    }
    span = (span + 0xfff) & ~0xffful;
    if (g_next_base + span > kLoadCeiling) {
        munmap(file, file_len);
        snprintf(error, error_size, "guest address space exhausted");
        return nullptr;
    }
    void *region = mmap((void *)g_next_base, span, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (region == MAP_FAILED) {
        munmap(file, file_len);
        snprintf(error, error_size, "mmap %zu bytes for %s failed", span, path);
        return nullptr;
    }
    g_next_base = (uintptr_t)region + span;

    Loaded *lib = new Loaded();
    lib->soname = path; // the map key is the full path (generations share sonames)
    lib->base = (uintptr_t)region;
    lib->vaddr_base = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            lib->vaddr_base = ph[i].p_vaddr;
            break;
        }
    }
    g_loaded[path] = lib;
    g_load_order.push_back(lib);

    // Copy each PT_LOAD in. Protections are narrowed only after the
    // relocations are written: .rel.dyn targets read-only segments in
    // several of these libraries, so the span stays RWX (as mmap'd)
    // until the relocation pass is done.
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        memcpy((void *)(lib->base + ph[i].p_vaddr), file + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset((void *)(lib->base + ph[i].p_vaddr + ph[i].p_filesz), 0,
                   ph[i].p_memsz - ph[i].p_filesz);
        }
    }

    fprintf(stderr, "guestelf: relocating %s\n", lib->soname.c_str());
    if (!apply_dynamic(lib, file, file_len, error, error_size)) {
        munmap(file, file_len);
        return nullptr;
    }

    // Narrow the protections to what each segment actually needs.
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        int prot = PROT_READ;
        if (ph[i].p_flags & PF_W) {
            prot |= PROT_WRITE;
        }
        if (ph[i].p_flags & PF_X) {
            prot |= PROT_EXEC;
        }
        uintptr_t page_start = lib->base + (ph[i].p_vaddr & ~0xffful);
        size_t page_len = ((ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~0xffful) -
                          (ph[i].p_vaddr & ~0xffful);
        mprotect((void *)page_start, page_len, prot);
    }
    munmap(file, file_len);

    // Constructors run after every relocation of the whole set succeeded,
    // in load (dependency) order, once the caller is done loading.
    return lib;
}

// The auth generation's constructors touch resources the bare guest does
// not have (system properties, /proc, the Android runtime); a crashing init
// is skipped rather than taking the daemon down. Enabled per set via
// guest_set_tolerant_inits(); the decrypt generation keeps strict inits.
static bool g_tolerant_inits = false;
static sigjmp_buf g_init_jmp;
static volatile sig_atomic_t g_init_active = 0;

extern "C" void amp_init_fault_handler(int, siginfo_t *, void *) {
    if (g_init_active) {
        g_init_active = 0;
        siglongjmp(g_init_jmp, 1);
    }
    // Not ours: restore the default action and re-raise.
    std::signal(SIGSEGV, SIG_DFL);
    std::signal(SIGBUS, SIG_DFL);
    std::raise(SIGSEGV);
}

struct InitGuard {
    InitGuard() {
        if (!g_tolerant_inits) {
            return;
        }
        struct sigaction sa;
        sa.sa_sigaction = amp_init_fault_handler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
    }
    static bool begin() {
        if (!g_tolerant_inits) {
            return true;
        }
        if (sigsetjmp(g_init_jmp, 1) != 0) {
            return false; // faulted: skip this entry
        }
        g_init_active = 1;
        return true;
    }
    static void end() { g_init_active = 0; }
};

void run_inits(void) {
    InitGuard guard;
    (void)guard;
    for (Loaded *lib : g_load_order) {
        fprintf(stderr, "guestelf: init %s\n", lib->soname.c_str());
        fflush(stderr);
        if (lib->init && !lib->init_ran) {
            lib->init_ran = true;
            if (InitGuard::begin()) {
                lib->init();
            } else {
                fprintf(stderr, "guestelf: init %s faulted; skipped\n",
                        lib->soname.c_str());
                fflush(stderr);
            }
            InitGuard::end();
        }
        if (lib->init_array && !lib->init_ran) {
            lib->init_ran = true;
            for (size_t i = 0; i < lib->init_array_count; i++) {
                if (g_tolerant_inits) {
                    fprintf(stderr, "guestelf: %s init_array[%zu]\n", lib->soname.c_str(), i);
                    fflush(stderr);
                }
                if (InitGuard::begin()) {
                    ((void (*)(void))(lib->init_array[i]))();
                    InitGuard::end();
                } else {
                    fprintf(stderr, "guestelf: init_array[%zu] of %s faulted; skipped\n", i,
                            lib->soname.c_str());
                    fflush(stderr);
                }
            }
        }
    }
    if (g_tolerant_inits) {
        // Hand faults back to the daemon-wide handler.
        struct sigaction sa;
        sa.sa_sigaction = amp_fault_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
    }
}

}  // namespace

bool guest_load(const char *path, char *error, size_t error_size) {
    init_self_symbols();
    {
        // Remember the directory of the first load; DT_NEEDED names are
        // bare sonames and resolve against it, falling back to the second
        // generation's directory when the file is not here.
        const char *slash = strrchr(path, '/');
        if (slash) {
            g_search_dir.assign(path, (size_t)(slash - path));
        }
        if (g_fallback_dir.empty() && slash) {
            g_fallback_dir = g_search_dir;
        }
    }
    Loaded *lib = load_one(path, error, error_size);
    if (!lib) {
        return false;
    }
    run_inits();
    return true;
}

void guest_set_fallback_dir(const char *dir) {
    if (dir && dir[0]) {
        g_fallback_dir = dir;
    }
}

void guest_set_tolerant_inits(bool tolerant) {
    g_tolerant_inits = tolerant;
}

void *guest_dlsym(const char *name) {
    init_self_symbols();
    return global_resolve(name);
}

void *guest_dlsym_in(const char *soname, const char *name) {
    init_self_symbols();
    auto it = g_loaded.find(soname);
    if (it == g_loaded.end()) {
        return nullptr;
    }
    Loaded *lib = it->second;
    if (!lib->dynsym || !lib->dynstr) {
        return nullptr;
    }
    for (uint32_t i = 1; i < lib->dynsym_count; i++) {
        Elf32_Sym *sym = &lib->dynsym[i];
        if (sym->st_value == 0 || sym->st_name == 0 ||
            sym->st_name >= 0x7fffffffu) {
            continue;
        }
        if (strcmp(lib->dynstr + sym->st_name, name) == 0) {
            return (void *)(lib->base + sym->st_value);
        }
    }
    return nullptr;
}

bool guest_is_loaded(const char *soname) {
    return g_loaded.find(soname) != g_loaded.end();
}

// --- crash forensics --------------------------------------------------------

namespace {

// Resolves one address to "lib+off" (or "daemon+off" / "self"), writing into
// a caller-provided buffer with write()-only primitives.
struct Span {
    uintptr_t lo, hi;
    const char *name;
};

void fmt_hex(char *dst, size_t cap, uintptr_t v) {
    snprintf(dst, cap, "0x%08x", (unsigned)v);
}

}  // namespace

void guest_report_fault(void *fault_addr, const void *stack_words, int word_count) {
    char line[128];
    fmt_hex(line, sizeof(line), (uintptr_t)fault_addr);
    fprintf(stderr, "\n=== GUEST FAULT at %s ===\n", line);
    // Build the span list once (a handful of libs; fine under a handler that
    // is about to die anyway).
    Span spans[32];
    int n = 0;
    for (Loaded *lib : g_load_order) {
        if (n >= 32) break;
        // The exact mapped extent is not tracked; use the reserved span
        // between consecutive bases (the loader allocates contiguously).
        spans[n].lo = lib->base;
        spans[n].hi = lib->base + 0x1000000; // generous; overlap is harmless
        spans[n].name = lib->soname.c_str();
        n++;
    }
    const uint32_t *words = (const uint32_t *)stack_words;
    for (int i = 0; i < word_count; i++) {
        uintptr_t v = words[i];
        if (v < 0x1000) continue;
        bool hit = false;
        for (int s = 0; s < n; s++) {
            if (v >= spans[s].lo && v < spans[s].hi) {
                snprintf(line, sizeof(line), "  [%3d] 0x%08x  %s+0x%x\n", i, (unsigned)v,
                         spans[s].name, (unsigned)(v - spans[s].lo));
                fputs(line, stderr);
                hit = true;
                break;
            }
        }
        if (!hit && v >= 0x00400000 && v < 0x00600000) {
            snprintf(line, sizeof(line), "  [%3d] 0x%08x  daemon+0x%x\n", i, (unsigned)v,
                     (unsigned)(v - 0x00400000));
            fputs(line, stderr);
            hit = true;
        }
        (void)hit;
    }
    fflush(stderr);
}

}  // namespace wrapper::apple
