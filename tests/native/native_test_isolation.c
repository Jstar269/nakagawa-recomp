/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Per-run test isolation for native tests (#735 item 8, #745).
 *
 * The native tests must never write synthetic fixtures into the developer's
 * real per-user directories (%LOCALAPPDATA%\Nakagawa on Windows). Each run
 * creates its own temporary root under the system temp directory, points the
 * per-user variables into that root before any fixture is written, and removes
 * the root at exit via atexit.
 *
 * Abort cleanup design:
 * Cleanup is registered strictly via atexit() for normal process exits. A
 * SIGABRT signal handler is intentionally not used because recursive filesystem
 * deletion, string formatting, and Win32/POSIX directory APIs are not
 * async-signal-safe. Calling them from a signal handler during abort() / assert()
 * can deadlock on CRT or heap locks or cause undefined behavior. Furthermore, leaving
 * the temporary directory intact upon assertion failure facilitates post-mortem
 * debugging, while exclusive per-process-id and attempt naming ensures no
 * collision with subsequent test runs.
 */

#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

/* The per-run root setup and its recursive cleanup run inside assert(); keep
 * them even in a build that defines NDEBUG (#735). */
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "native_test_isolation.h"
#include "nk_platform.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char g_test_root[NATIVE_TEST_PATH_MAX];
static bool g_test_root_owned = false;

static char g_real_config_probe[NATIVE_TEST_PATH_MAX];
static bool g_real_config_captured = false;
static bool g_real_config_existed_before = false;
static bool g_real_input_profiles_existed_before = false;
static size_t g_real_input_profiles_file_count = 0;
static bool g_real_disc_profile_existed_before = false;
static int64_t g_real_disc_profile_size = -1;
static uint64_t g_real_disc_profile_mtime = 0;

void native_test_set_env(const char *name, const char *value) {
    bool is_clear = (value == NULL || value[0] == '\0');
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wname[NATIVE_TEST_PATH_MAX];
    int name_converted = MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, NATIVE_TEST_PATH_MAX);
    assert(name_converted > 0);
    if (is_clear) {
        assert(_putenv_s(name, "") == 0);
        SetEnvironmentVariableW(wname, NULL);
    } else {
        assert(_putenv_s(name, value) == 0);
        WCHAR wval[NATIVE_TEST_PATH_MAX];
        int val_converted = MultiByteToWideChar(CP_UTF8, 0, value, -1, wval, NATIVE_TEST_PATH_MAX);
        assert(val_converted > 0);
        SetEnvironmentVariableW(wname, wval);
    }
#else
    if (is_clear) {
        assert(unsetenv(name) == 0);
    } else {
        assert(setenv(name, value, 1) == 0);
    }
#endif
}

bool native_test_path_within(const char *child, const char *parent) {
    if (!child || !parent) return false;
    size_t parent_length = strlen(parent);
    if (strlen(child) < parent_length) return false;
    if (strncmp(child, parent, parent_length) != 0) return false;
    return child[parent_length] == '\0' || child[parent_length] == '/' ||
           child[parent_length] == '\\';
}

/* Removes a directory tree without following links: a symlink or a Win32
 * reparse point is removed as itself, never descended into. */
#if defined(_WIN32) || defined(_WIN64)
static bool remove_test_tree_wide(const WCHAR *dir) {
    WCHAR pattern[NATIVE_TEST_PATH_MAX];
    WCHAR child[NATIVE_TEST_PATH_MAX];
    size_t dir_length = wcslen(dir);
    if (dir_length + 3 >= NATIVE_TEST_PATH_MAX) {
        fprintf(stderr, "NATIVE_TEST_ERROR: directory path too long for pattern: %ls\n", dir);
        return false;
    }
    memcpy(pattern, dir, dir_length * sizeof(WCHAR));
    pattern[dir_length] = L'\\';
    pattern[dir_length + 1] = L'*';
    pattern[dir_length + 2] = L'\0';

    bool success = true;
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(pattern, &found);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 ||
                wcscmp(found.cFileName, L"..") == 0) continue;
            size_t name_length = wcslen(found.cFileName);
            if (dir_length + 1 + name_length >= NATIVE_TEST_PATH_MAX) {
                fprintf(stderr, "NATIVE_TEST_ERROR: child path exceeds buffer: %ls\\%ls\n",
                        dir, found.cFileName);
                success = false;
                continue;
            }
            memcpy(child, dir, dir_length * sizeof(WCHAR));
            child[dir_length] = L'\\';
            memcpy(child + dir_length + 1, found.cFileName,
                   (name_length + 1) * sizeof(WCHAR));
            bool is_directory =
                (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool is_link =
                (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (is_directory && !is_link) {
                if (!remove_test_tree_wide(child)) success = false;
            } else if (is_directory) {
                if (!RemoveDirectoryW(child)) success = false;
            } else {
                if (!DeleteFileW(child)) success = false;
            }
        } while (FindNextFileW(handle, &found));
        FindClose(handle);
    }
    if (!RemoveDirectoryW(dir)) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            success = false;
        }
    }
    return success;
}

bool native_test_remove_tree(const char *path) {
    if (!path || !*path) return false;
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            (int)(sizeof(wide) / sizeof(wide[0]))) <= 0) {
        fprintf(stderr,
                "NATIVE_TEST_ERROR: native_test_remove_tree failed to convert path '%s' to UTF-16 (error %lu)\n",
                path, (unsigned long)GetLastError());
        return false;
    }
    return remove_test_tree_wide(wide);
}
#else
bool native_test_remove_tree(const char *path) {
    if (!path || !*path) return false;
    /* Attempt the removal before any inspection, so no earlier check can go
     * stale. unlink() removes a symlink as itself and fails on a directory
     * (EISDIR on Linux, EPERM on BSD/macOS); ENOENT means nothing to remove. */
    if (unlink(path) == 0 || errno == ENOENT) return true;
    /* O_NOFOLLOW makes open() refuse a symlink swapped in for the directory,
     * so the walk below cannot descend outside the root. */
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return true;
        if (rmdir(path) == 0 || errno == ENOENT) return true;
        fprintf(stderr, "NATIVE_TEST_ERROR: failed to open directory '%s': %s\n",
                path, strerror(errno));
        return false;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        fprintf(stderr, "NATIVE_TEST_ERROR: fdopendir failed on '%s': %s\n",
                path, strerror(errno));
        return false;
    }
    bool success = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        char child[NATIVE_TEST_PATH_MAX];
        int written = snprintf(child, sizeof(child), "%s/%s", path,
                               entry->d_name);
        if (written > 0 && (size_t)written < sizeof(child)) {
            if (!native_test_remove_tree(child)) success = false;
        } else {
            fprintf(stderr, "NATIVE_TEST_ERROR: child path overflow in '%s/%s'\n",
                    path, entry->d_name);
            success = false;
        }
    }
    closedir(dir);
    if (rmdir(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "NATIVE_TEST_ERROR: rmdir failed on '%s': %s\n",
                path, strerror(errno));
        success = false;
    }
    return success;
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
    (void)native_test_remove_tree(g_test_root);
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

void native_test_create_root(const char *tag) {
    native_test_capture_real_config_probe();
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
}

void native_test_isolate_user_data_roots(void) {
    assert(g_test_root_owned);
#if defined(_WIN32) || defined(_WIN64)
    native_test_set_env("LOCALAPPDATA", g_test_root);
    native_test_set_env("APPDATA", g_test_root);
#else
    static const char *const variables[] = {
        "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME"};
    static const char *const subdirs[] = {"cache", "config", "data", "state"};
    native_test_set_env("HOME", g_test_root);
    for (size_t i = 0; i < sizeof(variables) / sizeof(variables[0]); ++i) {
        char path[NATIVE_TEST_PATH_MAX + 32];
        int written = snprintf(path, sizeof(path), "%s/%s", g_test_root,
                               subdirs[i]);
        assert(written > 0 && (size_t)written < sizeof(path));
        native_test_set_env(variables[i], path);
    }
#endif
}

void native_test_assert_cache_root_isolated(void) {
    char cache_dir[NATIVE_TEST_PATH_MAX];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
    if (!native_test_path_within(cache_dir, g_test_root)) {
        fprintf(stderr,
                "NATIVE_TEST_GUARD: cache root is outside the temporary root "
                "(cache root '%s', temporary root '%s')\n",
                cache_dir, g_test_root);
        exit(EXIT_FAILURE);
    }
}

void native_test_assert_config_root_isolated(void) {
    char config_dir[NATIVE_TEST_PATH_MAX];
    assert(nk_platform_get_path(NK_PATH_CONFIG, config_dir, sizeof(config_dir)));
    if (!native_test_path_within(config_dir, g_test_root)) {
        fprintf(stderr,
                "NATIVE_TEST_GUARD: config root is outside the temporary root "
                "(config root '%s', temporary root '%s')\n",
                config_dir, g_test_root);
        exit(EXIT_FAILURE);
    }
}

#if defined(_WIN32) || defined(_WIN64)
static bool get_file_metadata(const char *path, int64_t *out_size, uint64_t *out_mtime) {
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            (int)(sizeof(wide) / sizeof(wide[0]))) <= 0) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &fad)) return false;
    if (out_size) {
        LARGE_INTEGER size;
        size.LowPart = fad.nFileSizeLow;
        size.HighPart = (LONG)fad.nFileSizeHigh;
        *out_size = (int64_t)size.QuadPart;
    }
    if (out_mtime) {
        ULARGE_INTEGER mtime;
        mtime.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        mtime.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        *out_mtime = mtime.QuadPart;
    }
    return true;
}
#else
static bool get_file_metadata(const char *path, int64_t *out_size, uint64_t *out_mtime) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (out_size) *out_size = (int64_t)st.st_size;
    if (out_mtime) *out_mtime = (uint64_t)st.st_mtime;
    return true;
}
#endif

static bool count_files_callback(const char *name, void *ctx) {
    (void)name;
    size_t *count = (size_t *)ctx;
    (*count)++;
    return true;
}

void native_test_capture_real_config_probe(void) {
    if (g_real_config_captured) return;
    g_real_config_captured = true;
    g_real_config_probe[0] = '\0';
    char sep = nk_platform_path_separator();
#if defined(_WIN32) || defined(_WIN64)
    const char *env_lad = getenv("LOCALAPPDATA");
    const char *env_ad = getenv("APPDATA");
    const char *base = (env_lad && env_lad[0]) ? env_lad : ((env_ad && env_ad[0]) ? env_ad : NULL);
    if (base) {
        snprintf(g_real_config_probe, sizeof(g_real_config_probe),
                 "%s%cNakagawa%cconfig", base, sep, sep);
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
        char probe[NATIVE_TEST_PATH_MAX + 64];
        g_real_config_existed_before = nk_platform_dir_exists(g_real_config_probe);
        snprintf(probe, sizeof(probe), "%s%cinput_profiles", g_real_config_probe, sep);
        g_real_input_profiles_existed_before = nk_platform_dir_exists(probe);
        if (g_real_input_profiles_existed_before) {
            g_real_input_profiles_file_count = 0;
            nk_platform_list_files(probe, count_files_callback, &g_real_input_profiles_file_count);
        }
        snprintf(probe, sizeof(probe), "%s%cinput_profiles%cUCUS98701.json",
                 g_real_config_probe, sep, sep);
        g_real_disc_profile_existed_before = nk_platform_file_exists(probe);
        if (g_real_disc_profile_existed_before) {
            assert(get_file_metadata(probe, &g_real_disc_profile_size, &g_real_disc_profile_mtime));
        }
    }
}

void native_test_assert_real_config_untouched(void) {
    if (!g_real_config_probe[0]) return;
    char sep = nk_platform_path_separator();
    char probe[NATIVE_TEST_PATH_MAX + 64];

    /* Disc profile assertion: must not be created if absent before; if it
     * existed before the run, its size and mtime must be unchanged. */
    snprintf(probe, sizeof(probe), "%s%cinput_profiles%cUCUS98701.json",
             g_real_config_probe, sep, sep);
    if (!g_real_disc_profile_existed_before) {
        assert(!nk_platform_file_exists(probe));
    } else {
        int64_t current_size = -1;
        uint64_t current_mtime = 0;
        assert(get_file_metadata(probe, &current_size, &current_mtime));
        assert(current_size == g_real_disc_profile_size);
        assert(current_mtime == g_real_disc_profile_mtime);
    }

    /* Input profiles directory assertion: must not be created if absent;
     * if it existed before, total file count within must be unchanged. */
    snprintf(probe, sizeof(probe), "%s%cinput_profiles", g_real_config_probe, sep);
    if (!g_real_input_profiles_existed_before) {
        assert(!nk_platform_dir_exists(probe));
    } else {
        size_t current_count = 0;
        nk_platform_list_files(probe, count_files_callback, &current_count);
        assert(current_count == g_real_input_profiles_file_count);
    }

    /* Real config directory assertion: must not be created if absent. */
    if (!g_real_config_existed_before) {
        assert(!nk_platform_dir_exists(g_real_config_probe));
    }
}

const char *native_test_get_root(void) {
    return g_test_root;
}

const char *native_test_get_real_config_probe(void) {
    return g_real_config_probe;
}
