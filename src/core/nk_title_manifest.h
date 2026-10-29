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
/* Keep equal to tools/nk_core/package_cache.py:MAX_BUILD_REPORT_JSON_BYTES. */
#define NK_BUILD_REPORT_MAX_BYTES (4 * 1024 * 1024)
#define NK_MANIFEST_LIMIT_CLASS "PRODUCT_SECURITY_POLICY"
#define NK_MANIFEST_MAX_JSON_DEPTH 16
#define NK_AOT_PACKAGE_SCHEMA_VERSION 2
#define NK_AOT_CACHE_SCHEMA_VERSION 2
#define NK_AOT_COMPLETION_SCHEMA_VERSION 2
/* Keep equal to tools/nk_core/package_cache.py GENERATED_CODE_ABI_EPOCH and RUNTIME_ABI_EPOCH;
 * both follow the CpuState layout version (CPUSTATE_ABI_EPOCHS there, pinned by
 * tools/test_package_cache.py): epoch 2 = CpuState ABI v3. */
#define NK_AOT_GENERATED_CODE_ABI_EPOCH 2
#define NK_AOT_RUNTIME_ABI_EPOCH 2
#define NK_AOT_COMPLETION_MANIFEST "completion-manifest.json"

typedef enum {
    NK_RUNTIME_PACKAGE_OK = 0,
    NK_RUNTIME_PACKAGE_MISSING,
    NK_RUNTIME_PACKAGE_INCOMPATIBLE,
    NK_RUNTIME_PACKAGE_STALE,
    /* A cached check has not produced a trustworthy package result. */
    NK_RUNTIME_PACKAGE_UNKNOWN = 4
} NkRuntimePackageStatus;

typedef struct {
    char package_root[NK_MAX_PATH];
    char executable_path[NK_MAX_PATH];
    char image_path[NK_MAX_PATH];
    bool validation_cache_hit;
} NkRuntimePackageInfo;

/* Parse and validate a manifest from a memory buffer.
 * If allow_override is false, collisions with public catalog entries are rejected.
 * If allow_override is true, collisions generate a noisy warning and are accepted.
 * The optional out_entry borrows parser overlay storage and is valid only until
 * the next manifest parse or overlay clear; callers that retain parsed titles
 * across catalog operations must copy them into an owned catalog snapshot while
 * holding nk_title_catalog_lock().
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

/* Load every *.json title manifest in dir as an overlay, in file-name order,
 * up to the overlay capacity. A missing dir loads nothing and is not an error.
 * Returns the number loaded; each refused or skipped file is described in
 * report (one line per file, truncated to report_len). */
int nk_title_manifest_load_overlay_dir(const char *dir, char *report, size_t report_len);

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
 * Validation registers nothing: it takes no parser overlay slot and publishes
 * no catalog epoch change (#670).
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
 * manifest is fully validated and, when requested, returned as an owned title
 * snapshot that survives subsequent overlay reloads. Release the snapshot with
 * nk_title_catalog_snapshot_release(). Like the writer, the reader only
 * validates: registered overlays, parser overlay storage and the catalog epoch
 * are unchanged, so package validation of an experimental title cannot evict a
 * registered overlay or invalidate other cached package statuses (#670). */
bool nk_title_manifest_read_experimental_profile(
    const char *user_data_root,
    const char *disc_id,
    const char *title_id,
    const char *selected_executable,
    NkTitleEntrySnapshot *out_title,
    char out_executable_sha256[65],
    char *error_buf,
    size_t error_buf_len
);

/* Validate an experimental profile created before an executable was selected.
 * This accepts only the identity-bound profile form with an empty executable
 * and null executable hashes; it never treats a bound profile as a match. */
bool nk_title_manifest_read_unbound_experimental_profile(
    const char *user_data_root,
    const char *disc_id,
    const char *title_id,
    NkTitleEntrySnapshot *out_title,
    char *error_buf,
    size_t error_buf_len
);

/* The executable leaf ("EBOOT.BIN" or "BOOT.BIN") that an experimental
 * profile binds, returned only when the whole profile validates for that leaf.
 * A disc imported while its EBOOT.BIN was encrypted has a library entry with
 * no selected executable; a later build with a user-supplied decrypted
 * EBOOT.elf rewrites the profile to bind EBOOT.BIN, and the library entry
 * adopts it from here (#1276). */
bool nk_title_manifest_experimental_profile_executable(
    const char *user_data_root,
    const char *disc_id,
    const char *title_id,
    char *out_leaf,
    size_t out_leaf_size
);

/* Validate the v2 generated package at <user_data_root>/packages/<DISC_ID>.
 * Experimental entries are additionally bound to their private profile's
 * selected executable hash. `current_disc_version` comes from the selected
 * library ISO inspection; `player_abi_version` comes from recomp.h. */
NkRuntimePackageStatus nk_title_manifest_validate_aot_package(
    const char *user_data_root,
    const char *disc_id,
    const char *title_id,
    bool is_experimental,
    const char *selected_executable,
    const char *current_disc_version,
    const char *source_iso_path,
    uint32_t player_abi_version,
    NkRuntimePackageInfo *out_info,
    char *reason,
    size_t reason_size
);

/* Validation hashes the package's files, which takes seconds for a large
 * package. A host that is shutting down can stop it early: once `cancelled`
 * returns true, the file being hashed is abandoned between 32 KiB reads and
 * the validation fails with the reason "Package validation was cancelled.".
 * The check must be safe to call from any thread. Install it before any
 * validation starts; NULL (the default) never cancels. */
typedef bool (*NkTitleManifestCancelFn)(void *user);
void nk_title_manifest_set_cancel_check(NkTitleManifestCancelFn cancelled, void *user);

/* Build a metadata identity for package status caches. It covers package.json,
 * build-report.json, the declared executable, every artifact in the completion
 * manifest, and the experimental profile identity when applicable. Missing
 * files are part of the identity too. */
bool nk_title_manifest_aot_package_cache_identity(
    const char *user_data_root,
    const char *disc_id,
    const char *title_id,
    bool is_experimental,
    const char *selected_executable,
    const char *current_disc_version,
    const char *source_iso_path,
    uint32_t player_abi_version,
    char out_identity[65]
);

/* The Nakagawa a package was built by (owner decision, 2026-10-29: the player
 * suggests a rebuild for a package an older Nakagawa built, and still launches
 * it). Every package build since v0.0.1 records these four values in its cache
 * key, and they are the parts of that key that come from Nakagawa itself rather
 * than from the disc, the compiler or the build options:
 *
 *   runtime_source_digest  cache.key.native.components: the runtime, core and
 *                          player C sources, the Makefile and assets/vfpu
 *                          (tools/nk_core/package_cache.py source_tree_digest)
 *   analyzer_sha256        cache.key.aot.components: tools/analyze.py
 *   codegen_sha256         cache.key.aot.components: tools/codegen.py
 *   planner_sha256         cache.codegen_options: tools/title_codegen_plan.py
 *
 * A package whose values differ from the ones the player's own source tree
 * gives was built by a different Nakagawa: rebuilding it compiles this
 * version's runtime and, when the translator changed, regenerates its C. No
 * release bumps anything by hand; any change to those files moves the identity.
 * The package format, the ABI epochs and the validator are untouched, so such a
 * package still validates and launches. Each value is 64 lowercase hex digits. */
typedef struct {
    char runtime_source_digest[65];
    char analyzer_sha256[65];
    char codegen_sha256[65];
    char planner_sha256[65];
} NkPackageBuildIdentity;

/* The identity a build from the source tree at `source_root` (the folder that
 * holds tools/nk_cli.py's tools/ folder: a checkout, or a release's source/)
 * records. Hashes about 20 MB, so it belongs on a worker thread; the cancel
 * check stops it like a package validation. False when a tool file is missing
 * or a file cannot be read. */
bool nk_title_manifest_source_build_identity(const char *source_root,
                                             NkPackageBuildIdentity *out);

/* The identity recorded by the package at <user_data_root>/packages/<DISC_ID>.
 * Reads package.json only (no validation, no hashing), so a library-wide scan
 * stays cheap. False when there is no package or it does not record all four
 * values: such a package is not judged here, and the validator says what is
 * wrong with it. */
bool nk_title_manifest_aot_package_build_identity(const char *user_data_root,
                                                  const char *disc_id,
                                                  NkPackageBuildIdentity *out);

bool nk_package_build_identity_equal(const NkPackageBuildIdentity *a,
                                     const NkPackageBuildIdentity *b);

/* White-box seams for the package-validation cache epoch contract (#670).
 * Declared and defined only when the test build defines
 * NK_TITLE_MANIFEST_TEST_SEAMS, the same shape as SR_CD_TEST_HOOKS and
 * SR_PROFILER_SELFTEST; production builds do not carry them. A native
 * regression installs a checkpoint to land a catalog clear/reload
 * deterministically at the cache publication point, and reads the published
 * entry back so it can assert that the epoch stamped on the entry is the epoch
 * its identity digest was computed under. */
#if defined(NK_TITLE_MANIFEST_TEST_SEAMS)
typedef void (*NkPackageValidationCheckpointFn)(void *ctx);
void nk_title_manifest_test_set_validation_checkpoint(
    NkPackageValidationCheckpointFn checkpoint, void *ctx);
bool nk_title_manifest_test_package_cache_entry(uint64_t *out_catalog_epoch,
                                                char out_status_identity[65]);
bool nk_title_manifest_test_source_iso_path_valid(const char *path);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NK_TITLE_MANIFEST_H */
