/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Stop game: the same question quitting with a game running asks, in a flow
   card. KEEP PLAYING holds focus first; STOP GAME is the danger button. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>

PlayerUiAction player_ui_screen_stop(NkUi *ui, const PlayerUiStopModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Keep playing"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 640.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Stop game", theme->warning);
    (void)snprintf(text, sizeof(text), "Stop %s?", safe_text(model->title, "the game"));
    nk_ui_text_role(ui, FLOW_ID_TITLE, nk_ui_frame_text(ui, text), NK_UI_TYPE_TITLE,
                    theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    "The game closes now, and anything since its last save is lost.",
                    NK_UI_TYPE_BODY_S, theme->muted);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_STOP_KEEP, "Keep playing", 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                PLAYER_UI_PRIMARY_NONE);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_STOP_CONFIRM, "Stop game", NK_UI_BUTTON_DANGER)) {
                result = action(PLAYER_UI_ACTION_PRIMARY, model->title_index,
                                PLAYER_UI_PRIMARY_STOP_GAME);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, "", prompts, 3u);
    return result;
}
