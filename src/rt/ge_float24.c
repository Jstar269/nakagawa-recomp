// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge_float24.c — project-authored reference model of GE float24 arithmetic
// (issue #695).  See ge_float24.h for the behavioural contract, the
// SPEC_ASSUMPTION / SPEC_AMBIGUITY labels and the hardware-oracle plan.
//
// Every operation is expressed through one internal multi-operand summation
// (sum_terms) and one normaliser (finish).  Values are carried as
// (sign, magnitude, scale) with value = (-1)^sign * magnitude * 2^scale, so no
// host floating-point arithmetic, and therefore no host rounding mode, takes
// part in any result.

#include "ge_float24.h"

#include <math.h>

enum { CLS_ZERO, CLS_FINITE, CLS_INF, CLS_NAN };

typedef struct Unpacked {
    int cls;
    unsigned sign;
    int scale;    /* CLS_FINITE: value = sig * 2^scale */
    uint64_t sig; /* CLS_FINITE: normalised, top bit at a known position */
} Unpacked;

static int max_exp_field(const SrGeF24Policy* policy) {
    return policy->exp255 == SR_GE_F24_EXP255_FINITE ? 255 : 254;
}

int sr_gef24_policy_valid(const SrGeF24Policy* policy) {
    if (!policy) {
        return 0;
    }
    if (policy->exp255 != SR_GE_F24_EXP255_SPECIAL && policy->exp255 != SR_GE_F24_EXP255_FINITE) {
        return 0;
    }
    if (policy->overflow != SR_GE_F24_OVERFLOW_INF && policy->overflow != SR_GE_F24_OVERFLOW_SATURATE) {
        return 0;
    }
    if (policy->zero_sign != SR_GE_F24_ZERO_IEEE && policy->zero_sign != SR_GE_F24_ZERO_POSITIVE) {
        return 0;
    }
    if (policy->product != SR_GE_F24_PRODUCT_TRUNCATED && policy->product != SR_GE_F24_PRODUCT_EXACT) {
        return 0;
    }
    if (policy->compose != SR_GE_F24_COMPOSE_PV_THEN_W && policy->compose != SR_GE_F24_COMPOSE_P_THEN_VW) {
        return 0;
    }
    /* There is no Inf encoding when exponent 255 is finite. */
    if (policy->overflow == SR_GE_F24_OVERFLOW_INF && policy->exp255 == SR_GE_F24_EXP255_FINITE) {
        return 0;
    }
    return 1;
}

static SrGeF24Result invalid_result(void) {
    SrGeF24Result r;
    r.value = 0;
    r.flags = SR_GE_F24_FLAG_INVALID;
    return r;
}

static SrGeF24 encode_zero(const SrGeF24Policy* policy, unsigned sign) {
    return (policy->zero_sign == SR_GE_F24_ZERO_POSITIVE || !sign) ? 0u : SR_GE_F24_SIGN_BIT;
}

static SrGeF24 encode_inf(unsigned sign) {
    return (sign ? SR_GE_F24_SIGN_BIT : 0u) | SR_GE_F24_EXP_MASK;
}

static SrGeF24 encode_overflow(const SrGeF24Policy* policy, unsigned sign, uint32_t* flags) {
    *flags |= SR_GE_F24_FLAG_OVERFLOW;
    if (policy->overflow == SR_GE_F24_OVERFLOW_INF) {
        *flags |= SR_GE_F24_FLAG_SPECIAL;
        return encode_inf(sign);
    }
    return (sign ? SR_GE_F24_SIGN_BIT : 0u) | ((uint32_t)max_exp_field(policy) << SR_GE_F24_EXP_SHIFT) |
           SR_GE_F24_FRAC_MASK;
}

static Unpacked decode(const SrGeF24Policy* policy, SrGeF24 value, uint32_t* flags) {
    Unpacked u;
    unsigned exp_field;
    uint32_t frac;

    value &= SR_GE_F24_MASK;
    u.sign = (value & SR_GE_F24_SIGN_BIT) ? 1u : 0u;
    exp_field = (value & SR_GE_F24_EXP_MASK) >> SR_GE_F24_EXP_SHIFT;
    frac = value & SR_GE_F24_FRAC_MASK;
    u.scale = 0;
    u.sig = 0;
    if (exp_field == 0) {
        /* A3: zero, or a denormal that flushes to zero. */
        u.cls = CLS_ZERO;
        if (frac != 0) {
            *flags |= SR_GE_F24_FLAG_FLUSHED;
        }
        return u;
    }
    if (exp_field == 255 && policy->exp255 == SR_GE_F24_EXP255_SPECIAL) {
        *flags |= SR_GE_F24_FLAG_SPECIAL;
        u.cls = frac ? CLS_NAN : CLS_INF;
        return u;
    }
    u.cls = CLS_FINITE;
    u.sig = 0x8000u | frac;
    u.scale = (int)exp_field - SR_GE_F24_EXP_BIAS - (SR_GE_F24_SIG_BITS - 1);
    return u;
}

static int msb_index(uint64_t v) {
    int p = -1;
    while (v) {
        v >>= 1;
        ++p;
    }
    return p;
}

/* Normalise sign * mag * 2^scale (mag != 0) to a float24, truncating toward
   zero (A2), flushing a denormal result (A3) and applying the overflow reading. */
static SrGeF24 finish(const SrGeF24Policy* policy, unsigned sign, uint64_t mag, int scale, uint32_t* flags) {
    int p = msb_index(mag);
    uint64_t sig;
    int exp_field;

    if (p > SR_GE_F24_SIG_BITS - 1) {
        int drop = p - (SR_GE_F24_SIG_BITS - 1);
        if (mag & ((UINT64_C(1) << drop) - 1u)) {
            *flags |= SR_GE_F24_FLAG_INEXACT;
        }
        sig = mag >> drop;
    } else {
        sig = mag << ((SR_GE_F24_SIG_BITS - 1) - p);
    }
    exp_field = scale + p + SR_GE_F24_EXP_BIAS;
    if (exp_field > max_exp_field(policy)) {
        return encode_overflow(policy, sign, flags);
    }
    if (exp_field <= 0) {
        *flags |= SR_GE_F24_FLAG_FLUSHED;
        return encode_zero(policy, sign);
    }
    return (sign ? SR_GE_F24_SIGN_BIT : 0u) | ((uint32_t)exp_field << SR_GE_F24_EXP_SHIFT) |
           ((uint32_t)sig & SR_GE_F24_FRAC_MASK);
}

/* Bring a finite term to exactly `width` significand bits (top bit at width-1)
   without changing its value; only ever widens. */
static void widen(Unpacked* u, int width) {
    int p = msb_index(u->sig);
    if (p < width - 1) {
        u->sig <<= (width - 1) - p;
        u->scale -= (width - 1) - p;
    }
}

/* The single multi-operand summation behind addition and row sums (A4, A5).
   Every finite term is first widened to `width` bits.  All terms are aligned
   to the largest term's exponent, each truncated toward zero to the
   width-bit grid there, the aligned integers are summed exactly, and the sum
   is normalised once by finish(). */
static SrGeF24 sum_terms(const SrGeF24Policy* policy, Unpacked* terms, size_t n, int width, uint32_t* flags) {
    size_t i;
    int have_finite = 0, have_nan = 0, have_pos_inf = 0, have_neg_inf = 0;
    int all_zero_negative = 1;
    int top = 0;
    int64_t sum = 0;

    for (i = 0; i < n; ++i) {
        switch (terms[i].cls) {
        case CLS_NAN:
            have_nan = 1;
            break;
        case CLS_INF:
            if (terms[i].sign) {
                have_neg_inf = 1;
            } else {
                have_pos_inf = 1;
            }
            break;
        case CLS_FINITE:
            widen(&terms[i], width);
            if (!have_finite || terms[i].scale > top) {
                top = terms[i].scale;
            }
            have_finite = 1;
            break;
        default:
            if (!terms[i].sign) {
                all_zero_negative = 0;
            }
            break;
        }
    }
    if (have_nan || (have_pos_inf && have_neg_inf)) {
        *flags |= SR_GE_F24_FLAG_SPECIAL;
        return SR_GE_F24_CANONICAL_NAN;
    }
    if (have_pos_inf || have_neg_inf) {
        *flags |= SR_GE_F24_FLAG_SPECIAL;
        return encode_inf(have_neg_inf ? 1u : 0u);
    }
    if (!have_finite) {
        /* Only zeros: IEEE gives -0 only when every term is -0. */
        return encode_zero(policy, all_zero_negative ? 1u : 0u);
    }
    for (i = 0; i < n; ++i) {
        int shift;
        uint64_t aligned;
        if (terms[i].cls != CLS_FINITE) {
            continue;
        }
        shift = top - terms[i].scale;
        if (shift >= width) {
            aligned = 0; /* shifted entirely off the grid; sig is nonzero */
            *flags |= SR_GE_F24_FLAG_INEXACT;
        } else {
            aligned = terms[i].sig >> shift;
            if ((aligned << shift) != terms[i].sig) {
                *flags |= SR_GE_F24_FLAG_INEXACT;
            }
        }
        sum += terms[i].sign ? -(int64_t)aligned : (int64_t)aligned;
    }
    if (sum == 0) {
        *flags |= SR_GE_F24_FLAG_CANCELLED;
        return encode_zero(policy, 0u);
    }
    return finish(policy, sum < 0 ? 1u : 0u, (uint64_t)(sum < 0 ? -sum : sum), top, flags);
}

/* Exact product of two decoded values, as a term (32-bit significand when
   finite).  Inf * 0 is NaN; zero products take the XOR sign. */
static Unpacked product_term(Unpacked a, Unpacked b) {
    Unpacked p;
    p.sign = a.sign ^ b.sign;
    p.scale = 0;
    p.sig = 0;
    if (a.cls == CLS_NAN || b.cls == CLS_NAN) {
        p.cls = CLS_NAN;
    } else if (a.cls == CLS_INF || b.cls == CLS_INF) {
        p.cls = (a.cls == CLS_ZERO || b.cls == CLS_ZERO) ? CLS_NAN : CLS_INF;
    } else if (a.cls == CLS_ZERO || b.cls == CLS_ZERO) {
        p.cls = CLS_ZERO;
    } else {
        p.cls = CLS_FINITE;
        p.sig = a.sig * b.sig;
        p.scale = a.scale + b.scale;
    }
    return p;
}

/* Collapse a product term to a float24 (truncating, flushing, overflowing). */
static SrGeF24 encode_term(const SrGeF24Policy* policy, Unpacked t, uint32_t* flags) {
    switch (t.cls) {
    case CLS_NAN:
        *flags |= SR_GE_F24_FLAG_SPECIAL;
        return SR_GE_F24_CANONICAL_NAN;
    case CLS_INF:
        *flags |= SR_GE_F24_FLAG_SPECIAL;
        return encode_inf(t.sign);
    case CLS_ZERO:
        return encode_zero(policy, t.sign);
    default:
        return finish(policy, t.sign, t.sig, t.scale, flags);
    }
}

SrGeF24Result sr_gef24_from_binary32(const SrGeF24Policy* policy, uint32_t bits) {
    SrGeF24Result r;
    unsigned sign = (bits >> 31) & 1u;
    unsigned exp_field = (bits >> 23) & 0xFFu;
    uint32_t frac23 = bits & 0x007FFFFFu;
    if (!sr_gef24_policy_valid(policy)) {
        return invalid_result();
    }
    r.flags = 0;
    if (exp_field == 0) {
        /* A3: a binary32 denormal flushes; its discarded bits report FLUSHED only. */
        if (frac23) {
            r.flags |= SR_GE_F24_FLAG_FLUSHED;
        }
        r.value = encode_zero(policy, sign);
    } else if (exp_field == 255 && policy->exp255 == SR_GE_F24_EXP255_SPECIAL) {
        /* A NaN whose payload lives only in the dropped byte must stay a NaN. */
        r.flags |= SR_GE_F24_FLAG_SPECIAL;
        r.value = frac23 ? SR_GE_F24_CANONICAL_NAN : encode_inf(sign);
    } else {
        /* A2: drop the low 8 fraction bits (sign-magnitude truncation toward zero). */
        if (bits & 0xFFu) {
            r.flags |= SR_GE_F24_FLAG_INEXACT;
        }
        r.value = (bits >> 8) & SR_GE_F24_MASK;
    }
    return r;
}

uint32_t sr_gef24_to_binary32(SrGeF24 value) {
    return (value & SR_GE_F24_MASK) << 8;
}

double sr_gef24_to_double(const SrGeF24Policy* policy, SrGeF24 value) {
    uint32_t flags = 0;
    Unpacked u;
    if (!sr_gef24_policy_valid(policy)) {
        return NAN;
    }
    u = decode(policy, value, &flags);
    switch (u.cls) {
    case CLS_NAN:
        return NAN;
    case CLS_INF:
        return u.sign ? -INFINITY : INFINITY;
    case CLS_ZERO:
        return (u.sign && policy->zero_sign == SR_GE_F24_ZERO_IEEE) ? -0.0 : 0.0;
    default:
        return ldexp(u.sign ? -(double)u.sig : (double)u.sig, u.scale);
    }
}

SrGeF24Result sr_gef24_add(const SrGeF24Policy* policy, SrGeF24 a, SrGeF24 b) {
    SrGeF24Result r;
    Unpacked terms[2];
    if (!sr_gef24_policy_valid(policy)) {
        return invalid_result();
    }
    r.flags = 0;
    terms[0] = decode(policy, a, &r.flags);
    terms[1] = decode(policy, b, &r.flags);
    r.value = sum_terms(policy, terms, 2, SR_GE_F24_SIG_BITS, &r.flags);
    return r;
}

SrGeF24Result sr_gef24_mul(const SrGeF24Policy* policy, SrGeF24 a, SrGeF24 b) {
    SrGeF24Result r;
    Unpacked ua, ub;
    if (!sr_gef24_policy_valid(policy)) {
        return invalid_result();
    }
    r.flags = 0;
    ua = decode(policy, a, &r.flags);
    ub = decode(policy, b, &r.flags);
    r.value = encode_term(policy, product_term(ua, ub), &r.flags);
    return r;
}

SrGeF24Result sr_gef24_rowsum(const SrGeF24Policy* policy, const SrGeF24* a, const SrGeF24* b, size_t n) {
    SrGeF24Result r;
    Unpacked terms[SR_GE_F24_ROWSUM_MAX];
    size_t i;
    if (!sr_gef24_policy_valid(policy) || !a || !b || n == 0 || n > SR_GE_F24_ROWSUM_MAX) {
        return invalid_result();
    }
    r.flags = 0;
    for (i = 0; i < n; ++i) {
        Unpacked ua = decode(policy, a[i], &r.flags);
        Unpacked ub = decode(policy, b[i], &r.flags);
        terms[i] = product_term(ua, ub);
        if (policy->product == SR_GE_F24_PRODUCT_TRUNCATED) {
            /* Each product becomes a float24 before it joins the row sum. */
            terms[i] = decode(policy, encode_term(policy, terms[i], &r.flags), &r.flags);
        }
    }
    r.value = sum_terms(policy, terms, n,
                        policy->product == SR_GE_F24_PRODUCT_TRUNCATED ? SR_GE_F24_SIG_BITS : 2 * SR_GE_F24_SIG_BITS,
                        &r.flags);
    return r;
}

uint32_t sr_gef24_mat4_mul(const SrGeF24Policy* policy, const SrGeF24Mat4* lhs, const SrGeF24Mat4* rhs,
                           SrGeF24Mat4* out) {
    SrGeF24Mat4 tmp;
    uint32_t flags = 0;
    int r, c, k;
    if (!sr_gef24_policy_valid(policy) || !lhs || !rhs || !out) {
        return SR_GE_F24_FLAG_INVALID;
    }
    for (r = 0; r < 4; ++r) {
        for (c = 0; c < 4; ++c) {
            SrGeF24 col[4];
            SrGeF24Result e;
            for (k = 0; k < 4; ++k) {
                col[k] = rhs->m[k][c];
            }
            e = sr_gef24_rowsum(policy, lhs->m[r], col, 4);
            tmp.m[r][c] = e.value;
            flags |= e.flags;
        }
    }
    *out = tmp;
    return flags;
}

uint32_t sr_gef24_compose_wvp(const SrGeF24Policy* policy, const SrGeF24Mat4* world, const SrGeF24Mat4* view,
                              const SrGeF24Mat4* proj, SrGeF24Mat4* out) {
    SrGeF24Mat4 inner;
    uint32_t flags;
    if (!sr_gef24_policy_valid(policy) || !world || !view || !proj || !out) {
        return SR_GE_F24_FLAG_INVALID;
    }
    if (policy->compose == SR_GE_F24_COMPOSE_PV_THEN_W) {
        flags = sr_gef24_mat4_mul(policy, proj, view, &inner);
        flags |= sr_gef24_mat4_mul(policy, &inner, world, out);
    } else {
        flags = sr_gef24_mat4_mul(policy, view, world, &inner);
        flags |= sr_gef24_mat4_mul(policy, proj, &inner, out);
    }
    return flags;
}

uint32_t sr_gef24_mat4_apply(const SrGeF24Policy* policy, const SrGeF24Mat4* m, const SrGeF24 v[4], SrGeF24 out[4]) {
    SrGeF24 tmp[4];
    uint32_t flags = 0;
    int r;
    if (!sr_gef24_policy_valid(policy) || !m || !v || !out) {
        return SR_GE_F24_FLAG_INVALID;
    }
    for (r = 0; r < 4; ++r) {
        SrGeF24Result e = sr_gef24_rowsum(policy, m->m[r], v, 4);
        tmp[r] = e.value;
        flags |= e.flags;
    }
    for (r = 0; r < 4; ++r) {
        out[r] = tmp[r];
    }
    return flags;
}

/* ---- reciprocal framework ---------------------------------------------- */

uint32_t sr_ge_recip_table_check(const SrGeRecipTable* table) {
    int i;
    uint64_t lo, hi;
    if (!table || table->out_frac_bits < SR_GE_RECIP_FRAC_MIN || table->out_frac_bits > SR_GE_RECIP_FRAC_MAX ||
        table->slope_shift > 31) {
        return SR_GE_F24_FLAG_INVALID;
    }
    lo = UINT64_C(1) << (table->out_frac_bits - 1);
    hi = UINT64_C(1) << table->out_frac_bits;
    for (i = 0; i < SR_GE_RECIP_SEGMENTS; ++i) {
        uint64_t drop = ((uint64_t)table->slope[i] * ((1u << SR_GE_RECIP_T_BITS) - 1u)) >> table->slope_shift;
        uint64_t y0 = table->base[i];
        /* y decreases with t, so the two endpoints bound the segment. */
        if (y0 > hi || drop > y0 || y0 - drop < lo) {
            return SR_GE_F24_FLAG_INVALID;
        }
    }
    return 0;
}

unsigned sr_ge_recip_segment(SrGeF24 value, unsigned* t_out) {
    uint32_t frac = value & SR_GE_F24_FRAC_MASK;
    if (t_out) {
        *t_out = frac & ((1u << SR_GE_RECIP_T_BITS) - 1u);
    }
    return frac >> SR_GE_RECIP_T_BITS;
}

uint32_t sr_ge_recip_table_value(const SrGeRecipTable* table, SrGeF24 value) {
    unsigned t;
    unsigned i;
    if (sr_ge_recip_table_check(table)) {
        return 0;
    }
    i = sr_ge_recip_segment(value, &t);
    /* A9: truncated product; table_check guarantees no underflow. */
    return table->base[i] - (uint32_t)(((uint64_t)table->slope[i] * t) >> table->slope_shift);
}

SrGeF24Result sr_ge_recip(const SrGeF24Policy* policy, const SrGeRecipTable* table, SrGeF24 value) {
    SrGeF24Result r;
    Unpacked u;
    uint32_t y;
    int exp_field;
    if (!sr_gef24_policy_valid(policy) || sr_ge_recip_table_check(table)) {
        return invalid_result();
    }
    r.flags = 0;
    u = decode(policy, value, &r.flags);
    switch (u.cls) {
    case CLS_NAN:
        r.value = SR_GE_F24_CANONICAL_NAN;
        return r;
    case CLS_INF:
        r.value = encode_zero(policy, u.sign);
        return r;
    case CLS_ZERO:
        r.flags |= SR_GE_F24_FLAG_DIV_ZERO;
        r.value = encode_overflow(policy, u.sign, &r.flags);
        return r;
    default:
        break;
    }
    y = sr_ge_recip_table_value(table, value);
    /* value = sig * 2^scale with sig in [2^15, 2^16): 1/value =
       (y / 2^F) * 2^-(exp_field - 127), so the reciprocal's scale is
       -F - (exp_field - 127). */
    exp_field = u.scale + (SR_GE_F24_SIG_BITS - 1) + SR_GE_F24_EXP_BIAS;
    r.value = finish(policy, u.sign, y, -(int)table->out_frac_bits - (exp_field - SR_GE_F24_EXP_BIAS), &r.flags);
    return r;
}
