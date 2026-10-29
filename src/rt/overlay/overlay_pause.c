/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The pause menu and its controls page. The menu is one card: GAME PAUSED, the
   game's name, then RESUME (the default) over SETTINGS, CONTROLS and QUIT TO
   LIBRARY, with VRAM VIEWER and PROFILE before QUIT in developer mode. Back resumes.
   The controls page lists what drives each PSP control, read from the bindings the game
   is using. */

#include "nk_overlay.h"
#include "nk_ui_widgets.h"

#include <string.h>

#define MENU_ID_EYEBROW 0x70001001u
#define MENU_ID_TITLE 0x70001002u
#define MENU_ID_DISC 0x70001003u
#define MENU_ID_ACTIONS 0x70001004u
#define MENU_ID_HEAD 0x70001005u

#define CONTROLS_ID_EYEBROW 0x70003001u
#define CONTROLS_ID_TITLE 0x70003002u
#define CONTROLS_ID_TABLES 0x70003003u
#define CONTROLS_ID_TABLE 0x70003010u    /* + 0x100 per table */
#define CONTROLS_ID_NOTE 0x70003004u
#define CONTROLS_ID_FOOT 0x70003005u

#define OVERLAY_DASH "\xE2\x80\x94"

static const char *nonempty(const char *text) {
    return text != NULL && text[0] != '\0' ? text : NULL;
}

NkOverlayAction nk_overlay_page_menu(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"},
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Resume"}
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    float inner = nk_overlay_page_begin(ui, model, 480.0f);
    if (inner <= 0.0f) return result;
    {
        NkUiLayout head = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                            nk_overlay_fit(), NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, MENU_ID_HEAD, &head, NULL)) {
            nk_overlay_eyebrow(ui, MENU_ID_EYEBROW, NULL, "Game paused", theme->muted);
            if (nonempty(model->title_name) != NULL) {
                NkUiTextStyle title = nk_ui_type(NK_UI_TYPE_TITLE, theme->paper);
                title.wrap = true;
                nk_ui_text(ui, MENU_ID_TITLE,
                           nk_ui_text_clamp(ui, model->title_name, &title, inner, 2), &title);
            } else {
                nk_ui_text_role(ui, MENU_ID_TITLE, "Paused", NK_UI_TYPE_TITLE, theme->paper);
            }
            if (nonempty(model->disc_id) != NULL) {
                nk_ui_text_role(ui, MENU_ID_DISC, model->disc_id, NK_UI_TYPE_DATA, theme->muted);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout column = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                              nk_overlay_fit(), NK_UI_SPACE_3);
        column.padding_top = NK_UI_SPACE_2;
        if (nk_ui_begin_container(ui, MENU_ID_ACTIONS, &column, NULL)) {
            if (nk_ui_button(ui, NK_OVERLAY_ID_RESUME, "Resume",
                             NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_LARGE | NK_UI_BUTTON_GROW)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_RESUME, 0);
            }
            if (nk_ui_button(ui, NK_OVERLAY_ID_SETTINGS, "Settings", NK_UI_BUTTON_GROW)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_SETTINGS, 0u);
            }
            if (nk_ui_button(ui, NK_OVERLAY_ID_CONTROLS, "Controls", NK_UI_BUTTON_GROW)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_CONTROLS, 0u);
            }
            if (model->developer &&
                nk_ui_button(ui, NK_OVERLAY_ID_VRAM, "VRAM viewer", NK_UI_BUTTON_GROW)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_VRAM, 0u);
            }
            if (model->developer &&
                nk_ui_button(ui, NK_OVERLAY_ID_PROFILE, "Profile", NK_UI_BUTTON_GROW)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_PROFILE, 0u);
            }
            if (nk_ui_button(ui, NK_OVERLAY_ID_QUIT, "Quit to library", NK_UI_BUTTON_GROW)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_QUIT, 0u);
            }
            nk_ui_end_container(ui);
        }
    }
    if (result.kind == NK_OVERLAY_ACT_NONE && nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        result = nk_overlay_action(NK_OVERLAY_ACT_RESUME, 0);
    }
    nk_overlay_page_end(ui, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    return result;
}

/* One mapping table: a header row and one row per control, hairlines between. */
static void controls_table(NkUi *ui, NkUiId id, const NkOverlayControlRow *rows, size_t count,
                           float row_height, NkUiSizing width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout table = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, width, nk_overlay_fit(), 0.0f);
    static const char *const heads[3] = {"PSP", "Keyboard", "Controller"};
    size_t i;
    if (!nk_ui_begin_container(ui, id, &table, NULL)) return;
    for (i = 0; i <= count; ++i) {
        NkUiId row_id = (NkUiId)(id + 1u + (NkUiId)i * 8u);
        NkUiLayout row = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                           nk_overlay_fixed(i == 0u ? 30.0f : row_height),
                                           NK_UI_SPACE_3);
        const char *cells[3];
        int c;
        row.align_y = NK_UI_ALIGN_CENTER;
        if (i == 0u) {
            cells[0] = heads[0];
            cells[1] = heads[1];
            cells[2] = heads[2];
        } else {
            const NkOverlayControlRow *r = &rows[i - 1u];
            cells[0] = nonempty(r->psp) != NULL ? r->psp : OVERLAY_DASH;
            cells[1] = nonempty(r->keyboard) != NULL ? r->keyboard : OVERLAY_DASH;
            cells[2] = nonempty(r->controller) != NULL ? r->controller : OVERLAY_DASH;
            nk_ui_divider(ui, (NkUiId)(row_id + 7u), true);
        }
        if (!nk_ui_begin_container(ui, row_id, &row, NULL)) continue;
        for (c = 0; c < 3; ++c) {
            NkUiLayout cell = nk_overlay_layout(NK_UI_LAYOUT_ROW,
                                                c == 0 ? nk_overlay_fixed(96.0f)
                                                       : nk_overlay_grow(),
                                                nk_overlay_fit(), 0.0f);
            if (!nk_ui_begin_container(ui, (NkUiId)(row_id + 1u + (NkUiId)c), &cell, NULL)) {
                continue;
            }
            if (i == 0u) {
                nk_ui_eyebrow(ui, (NkUiId)(row_id + 4u + (NkUiId)c), cells[c]);
            } else if (c == 0) {
                nk_ui_text_role(ui, (NkUiId)(row_id + 4u), cells[0], NK_UI_TYPE_BODY_S,
                                theme->paper);
            } else {
                NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA,
                                                c == 1 ? theme->muted : theme->paper);
                nk_ui_text(ui, (NkUiId)(row_id + 4u + (NkUiId)c),
                           nk_ui_text_clamp(ui, cells[c], &data, 400.0f, 1), &data);
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

NkOverlayAction nk_overlay_page_controls(NkUi *ui, NkOverlayState *state,
                                         const NkOverlayModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    float height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    size_t count = model->controls != NULL ? model->control_count : 0u;
    /* Short windows put the list in two tables side by side so the card fits. */
    bool split = height < 720.0f && count > 6u;
    float row_height = split ? 34.0f : 38.0f;
    float inner;
    if (count > NK_OVERLAY_MAX_CONTROL_ROWS) count = NK_OVERLAY_MAX_CONTROL_ROWS;
    inner = nk_overlay_page_begin(ui, model, split ? 880.0f : 640.0f);
    if (inner <= 0.0f) return result;
    nk_overlay_eyebrow(ui, CONTROLS_ID_EYEBROW, NULL, "Game paused \xC2\xB7 Controls", theme->muted);
    nk_ui_text_role(ui, CONTROLS_ID_TITLE, "Controls", NK_UI_TYPE_TITLE, theme->paper);
    if (count == 0u) {
        nk_ui_text_role(ui, CONTROLS_ID_NOTE, "No controls are mapped for this game.",
                        NK_UI_TYPE_BODY, theme->muted);
    } else if (split) {
        NkUiLayout tables = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                              nk_overlay_fit(), NK_UI_SPACE_6);
        size_t first = (count + 1u) / 2u;
        if (nk_ui_begin_container(ui, CONTROLS_ID_TABLES, &tables, NULL)) {
            controls_table(ui, CONTROLS_ID_TABLE, model->controls, first, row_height,
                           nk_overlay_grow());
            controls_table(ui, CONTROLS_ID_TABLE + 0x100u, model->controls + first,
                           count - first, row_height, nk_overlay_grow());
            nk_ui_end_container(ui);
        }
    } else {
        controls_table(ui, CONTROLS_ID_TABLE, model->controls, count, row_height,
                       nk_overlay_grow());
    }
    {
        NkUiTextStyle foot = nk_ui_type(NK_UI_TYPE_FOOTNOTE, theme->dim);
        foot.wrap = true;
        nk_ui_text(ui, CONTROLS_ID_FOOT,
                   "Esc, F1 or the controller's Guide button opens this menu. Change the "
                   "controller buttons in the library's Settings.", &foot);
    }
    if (nk_ui_button(ui, NK_OVERLAY_ID_CONTROLS_BACK, "Back", 0u) ||
        nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        nk_overlay_go(state, NK_OVERLAY_SCREEN_MENU, NK_OVERLAY_ID_CONTROLS);
    }
    nk_overlay_page_end(ui, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    return result;
}
