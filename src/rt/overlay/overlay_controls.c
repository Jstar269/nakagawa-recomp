/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The controls page's rows, described from the mapping in force rather than from a
   copy of it: the window's own key table and the input profile the window maps the
   pad through (#357, #520). Controller buttons are named the way the pad's own
   family names them, the rule the prompt bar follows (docs/PLAYER_UI_DESIGN.md,
   prompt glyphs): an Xbox-style pad says A, a Switch-style one B for the same south
   button, a PlayStation-style one Cross, and an unknown pad its position. */

#include "nk_overlay.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *psp;
    uint32_t bits;          /* the PSP buttons the row stands for */
    int stick;              /* the analog stick row: bound through the profile's axes */
} ControlsRowSpec;

static const ControlsRowSpec k_rows[] = {
    {"D-pad", NK_PSP_BTN_UP_BIT | NK_PSP_BTN_DOWN_BIT | NK_PSP_BTN_LEFT_BIT | NK_PSP_BTN_RIGHT_BIT, 0},
    {"Analog stick", 0u, 1},
    {"Cross", NK_PSP_BTN_CROSS_BIT, 0},
    {"Circle", NK_PSP_BTN_CIRCLE_BIT, 0},
    {"Square", NK_PSP_BTN_SQUARE_BIT, 0},
    {"Triangle", NK_PSP_BTN_TRIANGLE_BIT, 0},
    {"L", NK_PSP_BTN_LTRIGGER_BIT, 0},
    {"R", NK_PSP_BTN_RTRIGGER_BIT, 0},
    {"Start", NK_PSP_BTN_START_BIT, 0},
    {"Select", NK_PSP_BTN_SELECT_BIT, 0}
};

/* The button bit of each NkPspButton, from the header's own bit values. */
static const uint32_t k_psp_bits[NK_PSP_BTN_COUNT] = {
    [NK_PSP_BTN_SELECT] = NK_PSP_BTN_SELECT_BIT,
    [NK_PSP_BTN_START] = NK_PSP_BTN_START_BIT,
    [NK_PSP_BTN_UP] = NK_PSP_BTN_UP_BIT,
    [NK_PSP_BTN_RIGHT] = NK_PSP_BTN_RIGHT_BIT,
    [NK_PSP_BTN_DOWN] = NK_PSP_BTN_DOWN_BIT,
    [NK_PSP_BTN_LEFT] = NK_PSP_BTN_LEFT_BIT,
    [NK_PSP_BTN_LTRIGGER] = NK_PSP_BTN_LTRIGGER_BIT,
    [NK_PSP_BTN_RTRIGGER] = NK_PSP_BTN_RTRIGGER_BIT,
    [NK_PSP_BTN_TRIANGLE] = NK_PSP_BTN_TRIANGLE_BIT,
    [NK_PSP_BTN_CIRCLE] = NK_PSP_BTN_CIRCLE_BIT,
    [NK_PSP_BTN_CROSS] = NK_PSP_BTN_CROSS_BIT,
    [NK_PSP_BTN_SQUARE] = NK_PSP_BTN_SQUARE_BIT,
    [NK_PSP_BTN_HOME] = NK_PSP_BTN_HOME_BIT,
    [NK_PSP_BTN_HOLD] = NK_PSP_BTN_HOLD_BIT
};

/* Per family: Xbox-style (A south), Switch-style (A east), PlayStation-style,
   then the position names any other pad gets. */
enum { FAMILY_A_SOUTH, FAMILY_A_EAST, FAMILY_SHAPES, FAMILY_NEUTRAL, FAMILY_SETS };

static int family_set(NkUiPadFamily family) {
    switch (family) {
    case NK_UI_PAD_LETTERS_A_SOUTH: return FAMILY_A_SOUTH;
    case NK_UI_PAD_LETTERS_A_EAST: return FAMILY_A_EAST;
    case NK_UI_PAD_SHAPES: return FAMILY_SHAPES;
    default: return FAMILY_NEUTRAL;
    }
}

static const char *const k_button_names[NK_HOST_BUTTON_COUNT][FAMILY_SETS] = {
    [NK_HOST_BUTTON_SOUTH] = {"A", "B", "Cross", "South button"},
    [NK_HOST_BUTTON_EAST] = {"B", "A", "Circle", "East button"},
    [NK_HOST_BUTTON_WEST] = {"X", "Y", "Square", "West button"},
    [NK_HOST_BUTTON_NORTH] = {"Y", "X", "Triangle", "North button"},
    [NK_HOST_BUTTON_BACK] = {"View", "Minus", "Create", "Back"},
    [NK_HOST_BUTTON_GUIDE] = {"Guide", "Home", "PS button", "Guide"},
    [NK_HOST_BUTTON_START] = {"Menu", "Plus", "Options", "Start"},
    [NK_HOST_BUTTON_LEFT_STICK] = {"LS", "Left stick press", "L3", "Left stick press"},
    [NK_HOST_BUTTON_RIGHT_STICK] = {"RS", "Right stick press", "R3", "Right stick press"},
    [NK_HOST_BUTTON_LEFT_SHOULDER] = {"LB", "L", "L1", "Left shoulder"},
    [NK_HOST_BUTTON_RIGHT_SHOULDER] = {"RB", "R", "R1", "Right shoulder"},
    [NK_HOST_BUTTON_DPAD_UP] = {"D-pad", "D-pad", "D-pad", "D-pad"},
    [NK_HOST_BUTTON_DPAD_DOWN] = {"D-pad", "D-pad", "D-pad", "D-pad"},
    [NK_HOST_BUTTON_DPAD_LEFT] = {"D-pad", "D-pad", "D-pad", "D-pad"},
    [NK_HOST_BUTTON_DPAD_RIGHT] = {"D-pad", "D-pad", "D-pad", "D-pad"},
    [NK_HOST_BUTTON_MISC1] = {"Share", "Capture", "Mic", "Extra button"},
    [NK_HOST_BUTTON_RIGHT_PADDLE1] = {"Right paddle 1", "Right paddle 1", "Right paddle 1", "Right paddle 1"},
    [NK_HOST_BUTTON_LEFT_PADDLE1] = {"Left paddle 1", "Left paddle 1", "Left paddle 1", "Left paddle 1"},
    [NK_HOST_BUTTON_RIGHT_PADDLE2] = {"Right paddle 2", "Right paddle 2", "Right paddle 2", "Right paddle 2"},
    [NK_HOST_BUTTON_LEFT_PADDLE2] = {"Left paddle 2", "Left paddle 2", "Left paddle 2", "Left paddle 2"},
    [NK_HOST_BUTTON_TOUCHPAD] = {"Touchpad", "Touchpad", "Touchpad", "Touchpad"}
};

static const char *const k_trigger_names[2][FAMILY_SETS] = {
    {"LT", "ZL", "L2", "Left trigger"},
    {"RT", "ZR", "R2", "Right trigger"}
};

static const char *axis_stick_name(int axis) {
    switch (axis) {
    case NK_HOST_AXIS_LEFTX:
    case NK_HOST_AXIS_LEFTY: return "Left stick";
    case NK_HOST_AXIS_RIGHTX:
    case NK_HOST_AXIS_RIGHTY: return "Right stick";
    default: return NULL;
    }
}

static const char *source_name(const NkBindingSource *source, int set) {
    switch (source->type) {
    case NK_BINDING_HOST_BUTTON:
        if (source->index >= 0 && source->index < NK_HOST_BUTTON_COUNT) {
            return k_button_names[source->index][set];
        }
        return NULL;
    case NK_BINDING_HOST_TRIGGER:
        if (source->index == NK_HOST_AXIS_LEFT_TRIGGER) return k_trigger_names[0][set];
        if (source->index == NK_HOST_AXIS_RIGHT_TRIGGER) return k_trigger_names[1][set];
        return NULL;
    case NK_BINDING_HOST_AXIS_POS:
    case NK_BINDING_HOST_AXIS_NEG:
        if (source->index == NK_HOST_AXIS_LEFT_TRIGGER) return k_trigger_names[0][set];
        if (source->index == NK_HOST_AXIS_RIGHT_TRIGGER) return k_trigger_names[1][set];
        return axis_stick_name(source->index);
    default:
        return NULL;
    }
}

/* A row's names, each once, joined with " / ". */
typedef struct {
    const char *names[8];
    size_t count;
} NameList;

static void name_add(NameList *list, const char *name) {
    size_t i;
    if (name == NULL || name[0] == '\0') return;
    for (i = 0; i < list->count; ++i) {
        if (strcmp(list->names[i], name) == 0) return;
    }
    if (list->count < sizeof(list->names) / sizeof(list->names[0])) {
        list->names[list->count++] = name;
    }
}

static const char *names_store(NkOverlayControls *out, const NameList *list) {
    char *start = out->text + out->used;
    size_t room = sizeof(out->text) - out->used;
    size_t length = 0u;
    size_t i;
    if (list->count == 0u || room == 0u) return "";
    for (i = 0; i < list->count; ++i) {
        int written = snprintf(start + length, room - length, "%s%s", i == 0u ? "" : " / ",
                               list->names[i]);
        if (written < 0 || (size_t)written >= room - length) {
            /* Out of room: keep what fits whole. */
            start[length] = '\0';
            break;
        }
        length += (size_t)written;
    }
    out->used += length + 1u;
    return start;
}

void nk_overlay_describe_controls(NkOverlayControls *out, const NkInputProfile *profile,
                                  NkUiPadFamily family, const NkOverlayKey *keys,
                                  size_t key_count) {
    const int set = family_set(family);
    size_t r;
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    for (r = 0; r < sizeof(k_rows) / sizeof(k_rows[0]) && r < NK_OVERLAY_MAX_CONTROL_ROWS; ++r) {
        const ControlsRowSpec *spec = &k_rows[r];
        NameList keyboard;
        NameList controller;
        size_t i;
        memset(&keyboard, 0, sizeof(keyboard));
        memset(&controller, 0, sizeof(controller));
        for (i = 0; keys != NULL && i < key_count; ++i) {
            if ((keys[i].psp_bits & spec->bits) != 0u) name_add(&keyboard, keys[i].label);
        }
        if (profile != NULL && spec->stick) {
            name_add(&controller, axis_stick_name(profile->axes[NK_PSP_AXIS_ANALOG_X].host_axis));
            name_add(&controller, axis_stick_name(profile->axes[NK_PSP_AXIS_ANALOG_Y].host_axis));
        } else if (profile != NULL) {
            int b;
            for (b = 0; b < NK_PSP_BTN_COUNT; ++b) {
                if ((k_psp_bits[b] & spec->bits) == 0u) continue;
                name_add(&controller, source_name(&profile->psp_buttons[b].primary, set));
                name_add(&controller, source_name(&profile->psp_buttons[b].secondary, set));
            }
        }
        out->rows[r].psp = spec->psp;
        out->rows[r].keyboard = names_store(out, &keyboard);
        out->rows[r].controller = names_store(out, &controller);
    }
    out->count = r;
}
