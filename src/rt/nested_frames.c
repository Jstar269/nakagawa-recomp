// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Per-owner, per-depth guest frames for nested host->guest calls.
 * See src/rt/nested_frames.h for what this is, what it deliberately is not,
 * and the three measured hazards it exists to remove.
 *
 * Guard bands are written and checked through the HOST pointer rather than
 * MEM_W32/MEM_R32 on purpose.  They are runtime bookkeeping, not guest stores:
 * routing them through the guest store path would put 256 synthetic writes per
 * nested call into the last-writer, watchpoint and heap-watch diagnostics and
 * make those instruments lie about who wrote what.
 */

#include "recomp.h"
#include "nested_frames.h"

#include <stdio.h>
#include <string.h>

_Static_assert(SR_NESTED_FRAME_END - SR_NESTED_FRAME_BASE ==
                   SR_NESTED_FRAME_SLOTS * SR_NESTED_FRAME_STRIDE,
               "the reserved region must be exactly the slot array");
_Static_assert(SR_NESTED_FRAME_SP_OFF + SR_NESTED_FRAME_ARGSAVE +
                   SR_NESTED_FRAME_GUARD == SR_NESTED_FRAME_STRIDE,
               "argument-save area plus high guard must end exactly at the slot end");
_Static_assert(SR_NESTED_FRAME_SP_OFF > SR_NESTED_FRAME_GUARD,
               "the usable stack must sit above the low guard");
_Static_assert((SR_NESTED_FRAME_SP_OFF & 15u) == 0u,
               "the initial $sp must satisfy the o32 16-byte stack alignment");
_Static_assert((SR_NESTED_FRAME_GUARD & 3u) == 0u,
               "guard bands are checked one 32-bit word at a time");
_Static_assert(SR_NESTED_FRAME_SLOTS <= 31u,
               "the slot index must fit in the low bits of a frame handle");
_Static_assert(SR_NESTED_FRAME_MAX_DEPTH >= 1u,
               "a zero maximum depth would refuse every nested call");

#define NF_INDEX_BITS 5
#define NF_INDEX_MASK ((1u << NF_INDEX_BITS) - 1u)
#define NF_GEN_MASK   0x03FFFFFFu
#define NF_REPORT_MAX 8u

typedef struct {
    int      used;
    uint32_t owner;
    unsigned depth;
    uint32_t base;      /* slot base = SR_NESTED_FRAME_BASE + index * STRIDE */
    uint32_t gen;       /* incremented on every acquire; stale handles are rejected */
} NestedFrame;

static NestedFrame s_frames[SR_NESTED_FRAME_SLOTS];
static unsigned s_live;
static unsigned s_guard_failures;
static unsigned s_exhaustions;
static unsigned s_lifo_violations;
static unsigned s_reported_guard;
static unsigned s_reported_exhaust;
static unsigned s_reported_lifo;
static int s_region_checked;
static int s_region_ok;

/* Address-keyed so a guard band copied verbatim from a neighbouring slot -- the
 * shape a bulk guest memcpy through the region would produce -- still fails. */
static uint32_t nf_guard_word(uint32_t addr) {
    return 0x6e460000u ^ addr;
}

static void nf_guard_fill(uint32_t lo, uint32_t hi) {
    uint32_t a;
    for (a = lo; a < hi; a += 4u) {
        uint32_t w = nf_guard_word(a);
        memcpy(SR_HOST(a), &w, sizeof w);
    }
}

static int nf_guard_check(uint32_t lo, uint32_t hi, uint32_t *bad_addr, uint32_t *bad_val) {
    uint32_t a;
    for (a = lo; a < hi; a += 4u) {
        uint32_t w;
        memcpy(&w, SR_HOST(a), sizeof w);
        if (w != nf_guard_word(a)) {
            if (bad_addr) *bad_addr = a;
            if (bad_val) *bad_val = w;
            return 0;
        }
    }
    return 1;
}

/* The region is fixed at compile time, so this validates once: the arena must
 * actually cover it, and g_mem must exist before any guard byte is touched. */
static int nf_region_ready(void) {
    if (s_region_checked) return s_region_ok;
    if (!g_mem) return 0;            /* not yet initialised; re-check on the next call */
    s_region_checked = 1;
    s_region_ok = sr_guest_span_writable(SR_NESTED_FRAME_BASE,
                                         SR_NESTED_FRAME_END - SR_NESTED_FRAME_BASE);
    if (!s_region_ok)
        fprintf(stderr,
                "NESTED_FRAME: reserved region [0x%08x,0x%08x) is not inside the guest arena "
                "-- every nested host->guest call will be refused\n",
                SR_NESTED_FRAME_BASE, SR_NESTED_FRAME_END);
    return s_region_ok;
}

static int nf_make_handle(unsigned index, uint32_t gen) {
    return (int)(((gen & NF_GEN_MASK) << NF_INDEX_BITS) | (index & NF_INDEX_MASK));
}

/* Resolve a handle to its slot, rejecting a stale one (a slot that has since
 * been released and reissued) rather than corrupting the current owner. */
static NestedFrame *nf_resolve(int handle) {
    unsigned index;
    NestedFrame *f;
    if (handle < 0) return NULL;
    index = (unsigned)handle & NF_INDEX_MASK;
    if (index >= SR_NESTED_FRAME_SLOTS) return NULL;
    f = &s_frames[index];
    if (!f->used) return NULL;
    if (nf_make_handle(index, f->gen) != handle) return NULL;
    return f;
}

unsigned sr_nested_frame_owner_depth(uint32_t owner) {
    unsigned i, deepest = 0;
    for (i = 0; i < SR_NESTED_FRAME_SLOTS; i++)
        if (s_frames[i].used && s_frames[i].owner == owner && s_frames[i].depth > deepest)
            deepest = s_frames[i].depth;
    return deepest;
}

int sr_nested_frame_acquire(uint32_t owner, uint32_t *sp_out, int *handle_out) {
    unsigned depth, i;
    NestedFrame *f = NULL;
    uint32_t base, sp;

    if (!sp_out || !handle_out) return 0;
    if (!nf_region_ready()) return 0;

    depth = sr_nested_frame_owner_depth(owner) + 1u;
    if (depth > SR_NESTED_FRAME_MAX_DEPTH) {
        s_exhaustions++;
        if (s_reported_exhaust < NF_REPORT_MAX) {
            s_reported_exhaust++;
            fprintf(stderr,
                    "NESTED_FRAME: owner 0x%x already holds %u nested guest-call frames "
                    "(maximum %u) -- refusing to nest deeper\n",
                    owner, depth - 1u, (unsigned)SR_NESTED_FRAME_MAX_DEPTH);
        }
        return 0;
    }

    for (i = 0; i < SR_NESTED_FRAME_SLOTS; i++) {
        if (!s_frames[i].used) { f = &s_frames[i]; break; }
    }
    if (!f) {
        s_exhaustions++;
        if (s_reported_exhaust < NF_REPORT_MAX) {
            s_reported_exhaust++;
            fprintf(stderr,
                    "NESTED_FRAME: all %u nested guest-call frames are live (owner 0x%x, "
                    "depth %u) -- refusing the call\n",
                    (unsigned)SR_NESTED_FRAME_SLOTS, owner, depth);
        }
        return 0;
    }

    base = SR_NESTED_FRAME_BASE + i * SR_NESTED_FRAME_STRIDE;
    sp = base + SR_NESTED_FRAME_SP_OFF;
    nf_guard_fill(base, base + SR_NESTED_FRAME_GUARD);
    nf_guard_fill(base + SR_NESTED_FRAME_SP_OFF + SR_NESTED_FRAME_ARGSAVE,
                  base + SR_NESTED_FRAME_STRIDE);

    f->used = 1;
    f->owner = owner;
    f->depth = depth;
    f->base = base;
    f->gen = (f->gen + 1u) & NF_GEN_MASK;
    s_live++;

    *sp_out = sp;
    *handle_out = nf_make_handle(i, f->gen);
    return 1;
}

/* Shared by release and by the unwind paths, which must not re-run the LIFO
 * check: unwinding an owner reclaims its whole chain at once, so "this was not
 * the owner's deepest frame" is expected there and is not a violation. */
static int nf_free_slot(NestedFrame *f, int check_lifo) {
    uint32_t bad_addr = 0, bad_val = 0;
    int intact = 1;

    if (check_lifo && f->depth != sr_nested_frame_owner_depth(f->owner)) {
        s_lifo_violations++;
        if (s_reported_lifo < NF_REPORT_MAX) {
            s_reported_lifo++;
            fprintf(stderr,
                    "NESTED_FRAME: owner 0x%x released depth %u while holding depth %u "
                    "-- nested frames were not released innermost-first\n",
                    f->owner, f->depth, sr_nested_frame_owner_depth(f->owner));
        }
    }

    if (!nf_guard_check(f->base, f->base + SR_NESTED_FRAME_GUARD, &bad_addr, &bad_val) ||
        !nf_guard_check(f->base + SR_NESTED_FRAME_SP_OFF + SR_NESTED_FRAME_ARGSAVE,
                        f->base + SR_NESTED_FRAME_STRIDE, &bad_addr, &bad_val)) {
        intact = 0;
        s_guard_failures++;
        if (s_reported_guard < NF_REPORT_MAX) {
            s_reported_guard++;
            fprintf(stderr,
                    "NESTED_FRAME: guard word at 0x%08x is 0x%08x, expected 0x%08x "
                    "(owner 0x%x depth %u, frame [0x%08x,0x%08x), $sp was 0x%08x) "
                    "-- the nested callee ran off its frame\n",
                    bad_addr, bad_val, nf_guard_word(bad_addr), f->owner, f->depth,
                    f->base, f->base + SR_NESTED_FRAME_STRIDE,
                    f->base + SR_NESTED_FRAME_SP_OFF);
        }
    }

    f->used = 0;
    f->owner = 0;
    f->depth = 0;
    if (s_live) s_live--;
    return intact;
}

int sr_nested_frame_release(int handle) {
    NestedFrame *f = nf_resolve(handle);
    if (!f) {
        /* A handle that resolves to nothing is a release of something this
         * module does not own: never touch a slot on that path. */
        fprintf(stderr, "NESTED_FRAME: release of unknown or stale handle %d\n", handle);
        return 0;
    }
    return nf_free_slot(f, 1);
}

unsigned sr_nested_frame_release_owner(uint32_t owner) {
    unsigned i, freed = 0;
    for (i = 0; i < SR_NESTED_FRAME_SLOTS; i++) {
        if (s_frames[i].used && s_frames[i].owner == owner) {
            (void)nf_free_slot(&s_frames[i], 0);
            freed++;
        }
    }
    return freed;
}

unsigned sr_nested_frame_release_all(void) {
    unsigned i, freed = 0;
    for (i = 0; i < SR_NESTED_FRAME_SLOTS; i++) {
        if (s_frames[i].used) {
            (void)nf_free_slot(&s_frames[i], 0);
            freed++;
        }
    }
    return freed;
}

void sr_nested_frame_reset(void) {
    memset(s_frames, 0, sizeof s_frames);
    s_live = 0;
    s_guard_failures = 0;
    s_exhaustions = 0;
    s_lifo_violations = 0;
    s_reported_guard = 0;
    s_reported_exhaust = 0;
    s_reported_lifo = 0;
    s_region_checked = 0;
    s_region_ok = 0;
}

void sr_nested_frame_region(uint32_t *base, uint32_t *end) {
    if (base) *base = SR_NESTED_FRAME_BASE;
    if (end) *end = SR_NESTED_FRAME_END;
}

unsigned sr_nested_frame_live(void)            { return s_live; }
unsigned sr_nested_frame_guard_failures(void)  { return s_guard_failures; }
unsigned sr_nested_frame_exhaustions(void)     { return s_exhaustions; }
unsigned sr_nested_frame_lifo_violations(void) { return s_lifo_violations; }

int sr_nested_frame_handle_info(int handle, uint32_t *owner, unsigned *depth,
                                uint32_t *base, uint32_t *sp) {
    NestedFrame *f = nf_resolve(handle);
    if (!f) return 0;
    if (owner) *owner = f->owner;
    if (depth) *depth = f->depth;
    if (base) *base = f->base;
    if (sp) *sp = f->base + SR_NESTED_FRAME_SP_OFF;
    return 1;
}

int sr_nested_frame_handle_extent(int handle, uint32_t *floor, uint32_t *sp) {
    NestedFrame *f = nf_resolve(handle);
    if (!f) return 0;
    if (floor) *floor = f->base + SR_NESTED_FRAME_GUARD;
    if (sp) *sp = f->base + SR_NESTED_FRAME_SP_OFF;
    return 1;
}
