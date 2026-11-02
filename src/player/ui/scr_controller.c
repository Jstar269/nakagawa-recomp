/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Controller settings: the PSP buttons in two columns of named rows, each binding on the
   button that rebinds it, beside the stick and trigger card with the live input. The
   mapping being edited is named on the buttons card, so the scope is stated, never
   implied (#520). CONTROLLER and KEYBOARD above the cards choose whose bindings the rows
   show; for the keyboard the stick card holds the stick's four keys and names the keys
   Nakagawa keeps, for the controller it chooses the stick (#1299). Every change is saved
   as it is made. While a guided calibration runs, its step replaces the cards. Under a
   top bar with SETTINGS active, as the Settings page. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>

#define PAD_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_CONTROLS, (offset))
#define PAD_ID_ROOT PAD_ID(0x000001u)
#define PAD_ID_TOP PAD_ID(0x000010u)
#define PAD_ID_BODY PAD_ID(0x000020u)
#define PAD_ID_COLUMN PAD_ID(0x000021u)
#define PAD_ID_HEAD PAD_ID(0x000022u)
#define PAD_ID_CARDS PAD_ID(0x000023u)
#define PAD_ID_BUTTONS PAD_ID(0x000024u)
#define PAD_ID_STICK PAD_ID(0x000025u)
#define PAD_ID_ACTIONS PAD_ID(0x000026u)
#define PAD_ID_GRID PAD_ID(0x000027u)
#define PAD_ID_GRID_SPAN 0x4u
#define PAD_ID_LIVE PAD_ID(0x000030u)
#define PAD_ID_SPACER PAD_ID(0x000031u)
#define PAD_ID_DEVICE PAD_ID(0x000032u)
#define PAD_ID_DEVICE_BUTTONS PAD_ID(0x000033u)
#define PAD_ID_PROMPTS PAD_ID(0x000040u)
#define PAD_ID_WAVES PAD_ID(0x000050u)
#define PAD_ID_ROWS PAD_ID(0x000100u)         /* + control * 4, + 0-2: a binding row's parts */
#define PAD_ID_ROWS_SPAN 0x80u
#define PAD_ID_GLYPH_CELLS PAD_ID(0x000200u)  /* + control: the cell a binding's glyph sits in */
#define PAD_ID_GLYPH_CELLS_SPAN 0x20u
#define PAD_ID_STEPPERS PAD_ID(0x000280u)     /* + k * 4, + 0-2: a stick card row's parts */
#define PAD_ID_STEPPERS_SPAN 0x10u
#define PAD_ID_TEXT PAD_ID(0x001000u)
#define PAD_ID_TEXT_SPAN 0x1000u

static PlayerUiAction controller_op(PlayerUiControllerOp op, size_t control) {
    PlayerUiAction result;
    result.kind = PLAYER_UI_ACTION_CONTROLLER;
    result.primary = (PlayerUiPrimaryAction)op;
    result.title_index = control;
    return result;
}

/* A label cut to the room a small button of `width` has for it. */
static const char *controller_button_label(NkUi *ui, const char *label, float width) {
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_LABEL_S, 0u);
    return nk_ui_text_clamp(ui, nk_ui_caps(ui, label), &text,
                            width - 2.0f * NK_UI_SPACE_3 - 2.0f, 1);
}

/* A binding's glyph, centred in a cell as wide as the widest one (CREATE, six letters
   on a pill, is 43.6 px at 20 px tall), so the buttons after it line up. */
#define PAD_GLYPH_HEIGHT 20.0f
#define PAD_GLYPH_CELL 44.0f
static void controller_glyph(NkUi *ui, const PlayerUiControllerModel *model, size_t control) {
    const PlayerUiBindingGlyph *glyph = &model->glyphs[control];
    NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(PAD_GLYPH_CELL), sizing_fit(),
                                  0.0f);
    cell.align_x = NK_UI_ALIGN_CENTER;
    cell.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, (NkUiId)(PAD_ID_GLYPH_CELLS + (NkUiId)control), &cell,
                               NULL)) {
        return;
    }
    if (glyph->shown) {
        (void)nk_ui_prompt_glyph_in(ui, PLAYER_UI_ID_CONTROLLER_GLYPH(control), model->glyph_style,
                                    glyph->action, glyph->label, PAD_GLYPH_HEIGHT);
    }
    nk_ui_end_container(ui);
}

/* One PSP control: its name, the glyph of what it is bound to, then its binding in
   words on the button that rebinds it. The control the status note names (it gave
   its input to the last binding) has its name in the note's colour. */
static void controller_row(NkUi *ui, const PlayerUiControllerModel *model, size_t control,
                           const char *name, float name_width, float button_width,
                           PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_ROWS + (NkUiId)control * 4u);
    bool capturing = model->capturing == (int)control;
    bool noted = model->noted == (int)control;
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S,
                                    model->conflicted[control] || (noted && model->status_error)
                                        ? theme->error
                                        : noted ? theme->accent_dark : theme->paper);
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(name_width), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u,
                       nk_ui_text_clamp(ui, safe_text(name, "Button"), &text, name_width, 1),
                       &text);
            nk_ui_end_container(ui);
        }
    }
    controller_glyph(ui, model, control);
    if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_BUTTON(control),
                     controller_button_label(ui, capturing ? (model->keyboard ? "Press a key..."
                                                                              : "Press a button...")
                                                           : safe_text(model->bindings[control],
                                                                       "None"),
                                             button_width),
                     NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW |
                         (capturing ? NK_UI_BUTTON_PRIMARY : 0u))) {
        *result = controller_op(PLAYER_UI_CONTROLLER_REBIND, control);
    }
    nk_ui_end_container(ui);
}

/* A value in words with the - and + steps that change it. */
static void controller_stepper(NkUi *ui, uint32_t k, const char *value, float width,
                               NkUiId down, NkUiId up, PlayerUiControllerOp down_op,
                               PlayerUiControllerOp up_op, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_STEPPERS + k * 4u);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        /* Each step is its sign and the small button's padding. */
        float text_width = width - 2.0f * (2.0f * NK_UI_SPACE_3 + 10.0f) - 2.0f * NK_UI_SPACE_2;
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), 0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u, nk_ui_text_clamp(ui, safe_text(value, ""), &text,
                                                       text_width, 1), &text);
            nk_ui_end_container(ui);
        }
    }
    if (nk_ui_button(ui, down, "-", NK_UI_BUTTON_SMALL)) *result = controller_op(down_op, 0u);
    if (nk_ui_button(ui, up, "+", NK_UI_BUTTON_SMALL)) *result = controller_op(up_op, 0u);
    nk_ui_end_container(ui);
}

/* Which controller stick drives the PSP stick, named on the button that switches it:
   124 px, or as wide as its longer word at a larger text size. */
#define PAD_STICK_BUTTON_WIDTH 124.0f
static void controller_stick_choice(NkUi *ui, const PlayerUiControllerModel *model, float width,
                                    PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_STEPPERS + 2u * 4u);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
    float button = PAD_STICK_BUTTON_WIDTH;
    {
        /* controller_button_label keeps 2 px spare beside the padding. */
        float need = nk_ui_button_width(ui, "Right stick", NK_UI_BUTTON_SMALL) + 2.0f;
        if (need > button) button = need;
    }
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), 0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u,
                       nk_ui_text_clamp(ui, "PSP stick", &text, width - button - NK_UI_SPACE_2,
                                        1),
                       &text);
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(button), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 3u, &cell, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_STICK,
                             controller_button_label(ui, model->stick_right ? "Right stick"
                                                                            : "Left stick",
                                                     button),
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW)) {
                *result = controller_op(PLAYER_UI_CONTROLLER_STICK, 0u);
            }
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

static void controller_live(NkUi *ui, const PlayerUiControllerModel *model, float width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout lines = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 2.0f);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
    if (!nk_ui_begin_container(ui, PAD_ID_LIVE, &lines, NULL)) return;
    for (size_t i = 0u; i < model->live_count && i < PLAYER_UI_CONTROLLER_LIVE_LINES; ++i) {
        nk_ui_text(ui, (NkUiId)(PAD_ID_TEXT + 0x40u + (NkUiId)i),
                   nk_ui_text_clamp(ui, safe_text(model->live[i], " "), &text, width, 1), &text);
    }
    nk_ui_end_container(ui);
}

/* CONTROLLER and KEYBOARD: whose bindings the rows show, the one shown filled. The
   pair is 240 px, or as wide as a larger text size makes its two words. */
#define PAD_DEVICE_WIDTH 240.0f
static float controller_device_width(NkUi *ui) {
    /* controller_button_label keeps 2 px spare beside the padding. */
    float each = nk_ui_button_width(ui, "Controller", NK_UI_BUTTON_SMALL) + 2.0f;
    float pair = 2.0f * each + NK_UI_SPACE_2;
    return pair > PAD_DEVICE_WIDTH ? pair : PAD_DEVICE_WIDTH;
}

static void controller_device(NkUi *ui, const PlayerUiControllerModel *model,
                              PlayerUiAction *result) {
    float width = controller_device_width(ui);
    NkUiLayout pair = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(width), sizing_fit(),
                                  NK_UI_SPACE_2);
    float each = (width - NK_UI_SPACE_2) * 0.5f;
    uint32_t flags = NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW | NK_UI_BUTTON_ROW;
    if (!nk_ui_begin_container(ui, PAD_ID_DEVICE_BUTTONS, &pair, NULL)) return;
    if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SHOW_PAD,
                     controller_button_label(ui, "Controller", each),
                     flags | (model->keyboard ? 0u : NK_UI_BUTTON_PRIMARY))) {
        *result = controller_op(PLAYER_UI_CONTROLLER_SHOW_CONTROLLER, 0u);
    }
    if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SHOW_KEYBOARD,
                     controller_button_label(ui, "Keyboard", each),
                     flags | (model->keyboard ? NK_UI_BUTTON_PRIMARY : 0u))) {
        *result = controller_op(PLAYER_UI_CONTROLLER_SHOW_KEYBOARD, 0u);
    }
    nk_ui_end_container(ui);
}

/* For the keyboard, the stick card holds the stick's four keys, each rebound like a
   button's (#1299), then says which keys stay with Nakagawa. */
#define PAD_STICK_NAME_WIDTH 48.0f
static void controller_keyboard_card(NkUi *ui, const PlayerUiControllerModel *model, float card,
                                     PlayerUiAction *result) {
    static const char *const kept[] = {"Esc or F1 pauses the game.", "F3 shows the frame rate."};
    static const char *const directions[PLAYER_UI_CONTROLLER_STICK_KEYS] = {"Up", "Down", "Left",
                                                                            "Right"};
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
    float inner = card - 2.0f * NK_UI_SPACE_5;
    if (!nk_ui_card_begin(ui, PAD_ID_STICK, sizing_fixed(card), sizing_grow(), NK_UI_SPACE_2)) {
        return;
    }
    nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x21u, "Analog stick");
    for (size_t i = 0u; i < PLAYER_UI_CONTROLLER_STICK_KEYS; ++i) {
        controller_row(ui, model, PLAYER_UI_CONTROLLER_STICK_KEY + i, directions[i],
                       PAD_STICK_NAME_WIDTH,
                       inner - PAD_STICK_NAME_WIDTH - PAD_GLYPH_CELL - 2.0f * NK_UI_SPACE_2,
                       result);
    }
    nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x20u, "Keys Nakagawa keeps");
    for (size_t i = 0u; i < sizeof(kept) / sizeof(kept[0]); ++i) {
        nk_ui_text(ui, (NkUiId)(PAD_ID_TEXT + 0x50u + (NkUiId)i),
                   nk_ui_text_clamp(ui, kept[i], &text, inner, 2), &text);
    }
    nk_ui_end_container(ui);
}

static void controller_mapping(NkUi *ui, const PlayerUiControllerModel *model, float width,
                               float height, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool tall = height >= 680.0f;
    float column = width - 2.0f * (width < NK_UI_NARROW_WIDTH ? NK_UI_GUTTER_NARROW
                                                              : NK_UI_GUTTER);
    float stick_card;
    float buttons_inner;
    float half;
    float name_width;
    char eyebrow[160];
    NkUiLayout layout;
    if (column > 1120.0f) column = 1120.0f;
    stick_card = column >= 1000.0f ? 320.0f : 260.0f;
    /* Cards pad 20 px either side; the two columns of rows have 16 px between. */
    buttons_inner = column - NK_UI_SPACE_4 - stick_card - 2.0f * NK_UI_SPACE_5;
    half = (buttons_inner - NK_UI_SPACE_4) * 0.5f;
    /* The longest name, "L Trigger (L1)", needs about 100 px; the rest of the row is
       the binding's glyph and its words. */
    name_width = half * 0.36f;
    if (name_width > 104.0f) name_width = 104.0f;
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column), sizing_fit(),
                         tall ? NK_UI_SPACE_4 : NK_UI_SPACE_2);
    if (!nk_ui_begin_container(ui, PAD_ID_COLUMN, &layout, NULL)) return;
    {
        NkUiLayout head = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                      NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, PAD_ID_HEAD, &head, NULL)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S,
                                            model->status_error ? theme->error : theme->muted);
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.align_y = NK_UI_ALIGN_CENTER;
            /* The SETTINGS tab names the area, so a short window drops the
               heading to keep every row on screen. */
            if (tall) {
                nk_ui_text_role(ui, PAD_ID_TEXT + 0u, "CONTROLS", NK_UI_TYPE_DISPLAY,
                                theme->paper);
            }
            if (nk_ui_begin_container(ui, PAD_ID_DEVICE, &row, NULL)) {
                controller_device(ui, model, result);
                nk_ui_text(ui, PAD_ID_TEXT + 1u,
                           nk_ui_text_clamp(ui, safe_text(model->status,
                                                          "Choose a button to change it, then "
                                                          "press the controller button to use."),
                                            &text,
                                            column - controller_device_width(ui) - NK_UI_SPACE_3,
                                            1),
                           &text);
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
    if (nk_ui_begin_container(ui, PAD_ID_CARDS, &layout, NULL)) {
        if (nk_ui_card_begin(ui, PAD_ID_BUTTONS, sizing_grow(), sizing_grow(), NK_UI_SPACE_2)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_EYEBROW,
                                            model->scope_title ? theme->ready : theme->muted);
            (void)snprintf(eyebrow, sizeof(eyebrow), "%s" FLOW_MIDDOT "%s",
                           model->keyboard ? "Keys" : "Buttons",
                           safe_text(model->scope, "global mapping"));
            nk_ui_text(ui, PAD_ID_TEXT + 0x10u,
                       nk_ui_text_clamp(ui, nk_ui_caps(ui, eyebrow), &text, buttons_inner, 1),
                       &text);
            layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
            if (nk_ui_begin_container(ui, PAD_ID_GRID, &layout, NULL)) {
                for (size_t col = 0u; col < 2u; ++col) {
                    NkUiLayout list = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(half),
                                                  sizing_fit(),
                                                  tall ? NK_UI_SPACE_2 : NK_UI_SPACE_1);
                    if (!nk_ui_begin_container(ui, (NkUiId)(PAD_ID_GRID + 1u + (NkUiId)col),
                                               &list, NULL)) {
                        continue;
                    }
                    for (size_t row = 0u; row < PLAYER_UI_CONTROLLER_BUTTONS / 2u; ++row) {
                        size_t control = col * (PLAYER_UI_CONTROLLER_BUTTONS / 2u) + row;
                        controller_row(ui, model, control, model->names[control], name_width,
                                       half - name_width - PAD_GLYPH_CELL - 2.0f * NK_UI_SPACE_2,
                                       result);
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
        if (model->keyboard) {
            controller_keyboard_card(ui, model, stick_card, result);
        } else if (nk_ui_card_begin(ui, PAD_ID_STICK, sizing_fixed(stick_card), sizing_grow(),
                                    NK_UI_SPACE_2)) {
            float inner = stick_card - 2.0f * NK_UI_SPACE_5;
            nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x20u, "Stick and triggers");
            controller_stick_choice(ui, model, inner, result);
            controller_stepper(ui, 0u, model->deadzone, inner,
                               PLAYER_UI_ID_CONTROLLER_DEADZONE_DOWN,
                               PLAYER_UI_ID_CONTROLLER_DEADZONE_UP,
                               PLAYER_UI_CONTROLLER_DEADZONE_DOWN,
                               PLAYER_UI_CONTROLLER_DEADZONE_UP, result);
            controller_stepper(ui, 1u, model->trigger, inner,
                               PLAYER_UI_ID_CONTROLLER_TRIGGER_DOWN,
                               PLAYER_UI_ID_CONTROLLER_TRIGGER_UP,
                               PLAYER_UI_CONTROLLER_TRIGGER_DOWN,
                               PLAYER_UI_CONTROLLER_TRIGGER_UP, result);
            if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_CALIBRATE, "Calibrate stick",
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW)) {
                *result = controller_op(PLAYER_UI_CONTROLLER_CALIBRATE, 0u);
            }
            nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x21u, "Live input");
            controller_live(ui, model, inner);
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    /* Back sits at the right end, under the stick card, so Down from Calibrate
       reaches it rather than a binding in the grid; Left and Right stay in the row. */
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
    if (nk_ui_begin_container(ui, PAD_ID_ACTIONS, &layout, NULL)) {
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_RESET, "Reset to defaults",
                         NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_RESET, 0u);
        }
        /* Global or this game's own, offered only with a game selected (#520). */
        if (model->offer_scope &&
            nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SCOPE,
                         model->scope_title ? "Use the global mapping"
                                            : "Use this game's own mapping",
                         NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_TOGGLE_SCOPE, 0u);
        }
        nk_ui_spacer(ui, PAD_ID_SPACER, sizing_grow(), sizing_fixed(1.0f));
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_BACK, "Back", NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_BACK, 0u);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

/* A calibration step: what to do, its numbers, and its buttons. */
static void controller_calibration(NkUi *ui, const PlayerUiControllerModel *model, float width,
                                   PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const char *step;
    const char *title;
    const char *lead;
    const char *primary = NULL;
    PlayerUiControllerOp primary_op = PLAYER_UI_CONTROLLER_NONE;
    float card = width - 2.0f * NK_UI_GUTTER_NARROW;
    float inner;
    NkUiLayout row;
    if (card > 720.0f) card = 720.0f;
    inner = card - 56.0f;
    switch (model->page) {
        case PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST:
            step = "Step 1 of 3: rest";
            title = "Leave the stick and triggers alone";
            lead = "Don't touch the stick or the triggers for about a second while Nakagawa "
                   "reads where they rest.";
            break;
        case PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_EXTREMES:
            step = "Step 2 of 3: full range";
            title = "Move the stick and press the triggers";
            lead = "Turn the stick round its edge a few times and press both triggers all the "
                   "way, then choose Finish.";
            primary = "Finish";
            primary_op = PLAYER_UI_CONTROLLER_CALIBRATION_FINISH;
            break;
        case PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_RESULT:
        default:
            step = "Step 3 of 3: check";
            title = "Use this calibration?";
            lead = "These are the resting points and ranges Nakagawa read. Accept uses them for "
                   "the mapping you are editing.";
            primary = "Accept";
            primary_op = PLAYER_UI_CONTROLLER_CALIBRATION_ACCEPT;
            break;
    }
    if (!nk_ui_dialog_begin(ui, FLOW_ID_CARD, card)) return;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, step, theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, title, NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY, theme->muted);
        nk_ui_text(ui, FLOW_ID_LEAD, nk_ui_text_clamp(ui, lead, &text, inner, 3), &text);
    }
    if (model->page == PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST) {
        nk_ui_progress_bar(ui, FLOW_ID_BAR, inner, model->calibration_fraction, 0.0);
    }
    if (scr_wizard_box_begin(ui, 0u)) {
        controller_live(ui, model, inner - 32.0f);
        nk_ui_end_container(ui);
    }
    row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
    row.padding_top = NK_UI_SPACE_1;
    if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
        if (primary != NULL &&
            nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY, primary,
                         NK_UI_BUTTON_PRIMARY)) {
            *result = controller_op(primary_op, 0u);
        }
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL, "Cancel",
                         primary == NULL ? NK_UI_BUTTON_PRIMARY : 0u)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_CALIBRATION_CANCEL, 0u);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

PlayerUiAction player_ui_screen_controller(NkUi *ui, const PlayerUiControllerModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Change"},
        {NK_UI_PROMPT_ACT_BACK, "Back to settings"}
    };
    static const NkUiPromptItem prompts_calibrating[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Cancel"}
    };
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    bool calibrating;
    NkUiLayout layout;
    float width;
    float height;
    if (ui == NULL || model == NULL) return result;
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    calibrating = model->page != PLAYER_UI_CONTROLLER_PAGE_MAPPING;
    (void)nk_ui_waves_element(ui, PAD_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, PAD_ID_ROOT, &layout, NULL)) return result;
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, PAD_ID_TOP, PLAYER_UI_ROUTE_SETTINGS);
        if (tab != PLAYER_UI_ACTION_NONE) result = action(tab, 0u, PLAYER_UI_PRIMARY_NONE);
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    if (calibrating) {
        layout.align_y = NK_UI_ALIGN_CENTER;
        layout.padding_top = layout.padding_bottom = NK_UI_SPACE_4;
    } else {
        layout.padding_top = height >= 680.0f ? NK_UI_SPACE_7 : NK_UI_SPACE_4;
    }
    if (nk_ui_begin_container(ui, PAD_ID_BODY, &layout, NULL)) {
        if (calibrating) controller_calibration(ui, model, width, &result);
        else controller_mapping(ui, model, width, height, &result);
        nk_ui_end_container(ui);
    }
    nk_ui_prompt_bar(ui, PAD_ID_PROMPTS, "", calibrating ? prompts_calibrating : prompts,
                     calibrating ? 2u : 3u);
    nk_ui_end_container(ui);
    return result;
}
