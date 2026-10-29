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
 * platform layer (the decrypt tool and the NPUMDIMG reader nk_psp_npumdimg.c it
 * links, and the file-local copies in nk_iso.c, nk_xb.c, nk_title_manifest.c,
 * nk_library.c and nk_input_profile.c). Code that
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
/* The bytes of every regular file under path (path itself when it is a file),
 * without following a symlink, junction or other reparse point: one counts as
 * nothing. A top-level child of path named skip_child (NULL for none) is left
 * out. A missing path is 0 bytes and true; false means a part could not be read. */
bool nk_platform_tree_size(const char *path, const char *skip_child, uint64_t *out_bytes);
/* Delete path and everything under it, without following a symlink, junction or
 * other reparse point: the link itself is removed, never its target. A
 * top-level child named skip_child (NULL for none) is kept, and then so is
 * path. A missing path is already gone: true. */
bool nk_platform_remove_tree(const char *path, const char *skip_child);
int64_t nk_platform_get_file_size(const char *path);
bool nk_platform_mkdir_p(const char *dir_path);
/* Create a directory tree and private files for user-owned source material.
 * POSIX uses owner-only permissions and refuses a final symlink; Win32 uses
 * the user's application-data ACLs through the wide-character backend. */
bool nk_platform_mkdir_p_private(const char *dir_path);
FILE *nk_platform_fopen_private(const char *path, const char *mode);

/* Structured path routing */
bool nk_platform_get_path(NkPathType type, char *out_path, size_t max_len);

/* Point every per-user folder of this process at `path` instead of the
 * platform default, the player's counterpart of nk_cli's --user-data-root:
 * NK_PATH_DATA (the library, title manifests, packages, staged games, fonts)
 * is `path` itself, and NK_PATH_CONFIG, NK_PATH_CACHE, NK_PATH_LOGS and
 * NK_PATH_SAVES are its config, cache, logs and saves folders, so nothing of
 * the default profile is read or written. NULL or "" restores the default.
 * Returns false, leaving the previous setting, when the path is too long.
 * Call before the first lookup. */
bool nk_platform_set_app_data_dir_override(const char *path);
/* The host's local time zone: its standard offset from UTC in minutes (east of UTC positive)
 * and whether daylight saving is in effect now. False when the host does not say. */
bool nk_platform_local_time_zone(int *standard_minutes, bool *daylight_now);
/* Resolve the canonical per-user data directory without creating it. */
bool nk_platform_resolve_app_data_dir(char *out_path, size_t max_len);
/* nk_platform_get_path's folder for `type`, without creating it. */
bool nk_platform_resolve_path(NkPathType type, char *out_path, size_t max_len);
/* This program's own executable, as UTF-8 (GetModuleFileNameW on Windows,
 * /proc/self/exe on Linux). False where the platform does not say. */
bool nk_platform_get_executable_path(char *out_path, size_t max_len);
/* Get/create the canonical per-user data directory. */
bool nk_platform_get_app_data_dir(char *out_path, size_t max_len);
/* Resolve the pre-canonical Windows profile location used by older builds.
 * Returns false on non-Windows hosts or when USERPROFILE is unavailable. */
bool nk_platform_get_legacy_app_data_dir(char *out_path, size_t max_len);

/* The user's Desktop folder, where a first run's file pickers open. Windows:
 * FOLDERID_Desktop, which follows a Desktop the user or a sync tool moved.
 * POSIX: the Desktop folder in the home folder when it exists, else the home
 * folder itself. False when there is none. */
bool nk_platform_get_desktop_dir(char *out_path, size_t max_len);
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

/* Whether `path` is longer than the file APIs this process uses accept, so a
 * create or open of it fails for its length alone. On Win32 that is MAX_PATH
 * (260 with the terminator; 248 for a new directory) unless the process has
 * long paths enabled; on POSIX, PATH_MAX. A caller asks only after the create
 * or open failed, to tell a too-long path apart from a damaged input. */
bool nk_platform_path_exceeds_limit(const char *path, bool is_directory);

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
    /* Why the last nk_platform_spawn_process into this handle failed, as the
       OS said it: GetLastError() after CreateProcessW on Win32, errno on POSIX.
       0 when the spawn succeeded or failed before the OS was asked. */
    int spawn_error;
} NkProcessHandle;

/* What the OS error a refused process start carries says about the refusal.
   Only Win32 reports these; every POSIX error reads as NK_SPAWN_BLOCK_NONE. */
typedef enum {
    NK_SPAWN_BLOCK_NONE = 0,
    /* ERROR_ACCESS_DENIED. Microsoft Defender's attack surface reduction rules
       refuse a program this way, as do file permissions. */
    NK_SPAWN_BLOCK_ACCESS_DENIED,
    /* ERROR_VIRUS_INFECTED or ERROR_VIRUS_DELETED: the antivirus found a threat. */
    NK_SPAWN_BLOCK_THREAT,
    /* ERROR_ACCESS_DISABLED_BY_POLICY, ERROR_ACCESS_DISABLED_NO_SAFER_UI_BY_POLICY or
       ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION: a software restriction, AppLocker or
       App Control policy. */
    NK_SPAWN_BLOCK_POLICY
} NkSpawnBlock;

NkSpawnBlock nk_platform_spawn_block(int os_error);

/* The OS's own one-line text for an error code (FormatMessageW on Win32,
   strerror on POSIX), without a trailing newline. False when it has none. */
bool nk_platform_describe_os_error(int os_error, char *out, size_t out_size);

/* Spawn a child process with specified arguments, environment variables, and working directory.
 * argv: NULL-terminated array of arguments (argv[0] is program path)
 * envp: NULL-terminated array of "KEY=VAL" strings, or NULL to inherit current environment
 * working_directory: working directory path, or NULL to inherit current
 * output_log_path: optional append-only destination for stdout and stderr; when
 *                  absent, Windows sends those streams to NUL and creates no console
 */
bool nk_platform_spawn_process(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    const char *output_log_path,
    NkProcessHandle *out_process
);

bool nk_platform_is_process_running(NkProcessHandle *process);
int nk_platform_wait_process(NkProcessHandle *process, int timeout_ms);
/* Gives the foreground to the child's visible top-level window. The caller must
   be the foreground process, as a launcher that just started the child is.
   False when the child has no such window yet, or where there is no window
   system to ask (the POSIX build). */
bool nk_platform_bring_process_window_to_front(const NkProcessHandle *process);
void nk_platform_terminate_process(NkProcessHandle *process);
void nk_platform_close_process(NkProcessHandle *process);
/* Whether nk_platform_bring_process_window_to_front can work on this host at all. Win32 can;
   no POSIX backend raises another process's window (X11 and Wayland leave focus to the
   window manager), so a false from the raise there is expected rather than a failure. */
bool nk_platform_can_front_process_windows(void);

/* ---- Host seam ------------------------------------------------------------------------
 *
 * Code outside the nk_platform_*.c backends asks these instead of testing _WIN32,
 * __APPLE__, __linux__ or __ANDROID__ itself (docs/PLATFORM_PORTABILITY.md, "One platform
 * seam"; tools/test_platform_seam.py keeps the count of such tests from growing). */

/* The host this build runs on. User-facing text that names a host's own tools or folders
   chooses its wording from this at run time, so every variant compiles, and is tested, on
   every host. */
typedef enum {
    NK_HOST_WINDOWS = 0,
    NK_HOST_LINUX,
    NK_HOST_MACOS,
    NK_HOST_ANDROID,
    NK_HOST_OTHER_POSIX
} NkHostKind;

NkHostKind nk_platform_host_kind(void);

/* Path text comparison as the host's file system compares names: Win32 folds ASCII case
   and treats '/' and '\\' as one separator; POSIX compares bytes. Used to find a folder's
   text inside other text (the profile folder in a message, a root above a path). */
bool nk_platform_path_chars_equal(char a, char b);
/* '\\' or '/' on Win32; '/' on POSIX. */
bool nk_platform_is_path_separator(char c);

/* The raw environment values naming the user: the profile folder (USERPROFILE on Win32,
   HOME on POSIX) and the account name (USERNAME on Win32, USER on POSIX). NULL when unset.
   Not resolved, created or checked: callers that need the per-user folders use
   nk_platform_get_path. */
const char *nk_platform_env_home_dir(void);
const char *nk_platform_env_user_name(void);

/* This process's id (_getpid on Win32, getpid on POSIX). */
unsigned long nk_platform_process_id(void);

/* The OS error code of the last failed nk_platform or nk_*_utf8 file call on this thread:
   GetLastError() on Win32, errno on POSIX. Read it straight after the failing call. */
int nk_platform_last_os_error(void);

/* The SDL3_ttf shared library's file names, most specific first, NULL-terminated:
   SDL3_ttf.dll on Win32, libSDL3_ttf.0.dylib then libSDL3_ttf.dylib on macOS, and
   libSDL3_ttf.so.0 then libSDL3_ttf.so elsewhere. */
const char *const *nk_platform_sdl3_ttf_library_names(void);

/* The first system UI font file found (Win32: the open-source fonts in the per-user fonts
   folder, then %WINDIR%\Fonts; macOS and Linux: a short table of system font paths).
   False when none exists. */
bool nk_platform_find_system_ui_font(char *out_path, size_t max_len);

/* Make main's arguments UTF-8. Win32: the console's output code page becomes UTF-8 and argv
   is rebuilt from the UTF-16 command line (the narrow argv is in the ANSI code page); the
   new array lives until the process exits. POSIX arguments already are UTF-8 bytes: nothing
   changes. False, leaving both untouched, when the command line cannot be converted. */
bool nk_platform_init_utf8_main(int *argc, char ***argv);

/* Give a GUI program usable stdout and stderr. Win32 (a -mwindows program has no console):
   standard handles it was started with are kept; otherwise the streams go to the parent
   terminal's console when there is one, else to `fallback_log_name` in the NK_PATH_LOGS
   folder, rewritten each run. Both streams are then unbuffered, since the Windows CRT has no
   line buffering (#1303). POSIX streams already reach the terminal or the launcher's log:
   nothing changes. Call after nk_platform_set_app_data_dir_override. */
void nk_platform_setup_standard_streams(const char *fallback_log_name);

#ifdef __cplusplus
}
#endif

#endif /* NK_PLATFORM_H */
