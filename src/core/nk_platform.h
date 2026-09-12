/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_PLATFORM_H
#define NK_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standardized structured data directory categories */
typedef enum {
    NK_PATH_CONFIG,  /* User preferences & settings */
    NK_PATH_DATA,    /* Game library, manifests, installed assets */
    NK_PATH_CACHE,   /* Recompilation cache, shaders */
    NK_PATH_LOGS,    /* Runtime and application logs */
    NK_PATH_SAVES    /* PSP Memory Stick save data (ms0/PSP/SAVEDATA) */
} NkPathType;

/* 64-bit file seek and tell portable across Win32, Linux, and macOS */
int nk_fseek64(FILE *f, int64_t offset, int whence);
int64_t nk_ftell64(FILE *f);

/* Filesystem and path utilities (all paths in UTF-8) */
char nk_platform_path_separator(void);
bool nk_platform_file_exists(const char *path);
bool nk_platform_dir_exists(const char *path);
int64_t nk_platform_get_file_size(const char *path);
bool nk_platform_mkdir_p(const char *dir_path);

/* Structured path routing */
bool nk_platform_get_path(NkPathType type, char *out_path, size_t max_len);

/* Get canonical user data directory for Nakagawa (APPDATA on Win32, XDG on Linux, AppSupport on macOS) */
bool nk_platform_get_app_data_dir(char *out_path, size_t max_len);

/* Resolve `path` to an absolute path in `out_path`.
 *
 * The runtime refuses a relative SR_DATAROOT ("configured but is not a valid
 * absolute path") and then declines to build an index at all, so a launch
 * assembled from a relative working directory silently lost its data root.
 * Returns false and leaves `out_path` untouched when the path cannot be
 * resolved. Win32 uses GetFullPathNameA (which does not require the path to
 * exist); POSIX uses realpath (which does), so callers should resolve paths
 * they have already confirmed. */
bool nk_platform_absolute_path(const char *path, char *out_path, size_t max_len);

/* Process handle abstraction */
typedef struct {
    void *native_handle;
    int process_id;
    bool is_active;
    /* A POSIX child can only be reaped once. nk_platform_is_process_running
       reaps it to learn that it exited, which would otherwise throw the exit
       status away and leave nk_platform_wait_process nothing to report. The
       status is cached here instead. Win32 keeps the handle open and reads the
       code on demand, so it never sets these. */
    bool has_cached_exit;
    int cached_exit_code;
} NkProcessHandle;

/* Spawn a child process with specified arguments, environment variables, and working directory.
 * argv: NULL-terminated array of arguments (argv[0] is program path)
 * envp: NULL-terminated array of "KEY=VAL" strings, or NULL to inherit current environment
 * working_directory: working directory path, or NULL to inherit current
 */
bool nk_platform_spawn_process(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    NkProcessHandle *out_process
);

bool nk_platform_is_process_running(NkProcessHandle *process);
int nk_platform_wait_process(NkProcessHandle *process, int timeout_ms);
void nk_platform_terminate_process(NkProcessHandle *process);
void nk_platform_close_process(NkProcessHandle *process);

#ifdef __cplusplus
}
#endif

#endif /* NK_PLATFORM_H */
