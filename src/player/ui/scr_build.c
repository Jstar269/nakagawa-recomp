/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Building the package: the live stages, the elapsed time, the log and the
   outcome, in one centred dialog card. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>
#include <string.h>

PlayerUiAction player_ui_screen_build(NkUi *ui, const PlayerUiBuildModel *model) {
    static const char *const stages[4] = {"Preflight", "Extract", "Compile", "Package"};
    static const NkUiPromptItem prompts_running[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Cancel build"}
    };
    static const NkUiPromptItem prompts_done[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char elapsed[48];
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 720.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, model->built ? FLOW_MARK_DOT : FLOW_MARK_DIAMOND,
                 model->built ? "Package built" : "Building the package",
                 model->built ? theme->ready : theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, safe_text(model->title_line, "Package build"),
                    NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                     NK_UI_SPACE_2);
        if (nk_ui_begin_container(ui, FLOW_ID_STAGES, &row, NULL)) {
            for (int i = 0; i < 4; ++i) {
                bool done = model->built || model->stage > i;
                bool current = !model->built && model->stage == i;
                NkUiLayout chip = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(),
                                              sizing_fixed(NK_UI_CHIP_HEIGHT + 4.0f), 6.0f);
                NkUiStyle style;
                char label[48];
                memset(&style, 0, sizeof(style));
                style.corner_radius = NK_UI_RADIUS_CHIP;
                style.border_width = NK_UI_BORDER_HAIRLINE;
                style.fill_rgba = current ? theme->accent_dark : theme->surface_high;
                style.border_rgba = current ? theme->accent_dark
                                            : done ? theme->ready : theme->line_strong;
                chip.align_x = NK_UI_ALIGN_CENTER;
                chip.align_y = NK_UI_ALIGN_CENTER;
                (void)snprintf(label, sizeof(label), "%s%s", done ? FLOW_MARK_CHECK : "",
                               stages[i]);
                if (nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_STAGES + 1u + (NkUiId)i),
                                          &chip, &style)) {
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_STAGE_LABELS + (NkUiId)i),
                                    nk_ui_caps(ui, label), NK_UI_TYPE_LABEL_S,
                                    current ? theme->ink : done ? theme->ready : theme->muted);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
    }
    /* Determinate while the compile stage counts its parts; a sweep otherwise. */
    nk_ui_progress_bar(ui, FLOW_ID_BAR, inner, model->built ? 1.0f : model->fraction,
                       model->now_s);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                     NK_UI_SPACE_4);
        row.align_y = NK_UI_ALIGN_CENTER;
        if (nk_ui_begin_container(ui, FLOW_ID_STATUS, &row, NULL)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text(ui, FLOW_ID_MESSAGE,
                       nk_ui_text_clamp(ui, model->built
                           ? "Package written. Checking it before PLAY is offered."
                           : safe_text(model->message, "Building the package..."),
                           &text, inner - 140.0f, 2), &text);
            nk_ui_spacer(ui, FLOW_ID_STATUS_SPACER, sizing_grow(), sizing_fixed(1.0f));
            (void)snprintf(elapsed, sizeof(elapsed), "%u.%u s",
                           model->elapsed_ms / 1000u, (model->elapsed_ms % 1000u) / 100u);
            nk_ui_text_role(ui, FLOW_ID_ELAPSED, elapsed, NK_UI_TYPE_DATA, theme->muted);
            nk_ui_end_container(ui);
        }
    }
    if (!model->built && model->notice != NULL && model->notice[0] != '\0') {
        /* A calm note that other apps may feel slower: muted body text, no warning
           colour or mark, because nothing is wrong. */
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
        nk_ui_text(ui, FLOW_ID_NOTICE, nk_ui_text_clamp(ui, model->notice, &text, inner, 2),
                   &text);
    }
    {
        NkUiLayout log = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fixed(104.0f),
                                     2.0f);
        NkUiStyle style = scr_flow_box_style(theme, 0x080B0FFFu);
        log.padding_top = log.padding_bottom = 10.0f;
        log.padding_left = log.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_LOG, &log, &style)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
            text.line_height = 20.0f;
            for (size_t i = 0u; i < model->line_count && i < 4u; ++i) {
                nk_ui_text(ui, (NkUiId)(FLOW_ID_LOG + 1u + (NkUiId)i),
                           nk_ui_text_clamp(ui, safe_text(model->lines[i], " "), &text,
                                            inner - 28.0f, 1), &text);
            }
            if (model->line_count == 0u) {
                nk_ui_text(ui, FLOW_ID_LOG + 8u, "Waiting for the first build output...", &text);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_BUILD_ACTION,
                             model->built ? "Back to library" : "Cancel build",
                             model->built ? NK_UI_BUTTON_PRIMARY : NK_UI_BUTTON_DANGER)) {
                result = action(model->built ? PLAYER_UI_ACTION_LEAVE_BUILD
                                             : PLAYER_UI_ACTION_CANCEL_BUILD,
                                0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, "The game is translated and compiled on this PC. The first build takes a while.",
             model->built ? prompts_done : prompts_running, 2u);
    return result;
}
