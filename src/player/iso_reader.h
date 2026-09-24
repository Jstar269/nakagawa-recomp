/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_ISO_READER_H
#define NAKAGAWA_ISO_READER_H

#include "player_state.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool success;
    char disc_id[MAX_DISC_ID_LEN];
    char title_name[MAX_TITLE_LEN];
    char disc_version[16];
    char error_message[256];
    uint64_t file_size;
    bool is_supported;
    bool param_sfo_parsed;
    NkIsoExecutableReport executables;
    char matched_title_id[64];
    GameSupportStatus status;
} IsoInspectResult;

/* Inspects a raw PSP ISO9660 disc image directly */
bool iso_inspect_file(const char *iso_path, IsoInspectResult *out_result);

/* Icon and image loading from ISO disc image */
typedef enum {
    NK_ICON_OK = 0,
    NK_ICON_ERR_MISSING,
    NK_ICON_ERR_OVERSIZED,
    NK_ICON_ERR_CORRUPT,
    NK_ICON_ERR_INVALID_PARAM
} NkIconStatus;

#define NK_ICON_MAX_BYTES (1024 * 1024) /* 1 MiB bound */

/* Validates a PNG buffer in memory:
 * - Checks PNG 8-byte signature: \x89PNG\r\n\x1a\n
 * - Parses IHDR chunk (length 13, type "IHDR")
 * - Extracts width and height (big-endian 32-bit unsigned int)
 * - Validates dimensions: width > 0 && height > 0 && width <= 2048 && height <= 2048
 * Returns NK_ICON_OK on success, or NK_ICON_ERR_CORRUPT.
 */
NkIconStatus nk_iso_validate_png_header(const uint8_t *data, size_t size, uint32_t *out_w, uint32_t *out_h);

/* Reads an image file (e.g. "PSP_GAME/ICON0.PNG" or "PSP_GAME/PIC1.PNG")
 * from an ISO9660 disc image into memory.
 * - Enforces 1 MiB size limit (returns NK_ICON_ERR_OVERSIZED if > 1 MiB).
 * - Validates PNG header and dimensions.
 * - Allocates *out_data with malloc (caller must free()).
 */
NkIconStatus nk_iso_read_image_entry(const char *iso_path, const char *rel_path,
                                     uint8_t **out_data, size_t *out_size,
                                     uint32_t *out_w, uint32_t *out_h);

#endif /* NAKAGAWA_ISO_READER_H */
