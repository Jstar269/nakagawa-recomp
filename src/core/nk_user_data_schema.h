/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Versioned per-user files: the rule each writer of a persistent user file
 * (library.json, settings.json, the controller and keyboard profile) applies
 * before it replaces that file, so that no update, and no return to an older
 * player, loses what another player version wrote.
 *
 * Every such file is a JSON object with an integer "schema_version". Its reader
 * migrates an older version forward one step at a time (v1 -> v2 -> ... ->
 * current) and refuses a newer one by name. Before a writer replaces the file:
 *
 * - a file at this build's version is replaced as usual;
 * - a file at an OLDER version is first copied, byte for byte, to
 *   "<file>.v<version>.bak". The copy is complete and in place before the
 *   replacement starts, a failed copy stops the write, and an earlier backup
 *   of the same version is replaced by the newer original;
 * - a file at a NEWER version is never replaced: a later player wrote it, and
 *   this one cannot know what a rewrite would drop (NK_ERROR_NEWER_SCHEMA);
 * - a file with no readable version (empty, corrupt, not JSON, over the size
 *   bound) is replaced: its reader has already reported it and fallen back.
 *
 * The rule lives in the writer, not in reader state, so it holds whichever path
 * reaches the write. docs/RUNTIME_PACKAGING_ARCHITECTURE.md ("Per-user data
 * formats and versions") lists each file and its versions.
 *
 * Header-only: every translation unit that writes one of these files already
 * links nk_json.c and the platform layer, and no build list needs another
 * source. */

#ifndef NK_USER_DATA_SCHEMA_H
#define NK_USER_DATA_SCHEMA_H

#include "nk_json.h"
#include "nk_platform.h"
#include "nk_types.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* nk_user_data_file_version results that are not a version. */
#define NK_USER_DATA_VERSION_ABSENT (-1L)
#define NK_USER_DATA_VERSION_UNREADABLE (-2L)

/* The top-level integer schema_version of the JSON file at `path`, read under a
 * `max_bytes` bound; NK_USER_DATA_VERSION_ABSENT when there is no such file and
 * NK_USER_DATA_VERSION_UNREADABLE when it cannot be read as such an object. */
static inline long nk_user_data_file_version(const char *path, size_t max_bytes) {
    if (!path || !path[0]) return NK_USER_DATA_VERSION_UNREADABLE;
    if (!nk_platform_file_exists(path)) return NK_USER_DATA_VERSION_ABSENT;
    FILE *file = nk_fopen_utf8(path, "rb");
    if (!file) return NK_USER_DATA_VERSION_UNREADABLE;
    char *text = (char *)malloc(max_bytes + 1u);
    if (!text) {
        fclose(file);
        return NK_USER_DATA_VERSION_UNREADABLE;
    }
    size_t length = fread(text, 1u, max_bytes + 1u, file);
    bool read_failed = ferror(file) != 0;
    fclose(file);
    long version = NK_USER_DATA_VERSION_UNREADABLE;
    if (!read_failed && length > 0u && length <= max_bytes) {
        char error[64];
        NkJsonNode *root = nk_json_parse(text, length, error, sizeof(error));
        int64_t value = 0;
        if (root && nk_json_is_object(root) &&
            nk_json_get_int64(nk_json_obj_get(root, "schema_version"), &value) &&
            value >= 0 && value <= 1000000) {
            version = (long)value;
        }
        if (root) nk_json_free(root);
    }
    free(text);
    return version;
}

/* "<path>.v<version>.bak", the name an older file is kept under. */
static inline bool nk_user_data_backup_path(const char *path, long version, char *out,
                                            size_t out_size) {
    if (!path || !out || out_size == 0u || version < 0) return false;
    int written = snprintf(out, out_size, "%s.v%ld.bak", path, version);
    return written > 0 && (size_t)written < out_size;
}

/* Copies `path` to its version backup through a temporary sibling, so a backup
 * is either the complete original or not there. */
static inline bool nk_user_data_backup_version(const char *path, long version) {
    char backup[NK_MAX_PATH + 32];
    char temporary[NK_MAX_PATH + 40];
    if (!nk_user_data_backup_path(path, version, backup, sizeof(backup))) return false;
    int written = snprintf(temporary, sizeof(temporary), "%s.tmp", backup);
    if (written <= 0 || (size_t)written >= sizeof(temporary)) return false;
    FILE *source = nk_fopen_utf8(path, "rb");
    if (!source) return false;
    FILE *copy = nk_fopen_utf8(temporary, "wb");
    if (!copy) {
        fclose(source);
        return false;
    }
    bool copied = true;
    char block[8192];
    size_t length;
    while ((length = fread(block, 1u, sizeof(block), source)) > 0u) {
        if (fwrite(block, 1u, length, copy) != length) {
            copied = false;
            break;
        }
    }
    if (ferror(source)) copied = false;
    fclose(source);
    if (fflush(copy) != 0) copied = false;
    if (fclose(copy) != 0) copied = false;
    if (copied && nk_rename_utf8(temporary, backup) == 0) return true;
    (void)nk_remove_utf8(temporary);
    return false;
}

/* Applies the rule above to the file a writer is about to replace with one at
 * `current_version`. NK_OK lets the write go ahead (after any backup);
 * NK_ERROR_NEWER_SCHEMA refuses it for a newer file and NK_ERROR_IO for an
 * older one whose backup could not be made. `out_found_version`, when given,
 * receives what nk_user_data_file_version read. */
static inline NkResult nk_user_data_prepare_replace(const char *path, long current_version,
                                                    size_t max_bytes,
                                                    long *out_found_version) {
    long found = nk_user_data_file_version(path, max_bytes);
    if (out_found_version) *out_found_version = found;
    if (found < 0 || found == current_version) return NK_OK;
    if (found > current_version) return NK_ERROR_NEWER_SCHEMA;
    return nk_user_data_backup_version(path, found) ? NK_OK : NK_ERROR_IO;
}

#endif /* NK_USER_DATA_SCHEMA_H */
