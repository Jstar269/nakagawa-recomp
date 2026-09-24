/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_state.h"
#include <stdio.h>
#include <string.h>

static const char *player_runtime_root(const PlayerApp *app) {
    return (app && app->runtime_root[0]) ? app->runtime_root : NULL;
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

    /* Default settings */
    app->settings.resolution_scale = 4; /* 1080p modern default */
    app->settings.fullscreen = false;
    app->settings.vsync = true;
    app->settings.fps_cap = 60;
    app->settings.master_volume = 80;
    app->settings.reduce_motion = false;
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
    const char *root = app && app->runtime_root[0] ? app->runtime_root : NULL;
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

void player_app_sync_library(PlayerApp *app) {
    if (!app) return;
    app->game_count = 0;
    for (int i = 0; i < app->library.count && i < MAX_LIBRARY_GAMES; i++) {
        app->games[i] = app->library.entries[i];
        app->game_count++;
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

int player_app_focus_count(const PlayerApp *app) {
    if (!app) return 1;
    switch (app->active_view) {
        case VIEW_LIBRARY:
        case PLAYER_VIEW_READY_LIBRARY:
            if (app->game_count <= 0) return 1;
            {
                /* Order matches render_loaded_library: primary action
                 * (PLAY/STOP only when actionable), add, remove, then the
                 * paging stops when the library overflows. The unavailable
                 * pill is never a stop. */
                int count = 0;
                const GameRecord *game = (app->selected_game_index >= 0 &&
                                          app->selected_game_index < app->game_count)
                    ? &app->games[app->selected_game_index]
                    : NULL;
                bool game_has_package = game &&
                    player_app_validate_runtime_package(app, game, NULL, NULL, 0) ==
                        NK_RUNTIME_PACKAGE_OK;
                if (game && (game_has_package || app->is_game_running)) count++;
                count += 2; /* add + remove */
                if (app->game_count > player_app_visible_library_cards(app)) count += 2;
                return count < 1 ? 1 : count;
            }
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
             * fullscreen, reduce-motion) + volume stepper (2) + close (1),
             * in draw order. */
            return 13;
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

static bool resolution_scale_valid(int scale) {
    return scale == 1 || scale == 2 || scale == 3 || scale == 4 || scale == 8;
}

void player_app_set_resolution_scale(PlayerApp *app, int scale) {
    if (!app || !resolution_scale_valid(scale)) return;
    app->settings.resolution_scale = scale;
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
}

static bool fps_cap_valid(int cap) {
    return cap == 30 || cap == 60 || cap == 0;
}

void player_app_set_fps_cap(PlayerApp *app, int cap) {
    if (!app || !fps_cap_valid(cap)) return;
    app->settings.fps_cap = cap;
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
}

void player_app_toggle_fullscreen(PlayerApp *app) {
    if (!app) return;
    app->settings.fullscreen = !app->settings.fullscreen;
}

void player_app_toggle_vsync(PlayerApp *app) {
    if (!app) return;
    app->settings.vsync = !app->settings.vsync;
}

void player_app_toggle_reduce_motion(PlayerApp *app) {
    if (!app) return;
    app->settings.reduce_motion = !app->settings.reduce_motion;
}

void player_app_adjust_volume(PlayerApp *app, int delta) {
    if (!app) return;
    int volume = app->settings.master_volume + delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    app->settings.master_volume = volume;
}

bool player_app_remove_game(PlayerApp *app, int game_index) {
    if (!app || game_index < 0 || game_index >= app->game_count) return false;
    const char *disc_id = app->games[game_index].disc_id;
    if (!disc_id || disc_id[0] == '\0') return false;
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
    disp.is_prepared = player_app_validate_runtime_package(app, &disp, NULL, NULL, 0) ==
                       NK_RUNTIME_PACKAGE_OK;
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
    NkRuntimePackageStatus package_status = player_app_validate_runtime_package(
        app, game, NULL, package_error, sizeof(package_error));
    if (package_status != NK_RUNTIME_PACKAGE_OK) {
        player_app_set_error(app, "RUNTIME_PACKAGE_NOT_READY", "Runtime Package Not Ready",
                             package_error[0] ? package_error :
                                 "Runtime package is missing or incompatible (#297).",
                             "Return to Library", VIEW_LIBRARY);
        return false;
    }

    printf("[PLAYER] Preparing launch session for %s (%s)...\n", game->disc_id, game->title_name);

    NkResult res = nk_launch_prepare_session(&app->launch_session, game, player_runtime_root(app));
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
    } else if (eboot == NK_ISO_EXEC_PSP_ENCRYPTED) {
        static const unsigned int issues[] = { 295 };
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                             "Encrypted executable. Decryption support is in the works (#295).",
                             issues, 1);
    } else if (eboot == NK_ISO_EXEC_EMPTY_OR_ZERO) {
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                             "EBOOT.BIN is empty or zero-filled and cannot be analyzed.", NULL, 0);
    } else if (eboot == NK_ISO_EXEC_SCE_WRAPPER) {
        static const unsigned int issues[] = { 295 };
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                             "~SCE wrapper is not analyzable; container support is in the works (#295).",
                             issues, 1);
    } else if (eboot == NK_ISO_EXEC_PBP) {
        static const unsigned int issues[] = { 295 };
        player_preflight_add(preflight, "EXECUTABLE", PREFLIGHT_UNSUPPORTED,
                             "PBP is not plain ELF; executable unpacking is in the works (#295).",
                             issues, 1);
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

    char font_path[NK_MAX_PATH * 2];
    int written = snprintf(font_path, sizeof(font_path), "%s%cfont%cjpn0.pgf",
                           runtime_root, nk_platform_path_separator(),
                           nk_platform_path_separator());
    if (written > 0 && (size_t)written < sizeof(font_path) &&
        nk_platform_file_exists(font_path)) {
        player_preflight_add(preflight, "SYSTEM_FONTS", PREFLIGHT_OK,
                             "User-supplied PSP system font jpn0.pgf is available.", NULL, 0);
    } else {
        static const unsigned int issues[] = { 300 };
        player_preflight_add(preflight, "SYSTEM_FONTS", PREFLIGHT_MISSING,
                             "PSP font jpn0.pgf missing; provisioning is in the works (#300).",
                             issues, 1);
    }

    /* The public runtime drives the default device through SDL3 (#301). Whether a
       device exists is only known when the runtime starts; without one the game
       keeps running silently and says so once. */
    player_preflight_add(preflight, "AUDIO_OUTPUT", PREFLIGHT_OK,
                         "Sound plays through your default audio device. With no device, the game runs silently.",
                         NULL, 0);
}
