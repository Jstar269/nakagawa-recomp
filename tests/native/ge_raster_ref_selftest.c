// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge_raster_ref_selftest.c — conformance tests for the project-authored GE
// triangle setup and rasterization reference model (src/rt/ge_raster_ref.c,
// issue #696).
//
// Expected values come from the #696 behavioural contract and first-principles
// geometry, never from another renderer:
//   * hand-counted coverage of small triangles under both sample positions;
//   * a tiling oracle that needs no edge functions: an axis-aligned rectangle
//     split into triangles must cover exactly the samples in
//     [x0, x1) x [y0, y1) once each (top and left boundaries in, bottom and
//     right out), so any crack or double coverage on a shared edge fails;
//   * a second coverage model written as a symbolic perturbation of the sample
//     toward (+epsilon, +epsilon^2), which is how the top-left rule is derived;
//   * exact-arithmetic plane checks on power-of-two areas, and error bounds on
//     arbitrary areas against a SYNTHETIC (non-PSP) setup reciprocal table;
//   * exhaustive colour-factor checks against floor((x + 1/2)(s + 1/2) / 256).
// None of this is PSP-hardware evidence (SPEC_ASSUMPTION (#343 oracle pending)).

#include "ge_raster_ref.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

static SrGeRasterParams params(unsigned offset) {
    SrGeRasterParams p;
    p.snap = SR_GE_SNAP_TRUNCATE;
    p.sample_offset = offset;
    p.anchor_tie = SR_GE_ANCHOR_TIE_TOPMOST;
    p.start16 = SR_GE_START16_TABLE_BASES;
    p.grad_round = SR_GE_GRAD_TOWARD_ZERO;
    p.grad_frac_bits = 8;
    return p;
}

static SrGeVertex vtx(int32_t x, int32_t y) {
    SrGeVertex v;
    v.x = x;
    v.y = y;
    return v;
}

static uint64_t g_rng = UINT64_C(0xD1B54A32D192ED03);

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static int32_t rnd_range(int32_t lo, int32_t hi) {
    return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1));
}

/* A coordinate in [lo, hi], snapped to the sample grid half the time so ties
   with sample positions are frequent. */
static int32_t rnd_coord(int32_t lo, int32_t hi, unsigned offset) {
    int32_t v = rnd_range(lo, hi);
    if (rnd() & 1u) {
        int32_t g = (v / 16) * 16 + (int32_t)offset;
        if (g < lo) {
            g += 16;
        }
        if (g > hi) {
            g -= 16;
        }
        if (g >= lo && g <= hi) {
            v = g;
        }
    }
    return v;
}

static int count_mask(const uint8_t* m, int n) {
    int i, c = 0;
    for (i = 0; i < n; ++i) {
        c += m[i];
    }
    return c;
}

/* ---- tests -------------------------------------------------------------- */

static void test_params(void) {
    SrGeRasterParams p = params(0);
    CHECK(sr_ge_raster_params_valid(&p), "default params");
    p.sample_offset = 16;
    CHECK(!sr_ge_raster_params_valid(&p), "offset 16");
    p = params(0);
    p.start16 = (SrGeStart16)3;
    CHECK(!sr_ge_raster_params_valid(&p), "unknown start16");
    p = params(0);
    p.grad_frac_bits = SR_GE_GRAD_FRAC_MAX + 1;
    CHECK(!sr_ge_raster_params_valid(&p), "grad bits too large");
    CHECK(!sr_ge_raster_params_valid(NULL), "NULL params");
}

static void test_snap(void) {
    SrGeRasterParams t = params(0), f = params(0), n = params(0);
    int32_t v;
    uint32_t fl;
    f.snap = SR_GE_SNAP_FLOOR;
    n.snap = SR_GE_SNAP_NEAREST;

    fl = sr_ge_snap_12_4(&t, 1.0 / 16.0, &v);
    CHECK(v == 1 && fl == 0, "one subpixel is exact");
    fl = sr_ge_snap_12_4(&t, 1.0 / 32.0, &v);
    CHECK(v == 0 && fl == SR_GE_RASTER_FLAG_INEXACT, "half subpixel truncates to 0");
    sr_ge_snap_12_4(&f, 1.0 / 32.0, &v);
    CHECK(v == 0, "half subpixel floors to 0");
    sr_ge_snap_12_4(&n, 1.0 / 32.0, &v);
    CHECK(v == 1, "half subpixel rounds to 1");
    sr_ge_snap_12_4(&n, 0.03, &v);
    CHECK(v == 0, "0.48 subpixel rounds to 0");
    /* the largest double below one half subpixel: floor(x + 0.5) computed in
       floating point would round this up to 1 */
    sr_ge_snap_12_4(&n, nextafter(0.5, 0.0) / 16.0, &v);
    CHECK(v == 0, "just below half a subpixel rounds to 0, got %d", v);
    sr_ge_snap_12_4(&n, (10.0 * 16.0 + nextafter(0.5, 1.0)) / 16.0, &v);
    CHECK(v == 161, "just above half rounds up, got %d", v);
    sr_ge_snap_12_4(&t, 10.99, &v);
    CHECK(v == 175, "10.99 px truncates to 175 subpixels, got %d", v);
    sr_ge_snap_12_4(&n, 10.99, &v);
    CHECK(v == 176, "10.99 px rounds to 176 subpixels, got %d", v);
    fl = sr_ge_snap_12_4(&t, 4095.9375, &v);
    CHECK(v == 0xFFFF && fl == 0, "top of the 12.4 range");
    fl = sr_ge_snap_12_4(&t, 4096.0, &v);
    CHECK(v == 0 && fl == SR_GE_RASTER_FLAG_RANGE, "4096 px is out of range");
    fl = sr_ge_snap_12_4(&n, 4095.97, &v);
    CHECK(fl == SR_GE_RASTER_FLAG_RANGE, "rounding up past the range");
    /* the readings differ just below zero */
    fl = sr_ge_snap_12_4(&t, -0.01, &v);
    CHECK(v == 0 && fl == SR_GE_RASTER_FLAG_INEXACT, "-0.01 px truncates into range");
    fl = sr_ge_snap_12_4(&f, -0.01, &v);
    CHECK(fl == SR_GE_RASTER_FLAG_RANGE, "-0.01 px floors out of range");
    fl = sr_ge_snap_12_4(&n, -0.01, &v);
    CHECK(v == 0 && fl == SR_GE_RASTER_FLAG_INEXACT, "-0.01 px rounds to 0");
    CHECK(sr_ge_snap_12_4(&t, NAN, &v) == SR_GE_RASTER_FLAG_INVALID, "NaN");
    CHECK(sr_ge_snap_12_4(&t, 1.0, NULL) == SR_GE_RASTER_FLAG_INVALID, "NULL out");
}

static void test_hand_coverage(void) {
    SrGeRasterParams corner = params(0), centre = params(8);
    uint8_t m[8 * 8];
    /* 4-pixel right triangle with its hypotenuse on the bottom-right */
    SrGeVertex a[3];
    SrGeVertex b[3];
    a[0] = vtx(0, 0);
    a[1] = vtx(64, 0);
    a[2] = vtx(0, 64);
    /* mirrored: hypotenuse on the top-left */
    b[0] = vtx(64, 0);
    b[1] = vtx(64, 64);
    b[2] = vtx(0, 64);

    /* corner samples (i, j): top and left edges own their samples, the
       hypotenuse (a right edge here) does not: i + j < 4 -> 10 pixels */
    CHECK(sr_ge_raster_coverage(&corner, a, m, 8, 8) == 0 && count_mask(m, 64) == 10, "corner count %d",
          count_mask(m, 64));
    CHECK(m[0] == 1, "sample on the top-left vertex is covered");
    CHECK(m[4] == 0 && m[4 * 8] == 0, "samples on the far vertices are not covered");
    CHECK(m[3] == 1 && m[3 * 8] == 1, "samples on the top and left edges are covered");
    CHECK(m[1 * 8 + 3] == 0, "sample on the hypotenuse (a right edge) is not covered");
    /* centre samples (i + 1/2, j + 1/2): i + j + 1 < 4 -> 6 pixels */
    sr_ge_raster_coverage(&centre, a, m, 8, 8);
    CHECK(count_mask(m, 64) == 6, "centre count %d", count_mask(m, 64));
    /* mirrored triangle: the hypotenuse is a left edge and owns its samples,
       the right and bottom edges do not: corner i, j < 4, i + j >= 4 -> 6 */
    sr_ge_raster_coverage(&corner, b, m, 8, 8);
    CHECK(count_mask(m, 64) == 6, "mirrored corner count %d", count_mask(m, 64));
    CHECK(m[1 * 8 + 3] == 1, "sample on a left-edge hypotenuse is covered");
    CHECK(m[3 * 8 + 4] == 0 && m[4 * 8 + 3] == 0, "right and bottom edges do not own samples");
    /* centre: i, j <= 3, i + j + 1 >= 4 -> 10 */
    sr_ge_raster_coverage(&centre, b, m, 8, 8);
    CHECK(count_mask(m, 64) == 10, "mirrored centre count %d", count_mask(m, 64));
    /* the per-pixel query agrees with the window */
    CHECK(sr_ge_raster_covers(&centre, b, 3, 3) == 1 && sr_ge_raster_covers(&centre, b, 0, 0) == 0, "covers()");
    /* pixel (2, 1) of the first triangle: corner sample (32, 16) is inside,
       centre sample (40, 24) lies beyond the hypotenuse */
    CHECK(sr_ge_raster_covers(&corner, a, 2, 1) == 1 && sr_ge_raster_covers(&centre, a, 2, 1) == 0,
          "covers() honours the sample offset");
}

/* Second model: the top-left rule is the strict-inside test of the sample
   perturbed to (sx + e, sy + e^2) for an infinitesimal e > 0. */
static int model_covers(const SrGeVertex t[3], int64_t sx, int64_t sy) {
    int64_t area = (int64_t)(t[1].x - t[0].x) * (t[2].y - t[0].y) - (int64_t)(t[1].y - t[0].y) * (t[2].x - t[0].x);
    int i;
    if (area == 0) {
        return 0;
    }
    for (i = 0; i < 3; ++i) {
        const SrGeVertex* p = &t[i];
        const SrGeVertex* q = &t[(i + 1) % 3];
        int64_t ex = q->x - p->x, ey = q->y - p->y;
        /* cross((q - p), (s + d - p)) as a polynomial e^0 + c1 e + c2 e^2 */
        int64_t c0 = ex * (sy - p->y) - ey * (sx - p->x);
        int64_t c1 = -ey;
        int64_t c2 = ex;
        if (area < 0) {
            c0 = -c0;
            c1 = -c1;
            c2 = -c2;
        }
        if (c0 < 0 || (c0 == 0 && (c1 < 0 || (c1 == 0 && c2 <= 0)))) {
            return 0;
        }
    }
    return 1;
}

static void test_model_agreement(void) {
    int iter, bad = 0;
    unsigned off;
    for (off = 0; off < 16; off += 8) {
        SrGeRasterParams p = params(off);
        for (iter = 0; iter < 3000; ++iter) {
            SrGeVertex t[3];
            uint8_t m[24 * 24];
            int i, px, py;
            for (i = 0; i < 3; ++i) {
                t[i] = vtx(rnd_coord(0, 24 * 16, off), rnd_coord(0, 24 * 16, off));
            }
            if (iter % 7 == 0) {
                /* slivers: the third vertex sits within a subpixel of the line */
                int32_t sy = (t[0].y + t[1].y) / 2 + (int32_t)(rnd() % 3u) - 1;
                t[2] = vtx((t[0].x + t[1].x) / 2, sy < 0 ? 0 : sy);
            }
            sr_ge_raster_coverage(&p, t, m, 24, 24);
            for (py = 0; py < 24; ++py) {
                for (px = 0; px < 24; ++px) {
                    int want = model_covers(t, px * 16 + (int)off, py * 16 + (int)off);
                    if (m[py * 24 + px] != want || sr_ge_raster_covers(&p, t, px, py) != want) {
                        ++bad;
                    }
                }
            }
        }
    }
    CHECK(bad == 0, "%d samples disagree with the perturbation model", bad);
}

/* Tiling oracle.  `tris` triangles must cover each sample in the rectangle
   exactly once and nothing outside it. */
static int tiling_errors(const SrGeRasterParams* p, SrGeVertex tris[][3], int ntris, int32_t x0, int32_t y0, int32_t x1,
                         int32_t y1, int* doubles, int* cracks) {
    enum { W = 40, H = 40 };
    static uint8_t m[W * H];
    int count[W * H];
    int i, px, py, errors = 0;
    memset(count, 0, sizeof count);
    for (i = 0; i < ntris; ++i) {
        int k;
        /* random winding per triangle */
        if (rnd() & 1u) {
            SrGeVertex s = tris[i][1];
            tris[i][1] = tris[i][2];
            tris[i][2] = s;
        }
        sr_ge_raster_coverage(p, tris[i], m, W, H);
        for (k = 0; k < W * H; ++k) {
            count[k] += m[k];
        }
    }
    for (py = 0; py < H; ++py) {
        for (px = 0; px < W; ++px) {
            int32_t sx = px * 16 + (int32_t)p->sample_offset, sy = py * 16 + (int32_t)p->sample_offset;
            int want = sx >= x0 && sx < x1 && sy >= y0 && sy < y1;
            int got = count[py * W + px];
            if (got != want) {
                ++errors;
                if (got > want) {
                    ++*doubles;
                } else {
                    ++*cracks;
                }
            }
        }
    }
    return errors;
}

static void test_tiling(void) {
    int iter, errors = 0, doubles = 0, cracks = 0;
    unsigned off;
    for (off = 0; off < 16; off += 8) {
        SrGeRasterParams p = params(off);
        for (iter = 0; iter < 1500; ++iter) {
            SrGeVertex fan[4][3], diag[2][3];
            int32_t x0 = rnd_coord(0, 300, off), y0 = rnd_coord(0, 300, off);
            int32_t x1 = rnd_coord(x0 + 1, 600, off), y1 = rnd_coord(y0 + 1, 600, off);
            int32_t cx = rnd_coord(x0, x1, off), cy = rnd_coord(y0, y1, off);
            SrGeVertex c00 = vtx(x0, y0), c10 = vtx(x1, y0), c11 = vtx(x1, y1), c01 = vtx(x0, y1), ctr = vtx(cx, cy);
            if (iter % 5 == 0) {
                /* centre on a diagonal, which puts shared edges through samples */
                int32_t k = rnd_range(0, 16);
                ctr = vtx(x0 + (x1 - x0) * k / 16, y0 + (y1 - y0) * k / 16);
            }
            fan[0][0] = ctr;
            fan[0][1] = c00;
            fan[0][2] = c10;
            fan[1][0] = ctr;
            fan[1][1] = c10;
            fan[1][2] = c11;
            fan[2][0] = ctr;
            fan[2][1] = c11;
            fan[2][2] = c01;
            fan[3][0] = ctr;
            fan[3][1] = c01;
            fan[3][2] = c00;
            errors += tiling_errors(&p, fan, 4, x0, y0, x1, y1, &doubles, &cracks);
            diag[0][0] = c00;
            diag[0][1] = c10;
            diag[0][2] = c11;
            diag[1][0] = c00;
            diag[1][1] = c11;
            diag[1][2] = c01;
            errors += tiling_errors(&p, diag, 2, x0, y0, x1, y1, &doubles, &cracks);
            diag[0][0] = c10;
            diag[0][1] = c11;
            diag[0][2] = c01;
            diag[1][0] = c10;
            diag[1][1] = c01;
            diag[1][2] = c00;
            errors += tiling_errors(&p, diag, 2, x0, y0, x1, y1, &doubles, &cracks);
        }
    }
    CHECK(errors == 0, "tiling: %d wrong samples (%d double-covered, %d cracks)", errors, doubles, cracks);
}

/* Arbitrary edge shared by two triangles on opposite sides: no sample is
   covered twice, and every sample strictly inside the shared segment is
   covered exactly once. */
static void test_shared_edges(void) {
    int iter, twice = 0, on_edge_bad = 0, on_edge_seen = 0;
    for (iter = 0; iter < 6000; ++iter) {
        unsigned off = (iter & 1) ? 8u : 0u;
        SrGeRasterParams p = params(off);
        enum { W = 32 };
        uint8_t m1[W * W], m2[W * W];
        SrGeVertex t1[3], t2[3];
        SrGeVertex a = vtx(rnd_range(0, 31) * 16, rnd_range(0, 31) * 16);
        SrGeVertex b = vtx(rnd_range(0, 31) * 16, rnd_range(0, 31) * 16);
        SrGeVertex c = vtx(rnd_range(0, 511), rnd_range(0, 511));
        SrGeVertex d;
        int64_t sc = sr_ge_area2(&a, &b, &c);
        int px, py, k;
        if (sc == 0) {
            continue;
        }
        /* reflect c through the edge's midpoint to land d on the other side */
        d = vtx(a.x + b.x - c.x, a.y + b.y - c.y);
        if (d.x < 0 || d.y < 0 || d.x > 511 || d.y > 511) {
            continue;
        }
        t1[0] = a;
        t1[1] = b;
        t1[2] = c;
        t2[0] = b;
        t2[1] = a;
        t2[2] = d;
        sr_ge_raster_coverage(&p, t1, m1, W, W);
        sr_ge_raster_coverage(&p, t2, m2, W, W);
        for (py = 0; py < W; ++py) {
            for (px = 0; px < W; ++px) {
                SrGeVertex s = vtx(px * 16 + (int32_t)off, py * 16 + (int32_t)off);
                k = py * W + px;
                if (m1[k] && m2[k]) {
                    ++twice;
                }
                if (sr_ge_area2(&a, &b, &s) == 0 && (s.x - a.x) * (s.x - b.x) + (s.y - a.y) * (s.y - b.y) < 0) {
                    ++on_edge_seen;
                    if (m1[k] + m2[k] != 1) {
                        ++on_edge_bad;
                    }
                }
            }
        }
    }
    CHECK(twice == 0, "%d samples covered by both triangles of a shared edge", twice);
    CHECK(on_edge_seen > 2000, "only %d on-edge samples exercised", on_edge_seen);
    CHECK(on_edge_bad == 0, "%d on-edge samples covered zero or two times", on_edge_bad);
}

static void test_winding_and_degenerate(void) {
    SrGeRasterParams p = params(8);
    SrGeRasterParams corner = params(0);
    int iter, bad = 0;
    uint8_t m[16 * 16];
    SrGeVertex t[3];
    for (iter = 0; iter < 2000; ++iter) {
        uint8_t ma[16 * 16], mb[16 * 16], mc[16 * 16];
        SrGeVertex r1[3], r2[3];
        int i;
        for (i = 0; i < 3; ++i) {
            t[i] = vtx(rnd_coord(0, 256, 8), rnd_coord(0, 256, 8));
        }
        r1[0] = t[0];
        r1[1] = t[2];
        r1[2] = t[1];
        r2[0] = t[1];
        r2[1] = t[2];
        r2[2] = t[0];
        sr_ge_raster_coverage(&p, t, ma, 16, 16);
        sr_ge_raster_coverage(&p, r1, mb, 16, 16);
        sr_ge_raster_coverage(&p, r2, mc, 16, 16);
        if (memcmp(ma, mb, sizeof ma) || memcmp(ma, mc, sizeof ma)) {
            ++bad;
        }
    }
    CHECK(bad == 0, "%d triangles changed coverage with winding or rotation", bad);

    t[0] = vtx(0, 0);
    t[1] = vtx(100, 50);
    t[2] = vtx(200, 100);
    CHECK(sr_ge_raster_coverage(&p, t, m, 16, 16) == SR_GE_RASTER_FLAG_DEGENERATE && count_mask(m, 256) == 0,
          "collinear triangle covers nothing");
    t[1] = t[0];
    CHECK(sr_ge_raster_coverage(&p, t, m, 16, 16) == SR_GE_RASTER_FLAG_DEGENERATE, "coincident vertices");
    CHECK(sr_ge_raster_covers(&p, t, 0, 0) == 0, "degenerate covers() is 0");
    t[1] = vtx(70000, 0);
    CHECK(sr_ge_raster_coverage(&p, t, m, 16, 16) == SR_GE_RASTER_FLAG_RANGE, "out-of-range vertex");
    CHECK(sr_ge_raster_coverage(&p, t, NULL, 16, 16) == SR_GE_RASTER_FLAG_INVALID, "NULL mask");
    CHECK(sr_ge_raster_coverage(&p, t, m, 0, 16) == SR_GE_RASTER_FLAG_INVALID, "zero width");
    /* a sliver one subpixel tall still owns the samples on its top edge */
    t[0] = vtx(0, 16);
    t[1] = vtx(160, 16);
    t[2] = vtx(0, 17);
    sr_ge_raster_coverage(&corner, t, m, 16, 16);
    CHECK(count_mask(m, 256) == 10, "sliver on a top edge covers 10 samples, got %d", count_mask(m, 256));
    t[2] = vtx(0, 15); /* same sliver above the line: the line is now its bottom edge */
    sr_ge_raster_coverage(&corner, t, m, 16, 16);
    CHECK(count_mask(m, 256) == 0, "sliver above a bottom edge covers nothing, got %d", count_mask(m, 256));
    {
        SrGeVertex area_t[3];
        area_t[0] = vtx(0, 0);
        area_t[1] = vtx(16, 0);
        area_t[2] = vtx(0, 16);
        CHECK(sr_ge_area2(&area_t[0], &area_t[1], &area_t[2]) == 256, "clockwise on screen is positive");
        CHECK(sr_ge_area2(&area_t[0], &area_t[2], &area_t[1]) == -256, "counter-clockwise is negative");
        area_t[1] = vtx(INT32_MAX, 0);
        CHECK(sr_ge_area2(&area_t[0], &area_t[1], &area_t[2]) == 0, "out-of-range coordinates yield 0, not overflow");
    }
}

/* ---- setup reciprocal and planes ---------------------------------------- */

/* SYNTHETIC setup table: chords of 1/(1 + f) with bases rounded down to a
   multiple of 16, F = 20, 12 position bits.  NOT PSP coefficients. */
static void synthetic_setup_table(SrGeSetupRecipTable* t) {
    int i;
    t->out_frac_bits = 20;
    t->t_bits = 12;
    t->slope_shift = 12;
    for (i = 0; i < SR_GE_SETUP_SEGMENTS; ++i) {
        uint32_t y0 = (uint32_t)floor(ldexp(1.0, 20) / (1.0 + i / 256.0));
        uint32_t y1 = (uint32_t)floor(ldexp(1.0, 20) / (1.0 + (i + 1) / 256.0));
        y0 &= ~15u;
        t->base[i] = y0;
        t->slope[i] = y0 - y1;
    }
}

static void test_setup_table(void) {
    SrGeRasterParams p = params(0), pin = params(0);
    SrGeSetupRecipTable t, bad;
    unsigned tt, f;
    int bad_idx = 0;
    pin.start16 = SR_GE_START16_RECIP_INPUT;
    synthetic_setup_table(&t);
    CHECK(sr_ge_setup_table_check(&p, &t) == 0, "synthetic table validates");
    bad = t;
    bad.base[17] += 1u;
    CHECK(sr_ge_setup_table_check(&p, &bad) == SR_GE_RASTER_FLAG_INVALID,
          "TABLE_BASES reading rejects a base that is not a multiple of 16");
    CHECK(sr_ge_setup_table_check(&pin, &bad) == 0, "RECIP_INPUT reading accepts it");
    bad = t;
    bad.out_frac_bits = 21;
    CHECK(sr_ge_setup_table_check(&p, &bad) == SR_GE_RASTER_FLAG_INVALID, "F too large");
    bad = t;
    bad.t_bits = 17;
    CHECK(sr_ge_setup_table_check(&p, &bad) == SR_GE_RASTER_FLAG_INVALID, "t_bits too large");
    bad = t;
    bad.slope[255] = bad.base[255];
    CHECK(sr_ge_setup_table_check(&p, &bad) == SR_GE_RASTER_FLAG_INVALID, "endpoint below 1/2");
    bad = t;
    bad.base[0] = (1u << 20) + 16u;
    CHECK(sr_ge_setup_table_check(&p, &bad) == SR_GE_RASTER_FLAG_INVALID, "base above 1");

    CHECK(sr_ge_setup_segment(&t, 1, &tt) == 0 && tt == 0, "area 1");
    CHECK(sr_ge_setup_segment(&t, 3, &tt) == 128 && tt == 0, "area 3 = 2 * 1.5");
    CHECK(sr_ge_setup_segment(&t, (1u << 20) + (1u << 12), &tt) == 1 && tt == 0, "first position of segment 1");
    CHECK(sr_ge_setup_segment(&t, (1u << 21) - 1u, &tt) == 255 && tt == 4095, "last position");
    CHECK(sr_ge_setup_segment(&t, UINT64_C(1) << 32, &tt) == 0 && tt == 0, "large power-of-two area");
    CHECK(sr_ge_setup_segment(&t, (UINT64_C(1) << 32) + (UINT64_C(0xAB) << 24) + (UINT64_C(0x123) << 12), &tt) ==
                  0xABu &&
              tt == 0x123u,
          "right-shift path with fraction bits");
    for (f = 0; f < (1u << 20); ++f) {
        unsigned idx = sr_ge_setup_segment(&t, (1u << 20) + f, &tt);
        if (idx != (f >> 12) || tt != (f & 0xFFFu)) {
            ++bad_idx;
        }
    }
    CHECK(bad_idx == 0, "%d segment decompositions wrong at k = 20", bad_idx);
}

static int same_plane(const SrGePlane* a, const SrGePlane* b) {
    return a->ax == b->ax && a->ay == b->ay && a->start == b->start && a->dx == b->dx && a->dy == b->dy &&
           a->frac_bits == b->frac_bits;
}

static void test_planes(void) {
    SrGeRasterParams p = params(0);
    SrGeSetupRecipTable t;
    SrGePlane pl, pl2;
    SrGeVertex tri[3];
    int32_t attr[3];
    int64_t v;
    uint32_t fl;
    int i, bad = 0;
    synthetic_setup_table(&t);

    /* Exact case: doubled area 2^16 hits segment 0, t = 0, y = 2^20 exactly. */
    tri[0] = vtx(0, 0);
    tri[1] = vtx(256, 0);
    tri[2] = vtx(0, 256);
    attr[0] = 1000;
    attr[1] = 1000 + 4096;
    attr[2] = 1000 - 2048;
    fl = sr_ge_plane_setup(&p, &t, tri, attr, &pl);
    CHECK(fl == 0, "exact setup flags %#x", fl);
    CHECK(pl.dx == 16 * 256 && pl.dy == -8 * 256, "exact gradients %lld %lld", (long long)pl.dx, (long long)pl.dy);
    for (i = 0; i < 3; ++i) {
        sr_ge_plane_eval(&pl, tri[i].x, tri[i].y, &v);
        CHECK(v == (int64_t)attr[i] * 256, "vertex %d reproduced exactly: %lld", i, (long long)v);
    }
    /* R3 screen-linear: the edge midpoint is the plain average of its ends */
    sr_ge_plane_eval(&pl, 128, 128, &v);
    CHECK(v == (int64_t)(attr[1] + attr[2]) / 2 * 256, "midpoint is the screen-space average: %lld", (long long)v);
    sr_ge_plane_eval(&pl, 129, 0, &v);
    {
        int64_t v0;
        sr_ge_plane_eval(&pl, 128, 0, &v0);
        CHECK(v - v0 == pl.dx, "one subpixel step adds dx");
    }

    /* R4 anchoring: leftmost vertex, start equals the anchor attribute. */
    tri[0] = vtx(300, 10);
    tri[1] = vtx(20, 200);
    tri[2] = vtx(500, 400);
    CHECK(sr_ge_plane_anchor_leftmost(&p, tri) == 1, "leftmost is vertex 1");
    attr[0] = 5;
    attr[1] = 77;
    attr[2] = -9;
    sr_ge_plane_setup(&p, &t, tri, attr, &pl);
    CHECK(pl.ax == 20 && pl.ay == 200 && pl.start == 77 * 256, "plane anchored at the leftmost vertex");
    sr_ge_plane_eval(&pl, 20, 200, &v);
    CHECK(v == 77 * 256, "anchor value is exact");
    /* A3 ties */
    tri[2] = vtx(20, 50);
    CHECK(sr_ge_plane_anchor_leftmost(&p, tri) == 2, "TOPMOST tie picks the smaller y");
    {
        SrGeRasterParams first = p;
        first.anchor_tie = SR_GE_ANCHOR_TIE_FIRST;
        CHECK(sr_ge_plane_anchor_leftmost(&first, tri) == 1, "FIRST tie picks the lower index");
    }

    /* Every vertex order gives the same plane under the TOPMOST reading, for
       both gradient-rounding readings (reversed orders have negative area). */
    for (i = 0; i < 4000; ++i) {
        SrGeRasterParams pr = p;
        static const int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        SrGeVertex base_t[3];
        int32_t base_a[3];
        int j, k;
        pr.grad_round = (i & 1) ? SR_GE_GRAD_FLOOR : SR_GE_GRAD_TOWARD_ZERO;
        for (k = 0; k < 3; ++k) {
            base_t[k] = vtx(rnd_range(0, 2000), rnd_range(0, 2000));
            base_a[k] = rnd_range(-100000, 100000);
        }
        if (i % 3 == 0) {
            base_t[1].x = base_t[0].x; /* force a leftmost tie now and then */
        }
        if (sr_ge_plane_setup(&pr, &t, base_t, base_a, &pl) & SR_GE_RASTER_FLAG_DEGENERATE) {
            continue;
        }
        for (j = 1; j < 6; ++j) {
            SrGeVertex pt[3];
            int32_t pa[3];
            for (k = 0; k < 3; ++k) {
                pt[k] = base_t[perm[j][k]];
                pa[k] = base_a[perm[j][k]];
            }
            sr_ge_plane_setup(&pr, &t, pt, pa, &pl2);
            if (!same_plane(&pl, &pl2)) {
                ++bad;
            }
        }
    }
    CHECK(bad == 0, "%d planes changed with vertex order", bad);

    /* Arbitrary areas through the synthetic table stay within the error the
       table and the gradient truncation can introduce. */
    bad = 0;
    {
    int executed = 0;
    for (i = 0; i < 20000; ++i) {
        int k;
        double ex_dx, ex_dy, area;
        for (k = 0; k < 3; ++k) {
            tri[k] = vtx(rnd_range(0, 4000), rnd_range(0, 4000));
            attr[k] = rnd_range(SR_GE_ATTR_MIN, SR_GE_ATTR_MAX);
        }
        fl = sr_ge_plane_setup(&p, &t, tri, attr, &pl);
        if (fl == SR_GE_RASTER_FLAG_DEGENERATE) {
            continue;
        }
        if (fl & ~SR_GE_RASTER_FLAG_INEXACT) {
            ++bad; /* random in-range input must set up */
            continue;
        }
        ++executed;
        area = (double)sr_ge_area2(&tri[0], &tri[1], &tri[2]);
        ex_dx = ((double)(attr[1] - attr[0]) * (tri[2].y - tri[0].y) -
                 (double)(attr[2] - attr[0]) * (tri[1].y - tri[0].y)) /
                area * 256.0;
        ex_dy = ((double)(tri[1].x - tri[0].x) * (attr[2] - attr[0]) -
                 (double)(tri[2].x - tri[0].x) * (attr[1] - attr[0])) /
                area * 256.0;
        for (k = 0; k < 3; ++k) {
            double ddx = tri[k].x - pl.ax, ddy = tri[k].y - pl.ay, err, bound;
            if (sr_ge_plane_eval(&pl, tri[k].x, tri[k].y, &v)) {
                ++bad;
                continue;
            }
            err = fabs((double)v - (double)attr[k] * 256.0);
            /* relative reciprocal error < 2^-14 (bases floored by up to
               16/2^19 = 2^-15, chord error h^2 * max|f''| / 8 = 2^-18, the
               area significand cut to 20 bits and the truncated interpolation
               each <= 2^-20), plus one truncated gradient unit per subpixel
               moved */
            bound = ldexp(1.0, -14) * (fabs(ex_dx * ddx) + fabs(ex_dy * ddy)) + fabs(ddx) + fabs(ddy) + 1.0;
            if (err > bound) {
                ++bad;
            }
        }
    }
    CHECK(bad == 0, "%d plane vertices outside the table error bound", bad);
    CHECK(executed > 19000, "only %d random planes were checked", executed);
    }

    /* invalid inputs */
    attr[0] = SR_GE_ATTR_MAX + 1;
    CHECK(sr_ge_plane_setup(&p, &t, tri, attr, &pl) == SR_GE_RASTER_FLAG_INVALID, "attribute out of range");
    attr[0] = 0;
    CHECK(sr_ge_plane_setup_anchored(&p, &t, tri, attr, 3, &pl) == SR_GE_RASTER_FLAG_INVALID, "anchor 3");
    CHECK(sr_ge_plane_setup(&p, NULL, tri, attr, &pl) == SR_GE_RASTER_FLAG_INVALID, "NULL table");
    {
        SrGeRasterParams wide = p;
        wide.grad_frac_bits = 20;
        t.out_frac_bits = 19;
        CHECK(sr_ge_plane_setup(&wide, &t, tri, attr, &pl) == SR_GE_RASTER_FLAG_INVALID, "grad bits above F");
        synthetic_setup_table(&t);
    }
    tri[0] = vtx(0, 0);
    tri[1] = vtx(10, 10);
    tri[2] = vtx(20, 20);
    CHECK(sr_ge_plane_setup(&p, &t, tri, attr, &pl) == SR_GE_RASTER_FLAG_DEGENERATE, "degenerate plane");
}

static void test_start16_and_rounding(void) {
    SrGeRasterParams tb = params(0), ri = params(0), ps = params(0), fl = params(0);
    SrGeSetupRecipTable t;
    SrGePlane pl;
    SrGeVertex tri[3];
    int32_t attr[3];
    uint32_t f;
    synthetic_setup_table(&t);
    tb.grad_frac_bits = 0;
    ri.grad_frac_bits = 0;
    ri.start16 = SR_GE_START16_RECIP_INPUT;
    ps.start16 = SR_GE_START16_PLANE_START;

    /* doubled area 25: the RECIP_INPUT reading divides by 16 instead.
       num_x = 80 * 5 = 400: 400 / 16 = 25 vs. about 400 / 25 = 16. */
    tri[0] = vtx(0, 0);
    tri[1] = vtx(5, 0);
    tri[2] = vtx(0, 5);
    attr[0] = 0;
    attr[1] = 80;
    attr[2] = 0;
    f = sr_ge_plane_setup(&ri, &t, tri, attr, &pl);
    CHECK(pl.dx == 25 && (f & SR_GE_RASTER_FLAG_INEXACT), "RECIP_INPUT gradient %lld", (long long)pl.dx);
    sr_ge_plane_setup(&tb, &t, tri, attr, &pl);
    CHECK(pl.dx == 15 || pl.dx == 16, "TABLE_BASES gradient %lld", (long long)pl.dx);
    tri[1] = vtx(3, 0);
    tri[2] = vtx(0, 3); /* doubled area 9 rounds to 0 */
    pl.dx = pl.dy = pl.start = 12345;
    CHECK(sr_ge_plane_setup(&ri, &t, tri, attr, &pl) == (SR_GE_RASTER_FLAG_INEXACT | SR_GE_RASTER_FLAG_NO_SETUP),
          "area below 16 has no plane under RECIP_INPUT");
    CHECK(pl.dx == 0 && pl.dy == 0 && pl.start == 0, "a failed setup leaves a zeroed plane");
    {
        uint8_t m[4];
        CHECK(sr_ge_raster_coverage(&ri, tri, m, 2, 2) == 0 && m[0] == 1, "the same triangle still has coverage");
    }
    CHECK((sr_ge_plane_setup(&tb, &t, tri, attr, &pl) & ~SR_GE_RASTER_FLAG_INEXACT) == 0, "other readings set it up");

    /* PLANE_START: with 0 fraction bits the anchor value itself is floored
       to a multiple of 16. */
    ps.grad_frac_bits = 0;
    tri[1] = vtx(256, 0);
    tri[2] = vtx(0, 256);
    attr[0] = 37;
    sr_ge_plane_setup(&ps, &t, tri, attr, &pl);
    CHECK(pl.start == 32, "PLANE_START floors 37 to 32, got %lld", (long long)pl.start);
    attr[0] = -37;
    sr_ge_plane_setup(&ps, &t, tri, attr, &pl);
    CHECK(pl.start == -48, "PLANE_START floors -37 to -48, got %lld", (long long)pl.start);
    sr_ge_plane_setup(&tb, &t, tri, attr, &pl);
    CHECK(pl.start == -37, "other readings keep the start, got %lld", (long long)pl.start);

    /* A6: a negative inexact gradient differs by one unit between readings. */
    fl.grad_round = SR_GE_GRAD_FLOOR;
    fl.grad_frac_bits = 0;
    tri[0] = vtx(0, 0);
    tri[1] = vtx(48, 0);
    tri[2] = vtx(0, 48);
    attr[0] = 0;
    attr[1] = -100; /* -100/48 = -2.083 per subpixel */
    attr[2] = 0;
    f = sr_ge_plane_setup(&tb, &t, tri, attr, &pl);
    CHECK(f == SR_GE_RASTER_FLAG_INEXACT, "an inexact gradient reports INEXACT, flags %#x", f);
    {
        int64_t toward_zero = pl.dx;
        sr_ge_plane_setup(&fl, &t, tri, attr, &pl);
        CHECK(toward_zero == -2 && pl.dx == -3, "gradient readings %lld / %lld", (long long)toward_zero,
              (long long)pl.dx);
    }
    attr[1] = 100;
    sr_ge_plane_setup(&fl, &t, tri, attr, &pl);
    CHECK(pl.dx == 2, "positive gradients truncate under both readings");
    /* the floor reading applies to dy as well */
    attr[1] = 0;
    attr[2] = -100;
    sr_ge_plane_setup(&tb, &t, tri, attr, &pl);
    {
        int64_t toward_zero = pl.dy;
        sr_ge_plane_setup(&fl, &t, tri, attr, &pl);
        CHECK(toward_zero == -2 && pl.dy == -3 && pl.dx == 0, "dy readings %lld / %lld", (long long)toward_zero,
              (long long)pl.dy);
    }

    /* overflow is visible, not wrapped */
    pl.dx = INT64_C(1) << 61;
    pl.dy = 0;
    pl.start = 0;
    pl.ax = pl.ay = 0;
    {
        int64_t v = 123;
        CHECK(sr_ge_plane_eval(&pl, 8, 0, &v) == SR_GE_RASTER_FLAG_OVERFLOW && v == 0, "overflow flagged");
        CHECK(sr_ge_plane_eval(&pl, 3, 0, &v) == 0 && v == 3 * (INT64_C(1) << 61), "in-range product");
        CHECK(sr_ge_plane_eval(&pl, -4, 0, &v) == 0 && v == INT64_MIN, "INT64_MIN is representable");
        CHECK(sr_ge_plane_eval(&pl, -5, 0, &v) == SR_GE_RASTER_FLAG_OVERFLOW, "below INT64_MIN");
        pl.dx = 1;
        pl.start = INT64_MAX - 4;
        CHECK(sr_ge_plane_eval(&pl, 4, 0, &v) == 0 && v == INT64_MAX, "start + dx term reaches INT64_MAX");
        CHECK(sr_ge_plane_eval(&pl, 5, 0, &v) == SR_GE_RASTER_FLAG_OVERFLOW, "start + dx term overflows");
        pl.dx = 0;
        pl.dy = -1;
        pl.start = INT64_MIN + 2;
        CHECK(sr_ge_plane_eval(&pl, 0, 3, &v) == SR_GE_RASTER_FLAG_OVERFLOW, "dy term underflows");
        pl.start = 0;
        pl.dy = INT64_C(1) << 61;
        CHECK(sr_ge_plane_eval(&pl, 0, 4, &v) == SR_GE_RASTER_FLAG_OVERFLOW, "dy product overflows");
    }
}

/* ---- colour factors ----------------------------------------------------- */

static void test_color(void) {
    uint32_t x, s;
    int bad = 0, sym = 0, mono = 0, diff255 = 0, diff8 = 0;
    for (x = 0; x < 256; ++x) {
        for (s = 0; s < 256; ++s) {
            uint32_t v = sr_ge_color_mul(x, s);
            /* (2x+1)(2s+1)/1024 = (x + 1/2)(s + 1/2)/256 */
            uint32_t model = (uint32_t)floor((x + 0.5) * (s + 0.5) / 256.0);
            if (v != model || sr_ge_color_mul_signed(x, (int32_t)s) != (int32_t)v) {
                ++bad;
            }
            if (v != sr_ge_color_mul(s, x)) {
                ++sym;
            }
            if (s > 0 && v < sr_ge_color_mul(x, s - 1)) {
                ++mono;
            }
            if (v != (x * s + 127u) / 255u) {
                ++diff255;
            }
            if (v != ((x * s) >> 8)) {
                ++diff8;
            }
        }
        if (sr_ge_color_mul(x, 255) != x || sr_ge_color_mul(x, 0) != 0) {
            ++bad;
        }
    }
    CHECK(bad == 0, "%d colour products wrong", bad);
    CHECK(sym == 0 && mono == 0, "colour product must be symmetric and monotone");
    CHECK(diff255 > 0 && diff8 > 0, "R6 must differ from x*s/255 (%d) and x*s>>8 (%d)", diff255, diff8);
    CHECK(sr_ge_color_mul(128, 128) == 64 && sr_ge_color_mul(200, 100) == 78 && sr_ge_color_mul(16, 16) == 1 &&
              sr_ge_color_mul(1, 1) == 0 && sr_ge_color_mul(255, 255) == 255,
          "pinned products");
    CHECK(sr_ge_color_mul(256, 0) == UINT32_MAX, "out-of-range operand");

    /* R7: 1 - 2*alpha is not clamped at zero; "1" is 255 or 256 (A8). */
    CHECK(sr_ge_inverse_factor(0, 255) == 255 && sr_ge_inverse_factor(0, 256) == 256, "alpha 0");
    CHECK(sr_ge_inverse_factor(200, 255) == -145 && sr_ge_inverse_factor(200, 256) == -144, "alpha 200 unclamped");
    CHECK(sr_ge_inverse_factor(255, 255) == -255, "alpha 255");
    CHECK(sr_ge_inverse_factor(128, 256) == 0 && sr_ge_inverse_factor(128, 255) == -1, "alpha 128");
    CHECK(sr_ge_inverse_factor(1, 254) == INT32_MIN && sr_ge_inverse_factor(256, 255) == INT32_MIN, "bad args");
    /* a negative factor floors: 511 * -289 = -147679, / 1024 = -144.2 -> -145 */
    CHECK(sr_ge_color_mul_signed(255, -145) == -145, "signed product floors, got %d",
          (int)sr_ge_color_mul_signed(255, -145));
    CHECK(sr_ge_color_mul_signed(0, -1) == -1, "(1)(-1) / 1024 floors to -1");
    CHECK(sr_ge_color_mul_signed(0, 0) == 0, "(1)(1) / 1024 is 0");
    /* 511 * 513 = 262143 -> 255: even factor 256 does not reach 256 */
    CHECK(sr_ge_color_mul_signed(255, 256) == 255, "factor 256 product, got %d",
          (int)sr_ge_color_mul_signed(255, 256));
    CHECK(sr_ge_color_mul_signed(255, 513) == INT32_MIN, "factor out of range");
}

int main(void) {
    test_params();
    test_snap();
    test_hand_coverage();
    test_model_agreement();
    test_tiling();
    test_shared_edges();
    test_winding_and_degenerate();
    test_setup_table();
    test_planes();
    test_start16_and_rounding();
    test_color();
    if (g_failures) {
        printf("GE_RASTER_REF_SELFTEST FAIL failures=%d checks=%d\n", g_failures, g_checks);
        return 1;
    }
    printf("GE_RASTER_REF_SELFTEST PASS checks=%d\n", g_checks);
    return 0;
}
