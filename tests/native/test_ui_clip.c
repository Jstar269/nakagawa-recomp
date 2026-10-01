/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Regression for the blank library window: a scoped clip around the hero art
 * must leave the renderer exactly as it found it. Runs on SDL's software
 * renderer over a memory surface, so it needs no window or GPU. */

#include "ui_clip.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                               \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);    \
            g_failures++;                                              \
        }                                                              \
    } while (0)

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

int main(void) {
    SDL_Surface *surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA8888);
    SDL_Renderer *ren = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
    if (!ren) {
        fprintf(stderr, "FAIL: software renderer unavailable: %s\n", SDL_GetError());
        return 1;
    }

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
    if (g_failures) {
        fprintf(stderr, "test_ui_clip: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ui_clip: all checks passed\n");
    return 0;
}
