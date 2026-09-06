/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_TITLE_MANIFEST_H
#define NK_TITLE_MANIFEST_H

#include "generated/nk_title_catalog.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load and validate an external canonical title manifest JSON file, registering it
 * as an in-memory overlay in nk_title_catalog.
 *
 * Conforms to the canonical manifest schema (tools/title_manifest.py schema_version 1).
 *
 * Returns true if the manifest was parsed and validated successfully.
 * On failure, returns false and fills error_buf with a descriptive message.
 */
bool nk_title_manifest_load_overlay(
    const char *manifest_path,
    char *error_buf,
    size_t error_buf_len
);

#ifdef __cplusplus
}
#endif

#endif /* NK_TITLE_MANIFEST_H */
