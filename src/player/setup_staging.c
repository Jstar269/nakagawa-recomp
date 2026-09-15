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
           MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wide_count) > 0;
}

static bool discard_tree_wide(const WCHAR *path) {
    WCHAR pattern[32768];
    int pattern_length = _snwprintf(pattern, sizeof(pattern) / sizeof(pattern[0]),
                                    L"%ls\\*", path);
    if (pattern_length < 0 || (size_t)pattern_length >= sizeof(pattern) / sizeof(pattern[0])) return false;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    bool okay = true;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        WCHAR child[32768];
        int child_length = _snwprintf(child, sizeof(child) / sizeof(child[0]),
                                      L"%ls\\%ls", path, data.cFileName);
        if (child_length < 0 || (size_t)child_length >= sizeof(child) / sizeof(child[0])) {
            okay = false;
            break;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!discard_tree_wide(child)) okay = false;
        } else if (!DeleteFileW(child)) {
            okay = false;
        }
    } while (okay && FindNextFileW(find, &data));
    FindClose(find);
    if (!okay) return false;
    return RemoveDirectoryW(path) != 0 || GetLastError() == ERROR_PATH_NOT_FOUND;
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

    int child_fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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

    int root_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return DeleteFileW(wide) != 0;
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

static bool make_prx_source_path(const char *root, const char *file_name,
                                 bool include_prx_directory,
                                 char *out_path, size_t out_size) {
    if (!root || !root[0] || !file_name || !file_name[0] || !out_path || out_size == 0) return false;
    const char *suffix = include_prx_directory ? "PPSSPP/PSP/SYSTEM/DUMP/PRX/%s"
                                               : "PPSSPP/PSP/SYSTEM/DUMP/%s";
    int written = snprintf(out_path, out_size, "%s%c%s", root,
                           nk_platform_path_separator(), "");
    if (written < 0 || (size_t)written >= out_size) return false;
    int remaining = (int)(out_size - (size_t)written);
    int suffix_written = snprintf(out_path + written, (size_t)remaining,
                                  suffix, file_name);
    return suffix_written >= 0 && (size_t)suffix_written < (size_t)remaining;
}

static bool make_userprofile_prx_path(const char *root, const char *file_name,
                                      bool include_prx_directory,
                                      char *out_path, size_t out_size) {
    if (!root || !root[0] || !file_name || !file_name[0] || !out_path || out_size == 0) return false;
    const char *suffix = include_prx_directory
        ? "Documents/PPSSPP/PSP/SYSTEM/DUMP/PRX/%s"
        : "Documents/PPSSPP/PSP/SYSTEM/DUMP/%s";
    int written = snprintf(out_path, out_size, "%s%c%s", root,
                           nk_platform_path_separator(), "");
    if (written < 0 || (size_t)written >= out_size) return false;
    int remaining = (int)(out_size - (size_t)written);
    int suffix_written = snprintf(out_path + written, (size_t)remaining,
                                  suffix, file_name);
    return suffix_written >= 0 && (size_t)suffix_written < (size_t)remaining;
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
    if (!nk_platform_mkdir_p(parent)) {
        stage_error(context, NK_ERROR_IO, "cannot create decrypted PRX staging directory");
        return false;
    }

    FILE *source = stage_fopen(source_path, "rb");
    FILE *destination = stage_fopen(destination_path, "wb");
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
    const char *appdata = getenv("APPDATA");
    const char *userprofile = getenv("USERPROFILE");
    size_t discovered = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (stage_cancelled(context)) return false;
        char source_path[4096];
        source_path[0] = '\0';
        if (appdata && appdata[0]) {
            if (make_prx_source_path(appdata, names[i], false,
                                     source_path, sizeof(source_path)) &&
                !nk_platform_file_exists(source_path)) {
                make_prx_source_path(appdata, names[i], true,
                                     source_path, sizeof(source_path));
            }
        }
        if ((!source_path[0] || !nk_platform_file_exists(source_path)) &&
            userprofile && userprofile[0]) {
            if (!make_userprofile_prx_path(userprofile, names[i], false,
                                           source_path, sizeof(source_path)) ||
                !nk_platform_file_exists(source_path)) {
                make_userprofile_prx_path(userprofile, names[i], true,
                                         source_path, sizeof(source_path));
            }
        }
        if (!source_path[0] || !nk_platform_file_exists(source_path)) continue;

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
    if (!iso_path || !staging_root || !staging_root[0]) return NK_ERROR_GENERIC;
    if (nk_platform_dir_exists(staging_root)) {
        if (error_message && error_message_size > 0) {
            snprintf(error_message, error_message_size,
                     "staging directory already exists; refusing to reuse it");
        }
        return NK_ERROR_ALREADY_EXISTS;
    }
    if (!nk_platform_mkdir_p(staging_root)) {
        if (error_message && error_message_size > 0) {
            snprintf(error_message, error_message_size, "cannot create staging directory");
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
                 context.error_message[0] ? context.error_message : "game staging failed");
    }
    if (result == NK_OK && !discover_prx_for_stage(&context)) {
        result = context.callback_result != NK_OK ? context.callback_result : NK_ERROR_IO;
        if (error_message && error_message_size > 0) {
            snprintf(error_message, error_message_size, "%s",
                     context.error_message[0] ? context.error_message : "PRX discovery failed");
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
