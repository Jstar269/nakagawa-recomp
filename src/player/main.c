/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_state.h"
#include "iso_reader.h"
#include "ui_renderer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    PlayerApp app;
    player_app_init(&app);

    const char *screenshot_path = NULL;
    const char *test_view = NULL;
    int override_w = 1280;
    int override_h = 720;
    bool populate_sample = true;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--screenshot=", 13) == 0) {
            screenshot_path = argv[i] + 13;
        } else if (strncmp(argv[i], "--view=", 7) == 0) {
            test_view = argv[i] + 7;
        } else if (strncmp(argv[i], "--width=", 8) == 0) {
            override_w = atoi(argv[i] + 8);
        } else if (strncmp(argv[i], "--height=", 9) == 0) {
            override_h = atoi(argv[i] + 9);
        } else if (strcmp(argv[i], "--empty") == 0) {
            populate_sample = false;
        }
    }

    if (populate_sample && (!test_view || strcmp(test_view, "empty") != 0)) {
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
            app.prep_state.stage = STAGE_EXTRACTING_CONTAINERS;
            app.prep_state.completed_items = 18450;
            app.prep_state.total_items = 56672;
            app.prep_state.percentage = 32.5f;
            player_app_set_view(&app, VIEW_PREPARING);
        } else if (strcmp(test_view, "settings") == 0) {
            player_app_set_view(&app, VIEW_SETTINGS);
        } else if (strcmp(test_view, "error") == 0) {
            player_app_set_error(&app, "SOURCE_NOT_FOUND", "Game Source File Not Found",
                                 "Nakagawa could not locate the source ISO file on disk.",
                                 "Locate Game ISO", VIEW_LIBRARY);
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
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    /* Interactive Event Loop */
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

        /* Monitor running game process */
        if (app.is_game_running) {
            if (!nk_launch_is_running(&app.launch_session)) {
                int code = nk_launch_wait(&app.launch_session, 0);
                app.is_game_running = false;
                printf("[PLAYER] Game process exited with code %d\n", code);
            }
        }

        ui_render_frame(renderer, &app, &input);
        SDL_Delay(16); /* ~60 FPS loop */
    }

    if (app.is_game_running) {
        player_app_stop_game(&app);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
