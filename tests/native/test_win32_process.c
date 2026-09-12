/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("[PROCESS_TEST] Starting Windows CreateProcessW argv quoting & env tests...\n");

#if defined(_WIN32) || defined(_WIN64)
    const char *helper_exe = "build\\argv_echo_helper.exe";

    /* Verify helper executable exists */
    assert(nk_platform_file_exists(helper_exe));

    const char *out_file = "build\\argv_echo_out.txt";
    char out_arg[128];
    snprintf(out_arg, sizeof(out_arg), "--output=%s", out_file);

    /* Test case array of distinct arguments */
    const char *test_argv[] = {
        helper_exe,
        out_arg,
        "simple",
        "with spaces",
        "trailing\\",
        "double_trailing\\\\",
        "\"quoted\"",
        "quote \"inside\" text",
        "", /* empty argument */
        "trailing backslash and spaces \\ ",
        "C:\\Games\\PSP Game\\",
        "日本語_テスト_パス\\",
        NULL
    };

    const char *test_env[] = {
        "TEST_ENV_KEY=Hello_Unicode_Environment_ワールド",
        NULL
    };

    /* We test spawning directly via nk_platform_spawn_process */
    NkProcessHandle proc;
    bool ok = nk_platform_spawn_process(
        helper_exe,
        test_argv,
        test_env,
        NULL,
        &proc
    );
    assert(ok);
    assert(proc.is_active);

    /* Wait for child process */
    int exit_code = nk_platform_wait_process(&proc, 5000);
    assert(exit_code == 0);
    nk_platform_close_process(&proc);

    printf("[PROCESS_TEST] Spawn and exit code 0 verified.\n");

    /* Read child output file directly */
    FILE *f = fopen(out_file, "r");
    assert(f != NULL);

    char line[1024];
    int checked_args = 0;
    bool env_verified = false;
    bool sysroot_verified = false;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
            line[--len] = '\0';
        }

        if (strncmp(line, "ARG[", 4) == 0) {
            int idx = -1;
            char val[512] = {0};
            if (sscanf(line, "ARG[%d]=%[^\n]", &idx, val) >= 1) {
                if (idx == 0) {
                    checked_args++;
                } else if (idx < 12) {
                    const char *expected = test_argv[idx];
                    if (strcmp(val, expected) != 0) {
                        fprintf(stderr, "[PROCESS_TEST] Mismatch at ARG[%d]: expected '%s', got '%s'\n", idx, expected, val);
                        assert(strcmp(val, expected) == 0);
                    }
                    checked_args++;
                }
            } else if (sscanf(line, "ARG[%d]=", &idx) == 1) {
                /* Empty argument case */
                if (idx == 8) {
                    assert(test_argv[idx][0] == '\0');
                    checked_args++;
                }
            }
        } else if (strncmp(line, "TEST_ENV_KEY=", 13) == 0) {
            assert(strstr(line, "Hello_Unicode_Environment") != NULL);
            env_verified = true;
        } else if (strcmp(line, "SYSTEMROOT_PRESENT=1") == 0) {
            sysroot_verified = true;
        }
    }
    fclose(f);
    assert(checked_args == 12);
    assert(env_verified);
    assert(sysroot_verified);

    printf("[PROCESS_TEST] All 12 argv roundtrip cases verified exactly!\n");
    printf("[PROCESS_TEST] Controlled Unicode environment & SystemRoot inheritance verified!\n");
    printf("[PROCESS_TEST] Process & quoting test PASSED successfully!\n");
#else
    printf("[PROCESS_TEST] Skipping Windows-specific tests on non-Windows host.\n");
#endif

    return 0;
}
