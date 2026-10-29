// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Exercise the production sr_coro_main() implementation without HLE or the
 * SR_CORO_LIFECYCLE_TEST safety instrumentation.  Before the idempotency fix,
 * every repeated adoption allocated a fresh wrapper and changed the scheduler
 * identity; a tight caller could therefore consume unbounded host memory.
 */

#include "sr_coro.h"

#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
#include <fenv.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

static SIZE_T private_usage(void) {
    PROCESS_MEMORY_COUNTERS_EX counters;
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS *)&counters,
                              sizeof(counters))) {
        return (SIZE_T)-1;
    }
    return counters.PrivateUsage;
}
#endif

/* ---- switching: every backend, including the Android stack-switch one ----
 *
 * Two coroutines and the main one hand control round a ring. Each body runs a recursion that
 * spans the switch and keeps a value live in a register across every level; the two bodies use
 * different depths and multipliers, so a callee-saved register the backend failed to preserve
 * carries the other context's value and shows up as a wrong total. Inside a coroutine the stack
 * bounds must be known and cover the running frame, and a formatted double must print (on x86-64
 * a misaligned coroutine stack faults in the variadic prologue). Off Windows, a coroutine runs
 * with the rounding mode set to downward and main must keep round-to-nearest: both the reported
 * mode and a division's result are checked, because x86-64 fegetround reads the x87 control word
 * while float arithmetic follows MXCSR (on AArch64 both follow FPCR). glibc's swapcontext
 * restores that state, so the stack-switch backend must as well. */
#define RING_ROUNDS 2000u

typedef struct RingBody {
    SrCoro *self;
    SrCoro *next;
    unsigned depth;
    unsigned long long salt;
    unsigned rounds;
    unsigned long long total;
    int stack_ok;
    int format_ok;
    int round_ok;
} RingBody;

static SrCoro *s_ring_main;

#if defined(__GNUC__) || defined(__clang__)
#define RING_NOINLINE __attribute__((noinline))
#else
#define RING_NOINLINE
#endif

/* Per-body inputs, read through volatile loads so the compiler cannot recompute them after the
 * switch: it has to keep each value somewhere across it. The other coroutine overwrites them
 * while this one is suspended, so only the register or stack copy taken before the switch is
 * right afterwards. */
static volatile unsigned long long s_noise[16];
static volatile double s_fnoise[4];

static void ring_fill_noise(const RingBody *b, unsigned round) {
    for (unsigned i = 0; i < 16u; i++) s_noise[i] = (b->salt + i) * 0x9e3779b97f4a7c15ull + round;
    for (unsigned i = 0; i < 4u; i++) s_fnoise[i] = (double)((b->salt * (i + 3u) + round) & 0xffffu) + 0.25;
}

/* live != 0 switches at the bottom; live == 0 computes the same value with no switch. The
 * switch is called from the same frame that holds m[] and f[]: sixteen integers and four doubles,
 * more than the callee-saved general registers of x86-64 (6) or AArch64 (10) and the AArch64
 * callee-saved d8..d15, so an optimizing build keeps those registers busy across the switch. */
static RING_NOINLINE unsigned long long ring_recurse(const RingBody *b, unsigned depth,
                                                     unsigned long long acc, int live) {
    const unsigned long long here = acc * b->salt + depth;
    const unsigned long long m0 = s_noise[0] ^ here, m1 = s_noise[1] ^ here, m2 = s_noise[2] ^ here;
    const unsigned long long m3 = s_noise[3] ^ here, m4 = s_noise[4] ^ here, m5 = s_noise[5] ^ here;
    const unsigned long long m6 = s_noise[6] ^ here, m7 = s_noise[7] ^ here, m8 = s_noise[8] ^ here;
    const unsigned long long m9 = s_noise[9] ^ here, m10 = s_noise[10] ^ here;
    const unsigned long long m11 = s_noise[11] ^ here, m12 = s_noise[12] ^ here;
    const unsigned long long m13 = s_noise[13] ^ here, m14 = s_noise[14] ^ here;
    const unsigned long long m15 = s_noise[15] ^ here;
    const double f0 = s_fnoise[0], f1 = s_fnoise[1], f2 = s_fnoise[2], f3 = s_fnoise[3];
    unsigned long long r;
    if (depth == 0) {
        if (live) sr_coro_switch(b->next);
        r = here;
    } else {
        r = ring_recurse(b, depth - 1u, here, live);
    }
    r = (r ^ m0) * (m1 | 1u);
    r = (r ^ m2) * (m3 | 1u);
    r = (r ^ m4) * (m5 | 1u);
    r = (r ^ m6) * (m7 | 1u);
    r = (r ^ m8) * (m9 | 1u);
    r = (r ^ m10) * (m11 | 1u);
    r = (r ^ m12) * (m13 | 1u);
    r = (r ^ m14) * (m15 | 1u);
    /* Exact in binary64 (products of 16-bit values and quarters), so the downward rounding mode
     * the coroutines run under gives the same value as main's round-to-nearest reference. */
    r += (unsigned long long)((double)(r & 0xffu) * f0 + f1 * f2 - f3);
    return r + depth;
}

/* 1/3 in binary32: 0x3eaaaaab rounded to nearest, 0x3eaaaaaa rounded downward. */
static uint32_t ring_third_bits(void) {
    volatile float one = 1.0f;
    volatile float three = 3.0f;
    volatile float q = one / three;
    float value = q;
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void ring_body(void *arg) {
    RingBody *b = (RingBody *)arg;
    size_t remaining = 0;
    uintptr_t top = 0;
    volatile unsigned char marker = 0;
    char text[32];
    b->stack_ok = sr_coro_stack_remaining(&remaining) && remaining > 0 &&
                  remaining <= SR_GUEST_FIBER_STACK_SIZE && sr_coro_stack_top(&top) &&
                  top > (uintptr_t)&marker;
    snprintf(text, sizeof text, "%.3f", 1.5 + (double)b->rounds);
    b->format_ok = strcmp(text, "1.500") == 0;
#if !defined(_WIN32)
    b->round_ok = fesetround(FE_DOWNWARD) == 0 && ring_third_bits() == 0x3eaaaaaau;
#else
    b->round_ok = 1;
#endif
    for (;;) {
        ring_fill_noise(b, b->rounds);
        b->total += ring_recurse(b, b->depth, b->rounds, 1);
        b->rounds++;
#if !defined(_WIN32)
        if (fegetround() != FE_DOWNWARD || ring_third_bits() != 0x3eaaaaaau) b->round_ok = 0;
#endif
        if (b->rounds == RING_ROUNDS) {
            for (;;) sr_coro_switch(s_ring_main);
        }
    }
}

static unsigned long long ring_expected(const RingBody *b) {
    unsigned long long total = 0;
    for (unsigned r = 0; r < RING_ROUNDS; r++) {
        ring_fill_noise(b, r);
        total += ring_recurse(b, b->depth, r, 0);
    }
    return total;
}

static int ring_selftest(SrCoro *main_coro) {
    RingBody a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.depth = 8u;
    a.salt = 31u;
    b.depth = 5u;
    b.salt = 1000003u;
    s_ring_main = main_coro;
    a.self = sr_coro_create(ring_body, &a, SR_GUEST_FIBER_STACK_SIZE);
    b.self = sr_coro_create(ring_body, &b, SR_GUEST_FIBER_STACK_SIZE);
    if (!a.self || !b.self) {
        fprintf(stderr, "coro selftest: sr_coro_create failed\n");
        return 1;
    }
    a.next = b.self;
    b.next = main_coro;
    unsigned resumes = 0;
    while (a.rounds < RING_ROUNDS || b.rounds < RING_ROUNDS) {
        if (resumes++ > 4u * RING_ROUNDS) {
            fprintf(stderr, "coro selftest: ring did not finish (a=%u b=%u)\n", a.rounds, b.rounds);
            return 1;
        }
        sr_coro_switch(a.rounds < RING_ROUNDS ? a.self : b.self);
        if (sr_coro_current() != main_coro) {
            fprintf(stderr, "coro selftest: resumed main with the wrong current coroutine\n");
            return 1;
        }
#if !defined(_WIN32)
        if (fegetround() != FE_TONEAREST || ring_third_bits() != 0x3eaaaaabu) {
            fprintf(stderr, "coro selftest: a coroutine's rounding mode leaked into main\n");
            return 1;
        }
#endif
    }
    const unsigned long long want_a = ring_expected(&a);
    const unsigned long long want_b = ring_expected(&b);
    if (a.total != want_a || b.total != want_b) {
        fprintf(stderr, "coro selftest: state lost across switches (a=%llu want %llu, b=%llu "
                "want %llu)\n", a.total, want_a, b.total, want_b);
        return 1;
    }
    if (!a.stack_ok || !b.stack_ok || !a.format_ok || !b.format_ok || !a.round_ok || !b.round_ok) {
        fprintf(stderr, "coro selftest: in-coroutine check failed (stack %d/%d format %d/%d "
                "rounding %d/%d)\n", a.stack_ok, b.stack_ok, a.format_ok, b.format_ok,
                a.round_ok, b.round_ok);
        return 1;
    }
    sr_coro_destroy(a.self);
    sr_coro_destroy(b.self);
    printf("coro selftest: ring OK (%u rounds x 2 coroutines, state, stack bounds, alignment, "
           "rounding isolation)\n", RING_ROUNDS);
    return 0;
}


int main(void) {
    SrCoro *main_coro = sr_coro_main();
    if (!main_coro) {
        fprintf(stderr, "coro selftest: initial adoption failed\n");
        return 1;
    }

#if defined(_WIN32)
    SIZE_T private_before = private_usage();
    if (private_before == (SIZE_T)-1) {
        fprintf(stderr, "coro selftest: cannot query initial private memory\n");
        return 1;
    }
#endif

    for (unsigned i = 0; i < 1000000u; i++) {
        SrCoro *again = sr_coro_main();
        if (again != main_coro || sr_coro_current() != main_coro) {
            fprintf(stderr,
                    "coro selftest: adoption changed identity at iteration %u\n",
                    i);
            return 1;
        }
    }

#if defined(_WIN32)
    SIZE_T private_after = private_usage();
    if (private_after == (SIZE_T)-1) {
        fprintf(stderr, "coro selftest: cannot query final private memory\n");
        return 1;
    }
    SIZE_T growth = private_after >= private_before ? private_after - private_before : 0;
    if (growth > (SIZE_T)(1u << 20)) {
        fprintf(stderr,
                "coro selftest: repeated adoption grew private memory by %llu bytes\n",
                (unsigned long long)growth);
        return 1;
    }
    printf("coro selftest: private-memory growth=%llu bytes\n",
           (unsigned long long)growth);
#endif

    puts("coro selftest: OK (1000000 repeated adoptions, one identity)");
    return ring_selftest(main_coro);
}
