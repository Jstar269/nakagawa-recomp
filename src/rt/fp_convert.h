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
//
// Scalar arithmetic (below) extends the same behavioral-contract approach to
// FCR31-directed add/sub/mul/div.s and int-to-float conversion:
// - PSPAutotests fpu.expected (PSP_HARDWARE capture) pins mul.s RM anchors,
//   the FS bit-24 flush gate on smallest-normal*0.5, and FCC0 at bit 23.
// - MIPS32 Volume II defines RM-directed rounding for ADD/SUB/MUL/DIV.S and
//   CVT.S.W, with FS gating subnormal RESULT flushing only.
//
// Guest-visible FS semantics modeled here: FS=1 flushes an underflowing
// RESULT to signed zero. Input-side DAZ-on-FS is publicly unmeasured
// inference and is deliberately NOT modeled: subnormal inputs always keep
// gradual weight, matching both the measured FS=0 behavior and x86/ARM hosts.
// No COP1 cause/flag bits are produced; that remains unmodeled scope.

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

/* ---- Guest FCR31 field layout and scalar arithmetic helpers ----
 *
 * FCR31 bits consumed by this layer: RM [1:0], FCC0 [23], FS [24].
 *
 * Fast-path predicate: emitted scalar code may use the plain host operation
 * exactly when the guest has no non-default arithmetic policy. Only RM and FS
 * alter scalar results in the modeled slice; flags, enables, cause, FCC, and
 * reserved bits must neither force nor bypass the helper path.
 *
 * HOST INVARIANT -- ASSUMED, NOT VALIDATED: the fast path additionally
 * requires the host environment to sit at its IEEE defaults (round-to-
 * nearest, no FTZ, no DAZ). Nothing in the runtime establishes or checks
 * that environment today; a foreign library mutating process FP state could
 * silently bias fast-path results. This is retained as pre-existing risk
 * RISK-8 (latent host-FP-env contamination via flagless fibers); any startup
 * canary/validation is deliberately deferred so this slice stays bounded.
 * The helper path below is NOT exposed to that hazard: it pins RC/FTZ/DAZ
 * inside its own window regardless of ambient state.
 */
#define SR_FCR31_RM_MASK 0x00000003u
#define SR_FCR31_FCC0    0x00800000u
#define SR_FCR31_FS      0x01000000u

static inline int sr_fpu_scalar_fast(uint32_t fcr31) {
    return (fcr31 & (SR_FCR31_RM_MASK | SR_FCR31_FS)) == 0u;
}

#if defined(__SSE2__) || defined(_M_X64) || defined(__i386__)
#include <xmmintrin.h>

static inline uint32_t sr_fpu_env_save(void) {
    return _mm_getcsr();
}

/* Install the guest's rounding policy on the host FP environment.
 *
 * Guest RM uses the MIPS encoding (0=RN, 1=RZ, 2=RP/+inf, 3=RM/-inf); MXCSR
 * RC uses a different order (RN, -inf, +inf, zero), so the modes are
 * translated explicitly instead of copied. FS=1 maps to MXCSR.FTZ so an
 * underflowing result becomes signed zero, matching the PSP-visible flush
 * contract. DAZ is cleared unconditionally: the modeled contract gives
 * subnormal INPUTS gradual weight (input-side flushing is publicly unmeasured
 * inference and is not invented here), and a foreign library leaving DAZ set
 * must not be able to change helper-visible input semantics. Exception
 * mask/flag fields are preserved exactly as found, so no helper can unmask a
 * host FP exception. */
static inline void sr_fpu_env_apply_guest(uint32_t fcr31) {
    static const uint32_t rm_to_mxcsr[4] = {0u, 3u, 2u, 1u};
    uint32_t csr = _mm_getcsr();
    csr &= ~(0x3u << 13);
    csr &= ~(1u << 6);                              /* DAZ off: inputs stay gradual */
    csr |= rm_to_mxcsr[fcr31 & SR_FCR31_RM_MASK] << 13;
    if (fcr31 & SR_FCR31_FS) {
        csr |= 1u << 15;
    } else {
        csr &= ~(1u << 15);
    }
    _mm_setcsr(csr);
}

static inline void sr_fpu_env_restore(uint32_t saved) {
    _mm_setcsr(saved);
}

#else
/* Support boundary: the scoped-host-mechanism backend currently exists for
 * x86/x64 SSE2 hosts only -- which is every configuration the project builds
 * today (MinGW-w64 gcc and gcc/clang x86-64 CI). AArch64 (FPCR) and other
 * non-SSE backends are recorded follow-up work, deliberately out of scope
 * here. */
#error "sr_fpu scalar helpers need SSE2 for scoped host FP-environment control"
#endif

/* Ordering + opacity fence for the bounded operation inside each helper.
 *
 * The SSE environment writes are volatile asm and the guest operations are
 * ordinary C expressions; ISO C gives no FENV guarantee there, and this was
 * proven exploitable in practice: GCC -O1 reordered the CVT.S.W environment
 * window so the conversion ran under the AMBIENT host mode, and folded
 * literal-operand arithmetic at compile time under the default rounding mode
 * (both caught by the selftest optimization matrix). Two mechanisms close
 * that gap for every toolchain/optimization level the project builds:
 *   1. a "memory"-clobber barrier between each environment transition and
 *      the operation, so no memory access may move across the MXCSR write;
 *   2. volatile-qualified operand locals, so inlined/LTO'd constant
 *      arguments must round-trip through real stores and volatile loads and
 *      cannot be constant-folded under the compiler's default mode.
 * The claim is enforced, not assumed: the native selftest builds this header
 * at multiple optimization levels with literal-argument checks. */
#if defined(__GNUC__) || defined(__clang__)
static inline void sr_fpu_scalar_barrier(void) {
    __asm__ __volatile__("" ::: "memory");
}
#else
#error "sr_fpu scalar helpers need a memory-clobber compiler barrier"
#endif

/* Each wrapper bounds exactly one operation inside the guest environment.
 *
 * Why three separate mechanisms: the SSE transitions are volatile asm, the
 * guest operations are ordinary C expressions, and ISO C gives no FENV
 * guarantee tying them together. Proven exploitable on this toolchain: GCC
 * -O1 reordered the CVT.S.W window so the conversion ran under the AMBIENT
 * host mode, folded literal-operand arithmetic under the default rounding
 * mode, and even sank a register-only mulss/cvtsi2ss PAST the restoring
 * ldmxcsr where a memory-only barrier was the only guard. The wrapper
 * therefore composes:
 *   1. volatile operand locals -- constant arguments round-trip through real
 *      stores/loads, defeating compile-time folding (incl. LTO/inlining);
 *   2. a volatile STORE of the result inside the window -- the operation is
 *      chained by data dependency ahead of the store, and the surrounding
 *      memory-clobber barriers forbid the store (and with it the operation)
 *      from crossing either MXCSR transition;
 *   3. the environment restored exactly before return.
 * The claim is enforced, not assumed: the native selftest builds this header
 * at multiple optimization levels with literal-argument checks, and hostile-
 * environment tests prove ambient RC/FTZ/DAZ cannot alter helper results. */

static inline float sr_fpu_add_s(float a, float b, uint32_t fcr31) {
    volatile float va = a;
    volatile float vb = b;
    const uint32_t saved = sr_fpu_env_save();
    sr_fpu_env_apply_guest(fcr31);
    sr_fpu_scalar_barrier();
    volatile float vr = va + vb;
    sr_fpu_scalar_barrier();
    sr_fpu_env_restore(saved);
    return vr;
}

static inline float sr_fpu_sub_s(float a, float b, uint32_t fcr31) {
    volatile float va = a;
    volatile float vb = b;
    const uint32_t saved = sr_fpu_env_save();
    sr_fpu_env_apply_guest(fcr31);
    sr_fpu_scalar_barrier();
    volatile float vr = va - vb;
    sr_fpu_scalar_barrier();
    sr_fpu_env_restore(saved);
    return vr;
}

static inline float sr_fpu_mul_s(float a, float b, uint32_t fcr31) {
    volatile float va = a;
    volatile float vb = b;
    const uint32_t saved = sr_fpu_env_save();
    sr_fpu_env_apply_guest(fcr31);
    sr_fpu_scalar_barrier();
    volatile float vr = va * vb;
    sr_fpu_scalar_barrier();
    sr_fpu_env_restore(saved);
    return vr;
}

static inline float sr_fpu_div_s(float a, float b, uint32_t fcr31) {
    volatile float va = a;
    volatile float vb = b;
    const uint32_t saved = sr_fpu_env_save();
    sr_fpu_env_apply_guest(fcr31);
    sr_fpu_scalar_barrier();
    volatile float vr = va / vb;
    sr_fpu_scalar_barrier();
    sr_fpu_env_restore(saved);
    return vr;
}

/* CVT.S.W rounds per guest RM near the binary32 precision boundary. Unlike the
 * fast-path-guarded arithmetic above there is no host-default shortcut here:
 * a bare int-to-float cast follows whatever mode the host currently has (and
 * GCC -O1+ demonstrably reorders the conversion outside the MXCSR window
 * without the barrier), so even guest RN goes through the pinned path. FS
 * cannot affect this conversion (an integer converts to a magnitude of at
 * least 1.0 or to zero), so only RM is applied. */
static inline float sr_fpu_cvt_s_w(int32_t value, uint32_t fcr31) {
    volatile int32_t vv = value;
    const uint32_t saved = sr_fpu_env_save();
    sr_fpu_env_apply_guest(fcr31 & SR_FCR31_RM_MASK);
    sr_fpu_scalar_barrier();
    volatile float vr = (float)vv;
    sr_fpu_scalar_barrier();
    sr_fpu_env_restore(saved);
    return vr;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SR_FP_CONVERT_H */
