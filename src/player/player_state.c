/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_state.h"
#include <stdio.h>
#include <string.h>

void player_app_init(PlayerApp *app) {
    if (!app) return;
    memset(app, 0, sizeof(*app));
    app->active_view = VIEW_LIBRARY;
    app->selected_game_index = -1;
    app->window_width = 1280;
    app->window_height = 720;
    app->dpi_scale = 1.0f;
    app->should_quit = false;

    /* Default settings */
    app->settings.resolution_scale = 4; /* 1080p modern default */
    app->settings.fullscreen = false;
    app->settings.vsync = true;
    app->settings.fps_cap = 60;
    app->settings.master_volume = 80;
    /* Controller state: no fabrication. The event loop in src/player/main.c
       fills these in when SDL reports a gamepad, and clears them when it goes. */
    app->settings.controller_name[0] = '\0';
    app->settings.controller_connected = false;
    snprintf(app->settings.save_directory, sizeof(app->settings.save_directory), "savedata");

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
}

void player_app_sync_library(PlayerApp *app) {
    if (!app) return;
    app->game_count = 0;
    for (int i = 0; i < app->library.count && i < MAX_LIBRARY_GAMES; i++) {
        app->games[i] = app->library.entries[i];
        app->game_count++;
    }
    if (app->game_count > 0 && app->selected_game_index < 0) {
        app->selected_game_index = 0;
    }
}

bool player_app_add_game(PlayerApp *app, const GameRecord *game) {
    if (!app || !game || game->disc_id[0] == '\0') return false;

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
       when it had never once been reached. Marked prepared for the same reason
       the launch is real: the build output either exists or the launch fails
       closed and says so. */
    GameRecord disp;
    memset(&disp, 0, sizeof(disp));
    snprintf(disp.disc_id, sizeof(disp.disc_id), "TEST00006");
    snprintf(disp.title_name, sizeof(disp.title_name), "Nakagawa Display Smoke Fixture");
    snprintf(disp.disc_version, sizeof(disp.disc_version), "1.00");
    snprintf(disp.iso_path, sizeof(disp.iso_path), "fixtures/display_smoke/generate.py");
    snprintf(disp.prepared_root, sizeof(disp.prepared_root), "fixtures/display_smoke");
    snprintf(disp.title_id, sizeof(disp.title_id), "display-smoke-v1");
    disp.iso_size_bytes = 0ULL;
    disp.status = NK_STATUS_IDENTIFIED;
    disp.is_prepared = false;
    snprintf(disp.last_played, sizeof(disp.last_played), "Never");

    if (nk_library_add_or_update(&app->library, &disp) == NK_OK) {
        player_app_sync_library(app);
    }
}

bool player_app_launch_game(PlayerApp *app, int game_index) {
    if (!app || game_index < 0 || game_index >= app->game_count) return false;
    const GameRecord *game = &app->games[game_index];

    printf("[PLAYER] Preparing launch session for %s (%s)...\n", game->disc_id, game->title_name);

    NkResult res = nk_launch_prepare_session(&app->launch_session, game, ".");
    if (res != NK_OK) {
        printf("[PLAYER] Launch preparation failed: %s\n", app->launch_session.last_error);
        const char *err_code = "RUNTIME_NOT_FOUND";
        const char *err_title = "Recompiled Binary Not Available";
        if (strstr(app->launch_session.last_error, "manifest") != NULL ||
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
    printf("[PLAYER] Game started successfully (PID: %d)!\n", app->launch_session.process.process_id);
    return true;
}

void player_app_stop_game(PlayerApp *app) {
    if (!app || !app->is_game_running) return;
    printf("[PLAYER] Stopping active game session...\n");
    nk_launch_stop(&app->launch_session);
    app->is_game_running = false;
}
