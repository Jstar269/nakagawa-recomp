/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * xbrz_golden.cpp - prints the goldens src/rt/gpu_sdl3vk/tex_scale_selftest.c compares the C
 * translation (xbrz.c) with. It is built against the PRISTINE upstream xBRZ 1.9 source, never
 * against this directory, so a golden is upstream's own result:
 *
 *   g++ -std=c++23 -O2 -ffp-contract=off -I<xBRZ_1.9> third_party/xbrz/xbrz_golden.cpp \
 *       <xBRZ_1.9>/xbrz.cpp -o xbrz_golden && ./xbrz_golden
 *
 * <xBRZ_1.9> is the unpacked release archive named in PROVENANCE.md. The patterns below must
 * stay identical to tex_scale_selftest.c's xbrz_pattern().
 */
#include <xbrz.h> /* upstream's header, through -I; never this directory's */

#include <cstdint>
#include <cstdio>
#include <vector>

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

/* Pattern 0: shapes on an opaque field (lines of several slopes, a disc, a checkerboard, lone
 * pixels, a translucent band, a transparent hole). Pattern 1: noise over a small palette with
 * alphas 0, 64 and 255, so equal and near-equal colours meet in every arrangement. */
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

int main()
{
    static const int dims[2][2] = { { 40, 28 }, { 23, 17 } };
    for (int which = 0; which < 2; which++) {
        const int w = dims[which][0], h = dims[which][1];
        std::vector<uint32_t> src((size_t)w * h);
        xbrz_pattern(which, src.data(), w, h);
        for (int f = 2; f <= 6; f++) {
            std::vector<uint32_t> trg((size_t)w * h * f * f);
            xbrz::scale(f, src.data(), trg.data(), w, h, xbrz::ColorFormat::argbUnbuffered);
            std::printf("    { %d, %d, 0x%016llxull },\n", which, f,
                        (unsigned long long)fnv64(trg.data(), trg.size()));
        }
    }
    /* The 2x result of the top-left 6x4 corner of pattern 0, in full. */
    {
        uint32_t src[6 * 4];
        std::vector<uint32_t> full(40 * 28);
        xbrz_pattern(0, full.data(), 40, 28);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 6; x++) src[y * 6 + x] = full[(size_t)y * 40 + x];
        uint32_t trg[12 * 8];
        xbrz::scale(2, src, trg, 6, 4, xbrz::ColorFormat::argbUnbuffered);
        for (int i = 0; i < 12 * 8; i++)
            std::printf("0x%08Xu,%s", trg[i], (i % 6) == 5 ? "\n" : " ");
    }
    return 0;
}
