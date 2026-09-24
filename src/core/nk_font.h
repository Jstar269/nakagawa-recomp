/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_FONT_H
#define NK_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NK_FONT_STATUS_OK = 0,
    NK_FONT_STATUS_MISSING,
    NK_FONT_STATUS_INVALID
} NkFontStatus;

/* Retrieve canonical font cache directory (<user_data_root>/fonts/v1).
 * If user_data_root is NULL or empty, queries nk_platform_get_path(NK_PATH_DATA). */
bool nk_font_get_cache_dir(const char *user_data_root, char *out_dir, size_t max_len);

/* Structurally validate a PGF font file (magic, header size, bounds).
 * Computes the file size and lowercase SHA-256 hex string (65 bytes including NUL).
 * Returns true if valid; false on corruption/truncation with diagnostic in out_error. */
bool nk_font_validate_pgf(const char *file_path, uint64_t *out_size,
                          char *out_sha256, char *out_error, size_t error_len);

/* Check the system fonts availability in the local font cache and fallback path.
 * Checks <user_data_root>/fonts/v1/manifest.json.
 * If manifest is valid and files match -> returns NK_FONT_STATUS_OK.
 * If manifest is missing -> checks fallback <fallback_root>/font/jpn0.pgf.
 *   If fallback exists -> returns NK_FONT_STATUS_OK.
 *   If fallback missing -> returns NK_FONT_STATUS_MISSING.
 * If manifest exists but is invalid/corrupt -> returns NK_FONT_STATUS_INVALID.
 * Diagnostics and recommended next step are written to out_message. */
NkFontStatus nk_font_check_cache(const char *user_data_root,
                                 const char *fallback_root,
                                 char *out_message,
                                 size_t message_max_len);

/* Resolve the active font directory for runtime launch.
 * If cache is OK, resolves <user_data_root>/fonts/v1.
 * Else if fallback exists, resolves <fallback_root>/font.
 * Returns true if a font directory was resolved. */
bool nk_font_resolve_directory(const char *user_data_root,
                               const char *fallback_root,
                               char *out_dir,
                               size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* NK_FONT_H */
