/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_scale_selftest.c - texture scaling's CPU half (tex_scale.c) and the xBRZ translation it
 * runs (third_party/xbrz), with no GPU:
 *   - xbrz-golden: every factor 2-6 of two synthetic patterns equals the goldens printed by
 *     third_party/xbrz/xbrz_golden.cpp built against the pristine upstream xBRZ 1.9 source
 *     (ColorFormat::argbUnbuffered), and a 2x corner equals upstream's pixels one by one;
 *   - wrap and clamp: a repeating texture's copy is the middle of a 3x3 tiling's copy (no seam),
 *     a clamped one the middle of an edge-extended copy, and the R/B channel order is upstream's;
 *   - jobs: a queued job returns the same pixels with its serial and hash; a held queue keeps a
 *     job queued and a full one refuses the next; nothing waits for a worker;
 *   - settings, the per-size factor and the budget;
 *   - the smart 2D classifier: 1:1 sprites and triangles (either direction) are pixel-mapped;
 *     magnified, offset, rotated and perspective draws are not.
 */
#include "tex_scale.h"
#include "../../../third_party/xbrz/xbrz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "tex scale selftest: FAIL " __VA_ARGS__); \
                                              fputc('\n', stderr); s_fail = 1; } } while (0)

/* ---- the goldens (third_party/xbrz/xbrz_golden.cpp, upstream xBRZ 1.9) -------------------- */

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

/* Identical to xbrz_golden.cpp's xbrz_pattern(). */
static void xbrz_pattern(int which, uint32_t *px, int w, int h)
{
    uint32_t s = 0x1234567u + (uint32_t)which * 7919u;
    static const uint32_t pal[6] = { 0xFF202020u, 0xFFE0E0E0u, 0xFFC03020u, 0xFF2050D0u, 0xFF30A040u, 0xFFE0C040u };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (which == 0) {
                c = 0xFF406080u;
                int dx = x - 28, dy = y - 9;
                if (dx * dx + dy * dy <= 30) c = 0xFFF0D020u;
                if (x == y || x == 2 * y + 3 || 3 * x == y + 40) c = 0xFF101010u;
                if (x >= 2 && x < 10 && y >= 16 && y < 24 && ((x + y) & 1)) c = 0xFFFFFFFFu;
                if ((x * 7 + y * 13) % 41 == 0) c = 0xFFFF00FFu;
                if (y >= 24 && y < 26) c = (c & 0x00FFFFFFu) | 0x80000000u;
                if (x >= 33 && x < 38 && y >= 18 && y < 23) c = 0x00FF0000u + (uint32_t)x;
            } else {
                uint32_t r = lcg(&s);
                c = pal[r % 6u];
                uint32_t a = (r >> 4) % 7u;
                if (a == 0) c &= 0x00FFFFFFu;
                else if (a == 1) c = (c & 0x00FFFFFFu) | 0x40000000u;
            }
            px[y * w + x] = c;
        }
}

static uint64_t fnv64(const uint32_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 4; b++) { h ^= (p[i] >> (8 * b)) & 0xFFu; h *= 1099511628211ull; }
    return h;
}

static const struct { int which, factor; uint64_t hash; } k_goldens[] = {
    { 0, 2, 0x1f622b632e085e26ull },
    { 0, 3, 0x532ce942455cbe16ull },
    { 0, 4, 0x5581a6e15bd6f398ull },
    { 0, 5, 0xcc3287507f08f4ceull },
    { 0, 6, 0x3d29a40b5d299c50ull },
    { 1, 2, 0x4ed6616d3fe44ca8ull },
    { 1, 3, 0xdfa17729c4f907c9ull },
    { 1, 4, 0x848421983110c274ull },
    { 1, 5, 0x153e9dea3871246bull },
    { 1, 6, 0xb8ecdb2fa88e7a3aull },
};

/* xbrz::scale(2, <top-left 6x4 of pattern 0>, ..., argbUnbuffered), row by row. */
static const uint32_t k_corner_2x[12 * 8] = {
    0xC9FF00FFu, 0xFFFF00FFu, 0xFFA030C0u, 0xFF406080u, 0xFF406080u, 0xFF283848u,
    0xFF101010u, 0xFF101010u, 0xFF283848u, 0xFF406080u, 0xFF406080u, 0xC9406080u,
    0xFFFF00FFu, 0xFFA030C0u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u,
    0xFF1A2128u, 0xFF1A2128u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u,
    0xFFA030C0u, 0xFF406080u, 0xFF1A2128u, 0xFF283848u, 0xFF406080u, 0xFF406080u,
    0xFF406080u, 0xFF406080u, 0xFFD714E4u, 0xFFD714E4u, 0xFF101010u, 0xFF1A2128u,
    0xFF406080u, 0xFF406080u, 0xFF283848u, 0xFF101010u, 0xFF283848u, 0xFF406080u,
    0xFF406080u, 0xFF406080u, 0xFFD714E4u, 0xFFD714E4u, 0xFF101010u, 0xFF1A2128u,
    0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF283848u, 0xFF101010u, 0xFF283848u,
    0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u,
    0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF283848u, 0xFF101010u,
    0xFF283848u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u,
    0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF283848u,
    0xFF101010u, 0xFF283848u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u,
    0xC9406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u, 0xFF406080u,
    0xFF101010u, 0xFF101010u, 0xFF283848u, 0xFF406080u, 0xFF406080u, 0xC9406080u,
};

static void test_golden(void) {
    static const int dims[2][2] = { { 40, 28 }, { 23, 17 } };
    for (size_t g = 0; g < sizeof(k_goldens) / sizeof(k_goldens[0]); g++) {
        const int which = k_goldens[g].which, f = k_goldens[g].factor;
        const int w = dims[which][0], h = dims[which][1];
        uint32_t *src = (uint32_t *)malloc((size_t)w * h * 4u);
        uint32_t *trg = (uint32_t *)malloc((size_t)w * h * f * f * 4u);
        if (!src || !trg) { CHECK(0, "xbrz-golden: no memory"); free(src); free(trg); return; }
        xbrz_pattern(which, src, w, h);
        int ok = nk_xbrz_scale(f, src, trg, w, h, NULL, 0, h);
        uint64_t got = fnv64(trg, (size_t)w * h * f * f);
        CHECK(ok && got == k_goldens[g].hash, "xbrz-golden pattern=%d factor=%d hash=%016llx want %016llx",
              which, f, (unsigned long long)got, (unsigned long long)k_goldens[g].hash);
        free(src);
        free(trg);
    }
    uint32_t full[40 * 28], corner[6 * 4], out[12 * 8];
    xbrz_pattern(0, full, 40, 28);
    for (int y = 0; y < 4; y++) memcpy(&corner[y * 6], &full[y * 40], 6u * 4u);
    nk_xbrz_scale(2, corner, out, 6, 4, NULL, 0, 4);
    for (int i = 0; i < 12 * 8; i++)
        if (out[i] != k_corner_2x[i]) {
            CHECK(0, "xbrz-golden corner 2x pixel %d = %08x, upstream %08x", i, out[i], k_corner_2x[i]);
            break;
        }
    /* Slices: two halves scaled separately equal the whole (upstream's [yFirst, yLast)). */
    uint32_t *whole = (uint32_t *)malloc(40u * 28u * 9u * 4u), *halves = (uint32_t *)malloc(40u * 28u * 9u * 4u);
    if (whole && halves) {
        nk_xbrz_scale(3, full, whole, 40, 28, NULL, 0, 28);
        nk_xbrz_scale(3, full, halves, 40, 28, NULL, 0, 14);
        nk_xbrz_scale(3, full, halves, 40, 28, NULL, 14, 28);
        CHECK(memcmp(whole, halves, 40u * 28u * 9u * 4u) == 0, "xbrz-golden: two slices differ from one pass");
    }
    free(whole);
    free(halves);
    CHECK(!nk_xbrz_scale(7, corner, out, 6, 4, NULL, 0, 4) && !nk_xbrz_scale(0, corner, out, 6, 4, NULL, 0, 4),
          "xbrz-golden: factors 0 and 7 were accepted");
    if (!s_fail) printf("tex scale selftest: PASS xbrz-golden factors=2-6 patterns=2 corner=12x8\n");
}

/* ---- wrap, clamp and channel order -------------------------------------------------------- */

static uint32_t swap_rb(uint32_t p) { return (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu); }

/* Reference: scale `ext` (RGBA, ew x eh) with the xBRZ translation in upstream's ARGB order,
 * and cut the w x h block at (ox, oy) of the source out of the result. */
static void reference_crop(const uint32_t *ext, int ew, int eh, int f, int ox, int oy, int w, int h,
                           uint32_t *out) {
    uint32_t *argb = (uint32_t *)malloc((size_t)ew * eh * 4u);
    uint32_t *big = (uint32_t *)malloc((size_t)ew * eh * f * f * 4u);
    if (!argb || !big) { free(argb); free(big); return; }
    for (int i = 0; i < ew * eh; i++) argb[i] = swap_rb(ext[i]);
    nk_xbrz_scale(f, argb, big, ew, eh, NULL, 0, eh);
    for (int y = 0; y < h * f; y++)
        for (int x = 0; x < w * f; x++)
            out[(size_t)y * w * f + x] = swap_rb(big[(size_t)(y + oy * f) * ew * f + x + ox * f]);
    free(argb);
    free(big);
}

static void test_wrap_clamp(void) {
    enum { W = 13, H = 9, F = 3, E = 4 };
    uint32_t tex[W * H];
    uint32_t s = 99u;
    for (int i = 0; i < W * H; i++) {
        /* a few colours whose red and blue differ, in runs, so xBRZ blends along edges */
        static const uint32_t pal[4] = { 0xFF1020E0u, 0xFFE02010u, 0xFF20C040u, 0x80FFFFFFu };
        tex[i] = pal[(lcg(&s) >> 3) % 4u];
        if (i % W > 3 && i % W < 7 && i / W > 2) tex[i] = pal[0];
    }
    uint32_t got[W * F * H * F], want[W * F * H * F];
    /* repeat: the middle tile of a 3x3 tiling */
    uint32_t tiled[3 * W * 3 * H];
    for (int y = 0; y < 3 * H; y++)
        for (int x = 0; x < 3 * W; x++) tiled[y * 3 * W + x] = tex[(y % H) * W + (x % W)];
    CHECK(nk_tex_scale_image(tex, W, H, F, 0, 0, got), "wrap: scale failed");
    reference_crop(tiled, 3 * W, 3 * H, F, W, H, W, H, want);
    CHECK(memcmp(got, want, sizeof(got)) == 0, "wrap: a repeating texture's copy has a seam or the wrong channel order");
    /* clamp: the middle of a copy of the texture extended by its edge texels */
    uint32_t ext[(W + 2 * E) * (H + 2 * E)];
    for (int y = 0; y < H + 2 * E; y++)
        for (int x = 0; x < W + 2 * E; x++) {
            int sx = x - E < 0 ? 0 : (x - E >= W ? W - 1 : x - E);
            int sy = y - E < 0 ? 0 : (y - E >= H ? H - 1 : y - E);
            ext[y * (W + 2 * E) + x] = tex[sy * W + sx];
        }
    CHECK(nk_tex_scale_image(tex, W, H, F, 1, 1, got), "clamp: scale failed");
    reference_crop(ext, W + 2 * E, H + 2 * E, F, E, E, W, H, want);
    CHECK(memcmp(got, want, sizeof(got)) == 0, "clamp: a clamped texture's copy differs from its edge-extended copy");
    /* mixed: repeat in u, clamp in v */
    uint32_t mixed[3 * W * (H + 2 * E)];
    for (int y = 0; y < H + 2 * E; y++)
        for (int x = 0; x < 3 * W; x++) {
            int sy = y - E < 0 ? 0 : (y - E >= H ? H - 1 : y - E);
            mixed[y * 3 * W + x] = tex[sy * W + (x % W)];
        }
    CHECK(nk_tex_scale_image(tex, W, H, F, 0, 1, got), "mixed: scale failed");
    reference_crop(mixed, 3 * W, H + 2 * E, F, W, E, W, H, want);
    CHECK(memcmp(got, want, sizeof(got)) == 0, "mixed: repeat-u clamp-v copy differs");
    if (!s_fail) printf("tex scale selftest: PASS wrap-clamp repeat, clamp and mixed edges, R/B order\n");
}

/* ---- jobs ---------------------------------------------------------------------------------- */

static void test_jobs(void) {
    enum { W = 20, H = 12, F = 2 };
    uint32_t tex[W * H], want[W * H * F * F];
    uint32_t s = 7u;
    for (int i = 0; i < W * H; i++) tex[i] = 0xFF000000u | (lcg(&s) & 0x00C0C0C0u);
    nk_tex_scale_image(tex, W, H, F, 1, 0, want);
    NkTexScaleResult r;
    CHECK(!nk_tex_scale_take(&r), "jobs: a result before any job");

    CHECK(nk_tex_scale_submit(41u, 0xABCDu, tex, W, H, F, 1, 0), "jobs: submit refused");
    tex[0] ^= 0x00FFFFFFu;   /* the job owns its own copy */
    nk_tex_scale_wait_idle();
    CHECK(nk_tex_scale_take(&r) && r.serial == 41u && r.hash == 0xABCDu && r.w == W && r.h == H &&
          r.factor == F && memcmp(r.rgba, want, sizeof(want)) == 0,
          "jobs: the result is not the job's own scale with its serial and hash");
    nk_tex_scale_release(&r);
    tex[0] ^= 0x00FFFFFFu;

    /* held: the job stays queued, and a take does not wait for it */
    nk_tex_scale_set_hold(1);
    CHECK(nk_tex_scale_submit(42u, 1u, tex, W, H, F, 1, 0), "jobs: held submit refused");
    nk_tex_scale_wait_idle();
    CHECK(!nk_tex_scale_take(&r), "jobs: a held job finished");
    int queued = 1;
    while (nk_tex_scale_can_submit()) {
        if (!nk_tex_scale_submit(100u + (uint64_t)queued, 1u, tex, W, H, F, 1, 0)) break;
        queued++;
    }
    CHECK(queued == NK_TEX_SCALE_QUEUE && !nk_tex_scale_submit(999u, 1u, tex, W, H, F, 1, 0),
          "jobs: the queue held %d jobs, not %d, or took one more", queued, NK_TEX_SCALE_QUEUE);
    nk_tex_scale_set_hold(0);
    nk_tex_scale_wait_idle();
    int taken = 0;
    uint64_t last = 0;
    int ordered = 1;
    while (nk_tex_scale_take(&r)) {
        if (r.serial < last) ordered = 0;
        last = r.serial;
        taken++;
        nk_tex_scale_release(&r);
    }
    CHECK(taken == NK_TEX_SCALE_QUEUE && ordered, "jobs: took %d of %d, in order %d", taken, NK_TEX_SCALE_QUEUE, ordered);

    /* sync: the result is ready when submit returns */
    nk_tex_scale_set_sync(1);
    CHECK(nk_tex_scale_submit(7u, 2u, tex, W, H, F, 1, 0) && nk_tex_scale_take(&r) && r.serial == 7u &&
          memcmp(r.rgba, want, sizeof(want)) == 0, "jobs: sync submit did not finish its job");
    nk_tex_scale_release(&r);
    nk_tex_scale_set_sync(0);
    NkTexScaleStats st;
    nk_tex_scale_stats(&st);
    CHECK(st.refused_full >= 1u && st.scaled >= (uint64_t)NK_TEX_SCALE_QUEUE + 2u, "jobs: stats scaled=%llu full=%llu",
          (unsigned long long)st.scaled, (unsigned long long)st.refused_full);
    nk_tex_scale_shutdown();
    if (!s_fail) printf("tex scale selftest: PASS jobs queued=%d held, full, ordered, sync\n", queued);
}

/* ---- settings, factor, budget ------------------------------------------------------------ */

static void test_settings(void) {
    NkTexScaleConfig c;
    char why[96];
    CHECK(nk_tex_scale_parse(NULL, NULL, NULL, &c, why, sizeof(why)) && c.factor == 0 && c.smart2d == 0 &&
          c.budget_bytes == ((uint64_t)NK_TEX_SCALE_DEFAULT_BUDGET_MB << 20), "settings: unset is not off");
    CHECK(nk_tex_scale_parse("off", "off", NULL, &c, why, sizeof(why)) && c.factor == 0, "settings: off");
    CHECK(nk_tex_scale_parse("4", "on", "256", &c, why, sizeof(why)) && c.factor == 4 && c.smart2d == 1 &&
          c.budget_bytes == (256ull << 20), "settings: 4, on, 256");
    static const char *const bad_scale[] = { "1", "7", "x2", "44", "4x", " 4", "" };
    for (size_t i = 0; i < sizeof(bad_scale) / sizeof(bad_scale[0]); i++) {
        if (!bad_scale[i][0]) continue;   /* empty is unset */
        CHECK(!nk_tex_scale_parse(bad_scale[i], NULL, NULL, &c, why, sizeof(why)) && c.factor == 0,
              "settings: SR_TEX_SCALE=%s accepted", bad_scale[i]);
    }
    CHECK(!nk_tex_scale_parse("2", "yes", NULL, &c, why, sizeof(why)) && c.factor == 0 && c.smart2d == 0,
          "settings: SR_SMART_2D=yes accepted, or a refusal left a feature on");
    CHECK(!nk_tex_scale_parse("2", NULL, "0", &c, why, sizeof(why)) &&
          !nk_tex_scale_parse("2", NULL, "9000", &c, why, sizeof(why)) &&
          !nk_tex_scale_parse("2", NULL, "-5", &c, why, sizeof(why)), "settings: a bad budget accepted");
    /* authentic: nothing is read */
    const NkTexScaleConfig *a = nk_tex_scale_init_from_env(0);
    CHECK(a->factor == 0 && a->smart2d == 0, "settings: not enhanced, yet a feature is on");
    /* an item on through the environment is never silent about being outside 11.2 admission */
    {
        char line[192];
        NkTexScaleConfig on = { 0, 0, 1u << 20 };
        CHECK(nk_tex_scale_admission_line(&on, line, sizeof(line)) == 0 && line[0] == '\0',
              "settings: admission line printed with both features off");
        static const int cases[][2] = { { 4, 0 }, { 0, 1 }, { 2, 1 } };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            on.factor = cases[i][0]; on.smart2d = cases[i][1];
            const size_t n = nk_tex_scale_admission_line(&on, line, sizeof(line));
            CHECK(n > 0 && n == strlen(line) && strstr(line, "TEX_SCALE: guest-visible") == line &&
                  strstr(line, "SR_TEX_SCALE") && strstr(line, "not admitted") && strstr(line, "11.3.1"),
                  "settings: admission line missing for xbrz=%d smart2d=%d", on.factor, on.smart2d);
        }
    }

    CHECK(nk_tex_scale_factor_for(4, 256, 256) == 4 && nk_tex_scale_factor_for(6, 512, 512) == 4 &&
          nk_tex_scale_factor_for(4, 1024, 16) == 2 && nk_tex_scale_factor_for(2, 2048, 4) == 0 &&
          nk_tex_scale_factor_for(0, 16, 16) == 0 && nk_tex_scale_factor_for(5, 1, 1) == 5,
          "settings: factor_for");

    c.factor = 2; c.smart2d = 0; c.budget_bytes = 1u << 20;
    nk_tex_scale_configure(&c);
    CHECK(nk_tex_scale_budget_take(600u << 10) && !nk_tex_scale_budget_take(600u << 10) &&
          nk_tex_scale_budget_used() == (600u << 10), "budget: over-commit or wrong count");
    nk_tex_scale_budget_give(600u << 10);
    CHECK(nk_tex_scale_budget_used() == 0 && nk_tex_scale_budget_take(1u << 20), "budget: give");
    nk_tex_scale_budget_give(1u << 20);
    nk_tex_scale_configure(NULL);
    if (!s_fail) printf("tex scale selftest: PASS settings parse, authentic off, factor-for, budget\n");
}

/* ---- smart 2D classifier ----------------------------------------------------------------- */

static int tri(float x0, float y0, float u0, float v0, float x1, float y1, float u1, float v1,
               float x2, float y2, float u2, float v2, float rw0, float rw1, float rw2) {
    const float x[3] = { x0, x1, x2 }, y[3] = { y0, y1, y2 };
    const float rw[3] = { rw0, rw1, rw2 };
    const float u[3] = { u0 * rw0, u1 * rw1, u2 * rw2 }, v[3] = { v0 * rw0, v1 * rw1, v2 * rw2 };
    return nk_smart2d_triangle(x, y, u, v, rw);
}

static void test_smart2d(void) {
    /* sprites */
    CHECK(nk_smart2d_sprite(10, 10, 26, 26, 0, 0, 16, 16), "smart2d: 1:1 sprite");
    CHECK(nk_smart2d_sprite(10, 10, 26, 30, 32, 8, 48, 28), "smart2d: 1:1 sprite at a texel offset");
    CHECK(nk_smart2d_sprite(10, 10, 26, 26, 16, 0, 0, 16), "smart2d: 1:1 sprite mirrored in u");
    CHECK(!nk_smart2d_sprite(10, 10, 42, 42, 0, 0, 16, 16), "smart2d: a 2x magnified sprite is not 1:1");
    CHECK(!nk_smart2d_sprite(10, 10, 18, 18, 0, 0, 16, 16), "smart2d: a minified sprite is not 1:1");
    CHECK(!nk_smart2d_sprite(10, 10, 26, 26, 0.5f, 0, 16.5f, 16), "smart2d: a half-texel offset is not pixel-mapped");
    CHECK(!nk_smart2d_sprite(10, 10, 10, 26, 0, 0, 0, 16), "smart2d: an empty sprite");
    /* triangles: a through-mode quad's halves */
    CHECK(tri(10, 10, 0, 0, 26, 10, 16, 0, 10, 26, 0, 16, 1, 1, 1) &&
          tri(26, 10, 16, 0, 26, 26, 16, 16, 10, 26, 0, 16, 1, 1, 1), "smart2d: 1:1 triangle pair");
    CHECK(tri(10, 10, 16, 16, 26, 10, 0, 16, 10, 26, 16, 0, 1, 1, 1), "smart2d: 1:1 triangle flipped on both axes");
    CHECK(tri(10, 10, 0, 0, 26, 10, 16, 0, 10, 26, 0, 16, 0.5f, 0.5f, 0.5f), "smart2d: 1:1 triangle, equal 1/w");
    /* 3D: perspective, rotation, scale */
    CHECK(!tri(10, 10, 0, 0, 26, 10, 16, 0, 10, 26, 0, 16, 1.0f, 0.5f, 0.25f), "smart2d: a perspective triangle");
    CHECK(!tri(10, 10, 0, 0, 26, 10, 0, 16, 10, 26, 16, 0, 1, 1, 1), "smart2d: a rotated triangle (u along y)");
    CHECK(!tri(10, 10, 0, 0, 42, 10, 16, 0, 10, 42, 0, 16, 1, 1, 1), "smart2d: a magnified triangle");
    CHECK(!tri(100, 40, 3.2f, 7.1f, 180, 60, 40.9f, 2.0f, 120, 150, 11.4f, 63.0f, 0.02f, 0.015f, 0.03f),
          "smart2d: a 3D triangle");
    CHECK(!tri(10, 10, 0.5f, 0, 26, 10, 16.5f, 0, 10, 26, 0.5f, 16, 1, 1, 1), "smart2d: a half-texel triangle");
    CHECK(!tri(10, 10, 0, 0, 26, 10, 16, 0, 26, 10, 16, 0, 1, 1, 1), "smart2d: a degenerate triangle");
    if (!s_fail) printf("tex scale selftest: PASS smart2d sprites and triangles\n");
}

int main(void) {
    test_golden();
    test_wrap_clamp();
    test_jobs();
    test_settings();
    test_smart2d();
    if (s_fail) return 1;
    puts("tex scale selftest: OK");
    return 0;
}
