/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#endif

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sleep-ms") == 0 && i + 1 < argc) {
            int ms = atoi(argv[i + 1]);
#if defined(_WIN32) || defined(_WIN64)
            Sleep((DWORD)ms);
#else
            struct timespec req;
            req.tv_sec = ms / 1000;
            req.tv_nsec = (long)(ms % 1000) * 1000000L;
            nanosleep(&req, NULL);
#endif
            return 0;
        }
        if (strcmp(argv[i], "--spawn-grandchild") == 0 && i + 2 < argc) {
            const char *grandchild_exe = argv[i + 1];
            const char *pid_file = argv[i + 2];
#if defined(_WIN32) || defined(_WIN64)
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "\"%s\" --sleep-ms 60000", grandchild_exe);
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            memset(&pi, 0, sizeof(pi));
            if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                FILE *f = fopen(pid_file, "w");
                if (f) {
                    fprintf(f, "%lu\n", pi.dwProcessId);
                    fclose(f);
                }
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            Sleep(60000);
            return 0;
#else
            pid_t pid = fork();
            if (pid == 0) {
                char *g_argv[] = { (char *)grandchild_exe, (char *)"--sleep-ms", (char *)"60000", NULL };
                execv(grandchild_exe, g_argv);
                _exit(127);
            }
            if (pid > 0) {
                FILE *f = fopen(pid_file, "w");
                if (f) {
                    fprintf(f, "%d\n", (int)pid);
                    fclose(f);
                }
            }
            struct timespec req = { 60, 0 };
            nanosleep(&req, NULL);
            return 0;
#endif
        }
    }

#if defined(_WIN32) || defined(_WIN64)
    SetConsoleOutputCP(CP_UTF8);
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
#endif
    FILE *out = stdout;
    const char *out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--output=", 9) == 0) {
            out_path = argv[i] + 9;
            break;
        }
    }

    if (out_path) {
        out = fopen(out_path, "wb");
    }

#if defined(_WIN32) || defined(_WIN64)
    fprintf(out, "ARGC=%d\n", wargc);
    for (int i = 0; i < wargc; i++) {
        char u8[1024] = {0};
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u8, sizeof(u8), NULL, NULL);
        fprintf(out, "ARG[%d]=%s\n", i, u8);
    }
    LocalFree(wargv);
#else
    fprintf(out, "ARGC=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        fprintf(out, "ARG[%d]=%s\n", i, argv[i]);
    }
#endif

    const char *test_env = getenv("TEST_ENV_KEY");
    if (test_env) {
        fprintf(out, "TEST_ENV_KEY=%s\n", test_env);
    }

    const char *sys_root = getenv("SystemRoot");
    if (!sys_root) {
        sys_root = getenv("SYSTEMROOT");
    }
    if (sys_root) {
        fprintf(out, "SYSTEMROOT_PRESENT=1\n");
    }

    if (out && out != stdout) {
        fclose(out);
    }

    return 0;
}
