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
 * to reuse an existing tree, extracts EBOOT.BIN and the title's configured
 * loose-content roots, and invokes the native XB decoder for every copied
 * .xb/.xbN archive. If the user's
 * standard PPSSPP dump locations contain the three known already-decrypted
 * support PRXs, they are copied into `EXTRACTED/decrypted/` as an optional
 * local bridge; no decryption is performed. */
NkResult player_stage_game(const char *iso_path, const char *staging_root,
                           const char *const *loose_content_roots,
                           size_t loose_content_root_count,
                           const PlayerStageCallbacks *callbacks,
                           char *error_message, size_t error_message_size);

/* Summary-bearing variant used by the native player. The legacy entry point
 * above remains a wrapper for callers that only need the result. */
NkResult player_stage_game_with_summary(const char *iso_path,
                                        const char *staging_root,
                                        const char *const *loose_content_roots,
                                        size_t loose_content_root_count,
                                        const PlayerStageCallbacks *callbacks,
                                        PlayerStageSummary *summary,
                                        char *error_message,
                                        size_t error_message_size);

/* Remove a staging tree created by player_stage_game after a failed or
 * cancelled attempt. The function only accepts a basename beginning with
 * `.staging_`, so callers cannot accidentally hand it a general data root. */
bool player_stage_discard(const char *staging_root);

/* One title's staging request. Every route that installs a disc's files (the
 * setup wizard's worker, `--stage-only`, `--launch-now`, and `nk_cli prepare`
 * through the player) fills this from the title's manifest and calls
 * player_stage_title; none of them stages or promotes a tree on its own. */
typedef struct {
    const char *iso_path;
    /* The per-user data root. The title's files live in <root>/games/<disc>. */
    const char *user_data_root;
    const char *disc_id;
    /* PARAM.SFO disc version; part of the identity a reused tree must match. */
    const char *disc_version;
    const char *const *loose_content_roots;
    size_t loose_content_root_count;
    /* The manifest's filesystem.data_root ("" or NULL when the title has none).
     * When it lies under a loose-content root, the staged tree must contain it. */
    const char *data_root;
} PlayerStageTitleRequest;

/* True when the title's data root lies under one of its loose-content roots,
 * so its data can only come from the disc and the title needs staging before
 * it can start. A title whose data root is resolved elsewhere (or has none)
 * needs only its executable. */
bool player_stage_title_takes_data_from_disc(const PlayerStageTitleRequest *request);

typedef enum {
    /* The disc's files were extracted now and moved into place. */
    PLAYER_STAGE_TITLE_STAGED = 0,
    /* A complete tree staged earlier from the same disc was already in place. */
    PLAYER_STAGE_TITLE_REUSED = 1
} PlayerStageTitleOutcome;

/* Stage one title into the per-user data root as a single transaction:
 *
 *   1. take the title's staging lock (a second staging of the same title, in
 *      this or another process, is refused rather than allowed to share files);
 *   2. clear what an interrupted earlier attempt left behind (its unpromoted
 *      `.staging_<disc>` tree or a replaced tree it did not finish removing);
 *   3. reuse an existing `games/<disc>` tree when its staging record matches
 *      this request and every required folder is present;
 *   4. otherwise extract into `.staging_<disc>`, validate it, write its staging
 *      record, and promote it with one directory rename. An older or
 *      incomplete tree is replaced only after the new one is complete, and is
 *      restored if the replacing rename fails.
 *
 * An interruption at any point leaves either the previous tree or the new
 * complete tree at `games/<disc>`, never a partial one; the next call finishes
 * the cleanup and stages again. On success `prepared_root` receives the
 * promoted directory and `summary` the extracted counts (restored from the
 * staging record when the tree was reused). On failure `message` holds a
 * bracketed boundary code, a plain sentence, and what to do next. */
NkResult player_stage_title(const PlayerStageTitleRequest *request,
                            const PlayerStageCallbacks *callbacks,
                            PlayerStageSummary *summary,
                            PlayerStageTitleOutcome *outcome,
                            char *prepared_root, size_t prepared_root_size,
                            char *message, size_t message_size);

#ifdef __cplusplus
}
#endif

#endif /* NAKAGAWA_SETUP_STAGING_H */
