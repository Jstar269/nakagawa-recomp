/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_WAVES_H
#define NK_UI_WAVES_H

#include "nk_ui.h"

#define NK_UI_WAVES_RIBBON_COUNT 4u
#define NK_UI_WAVES_TINT_COUNT 12u
#define NK_UI_WAVES_ALPHA_CAP_MAX 0.24f
#define NK_UI_WAVES_LUMINANCE_MAX 0.0399f

typedef struct {
    uint32_t rgba[NK_UI_WAVES_RIBBON_COUNT]; /* 0xRRGGBBAA sRGB bytes */
    float cap;
} NkUiWavePalette;

typedef struct {
    double phase[NK_UI_WAVES_RIBBON_COUNT][3];
    double width_phase[NK_UI_WAVES_RIBBON_COUNT];
    float color_rgba[NK_UI_WAVES_RIBBON_COUNT][4];
    float cap;
    float width_px;
    float height_px;
} NkUiWavesCommand;

bool nk_ui_waves_palette(unsigned tint_index, NkUiWavePalette *out);
float nk_ui_waves_compute_cap(const NkUiWavePalette *palette, uint32_t surface_rgba);
bool nk_ui_waves_contrast_ok(const NkUiWavePalette *palette, uint32_t surface_rgba,
                             float luminance_limit);
void nk_ui_waves_command(double now_s, float width_px, float height_px,
                         const NkUiWavePalette *palette, NkUiWavesCommand *out);
bool nk_ui_waves_render_rgba(const NkUiWavePalette *palette, double now_s,
                             uint32_t width, uint32_t height, uint8_t *pixels,
                             size_t stride);

#endif /* NK_UI_WAVES_H */
