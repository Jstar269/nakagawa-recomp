/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_FOCUS_H
#define NK_UI_FOCUS_H

#include "nk_ui.h"

typedef struct {
    NkUiId id;
    NkUiRect rect;
    uint32_t flags;
    uint32_t scope;
} NkUiFocusEntry;

/* Full focus graph navigation is implemented in the input lane. */
NkUiId nk_ui_focus_directional(const NkUiFocusEntry *entries, size_t count,
                               NkUiId current, NkUiNavDir direction);

#endif /* NK_UI_FOCUS_H */
