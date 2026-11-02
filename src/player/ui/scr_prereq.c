/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The one-time build tool download: consent, then progress. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>
#include <string.h>

PlayerUiAction player_ui_screen_prereq(NkUi *ui, const PlayerUiPrereqModel *model) {
    static const NkUiPromptItem prompts_consent[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Cancel"}
    };
    static const NkUiPromptItem prompts_progress[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Cancel"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    char a[32];
    char b[32];
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 820.0f);
    if (inner <= 0.0f) return result;
    if (!model->in_progress) {
        size_t shown = model->item_count;
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "One-time build tool download", theme->info);
        nk_ui_text_role(ui, FLOW_ID_TITLE,
                        "Download the pinned tools needed to build this game?",
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "Nothing is downloaded until you choose DOWNLOAD, and your answer applies "
                        "to this build only. Each file is checked against its pinned hash.",
                        NK_UI_TYPE_BODY_S, theme->muted);
        {
            NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 0.0f);
            NkUiStyle style = scr_flow_box_style(theme, theme->surface);
            rows.padding_top = rows.padding_bottom = 8.0f;
            rows.padding_left = rows.padding_right = 14.0f;
            if (shown > 9u) shown = 9u;
            if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
                NkUiTextStyle line = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
                line.line_height = 20.0f;
                for (size_t i = 0u; i < shown; ++i) {
                    const PlayerUiPrereqItem *item = &model->items[i];
                    const char *license = item->license != NULL &&
                        strcmp(item->license, "NOASSERTION") == 0
                        ? "licence review in the works" : safe_text(item->license, "");
                    scr_flow_bytes(a, sizeof(a), item->bytes);
                    (void)snprintf(text, sizeof(text), "%s %s" FLOW_MIDDOT "%s" FLOW_MIDDOT "%s"
                                   FLOW_MIDDOT "%s",
                                   safe_text(item->name, "?"), safe_text(item->version, ""),
                                   safe_text(item->host, ""), a, license);
                    nk_ui_text(ui, (NkUiId)(FLOW_ID_ROWS + 1u + (NkUiId)i),
                               nk_ui_text_clamp(ui, text, &line, inner - 28.0f, 1), &line);
                }
                if (model->item_count > shown) {
                    (void)snprintf(text, sizeof(text), "and %zu more", model->item_count - shown);
                    nk_ui_text(ui, FLOW_ID_ROWS_MORE, text, &line);
                }
                nk_ui_end_container(ui);
            }
        }
        scr_flow_bytes(a, sizeof(a), model->total_bytes);
        (void)snprintf(text, sizeof(text), "Total %s across %zu components", a, model->item_count);
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                        theme->warning);
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_PREREQ_ACCEPT, "Download",
                                 NK_UI_BUTTON_PRIMARY)) {
                    result = action(PLAYER_UI_ACTION_PREREQ_ACCEPT, 0u, PLAYER_UI_PRIMARY_NONE);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_PREREQ_CANCEL, "Cancel", 0u)) {
                    result = action(PLAYER_UI_ACTION_PREREQ_CANCEL, 0u, PLAYER_UI_PRIMARY_NONE);
                }
                nk_ui_end_container(ui);
            }
        }
        scr_flow_end(ui, "", prompts_consent, 3u);
        return result;
    }
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Installing build tools", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "The files are checked before they are installed.",
                    NK_UI_TYPE_TITLE, theme->paper);
    (void)snprintf(text, sizeof(text), "Now: %s",
                   safe_text(model->current_item, "Starting the verified fetcher"));
    nk_ui_text_role(ui, FLOW_ID_LEAD, text, NK_UI_TYPE_BODY_S, theme->paper);
    nk_ui_progress_bar(ui, FLOW_ID_BAR, inner,
                       model->total_bytes > 0ull
                           ? (float)((double)model->total_received_bytes /
                                     (double)model->total_bytes) : -1.0f, 0.0);
    scr_flow_bytes(a, sizeof(a), model->total_received_bytes);
    scr_flow_bytes(b, sizeof(b), model->total_bytes);
    (void)snprintf(text, sizeof(text), "%s of %s", a, b);
    nk_ui_text_role(ui, FLOW_ID_TOTAL, text, NK_UI_TYPE_DATA, theme->muted);
    scr_flow_bytes(a, sizeof(a), model->item_received_bytes);
    scr_flow_bytes(b, sizeof(b), model->item_total_bytes);
    (void)snprintf(text, sizeof(text), "This item: %s of %s", a, b);
    nk_ui_text_role(ui, FLOW_ID_DETAILS, text, NK_UI_TYPE_DATA, theme->dim);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_PREREQ_CANCEL, "Cancel", 0u)) {
                result = action(PLAYER_UI_ACTION_PREREQ_CANCEL, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, "The build starts by itself once the tools are installed.", prompts_progress, 2u);
    return result;
}
