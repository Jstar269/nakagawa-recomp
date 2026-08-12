// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
/*
 * Guest code-address dispatch table (issue #45).
 *
 * Maps a guest code address to the recompiled C function that implements it. Computed
 * transfers (jalr / jr $reg, and jal/j to a target the analyzer did not recompile as a
 * function) resolve through here; direct edges to a known function are emitted by codegen
 * as a plain C call and never enter this table.
 *
 * This header holds the whole data structure and its primitives so the exact production
 * logic can be exercised host-neutrally (src/rt/dispatch_selftest.c). recomp.c owns the one
 * live instance and wraps these as sr_register()/sr_lookup().
 *
 * ---------------------------------------------------------------------------------------
 * The invariant this file exists to hold (issue #45):
 *
 *   The integer value zero must never, by itself, answer the question "is this slot
 *   occupied?". A guest code address of 0 is a first-class key -- the recompiled image is
 *   based at 0, so the function at image offset 0 has address 0 -- and must be registrable
 *   and retrievable exactly like any other. Occupancy is therefore carried in a dedicated
 *   `state` field, independent of both the key (`addr`) and the payload (`fn`).
 *
 *   The earlier design used `addr == 0` as the empty-slot sentinel, which made
 *   sr_register(0, fn) indistinguishable from an unused slot: a probe for key 0 stopped at
 *   the first slot whose stored key was 0 -- i.e. immediately -- and the L1 fast path was
 *   guarded by `addr != 0`. Address 0 was thus impossible to look up. See the tests.
 *
 * Distinguishing a real code address 0 from a NULL function pointer is NOT this table's
 * job. sr_lookup(0) answering "here is the function at offset 0" is correct. Whether a
 * *computed* dispatch target of 0 should run that function or be treated as a null-pointer
 * call is a policy decision made in dispatch() (it is a null call, and stays diagnosed):
 * this table only has to represent the mapping truthfully.
 *
 * Concurrency contract (unchanged from the previous design, and preserved by the tests):
 *   - registration happens one thread at a time (module init, the late-import bridge);
 *   - lookups run concurrently across host worker threads and coroutine yields;
 *   - a reader that observes a slot as occupied must observe the addr and fn that were
 *     stored for it -- never one key paired with another entry's function.
 * The single synchronization point is `state`: register publishes addr and fn (relaxed),
 * then stores state=1 with release LAST; a reader loads state with acquire and only then
 * trusts addr/fn. The L1 entry is a single 64-bit atomic so key and slot are read as one
 * unit; it is published with release after the main slot is fully valid.
 */
#ifndef SR_DISPATCH_TABLE_H
#define SR_DISPATCH_TABLE_H

#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
#include <atomic>
#ifndef _Atomic
#define _Atomic(T) std::atomic<T>
#endif
using std::atomic_load_explicit;
using std::atomic_store_explicit;
using std::memory_order_relaxed;
using std::memory_order_acquire;
using std::memory_order_release;
#else
#include <stdatomic.h>
#endif

#define SR_DTAB_SIZE     131072u
#define SR_DTAB_MASK     (SR_DTAB_SIZE - 1u)
#define SR_DTAB_L1_SIZE  4096u
#define SR_DTAB_L1_MASK  (SR_DTAB_L1_SIZE - 1u)

/* state: 0 = empty (never written), 1 = occupied. Independent of addr and fn so that a
 * key of 0 is representable. */
typedef struct {
    _Atomic(uint32_t)  addr;
    _Atomic(uint32_t)  state;
    _Atomic(uintptr_t) fn;
} SrDispatchEntry;

typedef struct {
    SrDispatchEntry   main[SR_DTAB_SIZE];
    /* Direct-mapped L1 in front of the open-addressed main table. Each entry packs
     * ((slot + 1) << 32) | guest_addr. The +1 bias means a fully-zero word is always
     * "empty", even for the legitimate (slot 0, addr 0) entry, so occupancy here is also
     * independent of the key -- the same bug, avoided the same way. */
    _Atomic(uint64_t) l1[SR_DTAB_L1_SIZE];
} SrDispatchTable;

static inline uint32_t sr_dtab_l1_index(uint32_t addr) {
    /* Ignore instruction alignment bits and mix nearby basic-block addresses. */
    uint32_t x = addr >> 2;
    x ^= x >> 12;
    x *= 0x9e3779b1u;
    return x & SR_DTAB_L1_MASK;
}

/* Register (or re-register) addr -> fn. Single-writer. fn must be non-zero: 0 is the
 * "not found" return of sr_dtab_lookup, and a recompiled function pointer is never NULL. */
static inline void sr_dtab_register(SrDispatchTable *t, uint32_t addr, uintptr_t fn) {
    uint32_t h = (addr >> 2) & SR_DTAB_MASK;
    for (;;) {
        uint32_t st = atomic_load_explicit(&t->main[h].state, memory_order_relaxed);
        if (st == 0u) break;   /* empty slot -- claim it */
        uint32_t a = atomic_load_explicit(&t->main[h].addr, memory_order_relaxed);
        if (a == addr) break;  /* re-registration of an existing key */
        h = (h + 1u) & SR_DTAB_MASK;
    }
    atomic_store_explicit(&t->main[h].addr, addr, memory_order_relaxed);
    atomic_store_explicit(&t->main[h].fn, fn, memory_order_relaxed);
    /* Publish occupancy LAST with release: a reader that observes state==1 (acquire) is
     * then guaranteed to see the addr and fn stored above. */
    atomic_store_explicit(&t->main[h].state, 1u, memory_order_release);
    uint32_t l1 = sr_dtab_l1_index(addr);
    atomic_store_explicit(&t->l1[l1], ((uint64_t)(h + 1u) << 32) | addr, memory_order_release);
}

/* Look up addr. Returns the registered fn, or 0 if the address is not registered. */
static inline uintptr_t sr_dtab_lookup(SrDispatchTable *t, uint32_t addr) {
    uint32_t li = sr_dtab_l1_index(addr);
    uint64_t pair = atomic_load_explicit(&t->l1[li], memory_order_acquire);
    if (pair != 0u && (uint32_t)pair == addr) {
        /* Matched key and slot as one unit; the slot's fn can only be the (valid) function
         * registered for this addr. The bias means pair==0 is the only "empty" encoding,
         * so a cached (slot 0, addr 0) entry is a hit, not a miss. */
        uint32_t slot = (uint32_t)(pair >> 32) - 1u;
        return atomic_load_explicit(&t->main[slot].fn, memory_order_acquire);
    }
    uint32_t h = (addr >> 2) & SR_DTAB_MASK;
    for (;;) {
        uint32_t st = atomic_load_explicit(&t->main[h].state, memory_order_acquire);
        if (st == 0u) return 0u;   /* empty slot terminates the probe -- addr not present */
        uint32_t a = atomic_load_explicit(&t->main[h].addr, memory_order_relaxed);
        if (a == addr) {
            uintptr_t fn = atomic_load_explicit(&t->main[h].fn, memory_order_relaxed);
            atomic_store_explicit(&t->l1[li], ((uint64_t)(h + 1u) << 32) | addr,
                                  memory_order_release);
            return fn;
        }
        h = (h + 1u) & SR_DTAB_MASK;
    }
}

/* This table is deliberately POLICY-FREE. It answers exactly one question -- "is address X
 * registered, and to what implementation?" -- and nothing about what a runtime pointer VALUE
 * means. In particular it does NOT decide whether a computed target of 0 is a NULL pointer,
 * a module-relative code offset 0, or a future image-relative target: that is unresolved
 * under #45 (an address-taken offset-0 function pointer carried through guest data also
 * arrives as integer 0 without module identity) and is owned by #20/#45, not by this API.
 * sr_dtab_lookup(0) therefore resolves the offset-0 function like any other key. The current
 * HST runtime null-call policy lives separately in dispatch() (the NULL_CALL_B exact hook,
 * which runs before lookup); keeping that policy out of this file is the point. */

#endif /* SR_DISPATCH_TABLE_H */
