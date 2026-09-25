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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

static const char *player_runtime_root(const PlayerApp *app) {
    return (app && app->runtime_root[0]) ? app->runtime_root : NULL;
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
        return;
    }
    snprintf(app->runtime_root, sizeof(app->runtime_root), "%s", root);
}

NkRuntimePackageStatus player_app_validate_runtime_package(
    const PlayerApp *app,
    const GameRecord *game,
    NkRuntimePackageInfo *out_info,
    char *reason,
    size_t reason_size
) {
    char default_root[NK_MAX_PATH];
    const char *root = player_package_root(app, game);
    if (!root) {
        if (!nk_platform_get_app_data_dir(default_root, sizeof(default_root))) {
            if (reason && reason_size) snprintf(reason, reason_size,
                "Per-user data directory is unavailable; package discovery cannot run.");
            return NK_RUNTIME_PACKAGE_MISSING;
        }
        root = default_root;
    }
    return nk_launch_validate_runtime_package(root, game, out_info, reason,
                                              reason_size);
}

bool player_app_game_has_runtime(const PlayerApp *app, const GameRecord *game) {
    if (!game) return false;
    NkRuntimePackageStatus status = player_app_validate_runtime_package(
        app, game, NULL, NULL, 0);
    if (status == NK_RUNTIME_PACKAGE_OK) {
        return true;
    }
    if (status == NK_RUNTIME_PACKAGE_MISSING && !game->is_experimental &&
        nk_launch_runtime_available(player_package_root(app, game), game->title_id)) {
        return true;
    }
    return false;
}

void player_app_sync_library(PlayerApp *app) {
    if (!app) return;
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
        game.is_prepared = nk_launch_validate_runtime_package(
            app->showcase_root, &game, NULL, NULL, 0) == NK_RUNTIME_PACKAGE_OK;
        if (!game.is_prepared) continue;
        game.status = NK_STATUS_PREPARED;
        snprintf(game.last_played, sizeof(game.last_played), "Never");
        app->showcase_games[app->showcase_count++] = game;
    }
    player_app_sync_library(app);
    return app->showcase_count > 0;
}

bool player_app_add_game(PlayerApp *app, const GameRecord *game) {
    if (!app || !game || game->disc_id[0] == '\0') return false;

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
                 * (PLAY/STOP/BUILD/REBUILD when actionable), add, remove, then the
                 * paging stops when the library overflows. The unavailable
                 * pill is never a stop. */
                int count = 0;
                const GameRecord *game = (app->selected_game_index >= 0 &&
                                          app->selected_game_index < app->game_count)
                    ? &app->games[app->selected_game_index]
                    : NULL;
                NkRuntimePackageStatus pkg_status = game ?
                    player_app_validate_runtime_package(app, game, NULL, NULL, 0) :
                    NK_RUNTIME_PACKAGE_MISSING;
                bool game_ready = game && player_app_game_has_runtime(app, game);
                bool can_build = game && !game_ready &&
                                 (pkg_status == NK_RUNTIME_PACKAGE_MISSING ||
                                  pkg_status == NK_RUNTIME_PACKAGE_STALE);
                if (game && (game_ready || app->is_game_running || can_build)) count++;
                count += player_game_is_showcase(game) ? 1 : 2; /* add + optional remove */
                if (app->game_count > player_app_visible_library_cards(app)) count += 2;
                return count < 1 ? 1 : count;
            }
        case VIEW_BUILDING_PACKAGE:
            return 1;
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
            /* Resolution (4) + frame cap (3) + display toggles (3: vsync,
             * fullscreen, reduce-motion) + volume stepper (2) + controller settings (1) + close (1),
             * in draw order. */
            return 14;
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
             * in draw order. */
            return 22;
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
                    return 4;
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
    settings->resolution_scale = 4; /* 1080p modern default */
    settings->fullscreen = false;
    settings->vsync = true;
    settings->fps_cap = 60;
    settings->master_volume = 80;
    settings->reduce_motion = false;
    settings->controller_name[0] = '\0';
    settings->controller_connected = false;
    snprintf(settings->save_directory, sizeof(settings->save_directory), "savedata");
}

static bool resolution_scale_valid(int scale) {
    return scale == 1 || scale == 2 || scale == 3 || scale == 4 || scale == 8;
}

/* Settings paths come from the per-user config directory, which is UTF-8 and
 * may contain non-ASCII characters; the narrow CRT fopen would misread them on
 * Windows, so open through the wide API there. */
static FILE *settings_fopen(const char *path, const char *mode) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wpath[32768];
    WCHAR wmode[16];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, 32768) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, wmode, 16) <= 0) {
        return NULL;
    }
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

static bool fps_cap_valid(int cap) {
    return cap == 30 || cap == 60 || cap == 0;
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

    FILE *f = settings_fopen(target, "rb");
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
    if (!sv_node || !nk_json_get_int64(sv_node, &sv) || sv != NK_PLAYER_SETTINGS_SCHEMA_VERSION) {
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
    if (fps_node && nk_json_get_int64(fps_node, &fps_val) && fps_cap_valid((int)fps_val)) {
        app->settings.fps_cap = (int)fps_val;
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

    FILE *f = settings_fopen(tmp_path, "wb");
    if (!f) return NK_ERROR_IO;

    fprintf(f, "{\n");
    fprintf(f, "  \"schema_version\": %d,\n", NK_PLAYER_SETTINGS_SCHEMA_VERSION);
    fprintf(f, "  \"resolution_scale\": %d,\n", app->settings.resolution_scale);
    fprintf(f, "  \"fps_cap\": %d,\n", app->settings.fps_cap);
    fprintf(f, "  \"vsync\": %s,\n", app->settings.vsync ? "true" : "false");
    fprintf(f, "  \"fullscreen\": %s,\n", app->settings.fullscreen ? "true" : "false");
    fprintf(f, "  \"reduce_motion\": %s,\n", app->settings.reduce_motion ? "true" : "false");
    fprintf(f, "  \"master_volume\": %d\n", app->settings.master_volume);
    fprintf(f, "}\n");

    if (fflush(f) != 0) {
        fclose(f);
        remove(tmp_path);
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
        DeleteFileA(tmp_path);
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
    /* UI offers 1/2/4/8. Scale 3 stays accepted for forward compatibility
     * (resolution_label knows it) but is skipped by the stepper. */
    static const int kOrder[] = { 1, 2, 4, 8 };
    int current = app->settings.resolution_scale;
    int at = 0;
    for (int i = 0; i < 4; i++) {
        if (kOrder[i] == current) {
            at = i;
            break;
        }
        if (kOrder[i] < current) at = i;
    }
    if (direction < 0) {
        at = (at + 3) % 4;
    } else {
        at = (at + 1) % 4;
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
    static const int kOrder[] = { 30, 60, 0 };
    int current = app->settings.fps_cap;
    int at = 1;
    for (int i = 0; i < 3; i++) {
        if (kOrder[i] == current) {
            at = i;
            break;
        }
    }
    if (direction < 0) {
        at = (at + 2) % 3;
    } else {
        at = (at + 1) % 3;
    }
    app->settings.fps_cap = kOrder[at];
    maybe_persist_settings(app);
}

void player_app_toggle_fullscreen(PlayerApp *app) {
    if (!app) return;
    app->settings.fullscreen = !app->settings.fullscreen;
    maybe_persist_settings(app);
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
    snprintf(app->last_error.recovery_action_label, sizeof(app->last_error.recovery_action_label), "%s", recovery_label ? recovery_label : "Return to Library");
    app->last_error.return_view = return_view;
    app->last_error.failed_stage[0] = '\0';
    app->last_error.boundary_text[0] = '\0';
    app->last_error.log_file_path[0] = '\0';
    app->active_view = VIEW_ERROR;
}

void player_app_populate_sample_games(PlayerApp *app) {
    if (!app) return;
    if (app->game_count > 0) return; /* Don't overwrite loaded library */

    /* Sample fixture for UI demonstration: marks as IDENTIFIED but NOT verified or
     * prepared. No verification or preparation pipeline has run in this build, so
     * the status must not claim otherwise. */
    GameRecord p5;
    memset(&p5, 0, sizeof(p5));
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

    /* The display fixture is the only public title whose runtime is BUILT under
       the layout nk_launch.c resolves (build/<title_id>/<title_id>), so it is the
       one demo entry whose PLAY NOW can actually start a runtime -- after
       `mingw32-make display-smoke`. Without it the demo library shows only titles
       that cannot launch, which is what made the launch path look implemented
       when it had never once been reached.

       is_prepared is PROBED rather than asserted. Claiming prepared when the
       runtime has not been built would put PLAY NOW in front of a launch that
       cannot work; claiming unprepared when it HAS been built sends the user to
       the preparation view, which in this build only says no pipeline is
       connected. The probe uses the launcher's own candidate search, so the card
       and the launch cannot disagree. */
    GameRecord disp;
    memset(&disp, 0, sizeof(disp));
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

bool player_app_launch_game(PlayerApp *app, int game_index) {
    if (!app || game_index < 0 || game_index >= app->game_count) return false;
    const GameRecord *game = &app->games[game_index];
    /* A launch initiated by the player always requests a GUI child, even if
       package preflight rejects it before launch-session preparation. */
    app->launch_session.config.gui_mode = true;

    char package_error[2048] = "";
    player_app_validate_runtime_package(
        app, game, NULL, package_error, sizeof(package_error));
    if (!player_app_game_has_runtime(app, game)) {
        player_app_set_error(app, "RUNTIME_PACKAGE_NOT_READY", "Runtime Package Not Ready",
                             package_error[0] ? package_error :
                                 "Runtime package is missing or incompatible; build it from the library (#297).",
                             "Return to Library", VIEW_LIBRARY);
        return false;
    }

    printf("[PLAYER] Preparing launch session for %s (%s)...\n", game->disc_id, game->title_name);

    NkResult res = nk_launch_prepare_session(&app->launch_session, game,
                                              player_package_root(app, game));
    /* nk_launch defaults gui_mode to false for headless harnesses. */
    app->launch_session.config.gui_mode = true;
    if (res != NK_OK) {
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
    app->launch_session.config.resolution_scale = app->settings.resolution_scale;
    app->launch_session.config.fps_cap = app->settings.fps_cap;
    app->launch_session.config.vsync = app->settings.vsync;
    app->launch_session.config.fullscreen = app->settings.fullscreen;

    printf("[PLAYER] Spawning runtime: %s (ISO: %s)\n", app->launch_session.executable_path, app->launch_session.iso_path);

    NkResult start_res = nk_launch_start(&app->launch_session);
    if (start_res != NK_OK) {
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
        if (app->library.library_path[0]) {
            nk_library_add_or_update(&app->library, &app->games[game_index]);
            nk_library_save(&app->library, app->library.library_path);
        }
    }
    printf("[PLAYER] Game started successfully (PID: %d)!\n", app->launch_session.process.process_id);
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
    if (!app || !app->is_game_running) return;
    printf("[PLAYER] Stopping active game session...\n");
    nk_launch_stop(&app->launch_session);
    app->is_game_running = false;
}

bool player_app_start_package_build(PlayerApp *app, int game_index) {
    if (!app || game_index < 0 || game_index >= app->game_count) return false;
    const GameRecord *game = &app->games[game_index];

    package_builder_init_session(&app->build_session, game->disc_id, game->title_name);

    char python_path[NK_MAX_PATH];
    if (!package_builder_find_python(python_path, sizeof(python_path))) {
        player_app_set_error(app, "PYTHON_NOT_FOUND", "Python 3 Interpreter Not Found",
                             "Python 3.14 was not found on PATH or in the MSYS2 toolchain.\n"
                             "Install it (see docs/SETUP.md) or set the PYTHON environment variable.",
                             "Return to Library", VIEW_LIBRARY);
        return false;
    }

    char cli_path[NK_MAX_PATH];
    if (!package_builder_find_cli(app->install_root, cli_path, sizeof(cli_path))) {
        player_app_set_error(app, "CLI_NOT_FOUND", "Nakagawa CLI Not Found",
                             "tools/nk_cli.py could not be located in the current workspace or install root.",
                             "Return to Library", VIEW_LIBRARY);
        return false;
    }

    /* Build into the same per-user root that package validation reads. */
    char user_data_root[NK_MAX_PATH];
    if (app->runtime_root[0]) {
        snprintf(user_data_root, sizeof(user_data_root), "%s", app->runtime_root);
    } else if (!nk_platform_get_app_data_dir(user_data_root, sizeof(user_data_root))) {
        player_app_set_error(app, "DATA_DIR_UNAVAILABLE", "Per-User Data Unavailable",
                             "The per-user data directory is unavailable, so there is nowhere to build the package.",
                             "Return to Library", VIEW_LIBRARY);
        return false;
    }
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
                app->wizard.is_extracting = true;
                app->wizard.extraction_requested = true;
                app->wizard.extraction_cancel_requested = false;
                app->wizard.extraction_failed = false;
                app->wizard.extraction_result = NK_OK;
                app->wizard.extraction_percent = 0;
                app->wizard.files_extracted = 0;
                app->wizard.total_files = 0;
                app->wizard.extraction_current_file[0] = '\0';
                app->wizard.extraction_error[0] = '\0';
                snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                         "Extracting game assets into local application data...");
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

static uint16_t player_read_le16(const unsigned char *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

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

static bool player_is_usable_mips_elf32(const char *path) {
    unsigned char header[52];
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size < (long)sizeof(header) || file_size > 512L * 1024L * 1024L ||
        fseek(file, 0, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return false;
    }
    uint16_t e_type = player_read_le16(header + 16);
    uint16_t machine = player_read_le16(header + 18);
    uint32_t version = player_read_le32(header + 20);
    uint32_t entry = player_read_le32(header + 24);
    uint32_t phoff = player_read_le32(header + 28);
    uint32_t shoff = player_read_le32(header + 32);
    uint16_t ehsize = player_read_le16(header + 40);
    uint16_t phentsize = player_read_le16(header + 42);
    uint16_t phnum = player_read_le16(header + 44);
    uint16_t shentsize = player_read_le16(header + 46);
    uint16_t shnum = player_read_le16(header + 48);
    bool valid = memcmp(header, "\x7f" "ELF", 4) == 0 &&
                 header[4] == 1 && header[5] == 1 && header[6] == 1 &&
                 (e_type == 1 || e_type == 2 || e_type == 3 || e_type == 0xffa0) &&
                 machine == 8 && version == 1 && ehsize == sizeof(header) &&
                 phentsize == 32 && phnum >= 1 && phnum <= 128 &&
                 phoff >= ehsize &&
                 (uint64_t)phoff + (uint64_t)phentsize * phnum <= (uint64_t)file_size;
    if (valid && shnum != 0) {
        valid = shentsize == 40 && shoff >= ehsize &&
                (uint64_t)shoff + (uint64_t)shentsize * shnum <= (uint64_t)file_size;
    } else if (valid && shoff != 0) {
        valid = false;
    }

    bool have_load = false;
    bool entry_executable = false;
    for (uint16_t i = 0; valid && i < phnum; i++) {
        unsigned char ph[32];
        uint64_t offset = (uint64_t)phoff + (uint64_t)i * phentsize;
        if (fseek(file, (long)offset, SEEK_SET) != 0 ||
            fread(ph, 1, sizeof(ph), file) != sizeof(ph)) {
            valid = false;
            break;
        }
        uint32_t type = player_read_le32(ph);
        uint32_t p_offset = player_read_le32(ph + 4);
        uint32_t vaddr = player_read_le32(ph + 8);
        uint32_t filesz = player_read_le32(ph + 16);
        uint32_t memsz = player_read_le32(ph + 20);
        uint32_t flags = player_read_le32(ph + 24);
        uint32_t align = player_read_le32(ph + 28);
        uint64_t memory_end = (uint64_t)vaddr + memsz;
        if ((uint64_t)p_offset + filesz > (uint64_t)file_size) {
            valid = false;
            break;
        }
        if (type != 1) continue;
        if (memsz < filesz || memory_end > 0x100000000ULL ||
            (align > 1 && ((align & (align - 1u)) != 0 ||
                           p_offset % align != vaddr % align))) {
            valid = false;
            break;
        }
        have_load = true;
        if ((flags & 1u) != 0 && vaddr <= entry && (uint64_t)entry < memory_end) {
            entry_executable = true;
        }
    }
    fclose(file);
    return valid && have_load && entry_executable;
}

static PlayerDecryptedEbootState player_find_decrypted_eboot(
    const char *runtime_root, const char *disc_id, char *directory,
    size_t directory_size, char *elf_path, size_t elf_path_size) {
    if (!player_decrypted_eboot_paths(runtime_root, disc_id, directory,
                                      directory_size, elf_path, elf_path_size)) {
        return PLAYER_DECRYPTED_EBOOT_PATH_INVALID;
    }
    FILE *file = fopen(elf_path, "rb");
    if (!file) return PLAYER_DECRYPTED_EBOOT_MISSING;
    fclose(file);
    return player_is_usable_mips_elf32(elf_path)
        ? PLAYER_DECRYPTED_EBOOT_VALID : PLAYER_DECRYPTED_EBOOT_INVALID;
}

void player_app_build_compatibility_preflight(
    PlayerApp *app, bool disc_readable, bool param_sfo_parsed,
    const NkIsoExecutableReport *executables) {
    if (!app) return;
    PlayerCompatibilityPreflight *preflight = &app->wizard.preflight;
    memset(preflight, 0, sizeof(*preflight));
    char default_runtime_root[NK_MAX_PATH];
    const char *runtime_root = app->runtime_root[0] ? app->runtime_root : NULL;
    if (!runtime_root && nk_platform_get_app_data_dir(default_runtime_root,
                                                      sizeof(default_runtime_root))) {
        runtime_root = default_runtime_root;
    }
    if (!runtime_root) runtime_root = ".";

    if (app->inspecting_game.is_experimental) {
        static const unsigned int issues[] = { 285, 308 };
        player_preflight_add(preflight, "EXPERIMENTAL", PREFLIGHT_IN_PROGRESS,
                             "Experimental: this game has not been verified. Compatibility is unknown. Second-title verification is in the works (#285); generic title intake is in the works (#308).",
                             issues, 2);
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
        static const unsigned int issues[] = { 295 };
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
                "supply decrypted modules at %.300s (#295). Automatic decryption is in the works.",
                decrypted_dir);
            player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                                 message, issues, 1);
        } else if (decrypted_state == PLAYER_DECRYPTED_EBOOT_MISSING) {
            char message[512];
            snprintf(message, sizeof(message),
                "Encrypted executable: supply decrypted modules at %.360s (#295). "
                "Automatic decryption is in the works.", decrypted_dir);
            player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                                 message, issues, 1);
        } else {
            player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                "Encrypted executable. Decryption support is in the works (#295).",
                issues, 1);
        }
    } else if (eboot == NK_ISO_EXEC_EMPTY_OR_ZERO) {
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                             "EBOOT.BIN is empty or zero-filled and cannot be analyzed.", NULL, 0);
    } else {
        static const unsigned int issues[] = { 308 };
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                             "Unknown/malformed executable boundary; broader title support is in the works (#308).",
                             issues, 1);
    }

    if (!app->inspecting_game.title_id[0]) {
        static const unsigned int issues[] = { 308 };
        player_preflight_add(preflight, "RUNTIME_PACKAGE", PREFLIGHT_UNSUPPORTED,
                             "Title profile missing; generic title support is in the works (#308).",
                             issues, 1);
    } else {
        char package_reason[2048] = "";
        NkRuntimePackageStatus package_status = player_app_validate_runtime_package(
            app, &app->inspecting_game, NULL, package_reason,
            sizeof(package_reason));
        static const unsigned int issues[] = { 296, 297 };
        PlayerPreflightStatus status = PREFLIGHT_MISSING;
        if (package_status == NK_RUNTIME_PACKAGE_OK) status = PREFLIGHT_OK;
        else if (package_status == NK_RUNTIME_PACKAGE_INCOMPATIBLE) status = PREFLIGHT_INCOMPATIBLE;
        else if (package_status == NK_RUNTIME_PACKAGE_STALE) status = PREFLIGHT_STALE;
        player_preflight_add(preflight, "RUNTIME_PACKAGE", status,
            package_reason[0] ? package_reason : "Runtime package validation did not complete.",
            status == PREFLIGHT_OK ? NULL : issues,
            status == PREFLIGHT_OK ? 0 : 2);
    }

    static const unsigned int font_issues[] = { 300 };
    char font_message[512] = "";
    NkFontStatus font_status = nk_font_check_cache(runtime_root, runtime_root,
                                                   font_message, sizeof(font_message));
    if (font_status == NK_FONT_STATUS_OK) {
        player_preflight_add(preflight, "SYSTEM_FONTS", PREFLIGHT_OK,
                             font_message[0] ? font_message : "User-supplied PSP system font jpn0.pgf is available.",
                             NULL, 0);
    } else if (font_status == NK_FONT_STATUS_INVALID) {
        player_preflight_add(preflight, "SYSTEM_FONTS", PREFLIGHT_INVALID,
                             font_message[0] ? font_message : "PSP font cache is invalid; run fonts import <folder> (#300).",
                             font_issues, 1);
    } else {
        player_preflight_add(preflight, "SYSTEM_FONTS", PREFLIGHT_MISSING,
                             font_message[0] ? font_message : "PSP font jpn0.pgf missing; run fonts import <folder> (#300).",
                             font_issues, 1);
    }

    /* The public runtime drives the default device through SDL3 (#301). Whether a
       device exists is only known when the runtime starts; without one the game
       keeps running silently and says so once. */
    player_preflight_add(preflight, "AUDIO_OUTPUT", PREFLIGHT_OK,
                         "Sound plays through your default audio device. With no device, the game runs silently.",
                         NULL, 0);
}
