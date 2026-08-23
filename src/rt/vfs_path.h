// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#ifndef SR_VFS_PATH_H
#define SR_VFS_PATH_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define sr_vfs_strnicmp(a, b, n) _strnicmp((a), (b), (n))
#else
#include <strings.h>
#define sr_vfs_strnicmp(a, b, n) strncasecmp((a), (b), (n))
#endif

static inline int sr_vfs_is_dos_device_name(const char *name, size_t len) {
    static const char *const devices[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    size_t base_len = 0;
    while (base_len < len && name[base_len] != '.') base_len++;
    if (base_len < 3 || base_len > 4) return 0;
    for (size_t i = 0; i < sizeof(devices)/sizeof(devices[0]); i++) {
        if (sr_vfs_strnicmp(name, devices[i], base_len) == 0 && devices[i][base_len] == '\0') {
            return 1;
        }
    }
    return 0;
}

static inline int sr_vfs_is_safe_component(const char *name, size_t len) {
    if (!name || len == 0) return 0;
    if (len == 1 && name[0] == '.') return 0;
    if (len == 2 && name[0] == '.' && name[1] == '.') return 0;
    if (name[len - 1] == '.' || name[len - 1] == ' ') return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return 0;
        }
    }
    if (sr_vfs_is_dos_device_name(name, len)) return 0;
    return 1;
}

/* Host-neutral VFS path join helper (Issue #19).
 *
 * Concatenates `root` directory and relative `guest` path:
 * - Strips leading device specifier (e.g. "ms0:") and leading slashes/backslashes from `guest`.
 * - Rejects directory traversal (".."), ADS (":"), DOS device names, wildcards, and trailing dots/spaces.
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

    for (const char *c = p; *c != '\0'; ) {
        const char *start = c;
        while (*c != '\0' && *c != '/' && *c != '\\') c++;
        size_t comp_len = (size_t)(c - start);
        if (comp_len == 2u && start[0] == '.' && start[1] == '.') return 0;
        for (size_t i = 0; i < comp_len; i++) {
            unsigned char ch = (unsigned char)start[i];
            if (ch < 0x20 || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
                return 0;
            }
        }
        if (comp_len > 0 && sr_vfs_is_dos_device_name(start, comp_len)) return 0;
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
        if (c == '/' || c == '\\') {
            while (*p == '/' || *p == '\\') p++;
            if (*p == '\0') break;
            c = sep;
        }
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

#ifdef _WIN32
static inline int sr_vfs_canonical_root(const char *root_utf8, wchar_t *out, size_t cap) {
    if (!root_utf8 || !out || cap == 0) return 0;
    int need = MultiByteToWideChar(CP_UTF8, 0, root_utf8, -1, NULL, 0);
    if (need <= 0 || (size_t)need > cap) return 0;
    wchar_t *wroot = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wroot) return 0;
    MultiByteToWideChar(CP_UTF8, 0, root_utf8, -1, wroot, need);
    for (wchar_t *p = wroot; *p; p++) if (*p == L'/') *p = L'\\';

    CreateDirectoryW(wroot, NULL);
    HANDLE h = CreateFileW(wroot, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wroot);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD len = GetFinalPathNameByHandleW(h, out, (DWORD)cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (len == 0 || len >= cap) return 0;

    if (out[len - 1] != L'\\') {
        if (len + 2 > cap) return 0;
        out[len] = L'\\';
        out[len + 1] = L'\0';
    }
    return 1;
}

static inline int sr_vfs_handle_is_contained(HANDLE h, const wchar_t *canonical_root) {
    if (h == INVALID_HANDLE_VALUE || !canonical_root) return 0;
    wchar_t final_path[MAX_PATH * 2];
    DWORD len = GetFinalPathNameByHandleW(h, final_path, (DWORD)(sizeof(final_path)/sizeof(wchar_t)),
                                         FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0) return 0;
    size_t root_len = wcslen(canonical_root);
    if (len + 1 == root_len && canonical_root[root_len - 1] == L'\\') {
        return _wcsnicmp(final_path, canonical_root, len) == 0;
    }
    if (len < root_len) return 0;
    if (_wcsnicmp(final_path, canonical_root, root_len) != 0) {
        return 0;
    }
    return 1;
}

static inline int sr_vfs_dir_is_contained(const char *dir_utf8, const wchar_t *canonical_root) {
    if (!dir_utf8 || !canonical_root) return 0;
    int need = MultiByteToWideChar(CP_UTF8, 0, dir_utf8, -1, NULL, 0);
    if (need <= 0) return 0;
    wchar_t *wdir = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wdir) return 0;
    MultiByteToWideChar(CP_UTF8, 0, dir_utf8, -1, wdir, need);
    for (wchar_t *p = wdir; *p; p++) if (*p == L'/') *p = L'\\';

    HANDLE h = CreateFileW(wdir, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wdir);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = sr_vfs_handle_is_contained(h, canonical_root);
    CloseHandle(h);
    return ok;
}
#endif

#endif /* SR_VFS_PATH_H */
