/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

int main(int argc, char *argv[]) {
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
