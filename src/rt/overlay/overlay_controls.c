/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The controls page's rows, described from the mapping in force rather than from a
   copy of it: the input profile the window maps the keyboard and the pad through
   (#357, #520). Controller buttons are named the way the pad's own
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

/* Words for one row's keys, from the profile's key codes. Both keys of a pair
   (Left and Right Shift) read as one word, and the four arrow keys as "Arrow
   keys", so a row stays short enough for the page. */
typedef struct {
    char words[2 * NK_PSP_BTN_COUNT][24];
    size_t count;
} KeyWords;

static bool key_listed(const int *keys, size_t count, int key) {
    size_t i;
    for (i = 0; i < count; ++i) {
        if (keys[i] == key) return true;
    }
    return false;
}

static void key_words_add(KeyWords *words, NameList *list, const char *text) {
    if (words->count >= sizeof(words->words) / sizeof(words->words[0])) return;
    snprintf(words->words[words->count], sizeof(words->words[0]), "%s", text);
    name_add(list, words->words[words->count++]);
}

/* Marks the listed keys in `group` as taken and adds `word` when all of them are listed. */
static void key_group(const int *keys, bool *taken, size_t count, const char *const *group,
                      size_t group_count, const char *word, KeyWords *words, NameList *list) {
    size_t g;
    size_t k;
    for (g = 0; g < group_count; ++g) {
        if (!key_listed(keys, count, nk_host_key_from_name(group[g]))) return;
    }
    for (k = 0; k < count; ++k) {
        for (g = 0; g < group_count; ++g) {
            if (keys[k] == nk_host_key_from_name(group[g])) taken[k] = true;
        }
    }
    key_words_add(words, list, word);
}

static void describe_keys(NameList *list, KeyWords *words, const NkInputProfile *profile,
                          uint32_t bits) {
    static const char *const k_arrows[] = {"up", "down", "left", "right"};
    static const char *const k_shift[] = {"lshift", "rshift"};
    static const char *const k_ctrl[] = {"lctrl", "rctrl"};
    static const char *const k_alt[] = {"lalt", "ralt"};
    int keys[2 * NK_PSP_BTN_COUNT];
    bool taken[2 * NK_PSP_BTN_COUNT];
    size_t count = 0u;
    size_t i;
    int b;
    for (b = 0; b < NK_PSP_BTN_COUNT; ++b) {
        const int slots[2] = {profile->psp_keys[b].primary, profile->psp_keys[b].secondary};
        if ((k_psp_bits[b] & bits) == 0u) continue;
        for (i = 0; i < 2u; ++i) {
            if (nk_host_key_name(slots[i]) != NULL && !key_listed(keys, count, slots[i])) {
                keys[count++] = slots[i];
            }
        }
    }
    memset(taken, 0, sizeof(taken));
    key_group(keys, taken, count, k_arrows, 4u, "Arrow keys", words, list);
    key_group(keys, taken, count, k_shift, 2u, "Shift", words, list);
    key_group(keys, taken, count, k_ctrl, 2u, "Ctrl", words, list);
    key_group(keys, taken, count, k_alt, 2u, "Alt", words, list);
    for (i = 0; i < count; ++i) {
        char text[24];
        if (taken[i]) continue;
        nk_host_key_words(keys[i], text, sizeof(text));
        key_words_add(words, list, text);
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
                                  NkUiPadFamily family) {
    const int set = family_set(family);
    NkInputProfile window_default;
    const NkInputProfile *keymap = profile;
    size_t r;
    if (out == NULL) return;
    if (keymap == NULL) {
        /* No profile: the window maps the keyboard through its defaults. */
        nk_input_profile_init_default(&window_default);
        keymap = &window_default;
    }
    memset(out, 0, sizeof(*out));
    for (r = 0; r < sizeof(k_rows) / sizeof(k_rows[0]) && r < NK_OVERLAY_MAX_CONTROL_ROWS; ++r) {
        const ControlsRowSpec *spec = &k_rows[r];
        NameList keyboard;
        NameList controller;
        KeyWords words;
        memset(&keyboard, 0, sizeof(keyboard));
        memset(&controller, 0, sizeof(controller));
        memset(&words, 0, sizeof(words));
        if (!spec->stick) describe_keys(&keyboard, &words, keymap, spec->bits);
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
