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

/* Security Limit Classification:
 * The 256 KB manifest ceiling is NOT a PSP hardware or UMD format limit.
 * LIMIT_CLASS = PRODUCT_SECURITY_POLICY
 *
 * Rationale:
 * Canonical Nakagawa title manifests define declarative title bindings (executable
 * entry, module definitions, filesystem data roots, memory stick paths). The largest
 * canonical repository manifest is ~4 KB. A 256 KB limit provides a >50x safety margin
 * for future title bindings while preventing hostile memory exhaustion and unbounded
 * JSON parsing attacks.
 */
#define NK_MANIFEST_MAX_BYTES (256 * 1024)
#define NK_MANIFEST_LIMIT_CLASS "PRODUCT_SECURITY_POLICY"
#define NK_MANIFEST_MAX_JSON_DEPTH 16

/* Parse and validate a manifest from a memory buffer.
 * If allow_override is false, collisions with public catalog entries are rejected.
 * If allow_override is true, collisions generate a noisy warning and are accepted.
 */
bool nk_title_manifest_parse_buffer(
    const char *json_str,
    size_t json_len,
    bool allow_override,
    NkTitleEntry *out_entry,
    char *error_buf,
    size_t error_buf_len
);

/* Load and validate an external canonical title manifest JSON file, registering it
 * as an in-memory overlay in nk_title_catalog.
 *
 * Conforms strictly to the canonical manifest schema (tools/title_manifest.py schema_version 1).
 *
 * Returns true if the manifest was parsed and validated successfully.
 * On failure, returns false and fills error_buf with a descriptive message.
 */
bool nk_title_manifest_load_overlay(
    const char *manifest_path,
    char *error_buf,
    size_t error_buf_len
);

bool nk_title_manifest_load_overlay_ext(
    const char *manifest_path,
    bool allow_override,
    char *error_buf,
    size_t error_buf_len
);

#ifdef __cplusplus
}
#endif

#endif /* NK_TITLE_MANIFEST_H */
