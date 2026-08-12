// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// Host-neutral regression for the guest code-address dispatch table (issue #45).
// Exercises the REAL production primitives from dispatch_table.h (the same code recomp.c
// wraps as sr_register/sr_lookup), not a duplicate model. The governing invariant:
//
//   guest code address 0 is a first-class key. It must register and look up exactly like
//   any other address, and occupancy must never be inferred from the key or the payload.
//
// Before the fix, sr_register(0, fn) was indistinguishable from an empty slot and the L1
// fast path was guarded by `addr != 0`, so sr_lookup(0) could never succeed. See
// scratchpad baseline_zero.c / the #45 report for the pre-fix failure.

#include "dispatch_table.h"
#include <stdio.h>
#include <stdlib.h>

static int g_failed = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL L%d: ", __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed = 1; } \
} while (0)

/* Real functions with observable side effects, so "executed the function at address 0"
 * and "swallowed a null call" are never observationally identical (issue #45 §16): the
 * pre-fix f_00000000 was a no-op, which is exactly why the defect hid. */
static volatile int g_zero_ran, g_a_ran, g_b_ran;
static void fn_zero(void) { g_zero_ran = 0x12345678; }
static void fn_a(void)    { g_a_ran++; }
static void fn_b(void)    { g_b_ran++; }
typedef void (*VoidFn)(void);

/* An address whose main-table home bucket is (0 >> 2) & MASK == 0 -- i.e. it collides with
 * address 0 at bucket 0. (addr >> 2) must be a multiple of SR_DTAB_SIZE. */
#define COLLIDES_WITH_ZERO (SR_DTAB_SIZE * 4u)   /* 0x00080000 */

int main(void) {
    SrDispatchTable *t = (SrDispatchTable *)calloc(1, sizeof *t);
    if (!t) { fprintf(stderr, "OOM\n"); return 2; }

    /* A. Basic zero registration -- the headline #45 case. */
    sr_dtab_register(t, 0x00000000u, (uintptr_t)(VoidFn)fn_zero);
    uintptr_t z = sr_dtab_lookup(t, 0x00000000u);
    CHECK(z == (uintptr_t)(VoidFn)fn_zero, "lookup(0) = 0x%jx, want fn_zero", (uintmax_t)z);

    /* A'. And it is a REAL function: calling it runs fn_zero (not a silent no-op). */
    g_zero_ran = 0;
    if (z) ((VoidFn)z)();
    CHECK(g_zero_ran == 0x12345678, "the function at address 0 did not execute");

    /* B. Unrelated nonzero addresses still map to their exact functions. */
    sr_dtab_register(t, 0x00012340u, (uintptr_t)(VoidFn)fn_a);
    sr_dtab_register(t, 0x002b76e0u, (uintptr_t)(VoidFn)fn_b);
    CHECK(sr_dtab_lookup(t, 0x00012340u) == (uintptr_t)(VoidFn)fn_a, "nonzero A lookup wrong");
    CHECK(sr_dtab_lookup(t, 0x002b76e0u) == (uintptr_t)(VoidFn)fn_b, "nonzero B lookup wrong");
    /* Address 0 still resolves after other registrations (no key aliasing). */
    CHECK(sr_dtab_lookup(t, 0x00000000u) == (uintptr_t)(VoidFn)fn_zero, "addr 0 lost after B");

    /* F. Occupancy is independent of key: an unregistered address that PROBES THROUGH the
     * occupied bucket 0 (home of key 0) must still be reported absent. This is the exact
     * spot the old sentinel got wrong -- a stored key of 0 used to terminate the probe. */
    CHECK(sr_dtab_lookup(t, COLLIDES_WITH_ZERO) == 0u,
          "collider wrongly found before it was registered");
    /* And a plain unregistered address is absent. */
    CHECK(sr_dtab_lookup(t, 0x00099998u) == 0u, "unregistered addr wrongly found");

    /* C. Hash collision that INVOLVES address zero, both registration orders. */
    {
        /* order 1: zero already registered above; add the collider. */
        sr_dtab_register(t, COLLIDES_WITH_ZERO, (uintptr_t)(VoidFn)fn_a);
        CHECK(sr_dtab_lookup(t, 0x00000000u) == (uintptr_t)(VoidFn)fn_zero,
              "addr 0 lost after collider registered");
        CHECK(sr_dtab_lookup(t, COLLIDES_WITH_ZERO) == (uintptr_t)(VoidFn)fn_a,
              "collider lost");

        /* order 2: fresh table, collider first, then zero. */
        SrDispatchTable *t2 = (SrDispatchTable *)calloc(1, sizeof *t2);
        if (!t2) { fprintf(stderr, "OOM\n"); return 2; }
        sr_dtab_register(t2, COLLIDES_WITH_ZERO, (uintptr_t)(VoidFn)fn_a);
        sr_dtab_register(t2, 0x00000000u, (uintptr_t)(VoidFn)fn_zero);
        CHECK(sr_dtab_lookup(t2, COLLIDES_WITH_ZERO) == (uintptr_t)(VoidFn)fn_a,
              "order2: collider lost");
        CHECK(sr_dtab_lookup(t2, 0x00000000u) == (uintptr_t)(VoidFn)fn_zero,
              "order2: addr 0 unfindable behind collider");
        free(t2);
    }

    /* D. Re-registration of address 0 replaces the function, like any other key. */
    sr_dtab_register(t, 0x00000000u, (uintptr_t)(VoidFn)fn_b);
    CHECK(sr_dtab_lookup(t, 0x00000000u) == (uintptr_t)(VoidFn)fn_b, "re-register(0) did not replace");
    sr_dtab_register(t, 0x00000000u, (uintptr_t)(VoidFn)fn_zero);  /* restore for later checks */

    /* E. L1 caching: the first lookup fills L1, the second is served from it. Both must
     * agree, including for address 0 (whose all-zero packed form used to read as empty). */
    for (int i = 0; i < 3; i++) {
        CHECK(sr_dtab_lookup(t, 0x00000000u) == (uintptr_t)(VoidFn)fn_zero, "addr 0 L1 pass %d", i);
        CHECK(sr_dtab_lookup(t, 0x00012340u) == (uintptr_t)(VoidFn)fn_a,    "addr A L1 pass %d", i);
    }

    /* The table is policy-free (issue #45): it maps registered address 0 to its function and
     * reports an unregistered address as absent. It does NOT decide what a runtime pointer
     * VALUE of 0 means -- whether a computed 0 is NULL, module-relative offset 0, or a future
     * image-relative target is unresolved under #45 and is the runtime's (dispatch()'s) job,
     * not this table's. So there is deliberately no "resolve computed target" test here: that
     * would encode an invalid general rule. We prove only the table semantics.
     *
     *   Case 1 -- sr_dtab_lookup(0) is the valid mapping and its function executes;
     *   absent  -- an UNREGISTERED address returns 0 and runs nothing. */
    {
        SrDispatchTable *t3 = (SrDispatchTable *)calloc(1, sizeof *t3);
        if (!t3) { fprintf(stderr, "OOM\n"); return 2; }

        /* absent: with nothing registered, lookup(0) is absent, not fn_zero. */
        g_zero_ran = 0;
        uintptr_t absent = sr_dtab_lookup(t3, 0x00000000u);
        CHECK(absent == 0u, "empty table: lookup(0) must be absent");
        if (absent) ((VoidFn)absent)();     /* must not run */
        CHECK(g_zero_ran == 0, "empty-table lookup(0) executed something");

        /* Case 1: once registered, the offset-0 mapping is valid and executes when invoked. */
        sr_dtab_register(t3, 0x00000000u, (uintptr_t)(VoidFn)fn_zero);
        uintptr_t valid = sr_dtab_lookup(t3, 0x00000000u);
        CHECK(valid == (uintptr_t)(VoidFn)fn_zero, "lookup(0) is not the registered mapping");
        if (valid) ((VoidFn)valid)();
        CHECK(g_zero_ran == 0x12345678, "the valid offset-0 mapping did not execute");
        free(t3);
    }

    /* Saturation-ish: many keys including 0 coexist and all resolve. */
    for (uint32_t a = 4u; a < 4u * 4000u; a += 4u)
        sr_dtab_register(t, a, (uintptr_t)(VoidFn)fn_a);
    CHECK(sr_dtab_lookup(t, 0x00000000u) == (uintptr_t)(VoidFn)fn_zero, "addr 0 survived bulk fill");
    CHECK(sr_dtab_lookup(t, 4u) == (uintptr_t)(VoidFn)fn_a, "first bulk key");
    CHECK(sr_dtab_lookup(t, 4u * 3999u) == (uintptr_t)(VoidFn)fn_a, "last bulk key");

    free(t);
    if (g_failed) { fprintf(stderr, "dispatch selftest: FAILED\n"); return 1; }
    printf("dispatch selftest: OK\n");
    return 0;
}
