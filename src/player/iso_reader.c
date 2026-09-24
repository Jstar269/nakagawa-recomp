/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "iso_reader.h"
#include "nk_iso.h"
#include <stdlib.h>
#include <string.h>

bool iso_inspect_file(const char *iso_path, IsoInspectResult *out_result) {
    if (!iso_path || !out_result) return false;
    memset(out_result, 0, sizeof(*out_result));

    NkIsoMetadata meta;
    NkResult res = nk_iso_inspect(iso_path, &meta);

    if (res != NK_OK) {
        snprintf(out_result->error_message, sizeof(out_result->error_message), "%s", meta.error_message);
        out_result->success = false;
        return false;
    }

    snprintf(out_result->disc_id, sizeof(out_result->disc_id), "%s", meta.disc_id);
    snprintf(out_result->title_name, sizeof(out_result->title_name), "%s", meta.title_name);
    snprintf(out_result->disc_version, sizeof(out_result->disc_version), "%s", meta.disc_version);
    out_result->file_size = (uint64_t)meta.file_size_bytes;
    out_result->is_supported = meta.is_supported;
    out_result->param_sfo_parsed = meta.param_sfo_parsed;
    out_result->executables = meta.executables;
    if (meta.matched_title && meta.matched_title->id) {
        snprintf(out_result->matched_title_id, sizeof(out_result->matched_title_id), "%s", meta.matched_title->id);
    }
    out_result->status = (GameSupportStatus)meta.status;
    out_result->success = true;

    return true;
}

NkIconStatus nk_iso_validate_png_header(const uint8_t *data, size_t size, uint32_t *out_w, uint32_t *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!data || size < 33) {
        return NK_ICON_ERR_CORRUPT;
    }

    /* PNG 8-byte signature: 0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A */
    static const uint8_t kPngMagic[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (memcmp(data, kPngMagic, 8) != 0) {
        return NK_ICON_ERR_CORRUPT;
    }

    /* First chunk must be IHDR: chunk data length must be 13 */
    uint32_t chunk_len = ((uint32_t)data[8] << 24) |
                         ((uint32_t)data[9] << 16) |
                         ((uint32_t)data[10] << 8) |
                         (uint32_t)data[11];
    if (chunk_len != 13) {
        return NK_ICON_ERR_CORRUPT;
    }

    /* Chunk type must be "IHDR" */
    if (memcmp(data + 12, "IHDR", 4) != 0) {
        return NK_ICON_ERR_CORRUPT;
    }

    /* Extract width & height (big-endian 32-bit uint) */
    uint32_t w = ((uint32_t)data[16] << 24) |
                 ((uint32_t)data[17] << 16) |
                 ((uint32_t)data[18] << 8) |
                 (uint32_t)data[19];
    uint32_t h = ((uint32_t)data[20] << 24) |
                 ((uint32_t)data[21] << 16) |
                 ((uint32_t)data[22] << 8) |
                 (uint32_t)data[23];

    /* Bound and validate dimensions: reasonable bounds for PSP icons and wallpapers */
    if (w == 0 || h == 0) {
        return NK_ICON_ERR_CORRUPT;
    }
    if (w > 2048 || h > 2048) {
        return NK_ICON_ERR_OVERSIZED;
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return NK_ICON_OK;
}

NkIconStatus nk_iso_read_image_entry(const char *iso_path, const char *rel_path,
                                     uint8_t **out_data, size_t *out_size,
                                     uint32_t *out_w, uint32_t *out_h) {
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;

    if (!iso_path || !iso_path[0] || !rel_path || !rel_path[0] || !out_data || !out_size) {
        return NK_ICON_ERR_INVALID_PARAM;
    }

    NkIsoReader *reader = nk_iso_reader_open(iso_path);
    if (!reader) {
        return NK_ICON_ERR_MISSING;
    }

    uint32_t lba = 0;
    uint32_t size = 0;
    bool is_dir = false;
    int rc = nk_iso_reader_lookup(reader, rel_path, &lba, &size, &is_dir);
    if (rc != 0 || is_dir) {
        nk_iso_reader_close(reader);
        return NK_ICON_ERR_MISSING;
    }

    if (size == 0) {
        nk_iso_reader_close(reader);
        return NK_ICON_ERR_CORRUPT;
    }

    if (size > NK_ICON_MAX_BYTES) {
        nk_iso_reader_close(reader);
        return NK_ICON_ERR_OVERSIZED;
    }

    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        nk_iso_reader_close(reader);
        return NK_ICON_ERR_CORRUPT;
    }

    int bytes_read = nk_iso_reader_read(reader, lba, 0, buf, size);
    nk_iso_reader_close(reader);

    if (bytes_read != (int)size) {
        free(buf);
        return NK_ICON_ERR_CORRUPT;
    }

    NkIconStatus val_res = nk_iso_validate_png_header(buf, size, out_w, out_h);
    if (val_res != NK_ICON_OK) {
        free(buf);
        return val_res;
    }

    *out_data = buf;
    *out_size = size;
    return NK_ICON_OK;
}
