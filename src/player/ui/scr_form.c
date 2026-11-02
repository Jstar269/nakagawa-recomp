/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The rows and boxes the setup wizard and Settings share: a dark inset panel, a
   setting's "Name: value" button with the sentence beside it, and the check
   columns. They draw with the setup wizard's ids (the WIZARD screen byte in
   nk_ui.h), so the panel keeps its id wherever it is used. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIZ_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_WIZARD, (offset))
#define WIZ_ID_BOX WIZ_ID(0x000010u)

/* The width a column of check rows takes: its design width, or the widest of
   its entries (the status words in caps, or the names) in `role` when a larger
   text size makes one wider. */
float scr_check_column_width(NkUi *ui, const PlayerUiCheckRow *checks, size_t count,
                                bool status, NkUiTypeRole role, const char *fallback,
                                float design) {
    NkUiTextStyle style = nk_ui_type(role, 0u);
    float width = design;
    for (size_t i = 0u; checks != NULL && i < count; ++i) {
        const char *text = status ? nk_ui_caps(ui, safe_text(checks[i].status, fallback))
                                  : safe_text(checks[i].name, fallback);
        float need = ceilf(nk_ui_text_width(ui, text, &style));
        if (need > width) width = need;
    }
    return width;
}

/* A dark inset panel for the step's facts. Returns true when opened. */
bool scr_wizard_box_begin(NkUi *ui, uint32_t edge) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 6.0f);
    NkUiStyle style = scr_flow_box_style(theme, theme->surface);
    if (edge != 0u) style.border_rgba = edge;
    box.padding_top = box.padding_bottom = 14.0f;
    box.padding_left = box.padding_right = 16.0f;
    return nk_ui_begin_container(ui, WIZ_ID_BOX, &box, &style);
}

/* The button column of a setting row, and the least room its sentence keeps
   beside it before the sentences move under the buttons. */
#define WIZARD_VALUE_CELL 220.0f
#define WIZARD_VALUE_SENTENCE_MIN 260.0f

/* Whether nk_ui_text_clamp cut `text` short to give `shown`. */
static bool text_was_cut(const char *shown, const char *text) {
    static const char ellipsis[] = "\xE2\x80\xA6";
    size_t n = sizeof(ellipsis) - 1u;
    size_t shown_length = strlen(shown);
    size_t text_length = strlen(text);
    return shown_length >= n && memcmp(shown + shown_length - n, ellipsis, n) == 0 &&
           !(text_length >= n && memcmp(text + text_length - n, ellipsis, n) == 0);
}

/* The column a group of rows `width` px across shares: the design's 220 px, or
   as wide as the widest label when that is wider, at every text size (a label
   such as "Interface size: Automatic" outgrew 220 px in the real font and was
   cut). The buttons line up, and when the column leaves a sentence too little
   room beside its button, or would cut one short of its two lines, every
   sentence goes under its button at the full width. */
WizardColumn scr_wizard_column(NkUi *ui, const WizardRow *rows, size_t count, float width) {
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    WizardColumn column = {WIZARD_VALUE_CELL, false};
    char label[48];
    float room;
    for (size_t i = 0u; i < count; ++i) {
        float need;
        (void)snprintf(label, sizeof(label), "%s: %s", rows[i].name, rows[i].value);
        need = nk_ui_button_width(ui, label, 0u);
        if (need > column.cell) column.cell = need;
    }
    /* At the design text size the sentences stay beside their buttons, as the
       design lays the page out; only larger type stacks them. */
    if (!(nk_ui_text_scale(ui) > 1.0f)) {
        return column;
    }
    room = width - column.cell - NK_UI_SPACE_4;
    column.stacked = room < WIZARD_VALUE_SENTENCE_MIN;
    for (size_t i = 0u; i < count && !column.stacked; ++i) {
        column.stacked = text_was_cut(nk_ui_text_clamp(ui, rows[i].sentence, &text, room, 2),
                                      rows[i].sentence);
    }
    return column;
}

/* A setting's button, "Name: value", with the sentence that explains it beside
   it (or under it, as `column` says; NULL lays the row out on its own); the
   button is filled while the setting is in effect. Returns true when the
   button is activated. */

bool scr_wizard_value(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                         const char *value, const char *sentence, bool on, float width,
                         const WizardColumn *column) {
    return scr_wizard_value_state(ui, id, text_id, name, value, sentence, on, false, width, column);
}

/* As scr_wizard_value; a disabled row cannot be focused and its words are dim, sentence included. */
bool scr_wizard_value_state(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                               const char *value, const char *sentence, bool on, bool disabled,
                               float width, const WizardColumn *column) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, disabled ? theme->dim : theme->muted);
    WizardColumn own;
    char label[48];
    float room;
    bool pressed = false;
    if (column == NULL) {
        WizardRow self = {name, value, sentence};
        own = scr_wizard_column(ui, &self, 1u, width);
        column = &own;
    }
    (void)snprintf(label, sizeof(label), "%s: %s", name, value);
    room = width - column->cell - NK_UI_SPACE_4;
    if (column->stacked) {
        /* The sentence sits close under its own button, and the next row's
           button further off, so each pair reads as one. */
        row = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), NK_UI_SPACE_1);
        row.padding_bottom = NK_UI_SPACE_3;
        room = width;
    }
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, text_id, &row, NULL)) return false;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(column->cell), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, text_id + 1u, &cell, NULL)) {
            pressed = nk_ui_button(ui, id, label,
                                   NK_UI_BUTTON_GROW | (on && !disabled ? NK_UI_BUTTON_PRIMARY : 0u) |
                                       (disabled ? NK_UI_BUTTON_DISABLED : 0u));
            nk_ui_end_container(ui);
        }
    }
    nk_ui_text(ui, text_id + 2u, nk_ui_text_clamp(ui, sentence, &text, room, 2), &text);
    nk_ui_end_container(ui);
    return pressed;
}

/* One option switch: a button reading "Name: On" or "Name: Off" (accent while
   on) with its sentence beside it. Returns true when activated. */
bool scr_wizard_switch(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                          const char *sentence, bool on, float width,
                          const WizardColumn *column) {
    return scr_wizard_value(ui, id, text_id, name, on ? "On" : "Off", sentence, on, width, column);
}

WizardColumn scr_online_developer_column(NkUi *ui, bool online, bool developer, float width) {
    const WizardRow rows[] = {
        {"Online mode", online ? "On" : "Off", WIZARD_ONLINE_SENTENCE},
        {"Developer mode", developer ? "On" : "Off", WIZARD_DEVELOPER_SENTENCE}
    };
    return scr_wizard_column(ui, rows, sizeof(rows) / sizeof(rows[0]), width);
}
