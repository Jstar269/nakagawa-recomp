/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#if defined(_WIN32) || defined(_WIN64)

#include "nk_platform.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int nk_fseek64(FILE *f, int64_t offset, int whence) {
    return _fseeki64(f, offset, whence);
}

int64_t nk_ftell64(FILE *f) {
    return _ftelli64(f);
}

char nk_platform_path_separator(void) {
    return '\\';
}

/* Bounded UTF-8 -> UTF-16 conversion */
static bool utf8_to_wide(const char *utf8, WCHAR *out_wide, size_t max_wide_chars) {
    if (!utf8 || !out_wide || max_wide_chars == 0) return false;
    int res = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out_wide, (int)max_wide_chars);
    if (res <= 0) {
        out_wide[0] = L'\0';
        return false;
    }
    return true;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static bool wide_to_utf8(const WCHAR *wide, char *out_utf8, size_t max_utf8_bytes) {
    if (!wide || !out_utf8 || max_utf8_bytes == 0) return false;
    int res = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out_utf8, (int)max_utf8_bytes, NULL, NULL);
    if (res <= 0) {
        out_utf8[0] = '\0';
        return false;
    }
    return true;
}

bool nk_platform_file_exists(const char *path) {
    if (!path || !*path) return false;
    WCHAR wpath[32768];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(WCHAR))) return false;
    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool nk_platform_dir_exists(const char *path) {
    if (!path || !*path) return false;
    WCHAR wpath[32768];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(WCHAR))) return false;
    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int64_t nk_platform_get_file_size(const char *path) {
    if (!path) return -1;
    WCHAR wpath[32768];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(WCHAR))) return -1;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad)) {
        return -1;
    }
    LARGE_INTEGER size;
    size.LowPart = fad.nFileSizeLow;
    size.HighPart = (LONG)fad.nFileSizeHigh;
    return (int64_t)size.QuadPart;
}

bool nk_platform_mkdir_p(const char *dir_path) {
    if (!dir_path || !*dir_path) return false;
    WCHAR wpath[32768];
    if (!utf8_to_wide(dir_path, wpath, sizeof(wpath) / sizeof(WCHAR))) return false;

    for (WCHAR *p = wpath + 1; *p; p++) {
        if (*p == L'/' || *p == L'\\') {
            WCHAR orig = *p;
            *p = L'\0';
            size_t sublen = wcslen(wpath);
            if (sublen > 2 && wpath[sublen - 1] != L':') {
                CreateDirectoryW(wpath, NULL);
            }
            *p = orig;
        }
    }
    CreateDirectoryW(wpath, NULL);
    DWORD attrs = GetFileAttributesW(wpath);
    return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
}

bool nk_platform_get_path(NkPathType type, char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base) {
        base = getenv("APPDATA");
    }
    if (!base || !*base) {
        base = getenv("USERPROFILE");
    }
    if (!base || !*base) return false;

    const char *subdir = "data";
    switch (type) {
        case NK_PATH_CONFIG: subdir = "config"; break;
        case NK_PATH_DATA:   subdir = "data"; break;
        case NK_PATH_CACHE:  subdir = "cache"; break;
        case NK_PATH_LOGS:   subdir = "logs"; break;
        case NK_PATH_SAVES:  subdir = "saves"; break;
        default:             subdir = "data"; break;
    }

    int written = snprintf(out_path, max_len, "%s\\Nakagawa\\%s", base, subdir);
    if (written < 0 || (size_t)written >= max_len) return false;

    nk_platform_mkdir_p(out_path);
    return true;
}

bool nk_platform_get_app_data_dir(char *out_path, size_t max_len) {
    return nk_platform_get_path(NK_PATH_DATA, out_path, max_len);
}

/* Helper to escape arguments for Windows command line */
static void append_escaped_arg(char *buf, size_t buf_len, const char *arg) {
    size_t cur_len = strlen(buf);
    if (cur_len + 3 >= buf_len) return;

    bool needs_quotes = false;
    if (strchr(arg, ' ') || strchr(arg, '\t') || strchr(arg, '\"') || *arg == '\0') {
        needs_quotes = true;
    }

    if (!needs_quotes) {
        snprintf(buf + cur_len, buf_len - cur_len, "%s%s", cur_len > 0 ? " " : "", arg);
        return;
    }

    if (cur_len > 0 && cur_len + 1 < buf_len) {
        buf[cur_len++] = ' ';
        buf[cur_len] = '\0';
    }

    if (cur_len + 1 < buf_len) buf[cur_len++] = '\"';

    for (const char *p = arg; *p && cur_len + 2 < buf_len; p++) {
        if (*p == '\"') {
            buf[cur_len++] = '\\';
        }
        buf[cur_len++] = *p;
    }

    if (cur_len + 1 < buf_len) buf[cur_len++] = '\"';
    buf[cur_len] = '\0';
}

bool nk_platform_spawn_process(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    NkProcessHandle *out_process
) {
    if (!executable_path || !out_process) return false;
    memset(out_process, 0, sizeof(*out_process));

    /* Convert executable path to wide */
    WCHAR wexec[32768];
    if (!utf8_to_wide(executable_path, wexec, sizeof(wexec) / sizeof(WCHAR))) {
        return false;
    }

    /* Build command line string */
    char cmd_line[32768];
    cmd_line[0] = '\0';

    if (argv && argv[0]) {
        for (int i = 0; argv[i]; i++) {
            append_escaped_arg(cmd_line, sizeof(cmd_line), argv[i]);
        }
    } else {
        append_escaped_arg(cmd_line, sizeof(cmd_line), executable_path);
    }

    WCHAR wcmd[32768];
    if (!utf8_to_wide(cmd_line, wcmd, sizeof(wcmd) / sizeof(WCHAR))) {
        return false;
    }

    /* Working directory to wide */
    WCHAR wwd_buf[32768];
    WCHAR *wwd = NULL;
    if (working_directory && *working_directory) {
        if (utf8_to_wide(working_directory, wwd_buf, sizeof(wwd_buf) / sizeof(WCHAR))) {
            wwd = wwd_buf;
        }
    }

    /* Build wide environment block if provided */
    WCHAR *wenv_block = NULL;
    DWORD creation_flags = 0;
    if (envp && envp[0]) {
        size_t total_wchars = 0;
        for (int i = 0; envp[i]; i++) {
            int needed = MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, NULL, 0);
            if (needed > 0) total_wchars += (size_t)needed;
        }
        total_wchars += 1; /* trailing double null */

        wenv_block = (WCHAR *)malloc(total_wchars * sizeof(WCHAR));
        if (wenv_block) {
            WCHAR *dest = wenv_block;
            for (int i = 0; envp[i]; i++) {
                int written = MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, dest, (int)(total_wchars - (dest - wenv_block)));
                if (written > 0) {
                    dest += written;
                }
            }
            *dest = L'\0';
            creation_flags |= CREATE_UNICODE_ENVIRONMENT;
        }
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    BOOL success = CreateProcessW(
        wexec,
        wcmd,
        NULL,
        NULL,
        FALSE,
        creation_flags,
        wenv_block,
        wwd,
        &si,
        &pi
    );

    if (wenv_block) {
        free(wenv_block);
    }

    if (!success) {
        return false;
    }

    CloseHandle(pi.hThread);
    out_process->native_handle = (void *)pi.hProcess;
    out_process->process_id = (int)pi.dwProcessId;
    out_process->is_active = true;
    return true;
}

bool nk_platform_is_process_running(NkProcessHandle *process) {
    if (!process || !process->native_handle || !process->is_active) return false;
    DWORD res = WaitForSingleObject((HANDLE)process->native_handle, 0);
    if (res == WAIT_TIMEOUT) {
        return true;
    }
    process->is_active = false;
    return false;
}

int nk_platform_wait_process(NkProcessHandle *process, int timeout_ms) {
    if (!process || !process->native_handle) return -1;
    DWORD wait_dur = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD res = WaitForSingleObject((HANDLE)process->native_handle, wait_dur);
    if (res == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess((HANDLE)process->native_handle, &exit_code);
        process->is_active = false;
        return (int)exit_code;
    }
    return -1;
}

void nk_platform_terminate_process(NkProcessHandle *process) {
    if (!process || !process->native_handle) return;
    TerminateProcess((HANDLE)process->native_handle, 1);
    process->is_active = false;
}

void nk_platform_close_process(NkProcessHandle *process) {
    if (!process) return;
    if (process->native_handle) {
        CloseHandle((HANDLE)process->native_handle);
        process->native_handle = NULL;
    }
    process->process_id = 0;
    process->is_active = false;
}

#endif /* _WIN32 */
