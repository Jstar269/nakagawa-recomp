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

/* UTF-8 file primitives. A per-user profile directory holding non-ASCII
 * characters (an accented given name, a Japanese user name) is a legal path
 * that the narrow CRT reinterprets in the active ANSI code page, so the
 * file opens, removals and renames the player and the shared core make on
 * user-data and install paths go through these, or through an equivalent
 * wide-character open local to a translation unit that is built without this
 * platform layer (the decrypt tool, and the file-local copies in nk_iso.c,
 * nk_xb.c, nk_title_manifest.c, nk_library.c and nk_input_profile.c). Code that
 * opens such a path must not use the narrow fopen/remove/rename on Win32.
 * Win32 converts strictly (invalid UTF-8 fails rather than opening a path
 * where the bad bytes became U+FFFD); POSIX file names are already UTF-8 bytes,
 * so these are the plain system calls.
 *
 * nk_remove_utf8 removes a FILE only: it fails on a directory on every host
 * (DeleteFileW cannot remove one; POSIX uses unlink rather than remove). One
 * difference remains by design of the host: a read-only file is removed on
 * POSIX but fails on Win32 until the caller clears the attribute, so callers
 * must not depend on removing read-only files. */
FILE *nk_fopen_utf8(const char *path, const char *mode);
int nk_remove_utf8(const char *path);
int nk_rename_utf8(const char *from, const char *to);

/* Filesystem and path utilities (all paths in UTF-8) */
char nk_platform_path_separator(void);
bool nk_platform_file_exists(const char *path);
bool nk_platform_dir_exists(const char *path);
/* True only when the OS says the name does not exist: ENOENT or ENOTDIR on POSIX, and
 * ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND on Win32. Any other failure (access denied, an
 * unreadable parent) is not missing, so a caller about to delete a name must not take it for
 * gone. An entry of any kind, a directory included, is not missing. */
bool nk_platform_path_missing(const char *path);
/* Call fn(name, ctx) for each regular file directly inside dir (names in UTF-8,
 * unsorted; subdirectories are skipped). Stops early when fn returns false.
 * Returns false when dir cannot be opened. */
typedef bool (*NkDirFileFn)(const char *name, void *ctx);
bool nk_platform_list_files(const char *dir, NkDirFileFn fn, void *ctx);
int64_t nk_platform_get_file_size(const char *path);
bool nk_platform_mkdir_p(const char *dir_path);
/* Create a directory tree and private files for user-owned source material.
 * POSIX uses owner-only permissions and refuses a final symlink; Win32 uses
 * the user's application-data ACLs through the wide-character backend. */
bool nk_platform_mkdir_p_private(const char *dir_path);
FILE *nk_platform_fopen_private(const char *path, const char *mode);

/* Structured path routing */
bool nk_platform_get_path(NkPathType type, char *out_path, size_t max_len);

/* Point every per-user data lookup in this process (the library, title
 * manifests, packages, staged games, fonts) at `path` instead of the
 * platform default, the player's counterpart of nk_cli's --user-data-root.
 * NULL or "" restores the default. Returns false, leaving the previous
 * setting, when the path is too long. Call before the first data lookup. */
bool nk_platform_set_app_data_dir_override(const char *path);
/* Resolve the canonical per-user data directory without creating it. */
bool nk_platform_resolve_app_data_dir(char *out_path, size_t max_len);
/* Get/create the canonical per-user data directory. */
bool nk_platform_get_app_data_dir(char *out_path, size_t max_len);
/* Resolve the pre-canonical Windows profile location used by older builds.
 * Returns false on non-Windows hosts or when USERPROFILE is unavailable. */
bool nk_platform_get_legacy_app_data_dir(char *out_path, size_t max_len);
#if defined(_WIN32) || defined(_WIN64)
/* Unit-testable Windows base selection: LOCALAPPDATA, Known Folder, APPDATA.
 * USERPROFILE is deliberately not a candidate. */
bool nk_platform_resolve_windows_data_base(
    const char *known_local_app_data,
    const char *local_app_data,
    const char *roaming_app_data,
    char *out_base,
    size_t max_len
);
#endif

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
    void *job_handle;
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
