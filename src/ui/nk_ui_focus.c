/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_focus.h"

NkUiId nk_ui_focus_directional(const NkUiFocusEntry *entries, size_t count,
                               NkUiId current, NkUiNavDir direction) {
    (void)entries;
    (void)count;
    (void)direction;
    return current;
}
