// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Round-trip harness for the deterministic PGF writer (issue #313).
 *
 * Opens one generated font through the public reader ABI and prints every guest
 * record and every drawn sample, so tools/test_pgf_writer.py can compare the
 * reader's answers bit-exactly with the glyph set the writer was given.  This
 * harness reads only the generated file named on its command line; it never
 * reads a font, a title, or any private input. */

#define _POSIX_C_SOURCE 200809L

#include "pgf_api.h"
#include "recomp.h"
#include "ge_shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELPER_GUEST_BASE 0x08010000u
#define HELPER_INFO_ADDR (HELPER_GUEST_BASE + 0x100u)
#define HELPER_IMAGE_ADDR (HELPER_GUEST_BASE + 0x200u)
#define HELPER_BUFFER_ADDR (HELPER_GUEST_BASE + 0x1000u)
#define HELPER_BUFFER_CAP 32768u
#define HELPER_GUEST_SIZE 0x00100000u
#define HELPER_MAX_PROBES 64
#define HELPER_FONT_INFO_SIZE 0x108u
#define HELPER_CHAR_INFO_SIZE 0x3cu
#define HELPER_GLYPH_IMAGE_SIZE 0x18u

uint8_t *g_mem;

void sr_gpu_vram_dirty(uint32_t address, uint32_t bytes) {
    (void)address;
    (void)bytes;
}

static uint32_t helper_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void helper_put_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void helper_put_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void helper_print_hex(const uint8_t *bytes, size_t size) {
    size_t i;
    for (i = 0; i < size; ++i) printf("%02x", bytes[i]);
}

static uint8_t *helper_guest(uint32_t address) {
    return SR_HOST(address);
}

static void helper_set_image(uint32_t format, uint32_t width, uint32_t height,
                             uint32_t bytes_per_line, uint32_t buffer) {
    uint8_t *image = helper_guest(HELPER_IMAGE_ADDR);
    memset(image, 0, HELPER_GLYPH_IMAGE_SIZE);
    helper_put_u32(image + 0x00u, format);
    helper_put_u32(image + 0x04u, 0);
    helper_put_u32(image + 0x08u, 0);
    helper_put_u16(image + 0x0cu, (uint16_t)width);
    helper_put_u16(image + 0x0eu, (uint16_t)height);
    helper_put_u16(image + 0x10u, (uint16_t)bytes_per_line);
    helper_put_u32(image + 0x14u, buffer);
}

static void helper_probe(const PGF *pgf, int code) {
    uint8_t *info = helper_guest(HELPER_INFO_ADDR);
    uint8_t *buffer;
    uint32_t width;
    uint32_t height;
    size_t pixels;
    int resolved;
    int drawn;

    memset(info, 0, HELPER_CHAR_INFO_SIZE);
    resolved = pgf_get_char_info(pgf, code, 0, HELPER_INFO_ADDR);
    printf("char %d %d ", code, pgf_has_char(pgf, code));
    helper_print_hex(info, HELPER_CHAR_INFO_SIZE);
    printf("\n");
    if (!resolved) {
        printf("draw %d none\n", code);
        return;
    }
    width = helper_u32(info + 0x00u);
    height = helper_u32(info + 0x04u);
    printf("draw %d %u %u ", code, width, height);
    /* Both dimensions are bounded to 127 first, then the product is formed in
     * size_t so it cannot wrap before it is compared with the buffer cap. */
    pixels = 0u;
    if (width != 0u && height != 0u && width <= 127u && height <= 127u)
        pixels = (size_t)width * (size_t)height;
    if (pixels == 0u || pixels > HELPER_BUFFER_CAP) {
        printf("skipped\n");
        return;
    }
    buffer = helper_guest(HELPER_BUFFER_ADDR);
    memset(buffer, 0xff, pixels);
    helper_set_image(2u, width, height, width, HELPER_BUFFER_ADDR);
    drawn = pgf_draw_glyph(pgf, code, 0, HELPER_IMAGE_ADDR);
    if (!drawn) {
        printf("refused\n");
        return;
    }
    helper_print_hex(buffer, pixels);
    printf("\n");
}

int main(int argc, char **argv) {
    PGF *pgf;
    int probe_count;
    int i;

    if (argc < 3 || argc > HELPER_MAX_PROBES + 2) {
        fprintf(stderr, "usage: %s FONT.pgf CODE [CODE...]\n", argv[0]);
        return 2;
    }
    probe_count = argc - 2;
    g_mem = (uint8_t *)calloc(1u, HELPER_GUEST_SIZE);
    if (!g_mem) {
        fprintf(stderr, "unable to allocate synthetic guest memory\n");
        return 2;
    }
    pgf = pgf_open(argv[1]);
    if (!pgf) {
        printf("open refused\n");
        free(g_mem);
        return 1;
    }
    printf("open ok\n");
    pgf_get_font_info(pgf, HELPER_INFO_ADDR);
    printf("font ");
    helper_print_hex(helper_guest(HELPER_INFO_ADDR), HELPER_FONT_INFO_SIZE);
    printf("\n");
    for (i = 0; i < probe_count; ++i) {
        helper_probe(pgf, atoi(argv[2 + i]));
    }
    pgf_close(pgf);
    free(g_mem);
    return 0;
}
