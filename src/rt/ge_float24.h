// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge_float24.h — project-authored reference model of GE float24 arithmetic,
// row-sum matrix transforms and the piecewise-linear perspective reciprocal
// framework (issue #695).
//
// This is a standalone reference module.  It is not wired into the production
// renderer; integration and an old-versus-new differential proof are a later,
// separately scheduled step.  Nothing here is PSP-measured: every behaviour is
// either taken from the #695 behavioural contract and labelled
// SPEC_ASSUMPTION (#343 oracle pending), or left open by that contract and
// labelled SPEC_AMBIGUITY — as an explicit SrGeF24Policy reading where both
// readings are implementable, otherwise as a fixed, named project reading.  No
// result of this module is a hardware-accuracy claim.
//
// Format: the contract fixes 1 sign, 8 exponent and 15 fraction bits with a
// 16-bit significand when normalised.  An SrGeF24 holds those 24 bits in its
// low bits — sign [23], biased exponent [22:15], fraction [14:0] — and bits
// [31:24] must be zero (every entry point masks them off).
//
// Rules stated by the contract (all SPEC_ASSUMPTION (#343 oracle pending)):
//   A2  finite conversion and arithmetic truncate toward zero to the 16-bit
//       significand (sign-magnitude truncation: magnitude never grows);
//   A3  denormal inputs and results flush to zero;
//   A4  addition aligns both operands to the larger exponent, truncates each
//       aligned operand to the 16-bit significand grid at that exponent, and
//       sums with no guard precision; the sum is then normalised, truncating a
//       carry-out bit toward zero (rule A2);
//   A5  a matrix row is evaluated as one multi-operand row sum: every term is
//       aligned to the largest term exponent and truncated to the term grid
//       there, the aligned terms are summed exactly, and the sum is truncated
//       once — not as a chain of independently rounded scalar additions;
//   A6  World, View and Projection are composed into one matrix before any
//       vertex is transformed;
//   A7  the perspective reciprocal is a 128-segment piecewise-linear lookup.
//
// Fixed project readings where the contract is silent (SPEC_AMBIGUITY; each
// has an oracle cell below and is not configurable here):
//   A1  bit layout and bias: a normal value is
//       (-1)^sign * 1.fraction * 2^(exponent - 127), i.e. the word is read as
//       the upper 24 bits of an IEEE-754 binary32 pattern, and exponent field 0
//       is the zero/denormal class;
//   A8  the 128 segments partition the significand interval [1, 2) uniformly
//       by the top 7 fraction bits, leaving 8 bits of in-segment position t;
//   A9  in-segment interpolation is y = base - ((slope * t) >> slope_shift)
//       with the product truncated, y read as a fixed-point 1/significand
//       (the widths are caller-supplied table fields).
//
// Configurable readings (SPEC_AMBIGUITY, see SrGeF24Policy):
//   exponent-255 patterns (IEEE-like Inf/NaN vs. an ordinary finite exponent),
//   overflow (Inf vs. saturation), the sign of generated zeros, whether row-sum
//   terms are truncated products or exact products, and the association order
//   of the World/View/Projection composition.  NaN payloads are not modelled:
//   every NaN result is the canonical pattern SR_GE_F24_CANONICAL_NAN.  The
//   reciprocal coefficient table is not specified at all; callers supply it.
//
// Hardware-oracle cells needed before any of the above can be called measured
// (design only; #343 owns the corpus).  Observable "Z" means the 16-bit depth
// word written for one point primitive at a fixed pixel with depth test off and
// depth write on; "XY" means the framebuffer pixel the point lands on.  World
// and View are identity and Projection carries the probe unless stated; the
// viewport scale is chosen so one float24 unit in the last place at the probed
// exponent maps to one observable step.
//   O1  (A1, A2) float32 vertex x = 0x3F8000FF and 0xBF8000FF: truncation
//       reports 1.0 and -1.0; round-to-nearest would report 1.0 + 1 ulp and
//       -1.0 - 1 ulp.  Repeat with low byte 0x80 to separate ties.
//   O2  (A3) vertex x carrying an exponent-0, fraction-0x4000 float32: flush
//       reports exactly the viewport offset; a gradual-underflow unit would not.
//   O3  (A4) Projection row (1.0, -1.0, 0, 0) applied to (1.0, 2^-16, 0, 1):
//       guard-bit-less alignment drops 2^-16 entirely and reports 1.0; any
//       adder that keeps guard bits reports the exactly representable
//       1 - 2^-16 (0x3F7FFF).
//   O4  (A4, carry-out) row (1.0, 1.0) on (0x3FFFFF, 0x3F8000) as float24:
//       the 17-bit sum 0x17FFF loses its low bit, so a truncated carry reports
//       0x403FFF and a rounded carry reports 0x404000.
//   O5  (A5) row (2^20, 1, -2^20, 1) on (1, 1, 1, 1): a row sum of truncated
//       products aligns both 1.0 terms off the 16-bit grid and reports 0; a row
//       sum of exact products keeps them on its 32-bit grid and reports 2.0; a
//       sequential chain reports 1.0.  Permuting the columns leaves a row sum
//       unchanged but changes a chain.
//   O6  (A5 product reading) row (1 + 2^-15, 1 + 2^-15) on (1 + 2^-15, -1):
//       truncated products and exact products differ in the last place.
//   O7  (A6, compose order) diagonal W, V, P with m[0][0] = 0x3D82E9,
//       0x42F07A, 0x3D5A83: (P*V)*W gives 0x3ED1EC and P*(V*W) gives 0x3ED1ED,
//       so the observed XY/Z selects the association.  With 0xC07FFF, 0x40E295,
//       0x400000 and vertex x = 0x420185, the composed matrix gives 0xC4E544
//       while applying W, V and P one after another gives 0xC4E543 (values
//       under truncated products; the selftest pins both).
//   O8  (overflow, exponent 255) a row whose exact sum exceeds the largest
//       exponent-254 value: Inf vs. saturation vs. finite exponent 255 give
//       clipped, edge-pinned or finite-but-huge Z respectively.
//   O9  (signed zero) rows producing -0 by flush, by product with -0 and by
//       exact cancellation, fed into a following row (0, 1/zero) path: the sign
//       of the resulting infinity, or saturation, exposes the zero's sign.
//   O10 (A7, A8, A9) sweep w over every fraction 0..0x7FFF at one exponent with
//       x = w (so projected x = x/w): the reported values recover each segment's
//       base and slope; kinks must fall at fraction multiples of 0x100.
//
// Vector/matrix convention (reference-API choice, not a hardware claim):
// column vectors, out = M * v, element m[row][column].

#ifndef NAKAGAWA_GE_FLOAT24_H
#define NAKAGAWA_GE_FLOAT24_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t SrGeF24;

#define SR_GE_F24_MASK 0x00FFFFFFu
#define SR_GE_F24_SIGN_BIT 0x00800000u
#define SR_GE_F24_EXP_MASK 0x007F8000u
#define SR_GE_F24_FRAC_MASK 0x00007FFFu
#define SR_GE_F24_EXP_SHIFT 15
#define SR_GE_F24_EXP_BIAS 127 /* SPEC_AMBIGUITY project reading A1 (#343 oracle pending) */
#define SR_GE_F24_SIG_BITS 16  /* hidden bit + 15 fraction bits */
#define SR_GE_F24_CANONICAL_NAN 0x007FC000u
#define SR_GE_F24_ONE 0x003F8000u

/* Status flags.  Every operation ORs the flags of everything it did. */
#define SR_GE_F24_FLAG_INEXACT 0x01u   /* truncation discarded nonzero bits */
#define SR_GE_F24_FLAG_FLUSHED 0x02u   /* a denormal input or result became zero */
#define SR_GE_F24_FLAG_OVERFLOW 0x04u  /* a result exceeded the largest finite exponent */
#define SR_GE_F24_FLAG_SPECIAL 0x08u   /* an Inf/NaN was consumed or produced */
#define SR_GE_F24_FLAG_CANCELLED 0x10u /* nonzero terms summed to exactly zero */
#define SR_GE_F24_FLAG_DIV_ZERO 0x20u  /* reciprocal of zero */
#define SR_GE_F24_FLAG_INVALID 0x40u   /* bad policy, table or argument; value is not meaningful */

/* SPEC_AMBIGUITY: what an exponent field of 255 means. */
typedef enum SrGeF24Exp255 {
    SR_GE_F24_EXP255_SPECIAL = 0, /* IEEE-like: fraction 0 is Inf, nonzero is NaN */
    SR_GE_F24_EXP255_FINITE = 1   /* an ordinary finite exponent; no Inf/NaN exist */
} SrGeF24Exp255;

/* SPEC_AMBIGUITY: the result of exceeding the largest finite exponent. */
typedef enum SrGeF24Overflow {
    SR_GE_F24_OVERFLOW_INF = 0,     /* signed Inf; requires SR_GE_F24_EXP255_SPECIAL */
    SR_GE_F24_OVERFLOW_SATURATE = 1 /* signed largest finite magnitude */
} SrGeF24Overflow;

/* SPEC_AMBIGUITY: the sign of a zero the arithmetic generates. */
typedef enum SrGeF24ZeroSign {
    /* IEEE-754 round-toward-zero sign rules: a flushed value keeps its sign,
       exact cancellation and (+0) + (-0) give +0, (-0) + (-0) gives -0, and a
       product of zeros takes the XOR of the operand signs. */
    SR_GE_F24_ZERO_IEEE = 0,
    SR_GE_F24_ZERO_POSITIVE = 1 /* every zero result is +0 */
} SrGeF24ZeroSign;

/* SPEC_AMBIGUITY: how a product enters a multi-operand row sum. */
typedef enum SrGeF24Product {
    SR_GE_F24_PRODUCT_TRUNCATED = 0, /* each product is first truncated to float24 */
    SR_GE_F24_PRODUCT_EXACT = 1      /* each product keeps its exact 32-bit significand */
} SrGeF24Product;

/* SPEC_AMBIGUITY: association order of the World/View/Projection composition. */
typedef enum SrGeF24Compose {
    SR_GE_F24_COMPOSE_PV_THEN_W = 0, /* M = (P * V) * W */
    SR_GE_F24_COMPOSE_P_THEN_VW = 1  /* M = P * (V * W) */
} SrGeF24Compose;

typedef struct SrGeF24Policy {
    SrGeF24Exp255 exp255;
    SrGeF24Overflow overflow;
    SrGeF24ZeroSign zero_sign;
    SrGeF24Product product;
    SrGeF24Compose compose;
} SrGeF24Policy;

typedef struct SrGeF24Result {
    SrGeF24 value;
    uint32_t flags;
} SrGeF24Result;

typedef struct SrGeF24Mat4 {
    SrGeF24 m[4][4];
} SrGeF24Mat4;

/* Largest term count accepted by sr_gef24_rowsum(). */
#define SR_GE_F24_ROWSUM_MAX 8

/* Returns 1 when every field holds a known enumerator and the combination is
   meaningful (OVERFLOW_INF needs EXP255_SPECIAL), 0 otherwise.  Every entry
   point taking a policy rejects an invalid one with SR_GE_F24_FLAG_INVALID. */
int sr_gef24_policy_valid(const SrGeF24Policy* policy);

/* binary32 -> float24 by dropping the low 8 fraction bits (truncation toward
   zero, A2), flushing denormals (A3) and applying the exponent-255 reading. */
SrGeF24Result sr_gef24_from_binary32(const SrGeF24Policy* policy, uint32_t bits);

/* float24 -> binary32 bit pattern; exact (bits << 8). */
uint32_t sr_gef24_to_binary32(SrGeF24 value);

/* float24 -> double, exact.  Denormal patterns read as signed zero under the
   policy's zero rule; exponent 255 reads as Inf/NaN or as a finite value. */
double sr_gef24_to_double(const SrGeF24Policy* policy, SrGeF24 value);

/* Guard-bit-less addition (A4). */
SrGeF24Result sr_gef24_add(const SrGeF24Policy* policy, SrGeF24 a, SrGeF24 b);

/* Multiplication: exact 32-bit significand product truncated to 16 bits (A2). */
SrGeF24Result sr_gef24_mul(const SrGeF24Policy* policy, SrGeF24 a, SrGeF24 b);

/* One multi-operand row sum, sum(a[i] * b[i]) for 1 <= n <= SR_GE_F24_ROWSUM_MAX (A5). */
SrGeF24Result sr_gef24_rowsum(const SrGeF24Policy* policy, const SrGeF24* a, const SrGeF24* b, size_t n);

/* out = lhs * rhs, each element one 4-term row sum.  out may alias an input.
   Returns the OR of the element flags. */
uint32_t sr_gef24_mat4_mul(const SrGeF24Policy* policy, const SrGeF24Mat4* lhs, const SrGeF24Mat4* rhs,
                           SrGeF24Mat4* out);

/* World/View/Projection pre-composition (A6) in the policy's association order. */
uint32_t sr_gef24_compose_wvp(const SrGeF24Policy* policy, const SrGeF24Mat4* world, const SrGeF24Mat4* view,
                              const SrGeF24Mat4* proj, SrGeF24Mat4* out);

/* out = m * v, each component one 4-term row sum.  out may alias v. */
uint32_t sr_gef24_mat4_apply(const SrGeF24Policy* policy, const SrGeF24Mat4* m, const SrGeF24 v[4], SrGeF24 out[4]);

/* ---- 128-segment piecewise-linear reciprocal framework (A7-A9) ---------- */

#define SR_GE_RECIP_SEGMENTS 128
#define SR_GE_RECIP_INDEX_BITS 7
#define SR_GE_RECIP_T_BITS 8 /* 15 fraction bits - 7 index bits */
#define SR_GE_RECIP_FRAC_MIN 16u
#define SR_GE_RECIP_FRAC_MAX 30u

/* Caller-supplied coefficients.  The contract specifies the mechanism but not
   the coefficients, so this module ships no table.  For a positive normal
   significand 1.f with segment i and position t, the table value is
   y = base[i] - ((slope[i] * t) >> slope_shift), read as 1/significand in
   unsigned fixed point with out_frac_bits fraction bits; every y must lie in
   [2^(out_frac_bits-1), 2^out_frac_bits]. */
typedef struct SrGeRecipTable {
    uint32_t base[SR_GE_RECIP_SEGMENTS];
    uint32_t slope[SR_GE_RECIP_SEGMENTS];
    unsigned out_frac_bits; /* SR_GE_RECIP_FRAC_MIN..SR_GE_RECIP_FRAC_MAX */
    unsigned slope_shift;   /* 0..31 */
} SrGeRecipTable;

/* 0 when the table's parameters and every segment's endpoints are in range;
   SR_GE_F24_FLAG_INVALID otherwise. */
uint32_t sr_ge_recip_table_check(const SrGeRecipTable* table);

/* Segment index (top 7 fraction bits) of a value; *t_out (optional) receives
   the 8-bit in-segment position. */
unsigned sr_ge_recip_segment(SrGeF24 value, unsigned* t_out);

/* Table value y for one value's significand, or 0 for an invalid table. */
uint32_t sr_ge_recip_table_value(const SrGeRecipTable* table, SrGeF24 value);

/* 1/x through the table, normalised and truncated to float24.  Zero raises
   SR_GE_F24_FLAG_DIV_ZERO and yields the policy's overflow result; Inf yields
   zero; NaN yields the canonical NaN. */
SrGeF24Result sr_ge_recip(const SrGeF24Policy* policy, const SrGeRecipTable* table, SrGeF24 value);

#endif /* NAKAGAWA_GE_FLOAT24_H */
