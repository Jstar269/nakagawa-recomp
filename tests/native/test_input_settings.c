/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_settings.h"
#include "nk_input_profile.h"
#include "nk_platform.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
static void test_setenv(const char *name, const char *val) {
    if (val && val[0]) {
        _putenv_s(name, val);
    } else {
        _putenv_s(name, "");
    }
}
#else
#include <unistd.h>
static void test_setenv(const char *name, const char *val) {
    if (val) {
        setenv(name, val, 1);
    } else {
        unsetenv(name);
    }
}
#endif

/* -----------------------------------------------------------------------------
 * 1. Default Load
 * -------------------------------------------------------------------------- */
static void test_default_load(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 1: Default load when file is missing...\n");

    InputSettingsState state;
    input_settings_init(&state);

    /* Point to non-existent file */
    NkResult res = input_settings_load(&state, "build/nonexistent_profile_12345.json");
    assert(res == NK_OK);
    assert(!state.loaded_from_file);
    assert(!state.has_load_diagnostic);
    assert(state.load_diagnostic[0] == '\0');
    assert(state.profile.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
    assert(state.profile.trigger_threshold == NK_INPUT_DEFAULT_TRIGGER_THRESHOLD);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == NK_INPUT_DEFAULT_DEADZONE_INNER);
    assert(!input_settings_has_conflicts(&state));

    /* Verify all 14 controls have valid names and bindings */
    for (int i = 0; i < INPUT_CONTROL_TOTAL_COUNT; i++) {
        const char *name = input_settings_control_name(i);
        assert(name != NULL && strlen(name) > 0);
        char binding_str[128] = {0};
        input_settings_format_binding(&state, i, binding_str, sizeof(binding_str));
        assert(strlen(binding_str) > 0);
    }

    printf("[INPUT_SETTINGS_TEST] Subtest 1 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * 2. Corrupt File -> Defaults + Diagnostic
 * -------------------------------------------------------------------------- */
static void test_corrupt_file_load_diagnostic(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 2: Corrupt file gives defaults + diagnostic...\n");

    const char *corrupt_path = "build/test_corrupt_profile.json";
    FILE *f = fopen(corrupt_path, "wb");
    assert(f != NULL);
    fputs("{\n  \"schema_version\": 999,\n  \"invalid\": true\n}\n", f);
    fclose(f);

    InputSettingsState state;
    NkResult res = input_settings_load(&state, corrupt_path);
    assert(res != NK_OK);
    assert(state.loaded_from_file);
    assert(state.has_load_diagnostic);
    assert(strlen(state.load_diagnostic) > 0);
    /* Falls back to safe defaults */
    assert(state.profile.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == NK_INPUT_DEFAULT_DEADZONE_INNER);
    assert(!input_settings_has_conflicts(&state));

    remove(corrupt_path);
    printf("[INPUT_SETTINGS_TEST] Subtest 2 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * 3. Bind / Rebind and Capture
 * -------------------------------------------------------------------------- */
static void test_bind_rebind_and_capture(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 3: Bind, rebind, and interactive capture...\n");

    InputSettingsState state;
    input_settings_init(&state);

    /* Direct assign */
    NkBindingSource src_north = { NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_NORTH };
    bool ok = input_settings_assign_binding(&state, INPUT_CONTROL_BTN_CROSS, src_north);
    assert(ok);
    assert(state.profile.psp_buttons[INPUT_CONTROL_BTN_CROSS].primary.type == NK_BINDING_HOST_BUTTON);
    assert(state.profile.psp_buttons[INPUT_CONTROL_BTN_CROSS].primary.index == NK_HOST_BUTTON_NORTH);

    /* Test capture workflow */
    assert(!input_settings_is_capturing(&state));
    ok = input_settings_start_capture(&state, INPUT_CONTROL_BTN_TRIANGLE);
    assert(ok);
    assert(input_settings_is_capturing(&state));
    assert(input_settings_get_capture_control(&state) == INPUT_CONTROL_BTN_TRIANGLE);
    assert(input_settings_get_capture_remaining_ms(&state) == INPUT_SETTINGS_CAPTURE_TIMEOUT_MS);

    /* Advance time */
    bool still_capturing = input_settings_update_capture(&state, 1000);
    assert(still_capturing);
    assert(input_settings_get_capture_remaining_ms(&state) == INPUT_SETTINGS_CAPTURE_TIMEOUT_MS - 1000);

    /* Feed capture input */
    NkBindingSource src_west = { NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_WEST };
    ok = input_settings_feed_capture_source(&state, src_west);
    assert(ok);
    assert(!input_settings_is_capturing(&state));
    assert(state.profile.psp_buttons[INPUT_CONTROL_BTN_TRIANGLE].primary.index == NK_HOST_BUTTON_WEST);

    /* Test capture cancel */
    input_settings_start_capture(&state, INPUT_CONTROL_BTN_CIRCLE);
    assert(input_settings_is_capturing(&state));
    input_settings_cancel_capture(&state);
    assert(!input_settings_is_capturing(&state));
    assert(input_settings_get_capture_control(&state) == -1);

    /* Test capture timeout */
    input_settings_start_capture(&state, INPUT_CONTROL_BTN_CIRCLE);
    still_capturing = input_settings_update_capture(&state, 6000);
    assert(!still_capturing);
    assert(!input_settings_is_capturing(&state));

    printf("[INPUT_SETTINGS_TEST] Subtest 3 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * 4. Conflict Detection
 * -------------------------------------------------------------------------- */
static void test_conflict_detection(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 4: Conflict detection without dropping bindings...\n");

    InputSettingsState state;
    input_settings_init(&state);

    /* In default mapping:
     * CROSS = SOUTH, CIRCLE = EAST, SQUARE = WEST, TRIANGLE = NORTH.
     * Rebind CIRCLE to SOUTH so both CROSS and CIRCLE are bound to SOUTH. */
    NkBindingSource src_south = { NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_SOUTH };
    input_settings_assign_binding(&state, INPUT_CONTROL_BTN_CIRCLE, src_south);

    /* Verify both controls retained the binding (not silently dropped) */
    assert(state.profile.psp_buttons[INPUT_CONTROL_BTN_CROSS].primary.index == NK_HOST_BUTTON_SOUTH);
    assert(state.profile.psp_buttons[INPUT_CONTROL_BTN_CIRCLE].primary.index == NK_HOST_BUTTON_SOUTH);

    /* Conflict must be detected */
    assert(input_settings_has_conflicts(&state));
    assert(input_settings_get_conflict_count(&state) >= 1);
    assert(input_settings_is_control_conflicted(&state, INPUT_CONTROL_BTN_CROSS));
    assert(input_settings_is_control_conflicted(&state, INPUT_CONTROL_BTN_CIRCLE));
    assert(!input_settings_is_control_conflicted(&state, INPUT_CONTROL_BTN_TRIANGLE));

    const char *summary = input_settings_get_conflict_summary(&state);
    assert(summary != NULL && strlen(summary) > 0);
    assert(strstr(summary, "Cross") != NULL || strstr(summary, "CROSS") != NULL);
    assert(strstr(summary, "Circle") != NULL || strstr(summary, "CIRCLE") != NULL);

    /* Saving with conflict should fail validation and set diagnostic */
    const char *tmp_save = "build/test_conflict_save.json";
    NkResult save_res = input_settings_save(&state, tmp_save);
    assert(save_res != NK_OK);
    assert(state.has_save_diagnostic);
    assert(strlen(state.save_diagnostic) > 0);

    /* Resolve conflict by rebinding CIRCLE to EAST */
    NkBindingSource src_east = { NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_EAST };
    input_settings_assign_binding(&state, INPUT_CONTROL_BTN_CIRCLE, src_east);

    assert(!input_settings_has_conflicts(&state));
    assert(input_settings_get_conflict_count(&state) == 0);
    assert(!input_settings_is_control_conflicted(&state, INPUT_CONTROL_BTN_CROSS));
    assert(!input_settings_is_control_conflicted(&state, INPUT_CONTROL_BTN_CIRCLE));

    printf("[INPUT_SETTINGS_TEST] Subtest 4 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * 5. Deadzone and Trigger Bounds
 * -------------------------------------------------------------------------- */
static void test_deadzone_and_trigger_bounds(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 5: Deadzone and trigger bounds validation...\n");

    InputSettingsState state;
    input_settings_init(&state);

    /* Deadzone adjustment within bounds */
    input_settings_set_deadzone(&state, NK_PSP_AXIS_ANALOG_X, 10000);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 10000);

    /* Deadzone clamp at lower bound (0) */
    input_settings_adjust_deadzone(&state, NK_PSP_AXIS_ANALOG_X, -20000);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 0);

    /* Deadzone clamp at upper bound (32766) */
    input_settings_set_deadzone(&state, NK_PSP_AXIS_ANALOG_X, 50000);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 32766);

    /* Trigger threshold clamp */
    input_settings_set_trigger_threshold(&state, 12000);
    assert(state.profile.trigger_threshold == 12000);

    input_settings_adjust_trigger_threshold(&state, -20000);
    assert(state.profile.trigger_threshold == 0);

    input_settings_set_trigger_threshold(&state, 40000);
    assert(state.profile.trigger_threshold == 32767);

    /* Axis inversion toggle */
    assert(!state.profile.axes[NK_PSP_AXIS_ANALOG_X].inverted);
    input_settings_toggle_axis_inversion(&state, NK_PSP_AXIS_ANALOG_X);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].inverted);
    input_settings_toggle_axis_inversion(&state, NK_PSP_AXIS_ANALOG_X);
    assert(!state.profile.axes[NK_PSP_AXIS_ANALOG_X].inverted);

    printf("[INPUT_SETTINGS_TEST] Subtest 5 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * 6. Reset to Defaults
 * -------------------------------------------------------------------------- */
static void test_reset_to_defaults(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 6: Reset to defaults...\n");

    InputSettingsState state;
    input_settings_init(&state);

    /* Mutate everything */
    NkBindingSource src_none = { NK_BINDING_NONE, 0 };
    input_settings_assign_binding(&state, INPUT_CONTROL_BTN_CROSS, src_none);
    input_settings_set_deadzone(&state, NK_PSP_AXIS_ANALOG_X, 25000);
    input_settings_set_trigger_threshold(&state, 20000);
    input_settings_toggle_axis_inversion(&state, NK_PSP_AXIS_ANALOG_Y);

    /* Reset */
    input_settings_reset_to_defaults(&state);

    assert(state.profile.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
    assert(state.profile.psp_buttons[INPUT_CONTROL_BTN_CROSS].primary.type == NK_BINDING_HOST_BUTTON);
    assert(state.profile.psp_buttons[INPUT_CONTROL_BTN_CROSS].primary.index == NK_HOST_BUTTON_SOUTH);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == NK_INPUT_DEFAULT_DEADZONE_INNER);
    assert(state.profile.trigger_threshold == NK_INPUT_DEFAULT_TRIGGER_THRESHOLD);
    assert(!state.profile.axes[NK_PSP_AXIS_ANALOG_Y].inverted);
    assert(!input_settings_has_conflicts(&state));

    printf("[INPUT_SETTINGS_TEST] Subtest 6 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * 7. Save / Load Round Trip with Atomic Replace
 * -------------------------------------------------------------------------- */
static void test_save_load_roundtrip(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 7: Save/load round trip...\n");

    const char *tmp_file = "build/test_roundtrip_profile.json";
    InputSettingsState state;
    input_settings_init(&state);

    /* Configure customized mapping */
    NkBindingSource src_north = { NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_NORTH };
    NkBindingSource src_south = { NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_SOUTH };
    /* Swap CROSS and TRIANGLE */
    input_settings_assign_binding(&state, INPUT_CONTROL_BTN_CROSS, src_north);
    input_settings_assign_binding(&state, INPUT_CONTROL_BTN_TRIANGLE, src_south);
    input_settings_set_deadzone(&state, -1, 14000);
    input_settings_set_trigger_threshold(&state, 10000);

    NkResult save_res = input_settings_save(&state, tmp_file);
    assert(save_res == NK_OK);
    assert(!state.has_save_diagnostic);
    assert(nk_platform_file_exists(tmp_file));

    /* Load into fresh state */
    InputSettingsState loaded;
    NkResult load_res = input_settings_load(&loaded, tmp_file);
    assert(load_res == NK_OK);
    assert(loaded.loaded_from_file);
    assert(!loaded.has_load_diagnostic);
    assert(!input_settings_has_conflicts(&loaded));

    assert(loaded.profile.psp_buttons[INPUT_CONTROL_BTN_CROSS].primary.index == NK_HOST_BUTTON_NORTH);
    assert(loaded.profile.psp_buttons[INPUT_CONTROL_BTN_TRIANGLE].primary.index == NK_HOST_BUTTON_SOUTH);
    assert(loaded.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 14000);
    assert(loaded.profile.axes[NK_PSP_AXIS_ANALOG_Y].deadzone_inner == 14000);
    assert(loaded.profile.trigger_threshold == 10000);

    /* A second save must replace the existing file, not fail on it. */
    input_settings_set_trigger_threshold(&loaded, 12000);
    assert(input_settings_save(&loaded, tmp_file) == NK_OK);
    InputSettingsState reloaded;
    assert(input_settings_load(&reloaded, tmp_file) == NK_OK);
    assert(reloaded.profile.trigger_threshold == 12000);

    remove(tmp_file);
    printf("[INPUT_SETTINGS_TEST] Subtest 7 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * 8. SR_PADSCRIPT Semantics Untouched
 * -------------------------------------------------------------------------- */
static void test_sr_padscript_semantics_untouched(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 8: SR_PADSCRIPT semantics remain untouched...\n");

    /* Verify nk_input_profile_padscript_active behavior */
    test_setenv("SR_PADSCRIPT", NULL);
    assert(!nk_input_profile_padscript_active());

    test_setenv("SR_PADSCRIPT", "");
    assert(!nk_input_profile_padscript_active());

    test_setenv("SR_PADSCRIPT", "synthetic_script.txt");
    assert(nk_input_profile_padscript_active());

    /* Path resolution helper resolves correctly under environment overrides */
    char path_buf[512] = {0};
    test_setenv("NK_INPUT_PROFILE", "build/override_profile.json");
    NkResult res = nk_input_profile_resolve_path(path_buf, sizeof(path_buf));
    assert(res == NK_OK);
    assert(strcmp(path_buf, "build/override_profile.json") == 0);

    test_setenv("NK_INPUT_PROFILE", NULL);
    test_setenv("SR_PADSCRIPT", NULL);

    printf("[INPUT_SETTINGS_TEST] Subtest 8 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * 9. Guided Calibration and Resting/Extreme Transform Math
 * -------------------------------------------------------------------------- */
static void test_guided_calibration_and_resting_extremes(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 9: Guided calibration and resting/extreme transforms...\n");

    /* 9a. Calibrated Axis Transform Math */
    /* Neutral stick with resting offset 500: at raw 500, output must be exact center 128 */
    uint8_t center_val = nk_input_profile_transform_axis_calibrated(500, 7849, 0, false, 500, -30000, 30000);
    assert(center_val == 128);

    /* Within deadzone around rest (e.g. 500 + 4000 = 4500): still 128 */
    assert(nk_input_profile_transform_axis_calibrated(4500, 7849, 0, false, 500, -30000, 30000) == 128);
    assert(nk_input_profile_transform_axis_calibrated(-3500, 7849, 0, false, 500, -30000, 30000) == 128);

    /* At positive extreme 30000: output must be 255 */
    assert(nk_input_profile_transform_axis_calibrated(30000, 7849, 0, false, 500, -30000, 30000) == 255);
    /* At negative extreme -30000: output must be 0 */
    assert(nk_input_profile_transform_axis_calibrated(-30000, 7849, 0, false, 500, -30000, 30000) == 0);

    /* 9b. Calibrated Trigger Evaluation Math */
    /* Trigger resting at 200, extreme at 30000, threshold at 8192 (~25%) */
    /* At rest (raw 200): not pressed */
    assert(!nk_input_profile_eval_trigger_calibrated(200, 8192, 200, 30000));
    assert(!nk_input_profile_eval_trigger_calibrated(100, 8192, 200, 30000));
    /* Pull 5000 (below threshold): not pressed */
    assert(!nk_input_profile_eval_trigger_calibrated(5000, 8192, 200, 30000));
    /* Pull 15000 (~50%): pressed */
    assert(nk_input_profile_eval_trigger_calibrated(15000, 8192, 200, 30000));
    /* Pull 30000 (100%): pressed */
    assert(nk_input_profile_eval_trigger_calibrated(30000, 8192, 200, 30000));

    /* 9c. Guided Calibration State Machine */
    InputSettingsState state;
    input_settings_init(&state);

    assert(!input_settings_is_calibrating(&state));
    assert(input_settings_get_calibration_stage(&state) == CALIBRATION_STAGE_INACTIVE);

    bool started = input_settings_start_calibration(&state);
    assert(started);
    assert(input_settings_is_calibrating(&state));
    assert(input_settings_get_calibration_stage(&state) == CALIBRATION_STAGE_REST);

    /* Advance time during rest stage with host axes at resting drift */
    int16_t rest_axes[NK_HOST_AXIS_COUNT] = {0};
    rest_axes[NK_HOST_AXIS_LEFTX] = 450;
    rest_axes[NK_HOST_AXIS_LEFTY] = -320;
    rest_axes[NK_HOST_AXIS_LEFT_TRIGGER] = 80;
    rest_axes[NK_HOST_AXIS_RIGHT_TRIGGER] = 90;

    /* Tick 500ms -> still in rest */
    input_settings_update_calibration(&state, 500, rest_axes);
    assert(input_settings_get_calibration_stage(&state) == CALIBRATION_STAGE_REST);

    /* Tick remaining 500ms -> should transition to EXTREMES stage */
    input_settings_update_calibration(&state, 500, rest_axes);
    assert(input_settings_get_calibration_stage(&state) == CALIBRATION_STAGE_EXTREMES);

    /* Simulate user moving stick in circles and pulling triggers */
    int16_t move_axes[NK_HOST_AXIS_COUNT] = {0};
    move_axes[NK_HOST_AXIS_LEFTX] = -29500;
    move_axes[NK_HOST_AXIS_LEFTY] = -31000;
    move_axes[NK_HOST_AXIS_LEFT_TRIGGER] = 31200;
    move_axes[NK_HOST_AXIS_RIGHT_TRIGGER] = 30800;
    input_settings_update_calibration(&state, 100, move_axes);

    move_axes[NK_HOST_AXIS_LEFTX] = 30500;
    move_axes[NK_HOST_AXIS_LEFTY] = 29800;
    input_settings_update_calibration(&state, 100, move_axes);

    /* User clicks Finish Sampling */
    bool finished = input_settings_finish_calibration_extremes(&state);
    assert(finished);
    assert(input_settings_get_calibration_stage(&state) == CALIBRATION_STAGE_RESULT);

    /* User clicks Accept */
    bool accepted = input_settings_accept_calibration(&state);
    assert(accepted);
    assert(!input_settings_is_calibrating(&state));
    assert(input_settings_get_calibration_stage(&state) == CALIBRATION_STAGE_INACTIVE);

    /* Verify profile has the calibrated values */
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].rest == 450);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].min_val == -29500);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].max_val == 30500);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_Y].rest == -320);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_Y].min_val == -31000);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_Y].max_val == 29800);
    assert(state.profile.trigger_rest == 90);
    assert(state.profile.trigger_extreme == 31200);

    /* 9d. Save/load round-trip preserves calibrated fields */
    const char *calib_file = "build/test_calib_profile.json";
    assert(input_settings_save(&state, calib_file) == NK_OK);

    InputSettingsState loaded;
    assert(input_settings_load(&loaded, calib_file) == NK_OK);
    assert(loaded.profile.axes[NK_PSP_AXIS_ANALOG_X].rest == 450);
    assert(loaded.profile.axes[NK_PSP_AXIS_ANALOG_X].min_val == -29500);
    assert(loaded.profile.axes[NK_PSP_AXIS_ANALOG_X].max_val == 30500);
    assert(loaded.profile.trigger_rest == 90);
    assert(loaded.profile.trigger_extreme == 31200);
    remove(calib_file);

    /* 9e. Backward compatibility: load profile without calibration fields */
    const char *legacy_file = "build/test_legacy_profile.json";
    FILE *lf = fopen(legacy_file, "w");
    assert(lf != NULL);
    fputs("{\n"
          "  \"schema_version\": 1,\n"
          "  \"device\": {\n"
          "    \"guid\": \"legacy_test\",\n"
          "    \"name_hint\": \"Legacy Controller\"\n"
          "  },\n"
          "  \"calibration\": {\n"
          "    \"trigger_threshold\": 8192,\n"
          "    \"analog_x\": {\n"
          "      \"host_axis\": \"leftx\",\n"
          "      \"deadzone_inner\": 7849,\n"
          "      \"deadzone_outer\": 0,\n"
          "      \"inverted\": false\n"
          "    },\n"
          "    \"analog_y\": {\n"
          "      \"host_axis\": \"lefty\",\n"
          "      \"deadzone_inner\": 7849,\n"
          "      \"deadzone_outer\": 0,\n"
          "      \"inverted\": false\n"
          "    }\n"
          "  },\n"
          "  \"psp_bindings\": [],\n"
          "  \"navigation_bindings\": []\n"
          "}\n", lf);
    fclose(lf);

    InputSettingsState legacy_loaded;
    assert(input_settings_load(&legacy_loaded, legacy_file) == NK_OK);
    /* Should have defaulted cleanly */
    assert(legacy_loaded.profile.axes[NK_PSP_AXIS_ANALOG_X].rest == 0);
    assert(legacy_loaded.profile.axes[NK_PSP_AXIS_ANALOG_X].min_val == -32768);
    assert(legacy_loaded.profile.axes[NK_PSP_AXIS_ANALOG_X].max_val == 32767);
    assert(legacy_loaded.profile.trigger_rest == 0);
    assert(legacy_loaded.profile.trigger_extreme == 32767);
    remove(legacy_file);

    printf("[INPUT_SETTINGS_TEST] Subtest 9 PASSED!\n");
}

int main(void) {
    printf("=================================================================\n");
    printf("Starting Nakagawa Native Player Input Settings Test Suite\n");
    printf("=================================================================\n");

    test_default_load();
    test_corrupt_file_load_diagnostic();
    test_bind_rebind_and_capture();
    test_conflict_detection();
    test_deadzone_and_trigger_bounds();
    test_reset_to_defaults();
    test_save_load_roundtrip();
    test_sr_padscript_semantics_untouched();
    test_guided_calibration_and_resting_extremes();

    printf("=================================================================\n");
    printf("ALL INPUT SETTINGS TESTS PASSED SUCCESSFULLY!\n");
    printf("=================================================================\n");
    return 0;
}
