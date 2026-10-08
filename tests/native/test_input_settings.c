/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* setenv/unsetenv are POSIX: without this, -std=c99 on glibc leaves them undeclared. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "input_settings.h"
#include "nk_input_profile.h"
#include "nk_platform.h"

/* The per-run root setup and its recursive cleanup run inside assert(); keep
 * them even in a build that defines NDEBUG (#735). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#include <windows.h>
#include <wchar.h>
#define test_rmdir _rmdir
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define test_rmdir rmdir
#endif

static void set_environment_value(const char *name, const char *value) {
#if defined(_WIN32) || defined(_WIN64)
    assert(_putenv_s(name, value ? value : "") == 0);
#else
    if (value) {
        assert(setenv(name, value, 1) == 0);
    } else {
        assert(unsetenv(name) == 0);
    }
#endif
}

static void test_setenv(const char *name, const char *val) {
    set_environment_value(name, val);
}

/* Per-run cache and config isolation (#735 item 8, #745).
 *
 * The native tests must never write synthetic fixtures into the developer's
 * real per-user directories (%LOCALAPPDATA%\Nakagawa on Windows). Each run
 * creates its own temporary root under the system temp directory, points the
 * per-user variables into that root before any fixture is written, and removes
 * the root at exit. Removal also runs from a SIGABRT handler, because assert()
 * reaches abort(), which skips atexit handlers. */
#define NATIVE_TEST_PATH_MAX 1024

static char g_test_root[NATIVE_TEST_PATH_MAX];
static bool g_test_root_owned = false;

static bool native_path_within(const char *child, const char *parent) {
    size_t parent_length = strlen(parent);
    if (strlen(child) < parent_length) return false;
    if (strncmp(child, parent, parent_length) != 0) return false;
    return child[parent_length] == '\0' || child[parent_length] == '/' ||
           child[parent_length] == '\\';
}

/* Removes a directory tree without following links: a symlink or a Win32
 * reparse point is removed as itself, never descended into. */
#if defined(_WIN32) || defined(_WIN64)
static void remove_test_tree_wide(const WCHAR *dir) {
    WCHAR pattern[NATIVE_TEST_PATH_MAX];
    WCHAR child[NATIVE_TEST_PATH_MAX];
    size_t dir_length = wcslen(dir);
    if (dir_length + 3 >= NATIVE_TEST_PATH_MAX) return;
    memcpy(pattern, dir, dir_length * sizeof(WCHAR));
    pattern[dir_length] = L'\\';
    pattern[dir_length + 1] = L'*';
    pattern[dir_length + 2] = L'\0';

    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(pattern, &found);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 ||
                wcscmp(found.cFileName, L"..") == 0) continue;
            size_t name_length = wcslen(found.cFileName);
            if (dir_length + 1 + name_length >= NATIVE_TEST_PATH_MAX) continue;
            memcpy(child, dir, dir_length * sizeof(WCHAR));
            child[dir_length] = L'\\';
            memcpy(child + dir_length + 1, found.cFileName,
                   (name_length + 1) * sizeof(WCHAR));
            bool is_directory =
                (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool is_link =
                (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (is_directory && !is_link) remove_test_tree_wide(child);
            else if (is_directory) RemoveDirectoryW(child);
            else DeleteFileW(child);
        } while (FindNextFileW(handle, &found));
        FindClose(handle);
    }
    RemoveDirectoryW(dir);
}

static void remove_test_tree(const char *path) {
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            (int)(sizeof(wide) / sizeof(wide[0]))) > 0) {
        remove_test_tree_wide(wide);
    }
}
#else
static void remove_test_tree(const char *path) {
    /* Attempt the removal before any inspection, so no earlier check can go
     * stale. unlink() removes a symlink as itself and fails on a directory
     * (EISDIR on Linux, EPERM on BSD/macOS); ENOENT means nothing to remove. */
    if (unlink(path) == 0 || errno == ENOENT) return;
    /* O_NOFOLLOW makes open() refuse a symlink swapped in for the directory,
     * so the walk below cannot descend outside the root. */
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd >= 0) {
        DIR *dir = fdopendir(fd);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) continue;
                char child[NATIVE_TEST_PATH_MAX];
                int written = snprintf(child, sizeof(child), "%s/%s", path,
                                       entry->d_name);
                if (written > 0 && (size_t)written < sizeof(child)) {
                    remove_test_tree(child);
                }
            }
            closedir(dir);
        } else {
            close(fd);
        }
    }
    rmdir(path);
}
#endif

static void cleanup_test_root(void) {
    if (!g_test_root_owned) return;
    g_test_root_owned = false;
#if defined(_WIN32) || defined(_WIN64)
    /* Win32 cannot remove a directory that is a process's working directory,
     * and a failing assertion can leave the working directory inside the root.
     * Move it to the temporary directory first. */
    WCHAR temp[NATIVE_TEST_PATH_MAX];
    DWORD temp_length = GetTempPathW((DWORD)(sizeof(temp) / sizeof(temp[0])), temp);
    if (temp_length > 0 && temp_length < sizeof(temp) / sizeof(temp[0])) {
        SetCurrentDirectoryW(temp);
    }
#endif
    remove_test_tree(g_test_root);
}

static void cleanup_test_root_on_abort(int signal_number) {
    cleanup_test_root();
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static bool native_temp_directory(char *out, size_t max_len) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    DWORD count = GetTempPathW((DWORD)(sizeof(wide) / sizeof(wide[0])), wide);
    if (count == 0 || count >= sizeof(wide) / sizeof(wide[0])) return false;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)max_len,
                            NULL, NULL) <= 0) return false;
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    if (strlen(tmp) >= max_len) return false;
    memcpy(out, tmp, strlen(tmp) + 1);
#endif
    size_t length = strlen(out);
    while (length > 1 && (out[length - 1] == '\\' || out[length - 1] == '/')) {
        out[--length] = '\0';
    }
    return length > 0;
}

/* Creates one directory with a single exclusive create: 1 when this call made
 * it, 0 when the name is already taken, -1 on any other failure. Nothing is
 * checked before the create, so a name planted in the temporary directory is
 * never trusted or raced. */
static int create_exclusive_directory(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            (int)(sizeof(wide) / sizeof(wide[0]))) <= 0) return -1;
    if (CreateDirectoryW(wide, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
    if (mkdir(path, 0700) == 0) return 1;
    return errno == EEXIST ? 0 : -1;
#endif
}

/* Creates this run's root, named by tag and process id, and arranges for it
 * to be removed at exit and on SIGABRT. Each candidate name is claimed by the
 * exclusive create itself; a name that is already taken moves to the next. */
static void create_test_root(const char *tag) {
    char temp_dir[NATIVE_TEST_PATH_MAX];
    assert(native_temp_directory(temp_dir, sizeof(temp_dir)));
#if defined(_WIN32) || defined(_WIN64)
    unsigned long process_id = (unsigned long)GetCurrentProcessId();
#else
    unsigned long process_id = (unsigned long)getpid();
#endif
    char sep = nk_platform_path_separator();
    bool created = false;
    for (unsigned attempt = 0; attempt < 1000u && !created; ++attempt) {
        int written = snprintf(g_test_root, sizeof(g_test_root),
                               "%s%cnk-native-%s-%lu-%u", temp_dir, sep, tag,
                               process_id, attempt);
        assert(written > 0 && (size_t)written < sizeof(g_test_root));
        int result = create_exclusive_directory(g_test_root);
        assert(result >= 0);
        created = result == 1;
    }
    assert(created);
    g_test_root_owned = true;
    (void)atexit(cleanup_test_root);
    (void)signal(SIGABRT, cleanup_test_root_on_abort);
}

/* Points every per-user root into this run's temporary root. Win32 reads an
 * explicit LOCALAPPDATA ahead of the Known Folder; POSIX reads the XDG_* and
 * HOME variables. */
static void isolate_user_data_roots(void) {
    assert(g_test_root_owned);
#if defined(_WIN32) || defined(_WIN64)
    set_environment_value("LOCALAPPDATA", g_test_root);
    set_environment_value("APPDATA", g_test_root);
#else
    static const char *const variables[] = {
        "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME"};
    static const char *const subdirs[] = {"cache", "config", "data", "state"};
    set_environment_value("HOME", g_test_root);
    for (size_t i = 0; i < sizeof(variables) / sizeof(variables[0]); ++i) {
        char path[NATIVE_TEST_PATH_MAX + 32];
        int written = snprintf(path, sizeof(path), "%s/%s", g_test_root,
                               subdirs[i]);
        assert(written > 0 && (size_t)written < sizeof(path));
        set_environment_value(variables[i], path);
    }
#endif
}

/* Guard (#735 item 8, #745): fails when the per-user config resolves outside this
 * run's temporary root, which is how a test reaches the real profile. */
static void assert_config_root_isolated(void) {
    char config_dir[NATIVE_TEST_PATH_MAX];
    assert(nk_platform_get_path(NK_PATH_CONFIG, config_dir, sizeof(config_dir)));
    if (!native_path_within(config_dir, g_test_root)) {
        fprintf(stderr,
                "NATIVE_TEST_GUARD: config root is outside the temporary root "
                "(config root '%s', temporary root '%s')\n",
                config_dir, g_test_root);
        exit(EXIT_FAILURE);
    }
}

static char g_real_config_probe[NATIVE_TEST_PATH_MAX];
static bool g_real_config_existed_before = false;
static bool g_real_input_profiles_existed_before = false;
static bool g_real_disc_profile_existed_before = false;

static void capture_real_config_probe(void) {
    g_real_config_probe[0] = '\0';
#if defined(_WIN32) || defined(_WIN64)
    const char *env_lad = getenv("LOCALAPPDATA");
    const char *env_ad = getenv("APPDATA");
    const char *base = (env_lad && env_lad[0]) ? env_lad : ((env_ad && env_ad[0]) ? env_ad : NULL);
    if (base) {
        snprintf(g_real_config_probe, sizeof(g_real_config_probe),
                 "%s\\Nakagawa\\config", base);
    }
#elif defined(__APPLE__)
    const char *env_home = getenv("HOME");
    if (env_home && env_home[0]) {
        snprintf(g_real_config_probe, sizeof(g_real_config_probe),
                 "%s/Library/Application Support/NakagawaRecomp/config", env_home);
    }
#else
    const char *env_xdg = getenv("XDG_CONFIG_HOME");
    const char *env_home = getenv("HOME");
    if (env_xdg && env_xdg[0]) {
        snprintf(g_real_config_probe, sizeof(g_real_config_probe),
                 "%s/nakagawa-recomp", env_xdg);
    } else if (env_home && env_home[0]) {
        snprintf(g_real_config_probe, sizeof(g_real_config_probe),
                 "%s/.config/nakagawa-recomp", env_home);
    }
#endif
    if (g_real_config_probe[0]) {
        char sep = nk_platform_path_separator();
        char probe[NATIVE_TEST_PATH_MAX + 64];
        g_real_config_existed_before = nk_platform_dir_exists(g_real_config_probe);
        snprintf(probe, sizeof(probe), "%s%cinput_profiles", g_real_config_probe, sep);
        g_real_input_profiles_existed_before = nk_platform_dir_exists(probe);
        snprintf(probe, sizeof(probe), "%s%cinput_profiles%cUCUS98701.json",
                 g_real_config_probe, sep, sep);
        g_real_disc_profile_existed_before = nk_platform_file_exists(probe);
    }
}

static void assert_real_config_untouched(void) {
    if (!g_real_config_probe[0]) return;
    char sep = nk_platform_path_separator();
    char probe[NATIVE_TEST_PATH_MAX + 64];
    snprintf(probe, sizeof(probe), "%s%cinput_profiles%cUCUS98701.json",
             g_real_config_probe, sep, sep);
    if (!g_real_disc_profile_existed_before) {
        assert(!nk_platform_file_exists(probe));
    }
    snprintf(probe, sizeof(probe), "%s%cinput_profiles", g_real_config_probe, sep);
    if (!g_real_input_profiles_existed_before) {
        assert(!nk_platform_dir_exists(probe));
    }
    if (!g_real_config_existed_before) {
        assert(!nk_platform_dir_exists(g_real_config_probe));
    }
}

/* -----------------------------------------------------------------------------
 * 1. Default Load
 * -------------------------------------------------------------------------- */
static void test_default_load(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 1: Default load when file is missing...\n");

    InputSettingsState state;
    input_settings_init(&state);

    /* Resolved default profile path must live inside temporary test root */
    assert(state.profile_path[0] != '\0');
    assert(native_path_within(state.profile_path, g_test_root));
    if (g_real_config_probe[0]) {
        assert(!native_path_within(state.profile_path, g_real_config_probe));
    }

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

    /* Path resolution without overrides resolves inside temporary test root */
    res = nk_input_profile_resolve_path(path_buf, sizeof(path_buf));
    assert(res == NK_OK);
    assert(native_path_within(path_buf, g_test_root));
    if (g_real_config_probe[0]) {
        assert(!native_path_within(path_buf, g_real_config_probe));
    }

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

/* -----------------------------------------------------------------------------
 * 10. Per-Title Mapping Scope, Global Isolation, and Atomic Persistence
 * -------------------------------------------------------------------------- */
static void test_per_title_scope_and_atomic_save(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 10: per-title scope, global isolation, atomic save...\n");

    const char *file_path = "build/test_per_title_settings.json";
    remove(file_path);

    /* A fresh state edits the global mapping and owns no per-title entry. */
    InputSettingsState state;
    input_settings_init(&state);
    assert(!input_settings_is_title_scope(&state));
    assert(strcmp(input_settings_scope_label(&state), "Global mapping") == 0);
    assert(state.file.title_count == 0);
    assert(!input_settings_has_title_mapping(&state, "UCUS98701"));

    /* A distinctive global binding to prove the per-title edit cannot touch it. */
    NkBindingSource guide;
    guide.type = NK_BINDING_HOST_BUTTON;
    guide.index = NK_HOST_BUTTON_GUIDE;
    assert(input_settings_assign_binding(&state, INPUT_CONTROL_BTN_CROSS, guide));
    assert(input_settings_save(&state, file_path) == NK_OK);

    /* Choosing a disc's own mapping seeds it from the global mapping, so the
     * first edit in that scope starts from what the user already had. */
    assert(input_settings_set_scope(&state, "UCUS98701"));
    assert(input_settings_is_title_scope(&state));
    assert(strcmp(input_settings_scope_label(&state), "UCUS98701") == 0);
    assert(input_settings_has_title_mapping(&state, "UCUS98701"));
    assert(state.file.title_count == 1);
    assert(state.profile.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_GUIDE);
    assert(state.file.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_GUIDE);

    /* Editing in the per-title scope changes only that disc. */
    NkBindingSource left_stick;
    left_stick.type = NK_BINDING_HOST_BUTTON;
    left_stick.index = NK_HOST_BUTTON_LEFT_STICK;
    assert(input_settings_assign_binding(&state, INPUT_CONTROL_BTN_CROSS, left_stick));
    input_settings_set_deadzone(&state, NK_PSP_AXIS_ANALOG_X, 5000);
    assert(state.profile.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_LEFT_STICK);
    assert(state.file.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_GUIDE);
    assert(state.file.global.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == NK_INPUT_DEFAULT_DEADZONE_INNER);

    /* The global scope still shows the global mapping, unchanged. */
    assert(input_settings_set_scope(&state, NULL));
    assert(!input_settings_is_title_scope(&state));
    assert(state.profile.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_GUIDE);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == NK_INPUT_DEFAULT_DEADZONE_INNER);

    /* Toggling flips between the two, which is what the library card and
     * Controller Settings do. Going back to the global mapping drops the disc's
     * own entry, so that disc's next launch uses the global profile. */
    assert(input_settings_toggle_scope(&state, "UCUS98701"));
    assert(input_settings_is_title_scope(&state));
    assert(state.profile.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_LEFT_STICK);
    assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 5000);
    assert(input_settings_toggle_scope(&state, "UCUS98701"));
    assert(!input_settings_is_title_scope(&state));
    assert(!input_settings_has_title_mapping(&state, "UCUS98701"));
    assert(state.file.title_count == 0);

    /* A disc ID that cannot name a file is refused and the scope is kept. */
    assert(!input_settings_set_scope(&state, "../escape"));
    assert(!input_settings_is_title_scope(&state));

    /* Saving writes both mappings: the global one the user edited before, and
     * the per-title one edited after. */
    assert(input_settings_set_scope(&state, "UCUS98701"));
    NkBindingSource rebind;
    rebind.type = NK_BINDING_HOST_BUTTON;
    rebind.index = NK_HOST_BUTTON_LEFT_STICK;
    assert(input_settings_assign_binding(&state, INPUT_CONTROL_BTN_CROSS, rebind));
    input_settings_set_deadzone(&state, NK_PSP_AXIS_ANALOG_X, 5000);
    assert(input_settings_save(&state, file_path) == NK_OK);
    assert(input_settings_save(&state, file_path) == NK_OK); /* atomic overwrite */

    InputSettingsState reloaded;
    assert(input_settings_load(&reloaded, file_path) == NK_OK);
    assert(reloaded.file.title_count == 1);
    assert(reloaded.file.global.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_GUIDE);
    assert(reloaded.file.global.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == NK_INPUT_DEFAULT_DEADZONE_INNER);
    char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
    const NkInputProfile *resolved =
        input_settings_resolve_for_disc(&reloaded, "UCUS98701", diag, sizeof(diag));
    assert(resolved != NULL);
    assert(resolved->psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_LEFT_STICK);
    assert(resolved->axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 5000);
    resolved = input_settings_resolve_for_disc(&reloaded, "ULUS10041", diag, sizeof(diag));
    assert(resolved == &reloaded.file.global);

    /* An interrupted write leaves the previous file intact: the temporary file
     * cannot be opened, so the rename never happens and the saved document on
     * disk is still the one that was there. */
    char blocker[64];
    snprintf(blocker, sizeof(blocker), "%s.tmp", file_path);
#if defined(_WIN32) || defined(_WIN64)
    assert(CreateDirectoryA(blocker, NULL) != 0);
#else
    assert(mkdir(blocker, 0777) == 0);
#endif
    assert(input_settings_set_scope(&state, "UCUS98701"));
    state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner = 1234;
    assert(input_settings_save(&state, file_path) != NK_OK);
    assert(state.has_save_diagnostic);
    assert(state.save_diagnostic[0] != '\0');
    InputSettingsState after_failed;
    assert(input_settings_load(&after_failed, file_path) == NK_OK);
    assert(after_failed.file.title[0].axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 5000);
#if defined(_WIN32) || defined(_WIN64)
    assert(RemoveDirectoryA(blocker) != 0);
#else
    assert(rmdir(blocker) == 0);
#endif
    assert(input_settings_save(&state, file_path) == NK_OK);
    assert(input_settings_load(&after_failed, file_path) == NK_OK);
    assert(after_failed.file.title[0].axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 1234);

    /* The file the launched runtime is handed carries the effective mapping for
     * that disc and nothing else. */
    char path[NK_MAX_PATH] = {0};
    assert(input_settings_write_disc_profile(&after_failed, "UCUS98701", path, sizeof(path),
                                             diag, sizeof(diag)) == NK_OK);
    assert(strstr(path, "UCUS98701") != NULL);
    assert(native_path_within(path, g_test_root));
    if (g_real_config_probe[0]) {
        assert(!native_path_within(path, g_real_config_probe));
    }
    assert_real_config_untouched();
    NkInputProfile handed;
    assert(nk_input_profile_load(&handed, path, diag, sizeof(diag)) == NK_OK);
    assert(handed.psp_buttons[NK_PSP_BTN_CROSS].primary.index == NK_HOST_BUTTON_LEFT_STICK);
    assert(handed.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner == 1234);

    /* An unsafe disc ID never produces a path at all. */
    char bad_path[NK_MAX_PATH];
    bad_path[0] = 'x';
    assert(input_settings_write_disc_profile(&after_failed, "../escape", bad_path, sizeof(bad_path),
                                             diag, sizeof(diag)) != NK_OK);
    assert(bad_path[0] == '\0');
    assert(strstr(diag, "disc ID") != NULL);

    /* A config directory that cannot hold the file is an error with a reason,
     * not a silently global launch. Point the per-user config location at a
     * regular file so creating the profile directory cannot succeed. */
    {
        const char *blocker_file = "build/test_config_blocker";
        FILE *bf = fopen(blocker_file, "wb");
        assert(bf != NULL);
        fputs("not a directory\n", bf);
        fclose(bf);
#if defined(_WIN32) || defined(_WIN64)
        const char *saved = getenv("LOCALAPPDATA");
        char saved_copy[1024];
        if (saved) snprintf(saved_copy, sizeof(saved_copy), "%s", saved);
        test_setenv("LOCALAPPDATA", blocker_file);
#else
        const char *saved = getenv("XDG_CONFIG_HOME");
        char saved_copy[1024];
        if (saved) snprintf(saved_copy, sizeof(saved_copy), "%s", saved);
        test_setenv("XDG_CONFIG_HOME", blocker_file);
#endif
        char blocked_path[NK_MAX_PATH];
        blocked_path[0] = 'x';
        assert(input_settings_write_disc_profile(&after_failed, "UCUS98701", blocked_path,
                                                 sizeof(blocked_path), diag, sizeof(diag)) != NK_OK);
        assert(blocked_path[0] == '\0');
        assert(diag[0] != '\0');
        if (saved) {
#if defined(_WIN32) || defined(_WIN64)
            test_setenv("LOCALAPPDATA", saved_copy);
#else
            test_setenv("XDG_CONFIG_HOME", saved_copy);
#endif
        } else {
#if defined(_WIN32) || defined(_WIN64)
            test_setenv("LOCALAPPDATA", NULL);
#else
            test_setenv("XDG_CONFIG_HOME", NULL);
#endif
        }
        remove(blocker_file);
        assert_config_root_isolated();
        assert_real_config_untouched();
    }

    /* The document the player writes is bounded: a full table refuses another
     * disc rather than dropping one. */
    for (int i = 0; after_failed.file.title_count < NK_INPUT_MAX_PER_TITLE; i++) {
        char disc_id[NK_MAX_DISC_ID_LEN];
        snprintf(disc_id, sizeof(disc_id), "FILL%05d", i);
        assert(input_settings_set_scope(&after_failed, disc_id));
    }
    assert(!input_settings_set_scope(&after_failed, "ONET00TOOMANY"));
    assert(!input_settings_is_title_scope(&after_failed) ||
           strcmp(after_failed.editing_disc_id, "ONET00TOOMANY") != 0);

    remove(path);
    remove(file_path);
    assert_real_config_untouched();
    printf("[INPUT_SETTINGS_TEST] Subtest 10 PASSED!\n");
}

/* -----------------------------------------------------------------------------
 * 11. Hostile Profile Files Through The Player Loader
 * -------------------------------------------------------------------------- */
static void test_hostile_profile_files(void) {
    printf("[INPUT_SETTINGS_TEST] Subtest 11: hostile files leave the player at defaults...\n");

    /* A file the user can edit is a file the user can break. Every shape here
     * has to leave the settings screen on the safe default mapping with the
     * diagnostic on screen, never with half a mapping the pad cannot see. */
    static const char *const kHostileFiles[] = {
        "",                                                                     /* empty */
        "   \n",                                                                /* blank */
        "{",                                                                    /* cut short */
        "{\"schema_version\": 2, \"device\": {\"guid\": \"g\"},",                  /* cut mid-object */
        "{\"schema_version\":2,\"device\":{\"guid\":\"g\"},"
        "\"calibration\":{\"trigger_threshold\":8192,"
        "\"analog_x\":{\"host_axis\":\"leftx\",\"deadzone_inner\":1000},"
        "\"analog_y\":{\"host_axis\":\"lefty\",\"deadzone_inner\":1000}},"
        "\"psp_bindings\":[{\"control\":\"cross\"}],\"navigation_bindings\":[]}", /* unbound control */
        "{\"schema_version\":2,\"device\":{\"guid\":\"g\"},"
        "\"calibration\":{\"trigger_threshold\":99999,"
        "\"analog_x\":{\"host_axis\":\"leftx\",\"deadzone_inner\":1000},"
        "\"analog_y\":{\"host_axis\":\"lefty\",\"deadzone_inner\":1000}},"
        "\"psp_bindings\":[],\"navigation_bindings\":[]}",                       /* out of range */
        "{\"schema_version\":2,\"device\":{\"guid\":\"g\xFF\"},"
        "\"calibration\":{\"trigger_threshold\":8192,"
        "\"analog_x\":{\"host_axis\":\"leftx\",\"deadzone_inner\":1000},"
        "\"analog_y\":{\"host_axis\":\"lefty\",\"deadzone_inner\":1000}},"
        "\"psp_bindings\":[],\"navigation_bindings\":[]}",                       /* invalid UTF-8 */
        "{\"schema_version\":2,\"device\":{\"guid\":\"g\"},"
        "\"calibration\":{\"trigger_threshold\":8192,"
        "\"analog_x\":{\"host_axis\":\"leftx\",\"deadzone_inner\":1000},"
        "\"analog_y\":{\"host_axis\":\"lefty\",\"deadzone_inner\":1000}},"
        "\"psp_bindings\":[],\"navigation_bindings\":[],"
        "\"per_title\":[{\"disc_id\":\"NUL\",\"profile\":{}}]}",                  /* reserved name */
        "{\"schema_version\":2,\"device\":{\"guid\":\"g\"},"
        "\"calibration\":{\"trigger_threshold\":8192,"
        "\"analog_x\":{\"host_axis\":\"leftx\",\"deadzone_inner\":1000},"
        "\"analog_y\":{\"host_axis\":\"lefty\",\"deadzone_inner\":1000}},"
        "\"psp_bindings\":[],\"navigation_bindings\":[],"
        "\"per_title\":[{\"disc_id\":\"../../escape\",\"profile\":{}}]}",         /* traversal */
        "{\"schema_version\":2,\"device\":{\"guid\":\"g\"},"
        "\"calibration\":{\"trigger_threshold\":8192,"
        "\"analog_x\":{\"host_axis\":\"leftx\",\"deadzone_inner\":1000},"
        "\"analog_y\":{\"host_axis\":\"lefty\",\"deadzone_inner\":1000}},"
        "\"psp_bindings\":[],\"navigation_bindings\":[]} TRAILING",               /* garbage */
    };
    static const char *const kRawNulFile =
        "{\"schema_version\":2,\"device\":{\"guid\":\"g\0h\"}}";
    size_t hostile_count = sizeof(kHostileFiles) / sizeof(kHostileFiles[0]);
    const char *path = "build/test_hostile_settings.json";

    assert(nk_platform_mkdir_p("build"));

    for (size_t i = 0; i < hostile_count; i++) {
        InputSettingsState state;
        FILE *f = fopen(path, "wb");
        assert(f != NULL);
        assert(fwrite(kHostileFiles[i], 1, strlen(kHostileFiles[i]), f) ==
               strlen(kHostileFiles[i]));
        assert(fclose(f) == 0);

        NkResult res = input_settings_load(&state, path);
        if (res == NK_OK || !state.has_load_diagnostic || state.load_diagnostic[0] == '\0') {
            printf("  hostile file %zu: expected a refusal with a diagnostic, got res=%d\n",
                   i, (int)res);
            assert(0 && "hostile profile file was not refused with a diagnostic");
        }
        assert(state.loaded_from_file);
        assert(state.profile.schema_version == NK_INPUT_PROFILE_SCHEMA_VERSION);
        assert(state.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner ==
               NK_INPUT_DEFAULT_DEADZONE_INNER);
        assert(state.profile.psp_buttons[INPUT_CONTROL_BTN_CROSS].primary.type !=
               NK_BINDING_NONE);
        assert(state.file.title_count == 0);
        assert(state.editing_disc_id[0] == '\0');
        assert(!input_settings_is_title_scope(&state));
        assert(!input_settings_has_conflicts(&state));
        /* A refused document leaves the player usable: a save from here writes
         * the defaults, and that file loads back. */
        if (i == 0 || i == 2) {
            assert(input_settings_save(&state, path) == NK_OK);
            assert(!state.has_save_diagnostic);
            InputSettingsState reloaded;
            assert(input_settings_load(&reloaded, path) == NK_OK);
            assert(!reloaded.has_load_diagnostic);
            assert(reloaded.file.title_count == 0);
        }
    }

    /* A NUL byte inside the file is data, not an end of file. */
    {
        InputSettingsState state;
        FILE *f = fopen(path, "wb");
        assert(f != NULL);
        assert(fwrite(kRawNulFile, 1, strlen(kRawNulFile) + 1, f) == strlen(kRawNulFile) + 1);
        assert(fclose(f) == 0);
        assert(input_settings_load(&state, path) != NK_OK);
        assert(state.has_load_diagnostic);
        assert(state.file.title_count == 0);
    }

    /* A disc ID the whitelist refuses never becomes a file name: the write is
     * refused before the profile directory is touched. */
    {
        static const char *const kUnusableIds[] = {
            "", "..", "../escape", "a/b", "NUL", "nul.json", "COM1", "LPT9", "AUX"
        };
        size_t n_ids = sizeof(kUnusableIds) / sizeof(kUnusableIds[0]);
        for (size_t i = 0; i < n_ids; i++) {
            InputSettingsState state;
            char out_path[NK_MAX_PATH];
            char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN];
            input_settings_init(&state);
            out_path[0] = 'X';
            diag[0] = '\0';
            assert(input_settings_write_disc_profile(&state, kUnusableIds[i], out_path,
                                                    sizeof(out_path), diag, sizeof(diag)) != NK_OK);
            assert(out_path[0] == '\0');
            assert(strstr(diag, "disc ID") != NULL);
        }
    }

    remove(path);
    printf("[INPUT_SETTINGS_TEST] Subtest 11 PASSED!\n");
}

int main(void) {
    /* Unbuffered: a failing case names itself on stdout, and the harness reads
     * that line even when the run aborts on the assert that follows. */
    setbuf(stdout, NULL);

    capture_real_config_probe();
    create_test_root("input-settings");
    isolate_user_data_roots();
    assert_config_root_isolated();
    assert_real_config_untouched();

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
    test_per_title_scope_and_atomic_save();
    test_hostile_profile_files();

    assert_real_config_untouched();

    printf("=================================================================\n");
    printf("ALL INPUT SETTINGS TESTS PASSED SUCCESSFULLY!\n");
    printf("=================================================================\n");
    return 0;
}
