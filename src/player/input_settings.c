/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "input_settings.h"
#include "nk_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kControlDisplayNames[INPUT_CONTROL_TOTAL_COUNT] = {
    "Select",
    "Start",
    "D-Pad Up",
    "D-Pad Right",
    "D-Pad Down",
    "D-Pad Left",
    "L Trigger (L1)",
    "R Trigger (R1)",
    "Triangle",
    "Circle",
    "Cross",
    "Square",
    "Home",
    "Hold",
    "Analog Stick"
};

const char *input_settings_control_name(int control_idx) {
    if (control_idx >= 0 && control_idx < INPUT_CONTROL_TOTAL_COUNT) {
        return kControlDisplayNames[control_idx];
    }
    return "Unknown Control";
}

static const char *friendly_host_button_name(NkHostGamepadButton btn) {
    switch (btn) {
    case NK_HOST_BUTTON_SOUTH:          return "Button South (A / Cross)";
    case NK_HOST_BUTTON_EAST:           return "Button East (B / Circle)";
    case NK_HOST_BUTTON_WEST:           return "Button West (X / Square)";
    case NK_HOST_BUTTON_NORTH:          return "Button North (Y / Triangle)";
    case NK_HOST_BUTTON_BACK:           return "Button Back / Select";
    case NK_HOST_BUTTON_START:          return "Button Start";
    case NK_HOST_BUTTON_LEFT_STICK:     return "Left Stick Click";
    case NK_HOST_BUTTON_RIGHT_STICK:    return "Right Stick Click";
    case NK_HOST_BUTTON_LEFT_SHOULDER:  return "Left Bumper (LB / L1)";
    case NK_HOST_BUTTON_RIGHT_SHOULDER: return "Right Bumper (RB / R1)";
    case NK_HOST_BUTTON_DPAD_UP:        return "D-Pad Up";
    case NK_HOST_BUTTON_DPAD_DOWN:      return "D-Pad Down";
    case NK_HOST_BUTTON_DPAD_LEFT:      return "D-Pad Left";
    case NK_HOST_BUTTON_DPAD_RIGHT:     return "D-Pad Right";
    case NK_HOST_BUTTON_GUIDE:          return "Guide / Home";
    case NK_HOST_BUTTON_MISC1:          return "Misc Button";
    case NK_HOST_BUTTON_TOUCHPAD:       return "Touchpad";
    default:                            return nk_host_button_name(btn);
    }
}

static const char *friendly_host_axis_name(NkHostGamepadAxis axis) {
    switch (axis) {
    case NK_HOST_AXIS_LEFTX:         return "Left Stick X";
    case NK_HOST_AXIS_LEFTY:         return "Left Stick Y";
    case NK_HOST_AXIS_RIGHTX:        return "Right Stick X";
    case NK_HOST_AXIS_RIGHTY:        return "Right Stick Y";
    case NK_HOST_AXIS_LEFT_TRIGGER:  return "Left Trigger (LT / L2)";
    case NK_HOST_AXIS_RIGHT_TRIGGER: return "Right Trigger (RT / R2)";
    default:                         return nk_host_axis_name(axis);
    }
}

static void format_single_source(const NkBindingSource *src, char *buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return;
    buf[0] = '\0';
    if (!src || src->type == NK_BINDING_NONE) {
        snprintf(buf, buf_sz, "None (Unbound)");
        return;
    }
    switch (src->type) {
    case NK_BINDING_HOST_BUTTON:
        snprintf(buf, buf_sz, "%s", friendly_host_button_name((NkHostGamepadButton)src->index));
        break;
    case NK_BINDING_HOST_TRIGGER:
        snprintf(buf, buf_sz, "%s", friendly_host_axis_name((NkHostGamepadAxis)src->index));
        break;
    case NK_BINDING_HOST_AXIS_POS:
        snprintf(buf, buf_sz, "%s +", friendly_host_axis_name((NkHostGamepadAxis)src->index));
        break;
    case NK_BINDING_HOST_AXIS_NEG:
        snprintf(buf, buf_sz, "%s -", friendly_host_axis_name((NkHostGamepadAxis)src->index));
        break;
    default:
        snprintf(buf, buf_sz, "Unknown");
        break;
    }
}

void input_settings_format_binding(const InputSettingsState *state, int control_idx, char *buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return;
    buf[0] = '\0';
    if (!state || control_idx < 0 || control_idx >= INPUT_CONTROL_TOTAL_COUNT) {
        snprintf(buf, buf_sz, "None");
        return;
    }

    if (control_idx == INPUT_CONTROL_ANALOG_STICK) {
        int dz_x = state->profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner;
        bool inv_x = state->profile.axes[NK_PSP_AXIS_ANALOG_X].inverted;
        bool inv_y = state->profile.axes[NK_PSP_AXIS_ANALOG_Y].inverted;
        snprintf(buf, buf_sz, "Left Stick (DZ: %d%s%s)",
                 dz_x,
                 inv_x ? ", Invert X" : "",
                 inv_y ? ", Invert Y" : "");
        return;
    }

    const NkDigitalBinding *bind = &state->profile.psp_buttons[control_idx];
    char primary_str[96] = {0};
    format_single_source(&bind->primary, primary_str, sizeof(primary_str));

    if (bind->secondary.type != NK_BINDING_NONE) {
        char sec_str[96] = {0};
        format_single_source(&bind->secondary, sec_str, sizeof(sec_str));
        snprintf(buf, buf_sz, "%s / %s", primary_str, sec_str);
    } else {
        snprintf(buf, buf_sz, "%s", primary_str);
    }
}

void input_settings_init(InputSettingsState *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    nk_input_profile_init_default(&state->profile);
    nk_input_profile_resolve_path(state->profile_path, sizeof(state->profile_path));
    state->capture_control = -1;
    state->capture_timeout_ms = INPUT_SETTINGS_CAPTURE_TIMEOUT_MS;
    input_settings_refresh_conflicts(state);
}

void input_settings_reset_to_defaults(InputSettingsState *state) {
    if (!state) return;
    nk_input_profile_init_default(&state->profile);
    state->capturing = false;
    state->capture_control = -1;
    state->capture_elapsed_ms = 0;
    state->save_diagnostic[0] = '\0';
    state->has_save_diagnostic = false;
    input_settings_refresh_conflicts(state);
}

static bool sources_equal(const NkBindingSource *a, const NkBindingSource *b) {
    if (!a || !b) return false;
    if (a->type == NK_BINDING_NONE || b->type == NK_BINDING_NONE) return false;
    return (a->type == b->type && a->index == b->index);
}

void input_settings_refresh_conflicts(InputSettingsState *state) {
    if (!state) return;
    state->conflict_count = 0;

    for (int i = 0; i < INPUT_CONTROL_DIGITAL_COUNT; i++) {
        for (int j = i + 1; j < INPUT_CONTROL_DIGITAL_COUNT; j++) {
            const NkDigitalBinding *b_i = &state->profile.psp_buttons[i];
            const NkDigitalBinding *b_j = &state->profile.psp_buttons[j];

            /* Check primary vs primary */
            if (sources_equal(&b_i->primary, &b_j->primary)) {
                if (state->conflict_count < INPUT_SETTINGS_MAX_CONFLICTS) {
                    InputBindingConflict *c = &state->conflicts[state->conflict_count++];
                    c->control_a = i;
                    c->control_b = j;
                    c->source = b_i->primary;
                    char src_name[96] = {0};
                    format_single_source(&b_i->primary, src_name, sizeof(src_name));
                    snprintf(c->description, sizeof(c->description),
                             "Conflict: %s bound to both %s and %s",
                             src_name,
                             input_settings_control_name(i),
                             input_settings_control_name(j));
                }
            }

            /* Check secondary conflicts if present */
            if (sources_equal(&b_i->secondary, &b_j->primary) ||
                sources_equal(&b_i->secondary, &b_j->secondary)) {
                if (state->conflict_count < INPUT_SETTINGS_MAX_CONFLICTS) {
                    InputBindingConflict *c = &state->conflicts[state->conflict_count++];
                    c->control_a = i;
                    c->control_b = j;
                    c->source = b_i->secondary;
                    char src_name[96] = {0};
                    format_single_source(&b_i->secondary, src_name, sizeof(src_name));
                    snprintf(c->description, sizeof(c->description),
                             "Conflict: %s bound to both %s and %s",
                             src_name,
                             input_settings_control_name(i),
                             input_settings_control_name(j));
                }
            }
        }
    }
}

bool input_settings_has_conflicts(const InputSettingsState *state) {
    return state && state->conflict_count > 0;
}

int input_settings_get_conflict_count(const InputSettingsState *state) {
    return state ? state->conflict_count : 0;
}

const InputBindingConflict *input_settings_get_conflict(const InputSettingsState *state, int index) {
    if (!state || index < 0 || index >= state->conflict_count) return NULL;
    return &state->conflicts[index];
}

bool input_settings_is_control_conflicted(const InputSettingsState *state, int control_idx) {
    if (!state || control_idx < 0 || control_idx >= INPUT_CONTROL_DIGITAL_COUNT) return false;
    for (int i = 0; i < state->conflict_count; i++) {
        if (state->conflicts[i].control_a == control_idx || state->conflicts[i].control_b == control_idx) {
            return true;
        }
    }
    return false;
}

const char *input_settings_get_conflict_summary(const InputSettingsState *state) {
    if (!state || state->conflict_count == 0) return "";
    return state->conflicts[0].description;
}

NkResult input_settings_load(InputSettingsState *state, const char *custom_path) {
    if (!state) return NK_ERROR_GENERIC;
    input_settings_init(state);

    const char *load_path = custom_path;
    if (!load_path || !load_path[0]) {
        if (nk_input_profile_resolve_path(state->profile_path, sizeof(state->profile_path)) != NK_OK) {
            return NK_OK;
        }
        load_path = state->profile_path;
    } else {
        strncpy(state->profile_path, load_path, sizeof(state->profile_path) - 1);
        state->profile_path[sizeof(state->profile_path) - 1] = '\0';
    }

    if (!nk_platform_file_exists(load_path)) {
        state->loaded_from_file = false;
        state->has_load_diagnostic = false;
        state->load_diagnostic[0] = '\0';
        input_settings_refresh_conflicts(state);
        return NK_OK;
    }

    state->loaded_from_file = true;
    NkResult res = nk_input_profile_load(&state->profile, load_path, state->load_diagnostic, sizeof(state->load_diagnostic));
    if (res != NK_OK) {
        nk_input_profile_init_default(&state->profile);
        state->has_load_diagnostic = true;
    } else {
        state->has_load_diagnostic = false;
    }
    input_settings_refresh_conflicts(state);
    return res;
}

NkResult input_settings_save(InputSettingsState *state, const char *custom_path) {
    if (!state) return NK_ERROR_GENERIC;
    const char *save_path = custom_path;
    if (!save_path || !save_path[0]) {
        if (!state->profile_path[0]) {
            if (nk_input_profile_resolve_path(state->profile_path, sizeof(state->profile_path)) != NK_OK) {
                snprintf(state->save_diagnostic, sizeof(state->save_diagnostic), "unable to resolve profile save path");
                state->has_save_diagnostic = true;
                return NK_ERROR_GENERIC;
            }
        }
        save_path = state->profile_path;
    }

    state->save_diagnostic[0] = '\0';
    state->has_save_diagnostic = false;

    NkResult res = nk_input_profile_save(&state->profile, save_path, state->save_diagnostic, sizeof(state->save_diagnostic));
    if (res != NK_OK) {
        state->has_save_diagnostic = true;
    }
    return res;
}

bool input_settings_start_capture(InputSettingsState *state, int control_idx) {
    if (!state || control_idx < 0 || control_idx >= INPUT_CONTROL_DIGITAL_COUNT) return false;
    state->capturing = true;
    state->capture_control = control_idx;
    state->capture_elapsed_ms = 0;
    state->capture_timeout_ms = INPUT_SETTINGS_CAPTURE_TIMEOUT_MS;
    return true;
}

void input_settings_cancel_capture(InputSettingsState *state) {
    if (!state) return;
    state->capturing = false;
    state->capture_control = -1;
    state->capture_elapsed_ms = 0;
}

bool input_settings_is_capturing(const InputSettingsState *state) {
    return state && state->capturing;
}

int input_settings_get_capture_control(const InputSettingsState *state) {
    return (state && state->capturing) ? state->capture_control : -1;
}

int input_settings_get_capture_remaining_ms(const InputSettingsState *state) {
    if (!state || !state->capturing) return 0;
    int rem = state->capture_timeout_ms - state->capture_elapsed_ms;
    return (rem > 0) ? rem : 0;
}

bool input_settings_update_capture(InputSettingsState *state, int delta_ms) {
    if (!state || !state->capturing) return false;
    state->capture_elapsed_ms += delta_ms;
    if (state->capture_elapsed_ms >= state->capture_timeout_ms) {
        state->capturing = false;
        state->capture_control = -1;
        state->capture_elapsed_ms = 0;
        return false;
    }
    return true;
}

bool input_settings_feed_capture_source(InputSettingsState *state, NkBindingSource source) {
    if (!state || !state->capturing) return false;
    int c = state->capture_control;
    state->capturing = false;
    state->capture_control = -1;
    state->capture_elapsed_ms = 0;
    return input_settings_assign_binding(state, c, source);
}

bool input_settings_assign_binding(InputSettingsState *state, int control_idx, NkBindingSource source) {
    if (!state || control_idx < 0 || control_idx >= INPUT_CONTROL_DIGITAL_COUNT) return false;
    state->profile.psp_buttons[control_idx].primary = source;
    input_settings_refresh_conflicts(state);
    return true;
}

bool input_settings_adjust_deadzone(InputSettingsState *state, int axis_idx, int32_t delta) {
    if (!state) return false;
    int start = (axis_idx < 0) ? 0 : axis_idx;
    int end = (axis_idx < 0) ? NK_PSP_AXIS_COUNT - 1 : axis_idx;
    if (start < 0 || end >= NK_PSP_AXIS_COUNT) return false;

    for (int i = start; i <= end; i++) {
        int32_t val = (int32_t)state->profile.axes[i].deadzone_inner + delta;
        int32_t max_allowed = 32766 - state->profile.axes[i].deadzone_outer;
        if (max_allowed < 0) max_allowed = 0;
        if (val < 0) val = 0;
        if (val > max_allowed) val = max_allowed;
        state->profile.axes[i].deadzone_inner = (int16_t)val;
    }
    return true;
}

bool input_settings_set_deadzone(InputSettingsState *state, int axis_idx, int32_t value) {
    if (!state) return false;
    int start = (axis_idx < 0) ? 0 : axis_idx;
    int end = (axis_idx < 0) ? NK_PSP_AXIS_COUNT - 1 : axis_idx;
    if (start < 0 || end >= NK_PSP_AXIS_COUNT) return false;

    for (int i = start; i <= end; i++) {
        int32_t val = value;
        int32_t max_allowed = 32766 - state->profile.axes[i].deadzone_outer;
        if (max_allowed < 0) max_allowed = 0;
        if (val < 0) val = 0;
        if (val > max_allowed) val = max_allowed;
        state->profile.axes[i].deadzone_inner = (int16_t)val;
    }
    return true;
}

bool input_settings_adjust_trigger_threshold(InputSettingsState *state, int32_t delta) {
    if (!state) return false;
    int32_t val = (int32_t)state->profile.trigger_threshold + delta;
    if (val < 0) val = 0;
    if (val > 32767) val = 32767;
    state->profile.trigger_threshold = (int16_t)val;
    return true;
}

bool input_settings_set_trigger_threshold(InputSettingsState *state, int32_t value) {
    if (!state) return false;
    int32_t val = value;
    if (val < 0) val = 0;
    if (val > 32767) val = 32767;
    state->profile.trigger_threshold = (int16_t)val;
    return true;
}

bool input_settings_toggle_axis_inversion(InputSettingsState *state, int axis_idx) {
    if (!state) return false;
    if (axis_idx < 0 || axis_idx >= NK_PSP_AXIS_COUNT) return false;
    state->profile.axes[axis_idx].inverted = !state->profile.axes[axis_idx].inverted;
    return true;
}
