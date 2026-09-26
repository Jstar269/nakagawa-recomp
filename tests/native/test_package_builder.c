/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Strict C99 hides POSIX declarations used by the route and environment probes. */
#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "package_builder.h"
#include "nk_platform.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <direct.h>
#define nk_tb_chdir _chdir
#define nk_tb_getcwd _getcwd
#else
#include <time.h>
#include <unistd.h>
#define nk_tb_chdir chdir
#define nk_tb_getcwd getcwd
#endif

/* Set (value non-NULL) or clear (value NULL) a process environment variable
 * for the duration of a probe, restored by the caller. */
static void nk_tb_set_env(const char *key, const char *value) {
    size_t len = strlen(key) + (value ? strlen(value) : 0) + 2;
    char *pair = (char *)malloc(len);
    assert(pair != NULL);
    if (value) snprintf(pair, len, "%s=%s", key, value);
    else snprintf(pair, len, "%s=", key);
#if defined(_WIN32) || defined(_WIN64)
    /* _putenv may retain the pointer, so the string outlives this call. */
    _putenv(pair);
#else
    if (value) setenv(key, value, 1); /* setenv copies */
    else unsetenv(key);
    free(pair);
#endif
}

static void test_progress_line_parsing(void) {
    printf("[PACKAGE_BUILDER_TEST] Subtest 1: progress JSON parsing\n");
    PackageProgressEvent ev;

    /* 1. Normal valid progress lines */
    const char *line1 = "{\"stage\": \"preflight\", \"status\": \"START\", \"message\": \"Inspecting disc...\"}\n";
    assert(package_builder_parse_progress_line(line1, strlen(line1), &ev));
    assert(strcmp(ev.stage, "preflight") == 0);
    assert(strcmp(ev.status, "START") == 0);
    assert(strcmp(ev.message, "Inspecting disc...") == 0);
    assert(ev.stage_enum == PACKAGE_BUILD_STAGE_PREFLIGHT);
    assert(ev.status_enum == PACKAGE_PROGRESS_STATUS_START);

    const char *line2 = "{\"stage\": \"compile\", \"status\": \"RUNNING\", \"message\": \"Building native host object\"}";
    assert(package_builder_parse_progress_line(line2, strlen(line2), &ev));
    assert(ev.stage_enum == PACKAGE_BUILD_STAGE_COMPILE);
    assert(ev.status_enum == PACKAGE_PROGRESS_STATUS_RUNNING);

    const char *line3 = "{\"stage\": \"package\", \"status\": \"PASS\", \"message\": \"Runtime package cached\"}";
    assert(package_builder_parse_progress_line(line3, strlen(line3), &ev));
    assert(ev.stage_enum == PACKAGE_BUILD_STAGE_PACKAGE);
    assert(ev.status_enum == PACKAGE_PROGRESS_STATUS_PASS);

    const char *line_fail = "{\"stage\": \"preflight\", \"status\": \"FAIL\", \"message\": \"Encrypted executable (#295)\"}";
    assert(package_builder_parse_progress_line(line_fail, strlen(line_fail), &ev));
    assert(ev.stage_enum == PACKAGE_BUILD_STAGE_PREFLIGHT);
    assert(ev.status_enum == PACKAGE_PROGRESS_STATUS_FAIL);
    assert(strstr(ev.message, "#295") != NULL);

    /* 2. Malformed or invalid lines */
    assert(!package_builder_parse_progress_line(NULL, 0, &ev));
    assert(!package_builder_parse_progress_line("", 0, &ev));
    assert(!package_builder_parse_progress_line("not json at all", 15, &ev));
    assert(!package_builder_parse_progress_line("{\"foo\": \"bar\"}", 14, &ev));
    assert(!package_builder_parse_progress_line("{\"stage\": \"preflight\"}", 22, &ev));
    assert(!package_builder_parse_progress_line("{truncated", 10, &ev));
}

static void test_state_machine_transitions(void) {
    printf("[PACKAGE_BUILDER_TEST] Subtest 2: state machine transitions\n");
    PackageBuildSession session;
    package_builder_init_session(&session, "ULUS10041", "Street Supremacy");

    assert(strcmp(session.disc_id, "ULUS10041") == 0);
    assert(strcmp(session.title_name, "Street Supremacy") == 0);
    assert(session.current_stage == PACKAGE_BUILD_STAGE_IDLE);
    assert(!session.is_building);
    assert(!session.is_complete);
    assert(!session.is_failed);
    assert(!session.is_cancelled);

    /* Event: preflight START */
    PackageProgressEvent ev;
    const char *l1 = "{\"stage\": \"preflight\", \"status\": \"START\", \"message\": \"Checking preflight\"}";
    assert(package_builder_parse_progress_line(l1, strlen(l1), &ev));
    package_builder_apply_event(&session, &ev);
    assert(session.current_stage == PACKAGE_BUILD_STAGE_PREFLIGHT);
    assert(strcmp(session.current_stage_name, "preflight") == 0);
    assert(strcmp(session.current_message, "Checking preflight") == 0);
    assert(!session.is_complete);
    assert(!session.is_failed);

    /* Event: extract RUNNING */
    const char *l2 = "{\"stage\": \"extract\", \"status\": \"RUNNING\", \"message\": \"Extracting plain modules\"}";
    assert(package_builder_parse_progress_line(l2, strlen(l2), &ev));
    package_builder_apply_event(&session, &ev);
    assert(session.current_stage == PACKAGE_BUILD_STAGE_EXTRACT);

    /* Event: compile RUNNING */
    const char *l3 = "{\"stage\": \"compile\", \"status\": \"RUNNING\", \"message\": \"Compiling recompiled C\"}";
    assert(package_builder_parse_progress_line(l3, strlen(l3), &ev));
    package_builder_apply_event(&session, &ev);
    assert(session.current_stage == PACKAGE_BUILD_STAGE_COMPILE);

    /* Event: package PASS -> complete */
    const char *l4 = "{\"stage\": \"package\", \"status\": \"PASS\", \"message\": \"Package validation OK\"}";
    assert(package_builder_parse_progress_line(l4, strlen(l4), &ev));
    package_builder_apply_event(&session, &ev);
    assert(session.current_stage == PACKAGE_BUILD_STAGE_COMPLETE);
    assert(session.is_complete);
    assert(!session.is_failed);

    /* Test failure transition */
    PackageBuildSession fail_session;
    package_builder_init_session(&fail_session, "ULES00123", "Encrypted Title");
    const char *l_err = "{\"stage\": \"preflight\", \"status\": \"FAIL\", \"message\": \"Encrypted executable (#295). Automatic decryption is in the works.\"}";
    assert(package_builder_parse_progress_line(l_err, strlen(l_err), &ev));
    package_builder_apply_event(&fail_session, &ev);
    assert(fail_session.current_stage == PACKAGE_BUILD_STAGE_FAILED);
    assert(fail_session.is_failed);
    assert(!fail_session.is_complete);
    assert(strcmp(fail_session.failure_boundary, "Encrypted executable (#295). Automatic decryption is in the works.") == 0);
}

static void test_output_line_circular_buffer(void) {
    printf("[PACKAGE_BUILDER_TEST] Subtest 3: output circular buffer\n");
    PackageBuildSession session;
    package_builder_init_session(&session, "TEST00001", "Buffer Test");

    assert(session.output_line_count == 0);

    /* Add 3 lines */
    package_builder_add_output_line(&session, "line 1");
    package_builder_add_output_line(&session, "line 2");
    package_builder_add_output_line(&session, "line 3");
    assert(session.output_line_count == 3);
    assert(strcmp(package_builder_get_output_line(&session, 0), "line 1") == 0);
    assert(strcmp(package_builder_get_output_line(&session, 1), "line 2") == 0);
    assert(strcmp(package_builder_get_output_line(&session, 2), "line 3") == 0);

    /* Fill buffer to capacity (8 lines) */
    for (int i = 4; i <= 8; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "line %d", i);
        package_builder_add_output_line(&session, buf);
    }
    assert(session.output_line_count == PACKAGE_BUILD_MAX_OUTPUT_LINES);
    assert(strcmp(package_builder_get_output_line(&session, 0), "line 1") == 0);
    assert(strcmp(package_builder_get_output_line(&session, 7), "line 8") == 0);

    /* Push 2 more lines -> wraps, dropping lines 1 and 2 */
    package_builder_add_output_line(&session, "line 9");
    package_builder_add_output_line(&session, "line 10");
    assert(session.output_line_count == PACKAGE_BUILD_MAX_OUTPUT_LINES);
    assert(strcmp(package_builder_get_output_line(&session, 0), "line 3") == 0);
    assert(strcmp(package_builder_get_output_line(&session, 6), "line 9") == 0);
    assert(strcmp(package_builder_get_output_line(&session, 7), "line 10") == 0);

    /* Out of bounds returns empty string */
    assert(strcmp(package_builder_get_output_line(&session, -1), "") == 0);
    assert(strcmp(package_builder_get_output_line(&session, 8), "") == 0);
}

static void test_python_and_cli_discovery(void) {
    printf("[PACKAGE_BUILDER_TEST] Subtest 4: toolchain discovery\n");
    char python_path[NK_MAX_PATH];
    bool found_py = package_builder_find_python(python_path, sizeof(python_path));
    assert(found_py);
    assert(strlen(python_path) > 0);
    printf("   Found python at: %s\n", python_path);

    char cli_path[NK_MAX_PATH];
    bool found_cli = package_builder_find_cli(".", cli_path, sizeof(cli_path));
    assert(found_cli);
    assert(strlen(cli_path) > 0);
    printf("   Found CLI at: %s\n", cli_path);
}

static void test_session_cancellation(void) {
    printf("[PACKAGE_BUILDER_TEST] Subtest 5: session cancellation\n");
    PackageBuildSession session;
    package_builder_init_session(&session, "ULUS10041", "Cancel Test");
    session.is_building = true;

    package_builder_cancel(&session);
    assert(!session.is_building);
    assert(session.is_cancelled);
    assert(session.current_stage == PACKAGE_BUILD_STAGE_CANCELLED);
    assert(strcmp(session.current_stage_name, "cancelled") == 0);
}

/* Bounded wait for a real package build (#509). The child's own exit status is
   the answer; this only caps a build that never ends. */
#define ROUTE_BUILD_TIMEOUT_MS 900000
#define ROUTE_POLL_INTERVAL_MS 50

static uint64_t route_now_ms(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (uint64_t)GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000);
#endif
}

static void route_pause_ms(int ms) {
#if defined(_WIN32) || defined(_WIN64)
    Sleep((DWORD)ms);
#else
    struct timespec request;
    request.tv_sec = ms / 1000;
    request.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&request, NULL);
#endif
}

/* Drive the BUILD PACKAGE action the way the player does: discover the
 * interpreter and the CLI through the player's own lookups, start the build
 * through the player's own session, and report the state the UI would show.
 * Nothing here is stubbed -- nk_cli.py build-package runs the production
 * analysis, codegen and compile. */
static int run_build_package_route(const char *install_root,
                                   const char *user_data_root,
                                   const char *disc_id,
                                   const char *log_dir) {
    char python_path[NK_MAX_PATH];
    if (!package_builder_find_python(python_path, sizeof(python_path))) {
        printf("PACKAGE_BUILD_ROUTE status=FAIL reason=python-not-found\n");
        return 2;
    }
    char cli_path[NK_MAX_PATH];
    if (!package_builder_find_cli(install_root, cli_path, sizeof(cli_path))) {
        printf("PACKAGE_BUILD_ROUTE status=FAIL reason=cli-not-found\n");
        return 2;
    }
    printf("PACKAGE_BUILD_ROUTE python=%s\n", python_path);
    printf("PACKAGE_BUILD_ROUTE cli=%s\n", cli_path);

    PackageBuildSession session;
    package_builder_init_session(&session, disc_id, "Player Package Route");
    if (package_builder_start(&session, python_path, cli_path, user_data_root, log_dir) != NK_OK) {
        printf("PACKAGE_BUILD_ROUTE status=FAIL reason=spawn-failed\n");
        printf("PACKAGE_BUILD_ROUTE boundary=%s\n", session.failure_boundary);
        return 2;
    }

    uint64_t deadline = route_now_ms() + ROUTE_BUILD_TIMEOUT_MS;
    while (session.is_building && route_now_ms() < deadline) {
        package_builder_poll(&session, route_now_ms());
        if (session.is_building) route_pause_ms(ROUTE_POLL_INTERVAL_MS);
    }
    if (session.is_building) {
        package_builder_cancel(&session);
        printf("PACKAGE_BUILD_ROUTE status=FAIL reason=timeout boundary=package build did not finish in %d ms\n",
               ROUTE_BUILD_TIMEOUT_MS);
        return 3;
    }

    for (int i = 0; i < session.output_line_count; i++) {
        printf("PACKAGE_BUILD_OUTPUT %s\n", package_builder_get_output_line(&session, i));
    }
    printf("PACKAGE_BUILD_ROUTE stage=%s complete=%d failed=%d exit=%d\n",
           session.current_stage_name, session.is_complete ? 1 : 0,
           session.is_failed ? 1 : 0, session.exit_code);
    if (session.failure_boundary[0]) {
        printf("PACKAGE_BUILD_ROUTE boundary=%s\n", session.failure_boundary);
    }
    printf("PACKAGE_BUILD_ROUTE log=%s\n", session.log_file_path);
    printf("PACKAGE_BUILD_ROUTE progress=%s\n", session.progress_file_path);

    if (session.is_failed) {
        printf("PACKAGE_BUILD_ROUTE status=FAIL\n");
        return 1;
    }
    if (!session.is_complete) {
        printf("PACKAGE_BUILD_ROUTE status=FAIL reason=incomplete\n");
        return 1;
    }
    printf("PACKAGE_BUILD_ROUTE status=PASS\n");
    return 0;
}

static void test_cli_search_order_and_guidance(void) {
    printf("[PACKAGE_BUILDER_TEST] Subtest 6: CLI search order and not-found guidance\n");
    char cands[PACKAGE_BUILDER_CLI_MAX_CANDIDATES][NK_MAX_PATH];

    /* Documented order: override, exe dir, exe parent, release layout, cwd, cwd parent. */
    int n = package_builder_cli_candidate_paths("C:/rel/bin", "D:/ovr", cands,
                                                PACKAGE_BUILDER_CLI_MAX_CANDIDATES);
    assert(n == 6);
    assert(strcmp(cands[0], "D:/ovr/tools/nk_cli.py") == 0);
    assert(strcmp(cands[1], "C:/rel/bin/tools/nk_cli.py") == 0);
    assert(strcmp(cands[2], "C:/rel/bin/../tools/nk_cli.py") == 0);
    assert(strcmp(cands[3], "C:/rel/bin/../source/tools/nk_cli.py") == 0);
    assert(strcmp(cands[4], "tools/nk_cli.py") == 0);
    assert(strcmp(cands[5], "../tools/nk_cli.py") == 0);

    /* No override: the executable folder leads. */
    n = package_builder_cli_candidate_paths("C:/rel/bin", NULL, cands,
                                            PACKAGE_BUILDER_CLI_MAX_CANDIDATES);
    assert(n == 5);
    assert(strcmp(cands[0], "C:/rel/bin/tools/nk_cli.py") == 0);

    /* Trailing separators never double up. */
    n = package_builder_cli_candidate_paths("C:/rel/bin/", "D:/ovr/", cands,
                                            PACKAGE_BUILDER_CLI_MAX_CANDIDATES);
    assert(n == 6);
    assert(strcmp(cands[0], "D:/ovr/tools/nk_cli.py") == 0);
    assert(strcmp(cands[1], "C:/rel/bin/tools/nk_cli.py") == 0);

    /* Unknown executable folder: the working-directory pair still applies. */
    n = package_builder_cli_candidate_paths("", "", cands,
                                            PACKAGE_BUILDER_CLI_MAX_CANDIDATES);
    assert(n == 2);
    assert(strcmp(cands[0], "tools/nk_cli.py") == 0);
    assert(strcmp(cands[1], "../tools/nk_cli.py") == 0);

    /* The not-found card names every searched location and every fix. */
    char msg[512];
    package_builder_describe_cli_not_found("C:/rel/bin", msg, sizeof(msg));
    assert(strstr(msg, "C:/rel/bin/tools") != NULL);
    assert(strstr(msg, "C:/rel/bin/../source/tools") != NULL);
    assert(strstr(msg, "NK_INSTALL_ROOT") != NULL);
    assert(strstr(msg, "source checkout") != NULL);

    /* End to end: isolated cwd, no override -> not found; NK_INSTALL_ROOT -> found. */
    char saved_cwd[NK_MAX_PATH];
    char probe_dir[NK_MAX_PATH + 64];
    assert(nk_tb_getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
    assert(nk_platform_get_path(NK_PATH_CACHE, probe_dir, sizeof(probe_dir)));
    size_t plen = strlen(probe_dir);
    snprintf(probe_dir + plen, sizeof(probe_dir) - plen, "%ccli_probe",
             nk_platform_path_separator());
    assert(nk_platform_mkdir_p(probe_dir));

    /* Copy the prior value out: clearing or resetting invalidates getenv's pointer. */
    char prior_override[1024];
    prior_override[0] = '\0';
    const char *live_override = getenv("NK_INSTALL_ROOT");
    if (live_override && live_override[0]) {
        snprintf(prior_override, sizeof(prior_override), "%s", live_override);
    }
    nk_tb_set_env("NK_INSTALL_ROOT", NULL);
    assert(nk_tb_chdir(probe_dir) == 0);

    char found[NK_MAX_PATH];
    assert(!package_builder_find_cli(probe_dir, found, sizeof(found)));

    nk_tb_set_env("NK_INSTALL_ROOT", saved_cwd);
    assert(package_builder_find_cli(probe_dir, found, sizeof(found)));
    assert(strstr(found, "nk_cli.py") != NULL);

    assert(nk_tb_chdir(saved_cwd) == 0);
    if (prior_override[0]) {
        nk_tb_set_env("NK_INSTALL_ROOT", prior_override);
    } else {
        nk_tb_set_env("NK_INSTALL_ROOT", NULL);
    }
}

static void test_toolchain_preflight(void) {
    printf("[PACKAGE_BUILDER_TEST] Subtest 7: toolchain preflight\n");
    char tool[32];
    char msg[512];

    assert(!package_builder_toolchain_missing(true, true, true,
                                              tool, sizeof(tool), msg, sizeof(msg)));
    assert(tool[0] == '\0');
    assert(msg[0] == '\0');

    assert(package_builder_toolchain_missing(true, false, true,
                                             tool, sizeof(tool), msg, sizeof(msg)));
    assert(strcmp(tool, "gcc") == 0);
    assert(strstr(msg, "BUILD_TOOLCHAIN_MISSING") != NULL);
    assert(strstr(msg, "in the works (#324)") != NULL);
    assert(strstr(msg, "gcc") != NULL);
    assert(strstr(msg, "PATH") != NULL);

    assert(package_builder_toolchain_missing(true, true, false,
                                             tool, sizeof(tool), msg, sizeof(msg)));
    assert(strcmp(tool, "mingw32-make") == 0);
    assert(strstr(msg, "mingw32-make") != NULL);

    assert(package_builder_toolchain_missing(false, true, true,
                                             tool, sizeof(tool), msg, sizeof(msg)));
    assert(strcmp(tool, "python") == 0);
    assert(strstr(msg, "python") != NULL);

    /* With several absent, the compiler is named first. */
    assert(package_builder_toolchain_missing(true, false, false,
                                             tool, sizeof(tool), msg, sizeof(msg)));
    assert(strcmp(tool, "gcc") == 0);

    /* PATH lookup refuses a name that cannot exist anywhere. */
    char found[NK_MAX_PATH];
    assert(!package_builder_find_tool("nk_no_such_tool_zz9", found, sizeof(found)));
}

int main(int argc, char *argv[]) {
    if (argc == 3 && strcmp(argv[1], "--find-cli") == 0) {
        char cli_path[NK_MAX_PATH];
        if (!package_builder_find_cli(argv[2], cli_path, sizeof(cli_path))) {
            puts("PACKAGE_BUILDER_CLI status=FAIL reason=not-found");
            return 1;
        }
        printf("PACKAGE_BUILDER_CLI status=PASS path=%s\n", cli_path);
        return 0;
    }
    if (argc == 6 && strcmp(argv[1], "--build-package") == 0) {
        return run_build_package_route(argv[2], argv[3], argv[4], argv[5]);
    }
    test_progress_line_parsing();
    test_state_machine_transitions();
    test_output_line_circular_buffer();
    test_python_and_cli_discovery();
    test_session_cancellation();
    test_cli_search_order_and_guidance();
    test_toolchain_preflight();

    printf("ALL PACKAGE BUILDER TESTS PASSED\n");
    return 0;
}
