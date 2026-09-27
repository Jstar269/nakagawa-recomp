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
    nk_input_profile_file_init_default(&state->file);
    state->profile = state->file.global;
    state->profile_path[0] = '\0';
    nk_input_profile_resolve_path(state->profile_path, sizeof(state->profile_path));
    state->capture_control = -1;
    state->capture_timeout_ms = INPUT_SETTINGS_CAPTURE_TIMEOUT_MS;
    input_settings_refresh_conflicts(state);
}

/* Copy the mapping being edited back into the document slot it belongs to, so a
 * save writes the user's edits to the right mapping and leaves the others be. */
static void store_current_scope(InputSettingsState *state) {
    if (!state) return;
    if (!state->editing_disc_id[0]) {
        state->file.global = state->profile;
        return;
    }
    nk_input_profile_file_set_title(&state->file, state->editing_disc_id,
                                    &state->profile, NULL, 0);
}

void input_settings_reset_to_defaults(InputSettingsState *state) {
    if (!state) return;
    nk_input_profile_init_default(&state->profile);
    state->capturing = false;
    state->capture_control = -1;
    state->capture_elapsed_ms = 0;
    state->save_diagnostic[0] = '\0';
    state->has_save_diagnostic = false;
    store_current_scope(state);
    input_settings_refresh_conflicts(state);
}

bool input_settings_is_title_scope(const InputSettingsState *state) {
    return state && state->editing_disc_id[0] != '\0';
}

bool input_settings_has_title_mapping(const InputSettingsState *state, const char *disc_id) {
    if (!state || !disc_id || !disc_id[0]) return false;
    return nk_input_profile_file_find_title(&state->file, disc_id) >= 0;
}

const char *input_settings_scope_label(const InputSettingsState *state) {
    if (!state) return "Global mapping";
    if (!state->editing_disc_id[0]) return "Global mapping";
    return state->editing_disc_id;
}

const NkInputProfile *input_settings_resolve_for_disc(
    const InputSettingsState *state,
    const char *disc_id,
    char *diag_buf,
    size_t diag_buf_sz
) {
    if (!state) {
        if (diag_buf && diag_buf_sz > 0) diag_buf[0] = '\0';
        return NULL;
    }
    return nk_input_profile_file_resolve(&state->file, disc_id, diag_buf, diag_buf_sz);
}

bool input_settings_set_scope(InputSettingsState *state, const char *disc_id) {
    if (!state) return false;
    /* Edits made in the scope being left are kept, so switching back and forth
     * between the global mapping and a disc's own mapping never loses work. */
    store_current_scope(state);
    if (!disc_id || !disc_id[0]) {
        state->profile = state->file.global;
        state->editing_disc_id[0] = '\0';
        input_settings_cancel_capture(state);
        input_settings_cancel_calibration(state);
        input_settings_refresh_conflicts(state);
        return true;
    }

    if (!nk_input_profile_disc_id_safe(disc_id)) return false;

    /* A disc that has no entry yet starts from a copy of the global mapping, so
     * the first edit in a per-title scope changes only what the user changed. */
    if (nk_input_profile_file_find_title(&state->file, disc_id) < 0) {
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
        if (nk_input_profile_file_set_title(&state->file, disc_id, &state->file.global,
                                            diag, sizeof(diag)) != NK_OK) {
            return false;
        }
    }
    int idx = nk_input_profile_file_find_title(&state->file, disc_id);
    if (idx < 0) return false;
    state->profile = state->file.title[idx];
    snprintf(state->editing_disc_id, sizeof(state->editing_disc_id), "%s", disc_id);
    input_settings_cancel_capture(state);
    input_settings_cancel_calibration(state);
    input_settings_refresh_conflicts(state);
    return true;
}

bool input_settings_toggle_scope(InputSettingsState *state, const char *disc_id) {
    if (!state) return false;
    if (input_settings_is_title_scope(state)) {
        /* Choosing the global mapping for this disc drops its own entry, so the
         * next launch really does run the global profile instead of a custom
         * mapping the user just said they no longer want. */
        char leaving[NK_MAX_DISC_ID_LEN];
        snprintf(leaving, sizeof(leaving), "%s", state->editing_disc_id);
        if (!input_settings_set_scope(state, NULL)) return false;
        return nk_input_profile_file_remove_title(&state->file, leaving);
    }
    return input_settings_set_scope(state, disc_id);
}

NkResult input_settings_write_disc_profile(
    const InputSettingsState *state,
    const char *disc_id,
    char *out_path,
    size_t out_path_sz,
    char *diag_buf,
    size_t diag_buf_sz
) {
    if (diag_buf && diag_buf_sz > 0) diag_buf[0] = '\0';
    if (out_path && out_path_sz > 0) out_path[0] = '\0';
    if (!state || !out_path || out_path_sz == 0) return NK_ERROR_GENERIC;
    if (!nk_input_profile_disc_id_safe(disc_id)) {
        snprintf(diag_buf, diag_buf_sz,
                 "disc ID '%s' cannot name a per-title profile file",
                 disc_id ? disc_id : "");
        return NK_ERROR_GENERIC;
    }

    const NkInputProfile *effective =
        nk_input_profile_file_resolve(&state->file, disc_id, diag_buf, diag_buf_sz);
    if (!effective) {
        snprintf(diag_buf, diag_buf_sz, "no input profile document is loaded");
        return NK_ERROR_GENERIC;
    }

    char config_dir[1024] = {0};
    if (!nk_platform_get_path(NK_PATH_CONFIG, config_dir, sizeof(config_dir))) {
        snprintf(diag_buf, diag_buf_sz, "per-user config directory is unavailable");
        return NK_ERROR_IO;
    }
    char sep = nk_platform_path_separator();
    char dir[sizeof(config_dir) + 32];
    int written = snprintf(dir, sizeof(dir), "%s%c%s", config_dir, sep, "input_profiles");
    if (written <= 0 || (size_t)written >= sizeof(dir)) {
        snprintf(diag_buf, diag_buf_sz, "per-title profile path is too long");
        return NK_ERROR_IO;
    }
    if (!nk_platform_dir_exists(dir) && !nk_platform_mkdir_p(dir)) {
        snprintf(diag_buf, diag_buf_sz, "could not create '%s'", dir);
        return NK_ERROR_IO;
    }

    written = snprintf(out_path, out_path_sz, "%s%c%s.json", dir, sep, disc_id);
    if (written <= 0 || (size_t)written >= out_path_sz) {
        out_path[0] = '\0';
        snprintf(diag_buf, diag_buf_sz, "per-title profile path is too long");
        return NK_ERROR_IO;
    }

    return nk_input_profile_save(effective, out_path, diag_buf, diag_buf_sz);
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
    NkResult res = nk_input_profile_file_load(&state->file, load_path,
                                              state->load_diagnostic,
                                              sizeof(state->load_diagnostic));
    if (res != NK_OK) {
        nk_input_profile_file_init_default(&state->file);
        state->has_load_diagnostic = true;
    } else {
        state->has_load_diagnostic = false;
    }
    /* A load returns editing to the global mapping: the document it just read
     * is the authority for what exists, and the per-title scopes are reachable
     * again through input_settings_set_scope. */
    state->editing_disc_id[0] = '\0';
    state->profile = state->file.global;
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

    /* The whole document is written, so entries for other discs are preserved. */
    store_current_scope(state);
    NkResult res = nk_input_profile_file_save(&state->file, save_path,
                                              state->save_diagnostic,
                                              sizeof(state->save_diagnostic));
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

bool input_settings_start_calibration(InputSettingsState *state) {
    if (!state) return false;
    memset(&state->calib, 0, sizeof(state->calib));
    state->calib.stage = CALIBRATION_STAGE_REST;
    state->calib.duration_ms = 1000;
    return true;
}

void input_settings_cancel_calibration(InputSettingsState *state) {
    if (!state) return;
    memset(&state->calib, 0, sizeof(state->calib));
    state->calib.stage = CALIBRATION_STAGE_INACTIVE;
}

bool input_settings_is_calibrating(const InputSettingsState *state) {
    return state && state->calib.stage != CALIBRATION_STAGE_INACTIVE;
}

GuidedCalibrationStage input_settings_get_calibration_stage(const InputSettingsState *state) {
    return state ? state->calib.stage : CALIBRATION_STAGE_INACTIVE;
}

bool input_settings_update_calibration(InputSettingsState *state, int delta_ms, const int16_t host_axes[NK_HOST_AXIS_COUNT]) {
    if (!state || state->calib.stage == CALIBRATION_STAGE_INACTIVE) return false;

    int ax_x = state->profile.axes[NK_PSP_AXIS_ANALOG_X].host_axis;
    int ax_y = state->profile.axes[NK_PSP_AXIS_ANALOG_Y].host_axis;
    int16_t cur_x = (host_axes && ax_x >= 0 && ax_x < NK_HOST_AXIS_COUNT) ? host_axes[ax_x] : 0;
    int16_t cur_y = (host_axes && ax_y >= 0 && ax_y < NK_HOST_AXIS_COUNT) ? host_axes[ax_y] : 0;
    int16_t cur_lt = (host_axes && NK_HOST_AXIS_LEFT_TRIGGER < NK_HOST_AXIS_COUNT) ? host_axes[NK_HOST_AXIS_LEFT_TRIGGER] : 0;
    int16_t cur_rt = (host_axes && NK_HOST_AXIS_RIGHT_TRIGGER < NK_HOST_AXIS_COUNT) ? host_axes[NK_HOST_AXIS_RIGHT_TRIGGER] : 0;

    if (state->calib.stage == CALIBRATION_STAGE_REST) {
        state->calib.rest_x_acc += cur_x;
        state->calib.rest_y_acc += cur_y;
        state->calib.rest_lt_acc += cur_lt;
        state->calib.rest_rt_acc += cur_rt;
        state->calib.sample_count++;
        state->calib.elapsed_ms += delta_ms;

        if (state->calib.elapsed_ms >= state->calib.duration_ms) {
            if (state->calib.sample_count > 0) {
                state->calib.sampled_rest_x = (int16_t)(state->calib.rest_x_acc / state->calib.sample_count);
                state->calib.sampled_rest_y = (int16_t)(state->calib.rest_y_acc / state->calib.sample_count);
                state->calib.sampled_rest_lt = (int16_t)(state->calib.rest_lt_acc / state->calib.sample_count);
                state->calib.sampled_rest_rt = (int16_t)(state->calib.rest_rt_acc / state->calib.sample_count);
            } else {
                state->calib.sampled_rest_x = cur_x;
                state->calib.sampled_rest_y = cur_y;
                state->calib.sampled_rest_lt = cur_lt;
                state->calib.sampled_rest_rt = cur_rt;
            }

            state->calib.sampled_min_x = state->calib.sampled_rest_x;
            state->calib.sampled_max_x = state->calib.sampled_rest_x;
            state->calib.sampled_min_y = state->calib.sampled_rest_y;
            state->calib.sampled_max_y = state->calib.sampled_rest_y;
            state->calib.sampled_max_lt = state->calib.sampled_rest_lt;
            state->calib.sampled_max_rt = state->calib.sampled_rest_rt;

            state->calib.stage = CALIBRATION_STAGE_EXTREMES;
            state->calib.elapsed_ms = 0;
        }
        return true;
    }

    if (state->calib.stage == CALIBRATION_STAGE_EXTREMES) {
        state->calib.elapsed_ms += delta_ms;
        if (cur_x < state->calib.sampled_min_x) state->calib.sampled_min_x = cur_x;
        if (cur_x > state->calib.sampled_max_x) state->calib.sampled_max_x = cur_x;
        if (cur_y < state->calib.sampled_min_y) state->calib.sampled_min_y = cur_y;
        if (cur_y > state->calib.sampled_max_y) state->calib.sampled_max_y = cur_y;
        if (cur_lt > state->calib.sampled_max_lt) state->calib.sampled_max_lt = cur_lt;
        if (cur_rt > state->calib.sampled_max_rt) state->calib.sampled_max_rt = cur_rt;
        return true;
    }

    return true;
}

bool input_settings_finish_calibration_extremes(InputSettingsState *state) {
    if (!state || state->calib.stage != CALIBRATION_STAGE_EXTREMES) return false;

    state->calib.result_rest_x = state->calib.sampled_rest_x;
    state->calib.result_min_x = (state->calib.sampled_min_x < state->calib.result_rest_x - 4000)
                                ? state->calib.sampled_min_x : -32768;
    state->calib.result_max_x = (state->calib.sampled_max_x > state->calib.result_rest_x + 4000)
                                ? state->calib.sampled_max_x : 32767;

    state->calib.result_rest_y = state->calib.sampled_rest_y;
    state->calib.result_min_y = (state->calib.sampled_min_y < state->calib.result_rest_y - 4000)
                                ? state->calib.sampled_min_y : -32768;
    state->calib.result_max_y = (state->calib.sampled_max_y > state->calib.result_rest_y + 4000)
                                ? state->calib.sampled_max_y : 32767;

    int16_t rest_trig = (state->calib.sampled_rest_lt > state->calib.sampled_rest_rt)
                        ? state->calib.sampled_rest_lt : state->calib.sampled_rest_rt;
    int16_t ext_trig = (state->calib.sampled_max_lt > state->calib.sampled_max_rt)
                       ? state->calib.sampled_max_lt : state->calib.sampled_max_rt;

    if (ext_trig <= rest_trig + 1000) {
        ext_trig = 32767;
        if (rest_trig >= ext_trig) rest_trig = 0;
    }

    state->calib.result_trigger_rest = rest_trig;
    state->calib.result_trigger_extreme = ext_trig;

    state->calib.stage = CALIBRATION_STAGE_RESULT;
    return true;
}

bool input_settings_accept_calibration(InputSettingsState *state) {
    if (!state || state->calib.stage != CALIBRATION_STAGE_RESULT) return false;

    state->profile.axes[NK_PSP_AXIS_ANALOG_X].rest = state->calib.result_rest_x;
    state->profile.axes[NK_PSP_AXIS_ANALOG_X].min_val = state->calib.result_min_x;
    state->profile.axes[NK_PSP_AXIS_ANALOG_X].max_val = state->calib.result_max_x;

    state->profile.axes[NK_PSP_AXIS_ANALOG_Y].rest = state->calib.result_rest_y;
    state->profile.axes[NK_PSP_AXIS_ANALOG_Y].min_val = state->calib.result_min_y;
    state->profile.axes[NK_PSP_AXIS_ANALOG_Y].max_val = state->calib.result_max_y;

    state->profile.trigger_rest = state->calib.result_trigger_rest;
    state->profile.trigger_extreme = state->calib.result_trigger_extreme;

    memset(&state->calib, 0, sizeof(state->calib));
    state->calib.stage = CALIBRATION_STAGE_INACTIVE;
    return true;
}
