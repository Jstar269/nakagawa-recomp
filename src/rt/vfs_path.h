// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#ifndef SR_VFS_PATH_H
#define SR_VFS_PATH_H

#include <stddef.h>
#include <string.h>

/* Host-neutral VFS path join helper (Issue #19).
 *
 * Concatenates `root` directory and relative `guest` path:
 * - Strips leading device specifier (e.g. "ms0:") and leading slashes/backslashes from `guest`.
 * - Rejects a ".." path *component* (directory traversal), returning 0. A literal ".."
 *   between separators (or at either end) escapes the root and is refused; ".." embedded
 *   inside a name ("file..bak", "...") is an ordinary filename and is allowed.
 * - Ensures exactly one separator ('/' or '\\') between `root` and non-empty `guest`.
 * - Does not introduce doubled separators if `root` already has a trailing slash/backslash.
 * - Converts guest slashes to host separators (`sep`).
 * - Returns the resulting length on success (> 0), or 0 on error / overflow.
 */
static inline int sr_vfs_host_dir_path(const char *root, const char *guest, char *out, size_t max, char sep) {
    if (!root || !guest || !out || max == 0) return 0;

    const char *p = strchr(guest, ':');
    if (p) p++;
    else p = guest;
    while (*p == '/' || *p == '\\') p++;

    /* Reject only a ".." *component* -- a maximal run of non-separator characters that
     * equals exactly "..". This is what escapes the root; "..' embedded in a longer name
     * (e.g. "file..bak") stays inside the directory and is a legal PSP filename, so an
     * earlier substring test (strstr(p, "..")) rejected valid names it should have kept.
     * Both '/' and '\\' delimit components; the guest may use either. */
    for (const char *c = p; *c != '\0'; ) {
        const char *start = c;
        while (*c != '\0' && *c != '/' && *c != '\\') c++;
        if ((size_t)(c - start) == 2u && start[0] == '.' && start[1] == '.') return 0;
        while (*c == '/' || *c == '\\') c++;
    }

    size_t root_len = strlen(root);
    if (root_len >= max) return 0;
    memcpy(out, root, root_len);
    size_t n = root_len;

    if (*p != '\0' && n > 0 && n < max - 1) {
        char last = out[n - 1];
        if (last != '/' && last != '\\') {
            out[n++] = sep;
        }
    }

    while (*p != '\0' && n < max - 1) {
        char c = *p++;
        if (c == '/' || c == '\\') c = sep;
        out[n++] = c;
    }
    if (*p != '\0') {
        out[0] = '\0';
        return 0;
    }
    out[n] = '\0';
    return (int)n;
}

/* Host-neutral flat-name mapping for the writable host-backed IoFileMgr route
 * (hle.c host_path_alloc; counterpart to sr_vfs_host_dir_path).
 *
 * The writable storage route does not mirror the guest directory tree: every
 * guest path is flattened into ONE host filename directly beneath `root`, so
 * no guest byte can select a host directory, drive, or device. The mapping is
 * exactly '/' '\\' ':' and ' ' -> '_'; every other byte passes through.
 *
 * The only guest strings whose flattened form is still a directory reference
 * are "", "." and ".." (they contain no mappable byte, so their flat form
 * would name the root itself or its parent). Those are rejected outright;
 * any other input yields a single component that cannot traverse above root.
 *
 * Returns the resulting length on success (> 0), or 0 on rejection / overflow.
 */
static inline int sr_vfs_host_flat_path(const char *root, const char *guest, char *out, size_t max) {
    if (!root || !guest || !out || max == 0) return 0;
    size_t guest_len = strlen(guest);
    if (guest_len == 0) return 0;
    if (guest_len == 1u && guest[0] == '.') return 0;
    if (guest_len == 2u && guest[0] == '.' && guest[1] == '.') return 0;

    size_t root_len = strlen(root);
    if (root_len >= max) return 0;
    memcpy(out, root, root_len);
    size_t n = root_len;

    if (n > 0 && n < max - 1) {
        char last = out[n - 1];
        if (last != '/' && last != '\\') out[n++] = '/';
    }

    for (size_t i = 0; i < guest_len; i++) {
        if (n >= max - 1) {
            out[0] = '\0';
            return 0;
        }
        char c = guest[i];
        out[n++] = (c == '/' || c == ':' || c == '\\' || c == ' ') ? '_' : c;
    }
    out[n] = '\0';
    return (int)n;
}

#endif /* SR_VFS_PATH_H */
