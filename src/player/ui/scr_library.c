/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"

#include <stdio.h>

#define COLOR_PAGE 0x0B0F14FFu
#define COLOR_SURFACE 0x121821FFu
#define COLOR_BORDER 0x303B48FFu
#define COLOR_MUTED 0xB6C0CCFFu
#define LIBRARY_CARD_HEIGHT 148.0f

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

static PlayerUiAction action(PlayerUiActionKind kind, size_t title_index) {
    PlayerUiAction result;
    result.kind = kind;
    result.primary = PLAYER_UI_PRIMARY_NONE;
    result.title_index = title_index;
    return result;
}

static const char *safe_text(const char *text, const char *fallback) {
    return text != NULL && text[0] != '\0' ? text : fallback;
}

PlayerUiAction player_ui_screen_library(NkUi *ui,
                                        const PlayerUiLibraryModel *model) {
    NkUiLayout root_layout;
    NkUiStyle root_style;
    size_t page_size;
    size_t start;
    size_t visible_count;
    size_t columns;
    size_t rows;
    float card_width;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u);
    if (ui == NULL || model == NULL) {
        return result;
    }

    root_layout = layout_column(16.0f, 28.0f);
    root_style = style_fill(COLOR_PAGE, COLOR_PAGE, 0.0f, 0.0f);
    if (!nk_ui_begin_container(ui, 0x10000001u, &root_layout, &root_style)) {
        return result;
    }
    nk_ui_label(ui, 0x10000002u, "NAKAGAWA RECOMP", 1, 22);
    nk_ui_label(ui, 0x10000003u, "LIBRARY", 0, 15);

    if (model->title_count == 0u || model->titles == NULL) {
        NkUiLayout empty_layout = layout_column(16.0f, 24.0f);
        NkUiStyle empty_style = style_fill(COLOR_SURFACE, COLOR_BORDER, 1.0f, 10.0f);
        empty_layout.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
        nk_ui_label(ui, 0x10000004u, "Your library is empty", 1, 32);
        if (nk_ui_begin_container(ui, 0x10000005u, &empty_layout, &empty_style)) {
            nk_ui_label(ui, 0x10000006u,
                        "Select a PSP ISO you own to add a game to your library.",
                        0, 18);
            if (nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_EMPTY_ADD,
                             "ADD A PSP DISC IMAGE", NK_UI_BUTTON_PRIMARY)) {
                result = action(PLAYER_UI_ACTION_ADD_DISC, 0u);
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
        return result;
    }

    columns = model->columns > 0u ? model->columns : 1u;
    if (columns > 4u) columns = 4u;
    page_size = model->page_size > 0u ? model->page_size : columns * 2u;
    if (page_size < columns) page_size = columns;
    start = model->page_index > SIZE_MAX / page_size
        ? model->title_count : model->page_index * page_size;
    if (start > model->title_count) start = model->title_count;
    visible_count = model->title_count - start;
    if (visible_count > page_size) visible_count = page_size;
    rows = visible_count == 0u ? 0u : (visible_count + columns - 1u) / columns;
    {
        float available_width = model->logical_width > 80.0f
            ? model->logical_width - 56.0f : 1160.0f;
        card_width = (available_width - 20.0f * (float)(columns - 1u)) /
                     (float)columns;
        if (card_width < 180.0f) card_width = 180.0f;
    }

    for (size_t row = 0u; row < rows; ++row) {
        NkUiLayout row_layout;
        row_layout.width = sizing_grow();
        row_layout.height = sizing_fixed(LIBRARY_CARD_HEIGHT);
        row_layout.direction = NK_UI_LAYOUT_ROW;
        row_layout.gap = 20.0f;
        row_layout.padding_left = 0.0f;
        row_layout.padding_top = 0.0f;
        row_layout.padding_right = 0.0f;
        row_layout.padding_bottom = 0.0f;
        if (!nk_ui_begin_container(ui, (NkUiId)(0x10000100u + (NkUiId)row),
                                   &row_layout, NULL)) {
            continue;
        }
        for (size_t col = 0u; col < columns; ++col) {
            size_t local_index = row * columns + col;
            size_t index;
            const PlayerUiTitle *title;
            NkUiLayout card_layout;
            NkUiStyle card_style;
            NkUiSizing card_width_sizing;
            NkUiId card_container_id;
            NkUiId item_id;
            if (local_index >= visible_count) break;
            index = start + local_index;
            title = &model->titles[index];
            card_width_sizing = sizing_fixed(card_width);
            card_layout.width = card_width_sizing;
            card_layout.height = sizing_grow();
            card_layout.direction = NK_UI_LAYOUT_COLUMN;
            card_layout.gap = 8.0f;
            card_layout.padding_left = 14.0f;
            card_layout.padding_top = 12.0f;
            card_layout.padding_right = 14.0f;
            card_layout.padding_bottom = 12.0f;
            card_style = style_fill(COLOR_SURFACE, COLOR_BORDER, 1.0f, 8.0f);
            card_container_id = (NkUiId)(0x10001000u + (NkUiId)index);
            item_id = PLAYER_UI_ID_LIBRARY_ITEM(index);
            if (!nk_ui_begin_container(ui, card_container_id,
                                       &card_layout, &card_style)) {
                continue;
            }
            if (nk_ui_button(ui, item_id,
                             safe_text(title->title, "Untitled PSP game"), 0u)) {
                result = action(PLAYER_UI_ACTION_OPEN_TITLE, index);
            }
            if (title->disc_id != NULL && title->disc_id[0] != '\0') {
                nk_ui_label(ui, (NkUiId)(0x30000000u + (NkUiId)(index * 2u)),
                            title->disc_id, 2, 14);
            }
            {
                NkUiLayout badge_layout;
                NkUiStyle badge_style = style_fill(title->status_rgba,
                                                   title->status_rgba,
                                                   0.0f, 5.0f);
                badge_layout.width = sizing_grow();
                badge_layout.height = sizing_fixed(26.0f);
                badge_layout.direction = NK_UI_LAYOUT_ROW;
                badge_layout.gap = 8.0f;
                badge_layout.padding_left = 8.0f;
                badge_layout.padding_top = 4.0f;
                badge_layout.padding_right = 8.0f;
                badge_layout.padding_bottom = 4.0f;
                if (nk_ui_begin_container(ui,
                        (NkUiId)(0x31000000u + (NkUiId)(index * 2u)),
                        &badge_layout, &badge_style)) {
                    nk_ui_label(ui,
                        (NkUiId)(0x31100000u + (NkUiId)(index * 2u)),
                        safe_text(title->status_marker, "●"), 0, 14);
                    nk_ui_label(ui,
                        (NkUiId)(0x31200000u + (NkUiId)(index * 2u)),
                        safe_text(title->library_status, ""), 0, 13);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }

    if (model->title_count > page_size) {
        size_t page_count = (model->title_count + page_size - 1u) / page_size;
        if (nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_PAGE_PREVIOUS,
                         "PREVIOUS PAGE",
                         model->page_index == 0u ? NK_UI_BUTTON_DISABLED : 0u)) {
            result = action(PLAYER_UI_ACTION_PAGE_PREVIOUS, 0u);
        }
        nk_ui_label(ui, 0x10000007u,
                    safe_text(model->page_label, "More titles"), 2, 13);
        if (nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_PAGE_NEXT,
                         "NEXT PAGE",
                         model->page_index + 1u >= page_count
                             ? NK_UI_BUTTON_DISABLED : 0u)) {
            result = action(PLAYER_UI_ACTION_PAGE_NEXT, 0u);
        }
    }
    if (nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_ADD, "ADD ANOTHER ISO", 0u)) {
        result = action(PLAYER_UI_ACTION_ADD_DISC, 0u);
    }
    nk_ui_end_container(ui);
    return result;
}
