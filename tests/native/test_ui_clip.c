/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Scoped renderer clipping and UTF-8 string clipping/truncation tests for player UI.
 * Runs on SDL's software renderer if available, or headless fallback. */

#include "ui_clip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                               \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);    \
            g_failures++;                                              \
        }                                                              \
    } while (0)

static bool is_valid_utf8(const char *str) {
    if (!str) return true;
    const unsigned char *bytes = (const unsigned char *)str;
    while (*bytes) {
        if (bytes[0] <= 0x7F) {
            bytes++;
        } else if ((bytes[0] & 0xE0) == 0xC0) {
            if ((bytes[1] & 0xC0) != 0x80) return false;
            bytes += 2;
        } else if ((bytes[0] & 0xF0) == 0xE0) {
            if ((bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80) return false;
            bytes += 3;
        } else if ((bytes[0] & 0xF8) == 0xF0) {
            if ((bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 || (bytes[3] & 0xC0) != 0x80) return false;
            bytes += 4;
        } else {
            return false;
        }
    }
    return true;
}

#if defined(__has_include) && __has_include(<SDL3/SDL.h>)
/* Paint the whole target white and report whether the given pixel took it. */
static bool pixel_is_white_after_full_fill(SDL_Renderer *ren, SDL_Surface *surface, int x, int y) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_FRect all = { 0.0f, 0.0f, (float)surface->w, (float)surface->h };
    SDL_RenderFillRect(ren, &all);
    SDL_FlushRenderer(ren);
    Uint8 r = 0, g = 0, b = 0, a = 0;
    if (!SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a)) return false;
    return r == 255 && g == 255 && b == 255;
}
#endif

int main(void) {
#if defined(__has_include) && __has_include(<SDL3/SDL.h>)
    SDL_Surface *surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA8888);
    SDL_Renderer *ren = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
    if (ren) {
        /* 1. No clip before the scope: none after it, and the next frame draws
              everywhere (the blank-window case). */
        {
            UiClipState saved = ui_clip_save(ren);
            SDL_Rect hero = { 10, 10, 20, 20 };
            SDL_SetRenderClipRect(ren, &hero);
            ui_clip_restore(ren, &saved);
            CHECK(!SDL_RenderClipEnabled(ren), "no clip is left enabled after restoring an unclipped state");
            CHECK(pixel_is_white_after_full_fill(ren, surface, 1, 1),
                  "a full-target fill after the scope reaches a pixel outside the hero rectangle");
            CHECK(pixel_is_white_after_full_fill(ren, surface, 15, 15),
                  "a full-target fill after the scope reaches a pixel inside the hero rectangle");
        }

        /* 2. An outer clip survives a nested scope unchanged. */
        {
            SDL_Rect outer = { 2, 2, 40, 40 };
            SDL_SetRenderClipRect(ren, &outer);
            UiClipState saved = ui_clip_save(ren);
            SDL_Rect inner = { 5, 5, 3, 3 };
            SDL_SetRenderClipRect(ren, &inner);
            ui_clip_restore(ren, &saved);
            SDL_Rect now = { 0, 0, 0, 0 };
            CHECK(SDL_RenderClipEnabled(ren), "the outer clip is still enabled after the nested scope");
            CHECK(SDL_GetRenderClipRect(ren, &now) && now.x == 2 && now.y == 2 && now.w == 40 && now.h == 40,
                  "the outer clip rectangle is restored exactly");
            CHECK(pixel_is_white_after_full_fill(ren, surface, 30, 30), "the outer clip still admits its own area");
            CHECK(!pixel_is_white_after_full_fill(ren, surface, 50, 50), "the outer clip still excludes outside pixels");
            SDL_SetRenderClipRect(ren, NULL);
        }

        SDL_DestroyRenderer(ren);
        SDL_DestroySurface(surface);
    }
#endif

    /* 3. String clipping tests: normal text preservation */
    {
        char out[128];
        const char *normal = "Short Title";
        ui_clip_ellipsize_utf8(out, sizeof(out), normal, strlen(normal));
        CHECK(strcmp(out, normal) == 0, "normal text within bounds is preserved unchanged");
        CHECK(is_valid_utf8(out), "normal text output is valid UTF-8");
    }

    /* 4. String clipping tests: extreme length title with multibyte UTF-8 (3-byte Japanese hiragana) */
    {
        /* Create a synthetic string of 500 Japanese characters "あ" (1500 bytes + null) */
        char long_title[1600];
        size_t idx = 0;
        for (int i = 0; i < 500; i++) {
            long_title[idx++] = (char)0xE3;
            long_title[idx++] = (char)0x81;
            long_title[idx++] = (char)0x82; /* あ */
        }
        long_title[idx] = '\0';

        char out[128]; /* NK_MAX_TITLE_LEN = 128 */
        /* Request clip at prefix_bytes = 300, which exceeds buffer size 128 */
        ui_clip_ellipsize_utf8(out, sizeof(out), long_title, 300);

        CHECK(strlen(out) < sizeof(out), "bounded write does not overflow output buffer");
        CHECK(is_valid_utf8(out), "extreme length multibyte UTF-8 string produces valid UTF-8");
        CHECK(strstr(out, "...") != NULL, "ellipsized string ends with ...");

        /* Test cut at exact continuation byte offset: 124 is not a multiple of 3, so index 124 is a continuation byte */
        ui_clip_ellipsize_utf8(out, sizeof(out), long_title, 124);
        CHECK(strlen(out) < sizeof(out), "bounded write at 124 byte limit");
        CHECK(is_valid_utf8(out), "truncating at continuation byte offset yields valid UTF-8");
    }

    /* 5. String clipping tests: 4-byte UTF-8 characters (emoji) and narrow clipping widths */
    {
        /* Synthetic string of 100 emoji 😀 (0xF0 0x9F 0x98 0x80) = 400 bytes */
        char emoji_str[500];
        size_t idx = 0;
        for (int i = 0; i < 100; i++) {
            emoji_str[idx++] = (char)0xF0;
            emoji_str[idx++] = (char)0x9F;
            emoji_str[idx++] = (char)0x98;
            emoji_str[idx++] = (char)0x80;
        }
        emoji_str[idx] = '\0';

        char out[64];
        /* Narrow clipping widths: 0..10 bytes */
        for (size_t narrow = 0; narrow <= 10; narrow++) {
            ui_clip_ellipsize_utf8(out, sizeof(out), emoji_str, narrow);
            CHECK(strlen(out) < sizeof(out), "narrow width bounded write");
            CHECK(is_valid_utf8(out), "narrow clipping width on 4-byte UTF-8 produces valid UTF-8");
        }
    }

    if (g_failures) {
        fprintf(stderr, "test_ui_clip: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ui_clip: all checks passed\n");
    return 0;
}
