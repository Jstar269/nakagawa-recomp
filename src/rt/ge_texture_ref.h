// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge_texture_ref.h — project-authored reference model of GE texture sampling
// rules (issue #703): 16-bit texel unpacking, CLUT index derivation and
// lookup, power-of-two wrap and clamp addressing, and 4-bit fixed-point
// bilinear filtering.
//
// This is a standalone reference module.  It is not wired into the production
// renderer; integration and an old-versus-new differential proof are a later,
// separately scheduled step.  Nothing here is PSP-measured.  Rules stated by
// the #703 behavioural contract are labelled SPEC_ASSUMPTION (#343 oracle
// pending); choices it leaves open are explicit SrGeTexParams readings
// labelled SPEC_AMBIGUITY.  No result is a hardware-accuracy claim.
//
// Contract rules modelled (all SPEC_ASSUMPTION (#343 oracle pending)):
//   T1  16-bit texels come in 5650, 5551 and 4444 layouts;
//   T2  channel widening: 5-bit v -> v*255/31, 6-bit v -> v*255/63 (integer
//       division, truncating), 4-bit v -> v*17, 1-bit alpha -> 0 or 255;
//   T3  CLUT indices are derived with an explicit shift and mask;
//   T4  power-of-two wrapping masks the coordinate with dimension - 1, and
//       clamping is a separate mode;
//   T5  bilinear filtering uses 4-bit sub-texel weights u, v in 0..15 and
//       ((c00*(16-u) + c01*u)*(16-v) + (c10*(16-u) + c11*u)*v + 128) >> 8 per
//       channel; the final +128 is itself an assumption the contract flags.
//
// Readings the contract does not determine (SPEC_AMBIGUITY):
//   B1  channel bit order inside a texel: red in the low bits (then green,
//       blue, alpha upward) or blue in the low bits; alpha is the top field;
//   B2  byte order of a 16-bit texel in memory (little or big endian);
//   B3  CLUT index order of operations: (raw >> shift) & mask or
//       (raw & mask) >> shift;
//   B4  which nibble of a byte holds the even texel of a 4-bit indexed
//       texture;
//   B5  the bilinear rounding bias (128 or 0);
//   B6  the texel-centre offset subtracted from a sample coordinate before it
//       splits into a texel index and a 4-bit fraction (0 or 8 sixteenths);
//   B7  wrapping a non-power-of-two dimension is unspecified and fails closed
//       with SR_GE_TEX_FLAG_INVALID; an index past the end of a CLUT fails
//       closed the same way.  Clamp is modelled for any dimension (a project
//       reading: clamping needs no power of two);
//   B8  boundary weights: the 4-bit weights stop at 15, so a sample exactly on
//       a texel boundary is modelled as the next texel with weight 0 rather
//       than the previous texel with weight 16 (the two agree in exact
//       arithmetic; hardware may differ in rounding).  The per-channel
//       intermediate is modelled as an exact unsigned value of at most
//       255*256 + 128 — the hardware width is SPEC_ASSUMPTION (#343 oracle
//       pending);
//   B9  4-bit indexed rows are modelled as packed continuously: texel
//       y*stride + x selects the nibble, so with an odd stride a row starts
//       mid-byte.  Whether rows are byte-aligned is unspecified.
//   The 32-bit 8888 layout is included for CLUT entries and direct texels; its
//   field order follows B1 and it needs no widening.
//
// Hardware-oracle cells needed before any of this can be called measured
// (design only; #343 owns the corpus).  Each draws one textured sprite in
// through mode with a nearest or linear filter and reads back the framebuffer
// as 8888.
//   X1  (T1, T2, B1, B2) a 32x1 texture holding every 5-bit value in one
//       channel of 5551 (and 5650 green for 6-bit, 4444 for 4-bit): the
//       readback separates v*255/31 from bit replication at v = 16 (131 vs
//       132) and names the channel order and byte order.
//   X2  (T3, B3) an 8-bit indexed texture with raw 0xAB and shift 4, mask
//       0x0F: shift-then-mask reads entry 0x0A, mask-then-shift entry 0x00.
//   X3  (B4) a 4-bit indexed 2x1 texture with byte 0x21 and a CLUT whose entry
//       1 and 2 differ: the left texel names the nibble order.
//   X4  (T4, B7) a 4x4 texture sampled at u = -1, 4 and 5 under wrap and
//       clamp; repeat with a 3-wide texture to see what non-power-of-two
//       wrapping does.
//   X5  (T5, B5, B6) a 2x1 texture (0, 255) sampled across sixteen sub-texel
//       positions with a linear filter: the midpoint reads 128 with a +128
//       bias and 127 without, and the position of the first change gives the
//       centre offset.

#ifndef NAKAGAWA_GE_TEXTURE_REF_H
#define NAKAGAWA_GE_TEXTURE_REF_H

#include <stddef.h>
#include <stdint.h>

#define SR_GE_TEX_FLAG_INVALID 0x01u /* bad argument or unspecified case; the result is not meaningful */

typedef struct SrGeRgba8 {
    uint8_t r, g, b, a;
} SrGeRgba8;

typedef enum SrGeTexFormat {
    SR_GE_TEX_5650 = 0,
    SR_GE_TEX_5551 = 1,
    SR_GE_TEX_4444 = 2,
    SR_GE_TEX_8888 = 3,
    SR_GE_TEX_INDEX4 = 4,  /* 4-bit CLUT index */
    SR_GE_TEX_INDEX8 = 5,  /* 8-bit CLUT index */
    SR_GE_TEX_INDEX16 = 6, /* 16-bit CLUT index */
    SR_GE_TEX_INDEX32 = 7  /* 32-bit CLUT index */
} SrGeTexFormat;

typedef enum SrGeChannelOrder {
    SR_GE_CHANNELS_R_LOW = 0, /* B1: red in the lowest field */
    SR_GE_CHANNELS_B_LOW = 1  /* B1: blue in the lowest field */
} SrGeChannelOrder;

typedef enum SrGeEndian {
    SR_GE_LITTLE_ENDIAN = 0, /* B2 */
    SR_GE_BIG_ENDIAN = 1
} SrGeEndian;

typedef enum SrGeClutOrder {
    SR_GE_CLUT_SHIFT_THEN_MASK = 0, /* B3 */
    SR_GE_CLUT_MASK_THEN_SHIFT = 1
} SrGeClutOrder;

typedef enum SrGeNibbleOrder {
    SR_GE_NIBBLE_LOW_FIRST = 0, /* B4: even texel in bits 3..0 */
    SR_GE_NIBBLE_HIGH_FIRST = 1
} SrGeNibbleOrder;

typedef enum SrGeAddressMode {
    SR_GE_ADDRESS_WRAP = 0, /* T4: coordinate & (dimension - 1) */
    SR_GE_ADDRESS_CLAMP = 1
} SrGeAddressMode;

typedef struct SrGeTexParams {
    SrGeChannelOrder channels;
    SrGeEndian endian;
    SrGeClutOrder clut_order;
    SrGeNibbleOrder nibbles;
    unsigned bilinear_bias;  /* B5: 128 or 0 */
    unsigned centre_offset;  /* B6: 0 or 8 sixteenths of a texel */
} SrGeTexParams;

int sr_ge_tex_params_valid(const SrGeTexParams* params);

/* T2 widening; the argument is masked to the field width. */
uint8_t sr_ge_expand5(uint32_t v);
uint8_t sr_ge_expand6(uint32_t v);
uint8_t sr_ge_expand4(uint32_t v);
uint8_t sr_ge_expand1(uint32_t v);

/* Assemble a 16-bit texel from two bytes in memory order (B2).  Invalid
   params or a NULL argument fail closed with *out = 0. */
uint32_t sr_ge_tex_load16(const SrGeTexParams* params, const uint8_t bytes[2], uint16_t* out);

/* Unpack a direct-colour texel.  16-bit formats use the low 16 bits of
   `texel`; 8888 uses all 32 (byte order already resolved by the caller). */
uint32_t sr_ge_tex_unpack(const SrGeTexParams* params, SrGeTexFormat format, uint32_t texel, SrGeRgba8* out);

/* T3/B3: CLUT index from a raw index texel.  shift must be 0..31. */
uint32_t sr_ge_clut_index(const SrGeTexParams* params, uint32_t raw, unsigned shift, uint32_t mask, uint32_t* index);

typedef struct SrGeClut {
    const uint32_t* entries; /* each entry holds one texel of `format` */
    size_t count;
    SrGeTexFormat format; /* 5650, 5551, 4444 or 8888 */
    unsigned shift;
    uint32_t mask;
} SrGeClut;

/* Index derivation plus lookup; an index >= count fails closed (B7). */
uint32_t sr_ge_clut_lookup(const SrGeTexParams* params, const SrGeClut* clut, uint32_t raw, SrGeRgba8* out);

/* T4: map a texel coordinate into [0, dim).  Wrap needs a power-of-two dim
   (B7); dim must be 1..4096.  Returns 0 and raises INVALID otherwise. */
uint32_t sr_ge_tex_address(SrGeAddressMode mode, int32_t coord, uint32_t dim, uint32_t* out);

/* T5: one bilinear blend of c[0] = c00, c[1] = c01 (u neighbour), c[2] = c10
   (v neighbour), c[3] = c11 with u, v in 0..15. */
uint32_t sr_ge_bilinear(const SrGeTexParams* params, const SrGeRgba8 c[4], unsigned u, unsigned v, SrGeRgba8* out);

typedef struct SrGeTexture {
    const uint8_t* data;
    size_t size;            /* bytes available at data */
    uint32_t width, height; /* texels, 1..4096 */
    uint32_t stride;        /* texels per row, width..8192 */
    SrGeTexFormat format;
    const SrGeClut* clut;   /* required for the INDEX formats */
    SrGeAddressMode address_u, address_v;
} SrGeTexture;

/* Fetch one texel at integer coordinates after addressing. */
uint32_t sr_ge_tex_fetch(const SrGeTexParams* params, const SrGeTexture* tex, int32_t x, int32_t y, SrGeRgba8* out);

/* Bilinear sample at (u, v) given in sixteenths of a texel (B6 applies). */
uint32_t sr_ge_tex_sample_bilinear(const SrGeTexParams* params, const SrGeTexture* tex, int32_t u16, int32_t v16,
                                   SrGeRgba8* out);

#endif /* NAKAGAWA_GE_TEXTURE_REF_H */
