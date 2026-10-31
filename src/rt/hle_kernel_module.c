// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Modules: loading, starting and stopping modules, LoadExec and the exit callback. Extracted
 * from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* Register or replace an export discovered while loading a PRX. The target is a guest address,
 * never a host function pointer. This makes module loading repeatable and keeps host ASLR out of
 * guest-visible state. Returns 1 on success and 0 for an invalid/full registration. */
int sr_hle_register_late_import(uint32_t nid, uint32_t target) {
    if (!nid || !target || target == SR_HLE_LATE_BUILTIN) return 0;
    hle_lock();
    unsigned lo = 0, hi = s_late_import_n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (s_late_imports[mid].nid < nid) lo = mid + 1;
        else hi = mid;
    }
    if (lo < s_late_import_n && s_late_imports[lo].nid == nid) {
            s_late_imports[lo].target = target;
            hle_unlock();
            return 1;
    }
    if (s_late_import_n == s_late_import_cap) {
        unsigned cap = s_late_import_cap ? s_late_import_cap * 2u : 256u;
        LateImportEntry *grown = cap > s_late_import_cap
            ? (LateImportEntry *)realloc(s_late_imports, (size_t)cap * sizeof(*grown))
            : NULL;
        if (!grown) { hle_unlock(); return 0; }
        s_late_imports = grown;
        s_late_import_cap = cap;
    }
    memmove(&s_late_imports[lo + 1], &s_late_imports[lo],
            (s_late_import_n - lo) * sizeof(s_late_imports[0]));
    s_late_imports[lo] = (LateImportEntry){nid, target};
    s_late_import_n++;
    hle_unlock();
    return 1;
}
/* The extra PRXs are compiled into the native dispatch table, but their data segments are not
 * part of hst_image.bin. Read the PSP ELF module/export headers at load time and publish the
 * relocated guest entry points. PSP resident-library entries contain one combined table:
 * function NIDs, variable NIDs, function addresses, variable addresses. */
typedef struct { uint32_t type, off, va, pa, filesz, memsz, flags, align; } SrElfPhdr;

static uint32_t elf_vaddr_to_file(const SrElfPhdr *ph, unsigned n, uint32_t va, size_t file_len) {
    for (unsigned i = 0; i < n; i++) {
        if (ph[i].type != 1 || va < ph[i].va || va - ph[i].va >= ph[i].filesz) continue;
        uint64_t off = (uint64_t)ph[i].off + (va - ph[i].va);
        if (off <= UINT32_MAX && off <= file_len && file_len - (size_t)off >= 1u)
            return (uint32_t)off;
    }
    return UINT32_MAX;
}

/* Module-info pointers in a relocatable PRX can already be rebased by the loader
 * contract, while the export walker also accepts the base-relative form used by
 * older public fixtures. Keep both representations anchored to the ELF segments. */
static uint32_t prx_vaddr_to_file(const SrElfPhdr *ph, unsigned n, uint32_t va,
                                 uint32_t base, size_t file_len) {
    uint32_t off = elf_vaddr_to_file(ph, n, va, file_len);
    if (off == UINT32_MAX && base != 0u && va >= base)
        off = elf_vaddr_to_file(ph, n, va - base, file_len);
    return off;
}

/* A 32-bit byte offset must be representable as this host's long for fseek.
 * The limit travels as a parameter rather than being written inline so that a
 * host whose long is wider than 32 bits (LP64) still takes the comparison
 * instead of a tautology the compiler rejects. On Windows, where long is
 * 32-bit, this is exactly `(uint64_t)off <= (uint64_t)LONG_MAX`. */
static int sr_off_fits_long(uint32_t off, long limit) {
    return (uint64_t)off <= (uint64_t)limit;
}

static int valid_module_info(const uint8_t *mi, const SrElfPhdr *ph, unsigned phnum,
                             uint32_t base, size_t file_len) {
    uint32_t gp, ent_top, ent_end, stub_top, stub_end;
    uint16_t attributes; uint8_t major, minor;
    memcpy(&attributes, mi, 2); major = mi[2]; minor = mi[3];
    if (major > 10 || minor > 10) return 0;

    if (!mi[4]) return 0;
    char first = (char)mi[4];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_')) return 0;
    int has_null = 0;
    for (unsigned i = 4; i < 32; i++) {
        if (mi[i] == 0) { has_null = 1; break; }
        char ch = (char)mi[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            return 0;
    }
    if (!has_null) return 0;

    memcpy(&gp, mi + 32, 4);
    memcpy(&ent_top, mi + 36, 4);
    memcpy(&ent_end, mi + 40, 4);
    memcpy(&stub_top, mi + 44, 4);
    memcpy(&stub_end, mi + 48, 4);

    if (ent_top % 4 != 0 || ent_end % 4 != 0 || stub_top % 4 != 0 || stub_end % 4 != 0) return 0;
    if (ent_top == 0 || ent_end <= ent_top || ent_end - ent_top > 0x10000u) return 0;
    if (stub_end < stub_top || stub_end - stub_top > 0x10000u) return 0;

    return prx_vaddr_to_file(ph, phnum, ent_top, base, file_len) != UINT32_MAX;
}

/* Most stripped PRXs put the PspModuleInfo file offset in the executable PT_LOAD p_paddr.
 * A few Sony modules instead use that field for another resident-data address. Fall back to
 * the same conservative aligned scan used by the codegen analyzer, rather than interpreting
 * an export/NID table as module-info and silently publishing nothing. */
static uint32_t find_module_info(FILE *f, const SrElfPhdr *ph, unsigned phnum,
                                 uint32_t base, size_t file_len, uint8_t mi[52]) {
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < phnum; i++) {
            if (ph[i].type != 1 || !(ph[i].flags & 1) || !ph[i].filesz) continue;
            uint64_t begin64 = pass == 0 ? ph[i].pa : ph[i].off;
            uint64_t end64 = pass == 0 ? begin64 + 1u : (uint64_t)ph[i].off + ph[i].filesz;
            if (begin64 > UINT32_MAX || end64 > file_len || end64 < begin64) continue;
            uint32_t begin = (uint32_t)begin64;
            uint32_t end = (uint32_t)end64;
            if (pass == 0 && !begin) continue;
            for (uint32_t off = begin; off <= end && end - off >= 52u; ) {
                if (!sr_off_fits_long(off, LONG_MAX) || fseek(f, (long)off, SEEK_SET) || fread(mi, 1, 52, f) != 52) break;
                if (valid_module_info(mi, ph, phnum, base, file_len)) return off;
                if (UINT32_MAX - off < 4u) break;
                off += 4u;
            }
        }
    }
    return UINT32_MAX;
}

/* TD-26 runtime gate: real module_start / module_stop execution. */
static int real_module_start_enabled(void) {
    const char *e = getenv("SR_REAL_MODULE_START");
    return (e != NULL && strcmp(e, "1") == 0);
}

/* The same variable is a tri-state gate for sceKernelStartModule, because "0" is an
 * environmental kill switch an operator sets to keep the Sony init path off the host
 * and therefore has to cover every module, libfont included:
 *   "1"    - translated/real module_start may run for any module.
 *   unset  - translated libfont module_start runs, so the guest owns its own
 *             readiness, and so does a guest-placed module's unless host HLE serves
 *             its exports; every other module keeps the host bypass in h_StartModule.
 *   "0"    - no module_start runs for any module; libfont stops the run by name
 *             instead of synthesizing guest readiness.
 * Any other value is refused rather than guessed, and the refusal is named once. */
typedef enum {
    MODULE_START_ALLOW_ALL,
    MODULE_START_LIBFONT_ONLY,
    MODULE_START_NONE
} ModuleStartPolicy;

static ModuleStartPolicy module_start_policy(void) {
    const char *e = getenv("SR_REAL_MODULE_START");
    if (e == NULL || e[0] == '\0') return MODULE_START_LIBFONT_ONLY;
    if (strcmp(e, "1") == 0) return MODULE_START_ALLOW_ALL;
    if (strcmp(e, "0") == 0) return MODULE_START_NONE;
    static int s_logged_bad_value = 0;
    if (!s_logged_bad_value) {
        s_logged_bad_value = 1;
        fprintf(stderr, "SR_REAL_MODULE_START: unsupported value \"%s\"; using the default "
                        "(translated libfont and guest-placed module startup)\n", e);
    }
    return MODULE_START_LIBFONT_ONLY;
}

/* A module-name test, not a substring test: the match has to be a whole path
 * element, so a path such as mylibfont.prx.bak cannot take a module's behavioural
 * path. */
static int module_path_is(const char *path, const char *name) {
    size_t span;
    const char *hit;
    if (!path || !name) return 0;
    span = strlen(name);
    for (hit = strstr(path, name); hit; hit = strstr(hit + 1, name)) {
        if ((hit == path || hit[-1] == '/' || hit[-1] == '\\') &&
            (hit[span] == '\0' || hit[span] == '/' || hit[span] == '\\')) return 1;
    }
    return 0;
}

/* Text/data/bss sizes of a PRX image as the kernel's loader sums them (uOFW
 * src/kd/loadcore/loadelf.c): with a section table, CheckElfSectionPRX -- PROGBITS sections
 * flagged exactly ALLOC|EXECINSTR or ALLOC are text, WRITE|ALLOC data, NOBITS WRITE|ALLOC
 * bss; without one, CheckElfSection -- PT_LOAD file bytes are text when executable and data
 * otherwise, memory past the file bytes bss. Returns 0 when the headers are malformed. */
static int prx_text_data_bss(const unsigned char *elf, size_t len, uint32_t *text,
                             uint32_t *data, uint32_t *bss) {
    uint64_t t = 0, d = 0, b = 0;
    if (!elf || len < 52u || memcmp(elf, "\177ELF\1\1", 6) != 0) return 0;
    uint32_t phoff, shoff; uint16_t phentsize, phnum, shentsize, shnum, shstrndx;
    memcpy(&phoff, elf + 28, 4); memcpy(&shoff, elf + 32, 4);
    memcpy(&phentsize, elf + 42, 2); memcpy(&phnum, elf + 44, 2);
    memcpy(&shentsize, elf + 46, 2); memcpy(&shnum, elf + 48, 2); memcpy(&shstrndx, elf + 50, 2);
    if (shoff > 0u) {
        if (shnum == 0u || shstrndx >= shnum || shentsize < 40u ||
            (uint64_t)shoff + (uint64_t)shentsize * shnum > len)
            return 0;
        for (uint32_t i = 0; i < shnum; i++) {
            const unsigned char *sh = elf + shoff + (size_t)i * shentsize;
            uint32_t type, flags, size;
            memcpy(&type, sh + 4, 4); memcpy(&flags, sh + 8, 4); memcpy(&size, sh + 20, 4);
            if (type == 1u) {                                   /* SHT_PROGBITS */
                if (flags == 6u || flags == 2u) t += size;      /* ALLOC|EXECINSTR, ALLOC */
                else if (flags == 3u) d += size;                /* WRITE|ALLOC */
            } else if (type == 8u && flags == 3u) {             /* SHT_NOBITS, WRITE|ALLOC */
                b += size;
            }
        }
    } else {
        if (phentsize < 32u || (uint64_t)phoff + (uint64_t)phentsize * phnum > len) return 0;
        for (uint32_t i = 0; i < phnum; i++) {
            const unsigned char *ph = elf + phoff + (size_t)i * phentsize;
            uint32_t type, filesz, memsz, flags;
            memcpy(&type, ph, 4); memcpy(&filesz, ph + 16, 4); memcpy(&memsz, ph + 20, 4);
            memcpy(&flags, ph + 24, 4);
            if (type != 1u) continue;
            if (flags & 1u) t += filesz; else d += filesz;
            if (filesz < memsz) b += memsz - filesz;
        }
    }
    if (t > UINT32_MAX || d > UINT32_MAX || b > UINT32_MAX) return 0;
    *text = (uint32_t)t; *data = (uint32_t)d; *bss = (uint32_t)b;
    return 1;
}
static int hle_loadexec_dialog_busy(void);

#define SR_LOAD_EXEC_MAX_ARGS (64u * 1024u)
SrLoadExecRequest s_loadexec_request;
int s_loadexec_pending;

/* Run a module entry (module_start/module_stop) as a nested guest call with the entry
 * ABI (args length, args pointer). A guest-placed module's entries run with its own
 * $gp; the fixed-address path keeps the caller's, as it always has. */
static uint32_t module_entry_call(CpuState *s, const LoadedModule *mod, uint32_t entry,
                                  uint32_t arglen, uint32_t argp) {
    if (mod->runtime_placed && mod->code)
        return ge_call_guest_gp_rv(s, entry, mod->gp, arglen, argp, 0);
    return ge_call_guest_rv(s, entry, arglen, argp, 0);
}

/* module_stop is a module-manager entry, so execute it in its own guest thread.
 * Thread creation uses the runtime's existing synthetic defaults (priority 32,
 * default stack); callers that need a physical PSP's module-thread option rules
 * remain behind this explicitly labelled boundary. The scheduler copies argp to
 * the stop thread's stack before starting it, then this caller joins and deletes
 * the temporary thread. */
static uint32_t module_stop_thread_invoke(CpuState *caller, const LoadedModule *mod,
                                          uint32_t arglen, uint32_t argp,
                                          uint32_t *status_out) {
    if (!caller || !mod || !mod->module_stop || sched_current_uid() == 0u) {
        fprintf(stderr, "PRX_MODULE_STOP_THREAD_UNAVAILABLE_SYNTHETIC: no running guest "
                        "thread or stop entry; refusing inline execution\n");
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    if (arglen != 0u && (!argp || !sr_guest_span_readable(argp, arglen)))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    uint32_t thread_uid = sched_create_thread_ex(mod->module_stop, 32, 0u, 0u,
                                                 "module_stop");
    if (!thread_uid) return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */
    uint32_t gp = mod->runtime_placed ? mod->gp : caller->r[28];
    if (sched_thread_set_gp(thread_uid, gp) != 0) {
        (void)sched_delete_thread(thread_uid);
        fprintf(stderr, "PRX_MODULE_STOP_THREAD_GP_SYNTHETIC: failed to seed $gp; refusing\n");
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    uint32_t rc = sched_start_thread(thread_uid, arglen, argp);
    if (rc != 0u) {
        (void)sched_delete_thread(thread_uid);
        return rc;
    }

    caller->r[4] = thread_uid;
    caller->r[5] = 0u;
    uint32_t module_status = h_WaitThreadEnd_impl(caller, 0);
    uint32_t delete_rc = sched_delete_thread(thread_uid);
    if (delete_rc != 0u) {
        fprintf(stderr, "PRX_MODULE_STOP_THREAD_JOIN_SYNTHETIC: stop thread uid=0x%x "
                        "did not become deletable (0x%08x); refusing module transition\n",
                thread_uid, delete_rc);
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    if (status_out) *status_out = module_status;
    return 0u;
}

/* Claim a module record, reusing a slot a previous unload released before growing the
 * table. Returns NULL only when all slots hold live modules. */
LoadedModule *alloc_loaded_module_slot(void) {
    for (int i = 0; i < s_nloaded_modules; i++) {
        if (!s_loaded_modules[i].in_use) {
            /* A released record still holds the dead module's uid, path and entry
             * addresses. Clear it and claim it here, so the caller cannot install a
             * module that find_loaded_module() and retire_loaded_module() both refuse
             * to see because in_use was never set. */
            LoadedModule *reused = &s_loaded_modules[i];
            memset(reused, 0, sizeof(*reused));
            reused->in_use = 1;
            return reused;
        }
    }
    if (s_nloaded_modules >= (int)(sizeof(s_loaded_modules) / sizeof(s_loaded_modules[0])))
        return NULL;
    LoadedModule *mod = &s_loaded_modules[s_nloaded_modules++];
    memset(mod, 0, sizeof(*mod));
    mod->in_use = 1;
    return mod;
}

uint32_t s_last_prx_entry = 0;
uint32_t s_last_prx_stop = 0;
uint32_t s_last_prx_base = 0;
/* The last module register_prx_exports read exports a function host HLE implements: its
 * identity is the export table, never its path (#1385). */
static int s_last_prx_host_served = 0;

unsigned register_prx_exports(const char *host_path, uint32_t base) {
    s_last_prx_host_served = 0;
    FILE *f = fopen(host_path, "rb");
    if (!f) {
        char temp_path[512];
        snprintf(temp_path, sizeof(temp_path), "../../%s", host_path);
        f = fopen(temp_path, "rb");
        if (!f) {
            snprintf(temp_path, sizeof(temp_path), "../%s", host_path);
            f = fopen(temp_path, "rb");
        }
        if (f) {
            fprintf(stderr, "register_prx_exports: resolved %s to %s\n", host_path, temp_path);
        }
    }
    uint8_t eh[52], mi[52];
    SrElfPhdr ph[16];
    unsigned registered = 0;
    size_t file_len = 0;
    if (!f) {
        fprintf(stderr, "register_prx_exports: FAILED to open %s (errno=%d)\n", host_path, errno);
        goto out;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "register_prx_exports: FAILED to seek %s\n", host_path);
        goto out;
    }
    long file_size = ftell(f);
    if (file_size < 0 || (uint64_t)file_size > SIZE_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "register_prx_exports: FAILED to size %s\n", host_path);
        goto out;
    }
    file_len = (size_t)file_size;
    if (file_len < sizeof(eh)) {
        fprintf(stderr, "register_prx_exports: truncated ELF header in %s\n", host_path);
        goto out;
    }
    if (fread(eh, 1, sizeof(eh), f) != sizeof(eh)) {
        fprintf(stderr, "register_prx_exports: FAILED to read ELF header from %s\n", host_path);
        goto out;
    }
    if (memcmp(eh, "\177ELF\1\1", 6)) {
        fprintf(stderr, "register_prx_exports: INVALID ELF magic in %s\n", host_path);
        goto out;
    }
    uint32_t phoff; uint16_t phentsz, phnum;
    memcpy(&phoff, eh + 28, 4); memcpy(&phentsz, eh + 42, 2); memcpy(&phnum, eh + 44, 2);
    uint32_t e_entry = 0;
    memcpy(&e_entry, eh + 24, 4);
    if (e_entry != 0 && e_entry <= UINT32_MAX - base) {
        s_last_prx_entry = base + e_entry;
    }
    uint64_t ph_bytes = (uint64_t)phentsz * phnum;
    if (phentsz != 32 || !phnum || phnum > 16 || ph_bytes > file_len || phoff > file_len - (size_t)ph_bytes ||
        phoff > (uint32_t)LONG_MAX || fseek(f, (long)phoff, SEEK_SET)) {
        fprintf(stderr, "register_prx_exports: INVALID phdr params in %s (phentsz=%u, phnum=%u, phoff=%u)\n",
                host_path, phentsz, phnum, phoff);
        goto out;
    }
    if (fread(ph, sizeof(ph[0]), phnum, f) != phnum) {
        fprintf(stderr, "register_prx_exports: FAILED to read phdrs from %s\n", host_path);
        goto out;
    }

    uint32_t mioff = find_module_info(f, ph, phnum, base, file_len, mi);
    if (mioff == UINT32_MAX) {
        fprintf(stderr, "register_prx_exports: FAILED to find module info in %s\n", host_path);
        goto out;
    }
    uint32_t ent_top, ent_end;
    memcpy(&ent_top, mi + 36, 4); memcpy(&ent_end, mi + 40, 4);
    if (ent_end < ent_top || ent_end - ent_top > 0x10000u) {
        fprintf(stderr, "register_prx_exports: INVALID export range in %s (0x%x - 0x%x)\n",
                host_path, ent_top, ent_end);
        goto out;
    }

    for (uint32_t ent = ent_top; ent <= ent_end && ent_end - ent >= 16u;) {
        uint8_t e[16];
        uint32_t off = prx_vaddr_to_file(ph, phnum, ent, base, file_len);
        if (off == UINT32_MAX || (uint64_t)off + sizeof(e) > file_len || off > (uint32_t)LONG_MAX ||
            fseek(f, (long)off, SEEK_SET) || fread(e, 1, sizeof(e), f) != sizeof(e)) break;
        unsigned words = e[8], nvars = e[9]; uint16_t nfuncs; uint32_t table;
        memcpy(&nfuncs, e + 10, 2); memcpy(&table, e + 12, 4);
        unsigned count = (unsigned)nfuncs + nvars;
        if (words < 4 || words > 0x40 || count > 1024) break;
        uint32_t toff = prx_vaddr_to_file(ph, phnum, table, base, file_len);
        if (toff != UINT32_MAX && count) {
            uint64_t pair_bytes = (uint64_t)count * 2u * sizeof(uint32_t);
            if (pair_bytes > SIZE_MAX || pair_bytes > file_len || (uint64_t)toff + pair_bytes > file_len || toff > (uint32_t)LONG_MAX)
                break;
            uint32_t *pairs = (uint32_t *)malloc((size_t)pair_bytes);
            if (!pairs) break;
            if (!fseek(f, (long)toff, SEEK_SET) && fread(pairs, sizeof(uint32_t), count * 2u, f) == count * 2u) {
                for (unsigned i = 0; i < nfuncs; i++) {
                    uint32_t target = pairs[count + i];
                    /* Export targets are PRX-relative virtual addresses.  Zero is a
                     * valid relative entry point when the module is loaded at a
                     * nonzero base; only reject the final absolute-address wrap. */
                    if (target <= UINT32_MAX - base &&
                        sr_hle_register_late_import(pairs[i], base + target)) registered++;
                    if (hle_find(pairs[i])) s_last_prx_host_served = 1;
                    if (target <= UINT32_MAX - base) {
                        if (pairs[i] == 0xd632acdbu) {
                            s_last_prx_entry = base + target;
                        } else if (pairs[i] == 0xcee05613u) {
                            s_last_prx_stop = base + target;
                        }
                    }
                }
            }
            free(pairs);
        }
        uint32_t advance = (uint32_t)words * 4u;
        if (advance == 0 || UINT32_MAX - ent < advance) break;
        ent += advance;
    }
out:
    if (f) fclose(f);
    if (registered) fprintf(stderr, "HLE: registered %u late imports from %s at 0x%08x\n",
                            registered, host_path, base);
    return registered;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Keep the synthetic PRX fixture on the same parser and publication path as production.
 * This symbol is compiled only into hle-thread-selftest; normal runtime builds cannot depend
 * on test-only loader entry points. */
unsigned sr_hle_test_register_prx_exports(const char *host_path, uint32_t base) {
    return register_prx_exports(host_path, base);
}
int sr_hle_test_last_prx_host_served(void) { return s_last_prx_host_served; }
#endif

static unsigned register_known_module(const char *module_root,
                                      const char *module_name,
                                      uint32_t base) {
    if (!module_root || !module_root[0] || !module_name || !module_name[0]) return 0;
    char host_path[1024];
    int written = snprintf(host_path, sizeof(host_path), "%s/%s",
                           module_root, module_name);
    if (written < 0 || (size_t)written >= sizeof(host_path)) return 0;
    return register_prx_exports(host_path, base);
}

static const char *guest_module_root(void) {
    const char *module_root = getenv("SR_MODULE_DIR");
    return (module_root && module_root[0]) ? module_root : "place_game_here/EXTRACTED/decrypted";
}

/* ---- Late-module image load (L4) ----
 * The recompiler translates a late module's code ahead of time at its manifest base,
 * but the module's initialised data (.data/.rodata, relocated pointer tables, strings)
 * only exists in guest RAM if the loader puts it there. The clean-room loader
 * (prx_loader.c) relocates the image; its only guest access is sr_prx_guest_write,
 * which this runtime implements as a host staging buffer so nothing touches guest RAM
 * until the module's exact range [base, end) has been reserved in the user partition. */
static uint8_t *s_prx_stage;
uint32_t s_prx_stage_base, s_prx_stage_len, s_prx_stage_cap;
int s_prx_staging;

int sr_prx_guest_write(uint32_t guest_addr, const void *src, uint32_t n) {
    if (!s_prx_staging || guest_addr < s_prx_stage_base || (n && !src)) return 1;
    uint64_t off = (uint64_t)guest_addr - s_prx_stage_base;
    uint64_t end = off + n;
    if (end > 0x04000000u) return 1;            /* larger than all of user RAM */
    if (end > s_prx_stage_cap) {
        uint64_t cap = s_prx_stage_cap ? s_prx_stage_cap : 0x10000u;
        while (cap < end) cap *= 2u;
        uint8_t *grown = (uint8_t *)realloc(s_prx_stage, (size_t)cap);
        if (!grown) return 1;
        memset(grown + s_prx_stage_cap, 0, (size_t)(cap - s_prx_stage_cap));
        s_prx_stage = grown;
        s_prx_stage_cap = (uint32_t)cap;
    }
    if (n) memcpy(s_prx_stage + off, src, n);
    if (end > s_prx_stage_len) s_prx_stage_len = (uint32_t)end;
    return 0;
}

void mark_module_started(uint32_t entry) {
    for (unsigned i = 0; i < s_prx_image_count; i++) {
        if (entry >= s_prx_images[i].base && entry < s_prx_images[i].end && !s_prx_images[i].started) {
            s_prx_images[i].started = 1;
            atomic_fetch_add_explicit(&s_prx_started_count, 1, memory_order_release);
        }
    }
}

int prx_image_bounds(uint32_t base, uint32_t *start_out, uint32_t *end_out) {
    for (unsigned i = 0; i < s_prx_image_count; i++) {
        if (s_prx_images[i].base != base) continue;
        if (start_out) *start_out = s_prx_images[i].base;
        if (end_out) *end_out = s_prx_images[i].end;
        return 1;
    }
    return 0;
}

/* The build of the scePsmfPlayer library the title loaded, as that module records it: the
 * module_sdk_version variable (system library NID 0x11B97506) of a module whose module info
 * names the player library.  scePsmfPlayerGetPsmfInfo's width/height outputs exist only in
 * build 0x03090510 (#279).  Names and rule follow PPSSPP Core/HLE/sceKernelModule.cpp
 * (3f40ddc8), which takes the version from the ~PSP header when there is one; a plain ELF
 * module such as LocoRoco 2's has only the export.  0 until such a module is placed. */
#define PRX_NID_MODULE_SDK_VERSION 0x11B97506u

static void note_psmfplayer_lib_version(const SrPrxImage *img) {
    static const char *const names[] = {
        "scePsmfP_library", "scePsmfPlayer", "libpsmfplayer", "psmf_jk", "jkPsmfP_library",
    };
    int player = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strncmp(img->modname, names[i], sizeof(img->modname)) == 0) player = 1;
    if (!player) return;
    s_psmfplayer_lib_version = 0u;
    for (uint32_t i = 0; i < img->nexp; i++) {
        const SrPrxExport *e = &img->exps[i];
        if (e->is_func || e->lib[0] != '\0' || e->nid != PRX_NID_MODULE_SDK_VERSION) continue;
        if (sr_guest_span_readable(e->addr, 4u)) s_psmfplayer_lib_version = MEM_R32(e->addr);
        break;
    }
}

int load_prx_image(const char *host_path, uint32_t base, const char *name) {
    for (unsigned i = 0; i < s_prx_image_count; i++) {
        if (s_prx_images[i].base != base) continue;
        /* The image is already resident and is not re-staged over a running module's
         * live data, but this load still depends on it. Report the base so the new
         * record references that image; recording image_base == 0 would leave it
         * holding module_start/module_stop addresses into a range the image owner
         * frees on its own unload, and unload would never know to keep it alive. */
        s_last_prx_base = base;
        return 1;
    }
    if (s_prx_image_count >= sizeof(s_prx_images) / sizeof(s_prx_images[0])) return 0;

    SrPrxImage img;
    char err[160] = "";
    memset(&img, 0, sizeof(img));
    if (s_prx_stage && s_prx_stage_cap) memset(s_prx_stage, 0, s_prx_stage_cap);
    s_prx_stage_base = base;
    s_prx_stage_len = 0;
    s_prx_staging = 1;
    int rc = sr_prx_load(host_path, base, &img, err, sizeof(err));
    s_prx_staging = 0;
    if (rc != 0) {
        fprintf(stderr, "PRX image: %s at 0x%08x: load failed: %s\n", host_path, base, err);
        return 0;
    }
    uint32_t size = (img.end > base && img.end - base > s_prx_stage_len) ? img.end - base
                                                                         : s_prx_stage_len;
    uint32_t alloc_uid = 0xFFFFFFFFu;
    int ok = 0;
    if (size == 0 || img.start != base) {
        fprintf(stderr, "PRX image: %s: unexpected span [0x%08x,0x%08x) for base 0x%08x\n",
                host_path, img.start, img.end, base);
    } else if ((alloc_uid = sr_alloc_block_at(base, size, name)) == 0xFFFFFFFFu) {
        fprintf(stderr,
                "GUEST_MODULE_LOAD_ADDRESS_COLLISION: %s: declared range "
                "[0x%08x,0x%08x) is not free in the user partition; "
                "refusing to relocate (#308)\n",
                host_path, base, base + size);
    } else if (!sr_guest_span_writable(base, size)) {
        /* The reservation is already taken, so hand it straight back rather than
         * leaking a partition range this image will never occupy. */
        free_block(alloc_uid);
        fprintf(stderr, "PRX image: %s: range [0x%08x,0x%08x) not writable guest memory\n",
                host_path, base, base + size);
    } else {
        memcpy(SR_HOST(base), s_prx_stage, s_prx_stage_len);
        if (size > s_prx_stage_len) memset(SR_HOST(base + s_prx_stage_len), 0, size - s_prx_stage_len);
        s_prx_images[s_prx_image_count++] = (PrxImage){base, base + size, alloc_uid, 0, NULL};
        s_last_prx_base = base;
        note_psmfplayer_lib_version(&img);
        fprintf(stderr, "PRX image: %s -> [0x%08x,0x%08x) %u exports, %u import stubs\n",
                img.modname, base, base + size, img.nexp, img.nimp);
        ok = 1;
    }
    sr_prx_image_free(&img);
    return ok;
}

/* Load one manifest-declared guest module's image and register its exports at its
 * manifest base (the same base the recompiler used, from the title manifest's modules[]
 * list). */
static int populate_guest_module(const char *file, uint32_t base, int required) {
    s_last_prx_entry = 0;
    s_last_prx_stop = 0;
    s_last_prx_base = 0;
    char image_path[1024];
    int written = snprintf(image_path, sizeof(image_path), "%s/%s", guest_module_root(), file);
    if (written <= 0 || (size_t)written >= sizeof(image_path)) {
        fprintf(stderr, "GUEST_MODULE_LOAD_FAILED: manifest module path exceeds the runtime limit\n");
        return 0;
    }
    FILE *image = fopen(image_path, "rb");
    if (!image) {
        if (required) {
            fprintf(stderr,
                    "GUEST_MODULE_INPUT_MISSING: required module is absent from SR_MODULE_DIR; "
                    "refusing load at its manifest address\n");
            return 0;
        }
    } else {
        fclose(image);
        if (!load_prx_image(image_path, base, file)) return 0;
    }
    (void)register_known_module(guest_module_root(), file, base);
    if (!s_last_prx_entry) s_last_prx_entry = base;
    return 1;
}

/* ---- Guest-placed modules (#704) ----
 * A module the title manifest places at run time was translated ahead of time without a
 * load address (tools/codegen.py, position-independent in its link space). When the game
 * loads it, the module gets memory from the user partition by the Low policy the
 * kernel's loader uses by default, the project's PRX loader (prx_loader.c) relocates the
 * image to that address, and the translation is bound there (sr_module_code_bind).
 * Unload unbinds it and returns the memory, so overlays that never fit together run one
 * after another.
 *
 * What this loader cannot reproduce faithfully is refused by name and fails closed with
 * SCE_KERNEL_ERROR_NOTIMP: a module the build did not translate, a file that is not the
 * one that was translated, a second live instance of one translation, and a placement
 * request outside the user partition's Low policy. */

/* SceKernelLMOption, from the public PSPSDK pspmodulemgr.h: size, mpidtext, mpiddata,
 * flags, position, access, reserved[2]. Fields past `size` are absent and default. */
#define GUEST_MODULE_OPTION_BYTES 20u
#define GUEST_MODULE_PARTITION_USER 2u
#define GUEST_MODULE_POSITION_LOW 0u
#define GUEST_MODULE_POSITION_HIGH 1u
/* The user partition hands out 256-byte slots (alloc_block). */
#define GUEST_MODULE_PARTITION_GRANULE 0x100u

typedef struct {
    uint32_t mpidtext;
    uint32_t mpiddata;
    uint32_t position;
} GuestModuleLoadOption;

static uint32_t guest_module_boundary(const char *boundary, const char *file, const char *detail) {
    fprintf(stderr, "GUEST_MODULE_BOUNDARY: %s module=%s: %s; in the works (#704)\n",
            boundary, file, detail);
    return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
}

/* Libfont's readiness belongs to its own module_start (#299). When that cannot run -- the
 * build did not translate it, its image was refused, it records no entry, or the operator's
 * SR_REAL_MODULE_START=0 kill switch keeps module_start off -- the game waits for readiness
 * nothing will ever set. The runtime never synthesizes it, so the run stops by name: one
 * semantic-boundary line in SR_BOOT_EVENT_FILE (the shape the dispatcher and the
 * unsupported-import stop write), which the player turns into its stopped-early card
 * instead of a frozen game, then its own exit status. The selftest harness counts the stop
 * and returns the named refusal, so a test can assert it. */
#define HLE_LIBFONT_STARTUP_STOP_EXIT 11
#define HLE_LIBFONT_STARTUP_BOUNDARY "libfont-startup-unavailable"
#ifdef SR_HLE_THREAD_SELFTEST
static uint32_t s_hle_libfont_startup_stops;
uint32_t sr_hle_test_libfont_startup_stops(void) { return s_hle_libfont_startup_stops; }
#endif
static uint32_t hle_libfont_startup_stop(const char *reason) {
    fprintf(stderr, "LIBFONT_STARTUP_UNAVAILABLE: %s; translated guest libfont startup is not "
                    "supported yet on this route; refusing to synthesize readiness, so the run "
                    "stops at %s\n", reason, HLE_LIBFONT_STARTUP_BOUNDARY);
    const char *path = getenv("SR_BOOT_EVENT_FILE");
    if (path && path[0]) {
        FILE *events = fopen(path, "ab");
        if (events) {
            fprintf(events,
                    "BOOT_EVENT phase=stop reason=semantic-boundary boundary=%s issue=%u\n",
                    HLE_LIBFONT_STARTUP_BOUNDARY, 299u);
            fclose(events);
        }
    }
#ifdef SR_HLE_THREAD_SELFTEST
    s_hle_libfont_startup_stops++;
    return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
#else
    sr_hit_hle = 1;
    if (sr_sched_on) {
        sr_trace_close();
        sr_compat_report();
        sr_perf_shutdown();
        fflush(stderr);
        _Exit(HLE_LIBFONT_STARTUP_STOP_EXIT);
    }
    longjmp(g_hle_jmp, 1);
#endif
}

static uint32_t read_guest_module_option(uint32_t option, GuestModuleLoadOption *out) {
    memset(out, 0, sizeof(*out));
    if (!option) return 0;
    if (!sr_guest_span_readable(option, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t declared = MEM_R32(option);
    uint32_t span = declared < GUEST_MODULE_OPTION_BYTES ? declared : GUEST_MODULE_OPTION_BYTES;
    if (span > 4u && !sr_guest_span_readable(option, span)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (span >= 8u) out->mpidtext = MEM_R32(option + 4u);
    if (span >= 12u) out->mpiddata = MEM_R32(option + 8u);
    if (span >= 17u) out->position = MEM_R8(option + 16u);
    return 0;
}

/* Read a staged module file whole. Returns 0 when it is absent; a file that exists but
 * cannot be read completely is reported through *unreadable. */
static unsigned char *read_guest_module_file(const char *path, size_t *size_out, int *unreadable) {
    *size_out = 0;
    *unreadable = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *bytes = NULL;
    long length = -1;
    if (fseek(f, 0, SEEK_END) == 0) length = ftell(f);
    if (length <= 0 || (uint64_t)length > 0x04000000u || fseek(f, 0, SEEK_SET) != 0 ||
        !(bytes = (unsigned char *)malloc((size_t)length)) ||
        fread(bytes, 1, (size_t)length, f) != (size_t)length) {
        free(bytes);
        fclose(f);
        *unreadable = 1;
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)length;
    return bytes;
}

int loadexec_module_file_matches_translation(const char *file) {
    if (!file || !file[0]) return 0;
    const SrModuleCode *code = sr_module_code_find(file);
    if (!code || code->unsupported_reason) return 0;
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/%s", guest_module_root(), file);
    if (written <= 0 || (size_t)written >= sizeof(path)) return 0;
    size_t size = 0u;
    int unreadable = 0;
    unsigned char *bytes = read_guest_module_file(path, &size, &unreadable);
    if (!bytes) return 0;
    uint64_t digest = sr_module_code_fnv1a64(bytes, size);
    free(bytes);
    return size == code->file_size && digest == code->file_fnv1a64;
}

/* Libraries a title ships as its own module, for which host HLE only stands in. When the
 * title's copy is staged and translated, its exports are the library (LLE first, AGENTS.md
 * section 6): the module is not host-served, so it starts as any guest-placed module does
 * and its exports take over the host handlers once it has started. The host handlers stay
 * for a title that does not stage the module and for the public synthetic fixtures (#286).
 * scePsmfPlayer is the SDK's libpsmfplayer.prx: user-mode code over sceMpeg, scePsmf and
 * the kernel, all of which stay reachable beneath it. */
static int guest_module_owns_library(const char *lib) {
    static const char *const owned[] = { "scePsmfPlayer" };
    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); i++)
        if (strcmp(lib, owned[i]) == 0) return 1;
    return 0;
}

/* Lay a guest-placed module out and bind its translation. On success `mod` holds the
 * module's entry points, image and $gp, and 0 is returned; otherwise the guest-visible
 * error is returned and nothing the attempt reserved stays reserved. */
static uint32_t load_guest_placed_module(const char *file, int required, uint32_t option,
                                         LoadedModule *mod) {
    char detail[192];
    GuestModuleLoadOption opt;
    uint32_t error = read_guest_module_option(option, &opt);
    if (error) return error;
    if ((opt.mpidtext && opt.mpidtext != GUEST_MODULE_PARTITION_USER) ||
        (opt.mpiddata && opt.mpiddata != GUEST_MODULE_PARTITION_USER)) {
        snprintf(detail, sizeof(detail), "requested partitions text=%u data=%u; only the user "
                 "partition is modelled", opt.mpidtext, opt.mpiddata);
        return guest_module_boundary("guest-module-partition-unsupported", file, detail);
    }
    if (opt.position != GUEST_MODULE_POSITION_LOW && opt.position != GUEST_MODULE_POSITION_HIGH) {
        snprintf(detail, sizeof(detail), "requested placement position %u; only Low and High "
                 "placement are modelled", opt.position);
        return guest_module_boundary("guest-module-placement-unsupported", file, detail);
    }

    char image_path[1024];
    int written = snprintf(image_path, sizeof(image_path), "%s/%s", guest_module_root(), file);
    if (written <= 0 || (size_t)written >= sizeof(image_path))
        return guest_module_boundary("guest-module-path-too-long", file,
                                     "the staged module path exceeds the runtime limit");
    size_t size = 0;
    int unreadable = 0;
    unsigned char *bytes = read_guest_module_file(image_path, &size, &unreadable);
    if (!bytes && !unreadable) {
        if (required) {
            fprintf(stderr, "GUEST_MODULE_INPUT_MISSING: required module %s is absent from "
                            "SR_MODULE_DIR; refusing load (#296)\n", file);
            return 0x80020190u;  /* SCE_KERNEL_ERROR_NO_MEMORY, as the fixed-address path */
        }
        /* An optional module that is not staged is served by host HLE, as an optional
         * fixed-address module is: the record exists, no image and no translation. */
        mod->runtime_placed = 1;
        fprintf(stderr, "GUEST_MODULE_HOST_SERVED: optional module %s is not staged; host "
                        "HLE serves its imports\n", file);
        return 0;
    }
    if (!bytes)
        return guest_module_boundary("guest-module-input-unreadable", file,
                                     "the staged module file could not be read");

    const SrModuleCode *code = sr_module_code_find(file);
    uint32_t base = 0;
    if (!code) {
        free(bytes);
        return guest_module_boundary("guest-module-untranslated", file,
                                     "the build did not translate this module");
    }
    uint64_t digest = sr_module_code_fnv1a64(bytes, size);
    if ((uint64_t)size != code->file_size || digest != code->file_fnv1a64) {
        free(bytes);
        snprintf(detail, sizeof(detail), "the staged file (0x%x bytes, FNV-1a 0x%016llx) is "
                 "not the file the build translated (0x%x bytes, FNV-1a 0x%016llx)",
                 (unsigned)size, (unsigned long long)digest,
                 code->file_size, (unsigned long long)code->file_fnv1a64);
        return guest_module_boundary("guest-module-identity-mismatch", file, detail);
    }
    if (code->unsupported_reason) {
        free(bytes);
        return guest_module_boundary("guest-module-translation-unsupported", file,
                                     code->unsupported_reason);
    }
    if (sr_module_code_is_bound(code, &base)) {
        free(bytes);
        snprintf(detail, sizeof(detail), "one instance is already live at 0x%08x; each "
                 "translation binds one image", base + code->link_low);
        return guest_module_boundary("guest-module-second-instance", file, detail);
    }
    uint32_t span = code->link_high - code->link_low;
    uint32_t align = code->alignment > GUEST_MODULE_PARTITION_GRANULE
                         ? code->alignment : GUEST_MODULE_PARTITION_GRANULE;
    if (code->link_high <= code->link_low || (align & (align - 1u)) != 0u ||
        span > UINT32_MAX - (align - GUEST_MODULE_PARTITION_GRANULE)) {
        free(bytes);
        snprintf(detail, sizeof(detail), "image extent [0x%08x,0x%08x) alignment 0x%x",
                 code->link_low, code->link_high, code->alignment);
        return guest_module_boundary("guest-module-layout-unsupported", file, detail);
    }
    if (s_prx_image_count >= sizeof(s_prx_images) / sizeof(s_prx_images[0])) {
        free(bytes);
        return guest_module_boundary("guest-module-table-full", file,
                                     "every resident-image slot is live");
    }

    /* Memory, as the kernel's loader takes it: for a relocatable module, the lowest free
     * user-partition range that holds the whole image (PSP_SMEM_Low), or with load option
     * position 1 the highest (PSP_SMEM_High, alloc_block_high); uOFW src/kd/modulemgr/modulemgr.c
     * (MIT) allocates a PRX body with the option's position as the block type. A relocatable
     * image is aligned inside its block, which carries the slack an alignment above the
     * partition granule needs; where inside that block the PSP's aligned High variant puts the
     * image is not measured. A fixed-address image owns exactly its link range whatever the
     * position, as uOFW's ELF path allocates by address. The status the guest sees when the
     * partition cannot hold the image is SCE_KERNEL_ERROR_NO_MEMORY (unmeasured). */
    uint32_t alloc_uid;
    uint32_t image = 0;
    if (code->relocatable) {
        uint32_t padded = span + (align - GUEST_MODULE_PARTITION_GRANULE);
        if (opt.position == GUEST_MODULE_POSITION_HIGH && !partition_high_modelled()) {
            free(bytes);
            return guest_module_boundary("guest-module-placement-unsupported", file,
                                         "High placement in a partition based below the runtime "
                                         "stacks is not modelled");
        }
        alloc_uid = opt.position == GUEST_MODULE_POSITION_HIGH ? alloc_block_high(padded)
                                                               : alloc_block(padded);
        if (alloc_uid != 0xFFFFFFFFu) {
            uint32_t block = block_addr(alloc_uid);
            image = (block + align - 1u) & ~(align - 1u);
            base = image - code->link_low;
        }
    } else {
        alloc_uid = sr_alloc_block_at(code->link_low, span, file);
        image = code->link_low;
        base = 0;
    }
    if (alloc_uid == 0xFFFFFFFFu) {
        free(bytes);
        fprintf(stderr, "GUEST_MODULE_LOAD_NO_MEMORY: %s needs 0x%x bytes%s; the user "
                        "partition cannot hold it now\n", file, span,
                code->relocatable ? "" : " at its fixed link address");
        return 0x80020190u;  /* SCE_KERNEL_ERROR_NO_MEMORY */
    }

    /* Relocate into the host staging buffer; guest memory is written only once the
     * loader has accepted the whole image. */
    SrPrxImage img;
    char err[160] = "";
    memset(&img, 0, sizeof(img));
    if (s_prx_stage && s_prx_stage_cap) memset(s_prx_stage, 0, s_prx_stage_cap);
    s_prx_stage_base = image;
    s_prx_stage_len = 0;
    s_prx_staging = 1;
    int rc = sr_prx_load_from_memory(bytes, size, base, &img, err, sizeof(err));
    s_prx_staging = 0;
    uint32_t text_size = 0, data_size = 0, bss_size = 0;
    const int sized = rc == 0 && prx_text_data_bss(bytes, size, &text_size, &data_size, &bss_size);
    free(bytes);
    const char *refusal = NULL;
    if (rc != 0) {
        refusal = err;
    } else if (img.start != image || img.end > image + span || s_prx_stage_len > span) {
        snprintf(detail, sizeof(detail), "the loader laid the image out at [0x%08x,0x%08x), "
                 "not inside [0x%08x,0x%08x)", img.start, img.end, image, image + span);
        refusal = detail;
    } else if (!sr_guest_span_writable(image, span)) {
        refusal = "the allocated range is not writable guest memory";
    }
    if (refusal) {
        char reason[192];
        snprintf(reason, sizeof(reason), "%s", refusal);
        sr_prx_image_free(&img);
        free_block(alloc_uid);
        return guest_module_boundary("guest-module-image-refused", file, reason);
    }
    memcpy(SR_HOST(image), s_prx_stage, s_prx_stage_len);
    if (span > s_prx_stage_len) memset(SR_HOST(image + s_prx_stage_len), 0, span - s_prx_stage_len);

    SrModuleBindDetail bind_detail;
    SrModuleBindResult bound = sr_module_code_bind(code, base, &bind_detail);
    if (bound != SR_MODULE_BIND_OK) {
        snprintf(detail, sizeof(detail), "%s at 0x%08x", sr_module_bind_result_name(bound),
                 bind_detail.address);
        sr_prx_image_free(&img);
        free_block(alloc_uid);
        return guest_module_boundary("guest-module-bind-refused", file, detail);
    }
    s_prx_images[s_prx_image_count++] = (PrxImage){image, image + span, alloc_uid, 0, code};
    note_psmfplayer_lib_version(&img);

    /* Exports: the named libraries' functions become link targets for importers once the
     * module has started (started_module_export); the system library supplies the stop
     * entry. A named export host HLE also implements marks the module host-served. */
    uint32_t module_stop = 0;
    unsigned exported = 0;
    for (uint32_t i = 0; i < img.nexp; i++) {
        const SrPrxExport *e = &img.exps[i];
        if (!e->is_func) continue;
        if (e->lib[0] == '\0') {
            if (e->nid == 0xCEE05613u) module_stop = e->addr;
            continue;
        }
        if (sr_hle_register_late_import(e->nid, e->addr)) exported++;
        if (hle_find(e->nid) && !guest_module_owns_library(e->lib)) mod->host_served = 1;
    }
    mod->runtime_placed = 1;
    mod->code = code;
    mod->gp = img.gp;
    /* The start routine is the system library's module_start, else the ELF entry -- when
     * that lies in the image. A library module that declares neither (an entry of
     * 0xFFFFFFFF, which the loader reports one byte below the image) has no start
     * routine at all: 0. */
    if (img.has_mod_start)
        mod->module_start = img.mod_start;
    else if (img.entry >= image && img.entry < image + span)
        mod->module_start = img.entry;
    else
        mod->module_start = 0;
    mod->module_stop = module_stop;
    mod->image_base = image;
    mod->image_end = image + span;
    memset(&mod->info, 0, sizeof(mod->info));
    mod->has_info = sized && img.nseg <= 4u;
    if (mod->has_info) {
        mod->info.nsegment = img.nseg;
        for (uint32_t i = 0; i < img.nseg; i++) {
            mod->info.segment_addr[i] = img.segs[i].guest_addr;
            mod->info.segment_size[i] = img.segs[i].mem_size;
        }
        mod->info.entry_addr = img.entry;
        mod->info.gp_value = img.gp;
        mod->info.text_addr = image;
        mod->info.text_size = text_size;
        mod->info.data_size = data_size;
        mod->info.bss_size = bss_size;
        mod->info.attribute = img.attr;
        mod->info.version[0] = img.version[0];
        mod->info.version[1] = img.version[1];
        memcpy(mod->info.name, img.modname, sizeof(mod->info.name));
    }
    sr_flight_prx_load(image, 0u, mod->module_start, img.nimp, exported);
    sr_prx_image_free(&img);
    return 0;
}

/* sceKernelLoadModule's work for one guest path. Only modules the title manifest
 * declares (by guest_path) were translated; any other path gets a record whose start
 * the host bypass serves. */
uint32_t load_module_by_guest_path(const char *path, uint32_t option) {
    const char *file = NULL;
    uint32_t base = 0;
    int required = 0;
    SrGuestModulePlacement placement = SR_GUEST_MODULE_FIXED;
    int declared = sr_title_config_guest_module(path, &file, &base, &required, &placement);
    LoadedModule *mod = alloc_loaded_module_slot();
    if (!mod) return 0x80020190u;  /* SCE_KERNEL_ERROR_NO_MEMORY */
    s_last_prx_entry = 0;
    s_last_prx_stop = 0;
    s_last_prx_base = 0;
    s_last_prx_host_served = 0;
    if (declared && placement == SR_GUEST_MODULE_RUNTIME) {
        uint32_t error = load_guest_placed_module(file, required, option, mod);
        if (error) {
            retire_loaded_module(mod);
            fprintf(stderr, "sceKernelLoadModule(\"%s\") -> 0x%08x\n", path, error);
            /* A named boundary refused libfont's translated image: its module_start can
             * never run, so the run stops by name rather than leave the game waiting. */
            if (error == HLE_KERNEL_ERROR_NOT_IMPLEMENTED && module_path_is(path, "libfont.prx"))
                return hle_libfont_startup_stop("the translated libfont image was refused at load");
            return error;
        }
    } else if (declared && !populate_guest_module(file, base, required)) {
        retire_loaded_module(mod);
        fprintf(stderr,
                "sceKernelLoadModule(\"%s\") failed closed at its manifest address\n",
                path);
        return 0x80020190u;  /* SCE_KERNEL_ERROR_NO_MEMORY */
    } else {
        mod->module_start = s_last_prx_entry;
        mod->module_stop = s_last_prx_stop;
        mod->image_base = s_last_prx_base;
        mod->host_served = s_last_prx_host_served;
        (void)prx_image_bounds(mod->image_base, NULL, &mod->image_end);
    }
    uint32_t uid = sr_alloc_uid();
    fprintf(stderr, "sceKernelLoadModule(\"%s\") -> uid=0x%x\n", path, uid);
    mod->uid = uid;
    snprintf(mod->path, sizeof(mod->path), "%s", path);
    mod->state = MODULE_STATE_LOADED;
    if (mod->code) {
        fprintf(stderr, "GUEST_MODULE_PLACED: %s uid=0x%x image=[0x%08x,0x%08x) gp=0x%08x "
                        "start=0x%08x stop=0x%08x%s\n",
                file, uid, mod->image_base,
                mod->image_base + (mod->code->link_high - mod->code->link_low), mod->gp,
                mod->module_start, mod->module_stop,
                mod->host_served ? " host-served" : "");
    }
    return uid;
}
/* Module-state errors are defined in the public PSPSDK pspkerror.h header. The
 * lifecycle mapping below is source-backed and remains unmeasured on hardware. */
#define SCE_KERNEL_ERROR_ALREADY_STARTED    0x80020133u
#define SCE_KERNEL_ERROR_NOT_STARTED        0x80020134u
#define SCE_KERNEL_ERROR_ALREADY_STOPPED    0x80020135u
#define SCE_KERNEL_ERROR_CAN_NOT_STOP       0x80020136u
#define SCE_KERNEL_ERROR_ALREADY_STOPPING   0x8002013fu
#define SCE_KERNEL_ERROR_MODULE_MGR_BUSY    0x80020143u

static const char *module_state_name(LoadedModuleState state) {
    switch (state) {
    case MODULE_STATE_LOADED: return "loaded";
    case MODULE_STATE_STARTING: return "starting";
    case MODULE_STATE_STARTED: return "started";
    case MODULE_STATE_STOPPING: return "stopping";
    case MODULE_STATE_STOPPED: return "stopped";
    case MODULE_STATE_UNLOADED: return "unloaded";
    default: return "invalid";
    }
}

static uint32_t reject_module_transition(const char *operation, uint32_t uid,
                                         const LoadedModule *mod, uint32_t error) {
    fprintf(stderr, "PRX_MODULE_INVALID_TRANSITION: %s uid=0x%x state=%s error=0x%08x\n",
            operation, uid, module_state_name(mod->state), error);
    return error;
}

/* Host path helpers defined with the rest of the host-path layer below. */
static int sr_exit_artifact_path_alloc(const wchar_t *name, char **out);

/* sceKernelExitGame terminates the guest process in thread and interrupt contexts. */
static uint32_t h_ExitGame(CpuState *s) {
    /* sceKernelExitGame takes no argument (PSPSDK psploadexec.h declares
     * `void sceKernelExitGame(void)`; PPSSPP registers NID 0x05572a5f with the
     * empty argument signature, and PSPAutotests tests/modules/loadexec-imports.S
     * imports it as an export distinct from sceKernelExitGameWithStatus
     * 0x2ac9954b, which is the one that does take a status). $a0 therefore holds
     * whatever the caller last left in it and must never become the host process
     * result -- a caller with a stale register would otherwise pick the runtime
     * exit status. Guest termination is unconditionally a successful host exit. */
    const int code = 0;
    int cur = sched_current_uid();
    fprintf(stderr, "GAMELOG: Guest requested exit (sceKernelExitGame()) uid=0x%x ra=0x%08x pc=0x%08x sp=0x%08x\n",
            cur, s->r[31], s->pc, s->r[29]);
    fprintf(stderr, "  regs: gp=%08x fp=%08x s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x s6=%08x s7=%08x\n",
            s->r[28], s->r[30], s->r[16], s->r[17], s->r[18], s->r[19], s->r[20], s->r[21], s->r[22], s->r[23]);
    fprintf(stderr, "  stack[%08x..%08x]:", s->r[29], s->r[29] + 0x3cu);
    for (uint32_t off = 0; off < 0x40; off += 4) {
        if ((off & 0x0f) == 0) fprintf(stderr, "\n    +%02x:", off);
        fprintf(stderr, " %08x", MEM_R32(s->r[29] + off));
    }
    fputc('\n', stderr);
    fflush(stderr);
    sr_flight_exit(0u);


    /* Terminal on hardware in every context.  Do not park the calling thread to keep the
     * runtime observable; that legacy diagnostic path allowed execution after ExitGame. */
    sr_trace_close();

    /* Write crash dump and exit flag for tools/mem_debug.py to trap and trace. They go
     * beside the executable that produced them, where tools/mem_debug.py
     * build_artifact_dir looks, so each title writes its own dump. The folder is
     * resolved at run time, never compiled in: a packaged game writes beside its own
     * executable, and the compiled object carries no build path (#1306). */
    char *sr_dump_path = NULL, *sr_flag_path = NULL;
    sr_exit_artifact_path_alloc(L"crash_dump.bin", &sr_dump_path);
    sr_exit_artifact_path_alloc(L"exited.flag", &sr_flag_path);
    FILE *f_dump = sr_dump_path ? sr_fopen_utf8(sr_dump_path, L"wb") : NULL;
    if (f_dump) {
        /* Versioned header so tools/mem_debug.py can reject dumps written by a
         * different CpuState ABI instead of misreading them:
         * "SRCD", format, ABI version, sizeof(CpuState), stack base, stack bytes. */
        uint32_t sp_base = s->r[29] & 0xFFFF0000u;
        uint32_t stack_len = sr_inrange(sp_base) ? 0x10000u : 0u;
        uint32_t hdr[6] = { 0x44435253u, 1u, SR_CPUSTATE_ABI_VERSION,
                            (uint32_t)sizeof(CpuState), sp_base, stack_len };
        fwrite(hdr, 1, sizeof hdr, f_dump);
        fwrite(s, 1, sizeof(CpuState), f_dump);
        if (stack_len) {
            fwrite(SR_HOST(sp_base), 1, stack_len, f_dump);
        }
        fclose(f_dump);
    }
    FILE *f_flag = sr_flag_path ? sr_fopen_utf8(sr_flag_path, L"w") : NULL;
    if (f_flag) {
        fprintf(f_flag, "1\n");
        fclose(f_flag);
    }
    free(sr_dump_path);
    free(sr_flag_path);

    fflush(NULL);
    if (cur == 0) {
        /* Interrupt context (PSP kernel callback chain). Hard-stop the host process: this
         * matches hardware (the kernel never returns to the caller once ExitGame fires).
         * A clean Crt exit lets the operator see the last 0.5 MB of trace before the
         * window disappears. */
        fprintf(stderr, "  pc=0x%08x ra=0x%08x sp=0x%08x gp=0x%08x v0=0x%08x a0=0x%08x a1=0x%08x\n",
                s->pc, s->r[31], s->r[29], s->r[28], s->r[2], s->r[4], s->r[5]);
        fprintf(stderr, "  insn[vblank-cb-begin]=0x%08x insn[vblank-cb-next]=0x%08x\n",
                s->pc ? MEM_R32(s->pc) : 0, s->pc ? MEM_R32(s->pc + 4) : 0);
        fflush(stderr);
        exit(code);
    }
    exit(code);
    return code; /* unreachable, keeps the handler signature honest */
}
/* Module identity by address (#1385). A live module record owns its image
 * [image_base, image_end); the program the host loader placed is the main module and
 * owns [sr_loaded_base(), sr_loaded_end()). A program LoadExec booted is itself a module
 * record. The main module's UID comes from the shared pool the first time a guest asks
 * for it, so it never aliases another kernel object (it used to be the constant 0x112,
 * the third UID the pool hands out, which a thread or semaphore also receives). */
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE 0x8002012eu
uint32_t s_main_module_uid;

static uint32_t module_uid_at(uint32_t addr) {
    for (int i = 0; i < s_nloaded_modules; i++) {
        const LoadedModule *mod = &s_loaded_modules[i];
        if (!mod->in_use || mod->state == MODULE_STATE_UNUSED ||
            mod->state == MODULE_STATE_UNLOADED)
            continue;
        if (mod->image_base < mod->image_end && addr >= mod->image_base &&
            addr < mod->image_end)
            return mod->uid;
    }
    uint32_t base = sr_loaded_base(), end = sr_loaded_end();
    if (base < end && addr >= base && addr < end) {
        if (!s_main_module_uid) s_main_module_uid = sr_alloc_uid();
        return s_main_module_uid;
    }
    return 0u;
}

/* An address no live module owns -- unmapped, or the range of a module since unloaded --
 * is refused. The PSP's error for it is unmeasured; this is the kernel's unknown-module
 * error, assumed until a hardware capture pins it. */
static uint32_t h_GetModuleIdByAddress(CpuState *s) {
    uint32_t uid = module_uid_at(A0);
    return uid ? uid : SCE_KERNEL_ERROR_UNKNOWN_MODULE;
}

/* The calling module: the one whose code issued the call, found by the return address
 * the caller left in $ra. */
static uint32_t h_GetModuleId(CpuState *s) {
    uint32_t uid = module_uid_at(s->r[31]);
    return uid ? uid : SCE_KERNEL_ERROR_UNKNOWN_MODULE;
}

/* The main module's kernel record, registered by the generated sr_register_all() when the
 * ELF has a SceModuleInfo (tools/codegen.py main_module_record). NULL when it has none, and
 * again after LoadExec replaces the program. */
const SrMainModuleRecord *s_main_module_record;

#define SCE_MODULE_PRIVILEGE_LEVELS        0x1e00u   /* MS|USB_WLAN|APP|VSH|KERNEL */
#define SCE_KERNEL_TH_USER_DEFAULT_STACK   0x40000u
#define SCE_KERNEL_TH_USER_MODE            0x80000000u

/* The module manager creates the module-start thread from the record (uOFW
 * src/kd/modulemgr/modulemgr.c, _StartModule): the declared module_start_thread_parameter
 * priority, stack size and attribute, or the module-init priority 32 and the 256 KiB user
 * stack when the module declares none; the attribute's top nibble is cleared and the
 * user-mode bit set, and the thread is named "SceModmgrStart" and runs with the module's gp.
 * A game's main module is a user module: the loader refuses to boot one whose privilege
 * level is not user (uOFW src/kd/loadcore/loadelf.c, sceKernelProbeExecutableObject). */
void sr_main_module_register(const SrMainModuleRecord *record) {
    s_main_module_record = record;
    if (!record) return;
    int priority = SCE_KERNEL_MODULE_INIT_PRIORITY;
    uint32_t stack_size = 0u, attr = 0u;
    if (record->has_start_thread_param) {
        priority = (int)record->start_thread_priority;
        stack_size = record->start_thread_stack_size;
        attr = record->start_thread_attr;
    }
    if (stack_size == 0u) stack_size = SCE_KERNEL_TH_USER_DEFAULT_STACK;
    attr = (attr & ~0xf0000000u) | SCE_KERNEL_TH_USER_MODE;
    sched_set_module_start_thread(priority, stack_size, attr, "SceModmgrStart",
                                  record->gp_value);
}
static uint32_t h_QueryModuleInfo(CpuState *s) {
    const uint32_t uid = A0, info = A1;
    if (sched_is_intr_context()) return 0x80020064u;      /* CANNOT_BE_CALLED_FROM_INTERRUPT */
    if (info == 0u || !sr_guest_span_writable(info, 96u)) return 0x800200d3u; /* ILLEGAL_ADDR */
    const uint32_t size = MEM_R32(info);
    if ((sr_sdkver_get(&g_sdk_version) & 0xffff0000u) >= 0x02080000u && size != 64u && size != 96u)
        return 0x800201bcu;                               /* ILLEGAL_SIZE */
    const int is_main = uid != 0u && uid == s_main_module_uid &&
                        sr_loaded_base() < sr_loaded_end();
    const SrMainModuleRecord *r = s_main_module_record;
    if (!is_main) {
        const LoadedModule *mod = find_loaded_module(uid);
        if (!mod) return SCE_KERNEL_ERROR_UNKNOWN_MODULE;
        if (mod->has_info) {
            r = &mod->info;
            goto answer;
        }
        fprintf(stderr, "SEMANTIC_BOUNDARY: MODULE_INFO_RECORD_UNAVAILABLE: sceKernelQueryModuleInfo "
                        "uid=0x%x is a module the runtime did not place, whose record holds no segment table, sizes, "
                        "attribute, version or name; refusing (0x80020002)\n", uid);
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    if (!r) {
        fprintf(stderr, "SEMANTIC_BOUNDARY: MODULE_INFO_RECORD_UNAVAILABLE: sceKernelQueryModuleInfo "
                        "uid=0x%x is the main module, but this build carries no record of it (the "
                        "ELF has no SceModuleInfo); refusing (0x80020002)\n", uid);
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
answer:
    MEM_W8(info + 4u, (uint8_t)r->nsegment);
    for (uint32_t i = 0; i < 4u; i++) {
        MEM_W32(info + 8u + 4u * i, r->segment_addr[i]);
        MEM_W32(info + 24u + 4u * i, r->segment_size[i]);
    }
    MEM_W32(info + 40u, r->entry_addr);
    MEM_W32(info + 44u, r->gp_value);
    MEM_W32(info + 48u, r->text_addr);
    MEM_W32(info + 52u, r->text_size);
    MEM_W32(info + 56u, r->data_size);
    MEM_W32(info + 60u, r->bss_size);
    if (size == 96u) {
        MEM_W16(info + 64u, (uint16_t)(r->attribute & ~SCE_MODULE_PRIVILEGE_LEVELS));
        MEM_W8(info + 66u, r->version[0]);
        MEM_W8(info + 67u, r->version[1]);
        int ended = 0;                                    /* strncpy: copy, then NUL pad */
        for (uint32_t i = 0; i < 27u; i++) {
            if (!ended && r->name[i] == 0u) ended = 1;
            MEM_W8(info + 68u + i, ended ? 0u : r->name[i]);
        }
        MEM_W8(info + 95u, 0u);
    }
    return 0;
}

static uint32_t h_StopModule_Trace(CpuState *s) {
    uint32_t modid = A0;
    LoadedModule *mod = find_loaded_module(modid);
    /* libfont's translated module_start runs under the default gate (module_start_policy),
     * so its module_stop runs too. A trace-only stop here would leave the module STARTED,
     * and sceKernelUnloadModule would refuse it with ALREADY_LOADED. */
    if (!real_module_start_enabled() &&
        (!mod || (!mod->runtime_placed && !module_path_is(mod->path, "libfont.prx") &&
                  mod->state == MODULE_STATE_STARTED &&
                  mod->start_entry_ran && mod->module_stop != 0))) {
        uint32_t uid = sched_current_uid();
        fprintf(stderr, "TRACE_STOPMODULE: uid=0x%x pc=0x%08x ra=0x%08x modid=0x%08x\n",
                uid, s->pc, s->r[31], s->r[4]);
        fflush(stderr);
        return 0;
    }

    uint32_t arglen = A1;
    uint32_t argp = A2;
    uint32_t status_ptr = A3;
    fprintf(stderr, "sceKernelStopModule(modid=0x%x, arglen=%u, argp=0x%x)\n", modid, arglen, argp);

    if (!mod) {
        return SCE_ERROR_MODULE_BAD_ID; /* unmeasured */
    }
    switch (mod->state) {
    case MODULE_STATE_STARTED:
        break;
    case MODULE_STATE_LOADED:
        return reject_module_transition("sceKernelStopModule", modid, mod,
                                        SCE_KERNEL_ERROR_NOT_STARTED);
    case MODULE_STATE_STOPPED:
        return reject_module_transition("sceKernelStopModule", modid, mod,
                                        SCE_KERNEL_ERROR_ALREADY_STOPPED);
    case MODULE_STATE_STOPPING:
        return reject_module_transition("sceKernelStopModule", modid, mod,
                                        SCE_KERNEL_ERROR_ALREADY_STOPPING);
    case MODULE_STATE_STARTING:
        return reject_module_transition("sceKernelStopModule", modid, mod,
                                        SCE_KERNEL_ERROR_MODULE_MGR_BUSY);
    default:
        return SCE_ERROR_MODULE_BAD_ID;
    }

    if (stack_arg(s, 0) != 0u) {
        fprintf(stderr, "PRX_MODULE_STOP_OPTION_SYNTHETIC: nonzero SceKernelSMOption is "
                        "unmeasured; refusing instead of guessing thread parameters\n");
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }

    if (!mod->start_entry_ran) {
        fprintf(stderr, "PRX_MODULE_HOST_SKIPPED_STOP: sceKernelStopModule uid=0x%x "
                        "start entry was host-skipped; stop entry not called (#280)\n",
                modid);
        mod->state = MODULE_STATE_STOPPED;
        return 0;
    }
    if (mod->module_stop == 0) {
        /* A missing module_stop entry is not the module attribute that forbids stopping.
         * Main returned success here; treating it as a no-op is an inference and remains
         * unmeasured on physical PSP hardware. */
        fprintf(stderr, "PRX_MODULE_STOP_NO_ENTRY: sceKernelStopModule uid=0x%x "
                        "has no declared module_stop entry; treating as success "
                        "(inference, hardware-unmeasured)\n", modid);
        mod->state = MODULE_STATE_STOPPED;
        return 0;
    }
    if (sr_lookup(mod->module_stop) == NULL) {
        fprintf(stderr, "PRX_MODULE_STOP_UNAVAILABLE: sceKernelStopModule uid=0x%x "
                        "module_stop=0x%08x is untranslated; in the works (#280)\n",
                modid, mod->module_stop);
        return SCE_KERNEL_ERROR_CAN_NOT_STOP;
    }
    mod->state = MODULE_STATE_STOPPING;
    fprintf(stderr, "PRX_MODULE_STOP_THREAD_SYNTHETIC: uid=0x%x stop=0x%08x "
                    "uses scheduler default priority/stack; module option rules remain unmeasured\n",
            modid, mod->module_stop);
    uint32_t rv = 0u;
    uint32_t call_rc = module_stop_thread_invoke(s, mod, arglen, argp, &rv);
    if (call_rc != 0u) {
        mod->state = MODULE_STATE_STARTED;
        return call_rc;
    }
    if (status_ptr && sr_guest_span_writable(status_ptr, 4u)) {
        MEM_W32(status_ptr, rv);
    }
    mod->state = MODULE_STATE_STOPPED;
    return 0;
}

static uint32_t h_UnloadModule_Trace(CpuState *s) {
    uint32_t modid = A0;
    LoadedModule *mod = find_loaded_module(modid);
    if (!real_module_start_enabled() && !mod) {
        uint32_t uid = sched_current_uid();
        fprintf(stderr, "TRACE_UNLOADMODULE: uid=0x%x pc=0x%08x ra=0x%08x modid=0x%08x\n",
                uid, s->pc, s->r[31], s->r[4]);
        fflush(stderr);
        return 0;
    }

    fprintf(stderr, "sceKernelUnloadModule(modid=0x%x)\n", modid);

    if (!mod) {
        return SCE_ERROR_MODULE_BAD_ID; /* unmeasured */
    }
    /* The public module manager contract unloads stopped modules. A loaded module
     * that has never started has no running guest state, so it can be released too. */
    switch (mod->state) {
    case MODULE_STATE_LOADED:
    case MODULE_STATE_STOPPED:
        break;
    case MODULE_STATE_STARTED:
        return reject_module_transition("sceKernelUnloadModule", modid, mod,
                                        SCE_ERROR_MODULE_ALREADY_LOADED);
    case MODULE_STATE_STARTING:
        return reject_module_transition("sceKernelUnloadModule", modid, mod,
                                        SCE_KERNEL_ERROR_MODULE_MGR_BUSY);
    case MODULE_STATE_STOPPING:
        return reject_module_transition("sceKernelUnloadModule", modid, mod,
                                        SCE_KERNEL_ERROR_ALREADY_STOPPING);
    default:
        return SCE_ERROR_MODULE_BAD_ID;
    }
    if (mod->image_base && mod->image_end > mod->image_base &&
        sched_module_range_busy(mod->image_base, mod->image_end, 0u)) {
        fprintf(stderr, "PRX_MODULE_UNLOAD_ACTIVE_THREAD_SYNTHETIC: uid=0x%x image="
                        "[0x%08x,0x%08x) has a ready/running continuation; refusing "
                        "with NOT_IMPLEMENTED (active-thread unload result unmeasured)\n",
                modid, mod->image_base, mod->image_end);
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    mod->state = MODULE_STATE_UNLOADED;
    if (mod->code) {
        fprintf(stderr, "GUEST_MODULE_UNLOADED: %s uid=0x%x image=[0x%08x,0x%08x)\n",
                mod->code->name, modid, mod->image_base,
                mod->image_base + (mod->code->link_high - mod->code->link_low));
    }
    /* The kernel unlinks a module's exports and releases its memory here, and this is
     * the only point at which a stopped module's export authority is retired. Stop
     * deliberately leaves the image resident: a stopped module is still loaded and can
     * be started again, so retiring its exports at stop would be an invented semantic. */
    if (mod->image_base && !another_record_references_image(mod)) {
        unload_prx_image(mod->image_base);
    }
    retire_loaded_module(mod);
    return 0;
}

static LoadedModule *module_for_guest_pc(const CpuState *s) {
    if (!s) return NULL;
    for (int i = 0; i < s_nloaded_modules; i++) {
        LoadedModule *mod = &s_loaded_modules[i];
        if (!mod->in_use || mod->state == MODULE_STATE_UNLOADED ||
            mod->image_end <= mod->image_base) continue;
        if ((s->pc >= mod->image_base && s->pc < mod->image_end) ||
            (s->r[31] >= mod->image_base && s->r[31] < mod->image_end))
            return mod;
    }
    return NULL;
}

/* PSPSDK declares the first argument unnamed. The successful homebrew path is
 * deliberately synthetic and accepts only zero there; other values stay behind
 * a named refusal until their meaning is measured. */
static uint32_t h_SelfStopUnloadModule(CpuState *s) {
    uint32_t unknown = A0;
    uint32_t arglen = A1;
    uint32_t argp = A2;
    LoadedModule *mod = module_for_guest_pc(s);
    uint32_t current_uid = sched_current_uid();
    if (unknown != 0u || !mod || current_uid == 0u || !mod->start_entry_ran ||
        mod->state != MODULE_STATE_STARTED) {
        fprintf(stderr, "PRX_SELFSTOP_UNAVAILABLE_SYNTHETIC: unknown=0x%x module=%s "
                        "thread=0x%x; refusing unsupported caller state\n",
                unknown, mod ? mod->path : "none", current_uid);
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    if (arglen != 0u && (!argp || !sr_guest_span_readable(argp, arglen)))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (mod->image_end > mod->image_base &&
        sched_module_range_busy(mod->image_base, mod->image_end, current_uid)) {
        fprintf(stderr, "PRX_SELFSTOP_SIBLING_THREAD_SYNTHETIC: uid=0x%x has another "
                        "ready/running continuation; refusing module-wide teardown\n",
                mod->uid);
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }

    uint32_t stop_status = 0u;
    if (mod->module_stop != 0u) {
        if (sr_lookup(mod->module_stop) == NULL)
            return SCE_KERNEL_ERROR_CAN_NOT_STOP;
        mod->state = MODULE_STATE_STOPPING;
        uint32_t rc = module_stop_thread_invoke(s, mod, arglen, argp, &stop_status);
        if (rc != 0u) {
            mod->state = MODULE_STATE_STARTED;
            return rc;
        }
    } else {
        fprintf(stderr, "PRX_SELFSTOP_NO_ENTRY_SYNTHETIC: uid=0x%x has no module_stop; "
                        "treating stop as a no-op\n", mod->uid);
    }
    mod->state = MODULE_STATE_STOPPED;
    s_deferred_self_stop_uid = mod->uid;
    s_deferred_self_stop_thread_uid = current_uid;
    /* The caller is executing from the image being retired. Delete its thread;
     * the scheduler exit hook releases the image only after this guest frame is
     * no longer runnable. The terminal result uses the module_stop result. */
    sched_exit_current_delete((int32_t)stop_status);
    return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
}

int loadexec_manifest_file_safe(const char *file) {
    if (!file || !file[0] || file[0] == '/' || file[0] == '\\' || strchr(file, ':'))
        return 0;
    const char *part = file;
    for (const char *p = file;; p++) {
        if (*p != '/' && *p != '\\' && *p != '\0') continue;
        size_t n = (size_t)(p - part);
        if (n == 0u || (n == 1u && part[0] == '.') ||
            (n == 2u && part[0] == '.' && part[1] == '.')) return 0;
        if (*p == '\0') break;
        part = p + 1;
    }
    return 1;
}

static uint32_t h_LoadExec(CpuState *s) {
    char guest_path[sizeof(s_loadexec_request.guest_path)];
    if (!A0 || !guest_cstr(A0, guest_path, sizeof(guest_path)))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    const char *file = NULL;
    int required = 0;
    SrGuestModulePlacement placement = SR_GUEST_MODULE_FIXED;
    if (!loadexec_ms0_path(guest_path) ||
        !sr_title_config_guest_module(guest_path, &file, NULL, &required, &placement) ||
        !required || placement != SR_GUEST_MODULE_RUNTIME ||
        !loadexec_manifest_file_safe(file)) {
        fprintf(stderr, "LOAD_EXEC_PACKAGE_PATH_REFUSED: sceKernelLoadExec path is not a "
                        "required runtime program in this package's declared ms0 namespace\n");
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    if (!loadexec_module_file_matches_translation(file)) {
        fprintf(stderr, "LOAD_EXEC_PROGRAM_SOURCE_REFUSED: package module is missing, "
                        "untranslated, or differs from the built module identity\n");
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    if (hle_loadexec_dialog_busy()) {
        fprintf(stderr, "LOAD_EXEC_DIALOG_ACTIVE_SYNTHETIC: refusing program replacement "
                        "while a utility dialog is active\n");
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    if (!sched_guest_thread_running()) {
        fprintf(stderr, "LOAD_EXEC_SCHEDULER_UNAVAILABLE: sceKernelLoadExec requires the "
                        "cooperative guest scheduler\n");
        return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    if (s_loadexec_pending) return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;

    uint32_t arglen = 0u;
    uint32_t argp = 0u;
    if (A1 != 0u) {
        if (!sr_guest_span_readable(A1, 12u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        uint32_t params_size = MEM_R32(A1);
        if (params_size < 12u) {
            fprintf(stderr, "LOAD_EXEC_PARAMETER_SIZE_SYNTHETIC: parameter block is "
                            "smaller than the documented arg fields; refusing\n");
            return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
        }
        arglen = MEM_R32(A1 + 4u);
        argp = MEM_R32(A1 + 8u);
        if (arglen > SR_LOAD_EXEC_MAX_ARGS) {
            fprintf(stderr, "LOAD_EXEC_ARGUMENT_LIMIT_SYNTHETIC: %u bytes exceeds the "
                            "bounded 64 KiB argument block\n", arglen);
            return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
        }
        if (arglen != 0u && (!argp || !sr_guest_span_readable(argp, arglen)))
            return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }

    /* A late program that is itself a loaded module gets its stop entry before
     * replacement. The primary package program has no late-module record; its
     * current thread is terminated below and the driver resets the process. */
    LoadedModule *current_module = module_for_guest_pc(s);
    if (current_module) {
        if (current_module->state != MODULE_STATE_STARTED) {
            fprintf(stderr, "LOAD_EXEC_CURRENT_MODULE_STATE_SYNTHETIC: module uid=0x%x "
                            "is not in a stable started state; refusing replacement\n",
                    current_module->uid);
            return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
        }
        if (current_module->module_stop != 0u) {
            if (sr_lookup(current_module->module_stop) == NULL)
                return SCE_KERNEL_ERROR_CAN_NOT_STOP;
            uint32_t stop_status = 0u;
            current_module->state = MODULE_STATE_STOPPING;
            uint32_t stop_rc = module_stop_thread_invoke(s, current_module, 0u, 0u,
                                                         &stop_status);
            if (stop_rc != 0u) {
                current_module->state = MODULE_STATE_STARTED;
                return stop_rc;
            }
            current_module->state = MODULE_STATE_STOPPED;
        } else {
            fprintf(stderr, "LOAD_EXEC_CURRENT_MODULE_NO_STOP_SYNTHETIC: uid=0x%x "
                            "has no module_stop entry; continuing replacement\n",
                    current_module->uid);
            current_module->state = MODULE_STATE_STOPPED;
        }
    }

    uint8_t *args = NULL;
    if (arglen != 0u) {
        args = (uint8_t *)malloc(arglen);
        if (!args) return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */
        for (uint32_t i = 0; i < arglen; i++) args[i] = MEM_R8(argp + i);
    }
    memset(&s_loadexec_request, 0, sizeof(s_loadexec_request));
    snprintf(s_loadexec_request.guest_path, sizeof(s_loadexec_request.guest_path), "%s",
             guest_path);
    s_loadexec_request.arglen = arglen;
    s_loadexec_request.args = args;
    s_loadexec_pending = 1;
    fprintf(stderr, "LOAD_EXEC_REQUEST: package program path accepted, arglen=%u; "
                    "stopping guest state before restart\n", arglen);
    sched_request_guest_reset();
    sched_exit_current_delete(0);
    return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
}
uint32_t s_exit_cb_uid;

static uint32_t h_RegisterExitCallback(CpuState *s) {
    uint32_t cb_uid = A0;
    if (!sr_callback_is_valid(cb_uid)) {
        /* PSP hardware changed validation in SDK 3.95. Before that, invalid callback
         * IDs were accepted with a success return; 3.95+ reports ILLEGAL_ARGUMENT. */
        return g_sdk_version >= 0x03090510u ? 0x800200D2u : 0u;
    }
    s_exit_cb_uid = cb_uid;
    if (getenv("SR_CBLOG"))
        fprintf(stderr, "CBLOG: registered external-exit callback uid=0x%x\n", cb_uid);
    return 0;
}
/* sceKernelStopUnloadSelfModuleWithStatus(status, ...): on hardware the kernel stops the
 * module's threads and reclaims its memory -- control NEVER returns to the caller. The
 * runtime loads exactly one game module and its threads are the only execution surface,
 * so full module-wide dissolution is not reproducible; the faithful-and-safe model is to
 * terminate the CALLING thread with the given status. That preserves the syscall's
 * terminal contract for the caller (returning 0 instead makes the guest panic
 * -- "something wrong" -- because it detects it is still alive after self-destruct),
 * while sibling threads keep running. Historical builds carried launcher/worker
 * keep-alive bypasses here ("silently succeed", "put the caller back to sleep"); those
 * are GONE -- the earlier comments describing them were stale. Behavior is identical for
 * every thread; no role/UID special cases. */
static uint32_t h_StopUnloadSelfModuleWithStatus(CpuState *s) {
    uint32_t uid = sched_current_uid();
    fprintf(stderr, "HLE: StopUnloadSelfModuleWithStatus uid=0x%x ra=0x%08x\n",
            uid, s->r[31]);
    fprintf(stderr, "TRACE_STOPUNLOADSELF: uid=0x%x pc=0x%08x ra=0x%08x ra2=0x%08x ra3=0x%08x\n",
            uid, s->pc, s->r[31],
            s->r[29] ? MEM_R32(s->r[29] + 0x0c) : 0,
            s->r[29] ? MEM_R32(s->r[29] + 0x10) : 0);
    fflush(stderr);
    /* Module self-unload is a separate lifecycle contract.  Its status has not
     * been covered by the ThreadMan exit oracle, so preserve the caller value
     * rather than routing it through the measured ExitThread normalization. */
    sched_exit_current_unchecked((int32_t)A0);
    return 0;   /* unreachable in the calling thread: the raw exit switches away */
}

/* UTF-8 path of the file NAME beside this executable, for the ExitGame diagnostics.
 * Returns 0 and leaves *out NULL when the executable's folder cannot be resolved. */
static int sr_exit_artifact_path_alloc(const wchar_t *name, char **out) {
    wchar_t *dir = NULL, *joined = NULL;
    *out = NULL;
    int ok = sr_wide_module_dir_alloc(&dir) && sr_wide_join_alloc(dir, name, &joined) &&
             sr_wide_to_utf8_alloc(joined, out);
    free(dir);
    free(joined);
    return ok;
}
static int hle_loadexec_dialog_busy(void) {
    dialog_lock();
    int busy = s_dlg_status != 0 || s_utility_slot == UTILITY_SLOT_OSK;
    dialog_unlock();
    return busy || s_osk_status != OSK_NONE;
}

/* sceKernelLoadModule(path, flags, option). The PSP's module manager opens the path with
 * sceIoOpen in the caller's context (uofw src/kd/modulemgr/loadModule.c), so a relative
 * path names the file under the calling thread's current directory, as every file-manager
 * path does. The module is then the one the manifest declares by that full path: a title
 * that changes into its data directory and loads "Target/libfont.prx" loads the declared,
 * translated libfont, not an undeclared host-served record with no start entry (#299). */
static uint32_t h_LoadModule(CpuState *s) {
    char resolved[512];
    uint32_t rc = io_guest_path(A0, resolved, sizeof(resolved));
    /* An unreadable path string is the module manager's illegal address, not an I/O error. */
    if (rc == 0x80010016u) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (rc != 0u) return rc;
    return load_module_by_guest_path(resolved, A2);
}

static const char *hle_fd_guest_path(uint32_t fd);

/* sceKernelLoadModuleByID(fd, flags, option). The file was opened by path, and that path
 * names the module: it loads exactly as sceKernelLoadModule loads the same path, with a
 * module record its start/stop/unload can find -- guest-placed, fixed-address, or
 * (undeclared) host-served. A descriptor that names no recorded file (never opened, closed,
 * not a file, or a path too long to record) names no module, so no record can back a uid
 * for it. It refuses by name before any load. The old fallback populated every
 * fixed-address manifest module under a bare uid that sceKernelStartModule rejected with
 * the generic bad id, and it left those images and exports with no owner to unload (#280). */
static uint32_t h_LoadModuleByID(CpuState *s) {
    const char *opened = hle_fd_guest_path(A0);
    if (opened) return load_module_by_guest_path(opened, A2);
    fprintf(stderr, "GUEST_MODULE_BOUNDARY: guest-module-descriptor-unrecorded fd=0x%x: no "
                    "recorded file names this descriptor, so no module record can back a uid; "
                    "refusing before any load\n", A0);
    return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;  /* named refusal; the PSP return is unmeasured */
}

static uint32_t h_StartModule(CpuState *s) {
    uint32_t uid = A0;
    uint32_t arglen = A1;
    uint32_t argp = A2;
    uint32_t status_ptr = A3;
    fprintf(stderr, "sceKernelStartModule(uid=0x%x, arglen=%u, argp=0x%x)\n", uid, arglen, argp);

    LoadedModule *mod = find_loaded_module(uid);
    if (!mod) {
        fprintf(stderr, "sceKernelStartModule(uid=0x%x) -> unknown or unloaded module\n", uid);
        return SCE_ERROR_MODULE_BAD_ID; /* unmeasured */
    }
    const char *path = mod->path;
    switch (mod->state) {
    case MODULE_STATE_LOADED:
        break;
    case MODULE_STATE_STOPPED:
        /* Inference, not hardware-measured: a stopped module is restartable. */
        break;
    case MODULE_STATE_STARTED:
        /* A start the host skipped (startup unavailable, or the module is host-HLE'd)
         * ran no guest code, and titles retry an unavailable startup: take the same
         * nonfatal host-skip path again rather than refusing it. */
        if (!mod->start_entry_ran) break;
        /* One module_start per live module record: a repeated StartModule would re-run
         * the guest entry (and its one-shot init side effects) over an initialised module. */
        fprintf(stderr, "sceKernelStartModule(uid=0x%x, path='%s') -> already started, "
                        "refusing re-entry\n", uid, mod->path);
        return reject_module_transition("sceKernelStartModule", uid, mod,
                                        SCE_KERNEL_ERROR_ALREADY_STARTED);
    case MODULE_STATE_STARTING:
        return reject_module_transition("sceKernelStartModule", uid, mod,
                                        SCE_KERNEL_ERROR_MODULE_MGR_BUSY);
    case MODULE_STATE_STOPPING:
        return reject_module_transition("sceKernelStartModule", uid, mod,
                                        SCE_KERNEL_ERROR_ALREADY_STOPPING);
    default:
        return SCE_ERROR_MODULE_BAD_ID;
    }
    LoadedModuleState previous_state = mod->state;
    mod->start_entry_ran = 0;

    int is_libfont = module_path_is(path, "libfont.prx");
    ModuleStartPolicy policy = module_start_policy();
    /* A guest-placed module is the game's own code laid out by the guest allocator: its
     * module_start runs by default, as libfont's does, unless host HLE implements one of
     * its exports -- then the host keeps serving them and the guest init is bypassed
     * like every other host-served module's. An export of a library the guest owns
     * (guest_module_owns_library) never makes its module host-served. */
    int guest_owned_start = is_libfont || (mod->runtime_placed && mod->code && !mod->host_served);
    int allow_entry = policy == MODULE_START_ALLOW_ALL ||
                      (policy == MODULE_START_LIBFONT_ONLY && guest_owned_start);
    /* A libfont module under the kill switch does not return here: startup is
     * unavailable, so it reaches the fail-closed startup boundary below. */
    int refuse_unavailable_libfont = policy == MODULE_START_NONE && is_libfont;

    /* A module whose exports host HLE implements (the PSMF and PSMF-player libraries, for
     * example) keeps the host bypass: its real module_start is SDK init code that assumes a
     * real PSP kernel underneath and hangs on an unconditional WaitSema. What the record knows
     * of a module is its export table (host_served), never a substring of its path, so a
     * look-alike file name changes nothing (#1385). */
    if (!allow_entry && !refuse_unavailable_libfont) {
        if (policy == MODULE_START_NONE) {
            fprintf(stderr, "sceKernelStartModule(uid=0x%x, path='%s') -> "
                            "SR_REAL_MODULE_START=0, module_start not executed\n",
                    uid, path ? path : "");
        } else if (mod->runtime_placed) {
            fprintf(stderr, "sceKernelStartModule(uid=0x%x, path='%s') -> host HLE serves "
                            "this module's exports; module_start not executed\n", uid, path);
        } else if (mod->host_served) {
            fprintf(stderr, "sceKernelStartModule(uid=0x%x, path='%s') -> host HLE serves this "
                            "module's exports; module_start not executed\n", uid, path ? path : "");
        } else {
            fprintf(stderr, "sceKernelStartModule(uid=0x%x) -> unknown module path, skipping entry\n", uid);
        }
        mod->state = MODULE_STATE_STARTED;
        return 0;
    }

    if (allow_entry && mod->runtime_placed && mod->code && mod->module_start == 0) {
        /* A guest-placed module that declares no start routine starts by making its
         * exports linkable; no guest code runs. Its stop entry, if any, runs at stop. */
        fprintf(stderr, "sceKernelStartModule(uid=0x%x, path='%s') -> module declares no "
                        "start routine; exports linked\n", uid, mod->path);
        mark_module_started(mod->image_base);
        mod->start_entry_ran = 1;
        mod->state = MODULE_STATE_STARTED;
        return 0;
    }

    if (allow_entry && mod->runtime_placed && mod->code &&
        sr_lookup(mod->module_start) == NULL) {
        /* The translation was bound, so an entry it has no body for is a build gap, not
         * a module without startup: refuse it rather than report a start that never ran. */
        char detail[96];
        snprintf(detail, sizeof(detail), "module_start 0x%08x has no translated body",
                 mod->module_start);
        mod->state = previous_state;
        uint32_t refused = guest_module_boundary("guest-module-start-untranslated",
                                                 mod->code->name, detail);
        return is_libfont ? hle_libfont_startup_stop("entry untranslated") : refused;
    }

    if (allow_entry && mod->module_start != 0 && sr_lookup(mod->module_start) != NULL) {
        mod->state = MODULE_STATE_STARTING;
        fprintf(stderr, "sceKernelStartModule(uid=0x%x, path='%s', entry=0x%08x): executing module_start\n",
                uid, mod->path, mod->module_start);
        uint32_t rv = module_entry_call(s, mod, mod->module_start, arglen, argp);
        mark_module_started(mod->module_start);
        if (status_ptr && sr_guest_span_writable(status_ptr, 4u)) {
            MEM_W32(status_ptr, rv);
        }
        mod->start_entry_ran = 1;
        mod->state = MODULE_STATE_STARTED;
        return 0;
    }

    /* Guest startup is unavailable: the entry is not translated, the image recorded none,
     * or the operator disabled module_start. Libfont readiness belongs to guest code and is
     * never synthesized, so the run stops by name (hle_libfont_startup_stop); the module
     * keeps its previous state for a harness that observes the refusal. */
    if (is_libfont) {
        const char *reason = !allow_entry ? "SR_REAL_MODULE_START=0 disabled guest startup"
                         : (mod->module_start == 0) ? "no module_start entry recorded"
                                                    : "entry untranslated";
        mod->state = previous_state;
        return hle_libfont_startup_stop(reason);
    }

    static int s_logged_untranslated = 0;
    if (!is_libfont && !s_logged_untranslated) {
        s_logged_untranslated = 1;
        if (mod->host_served) {
            fprintf(stderr, "sceKernelStartModule(uid=0x%x, path='%s') -> host HLE serves this "
                            "module's exports; module_start not executed\n", uid, mod->path);
        } else {
            fprintf(stderr, "sceKernelStartModule(uid=0x%x) -> unknown or untranslated entry, skipping\n", uid);
        }
    }
    mod->state = MODULE_STATE_STARTED;
    return 0;
}
#ifdef SR_HLE_THREAD_SELFTEST

/* Test-build-only call-throughs to the production handlers. */
uint32_t sr_hle_test_load_module(CpuState *s) { return h_LoadModule(s); }
uint32_t sr_hle_test_load_module_by_id(CpuState *s) { return h_LoadModuleByID(s); }
uint32_t sr_hle_test_start_module(CpuState *s) { return h_StartModule(s); }
uint32_t sr_hle_test_unload_module(CpuState *s) { return h_UnloadModule_Trace(s); }
#endif
static const char *hle_fd_guest_path(uint32_t fd) {
    if (fd >= (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) || !s_fds[fd].used ||
        s_fds[fd].kind != FD_KIND_FILE || !s_fds[fd].guest_path[0])
        return NULL;
    return s_fds[fd].guest_path;
}

void hle_register_exit_game_handler(void) {
    sr_hle_register(0x05572a5f, "sceKernelExitGame", h_ExitGame);
}

int sr_hle_loadexec_take(SrLoadExecRequest *out) {
    if (!out || !s_loadexec_pending) return 0;
    *out = s_loadexec_request;
    memset(&s_loadexec_request, 0, sizeof(s_loadexec_request));
    s_loadexec_pending = 0;
    return 1;
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_kernel_import_sweep_handlers(). */
void hle_register_kernel_import_sweep_kernel_module(void) {
    sr_hle_register(0xbd2f1094, "sceKernelLoadExec", h_LoadExec);
}

/* From hle_register_kernel_module_handlers(). */
void hle_register_module_lifecycle_handlers(void) {
    sr_hle_register_unsupported(0xf9275d98u, "sceKernelLoadModuleBufferUsbWlan", 0x80020002u);
    sr_hle_register(0x977de386, "sceKernelLoadModule", h_LoadModule);
    sr_hle_register(0xb7f46618, "sceKernelLoadModuleByID", h_LoadModuleByID);
    sr_hle_register(0x50f0c1ec, "sceKernelStartModule", h_StartModule);
    sr_hle_register(0xf0a26395, "sceKernelGetModuleId", h_GetModuleId);
    sr_hle_register(0xd8b73127, "sceKernelGetModuleIdByAddress", h_GetModuleIdByAddress);
    sr_hle_register(0x748cbed9, "sceKernelQueryModuleInfo", h_QueryModuleInfo);
    sr_hle_register(0xd1ff982a, "sceKernelStopModule", h_StopModule_Trace);
    sr_hle_register(0x2e0911aa, "sceKernelUnloadModule", h_UnloadModule_Trace);
    sr_hle_register(0x8f2df740, "sceKernelStopUnloadSelfModuleWithStatus", h_StopUnloadSelfModuleWithStatus);
    sr_hle_register(0xd675ebb8, "sceKernelSelfStopUnloadModule", h_SelfStopUnloadModule);
}

/* From hle_register_selftest_oracle_handlers(). */
void hle_register_selftest_oracle_kernel_module(void) {
    sr_hle_register(0x4ac57943, "sceKernelRegisterExitCallback", h_RegisterExitCallback);
}
