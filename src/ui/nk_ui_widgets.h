/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_WIDGETS_H
#define NK_UI_WIDGETS_H

#include "nk_ui.h"

/* Internal widget/core seam: focusable controls register their previous-frame
   Clay bounds so nk_ui can move focus before the next screen is built. */
void nk_ui_register_button(NkUi *ui, NkUiId id, uint32_t flags);
bool nk_ui_button_focused(const NkUi *ui, NkUiId id);

#endif /* NK_UI_WIDGETS_H */
