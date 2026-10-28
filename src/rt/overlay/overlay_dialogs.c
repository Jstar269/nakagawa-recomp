/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The overlay's dialogs, in the error-dialog form (docs/PLAYER_UI_DESIGN.md, Error
   dialogs): a status eyebrow, a title that says what happened, what to do, and the
   buttons with the recommended action first. Quit asks before ending the run and
   refuses while the game is saving; a lost controller pauses the game and says so. */

#include "nk_overlay.h"
#include "nk_ui_widgets.h"

#include <stdio.h>
#include <string.h>

#define DIALOG_ID_EYEBROW 0x70004001u
#define DIALOG_ID_TITLE 0x70004002u
#define DIALOG_ID_BODY 0x70004003u
#define DIALOG_ID_ACTIONS 0x70004004u

#define DIALOG_MARK_TRIANGLE "\xE2\x96\xB2"
#define DIALOG_MARK_DOT "\xE2\x97\x8F"

static const NkUiPromptItem k_dialog_prompts[] = {
    {NK_UI_PROMPT_ACT_MOVE, "Move"},
    {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
    {NK_UI_PROMPT_ACT_BACK, "Back"}
};

static void dialog_text(NkUi *ui, const char *title, const char *body, float inner) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle title_style = nk_ui_type(NK_UI_TYPE_TITLE, theme->paper);
    NkUiTextStyle body_style = nk_ui_type(NK_UI_TYPE_BODY, theme->muted);
    title_style.wrap = true;
    body_style.wrap = true;
    nk_ui_text(ui, DIALOG_ID_TITLE, nk_ui_text_clamp(ui, title, &title_style, inner, 2),
               &title_style);
    nk_ui_text(ui, DIALOG_ID_BODY, body, &body_style);
}

static bool actions_begin(NkUi *ui) {
    NkUiLayout row = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fit(), nk_overlay_fit(),
                                       NK_UI_SPACE_3);
    row.padding_top = NK_UI_SPACE_1;
    return nk_ui_begin_container(ui, DIALOG_ID_ACTIONS, &row, NULL);
}

NkOverlayAction nk_overlay_page_quit(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    float inner = nk_overlay_page_begin(ui, model, 560.0f);
    bool go_back = false;
    if (inner <= 0.0f) return result;
    if (model->saving) {
        /* Ending the run now would cut the save off part-way. */
        nk_overlay_eyebrow(ui, DIALOG_ID_EYEBROW, DIALOG_MARK_TRIANGLE, "Saving", theme->warning);
        dialog_text(ui, "The game is saving",
                    "Resume to let it finish, then quit from the pause menu.", inner);
        if (actions_begin(ui)) {
            if (nk_ui_button(ui, NK_OVERLAY_ID_RESUME, "Resume", NK_UI_BUTTON_PRIMARY)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_RESUME, 0);
            }
            if (nk_ui_button(ui, NK_OVERLAY_ID_QUIT_STAY, "Back", 0u)) go_back = true;
            nk_ui_end_container(ui);
        }
    } else {
        char title[192];
        if (model->title_name != NULL && model->title_name[0] != '\0') {
            (void)snprintf(title, sizeof(title), "Quit %s?", model->title_name);
        } else {
            (void)snprintf(title, sizeof(title), "Quit the game?");
        }
        nk_overlay_eyebrow(ui, DIALOG_ID_EYEBROW, DIALOG_MARK_TRIANGLE, "Quit to library",
                           theme->warning);
        dialog_text(ui, title, "Anything the game has not saved is lost.", inner);
        if (actions_begin(ui)) {
            if (nk_ui_button(ui, NK_OVERLAY_ID_QUIT_STAY, "Keep playing", NK_UI_BUTTON_PRIMARY)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_RESUME, 0);
            }
            if (nk_ui_button(ui, NK_OVERLAY_ID_QUIT_CONFIRM, "Quit to library",
                             NK_UI_BUTTON_DANGER | NK_UI_BUTTON_NOT_DEFAULT)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_QUIT, 0);
            }
            nk_ui_end_container(ui);
        }
    }
    if (go_back || (result.kind == NK_OVERLAY_ACT_NONE &&
                    nk_ui_take_event(ui, NK_UI_EV_BACK, NULL))) {
        nk_overlay_go(state, NK_OVERLAY_SCREEN_MENU, NK_OVERLAY_ID_QUIT);
    }
    nk_overlay_page_end(ui, "", k_dialog_prompts,
                        sizeof(k_dialog_prompts) / sizeof(k_dialog_prompts[0]));
    return result;
}

NkOverlayAction nk_overlay_page_pad_lost(NkUi *ui, NkOverlayState *state,
                                         const NkOverlayModel *model) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    float inner = nk_overlay_page_begin(ui, model, 560.0f);
    if (inner <= 0.0f) return result;
    if (model->pad_connected) {
        nk_overlay_eyebrow(ui, DIALOG_ID_EYEBROW, DIALOG_MARK_DOT,
                           "Controller connected \xC2\xB7 Game paused", theme->ready);
        dialog_text(ui, "Controller connected", "Resume when you are ready.", inner);
    } else {
        nk_overlay_eyebrow(ui, DIALOG_ID_EYEBROW, DIALOG_MARK_TRIANGLE,
                           "Controller disconnected \xC2\xB7 Game paused", theme->warning);
        dialog_text(ui, "Your controller disconnected",
                    "Reconnect it, or carry on with the keyboard. The game stays paused "
                    "until you resume.", inner);
    }
    if (actions_begin(ui)) {
        if (nk_ui_button(ui, NK_OVERLAY_ID_PAD_RESUME, "Resume", NK_UI_BUTTON_PRIMARY)) {
            result = nk_overlay_action(NK_OVERLAY_ACT_RESUME, 0);
        }
        if (nk_ui_button(ui, NK_OVERLAY_ID_PAD_MENU, "Pause menu", 0u)) {
            nk_overlay_go(state, NK_OVERLAY_SCREEN_MENU, 0u);
        }
        nk_ui_end_container(ui);
    }
    /* Back keeps the game paused: it goes to the menu, never into the game. */
    if (result.kind == NK_OVERLAY_ACT_NONE && nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        nk_overlay_go(state, NK_OVERLAY_SCREEN_MENU, 0u);
    }
    nk_overlay_page_end(ui, "", k_dialog_prompts,
                        sizeof(k_dialog_prompts) / sizeof(k_dialog_prompts[0]));
    return result;
}
