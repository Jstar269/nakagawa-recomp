/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Shared VFPU oracle input vectors.
 *
 * Included verbatim by BOTH the PSP-side probe and the Nakagawa-side emitter so
 * the two streams cannot drift.  Inputs are raw IEEE-754 bit patterns, never
 * decimal literals: a decimal literal is re-rounded by each compiler and would
 * silently make the two sides disagree about the question being asked.
 *
 * WHY THESE INPUTS
 * ----------------
 * assets/vfpu/ is PPSSPP's *reconstruction* of the Allegrex transcendental
 * tables (see assets/vfpu/PROVENANCE.json).  Upstream is explicit about how far
 * that reconstruction has been validated -- hrydgard/ppsspp issue #21070:
 *
 *   - the tables are "assumed bitwise exact" against several gigabytes of real
 *     PSP data, but "not _exhaustive_ (that would be 32 GB per function)";
 *   - range reduction is "intelligent guessing", not a known hardware algorithm;
 *   - "suspicious outputs for vsin/vcos with large inputs (x>2^32) haven't been
 *     exhaustively tested".
 *
 * pspdev.github.io/vfpu-docs additionally documents that these instructions are
 * approximate by design: roughly the low 2.5-3.5 mantissa bits are inaccurate
 * depending on the operation.  That inaccuracy is deterministic silicon
 * behaviour, so a bitwise comparison is still the right test -- the same input
 * yields the same bits on the same hardware every time.
 *
 * Group A is the general shape sweep: powers of two, values astride 1.0,
 * denormals, IEEE specials, plus midrange controls.
 *
 * Group B targets the upstream-flagged large-argument region directly.  It is
 * the highest-value part of this probe, because it is the one region the
 * upstream maintainers say the reconstruction is least validated in.
 */

#ifndef NAKAGAWA_VFPU_ORACLE_CASES_H
#define NAKAGAWA_VFPU_ORACLE_CASES_H

/* One shared count so a mismatched build cannot compare different-length runs. */
#define VFPU_ORACLE_INPUT_COUNT 46

/* Index of the first Group B entry, so a report can separate the general sweep
 * from the flagged region without re-deriving it. */
#define VFPU_ORACLE_LARGE_ARG_FIRST 24

/* Raw float bits. Keep this list append-only: inserting in the middle changes
 * every downstream digest and silently invalidates prior captures. */
static const unsigned int VFPU_ORACLE_INPUTS[VFPU_ORACLE_INPUT_COUNT] = {
    0x00000000u, /* +0.0                        */
    0x80000000u, /* -0.0                        */
    0x3F800000u, /* +1.0                        */
    0xBF800000u, /* -1.0                        */
    0x3F000000u, /* +0.5                        */
    0x40000000u, /* +2.0                        */
    0x40490FDBu, /* pi                          */
    0x3FC90FDBu, /* pi/2                        */
    0x3F7FFFFFu, /* largest float below 1.0     */
    0x3F800001u, /* smallest float above 1.0    */
    0x33800000u, /* 2^-24, near sin/cos rounding*/
    0x4B800000u, /* 2^24, large-argument range  */
    0x00000001u, /* smallest positive denormal  */
    0x007FFFFFu, /* largest denormal            */
    0x00800000u, /* smallest positive normal    */
    0x7F7FFFFFu, /* FLT_MAX                     */
    0x7F800000u, /* +inf                        */
    0xFF800000u, /* -inf                        */
    0x7FC00000u, /* quiet NaN                   */
    0x7F800001u, /* signalling NaN              */
    0x3E800000u, /* 0.25, exact power of two    */
    0x3FB504F3u, /* sqrt(2), irrational control */
    0x41200000u, /* 10.0, midrange control      */
    0xC1200000u, /* -10.0, negative control     */

    /* ---- Group B: the upstream-flagged x > 2^32 region (indices 24..39) ----
     * vsin/vcos compute sin/cos(pi/2 * x), so correctness here rests entirely on
     * range reduction -- the part upstream describes as guessed.  Exact powers
     * of two are included because a reduction that silently drops low bits still
     * looks plausible on them; the off-power values are what break that. */
    0x4F800000u, /* 2^32   -- the exact flagged boundary        */
    0x4F800001u, /* 2^32 + 1ulp                                 */
    0x4F000000u, /* 2^31   -- just below the boundary           */
    0x50000000u, /* 2^33                                        */
    0x51800000u, /* 2^36                                        */
    0x54800000u, /* 2^42                                        */
    0x5A000000u, /* 2^53   -- double mantissa limit             */
    0x5F000000u, /* 2^63                                        */
    0x6F800000u, /* 2^96                                        */
    0x7E800000u, /* 2^126  -- near overflow                     */
    0x4F800002u, /* 2^32 + 2ulp                                 */
    0x4FFFFFFFu, /* largest value below 2^33                    */
    0xCF800000u, /* -2^32  -- sign symmetry across the flag     */
    0xD0000000u, /* -2^33                                       */
    0x4F4CCCCDu, /* non-power-of-two in the flagged decade      */
    0x5B7F1234u, /* arbitrary large mantissa, stresses reduction*/

    /* ---- Group C: testing the (e & 31) == 1 prediction (indices 40..45) ----
     * Simulating sr_vfpu_cos's reduction shows the sign inversion occurs exactly
     * when the masked shift is 1, i.e. unbiased exponent e = 33, 65, 97, 129.
     * If 2^65 and 2^97 also inverted while 2^34/2^35 do not, the model is
     * confirmed and the anomaly is periodic in e mod 32 -- which is a much
     * sharper question to put to silicon than "large inputs look suspicious". */
    0x50800000u, /* 2^34  -- predicted +1.0 (shift 2)           */
    0x51000000u, /* 2^35  -- predicted +1.0 (shift 3)           */
    0x62000000u, /* 2^69  -- e=69, (e&31)=5, predicted +1.0     */
    0x60000000u, /* 2^65  -- e=65, (e&31)=1, predicted -1.0     */
    0x70000000u, /* 2^97  -- e=97, (e&31)=1, predicted -1.0     */
    0x5F800000u, /* 2^64  -- e=64, e%32==0, predicted +1.0      */
};

/* FNV-1a over the raw result bits.  A digest per operation keeps one hardware
 * launch cheap (HARDWARE_ORACLE.md section 3: bulk-compare to learn THAT
 * something diverged, then localize).  Spot values accompany it so a mismatch
 * still carries some signal without a second run. */
static unsigned int vfpu_oracle_digest_init(void) { return 0x811C9DC5u; }

static unsigned int vfpu_oracle_digest_step(unsigned int h, unsigned int value) {
    for (int byte = 0; byte < 4; ++byte) {
        h ^= (value >> (byte * 8)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

#endif /* NAKAGAWA_VFPU_ORACLE_CASES_H */
