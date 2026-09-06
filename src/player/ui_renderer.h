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
} UiInput;

/* Main render entry point */
void ui_render_frame(SDL_Renderer *renderer, PlayerApp *app, const UiInput *input);

/* Helper to capture current render target to BMP file */
bool ui_capture_screenshot(SDL_Renderer *renderer, const char *output_bmp_path);

#endif /* NAKAGAWA_UI_RENDERER_H */
