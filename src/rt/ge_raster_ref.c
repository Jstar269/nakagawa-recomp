// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge_raster_ref.c — project-authored reference model of GE triangle setup and
// rasterization rules (issue #696).  See ge_raster_ref.h for the behavioural
// contract, the SPEC_ASSUMPTION / SPEC_AMBIGUITY labels and the oracle plan.
//
// All geometry is exact integer arithmetic on 12.4 coordinates: edge functions
// and doubled areas fit in int64 for every coordinate in 0..0xFFFF, so the only
// rounding in this module is the one the setup reciprocal and the stated
// readings introduce.

#include "ge_raster_ref.h"

#include <math.h>

int sr_ge_raster_params_valid(const SrGeRasterParams* params) {
    if (!params) {
        return 0;
    }
    if (params->snap != SR_GE_SNAP_TRUNCATE && params->snap != SR_GE_SNAP_FLOOR && params->snap != SR_GE_SNAP_NEAREST) {
        return 0;
    }
    if (params->sample_offset >= SR_GE_SUBPIXELS) {
        return 0;
    }
    if (params->anchor_tie != SR_GE_ANCHOR_TIE_TOPMOST && params->anchor_tie != SR_GE_ANCHOR_TIE_FIRST) {
        return 0;
    }
    if (params->start16 != SR_GE_START16_TABLE_BASES && params->start16 != SR_GE_START16_RECIP_INPUT &&
        params->start16 != SR_GE_START16_PLANE_START) {
        return 0;
    }
    if (params->grad_round != SR_GE_GRAD_TOWARD_ZERO && params->grad_round != SR_GE_GRAD_FLOOR) {
        return 0;
    }
    if (params->grad_frac_bits > SR_GE_GRAD_FRAC_MAX) {
        return 0;
    }
    return 1;
}

uint32_t sr_ge_snap_12_4(const SrGeRasterParams* params, double pixels, int32_t* out) {
    double scaled, snapped;
    uint32_t flags = 0;
    if (out) {
        *out = 0;
    }
    if (!sr_ge_raster_params_valid(params) || !out || !isfinite(pixels)) {
        return SR_GE_RASTER_FLAG_INVALID;
    }
    scaled = pixels * SR_GE_SUBPIXELS;
    switch (params->snap) {
    case SR_GE_SNAP_TRUNCATE:
        snapped = trunc(scaled);
        break;
    case SR_GE_SNAP_FLOOR:
        snapped = floor(scaled);
        break;
    default:
        snapped = floor(scaled + 0.5);
        break;
    }
    if (snapped < 0.0 || snapped > (double)SR_GE_COORD_MAX) {
        return SR_GE_RASTER_FLAG_RANGE;
    }
    if (snapped != scaled) {
        flags |= SR_GE_RASTER_FLAG_INEXACT;
    }
    *out = (int32_t)snapped;
    return flags;
}

int64_t sr_ge_area2(const SrGeVertex* a, const SrGeVertex* b, const SrGeVertex* c) {
    return (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(b->y - a->y) * (c->x - a->x);
}

static int coord_ok(int32_t v) {
    return v >= 0 && (uint32_t)v <= SR_GE_COORD_MAX;
}

static int tri_ok(const SrGeVertex tri[3]) {
    int i;
    for (i = 0; i < 3; ++i) {
        if (!coord_ok(tri[i].x) || !coord_ok(tri[i].y)) {
            return 0;
        }
    }
    return 1;
}

/* Order the vertices so the doubled area is positive (clockwise on screen). */
static void normalise(const SrGeVertex tri[3], SrGeVertex out[3]) {
    out[0] = tri[0];
    if (sr_ge_area2(&tri[0], &tri[1], &tri[2]) < 0) {
        out[1] = tri[2];
        out[2] = tri[1];
    } else {
        out[1] = tri[1];
        out[2] = tri[2];
    }
}

/* R2: an edge i->j of a positive-area triangle is a top edge when it is
   horizontal and runs in +x (interior below), and a left edge when it runs in
   -y (interior to its right). */
static int is_top_left(int32_t dx, int32_t dy) {
    return dy < 0 || (dy == 0 && dx > 0);
}

static int sample_inside(const SrGeVertex t[3], int64_t sx, int64_t sy) {
    int i;
    for (i = 0; i < 3; ++i) {
        const SrGeVertex* p = &t[i];
        const SrGeVertex* q = &t[(i + 1) % 3];
        int32_t dx = q->x - p->x;
        int32_t dy = q->y - p->y;
        int64_t e = (int64_t)dx * (sy - p->y) - (int64_t)dy * (sx - p->x);
        if (e < 0 || (e == 0 && !is_top_left(dx, dy))) {
            return 0;
        }
    }
    return 1;
}

int sr_ge_raster_covers(const SrGeRasterParams* params, const SrGeVertex tri[3], int32_t px, int32_t py) {
    SrGeVertex t[3];
    if (!sr_ge_raster_params_valid(params) || !tri || !tri_ok(tri)) {
        return 0;
    }
    if (sr_ge_area2(&tri[0], &tri[1], &tri[2]) == 0) {
        return 0;
    }
    normalise(tri, t);
    return sample_inside(t, (int64_t)px * SR_GE_SUBPIXELS + params->sample_offset,
                         (int64_t)py * SR_GE_SUBPIXELS + params->sample_offset);
}

uint32_t sr_ge_raster_coverage(const SrGeRasterParams* params, const SrGeVertex tri[3], uint8_t* mask, int32_t width,
                               int32_t height) {
    SrGeVertex t[3];
    int32_t px, py;
    if (!sr_ge_raster_params_valid(params) || !tri || !mask || width <= 0 || height <= 0 || width > 4096 ||
        height > 4096) {
        return SR_GE_RASTER_FLAG_INVALID;
    }
    for (py = 0; py < height; ++py) {
        for (px = 0; px < width; ++px) {
            mask[(size_t)py * (size_t)width + (size_t)px] = 0;
        }
    }
    if (!tri_ok(tri)) {
        return SR_GE_RASTER_FLAG_RANGE;
    }
    if (sr_ge_area2(&tri[0], &tri[1], &tri[2]) == 0) {
        return SR_GE_RASTER_FLAG_DEGENERATE;
    }
    normalise(tri, t);
    for (py = 0; py < height; ++py) {
        for (px = 0; px < width; ++px) {
            if (sample_inside(t, (int64_t)px * SR_GE_SUBPIXELS + params->sample_offset,
                              (int64_t)py * SR_GE_SUBPIXELS + params->sample_offset)) {
                mask[(size_t)py * (size_t)width + (size_t)px] = 1;
            }
        }
    }
    return 0;
}

/* ---- setup reciprocal --------------------------------------------------- */

static int msb_index(uint64_t v) {
    int p = -1;
    while (v) {
        v >>= 1;
        ++p;
    }
    return p;
}

uint32_t sr_ge_setup_table_check(const SrGeRasterParams* params, const SrGeSetupRecipTable* table) {
    uint64_t lo, hi, tmax;
    int i;
    if (!sr_ge_raster_params_valid(params) || !table || table->out_frac_bits < SR_GE_SETUP_FRAC_MIN ||
        table->out_frac_bits > SR_GE_SETUP_FRAC_MAX || table->t_bits > SR_GE_SETUP_T_BITS_MAX ||
        table->slope_shift > 31) {
        return SR_GE_RASTER_FLAG_INVALID;
    }
    lo = UINT64_C(1) << (table->out_frac_bits - 1);
    hi = UINT64_C(1) << table->out_frac_bits;
    tmax = (UINT64_C(1) << table->t_bits) - 1u;
    for (i = 0; i < SR_GE_SETUP_SEGMENTS; ++i) {
        uint64_t y0 = table->base[i];
        uint64_t drop = ((uint64_t)table->slope[i] * tmax) >> table->slope_shift;
        /* y decreases with t, so the two endpoints bound the segment. */
        if (y0 > hi || drop > y0 || y0 - drop < lo) {
            return SR_GE_RASTER_FLAG_INVALID;
        }
        if (params->start16 == SR_GE_START16_TABLE_BASES && (table->base[i] & 15u) != 0) {
            return SR_GE_RASTER_FLAG_INVALID;
        }
    }
    return 0;
}

unsigned sr_ge_setup_segment(const SrGeSetupRecipTable* table, uint64_t area2, unsigned* t_out) {
    int k;
    unsigned total;
    uint64_t f, ext;
    unsigned t_bits = (table && table->t_bits <= SR_GE_SETUP_T_BITS_MAX) ? table->t_bits : 0u;
    if (t_out) {
        *t_out = 0;
    }
    if (area2 == 0) {
        return 0;
    }
    k = msb_index(area2);
    f = area2 - (UINT64_C(1) << k);
    total = SR_GE_SETUP_INDEX_BITS + t_bits;
    ext = (unsigned)k >= total ? f >> ((unsigned)k - total) : f << (total - (unsigned)k);
    if (t_out) {
        *t_out = (unsigned)(ext & ((UINT64_C(1) << t_bits) - 1u));
    }
    return (unsigned)(ext >> t_bits);
}

/* ---- planes ------------------------------------------------------------- */

int sr_ge_plane_anchor_leftmost(const SrGeRasterParams* params, const SrGeVertex tri[3]) {
    int best = 0, i;
    if (!sr_ge_raster_params_valid(params) || !tri) {
        return -1;
    }
    for (i = 1; i < 3; ++i) {
        if (tri[i].x < tri[best].x) {
            best = i;
        } else if (tri[i].x == tri[best].x && params->anchor_tie == SR_GE_ANCHOR_TIE_TOPMOST &&
                   tri[i].y < tri[best].y) {
            best = i;
        }
    }
    return best;
}

static int64_t floor_to_16(int64_t v) {
    int64_t m = v % 16;
    if (m < 0) {
        m += 16;
    }
    return v - m;
}

uint32_t sr_ge_plane_setup_anchored(const SrGeRasterParams* params, const SrGeSetupRecipTable* table,
                                    const SrGeVertex tri[3], const int32_t attr[3], int anchor, SrGePlane* out) {
    const SrGeVertex *a, *b, *c;
    int32_t va, vb, vc;
    int64_t area2, num[2];
    uint64_t area_in;
    unsigned seg, t, g;
    uint64_t y;
    int k, i;
    uint32_t flags = 0;
    SrGePlane p;

    if (!sr_ge_raster_params_valid(params) || sr_ge_setup_table_check(params, table) || !tri || !attr || !out ||
        anchor < 0 || anchor > 2) {
        return SR_GE_RASTER_FLAG_INVALID;
    }
    if (params->grad_frac_bits > table->out_frac_bits) {
        return SR_GE_RASTER_FLAG_INVALID;
    }
    if (!tri_ok(tri)) {
        return SR_GE_RASTER_FLAG_RANGE;
    }
    for (i = 0; i < 3; ++i) {
        if (attr[i] < SR_GE_ATTR_MIN || attr[i] > SR_GE_ATTR_MAX) {
            return SR_GE_RASTER_FLAG_INVALID;
        }
    }
    a = &tri[anchor];
    b = &tri[(anchor + 1) % 3];
    c = &tri[(anchor + 2) % 3];
    va = attr[anchor];
    vb = attr[(anchor + 1) % 3];
    vc = attr[(anchor + 2) % 3];
    area2 = sr_ge_area2(a, b, c);
    if (area2 == 0) {
        return SR_GE_RASTER_FLAG_DEGENERATE;
    }
    area_in = (uint64_t)(area2 < 0 ? -area2 : area2);
    if (params->start16 == SR_GE_START16_RECIP_INPUT) {
        uint64_t rounded = area_in & ~UINT64_C(15);
        if (rounded != area_in) {
            flags |= SR_GE_RASTER_FLAG_INEXACT;
        }
        if (rounded == 0) {
            return flags | SR_GE_RASTER_FLAG_DEGENERATE;
        }
        area_in = rounded;
    }

    /* Cramer's rule relative to the anchor: |num| < 2^42 for 24-bit attributes
       and 16-bit coordinates. */
    num[0] = (int64_t)(vb - va) * (c->y - a->y) - (int64_t)(vc - va) * (b->y - a->y);
    num[1] = (int64_t)(b->x - a->x) * (vc - va) - (int64_t)(c->x - a->x) * (vb - va);

    seg = sr_ge_setup_segment(table, area_in, &t);
    y = table->base[seg] - (((uint64_t)table->slope[seg] * t) >> table->slope_shift);
    k = msb_index(area_in);
    g = params->grad_frac_bits;

    for (i = 0; i < 2; ++i) {
        /* gradient = num / area ~= num * y * 2^-(F + k); in fixed point with g
           fraction bits that is (num * y) >> (F + k - g), and g <= F keeps the
           shift non-negative.  |num| * y < 2^62. */
        unsigned neg = (num[i] < 0) != (area2 < 0);
        uint64_t mag = (uint64_t)(num[i] < 0 ? -num[i] : num[i]) * y;
        unsigned shift = table->out_frac_bits + (unsigned)k - g;
        uint64_t q = mag >> shift;
        int lost = (q << shift) != mag;
        if (lost) {
            flags |= SR_GE_RASTER_FLAG_INEXACT;
            if (neg && params->grad_round == SR_GE_GRAD_FLOOR) {
                ++q;
            }
        }
        if (i == 0) {
            p.dx = neg ? -(int64_t)q : (int64_t)q;
        } else {
            p.dy = neg ? -(int64_t)q : (int64_t)q;
        }
    }
    p.start = (int64_t)va * ((int64_t)1 << g);
    if (params->start16 == SR_GE_START16_PLANE_START) {
        int64_t r = floor_to_16(p.start);
        if (r != p.start) {
            flags |= SR_GE_RASTER_FLAG_INEXACT;
        }
        p.start = r;
    }
    p.ax = a->x;
    p.ay = a->y;
    p.frac_bits = g;
    *out = p;
    return flags;
}

uint32_t sr_ge_plane_setup(const SrGeRasterParams* params, const SrGeSetupRecipTable* table, const SrGeVertex tri[3],
                           const int32_t attr[3], SrGePlane* out) {
    int anchor = sr_ge_plane_anchor_leftmost(params, tri);
    if (anchor < 0) {
        return SR_GE_RASTER_FLAG_INVALID;
    }
    return sr_ge_plane_setup_anchored(params, table, tri, attr, anchor, out);
}

static uint64_t magnitude(int64_t v) {
    return v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
}

/* a * b when the exact product fits in int64 (INT64_MIN included). */
static int mul_ok(int64_t a, int64_t b, int64_t* out) {
    if (a != 0 && b != 0) {
        uint64_t limit = ((a < 0) != (b < 0)) ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
        if (magnitude(a) > limit / magnitude(b)) {
            return 0;
        }
    }
    *out = a * b;
    return 1;
}

static int add_ok(int64_t a, int64_t b, int64_t* out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return 0;
    }
    *out = a + b;
    return 1;
}

uint32_t sr_ge_plane_eval(const SrGePlane* plane, int32_t x, int32_t y, int64_t* out) {
    int64_t tx, ty, v;
    if (!plane || !out) {
        return SR_GE_RASTER_FLAG_INVALID;
    }
    *out = 0;
    if (!mul_ok(plane->dx, (int64_t)x - plane->ax, &tx) || !mul_ok(plane->dy, (int64_t)y - plane->ay, &ty) ||
        !add_ok(plane->start, tx, &v) || !add_ok(v, ty, &v)) {
        return SR_GE_RASTER_FLAG_OVERFLOW;
    }
    *out = v;
    return 0;
}

/* ---- colour factors ----------------------------------------------------- */

uint32_t sr_ge_color_mul(uint32_t x, uint32_t s) {
    if (x > 255u || s > 255u) {
        return UINT32_MAX;
    }
    return ((2u * x + 1u) * (2u * s + 1u)) >> 10;
}

int32_t sr_ge_inverse_factor(uint32_t alpha, uint32_t one) {
    if (alpha > 255u || (one != 255u && one != 256u)) {
        return INT32_MIN;
    }
    /* R7: deliberately not clamped at zero. */
    return (int32_t)one - 2 * (int32_t)alpha;
}

int32_t sr_ge_color_mul_signed(uint32_t x, int32_t s) {
    int32_t p;
    if (x > 255u || s < -512 || s > 512) {
        return INT32_MIN;
    }
    p = (2 * (int32_t)x + 1) * (2 * s + 1);
    /* floor division by 1024, independent of the host's signed shift */
    return p >= 0 ? p / 1024 : -((-p + 1023) / 1024);
}
