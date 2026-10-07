// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge_raster_ref.h — project-authored reference model of GE triangle setup and
// rasterization rules (issue #696): 12.4 subpixel snapping, top-left coverage,
// screen-linear affine attribute planes anchored at the leftmost vertex, the
// 256-segment piecewise-linear setup reciprocal framework, and the integer
// colour-factor product.
//
// This is a standalone reference module.  It is not wired into the production
// renderer; integration and an old-versus-new differential proof are a later,
// separately scheduled step.  Nothing here is PSP-measured.  Rules stated by
// the #696 behavioural contract are labelled SPEC_ASSUMPTION (#343 oracle
// pending); choices the contract leaves open are explicit SrGeRasterParams
// readings labelled SPEC_AMBIGUITY.  No result is a hardware-accuracy claim.
//
// Contract rules modelled (all SPEC_ASSUMPTION (#343 oracle pending)):
//   R1  screen coordinates live on a fixed 12.4 grid, 16 subpixel units per
//       pixel;
//   R2  coverage follows a top-left fill convention: a sample exactly on an
//       edge belongs to the triangle only when that edge is a top edge (exactly
//       horizontal, with the interior below it) or a left edge (not horizontal,
//       with the interior to its right);
//   R3  depth, colour, fog and texture coordinates are interpolated by
//       screen-linear affine planes, not perspective-correct Z/W evaluation;
//   R4  each plane is anchored at the leftmost vertex;
//   R5  the triangle-area division uses a 256-segment piecewise-linear
//       reciprocal, "with starting values rounded down to multiples of 16";
//   R6  colour factors multiply as ((2*x + 1) * (2*s + 1)) >> 10;
//   R7  the inverse factor 1 - 2*alpha is not clamped to zero at that stage.
//
// Readings the contract does not determine (SPEC_AMBIGUITY):
//   A1  how a host coordinate snaps to 12.4 (truncate, floor or nearest) and
//       the representable range (modelled as unsigned 0 .. 0xFFFF);
//   A2  where in a pixel coverage is sampled (corner or centre);
//   A3  which vertex anchors a plane when two share the leftmost x;
//   A4  the "documented long-edge exception" to leftmost anchoring is not in
//       the sanitized specification and is NOT implemented: the anchored setup
//       entry point takes the anchor index from the caller so the exception can
//       be supplied once it is specified;
//   A5  what "starting values rounded down to multiples of 16" applies to: the
//       reciprocal table's segment start values, the reciprocal's input, or the
//       plane's start value at the anchor — all three readings are selectable;
//   A6  the rounding of a plane gradient (toward zero or toward minus infinity)
//       and its fixed-point width;
//   A7  the in-segment position width of the setup reciprocal and its
//       interpolation form (modelled like the #695 reciprocal:
//       y = base - ((slope * t) >> slope_shift), product truncated);
//   A8  the integer domain of "1" in 1 - 2*alpha (255 or 256), and how a
//       negative inverse factor enters the colour product (modelled with an
//       explicit floor so the result does not depend on the host's signed
//       shift).
//   The full setup-reciprocal coefficient table is not specified; callers
//   supply it and this module ships none.
//
// Hardware-oracle cells needed before any of this can be called measured
// (design only; #343 owns the corpus).  Every cell draws with the vertex
// pipeline in pre-transformed (through) mode, depth/alpha tests off, and reads
// back a small framebuffer window.
//   P1  (R1, A1) a vertex at x = 10 + k/32 pixel for k = 0..31: the first k
//       whose triangle edge moves by one subpixel reveals truncate, floor or
//       nearest snapping.
//   P2  (R2, A2) the 4-pixel right triangle (0,0),(4,0),(0,4) in pixel units:
//       10 covered pixels means corner sampling, 6 means centre sampling; then
//       mirror it to put the hypotenuse on the left/top to confirm which edges
//       own their samples.
//   P3  (R2) a rectangle split into four triangles around an interior point on
//       a sample position, drawn with additive blending at 1/4 intensity: any
//       pixel above 1/4 is double coverage, any 0 inside is a crack.
//   P4  (R3, R4, A3) a flat-depth-gradient triangle whose vertex colours are
//       chosen so leftmost-vertex anchoring and any other anchor differ by one
//       colour step at the far vertex; repeat with two vertices sharing min x.
//   P5  (A4) a long thin triangle whose long edge spans the full width: the
//       far-end colour reveals whether the long-edge exception moves the anchor.
//   P6  (R5, A5, A7) triangles of area 2^k * (1 + i/256) for i = 0..255 with a
//       linear colour ramp: the far-vertex colour error recovers each segment's
//       reciprocal and shows which value is rounded to a multiple of 16.
//   P7  (A6) a triangle with a negative colour gradient whose exact value lies
//       between steps: toward-zero and floor gradients differ by one step.
//   P8  (R6) blend-factor sweep: source colour x and fixed factor s for every
//       x, s in 0..255 using a constant-colour blend mode; compare against
//       ((2x+1)(2s+1))>>10, x*s/255 and (x*s)>>8.
//   P9  (R7, A8) alpha 0..255 with an inverse-alpha factor: values above 128
//       show whether the factor went negative (darkening below the clamped
//       result) and whether "1" is 255 or 256.
//
// Coordinate convention: x grows right, y grows down, both in 12.4 subpixel
// units.  A pixel (px, py) is sampled at (16*px + o, 16*py + o) where o is the
// SrGeRasterParams sample offset.

#ifndef NAKAGAWA_GE_RASTER_REF_H
#define NAKAGAWA_GE_RASTER_REF_H

#include <stddef.h>
#include <stdint.h>

#define SR_GE_SUBPIXELS 16      /* SPEC_ASSUMPTION (#343 oracle pending), R1 */
#define SR_GE_COORD_MAX 0xFFFFu /* SPEC_AMBIGUITY A1: unsigned 12.4 range */

/* Status flags; every entry point ORs what it did. */
#define SR_GE_RASTER_FLAG_INEXACT 0x01u    /* rounding discarded nonzero bits */
#define SR_GE_RASTER_FLAG_RANGE 0x02u      /* a coordinate fell outside the 12.4 range */
#define SR_GE_RASTER_FLAG_DEGENERATE 0x04u /* zero area: no coverage, no plane */
#define SR_GE_RASTER_FLAG_OVERFLOW 0x08u   /* a plane value left the int64 range */
#define SR_GE_RASTER_FLAG_INVALID 0x10u    /* bad argument, parameter or table */

typedef enum SrGeSnap {
    SR_GE_SNAP_TRUNCATE = 0, /* toward zero */
    SR_GE_SNAP_FLOOR = 1,    /* toward minus infinity */
    SR_GE_SNAP_NEAREST = 2   /* floor(16*x + 1/2) */
} SrGeSnap;

typedef enum SrGeAnchorTie {
    SR_GE_ANCHOR_TIE_TOPMOST = 0, /* smallest y among the leftmost vertices */
    SR_GE_ANCHOR_TIE_FIRST = 1    /* lowest vertex index among the leftmost */
} SrGeAnchorTie;

typedef enum SrGeStart16 {
    SR_GE_START16_TABLE_BASES = 0, /* every reciprocal segment base is a multiple of 16 */
    SR_GE_START16_RECIP_INPUT = 1, /* the doubled area is rounded down to a multiple of 16 */
    SR_GE_START16_PLANE_START = 2  /* the plane value at the anchor is rounded down */
} SrGeStart16;

typedef enum SrGeGradRound {
    SR_GE_GRAD_TOWARD_ZERO = 0,
    SR_GE_GRAD_FLOOR = 1
} SrGeGradRound;

typedef struct SrGeRasterParams {
    SrGeSnap snap;           /* A1 */
    unsigned sample_offset;  /* A2: 0..15 subpixels; 0 = pixel corner, 8 = centre */
    SrGeAnchorTie anchor_tie; /* A3 */
    SrGeStart16 start16;     /* A5 */
    SrGeGradRound grad_round; /* A6 */
    unsigned grad_frac_bits; /* A6: plane values carry this many fraction bits */
} SrGeRasterParams;

/* 1 when every field is in range, 0 otherwise. */
int sr_ge_raster_params_valid(const SrGeRasterParams* params);

typedef struct SrGeVertex {
    int32_t x; /* 12.4, 0..SR_GE_COORD_MAX */
    int32_t y; /* 12.4, 0..SR_GE_COORD_MAX */
} SrGeVertex;

/* Snap a host pixel coordinate to 12.4.  Out-of-range or non-finite input
   raises RANGE or INVALID and leaves *out at 0. */
uint32_t sr_ge_snap_12_4(const SrGeRasterParams* params, double pixels, int32_t* out);

/* Twice the signed area in subpixel^2 units: positive when a, b, c run
   clockwise on screen (y down). */
int64_t sr_ge_area2(const SrGeVertex* a, const SrGeVertex* b, const SrGeVertex* c);

/* Coverage of one pixel; 1 covered, 0 not.  Winding does not matter. */
int sr_ge_raster_covers(const SrGeRasterParams* params, const SrGeVertex tri[3], int32_t px, int32_t py);

/* Coverage of the window [0, width) x [0, height): mask[py * width + px] is
   set to 1 for covered pixels and 0 otherwise.  Returns flags (DEGENERATE for
   a zero-area triangle, which covers nothing). */
uint32_t sr_ge_raster_coverage(const SrGeRasterParams* params, const SrGeVertex tri[3], uint8_t* mask, int32_t width,
                               int32_t height);

/* ---- 256-segment setup reciprocal framework (R5, A5, A7) ---------------- */

#define SR_GE_SETUP_SEGMENTS 256
#define SR_GE_SETUP_INDEX_BITS 8
#define SR_GE_SETUP_FRAC_MIN 8u
#define SR_GE_SETUP_FRAC_MAX 20u
#define SR_GE_SETUP_T_BITS_MAX 16u

/* Caller-supplied coefficients.  For a doubled area 2^k * (1 + f) the segment
   is the top 8 bits of f and t the next t_bits bits; the table value
   y = base[i] - ((slope[i] * t) >> slope_shift) is read as 1/(1 + f) in
   unsigned fixed point with out_frac_bits fraction bits, and must lie in
   [2^(out_frac_bits-1), 2^out_frac_bits]. */
typedef struct SrGeSetupRecipTable {
    uint32_t base[SR_GE_SETUP_SEGMENTS];
    uint32_t slope[SR_GE_SETUP_SEGMENTS];
    unsigned out_frac_bits; /* SR_GE_SETUP_FRAC_MIN..SR_GE_SETUP_FRAC_MAX */
    unsigned t_bits;        /* 0..SR_GE_SETUP_T_BITS_MAX */
    unsigned slope_shift;   /* 0..31 */
} SrGeSetupRecipTable;

/* 0 when the table is usable under the reading in params (the TABLE_BASES
   reading additionally requires every base to be a multiple of 16);
   SR_GE_RASTER_FLAG_INVALID otherwise. */
uint32_t sr_ge_setup_table_check(const SrGeRasterParams* params, const SrGeSetupRecipTable* table);

/* Segment index and in-segment position for a positive doubled area. */
unsigned sr_ge_setup_segment(const SrGeSetupRecipTable* table, uint64_t area2, unsigned* t_out);

/* ---- affine attribute planes (R3, R4) ----------------------------------- */

#define SR_GE_ATTR_MIN (-(INT32_C(1) << 23))
#define SR_GE_ATTR_MAX ((INT32_C(1) << 23) - 1)
#define SR_GE_GRAD_FRAC_MAX 20u

/* One attribute plane: value(x, y) = start + dx * (x - ax) + dy * (y - ay)
   in fixed point with grad_frac_bits fraction bits, x and y in subpixels. */
typedef struct SrGePlane {
    int32_t ax, ay; /* anchor vertex position */
    int64_t start;  /* anchor attribute value, fixed point */
    int64_t dx, dy; /* per-subpixel gradients, fixed point */
    unsigned frac_bits;
} SrGePlane;

/* Index (0..2) of the leftmost vertex under the params' tie reading (R4, A3). */
int sr_ge_plane_anchor_leftmost(const SrGeRasterParams* params, const SrGeVertex tri[3]);

/* Set up one plane anchored at tri[anchor] for per-vertex attribute values
   attr[0..2] (each SR_GE_ATTR_MIN..SR_GE_ATTR_MAX, caller fixed point).  The
   params' grad_frac_bits must not exceed the table's out_frac_bits. */
uint32_t sr_ge_plane_setup_anchored(const SrGeRasterParams* params, const SrGeSetupRecipTable* table,
                                    const SrGeVertex tri[3], const int32_t attr[3], int anchor, SrGePlane* out);

/* sr_ge_plane_setup_anchored() at the leftmost vertex. */
uint32_t sr_ge_plane_setup(const SrGeRasterParams* params, const SrGeSetupRecipTable* table, const SrGeVertex tri[3],
                           const int32_t attr[3], SrGePlane* out);

/* Plane value at a subpixel position; OVERFLOW leaves *out at 0. */
uint32_t sr_ge_plane_eval(const SrGePlane* plane, int32_t x, int32_t y, int64_t* out);

/* ---- colour factors (R6, R7, A8) ---------------------------------------- */

/* ((2x + 1)(2s + 1)) >> 10 for x, s in 0..255; UINT32_MAX for an argument
   out of range. */
uint32_t sr_ge_color_mul(uint32_t x, uint32_t s);

/* ONE - 2*alpha, unclamped, for alpha 0..255 and one = 255 or 256 (A8);
   returns INT32_MIN for an argument out of range. */
int32_t sr_ge_inverse_factor(uint32_t alpha, uint32_t one);

/* floor(((2x + 1)(2s + 1)) / 1024) for x in 0..255 and a signed factor s in
   -512..512 (A8).  Returns INT32_MIN for an argument out of range. */
int32_t sr_ge_color_mul_signed(uint32_t x, int32_t s);

#endif /* NAKAGAWA_GE_RASTER_REF_H */
