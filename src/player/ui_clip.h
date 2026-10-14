/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Scoped renderer clipping and bounded UTF-8 string truncation for the player UI.
 *
 * SDL_GetRenderClipRect() succeeds -- and hands back an empty rectangle --
 * when clipping is off, so its return value does not say whether a clip was
 * set. Treating it that way "restored" a zero-size clip after the library
 * hero art, which hid everything drawn afterwards and, because renderer clip
 * state persists across frames, left every later frame blank.
 * SDL_RenderClipEnabled() is the question to ask. */

#ifndef NAKAGAWA_UI_CLIP_H
#define NAKAGAWA_UI_CLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(__has_include) && __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#else
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Rect {
    int x, y;
    int w, h;
} SDL_Rect;
static inline bool SDL_RenderClipEnabled(SDL_Renderer *ren) { (void)ren; return false; }
static inline bool SDL_GetRenderClipRect(SDL_Renderer *ren, SDL_Rect *rect) {
    (void)ren;
    if (rect) { rect->x = rect->y = rect->w = rect->h = 0; }
    return false;
}
static inline bool SDL_SetRenderClipRect(SDL_Renderer *ren, const SDL_Rect *rect) { (void)ren; (void)rect; return false; }
#endif

typedef struct {
    bool enabled;
    SDL_Rect rect;
} UiClipState;

static inline UiClipState ui_clip_save(SDL_Renderer *ren) {
    UiClipState state = { false, { 0, 0, 0, 0 } };
    state.enabled = SDL_RenderClipEnabled(ren);
    if (state.enabled) SDL_GetRenderClipRect(ren, &state.rect);
    return state;
}

static inline void ui_clip_restore(SDL_Renderer *ren, const UiClipState *state) {
    SDL_SetRenderClipRect(ren, state->enabled ? &state->rect : NULL);
}

/* Ensure a byte count limit falls on a valid UTF-8 character boundary by backing
 * up over any continuation bytes (0x80..0xBF). Returns the adjusted limit. */
static inline size_t ui_clip_utf8_bound(const char *str, size_t limit) {
    if (!str) return 0;
    size_t len = strlen(str);
    if (limit > len) limit = len;
    while (limit > 0 && ((unsigned char)str[limit] & 0xC0) == 0x80) {
        limit--;
    }
    return limit;
}

/* Format an ellipsized copy of str into out[out_size], truncating at prefix_bytes
 * while guaranteeing valid UTF-8 boundaries and bounded writes.
 * If str fits or prefix_bytes >= strlen(str), out receives str unchanged (up to out_size-1). */
static inline void ui_clip_ellipsize_utf8(char *out, size_t out_size, const char *str, size_t prefix_bytes) {
    if (!out || out_size == 0) return;
    if (!str) {
        out[0] = '\0';
        return;
    }
    size_t str_len = strlen(str);
    if (prefix_bytes >= str_len) {
        prefix_bytes = str_len;
        if (prefix_bytes >= out_size) prefix_bytes = out_size - 1;
        prefix_bytes = ui_clip_utf8_bound(str, prefix_bytes);
        memcpy(out, str, prefix_bytes);
        out[prefix_bytes] = '\0';
        return;
    }
    if (out_size <= 4) {
        size_t fit = ui_clip_utf8_bound(str, out_size - 1);
        memcpy(out, str, fit);
        out[fit] = '\0';
        return;
    }
    size_t max_prefix = out_size - 4;
    if (prefix_bytes > max_prefix) prefix_bytes = max_prefix;
    prefix_bytes = ui_clip_utf8_bound(str, prefix_bytes);
    memcpy(out, str, prefix_bytes);
    memcpy(out + prefix_bytes, "...", 3);
    out[prefix_bytes + 3] = '\0';
}

#endif /* NAKAGAWA_UI_CLIP_H */
