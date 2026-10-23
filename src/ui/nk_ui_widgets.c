/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_widgets.h"

void nk_ui_button_configuration(const NkUi *ui, uint32_t flags,
                                NkUiLayout *layout, NkUiStyle *style);
bool nk_ui_take_activation(NkUi *ui, NkUiId id);

bool nk_ui_button(NkUi *ui, NkUiId id, const char *label, uint32_t flags) {
    NkUiLayout layout;
    NkUiStyle style;
    bool opened;
    bool activated;
    if (ui == NULL || label == NULL) {
        return false;
    }
    nk_ui_button_configuration(ui, flags, &layout, &style);
    opened = nk_ui_begin_container(ui, id, &layout, &style);
    if (!opened) {
        return false;
    }
    activated = (flags & NK_UI_BUTTON_DISABLED) == 0u && nk_ui_take_activation(ui, id);
    nk_ui_label(ui, id, label, 0, 16);
    nk_ui_end_container(ui);
    return activated;
}
