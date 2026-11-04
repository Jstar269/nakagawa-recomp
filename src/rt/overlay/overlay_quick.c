/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The overlay's settings page: the settings a person changes with the game in front
   of them: fullscreen, game scaling, frame pacing and the performance HUD (and Ace
   Mode's speed readout in an Ace run). Each is a focusable row
   (docs/PLAYER_UI_DESIGN.md, Settings rows): scaling and frame pacing are segmented
   controls that left and right move along, the others toggles. Changes apply at once
   and last until the game closes. */

#include "nk_overlay.h"
#include "nk_ui_widgets.h"

#include <string.h>

#define QUICK_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_OVERLAY, (offset))
#define QUICK_ID_EYEBROW QUICK_ID(0x002001u)
#define QUICK_ID_TITLE QUICK_ID(0x002002u)
#define QUICK_ID_ROWS QUICK_ID(0x002003u)
#define QUICK_ID_FOOT QUICK_ID(0x002004u)
#define QUICK_ID_PART QUICK_ID(0x002100u)    /* + 0x40 per row, + k per part */
#define QUICK_ID_PART_SPAN 0x800u

#define QUICK_ROW_HEIGHT 64.0f
/* Under QUICK_COMPACT_BELOW logical pixels high (the Controls page's split rule), every row
   fits: rows are 48 px with a one-line description, on a wider card so the line holds. */
#define QUICK_COMPACT_BELOW 720.0f
#define QUICK_ROW_HEIGHT_COMPACT 48.0f
#define QUICK_CARD_WIDTH 640.0f
#define QUICK_CARD_WIDTH_COMPACT 880.0f
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
                      const char *description, float text_width, bool compact) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool focused = nk_ui_button_focused(ui, id);
    NkUiLayout layout = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                          nk_overlay_fixed(compact ? QUICK_ROW_HEIGHT_COMPACT
                                                                   : QUICK_ROW_HEIGHT),
                                          NK_UI_SPACE_4);
    NkUiStyle style;
    NkUiLayout text = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                        nk_overlay_fit(), 2.0f);
    memset(&style, 0, sizeof(style));
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    style.fill_rgba = nk_ui_pressed(ui, id) ? theme->pressed :
                      focused || nk_ui_hovered(ui, id) ? theme->surface_high : 0u;
    layout.align_y = NK_UI_ALIGN_CENTER;
    layout.padding_left = layout.padding_right = NK_UI_SPACE_4;
    layout.padding_top = layout.padding_bottom = compact ? NK_UI_SPACE_1 : NK_UI_SPACE_2;
    nk_ui_register_button(ui, id, NK_UI_BUTTON_OWNS_HORIZONTAL);
    if (!nk_ui_begin_container(ui, id, &layout, &style)) return false;
    if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
    if (nk_ui_begin_container(ui, part_id(row, 0u), &text, NULL)) {
        NkUiTextStyle label_style = row_label_style(theme);
        NkUiTextStyle description_style = row_description_style(theme);
        nk_ui_text(ui, part_id(row, 1u), label, &label_style);
        nk_ui_text(ui, part_id(row, 2u),
                   nk_ui_text_clamp(ui, description, &description_style, text_width,
                                    compact ? 1 : 2),
                   &description_style);
        nk_ui_end_container(ui);
    }
    return true;
}

/* The segmented control: a ground-filled track with one segment per option; the
   chosen one is raised, with an accent edge while its row has focus. */
static void segmented(NkUi *ui, unsigned row, NkUiId first_option, const char *const *options,
                      int count, int chosen, bool row_focused) {
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
        if (nk_ui_begin_container(ui, (NkUiId)(first_option + (NkUiId)i), &segment, &style)) {
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

/* A toggle row: Confirm flips it; right turns it on and left off, and a direction that
   matches the value changes nothing. The action carries 1 for on, 0 for off. */
static void toggle_row(NkUi *ui, NkUiId id, unsigned row, const char *label,
                       const char *description, float text_width, bool compact, bool on,
                       NkOverlayActionKind kind, NkOverlayAction *result) {
    bool focused = nk_ui_button_focused(ui, id);
    NkUiNavDir dir = NK_UI_DIR_NONE;
    if (row_begin(ui, id, row, label, description, text_width, compact)) {
        toggle(ui, row, on);
        nk_ui_end_container(ui);
    }
    if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
        if ((dir == NK_UI_DIR_RIGHT) != on &&
            (dir == NK_UI_DIR_LEFT || dir == NK_UI_DIR_RIGHT)) {
            *result = nk_overlay_action(kind, dir == NK_UI_DIR_RIGHT);
        }
    } else if (nk_ui_take_activation(ui, id)) {
        *result = nk_overlay_action(kind, on ? 0 : 1);
    }
}

/* A segmented row: left and right step along the options and stop at the ends; Confirm
   steps on, wrapping to the first. The action carries the new option's index. */
static void segmented_row(NkUi *ui, NkUiId id, unsigned row, const char *label,
                          const char *description, float text_width, bool compact,
                          const char *const *options, int count, int chosen,
                          NkOverlayActionKind kind, NkOverlayAction *result) {
    bool focused = nk_ui_button_focused(ui, id);
    NkUiNavDir dir = NK_UI_DIR_NONE;
    if (row_begin(ui, id, row, label, description, text_width, compact)) {
        segmented(ui, row, (NkUiId)(id + 1u), options, count, chosen, focused);
        nk_ui_end_container(ui);
    }
    if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
        int next = chosen + (dir == NK_UI_DIR_RIGHT ? 1 : dir == NK_UI_DIR_LEFT ? -1 : 0);
        if (next >= 0 && next < count && next != chosen) {
            *result = nk_overlay_action(kind, next);
        }
    } else if (nk_ui_take_activation(ui, id)) {
        *result = nk_overlay_action(kind, (chosen + 1) % count);
    }
}

NkOverlayAction nk_overlay_page_settings(NkUi *ui, NkOverlayState *state,
                                         const NkOverlayModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"},
        {NK_UI_PROMPT_ACT_CONFIRM, "Change"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    static const char *const scalings[NK_OVERLAY_SCALING_COUNT] = {"Sharp", "Integer", "Smooth"};
    static const char *const pacings[NK_OVERLAY_PACING_COUNT] = {"PSP timing", "No extra cap"};
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    int scaling = model->scaling >= 0 && model->scaling < NK_OVERLAY_SCALING_COUNT
        ? model->scaling : NK_OVERLAY_SCALING_SHARP;
    bool compact = model->logical_height > 0.0f && model->logical_height < QUICK_COMPACT_BELOW;
    float inner = nk_overlay_page_begin(ui, model,
                                        compact ? QUICK_CARD_WIDTH_COMPACT : QUICK_CARD_WIDTH);
    if (inner <= 0.0f) return result;
    nk_overlay_eyebrow(ui, QUICK_ID_EYEBROW, NULL, "Game paused \xC2\xB7 Settings", theme->muted);
    nk_ui_text_role(ui, QUICK_ID_TITLE, "Settings", NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiLayout rows = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                            nk_overlay_fit(), NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, QUICK_ID_ROWS, &rows, NULL)) {
            /* The rows' part ids (the second argument) keep the numbers the first three rows
               had before fullscreen and frame pacing joined, so a page without the new rows
               lays out exactly as it did. The label column leaves room for the widest
               control. */
            if (model->fullscreen > 0) {
                toggle_row(ui, NK_OVERLAY_ID_FULLSCREEN, 3u, "Fullscreen",
                           "The game fills the whole screen. Off puts it back in its window.",
                           inner - 180.0f, compact, model->fullscreen == 2,
                           NK_OVERLAY_ACT_SET_FULLSCREEN, &result);
            }
            segmented_row(ui, NK_OVERLAY_ID_SCALING, 0u, "Game scaling",
                          "Sharp keeps pixels crisp, Integer uses whole multiples, "
                          "Smooth blends them.", inner - 300.0f, compact, scalings,
                          NK_OVERLAY_SCALING_COUNT, scaling, NK_OVERLAY_ACT_SET_SCALING, &result);
            if (model->pacing > 0) {
                int pacing = model->pacing - 1 < NK_OVERLAY_PACING_COUNT
                    ? model->pacing - 1 : NK_OVERLAY_PACING_PSP;
                segmented_row(ui, NK_OVERLAY_ID_PACING, 4u, "Frame pacing",
                              "PSP timing paces frames like the PSP's screen. No extra cap "
                              "shows every frame.", inner - 300.0f, compact, pacings, NK_OVERLAY_PACING_COUNT, pacing,
                              NK_OVERLAY_ACT_SET_PACING, &result);
            }
            toggle_row(ui, NK_OVERLAY_ID_HUD, 1u, "Performance HUD",
                       "Frame rate and frame time in a corner of the game. F3 shows or "
                       "hides it too.", inner - 180.0f, compact, model->hud_on,
                       NK_OVERLAY_ACT_SET_HUD, &result);
            /* Ace Mode's speed readout (src/rt/ace.h), in an Ace run that can draw it. */
            if (model->ace_readout > 0) {
                toggle_row(ui, NK_OVERLAY_ID_READOUT, 2u, "Speed readout",
                           "The game's speed and frame rate in a corner, from Ace Mode. "
                           "Hidden while the HUD shows.", inner - 180.0f, compact,
                           model->ace_readout == 2, NK_OVERLAY_ACT_SET_READOUT, &result);
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
