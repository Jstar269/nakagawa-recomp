/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "setup_staging.h"

#include "nk_iso.h"
#include "nk_platform.h"
#include "nk_xb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_MSC_VER)
#include <strings.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#define strcasecmp _stricmp
#endif

#define PLAYER_STAGE_MAX_PRX_BYTES (256u * 1024u * 1024u)

typedef struct {
    const char *staging_root;
    PlayerStageCallbacks callbacks;
    PlayerStageSummary *summary;
    size_t iso_files_complete;
    size_t iso_total_files;
    size_t last_iso_files_complete;
    size_t current_xb_complete;
    size_t current_xb_total;
    char current_archive[4096];
    int last_percent;
    NkResult callback_result;
    char error_message[256];
} PlayerStageContext;

static void stage_error(PlayerStageContext *context, NkResult result,
                        const char *format, ...);

static bool stage_path_suffix_ci(const char *path, const char *suffix) {
    if (!path || !suffix) return false;
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len < suffix_len) return false;
    path += path_len - suffix_len;
    for (size_t i = 0; i < suffix_len; i++) {
        unsigned char a = (unsigned char)path[i];
        unsigned char b = (unsigned char)suffix[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

static bool stage_path_contains_ci(const char *path, const char *needle) {
    if (!path || !needle || !needle[0]) return false;
    size_t needle_len = strlen(needle);
    for (const char *at = path; *at; at++) {
        size_t i = 0;
        while (i < needle_len && at[i]) {
            unsigned char a = (unsigned char)at[i];
            unsigned char b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
            if (a != b) break;
            i++;
        }
        if (i == needle_len) return true;
    }
    return false;
}

static void stage_note_asset(PlayerStageContext *context, const NkXbEntry *entry) {
    if (!context || !context->summary || !entry) return;
    if (context->summary->extracted_asset_count == UINT32_MAX) {
        stage_error(context, NK_ERROR_INVALID_XB, "staged asset count overflow");
        return;
    }
    context->summary->extracted_asset_count++;
    if (stage_path_suffix_ci(entry->path, ".sgd") ||
        stage_path_suffix_ci(entry->path, ".sgh") ||
        stage_path_suffix_ci(entry->path, ".sgb") ||
        stage_path_suffix_ci(entry->path, ".vag") ||
        stage_path_suffix_ci(entry->path, ".at3")) {
        if (context->summary->extracted_audio_count == UINT32_MAX) {
            stage_error(context, NK_ERROR_INVALID_XB, "staged audio count overflow");
            return;
        }
        context->summary->extracted_audio_count++;
    }
    if (stage_path_suffix_ci(entry->path, ".gim")) {
        if (context->summary->extracted_visual_count == UINT32_MAX) {
            stage_error(context, NK_ERROR_INVALID_XB, "staged visual count overflow");
            return;
        }
        context->summary->extracted_visual_count++;
    }
    /* Menu members are the UI/layout side of the packed VFS. This records a
     * path class only; decoding proprietary menu bytes remains separate. */
    if (stage_path_contains_ci(entry->path, "/menu/") ||
        strncmp(entry->path, "menu/", 5) == 0) {
        if (context->summary->extracted_layout_count == UINT32_MAX) {
            stage_error(context, NK_ERROR_INVALID_XB, "staged layout count overflow");
            return;
        }
        context->summary->extracted_layout_count++;
    }
}

static void stage_error(PlayerStageContext *context, NkResult result,
                        const char *format, ...) {
    if (!context) return;
    context->callback_result = result;
    if (format) {
        va_list args;
        va_start(args, format);
        vsnprintf(context->error_message, sizeof(context->error_message), format, args);
        va_end(args);
        context->error_message[sizeof(context->error_message) - 1] = '\0';
    }
}

static const char *stage_default_error(NkResult result) {
    switch (result) {
        case NK_ERROR_GENERIC:
            return "[STAGE_FAILED] A staging step failed without a more specific result code; review the staging log and retry.";
        case NK_ERROR_FILE_NOT_FOUND:
            return "[STAGE_REQUIRED_FILE_MISSING] The ISO or PSP_GAME/SYSDIR/EBOOT.BIN is missing.";
        case NK_ERROR_INVALID_ISO:
            return "[STAGE_ISO_INVALID] The ISO has a malformed or incomplete PSP directory layout.";
        case NK_ERROR_UNSUPPORTED_TITLE:
            return "[STAGE_TITLE_UNSUPPORTED] This title is not supported by its current manifest.";
        case NK_ERROR_IO:
            return "[STAGE_STORAGE_IO] Could not read the ISO or write staged files; check access and free space.";
        case NK_ERROR_OUT_OF_MEMORY:
            return "[STAGE_MEMORY] The ISO staging operation ran out of memory.";
        case NK_ERROR_PROCESS_SPAWN:
            return "[STAGE_PROCESS] A required staging process could not be started.";
        case NK_ERROR_PERMISSION:
            return "[STAGE_PERMISSION] Permission was denied while staging the ISO.";
        case NK_ERROR_CANCELLED:
            return "[STAGE_CANCELLED] Asset staging was cancelled; the title was not added.";
        case NK_ERROR_ALREADY_EXISTS:
            return "[STAGE_ALREADY_EXISTS] A staging folder already exists; retry after cleanup.";
        case NK_ERROR_INVALID_XB:
            return "[STAGE_XB_INVALID] A companion XB asset archive could not be decoded.";
        case NK_ERROR_INVALID_EXECUTABLE:
            return "[STAGE_EXECUTABLE_INVALID] The PSP executable in the ISO is invalid.";
        case NK_OK:
        default:
            return "[STAGE_FAILED] Native ISO staging failed; verify the selected image and retry.";
    }
}

static bool stage_cancelled(PlayerStageContext *context) {
    if (!context || !context->callbacks.is_cancelled) return false;
    if (!context->callbacks.is_cancelled(context->callbacks.userdata)) return false;
    stage_error(context, NK_ERROR_CANCELLED, "Asset staging was cancelled.");
    return true;
}

static void stage_emit(PlayerStageContext *context, const char *path,
                       int percent, size_t files, size_t total) {
    if (!context || !context->callbacks.on_progress) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    context->last_percent = percent;
    context->callbacks.on_progress(path ? path : "", percent, files, total,
                                   context->callbacks.userdata);
}

static bool has_xb_suffix(const char *path) {
    if (!path) return false;
    const char *suffix = strrchr(path, '.');
    if (!suffix) return false;
    return strcasecmp(suffix, ".xb") == 0 || strcasecmp(suffix, ".xb0") == 0 ||
           strcasecmp(suffix, ".xb1") == 0 || strcasecmp(suffix, ".xb2") == 0 ||
           strcasecmp(suffix, ".xb3") == 0;
}

static bool make_staged_path(const char *root, const char *relative,
                             char *out_path, size_t out_size) {
    if (!root || !root[0] || !relative || !relative[0] || !out_path || out_size == 0) return false;
    size_t root_len = strlen(root);
    while (root_len > 0 && (root[root_len - 1] == '/' || root[root_len - 1] == '\\')) root_len--;
    int written = snprintf(out_path, out_size, "%.*s%c%s", (int)root_len, root,
                           nk_platform_path_separator(), relative);
    return written >= 0 && (size_t)written < out_size;
}

static bool staging_root_name_is_safe(const char *staging_root) {
    if (!staging_root || !staging_root[0]) return false;
    const char *base = strrchr(staging_root, '/');
    const char *backslash = strrchr(staging_root, '\\');
    if (backslash && (!base || backslash > base)) base = backslash;
    base = base ? base + 1 : staging_root;
    return strncmp(base, ".staging_", 9) == 0 && base[9] != '\0';
}

#if defined(_WIN32) || defined(_WIN64)
static bool stage_utf8_to_wide(const char *utf8, WCHAR *wide, int wide_count) {
    return utf8 && wide && wide_count > 0 &&
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, wide_count) > 0;
}

static bool discard_missing_error_wide(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_DELETE_PENDING;
}

static HANDLE discard_open_wide(const WCHAR *path) {
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;

    /* GENERIC_READ supplies FILE_LIST_DIRECTORY for ordinary directories. A
     * reparse point may only grant attribute/delete access, so retry with the
     * narrower request; the handle is still the object that will be deleted. */
    HANDLE handle = CreateFileW(path, GENERIC_READ | DELETE, share, NULL,
                                OPEN_EXISTING, flags, NULL);
    if (handle != INVALID_HANDLE_VALUE) return handle;
    return CreateFileW(path, FILE_READ_ATTRIBUTES | DELETE, share, NULL,
                       OPEN_EXISTING, flags, NULL);
}

static bool discard_file_id_wide(HANDLE handle, ULONGLONG *file_id) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!file_id || !GetFileInformationByHandle(handle, &info)) return false;
    *file_id = ((ULONGLONG)info.nFileIndexHigh << 32) |
               (ULONGLONG)info.nFileIndexLow;
    return true;
}

static bool discard_delete_handle_wide(HANDLE handle) {
    FILE_DISPOSITION_INFO disposition;
    disposition.DeleteFile = TRUE;
    bool deleted = SetFileInformationByHandle(handle, FileDispositionInfo,
                                              &disposition, sizeof(disposition)) != 0;
    DWORD error = deleted ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    return deleted || discard_missing_error_wide(error);
}

static bool discard_child_path_wide(const WCHAR *parent_path, const WCHAR *name,
                                    size_t name_length, WCHAR *child,
                                    size_t child_count) {
    if (!parent_path || !name || name_length == 0 || !child || child_count == 0) return false;
    size_t parent_length = wcslen(parent_path);
    bool has_separator = parent_length > 0 &&
                         (parent_path[parent_length - 1] == L'\\' ||
                          parent_path[parent_length - 1] == L'/');
    int written = _snwprintf(child, child_count, has_separator ? L"%ls%.*ls" : L"%ls\\%.*ls",
                             parent_path, (int)name_length, name);
    return written >= 0 && (size_t)written < child_count;
}

static bool discard_directory_wide(HANDLE directory, const WCHAR *directory_path);

static bool discard_entry_wide(const WCHAR *directory_path,
                               const FILE_ID_BOTH_DIR_INFO *entry) {
    if ((entry->FileNameLength % sizeof(WCHAR)) != 0) return false;
    size_t name_length = (size_t)entry->FileNameLength / sizeof(WCHAR);
    if (name_length == 0 || name_length > 32767u) return false;
    if ((name_length == 1 && entry->FileName[0] == L'.') ||
        (name_length == 2 && entry->FileName[0] == L'.' && entry->FileName[1] == L'.')) {
        return true;
    }

    WCHAR child_path[32768];
    if (!discard_child_path_wide(directory_path, entry->FileName, name_length,
                                 child_path, sizeof(child_path) / sizeof(child_path[0]))) {
        return false;
    }

    /* The directory is enumerated through its handle. Opening the name again
     * is only a way to obtain a delete handle on Win32, so bind that handle to
     * the enumerated object before inspecting or deleting it. */
    HANDLE child = discard_open_wide(child_path);
    if (child == INVALID_HANDLE_VALUE) return discard_missing_error_wide(GetLastError());
    ULONGLONG actual_id;
    ULONGLONG enumerated_id = (ULONGLONG)entry->FileId.QuadPart;
    if (!discard_file_id_wide(child, &actual_id) || actual_id != enumerated_id) {
        CloseHandle(child);
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(child, &info)) {
        CloseHandle(child);
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return discard_delete_handle_wide(child);
    }

    bool okay = discard_directory_wide(child, child_path);
    if (!okay) {
        CloseHandle(child);
        return false;
    }
    return discard_delete_handle_wide(child);
}

static bool discard_directory_wide(HANDLE directory, const WCHAR *directory_path) {
    if (!directory || !directory_path) return false;
    BYTE buffer[64 * 1024];
    for (;;) {
        if (!GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo,
                                           buffer, sizeof(buffer))) {
            DWORD error = GetLastError();
            return error == ERROR_NO_MORE_FILES;
        }

        BYTE *cursor = buffer;
        for (;;) {
            const FILE_ID_BOTH_DIR_INFO *entry = (const FILE_ID_BOTH_DIR_INFO *)cursor;
            if (!discard_entry_wide(directory_path, entry)) return false;
            if (entry->NextEntryOffset == 0) break;
            if (entry->NextEntryOffset >= sizeof(buffer) ||
                entry->NextEntryOffset > sizeof(buffer) - (size_t)(cursor - buffer)) {
                return false;
            }
            cursor += entry->NextEntryOffset;
        }
    }
}

static bool discard_tree_wide(const WCHAR *path) {
    HANDLE root = discard_open_wide(path);
    if (root == INVALID_HANDLE_VALUE) return discard_missing_error_wide(GetLastError());

    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(root, &info)) {
        CloseHandle(root);
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return discard_delete_handle_wide(root);
    }

    WCHAR directory_path[32768];
    DWORD path_length = GetFinalPathNameByHandleW(root, directory_path,
                                                   (DWORD)(sizeof(directory_path) / sizeof(directory_path[0])),
                                                   FILE_NAME_NORMALIZED);
    if (path_length == 0 || path_length >= sizeof(directory_path) / sizeof(directory_path[0])) {
        CloseHandle(root);
        return false;
    }
    if (!discard_directory_wide(root, directory_path)) {
        CloseHandle(root);
        return false;
    }
    return discard_delete_handle_wide(root);
}
#else
static bool discard_tree_posix_at(int parent_fd, const char *name);

static bool discard_open_directory_fd_posix(int dir_fd) {
    int iter_fd = dup(dir_fd);
    if (iter_fd < 0) return false;

    DIR *directory = fdopendir(iter_fd);
    if (!directory) {
        close(iter_fd);
        return false;
    }

    bool okay = true;
    struct dirent *entry;
    while (okay && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        okay = discard_tree_posix_at(dir_fd, entry->d_name);
    }

    closedir(directory);
    return okay;
}

static bool discard_tree_posix_at(int parent_fd, const char *name) {
    struct stat info;
    if (fstatat(parent_fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;

    if (!S_ISDIR(info.st_mode)) return unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT;

    int child_fd = openat(parent_fd, name,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (child_fd < 0) return errno == ENOENT;

    bool okay = discard_open_directory_fd_posix(child_fd);
    close(child_fd);
    if (!okay) return false;

    return unlinkat(parent_fd, name, AT_REMOVEDIR) == 0 || errno == ENOENT;
}

static bool discard_tree_posix(const char *path) {
    if (unlink(path) == 0) return true;
    if (errno == ENOENT) return true;
    if (errno != EISDIR && errno != EPERM) return false;

    int root_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0) return errno == ENOENT;

    bool okay = discard_open_directory_fd_posix(root_fd);
    close(root_fd);
    if (!okay) return false;

    return rmdir(path) == 0 || errno == ENOENT;
}
#endif

bool player_stage_discard(const char *staging_root) {
    if (!staging_root_name_is_safe(staging_root)) return false;
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[32768];
    if (!stage_utf8_to_wide(staging_root, wide, (int)(sizeof(wide) / sizeof(wide[0])))) return false;
    return discard_tree_wide(wide);
#else
    return discard_tree_posix(staging_root);
#endif
}

static FILE *stage_fopen(const char *path, const char *mode) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wpath[32768];
    WCHAR wmode[32];
    if (!stage_utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) ||
        !stage_utf8_to_wide(mode, wmode, (int)(sizeof(wmode) / sizeof(wmode[0])))) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

static bool make_prx_tree_path(const char *root, const char *layout,
                               const char *file_name, bool include_prx_directory,
                               char *out_path, size_t out_size) {
    if (!root || !root[0] || !layout || !layout[0] || !file_name || !file_name[0] ||
        !out_path || out_size == 0) return false;
    const char *suffix = include_prx_directory
        ? "PSP/SYSTEM/DUMP/PRX/%s" : "PSP/SYSTEM/DUMP/%s";
    int written = snprintf(out_path, out_size, "%s%c%s%c%s", root,
                           nk_platform_path_separator(), layout,
                           nk_platform_path_separator(), "");
    if (written < 0 || (size_t)written >= out_size) return false;
    int remaining = (int)(out_size - (size_t)written);
    int suffix_written = snprintf(out_path + written, (size_t)remaining,
                                  suffix, file_name);
    return suffix_written >= 0 && (size_t)suffix_written < (size_t)remaining;
}

static bool try_prx_tree(const char *root, const char *layout,
                         const char *file_name, char *out_path,
                         size_t out_size) {
    if (!make_prx_tree_path(root, layout, file_name, false,
                            out_path, out_size)) return false;
    if (nk_platform_file_exists(out_path)) return true;
    if (!make_prx_tree_path(root, layout, file_name, true,
                            out_path, out_size)) return false;
    return nk_platform_file_exists(out_path);
}

static bool find_prx_source(const char *file_name, char *out_path,
                            size_t out_size) {
    if (!file_name || !file_name[0] || !out_path || out_size == 0) return false;
#if defined(_WIN32) || defined(_WIN64)
    const char *appdata = getenv("APPDATA");
    const char *userprofile = getenv("USERPROFILE");
    if (appdata && try_prx_tree(appdata, "PPSSPP", file_name, out_path, out_size)) return true;
    if (userprofile && try_prx_tree(userprofile, "Documents/PPSSPP", file_name,
                                    out_path, out_size)) return true;
#else
    const char *home = getenv("HOME");
    const char *xdg_data = getenv("XDG_DATA_HOME");
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_data && try_prx_tree(xdg_data, "PPSSPP", file_name, out_path, out_size)) return true;
    if (xdg_data && try_prx_tree(xdg_data, "ppsspp", file_name, out_path, out_size)) return true;
    if (xdg_config && try_prx_tree(xdg_config, "PPSSPP", file_name, out_path, out_size)) return true;
    if (xdg_config && try_prx_tree(xdg_config, "ppsspp", file_name, out_path, out_size)) return true;
    if (home) {
        static const char *const layouts[] = {
            ".local/share/PPSSPP", ".local/share/ppsspp",
            ".config/PPSSPP", ".config/ppsspp",
            "Library/Application Support/PPSSPP", "Documents/PPSSPP",
            "PPSSPP"
        };
        for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
            if (try_prx_tree(home, layouts[i], file_name, out_path, out_size)) return true;
        }
    }
    /* Keep the legacy variables as a compatibility fallback for test fixtures
     * and existing portable installs; POSIX discovery above is the normal path. */
    const char *appdata = getenv("APPDATA");
    const char *userprofile = getenv("USERPROFILE");
    if (appdata && try_prx_tree(appdata, "PPSSPP", file_name, out_path, out_size)) return true;
    if (userprofile && try_prx_tree(userprofile, "Documents/PPSSPP", file_name,
                                    out_path, out_size)) return true;
#endif
    out_path[0] = '\0';
    return false;
}

static bool copy_prx_file(PlayerStageContext *context, const char *source_path,
                          const char *destination_path) {
    if (!context || !source_path || !destination_path) return false;
    int64_t source_size = nk_platform_get_file_size(source_path);
    if (source_size <= 0 || (uint64_t)source_size > PLAYER_STAGE_MAX_PRX_BYTES) {
        stage_error(context, NK_ERROR_IO, "PRX source is empty or exceeds the size budget");
        return false;
    }
    char parent[4096];
    size_t destination_length = strlen(destination_path);
    if (destination_length >= sizeof(parent)) return false;
    memcpy(parent, destination_path, destination_length + 1);
    char *slash = strrchr(parent, '/');
    char *backslash = strrchr(parent, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (!slash) return false;
    *slash = '\0';
    if (!nk_platform_mkdir_p_private(parent)) {
        stage_error(context, NK_ERROR_IO, "cannot create decrypted PRX staging directory");
        return false;
    }

    FILE *source = stage_fopen(source_path, "rb");
    FILE *destination = nk_platform_fopen_private(destination_path, "wb");
    if (!source || !destination) {
        if (source) fclose(source);
        if (destination) fclose(destination);
        nk_remove_utf8(destination_path);
        stage_error(context, NK_ERROR_IO, "cannot open PRX staging file");
        return false;
    }
    uint8_t buffer[64 * 1024];
    uint64_t remaining = (uint64_t)source_size;
    bool okay = true;
    while (remaining > 0) {
        size_t requested = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        size_t read_count = fread(buffer, 1, requested, source);
        if (read_count == 0 || fwrite(buffer, 1, read_count, destination) != read_count) {
            okay = false;
            break;
        }
        remaining -= read_count;
        if (stage_cancelled(context)) {
            okay = false;
            break;
        }
    }
    int source_close_failed = fclose(source) != 0;
    int destination_close_failed = fclose(destination) != 0;
    if (!okay || remaining != 0 || source_close_failed || destination_close_failed) {
        nk_remove_utf8(destination_path);
        if (context->callback_result == NK_ERROR_CANCELLED) return false;
        stage_error(context, NK_ERROR_IO, "failed while copying decrypted PRX");
        return false;
    }
    return true;
}

static bool discover_prx_for_stage(PlayerStageContext *context) {
    if (!context || !context->staging_root) return false;
    const char *names[] = {
        "libfont.prx",
        "scePsmf_library.prx",
        "scePsmfP_library.prx"
    };
    size_t discovered = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (stage_cancelled(context)) return false;
        char source_path[4096];
        if (!find_prx_source(names[i], source_path, sizeof(source_path))) continue;

        char relative_destination[256];
        int relative_written = snprintf(relative_destination,
                                        sizeof(relative_destination),
                                        "EXTRACTED/decrypted/%s", names[i]);
        if (relative_written < 0 || (size_t)relative_written >= sizeof(relative_destination)) {
            stage_error(context, NK_ERROR_IO, "PRX destination path is too long");
            return false;
        }
        char destination_path[4096];
        if (!make_staged_path(context->staging_root, relative_destination,
                              destination_path, sizeof(destination_path)) ||
            !copy_prx_file(context, source_path, destination_path)) return false;
        discovered++;
        if (context->callbacks.on_progress) {
            int percent = 95 + (int)((discovered * 5u) / 3u);
            context->callbacks.on_progress(relative_destination, percent,
                                           context->iso_files_complete + discovered,
                                           context->iso_total_files + 3u,
                                           context->callbacks.userdata);
        }
    }
    return true;
}

static bool xb_progress(const NkXbEntry *entry, size_t completed_entries,
                        size_t total_entries, void *userdata) {
    PlayerStageContext *context = (PlayerStageContext *)userdata;
    if (!context || !entry || total_entries == 0) return false;
    if (stage_cancelled(context)) return false;
    context->current_xb_complete = completed_entries;
    context->current_xb_total = total_entries;
    size_t total = context->iso_total_files + total_entries;
    size_t files = context->iso_files_complete + completed_entries;
    /* The archive's bytes are already counted, so unpacking it holds the
     * overall percentage and advances only the file count. */
    char current_path[4096];
    /* A progress label only: a very long name is shortened, never an error. */
    snprintf(current_path, sizeof(current_path), "%.2048s.d/%s",
             context->current_archive, entry->path);
    stage_emit(context, current_path, context->last_percent, files, total);
    stage_note_asset(context, entry);
    if (context->callback_result != NK_OK) return false;
    return true;
}

static bool iso_progress(const char *relative_path, uint64_t bytes_complete,
                         uint64_t bytes_total, size_t files_complete,
                         size_t total_files, void *userdata) {
    PlayerStageContext *context = (PlayerStageContext *)userdata;
    if (!context || !relative_path) return false;
    if (stage_cancelled(context)) return false;
    context->iso_files_complete = files_complete;
    context->iso_total_files = total_files;
    /* Overall progress follows the disc bytes copied (0-95%), so it never
     * moves backwards between files and archives; the optional support-PRX
     * copy finishes the last 5%. */
    int percent = bytes_total == 0 ? 0 : (int)((bytes_complete * 95u) / bytes_total);
    if (percent > 95) percent = 95;
    stage_emit(context, relative_path, percent, files_complete,
               total_files + context->current_xb_total);

    /* NkIsoProgressCallback's final callback is the only one whose completed
     * file count advances. Decode an archive only after its bytes have been
     * closed successfully, so the XB parser never reads a partial file. */
    if (files_complete <= context->last_iso_files_complete) return true;
    context->last_iso_files_complete = files_complete;
    if (!has_xb_suffix(relative_path)) return true;

    char archive_path[4096];
    if (!make_staged_path(context->staging_root, relative_path,
                          archive_path, sizeof(archive_path))) {
        stage_error(context, NK_ERROR_IO, "staged archive path is too long");
        return false;
    }
    /* Keep the archive identity in the extracted tree. The runtime asset
     * index intentionally consumes
     *   <data-root>/<archive>.xb[0-9].d/<member>
     * so two archives may contain the same guest-relative key without one
     * silently overwriting the other. */
    char unpack_relative[4096];
    int unpack_relative_written = snprintf(unpack_relative,
                                           sizeof(unpack_relative),
                                           "%s.d", relative_path);
    if (unpack_relative_written < 0 ||
        (size_t)unpack_relative_written >= sizeof(unpack_relative)) {
        stage_error(context, NK_ERROR_IO, "XB output path is too long");
        return false;
    }
    char unpack_root[4096];
    if (!make_staged_path(context->staging_root, unpack_relative,
                          unpack_root, sizeof(unpack_root))) {
        stage_error(context, NK_ERROR_IO, "XB output path is too long");
        return false;
    }
    snprintf(context->current_archive, sizeof(context->current_archive), "%s",
             relative_path);
    context->current_xb_complete = 0;
    context->current_xb_total = 0;
    char xb_error[256];
    NkResult result = nk_xb_unpack(archive_path, unpack_root, xb_progress,
                                   context, xb_error, sizeof(xb_error));
    if (result != NK_OK) {
        stage_error(context, result, "%s", xb_error[0] ? xb_error : "XB archive unpack failed.");
        return false;
    }
    return true;
}

NkResult player_stage_game_with_summary(const char *iso_path,
                                        const char *staging_root,
                                        const char *const *loose_content_roots,
                                        size_t loose_content_root_count,
                                        const PlayerStageCallbacks *callbacks,
                                        PlayerStageSummary *summary,
                                        char *error_message,
                                        size_t error_message_size) {
    if (error_message && error_message_size > 0) error_message[0] = '\0';
    if (summary) memset(summary, 0, sizeof(*summary));
    if (!iso_path || !iso_path[0] || !staging_root || !staging_root[0]) {
        if (error_message && error_message_size > 0) {
            snprintf(error_message, error_message_size, "%s",
                     "[STAGE_REQUEST_INVALID] Select an ISO and a valid staging destination.");
        }
        return NK_ERROR_GENERIC;
    }
    if (nk_platform_dir_exists(staging_root)) {
        if (error_message && error_message_size > 0) {
            snprintf(error_message, error_message_size, "%s",
                     stage_default_error(NK_ERROR_ALREADY_EXISTS));
        }
        return NK_ERROR_ALREADY_EXISTS;
    }
    if (!nk_platform_mkdir_p_private(staging_root)) {
        if (error_message && error_message_size > 0) {
            snprintf(error_message, error_message_size, "%s",
                     stage_default_error(NK_ERROR_IO));
        }
        return NK_ERROR_IO;
    }

    PlayerStageContext context;
    memset(&context, 0, sizeof(context));
    context.staging_root = staging_root;
    context.summary = summary;
    context.callback_result = NK_OK;
    if (callbacks) context.callbacks = *callbacks;

    NkResult result = nk_iso_extract_game(iso_path, staging_root,
                                          loose_content_roots,
                                          loose_content_root_count,
                                          iso_progress, &context);
    if (result != NK_OK && context.callback_result != NK_OK) {
        result = context.callback_result;
    }
    if (result != NK_OK && error_message && error_message_size > 0) {
        snprintf(error_message, error_message_size, "%s",
                 context.error_message[0] ? context.error_message : stage_default_error(result));
    }
    if (result == NK_OK && !discover_prx_for_stage(&context)) {
        result = context.callback_result != NK_OK ? context.callback_result : NK_ERROR_IO;
        if (error_message && error_message_size > 0) {
            snprintf(error_message, error_message_size, "%s",
                     context.error_message[0] ? context.error_message :
                         stage_default_error(result));
        }
    }
    if (result != NK_OK) {
        player_stage_discard(staging_root);
    }
    return result;
}

NkResult player_stage_game(const char *iso_path, const char *staging_root,
                           const char *const *loose_content_roots,
                           size_t loose_content_root_count,
                           const PlayerStageCallbacks *callbacks,
                           char *error_message, size_t error_message_size) {
    return player_stage_game_with_summary(iso_path, staging_root,
                                          loose_content_roots,
                                          loose_content_root_count,
                                          callbacks, NULL, error_message,
                                          error_message_size);
}

/* ------------------------------------------------------------------------- */
/* Title staging transaction                                                  */
/* ------------------------------------------------------------------------- */

#define PLAYER_STAGE_PATH_MAX 4096
#define PLAYER_STAGE_RECORD_RELATIVE "EXTRACTED/staging-record.txt"
#define PLAYER_STAGE_RECORD_MAGIC "nakagawa-staged-title 1\n"
#define PLAYER_STAGE_RECORD_MAX_BYTES 8192u

typedef struct {
    char games_root[PLAYER_STAGE_PATH_MAX];
    char staging_root[PLAYER_STAGE_PATH_MAX];
    char final_root[PLAYER_STAGE_PATH_MAX];
    char retired_root[PLAYER_STAGE_PATH_MAX];
    char lock_path[PLAYER_STAGE_PATH_MAX];
} PlayerStageTitlePaths;

typedef struct {
#if defined(_WIN32) || defined(_WIN64)
    HANDLE handle;
#else
    int fd;
#endif
    bool held;
} PlayerStageLock;

static void stage_title_message(char *message, size_t message_size,
                                const char *format, ...) {
    if (!message || message_size == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(message, message_size, format, args);
    va_end(args);
    message[message_size - 1] = '\0';
}

/* Disc IDs name directories here, so only the characters PARAM.SFO disc IDs
 * use are accepted; '.' in particular stays reserved for the transaction's
 * own sibling names. */
static bool stage_title_disc_id_valid(const char *disc_id) {
    if (!disc_id || !disc_id[0] || strlen(disc_id) > 32u) return false;
    for (const unsigned char *p = (const unsigned char *)disc_id; *p; p++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!ok) return false;
    }
    return true;
}

/* Record fields are newline-delimited, so no field may carry a control byte. */
static bool stage_title_field_valid(const char *value, size_t max_length) {
    if (!value) return true;
    size_t length = strlen(value);
    if (length > max_length) return false;
    for (size_t i = 0; i < length; i++) {
        if ((unsigned char)value[i] < 0x20u || value[i] == 0x7f) return false;
    }
    return true;
}

static bool stage_title_join(char *out, size_t out_size, const char *root,
                             const char *relative) {
    if (!make_staged_path(root, relative, out, out_size)) return false;
    size_t root_length = strlen(root);
    while (root_length > 0 &&
           (root[root_length - 1] == '/' || root[root_length - 1] == '\\')) {
        root_length--;
    }
    for (char *p = out + root_length + 1u; *p; p++) {
        if (*p == '/' || *p == '\\') *p = nk_platform_path_separator();
    }
    return true;
}

static bool stage_title_paths(const char *user_data_root, const char *disc_id,
                              PlayerStageTitlePaths *paths) {
    char name[64];
    if (!stage_title_join(paths->games_root, sizeof(paths->games_root),
                          user_data_root, "games")) return false;
    snprintf(name, sizeof(name), ".staging_%s", disc_id);
    if (!stage_title_join(paths->staging_root, sizeof(paths->staging_root),
                          paths->games_root, name)) return false;
    if (!stage_title_join(paths->final_root, sizeof(paths->final_root),
                          paths->games_root, disc_id)) return false;
    /* '.' never appears in a valid disc ID, so neither sibling below can be
     * another title's staging tree. Both keep the `.staging_` prefix that
     * player_stage_discard requires. */
    snprintf(name, sizeof(name), ".staging_%s.retired", disc_id);
    if (!stage_title_join(paths->retired_root, sizeof(paths->retired_root),
                          paths->games_root, name)) return false;
    snprintf(name, sizeof(name), ".staging_%s.lock", disc_id);
    return stage_title_join(paths->lock_path, sizeof(paths->lock_path),
                            paths->games_root, name);
}

/* 1 = acquired, 0 = held by another staging of this title, -1 = error. The
 * operating system releases the lock when its holder exits, so a crashed or
 * killed staging never blocks the next attempt. */
static int stage_lock_acquire(const char *path, PlayerStageLock *lock) {
    memset(lock, 0, sizeof(*lock));
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[32768];
    if (!stage_utf8_to_wide(path, wide, (int)(sizeof(wide) / sizeof(wide[0])))) return -1;
    /* No sharing: a second open of the same lock file fails while this handle
     * is open, in this process or any other. */
    HANDLE handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) ? 0 : -1;
    }
    lock->handle = handle;
#else
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct flock region;
    memset(&region, 0, sizeof(region));
    region.l_type = F_WRLCK;
    region.l_whence = SEEK_SET;
    /* fcntl record locks belong to the process: the player runs at most one
     * staging job at a time, and separate processes exclude each other. */
    if (fcntl(fd, F_SETLK, &region) != 0) {
        int error = errno;
        close(fd);
        return (error == EACCES || error == EAGAIN) ? 0 : -1;
    }
    lock->fd = fd;
#endif
    lock->held = true;
    return 1;
}

static void stage_lock_release(PlayerStageLock *lock) {
    if (!lock || !lock->held) return;
#if defined(_WIN32) || defined(_WIN64)
    CloseHandle(lock->handle);
#else
    close(lock->fd);
#endif
    lock->held = false;
}

/* Move a directory to a name that must not exist yet, flushing the rename
 * before returning. */
static bool stage_rename_directory(const char *from, const char *to) {
    if (!from || !to || nk_platform_dir_exists(to) || nk_platform_file_exists(to)) return false;
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide_from[32768];
    WCHAR wide_to[32768];
    if (!stage_utf8_to_wide(from, wide_from, (int)(sizeof(wide_from) / sizeof(wide_from[0]))) ||
        !stage_utf8_to_wide(to, wide_to, (int)(sizeof(wide_to) / sizeof(wide_to[0])))) return false;
    return MoveFileExW(wide_from, wide_to, MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}

bool player_stage_title_takes_data_from_disc(const PlayerStageTitleRequest *request) {
    if (!request || !request->data_root || !request->data_root[0] ||
        (request->loose_content_root_count != 0 && !request->loose_content_roots)) {
        return false;
    }
    size_t data_length = strlen(request->data_root);
    for (size_t i = 0; i < request->loose_content_root_count; i++) {
        const char *root = request->loose_content_roots[i];
        if (!root) continue;
        if (strcmp(root, ".") == 0) return true;
        size_t root_length = strlen(root);
        if (data_length >= root_length &&
            strncmp(request->data_root, root, root_length) == 0 &&
            (data_length == root_length || request->data_root[root_length] == '/')) {
            return true;
        }
    }
    return false;
}

/* Names the first required entry the tree lacks; NULL when it is complete. */
static const char *stage_tree_missing_entry(const char *tree,
                                            const PlayerStageTitleRequest *request) {
    char path[PLAYER_STAGE_PATH_MAX];
    if (!stage_title_join(path, sizeof(path), tree, "EBOOT.BIN") ||
        !nk_platform_file_exists(path)) {
        return "EBOOT.BIN";
    }
    for (size_t i = 0; i < request->loose_content_root_count; i++) {
        const char *root = request->loose_content_roots[i];
        if (strcmp(root, ".") == 0) continue;
        if (!stage_title_join(path, sizeof(path), tree, root) ||
            !nk_platform_dir_exists(path)) return root;
    }
    if (player_stage_title_takes_data_from_disc(request) &&
        (!stage_title_join(path, sizeof(path), tree, request->data_root) ||
         !nk_platform_dir_exists(path))) {
        return request->data_root;
    }
    return NULL;
}

static bool stage_record_identity(const PlayerStageTitleRequest *request,
                                  int64_t iso_size, char *out, size_t out_size) {
    int written = snprintf(out, out_size,
                           PLAYER_STAGE_RECORD_MAGIC
                           "disc_id=%s\ndisc_version=%s\niso_size=%lld\ndata_root=%s\n",
                           request->disc_id,
                           request->disc_version ? request->disc_version : "",
                           (long long)iso_size,
                           request->data_root ? request->data_root : "");
    if (written < 0 || (size_t)written >= out_size) return false;
    size_t used = (size_t)written;
    for (size_t i = 0; i < request->loose_content_root_count; i++) {
        written = snprintf(out + used, out_size - used, "root=%s\n",
                           request->loose_content_roots[i]);
        if (written < 0 || (size_t)written >= out_size - used) return false;
        used += (size_t)written;
    }
    return true;
}

static bool stage_record_write(const char *tree, const char *identity,
                               const PlayerStageSummary *summary) {
    char record_path[PLAYER_STAGE_PATH_MAX];
    char record_dir[PLAYER_STAGE_PATH_MAX];
    if (!stage_title_join(record_path, sizeof(record_path), tree,
                          PLAYER_STAGE_RECORD_RELATIVE) ||
        !stage_title_join(record_dir, sizeof(record_dir), tree, "EXTRACTED") ||
        !nk_platform_mkdir_p_private(record_dir)) return false;
    FILE *file = nk_platform_fopen_private(record_path, "wb");
    if (!file) return false;
    bool okay = fputs(identity, file) >= 0 &&
                fprintf(file, "assets=%lu\naudio=%lu\nvisual=%lu\nlayout=%lu\n",
                        (unsigned long)summary->extracted_asset_count,
                        (unsigned long)summary->extracted_audio_count,
                        (unsigned long)summary->extracted_visual_count,
                        (unsigned long)summary->extracted_layout_count) > 0;
    if (fclose(file) != 0) okay = false;
    if (!okay) nk_remove_utf8(record_path);
    return okay;
}

static bool stage_record_parse_count(const char **cursor, const char *key,
                                     uint32_t *value) {
    size_t key_length = strlen(key);
    if (strncmp(*cursor, key, key_length) != 0) return false;
    const char *p = *cursor + key_length;
    uint64_t parsed = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') {
        parsed = parsed * 10u + (uint64_t)(*p - '0');
        if (parsed > UINT32_MAX) return false;
        p++;
    }
    if (*p != '\n') return false;
    *value = (uint32_t)parsed;
    *cursor = p + 1;
    return true;
}

/* A tree is reused only when its record names this exact disc, revision,
 * image size, data root and loose-content roots. A tree staged by an older
 * build (no record) or for a different revision is replaced. */
static bool stage_record_matches(const char *tree, const char *identity,
                                 PlayerStageSummary *summary) {
    char record_path[PLAYER_STAGE_PATH_MAX];
    if (!stage_title_join(record_path, sizeof(record_path), tree,
                          PLAYER_STAGE_RECORD_RELATIVE)) return false;
    FILE *file = stage_fopen(record_path, "rb");
    if (!file) return false;
    char contents[PLAYER_STAGE_RECORD_MAX_BYTES + 1u];
    size_t length = fread(contents, 1, PLAYER_STAGE_RECORD_MAX_BYTES + 1u, file);
    bool read_failed = ferror(file) != 0;
    fclose(file);
    if (read_failed || length > PLAYER_STAGE_RECORD_MAX_BYTES) return false;
    contents[length] = '\0';
    size_t identity_length = strlen(identity);
    if (length < identity_length || memcmp(contents, identity, identity_length) != 0) {
        return false;
    }
    PlayerStageSummary parsed;
    memset(&parsed, 0, sizeof(parsed));
    const char *cursor = contents + identity_length;
    if (!stage_record_parse_count(&cursor, "assets=", &parsed.extracted_asset_count) ||
        !stage_record_parse_count(&cursor, "audio=", &parsed.extracted_audio_count) ||
        !stage_record_parse_count(&cursor, "visual=", &parsed.extracted_visual_count) ||
        !stage_record_parse_count(&cursor, "layout=", &parsed.extracted_layout_count) ||
        *cursor != '\0') {
        return false;
    }
    if (summary) *summary = parsed;
    return true;
}

/* Plain words for a failed extraction. The extractor's own technical detail
 * follows the sentence when it adds something. */
static void stage_title_extraction_message(NkResult result, const char *detail,
                                           char *message, size_t message_size) {
    const char *sentence;
    switch (result) {
        case NK_ERROR_CANCELLED:
            sentence = "[STAGE_CANCELLED] Setup was cancelled before the game's files were in place, so nothing was added. Start setup again whenever you are ready.";
            break;
        case NK_ERROR_FILE_NOT_FOUND:
            sentence = "[STAGE_REQUIRED_FILE_MISSING] The disc image or the game program on it could not be found. Check that the disc image file is still there and complete, then try again.";
            break;
        case NK_ERROR_INVALID_ISO:
            sentence = "[STAGE_ISO_INVALID] This file does not look like a complete PSP disc image. Make a fresh copy of your disc and try again.";
            break;
        case NK_ERROR_INVALID_XB:
            sentence = "[STAGE_XB_INVALID] One of the game's data archives on the disc could not be read. The disc image may be damaged; make a fresh copy of your disc and try again.";
            break;
        case NK_ERROR_IO:
            sentence = "[STAGE_STORAGE_IO] The game's files could not be written. Check that the drive has free space and that Nakagawa's data folder is writable, then try again.";
            break;
        case NK_ERROR_PERMISSION:
            sentence = "[STAGE_PERMISSION] Permission was denied while writing the game's files. Check the permissions of Nakagawa's data folder, then try again.";
            break;
        case NK_ERROR_OUT_OF_MEMORY:
            sentence = "[STAGE_MEMORY] The computer ran out of memory while setting up the game. Close other programs and try again.";
            break;
        default:
            sentence = "[STAGE_FAILED] The game's files could not be set up. Try again; if it keeps happening, the details below say what stopped it.";
            break;
    }
    if (result != NK_ERROR_CANCELLED && detail && detail[0] && detail[0] != '[') {
        stage_title_message(message, message_size, "%s Details: %s", sentence, detail);
    } else {
        stage_title_message(message, message_size, "%s", sentence);
    }
}

static bool stage_title_cancelled(const PlayerStageCallbacks *callbacks) {
    return callbacks && callbacks->is_cancelled &&
           callbacks->is_cancelled(callbacks->userdata);
}

/* Ends a failed transaction: the unpromoted tree goes, the lock is released,
 * and no counts are reported for work that did not land. */
static NkResult stage_title_fail(PlayerStageLock *lock, const char *staging_root,
                                 PlayerStageSummary *summary, NkResult result) {
    if (staging_root) (void)player_stage_discard(staging_root);
    stage_lock_release(lock);
    memset(summary, 0, sizeof(*summary));
    return result;
}

NkResult player_stage_title(const PlayerStageTitleRequest *request,
                            const PlayerStageCallbacks *callbacks,
                            PlayerStageSummary *summary,
                            PlayerStageTitleOutcome *outcome,
                            char *prepared_root, size_t prepared_root_size,
                            char *message, size_t message_size) {
    PlayerStageSummary local_summary;
    if (!summary) summary = &local_summary;
    memset(summary, 0, sizeof(*summary));
    if (outcome) *outcome = PLAYER_STAGE_TITLE_STAGED;
    if (prepared_root && prepared_root_size) prepared_root[0] = '\0';
    if (message && message_size) message[0] = '\0';

    bool request_valid = request && request->iso_path && request->iso_path[0] &&
        request->user_data_root && request->user_data_root[0] &&
        stage_title_disc_id_valid(request->disc_id) &&
        stage_title_field_valid(request->disc_version, 64u) &&
        stage_title_field_valid(request->data_root, 256u) &&
        prepared_root && prepared_root_size > 0 &&
        (request->loose_content_root_count == 0 || request->loose_content_roots);
    for (size_t i = 0; request_valid && i < request->loose_content_root_count; i++) {
        const char *root = request->loose_content_roots[i];
        request_valid = root && root[0] && stage_title_field_valid(root, 256u);
    }
    if (!request_valid) {
        stage_title_message(message, message_size, "%s",
            "[STAGE_REQUEST_INVALID] This game's setup details are incomplete, so its files were not set up. Add the disc again from the library.");
        return NK_ERROR_GENERIC;
    }

    PlayerStageTitlePaths paths;
    if (!stage_title_paths(request->user_data_root, request->disc_id, &paths) ||
        strlen(paths.final_root) >= prepared_root_size) {
        stage_title_message(message, message_size, "%s",
            "[STAGE_PATH_TOO_LONG] Nakagawa's data folder path is too long to hold this game's files. Use a shorter data folder location and try again.");
        return NK_ERROR_IO;
    }

    int64_t iso_size = nk_platform_get_file_size(request->iso_path);
    if (iso_size <= 0) {
        stage_title_extraction_message(NK_ERROR_FILE_NOT_FOUND, NULL, message, message_size);
        return NK_ERROR_FILE_NOT_FOUND;
    }
    char identity[PLAYER_STAGE_RECORD_MAX_BYTES / 2u];
    if (!stage_record_identity(request, iso_size, identity, sizeof(identity))) {
        stage_title_message(message, message_size, "%s",
            "[STAGE_REQUEST_INVALID] This game's setup details are too large to record. Add the disc again from the library.");
        return NK_ERROR_GENERIC;
    }

    if (!nk_platform_mkdir_p_private(paths.games_root)) {
        stage_title_extraction_message(NK_ERROR_IO, NULL, message, message_size);
        return NK_ERROR_IO;
    }
    PlayerStageLock lock;
    int lock_state = stage_lock_acquire(paths.lock_path, &lock);
    if (lock_state == 0) {
        stage_title_message(message, message_size, "%s",
            "[STAGE_BUSY] This game is already being set up by another Nakagawa window or command. Let that finish, then try again.");
        return NK_ERROR_ALREADY_EXISTS;
    }
    if (lock_state < 0) {
        stage_title_extraction_message(NK_ERROR_IO, NULL, message, message_size);
        return NK_ERROR_IO;
    }

    /* Whatever an interrupted attempt left behind is never promoted: an
     * unpromoted extraction is redone, and a replaced tree is finished off. */
    if ((nk_platform_dir_exists(paths.staging_root) &&
         !player_stage_discard(paths.staging_root)) ||
        (nk_platform_dir_exists(paths.retired_root) &&
         !player_stage_discard(paths.retired_root))) {
        stage_title_message(message, message_size, "%s",
            "[STAGE_CLEANUP_BLOCKED] An unfinished earlier setup of this game could not be cleared. Close any program that is using the game's folder, then try again.");
        return stage_title_fail(&lock, NULL, summary, NK_ERROR_IO);
    }

    bool replace_existing = false;
    if (nk_platform_dir_exists(paths.final_root)) {
        if (stage_record_matches(paths.final_root, identity, summary) &&
            !stage_tree_missing_entry(paths.final_root, request)) {
            snprintf(prepared_root, prepared_root_size, "%s", paths.final_root);
            if (outcome) *outcome = PLAYER_STAGE_TITLE_REUSED;
            stage_lock_release(&lock);
            return NK_OK;
        }
        memset(summary, 0, sizeof(*summary));
        replace_existing = true;
    } else if (nk_platform_file_exists(paths.final_root)) {
        stage_title_message(message, message_size, "%s",
            "[STAGE_PATH_BLOCKED] A file is in the way of this game's folder in Nakagawa's data folder. Move that file elsewhere, then try again.");
        return stage_title_fail(&lock, NULL, summary, NK_ERROR_ALREADY_EXISTS);
    }

    char detail[256];
    NkResult result = player_stage_game_with_summary(
        request->iso_path, paths.staging_root, request->loose_content_roots,
        request->loose_content_root_count, callbacks, summary, detail,
        sizeof(detail));
    if (result != NK_OK) {
        stage_title_extraction_message(result, detail, message, message_size);
        return stage_title_fail(&lock, NULL, summary, result);
    }

    const char *missing = stage_tree_missing_entry(paths.staging_root, request);
    if (missing && strcmp(missing, "EBOOT.BIN") == 0) {
        stage_title_extraction_message(NK_ERROR_FILE_NOT_FOUND, NULL,
                                       message, message_size);
        return stage_title_fail(&lock, paths.staging_root, summary,
                                NK_ERROR_FILE_NOT_FOUND);
    }
    if (missing) {
        stage_title_message(message, message_size,
            "[STAGE_DATA_FOLDER_MISSING] This disc image has no '%.64s' folder, which this game needs. The image may be incomplete or from a different version of the game; make a fresh copy of your disc and try again.",
            missing);
        return stage_title_fail(&lock, paths.staging_root, summary,
                                NK_ERROR_INVALID_ISO);
    }
    if (!stage_record_write(paths.staging_root, identity, summary)) {
        stage_title_extraction_message(NK_ERROR_IO, NULL, message, message_size);
        return stage_title_fail(&lock, paths.staging_root, summary, NK_ERROR_IO);
    }
    if (stage_title_cancelled(callbacks)) {
        stage_title_extraction_message(NK_ERROR_CANCELLED, NULL, message, message_size);
        return stage_title_fail(&lock, paths.staging_root, summary, NK_ERROR_CANCELLED);
    }

    /* The previous tree moves aside only once its replacement is complete,
     * and comes back if the replacing rename fails. */
    if (replace_existing &&
        !stage_rename_directory(paths.final_root, paths.retired_root)) {
        stage_title_message(message, message_size, "%s",
            "[STAGE_PROMOTE_FAILED] An older copy of this game's files could not be replaced. Close any program that is using the game's folder, then try again.");
        return stage_title_fail(&lock, paths.staging_root, summary, NK_ERROR_IO);
    }
    if (!stage_rename_directory(paths.staging_root, paths.final_root)) {
        if (replace_existing) {
            (void)stage_rename_directory(paths.retired_root, paths.final_root);
        }
        stage_title_message(message, message_size, "%s",
            "[STAGE_PROMOTE_FAILED] The game's files were extracted but could not be moved into place. Close any program that is using the game's folder and check free space, then try again.");
        return stage_title_fail(&lock, paths.staging_root, summary, NK_ERROR_IO);
    }
    /* A replaced tree that cannot be removed now is removed by the next
     * staging of this title; the new tree is already complete and in place. */
    if (replace_existing) (void)player_stage_discard(paths.retired_root);

    snprintf(prepared_root, prepared_root_size, "%s", paths.final_root);
    stage_lock_release(&lock);
    return NK_OK;
}
