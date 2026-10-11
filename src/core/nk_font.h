/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_FONT_H
#define NK_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../rt/pgf_api.h"
#include "nk_font_slots.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NK_FONT_STATUS_OK = 0,
    NK_FONT_STATUS_MISSING,
    NK_FONT_STATUS_INVALID
} NkFontStatus;

/* Where a slot's font comes from. Served in this order: the user's imported font, then the
 * project's font, then a named refusal (NK_FONT_SOURCE_NONE). */
typedef enum {
    NK_FONT_SOURCE_NONE = 0,
    NK_FONT_SOURCE_USER,
    NK_FONT_SOURCE_PROJECT
} NkFontSource;

#define NK_FONT_DETAIL_MAX 512
/* Plain-language status text: a per-slot detail or a per-file outcome. */
#define NK_FONT_TEXT_MAX 1024
#define NK_FONT_IMPORT_MAX_FILES 64

typedef struct {
    NkFontSource source;
    char detail[NK_FONT_TEXT_MAX];
} NkFontSlotState;

/* One PGF file read and checked: the reader's verdict, the file's size and SHA-256, and the
 * slot it names (or -1 with the reason in slot_reason). */
typedef struct {
    uint64_t size;
    char sha256[65];
    PgfVerdict verdict;
    int slot;
    char slot_reason[NK_FONT_DETAIL_MAX];
} NkFontInspection;

/* Read one PGF file (at most NK_FONT_PGF_MAX_BYTES) and check it with the native reader and
 * the slot rule. Returns false with out_error naming the failure. */
bool nk_font_inspect_pgf(const char *file_path, NkFontInspection *out,
                         char *out_error, size_t error_len);

/* Structurally validate a PGF font file with the native reader. Computes the file size and
 * lowercase SHA-256 hex string (65 bytes including NUL). Returns false with a diagnostic
 * in out_error when the reader refuses the file. */
bool nk_font_validate_pgf(const char *file_path, uint64_t *out_size,
                          char *out_sha256, char *out_error, size_t error_len);

/* Retrieve the per-user font cache directory (<user_data_root>/fonts/v2). If user_data_root
 * is NULL or empty, queries nk_platform_get_path(NK_PATH_DATA). */
bool nk_font_get_cache_dir(const char *user_data_root, char *out_dir, size_t max_len);

/* Check the imported-font manifest in the cache. MISSING: no manifest. INVALID: the manifest
 * or a listed file is malformed, mismatched, or refused by the reader. OK: every listed
 * file validates and names its slot. */
NkFontStatus nk_font_check_cache(const char *user_data_root,
                                 char *out_message,
                                 size_t message_max_len);

/* Per-slot preflight: the user's imported font when it validates, else the project font
 * at <project_root>/font/<slot file> when it validates, else NONE with the reason. */
void nk_font_slot_states(const char *user_data_root, const char *project_root,
                         NkFontSlotState states[NK_FONT_SLOT_COUNT]);

/* Name the slot for an accepted verdict (the rule the import flow applies). Returns the
 * slot, or -1 with the reason in reason. */
int nk_font_classify_verdict(const PgfVerdict *verdict, char *reason, size_t reason_len);

/* One file the import considered. imported is true when this file's bytes now serve the
 * slot named by slot. */
typedef struct {
    char name[256];
    int slot;
    bool imported;
    char detail[NK_FONT_TEXT_MAX];
} NkFontFileOutcome;

typedef struct {
    int file_count;
    NkFontFileOutcome files[NK_FONT_IMPORT_MAX_FILES];
    bool slot_imported[NK_FONT_SLOT_COUNT];
    int imported_count;
} NkFontImportResult;

/* Import the .pgf files directly inside folder (not subfolders), whatever their names. Each
 * file is read once, checked by the native reader, and named to a slot by its coverage.
 * choose[slot], when not NULL, names the file to use for a slot that several files name.
 * A slot named by several files with no choice is not imported. Chosen files are copied
 * into the cache as the slot's file and the manifest is rewritten; originals are not
 * touched. Returns false with out_error when the folder cannot be read or holds no .pgf. */
bool nk_font_import_folder(const char *user_data_root, const char *folder,
                           const char *const choose[NK_FONT_SLOT_COUNT],
                           NkFontImportResult *result, char *out_error, size_t error_len);

/* Remove the imported font for each slot with remove[slot] set. Deletes only the cache's slot
 * files. A slot's manifest entry is dropped once its file is gone, deleted now or already
 * absent; a file that cannot be deleted keeps its entry, and the other named slots are still
 * processed. The manifest is rewritten only when an entry was dropped, and removed when no
 * entry is left. Returns the number of slot files removed. Returns -1 with out_error when a
 * named file could not be deleted or the manifest could not be rewritten or removed; out_error
 * then names each file that stayed and the manifest problem, and gives the count of files that
 * were removed when there were any. */
int nk_font_remove_imports(const char *user_data_root,
                           const bool remove[NK_FONT_SLOT_COUNT],
                           char *out_error, size_t error_len);

/* Resolve the active font directory for runtime launch. If the cache manifest is OK,
 * resolves <user_data_root>/fonts/v2; else <fallback_root>/font when it exists. */
bool nk_font_resolve_directory(const char *user_data_root,
                               const char *fallback_root,
                               char *out_dir,
                               size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* NK_FONT_H */
