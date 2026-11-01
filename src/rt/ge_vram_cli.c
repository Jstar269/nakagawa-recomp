// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-11-01.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* The standalone VRAM decoder (nk_cli's `ge-vram-decoder`): decodes one surface of a captured
 * 2 MiB VRAM image with the runtime's own texture decoder. nk_cli builds this file together
 * with ge.c under -DSR_GE_VRAM_DECODER_CLI, which redirects ge.c's guest reads to the image
 * (ge_vram_cli_read8, bounds-checked per byte). Split out of ge.c on 2026-11-01 (#1531). */

#include "recomp.h"
#include "ge_shared.h"
#include "ge_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_GE_VRAM_DECODER_CLI
#error "ge_vram_cli.c is only built into the standalone decoder (-DSR_GE_VRAM_DECODER_CLI)"
#endif

extern int ge_vram_cli_bad_read;

uint8_t *g_mem;
int g_ge_capture_active;
void ge_capture_note_memory(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }
void sr_oor(uint32_t addr, uint32_t value, int store) {
    (void)addr; (void)value; (void)store;
    ge_vram_cli_bad_read = 1;
}
uint32_t sr_get_ge_status(void) { return 0; }

static int ge_vram_cli_parse_u32(const char *text, uint32_t *out) {
    char *end = NULL;
    unsigned long value;
    if (!text || !text[0] || !out) return 0;
    value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value > UINT32_MAX) return 0;
    *out = (uint32_t)value;
    return 1;
}

static int ge_vram_cli_read_file(const char *path, uint8_t *out, size_t size) {
    FILE *file = fopen(path, "rb");
    int ok;
    if (!file) return 0;
    ok = fread(out, 1, size, file) == size && fgetc(file) == EOF;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

int main(int argc, char **argv) {
    const size_t arena_size = (size_t)GE_CAPTURE_VRAM_BASE + GE_CAPTURE_VRAM_SIZE;
    uint8_t *arena = NULL;
    uint32_t addr, width, height, stride, format, swizzled, clut_addr, clut_format;
    uint64_t pixel_count, bytes_per_pixel, byte_extent;
    uint32_t *pixels = NULL;
    uint8_t clut_ram[2048];
    const uint8_t *clut_file = NULL;
    FILE *output = NULL;
    int result = 1;

    if (argc != 12 ||
        !ge_vram_cli_parse_u32(argv[2], &addr) ||
        !ge_vram_cli_parse_u32(argv[3], &width) ||
        !ge_vram_cli_parse_u32(argv[4], &height) ||
        !ge_vram_cli_parse_u32(argv[5], &stride) ||
        !ge_vram_cli_parse_u32(argv[6], &format) ||
        !ge_vram_cli_parse_u32(argv[7], &swizzled) ||
        !ge_vram_cli_parse_u32(argv[8], &clut_addr) ||
        !ge_vram_cli_parse_u32(argv[9], &clut_format)) {
        fprintf(stderr, "usage: ge-vram-decoder IMAGE ADDR WIDTH HEIGHT STRIDE FORMAT SWIZZLED CLUT_ADDR CLUT_FORMAT CLUT_FILE RGBA_OUT\n");
        return 2;
    }
    if (!width || !height || width > 4096u || height > 4096u || stride < width ||
        swizzled > 1u || (format > 10u && format != 11u) ||
        (format >= 8u && format <= 10u && swizzled) ||
        (format >= 8u && format <= 10u && (stride & 3u)) ||
        (uint64_t)width * height > 4194304u ||
        addr < GE_CAPTURE_VRAM_BASE || addr >= GE_CAPTURE_VRAM_BASE + GE_CAPTURE_VRAM_SIZE ||
        (clut_addr && (clut_addr < GE_CAPTURE_VRAM_BASE ||
                       clut_addr >= GE_CAPTURE_VRAM_BASE + GE_CAPTURE_VRAM_SIZE)) ||
        clut_format > 0x00FFFFFFu) {
        fprintf(stderr, "VRAM decoder rejected out-of-range or unsupported surface metadata\n");
        return 2;
    }
    if (format <= 3u) {
        bytes_per_pixel = format == 3u ? 4u : 2u;
        byte_extent = ((uint64_t)height - 1u) * stride * bytes_per_pixel +
                      (uint64_t)width * bytes_per_pixel;
        if ((uint64_t)addr + byte_extent >
            (uint64_t)GE_CAPTURE_VRAM_BASE + GE_CAPTURE_VRAM_SIZE) {
            fprintf(stderr, "VRAM decoder surface crosses the captured VRAM boundary\n");
            return 2;
        }
    }

    arena = (uint8_t *)calloc(1, arena_size);
    if (!arena) { fprintf(stderr, "VRAM decoder could not allocate the guest address window\n"); goto done; }
    g_mem = arena + GE_CAPTURE_VRAM_BASE;
    if (!ge_vram_cli_read_file(argv[1], arena, GE_CAPTURE_VRAM_SIZE)) {
        fprintf(stderr, "VRAM decoder input must be exactly 2 MiB\n"); goto done;
    }
    if (strcmp(argv[10], "-") != 0) {
        if (!ge_vram_cli_read_file(argv[10], clut_ram, sizeof clut_ram)) {
            fprintf(stderr, "VRAM decoder CLUT input must be exactly 2048 bytes\n"); goto done;
        }
        clut_file = clut_ram;
    }

    pixel_count = (uint64_t)width * height;
    pixels = (uint32_t *)malloc((size_t)pixel_count * sizeof *pixels);
    if (!pixels) { fprintf(stderr, "VRAM decoder could not allocate the pixel buffer\n"); goto done; }
    ge_vram_cli_decode(addr, width, height, stride, format, swizzled, clut_addr, clut_format,
                       clut_file, arena, pixels);
    if (ge_vram_cli_bad_read) {
        fprintf(stderr, "VRAM decoder read outside the captured 2 MiB VRAM image\n"); goto done;
    }
    output = fopen(argv[11], "wb");
    if (!output) { fprintf(stderr, "VRAM decoder could not open its RGBA output\n"); goto done; }
    for (uint64_t i = 0; i < pixel_count; i++) {
        uint32_t pixel = pixels[i];
        uint8_t rgba[4] = {(uint8_t)pixel, (uint8_t)(pixel >> 8),
                           (uint8_t)(pixel >> 16), (uint8_t)(pixel >> 24)};
        if (fwrite(rgba, 1, sizeof rgba, output) != sizeof rgba) {
            fprintf(stderr, "VRAM decoder RGBA output write failed\n"); goto done;
        }
    }
    if (fclose(output) != 0) { output = NULL; fprintf(stderr, "VRAM decoder RGBA output close failed\n"); goto done; }
    output = NULL;
    result = 0;
done:
    if (output) fclose(output);
    free(pixels);
    free(arena);
    g_mem = NULL;
    return result;
}
