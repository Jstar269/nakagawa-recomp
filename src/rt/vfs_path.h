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

/* Windows reserved-device component check.
 *
 * A guest-supplied single component is unsafe when its extension-stripped base
 * names a DOS reserved device: CON, PRN, AUX, NUL, COM1-9, LPT1-9. Win32 path
 * normalization resolves such a component in every DOS namespace, not the
 * filesystem, so "NUL" or "COM5" would silently bypass containment while still
 * succeeding. Edge semantics (F114-5): only those exact bases are reserved --
 * supersets ("CONSOLE", "COMMON", "NULL") and "COM0"/"LPT0" are ordinary
 * filenames on every Windows this runtime supports; an extension does NOT save
 * a reserved base because classic APIs strip it ("NUL.txt" -> device NUL).
 * The \\?\-prefixed writable VFS route does not treat devices specially, so
 * this rejection matters exactly where classic paths are used (savedata). */
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
        size_t dlen = strlen(devices[i]);
        if (dlen == base_len && sr_vfs_strnicmp(name, devices[i], base_len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* One savedata filename/component. Rejects everything Win32 would otherwise
 * reinterpret: separators (component split must happen before this runs),
 * ADS colons, wildcards, redirect metacharacters, control bytes, trailing
 * dot/space (classic normalization silently strips both, aliasing the name to
 * a different file) and reserved device bases. */
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
 * - Rejects a ".." path *component* (directory traversal), returning 0. A literal ".."
 *   between separators (or at either end) escapes the root and is refused; ".." embedded
 *   inside a name ("file..bak", "...") is an ordinary filename and is allowed.
 * - Also rejects per-component NTFS ADS syntax ("name:stream"), wildcards,
 *   redirect/control characters and DOS reserved-device components, so no
 *   guest byte can select a stream, device, or alias namespace downstream.
 * - Collapses repeated separators so "//" cannot smuggle an UNC prefix through.
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
        size_t comp_len = (size_t)(c - start);
        if (comp_len == 2u && start[0] == '.' && start[1] == '.') return 0;
        for (size_t i = 0; i < comp_len; i++) {
            unsigned char ch = (unsigned char)start[i];
            if (ch < 0x20 || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
                return 0;
            }
        }
        if (sr_vfs_is_dos_device_name(start, comp_len)) return 0;
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

#ifndef FILE_DISPOSITION_FLAG_DELETE
#define FILE_DISPOSITION_FLAG_DELETE 0x00000001
#endif
#ifndef FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x00000002
#endif

/* Canonical root identity (Windows).
 *
 * Resolves the configured storage root to its final absolute path ONCE, by
 * handle, and appends a trailing separator so every later containment check is
 * a prefix compare against that one canonical string. Every operation that
 * touches guest-named paths must then follow the same order:
 *
 *     OPEN -> HANDLE -> FINAL PATH VERIFY -> OPERATION
 *
 * A path that merely *looks* inside the root proves nothing on Windows: a
 * pre-planted NTFS junction or symlink re-resolves to an arbitrary target when
 * any classic API walks it. Only the final path of an open HANDLE reflects
 * where the object actually lives, and only holding that handle keeps the
 * verification and the operation pinned to the same object.
 *
 * F114-3: every buffer here is fixed (MAX_PATH*2 wchar). GetFinalPathNameByHandleW
 * reports the required length; anything longer fails CLOSED (the operation is
 * refused), never falls back to an unverified by-name call.
 *
 * F114-4: sr_vfs_canonical_root intentionally creates the bare root directory
 * if it is missing -- utility dialogs expect the memstick hierarchy to exist,
 * and this is the single documented creation side effect. Nothing deeper than
 * the root is ever created by this helper.
 */
static inline int sr_vfs_canonical_root(const char *root_utf8, wchar_t *out, size_t cap) {
    if (!root_utf8 || !out || cap == 0) return 0;
    int need = MultiByteToWideChar(CP_UTF8, 0, root_utf8, -1, NULL, 0);
    if (need <= 0 || (size_t)need > cap) return 0;
    wchar_t *wroot = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wroot) return 0;
    MultiByteToWideChar(CP_UTF8, 0, root_utf8, -1, wroot, need);
    for (wchar_t *p = wroot; *p; p++) if (*p == L'/') *p = L'\\';

    CreateDirectoryW(wroot, NULL);   /* documented F114-4 side effect: bare root only */
    HANDLE h = CreateFileW(wroot, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wroot);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD len = GetFinalPathNameByHandleW(h, out, (DWORD)cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    /* len >= cap means the final name does not fit: fail closed rather than
     * verify against a truncated prefix (F114-3). */
    if (len == 0 || len >= cap) return 0;

    if (out[len - 1] != L'\\') {
        if (len + 2 > cap) return 0;
        out[len] = L'\\';
        out[len + 1] = L'\0';
    }
    return 1;
}

/* Final-path containment for an already-open handle. The canonical root always
 * ends in L'\\', so matching that full prefix inherently aligns on a component
 * boundary: "...\\foo\\" can never prefix-match "...\\foobar", whose next byte
 * after the shared run is 'b', not the root's trailing separator. */
static inline int sr_vfs_handle_is_contained(HANDLE h, const wchar_t *canonical_root) {
    if (h == INVALID_HANDLE_VALUE || !canonical_root) return 0;
    wchar_t final_path[MAX_PATH * 2];
    DWORD len = GetFinalPathNameByHandleW(h, final_path, (DWORD)(sizeof(final_path)/sizeof(wchar_t)),
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0 || len >= sizeof(final_path)/sizeof(final_path[0])) return 0;
    size_t root_len = wcslen(canonical_root);
    if (root_len == 0 || canonical_root[root_len - 1] != L'\\') return 0;
    /* The root itself: final name is the root without its trailing separator. */
    if (len == root_len - 1) return _wcsnicmp(final_path, canonical_root, len) == 0;
    if (len < root_len) return 0;
    /* Prefix spans the trailing separator, so children are boundary-aligned. */
    return _wcsnicmp(final_path, canonical_root, root_len) == 0;
}

/* Wide-path directory containment: opens WITHOUT FILE_FLAG_OPEN_REPARSE_POINT,
 * so a junction anywhere in the path resolves to its target's final path and is
 * rejected whenever that target lies outside the root. */
static inline int sr_vfs_dir_is_contained_wide(const wchar_t *dir, const wchar_t *canonical_root) {
    if (!dir || !canonical_root) return 0;
    HANDLE h = CreateFileW(dir, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = sr_vfs_handle_is_contained(h, canonical_root);
    CloseHandle(h);
    return ok;
}

/* UTF-8 convenience wrapper around sr_vfs_dir_is_contained_wide. */
static inline int sr_vfs_dir_is_contained(const char *dir_utf8, const wchar_t *canonical_root) {
    if (!dir_utf8 || !canonical_root) return 0;
    int need = MultiByteToWideChar(CP_UTF8, 0, dir_utf8, -1, NULL, 0);
    if (need <= 0) return 0;
    wchar_t *wdir = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wdir) return 0;
    MultiByteToWideChar(CP_UTF8, 0, dir_utf8, -1, wdir, need);
    for (wchar_t *p = wdir; *p; p++) if (*p == L'/') *p = L'\\';

    int ok = sr_vfs_dir_is_contained_wide(wdir, canonical_root);
    free(wdir);
    return ok;
}

/* OPEN -> HANDLE -> FINAL PATH VERIFY for one file/object path. On success the
 * caller owns *out and MUST close it; the operation happens through this very
 * handle, never by re-resolving the name. flags selects the namespace behaviour:
 * pass FILE_FLAG_OPEN_REPARSE_POINT to stay on a link OBJECT itself (delete /
 * attribute work), omit it to resolve through the link (read/write targets).
 * disposition is a CreateFileW creation disposition (OPEN_EXISTING for reads,
 * OPEN_ALWAYS for write-or-create); the containment verdict always applies to
 * whatever object the disposition produced, before any byte moves. */
static inline int sr_vfs_open_contained_utf8(const char *path_utf8, DWORD desired_access,
                                             DWORD flags, DWORD disposition,
                                             const wchar_t *canonical_root, HANDLE *out) {
    if (!path_utf8 || !canonical_root || !out) return 0;
    *out = NULL;
    int need = MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, NULL, 0);
    if (need <= 0) return 0;
    wchar_t *wpath = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wpath) return 0;
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath, need);
    for (wchar_t *p = wpath; *p; p++) if (*p == L'/') *p = L'\\';

    HANDLE h = CreateFileW(wpath, desired_access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, disposition, flags, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!sr_vfs_handle_is_contained(h, canonical_root)) {
        CloseHandle(h);
        return 0;
    }
    *out = h;
    return 1;
}

/* Delete THROUGH a verified handle (F114-1). Prefers POSIX semantics
 * (FileDispositionInfoEx): the name disappears immediately and the deletion can
 * never be redirected because no by-name syscall ever runs after verification.
 * Pre-1709 Windows falls back to FileDispositionInfo: deletion is still bound to
 * this exact verified handle (nothing re-resolves the path), but the name only
 * vanishes at CloseHandle -- that visibility window is the documented residual.
 * Returns 1 when the disposition was accepted. */
static inline int sr_vfs_dispose_by_handle(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE) return 0;
#if defined(FILE_DISPOSITION_INFO_EX)
    {
        FILE_DISPOSITION_INFO_EX ex;
        ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
        if (SetFileInformationByHandle(h, FileDispositionInfoEx, &ex, sizeof(ex))) return 1;
        DWORD err = GetLastError();
        if (err != ERROR_INVALID_PARAMETER && err != ERROR_CALL_NOT_IMPLEMENTED &&
            err != ERROR_NOT_SUPPORTED) {
            return 0;
        }
    }
#endif
    {
        FILE_DISPOSITION_INFO legacy;
        legacy.DeleteFile = TRUE;
        return SetFileInformationByHandle(h, FileDispositionInfo, &legacy, sizeof(legacy)) ? 1 : 0;
    }
}

/* Contained delete of one leaf (file or link). Directories are refused: a save
 * directory's entries are removed individually, and a planted junction must
 * fail closed exactly like the old S_ISDIR check did -- never silently unlink.
 * The handle is opened with FILE_FLAG_OPEN_REPARSE_POINT so the disposition can
 * only ever affect the object named inside the root, not a link target. */
static inline int sr_vfs_delete_contained_leaf(const char *path_utf8, const wchar_t *canonical_root,
                                               int *was_dir) {
    if (was_dir) *was_dir = 0;
    HANDLE h;
    /* FILE_FLAG_BACKUP_SEMANTICS is required to open a directory at all and is
     * harmless for files; FILE_FLAG_OPEN_REPARSE_POINT pins the link object. */
    if (!sr_vfs_open_contained_utf8(path_utf8, DELETE | FILE_READ_ATTRIBUTES,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                    OPEN_EXISTING, canonical_root, &h)) {
        return 0;
    }
    BY_HANDLE_FILE_INFORMATION info;
    int ok = 0;
    if (GetFileInformationByHandle(h, &info)) {
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (was_dir) *was_dir = 1;      /* refuse: fail closed on directories/links */
        } else {
            ok = sr_vfs_dispose_by_handle(h);
        }
    }
    CloseHandle(h);
    return ok;
}

/* Ordered create of every missing component BELOW the canonical root (F114-2).
 *
 * `owned_rel_utf8` names ONLY the tree below the root (the caller splits at the
 * configured root boundary). Every RAW segment must pass the component rules
 * BEFORE any Win32 API sees it: GetFullPathNameW-style normalization silently
 * rewrites "bad." into "bad" and would otherwise erase the evidence. Creation
 * then extends a buffer that STARTS as the verified canonical root, so nothing
 * can be created outside the root even while the walk is in progress:
 *   exists as a REPARSE POINT -> reject BEFORE creating anything deeper
 *   exists                    -> verify by handle, keep walking
 *   missing                   -> create, verify by handle, continue
 * A pre-planted junction therefore causes rejection before any out-of-root
 * creation. Residual swap race (a link planted between two verifications) is
 * bounded by per-level verification and documented in the PR. Buffer growth
 * beyond MAX_PATH*2 fails closed (F114-3). */
static inline int sr_vfs_mkdirs_contained(const char *owned_rel_utf8,
                                          const wchar_t *canonical_root) {
    if (!owned_rel_utf8 || !canonical_root) return 0;
    size_t root_len = wcslen(canonical_root);
    if (root_len == 0 || canonical_root[root_len - 1] != L'\\') return 0;

    wchar_t partial[MAX_PATH * 2];
    if (root_len + 1u > sizeof(partial)/sizeof(partial[0])) return 0;
    memcpy(partial, canonical_root, root_len * sizeof(wchar_t));
    size_t plen = root_len;

    const char *p = owned_rel_utf8;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t comp_len = (size_t)(p - start);

        char narrow[MAX_PATH];
        if (comp_len == 0 || comp_len >= sizeof(narrow)) return 0;
        memcpy(narrow, start, comp_len);
        narrow[comp_len] = '\0';
        if (!sr_vfs_is_safe_component(narrow, comp_len)) return 0;

        int need = MultiByteToWideChar(CP_UTF8, 0, narrow, -1, NULL, 0);
        if (need <= 0) return 0;
        size_t cap_w = sizeof(partial)/sizeof(partial[0]);
        /* The root buffer already ends with a separator; keep exactly one. */
        if (plen + 1u + (size_t)need > cap_w) return 0;
        if (plen == 0 || partial[plen - 1] != L'\\') {
            partial[plen++] = L'\\';
        }
        MultiByteToWideChar(CP_UTF8, 0, narrow, -1, partial + plen, need);
        plen += (size_t)need - 1u;

        DWORD attrs = GetFileAttributesW(partial);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(partial, NULL)) return 0;
        } else if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            return 0;                     /* pre-planted link: reject before descending */
        }
        if (!sr_vfs_dir_is_contained_wide(partial, canonical_root)) return 0;
    }
    return 1;
}

#endif /* _WIN32 */

#endif /* SR_VFS_PATH_H */
