/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_UI_RENDERER_H
#define NAKAGAWA_UI_RENDERER_H

#include "player_state.h"
#include <SDL3/SDL.h>

typedef struct {
    int mouse_x;
    int mouse_y;
    bool mouse_down;
    bool mouse_clicked;
    /* Set for exactly one frame when the user presses Enter/Space (keyboard)
     * or gamepad SOUTH. A focused button treats this exactly like a click,
     * so every pointer action has a keyboard/gamepad equivalent. */
    bool activate_pressed;
} UiInput;

/* Main render entry point */
void ui_render_frame(SDL_Renderer *renderer, PlayerApp *app, const UiInput *input);

/* How many keyboard/gamepad focus stops the current view offers. The event
 * loop clamps app->focus_index into [0, count) before rendering; the
 * renderer draws the focus ring on app->focus_index and honors
 * UiInput.activate_pressed on it. Returns at least 1 whenever the view
 * draws any button. */
int ui_focus_count(const PlayerApp *app);

/* Release cached font textures, the font, and the dynamically loaded TTF
 * library. Call once per renderer lifetime before SDL_Quit. Safe to call
 * when typography never initialized. */
void ui_font_shutdown(void);

/* Pixel-density multiplier applied to raster point sizes (crisper type on
 * high-density displays). Values outside [1, 3] are ignored. */
void ui_font_set_density(float density);

/* Helper to capture current render target to BMP file */
bool ui_capture_screenshot(SDL_Renderer *renderer, const char *output_bmp_path);

#endif /* NAKAGAWA_UI_RENDERER_H */
