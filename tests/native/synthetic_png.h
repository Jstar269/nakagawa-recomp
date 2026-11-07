/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * synthetic_png.h - a tiny PNG writer for tests that need a replacement-pack file (#1880): RGBA8, no
 * compression (stored deflate blocks), so it needs no zlib. Every image a test writes is synthetic.
 * The includer supplies nk_fopen_utf8 (nk_platform.h). Static functions, one translation unit each.
 */
#ifndef NK_TESTS_SYNTHETIC_PNG_H
#define NK_TESTS_SYNTHETIC_PNG_H

#include "nk_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t synpng_crc_table[256];
static void synpng_crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        synpng_crc_table[n] = c;
    }
}
static uint32_t synpng_crc32(uint32_t crc, const uint8_t *d, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = synpng_crc_table[(crc ^ d[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}
static void synpng_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void synpng_chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t head[8];
    synpng_put32(head, (uint32_t)n);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = synpng_crc32(0, (const uint8_t *)type, 4);
    crc = synpng_crc32(crc, data, n);
    uint8_t tail[4];
    synpng_put32(tail, crc);
    fwrite(tail, 1, 4, f);
}

/* An RGBA8 PNG with stored (uncompressed) deflate blocks. `rgba` is w*h pixels, R in the low byte.
 * Returns 1 on success, 0 when the file cannot be written. */
static int synpng_write(const char *path, int w, int h, const uint32_t *rgba) {
    static int inited;
    if (!inited) { synpng_crc_init(); inited = 1; }
    const size_t row = 1u + (size_t)w * 4u;
    const size_t raw_n = row * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    const size_t blocks = (raw_n + 65534u) / 65535u;
    uint8_t *z = (uint8_t *)malloc(2 + raw_n + blocks * 5 + 4);
    if (!raw || !z) { free(raw); free(z); return 0; }
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * row] = 0;   /* filter: none */
        for (int x = 0; x < w; x++)
            memcpy(raw + (size_t)y * row + 1u + (size_t)x * 4u, &rgba[(size_t)y * w + x], 4);
    }
    size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    for (size_t off = 0; off < raw_n;) {
        const size_t n = raw_n - off > 65535u ? 65535u : raw_n - off;
        z[zn++] = (off + n >= raw_n) ? 1 : 0;
        z[zn++] = (uint8_t)(n & 0xff); z[zn++] = (uint8_t)(n >> 8);
        z[zn++] = (uint8_t)(~n & 0xff); z[zn++] = (uint8_t)((~n >> 8) & 0xff);
        memcpy(z + zn, raw + off, n);
        zn += n; off += n;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    synpng_put32(z + zn, (b << 16) | a);
    zn += 4;
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    synpng_put32(ihdr, (uint32_t)w); synpng_put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    synpng_chunk(f, "IHDR", ihdr, 13);
    synpng_chunk(f, "IDAT", z, zn);
    synpng_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}

#endif
