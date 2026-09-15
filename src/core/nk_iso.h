/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_ISO_H
#define NK_ISO_H

#include "nk_types.h"
#include "generated/nk_title_catalog.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char disc_id[NK_MAX_DISC_ID_LEN];
    char title_name[NK_MAX_TITLE_LEN];
    char disc_version[16];
    char volume_id[33];
    int64_t file_size_bytes;
    bool is_supported;
    const NkTitleEntry *matched_title;
    NkGameSupportStatus status;
    char error_message[256];
} NkIsoMetadata;

/* Inspect a raw PSP ISO9660 disc image directly.
 * Parses PVD, locates PSP_GAME/PARAM.SFO, extracts metadata,
 * and matches against the generated native title catalog.
 */
NkResult nk_iso_inspect(const char *iso_path, NkIsoMetadata *out_meta);

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

#ifdef __cplusplus
}
#endif

#endif /* NK_ISO_H */
