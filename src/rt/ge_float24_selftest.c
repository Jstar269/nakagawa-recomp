// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge_float24_selftest.c — conformance tests for the project-authored float24
// reference model (src/rt/ge_float24.c, issue #695).
//
// Expected values come from the #695 behavioural contract and first-principles
// arithmetic, never from another renderer.  Three kinds of evidence:
//   1. format checks over every normal 24-bit pattern (exponent fields 1..254,
//      plus 255 under the finite reading); zero, denormal and special patterns
//      are covered by targeted vectors;
//   2. hand-derived vectors whose expected words are constants chosen so that
//      truncation, guard-bit-less alignment and row-sum evaluation each give a
//      different answer from the obvious alternative rule;
//   3. a randomized differential against a second model of the same rules,
//      written in host double arithmetic (exact for 16- and 32-bit
//      significands) and sharing no code with the module, run under every
//      valid SrGeF24Policy reading.
// The O7 witnesses in test_matrices were found by a deterministic search over
// the module and cross-checked against an exact-rational model during review.
// None of this is PSP-hardware evidence (SPEC_ASSUMPTION (#343 oracle pending)).

#include "ge_float24.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            if (g_failures <= 40) {                                                                                    \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
                printf(__VA_ARGS__);                                                                                   \
                printf("\n");                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

/* ---- policies ----------------------------------------------------------- */

static SrGeF24Policy make_policy(int exp255, int overflow, int zero, int product, int compose) {
    SrGeF24Policy p;
    p.exp255 = (SrGeF24Exp255)exp255;
    p.overflow = (SrGeF24Overflow)overflow;
    p.zero_sign = (SrGeF24ZeroSign)zero;
    p.product = (SrGeF24Product)product;
    p.compose = (SrGeF24Compose)compose;
    return p;
}

/* The reading most tests use when a vector does not depend on the ambiguities. */
static SrGeF24Policy base_policy(void) {
    return make_policy(SR_GE_F24_EXP255_SPECIAL, SR_GE_F24_OVERFLOW_INF, SR_GE_F24_ZERO_IEEE,
                       SR_GE_F24_PRODUCT_TRUNCATED, SR_GE_F24_COMPOSE_PV_THEN_W);
}

static int all_valid_policies(SrGeF24Policy* out) {
    int n = 0, e, o, z, p, c;
    for (e = 0; e < 2; ++e) {
        for (o = 0; o < 2; ++o) {
            for (z = 0; z < 2; ++z) {
                for (p = 0; p < 2; ++p) {
                    for (c = 0; c < 2; ++c) {
                        SrGeF24Policy pol = make_policy(e, o, z, p, c);
                        if (sr_gef24_policy_valid(&pol)) {
                            out[n++] = pol;
                        }
                    }
                }
            }
        }
    }
    return n;
}

/* ---- second model: host double arithmetic -------------------------------- */

static unsigned t_field(SrGeF24 v) {
    return (v >> 15) & 0xFFu;
}

/* Exact value of a finite, normal float24 (exponent 1..254, or 255 when finite). */
static double t_val(SrGeF24 v) {
    double mag = ldexp((double)(0x8000u | (v & 0x7FFFu)), (int)t_field(v) - 142);
    return (v & 0x800000u) ? -mag : mag;
}

static SrGeF24 t_max_finite(const SrGeF24Policy* p, unsigned sign) {
    unsigned maxf = p->exp255 == SR_GE_F24_EXP255_FINITE ? 255u : 254u;
    return (sign ? 0x800000u : 0u) | (maxf << 15) | 0x7FFFu;
}

/* Truncate a nonzero double toward zero to 16 significant bits and encode it,
   applying flush and overflow exactly as the contract states. */
static SrGeF24 t_encode(const SrGeF24Policy* p, double x) {
    unsigned sign = x < 0;
    int e;
    double m = frexp(fabs(x), &e); /* |x| = m * 2^e, m in [0.5, 1) */
    unsigned sig = (unsigned)floor(ldexp(m, 16));
    int field = e - 1 + 127;
    int maxf = p->exp255 == SR_GE_F24_EXP255_FINITE ? 255 : 254;
    if (field > maxf) {
        return p->overflow == SR_GE_F24_OVERFLOW_INF ? ((sign ? 0x800000u : 0u) | 0x7F8000u) : t_max_finite(p, sign);
    }
    if (field <= 0) {
        return (sign && p->zero_sign == SR_GE_F24_ZERO_IEEE) ? 0x800000u : 0u;
    }
    return (sign ? 0x800000u : 0u) | ((unsigned)field << 15) | (sig & 0x7FFFu);
}

static double t_trunc_to_unit(double x, double unit) {
    return trunc(x / unit) * unit;
}

static SrGeF24 t_add(const SrGeF24Policy* p, SrGeF24 a, SrGeF24 b) {
    unsigned big = t_field(a) > t_field(b) ? t_field(a) : t_field(b);
    double unit = ldexp(1.0, (int)big - 142);
    double s = t_trunc_to_unit(t_val(a), unit) + t_trunc_to_unit(t_val(b), unit);
    return s == 0.0 ? 0u : t_encode(p, s);
}

static SrGeF24 t_mul(const SrGeF24Policy* p, SrGeF24 a, SrGeF24 b) {
    return t_encode(p, t_val(a) * t_val(b));
}

static SrGeF24 t_rowsum(const SrGeF24Policy* p, const SrGeF24* a, const SrGeF24* b, int n) {
    double prod[SR_GE_F24_ROWSUM_MAX];
    int top = -100000, i;
    double unit, sum = 0.0;
    for (i = 0; i < n; ++i) {
        int e;
        if (p->product == SR_GE_F24_PRODUCT_TRUNCATED) {
            prod[i] = t_val(t_mul(p, a[i], b[i]));
        } else {
            prod[i] = t_val(a[i]) * t_val(b[i]);
        }
        frexp(fabs(prod[i]), &e);
        if (e - 1 > top) {
            top = e - 1;
        }
    }
    /* grid unit: 16-bit terms for truncated products, 32-bit for exact ones */
    unit = ldexp(1.0, top - (p->product == SR_GE_F24_PRODUCT_TRUNCATED ? 15 : 31));
    for (i = 0; i < n; ++i) {
        sum += t_trunc_to_unit(prod[i], unit);
    }
    return sum == 0.0 ? 0u : t_encode(p, sum);
}

/* ---- deterministic generator -------------------------------------------- */

static uint64_t g_rng = UINT64_C(0x9E3779B97F4A7C15);

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static uint32_t rnd_frac(void) {
    uint32_t k = rnd() & 7u;
    if (k == 0) {
        return 0;
    }
    if (k == 1) {
        return 0x7FFFu;
    }
    return rnd() & 0x7FFFu;
}

static SrGeF24 rnd_f24(int exp_lo, int exp_hi) {
    unsigned field = (unsigned)(exp_lo + (int)(rnd() % (unsigned)(exp_hi - exp_lo + 1)));
    return ((rnd() & 1u) ? 0x800000u : 0u) | (field << 15) | rnd_frac();
}

/* A value whose exponent sits within +/-20 of `near`, clamped to [lo, hi]. */
static SrGeF24 rnd_near(SrGeF24 near, int lo, int hi) {
    int field = (int)t_field(near) + (int)(rnd() % 41u) - 20;
    if (field < lo) {
        field = lo;
    }
    if (field > hi) {
        field = hi;
    }
    return ((rnd() & 1u) ? 0x800000u : 0u) | ((unsigned)field << 15) | rnd_frac();
}

/* ---- tests -------------------------------------------------------------- */

static void test_policy_validation(void) {
    SrGeF24Policy p = base_policy();
    SrGeF24Result r;
    CHECK(sr_gef24_policy_valid(&p), "base policy must be valid");
    p.exp255 = SR_GE_F24_EXP255_FINITE;
    CHECK(!sr_gef24_policy_valid(&p), "Inf overflow needs an Inf encoding");
    r = sr_gef24_add(&p, SR_GE_F24_ONE, SR_GE_F24_ONE);
    CHECK(r.flags == SR_GE_F24_FLAG_INVALID, "invalid policy must fail closed, flags=%#x", r.flags);
    p = base_policy();
    p.product = (SrGeF24Product)7;
    CHECK(!sr_gef24_policy_valid(&p), "unknown product enumerator");
    CHECK(!sr_gef24_policy_valid(NULL), "NULL policy");
    p = base_policy();
    CHECK(sr_gef24_rowsum(&p, NULL, NULL, 1).flags == SR_GE_F24_FLAG_INVALID, "NULL operands");
    {
        SrGeF24 v[SR_GE_F24_ROWSUM_MAX + 1] = {0};
        CHECK(sr_gef24_rowsum(&p, v, v, 0).flags == SR_GE_F24_FLAG_INVALID, "n == 0");
        CHECK(sr_gef24_rowsum(&p, v, v, SR_GE_F24_ROWSUM_MAX + 1).flags == SR_GE_F24_FLAG_INVALID, "n too large");
    }
}

/* Every 24-bit pattern, both exponent-255 readings: conversion from binary32
   drops exactly the low byte toward zero, and widening/double reads are exact. */
static void test_format_exhaustive(void) {
    static const uint32_t lows[] = {0x00u, 0x01u, 0x7Fu, 0x80u, 0xFFu};
    SrGeF24Policy pols[2];
    int pi;
    pols[0] = base_policy();
    pols[1] = make_policy(SR_GE_F24_EXP255_FINITE, SR_GE_F24_OVERFLOW_SATURATE, SR_GE_F24_ZERO_IEEE,
                          SR_GE_F24_PRODUCT_TRUNCATED, SR_GE_F24_COMPOSE_PV_THEN_W);
    for (pi = 0; pi < 2; ++pi) {
        const SrGeF24Policy* p = &pols[pi];
        int finite255 = p->exp255 == SR_GE_F24_EXP255_FINITE;
        uint32_t v;
        int bad_conv = 0, bad_wide = 0, bad_dbl = 0;
        for (v = 0; v <= 0xFFFFFFu; ++v) {
            unsigned field = t_field(v);
            size_t k;
            if (sr_gef24_to_binary32(v) != (v << 8)) {
                ++bad_wide;
            }
            if (field == 0 || (field == 255 && !finite255)) {
                continue; /* covered by the targeted tests below */
            }
            if (sr_gef24_to_double(p, v) != t_val(v)) {
                ++bad_dbl;
            }
            for (k = 0; k < sizeof(lows) / sizeof(lows[0]); ++k) {
                SrGeF24Result r = sr_gef24_from_binary32(p, (v << 8) | lows[k]);
                uint32_t want_flags = lows[k] ? SR_GE_F24_FLAG_INEXACT : 0u;
                if (r.value != v || r.flags != want_flags) {
                    ++bad_conv;
                }
            }
        }
        CHECK(bad_conv == 0, "policy %d: %d binary32 truncations wrong", pi, bad_conv);
        CHECK(bad_wide == 0, "policy %d: %d widenings wrong", pi, bad_wide);
        CHECK(bad_dbl == 0, "policy %d: %d exact double reads wrong", pi, bad_dbl);
    }
}

static void test_conversion_vectors(void) {
    SrGeF24Policy p = base_policy();
    SrGeF24Policy pos = p;
    SrGeF24Policy fin = make_policy(SR_GE_F24_EXP255_FINITE, SR_GE_F24_OVERFLOW_SATURATE, SR_GE_F24_ZERO_IEEE,
                                    SR_GE_F24_PRODUCT_TRUNCATED, SR_GE_F24_COMPOSE_PV_THEN_W);
    SrGeF24Result r;
    uint32_t f;
    int bad = 0;
    pos.zero_sign = SR_GE_F24_ZERO_POSITIVE;

    /* A2: truncation toward zero, not round-to-nearest and not floor. */
    r = sr_gef24_from_binary32(&p, 0x3F8000FFu);
    CHECK(r.value == 0x3F8000u && r.flags == SR_GE_F24_FLAG_INEXACT, "1+255ulp32 -> %#x", r.value);
    r = sr_gef24_from_binary32(&p, 0xBF8000FFu);
    CHECK(r.value == 0xBF8000u, "negative truncates toward zero, got %#x", r.value);
    r = sr_gef24_from_binary32(&p, 0x3F800080u);
    CHECK(r.value == 0x3F8000u, "exact tie truncates, got %#x", r.value);
    r = sr_gef24_from_binary32(&p, 0x7F7FFFFFu);
    CHECK(r.value == 0x7F7FFFu && r.flags == SR_GE_F24_FLAG_INEXACT, "FLT_MAX truncates, no overflow");

    /* A3: every binary32 denormal flushes; sign follows the zero reading. */
    for (f = 1; f <= 0x7FFFFFu; f += 0x111u) {
        SrGeF24Result a = sr_gef24_from_binary32(&p, 0x80000000u | f);
        SrGeF24Result b = sr_gef24_from_binary32(&pos, 0x80000000u | f);
        SrGeF24Result c = sr_gef24_from_binary32(&p, f);
        if (a.value != 0x800000u || b.value != 0 || c.value != 0 || a.flags != SR_GE_F24_FLAG_FLUSHED ||
            c.flags != SR_GE_F24_FLAG_FLUSHED) {
            ++bad;
        }
    }
    CHECK(bad == 0, "%d binary32 denormals did not flush", bad);
    r = sr_gef24_from_binary32(&p, 0x00000080u);
    CHECK(r.value == 0 && r.flags == SR_GE_F24_FLAG_FLUSHED, "low-byte-only denormal flushes");
    r = sr_gef24_from_binary32(&p, 0x80000000u);
    CHECK(r.value == 0x800000u && r.flags == 0, "-0 keeps its sign under IEEE zero reading");
    r = sr_gef24_from_binary32(&pos, 0x80000000u);
    CHECK(r.value == 0, "-0 becomes +0 under the positive zero reading");

    /* Exponent 255: both readings. */
    r = sr_gef24_from_binary32(&p, 0xFF800000u);
    CHECK(r.value == 0xFF8000u && r.flags == SR_GE_F24_FLAG_SPECIAL, "-Inf");
    r = sr_gef24_from_binary32(&p, 0x7F800001u);
    CHECK(r.value == SR_GE_F24_CANONICAL_NAN, "NaN with payload only in the dropped byte stays NaN");
    r = sr_gef24_from_binary32(&fin, 0x7F800001u);
    CHECK(r.value == 0x7F8000u && r.flags == SR_GE_F24_FLAG_INEXACT, "finite exponent 255 truncates");
    CHECK(sr_gef24_to_double(&fin, 0x7FFFFFu) == ldexp(65535.0, 255 - 142), "finite exponent 255 value");
    CHECK(isinf(sr_gef24_to_double(&p, 0x7F8000u)), "Inf reads as Inf");
    CHECK(isnan(sr_gef24_to_double(&p, 0x7F8001u)), "NaN reads as NaN");
    CHECK(signbit(sr_gef24_to_double(&p, 0x800001u)) && sr_gef24_to_double(&p, 0x800001u) == 0.0,
          "denormal pattern reads as -0 under IEEE zero reading");
    CHECK(!signbit(sr_gef24_to_double(&pos, 0x800001u)), "denormal pattern reads as +0 under positive reading");
    CHECK(sr_gef24_from_binary32(&p, 0xFFFFFFFFu).value == SR_GE_F24_CANONICAL_NAN, "all-ones is NaN");
}

static void test_add_vectors(void) {
    SrGeF24Policy p = base_policy();
    SrGeF24Policy pos = p;
    SrGeF24Policy sat = p;
    SrGeF24Policy fin = make_policy(SR_GE_F24_EXP255_FINITE, SR_GE_F24_OVERFLOW_SATURATE, SR_GE_F24_ZERO_IEEE,
                                    SR_GE_F24_PRODUCT_TRUNCATED, SR_GE_F24_COMPOSE_PV_THEN_W);
    SrGeF24Result r;
    pos.zero_sign = SR_GE_F24_ZERO_POSITIVE;
    sat.overflow = SR_GE_F24_OVERFLOW_SATURATE;

    /* A4: 2^-16 is aligned entirely off the 16-bit grid of 1.0.  A guarded
       adder would return the exactly representable 1 - 2^-16 = 0x3F7FFF. */
    r = sr_gef24_add(&p, SR_GE_F24_ONE, 0xB78000u);
    CHECK(r.value == 0x3F8000u && (r.flags & SR_GE_F24_FLAG_INEXACT), "1 - 2^-16 -> %#x", r.value);
    r = sr_gef24_add(&p, SR_GE_F24_ONE, 0x378000u);
    CHECK(r.value == 0x3F8000u, "1 + 2^-16 -> %#x", r.value);
    /* exactly one unit in the last place survives alignment */
    r = sr_gef24_add(&p, SR_GE_F24_ONE, 0x380000u);
    CHECK(r.value == 0x3F8001u && r.flags == 0, "1 + 2^-15 -> %#x flags %#x", r.value, r.flags);
    /* aligned operand 1.5 ulp truncates to 1 ulp */
    r = sr_gef24_add(&p, SR_GE_F24_ONE, 0x384000u);
    CHECK(r.value == 0x3F8001u && r.flags == SR_GE_F24_FLAG_INEXACT, "1 + 1.5ulp -> %#x", r.value);
    /* 1 - 1.5 ulp: aligned operand truncates toward zero first (-1 ulp), so the
       sum is 1 - 2^-15, exactly 0x3F7FFE after normalisation; truncating the
       exact difference instead would give 0x3F7FFD. */
    r = sr_gef24_add(&p, SR_GE_F24_ONE, 0xB84000u);
    CHECK(r.value == 0x3F7FFEu, "1 - 1.5ulp -> %#x", r.value);

    /* A4 carry-out: 0xFFFF + 0x8000 = 0x17FFF; the dropped low bit truncates. */
    r = sr_gef24_add(&p, 0x3FFFFFu, 0x3F8000u);
    CHECK(r.value == 0x403FFFu && r.flags == SR_GE_F24_FLAG_INEXACT, "carry truncation -> %#x", r.value);
    r = sr_gef24_add(&p, 0xBFFFFFu, 0xBF8000u);
    CHECK(r.value == 0xC03FFFu, "negative carry truncates toward zero -> %#x", r.value);

    /* cancellation and renormalisation are exact */
    r = sr_gef24_add(&p, 0x3F8001u, 0xBF8000u);
    CHECK(r.value == 0x380000u && r.flags == 0, "(1+ulp) - 1 -> %#x", r.value);
    r = sr_gef24_add(&p, 0x3F8123u, 0xBF8123u);
    CHECK(r.value == 0 && r.flags == SR_GE_F24_FLAG_CANCELLED, "x - x -> +0");
    r = sr_gef24_add(&pos, 0x3F8123u, 0xBF8123u);
    CHECK(r.value == 0, "x - x -> +0 (positive reading)");

    /* signed zeros */
    CHECK(sr_gef24_add(&p, 0x800000u, 0x800000u).value == 0x800000u, "-0 + -0 = -0 (IEEE)");
    CHECK(sr_gef24_add(&pos, 0x800000u, 0x800000u).value == 0, "-0 + -0 = +0 (positive)");
    CHECK(sr_gef24_add(&p, 0x000000u, 0x800000u).value == 0, "+0 + -0 = +0");
    CHECK(sr_gef24_add(&p, 0x800000u, 0xBF8000u).value == 0xBF8000u, "-0 + -1 = -1");

    /* A3 on results: cancellation into the denormal range flushes */
    r = sr_gef24_add(&p, 0x00C000u, 0x808000u);
    CHECK(r.value == 0 && r.flags == SR_GE_F24_FLAG_FLUSHED, "1.5min - min flushes, got %#x", r.value);
    r = sr_gef24_add(&p, 0x80C000u, 0x008000u);
    CHECK(r.value == 0x800000u && r.flags == SR_GE_F24_FLAG_FLUSHED, "-(0.5min) flushes to -0 (IEEE)");
    r = sr_gef24_add(&pos, 0x80C000u, 0x008000u);
    CHECK(r.value == 0, "-(0.5min) flushes to +0 (positive)");
    /* A3 on inputs */
    r = sr_gef24_add(&p, 0x000001u, SR_GE_F24_ONE);
    CHECK(r.value == SR_GE_F24_ONE && r.flags == SR_GE_F24_FLAG_FLUSHED, "denormal operand flushes");

    /* exponent-alignment extremes */
    r = sr_gef24_add(&p, 0x7F0000u, 0x008000u);
    CHECK(r.value == 0x7F0000u && r.flags == SR_GE_F24_FLAG_INEXACT, "253-step alignment");
    r = sr_gef24_add(&p, SR_GE_F24_ONE, 0x37FFFFu);
    CHECK(r.value == 0x3F8000u, "operand just below one ulp vanishes");

    /* overflow readings */
    r = sr_gef24_add(&p, 0x7F7FFFu, 0x7F7FFFu);
    CHECK(r.value == 0x7F8000u && r.flags == (SR_GE_F24_FLAG_OVERFLOW | SR_GE_F24_FLAG_SPECIAL), "overflow -> Inf");
    r = sr_gef24_add(&sat, 0xFF7FFFu, 0xFF7FFFu);
    CHECK(r.value == 0xFF7FFFu && r.flags == SR_GE_F24_FLAG_OVERFLOW, "overflow -> -max");
    r = sr_gef24_add(&fin, 0x7F7FFFu, 0x7F7FFFu);
    CHECK(r.value == 0x7FFFFFu && r.flags == 0, "finite exponent 255 absorbs the carry, got %#x", r.value);
    r = sr_gef24_add(&fin, 0x7FFFFFu, 0x7FFFFFu);
    CHECK(r.value == 0x7FFFFFu && r.flags == SR_GE_F24_FLAG_OVERFLOW, "finite exponent 255 saturates");

    /* specials (exponent-255-special reading) */
    CHECK(sr_gef24_add(&p, 0x7F8000u, 0xFF8000u).value == SR_GE_F24_CANONICAL_NAN, "Inf - Inf = NaN");
    CHECK(sr_gef24_add(&p, 0x7F8000u, 0xFF7FFFu).value == 0x7F8000u, "Inf - max = Inf");
    CHECK(sr_gef24_add(&p, 0x7F8001u, SR_GE_F24_ONE).value == SR_GE_F24_CANONICAL_NAN, "NaN propagates");
    CHECK(sr_gef24_add(&p, 0xFF3F8000u, 0x3F8000u).value == 0x400000u, "bits above 23 are ignored");
}

static void test_mul_vectors(void) {
    SrGeF24Policy p = base_policy();
    SrGeF24Policy pos = p;
    SrGeF24Policy fin = make_policy(SR_GE_F24_EXP255_FINITE, SR_GE_F24_OVERFLOW_SATURATE, SR_GE_F24_ZERO_IEEE,
                                    SR_GE_F24_PRODUCT_TRUNCATED, SR_GE_F24_COMPOSE_PV_THEN_W);
    SrGeF24Result r;
    pos.zero_sign = SR_GE_F24_ZERO_POSITIVE;

    /* 0x8001 * 0xFFFE = 0x7FFFFFFE: the dropped 15 bits (0x7FFE) are above
       half, so truncation keeps 0x3FFFFF where round-to-nearest gives 0x400000. */
    r = sr_gef24_mul(&p, 0x3F8001u, 0x3FFFFEu);
    CHECK(r.value == 0x3FFFFFu && r.flags == SR_GE_F24_FLAG_INEXACT, "truncated product -> %#x", r.value);
    r = sr_gef24_mul(&p, 0xBF8001u, 0x3FFFFEu);
    CHECK(r.value == 0xBFFFFFu, "negative product truncates toward zero -> %#x", r.value);
    r = sr_gef24_mul(&p, 0x3FC000u, 0x400000u);
    CHECK(r.value == 0x404000u && r.flags == 0, "1.5 * 2 = 3 exactly");
    r = sr_gef24_mul(&p, 0x7F0000u, 0x400000u);
    CHECK(r.value == 0x7F8000u && (r.flags & SR_GE_F24_FLAG_OVERFLOW), "2^127 * 2 overflows");
    r = sr_gef24_mul(&fin, 0x7F0000u, 0x400000u);
    CHECK(r.value == 0x7F8000u && r.flags == 0, "2^128 is finite under the finite reading");
    r = sr_gef24_mul(&p, 0x008000u, 0x3F0000u);
    CHECK(r.value == 0 && r.flags == SR_GE_F24_FLAG_FLUSHED, "min * 0.5 flushes");
    r = sr_gef24_mul(&p, 0x808000u, 0x3F0000u);
    CHECK(r.value == 0x800000u, "-min * 0.5 flushes to -0 (IEEE)");
    CHECK(sr_gef24_mul(&p, 0x800000u, SR_GE_F24_ONE).value == 0x800000u, "-0 * 1 = -0 (IEEE)");
    CHECK(sr_gef24_mul(&p, 0x800000u, 0x800000u).value == 0, "-0 * -0 = +0");
    CHECK(sr_gef24_mul(&pos, 0x800000u, SR_GE_F24_ONE).value == 0, "-0 * 1 = +0 (positive)");
    CHECK(sr_gef24_mul(&p, 0x7F8000u, 0).value == SR_GE_F24_CANONICAL_NAN, "Inf * 0 = NaN");
    CHECK(sr_gef24_mul(&p, 0xFF8000u, 0x3F0000u).value == 0xFF8000u, "-Inf * 0.5 = -Inf");
}

/* Randomized differential against the double model, every valid policy. */
static void test_differential(void) {
    SrGeF24Policy pols[32];
    int np = all_valid_policies(pols), pi;
    CHECK(np == 24, "expected 24 valid policies, got %d", np);
    for (pi = 0; pi < np; ++pi) {
        const SrGeF24Policy* p = &pols[pi];
        int hi = p->exp255 == SR_GE_F24_EXP255_FINITE ? 255 : 254;
        int i, bad_add = 0, bad_mul = 0, bad_row = 0;
        for (i = 0; i < 40000; ++i) {
            SrGeF24 a = rnd_f24(1, hi);
            SrGeF24 b = rnd_near(a, 1, hi);
            SrGeF24Result r = sr_gef24_add(p, a, b);
            if (r.value != t_add(p, a, b)) {
                ++bad_add;
                if (bad_add == 1) {
                    printf("  add %06x + %06x: got %06x want %06x\n", a, b, r.value, t_add(p, a, b));
                }
            }
            a = rnd_f24(1, hi);
            b = rnd_f24(1, hi);
            r = sr_gef24_mul(p, a, b);
            if (r.value != t_mul(p, a, b)) {
                ++bad_mul;
            }
        }
        for (i = 0; i < 20000; ++i) {
            SrGeF24 a[SR_GE_F24_ROWSUM_MAX], b[SR_GE_F24_ROWSUM_MAX];
            int n = 1 + (int)(rnd() % SR_GE_F24_ROWSUM_MAX), k;
            SrGeF24Result r;
            a[0] = rnd_f24(100, 150);
            for (k = 0; k < n; ++k) {
                /* exponents kept in [100, 150] so no product leaves the range */
                a[k] = rnd_near(a[0], 100, 150);
                b[k] = rnd_near(a[0], 100, 150);
            }
            r = sr_gef24_rowsum(p, a, b, (size_t)n);
            if (r.value != t_rowsum(p, a, b, n)) {
                ++bad_row;
                if (bad_row == 1) {
                    printf("  rowsum n=%d product=%d: got %06x want %06x\n", n, (int)p->product, r.value,
                           t_rowsum(p, a, b, n));
                }
            }
        }
        CHECK(bad_add == 0, "policy %d: %d adds disagree with the model", pi, bad_add);
        CHECK(bad_mul == 0, "policy %d: %d products disagree with the model", pi, bad_mul);
        CHECK(bad_row == 0, "policy %d: %d row sums disagree with the model", pi, bad_row);
    }
}

static SrGeF24 chain(const SrGeF24Policy* p, const SrGeF24* a, const SrGeF24* b, int n) {
    SrGeF24 acc = sr_gef24_mul(p, a[0], b[0]).value;
    int i;
    for (i = 1; i < n; ++i) {
        acc = sr_gef24_add(p, acc, sr_gef24_mul(p, a[i], b[i]).value).value;
    }
    return acc;
}

static void test_rowsum_model(void) {
    SrGeF24Policy tr = base_policy();
    SrGeF24Policy ex = tr;
    /* 2^20, 1, -2^20, 1 */
    const SrGeF24 row[4] = {0x498000u, SR_GE_F24_ONE, 0xC98000u, SR_GE_F24_ONE};
    const SrGeF24 ones[4] = {SR_GE_F24_ONE, SR_GE_F24_ONE, SR_GE_F24_ONE, SR_GE_F24_ONE};
    const SrGeF24 u[2] = {0x3F8001u, 0x3F8001u};
    const SrGeF24 w[2] = {0x3F8001u, 0xBF8000u};
    SrGeF24Result r;
    int perm[24][4], np = 0, a, b, c, d, i, bad = 0, chain_varies = 0;
    ex.product = SR_GE_F24_PRODUCT_EXACT;

    /* A5 vector O5: truncated-product row sum 0, exact-product row sum 2, chain 1. */
    r = sr_gef24_rowsum(&tr, row, ones, 4);
    CHECK(r.value == 0 && (r.flags & SR_GE_F24_FLAG_CANCELLED), "O5 truncated row sum -> %#x", r.value);
    r = sr_gef24_rowsum(&ex, row, ones, 4);
    CHECK(r.value == 0x400000u, "O5 exact row sum -> %#x", r.value);
    CHECK(chain(&tr, row, ones, 4) == SR_GE_F24_ONE, "O5 sequential chain -> %#x", chain(&tr, row, ones, 4));

    /* O6: truncated products 0x3F8002 + 0xBF8001 = 2^-15; exact products keep
       the 2^-30 term: 2^-15 + 2^-30 = 0x380001. */
    r = sr_gef24_rowsum(&tr, u, w, 2);
    CHECK(r.value == 0x380000u, "O6 truncated products -> %#x", r.value);
    r = sr_gef24_rowsum(&ex, u, w, 2);
    CHECK(r.value == 0x380001u, "O6 exact products -> %#x", r.value);

    /* Special terms, the full 8-term width, overflow and signed zeros. */
    {
        SrGeF24Policy pos = tr;
        SrGeF24Policy fin = make_policy(SR_GE_F24_EXP255_FINITE, SR_GE_F24_OVERFLOW_SATURATE, SR_GE_F24_ZERO_IEEE,
                                        SR_GE_F24_PRODUCT_EXACT, SR_GE_F24_COMPOSE_PV_THEN_W);
        const SrGeF24 inf_row[2] = {0x7F8000u, SR_GE_F24_ONE};
        const SrGeF24 infs[2] = {0x7F8000u, 0xFF8000u};
        const SrGeF24 nan_row[2] = {0x7FC000u, SR_GE_F24_ONE};
        const SrGeF24 one_one[2] = {SR_GE_F24_ONE, SR_GE_F24_ONE};
        const SrGeF24 zero_one[2] = {0, SR_GE_F24_ONE};
        const SrGeF24 big[2] = {0x7F0000u, 0x7F0000u}; /* 2^127 */
        const SrGeF24 big126[2] = {0x7E8000u, 0x7E8000u};
        const SrGeF24 big_pm[2] = {0x7F0000u, 0xFF0000u}; /* 2^127, -2^127 */
        const SrGeF24 two[2] = {0x400000u, 0x400000u};
        const SrGeF24 negz[2] = {0x800000u, 0x800000u};
        SrGeF24 eight_a[8], eight_b[8];
        int j;
        pos.zero_sign = SR_GE_F24_ZERO_POSITIVE;
        CHECK(sr_gef24_rowsum(&tr, inf_row, one_one, 2).value == 0x7F8000u, "Inf + 1 row -> Inf");
        CHECK(sr_gef24_rowsum(&ex, infs, one_one, 2).value == SR_GE_F24_CANONICAL_NAN, "Inf - Inf row -> NaN");
        CHECK(sr_gef24_rowsum(&tr, inf_row, zero_one, 2).value == SR_GE_F24_CANONICAL_NAN, "Inf * 0 term -> NaN");
        CHECK(sr_gef24_rowsum(&ex, nan_row, one_one, 2).value == SR_GE_F24_CANONICAL_NAN, "NaN term -> NaN");
        for (j = 0; j < 8; ++j) {
            eight_a[j] = 0x3FFFFFu;
            eight_b[j] = SR_GE_F24_ONE;
        }
        /* 8 * 0xFFFF on the 2^-15 grid is 0x7FFF8: exact, 15.99976 = 0x417FFF */
        r = sr_gef24_rowsum(&tr, eight_a, eight_b, 8);
        CHECK(r.value == 0x417FFFu && r.flags == 0, "8-term row sum -> %#x flags %#x", r.value, r.flags);
        r = sr_gef24_rowsum(&ex, eight_a, eight_b, 8);
        CHECK(r.value == 0x417FFFu && r.flags == 0, "8-term exact row sum -> %#x", r.value);
        /* each product is 2^128: both readings overflow (exact products in the
           2^129 sum, truncated products per term) */
        r = sr_gef24_rowsum(&ex, big, two, 2);
        CHECK(r.value == 0x7F8000u && (r.flags & SR_GE_F24_FLAG_OVERFLOW), "exact-product overflow -> Inf");
        r = sr_gef24_rowsum(&tr, big, two, 2);
        CHECK(r.value == 0x7F8000u && (r.flags & SR_GE_F24_FLAG_OVERFLOW), "truncated-product overflow -> Inf");
        /* separating vector: 2^128 - 2^128.  Exact products cancel to +0
           before any range check; truncated products are +Inf and -Inf per
           term, whose sum is NaN (A11). */
        r = sr_gef24_rowsum(&ex, big_pm, two, 2);
        CHECK(r.value == 0 && r.flags == SR_GE_F24_FLAG_CANCELLED, "exact products cancel -> %#x flags %#x", r.value,
              r.flags);
        r = sr_gef24_rowsum(&tr, big_pm, two, 2);
        CHECK(r.value == SR_GE_F24_CANONICAL_NAN && (r.flags & SR_GE_F24_FLAG_OVERFLOW),
              "truncated products overflow per term -> %#x", r.value);
        r = sr_gef24_rowsum(&fin, big126, two, 2);
        CHECK(r.value == 0x7F8000u && r.flags == 0, "2^128 is finite under the finite reading, got %#x", r.value);
        r = sr_gef24_rowsum(&ex, big126, two, 2);
        CHECK(r.value == 0x7F8000u && (r.flags & SR_GE_F24_FLAG_OVERFLOW), "2^128 overflows exponent 254");
        CHECK(sr_gef24_rowsum(&tr, negz, one_one, 2).value == 0x800000u, "(-0)(1) + (-0)(1) = -0 (IEEE)");
        CHECK(sr_gef24_rowsum(&pos, negz, one_one, 2).value == 0, "(-0)(1) + (-0)(1) = +0 (positive)");
        CHECK(sr_gef24_rowsum(&ex, negz, negz, 2).value == 0, "(-0)(-0) + (-0)(-0) = +0");
    }

    /* A row sum is invariant under every permutation of its terms; a chain is not. */
    for (a = 0; a < 4; ++a) {
        for (b = 0; b < 4; ++b) {
            for (c = 0; c < 4; ++c) {
                for (d = 0; d < 4; ++d) {
                    if (a != b && a != c && a != d && b != c && b != d && c != d) {
                        perm[np][0] = a;
                        perm[np][1] = b;
                        perm[np][2] = c;
                        perm[np][3] = d;
                        ++np;
                    }
                }
            }
        }
    }
    for (i = 0; i < 3000; ++i) {
        SrGeF24 x[4], y[4], first_tr = 0, first_ex = 0, first_chain = 0;
        int k, j;
        x[0] = rnd_f24(110, 140);
        for (k = 0; k < 4; ++k) {
            x[k] = rnd_near(x[0], 110, 140);
            y[k] = rnd_near(x[0], 110, 140);
        }
        for (j = 0; j < np; ++j) {
            SrGeF24 px[4], py[4], vt, ve, vc;
            for (k = 0; k < 4; ++k) {
                px[k] = x[perm[j][k]];
                py[k] = y[perm[j][k]];
            }
            vt = sr_gef24_rowsum(&tr, px, py, 4).value;
            ve = sr_gef24_rowsum(&ex, px, py, 4).value;
            vc = chain(&tr, px, py, 4);
            if (j == 0) {
                first_tr = vt;
                first_ex = ve;
                first_chain = vc;
            } else {
                if (vt != first_tr || ve != first_ex) {
                    ++bad;
                }
                if (vc != first_chain) {
                    chain_varies = 1;
                }
            }
        }
    }
    CHECK(np == 24, "24 permutations");
    CHECK(bad == 0, "%d row sums changed under permutation", bad);
    CHECK(chain_varies, "sequential chains never changed under permutation: the comparison has no teeth");
}

static void set_identity(SrGeF24Mat4* m) {
    int r, c;
    for (r = 0; r < 4; ++r) {
        for (c = 0; c < 4; ++c) {
            m->m[r][c] = r == c ? SR_GE_F24_ONE : 0u;
        }
    }
}

static void set_diag(SrGeF24Mat4* m, SrGeF24 d) {
    set_identity(m);
    m->m[0][0] = d;
}

static void test_matrices(void) {
    SrGeF24Policy p = base_policy();
    SrGeF24Policy q = p;
    SrGeF24Mat4 I, W, V, P, M1, M2;
    SrGeF24 v[4], out[4];
    uint32_t flags;
    int i, k, found_order = 0, found_seq = 0, bad = 0;
    q.compose = SR_GE_F24_COMPOSE_P_THEN_VW;

    set_identity(&I);
    flags = sr_gef24_compose_wvp(&p, &I, &I, &I, &M1);
    CHECK(flags == 0 && memcmp(&M1, &I, sizeof I) == 0, "I*I*I = I exactly, flags %#x", flags);
    for (i = 0; i < 2000; ++i) {
        for (k = 0; k < 4; ++k) {
            v[k] = rnd_f24(1, 254);
        }
        flags = sr_gef24_mat4_apply(&p, &I, v, out);
        if (flags != 0 || memcmp(v, out, sizeof v) != 0) {
            ++bad;
        }
    }
    CHECK(bad == 0, "%d identity transforms were not exact", bad);
    CHECK(sr_gef24_mat4_mul(&p, &I, NULL, &M1) == SR_GE_F24_FLAG_INVALID, "NULL matrix fails closed");

    /* aliasing: out == input */
    set_diag(&W, 0x400000u);
    M1 = W;
    sr_gef24_mat4_mul(&p, &M1, &M1, &M1);
    CHECK(M1.m[0][0] == 0x408000u && M1.m[1][1] == SR_GE_F24_ONE, "aliased square of diag(2)");
    v[0] = SR_GE_F24_ONE;
    v[1] = 0x400000u;
    v[2] = 0;
    v[3] = SR_GE_F24_ONE;
    sr_gef24_mat4_apply(&p, &W, v, v);
    CHECK(v[0] == 0x400000u && v[1] == 0x400000u && v[3] == SR_GE_F24_ONE, "aliased apply");

    /* A6 + compose-order ambiguity: find diagonal W, V, P whose two
       associations differ, and a vertex where the composed matrix differs from
       applying W, V and P one after another.  Both readings are exercised. */
    for (i = 0; i < 20000 && !(found_order && found_seq); ++i) {
        SrGeF24 vs[4], t1[4], t2[4];
        set_diag(&W, rnd_f24(120, 134));
        set_diag(&V, rnd_f24(120, 134));
        set_diag(&P, rnd_f24(120, 134));
        sr_gef24_compose_wvp(&p, &W, &V, &P, &M1);
        sr_gef24_compose_wvp(&q, &W, &V, &P, &M2);
        if (!found_order && M1.m[0][0] != M2.m[0][0]) {
            found_order = 1;
            printf("  compose-order witness: W=%06x V=%06x P=%06x -> (PV)W=%06x P(VW)=%06x\n", W.m[0][0], V.m[0][0],
                   P.m[0][0], M1.m[0][0], M2.m[0][0]);
        }
        vs[0] = rnd_f24(120, 134);
        vs[1] = vs[2] = 0;
        vs[3] = SR_GE_F24_ONE;
        sr_gef24_mat4_apply(&p, &M1, vs, t1);
        sr_gef24_mat4_apply(&p, &W, vs, t2);
        sr_gef24_mat4_apply(&p, &V, t2, t2);
        sr_gef24_mat4_apply(&p, &P, t2, t2);
        if (!found_seq && t1[0] != t2[0]) {
            found_seq = 1;
            printf("  composed-vs-sequential witness: W=%06x V=%06x P=%06x v=%06x -> %06x vs %06x\n", W.m[0][0],
                   V.m[0][0], P.m[0][0], vs[0], t1[0], t2[0]);
        }
    }
    CHECK(found_order, "no triple separates the two composition orders");
    CHECK(found_seq, "no case separates composition from sequential application");

    /* The same witnesses as fixed vectors (oracle cell O7). */
    set_diag(&W, 0x3D82E9u);
    set_diag(&V, 0x42F07Au);
    set_diag(&P, 0x3D5A83u);
    sr_gef24_compose_wvp(&p, &W, &V, &P, &M1);
    sr_gef24_compose_wvp(&q, &W, &V, &P, &M2);
    CHECK(M1.m[0][0] == 0x3ED1ECu && M2.m[0][0] == 0x3ED1EDu, "O7 order witness: %06x %06x", M1.m[0][0],
          M2.m[0][0]);
    set_diag(&W, 0xC07FFFu);
    set_diag(&V, 0x40E295u);
    set_diag(&P, 0x400000u);
    sr_gef24_compose_wvp(&p, &W, &V, &P, &M1);
    v[0] = 0x420185u;
    v[1] = v[2] = 0;
    v[3] = SR_GE_F24_ONE;
    sr_gef24_mat4_apply(&p, &M1, v, out);
    CHECK(out[0] == 0xC4E544u, "O7 composed witness: %06x", out[0]);
    sr_gef24_mat4_apply(&p, &W, v, out);
    sr_gef24_mat4_apply(&p, &V, out, out);
    sr_gef24_mat4_apply(&p, &P, out, out);
    CHECK(out[0] == 0xC4E543u, "O7 sequential witness: %06x", out[0]);

    /* Translation column: row sum of (x, y, z, 1) against (1, 0, 0, tx). */
    set_identity(&W);
    W.m[0][3] = 0x408000u; /* tx = 4 */
    v[0] = 0xB78000u;      /* -2^-16, aligned away against 4.0 */
    v[1] = v[2] = 0;
    v[3] = SR_GE_F24_ONE;
    sr_gef24_mat4_apply(&p, &W, v, out);
    CHECK(out[0] == 0x408000u, "x + tx aligns x off the grid -> %#x", out[0]);
}

/* ---- reciprocal framework ----------------------------------------------- */

/* SYNTHETIC test table: chords of 1/m between segment endpoints, F = 24.
   These are NOT PSP coefficients; they exist only to exercise the framework. */
static void synthetic_chord_table(SrGeRecipTable* t) {
    int i;
    t->out_frac_bits = 24;
    t->slope_shift = 8;
    for (i = 0; i < SR_GE_RECIP_SEGMENTS; ++i) {
        uint32_t y0 = (uint32_t)floor(ldexp(1.0, 24) / (1.0 + i / 128.0));
        uint32_t y1 = (uint32_t)floor(ldexp(1.0, 24) / (1.0 + (i + 1) / 128.0));
        t->base[i] = y0;
        t->slope[i] = y0 - y1;
    }
}

static void test_recip(void) {
    SrGeF24Policy p = base_policy();
    SrGeF24Policy sat = p;
    SrGeF24Policy fin = make_policy(SR_GE_F24_EXP255_FINITE, SR_GE_F24_OVERFLOW_SATURATE, SR_GE_F24_ZERO_IEEE,
                                    SR_GE_F24_PRODUCT_TRUNCATED, SR_GE_F24_COMPOSE_PV_THEN_W);
    SrGeRecipTable t, bad;
    SrGeF24Result r;
    uint32_t f, prev = 0xFFFFFFFFu;
    unsigned e;
    int bad_idx = 0, bad_mono = 0, bad_err = 0, bad_knot = 0, bad_sign = 0, bad_exp = 0;
    double max_err = 0.0;
    sat.overflow = SR_GE_F24_OVERFLOW_SATURATE;
    synthetic_chord_table(&t);

    CHECK(sr_ge_recip_table_check(&t) == 0, "synthetic table must validate");
    bad = t;
    bad.out_frac_bits = 15;
    CHECK(sr_ge_recip_table_check(&bad) == SR_GE_F24_FLAG_INVALID, "F below 16");
    bad.out_frac_bits = 31;
    CHECK(sr_ge_recip_table_check(&bad) == SR_GE_F24_FLAG_INVALID, "F above 30");
    bad = t;
    bad.slope_shift = 32;
    CHECK(sr_ge_recip_table_check(&bad) == SR_GE_F24_FLAG_INVALID, "shift above 31");
    bad = t;
    bad.base[5] = (1u << 24) + 1u;
    CHECK(sr_ge_recip_table_check(&bad) == SR_GE_F24_FLAG_INVALID, "base above 1.0");
    bad = t;
    bad.slope[127] = bad.base[127]; /* endpoint falls below 0.5 */
    CHECK(sr_ge_recip_table_check(&bad) == SR_GE_F24_FLAG_INVALID, "endpoint below 0.5");
    bad = t;
    bad.slope[0] = 0xFFFFFFFFu;
    bad.slope_shift = 0;
    CHECK(sr_ge_recip_table_check(&bad) == SR_GE_F24_FLAG_INVALID, "slope underflows base");
    CHECK(sr_ge_recip(&p, &bad, SR_GE_F24_ONE).flags == SR_GE_F24_FLAG_INVALID, "bad table fails closed");
    CHECK(sr_ge_recip(&p, NULL, SR_GE_F24_ONE).flags == SR_GE_F24_FLAG_INVALID, "NULL table fails closed");

    /* A8: index = top 7 fraction bits, t = low 8, for every fraction. */
    for (f = 0; f <= 0x7FFFu; ++f) {
        unsigned tt = 999, idx = sr_ge_recip_segment(0x3F8000u | f, &tt);
        if (idx != (f >> 8) || tt != (f & 0xFFu)) {
            ++bad_idx;
        }
    }
    CHECK(bad_idx == 0, "%d segment indices wrong", bad_idx);
    {
        unsigned tt;
        CHECK(sr_ge_recip_segment(0x3F80FFu, &tt) == 0 && tt == 255, "last position of segment 0");
        CHECK(sr_ge_recip_segment(0x3F8100u, &tt) == 1 && tt == 0, "first position of segment 1");
        CHECK(sr_ge_recip_segment(0xBFFFFFu, &tt) == 127 && tt == 255, "last segment, sign ignored");
        CHECK(sr_ge_recip_segment(0x3F8000u, NULL) == 0, "t_out is optional");
    }

    /* A9: knots reproduce the base exactly; y is non-increasing; the chord
       table stays within its analytic error bound: chord error <= h^2/8 *
       max|(1/m)''| = 2^-14 / 8 * 2 = 2^-16, plus floor/truncation <= 2^-23. */
    for (f = 0; f <= 0x7FFFu; ++f) {
        uint32_t y = sr_ge_recip_table_value(&t, 0x3F8000u | f);
        double m = 1.0 + f / 32768.0;
        double err = fabs(ldexp((double)y, -24) - 1.0 / m);
        if ((f & 0xFFu) == 0 && y != t.base[f >> 8]) {
            ++bad_knot;
        }
        if (y > prev) {
            ++bad_mono;
        }
        prev = y;
        if (err > max_err) {
            max_err = err;
        }
        if (err > ldexp(1.0, -16) + ldexp(1.0, -23)) {
            ++bad_err;
        }
    }
    CHECK(bad_knot == 0, "%d knots differ from their base", bad_knot);
    CHECK(bad_mono == 0, "%d increases in a decreasing table", bad_mono);
    CHECK(bad_err == 0, "%d values exceed the chord error bound (max %.3g)", bad_err, max_err);

    /* Normalisation and exponent mapping. */
    r = sr_ge_recip(&p, &t, SR_GE_F24_ONE);
    CHECK(r.value == SR_GE_F24_ONE && r.flags == 0, "1/1 = 1 (y == 2^F path), got %#x", r.value);
    CHECK(sr_ge_recip(&p, &t, 0x400000u).value == 0x3F0000u, "1/2 = 0.5");
    CHECK(sr_ge_recip(&p, &t, 0x3F0000u).value == 0x400000u, "1/0.5 = 2");
    CHECK(sr_ge_recip(&p, &t, 0xC08000u).value == 0xBE8000u, "1/-4 = -0.25");
    /* y = floor(2^24 / 1.5) = 0xAAAAAA -> significand 0xAAAA, exponent 126 */
    r = sr_ge_recip(&p, &t, 0x3FC000u);
    CHECK(r.value == 0x3F2AAAu && r.flags == SR_GE_F24_FLAG_INEXACT, "1/1.5 -> %#x", r.value);
    for (e = 1; e <= 254; ++e) {
        SrGeF24Result rr = sr_ge_recip(&p, &t, e << 15);
        unsigned want = e <= 253 ? (254u - e) << 15 : 0u;
        if (rr.value != want || (e == 254 && rr.flags != SR_GE_F24_FLAG_FLUSHED)) {
            ++bad_exp;
        }
    }
    CHECK(bad_exp == 0, "%d power-of-two reciprocals wrong", bad_exp);
    CHECK(sr_ge_recip(&fin, &t, 0x7F8000u).value == 0 && sr_ge_recip(&fin, &t, 0x7F8000u).flags == SR_GE_F24_FLAG_FLUSHED,
          "1/2^128 flushes under the finite reading");
    for (f = 0; f <= 0x7FFFu; ++f) {
        SrGeF24Result pos = sr_ge_recip(&p, &t, 0x408000u | f);
        SrGeF24Result neg = sr_ge_recip(&p, &t, 0xC08000u | f);
        if (neg.value != (pos.value | 0x800000u) || neg.flags != pos.flags) {
            ++bad_sign;
        }
    }
    CHECK(bad_sign == 0, "%d reciprocals not sign-symmetric", bad_sign);

    /* Zero, Inf, NaN under each reading. */
    r = sr_ge_recip(&p, &t, 0);
    CHECK(r.value == 0x7F8000u &&
              r.flags == (SR_GE_F24_FLAG_DIV_ZERO | SR_GE_F24_FLAG_OVERFLOW | SR_GE_F24_FLAG_SPECIAL),
          "1/0 = Inf, flags %#x", r.flags);
    CHECK(sr_ge_recip(&p, &t, 0x800000u).value == 0xFF8000u, "1/-0 = -Inf");
    r = sr_ge_recip(&sat, &t, 0x800000u);
    CHECK(r.value == 0xFF7FFFu && (r.flags & SR_GE_F24_FLAG_DIV_ZERO), "1/-0 saturates");
    CHECK(sr_ge_recip(&fin, &t, 0).value == 0x7FFFFFu, "1/0 saturates to finite exponent 255");
    CHECK(sr_ge_recip(&p, &t, 0x000123u).flags == (SR_GE_F24_FLAG_FLUSHED | SR_GE_F24_FLAG_DIV_ZERO |
                                                    SR_GE_F24_FLAG_OVERFLOW | SR_GE_F24_FLAG_SPECIAL),
          "1/denormal = 1/0");
    CHECK(sr_ge_recip(&p, &t, 0xFF8000u).value == 0x800000u, "1/-Inf = -0");
    CHECK(sr_ge_recip(&p, &t, 0x7F8001u).value == SR_GE_F24_CANONICAL_NAN, "1/NaN = NaN");
}

int main(void) {
    test_policy_validation();
    test_format_exhaustive();
    test_conversion_vectors();
    test_add_vectors();
    test_mul_vectors();
    test_differential();
    test_rowsum_model();
    test_matrices();
    test_recip();
    if (g_failures) {
        printf("GE_FLOAT24_SELFTEST FAIL failures=%d checks=%d\n", g_failures, g_checks);
        return 1;
    }
    printf("GE_FLOAT24_SELFTEST PASS checks=%d\n", g_checks);
    return 0;
}
