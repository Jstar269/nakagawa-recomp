// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge_texture_ref_selftest.c — conformance tests for the project-authored GE
// texture sampling reference model (src/rt/ge_texture_ref.c, issue #703).
//
// Expected values come from the #703 behavioural contract and first-principles
// arithmetic, never from another renderer: exhaustive channel widening with
// pinned values that separate the stated formulas from bit replication,
// hand-built texels for every format and channel/byte-order reading, CLUT
// shift/mask boundaries under both orders, wrap/clamp edges, and bilinear
// blends checked exhaustively against floor(weighted sum / 256 + 1/2) with
// vectors that separate the +128 bias from plain truncation.
// None of this is PSP-hardware evidence (SPEC_ASSUMPTION (#343 oracle pending)).

#include "ge_texture_ref.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            if (g_failures <= 40) {                                                                                    \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
                printf(__VA_ARGS__);                                                                                   \
                printf("\n");                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

static SrGeTexParams params(void) {
    SrGeTexParams p;
    p.channels = SR_GE_CHANNELS_R_LOW;
    p.endian = SR_GE_LITTLE_ENDIAN;
    p.clut_order = SR_GE_CLUT_SHIFT_THEN_MASK;
    p.nibbles = SR_GE_NIBBLE_LOW_FIRST;
    p.bilinear_bias = 128;
    p.centre_offset = 0;
    return p;
}

static int rgba_eq(SrGeRgba8 c, unsigned r, unsigned g, unsigned b, unsigned a) {
    return c.r == r && c.g == g && c.b == b && c.a == a;
}

static SrGeRgba8 rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    SrGeRgba8 c;
    c.r = (uint8_t)r;
    c.g = (uint8_t)g;
    c.b = (uint8_t)b;
    c.a = (uint8_t)a;
    return c;
}

static uint64_t g_rng = UINT64_C(0xA0761D6478BD642F);

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static void test_params(void) {
    SrGeTexParams p = params();
    CHECK(sr_ge_tex_params_valid(&p), "default params");
    p.bilinear_bias = 64;
    CHECK(!sr_ge_tex_params_valid(&p), "bias 64");
    p = params();
    p.centre_offset = 4;
    CHECK(!sr_ge_tex_params_valid(&p), "centre offset 4");
    CHECK(!sr_ge_tex_params_valid(NULL), "NULL");
}

static void test_expand(void) {
    uint32_t v;
    int bad = 0, differs5 = 0, differs6 = 0;
    for (v = 0; v < 32; ++v) {
        /* T2: v*255/31 with truncating division, recomputed in floating point */
        if (sr_ge_expand5(v) != (uint8_t)floor(v * 255.0 / 31.0)) {
            ++bad;
        }
        if (sr_ge_expand5(v) != ((v << 3) | (v >> 2))) {
            ++differs5;
        }
    }
    for (v = 0; v < 64; ++v) {
        if (sr_ge_expand6(v) != (uint8_t)floor(v * 255.0 / 63.0)) {
            ++bad;
        }
        if (sr_ge_expand6(v) != ((v << 2) | (v >> 4))) {
            ++differs6;
        }
    }
    for (v = 0; v < 16; ++v) {
        if (sr_ge_expand4(v) != v * 17u || sr_ge_expand4(v) != ((v << 4) | v)) {
            ++bad;
        }
    }
    CHECK(bad == 0, "%d widened values wrong", bad);
    CHECK(differs5 > 0 && differs6 > 0, "the stated formulas must differ from bit replication somewhere");
    CHECK(sr_ge_expand5(0) == 0 && sr_ge_expand5(1) == 8 && sr_ge_expand5(15) == 123 && sr_ge_expand5(16) == 131 &&
              sr_ge_expand5(31) == 255,
          "pinned 5-bit values (16 -> 131, not the replicated 132)");
    CHECK(sr_ge_expand6(32) == 129 && sr_ge_expand6(63) == 255 && sr_ge_expand6(1) == 4,
          "pinned 6-bit values (32 -> 129, not the replicated 130)");
    CHECK(sr_ge_expand1(0) == 0 && sr_ge_expand1(1) == 255, "1-bit alpha");
    CHECK(sr_ge_expand5(0x20u | 3u) == sr_ge_expand5(3), "argument masked to 5 bits");
}

static void test_unpack(void) {
    SrGeTexParams p = params(), b = params(), be = params();
    SrGeRgba8 c;
    uint8_t bytes[2] = {0x34, 0x12};
    b.channels = SR_GE_CHANNELS_B_LOW;
    be.endian = SR_GE_BIG_ENDIAN;

    /* extrema of every format */
    sr_ge_tex_unpack(&p, SR_GE_TEX_5650, 0xFFFFu, &c);
    CHECK(rgba_eq(c, 255, 255, 255, 255), "5650 white");
    sr_ge_tex_unpack(&p, SR_GE_TEX_5650, 0x0000u, &c);
    CHECK(rgba_eq(c, 0, 0, 0, 255), "5650 black is opaque");
    sr_ge_tex_unpack(&p, SR_GE_TEX_5551, 0x0000u, &c);
    CHECK(rgba_eq(c, 0, 0, 0, 0), "5551 transparent black");
    sr_ge_tex_unpack(&p, SR_GE_TEX_5551, 0x8000u, &c);
    CHECK(rgba_eq(c, 0, 0, 0, 255), "5551 alpha bit");
    sr_ge_tex_unpack(&p, SR_GE_TEX_5551, 0x7FFFu, &c);
    CHECK(rgba_eq(c, 255, 255, 255, 0), "5551 white, alpha clear");
    sr_ge_tex_unpack(&p, SR_GE_TEX_4444, 0xFFFFu, &c);
    CHECK(rgba_eq(c, 255, 255, 255, 255), "4444 white");

    /* field positions and midpoints under both channel orders */
    sr_ge_tex_unpack(&p, SR_GE_TEX_5650, 0x001Fu, &c);
    CHECK(rgba_eq(c, 255, 0, 0, 255), "5650 low field is red (R_LOW)");
    sr_ge_tex_unpack(&b, SR_GE_TEX_5650, 0x001Fu, &c);
    CHECK(rgba_eq(c, 0, 0, 255, 255), "5650 low field is blue (B_LOW)");
    sr_ge_tex_unpack(&p, SR_GE_TEX_5650, 0x07E0u, &c);
    CHECK(rgba_eq(c, 0, 255, 0, 255), "5650 middle field is 6-bit green");
    sr_ge_tex_unpack(&p, SR_GE_TEX_5650, 0x8410u, &c); /* r=16 g=32 b=16 */
    CHECK(rgba_eq(c, 131, 129, 131, 255), "5650 midpoint");
    sr_ge_tex_unpack(&p, SR_GE_TEX_5551, 0x4210u, &c); /* r=g=b=16 */
    CHECK(rgba_eq(c, 131, 131, 131, 0), "5551 midpoint");
    sr_ge_tex_unpack(&p, SR_GE_TEX_4444, 0x1234u, &c);
    CHECK(rgba_eq(c, 68, 51, 34, 17), "4444 0x1234 R_LOW");
    sr_ge_tex_unpack(&b, SR_GE_TEX_4444, 0x1234u, &c);
    CHECK(rgba_eq(c, 34, 51, 68, 17), "4444 0x1234 B_LOW");
    sr_ge_tex_unpack(&p, SR_GE_TEX_4444, 0x8888u, &c);
    CHECK(rgba_eq(c, 136, 136, 136, 136), "4444 midpoint");
    sr_ge_tex_unpack(&p, SR_GE_TEX_8888, 0x80402010u, &c);
    CHECK(rgba_eq(c, 0x10, 0x20, 0x40, 0x80), "8888 R_LOW");
    sr_ge_tex_unpack(&b, SR_GE_TEX_8888, 0x80402010u, &c);
    CHECK(rgba_eq(c, 0x40, 0x20, 0x10, 0x80), "8888 B_LOW");

    /* byte order */
    CHECK(sr_ge_tex_load16(&p, bytes) == 0x1234u, "little endian");
    CHECK(sr_ge_tex_load16(&be, bytes) == 0x3412u, "big endian");

    CHECK(sr_ge_tex_unpack(&p, SR_GE_TEX_INDEX8, 0, &c) == SR_GE_TEX_FLAG_INVALID, "index format is not direct");
    CHECK(sr_ge_tex_unpack(&p, SR_GE_TEX_5650, 0, NULL) == SR_GE_TEX_FLAG_INVALID, "NULL out");
}

static void test_clut(void) {
    SrGeTexParams sm = params(), ms = params();
    uint32_t idx, raw;
    int bad = 0;
    uint32_t entries[16];
    SrGeClut clut;
    SrGeRgba8 c;
    unsigned i;
    ms.clut_order = SR_GE_CLUT_MASK_THEN_SHIFT;

    /* B3: the two orders disagree on raw 0xAB, shift 4, mask 0x0F */
    sr_ge_clut_index(&sm, 0xABu, 4, 0x0Fu, &idx);
    CHECK(idx == 0x0Au, "shift-then-mask -> %#x", idx);
    sr_ge_clut_index(&ms, 0xABu, 4, 0x0Fu, &idx);
    CHECK(idx == 0x00u, "mask-then-shift -> %#x", idx);
    sr_ge_clut_index(&ms, 0xABu, 4, 0xF0u, &idx);
    CHECK(idx == 0x0Au, "mask-then-shift with a high mask -> %#x", idx);
    /* boundaries: identity, full shift, empty mask, every 8-bit raw */
    for (raw = 0; raw < 256; ++raw) {
        uint32_t a, bb;
        sr_ge_clut_index(&sm, raw, 0, 0xFFu, &a);
        sr_ge_clut_index(&ms, raw, 0, 0xFFu, &bb);
        if (a != raw || bb != raw) {
            ++bad;
        }
    }
    CHECK(bad == 0, "shift 0 / mask 0xFF must be the identity under both orders");
    sr_ge_clut_index(&sm, 0x80000000u, 31, 1u, &idx);
    CHECK(idx == 1u, "shift 31");
    sr_ge_clut_index(&sm, 0xFFFFFFFFu, 3, 0u, &idx);
    CHECK(idx == 0u, "empty mask");
    CHECK(sr_ge_clut_index(&sm, 1, 32, 1, &idx) == SR_GE_TEX_FLAG_INVALID, "shift 32 rejected");

    for (i = 0; i < 16; ++i) {
        entries[i] = (i << 12) | (i << 8) | (i << 4) | i; /* 4444 grey ramp */
    }
    clut.entries = entries;
    clut.count = 16;
    clut.format = SR_GE_TEX_4444;
    clut.shift = 0;
    clut.mask = 0xFFu;
    CHECK(sr_ge_clut_lookup(&sm, &clut, 7, &c) == 0 && rgba_eq(c, 119, 119, 119, 119), "entry 7");
    CHECK(sr_ge_clut_lookup(&sm, &clut, 16, &c) == SR_GE_TEX_FLAG_INVALID && rgba_eq(c, 0, 0, 0, 0),
          "index past the CLUT fails closed");
    clut.mask = 0x0Fu;
    CHECK(sr_ge_clut_lookup(&sm, &clut, 0x1Fu, &c) == 0 && c.r == 255, "mask keeps the index in range");
    clut.format = SR_GE_TEX_INDEX8;
    CHECK(sr_ge_clut_lookup(&sm, &clut, 1, &c) == SR_GE_TEX_FLAG_INVALID, "CLUT entries must be direct colour");
}

static void test_address(void) {
    uint32_t out, dim;
    int bad = 0;
    for (dim = 1; dim <= 4096; dim <<= 1) {
        uint32_t a, b2, c2, d;
        sr_ge_tex_address(SR_GE_ADDRESS_WRAP, -1, dim, &a);
        sr_ge_tex_address(SR_GE_ADDRESS_WRAP, (int32_t)dim, dim, &b2);
        sr_ge_tex_address(SR_GE_ADDRESS_CLAMP, -5, dim, &c2);
        sr_ge_tex_address(SR_GE_ADDRESS_CLAMP, (int32_t)dim + 3, dim, &d);
        if (a != dim - 1 || b2 != 0 || c2 != 0 || d != dim - 1) {
            ++bad;
        }
    }
    CHECK(bad == 0, "%d power-of-two wrap/clamp edges wrong", bad);
    sr_ge_tex_address(SR_GE_ADDRESS_WRAP, -17, 16, &out);
    CHECK(out == 15, "-17 wraps to 15 (mask, not C remainder), got %u", out);
    sr_ge_tex_address(SR_GE_ADDRESS_WRAP, INT32_MIN, 64, &out);
    CHECK(out == 0, "INT32_MIN wraps to 0");
    sr_ge_tex_address(SR_GE_ADDRESS_CLAMP, 7, 3, &out);
    CHECK(out == 2, "clamp works for non-power-of-two dimensions");
    CHECK(sr_ge_tex_address(SR_GE_ADDRESS_WRAP, 1, 3, &out) == SR_GE_TEX_FLAG_INVALID, "non-power-of-two wrap");
    CHECK(sr_ge_tex_address(SR_GE_ADDRESS_CLAMP, 1, 0, &out) == SR_GE_TEX_FLAG_INVALID, "zero dimension");
    CHECK(sr_ge_tex_address(SR_GE_ADDRESS_CLAMP, 1, 8192, &out) == SR_GE_TEX_FLAG_INVALID, "dimension too large");
}

static void test_bilinear(void) {
    SrGeTexParams p = params(), t = params();
    SrGeRgba8 c[4], out;
    unsigned u, v;
    int iter, bad = 0, bounds = 0, bias_differs = 0;
    t.bilinear_bias = 0;

    /* corners reproduce exactly at weight zero */
    c[0] = rgba(10, 20, 30, 40);
    c[1] = rgba(50, 60, 70, 80);
    c[2] = rgba(90, 100, 110, 120);
    c[3] = rgba(130, 140, 150, 160);
    sr_ge_bilinear(&p, c, 0, 0, &out);
    CHECK(rgba_eq(out, 10, 20, 30, 40), "u = v = 0 is c00");
    /* T5 vector X5: 0 and 255 at u = 8 -> (255*8*16 + 128) >> 8 = 128; 127 without the bias */
    c[0] = c[2] = rgba(0, 0, 0, 0);
    c[1] = c[3] = rgba(255, 255, 255, 255);
    sr_ge_bilinear(&p, c, 8, 0, &out);
    CHECK(out.r == 128, "midpoint with +128 -> %u", out.r);
    sr_ge_bilinear(&t, c, 8, 0, &out);
    CHECK(out.r == 127, "midpoint without the bias -> %u", out.r);
    /* u = 15 never reaches the neighbour: 255*15/16 = 239.06 -> 239 */
    sr_ge_bilinear(&p, c, 15, 0, &out);
    CHECK(out.r == 239, "u = 15 -> %u", out.r);
    CHECK(sr_ge_bilinear(&p, c, 16, 0, &out) == SR_GE_TEX_FLAG_INVALID, "weight 16 is not a 4-bit weight");

    for (iter = 0; iter < 4000; ++iter) {
        int k;
        for (k = 0; k < 4; ++k) {
            c[k] = rgba(rnd() & 255u, rnd() & 255u, rnd() & 255u, rnd() & 255u);
        }
        for (u = 0; u < 16; ++u) {
            for (v = 0; v < 16; ++v) {
                SrGeRgba8 o2;
                double w00 = (16.0 - u) * (16.0 - v), w01 = u * (16.0 - v), w10 = (16.0 - u) * v, w11 = (double)u * v;
                double exact = (c[0].g * w00 + c[1].g * w01 + c[2].g * w10 + c[3].g * w11) / 256.0;
                unsigned lo = c[0].g, hi = c[0].g;
                sr_ge_bilinear(&p, c, u, v, &out);
                sr_ge_bilinear(&t, c, u, v, &o2);
                if (out.g != (unsigned)floor(exact + 0.5) || o2.g != (unsigned)floor(exact)) {
                    ++bad;
                }
                for (k = 1; k < 4; ++k) {
                    lo = c[k].g < lo ? c[k].g : lo;
                    hi = c[k].g > hi ? c[k].g : hi;
                }
                if (out.g < lo || out.g > hi) {
                    ++bounds;
                }
                if (out.g != o2.g) {
                    bias_differs = 1;
                }
            }
        }
    }
    CHECK(bad == 0, "%d blends disagree with floor(sum/256 [+ 1/2])", bad);
    CHECK(bounds == 0, "%d blends left the corner range", bounds);
    CHECK(bias_differs, "the bias reading never mattered: no teeth");
}

static void test_textures(void) {
    SrGeTexParams p = params(), hi = params(), centre = params(), be = params();
    /* 4x4 4444 grey ramp along x: texel (x, y) = grey level 4x (y ignored) */
    uint8_t direct[4 * 4 * 2];
    uint8_t idx4[2] = {0x21u, 0x43u};
    uint8_t idx8[4] = {0, 1, 2, 3};
    uint32_t entries[16];
    SrGeClut clut;
    SrGeTexture tex;
    SrGeRgba8 c;
    int x, y;
    hi.nibbles = SR_GE_NIBBLE_HIGH_FIRST;
    centre.centre_offset = 8;
    be.endian = SR_GE_BIG_ENDIAN;
    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            unsigned g = (unsigned)(4 * x + 3); /* 4-bit level */
            uint16_t t16 = (uint16_t)((15u << 12) | (g << 8) | (g << 4) | g);
            direct[(y * 4 + x) * 2] = (uint8_t)(t16 & 0xFFu);
            direct[(y * 4 + x) * 2 + 1] = (uint8_t)(t16 >> 8);
        }
    }
    memset(&tex, 0, sizeof tex);
    tex.data = direct;
    tex.size = sizeof direct;
    tex.width = tex.height = tex.stride = 4;
    tex.format = SR_GE_TEX_4444;
    tex.address_u = tex.address_v = SR_GE_ADDRESS_WRAP;

    CHECK(sr_ge_tex_fetch(&p, &tex, 2, 1, &c) == 0 && rgba_eq(c, 187, 187, 187, 255), "fetch (2,1)");
    CHECK(sr_ge_tex_fetch(&p, &tex, -1, 0, &c) == 0 && c.r == 255, "wrap -1 -> texel 3");
    CHECK(sr_ge_tex_fetch(&be, &tex, 0, 0, &c) == 0 && c.a != 255, "big-endian reading reorders the bytes");
    /* point sample at texel corner (offset 0) and centre (offset 8) */
    CHECK(sr_ge_tex_sample_bilinear(&p, &tex, 2 * 16, 16, &c) == 0 && c.r == 187, "corner-aligned sample is exact");
    CHECK(sr_ge_tex_sample_bilinear(&centre, &tex, 2 * 16 + 8, 16 + 8, &c) == 0 && c.r == 187,
          "centre-aligned sample is exact");
    /* halfway between texel 0 (51) and 1 (119): (51*8 + 119*8)*16 + 128 >> 8 = 85 */
    sr_ge_tex_sample_bilinear(&p, &tex, 8, 0, &c);
    CHECK(c.r == 85, "halfway 0..1 -> %u", c.r);
    /* u = -8: texel -1 (wrap -> 3, level 15) and texel 0 (level 3) */
    sr_ge_tex_sample_bilinear(&p, &tex, -8, 0, &c);
    CHECK(c.r == 153, "wrap blend across the edge -> %u", c.r);
    tex.address_u = SR_GE_ADDRESS_CLAMP;
    sr_ge_tex_sample_bilinear(&p, &tex, -8, 0, &c);
    CHECK(c.r == 51, "clamp blend across the edge -> %u", c.r);
    sr_ge_tex_sample_bilinear(&p, &tex, 3 * 16 + 8, 0, &c);
    CHECK(c.r == 255, "clamp at the far edge -> %u", c.r);
    tex.size = 7;
    CHECK(sr_ge_tex_fetch(&p, &tex, 3, 3, &c) == SR_GE_TEX_FLAG_INVALID, "fetch past the buffer fails closed");
    tex.size = sizeof direct;
    tex.width = 3;
    tex.address_u = SR_GE_ADDRESS_WRAP;
    CHECK(sr_ge_tex_fetch(&p, &tex, 0, 0, &c) == SR_GE_TEX_FLAG_INVALID, "non-power-of-two wrap fails closed");

    /* indexed textures: CLUT entry i is 4444 grey level i */
    for (x = 0; x < 16; ++x) {
        unsigned g = (unsigned)x;
        entries[x] = (15u << 12) | (g << 8) | (g << 4) | g;
    }
    clut.entries = entries;
    clut.count = 16;
    clut.format = SR_GE_TEX_4444;
    clut.shift = 0;
    clut.mask = 0xFFu;
    memset(&tex, 0, sizeof tex);
    tex.data = idx4;
    tex.size = sizeof idx4;
    tex.width = 4;
    tex.height = 1;
    tex.stride = 4;
    tex.format = SR_GE_TEX_INDEX4;
    tex.clut = &clut;
    tex.address_u = tex.address_v = SR_GE_ADDRESS_CLAMP;
    /* B4 vector X3: byte 0x21 */
    sr_ge_tex_fetch(&p, &tex, 0, 0, &c);
    CHECK(c.r == 17, "low-nibble-first texel 0 is index 1 -> %u", c.r);
    sr_ge_tex_fetch(&hi, &tex, 0, 0, &c);
    CHECK(c.r == 34, "high-nibble-first texel 0 is index 2 -> %u", c.r);
    sr_ge_tex_fetch(&p, &tex, 3, 0, &c);
    CHECK(c.r == 68, "texel 3 is index 4 -> %u", c.r);
    tex.clut = NULL;
    CHECK(sr_ge_tex_fetch(&p, &tex, 0, 0, &c) == SR_GE_TEX_FLAG_INVALID, "indexed texture needs a CLUT");
    tex.clut = &clut;
    tex.data = idx8;
    tex.size = sizeof idx8;
    tex.format = SR_GE_TEX_INDEX8;
    sr_ge_tex_fetch(&p, &tex, 2, 0, &c);
    CHECK(c.r == 34, "8-bit index 2 -> %u", c.r);
    idx8[2] = 200;
    CHECK(sr_ge_tex_fetch(&p, &tex, 2, 0, &c) == SR_GE_TEX_FLAG_INVALID, "index past the CLUT fails closed");
    CHECK(sr_ge_tex_sample_bilinear(&p, &tex, 2 * 16, 0, &c) == SR_GE_TEX_FLAG_INVALID,
          "a failed fetch fails the whole sample");
}

int main(void) {
    test_params();
    test_expand();
    test_unpack();
    test_clut();
    test_address();
    test_bilinear();
    test_textures();
    if (g_failures) {
        printf("GE_TEXTURE_REF_SELFTEST FAIL failures=%d checks=%d\n", g_failures, g_checks);
        return 1;
    }
    printf("GE_TEXTURE_REF_SELFTEST PASS checks=%d\n", g_checks);
    return 0;
}
