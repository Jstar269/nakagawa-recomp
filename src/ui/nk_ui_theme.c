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
    0xD7F24AFFu
};

const NkUiTheme *nk_ui_theme_n1(void) {
    return &g_n1_theme;
}
