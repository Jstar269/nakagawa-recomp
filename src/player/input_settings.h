/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_INPUT_SETTINGS_H
#define NAKAGAWA_INPUT_SETTINGS_H

#include "nk_input_profile.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_SETTINGS_CAPTURE_TIMEOUT_MS 5000
#define INPUT_SETTINGS_MAX_CONFLICTS 32

/**
 * Control items editable in the UI: 14 digital PSP buttons + analog stick.
 */
typedef enum {
    INPUT_CONTROL_BTN_SELECT = 0,
    INPUT_CONTROL_BTN_START,
    INPUT_CONTROL_BTN_UP,
    INPUT_CONTROL_BTN_RIGHT,
    INPUT_CONTROL_BTN_DOWN,
    INPUT_CONTROL_BTN_LEFT,
    INPUT_CONTROL_BTN_LTRIGGER,
    INPUT_CONTROL_BTN_RTRIGGER,
    INPUT_CONTROL_BTN_TRIANGLE,
    INPUT_CONTROL_BTN_CIRCLE,
    INPUT_CONTROL_BTN_CROSS,
    INPUT_CONTROL_BTN_SQUARE,
    INPUT_CONTROL_BTN_HOME,
    INPUT_CONTROL_BTN_HOLD,
    INPUT_CONTROL_DIGITAL_COUNT = 14,
    INPUT_CONTROL_ANALOG_STICK = 14,
    INPUT_CONTROL_TOTAL_COUNT = 15
} InputControlIndex;

/**
 * A detected duplicate binding conflict between two PSP controls.
 */
typedef struct {
    int control_a; /* InputControlIndex (0..13) */
    int control_b; /* InputControlIndex (0..13) */
    NkBindingSource source;
    char description[160];
} InputBindingConflict;

/**
 * Stages for guided analog calibration (#357).
 */
typedef enum {
    CALIBRATION_STAGE_INACTIVE = 0,
    CALIBRATION_STAGE_REST,      /* "Leave everything at rest" (sample resting values ~1 s) */
    CALIBRATION_STAGE_EXTREMES,  /* "Press each trigger fully / move stick in circles" (sample extremes) */
    CALIBRATION_STAGE_RESULT     /* Show result, let user accept or cancel */
} GuidedCalibrationStage;

/**
 * Guided calibration session state (#357).
 */
typedef struct {
    GuidedCalibrationStage stage;
    int elapsed_ms;
    int duration_ms; /* duration for rest sampling, default 1000 ms */

    /* Sampled resting values */
    int16_t sampled_rest_x;
    int16_t sampled_rest_y;
    int16_t sampled_rest_lt;
    int16_t sampled_rest_rt;
    int sample_count;
    int32_t rest_x_acc;
    int32_t rest_y_acc;
    int32_t rest_lt_acc;
    int32_t rest_rt_acc;

    /* Sampled extreme values */
    int16_t sampled_min_x;
    int16_t sampled_max_x;
    int16_t sampled_min_y;
    int16_t sampled_max_y;
    int16_t sampled_max_lt;
    int16_t sampled_max_rt;

    /* Computed candidate calibration ready to accept */
    int16_t result_rest_x;
    int16_t result_min_x;
    int16_t result_max_x;
    int16_t result_rest_y;
    int16_t result_min_y;
    int16_t result_max_y;
    int16_t result_trigger_rest;
    int16_t result_trigger_extreme;
} GuidedCalibrationState;

/**
 * In-memory controller settings editing state (SDL-free, unit-testable).
 */
typedef struct {
    NkInputProfile profile;
    char profile_path[NK_MAX_PATH];
    char load_diagnostic[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    char save_diagnostic[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    bool loaded_from_file;
    bool has_load_diagnostic;
    bool has_save_diagnostic;

    /* Conflict tracking */
    InputBindingConflict conflicts[INPUT_SETTINGS_MAX_CONFLICTS];
    int conflict_count;

    /* Capture state */
    bool capturing;
    int capture_control; /* InputControlIndex (0..13) */
    int capture_elapsed_ms;
    int capture_timeout_ms;

    /* Guided calibration state (#357) */
    GuidedCalibrationState calib;
} InputSettingsState;

/**
 * @brief Initialize input settings state to safe defaults.
 */
void input_settings_init(InputSettingsState *state);

/**
 * @brief Load profile from disk.
 *
 * If custom_path is NULL or empty, resolves the standard profile path via nk_input_profile_resolve_path.
 * If file does not exist, resets to defaults and returns NK_OK.
 * If file is corrupt or invalid, resets to defaults, records diagnostic, and returns error code.
 */
NkResult input_settings_load(InputSettingsState *state, const char *custom_path);

/**
 * @brief Save profile to disk atomically (write to .tmp then rename).
 *
 * If custom_path is NULL or empty, uses state->profile_path.
 */
NkResult input_settings_save(InputSettingsState *state, const char *custom_path);

/**
 * @brief Reset profile and calibration to defaults.
 */
void input_settings_reset_to_defaults(InputSettingsState *state);

/**
 * @brief Get human-readable display name for a control index (0..14).
 */
const char *input_settings_control_name(int control_idx);

/**
 * @brief Format current host binding for a control index into a human-readable string.
 */
void input_settings_format_binding(const InputSettingsState *state, int control_idx, char *buf, size_t buf_sz);

/**
 * @brief Start a "press a button to bind" capture for a digital control (0..13).
 */
bool input_settings_start_capture(InputSettingsState *state, int control_idx);

/**
 * @brief Cancel an active capture without modifying bindings.
 */
void input_settings_cancel_capture(InputSettingsState *state);

/**
 * @brief Check if capture is currently active.
 */
bool input_settings_is_capturing(const InputSettingsState *state);

/**
 * @brief Get control index being captured, or -1 if not capturing.
 */
int input_settings_get_capture_control(const InputSettingsState *state);

/**
 * @brief Get remaining milliseconds before capture timeout.
 */
int input_settings_get_capture_remaining_ms(const InputSettingsState *state);

/**
 * @brief Advance capture timer. If capture_elapsed_ms >= capture_timeout_ms, cancels capture.
 * @return true if still capturing, false if not capturing or just timed out.
 */
bool input_settings_update_capture(InputSettingsState *state, int delta_ms);

/**
 * @brief Supply a binding source to the active capture, assigning it and ending capture.
 * @return true if successfully assigned, false if not capturing or invalid source.
 */
bool input_settings_feed_capture_source(InputSettingsState *state, NkBindingSource source);

/**
 * @brief Assign a binding source directly to a digital control and re-evaluate conflicts.
 * Does not overwrite other controls (conflicts are preserved without dropping either).
 */
bool input_settings_assign_binding(InputSettingsState *state, int control_idx, NkBindingSource source);

/**
 * @brief Re-scan profile for duplicate host bindings and update conflict records.
 */
void input_settings_refresh_conflicts(InputSettingsState *state);

/**
 * @brief Check if any binding conflicts exist.
 */
bool input_settings_has_conflicts(const InputSettingsState *state);

/**
 * @brief Get total number of detected binding conflicts.
 */
int input_settings_get_conflict_count(const InputSettingsState *state);

/**
 * @brief Get conflict record by index.
 */
const InputBindingConflict *input_settings_get_conflict(const InputSettingsState *state, int index);

/**
 * @brief Check if a specific control is involved in a conflict.
 */
bool input_settings_is_control_conflicted(const InputSettingsState *state, int control_idx);

/**
 * @brief Get first conflict description string in plain words, or empty string if none.
 */
const char *input_settings_get_conflict_summary(const InputSettingsState *state);

/**
 * @brief Adjust inner deadzone within validated bounds [0, 32766].
 * @param axis_idx NK_PSP_AXIS_ANALOG_X or NK_PSP_AXIS_ANALOG_Y, or -1 for both.
 * @param delta change in value (positive or negative).
 */
bool input_settings_adjust_deadzone(InputSettingsState *state, int axis_idx, int32_t delta);

/**
 * @brief Set inner deadzone directly, clamped to [0, 32766].
 */
bool input_settings_set_deadzone(InputSettingsState *state, int axis_idx, int32_t value);

/**
 * @brief Adjust trigger threshold within validated bounds [0, 32767].
 */
bool input_settings_adjust_trigger_threshold(InputSettingsState *state, int32_t delta);

/**
 * @brief Set trigger threshold directly, clamped to [0, 32767].
 */
bool input_settings_set_trigger_threshold(InputSettingsState *state, int32_t value);

/**
 * @brief Toggle axis inversion for NK_PSP_AXIS_ANALOG_X or NK_PSP_AXIS_ANALOG_Y.
 */
bool input_settings_toggle_axis_inversion(InputSettingsState *state, int axis_idx);

/**
 * @brief Start guided calibration workflow for analog stick and triggers (#357).
 */
bool input_settings_start_calibration(InputSettingsState *state);

/**
 * @brief Cancel active guided calibration without modifying profile.
 */
void input_settings_cancel_calibration(InputSettingsState *state);

/**
 * @brief Check if guided calibration is currently active.
 */
bool input_settings_is_calibrating(const InputSettingsState *state);

/**
 * @brief Get current stage of guided calibration.
 */
GuidedCalibrationStage input_settings_get_calibration_stage(const InputSettingsState *state);

/**
 * @brief Advance calibration timer and sample live host axes.
 * @param delta_ms Elapsed time in milliseconds.
 * @param host_axes Array of current host axis values.
 * @return true if still calibrating, false if inactive.
 */
bool input_settings_update_calibration(InputSettingsState *state, int delta_ms, const int16_t host_axes[NK_HOST_AXIS_COUNT]);

/**
 * @brief Finish extremes sampling stage and transition to result review stage.
 */
bool input_settings_finish_calibration_extremes(InputSettingsState *state);

/**
 * @brief Accept calibration results and apply them to the profile.
 */
bool input_settings_accept_calibration(InputSettingsState *state);

#ifdef __cplusplus
}
#endif

#endif /* NAKAGAWA_INPUT_SETTINGS_H */
