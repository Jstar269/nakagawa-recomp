/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#if defined(_WIN32) || defined(_WIN64)

#include "nk_platform.h"
#include "nk_types.h"
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int nk_fseek64(FILE *f, int64_t offset, int whence) {
    return _fseeki64(f, offset, whence);
}

int64_t nk_ftell64(FILE *f) {
    return _ftelli64(f);
}

char nk_platform_path_separator(void) {
    return '\\';
}

/* Bounded UTF-8 -> UTF-16 conversion */
static bool utf8_to_wide(const char *utf8, WCHAR *out_wide, size_t max_wide_chars) {
    if (!utf8 || !out_wide || max_wide_chars == 0) return false;
    int res = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out_wide, (int)max_wide_chars);
    if (res <= 0) {
        out_wide[0] = L'\0';
        return false;
    }
    return true;
}

/* Bounded UTF-16 -> UTF-8 conversion; unpaired surrogates are refused. */
static bool wide_to_utf8(const WCHAR *wide, char *out_utf8, size_t max_utf8_bytes) {
    if (!wide || !out_utf8 || max_utf8_bytes == 0) return false;
    int res = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out_utf8,
                                  (int)max_utf8_bytes, NULL, NULL);
    if (res <= 0) {
        out_utf8[0] = '\0';
        return false;
    }
    return true;
}

FILE *nk_fopen_utf8(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
    WCHAR wpath[32768];
    WCHAR wmode[32];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(wpath[0])) ||
        !utf8_to_wide(mode, wmode, sizeof(wmode) / sizeof(wmode[0]))) return NULL;
    return _wfopen(wpath, wmode);
}

int nk_remove_utf8(const char *path) {
    if (!path || !*path) return -1;
    WCHAR wpath[32768];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(wpath[0]))) return -1;
    return DeleteFileW(wpath) ? 0 : -1;
}

int nk_rename_utf8(const char *from, const char *to) {
    if (!from || !*from || !to || !*to) return -1;
    WCHAR wfrom[32768];
    WCHAR wto[32768];
    if (!utf8_to_wide(from, wfrom, sizeof(wfrom) / sizeof(wfrom[0])) ||
        !utf8_to_wide(to, wto, sizeof(wto) / sizeof(wto[0]))) return -1;
    return MoveFileExW(wfrom, wto,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

bool nk_platform_file_exists(const char *path) {
    if (!path || !*path) return false;
    WCHAR wpath[32768];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(WCHAR))) return false;
    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool nk_platform_dir_exists(const char *path) {
    if (!path || !*path) return false;
    WCHAR wpath[32768];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(WCHAR))) return false;
    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool nk_platform_path_missing(const char *path) {
    if (!path || !*path) return false;
    WCHAR wpath[32768];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(WCHAR))) return false;
    if (GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) return false;
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

#define TREE_PATH_CAP 32768u

/* A tree walk keeps one path buffer: each level appends "\name", works on it
 * and cuts it back, so a deep tree costs a find record per level, not a path. */
typedef struct {
    WCHAR *path;
    size_t length;
} TreePath;

static bool tree_path_push(TreePath *tree, const WCHAR *name, size_t *saved) {
    size_t name_length = wcslen(name);
    if (tree->length + 1u + name_length >= TREE_PATH_CAP) return false;
    *saved = tree->length;
    tree->path[tree->length++] = L'\\';
    memcpy(tree->path + tree->length, name, (name_length + 1u) * sizeof(WCHAR));
    tree->length += name_length;
    return true;
}

static void tree_path_pop(TreePath *tree, size_t saved) {
    tree->length = saved;
    tree->path[saved] = L'\0';
}

/* The walk works on the extended-length form (the long-path prefix before a drive or UNC
 * path), so a
 * tree deeper than MAX_PATH is listed and removed without long-path support in
 * the process manifest or the registry (a default Windows install has
 * neither). That form takes backslashes only and a full path. */
static bool tree_path_extend(TreePath *tree) {
    static const WCHAR prefix[] = L"\\\\?\\";
    static const WCHAR unc_prefix[] = L"\\\\?\\UNC\\";
    WCHAR *full;
    DWORD full_length;
    size_t i;
    if (wcsncmp(tree->path, prefix, 4) == 0) return true;
    for (i = 0; tree->path[i]; ++i) {
        if (tree->path[i] == L'/') tree->path[i] = L'\\';
    }
    full = (WCHAR *)malloc(TREE_PATH_CAP * sizeof(WCHAR));
    if (!full) return false;
    full_length = GetFullPathNameW(tree->path, TREE_PATH_CAP, full, NULL);
    if (full_length == 0 || full_length >= TREE_PATH_CAP - 8u) {
        free(full);
        return false;
    }
    if (full[0] == L'\\' && full[1] == L'\\') {
        /* A network path takes the UNC form of the prefix. */
        if (wcslen(unc_prefix) + wcslen(full + 2) + 1u > TREE_PATH_CAP) {
            free(full);
            return false;
        }
        wcscpy(tree->path, unc_prefix);
        wcscat(tree->path, full + 2);
    } else {
        wcscpy(tree->path, prefix);
        wcscat(tree->path, full);
    }
    free(full);
    tree->length = wcslen(tree->path);
    while (tree->length > 7u && tree->path[tree->length - 1u] == L'\\') {
        tree->path[--tree->length] = L'\0';
    }
    return true;
}

static bool tree_name_is_dot(const WCHAR *name) {
    return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

/* Lists the directory at tree->path; GetLastError() holds the failure. */
static HANDLE tree_find_first(TreePath *tree, WIN32_FIND_DATAW *data) {
    size_t saved;
    if (!tree_path_push(tree, L"*", &saved)) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE find = FindFirstFileW(tree->path, data);
    DWORD error = GetLastError();
    tree_path_pop(tree, saved);
    SetLastError(error);
    return find;
}

static bool tree_size_at(TreePath *tree, void *ctx) {
    uint64_t *bytes = (uint64_t *)ctx;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(tree->path, GetFileExInfoStandard, &info)) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return true;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        *bytes += ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        return true;
    }
    WIN32_FIND_DATAW data;
    HANDLE find = tree_find_first(tree, &data);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
        size_t saved;
        if (tree_name_is_dot(data.cFileName)) continue;
        if (!tree_path_push(tree, data.cFileName, &saved)) {
            ok = false;
            break;
        }
        ok = tree_size_at(tree, ctx);
        tree_path_pop(tree, saved);
    } while (ok && FindNextFileW(find, &data));
    FindClose(find);
    return ok;
}

static bool tree_remove_at(TreePath *tree, void *ctx) {
    (void)ctx;
    DWORD attributes = GetFileAttributesW(tree->path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) {
        (void)SetFileAttributesW(tree->path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    }
    /* A junction or directory symlink goes as itself; its target stays. */
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(tree->path) != 0
                                                       : DeleteFileW(tree->path) != 0;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(tree->path) != 0;
    /* Subfolders first, then the files beside them: a removal that stops in a
       subfolder keeps this folder's own files, such as the package cache's
       ownership marker, so what is left is still recognised. */
    for (int pass = 0; pass < 2; ++pass) {
        WIN32_FIND_DATAW data;
        HANDLE find = tree_find_first(tree, &data);
        if (find == INVALID_HANDLE_VALUE) continue;
        bool ok = true;
        do {
            size_t saved;
            bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (tree_name_is_dot(data.cFileName)) continue;
            if (directory != (pass == 0)) continue;
            if (!tree_path_push(tree, data.cFileName, &saved)) {
                ok = false;
                break;
            }
            ok = tree_remove_at(tree, NULL);
            tree_path_pop(tree, saved);
        } while (ok && FindNextFileW(find, &data));
        FindClose(find);
        if (!ok) return false;
    }
    return RemoveDirectoryW(tree->path) != 0;
}

/* Runs fn on path, or with skip_child set on each top-level child but that one. */
static bool tree_walk_top(const char *path, const char *skip_child,
                          bool (*fn)(TreePath *, void *), void *ctx, bool *kept) {
    if (kept) *kept = false;
    if (!path || !*path) return false;
    TreePath tree;
    tree.path = (WCHAR *)malloc(TREE_PATH_CAP * sizeof(WCHAR));
    if (!tree.path) return false;
    bool ok = utf8_to_wide(path, tree.path, TREE_PATH_CAP);
    if (ok) {
        tree.length = wcslen(tree.path);
        ok = tree_path_extend(&tree);
    }
    if (ok) {
        DWORD attributes = GetFileAttributesW(tree.path);
        WCHAR wskip[1024];
        if (!skip_child || attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            ok = fn(&tree, ctx);
        } else if (!utf8_to_wide(skip_child, wskip, sizeof(wskip) / sizeof(wskip[0]))) {
            ok = false;
        } else {
            WIN32_FIND_DATAW data;
            HANDLE find = tree_find_first(&tree, &data);
            if (find == INVALID_HANDLE_VALUE) {
                ok = GetLastError() == ERROR_FILE_NOT_FOUND;
            } else {
                do {
                    size_t saved;
                    if (tree_name_is_dot(data.cFileName)) continue;
                    if (_wcsicmp(data.cFileName, wskip) == 0) {
                        if (kept) *kept = true;
                        continue;
                    }
                    if (!tree_path_push(&tree, data.cFileName, &saved)) {
                        ok = false;
                        break;
                    }
                    ok = fn(&tree, ctx);
                    tree_path_pop(&tree, saved);
                } while (ok && FindNextFileW(find, &data));
                FindClose(find);
            }
        }
    }
    free(tree.path);
    return ok;
}

bool nk_platform_tree_size(const char *path, const char *skip_child, uint64_t *out_bytes) {
    if (!out_bytes) return false;
    *out_bytes = 0;
    return tree_walk_top(path, skip_child, tree_size_at, out_bytes, NULL);
}

bool nk_platform_remove_tree(const char *path, const char *skip_child) {
    bool kept = false;
    if (!tree_walk_top(path, skip_child, tree_remove_at, NULL, &kept)) return false;
    if (!skip_child || kept) return true;
    /* Nothing was kept: the emptied directory goes too. */
    return tree_walk_top(path, NULL, tree_remove_at, NULL, NULL);
}

bool nk_platform_list_files(const char *dir, NkDirFileFn fn, void *ctx) {
    if (!dir || !*dir || !fn) return false;
    char pattern[32768];
    int written = snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    if (written < 0 || (size_t)written >= sizeof(pattern)) return false;
    WCHAR wpattern[32768];
    if (!utf8_to_wide(pattern, wpattern, sizeof(wpattern) / sizeof(WCHAR))) return false;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(wpattern, &data);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        char name[1024];
        if (!wide_to_utf8(data.cFileName, name, sizeof(name))) continue;
        if (!fn(name, ctx)) break;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return true;
}

int64_t nk_platform_get_file_size(const char *path) {
    if (!path) return -1;
    WCHAR wpath[32768];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(WCHAR))) return -1;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad)) {
        return -1;
    }
    LARGE_INTEGER size;
    size.LowPart = fad.nFileSizeLow;
    size.HighPart = (LONG)fad.nFileSizeHigh;
    return (int64_t)size.QuadPart;
}

bool nk_platform_mkdir_p(const char *dir_path) {
    if (!dir_path || !*dir_path) return false;
    WCHAR wpath[32768];
    if (!utf8_to_wide(dir_path, wpath, sizeof(wpath) / sizeof(WCHAR))) return false;

    for (WCHAR *p = wpath + 1; *p; p++) {
        if (*p == L'/' || *p == L'\\') {
            WCHAR orig = *p;
            *p = L'\0';
            size_t sublen = wcslen(wpath);
            if (sublen > 2 && wpath[sublen - 1] != L':') {
                CreateDirectoryW(wpath, NULL);
            }
            *p = orig;
        }
    }
    CreateDirectoryW(wpath, NULL);
    DWORD attrs = GetFileAttributesW(wpath);
    return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
}

/* RtlAreLongPathsEnabled (Windows 10 1607 and later) is true only when both the
 * process manifest and the system allow paths past MAX_PATH. */
static bool win32_long_paths_enabled(void) {
    typedef BOOLEAN (WINAPI *LongPathsEnabledFn)(void);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    FARPROC proc = GetProcAddress(ntdll, "RtlAreLongPathsEnabled");
    if (!proc) return false;
    LongPathsEnabledFn enabled;
    memcpy(&enabled, &proc, sizeof(enabled));
    return enabled() != 0;
}

bool nk_platform_path_exceeds_limit(const char *path, bool is_directory) {
    if (!path || !*path) return false;
    WCHAR wpath[32768];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(WCHAR))) return false;
    /* The extended-length form is not held to MAX_PATH. */
    if (wcsncmp(wpath, L"\\\\?\\", 4) == 0) return false;
    if (win32_long_paths_enabled()) return false;
    /* A relative path is held to the limit as the full path it names. */
    DWORD length = GetFullPathNameW(wpath, 0, NULL, NULL);
    if (length == 0) length = (DWORD)wcslen(wpath) + 1u;
    length -= 1u; /* the size it reports counts the terminator */
    /* CreateDirectoryW keeps room for an 8.3 file name inside a new directory. */
    DWORD limit = is_directory ? MAX_PATH - 12u : MAX_PATH;
    return length + 1u > limit;
}

bool nk_platform_mkdir_p_private(const char *dir_path) {
    /* Application-data directories inherit the per-user ACL established by
     * Windows. Keep the same wide-character path handling as mkdir_p so a
     * UTF-8 title path cannot fall back to the ANSI filesystem APIs. */
    return nk_platform_mkdir_p(dir_path);
}

FILE *nk_platform_fopen_private(const char *path, const char *mode) {
    if (!path || !mode || mode[0] != 'w' || strchr(mode, '+') != NULL) return NULL;
    WCHAR wpath[32768];
    WCHAR wmode[32];
    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(wpath[0])) ||
        !utf8_to_wide(mode, wmode, sizeof(wmode) / sizeof(wmode[0]))) return NULL;
    return _wfopen(wpath, wmode);
}

/* Reads an environment variable through the wide API as UTF-8. The narrow CRT
 * getenv() yields active-code-page bytes, which strict UTF-8 path consumers refuse. */
static bool env_get_utf8(const WCHAR *name, char *out_utf8, size_t max_utf8_bytes) {
    WCHAR wvalue[32768];
    DWORD n = GetEnvironmentVariableW(name, wvalue, (DWORD)(sizeof(wvalue) / sizeof(WCHAR)));
    if (n == 0 || n >= sizeof(wvalue) / sizeof(WCHAR)) return false;
    return wide_to_utf8(wvalue, out_utf8, max_utf8_bytes);
}

bool nk_platform_resolve_windows_data_base(
    const char *known_local_app_data,
    const char *local_app_data,
    const char *roaming_app_data,
    char *out_base,
    size_t max_len
) {
    if (!out_base || max_len == 0) return false;
    /* An explicit LOCALAPPDATA wins so launchers, tests and the Python tools
     * (which read the same variable) agree on one root; the Known Folder
     * covers processes started without it. */
    const char *base = local_app_data && local_app_data[0]
        ? local_app_data
        : (known_local_app_data && known_local_app_data[0]
            ? known_local_app_data
            : (roaming_app_data && roaming_app_data[0] ? roaming_app_data : NULL));
    if (!base) return false;
    size_t length = strlen(base);
    if (length >= max_len) return false;
    memcpy(out_base, base, length + 1);
    return true;
}

static bool known_local_app_data_utf8(char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;
    out_path[0] = '\0';
    PWSTR wide_path = NULL;
    HRESULT result = SHGetKnownFolderPath(
        &FOLDERID_LocalAppData, KF_FLAG_DEFAULT, NULL, &wide_path);
    bool resolved = SUCCEEDED(result) && wide_path &&
        wide_to_utf8(wide_path, out_path, max_len);
    if (wide_path) CoTaskMemFree(wide_path);
    return resolved;
}

bool nk_platform_get_desktop_dir(char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;
    out_path[0] = '\0';
    PWSTR wide_path = NULL;
    HRESULT result = SHGetKnownFolderPath(&FOLDERID_Desktop, KF_FLAG_DEFAULT, NULL, &wide_path);
    bool resolved = SUCCEEDED(result) && wide_path &&
        wide_to_utf8(wide_path, out_path, max_len) && nk_platform_dir_exists(out_path);
    if (wide_path) CoTaskMemFree(wide_path);
    if (!resolved) out_path[0] = '\0';
    return resolved;
}

static bool get_windows_data_base(char *out_base, size_t max_len) {
    char known_local_app_data[32768] = "";
    char local_app_data[32768] = "";
    char roaming_app_data[32768] = "";
    (void)known_local_app_data_utf8(known_local_app_data,
                                    sizeof(known_local_app_data));
    (void)env_get_utf8(L"LOCALAPPDATA", local_app_data,
                       sizeof(local_app_data));
    (void)env_get_utf8(L"APPDATA", roaming_app_data,
                       sizeof(roaming_app_data));
    if (nk_platform_resolve_windows_data_base(
            known_local_app_data, local_app_data, roaming_app_data,
            out_base, max_len)) {
        return true;
    }
    static bool reported_unavailable = false;
    if (!reported_unavailable) {
        reported_unavailable = true;
        fprintf(stderr,
            "DATA_DIR_UNAVAILABLE: LOCALAPPDATA is unset and Windows could not resolve "
            "FOLDERID_LocalAppData or APPDATA. USERPROFILE alone is not a data directory.\n");
    }
    return false;
}

static char s_app_data_dir_override[NK_MAX_PATH];

bool nk_platform_set_app_data_dir_override(const char *path) {
    if (!path || !path[0]) {
        s_app_data_dir_override[0] = '\0';
        return true;
    }
    size_t length = strlen(path);
    if (length >= sizeof(s_app_data_dir_override)) return false;
    memcpy(s_app_data_dir_override, path, length + 1u);
    return true;
}

/* With an override, every per-user folder of this process is in it: the data
   folder is the override itself, and settings, cache, logs and saves are its
   config, cache, logs and saves folders. A player opened on another data root
   then never reads or writes the default profile's settings or saves. */
static bool resolve_override_path(NkPathType type, char sep, char *out_path, size_t max_len) {
    const char *leaf = NULL;
    switch (type) {
        case NK_PATH_DATA:   leaf = NULL; break;
        case NK_PATH_CONFIG: leaf = "config"; break;
        case NK_PATH_CACHE:  leaf = "cache"; break;
        case NK_PATH_LOGS:   leaf = "logs"; break;
        case NK_PATH_SAVES:  leaf = "saves"; break;
        default: return false;
    }
    int written = leaf
        ? snprintf(out_path, max_len, "%s%c%s", s_app_data_dir_override, sep, leaf)
        : snprintf(out_path, max_len, "%s", s_app_data_dir_override);
    return written > 0 && (size_t)written < max_len;
}

static bool resolve_windows_path(NkPathType type, char *out_path,
                                 size_t max_len) {
    if (!out_path || max_len == 0) return false;
    if (s_app_data_dir_override[0]) {
        return resolve_override_path(type, '\\', out_path, max_len);
    }
    char base[32768];
    if (!get_windows_data_base(base, sizeof(base))) return false;

    const char *subdir = "data";
    switch (type) {
        case NK_PATH_CONFIG: subdir = "config"; break;
        case NK_PATH_DATA:   subdir = "data"; break;
        case NK_PATH_CACHE:  subdir = "cache"; break;
        case NK_PATH_LOGS:   subdir = "logs"; break;
        case NK_PATH_SAVES:  subdir = "saves"; break;
        default:             subdir = "data"; break;
    }

    int written = snprintf(out_path, max_len, "%s\\Nakagawa\\%s", base, subdir);
    return written > 0 && (size_t)written < max_len;
}

bool nk_platform_get_path(NkPathType type, char *out_path, size_t max_len) {
    if (!resolve_windows_path(type, out_path, max_len)) return false;
    (void)nk_platform_mkdir_p(out_path);
    return true;
}

bool nk_platform_resolve_app_data_dir(char *out_path, size_t max_len) {
    return resolve_windows_path(NK_PATH_DATA, out_path, max_len);
}

bool nk_platform_resolve_path(NkPathType type, char *out_path, size_t max_len) {
    return resolve_windows_path(type, out_path, max_len);
}

bool nk_platform_get_executable_path(char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;
    out_path[0] = '\0';
    WCHAR wide_path[32768];
    DWORD length = GetModuleFileNameW(NULL, wide_path,
                                      (DWORD)(sizeof(wide_path) / sizeof(wide_path[0])));
    if (length == 0 || length >= sizeof(wide_path) / sizeof(wide_path[0]) ||
        !wide_to_utf8(wide_path, out_path, max_len)) {
        out_path[0] = '\0';
        return false;
    }
    return true;
}

bool nk_platform_get_app_data_dir(char *out_path, size_t max_len) {
    return nk_platform_get_path(NK_PATH_DATA, out_path, max_len);
}

bool nk_platform_get_legacy_app_data_dir(char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;
    char profile[32768];
    if (!env_get_utf8(L"USERPROFILE", profile, sizeof(profile))) return false;
    int written = snprintf(out_path, max_len, "%s\\Nakagawa\\data", profile);
    return written > 0 && (size_t)written < max_len;
}

/* Helper to escape arguments for Windows command line according to Microsoft CRT rules */
static void append_escaped_arg(char *buf, size_t buf_len, const char *arg) {
    if (!buf || buf_len == 0 || !arg) return;

    size_t cur_len = strlen(buf);
    if (cur_len > 0) {
        if (cur_len + 1 >= buf_len) return;
        buf[cur_len++] = ' ';
        buf[cur_len] = '\0';
    }

    /* Check if argument needs quoting according to Windows rules:
     * Empty string, spaces, tabs, newlines, or double quotes require quoting. */
    bool needs_quotes = (*arg == '\0');
    for (const char *p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' || *p == '\"') {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes) {
        snprintf(buf + cur_len, buf_len - cur_len, "%s", arg);
        return;
    }

    if (cur_len + 1 >= buf_len) return;
    buf[cur_len++] = '\"';

    const char *p = arg;
    while (*p) {
        size_t num_backslashes = 0;
        while (*p == '\\') {
            num_backslashes++;
            p++;
        }

        if (*p == '\"') {
            /* 2N + 1 backslashes before quote */
            for (size_t i = 0; i < 2 * num_backslashes + 1 && cur_len + 1 < buf_len; i++) {
                buf[cur_len++] = '\\';
            }
            if (cur_len + 1 < buf_len) {
                buf[cur_len++] = '\"';
            }
            p++;
        } else if (*p == '\0') {
            /* 2N backslashes before closing quote */
            for (size_t i = 0; i < 2 * num_backslashes && cur_len + 1 < buf_len; i++) {
                buf[cur_len++] = '\\';
            }
            break;
        } else {
            /* N backslashes before normal character */
            for (size_t i = 0; i < num_backslashes && cur_len + 1 < buf_len; i++) {
                buf[cur_len++] = '\\';
            }
            if (cur_len + 1 < buf_len) {
                buf[cur_len++] = *p;
            }
            p++;
        }
    }

    if (cur_len + 1 < buf_len) {
        buf[cur_len++] = '\"';
    }
    buf[cur_len] = '\0';
}

typedef struct {
    WCHAR *str;
    size_t key_len;
} WEnvVar;

static int env_var_cmp(const void *a, const void *b) {
    const WEnvVar *va = (const WEnvVar *)a;
    const WEnvVar *vb = (const WEnvVar *)b;
    return _wcsicmp(va->str, vb->str);
}

static WCHAR *build_controlled_unicode_environment(const char * const *envp) {
    LPWCH parent_env = GetEnvironmentStringsW();
    if (!parent_env) return NULL;

    size_t capacity = 512;
    WEnvVar *vars = (WEnvVar *)malloc(capacity * sizeof(WEnvVar));
    if (!vars) {
        FreeEnvironmentStringsW(parent_env);
        return NULL;
    }
    size_t count = 0;
    bool failed = false;

    for (LPCWCH p = parent_env; *p; p += wcslen(p) + 1) {
        if (*p == L'=') continue;
        LPCWCH eq = wcschr(p, L'=');
        size_t klen = eq ? (size_t)(eq - p) : wcslen(p);

        if (count >= capacity) {
            capacity *= 2;
            WEnvVar *new_vars = (WEnvVar *)realloc(vars, capacity * sizeof(WEnvVar));
            if (!new_vars) { failed = true; break; }
            vars = new_vars;
        }

        vars[count].str = _wcsdup(p);
        vars[count].key_len = klen;
        if (!vars[count].str) { failed = true; break; }
        count++;
    }
    FreeEnvironmentStringsW(parent_env);

    if (envp && !failed) {
        for (int i = 0; envp[i]; i++) {
            WCHAR witem[4096];
            /* A requested variable that cannot be represented must not silently
             * fall back to the parent's value. */
            if (!utf8_to_wide(envp[i], witem, sizeof(witem) / sizeof(WCHAR))) {
                failed = true;
                break;
            }

            WCHAR *eq = wcschr(witem, L'=');
            size_t klen = eq ? (size_t)(eq - witem) : wcslen(witem);

            int found_idx = -1;
            for (size_t j = 0; j < count; j++) {
                if (vars[j].key_len == klen && _wcsnicmp(vars[j].str, witem, klen) == 0) {
                    found_idx = (int)j;
                    break;
                }
            }

            if (!eq) {
                /* An entry without '=' requests removal of the variable from the child's environment. */
                if (found_idx >= 0) {
                    free(vars[found_idx].str);
                    vars[found_idx] = vars[count - 1];
                    count--;
                }
                continue;
            }

            WCHAR *dup = _wcsdup(witem);
            if (!dup) { failed = true; break; }
            if (found_idx >= 0) {
                free(vars[found_idx].str);
                vars[found_idx].str = dup;
                vars[found_idx].key_len = klen;
            } else {
                if (count >= capacity) {
                    capacity *= 2;
                    WEnvVar *new_vars = (WEnvVar *)realloc(vars, capacity * sizeof(WEnvVar));
                    if (!new_vars) { free(dup); failed = true; break; }
                    vars = new_vars;
                }
                vars[count].str = dup;
                vars[count].key_len = klen;
                count++;
            }
        }
    }

    if (failed) {
        for (size_t i = 0; i < count; i++) free(vars[i].str);
        free(vars);
        return NULL;
    }

    qsort(vars, count, sizeof(WEnvVar), env_var_cmp);

    size_t total_wchars = 1;
    for (size_t i = 0; i < count; i++) {
        if (vars[i].str) {
            total_wchars += wcslen(vars[i].str) + 1;
        }
    }

    WCHAR *block = (WCHAR *)malloc(total_wchars * sizeof(WCHAR));
    if (block) {
        WCHAR *dest = block;
        for (size_t i = 0; i < count; i++) {
            if (vars[i].str) {
                size_t len = wcslen(vars[i].str);
                memcpy(dest, vars[i].str, len * sizeof(WCHAR));
                dest += len;
                *dest++ = L'\0';
            }
        }
        *dest = L'\0';
    }

    for (size_t i = 0; i < count; i++) {
        free(vars[i].str);
    }
    free(vars);

    return block;
}

bool nk_platform_spawn_process(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    const char *output_log_path,
    NkProcessHandle *out_process
) {
    if (!executable_path || !out_process) return false;
    memset(out_process, 0, sizeof(*out_process));

    WCHAR wexec[32768];
    if (!utf8_to_wide(executable_path, wexec, sizeof(wexec) / sizeof(WCHAR))) {
        return false;
    }

    char cmd_line[32768];
    cmd_line[0] = '\0';

    if (argv && argv[0]) {
        for (int i = 0; argv[i]; i++) {
            append_escaped_arg(cmd_line, sizeof(cmd_line), argv[i]);
        }
    } else {
        append_escaped_arg(cmd_line, sizeof(cmd_line), executable_path);
    }

    WCHAR wcmd[32768];
    if (!utf8_to_wide(cmd_line, wcmd, sizeof(wcmd) / sizeof(WCHAR))) {
        return false;
    }

    WCHAR wwd_buf[32768];
    WCHAR *wwd = NULL;
    if (working_directory && *working_directory) {
        /* Never launch in the parent's directory in place of a rejected one. */
        if (!utf8_to_wide(working_directory, wwd_buf, sizeof(wwd_buf) / sizeof(WCHAR))) {
            return false;
        }
        wwd = wwd_buf;
    }

    /* A NULL block would make CreateProcessW inherit the parent environment. */
    WCHAR *wenv_block = build_controlled_unicode_environment(envp);
    if (!wenv_block) return false;

    SECURITY_ATTRIBUTES handle_security;
    memset(&handle_security, 0, sizeof(handle_security));
    handle_security.nLength = sizeof(handle_security);
    handle_security.bInheritHandle = TRUE;

    HANDLE h_null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &handle_security, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (h_null == INVALID_HANDLE_VALUE) {
        free(wenv_block);
        return false;
    }

    HANDLE h_output = h_null;
    if (output_log_path && output_log_path[0]) {
        WCHAR wlog[32768];
        if (!utf8_to_wide(output_log_path, wlog,
                          sizeof(wlog) / sizeof(WCHAR))) {
            CloseHandle(h_null);
            free(wenv_block);
            return false;
        }
        h_output = CreateFileW(wlog, GENERIC_WRITE | FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               &handle_security, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h_output == INVALID_HANDLE_VALUE) {
            CloseHandle(h_null);
            free(wenv_block);
            return false;
        }
    }

    HANDLE inherited_handles[2] = { h_null, h_output };
    DWORD inherited_handle_count = h_output == h_null ? 1u : 2u;
    SIZE_T attribute_list_size = 0;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_list_size);
    if (attribute_list_size == 0) {
        if (h_output != h_null) CloseHandle(h_output);
        CloseHandle(h_null);
        free(wenv_block);
        return false;
    }
    LPPROC_THREAD_ATTRIBUTE_LIST attribute_list =
        (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_list_size);
    if (!attribute_list ||
        !InitializeProcThreadAttributeList(attribute_list, 1, 0,
                                           &attribute_list_size)) {
        free(attribute_list);
        if (h_output != h_null) CloseHandle(h_output);
        CloseHandle(h_null);
        free(wenv_block);
        return false;
    }
    if (!UpdateProcThreadAttribute(attribute_list, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited_handles,
                                   inherited_handle_count * sizeof(HANDLE),
                                   NULL, NULL)) {
        DeleteProcThreadAttributeList(attribute_list);
        free(attribute_list);
        if (h_output != h_null) CloseHandle(h_output);
        CloseHandle(h_null);
        free(wenv_block);
        return false;
    }

    DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                           CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;

    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = h_null;
    si.StartupInfo.hStdOutput = h_output;
    si.StartupInfo.hStdError = h_output;
    si.lpAttributeList = attribute_list;
    memset(&pi, 0, sizeof(pi));

    BOOL success = CreateProcessW(
        wexec,
        wcmd,
        NULL,
        NULL,
        TRUE,
        creation_flags,
        wenv_block,
        wwd,
        &si.StartupInfo,
        &pi
    );
    /* Read the refusal before the cleanup below can reset the thread's last
       error: a security product's block reaches the player only as this code. */
    DWORD create_error = success ? ERROR_SUCCESS : GetLastError();

    DeleteProcThreadAttributeList(attribute_list);
    free(attribute_list);
    if (h_output != h_null) CloseHandle(h_output);
    CloseHandle(h_null);

    if (wenv_block) {
        free(wenv_block);
    }

    if (!success) {
        out_process->spawn_error = (int)create_error;
        return false;
    }

    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    if (!hJob) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    memset(&jeli, 0, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)) ||
        !AssignProcessToJobObject(hJob, pi.hProcess)) {
        CloseHandle(hJob);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    /* Let this user-launched child claim the foreground when its window opens.
       The grant needs the launcher to be the foreground process now, which it
       is only while its own window keeps responding (#1298). */
    AllowSetForegroundWindow(pi.dwProcessId);
    /* The child was created suspended. If its thread cannot be resumed it
       never runs a single instruction, yet its handle still reads as running:
       the player would report a started game that never starts (#1290). */
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        DWORD error = GetLastError();
        fprintf(stderr, "[PLATFORM] ResumeThread failed for process %lu (error %lu); "
                        "the process was stopped\n",
                (unsigned long)pi.dwProcessId, (unsigned long)error);
        TerminateJobObject(hJob, 1);
        CloseHandle(hJob);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }
    CloseHandle(pi.hThread);
    out_process->native_handle = (void *)pi.hProcess;
    out_process->job_handle = (void *)hJob;
    out_process->process_id = (int)pi.dwProcessId;
    out_process->is_active = true;
    return true;
}

#ifndef ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION
#define ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION 4551L
#endif

NkSpawnBlock nk_platform_spawn_block(int os_error) {
    switch ((DWORD)os_error) {
        case ERROR_ACCESS_DENIED:
            return NK_SPAWN_BLOCK_ACCESS_DENIED;
        case ERROR_VIRUS_INFECTED:
        case ERROR_VIRUS_DELETED:
            return NK_SPAWN_BLOCK_THREAT;
        case ERROR_ACCESS_DISABLED_BY_POLICY:
        case ERROR_ACCESS_DISABLED_NO_SAFER_UI_BY_POLICY:
        case ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION:
            return NK_SPAWN_BLOCK_POLICY;
        default:
            return NK_SPAWN_BLOCK_NONE;
    }
}

bool nk_platform_describe_os_error(int os_error, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (os_error == 0) return false;
    WCHAR text[512];
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  NULL, (DWORD)os_error, 0, text,
                                  (DWORD)(sizeof(text) / sizeof(text[0])), NULL);
    while (length > 0 && (text[length - 1] == L'\r' || text[length - 1] == L'\n' ||
                          text[length - 1] == L' ')) {
        text[--length] = L'\0';
    }
    if (length == 0 || !wide_to_utf8(text, out, out_size)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool nk_platform_is_process_running(NkProcessHandle *process) {
    if (!process || !process->native_handle || !process->is_active) return false;
    DWORD res = WaitForSingleObject((HANDLE)process->native_handle, 0);
    if (res == WAIT_TIMEOUT) {
        return true;
    }
    process->is_active = false;
    return false;
}

typedef struct {
    DWORD process_id;
    HWND window;
} NkProcessWindowSearch;

static BOOL CALLBACK nk_find_process_window(HWND window, LPARAM data) {
    NkProcessWindowSearch *search = (NkProcessWindowSearch *)data;
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search->process_id || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != NULL) {
        return TRUE;
    }
    search->window = window;
    return FALSE;
}

bool nk_platform_bring_process_window_to_front(const NkProcessHandle *process) {
    if (!process || !process->is_active || process->process_id <= 0) return false;
    NkProcessWindowSearch search = {(DWORD)process->process_id, NULL};
    EnumWindows(nk_find_process_window, (LPARAM)&search);
    if (!search.window) return false;
    if (GetForegroundWindow() == search.window) return true;
    if (IsIconic(search.window)) ShowWindow(search.window, SW_RESTORE);
    return SetForegroundWindow(search.window) != 0;
}

int nk_platform_wait_process(NkProcessHandle *process, int timeout_ms) {
    if (!process || !process->native_handle) return -1;
    DWORD wait_dur = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD res = WaitForSingleObject((HANDLE)process->native_handle, wait_dur);
    if (res == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess((HANDLE)process->native_handle, &exit_code);
        process->is_active = false;
        return (int)exit_code;
    }
    return -1;
}

bool nk_platform_absolute_path(const char *path, char *out_path, size_t max_len) {
    if (!path || !*path || !out_path || max_len == 0) return false;
    char buffer[MAX_PATH * 2];
    DWORD written = GetFullPathNameA(path, (DWORD)sizeof(buffer), buffer, NULL);
    if (written == 0 || written >= sizeof(buffer) || written >= max_len) return false;

    memcpy(out_path, buffer, (size_t)written + 1);
    return true;
}

void nk_platform_terminate_process(NkProcessHandle *process) {
    if (!process) return;
    if (process->job_handle) {
        TerminateJobObject((HANDLE)process->job_handle, 1);
    }
    if (process->native_handle) {
        TerminateProcess((HANDLE)process->native_handle, 1);
    }
    process->is_active = false;
}

void nk_platform_close_process(NkProcessHandle *process) {
    if (!process) return;
    if (process->job_handle) {
        CloseHandle((HANDLE)process->job_handle);
        process->job_handle = NULL;
    }
    if (process->native_handle) {
        CloseHandle((HANDLE)process->native_handle);
        process->native_handle = NULL;
    }
    process->process_id = 0;
    process->is_active = false;
    process->has_cached_exit = false;
    process->cached_exit_code = 0;
}

#endif /* _WIN32 */

bool nk_platform_local_time_zone(int *standard_minutes, bool *daylight_now) {
    TIME_ZONE_INFORMATION zone;
    DWORD id;
    if (!standard_minutes || !daylight_now) return false;
    id = GetTimeZoneInformation(&zone);
    if (id == TIME_ZONE_ID_INVALID) return false;
    /* Bias is minutes to add to local time for UTC, so east of UTC is negative. */
    *standard_minutes = -(int)(zone.Bias + zone.StandardBias);
    *daylight_now = id == TIME_ZONE_ID_DAYLIGHT;
    return true;
}
