// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

/* *
 * The PRX import table maps each .sceStub.text stub to a (library, NID); tools/imports.py
 * resolves it and the codegen emits sr_syscall(s, NID) in each stub. A handler reads its
 * arguments from $a0-$a3 (and the stack for further args) and returns the $v0 value.
 *
 * The kernel object identifiers (thread/block UIDs) and allocation addresses PPSSPP returns
 * come from its own boot-time allocators, so they are not reproducible here without simulating
 * PPSSPP's whole kernel. This HLE is instead internally consistent: it hands out its own UIDs
 * and allocates from its own bump pointer, and the game uses those values uniformly. Functional
 * equivalence is checked by the sequence of import calls (by NID), not by UID values.
 *
 * After each handler, the caller-saved temp registers are poisoned to 0xDEADBEEF exactly as
 * PPSSPP's SetDeadbeefRegs does, so traces stay aligned on the registers the game actually
 * keeps (it never relies on caller-saved registers surviving a call).
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _MSC_EXTENSIONS
#define _MSC_EXTENSIONS  /* for _Exit on MSVC; harmless under MinGW */
#endif
#include "recomp.h"
#include "iso.h"
#include "pgf_api.h"
#include "pgd_api.h"
#include "evf.h"         /* pure sceKernelEventFlag pattern/mode semantics */
#include "asset_index.h" /* dynamic extracted-data index (issue #223) */
#include "sdkver.h"      /* retained compiled-SDK-version state (issue #71) */
#include "vfs_path.h"    /* host-neutral VFS path join helper (issue #19) */
#include "nid_names.h"   /* sr_nid_name(): names unknown NIDs in the trap below */
#include "atrac3p_bridge.h" /* PR-B: real ATRAC3+ decode in sceAtracDecodeData */
#include "fbcap_policy.h"   /* frame-capture slot policy for the present path (issue #57) */
#include "gpu_sdl3vk/ge_gpu.h" /* explicit guest-VRAM snapshot boundary */
#include "title_config.h"  /* title-qualified compatibility addresses (issue #98) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <setjmp.h>
#include <process.h>     /* _exit() */
#include <windows.h>     /* Sleep() */
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>

uint32_t sr_last_nid = 0;

int sr_thread_has_pending_callbacks(uint32_t thread_uid);
int sr_thread_dispatch_callbacks(void);    /* internal pump count; public CheckCallback is Boolean */
int sr_callback_is_valid(uint32_t uid);
uint32_t sr_callback_notify(uint32_t uid, uint32_t notify_arg);
void sr_callback_unregister_owner(uint32_t thread_uid);

/* Issue #143 diagnostic hooks.  Definitions live beside the display vblank
 * counter because the trace is deliberately bounded by guest-frame windows. */
static void ge_enqueue_trace_note_hle(CpuState *s, uint32_t nid, const char *name);
static void ge_enqueue_trace_note_callback(CpuState *s, uint32_t uid, uint32_t entry);

/* ---- handler table ---- */

typedef struct { uint32_t nid; const char *name; HleFn fn; } HleEntry;
/* Keep the registry comfortably above the current export set.  A full table
 * silently drops the late generic memcpy/memset handlers, which then makes a
 * valid NID look unimplemented even though its production function exists. */
#define HLE_CAP 1024
static HleEntry s_hle[HLE_CAP];
static int s_hle_n = 0;

/* Late imports are intentionally separate from recomp.c's address->native dispatch table.
 * Track A owns that table; this registry only publishes stable NID classifications/guest
 * targets. 0 means unresolved, UINT32_MAX-1 means a built-in HLE NID (call sr_syscall), and
 * every other result is a runtime-loaded guest export address (call dispatch on that target). */
#define LATE_IMPORT_CAP 256
typedef struct { uint32_t nid, target; } LateImportEntry;
static LateImportEntry s_late_imports[LATE_IMPORT_CAP];
static unsigned s_late_import_n;
static atomic_flag s_hle_lock = ATOMIC_FLAG_INIT;
static atomic_int s_hle_init_state;

/* HLE calls are among the hottest runtime paths.  Environment variables do
 * not change after process launch, so cache the logging switch instead of
 * searching the environment on every guest syscall. */
static int hle_log_on(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("SR_HLELOG") != NULL;
    return enabled;
}

/* Display-list tracing is extremely verbose (several writes per presented
 * frame).  Keep Standard mode quiet; Diagnostics enables SR_GEDUMP, and the
 * public SR_DEBUG GE bit (0x08) remains an equivalent opt-in. */
static int ge_log_on(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *debug = getenv("SR_DEBUG");
        enabled = getenv("SR_GEDUMP") != NULL || getenv("SR_GELOG") != NULL ||
                  (debug != NULL && (strtoul(debug, NULL, 0) & 0x08u) != 0u);
    }
    return enabled;
}

static void hle_lock(void) {
    while (atomic_flag_test_and_set_explicit(&s_hle_lock, memory_order_acquire)) { }
}
static void hle_unlock(void) { atomic_flag_clear_explicit(&s_hle_lock, memory_order_release); }

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
    if (s_late_import_n == LATE_IMPORT_CAP) { hle_unlock(); return 0; }
    memmove(&s_late_imports[lo + 1], &s_late_imports[lo],
            (s_late_import_n - lo) * sizeof(s_late_imports[0]));
    s_late_imports[lo] = (LateImportEntry){nid, target};
    s_late_import_n++;
    hle_unlock();
    return 1;
}

uint32_t sr_hle_resolve_late_import(uint32_t nid) {
    uint32_t result = 0;
    sr_hle_init();
    hle_lock();
    unsigned lo = 0, hi = s_late_import_n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (s_late_imports[mid].nid < nid) lo = mid + 1;
        else hi = mid;
    }
    if (lo < s_late_import_n && s_late_imports[lo].nid == nid)
        result = s_late_imports[lo].target;
    if (!result) {
        for (int i = 0; i < s_hle_n; i++) {
            if (s_hle[i].nid == nid) { result = SR_HLE_LATE_BUILTIN; break; }
        }
    }
    hle_unlock();
    return result;
}

void sr_hle_register(uint32_t nid, const char *name, HleFn fn) {
    /* Duplicate NID guard. The 3 dead duplicate registrations found in this file's static
     * NID table (sceUmdRegisterUMDCallBack, sceAtracSetLoopNum exact dupes, and the
     * sceKernelTotalFreeMemSize / synthetic "SysMemUserForUser_f919f628" collision on
     * 0xf919f628) were removed at the source. This guard stays as
     * a diagnostic backstop against future accidental duplicates, not a live workaround. */
    hle_lock();
    for (int i = 0; i < s_hle_n; i++) {
        if (s_hle[i].nid == nid) {
            fprintf(stderr, "HLE: duplicate NID 0x%08x (%s) rejected; first was %s\n",
                    nid, name, s_hle[i].name);
            hle_unlock();
            return;
        }
    }
    if (s_hle_n < HLE_CAP) { s_hle[s_hle_n].nid = nid; s_hle[s_hle_n].name = name; s_hle[s_hle_n].fn = fn; s_hle_n++; }
    hle_unlock();
}

static HleEntry *hle_find(uint32_t nid) {
    HleEntry *result = NULL;
    hle_lock();
    for (int i = 0; i < s_hle_n; i++) if (s_hle[i].nid == nid) { result = &s_hle[i]; break; }
    hle_unlock();
    return result;
}

/* ---- argument / return helpers ---- */

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])
/* Argument 5+idx of an HLE call. PSP userland is MIPS EABI: arguments 1..8 arrive in
 * a0-a3,t0-t3 (r4..r11) and only 9+ go to the stack (sp+0) -- same as PPSSPP's PARAM(n).
 * This used to read the o32 stack home slots (sp+16+), which EABI callers never write:
 * every >4-argument HLE call received stack garbage (found via sceMpegRingbufferConstruct
 * getting its fill callback from there -- the movie player jumped into the ring buffer). */
static uint32_t stack_arg(CpuState *s, int idx) {
    return idx < 4 ? s->r[8 + idx] : MEM_R32(s->r[29] + (uint32_t)(idx - 4) * 4);
}

/* A blocking call made with CPU interrupts disabled or thread dispatch disabled
 * returns this, per PSPAutotests tests/intr/waits.expected. The handlers below
 * check sched_wait_permitted() individually, each at the point its own oracle
 * cells put it -- never as a shared pre-handler gate, because the same oracle
 * shows the precedence is per-API: ILLEGAL_MODE (L72) and ILLEGAL_THID (L204)
 * both beat it, while sceKernelWaitSema's bad-object error (L54/L55) does not.
 * See docs/PSP_INTR_WAITS_MATRIX.md. */
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT 0x800201a7u

/* Semaphore error codes measured by PSPAutotests
 * tests/threads/semaphores/wait.expected, which is a NORMAL-context oracle and
 * therefore covers exactly the column tests/intr/waits.expected does not:
 *
 *   UNKNOWN_SEMID  sceKernelWaitSema(0, 1, NULL)          -> 80020199   (L21)
 *                  sceKernelWaitSema(0xDEADBEEF, 1, NULL) -> 80020199   (L23)
 *                  a deleted semaphore id                 -> 80020199   (L25)
 *   ILLEGAL_COUNT  need 100 against maxCount 1            -> 800201BD   (L3, L11)
 *                  need 0                                 -> 800201BD   (L13)
 *                  need -1                                -> 800201BD   (L5, L15)
 *
 * WAIT_TIMEOUT is the same file's L9/L17 result for a wait that was legal,
 * entered, and expired. */
#define SCE_KERNEL_ERROR_UNKNOWN_SEMID 0x80020199u
#define SCE_KERNEL_ERROR_ILLEGAL_COUNT 0x800201bdu
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT  0x800201a8u

/* ---- kernel object UID + user-memory bump allocator ---- */

/* Single shared UID pool matching PPSSPP: uid = 0x110, 0x111, ... incrementing by 1.
 * Two values are skipped so they can mean something else unambiguously: 0 is PSP's
 * "current thread" argument value, and SR_ROLE_UID_NONE is the scheduler's "this role
 * has not been captured" marker (recomp.h). Neither is reachable from 0x110 in any real
 * session, but the pool wraps in principle and a role marker that could also be a live
 * thread identity is precisely the defect this skip exists to make impossible. */
static uint32_t s_uid = 0x110;
uint32_t sr_alloc_uid(void) {
    while (s_uid == 0u || s_uid == SR_ROLE_UID_NONE) s_uid++;
    uint32_t uid = s_uid++;
    if (getenv("SR_WAKELOG")) fprintf(stderr, "ALLOC_UID: 0x%x\n", uid);
    return uid;
}

/* User-partition bump allocator. The kernel hands the game one user partition; the first
 * sceKernelAllocPartitionMemory Low block sits immediately after the loaded module (like
 * PPSSPP's loader, which places it at the module end rather than a round boundary). Starting at
 * the real module end keeps the game's internal sub-allocator producing stable addresses.
 *
 * Generic defaults: the heap base is the loaded module's end (BSS included, from the flat
 * image) rounded up to 4 KB, and the partition runs to a conventional PSP user-memory ceiling.
 * Both are overridable as hex via SR_HEAP_BASE / SR_PARTITION_TOP for a game that needs an exact
 * PSP layout to reproduce reference addresses bit-for-bit. The free size the game queries is
 * (top - bump pointer) and shrinks as it allocates -- not a fixed fake. */
static uint32_t s_heap = 0;              /* bump pointer in user RAM; 0 = not yet initialised */
static uint32_t s_part_top = 0;
static uint32_t s_heap_last_bump = 0;     /* mirror of last s_heap value, for the main-thread diagnostic */
uint32_t user_partition_last_heap(void) { return s_heap_last_bump ? s_heap_last_bump : s_heap; }

/* ---- diagnostic: snapshot state at libc "should be called from main thread" prints ----
 * The libc runtime on PSP caches sceKernelGetThreadId() into a BSS slot during
 * _init_libc and every subsequent guarded syscall (__cxa_guard_acquire, _malloc_r,
 * atexit, __assert) compares the current thread's UID against that cached value. When
 * they do not match it prints "libc:%s: should be called from main thread".
 *
 * At the moment that fires we want the *exact* combination that broke the check:
 *   - cur_uid the scheduler reports right now (gets cached via s_cur by the cooperative
 *     scheduler; can be 0 if the assertion runs during a transitional window)
 *   - the cached BSS slots the libc/libgcc helpers read (just past sr_load_segment's
 *     reported end at 0x0030a020 -- libc_main_thid sits in this tail)
 *   - the most recent allocation address handed out, so a BSS-aliasing alloc is
 *     immediately visible in the log.
 *
 * Snapshotting wraps a single fprintf so the trace stays readable; one struct per fire. */
typedef struct {
    uint32_t pc, ra;
    uint32_t a0, a1;            /* printf format msg ptr at A0 + libc fn-name at A1 */
    uint32_t cur_uid;
    uint32_t bss_tail[12];      /* [0x0030a020..0x0030a04c], eight core slots */
    uint32_t bss_main_ids[3];   /* [0x0030a054..0x0030a05c], suspect libc_main_thid probes */
    uint32_t bss_higher[3];     /* [0x0031a03c..0x0031a044], gp-relative frame table head */
    uint32_t heap_bump_ptr;     /* last s_heap value user_partition_init / alloc_block set */
    uint32_t heap_alloc_addr;   /* most recent rounded-up allocation address */
} MainThreadDiag;

static MainThreadDiag sr_last_mt_diag;
static uint32_t sr_last_alloc_addr = 0;     /* last heap allocation rounded address */

/* Run as: sr_capture_mainthread_diag(s, &sr_last_mt_diag); re-used by both the printf
 * and ExitThread hooks so a second snapshot lands beside the first without another struct. */
static MainThreadDiag *sr_capture_mainthread_diag(CpuState *s, MainThreadDiag *d) {
    uint32_t sp = s->r[29];
    d->pc  = s->pc;
    d->ra  = MEM_R32(sp + 4u);
    d->a0  = s->r[4];
    d->a1  = s->r[5];
    d->cur_uid = sched_current_uid();
    d->bss_tail[ 0] = MEM_R32(0x0030a020u); d->bss_tail[ 1] = MEM_R32(0x0030a024u);
    d->bss_tail[ 2] = MEM_R32(0x0030a028u); d->bss_tail[ 3] = MEM_R32(0x0030a02cu);
    d->bss_tail[ 4] = MEM_R32(0x0030a030u); d->bss_tail[ 5] = MEM_R32(0x0030a034u);
    d->bss_tail[ 6] = MEM_R32(0x0030a038u); d->bss_tail[ 7] = MEM_R32(0x0030a03cu);
    d->bss_tail[ 8] = MEM_R32(0x0030a040u); d->bss_tail[ 9] = MEM_R32(0x0030a044u);
    d->bss_tail[10] = MEM_R32(0x0030a048u); d->bss_tail[11] = MEM_R32(0x0030a04cu);
    d->bss_main_ids[0] = MEM_R32(0x0030a054u);
    d->bss_main_ids[1] = MEM_R32(0x0030a058u);
    d->bss_main_ids[2] = MEM_R32(0x0030a05cu);
    d->bss_higher[0] = MEM_R32(0x0031a03cu);
    d->bss_higher[1] = MEM_R32(0x0031a040u);
    d->bss_higher[2] = MEM_R32(0x0031a044u);
    d->heap_bump_ptr   = user_partition_last_heap();
    d->heap_alloc_addr = sr_last_alloc_addr;
    return d;
}

static void sr_dump_mainthread_diag(const char *prefix, const MainThreadDiag *d) {
    fprintf(stderr,
        "==%s== pc=0x%08x ra=0x%08x a0=0x%08x a1=0x%08x cur_uid=0x%x\n"
        "  bss[0x0030a000..+0x10]=%08x %08x %08x %08x\n"
        "  bss[0x0030a010..+0x10]=%08x %08x %08x %08x\n"
        "  bss[0x0030a020..+0x10]=%08x %08x %08x %08x\n"
        "  bss[0x0030a030..+0x10]=%08x %08x %08x %08x\n"
        "  bss[0x0030a040..+0x10]=%08x %08x %08x %08x  /* guest module/EH-metadata registry */\n"
        "  bss[0x0030a054..+0x0c]=%08x %08x %08x\n"
        "  bss[0x0031a03c..+0x0c]=%08x %08x %08x  /* gp-relative frame table head */\n"
        "  heap_bump_ptr=0x%08x heap_last_alloc=0x%08x %s\n",
        prefix,
        d->pc, d->ra, d->a0, d->a1, d->cur_uid,
        MEM_R32(0x0030a000u), MEM_R32(0x0030a004u), MEM_R32(0x0030a008u), MEM_R32(0x0030a00cu),
        MEM_R32(0x0030a010u), MEM_R32(0x0030a014u), MEM_R32(0x0030a018u), MEM_R32(0x0030a01cu),
        d->bss_tail[0], d->bss_tail[1], d->bss_tail[2], d->bss_tail[3],
        d->bss_tail[4], d->bss_tail[5], d->bss_tail[6], d->bss_tail[7],
        d->bss_tail[8], d->bss_tail[9], d->bss_tail[10], d->bss_tail[11],
        d->bss_main_ids[0], d->bss_main_ids[1], d->bss_main_ids[2],
        d->bss_higher[0], d->bss_higher[1], d->bss_higher[2],
        d->heap_bump_ptr, d->heap_alloc_addr,
        (d->heap_alloc_addr < sr_loaded_end())
            ? " <-- last alloc overlaps the loaded image/BSS!" : "");
    if (d->bss_tail[8]) {
        fprintf(stderr, "  deref[0x0030a040] -> MEM[0x%08x] = 0x%08x\n", d->bss_tail[8], MEM_R32(d->bss_tail[8]));
    }
    if (d->bss_tail[9]) {
        fprintf(stderr, "  deref[0x0030a044] -> MEM[0x%08x] = 0x%08x\n", d->bss_tail[9], MEM_R32(d->bss_tail[9]));
    }
}

/* Start the user partition after the complete flat image, including zero-filled BSS.
 * tools/prxload.py preserves the original ~PSP header's segment-memory sizes when a
 * stripped ELF has lost them. An explicit override may move the base higher, but may
 * never overlap the loaded module. */
static void user_partition_init(void) {
    if (s_heap) return;
    const char *eb = getenv("SR_HEAP_BASE");
    const char *et = getenv("SR_PARTITION_TOP");
    uint32_t loaded_end = sr_loaded_end();
    uint32_t minimum_base = (loaded_end + 0xFFFu) & ~0xFFFu;
    uint32_t base = eb ? (uint32_t)strtoul(eb, NULL, 16) : minimum_base;
    if (loaded_end == 0u || minimum_base < loaded_end) {
        fprintf(stderr, "user_partition_init: invalid loaded image end 0x%08x\n", loaded_end);
        abort();
    }
    if (base < minimum_base) {
        fprintf(stderr,
                "user_partition_init: SR_HEAP_BASE 0x%08x overlaps loaded image/BSS "
                "ending at 0x%08x (minimum 0x%08x)\n",
                base, loaded_end, minimum_base);
        abort();
    }
    s_heap = base;
    s_part_top = et ? (uint32_t)strtoul(et, NULL, 16) : 0x0A000000u;
    if (s_part_top <= s_heap) {
        fprintf(stderr,
                "user_partition_init: partition top 0x%08x is not above heap base 0x%08x\n",
                s_part_top, s_heap);
        abort();
    }
    fprintf(stderr,
            "user_partition_init: loaded_end=0x%08x heap_base=0x%08x top=0x%08x\n",
            loaded_end, s_heap, s_part_top);
    s_heap_last_bump = s_heap;
}

/* Partition metadata is kernel-owned. sceKernelGetBlockHeadAddr returns the first
 * caller-usable byte, so keep bookkeeping host-side: retail newlib immediately
 * writes malloc chunk metadata at the start of its UserSbrk block. */
typedef struct { uint32_t uid, addr, size, prev, next; } Block;
static Block s_blocks[256];
static int s_nblocks = 0;

static uint32_t alloc_block(uint32_t size) {
    static int trace = -1, guard = -1;
    static uint32_t max_req = 0;
    if (trace < 0) trace = getenv("SR_ALLOC_TRACE") ? 1 : 0;
    if (guard < 0) guard = getenv("SR_ALLOC_GUARD") ? (getenv("SR_ALLOC_GUARD")[0]!='0'?1:0) : 1;
    if (!max_req) {
        const char *em = getenv("SR_ALLOC_MAX");
        /* Retail requests 0x01340000 bytes for UserSbrk. Partition bounds are
         * the default authority; SR_ALLOC_MAX is only an optional stricter cap. */
        max_req = em ? (uint32_t)strtoul(em, NULL, 16) : 0xFFFFFFFFu;
    }
    user_partition_init();
    /* Guard: reject impossible sizes or overflow past partition top. Real PSP returns an
     * alloc-failure sentinel; bumping past partition top corrupts every later block (e.g. the
     * 0x56E00000 request that pushed heap to 0x57d08bc0 and spun thread 0x115 at 0x6ea40). */
    uint32_t aligned = (s_heap + 0xFFu) & ~0xFFu;
    int overflow = (size == 0) || (size > max_req) ||
                   (aligned + size < aligned) || (aligned + size > s_part_top);
    if (overflow && guard) {
        fprintf(stderr, "ALLOC_BLOCK_REJECT: size=%u (0x%x) heap=0x%08x top=0x%08x max=0x%08x\n",
                size, size, s_heap, s_part_top, max_req);
        fflush(stderr);
        return 0xFFFFFFFFu;  /* PSP alloc-failure sentinel */
    }
    uint32_t addr = aligned;   /* 256-byte align */
    sr_last_alloc_addr = addr;
    uint32_t prev = 0xFFFFFFFFu;                  /* nobody ahead of us in the freelist */
    uint32_t next = 0u;
    /* Preserve the diagnostic chain entirely host-side. */
    for (int i = s_nblocks - 1; i >= 0; --i) {
        if (s_blocks[i].uid != 0u && s_blocks[i].addr != 0u) {
            prev = s_blocks[i].addr;
            s_blocks[i].next = addr;
            break;
        }
    }
    s_heap = addr + size;
    s_heap_last_bump = s_heap;
    uint32_t uid = sr_alloc_uid();
    if (s_nblocks < 256) {
        s_blocks[s_nblocks].uid   = uid;
        s_blocks[s_nblocks].addr  = addr;
        s_blocks[s_nblocks].size  = size;
        s_blocks[s_nblocks].prev  = prev;
        s_blocks[s_nblocks].next  = next;
        s_nblocks++;
    }
    /* Telemetry: emit structured line so WebUI /api/recomp/allocs can parse the live block chain.
     * Format: ALLOC_BLOCK uid=0x%x addr=0x%08x size=0x%x prev=0x%08x next=0x%08x fl=%d
     * Gated on SR_POSTUMD so release/perf runs are not affected. */
    if (getenv("SR_POSTUMD")) {
        fprintf(stderr, "ALLOC_BLOCK: uid=0x%x addr=0x%08x size=0x%x prev=0x%08x next=0x%08x fl=0\n",
                uid, addr, size, prev, next);
        fflush(stderr);
    }
    return uid;
}
static uint32_t block_addr(uint32_t uid) {
    for (int i = 0; i < s_nblocks; i++) if (s_blocks[i].uid == uid) return s_blocks[i].addr;
    return 0;
}

/* ---- handlers ---- */

static uint32_t g_sdk_version = 0;

static uint32_t h_SetCompiledSdkVersion(CpuState *s) { return sr_sdkver_set(&g_sdk_version, A0); }

/* sceUtilityGetSystemParamInt(id, int *out): write the system setting and return 0. PPSSPP's
 * defaults (Core/HLE/sceUtility.cpp registry): English (1), Western button order, 24h clock.
 * A no-op that leaves *out untouched makes the game read garbage for the language and load the
 * wrong region assets. IDs follow PSP_SYSTEMPARAM_ID_INT_*. */
static uint32_t h_GetSystemParamInt(CpuState *s) {
    uint32_t id = A0, out = A1, v;
    switch (id) {
        case 2:  v = 1;  break;   /* ADHOC_CHANNEL: automatic */
        case 3:  v = 0;  break;   /* WLAN_POWERSAVE: off */
        case 4:  v = 1;  break;   /* DATE_FORMAT: MMDDYYYY */
        case 5:  v = 0;  break;   /* TIME_FORMAT: 24h */
        case 6:  v = 0;  break;   /* TIMEZONE offset (minutes) */
        case 7:  v = 0;  break;   /* DAYLIGHTSAVINGS: off */
        case 8:  v = 1;  break;   /* LANGUAGE: English (PPSSPP default) */
        case 9:  v = 1;  break;   /* BUTTON_PREFERENCE: cross = enter (Western) */
        default: v = 1;  break;   /* safe default */
    }
    if (out) MEM_W32(out, v);
    if (hle_log_on()) fprintf(stderr, "sceUtilityGetSystemParamInt: id=%u -> %u\n", id, v);
    return 0;
}
/* sceUtilityGetSystemParamString(id, char *out, int len): nickname etc. Write a short ASCII name. */
/* sceCtrlGetIdleCancelThreshold(int *idlereset, int *idleback): both thresholds "disabled". */
static uint32_t h_CtrlGetIdleCancelThreshold(CpuState *s) {
    if (A0) MEM_W32(A0, 0xFFFFFFFFu);   /* -1 = idle cancel disabled (PPSSPP default) */
    if (A1) MEM_W32(A1, 0xFFFFFFFFu);
    return 0;
}

static void guest_cstr(uint32_t addr, char *out, int max);

/* SysMemUserForUser */
static uint32_t h_AllocPartitionMemory(CpuState *s) {
    /* a0=partition, a1=name, a2=type, a3=size, [sp+16]=addr. Returns a block UID. */
    char name[64]; guest_cstr(A1, name, sizeof(name));
    uint32_t size = A3;
    uint32_t uid = alloc_block(size ? size : 16);
    fprintf(stderr, "  -> uid=0x%x addr=0x%08x (heap_bump_now=0x%08x)\n",
            uid, sr_last_alloc_addr, s_heap);
    return uid;
}
static uint32_t h_GetBlockHeadAddr(CpuState *s) { return block_addr(A0); }
static uint32_t h_FreePartitionMemory(CpuState *s) {
    uint32_t uid = A0;
    for (int i = 0; i < s_nblocks; i++) {
        if (s_blocks[i].uid == uid && s_blocks[i].addr != 0) {
            /* Kernel metadata is host-side. Never scribble a synthetic free header
             * into memory returned to the guest. Interior blocks are not compacted. */
            uint32_t addr = s_blocks[i].addr;
            s_blocks[i].uid = 0;                  /* slot now free */
            s_blocks[i].addr = 0;
            s_blocks[i].size = 0;
            s_blocks[i].prev = 0;
            s_blocks[i].next = 0;
            if (getenv("SR_ALLOC_TRACE")) {
                fprintf(stderr, "FreePartitionMemory: uid=0x%x addr=0x%08x marked free\n", uid, addr);
            }
            return 0;
        }
    }
    /* Block not found â€” could be an invalid UID. PSP returns error. */
    if (getenv("SR_ALLOC_TRACE")) fprintf(stderr, "FreePartitionMemory: uid=0x%x not found\n", uid);
    return 0x80020000;
}
static uint32_t partition_free(void) {
    user_partition_init();
    return s_heap < s_part_top ? s_part_top - s_heap : 0u;
}
/* A small accounting tail (the kernel keeps block headers per allocation) so the figure is not
 * the exact arithmetic free; PPSSPP reports e.g. 0x1a0b00 with several blocks live. */
static uint32_t h_TotalFreeMemSize(CpuState *s) { (void)s; uint32_t f = partition_free(); return f > (uint32_t)s_nblocks*0x100u ? f - (uint32_t)s_nblocks*0x100u : f; }
/* sceKernelMaxFreeMemSize: returns the largest contiguous free block. The bump allocator
 * only has one truly contiguous free region: from the current s_heap up to s_part_top.
 * The freelist blocks are scattered and not necessarily contiguous. Return that single
 * remaining chunk rather than the sum of all free blocks (which is what TotalFreeMemSize
 * approximates). */
static uint32_t h_MaxFreeMemSize(CpuState *s) { (void)s; user_partition_init(); return s_heap < s_part_top ? s_part_top - s_heap : 0u; }

/* Fixed Pool (FPL) â€” simple bump allocator per pool.  Enough for games that use FPL
 * to allocate objects whose constructors populate vtables (e.g. sceUtility dialogs). */
#define FPL_MAX 16
typedef struct { uint32_t base; uint32_t cur; uint32_t end; uint32_t bsize; int used; } FplPool;
static FplPool s_fpls[FPL_MAX];
static uint32_t h_CreateFpl(CpuState *s) {
    /* a0=name, a1=partition, a2=attr, a3=blockSize. 5th arg (numBlocks) is in t0 (r8). */
    uint32_t bsize = A3;
    uint32_t nblocks = stack_arg(s, 0);
    if (bsize == 0) bsize = 16;
    if (nblocks == 0) nblocks = 1;
    uint32_t total = bsize * nblocks;
    uint32_t block_uid = alloc_block(total ? total : 16);
    uint32_t pool = block_addr(block_uid);
    uint32_t uid = 0;
    for (int i = 0; i < FPL_MAX; i++) { if (!s_fpls[i].used) { uid = (uint32_t)(i + 0x500); s_fpls[i] = (FplPool){pool, pool, pool + total, bsize, 1}; break; } }
    fprintf(stderr, "sceKernelCreateFpl: uid=0x%x pool_uid=0x%x base=0x%08x bsize=%u nblocks=%u total=%u\n", uid, block_uid, pool, bsize, nblocks, total);
    return uid;
}
static uint32_t h_TryAllocateFpl(CpuState *s) {
    /* a0=fplUid, a1=dataPtrOut. Returns 0 on success. */
    uint32_t uid = A0, out = A1;
    uint32_t idx = uid - 0x500;
    if (idx >= FPL_MAX || !s_fpls[idx].used) { fprintf(stderr, "sceKernelTryAllocateFpl: bad uid=0x%x\n", uid); return 0x800200d3; }
    FplPool *p = &s_fpls[idx];
    if (p->cur + p->bsize > p->end) { fprintf(stderr, "sceKernelTryAllocateFpl: pool 0x%x exhausted\n", uid); return 0x800200d9; }
    uint32_t addr = p->cur; p->cur += p->bsize;
    if (out) MEM_W32(out, addr);
    fprintf(stderr, "sceKernelTryAllocateFpl: uid=0x%x -> 0x%08x (cur=0x%08x)\n", uid, addr, p->cur);
    return 0;
}
/* sceKernelAllocateFpl / ...CB are the BLOCKING allocate forms, so unlike
 * sceKernelTryAllocateFpl they are subject to the interrupt/dispatch context rule.
 * waits.expected puts the context decision ahead of the FPL object lookup: a bad
 * fpl id answers CAN_NOT_WAIT (L102/L103, L108/L109) rather than the bad-id error,
 * and so does a valid pool that could have satisfied the request immediately
 * (L104/L105, L110/L111). One leading check therefore covers all four cells, and
 * because it returns before h_TryAllocateFpl() runs, a rejected call performs no
 * part of the operation -- no block leaves the pool and no output pointer is
 * written.
 *
 * This is only a split of the blocking form away from the non-blocking one; the
 * two shared a handler before. sceKernelTryAllocateFpl keeps its own registration
 * straight to h_TryAllocateFpl and is deliberately untouched: it does not block,
 * so the context rule does not apply to it, and HST imports that NID rather than
 * these. In normal context sched_wait_permitted() is true and the call is
 * byte-for-byte what it was. FPL reclamation, exhaustion-blocking, waiter queues
 * and timeouts remain #16's. */
static uint32_t h_AllocateFpl(CpuState *s) {
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    return h_TryAllocateFpl(s);
}
static uint32_t h_DeleteFpl(CpuState *s) {
    uint32_t uid = A0; uint32_t idx = uid - 0x500;
    if (idx < FPL_MAX && s_fpls[idx].used) s_fpls[idx].used = 0;
    return 0;
}
static uint32_t h_FreeFpl(CpuState *s) { (void)s; return 0; }

/* ThreadManForUser, backed by the fiber scheduler (src/rt/sched.c). */
static uint32_t h_CreateThread(CpuState *s) {
    /* a0=name, a1=entry, a2=priority, a3=stackSize. Returns a UID bound to the entry.
     * sched_create_thread returns 0 when the TCB table or the stack arena is exhausted;
     * real PSP fails such a create instead of granting a smaller stack. */
    uint32_t uid = sched_create_thread(A1, (int)A2, A3);
    return uid ? uid : 0x80020190u;   /* SCE_KERNEL_ERROR_NO_MEMORY */
}
static uint32_t h_StartThread(CpuState *s) {
    /* a0=thid, a1=arglen, a2=argp. Mark ready, then preempt if it is higher priority -- PSP
     * runs a higher-priority started thread immediately. */
    uint32_t thid = A0;
    uint32_t result = sched_start_thread(thid, A1, A2);
    /* Historical "libc_main_thid" re-seed -- REMOVED. Statically and dynamically proven
     * to be part of the guest module/EH-metadata registry, not a thread ID scalar. */
    if (result == 0) sched_preempt();
    return result;
}
/* Forward decls of hle-internal diagnostic helpers (defined below). Used by h_ExitThread. */
extern void sr_postumd_advance(int active_after);
extern void sr_postumd_signal_shutdown(uint64_t captured_count);
extern uint64_t sr_postumd_reads(void);
extern void sr_exitsnap_capture(CpuState *s);
extern void sr_exitsnap_dump_latest(const char *tag);
static int s_exit_delete_request;
static uint32_t h_ExitThread(CpuState *s) {
    /* sceKernelExitThread(status): TERMINAL for the calling thread, unconditionally --
     * sched_exit_current() at the bottom records the status, releases WaitThreadEnd
     * joiners, unregisters the libc thread state, marks the TCB DORMANT, and switches
     * away; control never returns to the guest caller. Historical builds carried
     * launcher/worker "survive their own exit" bypasses here (return 0 and keep the
     * caller running); those are GONE -- the earlier comments describing them were stale.
     * Do not reintroduce a survival path: a route that deadlocks with no runnable
     * threads after a legitimate exit has a bug in whatever failed to keep its awaited
     * state alive, not in the exit itself.
     *
     * death_wish: if h_KernelPrintf just saw the libc reentrancy guard fire ("should be
     * called from main thread" / "no reent structure"), the dying thread's libc state is
     * already corrupt and its _exit() path is unwinding -- exit the host process instead
     * of scheduling into a broken world. The root and launcher threads are exempt: their
     * exit is part of normal boot teardown, not a libc failure. The exemption is a ROLE
     * test, not a UID-number test: a build with no launcher binding exempts nothing, so
     * an ordinary thread cannot inherit the exemption by its allocated number. */
    extern int sr_libc_death_wish;     /* defined alongside h_ExitGame below */
    int death_wish = sr_libc_death_wish;
    uint32_t uid = sched_current_uid();
    if (sched_uid_is_root(uid) || sched_uid_is_launcher(uid)) {
        death_wish = 0;
    }
    /* Trace call stack via RA chain */
    fprintf(stderr, "TRACE_EXIT: uid=0x%x pc=0x%08x ra=0x%08x ra2=0x%08x ra3=0x%08x\n",
            uid, s->pc, s->r[31],
            s->r[29] ? MEM_R32(s->r[29] + 0x0c) : 0,
            s->r[29] ? MEM_R32(s->r[29] + 0x10) : 0);
    fflush(stderr);
    /* ExitThread terminates only the caller. Joiners are released by
     * sched_exit_current(); waking an unrelated sleeping thread here fabricates a
     * sceKernelWakeupThread that the guest never issued. In HST that compatibility
     * shortcut let a short-lived character-resource worker wake the launcher, whose
     * teardown then destroyed the singleton beneath a sibling worker (issue #126).
     * Keep the env-gated primary-worker diagnostics independent of guest scheduling. */
    if (sched_uid_is_worker(uid)) {
        sr_exitsnap_capture(s);
        sr_exitsnap_dump_latest("worker-exit-pre");
        sr_postumd_signal_shutdown(sr_postumd_reads());
    }
    fprintf(stderr, "HLE: ExitThread cur_uid=0x%x ra=0x%08x (death_wish=%d)\n",
            uid, s->r[31], death_wish);
    fprintf(stderr,
            "  re-snapshot: cur_uid=0x%x libc_main_id[0x0030a040]=0x%08x [0x0030a058]=0x%08x "
            "frame_head[0x0031a03c]=0x%08x last_alloc=0x%08x\n",
            sched_current_uid(),
            MEM_R32(0x0030a040u), MEM_R32(0x0030a058u),
            MEM_R32(0x0031a03cu), sr_last_alloc_addr);
    fprintf(stderr,
            "  regs uid=0x%x: pc=0x%08x sp=0x%08x gp=0x%08x k0=0x%08x k0+4=0x%08x k0+0x38c=0x%08x "
            "v0=0x%08x a0=0x%08x t0..t3=0x%08x/0x%08x/0x%08x/0x%08x s0..s3=0x%08x/0x%08x/0x%08x/0x%08x\n",
            uid, s->pc, s->r[29], s->r[28], s->r[26], s->r[26] ? MEM_R32(s->r[26] + 4) : 0, s->r[26] ? MEM_R32(s->r[26] + 0x38c) : 0,
            s->r[2], s->r[4], s->r[8], s->r[9], s->r[10], s->r[11],
            s->r[16], s->r[17], s->r[18], s->r[19]);
    if (death_wish) {
        /* _exit: skip host atexit handlers (libc has already burned -- the death wish
         * is unconditional at this point). stderr flush so the trace survives. */
        fprintf(stderr, "GAMELOG: Guest requested exit (libc guard chain)\n");
        fflush(stderr);
        _exit(1);
    }
    int delete_request = s_exit_delete_request;
    s_exit_delete_request = 0;
    if (delete_request)
        sched_exit_current_delete((int32_t)A0);
    else
        sched_exit_current((int32_t)A0);
    return 0;
}
static uint32_t h_ExitDeleteThread(CpuState *s) {
    /* Share the terminal diagnostics and death-wish handling with ExitThread,
     * but select the delete-at-exit lifecycle operation at its final handoff. */
    s_exit_delete_request = 1;
    return h_ExitThread(s);
}
/* A delay is unconditionally a wait: sched_delay_current() parks the thread even for
 * usec 0 (it floors the duration at 1). There is no parameter or object to validate
 * ahead of the context check, so it is the first thing the handler does -- nothing has
 * been mutated at that point: no wake deadline, no thread state, no yield. (L2/L3) */
static uint32_t h_DelayThread(CpuState *s) {
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sched_delay_current(A0);
    return 0;
}
static uint32_t h_DelayThreadCB(CpuState *s) {
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;  /* L6/L7 */
    uint32_t usec = A0;
    uint32_t thread_uid = sched_current_uid();
    sched_vtime_refresh();
    uint64_t end_time = sched_vtime_deadline_after(usec);

    while (sched_vtime_us() < end_time) {
        if (sr_thread_has_pending_callbacks(thread_uid)) {
            sr_thread_dispatch_callbacks();
            sched_vtime_refresh();
            continue;
        }
        uint64_t remaining = end_time - sched_vtime_us();
        sched_set_current_cb_wait(1);
        sched_delay_current((uint32_t)remaining);
        sched_set_current_cb_wait(0);
        sched_vtime_refresh();
    }
    return 0;
}
static uint32_t h_ChangeThreadPriority(CpuState *s) { sched_set_priority(A0, (int)A1); return 0; }
static uint32_t h_TerminateDeleteThread(CpuState *s) {
    uint32_t result = sched_terminate_thread(A0);
    if (result != 0) return result;
    return sched_delete_thread(A0);
}
static uint32_t h_DeleteThread(CpuState *s) {
    return sched_delete_thread(A0);
}
static uint32_t h_GetThreadIdSched(CpuState *s) { (void)s; return sched_current_uid(); }
static uint32_t h_GetThreadPriority(CpuState *s) { (void)s; return (uint32_t)sched_current_priority(); }
static uint32_t h_GetThreadExitStatus(CpuState *s) { return sched_thread_exit_status(A0); }
static uint32_t h_GetThreadStackFreeSize(CpuState *s) {
    /* Walk from SP upward until we hit the stack sentinel (0xDEADBEEF). */
    uint32_t sp = s->r[29];
    uint32_t free = 0;
    for (uint32_t a = sp; a < sp + 65536; a += 4) {
        if (MEM_R32(a) == 0xDEADBEEF) { free = a - sp; break; }
    }
    if (!free) free = 0x1000;
    return free;
}
/* sceKernelSleepThread[CB] blocks only when no wakeup is banked; a pending wakeup is
 * consumed and the call returns without waiting. The context check therefore asks the
 * wakeup count first, so a rejected call decrements nothing, sets no sleep marker, and
 * does not yield -- and a sleep that would have been satisfied immediately keeps
 * working with interrupts or dispatch disabled. (L18/L19, L22/L23) */
static uint32_t h_SleepThread(CpuState *s) {
    (void)s;
    uint32_t uid = sched_current_uid();
    if (getenv("SR_WAKELOG")) {
        static int n=0;
        if (n++<8) fprintf(stderr, "SLEEP uid=0x%x\n", uid);
    }
    if (!sched_current_has_pending_wakeup() && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sched_thread_sleep();
    return 0;
}
static uint32_t h_SleepThreadCB(CpuState *s) {
    (void)s;
    uint32_t uid = sched_current_uid();
    if (getenv("SR_WAKELOG")) {
        static int n=0;
        if (n++<8) fprintf(stderr, "SLEEP_CB uid=0x%x\n", uid);
    }
    if (!sched_current_has_pending_wakeup() && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sched_thread_sleep_cb();
    return 0;
}
static uint32_t h_WakeupThread(CpuState *s) {
    if (getenv("SR_WAKELOG")) { static int n=0; if (n++<8) fprintf(stderr, "WAKEUP target=0x%x (from 0x%x)\n", A0, sched_current_uid()); }
    uint32_t result = sched_thread_wakeup(A0);
    if (result == 0) sched_preempt();
    return result;
}
static uint32_t h_CancelWakeupThread(CpuState *s) {
    int old = sched_thread_cancel_wakeup(A0);
    if (getenv("SR_WAKELOG")) { static int n=0; if (n++<8) fprintf(stderr, "CANCEL_WAKE target=0x%x old=%d (from 0x%x)\n", A0, old, sched_current_uid()); }
    return old < 0 ? 0x800201a0u : (uint32_t)old;
}
static uint32_t h_wait_thread_status(uint32_t uid) {
    uint32_t result = 0;
    if (sched_take_current_join_result(uid, &result)) return result;
    return sched_thread_exit_status(uid);
}
/* Would a join on `uid` actually have to block? Answered without side effects, so a
 * handler that is about to reject the call does not consume the banked join result
 * that h_wait_thread_status() would take. A target that is anything other than
 * NOT_DORMANT (0x800201a4) resolves the join immediately. */
static int h_wait_thread_would_block(uint32_t uid) {
    if (sched_current_join_result_pending(uid)) return 0;
    return sched_thread_exit_status(uid) == 0x800201A4u;
}
static uint32_t h_WaitThreadEnd(CpuState *s) {
    uint32_t uid = A0;
    uint32_t toptr = A1;
    uint32_t self = sched_current_uid();
    if (uid == 0 || uid == self) return 0x80020197u;

    /* ILLEGAL_THID above wins over the context restriction on hardware in every
     * context (L204/L205/L383), so the object check stays first. Only once the
     * target is established to still be running is this a genuine wait. Rejecting
     * here touches no join target, no timeout word and no wait state. (L208/L209) */
    if (h_wait_thread_would_block(uid) && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;

    uint32_t status = h_wait_thread_status(uid);
    if (status != 0x800201A4u) return status;

    if (!toptr) {
        sched_set_current_join_target(uid);
        while ((status = h_wait_thread_status(uid)) == 0x800201A4u)
            sched_block_on(uid);
        sched_clear_current_join_target();
        return status;
    }

    sched_vtime_refresh();
    uint64_t deadline = sched_vtime_deadline_after(MEM_R32(toptr));
    for (;;) {
        status = h_wait_thread_status(uid);
        if (status != 0x800201A4u) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            MEM_W32(toptr, now < deadline ? (uint32_t)(deadline - now) : 0u);
            return status;
        }
        sched_vtime_refresh();
        uint64_t now = sched_vtime_us();
        if (now >= deadline) {
            MEM_W32(toptr, 0);
            return 0x800201A8u;
        }
        uint32_t remaining = (uint32_t)(deadline - now);
        sched_set_current_join_target(uid);
        if (sched_block_on_timeout(uid, remaining)) {
            sched_clear_current_join_target();
            MEM_W32(toptr, 0);
            return 0x800201A8u;
        }
    }
}
static uint32_t h_WaitThreadEndCB(CpuState *s) {
    uint32_t uid = A0;
    uint32_t toptr = A1;
    uint32_t self = sched_current_uid();
    if (uid == 0 || uid == self) return 0x80020197u;

    /* Rejected before the callback dispatch below, not after: a call that returns
     * CAN_NOT_WAIT must not have run guest callback code on its way out. (L216/L217) */
    if (h_wait_thread_would_block(uid) && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;

    /* CB waits process callbacks even if the target was already dormant when the
     * syscall began. Hardware leaves the timeout untouched in that immediate case. */
    if (sr_thread_has_pending_callbacks(self))
        sr_thread_dispatch_callbacks();

    uint32_t status = h_wait_thread_status(uid);
    if (status != 0x800201A4u) return status;

    uint64_t deadline = 0;
    if (toptr) {
        sched_vtime_refresh();
        deadline = sched_vtime_deadline_after(MEM_R32(toptr));
    }

    for (;;) {
        if (sr_thread_has_pending_callbacks(self)) {
            sr_thread_dispatch_callbacks();
            status = h_wait_thread_status(uid);
            if (status != 0x800201A4u) {
                if (toptr) {
                    sched_vtime_refresh();
                    uint64_t now = sched_vtime_us();
                    MEM_W32(toptr, now < deadline ? (uint32_t)(deadline - now) : 0u);
                }
                return status;
            }
        }

        if (toptr) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= deadline) {
                MEM_W32(toptr, 0);
                return 0x800201A8u;
            }
            uint32_t remaining = (uint32_t)(deadline - now);
            sched_set_current_join_target(uid);
            sched_set_current_cb_wait(1);
            int timed_out = sched_block_on_timeout(uid, remaining);
            sched_set_current_cb_wait(0);
            if (timed_out) {
                sched_clear_current_join_target();
                MEM_W32(toptr, 0);
                return 0x800201A8u;
            }
        } else {
            sched_set_current_join_target(uid);
            sched_set_current_cb_wait(1);
            sched_block_on(uid);
            sched_set_current_cb_wait(0);
        }

        status = h_wait_thread_status(uid);
        if (status != 0x800201A4u) {
            if (toptr) {
                sched_vtime_refresh();
                uint64_t now = sched_vtime_us();
                MEM_W32(toptr, now < deadline ? (uint32_t)(deadline - now) : 0u);
            }
            return status;
        }
    }
}
static uint32_t h_ReferThreadRunStatus(CpuState *s) {
    uint32_t out = A1;
    if (out) {
        SrThreadRunStatus rs;
        int rc = sched_thread_run_status(A0, &rs);
        if (rc < 0) return 0x800201a0u;
        for (uint32_t i = 0; i < 0x2c; i++) MEM_W8(out + i, 0);
        MEM_W32(out + 0x00, rs.size);
        MEM_W32(out + 0x04, rs.status);
        MEM_W32(out + 0x08, rs.currentPriority);
        MEM_W32(out + 0x0c, rs.waitType);
        MEM_W32(out + 0x10, rs.waitId);
        MEM_W32(out + 0x14, rs.wakeupCount);
        MEM_W32(out + 0x18, rs.runClocksLow);
        MEM_W32(out + 0x1c, rs.runClocksHigh);
        MEM_W32(out + 0x20, rs.intrPreemptCount);
        MEM_W32(out + 0x24, rs.threadPreemptCount);
        MEM_W32(out + 0x28, rs.releaseCount);
        if (getenv("SR_WAKELOG")) { static int n=0; if (n++<12) fprintf(stderr,
            "REFER_RUN target=0x%x status=%u pri=%u waitType=%u waitId=0x%x wakeups=%u\n",
            A0, rs.status, rs.currentPriority, rs.waitType, rs.waitId, rs.wakeupCount); }
    }
    return 0;
}
/* scePower */
static uint32_t h_PowerGetBatteryLifePercent(CpuState *s) { (void)s; return 100; }
static uint32_t h_PowerIsBatteryCharging(CpuState *s) { (void)s; return 1; }
static uint32_t h_PowerIsBatteryExist(CpuState *s) { (void)s; return 1; }
static uint32_t h_PowerIsPowerOnline(CpuState *s) { (void)s; return 1; }
static uint32_t h_PowerGetCpuClockFrequencyInt(CpuState *s) { (void)s; return 333; }
static uint32_t h_PowerGetBusClockFrequencyInt(CpuState *s) { (void)s; return 166; }

static uint32_t s_power_cb_slots[16];

static uint32_t h_PowerRegisterCallback(CpuState *s) {
    int32_t slot = (int32_t)A0;
    uint32_t cb_uid = A1;

    if (slot < -1 || slot >= 32) return 0x80000102u;
    if (slot >= 16) return 0x80000023u;
    if (!sr_callback_is_valid(cb_uid)) return 0x80000100u;

    int32_t result = 0;
    if (slot == -1) {
        result = -1;
        for (int i = 0; i < 16; i++) {
            if (s_power_cb_slots[i] == 0) {
                s_power_cb_slots[i] = cb_uid;
                result = i;
                break;
            }
        }
        if (result < 0) return 0x80000022u;
    } else {
        if (s_power_cb_slots[slot] != 0) return 0x80000020u;
        s_power_cb_slots[slot] = cb_uid;
    }

    /* PSP hardware immediately notifies a newly registered callback. Re-registering
     * the same callback in another slot therefore increments its pending count. */
    (void)sr_callback_notify(cb_uid, 0x000010E4u);
    return (uint32_t)result;
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

static int valid_module_info(const uint8_t *mi, const SrElfPhdr *ph, unsigned phnum, size_t file_len) {
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

    return elf_vaddr_to_file(ph, phnum, ent_top, file_len) != UINT32_MAX;
}

/* Most stripped PRXs put the PspModuleInfo file offset in the executable PT_LOAD p_paddr.
 * A few Sony modules instead use that field for another resident-data address. Fall back to
 * the same conservative aligned scan used by the codegen analyzer, rather than interpreting
 * an export/NID table as module-info and silently publishing nothing. */
static uint32_t find_module_info(FILE *f, const SrElfPhdr *ph, unsigned phnum, size_t file_len, uint8_t mi[52]) {
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
                if ((uint64_t)off > (uint64_t)LONG_MAX || fseek(f, (long)off, SEEK_SET) || fread(mi, 1, 52, f) != 52) break;
                if (valid_module_info(mi, ph, phnum, file_len)) return off;
                if (UINT32_MAX - off < 4u) break;
                off += 4u;
            }
        }
    }
    return UINT32_MAX;
}

static unsigned register_prx_exports(const char *host_path, uint32_t base) {
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

    uint32_t mioff = find_module_info(f, ph, phnum, file_len, mi);
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
        uint32_t off = elf_vaddr_to_file(ph, phnum, ent, file_len);
        if (off == UINT32_MAX || (uint64_t)off + sizeof(e) > file_len || off > (uint32_t)LONG_MAX ||
            fseek(f, (long)off, SEEK_SET) || fread(e, 1, sizeof(e), f) != sizeof(e)) break;
        unsigned words = e[8], nvars = e[9]; uint16_t nfuncs; uint32_t table;
        memcpy(&nfuncs, e + 10, 2); memcpy(&table, e + 12, 4);
        unsigned count = (unsigned)nfuncs + nvars;
        if (words < 4 || words > 0x40 || count > 1024) break;
        uint32_t toff = elf_vaddr_to_file(ph, phnum, table, file_len);
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
#endif

/* PRX load bases. These MUST stay in sync with Makefile GAME_EXTRA_ELFS
 * (libfont.prx@0x32200000, scePsmf_library.prx@0x32280000,
 *  scePsmfP_library.prx@0x322f8868). Centralized here so the values are not
 * scattered as magic literals across the late-import path. */
static const uint32_t PRX_LIBFONT_BASE = 0x32200000u; /* must match Makefile GAME_EXTRA_ELFS */
static const uint32_t PRX_PSMF_BASE    = 0x32280000u; /* must match Makefile GAME_EXTRA_ELFS */
static const uint32_t PRX_PSMFP_BASE   = 0x322f8868u; /* must match Makefile GAME_EXTRA_ELFS */

static unsigned populate_known_module(const char *name) {
    if (name && (strstr(name, "libfont") || strstr(name, "LIBFONT")))
        return register_prx_exports("place_game_here/EXTRACTED/decrypted/libfont.prx", PRX_LIBFONT_BASE);
    if (name && (strstr(name, "PsmfP") || strstr(name, "psmfplayer") || strstr(name, "libpsmfplayer")))
        return register_prx_exports("place_game_here/EXTRACTED/decrypted/scePsmfP_library.prx", PRX_PSMFP_BASE);
    if (name && (strstr(name, "Psmf") || strstr(name, "psmf")))
        return register_prx_exports("place_game_here/EXTRACTED/decrypted/scePsmf_library.prx", PRX_PSMF_BASE);
    return 0;
}

/* sceUtility */
/* AV module IDs are firmware utility IDs, not aliases for the title's private
 * libfont/PSMF PRXs.  Keep this state separate from the explicit PRX loader
 * (h_LoadModule/h_LoadModuleByID), which is the only path that knows a PRX's
 * concrete export table and load address. */
enum {
    PSP_AV_MODULE_AVCODEC = 0x300,
    PSP_AV_MODULE_SASCORE = 0x301,
    PSP_AV_MODULE_ATRAC3PLUS = 0x302,
    PSP_AV_MODULE_MPEGBASE = 0x303,
    PSP_AV_MODULE_MP3 = 0x304,
    PSP_AV_MODULE_VAUDIO = 0x305,
    PSP_AV_MODULE_AAC = 0x306,
    PSP_AV_MODULE_G729 = 0x307,
    PSP_AV_MODULE_MP4 = 0x308,
};
#define SCE_ERROR_MODULE_BAD_ID             0x80111101u
#define SCE_ERROR_MODULE_ALREADY_LOADED     0x80111102u
#define SCE_ERROR_MODULE_NOT_LOADED         0x80111103u
#define SCE_ERROR_AV_MODULE_BAD_ID         0x80110f01u
#define SCE_ERROR_AV_MODULE_ALREADY_LOADED 0x80110f02u
#define SCE_ERROR_AV_MODULE_NOT_LOADED     0x80110f03u
#define SCE_ERROR_AV_LIBRARY_NOT_FOUND      0x8002013cu
static unsigned s_utility_av_loaded;

static int utility_av_index(uint32_t module) {
    return module >= PSP_AV_MODULE_AVCODEC && module <= PSP_AV_MODULE_MP4
               ? (int)(module - PSP_AV_MODULE_AVCODEC) : -1;
}

static const char *utility_av_name(int index) {
    static const char *const names[] = {
        "av_avcodec", "av_sascore", "av_atrac3plus", "av_mpegbase",
        "av_mp3", "av_vaudio", "av_aac", "av_g729", "av_mp4",
    };
    return index >= 0 && index < (int)(sizeof(names) / sizeof(names[0]))
               ? names[index] : "av_unknown";
}

static int utility_av_requires_codec(int index) {
    /* PPSSPP models ATRAC3+ and MPEGBASE as depending on AVCODEC.  Keep this
     * small dependency rule because the title loads AVCODEC before either path. */
    return index == (PSP_AV_MODULE_ATRAC3PLUS - PSP_AV_MODULE_AVCODEC) ||
           index == (PSP_AV_MODULE_MPEGBASE - PSP_AV_MODULE_AVCODEC) ||
           index == (PSP_AV_MODULE_MP4 - PSP_AV_MODULE_AVCODEC);
}

static uint32_t utility_av_load(uint32_t module, int av_api) {
    int index = utility_av_index(module);
    if (index < 0)
        return av_api ? SCE_ERROR_AV_MODULE_BAD_ID : SCE_ERROR_MODULE_BAD_ID;
    unsigned bit = 1u << (unsigned)index;
    if (s_utility_av_loaded & bit)
        return av_api ? SCE_ERROR_AV_MODULE_ALREADY_LOADED : SCE_ERROR_MODULE_ALREADY_LOADED;
    if (utility_av_requires_codec(index) &&
        !(s_utility_av_loaded & (1u << (PSP_AV_MODULE_AVCODEC - PSP_AV_MODULE_AVCODEC))))
        return SCE_ERROR_AV_LIBRARY_NOT_FOUND;
    s_utility_av_loaded |= bit;
    fprintf(stderr, "%s: recognized %s (AV module state loaded)\n",
            av_api ? "sceUtilityLoadAvModule" : "sceUtilityLoadModule",
            utility_av_name(index));
    fflush(stderr);
    return 0;
}

static uint32_t utility_av_unload(uint32_t module, int av_api) {
    int index = utility_av_index(module);
    if (index < 0)
        return av_api ? SCE_ERROR_AV_MODULE_BAD_ID : SCE_ERROR_MODULE_BAD_ID;
    unsigned bit = 1u << (unsigned)index;
    if (!(s_utility_av_loaded & bit))
        return av_api ? SCE_ERROR_AV_MODULE_NOT_LOADED : SCE_ERROR_MODULE_NOT_LOADED;
    s_utility_av_loaded &= ~bit;
    fprintf(stderr, "%s: recognized %s (AV module state unloaded)\n",
            av_api ? "sceUtilityUnloadAvModule" : "sceUtilityUnloadModule",
            utility_av_name(index));
    fflush(stderr);
    return 0;
}

static uint32_t h_UtilityLoadModule(CpuState *s) {
    uint32_t module = s->r[4];
    fprintf(stderr, "sceUtilityLoadModule: module=0x%08x\n", module);
    fflush(stderr);
    /* Utility AV loads are host-side module-state notifications.  Do not run
     * the title's private PRX module_start here: those entry points assume a
     * PSP kernel and are intentionally skipped.  The private export tables
     * are registered only by the concrete kernel PRX loader below. */
    /* A bare low byte is not a valid reason to publish one of the private
     * title PRX tables. Those exports are registered by the concrete kernel
     * module-load path instead. */
    return utility_av_load(module, 0);
}
static uint32_t h_UtilityUnloadModule(CpuState *s) {
    return utility_av_unload(s->r[4], 0);
}
static uint32_t h_UtilityLoadAvModule(CpuState *s) {
    uint32_t module = s->r[4];
    if (module > 7u) return SCE_ERROR_AV_MODULE_BAD_ID;
    return utility_av_load(PSP_AV_MODULE_AVCODEC + module, 1);
}
static uint32_t h_UtilityUnloadAvModule(CpuState *s) {
    uint32_t module = s->r[4];
    if (module > 7u) return SCE_ERROR_AV_MODULE_BAD_ID;
    return utility_av_unload(PSP_AV_MODULE_AVCODEC + module, 1);
}

static uint32_t h_ok(CpuState *s) { (void)s; return 0; }

/* 0x00061e74 is the game's newlib FILE write callback used by the module-processing log
 * stream. It is a valid MIPS function entry (jr ra; move v0,zero), but lies in the four-byte
 * gap between codegen's f_00061e4c and f_00061e7c discoveries and therefore misses sr_lookup.
 * The actual callback ABI is write(cookie, buffer, byte_count). Treat the diagnostic stream as
 * a sink and report the complete byte count; returning zero makes __sfvwrite_r retry/stall.
 * sr_syscall applies the standard caller-saved/HI/LO DEADBEEF poison after this returns. */
static uint32_t h_ModuleStreamWrite(CpuState *s) {
    uint32_t buf = s->r[4];
    if (sr_inrange(buf)) {
        char temp[512];
        int i;
        for (i = 0; i < 511; i++) {
            if (!sr_inrange(buf + i)) break;
            char c = (char)MEM_R8(buf + i);
            if (c == '\0') { temp[i] = '\0'; break; }
            temp[i] = c;
        }
        temp[i] = '\0';
        fprintf(stderr, "MODULE_LOG: %s\n", temp);
        fflush(stderr);
    }
    return s->r[6];
}
/* scePsmfPlayer structural model. The renderer/demux producer is intentionally separate: these
 * handlers validate the real guest control block, parse the PSMF header, and expose deterministic
 * lifecycle/queue state. Until a demux producer fills the queue matrix, data getters report the
 * documented NO_MORE_DATA result instead of pretending that a decoder succeeded. */
#define PSMF_STATUS_NONE 0u
#define PSMF_STATUS_INIT 1u
#define PSMF_STATUS_STANDBY 2u
#define PSMF_STATUS_PLAYING 4u
#define PSMF_STATUS_FINISHED 0x200u
#define PSMF_ERR_STATUS 0x80616001u
#define PSMF_ERR_STREAM 0x80616003u
#define PSMF_ERR_BUFSIZE 0x80616005u
#define PSMF_ERR_CONFIG 0x80616006u
#define PSMF_ERR_PARAM 0x80616008u
#define PSMF_ERR_NO_DATA 0x8061600cu
#define PSMF_ERR_ALREADY_INIT 0x80618005u
#define PSMF_Q_DEPTH 4
enum { PSMF_TRACK_VIDEO = 0, PSMF_TRACK_AUDIO = 1, PSMF_TRACKS = 2 };
enum { PSMF_Q_INPUT = 0, PSMF_Q_AU = 1, PSMF_Q_DECODED = 2, PSMF_Q_STAGES = 3 };
typedef struct { uint8_t state, flags; uint16_t reserved; uint32_t guestAddr, bytes; int64_t pts, dts; } SrPsmfQueueSlot;
typedef struct { uint32_t head, tail, count; SrPsmfQueueSlot slot[PSMF_Q_DEPTH]; } SrPsmfQueue;
typedef struct {
    int used; uint32_t guest, buffer, bufferSize, priority, tempBuf, tempSize;
    uint32_t fileLba, fileSize, streamOffset, streamSize, readOffset;
    uint32_t status, playerVersion, videoStreams, audioStreams, videoWidth, videoHeight;
    uint32_t videoCodec, videoStreamNum, audioCodec, audioStreamNum, playMode, playSpeed;
    uint32_t pixelMode, loopStatus, warmup, breakRequested; int64_t currentPts, durationPts;
    char path[512]; SrPsmfQueue q[PSMF_TRACKS][PSMF_Q_STAGES];
} SrPsmfPlayer;
static SrPsmfPlayer s_psmf_players[4];
/* scePsmfPlayer activity counters. The no-frame watchdog reports the sceMpeg counters,
 * which are a different library; without these there is no evidence for or against the
 * player path being involved in a no-new-frame stretch. File-local: no other translation
 * unit consumes them (the watchdog that prints them lives in this file). */
static unsigned long s_psmf_calls = 0, s_psmf_getvideo = 0, s_psmf_getaudio = 0;
static SrPsmfPlayer *psmf_find(uint32_t guest, int create) {
    SrPsmfPlayer *freeSlot = NULL;
    s_psmf_calls++;   /* every scePsmfPlayer handler resolves its control block here */
    for (size_t i = 0; i < sizeof(s_psmf_players) / sizeof(s_psmf_players[0]); i++) {
        if (s_psmf_players[i].used && s_psmf_players[i].guest == guest) return &s_psmf_players[i];
        if (!s_psmf_players[i].used && !freeSlot) freeSlot = &s_psmf_players[i];
    }
    if (create && guest && freeSlot) { memset(freeSlot, 0, sizeof(*freeSlot)); freeSlot->used=1; freeSlot->guest=guest; return freeSlot; }
    return NULL;
}
static void psmf_flush(SrPsmfPlayer *p) { if (p) memset(p->q, 0, sizeof(p->q)); }
static uint32_t psmf_be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t psmf_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static int psmf_parse_header(SrPsmfPlayer *p, const uint8_t *h, uint32_t n) {
    if (!p || !h || n < 0x82 || psmf_be32(h) != 0x50534d46u) return 0;
    uint32_t streams = psmf_be16(h + 0x80); if (streams > 128 || 0x82u + streams * 16u > n) return 0;
    p->streamOffset=psmf_be32(h+8); p->streamSize=psmf_be32(h+12); p->videoWidth=h[0x8e]*16u; p->videoHeight=h[0x8f]*16u;
    p->videoStreams=p->audioStreams=0; p->playerVersion=0;
    for (uint32_t i=0;i<streams;i++) { const uint8_t *s=h+0x82u+i*16u; if ((s[0]&0xe0u)==0xe0u) { p->videoStreams++; if(!psmf_be32(s+4)||!psmf_be32(s+8))p->playerVersion=1; } else if ((s[0]&0xf0u)==0xb0u || (s[0]&0xf0u)==0xf0u) p->audioStreams++; }
    if (!p->videoStreams) return 0;
    p->durationPts = psmf_be32(h + 0x5a);
    p->currentPts = 0;
    return 1;
}
static uint32_t h_PsmfCreate(CpuState *s) {
    SrPsmfPlayer *p=psmf_find(A0,1); if(!p||!A1)return PSMF_ERR_PARAM; uint32_t b=MEM_R32(A1),sz=MEM_R32(A1+4),pri=MEM_R32(A1+8);
    if(!b||sz<0x00285800u){MEM_W32(A0,0);return PSMF_ERR_BUFSIZE;} if(pri<0x10u||pri>=0x6eu){MEM_W32(A0,0);return PSMF_ERR_PARAM;}
    p->buffer=b;p->bufferSize=sz;p->priority=pri;p->status=PSMF_STATUS_INIT;p->pixelMode=3;p->loopStatus=1;p->videoCodec=0xe;p->audioCodec=0xf;MEM_W32(A0,A0);sched_delay_current(20000);return 0;
}
static uint32_t h_PsmfDelete(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);if(!p)return PSMF_ERR_STATUS;memset(p,0,sizeof(*p));MEM_W32(A0,0);sched_delay_current(20000);return 0;}
static uint32_t h_PsmfSetTempBuf(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);if(!p||p->status!=PSMF_STATUS_INIT)return PSMF_ERR_STATUS;if(!A1||A2<0x10000u)return PSMF_ERR_PARAM;p->tempBuf=A1;p->tempSize=A2;return 0;}
static uint32_t h_PsmfSetPsmfCB(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);if(!p||p->status!=PSMF_STATUS_INIT)return PSMF_ERR_STATUS;if(!A1)return PSMF_ERR_PARAM;char path[512];guest_cstr(A1,path,sizeof(path));uint32_t lba=0,sz=0;uint8_t h[2048];if(iso_lookup(path,&lba,&sz)!=0||sz<sizeof(h)||iso_read(lba,0,h,sizeof(h))!=(int)sizeof(h)||!psmf_parse_header(p,h,sizeof(h)))return PSMF_ERR_PARAM;strncpy(p->path,path,sizeof(p->path)-1);p->fileLba=lba;p->fileSize=sz;p->readOffset=p->streamOffset;p->status=PSMF_STATUS_STANDBY;psmf_flush(p);sched_delay_current(3100);return 0;}
static uint32_t h_PsmfConfig(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);if(!p)return PSMF_ERR_STATUS;if(A1==0){if(A2>1)return PSMF_ERR_PARAM;p->loopStatus=A2;return 0;}if(A1==1){if((int32_t)A2<-1||A2>3)return PSMF_ERR_PARAM;p->pixelMode=(A2==(uint32_t)-1)?3:A2;return 0;}return PSMF_ERR_CONFIG;}
static uint32_t h_PsmfStart(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);if(!p||p->status==PSMF_STATUS_INIT)return PSMF_ERR_STATUS;if(!A1)return PSMF_ERR_PARAM;uint32_t d=A1,vc=MEM_R32(d),vs=MEM_R32(d+4),ac=MEM_R32(d+8),as=MEM_R32(d+12);int32_t mode=(int32_t)MEM_R32(d+16),speed=(int32_t)MEM_R32(d+20),pts=(int32_t)stack_arg(s,0);if(mode<0||mode>5||vs>=p->videoStreams||(p->audioStreams&&as>=p->audioStreams))return PSMF_ERR_CONFIG;if(vc&&vc!=0xe)return PSMF_ERR_STREAM;if(p->audioStreams&&ac!=1&&ac!=0xf)return PSMF_ERR_STREAM;if(p->playerVersion==1&&pts!=0)return PSMF_ERR_PARAM;p->videoCodec=vc;p->videoStreamNum=vs;p->audioCodec=ac;p->audioStreamNum=as;p->playMode=(uint32_t)mode;p->playSpeed=(uint32_t)speed;p->currentPts=pts;p->warmup=0;p->breakRequested=0;psmf_flush(p);p->status=PSMF_STATUS_PLAYING;return 0;}
static uint32_t h_PsmfStop(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);if(!p||p->status<PSMF_STATUS_PLAYING)return PSMF_ERR_STATUS;p->status=PSMF_STATUS_STANDBY;p->breakRequested=0;psmf_flush(p);sched_delay_current(3000);return 0;}
static uint32_t h_PsmfBreak(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);if(!p)return PSMF_ERR_STATUS;p->breakRequested=1;psmf_flush(p);return 0;}
static uint32_t h_PsmfRelease(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);if(!p||p->status<PSMF_STATUS_STANDBY)return PSMF_ERR_STATUS;p->status=PSMF_STATUS_INIT;p->fileLba=p->fileSize=p->streamOffset=p->streamSize=0;psmf_flush(p);return 0;}
static uint32_t h_PsmfStatus(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);return p?p->status:PSMF_ERR_STATUS;}
static uint32_t h_PsmfUpdate(CpuState *s){SrPsmfPlayer*p=psmf_find(A0,0);if(!p||p->status<PSMF_STATUS_PLAYING)return PSMF_ERR_STATUS;if(p->status==PSMF_STATUS_FINISHED&&p->loopStatus){p->status=PSMF_STATUS_PLAYING;p->currentPts=0;psmf_flush(p);}return 0;}
static uint32_t h_PsmfGetVideo(CpuState *s){s_psmf_getvideo++;SrPsmfPlayer*p=psmf_find(A0,0);if(!p||p->status<PSMF_STATUS_PLAYING)return PSMF_ERR_STATUS;if(!A1)return PSMF_ERR_PARAM;return PSMF_ERR_NO_DATA;}
static uint32_t h_PsmfGetAudio(CpuState *s){s_psmf_getaudio++;SrPsmfPlayer*p=psmf_find(A0,0);if(!p||p->status<PSMF_STATUS_PLAYING)return PSMF_ERR_STATUS;if(!A1)return PSMF_ERR_PARAM;return PSMF_ERR_NO_DATA;}
static uint32_t h_PsmfAudioOutSize(CpuState *s){return psmf_find(A0,0)?8192u:PSMF_ERR_STATUS;}

static uint32_t h_IoDevctl(CpuState *s) {
    char dev[64];
    guest_cstr(A0, dev, sizeof(dev));
    uint32_t cmd = A1;
    if (hle_log_on())
        fprintf(stderr, "sceIoDevctl: dev='%s', cmd=0x%08x\n", dev, cmd);
    return 0;
}

/* sceKernelExitGame: real PSP terminates the process. Three callers observed so far:
 *  (1) the guest's libc _exit path (call site already inside the engine shutdown decision)
 *  (2) the launcher thread (uid 0x110 / 0x111) after the worker dissolves
 *  (3) the VBLANK handler inside engine_Shutdown: it calls sceKernelExitGame(0), which on
 *      a real PSP terminates the process and never returns. We must NOT return from this
 *      syscall when invoked from interrupt context (cur_uid==0): sched_thread_sleep() is
 *      a no-op there, the call falls through back into the callback body, the body
 *      reaches ExitGame again, and the recomp arms an infinite countdown of redundant
 *      GAMELOG messages per vblank (smoke_*.err saw 9497 ExitGames in one run from a
 *      single chain ending at the same PC). Behavioural fix: interrupt-context ExitGame
 *      is a host-process _exit. Thread-context ExitGame stays a thread sleep so the
 *      worker / launcher can be observed across subsequent yields. */
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


    /* Terminal on hardware in every context.  Do not park the calling thread to keep the
     * runtime observable; that legacy diagnostic path allowed execution after ExitGame. */
    sr_trace_close();

    /* Write crash dump and exit flag for tools/mem_debug.py to trap and trace */
    FILE *f_dump = fopen("build/hst/crash_dump.bin", "wb");
    if (f_dump) {
        fwrite(s, 1, sizeof(CpuState), f_dump);
        uint32_t sp_base = s->r[29] & 0xFFFF0000u;
        if (sr_inrange(sp_base)) {
            fwrite(SR_HOST(sp_base), 1, 0x10000, f_dump);
        }
        fclose(f_dump);
    }
    FILE *f_flag = fopen("build/hst/exited.flag", "w");
    if (f_flag) {
        fprintf(f_flag, "1\n");
        fclose(f_flag);
    }

    fflush(NULL);
    if (cur == 0) {
        /* Interrupt context (PSP kernel callback chain). Hard-stop the host process: this
         * matches hardware (the kernel never returns to the caller once ExitGame fires).
         * A clean Crt exit lets the operator see the last 0.5 MB of trace before the
         * window disappears. */
        fprintf(stderr, "  pc=0x%08x ra=0x%08x sp=0x%08x gp=0x%08x v0=0x%08x a0=0x%08x a1=0x%08x\n",
                s->pc, s->r[31], s->r[29], s->r[28], s->r[2], s->r[4], s->r[5]);
        fprintf(stderr, "  insn[vblank-cb-begin]=0x%08x insn[vblank-cb-next]=0x%08x 0x310a034=0x%08x 0x002cf6b4=0x%08x\n",
                s->pc ? MEM_R32(s->pc) : 0, s->pc ? MEM_R32(s->pc + 4) : 0,
                MEM_R32(0x310a034u), MEM_R32(0x002cf6b4u));
        fflush(stderr);
        /* Give the host a chance to drain. Without this, stdio buffers get truncated by
         * _exit; the operator sees ~16 KB of trailing context instead of 1 MB. */
        Sleep(50);
        fflush(NULL);
        _exit(code);
    }
    sr_trace_close(); fflush(NULL); Sleep(50); _exit(code);
    return code; /* unreachable, keeps the handler signature honest */
}

/* I4: post-umd.ufl tracker. Counter + active flag; stepped by h_IoRead after the
 *   head read of umd.ufl completes, peeked by h_IoOpen for subsequent IoOpens,
 *   flushed by h_ExitThread on the worker's first pre-shutdown syscall.
 *   Activated by SR_POSTUMD env (default off). The local copies here live at file
 *   scope so h_IoRead/h_IoOpen can see them without extern hop. */
static uint64_t s_postumd_count = 0;
static int      s_postumd_active = 0;
uint64_t sr_postumd_reads(void) { return s_postumd_count; }
void sr_postumd_advance(int active_after) {
    s_postumd_count++;
    if (active_after) s_postumd_active = 1;
}
void sr_postumd_signal_shutdown(uint64_t captured_count) {
    if (!s_postumd_active && captured_count == 0) return;
    s_postumd_active = 0;
    fprintf(stderr, "POSTUMD: shutdown signal after %llu post-umd IoReads (worker turn finalized)\n",
            (unsigned long long)captured_count);
    fflush(stderr);
}

/* I3: deepest-frame register snapshot when the worker (uid 0x115) is about to exit. The
 *   trace shows the worker exits via sceKernelExitThread after a
 *   Stop/Unload/Exit chain (run_err_new.log lines 165..180). The shutdown decision
 *   originates from a guest function (likely engine_Shutdown). Capturing r2/v0, r31/ra,
 *   r26/k0 stack, and PC at the moment the worker decides to unwind gives the calling
 *   function (i.e. the engine decision site). SR_EXITSNAP env-gates the trace. */
static int s_exitsnap_armed = -1;
typedef struct { uint32_t pc, ra, v0, k0, k0p4, a0, a1, a2, a3; uint64_t tick; } ExitSnap;
static ExitSnap s_last_worker_exit;
static int      s_last_worker_exit_valid = 0;
static int      s_last_worker_exit_count = 0;
void sr_exitsnap_capture(CpuState *s) {
    if (s_exitsnap_armed < 0) s_exitsnap_armed = getenv("SR_EXITSNAP") ? 1 : 0;
    if (!s_exitsnap_armed) return;
    if (!sched_current_is_worker() && !sched_current_is_launcher()) return;
    s_last_worker_exit.pc = s->pc;
    s_last_worker_exit.ra = s->r[31];
    s_last_worker_exit.v0 = s->r[2];
    s_last_worker_exit.k0 = s->r[26];
    s_last_worker_exit.k0p4 = s->r[26] ? MEM_R32(s->r[26] + 4) : 0;
    s_last_worker_exit.a0 = s->r[4];
    s_last_worker_exit.a1 = s->r[5];
    s_last_worker_exit.a2 = s->r[6];
    s_last_worker_exit.a3 = s->r[7];
    /* Tick is exposed via an extern since s_tick is static in sched.c. */
    s_last_worker_exit.tick = 0;
    s_last_worker_exit_valid = 1;
    s_last_worker_exit_count++;
    if (s_last_worker_exit_count <= 16) {
        fprintf(stderr, "EXITSNAP uid=0x%x pc=0x%08x ra=0x%08x v0=0x%08x k0=0x%08x k0+4=0x%08x a0..a3=%08x %08x %08x %08x\n",
                sched_current_uid(), s_last_worker_exit.pc, s_last_worker_exit.ra, s_last_worker_exit.v0,
                s_last_worker_exit.k0, s_last_worker_exit.k0p4,
                s_last_worker_exit.a0, s_last_worker_exit.a1, s_last_worker_exit.a2, s_last_worker_exit.a3);
        fflush(stderr);
    }
}
void sr_exitsnap_dump_latest(const char *tag) {
    if (!s_exitsnap_armed || !s_last_worker_exit_valid) return;
    fprintf(stderr, "EXITSNAP[%s]: pc=0x%08x ra=0x%08x v0=0x%08x k0=0x%08x k0+4=0x%08x\n",
            tag, s_last_worker_exit.pc, s_last_worker_exit.ra, s_last_worker_exit.v0,
            s_last_worker_exit.k0, s_last_worker_exit.k0p4);
    fflush(stderr);
}

/* Glue for the libc "should be called from main thread" path: when that string fires the
 * guest's libc reentrancy guard has already decided to shut down. The next syscall
 * EXECUTED by the same thread is normally sceKernelExitThread (it traps itself by
 * calling sceKernelExitThread with a poisoned stack), but if we let it run we drop
 * into the scheduler with NO runnable threads and just sit there burning vblanks.
 * This flag tells h_ExitThread to honour the guest's death wish and _exit(1) the host
 * cleanly instead. */
int sr_libc_death_wish = 0;

/* ModuleMgrForUser (h_module_uid removed; no current consumer -- sr_alloc_uid supplies module uids ). */

/* Forward decls for the callback infrastructure (defined at file scope below the VBLANK
 * sub-interrupt handler globals). h_CreateCallback and h_NotifyCallback reference them. */
typedef struct {
    uint32_t uid;
    uint32_t entry;
    uint32_t arg;
    uint32_t owner_thread_uid;
    int used;
    int pending;
    uint32_t notify_count;
    uint32_t notify_arg;
    char name[32];
} CallbackEntry;
extern CallbackEntry *s_callbacks;
extern size_t s_callbacks_len;
extern uint32_t sr_callback_table_register(uint32_t name_addr, uint32_t entry,
                                           uint32_t arg, uint32_t *error_out);
extern int sr_vblank_dispatch_registered(void);
extern int sr_callback_find_in_table(uint32_t uid);
static uint32_t h_GetModuleId(CpuState *s) { (void)s; return 0x112; }   /* main module's id (stable) */

static uint32_t h_StopModule_Trace(CpuState *s) {
    uint32_t uid = sched_current_uid();
    fprintf(stderr, "TRACE_STOPMODULE: uid=0x%x pc=0x%08x ra=0x%08x modid=0x%08x\n",
            uid, s->pc, s->r[31], s->r[4]);
    fflush(stderr);
    return 0;
}

static uint32_t h_UnloadModule_Trace(CpuState *s) {
    uint32_t uid = sched_current_uid();
    fprintf(stderr, "TRACE_UNLOADMODULE: uid=0x%x pc=0x%08x ra=0x%08x modid=0x%08x\n",
            uid, s->pc, s->r[31], s->r[4]);
    fflush(stderr);
    return 0;
}

static uint32_t h_CreateCallback(CpuState *s) {
    uint32_t error = 0;
    uint32_t uid = sr_callback_table_register(A0, A1, A2, &error);
    if (getenv("SR_CBLOG")) {
        fprintf(stderr,
                "CBLOG: CreateCallback name=0x%08x entry=0x%08x common=0x%08x -> 0x%08x\n",
                A0, A1, A2, uid ? uid : error);
    }
    return uid ? uid : error;
}
static uint32_t s_exit_cb_uid;

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

/* Kernel_Library: interrupt suspend/resume. Suspend returns the prior state (use 1); resume
 * is void. The game uses a lock counter at 0x30aa80 (nesting depth) and 0x30aa84 (saved state).
 * FUN_00011578 (AllocLock) calls SuspendIntr and increments 0x30aa80; on first nest, saves
 * the prior interrupt state to 0x30aa84. FUN_000115b4 (AllocUnlock) decrements 0x30aa80 and
 * on reaching zero, calls ResumeIntr. The spinlock at 0x10974 polls 0x30aa80 via bne. */
static uint32_t h_CpuSuspendIntr(CpuState *s) {
    (void)s;
    if (getenv("SR_SYSLOG")) {
        fprintf(stderr, "HLE: sceKernelCpuSuspendIntr called\n");
    }
    if (getenv("SR_DEBUG_THREAD_LOG")) {
        /* Role-resolved rather than UID-literal (the historical literals drifted and
         * left these diagnostics permanently silent). A build with no worker/launcher
         * binding has no role to report, so both branches stay quiet. */
        if (sched_current_is_worker()) {
            fprintf(stderr, "DEBUG: CpuSuspendIntr worker=0x%x: r16 (s0)=0x%x, r26 (k0)=0x%x, k0+4=0x%x, MEM(0x002cf6b4)=0x%x, ra=0x%x, sp=0x%x\n  Stack:",
                    sched_current_uid(), s->r[16], s->r[26], s->r[26] ? MEM_R32(s->r[26] + 4) : 0, MEM_R32(0x002cf6b4u), s->r[31], s->r[29]);
            for (int i = 0; i < 20; i++) {
                fprintf(stderr, " +%d:0x%x", i * 4, MEM_R32(s->r[29] + i * 4));
            }
            fprintf(stderr, "\n");
        }
        if (sched_current_is_launcher()) {
            fprintf(stderr, "DEBUG: CpuSuspendIntr launcher=0x%x: r16 (s0)=0x%x, r17 (s1)=0x%x, r26 (k0)=0x%x, ra=0x%x\n",
                    sched_current_uid(), s->r[16], s->r[17], s->r[26], s->r[31]);
        }
    }
    return sched_suspend_interrupts();
}

static uint32_t h_CpuResumeIntr(CpuState *s) {
    if (getenv("SR_SYSLOG")) {
        fprintf(stderr, "HLE: sceKernelCpuResumeIntr called\n");
    }
    if (getenv("SR_DEBUG_THREAD_LOG")) {
        /* Role-resolved (see h_CpuSuspendIntr). */
        if (sched_current_is_worker()) {
            fprintf(stderr, "DEBUG: CpuResumeIntr worker=0x%x: r2 (v0)=0x%x, r6 (a2)=0x%x, r16 (s0)=0x%x, r26 (k0)=0x%x, ra=0x%x, sp=0x%x\n  Stack:",
                    sched_current_uid(), s->r[2], s->r[6], s->r[16], s->r[26], s->r[31], s->r[29]);
            for (int i = 0; i < 20; i++) {
                fprintf(stderr, " +%d:0x%x", i * 4, MEM_R32(s->r[29] + i * 4));
            }
            fprintf(stderr, "\n");
        }
        if (sched_current_is_launcher()) {
            fprintf(stderr, "DEBUG: CpuResumeIntr launcher=0x%x: r2 (v0)=0x%x, r16 (s0)=0x%x, r17 (s1)=0x%x, r26 (k0)=0x%x, ra=0x%x, MEM(0x0030aa88)=0x%x\n",
                    sched_current_uid(), s->r[2], s->r[16], s->r[17], s->r[26], s->r[31], MEM_R32(0x0030aa88u));
        }
    }
    sched_resume_interrupts(A0);
    /* sched_resume_interrupts performs the eligible pending-source delivery
     * and the normal post-interrupt priority check. */
    return 0;
}
static uint32_t h_CpuResumeIntrWithSync(CpuState *s) {
    return h_CpuResumeIntr(s);
}
/* sceKernelIsCpuIntrSuspended(flag) asks whether the SUPPLIED saved-state token
 * came from an already-suspended CPU.  It is a pure predicate on the argument and
 * must not consult the live interrupt state: PSPAutotests tests/intr/suspended.expected
 * prints the same four results with interrupts enabled and with them suspended
 * (0 -> 1, 1 -> 0, 2 -> 0, 0xDEADBEEF -> 0), and the real-PSP capture on issue #88
 * (PSP-3001 / 6.61-ARK) reports those same values.  A token of 0 is what
 * sceKernelCpuSuspendIntr returns when interrupts were ALREADY off, so 0 -> "was
 * suspended" -> 1; any other token means they had been enabled. */
static uint32_t h_CpuIsIntrSuspended(CpuState *s) {
    return A0 == 0u ? 1u : 0u;
}
static uint32_t h_CpuIsIntrEnable(CpuState *s) {
    (void)s;
    return sched_interrupts_enabled() ? 1u : 0u;
}
static uint32_t h_SuspendDispatchThread(CpuState *s) {
    (void)s;
    return sched_suspend_dispatch();
}
static uint32_t h_ResumeDispatchThread(CpuState *s) {
    return sched_resume_dispatch(A0);
}

/* Boot-path setup calls. These return success (or the value the reference run returns) so the boot
 * proceeds down the same branch. They do not yet model the subsystem behind them; the import
 * sequence vs the reference run is what validates that the returned value drives the same path. */
static uint32_t h_UmdCheckMedium(CpuState *s) { (void)s; return 1; }      /* medium present */
/* ---- sceDmacMemcpy / sceDmacTryMemcpy ---------------------------------------
 *
 * The current PSP-3001 / 6.61-ARK contract used here is deliberately narrow:
 *
 *   - a zero request returns 0x80000104 (illegal size);
 *   - a NULL or invalid complete source/destination span returns 0x80000103
 *     before any guest or GPU-visible side effect;
 *   - the effective transfer length is min(requested, 0xC000), and a request
 *     above that ceiling still returns success after copying only that prefix;
 *   - same-pointer and forward/backward overlapping copies are memmove-correct;
 *   - the Try form is synchronous from a single caller's point of view and
 *     shares the measured copy/error contract;
 *   - no concurrent BUSY result has been established, so this runtime does not
 *     invent an asynchronous engine or a scheduler-owned DMA queue.
 *
 * The complete *requested* spans are validated before the effective length is
 * applied. Hardware has not yet settled whether an invalid truncated tail is
 * ignored, so validating the requested range is the conservative memory-safety
 * policy and is kept explicit rather than presented as a measured precedence.
 * The size-before-address ordering below is likewise a runtime ordering; the
 * combined size-zero-plus-invalid-pointer case was not part of the probe.
 * The measured ~376â€“382 us observation for a large call is caller wall time;
 * no guest-time rate law is inferred from it.
 * Guest RAM/VRAM share the runtime's unified host allocation, and this target
 * does not currently translate guest self-modifying code or maintain a separate
 * instruction-cache/dispatch-table invalidation layer. DMA therefore exposes
 * no additional code-invalidation side effect here; that boundary belongs to a
 * future dynamic-code correctness issue rather than this copy contract.
 */
#define SCE_DMAC_ERROR_ILLEGAL_ADDR 0x80000103u
#define SCE_DMAC_ERROR_ILLEGAL_SIZE 0x80000104u
#define SCE_DMAC_EFFECTIVE_MAX 0xC000u

static uint32_t h_DmacMemcpy(CpuState *s) {
    /* a0=dst, a1=src, a2=size. A real DMA copy in guest memory. */
    uint32_t dst = A0, src = A1, n = A2;

    /* Validate everything before touching guest memory: a rejected request must
     * leave the destination bytes and the GPU's view of them exactly as they
     * were, so no dirty notification may be issued on any failure path. */
    if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;
    if (dst == 0u || src == 0u) return SCE_DMAC_ERROR_ILLEGAL_ADDR;
    /* The complete spans, not just the base addresses. sr_guest_span_* is
     * overflow-safe (it compares the remaining arena extent against the size
     * rather than computing addr + size), so a request whose end wraps
     * uint32_t or crosses the end of modeled memory is rejected here rather
     * than truncated into a partial copy. */
    if (!sr_guest_span_readable(src, n) || !sr_guest_span_writable(dst, n)) {
        if (!sr_guest_span_writable(dst, n)) sr_oor(dst, 0u, 1);
        if (!sr_guest_span_readable(src, n)) sr_oor(src, 0u, 0);
        return SCE_DMAC_ERROR_ILLEGAL_ADDR;
    }
    uint32_t effective = n > SCE_DMAC_EFFECTIVE_MAX ? SCE_DMAC_EFFECTIVE_MAX : n;

    /* memmove, not memcpy: hardware showed both overlap directions landing
     * correctly, and dst == src must leave the buffer intact. */
    memmove(SR_HOST(dst), SR_HOST(src), effective);
    extern void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
    sr_gpu_vram_dirty(dst, effective);   /* notify only the bytes actually transferred */
    if (g_sr_heap_watch) sr_heap_note_bulk_write(dst, effective, 0u);
    return 0;
}

/* sceDmacTryMemcpy. Hardware shows it blocking for the full transfer and
 * producing the same result as the blocking form at every measured size, so it
 * shares those semantics deliberately rather than by aliasing an unrelated
 * handler. It is a distinct registered entry so that a future busy or
 * non-blocking measurement has somewhere to land without changing the
 * measured error and overlap behavior. */
static uint32_t h_DmacTryMemcpy(CpuState *s) {
    return h_DmacMemcpy(s);
}
static uint32_t h_Memset(CpuState *s) {
    /* a0=dst, a1=byte_val, a2=size */
    uint32_t dst = A0, val = A1 & 0xff, n = A2;
    if (n && sr_guest_span_writable(dst, n)) {
        memset(SR_HOST(dst), (int)val, n);
        if (g_sr_heap_watch) sr_heap_note_bulk_write(dst, n, 0u);
    } else if (n) {
        sr_oor(dst, val, 1);
    }
    return dst;
}
static uint32_t h_Memcpy(CpuState *s) {
    /* a0=dst, a1=src, a2=size */
    uint32_t dst = A0, src = A1, n = A2;
    if (n && sr_guest_span_readable(src, n) && sr_guest_span_writable(dst, n)) {
        memmove(SR_HOST(dst), SR_HOST(src), n);
        if (g_sr_heap_watch) sr_heap_note_bulk_write(dst, n, 0u);
    } else if (n) {
        sr_oor(dst, 0u, 1);
        sr_oor(src, 0u, 0);
    }
    return dst;
}

/* ---- checked UTF-8/UTF-16 host-path helpers (issue #223) --------------------------
 *
 * Guest paths and environment variables are UTF-8.  Keep them as UTF-8 in the
 * host-neutral index, then cross the Windows boundary exactly once with checked
 * conversions.  Every Windows file operation receives an absolute wide path;
 * the extended prefix keeps the route independent of MAX_PATH and of the
 * executable manifest's longPathAware setting.
 */
static int sr_utf8_to_wide_alloc(const char *src, wchar_t **out) {
    if (!src || !out || !sr_asset_index_valid_utf8(src) ||
        strlen(src) > (size_t)INT_MAX) return 0;
    *out = NULL;
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
    if (need <= 0 || (size_t)need > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *wide = (wchar_t *)calloc((size_t)need, sizeof(wchar_t));
    if (!wide) return 0;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, wide, need) != need) {
        free(wide);
        return 0;
    }
    *out = wide;
    return 1;
}

static int sr_wide_to_utf8_alloc(const wchar_t *src, char **out) {
    if (!src || !out || wcslen(src) > (size_t)INT_MAX) return 0;
    *out = NULL;
    int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1,
                                   NULL, 0, NULL, NULL);
    if (need <= 0) return 0;
    char *utf8 = (char *)malloc((size_t)need);
    if (!utf8) return 0;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1,
                            utf8, need, NULL, NULL) != need) {
        free(utf8);
        return 0;
    }
    *out = utf8;
    return 1;
}

static int sr_wide_env_alloc(const wchar_t *name, wchar_t **value_out,
                             int *present_out) {
    if (!name || !value_out || !present_out) return 0;
    *value_out = NULL;
    *present_out = 0;
    DWORD capacity = 256u;
    for (;;) {
        if (capacity > (DWORD)(SIZE_MAX / sizeof(wchar_t))) return 0;
        wchar_t *value = (wchar_t *)malloc((size_t)capacity * sizeof(*value));
        if (!value) return 0;
        SetLastError(ERROR_SUCCESS);
        DWORD length = GetEnvironmentVariableW(name, value, capacity);
        DWORD error = GetLastError();
        if (length == 0u && error == ERROR_ENVVAR_NOT_FOUND) {
            free(value);
            return 1;
        }
        if (length < capacity - 1u || (length == 0u && error == ERROR_SUCCESS)) {
            *value_out = value;
            *present_out = 1;
            return 1;
        }
        free(value);
        if (length < capacity) {
            if (capacity > UINT32_MAX / 2u) return 0;
            capacity *= 2u;
        } else {
            if (length == UINT32_MAX || length + 1u < length) return 0;
            capacity = length + 1u;
        }
    }
}

/* The save-data helpers retain their host-neutral UTF-8 API, but obtain the
 * Windows environment value through the wide API first so non-ASCII paths do
 * not depend on the process code page. */
static int sr_utf8_env_alloc(const wchar_t *name, char **value_out,
                             int *present_out) {
    if (!name || !value_out || !present_out) return 0;
    *value_out = NULL;
    *present_out = 0;
    wchar_t *wide = NULL;
    int present = 0;
    if (!sr_wide_env_alloc(name, &wide, &present)) return 0;
    if (!present) return 1;
    int ok = sr_wide_to_utf8_alloc(wide, value_out);
    free(wide);
    if (!ok) return 0;
    *present_out = 1;
    return 1;
}

static int sr_wide_join_alloc(const wchar_t *left, const wchar_t *right,
                              wchar_t **out) {
    if (!left || !right || !out) return 0;
    *out = NULL;
    size_t a = wcslen(left), b = wcslen(right);
    int separator = a > 0 && b > 0 && left[a - 1] != L'\\' && left[a - 1] != L'/';
    size_t extra = b;
    if (extra > SIZE_MAX - (size_t)separator - 1u) return 0;
    extra += (size_t)separator + 1u;
    if (a > SIZE_MAX - extra) return 0;
    size_t total = a + extra;
    if (total > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *joined = (wchar_t *)malloc(total * sizeof(*joined));
    if (!joined) return 0;
    memcpy(joined, left, a * sizeof(*joined));
    size_t at = a;
    if (separator) joined[at++] = L'\\';
    memcpy(joined + at, right, b * sizeof(*joined));
    joined[at + b] = L'\0';
    for (size_t i = 0; i < at + b; i++)
        if (joined[i] == L'/') joined[i] = L'\\';
    *out = joined;
    return 1;
}

static int sr_wide_get_current_directory(wchar_t **out) {
    if (!out) return 0;
    *out = NULL;
    DWORD capacity = 256u;
    for (;;) {
        wchar_t *buf = (wchar_t *)malloc((size_t)capacity * sizeof(*buf));
        if (!buf) return 0;
        DWORD length = GetCurrentDirectoryW(capacity, buf);
        if (length == 0) { free(buf); return 0; }
        if (length < capacity) { *out = buf; return 1; }
        free(buf);
        if (capacity > (DWORD)(SIZE_MAX / (2u * sizeof(*buf)))) return 0;
        capacity *= 2u;
    }
}

static int sr_wide_is_extended(const wchar_t *path) {
    return path && wcslen(path) >= 4u && path[0] == L'\\' && path[1] == L'\\' &&
           path[2] == L'?' && path[3] == L'\\';
}

static int sr_wide_is_drive_absolute(const wchar_t *path) {
    if (!path || wcslen(path) < 3u || path[1] != L':') return 0;
    int letter = (path[0] >= L'A' && path[0] <= L'Z') ||
                 (path[0] >= L'a' && path[0] <= L'z');
    return letter && (path[2] == L'\\' || path[2] == L'/');
}

static int sr_wide_is_absolute(const wchar_t *path) {
    return sr_wide_is_drive_absolute(path) ||
           (path && wcslen(path) >= 2u && path[0] == L'\\' && path[1] == L'\\');
}

static int sr_wide_extended_filesystem_root(const wchar_t *path) {
    if (!sr_wide_is_extended(path)) return 0;
    if (wcslen(path) >= 7u &&
        ((path[4] >= L'A' && path[4] <= L'Z') || (path[4] >= L'a' && path[4] <= L'z')) &&
        path[5] == L':' && (path[6] == L'\\' || path[6] == L'/')) return 1;
    if (_wcsnicmp(path, L"\\\\?\\UNC\\", 8u) != 0) return 0;
    const wchar_t *p = path + 8u;
    const wchar_t *server = p;
    while (*p && *p != L'\\' && *p != L'/') p++;
    if (p == server || !*p) return 0;
    while (*p == L'\\' || *p == L'/') p++;
    const wchar_t *share = p;
    while (*p && *p != L'\\' && *p != L'/') p++;
    return p != share;
}

/* Extended paths reject '.' and '..' components.  Rejecting those components
 * here is safer than passing an ambiguous path to a wide API; callers receive
 * a hard path-resolution error instead of silently escaping the configured root. */
static int sr_wide_has_dot_component(const wchar_t *path) {
    if (!path) return 1;
    const wchar_t *p = path;
    while (*p) {
        while (*p == L'\\' || *p == L'/') p++;
        const wchar_t *start = p;
        while (*p && *p != L'\\' && *p != L'/') p++;
        size_t n = (size_t)(p - start);
        if ((n == 1u && start[0] == L'.') ||
            (n == 2u && start[0] == L'.' && start[1] == L'.')) return 1;
    }
    return 0;
}

/* Resolve dot components before adding the extended-length prefix.  The
 * general writable VFS intentionally accepts relative roots such as
 * `SR_FSDIR=../fs`; rejecting the `..` that results from joining that path to
 * the CWD would break save/storage I/O.  Windows' full-path resolver gives
 * those paths their normal drive/UNC semantics, after which the extended APIs
 * receive a canonical path with no dot components. */
static int sr_wide_full_path_alloc(const wchar_t *input, wchar_t **out) {
    if (!input || !out) return 0;
    *out = NULL;
    DWORD capacity = 512u;
    for (;;) {
        if (capacity > (DWORD)(SIZE_MAX / sizeof(wchar_t))) return 0;
        wchar_t *resolved = (wchar_t *)malloc((size_t)capacity * sizeof(*resolved));
        if (!resolved) return 0;
        DWORD length = GetFullPathNameW(input, capacity, resolved, NULL);
        if (length == 0) {
            free(resolved);
            return 0;
        }
        if (length < capacity) {
            *out = resolved;
            return 1;
        }
        free(resolved);
        if (length == UINT32_MAX || length + 1u < length) return 0;
        capacity = length + 1u;
    }
}

static int sr_wide_extended_absolute_alloc(const wchar_t *absolute, wchar_t **out) {
    if (!absolute || !out || !sr_wide_is_absolute(absolute) ||
        sr_wide_has_dot_component(absolute)) return 0;
    if (sr_wide_is_extended(absolute) && !sr_wide_extended_filesystem_root(absolute)) return 0;
    if (absolute[0] == L'\\' && absolute[1] == L'\\' &&
        (absolute[2] == L'.' || absolute[2] == L'?') && !sr_wide_is_extended(absolute)) return 0;
    *out = NULL;
    size_t n = wcslen(absolute);
    const wchar_t *suffix = absolute;
    const wchar_t *prefix = L"\\\\?\\";
    size_t prefix_len = 4u;
    if (sr_wide_is_extended(absolute)) {
        if (n == SIZE_MAX || n + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
        wchar_t *copy = (wchar_t *)malloc((n + 1u) * sizeof(*copy));
        if (!copy) return 0;
        memcpy(copy, absolute, (n + 1u) * sizeof(*copy));
        *out = copy;
        return 1;
    }
    if (absolute[0] == L'\\' && absolute[1] == L'\\') {
        prefix = L"\\\\?\\UNC\\";
        prefix_len = 8u;
        suffix = absolute + 2;
        n -= 2u;
    }
    if (n == SIZE_MAX || prefix_len > SIZE_MAX - n - 1u) return 0;
    size_t total = prefix_len + n + 1u;
    if (total > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *extended = (wchar_t *)malloc(total * sizeof(*extended));
    if (!extended) return 0;
    memcpy(extended, prefix, prefix_len * sizeof(*extended));
    memcpy(extended + prefix_len, suffix, (n + 1u) * sizeof(*extended));
    *out = extended;
    return 1;
}

static int sr_wide_configured_root_wide_alloc(const wchar_t *value, wchar_t **out) {
    if (!value || !out || !value[0]) return 0;
    *out = NULL;
    size_t n = wcslen(value);
    if (n == SIZE_MAX || n + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *wide = (wchar_t *)malloc((n + 1u) * sizeof(*wide));
    if (!wide) return 0;
    memcpy(wide, value, (n + 1u) * sizeof(*wide));
    for (wchar_t *p = wide; *p; p++) if (*p == L'/') *p = L'\\';
    if (!(sr_wide_is_extended(wide) || sr_wide_is_absolute(wide))) {
        free(wide);
        return 0;
    }
    if (sr_wide_has_dot_component(wide)) {
        wchar_t *canonical = NULL;
        if (!sr_wide_full_path_alloc(wide, &canonical)) {
            free(wide);
            return 0;
        }
        free(wide);
        wide = canonical;
    }
    int ok = sr_wide_extended_absolute_alloc(wide, out);
    free(wide);
    return ok;
}

/* Resolve a UTF-8 path to an absolute extended-length Windows path. */
static int sr_wide_path_alloc(const char *utf8_path, wchar_t **out) {
    if (!utf8_path || !out || !utf8_path[0]) return 0;
    *out = NULL;
    wchar_t *raw = NULL;
    if (!sr_utf8_to_wide_alloc(utf8_path, &raw)) return 0;
    for (wchar_t *p = raw; *p; p++) if (*p == L'/') *p = L'\\';
    wchar_t *absolute = NULL;
    if (sr_wide_is_extended(raw) || sr_wide_is_absolute(raw)) {
        size_t n = wcslen(raw);
        if (n != SIZE_MAX && n + 1u <= SIZE_MAX / sizeof(wchar_t)) {
            absolute = (wchar_t *)malloc((n + 1u) * sizeof(*absolute));
            if (absolute) memcpy(absolute, raw, (n + 1u) * sizeof(*absolute));
        }
    } else {
        wchar_t *cwd = NULL;
        if (sr_wide_get_current_directory(&cwd)) {
            sr_wide_join_alloc(cwd, raw, &absolute);
            free(cwd);
        }
    }
    free(raw);
    if (!absolute) return 0;
    if (sr_wide_has_dot_component(absolute)) {
        wchar_t *canonical = NULL;
        if (!sr_wide_full_path_alloc(absolute, &canonical)) {
            free(absolute);
            return 0;
        }
        free(absolute);
        absolute = canonical;
    }
    int ok = sr_wide_extended_absolute_alloc(absolute, out);
    free(absolute);
    return ok;
}

static int sr_wide_module_dir_alloc(wchar_t **out) {
    if (!out) return 0;
    *out = NULL;
    DWORD capacity = 512u;
    for (;;) {
        wchar_t *module = (wchar_t *)malloc((size_t)capacity * sizeof(*module));
        if (!module) return 0;
        DWORD n = GetModuleFileNameW(NULL, module, capacity);
        if (n == 0) { free(module); return 0; }
        if (n < capacity - 1u) {
            while (n > 0 && module[n - 1] != L'\\' && module[n - 1] != L'/') n--;
            if (n == 0) { free(module); return 0; }
            module[n - 1] = L'\0';
            *out = module;
            return 1;
        }
        free(module);
        if (capacity > (DWORD)(SIZE_MAX / (2u * sizeof(*module)))) return 0;
        capacity *= 2u;
    }
}

static int sr_wide_parent_alloc(const wchar_t *path, wchar_t **out) {
    if (!path || !out) return 0;
    *out = NULL;
    size_t n = wcslen(path);
    while (n > 0 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) n--;
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    if (n == 0) return 0;
    /* Keep a drive root's trailing separator; a bare `C:` is drive-relative. */
    if (n == 2u && path[1] == L':') n = 3u;
    if (n == SIZE_MAX || n + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *parent = (wchar_t *)malloc((n + 1u) * sizeof(*parent));
    if (!parent) return 0;
    memcpy(parent, path, n * sizeof(*parent));
    parent[n] = L'\0';
    *out = parent;
    return 1;
}

static int sr_wide_module_font_root(wchar_t **out) {
    if (!out) return 0;
    *out = NULL;
    wchar_t *module_dir = NULL;
    wchar_t *font = NULL;
    int ok = sr_wide_module_dir_alloc(&module_dir) &&
             sr_wide_join_alloc(module_dir, L"font", &font) &&
             sr_wide_extended_absolute_alloc(font, out);
    free(module_dir);
    free(font);
    return ok;
}

/* The managed HST executable lives at <repo>/build/hst/hst.exe.  Resolve the
 * default data tree from that executable location, never from process CWD. */
static int sr_wide_module_data_root(wchar_t **out) {
    if (!out) return 0;
    *out = NULL;
    wchar_t *module_dir = NULL, *build_dir = NULL, *repo_dir = NULL;
    wchar_t *candidate = NULL, *next = NULL;
    int ok = sr_wide_module_dir_alloc(&module_dir) &&
             sr_wide_parent_alloc(module_dir, &build_dir) &&
             sr_wide_parent_alloc(build_dir, &repo_dir) &&
             sr_wide_join_alloc(repo_dir, L"place_game_here", &candidate) &&
             sr_wide_join_alloc(candidate,
                                L"EXTRACTED\\PSP_GAME\\USRDIR\\xbdata_extracted", &next) &&
             sr_wide_extended_absolute_alloc(next, out);
    free(module_dir);
    free(build_dir);
    free(repo_dir);
    free(candidate);
    free(next);
    return ok;
}

static FILE *sr_fopen_utf8(const char *path, const wchar_t *mode) {
    wchar_t *wide = NULL;
    if (!mode || !sr_wide_path_alloc(path, &wide)) return NULL;
    FILE *f = _wfopen(wide, mode);
    free(wide);
    return f;
}

static int sr_stream_size_u32(FILE *stream, uint32_t *size_out) {
    if (!stream || !size_out || _fseeki64(stream, 0, SEEK_END) != 0) return 0;
    __int64 end = _ftelli64(stream);
    int ok = end >= 0 && (uint64_t)end <= UINT32_MAX;
    if (_fseeki64(stream, 0, SEEK_SET) != 0) ok = 0;
    if (ok) *size_out = (uint32_t)end;
    return ok;
}

static int sr_ensure_directory_utf8(const char *path) {
    wchar_t *wide = NULL;
    if (!sr_wide_path_alloc(path, &wide)) return 0;
    BOOL made = CreateDirectoryW(wide, NULL);
    DWORD error = made ? ERROR_SUCCESS : GetLastError();
    DWORD attributes = (made || error != ERROR_ALREADY_EXISTS)
        ? FILE_ATTRIBUTE_DIRECTORY
        : GetFileAttributesW(wide);
    free(wide);
    return made || (error == ERROR_ALREADY_EXISTS &&
                    attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

static char *sr_utf8_join_alloc(const char *left, const char *right, char separator) {
    if (!left || !right) return NULL;
    size_t a = strlen(left), b = strlen(right);
    int need_separator = a > 0 && b > 0 && left[a - 1] != '/' && left[a - 1] != '\\';
    size_t extra = b;
    if (extra > SIZE_MAX - (size_t)need_separator - 1u) return NULL;
    extra += (size_t)need_separator + 1u;
    if (a > SIZE_MAX - extra) return NULL;
    char *joined = (char *)malloc(a + extra);
    if (!joined) return NULL;
    memcpy(joined, left, a);
    size_t at = a;
    if (need_separator) joined[at++] = separator;
    memcpy(joined + at, right, b + 1u);
    return joined;
}

static char *host_path_alloc(const char *guest) {
    char *configured = NULL;
    int configured_present = 0;
    if (!sr_utf8_env_alloc(L"SR_FSDIR", &configured, &configured_present)) return NULL;
    const char *dir = configured_present && configured[0] ? configured : "fs";
    if (!guest || !sr_ensure_directory_utf8(dir)) {
        free(configured);
        return NULL;
    }
    size_t root_len = strlen(dir), guest_len = strlen(guest);
    if (guest_len == 0 || root_len > SIZE_MAX - 2u || guest_len > SIZE_MAX - root_len - 2u) {
        free(configured);
        return NULL;
    }
    char *path = (char *)malloc(root_len + guest_len + 2u);
    if (!path) {
        free(configured);
        return NULL;
    }
    /* Flatten the guest string into a single filename beneath the fs root
     * (sr_vfs_host_flat_path); "." and ".." are rejected as directory
     * references. See vfs_path.h for the containment contract. */
    if (!sr_vfs_host_flat_path(dir, guest, path, root_len + guest_len + 2u)) {
        free(path);
        free(configured);
        return NULL;
    }
    free(configured);
    return path;
}

static char *host_dir_path_alloc(const char *guest) {
    char *configured = NULL;
    int configured_present = 0;
    if (!sr_utf8_env_alloc(L"SR_FSDIR", &configured, &configured_present)) return NULL;
    const char *root = configured_present && configured[0] ? configured : "fs";
    if (!guest) {
        free(configured);
        return NULL;
    }
    size_t cap;
    size_t root_len = strlen(root), guest_len = strlen(guest);
    if (root_len > SIZE_MAX - 2u || guest_len > SIZE_MAX - root_len - 2u) {
        free(configured);
        return NULL;
    }
    cap = root_len + guest_len + 2u;
    char *out = (char *)malloc(cap);
    if (!out) {
        free(configured);
        return NULL;
    }
    int n = sr_vfs_host_dir_path(root, guest, out, cap, '\\');
    if (n <= 0 || !sr_ensure_directory_utf8(root)) {
        free(out);
        free(configured);
        return NULL;
    }
    free(configured);
    return out;
}

/* sceLibFont. The PSP firmware fonts (flash0 font PGFs) are not on the game ISO, so we load the
 * user-supplied PGF fonts from an explicit SR_FONTDIR or from the executable's sibling `font`
 * directory and rasterise glyphs with the PGF reader (src/rt/pgf.c). A missing configured font is
 * reported as a path/configuration error; there is no current-working-directory retry or
 * synthetic fallback that could hide a bad root. */
static PGF *s_pgf_ltn = NULL, *s_pgf_jpn = NULL, *s_pgf_ltn8 = NULL, *s_pgf_kr = NULL;
static atomic_int s_pgf_state;

static PGF *font_open_from_root(const wchar_t *root, const wchar_t *name,
                                const char *source) {
    wchar_t *path = NULL;
    if (!sr_wide_join_alloc(root, name, &path)) {
        fprintf(stderr, "font_load: %s path construction failed\n", source);
        return NULL;
    }
    PGF *font = pgf_open_w(path);
    if (!font)
        fprintf(stderr, "font_load: %s font could not be opened (%ls)\n", source, path);
    free(path);
    return font;
}

static void font_load(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_pgf_state, &expected, 1,
                                                  memory_order_acq_rel, memory_order_acquire)) {
        while (atomic_load_explicit(&s_pgf_state, memory_order_acquire) == 1) { }
        return;
    }
    wchar_t *root = NULL;
    wchar_t *configured = NULL;
    int configured_present = 0;
    const char *source = NULL;
    int env_ok = sr_wide_env_alloc(L"SR_FONTDIR", &configured, &configured_present);
    if (!env_ok) {
        fprintf(stderr, "font_load: SR_FONTDIR could not be read\n");
    } else if (configured_present) {
        if (!configured[0] || !sr_wide_configured_root_wide_alloc(configured, &root)) {
            fprintf(stderr, "font_load: SR_FONTDIR is configured but is not a valid absolute path\n");
        } else {
            source = "SR_FONTDIR";
        }
    } else if (!sr_wide_module_font_root(&root)) {
        fprintf(stderr, "font_load: no SR_FONTDIR and executable font root could not be resolved\n");
    } else {
        source = "executable font root";
    }
    free(configured);
    if (root) {
        s_pgf_ltn = font_open_from_root(root, L"ltn0.pgf", source);
        s_pgf_jpn = font_open_from_root(root, L"jpn0.pgf", source);
        s_pgf_ltn8 = font_open_from_root(root, L"ltn8.pgf", source);
        s_pgf_kr = font_open_from_root(root, L"kr0.pgf", source);
    }
    free(root);
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "font_load: jpn0=%s ltn0=%s ltn8=%s kr0=%s\n",
                s_pgf_jpn ? "ok" : "MISSING", s_pgf_ltn ? "ok" : "MISSING",
                s_pgf_ltn8 ? "ok" : "MISSING", s_pgf_kr ? "ok" : "MISSING");
    atomic_store_explicit(&s_pgf_state, 2, memory_order_release);
}
/* HLE-to-guest call replay (defined later; used to invoke the game's allocFunc with the PSP
 * calling convention: allocFunc(userData=a0, size=a1) -> ptr). */
static uint32_t ge_call_guest_rv(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);

/* sceFont library layer (PPSSPP model): the FontLibrary and Font handles are real structs in
 * game-owned memory, carved from the caller's allocator so the game's Fpl accounting stays
 * consistent. The internal bookkeeping lives host-side (mirrors PPSSPP's FontLib/LoadedFont);
 * the guest struct just carries a magic + the identity fields, and the handle IS the guest ptr. */
#define SR_FONTLIB_MAGIC 0x464C4942u   /* 'FLIB' */
#define SR_FONT_MAGIC    0x464F4E54u   /* 'FONT' */
#define SR_FONTLIB_MIN_SIZE  0x28u
#define SR_FONT_SIZE     0x20u
#define SR_FONT_ERROR_OOM          0x80460001u
#define SR_FONT_ERROR_INVALID_LIB  0x80460002u
#define SR_FONT_ERROR_INVALID_ARG  0x80460003u
#define SR_FONT_ERROR_TOO_MANY     0x80460009u
#define SR_FONT_ERROR_INVALID_DATA 0x8046000au

typedef struct {
    const char *fileName;
    const char *fontName;
    uint16_t family, style, styleSub, language, region, country;
    float h, v, hRes, vRes;
} SrFontSpec;
/* The HST locale selector requests internal registry indices 0 (jpn0), 9 (ltn8), and
 * 17 (kr0). Keep the registry explicit so an absent indexed asset is an honest error rather
 * than silently returning ltn0 metrics with the wrong size/style. */
static const SrFontSpec s_font_specs[18] = {
    [0]  = {"jpn0.pgf", "FTT-NewRodin Pro DB", 0, 2, 0, 1, 0, 1, 10.125f, 10.125f, 128.f, 128.f},
    [9]  = {"ltn8.pgf", "FTT-NewRodin Pro Latin", 0, 0, 0, 2, 0, 1, 7.f, 7.f, 128.f, 128.f},
    [17] = {"kr0.pgf", "AsiaNHH(512Johab)", 0, 0, 0, 3, 0, 3, 10.125f, 10.125f, 128.f, 128.f},
};

typedef struct {
    int used;
    uint32_t handle;      /* guest pointer to the FontLibrary struct */
    uint32_t allocFn, freeFn, userData;
    int numFonts;
    int openCount;
    int pending;
    uint32_t params[11];  /* complete FontNewLibParams (0x2c), retained for callbacks */
    uint32_t allocSize;
} SrFontLib;
typedef struct {
    int used;
    int closing;
    unsigned refs;
    uint32_t handle;      /* guest pointer to the Font struct */
    uint32_t lib;         /* owning FontLibrary handle */
    const PGF *pgf;       /* PGF backend this font resolves against */
    PGF *ownedPgf;        /* non-NULL for sceFontOpenUserMemory */
    int index;
    const SrFontSpec *spec;
} SrFont;
static SrFontLib s_fontlibs[32];
static SrFont s_fonts[64];

static int fontlib_snapshot(uint32_t handle, SrFontLib *out) {
    int found = 0;
    if (!handle) return 0;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); i++)
        if (s_fontlibs[i].used && s_fontlibs[i].handle == handle) {
            if (out) *out = s_fontlibs[i];
            found = 1; break;
        }
    hle_unlock();
    return found;
}
static const PGF *font_acquire(uint32_t handle, SrFont **record) {
    const PGF *pgf = NULL;
    if (record) *record = NULL;
    if (!handle) return NULL;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++)
        if (s_fonts[i].used && !s_fonts[i].closing && s_fonts[i].handle == handle) {
            s_fonts[i].refs++;
            pgf = s_fonts[i].pgf;
            if (record) *record = &s_fonts[i];
            break;
        }
    hle_unlock();
    return pgf;
}
static void font_release(SrFont *f) {
    PGF *destroy = NULL;
    if (!f) return;
    hle_lock();
    if (f->refs) f->refs--;
    if (f->closing && f->refs == 0 && f->ownedPgf) {
        destroy = f->ownedPgf; f->ownedPgf = NULL; f->pgf = NULL;
    }
    if (f->closing && f->refs == 0 && !f->ownedPgf)
        memset(f, 0, sizeof(*f));
    hle_unlock();
    if (destroy) pgf_close(destroy);
}
/* Resolve the PGF for a character through its opened handle. Handle identity is preserved: a
 * missing glyph is reported by pgf.c rather than silently switching to another registry font. */
static const PGF *font_pgf_for(uint32_t handle, int cc, SrFont **record) {
    const PGF *p = font_acquire(handle, record);
    (void)cc;
    return p; /* An opened handle is permanently bound to its PGF; fallback stays inside pgf.c. */
}

/* Allocate size bytes of guest memory for a font handle. When the lib was created with a real
 * allocFunc, call it (PSP ABI) so the block comes from the game's Fpl pool; otherwise fall back
 * to the private bump heap. */
static uint32_t font_call_alloc(CpuState *s, uint32_t allocFn, uint32_t userData, uint32_t size) {
    if (allocFn) return ge_call_guest_rv(s, allocFn, userData, size, 0);
    return 0;
}
static void font_zero(uint32_t addr, uint32_t size) {
    if (!addr) return;
    for (uint32_t i = 0; i < size; i += 4) MEM_W32(addr + i, 0u);
}

static uint32_t font_lib_alloc_size(int numFonts) {
    /* PPSSPP's native FontLib layout: header + per-open slots + per-user-font slots +
     * internal-font style records. Keep arithmetic checked before calling guest code. */
    uint64_t n = (uint32_t)numFonts;
    uint64_t size = 0x4cull + n * 0x4cull + n * 0x230ull + 3ull * 0xa8ull;
    return size <= 0x7fffffffull ? (uint32_t)size : 0;
}

static const PGF *font_pgf_for_index(uint32_t index) {
    font_load();
    switch (index) {
    case 0:  return s_pgf_jpn;
    case 9:  return s_pgf_ltn8;
    case 17: return s_pgf_kr;
    default: return NULL;
    }
}

static void font_overlay_style(uint32_t fi, const SrFontSpec *spec, int userMemory) {
    if (!fi) return;
    /* PGFFontStyle begins at +0x5c. For user-memory PGFs, intrinsic metrics remain useful but
     * registry identity is deliberately unknown. */
    if (!spec || userMemory) {
        for (uint32_t i = 0; i < 0x90; i++) MEM_W8(fi + 0x70u + i, 0);
        return;
    }
    float f[4] = { spec->h, spec->v, spec->hRes, spec->vRes };
    for (int i = 0; i < 4; i++) { uint32_t w; memcpy(&w, &f[i], 4); MEM_W32(fi + 0x5cu + (uint32_t)i * 4, w); }
    MEM_W16(fi + 0x70, spec->family); MEM_W16(fi + 0x72, spec->style);
    MEM_W16(fi + 0x74, spec->styleSub); MEM_W16(fi + 0x76, spec->language);
    MEM_W16(fi + 0x78, spec->region); MEM_W16(fi + 0x7a, spec->country);
    for (uint32_t i = 0; i < 64; i++) MEM_W8(fi + 0x7c + i, 0);
    for (uint32_t i = 0; spec->fontName[i] && i < 63; i++) MEM_W8(fi + 0x7c + i, (uint8_t)spec->fontName[i]);
    for (uint32_t i = 0; i < 64; i++) MEM_W8(fi + 0xbc + i, 0);
    for (uint32_t i = 0; spec->fileName[i] && i < 63; i++) MEM_W8(fi + 0xbc + i, (uint8_t)spec->fileName[i]);
    MEM_W32(fi + 0xfc, 0); MEM_W32(fi + 0x100, 0);
}

static uint32_t h_FontNewLib(CpuState *s) {
    /* sceFontNewLib(a0=FontNewLibParams*, a1=u32* errorCode). PPSSPP param layout:
     * +0 userData, +4 numFonts, +8 cacheData, +0xc allocFunc, +0x10 freeFunc, ... */
    uint32_t params = A0, errPtr = A1;
    uint32_t userData = 0, allocFn = 0, freeFn = 0; int numFonts = 0;
    uint32_t paramsCopy[11] = {0};
    if (params) {
        for (int i = 0; i < 11; i++) paramsCopy[i] = MEM_R32(params + (uint32_t)i * 4);
        userData = MEM_R32(params + 0x00);
        numFonts = (int)MEM_R32(params + 0x04);
        allocFn  = MEM_R32(params + 0x0c);
        freeFn   = MEM_R32(params + 0x10);
    }
    if (!params || !allocFn || !freeFn || numFonts <= 0) {
        if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_INVALID_ARG);
        return 0;
    }
    if (numFonts > 9) numFonts = 9;
    uint32_t allocSize = font_lib_alloc_size(numFonts);
    if (!allocSize) { if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_OOM); return 0; }
    font_load();
    int slot = -1;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); i++)
        if (!s_fontlibs[i].used) { s_fontlibs[i].used = -1; slot = (int)i; break; }
    hle_unlock();
    if (slot < 0) { if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_TOO_MANY); return 0; }
    uint32_t lib = font_call_alloc(s, allocFn, userData, allocSize);
    if (lib) {
        font_zero(lib, allocSize);
        MEM_W32(lib + 0x00, SR_FONTLIB_MAGIC);
        MEM_W32(lib + 0x04, (uint32_t)numFonts);
        MEM_W32(lib + 0x08, userData);
        MEM_W32(lib + 0x0c, allocFn);
        MEM_W32(lib + 0x10, freeFn);
    }
    hle_lock();
    if (lib) {
        s_fontlibs[slot].used = 1; s_fontlibs[slot].handle = lib;
        s_fontlibs[slot].allocFn = allocFn; s_fontlibs[slot].freeFn = freeFn;
        s_fontlibs[slot].userData = userData; s_fontlibs[slot].numFonts = numFonts;
        s_fontlibs[slot].openCount = 0;
        memcpy(s_fontlibs[slot].params, paramsCopy, sizeof(paramsCopy));
        s_fontlibs[slot].allocSize = allocSize;
    } else memset(&s_fontlibs[slot], 0, sizeof(s_fontlibs[slot]));
    hle_unlock();
    if (errPtr) MEM_W32(errPtr, lib ? 0 : SR_FONT_ERROR_OOM);
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "h_FontNewLib: params=0x%08x numFonts=%d alloc=0x%08x free=0x%08x -> lib=0x%08x\n",
                params, numFonts, allocFn, freeFn, lib);
    return lib;
}
/* Shared Font-handle constructor for Open / OpenUserMemory. */
static uint32_t font_open_common(CpuState *s, uint32_t lib, uint32_t index, uint32_t mode,
                                 const PGF *pgf, PGF *ownedPgf, const SrFontSpec *spec,
                                 uint32_t errPtr) {
    SrFontLib L;
    if (!fontlib_snapshot(lib, &L)) {
        if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_INVALID_LIB);
        if (ownedPgf) pgf_close(ownedPgf);
        return 0;
    }
    if (!pgf) {
        if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_INVALID_DATA);
        if (ownedPgf) pgf_close(ownedPgf);
        return 0;
    }
    int slot = -1, libslot = -1;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); i++)
        if (s_fontlibs[i].used == 1 && s_fontlibs[i].handle == lib &&
            s_fontlibs[i].openCount + s_fontlibs[i].pending < s_fontlibs[i].numFonts) { libslot = (int)i; break; }
    for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++) {
        if (!s_fonts[i].used && !s_fonts[i].closing && s_fonts[i].refs == 0 && !s_fonts[i].ownedPgf) {
            s_fonts[i].used = -1; slot = (int)i; break;
        }
    }
    if (slot >= 0 && libslot >= 0) s_fontlibs[libslot].pending++;
    else if (slot >= 0) memset(&s_fonts[slot], 0, sizeof(s_fonts[slot]));
    hle_unlock();
    if (slot < 0 || libslot < 0) {
        if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_TOO_MANY);
        if (ownedPgf) pgf_close(ownedPgf);
        return 0;
    }
    uint32_t allocFn = L.allocFn, userData = L.userData;
    uint32_t fh = font_call_alloc(s, allocFn, userData, SR_FONT_SIZE);
    if (fh) {
        font_zero(fh, SR_FONT_SIZE);
        MEM_W32(fh + 0x00, SR_FONT_MAGIC);
        MEM_W32(fh + 0x04, lib);
        MEM_W32(fh + 0x08, index);
        MEM_W32(fh + 0x0c, (pgf && pgf == s_pgf_jpn) ? 1u : 0u);
        MEM_W32(fh + 0x10, mode);
    }
    hle_lock();
    if (fh) {
        s_fonts[slot].used = 1; s_fonts[slot].closing = 0; s_fonts[slot].refs = 0;
        s_fonts[slot].handle = fh; s_fonts[slot].lib = lib;
        s_fonts[slot].pgf = pgf; s_fonts[slot].ownedPgf = ownedPgf;
        s_fonts[slot].index = (int)index; s_fonts[slot].spec = spec;
        s_fontlibs[libslot].pending--; s_fontlibs[libslot].openCount++;
        MEM_W32(lib + 0x14, (uint32_t)s_fontlibs[libslot].openCount);
    } else {
        memset(&s_fonts[slot], 0, sizeof(s_fonts[slot]));
        if (s_fontlibs[libslot].used == 1 && s_fontlibs[libslot].pending > 0)
            s_fontlibs[libslot].pending--;
    }
    hle_unlock();
    if (!fh && ownedPgf) pgf_close(ownedPgf);
    if (errPtr) MEM_W32(errPtr, fh ? 0 : SR_FONT_ERROR_OOM);
    return fh;
}
static uint32_t h_FontOpen(CpuState *s) {
    /* sceFontOpen(a0=lib, a1=index, a2=mode, a3=u32* errorCode). */
    const PGF *pgf = font_pgf_for_index(A1);
    const SrFontSpec *spec = A1 < 18 ? &s_font_specs[A1] : NULL;
    if (!pgf || !spec->fileName) {
        if (A3) MEM_W32(A3, SR_FONT_ERROR_INVALID_DATA);
        return 0;
    }
    uint32_t fh = font_open_common(s, A0, A1, A2, pgf, NULL, spec, A3);
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "h_FontOpen: lib=0x%08x index=%u mode=%u -> font=0x%08x\n", A0, A1, A2, fh);
    return fh;
}
static uint32_t h_FontOpenUserMemory(CpuState *s) {
    /* sceFontOpenUserMemory(a0=lib, a1=memAddr, a2=memLen, a3=u32* errorCode). */
    font_load();
    PGF *owned = NULL;
    if (A1 && A2 >= 16 && A2 <= 16u * 1024u * 1024u) {
        uint8_t *copy = (uint8_t *)malloc(A2);
        if (copy) {
            for (uint32_t i = 0; i < A2; i++) copy[i] = MEM_R8(A1 + i);
            owned = pgf_open_memory(copy, A2);
            free(copy);
        }
    }
    uint32_t fh = font_open_common(s, A0, 0, 4, owned, owned, NULL, A3);
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "h_FontOpenUserMemory: lib=0x%08x mem=0x%08x len=%u -> font=0x%08x\n", A0, A1, A2, fh);
    return fh;
}
static uint32_t h_FontClose(CpuState *s) {
    /* sceFontClose(a0=font): retire the native font record. The guest struct's pool memory is
     * retained for the library's lifetime (the game frees the whole Fpl pool at teardown); we do
     * not replay the guest freeFunc here to avoid dispatching guest code from a Close reachable
     * on the save-icon render path. */
    PGF *destroy = NULL; uint32_t lib = 0; int found = 0; SrFontLib owner; memset(&owner, 0, sizeof(owner));
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++) {
        SrFont *f = &s_fonts[i];
        if (f->used == 1 && f->handle == A0) {
            found = 1; lib = f->lib; f->used = 0; f->closing = 1;
            if (f->refs == 0 && f->ownedPgf) { destroy = f->ownedPgf; f->ownedPgf = NULL; f->pgf = NULL; }
            if (f->refs == 0) memset(f, 0, sizeof(*f));
            for (size_t j = 0; j < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); j++)
                if (s_fontlibs[j].used == 1 && s_fontlibs[j].handle == lib && s_fontlibs[j].openCount > 0) {
                    owner = s_fontlibs[j];
                    s_fontlibs[j].openCount--; MEM_W32(lib + 0x14, (uint32_t)s_fontlibs[j].openCount); break;
                }
            break;
        }
    }
    hle_unlock();
    if (destroy) pgf_close(destroy);
    if (found && owner.freeFn)
        (void)ge_call_guest_rv(s, owner.freeFn, owner.userData, A0, 0);
    return found ? 0 : SR_FONT_ERROR_INVALID_ARG;
}
static uint32_t h_FontDoneLib(CpuState *s) {
    /* sceFontDoneLib(a0=lib): retire the library and any fonts still open under it. */
    PGF *destroy[64]; uint32_t handles[64]; size_t ndestroy = 0, nhandles = 0;
    int found = 0; SrFontLib owner; memset(&owner, 0, sizeof(owner));
    hle_lock();
    for (size_t j = 0; j < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); j++) {
        if (s_fontlibs[j].used == 1 && s_fontlibs[j].handle == A0) {
            if (s_fontlibs[j].pending) { hle_unlock(); return SR_FONT_ERROR_INVALID_ARG; }
            found = 1; owner = s_fontlibs[j]; memset(&s_fontlibs[j], 0, sizeof(s_fontlibs[j])); break;
        }
    }
    if (found) for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++) {
        SrFont *f = &s_fonts[i];
        if (f->used == 1 && f->lib == A0) {
            handles[nhandles++] = f->handle;
            f->used = 0; f->closing = 1;
            if (f->refs == 0 && f->ownedPgf) {
                destroy[ndestroy++] = f->ownedPgf; f->ownedPgf = NULL; f->pgf = NULL;
            }
            if (f->refs == 0) memset(f, 0, sizeof(*f));
        }
    }
    hle_unlock();
    for (size_t i = 0; i < ndestroy; i++) pgf_close(destroy[i]);
    if (found && owner.freeFn) {
        for (size_t i = 0; i < nhandles; i++)
            (void)ge_call_guest_rv(s, owner.freeFn, owner.userData, handles[i], 0);
        (void)ge_call_guest_rv(s, owner.freeFn, owner.userData, A0, 0);
    }
    return found ? 0 : SR_FONT_ERROR_INVALID_LIB;
}
static uint32_t h_FontGetFontInfo(CpuState *s) {
    font_load();
    SrFont *f = NULL; const PGF *p = font_acquire(A0, &f);
    if (!p || !A1) { font_release(f); return SR_FONT_ERROR_INVALID_ARG; }
    for (int i = 0; i < 0x108; i++) MEM_W8(A1 + (uint32_t)i, 0);
    pgf_get_font_info(p, A1);
    font_overlay_style(A1, f ? f->spec : NULL, f ? (f->ownedPgf != NULL) : 1);
    font_release(f);
    return 0;
}
uint32_t h_FontGetCharInfo(CpuState *s) {
    uint32_t ci = A2; if (!ci) return SR_FONT_ERROR_INVALID_ARG;
    uint32_t cc = A1 & 0xffff;
    SrFont *f = NULL; const PGF *p = font_pgf_for(A0, (int)cc, &f);
    if (p) { pgf_get_char_info(p, (int)cc, 0x5f, ci); font_release(f); return 0; }
    font_release(f);
    return SR_FONT_ERROR_INVALID_ARG;
#if 0 /* Retired synthetic metrics fallback. */
    for (int i = 0; i < 0x3c; i++) MEM_W8(ci + (uint32_t)i, 0);
    int draw = (cc > 32);                      /* space and controls: empty */
    MEM_W32(ci + 0, draw ? 8 : 0);             /* bitmapWidth */
    MEM_W32(ci + 4, draw ? 11 : 0);            /* bitmapHeight */
    MEM_W32(ci + 8, 0);                        /* bitmapLeft */
    MEM_W32(ci + 12, 0);                       /* bitmapTop */
    MEM_W32(ci + 16, 8 << 6);                  /* sfp26Width */
    MEM_W32(ci + 20, 11 << 6);                 /* sfp26Height */
    MEM_W32(ci + 24, 0);                       /* ascender */
    MEM_W32(ci + 28, (uint32_t)(-(11 << 6)));  /* descender */
    MEM_W32(ci + 32, 0);                       /* bearingHX */
    MEM_W32(ci + 36, 0);                       /* bearingHY */
    MEM_W32(ci + 48, 8 << 6);                  /* sfp26AdvanceH */
    MEM_W32(ci + 52, 12 << 6);                 /* sfp26AdvanceV */
    return 0;
#endif
}

#if 0 /* Retired synthetic bitmap font. */
static const uint8_t *font5x7(uint32_t cc) {
    if (cc >= 'a' && cc <= 'z') cc -= 32;
    switch (cc) {
        case 'A': { static const uint8_t r[7] = {14,17,17,31,17,17,17}; return r; }
        case 'B': { static const uint8_t r[7] = {30,17,17,30,17,17,30}; return r; }
        case 'C': { static const uint8_t r[7] = {14,17,16,16,16,17,14}; return r; }
        case 'D': { static const uint8_t r[7] = {30,17,17,17,17,17,30}; return r; }
        case 'E': { static const uint8_t r[7] = {31,16,16,30,16,16,31}; return r; }
        case 'F': { static const uint8_t r[7] = {31,16,16,30,16,16,16}; return r; }
        case 'G': { static const uint8_t r[7] = {14,17,16,23,17,17,15}; return r; }
        case 'H': { static const uint8_t r[7] = {17,17,17,31,17,17,17}; return r; }
        case 'I': { static const uint8_t r[7] = {14,4,4,4,4,4,14}; return r; }
        case 'J': { static const uint8_t r[7] = {7,2,2,2,18,18,12}; return r; }
        case 'K': { static const uint8_t r[7] = {17,18,20,24,20,18,17}; return r; }
        case 'L': { static const uint8_t r[7] = {16,16,16,16,16,16,31}; return r; }
        case 'M': { static const uint8_t r[7] = {17,27,21,21,17,17,17}; return r; }
        case 'N': { static const uint8_t r[7] = {17,25,21,19,17,17,17}; return r; }
        case 'O': { static const uint8_t r[7] = {14,17,17,17,17,17,14}; return r; }
        case 'P': { static const uint8_t r[7] = {30,17,17,30,16,16,16}; return r; }
        case 'Q': { static const uint8_t r[7] = {14,17,17,17,21,18,13}; return r; }
        case 'R': { static const uint8_t r[7] = {30,17,17,30,20,18,17}; return r; }
        case 'S': { static const uint8_t r[7] = {15,16,16,14,1,1,30}; return r; }
        case 'T': { static const uint8_t r[7] = {31,4,4,4,4,4,4}; return r; }
        case 'U': { static const uint8_t r[7] = {17,17,17,17,17,17,14}; return r; }
        case 'V': { static const uint8_t r[7] = {17,17,17,17,17,10,4}; return r; }
        case 'W': { static const uint8_t r[7] = {17,17,17,21,21,21,10}; return r; }
        case 'X': { static const uint8_t r[7] = {17,17,10,4,10,17,17}; return r; }
        case 'Y': { static const uint8_t r[7] = {17,17,10,4,4,4,4}; return r; }
        case 'Z': { static const uint8_t r[7] = {31,1,2,4,8,16,31}; return r; }
        case '0': { static const uint8_t r[7] = {14,17,19,21,25,17,14}; return r; }
        case '1': { static const uint8_t r[7] = {4,12,4,4,4,4,14}; return r; }
        case '2': { static const uint8_t r[7] = {14,17,1,2,4,8,31}; return r; }
        case '3': { static const uint8_t r[7] = {30,1,1,14,1,1,30}; return r; }
        case '4': { static const uint8_t r[7] = {2,6,10,18,31,2,2}; return r; }
        case '5': { static const uint8_t r[7] = {31,16,16,30,1,1,30}; return r; }
        case '6': { static const uint8_t r[7] = {6,8,16,30,17,17,14}; return r; }
        case '7': { static const uint8_t r[7] = {31,1,2,4,8,8,8}; return r; }
        case '8': { static const uint8_t r[7] = {14,17,17,14,17,17,14}; return r; }
        case '9': { static const uint8_t r[7] = {14,17,17,15,1,2,12}; return r; }
        case '.': { static const uint8_t r[7] = {0,0,0,0,0,12,12}; return r; }
        case ',': { static const uint8_t r[7] = {0,0,0,0,0,12,8}; return r; }
        case ':': { static const uint8_t r[7] = {0,12,12,0,12,12,0}; return r; }
        case '/': { static const uint8_t r[7] = {1,1,2,4,8,16,16}; return r; }
        case '-': { static const uint8_t r[7] = {0,0,0,31,0,0,0}; return r; }
        case '+': { static const uint8_t r[7] = {0,4,4,31,4,4,0}; return r; }
        case '!': { static const uint8_t r[7] = {4,4,4,4,4,0,4}; return r; }
        case '?': { static const uint8_t r[7] = {14,17,1,2,4,0,4}; return r; }
        case '(' : { static const uint8_t r[7] = {2,4,8,8,8,4,2}; return r; }
        case ')' : { static const uint8_t r[7] = {8,4,2,2,2,4,8}; return r; }
        case '\'' : { static const uint8_t r[7] = {4,4,8,0,0,0,0}; return r; }
        default: return NULL;
    }
}

static void font_write_pixel(uint32_t base, uint32_t fmt, int bpl, int bufW, int bufH, int px, int py, uint8_t val) {
    static const int pxBytes[5] = { 0, 0, 1, 3, 4 };
    if (fmt > 4 || px < 0 || px >= bufW || py < 0 || py >= bufH) return;
    int pb = pxBytes[fmt];
    uint32_t a = base + (uint32_t)(py * bpl) + (uint32_t)(pb == 0 ? px / 2 : px * pb);
    switch (fmt) {
        case 0: case 1: {
            uint8_t old = MEM_R8(a);
            uint8_t pix = val >> 4;
            if ((px & 1) != (int)fmt) MEM_W8(a, (uint8_t)((pix << 4) | (old & 0x0F)));
            else MEM_W8(a, (uint8_t)((old & 0xF0) | pix));
            break;
        }
        case 2: MEM_W8(a, val); break;
        case 3: MEM_W8(a, val); MEM_W8(a + 1, val); MEM_W8(a + 2, val); break;
        case 4: MEM_W32(a, (uint32_t)val | ((uint32_t)val << 8) | ((uint32_t)val << 16) | ((uint32_t)val << 24)); break;
    }
}
#endif

uint32_t h_FontGetCharGlyphImage(CpuState *s) {
    uint32_t cc = A1 & 0xffff, gi = A2;
    if (!gi) return SR_FONT_ERROR_INVALID_ARG;
    SrFont *f = NULL; const PGF *p = font_pgf_for(A0, (int)cc, &f);
    if (p) {
        (void)pgf_draw_glyph(p, (int)cc, 0x5f, gi);
        font_release(f); return 0;
    }
    font_release(f);
    return SR_FONT_ERROR_INVALID_ARG;
#if 0 /* Retired synthetic glyph fallback. */
    uint32_t fmt = MEM_R32(gi + 0);
    int x = (int)MEM_R32(gi + 4) >> 6, y = (int)MEM_R32(gi + 8) >> 6;
    int bufW = (int)(MEM_R16(gi + 12)), bufH = (int)(MEM_R16(gi + 14));
    int bpl = (int)(MEM_R16(gi + 16));
    uint32_t base = MEM_R32(gi + 20);
    if (getenv("SR_FONTLOG")) { static int n = 0; if (n++ < 12)
        fprintf(stderr, "glyph cc=0x%04x fmt=%u xy=(%d,%d) buf=0x%08x %dx%d bpl=%d\n", cc, fmt, x, y, base, bufW, bufH, bpl); }
    const uint8_t *rows = font5x7(cc);
    if (!rows && getenv("SR_FONTLOG")) { static unsigned char seen[65536]; if (!seen[cc]) { seen[cc] = 1;
        fprintf(stderr, "MISSING glyph cc=0x%04x '%c'\n", cc, (cc >= 32 && cc < 127) ? (char)cc : '?'); } }
    for (int yy = 0; yy < 11; yy++) for (int xx = 0; xx < 8; xx++) {
        int gx = xx - 1, gy = yy - 2;
        int on = rows && gx >= 0 && gx < 5 && gy >= 0 && gy < 7 && (rows[gy] & (1u << (4 - gx)));
        font_write_pixel(base, fmt, bpl, bufW, bufH, x + xx, y + yy, on ? 0xFF : 0x00);
    }
    return 0;
#endif
}

/* Some libfont revisions expose an internal glyph-table-id entry point even though the public
 * sceLibFont NID table only names the character-code form.  Keep the implementation available
 * to the module loader without inventing a firmware NID. */
uint32_t h_FontGetGlyphImageById(CpuState *s) {
    if (!A2) return SR_FONT_ERROR_INVALID_ARG;
    SrFont *f = NULL; const PGF *p = font_acquire(A0, &f);
    if (!p) { font_release(f); return SR_FONT_ERROR_INVALID_ARG; }
    int drawn = pgf_draw_glyph_by_id(p, (int)A1, A2);
    font_release(f);
    return drawn ? 0 : SR_FONT_ERROR_INVALID_ARG;
}
static uint32_t h_FontFindOptimumFont(CpuState *s) {
    if (A2) MEM_W32(A2, 0);            /* *errorCode */
    return 0;                          /* font index 0 */
}
/* ---- sceMpeg (PSMF video): faithful port in src/rt/mpeg.c (from PPSSPP) ----
 * These thin wrappers marshal MIPS args to the ported mpeg_* functions. The port implements the
 * real PSMF analysis, ring-buffer accounting, handle/context creation, stream registration, and
 * AU getters with timestamp progression + end-of-stream, so the movie playback loop runs and
 * completes exactly as on PPSSPP. The SDL3 build decodes AVC video through Windows Media
 * Foundation (h264_mf.c); ATRAC movie audio is still modelled as silence. */
uint32_t mpeg_init(void);
uint32_t mpeg_finish(void);
uint32_t mpeg_query_mem_size(uint32_t outAddr);
uint32_t mpeg_ringbuffer_query_mem_size(uint32_t packets);
uint32_t mpeg_ringbuffer_construct(uint32_t ring, uint32_t numPackets, uint32_t data, uint32_t size, uint32_t cbAddr, uint32_t cbArg);
uint32_t mpeg_create(uint32_t mpegAddr, uint32_t dataPtr, uint32_t size, uint32_t ringAddr, uint32_t frameWidth, uint32_t mode, uint32_t ddrTop);
uint32_t mpeg_delete(uint32_t mpegAddr);
uint32_t mpeg_query_stream_offset(uint32_t mpegAddr, uint32_t bufferAddr, uint32_t offsetAddr);
uint32_t mpeg_query_stream_size(uint32_t bufferAddr, uint32_t sizeAddr);
uint32_t mpeg_regist_stream(uint32_t mpegAddr, uint32_t streamType, uint32_t streamNum);
uint32_t mpeg_unregist_stream(uint32_t mpegAddr, uint32_t sid);
uint32_t mpeg_ringbuffer_available_size(uint32_t ring);
uint32_t mpeg_ringbuffer_put(CpuState *s, uint32_t ring, uint32_t numPackets, uint32_t available);
uint32_t mpeg_get_avc_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr);
uint32_t mpeg_get_atrac_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr);
uint32_t mpeg_avc_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t initAddr);
uint32_t mpeg_atrac_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t bufferAddr, uint32_t init);
uint32_t mpeg_avc_decode_stop(uint32_t mpegAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t statusAddr);
uint32_t mpeg_malloc_avc_es_buf(uint32_t mpegAddr);
uint32_t mpeg_free_avc_es_buf(uint32_t mpegAddr, uint32_t esBuf);
uint32_t mpeg_init_au(uint32_t mpegAddr, uint32_t esBuffer, uint32_t auAddr);
uint32_t mpeg_query_atrac_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr);

static uint32_t h_MpegInit(CpuState *s) { (void)s; return mpeg_init(); }
static uint32_t h_MpegMallocAvcEsBuf(CpuState *s) { return mpeg_malloc_avc_es_buf(A0); }
static uint32_t h_MpegFreeAvcEsBuf(CpuState *s) { return mpeg_free_avc_es_buf(A0, A1); }
static uint32_t h_MpegInitAu(CpuState *s) { return mpeg_init_au(A0, A1, A2); }
static uint32_t h_MpegQueryAtracEsSize(CpuState *s) { return mpeg_query_atrac_es_size(A0, A1, A2); }
static uint32_t h_MpegFinish(CpuState *s) { (void)s; return mpeg_finish(); }
/* sceMpegQueryMemSize() takes no args and RETURNS the context size in v0 (PPSSPP MpegRequiredMem:
 * 0x10000 for lib version >= 0x0105, which ACX uses). The earlier wrapper wrote it to a pointer and
 * returned 0, so the game allocated a 0-byte mpeg buffer and sceMpegCreate failed with NO_MEMORY. */
static uint32_t h_MpegQueryMemSize(CpuState *s) { (void)s; return 0x10000u; }
static uint32_t h_MpegCreate(CpuState *s) {
    uint32_t r = mpeg_create(A0, A1, A2, A3, stack_arg(s, 0), stack_arg(s, 1), stack_arg(s, 2));
    if (getenv("SR_MPEGLOG")) fprintf(stderr, "MpegCreate mpegAddr=0x%x data=0x%x size=0x%x ring=0x%x fw=%u -> 0x%x\n",
        A0, A1, A2, A3, stack_arg(s,0), r);
    return r;
}
static uint32_t h_MpegDelete(CpuState *s) { return mpeg_delete(A0); }
static uint32_t h_MpegRingbufferQueryMemSize(CpuState *s) { return mpeg_ringbuffer_query_mem_size(A0); }
static uint32_t h_MpegRingbufferConstruct(CpuState *s) { return mpeg_ringbuffer_construct(A0, A1, A2, A3, stack_arg(s, 0), stack_arg(s, 1)); }
static uint32_t h_MpegRingbufferAvailable(CpuState *s) { return mpeg_ringbuffer_available_size(A0); }
static uint32_t h_MpegRingbufferPut(CpuState *s) { return mpeg_ringbuffer_put(s, A0, A1, A2); }
static uint32_t h_MpegRegistStream(CpuState *s) { return mpeg_regist_stream(A0, A1, A2); }
static uint32_t h_MpegUnRegistStream(CpuState *s) { return mpeg_unregist_stream(A0, A1); }
static uint32_t h_MpegQueryStreamOffset(CpuState *s) { return mpeg_query_stream_offset(A0, A1, A2); }
static uint32_t h_MpegQueryStreamSize(CpuState *s) { return mpeg_query_stream_size(A0, A1); }
/* PPSSPP returns these via hleDelayResult so the playback thread yields (it does not busy-poll the
 * ring). Mirror that: delay the calling thread a frame-ish, longer when there is no data yet, so the
 * feeder/display threads run and the movie paces instead of hanging the scheduler. */
static uint32_t h_MpegGetAvcAu(CpuState *s) {
    uint32_t r = mpeg_get_avc_au(A0, A1, A2, A3);
    sched_delay_current(r == 0x80618001u ? 8000u : 3000u);
    return r;
}
static uint32_t h_MpegGetAtracAu(CpuState *s) {
    uint32_t r = mpeg_get_atrac_au(A0, A1, A2, A3);
    sched_delay_current(r == 0x80618001u ? 8000u : 3000u);
    return r;
}
/* PPSSPP charges real decode latency (sceMpeg.cpp: avcDecodeDelayMs=5400, atracDecodeDelayMs=3000,
 * passed to hleDelayResult in microseconds). Besides pacing, the delay is a guaranteed yield. */
static uint32_t h_MpegAvcDecode(CpuState *s) {
    uint32_t r = mpeg_avc_decode(A0, A1, A2, A3, stack_arg(s, 0));
    sched_delay_current(5400);
    return r;
}
static uint32_t h_MpegAtracDecode(CpuState *s) {
    uint32_t r = mpeg_atrac_decode(A0, A1, A2, A3);
    sched_delay_current(3000);
    return r;
}
static uint32_t h_MpegAvcDecodeStop(CpuState *s) { return mpeg_avc_decode_stop(A0, A1, A2, A3); }

/* sceAtrac3plus: control-flow model with the real streaming contract. The game feeds a track in
 * chunks: SetData installs the first chunk (buffer + size), then the audio thread polls
 * sceAtracGetStreamDataInfo for the ring write pointer/free bytes, reads the next file chunk into
 * it, and calls sceAtracAddStreamData. DecodeData consumes one frame (2048 samples, bytesPerFrame
 * bytes) per call from the fed data and only advances while data is available. GetRemainFrame and
 * GetStreamDataInfo report the honest ring state so the feed loop can advance. The earlier model
 * prevented the guest from streaming most of the track: it claimed ALLDATA_IS_ON_MEMORY with zero
 * writable bytes, so the guest starved at ~8% of the track. The streaming contract is now modeled
 * sufficiently for the guest to feed the complete ATRAC stream (the title BGM feeds end-to-end and
 * reaches DecodeData repeatedly). ATRAC3+ frames are decoded for real through the imported
 * decoder bridge (src/rt/atrac3p_bridge.h, #286); ATRAC3 (0x1001) has no decoder in this tree and
 * DecodeData reports the failure instead of fabricating PCM. Ported semantics from PPSSPP
 * Core/HLE/sceAtrac.cpp + AtracCtx.cpp (stream bookkeeping), minus the media engine. */
#define ATRAC_SAMPLES_PER_FRAME 2048
#define ATRAC_CODEC_AT3PLUS 0x1000u
#define ATRAC_CODEC_AT3 0x1001u
#define ATRAC_ERROR_NO_ATRACID 0x80630003u
#define ATRAC_ERROR_INVALID_CODECTYPE 0x80630004u
#define ATRAC_ERROR_BAD_ATRACID 0x80630005u
#define ATRAC_ERROR_UNKNOWN_FORMAT 0x80630006u
#define ATRAC_ERROR_ALLDATA_LOADED 0x80630009u
#define ATRAC_ERROR_NO_DATA 0x80630010u
#define ATRAC_ERROR_SIZE_TOO_SMALL 0x80630011u
#define ATRAC_ERROR_ADD_DATA_IS_TOO_BIG 0x80630018u
#define ATRAC_ERROR_NO_LOOP_INFORMATION 0x80630021u
#define ATRAC_ERROR_ALLDATA_DECODED 0x80630022u
#define ATRAC_PSP_ALLDATA_IS_ON_MEMORY 0xFFFFFFFFu
#define ATRAC_PSP_NONLOOP_STREAM_IS_ON_MEMORY 0xFFFFFFFEu
#define ATRAC_PSP_LOOP_STREAM_IS_ON_MEMORY 0xFFFFFFFDu
#define ATRAC_STATE_NO_DATA 1
#define ATRAC_STATE_ALL_DATA_LOADED 2
#define ATRAC_STATE_HALFWAY_BUFFER 3
#define ATRAC_STATE_STREAMED_NO_LOOP 4
#define ATRAC_STATE_STREAMED_LOOP_END 5
#define ATRAC_STATE_STREAMED_LOOP_TRAILER 6
#define ATRAC_RIFF_SCAN_MAX 0x00100000u
typedef struct {
    int used;
    uint32_t codecType;
    uint32_t buf;                 /* guest ring buffer base */
    uint32_t size;                /* bytes of file data fed so far (first_.size) */
    uint32_t bufferMaxSize;       /* ring capacity (SetData bufferSize) */
    uint32_t fileSize;            /* total track size in bytes (RIFF extent) */
    uint32_t dataByteOffset;      /* file offset where audio frames start */
    uint32_t bytesPerFrame;       /* frame size (RIFF fmt blockAlign) */
    int endSample, posSample, loopNum;
    int loopStartSample, loopEndSample;   /* 'smpl' loop points; -1 when absent */
    uint32_t bufferPos;           /* ring offset of the next frame to consume */
    uint32_t bufferValidBytes;    /* bytes available for decode in the ring */
    uint32_t bufferHeaderSize;    /* physical header prefix; becomes zero after first wrap */
    int bufferState;              /* ATRAC_STATE_* */
    /* Host-side FIFO view of the same guest ring. The PSP stream may split one
     * encoded frame across the physical end; keeping the valid bytes in FIFO
     * order lets the decoder consume that lawful split without rereading the
     * RIFF prefix or assuming a fixed wrap base. */
    uint8_t *streamWindow;
    uint32_t streamWindowSize;
    uint32_t streamWindowRead;
    uint32_t streamWindowWrite;
    uint32_t streamWindowQueued;
    /* PR-B: real ATRAC3+ decode through the imported decoder. dec is NULL
     * until a track with a known codec/channels/blockAlign is configured;
     * host_frame/host_pcm are per-slot staging owned by this context. */
    Atrac3pBridge *dec;           /* owned decoder bridge, or NULL */
    int channels;                 /* RIFF fmt channels (1..8) */
    int dec_channels;             /* bridge config (channels) at create time */
    int dec_align;                /* bridge config (bytesPerFrame) at create time */
    uint8_t *host_frame;          /* one frame copied out of the guest ring */
    int host_frame_cap;           /* allocated size of host_frame */
    int16_t *host_pcm;            /* decode output staging, channels*2048 */
} Atrac;
static Atrac s_atrac[8];
static int atrac_streamed_state(const Atrac *a);
static uint32_t atrac_stream_buffer_end(const Atrac *a);
static uint32_t atrac_remaining_frames(const Atrac *a);

static void atrac_stream_window_free(Atrac *a) {
    free(a->streamWindow);
    a->streamWindow = NULL;
    a->streamWindowSize = 0;
    a->streamWindowRead = 0;
    a->streamWindowWrite = 0;
    a->streamWindowQueued = 0;
}
static int atrac_stream_window_init(Atrac *a) {
    atrac_stream_window_free(a);
    if (!atrac_streamed_state(a) || a->bufferMaxSize == 0u)
        return 1;
    a->streamWindow = (uint8_t *)malloc(a->bufferMaxSize);
    if (!a->streamWindow) return 0;
    a->streamWindowSize = a->bufferMaxSize;
    a->streamWindowRead = 0;
    a->streamWindowWrite = 0;
    a->streamWindowQueued = 0;
    for (uint32_t i = 0; i < a->bufferValidBytes; i++) {
        uint32_t phys = a->bufferMaxSize ?
            (a->bufferPos + i) % a->bufferMaxSize : 0u;
        a->streamWindow[a->streamWindowWrite] =
            (uint8_t)MEM_R8(a->buf + phys);
        a->streamWindowWrite++;
        if (a->streamWindowWrite == a->streamWindowSize)
            a->streamWindowWrite = 0;
        a->streamWindowQueued++;
    }
    return 1;
}
static int atrac_stream_window_append(Atrac *a, uint32_t physical,
                                      uint32_t bytes) {
    if (!bytes) return 1;
    if (!a->streamWindow || !a->streamWindowSize ||
        bytes > a->streamWindowSize - a->streamWindowQueued)
        return 0;
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t phys = a->bufferMaxSize ?
            (physical + i) % a->bufferMaxSize : physical + i;
        a->streamWindow[a->streamWindowWrite] =
            (uint8_t)MEM_R8(a->buf + phys);
        a->streamWindowWrite++;
        if (a->streamWindowWrite == a->streamWindowSize)
            a->streamWindowWrite = 0;
    }
    a->streamWindowQueued += bytes;
    return 1;
}
static int atrac_stream_window_peek(const Atrac *a, uint8_t *out,
                                    uint32_t bytes) {
    if (!bytes || !a->streamWindow ||
        bytes > a->streamWindowQueued)
        return 0;
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t p = a->streamWindowRead + i;
        if (p >= a->streamWindowSize) p -= a->streamWindowSize;
        out[i] = a->streamWindow[p];
    }
    return 1;
}
static void atrac_stream_window_consume(Atrac *a, uint32_t bytes) {
    if (!bytes || !a->streamWindow) return;
    if (bytes > a->streamWindowQueued) bytes = a->streamWindowQueued;
    a->streamWindowRead += bytes;
    a->streamWindowRead %= a->streamWindowSize;
    a->streamWindowQueued -= bytes;
}
static int atrac_parse_track(Atrac *a, uint32_t buf, uint32_t size) {
    /* Scan the RIFF for "fmt " (frame size / blockAlign), "fact" (total samples),
     * "data" (frame payload offset) and "smpl" (loop points). The input is
     * guest-controlled: validate the complete supplied span first, then do all
     * chunk arithmetic in checked offsets so a hostile size cannot wrap the
     * cursor or make a malformed buffer look valid. Everything is parsed into
     * locals and committed only on success so a rejected buffer leaves the
     * previously configured context untouched. */
    if (size < 44u || !sr_guest_span_readable(buf, size)) return 0;
    if (MEM_R32(buf) != 0x46464952u /* 'RIFF' */ ||
        MEM_R32(buf + 8u) != 0x45564157u /* 'WAVE' */) return 0;

    uint32_t riff_end;
    uint32_t riff_size = MEM_R32(buf + 4u);
    if (!sr_size_add_ok(8u, riff_size, &riff_end) || riff_end < 44u)
        return 0;
    /* sceAtracSetData may receive only the currently filled prefix of a streamed
     * track.  The RIFF extent describes the eventual file, not necessarily this
     * call's buffer, so scan only the checked bytes that are available now. */
    uint32_t scan_end = size < riff_end ? size : riff_end;
    if (scan_end > ATRAC_RIFF_SCAN_MAX) scan_end = ATRAC_RIFF_SCAN_MAX;

    uint32_t fileSize = riff_end;
    uint32_t dataByteOffset = 0;
    uint32_t bytesPerFrame = 0;
    int channels = 0;
    int endSample = 0;
    int loopStartSample = -1, loopEndSample = -1;
    for (uint32_t off = 12u; off <= scan_end - 8u;) {
        uint32_t p;
        uint32_t next;
        uint32_t chunk_bytes;
        if (!sr_size_add_ok(buf, off, &p)) return 0;
        uint32_t id = MEM_R32(p), sz = MEM_R32(p + 4u);
        /* Bytes of this chunk's payload actually inside the validated span
         * (off+8 <= scan_end <= size by the loop condition). Chunks whose
         * payload we read must fit fully, or the buffer is rejected; a chunk
         * header may still be followed by data beyond the fed prefix, which
         * the 'data' handling below tolerates. */
        uint32_t avail = scan_end - (off + 8u);
        if (id == 0x74636166u /* 'fact' */) {
            if (sz < 4u || sz > avail) return 0;
            uint32_t samples = MEM_R32(p + 8u);
            if (samples == 0u || samples > 0x7fffffffu) return 0;
            endSample = (int)samples;
        } else if (id == 0x20746d66u /* 'fmt ' */) {
            if (sz < 16u || sz > avail) return 0;
            uint32_t ch = MEM_R16(p + 10u);      /* channels */
            uint32_t align = MEM_R16(p + 20u);   /* blockAlign = bytes per frame */
            if (ch < 1u || ch > 8u) return 0;
            if (align < 1u || align > 0x10000u) return 0;
            channels = (int)ch;
            bytesPerFrame = align;
        } else if (id == 0x61746164u /* 'data' */) {
            if (!sr_size_add_ok(off, 8u, &dataByteOffset)) return 0;
        } else if (id == 0x6c706d73u /* 'smpl' */) {
            /* payload: manufacturer, product, samplePeriod, MIDIUnityNote,
             * MIDIPitchFraction, SMPTEFormat, SMPTEOffset, numSampleLoops(28),
             * samplerData(32), then 24-byte loop records at 36+. */
            if (sz > avail) return 0;
            if (sz >= 36u + 24u) {
                uint32_t numLoops = MEM_R32(p + 36u);
                if (numLoops >= 1u) {
                    uint32_t start = MEM_R32(p + 52u), end = MEM_R32(p + 56u);
                    if (start < 0x80000000u && end <= 0x7fffffffu &&
                        (start != 0u || end != 0u)) {
                        loopStartSample = (int)start;
                        loopEndSample = (int)end;
                    }
                }
            }
        }
        if (!sr_size_add_ok(sz, sz & 1u, &chunk_bytes) ||
            !sr_size_add_ok(off, 8u, &next) ||
            !sr_size_add_ok(next, chunk_bytes, &next))
            return 0;
        /* A chunk payload may legitimately extend past the currently fed
         * prefix of a streamed track (the 'data' chunk covers the whole audio).
         * Only the next chunk header must be inside the fed bytes; stop the
         * scan when the payload runs past them. */
        if (next > scan_end) break;
        off = next;
    }
    if (endSample <= 0) return 0;
    a->fileSize = fileSize;
    a->dataByteOffset = dataByteOffset;
    a->bytesPerFrame = bytesPerFrame;
    a->channels = channels;
    a->endSample = endSample;
    a->loopStartSample = loopStartSample;
    a->loopEndSample = loopEndSample;
    return 1;
}
static int atrac_streamed_state(const Atrac *a) {
    return a->bufferState == ATRAC_STATE_STREAMED_NO_LOOP ||
           a->bufferState == ATRAC_STATE_STREAMED_LOOP_END ||
           a->bufferState == ATRAC_STATE_STREAMED_LOOP_TRAILER;
}
static void atrac_update_buffer_state(Atrac *a) {
    if (a->bufferMaxSize >= a->fileSize) {
        a->bufferState = a->size < a->fileSize ? ATRAC_STATE_HALFWAY_BUFFER
                                               : ATRAC_STATE_ALL_DATA_LOADED;
    } else if (a->loopEndSample <= 0) {
        a->bufferState = ATRAC_STATE_STREAMED_NO_LOOP;
    } else if ((uint32_t)a->loopEndSample >= (uint32_t)a->endSample) {
        a->bufferState = ATRAC_STATE_STREAMED_LOOP_END;
    } else {
        a->bufferState = ATRAC_STATE_STREAMED_LOOP_TRAILER;
    }
}
static uint32_t atrac_stream_buffer_end(const Atrac *a) {
    /* Ring end: frame-aligned capacity after the optional RIFF header. */
    if (a->bufferMaxSize <= a->bufferHeaderSize || a->bytesPerFrame == 0u)
        return a->bufferMaxSize;
    uint32_t frames = (a->bufferMaxSize - a->bufferHeaderSize) /
                      a->bytesPerFrame;
    return frames * a->bytesPerFrame + a->bufferHeaderSize;
}
static uint32_t atrac_stream_buffer_base(const Atrac *a) {
    return a->bufferHeaderSize;
}
static uint32_t atrac_file_offset_by_sample(const Atrac *a, int sample) {
    if (sample < 0) sample = 0;
    /* Frame*bytesPerFrame can exceed 32 bits for hostile RIFF metadata
     * (huge 'fact' sample count combined with a large blockAlign); clamp
     * so callers see an offset beyond any real file instead of a wrap. */
    uint64_t frame = (uint32_t)sample / ATRAC_SAMPLES_PER_FRAME;
    uint64_t byteOff = (uint64_t)a->dataByteOffset + frame * a->bytesPerFrame;
    return byteOff > 0xffffffffu ? 0xffffffffu : (uint32_t)byteOff;
}
static void atrac_calculate_stream_info(const Atrac *a, uint32_t *outOffset,
                                        uint32_t *outWritable, uint32_t *outReadOffset) {
    uint32_t readOffset = a->size;   /* file offset of the end of fed data */
    uint32_t offset = 0, writable = 0;
    if (a->bufferState == ATRAC_STATE_ALL_DATA_LOADED) {
        readOffset = 0; offset = 0; writable = 0;
    } else if (a->bufferState == ATRAC_STATE_HALFWAY_BUFFER) {
        offset = readOffset;
        writable = a->fileSize - readOffset;
    } else {
        uint32_t bufferEnd = atrac_stream_buffer_end(a);
        uint32_t validExtended = a->bufferPos + a->bufferValidBytes;
        if (validExtended < bufferEnd) {
            offset = validExtended;
            writable = bufferEnd - validExtended;
        } else {
            /* After the first lap the header prefix is gone and the physical
             * ring starts at zero, exactly as on the PSP/PPSSPP model. */
            uint32_t startUsed = validExtended - bufferEnd;
            offset = startUsed;
            writable = a->bufferPos > startUsed ? a->bufferPos - startUsed : 0;
        }
        if (readOffset >= a->fileSize) {
            if (a->bufferState == ATRAC_STATE_STREAMED_NO_LOOP) {
                /* Complete: nothing more to read. */
                readOffset = 0; offset = 0; writable = 0;
            } else {
                /* Loop from end: re-read from just before the loop point. */
                int loopOff = a->loopStartSample - ATRAC_SAMPLES_PER_FRAME * 2;
                readOffset = atrac_file_offset_by_sample(a, loopOff);
            }
        }
        {
            uint32_t endCheck;
            if (!sr_size_add_ok(readOffset, writable, &endCheck) ||
                endCheck > a->fileSize)
                writable = a->fileSize - readOffset;
        }
        if (offset + writable > a->bufferMaxSize) {
            offset = 0;
            writable = a->bufferMaxSize;
        }
    }
    if (outOffset) *outOffset = offset;
    if (outWritable) *outWritable = writable;
    if (outReadOffset) *outReadOffset = readOffset;
}
static void atrac_consume_frame(Atrac *a) {
    if (atrac_streamed_state(a))
        atrac_stream_window_consume(a, a->bytesPerFrame);
    a->bufferPos += a->bytesPerFrame;
    if (atrac_streamed_state(a)) {
        if (a->bufferValidBytes > a->bytesPerFrame)
            a->bufferValidBytes -= a->bytesPerFrame;
        else
            a->bufferValidBytes = 0;
    }
    uint32_t end = atrac_stream_buffer_end(a);
    if (a->bufferPos >= end) {
        a->bufferPos -= end;
        /* The data offset is a one-time header prefix. Once the read cursor
         * reaches the frame-aligned end, subsequent laps use the whole
         * physical buffer. */
        a->bufferHeaderSize = 0;
    }
}
static uint32_t atrac_remaining_frames(const Atrac *a) {
    if (a->bufferState == ATRAC_STATE_ALL_DATA_LOADED)
        return ATRAC_PSP_ALLDATA_IS_ON_MEMORY;
    uint32_t currentFileOffset =
        atrac_file_offset_by_sample(a, a->posSample - ATRAC_SAMPLES_PER_FRAME);
    if (a->size >= a->fileSize) {
        if (a->bufferState == ATRAC_STATE_STREAMED_NO_LOOP)
            return ATRAC_PSP_NONLOOP_STREAM_IS_ON_MEMORY;
        if (a->bufferState == ATRAC_STATE_STREAMED_LOOP_TRAILER &&
            a->posSample > a->loopEndSample)
            return ATRAC_PSP_NONLOOP_STREAM_IS_ON_MEMORY;
        if (atrac_streamed_state(a) && a->loopNum == 0)
            return ATRAC_PSP_LOOP_STREAM_IS_ON_MEMORY;
    }
    if (atrac_streamed_state(a))
        return a->bufferValidBytes / a->bytesPerFrame;
    int remainingBytes = (int)(a->size - currentFileOffset);
    if (remainingBytes < 0) remainingBytes = 0;
    return (uint32_t)remainingBytes / a->bytesPerFrame;
}
static int atrac_set_track(Atrac *a, uint32_t buf, uint32_t size) {
    /* Transactional: a rejected buffer leaves the previously configured
     * context untouched. */
    uint32_t oldBuf = a->buf;
    uint32_t oldSize = a->size;
    int oldPosSample = a->posSample;
    int oldLoopNum = a->loopNum;
    uint32_t oldBufferPos = a->bufferPos;
    uint32_t oldBufferValidBytes = a->bufferValidBytes;
    uint32_t oldBufferHeaderSize = a->bufferHeaderSize;
    uint8_t *oldStreamWindow = a->streamWindow;
    uint32_t oldStreamWindowSize = a->streamWindowSize;
    uint32_t oldStreamWindowRead = a->streamWindowRead;
    uint32_t oldStreamWindowWrite = a->streamWindowWrite;
    uint32_t oldStreamWindowQueued = a->streamWindowQueued;
    a->buf = buf;
    a->size = size;
    a->posSample = 0;
    a->loopNum = 0;
    a->bufferPos = 0;
    a->bufferValidBytes = 0;
    a->bufferHeaderSize = 0;
    if (!atrac_parse_track(a, buf, size)) {
        a->buf = oldBuf;
        a->size = oldSize;
        a->posSample = oldPosSample;
        a->loopNum = oldLoopNum;
        a->bufferPos = oldBufferPos;
        a->bufferValidBytes = oldBufferValidBytes;
        a->bufferHeaderSize = oldBufferHeaderSize;
        a->streamWindow = oldStreamWindow;
        a->streamWindowSize = oldStreamWindowSize;
        a->streamWindowRead = oldStreamWindowRead;
        a->streamWindowWrite = oldStreamWindowWrite;
        a->streamWindowQueued = oldStreamWindowQueued;
        return 0;
    }
    if (oldStreamWindow) free(oldStreamWindow);
    a->streamWindow = NULL;
    a->streamWindowSize = 0;
    a->streamWindowRead = 0;
    a->streamWindowWrite = 0;
    a->streamWindowQueued = 0;
    a->bufferMaxSize = size;
    if (a->size > a->fileSize) a->size = a->fileSize;
    if (a->bytesPerFrame == 0u || a->dataByteOffset == 0u) {
        /* No frame-size/ring contract in the fed prefix (e.g. a fact-only
         * envelope or a first buffer that has not reached the fmt/data chunks
         * yet): the ring cannot be sized, so treat the track as linear
         * all-data and let a later SetData with more of the file re-enter the
         * streaming path. */
        a->bufferState = ATRAC_STATE_ALL_DATA_LOADED;
        a->bufferPos = 0;
        a->bufferValidBytes = 0;
        a->bufferHeaderSize = 0;
        return 1;
    }
    atrac_update_buffer_state(a);
    if (atrac_streamed_state(a)) {
        a->bufferHeaderSize = a->dataByteOffset;
        a->bufferPos = a->dataByteOffset + a->bytesPerFrame;
        a->bufferValidBytes =
            a->size > a->bufferPos ? a->size - a->bufferPos : 0;
        (void)atrac_stream_window_init(a);
    }
    return 1;
}
static int atrac_log_on(void);   /* defined below; used by decoder lifecycle */
static int audio_stat_on(void);  /* SR_AUDIOSTAT gate; defined with the SAS aggregates */
/* SR_AUDIOSTAT aggregate counters (investigation telemetry). Declared here because
 * the ATRAC decode path and the vblank exit dump both precede the SAS definitions. */
static unsigned long g_sas_calls, g_sas_calls_add, g_sas_pre_nonzero, g_sas_post_nonzero;
static unsigned long g_sas_erased;    /* nonzero on entry, silent on exit */
static int g_sas_pre_peak, g_sas_post_peak;
static unsigned long g_atrac_frames, g_atrac_frames_nonzero;
static int g_atrac_peak;
static unsigned long g_sas_no_voice, g_sas_no_voice_overwrite;
/* Buffer identities: [0]=sceAudioOutput2 submissions per channel, plus the SAS
 * output targets, so the two stages can be shown to be the same memory. */
static uint32_t g_sas_bufs[4];
static int      g_sas_nbufs;
static void audio_note_sas_buf(uint32_t buf) {
    if (!buf) return;
    for (int i = 0; i < g_sas_nbufs; i++) if (g_sas_bufs[i] == buf) return;
    if (g_sas_nbufs < 4) g_sas_bufs[g_sas_nbufs++] = buf;
}
static uint32_t g_atrac_out_bufs[4];
static int      g_atrac_out_nbufs;
static void audio_note_atrac_out(uint32_t buf) {
    if (!buf) return;
    for (int i = 0; i < g_atrac_out_nbufs; i++) if (g_atrac_out_bufs[i] == buf) return;
    if (g_atrac_out_nbufs < 4) g_atrac_out_bufs[g_atrac_out_nbufs++] = buf;
}

/* PR-B: decoder lifecycle. Only the imported ATRAC3+ codec (0x1000) has a
 * decoder in this tree; AT3 (0x1001) and unknown configs keep the gap visible
 * (DecodeData returns a codec error instead of fabricating PCM). */
static void atrac_release_decoder(Atrac *a) {
    atrac3p_bridge_destroy(a->dec);
    a->dec = NULL;
    free(a->host_frame);
    a->host_frame = NULL;
    a->host_frame_cap = 0;
    free(a->host_pcm);
    a->host_pcm = NULL;
}
static void atrac_ensure_decoder(Atrac *a) {
    if (a->dec) {
        /* A reused slot (h_AtracSetData on an existing id) may carry a decoder
         * created for a previous track config; recreate when the RIFF changed. */
        if (a->dec_channels == a->channels && a->dec_align == (int)a->bytesPerFrame)
            return;
        atrac_release_decoder(a);
        /* Release already reset these; keep them consistent with a->dec == NULL. */
        a->dec_channels = 0;
        a->dec_align = 0;
    }
    if (a->codecType != ATRAC_CODEC_AT3PLUS) return;
    if (a->channels < 1 || a->channels > ATRAC3P_MAX_CHANNELS) return;
    if (a->bytesPerFrame == 0u) return;
    int ret = atrac3p_bridge_create(a->channels, (int)a->bytesPerFrame, &a->dec);
    if (ret < 0) {
        a->dec = NULL;
        if (atrac_log_on())
            fprintf(stderr, "ATRAC_DECODER: create failed ch=%d align=%u ret=%d\n",
                    a->channels, a->bytesPerFrame, ret);
        return;
    }
    a->dec_channels = a->channels;
    a->dec_align = (int)a->bytesPerFrame;
    a->host_frame_cap = (int)a->bytesPerFrame;
    a->host_frame = malloc((size_t)a->bytesPerFrame + ATRAC3P_PADDING_SIZE);
    a->host_pcm = malloc((size_t)a->channels * ATRAC_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!a->host_frame || !a->host_pcm) {
        atrac_release_decoder(a);
        return;
    }
    if (atrac_log_on())
        fprintf(stderr, "ATRAC_DECODER: id=%d ch=%d align=%u ready\n",
                (int)(a - s_atrac), a->channels, a->bytesPerFrame);
}

static int atrac_log_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_ATRACLOG") != NULL;
    return on;
}
static uint32_t h_AtracGetAtracID(CpuState *s) {
    uint32_t codecType = A0;
    if (codecType != ATRAC_CODEC_AT3PLUS && codecType != ATRAC_CODEC_AT3)
        return ATRAC_ERROR_INVALID_CODECTYPE;
    for (int i = 0; i < 8; i++) {
        if (s_atrac[i].used) continue;
        memset(&s_atrac[i], 0, sizeof(s_atrac[i]));
        s_atrac[i].used = 1;
        s_atrac[i].codecType = codecType;
        if (atrac_log_on())
            fprintf(stderr, "ATRAC_GETID: id=%d codec=0x%08x\n", i, codecType);
        return (uint32_t)i;
    }
    return ATRAC_ERROR_NO_ATRACID;
}
static uint32_t h_AtracSetDataAndGetID(CpuState *s) {
    int id = -1; for (int i = 0; i < 8; i++) if (!s_atrac[i].used) { id = i; break; }
    if (id < 0) return ATRAC_ERROR_NO_ATRACID;
    Atrac *a = &s_atrac[id];
    memset(a, 0, sizeof(*a));
    a->used = 1; a->codecType = ATRAC_CODEC_AT3PLUS;
    if (!atrac_set_track(a, A0, A1)) {
        /* The ID was never handed out: do not leave a half-configured slot
         * behind that later calls could mistake for a valid context. */
        a->used = 0;
        return ATRAC_ERROR_UNKNOWN_FORMAT;
    }
    atrac_ensure_decoder(a);
    if (atrac_log_on()) {
        fprintf(stderr, "ATRAC_SETDATA: id=%d buf=0x%08x size=%u fileSize=%u frame=%u dataOff=%u endSample=%d state=%d\n",
                id, a->buf, a->size, a->fileSize, a->bytesPerFrame, a->dataByteOffset, a->endSample, a->bufferState);
        fprintf(stderr, "ATRAC_SETDATA_BYTES: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                MEM_R8(a->buf+0), MEM_R8(a->buf+1), MEM_R8(a->buf+2), MEM_R8(a->buf+3),
                MEM_R8(a->buf+4), MEM_R8(a->buf+5), MEM_R8(a->buf+6), MEM_R8(a->buf+7),
                MEM_R8(a->buf+8), MEM_R8(a->buf+9), MEM_R8(a->buf+10), MEM_R8(a->buf+11),
                MEM_R8(a->buf+12), MEM_R8(a->buf+13), MEM_R8(a->buf+14), MEM_R8(a->buf+15));
    }
    return (uint32_t)id;
}
static Atrac *atrac_of(uint32_t id) { return id < 8 && s_atrac[id].used ? &s_atrac[id] : 0; }
static uint32_t h_AtracSetData(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || A2 < 44u || !sr_guest_span_readable(A1, A2))
        return ATRAC_ERROR_SIZE_TOO_SMALL;
    if (!atrac_set_track(a, A1, A2))
        return ATRAC_ERROR_UNKNOWN_FORMAT;
    atrac_ensure_decoder(a);
    if (atrac_log_on())
        fprintf(stderr, "ATRAC_SETDATA_EXISTING: id=%u buf=0x%08x size=%u fileSize=%u frame=%u dataOff=%u endSample=%d state=%d\n",
                A0, a->buf, a->size, a->fileSize, a->bytesPerFrame, a->dataByteOffset, a->endSample, a->bufferState);
    return 0;
}
static uint32_t h_AtracReleaseAtracID(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return ATRAC_ERROR_BAD_ATRACID;
    atrac_release_decoder(a);
    atrac_stream_window_free(a);
    a->used = 0;
    return 0;
}
#ifdef SR_HLE_THREAD_SELFTEST
/* Streamed-ring geometry probe for the #32 wrap regression. No PSP API
 * exposes the ring's internal read cursor, and the defect is precisely that
 * the cursor leaves the frame grid after a wrap -- so the regression needs to
 * see the cursor to assert the invariant. Test builds only; reads state, never
 * changes it. */
int sr_hle_test_atrac_ring(uint32_t id, uint32_t *pos, uint32_t *base,
                           uint32_t *end, uint32_t *frame, uint32_t *valid) {
    Atrac *a = atrac_of(id);
    if (!a) return 0;
    if (pos) *pos = a->bufferPos;
    if (base) *base = atrac_stream_buffer_base(a);
    if (end) *end = atrac_stream_buffer_end(a);
    if (frame) *frame = a->bytesPerFrame;
    if (valid) *valid = a->bufferValidBytes;
    return 1;
}
#endif
/* PR-B: decode the next frame through the bridge into guest `out`. Returns
 *   1  frame decoded and written (channels*ATRAC_SAMPLES_PER_FRAME interleaved s16),
 *   0  no frame available right now (streamed ring empty or frame not yet fed),
 *  -1  permanent failure (no decoder, or the frame failed to decode).
 * A -1 is a visible gap: the caller returns an error code instead of
 * fabricating PCM (no fake audio). */
static int atrac_decode_frame(Atrac *a, uint32_t out) {
    uint32_t src;
    int streamed = atrac_streamed_state(a);
    if (a->bytesPerFrame == 0u) return 0;   /* ring not sized yet (streaming refill) */
    if (streamed) {
        if (a->bufferValidBytes < a->bytesPerFrame) return 0;
        if (!a->streamWindow || a->streamWindowQueued < a->bytesPerFrame)
            return -1;
        src = a->buf + a->bufferPos;
    } else {
        /* ALL_DATA_LOADED / HALFWAY_BUFFER: linear buffer, frame by posSample. */
        uint32_t off = atrac_file_offset_by_sample(a, a->posSample);
        if (a->size < a->bytesPerFrame || off > a->size - a->bytesPerFrame)
            return 0;   /* fed bytes do not reach this frame yet */
        src = a->buf + off;
    }
    if (!a->dec || !a->host_frame || !a->host_pcm) return -1;
    if (a->bytesPerFrame > (uint32_t)a->host_frame_cap) return -1;
    if (streamed) {
        if (!atrac_stream_window_peek(a, a->host_frame, a->bytesPerFrame))
            return -1;
    } else {
        if (!sr_guest_span_readable(src, a->bytesPerFrame)) return -1;
        for (uint32_t i = 0u; i < a->bytesPerFrame; i++)
            a->host_frame[i] = (uint8_t)MEM_R8(src + i);
    }
    int n = 0;
    int ret = atrac3p_bridge_decode(a->dec, a->host_frame, (int)a->bytesPerFrame,
                                    a->host_pcm, &n);
    if (ret < 0 || n <= 0) {
        /* Name the source offset, not just the codec's verdict: a frame the
         * ring pointed at incorrectly and a frame the decoder genuinely cannot
         * handle produce the same codec error, and only the offset separates
         * them (#32 -- the ring-wrap defect above presented as a codec
         * "missing feature"). */
        if (atrac_log_on())
            fprintf(stderr, "ATRAC_DECODE: frame decode failed ret=%d n=%d src=0x%08x pos=%u base=%u end=%u frame=%u\n",
                    ret, n, src, a->bufferPos, atrac_stream_buffer_base(a),
                    atrac_stream_buffer_end(a), a->bytesPerFrame);
        return -1;
    }
    /* Whole-span preflight before the bulk guest write (AGENTS.md guest-memory
     * hardening): the complete channels*n*2 writable span must be valid, and a
     * rejected output address must not leave partial PCM behind. */
    if (out && !sr_guest_span_writable(out, (uint32_t)n * (uint32_t)a->channels * 2u))
        return -1;
    /* host_pcm is already interleaved channels*n s16 (PR-A contract). */
    if (out)
        for (int i = 0; i < n * a->channels; i++)
            MEM_W16(out + (uint32_t)i * 2u, (uint16_t)a->host_pcm[i]);
    if (audio_stat_on()) {
        int pk = 0;
        for (int i = 0; i < n * a->channels; i++) {
            int v = a->host_pcm[i];
            if (v < 0) v = -v;
            if (v > pk) pk = v;
        }
        g_atrac_frames++;
        if (pk) g_atrac_frames_nonzero++;
        if (pk > g_atrac_peak) g_atrac_peak = pk;
        audio_note_atrac_out(out);
    }
    return 1;
}
static uint32_t h_AtracDecodeData(CpuState *s) {
    /* a0=id, a1=outSamples, a2=*decodedSamples, a3=*finishFlag, sp+16=*remainFrames. */
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    uint32_t out = A1, decAddr = A2, finAddr = A3, remAddr = stack_arg(s, 0);
    int n = 0, finished = 0;
    int st = atrac_decode_frame(a, out);
    if (st < 0) {
        /* Honest gap: this track cannot produce PCM in this build (unsupported
         * codec, decoder allocation failure, or a frame that failed to decode).
         * Report the failure rather than fabricate silence. */
        if (decAddr) MEM_W32(decAddr, 0u);
        if (finAddr) MEM_W32(finAddr, 1u);
        if (remAddr) MEM_W32(remAddr, 0u);
        if (atrac_log_on())
            fprintf(stderr, "ATRAC_DECODE: id=%u permanent decode failure state=%d\n",
                    A0, a->bufferState);
        return ATRAC_ERROR_UNKNOWN_FORMAT;
    }
    if (st > 0) {
        n = ATRAC_SAMPLES_PER_FRAME;
        if (atrac_streamed_state(a)) atrac_consume_frame(a);
        a->posSample += n;
        if (atrac_streamed_state(a) && a->loopEndSample > 0 && a->posSample > a->loopEndSample) {
            a->posSample = a->loopStartSample >= 0 ? a->loopStartSample : 0;
            if (a->loopNum > 0) a->loopNum--;
            if (a->dec) atrac3p_bridge_reset(a->dec);   /* loop rewind: no cross-frame leak */
        } else if (a->posSample >= a->endSample) {
            if (a->loopNum == 0) finished = 1;
            else {
                a->posSample = a->loopStartSample >= 0 ? a->loopStartSample : 0;
                if (a->loopNum > 0) a->loopNum--;
                if (a->dec) atrac3p_bridge_reset(a->dec);
            }
        }
    }
    if (decAddr) MEM_W32(decAddr, (uint32_t)n);
    if (finAddr) MEM_W32(finAddr, (uint32_t)finished);
    if (remAddr) MEM_W32(remAddr, atrac_remaining_frames(a));
    if (atrac_log_on()) {
        static uint32_t call;
        call++;
        if (call <= 16u || (call & (call - 1u)) == 0u)
            fprintf(stderr, "ATRAC_DECODE: id=%u call=%u posSample=%d endSample=%d decoded=%d finished=%d state=%d avail=%u out=0x%08x out0=%04x out1=%04x out2=%04x out3=%04x\n",
                    A0, call, a->posSample, a->endSample, n, finished, a->bufferState, a->bufferValidBytes, out,
                    out ? MEM_R16(out + 0u) : 0, out ? MEM_R16(out + 2u) : 0,
                    out ? MEM_R16(out + 4u) : 0, out ? MEM_R16(out + 6u) : 0);
    }
    return 0;
}
/* sceAtracGetNextSample(id, *outN): how many samples the NEXT DecodeData call
 * will produce. Previously registered to the generic success handler, which
 * leaves the caller's output variable untouched -- so the game read whatever
 * was already there and got a truthful-looking answer by accident or not at
 * all. This is the most frequently called stub on the boot route (4049 calls).
 *
 * No new state is needed: h_AtracDecodeData advances posSample by
 * ATRAC_SAMPLES_PER_FRAME only when a frame is actually consumed and stops at
 * endSample, so the honest answer is exactly what that path will do next.
 * Reporting anything else here would contradict the decoder's own bookkeeping. */
static uint32_t h_AtracGetNextSample(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return 0x80000103u;   /* ILLEGAL_ADDR */
    int remain = a->endSample - a->posSample;
    if (remain < 0) remain = 0;
    if (remain > ATRAC_SAMPLES_PER_FRAME) remain = ATRAC_SAMPLES_PER_FRAME;
    MEM_W32(A1, (uint32_t)remain);
    return 0;
}
static uint32_t h_AtracGetRemainFrame(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return 0x80000103u;
    MEM_W32(A1, atrac_remaining_frames(a));
    return 0;
}
static uint32_t h_AtracGetStreamDataInfo(CpuState *s) {
    /* a1=*writePointer (buf + ring offset), a2=*writableBytes, a3=*readOffset (file). */
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    uint32_t offset = 0, writable = 0, readOffset = 0;
    atrac_calculate_stream_info(a, &offset, &writable, &readOffset);
    if (A1 && sr_guest_span_writable(A1, 4u))
        MEM_W32(A1, a->buf + offset);
    if (A2 && sr_guest_span_writable(A2, 4u))
        MEM_W32(A2, writable);
    if (A3 && sr_guest_span_writable(A3, 4u))
        MEM_W32(A3, readOffset);
    return 0;
}
static uint32_t h_AtracAddStreamData(CpuState *s) {
    /* a1=size (new bytes available), a2=*writableBytes (updated by the call). */
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    uint32_t n = A1, writeOffset = 0, maxWritable = 0, readOffset = 0;
    atrac_calculate_stream_info(a, &writeOffset, &maxWritable, &readOffset);
    if (a->bufferState == ATRAC_STATE_ALL_DATA_LOADED)
        return ATRAC_ERROR_ALLDATA_LOADED;
    if (n > maxWritable) return ATRAC_ERROR_ADD_DATA_IS_TOO_BIG;
    uint32_t newSize;
    if (!sr_size_add_ok(readOffset, n, &newSize) || newSize > a->fileSize)
        return ATRAC_ERROR_ADD_DATA_IS_TOO_BIG;
    uint32_t newValid = a->bufferValidBytes;
    if (atrac_streamed_state(a) &&
        (!sr_size_add_ok(a->bufferValidBytes, n, &newValid) ||
         newValid > a->bufferMaxSize))
        return ATRAC_ERROR_ADD_DATA_IS_TOO_BIG;
    /* Keep a logical FIFO view of the guest ring.  The physical write span can
     * begin at the tail of the old header-prefixed lap and continue at offset
     * zero (or, after the first lap, at any wrapped offset).  The decoder must
     * see those bytes in file order, not as one host-contiguous guest span. */
    if (atrac_streamed_state(a) &&
        !atrac_stream_window_append(a, writeOffset, n))
        return ATRAC_ERROR_ADD_DATA_IS_TOO_BIG;
    a->size = newSize;
    if (atrac_streamed_state(a)) a->bufferValidBytes = newValid;
    atrac_update_buffer_state(a);
    if (atrac_log_on())
        fprintf(stderr, "ATRAC_STREAM_DATA: id=%u n=%u size=%u writable=%u state=%d avail=%u pos=%u\n",
                A0, n, a->size, maxWritable, a->bufferState, a->bufferValidBytes, a->bufferPos);
    if (A2 && sr_guest_span_writable(A2, 4u)) {
        atrac_calculate_stream_info(a, NULL, &maxWritable, NULL);
        MEM_W32(A2, maxWritable);
    }
    return 0;
}
static uint32_t h_AtracGetNextDecodePosition(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (a->posSample >= a->endSample) return ATRAC_ERROR_ALLDATA_DECODED;
    if (A1) MEM_W32(A1, (uint32_t)a->posSample);
    return 0;
}
static uint32_t h_AtracGetSoundSample(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (A1) MEM_W32(A1, (uint32_t)a->endSample);          /* end sample */
    if (A2) MEM_W32(A2, a->loopStartSample >= 0 ? (uint32_t)a->loopStartSample : 0xFFFFFFFFu);
    if (A3) MEM_W32(A3, a->loopEndSample >= 0 ? (uint32_t)a->loopEndSample : 0xFFFFFFFFu);
    return 0;
}
static uint32_t h_AtracGetLoopStatus(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (A1) MEM_W32(A1, (uint32_t)a->loopNum);
    if (A2) MEM_W32(A2, a->loopEndSample > 0 && a->bufferState != ATRAC_STATE_STREAMED_LOOP_TRAILER ? 1u : 0u);
    return 0;
}
static uint32_t h_AtracSetLoopNum(CpuState *s) { Atrac *a = atrac_of(A0); if (a) a->loopNum = (int)A1; return 0; }
static uint32_t h_AtracResetPlayPosition(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return 0;
    a->posSample = (int)A1;
    if (a->dec) atrac3p_bridge_reset(a->dec);  /* PR-B: decoder history must not cross a seek */
    return 0;
}

/* sceUtility dialogs (savedata/msg/osk). Faithful to PPSSPP's PSPDialog status machine
 * (Core/Dialog/PSPDialog.cpp): status enum NONE=0, INITIALIZE=1, RUNNING=2, FINISHED=3,
 * SHUTDOWN=4. InitStart -> INITIALIZE; GetStatus returns the current status and then auto-advances
 * INITIALIZE->RUNNING and SHUTDOWN->NONE; the (real-hardware) utility thread completes the
 * autoload, modelled here by RUNNING->FINISHED after a few polls; ShutdownStart -> SHUTDOWN. The
 * earlier guess jumped straight to RUNNING and to NONE, skipping INITIALIZE(1) and SHUTDOWN(4),
 * which a game that waits to observe those states would hang on. result is the common-header
 * field at param+0x1c. */
static void guest_cstr(uint32_t addr, char *out, int max);

/* sceUtilitySavedata: real persistence on a virtual memory stick (src/rt/savedata.c). */
uint32_t sr_savedata_execute(uint32_t param);
uint32_t sr_savedata_prepare_utility(unsigned kind);

enum { SR_UTILITY_STORAGE_GAMEDATA = 1, SR_UTILITY_STORAGE_GAMESHARING = 2 };
static atomic_flag s_dialog_lock = ATOMIC_FLAG_INIT;
static void dialog_lock(void) {
    while (atomic_flag_test_and_set_explicit(&s_dialog_lock, memory_order_acquire)) { }
}
static void dialog_unlock(void) {
    atomic_flag_clear_explicit(&s_dialog_lock, memory_order_release);
}

static int s_dlg_status = 0, s_dlg_tick = 0;
static uint32_t s_dlg_param = 0, s_dlg_result = 0, s_dlg_generation = 0;
static unsigned s_dlg_work_started, s_dlg_work_done;
static int s_osk_current_clear(void);   /* fwd: savedata/msg dialogs take the slot from the OSK */
static uint32_t h_SavedataInitStart(CpuState *s) {
    if (!A0) return 0x80110004u;
    dialog_lock();
    s_dlg_param = A0; s_dlg_status = 1; s_dlg_tick = 0;
    s_dlg_work_started = s_dlg_work_done = 0;
    s_dlg_generation++;
    s_osk_current_clear();
    s_dlg_result = 0;
    MEM_W32(A0 + 0x1c, 0);
    dialog_unlock();
    if (getenv("SR_DLGLOG")) { static int n=0; fprintf(stderr, "** SavedataInitStart #%d **\n", ++n);
        uint32_t p = A0;
        char gn[16], sn[24]; guest_cstr(p+0x3c, gn, sizeof(gn)); guest_cstr(p+0x4c, sn, sizeof(sn));
        fprintf(stderr, "SavedataInitStart param=0x%08x size=%u mode=%d gameName='%s' saveName='%s' result=0x%08x\n",
            p, MEM_R32(p+0), (int)MEM_R32(p+0x30), gn, sn, s_dlg_result);
    }
    return 0;
}
static uint32_t h_DlgGetStatus(CpuState *s) {
    (void)s;
    dialog_lock();
    int ret = s_dlg_status;
    if (s_dlg_status == 1) {                              /* INITIALIZE -> RUNNING */
        s_dlg_status = 2; s_dlg_tick = 0;
    } else if (s_dlg_status == 4) {                       /* SHUTDOWN -> NONE */
        s_dlg_status = 0;
    }
    if (getenv("SR_DLGLOG")) {                            /* log transitions only (unbounded) */
        static int last = -1;
        if (s_dlg_status != last) {
            fprintf(stderr, "DlgGetStatus: ret=%d -> status=%d (result=0x%08x)\n", ret, s_dlg_status, s_dlg_result);
            last = s_dlg_status;
        }
    }
    dialog_unlock();
    return (uint32_t)ret;
}
static uint32_t h_SavedataUpdate(CpuState *s) {
    (void)s;
    uint32_t param = 0, generation = 0;
    dialog_lock();
    if (s_dlg_status == 1) {
        s_dlg_status = 2;
        s_dlg_tick = 0;
    }
    if (s_dlg_status == 2 && !s_dlg_work_started) {
        s_dlg_work_started = 1;
        param = s_dlg_param;
        generation = s_dlg_generation;
    } else if (s_dlg_status == 2 && s_dlg_work_done && ++s_dlg_tick >= 2) {
        s_dlg_status = 3;
        if (s_dlg_param) MEM_W32(s_dlg_param + 0x1c, s_dlg_result);
        if (getenv("SR_DLGLOG")) fprintf(stderr, "SavedataUpdate: FINISHED result=0x%08x written to 0x%08x\n",
                                         s_dlg_result, s_dlg_param + 0x1c);
    }
    dialog_unlock();
    if (param) {
        uint32_t result = sr_savedata_execute(param);
        dialog_lock();
        if (generation == s_dlg_generation) {
            s_dlg_result = result;
            s_dlg_work_done = 1;
        }
        dialog_unlock();
    }
    return 0;
}
static uint32_t h_DlgShutdown(CpuState *s) {
    (void)s;
    dialog_lock();
    s_dlg_status = 4;
    dialog_unlock();
    if (getenv("SR_DLGLOG")) fprintf(stderr, "DlgShutdownStart\n");
    return 0;
}

/* Lightweight utility services used during setup. Each service owns an independent state
 * machine so polling netconf cannot consume a message/gamedata transition. Init captures the
 * common parameter block and completes it successfully; Update advances RUNNING->FINISHED. */
typedef struct UtilityDialog {
    uint32_t param;
    uint32_t result;
    uint32_t generation;
    int status;
    unsigned ticks;
    unsigned kind;
    unsigned work_started : 1;
    unsigned work_done : 1;
} UtilityDialog;
enum { UTILITY_MSG = 0, UTILITY_NETCONF, UTILITY_GAMEDATA, UTILITY_GAMESHARING };
static UtilityDialog s_msg_dialog = {.kind = UTILITY_MSG};
static UtilityDialog s_net_dialog = {.kind = UTILITY_NETCONF};
static UtilityDialog s_gamedata_dialog = {.kind = UTILITY_GAMEDATA};
static UtilityDialog s_sharing_dialog = {.kind = UTILITY_GAMESHARING};

static uint32_t utility_dialog_init(UtilityDialog *d, uint32_t param) {
    if (!param) return 0x80110004u; /* SCE_ERROR_UTILITY_INVALID_PARAM */
    dialog_lock();
    d->param = param;
    d->result = 0;
    d->generation++;
    d->status = 1;
    d->ticks = 0;
    d->work_started = d->work_done = 0;
    s_osk_current_clear();
    MEM_W32(param + 0x1c, 0);
    dialog_unlock();
    return 0;
}
static uint32_t utility_dialog_update(UtilityDialog *d) {
    uint32_t param = 0, generation = 0, result = 0;
    unsigned kind = 0;
    dialog_lock();
    if (d->status == 1) { d->status = 2; d->ticks = 0; }
    if (d->status == 2 && !d->work_started) {
        d->work_started = 1;
        param = d->param;
        generation = d->generation;
        kind = d->kind;
    } else if (d->status == 2 && d->work_done && ++d->ticks >= 2) {
        d->status = 3;
        if (d->param) MEM_W32(d->param + 0x1c, d->result);
    }
    dialog_unlock();

    if (param) {
        if (kind == UTILITY_GAMEDATA)
            result = sr_savedata_prepare_utility(SR_UTILITY_STORAGE_GAMEDATA);
        else if (kind == UTILITY_GAMESHARING)
            result = sr_savedata_prepare_utility(SR_UTILITY_STORAGE_GAMESHARING);
        else if (kind == UTILITY_MSG && MEM_R32(param) >= 0x240u)
            MEM_W32(param + 0x23c, 1u); /* default affirmative/OK button */
        dialog_lock();
        if (generation == d->generation) {
            d->result = result;
            d->work_done = 1;
        }
        dialog_unlock();
    }
    return 0;
}
static uint32_t utility_dialog_status(UtilityDialog *d) {
    dialog_lock();
    uint32_t ret = (uint32_t)d->status;
    if (d->status == 1) {
        d->status = 2;
        d->ticks = 0;
    } else if (d->status == 4) {
        d->status = 0;
        d->param = 0;
        d->ticks = 0;
        d->work_started = d->work_done = 0;
    }
    dialog_unlock();
    return ret;
}
static uint32_t utility_dialog_shutdown(UtilityDialog *d) {
    dialog_lock();
    if (d->status) d->status = 4;
    dialog_unlock();
    return 0;
}
#define UTILITY_DIALOG_HANDLERS(prefix, slot) \
    static uint32_t prefix##Init(CpuState *s) { return utility_dialog_init(&(slot), A0); } \
    static uint32_t prefix##Update(CpuState *s) { (void)s; return utility_dialog_update(&(slot)); } \
    static uint32_t prefix##Status(CpuState *s) { (void)s; return utility_dialog_status(&(slot)); } \
    static uint32_t prefix##Shutdown(CpuState *s) { (void)s; return utility_dialog_shutdown(&(slot)); }
UTILITY_DIALOG_HANDLERS(h_MsgDialog, s_msg_dialog)
UTILITY_DIALOG_HANDLERS(h_NetDialog, s_net_dialog)
UTILITY_DIALOG_HANDLERS(h_GamedataDialog, s_gamedata_dialog)
UTILITY_DIALOG_HANDLERS(h_SharingDialog, s_sharing_dialog)

/* ---- sceUtilityOsk: the on-screen keyboard, backed by a native input box (osk_win.c).
 * Same PSPDialog status machine as the other utilities. When the dialog reaches RUNNING the
 * native modal input box collects the text (the game is parked polling OskGetStatus, exactly
 * as it would be while the real OSK overlay is up), then the result is written back into each
 * SceUtilityOskData field as UTF-16 and the status advances to FINISHED. */
int sr_osk_input(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap);
static int s_osk_status = 0;
static uint32_t s_osk_param = 0;
/* PPSSPP keeps a "current dialog type": OskGetStatus is WRONG_TYPE only while a DIFFERENT
 * utility dialog owns the slot. After an OSK shuts down it stays the current dialog and
 * GetStatus returns NONE(0) â€” a game spinning "while (OskGetStatus() != 0)" after name entry
 * hangs forever if we keep returning WRONG_TYPE there. */
static int s_osk_current = 0;
static int s_osk_current_clear(void) { s_osk_current = 0; return 0; }

static void osk_read_utf16(uint32_t addr, wchar_t *out, int max) {
    int i = 0;
    if (addr) for (; i < max - 1; i++) {
        uint16_t c = MEM_R16(addr + (uint32_t)i * 2);
        if (!c) break;
        out[i] = (wchar_t)c;
    }
    out[i] = 0;
}

static void osk_run(void) {
    uint32_t p = s_osk_param;
    if (!p) return;
    int nf = (int)MEM_R32(p + 0x30);                       /* fieldCount */
    uint32_t fields = MEM_R32(p + 0x34);                   /* SceUtilityOskData[] */
    if (nf < 1 || nf > 8 || !fields) return;
    for (int i = 0; i < nf; i++) {
        uint32_t f = fields + (uint32_t)i * 0x34;
        uint32_t descA = MEM_R32(f + 0x1c), inA = MEM_R32(f + 0x20);
        uint32_t outLen = MEM_R32(f + 0x24), outA = MEM_R32(f + 0x28);
        uint32_t outLimit = MEM_R32(f + 0x30);
        wchar_t desc[128], intext[256], out[256];
        osk_read_utf16(descA, desc, 128);
        osk_read_utf16(inA, intext, 256);
        int cap = outLen ? (int)outLen : 256;              /* u16 units incl. terminator */
        if (outLimit && (int)outLimit + 1 < cap) cap = (int)outLimit + 1;
        if (cap > 256) cap = 256;
        wcscpy(out, intext);
        int ok = sr_osk_input(desc, intext, out, cap);
        if (outA) {
            const wchar_t *w = ok ? out : intext;
            int j = 0;
            for (; w[j] && j < cap - 1; j++) MEM_W16(outA + (uint32_t)j * 2, (uint16_t)w[j]);
            MEM_W16(outA + (uint32_t)j * 2, 0);
        }
        MEM_W32(f + 0x2c, ok ? 2u : 1u);                   /* result: CHANGED / CANCELLED */
        if (getenv("SR_DLGLOG"))
            fprintf(stderr, "osk: field %d desc='%ls' in='%ls' -> %s '%ls'\n",
                    i, desc, intext, ok ? "ok" : "cancel", ok ? out : intext);
    }
    MEM_W32(p + 0x1c, 0);                                  /* common result */
}

static uint32_t h_OskInitStart(CpuState *s) {
    s_osk_param = A0;
    s_osk_status = 1;                                      /* INITIALIZE */
    s_osk_current = 1;                                     /* OSK owns the dialog slot */
    return 0;
}
/* sceUtilityOskGetStatus: with no OSK ever started (e.g. polled every boot frame while a
 * savedata dialog is up) PPSSPP returns SCE_ERROR_UTILITY_WRONG_TYPE (0x80110005). Once an
 * OSK ran, NONE(0) is a real status games wait for after shutdown. */
static uint32_t h_OskGetStatus(CpuState *s) {
    (void)s;
    if (!s_osk_current) return 0x80110005u;
    int ret = s_osk_status;
    if (s_osk_status == 1) s_osk_status = 2;               /* INITIALIZE -> RUNNING */
    else if (s_osk_status == 2) { osk_run(); s_osk_status = 3; }   /* RUNNING -> FINISHED */
    else if (s_osk_status == 4) { s_osk_status = 0; s_osk_param = 0; }
    return (uint32_t)ret;
}
static uint32_t h_OskUpdate(CpuState *s) { (void)s; return 0; }
static uint32_t h_OskShutdown(CpuState *s) { (void)s; if (s_osk_status) s_osk_status = 4; return 0; }

/* sceWlanGetEtherAddr: 6-byte MAC out through a0. Fixed value so save/profile stamps stay
 * stable across runs. */
static uint32_t h_WlanGetEtherAddr(CpuState *s) {
    static const uint8_t mac[6] = { 0x00, 0x13, 0x37, 0xAC, 0xC5, 0x10 };
    if (!A0) return 0x80000103u;              /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
    for (int i = 0; i < 6; i++) MEM_W8(A0 + (uint32_t)i, mac[i]);
    return 0;
}
static uint32_t h_WlanOn(CpuState *s) { (void)s; return 1; }   /* powered on / switch up */

static uint32_t h_OpenPSIDGetOpenPSID(CpuState *s) {
    /* sceOpenPSIDGetOpenPSID(SceOpenPSID *psid): writes 16-byte console unique ID.
     * Zero-fill is fine for boot; the game only checks non-zero to confirm the call worked. */
    uint32_t ptr = s->r[4];
    if (ptr && ptr < 0x0c000000u) {
        MEM_W32(ptr, 0x00000000u);
        MEM_W32(ptr + 4, 0x00000000u);
        MEM_W32(ptr + 8, 0x00000000u);
        MEM_W32(ptr + 12, 0x00000000u);
    }
    return 0;
}

static uint32_t h_VolatileMemLock(CpuState *s) {
    /* sceKernelVolatileMemLock(type, void **paddr, int *psize): hand the app the 4MB volatile
     * partition. PPSSPP returns base 0x08400000, size 0x00400000; the game uses it as the
     * destination scratch buffer for decompressing/copying loaded assets, so the out-params
     * must be filled or the copy targets NULL. */
    if (A1) MEM_W32(A1, 0x08400000u);
    if (A2) MEM_W32(A2, 0x00400000u);
    return 0;
}
/* Registered entry for sceUmdRegisterUMDCallBack. When UMD becomes ready the kernel fires
 * this callback (a0=unknown, a1=drive_stat), which in this game calls sceKernelWakeupThread
 * on the launcher (0x111) to unstick it from sceKernelSleepThread. */
static uint32_t s_umd_cb_uid = 0;

static uint32_t h_UmdDriveStat(CpuState *s) { (void)s; return 0x32; }     /* PRESENT|READY|READABLE (matches PPSSPP reference) */

/* Signal that the UMD drive is ready. Wake only threads waiting on the UMD object and
 * notify the registered callback if one exists. */
static void sr_umd_signal_ready(void) {
    extern void sched_wake(uint32_t);
    sched_wake(0x554D44u);
    if (s_umd_cb_uid)
        (void)sr_callback_notify(s_umd_cb_uid, 0x32u);
}

/* Shared body for sceUmdWaitDriveStat / ...WithTimer. The emulated drive is a
 * constant present|ready|readable (0x32; see h_UmdDriveStat), so a request whose
 * bits all fall inside that mask is satisfied immediately; anything else can never
 * be satisfied and waits out the timeout. The post-wait re-read is against the same
 * drive model, so it stays unsatisfiable and reports SCE_KERNEL_ERROR_WAIT_TIMEOUT
 * -- kept (rather than returning the error unconditionally) so the shape still
 * matches a real drive whose state could change across the wait. */
static uint32_t umd_wait_drive_stat(uint32_t want, uint32_t timeout_us) {
    const uint32_t drive = 0x32u;
    if ((drive & want) == want) return 0;
    (void)sched_block_on_timeout(0x554D44u, timeout_us);
    return (drive & want) == want ? 0u : 0x800201A8u;
}

static uint32_t h_UmdWaitDriveStat(CpuState *s) {
    (void)s;
    return umd_wait_drive_stat(A0, 50000u);
}

/* sceUmdWaitDriveStatWithTimer(stat, timeout_us): same as the untimed wait but the
 * caller supplies the timeout in $a1. Previously this shared h_UmdWaitDriveStat and
 * silently ignored that argument. */
static uint32_t h_UmdWaitDriveStatWithTimer(CpuState *s) {
    (void)s;
    return umd_wait_drive_stat(A0, A1 ? A1 : 50000u);
}

static uint32_t h_UmdWaitDriveStatCB(CpuState *s) {
    uint32_t self = sched_current_uid();
    if (sr_thread_has_pending_callbacks(self))
        sr_thread_dispatch_callbacks();

    if ((0x32u & A0) == A0) return 0;

    uint32_t timeout = A1 ? A1 : 8000u;
    /* PSP hardware rounds very small UMD wait timeouts upward. */
    if (timeout <= 4u) timeout = 15u;
    else if (timeout <= 215u) timeout = 250u;

    sched_set_current_cb_wait(1);
    int timed_out = sched_block_on_timeout(0x554D44u, timeout);
    sched_set_current_cb_wait(0);

    if (sr_thread_has_pending_callbacks(self))
        sr_thread_dispatch_callbacks();

    return timed_out ? 0x800201A8u : 0u;
}

/* sceUmdActivate: real PSP spins up the laser motor; do a small blocking delay before reporting
 * success so the caller sees a settle (matches "Active" transition in the real driver). After
 * the settle, fire the UMD-ready signal so any waiting thread and the registered callback fire. */
static uint32_t h_UmdActivate(CpuState *s) {
    (void)s;
    extern void sched_delay_current(uint32_t);
    /* 30 ms virtual delay keeps the loader's stat-read interleaving realistic. */
    sched_delay_current(30000);
    sr_umd_signal_ready();
    return 0;
}

/* sceUmdRegisterUMDCallBack: register a callback that fires when UMD drive state changes.
 * Registration only records the callback; it does not signal an event or alter any
 * thread's pending wakeup count. */
static uint32_t h_UmdRegisterUMDCallBack(CpuState *s) {
    if (!sr_callback_is_valid(A0))
        return 0x80010016u;
    s_umd_cb_uid = A0;
    if (getenv("SR_CBLOG"))
        fprintf(stderr, "CBLOG: UMD callback registered uid=0x%x\n", s_umd_cb_uid);
    return 0;
}

/* VBLANK sub-interrupt handler the game registers; delivered once per frame by the scheduler
 * (it typically wakes the sleeping game thread). PSP_VBLANK_INT = 30. */
static uint32_t g_vbl_handler = 0, g_vbl_arg = 0; static int g_vbl_on = 0;

/* Generic PSP callback objects are kernel objects, not a 16-entry subsystem
 * table. Hardware creates at least 1024 of them. Keep an append-only host array:
 * deleted holes are never reused, so dispatch order remains callback creation order. */
CallbackEntry *s_callbacks;
size_t s_callbacks_len;
static size_t s_callbacks_cap;

static int sr_callback_reserve_one(void) {
    if (s_callbacks_len < s_callbacks_cap) return 1;
    size_t next = s_callbacks_cap ? s_callbacks_cap * 2u : 64u;
    if (next < s_callbacks_cap || next > ((size_t)-1) / sizeof(*s_callbacks)) return 0;
    CallbackEntry *grown = (CallbackEntry *)realloc(s_callbacks, next * sizeof(*grown));
    if (!grown) return 0;
    memset(grown + s_callbacks_cap, 0, (next - s_callbacks_cap) * sizeof(*grown));
    s_callbacks = grown;
    s_callbacks_cap = next;
    return 1;
}

uint32_t sr_callback_table_register(uint32_t name_addr, uint32_t entry,
                                    uint32_t arg, uint32_t *error_out) {
    if (error_out) *error_out = 0x80020190u;
    if (!name_addr || !sr_inrange(name_addr)) {
        if (error_out) *error_out = 0x80020001u;
        return 0;
    }
    if (entry && !sr_inrange(entry)) {
        if (error_out) *error_out = 0x800200D3u;
        return 0;
    }
    if (!sr_callback_reserve_one()) return 0;

    CallbackEntry *cb = &s_callbacks[s_callbacks_len++];
    memset(cb, 0, sizeof(*cb));
    cb->uid = sr_alloc_uid();
    cb->entry = entry;
    cb->arg = arg;
    cb->owner_thread_uid = sched_current_uid();
    cb->used = 1;
    for (size_t i = 0; i < sizeof(cb->name) - 1u; i++) {
        uint32_t addr = name_addr + (uint32_t)i;
        if (!sr_inrange(addr)) break;
        cb->name[i] = (char)MEM_R8(addr);
        if (cb->name[i] == '\0') break;
    }
    cb->name[sizeof(cb->name) - 1u] = '\0';
    if (error_out) *error_out = 0;
    return cb->uid;
}

int sr_callback_find_in_table(uint32_t uid) {
    for (size_t i = 0; i < s_callbacks_len; i++)
        if (s_callbacks[i].used && s_callbacks[i].uid == uid) return (int)i;
    return -1;
}

int sr_callback_is_valid(uint32_t uid) {
    return sr_callback_find_in_table(uid) >= 0;
}

int sr_callback_table_unregister(uint32_t uid) {
    int idx = sr_callback_find_in_table(uid);
    if (idx < 0) return 0;
    memset(&s_callbacks[idx], 0, sizeof(s_callbacks[idx]));
    return 1;
}

void sr_callback_unregister_owner(uint32_t thread_uid) {
    for (size_t i = 0; i < s_callbacks_len; i++) {
        if (s_callbacks[i].used && s_callbacks[i].owner_thread_uid == thread_uid)
            memset(&s_callbacks[i], 0, sizeof(s_callbacks[i]));
    }
    if (s_exit_cb_uid && !sr_callback_is_valid(s_exit_cb_uid))
        s_exit_cb_uid = 0;
}

uint32_t sr_callback_notify(uint32_t uid, uint32_t notify_arg) {
    int idx = sr_callback_find_in_table(uid);
    if (idx < 0) return 0x800201A1u;
    CallbackEntry *cb = &s_callbacks[idx];
    cb->pending = 1;
    cb->notify_arg = notify_arg;
    cb->notify_count++;
    if (getenv("SR_CBLOG")) {
        fprintf(stderr,
                "CBLOG: NotifyCallback uid=0x%x count=%u arg=0x%08x owner=0x%x\n",
                uid, cb->notify_count, notify_arg, cb->owner_thread_uid);
    }
    sched_wake_callbacks(cb->owner_thread_uid);
    return 0;
}

int sr_thread_has_pending_callbacks(uint32_t thread_uid) {
    for (size_t i = 0; i < s_callbacks_len; i++) {
        if (s_callbacks[i].used &&
            s_callbacks[i].owner_thread_uid == thread_uid &&
            s_callbacks[i].pending)
            return 1;
    }
    return 0;
}

int sr_thread_dispatch_callbacks(void) {
    uint32_t thread_uid = sched_current_uid();
    extern CpuState *sr_cpu_for_callbacks(void);
    CpuState *cpu = sr_cpu_for_callbacks();
    if (!cpu) return 0;

    int total_dispatched = 0;
    /* Re-scan the owning thread from the beginning after every callback return and dispatch
     * exactly one pending callback per iteration, selected by UID. A callback body runs guest
     * code that may register new callbacks (reallocating s_callbacks), unregister, re-notify,
     * or delete itself and reuse its slot -- so no slot cursor can be carried across a
     * dispatch. Selecting and re-resolving by UID makes self-deletion-with-replacement and
     * mid-dispatch re-notification of an earlier callback correct, and matches PPSSPP's
     * ActionAfterCallback re-check. Termination is a full scan finding nothing pending (no
     * arbitrary host pass cap): a callback that endlessly re-notifies itself would also
     * occupy sceKernelCheckCallback on real hardware. */
    for (;;) {
        uint32_t selected_uid = 0;
        for (size_t i = 0; i < s_callbacks_len; i++) {
            if (s_callbacks[i].used &&
                s_callbacks[i].owner_thread_uid == thread_uid &&
                s_callbacks[i].pending) {
                selected_uid = s_callbacks[i].uid;
                break;
            }
        }
        if (selected_uid == 0) break;

        int idx = sr_callback_find_in_table(selected_uid);
        if (idx < 0) continue;
        CallbackEntry *cb = &s_callbacks[idx];

        total_dispatched++;
        uint32_t uid = cb->uid;
        uint32_t entry = cb->entry;
        uint32_t common_arg = cb->arg;
        uint32_t notify_arg = cb->notify_arg;
        uint32_t notify_count = cb->notify_count;

        /* The notification is consumed as the body starts, so a re-notification made by the
         * body forms a new pending event picked up on the next scan. */
        cb->pending = 0;
        cb->notify_count = 0;
        cb->notify_arg = 0;

        uint32_t ret = sr_callback_dispatch_one(
            cpu, entry, (int)notify_count, notify_arg, common_arg, dispatch);
        ge_enqueue_trace_note_callback(cpu, uid, entry);
        /* Apply the auto-delete rule to the dispatched UID, not a slot: the body may have
         * deleted this callback and registered a replacement into the same slot. */
        if (ret != 0)
            sr_callback_table_unregister(uid);
    }
    return total_dispatched;
}

/* Returns 1 if any registered entry was dispatched (used as a one-shot for engine latches).
 *
 * The scheduler calls this once per delivered vblank after the sub-interrupt handler.
 * Slots run in stable registration/slot order.  Each slot is re-read immediately before
 * dispatch so callbacks may safely unregister themselves or a later callback.
 * The SR_LATCH_BYPASS force-write of MEM[0x30ab8c]=1 was removed: 0x30ab8c is the engine's
 * "streaming I/O busy" flag (decomp 54073/26731), and pinning it =1 made the poweroff
 * watchdog never drain. */
int sr_vblank_dispatch_registered(void) {
    return 0;
}
/* h_NotifyCallback: 0xc11ba8c4 â€” synchronously dispatch a registered callback entry from
 * the calling thread context. The PSP firmware exposes this so the game can manually pump
 * a callback instead of waiting for the vblank IRQ. We honour the uid; the dispatched entry
 * receives the ABI packed by sr_callback_pack_args ($a0 = count, $a1 = notify arg,
 * $a2 = registered common arg). */
static uint32_t h_NotifyCallback(CpuState *s) {
    return sr_callback_notify(A0, A1);
}

/* sceKernelCheckCallback: run any callbacks pending on the calling thread and
 * report whether at least one ran. The kernel's public return is Boolean (1 if a
 * callback was serviced, 0 if none were pending) -- guest code polls it in a loop
 * (e.g. waiting for the HOME-button exit callback) and only branches on
 * zero/nonzero. The internal pump (sr_thread_dispatch_callbacks) still returns a
 * count, which is collapsed to 0/1 here. */
static uint32_t h_CheckCallback(CpuState *s) {
    (void)s;
    if (!sr_thread_has_pending_callbacks(sched_current_uid())) return 0;
    return sr_thread_dispatch_callbacks() > 0 ? 1u : 0u;
}

static uint32_t h_CancelCallback(CpuState *s) {
    int idx = sr_callback_find_in_table(A0);
    if (idx < 0) return 0x800201A1u;
    s_callbacks[idx].pending = 0;
    s_callbacks[idx].notify_count = 0;
    s_callbacks[idx].notify_arg = 0;
    return 0;
}

static uint32_t h_GetCallbackCount(CpuState *s) {
    int idx = sr_callback_find_in_table(A0);
    if (idx < 0) return 0x800201A1u;
    return s_callbacks[idx].notify_count;
}

static uint32_t h_ReferCallbackStatus(CpuState *s) {
    uint32_t uid = A0;
    uint32_t infop = A1;
    int idx = sr_callback_find_in_table(uid);
    if (idx < 0) return 0x800201A1u;
    if (infop && MEM_R32(infop) != 0) {
        CallbackEntry *cb = &s_callbacks[idx];
        for (uint32_t i = 0; i < 56u; i++) MEM_W8(infop + i, 0);
        MEM_W32(infop + 0, 56u);
        for (uint32_t i = 0; i < 32u; i++)
            MEM_W8(infop + 4u + i, (uint8_t)cb->name[i]);
        MEM_W32(infop + 36, cb->owner_thread_uid);
        MEM_W32(infop + 40, cb->entry);
        MEM_W32(infop + 44, cb->arg);
        MEM_W32(infop + 48, cb->notify_count);
        MEM_W32(infop + 52, cb->notify_arg);
    }
    return 0;
}
/* h_CreateNotifyCallback: 0x9f9b46b9 â€” same as sceKernelCreateCallback but the callback uid
 * is registered for NOTIFY semantics: the game calls sceKernelNotifyCallback (0xc11ba8c4) on
 * it, OR the kernel auto-fires it when triggered programmatically. The PSMF modules and the
 * PSP-LDD-aware HL code paths use this variant to install their ring-fill and stream-event
 * callbacks (sceMpeg uses the "Notify" path for its NEAR/full callbacks; the IdStorage +
 * audio thread models assume one uid per NamedCallback binding). We register against the
 * same s_callbacks[] table as h_CreateCallback, so h_NotifyCallback and the callback-aware
 * wait paths (sr_thread_dispatch_callbacks) surface it the same way; the raw vblank IRQ does
 * NOT dispatch callbacks (sr_vblank_dispatch_registered() is a deliberate no-op â€” see its
 * comment above). */
static uint32_t h_CreateNotifyCallback(CpuState *s) {
    uint32_t error = 0;
    uint32_t uid = sr_callback_table_register(A0, A1, A2, &error);
    if (getenv("SR_CBLOG")) {
        fprintf(stderr,
                "CBLOG: CreateNotifyCallback name=0x%08x entry=0x%08x common=0x%08x -> 0x%08x\n",
                A0, A1, A2, uid ? uid : error);
    }
    return uid ? uid : error;
}
/* h_DeleteCallback: 0xedba5844 â€” release a callback uid from the s_callbacks[] table.
 * h_CreateCallback returns these uids (they're real s_callbacks[] slots, not module uid
 * pool). Returning success without freeing would leave a stale entry that fires every
 * vblank against deleted code. */
static uint32_t h_DeleteCallback(CpuState *s) {
    return sr_callback_table_unregister(A0) ? 0u : 0x800201A1u;
}
/* h_DeleteNotifyCallback: 0x0ed48fe2 â€” release a Notify-flavored callback uid
 * (allocated by h_CreateNotifyCallback). Same backing table; identical semantics. */
static uint32_t h_DeleteNotifyCallback(CpuState *s) {
    return sr_callback_table_unregister(A0) ? 0u : 0x800201A1u;
}
static uint32_t h_RegisterSubIntr(CpuState *s) {
    /* a0=intno, a1=no, a2=handler, a3=arg. */
    if (A0 == 30) {
        fprintf(stderr, "HLE: registering VBLANK handler 0x%08x arg 0x%08x\n", A2, A3);
        g_vbl_handler = A2; g_vbl_arg = A3;
    }
    return 0;
}
static uint32_t h_EnableSubIntr(CpuState *s) { if (A0 == 30) g_vbl_on = 1; return 0; }
uint32_t sr_vblank_handler(void) { return g_vbl_on ? g_vbl_handler : 0; }
uint32_t sr_vblank_arg(void) { return g_vbl_arg; }

static uint32_t s_vcount_fwd;  /* mirror of s_vcount for clock/input timing (set in sr_display_advance_vcount) */
uint32_t sr_audio_vbl(void) { return s_vcount_fwd; }

/* All guest clock APIs observe the scheduler's single monotonic microsecond
 * timeline.  They never advance it merely by being queried. */
static uint64_t now_usec(void) { return sched_vtime_us(); }

static uint32_t h_GetSystemTimeLow(CpuState *s) {
    (void)s;
    static uint32_t last_log = 0;
    uint32_t t = (uint32_t)now_usec();
    if (hle_log_on() && t - last_log > 1000000) {
        fprintf(stderr, "HLE: GetSystemTimeLow %u\n", t);
        last_log = t;
    }
    return t;
}
static uint32_t h_GetSystemTimeWide(CpuState *s) {
    uint64_t t = now_usec();
    s->r[3] = (uint32_t)(t >> 32);
    return (uint32_t)t;
}

enum {
    RTC_ILLEGAL_ADDR = 0x80000103u,
    /* PSPAutotests tests/rtc/convert.expected: the RTC conversion family reports
     * invalid dates AND invalid output pointers as 0x800001fe
     * (SCE_KERNEL_ERROR_INVALID_VALUE): "Min year: 800001fe", "Year overflow:
     * 800001fe", "Zeroed time: 0 (800001fe)", "NULL filetime: -1337 (800001fe)". */
    RTC_INVALID_VALUE = 0x800001feu,
    RTC_UNIX_EPOCH_TICK = 62135596800000000ull,
    RTC_FILETIME_EPOCH_TICK = 50491123200000000ull,
};

/* The PSP timezone is a console setting, not the host process timezone.  The
 * current public configuration is UTC/standard time; keeping it explicit makes
 * every RTC/local and gettimeofday path deterministic and leaves the setting in
 * one place.  The retained, settable system-profile owner for timezone/daylight
 * is issue #77 and does not exist yet, so sceRtcGetCurrentClockLocalTime and
 * the UTC/local conversions run on this fixed UTC constant until #77 lands;
 * the explicit-offset sceRtcGetCurrentClock path is complete and independent.
 * #80's LocalTime criterion is therefore BLOCKED BY #77, not complete. */
static const int32_t s_psp_timezone_minutes = 0;
static const int32_t s_psp_daylight = 0;

static uint64_t s_rtc_epoch_tick;
static uint64_t s_rtc_last_tick;
static int s_rtc_epoch_initialized;

#ifdef SR_HLE_THREAD_SELFTEST
/* White-box fixture hook: forget the host-sampled RTC epoch so a test can
 * re-anchor it at its own deterministic scheduler time. The monotonic clamp on
 * rtc_now_tick() is correct for the production monotonic timeline; a fixture
 * that rewinds s_vtime_us between tests needs the anchor refreshed with it. */
void sr_hle_test_reset_rtc_epoch(void) {
    s_rtc_epoch_tick = 0;
    s_rtc_last_tick = 0;
    s_rtc_epoch_initialized = 0;
}
#endif

static uint64_t rtc_now_tick(void) {
    if (!s_rtc_epoch_initialized) {
        /* FILETIME is 100 ns since 1601-01-01; sample the host only once to
         * establish the calendar offset.  Anchor it at guest time zero so a
         * first RTC query made after boot-time work does not double-count the
         * elapsed scheduler time.  Subsequent reads are guest-time only. */
        FILETIME ft;
        ULARGE_INTEGER value;
        GetSystemTimeAsFileTime(&ft);
        value.LowPart = ft.dwLowDateTime;
        value.HighPart = ft.dwHighDateTime;
        uint64_t host_tick = RTC_FILETIME_EPOCH_TICK + value.QuadPart / 10u;
        uint64_t elapsed = now_usec();
        s_rtc_epoch_tick = host_tick >= elapsed ? host_tick - elapsed : 0u;
        s_rtc_last_tick = host_tick;
        s_rtc_epoch_initialized = 1;
    }
    uint64_t elapsed = now_usec();
    uint64_t tick = s_rtc_epoch_tick <= UINT64_MAX - elapsed
        ? s_rtc_epoch_tick + elapsed : UINT64_MAX;
    if (tick < s_rtc_last_tick) tick = s_rtc_last_tick;
    s_rtc_last_tick = tick;
    return tick;
}

static int rtc_write_u64(uint32_t addr, uint64_t value) {
    if (!addr || !sr_guest_span_writable(addr, 8u)) return 0;
    MEM_W32(addr, (uint32_t)value);
    MEM_W32(addr + 4u, (uint32_t)(value >> 32));
    return 1;
}
static int rtc_read_u64(uint32_t addr, uint64_t *out) {
    if (!addr || !out || !sr_guest_span_readable(addr, 8u)) return 0;
    *out = (uint64_t)MEM_R32(addr) | ((uint64_t)MEM_R32(addr + 4u) << 32);
    return 1;
}

typedef struct {
    uint32_t year, month, day, hour, minute, second, microsecond;
} RtcDateTime;

static int rtc_is_leap(uint32_t year) {
    return (year % 4u == 0u) && (year % 100u != 0u || year % 400u == 0u);
}
static uint32_t rtc_days_in_month(uint32_t year, uint32_t month) {
    static const uint8_t days[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < 1u || month > 12u) return 0;
    return days[month - 1u] + (month == 2u && rtc_is_leap(year));
}
static int rtc_read_datetime(uint32_t addr, RtcDateTime *out) {
    if (!addr || !out || !sr_guest_span_readable(addr, 16u)) return -1;
    out->year = MEM_R16(addr + 0u); out->month = MEM_R16(addr + 2u);
    out->day = MEM_R16(addr + 4u); out->hour = MEM_R16(addr + 6u);
    out->minute = MEM_R16(addr + 8u); out->second = MEM_R16(addr + 10u);
    out->microsecond = MEM_R32(addr + 12u);
    if (out->year < 1u || out->year > 9999u) return -1; /* PSP_TIME_INVALID_YEAR */
    if (out->month < 1u || out->month > 12u) return -2;
    if (out->day < 1u || out->day > rtc_days_in_month(out->year, out->month)) return -3;
    if (out->hour > 23u) return -4;
    if (out->minute > 59u) return -5;
    if (out->second > 59u) return -6;
    if (out->microsecond > 999999u) return -7;
    return 0;
}
static int rtc_write_datetime(uint32_t addr, const RtcDateTime *value) {
    if (!addr || !value || !sr_guest_span_writable(addr, 16u)) return 0;
    MEM_W16(addr + 0u, (uint16_t)value->year); MEM_W16(addr + 2u, (uint16_t)value->month);
    MEM_W16(addr + 4u, (uint16_t)value->day); MEM_W16(addr + 6u, (uint16_t)value->hour);
    MEM_W16(addr + 8u, (uint16_t)value->minute); MEM_W16(addr + 10u, (uint16_t)value->second);
    MEM_W32(addr + 12u, value->microsecond);
    return 1;
}
/* PSP RTC ticks are microseconds since 0001-01-01 in the proleptic Gregorian calendar. */
static uint64_t rtc_datetime_to_tick(const RtcDateTime *value) {
    uint64_t y = value->year - 1u;
    uint64_t days = y * 365u + y / 4u - y / 100u + y / 400u;
    for (uint32_t month = 1; month < value->month; month++)
        days += rtc_days_in_month(value->year, month);
    days += value->day - 1u;
    uint64_t seconds = ((days * 24u + value->hour) * 60u + value->minute) * 60u + value->second;
    return seconds * 1000000u + value->microsecond;
}
/* PSP tick->date conversion covers the FULL u64 tick range: PSPAutotests
 * tests/rtc/arithmetic.expected prints years 60267, 38202, 10000 and 26003
 * written through sceRtcSetTick for wrapped/extreme ticks, so there is no
 * year>9999 rejection here -- the fields are written with natural truncation
 * into the pspTime u16/u32 widths, exactly like the firmware. */
static int rtc_tick_to_datetime(uint64_t tick, RtcDateTime *out) {
    if (!out) return -1;
    uint64_t total_seconds = tick / 1000000u;
    uint64_t days = total_seconds / 86400u;
    uint64_t day_seconds = total_seconds % 86400u;
    uint64_t n400 = days / 146097u; days %= 146097u;
    uint64_t n100 = days / 36524u; if (n100 > 3u) n100 = 3u; days -= n100 * 36524u;
    uint64_t n4 = days / 1461u; days %= 1461u;
    uint64_t n1 = days / 365u; if (n1 > 3u) n1 = 3u; days -= n1 * 365u;
    uint64_t year = 1u + n400 * 400u + n100 * 100u + n4 * 4u + n1;
    uint32_t month = 1u;
    while (month <= 12u) {
        uint32_t dim = rtc_days_in_month((uint32_t)year, month);
        if (days < dim) break;
        days -= dim; month++;
    }
    out->year = (uint32_t)year; out->month = month; out->day = (uint32_t)days + 1u;
    out->hour = (uint32_t)(day_seconds / 3600u);
    out->minute = (uint32_t)((day_seconds / 60u) % 60u);
    out->second = (uint32_t)(day_seconds % 60u);
    out->microsecond = (uint32_t)(tick % 1000000u);
    return 0;
}

static int rtc_add_offset(uint64_t tick, int32_t minutes, uint64_t *out) {
    if (!out) return 0;
    int64_t delta = (int64_t)minutes * 60ll * 1000000ll;
    if (delta >= 0) {
        if (tick > UINT64_MAX - (uint64_t)delta) return 0;
        *out = tick + (uint64_t)delta;
    } else {
        uint64_t magnitude = (uint64_t)(-delta);
        if (tick < magnitude) return 0;
        *out = tick - magnitude;
    }
    return 1;
}
/* sceRtcGetCurrentClock[LocalTime] accepts every int32 timezone offset and
 * returns success (PSPAutotests tests/rtc/rtc.expected: 0, +13, +60, -60,
 * -600000, INT_MAX and -INT_MAX all print 00000000).  The offset is applied in
 * well-defined modulo-2^64 arithmetic -- the firmware's own u64 tick shape
 * (arithmetic.expected wraps to years 60267/38202 on negative overrun) -- so
 * an offset that crosses year 1 wraps instead of failing.  ConvertUtcToLocal
 * keeps the checked rtc_add_offset above because its overflow contract is not
 * autotest-verified; only the current-clock path uses the wrap form. */
static void rtc_add_offset_wrap(uint64_t tick, int32_t minutes, uint64_t *out) {
    int64_t delta = (int64_t)minutes * 60ll * 1000000ll;
    *out = tick + (uint64_t)delta;   /* defined modulo 2^64 for both signs */
}
static uint32_t h_GetSystemTime(CpuState *s) {
    (void)s;
    return rtc_write_u64(A0, now_usec()) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcGetCurrentTick(CpuState *s) {
    (void)s;
    return rtc_write_u64(A0, rtc_now_tick()) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcGetTick(CpuState *s) {
    (void)s;
    if (!A0 || !A1) return RTC_ILLEGAL_ADDR;
    if (!sr_guest_span_readable(A0, 16u)) return RTC_ILLEGAL_ADDR;
    RtcDateTime value;
    int valid = rtc_read_datetime(A0, &value);
    /* PSPAutotests tests/rtc/convert.expected: "Min year: 800001fe" and
     * "Year overflow: 800001fe" -- an invalid date reports
     * SCE_KERNEL_ERROR_INVALID_VALUE and leaves the output tick untouched
     * (the tick printed after the failure is the previous success). */
    if (valid != 0) return RTC_INVALID_VALUE;
    if (!sr_guest_span_writable(A1, 8u)) return RTC_ILLEGAL_ADDR;
    return rtc_write_u64(A1, rtc_datetime_to_tick(&value)) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcSetTick(CpuState *s) {
    (void)s;
    if (!A0 || !A1) return RTC_ILLEGAL_ADDR;
    uint64_t tick;
    if (!rtc_read_u64(A1, &tick)) return RTC_ILLEGAL_ADDR;
    RtcDateTime value;
    /* Any u64 tick converts; extreme/wrapped values truncate into the pspTime
     * field widths (PSPAutotests arithmetic.expected, checkSetTick). */
    rtc_tick_to_datetime(tick, &value);
    return rtc_write_datetime(A0, &value) ? 0u : RTC_ILLEGAL_ADDR;
}
/* sceRtcGetWin32FileTime(pspTime *in, u64 *out): convert from the PSP Gregorian
 * epoch to Windows' 1601 epoch and 100 ns units.
 *
 * PSPAutotests tests/rtc/convert.expected defines the failure contract:
 *   - "NULL filetime: -1337 (800001fe)"    invalid out pointer -> 0x800001fe, no write
 *   - "Zeroed time: 0 (800001fe)"          year 0             -> 0x800001fe, out := 0
 *   - "1600 January 01: 0 (800001fe)"      pre-1601 tick      -> 0x800001fe, out := 0
 *   - "Arbitrary date/time: 127779156600000010 (00000000)" -- 2005-11-31 is
 *     ACCEPTED (day 31 in a 30-day month) and converts with carry arithmetic,
 *     so this path does NOT run the strict days-in-month validator.  Only the
 *     year bound (1..9999) is checked here; the epoch bound below rejects
 *     everything before 1601-01-01.  On failure the output is written as 0. */
static uint32_t h_RtcGetWin32FileTime(CpuState *s) {
    (void)s;
    if (!A0 || !A1) return RTC_INVALID_VALUE;
    if (!sr_guest_span_readable(A0, 16u)) return RTC_INVALID_VALUE;
    if (!sr_guest_span_writable(A1, 8u)) return RTC_INVALID_VALUE;
    RtcDateTime value;
    value.year = MEM_R16(A0 + 0u); value.month = MEM_R16(A0 + 2u);
    value.day = MEM_R16(A0 + 4u); value.hour = MEM_R16(A0 + 6u);
    value.minute = MEM_R16(A0 + 8u); value.second = MEM_R16(A0 + 10u);
    value.microsecond = MEM_R32(A0 + 12u);
    if (value.year < 1u || value.year > 9999u) {
        MEM_W32(A1, 0u); MEM_W32(A1 + 4u, 0u);
        return RTC_INVALID_VALUE;
    }
    uint64_t tick = rtc_datetime_to_tick(&value);
    if (tick < RTC_FILETIME_EPOCH_TICK) {
        MEM_W32(A1, 0u); MEM_W32(A1 + 4u, 0u);
        return RTC_INVALID_VALUE;
    }
    uint64_t delta = tick - RTC_FILETIME_EPOCH_TICK;
    if (delta > UINT64_MAX / 10u) {
        MEM_W32(A1, 0u); MEM_W32(A1 + 4u, 0u);
        return RTC_INVALID_VALUE;
    }
    return rtc_write_u64(A1, delta * 10u) ? 0u : RTC_INVALID_VALUE;
}
static uint32_t h_LibcTime(CpuState *s) {
    (void)s;
    if (A0 && !sr_guest_span_writable(A0, 4u)) return RTC_ILLEGAL_ADDR;
    uint64_t tick = rtc_now_tick();
    uint64_t seconds = tick >= RTC_UNIX_EPOCH_TICK
        ? (tick - RTC_UNIX_EPOCH_TICK) / 1000000u : 0u;
    uint32_t value = (uint32_t)seconds;
    if (A0) MEM_W32(A0, value);
    return value;
}
/* The PSP libc clock surface uses the same one-million-tick-per-second unit as
 * sceKernelGetSystemTime; it is an elapsed guest CPU-time value, not Unix time. */
static uint32_t h_LibcClock(CpuState *s) { (void)s; return (uint32_t)now_usec(); }
/* Fill a struct timeval {sec, usec} and struct timezone {minuteswest,dsttime}.
 * Both spans are preflighted before any write. */
static uint32_t h_LibcGettimeofday(CpuState *s) {
    (void)s;
    if (A0 && !sr_guest_span_writable(A0, 8u)) return RTC_ILLEGAL_ADDR;
    if (A1 && !sr_guest_span_writable(A1, 8u)) return RTC_ILLEGAL_ADDR;
    uint64_t tick = rtc_now_tick();
    uint64_t unix_usec = tick >= RTC_UNIX_EPOCH_TICK ? tick - RTC_UNIX_EPOCH_TICK : 0u;
    if (A0) {
        MEM_W32(A0, (uint32_t)(unix_usec / 1000000u));
        MEM_W32(A0 + 4u, (uint32_t)(unix_usec % 1000000u));
    }
    if (A1) {
        MEM_W32(A1, (uint32_t)(-s_psp_timezone_minutes));
        MEM_W32(A1 + 4u, (uint32_t)s_psp_daylight);
    }
    return 0;
}
/* Fill a pspTime {u16 year,month,day,hour,min,sec; u32 usec}. The explicit
 * current-clock variant accepts a signed PSP timezone offset in minutes. */
static uint32_t h_RtcGetCurrentClock(CpuState *s) {
    (void)s;
    if (!sr_guest_span_writable(A0, 16u)) return RTC_ILLEGAL_ADDR;
    uint64_t local_tick;
    rtc_add_offset_wrap(rtc_now_tick(), (int32_t)A1, &local_tick);
    RtcDateTime value;
    rtc_tick_to_datetime(local_tick, &value);
    return rtc_write_datetime(A0, &value) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcGetCurrentClockLocal(CpuState *s) {
    (void)s;
    if (!sr_guest_span_writable(A0, 16u)) return RTC_ILLEGAL_ADDR;
    uint64_t local_tick;
    rtc_add_offset_wrap(rtc_now_tick(), s_psp_timezone_minutes, &local_tick);
    RtcDateTime value;
    rtc_tick_to_datetime(local_tick, &value);
    return rtc_write_datetime(A0, &value) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcConvertUtcToLocal(CpuState *s) {
    (void)s;
    uint64_t in, out;
    if (!A0 || !A1 || !rtc_read_u64(A0, &in) || !sr_guest_span_writable(A1, 8u) ||
        !rtc_add_offset(in, s_psp_timezone_minutes, &out)) return RTC_ILLEGAL_ADDR;
    return rtc_write_u64(A1, out) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcConvertLocalToUtc(CpuState *s) {
    (void)s;
    uint64_t in, out;
    if (!A0 || !A1 || !rtc_read_u64(A0, &in) || !sr_guest_span_writable(A1, 8u) ||
        !rtc_add_offset(in, -s_psp_timezone_minutes, &out)) return RTC_ILLEGAL_ADDR;
    return rtc_write_u64(A1, out) ? 0u : RTC_ILLEGAL_ADDR;
}
/* PSP's three std-fd imports intentionally alias the stdout handle (1).  The
 * descriptor table still reserves 0/1/2 with distinct standard identities so
 * ordinary file allocation and I/O dispatch never infer identity from a number. */
static uint32_t h_StdFd(CpuState *s) { (void)s; return 1; }

/* ---- IoFileMgrForUser: file IO from the game ISO (src/rt/iso.c) ---- */

static void guest_cstr(uint32_t addr, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { uint8_t c = MEM_R8(addr + (uint32_t)i); if (!c) break; out[i] = (char)c; }
    out[i] = 0;
}

extern void f_32200000(CpuState *s);
extern void f_32280000(CpuState *s);
extern void f_322f8868(CpuState *s);

typedef struct {
    uint32_t uid;
    char path[256];
} LoadedModule;

static LoadedModule s_loaded_modules[16];
static int s_nloaded_modules = 0;

static uint32_t h_LoadModule(CpuState *s) {
    char path[256]; guest_cstr(A0, path, sizeof(path));
    uint32_t uid = sr_alloc_uid();
    fprintf(stderr, "sceKernelLoadModule(\"%s\") -> uid=0x%x\n", path, uid);
    populate_known_module(path);
    /* The title checks this flag after the concrete libfont PRX load. Keep it
     * on the explicit PRX path instead of conflating libfont with AV module
     * id 0x302 (PSP_AV_MODULE_ATRAC3PLUS). Title-qualified: only when the
     * manifest configures the compat flag; generic sceKernelLoadModule
     * otherwise performs no guest write. */
    if (strstr(path, "libfont.prx")) {
        uint32_t flag;
        if (sr_title_config_libfont_ready_flag_addr(&flag)) {
            if (!sr_guest_span_writable(flag, 4)) {
                fprintf(stderr,
                        "libfont compat: flag 0x%08x not writable (from %s), skipping\n",
                        flag, sr_title_config()->source_id);
            } else {
                MEM_W32(flag, 1u);
                fprintf(stderr,
                        "libfont compat: flag 0x%08x <- 1 (title %s)\n",
                        flag, sr_title_config()->source_id);
            }
        } else {
            fprintf(stderr,
                    "libfont.prx loaded (generic: no compat flag write, title %s)\n",
                    sr_title_config()->source_id);
        }
    }
    if (s_nloaded_modules < 16) {
        s_loaded_modules[s_nloaded_modules].uid = uid;
        snprintf(s_loaded_modules[s_nloaded_modules].path, sizeof(s_loaded_modules[0].path), "%s", path);
        s_nloaded_modules++;
    }
    return uid;
}

/* LoadModuleByID receives an already-open file UID, so the original path is not part of this
 * ABI call. Populate the fixed set of statically recompiled late modules idempotently; the
 * sorted registry replaces duplicate NIDs and therefore remains safe across repeated loads. */
static uint32_t h_LoadModuleByID(CpuState *s) {
    (void)s;
    populate_known_module("libfont");
    populate_known_module("psmf");
    populate_known_module("libpsmfplayer");
    return sr_alloc_uid();
}

static uint32_t h_StartModule(CpuState *s) {
    uint32_t uid = A0;
    uint32_t arglen = A1;
    uint32_t argp = A2;
    fprintf(stderr, "sceKernelStartModule(uid=0x%x, arglen=%u, argp=0x%x)\n", uid, arglen, argp);
    const char *path = NULL;
    for (int i = 0; i < s_nloaded_modules; i++) {
        if (s_loaded_modules[i].uid == uid) {
            path = s_loaded_modules[i].path;
            break;
        }
    }
    /* Same root cause as the sceUtilityLoadModule fix above (see the long comment on
     * h_UtilityLoadModule): libfont/psmf/libpsmfplayer are fully host-side HLE'd, and their
     * real module_start entries (f_32200000/f_32280000/f_322f8868) are genuine Sony SDK init
     * code that assumes a real PSP kernel underneath -- running them hangs on an unconditional
     * WaitSema. This is a second, independent call path into the exact same three PRXs (via
     * sceKernelLoadModule + sceKernelStartModule instead of sceUtilityLoadModule), so it needs
     * the identical skip. populate_known_module(path) was already called in h_LoadModule when
     * the module was recorded, so it is not repeated here. */
    if (path) {
        if (strstr(path, "libfont.prx")) {
            fprintf(stderr, "sceKernelStartModule: recognized libfont.prx (module_start not executed; sceFont* is fully host-HLE'd)\n");
            return 0;
        } else if (strstr(path, "psmf.prx")) {
            fprintf(stderr, "sceKernelStartModule: recognized psmf.prx (module_start not executed; sceMpeg* is fully host-HLE'd)\n");
            return 0;
        } else if (strstr(path, "libpsmfplayer.prx")) {
            fprintf(stderr, "sceKernelStartModule: recognized libpsmfplayer.prx (module_start not executed; scePsmfPlayer* is fully host-HLE'd)\n");
            return 0;
        }
    }
    fprintf(stderr, "sceKernelStartModule(uid=0x%x) -> unknown module path, skipping entry\n", uid);
    return 0;
}

static uint32_t h_KernelPrintf(CpuState *s) {
    char msg[512]; guest_cstr(A0, msg, sizeof(msg));
    char arg[256] = "";
    /* PSP user-space pointers reside at 0x08000000..0x0BFFFFFF. The old check
     * `s->r[5] < 0x08000000u` was perfectly inverted: it accepted kernel/low
     * addresses and rejected real user strings. Only log the format argument if
     * it points into mapped guest user RAM. */
    if (s->r[5] && s->r[5] >= 0x08000000u && s->r[5] < 0x0C000000u && sr_inrange(s->r[5])) {
        guest_cstr(s->r[5], arg, sizeof(arg));
    }
    fprintf(stderr, "GAMELOG: format='%s' a1=0x%08x a2=0x%08x a3=0x%08x arg='%s'\n", msg, A1, A2, A3, arg);
    if (strstr(msg, "should be called from main thread")) {
        sr_capture_mainthread_diag(s, &sr_last_mt_diag);
        sr_dump_mainthread_diag("MAIN_THREAD_ASSERT", &sr_last_mt_diag);
    }
    return 0;
}

#define SCE_ERROR_KERNEL_TOO_MANY_OPEN_FILES 0x80020320u
#define SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR 0x80020323u
#define SCE_ERROR_KERNEL_INVALID_ARGUMENT    0x80020324u

typedef enum {
    FD_KIND_UNUSED = 0,
    FD_KIND_STD = 1,
    FD_KIND_FILE = 2,
} FdKind;

typedef struct {
    int used;
    FdKind kind;                 /* descriptor identity is independent of its number */
    uint8_t std_stream;          /* 0=stdin, 1=stdout, 2=stderr for FD_KIND_STD */
    uint32_t lba, size, off;
    int64_t async_res;
    FILE *host;
    SrPgd *pgd;
} Fd;
static Fd s_fds[64];
static int64_t s_closed_res[64];

/* Standard descriptors are real reserved entries in the guest namespace.  A
 * closed standard descriptor keeps its kind (so ordinary allocation can never
 * take the slot), while a closed ordinary file becomes fully unused and is
 * eligible for reuse. */
static void hle_fd_release(Fd *f, int preserve_std_kind) {
    if (!f) return;
    FdKind kind = f->kind;
    uint8_t std_stream = f->std_stream;
    if (f->pgd) sr_pgd_free(f->pgd);
    if (f->host) fclose(f->host);
    memset(f, 0, sizeof(*f));
    if (preserve_std_kind && kind == FD_KIND_STD) {
        f->kind = FD_KIND_STD;
        f->std_stream = std_stream;
    }
}

static void hle_fd_init(void) {
    for (size_t i = 0; i < sizeof(s_fds) / sizeof(s_fds[0]); i++) {
        if (s_fds[i].used) hle_fd_release(&s_fds[i], 0);
        else memset(&s_fds[i], 0, sizeof(s_fds[i]));
    }
    memset(s_closed_res, 0, sizeof(s_closed_res));
    for (uint8_t stream = 0; stream < 3; stream++) {
        Fd *f = &s_fds[stream];
        f->used = 1;
        f->kind = FD_KIND_STD;
        f->std_stream = stream;
    }
}

static int hle_fd_is_file(uint32_t fd) {
    return fd < (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) &&
           s_fds[fd].used && s_fds[fd].kind == FD_KIND_FILE;
}

static int hle_fd_is_std(uint32_t fd) {
    return fd < (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) &&
           s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD;
}
typedef struct {
    int used;
    int backend;                 /* 0 = ISO9660, 1 = hierarchical host storage */
    uint32_t index;
    char *path;
    HANDLE find;
    WIN32_FIND_DATAW data;
    int first;
} DirFd;
static DirFd s_dirfds[32];

/* ---- Data-root lookup: serve files extracted from the game's XB archives -------------
 *
 * The game requests paths like "data/menu/text/CommonText_Acce.to" which exist on the
 * ISO ONLY as packed XB archives (xbdata/<subdir>/<archive>.xb{,0,2,3}). The dev workflow
 * extracts those archives once with tools/extract_xb.py into xbdata_extracted/, producing
 * the tree:
 *     <SR_DATAROOT>/<subdir>/<archive>.xb[0-9].d/<relpath-as-on-disc>
 *
 * To serve those files through h_IoOpen / h_IoGetstat we build (once) a sorted relative-path
 * -> host-path cache by walking SR_DATAROOT recursively. Lookup is then O(log N).
 * The cache is published only after the walk, sort, expected-count, and
 * enumerated-size checks succeed; an enumeration or allocation failure destroys
 * the temporary table and permanently fails the data-root route. The actual
 * file open is deferred until the guest requests an entry and fails closed if
 * the indexed host path is no longer readable.
 *
 * SR_DATAROOT defaults to the executable-relative
 * "place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted" tree.
 */

static SrAssetIndex s_data_index;
static atomic_int s_data_state;

/* Extracted-data route states. The historical numeric values 0-3 are part of
 * the observable behavior of this file; DISABLED (4) is the terminal state for
 * runs whose profile declares no extracted-data census and for which no
 * operator configured SR_DATAROOT.
 *
 * Production order is: sr_host_data_prepare() runs ONCE, synchronously, on the
 * startup thread BEFORE any guest execution exists (see driver.c). Guest-time
 * lookups therefore only ever consume a TERMINAL state; a non-terminal
 * observation after guest start means preparation was skipped or is still
 * running, and the lookup fails closed with ONE bounded diagnostic rather than
 * building the index on the scheduler thread (the historical early-boot stall)
 * or spinning. */
#define SR_DATA_STATE_UNINITIALIZED 0
#define SR_DATA_STATE_INITIALIZING  1
#define SR_DATA_STATE_READY         2
#define SR_DATA_STATE_FAILED        3
#define SR_DATA_STATE_DISABLED      4

/* SR_DATA_EXPECTED_COUNT is now title_config owned; see runtime_bindings.expected_data_file_count */

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only white-box counters for the preparation contract. Production
 * builds compile none of this. */
static unsigned long s_data_test_walk_calls;         /* directories enumerated */
static unsigned long s_data_test_build_attempts;     /* census attempts total */
static unsigned long s_data_test_builds_after_guest; /* attempts after guest start */
static int s_data_test_guest_started;
static int s_data_test_pace_ms;                      /* per-directory pacing hook */
#endif
/* SR_DATA_EXPECTED_COUNT legacy define removed: use sr_title_config_expected_data_file_count() */

static char *data_rel_join(const char *prefix, const char *name) {
    return sr_utf8_join_alloc(prefix, name, '/');
}

typedef struct {
    wchar_t *host;
    char *rel;
} DataWalkDir;

static int data_walk_push(DataWalkDir **stack, size_t *count, size_t *capacity,
                          wchar_t *host, char *rel) {
    if (!stack || !count || !capacity || !host || !rel || *count == SIZE_MAX) return 0;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity : 16u;
        while (next <= *count) {
            if (next > SIZE_MAX / 2u) return 0;
            next *= 2u;
        }
        if (next > SIZE_MAX / sizeof(**stack)) return 0;
        DataWalkDir *grown = (DataWalkDir *)realloc(*stack, next * sizeof(**stack));
        if (!grown) return 0;
        *stack = grown;
        *capacity = next;
    }
    (*stack)[(*count)++] = (DataWalkDir){host, rel};
    return 1;
}

/* Iterative depth-first walk of `root`, recording every regular file's relative
 * path and absolute host path.  An explicit heap stack keeps a crafted 32k
 * extended path from consuming the host thread's call stack. */
static int data_walk(const wchar_t *root, const char *relprefix, SrAssetIndex *index) {
    if (!root || !relprefix || !index) return 0;
    DataWalkDir *stack = NULL;
    size_t stack_count = 0, stack_capacity = 0;
    size_t root_len = wcslen(root);
    if (root_len == SIZE_MAX || root_len + 1u > SIZE_MAX / sizeof(wchar_t)) return 0;
    wchar_t *initial_host = (wchar_t *)malloc((root_len + 1u) * sizeof(*initial_host));
    char *initial_rel = sr_asset_index_strdup(relprefix);
    if (!initial_host || !initial_rel) {
        free(initial_host);
        free(initial_rel);
        return 0;
    }
    memcpy(initial_host, root, (root_len + 1u) * sizeof(*initial_host));
    if (!data_walk_push(&stack, &stack_count, &stack_capacity, initial_host, initial_rel)) {
        free(initial_host);
        free(initial_rel);
        free(stack);
        return 0;
    }

    int ok = 1;
    while (ok && stack_count != 0u) {
        DataWalkDir current = stack[--stack_count];
#ifdef SR_HLE_THREAD_SELFTEST
        /* Test-only pacing: proves the census COMPLETES before the guest-start
         * boundary when the hook placement is correct (a placement proof, not a
         * speed proof). Production builds never pace. */
        if (s_data_test_pace_ms > 0) Sleep((DWORD)s_data_test_pace_ms);
        s_data_test_walk_calls++;
#endif
        wchar_t *pattern = NULL;
        if (!sr_wide_join_alloc(current.host, L"*", &pattern)) {
            fprintf(stderr, "host_data: failed to construct enumeration pattern\n");
            free(current.host);
            free(current.rel);
            ok = 0;
            break;
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        DWORD first_error = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        free(pattern);
        if (h == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "host_data: enumeration failed (error=%lu)\n", first_error);
            free(current.host);
            free(current.rel);
            ok = 0;
            break;
        }

        for (;;) {
            if (!(fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
                                              (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))) {
                char *name = NULL;
                char *child_rel = NULL;
                wchar_t *child_host = NULL;
                char *host_utf8 = NULL;
                char *key = NULL;
                if (!sr_wide_to_utf8_alloc(fd.cFileName, &name) ||
                    !(child_rel = data_rel_join(current.rel, name)) ||
                    !sr_wide_join_alloc(current.host, fd.cFileName, &child_host)) {
                    fprintf(stderr, "host_data: path conversion/join failed during enumeration\n");
                    ok = 0;
                } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                        fprintf(stderr, "host_data: refusing reparse-point directory\n");
                        ok = 0;
                    } else if (!data_walk_push(&stack, &stack_count, &stack_capacity,
                                               child_host, child_rel)) {
                        fprintf(stderr, "host_data: directory-stack allocation failed\n");
                        ok = 0;
                    } else {
                        child_host = NULL;
                        child_rel = NULL;
                    }
                } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    fprintf(stderr, "host_data: refusing reparse-point file\n");
                    ok = 0;
                } else {
                    HANDLE probe = CreateFileW(child_host, GENERIC_READ,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    DWORD probe_error = probe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
                    if (probe == INVALID_HANDLE_VALUE) {
                        fprintf(stderr, "host_data: indexed file readability check failed (entry=%zu error=%lu)\n",
                                index->count, probe_error);
                        ok = 0;
                    } else if (!sr_wide_to_utf8_alloc(child_host, &host_utf8)) {
                        fprintf(stderr, "host_data: host-path conversion failed during enumeration\n");
                        ok = 0;
                    }
                }
                if (ok && host_utf8) {
                    int variant = -1;
                    uint64_t file_size = ((uint64_t)fd.nFileSizeHigh << 32) |
                                         (uint64_t)fd.nFileSizeLow;
                    if (file_size > UINT32_MAX) {
                        fprintf(stderr, "host_data: indexed file exceeds guest size limit\n");
                        ok = 0;
                    } else if (!sr_asset_index_key_from_rel(child_rel, &key, &variant)) {
                        fprintf(stderr, "host_data: index-key conversion failed after %zu files\n",
                                index->count);
                        ok = 0;
                    } else if (!sr_asset_index_add_sized(index, key, host_utf8, variant,
                                                         file_size)) {
                        fprintf(stderr, "host_data: index allocation failed after %zu files\n",
                                index->count);
                        ok = 0;
                    }
                }
                free(name);
                free(child_rel);
                free(child_host);
                free(host_utf8);
                free(key);
            }
            if (!ok) break;
            if (!FindNextFileW(h, &fd)) {
                DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) {
                    fprintf(stderr, "host_data: enumeration terminated with error=%lu\n", error);
                    ok = 0;
                }
                break;
            }
        }
        FindClose(h);
        free(current.host);
        free(current.rel);
    }
    while (stack_count != 0u) {
        DataWalkDir pending = stack[--stack_count];
        free(pending.host);
        free(pending.rel);
    }
    free(stack);
    return ok;
}

static int data_root_validate(const wchar_t *root, int configured) {
    if (!root) return 0;
    DWORD attributes = GetFileAttributesW(root);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        fprintf(stderr, "host_data: root attributes failed (error=%lu)\n", error);
        return 0;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "host_data: data root is not a directory\n");
        return 0;
    }
    /* An explicitly supplied root is an operator-owned path and may be a
     * lawful junction used to stage a long-path fixture.  The executable-
     * anchored default must not silently follow a junction outside the repo. */
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) && !configured) {
        fprintf(stderr, "host_data: executable-relative root is a reparse point\n");
        return 0;
    }
    return 1;
}

/* Enumeration is the transactional completeness boundary.  WIN32_FIND_DATAW
 * supplies each regular file's checked size, while the walk's read-open probe
 * catches ACL/deletion races before the table is published.  The real open
 * remains fail-closed in h_IoOpen when the guest requests a file. */
static int data_validate_index(const SrAssetIndex *index) {
    if (!index || index->count == 0) return 0;
{
        uint32_t expected = sr_title_config_expected_data_file_count();
        if (expected != 0 && index->count != (size_t)expected) {
            fprintf(stderr, "host_data: expected %u files but enumerated %zu; refusing index\n",
                    (unsigned)expected, index->count);
            return 0;
        }
    }
    for (size_t i = 0; i < index->count; i++) {
        if (index->entries[i].size > UINT32_MAX) {
            fprintf(stderr, "host_data: indexed file metadata exceeds guest size limit (entry=%zu)\n", i);
            return 0;
        }
    }
    return 1;
}

/* Build the extracted-data index ONCE, before guest execution starts.
 *
 * Applicability predicate: the route applies iff an operator explicitly
 * configured SR_DATAROOT, OR the build profile declares an expected census
 * (sr_title_config_expected_data_file_count() > 0, set only when a title manifest configures expected_data_file_count). A generic profile
 * without SR_DATAROOT terminates DISABLED with ZERO filesystem enumeration --
 * a tree staged for one title can no longer be enumerated on behalf of another
 * build or a synthetic guest. Census internals (walk, normalization, sort,
 * publication checks, diagnostics) are unchanged from the historical lazy
 * implementation; only WHEN it runs and WHO may trigger it changed. */
int sr_host_data_prepare(void) {
#ifdef SR_HLE_THREAD_SELFTEST
    s_data_test_build_attempts++;
    if (s_data_test_guest_started &&
        atomic_load_explicit(&s_data_state, memory_order_acquire) != SR_DATA_STATE_READY)
        s_data_test_builds_after_guest++;
#endif
    int expected = SR_DATA_STATE_UNINITIALIZED;
    if (!atomic_compare_exchange_strong_explicit(&s_data_state, &expected,
                                                 SR_DATA_STATE_INITIALIZING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        /* Idempotent for repeat calls (selftests, future callers): a route
         * already terminal or being prepared is never re-scanned here. */
        return atomic_load_explicit(&s_data_state, memory_order_acquire);
    }
    wchar_t *configured_root = NULL;
    int configured_present = 0;
    int env_ok = sr_wide_env_alloc(L"SR_DATAROOT", &configured_root, &configured_present);
    if (!configured_present && sr_title_config_expected_data_file_count() == 0) {
        free(configured_root);
        atomic_store_explicit(&s_data_state, SR_DATA_STATE_DISABLED, memory_order_release);
        fprintf(stderr, "host_data: disabled (no SR_DATAROOT configured and this profile "
                        "declares no extracted-data census); guest lookups will fall back "
                        "to disc/VFS\n");
        return SR_DATA_STATE_DISABLED;
    }
    SrAssetIndex temporary;
    sr_asset_index_init(&temporary);
    const char *root_label = configured_present ? "<configured SR_DATAROOT>" :
        "<executable>/../../place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted";
    fprintf(stderr, "host_data: scanning %s ...\n", root_label);
    wchar_t *root_wide = NULL;
    int root_ok = env_ok && (configured_present ?
        (configured_root[0] && sr_wide_configured_root_wide_alloc(configured_root, &root_wide)) :
        sr_wide_module_data_root(&root_wide));
    if (!root_ok) {
        if (!env_ok)
            fprintf(stderr, "host_data: SR_DATAROOT could not be read\n");
        else if (configured_present)
            fprintf(stderr, "host_data: SR_DATAROOT is configured but is not a valid absolute path\n");
        else
            fprintf(stderr, "host_data: executable-relative data root could not be resolved\n");
    }
    free(configured_root);
    if (root_ok && !data_root_validate(root_wide, configured_present)) root_ok = 0;
    if (!root_ok ||
        !data_walk(root_wide, "", &temporary) ||
        !sr_asset_index_finalize(&temporary) ||
        !data_validate_index(&temporary)) {
        fprintf(stderr, "host_data: index initialization failed; refusing partial index\n");
        free(root_wide);
        sr_asset_index_destroy(&temporary);
        atomic_store_explicit(&s_data_state, SR_DATA_STATE_FAILED, memory_order_release);
        return SR_DATA_STATE_FAILED;
    }
    free(root_wide);
    if (!sr_asset_index_publish(&s_data_index, &temporary)) {
        fprintf(stderr, "host_data: failed to publish finalized index\n");
        sr_asset_index_destroy(&temporary);
        atomic_store_explicit(&s_data_state, SR_DATA_STATE_FAILED, memory_order_release);
        return SR_DATA_STATE_FAILED;
    }
    atomic_store_explicit(&s_data_state, SR_DATA_STATE_READY, memory_order_release);
    fprintf(stderr, "host_data: indexed %zu files under %s\n", s_data_index.count, root_label);
    return SR_DATA_STATE_READY;
}

/* Entry count of the published index; only meaningful after READY. Used by the
 * driver's BOOT_EVENT index_prepare_end record so a boot log states how much
 * was prepared without exposing internal structures. */
size_t sr_host_data_entry_count(void) {
    return atomic_load_explicit(&s_data_state, memory_order_acquire) == SR_DATA_STATE_READY
        ? s_data_index.count : 0u;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only white-box accessors for the preparation contract. Production
 * builds compile none of this. */
void sr_hle_test_data_mark_guest_start(void) { s_data_test_guest_started = 1; }
unsigned long sr_hle_test_data_walk_calls(void) { return s_data_test_walk_calls; }
unsigned long sr_hle_test_data_build_attempts(void) { return s_data_test_build_attempts; }
unsigned long sr_hle_test_data_builds_after_guest(void) { return s_data_test_builds_after_guest; }
int sr_hle_test_data_state(void) {
    return atomic_load_explicit(&s_data_state, memory_order_acquire);
}
size_t sr_hle_test_data_entry_count(void) { return s_data_index.count; }

void sr_hle_test_data_reset(int pace_ms) {
    sr_asset_index_destroy(&s_data_index);
    sr_asset_index_init(&s_data_index);
    atomic_store_explicit(&s_data_state, SR_DATA_STATE_UNINITIALIZED, memory_order_release);
    s_data_test_walk_calls = 0;
    s_data_test_build_attempts = 0;
    s_data_test_builds_after_guest = 0;
    s_data_test_guest_started = 0;
    s_data_test_pace_ms = pace_ms;
}
#endif

static char *data_normalize_guest_key(const char *guest_path, int *wanted_variant) {
    if (!guest_path || !wanted_variant) return NULL;
    /* Strip device prefix the same way iso_lookup does. */
    const char *p = guest_path;
    const char *c = strchr(p, ':');
    if (c) p = c + 1;
    while (*p == '/' || *p == '\\') p++;
    /* Cache keys are relative to an archive output directory (e.g. "data/menu/text/x.to"), but
     * the game may request the same file by its full "PSP_GAME/USRDIR/..." path. Strip
     * those leading segments so the binary search matches regardless of which form the
     * guest passes. */
    if (_strnicmp(p, "PSP_GAME/", 8) == 0 || _strnicmp(p, "PSP_GAME\\", 8) == 0) p += 8;
    if (_strnicmp(p, "USRDIR/", 7) == 0 || _strnicmp(p, "USRDIR\\", 7) == 0) p += 7;
    /* Localized roots are selected by the game itself.  For this build the table is
     * data_00_USE, data_02_FRE, data_03_SPA, ... and the matching archives are .xb0,
     * .xb2, .xb3, ... with internal paths rooted at plain data/. */
    *wanted_variant = -2;          /* -2 = unqualified path */
    const char *localized_tail = NULL;
    size_t guest_key_length = strlen(p);
    if (guest_key_length >= 8u && _strnicmp(p, "data_", 5) == 0 &&
        p[5] >= '0' && p[5] <= '9' && p[6] >= '0' && p[6] <= '9' &&
        p[7] == '_') {
        const char *slash = strpbrk(p + 8, "/\\");
        if (slash) {
            *wanted_variant = (p[5] - '0') * 10 + (p[6] - '0');
            localized_tail = slash;
        }
    }
    /* Lowercase fold. guest_path is NUL-terminated by guest_cstr, so take its
     * length up front and only read defined bytes (avoids reading uninitialized
     * tail bytes / garbage past the terminator). */
    char *localized = NULL;
    if (localized_tail) {
        size_t tail_len = strlen(localized_tail);
        if (tail_len > SIZE_MAX - 5u) return NULL;
        localized = (char *)malloc(tail_len + 5u);
        if (!localized) return NULL;
        memcpy(localized, "data", 4u);
        memcpy(localized + 4u, localized_tail, tail_len + 1u);
        p = localized;
    }
    size_t length = strlen(p);
    char *key = (char *)malloc(length + 1u);
    if (!key) { free(localized); return NULL; }
    for (size_t n = 0; n < length; n++) {
        char ch = p[n];
        if (ch == '\\') ch = '/';
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        key[n] = ch;
    }
    key[length] = 0;
    free(localized);
    return key;
}

/* Case-fold the guest key and binary-search the complete cache.
 *
 * Guest-time consumption contract: only a TERMINAL route state is consumed.
 * A non-terminal observation means preparation never ran (or has not finished)
 * before guest execution reached a lookup -- the exact early-boot stall this
 * seam exists to prevent. The lookup therefore refuses with ONE bounded
 * diagnostic and never begins, resumes, or waits on a census here. */
static const SrAssetIndexEntry *host_data_lookup(const char *guest_path) {
    int state = atomic_load_explicit(&s_data_state, memory_order_acquire);
    if (state != SR_DATA_STATE_READY) {
        static atomic_int s_data_lookup_warned;
        if (!atomic_exchange_explicit(&s_data_lookup_warned, 1, memory_order_acq_rel)) {
            fprintf(stderr, "host_data: lookup refused before preparation reached a "
                            "terminal state (state=%d); disc/VFS fallback stays active\n",
                    state);
        }
        return NULL;
    }
    int wanted_variant = -2;
    char *key = data_normalize_guest_key(guest_path, &wanted_variant);
    if (!key) return NULL;
    size_t first = sr_asset_index_lower_bound(&s_data_index, key);
    const SrAssetIndexEntry *chosen = NULL;
    for (size_t i = first; i < s_data_index.count &&
                           strcmp(s_data_index.entries[i].key, key) == 0; i++) {
        const SrAssetIndexEntry *candidate = &s_data_index.entries[i];
        if (wanted_variant >= 0) {
            if (candidate->variant == wanted_variant) { chosen = candidate; break; }
        } else if (!chosen || candidate->variant == -1 ||
                   (chosen->variant != -1 && candidate->variant < chosen->variant)) {
            chosen = candidate;
        }
    }
    free(key);
    return chosen;
}

/* Host-serve a guest file from the extracted-XB data root (SR_DATAROOT).
 *
 * This is the long-term groundwork for the text-loader (.to) fix and any other
 * game file whose bytes live inside an XB archive that the recomp's VFS/HLE
 * never reads off the disc. The normal sceIoOpen path handles files present in
 * the extracted tree
 * we open the host file here and return a real fd that is indistinguishable
 * from one h_IoOpen produced (same s_fds slot, same read/seek/close semantics),
 * so the downstream sceIoRead/sceIoSeek/sceIoClose thunks just work.
 *
 * Path normalization strips a device prefix (disc0:) and any leading
 * PSP_GAME/USRDIR or USRDIR segment, because the extracted cache keys are
 * relative (e.g. "data/menu/text/commonttext_acce.to") while the guest may pass
 * a full "disc0:/PSP_GAME/USRDIR/..." path. Returns the fd (>=1) on success or
 * -1 if the path is not in the extracted set (caller should fall through to the
 * real opener). The returned entry is owned by the immutable cache. */
static uint32_t h_IoOpen(CpuState *s) {
    /* a0=path, a1=flags, a2=mode. Returns an fd (>=0) or a negative error.
     * PSP flags: WRONLY=2, RDWR=3, APPEND=0x100, CREAT=0x200, TRUNC=0x400. */
    char path[256];
    guest_cstr(A0, path, sizeof(path));
    uint32_t flags = A1;
    if (getenv("SR_IOLOG"))
        fprintf(stderr, "HLE_IoOpen: opening '%s' flags=0x%x\n", path, flags);
    /* I4 mirror: every IoOpen after umd.ufl Read-completes is suspect of 'missing-game-file'
     * and pre-empts engine_Shutdown. Print path + uid of caller. */
    if (getenv("SR_POSTUMD")) {
        extern uint64_t sr_postumd_reads(void);
        if (sr_postumd_reads() > 0) {
            fprintf(stderr, "POSTUMD: Open(%s) flags=0x%x caller_uid=0x%x cur_uid=0x%x pc=0x%08x ra=0x%08x\n",
                    path, flags, A0 ? A0 : 0, sched_current_uid(), s->pc, s->r[31]);
            fflush(stderr);
        }
    }
    if (getenv("SR_PATHHEX")) {
        int bad = 0; for (int i = 0; path[i]; i++) if ((unsigned char)path[i] < 0x20 || (unsigned char)path[i] >= 0x7f) bad = 1;
        if (bad || path[0] == 0) {
            fprintf(stderr, "Open BAD path ptr=0x%08x bytes:", A0);
            for (int i = 0; i < 24; i++) fprintf(stderr, " %02x", MEM_R8(A0 + (uint32_t)i));
            fprintf(stderr, "\n");
        }
    }
    int slot = -1;
    for (int i = 3; i < (int)(sizeof(s_fds) / sizeof(s_fds[0])); i++)
        if (!s_fds[i].used) { slot = i; break; }
    if (slot < 0) return SCE_ERROR_KERNEL_TOO_MANY_OPEN_FILES;  /* too many open files */
    memset(&s_fds[slot], 0, sizeof(s_fds[slot]));
    s_fds[slot].kind = FD_KIND_FILE;

    int writing = (flags & 0x0002) != 0;        /* WRONLY or RDWR */
    int creating = (flags & 0x0200) != 0;
    uint32_t lba, size;
    int in_iso = (iso_lookup(path, &lba, &size) == 0);

    if (writing || creating || !in_iso) {
        /* Host-backed file (writable storage). */
        char *hp = host_path_alloc(path);
        const wchar_t *mode;
        if (flags & 0x0400) mode = L"w+b";            /* TRUNC */
        else if (flags & 0x0100) mode = L"a+b";       /* APPEND */
        else if (writing || creating) mode = L"r+b"; /* update; fall back to create below */
        else mode = L"rb";                           /* read-only host file (e.g. a prior save) */
        FILE *fp = hp ? sr_fopen_utf8(hp, mode) : NULL;
        if (!fp && (writing || creating) && hp) fp = sr_fopen_utf8(hp, L"w+b");
        free(hp);
        if (!fp) {
            /* Try the extracted-XB data-root cache (game asks for "data/menu/text/<X>.to"
             * but those files only exist on the ISO inside XB archives; dev workflow
             * extracts them under SR_DATAROOT/<sub>/<arc>.xb.d/<relpath>). */
            const SrAssetIndexEntry *entry = !writing && !creating ? host_data_lookup(path) : NULL;
            if (entry) {
                FILE *dfp = sr_fopen_utf8(entry->host, L"rb");
                if (dfp) {
                    uint32_t actual_size = 0;
                    if (!sr_stream_size_u32(dfp, &actual_size)) {
                        fprintf(stderr, "host_data: indexed asset size is unavailable or exceeds guest limit for %s\n",
                                path);
                        fclose(dfp);
                        return 0x80010005;
                    }
                    if (getenv("SR_IOLOG")) fprintf(stderr, "Open(%s) -> data-root %s size=%u\n",
                                                    path, entry->host, (unsigned)actual_size);
                    s_fds[slot].used = 1; s_fds[slot].host = dfp; s_fds[slot].lba = 0;
                    s_fds[slot].size = actual_size;
                    s_fds[slot].off = 0;
                    return (uint32_t)slot;
                }
                /* An indexed extracted asset must not silently fall through to an
                 * unrelated ISO or synthetic host path.  Enumeration was complete;
                 * an access-time open failure is a hard, attributed VFS error. */
                fprintf(stderr, "host_data: indexed asset open failed for %s (errno=%d)\n",
                        path, errno);
                if (in_iso) goto from_iso;
                return 0x80010002;
            }
            if (in_iso) goto from_iso;
            fprintf(stderr, "sceIoOpen: not found: %s\n", path);
            return 0x80010002;
        }
        uint32_t host_size = 0;
        if (!sr_stream_size_u32(fp, &host_size)) {
            fclose(fp);
            fprintf(stderr, "sceIoOpen: host file size is unavailable or exceeds guest limit: %s\n",
                    path);
            return 0x80010005;
        }
        s_fds[slot].used = 1; s_fds[slot].host = fp; s_fds[slot].lba = 0;
        s_fds[slot].size = host_size; s_fds[slot].off = 0;
        return (uint32_t)slot;
    }
from_iso:
    s_fds[slot].used = 1; s_fds[slot].host = NULL;
    s_fds[slot].lba = lba; s_fds[slot].size = size; s_fds[slot].off = 0;
    return (uint32_t)slot;
}
static uint32_t h_IoWrite(CpuState *s) {
    /* a0=fd, a1=src, a2=count. Returns bytes written. */
    uint32_t fd = A0, src = A1, count = A2;
    if (fd >= (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) || !s_fds[fd].used)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    Fd *f = &s_fds[fd];
    if (hle_fd_is_std(fd)) {                          /* std streams: dump to stderr */
        uint8_t buf[1024]; uint32_t n = count < sizeof(buf) ? count : sizeof(buf) - 1;
        for (uint32_t k = 0; k < n; k++) buf[k] = (uint8_t)MEM_R8(src + k);
        buf[n] = 0;
        if (hle_log_on()) {
            fprintf(stderr, "SCETYPEWRITE[%u]: %s\n", fd, (char*)buf);
            fflush(stderr);
        }
        return count;
    }
    if (f->kind != FD_KIND_FILE) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (!f->host) return 0x80010013;                 /* read-only (ISO) fd: not writable */
    fseek(f->host, (long)f->off, SEEK_SET);
    uint8_t tmp[4096]; uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
        for (uint32_t k = 0; k < n; k++) tmp[k] = (uint8_t)MEM_R8(src + done + k);
        done += (uint32_t)fwrite(tmp, 1, n, f->host);
        if (done < count) break;
    }
    fflush(f->host);
    f->off += done; if (f->off > f->size) f->size = f->off;
    return done;
}
static uint32_t h_IoRead(CpuState *s) {
    /* a0=fd, a1=dst, a2=count. Returns bytes read. */
    uint32_t fd = A0, dst = A1, count = A2;
    if (fd < 3 && s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD)
        return 0x80010009; /* baseline behavior preserved for standard streams */
    if (!hle_fd_is_file(fd)) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    Fd *f = &s_fds[fd];
    if (f->pgd) {
        /* Decrypt-on-read: f->off/f->size are the logical (decrypted) view; the
         * ciphertext is read from f->host at physical block offsets by pgd.c. */
        if (f->off >= f->size) return 0;
        if (f->off + count > f->size) count = f->size - f->off;
        uint32_t bs = sr_pgd_block_size(f->pgd), done = 0;
        while (done < count) {
            uint32_t L = f->off + done, bidx = L / bs, boff = L % bs;
            const uint8_t *blk = sr_pgd_block(f->pgd, f->host, bidx);
            if (!blk) break;
            uint32_t blen = sr_pgd_block_len(f->pgd, bidx);
            if (boff >= blen) break;
            uint32_t n = blen - boff; if (n > count - done) n = count - done;
            for (uint32_t k = 0; k < n; k++) MEM_W8(dst + done + k, blk[boff + k]);
            done += n;
        }
        f->off += done;
        return done;
    }
    if (f->off + count > f->size) count = f->size - f->off;
    /* Read in chunks straight into guest memory, from the host file or the ISO. */
    uint8_t tmp[4096];
    uint32_t done = 0;
    if (f->host) fseek(f->host, (long)f->off, SEEK_SET);
    while (done < count) {
        uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
        if (f->host) { size_t got = fread(tmp, 1, n, f->host); if (got < n) n = (uint32_t)got; }
        else {
            int got = iso_read(f->lba, f->off + done, tmp, n);
            n = got > 0 ? (uint32_t)got : 0;
        }
        for (uint32_t k = 0; k < n; k++) MEM_W8(dst + done + k, tmp[k]);
        done += n;
        if (n == 0) break;
    }
    f->off += done;
    if (getenv("SR_IOLOG")) {
        static int n = 0;
        if (n++ < 4000)
            fprintf(stderr, "Read vbl=%u fd=%u off=%u size=%u dst=0x%08x -> %u\n",
                    s_vcount_fwd, fd, f->off - done, count, dst, done);
    }
    /* I4 diagnostic: every Read on the worker (uid 0x115) AFTER dst==0x30b8d0 (umd.ufl
     * buffer) arms tracking. We record path+dst for every subsequent Read so we can see
     * if engine_Shutdown pre-empted a missing-file IoOpen. SR_POSTUMD env-gates; default
     * off by default so normal runs don't get spammed. */
    if (getenv("SR_POSTUMD") && sched_current_is_worker()) {
        if (dst == 0x0030b8d0u) {
            sr_postumd_advance(1);   /* arm */
            fprintf(stderr, "POSTUMD: armed at first umd.ufl Read fd=%u size=%u dst=0x%08x -> %u\n",
                    fd, count, dst, done);
            fflush(stderr);
        } else if (s_postumd_active) {
            sr_postumd_advance(0);
            if (s_postumd_count <= 64) {
                fprintf(stderr, "POSTUMD: Read fd=%u off_after=%u size=%u dst=0x%08x -> %u\n",
                        fd, f->off, count, dst, done);
                fflush(stderr);
            }
        }
    }
    /* Phase 2.A diagnostic: classify the umd.ufl payload the worker (uid 0x115) reads
     * into the guest buffer at 0x0030b8d0 (411568 bytes). The magic tells us which
     * decoder path we need -- raw PRX (rebase in host), scrambled (decrypt first), or
     * plain data (loader never executes it). One-shot so we don't spam the trace.
     *
     * Pivot: Phase 2.A revealed this buffer is a CSV path-manifest, not a PRX. The
     * guest launcher walks it to validate inner-file boundaries against ISO UMD disc
     * queries. Sample rows here so we can match the tokenizer the engine uses. */
    if (dst == 0x0030b8d0u && count == 411568u && getenv("SR_UMDDUMP")) {
        static int dumped = 0;
        if (!dumped) {
            dumped = 1;
            fprintf(stderr, "UMD_DUMP: umd.ufl head @ 0x0030b8d0 (fd=%u):\n", fd);
            for (int off = 0; off < 64; off++) {
                fprintf(stderr, "  +0x%03x:", (unsigned)off);                for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", MEM_R8((uint32_t)(0x0030b8d0u + (uint32_t)off + (uint32_t)i)));
                fprintf(stderr, " ");
                for (int i = 0; i < 16; i++) {
                    uint8_t c = MEM_R8((uint32_t)(0x0030b8d0u + (uint32_t)off + (uint32_t)i));
                    fprintf(stderr, "%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
                }
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "UMD_DUMP: trailing 64 bytes of the buffer:\n");
            uint32_t base = 0x0030b8d0u;
            uint32_t len = 411568u;
            for (int off = (int)(len - 64); off < (int)len; off += 16) {
                fprintf(stderr, "  +0x%06x:", (unsigned)off);
                for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", MEM_R8(base + (uint32_t)off + (uint32_t)i));
                fprintf(stderr, " ");
                for (int i = 0; i < 16; i++) {
                    uint8_t c = MEM_R8(base + (uint32_t)off + (uint32_t)i);
                    fprintf(stderr, "%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
                }
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    }
    return done;
}
static uint32_t h_IoLseek32(CpuState *s) {
    /* a0=fd, a1=offset, a2=whence. Returns new position (32-bit). */
    uint32_t fd = A0; int32_t off = (int32_t)A1; uint32_t whence = A2;
    if (fd < 3 && s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD)
        return 0x80010009; /* baseline behavior preserved for standard streams */
    if (!hle_fd_is_file(fd)) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (whence >= 3u) return SCE_ERROR_KERNEL_INVALID_ARGUMENT;
    Fd *f = &s_fds[fd];
    int64_t base = whence == 1 ? f->off : (whence == 2 ? f->size : 0);
    int64_t np = base + off; if (np < 0) np = 0; if (np > f->size) np = f->size;
    f->off = (uint32_t)np;
    return f->off;
}
static uint32_t h_IoLseek(CpuState *s) {
    /* a0=fd, [a2:a3]=64-bit offset, [sp+16]=whence. Returns 64-bit pos in v0:v1. */
    uint32_t fd = A0;
    int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);
    uint32_t whence = stack_arg(s, 0);
    if (fd < 3 && s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD) {
        s->r[3] = 0;
        return 0x80010009; /* baseline behavior preserved for standard streams */
    }
    if (!hle_fd_is_file(fd)) { s->r[3] = 0xFFFFFFFF; return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR; }
    if (whence >= 3u) { s->r[3] = 0xFFFFFFFF; return SCE_ERROR_KERNEL_INVALID_ARGUMENT; }
    Fd *f = &s_fds[fd];
    int64_t base = whence == 1 ? f->off : (whence == 2 ? f->size : 0);
    int64_t np = base + off; if (np < 0) np = 0; if (np > f->size) np = f->size;
    f->off = (uint32_t)np;
    s->r[3] = (uint32_t)((uint64_t)np >> 32);
    return (uint32_t)np;
}
/* sceIoIoctl (NID 0x63632449): device control on an open fd. First hit live
 * 2026-07-18, right after the first real savegame started existing (boot now
 * takes the save-read path). Reference semantics: PPSSPP Core/HLE/sceIo.cpp
 * __IoIoctl (pinned sparse clone, optional local reverse-engineering notes).
 * Implements the plain-file
 * command set the flat Fd model supports 1:1; the PGD/DRM trio implements only
 * the "file is not actually encrypted" path (PPSSPP proceeds identically for
 * plaintext files, and retail HST savedata is plaintext). Unknown commands
 * print one loud line per unique cmd and return FUNCTION_NOT_SUPPORTED
 * (0x80010086) exactly like the reference fallback. */
static uint32_t h_IoIoctl(CpuState *s) {
    /* a0=fd, a1=cmd, a2=indata, a3=inlen, t0=outdata, t1=outlen */
    uint32_t fd = A0, cmd = A1, in = A2, inlen = A3;
    uint32_t out = stack_arg(s, 0), outlen = stack_arg(s, 1);
    if (fd < 3 && s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD)
        return 0x80010009; /* baseline behavior preserved for standard streams */
    if (!hle_fd_is_file(fd)) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    Fd *f = &s_fds[fd];
    if (getenv("SR_IOLOG"))
        fprintf(stderr, "Ioctl fd=%u cmd=0x%08x in=0x%08x/%u out=0x%08x/%u\n",
                fd, cmd, in, inlen, out, outlen);
    switch (cmd) {
    case 0x01020003:  /* get sector size: ISOs always use 2048-byte sectors */
        if (!out || outlen < 4) return 0x80010016;
        MEM_W32(out, 2048u);
        return 0;
    case 0x01020004:  /* get current byte offset */
        if (!out || outlen < 4) return 0x80010016;
        MEM_W32(out, f->off);
        return 0;
    case 0x01010005: { /* seek: indata = { u64 offset; u32 unk; u32 whence } */
        if (!in || inlen < 4) return 0x80010016;
        int64_t off = (int64_t)(((uint64_t)MEM_R32(in + 4) << 32) | MEM_R32(in));
        uint32_t whence = MEM_R32(in + 12);
        int64_t base = whence == 1 ? f->off : (whence == 2 ? f->size : 0);
        int64_t np = base + off;
        if (np < 0 || np > f->size) return 0x80010005;  /* can't seek past EOF here */
        f->off = (uint32_t)np;
        return 0;
    }
    case 0x01020006:  /* get start sector (ISO-backed fds; host files report 0) */
        if (!out || outlen < 4) return 0x80010016;
        MEM_W32(out, f->host ? 0u : f->lba);
        return 0;
    case 0x01020007:  /* get file size, written as 64-bit */
        if (!out || outlen < 8) return 0x80010016;
        MEM_W32(out, f->size);
        MEM_W32(out + 4, 0u);
        return 0;
    case 0x01030008: { /* read: indata = u32 byte count, destination = outdata */
        if (!in || inlen < 4) return 0x80010016;
        uint32_t count = MEM_R32(in);
        if (!out || count > outlen) return 0x80010016;
        if (f->off + count > f->size) count = f->size - f->off;
        uint8_t tmp[4096];
        uint32_t done = 0;
        if (f->host) fseek(f->host, (long)f->off, SEEK_SET);
        while (done < count) {
            uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
            if (f->host) { size_t got = fread(tmp, 1, n, f->host); if (got < n) n = (uint32_t)got; }
            else {
                int got = iso_read(f->lba, f->off + done, tmp, n);
                n = got > 0 ? (uint32_t)got : 0;
            }
            for (uint32_t k = 0; k < n; k++) MEM_W8(out + done + k, tmp[k]);
            done += n;
            if (n == 0) break;
        }
        f->off += done;
        return done;
    }
    case 0x01d20001:  /* tell (PPSSPP writes the raw seek position here too) */
        if (!out || outlen < 4) return 0x80010016;
        MEM_W32(out, f->off);
        return 0;
    case 0x04100001: { /* PGD decrypt setup: attach real amctrl decryption (src/rt/pgd.c) */
        if (!f->host || !in || inlen < 16) return 0x80510204;
        uint8_t vkey[16];
        for (int i = 0; i < 16; i++) vkey[i] = (uint8_t)MEM_R8(in + (uint32_t)i);
        uint8_t header[0x90];
        long save = ftell(f->host);
        fseek(f->host, 0, SEEK_SET);
        size_t got = fread(header, 1, sizeof(header), f->host);
        if (save >= 0) fseek(f->host, save, SEEK_SET);
        if (got != sizeof(header)) return 0x80510204;
        SrPgd *p = sr_pgd_open(header, vkey);
        if (!p) {
            /* Wrong key, corrupt header, or a DRM type that needs a per-console
             * fuse key. Fail exactly like before so the game streams from UMD. */
            fprintf(stderr, "sceIoIoctl: fd=%u PGD open failed; game will stream from UMD\n", fd);
            return 0x80510204;  /* SCE_ERROR_PGD_INVALID_HEADER */
        }
        if (f->pgd) sr_pgd_free(f->pgd);
        f->pgd = p;
        f->size = sr_pgd_data_size(p);   /* switch fd to the logical decrypted view */
        f->off = 0;
        fprintf(stderr, "sceIoIoctl: fd=%u PGD decryption active (logical size=%u, block=%u)\n",
                fd, sr_pgd_data_size(p), sr_pgd_block_size(p));
        return 0;
    }
    case 0x04100002:  /* set PGD offset: header is at file offset 0 for this title */
        return 0;
    case 0x04100010:  /* get PGD data size */
        return f->pgd ? sr_pgd_data_size(f->pgd) : f->size;
    default: {
        static uint32_t seen[16]; static int nseen = 0;
        int new_cmd = 1;
        for (int i = 0; i < nseen; i++) if (seen[i] == cmd) { new_cmd = 0; break; }
        if (new_cmd && nseen < 16) {
            seen[nseen++] = cmd;
            fprintf(stderr, "sceIoIoctl: UNIMPL cmd=0x%08x fd=%u in=0x%08x/%u out=0x%08x/%u\n",
                    cmd, fd, in, inlen, out, outlen);
        }
        return 0x80010086;  /* SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED */
    }
    }
}
static uint32_t h_IoClose(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) || !s_fds[fd].used)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    hle_fd_release(&s_fds[fd], s_fds[fd].kind == FD_KIND_STD);
    s_closed_res[fd] = 0;
    return 0;
}

static uint32_t h_IoDopen(CpuState *s) {
    char path[512]; guest_cstr(A0, path, sizeof(path));
    for (uint32_t i = 0; i < sizeof(s_dirfds) / sizeof(s_dirfds[0]); i++) {
        if (!s_dirfds[i].used) {
            DirFd *d = &s_dirfds[i];
            memset(d, 0, sizeof(*d));
            d->used = 1; d->index = 0;
            d->path = sr_asset_index_strdup(path);
            if (!d->path) { memset(d, 0, sizeof(*d)); return 0x80010014u; }
            if (_strnicmp(path, "disc0:", 6) == 0 || _strnicmp(path, "umd:", 4) == 0) {
                IsoDirEntry probe;
                if (iso_list(path, 0, &probe) < 0) {
                    free(d->path); memset(d, 0, sizeof(*d)); return 0x80010014u;
                }
                d->backend = 0;
            } else {
                char *hp = host_dir_path_alloc(path);
                wchar_t *root = NULL, *pattern = NULL;
                if (!hp || !sr_wide_path_alloc(hp, &root) ||
                    !sr_wide_join_alloc(root, L"*", &pattern)) {
                    free(hp); free(root); free(pattern); free(d->path); memset(d, 0, sizeof(*d));
                    return 0x80010014u;
                }
#ifdef _WIN32
                /* Generic VFS enumeration containment: the resolved host
                 * directory must open onto an object whose FINAL path lives
                 * under SR_FSDIR's canonical root. A pre-planted junction in
                 * place of (or above) the enumerated directory resolves to its
                 * target here and is refused before FindFirstFileW ever runs. */
                {
                    char *configured_fs = NULL;
                    int configured_fs_present = 0;
                    sr_utf8_env_alloc(L"SR_FSDIR", &configured_fs, &configured_fs_present);
                    const char *fs_dir = configured_fs_present && configured_fs[0] ? configured_fs : "fs";
                    wchar_t canonical_fs[MAX_PATH * 2];
                    int fs_ok = sr_vfs_canonical_root(fs_dir, canonical_fs,
                                                      sizeof(canonical_fs)/sizeof(wchar_t));
                    free(configured_fs);

                    HANDLE h_dir = CreateFileW(root, FILE_READ_ATTRIBUTES,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                               NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
                    if (h_dir == INVALID_HANDLE_VALUE || !fs_ok ||
                        !sr_vfs_handle_is_contained(h_dir, canonical_fs)) {
                        if (h_dir != INVALID_HANDLE_VALUE) CloseHandle(h_dir);
                        free(hp); free(root); free(pattern); free(d->path); memset(d, 0, sizeof(*d));
                        return 0x80010014u;
                    }
                    CloseHandle(h_dir);
                }
#endif
                d->find = FindFirstFileW(pattern, &d->data);
                DWORD first_error = d->find == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                free(hp); free(root); free(pattern);
                if (d->find == INVALID_HANDLE_VALUE) {
                    fprintf(stderr, "sceIoDopen: enumeration failed (error=%lu)\n", first_error);
                    free(d->path); memset(d, 0, sizeof(*d)); return 0x80010014u;
                }
                d->backend = 1; d->first = 1;
            }
            return 0x100u + i;
        }
    }
    return SCE_ERROR_KERNEL_TOO_MANY_OPEN_FILES;
}

static uint32_t h_IoDread(CpuState *s) {
    uint32_t fd = A0, de = A1;
    if (fd < 0x100u || fd >= 0x100u + sizeof(s_dirfds) / sizeof(s_dirfds[0]))
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    DirFd *d = &s_dirfds[fd - 0x100u];
    if (!d->used) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (!de) return 0x80010009u; /* preserve baseline behavior for null pointer */
    IsoDirEntry e;
    if (d->backend == 0) {
        int r = iso_list(d->path, d->index, &e);
        if (r <= 0) return r < 0 ? 0x80010005u : 0;
        d->index++;
    } else {
        for (;;) {
            if (!d->first && !FindNextFileW(d->find, &d->data)) {
                DWORD error = GetLastError();
                if (error == ERROR_NO_MORE_FILES) return 0;
                fprintf(stderr, "sceIoDread: enumeration failed (error=%lu)\n", error);
                return 0x80010005u;
            }
            d->first = 0;
            if (!(d->data.cFileName[0] == L'.' &&
                  (d->data.cFileName[1] == L'\0' ||
                   (d->data.cFileName[1] == L'.' && d->data.cFileName[2] == L'\0')))) break;
        }
        memset(&e, 0, sizeof(e));
        char *name = NULL;
        if (!sr_wide_to_utf8_alloc(d->data.cFileName, &name)) return 0x80010005u;
        if (strlen(name) >= sizeof(e.name)) {
            free(name);
            fprintf(stderr, "sceIoDread: directory entry name exceeds guest buffer\n");
            return 0x80010005u;
        }
        memcpy(e.name, name, strlen(name) + 1u);
        free(name);
        e.is_dir = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!e.is_dir && d->data.nFileSizeHigh != 0u) {
            fprintf(stderr, "sceIoDread: file exceeds guest size limit\n");
            return 0x80010005u;
        }
        e.size = d->data.nFileSizeLow;
    }
    for (uint32_t i = 0; i < 0x15cu; i++) MEM_W8(de + i, 0);
    MEM_W32(de + 0x00, (e.is_dir ? 0x1000u : 0x2000u) | 0x0124u);
    MEM_W32(de + 0x04, e.is_dir ? 0x0010u : 0x0020u);
    MEM_W32(de + 0x08, e.size); MEM_W32(de + 0x0c, 0);
    MEM_W32(de + 0x40, e.lba);
    for (uint32_t i = 0; i < sizeof(e.name) && e.name[i]; i++) MEM_W8(de + 0x58 + i, (uint8_t)e.name[i]);
    return 1;
}

static uint32_t h_IoDclose(CpuState *s) {
    uint32_t fd = A0;
    if (fd < 0x100u || fd >= 0x100u + sizeof(s_dirfds) / sizeof(s_dirfds[0])) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    DirFd *d = &s_dirfds[fd - 0x100u];
    if (!d->used) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (d->backend == 1 && d->find != NULL && d->find != INVALID_HANDLE_VALUE) FindClose(d->find);
    free(d->path);
    memset(d, 0, sizeof(*d)); return 0;
}
/* Async IO: the operation completes synchronously and its result is stashed per-fd for the
 * matching sceIoWaitAsync/PollAsync to return (the game streams data this way). */
static uint32_t h_IoOpenAsync(CpuState *s) {
    uint32_t fd = h_IoOpen(s);
    if (fd < 64) s_fds[fd].async_res = (int64_t)(int32_t)fd;   /* open result */
    return fd;
}
static uint32_t h_IoReadAsync(CpuState *s) {
    uint32_t fd = A0;
    uint32_t off = (fd < 64) ? s_fds[fd].off : 0;
    uint32_t n = h_IoRead(s);
    if (getenv("SR_IOLOG")) fprintf(stderr, "ReadAsync fd=%u dst=0x%x size=%u (file off was %u) -> %u; first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
        fd, A1, A2, off, n, MEM_R8(A1), MEM_R8(A1+1), MEM_R8(A1+2), MEM_R8(A1+3), MEM_R8(A1+4), MEM_R8(A1+5), MEM_R8(A1+6), MEM_R8(A1+7));
    if (fd < 64 && s_fds[fd].used) s_fds[fd].async_res = (int64_t)(uint64_t)n;
    return 0;
}
static uint32_t h_IoLseekAsync(CpuState *s) {
    uint32_t fd = A0;
    uint32_t pos = h_IoLseek32(s);
    if (fd < 64 && s_fds[fd].used) s_fds[fd].async_res = (int64_t)(uint64_t)pos;
    return 0;
}
/* Result of the most recent async close per fd slot, so the customary
 * sceIoCloseAsync -> sceIoWaitAsync sequence reads 0 (success), not -1, after the slot is freed. */
static uint32_t h_IoWaitAsync(CpuState *s) {
    uint32_t fd = A0, resp = A1;
    if (hle_log_on())
        fprintf(stderr, "HLE: IoWaitAsync fd=0x%x (from 0x%x)\n", fd, sched_current_uid());
    int64_t r = -1;
    if (fd < 64) r = s_fds[fd].used ? s_fds[fd].async_res : s_closed_res[fd];
    if (resp) { MEM_W32(resp, (uint32_t)r); MEM_W32(resp + 4, (uint32_t)((uint64_t)r >> 32)); }
    return 0;   /* completed */
}
static uint32_t h_IoWaitAsyncCB(CpuState *s) {
    if (sr_thread_has_pending_callbacks(sched_current_uid())) {
        sr_thread_dispatch_callbacks();
    }
    return h_IoWaitAsync(s);
}
static uint32_t h_IoCloseAsync(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) || !s_fds[fd].used)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    hle_fd_release(&s_fds[fd], s_fds[fd].kind == FD_KIND_STD);
    s_closed_res[fd] = 0;
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* The focused native HLE harness exposes the small IoFileMgr slice under its
 * production handlers without widening the ThreadMan-only registry used by
 * that executable.  The wrappers and identity probe are test-build-only. */
uint32_t sr_hle_test_io_open(CpuState *s) { return h_IoOpen(s); }
uint32_t sr_hle_test_io_read(CpuState *s) { return h_IoRead(s); }
uint32_t sr_hle_test_io_write(CpuState *s) { return h_IoWrite(s); }
uint32_t sr_hle_test_io_lseek(CpuState *s) { return h_IoLseek(s); }
uint32_t sr_hle_test_io_lseek32(CpuState *s) { return h_IoLseek32(s); }
uint32_t sr_hle_test_io_dopen(CpuState *s) { return h_IoDopen(s); }
uint32_t sr_hle_test_io_dread(CpuState *s) { return h_IoDread(s); }
uint32_t sr_hle_test_io_dclose(CpuState *s) { return h_IoDclose(s); }
uint32_t sr_hle_test_io_ioctl(CpuState *s) { return h_IoIoctl(s); }
uint32_t sr_hle_test_io_close(CpuState *s) { return h_IoClose(s); }
uint32_t sr_hle_test_io_open_async(CpuState *s) { return h_IoOpenAsync(s); }
uint32_t sr_hle_test_io_close_async(CpuState *s) { return h_IoCloseAsync(s); }
int sr_hle_test_fd_kind(uint32_t fd) {
    return fd < (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) ? (int)s_fds[fd].kind : -1;
}
#endif
static uint32_t h_IoGetstat(CpuState *s) {
    /* a0=path, a1=SceIoStat*. SceIoStat layout: mode(+0), attr(+4), size(+8,64-bit),
     * ctime(+0x10), atime(+0x20), mtime(+0x30), st_private[6](+0x40). For UMD files PPSSPP
     * fills st_private[0] (+0x40) with the file's starting LBN; the game reads that to build
     * a raw "sce_lbn0x<LBN>" path. Omitting it made the game read garbage and fetch the wrong
     * sector (e.g. REGFILE.CDI at LBN 0x5f20 was read as 0x80). */
    char path[256]; guest_cstr(A0, path, sizeof(path));
    uint32_t lba, size, st = A1;
    if (iso_lookup(path, &lba, &size) != 0) {
        /* Not on the ISO -- try the extracted-XB data-root (graphical/text data the game
         * expects to read from paths like "data/menu/text/<X>.to", which are packed inside
         * XB archives on the real ISO; dev workflow extracts them onto a host tree). */
        const SrAssetIndexEntry *entry = host_data_lookup(path);
        if (entry) {
            FILE *probe = sr_fopen_utf8(entry->host, L"rb");
            uint32_t actual_size = 0;
            if (!probe || !sr_stream_size_u32(probe, &actual_size)) {
                if (probe) fclose(probe);
                fprintf(stderr, "host_data: indexed asset stat failed for %s (errno=%d)\n",
                        path, errno);
                return 0x80010002;
            }
            fclose(probe);
            if (getenv("SR_STATLOG"))
                fprintf(stderr, "Getstat(%s) -> data-root %s size=0x%x\n", path,
                        entry->host, (unsigned)actual_size);
            for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
            MEM_W32(st + 0,  0x2000 | 0x0124);   /* mode: regular file, r-x */
            MEM_W32(st + 4,  0x0004 | 0x0001);   /* attr: file */
            MEM_W32(st + 8,  actual_size); /* size low (SceOff is 64-bit at +8) */
            MEM_W32(st + 0x40, 0);                /* st_private[0]: no LBN (host-backed) */
            return 0;
        }
        if (getenv("SR_STATLOG")) fprintf(stderr, "Getstat(%s) -> NOT FOUND\n", path);
        return 0x80010002;
    }
    if (getenv("SR_STATLOG")) fprintf(stderr, "Getstat(%s) -> lba=0x%x size=0x%x\n", path, lba, size);
    for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
    MEM_W32(st + 0, 0x2000 | 0x0124);          /* mode: regular file, r-x */
    MEM_W32(st + 4, 0x0004 | 0x0001);          /* attr: file */
    MEM_W32(st + 8, size);                      /* size low (SceOff is 64-bit at +8) */
    MEM_W32(st + 0x40, iso_physical_lba(lba));  /* st_private[0]: physical UMD start LBN */
    return 0;
}

/* ---- audio / control / display / GE / SAS: functional stubs ----
 * These return success and neutral data so the boot reaches and runs its main loop without an
 * actual audio device or GPU. Real rendering (sceGe display lists) and audio are later work;
 * here the calls must not block forever and must hand back valid-shaped results. */

/* sceAudio: a small channel table. The *Blocking output calls must block until the buffer is
 * consumed by the (virtual) audio hardware; that block is what lets the rest of the game run,
 * so a no-op return makes the audio thread monopolise the CPU. Here it yields the thread for
 * the buffer's duration AND forwards the samples to the host waveOut backend (audio.c). */
extern void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR);
/* Slots 0..7 are the regular hardware channels. Slot 8 models the separate
 * sceAudioOutput2 channel, which has its own reservation and queue. */
static int s_audio_ch[9], s_audio_fmt[9];   /* regular fmt: 0=stereo, 0x10=mono */
static uint32_t s_audio_len[9];

/* Public contract vocabulary pinned to PSPSDK src/audio/pspaudio.h at
 * 314b2083f2e1eaf145fc5de342736336fe1f0148 and PSPAutotests
 * tests/audio/sceaudio/{reserve,datalen}.{c,expected} at
 * ea71108f00933712c4662276261b39cd42249b1e. Those sources are corroborative
 * inputs; the executable checks in this tree are host evidence. */
#define SCE_AUDIO_ERROR_NOT_INITIALIZED 0x80260001u
#define SCE_AUDIO_ERROR_OUTPUT_BUSY     0x80260002u
#define SCE_AUDIO_ERROR_INVALID_CH      0x80260003u
#define SCE_AUDIO_ERROR_NOT_FOUND       0x80260005u
#define SCE_AUDIO_ERROR_INVALID_SIZE    0x80260006u
#define SCE_AUDIO_ERROR_INVALID_FORMAT  0x80260007u
#define SCE_AUDIO_ERROR_NOT_RESERVED    0x80260008u
#define SCE_AUDIO_ERROR_INVALID_VOL     0x8026000bu
#define PSP_AUDIO_FORMAT_STEREO         0u
#define PSP_AUDIO_FORMAT_MONO           0x10u
#define PSP_AUDIO_SAMPLE_MIN            64u
#define PSP_AUDIO_SAMPLE_MAX            65472u

static int audio_regular_sample_count_valid(uint32_t frames) {
    return frames >= PSP_AUDIO_SAMPLE_MIN && frames <= PSP_AUDIO_SAMPLE_MAX &&
           (frames & 63u) == 0u;
}

static uint32_t h_AudioChReserve(CpuState *s) {
    int32_t ch = (int32_t)A0;
    if (ch < 0) {
        ch = -1;
        for (int32_t i = 0; i < 8; i++) {
            if (!s_audio_ch[i]) { ch = i; break; }
        }
        if (ch < 0) return SCE_AUDIO_ERROR_NOT_FOUND;
    }
    if (ch >= 8) return SCE_AUDIO_ERROR_INVALID_CH;
    if (!audio_regular_sample_count_valid(A1)) return SCE_AUDIO_ERROR_INVALID_SIZE;
    if (A2 != PSP_AUDIO_FORMAT_STEREO && A2 != PSP_AUDIO_FORMAT_MONO)
        return SCE_AUDIO_ERROR_INVALID_FORMAT;
    if (s_audio_ch[ch]) return SCE_AUDIO_ERROR_INVALID_CH;
    s_audio_ch[ch] = 1;
    s_audio_len[ch] = A1;
    s_audio_fmt[ch] = (int)A2;
    return (uint32_t)ch;
}
static uint32_t h_AudioChRelease(CpuState *s) { if (A0 < 8) s_audio_ch[A0] = 0; return 0; }
static uint32_t h_AudioSetChannelDataLen(CpuState *s) {
    if (A0 >= 8u) return SCE_AUDIO_ERROR_INVALID_CH;
    if (!s_audio_ch[A0]) return SCE_AUDIO_ERROR_NOT_INITIALIZED;
    if (!audio_regular_sample_count_valid(A1)) return SCE_AUDIO_ERROR_INVALID_SIZE;
    s_audio_len[A0] = A1;
    return 0;
}
/* Read a guest sample buffer, expand mono to stereo, hand to the backend, then block until
 * the host queue is back down to ~one buffer of lead (real sceAudio blocking semantics).
 * Pacing against the queue self-corrects: a late thread returns immediately and catches up,
 * an early one sleeps the difference. The old open-loop sleep of the buffer's duration ran
 * slightly slower than the device every iteration (sleep + decode time), so streams drifted
 * behind (voice lagging subtitles) and underran (crackle). */
extern int sr_audio_queued(int ch);
static uint32_t s_audio_delay_carry[9];

/* Convert a frame delta to microseconds without discarding the 44.1 kHz fractional carry.
 * The old integer division lost up to almost one microsecond on every wake and could also
 * produce a zero-delay scheduler spin for a small queue lead. */
static uint32_t audio_frames_to_us(uint32_t ch, uint32_t frames) {
    uint32_t slot = ch < 9u ? ch : 8u;
    uint64_t scaled = (uint64_t)frames * 1000000u + s_audio_delay_carry[slot];
    uint32_t us = (uint32_t)(scaled / 44100u);
    s_audio_delay_carry[slot] = (uint32_t)(scaled % 44100u);
    return us ? us : 1u;
}

/* SR_AUDIOSTAT: buffer identity per channel. Proving the buffer sceAudioOutput2
 * receives is the same one __sceSasCore wrote is what links the two stages; the
 * addresses are guest-side and the game double-buffers, so record a small set. */
static uint32_t g_audio_bufs[9][4];
static int      g_audio_nbufs[9];
static void audio_note_buf(uint32_t ch, uint32_t buf) {
    if (ch > 8u || !buf) return;
    for (int i = 0; i < g_audio_nbufs[ch]; i++) if (g_audio_bufs[ch][i] == buf) return;
    if (g_audio_nbufs[ch] < 4) g_audio_bufs[ch][g_audio_nbufs[ch]++] = buf;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Read-only regular-audio state for production-dispatch contract regressions.
 * The reset is test-build-only because sr_hle_init() is process-global while the
 * executable harness deliberately runs many isolated synthetic fixtures. */
void sr_hle_test_audio_reset(void) {
    memset(s_audio_ch, 0, sizeof(s_audio_ch));
    memset(s_audio_fmt, 0, sizeof(s_audio_fmt));
    memset(s_audio_len, 0, sizeof(s_audio_len));
    memset(s_audio_delay_carry, 0, sizeof(s_audio_delay_carry));
    memset(g_audio_bufs, 0, sizeof(g_audio_bufs));
    memset(g_audio_nbufs, 0, sizeof(g_audio_nbufs));
}

int sr_hle_test_audio_state(uint32_t ch, int *reserved, uint32_t *frames, int *format) {
    if (ch >= 8u) return 0;
    if (reserved) *reserved = s_audio_ch[ch];
    if (frames)   *frames   = s_audio_len[ch];
    if (format)   *format   = s_audio_fmt[ch];
    return 1;
}
#endif

static uint32_t audio_output(CpuState *s, uint32_t ch, uint32_t buf, int voll, int volr) {
    (void)s;
    uint32_t n = ch < 9 ? s_audio_len[ch] : 1024;
    int mono = ch < 9 && s_audio_fmt[ch] == (int)PSP_AUDIO_FORMAT_MONO;
    uint32_t bytes = 0;

    /* Validate the complete source span before telemetry, scalar reads, backend
     * submission, host-queue observation, or scheduler delay. A null buffer is
     * intentionally preserved: public PSPAutotests show it is accepted by the
     * regular blocking API, and this path has never dereferenced it. */
    if (buf && n > 0u &&
        (!sr_size_mul_ok(n, mono ? 2u : 4u, &bytes) ||
         !sr_guest_span_readable(buf, bytes)))
        return 0x80000103u; /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */

    if (audio_stat_on()) audio_note_buf(ch, buf);
    if (buf && n > 0 && n <= 65536) {
        static int16_t lr[65536 * 2];
        for (uint32_t i = 0; i < n; i++) {
            if (mono) { int16_t v = (int16_t)MEM_R16(buf + i * 2); lr[i*2] = v; lr[i*2+1] = v; }
            else { lr[i*2] = (int16_t)MEM_R16(buf + i * 4); lr[i*2+1] = (int16_t)MEM_R16(buf + i * 4 + 2); }
        }
        sr_audio_push((int)ch, lr, (int)n, voll, volr);
    }
    int q = sr_audio_queued((int)ch);
    if (q < 0 || n == 0) {                 /* no host audio: open-loop pacing as before */
        sched_delay_current(n ? audio_frames_to_us(ch, n) : 1000u);
        return n;
    }
    /* sr_audio_queued() is signed only so the backend can report -1. Once that
     * sentinel is excluded, keep the frame comparison/subtraction unsigned;
     * never narrow the guest-derived frame count into the signed queue domain.
     * This form is defense in depth, not a repaired defect: with the reserve and
     * SetChannelDataLen size contracts above, n can no longer exceed INT_MAX, so
     * no production dispatch distinguishes it and it has no failing-before. */
    while ((q = sr_audio_queued((int)ch)) >= 0 && (uint32_t)q > n)
        sched_delay_current(audio_frames_to_us(ch, (uint32_t)q - n));
    return n;
}
static uint32_t h_AudioOutputBlocking(CpuState *s) {
    /* sceAudioOutputBlocking(ch, vol, buf) */
    return audio_output(s, A0, A2, (int)(A1 & 0xFFFF), (int)(A1 & 0xFFFF));
}
static uint32_t h_AudioOutputPannedBlocking(CpuState *s) {
    /* sceAudioOutputPannedBlocking(ch, leftvol, rightvol, buf) */
    return audio_output(s, A0, A3, (int)(A1 & 0xFFFF), (int)(A2 & 0xFFFF));
}
static uint32_t h_AudioRestLen(CpuState *s) { (void)s; return 0; }         /* never backed up */

/* Keep the executable audio contract harness on the exact production NID
 * mapping without exposing the separate Output2 family to that focused test. */
static void hle_register_regular_audio_handlers(void) {
    sr_hle_register(0x5ec81c55, "sceAudioChReserve", h_AudioChReserve);
    sr_hle_register(0x6fc46853, "sceAudioChRelease", h_AudioChRelease);
    sr_hle_register(0x136caf51, "sceAudioOutputBlocking", h_AudioOutputBlocking);
    sr_hle_register(0x13f592bc, "sceAudioOutputPannedBlocking", h_AudioOutputPannedBlocking);
    sr_hle_register(0xe2d56b2d, "sceAudioOutputPanned", h_AudioOutputPannedBlocking);
    sr_hle_register(0x95fd0c2d, "sceAudioChangeChannelConfig", h_ok);
    sr_hle_register(0xb011922f, "sceAudioGetChannelRestLength", h_AudioRestLen);
    sr_hle_register(0xb7e1d8e7, "sceAudioChangeChannelVolume", h_ok);
    sr_hle_register(0xcb2e439e, "sceAudioSetChannelDataLen", h_AudioSetChannelDataLen);
}

#define AUDIO_OUTPUT2_CHANNEL 8u

static uint32_t h_AudioOutput2Reserve(CpuState *s) {
    uint32_t samples = A0;
    if (s_audio_ch[AUDIO_OUTPUT2_CHANNEL]) return SCE_AUDIO_ERROR_OUTPUT_BUSY;
    if (samples < 17u || samples > 4111u) return SCE_AUDIO_ERROR_INVALID_SIZE;
    s_audio_ch[AUDIO_OUTPUT2_CHANNEL] = 1;
    s_audio_fmt[AUDIO_OUTPUT2_CHANNEL] = 0;
    s_audio_len[AUDIO_OUTPUT2_CHANNEL] = samples;
    return 0;
}
static uint32_t h_AudioOutput2Release(CpuState *s) {
    (void)s;
    if (!s_audio_ch[AUDIO_OUTPUT2_CHANNEL]) return SCE_AUDIO_ERROR_NOT_RESERVED;
    s_audio_ch[AUDIO_OUTPUT2_CHANNEL] = 0;
    s_audio_len[AUDIO_OUTPUT2_CHANNEL] = 0;
    return 0;
}
static uint32_t h_AudioOutput2ChangeLength(CpuState *s) {
    if (!s_audio_ch[AUDIO_OUTPUT2_CHANNEL]) return SCE_AUDIO_ERROR_NOT_RESERVED;
    if (A0 < 17u || A0 > 4111u) return SCE_AUDIO_ERROR_INVALID_SIZE;
    s_audio_len[AUDIO_OUTPUT2_CHANNEL] = A0;
    return 0;
}
static uint32_t h_AudioOutput2Blocking(CpuState *s) {
    if (!s_audio_ch[AUDIO_OUTPUT2_CHANNEL]) return SCE_AUDIO_ERROR_NOT_RESERVED;
    if (A0 > 0x8000u) return SCE_AUDIO_ERROR_INVALID_VOL;
    (void)audio_output(s, AUDIO_OUTPUT2_CHANNEL, A1, (int)A0, (int)A0);
    return 0;
}
static uint32_t h_AudioOutput2Rest(CpuState *s) {
    (void)s;
    if (!s_audio_ch[AUDIO_OUTPUT2_CHANNEL]) return SCE_AUDIO_ERROR_NOT_RESERVED;
    int queued = sr_audio_queued(AUDIO_OUTPUT2_CHANNEL);
    return queued > 0 ? (uint32_t)queued : 0u;
}

/* SR_CALLCOUNT instrumentation: per-NID call tallies, dumped at the capture point. */
static struct { uint32_t nid; const char *nm; unsigned long n; } g_cc[512];
static int g_ncc = 0, g_callcount = 0;
static void sr_dump_calls(void) {
    if (!g_callcount) return;
    for (int a = 0; a < g_ncc; a++) for (int b = a + 1; b < g_ncc; b++)
        if (g_cc[b].n > g_cc[a].n) { __typeof__(g_cc[0]) t = g_cc[a]; g_cc[a] = g_cc[b]; g_cc[b] = t; }
    /* SR_CALLCOUNT_ALL: dump every tallied NID rather than the top 18. A family
     * that is ABSENT from this list was never called, which is the discriminator
     * a "does the title use this API at all" question needs; a truncated list
     * cannot distinguish "not called" from "not in the top 18". */
    int limit = getenv("SR_CALLCOUNT_ALL") ? g_ncc : (g_ncc < 18 ? g_ncc : 18);
    fprintf(stderr, "--- HLE calls (%d of %d distinct NIDs) ---\n", limit, g_ncc);
    for (int a = 0; a < limit; a++) fprintf(stderr, "  %-32s 0x%08x  %lu\n", g_cc[a].nm, g_cc[a].nid, g_cc[a].n);
}
/* PSP controller ring buffer, modelled on PPSSPP (Core/HLE/sceCtrl.cpp): one sample is latched
 * per VBLANK into a 64-entry ring; sceCtrlReadBufferPositive returns the samples accumulated
 * since the last read (real per-frame history), not a flat copy of the current state. Games do
 * edge detection across these samples, so a flat fill makes a button look permanently held and a
 * press is never seen -- which is why the title/attract loop never advanced on START. */
#define CTRL_RING 64
/* One guest SceCtrlData record: timestamp, button field, two analog axes, then
 * reserved bytes this runtime does not populate. */
#define CTRL_SAMPLE_BYTES 16u
/* A request larger than the sample ring is refused rather than clamped.
 * CORROBORATIVE_ONLY: PPSSPP Core/HLE/sceCtrl.cpp __CtrlReadBuffer() returns
 * SCE_KERNEL_ERROR_INVALID_SIZE for nBufs > NUM_CTRL_BUFFERS. Not measured here. */
#define SCE_CTRL_ERROR_INVALID_SIZE 0x80000104u
/* `ts` is stamped when the sample is latched, not when it is read. SceCtrlData.TimeStamp
 * contains the low 32 bits of the guest microsecond system clock at latch time
 * (corroborated by uOFW sceKernelGetSystemTimeLow, PSPAutotests ctrl/vblank, and emulator consensus). */
typedef struct { uint32_t btn; uint32_t ts; uint8_t lx, ly; } CtrlSample;
static CtrlSample s_ctrl_ring[CTRL_RING] = { [0 ... CTRL_RING-1] = { 0, 0, 128, 128 } };
static int s_ctrl_w = 1, s_ctrl_r = 0;   /* start with one sample available */

/* ---- state-qualified acceptance routes (issue #64) --------------------------------
 *
 * A pad script written as "press CROSS at vblank 8600" is a bet that the guest is on the
 * screen its author saw when they recorded it. Boot and transition durations vary between
 * otherwise identical replays, so that bet loses: seven replays of one script from one
 * restored save baseline reached two different menu depths, and the two divergent runs
 * spent their whole budget in Story Mode instead of the intended Exhibition match. Both
 * still reported a complete run, because "reached vblank N" was the only thing anything
 * checked. Elapsed vblanks are not state.
 *
 * A route program replaces the bet with a measurement. Steps run in sequence, and a step
 * that names a screen does not complete until that screen is observed:
 *
 *   SIGGRID <cols> <rows>        signature grid (default 12x8); must precede CHECKPOINT
 *   SAMPLE_EVERY <vblanks>       observation cadence (default 20)
 *   TOLERANCE <n>                default per-checkpoint match tolerance (default 12)
 *   CHECKPOINT <NAME> [tol=<n>] <hex>
 *                                a screen signature; repeat NAME for alternates
 *   WAIT <NAME> <timeout>        block until NAME is observed; fail loudly on timeout
 *   EXPECT <NAME>                assert NAME is on screen right now; fail loudly if not
 *   PRESS <hexmask> <width>      hold mask for width vblanks
 *   DELAY <n>                    advance n vblanks (input cadence within one screen)
 *   END                          route complete
 *
 * "Observed" means a coarse signature of the presented framebuffer: the frame is divided
 * into cols x rows cells and each cell contributes its mean R, G and B. A screen matches
 * when the mean absolute difference against a recorded signature is within tolerance.
 * This is a private-title acceptance signal, not a PSP oracle: it proves which screen the
 * emulated guest reached, nothing about hardware. It is used because the alternatives are
 * worse -- guest menu-state addresses would be exactly the title-address patching this
 * project forbids, and no existing runtime event distinguishes a menu from its submenu.
 *
 * Failure is loud and terminal: ROUTE_FAIL on stderr and exit 86, so a run that reached
 * the wrong screen can never be archived as a successful route. SR_ROUTE_NO_EXIT keeps
 * the process alive for the executable regression tests (presence-based, like the other
 * legacy SR_* switches); SR_ROUTE_LEARN prints every sampled signature so a new checkpoint
 * can be recorded from a route the author has visually identified.
 *
 * Signatures are derived from retail frames and therefore belong in the private route
 * file beside the rest of the run inputs; nothing here writes one into the repository.
 *
 * A file with no keyword lines keeps the original "frame hexmask width" behaviour exactly.
 */
#define ROUTE_MAX_CELLS  192          /* 16x12 */
#define ROUTE_SIG_MAX    (ROUTE_MAX_CELLS * 3)
#define ROUTE_MAX_CP     32
#define ROUTE_MAX_ALT    4
#define ROUTE_MIN_ACTIVE_BYTES 12
#define ROUTE_MAX_STEPS  256
#define ROUTE_MAX_LEGACY 256
#define ROUTE_NAME_MAX   32
#define ROUTE_FAIL_EXIT  86

enum { ROUTE_OP_WAIT = 1, ROUTE_OP_EXPECT, ROUTE_OP_PRESS, ROUTE_OP_DELAY, ROUTE_OP_UNTIL,
       ROUTE_OP_WHILE, ROUTE_OP_END };
enum { ROUTE_OFF = 0, ROUTE_LEGACY, ROUTE_RUNNING, ROUTE_DONE, ROUTE_FAILED };

/* One named screen. Parts of a screen legitimately vary between otherwise identical
 * replays -- HST redraws its menus over a club backdrop that is not the same every run --
 * so a whole-frame comparison rejects the right screen. Recording the same screen twice
 * under different variable content solves this without a hand-authored region mask: bytes
 * that disagree between the alternates carry the variation, not the identity, and are
 * dropped from the comparison. Measured on the two menu backdrops seen here that leaves
 * about half the frame informative, and the Main Menu still sits ~8x closer to itself than
 * to the submenu it was being confused with. A checkpoint recorded once compares whole. */
typedef struct {
    char    name[ROUTE_NAME_MAX];
    int     tol;
    int     nsig;
    int     nactive;
    uint8_t sig[ROUTE_MAX_ALT][ROUTE_SIG_MAX];
    uint8_t active[ROUTE_SIG_MAX];   /* 1 = this byte takes part in the comparison */
} RouteCheckpoint;

typedef struct {
    int      op;
    char     name[ROUTE_NAME_MAX];   /* WAIT / EXPECT / PRESS_UNTIL */
    uint32_t a, b, c, d;             /* PRESS: mask,width   DELAY/WAIT: vblanks
                                      * PRESS_UNTIL: mask,width,period,timeout */
    int      line;                   /* source line, for diagnostics */
} RouteStep;

static RouteCheckpoint s_route_cp[ROUTE_MAX_CP];
static int      s_route_ncp;
static RouteStep s_route_prog[ROUTE_MAX_STEPS];
static int      s_route_nsteps;
static int      s_route_pc;
static uint32_t s_route_step_start;
static int      s_route_step_started;
static int      s_route_state = ROUTE_OFF;
static int      s_route_cols = 12, s_route_rows = 8;
static int      s_route_tol = 12;
static int      s_route_sample_every = 20;
static int      s_route_learn;
static uint32_t s_route_keys;
/* Elapsed-cadence bookkeeping for the route observer (#109): the delivered
 * VCOUNT of the last due sampling attempt and whether one has happened since
 * the route (re)started. Reset by sr_route_reset and sr_route_load so a fresh
 * route always gets its first pending observation immediately. */
static uint32_t s_route_last_attempt;
static int      s_route_have_attempt;
static struct { uint32_t f, mask, w; } s_route_legacy[ROUTE_MAX_LEGACY];
static int      s_route_nlegacy;
static int      s_route_loaded;

static int route_sig_bytes(void) { return s_route_cols * s_route_rows * 3; }

/* A route that cannot be trusted must not be allowed to look like one that can. Every
 * failure path lands here: it names the step, the vblank and what was actually on screen,
 * then terminates with a distinct status so the manager's verdict cannot record the run
 * as complete. Tests set SR_ROUTE_NO_EXIT to inspect the terminal state instead. */
static void route_fail(const char *fmt, ...) {
    va_list ap;
    s_route_state = ROUTE_FAILED;
    fputs("ROUTE_FAIL: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    if (!getenv("SR_ROUTE_NO_EXIT")) _Exit(ROUTE_FAIL_EXIT);
}

static int route_hex_nib(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Exactly `want` bytes of hex and nothing else: a truncated signature would silently
 * compare only its prefix and match screens it has never seen. */
static int route_parse_sig(const char *hex, uint8_t *out, int want) {
    int n = 0;
    while (hex[0] && hex[1] && n < want) {
        int hi = route_hex_nib((unsigned char)hex[0]);
        int lo = route_hex_nib((unsigned char)hex[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return (n == want && *hex == '\0') ? 0 : -1;
}

/* Mean absolute difference over the checkpoint's informative bytes, taking the closest
 * recorded alternate. */
static int route_distance(const RouteCheckpoint *cp, const uint8_t *sig) {
    int n = route_sig_bytes(), best = 255;
    if (cp->nactive <= 0) return 255;
    for (int k = 0; k < cp->nsig; k++) {
        long sum = 0;
        for (int i = 0; i < n; i++) {
            if (!cp->active[i]) continue;
            int d = (int)cp->sig[k][i] - (int)sig[i];
            sum += d < 0 ? -d : d;
        }
        int mean = (int)(sum / cp->nactive);
        if (mean < best) best = mean;
    }
    return best;
}

/* Closest defined checkpoint to what is on screen. A failure that can say "this looks
 * like SINGLE_PLAYER_MENU (d=3)" is a diagnosis; "not MAIN_MENU" is only a complaint. */
static int route_best_match(const uint8_t *sig, int *out_d) {
    int best = -1, best_d = 255;
    for (int i = 0; i < s_route_ncp; i++) {
        int d = route_distance(&s_route_cp[i], sig);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (out_d) *out_d = best_d;
    return best;
}

/* What the run last actually saw. A failure diagnosed only from the vblank it fired on is
 * usually blank -- observations are periodic and a timeout rarely lands on one -- so the
 * last observation is kept and named instead. */
static char s_route_seen[96] = "no screen was observed at all";
static int  s_route_while_seen;   /* PRESS_WHILE has seen its screen at least once */

static const char *route_seen_desc(void) { return s_route_seen; }

static int route_find(const char *name) {
    for (int i = 0; i < s_route_ncp; i++)
        if (strcmp(s_route_cp[i].name, name) == 0) return i;
    return -1;
}

static int route_matches(const char *name, const uint8_t *sig, int *out_d) {
    int i = route_find(name);
    if (i < 0) { if (out_d) *out_d = 255; return 0; }
    int d = route_distance(&s_route_cp[i], sig);
    if (out_d) *out_d = d;
    return d <= s_route_cp[i].tol;
}

/* Decide which bytes of a checkpoint carry its identity. With one recorded signature that
 * is the whole frame; with several it is the bytes they agree on. */
static int route_finalize_checkpoints(const char *path) {
    int n = route_sig_bytes();
    for (int c = 0; c < s_route_ncp; c++) {
        RouteCheckpoint *cp = &s_route_cp[c];
        cp->nactive = 0;
        for (int i = 0; i < n; i++) {
            int lo = 255, hi = 0;
            for (int k = 0; k < cp->nsig; k++) {
                int v = cp->sig[k][i];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            cp->active[i] = (uint8_t)(hi - lo <= cp->tol);
            cp->nactive += cp->active[i];
        }
        if (cp->nactive < ROUTE_MIN_ACTIVE_BYTES) {
            fprintf(stderr,
                    "ROUTE_PARSE: %s: checkpoint '%s' has only %d of %d bytes in common across "
                    "its %d recorded signatures; there is not enough left to identify a screen\n",
                    path, cp->name, cp->nactive, n, cp->nsig);
            return -1;
        }
        if (cp->nsig > 1)
            fprintf(stderr, "ROUTE: checkpoint %s uses %d of %d bytes (%d signatures, tol=%d)\n",
                    cp->name, cp->nactive, n, cp->nsig, cp->tol);
    }
    /* The real requirement is not how much of a frame survives masking -- a title screen is
     * mostly backdrop and legitimately keeps a fifth of it -- but that the checkpoints can
     * still be told apart. Two that match each other make every assertion between them
     * vacuous, and a route that cannot fail is worse than no route, so refuse it here rather
     * than let it report success. */
    for (int a = 0; a < s_route_ncp; a++)
        for (int b = 0; b < s_route_ncp; b++) {
            if (a == b) continue;
            for (int k = 0; k < s_route_cp[b].nsig; k++) {
                int d = route_distance(&s_route_cp[a], s_route_cp[b].sig[k]);
                if (d <= s_route_cp[a].tol) {
                    fprintf(stderr,
                            "ROUTE_PARSE: %s: checkpoint '%s' also matches a recording of '%s' "
                            "(d=%d, tol=%d); the route could not tell them apart\n",
                            path, s_route_cp[a].name, s_route_cp[b].name, d, s_route_cp[a].tol);
                    return -1;
                }
            }
        }
    return 0;
}

/* Route files are authored by hand beside the private run inputs, so every parse error is
 * reported with its line and refuses the route rather than running a partial program. */
static int route_parse_line(char *line, int lineno, const char *path) {
    char *tok = strtok(line, " \t\r\n");
    if (!tok || tok[0] == '#') return 0;

    if (tok[0] >= '0' && tok[0] <= '9') {
        if (s_route_nsteps > 0 || s_route_ncp > 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: bare frame line inside a route program\n", path, lineno);
            return -1;
        }
        if (s_route_nlegacy >= ROUTE_MAX_LEGACY) return 0;
        char *m = strtok(NULL, " \t\r\n");
        char *w = strtok(NULL, " \t\r\n");
        if (!m || !w) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: expected '<frame> <hexmask> <width>'\n", path, lineno);
            return -1;
        }
        s_route_legacy[s_route_nlegacy].f    = (uint32_t)strtoul(tok, NULL, 10);
        s_route_legacy[s_route_nlegacy].mask = (uint32_t)strtoul(m, NULL, 16);
        s_route_legacy[s_route_nlegacy].w    = (uint32_t)strtoul(w, NULL, 10);
        s_route_nlegacy++;
        return 0;
    }

    if (s_route_nlegacy > 0) {
        fprintf(stderr, "ROUTE_PARSE: %s:%d: route program mixed with bare frame lines\n", path, lineno);
        return -1;
    }

    if (strcmp(tok, "SIGGRID") == 0) {
        char *c = strtok(NULL, " \t\r\n"), *r = strtok(NULL, " \t\r\n");
        if (!c || !r) { fprintf(stderr, "ROUTE_PARSE: %s:%d: SIGGRID <cols> <rows>\n", path, lineno); return -1; }
        int cols = atoi(c), rows = atoi(r);
        if (cols < 1 || rows < 1 || cols * rows > ROUTE_MAX_CELLS) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: SIGGRID %dx%d out of range (cols*rows <= %d)\n",
                    path, lineno, cols, rows, ROUTE_MAX_CELLS);
            return -1;
        }
        if (s_route_ncp > 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: SIGGRID must precede every CHECKPOINT\n", path, lineno);
            return -1;
        }
        s_route_cols = cols; s_route_rows = rows;
        return 0;
    }
    if (strcmp(tok, "SAMPLE_EVERY") == 0 || strcmp(tok, "TOLERANCE") == 0) {
        char *v = strtok(NULL, " \t\r\n");
        int n = v ? atoi(v) : -1;
        if (n < 1) { fprintf(stderr, "ROUTE_PARSE: %s:%d: %s <n>, n >= 1\n", path, lineno, tok); return -1; }
        if (tok[0] == 'S') { s_route_sample_every = n; return 0; }
        /* A checkpoint takes the tolerance in force when it is parsed, and its tolerance
         * also decides which bytes are informative. A later TOLERANCE would silently mean
         * something different for the checkpoints above it than the file appears to say. */
        if (s_route_ncp > 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: TOLERANCE must precede every CHECKPOINT\n", path, lineno);
            return -1;
        }
        s_route_tol = n;
        return 0;
    }
    if (strcmp(tok, "CHECKPOINT") == 0) {
        char *name = strtok(NULL, " \t\r\n");
        char *next = name ? strtok(NULL, " \t\r\n") : NULL;
        int tol = s_route_tol;
        if (!name || !next) { fprintf(stderr, "ROUTE_PARSE: %s:%d: CHECKPOINT <NAME> [tol=<n>] <hex>\n", path, lineno); return -1; }
        if (strncmp(next, "tol=", 4) == 0) {
            tol = atoi(next + 4);
            next = strtok(NULL, " \t\r\n");
            if (tol < 0 || !next) { fprintf(stderr, "ROUTE_PARSE: %s:%d: bad tol= or missing signature\n", path, lineno); return -1; }
        }
        if (strlen(name) >= ROUTE_NAME_MAX) { fprintf(stderr, "ROUTE_PARSE: %s:%d: checkpoint name too long\n", path, lineno); return -1; }
        int idx = route_find(name);
        if (idx < 0) {
            if (s_route_ncp >= ROUTE_MAX_CP) { fprintf(stderr, "ROUTE_PARSE: %s:%d: more than %d checkpoints\n", path, lineno, ROUTE_MAX_CP); return -1; }
            idx = s_route_ncp++;
            memset(&s_route_cp[idx], 0, sizeof s_route_cp[idx]);
            snprintf(s_route_cp[idx].name, ROUTE_NAME_MAX, "%s", name);
        }
        if (s_route_cp[idx].nsig >= ROUTE_MAX_ALT) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: more than %d signatures for '%s'\n", path, lineno, ROUTE_MAX_ALT, name);
            return -1;
        }
        if (route_parse_sig(next, s_route_cp[idx].sig[s_route_cp[idx].nsig], route_sig_bytes()) != 0) {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: signature must be exactly %d hex bytes for a %dx%d grid\n",
                    path, lineno, route_sig_bytes(), s_route_cols, s_route_rows);
            return -1;
        }
        s_route_cp[idx].nsig++;
        s_route_cp[idx].tol = tol;
        return 0;
    }

    {
        RouteStep st;
        memset(&st, 0, sizeof st);
        st.line = lineno;
        if (strcmp(tok, "WAIT") == 0 || strcmp(tok, "EXPECT") == 0) {
            st.op = tok[0] == 'W' ? ROUTE_OP_WAIT : ROUTE_OP_EXPECT;
            char *name = strtok(NULL, " \t\r\n");
            if (!name || strlen(name) >= ROUTE_NAME_MAX) { fprintf(stderr, "ROUTE_PARSE: %s:%d: %s <NAME>\n", path, lineno, tok); return -1; }
            snprintf(st.name, ROUTE_NAME_MAX, "%s", name);
            if (st.op == ROUTE_OP_WAIT) {
                char *t = strtok(NULL, " \t\r\n");
                long to = t ? atol(t) : -1;
                if (to < 1) { fprintf(stderr, "ROUTE_PARSE: %s:%d: WAIT <NAME> <timeout_vblanks>\n", path, lineno); return -1; }
                st.a = (uint32_t)to;
            }
        } else if (strcmp(tok, "PRESS_UNTIL") == 0 || strcmp(tok, "PRESS_WHILE") == 0) {
            /* The boot prefix is the one part of a route that genuinely has to repeat an
             * input until something happens: the warning screens and the intro movie each
             * need their own START and there is no way to know in advance how many. Written
             * as a fixed table it is the worst kind of timed route -- every extra press is
             * one that lands on whatever comes next if the run is faster than the recording.
             * Written as a repeat-until-observed it stops the moment the screen arrives.
             *
             * PRESS_WHILE is the other half of that problem. Some screens accept an input
             * only once the work behind them finishes -- the title screen draws its NEW
             * GAME / CONTINUE options long before it will act on one, and no pixel says
             * which. Repeating until the *next* screen appears leaves a window in which a
             * press can still be latched by it; repeating only while the current screen is
             * on show ends the input well before anything else can receive it. */
            st.op = tok[6] == 'U' ? ROUTE_OP_UNTIL : ROUTE_OP_WHILE;
            char *name = strtok(NULL, " \t\r\n");
            char *m = strtok(NULL, " \t\r\n"), *w = strtok(NULL, " \t\r\n");
            char *p = strtok(NULL, " \t\r\n"), *t = strtok(NULL, " \t\r\n");
            if (!name || strlen(name) >= ROUTE_NAME_MAX || !m || !w || !p || !t) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: %s <NAME> <hexmask> <width> <period> <timeout>\n", path, lineno, tok);
                return -1;
            }
            snprintf(st.name, ROUTE_NAME_MAX, "%s", name);
            st.a = (uint32_t)strtoul(m, NULL, 16);
            st.b = (uint32_t)strtoul(w, NULL, 10);
            st.c = (uint32_t)strtoul(p, NULL, 10);
            st.d = (uint32_t)strtoul(t, NULL, 10);
            if (st.b < 1 || st.c <= st.b || st.d < st.c) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: %s needs width >= 1, period > width, timeout >= period\n", path, lineno, tok);
                return -1;
            }
        } else if (strcmp(tok, "PRESS") == 0) {
            st.op = ROUTE_OP_PRESS;
            char *m = strtok(NULL, " \t\r\n"), *w = strtok(NULL, " \t\r\n");
            if (!m || !w) { fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS <hexmask> <width>\n", path, lineno); return -1; }
            st.a = (uint32_t)strtoul(m, NULL, 16);
            st.b = (uint32_t)strtoul(w, NULL, 10);
            if (st.b < 1) { fprintf(stderr, "ROUTE_PARSE: %s:%d: PRESS width must be >= 1\n", path, lineno); return -1; }
        } else if (strcmp(tok, "DELAY") == 0) {
            st.op = ROUTE_OP_DELAY;
            char *n = strtok(NULL, " \t\r\n");
            long v = n ? atol(n) : -1;
            if (v < 0) { fprintf(stderr, "ROUTE_PARSE: %s:%d: DELAY <vblanks>\n", path, lineno); return -1; }
            st.a = (uint32_t)v;
        } else if (strcmp(tok, "END") == 0) {
            st.op = ROUTE_OP_END;
        } else {
            fprintf(stderr, "ROUTE_PARSE: %s:%d: unknown route keyword '%s'\n", path, lineno, tok);
            return -1;
        }
        if (s_route_nsteps >= ROUTE_MAX_STEPS) { fprintf(stderr, "ROUTE_PARSE: %s:%d: more than %d steps\n", path, lineno, ROUTE_MAX_STEPS); return -1; }
        s_route_prog[s_route_nsteps++] = st;
    }
    return 0;
}

/* Discard every loaded route. Only the executable regression tests call this; a run loads
 * its route once. */
void sr_route_reset(void) {
    s_route_ncp = s_route_nsteps = s_route_nlegacy = 0;
    s_route_pc = 0;
    s_route_step_start = 0;
    s_route_step_started = 0;
    s_route_state = ROUTE_OFF;
    s_route_cols = 12; s_route_rows = 8;
    s_route_tol = 12;
    s_route_sample_every = 20;
    s_route_keys = 0;
    s_route_while_seen = 0;
    s_route_last_attempt = 0;
    s_route_have_attempt = 0;
    snprintf(s_route_seen, sizeof s_route_seen, "no screen was observed at all");
    s_route_loaded = 0;
}

int sr_route_load(const char *path) {
    char line[4096];
    int lineno = 0, bad = 0;
    FILE *fp;

    s_route_ncp = s_route_nsteps = s_route_nlegacy = 0;
    s_route_pc = 0;
    s_route_step_started = 0;
    s_route_keys = 0;
    s_route_while_seen = 0;
    s_route_last_attempt = 0;
    s_route_have_attempt = 0;
    s_route_state = ROUTE_OFF;
    s_route_loaded = 1;
    if (!path || !path[0]) return 0;
    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "ROUTE_PARSE: cannot open route file '%s'\n", path);
        return 0;
    }
    while (fgets(line, sizeof line, fp)) {
        lineno++;
        if (route_parse_line(line, lineno, path) != 0) { bad = 1; break; }
    }
    fclose(fp);
    if (bad) {
        route_fail("route file '%s' is not usable; refusing to run an unverified route", path);
        return 0;
    }
    if (s_route_nsteps > 0) {
        for (int i = 0; i < s_route_nsteps; i++) {
            RouteStep *st = &s_route_prog[i];
            if ((st->op == ROUTE_OP_WAIT || st->op == ROUTE_OP_EXPECT) && route_find(st->name) < 0) {
                fprintf(stderr, "ROUTE_PARSE: %s:%d: no CHECKPOINT defines '%s'\n", path, st->line, st->name);
                route_fail("route file '%s' names undefined checkpoints", path);
                return 0;
            }
        }
        if (route_finalize_checkpoints(path) != 0) {
            route_fail("route file '%s' has checkpoints that cannot be told apart", path);
            return 0;
        }
        s_route_state = ROUTE_RUNNING;
        fprintf(stderr, "ROUTE: program loaded from %s (%d steps, %d checkpoints, grid %dx%d, "
                        "sample_every=%d, tolerance=%d)\n",
                path, s_route_nsteps, s_route_ncp, s_route_cols, s_route_rows,
                s_route_sample_every, s_route_tol);
        return 1;
    }
    if (s_route_nlegacy > 0) {
        s_route_state = ROUTE_LEGACY;
        return 1;
    }
    if (s_route_ncp > 0) {
        /* Checkpoints with no program is an unfinished route file. Running it would leave
         * the pad on its default START pulse, which is not what the file asks for. */
        route_fail("route file '%s' defines checkpoints but no steps", path);
        return 0;
    }
    return 0;
}

static void route_load_once(void) {
    if (s_route_loaded) return;
    s_route_loaded = 1;
    s_route_learn = getenv("SR_ROUTE_LEARN") ? 1 : 0;
    const char *sp = getenv("SR_PADSCRIPT");
    if (sp && sp[0]) sr_route_load(sp);
}

static void route_advance(void) { s_route_pc++; s_route_step_started = 0; }

/* One vblank of the route program. `sig` is the observed screen signature, or NULL when
 * no observation was taken this vblank. Returns the button mask for this vblank. */
uint32_t sr_route_step(uint32_t v, const uint8_t *sig) {
    uint32_t keys = 0;
    if (s_route_state != ROUTE_RUNNING) return 0;
    if (sig) {
        int bd = 255, bi = route_best_match(sig, &bd);
        if (bi >= 0 && bd <= s_route_cp[bi].tol)
            snprintf(s_route_seen, sizeof s_route_seen, "last saw %s at vblank %u (d=%d)",
                     s_route_cp[bi].name, v, bd);
        else
            snprintf(s_route_seen, sizeof s_route_seen,
                     "last saw an unrecorded screen at vblank %u (closest %s d=%d)", v,
                     bi >= 0 ? s_route_cp[bi].name : "-", bd);
    }
    for (int guard = 0; guard <= ROUTE_MAX_STEPS; guard++) {
        if (s_route_pc >= s_route_nsteps) {
            s_route_state = ROUTE_DONE;
            fprintf(stderr, "ROUTE_OK: %d steps completed by vblank %u\n", s_route_nsteps, v);
            return keys;
        }
        RouteStep *st = &s_route_prog[s_route_pc];
        if (!s_route_step_started) { s_route_step_start = v; s_route_step_started = 1; }
        uint32_t el = v - s_route_step_start;
        switch (st->op) {
        case ROUTE_OP_PRESS:
            if (el < st->b) return keys | st->a;
            route_advance();
            continue;
        case ROUTE_OP_DELAY:
            if (el < st->a) return keys;
            route_advance();
            continue;
        case ROUTE_OP_UNTIL: {
            int d = 255;
            if (sig && route_matches(st->name, sig, &d)) {
                fprintf(stderr, "ROUTE: reached %s at vblank %u (step %d, d=%d, after %u vblanks)\n",
                        st->name, v, s_route_pc, d, el);
                route_advance();
                continue;
            }
            if (el >= st->d) {
                route_fail("line %d: PRESS_UNTIL %s gave up after %u vblanks (from vblank %u); %s",
                           st->line, st->name, el, s_route_step_start, route_seen_desc());
                return keys;
            }
            if ((el % st->c) < st->b) keys |= st->a;
            return keys;
        }
        case ROUTE_OP_WHILE: {
            int d = 255;
            if (sig && !route_matches(st->name, sig, &d)) {
                if (s_route_while_seen) {
                    fprintf(stderr, "ROUTE: left %s at vblank %u (step %d, after %u vblanks)\n",
                            st->name, v, s_route_pc, el);
                    s_route_while_seen = 0;
                    route_advance();
                    continue;
                }
            } else if (sig) {
                /* Do not let the step complete before its screen was ever on show: entering
                 * it one vblank early would otherwise skip the input entirely. */
                s_route_while_seen = 1;
            }
            if (el >= st->d) {
                int ever = s_route_while_seen;
                s_route_while_seen = 0;
                route_fail("line %d: PRESS_WHILE %s gave up after %u vblanks (from vblank %u); "
                           "the screen %s; %s",
                           st->line, st->name, el, s_route_step_start,
                           ever ? "never went away" : "was never on show", route_seen_desc());
                return keys;
            }
            if ((el % st->c) < st->b) keys |= st->a;
            return keys;
        }
        case ROUTE_OP_WAIT: {
            int d = 255;
            if (sig && route_matches(st->name, sig, &d)) {
                fprintf(stderr, "ROUTE: reached %s at vblank %u (step %d, d=%d)\n",
                        st->name, v, s_route_pc, d);
                route_advance();
                continue;
            }
            if (el >= st->a) {
                int wi = route_find(st->name);
                route_fail("line %d: WAIT %s timed out after %u vblanks (from vblank %u to %u); "
                           "%s (a match needs d<=%d)",
                           st->line, st->name, el, s_route_step_start, v, route_seen_desc(),
                           wi >= 0 ? s_route_cp[wi].tol : s_route_tol);
                return keys;
            }
            return keys;
        }
        case ROUTE_OP_EXPECT: {
            if (!sig) {
                /* An EXPECT that never receives an observation must not stall forever: the
                 * framebuffer sync can fail, and a silent stall would look like progress. */
                if (el >= (uint32_t)(s_route_sample_every * 8 + 60)) {
                    route_fail("line %d: EXPECT %s had no framebuffer observation for %u vblanks "
                               "from vblank %u", st->line, st->name, el, s_route_step_start);
                    return keys;
                }
                return keys;
            }
            int d = 255;
            if (route_matches(st->name, sig, &d)) {
                fprintf(stderr, "ROUTE: confirmed %s at vblank %u (step %d, d=%d)\n",
                        st->name, v, s_route_pc, d);
                route_advance();
                continue;
            }
            {
                int bd = 255, bi = route_best_match(sig, &bd);
                int wi = route_find(st->name);
                route_fail("line %d: EXPECT %s at vblank %u, but the screen is %s d=%d "
                           "(%s d=%d, match needs d<=%d)",
                           st->line, st->name, v,
                           bi >= 0 ? s_route_cp[bi].name : "<unknown>", bd, st->name, d,
                           wi >= 0 ? s_route_cp[wi].tol : s_route_tol);
            }
            return keys;
        }
        case ROUTE_OP_END:
            s_route_state = ROUTE_DONE;
            fprintf(stderr, "ROUTE_OK: %d steps completed by vblank %u\n", s_route_pc + 1, v);
            return keys;
        default:
            route_fail("line %d: corrupt route step", st->line);
            return keys;
        }
    }
    return keys;
}

int sr_route_status(void) { return s_route_state; }
int sr_route_sig_bytes(void) { return route_sig_bytes(); }
/* Test hook: run one observer sample exactly as the periodic sampler does and
 * report whether it produced an observation.  Exposed so the ownership rule --
 * the observer does not read the scanout until the guest owns it -- is testable
 * without a title, a route file or a GPU.  route_sample() is defined later in
 * this file, so the hook body lives beside it. */
int sr_route_test_sample(uint8_t *out);

/* sceCtrl: sticks centred. To drive past the skippable intro movie and confirmation prompts
 * without a human, pulse START/CROSS/CIRCLE for a few frames on a periodic cadence (edge presses,
 * so the game sees press+release). Disable with SR_NOINPUT for a truly neutral pad. */
static uint32_t h_CtrlButtons(void) {
    /* Live keyboard (windowed mode) is OR'd with the auto-input pulse below -- in this headless
     * window environment no key is ever pressed, so without the pulse the intro movie never gets
     * its START and loops forever. SR_NOINPUT disables the pulse for a neutral pad. */
    uint32_t keys = gui_on() ? gui_buttons() : 0;
    /* SR_PADSCRIPT=<file>: either a route program (issue #64: state-qualified steps) or the
     * original absolute table of "frame hexmask width" lines -- press mask at frame for width
     * frames. Either way it replaces the default START pulse entirely. The program's keys for
     * this vblank were computed by route_tick() before the sample was latched. */
    route_load_once();
    if (s_route_state != ROUTE_OFF && s_route_state != ROUTE_LEGACY) return keys | s_route_keys;
    {
        if (s_route_nlegacy > 0) {
            for (int i = 0; i < s_route_nlegacy; i++)
                if (s_vcount_fwd >= s_route_legacy[i].f &&
                    s_vcount_fwd < s_route_legacy[i].f + s_route_legacy[i].w)
                    keys |= s_route_legacy[i].mask;
            return keys;
        }
    }
    /* The auto-START pulse below only exists to advance the intro/attract in headless or no-input
     * runs. When a real controller is connected the player drives input themselves, so suppress the
     * pulse (otherwise a phantom START every few seconds would keep opening the pause menu). */
    if (getenv("SR_NOINPUT") || gui_pad_present()) return keys;
    /* Pulse START only, briefly, on a slow cadence to skip the (minutes-long) intro movie and the
     * "press start" prompt. Pressing CROSS/CIRCLE as well drove the menus into bad states (it
     * confirmed things the game was not ready for); START alone advances the intro without that.
     * SR_PAD=<hex> overrides the mask; SR_PADPERIOD/SR_PADWIDTH tune the cadence. */
    const char *m = getenv("SR_PAD");
    uint32_t mask = m ? (uint32_t)strtoul(m, NULL, 16) : 0x0008u;   /* START */
    const char *pe = getenv("SR_PADPERIOD"); int period = pe ? atoi(pe) : 240;
    const char *pw = getenv("SR_PADWIDTH");  int width  = pw ? atoi(pw) : 4;
    const char *ps = getenv("SR_PADSTART");  int startf = ps ? atoi(ps) : 0;  /* hold input until frame */
    if (period <= 0) period = 240;
    if ((int)s_vcount_fwd < startf) return keys;
    if ((int)(s_vcount_fwd % (uint32_t)period) < width) return keys | mask;
    return keys;
}
/* sceCtrlReadBuffer*: fill SceCtrlData[count]. Positive reports pressed buttons as set bits;
 * Negative reports them inverted (set = not pressed), so it must write ~buttons -- writing the
 * positive mask there makes the game see almost every button held and run wild (it jumped through
 * an uninitialised menu handler). */
/* Latch one controller sample per frame into the ring (called from sr_vblank_tick). */
void sr_ctrl_sample(void) {
    uint8_t lx = 128, ly = 128;
    if (gui_on()) gui_analog(&lx, &ly);
    uint32_t buttons = h_CtrlButtons();
    if (getenv("SR_INLOG")) {
        static uint32_t previous;
        if (buttons != previous) {
            fprintf(stderr, "ctrl_latch: vcount=%u buttons 0x%04x -> 0x%04x lx=%u ly=%u\n",
                    s_vcount_fwd, previous, buttons, lx, ly);
            previous = buttons;
        }
    }
    s_ctrl_ring[s_ctrl_w].btn = buttons;
    s_ctrl_ring[s_ctrl_w].ts = (uint32_t)sched_vtime_us();   /* low 32 bits of guest microsecond clock at latch */
    s_ctrl_ring[s_ctrl_w].lx = lx;
    s_ctrl_ring[s_ctrl_w].ly = ly;
    if (gui_on()) gui_consume_button_pulses();
    s_ctrl_w = (s_ctrl_w + 1) % CTRL_RING;
    if (s_ctrl_w == s_ctrl_r) s_ctrl_r = (s_ctrl_r + 1) % CTRL_RING;  /* drop oldest on overflow */
    sched_wake(CTRL_WAIT_OBJ);
}

/* Fill SceCtrlData[n] from the ring. Returns the number of new samples since the last read (the
 * PSP semantics), giving the game genuine per-frame history for edge/latch detection. Negative
 * reports inverted buttons. peek does not consume or block. */
static uint32_t ctrl_fill_n(uint32_t buf, uint32_t nbufs, int negate, int peek) {
    /* Contract order: an out-of-contract request is refused before the ring is
     * consulted, so a rejected call can neither consume history nor write a byte.
     * Clamping an oversized request the way this used to is a fabricated success:
     * the guest asked for something the API cannot do and was told it worked. */
    if (nbufs > CTRL_RING) return SCE_CTRL_ERROR_INVALID_SIZE;
    if (nbufs == 0) return 0;                      /* zero requested, zero written */
    int avail = (s_ctrl_w - s_ctrl_r + CTRL_RING) % CTRL_RING;
    /* Blocking ReadBuffer waits for at least one fresh sample (delivered each VBLANK). */
    if (!peek) {
        int guard = 0;
        while (avail == 0 && sr_sched_on && guard++ < 4) {
            sched_block_on(CTRL_WAIT_OBJ);
            avail = (s_ctrl_w - s_ctrl_r + CTRL_RING) % CTRL_RING;
        }
    }
    /* Divergence kept deliberately visible rather than quietly changed: PPSSPP
     * returns 0 here, this runtime hands back one stale sample. That sits on the
     * per-frame hot path of every run, not on an error path, so retiring it needs
     * its own evidence rather than riding along with the contract fixes above. */
    if (avail < 1) avail = 1;                      /* always give at least the latest */
    if (avail > (int)nbufs) avail = (int)nbufs;
    /* Whole-span preflight with checked arithmetic. The scalar accessors reject an
     * out-of-range store one word at a time, so without this a partially valid
     * destination gets part of the history written -- and a base near the top of
     * the address space wraps buf + i*16 down into an unrelated but IN-range guest
     * address -- while the return value still claims every sample landed.
     *
     * avail is bounded by CTRL_RING above, so the multiply cannot currently wrap and
     * that leg has NO failing-before -- it is defence in depth against a later ring
     * resize, not a repaired defect. The span check itself does have one. */
    uint32_t span = 0;
    if (!sr_size_mul_ok((uint32_t)avail, CTRL_SAMPLE_BYTES, &span) ||
        !sr_guest_span_writable(buf, span))
        return 0;
    /* Oldest of the delivered samples first; peek differs only in blocking and in
     * whether the read cursor is advanced below, not in where the window starts. */
    int start = (s_ctrl_w - avail + CTRL_RING) % CTRL_RING;
    for (int i = 0; i < avail; i++) {
        CtrlSample smp = s_ctrl_ring[(start + i) % CTRL_RING];
        uint32_t field = negate ? ~smp.btn : smp.btn;   /* negative mode inverts buttons only */
        uint32_t e = buf + (uint32_t)i * CTRL_SAMPLE_BYTES;
        MEM_W32(e + 0, smp.ts);        /* stamped when the sample was latched */
        MEM_W32(e + 4, field);
        MEM_W8(e + 8, smp.lx); MEM_W8(e + 9, smp.ly); MEM_W8(e + 10, 128); MEM_W8(e + 11, 128);
    }
    if (!peek) s_ctrl_r = s_ctrl_w;                /* consume */
    if (getenv("SR_INLOG")) {
        static unsigned long calls = 0;
        CtrlSample latest = s_ctrl_ring[(s_ctrl_w - 1 + CTRL_RING) % CTRL_RING];
        if ((++calls % 200) == 0 || (latest.btn & 0x8))
            fprintf(stderr, "ctrl_fill #%lu vc=%u avail=%d latest=0x%x lx=%u ly=%u buf=0x%08x neg=%d peek=%d\n",
                    calls, s_vcount_fwd, avail, latest.btn, latest.lx, latest.ly, buf, negate, peek);
    }
    return (uint32_t)avail;
}
static uint32_t ctrl_fill(uint32_t buf, uint32_t count, int negate) {
    return ctrl_fill_n(buf, count, negate, 0);
}
static uint32_t h_CtrlReadBuffer(CpuState *s) { return ctrl_fill(A0, A1, 0); }

/* sceDisplay: remember the framebuffer; vblank waits block until the next delivered vblank. */
static void dump_fb_fmt(const char *path, uint32_t fbaddr, int fmt, uint32_t stride);
typedef struct {
    uint32_t addr;
    int32_t stride;
    int32_t fmt;
} DisplayFrameState;

/* The PSP keeps the current scanout state separate from the next-frame
 * request.  The host presentation path is an additional, rate-limited layer;
 * it must not be confused with the guest's display latch or VBLANK state. */
static DisplayFrameState s_display_active = { 0x04000000u, 512, 3 };
static DisplayFrameState s_display_latched = { 0x04000000u, 512, 3 };
static int s_display_latched_pending;
/* Has the guest ever established a COMPLETE scanout state -- address, stride and
 * format all supplied by sceDisplaySetFrameBuf and applied to the active state?
 *
 * Until it has, the initializers above are the only thing s_display_active holds,
 * and they are a placeholder rather than an observation of anything.  The GE can
 * (and does) register a render target at 0x04000000 before the guest's first
 * SetFrameBuf, in whatever pixel format the display list asked for -- so a reader
 * that trusts the initializer's format decodes the buffer wrongly, and a reader
 * that hands the initializer to the GPU coherence boundary trips a mismatch alarm
 * that exists to catch genuine disagreements.
 *
 * Set only where s_display_active receives a complete guest-provided state: the
 * immediate (sync=0) path, and the VBLANK that applies a latched request.  A
 * latched request alone is not enough -- it publishes stride and format at once
 * but leaves the previous scanout address in place until the latch lands. */
static int s_display_configured;
static uint32_t s_framebuf = 0x04000000u, s_vcount = 0;
static uint32_t s_last_flip_vcount = 0;   /* no-frame watchdog clock (see sr_vblank_tick) */

/* sceDisplaySetFrameBuf outcome accounting.
 *
 * A stretch with no presented frame has two completely different causes, and the
 * no-frame watchdog cannot distinguish them from the flip counter alone:
 *   - the guest never asks for a flip (its render/present loop is not reaching the
 *     call), or
 *   - the guest does ask and this handler refuses the request.
 * Those point at opposite subsystems, so record which one actually happened rather
 * than leaving the diagnosis to inference. Counters only; no behavior depends on
 * them, and the cost is a few increments per call. */
static struct {
    unsigned long calls, immediate, latched, rejected;
    uint32_t last_vcount, last_addr, last_sync;
    int32_t  last_stride, last_fmt;
    uint32_t last_err, last_err_vcount, last_err_addr, last_err_sync;
    int32_t  last_err_stride, last_err_fmt;
} s_setfb;

/* Number of no-frame watchdog observations emitted (see sr_vblank_tick). The
 * threshold alone is a NO-NEW-FLIP observation, not a hang verdict, so this
 * counter only says how often the threshold was crossed. File-local: the
 * selftest reads it through the accessor below, nothing else consumes it. */
static unsigned long s_watchdog_fires = 0;
/* Highest 600-period bucket already reported for the current no-flip stretch.
 * VCOUNT advances by whole elapsed display periods at the scheduler source
 * latch (sr_display_advance_vcount), so a single serviced tick can carry the
 * no-flip distance across a 600 boundary without ever landing on a multiple of
 * 600 -- 598 -> 602 must still report exactly once.  Comparing bucket indices
 * detects the crossing; an exact-modulus test cannot.  Reset wherever
 * s_last_flip_vcount is reset so a fresh stretch starts at bucket 0. */
static uint32_t s_watchdog_bucket = 0;

#ifdef SR_HLE_THREAD_SELFTEST
/* White-box view of the flip accounting for the conformance harness. The counters are
 * file-static because nothing outside this file may steer display state; the regression
 * needs to read them to prove a refused request is recorded rather than silently lost. */
void sr_display_test_flip_counts(unsigned long *calls, unsigned long *immediate,
                                 unsigned long *latched, unsigned long *rejected,
                                 uint32_t *last_err);
/* White-box view of the no-frame observation count and the vblanks-since-flip
 * clock for the conformance harness. */
void sr_watchdog_test_state(unsigned long *fires, uint32_t *vblanks_since_flip);
#endif

/* Test-only: restore the display statics to the values a fresh process starts
 * with.  sr_hle_init() latches once per process, so a suite that runs many
 * cases in one executable has no other way to isolate display state -- and
 * without it a case that changes the latched pixel format silently rejects the
 * next case's immediate flip.  Production never calls this. */
void sr_display_test_reset(void) {
    DisplayFrameState boot = { 0x04000000u, 512, 3 };
    s_display_active = boot;
    s_display_latched = boot;
    s_display_latched_pending = 0;
    s_display_configured = 0;
    s_framebuf = 0x04000000u;
}

/* Record a refused sceDisplaySetFrameBuf and return the PSP error unchanged, so
 * every rejection path is accounted for in exactly one place. */
static uint32_t display_setframebuf_reject(uint32_t err, uint32_t addr, int32_t stride,
                                           int32_t fmt, uint32_t sync) {
    s_setfb.rejected++;
    s_setfb.last_err = err;
    s_setfb.last_err_vcount = s_vcount;
    s_setfb.last_err_addr = addr;
    s_setfb.last_err_stride = stride;
    s_setfb.last_err_fmt = fmt;
    s_setfb.last_err_sync = sync;
    return err;
}

/* A guest-VRAM dump is a publication boundary, not an ordinary present. The Vulkan
 * presenter deliberately queues its readback, so materialize only this display target
 * before exposing the legacy dump and refuse the artifact if the target cannot be made
 * current. NO_TARGET is a validated answer: it means guest memory is authoritative. */
static int snapshot_sync_ok(uint32_t fbaddr, uint32_t fmt, uint32_t stride, const char *what) {
    GeGpuFbDescriptor d = { fbaddr, fmt, stride, 480u, 272u };
    int rc = gegpu_sync_guest_fb(&d);
    if (rc == GEGPU_SYNC_OK || rc == GEGPU_SYNC_NO_TARGET) return 1;
    fprintf(stderr,
            "%s: snapshot synchronisation FAILED (rc=%d) for addr=0x%08x fmt=%u stride=%u "
            "480x272 -- refusing to publish stale or misread guest VRAM\n",
            what, rc, fbaddr, fmt, stride);
    return 0;
}

/* Bounded provenance trace for issue #143.  The missing post-match/menu panels
 * reproduce in both renderers, so the next useful split is whether the guest
 * submitted a display list at all.  Keep this separate from SR_GEDUMP: that
 * stream is intentionally broad and too noisy for a 35k-vblank route.
 *
 * SR_GE_ENQUEUE_TRACE=1 enables the trace.  Optional
 * SR_GE_ENQUEUE_TRACE_WINDOWS=a-b[,c-d...] restricts output to up to eight
 * inclusive vblank ranges.  An invalid, non-empty window string fails closed
 * (no trace records) instead of accidentally flooding stderr. */
#define GE_ENQUEUE_TRACE_MAX_WINDOWS 8
#define GE_ENQUEUE_TRACE_THREADS 64

typedef struct {
    uint32_t uid;
    uint32_t nid;
    uint32_t call_pc;
    uint32_t ra;
    uint32_t frame;
    const char *name;
    uint64_t seq;
} GeEnqueueTraceHle;

typedef struct {
    uint32_t uid;
    uint32_t callback_uid;
    uint32_t entry;
    uint32_t frame;
    uint64_t seq;
} GeEnqueueTraceCallback;

static GeEnqueueTraceHle s_ge_trace_hle[GE_ENQUEUE_TRACE_THREADS];
static GeEnqueueTraceCallback s_ge_trace_callback[GE_ENQUEUE_TRACE_THREADS];
static uint64_t s_ge_trace_seq;

static uint32_t ge_enqueue_trace_call_pc(const CpuState *s) {
    return s->r[31] >= 8u ? s->r[31] - 8u : 0u;
}

static int ge_enqueue_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("SR_GE_ENQUEUE_TRACE") != NULL;
    return enabled;
}

static int ge_enqueue_trace_in_window(void) {
    static int initialized;
    static int configured;
    static int valid;
    static uint32_t lo[GE_ENQUEUE_TRACE_MAX_WINDOWS];
    static uint32_t hi[GE_ENQUEUE_TRACE_MAX_WINDOWS];
    static int count;

    if (!ge_enqueue_trace_enabled()) return 0;
    if (!initialized) {
        initialized = 1;
        const char *windows = getenv("SR_GE_ENQUEUE_TRACE_WINDOWS");
        configured = windows != NULL && windows[0] != '\0';
        valid = !configured;
        if (configured) {
            const char *p = windows;
            valid = 1;
            while (*p && count < GE_ENQUEUE_TRACE_MAX_WINDOWS) {
                char *end = NULL;
                unsigned long first = strtoul(p, &end, 10);
                if (end == p || *end != '-' || first > UINT32_MAX) { valid = 0; break; }
                p = end + 1;
                unsigned long last = strtoul(p, &end, 10);
                if (end == p || last > UINT32_MAX || first > last) { valid = 0; break; }
                lo[count] = (uint32_t)first;
                hi[count] = (uint32_t)last;
                count++;
                p = end;
                if (*p == ',') p++;
                else if (*p != '\0') { valid = 0; break; }
            }
            if (*p != '\0' || count == 0) valid = 0;
            if (!valid) {
                fprintf(stderr,
                        "GE_ENQUEUE_TRACE: invalid SR_GE_ENQUEUE_TRACE_WINDOWS='%s' -- trace disabled\n",
                        windows);
            } else {
                for (int i = 0; i < count; i++)
                    fprintf(stderr, "GE_ENQUEUE_TRACE_WINDOW[%d]=%u-%u\n", i, lo[i], hi[i]);
            }
        }
    }
    if (!valid) return 0;
    if (!configured) return 1;
    for (int i = 0; i < count; i++)
        if (s_vcount >= lo[i] && s_vcount <= hi[i]) return 1;
    return 0;
}

static GeEnqueueTraceHle *ge_enqueue_trace_hle_slot(uint32_t uid) {
    GeEnqueueTraceHle *free_slot = NULL;
    for (int i = 0; i < GE_ENQUEUE_TRACE_THREADS; i++) {
        if (s_ge_trace_hle[i].uid == uid) return &s_ge_trace_hle[i];
        if (!s_ge_trace_hle[i].uid && !free_slot) free_slot = &s_ge_trace_hle[i];
    }
    return free_slot ? free_slot : &s_ge_trace_hle[uid % GE_ENQUEUE_TRACE_THREADS];
}

static GeEnqueueTraceCallback *ge_enqueue_trace_callback_slot(uint32_t uid) {
    GeEnqueueTraceCallback *free_slot = NULL;
    for (int i = 0; i < GE_ENQUEUE_TRACE_THREADS; i++) {
        if (s_ge_trace_callback[i].uid == uid) return &s_ge_trace_callback[i];
        if (!s_ge_trace_callback[i].uid && !free_slot) free_slot = &s_ge_trace_callback[i];
    }
    return free_slot ? free_slot : &s_ge_trace_callback[uid % GE_ENQUEUE_TRACE_THREADS];
}

static void ge_enqueue_trace_note_hle(CpuState *s, uint32_t nid, const char *name) {
    if (!ge_enqueue_trace_enabled()) return;
    uint32_t uid = sched_current_uid();
    GeEnqueueTraceHle *event = ge_enqueue_trace_hle_slot(uid);
    *event = (GeEnqueueTraceHle){
        uid, nid, ge_enqueue_trace_call_pc(s), s->r[31], s_vcount,
        name, ++s_ge_trace_seq
    };
}

static void ge_enqueue_trace_note_callback(CpuState *s, uint32_t uid, uint32_t entry) {
    if (!ge_enqueue_trace_enabled()) return;
    uint32_t thread_uid = sched_current_uid();
    GeEnqueueTraceCallback *event = ge_enqueue_trace_callback_slot(thread_uid);
    *event = (GeEnqueueTraceCallback){
        thread_uid, uid, entry, s_vcount, ++s_ge_trace_seq
    };
    (void)s;
}

static void ge_enqueue_trace_emit(CpuState *s, const char *op,
                                  uint32_t list_id, uint32_t list,
                                  uint32_t stall, uint32_t cbid) {
    if (!ge_enqueue_trace_in_window()) return;
    uint32_t uid = sched_current_uid();
    GeEnqueueTraceHle *prev_hle = ge_enqueue_trace_hle_slot(uid);
    GeEnqueueTraceCallback *prev_cb = ge_enqueue_trace_callback_slot(uid);
    fprintf(stderr,
            "GE_ENQUEUE_TRACE frame=%u op=%s thread=0x%x call_pc=0x%08x ra=0x%08x "
            "list_id=0x%08x list=0x%08x stall=0x%08x cbid=0x%08x "
            "prev_hle=%s/0x%08x@0x%08x,ra=0x%08x,frame=%u,seq=%llu "
            "prev_cb=0x%08x@0x%08x,frame=%u,seq=%llu\n",
            s_vcount, op, uid, ge_enqueue_trace_call_pc(s), s->r[31],
            list_id, list, stall, cbid,
            prev_hle->seq && prev_hle->name ? prev_hle->name : "none",
            prev_hle->seq ? prev_hle->nid : 0u,
            prev_hle->seq ? prev_hle->call_pc : 0u,
            prev_hle->seq ? prev_hle->ra : 0u,
            prev_hle->seq ? prev_hle->frame : 0u,
            (unsigned long long)prev_hle->seq,
            prev_cb->seq ? prev_cb->callback_uid : 0u,
            prev_cb->seq ? prev_cb->entry : 0u,
            prev_cb->seq ? prev_cb->frame : 0u,
            (unsigned long long)prev_cb->seq);
}

static void ge_enqueue_trace_result(CpuState *s, const char *op,
                                    uint32_t list_id, const char *outcome,
                                    uint32_t next_pc, int list_completed) {
    if (!ge_enqueue_trace_in_window()) return;
    extern unsigned long g_ge_list_sig, g_ge_prim_count;
    extern unsigned long g_list_writes, g_list_nonblack, g_list_clearpx;
    extern unsigned long g_ge_list_through_cmds, g_ge_list_transform_cmds;
    extern unsigned long g_ge_list_through_vertices, g_ge_list_transform_vertices;
    extern unsigned long g_ge_list_through_sprites, g_ge_list_transform_sprites;
    fprintf(stderr,
            "GE_ENQUEUE_RESULT frame=%u op=%s thread=0x%x call_pc=0x%08x "
            "list_id=0x%08x outcome=%s next_pc=0x%08x "
            "complete=%d sig=0x%08lx prims=%lu "
            "through=%lu/%lu/%lu transform=%lu/%lu/%lu "
            "writes=%lu nonblack=%lu clearpx=%lu\n",
            s_vcount, op, sched_current_uid(), ge_enqueue_trace_call_pc(s),
            list_id, outcome, next_pc, list_completed,
            list_completed ? g_ge_list_sig : 0ul,
            list_completed ? g_ge_prim_count : 0ul,
            list_completed ? g_ge_list_through_cmds : 0ul,
            list_completed ? g_ge_list_through_vertices : 0ul,
            list_completed ? g_ge_list_through_sprites : 0ul,
            list_completed ? g_ge_list_transform_cmds : 0ul,
            list_completed ? g_ge_list_transform_vertices : 0ul,
            list_completed ? g_ge_list_transform_sprites : 0ul,
            list_completed ? g_list_writes : 0ul,
            list_completed ? g_list_nonblack : 0ul,
            list_completed ? g_list_clearpx : 0ul);
}
static int display_address_valid(uint32_t addr) {
    uint32_t phys = (uint32_t)SR_PHYS(addr);
    return (phys >= 0x04000000u && phys < 0x04200000u) ||
           (phys >= 0x08000000u && phys < 0x0c000000u);
}

/* Validate the complete host-read span before the presenter dereferences it.
 * Negative PSP strides are accepted by the syscall contract but are not a
 * safe source for the top-left host read, so the host presentation is skipped
 * for those synthetic cases while the guest-visible state remains intact. */
static int display_host_span_valid(const DisplayFrameState *fb) {
    if (!fb->addr) return 1;
    if (fb->stride <= 0 || fb->fmt < 0 || fb->fmt > 3) return 0;
    uint32_t bpp = fb->fmt == 3 ? 4u : 2u;
    uint32_t row_bytes, bytes, last;
    if (!sr_size_mul_ok((uint32_t)fb->stride, bpp, &row_bytes) ||
        !sr_size_mul_ok(row_bytes, 272u, &bytes) || bytes == 0u)
        return 0;
    uint32_t phys = (uint32_t)SR_PHYS(fb->addr);
    if (!sr_size_add_ok(phys, bytes - 1u, &last)) return 0;
    if (phys >= 0x04000000u && phys < 0x04200000u)
        if (last >= 0x04200000u) return 0;
    if (phys >= 0x08000000u && phys < 0x0c000000u)
        if (last >= 0x0c000000u) return 0;
    return sr_guest_span_readable(fb->addr, bytes);
}

static void display_present_active(void) {
    if (!gui_on() || !s_display_active.addr) return;
    if (!display_host_span_valid(&s_display_active)) {
        fprintf(stderr, "DISPLAY_PRESENT: refusing invalid span addr=0x%08x stride=%d fmt=%d\n",
                s_display_active.addr, s_display_active.stride, s_display_active.fmt);
        return;
    }
    gui_present(s_display_active.addr, s_display_active.fmt,
                (uint32_t)s_display_active.stride);
}

/* ---- route observation (issue #64) ------------------------------------------------
 *
 * Decode one presented pixel. fmt: 0=5650, 1=5551, 2=4444, 3=8888 -- the same table
 * dump_fb_fmt writes its PPMs from, factored out so the route signature and the capture
 * files can never disagree about what a frame looked like. */
static void fb_decode_px(uint32_t fbaddr, int fmt, uint32_t stride, int x, int y,
                         unsigned char rgb[3]) {
    if (fmt == 3) {
        uint32_t p = MEM_R32(fbaddr + (uint32_t)(y * (int)stride + x) * 4);
        rgb[0] = p & 0xFF; rgb[1] = (p >> 8) & 0xFF; rgb[2] = (p >> 16) & 0xFF;
        return;
    }
    uint16_t p = MEM_R16(fbaddr + (uint32_t)(y * (int)stride + x) * 2);
    if (fmt == 1) {
        rgb[0] = (unsigned char)(((p) & 0x1F) * 255 / 31);
        rgb[1] = (unsigned char)(((p >> 5) & 0x1F) * 255 / 31);
        rgb[2] = (unsigned char)(((p >> 10) & 0x1F) * 255 / 31);
    } else if (fmt == 2) {
        rgb[0] = (unsigned char)(((p) & 0xF) * 17);
        rgb[1] = (unsigned char)(((p >> 4) & 0xF) * 17);
        rgb[2] = (unsigned char)(((p >> 8) & 0xF) * 17);
    } else {
        rgb[0] = (unsigned char)(((p) & 0x1F) * 255 / 31);
        rgb[1] = (unsigned char)(((p >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (unsigned char)(((p >> 11) & 0x1F) * 255 / 31);
    }
}

/* The presenter queues its readback, so guest VRAM has to be made current before it can
 * be believed -- the same boundary snapshot_sync_ok() enforces for published captures.
 * Reporting is bounded because a route samples repeatedly and a systematic sync failure
 * would otherwise bury the ROUTE_FAIL that follows it. */
static int route_sync_fb(void) {
    GeGpuFbDescriptor d = { s_display_active.addr, (uint32_t)s_display_active.fmt,
                            (uint32_t)s_display_active.stride, 480u, 272u };
    int rc = gegpu_sync_guest_fb(&d);
    if (rc == GEGPU_SYNC_OK || rc == GEGPU_SYNC_NO_TARGET) return 1;
    static int warned = 0;
    if (warned < 3) {
        warned++;
        fprintf(stderr, "ROUTE: framebuffer synchronisation failed (rc=%d); no observation "
                        "this sample\n", rc);
    }
    return 0;
}

/* Mean R/G/B of a fixed 4x4 subgrid of each cell. Subsampling rather than averaging every
 * pixel keeps the observation cheap enough that it cannot pace the run it is measuring:
 * at the default 12x8 grid this reads 1536 pixels, once every SAMPLE_EVERY vblanks, and
 * only while a WAIT or EXPECT is pending. */
static int route_sample(uint8_t *out) {
    /* Nothing to observe before the guest owns the scanout state: see
     * s_display_configured.  This is deliberately checked ahead of
     * route_sync_fb() so the observer never hands the GPU coherence boundary a
     * placeholder descriptor -- doing so raised a `snapshot sync refused'
     * mismatch during ordinary boot, which reads exactly like the genuine
     * disagreement that check exists to report. */
    if (!s_display_configured) return 0;
    if (!s_display_active.addr || !display_host_span_valid(&s_display_active)) return 0;
    if (!route_sync_fb()) return 0;
    uint32_t stride = (uint32_t)s_display_active.stride;
    if (!stride) stride = 512;
    int fmt = s_display_active.fmt;
    int k = 0;
    for (int cy = 0; cy < s_route_rows; cy++)
        for (int cx = 0; cx < s_route_cols; cx++) {
            unsigned sum[3] = { 0, 0, 0 };
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int x = (cx * 480) / s_route_cols + (sx * (480 / s_route_cols)) / 4;
                    int y = (cy * 272) / s_route_rows + (sy * (272 / s_route_rows)) / 4;
                    if (x > 479) x = 479;
                    if (y > 271) y = 271;
                    unsigned char rgb[3];
                    fb_decode_px(s_display_active.addr, fmt, stride, x, y, rgb);
                    sum[0] += rgb[0]; sum[1] += rgb[1]; sum[2] += rgb[2];
                }
            out[k++] = (uint8_t)(sum[0] / 16);
            out[k++] = (uint8_t)(sum[1] / 16);
            out[k++] = (uint8_t)(sum[2] / 16);
        }
    return 1;
}

int sr_route_test_sample(uint8_t *out) { return route_sample(out); }

/* SR_ROUTE_LEARN: print the signature of one frame. Authoring a checkpoint means looking
 * at a captured frame and deciding what screen it is, so the signature has to be emitted
 * for exactly the vblanks that are captured -- otherwise the author is transcribing a
 * signature from a frame they never saw. Called from the periodic sampler and again from
 * the capture path, deduplicated by vblank. */
static void route_learn_emit(uint32_t v, const uint8_t *sig) {
    static uint32_t last = UINT32_MAX;
    uint8_t local[ROUTE_SIG_MAX];
    char hex[ROUTE_SIG_MAX * 2 + 1];
    int n = route_sig_bytes();
    if (v == last) return;
    if (!sig) {
        if (!route_sample(local)) return;
        sig = local;
    }
    last = v;
    for (int i = 0; i < n; i++) snprintf(hex + i * 2, 3, "%02x", sig[i]);
    fprintf(stderr, "ROUTE_SIG v=%u %s\n", v, hex);
}

/* Once per delivered vblank, before the controller sample is latched, so a checkpoint
 * reached on this vblank can release its press on this vblank. */
static void route_tick(uint32_t v) {
    route_load_once();
    if (s_route_state != ROUTE_RUNNING && !s_route_learn) { s_route_keys = 0; return; }

    uint8_t sig[ROUTE_SIG_MAX];
    const uint8_t *observed = NULL;
    /* Which steps need to see the screen, stated as "everything except the ones that do
     * not". A step that needs an observation and never gets one cannot make progress and
     * only reveals itself at its timeout, so the default is deliberately to sample: adding
     * a new state-gated step must not be able to silently disable its own observations. */
    int pending = 0;
    if (s_route_state == ROUTE_RUNNING && s_route_pc < s_route_nsteps) {
        int op = s_route_prog[s_route_pc].op;
        pending = !(op == ROUTE_OP_PRESS || op == ROUTE_OP_DELAY || op == ROUTE_OP_END);
    }
    /* Elapsed-delivered-VCOUNT cadence (#109 reconstruction): delivered VCOUNT is
     * elapsed-period accounting and may jump over every exact residue of
     * SAMPLE_EVERY, so sampling only when v % SAMPLE_EVERY == 0 could starve a
     * pending route of its whole observation budget despite valid frame
     * delivery. Instead: the FIRST pending attempt samples immediately, and
     * every later attempt whose unsigned elapsed VCOUNT reached SAMPLE_EVERY
     * samples again. The due attempt is recorded BEFORE framebuffer readback so
     * failed coherence/readback attempts stay cadence-bounded instead of
     * retrying every vblank. */
    int due = !s_route_have_attempt ||
              (uint32_t)(v - s_route_last_attempt) >= (uint32_t)s_route_sample_every;
    if ((pending || s_route_learn) && due) {
        s_route_last_attempt = v;
        s_route_have_attempt = 1;
        if (route_sample(sig)) {
            observed = sig;
            if (s_route_learn) route_learn_emit(v, sig);
        }
    }
    s_route_keys = sr_route_step(v, observed);
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Selftest-only entry into the real route_tick path: lets the executable
 * regression drive production sampling/cadence/state-machine behavior without
 * a scheduler, a title, or a GPU. Production builds compile none of this. */
void sr_route_test_tick(uint32_t v) { route_tick(v); }
/* Read-only view of the cadence bookkeeping: whether an attempt was recorded
 * and which delivered VCOUNT it holds. Pins the record-BEFORE-readback order
 * (a failed readback must still consume its cadence slot). */
int sr_route_test_cadence_state(uint32_t *last_attempt) {
    if (last_attempt) *last_attempt = s_route_last_attempt;
    return s_route_have_attempt;
}
#endif

/* ---- swapchain-truthful present capture (issue #57) -------------------------------
 *
 * The capture-slot policy (fbcap_policy.h) decides who owns the next present:
 *   - SR_FBDUMP owns the first present whose vblank reaches the SR_FBDUMP threshold and
 *     publishes "present_source.ppm"; the FBDUMP block below then exits with a policy
 *     verdict (success only if the capture was actually published).
 *   - SR_FBSNAP owns every present selected by the <N>/<AFTER>/<WINDOWS> gates and
 *     publishes build/snapshots/frame_%04u.ppm (rotating) or frame_v<vcount>.ppm
 *     (windows). The legacy VRAM-side snap_*.ppm oracle file is kept beside it with its
 *     exact historical naming, so existing routes and tooling are unaffected.
 *
 * The arm MUST run before the present call so the recorded frame is exactly the one
 * being presented; the file is published inside the presenting submit, so a published
 * capture always corresponds to a presented frame. */
#define SR_FBSNAP_MAX_WINDOWS 8
static uint32_t s_fbsnap_win_lo[SR_FBSNAP_MAX_WINDOWS], s_fbsnap_win_hi[SR_FBSNAP_MAX_WINDOWS];
static int s_fbsnap_win_n = 0;
static char s_fbcap_armed[128];  /* path armed for the CURRENT frame's present ("" = none) */
static char s_fbcap_legacy[64];  /* legacy snap_*.ppm path for the same frame ("" = none) */

static void fbcap_parse_windows_once(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *w = getenv("SR_FBSNAP_WINDOWS");
    if (!w || !w[0]) return;
    const char *p = w;
    while (*p && s_fbsnap_win_n < SR_FBSNAP_MAX_WINDOWS) {
        char *end = NULL;
        unsigned long lo = strtoul(p, &end, 10);
        if (end == p) break;
        if (*end != '-') { p = end; while (*p == ',') p++; continue; }
        p = end + 1;
        unsigned long hi = strtoul(p, &end, 10);
        if (end == p) break;
        p = end;
        s_fbsnap_win_lo[s_fbsnap_win_n] = (uint32_t)lo;
        s_fbsnap_win_hi[s_fbsnap_win_n] = (uint32_t)hi;
        s_fbsnap_win_n++;
        while (*p == ',') p++;
    }
    for (int i = 0; i < s_fbsnap_win_n; i++)
        fprintf(stderr, "FBSNAP_WINDOW[%d] = %u..%u\n", i, s_fbsnap_win_lo[i], s_fbsnap_win_hi[i]);
    if (s_fbsnap_win_n == 0)
        fprintf(stderr, "FBSNAP_WINDOWS: could not parse '%s' -- no windows active\n", w);
}

/* Decide per present whether the NEXT present must be recorded, and arm it before the
 * present call. Host-side gate only: no guest work is skipped, no timing is changed,
 * and the frames that do get captured are byte-identical to an ungated run. */
static const char *fbcap_arm_for_present(uint32_t vcount, const DisplayFrameState *fb,
                                         uint32_t sync, int framebuf_set) {
    extern int sdl3vk_capture_arm(const char *path);
    s_fbcap_armed[0] = '\0';
    s_fbcap_legacy[0] = '\0';
    if (sync != 0u) return NULL;
    int fbsnap_on = sr_fbcap_env_on("SR_FBSNAP");
    int owner = sr_fbcap_owner(sr_fbcap_env_on("SR_FBDUMP"), fbsnap_on);
    if (owner == SR_FBCAP_NONE) return NULL;
    if (!framebuf_set || !display_host_span_valid(fb)) return NULL;
    if (owner == SR_FBCAP_FBDUMP) {
        const char *fd = getenv("SR_FBDUMP");
        if (!fd || vcount < (uint32_t)atoi(fd)) return NULL;
        if (!sr_fbcap_path(SR_FBCAP_FBDUMP, 0, s_fbcap_armed, sizeof s_fbcap_armed))
            return NULL;
        if (!sdl3vk_capture_arm(s_fbcap_armed)) {
            /* A refused arm must not leave a path behind: the report below would
             * otherwise print a stale result from an earlier capture. */
            s_fbcap_armed[0] = '\0';
            s_fbcap_legacy[0] = '\0';
            return NULL;
        }
        return s_fbcap_armed;
    }
    /* SR_FBSNAP: <N> every / AFTER / WINDOWS gates. */
    {
        static int fs = -2; static uint32_t fs_last = 0; static uint32_t fs_after = 0;
        if (fs == -2) {
            const char *e = getenv("SR_FBSNAP"); fs = e ? atoi(e) : 0;
            const char *a = getenv("SR_FBSNAP_AFTER");
            unsigned long av = a && a[0] ? strtoul(a, NULL, 10) : 0ul;
            fs_after = av > UINT32_MAX ? UINT32_MAX : (uint32_t)av;
            fbcap_parse_windows_once();
        }
        if (fs <= 0) return NULL;
        int in_window = 1;
        if (s_fbsnap_win_n > 0) {
            in_window = 0;
            for (int i = 0; i < s_fbsnap_win_n; i++)
                if (vcount >= s_fbsnap_win_lo[i] && vcount <= s_fbsnap_win_hi[i]) {
                    in_window = 1; break;
                }
        }
        if (!in_window || vcount < fs_after || vcount - fs_last < (uint32_t)fs) return NULL;
        fs_last = vcount;
        if (s_fbsnap_win_n > 0)
            snprintf(s_fbcap_armed, sizeof s_fbcap_armed, "frame_v%u.ppm", vcount);
        else if (!sr_fbcap_path(SR_FBCAP_FBSNAP, vcount, s_fbcap_armed, sizeof s_fbcap_armed))
            return NULL;
        if (s_fbsnap_win_n > 0)
            snprintf(s_fbcap_legacy, sizeof s_fbcap_legacy, "snap_v%u.ppm", vcount);
        else
            snprintf(s_fbcap_legacy, sizeof s_fbcap_legacy, "snap_%u.ppm",
                     (vcount / (uint32_t)fs) % 8u);
        if (!sdl3vk_capture_arm(s_fbcap_armed)) {
            s_fbcap_armed[0] = '\0';
            s_fbcap_legacy[0] = '\0';
            return NULL;
        }
        return s_fbcap_armed;
    }
}

static uint32_t h_DisplaySetFrameBuf(CpuState *s) {
    static int first_present = 1;
    uint32_t addr = A0;
    int32_t stride = (int32_t)A1;
    int32_t fmt = (int32_t)A2;
    uint32_t sync = A3;
    if (ge_log_on())
        fprintf(stderr, "DISPLAY_SET_FB: buf=0x%08x stride=%d fmt=%d sync=%u vcount=%u\n",
                addr, stride, fmt, sync, s_vcount);

    s_setfb.calls++;
    s_setfb.last_vcount = s_vcount;
    s_setfb.last_addr = addr;
    s_setfb.last_stride = stride;
    s_setfb.last_fmt = fmt;
    s_setfb.last_sync = sync;

    if (sync > 1u)                           /* SCE_KERNEL_ERROR_INVALID_MODE */
        return display_setframebuf_reject(0x80000107u, addr, stride, fmt, sync);
    if (addr && !display_address_valid(addr)) /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
        return display_setframebuf_reject(0x80000103u, addr, stride, fmt, sync);
    if ((addr & 0x0fu) != 0u)                /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
        return display_setframebuf_reject(0x80000103u, addr, stride, fmt, sync);
    if (((uint32_t)stride & 0x3fu) != 0u || (stride == 0 && addr != 0u))
        return display_setframebuf_reject(0x80000104u, addr, stride, fmt, sync);
    if (fmt < 0 || fmt > 3)                  /* SCE_KERNEL_ERROR_INVALID_FORMAT */
        return display_setframebuf_reject(0x80000108u, addr, stride, fmt, sync);
    if (sync == 0u &&
        (stride != s_display_latched.stride || fmt != s_display_latched.fmt))
        return display_setframebuf_reject(0x80000107u, addr, stride, fmt, sync);

    DisplayFrameState requested = { addr, stride, fmt };
    if (sync == 0u) {
        s_setfb.immediate++;
        /* PSP names sync=0 IMMEDIATE.  The host has no scanline clock, so this
         * updates the guest-visible/current state immediately; sync=1 remains
         * the modeled next-frame latch applied at VBLANK. */
        s_display_active = requested;
        s_display_latched = requested;
        s_display_latched_pending = 0;
        s_display_configured = 1;
        s_framebuf = addr;
        s_last_flip_vcount = s_vcount;
        s_watchdog_bucket = 0;
        /* Issue #57: arm any due present capture BEFORE the present so the recorded
         * frame is exactly the one being presented. */
        fbcap_arm_for_present(s_vcount, &s_display_active, 0u, s_framebuf != 0);
        display_present_active();
    } else {
        s_setfb.latched++;
        s_display_latched = requested;
        s_display_latched_pending = 1;
        /* PSP autotests observe format/stride immediately, but the address
         * remains the active scanout address until VBLANK starts. */
        s_display_active.stride = stride;
        s_display_active.fmt = fmt;
    }
    if (first_present) {
        first_present = 0;
        fprintf(stderr, "BOOT_EVENT phase=display_flip vcount=%u buffer=0x%08x stride=%u format=%u\n",
                s_vcount, A0, A1, A2);
    }
    /* Frame-delivery diagnostic (improvement #1): the buffer the game presents should be
     * the one the GE just drew into. A mismatch means the presented framebuffer holds
     * stale/empty VRAM (black screen) while the real scene sits in another buffer. */
    {
        static int fbd = -1;
        if (fbd < 0) { const char *e = getenv("SR_FBDIAG"); fbd = e ? 1 : 0; }
        if (fbd) {
            uint32_t ge_fb = ge_framebuffer();
            if (ge_fb && ge_fb != s_framebuf)
                fprintf(stderr, "FRAMEBUF MISMATCH: presented=0x%08x ge_draw_target=0x%08x\n", s_framebuf, ge_fb);
        }
    }
    /* SR_GEWATCH: interleave presents with GELIST lines to expose draw-vs-present ordering. */
    {
        static int gw = -1, gwa = 0;
        if (gw < 0) { gw = getenv("SR_GEWATCH") ? 1 : 0;
                      const char *p = getenv("SR_GEWATCH_AFTER"); gwa = p ? atoi(p) : 0; }
        if (gw && sync == 0u && s_vcount >= (uint32_t)gwa)
            fprintf(stderr, "PRESENT f=%u buf=0x%08x fmt=%d stride=%d\n",
                    s_vcount, s_display_active.addr, s_display_active.fmt,
                    s_display_active.stride);
    }
    /* The active state was presented above. A sync=1 request is intentionally
     * deferred until sr_vblank_tick applies the pending scanout state. */
    /* SR_FBSNAP report (issue #57): fbcap_arm_for_present() above already armed the
     * swapchain-truthful capture for this frame (path in s_fbcap_armed); the capture is
     * recorded inside the presenting submit and completes before this code runs. The
     * legacy VRAM-side oracle keeps its exact historical naming: rotating snap_%u.ppm
     * (8 most recent kept) without windows, snap_v<vcount>.ppm when windows are set, so
     * existing routes and tooling are unaffected. Host-side gate only: no guest work is
     * skipped, no timing changes, and the frames that are captured are byte-identical
     * to an ungated run. */
    if (s_fbcap_armed[0]) {
        if (s_fbcap_legacy[0]) {
            if (snapshot_sync_ok(s_display_active.addr, (uint32_t)s_display_active.fmt,
                                  (uint32_t)s_display_active.stride, "FBSNAP")) {
                dump_fb_fmt(s_fbcap_legacy, s_display_active.addr, s_display_active.fmt,
                            (uint32_t)s_display_active.stride);
                fprintf(stderr, "FBSNAP f=%u -> %s\n", s_vcount, s_fbcap_legacy);
                /* Issue #64: pair the captured frame with its route signature so a new
                 * checkpoint is transcribed from the frame the author actually looked at. */
                if (s_route_learn) route_learn_emit(s_vcount, NULL);
            } else {
                fprintf(stderr, "FBSNAP f=%u -> SKIPPED (synchronisation failed)\n", s_vcount);
            }
        }
        extern int sdl3vk_capture_result(void);
        int cres = sdl3vk_capture_result();
        if (cres != 0) {
            fprintf(stderr, "FBSNAP f=%u swapchain capture -> %s (result=%d)\n",
                    s_vcount, s_fbcap_armed, cres);
        } else {
            /* The output cap dropped this frame's present: the arm was cancelled and
             * must not be serviced by a later frame, nor reported with a stale result. */
            fprintf(stderr, "FBSNAP f=%u swapchain capture -> SKIPPED (no present serviced this frame)\n",
                    s_vcount);
        }
        s_fbcap_armed[0] = '\0';
        s_fbcap_legacy[0] = '\0';
    }
    /* The buffer handed to SetFrameBuf is a freshly-completed frame. With SR_FBDUMP=<N>, once N
     * frames have elapsed, the pre-present arm above recorded exactly this presented buffer in
     * present_source.ppm and the process exits with a policy verdict (issue #57): success(0)
     * only if the swapchain-truthful capture really was published, failure(1) otherwise. */
    {
        static int fbdu = -1; static int fbdu_n = 0;
        if (fbdu < 0) {
            const char *fd = getenv("SR_FBDUMP");
            fbdu = sr_fbcap_env_on("SR_FBDUMP");
            fbdu_n = fd ? atoi(fd) : 0;
        }
        if (sync == 0u && fbdu && s_fbcap_armed[0] && s_framebuf &&
            s_vcount >= (uint32_t)fbdu_n &&
            display_host_span_valid(&s_display_active)) {
            extern unsigned long g_ge_pixels;
            extern unsigned long g_tex_samples, g_tex_nonzero;
            extern int sdl3vk_capture_result(void);
            extern const char *sdl3vk_capture_source_label(void);
            int snap_ok = snapshot_sync_ok(s_display_active.addr,
                                            (uint32_t)s_display_active.fmt,
                                            (uint32_t)s_display_active.stride, "SR_FBDUMP");
            if (snap_ok)
                dump_fb_fmt("fb_present.ppm", s_display_active.addr, s_display_active.fmt,
                            (uint32_t)s_display_active.stride);
            /* Also snapshot the whole 2MB eDRAM so any rendered region can be found regardless of which
             * buffer/stride/format the game settled on. */
            FILE *raw = fopen("edram.bin", "wb");
            if (raw) { for (uint32_t a = 0x04000000; a < 0x04200000; a += 4) { uint32_t w = MEM_R32(a); fwrite(&w, 4, 1, raw); } fclose(raw); }
            sr_trace_close();
            fprintf(stderr, "presented frame %u: buf=0x%08x fmt=%u stride=%u ge_pixels=%lu tex_samples=%lu tex_nonzero=%lu\n",
                    s_vcount, s_display_active.addr, s_display_active.fmt,
                    (uint32_t)s_display_active.stride, g_ge_pixels, g_tex_samples, g_tex_nonzero);
            { extern unsigned long g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata;
              fprintf(stderr, "mpeg: ringPut=%lu getAvcAu=%lu avcDecode=%lu noData=%lu\n",
                      g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata); }
            sr_dump_calls();
            extern void sched_dump_threads(void); sched_dump_threads();
            /* Only a capture serviced by this frame's present may affect the exit
             * verdict. An unserviced arm is a failed attempt, never a stale success. */
            int cres = sdl3vk_capture_result();
            fprintf(stderr, "present capture result=%d (source=%s)%s\n", cres,
                    sdl3vk_capture_source_label(),
                    cres == 0 ? " (not serviced: no present this frame)" : "");
            if (!snap_ok) {
                fprintf(stderr, "SR_FBDUMP: no trustworthy framebuffer snapshot was written\n");
                _Exit(1);
            }
            _Exit(sr_fbcap_exit_status(SR_FBCAP_FBDUMP, cres));
        }
    }
    return 0;
}
static uint32_t h_DisplayGetFrameBuf(CpuState *s) {
    if (A3 > 1u) return 0x80000107u;
    const DisplayFrameState *fb = A3 == 1u ? &s_display_latched : &s_display_active;
    if (A0 && !sr_guest_span_writable(A0, 4u)) return 0x80000103u;
    if (A1 && !sr_guest_span_writable(A1, 4u)) return 0x80000103u;
    if (A2 && !sr_guest_span_writable(A2, 4u)) return 0x80000103u;
    if (A0) MEM_W32(A0, fb->addr);
    if (A1) MEM_W32(A1, (uint32_t)fb->stride);
    if (A2) MEM_W32(A2, (uint32_t)fb->fmt);
    return 0;
}

/* Dump a PSP framebuffer to a binary PPM. fmt: 0=5650, 1=5551, 2=4444, 3=8888. */
static void dump_fb_fmt(const char *path, uint32_t fbaddr, int fmt, uint32_t stride) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (!stride) stride = 512;
    fprintf(f, "P6\n480 272\n255\n");
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            unsigned char rgb[3];
            fb_decode_px(fbaddr, fmt, stride, x, y, rgb);
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "dumped framebuffer 0x%08x fmt=%d stride=%u -> %s\n", fbaddr, fmt, stride, path);
}
/* (dump_fb wrapper dropped -- dump_fb_fmt handles all paths.) */

/* Block until the scheduler delivers the next vblank. The old behaviour (delay one tick) let the
 * render loop wake while worker threads were still runnable and redraw the same frame dozens of
 * times per vblank -- the loading screen burned ~50s/60 frames in the rasterizer that way. */
static uint32_t h_DisplayWaitVblank(CpuState *s) {
    (void)s;
    if (ge_log_on()) fprintf(stderr, "HLE: WaitVblank (vcount=%u)\n", s_vcount);
    /* Both sceDisplayWaitVblank (L26/L27) and sceDisplayWaitVblankStart (L34/L35)
     * return CAN_NOT_WAIT here. On hardware these always wait for the NEXT vblank;
     * the per-thread vbl_seen latch that sched_wait_vblank() consults first is a
     * Nakagawa pacing artifact, so the rejection precedes it deliberately. Nothing
     * downstream runs: the latch is not consumed, vbl_seen is not advanced, no
     * thread blocks on VBLANK_WAIT_OBJ, and scheduler virtual time is untouched.
     *
     * The two NIDs share this handler, which is correct for PR-B because both
     * hardware cells agree. They diverge only from interrupt context, where
     * sceDisplayWaitVblank uniquely SUCCEEDS with 1 (L323) while
     * sceDisplayWaitVblankStart returns ILLEGAL_CONTEXT (L325) -- that split needs
     * two handlers and belongs to the interrupt-context work, not here. */
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sched_wait_vblank();
    return 0;
}
static uint32_t h_DisplayGetMode(CpuState *s) {
    if (A0) MEM_W32(A0, 0);  /* mode 0 */
    if (A1) MEM_W32(A1, 480); /* width */
    if (A2) MEM_W32(A2, 272); /* height */
    return 0;
}
uint32_t sr_get_ge_status(void) {
    /* Real PSP GE_STATUS layout (PPSSPP GPU.h GE_STATUS_*):
     *   bit 0x01  BUSY        -- 0 = GE is idle, ready to accept a new list
     *   bit 0x02  DOFLUSH     -- last DRAW op had a flush flag
     *   bit 0x04  DRAWEND     -- a DRAW semantics reached the GE end-of-list
     *   bit 0x08  FINISHEND   -- last FINISH/END command reached by the streamer
     *   bit 0x10  BP2 / intr  -- interrupt pending
     *   bit 0x20  VBLANK      -- currently inside the vsync interval
     *
     * The PSP command-streamer thread (uid 0x115 in our boot) polls this register and gates
     * workload submission on bits 0..4, NOT just on VBLANK. Returning 0x00 outside vblank
     * told the streamer "GE is busy / nothing finished", so it spun forever waiting for a
     * BUSY-clear handshake that only a real HW-cleared FINISH would issue.
     *
     * We bake on the "ready/IDLE" mask (bits 0x08 FINISHEND + 0x10 BP2           = 0x18)
     * and OR in VBLANK on the 1.5 ms cycle. bit 0x01 BUSY stays cleared, so any future
     * "is the GE idle?" check passes. If the guest ever reads this register mid-frame
     * the FINISHEND flag stays set; the previous-frame-finished-so-flip semantics that the
     * driver relies on keep working. */
    uint32_t vblank   = sched_display_is_vblank() ? 0x20u : 0x0u;
    return 0x18u | vblank;
}
static uint32_t h_DisplayIsVblank(CpuState *s) {
    (void)s;
    return (sr_get_ge_status() & 0x20) ? 1 : 0;
}
/* sceDisplayGetFramePerSec returns the PSP display refresh rate as a
 * single-precision float through $f0 (the MIPS float-return convention); the
 * integer $v0 status is what the handler returns.  The value is the same
 * 60000/1001 rational the scheduler uses for its display phase, evaluated as
 * float == 0x426fc29f bits (59.9400599f, PSP-3001/6.61-ARK measured anchor).
 * The previous h_ok registration returned 0 in $v0 but left $f0 stale, so a
 * guest reading the float received poisoned state (issue #80 display-clock
 * campaign). */
static uint32_t h_DisplayGetFramePerSec(CpuState *s) {
    (void)s;
    s->f[0] = 60000.0f / 1001.0f;
    return 0;
}

/* Called once per delivered VBLANK... */
void sr_ctrl_sample(void);
void ge_set_frame(uint32_t frame);
void ge_finish_latch_assist(void);   /* defined below (ge_finish_callback area) */

/* Guest-visible VCOUNT advances by elapsed display periods at scheduler
 * source-latch boundaries, decoupled from VBLANK service -- it is not the count
 * of delivered/serviced VBLANK episodes.  Reads remain observational: this is
 * deliberately not described as a strictly free-running register.  The source
 * latch advances the counter by the number of periods it just coalesced.
 * deliver_vblank() keeps calling sr_vblank_tick() exactly once per serviced
 * event; that service path must not also increment VCOUNT or a coalesced burst
 * would be double-counted. */
void sr_display_advance_vcount(uint32_t elapsed_periods) {
    s_vcount += elapsed_periods;
    s_vcount_fwd = s_vcount;
}
void sr_vblank_tick(void) {
    if (s_display_latched_pending) {
        s_display_active = s_display_latched;
        s_display_latched_pending = 0;
        s_display_configured = 1;
        s_framebuf = s_display_active.addr;
        s_last_flip_vcount = s_vcount;
        s_watchdog_bucket = 0;
        display_present_active();
    }
    if (ge_log_on() && (s_vcount & 0x3f) == 0)
        fprintf(stderr, "VBLANK tick %u\n", s_vcount);
    /* Full guest-PC dumps contain thousands of rows and synchronous five-second cadence
     * materially distorts the route being profiled. Keep periodic capture opt-in and let the
     * canonical manager choose a bounded default for runs that may be force-stopped before
     * atexit. Direct SR_PROFILE users otherwise get the normal exit dump only. */
    static uint32_t profile_dump_period = UINT32_MAX;
    if (profile_dump_period == UINT32_MAX) {
        const char *period = getenv("SR_PROFILE_DUMP_VBLANKS");
        unsigned long parsed = period && period[0] ? strtoul(period, NULL, 10) : 0;
        profile_dump_period = parsed > UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)parsed;
    }
    if (profile_dump_period > 0 && (s_vcount % profile_dump_period) == 0) {
        extern void sr_profile_dump(void);
        sr_profile_dump();
    }
    ge_set_frame(s_vcount);

    /* Issue #64: advance the state-qualified route before the controller sample is
     * latched, so a checkpoint observed on this vblank releases its press on this vblank
     * rather than one frame late. */
    route_tick(s_vcount_fwd);
    sr_ctrl_sample();   /* latch one controller sample per frame (PPSSPP ring semantics) */
    /* Last-resort un-wedge for the frame-ready latch: title-qualified.
     * Generic PSP has no such latch; HST's render loop gates presentation on
     * MEM[frame_latch]. If it has been stuck above 0 for a sustained stretch
     * with no list completing to clear it, force it down. This mirrors the
     * assist in ge_finish_callback but covers lists that never reach a finish
     * callback. Only when the title configures the latch (issue #98 #5).
     * Retirement: replace the timer hack with the real guest/runtime event it
     * approximates (list completion with no registered callback) once that event
     * is modeled generically. */
    {
        static uint32_t latch_stuck = 0;
        uint32_t latch;
        if (!sr_title_config_frame_latch_addr(&latch) ||
            !sr_guest_span_readable(latch, 4) || !sr_guest_span_writable(latch, 4)) {
            latch_stuck = 0;
        } else {
            uint32_t lc = MEM_R32(latch);
            if (lc > 0) {
                if (++latch_stuck > 30u) { ge_finish_latch_assist(); latch_stuck = 0; }
            } else {
                latch_stuck = 0;
            }
        }
    }
    /* No-frame watchdog: vblanks keep being delivered even when every game thread is
     * blocked, so a stretch with no new sceDisplaySetFrameBuf is a NO-NEW-FLIP
     * observation. It is not by itself a hang, scheduler-stall, or scene-transition
     * verdict: a legitimately static scene (e.g. a save-confirmation modal waiting for
     * user input) also stops presenting. The display-outcome counters, thread wait
     * state, and movie-stack activity below are the facts that let a human classify
     * the stretch.
     * SR_WATCHDOG_EXIT=<N>: abort after N vblanks with no new frame (default: no abort). */
    uint32_t diff = s_vcount - s_last_flip_vcount;
    /* s_vcount is monotonic for the life of the process and s_last_flip_vcount
     * is only ever assigned from it, so this unsigned difference is a true
     * elapsed-period count (it cannot wrap short of 2^32 periods, ~2.2 years at
     * 60 Hz) and the bucket index below is monotonic within a stretch. */
    uint32_t bucket = diff / 600u;
    if (bucket > s_watchdog_bucket) {
        s_watchdog_bucket = bucket;
        s_watchdog_fires++;
        fprintf(stderr,
                "WATCHDOG: no new frame presented for %u vblanks (~%us) - neutral "
                "NO-NEW-FLIP observation, not by itself a hang/stall verdict\n",
                diff, diff / 60);
        fprintf(stderr,
                "BOOT_EVENT phase=stalled observation=no_new_flip no_frame_vblanks=%u seconds=%u\n",
                diff, diff / 60);
        /* Which side of the display handoff stopped presenting: did the guest stop
         * asking for flips, or are its requests being refused? last_vcount vs
         * s_last_flip_vcount separates "no call since the last flip" from "calls
         * that did not present". */
        fprintf(stderr,
                "WATCHDOG_DISPLAY: calls=%lu immediate=%lu latched=%lu rejected=%lu "
                "last_call_v=%u last_req=0x%08x/%d/%d sync=%u "
                "last_err=0x%08x@v%u req=0x%08x/%d/%d sync=%u pending=%d active=0x%08x/%d/%d\n",
                s_setfb.calls, s_setfb.immediate, s_setfb.latched, s_setfb.rejected,
                s_setfb.last_vcount, s_setfb.last_addr, s_setfb.last_stride,
                s_setfb.last_fmt, s_setfb.last_sync,
                s_setfb.last_err, s_setfb.last_err_vcount, s_setfb.last_err_addr,
                s_setfb.last_err_stride, s_setfb.last_err_fmt, s_setfb.last_err_sync,
                s_display_latched_pending, s_display_active.addr,
                s_display_active.stride, s_display_active.fmt);
        /* Thread wait state: a parked thread's reason is read from the dump (wait
         * reason/deadline/callback/wakeup/join columns), not from a stale wait_obj. */
        { extern void sched_dump_threads(void); sched_dump_threads(); }
        /* Movie-stack activity: sceMpeg and scePsmfPlayer are separate libraries; these
         * counters say whether either stack was being fed. */
        extern unsigned long g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata;
        fprintf(stderr, "WATCHDOG_MPEG: put=%lu getavc=%lu avcdec=%lu nodata=%lu\n",
                g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata);
        fprintf(stderr, "WATCHDOG_PSMF: player_calls=%lu getvideo=%lu getaudio=%lu\n",
                s_psmf_calls, s_psmf_getvideo, s_psmf_getaudio);
        fflush(stderr);
        { static int wde = -1;
          if (wde < 0) { const char *e = getenv("SR_WATCHDOG_EXIT"); wde = e ? atoi(e) : 0; }
          if (wde > 0 && diff >= (uint32_t)wde) {
              fprintf(stderr, "WATCHDOG: aborting after %u vblanks with no new frame (SR_WATCHDOG_EXIT=%d)\n", diff, wde);
              _Exit(1);
          }
        }
    }
    /* Frame capture happens at sceDisplaySetFrameBuf (the instant a finished frame is presented),
     * which is the correct moment -- snapshotting here at an arbitrary vblank catches a buffer the
     * game has already begun clearing for the next frame. */

    /* SR_EXIT_AT_VBLANK=<V>: terminate cleanly once the guest has delivered V vblanks.
     *
     * A scripted route's useful work ends at its last press plus a settle window, but the only
     * stop control was -Duration, a wall-clock guess. Guessing low silently truncates the route
     * before its last inputs fire; guessing high burns minutes replaying a finished scene. This
     * makes the stop condition the same quantity the route is written in, so a route that ends
     * at vblank 41,200 stops there on every machine regardless of how fast it ran.
     *
     * PLACEMENT (exact, not "fully accounted"): this is the LAST statement of the tick, so
     * vblank V is complete in every respect this function is responsible for -- the frame
     * counter is advanced, ge_set_frame(V) has run, sr_ctrl_sample() has latched V's controller
     * sample (so a pad-script press scheduled for V is delivered before the exit), and the latch
     * assist and no-frame watchdog have run. What it does NOT wait for is work that happens outside
     * sr_vblank_tick: guest threads resumed by this vblank run after it returns, and a frame
     * whose sceDisplaySetFrameBuf lands later in vblank V is neither presented nor captured. So
     * a route's last input must be scheduled comfortably before V -- a settle window of a few
     * hundred vblanks -- and the last capture of interest must come from an earlier vblank.
     *
     * It ends the process; it never skips guest work, changes pacing, or touches rendering.
     * Exit status 0 -- reaching the requested vblank is success, unlike the watchdog abort. */
    {
        static uint32_t exit_at = 0; static int exit_init = 0;
        if (!exit_init) {
            exit_init = 1;
            const char *e = getenv("SR_EXIT_AT_VBLANK");
            unsigned long v = e && e[0] ? strtoul(e, NULL, 10) : 0ul;
            exit_at = v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
        }
        if (exit_at && s_vcount >= exit_at) {
            fprintf(stderr, "BOOT_EVENT phase=exit_at_vblank vblanks=%u (SR_EXIT_AT_VBLANK=%u)\n",
                    s_vcount, exit_at);
            if (audio_stat_on()) {
                fprintf(stderr,
                        "AUDIOSTAT_ATRAC: frames=%lu nonzero=%lu peak=%d\n",
                        g_atrac_frames, g_atrac_frames_nonzero, g_atrac_peak);
                fprintf(stderr,
                        "AUDIOSTAT_SAS: calls=%lu withmix=%lu pre_nonzero=%lu post_nonzero=%lu erased=%lu pre_peak=%d post_peak=%d no_voice=%lu no_voice_overwrite=%lu\n",
                        g_sas_calls, g_sas_calls_add, g_sas_pre_nonzero,
                        g_sas_post_nonzero, g_sas_erased, g_sas_pre_peak, g_sas_post_peak,
                        g_sas_no_voice, g_sas_no_voice_overwrite);
                fprintf(stderr, "AUDIOSTAT_BUF: atrac_out n=%d", g_atrac_out_nbufs);
                for (int i = 0; i < g_atrac_out_nbufs; i++)
                    fprintf(stderr, " 0x%08x", g_atrac_out_bufs[i]);
                fprintf(stderr, "\nAUDIOSTAT_BUF: sas_out n=%d", g_sas_nbufs);
                for (int i = 0; i < g_sas_nbufs; i++)
                    fprintf(stderr, " 0x%08x", g_sas_bufs[i]);
                fprintf(stderr, "\n");
                for (uint32_t c = 0; c < 9u; c++) {
                    if (!g_audio_nbufs[c]) continue;
                    fprintf(stderr, "AUDIOSTAT_BUF: ch=%u n=%d", c, g_audio_nbufs[c]);
                    for (int i = 0; i < g_audio_nbufs[c]; i++)
                        fprintf(stderr, " 0x%08x", g_audio_bufs[c][i]);
                    fprintf(stderr, "\n");
                }
#ifndef SR_HLE_THREAD_SELFTEST
                /* audio.c is not linked into the executable HLE harness. */
                extern void sr_audio_dump_stats(void);
                sr_audio_dump_stats();
#endif
            }
            sr_dump_calls();
            fflush(stderr);
            fflush(stdout);
            _Exit(0);
        }
    }
}
static uint32_t h_DisplayGetVcount(CpuState *s) { (void)s; return s_vcount; }
/* The PSP scans hCountPerVblank=286 lines per frame (PPSSPP Core/HW/Display.cpp). The
 * scheduler owns the rational 59.94-Hz phase, so these observations advance with
 * elapsed guest time and remain unchanged when a game polls them repeatedly. */
static uint32_t h_DisplayGetCurrentHcount(CpuState *s) {
    (void)s;
    return sched_display_current_hcount();
}
static uint32_t h_DisplayGetAccumulatedHcount(CpuState *s) {
    (void)s;
    return sched_display_accumulated_hcount();
}

/* sceGe_user: pretend the GE finishes immediately (DrawSync returns done). eDRAM at 0x04000000. */
typedef struct {
    int used;
    uint32_t signal_func, signal_arg;
    uint32_t finish_func, finish_arg;
} GeCallback;
static GeCallback s_ge_cb[16];
static uint32_t s_ge_list_next = 0;

/* Every nested guest call this runtime makes -- GE callbacks here and the MPEG
 * ring-refill callback in mpeg.c -- runs on this single hard-coded guest stack
 * address rather than on the calling thread's stack.  Naming it does not change
 * that; it makes the shared-stack property greppable and gives the executable
 * regression one place to read the value from.  Whether the PSP shares a stack
 * across nested guest calls is NOT established: see the callback-ABI regression
 * in hle_thread_selftest.c, which measures what this runtime does today. */
#define SR_CALL_GUEST_STACK 0x09df8000u

static void ge_call_guest(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!fn) { fprintf(stderr, "GE_CALL_GUEST: fn=0x0 (null, skipping)\n"); return; }
    if (ge_log_on())
        fprintf(stderr, "GE_CALL_GUEST: fn=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x cur_uid=0x%x\n",
                fn, a0, a1, a2, sched_current_uid());
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    int32_t save_slice = atomic_load_explicit(&sr_timeslice, memory_order_relaxed);
    memset(s, 0, sizeof(CpuState));
    s->r[4] = a0;
    s->r[5] = a1;
    s->r[6] = a2;
    s->r[28] = save.r[28];
    s->r[29] = SR_CALL_GUEST_STACK;
    s->r[31] = 0;
    s->vfpuCtrl[0] = 0xe4; s->vfpuCtrl[1] = 0xe4;
    s->pc = fn;
    atomic_store_explicit(&sr_timeslice, 20000, memory_order_relaxed);
    dispatch(s, fn);
    if (ge_log_on())
        fprintf(stderr, "GE_CALL_GUEST: fn=0x%08x returned, v0=0x%08x\n", fn, s->r[2]);
    memcpy(s, &save, sizeof(CpuState));
    atomic_store_explicit(&sr_timeslice, save_slice, memory_order_relaxed);
}

/* Like ge_call_guest but returns the guest function's v0 (r2). Used by HLE
 * stubs that must extract a value from a guest constructor (e.g. the guest
 * malloc f_00000bcc returns the allocated block in r2). */
static uint32_t ge_call_guest_rv(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!fn) { fprintf(stderr, "GE_CALL_GUEST_RV: fn=0x0 (null, skipping)\n"); return 0; }
    if (ge_log_on())
        fprintf(stderr, "GE_CALL_GUEST_RV: fn=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x cur_uid=0x%x\n",
                fn, a0, a1, a2, sched_current_uid());
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    int32_t save_slice = atomic_load_explicit(&sr_timeslice, memory_order_relaxed);
    memset(s, 0, sizeof(CpuState));
    s->r[4] = a0;
    s->r[5] = a1;
    s->r[6] = a2;
    s->r[28] = save.r[28];
    s->r[29] = SR_CALL_GUEST_STACK;
    s->r[31] = 0;
    s->vfpuCtrl[0] = 0xe4; s->vfpuCtrl[1] = 0xe4;
    s->pc = fn;
    atomic_store_explicit(&sr_timeslice, 20000, memory_order_relaxed);
    dispatch(s, fn);
    uint32_t rv = s->r[2];
    if (ge_log_on())
        fprintf(stderr, "GE_CALL_GUEST_RV: fn=0x%08x returned, v0=0x%08x\n", fn, rv);
    memcpy(s, &save, sizeof(CpuState));
    atomic_store_explicit(&sr_timeslice, save_slice, memory_order_relaxed);
    return rv;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Reach the production nested-guest-call marshalling from the executable
 * selftest.  ge_call_guest_rv() is static and its callers all need real GE or
 * MPEG state, so without this hook the only way to characterise the callback
 * ABI would be to re-implement it in the test -- which would measure the copy,
 * not the contract.  This adds no production behaviour: it is a call-through,
 * compiled only into the test executable. */
uint32_t sr_hle_test_call_guest(CpuState *s, uint32_t fn,
                                uint32_t a0, uint32_t a1, uint32_t a2) {
    return ge_call_guest_rv(s, fn, a0, a1, a2);
}
/* The nested-call scratch stack is a single hard-coded guest address shared by
 * every such call.  Expose it so the regression states the measured value once
 * instead of duplicating the literal. */
uint32_t sr_hle_test_call_guest_stack(void) { return SR_CALL_GUEST_STACK; }
#endif /* SR_HLE_THREAD_SELFTEST */

/* sceDisplaySetMode: on the real PSP this triggers the display driver to
 * initialise the vblank device (written to MEM[0x34B328]) and the render
 * context (global MEM[0x2CFC8C] -> struct at 0x31FC40, whose +0x80 must equal
 * 0x308). The recompiled game never reaches its own display-init path because
 * this syscall was a no-op, so the main render loop (f_00046dec / L_00046f70)
 * spins forever on two unmet conditions:
 *   1. f_000487f4's chk7 requires MEM[MEM[0x2CFC8C]+0x80] == 0x308 (it returns
 *      0 while 0x2CFC8C is still 0, because the render-context init f_0001dc00
 *      was never dispatched).
 *   2. The second poll waits on MEM[0x331B80] (frame-ready counter) which is
 *      never set.
 * We replay the game-owned init by dispatching the real constructors instead of
 * faking the structs by hand. Verified by build/run: expect the first frame to
 * be presented and the intro/menu to become reachable. */
/* The engine's allocator/render queues enter and leave a short critical section
 * through two callbacks in the global configuration block at 0x333138.  The
 * normal game initializer (f_00081a04) selects the exact pair below from the
 * threading mode at +0x30.  DisplaySetMode is currently the earliest safe
 * point in the native runtime's game-owned initialization replay; seed only a
 * missing pair here, reproducing that initializer's branch table verbatim.
 *
 * Keeping this mapping next to the replay is important: dispatching a null
 * callback happens to be tolerated by the recompiler, but silently skips the
 * critical section and leaves the worker spinning with no display list. */
static void ensure_runtime_sync_callbacks(CpuState *s);

static uint32_t h_DisplaySetMode(CpuState *s) {
    uint32_t mode = s->r[4], width = s->r[5], height = s->r[6];
    (void)mode; (void)width; (void)height;
    fprintf(stderr, "DISPLAY_SET_MODE: mode=%u %ux%u (title %s)\n", mode, width, height,
            sr_title_config()->source_id);
    fflush(stderr);

    ensure_runtime_sync_callbacks(s);

    SrTitleDisplayBringup bringup;
    if (!sr_title_config_display_bringup(&bringup)) {
        fprintf(stderr,
                "DISPLAY_SET_MODE: display bringup not configured (generic PSP semantics, title %s)\n",
                sr_title_config()->source_id);
        return 0; /* Generic PSP: mode/width/height already recorded via generic path if needed */
    }

    if (!sr_guest_span_writable(bringup.render_context_magic_addr, 4) ||
        !sr_guest_span_writable(bringup.render_table_ready_flag_addr, 1) ||
        !sr_guest_span_writable(bringup.render_context_word_addr, 4)) {
        fprintf(stderr,
                "DISPLAY_SET_MODE: bringup data addrs not writable (title %s), skipping bringup\n",
                sr_title_config()->source_id);
        return 0;
    }

    fprintf(stderr, "DISPLAY_SET_MODE: replaying title display-driver init (title %s)\n",
            sr_title_config()->source_id);

    /* Vblank device: allocate the device struct and let the real creator
     * (guest's vblank device init, normally reached via guest's init dispatcher)
     * populate it. Title-qualified addresses. */
    uint32_t dev = ge_call_guest_rv(s, bringup.malloc_entry, 0x40u, 0, 0);
    if (dev) {
        ge_call_guest(s, bringup.vblank_device_init_entry, dev, 0, 0);
    } else {
        fprintf(stderr, "DISPLAY_SET_MODE: guest malloc returned 0; skipping vblank device init (title %s)\n",
                sr_title_config()->source_id);
    }

    /* Render context: guest render-context init fully initialises the context
     * struct (including the +0x80 magic the poller checks). This is the gate
     * that unblocks the file load path. */
    ge_call_guest(s, bringup.render_context_init_entry, 0, 0, 0);

    /* Guarantee magic even if loader state differs. */
    if (MEM_R32(bringup.render_context_magic_addr) != 0x308u) {
        fprintf(stderr, "DISPLAY_SET_MODE: forcing render-context magic 0x%08x -> 0x308 (title %s)\n",
                bringup.render_context_magic_addr, sr_title_config()->source_id);
        MEM_W32(bringup.render_context_magic_addr, 0x308u);
    }

    /* Seed the frame-ready counter so the second render-loop wait clears and
     * the loop proceeds to present frame 0. Title-qualified: only when the
     * frame latch is configured. */
    uint32_t latch;
    if (sr_title_config_frame_latch_addr(&latch)) {
        if (sr_guest_span_writable(latch, 4)) {
            MEM_W32(latch, 1u);
            fprintf(stderr, "DISPLAY_SET_MODE: seeded frame latch 0x%08x <- 1 (title %s)\n",
                    latch, sr_title_config()->source_id);
        } else {
            fprintf(stderr,
                    "DISPLAY_SET_MODE: frame latch 0x%08x not writable, not seeding (title %s)\n",
                    latch, sr_title_config()->source_id);
        }
    } else {
        fprintf(stderr,
                "DISPLAY_SET_MODE: frame latch not configured, not seeding (title %s)\n",
                sr_title_config()->source_id);
    }

    /* Render-command table ready flag and context word. On real PSP a
     * background thread fills the table; our HLE has no background thread. */
    MEM_W8(bringup.render_table_ready_flag_addr, 1u);
    MEM_W32(bringup.render_context_word_addr, 1u);
    fprintf(stderr,
            "DISPLAY_SET_MODE: seeded table ready 0x%08x <-1 and ctx 0x%08x <-1 (title %s)\n",
            bringup.render_table_ready_flag_addr, bringup.render_context_word_addr,
            sr_title_config()->source_id);

    return 0; /* SCE_DISPLAY_SET_MODE_SUCCESS */
}

/* The engine's render loop gates "is a frame ready to present?" on a title-
 * specific counter (HST: MEM[0x331b80], decremented once per completed display
 * list). Title-qualified: the latch address is configuration. Generic titles
 * use the generic GE completion path without latch assist. */
static void ge_finish_callback(CpuState *s, uint32_t cbid, uint32_t list_id, uint32_t user_arg) {
    const size_t cb_max = sizeof(s_ge_cb) / sizeof(s_ge_cb[0]);
    uint32_t latch;
    int has_latch = sr_title_config_frame_latch_addr(&latch) &&
                    sr_guest_span_readable(latch, 4) && sr_guest_span_writable(latch, 4);
    if (cbid >= (uint32_t)cb_max) {
        fprintf(stderr, "GE_FINISH_CB: cbid=%u OUT OF RANGE (max %zu) -- no guest cb%s\n",
                cbid, cb_max, has_latch ? ", advancing latch" : " (generic: no latch)");
        if (has_latch) ge_finish_latch_assist();
        return;
    }
    GeCallback *cb = &s_ge_cb[cbid];
    if (!cb->used || !cb->finish_func) {
        fprintf(stderr, "GE_FINISH_CB: cbid=%u used=%d fn=0x%08x (SKIPPED - %s)%s\n",
                cbid, cb->used, cb->finish_func, !cb->used ? "not registered" : "no function",
                has_latch ? " -- advancing latch" : " (generic: no latch)");
        if (has_latch) ge_finish_latch_assist();
        return;
    }
    if (ge_log_on())
        fprintf(stderr, "GE_FINISH_CB: cbid=%u list=0x%08x fn=0x%08x arg=0x%08x\n",
                cbid, list_id, cb->finish_func, cb->finish_arg ? cb->finish_arg : user_arg);
    uint32_t pre_ctr = has_latch ? MEM_R32(latch) : 0u;
    ge_call_guest(s, cb->finish_func, list_id, cb->finish_arg ? cb->finish_arg : user_arg, cbid);
    if (has_latch) {
        uint32_t post_ctr = MEM_R32(latch);
        if (ge_log_on())
            fprintf(stderr, "GE_FINISH_CB: counter 0x%08x before=%u after=%u\n", latch, pre_ctr, post_ctr);
        if (post_ctr == pre_ctr && pre_ctr > 0) {
            MEM_W32(latch, pre_ctr - 1u);
            if (ge_log_on())
                fprintf(stderr, "GE_FINISH_CB: counter 0x%08x forcibly decremented to %u\n", latch, pre_ctr - 1u);
        }
    }
    if (ge_log_on())
        fprintf(stderr, "GE_FINISH_CB: callback fn=0x%08x returned\n", cb->finish_func);
}

/* Guarantee the frame-ready latch makes progress on every completed list,
 * even when no guest finish callback ran. Title-qualified: only when the
 * frame latch address is configured and writable; generic titles have no
 * latch to assist. Called from ge_finish_callback and sr_vblank_tick. */
void ge_finish_latch_assist(void) {
    uint32_t latch;
    if (!sr_title_config_frame_latch_addr(&latch)) return;
    if (!sr_guest_span_readable(latch, 4) || !sr_guest_span_writable(latch, 4)) return;
    uint32_t pre = MEM_R32(latch);
    if (pre > 0) {
        MEM_W32(latch, pre - 1u);
        if (ge_log_on())
            fprintf(stderr, "GE_FINISH_LATCH_ASSIST: 0x%08x %u -> %u (title %s)\n",
                    latch, pre, pre - 1u, sr_title_config()->source_id);
    }
}

#define GE_LIST_MAX 64
typedef struct {
    uint32_t uid;
    uint32_t start_pc;
    uint32_t current_pc;
    uint32_t stall_addr;
    uint32_t cbid;
    uint32_t cbarg;
    int status; // 0 = idle/free, 1 = stalled, 2 = completed
} GeListInfo;

static GeListInfo s_ge_lists[GE_LIST_MAX];

static uint32_t h_GeListEnQueue(CpuState *s) {
    uint32_t list = A0;
    uint32_t stall = A1;
    uint32_t cbid = A2;
    uint32_t cbarg = A3;
    uint32_t list_id = 0x35000000u | (s_ge_list_next++ & 0x00ffffffu);

    ge_enqueue_trace_emit(s, "enqueue", list_id, list, stall, cbid);

    int slot = -1;
    for (int i = 0; i < GE_LIST_MAX; i++) {
        if (s_ge_lists[i].status == 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        slot = 0;
    }

    s_ge_lists[slot].uid = list_id;
    s_ge_lists[slot].start_pc = list;
    s_ge_lists[slot].current_pc = list;
    s_ge_lists[slot].stall_addr = stall;
    s_ge_lists[slot].cbid = cbid;
    s_ge_lists[slot].cbarg = cbarg;
    s_ge_lists[slot].status = 1;

    /* stall == list means the ring buffer is empty â€” the game will fill it and advance the
     * stall via sceGeListUpdateStallAddr. Running it now would read uninitialized eDRAM;
     * just record as stalled and let UpdateStallAddr drive it.
     * Also treat stall == 0 as "run to completion" (no stall fence). */
    if (stall != 0 && stall == list) {
        ge_enqueue_trace_result(s, "enqueue", list_id, "deferred", list, 0);
        if (ge_log_on())
            fprintf(stderr, "GE_ENQ: list_id=0x%08x list=0x%08x stall=0x%08x cbid=%u -> DEFERRED (stall==list)\n",
                    list_id, list, stall, cbid);
        return list_id;
    }

    g_ge_stall_addr = stall;
    uint32_t next_pc = ge_run_list(list, 0);
    g_ge_stall_addr = 0;

    ge_enqueue_trace_result(s, "enqueue", list_id,
                            next_pc == 0 ? "done" : "stalled",
                            next_pc, next_pc == 0);

    if (ge_log_on())
        fprintf(stderr, "GE_ENQ: list_id=0x%08x list=0x%08x stall=0x%08x cbid=%u -> next_pc=0x%08x %s\n",
                list_id, list, stall, cbid, next_pc, next_pc == 0 ? "(DONE)" : "(stalled)");

    if (next_pc == 0) {
        s_ge_lists[slot].status = 2; // completed
        ge_finish_callback(s, cbid, list_id, cbarg);
    } else {
        /* List stalled at a non-start stall â€” game will advance via UpdateStallAddr.
         * Do NOT drain here: that runs past the game's write head into uninitialized eDRAM. */
        s_ge_lists[slot].current_pc = next_pc;
        /* status stays 1 (stalled) */
    }

    return list_id;
}

static uint32_t h_GeListUpdateStallAddr(CpuState *s) {
    uint32_t list_id = A0;
    uint32_t new_stall = A1;

    int slot = -1;
    for (int i = 0; i < GE_LIST_MAX; i++) {
        if (s_ge_lists[i].uid == list_id && s_ge_lists[i].status == 1) {
            slot = i;
            break;
        }
    }

    ge_enqueue_trace_emit(s, "update_stall", list_id,
                          slot >= 0 ? s_ge_lists[slot].start_pc : 0u,
                          new_stall,
                          slot >= 0 ? s_ge_lists[slot].cbid : 0u);

    if (slot == -1) {
        ge_enqueue_trace_result(s, "update_stall", list_id, "not_found", 0u, 0);
        if (ge_log_on())
            fprintf(stderr, "GE_UPDATE_STALL: list_id=0x%08x NOT FOUND or not stalled\n", list_id);
        return 0;
    }

    if (ge_log_on())
        fprintf(stderr, "GE_UPDATE_STALL: list_id=0x%08x stall=0x%08x cur_pc=0x%08x -> new_stall=0x%08x\n",
                list_id, s_ge_lists[slot].stall_addr, s_ge_lists[slot].current_pc, new_stall);

    s_ge_lists[slot].stall_addr = new_stall;
    g_ge_stall_addr = new_stall;

    uint32_t next_pc = ge_run_list(s_ge_lists[slot].current_pc, 1); // 1 = resume
    g_ge_stall_addr = 0;

    ge_enqueue_trace_result(s, "update_stall", list_id,
                            next_pc == 0 ? "done" : "stalled",
                            next_pc, next_pc == 0);

    if (ge_log_on())
        fprintf(stderr, "GE_UPDATE_STALL: ge_run_list returned 0x%08x %s\n",
                next_pc, next_pc == 0 ? "(COMPLETED)" : "(stalled)");

    if (next_pc == 0) {
        s_ge_lists[slot].status = 2; // completed
        if (ge_log_on())
            fprintf(stderr, "GE_UPDATE_STALL: list DONE, firing finish callback cbid=%u\n", s_ge_lists[slot].cbid);
        ge_finish_callback(s, s_ge_lists[slot].cbid, list_id, s_ge_lists[slot].cbarg);
    } else {
        s_ge_lists[slot].current_pc = next_pc;
    }

    return 0;
}

static uint32_t h_GeListSync(CpuState *s) {
    uint32_t qid = A0;
    uint32_t syncType = A1;
    for (int i = 0; i < GE_LIST_MAX; i++) {
        if (s_ge_lists[i].uid == qid) {
            if (ge_log_on())
                fprintf(stderr, "GE_SYNC: qid=0x%08x syncType=%u status=%d (cur_pc=0x%08x)\n",
                        qid, syncType, s_ge_lists[i].status, s_ge_lists[i].current_pc);
            if (s_ge_lists[i].status == 2) return 0;
            if (s_ge_lists[i].status == 1) {
                if (syncType == 1) return 1;
                sched_delay_current(1000);
                return 0;
            }
        }
    }
    if (ge_log_on())
        fprintf(stderr, "GE_SYNC: qid=0x%08x NOT FOUND in list table\n", qid);
    return 0;
}

static uint32_t h_GeUnsetCallback(CpuState *s) {
    uint32_t cbid = A0;
    if (cbid < (uint32_t)(sizeof(s_ge_cb) / sizeof(s_ge_cb[0]))) {
        s_ge_cb[cbid].used = 0;
    }
    if (getenv("SR_GELOG")) {
        fprintf(stderr, "sceGeUnsetCallback: cbid=%u\n", cbid);
    }
    return 0;
}

static uint32_t h_GeEdramGetSize(CpuState *s) {
    (void)s;
    return 0x00200000u;
}
static uint32_t h_GeDrawSync(CpuState *s) { (void)s; return 0; }
static uint32_t h_GeEdramGetAddr(CpuState *s) { (void)s; return 0x04000000; }
static uint32_t h_GeSetCallback(CpuState *s) {
    uint32_t info = A0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(s_ge_cb) / sizeof(s_ge_cb[0])); i++) {
        if (!s_ge_cb[i].used) {
            s_ge_cb[i].used = 1;
            s_ge_cb[i].signal_func = info ? MEM_R32(info + 0) : 0;
            s_ge_cb[i].signal_arg  = info ? MEM_R32(info + 4) : 0;
            s_ge_cb[i].finish_func = info ? MEM_R32(info + 8) : 0;
            s_ge_cb[i].finish_arg  = info ? MEM_R32(info + 12) : 0;
            if (ge_log_on())
                fprintf(stderr, "GE_SET_CB: cbid=%u sig=0x%08x/0x%08x fin=0x%08x/0x%08x\n",
                        i, s_ge_cb[i].signal_func, s_ge_cb[i].signal_arg,
                        s_ge_cb[i].finish_func, s_ge_cb[i].finish_arg);
            return i;
        }
    }
    return 0xffffffffu;
}

/* ---- sceSasCore: bounded, stateful voice mixer -------------------------------------
 *
 * The real PSP keeps a 3616-byte SceSasCore object in guest memory.  The native
 * implementation keeps the decoded state host-side, but the guest pointer is still a
 * load-bearing handle: every operation validates the same aligned, readable core span
 * and the initialized core identity before touching a voice.  This prevents the old
 * ``A1 & 31`` aliases from turning malformed calls into writes to another voice.
 *
 * The mixer is intentionally modest (VAG ADPCM, mono PCM, deterministic noise and a
 * simple ADSR), but all successful setters retain the state consumed by later queries or
 * mixing.  Unsupported waveform/ATRAC3 operations return a controlled SAS error instead
 * of fabricating success. */
#define SAS_VOICES 32
#define SAS_CORE_BYTES 3616u
#define SAS_GRAIN_MIN 64
#define SAS_GRAIN_MAX 2048
#define SAS_SAMPLE_RATE 44100
#define SAS_VOLUME_MAX 0x1000
#define SAS_PITCH_MIN 1
#define SAS_PITCH_MAX 0x4000
#define SAS_NOISE_FREQ_MAX 0x3f
#define SAS_ENVELOPE_MAX 0x40000000
#define SAS_ADSR_ATTACK  0x1u
#define SAS_ADSR_DECAY   0x2u
#define SAS_ADSR_SUSTAIN 0x4u
#define SAS_ADSR_RELEASE 0x8u
#define SAS_ADSR_ALL     (SAS_ADSR_ATTACK | SAS_ADSR_DECAY | SAS_ADSR_SUSTAIN | SAS_ADSR_RELEASE)

/* PSPSDK's public error values (pspsascore.h).  Keep these in one place so a rejected
 * call is distinguishable from a successful no-op in both production and selftests. */
#define SAS_ERROR_ADDRESS       0x80420005u
#define SAS_ERROR_VOICE_INDEX   0x80420010u
#define SAS_ERROR_NOISE_CLOCK   0x80420011u
#define SAS_ERROR_PITCH_VAL     0x80420012u
#define SAS_ERROR_ADSR_MODE     0x80420013u
#define SAS_ERROR_ADPCM_SIZE    0x80420014u
#define SAS_ERROR_LOOP_MODE     0x80420015u
#define SAS_ERROR_INVALID_STATE 0x80420016u
#define SAS_ERROR_VOLUME_VAL    0x80420018u
#define SAS_ERROR_ADSR_VAL      0x80420019u
#define SAS_ERROR_FX_TYPE       0x80420020u
#define SAS_ERROR_FX_FEEDBACK   0x80420021u
#define SAS_ERROR_FX_DELAY      0x80420022u
#define SAS_ERROR_FX_VOLUME_VAL 0x80420023u
#define SAS_ERROR_NOTINIT       0x80420100u
#define SAS_ERROR_ALRDYINIT     0x80420101u

typedef enum {
    SAS_VOICE_OFF = 0,
    SAS_VOICE_VAG,
    SAS_VOICE_PCM,
    SAS_VOICE_NOISE,
    SAS_VOICE_ATRAC3,
} SasVoiceType;

typedef struct {
    int on;                       /* keyed on and source not exhausted */
    int paused;                   /* playback pause bit */
    SasVoiceType type;            /* explicit source kind; never inferred from vag */
    uint32_t vag, vag_size;       /* VAG stream base and byte size */
    uint32_t pos;                 /* byte offset of the next 16-byte block */
    int loop_requested;           /* guest loop policy; data markers cannot override 0 */
    int loop_start;               /* marked block offset to loop to (-1 = none) */
    int hist1, hist2;             /* ADPCM filter state */
    int pitch;                    /* 0x1000 = native 44.1 kHz */
    int voll, volr;               /* dry L/R, 0..0x1000 */
    int effect_l, effect_r;       /* send L/R, 0..0x1000 */
    int16_t buf[28]; int bufn, bufi;
    uint32_t frac;                /* 12-bit fixed-point resample remainder */
    uint32_t pcm;                 /* mono signed-16 PCM source */
    uint32_t pcm_samples, pcm_pos;
    int pcm_loop_start;
    /* ADSR rates/modes and retained simple-envelope words. */
    int adsr_a, adsr_d, adsr_s, adsr_r;
    int mode_a, mode_d, mode_s, mode_r;
    int sustain_level;            /* 0..0x40000000 */
    uint32_t simple_adsr1, simple_adsr2;
    int env_level;                /* 0..0x40000000 */
    int env_phase;                /* 0=ATTACK, 1=DECAY, 2=SUSTAIN, 3=RELEASE, 4=OFF */
    int noise;                    /* 0..63 (noise clock) */
} SasVoice;

typedef struct {
    int initialized;
    uint32_t core;
    int grain;
    int max_voices;
    int output_mode;              /* 0=interleaved stereo, 1=four planar channels */
    int sample_rate;
    int effect_type;              /* -1=off, 0..8 are the PSP effect types */
    int effect_l, effect_r;
    int effect_delay, effect_feedback;
    int effect_dry, effect_wet;
} SasCoreState;

static SasVoice s_sasv[SAS_VOICES];
static SasCoreState s_sas_core;
static int s_sas_max_voices = SAS_VOICES;

#ifdef SR_HLE_THREAD_SELFTEST
/* The selftest creates several independent synthetic cores in one process.  A
 * real guest would keep one initialized core until teardown; this explicit
 * fixture hook resets only the host-side model and never enters production
 * registration or dispatch paths. */
void sr_hle_test_sas_reset(void) {
    memset(&s_sas_core, 0, sizeof(s_sas_core));
    memset(s_sasv, 0, sizeof(s_sasv));
    s_sas_max_voices = SAS_VOICES;
}
#endif

/* SR_SASLOG: decay-gated trace of KeyOn events and SAS mixer active-voice counts, to determine
 * whether the game ever keys on a SAS voice during the silent-PCM window (ISSUES.md P1). */
static int sas_log_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_SASLOG") != NULL;
    return on;
}

static const int vag_f0[5] = { 0, 60, 115,  98, 122 };
static const int vag_f1[5] = { 0,  0, -52, -55, -60 };

/* Decode the next 16-byte VAG block into v->buf. Returns 0 when the stream ends. */
static int sas_vag_block(SasVoice *v) {
    /* Do subtraction before addition: a forged pos near UINT32_MAX must not wrap into
     * the apparently-valid [pos, pos+16) test.  SetVoice already checked the complete
     * source span, but the cursor still needs this per-block guard after every loop. */
    if (v->pos > v->vag_size || v->vag_size - v->pos < 16u) return 0;
    uint32_t a = v->vag + v->pos;
    int hdr = MEM_R8(a), flags = MEM_R8(a + 1);
    if (flags == 7) return 0;                            /* end marker block */
    if (sas_log_on()) {
        /* Bounded per-voice VAG block dumps (two emissions max, matching the
         * docs): the first block (pos 0, first grain) and the block at +0x10,
         * which is the first data block after a legitimate zero prefix. The
         * 16-byte span is guaranteed by the pos+16 guard above; validate the
         * guest span anyway so the diagnostic never reads past mapped memory. */
        static uint32_t dumps[32];
        uint32_t vi = (uint32_t)(v - s_sasv);
        if (dumps[vi] < 2u && sr_guest_span_readable(v->vag + v->pos, 16u)) {
            uint8_t bh[16];
            for (uint32_t k = 0; k < 16; k++) bh[k] = MEM_R8(a + k);
            if (v->pos == 0u && v->bufn == 0) {
                fprintf(stderr, "SAS_VAG_B0: vbl=%u voice=%u vag=0x%08x pos=0x%x size=%u hdr=%02x flags=%02x b0=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        s_vcount, vi, v->vag, v->pos, v->vag_size, hdr, flags,
                        bh[0], bh[1], bh[2], bh[3], bh[4], bh[5], bh[6], bh[7],
                        bh[8], bh[9], bh[10], bh[11], bh[12], bh[13], bh[14], bh[15]);
                dumps[vi]++;
            } else if (v->pos == 0x10u) {
                fprintf(stderr, "SAS_VAG_B16: vbl=%u voice=%u vag=0x%08x pos=0x%x size=%u hdr=%02x flags=%02x b16=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        s_vcount, vi, v->vag, v->pos, v->vag_size, hdr, flags,
                        bh[0], bh[1], bh[2], bh[3], bh[4], bh[5], bh[6], bh[7],
                        bh[8], bh[9], bh[10], bh[11], bh[12], bh[13], bh[14], bh[15]);
                dumps[vi]++;
            }
        }
    }
    int pred = (hdr >> 4) & 0xF, shift = hdr & 0xF;
    if (pred > 4) pred = 0;
    if (flags == 6 && v->loop_requested) v->loop_start = (int)v->pos; /* guest-enabled loop */
    for (int i = 0; i < 28; i++) {
        int byte = MEM_R8(a + 2 + (i >> 1));
        int nib = (i & 1) ? (byte >> 4) : (byte & 0xF);
        int samp = (int)((int16_t)((uint16_t)nib << 12)) >> shift;
        samp += (v->hist1 * vag_f0[pred] + v->hist2 * vag_f1[pred]) >> 6;
        if (samp > 32767) samp = 32767;
        if (samp < -32768) samp = -32768;
        v->buf[i] = (int16_t)samp;

        v->hist2 = v->hist1; v->hist1 = samp;
    }
    v->pos += 16;
    if (flags == 3 && v->loop_requested && v->loop_start >= 0)
        v->pos = (uint32_t)v->loop_start;  /* data marker + guest policy */
    v->bufn = 28; v->bufi = 0;
    return 1;
}

/* SR_AUDIOSTAT aggregates: does the game put BGM PCM into the SAS output buffer
 * before SAS runs, and does SAS preserve or erase it? pre_peak is the buffer's
 * amplitude on entry, post_peak on exit. */
static int audio_stat_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_AUDIOSTAT") != NULL;
    return on;
}
/* Stateful handlers for the public SAS registrations.  The host keeps the
 * decoded voice state here while the guest core pointer remains a validated
 * identity handle. */
static uint32_t sas_core_address_ok(uint32_t core) {
    if (!core || (core & 0x3fu) != 0 || !sr_guest_span_readable(core, SAS_CORE_BYTES))
        return SAS_ERROR_ADDRESS;
    return 0;
}

static uint32_t sas_require_core(uint32_t core) {
    uint32_t e = sas_core_address_ok(core);
    if (e) return e;
    if (!s_sas_core.initialized || core != s_sas_core.core) return SAS_ERROR_NOTINIT;
    return 0;
}

static uint32_t sas_voice_for(uint32_t core, uint32_t raw_voice, SasVoice **out) {
    uint32_t e = sas_require_core(core);
    int32_t voice = (int32_t)raw_voice;
    if (e) return e;
    if (voice < 0 || voice >= s_sas_max_voices) return SAS_ERROR_VOICE_INDEX;
    *out = &s_sasv[voice];
    return 0;
}

static int sas_grain_valid(int32_t grain) {
    return grain >= SAS_GRAIN_MIN && grain <= SAS_GRAIN_MAX && (grain % 64) == 0;
}

static int sas_output_bytes(uint32_t *bytes) {
    uint32_t channels = s_sas_core.output_mode ? 4u : 2u;
    return sr_size_mul_ok((uint32_t)s_sas_core.grain, channels * 2u, bytes);
}

static uint32_t sas_output_validate(uint32_t out, int add) {
    uint32_t bytes = 0;
    if (!out || !sas_output_bytes(&bytes) || !sr_guest_span_writable(out, bytes))
        return SAS_ERROR_ADDRESS;
    if (add && !sr_guest_span_readable(out, bytes)) return SAS_ERROR_ADDRESS;
    return 0;
}

static int16_t sas_output_read(uint32_t out, uint32_t frame, int channel) {
    uint32_t off;
    if (s_sas_core.output_mode) {
        off = (uint32_t)channel * (uint32_t)s_sas_core.grain * 2u + frame * 2u;
    } else {
        if (channel > 1) return 0;
        off = frame * 4u + (uint32_t)channel * 2u;
    }
    return (int16_t)MEM_R16(out + off);
}

static void sas_output_write(uint32_t out, uint32_t frame, int channel, int value) {
    uint32_t off;
    if (s_sas_core.output_mode) {
        off = (uint32_t)channel * (uint32_t)s_sas_core.grain * 2u + frame * 2u;
    } else {
        if (channel > 1) return;
        off = frame * 4u + (uint32_t)channel * 2u;
    }
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    MEM_W16(out + off, (uint16_t)(int16_t)value);
}

static int sas_env_step(int configured, int fallback) {
    return configured > 0 ? configured : fallback;
}

static void sas_advance_envelope(SasVoice *v) {
    if (v->env_phase == 0) {
        int64_t level = (int64_t)v->env_level + sas_env_step(v->adsr_a, 0x1000000);
        if (level >= SAS_ENVELOPE_MAX) { v->env_level = SAS_ENVELOPE_MAX; v->env_phase = 1; }
        else v->env_level = (int)level;
    } else if (v->env_phase == 1) {
        int step = sas_env_step(v->adsr_d, 0x800000);
        if (v->env_level > v->sustain_level) {
            v->env_level -= step;
            if (v->env_level <= v->sustain_level) {
                v->env_level = v->sustain_level; v->env_phase = 2;
            }
        } else {
            v->env_level = v->sustain_level; v->env_phase = 2;
        }
    } else if (v->env_phase == 3) {
        v->env_level -= sas_env_step(v->adsr_r, 0x1000000);
        if (v->env_level <= 0) { v->env_level = 0; v->env_phase = 4; v->on = 0; }
    }
}

static int sas_next_sample(SasVoice *v, int *sample) {
    if (v->type == SAS_VOICE_NOISE) {
        static uint32_t nseed = 0x12345678;
        nseed = nseed * 1103515245u + 12345u;
        *sample = (int)(int16_t)(nseed >> 16);
        return 1;
    }
    if (v->type == SAS_VOICE_PCM) {
        if (v->pcm_pos >= v->pcm_samples) {
            if (v->pcm_loop_start >= 0 && v->pcm_loop_start < (int)v->pcm_samples)
                v->pcm_pos = (uint32_t)v->pcm_loop_start;
            else return 0;
        }
        *sample = (int16_t)MEM_R16(v->pcm + v->pcm_pos * 2u);
        v->pcm_pos++;
        return 1;
    }
    if (v->type != SAS_VOICE_VAG) return 0;
    if (v->bufi >= v->bufn && !sas_vag_block(v)) return 0;
    *sample = v->buf[v->bufi];
    return 1;
}

/* Mix one grain into out (s16 stereo or four planar channels). add=0 overwrites. */
static void sas_mix_stateful(uint32_t out, int add, int left_gain, int right_gain) {
    int32_t mixl[2048], mixr[2048], mixsl[2048], mixsr[2048];
    int n = s_sas_core.grain;
    for (int i = 0; i < n; i++) mixl[i] = mixr[i] = mixsl[i] = mixsr[i] = 0;
    int stat = audio_stat_on(), pre_peak = 0;
    if (stat) {
        g_sas_calls++;
        if (add) g_sas_calls_add++;
        audio_note_sas_buf(out);
        int active = 0;
        for (int vi = 0; vi < s_sas_max_voices; vi++) if (s_sasv[vi].on) active++;
        if (!active) g_sas_no_voice++;
        if (!active && !add) g_sas_no_voice_overwrite++;
        for (int i = 0; i < n; i++) {
            int l = sas_output_read(out, (uint32_t)i, 0);
            int r = sas_output_read(out, (uint32_t)i, 1);
            if (l < 0) l = -l;
            if (r < 0) r = -r;
            if (l > pre_peak) pre_peak = l;
            if (r > pre_peak) pre_peak = r;
        }
        if (pre_peak) g_sas_pre_nonzero++;
        if (pre_peak > g_sas_pre_peak) g_sas_pre_peak = pre_peak;
    }
    if (sas_log_on()) {
        static uint32_t call;
        static int last_active = -1;
        call++;
        int active = 0; unsigned busy = 0;
        for (int vi = 0; vi < s_sas_max_voices; vi++) {
            if (s_sasv[vi].on) active++;
            if (s_sasv[vi].on) busy |= 1u << vi;
        }
        if (call <= 64u || (call & (call - 1u)) == 0u || active != last_active) {
            fprintf(stderr, "SAS_MIX: call=%u vbl=%u out=0x%08x add=%d active=%d busy=0x%x grain=%d\n",
                    call, s_vcount, out, add, active, busy, n);
            for (int vi = 0; vi < s_sas_max_voices; vi++) {
                SasVoice *v = &s_sasv[vi];
                if (v->on)
                    fprintf(stderr, "SAS_MIX_V: vbl=%u voice=%d type=%d pos=%u bufn=%d bufi=%d pitch=%d vol=%d/%d env=%d\n",
                            s_vcount, vi, v->type, v->pos, v->bufn, v->bufi, v->pitch, v->voll, v->volr, v->env_level);
            }
        }
        last_active = active;
    }
    for (int vi = 0; vi < s_sas_max_voices; vi++) {
        SasVoice *v = &s_sasv[vi];
        if (!v->on || v->paused) continue;
        for (int i = 0; i < n; i++) {
            int samp = 0;
            if (!sas_next_sample(v, &samp)) { v->on = 0; break; }
            sas_advance_envelope(v);
            int s_env = (int)(((int64_t)samp * v->env_level) >> 30);
            mixl[i] += (s_env * v->voll) >> 12;
            mixr[i] += (s_env * v->volr) >> 12;
            mixsl[i] += (s_env * v->effect_l) >> 12;
            mixsr[i] += (s_env * v->effect_r) >> 12;
            v->frac += (uint32_t)(v->pitch > 0 ? v->pitch : 0x1000);
            while (v->frac >= 0x1000) {
                v->frac -= 0x1000;
                if (v->type == SAS_VOICE_VAG) v->bufi++;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        int l = (mixl[i] * left_gain) >> 12;
        int r = (mixr[i] * right_gain) >> 12;
        int sl = mixsl[i], sr = mixsr[i];
        if (add) {
            l += sas_output_read(out, (uint32_t)i, 0);
            r += sas_output_read(out, (uint32_t)i, 1);
            if (s_sas_core.output_mode) {
                sl += sas_output_read(out, (uint32_t)i, 2);
                sr += sas_output_read(out, (uint32_t)i, 3);
            }
        }
        sas_output_write(out, (uint32_t)i, 0, l);
        sas_output_write(out, (uint32_t)i, 1, r);
        if (s_sas_core.output_mode) {
            sas_output_write(out, (uint32_t)i, 2, sl);
            sas_output_write(out, (uint32_t)i, 3, sr);
        }
    }
    if (stat) {
        int post_peak = 0;
        for (int i = 0; i < n; i++) {
            int l = sas_output_read(out, (uint32_t)i, 0);
            int r = sas_output_read(out, (uint32_t)i, 1);
            if (l < 0) l = -l;
            if (r < 0) r = -r;
            if (l > post_peak) post_peak = l;
            if (r > post_peak) post_peak = r;
        }
        if (post_peak) g_sas_post_nonzero++;
        if (post_peak > g_sas_post_peak) g_sas_post_peak = post_peak;
        if (pre_peak && !post_peak) g_sas_erased++;
    }
}

static uint32_t h_SasInit(CpuState *s) {
    uint32_t e = sas_core_address_ok(A0);
    int32_t grain = (int32_t)A1, max_voices = (int32_t)A2;
    int32_t output_mode = (int32_t)A3, sample_rate = (int32_t)stack_arg(s, 0);
    if (e) return e;
    if (s_sas_core.initialized) return SAS_ERROR_ALRDYINIT;
    if (!sas_grain_valid(grain)) return SAS_ERROR_ADPCM_SIZE;
    if (max_voices < 1 || max_voices > SAS_VOICES) return SAS_ERROR_VOICE_INDEX;
    if (output_mode != 0 && output_mode != 1) return SAS_ERROR_INVALID_STATE;
    if (sample_rate != SAS_SAMPLE_RATE) return SAS_ERROR_INVALID_STATE;
    memset(&s_sas_core, 0, sizeof(s_sas_core));
    s_sas_core.initialized = 1; s_sas_core.core = A0; s_sas_core.grain = grain;
    s_sas_core.max_voices = max_voices; s_sas_core.output_mode = output_mode;
    s_sas_core.sample_rate = sample_rate; s_sas_core.effect_type = -1;
    s_sas_core.effect_l = s_sas_core.effect_r = SAS_VOLUME_MAX;
    s_sas_core.effect_dry = s_sas_core.effect_wet = 1;
    s_sas_max_voices = max_voices;
    memset(s_sasv, 0, sizeof(s_sasv));
    for (int i = 0; i < SAS_VOICES; i++) {
        s_sasv[i].pitch = 0x1000; s_sasv[i].loop_start = -1; s_sasv[i].pcm_loop_start = -1;
        s_sasv[i].sustain_level = SAS_ENVELOPE_MAX; s_sasv[i].env_phase = 4;
    }
    return 0;
}

static uint32_t h_SasSetVoice(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t size = (int32_t)A3, loop = (int32_t)stack_arg(s, 0);
    if (e) return e;
    if (size <= 0 || (((uint32_t)size & 15u) != 0)) return SAS_ERROR_ADPCM_SIZE;
    if (loop != 0 && loop != 1) return SAS_ERROR_LOOP_MODE;
    if (!A2 || !sr_guest_span_readable(A2, (uint32_t)size)) return SAS_ERROR_ADDRESS;
    v->on = 0; v->type = SAS_VOICE_VAG; v->vag = A2; v->vag_size = (uint32_t)size;
    v->pos = 0; v->loop_requested = loop; v->loop_start = -1;
    v->pcm = v->pcm_samples = v->pcm_pos = 0; v->pcm_loop_start = -1;
    v->noise = 0; v->hist1 = v->hist2 = 0; v->bufn = v->bufi = 0; v->frac = 0;
    return 0;
}

static uint32_t h_SasSetVoicePCM(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t samples = (int32_t)A3, loop_start = (int32_t)stack_arg(s, 0);
    uint32_t bytes = 0;
    if (e) return e;
    if (samples <= 0 || !sr_size_mul_ok((uint32_t)samples, 2u, &bytes)) return SAS_ERROR_ADPCM_SIZE;
    if (loop_start < -1 || loop_start >= samples) return SAS_ERROR_LOOP_MODE;
    if (!A2 || !sr_guest_span_readable(A2, bytes)) return SAS_ERROR_ADDRESS;
    v->on = 0; v->type = SAS_VOICE_PCM; v->pcm = A2; v->pcm_samples = (uint32_t)samples;
    v->pcm_pos = 0; v->pcm_loop_start = loop_start; v->vag = v->vag_size = v->pos = 0;
    v->loop_requested = loop_start >= 0; v->loop_start = -1; v->noise = 0; v->bufn = v->bufi = 0;
    return 0;
}

static uint32_t h_SasSetPitch(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t pitch = (int32_t)A2;
    if (e) return e;
    if (pitch < SAS_PITCH_MIN || pitch > SAS_PITCH_MAX) return SAS_ERROR_PITCH_VAL;
    v->pitch = pitch; return 0;
}

static uint32_t h_SasSetVolume(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t l = (int32_t)A2, r = (int32_t)A3;
    int32_t el = (int32_t)stack_arg(s, 0), er = (int32_t)stack_arg(s, 1);
    if (e) return e;
    if (l < 0 || l > SAS_VOLUME_MAX || r < 0 || r > SAS_VOLUME_MAX ||
        el < 0 || el > SAS_VOLUME_MAX || er < 0 || er > SAS_VOLUME_MAX)
        return SAS_ERROR_VOLUME_VAL;
    v->voll = l; v->volr = r; v->effect_l = el; v->effect_r = er; return 0;
}

static uint32_t h_SasSetADSR(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    uint32_t mask = A2;
    int32_t a = (int32_t)A3, d = (int32_t)stack_arg(s, 0);
    int32_t sustain = (int32_t)stack_arg(s, 1), r = (int32_t)stack_arg(s, 2);
    if (e) return e;
    if (mask & ~SAS_ADSR_ALL) return SAS_ERROR_ADSR_VAL;
    if ((mask & SAS_ADSR_ATTACK && a < 0) || (mask & SAS_ADSR_DECAY && d < 0) ||
        (mask & SAS_ADSR_SUSTAIN && sustain < 0) || (mask & SAS_ADSR_RELEASE && r < 0))
        return SAS_ERROR_ADSR_VAL;
    if (mask & SAS_ADSR_ATTACK) v->adsr_a = a;
    if (mask & SAS_ADSR_DECAY) v->adsr_d = d;
    if (mask & SAS_ADSR_SUSTAIN) v->adsr_s = sustain;
    if (mask & SAS_ADSR_RELEASE) v->adsr_r = r;
    return 0;
}

static uint32_t h_SasSetADSRmode(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    uint32_t mask = A2;
    int32_t a = (int32_t)A3, d = (int32_t)stack_arg(s, 0);
    int32_t sustain = (int32_t)stack_arg(s, 1), r = (int32_t)stack_arg(s, 2);
    if (e) return e;
    if (mask & ~SAS_ADSR_ALL) return SAS_ERROR_ADSR_MODE;
    if ((mask & SAS_ADSR_ATTACK && (a < 0 || a > 5)) ||
        (mask & SAS_ADSR_DECAY && (d < 0 || d > 5)) ||
        (mask & SAS_ADSR_SUSTAIN && (sustain < 0 || sustain > 5)) ||
        (mask & SAS_ADSR_RELEASE && (r < 0 || r > 5))) return SAS_ERROR_ADSR_MODE;
    if (mask & SAS_ADSR_ATTACK) v->mode_a = a;
    if (mask & SAS_ADSR_DECAY) v->mode_d = d;
    if (mask & SAS_ADSR_SUSTAIN) v->mode_s = sustain;
    if (mask & SAS_ADSR_RELEASE) v->mode_r = r;
    return 0;
}

static uint32_t h_SasSetSimpleADSR(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    if (e) return e;
    if ((A3 >> 13) & 1u) return SAS_ERROR_ADSR_MODE;
    v->simple_adsr1 = A2; v->simple_adsr2 = A3;
    v->adsr_a = (int)(A2 & 0xffffu); v->adsr_r = (int)(A3 & 0xffffu);
    v->adsr_d = 0; v->adsr_s = SAS_ENVELOPE_MAX;
    return 0;
}

static uint32_t h_SasSetSL(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t level = (int32_t)A2;
    if (e) return e;
    if (level < 0 || level > SAS_ENVELOPE_MAX) return SAS_ERROR_ADSR_VAL;
    v->sustain_level = level; return 0;
}

static uint32_t h_SasSetNoise(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t freq = (int32_t)A2;
    if (e) return e;
    if (freq < 0 || freq > SAS_NOISE_FREQ_MAX) return SAS_ERROR_NOISE_CLOCK;
    v->on = 0; v->type = SAS_VOICE_NOISE; v->noise = freq;
    v->vag = v->vag_size = v->pos = 0; v->pcm = v->pcm_samples = v->pcm_pos = 0;
    v->pcm_loop_start = -1; v->loop_requested = 0; v->loop_start = -1; v->bufn = v->bufi = 0;
    return 0;
}

static uint32_t h_SasGetEndFlag(CpuState *s) {
    uint32_t e = sas_require_core(A0), m = 0;
    if (e) return e;
    for (int i = 0; i < s_sas_max_voices; i++) if (!s_sasv[i].on) m |= 1u << i;
    return m;
}

static uint32_t h_SasSetKeyOn(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    if (e) return e;
    if (v->paused || v->type == SAS_VOICE_OFF ||
        (v->type == SAS_VOICE_VAG && (!v->vag || v->vag_size < 16u)) ||
        (v->type == SAS_VOICE_PCM && (!v->pcm || !v->pcm_samples))) return SAS_ERROR_INVALID_STATE;
    v->pos = 0; v->pcm_pos = 0; v->hist1 = v->hist2 = 0; v->bufn = v->bufi = 0; v->frac = 0;
    if (!v->voll && !v->volr) { v->voll = SAS_VOLUME_MAX; v->volr = SAS_VOLUME_MAX; }
    v->env_level = 0; v->env_phase = 0; v->on = 1;
    if (sas_log_on()) {
        static uint32_t calls; calls++;
        fprintf(stderr, "SAS_KEYON: call=%u vbl=%u voice=%u type=%d vag=0x%08x size=%u voll=%d volr=%d on=%d\n",
                calls, s_vcount, A1, v->type, v->vag, v->vag_size, v->voll, v->volr, v->on);
    }
    return 0;
}

static uint32_t h_SasSetKeyOff(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    if (e) return e;
    if (v->on) v->env_phase = 3;
    return 0;
}

static uint32_t h_SasGetEnvelopeHeight(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    return e ? e : (uint32_t)v->env_level;
}

static uint32_t h_SasGetAllEnvelopeHeights(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    if (!A1 || !sr_guest_span_writable(A1, SAS_VOICES * 4u)) return SAS_ERROR_ADDRESS;
    for (int i = 0; i < SAS_VOICES; i++) MEM_W32(A1 + (uint32_t)i * 4u, (uint32_t)s_sasv[i].env_level);
    return 0;
}

static uint32_t h_SasGetPauseFlag(CpuState *s) {
    uint32_t e = sas_require_core(A0), m = 0;
    if (e) return e;
    for (int i = 0; i < s_sas_max_voices; i++) if (s_sasv[i].paused) m |= 1u << i;
    return m;
}

static uint32_t h_SasSetPause(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    for (int i = 0; i < s_sas_max_voices; i++)
        if (A1 & (1u << i)) s_sasv[i].paused = A2 != 0;
    return 0;
}

static uint32_t h_SasSetGrain(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t grain = (int32_t)A1;
    if (e) return e;
    if (!sas_grain_valid(grain)) return SAS_ERROR_ADPCM_SIZE;
    s_sas_core.grain = grain; return 0;
}

static uint32_t h_SasGetGrain(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    return e ? e : (uint32_t)s_sas_core.grain;
}

static uint32_t h_SasSetOutputmode(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    if (A1 > 1u) return SAS_ERROR_INVALID_STATE;
    s_sas_core.output_mode = (int)A1; return 0;
}

static uint32_t h_SasGetOutputmode(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    return e ? e : (uint32_t)s_sas_core.output_mode;
}

static uint32_t h_SasRevType(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t type = (int32_t)A1;
    if (e) return e;
    if (type < -1 || type > 8) return SAS_ERROR_FX_TYPE;
    s_sas_core.effect_type = type; return 0;
}

static uint32_t h_SasRevParam(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t delay = (int32_t)A1, feedback = (int32_t)A2;
    if (e) return e;
    if (delay < 0 || delay > 128) return SAS_ERROR_FX_DELAY;
    if (feedback < 0 || feedback > 128) return SAS_ERROR_FX_FEEDBACK;
    s_sas_core.effect_delay = delay; s_sas_core.effect_feedback = feedback; return 0;
}

static uint32_t h_SasRevEVOL(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t l = (int32_t)A1, r = (int32_t)A2;
    if (e) return e;
    if (l < 0 || l > SAS_VOLUME_MAX || r < 0 || r > SAS_VOLUME_MAX) return SAS_ERROR_FX_VOLUME_VAL;
    s_sas_core.effect_l = l; s_sas_core.effect_r = r; return 0;
}

static uint32_t h_SasRevVON(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    s_sas_core.effect_dry = A1 != 0; s_sas_core.effect_wet = A2 != 0; return 0;
}

static uint32_t h_SasCore(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    e = sas_output_validate(A1, 0);
    if (e) return e;
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sas_mix_stateful(A1, 0, SAS_VOLUME_MAX, SAS_VOLUME_MAX); return 0;
}

static uint32_t h_SasCoreWithMix(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t left_gain = (int32_t)A2, right_gain = (int32_t)A3;
    if (e) return e;
    if (left_gain < 0 || left_gain > SAS_VOLUME_MAX || right_gain < 0 || right_gain > SAS_VOLUME_MAX)
        return SAS_ERROR_VOLUME_VAL;
    e = sas_output_validate(A1, 1);
    if (e) return e;
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sas_mix_stateful(A1, 1, left_gain, right_gain); return 0;
}

/* The public API exposes triangular/square and ATRAC3 voices, but the current runtime has
 * no source-owned implementation for those codecs. Refuse them after validating the core
 * and voice instead of routing them through h_ok and claiming success. */
static uint32_t h_SasUnsupportedVoice(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    (void)v;
    return e ? e : SAS_ERROR_INVALID_STATE;
}

/* ---- message pipes -----------------------------------------------------------------
 *
 * HST uses the nonblocking message-pipe API as a byte FIFO for its worker queues.  Keep the
 * payload in host-owned kernel memory (as the PSP kernel does), and copy only at the send/receive
 * boundary.  waitMode 0 requests a complete transfer; nonzero mode permits the largest transfer
 * currently possible and reports that byte count through resultSize.
 *
 * PSP-resource model (issue #178): the PSP kernel allocates a pipe buffer from kernel memory,
 * which is a small partition (single-digit MBs on retail).  A request larger than
 * MSG_PIPE_MAX_CAPACITY is outside the modeled range and is rejected BEFORE any host
 * allocation, UID hand-out, or slot reservation -- a malformed guest can therefore never make
 * the host allocate gigabytes.  The ceiling is a project-level hard cap for this loader, not
 * a claim about the exact retail error code for oversized requests: ILLEGAL_SIZE is the
 * deterministic in-model rejection (NO_MEMORY would also be defensible for an allocation the
 * model cannot satisfy; pick one and keep it documented). */

#define MSG_PIPE_MAX 32
/* 1 MiB ceiling: far above the title's observed FIFO usage and comfortably within
 * a small kernel partition, while keeping the 32-slot worst case at 32 MiB of
 * host memory.  Rejected creates return SCE_KERNEL_ERROR_ILLEGAL_SIZE. */
#define MSG_PIPE_MAX_CAPACITY 0x100000u
#define SCE_KERNEL_ERROR_UNKNOWN_MPPID 0x8002019eu
#define SCE_KERNEL_ERROR_MPP_FULL      0x800201b3u
#define SCE_KERNEL_ERROR_MPP_EMPTY     0x800201b4u
#define SCE_KERNEL_ERROR_ILLEGAL_SIZE  0x800201bcu
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR  0x80000103u
typedef struct {
    int used;
    uint32_t uid, attr, capacity, read_pos, write_pos, count;
    uint32_t send_calls, receive_calls;
    uint8_t *data;
    char name[32];
} MsgPipe;
static MsgPipe s_msg_pipes[MSG_PIPE_MAX];

static MsgPipe *msg_pipe_find(uint32_t uid) {
    for (int i = 0; i < MSG_PIPE_MAX; i++)
        if (s_msg_pipes[i].used && s_msg_pipes[i].uid == uid) return &s_msg_pipes[i];
    return NULL;
}

static int msg_pipe_trace_call(uint32_t calls) {
    return getenv("SR_MSGLOG") && (calls <= 16u || (calls & (calls - 1u)) == 0u);
}

static uint32_t h_CreateMsgPipe(CpuState *s) {
    /* a0=name, a1=partition, a2=attr, a3=bufferSize, t0=option. */
    (void)A1;
    /* Issue #178: validate against the documented PSP-resource model BEFORE any
     * host allocation.  Zero is illegal; sizes above MSG_PIPE_MAX_CAPACITY are
     * outside the modeled range.  Both fail before malloc, before a UID is
     * handed out, and before a slot is reserved, so a rejected create leaves
     * no observable state behind. */
    if (A3 == 0 || A3 > MSG_PIPE_MAX_CAPACITY) return SCE_KERNEL_ERROR_ILLEGAL_SIZE;
    /* A3 <= MSG_PIPE_MAX_CAPACITY (1 MiB), so the allocation is bounded and
     * size_t conversion cannot truncate. */
    size_t capacity = (size_t)A3;
    for (int i = 0; i < MSG_PIPE_MAX; i++) {
        MsgPipe *p = &s_msg_pipes[i];
        if (p->used) continue;
        uint8_t *data = (uint8_t *)malloc(capacity);
        if (!data) return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */
        memset(p, 0, sizeof(*p));
        p->used = 1;
        p->uid = sr_alloc_uid();
        p->attr = A2;
        p->capacity = A3;
        p->data = data;
        guest_cstr(A0, p->name, sizeof(p->name));
        if (hle_log_on() || getenv("SR_MSGLOG"))
            fprintf(stderr, "HLE: CreateMsgPipe uid=0x%x name='%s' attr=0x%x size=%u\n",
                    p->uid, p->name, p->attr, p->capacity);
        return p->uid;
    }
    return 0x80020190u;
}

static uint32_t h_DeleteMsgPipe(CpuState *s) {
    MsgPipe *p = msg_pipe_find(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_MPPID;
    if (getenv("SR_MSGLOG"))
        fprintf(stderr, "HLE: DeleteMsgPipe uid=0x%x name='%s' queued=%u send=%u receive=%u\n",
                p->uid, p->name, p->count, p->send_calls, p->receive_calls);
    free(p->data);
    memset(p, 0, sizeof(*p));
    sched_wake(A0);
    return 0;
}

static uint32_t h_TrySendMsgPipe(CpuState *s) {
    /* a0=uid, a1=source, a2=size, a3=waitMode, t0=resultSize. */
    uint32_t resultp = stack_arg(s, 0);
    /* Issue #178: preflight the complete writable resultSize span BEFORE any
     * write, so an invalid result pointer is rejected with no side effect
     * instead of being silently no-op'd mid-send.  Historical semantics are
     * preserved: resultSize is zeroed before the pipe lookup, but only once
     * its span has been validated. */
    if (resultp && !sr_guest_span_writable(resultp, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (resultp) MEM_W32(resultp, 0);
    MsgPipe *p = msg_pipe_find(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_MPPID;
    if (A2 == 0 || A2 > p->capacity) return SCE_KERNEL_ERROR_ILLEGAL_SIZE;
    p->send_calls++;
    uint32_t free_bytes = p->capacity - p->count;
    uint32_t amount = A2;
    if (amount > free_bytes) {
        if (A3 == 0 || free_bytes == 0) {
            if (msg_pipe_trace_call(p->send_calls))
                fprintf(stderr, "MSGPIPE: send uid=0x%x thread=0x%x call=%u requested=%u queued=%u -> FULL\n",
                        p->uid, sched_current_uid(), p->send_calls, A2, p->count);
            return SCE_KERNEL_ERROR_MPP_FULL;
        }
        amount = free_bytes;
    }
    /* Issue #178: preflight the complete source span for the ACTUAL transfer
     * amount before mutating FIFO indices/count.  Per-byte MEM_R8 reads are
     * individually bounds-safe, but a partial/invalid span would otherwise
     * advance write_pos/count while fabricating zero bytes for the invalid
     * tail.  Reject the whole operation instead. */
    if (!sr_guest_span_readable(A1, amount)) {
        if (msg_pipe_trace_call(p->send_calls))
            fprintf(stderr, "MSGPIPE: send uid=0x%x thread=0x%x call=%u requested=%u -> ILLEGAL_ADDR (src 0x%08x len %u)\n",
                    p->uid, sched_current_uid(), p->send_calls, A2, A1, amount);
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    for (uint32_t i = 0; i < amount; i++) {
        p->data[p->write_pos] = MEM_R8(A1 + i);
        p->write_pos = (p->write_pos + 1) % p->capacity;
    }
    p->count += amount;
    if (resultp) MEM_W32(resultp, amount);
    if (msg_pipe_trace_call(p->send_calls))
        fprintf(stderr, "MSGPIPE: send uid=0x%x thread=0x%x call=%u requested=%u transferred=%u queued=%u\n",
                p->uid, sched_current_uid(), p->send_calls, A2, amount, p->count);
    sched_wake(A0);
    return 0;
}

static uint32_t h_TryReceiveMsgPipe(CpuState *s) {
    /* a0=uid, a1=destination, a2=size, a3=waitMode, t0=resultSize. */
    uint32_t resultp = stack_arg(s, 0);
    /* Issue #178: preflight the complete writable resultSize span BEFORE any
     * write (see h_TrySendMsgPipe).  Historical semantics are preserved:
     * resultSize is zeroed before the pipe lookup, but only once its span has
     * been validated. */
    if (resultp && !sr_guest_span_writable(resultp, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (resultp) MEM_W32(resultp, 0);
    MsgPipe *p = msg_pipe_find(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_MPPID;
    if (A2 == 0 || A2 > p->capacity) return SCE_KERNEL_ERROR_ILLEGAL_SIZE;
    p->receive_calls++;
    uint32_t amount = A2;
    if (amount > p->count) {
        if (A3 == 0 || p->count == 0) {
            if (msg_pipe_trace_call(p->receive_calls))
                fprintf(stderr, "MSGPIPE: receive uid=0x%x thread=0x%x call=%u requested=%u queued=%u -> EMPTY\n",
                        p->uid, sched_current_uid(), p->receive_calls, A2, p->count);
            return SCE_KERNEL_ERROR_MPP_EMPTY;
        }
        amount = p->count;
    }
    /* Issue #178: preflight the complete destination span for the ACTUAL
     * transfer amount before mutating FIFO indices/count, so an invalid
     * destination can never leave the pipe partially drained. */
    if (!sr_guest_span_writable(A1, amount)) {
        if (msg_pipe_trace_call(p->receive_calls))
            fprintf(stderr, "MSGPIPE: receive uid=0x%x thread=0x%x call=%u requested=%u -> ILLEGAL_ADDR (dst 0x%08x len %u)\n",
                    p->uid, sched_current_uid(), p->receive_calls, A2, A1, amount);
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    for (uint32_t i = 0; i < amount; i++) {
        MEM_W8(A1 + i, p->data[p->read_pos]);
        p->read_pos = (p->read_pos + 1) % p->capacity;
    }
    p->count -= amount;
    if (resultp) MEM_W32(resultp, amount);
    if (msg_pipe_trace_call(p->receive_calls))
        fprintf(stderr, "MSGPIPE: receive uid=0x%x thread=0x%x call=%u requested=%u transferred=%u queued=%u\n",
                p->uid, sched_current_uid(), p->receive_calls, A2, amount, p->count);
    sched_wake(A0);
    return 0;
}

/* Message-pipe NID registrations.  Shared by the production registry and the
 * executable HLE selftest so the test exercises the exact production mapping
 * rather than a duplicate registration list. */
static void hle_register_msgpipe_handlers(void) {
    sr_hle_register(0x7c0dc2a0, "sceKernelCreateMsgPipe", h_CreateMsgPipe);
    sr_hle_register(0xf0b7da1c, "sceKernelDeleteMsgPipe", h_DeleteMsgPipe);
    sr_hle_register(0x884c9f90, "sceKernelTrySendMsgPipe", h_TrySendMsgPipe);
    sr_hle_register(0xdf52098f, "sceKernelTryReceiveMsgPipe", h_TryReceiveMsgPipe);
}

#ifdef SR_HLE_THREAD_SELFTEST
/* White-box message-pipe probes for the executable HLE selftest (issue #178).
 * The selftest needs to assert the explicit invariants
 *   count <= capacity, read_pos < capacity, write_pos < capacity
 * under send/receive churn, which is only observable through the real static
 * pipe state.  These read-only probes are compiled exclusively into the test
 * executable; production builds never see them. */
typedef struct {
    uint32_t capacity, count, read_pos, write_pos;
} SrMsgPipeState;
int sr_hle_test_msgpipe_state(uint32_t uid, SrMsgPipeState *out) {
    MsgPipe *p = msg_pipe_find(uid);
    if (!p || !out) return 0;
    out->capacity  = p->capacity;
    out->count     = p->count;
    out->read_pos  = p->read_pos;
    out->write_pos = p->write_pos;
    return 1;
}
/* The capacity ceiling is a documented constant; expose it so the selftest
 * asserts the exact legal/illegal boundaries without duplicating the literal. */
uint32_t sr_hle_test_msgpipe_max_capacity(void) { return MSG_PIPE_MAX_CAPACITY; }

/* Expose the measured effective-transfer ceiling to the executable regression
 * without duplicating the contract literal in its fixture. */
uint32_t sr_hle_test_dmac_effective_max(void) { return SCE_DMAC_EFFECTIVE_MAX; }
#endif /* SR_HLE_THREAD_SELFTEST */

/* ---- semaphores and event flags, backed by the scheduler's block/wake-on-object ---- */

typedef struct { int used; uint32_t uid; int count, maxc; uint32_t pattern; } Sync;
static Sync s_sync[128];
static Sync *sync_find(uint32_t uid) {
    for (int i = 0; i < 128; i++) if (s_sync[i].used && s_sync[i].uid == uid) return &s_sync[i];
    return NULL;
}
static Sync *sync_new(void) {
    for (int i = 0; i < 128; i++) if (!s_sync[i].used) { s_sync[i].used = 1; s_sync[i].uid = sr_alloc_uid(); return &s_sync[i]; }
    return NULL;
}

static uint32_t h_CreateSema(CpuState *s) {
    /* a0=name, a1=attr, a2=initCount, a3=maxCount. */
    Sync *m = sync_new(); if (!m) return 0x80020000;
    m->count = (int)A2; m->maxc = (int)A3;
    if (hle_log_on())
        fprintf(stderr, "HLE: CreateSema uid=0x%x init=%d max=%d (from uid=0x%x)\n", m->uid, (int)A2, (int)A3, sched_current_uid());
    return m->uid;
}

/* Complete the small piece of f_00081a04 that the display-init replay reaches
 * before the normal SGX configuration path.  +0x4c0 is written to 1 near the
 * start of that initializer.  When it is still zero, +0x30 is merely BSS and
 * must not be interpreted as an explicit mode 0 selection: the game's
 * "sgx-psp-di-sema" option is read with default 1 at 0x00081c24..0x00081c3c.
 * Mode 1 also owns a count-1 semaphore at +0x0c, created at
 * 0x00081c94..0x00081cb8.  Recreate both pieces together so the callbacks
 * block and wake with the same semantics as the original initializer.
 *
 * Title-qualified (issue #98): the config base, sema name pointer and
 * mode-keyed wrapper pairs are validated title configuration. An unconfigured
 * build performs no sync install at all (generic sceDisplaySetMode has no
 * sync side effects). The pairing and the mode that selects it are part of
 * the meaning and are not flattened. */
static void ensure_runtime_sync_callbacks(CpuState *s) {
    uint32_t config_base, sema_name_ptr;
    const SrTitleRuntimeSyncWrapper *wrappers;
    unsigned wrapper_count;
    if (!sr_title_config_runtime_sync(&config_base, &sema_name_ptr, &wrappers, &wrapper_count)) {
        if (hle_log_on())
            fprintf(stderr,
                    "DISPLAY_SET_MODE: runtime sync not configured (generic, title %s)\n",
                    sr_title_config()->source_id);
        return;
    }
    if (!sr_guest_span_writable(config_base, 0x4c4)) {
        fprintf(stderr,
                "DISPLAY_SET_MODE: runtime sync config 0x%08x not writable (title %s), skipping\n",
                config_base, sr_title_config()->source_id);
        return;
    }
    uint32_t mode = MEM_R32(config_base + 0x30u);
    uint32_t enter = MEM_R32(config_base + 0x34u);
    uint32_t leave = MEM_R32(config_base + 0x38u);
    const int initializer_ran = MEM_R32(config_base + 0x4c0u) != 0;

    if (enter && leave) return;

    if (!initializer_ran) {
        mode = 1;
        MEM_W32(config_base + 0x30u, mode);
    } else if (mode > 2) {
        fprintf(stderr,
                "DISPLAY_SET_MODE: invalid runtime sync mode %u; using game default mode 1 (title %s)\n",
                mode, sr_title_config()->source_id);
        mode = 1;
        MEM_W32(config_base + 0x30u, mode);
    }

    uint32_t cfg_enter = 0, cfg_leave = 0;
    int found = sr_title_config_runtime_sync_wrapper_for_mode(mode, &cfg_enter, &cfg_leave);
    if (!found) {
        /* Fallback to mode 1 if the requested mode is not in the configured set
         * (e.g. a synthetic title that only configures mode 0). Mirrors the
         * historical "invalid mode -> mode 1" fallback. */
        found = sr_title_config_runtime_sync_wrapper_for_mode(1, &cfg_enter, &cfg_leave);
        if (!found) {
            fprintf(stderr,
                    "DISPLAY_SET_MODE: no runtime sync wrapper for mode %u and no mode 1 fallback (title %s)\n",
                    mode, sr_title_config()->source_id);
            return;
        }
        fprintf(stderr, "DISPLAY_SET_MODE: mode %u not in config, fallback to mode 1 wrappers (title %s)\n", mode, sr_title_config()->source_id); mode = 1;
        MEM_W32(config_base + 0x30u, mode);
    }

    if (mode == 1) {
        uint32_t sema = MEM_R32(config_base + 0x0cu);
        if (!sync_find(sema)) {
            CpuState call = *s;
            call.r[4] = sema_name_ptr;
            call.r[5] = 0;
            call.r[6] = 1;
            call.r[7] = 1;
            if (!sr_guest_span_readable(sema_name_ptr, 16)) {
                fprintf(stderr,
                        "DISPLAY_SET_MODE: sema name 0x%08x not readable, still creating sema (title %s)\n",
                        sema_name_ptr, sr_title_config()->source_id);
            }
            sema = h_CreateSema(&call);
            MEM_W32(config_base + 0x0cu, sema);
            fprintf(stderr,
                    "DISPLAY_SET_MODE: created runtime sync sema 0x%x for mode 1 (title %s)\n",
                    sema, sr_title_config()->source_id);
        }
        enter = cfg_enter;
        leave = cfg_leave;
    } else {
        enter = cfg_enter;
        leave = cfg_leave;
    }

    MEM_W32(config_base + 0x34u, enter);
    MEM_W32(config_base + 0x38u, leave);
    fprintf(stderr,
            "DISPLAY_SET_MODE: runtime sync callbacks mode=%u enter=0x%08x leave=0x%08x (title %s)\n",
            mode, enter, leave, sr_title_config()->source_id);
}
static uint32_t h_DeleteSema(CpuState *s) { Sync *m = sync_find(A0); if (m) m->used = 0; return 0; }
/* Entry contract shared by sceKernelWaitSema and sceKernelWaitSemaCB.
 *
 * The order below is not a style choice; each step is placed where a measured
 * oracle cell puts it, and moving any one of them breaks a cell:
 *
 *  1. CONTEXT.  tests/intr/waits.expected probes this API with a bad id and with
 *     an invalid count, and gets CAN_NOT_WAIT for BOTH with interrupts disabled
 *     (L54, L56 / CB L62, L64) and with dispatch disabled (L55, L57 / CB L63,
 *     L65).  In normal context those same two calls answer 80020199 and
 *     800201BD (wait.expected L21/L23, L3/L13), so the context decision must
 *     come ahead of the object lookup AND ahead of count validation.  Being
 *     ahead of the lookup necessarily also puts it ahead of the availability
 *     test -- there is no object to test availability on yet.  That last step is
 *     forced by the measured cells rather than measured directly: no fixture
 *     calls WaitSema on an immediately-available count with a context disabled.
 *     PPSSPP takes the same order, as corroboration only.
 *
 *     This deliberately reverses the earlier "only once it would genuinely
 *     block" placement, which was chosen when the bad-object cells were still
 *     pinned as known deviations.
 *
 *  2. OBJECT.  Unknown or already-deleted id -> UNKNOWN_SEMID, replacing the
 *     generic 0x80020000 seam (wait.expected L21/L23/L25).
 *
 *  3. COUNT.  need <= 0 or need > maxCount -> ILLEGAL_COUNT, immediately.  need
 *     == maxCount is legal (L1: need 1 against max 1 succeeds).
 *
 *     `need` is read as a signed int precisely so a negative request stays
 *     negative here.  Comparing A1 unsigned would turn -1 into 4294967295 and
 *     route it through the same path as any oversized request; it would reach
 *     ILLEGAL_COUNT by luck, and the old `m->count -= need` would have ADDED to
 *     the semaphore.  wait.expected L5/L6 and L15/L16 pin both halves: the call
 *     fails, and the following ReferSemaStatus still reports cur=0.
 *
 *  4. AVAILABILITY / BLOCKING, in the callers.
 *
 * Step 3 running before any mutation is what makes the rejection total: no
 * decrement, no waiter, no timeout word written, and for the CB form no clock
 * sample and no callback dispatch.  wait.expected L11 and L13 show the supplied
 * timeout still holding its original 500ms after an oversized and a zero
 * request, so a rejection may not touch *toptr either.
 *
 * The combined case -- unknown id AND invalid count in the same call -- is NOT
 * pinned by any cited hardware cell.  Object-before-count is what the public
 * reference set supports; treat that single ordering as corroborated, not
 * measured.  See docs/PSP_INTR_WAITS_MATRIX.md. */
static uint32_t sema_wait_entry(const char *who, uint32_t uid, int need, Sync **out) {
    uint32_t err = 0;
    Sync *m = NULL;
    if (!sched_wait_permitted()) {
        err = SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    } else if ((m = sync_find(uid)) == NULL) {
        err = SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    } else if (need <= 0 || need > m->maxc) {
        err = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    }
    if (err) {
        if (hle_log_on())
            fprintf(stderr, "HLE: %s uid=0x%x need=%d rejected 0x%08x (from 0x%x)\n",
                    who, uid, need, err, sched_current_uid());
        return err;
    }
    *out = m;
    return 0;
}

static uint32_t h_WaitSema(CpuState *s) {
    /* a0=semaid, a1=signal, a2=timeout ptr (0=infinite, else *a2 = microseconds). */
    uint32_t uid = A0; int need = (int)A1; uint32_t toptr = A2;
    Sync *m = NULL;
    uint32_t err = sema_wait_entry("WaitSema", uid, need, &m);
    if (err) return err;
    if (hle_log_on())
        fprintf(stderr, "HLE: WaitSema uid=0x%x count=%d need=%d (from 0x%x)\n", uid, m->count, need, sched_current_uid());
    while (m->count < need) {
        if (toptr) {
            uint32_t usec = MEM_R32(toptr);
            if (sched_block_on_timeout(uid, usec)) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        } else {
            sched_block_on(uid);
        }
        /* Deleted out from under an ALREADY-BLOCKED waiter is a different cell
         * from the entry lookup above: wait.expected L20 measures 800201B5
         * (WAIT_DELETE), not UNKNOWN_SEMID. Producing it needs h_DeleteSema to
         * wake its waiters, which it does not do, so this arm is unreachable
         * today and its seam is left untouched rather than replaced with a value
         * that would be newly wrong. Out of scope for issue #43. */
        m = sync_find(uid); if (!m) return 0x80020000;
    }
    m->count -= need;
    return 0;
}
static uint32_t h_WaitSemaCB(CpuState *s) {
    uint32_t uid = A0; int need = (int)A1; uint32_t toptr = A2;
    Sync *m = NULL;
    /* Ahead of sched_vtime_refresh() and the callback loop: a rejected call must
     * not sample the clock, run callbacks or write a remaining-timeout word. */
    uint32_t err = sema_wait_entry("WaitSemaCB", uid, need, &m);
    if (err) return err;
    if (hle_log_on())
        fprintf(stderr, "HLE: WaitSemaCB uid=0x%x count=%d need=%d (from 0x%x)\n", uid, m->count, need, sched_current_uid());

    sched_vtime_refresh();
    uint64_t start = sched_vtime_us();
    uint64_t end = start + (toptr ? MEM_R32(toptr) : 0);

    while (m->count < need) {
        if (sr_thread_has_pending_callbacks(sched_current_uid())) {
            sr_thread_dispatch_callbacks();
            m = sync_find(uid); if (!m) return 0x80020000;
            sched_vtime_refresh();
            continue;
        }
        if (toptr) {
            sched_vtime_refresh();
            if (sched_vtime_us() >= end) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            uint32_t remaining = (uint32_t)(end - sched_vtime_us());
            MEM_W32(toptr, remaining);
            sched_set_current_cb_wait(1);
            int timed_out = sched_block_on_timeout(uid, remaining);
            sched_set_current_cb_wait(0);
            if (timed_out) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        } else {
            sched_set_current_cb_wait(1);
            sched_block_on(uid);
            sched_set_current_cb_wait(0);
        }
        /* See h_WaitSema: hardware's delete-during-wait answer is 800201B5
         * (wait.expected L20), which needs h_DeleteSema to wake waiters. Out of
         * scope for issue #43; seam left as it was. */
        m = sync_find(uid); if (!m) return 0x80020000;
        sched_vtime_refresh();
    }
    m->count -= need;
    return 0;
}
static uint32_t h_SignalSema(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    int signals = (int)A1;
    int new_count = m->count + signals;
    if (new_count > m->maxc) {
        if (hle_log_on()) fprintf(stderr, "HLE: SignalSema uid=0x%x would exceed maxc (%d -> %d capped at %d)\n", A0, m->count, new_count, m->maxc);
        new_count = m->maxc;
    }
    m->count = new_count;
    sched_wake(A0);
    sched_preempt();    /* a woken higher-priority waiter runs immediately */
    return 0;
}
/* Unchanged behavior: the two literals below are now the same named constants the
 * blocking forms use, so the family cannot drift to two different spellings of
 * one error. sceKernelPollSema has no context gate because it never blocks. */
static uint32_t h_PollSema(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    int need = (int)A1;
    if (need <= 0 || need > m->maxc) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (m->count < need) return 0x800201adu;   /* SCE_KERNEL_ERROR_SEMA_ZERO */
    m->count -= need;
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Read-only semaphore probe for the executable HLE selftest (issue #43).
 *
 * The count-validation regressions must assert that a REJECTED wait mutated
 * nothing, and no registered NID can observe the count without changing it:
 * sceKernelPollSema decrements on success and sceKernelReferSemaStatus is not
 * registered. This reads the real production Sync entry and is compiled only
 * into the test executable. It adds no field and changes no layout. */
int sr_hle_test_sema_state(uint32_t uid, int *count_out, int *max_out) {
    Sync *m = sync_find(uid);
    if (!m) return 0;
    if (count_out) *count_out = m->count;
    if (max_out)   *max_out   = m->maxc;
    return 1;
}
#endif /* SR_HLE_THREAD_SELFTEST */

/* ---------------------------------------------------------------------------
 * Lightweight mutexes.
 *
 * These were previously registered as no-ops (Create -> h_CreateSema, every
 * other entry -> h_ok) under a comment justifying it as "single-threaded
 * recompiler".  That premise is false: this runtime schedules real PSP threads
 * cooperatively (src/rt/sched.c), and a retained boot trace shows two distinct
 * guest threads locking the same LwMutex, so the no-op registration silently
 * removed mutual exclusion between them.
 *
 * The old registration was also wrong on the ABI.  sceKernelCreateLwMutex takes
 * (workarea, name, attr, initialCount, opt), so routing it to h_CreateSema --
 * which reads (name, attr, initCount, maxCount) -- shifted every argument by
 * one register: the semaphore was created with count=attr and max=initialCount.
 * It returned the uid where the API returns 0 on success, and it never wrote
 * the caller's workarea at all.
 *
 * The workarea is the whole point of an LwMutex: it lives in guest memory and
 * the guest reads lockLevel/lockThread/uid out of it directly.  Layout is fixed
 * by pspthreadman.h (SceLwMutexWorkarea):
 *
 *     +0x00 int   lockLevel        recursion count; 0 == unlocked
 *     +0x04 SceUID lockThread      owning thread uid; 0 when unlocked
 *     +0x08 int   attr
 *     +0x0c int   numWaitThreads
 *     +0x10 SceUID uid             kernel object id
 *     +0x14 int   pad[3]
 *
 * Error returns for the abusive cases (unlocking something you do not own,
 * non-recursive relock) are deliberately NOT invented here.  Those codes are
 * unverified, and returning a wrong error where hardware returns success would
 * be a worse defect than the permissive behaviour this replaces.  They are
 * left permissive-but-state-coherent, log under SR_HLELOG, and are the subject
 * of the lwmutex-semantics oracle case.  Do not "tidy" them into guesses.
 * ------------------------------------------------------------------------- */
#define LWMUTEX_WORKAREA_SIZE 0x20u
#define LWMUTEX_LOCK_LEVEL    0x00u
#define LWMUTEX_LOCK_THREAD   0x04u
#define LWMUTEX_ATTR          0x08u
#define LWMUTEX_NUM_WAIT      0x0cu
#define LWMUTEX_UID           0x10u
#define LWMUTEX_ATTR_RECURSIVE 0x0200u

/* The guest hands us a pointer; validate the entire struct, not just its first
 * word, before any access.  A partially-mapped workarea must be refused. */
static int lwmutex_workarea_ok(uint32_t wa) {
    return wa && sr_guest_span_writable(wa, LWMUTEX_WORKAREA_SIZE);
}

static uint32_t h_CreateLwMutex(CpuState *s) {
    /* a0=workarea, a1=name, a2=attr, a3=initialCount, [sp+16]=opt. */
    uint32_t wa = A0, attr = A2;
    int initial = (int)A3;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;   /* ILLEGAL_ADDR */
    Sync *m = sync_new(); if (!m) return 0x80020000;
    MEM_W32(wa + LWMUTEX_LOCK_LEVEL, (uint32_t)initial);
    MEM_W32(wa + LWMUTEX_LOCK_THREAD, initial > 0 ? sched_current_uid() : 0u);
    MEM_W32(wa + LWMUTEX_ATTR, attr);
    MEM_W32(wa + LWMUTEX_NUM_WAIT, 0u);
    MEM_W32(wa + LWMUTEX_UID, m->uid);
    if (hle_log_on())
        fprintf(stderr, "HLE: CreateLwMutex wa=0x%08x uid=0x%x attr=0x%x initial=%d (from uid=0x%x)\n",
                wa, m->uid, attr, initial, sched_current_uid());
    return 0;   /* the API returns 0 on success; the uid goes in the workarea */
}

static uint32_t h_DeleteLwMutex(CpuState *s) {
    uint32_t wa = A0;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;
    uint32_t uid = MEM_R32(wa + LWMUTEX_UID);
    Sync *m = sync_find(uid);
    if (m) m->used = 0;
    /* Anything still blocked here would wait forever otherwise. */
    sched_wake(uid);
    MEM_W32(wa + LWMUTEX_LOCK_LEVEL, 0u);
    MEM_W32(wa + LWMUTEX_LOCK_THREAD, 0u);
    MEM_W32(wa + LWMUTEX_UID, 0u);
    return 0;
}

/* Shared by Lock/TryLock/LockCB. `blocking` selects whether contention waits.
 * Returns 0 when the lock was taken, 1 when it was not (TryLock only). */
static int lwmutex_acquire(uint32_t wa, int count, int blocking) {
    uint32_t cur = sched_current_uid();
    for (;;) {
        int level = (int)MEM_R32(wa + LWMUTEX_LOCK_LEVEL);
        uint32_t owner = MEM_R32(wa + LWMUTEX_LOCK_THREAD);
        if (level == 0) {
            MEM_W32(wa + LWMUTEX_LOCK_LEVEL, (uint32_t)count);
            MEM_W32(wa + LWMUTEX_LOCK_THREAD, cur);
            return 0;
        }
        if (owner == cur) {
            /* Recursive relock. Without the RECURSIVE attribute this is a
             * caller error on hardware; the exact code is unverified, so we
             * stay permissive and keep the count coherent rather than guess. */
            if (!(MEM_R32(wa + LWMUTEX_ATTR) & LWMUTEX_ATTR_RECURSIVE) && hle_log_on())
                fprintf(stderr, "HLE: LockLwMutex wa=0x%08x recursive relock without "
                                "PSP_LW_MUTEX_ATTR_RECURSIVE (uid=0x%x)\n", wa, cur);
            MEM_W32(wa + LWMUTEX_LOCK_LEVEL, (uint32_t)(level + count));
            return 0;
        }
        if (!blocking) return 1;
        /* Contention accounting. Under the previous no-op registration every one
         * of these was a silently unserialised critical section, so the count is
         * the direct measure of what the no-op was costing. Bounded logging: the
         * first few, then sampled, so a hot mutex cannot distort the run it is
         * measuring. */
        {
            static unsigned long blocks = 0;
            if (++blocks <= 8u || (blocks % 1000u) == 0u)
                fprintf(stderr, "LWMUTEX_CONTENDED n=%lu wa=0x%08x owner=0x%x waiter=0x%x level=%d\n",
                        blocks, wa, owner, cur, level);
        }
        uint32_t uid = MEM_R32(wa + LWMUTEX_UID);
        MEM_W32(wa + LWMUTEX_NUM_WAIT, MEM_R32(wa + LWMUTEX_NUM_WAIT) + 1u);
        sched_block_on(uid);
        uint32_t waiters = MEM_R32(wa + LWMUTEX_NUM_WAIT);
        if (waiters) MEM_W32(wa + LWMUTEX_NUM_WAIT, waiters - 1u);
        if (!sync_find(uid)) return 1;   /* deleted while we waited */
    }
}

static uint32_t h_LockLwMutex(CpuState *s) {
    /* a0=workarea, a1=lockCount, a2=timeout ptr. */
    uint32_t wa = A0;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;
    int count = (int)A1; if (count <= 0) return 0x800200d2u;  /* ILLEGAL_ARGUMENT */
    return lwmutex_acquire(wa, count, 1) ? 0x800201b5u /* WAIT_DELETE */ : 0u;
}

static uint32_t h_TryLockLwMutex(CpuState *s) {
    uint32_t wa = A0;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;
    int count = (int)A1; if (count <= 0) return 0x800200d2u;
    /* Contended TryLock must report failure. The precise code is unverified;
     * ILLEGAL_ARGUMENT is not it, so report the generic thread-man failure and
     * let the oracle case replace this with the measured value. */
    return lwmutex_acquire(wa, count, 0) ? 0x80020000u : 0u;
}

static uint32_t h_UnlockLwMutex(CpuState *s) {
    /* a0=workarea, a1=unlockCount. */
    uint32_t wa = A0;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;
    int count = (int)A1; if (count <= 0) return 0x800200d2u;
    int level = (int)MEM_R32(wa + LWMUTEX_LOCK_LEVEL);
    uint32_t owner = MEM_R32(wa + LWMUTEX_LOCK_THREAD);
    uint32_t cur = sched_current_uid();
    if (level <= 0 || owner != cur) {
        /* Not ours to release. Leave the state alone so the real owner's
         * bookkeeping survives; the hardware error code is unverified. */
        if (hle_log_on())
            fprintf(stderr, "HLE: UnlockLwMutex wa=0x%08x from uid=0x%x but level=%d owner=0x%x\n",
                    wa, cur, level, owner);
        return 0;
    }
    level -= count;
    if (level < 0) level = 0;
    MEM_W32(wa + LWMUTEX_LOCK_LEVEL, (uint32_t)level);
    if (level == 0) {
        MEM_W32(wa + LWMUTEX_LOCK_THREAD, 0u);
        sched_wake(MEM_R32(wa + LWMUTEX_UID));
        sched_preempt();
    }
    return 0;
}

/* sceKernelReferLwMutexStatus / ...ByID stay registered as h_ok. Their
 * SceKernelLwMutexInfo layout is not in pspthreadman.h and this project has no
 * measured record of it, so writing a struct here would be invention, not
 * implementation. The gap stays visible until the oracle case measures it. */

static uint32_t h_CreateEventFlag(CpuState *s) {
    /* a0=name, a1=attr, a2=initPattern, a3=opt. */
    Sync *m = sync_new(); if (!m) return 0x80020000;
    m->pattern = A2;
    return m->uid;
}
static uint32_t h_DeleteEventFlag(CpuState *s) { Sync *m = sync_find(A0); if (m) m->used = 0; return 0; }
static uint32_t h_SetEventFlag(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    m->pattern |= A1; sched_wake(A0); sched_preempt(); return 0;
}
static uint32_t h_ClearEventFlag(CpuState *s) {
    /* sceKernelClearEventFlag(evfid, bits): A1 is the mask of bits to KEEP
     * (currentPattern &= bits), matching PSP/PPSSPP. It is NOT a mask of bits
     * to remove â€” inverting A1 here inverts the contract. */
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    m->pattern = sr_evf_clear_pattern(m->pattern, A1); return 0;
}
static uint32_t h_WaitEventFlag(CpuState *s) {
    /* a0=uid, a1=bits, a2=mode, a3=outBits, [sp+16]=timeout. */
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3, toptr = stack_arg(s, 0);
    uint32_t rc = sr_evf_check_wait_args(bits, mode);
    if (rc) return rc;
    Sync *m = sync_find(uid); if (!m) return 0x80020000;
    if (hle_log_on())
        fprintf(stderr, "HLE: WaitEventFlag uid=0x%x bits=0x%x pattern=0x%x (from 0x%x)\n", uid, bits, m->pattern, sched_current_uid());
    /* Third in line, and the ordering is the whole point. sr_evf_check_wait_args()
     * above already returned ILLEGAL_MODE for mode 0xFF, which hardware confirms beats
     * the context error (L72/L73) -- that cell must keep returning 0x80020195. The
     * object lookup keeps its precedence too, and an already-satisfied pattern is
     * consumed normally. Only a pattern that does not match yet is a genuine wait.
     * A rejected call writes no outBits and consumes no pattern. (L74/L75) */
    if (!sr_evf_matches(m->pattern, bits, mode) && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    while (!sr_evf_matches(m->pattern, bits, mode)) {
        if (toptr) {
            uint32_t usec = MEM_R32(toptr);
            if (sched_block_on_timeout(uid, usec)) { if (outp) MEM_W32(outp, m->pattern); return 0x800201A8; }
        } else {
            sched_block_on(uid);
        }
        m = sync_find(uid); if (!m) return 0x80020000;
    }
    if (outp) MEM_W32(outp, m->pattern);         /* outBits = pre-consume pattern */
    m->pattern = sr_evf_consume(m->pattern, bits, mode);
    return 0;
}
static uint32_t h_WaitEventFlagCB(CpuState *s) {
    /* a0=uid, a1=bits, a2=mode, a3=outBits, [sp+16]=timeout. */
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3, toptr = stack_arg(s, 0);
    uint32_t rc = sr_evf_check_wait_args(bits, mode);
    if (rc) return rc;
    Sync *m = sync_find(uid); if (!m) return 0x80020000;
    if (hle_log_on())
        fprintf(stderr, "HLE: WaitEventFlagCB uid=0x%x bits=0x%x pattern=0x%x (from 0x%x)\n", uid, bits, m->pattern, sched_current_uid());
    /* Same ordering as the non-CB form, and ahead of the clock sample and callback
     * loop so a rejected call leaves both untouched. ILLEGAL_MODE still wins (L82/L83);
     * an already-set pattern still returns 0. (L84/L85) */
    if (!sr_evf_matches(m->pattern, bits, mode) && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;

    sched_vtime_refresh();
    uint64_t start = sched_vtime_us();
    uint64_t end = start + (toptr ? MEM_R32(toptr) : 0);

    while (!sr_evf_matches(m->pattern, bits, mode)) {
        if (sr_thread_has_pending_callbacks(sched_current_uid())) {
            sr_thread_dispatch_callbacks();
            m = sync_find(uid); if (!m) return 0x80020000;
            sched_vtime_refresh();
            continue;
        }
        if (toptr) {
            sched_vtime_refresh();
            if (sched_vtime_us() >= end) { if (outp) MEM_W32(outp, m->pattern); return 0x800201A8; }
            uint32_t remaining = (uint32_t)(end - sched_vtime_us());
            MEM_W32(toptr, remaining);
            sched_set_current_cb_wait(1);
            int timed_out = sched_block_on_timeout(uid, remaining);
            sched_set_current_cb_wait(0);
            if (timed_out) { if (outp) MEM_W32(outp, m->pattern); return 0x800201A8; }
        } else {
            sched_set_current_cb_wait(1);
            sched_block_on(uid);
            sched_set_current_cb_wait(0);
        }
        m = sync_find(uid); if (!m) return 0x80020000;
        sched_vtime_refresh();
    }
    if (outp) MEM_W32(outp, m->pattern);
    m->pattern = sr_evf_consume(m->pattern, bits, mode);
    return 0;
}
static uint32_t h_PollEventFlag(CpuState *s) {
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3;
    uint32_t rc = sr_evf_check_poll_args(bits, mode);
    if (rc) return rc;
    Sync *m = sync_find(uid); if (!m) return 0x80020000;
    if (!sr_evf_matches(m->pattern, bits, mode)) {
        if (outp) MEM_W32(outp, m->pattern);
        return SR_EVF_ERR_COND;
    }
    if (outp) MEM_W32(outp, m->pattern);         /* outBits = pre-consume pattern */
    m->pattern = sr_evf_consume(m->pattern, bits, mode);
    return 0;
}
/* sceKernelReferEventFlagStatus(uid, SceKernelEventFlagInfo *info): size(0), name[32](4),
 * attr(36), initPattern(40), currentPattern(44), numWaitThreads(48). Size stays as the caller
 * wrote it; we don't track init pattern or waiters separately. */
static uint32_t h_ReferEventFlagStatus(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    uint32_t info = A1; if (!info) return 0x80020000;
    for (int i = 0; i < 32; i++) MEM_W8(info + 4 + (uint32_t)i, 0);
    MEM_W32(info + 36, 0x200);          /* PSP_EVENT_WAITMULTIPLE */
    MEM_W32(info + 40, m->pattern);
    MEM_W32(info + 44, m->pattern);
    MEM_W32(info + 48, 0);
    return 0;
}

static void hle_register_thread_exit_handlers(void) {
    sr_hle_register(0xaa73c935, "sceKernelExitThread", h_ExitThread);
    sr_hle_register(0x809ce29b, "sceKernelExitDeleteThread", h_ExitDeleteThread);
    sr_hle_register(0x278c0df5, "sceKernelWaitThreadEnd", h_WaitThreadEnd);
    sr_hle_register(0x840e8133, "sceKernelWaitThreadEndCB", h_WaitThreadEndCB);
}

/* The host PSP oracle mode is an extension of hle_thread_selftest, not a second
 * implementation.  Keep the small set of public NIDs it exercises in one registry
 * helper so every case still enters the production sr_syscall path. */
static void hle_register_selftest_oracle_handlers(void) {
    sr_hle_register(0x446d8de6, "sceKernelCreateThread", h_CreateThread);
    sr_hle_register(0xf475845d, "sceKernelStartThread", h_StartThread);
    sr_hle_register(0x293b45b8, "sceKernelGetThreadId", h_GetThreadIdSched);
    sr_hle_register(0x3b183e26, "sceKernelGetThreadExitStatus", h_GetThreadExitStatus);
    sr_hle_register(0x9fa03cd3, "sceKernelDeleteThread", h_DeleteThread);
    sr_hle_register(0x383f7bcc, "sceKernelTerminateDeleteThread", h_TerminateDeleteThread);
    sr_hle_register(0xd59ead2f, "sceKernelWakeupThread", h_WakeupThread);
    sr_hle_register(0x9ace131e, "sceKernelSleepThread", h_SleepThread);
    sr_hle_register(0xe81caf8f, "sceKernelCreateCallback", h_CreateCallback);
    sr_hle_register(0xedba5844, "sceKernelDeleteCallback", h_DeleteCallback);
    sr_hle_register(0xc11ba8c4, "sceKernelNotifyCallback", h_NotifyCallback);
    sr_hle_register(0x349d6d6c, "sceKernelCheckCallback", h_CheckCallback);
    sr_hle_register(0xba4051d6, "sceKernelCancelCallback", h_CancelCallback);
    sr_hle_register(0x2a3d44ff, "sceKernelGetCallbackCount", h_GetCallbackCount);
    sr_hle_register(0xd6da4ba1, "sceKernelCreateSema", h_CreateSema);
    sr_hle_register(0x28b6489c, "sceKernelDeleteSema", h_DeleteSema);
    sr_hle_register(0x3f53e640, "sceKernelSignalSema", h_SignalSema);
    sr_hle_register(0x58b1f937, "sceKernelPollSema", h_PollSema);
}

/* Registry scope for the issue #88 wait/blocking-context conformance matrix
 * (src/rt/intr_conformance.h).
 *
 * sr_hle_init()'s SR_HLE_THREAD_SELFTEST branch deliberately registers a narrow
 * set, so before this helper existed the executable harness could reach only 6 of
 * the 37 NIDs that PSPAutotests tests/intr/waits.expected covers -- including
 * neither sceKernelRegisterSubIntrHandler nor sceKernelEnableSubIntr, without
 * which no interrupt context can be constructed at all.
 *
 * Every line below was MOVED verbatim out of sr_hle_init()'s production branch,
 * which now calls this helper instead. There is one definition, reached by both
 * builds, so the harness cannot end up measuring a test-only mapping and no
 * handler behavior changes on either side. Nothing was added, removed, or
 * rewritten in the move: tools/hle_manifest.py reports the identical 375
 * (nid, name, handler) triples before and after it.
 *
 * Order is not behavior here. The registry is a unique-NID set consulted by a
 * linear hle_find() scan, and sr_hle_register() rejects a duplicate NID outright,
 * so moving these registrations earlier in the sequence cannot change which
 * handler any NID resolves to. */
static void hle_register_wait_conformance_handlers(void) {
    sr_hle_register(0xceadeb47, "sceKernelDelayThread", h_DelayThread);
    sr_hle_register(0x68da9e36, "sceKernelDelayThreadCB", h_DelayThreadCB);
    sr_hle_register(0x82826f70, "sceKernelSleepThreadCB", h_SleepThreadCB);
    sr_hle_register(0x36cdfade, "sceDisplayWaitVblank", h_DisplayWaitVblank);
    sr_hle_register(0x984c27e7, "sceDisplayWaitVblankStart", h_DisplayWaitVblank);
    sr_hle_register(0x4e3a1105, "sceKernelWaitSema", h_WaitSema);
    sr_hle_register(0x6d212bac, "sceKernelWaitSemaCB", h_WaitSemaCB);
    sr_hle_register(0x55c20a00, "sceKernelCreateEventFlag", h_CreateEventFlag);
    sr_hle_register(0x1fb15a32, "sceKernelSetEventFlag", h_SetEventFlag);
    sr_hle_register(0x402fcf22, "sceKernelWaitEventFlag", h_WaitEventFlag);
    sr_hle_register(0x328c546a, "sceKernelWaitEventFlagCB", h_WaitEventFlagCB);
    sr_hle_register(0xc07bb470, "sceKernelCreateFpl", h_CreateFpl);
    sr_hle_register(0xd979e9bf, "sceKernelAllocateFpl", h_AllocateFpl);
    sr_hle_register(0xe7282cb6, "sceKernelAllocateFplCB", h_AllocateFpl);
    /* Not a matrix NID -- waits.cpp never probes a Try form. It is here so the
     * selftest can pin, through production dispatch, that splitting the blocking
     * Allocate forms off this handler left it alone. Registered by this same
     * helper in both builds, so the production mapping is unchanged. */
    sr_hle_register(0x623ae665, "sceKernelTryAllocateFpl", h_TryAllocateFpl);
    /* Same reason, plus the selftest must hand every pool slot back: s_fpls[] holds
     * FPL_MAX=16 and the conformance matrix already uses all 16, so a test that
     * leaked one would starve the matrix rather than fail on its own assertion. */
    sr_hle_register(0xed1410e0, "sceKernelDeleteFpl", h_DeleteFpl);
    sr_hle_register(0xb7d098c6, "sceKernelCreateMutex", h_CreateSema);
    sr_hle_register(0xb011b11f, "sceKernelLockMutex", h_ok);
    sr_hle_register(0x5bf4dd27, "sceKernelLockMutexCB", h_ok);
    sr_hle_register(0x19cff145, "sceKernelCreateLwMutex", h_CreateLwMutex);
    sr_hle_register(0xbea46419, "sceKernelLockLwMutex", h_LockLwMutex);
    sr_hle_register(0x1fc64e09, "sceKernelLockLwMutexCB", h_LockLwMutex);
    sr_hle_register(0x3e0271d3, "sceKernelVolatileMemLock", h_VolatileMemLock);
    sr_hle_register(0x8ef08fce, "sceUmdWaitDriveStat", h_UmdWaitDriveStat);
    sr_hle_register(0x56202973, "sceUmdWaitDriveStatWithTimer", h_UmdWaitDriveStatWithTimer);
    sr_hle_register(0x4a9e5e29, "sceUmdWaitDriveStatCB", h_UmdWaitDriveStatCB);
    sr_hle_register(0x1f803938, "sceCtrlReadBufferPositive", h_CtrlReadBuffer);
    sr_hle_register(0x6a638d83, "sceIoRead", h_IoRead);
    sr_hle_register(0x42ec03ac, "sceIoWrite", h_IoWrite);
    sr_hle_register(0xe23eec33, "sceIoWaitAsync", h_IoWaitAsync);
    sr_hle_register(0x35dbd746, "sceIoWaitAsyncCB", h_IoWaitAsyncCB);
    sr_hle_register(0xca04a2b9, "sceKernelRegisterSubIntrHandler", h_RegisterSubIntr);
    sr_hle_register(0xfb8e22ec, "sceKernelEnableSubIntr", h_EnableSubIntr);
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Registry membership probe for the conformance harness. sr_syscall() calls
 * _Exit(7) on an unregistered NID under the fiber scheduler, so a matrix cell
 * whose NID is out of registry scope must be detected without calling it. */
int sr_hle_test_is_registered(uint32_t nid) { return hle_find(nid) != NULL; }

/* See the forward declaration next to the counters themselves. */
void sr_display_test_flip_counts(unsigned long *calls, unsigned long *immediate,
                                 unsigned long *latched, unsigned long *rejected,
                                 uint32_t *last_err) {
    if (calls)     *calls     = s_setfb.calls;
    if (immediate) *immediate = s_setfb.immediate;
    if (latched)   *latched   = s_setfb.latched;
    if (rejected)  *rejected  = s_setfb.rejected;
    if (last_err)  *last_err  = s_setfb.last_err;
}

/* See the forward declaration next to the counters themselves. */
void sr_watchdog_test_state(unsigned long *fires, uint32_t *vblanks_since_flip) {
    if (fires)             *fires             = s_watchdog_fires;
    if (vblanks_since_flip) *vblanks_since_flip = s_vcount - s_last_flip_vcount;
}
#endif /* SR_HLE_THREAD_SELFTEST */

static void hle_register_time_handlers(void) {
    sr_hle_register(0x092968f4, "sceKernelCpuSuspendIntr", h_CpuSuspendIntr);
    sr_hle_register(0x5f10d406, "sceKernelCpuResumeIntr", h_CpuResumeIntr);
    sr_hle_register(0x3b84732d, "sceKernelCpuResumeIntrWithSync", h_CpuResumeIntrWithSync);
    sr_hle_register(0x47a0b729, "sceKernelIsCpuIntrSuspended", h_CpuIsIntrSuspended);
    sr_hle_register(0xb55249d2, "sceKernelIsCpuIntrEnable", h_CpuIsIntrEnable);
    sr_hle_register(0x3ad58b8c, "sceKernelSuspendDispatchThread", h_SuspendDispatchThread);
    sr_hle_register(0x27e22ec2, "sceKernelResumeDispatchThread", h_ResumeDispatchThread);
    sr_hle_register(0x369ed59d, "sceKernelGetSystemTimeLow", h_GetSystemTimeLow);
    sr_hle_register(0x82bc5777, "sceKernelGetSystemTimeWide", h_GetSystemTimeWide);
    sr_hle_register(0xdb738f35, "sceKernelGetSystemTime", h_GetSystemTime);
    sr_hle_register(0x6ff40acc, "sceRtcGetTick", h_RtcGetTick);
    sr_hle_register(0x7ed29e40, "sceRtcSetTick", h_RtcSetTick);
    sr_hle_register(0xcf561893, "sceRtcGetWin32FileTime", h_RtcGetWin32FileTime);
    sr_hle_register(0x4cfa57b0, "sceRtcGetCurrentClock", h_RtcGetCurrentClock);
    sr_hle_register(0xe7c27d1b, "sceRtcGetCurrentClockLocalTime", h_RtcGetCurrentClockLocal);
    sr_hle_register(0x3f7ad767, "sceRtcGetCurrentTick", h_RtcGetCurrentTick);
    sr_hle_register(0x34885e0d, "sceRtcConvertUtcToLocalTime", h_RtcConvertUtcToLocal);
    sr_hle_register(0x779242a2, "sceRtcConvertLocalTimeToUTC", h_RtcConvertLocalToUtc);
    sr_hle_register(0x91e4f6a7, "sceKernelLibcClock", h_LibcClock);
    sr_hle_register(0x27cc57f0, "sceKernelLibcTime", h_LibcTime);
    sr_hle_register(0x71ec4271, "sceKernelLibcGettimeofday", h_LibcGettimeofday);
}

static void hle_register_display_handlers(void) {
    sr_hle_register(0x289d82fe, "sceDisplaySetFrameBuf", h_DisplaySetFrameBuf);
    sr_hle_register(0xeeda2e54, "sceDisplayGetFrameBuf", h_DisplayGetFrameBuf);
    /* 0x36cdfade is sceDisplayWaitVblank (issue #83); the CB variant is 0x46f186c3 and
     * remains unregistered until the callback-aware wait transaction lands. */
    sr_hle_register(0x0e20f177, "sceDisplaySetMode", h_DisplaySetMode);
    sr_hle_register(0x9c6eaad7, "sceDisplayGetVcount", h_DisplayGetVcount);
    sr_hle_register(0x773dd3a3, "sceDisplayGetCurrentHcount", h_DisplayGetCurrentHcount);
    sr_hle_register(0x210eab3a, "sceDisplayGetAccumulatedHcount", h_DisplayGetAccumulatedHcount);
    sr_hle_register(0x4d4e10ec, "sceDisplayIsVblank", h_DisplayIsVblank);
    sr_hle_register(0xdea197d4, "sceDisplayGetMode", h_DisplayGetMode);
    sr_hle_register(0xdba6c4c4, "sceDisplayGetFramePerSec", h_DisplayGetFramePerSec);
}

static void hle_register_ge_handlers(void) {
    /* sceGe_user */
    sr_hle_register(0xab49e76a, "sceGeListEnQueue", h_GeListEnQueue);
    sr_hle_register(0xb287bd61, "sceGeDrawSync", h_GeDrawSync);
    sr_hle_register(0xe47e40e4, "sceGeEdramGetAddr", h_GeEdramGetAddr);
    sr_hle_register(0xa4fc06a4, "sceGeSetCallback", h_GeSetCallback);
    sr_hle_register(0x03444eb4, "sceGeListSync", h_GeListSync);
    sr_hle_register(0x05db22ce, "sceGeUnsetCallback", h_GeUnsetCallback);
    sr_hle_register(0x1f6752ad, "sceGeEdramGetSize", h_GeEdramGetSize);
    sr_hle_register(0xe0d68148, "sceGeListUpdateStallAddr", h_GeListUpdateStallAddr);
}

static void hle_register_atrac_handlers(void) {
    /* sceAtrac3plus (control flow only; silence output). */
    sr_hle_register(0x7a20e7af, "sceAtracSetDataAndGetID", h_AtracSetDataAndGetID);
    sr_hle_register(0x61eb33f5, "sceAtracReleaseAtracID", h_AtracReleaseAtracID);
    sr_hle_register(0x6a8c3cd5, "sceAtracDecodeData", h_AtracDecodeData);
    sr_hle_register(0x9ae849a7, "sceAtracGetRemainFrame", h_AtracGetRemainFrame);
    sr_hle_register(0x5d268707, "sceAtracGetStreamDataInfo", h_AtracGetStreamDataInfo);
    sr_hle_register(0x7db31251, "sceAtracAddStreamData", h_AtracAddStreamData);
    sr_hle_register(0xe23e3a35, "sceAtracGetNextDecodePosition", h_AtracGetNextDecodePosition);
    sr_hle_register(0xa2bba8be, "sceAtracGetSoundSample", h_AtracGetSoundSample);
    sr_hle_register(0xfaa4f89b, "sceAtracGetLoopStatus", h_AtracGetLoopStatus);
    sr_hle_register(0x868120b5, "sceAtracSetLoopNum", h_AtracSetLoopNum);
    sr_hle_register(0x644e5607, "sceAtracResetPlayPosition", h_AtracResetPlayPosition);
    /* Additional sceAtrac stubs (not yet modelled; accepted to unblock init) */
    sr_hle_register(0x132f1eca, "sceAtracReinit", h_ok);
    sr_hle_register(0x0fae370e, "sceAtracSetHalfwayBufferAndGetID", h_AtracSetDataAndGetID);
    sr_hle_register(0x3f6e26b5, "sceAtracSetHalfwayBuffer", h_AtracSetDataAndGetID);
    sr_hle_register(0x0e2a73ab, "sceAtracSetData", h_AtracSetData);
    sr_hle_register(0x780f88d1, "sceAtracGetAtracID", h_AtracGetAtracID);
    sr_hle_register(0x2dd3e298, "sceAtracGetBufferInfoForResetting", h_ok);
    sr_hle_register(0xca3ca3d2, "sceAtracGetBufferInfoForReseting", h_ok);
    /* 0x31668bba was a single-nibble transcription error for 0x31668baa (the
     * canonical NID in src/rt/nid_names.h, reproducible as sha1(name)[0:4]).
     * No guest import can carry the typo, so this registration was previously
     * unreachable and a real sceAtracGetChannel call was an unhandled-NID miss.
     * Correcting it makes the h_ok acceptance actually take effect: the call
     * now returns fake success without reporting a channel count. That gap is
     * tracked by #286 and must not be read as sceAtracGetChannel being modelled. */
    sr_hle_register(0x31668baa, "sceAtracGetChannel", h_ok);
    sr_hle_register(0x36faabfb, "sceAtracGetNextSample", h_AtracGetNextSample);
    sr_hle_register(0xa554a158, "sceAtracGetBitrate", h_ok);
    sr_hle_register(0xb3b5d042, "sceAtracGetOutputChannel", h_ok);
    sr_hle_register(0xd6a5f2f7, "sceAtracGetMaxSample", h_ok);
    sr_hle_register(0x5622b7c1, "sceAtracSetAA3DataAndGetID", h_AtracSetDataAndGetID);
    sr_hle_register(0x5dd66588, "sceAtracSetAA3HalfwayBufferAndGetID", h_AtracSetDataAndGetID);
    sr_hle_register(0x472e3825, "sceAtracSetMOutDataAndGetID", h_AtracSetDataAndGetID);
    sr_hle_register(0x5cf9d852, "sceAtracSetMOutHalfwayBuffer", h_AtracSetDataAndGetID);
    sr_hle_register(0x9cd7de03, "sceAtracSetMOutHalfwayBufferAndGetID", h_AtracSetDataAndGetID);
    sr_hle_register(0xf6837a1a, "sceAtracSetMOutData", h_AtracSetDataAndGetID);
    sr_hle_register(0x83bf7afd, "sceAtracSetSecondBuffer", h_ok);
    sr_hle_register(0x83e85ea0, "sceAtracGetSecondBufferInfo", h_ok);
    sr_hle_register(0xd5c28cc0, "sceAtracReleaseResources", h_ok);
    sr_hle_register(0xd1f59fdb, "sceAtracStartEntry", h_ok);
    sr_hle_register(0xeca32a99, "sceAtracIsSecondBufferNeeded", h_ok);
    sr_hle_register(0xe88f759b, "sceAtracGetInternalErrorInfo", h_ok);
    sr_hle_register(0x231fc6b7, "_sceAtracGetContextAddress", h_ok);
    sr_hle_register(0x1575d64b, "sceAtracLowLevelInitDecoder", h_ok);
    sr_hle_register(0x0c116e1b, "sceAtracLowLevelDecode", h_ok);
    sr_hle_register(0x707b7629, "sceMpegFlushAllStream", h_ok);
}

/* sceSasCore: stateful SAS registrations.  This helper is called outside the selftest
 * gate, exactly like the sceAtrac family, so the executable HLE harness dispatches the
 * same registrations the game does instead of carrying a second, drifting test mapping. */
static void hle_register_sas_handlers(void) {
    sr_hle_register(0x68a46b95, "__sceSasGetEndFlag", h_SasGetEndFlag);
    sr_hle_register(0xa3589d81, "__sceSasCore", h_SasCore);
    sr_hle_register(0x50a14dfc, "__sceSasCoreWithMix", h_SasCoreWithMix);
    sr_hle_register(0x76f01aca, "__sceSasSetKeyOn", h_SasSetKeyOn);
    sr_hle_register(0xa0cf2fa4, "__sceSasSetKeyOff", h_SasSetKeyOff);
    sr_hle_register(0x42778a9f, "__sceSasInit", h_SasInit);
    sr_hle_register(0x99944089, "__sceSasSetVoice", h_SasSetVoice);
    sr_hle_register(0xad84d37f, "__sceSasSetPitch", h_SasSetPitch);
    sr_hle_register(0x440ca7d8, "__sceSasSetVolume", h_SasSetVolume);
    sr_hle_register(0x019b25eb, "__sceSasSetADSR", h_SasSetADSR);
    sr_hle_register(0x9ec3676a, "__sceSasSetADSRmode", h_SasSetADSRmode);
    sr_hle_register(0x33d4ab37, "__sceSasRevType", h_SasRevType);
    sr_hle_register(0x74ae582a, "__sceSasGetEnvelopeHeight", h_SasGetEnvelopeHeight);
    sr_hle_register(0x267a6dd2, "__sceSasRevParam", h_SasRevParam);
    sr_hle_register(0x2c8e6ab3, "__sceSasGetPauseFlag", h_SasGetPauseFlag);
    sr_hle_register(0x5f9529f6, "__sceSasSetSL", h_SasSetSL);
    sr_hle_register(0x787d04d5, "__sceSasSetPause", h_SasSetPause);
    sr_hle_register(0xb7660a23, "__sceSasSetNoise", h_SasSetNoise);
    sr_hle_register(0xcbcd4f79, "__sceSasSetSimpleADSR", h_SasSetSimpleADSR);
    sr_hle_register(0xd5a229c9, "__sceSasRevEVOL", h_SasRevEVOL);
    sr_hle_register(0xf983b186, "__sceSasRevVON", h_SasRevVON);
    sr_hle_register(0xe175ef66, "__sceSasGetOutputmode", h_SasGetOutputmode);
    sr_hle_register(0xe855bf76, "__sceSasSetOutputmode", h_SasSetOutputmode);
    sr_hle_register(0xd1e0a01e, "__sceSasSetGrain", h_SasSetGrain);
    sr_hle_register(0xbd11b7c2, "__sceSasGetGrain", h_SasGetGrain);
    sr_hle_register(0xd5ebbbcd, "__sceSasSetSteepWave", h_SasUnsupportedVoice);
    sr_hle_register(0xa232cbe6, "__sceSasSetTrianglarWave", h_SasUnsupportedVoice);
    sr_hle_register(0xe1cd9561, "__sceSasSetVoicePCM", h_SasSetVoicePCM);
    sr_hle_register(0x4aa9ead6, "__sceSasSetVoiceATRAC3", h_SasUnsupportedVoice);
    sr_hle_register(0x7497ea85, "__sceSasConcatenateATRAC3", h_SasUnsupportedVoice);
    sr_hle_register(0xf6107f00, "__sceSasUnsetATRAC3", h_SasUnsupportedVoice);
    sr_hle_register(0x07f58c24, "__sceSasGetAllEnvelopeHeights", h_SasGetAllEnvelopeHeights);
}

static void hle_register_utility_module_handlers(void) {
    sr_hle_register(0xc629af26, "sceUtilityLoadAvModule", h_UtilityLoadAvModule);
    sr_hle_register(0xf7d8d092, "sceUtilityUnloadAvModule", h_UtilityUnloadAvModule);
    sr_hle_register(0x2a6117a5, "sceUtilityLoadModule", h_UtilityLoadModule);
    sr_hle_register(0x2a2b3de0, "sceUtilityLoadModule", h_UtilityLoadModule);
    sr_hle_register(0x1579a30a, "sceUtilityUnloadModule", h_UtilityUnloadModule);
    sr_hle_register(0xe49bfe92, "sceUtilityUnloadModule", h_UtilityUnloadModule);
}

static void hle_register_bulk_memory_handlers(void) {
    sr_hle_register(0x617f3fe6, "sceDmacMemcpy", h_DmacMemcpy);
    /* Both DMAC copy NIDs register here rather than in the general table below,
     * so the executable regression enters the same registration the game does
     * instead of a test-only mapping. */
    sr_hle_register(0xd97f94d8, "sceDmacTryMemcpy", h_DmacTryMemcpy);
    sr_hle_register(0xa089eca4, "sceKernelMemset", h_Memset);
    sr_hle_register(0x1839852a, "sceKernelMemcpy", h_Memcpy);
}

/* Single definition called by both sr_hle_init() branches so the executable
 * regression dispatches the exact production sceKernelExitGame registration.
 * Terminating the host process is what stops the guest libc reentrancy guard
 * from looping. */
static void hle_register_exit_game_handler(void) {
    sr_hle_register(0x05572a5f, "sceKernelExitGame", h_ExitGame);
}

void sr_hle_init(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_hle_init_state, &expected, 1,
                                                  memory_order_acq_rel, memory_order_acquire)) {
        while (atomic_load_explicit(&s_hle_init_state, memory_order_acquire) != 2) { }
        return;
    }
    hle_fd_init();
    g_callcount = getenv("SR_CALLCOUNT") ? 1 : 0;
    hle_register_bulk_memory_handlers();
    hle_register_thread_exit_handlers();
    hle_register_selftest_oracle_handlers();
#ifdef SR_HLE_THREAD_SELFTEST
    /* The executable ThreadMan harness links this production translation unit
     * but intentionally registers only the family under test. Keeping both
     * public NIDs in the same helper used by the normal registry prevents the
     * test mapping from becoming a duplicate implementation. */
    hle_register_utility_module_handlers();
    hle_register_msgpipe_handlers();
    /* Wait/blocking APIs the issue #88 conformance matrix enters -- the same
     * definition the production branch below calls. */
    hle_register_wait_conformance_handlers();
    hle_register_regular_audio_handlers();
    hle_register_exit_game_handler();
    hle_register_ge_handlers();
#else
    /* Wait/blocking APIs shared with the issue #88 conformance matrix. Single
     * definition, called by both branches, so the selftest cannot drift from the
     * registry the game build uses. */
    hle_register_wait_conformance_handlers();
    /* Internal address callback, reached only after a normal dispatch-table miss. */
    sr_hle_register(0x00061e74u, "newlibModuleStreamWrite", h_ModuleStreamWrite);
    /* NID audit 2026-06: every entry below verified against PPSSPP's HLE tables. A handler on
     * the wrong NID is worse than none -- handlers that fill out-params write through whatever
     * the registers happen to hold for the REAL function's signature (the VolatileMemUnlock
     * mixup sprayed two wild words per asset load and zeroed the model-slot counter). */
    sr_hle_register(0x7591c7db, "sceKernelSetCompiledSdkVersion", h_SetCompiledSdkVersion);
    sr_hle_register(0x35669d4c, "sceKernelSetCompiledSdkVersion600_602", h_SetCompiledSdkVersion);
    sr_hle_register(0xf77d77cb, "sceKernelSetCompilerVersion", h_SetCompiledSdkVersion);
    sr_hle_register(0x237dbd4f, "sceKernelAllocPartitionMemory", h_AllocPartitionMemory);
    sr_hle_register(0x9d9a5ba1, "sceKernelGetBlockHeadAddr", h_GetBlockHeadAddr);
    sr_hle_register(0xb6d61d02, "sceKernelFreePartitionMemory", h_FreePartitionMemory);
    sr_hle_register(0xf919f628, "sceKernelTotalFreeMemSize", h_TotalFreeMemSize);
    sr_hle_register(0xa291f107, "sceKernelMaxFreeMemSize", h_MaxFreeMemSize);
    /* sceKernelTryAllocateFpl and sceKernelDeleteFpl moved to
     * hle_register_wait_conformance_handlers(), which this branch also calls --
     * same NIDs, names and handlers as before. */
    sr_hle_register(0xf6414a71, "sceKernelFreeFpl", h_FreeFpl);
    sr_hle_register(0x9f9b46b9, "sceKernelCreateNotifyCallback", h_CreateNotifyCallback);
    sr_hle_register(0x0ed48fe2, "sceKernelDeleteNotifyCallback", h_DeleteNotifyCallback);
    sr_hle_register(0x94aa61ee, "sceKernelGetThreadCurrentPriority", h_GetThreadPriority);
    sr_hle_register(0xfccfad26, "sceKernelCancelWakeupThread", h_CancelWakeupThread);
    sr_hle_register(0x71bc9871, "sceKernelChangeThreadPriority", h_ChangeThreadPriority);
    sr_hle_register(0xa66b0120, "sceKernelReferEventFlagStatus", h_ReferEventFlagStatus);
    sr_hle_register(0xffc36a14, "sceKernelReferThreadRunStatus", h_ReferThreadRunStatus);
    sr_hle_register(0xd8b73127, "sceKernelGetModuleIdByAddress", h_GetModuleId);
    /* Boot setup batch (return success / reference value). */
    sr_hle_register(0x4ac57943, "sceKernelRegisterExitCallback", h_RegisterExitCallback);
    sr_hle_register(0xa5da2406, "sceUtilityGetSystemParamInt", h_GetSystemParamInt);
    sr_hle_register(0x36aa6e91, "sceImposeSetLanguageMode", h_ok);
    /* sceUtility dialogs (OSK / savedata / netconf): no dialog active -> status 0, calls ok. */
    sr_hle_register(0xf3f76017, "sceUtilityOskGetStatus", h_OskGetStatus);
    sr_hle_register(0x4b85c861, "sceUtilityOskUpdate", h_OskUpdate);
    sr_hle_register(0x3dfaeba9, "sceUtilityOskShutdownStart", h_OskShutdown);
    sr_hle_register(0x1579a159, "sceUtilityLoadNetModule", h_ok);
    /* 0xf6269b82 is OskInitStart, NOT GetSystemParamString -- the old string handler wrote
     * A2 bytes through A1, both garbage for this signature. */
    sr_hle_register(0xf6269b82, "sceUtilityOskInitStart", h_OskInitStart);
    sr_hle_register(0x50c4cd57, "sceUtilitySavedataInitStart", h_SavedataInitStart);
    sr_hle_register(0x9790b33c, "sceUtilitySavedataShutdownStart", h_DlgShutdown);
    sr_hle_register(0x8874dbe0, "sceUtilitySavedataGetStatus", h_DlgGetStatus);
    sr_hle_register(0xd4b95ffb, "sceUtilitySavedataUpdate", h_SavedataUpdate);
    sr_hle_register(0x2ad8e239, "sceUtilityMsgDialogInitStart", h_MsgDialogInit);
    sr_hle_register(0x95fc253b, "sceUtilityMsgDialogUpdate", h_MsgDialogUpdate);
    sr_hle_register(0x9a1c91d7, "sceUtilityMsgDialogGetStatus", h_MsgDialogStatus);
    sr_hle_register(0x67af3428, "sceUtilityMsgDialogShutdownStart", h_MsgDialogShutdown);
    sr_hle_register(0x4db1e739, "sceUtilityNetconfInitStart", h_NetDialogInit);
    sr_hle_register(0x91e70e35, "sceUtilityNetconfUpdate", h_NetDialogUpdate);
    sr_hle_register(0x6332aa39, "sceUtilityNetconfGetStatus", h_NetDialogStatus);
    sr_hle_register(0xf88155f6, "sceUtilityNetconfShutdownStart", h_NetDialogShutdown);
    sr_hle_register(0x24ac31eb, "sceUtilityGamedataInstallInitStart", h_GamedataDialogInit);
    sr_hle_register(0x4aecd179, "sceUtilityGamedataInstallUpdate", h_GamedataDialogUpdate);
    sr_hle_register(0xb57e95d9, "sceUtilityGamedataInstallGetStatus", h_GamedataDialogStatus);
    sr_hle_register(0x32e32dcb, "sceUtilityGamedataInstallShutdownStart", h_GamedataDialogShutdown);
    sr_hle_register(0xc492f751, "sceUtilityGameSharingInitStart", h_SharingDialogInit);
    sr_hle_register(0x7853182d, "sceUtilityGameSharingUpdate", h_SharingDialogUpdate);
    sr_hle_register(0x946963f3, "sceUtilityGameSharingGetStatus", h_SharingDialogStatus);
    sr_hle_register(0xefc6f80f, "sceUtilityGameSharingShutdownStart", h_SharingDialogShutdown);
    sr_hle_register(0x64d50c56, "sceUtilityUnloadNetModule", h_ok);
    /* sceWlan: the game stamps saves/profiles with the console's MAC. A stable fake works
     * (PPSSPP behaviour); low 2 bits of byte 0 must be clear (locally-administered/multicast
     * OUI bits confuse some games -- PPSSPP masks them too). */
    sr_hle_register(0x0c622081, "sceWlanGetEtherAddr", h_WlanGetEtherAddr);
    sr_hle_register(0x93440b11, "sceWlanDevIsPowerOn", h_WlanOn);
    sr_hle_register(0xd7763699, "sceWlanGetSwitchState", h_WlanOn);
    sr_hle_register(0x04b7766e, "scePowerRegisterCallback", h_PowerRegisterCallback);
    sr_hle_register(0x46ebb729, "sceUmdCheckMedium", h_UmdCheckMedium);
    sr_hle_register(0xc6183d47, "sceUmdActivate", h_UmdActivate);
    sr_hle_register(0xaee7404d, "sceUmdRegisterUMDCallBack", h_UmdRegisterUMDCallBack);
    sr_hle_register(0x52089ca1, "sceKernelGetThreadStackFreeSize", h_GetThreadStackFreeSize);
    /* sceMpeg: faithful port (src/rt/mpeg.c). Drives the PSMF intro to completion. */
    sr_hle_register(0x682a619b, "sceMpegInit", h_MpegInit);
    sr_hle_register(0x874624d6, "sceMpegFinish", h_MpegFinish);
    sr_hle_register(0xc132e22f, "sceMpegQueryMemSize", h_MpegQueryMemSize);
    sr_hle_register(0xd8c5f121, "sceMpegCreate", h_MpegCreate);
    sr_hle_register(0x606a4649, "sceMpegDelete", h_MpegDelete);
    sr_hle_register(0x42560f23, "sceMpegRegistStream", h_MpegRegistStream);
    sr_hle_register(0x591a4aa2, "sceMpegUnRegistStream", h_MpegUnRegistStream);
    sr_hle_register(0x21ff80e4, "sceMpegQueryStreamOffset", h_MpegQueryStreamOffset);
    sr_hle_register(0x611e9e11, "sceMpegQueryStreamSize", h_MpegQueryStreamSize);
    sr_hle_register(0xd7a29f46, "sceMpegRingbufferQueryMemSize", h_MpegRingbufferQueryMemSize);
    sr_hle_register(0x37295ed8, "sceMpegRingbufferConstruct", h_MpegRingbufferConstruct);
    sr_hle_register(0x13407f13, "sceMpegRingbufferDestruct", h_ok);
    sr_hle_register(0xb240a59e, "sceMpegRingbufferPut", h_MpegRingbufferPut);
    sr_hle_register(0xb5f6dc87, "sceMpegRingbufferAvailableSize", h_MpegRingbufferAvailable);
    sr_hle_register(0xfe246728, "sceMpegGetAvcAu", h_MpegGetAvcAu);
    sr_hle_register(0xe1ce83a7, "sceMpegGetAtracAu", h_MpegGetAtracAu);
    sr_hle_register(0x0e3c2e9d, "sceMpegAvcDecode", h_MpegAvcDecode);
    sr_hle_register(0x800c44df, "sceMpegAtracDecode", h_MpegAtracDecode);
    sr_hle_register(0x740fccd1, "sceMpegAvcDecodeStop", h_MpegAvcDecodeStop);
    sr_hle_register(0x4571cc64, "sceMpegAvcDecodeFlush", h_ok);
    sr_hle_register(0xa780cf7e, "sceMpegMallocAvcEsBuf", h_MpegMallocAvcEsBuf);
    sr_hle_register(0xceb870b1, "sceMpegFreeAvcEsBuf", h_MpegFreeAvcEsBuf);
    sr_hle_register(0x167afd9e, "sceMpegInitAu", h_MpegInitAu);
    sr_hle_register(0xf8dcb679, "sceMpegQueryAtracEsSize", h_MpegQueryAtracEsSize);
    /* scePsmfPlayer: lifecycle and validation are implemented here; demux producers can later
     * advance the queue matrix without changing the ABI or the NID registration surface. */
    sr_hle_register(0x1078c008, "scePsmfPlayerStop", h_PsmfStop);
    sr_hle_register(0x1e57a8e7, "scePsmfPlayerConfigPlayer", h_PsmfConfig);
    sr_hle_register(0x235d8787, "scePsmfPlayerCreate", h_PsmfCreate);
    sr_hle_register(0x2beb1569, "scePsmfPlayerBreak", h_PsmfBreak);
    sr_hle_register(0x2d0e4e0a, "scePsmfPlayerSetTempBuf", h_PsmfSetTempBuf);
    sr_hle_register(0x3ea82a4b, "scePsmfPlayerGetAudioOutSize", h_PsmfAudioOutSize);
    sr_hle_register(0x46f61f8b, "scePsmfPlayerGetVideoData", h_PsmfGetVideo);
    sr_hle_register(0x58b83577, "scePsmfPlayerSetPsmfCB", h_PsmfSetPsmfCB);
    sr_hle_register(0x95a84ee5, "scePsmfPlayerStart", h_PsmfStart);
    sr_hle_register(0x9b71a274, "scePsmfPlayerDelete", h_PsmfDelete);
    sr_hle_register(0xa0b8ca55, "scePsmfPlayerUpdate", h_PsmfUpdate);
    sr_hle_register(0xb9848a74, "scePsmfPlayerGetAudioData", h_PsmfGetAudio);
    sr_hle_register(0xe792cd94, "scePsmfPlayerReleasePsmf", h_PsmfRelease);
    sr_hle_register(0xf8ef08a6, "scePsmfPlayerGetCurrentStatus", h_PsmfStatus);
    /* sceLibFont: synchronized handles backed by parsed firmware/user PGFs. */
    sr_hle_register(0x67f17ed7, "sceFontNewLib", h_FontNewLib);
    sr_hle_register(0xa834319d, "sceFontOpen", h_FontOpen);
    sr_hle_register(0x574b6fbc, "sceFontDoneLib", h_FontDoneLib);
    sr_hle_register(0x0da7535e, "sceFontGetFontInfo", h_FontGetFontInfo);
    sr_hle_register(0xdcc80c2f, "sceFontGetCharInfo", h_FontGetCharInfo);
    sr_hle_register(0x980f4895, "sceFontGetCharGlyphImage", h_FontGetCharGlyphImage);
    sr_hle_register(0x099ef33c, "sceFontFindOptimumFont", h_FontFindOptimumFont);
    sr_hle_register(0x3aea8cb6, "sceFontClose", h_FontClose);
    sr_hle_register(0xbb8e7fe6, "sceFontOpenUserMemory", h_FontOpenUserMemory);
    sr_hle_register(0x6af9b50a, "sceUmdCancelWaitDriveStat", h_ok);
    sr_hle_register(0x6b4a146c, "sceUmdGetDriveStat", h_UmdDriveStat);
    sr_hle_register(0x20628e6f, "sceUmdGetErrorStat", h_ok);
    /* Callback-aware UMD wait consumes callbacks while preserving the drive wait. */
    sr_hle_register(0x977de386, "sceKernelLoadModule", h_LoadModule);
    sr_hle_register(0xb7f46618, "sceKernelLoadModuleByID", h_LoadModuleByID);
    sr_hle_register(0x50f0c1ec, "sceKernelStartModule", h_StartModule);
    sr_hle_register(0xf0a26395, "sceKernelGetModuleId", h_GetModuleId);
    sr_hle_register(0xd1ff982a, "sceKernelStopModule", h_StopModule_Trace);
    sr_hle_register(0x2e0911aa, "sceKernelUnloadModule", h_UnloadModule_Trace);
    sr_hle_register(0x8f2df740, "sceKernelStopUnloadSelfModuleWithStatus", h_StopUnloadSelfModuleWithStatus);
    /* IoFileMgrForUser: file IO from the ISO. */
    sr_hle_register(0x109f50bc, "sceIoOpen", h_IoOpen);
    sr_hle_register(0x779103a0, "sceIoRename", h_ok);
    sr_hle_register(0x68963324, "sceIoLseek32", h_IoLseek32);
    sr_hle_register(0x27eb27b8, "sceIoLseek", h_IoLseek);
    sr_hle_register(0x63632449, "sceIoIoctl", h_IoIoctl);
    sr_hle_register(0x810c4bc3, "sceIoClose", h_IoClose);
    sr_hle_register(0xace946e8, "sceIoGetstat", h_IoGetstat);
    sr_hle_register(0xb29ddf9c, "sceIoDopen", h_IoDopen);
    sr_hle_register(0xe3eb004c, "sceIoDread", h_IoDread);
    sr_hle_register(0xeb092469, "sceIoDclose", h_IoDclose);
    sr_hle_register(0x89aa9906, "sceIoOpenAsync", h_IoOpenAsync);
    sr_hle_register(0xa0b5a7c2, "sceIoReadAsync", h_IoReadAsync);
    sr_hle_register(0x71b19e77, "sceIoLseekAsync", h_IoLseekAsync);
    sr_hle_register(0x3251ea56, "sceIoPollAsync", h_IoWaitAsync);
    sr_hle_register(0xff5940b6, "sceIoCloseAsync", h_IoCloseAsync);
    sr_hle_register(0x54f5fb11, "sceIoDevctl", h_IoDevctl);
    /* sceAudio regular channels share one production mapping with the executable
     * contract harness. Output2 remains production-only and otherwise unchanged. */
    hle_register_regular_audio_handlers();
    sr_hle_register(0x01562ba3, "sceAudioOutput2Reserve", h_AudioOutput2Reserve);
    sr_hle_register(0x2d53f36e, "sceAudioOutput2OutputBlocking", h_AudioOutput2Blocking);
    sr_hle_register(0x43196845, "sceAudioOutput2Release", h_AudioOutput2Release);
    sr_hle_register(0x63f2889c, "sceAudioOutput2ChangeLength", h_AudioOutput2ChangeLength);
    sr_hle_register(0x647cef33, "sceAudioOutput2GetRestSample", h_AudioOutput2Rest);
    /* sceCtrl */
    sr_hle_register(0x1f4011e6, "sceCtrlSetSamplingMode", h_ok);
    sr_hle_register(0x6a2774f3, "sceCtrlSetSamplingCycle", h_ok);
    sr_hle_register(0xa7144800, "sceCtrlSetIdleCancelThreshold", h_ok);
    /* 0x687660fa is GetIdleCancelThreshold(int*,int*), NOT ReadBufferNegative -- the pad
     * handler used pointer a1 as a buffer count and wrote up to a ring of SceCtrlData
     * through a1's 4-byte int (and could block the caller on the input ring). */
    sr_hle_register(0x687660fa, "sceCtrlGetIdleCancelThreshold", h_CtrlGetIdleCancelThreshold);
    hle_register_ge_handlers();
    /* Issue #86: the previous numeric NIDs for the four getters below were bogus (absent from
     * the PPSSPP-derived nid_names.h); they are replaced with the canonical NIDs so real title
     * imports route here instead of being unregistered, and 0x478fe6f5 is renamed to its
     * canonical non-Int label (the Int alias is 0xbd681969 and stays unregistered). The float
     * return shaping for the non-Int getters remains tracked by #86. */
    sr_hle_register(0x2085d15d, "scePowerGetBatteryLifePercent", h_PowerGetBatteryLifePercent);
    sr_hle_register(0x1e490401, "scePowerIsBatteryCharging", h_PowerIsBatteryCharging);
    sr_hle_register(0x0afd0d8b, "scePowerIsBatteryExist", h_PowerIsBatteryExist);
    sr_hle_register(0x87440f5e, "scePowerIsPowerOnline", h_PowerIsPowerOnline);
    sr_hle_register(0xfdb5bfe9, "scePowerGetCpuClockFrequencyInt", h_PowerGetCpuClockFrequencyInt);
    sr_hle_register(0x478fe6f5, "scePowerGetBusClockFrequency", h_PowerGetBusClockFrequencyInt);
    sr_hle_register(0x737486f2, "scePowerSetClockFrequency", h_ok);
    sr_hle_register(0xebd177d6, "scePowerSetClockFrequency350", h_ok);
    sr_hle_register(0x730ed8bc, "sceKernelReferCallbackStatus", h_ReferCallbackStatus);
    hle_register_utility_module_handlers();
    sr_hle_register(0x1b4217bc, "sceKernelSetCompiledSdkVersion603_605", h_SetCompiledSdkVersion);

    /* cache / misc UtilsForUser: no-ops are fine without a real cache. */
    sr_hle_register(0x79d1c3fa, "sceKernelDcacheWritebackAll", h_ok);
    /* Guest and host share one coherent byte array; cache maintenance has no
     * additional host-side work, but the syscall and success result are real. */
    sr_hle_register(0xb435dec5, "sceKernelDcacheWritebackInvalidateAll", h_ok);
    sr_hle_register(0x3ee30821, "sceKernelDcacheWritebackRange", h_ok);
    sr_hle_register(0x6ad345d7, "sceKernelSetGPO", h_ok);
    /* GPI reads hardware general-purpose input pins; always 0 on retail PSP. */
    sr_hle_register(0x37fb5c42, "sceKernelGetGPI", h_ok);
    /* StdioForKernel std handles and kernel printf NIDs (0xcab439df is the
     * StdioForKernel "printf" export; 0x13a5abef is SysMemUserForUser's
     * "sceKernelPrintf"). */
    sr_hle_register(0x13a5abef, "sceKernelPrintf", h_KernelPrintf);
    sr_hle_register(0xcab439df, "printf", h_KernelPrintf);
    sr_hle_register(0x172d316e, "sceKernelStdin", h_StdFd);
    sr_hle_register(0xa6bab2e9, "sceKernelStdout", h_StdFd);
    sr_hle_register(0xf78ba90a, "sceKernelStderr", h_StdFd);
    /* scePower / sceSuspendForUser / LoadExecForUser: locks and registrations succeed. */
    sr_hle_register(0x3aee7261, "sceKernelPowerUnlock", h_ok);
    sr_hle_register(0xeadb1bd7, "sceKernelPowerLock", h_ok);
    sr_hle_register(0x090ccb3f, "sceKernelPowerTick", h_ok);
    sr_hle_register(0xa14f40b2, "sceKernelVolatileMemTryLock", h_VolatileMemLock);
    /* Unlock takes only the type arg -- it must NOT run the Lock handler: writing the
     * out-params through leftover a1/a2 register garbage sprayed two wild 4-byte writes
     * per asset-load unlock (this zeroed the resource registry's model-slot counter,
     * which silently killed every .PMD model lookup, e.g. the hangar aircraft). */
    sr_hle_register(0xa569e425, "sceKernelVolatileMemUnlock", h_ok);
    hle_register_exit_game_handler();
    /* InterruptManager: record the VBLANK handler; the scheduler delivers it per frame. */
    sr_hle_register(0xd61e6961, "sceKernelReleaseSubIntrHandler", h_ok);
    sr_hle_register(0x8a389411, "sceKernelDisableSubIntr", h_ok);
    hle_register_msgpipe_handlers();
    /* semaphores */
    /* event flags */
    sr_hle_register(0xef9e4c70, "sceKernelDeleteEventFlag", h_DeleteEventFlag);
    sr_hle_register(0x812346e4, "sceKernelClearEventFlag", h_ClearEventFlag);
    sr_hle_register(0x30fd48f0, "sceKernelPollEventFlag", h_PollEventFlag);
    /* Lightweight mutexes. See the h_CreateLwMutex block above for why these
     * are no longer no-ops and which entries remain deliberately unimplemented.
     * The CB variants share the plain handler: a blocking lock here parks on
     * sched_block_on, which is already a callback-safe yield point. */
    sr_hle_register(0x60107536, "sceKernelDeleteLwMutex", h_DeleteLwMutex);
    sr_hle_register(0x7cff8cf3, "_sceKernelLockLwMutex", h_LockLwMutex);
    sr_hle_register(0x31327f19, "_sceKernelLockLwMutexCB", h_LockLwMutex);
    sr_hle_register(0xdc692ee3, "sceKernelTryLockLwMutex", h_TryLockLwMutex);
    sr_hle_register(0x37431849, "sceKernelTryLockLwMutex_600", h_TryLockLwMutex);
    sr_hle_register(0x71040d5c, "_sceKernelTryLockLwMutex", h_TryLockLwMutex);
    sr_hle_register(0x15b6446b, "sceKernelUnlockLwMutex", h_UnlockLwMutex);
    sr_hle_register(0xbeed3a47, "_sceKernelUnlockLwMutex", h_UnlockLwMutex);
    /* Status layout unmeasured -- see the note above h_CreateEventFlag. */
    sr_hle_register(0xc1734599, "sceKernelReferLwMutexStatus", h_ok);
    sr_hle_register(0x4c145944, "sceKernelReferLwMutexStatusByID", h_ok);
    /* regular mutexes (also no-ops) */
    sr_hle_register(0xf8170fbe, "sceKernelDeleteMutex", h_ok);
    sr_hle_register(0x0ddcd2c9, "sceKernelTryLockMutex", h_ok);
    sr_hle_register(0x6b30100f, "sceKernelUnlockMutex", h_ok);
    sr_hle_register(0x87d9223c, "sceKernelCancelMutex", h_ok);
    sr_hle_register(0xa9c2cb9a, "sceKernelReferMutexStatus", h_ok);

    /* Registry utility (sceReg) stubs */
    /* Registry utility (sceReg) stubs -- issue #78: all six NIDs were registered under the
     * wrong canonical names, corrupting import-coverage reports. The labels below match
     * src/rt/nid_names.h; the real sceRegExit NID (0x9b25edf1) and the read/write registry
     * model remain unregistered until the minimal registry implementation lands (#78). */
    sr_hle_register(0x0cae832b, "sceRegCloseCategory", h_ok);
    sr_hle_register(0x1d8a762e, "sceRegOpenCategory", h_ok);
    sr_hle_register(0x28a8e98a, "sceRegGetKeyValue", h_ok);
    sr_hle_register(0x92e41280, "sceRegOpenRegistry", h_ok);
    sr_hle_register(0xd4475aa8, "sceRegGetKeyInfo", h_ok);
    sr_hle_register(0xfa8a5739, "sceRegCloseRegistry", h_ok);
    /* sceOpenPSID: returns 16-byte console unique ID; zero-fill is fine for boot. */
    sr_hle_register(0xc69bebce, "sceOpenPSIDGetOpenPSID", h_OpenPSIDGetOpenPSID);
#endif

    hle_register_time_handlers();
    hle_register_display_handlers();

    hle_register_atrac_handlers();
    hle_register_sas_handlers();
    atomic_store_explicit(&s_hle_init_state, 2, memory_order_release);
}

/* ---- dispatch ---- */

uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    sr_hle_init();
    sr_last_nid = nid;
    if (getenv("SR_NIDLOG")) {
        static FILE *nf = NULL; static unsigned long nc = 0;
        if (!nf) nf = fopen("nidseq_mine.txt", "w");
        if (nf) { fprintf(nf, "0x%08x 0x%x %u\n", nid, sched_current_uid(), s_vcount);
                  if ((++nc & 0x3f) == 0) fflush(nf); }
    }
    HleEntry *e = hle_find(nid);
    if (hle_log_on()) {
        /* Deduplicate: only log each (thread, nid) pair once to avoid drowning
         * in thousands of identical SuspendIntr/ResumeIntr lines. */
        static struct { uint32_t uid, nid; } seen[2048]; static int nseen = 0;
        uint32_t cur = sched_current_uid(); int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i].uid == cur && seen[i].nid == nid) { known = 1; break; }
        if (!known) {
            if (nseen < 2048) { seen[nseen].uid = cur; seen[nseen].nid = nid; nseen++; }
            const char *nm = e ? e->name : sr_nid_name(nid);
            fprintf(stderr, "HLE: calling %s (0x%08x) from 0x%x\n", nm ? nm : "unknown", nid, cur);
        }
    }
    if (!e) {
        {
            const char *nm = sr_nid_name(nid);
            fprintf(stderr, "HLE: unimplemented nid 0x%08x (%s) (thread uid 0x%x)\n"
                            "     -> add a handler in src/rt/hle.c: sr_hle_register(0x%08xu, \"%s\", h_...);\n",
                    nid, nm ? nm : "unknown", sched_current_uid(), nid, nm ? nm : "sceUnknown");
        }
        sr_hit_hle = 1;
        /* Under the fiber scheduler, longjmp across fibers is invalid; stop the process cleanly
         * after flushing the trace. The plain driver (no scheduler) keeps the longjmp boundary. */
        if (sr_sched_on) { sr_trace_close(); fflush(stderr); _Exit(7); }
        longjmp(g_hle_jmp, 1);
    }
    if (getenv("SR_SYSLOG")) {
        /* Log the first time each (thread, nid) pair occurs, to see what each thread does. */
        static struct { uint32_t uid, nid; } seen[4096]; static int nseen = 0;
        uint32_t cur = sched_current_uid(); int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i].uid == cur && seen[i].nid == nid) { known = 1; break; }
        if (!known && nseen < 4096) { seen[nseen].uid = cur; seen[nseen].nid = nid; nseen++;
            fprintf(stderr, "thr 0x%x : %s (0x%08x)\n", cur, e->name, nid); }
    }
    if (g_callcount) {
        int j = 0; for (; j < g_ncc; j++) if (g_cc[j].nid == nid) break;
        if (j == g_ncc && g_ncc < 512) { g_cc[j].nid = nid; g_cc[j].nm = e->name; g_cc[j].n = 0; g_ncc++; }
        if (j < 512) g_cc[j].n++;
    }
    uint32_t ret = e->fn(s);
    ge_enqueue_trace_note_hle(s, nid, e->name);
    /* Poison caller-saved temps exactly like PPSSPP SetDeadbeefRegs: r1, r4-r15, r24, r25,
     * hi, lo. The return value in v0 (and v1) is written afterward and survives. */
    s->r[1] = 0xDEADBEEFu;
    for (int i = 4; i <= 15; i++) s->r[i] = 0xDEADBEEFu;
    s->r[24] = 0xDEADBEEFu; s->r[25] = 0xDEADBEEFu;
    s->hi = 0xDEADBEEFu; s->lo = 0xDEADBEEFu;
    s->r[2] = ret;
    return ret;
}
