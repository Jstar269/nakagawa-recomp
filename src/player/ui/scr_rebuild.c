/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Games an older Nakagawa built: the once-per-update prompt (Rebuild all,
   Later) or, with a title, the reminder a launch shows (Rebuild now, Play
   anyway). The rebuild is the primary choice and holds focus first; Back is
   the second choice, so nothing is rebuilt by accident. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

PlayerUiAction player_ui_screen_rebuild(NkUi *ui, const PlayerUiRebuildModel *model) {
    static const NkUiPromptItem prompts_update[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Later"}
    };
    static const NkUiPromptItem prompts_launch[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Play anyway"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    bool launch;
    float inner;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    launch = model->title != NULL;
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 640.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Older build", theme->info);
    nk_ui_text_role(ui, FLOW_ID_TITLE,
                    launch ? nk_ui_frame_text(ui, model->title) : "Rebuild your games",
                    NK_UI_TYPE_TITLE, theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD, safe_text(model->text, ""), NK_UI_TYPE_BODY_S,
                    theme->muted);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_REBUILD_PRIMARY,
                             launch ? "Rebuild now" : "Rebuild all", NK_UI_BUTTON_PRIMARY)) {
                result = action(PLAYER_UI_ACTION_REBUILD, 0u,
                                (PlayerUiPrimaryAction)(launch ? PLAYER_UI_REBUILD_NOW
                                                               : PLAYER_UI_REBUILD_ALL));
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_REBUILD_SECOND,
                             launch ? "Play anyway" : "Later", NK_UI_BUTTON_NOT_DEFAULT)) {
                result = action(PLAYER_UI_ACTION_REBUILD, 0u,
                                (PlayerUiPrimaryAction)(launch ? PLAYER_UI_REBUILD_PLAY_ANYWAY
                                                               : PLAYER_UI_REBUILD_LATER));
            }
            nk_ui_end_container(ui);
        }
    }
    if (result.kind == PLAYER_UI_ACTION_NONE && nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        result = action(PLAYER_UI_ACTION_REBUILD, 0u,
                        (PlayerUiPrimaryAction)(launch ? PLAYER_UI_REBUILD_PLAY_ANYWAY
                                                       : PLAYER_UI_REBUILD_LATER));
    }
    scr_flow_end(ui, "", launch ? prompts_launch : prompts_update, 3u);
    return result;
}
