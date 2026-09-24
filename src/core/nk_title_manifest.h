/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_TITLE_MANIFEST_H
#define NK_TITLE_MANIFEST_H

#include "generated/nk_title_catalog.h"
#include "nk_types.h"
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

typedef enum {
    NK_RUNTIME_PACKAGE_OK = 0,
    NK_RUNTIME_PACKAGE_MISSING,
    NK_RUNTIME_PACKAGE_INCOMPATIBLE,
    NK_RUNTIME_PACKAGE_STALE
} NkRuntimePackageStatus;

typedef struct {
    char package_root[NK_MAX_PATH];
    char executable_path[NK_MAX_PATH];
    char image_path[NK_MAX_PATH];
} NkRuntimePackageInfo;

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

/* Create a generic, user-local experimental profile for an uncatalogued PSP
 * disc. The profile is written below <user_data_root>/experimental/<DISC_ID>,
 * its embedded canonical manifest is validated by the native manifest parser,
 * and executable hashes are retained only in that user data directory.
 */
bool nk_title_manifest_write_experimental_profile(
    const char *iso_path,
    bool param_sfo_parsed,
    const char *disc_id,
    const char *title,
    const char *selected_executable,
    const char *user_data_root,
    char *out_profile_id,
    size_t out_profile_id_len,
    char *error_buf,
    size_t error_buf_len
);

/* Read the private experimental profile for one library identity. The embedded
 * manifest is fully validated and returned from bounded native overlay
 * storage; executable SHA-256 and selected-executable identity are returned
 * only to the caller. */
bool nk_title_manifest_read_experimental_profile(
    const char *user_data_root,
    const char *disc_id,
    const char *title_id,
    const char *selected_executable,
    NkTitleEntry *out_title,
    char out_executable_sha256[65],
    char *error_buf,
    size_t error_buf_len
);

/* Validate the v1 generated package at <user_data_root>/packages/<DISC_ID>.
 * Experimental entries are additionally bound to their private profile's
 * selected executable hash. `player_abi_version` comes from recomp.h. */
NkRuntimePackageStatus nk_title_manifest_validate_aot_package(
    const char *user_data_root,
    const char *disc_id,
    const char *title_id,
    bool is_experimental,
    const char *selected_executable,
    uint32_t player_abi_version,
    NkRuntimePackageInfo *out_info,
    char *reason,
    size_t reason_size
);

#ifdef __cplusplus
}
#endif

#endif /* NK_TITLE_MANIFEST_H */
