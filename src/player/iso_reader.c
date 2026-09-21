/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "iso_reader.h"
#include "nk_iso.h"
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
    if (meta.matched_title && meta.matched_title->id) {
        snprintf(out_result->matched_title_id, sizeof(out_result->matched_title_id), "%s", meta.matched_title->id);
    }
    out_result->status = (GameSupportStatus)meta.status;
    out_result->success = true;

    return true;
}
