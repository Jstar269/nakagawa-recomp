/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Remove from library (#1304): the game, what Nakagawa built for it with
   sizes, the promise about the disc image, and the saves box, which stays
   off until the user ticks it. Cancel holds focus first. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>

PlayerUiAction player_ui_screen_remove(NkUi *ui, const PlayerUiRemoveModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Cancel"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    char size[32];
    unsigned long long total = 0ull;
    size_t shown;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 640.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Remove from library", theme->warning);
    (void)snprintf(text, sizeof(text), "Remove %s?", safe_text(model->title, "this game"));
    nk_ui_text_role(ui, FLOW_ID_TITLE, nk_ui_frame_text(ui, text), NK_UI_TYPE_TITLE,
                    theme->paper);
    shown = model->items != NULL ? model->item_count : 0u;
    if (shown > PLAYER_UI_REMOVE_ITEM_CAP) shown = PLAYER_UI_REMOVE_ITEM_CAP;
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    model->files_shared
                        ? "It leaves your library. What Nakagawa built for it and its saves "
                          "stay: another image of this disc in your library uses them."
                    : shown > 0u
                        ? "It leaves your library, and Nakagawa deletes what it built for it:"
                        : "It leaves your library. Nakagawa has not built anything for it yet.",
                    NK_UI_TYPE_BODY_S, theme->muted);
    if (shown > 0u) {
        NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
        NkUiStyle style = scr_flow_box_style(theme, theme->surface);
        rows.padding_top = rows.padding_bottom = 10.0f;
        rows.padding_left = rows.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
            for (size_t i = 0u; i < shown; ++i) {
                NkUiLayout line = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                total += model->items[i].bytes;
                if (nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_ITEMS + (NkUiId)i), &line,
                                          NULL)) {
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x10u + (NkUiId)i),
                                    safe_text(model->items[i].label, "Built files"),
                                    NK_UI_TYPE_BODY_S, theme->paper);
                    nk_ui_spacer(ui, (NkUiId)(FLOW_ID_ITEMS + 0x20u + (NkUiId)i),
                                 sizing_grow(), sizing_fixed(1.0f));
                    scr_flow_bytes(size, sizeof(size), model->items[i].bytes);
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x30u + (NkUiId)i),
                                    nk_ui_frame_text(ui, size), NK_UI_TYPE_DATA, theme->muted);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
        scr_flow_bytes(size, sizeof(size), total);
        (void)snprintf(text, sizeof(text), "Frees %s", size);
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                        theme->warning);
    }
    nk_ui_text_role(ui, FLOW_ID_DETAILS,
                    FLOW_MARK_CHECK "The disc image on your PC is not touched.",
                    NK_UI_TYPE_BODY_S, theme->ready);
    if (model->has_saves) {
        scr_flow_bytes(size, sizeof(size), model->saves_bytes);
        (void)snprintf(text, sizeof(text), "%sAlso delete this game's saves (%s)",
                       model->delete_saves ? FLOW_MARK_CHECK : "", size);
        if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_SAVES, nk_ui_frame_text(ui, text), 0u)) {
            result = action(PLAYER_UI_ACTION_REMOVE_TOGGLE_SAVES, model->title_index,
                            PLAYER_UI_PRIMARY_NONE);
        }
        nk_ui_text_role(ui, FLOW_ID_MESSAGE,
                        model->delete_saves ? "Your saves for this game will be deleted too."
                                            : "Your saves stay.",
                        NK_UI_TYPE_BODY_S, model->delete_saves ? theme->error : theme->muted);
    } else if (!model->files_shared) {
        nk_ui_text_role(ui, FLOW_ID_MESSAGE, "This game has no saves.", NK_UI_TYPE_BODY_S,
                        theme->muted);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_CANCEL, "Cancel", 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                PLAYER_UI_PRIMARY_NONE);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_CONFIRM, "Remove", NK_UI_BUTTON_DANGER)) {
                result = action(PLAYER_UI_ACTION_REMOVE_CONFIRM, model->title_index,
                                model->delete_saves ? (PlayerUiPrimaryAction)1
                                                    : PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, "", prompts, 3u);
    return result;
}
