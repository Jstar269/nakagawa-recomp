// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Portable Allegrex scalar-FPU and VFPU float-to-word conversions.
//
// This implementation is independently authored from behavioral contracts:
// - PSPAutotests ea71108f00933712c4662276261b39cd42249b1e records Allegrex
//   cvt/trunc/ceil/floor results and VFPU vf2i results, including NaN and
//   infinities.
// - MIPS32 Architecture for Programmers, Volume II, revision 2.62 defines
//   ROUND.W as round-to-nearest with ties to even and CVT.W as FCSR-directed.
// - PPSSPP 06d5847da382336dc93a73b5e5adcd23de2f305d provides the public
//   finite-overflow precedent used where PSPAutotests has no boundary vector.
// No implementation code was copied from those projects.
//
// Scope: this layer selects result bits. The existing runtime does not model
// COP1 exception flags, enables, or traps, and this header does not claim to
// add them. PSPAutotests directly establishes ordinary inputs, FCR31-directed
// modes, ties, and non-finite result words. Fixed round.w.s ties, finite
// overflow, scaled VFPU overflow, and subnormal edges rely on the cited MIPS
// contract plus PPSSPP precedent rather than a direct current hardware vector.

#ifndef SR_FP_CONVERT_H
#define SR_FP_CONVERT_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum SrFpRoundMode {
    SR_FP_ROUND_NEAREST_EVEN = 0,
    SR_FP_ROUND_TO_ZERO = 1,
    SR_FP_ROUND_POSITIVE = 2,
    SR_FP_ROUND_NEGATIVE = 3,
};

/* Do not use nearbyint()/rint(): they follow the host rounding environment.
 * The input has already been bounded below, so floor(x) is finite. */
static inline double sr_fp_round_nearest_even(double x) {
    const double lower = floor(x);
    const double fraction = x - lower;
    if (fraction < 0.5) return lower;
    if (fraction > 0.5) return lower + 1.0;
    return fmod(lower, 2.0) == 0.0 ? lower : lower + 1.0;
}

/* Return the PSP-visible signed word bit pattern.
 *
 * NaN and positive invalid inputs map to INT32_MAX; negative invalid inputs
 * map to INT32_MIN. Finite overflow saturates by sign. The latter is supported
 * by PPSSPP's production JIT paths but lacks a direct PSPAutotests finite-
 * boundary vector, so callers/tests must not describe it as hardware-measured.
 * No float-to-integer cast occurs until the rounded value is proven in range. */
static inline uint32_t sr_fp_to_s32(double x, unsigned mode) {
    double rounded;

    if (x != x) return 0x7fffffffu;       /* NaN */
    if (x >= 2147483648.0) return 0x7fffffffu;
    if (x < -2147483648.0) return 0x80000000u;

    switch (mode & 3u) {
        case SR_FP_ROUND_TO_ZERO:
            rounded = x < 0.0 ? ceil(x) : floor(x);
            break;
        case SR_FP_ROUND_POSITIVE:
            rounded = ceil(x);
            break;
        case SR_FP_ROUND_NEGATIVE:
            rounded = floor(x);
            break;
        default:
            rounded = sr_fp_round_nearest_even(x);
            break;
    }

    if (rounded >= 2147483648.0) return 0x7fffffffu;
    if (rounded < -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)rounded;
}

static inline uint32_t sr_float_bits(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

static inline int sr_float_invalid_word(uint32_t bits, uint32_t *result) {
    const uint32_t magnitude = bits & 0x7fffffffu;
    if (magnitude < 0x7f800000u) return 0;
    /* A negative infinity is the only non-finite negative result. NaNs map to
     * INT32_MAX regardless of their sign or payload. Inspecting bits avoids a
     * host signaling-NaN comparison before the PSP-visible result is chosen. */
    *result = magnitude == 0x7f800000u && (bits >> 31) != 0u
        ? 0x80000000u : 0x7fffffffu;
    return 1;
}

/* Reconstruct every finite IEEE-754 binary32 value exactly in binary64 from
 * its bits. A direct C float-to-double promotion is exact mathematically, but
 * an x86 host with DAZ enabled can treat a subnormal source as zero during the
 * conversion. Integer mantissas and the powers used here are exactly
 * representable in binary64, independent of host rounding/FTZ/DAZ controls. */
static inline double sr_finite_float_to_double(uint32_t bits) {
    const uint32_t exponent = (bits >> 23) & 0xffu;
    const uint32_t fraction = bits & 0x007fffffu;
    const uint32_t significand = exponent == 0u ? fraction : 0x00800000u | fraction;
    const int power = exponent == 0u ? -149 : (int)exponent - 150;
    const double magnitude = ldexp((double)significand, power);
    return (bits >> 31) != 0u ? -magnitude : magnitude;
}

/* funct is the COP1 S-format function field. cvt.w.s uses fcr31[1:0]. */
static inline uint32_t sr_fpu_to_word(float x, uint32_t funct, uint32_t fcr31) {
    const uint32_t bits = sr_float_bits(x);
    uint32_t invalid_result = 0u;
    unsigned mode;
    if (sr_float_invalid_word(bits, &invalid_result)) return invalid_result;
    switch (funct) {
        case 0x0cu: mode = SR_FP_ROUND_NEAREST_EVEN; break; /* round.w.s */
        case 0x0du: mode = SR_FP_ROUND_TO_ZERO; break;      /* trunc.w.s */
        case 0x0eu: mode = SR_FP_ROUND_POSITIVE; break;     /* ceil.w.s */
        case 0x0fu: mode = SR_FP_ROUND_NEGATIVE; break;     /* floor.w.s */
        case 0x24u: mode = fcr31 & 3u; break;                /* cvt.w.s */
        default:    mode = SR_FP_ROUND_NEAREST_EVEN; break;
    }
    return sr_fp_to_s32(sr_finite_float_to_double(bits), mode);
}

/* VFPU vf2in/vf2iz/vf2iu/vf2id use the same four rounding modes after an
 * exact power-of-two scale. ldexp keeps the operation independent of the host
 * FP rounding mode for every float input and scale 0..31. */
static inline uint32_t sr_vfpu_to_word(float x, unsigned mode, unsigned scale) {
    const uint32_t bits = sr_float_bits(x);
    uint32_t invalid_result = 0u;
    if (sr_float_invalid_word(bits, &invalid_result)) return invalid_result;
    return sr_fp_to_s32(
        ldexp(sr_finite_float_to_double(bits), (int)(scale & 31u)), mode
    );
}

/* Interpret a PSP word as signed without an implementation-defined
 * uint32_t-to-int32_t conversion or assuming the host's signed representation. */
static inline int32_t sr_u32_as_s32(uint32_t bits) {
    if (bits <= 0x7fffffffu) return (int32_t)bits;
    return -1 - (int32_t)(0xffffffffu - bits);
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SR_FP_CONVERT_H */
