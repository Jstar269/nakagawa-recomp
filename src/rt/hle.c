// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors


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
 *
 * Layout (#307): this file keeps the handler table and registration API, the kernel object
 * UIDs and the user-memory allocator, the code two or more subsystems share, sr_hle_init()
 * and sr_syscall(). Each subsystem in tools/hle_registry_meta.py has its own file,
 * src/rt/hle_<subsystem>.c, with its handlers, their state and its registrations; hle_host.c,
 * hle_data.c and hle_route.c hold the host primitives, the host data lookup, and the
 * acceptance routes and frame capture. hle_internal.h is their private header, and
 * `python tools/hle_sources.py --defining-file NAME` names the file that defines NAME.
 */

#include "hle_internal.h"
#include "nid_names.h"   /* sr_nid_name(): names unknown NIDs in the trap below */
#include "hle_evidence.h" /* generated: NIDs with no recorded evidence (docs/HLE_COVERAGE.md) */
int sr_thread_has_pending_callbacks(uint32_t thread_uid);
int sr_thread_dispatch_callbacks(void);    /* internal pump count; public CheckCallback is Boolean */
void sr_callback_unregister_owner(uint32_t thread_uid);

/* Issue #143 diagnostic hooks.  Definitions live beside the display vblank
 * counter because the trace is deliberately bounded by guest-frame windows. */
static void ge_enqueue_trace_note_hle(CpuState *s, uint32_t nid, const char *name);
static void ge_enqueue_trace_note_callback(CpuState *s, uint32_t uid, uint32_t entry);
static uint32_t h_ControlledUnsupported(CpuState *s);
/* Keep the registry comfortably above the current export set.  A full table
 * silently drops the late generic memcpy/memset handlers, which then makes a
 * valid NID look unimplemented even though its production function exists. */
#define HLE_CAP 1024
static HleEntry s_hle[HLE_CAP];
static int s_hle_n = 0;
static atomic_uint s_hle_unsupported_summary_registered;

static void hle_unsupported_summary(void);

void hle_register_unsupported_summary(void) {
    if (!atomic_exchange_explicit(&s_hle_unsupported_summary_registered, 1u,
                                  memory_order_acq_rel))
        (void)atexit(hle_unsupported_summary);
}

static void hle_unsupported_summary(void) {
    for (int i = 0; i < s_hle_n; i++) {
        HleEntry *e = &s_hle[i];
        if (!(atomic_load_explicit(&e->unsupported_state, memory_order_acquire) & 1u))
            continue;
        uint32_t unsupported_error = atomic_load_explicit(&e->unimplemented_error,
                                                          memory_order_acquire);
        if (unsupported_error) {
            fprintf(stderr, "HLE unimplemented summary: %s (NID 0x%08x) -> 0x%08x; in the works (#341)\n",
                    e->name, e->nid, unsupported_error);
        } else {
            const char *issue = hle_success_exception_issue(e->nid);
            if (issue)
                fprintf(stderr, "HLE unimplemented summary: %s (NID 0x%08x) kept compatibility result 0; in the works (%s)\n",
                        e->name, e->nid, issue);
        }
    }
}

static void hle_note_unsupported(HleEntry *e) {
    if (atomic_exchange_explicit(&e->unsupported_state, 1u, memory_order_acq_rel))
        return;
    hle_register_unsupported_summary();
    fprintf(stderr, "HLE: controlled refusal: %s (NID 0x%08x) returned 0x%08x; in the works (#341)\n",
            e->name, e->nid, e->unsupported_error);
}

/* One stderr line the first time a NID with no recorded evidence runs its handler. Diagnostic
 * only: it changes no result or state. The class comes from the generated hle_evidence.h. */
static int hle_nid_unverified(uint32_t nid) {
    size_t lo = 0u, hi = SR_HLE_UNVERIFIED_NID_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (sr_hle_unverified_nids[mid] < nid) lo = mid + 1u;
        else hi = mid;
    }
    return lo < SR_HLE_UNVERIFIED_NID_COUNT && sr_hle_unverified_nids[lo] == nid;
}

static void hle_note_unverified(HleEntry *e) {
    if (!e->unverified || atomic_exchange_explicit(&e->unverified_noted, 1u, memory_order_acq_rel))
        return;
    fprintf(stderr, "HLE: unverified: %s (NID 0x%08x) has no recorded evidence (docs/HLE_COVERAGE.md)\n",
            e->name, e->nid);
}

static void hle_note_success_exception(HleEntry *e) {
    const char *issue = hle_success_exception_issue(e->nid);
    if (!issue || atomic_exchange_explicit(&e->unsupported_state, 1u, memory_order_acq_rel))
        return;
    hle_register_unsupported_summary();
    fprintf(stderr, "HLE: compatibility exception: %s (NID 0x%08x) keeps result 0; semantics in the works (%s)\n",
            e->name, e->nid, issue);
}
LateImportEntry *s_late_imports;
unsigned s_late_import_n, s_late_import_cap;
static atomic_flag s_hle_lock = ATOMIC_FLAG_INIT;
static atomic_int s_hle_init_state;

/* HLE calls are among the hottest runtime paths.  Environment variables do
 * not change after process launch, so cache the logging switch instead of
 * searching the environment on every guest syscall. */
int hle_log_on(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("SR_HLELOG") != NULL;
    return enabled;
}

static int hle_nidlog_on(void) {
    static int enabled = -1;
    return hle_env_switch(&enabled, "SR_NIDLOG");
}

int hle_syslog_on(void) {
    static int enabled = -1;
    return hle_env_switch(&enabled, "SR_SYSLOG");
}

/* Thread sleep, wakeup and run-status calls and every UID allocation report under
 * SR_WAKELOG; a guest can make those calls every frame. */
int hle_wakelog_on(void) {
    static int enabled = -1;
    return hle_env_switch(&enabled, "SR_WAKELOG");
}

/* Display-list tracing is extremely verbose (several writes per presented
 * frame).  Keep Standard mode quiet; Diagnostics enables SR_GEDUMP, and the
 * public SR_DEBUG GE bit (0x08) remains an equivalent opt-in. */
int ge_log_on(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *debug = getenv("SR_DEBUG");
        enabled = getenv("SR_GEDUMP") != NULL || getenv("SR_GELOG") != NULL ||
                  (debug != NULL && (strtoul(debug, NULL, 0) & 0x08u) != 0u);
    }
    return enabled;
}

void hle_lock(void) {
    while (atomic_flag_test_and_set_explicit(&s_hle_lock, memory_order_acquire)) { }
}
void hle_unlock(void) { atomic_flag_clear_explicit(&s_hle_lock, memory_order_release); }

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

/* Unlink every registered export whose target lies in [lo, hi). This is the export
 * authority a loaded image owns, so unloading that image retires exactly its own
 * exports: an importer can no longer resolve a NID to an address in a range the
 * kernel has just released. Entries outside the range belong to other resident
 * images and are left untouched. Sorted order by NID is preserved, so the lookup
 * above keeps its binary search. */
static void sr_hle_unregister_late_imports_in_range(uint32_t lo, uint32_t hi) {
    if (hi <= lo) return;
    hle_lock();
    unsigned kept = 0;
    for (unsigned i = 0; i < s_late_import_n; i++) {
        uint32_t t = s_late_imports[i].target;
        if (t >= lo && t < hi) continue;
        s_late_imports[kept++] = s_late_imports[i];
    }
    s_late_import_n = kept;
    hle_unlock();
}

static void sr_hle_register_entry(uint32_t nid, const char *name, HleFn fn,
                                  uint32_t unsupported_error) {
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
    if (s_hle_n < HLE_CAP) {
        HleEntry *e = &s_hle[s_hle_n++];
        e->nid = nid;
        e->name = name;
        e->fn = fn;
        e->unsupported_error = unsupported_error;
        atomic_store_explicit(&e->unimplemented_error, unsupported_error,
                              memory_order_release);
        atomic_store_explicit(&e->unsupported_state, 0u, memory_order_release);
        e->unverified = (unsigned char)hle_nid_unverified(nid);
        atomic_store_explicit(&e->unverified_noted, 0u, memory_order_release);
    }
    hle_unlock();
}

void sr_hle_register(uint32_t nid, const char *name, HleFn fn) {
    sr_hle_register_entry(nid, name, fn, 0u);
}

void sr_hle_register_unsupported(uint32_t nid, const char *name, uint32_t error) {
    if (!(error & 0x80000000u)) {
        fprintf(stderr, "HLE: invalid unsupported result 0x%08x for %s (NID 0x%08x)\n",
                error, name, nid);
        return;
    }
    sr_hle_register_entry(nid, name, h_ControlledUnsupported, error);
}

HleEntry *hle_find(uint32_t nid) {
    HleEntry *result = NULL;
    hle_lock();
    for (int i = 0; i < s_hle_n; i++) if (s_hle[i].nid == nid) { result = &s_hle[i]; break; }
    hle_unlock();
    return result;
}

/* ---- kernel object UID + user-memory bump allocator ---- */

/* Single shared UID pool matching PPSSPP: uid = 0x110, 0x111, ... incrementing by 1.
 * Two values are skipped so they can mean something else unambiguously: 0 is PSP's
 * "current thread" argument value, and SR_ROLE_UID_NONE is the scheduler's "this role
 * has not been captured" marker (recomp.h). Neither is reachable from 0x110 in any real
 * session, but the pool wraps in principle and a role marker that could also be a live
 * thread identity is precisely the defect this skip exists to make impossible. */
uint32_t s_uid = 0x110;
uint32_t sr_alloc_uid(void) {
    while (s_uid == 0u || s_uid == SR_ROLE_UID_NONE) s_uid++;
    uint32_t uid = s_uid++;
    if (hle_wakelog_on()) fprintf(stderr, "ALLOC_UID: 0x%x\n", uid);
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
uint32_t s_heap = 0;              /* bump pointer in user RAM; 0 = not yet initialised */
static uint32_t s_part_top = 0;
static uint32_t s_heap_base = 0;          /* partition floor: heap value at init, always >= loaded image end */
static uint32_t s_heap_last_bump = 0;     /* mirror of last s_heap value, for the main-thread diagnostic */
uint32_t user_partition_last_heap(void) { return s_heap_last_bump ? s_heap_last_bump : s_heap; }

uint32_t sr_last_alloc_addr = 0;     /* last heap allocation rounded address */
/* Start the user partition after the complete flat image, including zero-filled BSS.
 * tools/prxload.py preserves the original ~PSP header's segment-memory sizes when a
 * stripped ELF has lost them. An explicit override may move the base higher, but may
 * never overlap the loaded module. */
static int sr_user_partition_overlaps(uint32_t base, uint32_t end);
void user_partition_init(void) {
    if (s_heap) return;
    const char *eb = getenv("SR_HEAP_BASE");
    const char *et = getenv("SR_PARTITION_TOP");
    uint32_t loaded_end = sr_loaded_end();
    uint32_t minimum_base = (loaded_end + 0xFFFu) & ~0xFFFu;
    uint32_t base = eb ? (uint32_t)strtoul(eb, NULL, 16) : minimum_base;
    if (loaded_end == 0u || minimum_base < loaded_end) {
        fprintf(stderr, "user_partition_init: invalid loaded image end 0x%08x\n", loaded_end);
        sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, 0u, loaded_end, 0u);
        abort();
    }
    if (base < minimum_base) {
        fprintf(stderr,
                "user_partition_init: SR_HEAP_BASE 0x%08x overlaps loaded image/BSS "
                "ending at 0x%08x (minimum 0x%08x)\n",
                base, loaded_end, minimum_base);
        sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, 0u, loaded_end, (uint32_t)base);
        abort();
    }
    s_heap = base;
    s_heap_base = base;
    sched_set_stack_range_refusal(sr_user_partition_overlaps);
    s_part_top = et ? (uint32_t)strtoul(et, NULL, 16) : 0x0A000000u;
    if (s_part_top <= s_heap) {
        fprintf(stderr,
                "user_partition_init: partition top 0x%08x is not above heap base 0x%08x\n",
                s_part_top, s_heap);
        sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, 0u, (uint32_t)s_part_top, (uint32_t)s_heap);
        abort();
    }
    fprintf(stderr,
            "user_partition_init: loaded_end=0x%08x heap_base=0x%08x top=0x%08x\n",
            loaded_end, s_heap, s_part_top);
    s_heap_last_bump = s_heap;
}
Block s_blocks[HLE_MAX_PARTITION_BLOCKS];

/* Guest thread stacks are carved by the scheduler from a descending arena that ends at
 * 0x0a000000, the top of the PSP user-RAM span this partition also covers (the runtime's
 * VBLANK stack and nested frames live in kernel RAM). On a PSP, thread stacks come out of the
 * same user partition, so memory a game is told is free never contains a live stack.
 * Here the contiguous free space above the bump pointer therefore ends at the first live
 * stack above the partition base (and never above the arena ceiling). Before this, a
 * game that took the reported
 * free memory (21 MB and more) received a block over its own main-thread stack, zeroed
 * it, and returned through a clobbered $ra (AOT_RETURN_TARGET_MISMATCH). */
#define USER_PARTITION_RESERVED_FLOOR 0x0a000000u   /* sched.c SR_STACK_ARENA_CEIL */
extern uint32_t sched_guest_stack_floor_above(uint32_t addr);
static uint32_t user_partition_ceiling(void) {
    return s_part_top < USER_PARTITION_RESERVED_FLOOR ? s_part_top : USER_PARTITION_RESERVED_FLOOR;
}
/* The runtime's VBLANK stack and nested host->guest frames occupy the top of kernel RAM,
 * [0x082f0000, SR_NESTED_FRAME_END). A partition based below them (an image linked at a
 * low address) ends there, so no allocation is ever placed over a runtime stack. */
#define USER_PARTITION_RUNTIME_STACKS_BASE 0x082f0000u   /* sched.c SR_VBLANK_STACK_BASE */
static uint32_t partition_high_floor_above(uint32_t addr);
uint32_t user_partition_top(void) {
    uint32_t ceiling = user_partition_ceiling();
    if (s_heap_base < SR_NESTED_FRAME_END && ceiling > USER_PARTITION_RUNTIME_STACKS_BASE)
        ceiling = USER_PARTITION_RUNTIME_STACKS_BASE;
    uint32_t floor = sched_guest_stack_floor_above(s_heap_base);
    uint32_t high = partition_high_floor_above(s_heap_base);
    if (high < floor) floor = high;
    return floor < ceiling ? floor : ceiling;
}

int s_nblocks = 0;

/* 1 when [base, end) touches the bump region the partition has handed out (below the
 * current bump pointer, its free-list holes included). The scheduler refuses a new stack
 * there. Fixed-address reservations (sr_alloc_block_at: a title profile's module bases,
 * PSP_SMEM_Addr requests) keep their previous bounds and are not checked against stacks:
 * a reviewed profile's late PRX base can share the stack arena's address range on this
 * runtime. 0 before the partition exists. */
static int sr_user_partition_overlaps(uint32_t base, uint32_t end) {
    if (!s_heap || end <= base) return 0;
    return base < s_heap && end > s_heap_base;
}
FreeBlock s_free_blocks[HLE_MAX_FREE_BLOCKS];
int s_nfree_blocks = 0;

static void free_list_insert_sorted(uint32_t addr, uint32_t size) {
    /* Insert [addr, addr+size) into s_free_blocks sorted by addr and coalesce
     * with touching neighbours. No bump-pointer rollback: the caller owns the
     * heap invariant (partition_free_block checks the top span itself, and the
     * fixed-address path manages s_heap explicitly). */
    int idx = 0;
    while (idx < s_nfree_blocks && s_free_blocks[idx].addr < addr) {
        idx++;
    }

    int merge_left = (idx > 0 && s_free_blocks[idx - 1].addr + s_free_blocks[idx - 1].size == addr);
    int merge_right = (idx < s_nfree_blocks && addr + size == s_free_blocks[idx].addr);

    if (merge_left && merge_right) {
        s_free_blocks[idx - 1].size += size + s_free_blocks[idx].size;
        for (int j = idx; j < s_nfree_blocks - 1; j++) {
            s_free_blocks[j] = s_free_blocks[j + 1];
        }
        s_nfree_blocks--;
    } else if (merge_left) {
        s_free_blocks[idx - 1].size += size;
    } else if (merge_right) {
        s_free_blocks[idx].addr = addr;
        s_free_blocks[idx].size += size;
    } else {
        if (s_nfree_blocks < HLE_MAX_FREE_BLOCKS) {
            for (int j = s_nfree_blocks; j > idx; j--) {
                s_free_blocks[j] = s_free_blocks[j - 1];
            }
            s_free_blocks[idx].addr = addr;
            s_free_blocks[idx].size = size;
            s_nfree_blocks++;
        } else {
            fprintf(stderr, "partition_free_block: free block table exhausted\n");
        }
    }
}

static void partition_free_block(uint32_t addr, uint32_t size) {
    if (size == 0) return;

    /* Check if this freed block reaches or exceeds the bump pointer s_heap. */
    if (addr + size >= s_heap) {
        /* Check if it coalesces with the last free block in the free list. */
        if (s_nfree_blocks > 0 &&
            s_free_blocks[s_nfree_blocks - 1].addr + s_free_blocks[s_nfree_blocks - 1].size == addr) {
            s_heap = s_free_blocks[s_nfree_blocks - 1].addr;
            s_nfree_blocks--;
        } else {
            s_heap = addr;
        }
        s_heap_last_bump = s_heap;
        return;
    }

    /* Interior block: insert into s_free_blocks sorted by addr and coalesce. */
    free_list_insert_sorted(addr, size);

    /* If the highest free block now reaches s_heap, roll s_heap back. */
    if (s_nfree_blocks > 0 &&
        s_free_blocks[s_nfree_blocks - 1].addr + s_free_blocks[s_nfree_blocks - 1].size >= s_heap) {
        s_heap = s_free_blocks[s_nfree_blocks - 1].addr;
        s_heap_last_bump = s_heap;
        s_nfree_blocks--;
    }
}

uint32_t alloc_block(uint32_t size) {
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

    if (size == 0 || size > max_req) {
        if (guard) {
            fprintf(stderr, "ALLOC_BLOCK_REJECT: size=%u (0x%x) heap=0x%08x top=0x%08x max=0x%08x\n",
                    size, size, s_heap, user_partition_top(), max_req);
            fflush(stderr);
        }
        return 0xFFFFFFFFu;
    }

    uint32_t needed_slot = (size + 0xFFu) & ~0xFFu;
    if (needed_slot == 0) needed_slot = 0x100u;

    uint32_t addr = 0;
    uint32_t slot_size = 0;
    int found_fb = -1;

    /* First-fit search across sorted free list (PSP_SMEM_Low policy). */
    for (int i = 0; i < s_nfree_blocks; i++) {
        if (s_free_blocks[i].size >= needed_slot) {
            found_fb = i;
            break;
        }
    }

    if (found_fb >= 0) {
        addr = s_free_blocks[found_fb].addr;
        slot_size = needed_slot;
        if (s_free_blocks[found_fb].size == needed_slot) {
            for (int j = found_fb; j < s_nfree_blocks - 1; j++) {
                s_free_blocks[j] = s_free_blocks[j + 1];
            }
            s_nfree_blocks--;
        } else {
            s_free_blocks[found_fb].addr += needed_slot;
            s_free_blocks[found_fb].size -= needed_slot;
        }
    } else {
        /* Bump allocation from s_heap. */
        uint32_t aligned = (s_heap + 0xFFu) & ~0xFFu;
        uint32_t top = user_partition_top();
        int overflow = (aligned + size < aligned) || (aligned + size > top);
        if (overflow && guard) {
            fprintf(stderr, "ALLOC_BLOCK_REJECT: size=%u (0x%x) heap=0x%08x top=0x%08x max=0x%08x\n",
                    size, size, s_heap, user_partition_top(), max_req);
            fflush(stderr);
            return 0xFFFFFFFFu;  /* PSP alloc-failure sentinel */
        }
        addr = aligned;   /* 256-byte align */
        slot_size = needed_slot;
        s_heap = addr + size;
        s_heap_last_bump = s_heap;
    }

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

    uint32_t uid = sr_alloc_uid();
    /* Record in s_blocks, reusing an empty slot if possible. */
    int slot = -1;
    for (int i = 0; i < s_nblocks; i++) {
        if (s_blocks[i].uid == 0u) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && s_nblocks < HLE_MAX_PARTITION_BLOCKS) {
        slot = s_nblocks++;
    }
    if (slot >= 0) {
        s_blocks[slot].uid       = uid;
        s_blocks[slot].addr      = addr;
        s_blocks[slot].size      = size;
        s_blocks[slot].slot_size = slot_size;
        s_blocks[slot].prev      = prev;
        s_blocks[slot].next      = next;
        s_blocks[slot].high      = 0;
        s_blocks[slot].part5     = 0;
    }

    return uid;
}

/* Lowest live High block at or above addr, UINT32_MAX when none. */
static uint32_t partition_high_floor_above(uint32_t addr) {
    uint32_t floor = UINT32_MAX;
    for (int i = 0; i < s_nblocks; i++)
        if (s_blocks[i].uid != 0u && s_blocks[i].high && s_blocks[i].addr >= addr &&
            s_blocks[i].addr < floor)
            floor = s_blocks[i].addr;
    return floor;
}

/* A High block (PSP_SMEM_High): the highest free address range that holds it, taken from the top
 * of the arena the thread stacks share, as a PSP takes both from the top of the user partition.
 * Sizes round up to the partition's 0x100-byte granularity: pspautotests sysmem/memblock.expected
 * (2ed7c9c5) records two consecutive placement-1 blocks 0x100 apart for sizes 1 to 0x100, 0x200
 * for 0x101, 0x1100 for 0x1001 and 0x10100 for 0x10001, the second below the first. Returns the
 * block UID, or 0xFFFFFFFFu when no range holds it. */
/* High blocks come from the stack arena, which lies above kernel RAM; a partition based below the
 * runtime's stacks (an image linked low) ends under that arena, so it has no modelled High region
 * and its callers stop by name. */
int partition_high_modelled(void) {
    user_partition_init();
    return s_heap_base >= SR_NESTED_FRAME_END;
}

uint32_t alloc_block_high(uint32_t size) {
    user_partition_init();
    if (!partition_high_modelled()) return 0xFFFFFFFFu;
    if (size == 0u || size > UINT32_MAX - 0xffu) return 0xFFFFFFFFu;
    uint32_t slot_size = (size + 0xffu) & ~0xffu;
    int slot = -1;
    for (int i = 0; i < s_nblocks; i++)
        if (s_blocks[i].uid == 0u) { slot = i; break; }
    if (slot < 0) {
        if (s_nblocks >= HLE_MAX_PARTITION_BLOCKS) return 0xFFFFFFFFu;
        slot = s_nblocks;
    }
    uint32_t addr = 0u;
    if (!sched_partition_alloc_high(slot_size, user_partition_ceiling(), &addr)) return 0xFFFFFFFFu;
    if (slot == s_nblocks) s_nblocks++;
    sr_last_alloc_addr = addr;
    uint32_t uid = sr_alloc_uid();
    s_blocks[slot].uid       = uid;
    s_blocks[slot].addr      = addr;
    s_blocks[slot].size      = size;
    s_blocks[slot].slot_size = slot_size;
    s_blocks[slot].prev      = 0xFFFFFFFFu;
    s_blocks[slot].next      = 0u;
    s_blocks[slot].high      = 1;
    s_blocks[slot].part5     = 0;
    return uid;
}

/* Fixed-address reservation for the runtime module loader and for
 * sceKernelAllocPartitionMemory type 2 (PSP_SMEM_Addr / "Addr"): reserve exactly
 * [addr, addr+size) and return a block UID releasable with the existing free
 * path, or 0xFFFFFFFFu on failure. Failure (never corruption) results when the
 * range overlaps a live allocation, the loaded image/BSS, or lies outside
 * [heap base, partition top).
 *
 * Later Low allocations can never be handed reserved space, structurally:
 *  - a reservation at/above the bump pointer jumps s_heap past its end, and any
 *    virgin gap left behind ([old heap, addr)) is returned to the free list, so
 *    the bump pointer only ever moves forward over reserved ranges;
 *  - a reservation inside already-allocated space carves the range out of the
 *    free list (splitting one entry when strictly contained), so first-fit can
 *    never return overlapping space;
 *  - heap rollback on free can only land on a freed span: rollback targets are
 *    freed top spans or coalesced free spans, and a free span can never cross a
 *    live reservation, so s_heap stays >= every live reservation end and the
 *    bump path (which starts at s_heap) cannot reach one either.
 * Hence alloc_block itself is untouched: with no reservation live, every path
 * below is unreachable and its behaviour is byte-identical.
 *
 * UNMEASURED choices (no physical-hardware source): the address is honoured
 * exactly (no 256-byte rounding of addr/size, unlike Low's slot granularity);
 * slot_size tracks the exact size so free returns the exact range; a zero size
 * maps to 16 bytes like the existing handler; failure propagates the same
 * 0xFFFFFFFFu sentinel the existing OOM path returns rather than a guessed
 * firmware error code; only the partition bounds cap the size (SR_ALLOC_MAX stays
 * a game-request policy inside alloc_block, not a loader-path coupling). */
uint32_t sr_alloc_block_at(uint32_t addr, uint32_t size, const char *name) {
    (void)name;   /* kernel keeps block names for debug only; no guest-visible slot */
    user_partition_init();

    uint32_t eff = size ? size : 16u;
    uint64_t end64 = (uint64_t)addr + (uint64_t)eff;
    if (end64 > (uint64_t)0xFFFFFFFFu) return 0xFFFFFFFFu;
    uint32_t end = (uint32_t)end64;
    if (addr < s_heap_base || end > s_part_top) return 0xFFFFFFFFu;
    /* The runtime's stacks are never partition memory (user_partition_top). */
    if (addr < SR_NESTED_FRAME_END && end64 > USER_PARTITION_RUNTIME_STACKS_BASE)
        return 0xFFFFFFFFu;

    /* Live occupancy is the slot span [b.addr, b.addr+b.slot_size): Low blocks
     * own their 256-byte rounding slack, fixed blocks own their exact range. */
    for (int i = 0; i < s_nblocks; i++) {
        if (s_blocks[i].uid == 0u || s_blocks[i].addr == 0u) continue;
        uint64_t b_end = (uint64_t)s_blocks[i].addr + (uint64_t)s_blocks[i].slot_size;
        if ((uint64_t)addr < b_end && (uint64_t)s_blocks[i].addr < end64)
            return 0xFFFFFFFFu;
    }

    /* Below-heap space is either live (checked above) or free: every subspan of
     * [addr, end) below s_heap must be covered by free-list entries. */
    uint32_t old_heap = s_heap;
    uint32_t need = end <= old_heap ? end : old_heap;
    uint32_t cur = addr;
    if (cur < need) {
        for (int i = 0; i < s_nfree_blocks && cur < need; i++) {
            uint64_t f_end = (uint64_t)s_free_blocks[i].addr + (uint64_t)s_free_blocks[i].size;
            if (s_free_blocks[i].addr > cur) return 0xFFFFFFFFu;
            if (f_end > (uint64_t)cur) cur = f_end > (uint64_t)need ? need : (uint32_t)f_end;
        }
        if (cur < need) return 0xFFFFFFFFu;
    }

    /* Capacity pre-check so a half-carved state is impossible: at most one split
     * (a free entry strictly containing the range) plus at most one gap entry. */
    int need_slots = 0;
    for (int i = 0; i < s_nfree_blocks; i++) {
        uint64_t f_end = (uint64_t)s_free_blocks[i].addr + (uint64_t)s_free_blocks[i].size;
        if (s_free_blocks[i].addr < addr && end64 < f_end) { need_slots++; break; }
    }
    uint32_t gap_start = (old_heap + 0xFFu) & ~0xFFu;
    int have_gap = (addr > old_heap && gap_start < addr) ? 1 : 0;
    if (have_gap && gap_start < old_heap) return 0xFFFFFFFFu;   /* bump rounding wrapped */
    if (have_gap) need_slots++;
    /* A gap that spans the runtime's stacks (a low-based partition reserving above kernel
     * RAM) is returned as the two pieces either side of them. */
    int gap_splits = have_gap && gap_start < SR_NESTED_FRAME_END &&
                     addr > USER_PARTITION_RUNTIME_STACKS_BASE;
    if (gap_splits) need_slots++;
    if (s_nfree_blocks + need_slots > HLE_MAX_FREE_BLOCKS) return 0xFFFFFFFFu;

    int slot = -1;
    for (int i = 0; i < s_nblocks; i++) {
        if (s_blocks[i].uid == 0u) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_nblocks >= HLE_MAX_PARTITION_BLOCKS) return 0xFFFFFFFFu;
        slot = s_nblocks++;
    }

    /* Carve [addr, end) out of every overlapping free entry. */
    for (int i = 0; i < s_nfree_blocks;) {
        uint32_t f_addr = s_free_blocks[i].addr;
        uint64_t f_end64 = (uint64_t)f_addr + (uint64_t)s_free_blocks[i].size;
        if (f_addr >= end || f_end64 <= (uint64_t)addr) { i++; continue; }
        if (f_addr < addr && end64 < f_end64) {
            /* Strict containment: split into [f_addr, addr) + [end, f_end). */
            uint32_t right_addr = end;
            uint32_t right_size = (uint32_t)(f_end64 - end64);
            s_free_blocks[i].size = addr - f_addr;
            for (int j = s_nfree_blocks; j > i + 1; j--) s_free_blocks[j] = s_free_blocks[j - 1];
            s_free_blocks[i + 1].addr = right_addr;
            s_free_blocks[i + 1].size = right_size;
            s_nfree_blocks++;
            break;   /* one entry can strictly contain the range; the rest only touch it */
        } else if (f_addr < addr) {
            s_free_blocks[i].size = addr - f_addr;
            i++;
        } else if (end64 < f_end64) {
            uint32_t shrink = (uint32_t)(end64 - (uint64_t)f_addr);
            s_free_blocks[i].addr = end;
            s_free_blocks[i].size -= shrink;
            i++;
        } else {
            for (int j = i; j < s_nfree_blocks - 1; j++) s_free_blocks[j] = s_free_blocks[j + 1];
            s_nfree_blocks--;
        }
    }

    /* Return the skipped virgin gap to the free list (aligned start keeps later
     * Low heads 256-byte aligned: every Low take is a 0x100 multiple). The sliver
     * below the alignment stays leaked, exactly like today's bump alignment. */
    if (gap_splits) {
        if (gap_start < USER_PARTITION_RUNTIME_STACKS_BASE)
            free_list_insert_sorted(gap_start, USER_PARTITION_RUNTIME_STACKS_BASE - gap_start);
        if (addr > SR_NESTED_FRAME_END)
            free_list_insert_sorted(SR_NESTED_FRAME_END, addr - SR_NESTED_FRAME_END);
    } else if (have_gap) {
        free_list_insert_sorted(gap_start, addr - gap_start);
    }

    if (end > s_heap) {
        s_heap = end;
        s_heap_last_bump = s_heap;
    }
    sr_last_alloc_addr = addr;

    uint32_t prev = 0xFFFFFFFFu;
    uint32_t next = 0u;
    for (int i = s_nblocks - 1; i >= 0; --i) {
        if (i != slot && s_blocks[i].uid != 0u && s_blocks[i].addr != 0u) {
            prev = s_blocks[i].addr;
            s_blocks[i].next = addr;
            break;
        }
    }
    uint32_t uid = sr_alloc_uid();
    s_blocks[slot].uid       = uid;
    s_blocks[slot].addr      = addr;
    s_blocks[slot].size      = eff;
    s_blocks[slot].slot_size = eff;
    s_blocks[slot].prev      = prev;
    s_blocks[slot].next      = next;
    s_blocks[slot].high      = 0;
    s_blocks[slot].part5     = 0;

    return uid;
}

uint32_t block_addr(uint32_t uid) {
    for (int i = 0; i < s_nblocks; i++) if (s_blocks[i].uid == uid) return s_blocks[i].addr;
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
void sr_hle_test_partition_reset(void) {
    s_heap = 0;
    s_heap_base = 0;
    s_part_top = 0;
    s_heap_last_bump = 0;
    s_nblocks = 0;
    memset(s_blocks, 0, sizeof(s_blocks));
    memset(s_memory_blocks, 0, sizeof(s_memory_blocks));
    s_nfree_blocks = 0;
    memset(s_free_blocks, 0, sizeof(s_free_blocks));
}
int sr_hle_test_partition_free_block_count(void) { return s_nfree_blocks; }
int sr_hle_test_partition_get_free_block(int idx, uint32_t *addr_out, uint32_t *size_out) {
    if (idx < 0 || idx >= s_nfree_blocks) return 0;
    if (addr_out) *addr_out = s_free_blocks[idx].addr;
    if (size_out) *size_out = s_free_blocks[idx].size;
    return 1;
}
uint32_t sr_hle_test_partition_heap_ptr(void) {
    user_partition_init();
    return s_heap;
}
uint32_t sr_hle_test_partition_top(void) {
    user_partition_init();
    return s_part_top;
}
uint32_t sr_hle_test_partition_bump_top(void) {
    user_partition_init();
    return user_partition_top();   /* where bump allocations and the free sizes stop */
}
#endif

/* ---- handlers ---- */

uint32_t g_sdk_version = 0;
#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_compiled_sdk_version(void) {
    return sr_sdkver_get(&g_sdk_version);
}
#endif

/* sceUtilityGetSystemParamInt(id, int *out): write the system setting and return 0. A no-op that
 * leaves *out untouched makes the game read garbage for the language and load the wrong region
 * assets. IDs follow PSP_SYSTEMPARAM_ID_INT_*. This table seeds the virtual system registry
 * (sreg_systemparam_int), the one retained system profile every guest-visible reader of these
 * settings goes through: sceUtilityGetSystemParamInt, sceImpose, and the RTC local-time and
 * gettimeofday paths (psp_profile_local_offset_minutes, #1382). This is the one place the seed
 * values and their status are stated. Every value marked measured is the configured setting of the
 * PSP-3000 6.6.1 oracle console, not a factory default (factory defaults are unmeasured):
 * LANGUAGE, BUTTON_PREFERENCE, DATE_FORMAT, TIME_FORMAT and DAYLIGHTSAVINGS from the read-only
 * registry enumeration of 2026-10-09, and ADHOC_CHANNEL, TIMEZONE and LOCK_PARENTAL_LEVEL through
 * sceUtilityGetSystemParamInt on 2026-10-10 (hle-sysparam). WLAN_POWERSAVE is an unmeasured
 * project default. On the first run TIMEZONE and DAYLIGHTSAVINGS start from the host's local time
 * zone instead, when the launcher passes it (sreg_seed_host_time_zone). */
static uint32_t systemparam_int_value(uint32_t id) {
    switch (id) {
        case 2:  return 0;   /* ADHOC_CHANNEL: measured 0 (automatic) on the oracle, 2026-10-10 */
        case 3:  return 0;   /* WLAN_POWERSAVE: off (unmeasured project default) */
        case 4:  return 1;   /* DATE_FORMAT: measured */
        case 5:  return 1;   /* TIME_FORMAT: measured (1 = 12-hour in the PSPSDK enumeration) */
        case 6:  return 0xfffffed4u; /* TIMEZONE minutes from UTC: measured -300 on the oracle, 2026-10-10 */
        case 7:  return 1;   /* DAYLIGHTSAVINGS: measured 1 (PSPSDK SAVING) */
        case 8:  return 1;   /* LANGUAGE: measured (English) */
        case 9:  return 1;   /* BUTTON_PREFERENCE: measured */
        case 10: return 9;   /* LOCK_PARENTAL_LEVEL: measured 9 (off) on the oracle, 2026-10-10 */
        default: return 1;   /* unreachable through the getter, which refuses ids outside 2..10 */
    }
}

uint32_t sreg_systemparam_int(uint32_t id);
#define SREG_OVERLAY_SCHEMA "nakagawa.psp-system-registry-overlay"
#define SREG_OVERLAY_VERSION 1u
#define SREG_ALL_NODES (-2)
#define SREG_ERR_IO 0x80010005u           /* EIO: a flush could not persist the overlay */
#define SREG_NODE_LOGGED 0x04u      /* the placeholder read was already reported */

/* The /CONFIG tree measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration: every entry's
 * path, type and size, in the console's sceRegGetKeys order, minus the probe's own stray
 * categories and the user's saved network profiles. A string key's measured size depended on that
 * console's value, so every string row holds the empty-string size 1 instead. */
static const struct {
    const char *path;
    uint8_t type;
    uint16_t size;
} s_sreg_tree[] = {
    { "/CONFIG/VIDEO", SREG_TYPE_DIR, 0u },
    { "/CONFIG/VIDEO/menu_language", SREG_TYPE_BIN, 2u },
    { "/CONFIG/VIDEO/sound_language", SREG_TYPE_BIN, 2u },
    { "/CONFIG/VIDEO/subtitle_language", SREG_TYPE_BIN, 2u },
    { "/CONFIG/VIDEO/appended_volume", SREG_TYPE_INT, 4u },
    { "/CONFIG/VIDEO/lr_button_enable", SREG_TYPE_INT, 4u },
    { "/CONFIG/VIDEO/list_play_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/VIDEO/title_display_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/VIDEO/output_ext_menu", SREG_TYPE_INT, 4u },
    { "/CONFIG/VIDEO/output_ext_func", SREG_TYPE_INT, 4u },
    { "/CONFIG/PHOTO", SREG_TYPE_DIR, 0u },
    { "/CONFIG/PHOTO/slideshow_speed", SREG_TYPE_INT, 4u },
    { "/CONFIG/MUSIC", SREG_TYPE_DIR, 0u },
    { "/CONFIG/MUSIC/wma_play", SREG_TYPE_INT, 4u },
    { "/CONFIG/MUSIC/visualizer_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/MUSIC/track_info_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BROWSER/home_uri", SREG_TYPE_STR, 1u },
    { "/CONFIG/BROWSER/cookie_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/proxy_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/proxy_address", SREG_TYPE_STR, 1u },
    { "/CONFIG/BROWSER/proxy_port", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/picture", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/animation", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/javascript", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/cache_size", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/char_size", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/disp_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/connect_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/flash_activated", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/flash_play", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/proxy_protect", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/proxy_autoauth", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER/proxy_user", SREG_TYPE_STR, 1u },
    { "/CONFIG/BROWSER/proxy_password", SREG_TYPE_STR, 1u },
    { "/CONFIG/BROWSER/webpage_quality", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER2", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BROWSER2/tm_service", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER2/tm_ec_ttl", SREG_TYPE_INT, 4u },
    { "/CONFIG/BROWSER2/tm_ec_ttl_update_time", SREG_TYPE_BIN, 8u },
    { "/CONFIG/BROWSER2/tm_service_sub_status", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV", SREG_TYPE_DIR, 0u },
    { "/CONFIG/LFTV/easy_reg_done", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/netav_domain_name", SREG_TYPE_STR, 1u },
    { "/CONFIG/LFTV/netav_ip_address", SREG_TYPE_STR, 1u },
    { "/CONFIG/LFTV/netav_port_no_home", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/netav_port_no_away", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/netav_nonce", SREG_TYPE_STR, 1u },
    { "/CONFIG/LFTV/base_station_version", SREG_TYPE_STR, 1u },
    { "/CONFIG/LFTV/base_station_region", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/tuner_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/input_line", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/tv_channel", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/bitrate_home", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/bitrate_away", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/channel_setting_jp", SREG_TYPE_BIN, 24u },
    { "/CONFIG/LFTV/channel_setting_us", SREG_TYPE_BIN, 68u },
    { "/CONFIG/LFTV/channel_setting_us_catv", SREG_TYPE_BIN, 125u },
    { "/CONFIG/LFTV/overwrite_netav_setting", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/screen_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/remocon_setting_region", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/remocon_setting", SREG_TYPE_BIN, 96u },
    { "/CONFIG/LFTV/remocon_setting_revision", SREG_TYPE_STR, 1u },
    { "/CONFIG/LFTV/external_tuner_channel", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/ssid", SREG_TYPE_STR, 1u },
    { "/CONFIG/LFTV/audio_gain", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/broadcast_standard_video1", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/broadcast_standard_video2", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/version", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/tv_channel_range", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/tuner_type_no", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/input_line_no", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/audio_channel", SREG_TYPE_INT, 4u },
    { "/CONFIG/LFTV/shared_remocon_setting", SREG_TYPE_BIN, 96u },
    { "/CONFIG/RSS", SREG_TYPE_DIR, 0u },
    { "/CONFIG/RSS/download_items", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM", SREG_TYPE_DIR, 0u },
    { "/CONFIG/ALARM/alarm_0_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_1_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_2_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_3_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_4_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_5_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_6_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_7_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_8_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_9_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_0_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_1_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_2_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_3_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_4_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_5_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_6_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_7_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_8_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/ALARM/alarm_9_property", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO", SREG_TYPE_DIR, 0u },
    { "/CONFIG/PREMO/guide_page", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/response", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/ps3_name", SREG_TYPE_STR, 1u },
    { "/CONFIG/PREMO/ps3_mac", SREG_TYPE_BIN, 6u },
    { "/CONFIG/PREMO/ps3_keytype", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/ps3_key", SREG_TYPE_BIN, 16u },
    { "/CONFIG/PREMO/custom_video_bitrate1", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/custom_video_bitrate2", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/custom_video_buffer1", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/custom_video_buffer2", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/setting_internet", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/button_assign", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/flags", SREG_TYPE_INT, 4u },
    { "/CONFIG/PREMO/account_id", SREG_TYPE_BIN, 16u },
    { "/CONFIG/PREMO/login_id", SREG_TYPE_STR, 1u },
    { "/CONFIG/PREMO/password", SREG_TYPE_STR, 1u },
    { "/CONFIG/CAMERA", SREG_TYPE_DIR, 0u },
    { "/CONFIG/CAMERA/still_size", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/movie_size", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/still_quality", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/movie_quality", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/movie_fps", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/white_balance", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/exposure_bias", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/shutter_sound_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/file_folder", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/file_number", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/msid", SREG_TYPE_BIN, 16u },
    { "/CONFIG/CAMERA/still_effect", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/medium_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/file_number_eflash", SREG_TYPE_INT, 4u },
    { "/CONFIG/CAMERA/folder_number_eflash", SREG_TYPE_INT, 4u },
    { "/CONFIG/DISPLAY", SREG_TYPE_DIR, 0u },
    { "/CONFIG/DISPLAY/aspect_ratio", SREG_TYPE_INT, 4u },
    { "/CONFIG/DISPLAY/scan_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/DISPLAY/screensaver_start_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/DISPLAY/color_space_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/DISPLAY/pi_blending_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/NP", SREG_TYPE_DIR, 0u },
    { "/CONFIG/NP/env", SREG_TYPE_STR, 1u },
    { "/CONFIG/NP/account_id", SREG_TYPE_BIN, 16u },
    { "/CONFIG/NP/login_id", SREG_TYPE_STR, 1u },
    { "/CONFIG/NP/password", SREG_TYPE_STR, 1u },
    { "/CONFIG/NP/auto_sign_in_enable", SREG_TYPE_INT, 4u },
    { "/CONFIG/NP/nav_only", SREG_TYPE_INT, 4u },
    { "/CONFIG/NP/np_ad_clock_diff", SREG_TYPE_INT, 4u },
    { "/CONFIG/NP/view_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/NP/np_geo_filtering", SREG_TYPE_INT, 4u },
    { "/CONFIG/NP/guest_country", SREG_TYPE_STR, 1u },
    { "/CONFIG/NP/guest_lang", SREG_TYPE_STR, 1u },
    { "/CONFIG/NP/guest_yob", SREG_TYPE_INT, 4u },
    { "/CONFIG/NP/guest_mob", SREG_TYPE_INT, 4u },
    { "/CONFIG/NP/guest_dob", SREG_TYPE_INT, 4u },
    { "/CONFIG/NP/check_drm", SREG_TYPE_INT, 4u },
    { "/CONFIG/ONESEG", SREG_TYPE_DIR, 0u },
    { "/CONFIG/ONESEG/schedule_data_key", SREG_TYPE_BIN, 16u },
    { "/CONFIG/SYSTEM", SREG_TYPE_DIR, 0u },
    { "/CONFIG/SYSTEM/owner_name", SREG_TYPE_STR, 1u },
    { "/CONFIG/SYSTEM/backlight_brightness", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/umd_autoboot", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/usb_charge", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/umd_cache", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/usb_auto_connect", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/slide_action", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/first_boot_tick", SREG_TYPE_BIN, 64u },
    { "/CONFIG/SYSTEM/owner_mob", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/owner_dob", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/slide_welcome", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/exh_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/XMB", SREG_TYPE_DIR, 0u },
    { "/CONFIG/SYSTEM/XMB/theme_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/XMB/language", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/XMB/button_assign", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/XMB/THEME", SREG_TYPE_DIR, 0u },
    { "/CONFIG/SYSTEM/XMB/THEME/color_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/XMB/THEME/wallpaper_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/XMB/THEME/system_color", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/XMB/THEME/custom_theme_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/SOUND", SREG_TYPE_DIR, 0u },
    { "/CONFIG/SYSTEM/SOUND/main_volume", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/SOUND/mute", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/SOUND/avls", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/SOUND/equalizer_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/SOUND/operation_sound_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/SOUND/dynamic_normalizer", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/POWER_SAVING", SREG_TYPE_DIR, 0u },
    { "/CONFIG/SYSTEM/POWER_SAVING/suspend_interval", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/POWER_SAVING/backlight_off_interval", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/POWER_SAVING/wlan_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/POWER_SAVING/active_backlight_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/LOCK", SREG_TYPE_DIR, 0u },
    { "/CONFIG/SYSTEM/LOCK/password", SREG_TYPE_BIN, 4u },
    { "/CONFIG/SYSTEM/LOCK/parental_level", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/LOCK/browser_start", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/CHARACTER_SET", SREG_TYPE_DIR, 0u },
    { "/CONFIG/SYSTEM/CHARACTER_SET/oem", SREG_TYPE_INT, 4u },
    { "/CONFIG/SYSTEM/CHARACTER_SET/ansi", SREG_TYPE_INT, 4u },
    { "/CONFIG/DATE", SREG_TYPE_DIR, 0u },
    { "/CONFIG/DATE/time_format", SREG_TYPE_INT, 4u },
    { "/CONFIG/DATE/date_format", SREG_TYPE_INT, 4u },
    { "/CONFIG/DATE/summer_time", SREG_TYPE_INT, 4u },
    { "/CONFIG/DATE/time_zone_offset", SREG_TYPE_INT, 4u },
    { "/CONFIG/DATE/time_zone_area", SREG_TYPE_STR, 1u },
    { "/CONFIG/NETWORK", SREG_TYPE_DIR, 0u },
    { "/CONFIG/NETWORK/ADHOC", SREG_TYPE_DIR, 0u },
    { "/CONFIG/NETWORK/ADHOC/channel", SREG_TYPE_INT, 4u },
    { "/CONFIG/NETWORK/ADHOC/ssid_prefix", SREG_TYPE_STR, 1u },
    { "/CONFIG/NETWORK/INFRASTRUCTURE", SREG_TYPE_DIR, 0u },
    { "/CONFIG/NETWORK/INFRASTRUCTURE/latest_id", SREG_TYPE_INT, 4u },
    { "/CONFIG/NETWORK/INFRASTRUCTURE/eap_md5", SREG_TYPE_INT, 4u },
    { "/CONFIG/NETWORK/INFRASTRUCTURE/auto_setting", SREG_TYPE_INT, 4u },
    { "/CONFIG/NETWORK/INFRASTRUCTURE/wifisvc_setting", SREG_TYPE_INT, 4u },
    { "/CONFIG/NETWORK/INFRASTRUCTURE/btdun_warnings_check", SREG_TYPE_INT, 4u },
    { "/CONFIG/NETWORK/GO_MESSENGER", SREG_TYPE_DIR, 0u },
    { "/CONFIG/NETWORK/GO_MESSENGER/auth_name", SREG_TYPE_STR, 1u },
    { "/CONFIG/NETWORK/GO_MESSENGER/auth_key", SREG_TYPE_STR, 1u },
    { "/CONFIG/OSK", SREG_TYPE_DIR, 0u },
    { "/CONFIG/OSK/version_id", SREG_TYPE_INT, 4u },
    { "/CONFIG/OSK/disp_locale", SREG_TYPE_INT, 4u },
    { "/CONFIG/OSK/writing_locale", SREG_TYPE_INT, 4u },
    { "/CONFIG/OSK/input_char_mask", SREG_TYPE_INT, 4u },
    { "/CONFIG/OSK/keytop_index", SREG_TYPE_INT, 4u },
    { "/CONFIG/INFOBOARD", SREG_TYPE_DIR, 0u },
    { "/CONFIG/INFOBOARD/locale_lang", SREG_TYPE_STR, 1u },
    { "/CONFIG/INFOBOARD/qa_server", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BT/connect_mode", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE0", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BT/DEVICE0/audio_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE0/device_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE0/device_name", SREG_TYPE_BIN, 64u },
    { "/CONFIG/BT/DEVICE1", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BT/DEVICE1/audio_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE1/device_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE1/device_name", SREG_TYPE_BIN, 64u },
    { "/CONFIG/BT/DEVICE2", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BT/DEVICE2/audio_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE2/device_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE2/device_name", SREG_TYPE_BIN, 64u },
    { "/CONFIG/BT/DEVICE3", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BT/DEVICE3/audio_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE3/device_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE3/device_name", SREG_TYPE_BIN, 64u },
    { "/CONFIG/BT/DEVICE4", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BT/DEVICE4/audio_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE4/device_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE4/device_name", SREG_TYPE_BIN, 64u },
    { "/CONFIG/BT/DEVICE5", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BT/DEVICE5/audio_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE5/device_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE5/device_name", SREG_TYPE_BIN, 64u },
    { "/CONFIG/BT/DEVICE6", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BT/DEVICE6/audio_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE6/device_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE6/device_name", SREG_TYPE_BIN, 64u },
    { "/CONFIG/BT/DEVICE7", SREG_TYPE_DIR, 0u },
    { "/CONFIG/BT/DEVICE7/audio_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE7/device_type", SREG_TYPE_INT, 4u },
    { "/CONFIG/BT/DEVICE7/device_name", SREG_TYPE_BIN, 64u },
    { "/CONFIG/GAME", SREG_TYPE_DIR, 0u },
    { "/CONFIG/GAME/hibernation_ow_guide", SREG_TYPE_INT, 4u },
    { "/CONFIG/GAME/hibernation_op_guide", SREG_TYPE_INT, 4u },
    { "/CONFIG/GAME/subs_expiration_guide", SREG_TYPE_INT, 4u },
    /* PSP-3000 6.6.1, 2026-10-20 (PSP-REGISTRY-002): /SYSPROFILE lists two entries, one of them
     * RESOLUTION (DIR), which lists horizontal and vertical (INT, 4 bytes). The other entry's name
     * and RESOLUTION's position in the list were not measured, so only RESOLUTION is served. */
    { "/SYSPROFILE/RESOLUTION", SREG_TYPE_DIR, 0u },
    { "/SYSPROFILE/RESOLUTION/horizontal", SREG_TYPE_INT, 4u },
    { "/SYSPROFILE/RESOLUTION/vertical", SREG_TYPE_INT, 4u },
};

/* The integer keys sceUtilityGetSystemParamInt shares, with their PSP_SYSTEMPARAM_ID_INT_* id. */
static const struct {
    const char *path;
    uint32_t sysparam_id;
} s_sreg_sysparam_keys[] = {
    { "/CONFIG/SYSTEM/XMB/language",      8u },
    { "/CONFIG/SYSTEM/XMB/button_assign", 9u },
    { "/CONFIG/DATE/date_format",         4u },
    { "/CONFIG/DATE/time_format",         5u },
    { "/CONFIG/DATE/time_zone_offset",    6u },
    { "/CONFIG/DATE/summer_time",         7u },
    { "/CONFIG/NETWORK/ADHOC/channel",    2u },
};

/* Measured values of keys with no system-param id (PSP-3000 6.6.1; PREMO 2026-10-09, RESOLUTION
 * 2026-10-20). The RESOLUTION values are raw registry integers; their meaning is not inferred. */
static const struct {
    const char *path;
    uint32_t value;
} s_sreg_measured_ints[] = {
    { "/CONFIG/PREMO/button_assign", 0u },
    { "/SYSPROFILE/RESOLUTION/horizontal", 0x2012u },
    { "/SYSPROFILE/RESOLUTION/vertical", 0x2012u },
};

SregNode s_sreg_nodes[SREG_MAX_NODES];
static uint32_t s_sreg_seq;
static uint16_t s_sreg_next_id;
static int s_sreg_loaded;
int s_sreg_dirty;            /* the live registry differs from the last persisted overlay */
static int s_sreg_corrupt_on_disk;  /* an ignored corrupt overlay is moved aside by the next flush */

void sreg_free_node(int i) {
    free(s_sreg_nodes[i].value);
    memset(&s_sreg_nodes[i], 0, sizeof(s_sreg_nodes[i]));
}

static void sreg_clear_nodes(void) {
    for (uint32_t i = 0; i < SREG_MAX_NODES; i++) sreg_free_node((int)i);
    s_sreg_seq = 0;
    s_sreg_next_id = 0;
    s_sreg_dirty = 0;
}

static uint32_t sreg_free_node_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SREG_MAX_NODES; i++)
        if (!s_sreg_nodes[i].id) n++;
    return n;
}

int sreg_child(int parent, const char *name) {
    for (uint32_t i = 0; i < SREG_MAX_NODES; i++) {
        const SregNode *n = &s_sreg_nodes[i];
        if (n->id && n->parent == parent && strcmp(n->name, name) == 0) return (int)i;
    }
    return -1;
}

/* A fresh nonzero id that no live node carries, so a stale key handle can never name a newer
 * entry while fewer than 65535 ids are live (the node table holds 512). */
static uint16_t sreg_new_id(void) {
    for (uint32_t tries = 0; tries < 0x10000u; tries++) {
        uint16_t id = ++s_sreg_next_id;
        int used = 0;
        if (!id) continue;
        for (uint32_t i = 0; i < SREG_MAX_NODES && !used; i++) used = s_sreg_nodes[i].id == id;
        if (!used) return id;
    }
    return 0;
}

/* Add an entry (name already validated) under parent, -1 being the top level. A value starts
 * zero-filled. Returns the node index, or -1 when no node or value storage is free. */
int sreg_add(int parent, const char *name, uint32_t type, uint32_t size, uint8_t flags) {
    for (uint32_t i = 0; i < SREG_MAX_NODES; i++) {
        SregNode *n = &s_sreg_nodes[i];
        uint8_t *value = NULL;
        uint16_t id;
        if (n->id) continue;
        if (size) {
            value = (uint8_t *)calloc(1u, size);
            if (!value) return -1;
        }
        id = sreg_new_id();
        if (!id) {
            free(value);
            return -1;
        }
        memset(n, 0, sizeof(*n));
        n->id = id;
        n->type = (uint8_t)type;
        n->flags = flags;
        n->parent = parent;
        n->sysparam_id = -1;
        n->seq = ++s_sreg_seq;
        n->size = size;
        n->value = value;
        memcpy(n->name, name, strlen(name) + 1u);
        return (int)i;
    }
    return -1;
}

/* Resolve an absolute category path such as "/CONFIG/SYSTEM" (one trailing '/' is ignored). With
 * create, missing components are made as categories carrying flags, each new name cut to 26 bytes
 * (measured), and *created reports it. Lookups compare whole components, so a component longer
 * than 26 bytes never matches a stored name. The first pass validates every component and counts
 * free nodes, so a refused path creates nothing. Returns the node index, or -1 with *err set. */
int sreg_walk(const char *path, int create, uint8_t flags, uint32_t *err, int *created) {
    char comp[SREG_PATH_BYTES];
    size_t len = strlen(path);
    int cur = -1;
    if (created) *created = 0;
    if (len > 1u && path[len - 1u] == '/') len--;
    if (len < 2u || len >= SREG_PATH_BYTES || path[0] != '/') {
        *err = SREG_ERR_NOT_FOUND;
        return -1;
    }
    for (int pass = 0; pass < 2; pass++) {
        size_t at = 1u;
        uint32_t missing = 0;
        int exists = 1;
        cur = -1;
        while (at < len) {
            size_t end = at, n;
            int child;
            while (end < len && path[end] != '/') end++;
            n = end - at;
            if (n == 0u) {
                *err = SREG_ERR_NOT_FOUND; /* an empty component ("//") names nothing */
                return -1;
            }
            memcpy(comp, path + at, n);
            comp[n] = '\0';
            child = exists && n < SREG_NAME_BYTES ? sreg_child(cur, comp) : -1;
            if (child >= 0) {
                if (s_sreg_nodes[child].type != SREG_TYPE_DIR) {
                    /* A value key, not a category: it cannot be opened, nor replaced by one. */
                    *err = create ? SREG_ERR_EXIST : SREG_ERR_NOT_FOUND;
                    return -1;
                }
                cur = child;
            } else {
                exists = 0;
                if (!create) {
                    *err = SREG_ERR_NOT_FOUND;
                    return -1;
                }
                if (cur >= 0 && (s_sreg_nodes[cur].flags & SREG_NODE_READONLY) &&
                    !(flags & SREG_NODE_READONLY)) {
                    /* A read-only subtree is generated from the served fonts: an open cannot add to
                     * it, so a missing entry (an absent font index) finds nothing. */
                    fprintf(stderr, "sceReg: \"%s\" is not served: the font subtree is read-only and an "
                                    "open does not create entries in it\n", path);
                    *err = SREG_ERR_NOT_FOUND;
                    return -1;
                }
                if (n >= SREG_NAME_BYTES) comp[SREG_NAME_BYTES - 1u] = '\0';
                if (!sreg_name_ok(comp)) {
                    *err = SREG_ERR_INVAL;
                    return -1;
                }
                if (pass == 0) {
                    missing++;
                } else {
                    cur = sreg_add(cur, comp, SREG_TYPE_DIR, 0u, flags);
                    if (cur < 0) {
                        *err = SREG_ERR_NOSPC;
                        return -1;
                    }
                    if (created) *created = 1;
                }
            }
            at = end + 1u;
        }
        if (pass == 0) {
            if (missing == 0u) return cur;
            if (missing > sreg_free_node_count()) {
                *err = SREG_ERR_NOSPC;
                return -1;
            }
        }
    }
    return cur;
}

/* The absolute path of a category node. */
int sreg_node_path(int node, char *out, size_t cap) {
    int chain[SREG_PATH_BYTES / 2u];
    int depth = 0;
    size_t at = 0;
    for (int n = node; n >= 0; n = s_sreg_nodes[n].parent) {
        if (depth >= (int)(sizeof(chain) / sizeof(chain[0]))) return 0;
        chain[depth++] = n;
    }
    for (int d = depth - 1; d >= 0; d--) {
        const char *name = s_sreg_nodes[chain[d]].name;
        size_t n = strlen(name);
        if (at + n + 2u > cap) return 0;
        out[at++] = '/';
        memcpy(out + at, name, n);
        at += n;
    }
    if (at + 1u > cap) return 0;
    out[at] = '\0';
    return 1;
}

/* Live nodes in creation order: the entries of one category, or every node with SREG_ALL_NODES.
 * out holds SREG_MAX_NODES indices. */
uint32_t sreg_ordered(int parent, int *out) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < SREG_MAX_NODES; i++) {
        const SregNode *n = &s_sreg_nodes[i];
        uint32_t at;
        if (!n->id || (parent != SREG_ALL_NODES && n->parent != parent)) continue;
        at = count++;
        while (at > 0u && s_sreg_nodes[out[at - 1u]].seq > n->seq) {
            out[at] = out[at - 1u];
            at--;
        }
        out[at] = (int)i;
    }
    return count;
}

/* The node an absolute key path names ("/CONFIG/DATE/time_format"), or -1. */
int sreg_find_path(const char *path) {
    char cat[SREG_PATH_BYTES];
    const char *slash = strrchr(path, '/');
    uint32_t err = 0;
    int parent;
    size_t n = slash ? (size_t)(slash - path) : 0u;
    if (!slash || n == 0u || n >= sizeof(cat)) return -1;
    memcpy(cat, path, n);
    cat[n] = '\0';
    parent = sreg_walk(cat, 0, 0u, &err, NULL);
    return parent < 0 ? -1 : sreg_child(parent, slash + 1);
}

/* ---- /DATA/FONT: the font list the guest's own font library reads (FONT_PLAN section 4) ----
 * A read-only subtree generated from the served fonts when the defaults are built; it is never
 * persisted in the overlay and no write reaches it (SREG_NODE_READONLY). Which keys exist, their
 * order, and the numeric codes of each index come from flash0_font.c, where what a PSP-3000
 * measured is told apart from what it did not. The key names and types below are NOT measured:
 * they follow FONT_PLAN 4.2 and the keys the guest library is seen to ask for, and an unknown
 * key stays a named refusal. An index with no usable served font has no INFO<n> category. */
void flash0_font_sources(Flash0Sources *out);

/* Build the defaults: the measured tree, then the system-param-backed and measured values. Every
 * other value key keeps its neutral placeholder (INT 0, an empty string, or zero bytes). */
static void sreg_build_defaults(void) {
    uint32_t err = 0;
    (void)sreg_walk("/CONFIG", 1, SREG_NODE_DEFAULT, &err, NULL);
    (void)sreg_walk("/SYSPROFILE", 1, SREG_NODE_DEFAULT, &err, NULL);
    for (uint32_t i = 0; i < sizeof(s_sreg_tree) / sizeof(s_sreg_tree[0]); i++) {
        const char *path = s_sreg_tree[i].path;
        const char *slash = strrchr(path, '/');
        char cat[SREG_PATH_BYTES];
        size_t n = (size_t)(slash - path);
        int parent;
        memcpy(cat, path, n);
        cat[n] = '\0';
        parent = sreg_walk(cat, 0, 0u, &err, NULL);
        if (parent < 0) continue;
        (void)sreg_add(parent, slash + 1, s_sreg_tree[i].type, s_sreg_tree[i].size,
                       (uint8_t)(SREG_NODE_DEFAULT |
                                 (s_sreg_tree[i].type == SREG_TYPE_DIR ? 0u : SREG_NODE_PLACEHOLDER)));
    }
    for (uint32_t i = 0; i < sizeof(s_sreg_sysparam_keys) / sizeof(s_sreg_sysparam_keys[0]); i++) {
        int node = sreg_find_path(s_sreg_sysparam_keys[i].path);
        if (node < 0 || s_sreg_nodes[node].type != SREG_TYPE_INT) continue;
        s_sreg_nodes[node].sysparam_id = (int)s_sreg_sysparam_keys[i].sysparam_id;
        s_sreg_nodes[node].flags &= (uint8_t)~SREG_NODE_PLACEHOLDER;
        sreg_put_le32(s_sreg_nodes[node].value, systemparam_int_value(s_sreg_sysparam_keys[i].sysparam_id));
    }
    for (uint32_t i = 0; i < sizeof(s_sreg_measured_ints) / sizeof(s_sreg_measured_ints[0]); i++) {
        int node = sreg_find_path(s_sreg_measured_ints[i].path);
        if (node < 0 || s_sreg_nodes[node].type != SREG_TYPE_INT) continue;
        s_sreg_nodes[node].flags &= (uint8_t)~SREG_NODE_PLACEHOLDER;
        sreg_put_le32(s_sreg_nodes[node].value, s_sreg_measured_ints[i].value);
    }
    sreg_build_font_registry();
}

/* True when a value key holds its default, so the overlay need not record it. */
static int sreg_is_default_value(const SregNode *n) {
    if (!(n->flags & SREG_NODE_DEFAULT) || n->type == SREG_TYPE_DIR) return 0;
    if (n->sysparam_id >= 0)
        return n->size == 4u && sreg_get_le32(n->value) == systemparam_int_value((uint32_t)n->sysparam_id);
    for (uint32_t i = 0; i < sizeof(s_sreg_measured_ints) / sizeof(s_sreg_measured_ints[0]); i++) {
        if (n->type == SREG_TYPE_INT && n->size == 4u &&
            sreg_find_path(s_sreg_measured_ints[i].path) == (int)(n - s_sreg_nodes))
            return sreg_get_le32(n->value) == s_sreg_measured_ints[i].value;
    }
    if (n->type == SREG_TYPE_STR && n->size != 1u) return 0; /* the default is the empty string */
    for (uint32_t i = 0; i < n->size; i++)
        if (n->value[i]) return 0;
    return 1;
}

int sr_utf8_env_alloc(const wchar_t *name, char **value_out, int *present_out);
PspConsoleProfile s_console;

static int sreg_hex_bytes(const char *hex, uint8_t *out, size_t bytes);

static int console_profile_apply(const NkJsonNode *console, char *why, size_t why_len) {
    const NkJsonNode *jmac = nk_json_obj_get(console, "wlan_mac_hex");
    const NkJsonNode *jpsid = nk_json_obj_get(console, "open_psid_hex");
    PspConsoleProfile next;
    memset(&next, 0, sizeof(next));
    if (!nk_json_is_object(console) || !nk_json_is_string(jmac) || !nk_json_is_string(jpsid) ||
        !sreg_hex_bytes(nk_json_get_string(jmac), next.mac, sizeof(next.mac)) ||
        !sreg_hex_bytes(nk_json_get_string(jpsid), next.psid, sizeof(next.psid)) ||
        !console_identity_ok(next.mac, next.psid)) {
        snprintf(why, why_len, "its console object does not hold a wlan_mac_hex (12 hex digits, "
                               "first octet's two low bits clear) and a non-zero open_psid_hex "
                               "(32 hex digits)");
        return 0;
    }
    next.have_identity = 1;
    const NkJsonNode *jswitch = nk_json_obj_get(console, "wlan_switch");
    if (jswitch) {
        if (!nk_json_get_uint32(jswitch, &next.wlan_switch) || next.wlan_switch > 1u) {
            snprintf(why, why_len, "its console object's wlan_switch is not 0 or 1");
            return 0;
        }
        next.wlan_switch_set = 1;
    }
    s_console = next;
    return 1;
}

/* The host's random source: rand_s() (RtlGenRandom) on Windows, /dev/urandom elsewhere. */
static int console_random_bytes(uint8_t *out, size_t n) {
#ifdef _WIN32
    for (size_t i = 0; i < n; i++) {
        unsigned int r = 0;
        if (rand_s(&r) != 0) return 0;
        out[i] = (uint8_t)r;
    }
    return 1;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    int ok = f && fread(out, 1u, n, f) == n;
    if (f) fclose(f);
    return ok;
#endif
}

/* Apply a parsed overlay onto the defaults. Returns 1, or 0 with *why set; the caller discards a
 * partial application and serves the defaults. */
static int sreg_overlay_apply(const NkJsonNode *doc, char *why, size_t why_len) {
    const NkJsonNode *schema = nk_json_obj_get(doc, "schema");
    const NkJsonNode *cats = nk_json_obj_get(doc, "categories");
    const NkJsonNode *keys = nk_json_obj_get(doc, "keys");
    uint32_t version = 0, err = 0;
    static uint8_t value[SREG_VALUE_MAX];
    if (!nk_json_is_object(doc) || !nk_json_is_string(schema) ||
        strcmp(nk_json_get_string(schema), SREG_OVERLAY_SCHEMA) != 0) {
        snprintf(why, why_len, "it is not a system registry overlay");
        return 0;
    }
    if (!nk_json_get_uint32(nk_json_obj_get(doc, "schema_version"), &version) ||
        version != SREG_OVERLAY_VERSION) {
        snprintf(why, why_len, "its schema_version is not %u", SREG_OVERLAY_VERSION);
        return 0;
    }
    if (!nk_json_is_array(cats) || !nk_json_is_array(keys)) {
        snprintf(why, why_len, "it lacks the categories and keys arrays");
        return 0;
    }
    for (size_t i = 0; i < nk_json_array_count(cats); i++) {
        const NkJsonNode *item = nk_json_array_get(cats, i);
        const char *path = nk_json_is_string(item) ? nk_json_get_string(item) : NULL;
        if (!path || strlen(path) >= SREG_PATH_BYTES || sreg_walk(path, 1, 0u, &err, NULL) < 0) {
            snprintf(why, why_len, "categories[%u] is not a valid category path", (unsigned)i);
            return 0;
        }
    }
    for (size_t i = 0; i < nk_json_array_count(keys); i++) {
        const NkJsonNode *item = nk_json_array_get(keys, i);
        const NkJsonNode *jcat = nk_json_obj_get(item, "category");
        const NkJsonNode *jname = nk_json_obj_get(item, "name");
        const NkJsonNode *jtype = nk_json_obj_get(item, "type");
        const NkJsonNode *jhex = nk_json_obj_get(item, "value_hex");
        const char *name, *hex;
        uint32_t type, bytes;
        size_t hex_len;
        int cat, node;
        if (!nk_json_is_object(item) || !nk_json_is_string(jcat) || !nk_json_is_string(jname) ||
            !nk_json_is_string(jtype) || !nk_json_is_string(jhex)) {
            snprintf(why, why_len, "keys[%u] lacks category, name, type or value_hex", (unsigned)i);
            return 0;
        }
        name = nk_json_get_string(jname);
        hex = nk_json_get_string(jhex);
        type = sreg_type_from_name(nk_json_get_string(jtype));
        hex_len = strlen(hex);
        cat = strlen(nk_json_get_string(jcat)) < SREG_PATH_BYTES
                  ? sreg_walk(nk_json_get_string(jcat), 0, 0u, &err, NULL) : -1;
        if (cat < 0 || !sreg_name_ok(name) || !type || hex_len == 0u || (hex_len & 1u) ||
            hex_len > 2u * SREG_VALUE_MAX || (type == SREG_TYPE_INT && hex_len != 8u)) {
            snprintf(why, why_len, "keys[%u] is not a valid key record", (unsigned)i);
            return 0;
        }
        bytes = (uint32_t)(hex_len / 2u);
        for (uint32_t b = 0; b < bytes; b++) {
            int hi = sreg_hex_nibble(hex[2u * b]), lo = sreg_hex_nibble(hex[2u * b + 1u]);
            if (hi < 0 || lo < 0) {
                snprintf(why, why_len, "keys[%u] value_hex is not hexadecimal", (unsigned)i);
                return 0;
            }
            value[b] = (uint8_t)((hi << 4) | lo);
        }
        node = sreg_child(cat, name);
        if (node >= 0) {
            /* Only a default may be overridden, with its own type (and size, unless a string);
             * anything else is a duplicate record or a key that is a category. */
            SregNode *n = &s_sreg_nodes[node];
            if (!(n->flags & SREG_NODE_DEFAULT) || (n->flags & SREG_NODE_READONLY) || n->type != type ||
                (type != SREG_TYPE_STR && n->size != bytes)) {
                snprintf(why, why_len, "keys[%u] conflicts with an existing entry", (unsigned)i);
                return 0;
            }
            if (n->size != bytes) {
                uint8_t *grown = (uint8_t *)realloc(n->value, bytes);
                if (!grown) {
                    snprintf(why, why_len, "no memory for keys[%u]", (unsigned)i);
                    return 0;
                }
                n->value = grown;
                n->size = bytes;
            }
            n->flags &= (uint8_t)~SREG_NODE_PLACEHOLDER;
        } else {
            if (s_sreg_nodes[cat].flags & SREG_NODE_READONLY) {
                snprintf(why, why_len, "keys[%u] adds a key to a read-only category", (unsigned)i);
                return 0;
            }
            node = sreg_add(cat, name, type, bytes, 0u);
            if (node < 0) {
                snprintf(why, why_len, "it holds more entries than the registry model's %u", SREG_MAX_NODES);
                return 0;
            }
        }
        memcpy(s_sreg_nodes[node].value, value, bytes);
    }
    if (nk_json_obj_get(doc, "console") &&
        !console_profile_apply(nk_json_obj_get(doc, "console"), why, why_len))
        return 0;
    return 1;
}

/* Exactly `bytes` bytes from 2 * bytes hexadecimal digits. */
static int sreg_hex_bytes(const char *hex, uint8_t *out, size_t bytes) {
    if (!hex || strlen(hex) != 2u * bytes) return 0;
    for (size_t b = 0; b < bytes; b++) {
        int hi = sreg_hex_nibble(hex[2u * b]), lo = sreg_hex_nibble(hex[2u * b + 1u]);
        if (hi < 0 || lo < 0) return 0;
        out[b] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

uint32_t sreg_persist(const char *caller);

/* Parses SR_HOST_TIME_ZONE, "<minutes from UTC>,<0|1>" (the standard offset and whether daylight
 * saving is on now), as the launcher writes it from the host's local time zone (nk_launch.c).
 * Returns 0 for anything else: a zone must be a whole quarter hour within UTC-12:00..UTC+14:00. */
static int sreg_parse_host_time_zone(const char *text, int32_t *zone_out, uint32_t *daylight_out) {
    char *end = NULL;
    long zone;
    if (!text || !*text) return 0;
    errno = 0;
    zone = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != ',' || (end[1] != '0' && end[1] != '1') || end[2] != '\0')
        return 0;
    if (zone < -720 || zone > 840 || zone % 15 != 0) return 0;
    *zone_out = (int32_t)zone;
    *daylight_out = end[1] == '1' ? 1u : 0u;
    return 1;
}

/* First run (#1382): with no per-user overlay yet, the console's time zone and daylight-saving
 * setting start from the host's local time zone instead of the oracle console's measured -300 and
 * SAVING, and are saved to the overlay at once, so they are retained from then on the way a
 * console keeps its setting: a later change on the host does not move them, and a game or the
 * user can still change them through sceReg. Without SR_HOST_TIME_ZONE (a bare runtime, the
 * selftests) or with a malformed value, the measured seed stays. */
static void sreg_seed_host_time_zone(void) {
    char *env = NULL;
    int present = 0;
    int32_t zone;
    uint32_t daylight;
    int zone_node, daylight_node;
    if (!sr_utf8_env_alloc(L"SR_HOST_TIME_ZONE", &env, &present) || !present) {
        free(env);
        return;
    }
    if (!sreg_parse_host_time_zone(env, &zone, &daylight)) {
        fprintf(stderr, "system profile: ignoring SR_HOST_TIME_ZONE \"%.32s\" (want <minutes>,<0|1>); "
                        "the time zone keeps its seed\n", env ? env : "");
        free(env);
        return;
    }
    free(env);
    zone_node = sreg_find_path("/CONFIG/DATE/time_zone_offset");
    daylight_node = sreg_find_path("/CONFIG/DATE/summer_time");
    if (zone_node < 0 || daylight_node < 0) return;
    sreg_put_le32(s_sreg_nodes[zone_node].value, (uint32_t)zone);
    sreg_put_le32(s_sreg_nodes[daylight_node].value, daylight);
    s_sreg_dirty = 1;
    fprintf(stderr, "system profile: time zone %+d min from UTC, daylight saving %s, from the host's "
                    "local time zone on first run\n", (int)zone, daylight ? "on" : "off");
    (void)sreg_persist("system profile");
}

/* Load the defaults and the per-user overlay once per process. */
void sreg_ensure_loaded(void) {
    char *path, *text = NULL;
    char why[192];
    size_t len = 0;
    int got;
    if (s_sreg_loaded) return;
    s_sreg_loaded = 1;
    s_sreg_corrupt_on_disk = 0;
    memset(&s_console, 0, sizeof(s_console));
    sreg_clear_nodes();
    sreg_build_defaults();
    path = sreg_overlay_path();
    if (!path) return; /* the first flush that needs a file reports it */
    why[0] = '\0';
    got = sreg_read_file(path, &text, &len, why, sizeof(why));
    if (got > 0) {
        char parse_error[128];
        NkJsonNode *doc = nk_json_parse(text, len, parse_error, sizeof(parse_error));
        if (!doc) {
            snprintf(why, sizeof(why), "it is not valid JSON (%s)", parse_error);
            got = -1;
        } else {
            if (!sreg_overlay_apply(doc, why, sizeof(why))) got = -1;
            nk_json_free(doc);
        }
    }
    if (got < 0) {
        fprintf(stderr, "sceReg: ignoring the per-user registry overlay %s: %s; serving the modeled "
                        "defaults (the next flush moves it to %s.corrupt)\n", path, why, path);
        sreg_clear_nodes();
        sreg_build_defaults();
        memset(&s_console, 0, sizeof(s_console));
        s_sreg_corrupt_on_disk = 1;
    }
    s_sreg_dirty = 0;
    free(text);
    free(path);
    if (got == 0) sreg_seed_host_time_zone();
}

typedef struct {
    char *data;
    size_t len, cap;
    int failed;
} SregText;

static void sreg_text_add(SregText *t, const char *s, size_t n) {
    if (t->failed) return;
    if (t->len + n + 1u > t->cap) {
        size_t cap = t->cap ? t->cap : 1024u;
        char *grown;
        while (cap < t->len + n + 1u) cap *= 2u;
        grown = (char *)realloc(t->data, cap);
        if (!grown) {
            t->failed = 1;
            return;
        }
        t->data = grown;
        t->cap = cap;
    }
    memcpy(t->data + t->len, s, n);
    t->len += n;
    t->data[t->len] = '\0';
}

static void sreg_text_str(SregText *t, const char *s) { sreg_text_add(t, s, strlen(s)); }

/* Names and paths are printable ASCII (sreg_name_ok), so only '"' and '\\' need escaping. */
static void sreg_text_json_string(SregText *t, const char *s) {
    sreg_text_add(t, "\"", 1u);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') sreg_text_add(t, "\\", 1u);
        sreg_text_add(t, s, 1u);
    }
    sreg_text_add(t, "\"", 1u);
}

/* The overlay is every difference from the defaults: the categories a game created, the keys it
 * created, and the default keys whose value it changed, in creation order. */
static void sreg_overlay_render(SregText *t) {
    static int order[SREG_MAX_NODES];
    uint32_t count = sreg_ordered(SREG_ALL_NODES, order);
    char path[SREG_PATH_BYTES];
    int first = 1;
    sreg_text_str(t, "{\n  \"schema\": \"" SREG_OVERLAY_SCHEMA "\",\n  \"schema_version\": 1,\n"
                     "  \"categories\": [");
    for (uint32_t i = 0; i < count; i++) {
        const SregNode *n = &s_sreg_nodes[order[i]];
        if (n->type != SREG_TYPE_DIR || (n->flags & SREG_NODE_DEFAULT)) continue;
        if (!sreg_node_path(order[i], path, sizeof(path))) {
            t->failed = 1;
            return;
        }
        sreg_text_str(t, first ? "\n    " : ",\n    ");
        sreg_text_json_string(t, path);
        first = 0;
    }
    sreg_text_str(t, "\n  ],\n  \"keys\": [");
    first = 1;
    for (uint32_t i = 0; i < count; i++) {
        const SregNode *n = &s_sreg_nodes[order[i]];
        static const char digits[] = "0123456789abcdef";
        if (n->type == SREG_TYPE_DIR || (n->flags & SREG_NODE_READONLY) || sreg_is_default_value(n))
            continue;
        if (!sreg_node_path(n->parent, path, sizeof(path))) {
            t->failed = 1;
            return;
        }
        sreg_text_str(t, first ? "\n    {\"category\": " : ",\n    {\"category\": ");
        sreg_text_json_string(t, path);
        sreg_text_str(t, ", \"name\": ");
        sreg_text_json_string(t, n->name);
        sreg_text_str(t, ", \"type\": \"");
        sreg_text_str(t, sreg_type_name(n->type));
        sreg_text_str(t, "\", \"value_hex\": \"");
        for (uint32_t b = 0; b < n->size; b++) {
            char pair[2] = { digits[n->value[b] >> 4], digits[n->value[b] & 0x0fu] };
            sreg_text_add(t, pair, 2u);
        }
        sreg_text_str(t, "\"}");
        first = 0;
    }
    sreg_text_str(t, "\n  ]");
    if (s_console.have_identity) {
        static const char digits[] = "0123456789abcdef";
        sreg_text_str(t, ",\n  \"console\": {\"wlan_mac_hex\": \"");
        for (size_t b = 0; b < sizeof(s_console.mac); b++) {
            char pair[2] = { digits[s_console.mac[b] >> 4], digits[s_console.mac[b] & 0x0fu] };
            sreg_text_add(t, pair, 2u);
        }
        sreg_text_str(t, "\", \"open_psid_hex\": \"");
        for (size_t b = 0; b < sizeof(s_console.psid); b++) {
            char pair[2] = { digits[s_console.psid[b] >> 4], digits[s_console.psid[b] & 0x0fu] };
            sreg_text_add(t, pair, 2u);
        }
        sreg_text_str(t, "\"");
        if (s_console.wlan_switch_set)
            sreg_text_str(t, s_console.wlan_switch ? ", \"wlan_switch\": 1" : ", \"wlan_switch\": 0");
        sreg_text_str(t, "}");
    }
    sreg_text_str(t, "\n}\n");
}

/* Replace path with data atomically: write and sync <path>.tmp, then rename it over path. The
 * parent directory is created owner-private, since the overlay can hold the user's settings. */
static int sreg_write_atomic(const char *path, const char *data, size_t len) {
    size_t plen = strlen(path);
    char *tmp = (char *)malloc(plen + 5u), *dir = (char *)malloc(plen + 1u);
    char *cut;
    FILE *f;
    int ok;
    if (!tmp || !dir) {
        free(tmp);
        free(dir);
        return 0;
    }
    snprintf(tmp, plen + 5u, "%s.tmp", path);
    memcpy(dir, path, plen + 1u);
    cut = strrchr(dir, '/');
    {
        char *back = strrchr(dir, '\\');
        if (back && (!cut || back > cut)) cut = back;
    }
    if (cut && cut != dir) {
        *cut = '\0';
        (void)nk_platform_mkdir_p_private(dir);
    }
    free(dir);
    f = nk_platform_fopen_private(tmp, "wb");
    ok = f && fwrite(data, 1u, len, f) == len && fflush(f) == 0;
    if (f) {
        ok = sreg_sync_file(f) && ok;
        if (fclose(f) != 0) ok = 0;
    }
    if (ok && nk_rename_utf8(tmp, path) != 0) ok = 0;
    if (!ok) (void)nk_remove_utf8(tmp);
    free(tmp);
    return ok;
}

/* sceRegFlushRegistry / sceRegFlushCategory: persist the overlay when the live registry changed
 * since the last persist (a flush after reads writes nothing). */
uint32_t sreg_persist(const char *caller) {
    SregText text = { NULL, 0u, 0u, 0 };
    char *path;
    uint32_t rc = 0;
    if (!s_sreg_dirty) return 0;
    path = sreg_overlay_path();
    if (!path) {
        fprintf(stderr, "%s: no per-user data directory resolves for the registry overlay; set "
                        "SR_SYSTEM_REGISTRY (the change stays in memory only)\n", caller);
        return SREG_ERR_IO;
    }
    sreg_overlay_render(&text);
    if (text.failed) rc = SREG_ERR_IO;
    if (!rc && s_sreg_corrupt_on_disk && nk_platform_file_exists(path)) {
        size_t need = strlen(path) + sizeof(".corrupt");
        char *aside = (char *)malloc(need);
        if (aside) snprintf(aside, need, "%s.corrupt", path);
        if (!aside || nk_rename_utf8(path, aside) != 0) {
            fprintf(stderr, "%s: cannot move the unreadable registry overlay %s aside; nothing written\n",
                    caller, path);
            rc = SREG_ERR_IO;
        } else {
            fprintf(stderr, "%s: moved the unreadable registry overlay to %s\n", caller, aside);
        }
        free(aside);
    }
    if (!rc) {
        s_sreg_corrupt_on_disk = 0;
        if (!sreg_write_atomic(path, text.data, text.len)) {
            fprintf(stderr, "%s: cannot write the registry overlay %s (the change stays in memory only)\n",
                    caller, path);
            rc = SREG_ERR_IO;
        }
    }
    if (!rc) s_sreg_dirty = 0;
    free(text.data);
    free(path);
    return rc;
}

/* The first read of a placeholder (a default whose value is neither measured nor modeled) names
 * the key on stderr, once per key. Shared by every registry read path that serves a value. */
void sreg_note_placeholder_read(int node, const char *caller) {
    SregNode *n = &s_sreg_nodes[node];
    char path[SREG_PATH_BYTES];
    if ((n->flags & (SREG_NODE_PLACEHOLDER | SREG_NODE_LOGGED)) != SREG_NODE_PLACEHOLDER) return;
    n->flags |= SREG_NODE_LOGGED;
    if (!sreg_node_path(n->parent, path, sizeof(path))) path[0] = '\0';
    fprintf(stderr, "%s: %s/%s has no measured or modeled value; serving the neutral default "
                    "(%s)\n", caller, path, n->name,
            n->type == SREG_TYPE_INT ? "0" : n->type == SREG_TYPE_STR ? "an empty string" : "zero bytes");
}

/* sceUtilityGetSystemParamInt's value for a modeled id: the registry key it shares, else the
 * system-param table. */
uint32_t sreg_systemparam_int(uint32_t id) {
    sreg_ensure_loaded();
    for (uint32_t i = 0; i < SREG_MAX_NODES; i++) {
        const SregNode *n = &s_sreg_nodes[i];
        if (n->id && (n->flags & SREG_NODE_DEFAULT) && n->sysparam_id >= 0 &&
            (uint32_t)n->sysparam_id == id && n->type == SREG_TYPE_INT && n->size == 4u)
            return sreg_get_le32(n->value);
    }
    return systemparam_int_value(id);
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Drop the live registry and every handle, as a new process starts: the next sceReg call or
 * system-param read loads the defaults and the overlay file again. */
void sr_hle_test_sysreg_reset(void) {
    memset(&s_console, 0, sizeof(s_console));
    sreg_clear_nodes();
    memset(s_sreg_reg, 0, sizeof(s_sreg_reg));
    memset(s_sreg_cat, 0, sizeof(s_sreg_cat));
    s_sreg_loaded = 0;
    s_sreg_corrupt_on_disk = 0;
}
#endif

static void hle_record_unsupported_import_stop(uint32_t, const char *);

/* A handler that meets an argument shape this runtime does not model stops the run by name,
 * exactly as an import with no handler does: one UNSUPPORTED_IMPORT line, the player's
 * "unsupported-import-<name>" stop card, and exit 7. Returning an invented error code would
 * hand the game a result no PSP gives, and the game would carry on from it (#308). The
 * selftest harness counts the stop and returns SCE_KERNEL_ERROR_NOTIMP, so a test can
 * assert it. */
#ifdef SR_HLE_THREAD_SELFTEST
static uint32_t s_hle_unmodelled_stops;
uint32_t sr_hle_test_unmodelled_stops(void) { return s_hle_unmodelled_stops; }
#endif
__attribute__((format(printf, 2, 3)))
uint32_t hle_unmodelled_stop(const char *import_name, const char *fmt, ...) {
    char detail[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    fprintf(stderr, "UNSUPPORTED_IMPORT: %s %s; the run stops here (#308)\n", import_name, detail);
#ifdef SR_HLE_THREAD_SELFTEST
    s_hle_unmodelled_stops++;
    return HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
#else
    hle_record_unsupported_import_stop(sr_last_nid, import_name);
    sr_flight_unsupported_fatal(sr_last_nid, sched_current_uid(), 0u);
    sr_hit_hle = 1;
    if (sr_sched_on) {
        sr_trace_close();
        sr_compat_report();
        sr_perf_shutdown();
        fflush(stderr);
        _Exit(7);
    }
    longjmp(g_hle_jmp, 1);
#endif
}

/* The same named stop for handlers that live outside this file (hle_power.c). */
uint32_t sr_hle_unmodelled_stop(const char *import_name, const char *detail) {
    return hle_unmodelled_stop(import_name, "%s", detail);
}

int guest_cstr(uint32_t addr, char *out, int max);
uint32_t free_block(uint32_t uid) {
    for (int i = 0; i < s_nblocks; i++) {
        if (s_blocks[i].uid == uid && s_blocks[i].addr != 0) {
            /* Kernel metadata is host-side. Never scribble a synthetic free header
             * into memory returned to the guest. Free blocks are tracked host-side
             * and adjacent blocks are coalesced. */
            uint32_t addr = s_blocks[i].addr;
            uint32_t slot_sz = s_blocks[i].slot_size;
            int high = s_blocks[i].high;
            int part5 = s_blocks[i].part5;
            s_blocks[i].uid = 0;                  /* slot now free */
            s_blocks[i].addr = 0;
            s_blocks[i].size = 0;
            s_blocks[i].slot_size = 0;
            s_blocks[i].prev = 0;
            s_blocks[i].next = 0;
            s_blocks[i].high = 0;
            s_blocks[i].part5 = 0;
            /* A partition-5 block is only its s_blocks entry (volatile_part_alloc). */
            if (part5) { /* nothing else to return */ }
            else if (high) sched_partition_release_high(addr, slot_sz);
            else partition_free_block(addr, slot_sz);
            if (getenv("SR_ALLOC_TRACE")) {
                fprintf(stderr, "FreePartitionMemory: uid=0x%x addr=0x%08x marked free\n", uid, addr);
            }
            return 0;
        }
    }
    /* No live block has this UID. PSPAutotests tests/sysmem/partition.expected at 2ed7c9c5,
     * recorded on a PSP: freeing an already-freed UID, UID 0 or 0xDEADBEEF returns
     * SCE_KERNEL_ERROR_UNKNOWN_UID (0x800200CB) (#341). */
    if (getenv("SR_ALLOC_TRACE")) fprintf(stderr, "FreePartitionMemory: uid=0x%x not found\n", uid);
    return HLE_KERNEL_ERROR_UNKNOWN_UID;
}

FplPool s_fpls[FPL_MAX];

FplPool *fpl_lookup(uint32_t uid) {
    uint32_t idx = uid - 0x500u;
    return idx < FPL_MAX && s_fpls[idx].used ? &s_fpls[idx] : NULL;
}
VplPool s_vpls[VPL_MAX];

VplPool *vpl_lookup(uint32_t uid) {
    uint32_t i = uid - 0x600u;
    return i < VPL_MAX && s_vpls[i].used ? &s_vpls[i] : NULL;
}

/* Thread error codes from sched.c. */
#define SCE_KERNEL_ERROR_ILLEGAL_THID       0x80020197u
#define SCE_KERNEL_ERROR_UNKNOWN_THID       0x80020198u
uint32_t h_WaitThreadEnd_impl(CpuState *s, int is_cb) {
    uint32_t uid = A0, toptr = A1;
    uint32_t self = sched_current_uid();
    if (uid == 0 || uid == self) return 0x80020197u;
    uint32_t status = sched_thread_exit_status(uid);
    /* ILLEGAL_THID wins; reject a genuine wait before guest callbacks (L208/L217). */
    if (status == 0x800201A4u && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    if (status != 0x800201A4u) {
        /* Immediate CB joins still dispatch callbacks and leave timeout untouched. */
        if (is_cb && sr_thread_has_pending_callbacks(self)) {
            sr_thread_dispatch_callbacks();
            status = sched_thread_exit_status(uid);
        }
        return status;
    }

    uint64_t deadline = 0u;
    if (toptr) {
        sched_vtime_refresh();
        deadline = sched_vtime_deadline_after(MEM_R32(toptr));
    }
    /* Register before callbacks: even an initial callback can nest a join and
     * allow this target to exit/delete before this invocation first blocks. */
    SrWaitHandle invocation = sched_wait_begin_join(uid, deadline, is_cb);
    if (!invocation) return 0x80020190u;
    for (;;) {
        if (sched_wait_take_result(invocation, &status)) break;
        if (is_cb && sr_thread_has_pending_callbacks(self)) {
            sched_wait_callback(invocation, 1);
            sr_thread_dispatch_callbacks();
            sched_wait_callback(invocation, 0);
            if (sched_wait_take_result(invocation, &status)) break;
        }
        status = sched_thread_exit_status(uid);
        if (status != 0x800201A4u) break;
        uint32_t remaining = 0u;
        if (toptr) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= deadline) { status = SCE_KERNEL_ERROR_WAIT_TIMEOUT; break; }
            remaining = (uint32_t)(deadline - now);
        }
        int expired = sched_wait_block(invocation, remaining, toptr != 0u);
        if (sched_wait_take_result(invocation, &status)) break;
        if (expired) { status = SCE_KERNEL_ERROR_WAIT_TIMEOUT; break; }
    }
    if (toptr) {
        sched_vtime_refresh();
        uint64_t now = sched_vtime_us();
        MEM_W32(toptr, now < deadline ? (uint32_t)(deadline - now) : 0u);
    }
    (void)sched_wait_finish(invocation);
    return status;
}

/* Forward declaration for nested-frame guest execution. */
uint32_t ge_call_guest_rv(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);
uint32_t ge_call_guest_gp_rv(CpuState *s, uint32_t fn, uint32_t gp,
                                    uint32_t a0, uint32_t a1, uint32_t a2);
LoadedModule s_loaded_modules[HLE_MAX_LOADED_MODULES];
int s_nloaded_modules = 0;
uint32_t s_deferred_self_stop_uid;
uint32_t s_deferred_self_stop_thread_uid;
static void hle_module_thread_exit(uint32_t uid);
uint32_t s_loadexec_boot_uid;

LoadedModule *find_loaded_module(uint32_t uid) {
    for (int i = 0; i < s_nloaded_modules; i++) {
        if (s_loaded_modules[i].in_use && s_loaded_modules[i].uid == uid &&
            s_loaded_modules[i].state != MODULE_STATE_UNLOADED) {
            return &s_loaded_modules[i];
        }
    }
    return NULL;
}

/* Reclaim the record of a module that has been unloaded. The uid stops resolving, so a
 * later start/stop/unload of it fails closed on SCE_ERROR_MODULE_BAD_ID exactly as the
 * kernel rejects an id it no longer knows, and the fixed table does not fill up with
 * the tombstones of modules that are long gone (#280). */
void retire_loaded_module(LoadedModule *mod) {
    for (int i = 0; i < s_nloaded_modules; i++) {
        if (&s_loaded_modules[i] != mod || !mod->in_use) continue;
        for (int j = i + 1; j < s_nloaded_modules; j++) s_loaded_modules[j - 1] = s_loaded_modules[j];
        s_nloaded_modules--;
        memset(&s_loaded_modules[s_nloaded_modules], 0, sizeof(s_loaded_modules[0]));
        return;
    }
}
#ifdef SR_HLE_THREAD_SELFTEST
void sr_hle_test_unregister_late_imports(uint32_t lo, uint32_t hi) {
    sr_hle_unregister_late_imports_in_range(lo, hi);
}
#endif
PrxImage s_prx_images[HLE_MAX_LOADED_MODULES];
unsigned s_prx_image_count;
atomic_int s_prx_started_count;

/* The guest address a started module exports for `nid`, or 0. Importers are linked to a
 * module's exports only after that module has been initialised, as the kernel does. */
static uint32_t started_module_export(uint32_t nid) {
    if (atomic_load_explicit(&s_prx_started_count, memory_order_acquire) == 0) return 0;
    uint32_t t = sr_hle_resolve_late_import(nid);
    if (!t || t == SR_HLE_LATE_BUILTIN) return 0;
    for (unsigned i = 0; i < s_prx_image_count; i++)
        if (s_prx_images[i].started && t >= s_prx_images[i].base && t < s_prx_images[i].end) return t;
    return 0;
}

/* Retire one image: drop the export authority the kernel would unlink at unload, clear the
 * per-image started flag, release exactly the reservation this image owns, and reclaim the
 * table slot. Only the image's own [base,end) range and its own allocation are touched, so
 * a sibling module loaded elsewhere keeps its exports and its reservation. */
void unload_prx_image(uint32_t base) {
    for (unsigned i = 0; i < s_prx_image_count; i++) {
        if (s_prx_images[i].base != base) continue;
        PrxImage img = s_prx_images[i];
        if (img.started) {
            img.started = 0;
            atomic_fetch_sub_explicit(&s_prx_started_count, 1, memory_order_release);
        }
        /* Unbinding first retires the translation's dispatch and fetch authority and
         * its stale-code records, so nothing placed here later can reach this module's
         * bodies; then the exports and the memory go. */
        if (img.code) sr_module_code_unbind(img.code);
        sr_hle_unregister_late_imports_in_range(img.base, img.end);
        if (img.alloc_uid != 0u && img.alloc_uid != 0xFFFFFFFFu) free_block(img.alloc_uid);
        for (unsigned j = i + 1; j < s_prx_image_count; j++) s_prx_images[j - 1] = s_prx_images[j];
        s_prx_image_count--;
        memset(&s_prx_images[s_prx_image_count], 0, sizeof(s_prx_images[0]));
        return;
    }
}
uint32_t s_psmfplayer_lib_version;
#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_psmfplayer_lib_version(void) { return s_psmfplayer_lib_version; }
#endif
#ifdef SR_MPEG_MEDIA_SELFTEST
void sr_hle_test_set_psmfplayer_lib_version(uint32_t version) { s_psmfplayer_lib_version = version; }
#endif

uint32_t h_ok(CpuState *s) { (void)s; return 0; }

/* Registration refusals are intercepted by sr_syscall so their per-NID error
 * and diagnostic stay attached to the HLE table entry. This body is a defensive
 * fallback for any future path that calls the marker handler directly. */
static uint32_t h_ControlledUnsupported(CpuState *s) {
    (void)s;
    return 0x80010086u; /* SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED */
}
/* scePsmfPlayer model: the guest drives the real player, the host services the media
 * boundary underneath it.  The chain is
 *
 *   guest ISO path -> bounded PSMF producer (aud-delimited pictures, ATRAC frames)
 *                  -> per-track compressed-AU queues
 *                  -> H.264 backend / ATRAC3+ bridge
 *                  -> guest display buffer / PCM block
 *
 * The getters return success only when a decoder actually produced output: a compressed
 * access unit is never misreported as a decoded PSP frame or PCM block, a picture whose
 * PES packet carried no timestamp is extrapolated from the codec frame step (the reference
 * player does the same), and a missing decoder leaves the movie visibly short rather than
 * fabricating frames. */
#define PSMF_STATUS_NONE 0u
#define PSMF_ERR_ALREADY_INIT 0x80618005u
#define PSMF_AV_WINDOW_US 5000000ll          /* about 300 vblanks, like AUDIOSTAT_LEAD */
#define PSMF_AV_TIME_LIMIT_US (1ll << 40)    /* keeps every x9 sum far inside int64 */
#define PSMF_AV_PTS_LIMIT ((int64_t)1 << 48)
#define PSMF_AV_DISPLAY_ROWS 272u
#define PSMF_AV_LAT_LIMIT_US ((int64_t)1 << 32)

static struct {
    PsmfAvStream s[2];
    uint64_t seq;
    int64_t window_us, win_start_us;
    int win_started, exit_hooked;
    unsigned windows, paired;
    int64_t av_first, av_last, av_min, av_max, av_sum;   /* x9 */
} s_psmf_av;

#ifdef SR_MPEG_MEDIA_SELFTEST
static int s_psmf_av_test_enable = -1;
int s_psmf_av_test_clock_set, s_psmf_av_test_queue_set;
static int64_t s_psmf_av_test_clock;
static char s_psmf_av_last_window[512], s_psmf_av_last_final[2048];
static unsigned s_psmf_av_window_lines, s_psmf_av_final_lines;
#endif

static int psmf_av_on(void) {
#ifdef SR_MPEG_MEDIA_SELFTEST
    if (s_psmf_av_test_enable >= 0) return s_psmf_av_test_enable;
#endif
    static int v = -1;
    if (v < 0) v = (getenv("SR_MPEGAV") || getenv("SR_MPEGLOG")) ? 1 : 0;
    return v;
}

int64_t psmf_av_now_us(void) {
#ifdef SR_MPEG_MEDIA_SELFTEST
    if (s_psmf_av_test_clock_set) return s_psmf_av_test_clock;
#endif
    uint64_t t = sched_vtime_us();
    return t > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)t;
}

static void psmf_av_clear(void) {
    int64_t window = s_psmf_av.window_us;
    int hooked = s_psmf_av.exit_hooked;
    memset(&s_psmf_av, 0, sizeof(s_psmf_av));
    s_psmf_av.window_us = window;
    s_psmf_av.exit_hooked = hooked;
}

static void psmf_av_window_close(int64_t end_us) {
    PsmfAvStream *v = &s_psmf_av.s[PSMF_AV_VIDEO], *a = &s_psmf_av.s[PSMF_AV_AUDIO];
    if (!v->win_n && !a->win_n) return;
    int64_t vm = v->win_n ? v->win_sum_x9 / (int64_t)v->win_n : 0;
    int64_t am = a->win_n ? a->win_sum_x9 / (int64_t)a->win_n : 0;
    int64_t av = vm - am;
    int paired = v->win_n && a->win_n;
    s_psmf_av.windows++;
    if (paired) {
        if (!s_psmf_av.paired) s_psmf_av.av_first = s_psmf_av.av_min = s_psmf_av.av_max = av;
        s_psmf_av.av_last = av;
        if (av < s_psmf_av.av_min) s_psmf_av.av_min = av;
        if (av > s_psmf_av.av_max) s_psmf_av.av_max = av;
        s_psmf_av.av_sum += av;
        s_psmf_av.paired++;
    }
    char vo[32], ao[32], avs[32], line[512];
    psmf_av_fmt(vo, sizeof(vo), v->win_n != 0, vm);
    psmf_av_fmt(ao, sizeof(ao), a->win_n != 0, am);
    psmf_av_fmt(avs, sizeof(avs), paired, av);
    int n = snprintf(line, sizeof(line),
                     "PSMF_AV_PRESENT: win=%u end_us=%lld vn=%llu an=%llu voff_us=%s aoff_us=%s av_us=%s\n",
                     s_psmf_av.windows, (long long)end_us, (unsigned long long)v->win_n,
                     (unsigned long long)a->win_n, vo, ao, avs);
    if (n > 0 && (size_t)n < sizeof(line)) fputs(line, stderr);
#ifdef SR_MPEG_MEDIA_SELFTEST
    if (n > 0 && (size_t)n < sizeof(line)) {
        memcpy(s_psmf_av_last_window, line, (size_t)n + 1u);
        s_psmf_av_window_lines++;
    }
#endif
    v->win_sum_x9 = a->win_sum_x9 = 0;
    v->win_n = a->win_n = 0;
}

void psmf_av_window_tick(int64_t now_us) {
    int64_t span = s_psmf_av.window_us ? s_psmf_av.window_us : PSMF_AV_WINDOW_US;
    if (!s_psmf_av.win_started) {
        s_psmf_av.win_started = 1;
        s_psmf_av.win_start_us = now_us;
        return;
    }
    if (now_us - s_psmf_av.win_start_us >= span) {
        psmf_av_window_close(now_us);
        s_psmf_av.win_start_us = now_us;
    }
}

void psmf_av_final(const char *reason);
extern void (*sr_ge_texture_sample_hook)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                         uint32_t, uint32_t);
static void psmf_av_texture_sampled(uint32_t tex_addr, uint32_t tex_bufw, uint32_t tex_h,
                                    uint32_t tex_fmt, uint32_t fb_addr, uint32_t fb_stride,
                                    uint32_t fb_fmt);

PsmfAvEntry *psmf_av_delivered(int stream, uint32_t first, uint32_t bytes, int64_t pts, int pts_valid) {
    if (!psmf_av_on()) return NULL;
    if (!s_psmf_av.exit_hooked) { s_psmf_av.exit_hooked = 1; atexit(psmf_av_exit_report); }
    if (stream == PSMF_AV_VIDEO) sr_ge_texture_sample_hook = psmf_av_texture_sampled;
    PsmfAvStream *st = &s_psmf_av.s[stream];
    st->delivered++;
    uint32_t lo, hi;
    if (!psmf_av_range(first, bytes, &lo, &hi)) { st->nospan++; return NULL; }
    PsmfAvEntry *slot = NULL, *oldest = NULL;
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (!e->live) { if (!slot) slot = e; continue; }
        if (!e->composited && e->lo < hi && lo < e->hi) {
            if (e->content) {
                e->lo = e->hi = 0;   /* overwritten where it was written: followed by its bytes only */
            } else {
                e->live = 0; st->superseded++; if (!slot) slot = e; continue;
            }
        }
        if (!oldest || e->seq < oldest->seq) oldest = e;
    }
    if (!slot) { oldest->live = 0; st->evicted++; slot = oldest; }
    slot->seq = ++s_psmf_av.seq;
    slot->lo = lo;
    slot->hi = hi;
    slot->pts = pts;
    slot->pts_valid = pts_valid;
    slot->delivered_us = psmf_av_now_us();
    slot->live = 1;
    slot->composited = 0;
    slot->fb_lo = slot->fb_hi = 0;
    slot->content = 0;
    slot->content_bytes = 0;
    return slot;
}

/* A textured draw (GE_PRIM) samples [tex_addr, +level-0 bytes) into the render target at
 * fb_addr (fb_stride pixels, fb_fmt): a waiting picture it overlaps is composited there. */
static void psmf_av_texture_sampled(uint32_t tex_addr, uint32_t tex_bufw, uint32_t tex_h,
                                    uint32_t tex_fmt, uint32_t fb_addr, uint32_t fb_stride,
                                    uint32_t fb_fmt) {
    if (!psmf_av_on()) return;
    PsmfAvStream *st = &s_psmf_av.s[PSMF_AV_VIDEO];
    uint32_t tlo, thi, row, fbytes, flo, fhi;
    uint32_t tbytes = psmf_av_texture_bytes(tex_bufw, tex_h, tex_fmt);
    if (!tbytes || !psmf_av_range(tex_addr, tbytes, &tlo, &thi) || fb_fmt > 3u ||
        !sr_size_mul_ok(fb_stride, fb_fmt == 3u ? 4u : 2u, &row) ||
        !sr_size_mul_ok(row, PSMF_AV_DISPLAY_ROWS, &fbytes) ||
        !psmf_av_range(fb_addr, fbytes, &flo, &fhi))
        return;
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (!e->live || e->composited || !(e->lo < thi && tlo < e->hi)) continue;
        e->composited = 1;
        e->fb_lo = flo;
        e->fb_hi = fhi;
        st->composited++;
    }
}

/* One delivery meets its presentation/play time.  play_ok = 0 means the time itself was not
 * measured (no host audio queue). */
void psmf_av_match(PsmfAvStream *st, PsmfAvEntry *e, int64_t play_us, int play_ok) {
    e->live = 0;
    if (!e->pts_valid || !play_ok || e->pts < 0 || e->pts >= PSMF_AV_PTS_LIMIT ||
        play_us < 0 || play_us >= PSMF_AV_TIME_LIMIT_US) {
        st->unmeasured++;
        return;
    }
    st->matched++;
    int64_t x9 = play_us * 9 - e->pts * 100;
    st->win_sum_x9 += x9;
    st->win_n++;
    int64_t lat = play_us - e->delivered_us;
    if (lat < 0) lat = 0;
    if (lat >= PSMF_AV_LAT_LIMIT_US) lat = PSMF_AV_LAT_LIMIT_US - 1;
    st->lat_sum_us += (uint64_t)lat;
    if (lat > st->lat_max_us) st->lat_max_us = lat;
}

/* A framebuffer became the visible one (sceDisplaySetFrameBuf sync=0, or the VBLANK latch). */
void psmf_av_video_presented(uint32_t addr, int32_t stride, int32_t fmt) {
    if (!psmf_av_on()) return;
    PsmfAvStream *st = &s_psmf_av.s[PSMF_AV_VIDEO];
    st->boundary_calls++;
    uint32_t row, bytes, lo, hi;
    if (!addr || stride <= 0 || fmt < 0 || fmt > 3 ||
        !sr_size_mul_ok((uint32_t)stride, fmt == 3 ? 4u : 2u, &row) ||
        !sr_size_mul_ok(row, PSMF_AV_DISPLAY_ROWS, &bytes) ||
        !psmf_av_range(addr, bytes, &lo, &hi))
        return;
    int hit = 0;
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (!e->live) continue;
        int own = e->lo < hi && lo < e->hi;
        int drawn = e->composited && e->fb_lo < hi && lo < e->fb_hi;
        if (!own && !drawn) continue;
        int64_t now = psmf_av_now_us();
        if (!hit) psmf_av_window_tick(now);
        hit = 1;
        if (!own) st->composited_matched++;
        psmf_av_match(st, e, now, 1);
    }
    if (hit) st->boundary_hits++;
}

/* An audio buffer reaches an sceAudioOutput* call.  rate_hz is the guest-side rate used to
 * place a block that begins part-way into the submitted buffer. */
void psmf_av_audio_submit(int ch, uint32_t buf, uint32_t bytes, uint32_t bytes_per_frame,
                                 uint32_t rate_hz) {
    if (!psmf_av_on()) return;
    PsmfAvStream *st = &s_psmf_av.s[PSMF_AV_AUDIO];
    st->boundary_calls++;
    uint32_t lo, hi;
    if (!bytes_per_frame || !rate_hz || !psmf_av_range(buf, bytes, &lo, &hi)) return;
    int any = 0;
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (e->live && e->lo < hi && lo < e->hi) { any = 1; break; }
    }
    if (!any) {
        psmf_av_audio_by_content(st, ch, buf, bytes, bytes_per_frame, rate_hz);
        return;
    }
    int queued = psmf_av_queue_frames(ch);          /* lead ahead of this buffer, read before the push */
    int64_t now = psmf_av_now_us();
    int64_t lead_us = queued >= 0 ? (int64_t)queued * 1000000 / 44100 : 0;
    psmf_av_window_tick(now);
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (!e->live || !(e->lo < hi && lo < e->hi)) continue;
        int64_t frames = ((int64_t)e->lo - (int64_t)lo) / (int64_t)bytes_per_frame;
        int64_t play = now + lead_us + frames * 1000000 / (int64_t)rate_hz;
        psmf_av_match(st, e, play, queued >= 0);
    }
    st->boundary_hits++;
}

void psmf_av_final(const char *reason) {
    if (!psmf_av_on()) return;
    PsmfAvStream *v = &s_psmf_av.s[PSMF_AV_VIDEO], *a = &s_psmf_av.s[PSMF_AV_AUDIO];
    if (!v->delivered && !a->delivered) return;
    psmf_av_window_close(psmf_av_now_us());
    uint64_t pend[2] = {0, 0};
    for (int s = 0; s < 2; s++)
        for (unsigned i = 0; i < PSMF_AV_PENDING; i++)
            if (s_psmf_av.s[s].pend[i].live) pend[s]++;
    char vlm[32], vlx[32], alm[32], alx[32], mn[32], mx[32], mean[32], first[32], last[32], drift[32];
    psmf_av_fmt(vlm, sizeof(vlm), v->matched != 0, v->matched ? (int64_t)(v->lat_sum_us / v->matched) * 9 : 0);
    psmf_av_fmt(vlx, sizeof(vlx), v->matched != 0, v->lat_max_us * 9);
    psmf_av_fmt(alm, sizeof(alm), a->matched != 0, a->matched ? (int64_t)(a->lat_sum_us / a->matched) * 9 : 0);
    psmf_av_fmt(alx, sizeof(alx), a->matched != 0, a->lat_max_us * 9);
    int have = s_psmf_av.paired != 0;
    psmf_av_fmt(mn, sizeof(mn), have, s_psmf_av.av_min);
    psmf_av_fmt(mx, sizeof(mx), have, s_psmf_av.av_max);
    psmf_av_fmt(mean, sizeof(mean), have, have ? s_psmf_av.av_sum / (int64_t)s_psmf_av.paired : 0);
    psmf_av_fmt(first, sizeof(first), have, s_psmf_av.av_first);
    psmf_av_fmt(last, sizeof(last), have, s_psmf_av.av_last);
    psmf_av_fmt(drift, sizeof(drift), s_psmf_av.paired >= 2, s_psmf_av.av_last - s_psmf_av.av_first);
    char line[2048];
    int n = snprintf(line, sizeof(line),
        "PSMF_AV_PRESENT_FINAL: reason=%s windows=%u paired=%u"
        " vdel=%llu vmatch=%llu vunmeas=%llu vsuper=%llu vevict=%llu vnospan=%llu vpend=%llu vbnd=%llu vhit=%llu"
        " vcomp=%llu vcompmatch=%llu"
        " adel=%llu amatch=%llu aunmeas=%llu asuper=%llu aevict=%llu anospan=%llu apend=%llu abnd=%llu ahit=%llu"
        " acontent=%llu"
        " vlat_mean_us=%s vlat_max_us=%s alat_mean_us=%s alat_max_us=%s"
        " av_min_us=%s av_max_us=%s av_mean_us=%s av_first_us=%s av_last_us=%s drift_us=%s\n",
        reason, s_psmf_av.windows, s_psmf_av.paired,
        (unsigned long long)v->delivered, (unsigned long long)v->matched, (unsigned long long)v->unmeasured,
        (unsigned long long)v->superseded, (unsigned long long)v->evicted, (unsigned long long)v->nospan,
        (unsigned long long)pend[0], (unsigned long long)v->boundary_calls, (unsigned long long)v->boundary_hits,
        (unsigned long long)v->composited, (unsigned long long)v->composited_matched,
        (unsigned long long)a->delivered, (unsigned long long)a->matched, (unsigned long long)a->unmeasured,
        (unsigned long long)a->superseded, (unsigned long long)a->evicted, (unsigned long long)a->nospan,
        (unsigned long long)pend[1], (unsigned long long)a->boundary_calls, (unsigned long long)a->boundary_hits,
        (unsigned long long)a->content_matched,
        vlm, vlx, alm, alx, mn, mx, mean, first, last, drift);
    if (n > 0 && (size_t)n < sizeof(line)) {
        fputs(line, stderr);
#ifdef SR_MPEG_MEDIA_SELFTEST
        memcpy(s_psmf_av_last_final, line, (size_t)n + 1u);
        s_psmf_av_final_lines++;
#endif
    }
    psmf_av_clear();
}

/* Entry points for the sceMpeg path in mpeg.c (a separate translation unit in production):
 * sceMpegAvcDecode/Csc pictures and sceMpegAtracDecode blocks.  A guest PSMF player copies each
 * sceMpegAtracDecode block into its own output buffer before the game plays it, so a block is
 * also followed by its bytes. */
int sr_psmf_av_active(void) { return psmf_av_on(); }
void sr_psmf_av_note_video_delivery(uint32_t first, uint32_t bytes, int64_t pts, int pts_valid) {
    (void)psmf_av_delivered(PSMF_AV_VIDEO, first, bytes, pts, pts_valid);
}
void sr_psmf_av_note_audio_delivery(uint32_t first, uint32_t bytes, int64_t pts, int pts_valid) {
    PsmfAvEntry *e = psmf_av_delivered(PSMF_AV_AUDIO, first, bytes, pts, pts_valid);
    if (e) {
        e->content = psmf_av_content(first, bytes);
        e->content_bytes = e->content ? bytes : 0u;
    }
}

#ifdef SR_MPEG_MEDIA_SELFTEST
/* Test-only controls and read-back for the presentation-boundary measurement.  The clock and
 * the host queue lead are injected because the fixture has no running scheduler and no host
 * mixer; everything between the guest call and the recorded offset is production code. */
void sr_hle_test_psmf_av_reset(void) {
    memset(&s_psmf_av, 0, sizeof(s_psmf_av));
    s_psmf_av_test_enable = -1;
    s_psmf_av_test_clock_set = s_psmf_av_test_queue_set = 0;
    s_psmf_av_last_window[0] = s_psmf_av_last_final[0] = '\0';
    s_psmf_av_window_lines = s_psmf_av_final_lines = 0;
}
void sr_hle_test_psmf_av_enable(int on) { s_psmf_av_test_enable = on; }
void sr_hle_test_psmf_av_set_window_us(int64_t us) { s_psmf_av.window_us = us; }
void sr_hle_test_psmf_av_set_clock_us(int64_t us) { s_psmf_av_test_clock = us; s_psmf_av_test_clock_set = 1; }
void sr_hle_test_psmf_av_set_queue_frames(int frames) { s_psmf_av_test_queue = frames; s_psmf_av_test_queue_set = 1; }
const char *sr_hle_test_psmf_av_last_window(void) { return s_psmf_av_last_window; }
const char *sr_hle_test_psmf_av_last_final(void) { return s_psmf_av_last_final; }
unsigned sr_hle_test_psmf_av_window_lines(void) { return s_psmf_av_window_lines; }
unsigned sr_hle_test_psmf_av_final_lines(void) { return s_psmf_av_final_lines; }
/* Live counters before the final report clears them. */
int64_t sr_hle_test_psmf_av_counter(int audio, const char *key) {
    PsmfAvStream *st = &s_psmf_av.s[audio ? PSMF_AV_AUDIO : PSMF_AV_VIDEO];
    if (!strcmp(key, "delivered")) return (int64_t)st->delivered;
    if (!strcmp(key, "matched")) return (int64_t)st->matched;
    if (!strcmp(key, "unmeasured")) return (int64_t)st->unmeasured;
    if (!strcmp(key, "superseded")) return (int64_t)st->superseded;
    if (!strcmp(key, "evicted")) return (int64_t)st->evicted;
    if (!strcmp(key, "nospan")) return (int64_t)st->nospan;
    if (!strcmp(key, "content_matched")) return (int64_t)st->content_matched;
    if (!strcmp(key, "boundary")) return (int64_t)st->boundary_calls;
    if (!strcmp(key, "hits")) return (int64_t)st->boundary_hits;
    if (!strcmp(key, "composited")) return (int64_t)st->composited;
    if (!strcmp(key, "composited_matched")) return (int64_t)st->composited_matched;
    if (!strcmp(key, "pending")) {
        int64_t n = 0;
        for (unsigned i = 0; i < PSMF_AV_PENDING; i++) if (st->pend[i].live) n++;
        return n;
    }
    return -1;
}
void sr_hle_test_psmf_av_report(void) { psmf_av_final("test"); }
#endif
FILE *sr_fopen_utf8(const char *path, const wchar_t *mode);
extern CallbackEntry *s_callbacks;
extern size_t s_callbacks_len;
extern int sr_vblank_dispatch_registered(void);
extern int sr_callback_find_in_table(uint32_t uid);

/* sceKernelQueryModuleInfo(uid, info), from uOFW src/kd/modulemgr/queryModule.c (MIT):
 * interrupt context, then a 96-byte user buffer, then (compiled SDK 2.80 or later) a size of
 * 64 (SceKernelModuleInfoV1) or 96 (SceKernelModuleInfo), then the module. The first 64 bytes
 * are always written; attribute (privilege bits cleared), version and name only for size 96.
 * The main module answers from its generated record, and a module the runtime placed itself
 * from the record taken when it was placed. Any other loaded module's record holds no segment
 * table, text/data/bss sizes, attribute, version or name, so it is refused by name rather than
 * answered with partial values; so is a main module built without a record. */
#define SCE_KERNEL_ERROR_CANNOT_GET_MODULE_INFO 0x8002014au

/* True when a live module record other than `mod` still depends on the same PRX image.
 * Two handles can name one base: a second load of an already-resident base reuses that
 * image instead of staging a second copy, and both records then hold entry addresses
 * into it. Retiring the image while either record is alive would free the range out
 * from under the other handle's module_start/module_stop. */
int another_record_references_image(const LoadedModule *mod) {
    for (int i = 0; i < s_nloaded_modules; i++) {
        const LoadedModule *other = &s_loaded_modules[i];
        if (other == mod || !other->in_use || other->state == MODULE_STATE_UNLOADED) continue;
        if (other->image_base == mod->image_base) return 1;
    }
    return 0;
}

static void hle_module_thread_exit(uint32_t uid) {
    if (!s_deferred_self_stop_uid || uid != s_deferred_self_stop_thread_uid) return;
    LoadedModule *mod = find_loaded_module(s_deferred_self_stop_uid);
    uint32_t modid = s_deferred_self_stop_uid;
    s_deferred_self_stop_uid = 0u;
    s_deferred_self_stop_thread_uid = 0u;
    if (!mod || mod->state != MODULE_STATE_STOPPED) return;
    if (mod->image_base && mod->image_end > mod->image_base &&
        sched_module_range_busy(mod->image_base, mod->image_end, 0u)) {
        fprintf(stderr, "PRX_SELFSTOP_UNLOAD_DEFERRED_SYNTHETIC: uid=0x%x still has a "
                        "ready/running image continuation; preserving image and exports\n",
                modid);
        return;
    }
    mod->state = MODULE_STATE_UNLOADED;
    if (mod->image_base && !another_record_references_image(mod))
        unload_prx_image(mod->image_base);
    retire_loaded_module(mod);
    fprintf(stderr, "PRX_SELFSTOP_UNLOADED_SYNTHETIC: uid=0x%x caller thread exited; "
                    "retired the stopped module\n", modid);
}

int loadexec_ms0_path(const char *path) {
    static const char prefix[] = "ms0:/";
    if (!path) return 0;
    for (size_t i = 0; i < sizeof(prefix) - 1u; i++) {
        char c = path[i];
        if (!c) return 0;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != prefix[i]) return 0;
    }
    return 1;
}

/* Player quit request (#1392). PSPSDK documents the exit callback as the game's notice that
 * the user asked to quit from the HOME menu: the callback does its cleanup and calls
 * sceKernelExitGame itself. Measured under PSPLink, sceKernelExitGame does not run a
 * registered exit callback, so this request is the only place the callback is notified and
 * h_ExitGame stays terminal. The notification goes through sr_callback_notify, so the body
 * runs on the thread that created the callback at its next callback-servicing wait.
 *
 * Unmeasured on the stock-firmware HOME-menu path: the notify argument (0 here) and how long
 * the firmware waits. SR_EXIT_REQUEST_WINDOW_US is a host policy, not PSP timing: a guest
 * that has not exited when that much guest time has passed is ended by the player. */
#define SR_EXIT_REQUEST_WINDOW_US 5000000u
int s_exit_request_pending;
uint64_t s_exit_request_deadline_us;

/* Returns 1 when the guest's exit callback was notified (now or by an earlier request) and
 * its window is still open, so the player keeps running; 0 when the player exits now: no
 * valid callback is registered, the notification failed, or the window has passed. */
int sr_exit_request_quit(void) {
    if (s_exit_request_pending) return !sr_exit_request_expired();
    if (!s_exit_cb_uid || !sr_callback_is_valid(s_exit_cb_uid)) return 0;
    if (sr_callback_notify(s_exit_cb_uid, 0u) != 0u) return 0;
    s_exit_request_pending = 1;
    s_exit_request_deadline_us = sched_vtime_us() + SR_EXIT_REQUEST_WINDOW_US;
    fprintf(stderr, "GAMELOG: quit requested; notified exit callback uid=0x%x, waiting up to "
                    "%u us of guest time for sceKernelExitGame\n",
            s_exit_cb_uid, SR_EXIT_REQUEST_WINDOW_US);
    return 1;
}

/* 1 once a notified quit request's guest-time window has passed without the guest exiting. */
int sr_exit_request_expired(void) {
    return s_exit_request_pending && sched_vtime_us() >= s_exit_request_deadline_us;
}

int sr_savedata_pause_busy(void);   /* the savedata dialog is running (defined below) */
static int s_quit_stop_armed;
static uint64_t s_quit_stop_deadline_us;

/* The player's stop decision for a quit request, polled at scheduler boundaries (#1377). The
 * first call notifies the exit callback (sr_exit_request_quit) and opens one window of guest
 * time, SR_EXIT_REQUEST_WINDOW_US, whether or not a callback is registered. Returns 0 while the
 * run goes on: the guest's callback window is open, or a savedata dialog it started is still
 * running inside that window, so a quit does not cut off a save in progress (the pause
 * overlay's Quit is refused for the same reason). Returns 1 once the run should end; the window
 * bounds the wait, so a guest that never finishes cannot hold the player open. The stop lands
 * only between guest slices, never inside the sceUtilitySavedataUpdate call that writes the
 * save (sr_savedata_execute_sized runs within it), so no write is cut off halfway. */
int sr_exit_request_stop_due(void) {
    if (!s_quit_stop_armed) {
        s_quit_stop_armed = 1;
        s_quit_stop_deadline_us = sched_vtime_us() + SR_EXIT_REQUEST_WINDOW_US;
    }
    /* One window from the request: a callback the guest registers later is still notified,
     * but it cannot open a second window past this one. */
    if (sched_vtime_us() >= s_quit_stop_deadline_us) return 1;
    if (sr_exit_request_quit()) return 0;
    if (sched_vtime_us() < s_quit_stop_deadline_us && sr_savedata_pause_busy()) return 0;
    return 1;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* A fresh quit for each test: nothing in the runtime resets a quit request, which lasts for
 * the process (a LoadExec in the window does not give the next program a new one). */
void sr_hle_test_exit_request_reset(void) {
    s_exit_request_pending = 0;
    s_exit_request_deadline_us = 0u;
    s_quit_stop_armed = 0;
    s_quit_stop_deadline_us = 0u;
}
#endif
#ifdef _WIN32

/* The save-data helpers retain their host-neutral UTF-8 API, but obtain the
 * Windows environment value through the wide API first so non-ASCII paths do
 * not depend on the process code page. */
int sr_utf8_env_alloc(const wchar_t *name, char **value_out,
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

FILE *sr_fopen_utf8(const char *path, const wchar_t *mode) {
    wchar_t *wide = NULL;
    if (!mode || !sr_wide_path_alloc(path, &wide)) return NULL;
    FILE *f = _wfopen(wide, mode);
    free(wide);
    return f;
}
#else /* !_WIN32: the same host-path layer expressed with POSIX primitives ----- */

int sr_utf8_env_alloc(const wchar_t *name, char **value_out,
                             int *present_out) {
    if (!name || !value_out || !present_out) return 0;
    *value_out = NULL;
    *present_out = 0;
    char *narrow_name = NULL;
    if (!sr_wide_to_utf8_alloc(name, &narrow_name)) return 0;
    const char *value = getenv(narrow_name);
    free(narrow_name);
    if (!value) return 1;
    size_t n = strlen(value);
    if (n > (size_t)INT_MAX) return 0;
    char *copy = (char *)malloc(n + 1u);
    if (!copy) return 0;
    memcpy(copy, value, n + 1u);
    *value_out = copy;
    *present_out = 1;
    return 1;
}

FILE *sr_fopen_utf8(const char *path, const wchar_t *mode) {
    char *narrow = NULL;
    FILE *f = NULL;
    if (!path || !mode || !sr_narrow_mode_alloc(mode, &narrow)) return NULL;
    f = fopen(path, narrow);
    free(narrow);
    return f;
}
#endif /* _WIN32 */

void flash0_font_sources(Flash0Sources *out) {
    flash0_user_data_root(out->user_data_dir, sizeof(out->user_data_dir));

    wchar_t *configured = NULL;
    wchar_t *root = NULL;
    int configured_present = 0;
    if (!sr_wide_env_alloc(L"SR_FONTDIR", &configured, &configured_present)) {
        flash0_refuse_font_dir("could not be read");
    } else if (configured_present) {
        if (!configured[0] || !sr_wide_configured_root_wide_alloc(configured, &root))
            flash0_refuse_font_dir("is configured but is not a valid absolute path");
    } else {
        sr_wide_module_font_root(&root);
    }
    free(configured);
    char *utf8 = NULL;
    if (root && sr_wide_to_utf8_alloc(root, &utf8))
        flash0_copy_root(out->project_dir, sizeof(out->project_dir), utf8);
    else
        out->project_dir[0] = '\0';
    free(utf8);
    free(root);
}

/* HLE-to-guest call replay (defined later; used to invoke the game's allocFunc with the PSP
 * calling convention: allocFunc(userData=a0, size=a1) -> ptr). */
uint32_t ge_call_guest_rv(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);
#define SR_FONTLIB_MIN_SIZE  0x28u
uint32_t mpeg_query_mem_size(uint32_t outAddr);
#define ATRAC_ERROR_NO_LOOP_INFORMATION 0x80630021u
#define ATRAC_STATE_NO_DATA 1
int audio_stat_on(void);  /* SR_AUDIOSTAT gate; defined with the SAS aggregates */
#ifdef SR_HLE_THREAD_SELFTEST
/* Test builds only: the compiled SDK version libmp3 consults, set without a ThreadMan call. */
void sr_hle_test_set_compiled_sdk_version(uint32_t version) {
    (void)sr_sdkver_set(&g_sdk_version, version);
}
#endif

/* sceUtility dialogs (savedata/msg). The public contract is a status machine the game polls:
 * InitStart arms it, Update refreshes it, GetStatus reports where it is and ShutdownStart
 * removes it, and result is the common-header field at param+0x1c. Each transition belongs
 * to the call or event that drives it on a PSP (#1386), from the PSPAutotests hardware
 * captures (hrydgard/pspautotests e2d098fc, tests/utility/savedata/{initstatus,noupdate,
 * shutdownstatus}.expected and tests/utility/dialog/{status,priority}.expected):
 *  - INIT -> RUNNING is the dialog's own start-up. Every caller priority from 16 to 111 sees
 *    INIT on the GetStatus right after InitStart, and RUNNING follows with no Update call.
 *    Update does not move it: a message Update during INIT returns INVALID_STATUS, and
 *    savedata's (unmeasured) fails closed the same way. How long INIT lasts is unmeasured, so
 *    here start-up completes once a GetStatus has reported INIT.
 *  - RUNNING -> FINISHED needs Update: a savedata dialog polled for two seconds without one
 *    stays RUNNING. The first Update does the work; FINISHED comes after the work and two more
 *    Updates, a count no capture measures.
 *  - SHUTDOWN -> NONE is the shutdown's own completion. A message dialog shows SHUTDOWN to
 *    every caller priority measured; a savedata shutdown ends at priority 0x20, so a caller
 *    at 0x20 sees SHUTDOWN and a worse one sees NONE at once (h_DlgShutdown). When NONE
 *    follows a reported SHUTDOWN is unmeasured: here on the next GetStatus.
 * The OSK is a separate utility with its own state enum and is handled with the rest of it
 * below. */
int guest_cstr(uint32_t addr, char *out, int max);
static atomic_flag s_dialog_lock = ATOMIC_FLAG_INIT;
void dialog_lock(void) {
    while (atomic_flag_test_and_set_explicit(&s_dialog_lock, memory_order_acquire)) { }
}
void dialog_unlock(void) {
    atomic_flag_clear_explicit(&s_dialog_lock, memory_order_release);
}
/* The type that last took the slot. Written under dialog_lock, and only by guest syscalls and
 * the guest resets, so a handler on the guest thread may read it directly. */
int s_utility_slot = UTILITY_SLOT_NONE;

/* The guest wall clock and the console time zone, for save timestamps (#1387, savedata.c).
 * Both are the RTC's own, defined with it below: the RTC tick (microseconds since 0001-01-01,
 * anchored to the host clock once and advanced by guest time) and the console's registry time
 * zone plus daylight saving, never the host process time zone. */
uint64_t rtc_now_tick(void);
int64_t psp_profile_local_offset_minutes(int32_t *zone_out, uint32_t *daylight_out);
uint64_t sr_savedata_guest_rtc_tick(void) { return rtc_now_tick(); }
int32_t sr_savedata_console_offset_minutes(void) {
    return (int32_t)psp_profile_local_offset_minutes(NULL, NULL);
}

int s_dlg_status = 0, s_dlg_tick = 0;
int sr_savedata_pause_busy(void) {
    dialog_lock();
    const int busy = s_dlg_status >= 1 && s_dlg_status <= 3;
    dialog_unlock();
    return busy;
}

/* Forget every dialog and free the slot. A new generation drops a result whose work is still
 * running outside the lock. Caller holds dialog_lock. */
void utility_dialogs_reset_locked(void) {
    UtilityDialog *const dialogs[] = { &s_msg_dialog, &s_net_dialog, &s_gamedata_dialog,
                                       &s_sharing_dialog };
    s_dlg_status = 0;
    s_dlg_tick = 0;
    s_dlg_param = 0u;
    s_dlg_result = 0u;
    s_dlg_work_started = s_dlg_work_done = 0u;
    s_dlg_generation++;
    for (size_t i = 0; i < sizeof(dialogs) / sizeof(dialogs[0]); i++) {
        UtilityDialog *d = dialogs[i];
        d->param = 0u;
        d->result = 0u;
        d->generation++;
        d->status = 0;
        d->ticks = 0u;
        d->work_started = d->work_done = d->aborted = 0u;
    }
    s_utility_slot = UTILITY_SLOT_NONE;
}
int s_osk_status = OSK_NONE;
/* Test-only: free the utility dialog slot, forgetting the keyboard (abandoning any answer still
 * pending) and every other dialog, so the next case starts with no dialog ever opened. */
void sr_hle_test_osk_slot_reset(void) {
    osk_abandon_request();
    s_osk_status = OSK_NONE;
    s_osk_param = 0;
    s_osk_field = 0;
    dialog_lock();
    utility_dialogs_reset_locked();
    dialog_unlock();
}

/* The console profile's identity, generated and persisted on first use (see PspConsoleProfile).
 * Returns 0 only when the host has no random source, which leaves the caller nothing to report. */
int console_profile_identity(void) {
    sreg_ensure_loaded();
    if (s_console.have_identity) return 1;
    PspConsoleProfile next;
    memset(&next, 0, sizeof(next));
    if (!console_random_bytes(next.mac, sizeof(next.mac)) ||
        !console_random_bytes(next.psid, sizeof(next.psid)))
        return 0;
    next.mac[0] = (uint8_t)(next.mac[0] & 0xfcu);   /* unicast, universally administered */
    if (!console_identity_ok(next.mac, next.psid)) next.psid[15] = 1u;
    next.have_identity = 1;
    s_console = next;
    s_sreg_dirty = 1;
    /* A console never fails these calls, so a save that fails does not fail the call: the
     * identity stays this run's, and the line says it will not be the same next run. */
    if (sreg_persist("system profile") == 0)
        fprintf(stderr, "system profile: generated this installation's WLAN MAC address and "
                        "OpenPSID; kept in the registry overlay\n");
    else
        fprintf(stderr, "system profile: generated a WLAN MAC address and OpenPSID for this run "
                        "only: they could not be saved, so the next run reports different ones\n");
    return 1;
}

/* Generic PSP callback objects are kernel objects, not a 16-entry subsystem
 * table. Hardware creates at least 1024 of them. Keep an append-only host array:
 * deleted holes are never reused, so dispatch order remains callback creation order. */
CallbackEntry *s_callbacks;
size_t s_callbacks_len;

int sr_callback_find_in_table(uint32_t uid) {
    for (size_t i = 0; i < s_callbacks_len; i++)
        if (s_callbacks[i].used && s_callbacks[i].uid == uid) return (int)i;
    return -1;
}

int sr_callback_is_valid(uint32_t uid) {
    return sr_callback_find_in_table(uid) >= 0;
}
uint32_t s_memstick_callbacks[MEMSTICK_CALLBACK_MAX];
uint32_t s_memstick_callback_count;

int memstick_callback_remove_first(uint32_t uid) {
    for (uint32_t i = 0; i < s_memstick_callback_count; i++) {
        if (s_memstick_callbacks[i] != uid) continue;
        memmove(&s_memstick_callbacks[i], &s_memstick_callbacks[i + 1u],
                (s_memstick_callback_count - i - 1u) * sizeof(s_memstick_callbacks[0]));
        s_memstick_callbacks[--s_memstick_callback_count] = 0u;
        return 1;
    }
    return 0;
}

static void memstick_callback_forget(uint32_t uid) {
    while (memstick_callback_remove_first(uid)) { }
}

int sr_callback_table_unregister(uint32_t uid) {
    int idx = sr_callback_find_in_table(uid);
    if (idx < 0) return 0;
    memset(&s_callbacks[idx], 0, sizeof(s_callbacks[idx]));
    memstick_callback_forget(uid);
    return 1;
}

void sr_callback_unregister_owner(uint32_t thread_uid) {
    for (size_t i = 0; i < s_callbacks_len; i++) {
        if (s_callbacks[i].used && s_callbacks[i].owner_thread_uid == thread_uid) {
            memstick_callback_forget(s_callbacks[i].uid);
            memset(&s_callbacks[i], 0, sizeof(s_callbacks[i]));
        }
    }
    if (s_exit_cb_uid && !sr_callback_is_valid(s_exit_cb_uid))
        s_exit_cb_uid = 0;
}

extern void sr_mutex_release_thread(uint32_t thread_uid);
static void mbx_remove_thread_waiters(uint32_t thread_uid);
static void io_cwd_release_thread(uint32_t thread_uid);


void sr_hle_release_thread_resources(uint32_t thread_uid) {
    sr_mutex_release_thread(thread_uid);
    if (thread_uid) {
        io_cwd_release_thread(thread_uid);
        mbx_remove_thread_waiters(thread_uid);

        for (int i = 0; i < FPL_MAX; i++) {
            if (s_fpls[i].used) fpl_remove_waiter(&s_fpls[i], thread_uid);
        }
        for (int i = 0; i < VPL_MAX; i++) {
            if (s_vpls[i].used) vpl_remove_waiter(&s_vpls[i], thread_uid);
        }
        for (int i = 0; i < TLSPL_MAX; i++) {
            if (s_tlspls[i].used) tlspl_release_thread(&s_tlspls[i], thread_uid);
        }
    }
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

int sr_thread_dispatch_callback_uid(uint32_t thread_uid, CpuState *cpu,
                                           uint32_t selected_uid) {
    int idx = sr_callback_find_in_table(selected_uid);
    if (idx < 0) return 0;
    CallbackEntry *cb = &s_callbacks[idx];
    if (!cb->used || cb->owner_thread_uid != thread_uid || !cb->pending) return 0;

    uint32_t uid = cb->uid;
    uint32_t entry = cb->entry;
    uint32_t common_arg = cb->arg;
    uint32_t notify_arg = cb->notify_arg;
    uint32_t notify_count = cb->notify_count;

    /* The notification is consumed as the body starts, so a re-notification made by the
     * body forms a new pending event picked up by a later scan. */
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
    return 1;
}

int sr_thread_dispatch_callbacks(void) {
    uint32_t thread_uid = sched_current_uid();
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
        if (sr_thread_dispatch_callback_uid(thread_uid, cpu, selected_uid))
            total_dispatched++;
    }
    return total_dispatched;
}

/* Run guest callback code on the current fiber with a private AOT return base.
 * The interrupted call frames remain below the scope floor and cannot be
 * inspected or popped by callback returns. */
int hle_dispatch_guest_scoped(CpuState *s, uint32_t fn,
                                     const char *owner, uint32_t *fatal_target) {
    SrAotReturnBoundaryScope scope;
    sr_aot_return_boundary_scope_begin(&scope);
    dispatch(s, fn);
    const uint32_t flow_kind = s->flow_kind;
    const uint32_t flow_target = s->flow_target;
    const int scope_failed = sr_aot_return_boundary_scope_end(s, &scope, owner);
    if (scope_failed || flow_kind == SR_FLOW_FATAL || s->flow_kind == SR_FLOW_FATAL) {
        if (fatal_target)
            *fatal_target = s->flow_kind == SR_FLOW_FATAL ? s->flow_target : flow_target;
        return 1;
    }
    return 0;
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

uint32_t s_vcount_fwd;  /* mirror of s_vcount for clock/input timing (set in sr_display_advance_vcount) */
uint32_t sr_audio_vbl(void) { return s_vcount_fwd; }

VTimer s_vtimers[VTIMER_MAX];

VTimer *vtimer_lookup(uint32_t uid) {
    for (int i = 0; i < VTIMER_MAX; i++)
        if (s_vtimers[i].used && s_vtimers[i].uid == uid) return &s_vtimers[i];
    return NULL;
}
/* The shortest reschedule a handler return gets (PSP-HLE-INTR-001, PSP-3000 6.6.1). */
#define VTIMER_MIN_DELAY_US 100u

uint32_t s_vtimer_in_handler;   /* uid whose handler is running, else 0 */

/* Guest time at which a running timer with a handler reaches its schedule, else UINT64_MAX. */
static uint64_t vtimer_handler_deadline(const VTimer *t) {
    if (!t->used || !t->running || !t->handler || s_vtimer_in_handler == t->uid) return UINT64_MAX;
    if (t->schedule <= t->accum_us) return t->started_us;
    uint64_t left = t->schedule - t->accum_us;
    return left > UINT64_MAX - t->started_us ? UINT64_MAX : t->started_us + left;
}

static uint64_t vtimer_next_deadline_us(void) {
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < VTIMER_MAX; i++) {
        uint64_t d = vtimer_handler_deadline(&s_vtimers[i]);
        if (d < next) next = d;
    }
    return next;
}

/* Run one handler as a timer interrupt on a nested guest frame, with the two clocks it is
 * handed just above that frame. Returns its $v0. */
static uint32_t vtimer_call_handler(CpuState *s, const VTimer *t, uint64_t scheduled,
                                    uint64_t actual) {
    uint32_t frame_sp = 0;
    int frame = -1;
    if (!sr_nested_frame_acquire(sched_current_uid(), &frame_sp, &frame) || frame_sp < 0x40u ||
        !sr_guest_span_writable((frame_sp - 16u) & ~7u, 16u)) {
        fprintf(stderr, "VTIMER_HANDLER: fn=0x%08x uid=0x%x has no nested guest-call frame; "
                        "the run stops here (#308)\n", t->handler, t->uid);
        if (frame >= 0) (void)sr_nested_frame_release(frame);
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = t->handler;
        return 0u;
    }
    const uint32_t clocks = (frame_sp - 16u) & ~7u;
    MEM_W32(clocks, (uint32_t)scheduled);
    MEM_W32(clocks + 4u, (uint32_t)(scheduled >> 32));
    MEM_W32(clocks + 8u, (uint32_t)actual);
    MEM_W32(clocks + 12u, (uint32_t)(actual >> 32));
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    int32_t save_slice = atomic_load_explicit(&sr_timeslice, memory_order_relaxed);
    memset(s, 0, sizeof(CpuState));
    s->r[4] = t->uid;
    s->r[5] = clocks;
    s->r[6] = clocks + 8u;
    s->r[7] = t->common;
    s->r[28] = save.r[28];
    s->r[29] = clocks - 16u;
    s->r[31] = 0;
    s->vfpuCtrl[0] = 0xe4; s->vfpuCtrl[1] = 0xe4;
    s->pc = t->handler;
    atomic_store_explicit(&sr_timeslice, 20000, memory_order_relaxed);
    uint32_t fatal_target = 0u;
    int fatal = hle_dispatch_guest_scoped(s, t->handler, "VTimer handler", &fatal_target);
    uint32_t ret = s->r[2];
    memcpy(s, &save, sizeof(CpuState));
    if (fatal) {
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = fatal_target;
    }
    sr_cpu_link_clear(s);
    atomic_store_explicit(&sr_timeslice, save_slice, memory_order_relaxed);
    (void)sr_nested_frame_release(frame);
    return ret;
}

/* The scheduler's VTimer interrupt step: run the most overdue handler once. */
static void vtimer_service_one(void) {
    uint64_t now = sched_vtime_us();
    int best = -1;
    uint64_t best_deadline = UINT64_MAX;
    for (int i = 0; i < VTIMER_MAX; i++) {
        uint64_t d = vtimer_handler_deadline(&s_vtimers[i]);
        if (d <= now && d < best_deadline) { best = i; best_deadline = d; }
    }
    if (best < 0) return;
    VTimer *t = &s_vtimers[best];
    const uint32_t uid = t->uid, handler = t->handler;
    const uint64_t scheduled = t->schedule;
    s_vtimer_in_handler = uid;
    uint32_t delay = vtimer_call_handler(s_cpu, t, scheduled, vtimer_elapsed_at(t, now));
    s_vtimer_in_handler = 0u;
    t = vtimer_lookup(uid);
    if (!t || t->handler != handler || t->schedule != scheduled) return;
    /* A return below 100 us reschedules 100 us on (PSP-HLE-INTR-001, vtimer-ret-1us: returns
     * of 1 gave scheduled times 10000, 10100, 10200, 10300, 10400). */
    if (delay == 0u) t->handler = 0u;
    else t->schedule = scheduled + (delay < VTIMER_MIN_DELAY_US ? VTIMER_MIN_DELAY_US : delay);
}

/* The console's time zone and daylight-saving setting (#1382). It is a console setting, never the
 * host process timezone, and it lives in the one retained system profile: the virtual system
 * registry keys /CONFIG/DATE/time_zone_offset and /CONFIG/DATE/summer_time, which
 * sceUtilityGetSystemParamInt reports as TIMEZONE (6) and DAYLIGHTSAVINGS (7). The RTC local-time
 * APIs and gettimeofday read them here on every call, so a value a game writes through sceReg, or
 * the per-user overlay restores, reaches every API at once and the getter and the clocks cannot
 * disagree. The seed values and their measured status are stated once, at systemparam_int_value.
 * PSPSDK psputility_sysparam.h: TIMEZONE is the offset from UTC in minutes (EST = -300) and
 * DAYLIGHTSAVINGS is 0 (STD) or 1 (SAVING). That SAVING moves local time 60 minutes ahead is the
 * setting's meaning, not a measurement: the firmware's arithmetic is unmeasured, and so is its
 * handling of other values, which count as SAVING here. The explicit-offset sceRtcGetCurrentClock
 * path does not use the profile. Returns the local offset in minutes; zone_out and daylight_out,
 * when not NULL, receive the two settings. */
int64_t psp_profile_local_offset_minutes(int32_t *zone_out, uint32_t *daylight_out) {
    int32_t zone = (int32_t)sreg_systemparam_int(6u);   /* PSP_SYSTEMPARAM_ID_INT_TIMEZONE */
    uint32_t daylight = sreg_systemparam_int(7u);       /* PSP_SYSTEMPARAM_ID_INT_DAYLIGHTSAVINGS */
    if (zone_out) *zone_out = zone;
    if (daylight_out) *daylight_out = daylight;
    return (int64_t)zone + (daylight ? 60 : 0);
}

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

uint64_t rtc_now_tick(void) {
    if (!s_rtc_epoch_initialized) {
        /* FILETIME is 100 ns since 1601-01-01; sample the host only once to
         * establish the calendar offset.  Anchor it at guest time zero so a
         * first RTC query made after boot-time work does not double-count the
         * elapsed scheduler time.  Subsequent reads are guest-time only. */
        uint64_t host_tick = RTC_FILETIME_EPOCH_TICK + sr_host_filetime_ticks() / 10u;
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

/* ---- IoFileMgrForUser: file IO from the game ISO (src/rt/iso.c) ---- */

int guest_cstr(uint32_t addr, char *out, int max) {
    if (!out || max <= 0) return 0;
    out[0] = '\0';
    if (!addr) return 0;
    for (int i = 0; i < max; i++) {
        uint32_t cur = 0;
        if (!sr_size_add_ok(addr, (uint32_t)i, &cur) || !sr_guest_span_readable(cur, 1u)) {
            out[0] = '\0';
            return 0;
        }
        uint8_t c = MEM_R8(cur);
        if (!c) {
            out[i] = '\0';
            return 1;
        }
        if (i < max - 1) {
            out[i] = (char)c;
        }
    }
    out[max - 1] = '\0';
    return 0;
}

extern void f_32200000(CpuState *s);
extern void f_32280000(CpuState *s);
extern void f_322f8868(CpuState *s);

uint32_t io_guest_path(uint32_t guest_addr, char *out, size_t capacity);

#ifdef SR_HLE_THREAD_SELFTEST
/* The module-stop NIDs sr_syscall routes onto a guest thread for direct test calls. */
#define NID_SCE_KERNEL_STOP_MODULE 0xd1ff982au
#define NID_SCE_KERNEL_SELF_STOP_UNLOAD_MODULE 0xd675ebb8u
uint32_t sr_hle_test_stop_module(CpuState *s) { return sr_syscall(s, NID_SCE_KERNEL_STOP_MODULE); }

#define HLE_TEST_SCHED_CALL_ENTRY 0x08ffdf00u
static uint32_t s_hle_test_scheduled_nid;
static uint32_t s_hle_test_scheduled_args[5];
static uint32_t s_hle_test_scheduled_gp;
static uint32_t s_hle_test_scheduled_result;
static uint32_t s_hle_test_scheduled_call_uid;
extern void sr_test_register_guest_fn(uint32_t addr, RecompFn fn);

uint32_t sr_hle_test_scheduled_call_uid(void) {
    return s_hle_test_scheduled_call_uid;
}

static void hle_test_scheduled_call_body(CpuState *s) {
    for (unsigned i = 0; i < 5u; i++) s->r[4u + i] = s_hle_test_scheduled_args[i];
    s->r[28] = s_hle_test_scheduled_gp;
    s_hle_test_scheduled_call_uid = sched_current_uid();
    s_hle_test_scheduled_result = sr_syscall(s, s_hle_test_scheduled_nid);
    s->r[2] = s_hle_test_scheduled_result;
}

static uint32_t hle_test_run_on_guest_thread(CpuState *s, uint32_t nid) {
    s_hle_test_scheduled_nid = nid;
    for (unsigned i = 0; i < 5u; i++) s_hle_test_scheduled_args[i] = s->r[4u + i];
    s_hle_test_scheduled_gp = s->r[28];
    s_hle_test_scheduled_result = HLE_KERNEL_ERROR_NOT_IMPLEMENTED;
    s_hle_test_scheduled_call_uid = 0u;
    sr_test_register_guest_fn(HLE_TEST_SCHED_CALL_ENTRY, hle_test_scheduled_call_body);
    sched_run(HLE_TEST_SCHED_CALL_ENTRY, 0u, 0u);
    return s_hle_test_scheduled_result;
}

uint32_t sr_hle_test_register_module(const char *path, uint32_t module_start, uint32_t module_stop) {
    LoadedModule *mod = alloc_loaded_module_slot();
    if (!mod) return 0;
    uint32_t uid = sr_alloc_uid();
    mod->uid = uid;
    snprintf(mod->path, sizeof(mod->path), "%s", path ? path : "test_module.prx");
    mod->module_start = module_start;
    mod->module_stop = module_stop;
    mod->image_base = 0;
    mod->image_end = 0;
    mod->state = MODULE_STATE_LOADED;
    return uid;
}

int sr_hle_test_set_module_image(uint32_t uid, uint32_t base, uint32_t end, uint32_t gp) {
    LoadedModule *mod = find_loaded_module(uid);
    if (!mod || base >= end) return 0;
    mod->image_base = base;
    mod->image_end = end;
    mod->gp = gp;
    mod->runtime_placed = 1;
    return 1;
}

void sr_hle_test_module_reset(void) {
    s_main_module_uid = 0u;
    s_main_module_record = NULL;
    s_deferred_self_stop_uid = 0u;
    s_deferred_self_stop_thread_uid = 0u;
    s_nloaded_modules = 0;
    memset(s_loaded_modules, 0, sizeof(s_loaded_modules));
    s_prx_image_count = 0;
    memset(s_prx_images, 0, sizeof(s_prx_images));
    s_psmfplayer_lib_version = 0u;
    atomic_store_explicit(&s_prx_started_count, 0, memory_order_release);
}

/* Read-only views of the lifecycle state a regression needs to observe: the export
 * address a started module currently authorises for a NID, and how many module and
 * image records are live. Used to prove an unload retired exactly its own state. */
uint32_t sr_hle_test_started_export(uint32_t nid) { return started_module_export(nid); }
unsigned sr_hle_test_image_count(void) { return s_prx_image_count; }
unsigned sr_hle_test_module_count(void) { return (unsigned)s_nloaded_modules; }
/* -1 when no live record has the UID; else whether host HLE serves its exports. */
int sr_hle_test_module_host_served(uint32_t uid) {
    LoadedModule *mod = find_loaded_module(uid);
    return mod ? mod->host_served : -1;
}
int sr_hle_test_started_image_count(void) {
    return atomic_load_explicit(&s_prx_started_count, memory_order_acquire);
}

/* Load a synthetic PRX through the production image loader and bind it to a fresh
 * module record, performing exactly the ownership work h_LoadModule does for a
 * manifest-declared module: the real relocation/staging load, the real fixed-address
 * user-partition reservation, the real export registration, and the same
 * record->image_base link that lets unload retire this image. Only the manifest lookup
 * and the module's entry addresses are supplied by the caller. */
uint32_t sr_hle_test_load_prx_image(const char *host_path, uint32_t base,
                                    uint32_t patch_nid, uint32_t patch_off) {
    s_last_prx_entry = 0;
    s_last_prx_stop = 0;
    s_last_prx_base = 0;
    LoadedModule *mod = alloc_loaded_module_slot();
    if (!mod) return 0;
    if (!load_prx_image(host_path, base, "lifecycle")) {
        retire_loaded_module(mod);
        return 0;
    }
    (void)register_prx_exports(host_path, base);
    /* The fixture leaves its module-info export span empty (the two production readers
     * of that span disagree on its base, so no single value satisfies both), so the
     * dynamic export under test is published through the same late-import registry
     * register_prx_exports() writes into, at the guest address the caller names. */
    sr_hle_register_late_import(patch_nid, base + patch_off);
    uint32_t uid = sr_alloc_uid();
    memset(mod, 0, sizeof(*mod));
    mod->in_use = 1;
    mod->uid = uid;
    snprintf(mod->path, sizeof(mod->path), "%s", host_path);
    mod->module_start = base + 0x70;   /* module_start address the fixture declares */
    mod->module_stop = base + 0x78;    /* module_stop address the fixture declares */
    mod->image_base = s_last_prx_base;
    (void)prx_image_bounds(mod->image_base, NULL, &mod->image_end);
    mod->state = MODULE_STATE_LOADED;
    return uid;
}
#endif
Fd s_fds[64];
DirFd s_dirfds[64];

int64_t s_closed_res[64];
IoAsyncState s_closed_async_state[64];
uint64_t s_io_dispatch_epoch;
uint64_t s_io_async_sequence;
static atomic_uint s_io_raw_device_boundary_reported;
IoCwd s_io_cwds[64];

IoCwd *io_cwd_find(uint32_t thread_uid, int create) {
    IoCwd *empty = NULL;
    for (size_t i = 0; i < sizeof(s_io_cwds) / sizeof(s_io_cwds[0]); i++) {
        if (s_io_cwds[i].used && s_io_cwds[i].thread_uid == thread_uid)
            return &s_io_cwds[i];
        if (!s_io_cwds[i].used && !empty) empty = &s_io_cwds[i];
    }
    if (!create || !empty) return NULL;
    memset(empty, 0, sizeof(*empty));
    empty->used = 1;
    empty->thread_uid = thread_uid;
    return empty;
}

static void io_cwd_release_thread(uint32_t thread_uid) {
    for (size_t i = 0; i < sizeof(s_io_cwds) / sizeof(s_io_cwds[0]); i++) {
        if (s_io_cwds[i].used && s_io_cwds[i].thread_uid == thread_uid)
            memset(&s_io_cwds[i], 0, sizeof(s_io_cwds[i]));
    }
}

/* PSP relative file-manager paths resolve against the calling thread's CWD.
 * Device-qualified paths keep their explicit device and never pass through the
 * host filesystem before the existing contained VFS validates them. */
uint32_t io_guest_path(uint32_t guest_addr, char *out, size_t capacity) {
    char input[512];
    if (!out || capacity == 0u || !guest_cstr(guest_addr, input, (int)sizeof(input)))
        return 0x80010016u;
    if (strchr(input, ':') || input[0] == '/' || input[0] == '\\') {
        size_t len = strlen(input);
        if (len >= capacity) return 0x80010024u;
        memcpy(out, input, len + 1u);
        return 0u;
    }
    IoCwd *cwd = io_cwd_find(sched_current_uid(), 1);
    if (!cwd) return 0x8001000cu;
    size_t cwd_len = strlen(cwd->path), input_len = strlen(input);
    int separator = cwd_len != 0u && cwd->path[cwd_len - 1u] != '/' &&
                    cwd->path[cwd_len - 1u] != '\\';
    if (cwd_len + (size_t)separator + input_len + 1u > capacity)
        return 0x80010024u;
    memcpy(out, cwd->path, cwd_len);
    size_t at = cwd_len;
    if (separator) out[at++] = '/';
    memcpy(out + at, input, input_len + 1u);
    return 0u;
}

void hle_fd_init(void) {
    for (size_t i = 0; i < sizeof(s_dirfds) / sizeof(s_dirfds[0]); i++)
        hle_dirfd_clear(&s_dirfds[i]);
    for (size_t i = 0; i < sizeof(s_fds) / sizeof(s_fds[0]); i++) {
        if (s_fds[i].used) hle_fd_release(&s_fds[i], 0);
        else memset(&s_fds[i], 0, sizeof(s_fds[i]));
    }
    memset(s_closed_res, 0, sizeof(s_closed_res));
    memset(s_closed_async_state, 0, sizeof(s_closed_async_state));
    memset(s_io_cwds, 0, sizeof(s_io_cwds));
    s_io_dispatch_epoch = 0;
    s_io_async_sequence = 0;
    atomic_store_explicit(&s_io_raw_device_boundary_reported, 0u, memory_order_release);
    for (uint8_t stream = 0; stream < 3; stream++) {
        Fd *f = &s_fds[stream];
        f->used = 1;
        f->kind = FD_KIND_STD;
        f->std_stream = stream;
    }
}

int hle_fd_is_file(uint32_t fd) {
    return fd < (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) &&
           s_fds[fd].used && s_fds[fd].kind == FD_KIND_FILE;
}

int hle_fd_is_raw_device(uint32_t fd) {
    return fd < (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) &&
           s_fds[fd].used && s_fds[fd].kind == FD_KIND_RAW_DEVICE;
}

uint32_t hle_io_raw_device_boundary(IoRawDeviceBoundary operation,
                                           const char *api, const char *path,
                                           uint32_t fd) {
    unsigned bit = 1u << (unsigned)operation;
    unsigned previous = atomic_fetch_or_explicit(&s_io_raw_device_boundary_reported,
                                                 bit, memory_order_acq_rel);
    if (!(previous & bit)) {
        if (path && path[0])
            fprintf(stderr,
                    "SEMANTIC_BOUNDARY: IO_RAW_DEVICE_OPERATION: %s on raw device '%s' "
                    "is not supported yet (0x80010086)\n", api, path);
        else
            fprintf(stderr,
                    "SEMANTIC_BOUNDARY: IO_RAW_DEVICE_OPERATION: %s on raw device "
                    "descriptor %u is not supported yet (0x80010086)\n", api, fd);
    }
    return 0x80010086u;
}

uint32_t hle_io_raw_fd_boundary(uint32_t fd, IoRawDeviceBoundary operation,
                                       const char *api) {
    if (!hle_fd_is_raw_device(fd)) return 0u;
    return hle_io_raw_device_boundary(operation, api, s_fds[fd].guest_path, fd);
}

int hle_fd_is_raw_umd(uint32_t fd) {
    return hle_fd_is_raw_device(fd) && s_fds[fd].raw_umd && s_fds[fd].size != 0u;
}

int hle_fd_allocate(void) {
    for (int i = 3; i < (int)(sizeof(s_fds) / sizeof(s_fds[0])); i++) {
        if (!s_fds[i].used && !s_dirfds[i].used &&
            s_fds[i].async.state == IO_ASYNC_IDLE &&
            s_closed_async_state[i] == IO_ASYNC_IDLE)
            return i;
    }
    return -1;
}
/* Destination of a seek on a file descriptor (#1380). pspautotests tests/io/file at
 * f573fe8c9122a8ff5db4f030d74557bed644b4e0 records -1 for a destination before the start
 * and accepts one past the end (a read there returns 0). A Memory Stick file's position is
 * 32 bits wide: a destination of 0xFFFFFFFF is accepted and one past it answers EINVAL
 * (0x80010016) with the position unchanged, from SEEK_SET and from SEEK_END alike (measured on
 * a PSP-3000 6.61 by PSP-KERNEL-SEM-001 io-seek64 and io-seek64-rdwr, 2026-10-31, probe
 * 0c3cf0da). Other devices keep the named not-supported boundary there (UNMEASURED). A refused
 * seek leaves the position unchanged. WHENCE is already validated (< 3). */
int loadexec_ms0_path(const char *path);

/* SR_CALLCOUNT instrumentation: per-NID call tallies, dumped at the capture point. */
static struct { uint32_t nid; const char *nm; unsigned long n; } g_cc[512];
static int g_ncc = 0, g_callcount = 0;
void sr_dump_calls(void) {
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
/* In digital mode (sampling mode 0) every sample reports a centred stick (0x80, 0x80), also
 * right after analog mode reported it off centre; analog mode (1) reports its live position.
 * Measured on a PSP-3000 6.61 by PSP-KERNEL-SEM-001 ctrl-mode-digital, ctrl-mode-analog and
 * ctrl-mode-digital-again (2026-10-31, probe 0c3cf0da, stick at rest: analog read 0x75,0x7c and
 * 0x83,0x7f, digital 0x80,0x80 before and after). A test build can stand in for the stick. */
#ifdef SR_HLE_THREAD_SELFTEST
int s_ctrl_test_stick = 0;
uint8_t s_ctrl_test_lx = 128u, s_ctrl_test_ly = 128u;
void sr_hle_test_ctrl_stick(uint8_t lx, uint8_t ly) {
    s_ctrl_test_stick = 1;
    s_ctrl_test_lx = lx;
    s_ctrl_test_ly = ly;
}
#endif
#ifdef SR_HLE_THREAD_SELFTEST
/* White-box view of the no-frame observation count and the vblanks-since-flip
 * clock for the conformance harness. */
void sr_watchdog_test_state(unsigned long *fires, uint32_t *vblanks_since_flip);
#endif

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

/* Like ge_call_guest but returns the guest function's v0 (r2). Used by HLE
 * stubs that must extract a value from a guest constructor (e.g. the guest
 * malloc f_00000bcc returns the allocated block in r2). */
uint32_t ge_call_guest_gp_rv(CpuState *s, uint32_t fn, uint32_t gp,
                                    uint32_t a0, uint32_t a1, uint32_t a2);
uint32_t ge_call_guest_rv(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    return ge_call_guest_gp_rv(s, fn, s->r[28], a0, a1, a2);
}

/* The same nested call with the callee's $gp named explicitly: a module's entry points
 * run with the module's own $gp, as the kernel starts a module, not the caller's. */
uint32_t ge_call_guest_gp_rv(CpuState *s, uint32_t fn, uint32_t gp,
                                    uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t frame_sp = 0;
    int frame = -1;
    if (!fn) { fprintf(stderr, "GE_CALL_GUEST_RV: fn=0x0 (null, skipping)\n"); return 0; }
    if (!sr_nested_frame_acquire(sched_current_uid(), &frame_sp, &frame)) {
        fprintf(stderr, "GE_CALL_GUEST_RV: fn=0x%08x refused -- no nested guest-call frame "
                        "available (cur_uid=0x%x)\n", fn, sched_current_uid());
        return 0;
    }
    if (ge_log_on())
        fprintf(stderr, "GE_CALL_GUEST_RV: fn=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x cur_uid=0x%x sp=0x%08x\n",
                fn, a0, a1, a2, sched_current_uid(), frame_sp);
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    int32_t save_slice = atomic_load_explicit(&sr_timeslice, memory_order_relaxed);
    memset(s, 0, sizeof(CpuState));
    s->r[4] = a0;
    s->r[5] = a1;
    s->r[6] = a2;
    s->r[28] = gp;
    s->r[29] = frame_sp;
    s->r[31] = 0;
    s->vfpuCtrl[0] = 0xe4; s->vfpuCtrl[1] = 0xe4;
    s->pc = fn;
    atomic_store_explicit(&sr_timeslice, 20000, memory_order_relaxed);
    uint32_t callback_fatal_target = 0u;
    int callback_fatal = hle_dispatch_guest_scoped(
        s, fn, "GE guest callback with return value", &callback_fatal_target);
    uint32_t rv = s->r[2];
    if (ge_log_on())
        fprintf(stderr, "GE_CALL_GUEST_RV: fn=0x%08x returned, v0=0x%08x\n", fn, rv);
    memcpy(s, &save, sizeof(CpuState));
    if (callback_fatal) {
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = callback_fatal_target;
    }
    /* Returning to the interrupted context ends any ll..sc window it had open. */
    sr_cpu_link_clear(s);
    atomic_store_explicit(&sr_timeslice, save_slice, memory_order_relaxed);
    (void)sr_nested_frame_release(frame);
    return rv;
}

/* ---- opt-in GE completion timing (SR_GE_TIMING=1, issue #1296) ----------------------------
 *
 * By default the runtime executes a display list synchronously inside the call that hands it
 * to the GE (sceGeListEnQueue, sceGeListUpdateStallAddr, sceGeContinue) and completes it
 * there: the finish callback runs and every later sync reads "done" with no guest time spent.
 * On a PSP the GE draws while the CPU runs on, and sceGeDrawSync / sceGeListSync wait for it.
 *
 * With SR_GE_TIMING=1 a list that reached its END is GE_LIST_DRAWING until its completion
 * interrupt has been handled:
 *   - The GE is one serial timeline (s_ge_busy_until_ns).  Each list's fixed cost and each
 *     executed chunk are charged from max(now, busy-until), so a list queued behind a busy GE
 *     finishes after it.  Measured (CASE=ge-timing, C5/T15): a list enqueued while the GE is
 *     busy finishes after the running one, and back-to-back lists show no measurable gap.
 *   - The cost of each chunk is ge.c's GE time model, in GE clocks, measured on a PSP-3000
 *     (CASE=ge-timing, PSP-GE-TIMING-001, PR #1321; issue #1296's third comment).  The GE clock
 *     is the bus clock, so clocks become guest time at the emulated bus clock
 *     (sr_power_bus_clock_mhz) at the moment the work is charged.
 *   - Completion is an interrupt source the scheduler delivers (sched_set_ge_timing_hooks):
 *     the finish callback runs from scheduler_service_pending(), in interrupt context,
 *     never before the modelled time, due lists in completion order.  Measured (C1): the
 *     finish callback runs at the end of the list.
 *   - The peeks sceGeDrawSync(1) and sceGeListSync(qid, 1) read 2 until that interrupt has
 *     been handled, then 0 (measured, C3).  sceGeDrawSync(0) and sceGeListSync(qid, 0) delay
 *     the calling thread until the modelled time; the scheduler delivers the due completion
 *     before the thread runs again, so they return after the callback (measured, C2/C6).
 *     With CPU interrupts suspended the completion cannot be delivered and a blocking sync
 *     returns once the time has passed: UNMEASURED.
 * Known divergences: a SIGNAL with behaviour CONTINUE calls the signal callback mid-list on a
 * PSP (measured, C4) but signal callbacks are not dispatched here; sceGeBreak(0) does not pause
 * a drawing list (UNMEASURED); sceGeListEnQueueHead appends to the timeline instead of jumping
 * the queue (UNMEASURED). */
static int s_ge_timing = -1;              /* -1 until SR_GE_TIMING is read */
unsigned s_ge_timing_drawing;      /* lists in GE_LIST_DRAWING */

int ge_timing_on(void) {
    if (s_ge_timing < 0) {
        const char *on = getenv("SR_GE_TIMING");
        s_ge_timing = on && strcmp(on, "1") == 0;
        ge_timing_set_enabled(s_ge_timing);
        if (s_ge_timing)
            fprintf(stderr, "GE_TIMING: on, GE time model measured on PSP-3000 (CASE=ge-timing, "
                            "issue #1296), bus clock %u MHz\n", (unsigned)sr_power_bus_clock_mhz());
    }
    return s_ge_timing;
}

/* Scheduler hook: the earliest completion time of a drawing list, else UINT64_MAX. */
static uint64_t ge_timing_next_deadline_us(void) {
    uint64_t next = UINT64_MAX;
    if (!s_ge_timing_drawing) return next;
    for (int i = 0; i < GE_LIST_MAX; i++)
        if (s_ge_lists[i].status == GE_LIST_DRAWING && s_ge_lists[i].done_us < next)
            next = s_ge_lists[i].done_us;
    return next;
}

/* Scheduler hook, run as an interrupt episode: complete every list whose time has passed,
 * in completion order, and run its finish callback. */
static void ge_timing_service_due(void) {
    while (s_ge_timing_drawing) {
        uint64_t now = sched_vtime_us();
        int best = -1;
        for (int i = 0; i < GE_LIST_MAX; i++) {
            const GeListInfo *l = &s_ge_lists[i];
            if (l->status != GE_LIST_DRAWING || l->done_us > now) continue;
            if (best < 0 || l->done_us < s_ge_lists[best].done_us ||
                (l->done_us == s_ge_lists[best].done_us && l->finish_seq < s_ge_lists[best].finish_seq))
                best = i;
        }
        if (best < 0) return;
        GeListInfo *l = &s_ge_lists[best];
        ge_list_completed(best);
        s_ge_timing_drawing--;
        if (ge_log_on())
            fprintf(stderr, "GE_TIMING: list_id=0x%08x completed at %llu us (modelled %llu us)\n",
                    l->uid, (unsigned long long)now, (unsigned long long)l->done_us);
        ge_finish_callback(s_cpu, l->cbid, l->finish_id, l->cbarg);
    }
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only switch for the model; resets the GE timeline. */
void sr_hle_test_ge_timing(int on) {
    s_ge_timing = on ? 1 : 0;
    ge_timing_set_enabled(s_ge_timing);
    s_ge_busy_until_ns = 0;
}
#endif
#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only: the GE list queue as at boot, so a blocking-sync test does not wait
 * on a list an earlier test left stalled. */
void sr_hle_test_ge_lists_reset(void) {
    memset(s_ge_lists, 0, sizeof(s_ge_lists));
    s_ge_list_done_bits = UINT64_MAX;
    s_ge_list_order = 0;
    s_ge_timing_drawing = 0;
    ge_free_rebuild();
}
#endif

/* SR_AUDIOSTAT aggregates: does the game put BGM PCM into the SAS output buffer
 * before SAS runs, and does SAS preserve or erase it? pre_peak is the buffer's
 * amplitude on entry, post_peak on exit. */
int audio_stat_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_AUDIOSTAT") != NULL;
    return on;
}
MsgPipe s_msg_pipes[MSG_PIPE_MAX];


MsgPipe *msg_pipe_find(uint32_t uid) {
    for (int i = 0; i < MSG_PIPE_MAX; i++)
        if (s_msg_pipes[i].used && s_msg_pipes[i].uid == uid) return &s_msg_pipes[i];
    return NULL;
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

#endif /* SR_HLE_THREAD_SELFTEST */
Mbx s_mbx[MBX_MAX];

Mbx *mbx_find(uint32_t uid) {
    for (int i = 0; i < MBX_MAX; i++)
        if (s_mbx[i].used && s_mbx[i].uid == uid) return &s_mbx[i];
    return NULL;
}

static void mbx_remove_thread_waiters(uint32_t thread_uid) {
    for (int i = 0; i < MBX_MAX; i++) {
        if (s_mbx[i].used) mbx_remove_waiter(&s_mbx[i], thread_uid);
    }
}
Sync s_sync[128];

/* PSP-B2-01 / PSP-B3-01 (psp-hw-20260917): cancel/release family, measured by
 * fixtures wait-b2 and kernel-b3. One definition for the production registry
 * and the executable selftest (same rule as the msgpipe helpers above). */
static void hle_register_cancel_release_handlers(void) {
    hle_register_cancel_release_kernel_sync();
    hle_register_cancel_release_kernel_thread();
}

/* sceKernelReferLwMutexStatus / ...ByID stay registered as h_ok. Their
 * SceKernelLwMutexInfo layout is not in pspthreadman.h and this project has no
 * measured record of it, so writing a struct here would be invention, not
 * implementation. The gap stays visible until the oracle case measures it. */

/* =========================================================================
 * PSP Heavyweight Mutex Implementation (SceKernelMutex)
 * ========================================================================= */

#define PSP_MUTEX_ATTR_FIFO            0x0000u
/* SCE_KERNEL_ERROR_WAIT_DELETE is defined above with the semaphore oracle values.
 * WAIT_TIMEOUT, CAN_NOT_WAIT and ILLEGAL_COUNT are already defined above, next to
 * the PSPAutotests oracle citations that establish their values. Re-defining them
 * here produced three -Wmacro-redefined warnings on every build of this file. */
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR               0x80020191u

Mutex **s_mutexes = NULL;
size_t  s_mutex_len = 0;

Mutex *mutex_find(uint32_t uid) {
    if (!uid || !s_mutexes) return NULL;
    for (size_t i = 0; i < s_mutex_len; i++) {
        if (s_mutexes[i] && s_mutexes[i]->uid == uid)
            return s_mutexes[i];
    }
    return NULL;
}

void sr_mutex_release_thread(uint32_t thread_uid) {
    if (!thread_uid || !s_mutexes) return;
    for (size_t i = 0; i < s_mutex_len; i++) {
        Mutex *m = s_mutexes[i];
        if (!m) continue;
        /* Scheduler owner teardown has already detached every nesting level. */
        if (m->lock_thread == thread_uid) (void)mutex_grant_next(m);
    }
}

void mutex_reset_all(void) {
    if (!s_mutexes) return;
    for (size_t i = 0; i < s_mutex_len; i++) {
        if (s_mutexes[i]) {
            if (s_mutexes[i]->waiters) free(s_mutexes[i]->waiters);
            free(s_mutexes[i]);
        }
    }
    free(s_mutexes);
    s_mutexes = NULL;
    s_mutex_len = 0;
    s_mutex_cap = 0;
}
#ifdef SR_HLE_THREAD_SELFTEST
void sr_hle_test_mutex_reset(void) { mutex_reset_all(); }
#endif
EvfWaiter s_evf_waiters[EVF_WAITER_MAX];

/* The host PSP oracle mode is an extension of hle_thread_selftest, not a second
 * implementation.  Keep the small set of public NIDs it exercises in one registry
 * helper so every case still enters the production sr_syscall path. */
static void hle_register_selftest_oracle_handlers(void) {
    hle_register_selftest_oracle_kernel_thread();
    /* The quit-request selftest (#1392) registers its exit callback through this NID. */
    hle_register_selftest_oracle_kernel_module();
    hle_register_selftest_oracle_kernel_sync();
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
    hle_register_wait_conformance_kernel_thread();
    hle_register_wait_conformance_display();
    hle_register_wait_conformance_kernel_sync_1();
    hle_register_wait_conformance_kernel_memory_1();
    hle_register_wait_conformance_kernel_sync_2();
    hle_register_wait_conformance_kernel_memory_2();
    hle_register_wait_conformance_umd();
    hle_register_wait_conformance_input();
    hle_register_wait_conformance_io();
    hle_register_wait_conformance_kernel_interrupt();
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Registry membership probe for the conformance harness. sr_syscall() calls
 * _Exit(7) on an unregistered NID under the fiber scheduler, so a matrix cell
 * whose NID is out of registry scope must be detected without calling it. */
int sr_hle_test_is_registered(uint32_t nid) { return hle_find(nid) != NULL; }

/* See the forward declaration next to the counters themselves. */
void sr_watchdog_test_state(unsigned long *fires, uint32_t *vblanks_since_flip) {
    if (fires)             *fires             = s_watchdog_fires;
    if (vblanks_since_flip) *vblanks_since_flip = s_vcount - s_last_flip_vcount;
}
#endif /* SR_HLE_THREAD_SELFTEST */

static void hle_register_time_handlers(void) {
    hle_register_time_kernel_interrupt();
    hle_register_time_kernel_thread();
    hle_register_time_kernel_time_1();
    hle_register_time_rtc();
    hle_register_time_kernel_time_2();
}

static void hle_register_kernel_module_handlers(void) {
    hle_register_kernel_module_npdrm();
    hle_register_module_lifecycle_handlers();
}

static void hle_register_bulk_memory_handlers(void) {
    hle_register_bulk_memory_cache_dma();
    hle_register_bulk_memory_kernel_utils();
}

/* Single definition called by both sr_hle_init() branches so the executable
 * regression dispatches the exact production sceKernelExitGame registration.
 * Terminating the host process is what stops the guest libc reentrancy guard
 * from looping. */
static void hle_register_partition_savedata_handlers(void) {
    hle_register_partition_savedata_kernel_memory();
    hle_register_partition_savedata_utility();
}

/* scePsmfPlayer: lifecycle, bounded source/demux production, and queue ownership are
 * implemented here; decoder output remains a separate fail-closed stage.  One definition,
 * called by both registry branches, so the executable harness dispatches the production
 * mapping and the named media boundaries are reachable from a production-path test. */
/* sceLibFont: one mapping for the production registry and the executable harness, so the
 * harness dispatches the production handlers (issue #1384). */
/* The console profile's getters (#1382): one helper for the production registry and the
 * selftest, so both dispatch the same mapping. */
static void hle_register_console_profile_handlers(void) {
    hle_register_console_profile_network();
    hle_register_console_profile_system();
}

/* sceReg (virtual system registry) and sceUtilityGetSystemParamInt. The system-param getter reads
 * the registry's modeled keys, so both are registered here: the executable harness and the
 * production registry then dispatch one mapping. sceRegExit (0x9b25edf1), sceRegRemoveKey
 * (0x3615bc87, no public argument documentation) and sceRegRemoveRegistry (0xdeda92bf, which this
 * model never performs) stay unregistered, so a call is a visible dispatch miss. */
static void hle_register_sysreg_handlers(void) {
    hle_register_sysreg_utility();
    hle_register_sysreg_system();
}

/* Public APIs reported as first-stop kernel imports in the library sweep.
 * Keep this one mapping in both the executable harness and production registry;
 * facilities with missing lifecycle state are registered as explicit refusals. */
static void hle_register_kernel_import_sweep_handlers(void) {
    hle_register_kernel_import_sweep_kernel_thread_1();
    hle_register_kernel_import_sweep_kernel_time_1();
    hle_register_kernel_import_sweep_kernel_thread_2();
    hle_register_kernel_import_sweep_kernel_time_2();
    hle_register_kernel_import_sweep_kernel_memory();
    hle_register_kernel_import_sweep_ge();
    hle_register_kernel_import_sweep_kernel_module();
    hle_register_kernel_import_sweep_display();
}

/* Imports that stopped titles first in the 2026-10-29 all-games launch sweep, each implemented
 * from PSP-recorded behaviour (the evidence is at each handler). One helper for both
 * sr_hle_init() branches, so the executable harness dispatches the production mapping. */
static void hle_register_launch_sweep_blocker_handlers(void) {
    hle_register_launch_sweep_blocker_rtc();
    hle_register_launch_sweep_blocker_kernel_thread();
    hle_register_launch_sweep_blocker_font();
}

static void hle_register_unregistered_import_batch(void) {
    hle_register_unregistered_import_kernel_time_1();
    hle_register_unregistered_import_kernel_thread_1();
    /* scePower_469989ad (0x469989ad) has no public name in the sources this project may consult, so
     * it is refused under its synthetic name rather than given a guessed one. Its contract needs a
     * named source and a probe before it can be modelled. */
    hle_register_unregistered_import_power();
    /* sceKernelReferSystemStatus: measured for the bytes written and the size field; see h_ReferSystemStatus. */
    hle_register_unregistered_import_kernel_thread_2();
    /* VTimer lifecycle and query surface (measured; see the VTimer block above). */
    hle_register_unregistered_import_kernel_time_2();
    hle_register_unregistered_import_display();
    hle_register_unregistered_import_utility();
    hle_register_unregistered_import_network_1();
    /* sceRtcGetAccumulativeTime (0x011f03c1) is not declared in the PSPSDK RTC header, so its contract
     * is not established here and it is refused under its name rather than guessed. */
    hle_register_unregistered_import_rtc();
    hle_register_unregistered_import_network_2();
    hle_register_unregistered_import_audio_codec();
}

/* What a stalled thread's object wait is on, for the watchdog's thread dump: the kernel
 * object's class, name and the state that decides whether the wait can end. 0 when no object
 * has the uid (a thread left waiting on a deleted object, or a wait the HLE does not own). */
static int hle_describe_wait_object(uint32_t thread_uid, uint32_t obj, char *out, size_t cap) {
    Sync *y = NULL;
    for (int i = 0; i < (int)(sizeof(s_sync) / sizeof(s_sync[0])); i++)
        if (s_sync[i].used && s_sync[i].uid == obj) { y = &s_sync[i]; break; }
    if (y && y->kind == SYNC_KIND_SEMA) {
        snprintf(out, cap, "semaphore 0x%x '%s' count=%d max=%d", obj, y->name, y->count, y->maxc);
        return 1;
    }
    if (y && y->kind == SYNC_KIND_EVENT_FLAG) {
        for (int i = 0; i < EVF_WAITER_MAX; i++) {
            const EvfWaiter *w = &s_evf_waiters[i];
            if (w->active && w->evf == obj && w->thread == thread_uid) {
                snprintf(out, cap, "event flag 0x%x '%s' pattern=0x%08x wants=0x%08x mode=%s%s", obj,
                         y->name, y->pattern, w->bits, (w->mode & 1u) ? "OR" : "AND",
                         (w->mode & 0x30u) ? "+clear" : "");
                return 1;
            }
        }
        snprintf(out, cap, "event flag 0x%x '%s' pattern=0x%08x", obj, y->name, y->pattern);
        return 1;
    }
    if (y && y->kind == SYNC_KIND_LWMUTEX) {
        uint32_t wa = y->workarea;
        if (wa && sr_guest_span_readable(wa, 8u))
            snprintf(out, cap, "lwmutex 0x%x '%s' lockLevel=%d owner=0x%x", obj, y->name,
                     (int32_t)MEM_R32(wa), MEM_R32(wa + 4u));
        else
            snprintf(out, cap, "lwmutex 0x%x '%s'", obj, y->name);
        return 1;
    }
    const Mutex *m = mutex_find(obj);
    if (m) {
        snprintf(out, cap, "mutex 0x%x '%s' count=%d owner=0x%x waiters=%d", obj, m->name,
                 m->current_count, m->lock_thread, m->num_wait_threads);
        return 1;
    }
    const Mbx *x = mbx_find(obj);
    if (x) {
        snprintf(out, cap, "message box 0x%x '%s' messages=%d", obj, x->name, x->nmsgs);
        return 1;
    }
    const MsgPipe *pipe = msg_pipe_find(obj);
    if (pipe) {
        snprintf(out, cap, "message pipe 0x%x '%s' bytes=%u/%u", obj, pipe->name, pipe->count,
                 pipe->capacity);
        return 1;
    }
    const FplPool *fpl = fpl_lookup(obj);
    if (fpl) {
        snprintf(out, cap, "fixed pool 0x%x '%s' free=%d/%d", obj, fpl->name, fpl->nfree,
                 fpl->nblocks);
        return 1;
    }
    if (vpl_lookup(obj)) {
        snprintf(out, cap, "variable pool 0x%x", obj);
        return 1;
    }
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
int sr_hle_test_describe_wait_object(uint32_t thread_uid, uint32_t obj, char *out, size_t cap) {
    return hle_describe_wait_object(thread_uid, obj, out, cap);
}
#endif

static const char *hle_watchdog_nid_name(uint32_t nid) {
    const HleEntry *e = hle_find(nid);
    return e ? e->name : sr_nid_name(nid);
}

void sr_hle_init(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_hle_init_state, &expected, 1,
                                                  memory_order_acq_rel, memory_order_acquire)) {
        while (atomic_load_explicit(&s_hle_init_state, memory_order_acquire) != 2) { }
        return;
    }
    sched_set_thread_exit_hook(hle_module_thread_exit);
    sched_set_watchdog_describers(hle_describe_wait_object, hle_watchdog_nid_name);
#ifdef SR_MPEG_MEDIA_SELFTEST
    hle_register_mpeg_ycbcr_handlers();
    hle_register_mpeg_avc_decode_handler();
    /* The media fixture drives the production scePsmfPlayer handlers through sr_syscall,
     * so the guest getter and the A/V presentation boundaries are reachable from a
     * production-path test (issue #279). */
    hle_register_psmf_player_handlers();
    /* ... and the host presentation boundaries those pictures and blocks reach: the display
     * flip and the audio output calls, so the presentation-offset measurement is exercised
     * through the production handlers rather than a white-box call. */
    hle_register_display_setframebuf_handler();
    hle_register_regular_audio_handlers();
    hle_register_output2_audio_handlers();
#else
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
    hle_register_kernel_module_handlers();
    hle_register_font_handlers();
    hle_register_console_profile_handlers();
    hle_register_msgpipe_handlers();
    hle_register_mbx_handlers();
    /* PSP-B2-01 / PSP-B3-01 cancel/release family: one definition for both
     * branches so the selftest exercises the production mapping. */
    hle_register_cancel_release_handlers();
    /* Wait/blocking APIs the issue #88 conformance matrix enters -- the same
     * definition the production branch below calls. */
    hle_register_wait_conformance_handlers();
    hle_register_kernel_import_sweep_handlers();
    hle_register_launch_sweep_blocker_handlers();
    hle_register_sysreg_handlers();
    hle_register_unregistered_import_batch();
    hle_register_umd_callback_handlers();
    hle_register_regular_audio_handlers();
    hle_register_output2_audio_handlers();
    hle_register_exit_game_handler();
    hle_register_ge_handlers();
    sr_hle_power_register_clock();
    hle_register_power_coexistence_handlers();
    sr_hle_power_register_callback();
    sr_hle_power_register_battery();
    hle_register_devkit_version_handler();
    /* TD-24 batch 4 families: shared with production through the helpers, so
     * the executable harness dispatches the exact production mapping. */
    hle_register_ctrl_sampling_handlers();
    sr_hle_power_register_lock();
    hle_register_gpi_gpo_handlers();
    hle_register_mpeg_shared_handlers();
    hle_register_psmf_player_handlers();
    hle_register_partition_savedata_handlers();
    hle_register_impose_handlers();
    hle_register_osk_handlers();
    hle_register_utility_dialog_handlers();
    hle_register_dcache_handlers();
    hle_register_icache_handlers();
#else
    /* Wait/blocking APIs shared with the issue #88 conformance matrix. Single
     * definition, called by both branches, so the selftest cannot drift from the
     * registry the game build uses. */
    hle_register_cancel_release_handlers();
    hle_register_wait_conformance_handlers();
    hle_register_kernel_import_sweep_handlers();
    hle_register_launch_sweep_blocker_handlers();
    hle_register_unregistered_import_batch();
    hle_register_umd_callback_handlers();
    hle_register_partition_savedata_handlers();
    /* Internal address callback, reached only after a normal dispatch-table miss. */
    hle_register_init_kernel_utils_1();
    /* NID audit 2026-06: every entry below verified against PPSSPP's HLE tables. A handler on
     * the wrong NID is worse than none -- handlers that fill out-params write through whatever
     * the registers happen to hold for the REAL function's signature (the VolatileMemUnlock
     * mixup sprayed two wild words per asset load and zeroed the model-slot counter). */
    hle_register_init_kernel_memory_1();
    /* sceKernelAllocPartitionMemory, sceKernelGetBlockHeadAddr, sceKernelFreePartitionMemory,
     * sceKernelTotalFreeMemSize, sceKernelMaxFreeMemSize registered via
     * hle_register_partition_savedata_handlers() */
    /* sceKernelTryAllocateFpl, sceKernelFreeFpl, and sceKernelDeleteFpl moved to
     * hle_register_wait_conformance_handlers(), which this branch also calls --
     * same NIDs, names and handlers as before. */
    hle_register_init_kernel_thread_1();
    /* sceImpose language/confirm-button mode: retained state shared with the system-param table
     * above (the getter had no registration at all before this). */
    hle_register_impose_handlers();
    hle_register_osk_handlers();
    hle_register_utility_dialog_handlers();
    hle_register_console_profile_handlers();
    sr_hle_power_register_callback();
    /* sceMpeg: faithful port (src/rt/mpeg.c). Drives the PSMF intro to completion. */
    hle_register_init_mpeg();
    /* scePsmfPlayer: lifecycle, bounded source/demux production, and queue ownership are
     * implemented here; decoder output remains a separate fail-closed stage. */
    hle_register_psmf_player_handlers();
    /* sceLibFont: synchronized handles backed by parsed firmware/user PGFs. */
    hle_register_font_handlers();
    hle_register_init_umd();
    /* Callback-aware UMD wait consumes callbacks while preserving the drive wait. */
    hle_register_kernel_module_handlers();
    /* IoFileMgrForUser: file IO from the ISO. */
    hle_register_iofilemgr_handlers();
    /* sceAudio regular channels and Output2 share one production mapping with the
     * executable audio contract harness. */
    hle_register_regular_audio_handlers();
    hle_register_output2_audio_handlers();
    /* sceCtrl sampling family: shared with the executable harness through
     * hle_register_ctrl_sampling_handlers(). */
    hle_register_ctrl_sampling_handlers();
    hle_register_ge_handlers();
    /* Issue #86 battery getters: registered from hle_power.c, at the position these four calls held. */
    sr_hle_power_register_battery();
    /* Clock getters/setters: shared with the executable harness through
     * sr_hle_power_register_clock(), which carries the canonical #86
     * getter NIDs noted above. */
    sr_hle_power_register_clock();
    hle_register_power_coexistence_handlers();
    hle_register_init_kernel_thread_2();
    hle_register_utility_module_handlers();
    hle_register_init_kernel_memory_2();
    hle_register_devkit_version_handler();

    /* Cache maintenance: registration helpers shared with the executable harness. The
     * Icache NIDs bind and unbind code overlays; under SR_STALE_DETECT they also run the
     * TD-27 check-and-abort contract the Dcache handlers run. */
    hle_register_dcache_handlers();
    hle_register_icache_handlers();
    /* GPO latch / GPI pins: shared with the executable harness through
     * hle_register_gpi_gpo_handlers(). */
    hle_register_gpi_gpo_handlers();
    /* StdioForKernel std handles and kernel printf NIDs (0xcab439df is the
     * StdioForKernel "printf" export; 0x13a5abef is SysMemUserForUser's
     * "sceKernelPrintf"). */
    hle_register_init_kernel_utils_2();
    /* scePower / sceSuspendForUser / LoadExecForUser: locks and registrations succeed. */
    sr_hle_power_register_lock();
    /* Unlock takes only the type arg -- it must NOT run the Lock handler: writing the
     * out-params through leftover a1/a2 register garbage sprayed two wild 4-byte writes
     * per asset-load unlock (this zeroed the resource registry's model-slot counter,
     * which silently killed every .PMD model lookup, e.g. the hangar aircraft). */
    hle_register_exit_game_handler();
    /* InterruptManager Disable/Release pair: shared with the executable harness
     * through hle_register_wait_conformance_handlers(), next to Register/Enable. */
    hle_register_msgpipe_handlers();
    hle_register_mbx_handlers();
    /* semaphores */
    /* Event flag handlers are registered by hle_register_wait_conformance_handlers. */
    /* Lightweight mutexes: created and locked via hle_register_wait_conformance_handlers. */

    /* The sceReg family and sceUtilityGetSystemParamInt: see hle_register_sysreg_handlers(). */
    hle_register_sysreg_handlers();
#endif

    hle_register_time_handlers();
    hle_register_display_handlers();

    hle_register_atrac_handlers();
    hle_register_mp3_handlers();
    hle_register_sas_handlers();
#endif
#ifndef SR_MPEG_MEDIA_SELFTEST
    /* Opt-in GE completion timing: the hooks report nothing unless SR_GE_TIMING=1. */
    sched_set_ge_timing_hooks(ge_timing_next_deadline_us, ge_timing_service_due);
    (void)ge_timing_on();
    sched_set_vtimer_hooks(vtimer_next_deadline_us, vtimer_service_one);
#endif
    atomic_store_explicit(&s_hle_init_state, 2, memory_order_release);
}

/* ---- dispatch ---- */

/* Import linking for runtime-loaded modules: a NID exported by a module whose image is resident
 * and whose module_start has run executes that module's recompiled guest code (e.g.
 * scePsmfPlayer* -> libpsmfplayer.prx), exactly as the kernel links the importer's stub to the
 * export. Returns 1 when the call was linked (result in v0); the host handler then only serves
 * NIDs no started module exports. */
static int link_started_export(CpuState *s, uint32_t nid) {
    uint32_t target = started_module_export(nid);
    RecompFn gfn = target ? sr_lookup(target) : NULL;
    if (!gfn) return 0;
    /* Log each linked NID once. */
    static uint32_t s_linked_seen[256];
    static unsigned s_linked_n = 0;
    unsigned k = 0;
    while (k < s_linked_n && s_linked_seen[k] != nid) k++;
    if (k == s_linked_n && s_linked_n < 256) {
        s_linked_seen[s_linked_n++] = nid;
        const char *nm = sr_nid_name(nid);
        fprintf(stderr, "PRX link: %s (0x%08x) -> guest 0x%08x\n", nm ? nm : "?", nid, target);
    }
    if (sr_perf_enabled) sr_perf_aot_begin(target);
    int exec_previous = sr_exec_enter(SR_EXEC_TIER_AOT);
    sr_call_body(gfn, s);   /* completes the export's tail chain inside its span (#855) */
    sr_exec_leave(exec_previous);
    if (sr_perf_enabled) sr_perf_aot_end();
    return 1;
}

/* An import with no handler stops the run (exit 7). Name that stop for the player the
 * way the dispatcher names its own: one semantic-boundary line in SR_BOOT_EVENT_FILE,
 * "unsupported-import-<name>" in the player's [a-z0-9-] alphabet, so its card says the
 * game stopped early at that import instead of showing a bare child exit code (#308). */
static void hle_record_unsupported_import_stop(uint32_t nid, const char *name) {
    const char *path = getenv("SR_BOOT_EVENT_FILE");
    if (!path || !path[0]) return;
    char boundary[96];
    int written = snprintf(boundary, sizeof(boundary), "unsupported-import-");
    if (written <= 0) return;
    size_t at = (size_t)written;
    if (name && name[0]) {
        for (const char *c = name; *c && at + 1u < sizeof(boundary); c++) {
            char ch = *c;
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) ch = '-';
            boundary[at++] = ch;
        }
        boundary[at] = '\0';
    } else {
        snprintf(boundary + at, sizeof(boundary) - at, "0x%08x", nid);
    }
    FILE *events = fopen(path, "ab");
    if (!events) return;
    fprintf(events,
            "BOOT_EVENT phase=stop reason=semantic-boundary boundary=%s issue=%u\n",
            boundary, 308u);
    fclose(events);
}

/* SR_NIDLOG: one line per syscall (NID, thread, VBLANK) to nidseq_mine.txt in the run
 * folder (docs/DEBUGGING.md), flushed every 64 lines. */
static void hle_nidlog_note(uint32_t nid) {
    static FILE *nf = NULL;
    static unsigned long nc = 0;
    if (!nf) nf = fopen("nidseq_mine.txt", "w");
    if (!nf) return;
    fprintf(nf, "0x%08x 0x%x %u\n", nid, sched_current_uid(), s_vcount);
    if ((++nc & 0x3f) == 0) fflush(nf);
}

uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    sr_tail_assert_empty("tail-transfer-live-at-syscall");
    sr_hle_init();
#ifdef SR_HLE_THREAD_SELFTEST
    /* Direct white-box calls have no current TCB. Route module stop/self-stop
     * through the production handlers inside the real scheduler; nested calls
     * from that guest thread take the ordinary dispatch path below. */
    if ((nid == NID_SCE_KERNEL_STOP_MODULE || nid == NID_SCE_KERNEL_SELF_STOP_UNLOAD_MODULE) &&
        !sched_guest_thread_running())
        return hle_test_run_on_guest_thread(s, nid);
#endif
    io_async_dispatch_tick();
    /* A syscall is where a guest thread can stop making scheduler progress, so the
     * attribution of a late display period has to be able to name it. */
    const int rt_phase_saved = sr_rt_phase;
    sr_rt_phase = SR_RT_PHASE_SYSCALL;
    sr_rt_nid = nid;
    const int import_active_saved = sr_import_active;
    const uint32_t import_nid_saved = sr_import_nid;
    sr_import_active = 1;
    sr_import_nid = nid;
    /* One store per import, and only while a route is waiting on one: this is what lets a
     * route gate on a guest-visible event instead of on a screen signature. */
    if (s_route_watch_nid) route_note_nid(nid);
    uint64_t flight_sequence = sr_flight_hle_import(
        nid, sched_current_uid(), s ? s->r[4] : 0u, s ? s->pc : 0u, s ? s->r[31] : 0u);
    if (flight_sequence != 0u && s) {
        sr_flight_hle_arguments(flight_sequence, s->r[4], s->r[5], s->r[6], s->r[7]);
    }
    sr_last_nid = nid;
    if (hle_nidlog_on()) hle_nidlog_note(nid);
    if (link_started_export(s, nid)) {
        uint32_t ret = s->r[2];
        if (flight_sequence != 0u) sr_flight_hle_return(flight_sequence, ret);
        sched_note_import_return(nid, ret);
        sr_rt_phase = rt_phase_saved;
        sr_import_active = import_active_saved;
        sr_import_nid = import_nid_saved;
        return ret;
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
        sr_flight_unsupported_fatal(nid, sched_current_uid(), s->pc);
        {
            const char *nm = sr_nid_name(nid);
            fprintf(stderr, "HLE: unimplemented nid 0x%08x (%s) (thread uid 0x%x)\n"
                            "     -> add a handler in src/rt/hle.c: sr_hle_register(0x%08xu, \"%s\", h_...);\n",
                    nid, nm ? nm : "unknown", sched_current_uid(), nid, nm ? nm : "sceUnknown");
            hle_record_unsupported_import_stop(nid, nm);
        }
        sr_hit_hle = 1;
        /* Under the fiber scheduler, longjmp across fibers is invalid; stop the process cleanly
         * after flushing the trace. The plain driver (no scheduler) keeps the longjmp boundary. */
        if (sr_sched_on) { sr_trace_close(); sr_compat_report(); sr_perf_shutdown(); fflush(stderr); _Exit(7); }
        longjmp(g_hle_jmp, 1);
    }
    if (hle_syslog_on()) {
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
    uint32_t ret;
    if (e->unsupported_error) {
        hle_note_unsupported(e);
        ret = e->unsupported_error;
    } else {
        hle_note_unverified(e);
        ret = e->fn(s);
        hle_note_success_exception(e);
    }
    ge_enqueue_trace_note_hle(s, nid, e->name);
    /* An HLE handler stands in for a kernel syscall, and a syscall returns through eret,
     * which clears LLbit (MIPS32): an ll..sc window spanning an HLE call fails its sc.
     * link_started_export() above is deliberately exempt -- a call into another started
     * module's export is a plain jump on hardware, with no exception return. */
    sr_cpu_link_clear(s);
    /* Poison caller-saved temps exactly like PPSSPP SetDeadbeefRegs: r1, r4-r15, r24, r25,
     * hi, lo. The return value in v0 (and v1) is written afterward and survives. */
    s->r[1] = 0xDEADBEEFu;
    for (int i = 4; i <= 15; i++) s->r[i] = 0xDEADBEEFu;
    s->r[24] = 0xDEADBEEFu; s->r[25] = 0xDEADBEEFu;
    s->hi = 0xDEADBEEFu; s->lo = 0xDEADBEEFu;
    s->r[2] = ret;
    if (flight_sequence != 0u) sr_flight_hle_return(flight_sequence, ret);
    if (e->unsupported_error) {
        sr_flight_unsupported(e->nid, e->unsupported_error, sched_current_uid(), s->pc);
    }
    sched_note_import_return(nid, ret);
    sr_rt_phase = rt_phase_saved;
    sr_import_active = import_active_saved;
    sr_import_nid = import_nid_saved;
    return ret;
}

void sr_hle_loadexec_release(SrLoadExecRequest *request) {
    if (!request) return;
    free(request->args);
    memset(request, 0, sizeof(*request));
}

void loadexec_reset_user_partition(void) {
    s_heap = 0;
    s_heap_base = 0;
    s_part_top = 0;
    s_heap_last_bump = 0;
    s_nblocks = 0;
    memset(s_blocks, 0, sizeof(s_blocks));
    memset(s_memory_blocks, 0, sizeof(s_memory_blocks));
    s_nfree_blocks = 0;
    memset(s_free_blocks, 0, sizeof(s_free_blocks));
}

int sr_hle_loadexec_boot_module(const char *guest_path, uint32_t *entry_out,
                                uint32_t *gp_out) {
    const char *file = NULL;
    int required = 0;
    SrGuestModulePlacement placement = SR_GUEST_MODULE_FIXED;
    if (!guest_path || !entry_out || !gp_out || !loadexec_ms0_path(guest_path) ||
        !sr_title_config_guest_module(guest_path, &file, NULL, &required, &placement) ||
        !required || placement != SR_GUEST_MODULE_RUNTIME ||
        !loadexec_manifest_file_safe(file) ||
        !loadexec_module_file_matches_translation(file)) {
        fprintf(stderr, "LOAD_EXEC_PACKAGE_PATH_REFUSED: program is absent from the "
                        "required runtime-module list\n");
        return 0;
    }

    uint32_t uid = load_module_by_guest_path(guest_path, 0u);
    if (uid == 0u || (uid & 0x80000000u)) {
        fprintf(stderr, "LOAD_EXEC_PROGRAM_LOAD_FAILED: declared package program refused "
                        "by its runtime module loader\n");
        return 0;
    }
    LoadedModule *mod = find_loaded_module(uid);
    if (!mod || !mod->runtime_placed || !mod->code || mod->module_start == 0u ||
        sr_lookup(mod->module_start) == NULL) {
        fprintf(stderr, "LOAD_EXEC_PROGRAM_ENTRY_UNAVAILABLE: declared package program "
                        "has no translated module_start; refusing\n");
        if (mod) {
            mod->state = MODULE_STATE_STOPPED;
            unload_prx_image(mod->image_base);
            retire_loaded_module(mod);
        }
        return 0;
    }
    /* This image becomes the new root program rather than a guest LoadModule
     * handle. Mark it as the active program before entering its module_start so
     * a subsequent LoadExec can stop it on a guest stop thread. */
    mod->state = MODULE_STATE_STARTED;
    mod->start_entry_ran = 1;
    mark_module_started(mod->module_start);
    s_loadexec_boot_uid = uid;
    *entry_out = mod->module_start;
    *gp_out = mod->gp;
    return 1;
}

void sr_hle_loadexec_finish(void) {
    if (!s_loadexec_boot_uid) return;
    LoadedModule *mod = find_loaded_module(s_loadexec_boot_uid);
    uint32_t uid = s_loadexec_boot_uid;
    s_loadexec_boot_uid = 0u;
    if (!mod || mod->state != MODULE_STATE_STARTED) return;
    mark_module_started(mod->module_start);
    mod->start_entry_ran = 1;
    fprintf(stderr, "LOAD_EXEC_PROGRAM_STARTED: uid=0x%x entry=0x%08x\n",
            uid, mod->module_start);
}
