/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#define _POSIX_C_SOURCE 200809L

#include "nk_input_profile.h"
#include "nk_json.h"
#include "nk_platform.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

/* -----------------------------------------------------------------------------
 * String Helpers
 * -------------------------------------------------------------------------- */

static int nk_ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int nk_ascii_casecmp(const char *a, const char *b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        int ca = nk_ascii_lower((unsigned char)*a);
        int cb = nk_ascii_lower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return nk_ascii_lower((unsigned char)*a) - nk_ascii_lower((unsigned char)*b);
}

static FILE *nk_input_fopen(const char *path, const char *mode) {
#if defined(_WIN32) || defined(_WIN64)
    if (!path || !mode) return NULL;
    WCHAR wpath[32768];
    WCHAR wmode[32];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1,
                            wmode, (int)(sizeof(wmode) / sizeof(wmode[0]))) <= 0) {
        return NULL;
    }
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

static void diag_set(char *buf, size_t sz, const char *fmt, ...) {
    if (!buf || sz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
}

/* -----------------------------------------------------------------------------
 * Name Registries
 * -------------------------------------------------------------------------- */

static const struct {
    NkPspButton btn;
    const char *name;
    uint32_t mask;
} kPspButtons[] = {
    { NK_PSP_BTN_SELECT,   "select",   0x0001 },
    { NK_PSP_BTN_START,    "start",    0x0008 },
    { NK_PSP_BTN_UP,       "up",       0x0010 },
    { NK_PSP_BTN_RIGHT,    "right",    0x0020 },
    { NK_PSP_BTN_DOWN,     "down",     0x0040 },
    { NK_PSP_BTN_LEFT,     "left",     0x0080 },
    { NK_PSP_BTN_LTRIGGER, "ltrigger", 0x0100 },
    { NK_PSP_BTN_RTRIGGER, "rtrigger", 0x0200 },
    { NK_PSP_BTN_TRIANGLE, "triangle", 0x1000 },
    { NK_PSP_BTN_CIRCLE,   "circle",   0x2000 },
    { NK_PSP_BTN_CROSS,    "cross",    0x4000 },
    { NK_PSP_BTN_SQUARE,   "square",   0x8000 },
    { NK_PSP_BTN_HOME,     "home",     0x010000 },
    { NK_PSP_BTN_HOLD,     "hold",     0x020000 }
};

static const struct {
    NkPspAxis axis;
    const char *name;
} kPspAxes[] = {
    { NK_PSP_AXIS_ANALOG_X, "analog_x" },
    { NK_PSP_AXIS_ANALOG_Y, "analog_y" }
};

static const struct {
    NkHostGamepadButton btn;
    const char *name;
} kHostButtons[] = {
    { NK_HOST_BUTTON_SOUTH,          "south" },
    { NK_HOST_BUTTON_EAST,           "east" },
    { NK_HOST_BUTTON_WEST,           "west" },
    { NK_HOST_BUTTON_NORTH,          "north" },
    { NK_HOST_BUTTON_BACK,           "back" },
    { NK_HOST_BUTTON_GUIDE,          "guide" },
    { NK_HOST_BUTTON_START,          "start" },
    { NK_HOST_BUTTON_LEFT_STICK,     "left_stick" },
    { NK_HOST_BUTTON_RIGHT_STICK,    "right_stick" },
    { NK_HOST_BUTTON_LEFT_SHOULDER,  "left_shoulder" },
    { NK_HOST_BUTTON_RIGHT_SHOULDER, "right_shoulder" },
    { NK_HOST_BUTTON_DPAD_UP,        "dpad_up" },
    { NK_HOST_BUTTON_DPAD_DOWN,      "dpad_down" },
    { NK_HOST_BUTTON_DPAD_LEFT,      "dpad_left" },
    { NK_HOST_BUTTON_DPAD_RIGHT,     "dpad_right" },
    { NK_HOST_BUTTON_MISC1,          "misc1" },
    { NK_HOST_BUTTON_RIGHT_PADDLE1,  "right_paddle1" },
    { NK_HOST_BUTTON_LEFT_PADDLE1,   "left_paddle1" },
    { NK_HOST_BUTTON_RIGHT_PADDLE2,  "right_paddle2" },
    { NK_HOST_BUTTON_LEFT_PADDLE2,   "left_paddle2" },
    { NK_HOST_BUTTON_TOUCHPAD,       "touchpad" }
};

static const struct {
    NkHostGamepadAxis axis;
    const char *name;
} kHostAxes[] = {
    { NK_HOST_AXIS_LEFTX,         "leftx" },
    { NK_HOST_AXIS_LEFTY,         "lefty" },
    { NK_HOST_AXIS_RIGHTX,        "rightx" },
    { NK_HOST_AXIS_RIGHTY,        "righty" },
    { NK_HOST_AXIS_LEFT_TRIGGER,  "left_trigger" },
    { NK_HOST_AXIS_RIGHT_TRIGGER, "right_trigger" }
};

static const struct {
    NkNavAction action;
    const char *name;
} kNavActions[] = {
    { NK_NAV_ACTION_CONFIRM,   "confirm" },
    { NK_NAV_ACTION_CANCEL,    "cancel" },
    { NK_NAV_ACTION_UP,        "up" },
    { NK_NAV_ACTION_DOWN,      "down" },
    { NK_NAV_ACTION_LEFT,      "left" },
    { NK_NAV_ACTION_RIGHT,     "right" },
    { NK_NAV_ACTION_PAGE_PREV, "page_prev" },
    { NK_NAV_ACTION_PAGE_NEXT, "page_next" },
    { NK_NAV_ACTION_MENU,      "menu" }
};

uint32_t nk_psp_button_bitmask(NkPspButton btn) {
    if ((int)btn >= 0 && (int)btn < NK_PSP_BTN_COUNT) {
        return kPspButtons[btn].mask;
    }
    return 0;
}

const char *nk_psp_button_name(NkPspButton btn) {
    if ((int)btn >= 0 && (int)btn < NK_PSP_BTN_COUNT) {
        return kPspButtons[btn].name;
    }
    return "unknown";
}

NkPspButton nk_psp_button_from_name(const char *name) {
    if (!name) return (NkPspButton)-1;
    for (size_t i = 0; i < sizeof(kPspButtons) / sizeof(kPspButtons[0]); i++) {
        if (nk_ascii_casecmp(name, kPspButtons[i].name) == 0) {
            return kPspButtons[i].btn;
        }
    }
    return (NkPspButton)-1;
}

const char *nk_psp_axis_name(NkPspAxis axis) {
    if ((int)axis >= 0 && (int)axis < NK_PSP_AXIS_COUNT) {
        return kPspAxes[axis].name;
    }
    return "unknown";
}

NkPspAxis nk_psp_axis_from_name(const char *name) {
    if (!name) return (NkPspAxis)-1;
    for (size_t i = 0; i < sizeof(kPspAxes) / sizeof(kPspAxes[0]); i++) {
        if (nk_ascii_casecmp(name, kPspAxes[i].name) == 0) {
            return kPspAxes[i].axis;
        }
    }
    return (NkPspAxis)-1;
}

const char *nk_host_button_name(NkHostGamepadButton btn) {
    for (size_t i = 0; i < sizeof(kHostButtons) / sizeof(kHostButtons[0]); i++) {
        if (kHostButtons[i].btn == btn) return kHostButtons[i].name;
    }
    return "unknown";
}

NkHostGamepadButton nk_host_button_from_name(const char *name) {
    if (!name) return NK_HOST_BUTTON_INVALID;
    for (size_t i = 0; i < sizeof(kHostButtons) / sizeof(kHostButtons[0]); i++) {
        if (nk_ascii_casecmp(name, kHostButtons[i].name) == 0) {
            return kHostButtons[i].btn;
        }
    }
    return NK_HOST_BUTTON_INVALID;
}

const char *nk_host_axis_name(NkHostGamepadAxis axis) {
    for (size_t i = 0; i < sizeof(kHostAxes) / sizeof(kHostAxes[0]); i++) {
        if (kHostAxes[i].axis == axis) return kHostAxes[i].name;
    }
    return "unknown";
}

NkHostGamepadAxis nk_host_axis_from_name(const char *name) {
    if (!name) return NK_HOST_AXIS_INVALID;
    for (size_t i = 0; i < sizeof(kHostAxes) / sizeof(kHostAxes[0]); i++) {
        if (nk_ascii_casecmp(name, kHostAxes[i].name) == 0) {
            return kHostAxes[i].axis;
        }
    }
    return NK_HOST_AXIS_INVALID;
}

const char *nk_nav_action_name(NkNavAction action) {
    if ((int)action >= 0 && (int)action < NK_NAV_ACTION_COUNT) {
        return kNavActions[action].name;
    }
    return "unknown";
}

NkNavAction nk_nav_action_from_name(const char *name) {
    if (!name) return (NkNavAction)-1;
    for (size_t i = 0; i < sizeof(kNavActions) / sizeof(kNavActions[0]); i++) {
        if (nk_ascii_casecmp(name, kNavActions[i].name) == 0) {
            return kNavActions[i].action;
        }
    }
    return (NkNavAction)-1;
}

/* -----------------------------------------------------------------------------
 * Transforms
 * -------------------------------------------------------------------------- */

uint8_t nk_input_profile_transform_axis(
    int16_t raw_axis,
    int16_t deadzone_inner,
    int16_t deadzone_outer,
    bool inverted
) {
    int32_t val = (int32_t)raw_axis;
    if (inverted) {
        val = -val;
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
    }

    if (val >= -deadzone_inner && val <= deadzone_inner) {
        return 128;
    }

    if (deadzone_outer > 0) {
        int32_t outer_threshold = 32767 - (int32_t)deadzone_outer;
        if (val >= outer_threshold) {
            val = 32767;
        } else if (val <= -outer_threshold) {
            val = -32768;
        }
    }

    return (uint8_t)(((int32_t)val + 32768) * 255 / 65535);
}

bool nk_input_profile_eval_trigger(int16_t raw_trigger, int16_t trigger_threshold) {
    return raw_trigger > trigger_threshold;
}

/* -----------------------------------------------------------------------------
 * Profile Default Initialization
 * -------------------------------------------------------------------------- */

void nk_input_profile_init_default(NkInputProfile *profile) {
    if (!profile) return;
    memset(profile, 0, sizeof(*profile));

    profile->schema_version = NK_INPUT_PROFILE_SCHEMA_VERSION;
    snprintf(profile->guid, sizeof(profile->guid), "default");
    snprintf(profile->name_hint, sizeof(profile->name_hint), "Standard Gamepad");
    profile->trigger_threshold = NK_INPUT_DEFAULT_TRIGGER_THRESHOLD;

    /* Axes: Left Stick X/Y */
    profile->axes[NK_PSP_AXIS_ANALOG_X].host_axis = NK_HOST_AXIS_LEFTX;
    profile->axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner = NK_INPUT_DEFAULT_DEADZONE_INNER;
    profile->axes[NK_PSP_AXIS_ANALOG_X].deadzone_outer = NK_INPUT_DEFAULT_DEADZONE_OUTER;
    profile->axes[NK_PSP_AXIS_ANALOG_X].inverted = false;

    profile->axes[NK_PSP_AXIS_ANALOG_Y].host_axis = NK_HOST_AXIS_LEFTY;
    profile->axes[NK_PSP_AXIS_ANALOG_Y].deadzone_inner = NK_INPUT_DEFAULT_DEADZONE_INNER;
    profile->axes[NK_PSP_AXIS_ANALOG_Y].deadzone_outer = NK_INPUT_DEFAULT_DEADZONE_OUTER;
    profile->axes[NK_PSP_AXIS_ANALOG_Y].inverted = false;

    /* PSP Buttons matching gpu_sdl3vk/sdl3vk.c */
    profile->psp_buttons[NK_PSP_BTN_SELECT].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_SELECT].primary.index = NK_HOST_BUTTON_BACK;

    profile->psp_buttons[NK_PSP_BTN_START].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_START].primary.index = NK_HOST_BUTTON_START;

    profile->psp_buttons[NK_PSP_BTN_UP].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_UP].primary.index = NK_HOST_BUTTON_DPAD_UP;

    profile->psp_buttons[NK_PSP_BTN_RIGHT].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_RIGHT].primary.index = NK_HOST_BUTTON_DPAD_RIGHT;

    profile->psp_buttons[NK_PSP_BTN_DOWN].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_DOWN].primary.index = NK_HOST_BUTTON_DPAD_DOWN;

    profile->psp_buttons[NK_PSP_BTN_LEFT].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_LEFT].primary.index = NK_HOST_BUTTON_DPAD_LEFT;

    profile->psp_buttons[NK_PSP_BTN_LTRIGGER].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_LTRIGGER].primary.index = NK_HOST_BUTTON_LEFT_SHOULDER;
    profile->psp_buttons[NK_PSP_BTN_LTRIGGER].secondary.type = NK_BINDING_HOST_TRIGGER;
    profile->psp_buttons[NK_PSP_BTN_LTRIGGER].secondary.index = NK_HOST_AXIS_LEFT_TRIGGER;

    profile->psp_buttons[NK_PSP_BTN_RTRIGGER].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_RTRIGGER].primary.index = NK_HOST_BUTTON_RIGHT_SHOULDER;
    profile->psp_buttons[NK_PSP_BTN_RTRIGGER].secondary.type = NK_BINDING_HOST_TRIGGER;
    profile->psp_buttons[NK_PSP_BTN_RTRIGGER].secondary.index = NK_HOST_AXIS_RIGHT_TRIGGER;

    profile->psp_buttons[NK_PSP_BTN_TRIANGLE].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_TRIANGLE].primary.index = NK_HOST_BUTTON_NORTH;

    profile->psp_buttons[NK_PSP_BTN_CIRCLE].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_CIRCLE].primary.index = NK_HOST_BUTTON_EAST;

    profile->psp_buttons[NK_PSP_BTN_CROSS].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_SOUTH;

    profile->psp_buttons[NK_PSP_BTN_SQUARE].primary.type = NK_BINDING_HOST_BUTTON;
    profile->psp_buttons[NK_PSP_BTN_SQUARE].primary.index = NK_HOST_BUTTON_WEST;

    /* HOME stays unbound by default: the pre-profile runtime never mapped the host GUIDE
     * button, and the default must reproduce that mapping exactly. Users may bind it. */
    profile->psp_buttons[NK_PSP_BTN_HOME].primary.type = NK_BINDING_NONE;

    profile->psp_buttons[NK_PSP_BTN_HOLD].primary.type = NK_BINDING_NONE;

    /* Navigation bindings matching player/main.c */
    profile->nav_bindings[NK_NAV_ACTION_CONFIRM].primary.type = NK_BINDING_HOST_BUTTON;
    profile->nav_bindings[NK_NAV_ACTION_CONFIRM].primary.index = NK_HOST_BUTTON_SOUTH;

    profile->nav_bindings[NK_NAV_ACTION_CANCEL].primary.type = NK_BINDING_HOST_BUTTON;
    profile->nav_bindings[NK_NAV_ACTION_CANCEL].primary.index = NK_HOST_BUTTON_EAST;

    profile->nav_bindings[NK_NAV_ACTION_UP].primary.type = NK_BINDING_HOST_BUTTON;
    profile->nav_bindings[NK_NAV_ACTION_UP].primary.index = NK_HOST_BUTTON_DPAD_UP;

    profile->nav_bindings[NK_NAV_ACTION_DOWN].primary.type = NK_BINDING_HOST_BUTTON;
    profile->nav_bindings[NK_NAV_ACTION_DOWN].primary.index = NK_HOST_BUTTON_DPAD_DOWN;

    profile->nav_bindings[NK_NAV_ACTION_LEFT].primary.type = NK_BINDING_HOST_BUTTON;
    profile->nav_bindings[NK_NAV_ACTION_LEFT].primary.index = NK_HOST_BUTTON_DPAD_LEFT;

    profile->nav_bindings[NK_NAV_ACTION_RIGHT].primary.type = NK_BINDING_HOST_BUTTON;
    profile->nav_bindings[NK_NAV_ACTION_RIGHT].primary.index = NK_HOST_BUTTON_DPAD_RIGHT;

    profile->nav_bindings[NK_NAV_ACTION_PAGE_PREV].primary.type = NK_BINDING_HOST_BUTTON;
    profile->nav_bindings[NK_NAV_ACTION_PAGE_PREV].primary.index = NK_HOST_BUTTON_LEFT_SHOULDER;

    profile->nav_bindings[NK_NAV_ACTION_PAGE_NEXT].primary.type = NK_BINDING_HOST_BUTTON;
    profile->nav_bindings[NK_NAV_ACTION_PAGE_NEXT].primary.index = NK_HOST_BUTTON_RIGHT_SHOULDER;

    profile->nav_bindings[NK_NAV_ACTION_MENU].primary.type = NK_BINDING_HOST_BUTTON;
    profile->nav_bindings[NK_NAV_ACTION_MENU].primary.index = NK_HOST_BUTTON_START;
}

/* -----------------------------------------------------------------------------
 * Profile Validation & Conflict Detection
 * -------------------------------------------------------------------------- */

static bool check_source_conflict(
    const NkBindingSource *src,
    int owner_idx,
    int button_owners[NK_HOST_BUTTON_COUNT],
    int trigger_owners[NK_HOST_AXIS_COUNT],
    int axis_pos_owners[NK_HOST_AXIS_COUNT],
    int axis_neg_owners[NK_HOST_AXIS_COUNT],
    int *out_conflict_owner,
    const char **out_kind,
    const char **out_name
) {
    if (!src || src->type == NK_BINDING_NONE) return true;

    if (src->type == NK_BINDING_HOST_BUTTON) {
        if (src->index < 0 || src->index >= NK_HOST_BUTTON_COUNT) return false;
        if (button_owners[src->index] != -1 && button_owners[src->index] != owner_idx) {
            *out_conflict_owner = button_owners[src->index];
            *out_kind = "button";
            *out_name = nk_host_button_name((NkHostGamepadButton)src->index);
            return false;
        }
        button_owners[src->index] = owner_idx;
    } else if (src->type == NK_BINDING_HOST_TRIGGER) {
        if (src->index < 0 || src->index >= NK_HOST_AXIS_COUNT) return false;
        if (trigger_owners[src->index] != -1 && trigger_owners[src->index] != owner_idx) {
            *out_conflict_owner = trigger_owners[src->index];
            *out_kind = "trigger";
            *out_name = nk_host_axis_name((NkHostGamepadAxis)src->index);
            return false;
        }
        trigger_owners[src->index] = owner_idx;
    } else if (src->type == NK_BINDING_HOST_AXIS_POS) {
        if (src->index < 0 || src->index >= NK_HOST_AXIS_COUNT) return false;
        if (axis_pos_owners[src->index] != -1 && axis_pos_owners[src->index] != owner_idx) {
            *out_conflict_owner = axis_pos_owners[src->index];
            *out_kind = "axis_pos";
            *out_name = nk_host_axis_name((NkHostGamepadAxis)src->index);
            return false;
        }
        axis_pos_owners[src->index] = owner_idx;
    } else if (src->type == NK_BINDING_HOST_AXIS_NEG) {
        if (src->index < 0 || src->index >= NK_HOST_AXIS_COUNT) return false;
        if (axis_neg_owners[src->index] != -1 && axis_neg_owners[src->index] != owner_idx) {
            *out_conflict_owner = axis_neg_owners[src->index];
            *out_kind = "axis_neg";
            *out_name = nk_host_axis_name((NkHostGamepadAxis)src->index);
            return false;
        }
        axis_neg_owners[src->index] = owner_idx;
    }
    return true;
}

NkResult nk_input_profile_validate(
    const NkInputProfile *profile,
    char *diag_buf,
    size_t diag_buf_sz
) {
    if (diag_buf && diag_buf_sz > 0) diag_buf[0] = '\0';
    if (!profile) {
        diag_set(diag_buf, diag_buf_sz, "profile pointer is null");
        return NK_ERROR_GENERIC;
    }

    if (profile->schema_version != NK_INPUT_PROFILE_SCHEMA_VERSION) {
        if (profile->schema_version > NK_INPUT_PROFILE_SCHEMA_VERSION) {
            diag_set(diag_buf, diag_buf_sz,
                     "unsupported future schema_version %d (current version is %d)",
                     profile->schema_version, NK_INPUT_PROFILE_SCHEMA_VERSION);
        } else {
            diag_set(diag_buf, diag_buf_sz,
                     "unsupported or invalid schema_version %d (expected %d)",
                     profile->schema_version, NK_INPUT_PROFILE_SCHEMA_VERSION);
        }
        return NK_ERROR_GENERIC;
    }

    if (profile->guid[0] == '\0') {
        diag_set(diag_buf, diag_buf_sz, "device GUID is required");
        return NK_ERROR_GENERIC;
    }

    if (profile->trigger_threshold < 0) {
        diag_set(diag_buf, diag_buf_sz,
                 "trigger_threshold %d out of range [0, 32767]",
                 profile->trigger_threshold);
        return NK_ERROR_GENERIC;
    }

    /* Validate axes */
    for (int i = 0; i < NK_PSP_AXIS_COUNT; i++) {
        const NkAxisCalibration *ax = &profile->axes[i];
        if (ax->host_axis < 0 || ax->host_axis >= NK_HOST_AXIS_COUNT) {
            diag_set(diag_buf, diag_buf_sz,
                     "axis '%s' has invalid host axis index %d",
                     nk_psp_axis_name((NkPspAxis)i), ax->host_axis);
            return NK_ERROR_GENERIC;
        }
        if (ax->deadzone_inner < 0) {
            diag_set(diag_buf, diag_buf_sz,
                     "axis '%s' deadzone_inner %d out of range [0, 32767]",
                     nk_psp_axis_name((NkPspAxis)i), ax->deadzone_inner);
            return NK_ERROR_GENERIC;
        }
        if (ax->deadzone_outer < 0) {
            diag_set(diag_buf, diag_buf_sz,
                     "axis '%s' deadzone_outer %d out of range [0, 32767]",
                     nk_psp_axis_name((NkPspAxis)i), ax->deadzone_outer);
            return NK_ERROR_GENERIC;
        }
        if ((int32_t)ax->deadzone_inner + (int32_t)ax->deadzone_outer >= 32767) {
            diag_set(diag_buf, diag_buf_sz,
                     "axis '%s' combined deadzones (%d + %d) out of range (< 32767)",
                     nk_psp_axis_name((NkPspAxis)i), ax->deadzone_inner, ax->deadzone_outer);
            return NK_ERROR_GENERIC;
        }
    }

    if (profile->axes[NK_PSP_AXIS_ANALOG_X].host_axis == profile->axes[NK_PSP_AXIS_ANALOG_Y].host_axis) {
        diag_set(diag_buf, diag_buf_sz,
                 "conflicting axis binding: host axis '%s' is bound to both 'analog_x' and 'analog_y'",
                 nk_host_axis_name(profile->axes[NK_PSP_AXIS_ANALOG_X].host_axis));
        return NK_ERROR_GENERIC;
    }

    /* Conflict detection across PSP digital buttons */
    int button_owners[NK_HOST_BUTTON_COUNT];
    int trigger_owners[NK_HOST_AXIS_COUNT];
    int axis_pos_owners[NK_HOST_AXIS_COUNT];
    int axis_neg_owners[NK_HOST_AXIS_COUNT];
    for (int i = 0; i < NK_HOST_BUTTON_COUNT; i++) button_owners[i] = -1;
    for (int i = 0; i < NK_HOST_AXIS_COUNT; i++) {
        trigger_owners[i] = -1;
        axis_pos_owners[i] = -1;
        axis_neg_owners[i] = -1;
    }

    for (int i = 0; i < NK_PSP_BTN_COUNT; i++) {
        int conflict_owner = -1;
        const char *kind = NULL;
        const char *name = NULL;

        if (!check_source_conflict(&profile->psp_buttons[i].primary, i,
                                   button_owners, trigger_owners, axis_pos_owners, axis_neg_owners,
                                   &conflict_owner, &kind, &name)) {
            diag_set(diag_buf, diag_buf_sz,
                     "conflicting binding: host %s '%s' is bound to both '%s' and '%s'",
                     kind, name,
                     nk_psp_button_name((NkPspButton)conflict_owner),
                     nk_psp_button_name((NkPspButton)i));
            return NK_ERROR_GENERIC;
        }

        if (!check_source_conflict(&profile->psp_buttons[i].secondary, i,
                                   button_owners, trigger_owners, axis_pos_owners, axis_neg_owners,
                                   &conflict_owner, &kind, &name)) {
            diag_set(diag_buf, diag_buf_sz,
                     "conflicting binding: host %s '%s' is bound to both '%s' and '%s'",
                     kind, name,
                     nk_psp_button_name((NkPspButton)conflict_owner),
                     nk_psp_button_name((NkPspButton)i));
            return NK_ERROR_GENERIC;
        }
    }

    /* Conflict detection across Navigation bindings */
    for (int i = 0; i < NK_HOST_BUTTON_COUNT; i++) button_owners[i] = -1;
    for (int i = 0; i < NK_HOST_AXIS_COUNT; i++) {
        trigger_owners[i] = -1;
        axis_pos_owners[i] = -1;
        axis_neg_owners[i] = -1;
    }

    for (int i = 0; i < NK_NAV_ACTION_COUNT; i++) {
        int conflict_owner = -1;
        const char *kind = NULL;
        const char *name = NULL;

        if (!check_source_conflict(&profile->nav_bindings[i].primary, i,
                                   button_owners, trigger_owners, axis_pos_owners, axis_neg_owners,
                                   &conflict_owner, &kind, &name)) {
            diag_set(diag_buf, diag_buf_sz,
                     "conflicting navigation binding: host %s '%s' is bound to both '%s' and '%s'",
                     kind, name,
                     nk_nav_action_name((NkNavAction)conflict_owner),
                     nk_nav_action_name((NkNavAction)i));
            return NK_ERROR_GENERIC;
        }

        if (!check_source_conflict(&profile->nav_bindings[i].secondary, i,
                                   button_owners, trigger_owners, axis_pos_owners, axis_neg_owners,
                                   &conflict_owner, &kind, &name)) {
            diag_set(diag_buf, diag_buf_sz,
                     "conflicting navigation binding: host %s '%s' is bound to both '%s' and '%s'",
                     kind, name,
                     nk_nav_action_name((NkNavAction)conflict_owner),
                     nk_nav_action_name((NkNavAction)i));
            return NK_ERROR_GENERIC;
        }
    }

    return NK_OK;
}

/* -----------------------------------------------------------------------------
 * JSON Serialization & Deserialization
 * -------------------------------------------------------------------------- */

static bool parse_binding_source_str(
    const char *str,
    NkBindingSource *out_src,
    char *diag_buf,
    size_t diag_buf_sz
) {
    if (!str || !*str || nk_ascii_casecmp(str, "none") == 0) {
        out_src->type = NK_BINDING_NONE;
        out_src->index = 0;
        return true;
    }

    /* Check prefix "button:" or "axis:" */
    if (strncmp(str, "button:", 7) == 0) {
        NkHostGamepadButton btn = nk_host_button_from_name(str + 7);
        if (btn == NK_HOST_BUTTON_INVALID) {
            diag_set(diag_buf, diag_buf_sz, "unknown host button '%s'", str + 7);
            return false;
        }
        out_src->type = NK_BINDING_HOST_BUTTON;
        out_src->index = (int)btn;
        return true;
    }

    if (strncmp(str, "trigger:", 8) == 0) {
        NkHostGamepadAxis ax = nk_host_axis_from_name(str + 8);
        if (ax == NK_HOST_AXIS_INVALID) {
            diag_set(diag_buf, diag_buf_sz, "unknown host axis '%s'", str + 8);
            return false;
        }
        out_src->type = NK_BINDING_HOST_TRIGGER;
        out_src->index = (int)ax;
        return true;
    }

    if (str[0] == '+') {
        NkHostGamepadAxis ax = nk_host_axis_from_name(str + 1);
        if (ax == NK_HOST_AXIS_INVALID) {
            diag_set(diag_buf, diag_buf_sz, "unknown host axis '%s'", str + 1);
            return false;
        }
        out_src->type = NK_BINDING_HOST_AXIS_POS;
        out_src->index = (int)ax;
        return true;
    }

    if (str[0] == '-') {
        NkHostGamepadAxis ax = nk_host_axis_from_name(str + 1);
        if (ax == NK_HOST_AXIS_INVALID) {
            diag_set(diag_buf, diag_buf_sz, "unknown host axis '%s'", str + 1);
            return false;
        }
        out_src->type = NK_BINDING_HOST_AXIS_NEG;
        out_src->index = (int)ax;
        return true;
    }

    /* Direct button check */
    NkHostGamepadButton btn = nk_host_button_from_name(str);
    if (btn != NK_HOST_BUTTON_INVALID) {
        out_src->type = NK_BINDING_HOST_BUTTON;
        out_src->index = (int)btn;
        return true;
    }

    /* Direct axis trigger check */
    NkHostGamepadAxis ax = nk_host_axis_from_name(str);
    if (ax == NK_HOST_AXIS_LEFT_TRIGGER || ax == NK_HOST_AXIS_RIGHT_TRIGGER) {
        out_src->type = NK_BINDING_HOST_TRIGGER;
        out_src->index = (int)ax;
        return true;
    }

    diag_set(diag_buf, diag_buf_sz, "unrecognized host input identifier '%s'", str);
    return false;
}

static void format_binding_source(const NkBindingSource *src, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!src || src->type == NK_BINDING_NONE) {
        snprintf(out, out_sz, "none");
        return;
    }
    switch (src->type) {
    case NK_BINDING_HOST_BUTTON:
        snprintf(out, out_sz, "%s", nk_host_button_name((NkHostGamepadButton)src->index));
        break;
    case NK_BINDING_HOST_TRIGGER:
        snprintf(out, out_sz, "%s", nk_host_axis_name((NkHostGamepadAxis)src->index));
        break;
    case NK_BINDING_HOST_AXIS_POS:
        snprintf(out, out_sz, "+%s", nk_host_axis_name((NkHostGamepadAxis)src->index));
        break;
    case NK_BINDING_HOST_AXIS_NEG:
        snprintf(out, out_sz, "-%s", nk_host_axis_name((NkHostGamepadAxis)src->index));
        break;
    default:
        snprintf(out, out_sz, "none");
        break;
    }
}

static inline void json_free(JsonNode *node) {
    nk_json_free(node);
}

static inline JsonNode *obj_get(const JsonNode *obj, const char *key) {
    return nk_json_obj_get(obj, key);
}

/* -----------------------------------------------------------------------------
 * Parse JSON into NkInputProfile
 * -------------------------------------------------------------------------- */

static bool parse_axis_node(
    const JsonNode *node,
    NkAxisCalibration *out_axis,
    const char *axis_name,
    char *diag_buf,
    size_t diag_buf_sz
) {
    if (!node || node->type != JSON_OBJECT) {
        diag_set(diag_buf, diag_buf_sz, "axis '%s' definition must be an object", axis_name);
        return false;
    }

    JsonNode *ha = obj_get(node, "host_axis");
    if (!ha || ha->type != JSON_STRING) {
        diag_set(diag_buf, diag_buf_sz, "axis '%s' missing 'host_axis' string", axis_name);
        return false;
    }
    NkHostGamepadAxis axis_idx = nk_host_axis_from_name(ha->u.str_val);
    if (axis_idx == NK_HOST_AXIS_INVALID) {
        diag_set(diag_buf, diag_buf_sz, "axis '%s' unknown host axis '%s'", axis_name, ha->u.str_val);
        return false;
    }
    out_axis->host_axis = axis_idx;

    JsonNode *dz_in = obj_get(node, "deadzone_inner");
    if (!dz_in || dz_in->type != JSON_NUMBER) {
        diag_set(diag_buf, diag_buf_sz, "axis '%s' missing 'deadzone_inner' number", axis_name);
        return false;
    }
    if (dz_in->u.num.num_val < 0 || dz_in->u.num.num_val > 32767) {
        diag_set(diag_buf, diag_buf_sz, "axis '%s' deadzone_inner %.0f out of range [0, 32767]",
                 axis_name, dz_in->u.num.num_val);
        return false;
    }
    out_axis->deadzone_inner = (int16_t)dz_in->u.num.num_val;

    JsonNode *dz_out = obj_get(node, "deadzone_outer");
    if (dz_out) {
        if (dz_out->type != JSON_NUMBER || dz_out->u.num.num_val < 0 || dz_out->u.num.num_val > 32767) {
            diag_set(diag_buf, diag_buf_sz, "axis '%s' deadzone_outer out of range [0, 32767]", axis_name);
            return false;
        }
        out_axis->deadzone_outer = (int16_t)dz_out->u.num.num_val;
    } else {
        out_axis->deadzone_outer = 0;
    }

    if ((int32_t)out_axis->deadzone_inner + (int32_t)out_axis->deadzone_outer >= 32767) {
        diag_set(diag_buf, diag_buf_sz, "axis '%s' combined deadzones (%d + %d) out of range (< 32767)",
                 axis_name, out_axis->deadzone_inner, out_axis->deadzone_outer);
        return false;
    }

    JsonNode *inv = obj_get(node, "inverted");
    if (inv) {
        if (inv->type != JSON_BOOL) {
            diag_set(diag_buf, diag_buf_sz, "axis '%s' 'inverted' must be boolean", axis_name);
            return false;
        }
        out_axis->inverted = inv->u.bool_val;
    } else {
        out_axis->inverted = false;
    }
    return true;
}

static bool parse_binding_node(
    const JsonNode *node,
    NkDigitalBinding *out_binding,
    char *diag_buf,
    size_t diag_buf_sz
) {
    if (!node) return false;
    out_binding->primary.type = NK_BINDING_NONE;
    out_binding->secondary.type = NK_BINDING_NONE;

    if (node->type == JSON_STRING) {
        return parse_binding_source_str(node->u.str_val, &out_binding->primary, diag_buf, diag_buf_sz);
    }

    if (node->type == JSON_OBJECT) {
        JsonNode *pri = obj_get(node, "primary");
        if (pri) {
            if (pri->type != JSON_STRING) {
                diag_set(diag_buf, diag_buf_sz, "binding 'primary' must be string");
                return false;
            }
            if (!parse_binding_source_str(pri->u.str_val, &out_binding->primary, diag_buf, diag_buf_sz)) {
                return false;
            }
        }
        JsonNode *sec = obj_get(node, "secondary");
        if (sec) {
            if (sec->type != JSON_STRING) {
                diag_set(diag_buf, diag_buf_sz, "binding 'secondary' must be string");
                return false;
            }
            if (!parse_binding_source_str(sec->u.str_val, &out_binding->secondary, diag_buf, diag_buf_sz)) {
                return false;
            }
        }
        return true;
    }

    diag_set(diag_buf, diag_buf_sz, "binding must be a string or object");
    return false;
}

NkResult nk_input_profile_parse_json(
    NkInputProfile *out_profile,
    const char *json_str,
    size_t json_len,
    char *diag_buf,
    size_t diag_buf_sz
) {
    if (diag_buf && diag_buf_sz > 0) diag_buf[0] = '\0';
    if (!out_profile) return NK_ERROR_GENERIC;

    nk_input_profile_init_default(out_profile);

    if (!json_str || json_len == 0) {
        diag_set(diag_buf, diag_buf_sz, "empty JSON input");
        return NK_ERROR_GENERIC;
    }

    JsonNode *root = nk_json_parse(json_str, json_len, diag_buf, diag_buf_sz);
    if (!root) {
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }

    if (root->type != JSON_OBJECT) {
        diag_set(diag_buf, diag_buf_sz, "root JSON node must be an object");
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }

    /* 1. schema_version */
    JsonNode *sv = obj_get(root, "schema_version");
    if (!sv || sv->type != JSON_NUMBER) {
        diag_set(diag_buf, diag_buf_sz, "missing or non-numeric schema_version");
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }
    int ver = (int)sv->u.num.num_val;
    if (ver != NK_INPUT_PROFILE_SCHEMA_VERSION) {
        if (ver > NK_INPUT_PROFILE_SCHEMA_VERSION) {
            diag_set(diag_buf, diag_buf_sz,
                     "unsupported future schema_version %d (current version is %d)",
                     ver, NK_INPUT_PROFILE_SCHEMA_VERSION);
        } else {
            diag_set(diag_buf, diag_buf_sz,
                     "unsupported schema_version %d (only version %d is supported)",
                     ver, NK_INPUT_PROFILE_SCHEMA_VERSION);
        }
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }
    out_profile->schema_version = ver;

    /* 2. Device Identity */
    JsonNode *dev = obj_get(root, "device");
    if (!dev || dev->type != JSON_OBJECT) {
        diag_set(diag_buf, diag_buf_sz, "missing 'device' object");
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }
    JsonNode *guid = obj_get(dev, "guid");
    if (!guid || guid->type != JSON_STRING || guid->u.str_val[0] == '\0') {
        diag_set(diag_buf, diag_buf_sz, "device missing non-empty 'guid' string");
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }
    snprintf(out_profile->guid, sizeof(out_profile->guid), "%s", guid->u.str_val);

    JsonNode *name = obj_get(dev, "name_hint");
    if (name && name->type == JSON_STRING) {
        snprintf(out_profile->name_hint, sizeof(out_profile->name_hint), "%s", name->u.str_val);
    } else {
        snprintf(out_profile->name_hint, sizeof(out_profile->name_hint), "Generic Controller");
    }

    /* 3. Calibration */
    JsonNode *cal = obj_get(root, "calibration");
    if (!cal || cal->type != JSON_OBJECT) {
        diag_set(diag_buf, diag_buf_sz, "missing 'calibration' object");
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }

    JsonNode *tt = obj_get(cal, "trigger_threshold");
    if (!tt || tt->type != JSON_NUMBER) {
        diag_set(diag_buf, diag_buf_sz, "calibration missing 'trigger_threshold'");
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }
    if (tt->u.num.num_val < 0 || tt->u.num.num_val > 32767) {
        diag_set(diag_buf, diag_buf_sz, "trigger_threshold %.0f out of range [0, 32767]", tt->u.num.num_val);
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }
    out_profile->trigger_threshold = (int16_t)tt->u.num.num_val;

    JsonNode *ax_x = obj_get(cal, "analog_x");
    if (!parse_axis_node(ax_x, &out_profile->axes[NK_PSP_AXIS_ANALOG_X], "analog_x", diag_buf, diag_buf_sz)) {
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }

    JsonNode *ax_y = obj_get(cal, "analog_y");
    if (!parse_axis_node(ax_y, &out_profile->axes[NK_PSP_AXIS_ANALOG_Y], "analog_y", diag_buf, diag_buf_sz)) {
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }

    /* 4. PSP Bindings */
    JsonNode *pb = obj_get(root, "psp_bindings");
    if (!pb || pb->type != JSON_ARRAY) {
        diag_set(diag_buf, diag_buf_sz, "missing 'psp_bindings' array");
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }

    bool bound_psp[NK_PSP_BTN_COUNT];
    memset(bound_psp, 0, sizeof(bound_psp));
    /* Clear default buttons before applying file's bindings */
    for (int i = 0; i < NK_PSP_BTN_COUNT; i++) {
        out_profile->psp_buttons[i].primary.type = NK_BINDING_NONE;
        out_profile->psp_buttons[i].secondary.type = NK_BINDING_NONE;
    }

    for (size_t i = 0; i < pb->u.arr.count; i++) {
        const JsonNode *item = pb->u.arr.items[i];
        if (!item || item->type != JSON_OBJECT) {
            diag_set(diag_buf, diag_buf_sz, "psp_bindings entry %zu must be an object", i);
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }

        JsonNode *ctrl = obj_get(item, "control");
        if (!ctrl || ctrl->type != JSON_STRING) {
            diag_set(diag_buf, diag_buf_sz, "psp_bindings entry %zu missing 'control' name", i);
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }

        NkPspButton btn = nk_psp_button_from_name(ctrl->u.str_val);
        if ((int)btn < 0 || (int)btn >= NK_PSP_BTN_COUNT) {
            diag_set(diag_buf, diag_buf_sz, "unknown PSP control '%s'", ctrl->u.str_val);
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }

        if (bound_psp[btn]) {
            diag_set(diag_buf, diag_buf_sz, "duplicate binding for control '%s'", ctrl->u.str_val);
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }
        bound_psp[btn] = true;

        if (!parse_binding_node(item, &out_profile->psp_buttons[btn], diag_buf, diag_buf_sz)) {
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }
    }

    /* 5. Navigation Bindings */
    JsonNode *nb = obj_get(root, "navigation_bindings");
    if (!nb || nb->type != JSON_ARRAY) {
        diag_set(diag_buf, diag_buf_sz, "missing 'navigation_bindings' array");
        json_free(root);
        nk_input_profile_init_default(out_profile);
        return NK_ERROR_GENERIC;
    }

    bool bound_nav[NK_NAV_ACTION_COUNT];
    memset(bound_nav, 0, sizeof(bound_nav));
    for (int i = 0; i < NK_NAV_ACTION_COUNT; i++) {
        out_profile->nav_bindings[i].primary.type = NK_BINDING_NONE;
        out_profile->nav_bindings[i].secondary.type = NK_BINDING_NONE;
    }

    for (size_t i = 0; i < nb->u.arr.count; i++) {
        const JsonNode *item = nb->u.arr.items[i];
        if (!item || item->type != JSON_OBJECT) {
            diag_set(diag_buf, diag_buf_sz, "navigation_bindings entry %zu must be an object", i);
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }

        JsonNode *act = obj_get(item, "action");
        if (!act || act->type != JSON_STRING) {
            diag_set(diag_buf, diag_buf_sz, "navigation_bindings entry %zu missing 'action' name", i);
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }

        NkNavAction action = nk_nav_action_from_name(act->u.str_val);
        if ((int)action < 0 || (int)action >= NK_NAV_ACTION_COUNT) {
            diag_set(diag_buf, diag_buf_sz, "unknown navigation action '%s'", act->u.str_val);
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }

        if (bound_nav[action]) {
            diag_set(diag_buf, diag_buf_sz, "duplicate binding for navigation action '%s'", act->u.str_val);
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }
        bound_nav[action] = true;

        if (!parse_binding_node(item, &out_profile->nav_bindings[action], diag_buf, diag_buf_sz)) {
            json_free(root);
            nk_input_profile_init_default(out_profile);
            return NK_ERROR_GENERIC;
        }
    }

    json_free(root);

    /* Final validation pass (checks range, deadzone sum, and conflicting bindings) */
    NkResult val_res = nk_input_profile_validate(out_profile, diag_buf, diag_buf_sz);
    if (val_res != NK_OK) {
        nk_input_profile_init_default(out_profile);
        return val_res;
    }

    return NK_OK;
}

/* -----------------------------------------------------------------------------
 * File Load and Atomic Save
 * -------------------------------------------------------------------------- */

static void escape_json(const char *src, char *dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return;
    size_t d = 0;
    for (size_t s = 0; src && src[s] && d + 2 < dst_sz; s++) {
        char c = src[s];
        if (c == '"' || c == '\\') {
            dst[d++] = '\\';
            dst[d++] = c;
        } else if (c == '\n') {
            dst[d++] = '\\'; dst[d++] = 'n';
        } else if (c == '\r') {
            dst[d++] = '\\'; dst[d++] = 'r';
        } else if (c == '\t') {
            dst[d++] = '\\'; dst[d++] = 't';
        } else {
            dst[d++] = c;
        }
    }
    dst[d] = '\0';
}

NkResult nk_input_profile_save(
    const NkInputProfile *profile,
    const char *file_path,
    char *diag_buf,
    size_t diag_buf_sz
) {
    if (diag_buf && diag_buf_sz > 0) diag_buf[0] = '\0';
    if (!profile || !file_path || !*file_path) {
        diag_set(diag_buf, diag_buf_sz, "invalid profile or file path");
        return NK_ERROR_GENERIC;
    }

    NkResult val_res = nk_input_profile_validate(profile, diag_buf, diag_buf_sz);
    if (val_res != NK_OK) return val_res;

    char tmp_path[NK_MAX_PATH + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", file_path);

    FILE *f = nk_input_fopen(tmp_path, "wb");
    if (!f) {
        diag_set(diag_buf, diag_buf_sz, "failed to open temporary file '%s' for writing", tmp_path);
        return NK_ERROR_IO;
    }

    char esc_guid[NK_INPUT_GUID_MAX_LEN * 2];
    char esc_name[NK_INPUT_NAME_HINT_MAX_LEN * 2];
    escape_json(profile->guid, esc_guid, sizeof(esc_guid));
    escape_json(profile->name_hint, esc_name, sizeof(esc_name));

    fprintf(f, "{\n");
    fprintf(f, "  \"schema_version\": %d,\n", profile->schema_version);
    fprintf(f, "  \"device\": {\n");
    fprintf(f, "    \"guid\": \"%s\",\n", esc_guid);
    fprintf(f, "    \"name_hint\": \"%s\"\n", esc_name);
    fprintf(f, "  },\n");
    fprintf(f, "  \"calibration\": {\n");
    fprintf(f, "    \"trigger_threshold\": %d,\n", profile->trigger_threshold);

    for (int i = 0; i < NK_PSP_AXIS_COUNT; i++) {
        const NkAxisCalibration *ax = &profile->axes[i];
        fprintf(f, "    \"%s\": {\n", nk_psp_axis_name((NkPspAxis)i));
        fprintf(f, "      \"host_axis\": \"%s\",\n", nk_host_axis_name(ax->host_axis));
        fprintf(f, "      \"deadzone_inner\": %d,\n", ax->deadzone_inner);
        fprintf(f, "      \"deadzone_outer\": %d,\n", ax->deadzone_outer);
        fprintf(f, "      \"inverted\": %s\n", ax->inverted ? "true" : "false");
        fprintf(f, "    }%s\n", (i < NK_PSP_AXIS_COUNT - 1) ? "," : "");
    }
    fprintf(f, "  },\n");

    /* PSP bindings */
    fprintf(f, "  \"psp_bindings\": [\n");
    for (int i = 0; i < NK_PSP_BTN_COUNT; i++) {
        char pri_str[64], sec_str[64];
        format_binding_source(&profile->psp_buttons[i].primary, pri_str, sizeof(pri_str));
        format_binding_source(&profile->psp_buttons[i].secondary, sec_str, sizeof(sec_str));

        fprintf(f, "    {\n");
        fprintf(f, "      \"control\": \"%s\",\n", nk_psp_button_name((NkPspButton)i));
        fprintf(f, "      \"primary\": \"%s\"", pri_str);
        if (profile->psp_buttons[i].secondary.type != NK_BINDING_NONE) {
            fprintf(f, ",\n      \"secondary\": \"%s\"\n", sec_str);
        } else {
            fprintf(f, "\n");
        }
        fprintf(f, "    }%s\n", (i < NK_PSP_BTN_COUNT - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    /* Navigation bindings */
    fprintf(f, "  \"navigation_bindings\": [\n");
    for (int i = 0; i < NK_NAV_ACTION_COUNT; i++) {
        char pri_str[64], sec_str[64];
        format_binding_source(&profile->nav_bindings[i].primary, pri_str, sizeof(pri_str));
        format_binding_source(&profile->nav_bindings[i].secondary, sec_str, sizeof(sec_str));

        fprintf(f, "    {\n");
        fprintf(f, "      \"action\": \"%s\",\n", nk_nav_action_name((NkNavAction)i));
        fprintf(f, "      \"primary\": \"%s\"", pri_str);
        if (profile->nav_bindings[i].secondary.type != NK_BINDING_NONE) {
            fprintf(f, ",\n      \"secondary\": \"%s\"\n", sec_str);
        } else {
            fprintf(f, "\n");
        }
        fprintf(f, "    }%s\n", (i < NK_NAV_ACTION_COUNT - 1) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    if (fflush(f) != 0) {
        fclose(f);
        remove(tmp_path);
        diag_set(diag_buf, diag_buf_sz, "failed to flush temporary profile file '%s'", tmp_path);
        return NK_ERROR_IO;
    }

#if defined(_WIN32) || defined(_WIN64)
    int fd = _fileno(f);
    if (fd >= 0) {
        HANDLE hFile = (HANDLE)_get_osfhandle(fd);
        if (hFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(hFile);
        }
    }
    fclose(f);

    WCHAR wtmp[32768], wtarget[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, tmp_path, -1, wtmp, 32768) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, file_path, -1, wtarget, 32768) <= 0) {
        DeleteFileA(tmp_path);
        diag_set(diag_buf, diag_buf_sz, "failed UTF-8 conversion for path '%s'", file_path);
        return NK_ERROR_IO;
    }

    if (!MoveFileExW(wtmp, wtarget, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(wtmp);
        diag_set(diag_buf, diag_buf_sz, "atomic replace failed for '%s'", file_path);
        return NK_ERROR_IO;
    }
#else
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);

    if (rename(tmp_path, file_path) != 0) {
        remove(tmp_path);
        diag_set(diag_buf, diag_buf_sz, "atomic rename failed from '%s' to '%s'", tmp_path, file_path);
        return NK_ERROR_IO;
    }
#endif

    return NK_OK;
}

NkResult nk_input_profile_load(
    NkInputProfile *out_profile,
    const char *file_path,
    char *diag_buf,
    size_t diag_buf_sz
) {
    if (diag_buf && diag_buf_sz > 0) diag_buf[0] = '\0';
    if (!out_profile) return NK_ERROR_GENERIC;

    nk_input_profile_init_default(out_profile);

    if (!file_path || !*file_path) {
        diag_set(diag_buf, diag_buf_sz, "file path is null or empty");
        return NK_ERROR_GENERIC;
    }

    if (!nk_platform_file_exists(file_path)) {
        diag_set(diag_buf, diag_buf_sz, "input profile file not found: '%s'", file_path);
        return NK_ERROR_FILE_NOT_FOUND;
    }

    FILE *f = nk_input_fopen(file_path, "rb");
    if (!f) {
        diag_set(diag_buf, diag_buf_sz, "failed to open profile file '%s'", file_path);
        return NK_ERROR_IO;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        diag_set(diag_buf, diag_buf_sz, "profile file size %ld is invalid or exceeds 1 MB", sz);
        return NK_ERROR_IO;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        diag_set(diag_buf, diag_buf_sz, "out of memory allocating file buffer");
        return NK_ERROR_OUT_OF_MEMORY;
    }

    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[read_bytes] = '\0';

    NkResult res = nk_input_profile_parse_json(out_profile, buf, read_bytes, diag_buf, diag_buf_sz);
    free(buf);
    return res;
}

NkResult nk_input_profile_resolve_path(char *out_path, size_t out_path_sz) {
    if (!out_path || out_path_sz == 0) return NK_ERROR_GENERIC;
    out_path[0] = '\0';

    const char *env_path = getenv("NK_INPUT_PROFILE");
    if (!env_path || !env_path[0]) {
        env_path = getenv("SR_INPUT_PROFILE");
    }

    if (env_path && env_path[0]) {
        if (strlen(env_path) >= out_path_sz) return NK_ERROR_GENERIC;
        strncpy(out_path, env_path, out_path_sz - 1);
        out_path[out_path_sz - 1] = '\0';
        return NK_OK;
    }

    char config_dir[1024] = {0};
    if (nk_platform_get_path(NK_PATH_CONFIG, config_dir, sizeof(config_dir))) {
        char sep = nk_platform_path_separator();
        int n = snprintf(out_path, out_path_sz, "%s%cinput_profile.json", config_dir, sep);
        if (n < 0 || (size_t)n >= out_path_sz) return NK_ERROR_GENERIC;
        return NK_OK;
    }

    return NK_ERROR_GENERIC;
}

bool nk_input_profile_padscript_active(void) {
    const char *sp = getenv("SR_PADSCRIPT");
    return sp && sp[0] != '\0';
}

/* -----------------------------------------------------------------------------
 * Runtime Evaluation Helpers
 * -------------------------------------------------------------------------- */

static bool eval_source(
    const NkBindingSource *src,
    const bool host_buttons[NK_HOST_BUTTON_COUNT],
    const int16_t host_axes[NK_HOST_AXIS_COUNT],
    int16_t trigger_threshold
) {
    if (!src || src->type == NK_BINDING_NONE) return false;
    switch (src->type) {
    case NK_BINDING_HOST_BUTTON:
        if (host_buttons && src->index >= 0 && src->index < NK_HOST_BUTTON_COUNT) {
            return host_buttons[src->index];
        }
        break;
    case NK_BINDING_HOST_TRIGGER:
        if (host_axes && src->index >= 0 && src->index < NK_HOST_AXIS_COUNT) {
            return host_axes[src->index] > trigger_threshold;
        }
        break;
    case NK_BINDING_HOST_AXIS_POS:
        if (host_axes && src->index >= 0 && src->index < NK_HOST_AXIS_COUNT) {
            return host_axes[src->index] > trigger_threshold;
        }
        break;
    case NK_BINDING_HOST_AXIS_NEG:
        if (host_axes && src->index >= 0 && src->index < NK_HOST_AXIS_COUNT) {
            return host_axes[src->index] < -trigger_threshold;
        }
        break;
    default:
        break;
    }
    return false;
}

uint32_t nk_input_profile_eval_buttons(
    const NkInputProfile *profile,
    const bool host_buttons[NK_HOST_BUTTON_COUNT],
    const int16_t host_axes[NK_HOST_AXIS_COUNT]
) {
    if (!profile) return 0;
    uint32_t mask = 0;
    for (int i = 0; i < NK_PSP_BTN_COUNT; i++) {
        const NkDigitalBinding *b = &profile->psp_buttons[i];
        bool pressed = false;
        if (eval_source(&b->primary, host_buttons, host_axes, profile->trigger_threshold)) {
            pressed = true;
        } else if (eval_source(&b->secondary, host_buttons, host_axes, profile->trigger_threshold)) {
            pressed = true;
        }
        if (pressed) {
            mask |= nk_psp_button_bitmask((NkPspButton)i);
        }
    }
    return mask;
}

void nk_input_profile_eval_analog(
    const NkInputProfile *profile,
    const int16_t host_axes[NK_HOST_AXIS_COUNT],
    uint8_t *out_lx,
    uint8_t *out_ly
) {
    if (out_lx) *out_lx = 128;
    if (out_ly) *out_ly = 128;
    if (!profile || !host_axes) return;

    int ax_idx = profile->axes[NK_PSP_AXIS_ANALOG_X].host_axis;
    if (ax_idx >= 0 && ax_idx < NK_HOST_AXIS_COUNT && out_lx) {
        *out_lx = nk_input_profile_transform_axis(
            host_axes[ax_idx],
            profile->axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner,
            profile->axes[NK_PSP_AXIS_ANALOG_X].deadzone_outer,
            profile->axes[NK_PSP_AXIS_ANALOG_X].inverted
        );
    }

    int ay_idx = profile->axes[NK_PSP_AXIS_ANALOG_Y].host_axis;
    if (ay_idx >= 0 && ay_idx < NK_HOST_AXIS_COUNT && out_ly) {
        *out_ly = nk_input_profile_transform_axis(
            host_axes[ay_idx],
            profile->axes[NK_PSP_AXIS_ANALOG_Y].deadzone_inner,
            profile->axes[NK_PSP_AXIS_ANALOG_Y].deadzone_outer,
            profile->axes[NK_PSP_AXIS_ANALOG_Y].inverted
        );
    }
}

uint32_t nk_input_profile_eval_navigation(
    const NkInputProfile *profile,
    const bool host_buttons[NK_HOST_BUTTON_COUNT],
    const int16_t host_axes[NK_HOST_AXIS_COUNT]
) {
    if (!profile) return 0;
    uint32_t mask = 0;
    for (int i = 0; i < NK_NAV_ACTION_COUNT; i++) {
        const NkDigitalBinding *b = &profile->nav_bindings[i];
        bool active = false;
        if (eval_source(&b->primary, host_buttons, host_axes, profile->trigger_threshold)) {
            active = true;
        } else if (eval_source(&b->secondary, host_buttons, host_axes, profile->trigger_threshold)) {
            active = true;
        }
        if (active) {
            mask |= (1u << i);
        }
    }
    return mask;
}
