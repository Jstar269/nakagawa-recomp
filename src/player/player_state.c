/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* fileno/fsync are POSIX: without this, -std=c99 on glibc leaves them undeclared. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "player_state.h"
#include "nk_font.h"
#include "nk_json.h"
#include "nk_platform.h"
#include "nk_psp_container.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#ifdef NK_PLAYER_UI_REGRESSION_TEST
static uint64_t s_player_ui_validation_calls;

uint64_t player_app_ui_test_validation_calls(void) {
    return s_player_ui_validation_calls;
}
#endif

static const char *player_runtime_root(const PlayerApp *app) {
    return (app && app->runtime_root[0]) ? app->runtime_root : NULL;
}

static bool player_app_data_root(const PlayerApp *app, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (app && app->runtime_root[0]) {
        int n = snprintf(out, out_size, "%s", app->runtime_root);
        return n > 0 && (size_t)n < out_size;
    }
    return nk_platform_get_app_data_dir(out, out_size);
}

bool player_game_is_showcase(const GameRecord *game) {
    return game && strncmp(game->title_id, "showcase-", 9) == 0;
}

static const char *player_package_root(const PlayerApp *app, const GameRecord *game) {
    if (app && game && player_game_is_showcase(game) && app->showcase_root[0]) {
        return app->showcase_root;
    }
    return player_runtime_root(app);
}

bool player_app_title_profile_refusal_for_disc(
    const PlayerApp *app,
    const char *disc_id,
    char *reason,
    size_t reason_size
) {
    if (reason && reason_size) reason[0] = '\0';
    if (!app || !disc_id || !disc_id[0]) return false;
    const char *line = app->title_manifest_report;
    const char marker[] = " (disc ";
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t)(end - line) : strlen(line);
        const char *id_start = NULL;
        for (size_t i = 0; i + sizeof(marker) - 1u < line_len; i++) {
            if (memcmp(line + i, marker, sizeof(marker) - 1u) == 0) {
                id_start = line + i + sizeof(marker) - 1u;
                break;
            }
        }
        if (id_start) {
            const char *id_end = memchr(id_start, ')',
                                        (size_t)(line + line_len - id_start));
            if (id_end && (size_t)(id_end - id_start) == strlen(disc_id) &&
                memcmp(id_start, disc_id, strlen(disc_id)) == 0 &&
                (size_t)(id_end - line + 2u) < line_len && id_end[1] == ':' &&
                id_end[2] == ' ') {
                if (reason && reason_size) {
                    size_t detail_len = line_len - (size_t)(id_end + 3 - line);
                    if (detail_len >= reason_size) detail_len = reason_size - 1u;
                    memcpy(reason, id_end + 3, detail_len);
                    reason[detail_len] = '\0';
                }
                return true;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

NkLaunchDataRootStatus player_app_game_data_root_status(
    const PlayerApp *app,
    const GameRecord *game,
    char *resolved_path,
    size_t resolved_path_size,
    char *reason,
    size_t reason_size
) {
    const char *root = player_package_root(app, game);
    return nk_launch_game_data_root_status(root, game, resolved_path,
                                           resolved_path_size, reason,
                                           reason_size);
}

static bool player_join_path(char *out, size_t out_size, const char *root,
                             const char *relative) {
    char sep = nk_platform_path_separator();
    int written;
    if (!out || !out_size || !root || !root[0] || !relative || !relative[0]) return false;
    written = snprintf(out, out_size, "%s%c%s", root, sep, relative);
    return written > 0 && (size_t)written < out_size;
}

void player_app_init(PlayerApp *app) {
    if (!app) return;
    memset(app, 0, sizeof(*app));
    app->active_view = VIEW_LIBRARY;
    app->selected_game_index = -1;
    app->window_width = 1280;
    app->window_height = 720;
    app->dpi_scale = 1.0f;
    app->should_quit = false;

    /* Initialize settings from disk or defaults */
    player_app_settings_init_default(&app->settings);
    player_app_load_settings(app, NULL);

    /* Default preparation state */
    app->prep_state.stage = STAGE_IDLE;
    app->prep_state.percentage = 0.0f;
    app->prep_state.cancellable = true;

    /* Initialize native library */
    nk_library_init(&app->library);
    NkResult lib_res = nk_library_load(&app->library, NULL);
    if (lib_res == NK_OK && app->library.count > 0) {
        player_app_sync_library(app);
    }

    /* Initialize controller input settings (#357) */
    input_settings_init(&app->input_settings);
    input_settings_load(&app->input_settings, NULL);
}

void player_app_set_runtime_root(PlayerApp *app, const char *root) {
    if (!app) return;
    if (!root) {
        app->runtime_root[0] = '\0';
        player_app_runtime_package_cache_invalidate(app);
        return;
    }
    snprintf(app->runtime_root, sizeof(app->runtime_root), "%s", root);
    player_app_runtime_package_cache_invalidate(app);
}

void player_app_runtime_package_cache_invalidate(PlayerApp *app) {
    if (!app) return;
    memset(app->runtime_package_cache, 0,
           sizeof(app->runtime_package_cache));
    app->runtime_package_cache_generation++;
}

static bool player_runtime_worker_failure_matches(
    const PlayerRuntimePackageWorkerFailure *failure,
    const GameRecord *game) {
    return failure && failure->valid && game &&
        strcmp(failure->disc_id, game->disc_id) == 0 &&
        strcmp(failure->title_id, game->title_id) == 0 &&
        strcmp(failure->selected_executable, game->selected_executable) == 0;
}

static int player_runtime_worker_failure_index(
    const PlayerApp *app, const GameRecord *game) {
    if (!app || !game) return -1;
    for (int i = 0; i < MAX_LIBRARY_GAMES; i++) {
        if (player_runtime_worker_failure_matches(
                &app->runtime_package_worker_failures[i], game)) {
            return i;
        }
    }
    return -1;
}

static void player_app_prune_runtime_worker_failures(PlayerApp *app) {
    if (!app) return;
    for (int i = 0; i < MAX_LIBRARY_GAMES; i++) {
        PlayerRuntimePackageWorkerFailure *failure =
            &app->runtime_package_worker_failures[i];
        if (!failure->valid) continue;
        bool still_present = false;
        for (int j = 0; j < app->game_count; j++) {
            if (player_runtime_worker_failure_matches(failure,
                                                       &app->games[j])) {
                still_present = true;
                break;
            }
        }
        if (!still_present) memset(failure, 0, sizeof(*failure));
    }
}

bool player_app_runtime_package_worker_start_failed(
    const PlayerApp *app, const GameRecord *game) {
    return player_runtime_worker_failure_index(app, game) >= 0;
}

bool player_app_runtime_package_worker_start_failed_retry_due(
    const PlayerApp *app, const GameRecord *game, uint64_t now_ms) {
    int index = player_runtime_worker_failure_index(app, game);
    return index >= 0 &&
        now_ms >= app->runtime_package_worker_failures[index].retry_after_ms;
}

void player_app_runtime_package_worker_start_failed_record(
    PlayerApp *app, const GameRecord *game, uint64_t now_ms) {
    if (!app || !game) return;
    int slot = player_runtime_worker_failure_index(app, game);
    if (slot < 0) {
        for (int i = 0; i < MAX_LIBRARY_GAMES; i++) {
            if (!app->runtime_package_worker_failures[i].valid) {
                slot = i;
                break;
            }
        }
    }
    /* The side table is bounded to the maximum library size. A full table can
       only mean the caller has not synchronized after removing a title; do
       not evict another unresolved title to make room. */
    if (slot < 0) return;
    PlayerRuntimePackageWorkerFailure *failure =
        &app->runtime_package_worker_failures[slot];
    if (!failure->valid) {
        memset(failure, 0, sizeof(*failure));
        failure->valid = true;
        snprintf(failure->disc_id, sizeof(failure->disc_id), "%s", game->disc_id);
        snprintf(failure->title_id, sizeof(failure->title_id), "%s", game->title_id);
        snprintf(failure->selected_executable, sizeof(failure->selected_executable),
                 "%s", game->selected_executable);
    }
    if (failure->failure_count < 5) failure->failure_count++;
    uint64_t delay_ms = 5000;
    for (uint32_t i = 1; i < failure->failure_count && delay_ms < 60000; i++) {
        delay_ms *= 2;
    }
    if (delay_ms > 60000) delay_ms = 60000;
    failure->retry_after_ms = now_ms > UINT64_MAX - delay_ms
        ? UINT64_MAX : now_ms + delay_ms;
}

void player_app_runtime_package_worker_start_failed_clear(
    PlayerApp *app, const GameRecord *game) {
    if (!app || !game) return;
    int index = player_runtime_worker_failure_index(app, game);
    if (index >= 0) {
        memset(&app->runtime_package_worker_failures[index], 0,
               sizeof(app->runtime_package_worker_failures[index]));
    }
}

static void player_runtime_cache_set_game_key(
    PlayerRuntimePackageCacheEntry *entry, const GameRecord *game) {
    if (!entry || !game) return;
    snprintf(entry->disc_id, sizeof(entry->disc_id), "%s", game->disc_id);
    snprintf(entry->title_id, sizeof(entry->title_id), "%s", game->title_id);
    snprintf(entry->selected_executable, sizeof(entry->selected_executable),
             "%s", game->selected_executable);
}

void player_app_runtime_package_cache_mark_pending(PlayerApp *app,
                                                    int game_index) {
    if (!app || game_index < 0 || game_index >= app->game_count) return;
    PlayerRuntimePackageCacheEntry *entry =
        &app->runtime_package_cache[game_index];
    const GameRecord *game = &app->games[game_index];
    bool same_game = strcmp(entry->disc_id, game->disc_id) == 0 &&
                     strcmp(entry->title_id, game->title_id) == 0 &&
                     strcmp(entry->selected_executable,
                            game->selected_executable) == 0;
    if (!same_game) {
        memset(entry, 0, sizeof(*entry));
        player_runtime_cache_set_game_key(entry, game);
    }
    entry->validation_pending = true;
    entry->validation_failed = false;
}

void player_app_runtime_package_cache_mark_explicit_retry(PlayerApp *app,
                                                          int game_index) {
    if (!app || game_index < 0 || game_index >= app->game_count) return;
    PlayerRuntimePackageCacheEntry *entry =
        &app->runtime_package_cache[game_index];
    const GameRecord *game = &app->games[game_index];
    bool same_game = strcmp(entry->disc_id, game->disc_id) == 0 &&
                     strcmp(entry->title_id, game->title_id) == 0 &&
                     strcmp(entry->selected_executable,
                            game->selected_executable) == 0;
    if (!same_game) {
        memset(entry, 0, sizeof(*entry));
        player_runtime_cache_set_game_key(entry, game);
    }
    entry->explicit_retry_pending = true;
}

void player_app_runtime_package_cache_mark_failed(PlayerApp *app,
                                                   int game_index,
                                                   uint64_t now_ms) {
    if (!app || game_index < 0 || game_index >= app->game_count) return;
    PlayerRuntimePackageCacheEntry *entry =
        &app->runtime_package_cache[game_index];
    entry->status_valid = false;
    entry->identity_valid = false;
    entry->validation_pending = false;
    entry->validation_failed = true;
    entry->runtime_available = false;
    entry->package_identity[0] = '\0';
    entry->last_checked_ms = 0;
    player_app_runtime_package_worker_start_failed_record(
        app, &app->games[game_index], now_ms);
}

void player_app_runtime_package_cache_store(
    PlayerApp *app, int game_index, const GameRecord *game,
    bool identity_valid, const char *package_identity,
    NkRuntimePackageStatus status, bool runtime_available,
    uint64_t checked_ms) {
    if (!app || !game || game_index < 0 || game_index >= app->game_count) return;
    PlayerRuntimePackageCacheEntry *entry =
        &app->runtime_package_cache[game_index];
    /* A stored reason survives a store for the same title; a warm cache hit
     * stores no new reason, and the earlier one still describes the package. */
    char prior_reason[sizeof(entry->reason)] = "";
    if (strcmp(entry->disc_id, game->disc_id) == 0 &&
        strcmp(entry->title_id, game->title_id) == 0 &&
        strcmp(entry->selected_executable, game->selected_executable) == 0) {
        snprintf(prior_reason, sizeof(prior_reason), "%s", entry->reason);
    }
    memset(entry, 0, sizeof(*entry));
    player_runtime_cache_set_game_key(entry, game);
    snprintf(entry->reason, sizeof(entry->reason), "%s", prior_reason);
    entry->status_valid = true;
    entry->identity_valid = identity_valid;
    entry->runtime_available = runtime_available;
    entry->status = status;
    entry->last_checked_ms = checked_ms;
    if (package_identity) {
        snprintf(entry->package_identity, sizeof(entry->package_identity), "%s",
                 package_identity);
    }
}

static const PlayerRuntimePackageCacheEntry *player_runtime_cache_for_game(
    const PlayerApp *app, const GameRecord *game);

void player_app_runtime_package_cache_set_reason(PlayerApp *app, int game_index,
                                                 const char *reason) {
    if (!app || !reason || !reason[0] || game_index < 0 ||
        game_index >= app->game_count) return;
    snprintf(app->runtime_package_cache[game_index].reason,
             sizeof(app->runtime_package_cache[game_index].reason), "%s", reason);
}

bool player_app_runtime_package_status_known(const PlayerApp *app,
                                             const GameRecord *game) {
    const PlayerRuntimePackageCacheEntry *entry =
        player_runtime_cache_for_game(app, game);
    return entry && entry->status_valid && !entry->validation_pending;
}

const char *player_app_runtime_package_reason(const PlayerApp *app,
                                              const GameRecord *game) {
    const PlayerRuntimePackageCacheEntry *entry =
        player_runtime_cache_for_game(app, game);
    return entry && entry->reason[0] ? entry->reason : NULL;
}

size_t player_library_status_text(bool has_runtime, bool checking, bool check_failed,
                                  bool assets_staged, uint32_t staged_asset_count,
                                  const char *reason, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    /* The validator's first sentence is the reason a card can afford: the rest
     * is detail for the logs. */
    char first[256] = "";
    if (reason) {
        size_t n = 0;
        while (reason[n] && reason[n] != '.' && n + 1 < sizeof(first)) {
            first[n] = reason[n];
            n++;
        }
        first[n] = '\0';
    }
    int written = 0;
    if (has_runtime) {
        written = snprintf(out, out_size, "Status: Prepared");
    } else if (checking) {
        written = snprintf(out, out_size, "Status: Checking package...");
    } else if (check_failed) {
        written = snprintf(out, out_size, "Check failed: %s",
                           first[0] ? first : "the package could not be checked");
    } else if (assets_staged) {
        written = first[0]
            ? snprintf(out, out_size, "Assets staged: %u. %s",
                       (unsigned)staged_asset_count, first)
            : snprintf(out, out_size, "Assets staged: %u",
                       (unsigned)staged_asset_count);
    } else if (first[0]) {
        written = snprintf(out, out_size, "Not prepared: %s", first);
    } else {
        written = snprintf(out, out_size, "Status: Not prepared");
    }
    if (written < 0) return 0;
    return (size_t)written < out_size ? (size_t)written : out_size - 1;
}

NkRuntimePackageStatus player_app_validate_runtime_package(
    const PlayerApp *app,
    const GameRecord *game,
    NkRuntimePackageInfo *out_info,
    char *reason,
    size_t reason_size
) {
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    s_player_ui_validation_calls++;
#endif
    char default_root[NK_MAX_PATH];
    const char *root = player_package_root(app, game);
    if (!root) {
        if (!nk_platform_get_app_data_dir(default_root, sizeof(default_root))) {
            if (reason && reason_size) snprintf(reason, reason_size,
                "DATA_DIR_UNAVAILABLE: the Windows Local AppData known folder, LOCALAPPDATA, or APPDATA could not be resolved; package discovery cannot run.");
            return NK_RUNTIME_PACKAGE_MISSING;
        }
        root = default_root;
    }
    return nk_launch_validate_runtime_package(root, game, out_info, reason,
                                              reason_size);
}

static const PlayerRuntimePackageCacheEntry *player_runtime_cache_for_game(
    const PlayerApp *app, const GameRecord *game) {
    if (!app || !game) return NULL;
    for (int i = 0; i < app->game_count; i++) {
        const PlayerRuntimePackageCacheEntry *entry =
            &app->runtime_package_cache[i];
        if (strcmp(app->games[i].disc_id, game->disc_id) == 0 &&
            strcmp(app->games[i].title_id, game->title_id) == 0 &&
            strcmp(entry->disc_id, game->disc_id) == 0 &&
            strcmp(entry->title_id, game->title_id) == 0 &&
            strcmp(entry->selected_executable, game->selected_executable) == 0) {
            return entry;
        }
    }
    return NULL;
}

NkRuntimePackageStatus player_app_cached_runtime_package_status(
    const PlayerApp *app, const GameRecord *game) {
    /* This record survives cache invalidation and distinguishes a worker
       infrastructure failure from a validator result of MISSING. */
    if (player_app_runtime_package_worker_start_failed(app, game)) {
        return NK_RUNTIME_PACKAGE_UNKNOWN;
    }
    const PlayerRuntimePackageCacheEntry *entry =
        player_runtime_cache_for_game(app, game);
    if (entry && entry->status_valid) return entry->status;
    /* A failed worker start produced no package result; do not turn that
       infrastructure failure into a validated MISSING status. */
    if (entry && entry->validation_failed) return NK_RUNTIME_PACKAGE_UNKNOWN;
    return game && game->title_id[0] == '\0'
        ? NK_RUNTIME_PACKAGE_INCOMPATIBLE : NK_RUNTIME_PACKAGE_MISSING;
}

bool player_app_runtime_package_check_pending(const PlayerApp *app,
                                               const GameRecord *game) {
    const PlayerRuntimePackageCacheEntry *entry =
        player_runtime_cache_for_game(app, game);
    return entry && entry->validation_pending;
}

bool player_app_runtime_package_check_failed(const PlayerApp *app,
                                              const GameRecord *game) {
    /* A retry has claimed this title again. Keep the title-keyed worker
       failure record until a result arrives, but let the renderer show the
       in-flight check instead of a stale failure badge. A second worker-start
       failure clears validation_pending before returning here and restores
       the actionable failure state. */
    const PlayerRuntimePackageCacheEntry *entry =
        player_runtime_cache_for_game(app, game);
    if (entry && entry->validation_pending) return false;
    if (player_app_runtime_package_worker_start_failed(app, game)) return true;
    return entry && entry->validation_failed;
}

bool player_app_cached_game_has_runtime(const PlayerApp *app,
                                        const GameRecord *game) {
    const PlayerRuntimePackageCacheEntry *entry =
        player_runtime_cache_for_game(app, game);
    return entry && entry->status_valid && entry->runtime_available;
}

bool player_app_game_has_runtime(const PlayerApp *app, const GameRecord *game) {
    if (!game) return false;
    NkLaunchDataRootStatus data_status = player_app_game_data_root_status(
        app, game, NULL, 0, NULL, 0);
    if (data_status != NK_LAUNCH_DATA_ROOT_READY &&
        data_status != NK_LAUNCH_DATA_ROOT_NOT_REQUIRED) return false;
    NkRuntimePackageStatus status = player_app_validate_runtime_package(
        app, game, NULL, NULL, 0);
    if (status == NK_RUNTIME_PACKAGE_OK) return true;
    return status == NK_RUNTIME_PACKAGE_MISSING && !game->is_experimental &&
           nk_launch_runtime_available(player_package_root(app, game),
                                       game->title_id);
}

void player_app_sync_library(PlayerApp *app) {
    if (!app) return;
    player_app_runtime_package_cache_invalidate(app);
    app->game_count = 0;
    for (int i = 0; i < app->library.count && i < MAX_LIBRARY_GAMES; i++) {
        app->games[i] = app->library.entries[i];
        app->game_count++;
    }
    for (int i = 0; i < app->showcase_count && app->game_count < MAX_LIBRARY_GAMES; i++) {
        bool already_present = false;
        for (int j = 0; j < app->game_count; j++) {
            if (strcmp(app->games[j].disc_id, app->showcase_games[i].disc_id) == 0) {
                already_present = true;
                break;
            }
        }
        if (!already_present) app->games[app->game_count++] = app->showcase_games[i];
    }
    if (app->game_count <= 0) {
        app->selected_game_index = -1;
        app->library_scroll_index = 0;
    } else {
        if (app->selected_game_index < 0) app->selected_game_index = 0;
        if (app->selected_game_index >= app->game_count) {
            app->selected_game_index = app->game_count - 1;
        }
    }
    /* Library synchronization intentionally invalidates the result cache, but
       a title-keyed worker-start failure remains actionable while that title
       is still present. Removal drops it; re-adding then starts clean. */
    player_app_prune_runtime_worker_failures(app);
}

bool player_merge_readded_game(const GameRecord *existing, GameRecord *incoming) {
    if (!existing || !incoming) return false;
    if (strcmp(existing->disc_id, incoming->disc_id) != 0 ||
        strcmp(existing->disc_version, incoming->disc_version) != 0 ||
        existing->iso_size_bytes != incoming->iso_size_bytes) return false;
    bool merged = false;
    if (existing->assets_staged && !incoming->assets_staged && existing->prepared_root[0]) {
        incoming->assets_staged = true;
        incoming->is_prepared = existing->is_prepared;
        snprintf(incoming->prepared_root, sizeof(incoming->prepared_root), "%s",
                 existing->prepared_root);
        incoming->extracted_asset_count = existing->extracted_asset_count;
        incoming->extracted_audio_count = existing->extracted_audio_count;
        incoming->extracted_visual_count = existing->extracted_visual_count;
        incoming->extracted_layout_count = existing->extracted_layout_count;
        merged = true;
    }
    if (!incoming->last_played[0] && existing->last_played[0]) {
        snprintf(incoming->last_played, sizeof(incoming->last_played), "%s",
                 existing->last_played);
        merged = true;
    }
    return merged;
}

int player_app_ttf_library_candidates(const char *exe_dir,
                                      char out[][MAX_PATH_LEN], int max_out) {
    if (!out || max_out <= 0) return 0;

#if defined(_WIN32) || defined(_WIN64)
    static const char *kLibs[] = { "SDL3_ttf.dll", NULL };
#elif defined(__APPLE__)
    static const char *kLibs[] = { "libSDL3_ttf.0.dylib", "libSDL3_ttf.dylib", NULL };
#else
    static const char *kLibs[] = { "libSDL3_ttf.so.0", "libSDL3_ttf.so", NULL };
#endif

    int count = 0;
    /* 1. Beside the executable: a user can drop the library next to the player
     *    (the release does not ship it), and that beats any PATH hit. */
    if (exe_dir && exe_dir[0] && count < max_out) {
        size_t len = strlen(exe_dir);
        bool has_sep = exe_dir[len - 1] == '/' || exe_dir[len - 1] == '\\';
        snprintf(out[count], MAX_PATH_LEN, "%s%s%s", exe_dir, has_sep ? "" : "/", kLibs[0]);
        count++;
    }
    /* 2. Bare names: the platform loader's default search (PATH on Windows,
     *    the loader path elsewhere). */
    for (int i = 0; kLibs[i] && count < max_out; i++) {
        snprintf(out[count], MAX_PATH_LEN, "%s", kLibs[i]);
        count++;
    }
    return count;
}

bool player_app_discover_showcase(PlayerApp *app, const char *executable_directory) {
    if (!app || !executable_directory || !executable_directory[0]) return false;
    int written = snprintf(app->showcase_root, sizeof(app->showcase_root),
                           "%sdemos", executable_directory);
    if (written <= 0 || (size_t)written >= sizeof(app->showcase_root)) {
        app->showcase_root[0] = '\0';
        return false;
    }

    app->showcase_count = 0;
    for (int i = 0; i < nk_title_catalog_count && app->showcase_count < MAX_SHOWCASE_GAMES; i++) {
        const NkTitleEntry *title = &nk_title_catalog_entries[i];
        if (!title->id || strncmp(title->id, "showcase-", 9) != 0 ||
            !title->primary_disc_id || !title->display_name) continue;
        GameRecord game;
        memset(&game, 0, sizeof(game));
        game.is_sample = true;
        snprintf(game.disc_id, sizeof(game.disc_id), "%s", title->primary_disc_id);
        snprintf(game.title_name, sizeof(game.title_name), "%s", title->display_name);
        snprintf(game.title_id, sizeof(game.title_id), "%s", title->id);
        snprintf(game.disc_version, sizeof(game.disc_version), "1.00");
        snprintf(game.selected_executable, sizeof(game.selected_executable), "EBOOT.BIN");
        if (!player_join_path(game.iso_path, sizeof(game.iso_path), app->showcase_root,
                              "images")) continue;
        size_t image_root_len = strlen(game.iso_path);
        int image_written = snprintf(game.iso_path + image_root_len,
            sizeof(game.iso_path) - image_root_len, "%c%s.iso",
            nk_platform_path_separator(), title->primary_disc_id);
        if (image_written <= 0 ||
            (size_t)image_written >= sizeof(game.iso_path) - image_root_len ||
            !nk_platform_file_exists(game.iso_path)) continue;
        snprintf(game.prepared_root, sizeof(game.prepared_root), "%s", app->showcase_root);
        game.iso_size_bytes = (uint64_t)nk_platform_get_file_size(game.iso_path);
        NkLaunchDataRootStatus data_root_status = player_app_game_data_root_status(
            app, &game, NULL, 0, NULL, 0);
        game.is_prepared = nk_launch_validate_runtime_package(
            app->showcase_root, &game, NULL, NULL, 0) == NK_RUNTIME_PACKAGE_OK &&
            (data_root_status == NK_LAUNCH_DATA_ROOT_READY ||
             data_root_status == NK_LAUNCH_DATA_ROOT_NOT_REQUIRED);
        if (!game.is_prepared) continue;
        game.status = NK_STATUS_PREPARED;
        snprintf(game.last_played, sizeof(game.last_played), "Never");
        app->showcase_games[app->showcase_count++] = game;
    }
    player_app_sync_library(app);
    return app->showcase_count > 0;
}

#if defined(_WIN32) || defined(_WIN64)
static bool player_display_path_char_equal(char a, char b) {
    if (a == '/') a = '\\';
    if (b == '/') b = '\\';
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}
static bool player_display_is_separator(char c) {
    return c == '\\' || c == '/';
}
#else
static bool player_display_path_char_equal(char a, char b) {
    return a == b;
}
static bool player_display_is_separator(char c) {
    return c == '/';
}
#endif

/* The profile folder without trailing separators; 0 when it is empty. */
static size_t player_display_home_length(const char *home) {
    size_t n = strlen(home);
    while (n > 0 && player_display_is_separator(home[n - 1])) n--;
    return n;
}

/* True when the profile folder occurs at `at` as whole path components: what
 * follows it ends the text, or is a separator, white space, quote or bracket. */
static bool player_display_home_matches(const char *at, const char *home, size_t home_len) {
    for (size_t i = 0; i < home_len; i++) {
        if (at[i] == '\0' || !player_display_path_char_equal(at[i], home[i])) return false;
    }
    char next = at[home_len];
    return next == '\0' || player_display_is_separator(next) ||
           isspace((unsigned char)next) || next == '"' || next == '\'' ||
           next == ')' || next == ']' || next == ',';
}

size_t player_display_text_with_home(const char *in, const char *home,
                                     char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!in) return 0;
    size_t home_len = home ? player_display_home_length(home) : 0;
    size_t used = 0;
    const char *p = in;
    while (*p) {
        bool hit = false;
        if (home_len > 0) {
            /* A match must start a path component, not continue a name. */
            bool at_boundary = p == in ||
                !(isalnum((unsigned char)p[-1]) || p[-1] == '_' ||
                  p[-1] == '.' || p[-1] == '-');
            hit = at_boundary && player_display_home_matches(p, home, home_len);
        }
        if (hit) {
            if (used + 2 > out_size) break;
            out[used++] = '~';
            p += home_len;
            continue;
        }
        if (used + 2 > out_size) break;
        out[used++] = *p++;
    }
    out[used] = '\0';
    return used;
}

size_t player_display_text(const char *in, char *out, size_t out_size) {
    static char home[MAX_PATH_LEN];
    static bool home_ready;
    if (!home_ready) {
        const char *value = NULL;
#if defined(_WIN32) || defined(_WIN64)
        value = getenv("USERPROFILE");
#else
        value = getenv("HOME");
#endif
        if (value) snprintf(home, sizeof(home), "%s", value);
        else home[0] = '\0';
        home_ready = true;
    }
    return player_display_text_with_home(in, home, out, out_size);
}

size_t player_bitmap_glyph(const char *in, char out[3]) {
    unsigned char lead = (unsigned char)in[0];
    if (lead < 0x80) {
        if (lead >= 0x20 && lead < 0x7F) {
            out[0] = (char)lead;
        } else if (lead == '\t' || lead == '\n' || lead == '\r') {
            out[0] = ' ';
        } else {
            out[0] = '?';
        }
        out[1] = '\0';
        return 1;
    }
    size_t seq = (lead >= 0xC2 && lead <= 0xDF) ? 2u
               : (lead >= 0xE0 && lead <= 0xEF) ? 3u
               : (lead >= 0xF0 && lead <= 0xF4) ? 4u
               : 0u;
    /* nk_json_validate_utf8 reads only the seq bytes given, so a NUL inside the
     * sequence (or a missing continuation byte) fails here without overrun. */
    if (seq == 0 || !nk_json_validate_utf8((const uint8_t *)in, seq)) {
        out[0] = '?';
        out[1] = '\0';
        return 1;
    }
    if (lead == 0xE2 && (unsigned char)in[1] == 0x84 && (unsigned char)in[2] == 0xA2) {
        out[0] = 'T';
        out[1] = 'M';
        out[2] = '\0';
        return 3;
    }
    out[0] = '?';
    out[1] = '\0';
    return seq;
}

size_t player_text_for_bitmap_font(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!in) return 0;
    size_t used = 0;
    const char *p = in;
    while (*p) {
        char glyph[3];
        size_t advance = player_bitmap_glyph(p, glyph);
        size_t glyph_len = strlen(glyph);
        if (used + glyph_len + 1 > out_size) break;
        memcpy(out + used, glyph, glyph_len);
        used += glyph_len;
        p += advance;
    }
    out[used] = '\0';
    return used;
}

bool player_app_add_game(PlayerApp *app, const GameRecord *game) {
    if (!app || !game || game->disc_id[0] == '\0') return false;
    /* A sample is shown, never added to the user's library. */
    if (game->is_sample) return false;

    GameRecord merged = *game;
    for (int i = 0; i < app->library.count; i++) {
        if (strcmp(app->library.entries[i].disc_id, game->disc_id) == 0) {
            player_merge_readded_game(&app->library.entries[i], &merged);
            break;
        }
    }
    game = &merged;

    /* Both results used to be discarded before returning an unconditional
       true, so a rejected insert (the 64-entry limit) or an unwritable or full
       user-data directory still reported success. The entry then vanished on
       the next start, having never been persisted. Report what actually
       happened instead. */
    NkResult add_res = nk_library_add_or_update(&app->library, game);
    if (add_res != NK_OK) {
        player_app_sync_library(app);
        return false;
    }

    NkResult save_res = nk_library_save(&app->library, NULL);
    player_app_sync_library(app);
    return save_res == NK_OK;
}

int player_app_find_game_by_disc_id(const PlayerApp *app, const char *disc_id) {
    if (!app || !disc_id || disc_id[0] == 0) return -1;
    for (int i = 0; i < app->game_count; i++) {
        if (strcmp(app->games[i].disc_id, disc_id) == 0) {
            return i;
        }
    }
    return -1;
}

const char *player_app_selected_disc_id(const PlayerApp *app) {
    if (!app) return NULL;
    if (app->selected_game_index < 0 || app->selected_game_index >= app->game_count) return NULL;
    if (!app->games[app->selected_game_index].disc_id[0]) return NULL;
    return app->games[app->selected_game_index].disc_id;
}

int player_app_visible_library_cards(const PlayerApp *app) {
    if (!app) return 1;
    /* Cards are 260 wide on a 280 pitch, inset 32 from the left edge and given
       the same margin on the right. */
    int usable = app->window_width - 64;
    int fit = usable / 280;
    return fit < 1 ? 1 : fit;
}

int player_app_focus_count(const PlayerApp *app) {
    if (!app) return 1;
    switch (app->active_view) {
        case VIEW_LIBRARY:
        case PLAYER_VIEW_READY_LIBRARY:
            if (app->game_count <= 0) return 1;
            {
                /* Order matches render_loaded_library: primary action
                 * (PLAY/STOP/BUILD/REBUILD or a pending package check), add, remove, the
                 * per-title controller mapping choice, then the paging stops
                 * when the library overflows. The unavailable pill is never a
                 * stop. */
                int count = 0;
                const GameRecord *game = (app->selected_game_index >= 0 &&
                                          app->selected_game_index < app->game_count)
                    ? &app->games[app->selected_game_index]
                    : NULL;
                NkRuntimePackageStatus pkg_status = game ?
                    player_app_cached_runtime_package_status(app, game) :
                    NK_RUNTIME_PACKAGE_MISSING;
                bool game_ready =
                    game && player_app_cached_game_has_runtime(app, game);
                bool checking = game &&
                    player_app_runtime_package_check_pending(app, game);
                bool check_failed = game &&
                    player_app_runtime_package_check_failed(app, game);
                bool can_build = game && !game_ready && !checking &&
                                 !check_failed &&
                                 (pkg_status == NK_RUNTIME_PACKAGE_MISSING ||
                                  pkg_status == NK_RUNTIME_PACKAGE_STALE);
                if (game && (game_ready || app->is_game_running || can_build ||
                             check_failed || checking)) count++;
                count += player_game_is_showcase(game) ? 1 : 2; /* add + optional remove */
                count += game ? 1 : 0; /* global / this-game controller mapping */
                if (app->game_count > player_app_visible_library_cards(app)) count += 2;
                return count < 1 ? 1 : count;
            }
        case VIEW_BUILDING_PACKAGE:
        case VIEW_PREREQ_PROGRESS:
            return 1;
        case VIEW_PREREQ_CONSENT:
            return 2;
        case VIEW_PREREQ_ABOUT:
            return app->prerequisites.item_count > 0 ? 2 : 1;
        case VIEW_CONFIRM_REMOVE_TOOLS:
            return 2;
        case VIEW_INSPECTING:
            return 1;
        case VIEW_SUPPORTED_TITLE:
        case VIEW_EXPERIMENTAL_TITLE:
            return 2;
        case VIEW_UNSUPPORTED_TITLE:
            return 1;
        case VIEW_PREPARING:
            return 1;
        case VIEW_SETTINGS:
            /* Resolution (3) + presentation pacing (2) + game display toggles (3: vsync,
             * fullscreen, reduce-motion) + launcher fullscreen (1) + volume stepper (2) +
             * controller settings (1) + notices and remove-tools controls (2) + close (1),
             * in draw order. The 8x preset is not offered (GPU scale caps at 4x). */
            return 15;
        case VIEW_CONTROLLER_SETTINGS:
            if (input_settings_is_calibrating(&app->input_settings)) {
                switch (input_settings_get_calibration_stage(&app->input_settings)) {
                    case CALIBRATION_STAGE_REST:
                        return 1;
                    case CALIBRATION_STAGE_EXTREMES:
                    case CALIBRATION_STAGE_RESULT:
                        return 2;
                    default:
                        return 1;
                }
            }
            /* 14 digital controls rebind buttons + deadzone [-]/[+] (2) +
             * trigger threshold [-]/[+] (2) + guided calibration (1) + save (1) + reset (1) + back (1),
             * in draw order, plus the global / this-game mapping choice when a
             * library disc is available to name. */
            return player_app_selected_disc_id(app) ? 23 : 22;
        case VIEW_ERROR:
            return 1;
        case VIEW_SETUP_WIZARD:
            switch (app->wizard.step) {
                case WIZARD_STEP_WELCOME:
                    return 2;
                case WIZARD_STEP_SELECT_GAME:
                    return app->wizard.iso_selected ? 4 : 3;
                case WIZARD_STEP_INSPECT_VERIFY:
                    if (app->wizard.is_extracting) return 1;
                    return 3;
                case WIZARD_STEP_SYSTEM_FONTS:
                    return 6;
                case WIZARD_STEP_READY_LAUNCH:
                    return 3;
                default:
                    return 1;
            }
        default:
            return 1;
    }
}

void player_app_move_selection(PlayerApp *app, int delta) {
    if (!app || app->game_count <= 0) return;
    int index = app->selected_game_index;
    if (index < 0) {
        index = 0;
    } else {
        index += delta;
    }
    if (index < 0) index = 0;
    if (index >= app->game_count) index = app->game_count - 1;
    app->selected_game_index = index;
}

void player_app_move_focus(PlayerApp *app, int delta, int focus_count) {
    if (!app || focus_count <= 0) return;
    int focus = app->focus_index + delta;
    if (focus < 0) focus = 0;
    if (focus >= focus_count) focus = focus_count - 1;
    app->focus_index = focus;
}

void player_app_settings_init_default(PlayerSettings *settings) {
    if (!settings) return;
    settings->resolution_scale = 1; /* native 480x272: the verified path; upscaling is opt-in */
    settings->fullscreen = false;
    settings->launcher_fullscreen = false;
    settings->vsync = true;
    settings->fps_cap = -1;
    settings->master_volume = 80;
    settings->reduce_motion = false;
    settings->launcher_window_maximized = false;
    settings->launcher_window_position_valid = false;
    settings->launcher_window_x = 0;
    settings->launcher_window_y = 0;
    settings->launcher_window_width = 1280;
    settings->launcher_window_height = 720;
    settings->controller_name[0] = '\0';
    settings->controller_connected = false;
}

static bool resolution_scale_valid(int scale) {
    /* The GPU rasterizer supports at most 4x (ge_gpu MAX_SCALE); 8x was never
       honoured by any consumer, so a persisted 8 falls back to the default
       instead of pretending. */
    return scale == 1 || scale == 2 || scale == 3 || scale == 4;
}

static bool fps_cap_valid(int cap) {
    return cap == -1 || cap == 0;
}

NkResult player_app_load_settings(PlayerApp *app, const char *file_path) {
    if (!app) return NK_ERROR_GENERIC;

    char resolved_path[MAX_PATH_LEN];
    const char *target = file_path;
    if (!target || !target[0]) {
        if (app->settings_path[0]) {
            target = app->settings_path;
        } else {
            char config_dir[MAX_PATH_LEN];
            if (!nk_platform_get_path(NK_PATH_CONFIG, config_dir, sizeof(config_dir))) {
                player_app_settings_init_default(&app->settings);
                return NK_ERROR_IO;
            }
            size_t dlen = strlen(config_dir);
            if (dlen + 16 >= sizeof(resolved_path)) {
                player_app_settings_init_default(&app->settings);
                return NK_ERROR_IO;
            }
            char sep = nk_platform_path_separator();
            memcpy(resolved_path, config_dir, dlen);
            resolved_path[dlen] = sep;
            memcpy(resolved_path + dlen + 1, "settings.json", 14);
            target = resolved_path;
        }
    }
    snprintf(app->settings_path, sizeof(app->settings_path), "%s", target);

    if (!nk_platform_file_exists(target)) {
        player_app_settings_init_default(&app->settings);
        app->settings_notice[0] = '\0';
        return NK_OK;
    }

    /* Settings, decrypted-module, font and key paths all come from the
     * per-user data directory, which is UTF-8 and may hold non-ASCII
     * characters; the narrow CRT would misread them on Windows. */
    FILE *f = nk_fopen_utf8(target, "rb");
    if (!f) {
        player_app_settings_init_default(&app->settings);
        snprintf(app->settings_notice, sizeof(app->settings_notice),
                 "Could not open settings file; defaults restored.");
        return NK_ERROR_IO;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        player_app_settings_init_default(&app->settings);
        snprintf(app->settings_notice, sizeof(app->settings_notice),
                 "Settings file has invalid size; defaults restored.");
        return NK_ERROR_GENERIC;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        player_app_settings_init_default(&app->settings);
        return NK_ERROR_OUT_OF_MEMORY;
    }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    char err_buf[256] = {0};
    NkJsonNode *root = nk_json_parse(buf, got, err_buf, sizeof(err_buf));
    free(buf);

    if (!root || !nk_json_is_object(root)) {
        if (root) nk_json_free(root);
        player_app_settings_init_default(&app->settings);
        snprintf(app->settings_notice, sizeof(app->settings_notice),
                 "Settings file is corrupt; defaults restored.");
        return NK_ERROR_GENERIC;
    }

    NkJsonNode *sv_node = nk_json_obj_get(root, "schema_version");
    int64_t sv = 0;
    if (!sv_node || !nk_json_get_int64(sv_node, &sv) ||
        (sv != 1 && sv != NK_PLAYER_SETTINGS_SCHEMA_VERSION)) {
        nk_json_free(root);
        player_app_settings_init_default(&app->settings);
        snprintf(app->settings_notice, sizeof(app->settings_notice),
                 "Unsupported settings schema version; defaults restored.");
        return NK_ERROR_GENERIC;
    }

    /* Reset defaults first then apply fields */
    player_app_settings_init_default(&app->settings);

    NkJsonNode *scale_node = nk_json_obj_get(root, "resolution_scale");
    int64_t scale_val = 0;
    if (scale_node && nk_json_get_int64(scale_node, &scale_val) && resolution_scale_valid((int)scale_val)) {
        app->settings.resolution_scale = (int)scale_val;
    }

    NkJsonNode *fps_node = nk_json_obj_get(root, "fps_cap");
    int64_t fps_val = 0;
    if (fps_node && nk_json_get_int64(fps_node, &fps_val)) {
        if (sv == 1 && (fps_val == 30 || fps_val == 60)) {
            /* Legacy values capped host presentation and could hide a game's
             * native cadence. Migrate both to the authentic PSP scanout path. */
            app->settings.fps_cap = -1;
        } else if (fps_val == -1 || fps_val == 0) {
            app->settings.fps_cap = (int)fps_val;
        }
    }

    NkJsonNode *vsync_node = nk_json_obj_get(root, "vsync");
    bool vsync_val = false;
    if (vsync_node && nk_json_get_bool(vsync_node, &vsync_val)) {
        app->settings.vsync = vsync_val;
    }

    NkJsonNode *fs_node = nk_json_obj_get(root, "fullscreen");
    bool fs_val = false;
    if (fs_node && nk_json_get_bool(fs_node, &fs_val)) {
        app->settings.fullscreen = fs_val;
    }

    NkJsonNode *launcher_fs_node = nk_json_obj_get(root, "launcher_fullscreen");
    bool launcher_fs_val = false;
    if (launcher_fs_node && nk_json_get_bool(launcher_fs_node, &launcher_fs_val)) {
        app->settings.launcher_fullscreen = launcher_fs_val;
    }

    NkJsonNode *launcher_max_node = nk_json_obj_get(root, "launcher_window_maximized");
    bool launcher_max_val = false;
    if (launcher_max_node && nk_json_get_bool(launcher_max_node, &launcher_max_val)) {
        app->settings.launcher_window_maximized = launcher_max_val;
    }

    NkJsonNode *window_x_node = nk_json_obj_get(root, "launcher_window_x");
    NkJsonNode *window_y_node = nk_json_obj_get(root, "launcher_window_y");
    NkJsonNode *window_w_node = nk_json_obj_get(root, "launcher_window_width");
    NkJsonNode *window_h_node = nk_json_obj_get(root, "launcher_window_height");
    NkJsonNode *window_position_valid_node =
        nk_json_obj_get(root, "launcher_window_position_valid");
    bool window_position_valid = false;
    int64_t window_x = 0;
    int64_t window_y = 0;
    int64_t window_w = 0;
    int64_t window_h = 0;
    if (window_position_valid_node &&
        nk_json_get_bool(window_position_valid_node, &window_position_valid) &&
        window_position_valid && window_x_node && window_y_node &&
        nk_json_get_int64(window_x_node, &window_x) &&
        nk_json_get_int64(window_y_node, &window_y) &&
        window_x >= -131072 && window_x <= 131072 &&
        window_y >= -131072 && window_y <= 131072) {
        app->settings.launcher_window_x = (int)window_x;
        app->settings.launcher_window_y = (int)window_y;
        app->settings.launcher_window_position_valid = true;
    }
    if (window_w_node && nk_json_get_int64(window_w_node, &window_w) &&
        window_w >= 320 && window_w <= 16384) {
        app->settings.launcher_window_width = (int)window_w;
    }
    if (window_h_node && nk_json_get_int64(window_h_node, &window_h) &&
        window_h >= 240 && window_h <= 16384) {
        app->settings.launcher_window_height = (int)window_h;
    }

    NkJsonNode *rm_node = nk_json_obj_get(root, "reduce_motion");
    bool rm_val = false;
    if (rm_node && nk_json_get_bool(rm_node, &rm_val)) {
        app->settings.reduce_motion = rm_val;
    }

    NkJsonNode *vol_node = nk_json_obj_get(root, "master_volume");
    int64_t vol_val = 0;
    if (vol_node && nk_json_get_int64(vol_node, &vol_val)) {
        if (vol_val < 0) vol_val = 0;
        if (vol_val > 100) vol_val = 100;
        app->settings.master_volume = (int)vol_val;
    }

    nk_json_free(root);
    app->settings_notice[0] = '\0';
    return NK_OK;
}

NkResult player_app_save_settings(const PlayerApp *app, const char *file_path) {
    if (!app) return NK_ERROR_GENERIC;

    char resolved_path[MAX_PATH_LEN];
    const char *target = file_path;
    if (!target || !target[0]) {
        if (app->settings_path[0]) {
            target = app->settings_path;
        } else {
            char config_dir[MAX_PATH_LEN];
            if (!nk_platform_get_path(NK_PATH_CONFIG, config_dir, sizeof(config_dir))) {
                return NK_ERROR_IO;
            }
            size_t dlen = strlen(config_dir);
            if (dlen + 16 >= sizeof(resolved_path)) {
                return NK_ERROR_IO;
            }
            char sep = nk_platform_path_separator();
            memcpy(resolved_path, config_dir, dlen);
            resolved_path[dlen] = sep;
            memcpy(resolved_path + dlen + 1, "settings.json", 14);
            target = resolved_path;
        }
    }

    char tmp_path[MAX_PATH_LEN + 16];
    size_t tlen = strlen(target);
    if (tlen + 8 >= sizeof(tmp_path)) return NK_ERROR_IO;
    memcpy(tmp_path, target, tlen);
    memcpy(tmp_path + tlen, ".tmp", 5);

    char parent[MAX_PATH_LEN];
    snprintf(parent, sizeof(parent), "%s", target);
    char *slash = strrchr(parent, '/');
    char *bslash = strrchr(parent, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash && slash != parent) {
        *slash = '\0';
        if (!nk_platform_dir_exists(parent)) nk_platform_mkdir_p(parent);
    }

    FILE *f = nk_fopen_utf8(tmp_path, "wb");
    if (!f) return NK_ERROR_IO;

    fprintf(f, "{\n");
    fprintf(f, "  \"schema_version\": %d,\n", NK_PLAYER_SETTINGS_SCHEMA_VERSION);
    fprintf(f, "  \"resolution_scale\": %d,\n", app->settings.resolution_scale);
    fprintf(f, "  \"fps_cap\": %d,\n", app->settings.fps_cap);
    fprintf(f, "  \"vsync\": %s,\n", app->settings.vsync ? "true" : "false");
    fprintf(f, "  \"fullscreen\": %s,\n", app->settings.fullscreen ? "true" : "false");
    fprintf(f, "  \"launcher_fullscreen\": %s,\n",
            app->settings.launcher_fullscreen ? "true" : "false");
    fprintf(f, "  \"launcher_window_maximized\": %s,\n",
            app->settings.launcher_window_maximized ? "true" : "false");
    fprintf(f, "  \"launcher_window_position_valid\": %s,\n",
            app->settings.launcher_window_position_valid ? "true" : "false");
    fprintf(f, "  \"launcher_window_x\": %d,\n", app->settings.launcher_window_x);
    fprintf(f, "  \"launcher_window_y\": %d,\n", app->settings.launcher_window_y);
    fprintf(f, "  \"launcher_window_width\": %d,\n", app->settings.launcher_window_width);
    fprintf(f, "  \"launcher_window_height\": %d,\n", app->settings.launcher_window_height);
    fprintf(f, "  \"reduce_motion\": %s,\n", app->settings.reduce_motion ? "true" : "false");
    fprintf(f, "  \"master_volume\": %d\n", app->settings.master_volume);
    fprintf(f, "}\n");

    if (fflush(f) != 0) {
        fclose(f);
        nk_remove_utf8(tmp_path);
        return NK_ERROR_IO;
    }

#if defined(_WIN32) || defined(_WIN64)
    int fd = _fileno(f);
    if (fd >= 0) {
        HANDLE hFile = (HANDLE)_get_osfhandle(fd);
        if (hFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(hFile);
        }
    }
    fclose(f);

    WCHAR wtmp[32768], wtarget[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, tmp_path, -1, wtmp, 32768) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1, wtarget, 32768) <= 0) {
        nk_remove_utf8(tmp_path);
        return NK_ERROR_IO;
    }

    if (!MoveFileExW(wtmp, wtarget, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(wtmp);
        return NK_ERROR_IO;
    }
#else
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);

    if (rename(tmp_path, target) != 0) {
        remove(tmp_path);
        return NK_ERROR_IO;
    }
#endif

    return NK_OK;
}

static void maybe_persist_settings(PlayerApp *app) {
    if (app && app->settings_path[0]) {
        player_app_save_settings(app, app->settings_path);
    }
}

void player_app_set_resolution_scale(PlayerApp *app, int scale) {
    if (!app || !resolution_scale_valid(scale)) return;
    app->settings.resolution_scale = scale;
    maybe_persist_settings(app);
}

void player_app_cycle_resolution_scale(PlayerApp *app, int direction) {
    if (!app) return;
    /* UI offers 1/2/4: the GPU rasterizer caps at 4x (ge_gpu MAX_SCALE), so
     * scale 8 is not selectable. Scale 3 stays accepted for forward compatibility
     * (resolution_label knows it) but is skipped by the stepper. */
    static const int kOrder[] = { 1, 2, 4 };
    int current = app->settings.resolution_scale;
    int at = 0;
    for (int i = 0; i < 3; i++) {
        if (kOrder[i] == current) {
            at = i;
            break;
        }
        if (kOrder[i] < current) at = i;
    }
    if (direction < 0) {
        at = (at + 2) % 3;
    } else {
        at = (at + 1) % 3;
    }
    app->settings.resolution_scale = kOrder[at];
    maybe_persist_settings(app);
}

void player_app_set_fps_cap(PlayerApp *app, int cap) {
    if (!app || !fps_cap_valid(cap)) return;
    app->settings.fps_cap = cap;
    maybe_persist_settings(app);
}

void player_app_cycle_fps_cap(PlayerApp *app, int direction) {
    if (!app) return;
    (void)direction; /* With two choices, either direction selects the other. */
    app->settings.fps_cap = app->settings.fps_cap == -1 ? 0 : -1;
    maybe_persist_settings(app);
}

void player_app_toggle_fullscreen(PlayerApp *app) {
    if (!app) return;
    app->settings.fullscreen = !app->settings.fullscreen;
    maybe_persist_settings(app);
}

void player_app_toggle_launcher_fullscreen(PlayerApp *app) {
    if (!app) return;
    app->settings.launcher_fullscreen = !app->settings.launcher_fullscreen;
    maybe_persist_settings(app);
}

PlayerCloseDecision player_app_close_decision(const PlayerApp *app,
                                              bool user_confirmed) {
    if (app && app->is_game_running && !user_confirmed) {
        return PLAYER_CLOSE_CONFIRM_REQUIRED;
    }
    return PLAYER_CLOSE_QUIT;
}

bool player_app_close_request_batch_claim(bool *close_request_handled) {
    if (!close_request_handled || *close_request_handled) return false;
    *close_request_handled = true;
    return true;
}

void player_app_note_close_confirmation_failure(PlayerApp *app) {
    if (!app) return;
    app->close_confirmation_pending = true;
}

bool player_app_take_close_confirmation_fallback(PlayerApp *app) {
    if (!app || !app->close_confirmation_pending) return false;
    app->close_confirmation_pending = false;
    return true;
}

bool player_settings_uses_two_columns(int window_width, int window_height) {
    float width = (float)window_width;
    float card_width = width - 64.0f;
    if (card_width < 340.0f) {
        card_width = width > 32.0f ? width - 32.0f : width;
    }
    if (card_width > 1216.0f) card_width = 1216.0f;
    /* The wide settings controls reserve 220 px plus a 310 px button in the
       first column and the right column starts at
       player_settings_second_column_offset(), which never sits inside them,
       so the two columns are disjoint from the first card width the layout
       can hold (980 px). The single-column flow needs far more height than
       the narrow windows that reach it, so it must stay the exception. The
       management row sits below the launcher toggle; 620 px is the first raw
       client height where the compressed card keeps those rows disjoint. */
    return card_width >= 980.0f && window_height >= 620;
}

float player_settings_second_column_offset(float card_width) {
    /* The launcher control and its hint end 562 px into the card; keep a
       24 px gutter before the right column, which otherwise starts at half
       the card. */
    float half = card_width * 0.5f;
    return half > 586.0f ? half : 586.0f;
}

bool player_app_boot_event_is_window_ready(const char *line) {
    return line && (strstr(line, "BOOT_EVENT phase=window_ready") != NULL ||
                    strstr(line, "BOOT_EVENT phase=first_frame") != NULL);
}

bool player_app_child_window_ready(PlayerApp *app) {
    if (!app || app->child_window_ready || !app->boot_event_file_path[0]) {
        return app && app->child_window_ready;
    }

    FILE *file = nk_fopen_utf8(app->boot_event_file_path, "rb");
    if (!file) return false;

    char events[8192];
    size_t length = fread(events, 1, sizeof(events) - 1, file);
    fclose(file);
    events[length] = '\0';

    char *line = events;
    while (*line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        if (player_app_boot_event_is_window_ready(line)) {
            app->child_window_ready = true;
            break;
        }
        if (!next) break;
        line = next + 1;
    }
    return app->child_window_ready;
}

bool player_window_fit_to_display(PlayerWindowRect requested,
                                  PlayerWindowRect usable,
                                  PlayerWindowFrame frame,
                                  bool requested_position_valid,
                                  PlayerWindowRect *out) {
    if (!out || usable.width <= 0 || usable.height <= 0) return false;

    int left = frame.left > 0 ? frame.left : 0;
    int right = frame.right > 0 ? frame.right : 0;
    int top = frame.top > 0 ? frame.top : 0;
    int bottom = frame.bottom > 0 ? frame.bottom : 0;

    /* The fitted rectangle is a CLIENT rectangle: SDL_SetWindowSize and
       SDL_SetWindowPosition both address the client area, and the geometry the
       launcher persists (SDL_GetWindowSize/SDL_GetWindowPosition) is a client
       rectangle too, so a saved rect round-trips without a frame conversion.
       Clamping therefore happens against the display's client area -- the
       usable bounds reduced by the window frame. Clamping against the raw
       usable bounds instead placed the frame outside the display: on a real
       3840x2160 display a saved window at (0,0) was measured at a Win32 outer
       rect of (-11,-45), putting the title bar off-screen. */
    int64_t min_x = (int64_t)usable.x + left;
    int64_t min_y = (int64_t)usable.y + top;
    int64_t area_width = (int64_t)usable.width - left - right;
    int64_t area_height = (int64_t)usable.height - top - bottom;
    if (area_width <= 0 || area_height <= 0) return false;

    int requested_width = requested.width > 0 ? requested.width : 1280;
    int requested_height = requested.height > 0 ? requested.height : 720;
    double scale = 1.0;
    if ((double)area_width / requested_width < scale) {
        scale = (double)area_width / requested_width;
    }
    if ((double)area_height / requested_height < scale) {
        scale = (double)area_height / requested_height;
    }

    int width = (int)(requested_width * scale);
    int height = (int)(requested_height * scale);
    if (width < 1 || height < 1) return false;
    /* Rounding must never produce a client rectangle larger than the area. */
    if ((int64_t)width > area_width) width = (int)area_width;
    if ((int64_t)height > area_height) height = (int)area_height;

    int64_t max_x = min_x + area_width - width;
    int64_t max_y = min_y + area_height - height;
    int64_t x = requested.x;
    int64_t y = requested.y;

    bool intersects = requested_position_valid &&
        x < min_x + area_width &&
        x + width > min_x &&
        y < min_y + area_height &&
        y + height > min_y;
    if (!intersects) {
        x = min_x + (area_width - width) / 2;
        y = min_y + (area_height - height) / 2;
    } else {
        if (x < min_x) x = min_x;
        if (x > max_x) x = max_x;
        if (y < min_y) y = min_y;
        if (y > max_y) y = max_y;
    }

    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX) return false;
    out->x = (int)x;
    out->y = (int)y;
    out->width = width;
    out->height = height;
    return true;
}

void player_app_toggle_vsync(PlayerApp *app) {
    if (!app) return;
    app->settings.vsync = !app->settings.vsync;
    maybe_persist_settings(app);
}

void player_app_toggle_reduce_motion(PlayerApp *app) {
    if (!app) return;
    app->settings.reduce_motion = !app->settings.reduce_motion;
    maybe_persist_settings(app);
}

void player_app_adjust_volume(PlayerApp *app, int delta) {
    if (!app) return;
    int volume = app->settings.master_volume + delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    app->settings.master_volume = volume;
    maybe_persist_settings(app);
}

bool player_app_remove_game(PlayerApp *app, int game_index) {
    if (!app || game_index < 0 || game_index >= app->game_count) return false;
    const char *disc_id = app->games[game_index].disc_id;
    if (!disc_id || disc_id[0] == '\0') return false;
    if (player_game_is_showcase(&app->games[game_index])) return false;
    /* Only the library entry is removed. The user's ISO file on disk is
     * never touched. */
    if (nk_library_remove(&app->library, disc_id) != NK_OK) return false;
    if (nk_library_save(&app->library, NULL) != NK_OK) {
        player_app_sync_library(app);
        return false;
    }
    player_app_sync_library(app);
    if (app->game_count <= 0) {
        app->selected_game_index = -1;
        app->library_scroll_index = 0;
    } else {
        if (app->selected_game_index >= app->game_count) {
            app->selected_game_index = app->game_count - 1;
        }
        if (app->library_scroll_index < 0) app->library_scroll_index = 0;
    }
    app->focus_index = 0;
    return true;
}

void player_app_set_view(PlayerApp *app, PlayerView view) {
    if (!app) return;
    app->active_view = view;
    app->focus_index = 0;
}

void player_app_set_error(PlayerApp *app, const char *code, const char *title, const char *msg, const char *recovery_label, PlayerView return_view) {
    if (!app) return;
    snprintf(app->last_error.error_code, sizeof(app->last_error.error_code), "%s", code ? code : "ERROR_UNKNOWN");
    snprintf(app->last_error.title, sizeof(app->last_error.title), "%s", title ? title : "Operation Failed");
    snprintf(app->last_error.message, sizeof(app->last_error.message), "%s", msg ? msg : "An unexpected error occurred.");
    app->last_error.details[0] = '\0';
    snprintf(app->last_error.recovery_action_label, sizeof(app->last_error.recovery_action_label), "%s", recovery_label ? recovery_label : "Return to Library");
    app->last_error.return_view = return_view;
    app->last_error.failed_stage[0] = '\0';
    app->last_error.boundary_text[0] = '\0';
    app->last_error.log_file_path[0] = '\0';
    app->active_view = VIEW_ERROR;
}

void player_app_set_cli_not_found_error(PlayerApp *app,
                                        const char *recovery_label,
                                        PlayerView return_view) {
    if (!app) return;
    player_app_set_error(
        app, "CLI_NOT_FOUND", "Build Tools Not Found",
        "Nakagawa Recomp's build tools were not found next to the app. "
        "Reinstall Nakagawa Recomp and keep its folder together.",
        recovery_label, return_view);
    /* After set_error, which resets last_error, so the details survive. */
    package_builder_describe_cli_not_found(app->install_root,
                                           app->last_error.details,
                                           sizeof(app->last_error.details));
    fprintf(stderr, "[PLAYER] CLI_NOT_FOUND details: %s\n",
            app->last_error.details);
}

void player_app_populate_sample_games(PlayerApp *app) {
    if (!app) return;
    if (app->game_count > 0) return; /* Don't overwrite loaded library */

    /* Sample fixture for UI demonstration: marks as IDENTIFIED but NOT verified or
     * prepared. No verification or preparation pipeline has run in this build, so
     * the status must not claim otherwise. */
    GameRecord p5;
    memset(&p5, 0, sizeof(p5));
    p5.is_sample = true;
    snprintf(p5.disc_id, sizeof(p5.disc_id), "TEST00005");
    snprintf(p5.title_name, sizeof(p5.title_name), "PSPDEV Phase 5 Source-Owned Fixture");
    snprintf(p5.disc_version, sizeof(p5.disc_version), "1.00");
    snprintf(p5.iso_path, sizeof(p5.iso_path), "fixtures/pspdev_phase5/disc/game.iso");
    snprintf(p5.prepared_root, sizeof(p5.prepared_root), "fixtures/pspdev_phase5");
    snprintf(p5.title_id, sizeof(p5.title_id), "pspdev-phase5-v1");
    p5.iso_size_bytes = 2097152ULL;
    p5.status = NK_STATUS_IDENTIFIED;
    p5.is_prepared = false;
    snprintf(p5.last_played, sizeof(p5.last_played), "Never");

    /* In memory only. This went through player_app_add_game, which saves, so a
       demo or screenshot run wrote a fixture the user does not own into their
       real library.json and left it there. A fixture is for looking at, not
       for keeping. */
    if (nk_library_add_or_update(&app->library, &p5) == NK_OK) {
        player_app_sync_library(app);
    }

    /* The display fixture's generator is not source media. Even when its
       runtime artifacts exist, the package cannot be reported ready without
       the exact ISO revision required by launch validation. Keep the demo card
       truthful until the fixture has a source ISO and matching private identity. */
    GameRecord disp;
    memset(&disp, 0, sizeof(disp));
    disp.is_sample = true;
    snprintf(disp.disc_id, sizeof(disp.disc_id), "TEST00006");
    snprintf(disp.title_name, sizeof(disp.title_name), "Nakagawa Display Smoke Fixture");
    snprintf(disp.disc_version, sizeof(disp.disc_version), "1.00");
    snprintf(disp.iso_path, sizeof(disp.iso_path), "fixtures/display_smoke/generate.py");
    snprintf(disp.prepared_root, sizeof(disp.prepared_root), "fixtures/display_smoke");
    snprintf(disp.title_id, sizeof(disp.title_id), "display-smoke-v1");
    disp.iso_size_bytes = 0ULL;
    disp.is_prepared = player_app_game_has_runtime(app, &disp);
    disp.status = disp.is_prepared ? NK_STATUS_PREPARED : NK_STATUS_IDENTIFIED;
    snprintf(disp.last_played, sizeof(disp.last_played), "Never");

    if (nk_library_add_or_update(&app->library, &disp) == NK_OK) {
        player_app_sync_library(app);
    }
}

/* One formatter for the per-process boot-event marker pathname. The launch
 * sequence is process state, so the pathname is derived from a shared helper
 * rather than rebuilt by each caller. */
static int player_format_boot_event_path(char *out, size_t out_size,
                                         unsigned int sequence) {
    if (!out || out_size == 0) return 0;

    char cache_dir[MAX_PATH_LEN];
    if (!nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir))) {
        out[0] = '\0';
        return 0;
    }

#if defined(_WIN32) || defined(_WIN64)
    unsigned long process_id = (unsigned long)_getpid();
#else
    unsigned long process_id = (unsigned long)getpid();
#endif
    int written = snprintf(out, out_size, "%s%cplayer-boot-%lu-%u.events",
                           cache_dir, nk_platform_path_separator(),
                           process_id, sequence);
    if (written <= 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return 0;
    }
    return written;
}

/* Per-process launch sequence for the boot-event marker pathname. Single
 * shared state: the next-pathname query must predict the sequence the next
 * launch really consumes. */
static unsigned int player_boot_event_sequence;

void player_app_next_boot_event_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return;

    if (!player_format_boot_event_path(out, out_size,
                                       player_boot_event_sequence + 1u)) {
        out[0] = '\0';
    }
}

static bool player_prepare_boot_event_file(PlayerApp *app) {
    if (!app) return false;

    ++player_boot_event_sequence;
    if (!player_format_boot_event_path(app->boot_event_file_path,
                                       sizeof(app->boot_event_file_path),
                                       player_boot_event_sequence)) {
        app->boot_event_file_path[0] = '\0';
        return false;
    }

    int removal_result = remove(app->boot_event_file_path);
    /* An absent marker is the only expected remove() failure. Any other
       filesystem error leaves the handoff path unproven and must not let a
       stale readable marker authorize launcher minimization. Recheck both
       regular-file and directory paths so a concurrent replacement also
       fails closed. */
    if ((removal_result != 0 && errno != ENOENT) ||
        nk_platform_file_exists(app->boot_event_file_path) ||
        nk_platform_dir_exists(app->boot_event_file_path)) {
        app->boot_event_file_path[0] = '\0';
        return false;
    }
    return true;
}

bool player_app_should_attempt_window_handoff(bool interactive_window,
                                              bool game_running,
                                              bool handoff_attempted,
                                              bool child_window_ready) {
    /* The handoff is attempted at most once per launch. A real
     * SDL_MinimizeWindow failure leaves the launcher visible, which is the
     * intended fail-closed result, but the attempt must not be retried every
     * frame: each retry would rewrite the launcher's settings. */
    return interactive_window && game_running && !handoff_attempted &&
           child_window_ready;
}

/* Copies a boundary reason into the error card. A reason longer than the
 * card's buffer is shortened there and written whole to stderr, so it is
 * never silently lost. */
static void player_set_boundary_text(PlayerApp *app, const char *reason) {
    int n = snprintf(app->last_error.boundary_text,
                     sizeof(app->last_error.boundary_text), "%s", reason);
    if (n >= (int)sizeof(app->last_error.boundary_text)) {
        fprintf(stderr, "[PLAYER] full boundary reason: %s\n", reason);
    }
}

bool player_app_launch_game(PlayerApp *app, int game_index) {
    if (!app || game_index < 0 || game_index >= app->game_count) return false;
    const GameRecord *game = &app->games[game_index];
    app->close_confirmation_pending = false;
    /* A launch initiated by the player requests a GUI child by default, even if
       package preflight rejects it before launch-session preparation. */
    app->launch_session.config.gui_mode = !app->launch_headless;

    char package_error[2048] = "";
    NkRuntimePackageStatus package_status = player_app_validate_runtime_package(
        app, game, NULL, package_error, sizeof(package_error));
    char data_root_path[NK_MAX_PATH] = "";
    char data_root_reason[1024] = "";
    NkLaunchDataRootStatus data_root_status = player_app_game_data_root_status(
        app, game, data_root_path, sizeof(data_root_path), data_root_reason,
        sizeof(data_root_reason));
    bool runtime_available = package_status == NK_RUNTIME_PACKAGE_OK ||
        (package_status == NK_RUNTIME_PACKAGE_MISSING && !game->is_experimental &&
         nk_launch_runtime_available(player_package_root(app, game),
                                     game->title_id));
    if (!runtime_available) {
        player_app_set_error(app, "RUNTIME_PACKAGE_NOT_READY", "Game Not Ready",
                             "This game isn't ready to start yet. Build or repair its game files, then try again.",
                             "Return to Library", VIEW_LIBRARY);
        player_set_boundary_text(app, package_error[0] ? package_error :
                                     "Runtime package is missing or incompatible.");
        return false;
    }
    if (data_root_status != NK_LAUNCH_DATA_ROOT_READY &&
        data_root_status != NK_LAUNCH_DATA_ROOT_NOT_REQUIRED) {
        player_app_set_error(app,
                             data_root_status == NK_LAUNCH_DATA_ROOT_MISSING
                                 ? "TITLE_DATA_MISSING" : "TITLE_PROFILE_INVALID",
                             data_root_status == NK_LAUNCH_DATA_ROOT_MISSING
                                 ? "Game Data Missing" : "Game Settings Unavailable",
                             data_root_status == NK_LAUNCH_DATA_ROOT_MISSING
                                 ? "This game needs its data files before it can start. Add the missing files, then try again."
                                 : "This game's title settings could not be checked. Review the details and try again.",
                             "Return to Library", VIEW_LIBRARY);
        player_set_boundary_text(app, data_root_reason[0] ? data_root_reason :
                                     "The title data folder could not be resolved.");
        return false;
    }

    app->child_window_ready = false;
    app->launch_session.boot_event_file_path[0] = '\0';
    if (!player_prepare_boot_event_file(app)) {
        player_app_set_error(
            app,
            "LAUNCH_DETAILS_UNAVAILABLE",
            "Launch Details Unavailable",
            "Nakagawa couldn't save the game's launch details, so the game was not started. Check the app's cache folder and try again.",
            "Return to Library",
            VIEW_LIBRARY
        );
        return false;
    }
    printf("[PLAYER] Preparing launch session for %s (%s)...\n", game->disc_id, game->title_name);

    NkResult res = nk_launch_prepare_session(&app->launch_session, game,
                                              player_package_root(app, game));
    /* nk_launch defaults gui_mode to false for headless harnesses. */
    app->launch_session.config.gui_mode = !app->launch_headless;
    if (res != NK_OK) {
        if (app->boot_event_file_path[0]) remove(app->boot_event_file_path);
        printf("[PLAYER] Launch preparation failed: %s\n", app->launch_session.last_error);
        const char *err_code = "RUNTIME_NOT_FOUND";
        const char *err_title = "Recompiled Binary Not Available";
        if (res == NK_ERROR_INVALID_EXECUTABLE) {
            err_code = "STAGED_EXECUTABLE_INVALID";
            err_title = "Staged Executable Is Invalid";
        } else if (strstr(app->launch_session.last_error, "manifest") != NULL ||
            strstr(app->launch_session.last_error, "profile") != NULL ||
            strstr(app->launch_session.last_error, "catalogue") != NULL ||
            strstr(app->launch_session.last_error, "catalog") != NULL) {
            err_code = "MANIFEST_MISMATCH";
            err_title = "Title Manifest Mismatch";
        }
        player_app_set_error(
            app,
            err_code,
            err_title,
            app->launch_session.last_error[0] ? app->launch_session.last_error : "The recompiled game binary or configuration could not be found.",
            "Return to Library",
            VIEW_LIBRARY
        );
        return false;
    }

    /* Apply user settings via typed runtime configuration */
    player_app_apply_settings_to_session(&app->settings, &app->launch_session.config);
    player_app_apply_input_profile_to_session(app, game);
    if (app->boot_event_file_path[0]) {
        snprintf(app->launch_session.boot_event_file_path,
                 sizeof(app->launch_session.boot_event_file_path), "%s",
                 app->boot_event_file_path);
    }

    printf("[PLAYER] Spawning runtime: %s (ISO: %s)\n", app->launch_session.executable_path, app->launch_session.iso_path);

    NkResult start_res = nk_launch_start(&app->launch_session);
    if (start_res != NK_OK) {
        if (app->boot_event_file_path[0]) remove(app->boot_event_file_path);
        printf("[PLAYER] Runtime process spawn failed: %s\n", app->launch_session.last_error);
        player_app_set_error(
            app,
            "PROCESS_SPAWN_FAILED",
            "Failed to Launch Game",
            app->launch_session.last_error[0] ? app->launch_session.last_error : "Operating system failed to start the runtime process (CreateProcess failed).",
            "Return to Library",
            VIEW_LIBRARY
        );
        return false;
    }

    app->is_game_running = true;
    app->launch_time_ms = 0;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info) {
        strftime(app->games[game_index].last_played, sizeof(app->games[game_index].last_played),
                 "%Y-%m-%d %H:%M", tm_info);
        /* A bundled demo or sample that ran is not a library title: its last-played
         * time stays in memory and the library file is left as the user made it. */
        if (app->library.library_path[0] && !app->games[game_index].is_sample) {
            nk_library_add_or_update(&app->library, &app->games[game_index]);
            nk_library_save(&app->library, app->library.library_path);
        }
    }
    printf("[PLAYER] Game started successfully (PID: %d)!\n", app->launch_session.process.process_id);
    return true;
}

static bool player_copy_bounded_text(char *destination, size_t destination_size,
                                     const char *source) {
    if (!destination || destination_size == 0 || !source) return false;
    size_t length = strlen(source);
    if (length >= destination_size) return false;
    memcpy(destination, source, length + 1);
    return true;
}

static bool player_plan_take_entry(PlayerStagePlan *plan, const NkTitleEntry *entry) {
    if (!entry || entry->loose_content_root_count < 0 ||
        entry->loose_content_root_count > NK_TITLE_MAX_LOOSE_CONTENT_ROOTS ||
        (entry->loose_content_root_count != 0 && !entry->loose_content_roots)) return false;
    if (entry->data_root &&
        !player_copy_bounded_text(plan->data_root, sizeof(plan->data_root), entry->data_root)) {
        return false;
    }
    for (int i = 0; i < entry->loose_content_root_count; i++) {
        const NkLooseContentRoot *binding = &entry->loose_content_roots[i];
        if (!binding->root ||
            !player_copy_bounded_text(plan->root_storage[i], sizeof(plan->root_storage[i]),
                               binding->root)) return false;
        plan->roots[i] = plan->root_storage[i];
        plan->request.loose_content_root_count++;
    }
    return true;
}

bool player_app_build_stage_plan(const GameRecord *game, PlayerStagePlan *plan) {
    if (!game || !plan) return false;
    memset(plan, 0, sizeof(*plan));
    if (!player_copy_bounded_text(plan->iso_path, sizeof(plan->iso_path), game->iso_path) ||
        !player_copy_bounded_text(plan->disc_id, sizeof(plan->disc_id), game->disc_id) ||
        !player_copy_bounded_text(plan->disc_version, sizeof(plan->disc_version),
                           game->disc_version) ||
        !nk_platform_get_app_data_dir(plan->user_data_root,
                                      sizeof(plan->user_data_root))) return false;
    plan->request.iso_path = plan->iso_path;
    plan->request.user_data_root = plan->user_data_root;
    plan->request.disc_id = plan->disc_id;
    plan->request.disc_version = plan->disc_version;
    plan->request.loose_content_roots = plan->roots;
    plan->request.data_root = plan->data_root;

    bool valid;
    if (game->is_experimental) {
        char profile_hash[65];
        char error[256];
        NkTitleEntrySnapshot snapshot = {0};
        valid = nk_title_manifest_read_experimental_profile(
                    plan->user_data_root, game->disc_id, game->title_id,
                    game->selected_executable, &snapshot, profile_hash, error,
                    sizeof(error)) &&
                snapshot.entry.id && strcmp(snapshot.entry.id, game->title_id) == 0 &&
                snapshot.entry.primary_disc_id &&
                strcmp(snapshot.entry.primary_disc_id, game->disc_id) == 0 &&
                player_plan_take_entry(plan, &snapshot.entry);
        nk_title_catalog_snapshot_release(&snapshot);
    } else {
        nk_title_catalog_lock();
        const NkTitleEntry *by_disc = game->disc_id[0]
            ? nk_title_catalog_find_by_disc_id_locked(game->disc_id) : NULL;
        const NkTitleEntry *by_id = game->title_id[0]
            ? nk_title_catalog_find_by_id_locked(game->title_id) : NULL;
        if (!by_disc) {
            /* Not catalogued by disc: only the executable is staged. */
            valid = true;
        } else if (game->title_id[0] && (!by_id || strcmp(by_disc->id, by_id->id) != 0)) {
            valid = false;
        } else {
            valid = player_plan_take_entry(plan, by_disc);
        }
        nk_title_catalog_unlock();
    }
    if (!valid) {
        plan->request.loose_content_root_count = 0;
        plan->data_root[0] = '\0';
    }
    return valid;
}

bool player_app_inspected_game_needs_staging(const PlayerApp *app) {
    if (!app || !app->inspecting_game.disc_id[0]) return false;
    /* A disc already in the library keeps its staged files: judge the record
       ADD TO LIBRARY will actually save. */
    GameRecord merged = app->inspecting_game;
    for (int i = 0; i < app->library.count; i++) {
        if (strcmp(app->library.entries[i].disc_id, merged.disc_id) == 0) {
            (void)player_merge_readded_game(&app->library.entries[i], &merged);
            break;
        }
    }
    PlayerStagePlan *plan = (PlayerStagePlan *)calloc(1, sizeof(*plan));
    bool takes_data_from_disc = plan && player_app_build_stage_plan(&merged, plan) &&
                                player_stage_title_takes_data_from_disc(&plan->request);
    free(plan);
    if (!takes_data_from_disc) return false;
    return player_app_game_data_root_status(app, &merged, NULL, 0, NULL, 0) !=
           NK_LAUNCH_DATA_ROOT_READY;
}

bool player_app_add_inspected_game(PlayerApp *app) {
    if (!app) return false;
    if (player_app_inspected_game_needs_staging(app)) {
        player_app_start_setup_wizard(app);
        player_app_wizard_begin_extraction(app);
        return true;
    }
    if (!player_app_add_game(app, &app->inspecting_game)) return false;
    player_app_set_view(app, VIEW_LIBRARY);
    return true;
}

bool player_app_register_staged_game(PlayerApp *app) {
    if (!app || !app->inspecting_game.disc_id[0] ||
        !app->inspecting_game.assets_staged ||
        !app->inspecting_game.prepared_root[0]) return false;

    if (!player_app_add_game(app, &app->inspecting_game)) return false;
    int index = player_app_find_game_by_disc_id(app, app->inspecting_game.disc_id);
    if (index < 0) return false;
    app->selected_game_index = index;
    app->wizard.step = WIZARD_STEP_READY_LAUNCH;
    app->active_view = PLAYER_VIEW_READY_LIBRARY;
    app->focus_index = 0;
    return true;
}

void player_app_stop_game(PlayerApp *app) {
    if (!app) return;
    if (!app->is_game_running) {
        app->close_confirmation_pending = false;
        return;
    }
    printf("[PLAYER] Stopping active game session...\n");
    nk_launch_stop(&app->launch_session);
    app->is_game_running = false;
    app->close_confirmation_pending = false;
    app->child_window_ready = false;
    if (app->boot_event_file_path[0]) remove(app->boot_event_file_path);
}

void player_app_apply_settings_to_session(const PlayerSettings *settings,
                                          NkRuntimeConfig *config) {
    if (!settings || !config) return;
    config->resolution_scale = settings->resolution_scale;
    config->fps_cap = settings->fps_cap;
    config->vsync = settings->vsync;
    config->fullscreen = settings->fullscreen;
    config->master_volume = settings->master_volume;
}

NkResult player_app_apply_input_profile_to_session(PlayerApp *app, const GameRecord *game) {
    if (!app) return NK_ERROR_GENERIC;
    app->input_profile_notice[0] = '\0';
    if (!game || !game->disc_id[0]) return NK_ERROR_GENERIC;

    char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
    if (!input_settings_has_title_mapping(&app->input_settings, game->disc_id)) {
        /* No per-title entry: the disc runs the global mapping, which is the
         * file the player already owns and the runtime already resolves. */
        if (app->input_settings.profile_path[0]) {
            snprintf(app->launch_session.config.input_profile_path,
                     sizeof(app->launch_session.config.input_profile_path), "%s",
                     app->input_settings.profile_path);
        }
        return NK_OK;
    }

    char path[NK_MAX_PATH] = {0};
    char reason[96] = {0};
    NkResult res = input_settings_write_disc_profile(&app->input_settings, game->disc_id,
                                                     path, sizeof(path), diag, sizeof(diag));
    if (res != NK_OK) {
        snprintf(reason, sizeof(reason), "%.95s", diag);
        snprintf(app->input_profile_notice, sizeof(app->input_profile_notice),
                 "Mapping for %s could not be handed to the game (%.48s); it starts on the global mapping.",
                 game->disc_id, reason[0] ? reason : "unknown reason");
        printf("[PLAYER] %s\n", app->input_profile_notice);
        return res;
    }

    snprintf(app->launch_session.config.input_profile_path,
             sizeof(app->launch_session.config.input_profile_path), "%s", path);
    return NK_OK;
}

static bool player_read_semantic_stop(const char *path, char *boundary,
                                     size_t boundary_size,
                                     unsigned int *issue_number) {
    static const char prefix[] =
        "BOOT_EVENT phase=stop reason=semantic-boundary boundary=";
    if (boundary && boundary_size) boundary[0] = '\0';
    if (issue_number) *issue_number = 0;
    if (!path || !path[0] || !boundary || boundary_size == 0 || !issue_number) {
        return false;
    }
    FILE *events = nk_fopen_utf8(path, "rb");
    if (!events) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), events)) {
        if (strncmp(line, prefix, sizeof(prefix) - 1u) != 0) continue;
        const char *start = line + sizeof(prefix) - 1u;
        const char *separator = strchr(start, ' ');
        const char *issue = separator ? strstr(separator, " issue=") : NULL;
        if (!separator || !issue || issue <= start) continue;
        size_t name_len = (size_t)(separator - start);
        if (name_len == 0 || name_len >= boundary_size || name_len >= 96u) continue;
        bool valid_name = true;
        for (size_t i = 0; i < name_len; i++) {
            unsigned char ch = (unsigned char)start[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) {
                valid_name = false;
                break;
            }
        }
        if (!valid_name) continue;
        const char *digits = issue + 7;
        char *digits_end = NULL;
        unsigned long parsed = strtoul(digits, &digits_end, 10);
        if (digits_end == digits || parsed == 0 || parsed > 9999ul ||
            (*digits_end != '\n' && *digits_end != '\r' && *digits_end != '\0')) {
            continue;
        }
        memcpy(boundary, start, name_len);
        boundary[name_len] = '\0';
        *issue_number = (unsigned int)parsed;
        found = true;
    }
    fclose(events);
    return found;
}

bool player_app_monitor_game_session(PlayerApp *app, uint64_t now_ms) {
    if (!app || !app->is_game_running) return false;
    if (app->launch_time_ms == 0) {
        app->launch_time_ms = now_ms;
    }
    if (nk_launch_is_running(&app->launch_session)) {
        return false;
    }
    uint64_t elapsed_ms = now_ms >= app->launch_time_ms
        ? (now_ms - app->launch_time_ms) : 0;
    int code = nk_launch_wait(&app->launch_session, 0);
    char stopped_boundary[96] = "";
    unsigned int stopped_issue = 0;
    bool has_semantic_stop = player_read_semantic_stop(
        app->boot_event_file_path, stopped_boundary, sizeof(stopped_boundary),
        &stopped_issue);
    app->is_game_running = false;
    app->close_confirmation_pending = false;
    /* The child has exited and been reaped: release its process and job
       handles before the session is reused. The natural-exit path used to
       drop them on the floor; the next launch's nk_launch_prepare_session
       memset silently discarded the stale handle, leaking one process and
       one job handle per finished game in the long-lived player (#511). */
    nk_launch_stop(&app->launch_session);
    app->child_window_ready = false;
    if (app->boot_event_file_path[0] && !has_semantic_stop) {
        remove(app->boot_event_file_path);
    }
    printf("[PLAYER] Game process exited with code %d (ran for %llu ms)\n", code,
           (unsigned long long)elapsed_ms);

    if (has_semantic_stop) {
        player_app_set_error(
            app, "RUNTIME_SEMANTIC_BOUNDARY", "Game Stopped Early",
            "This part of the game isn't supported yet. Check the details below and try again after an update.",
            "Return to Library", VIEW_LIBRARY);
        snprintf(app->last_error.boundary_text,
                 sizeof(app->last_error.boundary_text),
                 "RUNTIME_SEMANTIC_BOUNDARY: %s; not supported yet.",
                 stopped_boundary);
        snprintf(app->last_error.log_file_path,
                 sizeof(app->last_error.log_file_path), "%s",
                 app->boot_event_file_path);
    } else if (elapsed_ms < 500) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg),
                 "Child runtime exited prematurely after %llu ms (exit code %d).\n"
                 "Process terminated before initialization or scheduler loop could start.",
                 (unsigned long long)elapsed_ms, code);
        player_app_set_error(app, "RUNTIME_PREMATURE_EXIT", "Child Process Terminated Early",
                             err_msg, "Return to Library", VIEW_LIBRARY);
    } else if (code != 0) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg),
                 "Child runtime process exited abnormally with code %d.\n"
                 "Check runtime log files for crash traceback or missing symbol details.",
                 code);
        player_app_set_error(app, "RUNTIME_ERROR_EXIT", "Child Process Error Exit",
                             err_msg, "Return to Library", VIEW_LIBRARY);
    }
    return true;
}

bool player_app_start_package_build(PlayerApp *app, int game_index) {
    if (!app || game_index < 0 || game_index >= app->game_count) return false;
    const GameRecord *game = &app->games[game_index];

    package_builder_init_session(&app->build_session, game->disc_id, game->title_name);

    char cli_path[NK_MAX_PATH];
    if (!package_builder_find_cli(app->install_root, cli_path, sizeof(cli_path))) {
        player_app_set_cli_not_found_error(app, "Return to Library",
                                           VIEW_LIBRARY);
        return false;
    }

    /* Keep downloaded tools and packages under the same app-data root, while
       still allowing a caller to supply an isolated user-data root. */
    char user_data_root[NK_MAX_PATH];
    if (app->runtime_root[0]) {
        snprintf(user_data_root, sizeof(user_data_root), "%s", app->runtime_root);
    } else if (!nk_platform_get_app_data_dir(user_data_root, sizeof(user_data_root))) {
        player_app_set_error(app, "DATA_DIR_UNAVAILABLE", "Per-User Data Unavailable",
                             "DATA_DIR_UNAVAILABLE: Windows could not resolve Local AppData. Set LOCALAPPDATA or APPDATA before building the package or installing tools.",
                             "Return to Library", VIEW_LIBRARY);
        return false;
    }

    /* Toolchain preflight: fail here with the missing tool named instead of
     * deep inside the build when gcc or mingw32-make is not on PATH. */
    char gcc_path[NK_MAX_PATH];
    char make_path[NK_MAX_PATH];
    char python_path[NK_MAX_PATH];
    bool have_python = package_builder_find_python_in_root(user_data_root,
                                                            python_path, sizeof(python_path));
    bool have_gcc = package_builder_find_tool_in_root("gcc", user_data_root,
                                                       gcc_path, sizeof(gcc_path));
    bool have_make = package_builder_find_tool_in_root("mingw32-make", user_data_root,
                                                        make_path, sizeof(make_path));
    bool missing_toolchain = !have_gcc || !have_make;
    if (!have_python || missing_toolchain) {
#if !defined(_WIN32) && !defined(_WIN64)
        player_app_set_error(app, "PREREQUISITE_PLATFORM_UNSUPPORTED",
                             "Build Tools Not Available on This Platform",
                             "Automatic prerequisite installation currently supports Windows x64 with UCRT64. Linux build-tool installation is in the works.",
                             "Return to Library", VIEW_LIBRARY);
        return false;
#else
        PackagePrerequisiteList list;
        char manifest_error[512];
        if (!package_builder_load_prerequisites(cli_path, &list,
                                                manifest_error, sizeof(manifest_error))) {
            player_app_set_error(app, "PREREQUISITE_MANIFEST_INVALID",
                                 "Pinned Build Tools Could Not Be Loaded",
                                 manifest_error,
                                 "Return to Library", VIEW_LIBRARY);
            return false;
        }
        if (!player_app_prereq_begin(app, game_index, !have_python,
                                     missing_toolchain)) return false;
        app->prerequisites.items = list;
        app->prerequisites.item_count = (int)list.count;
        app->prerequisites.total_bytes = list.total_bytes;
        app->prerequisites.bootstrap_python = !have_python;
        return true;
#endif
    }

    if (!have_python) {
        player_app_set_error(app, "PYTHON_NOT_FOUND", "Python 3 Interpreter Not Found",
                             "Python 3.14 was not found and the pinned bootstrap is unavailable.",
                             "Return to Library", VIEW_LIBRARY);
        return false;
    }

    /* Build into the same per-user root that package validation reads. */
    char log_dir[NK_MAX_PATH];
    snprintf(log_dir, sizeof(log_dir), "%.490s%clogs", user_data_root, nk_platform_path_separator());
    nk_platform_mkdir_p(log_dir);

    NkResult res = package_builder_start(&app->build_session, python_path, cli_path, user_data_root, log_dir);
    if (res != NK_OK) {
        player_app_set_error(app, "SPAWN_FAILED", "Failed to Start Package Builder",
                             "Failed to spawn the package builder child process.",
                             "Return to Library", VIEW_LIBRARY);
        return false;
    }

    player_app_set_view(app, VIEW_BUILDING_PACKAGE);
    return true;
}

void player_app_cancel_package_build(PlayerApp *app) {
    if (!app) return;
    package_builder_cancel(&app->build_session);
}

bool player_app_prereq_begin(PlayerApp *app, int game_index,
                             bool python_missing, bool toolchain_missing) {
    if (!app || game_index < 0 || game_index >= app->game_count ||
        (!python_missing && !toolchain_missing)) return false;
    memset(&app->prerequisites, 0, sizeof(app->prerequisites));
    app->prerequisites.phase = PLAYER_PREREQ_CONSENT;
    app->prerequisites.game_index = game_index;
    app->prerequisites.bootstrap_python = python_missing;
    app->prerequisite_job_started = false;
    app->prerequisite_fetcher_started = false;
    app->prerequisite_bootstrap_ready = false;
    app->prerequisite_cancel_sent = false;
    app->active_view = VIEW_PREREQ_CONSENT;
    app->focus_index = 0;
    return true;
}

void player_app_prereq_accept(PlayerApp *app, bool bootstrap_python) {
    if (!app || app->prerequisites.phase != PLAYER_PREREQ_CONSENT) return;
    app->prerequisite_cancel_sent = false;
    app->prerequisites.phase = bootstrap_python
        ? PLAYER_PREREQ_BOOTSTRAP : PLAYER_PREREQ_DOWNLOAD;
    app->active_view = VIEW_PREREQ_PROGRESS;
    app->focus_index = 0;
}

void player_app_prereq_update_progress(PlayerApp *app, const char *item,
                                       uint64_t item_received,
                                       uint64_t item_total,
                                       uint64_t total_received,
                                       uint64_t total_bytes) {
    if (!app || (app->prerequisites.phase != PLAYER_PREREQ_BOOTSTRAP &&
                 app->prerequisites.phase != PLAYER_PREREQ_DOWNLOAD)) return;
    snprintf(app->prerequisites.current_item, sizeof(app->prerequisites.current_item),
             "%s", item ? item : "");
    app->prerequisites.item_total_bytes = item_total;
    app->prerequisites.item_received_bytes = item_received > item_total
        ? item_total : item_received;
    app->prerequisites.total_bytes = total_bytes;
    app->prerequisites.total_received_bytes = total_received > total_bytes
        ? total_bytes : total_received;
}

void player_app_prereq_complete(PlayerApp *app) {
    if (!app || (app->prerequisites.phase != PLAYER_PREREQ_BOOTSTRAP &&
                 app->prerequisites.phase != PLAYER_PREREQ_DOWNLOAD)) return;
    app->prerequisites.phase = PLAYER_PREREQ_INSTALLED;
    app->prerequisites.resume_build_pending = true;
    app->prerequisite_job_started = false;
    app->prerequisite_fetcher_started = false;
    app->prerequisite_bootstrap_ready = false;
    app->prerequisite_cancel_sent = false;
    app->active_view = VIEW_LIBRARY;
}

void player_app_prereq_fail(PlayerApp *app, const char *code,
                            const char *message) {
    if (!app) return;
    snprintf(app->prerequisites.error_code, sizeof(app->prerequisites.error_code),
             "%s", code && code[0] ? code : "PREREQUISITE_INSTALL_FAILED");
    snprintf(app->prerequisites.error_message, sizeof(app->prerequisites.error_message),
             "%s", message && message[0] ? message :
             "The prerequisite could not be installed. Check the connection and free disk space, then retry.");
    app->prerequisites.phase = PLAYER_PREREQ_FAILED;
    if (strcmp(app->prerequisites.error_code, "CLI_NOT_FOUND") == 0) {
        player_app_set_cli_not_found_error(app, "Retry Download",
                                           VIEW_PREREQ_CONSENT);
    } else {
        player_app_set_error(app, app->prerequisites.error_code,
                             "Build Prerequisite Could Not Be Installed",
                             app->prerequisites.error_message,
                             "Retry Download", VIEW_PREREQ_CONSENT);
    }
}

void player_app_prereq_retry(PlayerApp *app) {
    if (!app || app->prerequisites.phase != PLAYER_PREREQ_FAILED ||
        app->last_error.return_view != VIEW_PREREQ_CONSENT) return;
    app->prerequisites.phase = PLAYER_PREREQ_CONSENT;
    app->prerequisites.cancel_requested = false;
    app->prerequisite_cancel_sent = false;
    player_app_set_view(app, VIEW_PREREQ_CONSENT);
}

void player_app_prereq_cancel(PlayerApp *app) {
    if (!app) return;
    app->prerequisites.cancel_requested = true;
    app->prerequisites.resume_build_pending = false;
    if (app->prerequisites.phase == PLAYER_PREREQ_CONSENT) {
        player_app_prereq_finish_cancel(app);
    }
}

void player_app_prereq_finish_cancel(PlayerApp *app) {
    if (!app) return;
    app->prerequisites.cancel_requested = false;
    app->prerequisites.resume_build_pending = false;
    app->prerequisites.phase = PLAYER_PREREQ_CANCELLED;
    app->prerequisite_job_started = false;
    app->prerequisite_fetcher_started = false;
    app->prerequisite_bootstrap_ready = false;
    app->prerequisite_cancel_sent = false;
    app->active_view = VIEW_LIBRARY;
    app->focus_index = 0;
}

bool player_app_prereq_take_resume(PlayerApp *app) {
    if (!app || !app->prerequisites.resume_build_pending) return false;
    app->prerequisites.resume_build_pending = false;
    return true;
}

bool player_app_open_prerequisite_about(PlayerApp *app) {
    if (!app) return false;
    char data_root[NK_MAX_PATH];
    char cli_path[NK_MAX_PATH];
    char error[512];
    PackagePrerequisiteList list;
    if (!player_app_data_root(app, data_root, sizeof(data_root))) {
        player_app_set_error(app, "DATA_DIR_UNAVAILABLE", "Per-User Data Unavailable",
                             "DATA_DIR_UNAVAILABLE: Windows could not resolve Local AppData. Set LOCALAPPDATA or APPDATA before inspecting installed tools.",
                             "Return to Settings", VIEW_SETTINGS);
        return false;
    }
    if (!package_builder_find_cli(app->install_root, cli_path, sizeof(cli_path))) {
        player_app_set_cli_not_found_error(app, "Return to Settings",
                                           VIEW_SETTINGS);
        return false;
    }
    if (!package_builder_load_prerequisites(cli_path, &list, error, sizeof(error))) {
        player_app_set_error(app, "PREREQUISITE_MANIFEST_INVALID",
                             "Pinned Build Tools Could Not Be Loaded", error,
                             "Return to Settings", VIEW_SETTINGS);
        return false;
    }
    package_builder_mark_prerequisites_installed(&list, data_root);
    app->prerequisites.items = list;
    app->prerequisites.item_count = 0;
    for (size_t i = 0; i < list.count; i++) {
        if (list.items[i].installed) app->prerequisites.item_count++;
    }
    app->requested_open_path[0] = '\0';
    player_app_set_view(app, VIEW_PREREQ_ABOUT);
    return true;
}

void player_app_remove_prerequisites(PlayerApp *app) {
    if (!app) return;
    if (app->prerequisite_job_started || app->prerequisite_fetcher_started ||
        app->prerequisites.phase == PLAYER_PREREQ_BOOTSTRAP ||
        app->prerequisites.phase == PLAYER_PREREQ_DOWNLOAD) {
        player_app_set_error(app, "TOOLS_IN_USE", "Build Tools Are In Use",
                             "Wait for the prerequisite operation to finish or cancel it before removing downloaded tools.",
                             "Return to Settings", VIEW_SETTINGS);
        return;
    }
    char data_root[NK_MAX_PATH];
    char code[48];
    char message[512];
    if (!player_app_data_root(app, data_root, sizeof(data_root))) {
        snprintf(code, sizeof(code), "DATA_DIR_UNAVAILABLE");
        snprintf(message, sizeof(message), "DATA_DIR_UNAVAILABLE: Windows could not resolve Local AppData. Set LOCALAPPDATA or APPDATA. No files were removed.");
    } else if (package_builder_remove_downloaded_tools(data_root, code, sizeof(code),
                                                       message, sizeof(message))) {
        for (size_t i = 0; i < app->prerequisites.items.count; i++)
            app->prerequisites.items.items[i].installed = false;
        app->prerequisites.item_count = 0;
        app->prerequisites.phase = PLAYER_PREREQ_IDLE;
        snprintf(app->settings_notice, sizeof(app->settings_notice),
                 "Downloaded build tools removed from app data.");
        player_app_set_view(app, VIEW_SETTINGS);
        return;
    }
    player_app_set_error(app, code[0] ? code : "TOOLS_REMOVE_FAILED",
                         "Downloaded Build Tools Could Not Be Removed",
                         message[0] ? message : "Close applications using the tools and retry.",
                         "Return to Settings", VIEW_SETTINGS);
}

void player_app_set_build_error(
    PlayerApp *app,
    const char *failed_stage,
    const char *boundary_text,
    const char *log_file_path
) {
    if (!app) return;
    char msg[512];
    if (boundary_text && boundary_text[0]) {
        snprintf(msg, sizeof(msg), "%s", boundary_text);
    } else {
        snprintf(msg, sizeof(msg), "Package build failed during stage '%s'. Check logs for details.",
                 failed_stage && failed_stage[0] ? failed_stage : "unknown");
    }
    player_app_set_error(app, "PACKAGE_BUILD_FAILED", "Package Build Failed",
                         msg, "Return to Library", VIEW_LIBRARY);
    if (failed_stage) {
        snprintf(app->last_error.failed_stage, sizeof(app->last_error.failed_stage), "%s", failed_stage);
    }
    if (boundary_text) {
        snprintf(app->last_error.boundary_text, sizeof(app->last_error.boundary_text), "%s", boundary_text);
    }
    if (log_file_path) {
        snprintf(app->last_error.log_file_path, sizeof(app->last_error.log_file_path), "%s", log_file_path);
    }
}

void player_app_start_setup_wizard(PlayerApp *app) {
    if (!app) return;
    memset(&app->wizard, 0, sizeof(app->wizard));
    app->wizard.step = WIZARD_STEP_WELCOME;
    app->wizard.extraction_result = NK_OK;
    app->wizard.iso_selected = (app->inspecting_game.iso_path[0] != '\0');
    snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
             "Welcome to the Nakagawa Recomp First-Time Setup Wizard.");
    app->active_view = VIEW_SETUP_WIZARD;
    app->focus_index = 0;
}

void player_app_wizard_next(PlayerApp *app) {
    if (!app) return;
    switch (app->wizard.step) {
        case WIZARD_STEP_WELCOME:
            app->wizard.step = WIZARD_STEP_SELECT_GAME;
            app->focus_index = 0;
            break;
        case WIZARD_STEP_SELECT_GAME:
            if (app->inspecting_game.iso_path[0] != '\0') {
                app->wizard.iso_selected = true;
                app->wizard.step = WIZARD_STEP_INSPECT_VERIFY;
            } else {
                app->request_file_picker = true;
            }
            app->focus_index = 0;
            break;
        case WIZARD_STEP_INSPECT_VERIFY:
            if (app->wizard.extraction_complete) {
                app->wizard.step = WIZARD_STEP_SYSTEM_FONTS;
            } else if (app->inspecting_game.status == NK_STATUS_VERIFIED &&
                       !app->wizard.is_extracting) {
                player_app_wizard_begin_extraction(app);
            } else if (app->wizard.is_extracting) {
                /* The only action exposed while the worker is active is
                   cancellation; ignore an accidental second activation. */
                break;
            } else {
                app->wizard.step = WIZARD_STEP_SELECT_GAME;
                app->request_file_picker = true;
            }
            app->focus_index = 0;
            break;
        case WIZARD_STEP_SYSTEM_FONTS:
            app->wizard.font_confirmed = true;
            app->wizard.step = WIZARD_STEP_READY_LAUNCH;
            app->focus_index = 0;
            break;
        case WIZARD_STEP_READY_LAUNCH:
            if (app->inspecting_game.disc_id[0] != '\0') {
                player_app_add_game(app, &app->inspecting_game);
                int idx = player_app_find_game_by_disc_id(app, app->inspecting_game.disc_id);
                if (idx >= 0) {
                    app->selected_game_index = idx;
                }
            }
            app->active_view = VIEW_LIBRARY;
            app->focus_index = 0;
            break;
        default:
            break;
    }
}

void player_app_wizard_begin_extraction(PlayerApp *app) {
    if (!app) return;
    app->wizard.iso_selected = true;
    app->wizard.step = WIZARD_STEP_INSPECT_VERIFY;
    app->wizard.is_extracting = true;
    app->wizard.extraction_requested = true;
    app->wizard.extraction_cancel_requested = false;
    app->wizard.extraction_complete = false;
    app->wizard.extraction_failed = false;
    app->wizard.extraction_result = NK_OK;
    app->wizard.extraction_percent = 0;
    app->wizard.files_extracted = 0;
    app->wizard.total_files = 0;
    app->wizard.extraction_current_file[0] = '\0';
    app->wizard.extraction_error[0] = '\0';
    snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
             "Extracting game assets into local application data...");
    app->focus_index = 0;
}

void player_app_wizard_back(PlayerApp *app) {
    if (!app) return;
    switch (app->wizard.step) {
        case WIZARD_STEP_WELCOME:
            player_app_wizard_cancel(app);
            break;
        case WIZARD_STEP_SELECT_GAME:
            app->wizard.step = WIZARD_STEP_WELCOME;
            app->focus_index = 0;
            break;
        case WIZARD_STEP_INSPECT_VERIFY:
            if (app->wizard.is_extracting) {
                app->wizard.extraction_cancel_requested = true;
                snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                         "Cancelling asset extraction...");
                break;
            }
            app->wizard.step = WIZARD_STEP_SELECT_GAME;
            app->focus_index = 0;
            break;
        case WIZARD_STEP_SYSTEM_FONTS:
            app->wizard.step = WIZARD_STEP_INSPECT_VERIFY;
            app->focus_index = 0;
            break;
        case WIZARD_STEP_READY_LAUNCH:
            app->wizard.step = WIZARD_STEP_SYSTEM_FONTS;
            app->active_view = VIEW_SETUP_WIZARD;
            app->focus_index = 0;
            break;
        default:
            break;
    }
}

void player_app_wizard_cancel(PlayerApp *app) {
    if (!app) return;
    if (app->wizard.is_extracting) {
        app->wizard.extraction_cancel_requested = true;
        snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                 "Cancelling asset extraction...");
        return;
    }
    app->active_view = VIEW_LIBRARY;
    app->focus_index = 0;
}

void player_app_wizard_reset_extraction(PlayerApp *app) {
    if (!app) return;
    app->inspecting_game.assets_staged = false;
    app->inspecting_game.extracted_asset_count = 0;
    app->inspecting_game.extracted_audio_count = 0;
    app->inspecting_game.extracted_visual_count = 0;
    app->inspecting_game.extracted_layout_count = 0;
    app->wizard.extraction_percent = 0;
    app->wizard.files_extracted = 0;
    app->wizard.total_files = 0;
    app->wizard.is_extracting = false;
    app->wizard.extraction_complete = false;
    app->wizard.extraction_failed = false;
    app->wizard.extraction_requested = false;
    app->wizard.extraction_cancel_requested = false;
    app->wizard.extraction_result = NK_OK;
    app->wizard.extraction_current_file[0] = '\0';
    app->wizard.extraction_error[0] = '\0';
    app->wizard.staging_root[0] = '\0';
}

bool player_app_wizard_take_extraction_request(PlayerApp *app) {
    if (!app || !app->wizard.extraction_requested) return false;
    app->wizard.extraction_requested = false;
    return true;
}

void player_app_wizard_request_cancel(PlayerApp *app) {
    if (!app) return;
    app->wizard.extraction_cancel_requested = true;
}

bool player_app_wizard_cancel_requested(const PlayerApp *app) {
    return app && app->wizard.extraction_cancel_requested;
}

void player_app_wizard_set_extraction_progress(PlayerApp *app, int percent,
                                               int files_extracted, int total_files,
                                               const char *current_file) {
    if (!app) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (files_extracted < 0) files_extracted = 0;
    if (total_files < 0) total_files = 0;
    app->wizard.extraction_percent = percent;
    app->wizard.files_extracted = files_extracted;
    app->wizard.total_files = total_files;
    if (current_file) {
        snprintf(app->wizard.extraction_current_file,
                 sizeof(app->wizard.extraction_current_file), "%s", current_file);
    }
}

void player_app_wizard_finish_extraction(PlayerApp *app, NkResult result,
                                         const char *error_message) {
    if (!app) return;
    app->wizard.is_extracting = false;
    app->wizard.extraction_result = result;
    app->wizard.extraction_requested = false;
    app->wizard.extraction_cancel_requested = false;
    if (result == NK_OK) {
        app->wizard.extraction_complete = true;
        app->wizard.extraction_failed = false;
        app->wizard.extraction_percent = 100;
        snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                 "Game assets extracted and staged successfully.");
        /* A worker completion that already carries a promoted staging root is
         * the end of the first-run transaction. The caller registers the
         * record before reaching here, so the next rendered frame is the
         * actionable library card rather than a wizard step that forgets the
         * newly installed title. Keep the old Fonts step for state-only
         * callers that have not supplied a staged root. */
        if (app->inspecting_game.assets_staged &&
            app->inspecting_game.prepared_root[0]) {
            app->wizard.step = WIZARD_STEP_READY_LAUNCH;
            app->active_view = PLAYER_VIEW_READY_LIBRARY;
        } else {
            app->wizard.step = WIZARD_STEP_SYSTEM_FONTS;
        }
        app->focus_index = 0;
    } else {
        app->wizard.extraction_complete = false;
        app->wizard.extraction_failed = true;
        snprintf(app->wizard.extraction_error, sizeof(app->wizard.extraction_error),
                 "%s", error_message && error_message[0] ? error_message : "Asset extraction failed.");
        snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                 "%s", app->wizard.extraction_error);
    }
}

static void player_preflight_add(PlayerCompatibilityPreflight *preflight,
                                 const char *code, PlayerPreflightStatus status,
                                 const char *message, const unsigned int *issues,
                                 size_t issue_count) {
    if (!preflight || preflight->count >= sizeof(preflight->checks) /
                                      sizeof(preflight->checks[0])) return;
    PlayerPreflightCheck *check = &preflight->checks[preflight->count++];
    memset(check, 0, sizeof(*check));
    snprintf(check->code, sizeof(check->code), "%s", code ? code : "");
    check->status = status;
    snprintf(check->message, sizeof(check->message), "%s", message ? message : "");
    if (issues) {
        if (issue_count > sizeof(check->issue_numbers) / sizeof(check->issue_numbers[0])) {
            issue_count = sizeof(check->issue_numbers) / sizeof(check->issue_numbers[0]);
        }
        memcpy(check->issue_numbers, issues, issue_count * sizeof(issues[0]));
        check->issue_count = issue_count;
    }
}

typedef enum {
    PLAYER_DECRYPTED_EBOOT_MISSING = 0,
    PLAYER_DECRYPTED_EBOOT_INVALID,
    PLAYER_DECRYPTED_EBOOT_VALID,
    PLAYER_DECRYPTED_EBOOT_PATH_INVALID
} PlayerDecryptedEbootState;

static uint32_t player_read_le32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool player_decrypted_eboot_paths(const char *runtime_root,
                                         const char *disc_id,
                                         char *directory, size_t directory_size,
                                         char *elf_path, size_t elf_path_size) {
    char canonical_id[10];
    if (!runtime_root || !disc_id || !directory || !elf_path ||
        directory_size == 0 || elf_path_size == 0 || strlen(disc_id) != 9) {
        return false;
    }
    for (size_t i = 0; i < 9; i++) {
        unsigned char ch = (unsigned char)disc_id[i];
        if (i < 4) {
            if (!isalpha(ch) || ch > 0x7f) return false;
            canonical_id[i] = (char)toupper(ch);
        } else {
            if (!isdigit(ch)) return false;
            canonical_id[i] = (char)ch;
        }
    }
    canonical_id[9] = '\0';
    int written = snprintf(directory, directory_size, "%s%ctitles%c%s%cdecrypted",
                           runtime_root, nk_platform_path_separator(),
                           nk_platform_path_separator(), canonical_id,
                           nk_platform_path_separator());
    if (written < 0 || (size_t)written >= directory_size) return false;
    written = snprintf(elf_path, elf_path_size, "%s%cEBOOT.elf", directory,
                       nk_platform_path_separator());
    return written >= 0 && (size_t)written < elf_path_size;
}

/* The shared PSP ELF32/MIPS layout rule (nk_iso.c) read from a file. */
static bool player_file_image_read(void *context, uint64_t offset, void *dst,
                                   uint32_t bytes) {
    FILE *file = (FILE *)context;
    if (offset > (uint64_t)LONG_MAX ||
        fseek(file, (long)offset, SEEK_SET) != 0) {
        return false;
    }
    return fread(dst, 1, bytes, file) == bytes;
}

static bool player_is_usable_mips_elf32(const char *path, bool module) {
    FILE *file = nk_fopen_utf8(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size < 0 || file_size > 512L * 1024L * 1024L) {
        fclose(file);
        return false;
    }
    bool usable = nk_elf32_mips_layout_violation(
                      player_file_image_read, file, (uint64_t)file_size, module) == NULL;
    fclose(file);
    return usable;
}

static PlayerDecryptedEbootState player_find_decrypted_eboot(
    const char *runtime_root, const char *disc_id, char *directory,
    size_t directory_size, char *elf_path, size_t elf_path_size) {
    if (!player_decrypted_eboot_paths(runtime_root, disc_id, directory,
                                      directory_size, elf_path, elf_path_size)) {
        return PLAYER_DECRYPTED_EBOOT_PATH_INVALID;
    }
    FILE *file = nk_fopen_utf8(elf_path, "rb");
    if (!file) return PLAYER_DECRYPTED_EBOOT_MISSING;
    fclose(file);
    return player_is_usable_mips_elf32(elf_path, false)
        ? PLAYER_DECRYPTED_EBOOT_VALID : PLAYER_DECRYPTED_EBOOT_INVALID;
}

/* Issue #308: built-in decryption boundary.  The player holds no key
 * material: a local-only key file at <user data>/keys/psp-keyfile.json (or
 * $NAKAGAWA_PSP_KEY_FILE) unlocks the boundary, which unwraps the disc's
 * encrypted executable and encrypted PRX modules into the private per-title
 * folder only, staged to a temporary name and renamed into place. */
typedef enum {
    PLAYER_BOUNDARY_OK = 0,
    PLAYER_BOUNDARY_NO_KEYFILE = 1,
    PLAYER_BOUNDARY_FAILED = 2
} PlayerBoundaryStatus;

static PlayerBoundaryStatus player_try_builtin_decrypt_member(
    const char *runtime_root, const char *iso_path, const char *disc_rel_path,
    const char *decrypt_dir, const char *out_name, const char *out_path,
    char *detail, size_t detail_size, bool module) {
    char key_path[NK_MAX_PATH + 64];
    char stage_dir[NK_MAX_PATH + 32];
    char stage_in[NK_MAX_PATH + 320];
    char stage_out[NK_MAX_PATH + 320];
    const char *env_override = getenv("NAKAGAWA_PSP_KEY_FILE");
    char keystore_error[256];
    NkKeystore *ks;
    NkPspCtx ctx;
    u8 *input = NULL;
    u8 *plain = NULL;
    size_t plain_size = 0;
    size_t input_size = 0;
    long file_size;
    FILE *file;
    int rc;

    if (env_override != NULL && env_override[0] != '\0') {
        snprintf(key_path, sizeof(key_path), "%s", env_override);
    } else {
        snprintf(key_path, sizeof(key_path), "%s/keys/psp-keyfile.json",
                 runtime_root);
    }
    file = nk_fopen_utf8(key_path, "rb");
    if (!file) return PLAYER_BOUNDARY_NO_KEYFILE;
    fclose(file);

    snprintf(stage_dir, sizeof(stage_dir), "%s/cache/decrypted", runtime_root);
    if (!nk_platform_dir_exists(stage_dir) && !nk_platform_mkdir_p(stage_dir)) {
        snprintf(detail, detail_size, "the private user-data cache is unavailable");
        return PLAYER_BOUNDARY_FAILED;
    }
    snprintf(stage_in, sizeof(stage_in), "%s/%s.in.stage", stage_dir, out_name);
    snprintf(stage_out, sizeof(stage_out), "%s/%s.stage", stage_dir, out_name);
    nk_remove_utf8(stage_in);
    nk_remove_utf8(stage_out);

    if (nk_iso_extract_file(iso_path, disc_rel_path, stage_in) != NK_OK) {
        snprintf(detail, detail_size, "the disc copy could not be extracted");
        nk_remove_utf8(stage_in);
        return PLAYER_BOUNDARY_FAILED;
    }
    file = nk_fopen_utf8(stage_in, "rb");
    if (file == NULL) {
        snprintf(detail, detail_size, "the disc copy could not be read");
        nk_remove_utf8(stage_in);
        return PLAYER_BOUNDARY_FAILED;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0 ||
        file_size > (long)(64u * 1024u * 1024u) || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        nk_remove_utf8(stage_in);
        snprintf(detail, detail_size, "the disc copy has an implausible size");
        return PLAYER_BOUNDARY_FAILED;
    }
    input_size = (size_t)file_size;
    input = (u8 *)malloc(input_size);
    if (input == NULL || fread(input, 1, input_size, file) != input_size) {
        fclose(file);
        free(input);
        nk_remove_utf8(stage_in);
        snprintf(detail, detail_size, "the disc copy could not be buffered");
        return PLAYER_BOUNDARY_FAILED;
    }
    fclose(file);
    nk_remove_utf8(stage_in);

    ks = nk_keystore_create();
    if (ks == NULL) {
        free(input);
        snprintf(detail, detail_size, "out of memory loading the key file");
        return PLAYER_BOUNDARY_FAILED;
    }
    if (nk_keystore_load_file(ks, key_path, keystore_error,
                              sizeof(keystore_error)) != NK_PSP_OK) {
        snprintf(detail, detail_size, "the key file is invalid: %.180s",
                 keystore_error);
        nk_keystore_free(ks);
        free(input);
        return PLAYER_BOUNDARY_FAILED;
    }
    nk_psp_ctx_init(&ctx, ks);
    rc = nk_container_decrypt(&ctx, input, input_size, &plain, &plain_size);
    nk_keystore_free(ks);
    free(input);
    if (rc != NK_PSP_OK) {
        snprintf(detail, detail_size, "%s",
                 ctx.message[0] != '\0' ? ctx.message
                                         : "the container could not be decrypted");
        return PLAYER_BOUNDARY_FAILED;
    }

    if (!nk_platform_dir_exists(decrypt_dir) &&
        !nk_platform_mkdir_p(decrypt_dir)) {
        free(plain);
        snprintf(detail, detail_size, "the per-title decrypted folder is unavailable");
        return PLAYER_BOUNDARY_FAILED;
    }
    file = nk_fopen_utf8(stage_out, "wb");
    if (file == NULL || fwrite(plain, 1, plain_size, file) != plain_size) {
        if (file != NULL) fclose(file);
        free(plain);
        nk_remove_utf8(stage_out);
        snprintf(detail, detail_size, "the decrypted image could not be written");
        return PLAYER_BOUNDARY_FAILED;
    }
    fclose(file);
    free(plain);
    nk_remove_utf8(out_path);
    if (nk_rename_utf8(stage_out, out_path) != 0) {
        nk_remove_utf8(stage_out);
        snprintf(detail, detail_size, "the decrypted image could not be moved into place");
        return PLAYER_BOUNDARY_FAILED;
    }
    if (!player_is_usable_mips_elf32(out_path, module)) {
        nk_remove_utf8(out_path);
        snprintf(detail, detail_size, "the decrypted image is not a usable MIPS ELF32");
        return PLAYER_BOUNDARY_FAILED;
    }
    return PLAYER_BOUNDARY_OK;
}

/* Issue #308 guest-module boundary: the disc's own PRX modules are resolved
   through the same per-title folder and built-in boundary as the executable,
   one module at a time and fail closed per module.  CFW patch-module exclusion
   stays the intake route's named job; this reports decryption readiness. */
/* Disc surveys found 35+ PRXs in one folder; 256 covers that count with a
   fixed upper bound for malformed images. */
#define PLAYER_MAX_GUEST_MODULES 256

typedef struct {
    char name[256];
    char rel_path[NK_ISO_MODULE_TREE_MAX_PATH_BYTES];
    uint32_t lba;
    uint32_t size;
    bool encrypted;
} PlayerModuleCandidate;

typedef enum {
    PLAYER_MODULE_SCAN_OK = 0,
    PLAYER_MODULE_SCAN_CANDIDATE_LIMIT,
    PLAYER_MODULE_SCAN_DUPLICATE_NAME,
    PLAYER_MODULE_SCAN_DIRECTORY_LIMIT,
    PLAYER_MODULE_SCAN_PATH_LIMIT,
    PLAYER_MODULE_SCAN_INVALID_TREE,
    PLAYER_MODULE_SCAN_OPEN_FAILED,
    PLAYER_MODULE_SCAN_INVALID_ARGUMENT,
    PLAYER_MODULE_SCAN_CALLBACK_STOPPED
} PlayerModuleScanStatus;

static bool player_name_equals_ignore_case(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool player_name_has_suffix_ignore_case(const char *name,
                                               const char *suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    return name_len >= suffix_len &&
           player_name_equals_ignore_case(name + (name_len - suffix_len), suffix);
}

/* A disc directory entry names a file the boundary writes under the private
 * per-title folder, and a crafted image controls those bytes. Accept only the
 * rule the tooling applies (tools/title_manifest.py FILENAME_RE and
 * WINDOWS_RESERVED): an alphanumeric first character, then [A-Za-z0-9._-],
 * at most 128 characters, no trailing dot, no reserved Windows device name. So
 * "..\\x.prx", "a/b.prx" or "CON.prx" can never leave or alias that folder. */
bool player_module_name_is_safe(const char *name) {
    static const char *const reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    size_t len = strlen(name);
    if (len == 0 || len > 128 || name[len - 1] == '.') return false;
    if (!isalnum((unsigned char)name[0])) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-') return false;
    }
    size_t base_len = strcspn(name, ".");
    for (size_t r = 0; r < sizeof(reserved) / sizeof(reserved[0]); r++) {
        if (strlen(reserved[r]) != base_len) continue;
        size_t k = 0;
        while (k < base_len &&
               toupper((unsigned char)name[k]) == (unsigned char)reserved[r][k]) k++;
        if (k == base_len) return false;
    }
    return true;
}

static bool player_module_name_is_executable(const char *name,
                                             const char *selected_name) {
    return player_name_equals_ignore_case(name, "EBOOT.BIN") ||
           player_name_equals_ignore_case(name, "BOOT.BIN") ||
           player_name_equals_ignore_case(name, "EBOOT.OLD") ||
           (selected_name != NULL && selected_name[0] != '\0' &&
            player_name_equals_ignore_case(name, selected_name));
}

static bool player_prx_header_supported(const unsigned char *header,
                                        uint32_t header_size) {
    if (header_size < 0x64 || header[0x27] < 1 || header[0x27] > 4) return false;
    uint32_t total = 0;
    for (uint32_t i = 0; i < header[0x27]; i++) {
        uint32_t value = player_read_le32(header + 0x54 + i * 4);
        if (value == 0 || value > 64u * 1024u * 1024u) return false;
        total += value;
    }
    return total <= 64u * 1024u * 1024u;
}

/* The shared PSP ELF32/MIPS layout rule (nk_iso.c) for a module still inside the ISO. */
static bool player_iso_elf32_mips_usable(NkIsoReader *reader, uint32_t lba,
                                         uint32_t size, bool module) {
    return nk_iso_elf32_mips_layout_usable(reader, lba, size, module);
}

typedef struct {
    NkIsoReader *reader;
    const char *selected_name;
    PlayerModuleCandidate *modules;
    size_t max_modules;
    size_t count;
    PlayerModuleScanStatus status;
    char *boundary_message;
    size_t boundary_message_size;
} PlayerModuleScanContext;

static bool player_scan_module_entry(const char *member_path,
                                     const NkIsoDirEntry *entry,
                                     void *userdata) {
    PlayerModuleScanContext *context = (PlayerModuleScanContext *)userdata;
    unsigned char header[0x64];
    uint32_t header_size;
    bool encrypted = false;
    if (entry->multi_extent || !player_module_name_is_safe(entry->name)) return true;
    if (!player_name_has_suffix_ignore_case(entry->name, ".prx") &&
        !player_name_has_suffix_ignore_case(entry->name, ".elf")) return true;
    if (player_module_name_is_executable(entry->name, context->selected_name)) {
        return true;
    }
    if (entry->size == 0 || entry->size > 64u * 1024u * 1024u) return true;
    header_size = entry->size < (uint32_t)sizeof(header)
                      ? entry->size : (uint32_t)sizeof(header);
    if (nk_iso_reader_read(context->reader, entry->lba, 0, header, header_size) !=
        (int)header_size) {
        return true;
    }
    if (memcmp(header, "\x7f" "ELF", 4) == 0) {
        if (!player_iso_elf32_mips_usable(context->reader, entry->lba,
                                         entry->size, true)) {
            return true;
        }
    } else if (memcmp(header, "~PSP", 4) == 0) {
        if (!player_prx_header_supported(header, header_size)) return true;
        encrypted = true;
    } else if (memcmp(header, "~SCE", 4) == 0) {
        encrypted = true;
    } else {
        return true;
    }

    for (size_t i = 0; i < context->count; i++) {
        if (player_name_equals_ignore_case(context->modules[i].name,
                                           entry->name)) {
            context->status = PLAYER_MODULE_SCAN_DUPLICATE_NAME;
            snprintf(context->boundary_message, context->boundary_message_size,
                     "DUPLICATE_DISC_MODULE_BASENAME: %.128s occurs at %.900s "
                     "and %.900s; automatic module intake is in the works.",
                     entry->name, context->modules[i].rel_path, member_path);
            return false;
        }
    }
    if (context->count >= context->max_modules) {
        context->status = PLAYER_MODULE_SCAN_CANDIDATE_LIMIT;
        snprintf(context->boundary_message, context->boundary_message_size,
                 "DISC_MODULE_CANDIDATE_LIMIT: ISO contains more than %u "
                 "guest-module candidates; larger module sets are in the works.",
                 PLAYER_MAX_GUEST_MODULES);
        return false;
    }
    memset(&context->modules[context->count], 0,
           sizeof(context->modules[context->count]));
    snprintf(context->modules[context->count].name,
             sizeof(context->modules[context->count].name), "%s", entry->name);
    snprintf(context->modules[context->count].rel_path,
             sizeof(context->modules[context->count].rel_path), "%s", member_path);
    context->modules[context->count].lba = entry->lba;
    context->modules[context->count].size = entry->size;
    context->modules[context->count].encrypted = encrypted;
    context->count++;
    return true;
}

static PlayerModuleScanStatus player_scan_disc_modules(
    const char *iso_path, const char *selected_name,
    PlayerModuleCandidate *modules, size_t max_modules, size_t *out_count,
    char *boundary_message, size_t boundary_message_size) {
    NkIsoReader *reader = nk_iso_reader_open(iso_path);
    PlayerModuleScanContext context;
    NkIsoModuleWalkStatus walk_status;
    if (out_count) *out_count = 0;
    if (boundary_message && boundary_message_size) boundary_message[0] = '\0';
    if (reader == NULL) {
        if (boundary_message && boundary_message_size) {
            snprintf(boundary_message, boundary_message_size,
                     "DISC_MODULE_TREE_OPEN_FAILED: ISO could not be opened.");
        }
        return PLAYER_MODULE_SCAN_OPEN_FAILED;
    }
    memset(&context, 0, sizeof(context));
    context.reader = reader;
    context.selected_name = selected_name;
    context.modules = modules;
    context.max_modules = max_modules;
    context.status = PLAYER_MODULE_SCAN_OK;
    context.boundary_message = boundary_message;
    context.boundary_message_size = boundary_message_size;
    walk_status = nk_iso_reader_walk_module_tree(
        reader, player_scan_module_entry, &context);
    nk_iso_reader_close(reader);
    if (out_count) *out_count = context.count;
    if (context.status != PLAYER_MODULE_SCAN_OK) return context.status;
    switch (walk_status) {
    case NK_ISO_MODULE_WALK_OK:
        return PLAYER_MODULE_SCAN_OK;
    case NK_ISO_MODULE_WALK_DIRECTORY_LIMIT:
        if (boundary_message && boundary_message_size) {
            snprintf(boundary_message, boundary_message_size,
                     "DISC_MODULE_DIRECTORY_LIMIT: ISO module discovery exceeds "
                     "%u directories; broader discovery is in the works.",
                     NK_ISO_MODULE_TREE_MAX_DIRECTORIES);
        }
        return PLAYER_MODULE_SCAN_DIRECTORY_LIMIT;
    case NK_ISO_MODULE_WALK_PATH_LIMIT:
        if (boundary_message && boundary_message_size) {
            snprintf(boundary_message, boundary_message_size,
                     "DISC_MODULE_PATH_LIMIT: ISO member path exceeds %u bytes; "
                     "broader path-aware intake is in the works.",
                     NK_ISO_MODULE_TREE_MAX_PATH_BYTES);
        }
        return PLAYER_MODULE_SCAN_PATH_LIMIT;
    case NK_ISO_MODULE_WALK_INVALID_TREE:
        if (boundary_message && boundary_message_size) {
            snprintf(boundary_message, boundary_message_size,
                     "DISC_MODULE_TREE_INVALID: ISO module directories could "
                     "not be listed safely.");
        }
        return PLAYER_MODULE_SCAN_INVALID_TREE;
    case NK_ISO_MODULE_WALK_INVALID_ARGUMENT:
        if (boundary_message && boundary_message_size) {
            snprintf(boundary_message, boundary_message_size,
                     "DISC_MODULE_SCAN_INVALID_ARGUMENT: ISO module scan "
                     "received invalid reader state.");
        }
        return PLAYER_MODULE_SCAN_INVALID_ARGUMENT;
    case NK_ISO_MODULE_WALK_CALLBACK_STOPPED:
        if (boundary_message && boundary_message_size) {
            snprintf(boundary_message, boundary_message_size,
                     "DISC_MODULE_SCAN_CALLBACK_STOPPED: ISO module scan "
                     "stopped without a candidate boundary.");
        }
        return PLAYER_MODULE_SCAN_CALLBACK_STOPPED;
    }
    if (boundary_message && boundary_message_size) {
        snprintf(boundary_message, boundary_message_size,
                 "DISC_MODULE_SCAN_STATUS_UNKNOWN: ISO module scan returned "
                 "an unknown status.");
    }
    return PLAYER_MODULE_SCAN_INVALID_TREE;
}

static void player_check_guest_modules(PlayerApp *app,
                                       PlayerCompatibilityPreflight *preflight,
                                       const char *runtime_root) {
    PlayerModuleCandidate *modules = NULL;
    char decrypted_dir[NK_MAX_PATH * 2];
    char decrypted_elf[NK_MAX_PATH * 2];
    char key_path[NK_MAX_PATH + 64];
    char first_name[256];
    char first_detail[320];
    char message[2048];
    char discovery_message[2048];
    char more[40];
    size_t total = 0, ready = 0, not_ready = 0;
    bool first_encrypted = false;
    bool first_boundary = false;
    const char *env_override;
    PlayerModuleScanStatus scan_status;
    if (!app->inspecting_game.iso_path[0] || !app->inspecting_game.disc_id[0]) {
        return;
    }
    if (!player_decrypted_eboot_paths(runtime_root, app->inspecting_game.disc_id,
                                      decrypted_dir, sizeof(decrypted_dir),
                                      decrypted_elf, sizeof(decrypted_elf))) {
        return;
    }
    modules = (PlayerModuleCandidate *)calloc(PLAYER_MAX_GUEST_MODULES,
                                               sizeof(*modules));
    if (modules == NULL) {
        static const unsigned int issues[] = { 308 };
        player_preflight_add(preflight, "GUEST_MODULES", PREFLIGHT_UNSUPPORTED,
                             "DISC_MODULE_SCAN_ALLOCATION_FAILED: module discovery "
                             "could not allocate its bounded scan table.",
                             issues, 1);
        return;
    }
    scan_status = player_scan_disc_modules(
        app->inspecting_game.iso_path, app->inspecting_game.selected_executable,
        modules, PLAYER_MAX_GUEST_MODULES, &total, discovery_message,
        sizeof(discovery_message));
    if (scan_status != PLAYER_MODULE_SCAN_OK) {
        static const unsigned int issues[] = { 308 };
        player_preflight_add(preflight, "GUEST_MODULES", PREFLIGHT_UNSUPPORTED,
                             discovery_message[0] ? discovery_message :
                                 "DISC_MODULE_SCAN_FAILED: module discovery failed "
                                 "closed.",
                             issues, 1);
        free(modules);
        return;
    }
    if (total == 0) {
        free(modules);
        return;
    }
    env_override = getenv("NAKAGAWA_PSP_KEY_FILE");
    if (env_override != NULL && env_override[0] != '\0') {
        snprintf(key_path, sizeof(key_path), "%s", env_override);
    } else {
        snprintf(key_path, sizeof(key_path), "%s/keys/psp-keyfile.json",
                 runtime_root);
    }
    first_name[0] = '\0';
    first_detail[0] = '\0';
    for (size_t i = 0; i < total; i++) {
        PlayerModuleCandidate *module = &modules[i];
        char module_path[NK_MAX_PATH * 2 + 320];
        int written = snprintf(module_path, sizeof(module_path), "%s%c%s",
                               decrypted_dir, nk_platform_path_separator(),
                               module->name);
        FILE *existing;
        char detail[320];
        PlayerBoundaryStatus status;
        if (written < 0 || (size_t)written >= sizeof(module_path)) {
            not_ready++;
            if (first_name[0] == '\0') {
                snprintf(first_name, sizeof(first_name), "%.200s", module->name);
                snprintf(first_detail, sizeof(first_detail),
                         "the module path is too long");
            }
            continue;
        }
        /* A user-supplied plain module always wins and is never overwritten. */
        existing = nk_fopen_utf8(module_path, "rb");
        if (existing != NULL) {
            fclose(existing);
            if (player_is_usable_mips_elf32(module_path, true)) {
                ready++;
                continue;
            }
            not_ready++;
            if (first_name[0] == '\0') {
                snprintf(first_name, sizeof(first_name), "%.200s", module->name);
                snprintf(first_detail, sizeof(first_detail),
                         "the supplied copy is not a usable plain module");
            }
            continue;
        }
        if (!module->encrypted) {
            ready++;
            continue;
        }
        detail[0] = '\0';
        status = player_try_builtin_decrypt_member(
            runtime_root, app->inspecting_game.iso_path, module->rel_path,
            decrypted_dir, module->name, module_path, detail, sizeof(detail),
            true);
        if (status == PLAYER_BOUNDARY_OK) {
            ready++;
            continue;
        }
        not_ready++;
        if (first_name[0] == '\0') {
            snprintf(first_name, sizeof(first_name), "%.200s", module->name);
            if (status == PLAYER_BOUNDARY_NO_KEYFILE) {
                first_encrypted = true;
            } else {
                first_boundary = true;
                snprintf(first_detail, sizeof(first_detail), "%s", detail);
            }
        }
    }
    if (not_ready == 0) {
        snprintf(message, sizeof(message),
                 "Guest modules: %u of %u ready.",
                 (unsigned)ready, (unsigned)total);
        player_preflight_add(preflight, "GUEST_MODULES", PREFLIGHT_OK, message,
                             NULL, 0);
        free(modules);
        return;
    }
    more[0] = '\0';
    if (not_ready > 1) {
        snprintf(more, sizeof(more), " (%u more not ready)",
                 (unsigned)(not_ready - 1));
    }
    {
        static const unsigned int issues[] = { 308 };
        if (first_encrypted) {
            snprintf(message, sizeof(message),
                     "Guest modules: %u of %u ready; %.200s is encrypted%s. "
                     "Supply decrypted modules at %.220s, or a local key "
                     "file at %.170s to enable the built-in boundary.",
                     (unsigned)ready, (unsigned)total, first_name, more,
                     decrypted_dir, key_path);
            player_preflight_add(preflight, "GUEST_MODULES", PREFLIGHT_MISSING,
                                 message, issues, 1);
        } else if (first_boundary) {
            snprintf(message, sizeof(message),
                     "Guest modules: %u of %u ready; %.200s could not be "
                     "decrypted (%.190s)%s. Supply decrypted modules at %.220s "
                     "or add the missing entry to your local key file.",
                     (unsigned)ready, (unsigned)total, first_name, first_detail,
                     more, decrypted_dir);
            player_preflight_add(preflight, "GUEST_MODULES", PREFLIGHT_UNSUPPORTED,
                                 message, issues, 1);
        } else {
            snprintf(message, sizeof(message),
                     "Guest modules: %u of %u ready; %.200s is not ready "
                     "(%.190s)%s. Supply decrypted modules at %.220s.",
                     (unsigned)ready, (unsigned)total, first_name, first_detail,
                     more, decrypted_dir);
            player_preflight_add(preflight, "GUEST_MODULES", PREFLIGHT_UNSUPPORTED,
                                 message, issues, 1);
        }
    }
    free(modules);
}

/* Append text to a bounded buffer, never past its end. */
static void player_font_append(char *out, size_t out_len, const char *text) {
    size_t used;
    size_t room;
    if (!out || out_len == 0 || !text) return;
    used = strlen(out);
    if (used >= out_len - 1u) return;
    room = out_len - 1u - used;
    strncpy(out + used, text, room);
    out[used + room] = '\0';
}

void player_app_refresh_font_status(PlayerApp *app) {
    NkFontSlotState states[NK_FONT_SLOT_COUNT];
    char root[NK_MAX_PATH];
    if (!app) return;
    /* The same root the preflight checks; empty when none resolves, and the nk_font calls then
       report that the cache location is unavailable. */
    if (!player_app_data_root(app, root, sizeof(root))) root[0] = '\0';
    nk_font_slot_states(root, root, states);
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        snprintf(app->wizard.font_slot_detail[slot], sizeof(app->wizard.font_slot_detail[slot]),
                 "%s", states[slot].detail);
    }
}

bool player_app_fonts_import_folder(PlayerApp *app, const char *folder) {
    NkFontImportResult result;
    char error[NK_FONT_DETAIL_MAX] = "";
    char root[NK_MAX_PATH];
    if (!app) return false;
    if (!folder || !*folder) {
        snprintf(app->wizard.font_message, sizeof(app->wizard.font_message), "No folder was chosen.");
        return false;
    }
    if (!player_app_data_root(app, root, sizeof(root))) root[0] = '\0';
    if (!nk_font_import_folder(root, folder, NULL, &result, error, sizeof(error))) {
        snprintf(app->wizard.font_message, sizeof(app->wizard.font_message),
                 "Import stopped: %s.", error);
        player_app_refresh_font_status(app);
        return false;
    }
    if (result.imported_count == 0) {
        /* Say why, from the first file that was not used: a refusal, or several files for one slot. */
        const char *reason = "no .pgf file names a PSP font slot";
        for (int i = 0; i < result.file_count; i++) {
            if (result.files[i].detail[0] != '\0' && strstr(result.files[i].detail, "not imported") != NULL) {
                reason = result.files[i].detail;
                break;
            }
            if (result.files[i].detail[0] != '\0' && strncmp(result.files[i].detail, "refused", 7) == 0) {
                reason = result.files[i].detail;
            }
        }
        snprintf(app->wizard.font_message, sizeof(app->wizard.font_message),
                 "No font was imported: %.400s.", reason);
    } else {
        snprintf(app->wizard.font_message, sizeof(app->wizard.font_message),
                 "Imported %d PSP font slot(s) into the player's font cache.", result.imported_count);
    }
    player_app_refresh_font_status(app);
    return result.imported_count > 0;
}

bool player_app_fonts_remove_imports(PlayerApp *app) {
    bool remove[NK_FONT_SLOT_COUNT];
    char error[NK_FONT_DETAIL_MAX] = "";
    char root[NK_MAX_PATH];
    int removed;
    if (!app) return false;
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) remove[slot] = true;
    if (!player_app_data_root(app, root, sizeof(root))) root[0] = '\0';
    removed = nk_font_remove_imports(root, remove, error, sizeof(error));
    if (removed < 0) {
        snprintf(app->wizard.font_message, sizeof(app->wizard.font_message),
                 "Could not remove the imported fonts: %s.", error);
    } else if (removed == 0) {
        snprintf(app->wizard.font_message, sizeof(app->wizard.font_message),
                 "No imported PSP font to remove.");
    } else {
        snprintf(app->wizard.font_message, sizeof(app->wizard.font_message),
                 "Removed %d imported PSP font file(s). Project fonts, if any, are unchanged.", removed);
    }
    player_app_refresh_font_status(app);
    return removed >= 0;
}

void player_app_build_compatibility_preflight(
    PlayerApp *app, bool disc_readable, bool param_sfo_parsed,
    const NkIsoExecutableReport *executables) {
    if (!app) return;
    PlayerCompatibilityPreflight *preflight = &app->wizard.preflight;
    memset(preflight, 0, sizeof(*preflight));
    /* The Fonts step resolves the same root through player_app_data_root. */
    char runtime_root[NK_MAX_PATH];
    if (!player_app_data_root(app, runtime_root, sizeof(runtime_root))) {
        snprintf(runtime_root, sizeof(runtime_root), "%s", ".");
    }

    if (app->inspecting_game.is_experimental) {
        static const unsigned int issues[] = { 308 };
        player_preflight_add(preflight, "EXPERIMENTAL", PREFLIGHT_IN_PROGRESS,
                             "Experimental: this game has not been verified. Compatibility is unknown. Verification for additional titles and generic title intake are in the works.",
                             issues, 1);
    }

    if (!disc_readable) {
        player_preflight_add(preflight, "DISC_SFO", PREFLIGHT_UNSUPPORTED,
                             "Disc metadata could not be read safely; PARAM.SFO and executables were not trusted.",
                             NULL, 0);
    } else if (param_sfo_parsed) {
        player_preflight_add(preflight, "DISC_SFO", PREFLIGHT_OK,
                             "Disc image is readable and PARAM.SFO was parsed.", NULL, 0);
    } else {
        player_preflight_add(preflight, "DISC_SFO", PREFLIGHT_MISSING,
                             "Disc image is readable, but PSP_GAME/PARAM.SFO is missing or could not be parsed.",
                             NULL, 0);
    }

    NkIsoExecutableKind eboot = executables ? executables->eboot.kind : NK_ISO_EXEC_UNKNOWN;
    bool boot_fallback = executables && executables->selected == NK_ISO_EXEC_SELECTION_BOOT &&
                         executables->boot_fallback;
    if (executables && executables->selected == NK_ISO_EXEC_SELECTION_EBOOT) {
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_OK,
                             "EBOOT.BIN is a plain MIPS ELF32 and selected for analysis.", NULL, 0);
    } else if (boot_fallback) {
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_OK,
                             "BOOT.BIN selected for analysis because EBOOT.BIN is encrypted.", NULL, 0);
    } else if (eboot == NK_ISO_EXEC_PSP_ENCRYPTED ||
               eboot == NK_ISO_EXEC_SCE_WRAPPER || eboot == NK_ISO_EXEC_PBP) {
        static const unsigned int issues[] = { 308 };
        char decrypted_dir[NK_MAX_PATH * 2];
        char decrypted_elf[NK_MAX_PATH * 2];
        PlayerDecryptedEbootState decrypted_state = player_find_decrypted_eboot(
            runtime_root, app->inspecting_game.disc_id, decrypted_dir,
            sizeof(decrypted_dir), decrypted_elf, sizeof(decrypted_elf));
        if (decrypted_state == PLAYER_DECRYPTED_EBOOT_VALID) {
            player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_OK,
                "User-supplied decrypted EBOOT.elf is a valid MIPS ELF32 and selected for analysis.",
                NULL, 0);
        } else if (decrypted_state == PLAYER_DECRYPTED_EBOOT_INVALID) {
            char message[512];
            snprintf(message, sizeof(message),
                "Encrypted executable: EBOOT.elf is not a usable MIPS ELF32; "
                "replace it with a valid decrypted EBOOT.elf at %.300s.",
                decrypted_dir);
            player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                                 message, NULL, 0);
        } else if (decrypted_state == PLAYER_DECRYPTED_EBOOT_MISSING) {
            char boundary_detail[320] = "";
            char key_path[NK_MAX_PATH + 64];
            const char *env_override = getenv("NAKAGAWA_PSP_KEY_FILE");
            PlayerBoundaryStatus boundary = player_try_builtin_decrypt_member(
                runtime_root, app->inspecting_game.iso_path,
                "PSP_GAME/SYSDIR/EBOOT.BIN", decrypted_dir, "EBOOT.elf",
                decrypted_elf, boundary_detail, sizeof(boundary_detail), false);
            if (env_override != NULL && env_override[0] != '\0') {
                snprintf(key_path, sizeof(key_path), "%s", env_override);
            } else {
                snprintf(key_path, sizeof(key_path),
                         "%s/keys/psp-keyfile.json", runtime_root);
            }
            if (boundary == PLAYER_BOUNDARY_OK) {
                player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_OK,
                    "The built-in decryption boundary produced a usable MIPS ELF32 and it is selected for analysis.",
                    NULL, 0);
            } else if (boundary == PLAYER_BOUNDARY_FAILED) {
                char message[512];
                snprintf(message, sizeof(message),
                    "Encrypted executable: the built-in decryption boundary failed "
                    "(%.190s); supply decrypted modules at %.190s.",
                    boundary_detail, decrypted_dir);
                player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                                     message, issues, 1);
            } else {
                char message[512];
                snprintf(message, sizeof(message),
                    "Encrypted executable: supply decrypted modules at %.220s, "
                    "or a local key file at %.170s to enable the built-in boundary.",
                    decrypted_dir, key_path);
                player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                                     message, issues, 1);
            }
        } else {
            static const unsigned int path_issues[] = { 308 };
            char message[512];
            snprintf(message, sizeof(message),
                "Encrypted executable: the per-title decrypted-data path could "
                "not be resolved safely. Correct the user-data path and retry; "
                "broader ISO-to-Play support is in the works.");
            player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                                 message, path_issues, 1);
        }
    } else if (eboot == NK_ISO_EXEC_EMPTY_OR_ZERO) {
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                             "EBOOT.BIN is empty or zero-filled and cannot be analyzed.", NULL, 0);
    } else {
        static const unsigned int issues[] = { 308 };
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                             "Unknown/malformed executable boundary; broader title support is in the works.",
                             issues, 1);
    }

    if (disc_readable) {
        player_check_guest_modules(app, preflight, runtime_root);
    }

    if (!app->inspecting_game.title_id[0]) {
        static const unsigned int issues[] = { 308 };
        player_preflight_add(preflight, "RUNTIME_PACKAGE", PREFLIGHT_UNSUPPORTED,
                             "Title profile missing; generic title support is in the works.",
                             issues, 1);
    } else {
        char package_reason[2048] = "";
        NkRuntimePackageStatus package_status = player_app_validate_runtime_package(
            app, &app->inspecting_game, NULL, package_reason,
            sizeof(package_reason));
        static const unsigned int issues[] = { 308 };
        PlayerPreflightStatus status = PREFLIGHT_MISSING;
        if (package_status == NK_RUNTIME_PACKAGE_OK) status = PREFLIGHT_OK;
        else if (package_status == NK_RUNTIME_PACKAGE_INCOMPATIBLE) status = PREFLIGHT_INCOMPATIBLE;
        else if (package_status == NK_RUNTIME_PACKAGE_STALE) status = PREFLIGHT_STALE;
        player_preflight_add(preflight, "RUNTIME_PACKAGE", status,
            package_reason[0] ? package_reason : "Runtime package validation did not complete.",
            status == PREFLIGHT_OK ? NULL : issues,
            status == PREFLIGHT_OK ? 0 : 1);
    }

    if (app->inspecting_game.title_id[0]) {
        char data_root_path[NK_MAX_PATH] = "";
        char data_root_reason[1024] = "";
        NkLaunchDataRootStatus data_root_status = player_app_game_data_root_status(
            app, &app->inspecting_game, data_root_path, sizeof(data_root_path),
            data_root_reason, sizeof(data_root_reason));
        static const unsigned int issues[] = { 308 };
        if (data_root_status == NK_LAUNCH_DATA_ROOT_READY ||
            data_root_status == NK_LAUNCH_DATA_ROOT_NOT_REQUIRED) {
            player_preflight_add(preflight, "DATA_ROOT", PREFLIGHT_OK,
                data_root_status == NK_LAUNCH_DATA_ROOT_NOT_REQUIRED
                    ? "This game does not need a separate data folder."
                    : "This game's data folder is available.",
                NULL, 0);
        } else {
            player_preflight_add(preflight, "DATA_ROOT",
                data_root_status == NK_LAUNCH_DATA_ROOT_MISSING
                    ? PREFLIGHT_MISSING : PREFLIGHT_UNSUPPORTED,
                data_root_reason[0] ? data_root_reason
                    : "This game's data folder could not be checked.",
                issues, 1);
        }
    }

    static const unsigned int font_issues[] = { 313 };
    char font_message[NK_FONT_TEXT_MAX] = "";
    NkFontStatus font_status = nk_font_check_cache(runtime_root, font_message, sizeof(font_message));
    if (font_status == NK_FONT_STATUS_INVALID) {
        player_preflight_add(preflight, "SYSTEM_FONTS", PREFLIGHT_INVALID,
                             font_message[0] ? font_message : "PSP font cache is invalid; run fonts import <folder>.",
                             font_issues, 1);
    } else {
        NkFontSlotState states[NK_FONT_SLOT_COUNT];
        char summary[2048] = "";
        bool every_slot = true;
        nk_font_slot_states(runtime_root, runtime_root, states);
        for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
            if (states[slot].source == NK_FONT_SOURCE_NONE) every_slot = false;
            player_font_append(summary, sizeof(summary), states[slot].detail);
            player_font_append(summary, sizeof(summary), " ");
        }
        player_preflight_add(preflight, "SYSTEM_FONTS",
                             every_slot ? PREFLIGHT_OK : PREFLIGHT_MISSING,
                             summary, font_issues, every_slot ? 0 : 1);
    }

    /* The public runtime drives the default device through SDL3 (#301). Whether a
       device exists is only known when the runtime starts; without one the game
       keeps running silently and says so once. */
    player_preflight_add(preflight, "AUDIO_OUTPUT", PREFLIGHT_OK,
                         "Sound plays through your default audio device. With no device, the game runs silently.",
                         NULL, 0);
}
