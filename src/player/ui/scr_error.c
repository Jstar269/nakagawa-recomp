/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The error card: what failed, the details and the report. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>

PlayerUiAction player_ui_screen_error(NkUi *ui, const PlayerUiErrorModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Back to library"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    float height = model != NULL && model->logical_height > 80.0f ? model->logical_height : 800.0f;
    bool has_details = model != NULL && model->details != NULL && model->details[0] != '\0';
    int detail_lines;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    /* A details box (a path list, a boundary) gets a wider card, and every 16 px
       of height past the 540 px minimum window buys one more line of it, so the
       fix it names is not cut off. */
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       height, has_details ? 880.0f : 700.0f);
    if (inner <= 0.0f) return result;
    detail_lines = 6 + (height > 540.0f ? (int)((height - 540.0f) / 16.0f) : 0);
    if (detail_lines > 16) detail_lines = 16;
    scr_flow_eyebrow(ui, FLOW_MARK_CROSS, safe_text(model->badge, "Error"), theme->error);
    nk_ui_text_role(ui, FLOW_ID_TITLE, safe_text(model->title, "Something went wrong"),
                    NK_UI_TYPE_TITLE, theme->paper);
    if (model->failed_stage != NULL && model->failed_stage[0] != '\0') {
        char stage[96];
        (void)snprintf(stage, sizeof(stage), "Failed stage: %s", model->failed_stage);
        nk_ui_status_pill(ui, FLOW_ID_STAGES, FLOW_MARK_TRIANGLE, stage, theme->warning);
    }
    if (model->message != NULL && model->message[0] != '\0') {
        nk_ui_text_role(ui, FLOW_ID_MESSAGE, model->message, NK_UI_TYPE_BODY, theme->muted);
    }
    if ((model->details != NULL && model->details[0] != '\0') ||
        (model->log_path != NULL && model->log_path[0] != '\0')) {
        NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
        NkUiStyle style = scr_flow_box_style(theme, theme->surface);
        box.padding_top = box.padding_bottom = 12.0f;
        box.padding_left = box.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_DETAILS, &box, &style)) {
            NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
            data.wrap = true;
            if (model->details != NULL && model->details[0] != '\0') {
                nk_ui_eyebrow(ui, FLOW_ID_DETAILS + 1u, "Details");
                nk_ui_text(ui, FLOW_ID_DETAILS + 2u,
                           nk_ui_text_clamp(ui, model->details, &data, inner - 28.0f,
                                            detail_lines), &data);
            }
            if (model->log_path != NULL && model->log_path[0] != '\0') {
                char log[600];
                (void)snprintf(log, sizeof(log), "Log file: %s", model->log_path);
                nk_ui_text(ui, FLOW_ID_DETAILS + 3u,
                           nk_ui_text_clamp(ui, log, &data, inner - 28.0f, 2), &data);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_ERROR_RECOVER,
                             safe_text(model->recovery_label, "Back to library"),
                             NK_UI_BUTTON_PRIMARY)) {
                result = action(PLAYER_UI_ACTION_ERROR_RECOVER, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            if (model->retry_label != NULL && model->retry_label[0] != '\0' &&
                nk_ui_button(ui, PLAYER_UI_ID_ERROR_RETRY, model->retry_label, 0u)) {
                result = action(PLAYER_UI_ACTION_ERROR_RETRY, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            if (model->copy_available &&
                nk_ui_button(ui, PLAYER_UI_ID_ERROR_COPY,
                             model->copied ? "Details copied" : "Copy details", 0u)) {
                result = action(PLAYER_UI_ACTION_ERROR_COPY, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    /* Report this game: links the user's browser opens, on a row of their
       own. A known cause leads with its issue and Report steps back. */
    if (model->report_available) {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.align_y = NK_UI_ALIGN_CENTER;
        if (model->known_issue != 0u) {
            char known[64];
            (void)snprintf(known, sizeof(known), "This is a known issue (#%u).",
                           model->known_issue);
            nk_ui_text_role(ui, FLOW_ID_REPORT_TEXT, nk_ui_frame_text(ui, known),
                            NK_UI_TYPE_BODY_S, theme->paper);
        }
        if (nk_ui_begin_container(ui, FLOW_ID_REPORT_ROW, &row, NULL)) {
            if (model->known_issue != 0u) {
                char issue[32];
                (void)snprintf(issue, sizeof(issue), "See issue #%u", model->known_issue);
                if (nk_ui_button(ui, PLAYER_UI_ID_ERROR_ISSUE, nk_ui_frame_text(ui, issue),
                                 0u)) {
                    result = action(PLAYER_UI_ACTION_ERROR_KNOWN_ISSUE, 0u,
                                    PLAYER_UI_PRIMARY_NONE);
                }
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_ERROR_REPORT, "Report this game",
                             model->known_issue != 0u ? NK_UI_BUTTON_SMALL : 0u)) {
                result = action(PLAYER_UI_ACTION_ERROR_REPORT, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_ERROR_REPORTS, "See existing reports",
                             NK_UI_BUTTON_SMALL)) {
                result = action(PLAYER_UI_ACTION_ERROR_SEE_REPORTS, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, model->report_available
                     ? "Copy details uses the clipboard and Report opens your browser. "
                       "Nothing is sent anywhere."
                 : model->copy_available
                     ? "Copy details puts a short report on the clipboard. Nothing is sent "
                       "anywhere."
                     : "",
             prompts, 2u);
    return result;
}
