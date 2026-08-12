// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * guestmem_selftest.c - behavioral proof of the guest-memory bounds contract
 * (issue #15, and the #76 "execute, don't grep" testing philosophy).
 *
 * Compiles standalone against src/rt/recomp.h and executes real assertions on the
 * pure bounds/arithmetic helpers:
 *   - sr_inrange / sr_inrange_n      (existing scalar-access bounds)
 *   - sr_guest_span_readable/writable (bulk/parser span validation)
 *   - sr_size_add_ok / sr_size_mul_ok (overflow-safe size arithmetic)
 *
 * The core guarantee is overflow safety: a (base, size) pair whose naive sum wraps
 * uint32_t must be REJECTED, never accepted. A deterministic differential fuzz
 * compares each helper against a 64-bit-correct reference over the full 32-bit
 * range, so an overflow bug can't hide behind hand-picked cases.
 */

#include "recomp.h"

/* Must match SR_PHYS()/the arena end in recomp.h. tools/test_guestmem_c.py asserts
 * these constants still match the header so the reference cannot silently drift. */
#define ARENA_END 0x0c000000ULL
#define PHYS_MASK 0x1fffffffu

static int g_fail = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            g_fail++;                                                       \
            printf("FAIL: ");                                               \
            printf(__VA_ARGS__);                                            \
            printf("  [%s:%d]\n", __FILE__, __LINE__);                      \
        }                                                                   \
    } while (0)

/* 64-bit-correct reference: is [phys(a), phys(a)+w) fully inside the arena? */
static int ref_span_ok(uint32_t a, uint32_t w) {
    uint64_t phys = (uint64_t)(a & PHYS_MASK);
    return (phys + (uint64_t)w) <= ARENA_END;
}

static void test_scalar_bounds(void) {
    /* First byte in / out of the arena. */
    CHECK(sr_inrange(0x08000000u), "RAM base must be in range");
    CHECK(sr_inrange(0x0bffffffu), "last arena byte must be in range");
    CHECK(!sr_inrange(0x0c000000u), "one past the arena must be out of range");
    /* kseg0/kseg1 mirrors alias down to the same physical arena. */
    CHECK(sr_inrange(0x88000000u), "kseg0 mirror of RAM base must be in range");
    CHECK(sr_inrange(0xa8000000u), "kseg1 mirror of RAM base must be in range");

    /* Width-aware: the LAST byte must land inside the arena, not just the first. */
    CHECK(sr_inrange_n(0x0bfffffcu, 4), "aligned 4-byte read ending at arena end is ok");
    CHECK(!sr_inrange_n(0x0bfffffeu, 4), "4-byte read whose tail passes arena end must fail");
    CHECK(!sr_inrange_n(0x0bffffffu, 2), "2-byte read straddling arena end must fail");
}

static void test_overflow_safety(void) {
    /* The heart of #15: a base in-arena with a size so large that base+size wraps
     * uint32_t must still be rejected. A naive `phys + size <= END` would wrap and
     * wrongly accept. */
    CHECK(!sr_inrange_n(0x08000000u, 0xFFFFFFFFu), "huge width from in-arena base must be rejected");
    CHECK(!sr_guest_span_readable(0x08000000u, 0xFFFFFFFFu), "huge readable span must be rejected");
    CHECK(!sr_guest_span_writable(0x08000000u, 0xFFFFFFFFu), "huge writable span must be rejected");
    /* Exactly filling the arena from the base is the largest valid span. */
    CHECK(sr_guest_span_readable(0x08000000u, 0x04000000u), "span exactly to arena end is ok");
    CHECK(!sr_guest_span_readable(0x08000000u, 0x04000001u), "span one past arena end must fail");
}

static void test_zero_size_span(void) {
    /* A zero-size span touches nothing and is always valid, even from an address
     * that is itself out of range (the base is never dereferenced). */
    CHECK(sr_guest_span_readable(0x08000000u, 0), "zero-size span at a valid base is ok");
    CHECK(sr_guest_span_readable(0xdeadbeefu, 0), "zero-size span at an invalid base is still ok");
    CHECK(sr_guest_span_writable(0xffffffffu, 0), "zero-size writable span is always ok");
}

static void test_checked_arith(void) {
    uint32_t out = 0xAAAAAAAAu;
    CHECK(sr_size_add_ok(10u, 20u, &out) && out == 30u, "10+20 must succeed as 30");
    CHECK(sr_size_add_ok(0xFFFFFFFFu, 0u, &out) && out == 0xFFFFFFFFu, "max+0 must succeed");
    out = 0x5555u;
    CHECK(!sr_size_add_ok(0xFFFFFFFFu, 1u, &out), "max+1 must report overflow");
    CHECK(out == 0x5555u, "add overflow must leave *out unmodified");

    out = 0xAAAAAAAAu;
    CHECK(sr_size_mul_ok(0u, 0xFFFFFFFFu, &out) && out == 0u, "0*max must succeed as 0");
    CHECK(sr_size_mul_ok(0x10000u, 0x10000u, NULL) == 0, "0x10000*0x10000 must overflow");
    out = 0x1234u;
    CHECK(!sr_size_mul_ok(0x80000000u, 3u, &out), "0x80000000*3 must report overflow");
    CHECK(out == 0x1234u, "mul overflow must leave *out unmodified");
    CHECK(sr_size_mul_ok(0xFFFFu, 0x10001u, &out) && out == 0xFFFFFFFFu, "0xFFFF*0x10001 == max, no overflow");
}

static void fuzz_differential(void) {
    /* Deterministic LCG; no host RNG, so the run is reproducible. Covers the full
     * 32-bit range for both base and size, hitting near-boundary and huge values. */
    uint32_t x = 0x12345678u;
    const int iters = 400000;
    int span_mismatch = 0, add_mismatch = 0, mul_mismatch = 0;
    for (int i = 0; i < iters; i++) {
        x = x * 1103515245u + 12345u;
        uint32_t a = x;
        x = x * 1103515245u + 12345u;
        uint32_t s = x;
        /* Bias a chunk of samples toward the boundary and toward huge sizes. */
        if ((i & 7) == 0) a = 0x08000000u + (x & 0x07ffffffu);
        if ((i & 3) == 0) s = 0xFFFFFF00u + (x & 0xFFu);

        if (s != 0u) {
            int got = sr_inrange_n(a, s);
            int want = ref_span_ok(a, s);
            if (got != want) span_mismatch++;
            int gotr = sr_guest_span_readable(a, s);
            if (gotr != want) span_mismatch++;
        }

        uint32_t out = 0;
        int add_ok = sr_size_add_ok(a, s, &out);
        int add_ref = ((uint64_t)a + (uint64_t)s) <= 0xFFFFFFFFULL;
        if (add_ok != add_ref) add_mismatch++;
        if (add_ok && out != (uint32_t)(a + s)) add_mismatch++;

        int mul_ok = sr_size_mul_ok(a, s, &out);
        int mul_ref = ((uint64_t)a * (uint64_t)s) <= 0xFFFFFFFFULL;
        if (mul_ok != mul_ref) mul_mismatch++;
        if (mul_ok && out != (uint32_t)(a * s)) mul_mismatch++;
    }
    CHECK(span_mismatch == 0, "span check disagreed with 64-bit reference %d time(s)", span_mismatch);
    CHECK(add_mismatch == 0, "checked add disagreed with reference %d time(s)", add_mismatch);
    CHECK(mul_mismatch == 0, "checked mul disagreed with reference %d time(s)", mul_mismatch);
}

int main(void) {
    test_scalar_bounds();
    test_overflow_safety();
    test_zero_size_span();
    test_checked_arith();
    fuzz_differential();
    if (g_fail == 0) {
        printf("guestmem selftest: OK\n");
        return 0;
    }
    printf("guestmem selftest: %d FAILURE(S)\n", g_fail);
    return 1;
}
