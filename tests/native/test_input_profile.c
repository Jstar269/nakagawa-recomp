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
 * Main Runner
 * -------------------------------------------------------------------------- */

int main(void) {
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

    printf("=================================================================\n");
    printf("ALL HOST INPUT PROFILE TESTS PASSED SUCCESSFULLY!\n");
    printf("=================================================================\n");
    return 0;
}
