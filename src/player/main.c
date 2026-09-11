/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_state.h"
#include "iso_reader.h"
#include "ui_renderer.h"
#include "nk_title_manifest.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <shellapi.h>
#endif

static void SDLCALL on_file_dialog_callback(void *userdata, const char * const *filelist, int filter) {
    (void)filter;
    PlayerApp *app = (PlayerApp *)userdata;
    if (!app || !filelist || !filelist[0]) {
        return;
    }
    const char *selected_path = filelist[0];
    printf("[PLAYER] ISO Selected: %s\n", selected_path);

    IsoInspectResult res;
    if (iso_inspect_file(selected_path, &res)) {
        snprintf(app->inspecting_game.disc_id, sizeof(app->inspecting_game.disc_id), "%s", res.disc_id);
        snprintf(app->inspecting_game.title_name, sizeof(app->inspecting_game.title_name), "%s", res.title_name);
        snprintf(app->inspecting_game.disc_version, sizeof(app->inspecting_game.disc_version), "%s", res.disc_version);
        snprintf(app->inspecting_game.iso_path, sizeof(app->inspecting_game.iso_path), "%s", selected_path);
        app->inspecting_game.iso_size_bytes = res.file_size;
        app->inspecting_game.status = (NkGameSupportStatus)res.status;
        app->inspecting_game.is_prepared = false;

        if (res.is_supported) {
            player_app_set_view(app, VIEW_SUPPORTED_TITLE);
        } else {
            player_app_set_view(app, VIEW_UNSUPPORTED_TITLE);
        }
    } else {
        player_app_set_error(app, "ISO_CORRUPT", "Unreadable PSP Disc Image",
                             res.error_message[0] ? res.error_message : "The selected file is not a valid ISO9660 disc image.",
                             "Try Another File", VIEW_LIBRARY);
    }
}

static void trigger_file_picker(SDL_Window *window, PlayerApp *app) {
    SDL_DialogFileFilter filters[] = {
        { "PSP Disc Images (*.iso)", "iso" },
        { "All Files (*.*)", "*" }
    };
    SDL_ShowOpenFileDialog(on_file_dialog_callback, app, window, filters, 2, NULL, false);
}

int main(int argc, char *argv[]) {
#if defined(_WIN32) || defined(_WIN64)
    SetConsoleOutputCP(CP_UTF8);
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char **u8_argv = (char **)malloc((size_t)wargc * sizeof(char *));
    for (int i = 0; i < wargc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        u8_argv[i] = (char *)malloc((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u8_argv[i], len, NULL, NULL);
    }
    argc = wargc;
    argv = u8_argv;
#endif

    PlayerApp app;
    player_app_init(&app);

    const char *screenshot_path = NULL;
    const char *test_view = NULL;
    const char *manifest_overlay_path = NULL;
    const char *initial_iso_path = NULL;
    const char *runtime_bin_path = NULL;
    bool launch_now = false;
    int override_w = 1280;
    int override_h = 720;
    /* Demo fixtures are opt-in. Defaulting them on meant a normal first
       launch with an empty library populated TEST00005 and, through
       player_app_add_game, PERSISTED it: a new user's first sight of the
       program was a game they do not have, written into their real library
       file, instead of the documented empty-library prompt. */
    bool populate_sample = false;
    bool force_empty = false;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--screenshot=", 13) == 0) {
            screenshot_path = argv[i] + 13;
        } else if (strncmp(argv[i], "--view=", 7) == 0) {
            test_view = argv[i] + 7;
        } else if (strncmp(argv[i], "--manifest-overlay=", 19) == 0) {
            manifest_overlay_path = argv[i] + 19;
        } else if (strncmp(argv[i], "--iso=", 6) == 0) {
            initial_iso_path = argv[i] + 6;
        } else if (strncmp(argv[i], "--runtime-bin=", 14) == 0) {
            runtime_bin_path = argv[i] + 14;
        } else if (strcmp(argv[i], "--launch-now") == 0) {
            launch_now = true;
        } else if (strncmp(argv[i], "--width=", 8) == 0) {
            override_w = atoi(argv[i] + 8);
        } else if (strncmp(argv[i], "--height=", 9) == 0) {
            override_h = atoi(argv[i] + 9);
        } else if (strcmp(argv[i], "--demo") == 0) {
            populate_sample = true;
        } else if (strcmp(argv[i], "--empty") == 0) {
            force_empty = true;
        }
    }

    /* A --view= run is an explicit test or capture invocation, so it gets the
       fixture too. --empty wins regardless of argument order. */
    if (test_view && strcmp(test_view, "empty") != 0) {
        populate_sample = true;
    }
    if (force_empty) {
        populate_sample = false;
    }

    /* Load external manifest overlay if requested */
    if (manifest_overlay_path) {
        char err_msg[256];
        if (nk_title_manifest_load_overlay(manifest_overlay_path, err_msg, sizeof(err_msg))) {
            printf("[PLAYER] External manifest overlay loaded successfully: %s\n", manifest_overlay_path);
        } else {
            fprintf(stderr, "[PLAYER] Failed to load external manifest overlay: %s\n", err_msg);
            return 1;
        }
    }

    /* Inspect initial ISO if requested */
    if (initial_iso_path) {
        populate_sample = false;
        IsoInspectResult res;
        if (iso_inspect_file(initial_iso_path, &res)) {
            printf("[PLAYER] Inspected ISO %s -> Disc ID: %s, Title: %s, Supported: %d\n",
                   initial_iso_path, res.disc_id, res.title_name, res.is_supported ? 1 : 0);
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "%s", res.disc_id);
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name), "%s", res.title_name);
            snprintf(app.inspecting_game.disc_version, sizeof(app.inspecting_game.disc_version), "%s", res.disc_version);
            snprintf(app.inspecting_game.iso_path, sizeof(app.inspecting_game.iso_path), "%s", initial_iso_path);
            if (res.matched_title_id[0]) {
                snprintf(app.inspecting_game.title_id, sizeof(app.inspecting_game.title_id), "%s", res.matched_title_id);
            }
            app.inspecting_game.iso_size_bytes = res.file_size;
            app.inspecting_game.status = (NkGameSupportStatus)res.status;
            /* is_prepared intentionally NOT set: no preparation has run in this
             * build; the entry keeps the inspection-reported status. */

            /* Persist only a qualified title. The interactive flow offers ADD
               TO LIBRARY solely on the supported-title screen, so opening an
               unsupported image through --iso or a file association used to
               add it to the library permanently while the very next view said
               the title was not qualified. */
            bool stored = res.is_supported && player_app_add_game(&app, &app.inspecting_game);

            if (res.is_supported && !stored) {
                fprintf(stderr, "[PLAYER] Could not store %s in the library.\n",
                        app.inspecting_game.disc_id);
                player_app_set_error(&app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                     "The title could not be stored. The library may be full, "
                                     "or the user data directory is not writable.",
                                     "Return to Library", VIEW_LIBRARY);
            } else if (stored) {
                player_app_set_view(&app, VIEW_SUPPORTED_TITLE);
                if (launch_now) {
                    printf("[PLAYER] Launching supported title now...\n");
                    const char *target_root = runtime_bin_path ? runtime_bin_path : ".";
                    /* nk_library_add_or_update updates an existing record in
                       place rather than appending it, so in a library holding
                       several games the last entry is a DIFFERENT title
                       whenever this disc was already known. Taking
                       app.game_count - 1 therefore prepared and launched some
                       other game while the console said it was launching the
                       requested ISO. Find the entry by its inspected disc ID. */
                    int game_idx = player_app_find_game_by_disc_id(&app, app.inspecting_game.disc_id);
                    NkResult lres;
                    if (game_idx < 0) {
                        memset(&app.launch_session, 0, sizeof(app.launch_session));
                        snprintf(app.launch_session.last_error, sizeof(app.launch_session.last_error),
                                 "Inspected disc %s is not present in the library after adding it.",
                                 app.inspecting_game.disc_id);
                        lres = NK_ERROR_FILE_NOT_FOUND;
                    } else {
                        lres = nk_launch_prepare_session(&app.launch_session, &app.games[game_idx], target_root);
                    }
                    /* --launch-now stands in for pressing PLAY NOW, so it has to
                       launch the way PLAY NOW does. nk_launch defaults gui_mode
                       to false, which put --sched on the argv: the runtime ran to
                       completion and exited 0 without ever opening a window, so
                       the launch looked successful while showing nothing. */
                    app.launch_session.config.gui_mode = true;
                    if (lres == NK_OK) {
                        printf("[PLAYER] Launch session prepared successfully!\n");
                        printf("[PLAYER] Executable: %s\n", app.launch_session.executable_path);
                        printf("[PLAYER] ISO: %s\n", app.launch_session.iso_path);
                        NkResult sres = nk_launch_start(&app.launch_session);
                        if (sres == NK_OK) {
                            printf("[PLAYER] Child process started! PID: %d\n", app.launch_session.process.process_id);
                            app.is_game_running = true;
                            app.launch_time_ms = SDL_GetTicks();
                        } else {
                            fprintf(stderr, "[PLAYER] Failed to start runtime: %s\n", app.launch_session.last_error);
                            player_app_set_error(&app, "PROCESS_SPAWN_FAILED", "Failed to Spawn Process",
                                                 app.launch_session.last_error[0] ? app.launch_session.last_error : "CreateProcess failed.",
                                                 "Return to Library", VIEW_LIBRARY);
                        }
                    } else {
                        fprintf(stderr, "[PLAYER] Failed to prepare launch session: %s\n", app.launch_session.last_error);
                        const char *err_code = "RUNTIME_NOT_FOUND";
                        const char *err_title = "Recompiled Binary Not Available";
                        if (strstr(app.launch_session.last_error, "manifest") != NULL ||
                            strstr(app.launch_session.last_error, "profile") != NULL ||
                            strstr(app.launch_session.last_error, "catalog") != NULL) {
                            err_code = "MANIFEST_MISMATCH";
                            err_title = "Title Manifest Mismatch";
                        }
                        player_app_set_error(&app, err_code, err_title,
                                             app.launch_session.last_error[0] ? app.launch_session.last_error : "Launch preparation failed.",
                                             "Return to Library", VIEW_LIBRARY);
                    }
                }
            } else {
                player_app_set_view(&app, VIEW_UNSUPPORTED_TITLE);
            }
        } else {
            fprintf(stderr, "[PLAYER] Failed to inspect ISO: %s\n", initial_iso_path);
            return 1;
        }
    }

    if (populate_sample) {
        player_app_populate_sample_games(&app);
    }

    /* Configure test view if requested */
    if (test_view) {
        if (strcmp(test_view, "empty") == 0) {
            app.game_count = 0;
            app.selected_game_index = -1;
            player_app_set_view(&app, VIEW_LIBRARY);
        } else if (strcmp(test_view, "library") == 0) {
            player_app_set_view(&app, VIEW_LIBRARY);
        } else if (strcmp(test_view, "inspecting") == 0) {
            player_app_set_view(&app, VIEW_INSPECTING);
        } else if (strcmp(test_view, "supported") == 0) {
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "TEST00001");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name), "Nakagawa Synthetic Allegrex Fixture");
            player_app_set_view(&app, VIEW_SUPPORTED_TITLE);
        } else if (strcmp(test_view, "unsupported") == 0) {
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "ULES99999");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name), "Unknown PSP Game");
            player_app_set_view(&app, VIEW_UNSUPPORTED_TITLE);
        } else if (strcmp(test_view, "preparing") == 0) {
            /* No preparation pipeline is connected: render honest idle state */
            app.prep_state.stage = STAGE_IDLE;
            app.prep_state.completed_items = 0;
            app.prep_state.total_items = 0;
            app.prep_state.percentage = 0.0f;
            player_app_set_view(&app, VIEW_PREPARING);
        } else if (strcmp(test_view, "settings") == 0) {
            player_app_set_view(&app, VIEW_SETTINGS);
        } else if (strcmp(test_view, "error") == 0) {
            player_app_set_error(&app, "SOURCE_NOT_FOUND", "Game Source File Not Found",
                                 "Nakagawa could not locate the source ISO file on disk.",
                                 "Locate Game ISO", VIEW_LIBRARY);
        } else if (strcmp(test_view, "focus") == 0) {
            app.focus_index = 1;
            player_app_set_view(&app, VIEW_LIBRARY);
        }
    }

    app.window_width = override_w;
    app.window_height = override_h;

    /* Initialize SDL3 */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (screenshot_path != NULL) {
        win_flags |= SDL_WINDOW_HIDDEN;
    }

    SDL_Window *window = SDL_CreateWindow("Nakagawa Recomp", app.window_width, app.window_height, win_flags);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    UiInput input;
    memset(&input, 0, sizeof(input));

    /* Headless Screenshot Mode */
    if (screenshot_path != NULL) {
        SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, app.window_width, app.window_height);
        if (target) {
            SDL_SetRenderTarget(renderer, target);
        }
        ui_render_frame(renderer, &app, &input);
        if (ui_capture_screenshot(renderer, screenshot_path)) {
            printf("[PLAYER] Screenshot saved to %s (%dx%d)\n", screenshot_path, app.window_width, app.window_height);
        } else {
            fprintf(stderr, "[PLAYER] Failed saving screenshot to %s: %s\n", screenshot_path, SDL_GetError());
        }
        if (target) {
            SDL_SetRenderTarget(renderer, NULL);
            SDL_DestroyTexture(target);
        }

        if (app.is_game_running) {
            printf("[PLAYER] Verifying child process runtime health...\n");
            SDL_Delay(1000);
            if (nk_launch_is_running(&app.launch_session)) {
                printf("[PLAYER] Child process running verified! PID: %d\n", app.launch_session.process.process_id);
                nk_launch_stop(&app.launch_session);
                printf("[PLAYER] Child process stopped cleanly after verification.\n");
            } else {
                int code = nk_launch_wait(&app.launch_session, 0);
                printf("[PLAYER] Child process exited with code %d\n", code);
            }
            app.is_game_running = false;
        }

        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    /* Interactive Event Loop */
    /* SDL_INIT_GAMEPAD was requested and the UI has always had a controller
       badge and a Controller settings tab, but nothing ever opened a pad or
       set settings.controller_connected -- the badge could only ever say
       KEYBOARD READY, and the "gamepad input" the progress document claimed
       did not exist. Open the first pad that appears, report its real name,
       and map the d-pad and shoulders onto the same library selection the
       arrow keys drive. */
    SDL_Gamepad *gamepad = NULL;

    bool running = true;
    while (running && !app.should_quit) {
        input.mouse_clicked = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                    app.window_width = event.window.data1;
                    app.window_height = event.window.data2;
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    input.mouse_x = (int)event.motion.x;
                    input.mouse_y = (int)event.motion.y;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        input.mouse_down = true;
                        input.mouse_clicked = true;
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        input.mouse_down = false;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE) {
                        if (app.active_view != VIEW_LIBRARY) {
                            player_app_set_view(&app, VIEW_LIBRARY);
                        } else {
                            running = false;
                        }
                    } else if (event.key.key == SDLK_O) {
                        /* Trigger file picker */
                        trigger_file_picker(window, &app);
                    } else if (app.active_view == VIEW_LIBRARY) {
                        /* Keyboard selection across the whole library, not
                           just the cards that happen to fit on screen. */
                        if (event.key.key == SDLK_LEFT) {
                            player_app_move_selection(&app, -1);
                        } else if (event.key.key == SDLK_RIGHT) {
                            player_app_move_selection(&app, 1);
                        } else if (event.key.key == SDLK_HOME) {
                            app.selected_game_index = app.game_count > 0 ? 0 : -1;
                        } else if (event.key.key == SDLK_END) {
                            app.selected_game_index = app.game_count - 1;
                        } else if (event.key.key == SDLK_PAGEUP) {
                            player_app_move_selection(&app, -player_app_visible_library_cards(&app));
                        } else if (event.key.key == SDLK_PAGEDOWN) {
                            player_app_move_selection(&app, player_app_visible_library_cards(&app));
                        }
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    if (app.active_view == VIEW_LIBRARY && event.wheel.y != 0.0f) {
                        player_app_move_selection(&app, event.wheel.y > 0.0f ? -1 : 1);
                    }
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad) {
                        gamepad = SDL_OpenGamepad(event.gdevice.which);
                        if (gamepad) {
                            const char *pad_name = SDL_GetGamepadName(gamepad);
                            snprintf(app.settings.controller_name, sizeof(app.settings.controller_name),
                                     "%s", pad_name ? pad_name : "Controller");
                            app.settings.controller_connected = true;
                            printf("[PLAYER] Gamepad connected: %s\n", app.settings.controller_name);
                        }
                    }
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && event.gdevice.which == SDL_GetGamepadID(gamepad)) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = NULL;
                        app.settings.controller_name[0] = '\0';
                        app.settings.controller_connected = false;
                        printf("[PLAYER] Gamepad disconnected\n");
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (app.active_view == VIEW_LIBRARY) {
                        switch (event.gbutton.button) {
                            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                                player_app_move_selection(&app, -1);
                                break;
                            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                                player_app_move_selection(&app, 1);
                                break;
                            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                                player_app_move_selection(&app, -player_app_visible_library_cards(&app));
                                break;
                            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                                player_app_move_selection(&app, player_app_visible_library_cards(&app));
                                break;
                            default:
                                break;
                        }
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                        player_app_set_view(&app, VIEW_LIBRARY);
                    }
                    break;
                case SDL_EVENT_DROP_FILE:
                    if (event.drop.data) {
                        const char *dropped = event.drop.data;
                        const char *files[2] = { dropped, NULL };
                        on_file_dialog_callback(&app, files, 0);
                    }
                    break;
                default:
                    break;
            }
        }

        /* A renderer control asked for the host file dialog. The renderer has
           no window handle and must stay free of platform dialog calls, so the
           request is serviced here. */
        if (app.request_file_picker) {
            app.request_file_picker = false;
            trigger_file_picker(window, &app);
        }

        /* Monitor running game process */
        if (app.is_game_running) {
            if (app.launch_time_ms == 0) {
                app.launch_time_ms = SDL_GetTicks();
            }
            if (!nk_launch_is_running(&app.launch_session)) {
                uint64_t now_ms = SDL_GetTicks();
                uint64_t elapsed_ms = now_ms >= app.launch_time_ms ? (now_ms - app.launch_time_ms) : 0;
                int code = nk_launch_wait(&app.launch_session, 0);
                app.is_game_running = false;
                printf("[PLAYER] Game process exited with code %d (ran for %llu ms)\n", code, (unsigned long long)elapsed_ms);

                if (elapsed_ms < 500) {
                    char err_msg[512];
                    snprintf(err_msg, sizeof(err_msg),
                             "Child runtime exited prematurely after %llu ms (exit code %d).\n"
                             "Process terminated before initialization or scheduler loop could start.",
                             (unsigned long long)elapsed_ms, code);
                    player_app_set_error(&app, "RUNTIME_PREMATURE_EXIT", "Child Process Terminated Early",
                                         err_msg, "Return to Library", VIEW_LIBRARY);
                } else if (code != 0) {
                    char err_msg[512];
                    snprintf(err_msg, sizeof(err_msg),
                             "Child runtime process exited abnormally with code %d.\n"
                             "Check runtime log files for crash traceback or missing symbol details.",
                             code);
                    player_app_set_error(&app, "RUNTIME_ERROR_EXIT", "Child Process Error Exit",
                                         err_msg, "Return to Library", VIEW_LIBRARY);
                }
            }
        }

        ui_render_frame(renderer, &app, &input);
        SDL_Delay(16); /* ~60 FPS loop */
    }

    if (app.is_game_running) {
        player_app_stop_game(&app);
    }

    if (gamepad) {
        SDL_CloseGamepad(gamepad);
        gamepad = NULL;
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
