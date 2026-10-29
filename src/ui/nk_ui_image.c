/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* PNG decoding for nk_ui images: the project's bundled prompt glyphs and, in
   the player, ICON0.PNG / PIC1.PNG read at run time from the user's own disc.
   Decoding is stb_image (third_party/stb, used under its public-domain
   option), built for PNG only from memory. The limits are checked against the
   PNG header before any pixel is decoded, so a hostile or damaged file costs
   at most the caller's bounds. */

#include "nk_ui_image.h"

#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
/* STB_IMAGE_STATIC declares stb_image's unused entry points static; GCC reports
   them at the end of the file, so this one stays off for the whole file. */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wpedantic"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif
#include "../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static uint64_t g_nk_ui_image_serial;

static uint64_t nk_ui_image_next_serial(void) {
#if defined(_MSC_VER)
    return (uint64_t)_InterlockedIncrement64((volatile __int64 *)&g_nk_ui_image_serial);
#else
    return __atomic_add_fetch(&g_nk_ui_image_serial, 1u, __ATOMIC_RELAXED);
#endif
}

uint64_t nk_ui_image_new_serial(void) {
    return nk_ui_image_next_serial();
}

static const uint8_t g_png_signature[8] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};

const char *nk_ui_image_status_name(NkUiImageStatus status) {
    switch (status) {
    case NK_UI_IMAGE_OK: return "OK";
    case NK_UI_IMAGE_EMPTY: return "EMPTY";
    case NK_UI_IMAGE_NOT_PNG: return "NOT_PNG";
    case NK_UI_IMAGE_TOO_LARGE: return "TOO_LARGE";
    case NK_UI_IMAGE_DECODE_FAILED: return "DECODE_FAILED";
    default: return "UNKNOWN";
    }
}

NkUiImageStatus nk_ui_image_decode_png(const uint8_t *bytes, size_t size,
                                       const NkUiImageLimits *limits, NkUiImage *out) {
    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char *pixels;
    size_t count;
    size_t i;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (bytes == NULL || size == 0u || out == NULL || limits == NULL) {
        return NK_UI_IMAGE_EMPTY;
    }
    if (size < sizeof(g_png_signature) ||
        memcmp(bytes, g_png_signature, sizeof(g_png_signature)) != 0) {
        return NK_UI_IMAGE_NOT_PNG;
    }
    /* stb_image takes an int length. */
    if (size > limits->max_bytes || size > (size_t)0x7FFFFFFF) {
        return NK_UI_IMAGE_TOO_LARGE;
    }
    /* The first chunk of a PNG is IHDR: width and height, big-endian, at bytes
       16 and 20. Refuse oversized dimensions from them before stb_image reads
       anything else. */
    if (size >= 24u && memcmp(bytes + 12, "IHDR", 4u) == 0) {
        uint32_t header_w = ((uint32_t)bytes[16] << 24) | ((uint32_t)bytes[17] << 16) |
                            ((uint32_t)bytes[18] << 8) | (uint32_t)bytes[19];
        uint32_t header_h = ((uint32_t)bytes[20] << 24) | ((uint32_t)bytes[21] << 16) |
                            ((uint32_t)bytes[22] << 8) | (uint32_t)bytes[23];
        if (header_w > limits->max_width || header_h > limits->max_height) {
            return NK_UI_IMAGE_TOO_LARGE;
        }
    }
    if (!stbi_info_from_memory(bytes, (int)size, &w, &h, &channels) || w <= 0 || h <= 0) {
        return NK_UI_IMAGE_DECODE_FAILED;
    }
    if ((uint32_t)w > limits->max_width || (uint32_t)h > limits->max_height) {
        return NK_UI_IMAGE_TOO_LARGE;
    }
    pixels = stbi_load_from_memory(bytes, (int)size, &w, &h, &channels, 4);
    if (pixels == NULL) {
        return NK_UI_IMAGE_DECODE_FAILED;
    }
    /* The header was read once already; the decode must agree with it. */
    if (w <= 0 || h <= 0 || (uint32_t)w > limits->max_width ||
        (uint32_t)h > limits->max_height) {
        stbi_image_free(pixels);
        return NK_UI_IMAGE_DECODE_FAILED;
    }
    /* w and h are at most STBI_MAX_DIMENSIONS, so the count fits a size_t. */
    count = (size_t)w * (size_t)h;
    for (i = 0; i < count; ++i) {
        unsigned char *p = pixels + i * 4u;
        unsigned a = p[3];
        if (a != 255u) {
            p[0] = (unsigned char)((p[0] * a + 127u) / 255u);
            p[1] = (unsigned char)((p[1] * a + 127u) / 255u);
            p[2] = (unsigned char)((p[2] * a + 127u) / 255u);
        }
    }
    out->width = (uint32_t)w;
    out->height = (uint32_t)h;
    out->rgba_pm = pixels;
    out->serial = nk_ui_image_next_serial();
    return NK_UI_IMAGE_OK;
}

bool nk_ui_image_resample(const NkUiImage *src, uint32_t w, uint32_t h, uint8_t *dst) {
    uint32_t x;
    uint32_t y;
    if (src == NULL || src->rgba_pm == NULL || src->width == 0u || src->height == 0u ||
        w == 0u || h == 0u || dst == NULL) {
        return false;
    }
    for (y = 0; y < h; ++y) {
        /* Source rows [y0, y1) map onto destination row y; at least one. */
        uint32_t y0 = (uint32_t)((uint64_t)y * src->height / h);
        uint32_t y1 = (uint32_t)((uint64_t)(y + 1u) * src->height / h);
        if (y1 <= y0) y1 = y0 + 1u;
        for (x = 0; x < w; ++x) {
            uint32_t x0 = (uint32_t)((uint64_t)x * src->width / w);
            uint32_t x1 = (uint32_t)((uint64_t)(x + 1u) * src->width / w);
            uint32_t sum[4] = {0u, 0u, 0u, 0u};
            uint32_t count;
            uint32_t sx;
            uint32_t sy;
            uint8_t *out = dst + ((size_t)y * w + x) * 4u;
            if (x1 <= x0) x1 = x0 + 1u;
            for (sy = y0; sy < y1; ++sy) {
                const uint8_t *row = src->rgba_pm + (size_t)sy * src->width * 4u;
                for (sx = x0; sx < x1; ++sx) {
                    sum[0] += row[sx * 4u];
                    sum[1] += row[sx * 4u + 1u];
                    sum[2] += row[sx * 4u + 2u];
                    sum[3] += row[sx * 4u + 3u];
                }
            }
            count = (y1 - y0) * (x1 - x0);
            out[0] = (uint8_t)((sum[0] + count / 2u) / count);
            out[1] = (uint8_t)((sum[1] + count / 2u) / count);
            out[2] = (uint8_t)((sum[2] + count / 2u) / count);
            out[3] = (uint8_t)((sum[3] + count / 2u) / count);
        }
    }
    return true;
}

void nk_ui_image_free(NkUiImage *image) {
    if (image == NULL) {
        return;
    }
    if (image->rgba_pm != NULL) {
        stbi_image_free(image->rgba_pm);
    }
    memset(image, 0, sizeof(*image));
}
