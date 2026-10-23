/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_THEME_H
#define NK_UI_THEME_H

#include "nk_ui.h"

/* Colors use 0xRRGGBBAA sRGB bytes. */
struct NkUiTheme {
    uint32_t ink;
    uint32_t paper;
    uint32_t surface;
    uint32_t surface_raised;
    uint32_t surface_high;
    uint32_t accent_dark;
    uint32_t accent_light;
    uint32_t accent_pressed;
    uint32_t ready;
    uint32_t warning;
    uint32_t info;
    uint32_t error;
    uint32_t muted;
    uint32_t focus;
};

/* nk_ui_theme_n1() is declared in nk_ui.h. */

#endif /* NK_UI_THEME_H */
