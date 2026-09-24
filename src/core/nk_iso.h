/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_ISO_H
#define NK_ISO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifndef NK_ISO_NO_PLAYER_EXTRAS
#include "nk_types.h"
#include "generated/nk_title_catalog.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Low-level / VFS ISO reader interface */
typedef struct NkIsoReader NkIsoReader;

typedef struct {
    char name[256];
    uint32_t lba;
    uint32_t size;
    bool is_directory;
} NkIsoDirEntry;

/* Open an ISO image for reading. Returns NULL on failure. */
NkIsoReader *nk_iso_reader_open(const char *iso_path);

/* Close an open ISO reader. */
void nk_iso_reader_close(NkIsoReader *reader);

/* Resolve a path (e.g. "PSP_GAME/PARAM.SFO") to its extent LBA and size.
 * If out_is_dir is non-NULL, sets whether it is a directory.
 * Returns 0 on success, -1 if not found.
 */
int nk_iso_reader_lookup(NkIsoReader *reader, const char *path, uint32_t *out_lba, uint32_t *out_size, bool *out_is_dir);

/* Read bytes from extent at lba + offset into dst.
 * Returns bytes read, or -1 on error.
 */
int nk_iso_reader_read(NkIsoReader *reader, uint32_t lba, uint64_t offset, void *dst, uint32_t bytes);

/* Return the directory entry at 0-based index in directory at dir_path.
 * Returns 1 on success, 0 on end of directory, -1 on invalid / not a directory.
 */
int nk_iso_reader_list(NkIsoReader *reader, const char *dir_path, uint32_t index, NkIsoDirEntry *out_entry);

/* Return volume ID of reader */
const char *nk_iso_reader_volume_id(const NkIsoReader *reader);

/* Return total file size in bytes */
uint64_t nk_iso_reader_file_size(const NkIsoReader *reader);

#ifndef NK_ISO_NO_PLAYER_EXTRAS
typedef enum {
    NK_ISO_EXEC_UNKNOWN = 0,
    NK_ISO_EXEC_MIPS_ELF32,
    NK_ISO_EXEC_PSP_ENCRYPTED,
    NK_ISO_EXEC_SCE_WRAPPER,
    NK_ISO_EXEC_EMPTY_OR_ZERO,
    NK_ISO_EXEC_PBP
} NkIsoExecutableKind;

typedef enum {
    NK_ISO_EXEC_SELECTION_NONE = 0,
    NK_ISO_EXEC_SELECTION_EBOOT,
    NK_ISO_EXEC_SELECTION_BOOT
} NkIsoExecutableSelection;

typedef struct {
    NkIsoExecutableKind kind;
    uint32_t size_bytes;
    bool present;
} NkIsoExecutableCandidate;

typedef struct {
    NkIsoExecutableCandidate eboot;
    NkIsoExecutableCandidate boot;
    NkIsoExecutableSelection selected;
    char selected_path[16];
    bool boot_fallback;
} NkIsoExecutableReport;

typedef struct {
    char disc_id[NK_MAX_DISC_ID_LEN];
    char title_name[NK_MAX_TITLE_LEN];
    char disc_version[16];
    char volume_id[33];
    int64_t file_size_bytes;
    bool is_supported;
    bool param_sfo_parsed;
    NkIsoExecutableReport executables;
    const NkTitleEntry *matched_title;
    NkGameSupportStatus status;
    char error_message[256];
} NkIsoMetadata;

/* Inspect a raw PSP ISO9660 disc image directly.
 * Parses PVD, locates PSP_GAME/PARAM.SFO, extracts metadata,
 * and matches against the generated native title catalog.
 */
NkResult nk_iso_inspect(const char *iso_path, NkIsoMetadata *out_meta);

/* Parse an in-memory PARAM.SFO buffer and extract metadata.
 * Returns true if valid, false if malformed or duplicate conflict.
 */
bool nk_iso_parse_sfo_buffer(const uint8_t *sfo, size_t sfo_size, NkIsoMetadata *meta);

/* Classify PSP_GAME/SYSDIR/EBOOT.BIN and BOOT.BIN using bounded reads from
 * the ISO. A validated plaintext MIPS ELF32 is selected for analysis; BOOT.BIN
 * is selected only when EBOOT.BIN is encrypted and BOOT.BIN is plaintext ELF. */
NkResult nk_iso_classify_executables(const char *iso_path,
                                     NkIsoExecutableReport *out_report);

/* Extract a file from an ISO9660 disc image to the host filesystem.
 * disc_rel_path: path inside ISO (e.g. "PSP_GAME/PARAM.SFO" or "PSP_GAME/ICON0.PNG")
 * host_dest_path: destination file path on host
 */
NkResult nk_iso_extract_file(const char *iso_path, const char *disc_rel_path, const char *host_dest_path);

/* Progress callback used by the first-time setup staging route. The callback
 * is invoked while a selected file is copied and once more after that file is
 * closed successfully. Returning false aborts the walk with NK_ERROR_CANCELLED.
 * `bytes_complete` and `bytes_total` are cumulative across the selected
 * EBOOT.BIN and PSP_GAME/USRDIR/xbdata tree. */
typedef bool (*NkIsoProgressCallback)(const char *relative_path,
                                      uint64_t bytes_complete,
                                      uint64_t bytes_total,
                                      size_t files_complete,
                                      size_t total_files,
                                      void *userdata);

/* Extract only the game payload needed by the native player into a caller
 * owned staging root: EBOOT.BIN and every regular file below
 * PSP_GAME/USRDIR/xbdata. The ISO is walked from its own directory records;
 * no host-side directory listing or external extractor is involved. */
NkResult nk_iso_extract_game(const char *iso_path, const char *host_root,
                             NkIsoProgressCallback progress, void *userdata);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NK_ISO_H */
