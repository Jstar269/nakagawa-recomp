/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_vk.h"

bool nk_ui_vk_available(void) {
    return false;
}

bool nk_ui_vk_render(NkUiVk *vk, const NkUiDrawList *list) {
    (void)vk;
    (void)list;
    return false;
}
