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

#endif /* NAKAGAWA_UI_CLIP_H */
