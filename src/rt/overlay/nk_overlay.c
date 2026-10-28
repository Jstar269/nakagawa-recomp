/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The in-game overlay's page state and the frame every page shares: no top bar
   and no waves (the frozen game frame under the scrim is the background), one
   centred card, and the prompt bar along the bottom. */

#include "nk_overlay.h"
#include "nk_ui_widgets.h"

#include <string.h>

#define OVERLAY_ID_ROOT 0x70000001u
#define OVERLAY_ID_BODY 0x70000002u
#define OVERLAY_ID_CARD 0x70000003u
#define OVERLAY_ID_PROMPTS 0x70000004u

void nk_overlay_open(NkOverlayState *state, NkOverlayOpenReason reason) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->open = true;
    state->screen = reason == NK_OVERLAY_OPEN_PAD_LOST ? NK_OVERLAY_SCREEN_PAD_LOST
                                                       : NK_OVERLAY_SCREEN_MENU;
}

void nk_overlay_close(NkOverlayState *state) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
}

const char *nk_overlay_scaling_label(int scaling) {
    switch (scaling) {
    case NK_OVERLAY_SCALING_INTEGER: return "Integer";
    case NK_OVERLAY_SCALING_SMOOTH: return "Smooth";
    default: return "Sharp";
    }
}

void nk_overlay_go(NkOverlayState *state, NkOverlayScreen screen, NkUiId focus) {
    state->screen = screen;
    state->restore_focus = focus;
}

NkOverlayAction nk_overlay_action(NkOverlayActionKind kind, int value) {
    NkOverlayAction action;
    action.kind = kind;
    action.value = value;
    return action;
}

NkUiSizing nk_overlay_grow(void) {
    NkUiSizing sizing = {NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    return sizing;
}

NkUiSizing nk_overlay_fit(void) {
    NkUiSizing sizing = {NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    return sizing;
}

NkUiSizing nk_overlay_fixed(float value) {
    NkUiSizing sizing = {NK_UI_SIZE_FIXED, value, 0.0f, 0.0f};
    return sizing;
}

NkUiLayout nk_overlay_layout(NkUiLayoutDirection direction, NkUiSizing width,
                             NkUiSizing height, float gap) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    layout.direction = direction;
    layout.gap = gap;
    return layout;
}

float nk_overlay_page_begin(NkUi *ui, const NkOverlayModel *model, float card_width) {
    float width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    float card;
    NkUiLayout layout = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                          nk_overlay_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, OVERLAY_ID_ROOT, &layout, NULL)) {
        return 0.0f;
    }
    layout = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(), nk_overlay_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.align_y = NK_UI_ALIGN_CENTER;
    layout.padding_left = layout.padding_right = NK_UI_GUTTER_NARROW;
    layout.padding_top = layout.padding_bottom = NK_UI_SPACE_4;
    if (!nk_ui_begin_container(ui, OVERLAY_ID_BODY, &layout, NULL)) {
        nk_ui_end_container(ui);
        return 0.0f;
    }
    card = width - 2.0f * NK_UI_GUTTER_NARROW;
    if (card > card_width) card = card_width;
    if (!nk_ui_dialog_begin(ui, OVERLAY_ID_CARD, card)) {
        nk_ui_end_container(ui);
        nk_ui_end_container(ui);
        return 0.0f;
    }
    /* The dialog card's padding is 28 px on each side. */
    return card - 56.0f;
}

void nk_overlay_page_end(NkUi *ui, const char *hint, const NkUiPromptItem *prompts,
                         size_t count) {
    nk_ui_end_container(ui); /* card */
    nk_ui_end_container(ui); /* body */
    nk_ui_prompt_bar(ui, OVERLAY_ID_PROMPTS, hint != NULL ? hint : "", prompts, count);
    nk_ui_end_container(ui); /* root */
}

void nk_overlay_eyebrow(NkUi *ui, NkUiId id, const char *marker, const char *text,
                        uint32_t rgba) {
    char line[160];
    if (marker != NULL && marker[0] != '\0') {
        size_t m = strlen(marker);
        size_t t = strlen(text);
        if (m + 1u + t >= sizeof(line)) t = sizeof(line) - m - 2u;
        memcpy(line, marker, m);
        line[m] = ' ';
        memcpy(line + m + 1u, text, t);
        line[m + 1u + t] = '\0';
    } else {
        size_t t = strlen(text);
        if (t >= sizeof(line)) t = sizeof(line) - 1u;
        memcpy(line, text, t);
        line[t] = '\0';
    }
    nk_ui_text_role(ui, id, nk_ui_caps(ui, line), NK_UI_TYPE_EYEBROW, rgba);
}

NkOverlayAction nk_overlay_frame(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model) {
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    bool menu;
    if (ui == NULL || state == NULL || model == NULL || !state->open) {
        return result;
    }
    if (state->restore_focus != 0u) {
        nk_ui_focus(ui, state->restore_focus);
        state->restore_focus = 0u;
    }
    /* F1 and the pad's Start (the menu button) close the overlay from any page,
       as Esc does from the menu. Guide is the host's (it never reaches nk_ui). The
       page is still built: a frame without it would drop the focus it keeps. */
    menu = nk_ui_take_event(ui, NK_UI_EV_MENU, NULL);
    switch (state->screen) {
    case NK_OVERLAY_SCREEN_SETTINGS:
        result = nk_overlay_page_settings(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_CONTROLS:
        result = nk_overlay_page_controls(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_QUIT:
        result = nk_overlay_page_quit(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_PAD_LOST:
        result = nk_overlay_page_pad_lost(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_MENU:
    default:
        result = nk_overlay_page_menu(ui, state, model);
        break;
    }
    return menu ? nk_overlay_action(NK_OVERLAY_ACT_RESUME, 0) : result;
}
