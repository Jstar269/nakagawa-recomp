/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_PORTABLE_H
#define NK_PORTABLE_H

/* Portable mode (#1850): a marker file, portable.txt, in the folder of the player
 * executable keeps every per-user folder in <that folder>/data instead of the
 * per-user default. The player's own seam does the routing: the root found here is
 * handed to nk_platform_set_app_data_dir_override, so the data folder is the root and
 * config, cache, logs and saves are its subfolders, exactly as with --user-data-root.
 * Nothing else decides the root.
 *
 * Header-only on purpose: every function is built from nk_platform.h calls, and the
 * player, the tests and the About page all see the same decision.
 *
 * A marker beside a folder that cannot be written is REFUSED, never quietly answered
 * with the default root: the user asked for their data to travel with the program, and
 * writing it somewhere else would hide that it did not. */

#include "nk_platform.h"
#include "nk_types.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define NK_PORTABLE_MARKER "portable.txt"
#define NK_PORTABLE_DATA_LEAF "data"
#define NK_PORTABLE_PROBE_LEAF ".nk-portable-write-test"

typedef enum {
    NK_PORTABLE_ABSENT = 0, /* no marker: the default per-user root stands */
    NK_PORTABLE_ACTIVE,     /* marker found, root usable */
    NK_PORTABLE_REFUSED     /* marker found, root unusable: stop with `message` */
} NkPortableState;

typedef struct {
    NkPortableState state;
    char root[NK_MAX_PATH];     /* <exe folder>/data; set for ACTIVE and REFUSED */
    char message[1536];          /* the refusal, for REFUSED; else empty */
} NkPortableResolution;

/* The length of exe_path up to, not including, its last separator ('/' or '\\').
 * 0 when it has none or the folder would be empty. */
static inline size_t nk_portable_folder_length(const char *exe_path) {
    size_t length = exe_path ? strlen(exe_path) : 0u;
    while (length > 0u && exe_path[length - 1u] != '/' && exe_path[length - 1u] != '\\') {
        length--;
    }
    if (length <= 1u) return 0u;
    return length - 1u;
}

/* Whether a file can be created, written and removed in `dir`. */
static inline bool nk_portable_dir_writable(const char *dir) {
    char probe[NK_MAX_PATH];
    int n = snprintf(probe, sizeof(probe), "%s%c%s", dir, nk_platform_path_separator(),
                     NK_PORTABLE_PROBE_LEAF);
    if (n <= 0 || (size_t)n >= sizeof(probe)) return false;
    FILE *file = nk_fopen_utf8(probe, "wb");
    if (!file) return false;
    bool wrote = fputc('x', file) != EOF;
    if (fclose(file) != 0) wrote = false;
    (void)nk_remove_utf8(probe);
    return wrote;
}

/* Decide the data root for the program at exe_path. Creates <folder>/data when the
 * marker is there; touches nothing otherwise. */
static inline NkPortableState nk_portable_resolve(const char *exe_path,
                                                  NkPortableResolution *out) {
    char folder[NK_MAX_PATH];
    char marker[NK_MAX_PATH];
    size_t folder_length;
    char sep = nk_platform_path_separator();
    if (!out) return NK_PORTABLE_ABSENT;
    memset(out, 0, sizeof(*out));
    out->state = NK_PORTABLE_ABSENT;
    folder_length = nk_portable_folder_length(exe_path);
    if (folder_length == 0u || folder_length >= sizeof(folder)) return out->state;
    memcpy(folder, exe_path, folder_length);
    folder[folder_length] = '\0';
    if (snprintf(marker, sizeof(marker), "%s%c%s", folder, sep, NK_PORTABLE_MARKER) >=
        (int)sizeof(marker) || !nk_platform_file_exists(marker)) {
        return out->state;
    }
    out->state = NK_PORTABLE_REFUSED;
    /* The override appends "/config", "/cache" and so on, and platform lookups
       need room for those below the root. */
    if (snprintf(out->root, sizeof(out->root), "%s%c%s", folder, sep, NK_PORTABLE_DATA_LEAF) >=
        (int)sizeof(out->root) - 64) {
        out->root[0] = '\0';
        snprintf(out->message, sizeof(out->message),
                 "[PORTABLE_PATH_TOO_LONG] %s is next to the program in %s, but that folder's "
                 "path is too long to keep data under it. Move Nakagawa to a shorter folder, "
                 "or delete %s to use the normal per-user data folder.",
                 NK_PORTABLE_MARKER, folder, NK_PORTABLE_MARKER);
        return out->state;
    }
    if (!nk_platform_mkdir_p_private(out->root) || !nk_portable_dir_writable(out->root)) {
        snprintf(out->message, sizeof(out->message),
                 "[PORTABLE_NOT_WRITABLE] %s is next to the program in %s, so Nakagawa keeps "
                 "its data in %s, but that folder cannot be written. Nothing was saved "
                 "anywhere else. Move Nakagawa to a folder you can write to, or delete %s "
                 "to use the normal per-user data folder.",
                 NK_PORTABLE_MARKER, folder, out->root, NK_PORTABLE_MARKER);
        return out->state;
    }
    out->state = NK_PORTABLE_ACTIVE;
    return out->state;
}

/* nk_portable_resolve for this process's own executable, and on ACTIVE point the
 * platform's per-user folders at the portable root. Call before anything resolves a
 * per-user folder. A host that cannot name its executable has no portable mode. */
static inline NkPortableState nk_portable_apply(NkPortableResolution *out) {
    char exe[NK_MAX_PATH];
    if (!nk_platform_get_executable_path(exe, sizeof(exe))) {
        if (out) memset(out, 0, sizeof(*out));
        return NK_PORTABLE_ABSENT;
    }
    if (nk_portable_resolve(exe, out) != NK_PORTABLE_ACTIVE) return out->state;
    if (!nk_platform_set_app_data_dir_override(out->root)) {
        out->state = NK_PORTABLE_REFUSED;
        snprintf(out->message, sizeof(out->message),
                 "[PORTABLE_PATH_TOO_LONG] The portable data folder %s could not be used. "
                 "Move Nakagawa to a shorter folder.", out->root);
    }
    return out->state;
}

/* Whether this process runs in portable mode: the marker sits beside the executable
 * and the platform's data folder is the portable root. Answers the About page. */
static inline bool nk_portable_in_use(void) {
    char exe[NK_MAX_PATH];
    char root[NK_MAX_PATH];
    char marker[NK_MAX_PATH];
    char data[NK_MAX_PATH];
    size_t folder_length;
    if (!nk_platform_get_executable_path(exe, sizeof(exe)) ||
        !nk_platform_resolve_app_data_dir(data, sizeof(data))) {
        return false;
    }
    folder_length = nk_portable_folder_length(exe);
    if (folder_length == 0u || folder_length >= sizeof(root)) return false;
    memcpy(root, exe, folder_length);
    root[folder_length] = '\0';
    if (snprintf(marker, sizeof(marker), "%s%c%s", root, nk_platform_path_separator(),
                 NK_PORTABLE_MARKER) >= (int)sizeof(marker) ||
        !nk_platform_file_exists(marker)) {
        return false;
    }
    if (snprintf(root + folder_length, sizeof(root) - folder_length, "%c%s",
                 nk_platform_path_separator(), NK_PORTABLE_DATA_LEAF) >=
        (int)(sizeof(root) - folder_length)) {
        return false;
    }
    return strcmp(root, data) == 0;
}

#endif /* NK_PORTABLE_H */
