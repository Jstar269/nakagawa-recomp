/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_theme.h"

static const NkUiTheme g_n1_theme = {
    0x10151CFFu, /* ink */
    0xF5F7FAFFu, /* paper */
    0x0B0F14FFu, /* page / surface */
    0x121821FFu,
    0x18202BFFu,
    0xD7F24AFFu, /* dark surface accent */
    0x6E9600FFu, /* light surface accent */
    0xB4D134FFu,
    0x2DD4BFFFu, /* Ready */
    0xDBB024FFu, /* warning */
    0x6CB6FFFFu,
    0xFF7B72FFu,
    0x9AA4B2FFu,
    0xD7F24AFFu,
    0x1C232DFFu, /* line */
    0x2E3846FFu, /* line_strong */
    0x6B7684FFu, /* dim */
    0x222C39FFu, /* pressed */
    0x2A1A1CFFu, /* pressed_danger */
    0x0B0F14C7u, /* scrim, 78% */
    0x0B0F14B8u, /* art_scrim, 72% */
    0xC9D6E3FFu, /* art_label */
    0x3A4552FFu, /* placeholder */
    0x1F2613FFu  /* capture */
};

const NkUiTheme *nk_ui_theme_n1(void) {
    return &g_n1_theme;
}

static const char *const g_font_paths[NK_UI_FONT_COUNT] = {
    "assets/fonts/ui/geist/Geist-Regular.ttf",
    "assets/fonts/ui/chakra-petch/ChakraPetch-SemiBold.ttf",
    "assets/fonts/ui/geist-mono/GeistMono-Regular.ttf",
    "assets/fonts/ui/noto-sans-jp/NotoSansJP-Regular.ttf",
    "assets/fonts/ui/geist/Geist-SemiBold.ttf",
    "assets/fonts/ui/geist/Geist-Medium.ttf",
    "assets/fonts/ui/geist-mono/GeistMono-Medium.ttf",
    "assets/fonts/ui/chakra-petch/ChakraPetch-Bold.ttf"
};

const char *nk_ui_font_path(int font) {
    if (font < 0 || font >= NK_UI_FONT_COUNT) {
        return NULL;
    }
    return g_font_paths[font];
}

typedef struct {
    int font;
    int size_px;
    float tracking_em;
    float line_height;
} NkUiTypeSpec;

static const NkUiTypeSpec g_type_scale[NK_UI_TYPE_COUNT] = {
    {NK_UI_FONT_DISPLAY_BOLD, 40, 0.04f, 44.0f},   /* display-xl */
    {NK_UI_FONT_DISPLAY_BOLD, 30, 0.04f, 32.0f},   /* display */
    {NK_UI_FONT_DISPLAY_BOLD, 18, 0.06f, 20.0f},   /* wordmark */
    {NK_UI_FONT_BODY_SEMIBOLD, 30, -0.01f, 34.0f}, /* title-xl */
    {NK_UI_FONT_BODY_SEMIBOLD, 22, 0.0f, 28.0f},   /* title */
    {NK_UI_FONT_BODY_SEMIBOLD, 17, 0.0f, 24.0f},   /* heading */
    {NK_UI_FONT_BODY_SEMIBOLD, 15, 0.0f, 20.0f},   /* tile title */
    {NK_UI_FONT_BODY, 17, 0.0f, 26.0f},            /* body-l */
    {NK_UI_FONT_BODY, 15, 0.0f, 22.0f},            /* body */
    {NK_UI_FONT_BODY, 14, 0.0f, 21.0f},            /* body-s */
    {NK_UI_FONT_BODY, 13, 0.0f, 19.0f},            /* caption */
    {NK_UI_FONT_BODY, 12, 0.0f, 16.0f},            /* footnote */
    {NK_UI_FONT_MONO_MEDIUM, 12, 0.08f, 16.0f},    /* label */
    {NK_UI_FONT_MONO_MEDIUM, 14, 0.12f, 18.0f},    /* label-l */
    {NK_UI_FONT_MONO_MEDIUM, 11, 0.06f, 14.0f},    /* label-s */
    {NK_UI_FONT_MONO_MEDIUM, 11, 0.12f, 14.0f},    /* eyebrow */
    {NK_UI_FONT_MONO_MEDIUM, 10, 0.08f, 12.0f},    /* micro */
    {NK_UI_FONT_MONO, 12, 0.0f, 16.0f}             /* data */
};

NkUiTextStyle nk_ui_type(NkUiTypeRole role, uint32_t rgba) {
    NkUiTextStyle style;
    const NkUiTypeSpec *spec;
    if ((unsigned)role >= (unsigned)NK_UI_TYPE_COUNT) {
        role = NK_UI_TYPE_BODY;
    }
    spec = &g_type_scale[role];
    style.font = spec->font;
    style.size_px = spec->size_px;
    style.rgba = rgba;
    style.letter_spacing = spec->tracking_em * (float)spec->size_px;
    style.line_height = spec->line_height;
    style.wrap = role >= NK_UI_TYPE_TITLE_XL && role <= NK_UI_TYPE_FOOTNOTE;
    return style;
}
