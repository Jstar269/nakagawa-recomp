// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

/* *
 * Loads a statically-linked PSP ELF into guest memory, seeds the CpuState from the entry
 * register state the PPSSPP reference trace recorded ("# init" line in the trace), registers
 * every generated function, and runs the entry function with tracing on. Execution stops
 * cleanly at the first HLE call (sr_hle_call longjmps), exactly where the reference trace is
 * truncated for comparison.
 *
 * Usage: driver <elf> <ref-trace> <out-trace>
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "debug.h"
#include "title_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

void sr_register_all(void);

#ifndef SR_SELFTEST_ONLY
#ifdef _WIN32
/* Last-resort crash reporter: a silent host fault (access violation etc.) otherwise kills
 * the process with nothing in the log. Maps a faulting address inside the guest arena back
 * to its guest address. Dumps register state and stack trace for post-mortem analysis. */
static LONG WINAPI sr_crash_filter(EXCEPTION_POINTERS *ep) {
    CONTEXT *ctx = ep->ContextRecord;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;

    fprintf(stderr, "\n");
    fprintf(stderr, "=== PSP RECOMPILER CRASH REPORT ===\n");
    fprintf(stderr, "Exception: 0x%08lx at host %p\n", er->ExceptionCode, er->ExceptionAddress);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const uint8_t *fa = (const uint8_t *)er->ExceptionInformation[1];
        int is_write = er->ExceptionInformation[0];
        fprintf(stderr, "Fault: %s of host %p", is_write ? "WRITE" : "READ", fa);
        if (g_mem && fa >= g_mem - 0x04000000 && fa < g_mem + 0x04000000) {
            uint32_t guest_addr = (uint32_t)(fa - g_mem) + 0x08000000u;
            fprintf(stderr, " -> guest 0x%08x", guest_addr);
            // Check if address is in a known memory watch
            for (int i = 0; i < g_sr_mem_watch_count; i++) {
                if (guest_addr >= g_sr_mem_watches[i].start && guest_addr < g_sr_mem_watches[i].end) {
                    fprintf(stderr, " [WATCHED: %s]", g_sr_mem_watches[i].label);
                }
            }
        }
        fprintf(stderr, "\n");
    }

    // Dump x64 register state (Windows CONTEXT)
    fprintf(stderr, "\n--- Host Registers ---\n");
    fprintf(stderr, "RIP=0x%016llx  RSP=0x%016llx\n", (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp);
    fprintf(stderr, "RAX=0x%016llx  RBX=0x%016llx\n", (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx);
    fprintf(stderr, "RCX=0x%016llx  RDX=0x%016llx\n", (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
    fprintf(stderr, "RSI=0x%016llx  RDI=0x%016llx\n", (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi);
    fprintf(stderr, "RBP=0x%016llx  R8 =0x%016llx\n", (unsigned long long)ctx->Rbp, (unsigned long long)ctx->R8);
    fprintf(stderr, "R9 =0x%016llx  R10=0x%016llx\n", (unsigned long long)ctx->R9, (unsigned long long)ctx->R10);
    fprintf(stderr, "R11=0x%016llx  R12=0x%016llx\n", (unsigned long long)ctx->R11, (unsigned long long)ctx->R12);
    fprintf(stderr, "R13=0x%016llx  R14=0x%016llx\n", (unsigned long long)ctx->R13, (unsigned long long)ctx->R14);
    fprintf(stderr, "R15=0x%016llx\n", (unsigned long long)ctx->R15);

    // Dump guest CpuState if available
    extern CpuState *s_cpu;
    if (s_cpu) {
        fprintf(stderr, "\n--- Guest CpuState ---\n");
        fprintf(stderr, "PC=0x%08x  SP(r29)=0x%08x  RA(r31)=0x%08x\n", s_cpu->pc, s_cpu->r[29], s_cpu->r[31]);
        fprintf(stderr, "r4=0x%08x  r5=0x%08x  r6=0x%08x  r7=0x%08x\n", s_cpu->r[4], s_cpu->r[5], s_cpu->r[6], s_cpu->r[7]);
        fprintf(stderr, "r8=0x%08x  r9=0x%08x  r10=0x%08x r11=0x%08x\n", s_cpu->r[8], s_cpu->r[9], s_cpu->r[10], s_cpu->r[11]);
        fprintf(stderr, "r12=0x%08x r13=0x%08x r14=0x%08x r15=0x%08x\n", s_cpu->r[12], s_cpu->r[13], s_cpu->r[14], s_cpu->r[15]);
        fprintf(stderr, "r16=0x%08x r17=0x%08x r18=0x%08x r19=0x%08x\n", s_cpu->r[16], s_cpu->r[17], s_cpu->r[18], s_cpu->r[19]);
        fprintf(stderr, "r20=0x%08x r21=0x%08x r22=0x%08x r23=0x%08x\n", s_cpu->r[20], s_cpu->r[21], s_cpu->r[22], s_cpu->r[23]);
        fprintf(stderr, "r24=0x%08x r25=0x%08x r26=0x%08x r27=0x%08x\n", s_cpu->r[24], s_cpu->r[25], s_cpu->r[26], s_cpu->r[27]);
        fprintf(stderr, "r28=0x%08x r30=0x%08x\n", s_cpu->r[28], s_cpu->r[30]);
        fprintf(stderr, "hi=0x%08x  lo=0x%08x  fcr31=0x%08x\n", s_cpu->hi, s_cpu->lo, s_cpu->fcr31);
    }

    fprintf(stderr, "\n=== END CRASH REPORT ===\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;   /* still crash (after reporting) */
}
#endif
#endif

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static inline int range_within(size_t total, uint64_t offset, uint64_t length) {
    return offset <= total && length <= (uint64_t)total - offset;
}

static int sr_redirect_quiet_stream(FILE *stream, const char *path, const char *name) {
    if (freopen(path, "w", stream) != NULL) return 1;
    /* Quiet mode is best effort.  Make a failed redirect explicit while leaving
     * the caller's stream available for the normal driver diagnostics. */
    fprintf(stderr, "warning: SR_QUIET could not redirect %s to %s\n", name, path);
    return 0;
}

static uint8_t *read_file(const char *path, size_t *out_len) {
    if (!path || !out_len) { fprintf(stderr, "invalid read_file params\n"); exit(2); }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    if (fseek(f, 0, SEEK_END) != 0) { fprintf(stderr, "cannot seek %s\n", path); fclose(f); exit(2); }
    long n = ftell(f);
    if (n < 0) { fprintf(stderr, "cannot size %s\n", path); fclose(f); exit(2); }
    if (fseek(f, 0, SEEK_SET) != 0) { fprintf(stderr, "cannot rewind %s\n", path); fclose(f); exit(2); }
    size_t size = (size_t)n;
    if ((long)size != n) { fprintf(stderr, "file too large %s\n", path); fclose(f); exit(2); }
    uint8_t *d = (uint8_t *)malloc(size ? size : 1);
    if (!d) { fprintf(stderr, "allocation failure for %s\n", path); fclose(f); exit(2); }
    if (size > 0 && fread(d, 1, size, f) != size) {
        fprintf(stderr, "short read %s\n", path);
        free(d);
        fclose(f);
        exit(2);
    }
    fclose(f);
    *out_len = size;
    return d;
}

static uint32_t load_elf(const uint8_t *elf, size_t len) {
    if (!elf || len < 52 || memcmp(elf, "\x7f""ELF", 4) != 0) {
        fprintf(stderr, "not an ELF or truncated header\n");
        exit(2);
    }
    uint32_t e_entry = rd32(elf + 24);
    uint32_t e_phoff = rd32(elf + 28);
    uint16_t phentsize = rd16(elf + 42);
    uint16_t phnum = rd16(elf + 44);

    if (phnum != 0 && phentsize < 32) {
        fprintf(stderr, "ELF program header entry too small\n");
        exit(2);
    }
    uint32_t ph_table_size = 0;
    if (!sr_size_mul_ok(phentsize, phnum, &ph_table_size)) {
        fprintf(stderr, "ELF program header table size overflow\n");
        exit(2);
    }
    if (!range_within(len, e_phoff, ph_table_size)) {
        fprintf(stderr, "ELF program header table out of range\n");
        exit(2);
    }

    /* Pass 1: Validate all PT_LOAD segments before mutating guest RAM */
    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t ph_offset = (uint64_t)e_phoff + (uint64_t)i * phentsize;
        const uint8_t *ph = elf + ph_offset;
        uint32_t p_type = rd32(ph + 0);
        uint32_t p_off = rd32(ph + 4);
        uint32_t p_vaddr = rd32(ph + 8);
        uint32_t p_filesz = rd32(ph + 16);
        uint32_t p_memsz = rd32(ph + 20);

        if (p_type != 1) /* PT_LOAD */
            continue;

        if (p_filesz > p_memsz) {
            fprintf(stderr, "ELF PT_LOAD filesz exceeds memsz\n");
            exit(2);
        }
        if (!range_within(len, p_off, p_filesz)) {
            fprintf(stderr, "ELF PT_LOAD data out of range\n");
            exit(2);
        }
        if (p_memsz > 0 && !sr_guest_span_writable(p_vaddr, p_memsz)) {
            fprintf(stderr, "ELF PT_LOAD guest range invalid: vaddr=0x%08x memsz=0x%08x\n", p_vaddr, p_memsz);
            exit(2);
        }
    }

    /* Pass 2: Load validated segments */
    sr_mem_init();
    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t ph_offset = (uint64_t)e_phoff + (uint64_t)i * phentsize;
        const uint8_t *ph = elf + ph_offset;
        uint32_t p_type = rd32(ph + 0);
        uint32_t p_off = rd32(ph + 4);
        uint32_t p_vaddr = rd32(ph + 8);
        uint32_t p_filesz = rd32(ph + 16);
        if (p_type == 1 && p_filesz) /* PT_LOAD */
            sr_load_segment(p_vaddr, elf + p_off, p_filesz);
    }
    return e_entry;
}

static int parse_indexed_register(const char *name, char prefix, uint32_t *index) {
    if (!name || !index || name[0] != prefix) return 0;
    if (name[1] < '0' || name[1] > '9') return 0;
    char *end = NULL;
    errno = 0;
    unsigned long long val = strtoull(name + 1, &end, 10);
    if (errno != 0 || !end || end == name + 1 || *end != '\0' || val > 31ULL) return 0;
    *index = (uint32_t)val;
    return 1;
}

static int parse_hex32(const char *text, uint32_t *value) {
    if (!text || !value || *text == '\0' || *text == '-' || *text == '+' || *text == ' ' || *text == '\t') return 0;
    const char *start = text;
    if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
        start += 2;
    }
    if (!((*start >= '0' && *start <= '9') || (*start >= 'a' && *start <= 'f') || (*start >= 'A' && *start <= 'F'))) {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(start, &end, 16);
    if (errno != 0 || !end || end == start || *end != '\0' || parsed > 0xFFFFFFFFULL) return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static void seed_from_init(const char *trace_path, CpuState *s) {
    if (strcmp(trace_path, "none") == 0) return;
    FILE *f = fopen(trace_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", trace_path); exit(2); }
    char buf[8192];
    int found = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "# init", 6) != 0)
            continue;
        found = 1;
        char *tok = strtok(buf + 6, " \t\n");
        while (tok) {
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = 0;
                const char *name = tok;
                const char *val_str = eq + 1;
                uint32_t val = 0, idx = 0;
                if (parse_hex32(val_str, &val)) {
                    if (parse_indexed_register(name, 'r', &idx)) s->r[idx] = val;
                    else if (parse_indexed_register(name, 'f', &idx)) s->fi[idx] = val;
                    else if (strcmp(name, "hi") == 0) s->hi = val;
                    else if (strcmp(name, "lo") == 0) s->lo = val;
                    else if (strcmp(name, "fcr31") == 0) s->fcr31 = val;
                }
            }
            tok = strtok(NULL, " \t\n");
        }
        break;
    }
    fclose(f);
    if (!found) { fprintf(stderr, "no '# init' in %s\n", trace_path); exit(2); }
}

#ifdef SR_SELFTEST_ONLY
int driver_main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    uint32_t entry;
    const char *ref_trace, *out;

#ifndef SR_SELFTEST_ONLY
#ifdef _WIN32
    SetUnhandledExceptionFilter(sr_crash_filter);
#endif
#endif

    /* Performance is an explicit visual/audio smoke-test mode. Redirecting both C
     * streams to the null device removes terminal/file I/O without scattering a
     * behavior-changing quiet check through every compatibility diagnostic. */
    {
        const char *quiet = getenv("SR_QUIET");
        if (quiet && quiet[0] && strcmp(quiet, "0") != 0) {
#ifdef _WIN32
            sr_redirect_quiet_stream(stdout, "NUL", "stdout");
            sr_redirect_quiet_stream(stderr, "NUL", "stderr");
#else
            sr_redirect_quiet_stream(stdout, "/dev/null", "stdout");
            sr_redirect_quiet_stream(stderr, "/dev/null", "stderr");
#endif
        }
    }

    /* Initialize debug framework from SR_DEBUG env var (or legacy SR_* vars) */
    g_sr_debug = sr_debug_init();
    sr_debug_init_watches();
    sr_perf_init();
    sr_profile_init();

    if (g_sr_debug) {
        fprintf(stderr, "Debug: SR_DEBUG=0x%02x (", g_sr_debug);
        if (g_sr_debug & SR_DBG_MEM)   fprintf(stderr, "MEM ");
        if (g_sr_debug & SR_DBG_HLE)   fprintf(stderr, "HLE ");
        if (g_sr_debug & SR_DBG_SCHED) fprintf(stderr, "SCHED ");
        if (g_sr_debug & SR_DBG_GE)    fprintf(stderr, "GE ");
        if (g_sr_debug & SR_DBG_INPUT) fprintf(stderr, "INPUT ");
        if (g_sr_debug & SR_DBG_FS)    fprintf(stderr, "FS ");
        if (g_sr_debug & SR_DBG_VIDEO) fprintf(stderr, "VIDEO ");
        if (g_sr_debug & SR_DBG_MISC)  fprintf(stderr, "MISC ");
        fprintf(stderr, ")\n");
    }

#ifdef SR_PUBLIC_SAFE
    fprintf(stderr, "BOOT_EVENT phase=init public_safe=1\n");
#else
    fprintf(stderr, "BOOT_EVENT phase=init public_safe=0\n");
#endif

    /* Image mode: a pre-relocated flat image (e.g. a rebased PRX from tools/prxload.py) is
     * loaded at <base> and run from <entry>. Required for relocatable PRXs, which must be
     * rebased + relocated before they have concrete addresses.
     *   driver --image <image.bin> <base-hex> <entry-hex> <ref-trace> <out-trace>  */
    if (argc >= 7 && strcmp(argv[1], "--image") == 0) {
        size_t len = 0;
        uint8_t *img = read_file(argv[2], &len);
        uint32_t base = 0;
        if (!parse_hex32(argv[3], &base) || !parse_hex32(argv[4], &entry)) {
            fprintf(stderr, "invalid base or entry address: base=%s entry=%s\n", argv[3], argv[4]);
            free(img);
            exit(2);
        }
        ref_trace = argv[5];
        out = argv[6];
        if (len > 0xFFFFFFFFu || !sr_guest_span_writable(base, (uint32_t)len)) {
            fprintf(stderr, "image guest span invalid: base=0x%08x len=0x%zx\n", base, (size_t)len);
            free(img);
            exit(2);
        }
        sr_mem_init();
        sr_load_segment(base, img, (uint32_t)len);
        goto have_image;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: driver <elf> <ref-trace> <out-trace>\n"
                        "       driver --image <image.bin> <base-hex> <entry-hex> <ref-trace> <out-trace>\n");
        return 2;
    }
    {
        size_t len = 0;
        uint8_t *elf = read_file(argv[1], &len);
        entry = load_elf(elf, len);
    }
    ref_trace = argv[2];
    out = argv[3];
have_image:;
    fprintf(stderr, "BOOT_EVENT phase=image_loaded entry=0x%08x\n", entry);

    CpuState s;
    memset(&s, 0, sizeof(s));
    s.vfpuCtrl[0] = 0xe4;  /* SPREFIX = identity */
    s.vfpuCtrl[1] = 0xe4;  /* TPREFIX = identity */
    s.vfpuCtrl[2] = 0;     /* DPREFIX = none */
    seed_from_init(ref_trace, &s);
    if (s.r[29] == 0) s.r[29] = 0x09F00000;   /* default stack: top of user RAM (0x08000000..0x0BFFFFFF); was 0x00400000 (kernel driver region — wrong) */
    s.pc = entry;

    fprintf(stderr, "Calling sr_register_all()...\n");
    sr_register_all();
    fprintf(stderr, "sr_register_all() returned\n");
    fprintf(stderr, "BOOT_EVENT phase=runtime_registered entry=0x%08x\n", entry);
    /* "none" as the out path disables tracing -- much faster for HLE bring-up, where the goal
     * is to reach the next unimplemented import rather than diff a trace. */
    if (strcmp(out, "none") != 0 && sr_trace_open(out, "cgtest", entry) != 0) {
        fprintf(stderr, "cannot open out trace %s\n", out);
        return 2;
    }
    RecompFn fn = sr_lookup(entry);
    if (!fn) {
        /* Entry point not compiled — the codegen likely skipped it (e.g. a module-start
         * wrapper whose first instruction is a syscall the analyzer treats as a HLE boundary).
         * A title configuration may name that module-start address; with no configured
         * fallback there is nothing generic to try, so the existing failure path stands. */
        uint32_t fallback = sr_title_config_fallback_entry();
        if (!fallback) {
            fprintf(stderr, "no recompiled function at entry 0x%08x "
                            "(no title fallback entry is configured)\n", entry);
            return 2;
        }
        fprintf(stderr, "entry 0x%08x not compiled, trying configured fallback 0x%08x\n", entry, fallback);
        fn = sr_lookup(fallback);
        if (!fn) { fprintf(stderr, "no recompiled function at entry 0x%08x (fallback also missing)\n", entry); return 2; }
        entry = fallback;
        s.pc = entry;
    }

    int use_sched = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--sched") == 0) use_sched = 1;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--gui") == 0) { gui_init(SR_APP_TITLE); use_sched = 1; }

    if (use_sched) {
        /* Run with the cooperative scheduler so the game's threads interleave (the boot busy-
         * waits on a sibling). sched_run uses s as the live register file for whichever thread
         * runs; it returns when no thread is runnable, or the process exits at an unimplemented
         * import inside a thread fiber. */
        fprintf(stderr, "BOOT_EVENT phase=guest_start mode=scheduler entry=0x%08x\n", entry);
        sched_init(&s);
        sched_run(entry, s.r[4], s.r[5]);
    } else {
        fn(&s);
    }
    sr_trace_close();

    const char *ppm_path = getenv("SR_PPM_DUMP");
    if (ppm_path) {
        uint32_t fb_addr = 0x09000000;
        FILE *pf = fopen(ppm_path, "wb");
        if (pf) {
            fprintf(pf, "P6\n64 64\n255\n");
            for (int i = 0; i < 64 * 64; i++) {
                uint32_t pixel = MEM_R32(fb_addr + i * 4);
                uint8_t r = pixel & 0xFF;
                uint8_t g = (pixel >> 8) & 0xFF;
                uint8_t b = (pixel >> 16) & 0xFF;
                fputc(r, pf);
                fputc(g, pf);
                fputc(b, pf);
            }
            fclose(pf);
            fprintf(stderr, "[driver] Framebuffer PPM dumped to %s\n", ppm_path);
        }
    }

    fprintf(stderr, "done (hit_hle=%d)\n", sr_hit_hle);

    return 0;
}
