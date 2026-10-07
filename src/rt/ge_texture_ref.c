// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge_texture_ref.c — project-authored reference model of GE texture sampling
// rules (issue #703).  See ge_texture_ref.h for the behavioural contract, the
// SPEC_ASSUMPTION / SPEC_AMBIGUITY labels and the oracle plan.

#include "ge_texture_ref.h"

int sr_ge_tex_params_valid(const SrGeTexParams* params) {
    if (!params) {
        return 0;
    }
    if (params->channels != SR_GE_CHANNELS_R_LOW && params->channels != SR_GE_CHANNELS_B_LOW) {
        return 0;
    }
    if (params->endian != SR_GE_LITTLE_ENDIAN && params->endian != SR_GE_BIG_ENDIAN) {
        return 0;
    }
    if (params->clut_order != SR_GE_CLUT_SHIFT_THEN_MASK && params->clut_order != SR_GE_CLUT_MASK_THEN_SHIFT) {
        return 0;
    }
    if (params->nibbles != SR_GE_NIBBLE_LOW_FIRST && params->nibbles != SR_GE_NIBBLE_HIGH_FIRST) {
        return 0;
    }
    if (params->bilinear_bias != 128u && params->bilinear_bias != 0u) {
        return 0;
    }
    if (params->centre_offset != 0u && params->centre_offset != 8u) {
        return 0;
    }
    return 1;
}

uint8_t sr_ge_expand5(uint32_t v) {
    return (uint8_t)((v & 31u) * 255u / 31u);
}

uint8_t sr_ge_expand6(uint32_t v) {
    return (uint8_t)((v & 63u) * 255u / 63u);
}

uint8_t sr_ge_expand4(uint32_t v) {
    return (uint8_t)((v & 15u) * 17u);
}

uint8_t sr_ge_expand1(uint32_t v) {
    return (v & 1u) ? 255u : 0u;
}

static uint16_t load16(const SrGeTexParams* params, const uint8_t* bytes) {
    if (params->endian == SR_GE_BIG_ENDIAN) {
        return (uint16_t)(((unsigned)bytes[0] << 8) | bytes[1]);
    }
    return (uint16_t)(((unsigned)bytes[1] << 8) | bytes[0]);
}

uint32_t sr_ge_tex_load16(const SrGeTexParams* params, const uint8_t bytes[2], uint16_t* out) {
    if (out) {
        *out = 0;
    }
    if (!sr_ge_tex_params_valid(params) || !bytes || !out) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    *out = load16(params, bytes);
    return 0;
}

static uint32_t load32(const SrGeTexParams* params, const uint8_t* b) {
    if (params->endian == SR_GE_BIG_ENDIAN) {
        return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    }
    return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
}

static int is_direct(SrGeTexFormat f) {
    return f == SR_GE_TEX_5650 || f == SR_GE_TEX_5551 || f == SR_GE_TEX_4444 || f == SR_GE_TEX_8888;
}

uint32_t sr_ge_tex_unpack(const SrGeTexParams* params, SrGeTexFormat format, uint32_t texel, SrGeRgba8* out) {
    uint8_t lo, mid, hi, a;
    if (out) {
        out->r = out->g = out->b = out->a = 0;
    }
    if (!sr_ge_tex_params_valid(params) || !out || !is_direct(format)) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    switch (format) {
    case SR_GE_TEX_5650:
        lo = sr_ge_expand5(texel);
        mid = sr_ge_expand6(texel >> 5);
        hi = sr_ge_expand5(texel >> 11);
        a = 255u;
        break;
    case SR_GE_TEX_5551:
        lo = sr_ge_expand5(texel);
        mid = sr_ge_expand5(texel >> 5);
        hi = sr_ge_expand5(texel >> 10);
        a = sr_ge_expand1(texel >> 15);
        break;
    case SR_GE_TEX_4444:
        lo = sr_ge_expand4(texel);
        mid = sr_ge_expand4(texel >> 4);
        hi = sr_ge_expand4(texel >> 8);
        a = sr_ge_expand4(texel >> 12);
        break;
    default:
        lo = (uint8_t)texel;
        mid = (uint8_t)(texel >> 8);
        hi = (uint8_t)(texel >> 16);
        a = (uint8_t)(texel >> 24);
        break;
    }
    out->g = mid;
    out->a = a;
    if (params->channels == SR_GE_CHANNELS_R_LOW) {
        out->r = lo;
        out->b = hi;
    } else {
        out->r = hi;
        out->b = lo;
    }
    return 0;
}

uint32_t sr_ge_clut_index(const SrGeTexParams* params, uint32_t raw, unsigned shift, uint32_t mask, uint32_t* index) {
    if (index) {
        *index = 0;
    }
    if (!sr_ge_tex_params_valid(params) || !index || shift > 31u) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    if (params->clut_order == SR_GE_CLUT_SHIFT_THEN_MASK) {
        *index = (raw >> shift) & mask;
    } else {
        *index = (raw & mask) >> shift;
    }
    return 0;
}

uint32_t sr_ge_clut_lookup(const SrGeTexParams* params, const SrGeClut* clut, uint32_t raw, SrGeRgba8* out) {
    uint32_t index;
    if (out) {
        out->r = out->g = out->b = out->a = 0;
    }
    if (!clut || !clut->entries || !out || !is_direct(clut->format)) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    if (sr_ge_clut_index(params, raw, clut->shift, clut->mask, &index)) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    if (index >= clut->count) {
        return SR_GE_TEX_FLAG_INVALID; /* B7: no guessed wrap or clamp */
    }
    return sr_ge_tex_unpack(params, clut->format, clut->entries[index], out);
}

uint32_t sr_ge_tex_address(SrGeAddressMode mode, int32_t coord, uint32_t dim, uint32_t* out) {
    if (out) {
        *out = 0;
    }
    if (!out || dim == 0 || dim > 4096u) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    if (mode == SR_GE_ADDRESS_WRAP) {
        if (dim & (dim - 1u)) {
            return SR_GE_TEX_FLAG_INVALID; /* B7 */
        }
        /* two's-complement mask: -1 wraps to dim - 1 */
        *out = (uint32_t)coord & (dim - 1u);
        return 0;
    }
    if (mode == SR_GE_ADDRESS_CLAMP) {
        *out = coord < 0 ? 0u : ((uint32_t)coord >= dim ? dim - 1u : (uint32_t)coord);
        return 0;
    }
    return SR_GE_TEX_FLAG_INVALID;
}

static uint8_t blend_channel(unsigned c00, unsigned c01, unsigned c10, unsigned c11, unsigned u, unsigned v,
                             unsigned bias) {
    /* exact: at most 255*256 + 128 (B8 models no narrower hardware width) */
    unsigned sum = (c00 * (16u - u) + c01 * u) * (16u - v) + (c10 * (16u - u) + c11 * u) * v + bias;
    return (uint8_t)(sum >> 8);
}

uint32_t sr_ge_bilinear(const SrGeTexParams* params, const SrGeRgba8 c[4], unsigned u, unsigned v, SrGeRgba8* out) {
    SrGeRgba8 r;
    if (!sr_ge_tex_params_valid(params) || !c || !out || u > 15u || v > 15u) {
        if (out) {
            out->r = out->g = out->b = out->a = 0;
        }
        return SR_GE_TEX_FLAG_INVALID;
    }
    /* computed into a local first so out may alias c */
    r.r = blend_channel(c[0].r, c[1].r, c[2].r, c[3].r, u, v, params->bilinear_bias);
    r.g = blend_channel(c[0].g, c[1].g, c[2].g, c[3].g, u, v, params->bilinear_bias);
    r.b = blend_channel(c[0].b, c[1].b, c[2].b, c[3].b, u, v, params->bilinear_bias);
    r.a = blend_channel(c[0].a, c[1].a, c[2].a, c[3].a, u, v, params->bilinear_bias);
    *out = r;
    return 0;
}

static unsigned texel_bits(SrGeTexFormat f) {
    switch (f) {
    case SR_GE_TEX_INDEX4:
        return 4;
    case SR_GE_TEX_INDEX8:
        return 8;
    case SR_GE_TEX_5650:
    case SR_GE_TEX_5551:
    case SR_GE_TEX_4444:
    case SR_GE_TEX_INDEX16:
        return 16;
    case SR_GE_TEX_8888:
    case SR_GE_TEX_INDEX32:
        return 32;
    default:
        return 0;
    }
}

uint32_t sr_ge_tex_fetch(const SrGeTexParams* params, const SrGeTexture* tex, int32_t x, int32_t y, SrGeRgba8* out) {
    uint32_t ax, ay, raw;
    size_t t, byte, need;
    unsigned bits;
    if (out) {
        out->r = out->g = out->b = out->a = 0;
    }
    if (!sr_ge_tex_params_valid(params) || !tex || !tex->data || !out || tex->width == 0 || tex->height == 0 ||
        tex->width > 4096u || tex->height > 4096u || tex->stride < tex->width || tex->stride > 8192u) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    bits = texel_bits(tex->format);
    if (bits == 0 || (!is_direct(tex->format) && !tex->clut)) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    if (sr_ge_tex_address(tex->address_u, x, tex->width, &ax) ||
        sr_ge_tex_address(tex->address_v, y, tex->height, &ay)) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    t = (size_t)ay * tex->stride + ax;
    byte = t * bits / 8u;
    need = bits >= 8u ? bits / 8u : 1u;
    if (byte + need > tex->size) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    switch (bits) {
    case 4: {
        unsigned b = tex->data[byte];
        int high = ((t & 1u) != 0) == (params->nibbles == SR_GE_NIBBLE_LOW_FIRST);
        raw = high ? (b >> 4) : (b & 15u);
        break;
    }
    case 8:
        raw = tex->data[byte];
        break;
    case 16:
        raw = load16(params, tex->data + byte);
        break;
    default:
        raw = load32(params, tex->data + byte);
        break;
    }
    if (is_direct(tex->format)) {
        return sr_ge_tex_unpack(params, tex->format, raw, out);
    }
    return sr_ge_clut_lookup(params, tex->clut, raw, out);
}

/* floor(s / 16) and s - 16 * floor(s / 16), independent of signed shifts */
static void split16(int64_t s, int32_t* whole, unsigned* frac) {
    int64_t w = s >= 0 ? s / 16 : -((-s + 15) / 16);
    *whole = (int32_t)w;
    *frac = (unsigned)(s - w * 16);
}

uint32_t sr_ge_tex_sample_bilinear(const SrGeTexParams* params, const SrGeTexture* tex, int32_t u16, int32_t v16,
                                   SrGeRgba8* out) {
    SrGeRgba8 c[4];
    int32_t x0, y0;
    unsigned fu, fv;
    uint32_t flags;
    if (out) {
        out->r = out->g = out->b = out->a = 0;
    }
    if (!sr_ge_tex_params_valid(params) || !out) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    split16((int64_t)u16 - params->centre_offset, &x0, &fu);
    split16((int64_t)v16 - params->centre_offset, &y0, &fv);
    flags = sr_ge_tex_fetch(params, tex, x0, y0, &c[0]);
    flags |= sr_ge_tex_fetch(params, tex, x0 + 1, y0, &c[1]);
    flags |= sr_ge_tex_fetch(params, tex, x0, y0 + 1, &c[2]);
    flags |= sr_ge_tex_fetch(params, tex, x0 + 1, y0 + 1, &c[3]);
    if (flags) {
        return SR_GE_TEX_FLAG_INVALID;
    }
    return sr_ge_bilinear(params, c, fu, fv, out);
}
