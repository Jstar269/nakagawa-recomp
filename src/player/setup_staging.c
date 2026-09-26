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
        remove(destination_path);
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
        remove(destination_path);
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
            int percent = 90 + (int)((discovered * 10u) / 3u);
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
    int percent = 60 + (int)((completed_entries * 40u) / total_entries);
    char current_path[4096];
    snprintf(current_path, sizeof(current_path), "xbdata/%s", entry->path);
    stage_emit(context, current_path, percent, files, total);
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
    int percent = bytes_total == 0 ? 0 : (int)((bytes_complete * 60u) / bytes_total);
    if (percent > 60) percent = 60;
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

    NkResult result = nk_iso_extract_game(iso_path, staging_root, iso_progress,
                                          &context);
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
                           const PlayerStageCallbacks *callbacks,
                           char *error_message, size_t error_message_size) {
    return player_stage_game_with_summary(iso_path, staging_root, callbacks,
                                          NULL, error_message,
                                          error_message_size);
}
