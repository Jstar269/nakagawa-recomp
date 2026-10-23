/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"

#define COLOR_PAGE 0x0B0F14FFu
#define COLOR_SURFACE 0x121821FFu
#define COLOR_BORDER 0x303B48FFu
#define COLOR_INFO 0x6CB6FFFFu

static NkUiSizing sizing_grow(void) {
    NkUiSizing sizing = {NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    return sizing;
}

static NkUiSizing sizing_fixed(float value) {
    NkUiSizing sizing = {NK_UI_SIZE_FIXED, value, 0.0f, 0.0f};
    return sizing;
}

static NkUiLayout layout_column(float gap, float padding) {
    NkUiLayout layout;
    layout.width = sizing_grow();
    layout.height = sizing_grow();
    layout.direction = NK_UI_LAYOUT_COLUMN;
    layout.gap = gap;
    layout.padding_left = padding;
    layout.padding_top = padding;
    layout.padding_right = padding;
    layout.padding_bottom = padding;
    return layout;
}

static NkUiStyle style_fill(uint32_t fill, uint32_t border, float border_width,
                            float radius) {
    NkUiStyle style;
    style.fill_rgba = fill;
    style.border_rgba = border;
    style.border_width = border_width;
    style.corner_radius = radius;
    return style;
}

static PlayerUiAction action(PlayerUiActionKind kind, size_t title_index,
                             PlayerUiPrimaryAction primary) {
    PlayerUiAction result;
    result.kind = kind;
    result.primary = primary;
    result.title_index = title_index;
    return result;
}

static const char *safe_text(const char *text, const char *fallback) {
    return text != NULL && text[0] != '\0' ? text : fallback;
}

PlayerUiAction player_ui_screen_title(NkUi *ui,
                                      const PlayerUiTitleModel *model) {
    NkUiLayout root_layout;
    NkUiStyle root_style;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u,
                                   PLAYER_UI_PRIMARY_NONE);
    const PlayerUiTitle *title;
    if (ui == NULL || model == NULL || model->title == NULL) return result;
    title = model->title;

    root_layout = layout_column(16.0f, 28.0f);
    root_style = style_fill(COLOR_PAGE, COLOR_PAGE, 0.0f, 0.0f);
    if (!nk_ui_begin_container(ui, 0x20000001u, &root_layout, &root_style)) {
        return result;
    }
    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_BACK, "BACK TO LIBRARY", 0u)) {
        result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                        PLAYER_UI_PRIMARY_NONE);
        nk_ui_end_container(ui);
        return result;
    }
    nk_ui_label(ui, 0x20000002u, "NAKAGAWA RECOMP", 1, 18);
    nk_ui_label(ui, 0x20000003u, safe_text(title->title, "Untitled PSP game"),
                1, 34);
    if (title->disc_id != NULL && title->disc_id[0] != '\0') {
        nk_ui_label(ui, 0x20000004u, title->disc_id, 2, 15);
    }

    {
        NkUiLayout compatibility_layout = layout_column(8.0f, 20.0f);
        NkUiStyle compatibility_style =
            style_fill(COLOR_SURFACE, COLOR_BORDER, 1.0f, 8.0f);
        compatibility_layout.height =
            (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
        if (nk_ui_begin_container(ui, 0x20000005u,
                                  &compatibility_layout, &compatibility_style)) {
            nk_ui_label(ui, 0x20000006u, "COMPATIBILITY", 0, 13);
            nk_ui_label(ui, 0x20000007u,
                        safe_text(title->compatibility_word, ""), 1, 24);
            if (title->compatibility_detail != NULL &&
                title->compatibility_detail[0] != '\0') {
                nk_ui_label(ui, 0x20000008u, title->compatibility_detail,
                            0, 15);
            }
            nk_ui_end_container(ui);
        }
    }

    if (title->library_status != NULL && title->library_status[0] != '\0') {
        NkUiLayout status_layout;
        NkUiStyle status_style = style_fill(title->status_rgba,
                                            title->status_rgba, 0.0f, 5.0f);
        status_layout.width = sizing_grow();
        status_layout.height = sizing_fixed(28.0f);
        status_layout.direction = NK_UI_LAYOUT_ROW;
        status_layout.gap = 8.0f;
        status_layout.padding_left = 8.0f;
        status_layout.padding_top = 4.0f;
        status_layout.padding_right = 8.0f;
        status_layout.padding_bottom = 4.0f;
        if (nk_ui_begin_container(ui, 0x20000009u,
                                  &status_layout, &status_style)) {
            nk_ui_label(ui, 0x2000000Du,
                        safe_text(title->status_marker, "●"), 0, 14);
            nk_ui_label(ui, 0x2000000Eu, title->library_status, 0, 14);
            nk_ui_end_container(ui);
        }
    }
    if (title->library_status_detail != NULL &&
        title->library_status_detail[0] != '\0') {
        nk_ui_label(ui, 0x2000000Au, title->library_status_detail, 0, 13);
    }
    if (title->input_note != NULL && title->input_note[0] != '\0') {
        NkUiLayout note_layout = layout_column(4.0f, 14.0f);
        NkUiStyle note_style = style_fill(COLOR_SURFACE, COLOR_INFO, 1.0f, 6.0f);
        note_layout.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
        if (nk_ui_begin_container(ui, 0x2000000Bu, &note_layout, &note_style)) {
            nk_ui_label(ui, 0x2000000Cu, title->input_note, 0, 14);
            nk_ui_end_container(ui);
        }
    }
    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_PRIMARY,
                     safe_text(title->primary_action_label, "UNAVAILABLE"),
                     title->primary_action_enabled
                         ? NK_UI_BUTTON_PRIMARY : NK_UI_BUTTON_DISABLED)) {
        result = action(PLAYER_UI_ACTION_PRIMARY, model->title_index,
                        title->primary_action);
    }
    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_SAVE_FOLDER,
                     "OPEN SAVE FOLDER",
                     title->save_folder_enabled ? 0u : NK_UI_BUTTON_DISABLED)) {
        result = action(PLAYER_UI_ACTION_OPEN_SAVE_FOLDER,
                        model->title_index, PLAYER_UI_PRIMARY_NONE);
    }
    nk_ui_end_container(ui);
    return result;
}
