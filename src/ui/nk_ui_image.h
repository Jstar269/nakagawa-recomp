/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_IMAGE_H
#define NK_UI_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A decoded RGBA8 image for nk_ui: premultiplied alpha, rows top to bottom,
   width * 4 bytes per row, owned by whoever decoded it. serial is unique per
   successful decode in this process, so a backend can key a texture by it and
   never confuse a freed image with a new one at the same address. */
typedef struct NkUiImage {
    uint32_t width;
    uint32_t height;
    uint8_t *rgba_pm;
    uint64_t serial;
} NkUiImage;

typedef enum {
    NK_UI_IMAGE_OK = 0,
    NK_UI_IMAGE_EMPTY,          /* NULL or zero-length input, or a NULL output */
    NK_UI_IMAGE_NOT_PNG,        /* the 8-byte PNG signature is missing */
    NK_UI_IMAGE_TOO_LARGE,      /* encoded bytes or dimensions over the caller's limits */
    NK_UI_IMAGE_DECODE_FAILED   /* malformed or unsupported PNG, or out of memory */
} NkUiImageStatus;

/* Upper bounds a caller passes in. Images over a bound are refused before any
   pixel is decoded; the PNG header is read first. */
typedef struct {
    size_t max_bytes;
    uint32_t max_width;
    uint32_t max_height;
} NkUiImageLimits;

/* Decodes one PNG from memory (stb_image, PNG only, no file access). On any
   status other than NK_UI_IMAGE_OK, *out is zeroed and owns nothing. The
   decoder keeps no state between calls, so separate threads may decode at
   once. */
NkUiImageStatus nk_ui_image_decode_png(const uint8_t *bytes, size_t size,
                                       const NkUiImageLimits *limits, NkUiImage *out);

/* A fresh serial for an image whose pixels the caller fills and owns (never passed to
   nk_ui_image_free). Take a new one whenever the pixels change, so a backend that keyed a
   texture by the old serial uploads the new pixels. */
uint64_t nk_ui_image_new_serial(void);

/* Frees the pixels and zeroes the image. NULL and an empty image are fine. */
void nk_ui_image_free(NkUiImage *image);

/* Area-averages `src` down (or nearest-samples up) to w x h premultiplied
   RGBA8 pixels in `dst` (w * h * 4 bytes). Averaging premultiplied pixels keeps
   edges clean, so a 64 px glyph drawn at 24 px stays crisp. Returns false for
   an empty image, a zero size or a NULL buffer. */
bool nk_ui_image_resample(const NkUiImage *src, uint32_t w, uint32_t h, uint8_t *dst);

/* A stable word for a status, for logs. */
const char *nk_ui_image_status_name(NkUiImageStatus status);

#endif /* NK_UI_IMAGE_H */
