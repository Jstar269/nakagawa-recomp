/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_input_profile.h"
#include "nk_platform.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif

/* -----------------------------------------------------------------------------
 * Subtest 1: Default Profile Equals Current Runtime Mapping
 * -------------------------------------------------------------------------- */

static void test_default_equals_current_mapping(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 1: Default equals current runtime mapping...\n");

    NkInputProfile profile;
    nk_input_profile_init_default(&profile);

    assert(profile.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
    assert(profile.trigger_threshold == 8192);
    assert(profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 7849);
    assert(profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_outer == 0);
    assert(!profile.axes[NK_PSP_AXIS_ANALOG_X].inverted);
    assert(profile.axes[NK_PSP_AXIS_ANALOG_Y].deadzone_inner == 7849);
    assert(profile.axes[NK_PSP_AXIS_ANALOG_Y].deadzone_outer == 0);
    assert(!profile.axes[NK_PSP_AXIS_ANALOG_Y].inverted);

    /* Test all 12 digital buttons from sdl3vk.c lines 503-514 */
    static const struct {
        NkHostGamepadButton host_btn;
        uint32_t expected_psp_bit;
        const char *name;
    } kButtons[] = {
        { NK_HOST_BUTTON_SOUTH,          0x4000, "CROSS" },
        { NK_HOST_BUTTON_EAST,           0x2000, "CIRCLE" },
        { NK_HOST_BUTTON_WEST,           0x8000, "SQUARE" },
        { NK_HOST_BUTTON_NORTH,          0x1000, "TRIANGLE" },
        { NK_HOST_BUTTON_START,          0x0008, "START" },
        { NK_HOST_BUTTON_BACK,           0x0001, "SELECT" },
        { NK_HOST_BUTTON_LEFT_SHOULDER,  0x0100, "LTRIGGER (button)" },
        { NK_HOST_BUTTON_RIGHT_SHOULDER, 0x0200, "RTRIGGER (button)" },
        { NK_HOST_BUTTON_DPAD_UP,        0x0010, "UP" },
        { NK_HOST_BUTTON_DPAD_DOWN,      0x0040, "DOWN" },
        { NK_HOST_BUTTON_DPAD_LEFT,      0x0080, "LEFT" },
        { NK_HOST_BUTTON_DPAD_RIGHT,     0x0020, "RIGHT" }
    };

    for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
        bool host_buttons[NK_HOST_BUTTON_COUNT];
        int16_t host_axes[NK_HOST_AXIS_COUNT];
        memset(host_buttons, 0, sizeof(host_buttons));
        memset(host_axes, 0, sizeof(host_axes));

        host_buttons[kButtons[i].host_btn] = true;
        uint32_t psp_mask = nk_input_profile_eval_buttons(&profile, host_buttons, host_axes);
        assert(psp_mask == kButtons[i].expected_psp_bit);
    }

    /* Test trigger axes as L/R buttons (sdl3vk.c lines 516-517) */
    {
        bool host_buttons[NK_HOST_BUTTON_COUNT];
        int16_t host_axes[NK_HOST_AXIS_COUNT];
        memset(host_buttons, 0, sizeof(host_buttons));
        memset(host_axes, 0, sizeof(host_axes));

        /* Left trigger below threshold */
        host_axes[NK_HOST_AXIS_LEFT_TRIGGER] = 8192;
        assert((nk_input_profile_eval_buttons(&profile, host_buttons, host_axes) & 0x0100) == 0);

        /* Left trigger above threshold */
        host_axes[NK_HOST_AXIS_LEFT_TRIGGER] = 8193;
        assert((nk_input_profile_eval_buttons(&profile, host_buttons, host_axes) & 0x0100) == 0x0100);

        /* Right trigger below threshold */
        host_axes[NK_HOST_AXIS_LEFT_TRIGGER] = 0;
        host_axes[NK_HOST_AXIS_RIGHT_TRIGGER] = 8192;
        assert((nk_input_profile_eval_buttons(&profile, host_buttons, host_axes) & 0x0200) == 0);

        /* Right trigger above threshold */
        host_axes[NK_HOST_AXIS_RIGHT_TRIGGER] = 8193;
        assert((nk_input_profile_eval_buttons(&profile, host_buttons, host_axes) & 0x0200) == 0x0200);
    }

    /* Test analog axes transform across full 16-bit domain vs sdl3vk.c formula (lines 520-521) */
    for (int ax = -32768; ax <= 32767; ax++) {
        uint8_t expected = 128;
        if (ax < -7849 || ax > 7849) {
            expected = (uint8_t)(((int32_t)ax + 32768) * 255 / 65535);
        }
        uint8_t actual = nk_input_profile_transform_axis((int16_t)ax, 7849, 0, false);
        assert(actual == expected);
    }

    /* Test eval_analog helper */
    {
        int16_t host_axes[NK_HOST_AXIS_COUNT];
        memset(host_axes, 0, sizeof(host_axes));
        host_axes[NK_HOST_AXIS_LEFTX] = 16000;
        host_axes[NK_HOST_AXIS_LEFTY] = -16000;

        uint8_t lx = 0, ly = 0;
        nk_input_profile_eval_analog(&profile, host_axes, &lx, &ly);

        uint8_t exp_lx = (uint8_t)(((int32_t)16000 + 32768) * 255 / 65535);
        uint8_t exp_ly = (uint8_t)(((int32_t)-16000 + 32768) * 255 / 65535);
        assert(lx == exp_lx);
        assert(ly == exp_ly);
    }

    /* Test player navigation bindings matching player/main.c */
    {
        bool host_buttons[NK_HOST_BUTTON_COUNT];
        int16_t host_axes[NK_HOST_AXIS_COUNT];
        memset(host_buttons, 0, sizeof(host_buttons));
        memset(host_axes, 0, sizeof(host_axes));

        host_buttons[NK_HOST_BUTTON_SOUTH] = true;
        uint32_t nav = nk_input_profile_eval_navigation(&profile, host_buttons, host_axes);
        assert(nav & (1u << NK_NAV_ACTION_CONFIRM));

        host_buttons[NK_HOST_BUTTON_SOUTH] = false;
        host_buttons[NK_HOST_BUTTON_EAST] = true;
        nav = nk_input_profile_eval_navigation(&profile, host_buttons, host_axes);
        assert(nav & (1u << NK_NAV_ACTION_CANCEL));

        host_buttons[NK_HOST_BUTTON_EAST] = false;
        host_buttons[NK_HOST_BUTTON_START] = true;
        nav = nk_input_profile_eval_navigation(&profile, host_buttons, host_axes);
        assert(nav & (1u << NK_NAV_ACTION_MENU));
    }

    printf("[INPUT_PROFILE_TEST] Subtest 1 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 2: Deadzone and Inversion Transform Vectors
 * -------------------------------------------------------------------------- */

static void test_deadzone_and_inversion_vectors(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 2: Deadzone and inversion transform vectors...\n");

    /* Centre vectors */
    assert(nk_input_profile_transform_axis(0, 5000, 0, false) == 128);
    assert(nk_input_profile_transform_axis(0, 5000, 0, true) == 128);

    /* Deadzone edge vectors */
    assert(nk_input_profile_transform_axis(5000, 5000, 0, false) == 128);
    assert(nk_input_profile_transform_axis(-5000, 5000, 0, false) == 128);
    assert(nk_input_profile_transform_axis(5001, 5000, 0, false) > 128);
    assert(nk_input_profile_transform_axis(-5001, 5000, 0, false) < 128);

    /* Extreme limits */
    assert(nk_input_profile_transform_axis(32767, 5000, 0, false) == 255);
    assert(nk_input_profile_transform_axis(-32768, 5000, 0, false) == 0);

    /* Inversion vectors */
    assert(nk_input_profile_transform_axis(32767, 5000, 0, true) == 0);
    assert(nk_input_profile_transform_axis(-32768, 5000, 0, true) == 255);
    assert(nk_input_profile_transform_axis(5001, 5000, 0, true) < 128);
    assert(nk_input_profile_transform_axis(-5001, 5000, 0, true) > 128);
    assert(nk_input_profile_transform_axis(3000, 5000, 0, true) == 128);

    /* Outer deadzone saturation vectors */
    int16_t inner = 2000;
    int16_t outer = 4000;
    /* Beyond 32767 - 4000 = 28767, value saturates to 255 */
    assert(nk_input_profile_transform_axis(28767, inner, outer, false) == 255);
    assert(nk_input_profile_transform_axis(30000, inner, outer, false) == 255);
    assert(nk_input_profile_transform_axis(28766, inner, outer, false) < 255);

    /* Negative outer deadzone saturation: below -28767 */
    assert(nk_input_profile_transform_axis(-28767, inner, outer, false) == 0);
    assert(nk_input_profile_transform_axis(-30000, inner, outer, false) == 0);
    assert(nk_input_profile_transform_axis(-28766, inner, outer, false) > 0);

    /* Outer deadzone with inversion */
    assert(nk_input_profile_transform_axis(28767, inner, outer, true) == 0);
    assert(nk_input_profile_transform_axis(-28767, inner, outer, true) == 255);

    /* Trigger threshold vectors */
    assert(!nk_input_profile_eval_trigger(8192, 8192));
    assert(nk_input_profile_eval_trigger(8193, 8192));
    assert(!nk_input_profile_eval_trigger(0, 8192));
    assert(nk_input_profile_eval_trigger(32767, 8192));
    assert(!nk_input_profile_eval_trigger(-1000, 8192));

    printf("[INPUT_PROFILE_TEST] Subtest 2 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 3: Strict-Load Failures Reset to Defaults With Actionable Diagnostic
 * -------------------------------------------------------------------------- */

static void test_strict_load_failures(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 3: Strict-load failures reset to defaults with diagnostic...\n");

    char diag[256];
    NkInputProfile profile;

    /* 1. Malformed JSON */
    const char *bad_json = "{ \"schema_version\": 1, \"device\": { bad_syntax ";
    NkResult res = nk_input_profile_parse_json(&profile, bad_json, strlen(bad_json), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(diag[0] != '\0');
    assert(strstr(diag, "malformed JSON") != NULL || strstr(diag, "offset") != NULL);
    assert(profile.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
    assert(profile.trigger_threshold == NK_INPUT_DEFAULT_TRIGGER_THRESHOLD);

    /* 2. Out of range trigger threshold */
    const char *bad_threshold =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 40000,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": []\n"
        "}";
    res = nk_input_profile_parse_json(&profile, bad_threshold, strlen(bad_threshold), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "trigger_threshold") != NULL && strstr(diag, "out of range") != NULL);
    assert(profile.trigger_threshold == NK_INPUT_DEFAULT_TRIGGER_THRESHOLD);

    /* 3. Out of range deadzone */
    const char *bad_deadzone =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 50000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": []\n"
        "}";
    res = nk_input_profile_parse_json(&profile, bad_deadzone, strlen(bad_deadzone), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "deadzone_inner") != NULL && strstr(diag, "out of range") != NULL);
    assert(profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == NK_INPUT_DEFAULT_DEADZONE_INNER);

    /* 4. Combined deadzone sum out of range */
    const char *bad_deadzone_sum =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 20000, \"deadzone_outer\": 20000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": []\n"
        "}";
    res = nk_input_profile_parse_json(&profile, bad_deadzone_sum, strlen(bad_deadzone_sum), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "combined deadzones") != NULL);

    /* 5. Unknown PSP control */
    const char *bad_control =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [ { \"control\": \"hyper_turbo_button\", \"primary\": \"south\" } ],\n"
        "  \"navigation_bindings\": []\n"
        "}";
    res = nk_input_profile_parse_json(&profile, bad_control, strlen(bad_control), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "unknown PSP control") != NULL);

    /* 6. Unknown navigation action */
    const char *bad_nav =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": [ { \"action\": \"somersault\", \"primary\": \"south\" } ]\n"
        "}";
    res = nk_input_profile_parse_json(&profile, bad_nav, strlen(bad_nav), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "unknown navigation action") != NULL);

    /* 7. Duplicate binding for same control */
    const char *dup_control =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [\n"
        "    { \"control\": \"cross\", \"primary\": \"south\" },\n"
        "    { \"control\": \"cross\", \"primary\": \"east\" }\n"
        "  ],\n"
        "  \"navigation_bindings\": []\n"
        "}";
    res = nk_input_profile_parse_json(&profile, dup_control, strlen(dup_control), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "duplicate binding") != NULL && strstr(diag, "cross") != NULL);

    /* 8. Duplicate binding for same navigation action */
    const char *dup_nav =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": [\n"
        "    { \"action\": \"confirm\", \"primary\": \"south\" },\n"
        "    { \"action\": \"confirm\", \"primary\": \"east\" }\n"
        "  ]\n"
        "}";
    res = nk_input_profile_parse_json(&profile, dup_nav, strlen(dup_nav), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "duplicate binding") != NULL && strstr(diag, "confirm") != NULL);

    /* 9. Hostile input: duplicate JSON object key rejected by shared parser */
    const char *dup_json_key =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": []\n"
        "}";
    res = nk_input_profile_parse_json(&profile, dup_json_key, strlen(dup_json_key), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "duplicate JSON object key") != NULL);

    /* 10. Hostile input: JSON nesting exceeds maximum depth */
    const char *deep_nest =
        "[[[[[[[[[[[[[[[[[[[[{\"schema_version\":1}]]]]]]]]]]]]]]]]]]]]";
    res = nk_input_profile_parse_json(&profile, deep_nest, strlen(deep_nest), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "nesting exceeds maximum depth") != NULL);

    /* 11. Hostile input: invalid UTF-8 sequence */
    const char invalid_utf8[] = "{\"schema_version\": 1, \"device\": { \"guid\": \"\xFF\xFE\", \"name_hint\": \"bad\" }}";
    res = nk_input_profile_parse_json(&profile, invalid_utf8, sizeof(invalid_utf8) - 1, diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "UTF-8") != NULL);

    printf("[INPUT_PROFILE_TEST] Subtest 3 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 4: Future Version Refused Without Downgrade
 * -------------------------------------------------------------------------- */

static void test_future_version_refused(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 4: Future version refused without downgrade...\n");

    char diag[256];
    NkInputProfile profile;

    const char *future_json =
        "{\n"
        "  \"schema_version\": 3,\n"
        "  \"device\": { \"guid\": \"guid_v3\", \"name_hint\": \"Future Controller\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": []\n"
        "}";

    NkResult res = nk_input_profile_parse_json(&profile, future_json, strlen(future_json), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "unsupported future schema_version 3") != NULL);
    /* Safe defaults fallback */
    assert(profile.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
    assert(profile.trigger_threshold == NK_INPUT_DEFAULT_TRIGGER_THRESHOLD);

    /* Write future file to disk and verify loading from disk does not downgrade or overwrite it */
    const char *file_path = "build/test_future_profile.json";
    FILE *f = fopen(file_path, "wb");
    assert(f != NULL);
    fputs(future_json, f);
    fclose(f);

    res = nk_input_profile_load(&profile, file_path, diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "unsupported future schema_version 3") != NULL);

    /* Verify the file on disk is still the future version and has not been overwritten */
    f = fopen(file_path, "rb");
    assert(f != NULL);
    char readback[512];
    size_t rb_bytes = fread(readback, 1, sizeof(readback) - 1, f);
    fclose(f);
    readback[rb_bytes] = '\0';
    assert(strstr(readback, "\"schema_version\": 3") != NULL);

    remove(file_path);
    printf("[INPUT_PROFILE_TEST] Subtest 4 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 5: Conflict Detection
 * -------------------------------------------------------------------------- */

static void test_conflict_detection(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 5: Conflict detection...\n");

    char diag[256];
    NkInputProfile profile;

    /* 1. Conflicting PSP button binding (both CROSS and CIRCLE bound to SOUTH) */
    const char *conflict_psp =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [\n"
        "    { \"control\": \"cross\", \"primary\": \"south\" },\n"
        "    { \"control\": \"circle\", \"primary\": \"south\" }\n"
        "  ],\n"
        "  \"navigation_bindings\": []\n"
        "}";
    NkResult res = nk_input_profile_parse_json(&profile, conflict_psp, strlen(conflict_psp), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "conflicting binding") != NULL);
    assert(strstr(diag, "south") != NULL);

    /* 2. Conflicting navigation binding (both CONFIRM and CANCEL bound to SOUTH) */
    const char *conflict_nav =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"guid1\", \"name_hint\": \"name1\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": [\n"
        "    { \"action\": \"confirm\", \"primary\": \"south\" },\n"
        "    { \"action\": \"cancel\", \"primary\": \"south\" }\n"
        "  ]\n"
        "}";
    res = nk_input_profile_parse_json(&profile, conflict_nav, strlen(conflict_nav), diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "conflicting navigation binding") != NULL);
    assert(strstr(diag, "south") != NULL);

    /* 3. In-memory validation conflict detection */
    nk_input_profile_init_default(&profile);
    profile.psp_buttons[NK_PSP_BTN_CIRCLE].primary = profile.psp_buttons[NK_PSP_BTN_CROSS].primary;
    res = nk_input_profile_validate(&profile, diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "conflicting binding") != NULL);

    /* 4. Axis conflict detection (both analog stick axes mapped to same host axis) */
    nk_input_profile_init_default(&profile);
    profile.axes[NK_PSP_AXIS_ANALOG_Y].host_axis = profile.axes[NK_PSP_AXIS_ANALOG_X].host_axis;
    res = nk_input_profile_validate(&profile, diag, sizeof(diag));
    assert(res != NK_OK);
    assert(strstr(diag, "conflicting axis binding") != NULL);

    printf("[INPUT_PROFILE_TEST] Subtest 5 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 6: Save and Load Round-Trip With Atomic Persistence
 * -------------------------------------------------------------------------- */

static void test_save_load_round_trip(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 6: Save/load round trip with atomic replacement...\n");

    const char *file_path = "build/test_saved_profile.json";
    char diag[256];

    NkInputProfile original;
    nk_input_profile_init_default(&original);

    /* Customize the profile */
    snprintf(original.guid, sizeof(original.guid), "030000005e0400008e02000000007200");
    snprintf(original.name_hint, sizeof(original.name_hint), "Customized Xbox Elite Pad");
    original.trigger_threshold = 12345;

    original.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner = 5500;
    original.axes[NK_PSP_AXIS_ANALOG_X].deadzone_outer = 1500;
    original.axes[NK_PSP_AXIS_ANALOG_X].inverted = true;

    original.axes[NK_PSP_AXIS_ANALOG_Y].deadzone_inner = 6500;
    original.axes[NK_PSP_AXIS_ANALOG_Y].deadzone_outer = 2000;
    original.axes[NK_PSP_AXIS_ANALOG_Y].inverted = false;

    /* Save to file */
    NkResult save_res = nk_input_profile_save(&original, file_path, diag, sizeof(diag));
    assert(save_res == NK_OK);
    assert(nk_platform_file_exists(file_path));

    /* Load back */
    NkInputProfile loaded;
    NkResult load_res = nk_input_profile_load(&loaded, file_path, diag, sizeof(diag));
    assert(load_res == NK_OK);

    /* Assert field-by-field equality */
    assert(loaded.schema_version == original.schema_version);
    assert(strcmp(loaded.guid, original.guid) == 0);
    assert(strcmp(loaded.name_hint, original.name_hint) == 0);
    assert(loaded.trigger_threshold == original.trigger_threshold);

    for (int i = 0; i < NK_PSP_AXIS_COUNT; i++) {
        assert(loaded.axes[i].host_axis == original.axes[i].host_axis);
        assert(loaded.axes[i].deadzone_inner == original.axes[i].deadzone_inner);
        assert(loaded.axes[i].deadzone_outer == original.axes[i].deadzone_outer);
        assert(loaded.axes[i].inverted == original.axes[i].inverted);
    }

    for (int i = 0; i < NK_PSP_BTN_COUNT; i++) {
        assert(loaded.psp_buttons[i].primary.type == original.psp_buttons[i].primary.type);
        assert(loaded.psp_buttons[i].primary.index == original.psp_buttons[i].primary.index);
        assert(loaded.psp_buttons[i].secondary.type == original.psp_buttons[i].secondary.type);
        assert(loaded.psp_buttons[i].secondary.index == original.psp_buttons[i].secondary.index);
    }

    for (int i = 0; i < NK_NAV_ACTION_COUNT; i++) {
        assert(loaded.nav_bindings[i].primary.type == original.nav_bindings[i].primary.type);
        assert(loaded.nav_bindings[i].primary.index == original.nav_bindings[i].primary.index);
        assert(loaded.nav_bindings[i].secondary.type == original.nav_bindings[i].secondary.type);
        assert(loaded.nav_bindings[i].secondary.index == original.nav_bindings[i].secondary.index);
    }

    /* Test atomic overwrite on existing file */
    original.trigger_threshold = 9999;
    save_res = nk_input_profile_save(&original, file_path, diag, sizeof(diag));
    assert(save_res == NK_OK);

    load_res = nk_input_profile_load(&loaded, file_path, diag, sizeof(diag));
    assert(load_res == NK_OK);
    assert(loaded.trigger_threshold == 9999);

    remove(file_path);
    printf("[INPUT_PROFILE_TEST] Subtest 6 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 7: Per-Title Map, Selection, and Global Fallback
 * -------------------------------------------------------------------------- */

static void test_per_title_selection_and_fallback(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 7: per-title map, selection and fallback...\n");

    char diag[256];
    NkInputProfileFile doc;
    nk_input_profile_file_init_default(&doc);
    assert(doc.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
    assert(doc.title_count == 0);

    /* The global mapping: CROSS on the host south button. */
    assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.type == NK_BINDING_HOST_BUTTON);
    assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_SOUTH);

    /* A disc's own mapping starts as a validated copy, then differs. The host
     * buttons it takes are ones the global mapping leaves free, because a
     * mapping that double-binds a host button is refused, not silently kept. */
    NkInputProfile tennis;
    nk_input_profile_init_default(&tennis);
    tennis.psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_GUIDE;
    tennis.axes[NK_PSP_AXIS_ANALOG_Y].inverted = true;
    assert(nk_input_profile_file_set_title(&doc, "UCUS98701", &tennis, diag, sizeof(diag)) == NK_OK);
    assert(doc.title_count == 1);
    assert(strcmp(doc.title_disc_id[0], "UCUS98701") == 0);
    assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_SOUTH);

    /* A second disc gets its own entry: two mappings in one document. */
    NkInputProfile boxing;
    nk_input_profile_init_default(&boxing);
    boxing.psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_LEFT_STICK;
    assert(nk_input_profile_file_set_title(&doc, "ULUS10041", &boxing, diag, sizeof(diag)) == NK_OK);
    assert(doc.title_count == 2);

    /* Saving and reloading preserves both entries and the global mapping. */
    const char *file_path = "build/test_per_title_profile.json";
    assert(nk_input_profile_file_save(&doc, file_path, diag, sizeof(diag)) == NK_OK);
    assert(nk_platform_file_exists(file_path));

    NkInputProfileFile loaded;
    assert(nk_input_profile_file_load(&loaded, file_path, diag, sizeof(diag)) == NK_OK);
    assert(loaded.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
    assert(loaded.title_count == 2);
    assert(nk_input_profile_file_find_title(&loaded, "UCUS98701") == 0);
    assert(nk_input_profile_file_find_title(&loaded, "ULUS10041") == 1);
    assert(nk_input_profile_file_find_title(&loaded, "ULES99999") == -1);
    assert(loaded.title[0].psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_GUIDE);
    assert(loaded.title[0].axes[NK_PSP_AXIS_ANALOG_Y].inverted);
    assert(loaded.title[1].psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_LEFT_STICK);
    assert(loaded.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_SOUTH);

    /* Selection: a disc with an entry gets it, any other disc gets the global
     * mapping, and the case of a disc ID does not decide. */
    char resolve_diag[256];
    const NkInputProfile *resolved =
        nk_input_profile_file_resolve(&loaded, "UCUS98701", resolve_diag, sizeof(resolve_diag));
    assert(resolved == &loaded.title[0]);
    assert(resolve_diag[0] == '\0');
    resolved = nk_input_profile_file_resolve(&loaded, "ucus98701", resolve_diag, sizeof(resolve_diag));
    assert(resolved == &loaded.title[0]);
    resolved = nk_input_profile_file_resolve(&loaded, "ULES99999", resolve_diag, sizeof(resolve_diag));
    assert(resolved == &loaded.global);
    assert(resolve_diag[0] == '\0');
    resolved = nk_input_profile_file_resolve(&loaded, NULL, resolve_diag, sizeof(resolve_diag));
    assert(resolved == &loaded.global);

    /* The resolved entries really do hand the guest different buttons. */
    bool host_buttons[NK_HOST_BUTTON_COUNT];
    int16_t host_axes[NK_HOST_AXIS_COUNT];
    memset(host_buttons, 0, sizeof(host_buttons));
    memset(host_axes, 0, sizeof(host_axes));
    host_buttons[NK_HOST_BUTTON_LEFT_STICK] = true;
    assert(nk_input_profile_eval_buttons(resolved /* ULUS10041 */, host_buttons, host_axes) == 0);
    host_buttons[NK_HOST_BUTTON_LEFT_STICK] = false;
    host_buttons[NK_HOST_BUTTON_GUIDE] = true;
    assert(nk_input_profile_eval_buttons(&loaded.global, host_buttons, host_axes) == 0);
    resolved = nk_input_profile_file_resolve(&loaded, "UCUS98701", resolve_diag, sizeof(resolve_diag));
    assert(nk_input_profile_eval_buttons(resolved, host_buttons, host_axes) == NK_PSP_BTN_CROSS_BIT);
    host_buttons[NK_HOST_BUTTON_GUIDE] = false;
    resolved = nk_input_profile_file_resolve(&loaded, "ULUS10041", resolve_diag, sizeof(resolve_diag));
    assert(nk_input_profile_eval_buttons(resolved, host_buttons, host_axes) == 0);
    host_buttons[NK_HOST_BUTTON_LEFT_STICK] = true;
    assert(nk_input_profile_eval_buttons(resolved, host_buttons, host_axes) == NK_PSP_BTN_CROSS_BIT);

    /* Removing an entry returns that disc to the global mapping. */
    assert(nk_input_profile_file_remove_title(&loaded, "UCUS98701"));
    assert(loaded.title_count == 1);
    resolved = nk_input_profile_file_resolve(&loaded, "UCUS98701", resolve_diag, sizeof(resolve_diag));
    assert(resolved == &loaded.global);
    assert(!nk_input_profile_file_remove_title(&loaded, "UCUS98701"));

    remove(file_path);
    printf("[INPUT_PROFILE_TEST] Subtest 7 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 8: Schema 1 Files Still Load (Migration)
 * -------------------------------------------------------------------------- */

static void test_schema_one_migration(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 8: schema 1 file migrates without loss...\n");

    const char *v1_json =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"v1_guid\", \"name_hint\": \"Schema One Pad\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 4096,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 3000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 3000 }\n"
        "  },\n"
        "  \"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"west\" } ],\n"
        "  \"navigation_bindings\": [ { \"action\": \"confirm\", \"primary\": \"south\" } ]\n"
        "}";

    char diag[256];
    NkInputProfileFile doc;
    assert(nk_input_profile_file_parse_json(&doc, v1_json, strlen(v1_json),
                                            diag, sizeof(diag)) == NK_OK);
    /* Migrated: the current schema, no per-title entries, same mapping. */
    assert(doc.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
    assert(doc.title_count == 0);
    assert(strcmp(doc.global.guid, "v1_guid") == 0);
    assert(strcmp(doc.global.name_hint, "Schema One Pad") == 0);
    assert(doc.global.trigger_threshold == 4096);
    assert(doc.global.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 3000);
    assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_WEST);
    assert(doc.global.nav_bindings[NK_NAV_ACTION_CONFIRM].primary.index == NK_HOST_BUTTON_SOUTH);
    assert(doc.global.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);

    /* A per_title map cannot be smuggled into a schema 1 document. */
    const char *v1_with_titles =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"device\": { \"guid\": \"v1_guid\", \"name_hint\": \"Schema One Pad\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 4096,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 3000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 3000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": [],\n"
        "  \"per_title\": []\n"
        "}";
    assert(nk_input_profile_file_parse_json(&doc, v1_with_titles, strlen(v1_with_titles),
                                            diag, sizeof(diag)) != NK_OK);
    assert(strstr(diag, "cannot carry a per_title map") != NULL);

    /* Re-saving a migrated document writes the current schema. */
    const char *migrated_path = "build/test_migrated_profile.json";
    assert(nk_input_profile_file_save(&doc, NULL, diag, sizeof(diag)) != NK_OK); /* no path */
    assert(nk_input_profile_file_parse_json(&doc, v1_json, strlen(v1_json),
                                            diag, sizeof(diag)) == NK_OK);
    assert(nk_input_profile_file_save(&doc, migrated_path, diag, sizeof(diag)) == NK_OK);

    FILE *f = fopen(migrated_path, "rb");
    assert(f != NULL);
    char readback[4096];
    size_t rb = fread(readback, 1, sizeof(readback) - 1, f);
    fclose(f);
    readback[rb] = '\0';
    assert(strstr(readback, "\"schema_version\": 2") != NULL);
    assert(strstr(readback, "\"per_title\"") != NULL);

    NkInputProfileFile reloaded;
    assert(nk_input_profile_file_load(&reloaded, migrated_path, diag, sizeof(diag)) == NK_OK);
    assert(reloaded.title_count == 0);
    assert(reloaded.global.trigger_threshold == 4096);
    remove(migrated_path);

    printf("[INPUT_PROFILE_TEST] Subtest 8 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 9: Invalid Per-Title Entries Fail Closed
 * -------------------------------------------------------------------------- */

static void test_per_title_fail_closed(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 9: invalid per-title entries fail closed...\n");

    char diag[256];
    NkInputProfileFile doc;

    /* An entry whose bindings conflict with each other is refused, and the whole
     * document falls back to defaults rather than applying half of it. */
    const char *conflicting_entry =
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": [],\n"
        "  \"per_title\": [\n"
        "    {\n"
        "      \"disc_id\": \"UCUS98701\",\n"
        "      \"profile\": {\n"
        "        \"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" },\n"
        "        \"calibration\": {\n"
        "          \"trigger_threshold\": 8192,\n"
        "          \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "          \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "        },\n"
        "        \"psp_bindings\": [\n"
        "          { \"control\": \"cross\", \"primary\": \"south\" },\n"
        "          { \"control\": \"circle\", \"primary\": \"south\" }\n"
        "        ],\n"
        "        \"navigation_bindings\": []\n"
        "      }\n"
        "    }\n"
        "  ]\n"
        "}";
    assert(nk_input_profile_file_parse_json(&doc, conflicting_entry, strlen(conflicting_entry),
                                            diag, sizeof(diag)) != NK_OK);
    assert(strstr(diag, "UCUS98701") != NULL);
    assert(strstr(diag, "conflicting binding") != NULL);
    assert(doc.title_count == 0);
    assert(doc.global.trigger_threshold == NK_INPUT_DEFAULT_TRIGGER_THRESHOLD);
    assert(strcmp(doc.global.guid, "default") == 0);

    /* An out-of-range value inside an entry refuses the document the same way. */
    const char *out_of_range_entry =
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": [],\n"
        "  \"per_title\": [\n"
        "    {\n"
        "      \"disc_id\": \"ULUS10041\",\n"
        "      \"profile\": {\n"
        "        \"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" },\n"
        "        \"calibration\": {\n"
        "          \"trigger_threshold\": 99999,\n"
        "          \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "          \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "        },\n"
        "        \"psp_bindings\": [],\n"
        "        \"navigation_bindings\": []\n"
        "      }\n"
        "    }\n"
        "  ]\n"
        "}";
    assert(nk_input_profile_file_parse_json(&doc, out_of_range_entry, strlen(out_of_range_entry),
                                            diag, sizeof(diag)) != NK_OK);
    assert(strstr(diag, "ULUS10041") != NULL);
    assert(strstr(diag, "trigger_threshold") != NULL);
    assert(doc.title_count == 0);

    /* A disc ID that could name a file outside the profile directory is refused
     * before it ever reaches the filesystem. */
    const char *unsafe_disc =
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": [],\n"
        "  \"per_title\": [\n"
        "    { \"disc_id\": \"../escape\", \"profile\": {\n"
        "        \"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" },\n"
        "        \"calibration\": {\n"
        "          \"trigger_threshold\": 8192,\n"
        "          \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "          \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "        },\n"
        "        \"psp_bindings\": [],\n"
        "        \"navigation_bindings\": [] } }\n"
        "  ]\n"
        "}";
    assert(nk_input_profile_file_parse_json(&doc, unsafe_disc, strlen(unsafe_disc),
                                            diag, sizeof(diag)) != NK_OK);
    assert(strstr(diag, "disc_id") != NULL);
    assert(doc.title_count == 0);
    assert(!nk_input_profile_disc_id_safe("../escape"));
    assert(!nk_input_profile_disc_id_safe(""));
    assert(!nk_input_profile_disc_id_safe(".."));
    assert(nk_input_profile_disc_id_safe("UCUS98701"));
    assert(!nk_input_profile_disc_id_safe("NUL"));
    assert(!nk_input_profile_disc_id_safe("con.json"));
    assert(!nk_input_profile_disc_id_safe("Com1"));
    assert(!nk_input_profile_disc_id_safe("LPT9.x"));
    assert(!nk_input_profile_disc_id_safe("UCUS98701."));
    assert(nk_input_profile_disc_id_safe("COM10"));
    assert(nk_input_profile_disc_id_safe("CONSOLE"));
    assert(nk_input_profile_disc_id_safe("TEST00006"));

    /* Two entries for the same disc are ambiguous, so the document is refused. */
    const char *duplicate_disc =
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" },\n"
        "  \"calibration\": {\n"
        "    \"trigger_threshold\": 8192,\n"
        "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "  },\n"
        "  \"psp_bindings\": [],\n"
        "  \"navigation_bindings\": [],\n"
        "  \"per_title\": [\n"
        "    { \"disc_id\": \"UCUS98701\", \"profile\": {\n"
        "        \"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" },\n"
        "        \"calibration\": {\n"
        "          \"trigger_threshold\": 8192,\n"
        "          \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "          \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "        },\n"
        "        \"psp_bindings\": [],\n"
        "        \"navigation_bindings\": [] } },\n"
        "    { \"disc_id\": \"ucus98701\", \"profile\": {\n"
        "        \"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" },\n"
        "        \"calibration\": {\n"
        "          \"trigger_threshold\": 8192,\n"
        "          \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
        "          \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
        "        },\n"
        "        \"psp_bindings\": [],\n"
        "        \"navigation_bindings\": [] } }\n"
        "  ]\n"
        "}";
    assert(nk_input_profile_file_parse_json(&doc, duplicate_disc, strlen(duplicate_disc),
                                            diag, sizeof(diag)) != NK_OK);
    assert(strstr(diag, "duplicate per_title entry") != NULL);
    assert(doc.title_count == 0);

    /* Storing an invalid mapping into a document is refused, and the document
     * keeps exactly the entries it had. */
    nk_input_profile_file_init_default(&doc);
    NkInputProfile good;
    nk_input_profile_init_default(&good);
    good.psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_GUIDE;
    assert(nk_input_profile_file_set_title(&doc, "UCUS98701", &good, diag, sizeof(diag)) == NK_OK);
    NkInputProfile bad = good;
    bad.psp_buttons[NK_PSP_BTN_CIRCLE] = bad.psp_buttons[NK_PSP_BTN_CROSS];
    assert(nk_input_profile_file_set_title(&doc, "ULUS10041", &bad, diag, sizeof(diag)) != NK_OK);
    assert(strstr(diag, "conflicting binding") != NULL);
    assert(doc.title_count == 1);
    assert(nk_input_profile_file_find_title(&doc, "ULUS10041") == -1);
    assert(nk_input_profile_file_set_title(&doc, "ULUS/10041", &good, diag, sizeof(diag)) != NK_OK);
    assert(doc.title_count == 1);

    /* The table is bounded: a full document refuses another disc rather than
     * dropping one silently. */
    for (int i = 0; doc.title_count < NK_INPUT_MAX_PER_TITLE; i++) {
        char disc_id[NK_MAX_DISC_ID_LEN];
        snprintf(disc_id, sizeof(disc_id), "FILL%05d", i);
        assert(nk_input_profile_file_set_title(&doc, disc_id, &good, diag, sizeof(diag)) == NK_OK);
    }
    assert(doc.title_count == NK_INPUT_MAX_PER_TITLE);
    assert(nk_input_profile_file_set_title(&doc, "ONET00TOOMANY", &good, diag, sizeof(diag)) != NK_OK);
    assert(strstr(diag, "table is full") != NULL);
    assert(doc.title_count == NK_INPUT_MAX_PER_TITLE);
    assert(nk_input_profile_file_find_title(&doc, "UCUS98701") == 0);

    /* A save with an invalid in-memory entry never replaces the file on disk. */
    const char *keep_path = "build/test_per_title_keep.json";
    assert(nk_input_profile_file_save(&doc, keep_path, diag, sizeof(diag)) == NK_OK);
    doc.title[1].psp_buttons[NK_PSP_BTN_CIRCLE] = doc.title[1].psp_buttons[NK_PSP_BTN_CROSS];
    assert(nk_input_profile_file_save(&doc, keep_path, diag, sizeof(diag)) != NK_OK);
    assert(strstr(diag, "per-title mapping") != NULL);
    NkInputProfileFile on_disk;
    assert(nk_input_profile_file_load(&on_disk, keep_path, diag, sizeof(diag)) == NK_OK);
    assert(on_disk.title_count == NK_INPUT_MAX_PER_TITLE);
    remove(keep_path);

    printf("[INPUT_PROFILE_TEST] Subtest 9 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 10: Hostile Profile Documents Fail Closed
 * ------------------------------------------------------------------------- */

/* The members every hostile case below starts from: a document the parser
 * really accepts. A case replaces exactly one member, so the difference between
 * an accepted document and a refused one is only the thing under test. */
static const char *const kSeedDevice =
    "\"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\" }";
static const char *const kSeedCalibration =
    "\"calibration\": {\n"
    "    \"trigger_threshold\": 8192,\n"
    "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
    "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
    "  }";
static const char *const kSeedPspBindings =
    "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"south\" } ]";
static const char *const kSeedNavBindings =
    "\"navigation_bindings\": [ { \"action\": \"confirm\", \"primary\": \"button:south\" } ]";
/* The mapping object a per_title entry carries. */
static const char *const kSeedEntryProfile =
    "{ \"device\": { \"guid\": \"guid1\", \"name_hint\": \"Pad\" },\n"
    "  \"calibration\": {\n"
    "    \"trigger_threshold\": 8192,\n"
    "    \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },\n"
    "    \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 }\n"
    "  },\n"
    "  \"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"south\" } ],\n"
    "  \"navigation_bindings\": [ { \"action\": \"confirm\", \"primary\": \"button:south\" } ]\n"
    "}";

/* Scratch for documents built on the fly. The biggest case builds a one-line
 * document of 200 KiB with an over-long field inside it, so this is sized for
 * both, and s_hostile_big adds the 1 MiB size-cap case the loader refuses. */
static char s_hostile_doc[512 * 1024];
static char s_hostile_big[1024 * 1024 + 64];

static void fill_repeat(char *dst, size_t dst_sz, char c, size_t count) {
    if (count > dst_sz - 1) count = dst_sz - 1;
    memset(dst, c, count);
    dst[count] = '\0';
}

/* Build the seed document with the named members replaced; NULL keeps the
 * seed's member, so a case shows only what it attacks. */
static size_t render_doc(char *out, size_t out_sz, const char *device,
                         const char *calibration, const char *psp_bindings,
                         const char *nav_bindings) {
    int n = snprintf(out, out_sz,
                     "{\n  \"schema_version\": 2,\n  %s,\n  %s,\n  %s,\n  %s\n}",
                     device ? device : kSeedDevice,
                     calibration ? calibration : kSeedCalibration,
                     psp_bindings ? psp_bindings : kSeedPspBindings,
                     nav_bindings ? nav_bindings : kSeedNavBindings);
    assert(n > 0 && (size_t)n < out_sz);
    return (size_t)n;
}

static size_t render_entry(char *out, size_t out_sz, const char *disc_id,
                           const char *profile) {
    int n = snprintf(out, out_sz, "{ \"disc_id\": %s, \"profile\": %s }",
                     disc_id, profile ? profile : kSeedEntryProfile);
    assert(n > 0 && (size_t)n < out_sz);
    return (size_t)n;
}

static size_t render_per_title_doc(char *out, size_t out_sz,
                                   const char *schema_version, const char *entries) {
    int n = snprintf(out, out_sz,
                     "{\n  \"schema_version\": %s,\n  %s,\n  %s,\n  %s,\n  %s,\n"
                     "  \"per_title\": [ %s ]\n}",
                     schema_version, kSeedDevice, kSeedCalibration,
                     kSeedPspBindings, kSeedNavBindings, entries);
    assert(n > 0 && (size_t)n < out_sz);
    return (size_t)n;
}

/* A hostile document must fail closed: the parser's own named error has to name
 * the offending member, and the caller must be left with the safe default
 * mapping and no per-title entry, never a partially applied mapping. */
static void expect_refused(const char *label, const char *json, size_t len,
                           const char *named_error) {
    char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    char why[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    NkInputProfileFile doc;

    diag[0] = '\0';
    NkResult res = nk_input_profile_file_parse_json(&doc, json, len, diag, sizeof(diag));
    if (res == NK_OK || diag[0] == '\0' || (named_error && strstr(diag, named_error) == NULL)) {
        printf("  %s: expected a refusal naming '%s', got res=%d diag='%s'\n",
               label, named_error ? named_error : "(any diagnostic)", (int)res, diag);
        assert(0 && "hostile document was not refused with the parser's named error");
    }

    assert(doc.title_count == 0);
    assert(strcmp(doc.global.guid, "default") == 0);
    assert(doc.global.trigger_threshold == NK_INPUT_DEFAULT_TRIGGER_THRESHOLD);
    assert(doc.global.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == NK_INPUT_DEFAULT_DEADZONE_INNER);
    assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.type == NK_BINDING_HOST_BUTTON);
    assert(nk_input_profile_validate(&doc.global, why, sizeof(why)) == NK_OK);
}

/* Whatever a parseable document loads must be a mapping the runtime can use
 * unchanged: every mapping validates, every stored disc ID is one the whitelist
 * accepts, and the per-title table is within its bound. */
static void expect_accepted(const char *label, const char *json, size_t len,
                            NkInputProfileFile *out) {
    char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    char why[NK_INPUT_DIAGNOSTIC_MAX_LEN];

    diag[0] = '\0';
    NkResult res = nk_input_profile_file_parse_json(out, json, len, diag, sizeof(diag));
    if (res != NK_OK) {
        printf("  %s: expected acceptance, got diag='%s'\n", label, diag);
        assert(0 && "document the parser accepts today was refused");
    }

    assert(nk_input_profile_validate(&out->global, why, sizeof(why)) == NK_OK);
    assert(out->title_count >= 0 && out->title_count <= NK_INPUT_MAX_PER_TITLE);
    for (int i = 0; i < out->title_count; i++) {
        assert(nk_input_profile_disc_id_safe(out->title_disc_id[i]));
        assert(nk_input_profile_validate(&out->title[i], why, sizeof(why)) == NK_OK);
        assert(nk_input_profile_file_find_title(out, out->title_disc_id[i]) == i);
    }
}

static void test_hostile_documents_fail_closed(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 10: hostile documents fail closed with the named error...\n");

    NkInputProfileFile doc;
    char label[64];
    size_t len;

    /* The corpus only means something if the document it is derived from is one
     * the parser accepts: otherwise every refusal below would pass for the
     * wrong reason. */
    len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL, NULL, NULL);
    expect_accepted("seed document", s_hostile_doc, len, &doc);
    assert(strcmp(doc.global.guid, "g") == 0);
    assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_SOUTH);

    /* --- Truncated files: never a partial mapping -------------------------- */
    {
        char entries[1024];
        char seed[2048];
        (void)render_entry(entries, sizeof(entries), "\"UCUS98701\"", NULL);
        size_t seed_len = render_per_title_doc(seed, sizeof(seed), "2", entries);

        /* Every proper prefix of an accepted document is refused, the zero-length
         * one included, and each refusal leaves the default mapping in place. */
        for (size_t cut = 0; cut < seed_len; cut++) {
            char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
            char why[NK_INPUT_DIAGNOSTIC_MAX_LEN];
            NkInputProfileFile cut_doc;
            diag[0] = '\0';
            NkResult res = nk_input_profile_file_parse_json(&cut_doc, seed, cut,
                                                            diag, sizeof(diag));
            if (res == NK_OK || diag[0] == '\0') {
                printf("  truncated at %zu of %zu bytes: expected a refusal, got res=%d diag='%s'\n",
                       cut, seed_len, (int)res, diag);
                assert(0 && "a truncated document was not refused");
            }
            assert(cut_doc.title_count == 0);
            assert(strcmp(cut_doc.global.guid, "default") == 0);
            assert(nk_input_profile_validate(&cut_doc.global, why, sizeof(why)) == NK_OK);
        }
        expect_accepted("untruncated per-title document", seed, seed_len, &doc);
        assert(doc.title_count == 1);
        assert(strcmp(doc.title_disc_id[0], "UCUS98701") == 0);
    }

    expect_refused("empty document", "", 0, "empty JSON input");
    expect_refused("whitespace only", "  \n\t\r ", 6, NULL);
    expect_refused("root is an array", "[1,2,3]", 7, "root JSON node must be an object");
    expect_refused("root is a bare number", "42", 2, "root JSON node must be an object");
    expect_refused("trailing garbage after the root", "{\"schema_version\":1} GARBAGE", 27,
                   "Trailing garbage");
    expect_refused("unterminated string", "{\"schema_version\":2,\"device\":{\"guid\":\"g",
                   39, "Unterminated string");
    expect_refused("truncated per_title array", "{\"schema_version\":2,\"per_title\":[", 33, NULL);
    expect_refused("missing schema_version", "{\"device\":{}}", 13,
                   "missing or non-numeric schema_version");

    /* A NUL byte in the file is data, not a terminator: the parser is handed a
     * length, so a raw control character inside a string is refused. */
    {
        const char raw_nul[] = "{\"schema_version\":2,\"device\":{\"guid\":\"g\0h\"}}";
        expect_refused("raw NUL byte inside a string", raw_nul, sizeof(raw_nul) - 1,
                       "Unescaped control character in string");
    }

    /* --- Unknown keys: never acted on -------------------------------------- */
    {
        NkInputProfileFile plain;
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL, NULL, NULL);
        expect_accepted("plain document", s_hostile_doc, len, &plain);

        /* An unknown key at any level is ignored, because the format is
         * forward-compatible. What must never happen is an unknown key taking
         * effect: the mapping that loads is bit-for-bit the one the known
         * members describe. */
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc),
                         "\"device\": { \"guid\": \"g\", \"name_hint\": \"Pad\", \"vendor\": \"acme\" }",
                         "\"calibration\": { \"trigger_threshold\": 8192, \"future_knob\": 3,"
                         " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
                         " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
                         "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"south\","
                         " \"tertiary\": \"east\" } ]",
                         "\"navigation_bindings\": [ { \"action\": \"confirm\","
                         " \"primary\": \"button:south\", \"extra\": 1 } ]");
        expect_accepted("document with unknown keys", s_hostile_doc, len, &doc);
        assert(memcmp(&plain.global, &doc.global, sizeof(NkInputProfile)) == 0);

        /* A misspelled REQUIRED key is different: the member it stood for is
         * missing, so the document is refused naming that member. */
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL,
                         "\"calibration\": { \"trigger_threshhold\": 8192,"
                         " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
                         " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
                         NULL, NULL);
        expect_refused("misspelled trigger_threshold", s_hostile_doc, len,
                       "calibration missing 'trigger_threshold'");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL,
                         "\"calibration\": { \"trigger_threshold\": 8192,"
                         " \"analog_x\": { \"host_axsi\": \"leftx\", \"deadzone_inner\": 1000 },"
                         " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
                         NULL, NULL);
        expect_refused("misspelled host_axis", s_hostile_doc, len, "missing 'host_axis' string");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), "\"devise\": { \"guid\": \"g\" }",
                         NULL, NULL, NULL);
        expect_refused("misspelled device object", s_hostile_doc, len, "missing 'device' object");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL, NULL,
                         "\"navigation_bindings\": {}");
        expect_refused("navigation_bindings is an object", s_hostile_doc, len,
                       "missing 'navigation_bindings' array");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL,
                         "\"calibration\": { \"trigger_threshold\": 8192,"
                         " \"analog_x\": \"leftx\","
                         " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
                         NULL, NULL);
        expect_refused("analog_x is a string", s_hostile_doc, len,
                       "axis 'analog_x' definition must be an object");

        /* A member of the wrong type is refused where it is required, and falls
         * back to the documented default where the format makes it optional. */
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL,
                         "\"calibration\": { \"trigger_threshold\": \"8192\","
                         " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
                         " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
                         NULL, NULL);
        expect_refused("trigger_threshold is a string", s_hostile_doc, len,
                       "calibration missing 'trigger_threshold'");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL,
                         "\"calibration\": { \"trigger_threshold\": 8192,"
                         " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000,"
                         " \"inverted\": 1 },"
                         " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
                         NULL, NULL);
        expect_refused("inverted is a number", s_hostile_doc, len, "'inverted' must be boolean");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                         "\"psp_bindings\": { \"control\": \"cross\" }", NULL);
        expect_refused("psp_bindings is an object", s_hostile_doc, len,
                       "missing 'psp_bindings' array");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc),
                         "\"device\": { \"guid\": \"g\", \"name_hint\": 42 }", NULL, NULL, NULL);
        expect_accepted("non-string name_hint falls back", s_hostile_doc, len, &doc);
        assert(strcmp(doc.global.name_hint, "Generic Controller") == 0);
    }

    /* --- Duplicate keys at every level ------------------------------------- */
    {
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc),
                         "\"device\": { \"guid\": \"g\", \"guid\": \"g2\" }", NULL, NULL, NULL);
        expect_refused("duplicate guid", s_hostile_doc, len, "duplicate JSON object key");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL,
                         "\"calibration\": { \"trigger_threshold\": 8192,"
                         " \"trigger_threshold\": 9000,"
                         " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
                         " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
                         NULL, NULL);
        expect_refused("duplicate trigger_threshold", s_hostile_doc, len,
                       "duplicate JSON object key");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL,
                         "\"calibration\": { \"trigger_threshold\": 8192,"
                         " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
                         " \"analog_x\": { \"host_axis\": \"rightx\", \"deadzone_inner\": 1000 },"
                         " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
                         NULL, NULL);
        expect_refused("duplicate analog_x", s_hostile_doc, len, "duplicate JSON object key");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                         "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"south\","
                         " \"primary\": \"east\" } ]", NULL);
        expect_refused("duplicate primary in a binding", s_hostile_doc, len,
                       "duplicate JSON object key");

        /* A key that equals a real one only up to an embedded NUL is an unknown
         * key, not a duplicate -- and an unknown binding key binds nothing. */
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                         "\"psp_bindings\": [ { \"control\": \"cross\","
                         " \"pri\\u0000mary\": \"south\" } ]", NULL);
        expect_refused("binding key with an embedded NUL", s_hostile_doc, len,
                       "neither 'primary' nor 'secondary'");

        {
            char entries[1024];
            (void)render_entry(entries, sizeof(entries),
                               "\"UCUS98701\", \"disc_id\": \"ULUS10041\"", NULL);
            len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
            expect_refused("duplicate disc_id in one per_title entry", s_hostile_doc, len,
                           "duplicate JSON object key");
        }
    }

    /* --- Out-of-range axis values ------------------------------------------ */
    {
        static const struct {
            const char *calibration;
            const char *named_error;
        } kAxisCases[] = {
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 32768 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "deadzone_inner" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": -1 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "deadzone_inner" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000,"
              " \"deadzone_outer\": -5 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "deadzone_outer" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 20000,"
              " \"deadzone_outer\": 20000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "combined deadzones" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000,"
              " \"rest\": 32768 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "rest out of range" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000,"
              " \"min_val\": 40000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "min_val out of range" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000,"
              " \"max_val\": -40000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "max_val out of range" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000,"
              " \"min_val\": 100, \"max_val\": -100 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "must be less than max_val" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000,"
              " \"min_val\": 0, \"max_val\": 100, \"rest\": 5000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "rest (5000) out of range" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"left_z\", \"deadzone_inner\": 1000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "unknown host axis" },
            { "\"calibration\": { \"trigger_threshold\": 8192,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
              " \"analog_y\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 } }",
              "conflicting axis binding" }
        };
        size_t cases = sizeof(kAxisCases) / sizeof(kAxisCases[0]);
        for (size_t i = 0; i < cases; i++) {
            size_t dlen = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL,
                                     kAxisCases[i].calibration, NULL, NULL);
            snprintf(label, sizeof(label), "axis case %zu", i);
            expect_refused(label, s_hostile_doc, dlen, kAxisCases[i].named_error);
        }

        /* Trigger bounds, including magnitudes that overflow a double to
         * infinity: refused, never cast into the field. */
        static const struct {
            const char *calibration;
            const char *named_error;
        } kTriggerCases[] = {
            { "\"calibration\": { \"trigger_threshold\": 32768,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "trigger_threshold" },
            { "\"calibration\": { \"trigger_threshold\": -1,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "trigger_threshold" },
            { "\"calibration\": { \"trigger_threshold\": 1e999,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "trigger_threshold" },
            { "\"calibration\": { \"trigger_threshold\": -1e999,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "trigger_threshold" },
            { "\"calibration\": { \"trigger_threshold\": 8192, \"trigger_rest\": 40000,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "trigger_rest out of range" },
            { "\"calibration\": { \"trigger_threshold\": 8192, \"trigger_rest\": 30000,"
              " \"trigger_extreme\": 100,"
              " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
              " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } }",
              "must be less than trigger_extreme" }
        };
        cases = sizeof(kTriggerCases) / sizeof(kTriggerCases[0]);
        for (size_t i = 0; i < cases; i++) {
            size_t dlen = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL,
                                     kTriggerCases[i].calibration, NULL, NULL);
            snprintf(label, sizeof(label), "trigger case %zu", i);
            expect_refused(label, s_hostile_doc, dlen, kTriggerCases[i].named_error);
        }
    }

    /* --- Out-of-range button values ---------------------------------------- */
    {
        static const struct {
            bool nav;
            const char *bindings;
            const char *named_error;
        } kBindingCases[] = {
            { false, "\"psp_bindings\": [ { \"control\": \"crossx\", \"primary\": \"south\" } ]",
              "unknown PSP control" },
            { false, "\"psp_bindings\": [ { \"control\": 42, \"primary\": \"south\" } ]",
              "missing 'control' name" },
            { false, "\"psp_bindings\": [ \"cross\" ]",
              "psp_bindings entry 0 must be an object" },
            { false, "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": 42 } ]",
              "binding 'primary' must be string" },
            { false, "\"psp_bindings\": [ { \"control\": \"cross\", \"secondary\": [ \"south\" ] } ]",
              "binding 'secondary' must be string" },
            { false, "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"button:nonexistent\" } ]",
              "unknown host button" },
            { false, "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"button:\" } ]",
              "unknown host button" },
            { false, "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"trigger:nosuch\" } ]",
              "unknown host axis" },
            { false, "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"+nosuch\" } ]",
              "unknown host axis" },
            { false, "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"-nosuch\" } ]",
              "unknown host axis" },
            { false, "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"hyper_turbo\" } ]",
              "unrecognized host input identifier" },
            { false, "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"button:south\","
                      " \"secondary\": \"button:nowhere\" } ]",
              "unknown host button" },
            { true, "\"navigation_bindings\": [ { \"action\": \"confirmx\", \"primary\": \"south\" } ]",
              "unknown navigation action" },
            { true, "\"navigation_bindings\": [ { \"action\": 7, \"primary\": \"south\" } ]",
              "missing 'action' name" },
            { true, "\"navigation_bindings\": [ { \"action\": \"confirm\", \"primary\": \"nope\" } ]",
              "unrecognized host input identifier" }
        };
        size_t cases = sizeof(kBindingCases) / sizeof(kBindingCases[0]);
        for (size_t i = 0; i < cases; i++) {
            size_t dlen = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                                     kBindingCases[i].nav ? NULL : kBindingCases[i].bindings,
                                     kBindingCases[i].nav ? kBindingCases[i].bindings : NULL);
            snprintf(label, sizeof(label), "binding case %zu", i);
            expect_refused(label, s_hostile_doc, dlen, kBindingCases[i].named_error);
        }
    }

    /* --- Over-long lines and over-long fields ------------------------------ */
    {
        char field[8192];
        char member[8320];

        /* A field far past its fixed buffer is bounded by the field, not
         * refused: guid and name_hint are informational, and the mapping the
         * runtime gets is the one the prefix describes. The invariant that
         * matters is that nothing is written past the field. */
        fill_repeat(field, sizeof(field), 'A', 4096);
        snprintf(member, sizeof(member), "\"device\": { \"guid\": \"%s\", \"name_hint\": \"Pad\" }",
                 field);
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), member, NULL, NULL, NULL);
        expect_accepted("over-long guid", s_hostile_doc, len, &doc);
        assert(strlen(doc.global.guid) == NK_INPUT_GUID_MAX_LEN - 1);
        assert(doc.global.guid[NK_INPUT_GUID_MAX_LEN - 2] == 'A');

        snprintf(member, sizeof(member),
                 "\"device\": { \"guid\": \"g\", \"name_hint\": \"%s\" }", field);
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), member, NULL, NULL, NULL);
        expect_accepted("over-long name_hint", s_hostile_doc, len, &doc);
        assert(strlen(doc.global.name_hint) == NK_INPUT_NAME_HINT_MAX_LEN - 1);

        /* A field that decides what the parser does is refused instead. */
        snprintf(member, sizeof(member), "\"device\": { \"guid\": \"%s\" }", field);
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), member, NULL, NULL, NULL);
        expect_accepted("over-long guid with no name_hint", s_hostile_doc, len, &doc);

        snprintf(member, sizeof(member), "\"psp_bindings\": [ { \"control\": \"%s\","
                 " \"primary\": \"south\" } ]", field);
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL, member, NULL);
        expect_refused("over-long control name", s_hostile_doc, len, "unknown PSP control");

        snprintf(member, sizeof(member), "\"psp_bindings\": [ { \"control\": \"cross\","
                 " \"primary\": \"%s\" } ]", field);
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL, member, NULL);
        expect_refused("over-long binding string", s_hostile_doc, len,
                       "unrecognized host input identifier");

        snprintf(member, sizeof(member), "\"psp_bindings\": [ { \"control\": \"cross\","
                 " \"primary\": \"button:%s\" } ]", field);
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL, member, NULL);
        expect_refused("over-long host button name", s_hostile_doc, len, "unknown host button");

        /* A disc ID that could name a file is refused, never truncated into one. */
        {
            char entries[8448];
            char disc[8300];
            snprintf(disc, sizeof(disc), "\"%s\"", field);
            (void)render_entry(entries, sizeof(entries), disc, NULL);
            len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
            expect_refused("over-long disc_id", s_hostile_doc, len, "disc_id");
        }

        /* One 200 KiB line, with no line break anywhere in it: a document the
         * parser must still read to the end. */
        {
            static const char head[] =
                "{\"schema_version\":2,\"device\":{\"guid\":\"g\",\"name_hint\":\"";
            static const char tail[] =
                "\"},\"calibration\":{\"trigger_threshold\":8192,"
                "\"analog_x\":{\"host_axis\":\"leftx\",\"deadzone_inner\":1000},"
                "\"analog_y\":{\"host_axis\":\"lefty\",\"deadzone_inner\":1000}},"
                "\"psp_bindings\":[{\"control\":\"cross\",\"primary\":\"south\"}],"
                "\"navigation_bindings\":[{\"action\":\"confirm\",\"primary\":\"button:south\"}]}";
            size_t head_len = sizeof(head) - 1;
            size_t tail_len = sizeof(tail) - 1;
            size_t big_len = 200 * 1024;
            assert(head_len + big_len + tail_len + 1 <= sizeof(s_hostile_doc));
            fill_repeat(s_hostile_big, sizeof(s_hostile_big), 'A', big_len);
            memcpy(s_hostile_doc, head, head_len);
            memcpy(s_hostile_doc + head_len, s_hostile_big, big_len);
            memcpy(s_hostile_doc + head_len + big_len, tail, tail_len);
            len = head_len + big_len + tail_len;
            s_hostile_doc[len] = '\0';
        }
        assert(memchr(s_hostile_doc, '\n', len) == NULL);
        expect_accepted("one 200 KiB line", s_hostile_doc, len, &doc);
        assert(strlen(doc.global.name_hint) == NK_INPUT_NAME_HINT_MAX_LEN - 1);

        /* The same line with a byte before the end turned to junk: refused. */
        s_hostile_doc[len - 2] = '\x01';
        expect_refused("one 200 KiB line with junk near the end", s_hostile_doc, len, NULL);
    }

    /* --- Embedded NULs ------------------------------------------------------ */
    /* A \u0000 escape decodes to a NUL inside a C string, so everything
     * behind it is unreachable. What must never happen is the bytes behind a
     * NUL reaching a file name, a binding or a buffer index. */
    {
        char entries[1024];

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc),
                         "\"device\": { \"guid\": \"a\\u0000b\", \"name_hint\": \"P\\u0000ad\" }",
                         NULL, NULL, NULL);
        expect_accepted("NUL inside an informational field", s_hostile_doc, len, &doc);
        assert(strcmp(doc.global.guid, "a") == 0);
        assert(strcmp(doc.global.name_hint, "P") == 0);

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                         "\"psp_bindings\": [ { \"control\": \"cro\\u0000ss\","
                         " \"primary\": \"south\" } ]", NULL);
        expect_refused("NUL inside a control name", s_hostile_doc, len,
                       "unknown PSP control 'cro'");

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                         "\"psp_bindings\": [ { \"control\": \"cross\","
                         " \"primary\": \"\\u0000south\" } ]", NULL);
        expect_refused("NUL at the head of a binding", s_hostile_doc, len,
                       "binding 'primary' is an empty string");

        /* The bytes behind the NUL never reach the mapping: what binds is the
         * prefix, and the prefix is still a host input in range. */
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                         "\"psp_bindings\": [ { \"control\": \"cross\","
                         " \"primary\": \"south\\u0000/../../evil\" } ]", NULL);
        expect_accepted("NUL inside a binding string", s_hostile_doc, len, &doc);
        assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.type == NK_BINDING_HOST_BUTTON);
        assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_SOUTH);

        /* A disc ID whose usable prefix is a plain name loads as exactly that
         * prefix, and the whole stored ID is one the whitelist accepts. */
        (void)render_entry(entries, sizeof(entries), "\"UCU\\u0000/../../secret\"", NULL);
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
        expect_accepted("NUL inside a disc_id", s_hostile_doc, len, &doc);
        assert(doc.title_count == 1);
        assert(strcmp(doc.title_disc_id[0], "UCU") == 0);
        assert(nk_input_profile_disc_id_safe(doc.title_disc_id[0]));
        assert(nk_input_profile_file_find_title(&doc, "UCU") == 0);
        assert(nk_input_profile_file_find_title(&doc, "UCU/../../secret") == -1);

        /* A disc ID whose prefix is itself unsafe is refused outright. */
        (void)render_entry(entries, sizeof(entries), "\"..\\u0000/x\"", NULL);
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
        expect_refused("unsafe disc_id prefix before a NUL", s_hostile_doc, len, "disc_id");

        (void)render_entry(entries, sizeof(entries), "\"\\u0000\"", NULL);
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
        expect_refused("disc_id that is only a NUL", s_hostile_doc, len, "disc_id");
    }

    /* --- Invalid UTF-8 ------------------------------------------------------ */
    /* A lead byte above 0xF4 is also invalid UTF-8, and the shared validator
     * (src/core/nk_json.c nk_json_validate_utf8) accepts it today: the 4-byte
     * branch only range-checks the 0xF0 and 0xF4 lead bytes, so 0xF5..0xFF slip
     * through. It costs no memory safety here -- the bytes stay inside a
     * NUL-terminated field -- so the case is left to the JSON owner rather than
     * asserted here as accepted. See the report. */
    {
        static const char *const kBadUtf8[] = {
            "\x80",                    /* lone continuation byte */
            "\xC0\x80",                /* overlong NUL */
            "\xC3",                    /* truncated 2-byte sequence */
            "\xE2\x82",                /* truncated 3-byte sequence */
            "\xED\xA0\x80",            /* UTF-16 surrogate half */
            "\xF0\x9F",                /* 4-byte sequence cut short */
            "\xFE\xFF"                 /* not a UTF-8 byte at all */
        };
        size_t bad_count = sizeof(kBadUtf8) / sizeof(kBadUtf8[0]);

        for (size_t i = 0; i < bad_count; i++) {
            char member[64];
            char entries[1024];
            size_t n;

            snprintf(member, sizeof(member), "\"device\": { \"guid\": \"g%s\" }", kBadUtf8[i]);
            len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), member, NULL, NULL, NULL);
            snprintf(label, sizeof(label), "invalid UTF-8 %zu in a guid", i);
            expect_refused(label, s_hostile_doc, len, "UTF-8");

            snprintf(member, sizeof(member),
                     "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"%s\" } ]",
                     kBadUtf8[i]);
            len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL, member, NULL);
            snprintf(label, sizeof(label), "invalid UTF-8 %zu in a binding", i);
            expect_refused(label, s_hostile_doc, len, "UTF-8");

            n = (size_t)snprintf(entries, sizeof(entries),
                                 "{ \"disc_id\": \"UCU%s\", \"profile\": %s }",
                                 kBadUtf8[i], kSeedEntryProfile);
            assert(n < sizeof(entries));
            len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
            snprintf(label, sizeof(label), "invalid UTF-8 %zu in a disc_id", i);
            expect_refused(label, s_hostile_doc, len, "UTF-8");
        }

        /* A key position, so the refusal cannot depend on where the bad byte is. */
        {
            const char bad_key[] = "{\"schema_version\":2,\"devi\xFF" "ce\":{\"guid\":\"g\"}}";
            expect_refused("invalid UTF-8 in a key", bad_key, sizeof(bad_key) - 1, "UTF-8");
        }

        /* Valid multi-byte text is data, not an attack: it must load, and it
         * must survive a save/load round trip byte for byte. */
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc),
                         "\"device\": { \"guid\": \"g\","                         "\"name_hint\": \"P\xc3" "\xa4" "d \xf0\x9f\x8e\xae\" }",
                         NULL, NULL, NULL);
        expect_accepted("valid multi-byte name_hint", s_hostile_doc, len, &doc);
        assert(strcmp(doc.global.name_hint, "P\xc3" "\xa4" "d \xf0\x9f\x8e\xae") == 0);
        {
            const char *path = "build/test_hostile_utf8.json";
            char why[NK_INPUT_DIAGNOSTIC_MAX_LEN];
            assert(nk_input_profile_file_save(&doc, path, why, sizeof(why)) == NK_OK);
            NkInputProfileFile back;
            assert(nk_input_profile_file_load(&back, path, why, sizeof(why)) == NK_OK);
            assert(strcmp(back.global.name_hint, "P\xc3" "\xa4" "d \xf0\x9f\x8e\xae") == 0);
            remove(path);
        }
    }

    /* --- Reserved Windows device names in disc IDs ------------------------ */
    {
        static const char *const kReserved[] = {
            "NUL", "nul", "NUL.json", "nul.txt", "Nul",
            "CON", "con.json", "PRN", "prn.json", "AUX", "aux.json",
            "COM1", "com1.json", "COM2", "COM3", "COM4", "COM5", "COM6",
            "COM7", "COM8", "COM9", "com9.txt",
            "LPT1", "lpt1.json", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
            "LPT7", "LPT8", "LPT9", "lpt9.txt"
        };
        static const char *const kAllowed[] = {
            "COM0", "COM10", "LPT0", "LPT10", "CONSOLE", "NULS", "PRINTER",
            "AUXILIARY", "UCUS98701", "TEST.00001", "T-1234_5"
        };
        size_t n_reserved = sizeof(kReserved) / sizeof(kReserved[0]);
        size_t n_allowed = sizeof(kAllowed) / sizeof(kAllowed[0]);
        char entries[1024];
        char disc[64];

        /* A disc ID names a file under the profile directory, so the Windows
         * device names are refused there exactly as they are on the way in
         * through the API. */
        for (size_t i = 0; i < n_reserved; i++) {
            size_t dlen;
            assert(!nk_input_profile_disc_id_safe(kReserved[i]));
            snprintf(disc, sizeof(disc), "\"%s\"", kReserved[i]);
            (void)render_entry(entries, sizeof(entries), disc, NULL);
            dlen = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
            snprintf(label, sizeof(label), "reserved disc_id %s", kReserved[i]);
            expect_refused(label, s_hostile_doc, dlen, "disc_id");
        }

        for (size_t i = 0; i < n_allowed; i++) {
            size_t dlen;
            assert(nk_input_profile_disc_id_safe(kAllowed[i]));
            snprintf(disc, sizeof(disc), "\"%s\"", kAllowed[i]);
            (void)render_entry(entries, sizeof(entries), disc, NULL);
            dlen = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
            snprintf(label, sizeof(label), "allowed disc_id %s", kAllowed[i]);
            expect_accepted(label, s_hostile_doc, dlen, &doc);
            assert(doc.title_count == 1);
            assert(strcmp(doc.title_disc_id[0], kAllowed[i]) == 0);
        }

        /* Path-shaped and dot-shaped names never reach the filesystem. Each id
         * is the C string the whitelist sees, and the JSON literal the document
         * carries, so a control character is written escaped rather than raw. */
        static const struct {
            const char *id;
            const char *json;
        } kUnsafeIds[] = {
            { ".", "\".\"" },
            { "..", "\"..\"" },
            { "UCUS98701.", "\"UCUS98701.\"" },
            { "a/b", "\"a/b\"" },
            { "a\\b", "\"a\\\\b\"" },
            { "C:evil", "\"C:evil\"" },
            { "UC US", "\"UC US\"" },
            { "uc\tx", "\"uc\\tx\"" },
            { "UCUS98701..", "\"UCUS98701..\"" },
            { "  ", "\"  \"" },
            { "AUX ", "\"AUX \"" },
            { "%2e%2e", "\"%2e%2e\"" }
        };
        size_t n_unsafe = sizeof(kUnsafeIds) / sizeof(kUnsafeIds[0]);
        for (size_t i = 0; i < n_unsafe; i++) {
            size_t dlen;
            assert(!nk_input_profile_disc_id_safe(kUnsafeIds[i].id));
            (void)render_entry(entries, sizeof(entries), kUnsafeIds[i].json, NULL);
            dlen = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
            snprintf(label, sizeof(label), "unsafe disc_id %s", kUnsafeIds[i].id);
            expect_refused(label, s_hostile_doc, dlen, "disc_id");
        }

        /* A disc ID one character too long is refused, not truncated into a
         * different disc's file. */
        {
            char longest[NK_MAX_DISC_ID_LEN + 1];
            memset(longest, 'A', sizeof(longest) - 1);
            longest[sizeof(longest) - 1] = '\0';
            assert(strlen(longest) == NK_MAX_DISC_ID_LEN);
            assert(!nk_input_profile_disc_id_safe(longest));
            snprintf(disc, sizeof(disc), "\"%s\"", longest);
            (void)render_entry(entries, sizeof(entries), disc, NULL);
            len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
            expect_refused("disc_id at the length limit", s_hostile_doc, len, "disc_id");

            longest[NK_MAX_DISC_ID_LEN - 1] = '\0';
            assert(nk_input_profile_disc_id_safe(longest));
            snprintf(disc, sizeof(disc), "\"%s\"", longest);
            (void)render_entry(entries, sizeof(entries), disc, NULL);
            len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
            expect_accepted("disc_id just under the length limit", s_hostile_doc, len, &doc);
        }
    }

    /* --- The per-title table's maximum entry count ------------------------- */
    {
        char entries[NK_INPUT_MAX_PER_TITLE * 512];
        char disc[64];
        size_t used = 0;
        int written;

        /* Exactly the bound: every disc keeps its own mapping. */
        for (int i = 0; i < NK_INPUT_MAX_PER_TITLE; i++) {
            char one[512];
            written = snprintf(one, sizeof(one),
                               "%s{ \"disc_id\": \"TITLE%05d\", \"profile\": %s }",
                               i ? ", " : "", i, kSeedEntryProfile);
            assert(written > 0 && (size_t)written < sizeof(one));
            assert(used + (size_t)written < sizeof(entries));
            memcpy(entries + used, one, (size_t)written);
            used += (size_t)written;
        }
        entries[used] = '\0';
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
        expect_accepted("per_title at the maximum entry count", s_hostile_doc, len, &doc);
        assert(doc.title_count == NK_INPUT_MAX_PER_TITLE);
        for (int i = 0; i < NK_INPUT_MAX_PER_TITLE; i++) {
            char want[16];
            const NkInputProfile *effective;
            snprintf(want, sizeof(want), "TITLE%05d", i);
            assert(nk_input_profile_file_find_title(&doc, want) == i);
            effective = nk_input_profile_file_resolve(&doc, want, NULL, 0);
            assert(effective == &doc.title[i]);
        }

        /* One entry past the bound refuses the whole document rather than
         * dropping a disc's mapping without saying so. */
        written = snprintf(entries + used, sizeof(entries) - used,
                           ", { \"disc_id\": \"TITLE99999\", \"profile\": %s }",
                           kSeedEntryProfile);
        assert(written > 0);
        used += (size_t)written;
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
        expect_refused("per_title one entry past the bound", s_hostile_doc, len, "more than");

        /* A repeated disc is ambiguous even when the table has room. */
        used = 0;
        entries[0] = '\0';
        for (int i = 0; i < 2; i++) {
            written = snprintf(entries + used, sizeof(entries) - used,
                               "%s{ \"disc_id\": \"TITLE00001\", \"profile\": %s }",
                               i ? ", " : "", kSeedEntryProfile);
            assert(written > 0);
            used += (size_t)written;
        }
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
        expect_refused("per_title with a repeated disc", s_hostile_doc, len,
                       "duplicate per_title entry");

        /* Structural damage in the array is refused naming the entry. */
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", "\"UCUS98701\"");
        expect_refused("per_title entry is not an object", s_hostile_doc, len,
                       "per_title entry 0 must be an object");

        {
            int n = snprintf(entries, sizeof(entries), "{ \"profile\": %s }",
                             kSeedEntryProfile);
            assert(n > 0 && (size_t)n < sizeof(entries));
            len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
            expect_refused("per_title entry has no disc_id", s_hostile_doc, len, "disc_id");
        }

        (void)render_entry(entries, sizeof(entries), "\"UCUS98701\"", "\"not an object\"");
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
        expect_refused("per_title profile is a string", s_hostile_doc, len,
                       "missing 'profile' object");

        (void)render_entry(entries, sizeof(entries), "\"UCUS98701\"", "[]");
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
        expect_refused("per_title profile is an array", s_hostile_doc, len,
                       "missing 'profile' object");

        /* A per-title map is a schema 2 member; a schema 1 file cannot carry one. */
        (void)render_entry(entries, sizeof(entries), "\"UCUS98701\"", NULL);
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "1", entries);
        expect_refused("per_title in a schema 1 document", s_hostile_doc, len,
                       "cannot carry a per_title map");

        (void)disc;
        len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", "");
        assert(len > 0);
        expect_accepted("empty per_title array", s_hostile_doc, len, &doc);
        assert(doc.title_count == 0);

        {
            int n = snprintf(s_hostile_doc, sizeof(s_hostile_doc),
                             "{\n  \"schema_version\": 2,\n  %s,\n  %s,\n  %s,\n  %s,\n"
                             "  \"per_title\": { \"disc_id\": \"UCUS98701\" }\n}",
                             kSeedDevice, kSeedCalibration, kSeedPspBindings, kSeedNavBindings);
            assert(n > 0 && (size_t)n < sizeof(s_hostile_doc));
            expect_refused("per_title is an object", s_hostile_doc, (size_t)n,
                           "'per_title' must be an array");
        }
    }

    printf("[INPUT_PROFILE_TEST] Subtest 10 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 11: Hostile Files On Disk Fail Closed At The Loader
 * ------------------------------------------------------------------------- */

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (size > 0) {
        assert(fwrite(data, 1, size, f) == size);
    }
    assert(fclose(f) == 0);
}

/* The loader is the boundary a user's file actually crosses, so every refusal
 * has to name itself there and leave the default mapping behind. */
static void expect_load_refused(const char *label, const char *path,
                                const char *named_error) {
    char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    char why[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    NkInputProfileFile doc;

    diag[0] = '\0';
    NkResult res = nk_input_profile_file_load(&doc, path, diag, sizeof(diag));
    if (res == NK_OK || diag[0] == '\0' || (named_error && strstr(diag, named_error) == NULL)) {
        printf("  %s: expected a refusal naming '%s', got res=%d diag='%s'\n",
               label, named_error ? named_error : "(any diagnostic)", (int)res, diag);
        assert(0 && "hostile file was not refused by the loader");
    }
    assert(strcmp(doc.global.guid, "default") == 0);
    assert(doc.global.trigger_threshold == NK_INPUT_DEFAULT_TRIGGER_THRESHOLD);
    assert(doc.title_count == 0);
    assert(nk_input_profile_validate(&doc.global, why, sizeof(why)) == NK_OK);
}

static void test_hostile_profile_file_loader(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 11: hostile files on disk fail closed at the loader...\n");

    const char *path = "build/test_hostile_profile.json";
    char why[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    NkInputProfileFile doc;
    size_t len;

    assert(nk_platform_mkdir_p("build"));

    /* A file that is there but empty is not an empty profile: it is refused. */
    write_bytes(path, "", 0);
    expect_load_refused("empty file", path, "is invalid or exceeds 1 MB");

    /* One byte of a document, and the whole document cut in half. */
    write_bytes(path, "{", 1);
    expect_load_refused("one-byte file", path, NULL);

    {
        char seed[2048];
        char entries[1024];
        (void)render_entry(entries, sizeof(entries), "\"UCUS98701\"", NULL);
        len = render_per_title_doc(seed, sizeof(seed), "2", entries);

        write_bytes(path, seed, len / 2);
        expect_load_refused("file cut in half", path, NULL);

        /* A file whose last byte is a NUL is truncated garbage, not a document. */
        {
            char with_nul[2100];
            assert(len + 1 < sizeof(with_nul));
            memcpy(with_nul, seed, len);
            with_nul[len] = '\0';
            write_bytes(path, with_nul, len + 1);
            expect_load_refused("file with a NUL after the root", path, "Trailing garbage");
        }

        write_bytes(path, seed, len);
        assert(nk_input_profile_file_load(&doc, path, why, sizeof(why)) == NK_OK);
        assert(doc.title_count == 1);
        assert(strcmp(doc.title_disc_id[0], "UCUS98701") == 0);
    }

    /* The size cap: one byte past 1 MiB is refused before anything is parsed,
     * and exactly 1 MiB is read and then refused as a malformed document. */
    fill_repeat(s_hostile_big, sizeof(s_hostile_big), 'A', 1024 * 1024);
    write_bytes(path, s_hostile_big, 1024 * 1024);
    expect_load_refused("file of exactly 1 MiB", path, NULL);
    write_bytes(path, s_hostile_big, 1024 * 1024 + 1);
    expect_load_refused("file one byte past 1 MiB", path, "exceeds 1 MB");
    remove(path);

    /* A path that is a directory, a path that is empty, and a path that is not
     * there at all: each one refuses itself, none of them reads. */
    assert(nk_platform_mkdir_p("build/test_hostile_dir"));
    expect_load_refused("path is a directory", "build/test_hostile_dir", NULL);
    expect_load_refused("empty path", "", "file path is null or empty");

    {
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
        diag[0] = '\0';
        NkResult res = nk_input_profile_file_load(&doc, NULL, diag, sizeof(diag));
        assert(res != NK_OK);
        assert(strstr(diag, "file path is null or empty") != NULL);
        assert(strcmp(doc.global.guid, "default") == 0);
    }

    {
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
        diag[0] = '\0';
        NkResult res = nk_input_profile_file_load(&doc, "build/test_hostile_absent.json",
                                                  diag, sizeof(diag));
        assert(res == NK_ERROR_FILE_NOT_FOUND);
        assert(strstr(diag, "not found") != NULL);
        assert(strcmp(doc.global.guid, "default") == 0);
    }

    printf("[INPUT_PROFILE_TEST] Subtest 11 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Subtest 12: A Control The Document Names But Never Binds
 * ------------------------------------------------------------------------- */

static void test_unbound_control_fails_closed(void) {
    printf("[INPUT_PROFILE_TEST] Subtest 12: a control named but never bound is refused...\n");

    char label[64];
    char entries[1024];
    size_t len;

    /* An entry that names a control but not what it binds used to load as a
     * control the guest can never press, with no diagnostic at all. The screen
     * shows a mapping, the file has a control, and the only symptom is a game
     * that looks frozen. Each shape below used to load; each now fails closed
     * naming the binding that is missing. */
    static const struct {
        const char *bindings;
        const char *named_error;
    } kUnbound[] = {
        { "\"psp_bindings\": [ { \"control\": \"cross\" } ]",
          "neither 'primary' nor 'secondary'" },
        { "\"psp_bindings\": [ { \"control\": \"cross\", \"prmary\": \"south\" } ]",
          "neither 'primary' nor 'secondary'" },
        { "\"psp_bindings\": [ { \"control\": \"cross\", \"PRIMARY\": \"south\" } ]",
          "neither 'primary' nor 'secondary'" },
        { "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"\" } ]",
          "empty string" },
        { "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"\\u0000south\" } ]",
          "empty string" },
        { "\"psp_bindings\": [ { \"control\": \"cross\", \"secondary\": \"\" } ]",
          "empty string" },
        { "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": null } ]",
          "must be string" },
        { "\"psp_bindings\": \"south\"",
          "missing 'psp_bindings' array" }
    };
    size_t cases = sizeof(kUnbound) / sizeof(kUnbound[0]);
    for (size_t i = 0; i < cases; i++) {
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                         kUnbound[i].bindings, NULL);
        snprintf(label, sizeof(label), "unbound control %zu", i);
        expect_refused(label, s_hostile_doc, len, kUnbound[i].named_error);
    }

    /* The same rule for a navigation action. */
    len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL, NULL,
                     "\"navigation_bindings\": [ { \"action\": \"confirm\" } ]");
    expect_refused("navigation action named but never bound", s_hostile_doc, len,
                   "neither 'primary' nor 'secondary'");

    /* An unbound control inside a per-title entry refuses the whole document,
     * so no disc is left half-mapped. */
    (void)render_entry(entries, sizeof(entries), "\"UCUS98701\"",
                       "{ \"device\": { \"guid\": \"g\" },"
                       " \"calibration\": { \"trigger_threshold\": 8192,"
                       " \"analog_x\": { \"host_axis\": \"leftx\", \"deadzone_inner\": 1000 },"
                       " \"analog_y\": { \"host_axis\": \"lefty\", \"deadzone_inner\": 1000 } },"
                       " \"psp_bindings\": [ { \"control\": \"cross\" } ],"
                       " \"navigation_bindings\": [] }");
    len = render_per_title_doc(s_hostile_doc, sizeof(s_hostile_doc), "2", entries);
    expect_refused("unbound control in a per-title entry", s_hostile_doc, len,
                   "per-title mapping for 'UCUS98701'");

    /* "none" is how a control is deliberately left unbound, and it still
     * loads: the document says so instead of leaving it to be guessed. */
    {
        NkInputProfileFile doc;
        char why[NK_INPUT_DIAGNOSTIC_MAX_LEN];
        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                         "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"none\" } ]",
                         NULL);
        expect_accepted("explicitly unbound control", s_hostile_doc, len, &doc);
        assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.type == NK_BINDING_NONE);
        assert(nk_input_profile_validate(&doc.global, why, sizeof(why)) == NK_OK);

        len = render_doc(s_hostile_doc, sizeof(s_hostile_doc), NULL, NULL,
                         "\"psp_bindings\": [ { \"control\": \"cross\", \"primary\": \"south\","
                         " \"secondary\": \"none\" } ]", NULL);
        expect_accepted("unbound secondary binding", s_hostile_doc, len, &doc);
        assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_SOUTH);
        assert(doc.global.psp_buttons[NK_PSP_BTN_CROSS].secondary.type == NK_BINDING_NONE);
    }

    /* An out-of-range host index in a mapping cannot be written in JSON, but a
     * caller can hand one to the validator (a capture, a future writer, a
     * document round-tripped through memory). It has to be refused naming the
     * field, never indexed with: the conflict pass used to report it as a
     * conflict between two names it did not have. */
    {
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
        NkInputProfile prof;
        struct {
            int button;
            int axis;
            NkBindingSourceType type;
            const char *named_error;
        } kBad[] = {
            { NK_HOST_BUTTON_COUNT, 0, NK_BINDING_HOST_BUTTON, "host button index" },
            { -1, 0, NK_BINDING_HOST_BUTTON, "host button index" },
            { 0, NK_HOST_AXIS_COUNT, NK_BINDING_HOST_TRIGGER, "host axis index" },
            { 0, -1, NK_BINDING_HOST_TRIGGER, "host axis index" },
            { 0, 999, NK_BINDING_HOST_AXIS_POS, "host axis index" },
            { 0, 999, NK_BINDING_HOST_AXIS_NEG, "host axis index" }
        };
        size_t n_bad = sizeof(kBad) / sizeof(kBad[0]);

        for (size_t i = 0; i < n_bad; i++) {
            diag[0] = '\0';
            nk_input_profile_init_default(&prof);
            prof.psp_buttons[NK_PSP_BTN_CROSS].primary.type = kBad[i].type;
            prof.psp_buttons[NK_PSP_BTN_CROSS].primary.index =
                kBad[i].button ? kBad[i].button : kBad[i].axis;
            assert(nk_input_profile_validate(&prof, diag, sizeof(diag)) != NK_OK);
            snprintf(label, sizeof(label), "out-of-range index %zu", i);
            if (strstr(diag, "out of range") == NULL ||
                strstr(diag, kBad[i].named_error) == NULL) {
                printf("  %s: diagnostic '%s' does not name the field\n", label, diag);
                assert(0 && "out-of-range binding index was not named");
            }

            /* The same index on a navigation binding. */
            diag[0] = '\0';
            nk_input_profile_init_default(&prof);
            prof.nav_bindings[NK_NAV_ACTION_CONFIRM].secondary.type = kBad[i].type;
            prof.nav_bindings[NK_NAV_ACTION_CONFIRM].secondary.index =
                kBad[i].button ? kBad[i].button : kBad[i].axis;
            assert(nk_input_profile_validate(&prof, diag, sizeof(diag)) != NK_OK);
            assert(strstr(diag, "out of range") != NULL);
        }

        /* A null profile is a caller's error, named rather than read. */
        diag[0] = '\0';
        assert(nk_input_profile_validate(NULL, diag, sizeof(diag)) != NK_OK);
        assert(strstr(diag, "null") != NULL);

        /* An out-of-range mapping is refused on the way into a document too. */
        {
            NkInputProfileFile target;
            nk_input_profile_file_init_default(&target);
            nk_input_profile_init_default(&prof);
            prof.psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_COUNT;
            diag[0] = '\0';
            assert(nk_input_profile_file_set_title(&target, "UCUS98701", &prof,
                                                   diag, sizeof(diag)) != NK_OK);
            assert(strstr(diag, "out of range") != NULL);
            assert(target.title_count == 0);

            /* And the document the player is editing keeps its entries. */
        }
    }

    printf("[INPUT_PROFILE_TEST] Subtest 12 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * Main Runner
 * -------------------------------------------------------------------------- */

int main(void) {
    /* Unbuffered: a failing case names itself on stdout, and the harness reads
     * that line even when the run aborts on the assert that follows. */
    setbuf(stdout, NULL);

    printf("=================================================================\n");
    printf("Starting Nakagawa Host Input Profile & Calibration Test Suite\n");
    printf("=================================================================\n");

    test_default_equals_current_mapping();
    test_deadzone_and_inversion_vectors();
    test_strict_load_failures();
    test_future_version_refused();
    test_conflict_detection();
    test_save_load_round_trip();
    test_per_title_selection_and_fallback();
    test_schema_one_migration();
    test_per_title_fail_closed();
    test_hostile_documents_fail_closed();
    test_hostile_profile_file_loader();
    test_unbound_control_fails_closed();

    printf("=================================================================\n");
    printf("ALL HOST INPUT PROFILE TESTS PASSED SUCCESSFULLY!\n");
    printf("=================================================================\n");
    return 0;
}
