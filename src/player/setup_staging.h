/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_SETUP_STAGING_H
#define NAKAGAWA_SETUP_STAGING_H

#include "nk_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*PlayerStageCancelCallback)(void *userdata);
typedef void (*PlayerStageProgressCallback)(const char *current_path,
                                            int percent,
                                            size_t files_extracted,
                                            size_t total_files,
                                            void *userdata);

typedef struct {
    PlayerStageCancelCallback is_cancelled;
    PlayerStageProgressCallback on_progress;
    void *userdata;
} PlayerStageCallbacks;

/* Counts produced by the native XB walk. These are staging facts, not a
 * claim that a runtime or a private title acceptance route is ready. */
typedef struct {
    uint32_t extracted_asset_count;
    uint32_t extracted_audio_count;
    uint32_t extracted_visual_count;
    uint32_t extracted_layout_count;
} PlayerStageSummary;

/* Build one isolated game staging tree from the selected ISO. The caller
 * supplies the exact `.staging_<disc_id>` destination; this function refuses
 * to reuse an existing tree, extracts the selected ISO payload, and invokes
 * the native XB decoder for every copied .xb/.xbN archive. If the user's
 * standard PPSSPP dump locations contain the three known already-decrypted
 * support PRXs, they are copied into `EXTRACTED/decrypted/` as an optional
 * local bridge; no decryption is performed. */
NkResult player_stage_game(const char *iso_path, const char *staging_root,
                           const PlayerStageCallbacks *callbacks,
                           char *error_message, size_t error_message_size);

/* Summary-bearing variant used by the native player. The legacy entry point
 * above remains a wrapper for callers that only need the result. */
NkResult player_stage_game_with_summary(const char *iso_path,
                                        const char *staging_root,
                                        const PlayerStageCallbacks *callbacks,
                                        PlayerStageSummary *summary,
                                        char *error_message,
                                        size_t error_message_size);

/* Remove a staging tree created by player_stage_game after a failed or
 * cancelled attempt. The function only accepts a basename beginning with
 * `.staging_`, so callers cannot accidentally hand it a general data root. */
bool player_stage_discard(const char *staging_root);

#ifdef __cplusplus
}
#endif

#endif /* NAKAGAWA_SETUP_STAGING_H */
