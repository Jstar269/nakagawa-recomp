/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Portable mode (#1850): a portable.txt beside the executable moves every per-user
 * folder under <that folder>/data; a marker beside a folder that cannot be written is
 * refused with a message and never answered with the default root; with no marker the
 * default root is unchanged. */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "nk_platform.h"
#include "nk_portable.h"
#include "native_test_isolation.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char g_sep;

/* A fresh install folder under the run's temp root, with an executable path inside it. */
static void make_install(const char *name, char *folder, size_t folder_size, char *exe,
                         size_t exe_size) {
    assert(snprintf(folder, folder_size, "%s%c%s", native_test_get_root(), g_sep, name) <
           (int)folder_size);
    assert(nk_platform_mkdir_p(folder));
    assert(snprintf(exe, exe_size, "%s%cnakagawa_player.exe", folder, g_sep) < (int)exe_size);
}

static void write_marker(const char *folder) {
    char marker[NK_MAX_PATH];
    assert(snprintf(marker, sizeof(marker), "%s%c%s", folder, g_sep, NK_PORTABLE_MARKER) <
           (int)sizeof(marker));
    FILE *file = nk_fopen_utf8(marker, "wb");
    assert(file != NULL);
    fputs("portable\n", file);
    fclose(file);
}

static bool within(const char *path, const char *root) {
    size_t n = strlen(root);
    return strncmp(path, root, n) == 0 && (path[n] == g_sep || path[n] == '\0');
}

static void test_default_unchanged(void) {
    char folder[NK_MAX_PATH], exe[NK_MAX_PATH], before[NK_MAX_PATH], after[NK_MAX_PATH];
    NkPortableResolution res;
    make_install("absent", folder, sizeof(folder), exe, sizeof(exe));
    assert(nk_platform_resolve_app_data_dir(before, sizeof(before)));
    assert(nk_portable_resolve(exe, &res) == NK_PORTABLE_ABSENT);
    assert(res.state == NK_PORTABLE_ABSENT && res.root[0] == '\0' && res.message[0] == '\0');
    /* No marker: no data folder is made beside the program either. */
    char data[NK_MAX_PATH];
    assert(snprintf(data, sizeof(data), "%s%cdata", folder, g_sep) < (int)sizeof(data));
    assert(!nk_platform_dir_exists(data));
    assert(nk_platform_resolve_app_data_dir(after, sizeof(after)));
    assert(strcmp(before, after) == 0);
    /* This test program has no marker beside it, so the process-level entry point
       leaves the platform root alone, too. */
    assert(nk_portable_apply(&res) == NK_PORTABLE_ABSENT);
    assert(!nk_portable_in_use());
    assert(nk_platform_resolve_app_data_dir(after, sizeof(after)));
    assert(strcmp(before, after) == 0);
    /* A path without a folder part has no portable mode. */
    assert(nk_portable_resolve("nakagawa_player.exe", &res) == NK_PORTABLE_ABSENT);
    assert(nk_portable_resolve("", &res) == NK_PORTABLE_ABSENT);
    assert(nk_portable_resolve(NULL, &res) == NK_PORTABLE_ABSENT);
}

static void test_marker_elsewhere_is_ignored(void) {
    char folder[NK_MAX_PATH], exe[NK_MAX_PATH], other[NK_MAX_PATH], other_exe[NK_MAX_PATH];
    NkPortableResolution res;
    make_install("here", folder, sizeof(folder), exe, sizeof(exe));
    make_install("there", other, sizeof(other), other_exe, sizeof(other_exe));
    write_marker(other);
    assert(nk_portable_resolve(exe, &res) == NK_PORTABLE_ABSENT);
    /* A marker one folder up is not "beside" the executable. */
    char nested[NK_MAX_PATH], nested_exe[NK_MAX_PATH];
    assert(snprintf(nested, sizeof(nested), "%s%cbin", other, g_sep) < (int)sizeof(nested));
    assert(nk_platform_mkdir_p(nested));
    assert(snprintf(nested_exe, sizeof(nested_exe), "%s%cnakagawa_player.exe", nested, g_sep) <
           (int)sizeof(nested_exe));
    assert(nk_portable_resolve(nested_exe, &res) == NK_PORTABLE_ABSENT);
}

static void test_marker_routes_every_folder(void) {
    char folder[NK_MAX_PATH], exe[NK_MAX_PATH], expected[NK_MAX_PATH];
    char default_data[NK_MAX_PATH];
    NkPortableResolution res;
    make_install("portable", folder, sizeof(folder), exe, sizeof(exe));
    write_marker(folder);
    assert(nk_platform_resolve_app_data_dir(default_data, sizeof(default_data)));
    assert(nk_portable_resolve(exe, &res) == NK_PORTABLE_ACTIVE);
    assert(snprintf(expected, sizeof(expected), "%s%cdata", folder, g_sep) <
           (int)sizeof(expected));
    assert(strcmp(res.root, expected) == 0);
    assert(res.message[0] == '\0');
    assert(nk_platform_dir_exists(res.root));
    /* The probe file is gone. */
    char probe[NK_MAX_PATH];
    assert(snprintf(probe, sizeof(probe), "%s%c%s", res.root, g_sep, NK_PORTABLE_PROBE_LEAF) <
           (int)sizeof(probe));
    assert(!nk_platform_file_exists(probe));

    /* Handing the root to the platform seam moves every per-user folder, so nothing that
       asks the platform can reach the default profile. */
    assert(nk_platform_set_app_data_dir_override(res.root));
    static const NkPathType types[] = {NK_PATH_DATA, NK_PATH_CONFIG, NK_PATH_CACHE,
                                       NK_PATH_LOGS, NK_PATH_SAVES};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        char path[NK_MAX_PATH], resolved[NK_MAX_PATH];
        assert(nk_platform_get_path(types[i], path, sizeof(path)));
        assert(within(path, res.root));
        assert(nk_platform_resolve_path(types[i], resolved, sizeof(resolved)));
        assert(strcmp(path, resolved) == 0);
    }
    char app_data[NK_MAX_PATH];
    assert(nk_platform_get_app_data_dir(app_data, sizeof(app_data)));
    assert(strcmp(app_data, res.root) == 0);
    assert(strcmp(app_data, default_data) != 0);
    assert(nk_platform_set_app_data_dir_override(NULL));
    char restored[NK_MAX_PATH];
    assert(nk_platform_resolve_app_data_dir(restored, sizeof(restored)));
    assert(strcmp(restored, default_data) == 0);

    /* A second start finds the data folder already there. */
    assert(nk_portable_resolve(exe, &res) == NK_PORTABLE_ACTIVE);
}

static void expect_refused(const char *exe, const char *code, const char *folder) {
    char before[NK_MAX_PATH], after[NK_MAX_PATH];
    NkPortableResolution res;
    assert(nk_platform_resolve_app_data_dir(before, sizeof(before)));
    assert(nk_portable_resolve(exe, &res) == NK_PORTABLE_REFUSED);
    assert(res.state == NK_PORTABLE_REFUSED);
    assert(strstr(res.message, code) != NULL);
    assert(strstr(res.message, NK_PORTABLE_MARKER) != NULL);
    if (folder) assert(strstr(res.message, folder) != NULL);
    /* Refused means the default root was not taken instead. */
    assert(nk_platform_resolve_app_data_dir(after, sizeof(after)));
    assert(strcmp(before, after) == 0);
}

#if !defined(_WIN32) && !defined(_WIN64)
/* chmod a folder through a descriptor opened once, so no name is checked and then used. */
static void set_dir_mode(const char *dir, mode_t mode) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    assert(fchmod(fd, mode) == 0);
    assert(close(fd) == 0);
}
#endif

static void test_unwritable_is_refused(void) {
    char folder[NK_MAX_PATH], exe[NK_MAX_PATH], data[NK_MAX_PATH];
    make_install("blocked", folder, sizeof(folder), exe, sizeof(exe));
    write_marker(folder);
    /* A file where the data folder belongs: the folder cannot be made or written on any host. */
    assert(snprintf(data, sizeof(data), "%s%cdata", folder, g_sep) < (int)sizeof(data));
    FILE *file = nk_fopen_utf8(data, "wb");
    assert(file != NULL);
    fputs("not a folder\n", file);
    fclose(file);
    expect_refused(exe, "PORTABLE_NOT_WRITABLE", folder);
    assert(!nk_platform_dir_exists(data));

#if !defined(_WIN32) && !defined(_WIN64)
    /* A genuinely read-only install folder, where the host enforces it (not as root). The
       mode is changed through an open descriptor, never by name after a check. */
    if (geteuid() != 0) {
        char ro[NK_MAX_PATH], ro_exe[NK_MAX_PATH], ro_data[NK_MAX_PATH];
        make_install("readonly", ro, sizeof(ro), ro_exe, sizeof(ro_exe));
        write_marker(ro);
        set_dir_mode(ro, 0555);
        expect_refused(ro_exe, "PORTABLE_NOT_WRITABLE", ro);
        set_dir_mode(ro, 0755);
        /* And a data folder that exists but is read-only: made read-only in one step. */
        assert(snprintf(ro_data, sizeof(ro_data), "%s%cdata", ro, g_sep) < (int)sizeof(ro_data));
        assert(mkdir(ro_data, 0555) == 0);
        expect_refused(ro_exe, "PORTABLE_NOT_WRITABLE", ro);
    }
#endif
}

static void test_too_long_is_refused(void) {
    char base[NK_MAX_PATH], leaf[NK_MAX_PATH], long_folder[NK_MAX_PATH], exe[NK_MAX_PATH];
    char marker[NK_MAX_PATH];
    NkPortableResolution res;
    /* A folder whose path leaves no room for the data subfolders below it. */
    assert(snprintf(base, sizeof(base), "%s", native_test_get_root()) < (int)sizeof(base));
    size_t used = strlen(base) + 1u;
    size_t want = NK_MAX_PATH - 64u - 5u; /* "<folder>/data" reaches the limit */
    assert(used < want);
    /* Components of at most 100 characters: a name over 255 is refused by POSIX file systems. */
    leaf[0] = '\0';
    for (size_t length = used - 1u; length < want;) {
        size_t left = want - length;
        /* Never leave a lone separator for the last component. */
        size_t part = left > 100u ? (left - 100u == 1u ? 99u : 100u) : left;
        size_t at = strlen(leaf);
        assert(part >= 2u); /* a separator and a character */
        leaf[at] = g_sep;
        memset(leaf + at + 1u, 'b', part - 1u);
        leaf[at + part] = '\0';
        length += part;
    }
    /* leaf starts with a separator. */
    assert(snprintf(long_folder, sizeof(long_folder), "%s%s", base, leaf) <
           (int)sizeof(long_folder));
    assert(strlen(long_folder) == want);
    assert(snprintf(marker, sizeof(marker), "%s%c%s", long_folder, g_sep, NK_PORTABLE_MARKER) <
           (int)sizeof(marker));
    assert(snprintf(exe, sizeof(exe), "%s%cx.exe", long_folder, g_sep) < (int)sizeof(exe));
    FILE *file = nk_platform_mkdir_p(long_folder) ? nk_fopen_utf8(marker, "wb") : NULL;
    if (!file) {
        /* Windows file APIs without long-path support cannot make this folder. */
        printf("[PORTABLE_TEST] too-long case skipped: the host cannot make the folder\n");
        return;
    }
    fclose(file);
    assert(nk_portable_resolve(exe, &res) == NK_PORTABLE_REFUSED);
    assert(strstr(res.message, "PORTABLE_PATH_TOO_LONG") != NULL);
    assert(res.root[0] == '\0');
}

int main(void) {
    g_sep = nk_platform_path_separator();
    native_test_create_root("portable");
    native_test_isolate_user_data_roots();
    test_default_unchanged();
    test_marker_elsewhere_is_ignored();
    test_marker_routes_every_folder();
    test_unwritable_is_refused();
    test_too_long_is_refused();
    printf("[PORTABLE_TEST] ok\n");
    return 0;
}
