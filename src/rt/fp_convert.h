// SPDX-License-Identifier: GPL-3.0-or-later
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
 * FAST PATH DISPOSITION -- REMOVED FOR CORRECTNESS (review of 38c2ba9/14f1541):
 * an earlier slice gated a plain host operation on a guest-state predicate
 * (sr_fpu_scalar_fast). Hostile-environment fixtures proved that path unsafe
 * at its known failing points: ambient host RC changed guest results under
 * guest-RN, ambient FTZ flushed guest-FS=0 gradual underflow, and every plain
 * host operation leaked MXCSR sticky status bits into caller state. Nothing
 * establishes or validates a host-default environment today (flagless fibers
 * make per-thread isolation unavailable), so ALL scalar operations route
 * through the scoped helpers below until a validated host-invariant design
 * exists. Re-introducing any fast path requires proving both sides of that
 * contract; see RISK-8.
 *
 * Field constants retained: they name the architectural bits the helpers and
 * emitted c.cond/ctc1 code consume.
 */
#define SR_FCR31_RM_MASK 0x00000003u
#define SR_FCR31_FCC0    0x00800000u
#define SR_FCR31_FS      0x01000000u
/* FCR31 bits 25..31 (FCC1..FCC7 on MIPS IV) do not exist on the PSP FPU: a
 * guest ctc1 of 0xFE800000 reads back 0x00800000 (PSP-FPU-CC-001, PSP-3000 /
 * 6.61, 2026-10-15), so ctc1 drops them and cfc1 never returns them. Only these
 * seven bits were measured as not writable; no other FCR31 bit is masked. The
 * generated C spells the same word as a literal (tools/codegen.py, ctc1). */
#define SR_FCR31_CTC1_UNWRITABLE 0xfe000000u

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
 * must not be able to change helper-visible input semantics.
 *
 * Exception control is fully masked for the bounded window: guest operations
 * routinely raise inexact/underflow/overflow, and preserving a hostile
 * caller's unmasked state would let them escalate into host FP faults
 * (reproduced: EXCEPTION_FLT_UNDERFLOW crash before this hardening). Sticky
 * flag bits raised inside the window never reach caller state because the
 * full-word restore below rewrites them; masks, like every other field, are
 * restored exactly as found. */
static inline void sr_fpu_env_apply_guest(uint32_t fcr31) {
    static const uint32_t rm_to_mxcsr[4] = {0u, 3u, 2u, 1u};
    uint32_t csr = _mm_getcsr();
    csr &= ~(0x3u << 13);
    csr &= ~(1u << 6);                              /* DAZ off: inputs stay gradual */
    csr |= 0x1f80u;                                 /* mask IM..PM: no host FP faults */
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

#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
/* AArch64 (Android arm64-v8a, Linux arm64): the guest window runs under FPCR.
 *
 * RMode [23:22] encodes RN=0, RP=1, RM=2, RZ=3, so the MIPS RM field is translated as it is for
 * MXCSR. FS is NOT mapped onto FPCR.FZ: FZ also flushes subnormal inputs, and without FEAT_AFP it
 * detects tininess before rounding, while the modeled contract (above) keeps subnormal inputs
 * gradual and the x86 backend detects tininess after rounding. The window therefore runs with
 * FZ=0 and the helpers below flush in software (sr_fpu_soft_ftz_*), which reproduces the x86
 * FTZ result bit for bit. Inside the window the trap enables (IOE..IXE, IDE) are cleared so a
 * guest operation never raises a host FP fault, and DN, FZ16 and the FEAT_AFP controls (AH, FIZ,
 * NEP) are cleared so NaN propagation and input handling are the IEEE defaults; the caller's FPCR
 * is restored exactly afterwards. FPCR's defined fields all sit in bits [26:0]. */
#define SR_FPU_SOFT_FS 1

static inline uint32_t sr_fpu_env_save(void) {
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr) :: "memory");
    return (uint32_t)fpcr;
}

static inline void sr_fpu_env_apply_guest(uint32_t fcr31) {
    static const uint32_t rm_to_rmode[4] = {0u, 3u, 1u, 2u};
    uint64_t fpcr = sr_fpu_env_save();
    fpcr &= ~(uint64_t)((3u << 22) | (1u << 24) | (1u << 25) | (1u << 19) |
                        (1u << 15) | (0x1fu << 8) | 0x7u);
    fpcr |= (uint64_t)rm_to_rmode[fcr31 & SR_FCR31_RM_MASK] << 22;
    __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr) : "memory");
}

static inline void sr_fpu_env_restore(uint32_t saved) {
    const uint64_t fpcr = saved;
    __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr) : "memory");
}

#else
/* Support boundary: the scoped-host-mechanism backend currently exists for
 * x86/x64 SSE2 hosts compiled with GCC-compatible toolchains -- which covers
 * every configuration that actually builds this code today (MinGW-w64 gcc
 * locally across -O0..-O3, gcc on ubuntu CI). Clang appears in CI only as a
 * compiler-info make-override probe and has NOT yet been exercised against
 * these helpers; MSVC is not used for them. Those compilers fail closed here
 * rather than risk a silently miscompiled window. AArch64 has the FPCR backend
 * above; any other host architecture fails closed here. */
#error "sr_fpu scalar helpers need SSE2 or AArch64 FPCR, with GCC-compatible barriers, for scoped host FP-environment control"
#endif

/* Ordering + opacity fence for the bounded operation inside each helper.
 *
 * The SSE environment writes are volatile asm and the guest operations are
 * ordinary C expressions; ISO C gives no FENV guarantee there, and this was
 * proven exploitable in practice: GCC -O1 reordered the CVT.S.W environment
 * window so the conversion ran under the AMBIENT host mode, and folded
 * literal-operand arithmetic at compile time under the default rounding mode
 * (both caught by the selftest optimization matrix). The mechanisms below
 * close that gap on the GCC toolchain family this header is actually built
 * with; other compilers fail closed at the gates above. Two of them:
 *   1. a "memory"-clobber barrier between each environment transition and
 *      the operation, so no memory access may move across the MXCSR write;
 *   2. volatile-qualified operand locals, so inlined/LTO'd constant
 *      arguments must round-trip through real stores and volatile loads and
 *      cannot be constant-folded under the compiler's default mode.
 * Enforced by the committed selftest optimization matrix, not assumed. */
#if defined(__GNUC__) || defined(__clang__)
static inline void sr_fpu_scalar_barrier(void) {
    __asm__ __volatile__("" ::: "memory");
}
#else
#error "sr_fpu scalar helpers need a memory-clobber compiler barrier"
#endif

/* ---- Software FS flush (the AArch64 backend; testable on every host) ----
 *
 * The x86 backend implements FS=1 with MXCSR.FTZ: a result that is tiny AFTER rounding (rounded
 * to 24 significant bits with an unbounded exponent, the result has magnitude below 2^-126) is
 * replaced by a zero of its sign. These functions produce the same bits from the result of the
 * same operation computed with gradual underflow, under the guest rounding mode, inside the
 * guest window:
 *   - a subnormal gradual result always means the result was tiny, so it flushes;
 *   - add/sub: a sum of two binary32 values is a multiple of 2^-149, so a tiny sum is exact and
 *     its gradual result is subnormal (or zero); the rule above is complete;
 *   - mul/div: a gradual result of exactly +-2^-126 is ambiguous (the unbounded-exponent result
 *     may be just below 2^-126), so the operation is repeated with one operand scaled by 2^64,
 *     which is exact and lands the result in the normal range where rounding is unbounded, and
 *     the scaled magnitude is compared with 2^-62.
 * Scaling cannot overflow in that case: a product or quotient of magnitude near 2^-126 bounds the
 * scaled operand well below 2^128 (|a| <= 2^23 for mul, |a| <= 2^2 for div). */
static inline float sr_fpu_soft_signed_zero(float r) {
    const uint32_t sign = sr_float_bits(r) & 0x80000000u;
    float z;
    memcpy(&z, &sign, sizeof z);
    return z;
}

static inline int sr_fpu_soft_is_subnormal(float r) {
    const uint32_t bits = sr_float_bits(r);
    return (bits & 0x7f800000u) == 0u && (bits & 0x007fffffu) != 0u;
}

static inline float sr_fpu_soft_ftz_sum(float r) {
    return sr_fpu_soft_is_subnormal(r) ? sr_fpu_soft_signed_zero(r) : r;
}

static inline float sr_fpu_soft_ftz_mul(float a, float b, float r) {
    if (sr_fpu_soft_is_subnormal(r)) return sr_fpu_soft_signed_zero(r);
    if ((sr_float_bits(r) & 0x7fffffffu) != 0x00800000u) return r;
    volatile float va = a * 0x1p64f;
    volatile float vb = b;
    volatile float vs = va * vb;
    return (sr_float_bits(vs) & 0x7fffffffu) < 0x20800000u ? sr_fpu_soft_signed_zero(r) : r;
}

static inline float sr_fpu_soft_ftz_div(float a, float b, float r) {
    if (sr_fpu_soft_is_subnormal(r)) return sr_fpu_soft_signed_zero(r);
    if ((sr_float_bits(r) & 0x7fffffffu) != 0x00800000u) return r;
    volatile float va = a * 0x1p64f;
    volatile float vb = b;
    volatile float vs = va / vb;
    return (sr_float_bits(vs) & 0x7fffffffu) < 0x20800000u ? sr_fpu_soft_signed_zero(r) : r;
}

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
 * Scope of the claim: verified on MinGW-w64 GCC across -O0..-O3 by the
 * committed selftest matrix (literal-argument folding guards, hostile-host
 * matrix, restoration checks); Clang/MSVC are NOT exercised and fail closed
 * at the gates above. */

static inline float sr_fpu_add_s(float a, float b, uint32_t fcr31) {
    volatile float va = a;
    volatile float vb = b;
    const uint32_t saved = sr_fpu_env_save();
    sr_fpu_env_apply_guest(fcr31);
    sr_fpu_scalar_barrier();
    volatile float vr = va + vb;
#if defined(SR_FPU_SOFT_FS)
    if (fcr31 & SR_FCR31_FS) vr = sr_fpu_soft_ftz_sum(vr);
#endif
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
#if defined(SR_FPU_SOFT_FS)
    if (fcr31 & SR_FCR31_FS) vr = sr_fpu_soft_ftz_sum(vr);
#endif
    sr_fpu_scalar_barrier();
    sr_fpu_env_restore(saved);
    return vr;
}

static inline float sr_fpu_mul_s(float a, float b, uint32_t fcr31) {
    volatile float va = a;
    volatile float vb = b;
    const uint32_t a_mag = sr_float_bits(va) & 0x7fffffffu;
    const uint32_t b_mag = sr_float_bits(vb) & 0x7fffffffu;
    if ((a_mag == 0x7f800000u && b_mag == 0u) ||
        (b_mag == 0x7f800000u && a_mag == 0u)) {
        const uint32_t canonical_qnan = 0x7fc00000u;
        float result;
        memcpy(&result, &canonical_qnan, sizeof result);
        return result;
    }
    const uint32_t saved = sr_fpu_env_save();
    sr_fpu_env_apply_guest(fcr31);
    sr_fpu_scalar_barrier();
    volatile float vr = va * vb;
#if defined(SR_FPU_SOFT_FS)
    if (fcr31 & SR_FCR31_FS) vr = sr_fpu_soft_ftz_mul(va, vb, vr);
#endif
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
#if defined(SR_FPU_SOFT_FS)
    if (fcr31 & SR_FCR31_FS) vr = sr_fpu_soft_ftz_div(va, vb, vr);
#endif
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

/* MIPS32 C.cond.fmt needs raw-word ordering: a host comparison can apply DAZ to
 * a denormal before the predicate sees it. The reference lane is independent of
 * this helper, so a mutation of the unordered bit or predicate table is visible. */
static inline unsigned sr_fpu_condition_s(unsigned condition, uint32_t a_bits,
                                          uint32_t b_bits) {
    const unsigned a_sign = a_bits >> 31;
    const unsigned b_sign = b_bits >> 31;
    const unsigned a_mag = a_bits & 0x7fffffffu;
    const unsigned b_mag = b_bits & 0x7fffffffu;
    const unsigned unordered = ((a_mag & 0x7f800000u) == 0x7f800000u &&
                                (a_mag & 0x007fffffu) != 0u) ||
                               ((b_mag & 0x7f800000u) == 0x7f800000u &&
                                (b_mag & 0x007fffffu) != 0u);
    const unsigned equal = !unordered && a_mag == b_mag &&
        (a_mag == 0u || a_sign == b_sign);
    const unsigned less = !unordered &&
        !((a_mag == 0u) && (b_mag == 0u)) &&
        (a_sign != b_sign ? a_sign != 0u :
         (a_sign != 0u ? a_mag > b_mag : a_mag < b_mag));
    return (unordered && (condition & 1u)) ||
           (equal && (condition & 2u)) ||
           (less && (condition & 4u));
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SR_FP_CONVERT_H */
