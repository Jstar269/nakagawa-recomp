// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Verification-only binary32 reference evaluator for the scalar cosim oracle.
 *
 * Public contract: MIPS32 Architecture for Programmers, Volume II, revision
 * 2.62, Chapter 3 instruction sections ADD.fmt, SUB.fmt, MUL.fmt, DIV.fmt,
 * SQRT.fmt, ABS.fmt, MOV.fmt, NEG.fmt, ROUND.fmt, TRUNC.fmt, CEIL.fmt,
 * FLOOR.fmt, CVT.S.W, CVT.W.S, C.cond.fmt, BC1T, and BC1F; Volume I's COP1 FCSR
 * RM/FS definitions govern directed rounding and result flushing. The routines
 * below operate only on IEEE-754 binary32 words and integer bit arrays. They do
 * not include fp_convert.h, call an sr_fpu_* helper, use a host floating-point
 * expression, or consult the host rounding environment.
 *
 * PSP-specific evidence already in this repository is deliberately narrow:
 * src/rt/fp_convert.h cites PSPAutotests commit
 * ea71108f00933712c4662276261b39cd42249b1e for conversion non-finite results,
 * and the captured fpu.expected anchors for RM, FCC0 at FCR31 bit 23, and the
 * FS result-flush gate. src/rt/fp_convert_selftest.c fixes the observable
 * conversion words, gradual subnormal inputs, and min-normal*0.5 FS result;
 * tools/test_codegen_fp_convert.py pins generated AOT compare/branch and
 * cvt.s.w results. Mul.s infinity*zero is the repository's explicit canonical
 * qNaN result, not a host-NaN assumption.
 *
 * Exact NaN payload propagation, signaling-NaN quieting, FS behavior for an
 * exact subnormal result, and COP1 exception/flag/enable behavior have no PSP
 * measurement in that evidence. Such cases return EXPECTED_UNKNOWN for #312
 * and are excluded from pass/fail comparison rather than guessed.
 */

#include "fpu_reference.h"

#include <string.h>

#define REF_BIG_LIMBS 16u
#define REF_BIG_BITS (REF_BIG_LIMBS * 32u)
#define REF_FS 0x01000000u

typedef struct RefBig {
    uint32_t limb[REF_BIG_LIMBS];
} RefBig;

typedef struct RefFinite {
    uint32_t sign;
    uint32_t significand;
    int exponent;
} RefFinite;

static void ref_status(enum FpuReferenceStatus *status,
                       enum FpuReferenceStatus value) {
    if (status != NULL) {
        *status = value;
    }
}

static void big_zero(RefBig *value) {
    memset(value, 0, sizeof *value);
}

static void big_set_u64(RefBig *value, uint64_t integer) {
    big_zero(value);
    value->limb[0] = (uint32_t)integer;
    value->limb[1] = (uint32_t)(integer >> 32);
}

static unsigned big_bit(const RefBig *value, unsigned bit) {
    if (bit >= REF_BIG_BITS) {
        return 0u;
    }
    return (value->limb[bit / 32u] >> (bit % 32u)) & 1u;
}

static int big_any_below(const RefBig *value, unsigned count) {
    unsigned whole = count / 32u;
    unsigned partial = count % 32u;
    for (unsigned i = 0u; i < whole && i < REF_BIG_LIMBS; i++) {
        if (value->limb[i] != 0u) {
            return 1;
        }
    }
    if (partial != 0u && whole < REF_BIG_LIMBS &&
        (value->limb[whole] & ((1u << partial) - 1u)) != 0u) {
        return 1;
    }
    return 0;
}

static int big_is_zero(const RefBig *value) {
    for (unsigned i = 0u; i < REF_BIG_LIMBS; i++) {
        if (value->limb[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int big_high_bit(const RefBig *value) {
    for (unsigned i = REF_BIG_LIMBS; i-- > 0u;) {
        if (value->limb[i] != 0u) {
            for (unsigned bit = 32u; bit-- > 0u;) {
                if ((value->limb[i] & (1u << bit)) != 0u) {
                    return (int)(i * 32u + bit);
                }
            }
        }
    }
    return -1;
}

static void big_shift_left(RefBig *value, unsigned count) {
    const unsigned words = count / 32u;
    const unsigned bits = count % 32u;
    if (words >= REF_BIG_LIMBS) {
        big_zero(value);
        return;
    }
    for (unsigned i = REF_BIG_LIMBS; i-- > words;) {
        uint32_t result = value->limb[i - words] << bits;
        if (bits != 0u && i > words) {
            result |= value->limb[i - words - 1u] >> (32u - bits);
        }
        value->limb[i] = result;
    }
    for (unsigned i = 0u; i < words; i++) {
        value->limb[i] = 0u;
    }
}

static void big_shift_right(const RefBig *value, unsigned count, RefBig *result) {
    const unsigned words = count / 32u;
    const unsigned bits = count % 32u;
    for (unsigned i = 0u; i < REF_BIG_LIMBS; i++) {
        uint32_t part = 0u;
        if (i + words < REF_BIG_LIMBS) {
            part = value->limb[i + words] >> bits;
        }
        if (bits != 0u && i + words + 1u < REF_BIG_LIMBS) {
            part |= value->limb[i + words + 1u] << (32u - bits);
        }
        result->limb[i] = part;
    }
}

static int big_compare(const RefBig *a, const RefBig *b) {
    for (unsigned i = REF_BIG_LIMBS; i-- > 0u;) {
        if (a->limb[i] < b->limb[i]) {
            return -1;
        }
        if (a->limb[i] > b->limb[i]) {
            return 1;
        }
    }
    return 0;
}

static void big_add(RefBig *a, const RefBig *b) {
    uint64_t carry = 0u;
    for (unsigned i = 0u; i < REF_BIG_LIMBS; i++) {
        const uint64_t sum = (uint64_t)a->limb[i] + b->limb[i] + carry;
        a->limb[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
}

static void big_subtract(RefBig *a, const RefBig *b) {
    uint32_t borrow = 0u;
    for (unsigned i = 0u; i < REF_BIG_LIMBS; i++) {
        const int64_t difference = (int64_t)a->limb[i] - b->limb[i] - borrow;
        a->limb[i] = (uint32_t)difference;
        borrow = difference < 0 ? 1u : 0u;
    }
}

static void big_increment(RefBig *value) {
    for (unsigned i = 0u; i < REF_BIG_LIMBS; i++) {
        value->limb[i]++;
        if (value->limb[i] != 0u) {
            return;
        }
    }
}

static void big_add_u32(RefBig *value, uint32_t addend) {
    uint64_t carry = addend;
    for (unsigned i = 0u; i < REF_BIG_LIMBS && carry != 0u; i++) {
        const uint64_t sum = (uint64_t)value->limb[i] + (uint32_t)carry;
        value->limb[i] = (uint32_t)sum;
        carry = (carry >> 32) + (sum >> 32);
    }
}

static void big_set_bit(RefBig *value, unsigned bit) {
    if (bit < REF_BIG_BITS) {
        value->limb[bit / 32u] |= 1u << (bit % 32u);
    }
}

static void big_decimal_sqrt(const RefBig *value, unsigned fractional_bits,
                             RefBig *root, int *inexact) {
    const int highest = big_high_bit(value);
    const unsigned bit_count = (unsigned)highest + 1u;
    RefBig remainder;
    unsigned bit = bit_count;
    int discarded = 0;
    big_zero(root);
    big_zero(&remainder);
    while (bit != 0u) {
        unsigned group = 0u;
        unsigned count;
        RefBig trial;
        int subtract;
        if (bit == bit_count && (bit_count & 1u) != 0u) {
            count = 1u;
        } else {
            count = 2u;
        }
        while (count-- != 0u && bit != 0u) {
            group = (group << 1) | big_bit(value, --bit);
        }
        big_shift_left(&remainder, 2u);
        big_add_u32(&remainder, group);
        trial = *root;
        big_shift_left(&trial, 2u);
        big_increment(&trial);
        subtract = big_compare(&remainder, &trial) >= 0;
        if (subtract) {
            big_subtract(&remainder, &trial);
        }
        {
            RefBig shifted = *root;
            big_shift_left(&shifted, 1u);
            *root = shifted;
        }
        if (subtract) {
            big_increment(root);
        }
    }
    if (fractional_bits != 0u) {
        RefBig truncated;
        discarded = big_any_below(root, fractional_bits);
        big_shift_right(root, fractional_bits, &truncated);
        *root = truncated;
    }
    *inexact = discarded || !big_is_zero(&remainder);
}

static int float_is_nan(uint32_t bits) {
    return (bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0u;
}

static int float_is_inf(uint32_t bits) {
    return (bits & 0x7fffffffu) == 0x7f800000u;
}

static int float_is_zero(uint32_t bits) {
    return (bits & 0x7fffffffu) == 0u;
}

static int float_is_subnormal(uint32_t bits) {
    return (bits & 0x7f800000u) == 0u && (bits & 0x007fffffu) != 0u;
}

static int float_is_signaling_nan(uint32_t bits) {
    return float_is_nan(bits) && (bits & 0x00400000u) == 0u;
}

static int float_unpack_finite(uint32_t bits, RefFinite *value) {
    const uint32_t exponent = (bits >> 23) & 0xffu;
    const uint32_t fraction = bits & 0x007fffffu;
    if (exponent == 0xffu) {
        return 0;
    }
    value->sign = bits >> 31;
    if (exponent == 0u) {
        value->significand = fraction;
        value->exponent = -149;
    } else {
        value->significand = 0x00800000u | fraction;
        value->exponent = (int)exponent - 150;
    }
    return 1;
}

static int float_finite_exponent(uint32_t bits) {
    const uint32_t exponent = (bits >> 23) & 0xffu;
    return exponent == 0u ? -149 : (int)exponent - 150;
}

static uint32_t signed_zero(unsigned sign, unsigned mode) {
    if (sign != 0u && mode == 3u) {
        return 0x80000000u;
    }
    return 0u;
}

static uint32_t overflow_result(unsigned sign, unsigned mode) {
    const int toward_zero = mode == 1u;
    const int toward_negative = mode == 3u && sign == 0u;
    const int toward_positive = mode == 2u && sign != 0u;
    const uint32_t magnitude = toward_zero || toward_negative || toward_positive
        ? 0x7f7fffffu : 0x7f800000u;
    return (sign << 31) | magnitude;
}

static uint32_t round_exact(const RefBig *magnitude, int exponent,
                            unsigned sign, unsigned mode, int fs,
                            int sticky, enum FpuReferenceStatus *status) {
    const int high = big_high_bit(magnitude);
    int unbiased;
    if (high < 0) {
        return signed_zero(sign, mode);
    }
    unbiased = exponent + high;

    if (unbiased > 127) {
        return overflow_result(sign, mode);
    }

    if (unbiased >= -126) {
        const int shift = high - 23;
        RefBig significand;
        if (shift <= 0) {
            significand = *magnitude;
            big_shift_left(&significand, (unsigned)(-shift));
        } else {
            const unsigned count = (unsigned)shift;
            const unsigned guard = big_bit(magnitude, count - 1u);
            const int remainder = big_any_below(magnitude, count - 1u) || sticky;
            unsigned increment = 0u;
            big_shift_right(magnitude, count, &significand);
            if (mode == 0u) {
                increment = guard && (remainder || (significand.limb[0] & 1u));
            } else if (mode == 2u) {
                increment = sign == 0u && (remainder || guard);
            } else if (mode == 3u) {
                increment = sign != 0u && (remainder || guard);
            }
            if (increment) {
                big_increment(&significand);
                if (significand.limb[0] == 0x01000000u &&
                    (significand.limb[1] | significand.limb[2] |
                     significand.limb[3] | significand.limb[4] |
                     significand.limb[5] | significand.limb[6] |
                     significand.limb[7] | significand.limb[8] |
                     significand.limb[9] | significand.limb[10] |
                     significand.limb[11] | significand.limb[12] |
                     significand.limb[13] | significand.limb[14] |
                     significand.limb[15]) == 0u) {
                    {
                        const RefBig shifted = significand;
                        big_shift_right(&shifted, 1u, &significand);
                    }
                    unbiased++;
                    if (unbiased > 127) {
                        return overflow_result(sign, mode);
                    }
                }
            }
        }
        return (sign << 31) | ((uint32_t)(unbiased + 127) << 23) |
               (significand.limb[0] & 0x007fffffu);
    }

    {
        const int shift = -149 - exponent;
        const unsigned count = (unsigned)shift;
        RefBig fraction;
        unsigned increment = 0u;
        if (count == 0u) {
            if (fs && !big_is_zero(magnitude)) {
                ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
            }
            return (sign << 31) | (magnitude->limb[0] & 0x007fffffu);
        }
        {
            const unsigned guard = big_bit(magnitude, count - 1u);
            const int remainder = big_any_below(magnitude, count - 1u) || sticky;
            big_shift_right(magnitude, count, &fraction);
        if (mode == 0u) {
            increment = guard && (remainder || (fraction.limb[0] & 1u));
        } else if (mode == 2u) {
            increment = sign == 0u && (remainder || guard);
        } else if (mode == 3u) {
            increment = sign != 0u && (remainder || guard);
        }
        if (increment) {
            big_increment(&fraction);
        }
        if (fraction.limb[0] == 0x00800000u) {
            return (sign << 31) | 0x00800000u;
        }
        if (fraction.limb[0] != 0u && fs) {
            ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
        }
        return (sign << 31) | fraction.limb[0];
    }
}
}

static uint32_t exact_add_sub(const RefFinite *a, const RefFinite *b,
                              int subtract, unsigned mode, unsigned fs,
                              enum FpuReferenceStatus *status) {
    const int exponent = a->exponent < b->exponent ? a->exponent : b->exponent;
    const uint32_t b_sign = b->sign ^ (subtract != 0 ? 1u : 0u);
    RefBig left;
    RefBig right;
    big_set_u64(&left, a->significand);
    big_set_u64(&right, b->significand);
    big_shift_left(&left, (unsigned)(a->exponent - exponent));
    big_shift_left(&right, (unsigned)(b->exponent - exponent));
    if (a->sign == b_sign) {
        big_add(&left, &right);
        return round_exact(&left, exponent, a->sign, mode, fs, 0, status);
    }
    if (big_compare(&left, &right) >= 0) {
        big_subtract(&left, &right);
        if (big_high_bit(&left) < 0) {
            return signed_zero(mode == 3u ? 1u : 0u, mode);
        }
        return round_exact(&left, exponent, a->sign, mode, fs, 0, status);
    }
    big_subtract(&right, &left);
    if (big_high_bit(&right) < 0) {
        return signed_zero(mode == 3u ? 1u : 0u, mode);
    }
    return round_exact(&right, exponent, b_sign, mode, fs, 0, status);
}

static uint32_t binary_add(uint32_t a_bits, uint32_t b_bits, unsigned mode,
                           unsigned fs, enum FpuReferenceStatus *status) {
    RefFinite a;
    RefFinite b;
    float_unpack_finite(a_bits, &a);
    float_unpack_finite(b_bits, &b);
    if (a.significand == 0u || b.significand == 0u) {
        if (a.significand == 0u && b.significand == 0u) {
            if (a.sign == b.sign) {
                return a.sign << 31;
            }
            return signed_zero(mode == 3u ? 1u : 0u, mode);
        }
        if (a.significand == 0u) {
            if (fs && float_is_subnormal(b_bits)) {
                ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
            }
            return b_bits;
        }
        if (fs && float_is_subnormal(a_bits)) {
            ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
        }
        return a_bits;
    }
    return exact_add_sub(&a, &b, 0, mode, fs, status);
}

static uint32_t binary_sub(uint32_t a_bits, uint32_t b_bits, unsigned mode,
                           unsigned fs, enum FpuReferenceStatus *status) {
    RefFinite a;
    RefFinite b;
    float_unpack_finite(a_bits, &a);
    float_unpack_finite(b_bits, &b);
    const uint32_t b_original_sign = b.sign;
    b.sign ^= 1u;
    if (a.significand == 0u || b.significand == 0u) {
        if (a.significand == 0u && b.significand == 0u) {
            if (a.sign == b.sign) {
                return a.sign << 31;
            }
            return signed_zero(mode == 3u ? 1u : 0u, mode);
        }
        if (a.significand == 0u) {
            if (fs && float_is_subnormal(b_bits)) {
                ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
            }
            return b_bits ^ 0x80000000u;
        }
        if (fs && float_is_subnormal(a_bits)) {
            ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
        }
        return a_bits;
    }
    b.sign = b_original_sign;
    return exact_add_sub(&a, &b, 1, mode, fs, status);

}

static uint32_t binary_mul(uint32_t a_bits, uint32_t b_bits, unsigned mode,
                           unsigned fs, enum FpuReferenceStatus *status) {
    RefFinite a;
    RefFinite b;
    const unsigned sign = (a_bits >> 31) ^ (b_bits >> 31);
    const int a_zero = float_is_zero(a_bits);
    const int b_zero = float_is_zero(b_bits);
    const int a_inf = float_is_inf(a_bits);
    const int b_inf = float_is_inf(b_bits);
    if (float_is_nan(a_bits) || float_is_nan(b_bits)) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
        return 0x7fc00000u;
    }
    if ((a_inf && b_zero) || (b_inf && a_zero)) {
        return 0x7fc00000u;
    }
    if (a_inf || b_inf) {
        return (sign << 31) | 0x7f800000u;
    }
    if (a_zero || b_zero) {
        return sign << 31;
    }
    float_unpack_finite(a_bits, &a);
    float_unpack_finite(b_bits, &b);
    {
        const uint64_t product = (uint64_t)a.significand * b.significand;
        RefBig magnitude;
        big_set_u64(&magnitude, product);
        return round_exact(&magnitude, a.exponent + b.exponent, sign, mode,
                           fs != 0, 0, status);
    }
}

static uint32_t binary_div(uint32_t a_bits, uint32_t b_bits, unsigned mode,
                           unsigned fs, enum FpuReferenceStatus *status) {
    RefFinite a;
    RefFinite b;
    const unsigned sign = (a_bits >> 31) ^ (b_bits >> 31);
    const int a_zero = float_is_zero(a_bits);
    const int b_zero = float_is_zero(b_bits);
    const int a_inf = float_is_inf(a_bits);
    const int b_inf = float_is_inf(b_bits);
    if (float_is_nan(a_bits) || float_is_nan(b_bits)) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
        return 0x7fc00000u;
    }
    if ((a_inf && b_inf) || (a_zero && b_zero)) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
        return 0x7fc00000u;
    }
    if (a_inf) {
        return (sign << 31) | 0x7f800000u;
    }
    if (b_inf) {
        return sign << 31;
    }
    if (a_zero) {
        return sign << 31;
    }
    if (b_zero) {
        return (sign << 31) | 0x7f800000u;
    }
    float_unpack_finite(a_bits, &a);
    float_unpack_finite(b_bits, &b);
    {
        const uint32_t integer = a.significand / b.significand;
        uint32_t remainder = a.significand % b.significand;
        RefBig magnitude;
        big_set_u64(&magnitude, integer);
        big_shift_left(&magnitude, 64u);
        for (int bit = 63; bit >= 0; bit--) {
            const uint32_t shifted = remainder << 1;
            if (shifted >= b.significand) {
                remainder = shifted - b.significand;
                big_set_bit(&magnitude, (unsigned)bit);
            } else {
                remainder = shifted;
            }
        }
        {
            int quotient_exponent = a.exponent - b.exponent - 64;
            const int quotient_high = big_high_bit(&magnitude);
            if (quotient_high >= 0 && quotient_high < 23) {
                const unsigned left = (unsigned)(23 - quotient_high);
                big_shift_left(&magnitude, left);
                quotient_exponent -= (int)left;
            }
            return round_exact(&magnitude, quotient_exponent, sign,
                               mode, fs != 0, remainder != 0u, status);
        }
    }
}

uint32_t fpu_reference_binary(unsigned op, uint32_t a_bits, uint32_t b_bits,
                              uint32_t fcr31,
                              enum FpuReferenceStatus *status) {
    const unsigned mode = fcr31 & 3u;
    const unsigned fs = (fcr31 & REF_FS) != 0u ? 1u : 0u;
    ref_status(status, COSIM_FPU_REFERENCE_OK);
    if (float_is_nan(a_bits) || float_is_nan(b_bits)) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
        return 0x7fc00000u;
    }
    switch (op) {
    case COSIM_FPU_REFERENCE_ADD:
        if (float_is_nan(a_bits) || float_is_nan(b_bits)) {
            return 0x7fc00000u;
        }
        if (float_is_inf(a_bits) && float_is_inf(b_bits) &&
            (a_bits >> 31) != (b_bits >> 31)) {
            ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
            return 0x7fc00000u;
        }
        if (float_is_inf(a_bits)) {
            return a_bits;
        }
        if (float_is_inf(b_bits)) {
            return b_bits;
        }
        return binary_add(a_bits, b_bits, mode, fs, status);
    case COSIM_FPU_REFERENCE_SUB:
        if (float_is_nan(a_bits) || float_is_nan(b_bits)) {
            return 0x7fc00000u;
        }
        if (float_is_inf(a_bits) && float_is_inf(b_bits) &&
            (a_bits >> 31) == (b_bits >> 31)) {
            ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
            return 0x7fc00000u;
        }
        if (float_is_inf(a_bits)) {
            return a_bits;
        }
        if (float_is_inf(b_bits)) {
            return b_bits ^ 0x80000000u;
        }
        return binary_sub(a_bits, b_bits, mode, fs, status);
    case COSIM_FPU_REFERENCE_MUL:
        return binary_mul(a_bits, b_bits, mode, fs, status);
    case COSIM_FPU_REFERENCE_DIV:
        return binary_div(a_bits, b_bits, mode, fs, status);
    default:
        ref_status(status, COSIM_FPU_REFERENCE_INVALID);
        return 0u;
    }
}

static uint32_t unary_sqrt(uint32_t bits, unsigned mode, unsigned fs,
                           enum FpuReferenceStatus *status) {
    RefFinite value;
    if (mode != 0u) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
    }
    if (float_is_nan(bits)) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
        return 0x7fc00000u;
    }
    if (float_is_zero(bits)) {
        return bits;
    }
    if ((bits >> 31) != 0u) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
        return 0x7fc00000u;
    }
    if (float_is_inf(bits)) {
        return bits;
    }
    float_unpack_finite(bits, &value);
    {
        RefBig radicand;
        RefBig root;
        int exponent = value.exponent;
        int inexact;
        big_set_u64(&radicand, value.significand);
        if (exponent % 2 != 0) {
            big_shift_left(&radicand, 1u);
            exponent--;
        }
        big_shift_left(&radicand, 64u);
        big_decimal_sqrt(&radicand, 0u, &root, &inexact);
        return round_exact(&root, exponent / 2 - 32, 0u, mode, fs,
                           inexact, status);
    }
}

uint32_t fpu_reference_unary(unsigned op, uint32_t a_bits, uint32_t fcr31,
                             enum FpuReferenceStatus *status) {
    const unsigned mode = fcr31 & 3u;
    const unsigned fs = (fcr31 & REF_FS) != 0u ? 1u : 0u;
    ref_status(status, COSIM_FPU_REFERENCE_OK);
    if (float_is_nan(a_bits)) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
    }
    switch (op) {
    case COSIM_FPU_REFERENCE_SQRT:
        return unary_sqrt(a_bits, mode, fs, status);
    case COSIM_FPU_REFERENCE_ABS:
        return a_bits & 0x7fffffffu;
    case COSIM_FPU_REFERENCE_MOV:
        return a_bits;
    case COSIM_FPU_REFERENCE_NEG:
        return a_bits ^ 0x80000000u;
    default:
        ref_status(status, COSIM_FPU_REFERENCE_INVALID);
        return 0u;
    }
}

static unsigned finite_magnitude_floor(const RefFinite *value,
                                       uint32_t *fraction_nonzero) {
    uint32_t integral;
    uint32_t fraction = 0u;
    if (value->exponent >= 0) {
        integral = value->significand << value->exponent;
    } else if (-value->exponent >= 32) {
        integral = 0u;
        fraction = value->significand;
    } else {
        const unsigned shift = (unsigned)-value->exponent;
        integral = value->significand >> shift;
        fraction = value->significand & ((1u << shift) - 1u);
    }
    *fraction_nonzero = fraction != 0u ? 1u : 0u;
    return integral;
}

uint32_t fpu_reference_to_word(uint32_t bits, unsigned funct, uint32_t fcr31,
                               enum FpuReferenceStatus *status) {
    unsigned mode;
    RefFinite value;
    uint32_t integral;
    uint32_t fraction;
    uint32_t fraction_nonzero;
    uint32_t rounded;
    ref_status(status, COSIM_FPU_REFERENCE_OK);
    if (float_is_signaling_nan(bits)) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
    }
    if (float_is_nan(bits)) {
        return 0x7fffffffu;
    }
    if (float_is_inf(bits)) {
        return (bits >> 31) != 0u ? 0x80000000u : 0x7fffffffu;
    }
    switch (funct) {
    case 0x0du: mode = 1u; break;
    case 0x0eu: mode = 2u; break;
    case 0x0fu: mode = 3u; break;
    case 0x24u: mode = fcr31 & 3u; break;
    default: mode = 0u; break;
    }
    if (float_is_zero(bits)) {
        return 0u;
    }
    float_unpack_finite(bits, &value);
    if (value.exponent >= 8) {
        return value.sign != 0u ? 0x80000000u : 0x7fffffffu;
    }
    integral = finite_magnitude_floor(&value, &fraction_nonzero);
    fraction = value.significand;
    if (value.exponent < 0) {
        const unsigned shift = (unsigned)-value.exponent;
        if (shift < 32u) {
            fraction &= (1u << shift) - 1u;
        }
    } else {
        fraction = 0u;
    }
    rounded = integral;
    if (mode == 0u) {
        const unsigned half_shift = value.exponent < 0
            ? (unsigned)(-value.exponent - 1) : 32u;
        const uint32_t half_mask = half_shift < 32u ? 1u << half_shift : 0u;
        const int tie = (fraction & half_mask) != 0u;
        const int below_half = half_mask != 0u &&
            (fraction & (half_mask - 1u)) != 0u;
        if (tie && (below_half || (integral & 1u))) {
            rounded++;
        }
    } else if (mode == 2u && value.sign == 0u && fraction_nonzero) {
        rounded++;
    } else if (mode == 3u && value.sign != 0u && fraction_nonzero) {
        rounded++;
    }
    if (rounded >= 0x80000000u) {
        return value.sign != 0u ? 0x80000000u : 0x7fffffffu;
    }
    return value.sign != 0u ? 0u - rounded : rounded;
}

uint32_t fpu_reference_cvt_s_w(int32_t value, uint32_t fcr31,
                               enum FpuReferenceStatus *status) {
    const uint32_t word = (uint32_t)value;
    const uint32_t magnitude = value < 0 ? 0u - word : word;
    RefBig significand;
    big_set_u64(&significand, magnitude);
    ref_status(status, COSIM_FPU_REFERENCE_OK);
    if (magnitude == 0u) {
        return 0u;
    }
    return round_exact(&significand, 0, value < 0 ? 1u : 0u,
                       fcr31 & 3u, 0, 0, status);
}

/*
 * MIPS32 Architecture for Programmers, Volume II, revision 2.62, C.cond.fmt
 * predicate table. Each row is one condition from 0 through 15; columns are
 * less, equal, greater, and unordered. Signaling predicates 8 through 15
 * repeat 0 through 7; bit 3 only selects signaling behavior.
 */
static const uint8_t ref_ccond_truth[16][4] = {
    {0u, 0u, 0u, 0u}, {0u, 0u, 0u, 1u}, {0u, 1u, 0u, 0u}, {0u, 1u, 0u, 1u},
    {1u, 0u, 0u, 0u}, {1u, 0u, 0u, 1u}, {1u, 1u, 0u, 0u}, {1u, 1u, 0u, 1u},
    {0u, 0u, 0u, 0u}, {0u, 0u, 0u, 1u}, {0u, 1u, 0u, 0u}, {0u, 1u, 0u, 1u},
    {1u, 0u, 0u, 0u}, {1u, 0u, 0u, 1u}, {1u, 1u, 0u, 0u}, {1u, 1u, 0u, 1u},
};

unsigned fpu_reference_compare(unsigned condition, uint32_t a_bits,
                               uint32_t b_bits,
                               enum FpuReferenceStatus *status) {
    const int unordered = float_is_nan(a_bits) || float_is_nan(b_bits);
    int equal = 0;
    int less = 0;
    unsigned relation;
    ref_status(status, COSIM_FPU_REFERENCE_OK);
    if (condition > 15u) {
        ref_status(status, COSIM_FPU_REFERENCE_INVALID);
        return 0u;
    }
    if (!unordered) {
        const int a_infinity = float_is_inf(a_bits);
        const int b_infinity = float_is_inf(b_bits);
        if (float_is_zero(a_bits) && float_is_zero(b_bits)) {
            equal = 1;
        } else if (a_infinity || b_infinity) {
            if (a_infinity && b_infinity) {
                equal = (a_bits >> 31) == (b_bits >> 31);
                less = (a_bits >> 31) != 0u && (b_bits >> 31) == 0u;
            } else if (a_infinity) {
                less = (a_bits >> 31) != 0u;
            } else {
                less = (b_bits >> 31) == 0u;
            }
        } else {
            RefFinite a;
            RefFinite b;
            const int a_exponent = float_finite_exponent(a_bits);
            const int b_exponent = float_finite_exponent(b_bits);
            const int exponent = a_exponent < b_exponent
                ? a_exponent : b_exponent;
            RefBig left;
            RefBig right;
            float_unpack_finite(a_bits, &a);
            float_unpack_finite(b_bits, &b);
            big_set_u64(&left, a.significand);
            big_set_u64(&right, b.significand);
            big_shift_left(&left, (unsigned)(a.exponent - exponent));
            big_shift_left(&right, (unsigned)(b.exponent - exponent));
            if (a.significand == b.significand && a.exponent == b.exponent &&
                (a.significand == 0u || a.sign == b.sign)) {
                equal = 1;
                less = 0;
            } else {
                equal = 0;
                less = big_compare(&left, &right) < 0;
                if (a.sign != b.sign) {
                    less = a.sign != 0u;
                } else if (a.sign != 0u) {
                    less = !less;
                }
            }
        }
    }
    relation = unordered ? 3u : less ? 0u : equal ? 1u : 2u;
    if (unordered && condition >= 8u) {
        ref_status(status, COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN);
    }
    return ref_ccond_truth[condition][relation];
}

unsigned fpu_reference_bc1(unsigned tf, unsigned condition) {
    return tf != 0u ? condition != 0u : condition == 0u;
}
