/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The overlay's settings page: the two settings a person changes with the game in
   front of them, game scaling and the performance HUD. Each is a focusable row
   (docs/PLAYER_UI_DESIGN.md, Settings rows): scaling is a segmented control that
   left and right move along, the HUD a toggle. Changes apply at once and last until
   the game closes. */

#include "nk_overlay.h"
#include "nk_ui_widgets.h"

#include <string.h>

#define QUICK_ID_EYEBROW 0x70002001u
#define QUICK_ID_TITLE 0x70002002u
#define QUICK_ID_ROWS 0x70002003u
#define QUICK_ID_FOOT 0x70002004u
#define QUICK_ID_PART 0x70002100u    /* + 0x40 per row, + k per part */

#define QUICK_ROW_HEIGHT 64.0f
#define QUICK_SEGMENT_HEIGHT 30.0f
#define QUICK_TRACK_RADIUS 9.0f
#define QUICK_TOGGLE_W 46.0f
#define QUICK_TOGGLE_H 26.0f
#define QUICK_KNOB 20.0f

static NkUiId part_id(unsigned row, unsigned k) {
    return (NkUiId)(QUICK_ID_PART + row * 0x40u + k);
}

static NkUiTextStyle row_label_style(const NkUiTheme *theme) {
    NkUiTextStyle style;
    memset(&style, 0, sizeof(style));
    style.font = NK_UI_FONT_BODY_MEDIUM;
    style.size_px = 15;
    style.rgba = theme->paper;
    style.line_height = 22.0f;
    return style;
}

static NkUiTextStyle row_description_style(const NkUiTheme *theme) {
    NkUiTextStyle style;
    memset(&style, 0, sizeof(style));
    style.font = NK_UI_FONT_BODY;
    style.size_px = 12;
    style.rgba = theme->muted;
    style.line_height = 16.0f;
    style.wrap = true;
    return style;
}

/* Opens a settings row: focusable, filled while focused, with its label and
   description on the left. Returns true when opened; the caller adds the control
   and closes it with nk_ui_end_container. */
static bool row_begin(NkUi *ui, NkUiId id, unsigned row, const char *label,
                      const char *description, float text_width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool focused = nk_ui_button_focused(ui, id);
    NkUiLayout layout = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                          nk_overlay_fixed(QUICK_ROW_HEIGHT), NK_UI_SPACE_4);
    NkUiStyle style;
    NkUiLayout text = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                        nk_overlay_fit(), 2.0f);
    memset(&style, 0, sizeof(style));
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    style.fill_rgba = focused ? theme->surface_high : 0u;
    layout.align_y = NK_UI_ALIGN_CENTER;
    layout.padding_left = layout.padding_right = NK_UI_SPACE_4;
    layout.padding_top = layout.padding_bottom = NK_UI_SPACE_2;
    nk_ui_register_button(ui, id, NK_UI_BUTTON_OWNS_HORIZONTAL);
    if (!nk_ui_begin_container(ui, id, &layout, &style)) return false;
    if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
    if (nk_ui_begin_container(ui, part_id(row, 0u), &text, NULL)) {
        NkUiTextStyle label_style = row_label_style(theme);
        NkUiTextStyle description_style = row_description_style(theme);
        nk_ui_text(ui, part_id(row, 1u), label, &label_style);
        nk_ui_text(ui, part_id(row, 2u),
                   nk_ui_text_clamp(ui, description, &description_style, text_width, 2),
                   &description_style);
        nk_ui_end_container(ui);
    }
    return true;
}

/* The segmented control: a ground-filled track with one segment per option; the
   chosen one is raised, with an accent edge while its row has focus. */
static void segmented(NkUi *ui, unsigned row, const char *const *options, int count,
                      int chosen, bool row_focused) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout track = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fit(),
                                         nk_overlay_fit(), 0.0f);
    NkUiStyle track_style;
    int i;
    memset(&track_style, 0, sizeof(track_style));
    track_style.fill_rgba = theme->surface;
    track_style.border_rgba = theme->line_strong;
    track_style.border_width = NK_UI_BORDER_HAIRLINE;
    track_style.corner_radius = QUICK_TRACK_RADIUS;
    track.padding_left = track.padding_right = 3.0f;
    track.padding_top = track.padding_bottom = 3.0f;
    if (!nk_ui_begin_container(ui, part_id(row, 3u), &track, &track_style)) return;
    for (i = 0; i < count; ++i) {
        bool on = i == chosen;
        NkUiLayout segment = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fit(),
                                               nk_overlay_fixed(QUICK_SEGMENT_HEIGHT), 0.0f);
        NkUiStyle style;
        NkUiTextStyle text;
        memset(&style, 0, sizeof(style));
        memset(&text, 0, sizeof(text));
        segment.align_x = NK_UI_ALIGN_CENTER;
        segment.align_y = NK_UI_ALIGN_CENTER;
        segment.padding_left = segment.padding_right = NK_UI_SPACE_3;
        style.corner_radius = QUICK_TRACK_RADIUS - 3.0f;
        if (on) {
            style.fill_rgba = theme->surface_high;
            if (row_focused) {
                style.border_rgba = theme->accent_dark;
                style.border_width = 2.0f;
            }
        }
        text.font = NK_UI_FONT_BODY_MEDIUM;
        text.size_px = 13;
        text.rgba = on ? theme->paper : theme->muted;
        text.line_height = 18.0f;
        if (nk_ui_begin_container(ui, (NkUiId)(NK_OVERLAY_ID_SCALING + 1u + (NkUiId)i),
                                  &segment, &style)) {
            nk_ui_text(ui, part_id(row, 8u + (unsigned)i), options[i], &text);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

/* The toggle: ON or OFF, then a pill track with its knob at the matching end. */
static void toggle(NkUi *ui, unsigned row, bool on) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout group = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fit(), nk_overlay_fit(),
                                         NK_UI_SPACE_2);
    NkUiLayout track = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fixed(QUICK_TOGGLE_W),
                                         nk_overlay_fixed(QUICK_TOGGLE_H), 0.0f);
    NkUiLayout knob = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fixed(QUICK_KNOB),
                                        nk_overlay_fixed(QUICK_KNOB), 0.0f);
    NkUiStyle track_style;
    NkUiStyle knob_style;
    group.align_y = NK_UI_ALIGN_CENTER;
    track.align_y = NK_UI_ALIGN_CENTER;
    track.align_x = on ? NK_UI_ALIGN_END : NK_UI_ALIGN_START;
    track.padding_left = track.padding_right = 3.0f;
    memset(&track_style, 0, sizeof(track_style));
    track_style.fill_rgba = on ? theme->accent_dark : theme->line_strong;
    track_style.corner_radius = NK_UI_RADIUS_PILL;
    memset(&knob_style, 0, sizeof(knob_style));
    knob_style.fill_rgba = on ? theme->ink : theme->muted;
    knob_style.corner_radius = NK_UI_RADIUS_PILL;
    if (!nk_ui_begin_container(ui, part_id(row, 3u), &group, NULL)) return;
    nk_ui_text_role(ui, part_id(row, 4u), on ? "ON" : "OFF", NK_UI_TYPE_LABEL_S,
                    on ? theme->paper : theme->muted);
    if (nk_ui_begin_container(ui, part_id(row, 5u), &track, &track_style)) {
        if (nk_ui_begin_container(ui, part_id(row, 6u), &knob, &knob_style)) {
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

NkOverlayAction nk_overlay_page_settings(NkUi *ui, NkOverlayState *state,
                                         const NkOverlayModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"},
        {NK_UI_PROMPT_ACT_CONFIRM, "Change"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    static const char *const scalings[NK_OVERLAY_SCALING_COUNT] = {"Sharp", "Integer", "Smooth"};
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    int scaling = model->scaling >= 0 && model->scaling < NK_OVERLAY_SCALING_COUNT
        ? model->scaling : NK_OVERLAY_SCALING_SHARP;
    float inner = nk_overlay_page_begin(ui, model, 640.0f);
    NkUiNavDir dir = NK_UI_DIR_NONE;
    if (inner <= 0.0f) return result;
    nk_overlay_eyebrow(ui, QUICK_ID_EYEBROW, NULL, "Game paused \xC2\xB7 Settings", theme->muted);
    nk_ui_text_role(ui, QUICK_ID_TITLE, "Settings", NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiLayout rows = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                            nk_overlay_fit(), NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, QUICK_ID_ROWS, &rows, NULL)) {
            bool focused = nk_ui_button_focused(ui, NK_OVERLAY_ID_SCALING);
            /* The label column leaves room for the widest control. */
            if (row_begin(ui, NK_OVERLAY_ID_SCALING, 0u, "Game scaling",
                          "Sharp keeps pixels crisp, Integer uses whole multiples, "
                          "Smooth blends them.", inner - 300.0f)) {
                segmented(ui, 0u, scalings, NK_OVERLAY_SCALING_COUNT, scaling, focused);
                nk_ui_end_container(ui);
            }
            if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
                int next = scaling + (dir == NK_UI_DIR_RIGHT ? 1 : dir == NK_UI_DIR_LEFT ? -1 : 0);
                if (next >= 0 && next < NK_OVERLAY_SCALING_COUNT && next != scaling) {
                    result = nk_overlay_action(NK_OVERLAY_ACT_SET_SCALING, next);
                }
            } else if (nk_ui_take_activation(ui, NK_OVERLAY_ID_SCALING)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_SET_SCALING,
                                           (scaling + 1) % NK_OVERLAY_SCALING_COUNT);
            }
            focused = nk_ui_button_focused(ui, NK_OVERLAY_ID_HUD);
            if (row_begin(ui, NK_OVERLAY_ID_HUD, 1u, "Performance HUD",
                          "Frame rate and frame time in a corner of the game. F3 shows or "
                          "hides it too.", inner - 180.0f)) {
                toggle(ui, 1u, model->hud_on);
                nk_ui_end_container(ui);
            }
            if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
                if ((dir == NK_UI_DIR_RIGHT) != model->hud_on &&
                    (dir == NK_UI_DIR_LEFT || dir == NK_UI_DIR_RIGHT)) {
                    result = nk_overlay_action(NK_OVERLAY_ACT_SET_HUD, dir == NK_UI_DIR_RIGHT);
                }
            } else if (nk_ui_take_activation(ui, NK_OVERLAY_ID_HUD)) {
                result = nk_overlay_action(NK_OVERLAY_ACT_SET_HUD, model->hud_on ? 0 : 1);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiTextStyle foot = nk_ui_type(NK_UI_TYPE_FOOTNOTE, theme->dim);
        foot.wrap = true;
        nk_ui_text(ui, QUICK_ID_FOOT,
                   "Changes apply now and last until the game closes. The library's "
                   "Settings choose what a game starts with.", &foot);
    }
    if (nk_ui_button(ui, NK_OVERLAY_ID_SETTINGS_BACK, "Back", 0u) ||
        nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        nk_overlay_go(state, NK_OVERLAY_SCREEN_MENU, NK_OVERLAY_ID_SETTINGS);
    }
    nk_overlay_page_end(ui, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    return result;
}
