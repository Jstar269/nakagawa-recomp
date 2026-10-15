/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Scoped renderer clipping for the player UI.
 *
 * SDL_GetRenderClipRect() succeeds -- and hands back an empty rectangle --
 * when clipping is off, so its return value does not say whether a clip was
 * set. Treating it that way "restored" a zero-size clip after the library
 * hero art, which hid everything drawn afterwards and, because renderer clip
 * state persists across frames, left every later frame blank.
 * SDL_RenderClipEnabled() is the question to ask. */

#ifndef NAKAGAWA_UI_CLIP_H
#define NAKAGAWA_UI_CLIP_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

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

/* UTF-8 truncation for player UI text. Everything below is pure C (no SDL calls)
 * and writes only inside the caller's buffer. A cut never lands inside a
 * multi-byte sequence: a byte in 0x80..0xBF is a continuation byte, so the cut
 * point is moved back until the byte at the cut is a character start. */

/* Largest k <= limit such that str[k] is not a continuation byte, so str[0..k)
 * ends on a character boundary. Requires limit <= strlen(str); the terminating
 * NUL is not a continuation byte, so limit == strlen(str) is returned unchanged. */
static inline size_t ui_clip_utf8_floor(const char *str, size_t limit) {
    while (limit > 0 && ((unsigned char)str[limit] & 0xC0) == 0x80) limit--;
    return limit;
}

/* Writes str[0..k) followed by "..." and a NUL into out[0..out_size), where k is
 * prefix_bytes clamped to strlen(str) and to the room left by the marker
 * (out_size - 4), then floored to a character boundary. The caller has already decided that str is
 * being cut, so the marker is always appended. Returns k, the bytes of str kept.
 * out_size 0 writes nothing. out_size 1..3 cannot hold the marker, so out becomes
 * "" and 0 is returned. */
static inline size_t ui_clip_ellipsize_utf8(char *out, size_t out_size, const char *str, size_t prefix_bytes) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!str || out_size < 4) return 0;
    size_t len = strlen(str);
    size_t keep = prefix_bytes < len ? prefix_bytes : len;
    if (keep > out_size - 4) keep = out_size - 4;
    keep = ui_clip_utf8_floor(str, keep);
    memcpy(out, str, keep);
    memcpy(out + keep, "...", 3);
    out[keep + 3] = '\0';
    return keep;
}

/* Last line of a wrapped paragraph that runs past the line budget. Its final
 * three bytes give way to "...", cut back to a character boundary, so the
 * visible shape is unchanged for ASCII. A line of three bytes or fewer has no
 * room for a marker and is copied unchanged, as before. Writes out[0..out_size)
 * and returns the bytes of line kept, not counting the marker or the NUL. */
static inline size_t ui_clip_wrapped_last_line(char *out, size_t out_size, const char *line) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!line) return 0;
    size_t len = strlen(line);
    if (len <= 3) {
        size_t n = len < out_size ? len : out_size - 1;
        n = ui_clip_utf8_floor(line, n);
        memcpy(out, line, n);
        out[n] = '\0';
        return n;
    }
    return ui_clip_ellipsize_utf8(out, out_size, line, len - 3);
}

#endif /* NAKAGAWA_UI_CLIP_H */
