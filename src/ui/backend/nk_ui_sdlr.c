/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_sdlr.h"

bool nk_ui_sdlr_available(void) {
    return false;
}

bool nk_ui_sdlr_render(NkUiSdlr *renderer, const NkUiDrawList *list) {
    (void)renderer;
    (void)list;
    return false;
}
