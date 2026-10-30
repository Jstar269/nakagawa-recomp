/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_TYPES_H
#define NK_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_MAX_PATH 512
#define NK_MAX_TITLE_LEN 128
#define NK_MAX_DISC_ID_LEN 32
/* The most titles one library holds: the same record limit as the Python
   tools (MAX_LIBRARY_GAMES in tools/nk_core/library.py), so a library the
   tools write always loads in the player. A library of this size is too large
   for a stack frame; keep NkLibrary and PlayerApp in static or heap storage. */
#define NK_MAX_GAMES 4096
#define NK_MAX_EXECUTABLE_PATH (255u + 1u) /* Python boot-name limit plus NUL */
#define NK_MAX_SELECTED_EXECUTABLE_PATH 16
/* An enhancement package id (at most 64 characters, see
   nk_title_manifest_enhancement_id_valid) plus NUL. */
#define NK_MAX_ENHANCEMENT_PACKAGE_ID (64u + 1u)

typedef enum {
    NK_OK = 0,
    NK_ERROR_GENERIC = -1,
    NK_ERROR_FILE_NOT_FOUND = -2,
    NK_ERROR_INVALID_ISO = -3,
    NK_ERROR_UNSUPPORTED_TITLE = -4,
    NK_ERROR_IO = -5,
    NK_ERROR_OUT_OF_MEMORY = -6,
    NK_ERROR_PROCESS_SPAWN = -7,
    NK_ERROR_PERMISSION = -8,
    NK_ERROR_CANCELLED = -9,
    NK_ERROR_ALREADY_EXISTS = -10,
    NK_ERROR_INVALID_XB = -11,
    NK_ERROR_INVALID_EXECUTABLE = -12,
    /* A host path the work needs is longer than the OS file APIs accept
       (nk_platform_path_exceeds_limit); the input itself is fine. */
    NK_ERROR_PATH_TOO_LONG = -13
} NkResult;

typedef enum {
    NK_STATUS_UNIDENTIFIED = 0,
    NK_STATUS_IDENTIFIED,
    NK_STATUS_SUPPORTED_PREPARATION,
    NK_STATUS_PREPARED,
    NK_STATUS_BOOTS,
    NK_STATUS_PLAYABLE,
    NK_STATUS_VERIFIED
} NkGameSupportStatus;

typedef struct {
    char disc_id[NK_MAX_DISC_ID_LEN];
    char title_name[NK_MAX_TITLE_LEN];
    char disc_version[16];
    char iso_path[NK_MAX_PATH];
    /* A compressed disc image (.cso, .zso, .chd) the user added. iso_path is
       then the plain .iso it was expanded into (nk_disc_expand.h), which every
       later step reads; this path is only shown to the user. Empty for a disc
       added as an .iso. */
    char source_image_path[NK_MAX_PATH];
    char prepared_root[NK_MAX_PATH];
    char title_id[64];
    uint64_t iso_size_bytes;
    NkGameSupportStatus status;
    bool is_experimental;
    uint32_t executable_eboot_kind;
    uint32_t executable_boot_kind;
    uint32_t executable_selection;
    bool executable_boot_fallback;
    /* Original executable filename as loaded by the PSP, separate from the
       normalized executable selected for analysis/package identity. */
    char boot_executable[NK_MAX_EXECUTABLE_PATH];
    char selected_executable[NK_MAX_SELECTED_EXECUTABLE_PATH];
    bool is_prepared;
    /* Disc payload staging is deliberately separate from runtime readiness.
       A staged title has its EBOOT/XB data in prepared_root, but may still
       need a locally built recompiled runtime or module preparation before
       PLAY NOW can succeed. */
    bool assets_staged;
    uint32_t extracted_asset_count;
    uint32_t extracted_audio_count;
    uint32_t extracted_visual_count;
    uint32_t extracted_layout_count;
    char last_played[32];
    /* A bundled demo or UI fixture, shown as SAMPLE. It exists only in memory:
       nk_library_save never writes it, so it cannot reach the user's library.json. */
    bool is_sample;
    /* A catalog-discovered showcase package owns its compiled inputs and does
       not bind package validity to the companion synthetic ISO identity. */
    bool is_showcase_package;
    /* The package this entry validates and launches: empty for the authentic
       package, packages/<disc_id>; an enhancement package id for the enhanced
       build, packages/<disc_id>-enhanced-<id> (docs/ENHANCEMENT_CONTRACT.md
       section 9). The player sets it from its per-title setting; the library
       file never stores it. */
    char enhancement_package_id[NK_MAX_ENHANCEMENT_PACKAGE_ID];
} NkGameEntry;

#ifdef __cplusplus
}
#endif

#endif /* NK_TYPES_H */
