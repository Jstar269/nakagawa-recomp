/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Shared VFPU NaN/Inf matrix-and-multiply probe inputs (issue #40).
 *
 * Included verbatim by the PSP-side probe so the record shape cannot drift
 * from the published analysis.  Inputs are raw IEEE-754 bit patterns, never
 * decimal literals: a decimal literal is re-rounded by each compiler and would
 * silently change the question being asked.
 *
 * WHY THESE INPUTS
 * ----------------
 * The host-synthetic differential audit of issue #40 reduced 23-24 diverging
 * vmmul/vtfm instruction words (all op-family 0x3C, sub 0..3, 3-lane) to a
 * single mechanism: when one output lane's dot product meets two different
 * NaNs, x86-64 SSE's ADDSS returns the *first* NaN source, and the two host
 * implementations (generated C expression vs. interpreter loop) compiled to
 * opposite addss operand orders at -O0, so each kept a different NaN
 * (0xFFC00000 from an invalid inf*0 product vs. 0x7FC00001, the quieted input
 * sNaN).  None of those host words is established PSP behavior.
 *
 * Silicon result-bit evidence (accepted hardware-oracle record): the probe
 * was repaired and run on physical PSP-3000 (ARK-5 6.61) across 20 runs per
 * vector.  Established cells:
 *   - sNaN quieting: 0x7FC00001 (quiet bit set, payload preserved);
 *   - order independence: a lane whose dot product meets two different NaNs
 *     produces one stable word regardless of operand order;
 *   - default invalid NaN: 0x7FC00000 (invalid inf*0 product);
 *   - FTZ: subnormal products are flushed to signed zero;
 *   - zero sign and inf/nan propagation preserved across all vectors.
 * The per-case output words are recorded in the accepted hardware-evidence
 * lane (PSPLink/private records), not reproduced here; this public fixture
 * carries the inputs and the established cells only.
 *
 * Public-source evidence status (see the issue audit):
 *   - PSPAutotests tests/cpu/vfpu/matrix.c covers vmmul/vtfm with ordinary
 *     finite matrices only; the NaN/Inf cases in vector.c are recorded as
 *     "%f" text ("nan"), so payloads and signs are not recorded anywhere.
 *   - PPSSPP's default vmmul/vtfm path is a plain accumulation loop; its
 *     opt-in "accurate" dot path (vfpu_dot) canonicalizes any NaN dot result
 *     to 0x7F800001 and flushes subnormal products to signed zero, but
 *     hrydgard/ppsspp issue #21070 states vfpu_dot is confirmed *not* to
 *     exactly match PSP hardware, so that model is corroboration, not
 *     hardware evidence.  PPSSPP behavior is never promoted to hardware
 *     truth here.
 *   - The host words seen in the differential (0xFFC00000 vs 0x7FC00001)
 *     are host artifacts of the -O0 addss operand order, not PSP behavior;
 *     neither equals the measured default invalid NaN 0x7FC00000.
 *
 * MATRIX LAYOUT
 * -------------
 * Matches the hardware/PPSSPP matrix register layout (ReadMatrix): for a 3x3
 * (t-size) matrix, element (column c, row r) of matrix M<n> lives at
 * S<n><c><r>.  So:
 *   M000 -> S000,S001,S002 | S010,S011,S012 | S020,S021,S022
 *   M100 -> S100,S101,S102 | S110,S111,S112 | S120,S121,S122
 *   M200 -> S200,S201,S202 | S210,S211,S212 | S220,S221,S222
 * vmmul computes Mvd[a][b] = sum_c Mvs[b][c] * Mvt[a][c] (the PPSSPP/codegen
 * convention, which is what the host audit used).
 */

#ifndef NAKAGAWA_VFPU_NAN_CASES_H
#define NAKAGAWA_VFPU_NAN_CASES_H

#define VFPU_NAN_CASE_COUNT 8

typedef struct {
    const char *id;          /* stable case id, printed verbatim          */
    unsigned op;             /* 0 = vmmul.t M200, M000, M100             */
                             /* 1 = vtfm3.t C200, M100, C000             */
    unsigned int in[18];     /* raw bits: M000[9] then M100[9]           */
} VfpuNanCase;

/* One divergent-lane dot product per case.  Lane (0,0) of the vmmul result is
 *   M100[0][0]*M000[0][0] + M100[0][1]*M000[0][1] + M100[0][2]*M000[0][2]
 * (for vtfm3, d[0] = M100[0][k]*C000[k] over k=0..2 with the same matrix
 * convention).  Every case keeps the other lanes finite (all-zero matrices
 * produce +0.0 lanes) so a divergence in lane (0,0) is unambiguous. */
static const VfpuNanCase VFPU_NAN_CASES[VFPU_NAN_CASE_COUNT] = {
    {
        "vmmul-nan-payload", 0,
        /* M000: {+0.0, +sNaN, +0.0 | 0,0,0 | 0,0,0}
           M100: {+inf, +0.0, +0.0 | 0,0,0 | 0,0,0}
           lane = inf*0 + 0*sNaN + 0*0 -> two different NaNs meet. */
        {0x00000000u, 0x7F800001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x7F800000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    },
    {
        "vmmul-nan-reversed", 0,
        /* M000: {+sNaN, +0.0, +0.0 | 0,0,0 | 0,0,0}
           M100: {+0.0, +inf, +0.0 | 0,0,0 | 0,0,0}
           lane = 0*sNaN + inf*0 + 0*0 -> same two NaNs, reversed order. */
        {0x7F800001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x7F800000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    },
    {
        "vmmul-snan-quiet", 0,
        /* M000: {+0.0, +1.0, +0.0 | 0,0,0 | 0,0,0}
           M100: {+0.0, +sNaN, +0.0 | 0,0,0 | 0,0,0}
           lane = 0*0 + 1*sNaN + 0*0 -> single NaN: quieting/payload only. */
        {0x00000000u, 0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x7F800001u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    },
    {
        "vmmul-nsnan-quiet", 0,
        /* M000: {+0.0, -1.0, +0.0 | 0,0,0 | 0,0,0}
           M100: {+0.0, -sNaN(0xFF800001), +0.0 | 0,0,0 | 0,0,0}
           lane = 0*0 + -1*-sNaN + 0*0 -> negative sNaN: sign preservation. */
        {0x00000000u, 0xBF800000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xFF800001u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    },
    {
        "vmmul-inf-times-zero", 0,
        /* M000: {+0.0, +0.0, +0.0 | 0,0,0 | 0,0,0}
           M100: {+inf, +inf, +0.0 | 0,0,0 | 0,0,0}
           lane = inf*0 + inf*0 + 0*0 -> invalid inf*0 NaN word, no sNaN. */
        {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x7F800000u, 0x7F800000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    },
    {
        "vmmul-inf-minus-inf", 0,
        /* M000: {+inf, +inf, +0.0 | 0,0,0 | 0,0,0}
           M100: {-inf, +inf, +0.0 | 0,0,0 | 0,0,0}
           lane = inf*-inf + inf*inf + 0*0 -> -inf + +inf invalid NaN word. */
        {0x7F800000u, 0x7F800000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0xFF800000u, 0x7F800000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    },
    {
        "vmmul-subnormal-product", 0,
        /* M000: {+1.0, +0.0, +0.0 | 0,0,0 | 0,0,0}
           M100: {+min-denormal(0x00000001), +0.0, +0.0 | 0,0,0 | 0,0,0}
           lane = 1.0*denorm + 0*0 + 0*0 -> is the subnormal product
           preserved or flushed to signed zero? */
        {0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    },
    {
        "vtfm3-nan-payload", 1,
        /* vtfm3.t C200, M100, C000 with C000 = {+0.0, +sNaN, +0.0} and the
           M100 matrix of case 1.  d[0] = inf*0 + 0*sNaN + 0*0 -> two NaNs
           in the matrix/vector multiply chain. */
        {0x00000000u, 0x7F800001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x7F800000u, 0x00000000u, 0x00000000u,
         0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    },
};

#endif /* NAKAGAWA_VFPU_NAN_CASES_H */
