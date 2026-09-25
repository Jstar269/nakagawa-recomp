/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_INPUT_PROFILE_H
#define NK_INPUT_PROFILE_H

#include "nk_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_INPUT_PROFILE_SCHEMA_VERSION 1
#define NK_INPUT_GUID_MAX_LEN 64
#define NK_INPUT_NAME_HINT_MAX_LEN 128
#define NK_INPUT_DIAGNOSTIC_MAX_LEN 256

/* Default calibration values matching current runtime (src/rt/gpu_sdl3vk/sdl3vk.c) */
#define NK_INPUT_DEFAULT_DEADZONE_INNER 7849
#define NK_INPUT_DEFAULT_DEADZONE_OUTER 0
#define NK_INPUT_DEFAULT_TRIGGER_THRESHOLD 8192

/**
 * Standard PSP digital buttons (14 total).
 * Corresponding to bits in SceCtrlData.Buttons:
 *   SELECT    = 0x000001
 *   START     = 0x000008
 *   UP        = 0x000010
 *   RIGHT     = 0x000020
 *   DOWN      = 0x000040
 *   LEFT      = 0x000080
 *   LTRIGGER  = 0x000100
 *   RTRIGGER  = 0x000200
 *   TRIANGLE  = 0x001000
 *   CIRCLE    = 0x002000
 *   CROSS     = 0x004000
 *   SQUARE    = 0x008000
 *   HOME      = 0x010000
 *   HOLD      = 0x020000
 */
typedef enum {
    NK_PSP_BTN_SELECT = 0,
    NK_PSP_BTN_START,
    NK_PSP_BTN_UP,
    NK_PSP_BTN_RIGHT,
    NK_PSP_BTN_DOWN,
    NK_PSP_BTN_LEFT,
    NK_PSP_BTN_LTRIGGER,
    NK_PSP_BTN_RTRIGGER,
    NK_PSP_BTN_TRIANGLE,
    NK_PSP_BTN_CIRCLE,
    NK_PSP_BTN_CROSS,
    NK_PSP_BTN_SQUARE,
    NK_PSP_BTN_HOME,
    NK_PSP_BTN_HOLD,
    NK_PSP_BTN_COUNT /* 14 */
} NkPspButton;

/**
 * Standard PSP analog stick axes (2 total).
 */
typedef enum {
    NK_PSP_AXIS_ANALOG_X = 0,
    NK_PSP_AXIS_ANALOG_Y,
    NK_PSP_AXIS_COUNT /* 2 */
} NkPspAxis;

/**
 * Host SDL Gamepad buttons.
 * Values match SDL_GamepadButton enum values in SDL3/SDL2 exactly.
 */
typedef enum {
    NK_HOST_BUTTON_INVALID = -1,
    NK_HOST_BUTTON_SOUTH = 0,
    NK_HOST_BUTTON_EAST = 1,
    NK_HOST_BUTTON_WEST = 2,
    NK_HOST_BUTTON_NORTH = 3,
    NK_HOST_BUTTON_BACK = 4,
    NK_HOST_BUTTON_GUIDE = 5,
    NK_HOST_BUTTON_START = 6,
    NK_HOST_BUTTON_LEFT_STICK = 7,
    NK_HOST_BUTTON_RIGHT_STICK = 8,
    NK_HOST_BUTTON_LEFT_SHOULDER = 9,
    NK_HOST_BUTTON_RIGHT_SHOULDER = 10,
    NK_HOST_BUTTON_DPAD_UP = 11,
    NK_HOST_BUTTON_DPAD_DOWN = 12,
    NK_HOST_BUTTON_DPAD_LEFT = 13,
    NK_HOST_BUTTON_DPAD_RIGHT = 14,
    NK_HOST_BUTTON_MISC1 = 15,
    NK_HOST_BUTTON_RIGHT_PADDLE1 = 16,
    NK_HOST_BUTTON_LEFT_PADDLE1 = 17,
    NK_HOST_BUTTON_RIGHT_PADDLE2 = 18,
    NK_HOST_BUTTON_LEFT_PADDLE2 = 19,
    NK_HOST_BUTTON_TOUCHPAD = 20,
    NK_HOST_BUTTON_COUNT
} NkHostGamepadButton;

/**
 * Host SDL Gamepad axes.
 * Values match SDL_GamepadAxis enum values in SDL3/SDL2 exactly.
 */
typedef enum {
    NK_HOST_AXIS_INVALID = -1,
    NK_HOST_AXIS_LEFTX = 0,
    NK_HOST_AXIS_LEFTY = 1,
    NK_HOST_AXIS_RIGHTX = 2,
    NK_HOST_AXIS_RIGHTY = 3,
    NK_HOST_AXIS_LEFT_TRIGGER = 4,
    NK_HOST_AXIS_RIGHT_TRIGGER = 5,
    NK_HOST_AXIS_COUNT
} NkHostGamepadAxis;

/**
 * Host binding source type for digital presses.
 */
typedef enum {
    NK_BINDING_NONE = 0,
    NK_BINDING_HOST_BUTTON,      /* Host gamepad button pressed */
    NK_BINDING_HOST_TRIGGER,     /* Host trigger axis > trigger_threshold */
    NK_BINDING_HOST_AXIS_POS,    /* Host axis positive half > deadzone_inner */
    NK_BINDING_HOST_AXIS_NEG     /* Host axis negative half < -deadzone_inner */
} NkBindingSourceType;

/**
 * A single host binding specification.
 */
typedef struct {
    NkBindingSourceType type;
    int index; /* NkHostGamepadButton or NkHostGamepadAxis */
} NkBindingSource;

/**
 * Binding for a digital action (e.g. PSP button or navigation action).
 * Supports primary binding and optional secondary binding.
 */
typedef struct {
    NkBindingSource primary;
    NkBindingSource secondary;
} NkDigitalBinding;

/**
 * Per-axis calibration for analog sticks.
 */
typedef struct {
    NkHostGamepadAxis host_axis; /* Which host axis drives this PSP axis */
    int16_t deadzone_inner;      /* Inner deadzone (neutral deadband), range [0, 32766] */
    int16_t deadzone_outer;      /* Outer deadzone (edge saturation), range [0, 32766] */
    bool inverted;               /* Invert axis direction */
    int16_t rest;                /* Rest / center offset (default 0) */
    int16_t min_val;             /* Minimum extreme value (default -32768) */
    int16_t max_val;             /* Maximum extreme value (default 32767) */
} NkAxisCalibration;

/**
 * Player navigation actions (for menus, library, settings).
 */
typedef enum {
    NK_NAV_ACTION_CONFIRM = 0,   /* Activate / Select */
    NK_NAV_ACTION_CANCEL,        /* Back / Return */
    NK_NAV_ACTION_UP,            /* Focus up */
    NK_NAV_ACTION_DOWN,          /* Focus down */
    NK_NAV_ACTION_LEFT,          /* Focus left / previous card */
    NK_NAV_ACTION_RIGHT,         /* Focus right / next card */
    NK_NAV_ACTION_PAGE_PREV,     /* Page back in library */
    NK_NAV_ACTION_PAGE_NEXT,     /* Page forward in library */
    NK_NAV_ACTION_MENU,          /* Toggle settings / menu */
    NK_NAV_ACTION_COUNT /* 9 */
} NkNavAction;

/**
 * Versioned host input profile.
 */
typedef struct {
    int schema_version;
    char guid[NK_INPUT_GUID_MAX_LEN];
    char name_hint[NK_INPUT_NAME_HINT_MAX_LEN];

    int16_t trigger_threshold; /* Threshold for trigger axes to act as digital press */
    int16_t trigger_rest;      /* Resting value when unpressed (default 0) */
    int16_t trigger_extreme;   /* Extreme value when fully pressed (default 32767) */

    NkAxisCalibration axes[NK_PSP_AXIS_COUNT];
    NkDigitalBinding psp_buttons[NK_PSP_BTN_COUNT];
    NkDigitalBinding nav_bindings[NK_NAV_ACTION_COUNT];
} NkInputProfile;

/**
 * @brief Transform raw host SDL axis value to PSP analog byte (0..255, centre 128).
 *
 * Mapping Formula:
 * 1. Inversion:
 *      if inverted:
 *          val = -raw_axis (clamped: -32768 becomes 32767)
 *      else:
 *          val = raw_axis
 *
 * 2. Inner Deadzone:
 *      if |val| <= deadzone_inner:
 *          return 128 (PSP neutral / center)
 *
 * 3. Outer Deadzone:
 *      if deadzone_outer > 0:
 *          outer_threshold = 32767 - deadzone_outer
 *          if val >= outer_threshold:
 *              val = 32767
 *          else if val <= -outer_threshold:
 *              val = -32768
 *
 * 4. PSP Byte Mapping:
 *      byte = (uint8_t)(((int32_t)val + 32768) * 255 / 65535)
 *
 * In the default profile (deadzone_inner = 7849, deadzone_outer = 0, inverted = false),
 * this yields 128 for |raw| <= 7849 and ((raw + 32768) * 255 / 65535) for |raw| > 7849,
 * reproducing the runtime's src/rt/gpu_sdl3vk/sdl3vk.c exactly.
 */
uint8_t nk_input_profile_transform_axis(
    int16_t raw_axis,
    int16_t deadzone_inner,
    int16_t deadzone_outer,
    bool inverted
);

/**
 * @brief Transform raw host axis to PSP byte using resting center and measured extremes.
 */
uint8_t nk_input_profile_transform_axis_calibrated(
    int16_t raw_axis,
    int16_t deadzone_inner,
    int16_t deadzone_outer,
    bool inverted,
    int16_t rest,
    int16_t min_val,
    int16_t max_val
);

/**
 * @brief Test whether a raw trigger axis value satisfies the trigger threshold.
 *
 * Formula:
 *      pressed = (raw_trigger > trigger_threshold)
 *
 * With the default threshold of 8192 (matching sdl3vk.c), pulls > 8192 return true.
 */
bool nk_input_profile_eval_trigger(int16_t raw_trigger, int16_t trigger_threshold);

/**
 * @brief Test whether a raw trigger satisfies threshold calibrated against resting/extreme pull.
 */
bool nk_input_profile_eval_trigger_calibrated(
    int16_t raw_trigger,
    int16_t trigger_threshold,
    int16_t trigger_rest,
    int16_t trigger_extreme
);

/**
 * @brief Return standard PSP button bitmask for an enum button.
 */
uint32_t nk_psp_button_bitmask(NkPspButton btn);

/**
 * @brief String names for controls and actions.
 */
const char *nk_psp_button_name(NkPspButton btn);
NkPspButton nk_psp_button_from_name(const char *name);

const char *nk_psp_axis_name(NkPspAxis axis);
NkPspAxis nk_psp_axis_from_name(const char *name);

const char *nk_host_button_name(NkHostGamepadButton btn);
NkHostGamepadButton nk_host_button_from_name(const char *name);

const char *nk_host_axis_name(NkHostGamepadAxis axis);
NkHostGamepadAxis nk_host_axis_from_name(const char *name);

const char *nk_nav_action_name(NkNavAction action);
NkNavAction nk_nav_action_from_name(const char *name);

/**
 * @brief Initialize profile to standard safe defaults matching current runtime.
 */
void nk_input_profile_init_default(NkInputProfile *profile);

/**
 * @brief Validate an input profile for range errors, duplicates, and conflicts.
 *
 * @param profile Profile to validate.
 * @param diag_buf Optional diagnostic message buffer.
 * @param diag_buf_sz Size of diag_buf.
 * @return NK_OK (0) if valid, or NK_ERROR_* (<0) on validation error.
 */
NkResult nk_input_profile_validate(
    const NkInputProfile *profile,
    char *diag_buf,
    size_t diag_buf_sz
);

/**
 * @brief Strictly load an input profile from disk.
 *
 * Fails to safe defaults on unknown version, out-of-range value, duplicate or
 * conflicting binding, or malformed JSON. A future version is never downgraded.
 */
NkResult nk_input_profile_load(
    NkInputProfile *out_profile,
    const char *file_path,
    char *diag_buf,
    size_t diag_buf_sz
);

/**
 * @brief Strictly parse an input profile from in-memory JSON text.
 */
NkResult nk_input_profile_parse_json(
    NkInputProfile *out_profile,
    const char *json_str,
    size_t json_len,
    char *diag_buf,
    size_t diag_buf_sz
);

/**
 * @brief Save input profile to disk atomically (write to .tmp then rename).
 */
NkResult nk_input_profile_save(
    const NkInputProfile *profile,
    const char *file_path,
    char *diag_buf,
    size_t diag_buf_sz
);

/**
 * @brief Resolve the active input profile file path.
 *
 * Checks NK_INPUT_PROFILE and SR_INPUT_PROFILE environment variables.
 * If unset or empty, resolves <config_dir>/input_profile.json via nk_platform_get_path.
 *
 * @param out_path Buffer to receive resolved null-terminated path.
 * @param out_path_sz Capacity of out_path.
 * @return NK_OK (0) on success, or NK_ERROR_* (<0) on failure.
 */
NkResult nk_input_profile_resolve_path(char *out_path, size_t out_path_sz);

/**
 * @brief Check whether automated deterministic script input is active.
 *
 * When SR_PADSCRIPT is set and non-empty, scripted input takes precedence
 * and host input profiles must not override default input mappings.
 */
bool nk_input_profile_padscript_active(void);

/**
 * @brief Evaluate the 14 PSP digital buttons from host gamepad state.
 */
uint32_t nk_input_profile_eval_buttons(
    const NkInputProfile *profile,
    const bool host_buttons[NK_HOST_BUTTON_COUNT],
    const int16_t host_axes[NK_HOST_AXIS_COUNT]
);

/**
 * @brief Evaluate PSP analog stick (lx, ly) from host gamepad state.
 */
void nk_input_profile_eval_analog(
    const NkInputProfile *profile,
    const int16_t host_axes[NK_HOST_AXIS_COUNT],
    uint8_t *out_lx,
    uint8_t *out_ly
);

/**
 * @brief Evaluate player navigation actions from host gamepad state.
 */
uint32_t nk_input_profile_eval_navigation(
    const NkInputProfile *profile,
    const bool host_buttons[NK_HOST_BUTTON_COUNT],
    const int16_t host_axes[NK_HOST_AXIS_COUNT]
);

#ifdef __cplusplus
}
#endif

#endif /* NK_INPUT_PROFILE_H */
