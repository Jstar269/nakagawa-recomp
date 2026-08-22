// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
//
/*
 * The codegen emits one C function per guest function with signature void f_<hexaddr>(CpuState*).
 * Those functions read and write this CpuState and access guest memory through the macros
 * below. Computed transfers go through dispatch(). When tracing is enabled (a trace file is
 * set), each translated instruction reports itself so the output can be diffed against the
 * PPSSPP reference trace (tools/TRACE_FORMAT.md).
 */

#ifndef PSP_RECOMP_RT_H
#define PSP_RECOMP_RT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "fp_convert.h"
#ifdef __cplusplus
#include <atomic>
typedef std::atomic_int_least32_t atomic_int_least32_t;
#else
#include <stdatomic.h>
#endif

/* Debug framework — included early so sr_w32() can call sr_check_mem_watch(). */
#include "debug.h"
#include "perf.h"

typedef struct CpuState {
    uint32_t r[32];     /* r[0] reads 0; the codegen never emits a write to r[0]. */
    uint32_t hi, lo;
    /* NOT the current instruction. In the recompiled runtime nothing tracks the program
     * counter per instruction: sr_begin_impl() records the pc into its own static trace
     * slot, never into this field. Only three things write here -- SR_YIELD when the
     * timeslice expires (storing the yield target), dispatch()'s no-target path, and a
     * handful of custom stubs. So this holds the last *preemption* point, which can be
     * an entire call tree away from where a fault is observed. Diagnostics that print it
     * must label it as such (`last_yield_pc=`), never as the faulting pc.
     * For the exact call site of the jal/jalr currently executing, use `r[31] - 8`: the
     * generated code assigns ra = call+8 at the call instruction itself. (The reference
     * interpreter in src/ref/cpu.h is different -- there pc IS exact.) */
    uint32_t pc;
    union {
        float f[32];
        uint32_t fi[32];
    };
    uint32_t fcr31;
    uint32_t fpcond;    /* FP compare result, separate from fcr31 to match PPSSPP */
    union {
        float v[128];   /* VFPU register file; physical order matches PPSSPP's v[128] */
        uint32_t vi[128];
    };
    uint32_t vfpuCtrl[16];  /* VFPU control: prefixes (S/T/D), cc, etc. */
    uint32_t status;        /* COP0 status register */
    uint32_t next_pc;       /* Branch/delay-slot bookkeeping for reference parity */
    uint32_t in_delay_slot;  /* Parity with ref::CpuState */
} CpuState;

/* Guest memory: a single host region. g_mem points at guest 0x08000000, and the underlying
 * allocation also extends 0x04000000 bytes *below* g_mem so the same arena covers VRAM/eDRAM
 * (0x04000000..0x041fffff). SR_HOST maps a VRAM address to g_mem minus an offset that still
 * lands inside the allocation. Keeping SR_RAM_BASE at 0x08000000 means the recompiled code's
 * MEM_* macros are unchanged (no rebuild of the big object). */
extern uint8_t *g_mem;
#define SR_RAM_BASE 0x08000000u
#define SR_PHYS(a)  ((a) & 0x1FFFFFFFu)
/* Signed offset so addresses below SR_RAM_BASE map below g_mem.
 * With SR_RAM_BASE=0x08000000, RAM starts at g_mem and VRAM/eDRAM at 0x04000000 is at g_mem - 0x04000000. */
#define SR_HOST(a)  (g_mem + (int32_t)(SR_PHYS(a) - SR_RAM_BASE))

/* The arena covers guest physical [0, 0x0c000000). */
static inline int sr_inrange(uint32_t a) {
    return (uint32_t)SR_PHYS(a) < 0x0c000000u;
}
/* Width-aware variant for multi-byte accesses. sr_inrange(a) alone only checks the
 * FIRST byte, so e.g. a 4-byte read starting at phys 0x0bfffffe would pass it while
 * its last byte lands past the arena's actual end (past the calloc'd allocation).
 * Used by the r16/r32/w16/w32 accessors below; sr_inrange(a) (width 1) is unchanged
 * for sr_r8/sr_w8_pc and the arena/texture-bounds checks elsewhere in the runtime. */
static inline int sr_inrange_n(uint32_t a, uint32_t width) {
    uint32_t phys = (uint32_t)SR_PHYS(a);
    return phys < 0x0c000000u && (0x0c000000u - phys) >= width;
}

/* --- Guest-memory span validation (issue #15) --------------------------------
 * Overflow-safe bounds checks for BULK / string / parser accesses, where a whole
 * [addr, addr+size) range is touched at once (a memcpy, a string walk, a parsed
 * record) rather than one scalar. The scalar accessors already use sr_inrange_n();
 * these give bulk boundaries the same overflow-safe guarantee under an
 * intent-revealing name. Routing existing HLE/parser call sites through them is
 * tracked by #15; the checks themselves are proven by src/rt/guestmem_selftest.c.
 *
 * A zero-size span is always valid (it touches no bytes, so the base need not be
 * in range). readable/writable are identical today because the guest arena is one
 * uniformly read/write region; they are kept distinct so a call site states intent
 * and a future read-only region only changes one of them. */
static inline int sr_guest_span_readable(uint32_t addr, uint32_t size) {
    if (size == 0u) return 1;
    return sr_inrange_n(addr, size);
}
static inline int sr_guest_span_writable(uint32_t addr, uint32_t size) {
    if (size == 0u) return 1;
    return sr_inrange_n(addr, size);
}

/* Checked size arithmetic for computing a span extent BEFORE validating it, so a
 * parser cannot wrap uint32_t (e.g. count*stride, base+len) into a small value that
 * then passes a bounds check. Return 1 and store the result on success; return 0 on
 * overflow and leave *out unmodified. `out` may be NULL to test only for overflow. */
static inline int sr_size_add_ok(uint32_t a, uint32_t b, uint32_t *out) {
    if (a > 0xFFFFFFFFu - b) return 0;
    if (out) *out = a + b;
    return 1;
}
static inline int sr_size_mul_ok(uint32_t a, uint32_t b, uint32_t *out) {
    if (a != 0u && b > 0xFFFFFFFFu / a) return 0;
    if (out) *out = a * b;
    return 1;
}
extern void sr_oor(uint32_t a, uint32_t v, int store);   /* records out-of-range access (diag) */
/* SR_HEAP_WATCH tracks allocator-owned free headers dynamically, so diagnostics survive
 * allocation-layout changes. The branch stays cold in normal runs; bulk native copy/clear
 * fast paths call sr_heap_note_bulk_write explicitly because they bypass MEM_W*. */
extern int g_sr_heap_watch;
extern int g_sr_metadata_watch;
extern int g_hle_depth;
extern CpuState *s_cpu;

/* A matched address/value watch can request a bounded register snapshot at one
 * exact writer PC. This is observational and inactive unless both a watch and
 * SR_WATCH_CONTEXT_PC are configured. */
static inline void sr_log_mem_watch_context(uint32_t pc) {
    if (__builtin_expect(g_sr_mem_watch_context_pc == 0u ||
                         pc != g_sr_mem_watch_context_pc ||
                         g_sr_mem_watch_context_count >= g_sr_mem_watch_context_limit ||
                         s_cpu == NULL ||
                         (g_sr_mem_watch_context_fpr >= 0 &&
                          s_cpu->fi[g_sr_mem_watch_context_fpr] != g_sr_mem_watch_context_fpr_value), 1)) {
        return;
    }
    unsigned hit = ++g_sr_mem_watch_context_count;
    fprintf(stderr,
            "MEM_WATCH_CONTEXT pc=0x%08x hit=%u last_yield_pc=0x%08x hi=0x%08x lo=0x%08x fcr31=0x%08x fpcond=%u\n",
            pc, hit, s_cpu->pc, s_cpu->hi, s_cpu->lo, s_cpu->fcr31, s_cpu->fpcond);
    for (unsigned base = 0; base < 32; base += 8) {
        fprintf(stderr, "MEM_WATCH_CONTEXT_GPR");
        for (unsigned i = base; i < base + 8; i++) {
            fprintf(stderr, " r%u=0x%08x", i, s_cpu->r[i]);
        }
        fputc('\n', stderr);
    }
    for (unsigned base = 0; base < 32; base += 8) {
        fprintf(stderr, "MEM_WATCH_CONTEXT_FPR");
        for (unsigned i = base; i < base + 8; i++) {
            fprintf(stderr, " f%u=0x%08x", i, s_cpu->fi[i]);
        }
        fputc('\n', stderr);
    }
}

/* Bounded context for one exact generated store PC. Unlike value watches this
 * does not log unrelated stores that happen to carry the same common value. */
static inline void sr_log_store_context(uint32_t addr, uint32_t value,
                                        unsigned width, uint32_t pc) {
    if (__builtin_expect(g_sr_store_context_pc == 0u ||
                         pc != g_sr_store_context_pc ||
                         g_sr_store_context_count >= g_sr_store_context_limit ||
                         s_cpu == NULL, 1)) {
        return;
    }
    unsigned hit = ++g_sr_store_context_count;
    fprintf(stderr,
            "STORE_CONTEXT pc=0x%08x hit=%u addr=0x%08x width=%u val=0x%08x last_yield_pc=0x%08x hi=0x%08x lo=0x%08x\n",
            pc, hit, addr, width, value, s_cpu->pc, s_cpu->hi, s_cpu->lo);
    for (unsigned base = 0; base < 32; base += 8) {
        fprintf(stderr, "STORE_CONTEXT_GPR");
        for (unsigned i = base; i < base + 8; i++) {
            fprintf(stderr, " r%u=0x%08x", i, s_cpu->r[i]);
        }
        fputc('\n', stderr);
    }
    for (unsigned base = 0; base < 32; base += 8) {
        fprintf(stderr, "STORE_CONTEXT_FPR");
        for (unsigned i = base; i < base + 8; i++) {
            fprintf(stderr, " f%u=0x%08x", i, s_cpu->fi[i]);
        }
        fputc('\n', stderr);
    }
    if (g_sr_store_context_mem_gpr >= 0) {
        uint32_t base = s_cpu->r[g_sr_store_context_mem_gpr] +
                        g_sr_store_context_mem_offset;
        fprintf(stderr, "STORE_CONTEXT_MEM r%d+0x%08x base=0x%08x",
                g_sr_store_context_mem_gpr, g_sr_store_context_mem_offset, base);
        for (unsigned i = 0; i < g_sr_store_context_mem_words; i++) {
            uint32_t word_addr = base + i * 4u;
            uint32_t word = 0;
            if (sr_inrange_n(word_addr, 4u)) {
                memcpy(&word, SR_HOST(word_addr), sizeof word);
                fprintf(stderr, " w%u=0x%08x", i, word);
            } else {
                fprintf(stderr, " w%u=<oor>", i);
            }
        }
        fputc('\n', stderr);
    }
    fflush(stderr);
}

static inline void sr_check_metadata_watch(uint32_t addr, uint32_t val, int write, int width, uint32_t pc) {
    if (__builtin_expect(g_sr_metadata_watch, 0)) {
        /* Use uint64_t to prevent wrap-around when addr is near UINT32_MAX: a
         * 32-bit addr + width - 1 can wrap to a small value and falsely match
         * the monitored window.  The comparison against 32-bit constants is safe
         * because both sides are widened before the comparison. */
        uint64_t end = (uint64_t)addr + (uint32_t)width - 1ULL;
        if ((uint64_t)addr <= 0x0030a0bfULL && end >= 0x0030a040ULL) {
            extern uint32_t sched_current_uid(void);
            fprintf(stderr, "METADATA_WATCH: %s addr=0x%08x width=%d val=0x%08x pc=0x%08x thread_uid=0x%x caller=%s\n",
                    write ? "WRITE" : "READ",
                    addr, width, val, pc, sched_current_uid(),
                    ((g_hle_depth > 0) || (sched_current_uid() == 0)) ? "host/HLE" : "guest");
        }
    }
}

void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc);
void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc);

static inline uint8_t  sr_r8 (uint32_t a) {
    if (sr_inrange(a)) {
        uint8_t v = *(uint8_t  *)SR_HOST(a);
        sr_check_metadata_watch(a, v, 0, 1, s_cpu ? s_cpu->pc : 0);
        return v;
    }
    sr_oor(a,0,0); return 0u;
}
static inline uint16_t sr_r16(uint32_t a) {
    if (sr_inrange_n(a, 2)) {
        uint16_t v; memcpy(&v, SR_HOST(a), sizeof v);
        sr_check_metadata_watch(a, v, 0, 2, s_cpu ? s_cpu->pc : 0);
        return v;
    }
    sr_oor(a,0,0); return 0u;
}
static inline uint32_t sr_r32(uint32_t a) {
    if ((a & 0xFFFFFFFC) == 0x04084000) {
        extern uint32_t sr_get_ge_status(void);
        return sr_get_ge_status();
    }
    if (sr_inrange_n(a, 4)) {
        uint32_t v; memcpy(&v, SR_HOST(a), sizeof v);
        sr_check_metadata_watch(a, v, 0, 4, s_cpu ? s_cpu->pc : 0);
        return v;
    }
    sr_oor(a,0,0); return 0u;
}
static inline void sr_w8_pc(uint32_t a, uint8_t v, uint32_t pc) {
    if (__builtin_expect(g_sr_store_context_pc != 0u, 0)) sr_log_store_context(a, v, 1u, pc);
    if (__builtin_expect(g_sr_last_writer_enabled, 0)) sr_note_mem_write(a, 1u, v, pc);
    if (sr_check_mem_watch(a, v, 1, pc)) sr_log_mem_watch_context(pc);
    sr_check_metadata_watch(a, v, 1, 1, pc);
    if (__builtin_expect(g_sr_heap_watch, 0)) sr_heap_note_write(a, 1u, v, pc);
    if (sr_inrange(a)) *(uint8_t *)SR_HOST(a) = v; else sr_oor(a, v, 1);
}
static inline void sr_w16_pc(uint32_t a, uint16_t v, uint32_t pc) {
    if (__builtin_expect(g_sr_store_context_pc != 0u, 0)) sr_log_store_context(a, v, 2u, pc);
    if (__builtin_expect(g_sr_last_writer_enabled, 0)) sr_note_mem_write(a, 2u, v, pc);
    if (sr_check_mem_watch(a, v, 1, pc)) sr_log_mem_watch_context(pc);
    sr_check_metadata_watch(a, v, 1, 2, pc);
    if (__builtin_expect(g_sr_heap_watch, 0)) sr_heap_note_write(a, 2u, v, pc);
    if (sr_inrange_n(a, 2)) memcpy(SR_HOST(a), &v, sizeof v); else sr_oor(a, v, 1);
}
static inline void sr_w32_pc(uint32_t a, uint32_t v, uint32_t pc) {
    if (__builtin_expect(g_sr_store_context_pc != 0u, 0)) sr_log_store_context(a, v, 4u, pc);
    if (__builtin_expect(g_sr_last_writer_enabled, 0)) sr_note_mem_write(a, 4u, v, pc);
    if (sr_check_mem_watch(a, v, 1, pc)) sr_log_mem_watch_context(pc);
    sr_check_metadata_watch(a, v, 1, 4, pc);
    if (__builtin_expect(g_sr_heap_watch, 0)) sr_heap_note_write(a, 4u, v, pc);
    if (sr_inrange_n(a, 4)) memcpy(SR_HOST(a), &v, sizeof v); else sr_oor(a, v, 1);
}
static inline void sr_w8 (uint32_t a, uint8_t v) { sr_w8_pc(a, v, 0); }
static inline void sr_w16(uint32_t a, uint16_t v) { sr_w16_pc(a, v, 0); }
static inline void sr_w32(uint32_t a, uint32_t v) { sr_w32_pc(a, v, 0); }

#define MEM_R8(a)   sr_r8(a)
#define MEM_R16(a)  sr_r16(a)
#define MEM_R32(a)  sr_r32(a)
#define MEM_W8(a,v)  sr_w8((a), (uint8_t)(v))
#define MEM_W16(a,v) sr_w16((a), (uint16_t)(v))
#define MEM_W32(a,v) sr_w32((a), (uint32_t)(v))
#define MEM_W8_PC(a,v,pc)  sr_w8_pc((a), (uint8_t)(v), (pc))
#define MEM_W16_PC(a,v,pc) sr_w16_pc((a), (uint16_t)(v), (pc))
#define MEM_W32_PC(a,v,pc) sr_w32_pc((a), (uint32_t)(v), (pc))

void  sr_mem_init(void);
void  sr_load_segment(uint32_t vaddr, const void *data, uint32_t len);
uint32_t sr_loaded_end(void);   /* highest guest address the loader wrote (module end incl. BSS) */

/* Backing store for the game's newlib allocation API (see tools/codegen.py custom
 * stubs). The retail memalign/realloc bodies edit dlmalloc metadata directly, so every
 * metadata-manipulating entry point must stay on this host allocator's header ABI. */
uint32_t sr_newlib_malloc(uint32_t size, uint32_t guest_ra);
void     sr_newlib_free(uint32_t ptr, uint32_t guest_ra);
uint32_t sr_newlib_memalign(uint32_t alignment, uint32_t size, uint32_t guest_ra);
uint32_t sr_newlib_realloc(uint32_t ptr, uint32_t size, uint32_t guest_ra);

/* Unaligned word access (MIPS LWL/LWR/SWL/SWR), little-endian, matching PPSSPP's
 * interpreter. The load forms take the current rt and the effective address and return the
 * merged register value; the store forms read-modify-write the aligned word at addr&~3. */
uint32_t sr_lwl(uint32_t rtv, uint32_t addr);
uint32_t sr_lwr(uint32_t rtv, uint32_t addr);
void     sr_swl(uint32_t addr, uint32_t rtv);
void     sr_swr(uint32_t addr, uint32_t rtv);
void     sr_swl_pc(uint32_t addr, uint32_t rtv, uint32_t pc);
void     sr_swr_pc(uint32_t addr, uint32_t rtv, uint32_t pc);
void     sr_break(CpuState *s, uint32_t code, uint32_t pc);
void     sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
uint32_t sr_bitrev(uint32_t x);

/* PSP-EABI bridge used by the generated guest sprintf entry. */
void sr_guest_sprintf(CpuState *s);

/* VFPU source/destination prefix application (ARCHITECTURE section 6.4), ported from
 * PPSSPP. sr_vread reads n lanes from physical indices idx[], then applies a source
 * prefix (swizzle/abs/negate/constant). sr_vwrite applies the destination prefix
 * (saturate) and write mask, then stores. The prefix value comes from s->vfpuCtrl. */
void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix);
void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t dprefix);

/* VFPU transcendentals, exact ports of PPSSPP's table-based kernels (the PSP hardware does
 * not compute these with IEEE math). The lookup tables are loaded once from assets/vfpu/
 * (override the directory with PSP_VFPU_TABLES); the loader validates exact length, EOF,
 * SHA-256 and value-domain invariants before atomic publication (see vfpu_tables.h, #187). */
float sr_vfpu_rcp(float x);
float sr_vfpu_rsqrt(float x);
float sr_vfpu_sqrt(float x);
float sr_vfpu_asin(float x);
float sr_vfpu_log2(float x);
float sr_vfpu_sin(float x);
float sr_vfpu_cos(float x);
float sr_vfpu_exp2(float x);

/* Single-instruction VFPU interpreter (src/rt/vfpu_interp.c). Returns SR_VFPU_COMPUTE for a
 * value-producing op (compare v[]/f[] to the reference trace), SR_VFPU_STATE for a prefix,
 * control, or store op, or SR_VFPU_OTHER for an instruction it cannot execute. */
#define SR_VFPU_OTHER   0
#define SR_VFPU_COMPUTE 1
#define SR_VFPU_STATE   2
int sr_vfpu_interp(CpuState *s, uint32_t op);

/* Codegen tags a per-instruction VFPU fallback address and sends it through dispatch().
 * The tag is outside the PSP physical arena; dispatch reads the original word at the low
 * address and invokes sr_vfpu_interp without changing CpuState's ABI/layout. */
#define SR_DISPATCH_VFPU_TAG  0x40000000u
#define SR_DISPATCH_VFPU_MASK 0xFC000000u

/* Dispatch: guest address -> native recompiled function (section 7). Computed jumps/calls go
 * through here; unknown targets would fall to the interpreter once it is linked in. */
typedef void (*RecompFn)(CpuState *);
void     sr_register(uint32_t addr, RecompFn fn);
uint32_t sr_register_count(void);  /* number of sr_register() calls performed so far */
RecompFn sr_lookup(uint32_t addr);
void     dispatch(CpuState *s, uint32_t target);

/* Tracing. When a trace file is open, the generated code reports each instruction. sr_begin
 * snapshots the register file and records pc/op; sr_end diffs and emits the line, with an
 * optional store (addr/size) read back from guest memory. Emit order follows PPSSPP: a branch
 * reports before its delay slot. */
int  sr_trace_open(const char *path, const char *target, uint32_t start_pc);
void sr_trace_close(void);
/* Throughput: the generated chunks emit an sr_begin/sr_end pair around *every* guest
 * instruction (~1.5M call sites total). Since those huge files must compile at -O0, the
 * release build removes the hooks in the preprocessor. TRACE=1 retains a predicted-false
 * runtime gate and byte-accurate instruction tracing for oracle/diff builds. The address
 * expression passed to sr_end is a pure register-plus-constant computation. */
extern int sr_trace_active;
void sr_begin_impl(CpuState *s, uint32_t pc, uint32_t op);
void sr_end_impl(CpuState *s, uint32_t mem_addr, int mem_size);
#ifdef SR_INSTRUCTION_TRACE
#define sr_begin(s, pc, op)       do { if (__builtin_expect(sr_trace_active, 0)) sr_begin_impl((s), (pc), (op)); } while (0)
#define sr_end(s, mem_addr, size) do { if (__builtin_expect(sr_trace_active, 0)) sr_end_impl((s), (mem_addr), (size)); } while (0)
#else
/* The generated chunks are intentionally compiled at -O0 to avoid compiler
 * OOM.  At that optimization level even a predicted-false trace test remains
 * in every guest instruction.  Compile the hooks completely out of release
 * chunks; TRACE=1 restores the byte-accurate oracle/diff instrumentation. */
#define sr_begin(s, pc, op)       ((void)0)
#define sr_end(s, mem_addr, size) ((void)0)
#endif

/* HLE boundary marker used by the bring-up driver: a call to an unresolved import stop the
 * traced run so the comparison ends exactly where the reference trace reaches its first syscall. */
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_hle_advance_time(uint32_t us);                                              /* virtual-time charge per HLE (sched.c) */
extern int sr_hit_hle;

/* HLE syscall dispatch. The recompiled import stub at <stub> calls sr_syscall with the NID
 * resolved from the PRX import table. It dispatches to the registered handler (which reads
 * arguments from $a0-$a3 and returns the $v0 value), then poisons the caller-saved temp
 * registers to 0xDEADBEEF exactly as PPSSPP's kernel does, and writes the return to $v0. A
 * NID with no handler logs and stops at the HLE boundary (longjmp) so bring-up can see which
 * import to implement next. sr_last_nid records the most recent dispatched NID. */
uint32_t sr_alloc_uid(void);

/* sceGe display-list GPU (src/rt/ge.c): execute a GE command list, rasterising into VRAM. */
uint32_t ge_run_list(uint32_t addr, int resume);
extern uint32_t g_ge_stall_addr;
uint32_t ge_framebuffer(void);

/* Interactive window front-end (src/rt/gui.c, Win32). gui_init opens the window; gui_present is
 * called from sceDisplaySetFrameBuf to show a frame, pump messages, and sample the keyboard;
 * gui_buttons returns the live PSP pad state; gui_on reports whether the window is active. */
#define SR_APP_TITLE "Nakagawa Recomp"   /* canonical window caption; see also gpu_sdl3vk/sdl3vk.c */
void     gui_init(const char *title);
int      gui_on(void);
uint32_t gui_buttons(void);
void     gui_consume_button_pulses(void);          /* after one PSP VBLANK sample */
void     gui_analog(uint8_t *lx, uint8_t *ly);   /* live left-stick (0..255, 128=centre) */
int      gui_pad_present(void);                  /* 1 when a game controller is connected */
void     gui_present(uint32_t fbaddr, int fmt, uint32_t stride);

typedef uint32_t (*HleFn)(CpuState *s);
void     sr_hle_register(uint32_t nid, const char *name, HleFn fn);
void     sr_hle_init(void);     /* registers all built-in handlers (idempotent) */
uint32_t sr_syscall(CpuState *s, uint32_t nid);
extern uint32_t sr_last_nid;

/* Late-import resolution contract (src/rt/hle.c, Track B). Runtime-loaded PRX exports are
 * published by module load through sr_hle_register_late_import(); dispatch() queries
 * sr_hle_resolve_late_import() on a lookup miss. Result meanings:
 *   0                    unresolved — caller falls back to its miss handling
 *   SR_HLE_LATE_BUILTIN  the id names a built-in HLE handler — trap into sr_syscall
 *   anything else        a rebased guest export address — dispatch to that target
 * The registry is protected by hle.c's spinlock; both calls are fiber/thread safe. */
#define SR_HLE_LATE_BUILTIN (UINT32_MAX - 1u)
int      sr_hle_register_late_import(uint32_t nid, uint32_t target);
uint32_t sr_hle_resolve_late_import(uint32_t nid);

/* Cooperative scheduler with preemptive yield points (src/rt/sched.c). The recompiled code
 * calls SR_YIELD at function entry and loop back-edges; when the scheduler is active and the
 * thread's timeslice runs out, it switches to another ready thread (Windows fibers). When the
 * scheduler is off (per-function differential, plain driver) SR_YIELD is a cheap no-op that
 * changes nothing, so it does not affect the existing trace verification. */
extern int     sr_sched_on;
extern atomic_int_least32_t sr_timeslice;
void sr_yield(CpuState *s);
/* Environment-gated, bounded guest-function probe used by boot diagnostics.
 * Codegen emits calls only at explicitly reviewed function boundaries. */
void sr_boot_probe(CpuState *s, uint32_t guest_pc);
/* Returns 1 if the host wall-clock has crossed the vblank quantum since the last vblank
 * delivery. Defined in sched.c; safe to call from anywhere reactive. Used by SR_YIELD to
 * force a premature slice expiry when the recomp-emitted yield cadence is too sparse to
 * keep the engine vblank callback chain alive (e.g. during a busy-wait on a guest latch). */
int sr_vblank_quantum_due(void);
/* f_00065c60 field-writer intercept: the container walker reads count from
 * r18+0x04 but f_0005a500's control block init puts count at r18+0x00 and
 * cursor at r18+0x04.  Since both functions are called directly (not via
 * dispatch()), the only hook point is SR_YIELD at function entry.  Copy
 * count from +0x00 to +0x04 before the walker body executes.  Idempotent:
 * if +0x04 already holds count, writing it again is harmless. */
extern int g_prof_enabled;
void sr_profile_init(void);
void sr_profile_dump(void);
void sr_profile_block(uint32_t target_pc);
#ifdef SR_PROFILER_SELFTEST
void sr_profile_test_reset(void);
uint64_t sr_profile_test_block_count(uint32_t pc);
uint64_t sr_profile_test_lookup_drops(void);
#endif

#define SR_YIELD(s, target_pc) do { \
    if (__builtin_expect(g_prof_enabled, 0)) { \
        sr_profile_block(target_pc); \
    } \
    if (__builtin_expect(sr_sched_on, 1)) { \
        if (atomic_fetch_sub_explicit(&sr_timeslice, 1, memory_order_relaxed) <= 1) { \
            (s)->pc = (target_pc); \
            sr_yield(s); \
        } \
    } \
} while (0)



/* Scheduler API used by the thread HLE handlers and the driver. */
#define SCHED_INTR_VBLANK 0x00000001u /* coalescing display source */
#define SCHED_INTR_GE     0x00000002u /* reserved GE source; retained until its handler lands */

/* Synchronously prepare the extracted-XB data route (hle.c) BEFORE any guest
 * execution exists. The cold SR_DATAROOT census must never begin from a guest
 * HLE call: it would run the whole filesystem walk on the single guest-scheduler
 * thread and starve every guest thread, tick, and VBLANK under contention.
 * Returns 1 iff the route reached READY; FAILED/DISABLED keep lookups failing
 * closed to the ordinary ISO/VFS path and are not startup errors. */
int sr_host_data_prepare(void);

void     sched_init(CpuState *cpu);                 /* CpuState the running thread reads/writes */
uint32_t sched_create_thread(uint32_t entry, int priority, uint32_t stack_size);
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp);
void     sched_exit_current(int32_t status);        /* non-delete exit; normalize signed-negative status */
void     sched_exit_current_unchecked(int32_t status); /* raw status for non-ThreadMan teardown */
void     sched_exit_current_delete(int32_t status); /* current thread exits and deletes its object */
/* PSP interrupt suspension also suppresses preemption on the single CPU.  Retail
 * newlib relies on this around every mutation of its global malloc-bin state. */
uint32_t sched_suspend_interrupts(void);             /* returns previous enabled state */
void     sched_resume_interrupts(uint32_t state);    /* restores a prior state */
int      sched_interrupts_enabled(void);
uint32_t sched_suspend_dispatch(void);              /* sceKernelSuspendDispatchThread */
uint32_t sched_resume_dispatch(uint32_t state);     /* sceKernelResumeDispatchThread */
int      sched_dispatch_enabled(void);
/* State-only query: interrupts enabled AND dispatch enabled. Carries no policy --
 * each blocking handler asks it at its own point, after its own validation. See the
 * definition in sched.c for why a universal pre-handler gate is ruled out. */
int      sched_wait_permitted(void);
int      sched_current_has_pending_wakeup(void);        /* banked sceKernelWakeupThread count */
int      sched_current_join_result_pending(uint32_t uid); /* non-consuming join-result peek */
void     sched_raise_interrupt(uint32_t source);     /* latch source; delivery occurs at a scheduler boundary */
uint32_t sched_pending_interrupts(void);             /* source bits not yet serviced */
void     sched_delay_current(uint32_t usec);        /* block current thread for usec */
void     sched_preempt(void);                       /* yield now if a higher-priority thread is ready */
void     sched_block_on(uint32_t obj);              /* block current thread until sched_wake(obj) */
void     sched_wait_vblank(void);                   /* block current thread until the next delivered vblank */
int      sched_block_on_timeout(uint32_t obj, uint32_t usec);  /* returns 1 if timed out */
void     sched_wake(uint32_t obj);                  /* ready all threads blocked on obj */
/* Wait-object ids shared between hle.c (wait side) and sched.c (thread-dump side). */
#define CTRL_WAIT_OBJ 0xC471D000u                   /* sceCtrl blocking reads park on this object */
uint64_t sched_vtime_us(void);
uint64_t sched_vtime_deadline_after(uint64_t delta); /* saturating guest-time deadline */
void     sched_vtime_refresh(void);
/* Display scanout observations derived from the same monotonic guest timeline.  The
 * 59.94-Hz frame phase is rational (60000/1001), so repeated reads are stable and
 * elapsed scheduler time—not HLE call count—advances the HCOUNT source. */
uint32_t sched_display_current_hcount(void);
uint32_t sched_display_accumulated_hcount(void);
int      sched_display_is_vblank(void);
/* Advance guest-visible VCOUNT by the number of elapsed display periods the
 * scheduler source just latched.  This is display-period accounting, deliberately
 * separate from deliver_vblank()/sr_vblank_tick(), which runs once per serviced
 * source event and owns framebuffer/interrupt/callback side effects. */
void     sr_display_advance_vcount(uint32_t elapsed_periods);
void     sched_set_current_cb_wait(int cb_wait);    /* mark running thread as callback-waiting */
void     sched_wake_callbacks(uint32_t thread_uid); /* wake thread waiting in CB-wait */
void     sched_thread_sleep(void);                  /* sceKernelSleepThread (wakeup-count) */
void     sched_thread_sleep_cb(void);               /* sceKernelSleepThreadCB (wakeup-count) */
uint32_t sched_thread_wakeup(uint32_t uid);         /* sceKernelWakeupThread (banks if not asleep) */
void     sched_set_priority(uint32_t uid, int priority);   /* sceKernelChangeThreadPriority */
uint32_t sched_terminate_thread(uint32_t uid);      /* sceKernelTerminateThread */
uint32_t sched_delete_thread(uint32_t uid);          /* sceKernelDeleteThread object removal */
int      sched_thread_cancel_wakeup(uint32_t uid);  /* sceKernelCancelWakeupThread; uid 0=current */
/* Thread role identity.
 *
 * A role UID is an OUTCOME of allocation, never configuration: the scheduler records
 * whichever UID the allocator happened to give the thread that took the role. UIDs are
 * handed out from 0x110 upward, so any plausible-looking UID is also an ordinary UID --
 * which is why "no role" must be represented structurally rather than by a numeric
 * default. SR_ROLE_UID_NONE is that representation, and sr_alloc_uid() never returns it.
 *
 * Read a role UID with the accessors below only when you want the number itself (a
 * diagnostic label, a table key). To ask "is this thread the worker?", use the
 * predicates: they fail closed on an uncaptured role, and they never treat UID 0 --
 * PSP's "current thread" / "no thread" value -- as a captured role. */
#define SR_ROLE_UID_NONE 0xFFFFFFFFu

uint32_t sched_root_uid(void);                      /* captured root UID, or SR_ROLE_UID_NONE */
uint32_t sched_worker_uid(void);                    /* captured worker UID, or SR_ROLE_UID_NONE */
uint32_t sched_launcher_uid(void);                  /* captured launcher UID, or SR_ROLE_UID_NONE */
int      sched_role_uid_captured(uint32_t role_uid);/* 1 when a role accessor returned a real UID */
int      sched_uid_is_root(uint32_t uid);           /* fail-closed role tests: 0 when uncaptured */
int      sched_uid_is_worker(uint32_t uid);
int      sched_uid_is_launcher(uint32_t uid);
int      sched_current_is_worker(void);             /* 0 when there is no current thread */
int      sched_current_is_launcher(void);
typedef struct SrThreadRunStatus {
    uint32_t size;
    uint32_t status;
    uint32_t currentPriority;
    uint32_t waitType;
    uint32_t waitId;
    uint32_t wakeupCount;
    uint32_t runClocksLow;
    uint32_t runClocksHigh;
    uint32_t intrPreemptCount;
    uint32_t threadPreemptCount;
    uint32_t releaseCount;
} SrThreadRunStatus;
int      sched_thread_run_status(uint32_t uid, SrThreadRunStatus *out);
uint32_t sched_thread_exit_status(uint32_t uid);
void     sched_set_current_join_target(uint32_t uid);
void     sched_clear_current_join_target(void);
int      sched_take_current_join_result(uint32_t uid, uint32_t *result_out);
int      sched_current_priority(void);
int      sched_is_dormant(uint32_t uid);
uint32_t sched_current_uid(void);                   /* 0 when no thread is current */
void     sched_run(uint32_t entry, uint32_t arglen, uint32_t argp);  /* run from the entry thread */

/* PSP callback ABI: SceKernelCallbackFunction(int count, int arg, void *common).
 * PPSSPP (__KernelRunCallbackOnThread) passes { notifyCount, notifyArg, commonArgument },
 * so $a0 = notify count, $a1 = notify argument, $a2 = the arg registered at
 * sceKernelCreateCallback time.  Shared by the production dispatcher (hle.c) and the
 * scheduler selftest so the register order is asserted against the code the game runs. */
static inline void sr_callback_pack_args(CpuState *cpu, int notify_count,
                                         uint32_t notify_arg, uint32_t common_arg) {
    cpu->r[4] = (uint32_t)notify_count;
    cpu->r[5] = notify_arg;
    cpu->r[6] = common_arg;
}

/* Run one already-armed callback as a nested call on the owning thread's live register
 * file, the same way any other guest `jal` is dispatched: only the registers a real `jal`
 * sets (argument registers, $ra, $pc) are touched, and the full pre-call snapshot is
 * restored once the callback returns. Everything else -- callee-saved regs, HI/LO,
 * FPU/VFPU state, the guest stack -- is left exactly as the interrupted thread had it,
 * and is the callback's own prologue/epilogue's responsibility to save/restore per the
 * MIPS calling convention, matching real hardware/PPSSPP callback dispatch.
 *
 * dispatch_fn is injected (rather than calling dispatch() directly) so this exact
 * production code path can be unit-tested with a stand-in dispatcher instead of linking
 * the full recompiled runtime; hle.c passes the real dispatch().
 *
 * Returns the callback's $v0 (observed before the interrupted context is restored), so
 * the caller can implement the PSP kernel's "a callback that returns non-zero is
 * automatically deleted" rule. */
static inline uint32_t sr_callback_dispatch_one(CpuState *cpu, uint32_t entry, int notify_count,
                                                uint32_t notify_arg, uint32_t common_arg,
                                                void (*dispatch_fn)(CpuState *, uint32_t)) {
    CpuState save = *cpu;
    sr_callback_pack_args(cpu, notify_count, notify_arg, common_arg);
    /* PSP callbacks are nested calls on the interrupted thread. In particular, $gp
     * is inherited from that live context; the kernel does not install a
     * callback-global GP. */
    cpu->r[31] = 0;
    cpu->pc = entry;
    dispatch_fn(cpu, entry);
    uint32_t ret = cpu->r[2];
    *cpu = save;
    return ret;
}

void sr_unimplemented(uint32_t pc, const char *reason);

#include <setjmp.h>
extern jmp_buf g_hle_jmp;

/* ---- register conflict resolution ---- */
#undef r0
#undef r1
#undef r2
#undef r3
#undef r4
#undef r5
#undef r6
#undef r7
#undef r8
#undef r9
#undef r10
#undef r11
#undef r12
#undef r13
#undef r14
#undef r15
#undef r16
#undef r17
#undef r18
#undef r19
#undef r20
#undef r21
#undef r22
#undef r23
#undef r24
#undef r25
#undef r26
#undef r27
#undef r28
#undef r29
#undef r30
#undef r31


#ifdef __cplusplus
#include "cpu.h"
static_assert(sizeof(::CpuState) == sizeof(ref::CpuState), "CpuState structural layout drift detected!");
#define SR_CPUSTATE_OFFSET_ASSERT(field) \
    static_assert(offsetof(::CpuState, field) == offsetof(ref::CpuState, field), \
                  "CpuState offset drift: " #field)
SR_CPUSTATE_OFFSET_ASSERT(r);
SR_CPUSTATE_OFFSET_ASSERT(hi);
SR_CPUSTATE_OFFSET_ASSERT(lo);
SR_CPUSTATE_OFFSET_ASSERT(pc);
SR_CPUSTATE_OFFSET_ASSERT(f);
SR_CPUSTATE_OFFSET_ASSERT(fi);
SR_CPUSTATE_OFFSET_ASSERT(fcr31);
SR_CPUSTATE_OFFSET_ASSERT(fpcond);
SR_CPUSTATE_OFFSET_ASSERT(v);
SR_CPUSTATE_OFFSET_ASSERT(vi);
SR_CPUSTATE_OFFSET_ASSERT(vfpuCtrl);
SR_CPUSTATE_OFFSET_ASSERT(status);
SR_CPUSTATE_OFFSET_ASSERT(next_pc);
SR_CPUSTATE_OFFSET_ASSERT(in_delay_slot);
#undef SR_CPUSTATE_OFFSET_ASSERT
#endif

#endif
