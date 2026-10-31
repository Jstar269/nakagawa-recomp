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
 * Control items editable in the UI: the 14 digital PSP buttons, the analog stick
 * (its controller stick and calibration), and the stick's four keyboard keys (#1299).
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
    INPUT_CONTROL_STICK_UP = 15,   /* the stick's keyboard keys, NkPspStickDir order */
    INPUT_CONTROL_STICK_DOWN,
    INPUT_CONTROL_STICK_LEFT,
    INPUT_CONTROL_STICK_RIGHT,
    INPUT_CONTROL_TOTAL_COUNT = 19
} InputControlIndex;

/**
 * The device whose bindings the Controls page shows and edits (#1299).
 */
typedef enum {
    INPUT_DEVICE_CONTROLLER = 0,
    INPUT_DEVICE_KEYBOARD
} InputSettingsDevice;

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
 *
 * `file` is the whole persisted profile document: the global mapping plus the
 * optional per-title entries (#520). `profile` is the mapping the user is
 * editing right now, which is either the global one (`editing_disc_id` empty) or
 * the per-title entry named by `editing_disc_id`. Editing a per-title mapping
 * therefore never touches the global one.
 */
typedef struct {
    NkInputProfileFile file;
    NkInputProfile profile;
    char profile_path[NK_MAX_PATH];
    char editing_disc_id[NK_MAX_DISC_ID_LEN];
    char load_diagnostic[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    char save_diagnostic[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    bool loaded_from_file;
    bool has_load_diagnostic;
    bool has_save_diagnostic;

    /* Conflict tracking */
    InputBindingConflict conflicts[INPUT_SETTINGS_MAX_CONFLICTS];
    int conflict_count;

    /* Capture state: a capture binds the device being edited */
    bool capturing;
    int capture_control; /* a control the device being edited can bind */
    int capture_elapsed_ms;
    int capture_timeout_ms;
    InputSettingsDevice device;
    /* What the last binding did, in plain words, or empty: an input taken from
     * another control (note_control, which now has the input this one had), or
     * why an input was refused (note_error). */
    char note[160];
    bool note_error;
    int note_control; /* the other control the last binding changed, or -1 */
    /* The mapping changed since it was last written: input_settings_save_if_dirty
     * writes it, so every change persists without a Save step (#1299). */
    bool dirty;

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
 * If custom_path is NULL or empty, uses state->profile_path. The mapping being
 * edited is written back into its slot first, so per-title entries survive a
 * save of the global mapping and vice versa.
 */
NkResult input_settings_save(InputSettingsState *state, const char *custom_path);

/**
 * @brief Choose which mapping is being edited: the global one, or a disc's own.
 *
 * disc_id NULL or empty selects the global mapping. A disc that has no entry yet
 * gets one seeded from the global mapping, so switching to a title always lands
 * on an editable copy rather than an empty state. Any in-flight capture or
 * calibration is cancelled, because it belonged to the previous scope.
 * @return false for an unusable disc ID or a full per-title table; the previous
 *         scope is kept in that case.
 */
bool input_settings_set_scope(InputSettingsState *state, const char *disc_id);

/**
 * @brief Switch between the global mapping and `disc_id`'s own mapping.
 *
 * Leaving a per-title scope for the global one drops that disc's entry, so a
 * disc the user put back on the global mapping really launches on it.
 * @return true when the scope changed; false when the switch was refused.
 */
bool input_settings_toggle_scope(InputSettingsState *state, const char *disc_id);

/**
 * @brief True when the state is editing a per-disc mapping rather than the
 * global one.
 */
bool input_settings_is_title_scope(const InputSettingsState *state);

/**
 * @brief True when the document already carries a per-disc mapping for
 * `disc_id`, which is what makes a launched disc use its own mapping.
 */
bool input_settings_has_title_mapping(const InputSettingsState *state, const char *disc_id);

/**
 * @brief Human-readable scope of the mapping being edited: "Global mapping" or
 * the disc ID of the per-title mapping.
 */
const char *input_settings_scope_label(const InputSettingsState *state);

/**
 * @brief Effective mapping for `disc_id` in the loaded document, without
 * changing which mapping is being edited.
 */
const NkInputProfile *input_settings_resolve_for_disc(
    const InputSettingsState *state,
    const char *disc_id,
    char *diag_buf,
    size_t diag_buf_sz
);

/**
 * @brief Write the effective mapping for `disc_id` to
 * <config>/input_profiles/<disc_id>.json and report the path.
 *
 * This is the file the player hands the launched runtime as NK_INPUT_PROFILE,
 * so a per-title mapping reaches the child exactly like the global one does.
 * Atomic (temp file plus rename); an unsafe disc ID, an unwritable directory or
 * a refused mapping is an error with a diagnostic, never a partial file.
 */
NkResult input_settings_write_disc_profile(
    const InputSettingsState *state,
    const char *disc_id,
    char *out_path,
    size_t out_path_sz,
    char *diag_buf,
    size_t diag_buf_sz
);

/**
 * @brief Reset profile and calibration to defaults.
 */
void input_settings_reset_to_defaults(InputSettingsState *state);

/**
 * @brief Get human-readable display name for a control index (InputControlIndex).
 */
const char *input_settings_control_name(int control_idx);

/**
 * @brief True for a control that has keyboard keys: a PSP button or one of the
 * stick's four directions.
 */
bool input_settings_control_has_keys(int control_idx);

/**
 * @brief True for a control the device being edited can bind: on the keyboard a
 * control with keys, on the controller a PSP button (the stick is chosen with
 * input_settings_set_stick).
 */
bool input_settings_control_bindable(const InputSettingsState *state, int control_idx);

/**
 * The most characters a default binding's words take, and any one input's: what the Controls
 * page's binding button shows whole (PLAYER_UI_CONTROLLER_BINDING_CHARS in player_ui_nk.h).
 * Two inputs the user binds to one control may be longer; the button then ends them with "...".
 */
#define INPUT_SETTINGS_BINDING_TEXT_MAX 20

/**
 * @brief Format current host binding for a control index into a human-readable string.
 */
void input_settings_format_binding(const InputSettingsState *state, int control_idx, char *buf, size_t buf_sz);

/**
 * @brief Start a "press a button to bind" capture for a control the device being
 * edited can bind (input_settings_control_bindable): a controller button, or a key.
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
 * @return true if successfully assigned, false if not capturing, capturing a key,
 *         or invalid source.
 */
bool input_settings_feed_capture_source(InputSettingsState *state, NkBindingSource source);

/**
 * @brief Make `source` a digital control's (0..13) controller binding.
 *
 * A source another PSP button already uses moves here, and that button takes
 * the one this control had in its place (none, when it had none): the mapping
 * never holds one input twice, so every change can be saved, and `note` names
 * the other button. A NK_BINDING_NONE source unbinds the control.
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
 * @brief Choose the device whose bindings are shown and edited. Cancels a capture.
 */
void input_settings_set_device(InputSettingsState *state, InputSettingsDevice device);

/**
 * @brief Name of a bindable host key in plain words ("X", "Left Shift", "Enter"),
 * by its position on a US keyboard; "None" for NK_HOST_KEY_NONE or a key that
 * cannot be bound.
 */
void input_settings_format_key(int key, char *buf, size_t buf_sz);

/**
 * @brief Format a PSP control's keyboard keys: "Enter", "Space / X", "Shift" for both shift keys, or "None".
 */
void input_settings_format_key_binding(const InputSettingsState *state, int control_idx,
                                       char *buf, size_t buf_sz);

/* The prompt glyph beside a binding on the Controls page (#1299): the host input it
 * shows, as the kinds of glyph the UI draws, and the legend printed on a keycap or a
 * pill. Face buttons go by position, the way the prompt bar draws them. */
typedef enum {
    INPUT_GLYPH_NONE = 0,       /* unbound: no glyph */
    INPUT_GLYPH_FACE_SOUTH,
    INPUT_GLYPH_FACE_EAST,
    INPUT_GLYPH_FACE_WEST,
    INPUT_GLYPH_FACE_NORTH,
    INPUT_GLYPH_SHOULDER_LEFT,
    INPUT_GLYPH_SHOULDER_RIGHT,
    INPUT_GLYPH_PAD_START,      /* the pad's menu button */
    INPUT_GLYPH_DPAD,           /* one of the D-pad's directions */
    INPUT_GLYPH_PAD_LEGEND,     /* any other pad input: a pill with .legend */
    INPUT_GLYPH_ARROW_KEY,      /* one of the arrow keys */
    INPUT_GLYPH_KEY             /* any other key: a keycap with .legend */
} InputGlyphKind;

/* Whose names a pad input with no glyph of its own is given on its pill: the
 * pad's family, as its south button's label tells them apart. */
typedef enum {
    INPUT_PAD_NAMES_NEUTRAL = 0,
    INPUT_PAD_NAMES_A_SOUTH,    /* A on the south button */
    INPUT_PAD_NAMES_A_EAST,     /* A on the east button */
    INPUT_PAD_NAMES_SHAPES,     /* a cross on the south button */
    INPUT_PAD_NAMES_COUNT
} InputPadNames;

typedef struct {
    InputGlyphKind kind;
    char legend[8];             /* PAD_LEGEND and KEY: "LT", "SHIFT"; otherwise empty */
} InputBindingGlyph;

/**
 * @brief The glyph of a digital control's (0..13) keyboard key: its first key, or its
 * second when it has only that. A keycap carries the key's printed legend in caps
 * ("X", "SHIFT", "ENTER"), the same for a key's left and right or keypad copy.
 */
void input_settings_key_glyph(const InputSettingsState *state, int control_idx,
                              InputBindingGlyph *out);

/**
 * @brief The glyph of a digital control's (0..13) controller binding: its first
 * source, or its second when it has only that. A pad input with no glyph of its own
 * (a trigger, a stick, Back, Guide) gets a pill named the way `names` names it.
 */
void input_settings_pad_glyph(const InputSettingsState *state, int control_idx,
                              InputPadNames names, InputBindingGlyph *out);

/**
 * @brief Make `key` the keyboard key of a control with keys, keeping its second key.
 *
 * A key another control already uses moves here, and that control takes the key
 * this one had in its place (no key, when it had none), so one key never drives
 * two controls (the profile refuses that) and the change can always be saved;
 * `note` names the other control. A key that cannot be bound (Esc, the function
 * keys) is refused: the mapping is left as it was and `note` says why.
 */
bool input_settings_assign_key(InputSettingsState *state, int control_idx, int key);

/**
 * @brief Supply the pressed key (a USB HID usage, as SDL_Scancode) to an active
 * keyboard capture, ending it. Returns true when the key was assigned.
 */
bool input_settings_feed_capture_key(InputSettingsState *state, int key);

/**
 * @brief Reset the bindings of the device being edited to defaults, keeping the
 * other device's: the keyboard keys (buttons and stick), or the controller
 * mapping and calibration.
 */
void input_settings_reset_device_to_defaults(InputSettingsState *state);

/**
 * @brief Drive the PSP stick with the controller's right stick (true) or its left
 * stick (false), both axes, keeping their calibration.
 */
void input_settings_set_stick(InputSettingsState *state, bool right);

/**
 * @brief True when the PSP stick follows the controller's right stick.
 */
bool input_settings_stick_is_right(const InputSettingsState *state);

/**
 * @brief Write the mapping when it changed since it was last written, unless a
 * capture or calibration is still running (it is written once that ends).
 *
 * The write is input_settings_save's; a failed write leaves save_diagnostic set
 * and is not retried until the next change. NK_OK when nothing needed writing.
 */
NkResult input_settings_save_if_dirty(InputSettingsState *state);

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
