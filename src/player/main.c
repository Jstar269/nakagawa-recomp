/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_state.h"
#include "package_builder.h"
#include "iso_reader.h"
#include "ui_renderer.h"
#include "nk_title_manifest.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "setup_staging.h"
#include "nk_platform.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <shellapi.h>
#endif

static bool player_view_is_library(PlayerView view) {
    return view == VIEW_LIBRARY || view == PLAYER_VIEW_READY_LIBRARY;
}

static bool player_populate_inspected_game(PlayerApp *app, const char *iso_path,
                                           const IsoInspectResult *result,
                                           char *error, size_t error_len) {
    if (!app || !iso_path || !result) return false;
    GameRecord *game = &app->inspecting_game;
    memset(game, 0, sizeof(*game));
    const char *title_id_end = memchr(result->matched_title_id, '\0',
                                      sizeof(result->matched_title_id));
    if (!title_id_end ||
        (size_t)(title_id_end - result->matched_title_id) >= sizeof(game->title_id)) {
        if (error && error_len) {
            snprintf(error, error_len,
                     "Inspected catalog title ID exceeds the player identity limit.");
        }
        return false;
    }
    snprintf(game->disc_id, sizeof(game->disc_id), "%s", result->disc_id);
    snprintf(game->title_name, sizeof(game->title_name), "%s", result->title_name);
    snprintf(game->disc_version, sizeof(game->disc_version), "%s", result->disc_version);
    snprintf(game->iso_path, sizeof(game->iso_path), "%s", iso_path);
    memcpy(game->title_id, result->matched_title_id,
           (size_t)(title_id_end - result->matched_title_id) + 1);
    game->iso_size_bytes = result->file_size;
    game->status = (NkGameSupportStatus)result->status;
    game->executable_eboot_kind = (uint32_t)result->executables.eboot.kind;
    game->executable_boot_kind = (uint32_t)result->executables.boot.kind;
    game->executable_selection = (uint32_t)result->executables.selected;
    game->executable_boot_fallback = result->executables.boot_fallback;
    snprintf(game->selected_executable, sizeof(game->selected_executable), "%s",
             result->executables.selected_path);

    if (!result->is_supported && result->param_sfo_parsed) {
        char user_data_root[NK_MAX_PATH];
        char profile_id[64];
        if (!nk_platform_get_app_data_dir(user_data_root, sizeof(user_data_root)) ||
            !nk_title_manifest_write_experimental_profile(
                iso_path, result->param_sfo_parsed, result->disc_id,
                result->title_name, result->executables.selected_path,
                user_data_root, profile_id, sizeof(profile_id), error, error_len)) {
            if (error && error_len && !error[0]) {
                snprintf(error, error_len, "Could not create the local experimental title profile.");
            }
            return false;
        }
        snprintf(game->title_id, sizeof(game->title_id), "%s", profile_id);
        game->is_experimental = true;
        game->status = NK_STATUS_IDENTIFIED;
    }
    return true;
}

static void SDLCALL on_file_dialog_callback(void *userdata, const char * const *filelist, int filter) {
    (void)filter;
    PlayerApp *app = (PlayerApp *)userdata;
    if (!app || !filelist || !filelist[0]) {
        return;
    }
    if (app->wizard.is_extracting) {
        /* A second ISO cannot replace the source while the worker owns the
           current staging transaction. */
        return;
    }
    const char *selected_path = filelist[0];
    printf("[PLAYER] ISO Selected: %s\n", selected_path);

    IsoInspectResult res;
    if (iso_inspect_file(selected_path, &res)) {
        char profile_error[256] = "";
        if (!player_populate_inspected_game(app, selected_path, &res,
                                            profile_error, sizeof(profile_error))) {
            if (app->active_view == VIEW_SETUP_WIZARD) {
                player_app_wizard_reset_extraction(app);
                app->wizard.iso_selected = false;
                snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                         "%s", profile_error[0] ? profile_error : "Experimental profile could not be saved.");
            } else {
                player_app_set_error(app, "EXPERIMENTAL_PROFILE_FAILED",
                                     "Could Not Save Experimental Profile",
                                     profile_error[0] ? profile_error : "Experimental profile could not be saved.",
                                     "Return to Library", VIEW_LIBRARY);
            }
            return;
        }
        player_app_build_compatibility_preflight(app, res.success,
                                                  res.param_sfo_parsed,
                                                  &res.executables);
        app->inspecting_game.is_prepared = false;
        app->inspecting_game.prepared_root[0] = '\0';

        if (app->active_view == VIEW_SETUP_WIZARD) {
            player_app_wizard_reset_extraction(app);
            app->wizard.iso_selected = true;
            app->wizard.step = WIZARD_STEP_INSPECT_VERIFY;
            if (app->inspecting_game.is_experimental) {
                snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                         "Experimental: this game has not been verified. Compatibility is unknown. Review the missing preflight checks below.");
            } else {
                snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                         "Disc image inspected: %s (%s). Review the preflight checks below.",
                         res.title_name, res.disc_id);
            }
            app->focus_index = 0;
        } else if (app->inspecting_game.is_experimental) {
            player_app_set_view(app, VIEW_EXPERIMENTAL_TITLE);
        } else if (res.is_supported) {
            player_app_set_view(app, VIEW_SUPPORTED_TITLE);
        } else {
            player_app_set_view(app, VIEW_UNSUPPORTED_TITLE);
        }
    } else {
        if (app->active_view == VIEW_SETUP_WIZARD) {
            player_app_wizard_reset_extraction(app);
            app->wizard.iso_selected = false;
            snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                     "Selected file is not a valid PSP disc image: %.190s",
                     res.error_message[0] ? res.error_message : "Not a valid ISO9660 image.");
        } else {
            player_app_set_error(app, "ISO_CORRUPT", "Unreadable PSP Disc Image",
                                 res.error_message[0] ? res.error_message : "The selected file is not a valid ISO9660 disc image.",
                                 "Try Another File", VIEW_LIBRARY);
        }
    }
}

static void trigger_file_picker(SDL_Window *window, PlayerApp *app) {
    SDL_DialogFileFilter filters[] = {
        { "PSP Disc Images (*.iso)", "iso" },
        { "All Files (*.*)", "*" }
    };
    SDL_ShowOpenFileDialog(on_file_dialog_callback, app, window, filters, 2, NULL, false);
}

#define PLAYER_UI_LOGICAL_WIDTH 1280
#define PLAYER_UI_LOGICAL_HEIGHT 720
#define PLAYER_WINDOW_MIN_LOGICAL_WIDTH 960
#define PLAYER_WINDOW_MIN_LOGICAL_HEIGHT 540

static SDL_DisplayID player_window_display(const PlayerSettings *settings) {
    SDL_DisplayID display = 0;
    if (settings && settings->launcher_window_position_valid) {
        SDL_Point center = {
            settings->launcher_window_x + settings->launcher_window_width / 2,
            settings->launcher_window_y + settings->launcher_window_height / 2
        };
        display = SDL_GetDisplayForPoint(&center);
    }
    if (!display) display = SDL_GetPrimaryDisplay();
    return display;
}

static bool player_display_usable_bounds(SDL_DisplayID display, SDL_Rect *bounds) {
    if (!display || !bounds) return false;
    if (!SDL_GetDisplayUsableBounds(display, bounds)) {
        if (!SDL_GetDisplayBounds(display, bounds)) return false;
    }
    return bounds->w > 0 && bounds->h > 0;
}

static float player_display_content_scale(SDL_DisplayID display) {
    float scale = SDL_GetDisplayContentScale(display);
    if (scale < 1.0f || scale > 3.0f) scale = 1.0f;
    return scale;
}

static PlayerWindowFrame player_estimated_window_frame(float content_scale) {
    PlayerWindowFrame frame;
    frame.left = (int)(8.0f * content_scale + 0.999f);
    frame.right = frame.left;
    frame.top = (int)(32.0f * content_scale + 0.999f);
    frame.bottom = frame.left;
    return frame;
}

static PlayerWindowRect player_requested_window(const PlayerSettings *settings,
                                                 float content_scale) {
    PlayerWindowRect requested;
    requested.x = settings ? settings->launcher_window_x : 0;
    requested.y = settings ? settings->launcher_window_y : 0;
    if (settings && settings->launcher_window_position_valid) {
        requested.width = settings->launcher_window_width;
        requested.height = settings->launcher_window_height;
    } else {
        requested.width = (int)(PLAYER_UI_LOGICAL_WIDTH * content_scale + 0.5f);
        requested.height = (int)(PLAYER_UI_LOGICAL_HEIGHT * content_scale + 0.5f);
    }
    return requested;
}

static bool player_apply_window_geometry(SDL_Window *window,
                                         PlayerWindowRect requested,
                                         PlayerWindowRect usable,
                                         PlayerWindowFrame frame,
                                         bool position_valid,
                                         PlayerWindowRect *fitted) {
    PlayerWindowRect next;
    if (!window || !player_window_fit_to_display(requested, usable, frame,
                                                 position_valid, &next)) {
        return false;
    }
    bool size_set = SDL_SetWindowSize(window, next.width, next.height);
    bool position_set = SDL_SetWindowPosition(window, next.x, next.y);
    if (fitted) *fitted = next;
    return size_set && position_set;
}

static bool player_refit_to_actual_frame(SDL_Window *window,
                                         PlayerWindowRect requested,
                                         SDL_Rect usable,
                                         bool position_valid,
                                         PlayerWindowRect *fitted) {
    int top = 0;
    int left = 0;
    int bottom = 0;
    int right = 0;
    if (!SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right)) {
        return false;
    }
    PlayerWindowRect usable_rect = { usable.x, usable.y, usable.w, usable.h };
    PlayerWindowFrame frame = { top, left, bottom, right };
    return player_apply_window_geometry(window, requested, usable_rect, frame,
                                        position_valid, fitted);
}

static void player_capture_window_settings(PlayerApp *app, SDL_Window *window) {
    if (!app || !window) return;
    Uint32 flags = SDL_GetWindowFlags(window);
    app->settings.launcher_fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    if (app->settings.launcher_fullscreen) return;

    app->settings.launcher_window_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    if (flags & SDL_WINDOW_MINIMIZED) return;
    if (app->settings.launcher_window_maximized) return;

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (SDL_GetWindowPosition(window, &x, &y) &&
        SDL_GetWindowSize(window, &width, &height) &&
        width >= 320 && height >= 240) {
        app->settings.launcher_window_x = x;
        app->settings.launcher_window_y = y;
        app->settings.launcher_window_width = width;
        app->settings.launcher_window_height = height;
        app->settings.launcher_window_position_valid = true;
    }
}

static void player_apply_launcher_fullscreen(PlayerApp *app, SDL_Window *window,
                                              bool *applied_fullscreen) {
    if (!app || !window || !applied_fullscreen ||
        app->settings.launcher_fullscreen == *applied_fullscreen) {
        return;
    }
    bool requested = app->settings.launcher_fullscreen;
    if (SDL_SetWindowFullscreen(window, requested)) {
        *applied_fullscreen = requested;
    } else {
        app->settings.launcher_fullscreen = *applied_fullscreen;
        snprintf(app->settings_notice, sizeof(app->settings_notice),
                 "Could not change launcher fullscreen mode: %.72s",
                 SDL_GetError());
        player_app_save_settings(app, NULL);
    }
}

static bool player_confirm_quit(SDL_Window *window, PlayerApp *app) {
    if (player_app_take_close_confirmation_fallback(app)) {
        fprintf(stderr,
                "[PLAYER] Close confirmation was unavailable; the second close request is treated as explicit force-quit.\n");
        return player_app_close_decision(app, true) == PLAYER_CLOSE_QUIT;
    }
    if (player_app_close_decision(app, false) != PLAYER_CLOSE_CONFIRM_REQUIRED) {
        return true;
    }

    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT |
              SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
          0, "Cancel" },
        { 0, 1, "Close game and quit" }
    };
    const SDL_MessageBoxData dialog = {
        SDL_MESSAGEBOX_WARNING,
        window,
        "Game still running",
        "A game is running. Close it and quit?",
        2,
        buttons,
        NULL
    };
    int button_id = 0;
    bool dialog_shown = false;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (!getenv("NK_UI_TEST_MESSAGEBOX_FAIL")) {
        dialog_shown = SDL_ShowMessageBox(&dialog, &button_id);
    }
#else
    dialog_shown = SDL_ShowMessageBox(&dialog, &button_id);
#endif
    if (!dialog_shown) {
        player_app_note_close_confirmation_failure(app);
        fprintf(stderr,
                "[PLAYER] Close confirmation failed: %.160s. Repeat the close request to force-quit safely.\n",
                SDL_GetError());
        return false;
    }
    return player_app_close_decision(app, button_id == 1) == PLAYER_CLOSE_QUIT;
}

/* Keep the user-input mapping in one place so the native loop and the headless
 * regression drive the same SDL event path. Worker and device-lifecycle events
 * remain owned by the loop because they carry live handles. */
static bool player_dispatch_ui_event(PlayerApp *app, UiInput *input,
                                     SDL_Window *window, bool *running,
                                     bool *close_request_handled_in_batch,
                                     const SDL_Event *event) {
    if (!app || !input || !running || !event) return false;

    switch (event->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (player_app_close_request_batch_claim(
                close_request_handled_in_batch) &&
            player_confirm_quit(window, app)) {
            *running = false;
        }
        return true;
    case SDL_EVENT_WINDOW_RESIZED:
        if (!app->logical_ui) {
            app->window_width = event->window.data1;
            app->window_height = event->window.data2;
        }
        if (!(SDL_GetWindowFlags(window) &
              (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED |
               SDL_WINDOW_MINIMIZED))) {
            app->settings.launcher_window_width = event->window.data1;
            app->settings.launcher_window_height = event->window.data2;
        }
        return true;
    case SDL_EVENT_WINDOW_MOVED:
        if (!(SDL_GetWindowFlags(window) &
              (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED |
               SDL_WINDOW_MINIMIZED))) {
            app->settings.launcher_window_x = event->window.data1;
            app->settings.launcher_window_y = event->window.data2;
            app->settings.launcher_window_position_valid = true;
        }
        return true;
    case SDL_EVENT_WINDOW_MAXIMIZED:
        app->settings.launcher_window_maximized = true;
        return true;
    case SDL_EVENT_WINDOW_RESTORED:
        app->settings.launcher_window_maximized =
            (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        input->mouse_x = (int)event->motion.x;
        input->mouse_y = (int)event->motion.y;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event->button.button == SDL_BUTTON_LEFT) {
            input->mouse_down = true;
            input->mouse_clicked = true;
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event->button.button == SDL_BUTTON_LEFT) input->mouse_down = false;
        return true;
    case SDL_EVENT_KEY_DOWN:
        if (!event->key.repeat &&
            (event->key.key == SDLK_F11 ||
             ((event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER) &&
              (event->key.mod & SDL_KMOD_ALT)))) {
            player_app_toggle_launcher_fullscreen(app);
        } else if (event->key.key == SDLK_ESCAPE) {
            if (app->active_view == VIEW_SETUP_WIZARD) {
                player_app_wizard_back(app);
            } else if (app->active_view == VIEW_CONTROLLER_SETTINGS) {
                if (input_settings_is_capturing(&app->input_settings)) {
                    input_settings_cancel_capture(&app->input_settings);
                } else if (input_settings_is_calibrating(&app->input_settings)) {
                    input_settings_cancel_calibration(&app->input_settings);
                } else {
                    player_app_set_view(app, VIEW_SETTINGS);
                }
            } else if (app->active_view == VIEW_PREREQ_CONSENT ||
                       app->active_view == VIEW_PREREQ_PROGRESS) {
                player_app_prereq_cancel(app);
            } else if (app->active_view == VIEW_PREREQ_ABOUT ||
                       app->active_view == VIEW_CONFIRM_REMOVE_TOOLS) {
                player_app_set_view(app, VIEW_SETTINGS);
            } else if (app->active_view == VIEW_BUILDING_PACKAGE) {
                player_app_cancel_package_build(app);
                player_app_set_view(app, VIEW_LIBRARY);
            } else if (!player_view_is_library(app->active_view)) {
                player_app_set_view(app, VIEW_LIBRARY);
            } else {
                if (player_confirm_quit(window, app)) *running = false;
            }
        } else if (event->key.key == SDLK_O && !app->wizard.is_extracting) {
            trigger_file_picker(window, app);
        } else if (event->key.key == SDLK_S && app->active_view != VIEW_INSPECTING) {
            if (app->active_view == VIEW_SETTINGS) {
                player_app_set_view(app, VIEW_LIBRARY);
            } else {
                player_app_set_view(app, VIEW_SETTINGS);
            }
        } else if (event->key.key == SDLK_TAB) {
            int count = ui_focus_count(app);
            player_app_move_focus(app, (event->key.mod & SDL_KMOD_SHIFT) ? -1 : 1,
                                  count);
        } else if (event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER ||
                   event->key.key == SDLK_SPACE) {
            /* Held-key auto-repeat must not re-fire actions: the first PLAY
               press swaps the button to STOP, so a repeat would stop the game. */
            if (!event->key.repeat) input->activate_pressed = true;
        } else if (player_view_is_library(app->active_view)) {
            if (event->key.key == SDLK_LEFT) {
                player_app_move_selection(app, -1);
            } else if (event->key.key == SDLK_RIGHT) {
                player_app_move_selection(app, 1);
            } else if (event->key.key == SDLK_UP) {
                player_app_move_focus(app, -1, ui_focus_count(app));
            } else if (event->key.key == SDLK_DOWN) {
                player_app_move_focus(app, 1, ui_focus_count(app));
            } else if (event->key.key == SDLK_HOME) {
                app->selected_game_index = app->game_count > 0 ? 0 : -1;
            } else if (event->key.key == SDLK_END) {
                app->selected_game_index = app->game_count - 1;
            } else if (event->key.key == SDLK_PAGEUP) {
                player_app_move_selection(app, -player_app_visible_library_cards(app));
            } else if (event->key.key == SDLK_PAGEDOWN) {
                player_app_move_selection(app, player_app_visible_library_cards(app));
            }
        } else if (event->key.key == SDLK_LEFT || event->key.key == SDLK_UP) {
            player_app_move_focus(app, -1, ui_focus_count(app));
        } else if (event->key.key == SDLK_RIGHT || event->key.key == SDLK_DOWN) {
            player_app_move_focus(app, 1, ui_focus_count(app));
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (player_view_is_library(app->active_view) && event->wheel.y != 0.0f) {
            player_app_move_selection(app, event->wheel.y > 0.0f ? -1 : 1);
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (app->active_view == VIEW_CONTROLLER_SETTINGS &&
            input_settings_is_capturing(&app->input_settings) &&
            (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
             event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
            event->gaxis.value > 16000) {
            NkBindingSource src;
            src.type = NK_BINDING_HOST_TRIGGER;
            src.index = event->gaxis.axis;
            input_settings_feed_capture_source(&app->input_settings, src);
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (app->active_view == VIEW_CONTROLLER_SETTINGS &&
            input_settings_is_capturing(&app->input_settings)) {
            NkBindingSource src;
            src.type = NK_BINDING_HOST_BUTTON;
            src.index = event->gbutton.button;
            input_settings_feed_capture_source(&app->input_settings, src);
            return true;
        }
        if (player_view_is_library(app->active_view)) {
            switch (event->gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                player_app_move_selection(app, -1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                player_app_move_selection(app, 1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                player_app_move_focus(app, -1, ui_focus_count(app));
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                player_app_move_focus(app, 1, ui_focus_count(app));
                break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                player_app_move_selection(app, -player_app_visible_library_cards(app));
                break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                player_app_move_selection(app, player_app_visible_library_cards(app));
                break;
            case SDL_GAMEPAD_BUTTON_SOUTH:
                input->activate_pressed = true;
                break;
            case SDL_GAMEPAD_BUTTON_START:
                player_app_set_view(app, VIEW_SETTINGS);
                break;
            default:
                break;
            }
        } else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
            if (app->active_view == VIEW_SETUP_WIZARD) {
                player_app_wizard_back(app);
            } else if (app->active_view == VIEW_CONTROLLER_SETTINGS) {
                if (input_settings_is_calibrating(&app->input_settings)) {
                    input_settings_cancel_calibration(&app->input_settings);
                } else {
                    player_app_set_view(app, VIEW_SETTINGS);
                }
            } else if (app->active_view == VIEW_PREREQ_CONSENT ||
                       app->active_view == VIEW_PREREQ_PROGRESS) {
                player_app_prereq_cancel(app);
            } else if (app->active_view == VIEW_PREREQ_ABOUT ||
                       app->active_view == VIEW_CONFIRM_REMOVE_TOOLS) {
                player_app_set_view(app, VIEW_SETTINGS);
            } else if (app->active_view == VIEW_BUILDING_PACKAGE) {
                player_app_cancel_package_build(app);
                player_app_set_view(app, VIEW_LIBRARY);
            } else {
                player_app_set_view(app, VIEW_LIBRARY);
            }
        } else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
            input->activate_pressed = true;
        } else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT ||
                   event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
            player_app_move_focus(app, -1, ui_focus_count(app));
        } else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ||
                   event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
            player_app_move_focus(app, 1, ui_focus_count(app));
        } else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
            if (app->active_view == VIEW_SETTINGS ||
                app->active_view == VIEW_CONTROLLER_SETTINGS) {
                player_app_set_view(app, VIEW_LIBRARY);
            } else {
                player_app_set_view(app, VIEW_SETTINGS);
            }
        }
        return true;
    case SDL_EVENT_DROP_FILE:
        if (event->drop.data) {
            const char *files[2] = { event->drop.data, NULL };
            on_file_dialog_callback(app, files, 0);
        }
        return true;
    default:
        return false;
    }
}

#ifdef NK_PLAYER_UI_REGRESSION_TEST
enum {
    PLAYER_UI_TEST_ACTION_WAIT_MS = -20260926,
    PLAYER_UI_TEST_ACTION_WAIT_VIEW = -20260927,
    PLAYER_UI_TEST_ACTION_ASSERT_CONSENT = -20260928,
    PLAYER_UI_TEST_ACTION_ASSERT_VIEW = -20260929,
    PLAYER_UI_TEST_ACTION_ASSERT_GAME_RUNNING = -20260930,
    PLAYER_UI_TEST_ACTION_WAIT_ART_ATTEMPT = -20261001,
    PLAYER_UI_TEST_ACTION_MOUNT_ART_ISO = -20261002,
    PLAYER_UI_TEST_ACTION_SATURATE_ART_CACHE = -20261003,
    PLAYER_UI_TEST_ACTION_START_SYNTHETIC_BUILD = -20261004,
    PLAYER_UI_TEST_ACTION_INVALIDATE_AND_START_SYNTHETIC_BUILD = -20261005,
    PLAYER_UI_TEST_ACTION_WAIT_PACKAGE_FAILURE = -20261006,
    PLAYER_UI_TEST_ACTION_INVALIDATE_PACKAGE_CACHE = -20261007,
    PLAYER_UI_TEST_ACTION_WAIT_ART_DELAY_ACTIVE = -20261008
};

static unsigned s_ui_test_package_status_thread_create_attempts;
#define UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURES 8
static unsigned s_ui_test_fail_package_status_thread_create_on_attempts[
    UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURES];
static unsigned s_ui_test_fail_package_status_thread_create_count;
#define UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURE_TITLES 8
static char s_ui_test_fail_package_status_thread_create_for[
    UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURE_TITLES][MAX_DISC_ID_LEN];
static unsigned
    s_ui_test_fail_package_status_thread_create_for_counts[
        UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURE_TITLES];
static unsigned s_ui_test_fail_package_status_thread_create_for_count;
static bool s_ui_test_waiting_for_package_failure;
static char s_ui_test_wait_package_failure_disc[MAX_DISC_ID_LEN];
static unsigned s_ui_test_wait_package_failure_target;

static bool player_ui_test_parse_views(char *names, uint32_t *mask) {
    static const struct { const char *name; PlayerView view; } views[] = {
        { "library", VIEW_LIBRARY }, { "ready_library", PLAYER_VIEW_READY_LIBRARY },
        { "supported", VIEW_SUPPORTED_TITLE }, { "experimental", VIEW_EXPERIMENTAL_TITLE },
        { "unsupported", VIEW_UNSUPPORTED_TITLE }, { "settings", VIEW_SETTINGS },
        { "building_package", VIEW_BUILDING_PACKAGE }, { "error", VIEW_ERROR },
        { "prereq_consent", VIEW_PREREQ_CONSENT },
        { "prereq_progress", VIEW_PREREQ_PROGRESS },
        { "prereq_about", VIEW_PREREQ_ABOUT },
        { "confirm_remove_tools", VIEW_CONFIRM_REMOVE_TOOLS }
    };
    if (!names || !names[0] || !mask) return false;
    uint32_t parsed_mask = 0;
    char *name = names;
    while (name) {
        char *next = strchr(name, '|');
        if (next) *next++ = '\0';
        if (!name[0]) return false;
        bool found = false;
        for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
            if (strcmp(name, views[i].name) == 0) {
                if ((unsigned)views[i].view >= 32) return false;
                parsed_mask |= UINT32_C(1) << (unsigned)views[i].view;
                found = true;
                break;
            }
        }
        if (!found) return false;
        name = next;
    }
    *mask = parsed_mask;
    return parsed_mask != 0;
}

static bool player_ui_test_parse_positive_u64(const char *text, uint64_t *value) {
    if (!text || !text[0] || text[0] == '-' || !value) return false;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (end == text || !end || *end != '\0' || parsed == 0) return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool player_ui_test_parse_fail_attempts(const char *text) {
    if (!text || !text[0]) return false;
    char copy[128];
    if (strlen(text) >= sizeof(copy)) return false;
    snprintf(copy, sizeof(copy), "%s", text);
    unsigned count = 0;
    char *item = copy;
    while (item) {
        char *next = strchr(item, ',');
        if (next) *next++ = '\0';
        uint64_t attempt = 0;
        if (!player_ui_test_parse_positive_u64(item, &attempt) ||
            attempt > UINT_MAX ||
            count >= UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURES) {
            return false;
        }
        s_ui_test_fail_package_status_thread_create_on_attempts[count++] =
            (unsigned)attempt;
        item = next;
    }
    s_ui_test_fail_package_status_thread_create_count = count;
    return count > 0;
}

static bool player_ui_test_parse_fail_titles(const char *text) {
    if (!text || !text[0]) return false;
    char copy[512];
    if (strlen(text) >= sizeof(copy)) return false;
    snprintf(copy, sizeof(copy), "%s", text);
    unsigned count = 0;
    char *item = copy;
    while (item) {
        char *next = strchr(item, ',');
        if (next) *next++ = '\0';
        if (!item[0] || strlen(item) >= MAX_DISC_ID_LEN ||
            count >= UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURE_TITLES) {
            return false;
        }
        snprintf(s_ui_test_fail_package_status_thread_create_for[count++],
                 MAX_DISC_ID_LEN, "%.*s", MAX_DISC_ID_LEN - 1, item);
        item = next;
    }
    s_ui_test_fail_package_status_thread_create_for_count = count;
    return count > 0;
}

static bool player_ui_test_fail_thread_create_for_game(
    const GameRecord *game) {
    if (!game) return false;
    for (unsigned i = 0;
         i < s_ui_test_fail_package_status_thread_create_for_count; i++) {
        if (strcmp(game->disc_id,
                   s_ui_test_fail_package_status_thread_create_for[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int player_ui_test_fail_title_index(const char *disc_id) {
    if (!disc_id) return -1;
    for (unsigned i = 0;
         i < s_ui_test_fail_package_status_thread_create_for_count; i++) {
        if (strcmp(disc_id,
                   s_ui_test_fail_package_status_thread_create_for[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int player_ui_test_next_event(const char *script, size_t *cursor,
                                    SDL_Event *event) {
    if (!script || !cursor || !event) return -1;
    size_t length = strlen(script);
    while (*cursor < length && script[*cursor] == ';') (*cursor)++;
    if (*cursor >= length) return 0;

    size_t start = *cursor;
    while (*cursor < length && script[*cursor] != ';') (*cursor)++;
    size_t token_length = *cursor - start;
    if (token_length == 0 || token_length >= 1024) return -1;
    char token[1024];
    memcpy(token, script + start, token_length);
    token[token_length] = '\0';
    memset(event, 0, sizeof(*event));

    if (strcmp(token, "QUIT") == 0) {
        event->type = SDL_EVENT_QUIT;
        return 1;
    }
    if (strcmp(token, "CLOSE") == 0) {
        event->type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        return 1;
    }
    if (strncmp(token, "WAIT_MS=", 8) == 0) {
        uint64_t duration = 0;
        if (!player_ui_test_parse_positive_u64(token + 8, &duration) ||
            duration > UINT32_MAX) return -1;
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_WAIT_MS;
        event->user.windowID = (Uint32)duration;
        return 1;
    }
    if (strncmp(token, "WAIT_VIEW=", 10) == 0) {
        char *comma = strchr(token + 10, ',');
        if (!comma) return -1;
        *comma = '\0';
        uint32_t target_mask = 0;
        uint64_t timeout = 0;
        if (!player_ui_test_parse_views(token + 10, &target_mask) ||
            !player_ui_test_parse_positive_u64(comma + 1, &timeout) ||
            timeout > UINT32_MAX) return -1;
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_WAIT_VIEW;
        event->user.windowID = target_mask;
        event->user.timestamp = timeout;
        return 1;
    }
    if (strncmp(token, "WAIT_ART_ATTEMPT=", 17) == 0) {
        uint64_t attempt = 0;
        if (!player_ui_test_parse_positive_u64(token + 17, &attempt) ||
            attempt > UINT32_MAX) return -1;
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_WAIT_ART_ATTEMPT;
        event->user.windowID = (Uint32)attempt;
        return 1;
    }
    if (strcmp(token, "WAIT_ART_DELAY_ACTIVE") == 0) {
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_WAIT_ART_DELAY_ACTIVE;
        return 1;
    }
    if (strncmp(token, "WAIT_PACKAGE_FAILURE=", 21) == 0) {
        char *comma = strchr(token + 21, ',');
        uint64_t target = 0;
        if (!comma) return -1;
        *comma = '\0';
        if (!token[21] || strlen(token + 21) >= MAX_DISC_ID_LEN ||
            !player_ui_test_parse_positive_u64(comma + 1, &target) ||
            target > UINT_MAX) return -1;
        snprintf(s_ui_test_wait_package_failure_disc,
                 sizeof(s_ui_test_wait_package_failure_disc),
                 "%s", token + 21);
        s_ui_test_wait_package_failure_target = (unsigned)target;
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_WAIT_PACKAGE_FAILURE;
        return 1;
    }
    if (strcmp(token, "INVALIDATE_PACKAGE_CACHE") == 0) {
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_INVALIDATE_PACKAGE_CACHE;
        return 1;
    }
    if (strcmp(token, "MOUNT_ART_ISO") == 0) {
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_MOUNT_ART_ISO;
        return 1;
    }
    if (strcmp(token, "SATURATE_ART_CACHE") == 0) {
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_SATURATE_ART_CACHE;
        return 1;
    }
    if (strncmp(token, "START_SYNTHETIC_BUILD=", 22) == 0) {
        uint64_t ordinal = 0;
        if (!player_ui_test_parse_positive_u64(token + 22, &ordinal) ||
            ordinal > MAX_LIBRARY_GAMES) return -1;
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_START_SYNTHETIC_BUILD;
        /* The script is one-based so title zero remains expressible. */
        event->user.windowID = (Uint32)(ordinal - 1);
        return 1;
    }
    if (strncmp(token, "INVALIDATE_AND_START_SYNTHETIC_BUILD=", 37) == 0) {
        uint64_t ordinal = 0;
        if (!player_ui_test_parse_positive_u64(token + 37, &ordinal) ||
            ordinal > MAX_LIBRARY_GAMES) return -1;
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_INVALIDATE_AND_START_SYNTHETIC_BUILD;
        /* The script is one-based so title zero remains expressible. */
        event->user.windowID = (Uint32)(ordinal - 1);
        return 1;
    }
    if (strncmp(token, "ASSERT_VIEW=", 12) == 0) {
        uint32_t target_mask = 0;
        if (!player_ui_test_parse_views(token + 12, &target_mask) ||
            (target_mask & (target_mask - 1)) != 0) return -1;
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_ASSERT_VIEW;
        event->user.windowID = target_mask;
        return 1;
    }
    if (strcmp(token, "ASSERT_GAME_RUNNING") == 0) {
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_ASSERT_GAME_RUNNING;
        return 1;
    }
    if (strncmp(token, "ASSERT_CONSENT=", 15) == 0) {
        char *comma = strchr(token + 15, ',');
        if (!comma) return -1;
        *comma = '\0';
        uint64_t item_count = 0;
        uint64_t total_bytes = 0;
        if (!player_ui_test_parse_positive_u64(token + 15, &item_count) ||
            item_count > PACKAGE_BUILDER_MAX_PREREQUISITES ||
            !player_ui_test_parse_positive_u64(comma + 1, &total_bytes)) return -1;
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_ASSERT_CONSENT;
        event->user.windowID = (Uint32)item_count;
        event->user.timestamp = total_bytes;
        return 1;
    }
    if (strncmp(token, "DROP_FILE=", 10) == 0) {
        event->type = SDL_EVENT_DROP_FILE;
        event->drop.data = SDL_strdup(token + 10);
        return event->drop.data ? 1 : -1;
    }
    if (strncmp(token, "MOUSE_MOVE=", 11) == 0) {
        int x = 0;
        int y = 0;
        char trailing = '\0';
        if (sscanf(token + 11, "%d,%d%c", &x, &y, &trailing) != 2) return -1;
        event->type = SDL_EVENT_MOUSE_MOTION;
        event->motion.x = (float)x;
        event->motion.y = (float)y;
        return 1;
    }
    if (strcmp(token, "MOUSE_DOWN") == 0 || strcmp(token, "MOUSE_UP") == 0) {
        event->type = strcmp(token, "MOUSE_DOWN") == 0
            ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event->button.button = SDL_BUTTON_LEFT;
        return 1;
    }
    if (strncmp(token, "KEY_", 4) == 0) {
        static const struct { const char *name; SDL_Keycode key; } keys[] = {
            { "ESCAPE", SDLK_ESCAPE }, { "RETURN", SDLK_RETURN },
            { "TAB", SDLK_TAB }, { "SPACE", SDLK_SPACE }, { "LEFT", SDLK_LEFT },
            { "RIGHT", SDLK_RIGHT }, { "UP", SDLK_UP }, { "DOWN", SDLK_DOWN },
            { "HOME", SDLK_HOME }, { "END", SDLK_END },
            { "PAGEUP", SDLK_PAGEUP }, { "PAGEDOWN", SDLK_PAGEDOWN },
            { "S", SDLK_S }, { "O", SDLK_O }
        };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (strcmp(token + 4, keys[i].name) == 0) {
                event->type = SDL_EVENT_KEY_DOWN;
                event->key.key = keys[i].key;
                return 1;
            }
        }
        return -1;
    }
    if (strncmp(token, "PAD_", 4) == 0) {
        static const struct { const char *name; SDL_GamepadButton button; } buttons[] = {
            { "SOUTH", SDL_GAMEPAD_BUTTON_SOUTH },
            { "NORTH", SDL_GAMEPAD_BUTTON_NORTH },
            { "EAST", SDL_GAMEPAD_BUTTON_EAST },
            { "DPAD_LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT },
            { "DPAD_RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
            { "DPAD_UP", SDL_GAMEPAD_BUTTON_DPAD_UP },
            { "DPAD_DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN },
            { "START", SDL_GAMEPAD_BUTTON_START }
        };
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            if (strcmp(token + 4, buttons[i].name) == 0) {
                event->type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                event->gbutton.button = buttons[i].button;
                return 1;
            }
        }
        return -1;
    }
    return -1;
}

static bool player_ui_test_copy_file(const char *source, const char *destination) {
    if (!source || !source[0] || !destination || !destination[0]) return false;
    FILE *input = fopen(source, "rb");
    if (!input) return false;
    FILE *output = fopen(destination, "wb");
    if (!output) {
        fclose(input);
        return false;
    }
    char buffer[8192];
    bool ok = true;
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        if (fwrite(buffer, 1, count, output) != count) {
            ok = false;
            break;
        }
    }
    if (ferror(input) || fclose(input) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    return ok;
}

static bool player_ui_test_start_synthetic_build(PlayerApp *app,
                                                  unsigned game_index) {
    if (!app || game_index >= (unsigned)app->game_count) return false;
    app->selected_game_index = (int)game_index;
    const GameRecord *game = &app->games[game_index];
    package_builder_init_session(&app->build_session,
                                 game->disc_id, game->title_name);
    app->build_session.is_complete = true;
    player_app_set_view(app, VIEW_BUILDING_PACKAGE);
    return true;
}

static const char *player_ui_test_view_name(PlayerView view) {
    switch (view) {
    case VIEW_LIBRARY: return "library";
    case VIEW_INSPECTING: return "inspecting";
    case VIEW_SUPPORTED_TITLE: return "supported";
    case VIEW_EXPERIMENTAL_TITLE: return "experimental";
    case VIEW_UNSUPPORTED_TITLE: return "unsupported";
    case VIEW_PREPARING: return "preparing";
    case VIEW_SETTINGS: return "settings";
    case VIEW_ERROR: return "error";
    case VIEW_SETUP_WIZARD: return "wizard";
    case PLAYER_VIEW_READY_LIBRARY: return "ready_library";
    case VIEW_CONTROLLER_SETTINGS: return "controller";
    case VIEW_BUILDING_PACKAGE: return "building_package";
    case VIEW_PREREQ_CONSENT: return "prereq_consent";
    case VIEW_PREREQ_PROGRESS: return "prereq_progress";
    case VIEW_PREREQ_ABOUT: return "prereq_about";
    case VIEW_CONFIRM_REMOVE_TOOLS: return "confirm_remove_tools";
    default: return "unknown";
    }
}

static const char *player_ui_test_wizard_step_name(WizardStep step) {
    switch (step) {
    case WIZARD_STEP_WELCOME: return "welcome";
    case WIZARD_STEP_SELECT_GAME: return "select_game";
    case WIZARD_STEP_INSPECT_VERIFY: return "inspect_verify";
    case WIZARD_STEP_SYSTEM_FONTS: return "system_fonts";
    case WIZARD_STEP_READY_LAUNCH: return "ready_launch";
    default: return "unknown";
    }
}

static uint64_t player_ui_test_frame_hash(SDL_Renderer *renderer) {
    SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
    if (!surface || !surface->pixels || surface->pitch <= 0 || surface->h <= 0) {
        if (surface) SDL_DestroySurface(surface);
        return 0;
    }
    const uint8_t *pixels = (const uint8_t *)surface->pixels;
    size_t size = (size_t)surface->pitch * (size_t)surface->h;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < size; i++) {
        hash ^= pixels[i];
        hash *= UINT64_C(1099511628211);
    }
    SDL_DestroySurface(surface);
    return hash;
}

static void player_ui_test_report_frame(int frame_number, const PlayerApp *app,
                                        SDL_Renderer *renderer, bool running,
                                        uint64_t render_elapsed_ns,
                                        uint64_t validation_calls) {
    const GameRecord *selected = NULL;
    NkRuntimePackageStatus package_status = NK_RUNTIME_PACKAGE_MISSING;
    if (app->selected_game_index >= 0 && app->selected_game_index < app->game_count) {
        selected = &app->games[app->selected_game_index];
        package_status = player_app_cached_runtime_package_status(app, selected);
    }
    SDL_FRect badge = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool badge_valid = ui_last_status_badge_rect(&badge);
    /* Every unchecked title is pending or explicitly unresolved. An
       unclaimed title would otherwise make its card offer a build for a
       package that may already exist. */
    int titles_pending = 0;
    int titles_failed = 0;
    int titles_unclaimed = 0;
    for (int i = 0; i < app->game_count && i < MAX_LIBRARY_GAMES; i++) {
        const PlayerRuntimePackageCacheEntry *cached =
            &app->runtime_package_cache[i];
        const GameRecord *cached_game = &app->games[i];
        bool same_game = strcmp(cached->disc_id, cached_game->disc_id) == 0 &&
                         strcmp(cached->title_id, cached_game->title_id) == 0 &&
                         strcmp(cached->selected_executable,
                                cached_game->selected_executable) == 0;
        if (same_game && cached->status_valid) continue;
        if (same_game && cached->validation_pending) {
            titles_pending++;
        } else if (same_game && cached->validation_failed) {
            titles_failed++;
        } else {
            titles_unclaimed++;
        }
    }
    int settings_two_col = app->active_view == VIEW_SETTINGS
        ? (player_settings_uses_two_columns(app->window_width,
                                             app->window_height) ? 1 : 0) : -1;
    printf("[PLAYER_UI_TEST] frame=%d ticks_ms=%llu view=%s "
           "selected=%d selected_disc=%s selected_title_id=%s "
           "focus=%d focus_count=%d settings_two_col=%d wizard_step=%s "
           "font_confirmed=%d extracting=%d extraction_percent=%d extraction_cancel=%d error=%s "
           "picker=%d package_building=%d package_cancelled=%d profile_fallback=%d "
           "controller_capturing=%d controller_conflicts=%d calibrating=%d "
           "error_text_complete=%d error_text_lines=%d "
           "error_details_complete=%d error_details_lines=%d error_details_available=%d "
           "select_binding=%d start_binding=%d circle_binding=%d profile_save_notice=%d "
           "input_scope=%s input_titles=%d input_notice=%d "
           "selected_experimental=%d selected_prepared=%d selected_staged=%d "
           "selected_runtime=%d selected_package_status=%d "
           "selected_package_check_failed=%d package_check_failed_badge=%d games=%d "
           "package_status_thread_attempts=%u "
           "titles_pending=%d titles_failed=%d titles_unclaimed=%d "
           "prereq_items=%zu prereq_bytes=%llu game_running=%d build_stage=%d "
           "font=%s font_reason=%s "
           "badge=%d,%d,%d,%d running=%d pixels=%016llx "
           "render_ns=%llu package_validations=%llu\n",
           frame_number, (unsigned long long)SDL_GetTicks(),
           player_ui_test_view_name(app->active_view),
           app->selected_game_index,
           selected ? selected->disc_id : "NONE",
           selected ? selected->title_id : "NONE",
           app->focus_index, ui_focus_count(app), settings_two_col,
           player_ui_test_wizard_step_name(app->wizard.step),
           app->wizard.font_confirmed ? 1 : 0,
           app->wizard.is_extracting ? 1 : 0, app->wizard.extraction_percent,
           player_app_wizard_cancel_requested(app) ? 1 : 0,
           app->last_error.error_code[0] ? app->last_error.error_code : "NONE",
           app->request_file_picker ? 1 : 0,
           app->build_session.is_building ? 1 : 0,
           app->build_session.is_cancelled ? 1 : 0,
           app->input_settings.has_load_diagnostic ? 1 : 0,
           input_settings_is_capturing(&app->input_settings) ? 1 : 0,
           input_settings_has_conflicts(&app->input_settings) ? 1 : 0,
           input_settings_is_calibrating(&app->input_settings) ? 1 : 0,
           ui_test_error_text_complete() ? 1 : 0,
           ui_test_error_text_lines(),
           ui_test_error_details_complete() ? 1 : 0,
           ui_test_error_details_lines(),
           app->last_error.details[0] ? 1 : 0,
           app->input_settings.profile.psp_buttons[INPUT_CONTROL_BTN_SELECT].primary.index,
           app->input_settings.profile.psp_buttons[INPUT_CONTROL_BTN_START].primary.index,
           app->input_settings.profile.psp_buttons[INPUT_CONTROL_BTN_CIRCLE].primary.index,
           app->input_settings.has_save_diagnostic ? 1 : 0,
           input_settings_is_title_scope(&app->input_settings)
               ? input_settings_scope_label(&app->input_settings) : "global",
           app->input_settings.file.title_count,
           app->input_profile_notice[0] ? 1 : 0,
           selected && selected->is_experimental ? 1 : 0,
           selected && selected->is_prepared ? 1 : 0,
           selected && selected->assets_staged ? 1 : 0,
           selected && player_app_cached_game_has_runtime(app, selected) ? 1 : 0,
           (int)package_status,
           selected && player_app_runtime_package_check_failed(app, selected) ? 1 : 0,
           selected && strcmp(ui_test_last_status_badge_label(),
                               "PACKAGE CHECK FAILED") == 0 ? 1 : 0,
           app->game_count,
           s_ui_test_package_status_thread_create_attempts,
           titles_pending, titles_failed, titles_unclaimed,
           app->prerequisites.items.count,
           (unsigned long long)app->prerequisites.total_bytes,
           app->is_game_running ? 1 : 0,
           (int)app->build_session.current_stage,
           ui_font_mode(), ui_font_fallback_reason(),
           badge_valid ? (int)badge.x : -1, badge_valid ? (int)badge.y : -1,
           badge_valid ? (int)badge.w : 0, badge_valid ? (int)badge.h : 0,
           running ? 1 : 0,
           (unsigned long long)player_ui_test_frame_hash(renderer),
           (unsigned long long)render_elapsed_ns,
           (unsigned long long)validation_calls);

    static bool consent_items_reported = false;
    if (app->active_view == VIEW_PREREQ_CONSENT && !consent_items_reported) {
        printf("[PLAYER_UI_TEST] consent items=%zu total_bytes=%llu ids=",
               app->prerequisites.items.count,
               (unsigned long long)app->prerequisites.total_bytes);
        for (size_t i = 0; i < app->prerequisites.items.count; i++) {
            printf("%s%s", i ? "," : "", app->prerequisites.items.items[i].id);
        }
        printf("\n");
        consent_items_reported = true;
    } else if (app->active_view != VIEW_PREREQ_CONSENT) {
        consent_items_reported = false;
    }
}
#endif

enum {
    PLAYER_STAGING_EVENT_PROGRESS = 1,
    PLAYER_STAGING_EVENT_COMPLETE = 2
};

enum { PLAYER_PACKAGE_STATUS_EVENT_CODE = 0x50534348 };

typedef struct {
    SDL_Thread *thread;
    int game_index;
    uint64_t cache_generation;
    uint64_t build_session_generation;
    bool force;
    bool prior_status_valid;
    bool prior_identity_valid;
    bool status_unchanged;
    bool identity_valid;
    bool runtime_available;
    bool identity_changed_during_validation;
    /* Set only when the completion event could not be queued. The UI thread
       polls this so a lost hand-off finishes the job instead of leaving the
       card pending for the rest of the session. */
    SDL_AtomicInt handoff_failed;
    char runtime_root[MAX_PATH_LEN];
    char showcase_root[MAX_PATH_LEN];
    char prior_identity[65];
    char package_identity[65];
    char reason[2048];
    GameRecord game;
    NkRuntimePackageStatus status;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    SDL_Semaphore *catalog_test_start;
    SDL_Semaphore *catalog_test_done;
    unsigned catalog_test_repetitions;
    unsigned catalog_test_status_checks;
    bool catalog_test_status_mismatch;
#endif
} PlayerPackageStatusJob;

#ifdef NK_PLAYER_UI_REGRESSION_TEST
static const char *s_ui_test_catalog_reload_path;
static unsigned s_ui_test_catalog_reload_count;
static unsigned s_ui_test_catalog_reload_completed;
static bool s_ui_test_catalog_reload_started;
static bool s_ui_test_catalog_reload_failed;
static bool ui_test_drop_worker_event;
/* The cache-epoch build regression deliberately defers the automatic rescan
   for one invalidation so an old post-build latch cannot be completed by a
   background job before the restarted synthetic build owns the queue. */
static bool s_ui_test_defer_cache_rescan_once;
#endif

static bool player_package_status_push_completion(SDL_Event *event) {
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    /* Models SDL refusing the push so the harness can prove the reclaim path.
       The event must not be queued at all: a queued event the reclaim then
       races would free a job the queue still owns. */
    if (ui_test_drop_worker_event) return false;
#endif
    return SDL_PushEvent(event) != 0;
}

static int SDLCALL player_package_status_thread_main(void *userdata) {
    PlayerPackageStatusJob *job = (PlayerPackageStatusJob *)userdata;
    if (!job) return 1;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    const char *delay_text = getenv("NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS");
    if (delay_text && delay_text[0]) {
        char *end = NULL;
        unsigned long delay = strtoul(delay_text, &end, 10);
        if (end && *end == '\0' && delay <= 5000) SDL_Delay((Uint32)delay);
    }
#endif
    char default_root[NK_MAX_PATH];
    const char *root = NULL;
    if (player_game_is_showcase(&job->game) && job->showcase_root[0]) {
        root = job->showcase_root;
    } else if (job->runtime_root[0]) {
        root = job->runtime_root;
    } else if (nk_platform_get_app_data_dir(default_root, sizeof(default_root))) {
        root = default_root;
    }

    char before_identity[65] = "";
    bool before_valid = root && nk_launch_runtime_package_cache_identity(
        root, &job->game, before_identity);
    if (!job->force && job->prior_status_valid && job->prior_identity_valid &&
        before_valid && strcmp(before_identity, job->prior_identity) == 0) {
        job->status_unchanged = true;
        job->identity_valid = true;
        snprintf(job->package_identity, sizeof(job->package_identity), "%s",
                 before_identity);
    } else if (root) {
        job->status = nk_launch_validate_runtime_package(
            root, &job->game, NULL, job->reason, sizeof(job->reason));
        job->runtime_available = job->status == NK_RUNTIME_PACKAGE_OK ||
            (job->status == NK_RUNTIME_PACKAGE_MISSING &&
             !job->game.is_experimental &&
             nk_launch_runtime_available(root, job->game.title_id));
    } else {
        job->status = NK_RUNTIME_PACKAGE_MISSING;
        snprintf(job->reason, sizeof(job->reason),
                 "Per-user data directory is unavailable; package discovery cannot run.");
    }

#ifdef NK_PLAYER_UI_REGRESSION_TEST
    /* Every exit below reaches this loop, including the warm-cache shortcut
       above, so the UI thread's reload hand-off can never wait for a
       repetition this worker would skip. */
    if (job->catalog_test_start && job->catalog_test_done) {
        for (unsigned i = 0; i < job->catalog_test_repetitions; i++) {
            (void)SDL_WaitSemaphore(job->catalog_test_start);
            NkRuntimePackageStatus repeated_status = job->status;
            if (root) {
                repeated_status = nk_launch_validate_runtime_package(
                    root, &job->game, NULL, NULL, 0);
            }
            if (repeated_status != job->status) {
                job->catalog_test_status_mismatch = true;
            }
            job->catalog_test_status_checks++;
            (void)SDL_SignalSemaphore(job->catalog_test_done);
        }
    }
#endif

    if (!job->status_unchanged) {
        char after_identity[65] = "";
        bool after_valid = root && nk_launch_runtime_package_cache_identity(
            root, &job->game, after_identity);
        job->identity_valid = after_valid;
        if (after_valid) {
            snprintf(job->package_identity, sizeof(job->package_identity), "%s",
                     after_identity);
            job->identity_changed_during_validation = before_valid &&
                strcmp(before_identity, after_identity) != 0;
        }
    }
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_USER;
    event.user.code = PLAYER_PACKAGE_STATUS_EVENT_CODE;
    event.user.data1 = job;
    if (!player_package_status_push_completion(&event)) {
        /* The result is computed and stays valid; only the notification was
           lost. Publish that fact and let the UI thread finish the job. */
        SDL_SetAtomicInt(&job->handoff_failed, 1);
        fprintf(stderr, "[PLAYER] Package status for %s could not be queued; "
                "the launcher will apply it from the worker result.\n",
                job->game.disc_id);
    }
    return 0;
}

static void player_package_status_queue_game(PlayerApp *app, int game_index,
                                             bool force,
                                             bool requested[MAX_LIBRARY_GAMES],
                                             bool force_requested[MAX_LIBRARY_GAMES]) {
    if (!app || game_index < 0 || game_index >= app->game_count) return;
    const PlayerRuntimePackageCacheEntry *cached =
        &app->runtime_package_cache[game_index];
    const GameRecord *game = &app->games[game_index];
    bool same_game = strcmp(cached->disc_id, game->disc_id) == 0 &&
                     strcmp(cached->title_id, game->title_id) == 0 &&
                     strcmp(cached->selected_executable,
                            game->selected_executable) == 0;
    /* Worker creation failures recover on a bounded title-keyed backoff.
       Explicit retries bypass the deadline; ordinary selection and periodic
       scans can re-arm only after it expires. */
    bool worker_start_failed =
        player_app_runtime_package_worker_start_failed(app, game);
    bool retry_due = player_app_runtime_package_worker_start_failed_retry_due(
        app, game, SDL_GetTicks());
    if (!force && worker_start_failed && !retry_due) return;
    if (!force && same_game && cached->validation_failed &&
        !worker_start_failed) return;
    requested[game_index] = true;
    if (force) force_requested[game_index] = true;
    /* A queued-but-not-yet-started title has no status of its own. Claiming it
       as checking keeps the card from offering a build for a package that may
       already be prepared; a title that already has a status keeps it, so the
       BUILD/REBUILD actions do not blink away on every rescan. */
    if (!same_game || !cached->status_valid) {
        player_app_runtime_package_cache_mark_pending(app, game_index);
    }
}

static void player_package_status_queue_all(PlayerApp *app,
                                            bool requested[MAX_LIBRARY_GAMES],
                                            bool force_requested[MAX_LIBRARY_GAMES]) {
    if (!app) return;
    for (int i = 0; i < app->game_count; i++) {
        player_package_status_queue_game(app, i, false, requested,
                                         force_requested);
    }
}

static bool player_package_status_start_next(
    PlayerApp *app, PlayerPackageStatusJob **job_slot,
    bool requested[MAX_LIBRARY_GAMES], bool force_requested[MAX_LIBRARY_GAMES],
    const char build_check_disc_id[MAX_DISC_ID_LEN],
    uint64_t build_check_session_generation,
    uint64_t build_check_cache_generation,
    bool *build_check_pending) {
    if (!app || !job_slot || *job_slot) return false;
    (void)build_check_cache_generation;
    int selected = app->selected_game_index;
    int index = -1;
    if (selected >= 0 && selected < app->game_count && requested[selected]) {
        index = selected;
    } else {
        for (int i = 0; i < app->game_count; i++) {
            if (requested[i]) {
                index = i;
                break;
            }
        }
    }
    if (index < 0) return false;

    PlayerPackageStatusJob *job =
        (PlayerPackageStatusJob *)calloc(1, sizeof(*job));
    if (!job) return false;
    job->game_index = index;
    job->cache_generation = app->runtime_package_cache_generation;
    job->force = force_requested[index];
    job->game = app->games[index];
    if (build_check_pending && *build_check_pending &&
        build_check_disc_id &&
        strcmp(build_check_disc_id, job->game.disc_id) == 0 &&
        build_check_session_generation != 0 &&
        app->build_session.session_generation == build_check_session_generation) {
        job->build_session_generation = build_check_session_generation;
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (s_ui_test_catalog_reload_count > 0 &&
        s_ui_test_catalog_reload_path &&
        s_ui_test_catalog_reload_path[0] &&
        !s_ui_test_catalog_reload_started) {
        job->catalog_test_start = SDL_CreateSemaphore(0);
        job->catalog_test_done = SDL_CreateSemaphore(0);
        job->catalog_test_repetitions = s_ui_test_catalog_reload_count;
        if (!job->catalog_test_start || !job->catalog_test_done) {
            if (job->catalog_test_start) {
                SDL_DestroySemaphore(job->catalog_test_start);
                job->catalog_test_start = NULL;
            }
            if (job->catalog_test_done) {
                SDL_DestroySemaphore(job->catalog_test_done);
                job->catalog_test_done = NULL;
            }
        }
    }
#endif
    snprintf(job->runtime_root, sizeof(job->runtime_root), "%s",
             app->runtime_root);
    snprintf(job->showcase_root, sizeof(job->showcase_root), "%s",
             app->showcase_root);
    PlayerRuntimePackageCacheEntry *cached =
        &app->runtime_package_cache[index];
    cached->explicit_retry_pending = false;
    bool same_game = strcmp(cached->disc_id, job->game.disc_id) == 0 &&
                     strcmp(cached->title_id, job->game.title_id) == 0 &&
                     strcmp(cached->selected_executable,
                            job->game.selected_executable) == 0;
    if (same_game) {
        job->prior_status_valid = cached->status_valid;
        job->prior_identity_valid = cached->identity_valid;
        job->status = cached->status;
        job->runtime_available = cached->runtime_available;
        snprintf(job->prior_identity, sizeof(job->prior_identity), "%s",
                 cached->package_identity);
    }
    requested[index] = false;
    force_requested[index] = false;
    player_app_runtime_package_cache_mark_pending(app, index);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    s_ui_test_package_status_thread_create_attempts++;
    bool fail_thread_create =
        player_ui_test_fail_thread_create_for_game(&job->game);
    for (unsigned failure = 0;
         failure < s_ui_test_fail_package_status_thread_create_count;
         failure++) {
        if (s_ui_test_package_status_thread_create_attempts ==
            s_ui_test_fail_package_status_thread_create_on_attempts[failure]) {
            fail_thread_create = true;
            break;
        }
    }
    if (fail_thread_create) {
        int fail_title_index = player_ui_test_fail_title_index(job->game.disc_id);
        if (fail_title_index >= 0) {
            s_ui_test_fail_package_status_thread_create_for_counts[
                fail_title_index]++;
        }
        job->thread = NULL;
    } else
#endif
    {
        job->thread = SDL_CreateThread(player_package_status_thread_main,
                                       "nakagawa-package-status", job);
    }
    if (!job->thread) {
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        if (job->catalog_test_start) {
            SDL_DestroySemaphore(job->catalog_test_start);
        }
        if (job->catalog_test_done) {
            SDL_DestroySemaphore(job->catalog_test_done);
        }
#endif
        player_app_runtime_package_cache_mark_failed(app, index,
                                                     SDL_GetTicks());
        fprintf(stderr,
                "[PLAYER] Package status for %s is unresolved because its "
                "worker could not start; use Retry Package Check.\n",
                job->game.disc_id);
        if (job->build_session_generation != 0) {
            /* Let the user see the unresolved card and retry. Keep the build
               check identity latched so a successful retry still completes
               the normal post-build library transition. */
            player_app_set_view(app, VIEW_LIBRARY);
        }
        free(job);
        return false;
    }
    *job_slot = job;
    return true;
}

static void player_package_status_finish(PlayerApp *app,
                                         PlayerPackageStatusJob **job_slot,
                                         bool requested[MAX_LIBRARY_GAMES],
                                         bool force_requested[MAX_LIBRARY_GAMES],
                                         char build_check_disc_id[MAX_DISC_ID_LEN],
                                         uint64_t *build_check_session_generation,
                                         uint64_t *build_check_cache_generation,
                                         bool *build_check_pending) {
    if (!app || !job_slot || !*job_slot) return;
    PlayerPackageStatusJob *job = *job_slot;
    if (job->thread) {
        SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (job->catalog_test_repetitions > 0) {
        bool stress_passed = !s_ui_test_catalog_reload_failed &&
            s_ui_test_catalog_reload_completed ==
                job->catalog_test_repetitions &&
            job->catalog_test_status_checks ==
                job->catalog_test_repetitions &&
            !job->catalog_test_status_mismatch;
        printf("[PLAYER_UI_TEST] catalog_reload result=%s reloads=%u "
               "status_checks=%u status=%d\n",
               stress_passed ? "PASS" : "FAIL",
               s_ui_test_catalog_reload_completed,
               job->catalog_test_status_checks, (int)job->status);
        if (job->catalog_test_start) {
            SDL_DestroySemaphore(job->catalog_test_start);
            job->catalog_test_start = NULL;
        }
        if (job->catalog_test_done) {
            SDL_DestroySemaphore(job->catalog_test_done);
            job->catalog_test_done = NULL;
        }
    }
#endif
    int current_index = -1;
    for (int i = 0; i < app->game_count; i++) {
        if (strcmp(app->games[i].disc_id, job->game.disc_id) == 0 &&
            strcmp(app->games[i].title_id, job->game.title_id) == 0 &&
            strcmp(app->games[i].selected_executable,
                   job->game.selected_executable) == 0) {
            current_index = i;
            break;
        }
    }
    bool cache_generation_matches =
        job->cache_generation == app->runtime_package_cache_generation;
    /* Any completed worker result resolves a prior worker-start failure for
       this title. A stale cache generation requires revalidation, not a
       permanent failure badge that outranks that later result. */
    if (current_index >= 0) {
        player_app_runtime_package_worker_start_failed_clear(app, &job->game);
    }
    if (current_index >= 0 && cache_generation_matches) {
        if (job->status == NK_RUNTIME_PACKAGE_OK) {
            app->games[current_index].is_prepared = true;
            app->games[current_index].status = NK_STATUS_PREPARED;
        } else if (!job->runtime_available &&
                   app->games[current_index].is_prepared) {
            app->games[current_index].is_prepared = false;
            if (app->games[current_index].assets_staged) {
                app->games[current_index].status =
                    NK_STATUS_SUPPORTED_PREPARATION;
            }
        }
        player_app_runtime_package_cache_store(
            app, current_index, &job->game, job->identity_valid,
            job->package_identity, job->status, job->runtime_available,
            SDL_GetTicks());
        if (job->identity_changed_during_validation) {
            player_package_status_queue_game(app, current_index, true,
                                             requested, force_requested);
        }
    } else if (current_index >= 0) {
        /* The worker result belongs to an invalidated cache generation. Its
           failure side record was cleared above; claim a fresh result now. */
        player_package_status_queue_game(app, current_index, false,
                                         requested, force_requested);
    }

    /* The finished job owns the build's outcome only when it is still the same
       disc: a library edit between the build and its check can move the entry,
       and the index it had then is not an identity. */
    bool handles_build = build_check_pending && *build_check_pending &&
                         current_index >= 0 && build_check_disc_id &&
                         strcmp(build_check_disc_id, job->game.disc_id) == 0 &&
                         build_check_session_generation &&
                         *build_check_session_generation != 0 &&
                         job->build_session_generation ==
                             *build_check_session_generation &&
                         app->build_session.session_generation ==
                             *build_check_session_generation;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (handles_build) {
        printf("[PLAYER_UI_TEST] post_build_validation status=%d reason=%s "
               "build_session_generation=%llu current_build_session_generation=%llu\n",
               (int)job->status,
               job->reason[0] ? job->reason : "none",
               (unsigned long long)job->build_session_generation,
               (unsigned long long)app->build_session.session_generation);
    }
#endif
    if (handles_build) {
        *build_check_pending = false;
        *build_check_session_generation = 0;
        *build_check_cache_generation = 0;
        build_check_disc_id[0] = '\0';
        if (job->status == NK_RUNTIME_PACKAGE_OK) {
            app->games[current_index].is_prepared = true;
            app->games[current_index].status = NK_STATUS_PREPARED;
            nk_library_add_or_update(&app->library,
                                     &app->games[current_index]);
            player_app_sync_library(app);
            current_index = player_app_find_game_by_disc_id(
                app, job->game.disc_id);
            if (current_index >= 0) {
                player_app_runtime_package_cache_store(
                    app, current_index, &job->game, job->identity_valid,
                    job->package_identity, job->status,
                    job->runtime_available, SDL_GetTicks());
            }
            player_app_set_view(app, PLAYER_VIEW_READY_LIBRARY);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
            printf("[PLAYER_UI_TEST] post_build_transition view=ready_library "
                   "disc=%s found=%d prepared=%d status=%d\n",
                   job->game.disc_id, current_index >= 0,
                   current_index >= 0
                       ? app->games[current_index].is_prepared : 0,
                   current_index >= 0
                       ? (int)app->games[current_index].status : -1);
#endif
        } else {
            player_app_set_build_error(
                app, "package",
                job->reason[0] ? job->reason
                               : "Package re-validation failed after build completed.",
                app->build_session.log_file_path);
        }
    }
    free(job);
    *job_slot = NULL;
}

/* A worker that could not queue its completion event has no other way to
   announce it, so the UI thread finishes the job from the result the worker
   already computed. Ignoring this would leave the card claiming
   "CHECKING PACKAGE..." for the rest of the session with no job slot free. */
static void player_package_status_recover_lost_handoff(
    PlayerApp *app,
    PlayerPackageStatusJob **job_slot,
    bool requested[MAX_LIBRARY_GAMES], bool force_requested[MAX_LIBRARY_GAMES],
    char build_check_disc_id[MAX_DISC_ID_LEN],
    uint64_t *build_check_session_generation,
    uint64_t *build_check_cache_generation,
    bool *build_check_pending) {
    if (!app || !job_slot || !*job_slot) return;
    if (!SDL_GetAtomicInt(&(*job_slot)->handoff_failed)) return;
    player_package_status_finish(app, job_slot, requested, force_requested,
                                 build_check_disc_id,
                                 build_check_session_generation,
                                 build_check_cache_generation,
                                 build_check_pending);
}

typedef struct {
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    char iso_path[NK_MAX_PATH];
    char staging_root[4096];
    char final_root[4096];
    char loose_content_root_storage[NK_TITLE_MAX_LOOSE_CONTENT_ROOTS][241];
    const char *loose_content_roots[NK_TITLE_MAX_LOOSE_CONTENT_ROOTS];
    size_t loose_content_root_count;
    bool cancel_requested;
    bool finished;
    bool completion_handled;
    NkResult result;
    int percent;
    size_t files_extracted;
    size_t total_files;
    char current_file[4096];
    char error_message[256];
    PlayerStageSummary summary;
} PlayerStagingJob;

static bool player_copy_staging_roots(
    const GameRecord *game,
    char storage[NK_TITLE_MAX_LOOSE_CONTENT_ROOTS][241],
    const char *roots[NK_TITLE_MAX_LOOSE_CONTENT_ROOTS],
    size_t *root_count
) {
    if (!game || !storage || !roots || !root_count) return false;
    *root_count = 0;
    if (game->is_experimental) {
        char user_data_root[NK_MAX_PATH];
        char profile_hash[65];
        char error[256];
        NkTitleEntrySnapshot snapshot = {0};
        if (!nk_platform_get_app_data_dir(user_data_root, sizeof(user_data_root)) ||
            !nk_title_manifest_read_experimental_profile(
                user_data_root, game->disc_id, game->title_id,
                game->selected_executable, &snapshot,
                profile_hash, error, sizeof(error))) {
            nk_title_catalog_snapshot_release(&snapshot);
            return false;
        }
        const NkTitleEntry *entry = &snapshot.entry;
        bool valid = entry->id && strcmp(entry->id, game->title_id) == 0 &&
            entry->primary_disc_id &&
            strcmp(entry->primary_disc_id, game->disc_id) == 0 &&
            entry->loose_content_root_count >= 0 &&
            entry->loose_content_root_count <= NK_TITLE_MAX_LOOSE_CONTENT_ROOTS &&
            (entry->loose_content_root_count == 0 || entry->loose_content_roots);
        for (int i = 0; valid && i < entry->loose_content_root_count; i++) {
            const NkLooseContentRoot *binding = &entry->loose_content_roots[i];
            if (!binding->root || strlen(binding->root) >= sizeof(storage[i])) {
                valid = false;
                break;
            }
            snprintf(storage[i], sizeof(storage[i]), "%s", binding->root);
            roots[i] = storage[i];
            (*root_count)++;
        }
        nk_title_catalog_snapshot_release(&snapshot);
        if (!valid) *root_count = 0;
        return valid;
    }

    nk_title_catalog_lock();
    const NkTitleEntry *by_disc = game->disc_id[0]
        ? nk_title_catalog_find_by_disc_id_locked(game->disc_id) : NULL;
    const NkTitleEntry *by_id = game->title_id[0]
        ? nk_title_catalog_find_by_id_locked(game->title_id) : NULL;
    if (!by_disc && !by_id) {
        nk_title_catalog_unlock();
        return true;
    }
    if (by_disc && game->title_id[0] &&
        (!by_id || strcmp(by_disc->id, by_id->id) != 0)) {
        nk_title_catalog_unlock();
        return false;
    }
    if (!by_disc) {
        nk_title_catalog_unlock();
        return true;
    }
    const NkTitleEntry *entry = by_disc;
    if (!entry || entry->loose_content_root_count < 0 ||
        entry->loose_content_root_count > NK_TITLE_MAX_LOOSE_CONTENT_ROOTS ||
        (entry->loose_content_root_count != 0 && !entry->loose_content_roots)) {
        nk_title_catalog_unlock();
        return false;
    }
    for (int i = 0; i < entry->loose_content_root_count; i++) {
        const NkLooseContentRoot *binding = &entry->loose_content_roots[i];
        if (!binding->root || strlen(binding->root) >= sizeof(storage[i])) {
            nk_title_catalog_unlock();
            return false;
        }
        snprintf(storage[i], sizeof(storage[i]), "%s", binding->root);
        roots[i] = storage[i];
        (*root_count)++;
    }
    nk_title_catalog_unlock();
    return true;
}

static void staging_push_event(PlayerStagingJob *job, int code) {
    if (!job) return;
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_USER;
    event.user.code = code;
    event.user.data1 = job;
    SDL_PushEvent(&event);
}

static bool staging_cancelled(void *userdata) {
    PlayerStagingJob *job = (PlayerStagingJob *)userdata;
    if (!job || !job->mutex) return true;
    SDL_LockMutex(job->mutex);
    bool cancelled = job->cancel_requested;
    SDL_UnlockMutex(job->mutex);
    return cancelled;
}

static void staging_progress(const char *current_path, int percent,
                             size_t files_extracted, size_t total_files,
                             void *userdata) {
    PlayerStagingJob *job = (PlayerStagingJob *)userdata;
    if (!job || !job->mutex) return;
    bool changed;
    SDL_LockMutex(job->mutex);
    changed = job->percent != percent || job->files_extracted != files_extracted ||
              job->total_files != total_files ||
              strcmp(job->current_file, current_path ? current_path : "") != 0;
    job->percent = percent;
    job->files_extracted = files_extracted;
    job->total_files = total_files;
    snprintf(job->current_file, sizeof(job->current_file), "%s",
             current_path ? current_path : "");
    SDL_UnlockMutex(job->mutex);
    if (changed) staging_push_event(job, PLAYER_STAGING_EVENT_PROGRESS);
}

static int SDLCALL staging_thread_main(void *userdata) {
    PlayerStagingJob *job = (PlayerStagingJob *)userdata;
    if (!job) return -1;
    PlayerStageCallbacks callbacks;
    callbacks.is_cancelled = staging_cancelled;
    callbacks.on_progress = staging_progress;
    callbacks.userdata = job;

    char error_message[256];
    NkResult result = player_stage_game_with_summary(job->iso_path,
                                                     job->staging_root,
                                                     job->loose_content_roots,
                                                     job->loose_content_root_count,
                                                     &callbacks, &job->summary,
                                                     error_message,
                                                     sizeof(error_message));
    SDL_LockMutex(job->mutex);
    job->result = result;
    snprintf(job->error_message, sizeof(job->error_message), "%s",
             error_message[0] ? error_message : "");
    job->finished = true;
    SDL_UnlockMutex(job->mutex);
    staging_push_event(job, PLAYER_STAGING_EVENT_COMPLETE);
    return result == NK_OK ? 0 : 1;
}

static bool valid_disc_id_for_staging(const char *disc_id) {
    if (!disc_id || !disc_id[0]) return false;
    for (const unsigned char *p = (const unsigned char *)disc_id; *p; p++) {
        if (!(isalnum(*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool staging_paths_for_disc(const char *disc_id, char *staging_root,
                                   size_t staging_size, char *final_root,
                                   size_t final_size) {
    if (!valid_disc_id_for_staging(disc_id) || !staging_root || !final_root ||
        staging_size == 0 || final_size == 0) return false;
    char games_root[4096];
    int games_written;
    char app_data[NK_MAX_PATH];
    if (!nk_platform_get_app_data_dir(app_data, sizeof(app_data))) return false;
    games_written = snprintf(games_root, sizeof(games_root), "%s%cgames",
                                 app_data, nk_platform_path_separator());
    if (games_written < 0 || (size_t)games_written >= sizeof(games_root)) return false;
    int staging_written = snprintf(staging_root, staging_size, "%s%c.staging_%s",
                                   games_root, nk_platform_path_separator(), disc_id);
    int final_written = snprintf(final_root, final_size, "%s%c%s", games_root,
                                 nk_platform_path_separator(), disc_id);
    return staging_written >= 0 && final_written >= 0 &&
           (size_t)staging_written < staging_size && (size_t)final_written < final_size;
}

static bool copy_bounded_text(char *destination, size_t destination_size,
                              const char *source) {
    if (!destination || destination_size == 0 || !source) return false;
    size_t length = strlen(source);
    if (length >= destination_size) return false;
    memcpy(destination, source, length + 1);
    return true;
}

static bool promote_staging_root(const char *staging_root, const char *final_root) {
    if (!staging_root || !final_root || nk_platform_dir_exists(final_root)) return false;
#if defined(_WIN32) || defined(_WIN64)
    WCHAR w_staging[32768];
    WCHAR w_final[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, staging_root, -1, w_staging,
                            (int)(sizeof(w_staging) / sizeof(w_staging[0]))) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, final_root, -1, w_final,
                            (int)(sizeof(w_final) / sizeof(w_final[0]))) <= 0) return false;
    return MoveFileExW(w_staging, w_final, MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(staging_root, final_root) == 0;
#endif
}

static bool staged_payload_is_complete(const char *root,
                                       const char *const *loose_content_roots,
                                       size_t loose_content_root_count) {
    if (!root || !root[0]) return false;
    char eboot_path[4096];
    int eboot_written = snprintf(eboot_path, sizeof(eboot_path), "%s%cEBOOT.BIN",
                                 root, nk_platform_path_separator());
    if (eboot_written <= 0 || (size_t)eboot_written >= sizeof(eboot_path) ||
        !nk_platform_file_exists(eboot_path) ||
        loose_content_root_count > NK_TITLE_MAX_LOOSE_CONTENT_ROOTS ||
        (loose_content_root_count != 0u && !loose_content_roots)) return false;
    for (size_t i = 0; i < loose_content_root_count; i++) {
        if (strcmp(loose_content_roots[i], ".") == 0) continue;
        char content_root[4096];
        int written = snprintf(content_root, sizeof(content_root), "%s%c%s", root,
                               nk_platform_path_separator(), loose_content_roots[i]);
        if (written <= 0 || (size_t)written >= sizeof(content_root)) return false;
        for (char *p = content_root + strlen(root) + 1u; *p; p++) {
            if (*p == '/' || *p == '\\') *p = nk_platform_path_separator();
        }
        if (!nk_platform_dir_exists(content_root)) return false;
    }
    return true;
}

/* A completed promotion can outlive the library write if the user-data
 * filesystem is full or temporarily unavailable. Reuse only the exact root
 * derived from the inspected disc ID and only when the executable and all
 * manifest-configured staging roots are present; never replace it with a fresh tree. */
static bool adopt_existing_staged_root(PlayerApp *app, const char *final_root) {
    if (!app || !final_root) return false;
    char root_storage[NK_TITLE_MAX_LOOSE_CONTENT_ROOTS][241] = {{0}};
    const char *loose_content_roots[NK_TITLE_MAX_LOOSE_CONTENT_ROOTS] = {0};
    size_t loose_content_root_count = 0;
    if (!player_copy_staging_roots(&app->inspecting_game, root_storage,
                                   loose_content_roots,
                                   &loose_content_root_count) ||
        !staged_payload_is_complete(final_root, loose_content_roots,
                                    loose_content_root_count) ||
        !copy_bounded_text(app->inspecting_game.prepared_root,
                           sizeof(app->inspecting_game.prepared_root),
                           final_root)) return false;
    app->inspecting_game.assets_staged = true;
    app->inspecting_game.is_prepared = false;
    app->inspecting_game.status = NK_STATUS_SUPPORTED_PREPARATION;
    copy_bounded_text(app->wizard.staging_root, sizeof(app->wizard.staging_root),
                      final_root);
    return player_app_register_staged_game(app);
}

static void request_staging_cancel(PlayerStagingJob *job) {
    if (!job || !job->mutex) return;
    SDL_LockMutex(job->mutex);
    job->cancel_requested = true;
    SDL_UnlockMutex(job->mutex);
}

static bool start_staging_job(PlayerApp *app, PlayerStagingJob **job_slot) {
    if (!app || !job_slot || !app->inspecting_game.iso_path[0]) return false;
    if (*job_slot) {
        if (!(*job_slot)->completion_handled) return false;
        SDL_DestroyMutex((*job_slot)->mutex);
        free(*job_slot);
        *job_slot = NULL;
    }
    PlayerStagingJob *job = (PlayerStagingJob *)calloc(1, sizeof(*job));
    if (!job) {
        player_app_wizard_finish_extraction(app, NK_ERROR_OUT_OF_MEMORY,
                                            "Could not allocate the staging worker.");
        return false;
    }
    if (!player_copy_staging_roots(&app->inspecting_game,
                                   job->loose_content_root_storage,
                                   job->loose_content_roots,
                                   &job->loose_content_root_count)) {
        free(job);
        player_app_wizard_finish_extraction(
            app, NK_ERROR_UNSUPPORTED_TITLE,
            "The selected title's loose-content root configuration could not be resolved.");
        return false;
    }
    if (!staging_paths_for_disc(app->inspecting_game.disc_id, job->staging_root,
                                sizeof(job->staging_root), job->final_root,
                                sizeof(job->final_root))) {
        free(job);
        player_app_wizard_finish_extraction(app, NK_ERROR_INVALID_XB,
                                            "The inspected disc ID cannot be used for a safe staging directory.");
        return false;
    }
    if (!copy_bounded_text(job->iso_path, sizeof(job->iso_path), app->inspecting_game.iso_path) ||
        !copy_bounded_text(app->wizard.staging_root, sizeof(app->wizard.staging_root),
                           job->staging_root) ||
        strlen(job->final_root) >= sizeof(app->inspecting_game.prepared_root)) {
        free(job);
        player_app_wizard_finish_extraction(app, NK_ERROR_IO,
                                            "The local application data path is too long for the player record.");
        return false;
    }
    if (nk_platform_dir_exists(job->final_root)) {
        bool complete = staged_payload_is_complete(job->final_root,
                                                   job->loose_content_roots,
                                                   job->loose_content_root_count);
        if (complete && adopt_existing_staged_root(app, job->final_root)) {
            free(job);
            player_app_wizard_finish_extraction(
                app, NK_OK, "Recovered the previously promoted staging tree.");
            return true;
        }
        free(job);
        player_app_wizard_finish_extraction(
            app, NK_ERROR_IO,
            complete
                ? "The existing staged title could not be saved to the library."
                : "An incomplete staged title already exists; it was left untouched.");
        return false;
    }
    job->mutex = SDL_CreateMutex();
    if (!job->mutex) {
        free(job);
        player_app_wizard_finish_extraction(app, NK_ERROR_OUT_OF_MEMORY,
                                            "Could not create the staging worker lock.");
        return false;
    }
    job->thread = SDL_CreateThread(staging_thread_main, "nakagawa-staging", job);
    if (!job->thread) {
        SDL_DestroyMutex(job->mutex);
        free(job);
        player_app_wizard_finish_extraction(app, NK_ERROR_OUT_OF_MEMORY,
                                            "Could not start the staging worker.");
        return false;
    }
    *job_slot = job;
    return true;
}

static void sync_staging_progress(PlayerApp *app, PlayerStagingJob *job) {
    if (!app || !job || !job->mutex) return;
    SDL_LockMutex(job->mutex);
    int percent = job->percent;
    size_t files = job->files_extracted;
    size_t total = job->total_files;
    char current[4096];
    snprintf(current, sizeof(current), "%s", job->current_file);
    SDL_UnlockMutex(job->mutex);
    player_app_wizard_set_extraction_progress(app, percent, (int)files,
                                              (int)total, current);
}

static void finish_staging_job(PlayerApp *app, PlayerStagingJob *job) {
    if (!app || !job || !job->mutex || job->completion_handled) return;
    SDL_LockMutex(job->mutex);
    NkResult result = job->result;
    char error_message[256];
    snprintf(error_message, sizeof(error_message), "%s", job->error_message);
    bool finished = job->finished;
    SDL_UnlockMutex(job->mutex);
    if (!finished) return;
    if (job->thread) {
        SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
    }
    if (result == NK_OK && !promote_staging_root(job->staging_root, job->final_root)) {
        result = NK_ERROR_IO;
        snprintf(error_message, sizeof(error_message),
                 "Asset staging completed, but atomic promotion to the game directory failed.");
        player_stage_discard(job->staging_root);
    }
    if (result == NK_OK) {
        if (!copy_bounded_text(app->inspecting_game.prepared_root,
                               sizeof(app->inspecting_game.prepared_root),
                               job->final_root)) {
            result = NK_ERROR_IO;
            snprintf(error_message, sizeof(error_message),
                     "The promoted game path is too long for the player record.");
        }
    }
    if (result == NK_OK) {
        app->inspecting_game.assets_staged = true;
        app->inspecting_game.extracted_asset_count = job->summary.extracted_asset_count;
        app->inspecting_game.extracted_audio_count = job->summary.extracted_audio_count;
        app->inspecting_game.extracted_visual_count = job->summary.extracted_visual_count;
        app->inspecting_game.extracted_layout_count = job->summary.extracted_layout_count;
        /* Package status is refreshed by the package worker after this result
           is returned to the UI thread. */
        app->inspecting_game.is_prepared = false;
        app->inspecting_game.status = NK_STATUS_SUPPORTED_PREPARATION;
        copy_bounded_text(app->wizard.staging_root, sizeof(app->wizard.staging_root),
                          job->final_root);
        if (!player_app_register_staged_game(app)) {
            result = NK_ERROR_IO;
            snprintf(error_message, sizeof(error_message),
                     "Assets were staged, but the title could not be saved to the library.");
        }
    }
    player_app_wizard_finish_extraction(app, result,
                                        error_message[0] ? error_message : NULL);
    job->completion_handled = true;
}

static void destroy_staging_job(PlayerStagingJob **job_slot) {
    if (!job_slot || !*job_slot) return;
    PlayerStagingJob *job = *job_slot;
    request_staging_cancel(job);
    if (job->thread) {
        SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
    }
    if (job->mutex) SDL_DestroyMutex(job->mutex);
    free(job);
    *job_slot = NULL;
}

/* Headless verification path for `--iso=<path> --stage-only`. It uses the
 * same native staging and promotion functions as the SDL worker, then the
 * same PlayerApp registration path. No window or timer is needed for this
 * deterministic CLI contract. */
static int stage_iso_synchronously(PlayerApp *app) {
    if (!app || !app->inspecting_game.iso_path[0] ||
        !app->inspecting_game.disc_id[0]) {
        fprintf(stderr, "[PLAYER] --stage-only requires a supported --iso=<path>.\n");
        return 2;
    }

    char staging_root[4096];
    char final_root[4096];
    char root_storage[NK_TITLE_MAX_LOOSE_CONTENT_ROOTS][241] = {{0}};
    const char *loose_content_roots[NK_TITLE_MAX_LOOSE_CONTENT_ROOTS] = {0};
    size_t loose_content_root_count = 0;
    if (!player_copy_staging_roots(&app->inspecting_game, root_storage,
                                   loose_content_roots,
                                   &loose_content_root_count)) {
        fprintf(stderr, "[PLAYER] The selected title's loose-content root configuration could not be resolved.\n");
        return 3;
    }
    if (!staging_paths_for_disc(app->inspecting_game.disc_id,
                                staging_root, sizeof(staging_root),
                                final_root, sizeof(final_root))) {
        fprintf(stderr, "[PLAYER] Could not derive a safe staging path for %s.\n",
                app->inspecting_game.disc_id);
        return 3;
    }

    if (nk_platform_dir_exists(final_root)) {
        if (adopt_existing_staged_root(app, final_root)) {
            player_app_wizard_finish_extraction(
                app, NK_OK, "Recovered the previously promoted staging tree.");
            printf("[PLAYER] STAGING_RESULT status=PASS recovered=1 disc_id=%s\n",
                   app->inspecting_game.disc_id);
            return 0;
        }
        fprintf(stderr, "[PLAYER] --stage-only found an existing staged title that could not be adopted.\n");
        return 7;
    }

    PlayerStageSummary summary;
    char error_message[256];
    NkResult result = player_stage_game_with_summary(
        app->inspecting_game.iso_path, staging_root, loose_content_roots,
        loose_content_root_count, NULL, &summary,
        error_message, sizeof(error_message));
    if (result != NK_OK) {
        fprintf(stderr, "[PLAYER] --stage-only failed (%d): %s\n", (int)result,
                error_message[0] ? error_message : "native staging failed");
        return 4;
    }
    if (!promote_staging_root(staging_root, final_root)) {
        player_stage_discard(staging_root);
        fprintf(stderr, "[PLAYER] --stage-only could not promote the staging tree.\n");
        return 5;
    }
    if (!copy_bounded_text(app->inspecting_game.prepared_root,
                           sizeof(app->inspecting_game.prepared_root),
                           final_root)) {
        fprintf(stderr, "[PLAYER] --stage-only promoted a path too long for the library record.\n");
        return 6;
    }

    app->inspecting_game.assets_staged = true;
    app->inspecting_game.extracted_asset_count = summary.extracted_asset_count;
    app->inspecting_game.extracted_audio_count = summary.extracted_audio_count;
    app->inspecting_game.extracted_visual_count = summary.extracted_visual_count;
    app->inspecting_game.extracted_layout_count = summary.extracted_layout_count;
    app->inspecting_game.is_prepared = player_app_validate_runtime_package(
        app, &app->inspecting_game, NULL, NULL, 0) == NK_RUNTIME_PACKAGE_OK;
    app->inspecting_game.status = app->inspecting_game.is_prepared
        ? NK_STATUS_PREPARED : NK_STATUS_SUPPORTED_PREPARATION;
    copy_bounded_text(app->wizard.staging_root, sizeof(app->wizard.staging_root),
                      final_root);

    if (!player_app_register_staged_game(app)) {
        player_app_wizard_finish_extraction(
            app, NK_ERROR_IO,
            "Assets were staged, but the title could not be saved to the library.");
        fprintf(stderr, "[PLAYER] --stage-only could not save the staged title.\n");
        return 7;
    }
    player_app_wizard_finish_extraction(app, NK_OK, NULL);
    printf("[PLAYER] STAGING_RESULT status=PASS view=PLAYER_VIEW_READY_LIBRARY "
           "disc_id=%s assets=%u audio=%u visual=%u layout=%u runtime=%s\n",
           app->inspecting_game.disc_id,
           (unsigned)summary.extracted_asset_count,
           (unsigned)summary.extracted_audio_count,
           (unsigned)summary.extracted_visual_count,
           (unsigned)summary.extracted_layout_count,
           app->inspecting_game.is_prepared ? "ready" : "not-ready");
    return 0;
}

/* Bound on one headless --launch-index run. A guest that reaches its own exit
   ends the run sooner; this only caps a run that would otherwise wait forever. */
#define NK_HEADLESS_LAUNCH_TIMEOUT_MS 120000

static bool player_prerequisite_data_root(const PlayerApp *app,
                                          char *out, size_t out_size) {
    if (!app || !out || out_size == 0) return false;
    if (app->runtime_root[0]) {
        int n = snprintf(out, out_size, "%s", app->runtime_root);
        return n > 0 && (size_t)n < out_size;
    }
    return nk_platform_get_app_data_dir(out, out_size);
}

static void player_report_legacy_data_root(void) {
#if defined(_WIN32) || defined(_WIN64)
    static bool reported = false;
    if (reported) return;
    reported = true;

    char data_root[32768] = "";
    char legacy_root[32768];
    bool has_data_root = nk_platform_resolve_app_data_dir(data_root, sizeof(data_root));
    if (!nk_platform_get_legacy_app_data_dir(legacy_root, sizeof(legacy_root)) ||
        !nk_platform_dir_exists(legacy_root) ||
        (has_data_root && nk_platform_dir_exists(data_root))) {
        return;
    }
    fprintf(stderr,
        "LEGACY_DATA_DIR_FOUND: old_path=\"%s\" new_path=\"%s\". "
        "Move the old data manually; the player will not move or delete it.\n",
        legacy_root,
        has_data_root ? data_root : "unavailable; set LOCALAPPDATA or APPDATA");
#endif
}

static const char *player_prerequisite_display_name(const PlayerApp *app,
                                                     const char *item_id) {
    if (!app || !item_id) return item_id ? item_id : "";
    for (size_t i = 0; i < app->prerequisites.items.count; i++) {
        const PackagePrerequisite *item = &app->prerequisites.items.items[i];
        if (strcmp(item->id, item_id) == 0) return item->name;
    }
    return item_id;
}

static void player_start_prerequisite_operation(PlayerApp *app) {
    if (!app || app->prerequisite_job_started || app->prerequisite_fetcher_started) return;
    if (app->prerequisites.phase != PLAYER_PREREQ_BOOTSTRAP &&
        app->prerequisites.phase != PLAYER_PREREQ_DOWNLOAD) return;

    char data_root[NK_MAX_PATH];
    if (!player_prerequisite_data_root(app, data_root, sizeof(data_root))) {
        player_app_prereq_fail(app, "DATA_DIR_UNAVAILABLE",
            "DATA_DIR_UNAVAILABLE: Windows could not resolve Local AppData. Set LOCALAPPDATA or APPDATA and retry.");
        return;
    }

    if (app->prerequisites.phase == PLAYER_PREREQ_BOOTSTRAP) {
        const PackagePrerequisite *python_item = NULL;
        for (size_t i = 0; i < app->prerequisites.items.count; i++) {
            if (strcmp(app->prerequisites.items.items[i].id, "cpython-embed-amd64") == 0) {
                python_item = &app->prerequisites.items.items[i];
                break;
            }
        }
        if (!python_item || !package_builder_bootstrap_python_start(
                &app->bootstrap_session, python_item, data_root)) {
            player_app_prereq_fail(app, "PYTHON_BOOTSTRAP_START_FAILED",
                "The verified CPython bootstrap could not start. Check app-data permissions and retry.");
            return;
        }
        app->prerequisite_job_started = true;
        app->prerequisites.current_item[0] = '\0';
        app->prerequisites.item_received_bytes = 0;
        app->prerequisites.item_total_bytes = python_item->size_bytes;
        app->prerequisites.total_received_bytes = 0;
        return;
    }

    char python_path[NK_MAX_PATH];
    char cli_path[NK_MAX_PATH];
    if (!package_builder_find_python_in_root(data_root, python_path, sizeof(python_path))) {
        player_app_prereq_fail(app, "PYTHON_NOT_FOUND",
            "The verified CPython runtime is unavailable in app data. Retry the bootstrap download.");
        return;
    }
    if (!package_builder_find_cli(app->install_root, cli_path, sizeof(cli_path))) {
        player_app_prereq_fail(app, "CLI_NOT_FOUND",
            "The packaged source/tools folder could not be located. Repair the release source folder, then retry.");
        return;
    }
    char log_dir[NK_MAX_PATH];
    int n = snprintf(log_dir, sizeof(log_dir), "%s%clogs", data_root,
                     nk_platform_path_separator());
    if (n < 0 || (size_t)n >= sizeof(log_dir) || !nk_platform_mkdir_p(log_dir)) {
        player_app_prereq_fail(app, "DISK_FULL",
            "The app-data log folder could not be created. Free disk space or check permissions, then retry.");
        return;
    }
    NkResult result = package_builder_start_prerequisite_fetch(
        &app->build_session, python_path, cli_path, data_root, log_dir,
        app->prerequisite_bootstrap_ready);
    if (result != NK_OK) {
        player_app_prereq_fail(app,
            app->build_session.failure_code[0] ? app->build_session.failure_code : "PREREQUISITE_FETCH_START_FAILED",
            app->build_session.failure_boundary[0] ? app->build_session.failure_boundary :
                "The verified prerequisite fetcher could not start. Check app-data permissions and retry.");
        return;
    }
    app->prerequisite_fetcher_started = true;
    app->prerequisites.current_item[0] = '\0';
}

static void player_update_prerequisite_operation(PlayerApp *app) {
    if (!app) return;
    if (app->prerequisites.phase == PLAYER_PREREQ_BOOTSTRAP &&
        !app->prerequisite_job_started) {
        if (app->prerequisites.cancel_requested) {
            player_app_prereq_finish_cancel(app);
        } else {
            player_start_prerequisite_operation(app);
        }
    }
    if (app->prerequisites.phase == PLAYER_PREREQ_BOOTSTRAP &&
        app->prerequisite_job_started) {
        if (app->prerequisites.cancel_requested) {
            package_builder_bootstrap_python_cancel(&app->bootstrap_session);
        }
        uint64_t received = 0;
        bool finished = false;
        bool succeeded = false;
        char code[48] = "";
        char message[512] = "";
        package_builder_bootstrap_python_poll(&app->bootstrap_session, &received,
            &finished, &succeeded, code, sizeof(code), message, sizeof(message));
        player_app_prereq_update_progress(app,
            player_prerequisite_display_name(app, "cpython-embed-amd64"), received,
            app->prerequisites.item_total_bytes, received,
            app->prerequisites.total_bytes);
        if (finished && app->prerequisite_job_started) {
            package_builder_bootstrap_python_close(&app->bootstrap_session);
            app->prerequisite_job_started = false;
            if (succeeded) {
                app->prerequisite_bootstrap_ready = true;
                app->prerequisites.bootstrap_python = false;
                app->prerequisites.phase = PLAYER_PREREQ_DOWNLOAD;
                app->prerequisites.item_received_bytes = app->prerequisites.item_total_bytes;
                app->prerequisites.total_received_bytes = app->prerequisites.item_total_bytes;
            } else if (app->prerequisites.cancel_requested) {
                player_app_prereq_finish_cancel(app);
            } else {
                player_app_prereq_fail(app, code[0] ? code : "PYTHON_BOOTSTRAP_FAILED",
                    message[0] ? message : "The verified CPython runtime could not be installed. Check the connection and app-data space, then retry.");
            }
        }
    }

    if (app->prerequisites.phase == PLAYER_PREREQ_DOWNLOAD &&
        !app->prerequisite_fetcher_started) {
        if (app->prerequisites.cancel_requested) {
            player_app_prereq_finish_cancel(app);
        } else {
            player_start_prerequisite_operation(app);
        }
    }
    if (app->prerequisites.phase == PLAYER_PREREQ_DOWNLOAD &&
        app->prerequisite_fetcher_started) {
        if (app->prerequisites.cancel_requested && !app->prerequisite_cancel_sent) {
            package_builder_request_prerequisite_cancel(&app->build_session,
                app->build_session.cancel_file_path);
            app->prerequisite_cancel_sent = true;
        }
        package_builder_poll(&app->build_session, SDL_GetTicks());
        player_app_prereq_update_progress(app,
            player_prerequisite_display_name(app, app->build_session.current_item_id),
            app->build_session.item_received_bytes, app->build_session.item_total_bytes,
            app->build_session.total_received_bytes, app->build_session.total_bytes);
        if (!app->build_session.is_building) {
            app->prerequisite_fetcher_started = false;
            if (app->prerequisites.cancel_requested || app->build_session.is_cancelled ||
                strcmp(app->build_session.failure_code, "INSTALL_CANCELLED") == 0) {
                player_app_prereq_finish_cancel(app);
            } else if (app->build_session.is_failed) {
                player_app_prereq_fail(app,
                    app->build_session.failure_code[0] ? app->build_session.failure_code : "PREREQUISITE_INSTALL_FAILED",
                    app->build_session.failure_boundary[0] ? app->build_session.failure_boundary :
                        "The verified build prerequisites could not be installed. Check the error and retry.");
            } else if (app->build_session.is_complete &&
                       app->build_session.prerequisite_install_complete) {
                player_app_prereq_complete(app);
            } else {
                player_app_prereq_fail(app, "PREREQUISITE_INSTALL_FAILED",
                    "The prerequisite fetcher exited before confirming a complete install. Retry the download.");
            }
        }
    }
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

    player_report_legacy_data_root();

    PlayerApp app;
    player_app_init(&app);
    const char *executable_directory = SDL_GetBasePath();
    if (executable_directory) {
        player_app_discover_showcase(&app, executable_directory);
    }

    const char *screenshot_path = NULL;
    const char *test_view = NULL;
    const char *manifest_overlay_path = NULL;
    const char *initial_iso_path = NULL;
    const char *runtime_root_path = NULL;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    const char *ui_test_events = "";
    const char *ui_test_screenshot_path = NULL;
    const char *ui_test_error_code = NULL;
    const char *ui_test_iso_path = NULL;
    const char *ui_test_art_mount_source = NULL;
    bool ui_test_wait_background = false;
    bool ui_test_mode = false;
    bool ui_test_failed = false;
    bool ui_test_quit_queued = false;
    size_t ui_test_event_cursor = 0;
    int ui_test_frame = 0;
    bool ui_test_waiting_for_time = false;
    bool ui_test_waiting_for_view = false;
    bool ui_test_waiting_for_art_attempt = false;
    bool ui_test_waiting_for_art_delay = false;
    uint64_t ui_test_wait_art_delay_deadline = 0;
    unsigned ui_test_wait_art_attempt_target = 0;
    uint64_t ui_test_wait_deadline = 0;
    uint32_t ui_test_wait_view_mask = UINT32_C(1) << VIEW_LIBRARY;
    bool ui_test_fake_game_running = false;
#endif
    bool launch_now = false;
    bool stage_initial_iso = false;
    bool stage_only = false;
    bool launch_index_requested = false;
    int launch_index = -1;
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
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nakagawa_player [--iso=<path>] [--stage|--stage-only] "
                   "[--runtime-root=<path>] [--view=<name>] [--screenshot=<bmp>] "
                   "[--launch-index=N [--headless-launch]]\n");
            return 0;
        } else if (strncmp(argv[i], "--screenshot=", 13) == 0) {
            screenshot_path = argv[i] + 13;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        } else if (strncmp(argv[i], "--ui-test-events=", 17) == 0) {
            ui_test_events = argv[i] + 17;
            ui_test_mode = true;
        } else if (strncmp(argv[i], "--ui-test-screenshot=", 21) == 0) {
            ui_test_screenshot_path = argv[i] + 21;
            ui_test_mode = true;
        } else if (strncmp(argv[i], "--ui-test-error-code=", 21) == 0) {
            ui_test_error_code = argv[i] + 21;
            ui_test_mode = true;
        } else if (strncmp(argv[i], "--ui-test-iso-path=", 19) == 0) {
            ui_test_iso_path = argv[i] + 19;
            ui_test_mode = true;
        } else if (strncmp(argv[i], "--ui-test-art-mount-source=", 27) == 0) {
            ui_test_art_mount_source = argv[i] + 27;
            ui_test_mode = true;
        } else if (strcmp(argv[i], "--ui-test-wait-background") == 0) {
            ui_test_wait_background = true;
            ui_test_mode = true;
        } else if (strncmp(argv[i], "--ui-test-catalog-reload-path=", 30) == 0) {
            s_ui_test_catalog_reload_path = argv[i] + 30;
            ui_test_mode = true;
        } else if (strncmp(argv[i], "--ui-test-catalog-reload-count=", 31) == 0) {
            uint64_t count = 0;
            if (!player_ui_test_parse_positive_u64(argv[i] + 31, &count) ||
                count > 1024) {
                fprintf(stderr, "[PLAYER_UI_TEST] invalid catalog reload count.\n");
                ui_test_failed = true;
            } else {
                s_ui_test_catalog_reload_count = (unsigned)count;
            }
            ui_test_mode = true;
#endif
        } else if (strncmp(argv[i], "--view=", 7) == 0) {
            test_view = argv[i] + 7;
        } else if (strncmp(argv[i], "--manifest-overlay=", 19) == 0) {
            manifest_overlay_path = argv[i] + 19;
        } else if (strncmp(argv[i], "--iso=", 6) == 0) {
            initial_iso_path = argv[i] + 6;
        } else if (strncmp(argv[i], "--runtime-root=", 15) == 0) {
            runtime_root_path = argv[i] + 15;
        } else if (strncmp(argv[i], "--runtime-bin=", 14) == 0) {
            /* Keep the old spelling as a compatibility alias. The value is a
               resolver root, not necessarily a binary path. */
            runtime_root_path = argv[i] + 14;
        } else if (strcmp(argv[i], "--launch-now") == 0) {
            launch_now = true;
        } else if (strcmp(argv[i], "--stage") == 0) {
            stage_initial_iso = true;
        } else if (strcmp(argv[i], "--stage-only") == 0) {
            stage_initial_iso = true;
            stage_only = true;
        } else if (strncmp(argv[i], "--launch-index=", 15) == 0) {
            launch_index_requested = true;
            launch_index = atoi(argv[i] + 15);
        } else if (strncmp(argv[i], "--width=", 8) == 0) {
            override_w = atoi(argv[i] + 8);
        } else if (strncmp(argv[i], "--height=", 9) == 0) {
            override_h = atoi(argv[i] + 9);
        } else if (strcmp(argv[i], "--demo") == 0) {
            populate_sample = true;
        } else if (strcmp(argv[i], "--headless-launch") == 0) {
            app.launch_headless = true;
        } else if (strcmp(argv[i], "--empty") == 0) {
            force_empty = true;
        } else if (strcmp(argv[i], "--wizard") == 0) {
            force_empty = true;
            test_view = "wizard";
        }
    }

#ifdef NK_PLAYER_UI_REGRESSION_TEST
    {
        const char *fake_running = getenv("NK_UI_TEST_GAME_RUNNING");
        ui_test_fake_game_running = ui_test_mode && fake_running && fake_running[0];
    }
    const char *fail_package_thread_at =
        getenv("NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT");
    if (ui_test_mode && fail_package_thread_at) {
        if (!player_ui_test_parse_fail_attempts(fail_package_thread_at)) {
            fprintf(stderr,
                    "[PLAYER_UI_TEST] invalid package status thread fail attempt.\n");
            ui_test_failed = true;
        }
    }
    const char *fail_package_thread_for =
        getenv("NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_FOR");
    if (ui_test_mode && fail_package_thread_for &&
        !player_ui_test_parse_fail_titles(fail_package_thread_for)) {
        fprintf(stderr,
                "[PLAYER_UI_TEST] invalid package status thread fail title.\n");
        ui_test_failed = true;
    }
    if (ui_test_mode && getenv("NK_UI_TEST_DROP_WORKER_EVENT")) {
        /* Model SDL refusing a worker completion push, so the launcher's
           reclaim path is exercised by the same harness that drives the real
           event loop. */
        ui_test_drop_worker_event = true;
        ui_renderer_test_drop_art_events(true);
    }
    if ((s_ui_test_catalog_reload_count > 0) !=
        (s_ui_test_catalog_reload_path &&
         s_ui_test_catalog_reload_path[0])) {
        fprintf(stderr, "[PLAYER_UI_TEST] catalog reload path/count must be paired.\n");
        ui_test_failed = true;
    }
    if (ui_test_mode) setvbuf(stdout, NULL, _IONBF, 0);
#endif

    /* A --view= run is an explicit test or capture invocation, so it gets the
       fixture too. --empty wins regardless of argument order. */
    if (test_view && strcmp(test_view, "empty") != 0) {
        populate_sample = true;
    }
    if (force_empty) {
        populate_sample = false;
    }
    /* --launch-index names a library entry. It does not switch on the demo
       fixtures: an empty library is refused below unless --demo asks for them. */
    if (runtime_root_path) {
        player_app_set_runtime_root(&app, runtime_root_path);
    }
    {
        const char *base = SDL_GetBasePath();
        if (base) snprintf(app.install_root, sizeof(app.install_root), "%s", base);
    }
    if (stage_only && !initial_iso_path) {
        fprintf(stderr, "[PLAYER] --stage-only requires --iso=<path>.\n");
        return 2;
    }

    /* Title manifests the user keeps in <user data>/manifests are loaded on every
     * start, so a game that needs one is recognized without command-line flags. */
    {
        char user_data_root[NK_MAX_PATH];
        bool have_root = app.runtime_root[0]
            ? snprintf(user_data_root, sizeof(user_data_root), "%s", app.runtime_root) > 0
            : nk_platform_get_app_data_dir(user_data_root, sizeof(user_data_root));
        char manifest_dir[NK_MAX_PATH + 16];
        if (have_root && snprintf(manifest_dir, sizeof(manifest_dir), "%s%cmanifests",
                                  user_data_root, nk_platform_path_separator()) > 0) {
            char report[2048];
            int loaded = nk_title_manifest_load_overlay_dir(manifest_dir, report, sizeof(report));
            if (loaded > 0) printf("[PLAYER] Loaded %d title manifest(s) from %s\n", loaded, manifest_dir);
            if (report[0]) fprintf(stderr, "[PLAYER] Title manifests not loaded:\n%s", report);
        }
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
            char profile_error[256] = "";
            if (!player_populate_inspected_game(&app, initial_iso_path, &res,
                                                profile_error, sizeof(profile_error))) {
                fprintf(stderr, "[PLAYER] Could not create experimental profile: %s\n",
                        profile_error[0] ? profile_error : "unspecified profile error");
                return 4;
            }
            player_app_build_compatibility_preflight(&app, res.success,
                                                      res.param_sfo_parsed,
                                                      &res.executables);
            /* is_prepared intentionally NOT set: no preparation has run in this
             * build; the entry keeps the inspection-reported status. */

            if (stage_only && !res.is_supported) {
                fprintf(stderr, "[PLAYER] --stage-only refuses unsupported disc ID %s.\n",
                        app.inspecting_game.disc_id);
                return 3;
            }

            if (res.is_supported && stage_initial_iso) {
                /* --stage enters the same wizard state used by the file picker,
                   while --stage-only drives the same native worker synchronously
                   for CI/headless verification. */
                player_app_start_setup_wizard(&app);
                app.wizard.iso_selected = true;
                app.wizard.step = WIZARD_STEP_INSPECT_VERIFY;
                player_app_build_compatibility_preflight(&app, res.success,
                                                          res.param_sfo_parsed,
                                                          &res.executables);
                if (stage_only) {
                    return stage_iso_synchronously(&app);
                }
                app.wizard.is_extracting = true;
                app.wizard.extraction_requested = true;
                snprintf(app.wizard.status_message, sizeof(app.wizard.status_message),
                         "Extracting game assets into local application data...");
            } else {
            /* Catalogued titles and structurally identified experimental discs
               are offered on their card. The card's ADD TO LIBRARY stores them,
               as the file picker and the setup wizard already do. --launch-now
               is the one exception: it must launch, a launch needs a library
               record, and the wizard's LAUNCH GAME NOW likewise adds before it
               launches. Images without parsed PARAM.SFO remain refused on the
               unsupported view. */
            bool should_store = res.is_supported || app.inspecting_game.is_experimental;
            bool add_before_launch = should_store && launch_now &&
                                     !app.inspecting_game.is_experimental;
            bool stored = add_before_launch && player_app_add_game(&app, &app.inspecting_game);

            if (launch_now && app.inspecting_game.is_experimental) {
                const char *message =
                    "Launch now is unavailable for experimental titles. Generic ISO-to-Play "
                    "support is in the works. This title was not added; review its "
                    "compatibility checks before adding it to the library.";
                fprintf(stderr, "[PLAYER] EXPERIMENTAL_LAUNCH_UNAVAILABLE: %s\n", message);
                player_app_set_error(&app, "EXPERIMENTAL_LAUNCH_UNAVAILABLE",
                                     "Experimental Title Cannot Launch Yet", message,
                                     "Review Compatibility", VIEW_EXPERIMENTAL_TITLE);
            } else if (add_before_launch && !stored) {
                fprintf(stderr, "[PLAYER] Could not store %s in the library.\n",
                        app.inspecting_game.disc_id);
                player_app_set_error(&app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                     "The title could not be stored. The library may be full, "
                                     "or the user data directory is not writable.",
                                     "Return to Library", VIEW_LIBRARY);
            } else if (should_store) {
                player_app_set_view(&app, app.inspecting_game.is_experimental
                    ? VIEW_EXPERIMENTAL_TITLE : VIEW_SUPPORTED_TITLE);
                if (stored) {
                    printf("[PLAYER] Launching supported title now...\n");
                    const char *target_root = app.runtime_root[0] ? app.runtime_root : NULL;
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
                    /* --launch-now stands in for pressing PLAY NOW. The
                       launcher's default is headless for test harnesses, so
                       this inline path must explicitly request the window too. */
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
                            /* The disc is in the library now, so the card's
                               ADD TO LIBRARY would be stale: show the library,
                               as the wizard's LAUNCH GAME NOW does. */
                            player_app_set_view(&app, VIEW_LIBRARY);
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
                        if (lres == NK_ERROR_INVALID_EXECUTABLE) {
                            err_code = "STAGED_EXECUTABLE_INVALID";
                            err_title = "Staged Executable Is Invalid";
                        } else if (strstr(app.launch_session.last_error, "manifest") != NULL ||
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
            }
        } else {
            fprintf(stderr, "[PLAYER] Failed to inspect ISO: %s\n", initial_iso_path);
            return 1;
        }
    }

    /* Entries at or after this index were not loaded from library.json: they are
       the bundled demo samples (or prepared showcase demos) added below. */
    int user_library_count = app.library.count;
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
        } else if (strcmp(test_view, "experimental-library") == 0) {
            /* In-memory synthetic card for rendered UI regression; it is never
               admitted to the user's persistent library. */
            nk_library_init(&app.library);
            app.game_count = 0;
            app.selected_game_index = -1;
            GameRecord experimental;
            memset(&experimental, 0, sizeof(experimental));
            snprintf(experimental.disc_id, sizeof(experimental.disc_id), "TEST00007");
            snprintf(experimental.title_name, sizeof(experimental.title_name),
                     "Nakagawa Synthetic Experimental Fixture");
            snprintf(experimental.title_id, sizeof(experimental.title_id),
                     "synthetic-ui-test-v1");
            experimental.status = NK_STATUS_IDENTIFIED;
            experimental.is_experimental = true;
            app.games[0] = experimental;
            app.game_count = 1;
            app.selected_game_index = 0;
            player_app_set_view(&app, VIEW_LIBRARY);
        } else if (strcmp(test_view, "ready-library") == 0 ||
                   strcmp(test_view, "ready") == 0 ||
                   strcmp(test_view, "build-ready") == 0) {
            /* Synthetic capture fixture for the post-staging card. It is kept
               in memory only so the screenshot path never writes a fabricated
               title into the user's library. */
            app.game_count = 0;
            app.selected_game_index = -1;
            {
                GameRecord staged;
                memset(&staged, 0, sizeof(staged));
                snprintf(staged.disc_id, sizeof(staged.disc_id), "TEST00006");
                snprintf(staged.title_name, sizeof(staged.title_name),
                         "Nakagawa Display Smoke Fixture");
                snprintf(staged.disc_version, sizeof(staged.disc_version), "1.00");
                snprintf(staged.title_id, sizeof(staged.title_id), "display-smoke-v1");
                snprintf(staged.iso_path, sizeof(staged.iso_path),
                         "fixtures/display_smoke/generate.py");
                snprintf(staged.prepared_root, sizeof(staged.prepared_root),
                         "build/display-smoke-v1/staged");
                staged.status = NK_STATUS_SUPPORTED_PREPARATION;
                staged.assets_staged = true;
                staged.extracted_asset_count = 2361;
                staged.extracted_audio_count = 184;
                staged.extracted_visual_count = 642;
                staged.extracted_layout_count = 97;
                snprintf(staged.last_played, sizeof(staged.last_played), "Never");
                app.games[0] = staged;
                app.game_count = 1;
                app.selected_game_index = 0;
            }
            if (strcmp(test_view, "build-ready") == 0) {
                player_app_set_view(&app, VIEW_BUILDING_PACKAGE);
                package_builder_init_session(&app.build_session,
                                             app.games[0].disc_id,
                                             app.games[0].title_name);
                app.build_session.is_complete = true;
            } else {
                player_app_set_view(&app, PLAYER_VIEW_READY_LIBRARY);
            }
        } else if (strcmp(test_view, "inspecting") == 0) {
            player_app_set_view(&app, VIEW_INSPECTING);
        } else if (strcmp(test_view, "supported") == 0) {
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "TEST00001");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name), "Nakagawa Synthetic Allegrex Fixture");
            player_app_set_view(&app, VIEW_SUPPORTED_TITLE);
        } else if (strcmp(test_view, "experimental") == 0) {
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "TEST00007");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name),
                     "Nakagawa Synthetic Experimental Fixture");
            snprintf(app.inspecting_game.title_id, sizeof(app.inspecting_game.title_id),
                     "synthetic-ui-test-v1");
            snprintf(app.inspecting_game.selected_executable,
                     sizeof(app.inspecting_game.selected_executable), "EBOOT.BIN");
            app.inspecting_game.status = NK_STATUS_IDENTIFIED;
            app.inspecting_game.is_experimental = true;
            player_app_set_view(&app, VIEW_EXPERIMENTAL_TITLE);
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
        } else if (strcmp(test_view, "controller") == 0 || strcmp(test_view, "controller_settings") == 0) {
            player_app_set_view(&app, VIEW_CONTROLLER_SETTINGS);
        } else if (strcmp(test_view, "error") == 0) {
            player_app_set_error(&app, "SOURCE_NOT_FOUND", "Game Source File Not Found",
                                 "Nakagawa could not locate the source ISO file on disk.",
                                 "Locate Game ISO", VIEW_LIBRARY);
        } else if (strcmp(test_view, "focus") == 0) {
            /* set_view resets focus, so the focused card is chosen after it. */
            player_app_set_view(&app, VIEW_LIBRARY);
            app.focus_index = 1;
        } else if (strcmp(test_view, "wizard") == 0 || strcmp(test_view, "wizard1") == 0) {
            player_app_start_setup_wizard(&app);
        } else if (strcmp(test_view, "wizard2") == 0) {
            player_app_start_setup_wizard(&app);
            app.wizard.step = WIZARD_STEP_SELECT_GAME;
        } else if (strcmp(test_view, "wizard3") == 0) {
            player_app_start_setup_wizard(&app);
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "UCUS98701");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name), "Hot Shots Tennis: Get a Grip");
            snprintf(app.inspecting_game.disc_version, sizeof(app.inspecting_game.disc_version), "1.00");
            snprintf(app.inspecting_game.iso_path, sizeof(app.inspecting_game.iso_path), "games/tennis.iso");
            app.inspecting_game.status = NK_STATUS_VERIFIED;
            app.wizard.iso_selected = true;
            app.wizard.step = WIZARD_STEP_INSPECT_VERIFY;
        } else if (strcmp(test_view, "wizard-staging") == 0) {
            player_app_start_setup_wizard(&app);
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "TEST00008");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name),
                     "Nakagawa Synthetic Staging Fixture");
            snprintf(app.inspecting_game.iso_path, sizeof(app.inspecting_game.iso_path),
                     "fixtures/synthetic_staging.iso");
            app.inspecting_game.status = NK_STATUS_VERIFIED;
            app.wizard.iso_selected = true;
            app.wizard.step = WIZARD_STEP_INSPECT_VERIFY;
            app.wizard.is_extracting = true;
            app.wizard.extraction_percent = 67;
            app.wizard.files_extracted = 7;
            app.wizard.total_files = 12;
            snprintf(app.wizard.extraction_current_file,
                     sizeof(app.wizard.extraction_current_file),
                     "xbdata/menu.xb");
            snprintf(app.wizard.status_message, sizeof(app.wizard.status_message),
                     "Decoding the selected synthetic XB assets...");
        } else if (strcmp(test_view, "wizard4") == 0) {
            player_app_start_setup_wizard(&app);
            app.wizard.step = WIZARD_STEP_SYSTEM_FONTS;
        } else if (strcmp(test_view, "wizard5") == 0) {
            player_app_start_setup_wizard(&app);
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "UCUS98701");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name), "Hot Shots Tennis: Get a Grip");
            app.wizard.step = WIZARD_STEP_READY_LAUNCH;
        } else if (strcmp(test_view, "building") == 0 || strcmp(test_view, "building_package") == 0) {
            player_app_set_view(&app, VIEW_BUILDING_PACKAGE);
            const char *disc = (app.selected_game_index >= 0 && app.selected_game_index < app.game_count)
                ? app.games[app.selected_game_index].disc_id : "ULUS10041";
            const char *title = (app.selected_game_index >= 0 && app.selected_game_index < app.game_count)
                ? app.games[app.selected_game_index].title_name : "Street Supremacy";
            package_builder_init_session(&app.build_session, disc, title);
            app.build_session.is_building = true;
            app.build_session.current_stage = PACKAGE_BUILD_STAGE_COMPILE;
            snprintf(app.build_session.current_stage_name, sizeof(app.build_session.current_stage_name), "compile");
            snprintf(app.build_session.current_message, sizeof(app.build_session.current_message),
                     "Compiling translated native host C sources into package binary...");
            app.build_session.elapsed_ms = 1420;
            package_builder_add_output_line(&app.build_session, "[preflight] PASS: Disc preflight inspection succeeded.");
            package_builder_add_output_line(&app.build_session, "[extract] PASS: Staged plaintext guest modules to cache.");
            package_builder_add_output_line(&app.build_session, "[compile] RUNNING: Compiling translated native C sources...");
        }
    }

#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (ui_test_iso_path && app.selected_game_index >= 0 &&
        app.selected_game_index < app.game_count) {
        snprintf(app.games[app.selected_game_index].iso_path,
                 sizeof(app.games[app.selected_game_index].iso_path), "%s",
                 ui_test_iso_path);
    }
    if (ui_test_fake_game_running) app.is_game_running = true;
    if (ui_test_error_code) {
        if (strcmp(ui_test_error_code, "CLI_NOT_FOUND") == 0) {
            player_app_set_cli_not_found_error(&app, "Return to Library",
                                               VIEW_LIBRARY);
        } else if (strcmp(ui_test_error_code, "UI_TEST_LONG_ERROR") == 0) {
            static const char long_error_message[] =
                "Build setup could not continue because a required file was missing. "
                "Check the message, follow the suggested recovery steps, and try again. "
                "If the application files are incomplete, reinstall the app and keep "
                "its folder together. Return to the library after correcting the issue. "
                "This final sentence confirms the full message remains visible beyond "
                "the old four-line limit. Keep every recovery step on this card so the "
                "player can read the complete instruction before leaving this screen.";
            player_app_set_error(&app, ui_test_error_code,
                                 "Synthetic Long Error", long_error_message,
                                 "Return to Library", VIEW_LIBRARY);
        } else {
            player_app_set_error(&app, ui_test_error_code, ui_test_error_code,
                                 "Synthetic UI regression error state.",
                                 "Return to Library", VIEW_LIBRARY);
        }
    }
#endif

#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (ui_test_mode) app.settings.reduce_motion = true;
#endif

    app.window_width = override_w;
    app.window_height = override_h;

    /* Mechanical launch driver for the real player path. This deliberately
       stops before SDL initialization: the child runtime owns the game window,
       while the parent waits for its real exit status. A test can set
       SR_BOOT_EVENT_FILE to receive the child's window/first-frame milestones. */
    if (launch_index_requested) {
        if (app.library.count == 0 && !populate_sample) {
            fprintf(stderr, "[PLAYER] --launch-index=%d: your library is empty, so there is "
                    "nothing to launch. Add a game first, or pass --demo to launch the "
                    "bundled SAMPLE demo entries.\n", launch_index);
            return 2;
        }
        if (launch_index < 0 || launch_index >= app.game_count) {
            fprintf(stderr, "[PLAYER] Invalid --launch-index=%d for library count %d.\n",
                    launch_index, app.game_count);
            return 2;
        }
        app.selected_game_index = launch_index;
        const GameRecord *launch_game = &app.games[launch_index];
        /* Bundled demo entries are not loaded from library.json. */
        bool sample_entry = launch_index >= user_library_count;
        NkRuntimePackageStatus launch_status =
            player_app_validate_runtime_package(
                &app, launch_game, NULL, NULL, 0);
        bool launch_available = launch_status == NK_RUNTIME_PACKAGE_OK ||
            (launch_status == NK_RUNTIME_PACKAGE_MISSING &&
             !launch_game->is_experimental &&
             nk_launch_runtime_available(
                 app.runtime_root[0] ? app.runtime_root : NULL,
                 launch_game->title_id));
        if (!launch_available) {
            if (sample_entry) {
                fprintf(stderr, "[PLAYER] Launch index %d is a bundled SAMPLE demo, not in "
                        "library.json, and has no prepared runtime; PLAY NOW is unavailable.\n",
                        launch_index);
            } else {
                fprintf(stderr, "[PLAYER] Launch index %d has no prepared runtime; PLAY NOW is unavailable.\n",
                        launch_index);
            }
            return 3;
        }
        if (sample_entry) {
            printf("[PLAYER] Launch index %d is a bundled SAMPLE demo, not in library.json: "
                   "PLAY NOW available for %s (%s).\n",
                   launch_index, app.games[launch_index].disc_id,
                   app.games[launch_index].title_name);
        } else {
            printf("[PLAYER] Launch index %d: PLAY NOW available for %s (%s).\n",
                   launch_index, app.games[launch_index].disc_id,
                   app.games[launch_index].title_name);
        }
        if (!player_app_launch_game(&app, launch_index)) {
            fprintf(stderr, "[PLAYER] --launch-index failed: %s\n",
                    app.launch_session.last_error);
            return 4;
        }
        printf("[PLAYER] Launch argv contains --gui: %s\n",
               app.launch_session.argv_has_gui ? "yes" : "no");
        if (!app.launch_session.argv_has_gui && !app.launch_headless) {
            /* A windowed launch that lost its --gui request is a failure. */
            player_app_stop_game(&app);
            return 5;
        }
        if (!app.launch_session.argv_has_gui) {
            /* Headless launch: the same player-owned session, spawned without a
               window so a host with no display can still run it. The bound is
               the whole check -- a run that has not ended inside it is reported
               as a timeout with a distinct status, never as a launch. */
            int headless_code = nk_launch_wait(&app.launch_session,
                                               NK_HEADLESS_LAUNCH_TIMEOUT_MS);
            app.is_game_running = false;
            if (headless_code < 0) {
                fprintf(stderr,
                        "[PLAYER] Headless launch did not finish within %d ms; stopping the child.\n",
                        NK_HEADLESS_LAUNCH_TIMEOUT_MS);
                player_app_stop_game(&app);
                return 7;
            }
            printf("[PLAYER] Headless launch child exited with code %d.\n", headless_code);
            return headless_code;
        }
        int child_code = nk_launch_wait(&app.launch_session, -1);
        app.is_game_running = false;
        printf("[PLAYER] Launch-index child exited with code %d.\n", child_code);
        return child_code < 0 ? 6 : child_code;
    }

    /* Initialize SDL3 */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    bool interactive_window = screenshot_path == NULL
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        && !ui_test_mode
#endif
        ;
    SDL_DisplayID startup_display = 0;
    SDL_Rect usable_bounds = { 0, 0, 0, 0 };
    bool have_usable_bounds = false;
    float display_content_scale = 1.0f;
    PlayerWindowRect requested_window = {
        0, 0, app.window_width, app.window_height
    };
    PlayerWindowRect startup_window = requested_window;
    PlayerWindowFrame estimated_frame = { 0, 0, 0, 0 };
    int minimum_width = 640;
    int minimum_height = 480;
    if (interactive_window) {
        startup_display = player_window_display(&app.settings);
        have_usable_bounds = player_display_usable_bounds(startup_display,
                                                          &usable_bounds);
        display_content_scale = player_display_content_scale(startup_display);
        requested_window = player_requested_window(&app.settings,
                                                    display_content_scale);
        estimated_frame = player_estimated_window_frame(display_content_scale);
        if (have_usable_bounds) {
            PlayerWindowRect usable_rect = {
                usable_bounds.x, usable_bounds.y, usable_bounds.w, usable_bounds.h
            };
            if (!player_window_fit_to_display(
                    requested_window, usable_rect, estimated_frame,
                    app.settings.launcher_window_position_valid,
                    &startup_window)) {
                int fallback_width = usable_bounds.w -
                    estimated_frame.left - estimated_frame.right;
                int fallback_height = usable_bounds.h -
                    estimated_frame.top - estimated_frame.bottom;
                if (fallback_width < 1) fallback_width = 1;
                if (fallback_height < 1) fallback_height = 1;
                startup_window = (PlayerWindowRect){
                    usable_bounds.x, usable_bounds.y,
                    fallback_width, fallback_height
                };
            }
            int available_width = usable_bounds.w -
                estimated_frame.left - estimated_frame.right;
            int available_height = usable_bounds.h -
                estimated_frame.top - estimated_frame.bottom;
            int scaled_min_width = (int)(PLAYER_WINDOW_MIN_LOGICAL_WIDTH *
                                         display_content_scale + 0.5f);
            int scaled_min_height = (int)(PLAYER_WINDOW_MIN_LOGICAL_HEIGHT *
                                          display_content_scale + 0.5f);
            if (available_width < scaled_min_width) scaled_min_width = available_width;
            if (available_height < scaled_min_height) scaled_min_height = available_height;
            minimum_width = scaled_min_width < startup_window.width
                ? scaled_min_width : startup_window.width;
            minimum_height = scaled_min_height < startup_window.height
                ? scaled_min_height : startup_window.height;
            if (minimum_width < 1) minimum_width = 1;
            if (minimum_height < 1) minimum_height = 1;
        }
    }

    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (screenshot_path != NULL
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        || ui_test_mode
#endif
    ) {
        win_flags |= SDL_WINDOW_HIDDEN;
    }

    SDL_Window *window = SDL_CreateWindow("Nakagawa Recomp", startup_window.width,
                                          startup_window.height, win_flags);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    if (interactive_window && have_usable_bounds) {
        SDL_Rect usable = usable_bounds;
        player_apply_window_geometry(
            window, requested_window,
            (PlayerWindowRect){ usable.x, usable.y, usable.w, usable.h },
            estimated_frame, app.settings.launcher_window_position_valid,
            &startup_window);
    }
    /* Keep a useful minimum where the display permits it. On smaller displays
       the minimum shrinks to the available client area rather than covering
       the taskbar or extending onto a monitor that is no longer connected. */
    SDL_SetWindowMinimumSize(window, minimum_width, minimum_height);
    /* The window rectangle uses SDL's native screen units. Pixel density is
       only for the high-density renderer/font backing store. */
    app.dpi_scale = SDL_GetWindowPixelDensity(window);
    if (app.dpi_scale < 1.0f) app.dpi_scale = 1.0f;
    bool window_frame_measured = !interactive_window;
    if (interactive_window && have_usable_bounds) {
        window_frame_measured = player_refit_to_actual_frame(
            window, requested_window, usable_bounds,
            app.settings.launcher_window_position_valid, &startup_window);
    }

    bool applied_launcher_fullscreen = false;
    bool launcher_minimized_for_game = false;
    bool launcher_handoff_attempted = false;
    Uint32 launcher_restore_flags = 0;
    if (interactive_window && app.settings.launcher_fullscreen) {
        applied_launcher_fullscreen = SDL_SetWindowFullscreen(window, true);
        if (!applied_launcher_fullscreen) {
            app.settings.launcher_fullscreen = false;
            snprintf(app.settings_notice, sizeof(app.settings_notice),
                     "Could not restore launcher fullscreen mode: %.72s",
                     SDL_GetError());
        }
    } else if (interactive_window &&
               app.settings.launcher_window_maximized) {
        SDL_MaximizeWindow(window);
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    bool logical_presentation_set = interactive_window &&
        SDL_SetRenderLogicalPresentation(
            renderer, PLAYER_UI_LOGICAL_WIDTH, PLAYER_UI_LOGICAL_HEIGHT,
            SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (logical_presentation_set) {
        app.logical_ui = true;
        app.window_width = PLAYER_UI_LOGICAL_WIDTH;
        app.window_height = PLAYER_UI_LOGICAL_HEIGHT;
    } else if (interactive_window) {
        int actual_width = startup_window.width;
        int actual_height = startup_window.height;
        if (!SDL_GetWindowSize(window, &actual_width, &actual_height)) {
            actual_width = startup_window.width;
            actual_height = startup_window.height;
        }
        app.window_width = actual_width > 0 ? actual_width : PLAYER_UI_LOGICAL_WIDTH;
        app.window_height = actual_height > 0 ? actual_height : PLAYER_UI_LOGICAL_HEIGHT;
    }

    UiInput input;
    memset(&input, 0, sizeof(input));

#ifdef NK_PLAYER_UI_REGRESSION_TEST
    SDL_Texture *ui_test_target = NULL;
    if (ui_test_mode) {
        ui_test_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET,
                                           app.window_width, app.window_height);
        if (!ui_test_target || !SDL_SetRenderTarget(renderer, ui_test_target)) {
            fprintf(stderr, "[PLAYER_UI_TEST] Could not create SDL render target: %s\n",
                    SDL_GetError());
            if (ui_test_target) SDL_DestroyTexture(ui_test_target);
            ui_font_shutdown();
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }
#endif

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
            printf("[PLAYER] Checking child process runtime health reactively...\n");
            if (nk_launch_is_running(&app.launch_session)) {
                printf("[PLAYER] Child process is running. PID: %d\n", app.launch_session.process.process_id);
                nk_launch_stop(&app.launch_session);
                printf("[PLAYER] Child process stopped cleanly after the screenshot.\n");
            } else {
                int code = nk_launch_wait(&app.launch_session, 0);
                printf("[PLAYER] Child process exited with code %d\n", code);
            }
            app.is_game_running = false;
        }

        ui_font_shutdown();
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
    if (SDL_HasGamepad()) {
        int npads = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&npads);
        if (ids && npads > 0) {
            gamepad = SDL_OpenGamepad(ids[0]);
            if (gamepad) {
                const char *pad_name = SDL_GetGamepadName(gamepad);
                snprintf(app.settings.controller_name, sizeof(app.settings.controller_name),
                         "%s", pad_name ? pad_name : "Controller");
                app.settings.controller_connected = true;
            }
        }
        SDL_free(ids);
    }
    PlayerStagingJob *staging_job = NULL;
    PlayerPackageStatusJob *package_status_job = NULL;
    bool package_status_requested[MAX_LIBRARY_GAMES] = { false };
    bool package_status_force_requested[MAX_LIBRARY_GAMES] = { false };
    player_package_status_queue_all(&app, package_status_requested,
                                   package_status_force_requested);
    uint64_t last_package_cache_generation =
        app.runtime_package_cache_generation;
    int last_package_selection = app.selected_game_index;
    uint64_t last_package_scan_tick = SDL_GetTicks();
    char build_check_disc_id[MAX_DISC_ID_LEN] = "";
    uint64_t build_check_session_generation = 0;
    uint64_t build_check_cache_generation = 0;
    bool build_check_pending = false;
    (void)player_package_status_start_next(
        &app, &package_status_job, package_status_requested,
        package_status_force_requested, build_check_disc_id,
        build_check_session_generation, build_check_cache_generation,
        &build_check_pending);

    app.enable_focus_handoff = interactive_window;
    bool running = true;
    uint64_t last_tick = SDL_GetTicks();
    /* The first frame is rendered before the event wait. A bounded wait keeps
       process-exit monitoring alive while the user is idle; staging progress
       and normal input still wake the loop immediately. */
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    uint64_t first_frame_started_ns = ui_test_mode ? SDL_GetTicksNS() : 0;
    uint64_t first_frame_validation_count = ui_test_mode
        ? player_app_ui_test_validation_calls() : 0;
#endif
    ui_render_frame(renderer, &app, &input);
    if (interactive_window && have_usable_bounds && !window_frame_measured) {
        window_frame_measured = player_refit_to_actual_frame(
            window, requested_window, usable_bounds,
            app.settings.launcher_window_position_valid, &startup_window);
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    char *ui_test_drop_data = NULL;
    if (ui_test_mode) {
        uint64_t validations = player_app_ui_test_validation_calls();
        validations = validations >= first_frame_validation_count
            ? validations - first_frame_validation_count : validations;
        player_ui_test_report_frame(
            0, &app, renderer, true,
            SDL_GetTicksNS() - first_frame_started_ns, validations);
    }
#endif
    while (running && !app.should_quit) {
        /* A worker that could not queue its own completion has no other way
           to report it; apply its result before anything reads the pending
           state this frame. */
        player_package_status_recover_lost_handoff(
            &app, &package_status_job, package_status_requested,
            package_status_force_requested, build_check_disc_id,
            &build_check_session_generation,
            &build_check_cache_generation,
            &build_check_pending);
        ui_renderer_recover_lost_art_handoffs();
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        if (ui_test_mode) {
            uint64_t ui_test_now = SDL_GetTicks();
            bool ui_test_block_script = false;
            if (ui_test_waiting_for_view) {
                uint32_t current_view_mask = (unsigned)app.active_view < 32
                    ? UINT32_C(1) << (unsigned)app.active_view : 0;
                if (ui_test_wait_view_mask & current_view_mask) {
                    printf("[PLAYER_UI_TEST] wait_view actual=%s result=PASS\n",
                           player_ui_test_view_name(app.active_view));
                    ui_test_waiting_for_view = false;
                } else if (ui_test_now >= ui_test_wait_deadline) {
                    fprintf(stderr, "[PLAYER_UI_TEST] wait_view result=FAIL actual=%s\n",
                            player_ui_test_view_name(app.active_view));
                    ui_test_failed = true;
                    ui_test_quit_queued = true;
                    running = false;
                } else {
                    ui_test_block_script = true;
                }
            }
            if (ui_test_waiting_for_art_attempt) {
                unsigned completed = ui_renderer_test_art_attempt_count();
                if (completed >= ui_test_wait_art_attempt_target) {
                    printf("[PLAYER_UI_TEST] wait_art_attempt target=%u result=PASS\n",
                           ui_test_wait_art_attempt_target);
                    ui_test_waiting_for_art_attempt = false;
                } else {
                    ui_test_block_script = true;
                }
            }
            if (ui_test_waiting_for_art_delay) {
                if (ui_renderer_test_art_delay_active()) {
                    printf("[PLAYER_UI_TEST] wait_art_delay_active result=PASS\n");
                    ui_test_waiting_for_art_delay = false;
                } else if (ui_test_now >= ui_test_wait_art_delay_deadline) {
                    fprintf(stderr,
                            "[PLAYER_UI_TEST] wait_art_delay_active result=FAIL\n");
                    ui_test_failed = true;
                    ui_test_quit_queued = true;
                    ui_test_waiting_for_art_delay = false;
                    running = false;
                } else {
                    ui_test_block_script = true;
                }
            }
            if (s_ui_test_waiting_for_package_failure) {
                int failure_index = player_ui_test_fail_title_index(
                    s_ui_test_wait_package_failure_disc);
                unsigned completed = failure_index >= 0
                    ? s_ui_test_fail_package_status_thread_create_for_counts[
                        failure_index]
                    : 0;
                if (completed >= s_ui_test_wait_package_failure_target) {
                    printf("[PLAYER_UI_TEST] wait_package_failure disc=%s "
                           "target=%u result=PASS\n",
                           s_ui_test_wait_package_failure_disc,
                           s_ui_test_wait_package_failure_target);
                    s_ui_test_waiting_for_package_failure = false;
                } else {
                    ui_test_block_script = true;
                }
            }
            if (ui_test_waiting_for_time) {
                if (ui_test_now >= ui_test_wait_deadline) {
                    printf("[PLAYER_UI_TEST] wait_ms result=PASS\n");
                    ui_test_waiting_for_time = false;
                } else {
                    ui_test_block_script = true;
                }
            }
            if (!ui_test_block_script && !ui_test_quit_queued && running) {
            SDL_Event scripted_event;
            int next_event = player_ui_test_next_event(
                ui_test_events, &ui_test_event_cursor, &scripted_event);
            if (next_event > 0) {
                if (scripted_event.type == SDL_EVENT_USER &&
                    scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_ART_ATTEMPT) {
                    ui_test_wait_art_attempt_target = scripted_event.user.windowID;
                    ui_test_waiting_for_art_attempt = true;
                    printf("[PLAYER_UI_TEST] wait_art_attempt target=%u started\n",
                           ui_test_wait_art_attempt_target);
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_ART_DELAY_ACTIVE) {
                    ui_test_waiting_for_art_delay = true;
                    ui_test_wait_art_delay_deadline = ui_test_now + 10000;
                    printf("[PLAYER_UI_TEST] wait_art_delay_active started\n");
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_PACKAGE_FAILURE) {
                    s_ui_test_waiting_for_package_failure = true;
                    printf("[PLAYER_UI_TEST] wait_package_failure disc=%s "
                           "target=%u started\n",
                           s_ui_test_wait_package_failure_disc,
                           s_ui_test_wait_package_failure_target);
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_INVALIDATE_PACKAGE_CACHE) {
                    player_app_runtime_package_cache_invalidate(&app);
                    printf("[PLAYER_UI_TEST] invalidate_package_cache generation=%llu\n",
                           (unsigned long long)app.runtime_package_cache_generation);
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_MOUNT_ART_ISO) {
                    bool mounted = player_ui_test_copy_file(
                        ui_test_art_mount_source, ui_test_iso_path);
                    printf("[PLAYER_UI_TEST] mount_art_iso result=%s\n",
                           mounted ? "PASS" : "FAIL");
                    if (!mounted) {
                        ui_test_failed = true;
                        ui_test_quit_queued = true;
                        running = false;
                    }
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_SATURATE_ART_CACHE) {
                    if (!ui_renderer_test_saturate_art_cache(renderer, ui_test_iso_path)) {
                        ui_test_failed = true;
                    }
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_START_SYNTHETIC_BUILD) {
                    bool started = player_ui_test_start_synthetic_build(
                        &app, scripted_event.user.windowID);
                    printf("[PLAYER_UI_TEST] synthetic_build result=%s index=%u disc=%s\n",
                           started ? "PASS" : "FAIL",
                           scripted_event.user.windowID,
                           started ? app.games[app.selected_game_index].disc_id : "NONE");
                    if (!started) {
                        ui_test_failed = true;
                        ui_test_quit_queued = true;
                        running = false;
                    }
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_INVALIDATE_AND_START_SYNTHETIC_BUILD) {
                    player_app_runtime_package_cache_invalidate(&app);
                    s_ui_test_defer_cache_rescan_once = true;
                    bool started = player_ui_test_start_synthetic_build(
                        &app, scripted_event.user.windowID);
                    printf("[PLAYER_UI_TEST] invalidate_cache_and_synthetic_build result=%s index=%u disc=%s generation=%llu build_session_generation=%llu\n",
                           started ? "PASS" : "FAIL",
                           scripted_event.user.windowID,
                           started ? app.games[app.selected_game_index].disc_id : "NONE",
                           (unsigned long long)app.runtime_package_cache_generation,
                           (unsigned long long)app.build_session.session_generation);
                    if (!started) {
                        ui_test_failed = true;
                        ui_test_quit_queued = true;
                        running = false;
                    }
                } else if (scripted_event.type == SDL_EVENT_USER &&
                    scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_MS) {
                    ui_test_wait_deadline = ui_test_now + scripted_event.user.windowID;
                    ui_test_waiting_for_time = true;
                    printf("[PLAYER_UI_TEST] wait_ms=%u started\n",
                           scripted_event.user.windowID);
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_VIEW) {
                    ui_test_wait_view_mask = scripted_event.user.windowID;
                    ui_test_wait_deadline = ui_test_now + scripted_event.user.timestamp;
                    ui_test_waiting_for_view = true;
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_ASSERT_CONSENT) {
                    bool matches = app.active_view == VIEW_PREREQ_CONSENT &&
                        app.prerequisites.items.count == scripted_event.user.windowID &&
                        app.prerequisites.total_bytes == scripted_event.user.timestamp;
                    printf("[PLAYER_UI_TEST] assert_consent result=%s items=%zu expected_items=%u "
                           "total_bytes=%llu expected_bytes=%llu\n",
                           matches ? "PASS" : "FAIL",
                           app.prerequisites.items.count, scripted_event.user.windowID,
                           (unsigned long long)app.prerequisites.total_bytes,
                           (unsigned long long)scripted_event.user.timestamp);
                    if (!matches) {
                        ui_test_failed = true;
                        ui_test_quit_queued = true;
                        running = false;
                    }
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_ASSERT_VIEW) {
                    uint32_t current_view_mask = (unsigned)app.active_view < 32
                        ? UINT32_C(1) << (unsigned)app.active_view : 0;
                    bool matches = (scripted_event.user.windowID & current_view_mask) != 0;
                    printf("[PLAYER_UI_TEST] assert_view result=%s actual=%s\n",
                           matches ? "PASS" : "FAIL",
                           player_ui_test_view_name(app.active_view));
                    if (!matches) {
                        ui_test_failed = true;
                        ui_test_quit_queued = true;
                        running = false;
                    }
                } else if (scripted_event.type == SDL_EVENT_USER &&
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_ASSERT_GAME_RUNNING) {
                    printf("[PLAYER_UI_TEST] assert_game_running result=%s\n",
                           app.is_game_running ? "PASS" : "FAIL");
                    if (!app.is_game_running) {
                        ui_test_failed = true;
                        ui_test_quit_queued = true;
                        running = false;
                    }
                } else {
                    if (scripted_event.type == SDL_EVENT_DROP_FILE) {
                        ui_test_drop_data = (char *)scripted_event.drop.data;
                    }
                    if (!SDL_PushEvent(&scripted_event)) {
                        if (ui_test_drop_data) {
                            SDL_free(ui_test_drop_data);
                            ui_test_drop_data = NULL;
                        }
                        ui_test_failed = true;
                        running = false;
                        fprintf(stderr, "[PLAYER_UI_TEST] Could not queue synthetic SDL event.\n");
                    } else if (scripted_event.type == SDL_EVENT_QUIT) {
                        ui_test_quit_queued = true;
                    }
                }
            } else if (next_event == 0) {
                if (!(ui_test_wait_background &&
                      (package_status_job || ui_renderer_art_pending()))) {
                    memset(&scripted_event, 0, sizeof(scripted_event));
                    scripted_event.type = SDL_EVENT_QUIT;
                    if (!SDL_PushEvent(&scripted_event)) {
                        ui_test_failed = true;
                        running = false;
                        fprintf(stderr, "[PLAYER_UI_TEST] Could not queue synthetic quit.\n");
                    } else {
                        ui_test_quit_queued = true;
                    }
                }
            } else {
                ui_test_failed = true;
                running = false;
                fprintf(stderr, "[PLAYER_UI_TEST] Invalid synthetic event script near byte %llu.\n",
                        (unsigned long long)ui_test_event_cursor);
            }
            }
        }
#endif
        uint64_t now_tick = SDL_GetTicks();
        uint32_t delta_ms = (uint32_t)(now_tick >= last_tick ? (now_tick - last_tick) : 0);
        last_tick = now_tick;
        if (app.active_view == VIEW_CONTROLLER_SETTINGS && input_settings_is_capturing(&app.input_settings)) {
            input_settings_update_capture(&app.input_settings, delta_ms > 0 ? delta_ms : 1);
        }

        input.mouse_clicked = false;
        input.activate_pressed = false;
        bool close_request_handled_in_batch = false;
        SDL_Event event;
        bool event_available = SDL_WaitEventTimeout(&event, 50);
        if (event_available) {
            do {
                if (app.logical_ui &&
                    (event.type == SDL_EVENT_MOUSE_MOTION ||
                     event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                     event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                     event.type == SDL_EVENT_MOUSE_WHEEL ||
                     event.type == SDL_EVENT_FINGER_MOTION ||
                     event.type == SDL_EVENT_FINGER_DOWN ||
                     event.type == SDL_EVENT_FINGER_UP ||
                     event.type == SDL_EVENT_PEN_MOTION ||
                     event.type == SDL_EVENT_PEN_DOWN ||
                     event.type == SDL_EVENT_PEN_UP)) {
                    SDL_ConvertEventToRenderCoordinates(renderer, &event);
                }
                if (!player_dispatch_ui_event(&app, &input, window, &running,
                                              &close_request_handled_in_batch,
                                              &event)) {
                    switch (event.type) {
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
                    case SDL_EVENT_USER:
                        if (ui_renderer_handle_async_event(renderer, &event)) {
                            break;
                        }
                        if (package_status_job &&
                            event.user.data1 == package_status_job &&
                            event.user.code == PLAYER_PACKAGE_STATUS_EVENT_CODE) {
                            player_package_status_finish(
                                &app, &package_status_job,
                                package_status_requested,
                                package_status_force_requested,
                                build_check_disc_id,
                                &build_check_session_generation,
                                &build_check_cache_generation,
                                &build_check_pending);
                        } else if (event.user.data1 == staging_job) {
                            if (event.user.code == PLAYER_STAGING_EVENT_PROGRESS) {
                                sync_staging_progress(&app, staging_job);
                            } else if (event.user.code == PLAYER_STAGING_EVENT_COMPLETE) {
                                sync_staging_progress(&app, staging_job);
                                finish_staging_job(&app, staging_job);
                                player_package_status_queue_game(
                                    &app, app.selected_game_index, true,
                                    package_status_requested,
                                    package_status_force_requested);
                            }
                        }
                        break;
                    default:
                        break;
                    }
                }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
                if (ui_test_drop_data && event.type == SDL_EVENT_DROP_FILE &&
                    event.drop.data == ui_test_drop_data) {
                    SDL_free(ui_test_drop_data);
                    ui_test_drop_data = NULL;
                }
#endif
            } while (SDL_PollEvent(&event));
        }

        /* A renderer control asked for the host file dialog. The renderer has
           no window handle and must stay free of platform dialog calls, so the
           request is serviced here. */
        if (app.request_file_picker) {
#ifdef NK_PLAYER_UI_REGRESSION_TEST
            if (!ui_test_mode) {
#endif
            app.request_file_picker = false;
            trigger_file_picker(window, &app);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
            }
#endif
        }
        if (app.request_open_license_folder) {
            app.request_open_license_folder = false;
#if defined(_WIN32) || defined(_WIN64)
            if (app.requested_open_path[0]) {
                (void)ShellExecuteA(NULL, "open", app.requested_open_path,
                                    NULL, NULL, SW_SHOWNORMAL);
            }
#endif
        }

        /* Step 3 requests one worker; progress and completion return through
           SDL user events, so the UI thread never polls a job or performs ISO
           I/O. */
        if (player_app_wizard_take_extraction_request(&app)) {
            start_staging_job(&app, &staging_job);
            if (!staging_job) {
                player_package_status_queue_game(
                    &app, app.selected_game_index, true,
                    package_status_requested,
                    package_status_force_requested);
            }
        }
        if (staging_job && app.wizard.is_extracting &&
            player_app_wizard_cancel_requested(&app)) {
            request_staging_cancel(staging_job);
        }

        if (app.selected_game_index != last_package_selection) {
            last_package_selection = app.selected_game_index;
            player_package_status_queue_game(
                &app, app.selected_game_index, false,
                    package_status_requested, package_status_force_requested);
        }
        if (app.runtime_package_cache_generation !=
            last_package_cache_generation) {
            memset(package_status_requested, 0,
                   sizeof(package_status_requested));
            memset(package_status_force_requested, 0,
                   sizeof(package_status_force_requested));
#ifdef NK_PLAYER_UI_REGRESSION_TEST
            if (s_ui_test_defer_cache_rescan_once) {
                s_ui_test_defer_cache_rescan_once = false;
            } else
#endif
            player_package_status_queue_all(
                &app, package_status_requested,
                package_status_force_requested);
            last_package_cache_generation =
                app.runtime_package_cache_generation;
        }
        uint64_t package_scan_now = SDL_GetTicks();
        if (package_scan_now - last_package_scan_tick >= 2000) {
            player_package_status_queue_all(
                &app, package_status_requested,
                package_status_force_requested);
            last_package_scan_tick = package_scan_now;
        }
        player_update_prerequisite_operation(&app);
        if (player_app_prereq_take_resume(&app)) {
            int game_index = app.prerequisites.game_index;
            if (!player_app_start_package_build(&app, game_index)) {
                /* The state helper has already opened the actionable error card. */
            }
        }

        /* Clamp keyboard/gamepad focus before rendering so activation can
         * never target a control the current view no longer draws. */
        player_app_move_focus(&app, 0, ui_focus_count(&app));

        bool build_check_identity_changed = build_check_pending &&
            (!build_check_disc_id[0] ||
             !app.build_session.disc_id[0] ||
             strcmp(build_check_disc_id, app.build_session.disc_id) != 0 ||
             build_check_session_generation == 0 ||
             app.build_session.session_generation !=
                 build_check_session_generation);
        if (build_check_identity_changed) {
            /* Only a different build session or title releases the latch;
               cache epochs describe status identity, not build identity. */
            build_check_pending = false;
            build_check_disc_id[0] = '\0';
            build_check_session_generation = 0;
            build_check_cache_generation = 0;
        }

        /* Monitor background package build session */
        if (app.active_view == VIEW_BUILDING_PACKAGE) {
            package_builder_poll(&app.build_session, SDL_GetTicks());
            if (app.build_session.is_complete && !build_check_pending) {
                if (app.selected_game_index >= 0 &&
                    app.selected_game_index < app.game_count) {
                    snprintf(build_check_disc_id, sizeof(build_check_disc_id),
                             "%s",
                             app.games[app.selected_game_index].disc_id);
                    build_check_session_generation =
                        app.build_session.session_generation;
                    build_check_cache_generation =
                        app.runtime_package_cache_generation;
                    build_check_pending = true;
                    player_package_status_queue_game(
                        &app, app.selected_game_index, true,
                        package_status_requested,
                        package_status_force_requested);
                } else {
                    player_app_set_build_error(
                        &app, "package",
                        "Package completed without a selected library title.",
                        app.build_session.log_file_path);
                }
            } else if (app.build_session.is_failed) {
                player_app_set_build_error(&app,
                                           app.build_session.current_stage_name[0] ? app.build_session.current_stage_name : "build",
                                           app.build_session.failure_boundary[0] ? app.build_session.failure_boundary : "Package build failed.",
                                           app.build_session.log_file_path);
            } else if (app.build_session.is_cancelled) {
                player_app_set_view(&app, VIEW_LIBRARY);
            }
        }

        (void)player_package_status_start_next(
            &app, &package_status_job, package_status_requested,
            package_status_force_requested, build_check_disc_id,
            build_check_session_generation, build_check_cache_generation,
            &build_check_pending);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        if (package_status_job && package_status_job->catalog_test_start &&
            package_status_job->catalog_test_done &&
            !s_ui_test_catalog_reload_started) {
            s_ui_test_catalog_reload_started = true;
            for (unsigned i = 0; i < s_ui_test_catalog_reload_count; i++) {
                (void)SDL_SignalSemaphore(
                    package_status_job->catalog_test_start);
                char reload_error[256] = "";
                if (!nk_title_manifest_load_overlay_ext(
                        s_ui_test_catalog_reload_path, true, reload_error,
                        sizeof(reload_error))) {
                    s_ui_test_catalog_reload_failed = true;
                } else {
                    s_ui_test_catalog_reload_completed++;
                }
                (void)SDL_WaitSemaphore(package_status_job->catalog_test_done);
            }
        }
#endif

        /* Monitor running game process */
        bool child_exited = false;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        if (!(ui_test_mode && ui_test_fake_game_running)) {
#endif
            child_exited = player_app_monitor_game_session(&app, SDL_GetTicks());
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        }
#endif
        (void)child_exited;
        if (interactive_window && launcher_minimized_for_game &&
            !app.is_game_running) {
            SDL_RestoreWindow(window);
            if (app.settings.launcher_fullscreen) {
                SDL_SetWindowFullscreen(window, true);
            } else if (launcher_restore_flags & SDL_WINDOW_FULLSCREEN) {
                SDL_SetWindowFullscreen(window, false);
            }
            if (!app.settings.launcher_fullscreen &&
                (app.settings.launcher_window_maximized ||
                 (launcher_restore_flags & SDL_WINDOW_MAXIMIZED))) {
                SDL_MaximizeWindow(window);
            }
            SDL_RaiseWindow(window);
            launcher_minimized_for_game = false;
        }
        /* A later launch gets its own handoff attempt, including after a
           minimize that failed and left the launcher visible. */
        if (interactive_window && !app.is_game_running) {
            launcher_handoff_attempted = false;
        }
        /* Interactive launches must have a boot-event path and a real
           window_ready/first_frame marker before the launcher yields focus.
           Missing evidence keeps the launcher visible. The attempt is latched
           so a failed SDL_MinimizeWindow leaves the launcher visible without
           recapturing and rewriting the launcher settings on every frame. */
        if (player_app_should_attempt_window_handoff(
                interactive_window, app.is_game_running,
                launcher_handoff_attempted,
                app.launch_session.boot_event_file_path[0]
                    ? player_app_child_window_ready(&app)
                    : false)) {
            launcher_handoff_attempted = true;
            player_capture_window_settings(&app, window);
            player_app_save_settings(&app, NULL);
            launcher_restore_flags = SDL_GetWindowFlags(window);
            launcher_minimized_for_game = SDL_MinimizeWindow(window);
        }
        if (interactive_window) {
            player_apply_launcher_fullscreen(&app, window,
                                             &applied_launcher_fullscreen);
        }

        /* Live input sampling for controller settings monitor (#357) */
        if (gamepad) {
            for (int b = 0; b < NK_HOST_BUTTON_COUNT; b++) {
                app.host_buttons_live[b] = SDL_GetGamepadButton(gamepad, (SDL_GamepadButton)b);
            }
            for (int a = 0; a < NK_HOST_AXIS_COUNT; a++) {
                app.host_axes_live[a] = SDL_GetGamepadAxis(gamepad, (SDL_GamepadAxis)a);
            }
        } else {
            memset(app.host_buttons_live, 0, sizeof(app.host_buttons_live));
            memset(app.host_axes_live, 0, sizeof(app.host_axes_live));
        }

        /* Update guided analog calibration when active (#357) */
        if (app.active_view == VIEW_CONTROLLER_SETTINGS) {
            static uint64_t s_last_cal_tick = 0;
            uint64_t cur_tick = SDL_GetTicks();
            if (s_last_cal_tick == 0) s_last_cal_tick = cur_tick;
            uint32_t dt_ms = (uint32_t)(cur_tick - s_last_cal_tick);
            s_last_cal_tick = cur_tick;
            if (input_settings_is_calibrating(&app.input_settings)) {
                input_settings_update_calibration(&app.input_settings, dt_ms,
                                                  app.host_axes_live);
            }
        }

#ifdef NK_PLAYER_UI_REGRESSION_TEST
        uint64_t frame_started_ns = ui_test_mode ? SDL_GetTicksNS() : 0;
        uint64_t frame_validation_count = ui_test_mode
            ? player_app_ui_test_validation_calls() : 0;
#endif
        ui_render_frame(renderer, &app, &input);
        /* Consume renderer requests in the same frame that created them. A
           later event batch may change the selected title before the next
           loop iteration, which must not redirect this retry. */
        if (app.request_package_status_retry) {
            app.request_package_status_retry = false;
            player_app_runtime_package_cache_mark_explicit_retry(
                &app, app.selected_game_index);
            player_package_status_queue_game(
                &app, app.selected_game_index, true,
                package_status_requested, package_status_force_requested);
        }
        if (interactive_window) {
            player_apply_launcher_fullscreen(&app, window,
                                             &applied_launcher_fullscreen);
        }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        if (ui_test_mode) {
            ui_test_frame++;
            uint64_t validations = player_app_ui_test_validation_calls();
            validations = validations >= frame_validation_count
                ? validations - frame_validation_count : validations;
            player_ui_test_report_frame(ui_test_frame, &app, renderer,
                                       running && !app.should_quit,
                                       SDL_GetTicksNS() - frame_started_ns,
                                       validations);
            if (!running || app.should_quit) {
                bool captured = ui_test_screenshot_path &&
                    ui_capture_screenshot(renderer, ui_test_screenshot_path);
                printf("[PLAYER_UI_TEST] screenshot=%s width=%d height=%d\n",
                       captured ? "PASS" : "FAIL", app.window_width, app.window_height);
                if (!captured) ui_test_failed = true;
            }
        }
#endif
    }

    if (app.is_game_running) {
        player_app_stop_game(&app);
    }

    if (interactive_window) {
        player_capture_window_settings(&app, window);
        player_app_save_settings(&app, NULL);
    }
    if (app.prerequisite_job_started) {
        package_builder_bootstrap_python_cancel(&app.bootstrap_session);
        package_builder_bootstrap_python_close(&app.bootstrap_session);
        app.prerequisite_job_started = false;
    }
    if (app.prerequisite_fetcher_started) {
        package_builder_request_prerequisite_cancel(&app.build_session,
            app.build_session.cancel_file_path);
        while (app.build_session.is_building) {
            package_builder_poll(&app.build_session, SDL_GetTicks());
            if (app.build_session.is_building) {
                (void)nk_platform_wait_process(&app.build_session.process, 1000);
            }
        }
        app.prerequisite_fetcher_started = false;
    }

    destroy_staging_job(&staging_job);
    if (package_status_job) {
        if (package_status_job->thread) {
            SDL_WaitThread(package_status_job->thread, NULL);
            package_status_job->thread = NULL;
        }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        if (package_status_job->catalog_test_start) {
            SDL_DestroySemaphore(package_status_job->catalog_test_start);
            package_status_job->catalog_test_start = NULL;
        }
        if (package_status_job->catalog_test_done) {
            SDL_DestroySemaphore(package_status_job->catalog_test_done);
            package_status_job->catalog_test_done = NULL;
        }
#endif
        free(package_status_job);
        package_status_job = NULL;
    }

    if (gamepad) {
        SDL_CloseGamepad(gamepad);
        gamepad = NULL;
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (ui_test_target) {
        SDL_SetRenderTarget(renderer, NULL);
        SDL_DestroyTexture(ui_test_target);
    }
#endif
    ui_font_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (ui_test_mode && ui_test_failed) return 1;
#endif
    return 0;
}
