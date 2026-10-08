/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Feature-test macro first: the per-run cache root uses POSIX setenv, mkdir,
 * open, fdopendir, and getpid on non-Windows hosts. */
#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "nk_types.h"
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_launch.h"
#include "generated/nk_title_catalog.h"
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

static void set_environment_value(const char *name, const char *value) {
#if defined(_WIN32) || defined(_WIN64)
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

/* Per-run cache isolation (#735 item 8).
 *
 * The native tests must never write synthetic fixtures into the developer's
 * real per-user directories (%LOCALAPPDATA%\Nakagawa on Windows). Each run
 * creates its own temporary root under the system temp directory, points the
 * per-user variables into that root before any fixture is written, and removes
 * the root at exit. Removal also runs from a SIGABRT handler, because assert()
 * reaches abort(), which skips atexit handlers. */
#define NATIVE_TEST_PATH_MAX 1024

static char g_test_root[NATIVE_TEST_PATH_MAX];
static bool g_test_root_owned = false;

static bool native_path_within(const char *child, const char *parent) {
    size_t parent_length = strlen(parent);
    if (strlen(child) < parent_length) return false;
    if (strncmp(child, parent, parent_length) != 0) return false;
    return child[parent_length] == '\0' || child[parent_length] == '/' ||
           child[parent_length] == '\\';
}

/* Removes a directory tree without following links: a symlink or a Win32
 * reparse point is removed as itself, never descended into. */
#if defined(_WIN32) || defined(_WIN64)
static void remove_test_tree_wide(const WCHAR *dir) {
    WCHAR pattern[NATIVE_TEST_PATH_MAX];
    WCHAR child[NATIVE_TEST_PATH_MAX];
    size_t dir_length = wcslen(dir);
    if (dir_length + 3 >= NATIVE_TEST_PATH_MAX) return;
    memcpy(pattern, dir, dir_length * sizeof(WCHAR));
    pattern[dir_length] = L'\\';
    pattern[dir_length + 1] = L'*';
    pattern[dir_length + 2] = L'\0';

    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(pattern, &found);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 ||
                wcscmp(found.cFileName, L"..") == 0) continue;
            size_t name_length = wcslen(found.cFileName);
            if (dir_length + 1 + name_length >= NATIVE_TEST_PATH_MAX) continue;
            memcpy(child, dir, dir_length * sizeof(WCHAR));
            child[dir_length] = L'\\';
            memcpy(child + dir_length + 1, found.cFileName,
                   (name_length + 1) * sizeof(WCHAR));
            bool is_directory =
                (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool is_link =
                (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (is_directory && !is_link) remove_test_tree_wide(child);
            else if (is_directory) RemoveDirectoryW(child);
            else DeleteFileW(child);
        } while (FindNextFileW(handle, &found));
        FindClose(handle);
    }
    RemoveDirectoryW(dir);
}

static void remove_test_tree(const char *path) {
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            (int)(sizeof(wide) / sizeof(wide[0]))) > 0) {
        remove_test_tree_wide(wide);
    }
}
#else
static void remove_test_tree(const char *path) {
    /* Attempt the removal before any inspection, so no earlier check can go
     * stale. unlink() removes a symlink as itself and fails on a directory
     * (EISDIR on Linux, EPERM on BSD/macOS); ENOENT means nothing to remove. */
    if (unlink(path) == 0 || errno == ENOENT) return;
    /* O_NOFOLLOW makes open() refuse a symlink swapped in for the directory,
     * so the walk below cannot descend outside the root. */
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd >= 0) {
        DIR *dir = fdopendir(fd);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) continue;
                char child[NATIVE_TEST_PATH_MAX];
                int written = snprintf(child, sizeof(child), "%s/%s", path,
                                       entry->d_name);
                if (written > 0 && (size_t)written < sizeof(child)) {
                    remove_test_tree(child);
                }
            }
            closedir(dir);
        } else {
            close(fd);
        }
    }
    rmdir(path);
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
    remove_test_tree(g_test_root);
}

static void cleanup_test_root_on_abort(int signal_number) {
    cleanup_test_root();
    signal(signal_number, SIG_DFL);
    raise(signal_number);
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

/* Creates this run's root, named by tag and process id, and arranges for it
 * to be removed at exit and on SIGABRT. Each candidate name is claimed by the
 * exclusive create itself; a name that is already taken moves to the next. */
static void create_test_root(const char *tag) {
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
    (void)signal(SIGABRT, cleanup_test_root_on_abort);
}

/* Points every per-user root into this run's temporary root. Win32 reads an
 * explicit LOCALAPPDATA ahead of the Known Folder; POSIX reads the XDG_* and
 * HOME variables. */
static void isolate_user_data_roots(void) {
    assert(g_test_root_owned);
#if defined(_WIN32) || defined(_WIN64)
    set_environment_value("LOCALAPPDATA", g_test_root);
#else
    static const char *const variables[] = {
        "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME"};
    static const char *const subdirs[] = {"cache", "config", "data", "state"};
    set_environment_value("HOME", g_test_root);
    for (size_t i = 0; i < sizeof(variables) / sizeof(variables[0]); ++i) {
        char path[NATIVE_TEST_PATH_MAX + 32];
        int written = snprintf(path, sizeof(path), "%s/%s", g_test_root,
                               subdirs[i]);
        assert(written > 0 && (size_t)written < sizeof(path));
        set_environment_value(variables[i], path);
    }
#endif
}

/* Guard (#735 item 8): fails when the per-user cache resolves outside this
 * run's temporary root, which is how a test reaches the real profile. */
static void assert_cache_root_isolated(void) {
    char cache_dir[NATIVE_TEST_PATH_MAX];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
    if (!native_path_within(cache_dir, g_test_root)) {
        fprintf(stderr,
                "NATIVE_TEST_GUARD: cache root is outside the temporary root "
                "(cache root '%s', temporary root '%s')\n",
                cache_dir, g_test_root);
        exit(EXIT_FAILURE);
    }
}

int main(void) {
    create_test_root("core-catalog");
    isolate_user_data_roots();
    assert_cache_root_isolated();

    /* Catalog SIZE is deliberately not asserted here. It is a literal that every
       new public title has to come back and edit, and it never caught anything:
       tools/title_catalog_codegen.py --verify and the codegen drift test already
       prove the catalog matches assets/titles/ exactly. What the native side can
       usefully check is that the table it compiled is internally coherent. */
    printf("[NATIVE_TEST] Verifying public title catalog integrity...\n");
    assert(nk_title_catalog_count > 0);
    for (int i = 0; i < nk_title_catalog_count; i++) {
        const NkTitleEntry *e = &nk_title_catalog_entries[i];
        assert(e->id != NULL && e->id[0] != '\0');
        assert(e->game_name != NULL && e->game_name[0] != '\0');
        assert(e->display_name != NULL && e->display_name[0] != '\0');
        assert(e->primary_disc_id != NULL && e->primary_disc_id[0] != '\0');
        /* Every disc id resolves, and resolves to THIS entry. Two entries sharing
           a disc id used to be reachable: unassigned synthetic titles all shared a
           "TEST00000" sentinel, so lookup silently returned whichever came first. */
        NkTitleEntrySnapshot by_disc = {0};
        NkTitleEntrySnapshot by_id = {0};
        assert(nk_title_catalog_find_by_disc_id(e->primary_disc_id, &by_disc));
        assert(nk_title_catalog_find_by_id(e->id, &by_id));
        assert(strcmp(by_disc.entry.id, e->id) == 0);
        assert(strcmp(by_id.entry.id, e->id) == 0);
        nk_title_catalog_snapshot_release(&by_disc);
        nk_title_catalog_snapshot_release(&by_id);
    }

    printf("[NATIVE_TEST] Verifying public source-owned title lookups...\n");
    NkTitleEntrySnapshot t_synth1 = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00001", &t_synth1));
    assert(strcmp(t_synth1.entry.id, "synthetic-allegrex-v1") == 0);
    assert(t_synth1.entry.kind == NK_TITLE_KIND_SYNTHETIC);

    NkTitleEntrySnapshot t_p5 = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00005", &t_p5));
    assert(strcmp(t_p5.entry.id, "pspdev-phase5-v1") == 0);
    assert(t_p5.entry.kind == NK_TITLE_KIND_SYNTHETIC);

    NkTitleEntrySnapshot t_synth2 = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00002", &t_synth2));
    assert(strcmp(t_synth2.entry.id, "synthetic-title2-v1") == 0);

    /* The display fixture is the one public title built under the layout
       src/core/nk_launch.c can actually resolve, so its addresses are load
       bearing for the launch path, not just for the catalog. */
    NkTitleEntrySnapshot t_disp = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00006", &t_disp));
    assert(strcmp(t_disp.entry.id, "display-smoke-v1") == 0);
    assert(t_disp.entry.kind == NK_TITLE_KIND_SYNTHETIC);
    assert(strcmp(t_disp.entry.game_name, "display-smoke") == 0);
    assert(t_disp.entry.executable_base == 0x08810000u);
    assert(t_disp.entry.executable_entry == 0x08810000u);

    /* Verify normalization (hyphens/spaces) */
    NkTitleEntrySnapshot normalized = {0};
    assert(nk_title_catalog_find_by_disc_id("test-00001", &normalized));
    assert(strcmp(normalized.entry.id, t_synth1.entry.id) == 0);
    nk_title_catalog_snapshot_release(&normalized);
    assert(nk_title_catalog_find_by_disc_id("  TEST_00005  ", &normalized));
    assert(strcmp(normalized.entry.id, t_p5.entry.id) == 0);
    nk_title_catalog_snapshot_release(&normalized);

    printf("[NATIVE_TEST] Verifying retail IDs are NOT present in public catalog...\n");
    NkTitleEntrySnapshot missing = {0};
    assert(!nk_title_catalog_find_by_disc_id("UCUS98701", &missing));
    assert(!nk_title_catalog_find_by_disc_id("UCES01402", &missing));
    assert(!nk_title_catalog_find_by_disc_id("ULUS99999", &missing));
    assert(!nk_title_catalog_find_by_id("hst-ucus98701-v1", &missing));

    printf("[NATIVE_TEST] Verifying in-memory private overlay registration...\n");
    NkTitleEntry private_overlay;
    memset(&private_overlay, 0, sizeof(private_overlay));
    private_overlay.id = "private-local-test-v1";
    private_overlay.display_name = "Local Acceptance Private Title";
    private_overlay.kind = NK_TITLE_KIND_RETAIL;
    private_overlay.primary_disc_id = "UCUS98701";

    /* Before registration: NULL */
    assert(!nk_title_catalog_find_by_disc_id("UCUS98701", &missing));

    /* Register overlay */
    nk_title_catalog_register_overlay(&private_overlay);
    NkTitleEntrySnapshot found_ov = {0};
    NkTitleEntrySnapshot found_ov_by_id = {0};
    assert(nk_title_catalog_find_by_disc_id("UCUS-98701", &found_ov));
    assert(strcmp(found_ov.entry.id, "private-local-test-v1") == 0);
    assert(nk_title_catalog_find_by_id("private-local-test-v1",
                                       &found_ov_by_id));
    assert(strcmp(found_ov_by_id.entry.id, found_ov.entry.id) == 0);

    /* Clear overlay */
    nk_title_catalog_clear_overlay();
    assert(strcmp(found_ov.entry.display_name,
                  "Local Acceptance Private Title") == 0);
    assert(!nk_title_catalog_find_by_disc_id("UCUS98701", &missing));
    assert(!nk_title_catalog_find_by_id("private-local-test-v1", &missing));

    printf("[NATIVE_TEST] Verifying library in-memory operations...\n");
    NkLibrary lib;
    nk_library_init(&lib);
    assert(nk_library_count(&lib) == 0);

    NkGameEntry g1;
    memset(&g1, 0, sizeof(g1));
    snprintf(g1.disc_id, sizeof(g1.disc_id), "TEST00001");
    snprintf(g1.title_name, sizeof(g1.title_name), "%s", t_synth1.entry.display_name);
    snprintf(g1.title_id, sizeof(g1.title_id), "%s", t_synth1.entry.id);
    g1.status = NK_STATUS_VERIFIED;
    g1.is_prepared = true;

    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_count(&lib) == 1);
    assert(nk_library_find_by_disc_id(&lib, "TEST00001") != NULL);

    assert(nk_library_remove(&lib, "TEST00001") == NK_OK);
    assert(nk_library_count(&lib) == 0);

    printf("[NATIVE_TEST] Verifying structured data directory routing...\n");
    char path_buf[512];
    assert(nk_platform_get_path(NK_PATH_CONFIG, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_DATA, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_CACHE, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_LOGS, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_SAVES, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    printf("[NATIVE_TEST] Verifying Unicode directory & file handling...\n");
    char cache_dir[512];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));

    /* Test 1: Japanese characters (日本語) */
    char u_jp[600];
    snprintf(u_jp, sizeof(u_jp), "%s%c%s", cache_dir, nk_platform_path_separator(), "テスト_日本語_ディレクトリ");
    assert(nk_platform_mkdir_p(u_jp));
    assert(nk_platform_dir_exists(u_jp));

    /* Test 2: Latin accents and spaces */
    char u_latin[600];
    snprintf(u_latin, sizeof(u_latin), "%s%c%s", cache_dir, nk_platform_path_separator(), "Carpeta con Espacios y Acentos áéíóú");
    assert(nk_platform_mkdir_p(u_latin));
    assert(nk_platform_dir_exists(u_latin));

    /* Test 3: Emoji directory */
    char u_emoji[600];
    snprintf(u_emoji, sizeof(u_emoji), "%s%c%s", cache_dir, nk_platform_path_separator(), "🎮_PSP_Game_Directory");
    assert(nk_platform_mkdir_p(u_emoji));
    assert(nk_platform_dir_exists(u_emoji));

    /* Test 4: File existence in Unicode directory */
    char u_file[700];
    snprintf(u_file, sizeof(u_file), "%s%c%s", u_jp, nk_platform_path_separator(), "テスト_file.txt");
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wfile[1024];
    MultiByteToWideChar(CP_UTF8, 0, u_file, -1, wfile, 1024);
    FILE *f_uni = _wfopen(wfile, L"wb");
#else
    FILE *f_uni = fopen(u_file, "wb");
#endif
    assert(f_uni != NULL);
    const char test_data[] = "Unicode content check 12345";
    fwrite(test_data, 1, sizeof(test_data), f_uni);
    fclose(f_uni);

    assert(nk_platform_file_exists(u_file));
    assert(nk_platform_get_file_size(u_file) == (int64_t)sizeof(test_data));

    printf("[NATIVE_TEST] ALL CORE NATIVE C TESTS PASSED SUCCESSFULLY!\n");
    nk_title_catalog_snapshot_release(&found_ov);
    nk_title_catalog_snapshot_release(&found_ov_by_id);
    nk_title_catalog_snapshot_release(&t_synth1);
    nk_title_catalog_snapshot_release(&t_p5);
    nk_title_catalog_snapshot_release(&t_synth2);
    nk_title_catalog_snapshot_release(&t_disp);
    return 0;
}
