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
    /* Malformed UTF-8 must fail the spawn, never be dropped: a dropped working
     * directory launches in the parent's directory, and a dropped variable
     * inherits the parent's value. */
    const char bad_utf8[] = {(char)0xC3, (char)0x28, '\0'};
    char bad_env_item[32];
    snprintf(bad_env_item, sizeof(bad_env_item), "TEST_ENV_KEY=%s", bad_utf8);
    const char *bad_env[] = {bad_env_item, NULL};
    const char *quiet_argv[] = {helper_exe, out_arg, NULL};
    NkProcessHandle refused;
    assert(!nk_platform_spawn_process(helper_exe, quiet_argv, NULL, bad_utf8, &refused));
    assert(!refused.is_active);
    assert(!nk_platform_spawn_process(helper_exe, quiet_argv, bad_env, NULL, &refused));
    assert(!refused.is_active);
    printf("[PROCESS_TEST] Invalid UTF-8 working directory and environment refused.\n");

    /* Profile directories are read through the wide API: a LOCALAPPDATA outside
     * the active code page must round-trip as UTF-8, not as '?' bytes. */
    static WCHAR saved_lad[32768];
    DWORD saved_len = GetEnvironmentVariableW(L"LOCALAPPDATA", saved_lad, 32768);
    WCHAR wcwd[1024];
    DWORD cwd_len = GetCurrentDirectoryW(1024, wcwd);
    assert(cwd_len > 0 && cwd_len < 1024);
    WCHAR wbase[1200];
    /* Separators are passed as arguments so no literal reads as a UNC path. */
    const WCHAR *wsep = L"\\";
    _snwprintf(wbase, 1200, L"%ls%lsbuild%lsnk_env_\x65E5\x672C\x00E9", wcwd, wsep, wsep);
    wbase[1199] = L'\0';
    assert(_wputenv_s(L"LOCALAPPDATA", wbase) == 0);
    char expected[4096];
    assert(WideCharToMultiByte(CP_UTF8, 0, wbase, -1, expected, sizeof(expected), NULL, NULL) > 0);
    const char *sep = "\\";
    size_t exp_len = strlen(expected);
    snprintf(expected + exp_len, sizeof(expected) - exp_len, "%sNakagawa%scache", sep, sep);
    char got[4096];
    assert(nk_platform_get_path(NK_PATH_CACHE, got, sizeof(got)));
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "[PROCESS_TEST] profile path mismatch: expected '%s', got '%s'\n", expected, got);
        assert(strcmp(got, expected) == 0);
    }
    assert(nk_platform_dir_exists(got));
    assert(_wputenv_s(L"LOCALAPPDATA",
                      saved_len > 0 && saved_len < 32768 ? saved_lad : L"") == 0);
    printf("[PROCESS_TEST] Non-code-page profile directory resolved as UTF-8.\n");

    /* Process tree termination:
     * A child process spawns a grandchild process. When the child process is
     * terminated via nk_platform_terminate_process, the grandchild must also
     * be terminated (not left running). */
    printf("[PROCESS_TEST] Testing process tree termination...\n");
    const char *pid_file = "build\\test_grandchild.pid";
    remove(pid_file);

    const char *tree_argv[] = {
        helper_exe,
        "--spawn-grandchild",
        helper_exe,
        pid_file,
        NULL
    };

    NkProcessHandle tree_proc;
    bool tree_ok = nk_platform_spawn_process(
        helper_exe,
        tree_argv,
        NULL,
        NULL,
        &tree_proc
    );
    assert(tree_ok);
    assert(tree_proc.is_active);

    DWORD grandchild_pid = 0;
    for (int i = 0; i < 200; i++) {
        FILE *pf = fopen(pid_file, "r");
        if (pf) {
            if (fscanf(pf, "%lu", &grandchild_pid) == 1 && grandchild_pid > 0) {
                fclose(pf);
                break;
            }
            fclose(pf);
        }
        Sleep(20);
    }
    assert(grandchild_pid > 0);

    HANDLE hGrandchild = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, grandchild_pid);
    assert(hGrandchild != NULL);
    assert(WaitForSingleObject(hGrandchild, 0) == WAIT_TIMEOUT);

    nk_platform_terminate_process(&tree_proc);
    nk_platform_close_process(&tree_proc);

    DWORD wait_res = WaitForSingleObject(hGrandchild, 2000);
    if (wait_res != WAIT_OBJECT_0) {
        TerminateProcess(hGrandchild, 1);
        CloseHandle(hGrandchild);
        remove(pid_file);
        fprintf(stderr, "[PROCESS_TEST] Grandchild PID %lu survived child termination!\n", grandchild_pid);
        assert(wait_res == WAIT_OBJECT_0);
    }
    CloseHandle(hGrandchild);
    remove(pid_file);
    printf("[PROCESS_TEST] Process tree termination verified: grandchild terminated!\n");

    printf("[PROCESS_TEST] Process & quoting test PASSED successfully!\n");
#else
    printf("[PROCESS_TEST] Skipping Windows-specific tests on non-Windows host.\n");
#endif

    return 0;
}
