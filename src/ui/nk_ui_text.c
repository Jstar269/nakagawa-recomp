/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_text.h"

#include <string.h>

static size_t nk_ui_utf8_codepoints(const char *text, size_t length) {
    size_t count = 0;
    size_t i = 0;

    while (i < length) {
        unsigned char lead = (unsigned char)text[i];
        size_t width = 1;
        if ((lead & 0xE0u) == 0xC0u) {
            width = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            width = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            width = 4;
        }
        if (width > length - i) {
            width = 1;
        }
        i += width;
        ++count;
    }
    return count;
}

static NkUiTextSize nk_ui_text_fixed_measure(void *user, int font, int px,
                                            const char *text, size_t length) {
    NkUiTextSize measured = {0.0f, 0.0f, 0.0f};
    (void)user;
    (void)font;
    if (text == NULL || px <= 0) {
        return measured;
    }
    measured.width = (float)nk_ui_utf8_codepoints(text, length) * (float)px * 0.5f;
    measured.height = (float)px;
    measured.baseline = (float)px * 0.8f;
    return measured;
}

static int nk_ui_text_fixed_shape(void *user, int font, int px, const char *text,
                                  size_t length, NkUiGlyph *out, int max) {
    int count = 0;
    size_t i = 0;
    float advance;
    (void)user;
    (void)font;
    if (text == NULL || px <= 0 || max < 0) {
        return 0;
    }
    advance = (float)px * 0.5f;
    while (i < length && count < max) {
        unsigned char lead = (unsigned char)text[i];
        uint32_t codepoint = lead;
        size_t width = 1;
        if ((lead & 0xE0u) == 0xC0u && i + 1 < length) {
            codepoint = ((uint32_t)(lead & 0x1Fu) << 6) |
                        ((uint32_t)((unsigned char)text[i + 1] & 0x3Fu));
            width = 2;
        } else if ((lead & 0xF0u) == 0xE0u && i + 2 < length) {
            codepoint = ((uint32_t)(lead & 0x0Fu) << 12) |
                        ((uint32_t)((unsigned char)text[i + 1] & 0x3Fu) << 6) |
                        ((uint32_t)((unsigned char)text[i + 2] & 0x3Fu));
            width = 3;
        } else if ((lead & 0xF8u) == 0xF0u && i + 3 < length) {
            codepoint = ((uint32_t)(lead & 0x07u) << 18) |
                        ((uint32_t)((unsigned char)text[i + 1] & 0x3Fu) << 12) |
                        ((uint32_t)((unsigned char)text[i + 2] & 0x3Fu) << 6) |
                        ((uint32_t)((unsigned char)text[i + 3] & 0x3Fu));
            width = 4;
        }
        if (width > length - i) {
            width = 1;
            codepoint = lead;
        }
        out[count].glyph = codepoint;
        out[count].codepoint = codepoint;
        out[count].x = advance * (float)count;
        out[count].y = 0.0f;
        out[count].advance_x = advance;
        out[count].advance_y = 0.0f;
        ++count;
        i += width;
    }
    return count;
}

static bool nk_ui_text_fixed_raster(void *user, int font, int px, uint32_t glyph,
                                    NkUiGlyphBitmap *out) {
    (void)user;
    (void)font;
    (void)px;
    (void)glyph;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

NkUiTextBackend nk_ui_text_fixed(void) {
    NkUiTextBackend backend;
    backend.user = NULL;
    backend.measure = nk_ui_text_fixed_measure;
    backend.shape = nk_ui_text_fixed_shape;
    backend.raster = nk_ui_text_fixed_raster;
    return backend;
}
