/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * The native player: the launcher window a user sees first.
 *
 * main() reads the command line, loads the user's library and settings
 * (player_state.c), opens the SDL3 window and runs the event loop. Each frame
 * draws the active screen: the library and title screens come from the new UI
 * in src/player/ui/, every other screen from ui_renderer.c (all of them with
 * --ui=legacy). Slow work runs on its own thread so the window stays
 * responsive:
 *   - staging copies a disc's files into the user's per-title folder
 *     (setup_staging.c, src/core/nk_iso.c);
 *   - the package jobs check and build a title's runtime package
 *     (package_builder.c, which runs `tools/nk_cli.py build-package`);
 *   - the launch job starts the built game as its own process
 *     (src/core/nk_launch.c).
 *
 * Code under NK_PLAYER_UI_REGRESSION_TEST drives these paths from scripted
 * events for the UI regression tests; the shipped player does not include it.
 */

#include "player_state.h"
#include "package_builder.h"
#include "iso_reader.h"
#include "ui_renderer.h"
#include "ui/player_ui_nk.h"
#include "nk_title_manifest.h"
#include "nk_ui_widgets.h"
#include "nk_disc_chd.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "setup_staging.h"
#include "nk_platform.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#endif

static const char k_invalid_disc_message[] = "This file isn't a PSP disc image.";

static bool player_view_is_library(PlayerView view) {
    return view == VIEW_LIBRARY || view == PLAYER_VIEW_READY_LIBRARY;
}

static bool player_populate_inspected_game(PlayerApp *app, const char *iso_path,
                                           const char *source_image_path,
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
    /* A compressed image keeps its own path for the user; every later step
       reads the expanded .iso in iso_path. */
    snprintf(game->source_image_path, sizeof(game->source_image_path), "%s",
             source_image_path ? source_image_path : "");
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

    char refused_profile[512] = "";
    bool profile_refused = player_app_title_profile_refusal_for_disc(
        app, result->disc_id, refused_profile, sizeof(refused_profile));
    if (!result->is_supported && result->param_sfo_parsed && !profile_refused) {
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

/* A picked file the player refuses by name: the wizard's status line, or the
   error card everywhere else. */
static void player_show_picked_refusal(PlayerApp *app, const char *code, const char *title,
                                       const char *message) {
    if (app->active_view == VIEW_SETUP_WIZARD) {
        player_app_wizard_reset_extraction(app);
        app->wizard.iso_selected = false;
        snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                 "%.48s: %.200s", code, message);
    } else {
        player_app_set_error(app, code, title, message, "Try Another File", VIEW_LIBRARY);
    }
}

/* A picked (or converted, or expanded) disc image: inspect it and continue in
   the view it was picked from. source_image_path names the compressed image an
   expanded .iso came from (NULL otherwise). */
static void player_take_picked_disc_from(PlayerApp *app, const char *selected_path,
                                         const char *source_image_path) {
    IsoInspectResult res;
    if (iso_inspect_file(selected_path, &res)) {
        char profile_error[256] = "";
        if (!player_populate_inspected_game(app, selected_path, source_image_path, &res,
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
        const bool named_boundary = res.boundary_code[0] != '\0';
        const char *message = res.error_message[0]
            ? res.error_message
            : "The selected file is not a valid ISO9660 disc image.";
        if (app->active_view == VIEW_SETUP_WIZARD && !named_boundary) {
            player_app_wizard_reset_extraction(app);
            app->wizard.iso_selected = false;
            snprintf(app->wizard.status_message, sizeof(app->wizard.status_message),
                     "%s", k_invalid_disc_message);
        } else if (named_boundary) {
            player_show_picked_refusal(app, res.boundary_code,
                                       player_disc_refusal_title(res.boundary_code), message);
        } else {
            player_app_set_error(app, "ISO_CORRUPT", "Not a PSP Disc Image",
                                 k_invalid_disc_message, "Try Another File", VIEW_LIBRARY);
        }
    }
}

static void player_take_picked_disc(PlayerApp *app, const char *selected_path) {
    player_take_picked_disc_from(app, selected_path, NULL);
}

/* A compressed disc image (.cso, .zso, .chd): expanded once into a private ISO
   on the conversion worker, then imported exactly as a picked .iso. An
   expansion of the same unchanged file is used as it is; anything the player
   cannot read is refused by name. */
static void player_take_picked_compressed(PlayerApp *app, const char *path) {
    PlayerPsnPackage package;
    char code[48];
    char message[512];
    if (!player_disc_image_inspect(path, &package, code, sizeof(code), message,
                                   sizeof(message))) {
        player_show_picked_refusal(app, code, player_disc_refusal_title(code), message);
        return;
    }
    if (package.iso_ready) {
        printf("[PLAYER] Disc image %s: using its expanded disc image\n",
               nk_disc_format_extension(package.disc_format));
        player_take_picked_disc_from(app, package.iso_path, package.pbp_path);
        return;
    }
    printf("[PLAYER] Disc image %s: expanding it into a plain disc image\n",
           nk_disc_format_extension(package.disc_format));
    player_app_psn_conversion_begin(app, &package);
}

/* A PSN release of a UMD game (#295 phase 2): converted once into a private
   ISO on the conversion worker, then imported exactly as a picked disc. A
   conversion already in place is used as it is; other packages are refused by
   name. */
static void player_take_picked_package(PlayerApp *app, const char *path) {
    PlayerPsnPackage package;
    char code[48];
    char message[512];
    if (!player_psn_package_inspect(path, &package, code, sizeof(code), message,
                                    sizeof(message))) {
        player_show_picked_refusal(app, code, "Package Not Supported", message);
        return;
    }
    if (package.iso_ready) {
        printf("[PLAYER] PSN package %s: using its converted disc image\n", package.disc_id);
        player_take_picked_disc(app, package.iso_path);
        return;
    }
    printf("[PLAYER] PSN package %s: converting it into a disc image\n", package.disc_id);
    player_app_psn_conversion_begin(app, &package);
}

static void SDLCALL on_file_dialog_callback(void *userdata, const char * const *filelist, int filter) {
    (void)filter;
    PlayerApp *app = (PlayerApp *)userdata;
    if (!app || !filelist || !filelist[0]) {
        return;
    }
    if (app->wizard.is_extracting || app->psn_conversion_active) {
        /* A second file cannot replace the source while a worker owns the
           current staging transaction or conversion. */
        return;
    }
    const char *selected_path = filelist[0];
    printf("[PLAYER] ISO Selected: %s\n", selected_path);
    player_app_remember_picker_folder(app, selected_path, false);
    if (player_path_is_pbp_package(selected_path)) {
        player_take_picked_package(app, selected_path);
        return;
    }
    if (nk_disc_image_format(selected_path) != NK_DISC_FORMAT_NONE) {
        player_take_picked_compressed(app, selected_path);
        return;
    }
    player_take_picked_disc(app, selected_path);
}

/* Where a picker opens (player_app_picker_start_folder). SDL keeps reading
   the filters, and may read this, until the dialog's callback runs, so
   neither lives on the stack. */
static char s_picker_start[NK_MAX_PATH];

static const char *player_picker_start(const PlayerApp *app) {
    return player_app_picker_start_folder(app, s_picker_start, sizeof(s_picker_start))
        ? s_picker_start : NULL;
}

static void trigger_file_picker(SDL_Window *window, PlayerApp *app) {
    static const SDL_DialogFileFilter filters[] = {
        { "PSP Disc Images and PSN Packages (*.iso, *.cso, *.zso, *.chd, EBOOT.PBP)",
          "iso;cso;zso;chd;pbp" },
        { "All Files (*.*)", "*" }
    };
    SDL_ShowOpenFileDialog(on_file_dialog_callback, app, window, filters, 2,
                           player_picker_start(app), false);
}

/* The PSP fonts import. The picker shows the .pgf files themselves, which a
   folder picker hides, so the user can see they are in the right folder; any
   one of them names the folder the import reads. A cancelled dialog arrives
   with no entry and changes nothing. */
static void on_font_files_dialog_callback(void *userdata, const char *const *filelist, int filter) {
    PlayerApp *app = (PlayerApp *)userdata;
    char folder[NK_MAX_PATH];
    (void)filter;
    if (!app) return;
    if (!filelist || !filelist[0]) return;
    if (!player_path_parent_folder(filelist[0], folder, sizeof(folder))) return;
    player_app_remember_picker_folder(app, folder, true);
    (void)player_app_fonts_import_folder(app, folder);
}

static void trigger_font_folder_picker(SDL_Window *window, PlayerApp *app) {
    static const SDL_DialogFileFilter filters[] = {
        { "PSP fonts (*.pgf)", "pgf" }
    };
    SDL_ShowOpenFileDialog(on_font_files_dialog_callback, app, window, filters, 1,
                           player_picker_start(app), true);
}

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

/* Records the desktop bounds of the display that holds the launcher window, read
 * just before a launch so the game window opens on that display (the runtime gets
 * them as SR_LAUNCHER_DISPLAY_BOUNDS). The child cannot use our SDL_DisplayID: IDs
 * are per process. Unknown bounds are left zero and the game uses the primary. */
static void player_note_launcher_display(PlayerApp *app, SDL_Window *window) {
    if (!app) return;
    memset(app->launcher_display_bounds, 0, sizeof(app->launcher_display_bounds));
    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : 0;
    SDL_Rect bounds;
    if (!display || !SDL_GetDisplayBounds(display, &bounds) || bounds.w <= 0 ||
        bounds.h <= 0) {
        fprintf(stderr, "[PLAYER] Launcher display unknown (%s); the game opens on the "
                        "primary display.\n", SDL_GetError());
        return;
    }
    app->launcher_display_bounds[0] = bounds.x;
    app->launcher_display_bounds[1] = bounds.y;
    app->launcher_display_bounds[2] = bounds.w;
    app->launcher_display_bounds[3] = bounds.h;
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
        requested.width = (int)(PLAYER_UI_DESIGN_WIDTH * content_scale + 0.5f);
        requested.height = (int)(PLAYER_UI_DESIGN_HEIGHT * content_scale + 0.5f);
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

#if !defined(_WIN32) && !defined(_WIN64)
/* The app icon (the N1 mark on its ink tile) as the window icon: 32 px, with
   larger copies SDL picks for high-density displays. Windows needs no call: the
   executable carries the icon (assets/brand/nakagawa.rc) and SDL gives every
   window its sizes, where SDL_SetWindowIcon would stretch one bitmap to all. */
static void player_set_window_icon(SDL_Window *window) {
    static const int sizes[] = { 32, 48, 64, 128, 256 };
    SDL_Surface *icon = NULL;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        SDL_Surface *image = SDL_CreateSurface(sizes[i], sizes[i], SDL_PIXELFORMAT_RGBA32);
        if (!image || !nk_ui_app_icon_rgba(sizes[i], (unsigned char *)image->pixels,
                                           (size_t)image->pitch)) {
            SDL_DestroySurface(image);
            break;
        }
        if (!icon) {
            icon = image;
        } else {
            SDL_AddSurfaceAlternateImage(icon, image);
            SDL_DestroySurface(image);
        }
    }
    if (!icon || !SDL_SetWindowIcon(window, icon)) {
        fprintf(stderr, "The window icon was not set: %s\n", SDL_GetError());
    }
    SDL_DestroySurface(icon);
}
#endif

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

    /* Short labels: SDL3's Windows box does not widen a button for a long one
       (PLAYER_DIALOG_BUTTON_LABEL_MAX). Cancel stays the Return and Escape
       default. */
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT |
              SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
          0, PLAYER_QUIT_DIALOG_CANCEL_LABEL },
        { 0, 1, PLAYER_QUIT_DIALOG_QUIT_LABEL }
    };
    char message[NK_MAX_TITLE_LEN + 128];
    player_app_quit_confirmation_message(app, message, sizeof(message));
    const SDL_MessageBoxData dialog = {
        SDL_MESSAGEBOX_WARNING,
        window,
        PLAYER_QUIT_DIALOG_TITLE,
        message,
        (int)(sizeof(buttons) / sizeof(buttons[0])),
        buttons,
        NULL
    };
    int button_id = 0;
    bool dialog_shown = false;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    /* The headless regression cannot open a modal box, so it reads the dialog
       the host would show from these lines; the message's line break is
       written as \n to keep it on one. */
    printf("[PLAYER_UI_TEST] QUIT_DIALOG title=%s\n", dialog.title);
    printf("[PLAYER_UI_TEST] QUIT_DIALOG message=");
    for (const char *ch = dialog.message; *ch; ++ch) {
        if (*ch == '\n') fputs("\\n", stdout);
        else putchar(*ch);
    }
    putchar('\n');
    for (int i = 0; i < dialog.numbuttons; ++i) {
        const SDL_MessageBoxButtonData *button = &dialog.buttons[i];
        printf("[PLAYER_UI_TEST] QUIT_DIALOG button id=%d return_default=%d "
               "escape_default=%d label=%s\n",
               button->buttonID,
               (button->flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) != 0,
               (button->flags & SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT) != 0,
               button->text);
    }
    fflush(stdout);
    if (!getenv("NK_UI_TEST_MESSAGEBOX_FAIL")) {
        dialog_shown = SDL_ShowMessageBox(&dialog, &button_id);
    }
#else
    /* The box is owned by the launcher window: bring that to the front first,
       or the box opens behind the running game's window. */
    if (window != NULL) {
        (void)SDL_RestoreWindow(window);
        (void)SDL_RaiseWindow(window);
    }
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
/* Select is held: with R it toggles the developer log panel. */
static bool s_pad_select_held;

static bool player_dispatch_ui_event(PlayerApp *app, UiInput *input,
                                     PlayerUiNk *next_ui,
                                     SDL_Window *window, bool *running,
                                     bool *close_request_handled_in_batch,
                                     const SDL_Event *event) {
    bool use_next_ui = player_ui_nk_view_active(next_ui, app);
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
        if (use_next_ui) return player_ui_nk_handle_event(next_ui, event);
        input->mouse_x = (int)event->motion.x;
        input->mouse_y = (int)event->motion.y;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (use_next_ui) return player_ui_nk_handle_event(next_ui, event);
        if (event->button.button == SDL_BUTTON_LEFT) {
            input->mouse_down = true;
            input->mouse_clicked = true;
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (use_next_ui) return player_ui_nk_handle_event(next_ui, event);
        if (event->button.button == SDL_BUTTON_LEFT) input->mouse_down = false;
        return true;
    case SDL_EVENT_KEY_DOWN:
        if (app->active_view == VIEW_CONTROLLER_SETTINGS &&
            app->input_settings.device == INPUT_DEVICE_KEYBOARD &&
            input_settings_is_capturing(&app->input_settings) &&
            event->key.key != SDLK_ESCAPE) {
            /* While a PSP button waits for a key, the next key pressed is its
               key, by position (the scancode). Esc still stops the wait below. */
            if (!event->key.repeat) {
                (void)input_settings_feed_capture_key(&app->input_settings,
                                                      (int)event->key.scancode);
            }
            return true;
        }
        if (!event->key.repeat && event->key.key == SDLK_F12 &&
            app->settings.developer_mode) {
            /* The developer log panel, from any launcher view. */
            if (app->active_view == VIEW_DEVELOPER_LOG) {
                player_app_close_developer_log(app);
            } else {
                (void)player_app_open_developer_log(app);
            }
            return true;
        }
        if (!event->key.repeat &&
            (event->key.key == SDLK_F11 ||
             ((event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER) &&
              (event->key.mod & SDL_KMOD_ALT)))) {
            player_app_toggle_launcher_fullscreen(app);
        } else if (use_next_ui && !event->key.repeat && event->key.key == SDLK_O &&
                   !app->wizard.is_extracting) {
            /* The library's shortcuts work the same in the next UI: O adds a
               disc image, S opens Settings, Esc at the library asks to quit. */
            app->request_file_picker = true;
        } else if (use_next_ui && !event->key.repeat && event->key.key == SDLK_S &&
                   player_view_is_library(app->active_view)) {
            player_app_set_view(app, VIEW_SETTINGS);
        } else if (use_next_ui && !event->key.repeat && event->key.key == SDLK_ESCAPE &&
                   player_view_is_library(app->active_view) &&
                   player_ui_nk_at_library_root(next_ui)) {
            if (player_confirm_quit(window, app)) *running = false;
        } else if (use_next_ui && !(event->key.key == SDLK_ESCAPE &&
                                    !player_view_is_library(app->active_view))) {
            return player_ui_nk_handle_event(next_ui, event);
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
            } else if (app->active_view == VIEW_CONFIRM_REMOVE_TOOLS) {
                player_app_set_view(app, VIEW_BUILD_CACHE);
            } else if (app->active_view == VIEW_PREREQ_ABOUT ||
                       app->active_view == VIEW_BUILD_CACHE ||
                       app->active_view == VIEW_PSP_FONTS ||
                       app->active_view == VIEW_UPDATES ||
                       app->active_view == VIEW_CONFIRM_FACTORY_RESET ||
                       app->active_view == VIEW_ONLINE_SETTINGS) {
                player_app_set_view(app, VIEW_SETTINGS);
            } else if (app->active_view == VIEW_DEVELOPER_LOG) {
                player_app_close_developer_log(app);
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
    case SDL_EVENT_KEY_UP:
        if (use_next_ui) return player_ui_nk_handle_event(next_ui, event);
        return false;
    case SDL_EVENT_MOUSE_WHEEL:
        if (use_next_ui) return player_ui_nk_handle_event(next_ui, event);
        if (player_view_is_library(app->active_view) && event->wheel.y != 0.0f) {
            player_app_move_selection(app, event->wheel.y > 0.0f ? -1 : 1);
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        /* A trigger waiting to be bound is the binding in both launchers. */
        if (use_next_ui && !(app->active_view == VIEW_CONTROLLER_SETTINGS &&
                             input_settings_is_capturing(&app->input_settings))) {
            return player_ui_nk_handle_event(next_ui, event);
        }
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
        if (event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
            s_pad_select_held = true;
        } else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER &&
                   s_pad_select_held && app->settings.developer_mode) {
            /* Select + R: the developer log panel. */
            if (app->active_view == VIEW_DEVELOPER_LOG) {
                player_app_close_developer_log(app);
            } else {
                (void)player_app_open_developer_log(app);
            }
            return true;
        }
        if (use_next_ui && event->gbutton.button == SDL_GAMEPAD_BUTTON_START &&
            player_view_is_library(app->active_view)) {
            player_app_set_view(app, VIEW_SETTINGS);
            return true;
        }
        /* While a control waits to be bound, the next press is its binding, in
           both launchers. */
        if (app->active_view == VIEW_CONTROLLER_SETTINGS &&
            input_settings_is_capturing(&app->input_settings)) {
            NkBindingSource src;
            if (app->input_settings.device == INPUT_DEVICE_KEYBOARD) {
                /* A controller has no keys: its press stops the wait for one. */
                input_settings_cancel_capture(&app->input_settings);
                return true;
            }
            src.type = NK_BINDING_HOST_BUTTON;
            src.index = event->gbutton.button;
            input_settings_feed_capture_source(&app->input_settings, src);
            return true;
        }
        if (use_next_ui && !(event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST &&
                             !player_view_is_library(app->active_view))) {
            return player_ui_nk_handle_event(next_ui, event);
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
            } else if (app->active_view == VIEW_CONFIRM_REMOVE_TOOLS) {
                player_app_set_view(app, VIEW_BUILD_CACHE);
            } else if (app->active_view == VIEW_PREREQ_ABOUT ||
                       app->active_view == VIEW_BUILD_CACHE ||
                       app->active_view == VIEW_PSP_FONTS ||
                       app->active_view == VIEW_UPDATES ||
                       app->active_view == VIEW_CONFIRM_FACTORY_RESET ||
                       app->active_view == VIEW_ONLINE_SETTINGS) {
                player_app_set_view(app, VIEW_SETTINGS);
            } else if (app->active_view == VIEW_DEVELOPER_LOG) {
                player_app_close_developer_log(app);
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
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) s_pad_select_held = false;
        if (use_next_ui) return player_ui_nk_handle_event(next_ui, event);
        return false;
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_PEN_MOTION:
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        if (use_next_ui) return player_ui_nk_handle_event(next_ui, event);
        return false;
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

static bool player_ui_save_folder_path(const GameRecord *game,
                                      char *out, size_t out_size) {
    char saves_root[NK_MAX_PATH];
    const char *slot;
    int written;
    if (game == NULL || out == NULL || out_size == 0u ||
        !nk_platform_get_path(NK_PATH_SAVES, saves_root, sizeof(saves_root))) {
        return false;
    }
    slot = game->disc_id[0] != '\0' ? game->disc_id
         : (game->title_id[0] != '\0' ? game->title_id : "unidentified");
    written = snprintf(out, out_size, "%s%c%s", saves_root,
                       nk_platform_path_separator(), slot);
    if (written <= 0 || (size_t)written >= out_size) return false;
    return nk_platform_mkdir_p(out);
}

#if !defined(_WIN32) && !defined(_WIN64)
static bool player_ui_file_url(const char *path, char *out, size_t out_size) {
    static const char prefix[] = "file://";
    size_t used = sizeof(prefix) - 1u;
    if (path == NULL || path[0] != '/' || out == NULL ||
        out_size <= used) {
        return false;
    }
    memcpy(out, prefix, used);
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        unsigned char c = *p;
        bool safe = isalnum(c) || c == '/' || c == '-' || c == '_' ||
                    c == '.' || c == '~' || c == ':';
        if (safe) {
            if (used + 1u >= out_size) return false;
            out[used++] = (char)c;
        } else {
            if (used + 3u >= out_size) return false;
            static const char hex[] = "0123456789ABCDEF";
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 15u];
        }
    }
    out[used] = '\0';
    return true;
}
#endif

static void player_ui_open_save_folder(PlayerApp *app, size_t title_index) {
    char path[NK_MAX_PATH];
    if (app == NULL || title_index >= (size_t)app->game_count ||
        !player_ui_save_folder_path(&app->games[title_index],
                                    path, sizeof(path))) {
        fprintf(stderr, "[PLAYER] The save folder could not be opened.\n");
        return;
    }
#if defined(_WIN32) || defined(_WIN64)
    (void)ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#else
    {
        char url[NK_MAX_PATH * 3u];
        if (!player_ui_file_url(path, url, sizeof(url)) || !SDL_OpenURL(url)) {
            fprintf(stderr, "[PLAYER] The save folder could not be opened: %s\n",
                    SDL_GetError());
            player_app_set_folder_open_error(app, path, SDL_GetError());
        }
    }
#endif
}

/* Saves the inspected title to the library and selects it, as the existing
   wizard's Ready step does. Returns the title's index, or -1. */
static int player_ui_wizard_save_title(PlayerApp *app) {
    int idx;
    if (app->inspecting_game.disc_id[0] == '\0') return -1;
    player_app_add_game(app, &app->inspecting_game);
    idx = player_app_find_game_by_disc_id(app, app->inspecting_game.disc_id);
    if (idx >= 0) app->selected_game_index = idx;
    return idx;
}

/* The setup wizard's controls: the same handlers the existing renderer's
   wizard buttons call (render_setup_wizard in ui_renderer.c). */
static void player_ui_wizard_apply(PlayerApp *app, PlayerUiWizardOp op) {
    switch (op) {
        case PLAYER_UI_WIZARD_NEXT:
            player_app_wizard_next(app);
            break;
        case PLAYER_UI_WIZARD_BACK:
            player_app_wizard_back(app);
            break;
        case PLAYER_UI_WIZARD_CANCEL:
            player_app_wizard_cancel(app);
            break;
        case PLAYER_UI_WIZARD_CHOOSE_ISO:
            app->request_file_picker = true;
            break;
        case PLAYER_UI_WIZARD_CHOOSE_DIFFERENT:
            app->wizard.step = WIZARD_STEP_SELECT_GAME;
            app->request_file_picker = true;
            break;
        case PLAYER_UI_WIZARD_ADD_EXPERIMENTAL:
            if (player_app_add_game(app, &app->inspecting_game)) {
                player_app_set_view(app, VIEW_LIBRARY);
            } else {
                player_app_set_error(app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                     "The experimental title could not be stored. The library "
                                     "may be full, or the user data directory is not writable.",
                                     "Return to Library", VIEW_LIBRARY);
            }
            break;
        case PLAYER_UI_WIZARD_ACCEPT_FONTS:
            app->wizard.font_confirmed = true;
            player_app_wizard_next(app);
            break;
        case PLAYER_UI_WIZARD_CYCLE_RESOLUTION:
            player_app_cycle_resolution_scale(app, 1);
            break;
        case PLAYER_UI_WIZARD_IMPORT_FONTS:
            app->request_font_folder_picker = true;
            break;
        case PLAYER_UI_WIZARD_REMOVE_FONTS:
            player_app_fonts_remove_imports(app);
            break;
        case PLAYER_UI_WIZARD_FINISH: {
            bool runtime_ready = app->inspecting_game.is_prepared;
            int idx = player_ui_wizard_save_title(app);
            if (idx >= 0 && runtime_ready) player_app_request_launch(app, idx);
            player_app_set_view(app, VIEW_LIBRARY);
            break;
        }
        case PLAYER_UI_WIZARD_GO_TO_LIBRARY:
            (void)player_ui_wizard_save_title(app);
            player_app_wizard_cancel(app);
            break;
        case PLAYER_UI_WIZARD_TOGGLE_ONLINE:
            player_app_set_online_mode(app, !app->settings.online_mode);
            break;
        case PLAYER_UI_WIZARD_TOGGLE_DEVELOPER:
            player_app_set_developer_mode(app, !app->settings.developer_mode);
            break;
        case PLAYER_UI_WIZARD_SKIP_OPTIONS:
            player_app_wizard_skip_options(app);
            break;
        case PLAYER_UI_WIZARD_NONE:
        default:
            break;
    }
}

/* Copy details: the error card's report goes to the clipboard and nowhere else. */
static void player_ui_copy_error_details(PlayerApp *app) {
    char report[PLAYER_ERROR_DETAILS_MAX + 4096];
    (void)player_app_format_error_report(app, "v" NK_PLAYER_VERSION, report, sizeof(report));
    bool copied = report[0] && SDL_SetClipboardText(report);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    /* The test reads the report from stdout, one line: newlines become " | ". */
    printf("[PLAYER_UI_TEST] copied_details clipboard=%d report=", copied ? 1 : 0);
    for (const char *c = report; *c; c++) {
        if (*c == '\n') fputs(c[1] ? " | " : "", stdout);
        else putchar(*c);
    }
    putchar('\n');
#endif
    app->last_error.details_copied = copied;
    if (!copied) {
        player_dev_log_add(app, "error", "Copy details: the clipboard did not take the report (%s)",
                           SDL_GetError());
    }
}

/* The game-found card's controls: the same handlers the existing renderer's
   buttons call. */
static void player_ui_disc_apply(PlayerApp *app, PlayerUiDiscOp op) {
    switch (op) {
        case PLAYER_UI_DISC_ADD:
            if (app->active_view == VIEW_SUPPORTED_TITLE) {
                /* A title that takes its data from the disc has its files set
                   up first by the wizard's staging worker; any other title is
                   saved directly. */
                if (!player_app_add_inspected_game(app)) {
                    player_app_set_error(app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                         "The title could not be stored. The library may be full, "
                                         "or the user data directory is not writable.",
                                         "Return to Library", VIEW_LIBRARY);
                }
            } else if (app->active_view == VIEW_EXPERIMENTAL_TITLE &&
                       player_preflight_blocking_check(&app->wizard.preflight) == NULL) {
                if (player_app_add_game(app, &app->inspecting_game)) {
                    player_app_set_view(app, VIEW_LIBRARY);
                } else {
                    player_app_set_error(app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                         "The experimental title could not be stored. The library "
                                         "may be full, or the user data directory is not writable.",
                                         "Return to Library", VIEW_LIBRARY);
                }
            }
            break;
        case PLAYER_UI_DISC_BACK:
            player_app_set_view(app, VIEW_LIBRARY);
            break;
        case PLAYER_UI_DISC_CHOOSE_ANOTHER:
            app->request_file_picker = true;
            break;
        case PLAYER_UI_DISC_NONE:
        default:
            break;
    }
}

/* ---- Check for updates (update_check.h) -------------------------------------------
   One step at a time runs on a worker thread: the check, the download or the
   install. Each starts from a click, or once at start when the user turned that
   on; nothing retries or polls. The main loop picks up the result. */

typedef enum {
    PLAYER_UPDATE_JOB_CHECK,
    PLAYER_UPDATE_JOB_DOWNLOAD,
    PLAYER_UPDATE_JOB_INSTALL
} PlayerUpdateJobKind;

typedef struct {
    PlayerUpdateJobKind kind;
    SDL_Thread *thread;
    SDL_AtomicInt done;
    SDL_AtomicInt cancel;
    SDL_AtomicInt received_kib;
    PackageHttpOpen transport;   /* NULL: the native one */
    void *transport_context;
    NkUpdateRelease release;
    char downloads_dir[NK_MAX_PATH];
    char zip_path[NK_MAX_PATH];
    char install_dir[NK_MAX_PATH];
    char log_path[NK_MAX_PATH];
    bool ok;
    NkUpdateCheckResult check;
    NkUpdateError error;
} PlayerUpdateJob;

static PlayerUpdateJob *s_update_job;

#ifdef NK_PLAYER_UI_REGRESSION_TEST
/* The regression build never reaches the network: its transport answers from
   files in NK_UI_TEST_UPDATE_DIR (releases.json for the API, otherwise the
   URL's last part), and a missing file is "offline". */
typedef struct {
    FILE *file;
    char final_url[512];
} PlayerUpdateTestResponse;

static bool player_update_test_read(void *context, unsigned char *buffer, size_t capacity,
                                    size_t *bytes_read) {
    PlayerUpdateTestResponse *response = (PlayerUpdateTestResponse *)context;
    *bytes_read = fread(buffer, 1, capacity, response->file);
    /* A slow link, so the progress and Cancel can be seen. */
    if (getenv("NK_UI_TEST_UPDATE_SLOW") != NULL) SDL_Delay(40);
    return !ferror(response->file);
}

static void player_update_test_close(void *context) {
    PlayerUpdateTestResponse *response = (PlayerUpdateTestResponse *)context;
    fclose(response->file);
    free(response);
}

static bool player_update_test_open(void *context, const char *url, const char *allowed_hosts,
                                    PackageHttpResponse *out) {
    const char *dir = getenv("NK_UI_TEST_UPDATE_DIR");
    const char *name = strrchr(url, '/');
    char path[NK_MAX_PATH];
    PlayerUpdateTestResponse *response;
    int64_t size;
    (void)context;
    (void)allowed_hosts;
    if (!dir || !name) return false;
    name = strcmp(url, NK_UPDATE_RELEASES_URL) == 0 ? "releases.json" : name + 1;
    if (snprintf(path, sizeof(path), "%s%c%s", dir, nk_platform_path_separator(), name) >=
        (int)sizeof(path)) {
        return false;
    }
    printf("[PLAYER_UI_TEST] update_request=%s\n", url);
    fflush(stdout);
    size = nk_platform_get_file_size(path);
    response = (PlayerUpdateTestResponse *)calloc(1, sizeof(*response));
    if (!response || size < 0 || !(response->file = nk_fopen_utf8(path, "rb"))) {
        free(response);
        return false;
    }
    snprintf(response->final_url, sizeof(response->final_url), "%s", url);
    memset(out, 0, sizeof(*out));
    out->final_url = response->final_url;
    out->has_content_length = true;
    out->content_length = (uint64_t)size;
    out->context = response;
    out->read = player_update_test_read;
    out->close = player_update_test_close;
    return true;
}
#endif

static bool player_update_progress(void *context, uint64_t received, uint64_t expected) {
    PlayerUpdateJob *job = (PlayerUpdateJob *)context;
    (void)expected;
    SDL_SetAtomicInt(&job->received_kib, (int)(received / 1024u));
    return SDL_GetAtomicInt(&job->cancel) == 0;
}

static int SDLCALL player_update_thread_main(void *userdata) {
    PlayerUpdateJob *job = (PlayerUpdateJob *)userdata;
    switch (job->kind) {
        case PLAYER_UPDATE_JOB_CHECK:
            job->ok = nk_update_check(job->transport, job->transport_context, NK_PLAYER_VERSION,
                                      &job->check, &job->error);
            break;
        case PLAYER_UPDATE_JOB_DOWNLOAD:
            job->ok = nk_update_download(job->transport, job->transport_context, &job->release,
                                         job->downloads_dir, player_update_progress, job,
                                         job->zip_path, sizeof(job->zip_path), &job->error);
            break;
        case PLAYER_UPDATE_JOB_INSTALL:
            job->ok = nk_update_install(job->zip_path, job->install_dir,
                                        job->log_path[0] ? job->log_path : NULL, &job->error);
            break;
    }
    SDL_SetAtomicInt(&job->done, 1);
    return 0;
}

#ifdef NK_PLAYER_UI_REGRESSION_TEST
static const char *player_update_phase_name(PlayerUpdatePhase phase) {
    switch (phase) {
        case PLAYER_UPDATE_CHECKING: return "checking";
        case PLAYER_UPDATE_CURRENT: return "current";
        case PLAYER_UPDATE_AVAILABLE: return "available";
        case PLAYER_UPDATE_DOWNLOADING: return "downloading";
        case PLAYER_UPDATE_DOWNLOADED: return "downloaded";
        case PLAYER_UPDATE_INSTALLING: return "installing";
        case PLAYER_UPDATE_INSTALLED: return "installed";
        case PLAYER_UPDATE_FAILED: return "failed";
        case PLAYER_UPDATE_IDLE:
        default: return "idle";
    }
}
#endif

static void player_update_fail(PlayerApp *app, PlayerUpdatePhase step, const char *code,
                               const char *title, const char *message) {
    snprintf(app->update.error.code, sizeof(app->update.error.code), "%s", code);
    snprintf(app->update.error.title, sizeof(app->update.error.title), "%s", title);
    snprintf(app->update.error.message, sizeof(app->update.error.message), "%s", message);
    app->update.failed_step = step;
    app->update.phase = PLAYER_UPDATE_FAILED;
}

static void player_update_start(PlayerApp *app, PlayerUpdateJobKind kind) {
    PlayerUpdateJob *job;
    PlayerUpdatePhase step = kind == PLAYER_UPDATE_JOB_CHECK ? PLAYER_UPDATE_CHECKING
                           : kind == PLAYER_UPDATE_JOB_DOWNLOAD ? PLAYER_UPDATE_DOWNLOADING
                                                                : PLAYER_UPDATE_INSTALLING;
    char data_dir[NK_MAX_PATH];
    char log_dir[NK_MAX_PATH];
    if (s_update_job != NULL) return;   /* one step at a time */
    job = (PlayerUpdateJob *)calloc(1, sizeof(*job));
    if (!job) {
        player_update_fail(app, step, "UPDATE_START_FAILED", "Couldn't Start",
                           "Nakagawa couldn't start this step. Restart it and try again.");
        return;
    }
    job->kind = kind;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    job->transport = player_update_test_open;
#endif
    job->release = app->update.check.newest;
    snprintf(job->zip_path, sizeof(job->zip_path), "%s", app->update.zip_path);
    if (!nk_platform_get_path(NK_PATH_DATA, data_dir, sizeof(data_dir)) ||
        snprintf(job->downloads_dir, sizeof(job->downloads_dir), "%s%cdownloads", data_dir,
                 nk_platform_path_separator()) >= (int)sizeof(job->downloads_dir)) {
        free(job);
        player_update_fail(app, step, "DATA_DIR_UNAVAILABLE", "Couldn't Start",
                           "Nakagawa's data folder is not available, so there is nowhere to "
                           "save the download.");
        return;
    }
    if (!nk_platform_get_path(NK_PATH_LOGS, log_dir, sizeof(log_dir)) ||
        snprintf(job->log_path, sizeof(job->log_path), "%s%cupdate-install.log", log_dir,
                 nk_platform_path_separator()) >= (int)sizeof(job->log_path)) {
        job->log_path[0] = '\0';   /* the unpacker's output then goes nowhere */
    }
    if (kind == PLAYER_UPDATE_JOB_INSTALL &&
        !nk_update_install_folder(app->install_root, job->downloads_dir,
                                  job->release.zip_name, job->install_dir,
                                  sizeof(job->install_dir))) {
        free(job);
        player_update_fail(app, step, "UPDATE_INSTALL_PATH", "Couldn't Install",
                           "The folder for the new version has too long a path. Unpack the "
                           "ZIP yourself.");
        return;
    }
    job->thread = SDL_CreateThread(player_update_thread_main, "nakagawa-update", job);
    if (!job->thread) {
        free(job);
        player_update_fail(app, step, "UPDATE_START_FAILED", "Couldn't Start",
                           "Nakagawa couldn't start this step. Restart it and try again.");
        return;
    }
    s_update_job = job;
    app->update.phase = step;
    app->update.received_bytes = 0;
    app->update.notice = false;
    if (kind == PLAYER_UPDATE_JOB_CHECK) {
        memset(&app->update.check, 0, sizeof(app->update.check));
        app->update.zip_path[0] = '\0';
        app->update.install_dir[0] = '\0';
    }
    player_dev_log_add(app, "update", "%s started",
                       kind == PLAYER_UPDATE_JOB_CHECK ? "check for updates"
                       : kind == PLAYER_UPDATE_JOB_DOWNLOAD ? "update download" : "update install");
}

/* The main loop's half: progress, then the finished step's result. */
static void player_update_poll(PlayerApp *app) {
    PlayerUpdateJob *job = s_update_job;
    if (!job) return;
    if (job->kind == PLAYER_UPDATE_JOB_DOWNLOAD) {
        app->update.received_bytes = (uint64_t)SDL_GetAtomicInt(&job->received_kib) * 1024u;
    }
    if (!SDL_GetAtomicInt(&job->done)) return;
    SDL_WaitThread(job->thread, NULL);
    s_update_job = NULL;
    if (!job->ok) {
        player_dev_log_add(app, "update", "%s: %s", job->error.code, job->error.message);
        if (job->kind == PLAYER_UPDATE_JOB_CHECK && app->update.from_start) {
            /* The check at start never shows a failure; Check for updates does. */
            app->update.phase = PLAYER_UPDATE_IDLE;
        } else {
            app->update.error = job->error;
            app->update.failed_step = job->kind == PLAYER_UPDATE_JOB_CHECK
                ? PLAYER_UPDATE_CHECKING
                : job->kind == PLAYER_UPDATE_JOB_DOWNLOAD ? PLAYER_UPDATE_DOWNLOADING
                                                          : PLAYER_UPDATE_INSTALLING;
            app->update.phase = PLAYER_UPDATE_FAILED;
        }
    } else if (job->kind == PLAYER_UPDATE_JOB_CHECK) {
        app->update.check = job->check;
        app->update.phase = job->check.newer ? PLAYER_UPDATE_AVAILABLE : PLAYER_UPDATE_CURRENT;
        app->update.notice = app->update.from_start && job->check.newer;
        player_dev_log_add(app, "update", "newest release %s; this is %s",
                           job->check.newest.tag, NK_PLAYER_VERSION);
    } else if (job->kind == PLAYER_UPDATE_JOB_DOWNLOAD) {
        snprintf(app->update.zip_path, sizeof(app->update.zip_path), "%s", job->zip_path);
        app->update.received_bytes = job->release.zip_size;
        app->update.phase = PLAYER_UPDATE_DOWNLOADED;
    } else {
        snprintf(app->update.install_dir, sizeof(app->update.install_dir), "%s",
                 job->install_dir);
        app->update.phase = PLAYER_UPDATE_INSTALLED;
    }
    app->update.from_start = false;
    free(job);
}

/* Closing the player never waits on GitHub: a running step is told to stop and
   left to end with the process. */
static void player_update_shutdown(void) {
    if (!s_update_job) return;
    SDL_SetAtomicInt(&s_update_job->cancel, 1);
    if (SDL_GetAtomicInt(&s_update_job->done)) {
        SDL_WaitThread(s_update_job->thread, NULL);
        free(s_update_job);
    } else {
        SDL_DetachThread(s_update_job->thread);
    }
    s_update_job = NULL;
}

static void player_update_open_release_page(const PlayerApp *app) {
    const char *url = app->update.check.newest.page_url;
    /* Only this repository's release pages, as the check verified. */
    if (strncmp(url, NK_UPDATE_RELEASE_PAGE_PREFIX, strlen(NK_UPDATE_RELEASE_PAGE_PREFIX)) != 0) {
        return;
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    printf("[PLAYER_UI_TEST] open_url=%s\n", url);
    fflush(stdout);
#else
    if (!SDL_OpenURL(url)) {
        fprintf(stderr, "[PLAYER] The release page could not be opened: %s\n", SDL_GetError());
    }
#endif
}

/* The Settings controls: the same handlers the existing renderer's Settings
   buttons call. Each change is saved as it is made. */
static void player_ui_settings_apply(PlayerApp *app, PlayerUiSettingsOp op) {
    switch (op) {
        case PLAYER_UI_SETTINGS_CHECK_UPDATES:
            player_app_set_view(app, VIEW_UPDATES);
            app->update.from_start = false;
            player_update_start(app, PLAYER_UPDATE_JOB_CHECK);
            break;
        case PLAYER_UI_SETTINGS_OPEN_UPDATES:
            app->update.notice = false;
            player_app_set_view(app, VIEW_UPDATES);
            break;
        case PLAYER_UI_SETTINGS_TOGGLE_UPDATE_AT_START:
            player_app_toggle_update_check_at_start(app);
            break;
        case PLAYER_UI_SETTINGS_DOWNLOAD_UPDATE:
            if (app->update.phase == PLAYER_UPDATE_AVAILABLE) {
                player_update_start(app, PLAYER_UPDATE_JOB_DOWNLOAD);
            }
            break;
        case PLAYER_UI_SETTINGS_CANCEL_UPDATE:
            if (s_update_job && s_update_job->kind == PLAYER_UPDATE_JOB_DOWNLOAD) {
                SDL_SetAtomicInt(&s_update_job->cancel, 1);
            }
            break;
        case PLAYER_UI_SETTINGS_INSTALL_UPDATE:
            if (app->update.phase == PLAYER_UPDATE_DOWNLOADED) {
                player_update_start(app, PLAYER_UPDATE_JOB_INSTALL);
            }
            break;
        case PLAYER_UI_SETTINGS_OPEN_UPDATE_FOLDER: {
            char folder[NK_MAX_PATH];
            size_t cut;
            if (app->update.phase == PLAYER_UPDATE_INSTALLED) {
                (void)player_app_request_open_folder(app, app->update.install_dir);
                break;
            }
            snprintf(folder, sizeof(folder), "%s", app->update.zip_path);
            cut = strlen(folder);
            while (cut > 0u && folder[cut - 1u] != '/' && folder[cut - 1u] != '\\') --cut;
            if (cut == 0u) break;
            folder[cut - 1u] = '\0';
            (void)player_app_request_open_folder(app, folder);
            break;
        }
        case PLAYER_UI_SETTINGS_OPEN_RELEASE_PAGE: player_update_open_release_page(app); break;
        case PLAYER_UI_SETTINGS_CYCLE_RESOLUTION: player_app_cycle_resolution_scale(app, 1); break;
        case PLAYER_UI_SETTINGS_TOGGLE_PACING:
            player_app_set_fps_cap(app, app->settings.fps_cap < 0 ? 0 : -1);
            break;
        case PLAYER_UI_SETTINGS_TOGGLE_VSYNC: player_app_toggle_vsync(app); break;
        case PLAYER_UI_SETTINGS_TOGGLE_FULLSCREEN: player_app_toggle_fullscreen(app); break;
        case PLAYER_UI_SETTINGS_CYCLE_SCALING: player_app_cycle_present_scaling(app, 1); break;
        case PLAYER_UI_SETTINGS_VOLUME_DOWN: player_app_adjust_volume(app, -5); break;
        case PLAYER_UI_SETTINGS_VOLUME_UP: player_app_adjust_volume(app, 5); break;
        case PLAYER_UI_SETTINGS_TOGGLE_UI_SOUNDS: player_app_toggle_ui_sounds(app); break;
        case PLAYER_UI_SETTINGS_TOGGLE_REDUCE_MOTION: player_app_toggle_reduce_motion(app); break;
        case PLAYER_UI_SETTINGS_TOGGLE_LAUNCHER_FULLSCREEN:
            player_app_toggle_launcher_fullscreen(app);
            break;
        case PLAYER_UI_SETTINGS_OPEN_CONTROLLER:
            player_app_set_view(app, VIEW_CONTROLLER_SETTINGS);
            break;
        case PLAYER_UI_SETTINGS_OPEN_ONLINE: player_app_set_view(app, VIEW_ONLINE_SETTINGS); break;
        case PLAYER_UI_SETTINGS_OPEN_NOTICES: (void)player_app_open_prerequisite_about(app); break;
        case PLAYER_UI_SETTINGS_OPEN_REMOVE_TOOLS:
            player_app_set_view(app, VIEW_CONFIRM_REMOVE_TOOLS);
            break;
        case PLAYER_UI_SETTINGS_DONE:
            player_app_save_settings(app, NULL);
            player_app_set_view(app, VIEW_LIBRARY);
            break;
        case PLAYER_UI_SETTINGS_BACK: player_app_set_view(app, VIEW_SETTINGS); break;
        case PLAYER_UI_SETTINGS_TOGGLE_ONLINE:
            player_app_set_online_mode(app, !app->settings.online_mode);
            break;
        case PLAYER_UI_SETTINGS_TOGGLE_NEWS: player_app_toggle_online_feature(app, 0); break;
        case PLAYER_UI_SETTINGS_TOGGLE_REPORTS: player_app_toggle_online_feature(app, 1); break;
        case PLAYER_UI_SETTINGS_TOGGLE_DEVELOPER:
            player_app_set_developer_mode(app, !app->settings.developer_mode);
            break;
        case PLAYER_UI_SETTINGS_OPEN_DEVELOPER_LOG: (void)player_app_open_developer_log(app); break;
        case PLAYER_UI_SETTINGS_OPEN_LICENSE_TEXTS:
            (void)player_app_request_license_folder(app);
            break;
        case PLAYER_UI_SETTINGS_REMOVE_TOOLS: player_app_remove_prerequisites(app); break;
        case PLAYER_UI_SETTINGS_OPEN_FACTORY_RESET:
            player_app_set_view(app, VIEW_CONFIRM_FACTORY_RESET);
            break;
        /* The event loop runs the reset: it owns the background jobs. */
        case PLAYER_UI_SETTINGS_FACTORY_RESET: app->request_factory_reset = true; break;
        case PLAYER_UI_SETTINGS_OPEN_BUILD_CACHE: player_app_set_view(app, VIEW_BUILD_CACHE); break;
        case PLAYER_UI_SETTINGS_TOGGLE_SHARE_LIBRARIES:
            player_app_toggle_share_system_libraries(app);
            break;
        case PLAYER_UI_SETTINGS_CLEAR_SHARED_LIBRARIES:
            player_app_clear_shared_library_cache(app);
            break;
        case PLAYER_UI_SETTINGS_KEEP_TOOLS: player_app_set_view(app, VIEW_BUILD_CACHE); break;
        case PLAYER_UI_SETTINGS_OPEN_FONTS:
            player_app_refresh_font_status(app);
            player_app_set_view(app, VIEW_PSP_FONTS);
            break;
        case PLAYER_UI_SETTINGS_IMPORT_FONTS: app->request_font_folder_picker = true; break;
        case PLAYER_UI_SETTINGS_REMOVE_FONTS: player_app_fonts_remove_imports(app); break;
        case PLAYER_UI_SETTINGS_NONE:
        default:
            break;
    }
}

/* The controller settings controls: the same input_settings handlers the
   existing renderer's controller buttons call. */
static void player_ui_controller_apply(PlayerApp *app, PlayerUiControllerOp op,
                                       size_t control) {
    InputSettingsState *input = &app->input_settings;
    switch (op) {
        case PLAYER_UI_CONTROLLER_REBIND:
            if (control >= (size_t)INPUT_CONTROL_DIGITAL_COUNT) break;
            /* Choosing the button that is waiting stops the wait. */
            if (input->capturing && input->capture_control == (int)control) {
                input_settings_cancel_capture(input);
            } else {
                (void)input_settings_start_capture(input, (int)control);
            }
            break;
        case PLAYER_UI_CONTROLLER_DEADZONE_DOWN:
            (void)input_settings_adjust_deadzone(input, -1, -1000);
            break;
        case PLAYER_UI_CONTROLLER_DEADZONE_UP:
            (void)input_settings_adjust_deadzone(input, -1, 1000);
            break;
        case PLAYER_UI_CONTROLLER_TRIGGER_DOWN:
            (void)input_settings_adjust_trigger_threshold(input, -1000);
            break;
        case PLAYER_UI_CONTROLLER_TRIGGER_UP:
            (void)input_settings_adjust_trigger_threshold(input, 1000);
            break;
        case PLAYER_UI_CONTROLLER_CALIBRATE: (void)input_settings_start_calibration(input); break;
        case PLAYER_UI_CONTROLLER_SAVE:
            if (input_settings_save(input, NULL) == NK_OK) {
                snprintf(input->save_diagnostic, sizeof(input->save_diagnostic),
                         "Profile successfully saved to disk");
                input->has_save_diagnostic = true;
            }
            break;
        case PLAYER_UI_CONTROLLER_RESET:
            /* The defaults of the device shown: the other device keeps its own. */
            input_settings_reset_device_to_defaults(input);
            break;
        case PLAYER_UI_CONTROLLER_SHOW_CONTROLLER:
            input_settings_set_device(input, INPUT_DEVICE_CONTROLLER);
            break;
        case PLAYER_UI_CONTROLLER_SHOW_KEYBOARD:
            input_settings_set_device(input, INPUT_DEVICE_KEYBOARD);
            break;
        case PLAYER_UI_CONTROLLER_BACK:
        case PLAYER_UI_CONTROLLER_CLOSE:
            if (input_settings_is_capturing(input)) input_settings_cancel_capture(input);
            if (input_settings_is_calibrating(input)) input_settings_cancel_calibration(input);
            player_app_set_view(app, op == PLAYER_UI_CONTROLLER_BACK ? VIEW_SETTINGS
                                                                     : VIEW_LIBRARY);
            break;
        case PLAYER_UI_CONTROLLER_TOGGLE_SCOPE:
            if (player_app_selected_disc_id(app) != NULL) {
                (void)input_settings_toggle_scope(input, player_app_selected_disc_id(app));
            }
            break;
        case PLAYER_UI_CONTROLLER_CALIBRATION_FINISH:
            (void)input_settings_finish_calibration_extremes(input);
            break;
        case PLAYER_UI_CONTROLLER_CALIBRATION_ACCEPT:
            (void)input_settings_accept_calibration(input);
            break;
        case PLAYER_UI_CONTROLLER_CALIBRATION_CANCEL:
            input_settings_cancel_calibration(input);
            break;
        case PLAYER_UI_CONTROLLER_NONE:
        default:
            break;
    }
}

/* The developer log controls, as the existing renderer's log buttons. */
static void player_ui_developer_log_apply(PlayerApp *app, PlayerUiDeveloperLogOp op) {
    static char clip[PLAYER_DEV_LOG_LINES * PLAYER_DEV_LOG_LINE_MAX + 1];
    switch (op) {
        case PLAYER_UI_DEVELOPER_LOG_COPY:
            (void)player_dev_log_text(app, clip, sizeof(clip));
            if (SDL_SetClipboardText(clip)) {
                snprintf(app->dev_log.notice, sizeof(app->dev_log.notice), "Copied %zu lines.",
                         player_dev_log_count(app));
            } else {
                snprintf(app->dev_log.notice, sizeof(app->dev_log.notice),
                         "The log could not be copied.");
            }
            break;
        case PLAYER_UI_DEVELOPER_LOG_SAVE: (void)player_dev_log_save(app); break;
        case PLAYER_UI_DEVELOPER_LOG_CLOSE: player_app_close_developer_log(app); break;
        case PLAYER_UI_DEVELOPER_LOG_NONE:
        default:
            break;
    }
}

static void player_ui_nk_apply_action(PlayerApp *app,
                                     const PlayerUiAction *action) {
    int index;
    /* Most frames carry no action, and its title_index is 0: selecting that
       index here made every open title page show the first game (#1301). */
    if (app == NULL || action == NULL || action->kind == PLAYER_UI_ACTION_NONE) return;
    if (action->kind == PLAYER_UI_ACTION_WIZARD) {
        player_ui_wizard_apply(app, (PlayerUiWizardOp)action->primary);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_DISC) {
        player_ui_disc_apply(app, (PlayerUiDiscOp)action->primary);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_SETTINGS) {
        player_ui_settings_apply(app, (PlayerUiSettingsOp)action->primary);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_CONTROLLER) {
        player_ui_controller_apply(app, (PlayerUiControllerOp)action->primary,
                                   action->title_index);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_DEVELOPER_LOG) {
        player_ui_developer_log_apply(app, (PlayerUiDeveloperLogOp)action->primary);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_REBUILD) {
        /* Games an older Nakagawa built: the prompt, the library banner, the
           launch reminder and the title page's Rebuild. */
        switch ((PlayerUiRebuildOp)action->primary) {
            case PLAYER_UI_REBUILD_ALL:
                if (player_app_rebuild_prompt_visible(app)) {
                    player_app_rebuild_prompt_answer(app, true);
                } else {
                    (void)player_app_rebuild_all(app);
                }
                break;
            case PLAYER_UI_REBUILD_LATER:
                player_app_rebuild_prompt_answer(app, false);
                break;
            case PLAYER_UI_REBUILD_HIDE_BANNER:
                player_app_rebuild_banner_dismiss(app);
                break;
            case PLAYER_UI_REBUILD_NOW:
                player_app_rebuild_reminder_rebuild_now(app);
                break;
            case PLAYER_UI_REBUILD_PLAY_ANYWAY:
                player_app_rebuild_reminder_play_anyway(app);
                break;
            case PLAYER_UI_REBUILD_TITLE:
                if (action->title_index < (size_t)app->game_count) {
                    app->selected_game_index = (int)action->title_index;
                    (void)player_app_rebuild_package(app, (int)action->title_index);
                }
                break;
            case PLAYER_UI_REBUILD_NONE:
            default:
                break;
        }
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_STALL_KEEP_WAITING) {
        player_app_keep_waiting_for_stalled_game(app);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_STALL_STOP_GAME) {
        player_app_stop_stalled_game(app);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_OPEN_BUILD_INPUT_FOLDER) {
        const char *path = app->package_build_preflight.action_folder;
        if (action->title_index < (size_t)app->game_count && path[0]) {
            (void)nk_platform_mkdir_p_private(path);
            (void)player_app_request_open_folder(app, path);
        }
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_ADD_DISC) {
        app->request_file_picker = true;
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_START_WIZARD) {
        player_app_start_setup_wizard(app);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_OPEN_SETTINGS) {
        player_app_set_view(app, VIEW_SETTINGS);
        return;
    }
    /* The build, build tool download and error screens: the same handlers
       the existing renderer's buttons call. */
    if (action->kind == PLAYER_UI_ACTION_CANCEL_BUILD) {
        player_app_cancel_package_build(app);
        player_app_set_view(app, VIEW_LIBRARY);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_LEAVE_BUILD) {
        player_app_set_view(app, VIEW_LIBRARY);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_PREREQ_ACCEPT) {
        player_app_prereq_accept(app, app->prerequisites.bootstrap_python);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_PREREQ_CANCEL) {
        player_app_prereq_cancel(app);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_ERROR_RECOVER &&
        app->active_view == VIEW_PREPARING) {
        player_app_set_view(app, VIEW_LIBRARY);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_ERROR_RECOVER) {
        if (app->last_error.return_view == VIEW_PREREQ_CONSENT &&
            app->prerequisites.phase == PLAYER_PREREQ_FAILED) {
            player_app_prereq_retry(app);
        } else {
            player_app_set_view(app, app->last_error.return_view);
            if (strcmp(app->last_error.error_code, "ISO_CORRUPT") == 0 ||
                strcmp(app->last_error.error_code, "SOURCE_NOT_FOUND") == 0 ||
                strncmp(app->last_error.error_code, "PBP_", 4) == 0 ||
                strncmp(app->last_error.error_code, "DISC_IMAGE_", 11) == 0 ||
                strcmp(app->last_error.error_code, "DISC_NOT_PSP") == 0) {
                app->request_file_picker = true;
            }
        }
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_ERROR_RETRY) {
        /* Restart game or Try again: the same launch and build the Library
           runs. A game that has left the library returns there. */
        if (!player_app_retry_from_error(app)) player_app_set_view(app, VIEW_LIBRARY);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_ERROR_COPY) {
        player_ui_copy_error_details(app);
        return;
    }
    if (action->kind == PLAYER_UI_ACTION_OPEN_NOTICES) {
        /* The third-party notices are the existing licences screen. */
        player_app_set_view(app, VIEW_PREREQ_ABOUT);
        return;
    }
    if (action->title_index >= (size_t)app->game_count) return;
    index = (int)action->title_index;
    app->selected_game_index = index;
    if (action->kind == PLAYER_UI_ACTION_OPEN_SAVE_FOLDER) {
        player_ui_open_save_folder(app, action->title_index);
    } else if (action->kind == PLAYER_UI_ACTION_REMOVE_CONFIRM) {
        /* Remove from library (#1304): what Nakagawa built goes with it, the
           saves only when the dialog's box was ticked. */
        char reason[NK_MAX_PATH + 96];
        if (!player_app_remove_game_and_data(app, index, action->primary == 1,
                                             reason, sizeof(reason))) {
            player_app_set_error(app, "REMOVE_FAILED", "The game was not removed", reason,
                                 "Back to library", VIEW_LIBRARY);
        }
    } else if (action->kind == PLAYER_UI_ACTION_PRIMARY) {
        switch (action->primary) {
            case PLAYER_UI_PRIMARY_PLAY:
                /* An outdated game asks first (Rebuild now or Play anyway). */
                (void)player_app_request_play(app, index);
                break;
            case PLAYER_UI_PRIMARY_BUILD:
                (void)player_app_start_package_build(app, index);
                break;
            case PLAYER_UI_PRIMARY_REBUILD:
                (void)player_app_rebuild_package(app, index);
                break;
            case PLAYER_UI_PRIMARY_RETRY_PACKAGE_CHECK:
                app->request_package_status_retry = true;
                break;
            case PLAYER_UI_PRIMARY_STOP_GAME:
                player_app_stop_game(app);
                break;
            case PLAYER_UI_PRIMARY_NONE:
            default:
                break;
        }
    }
}

/* The screens ui_renderer.c draws are laid out on a PlayerUiCanvas and drawn
   with a render scale straight into the output's pixels, so text, icons and
   card edges are rasterised at the size they are shown (#1129). SDL's logical
   presentation stays off: it stretched a fixed 1280x720 canvas to the window.
   An interactive launcher refits to the window every frame; a capture draws
   its fixed --width x --height layout at --render-scale. */
static bool s_ui_canvas_from_window = false;
static PlayerUiCanvas s_ui_capture_canvas = {
    PLAYER_UI_DESIGN_WIDTH, PLAYER_UI_DESIGN_HEIGHT, 1.0f
};

static void player_fit_ui_canvas(PlayerApp *app, SDL_Renderer *renderer) {
    PlayerUiCanvas canvas = s_ui_capture_canvas;
    if (s_ui_canvas_from_window) {
        int pixel_width = 0;
        int pixel_height = 0;
        /* A failed fit (a minimized window has no pixels) keeps the last
           canvas, so layout and hit-testing still agree with the screen. */
        if (!SDL_GetRenderOutputSize(renderer, &pixel_width, &pixel_height) ||
            !player_ui_canvas_fit(pixel_width, pixel_height, &canvas)) {
            return;
        }
    }
    if (!SDL_SetRenderScale(renderer, canvas.scale, canvas.scale)) return;
    app->window_width = canvas.width;
    app->window_height = canvas.height;
    app->dpi_scale = canvas.scale;
}

/* With the next UI on, the launcher switches presentation with the renderer
   that draws the frame: nk_ui lays out at the window's real size, and the
   screens still drawn by ui_renderer.c (setup wizard, settings, build progress,
   dialogs) use the fitted canvas above. */
static bool s_switch_presentation = false;
static int s_presentation = -1; /* 0 fitted canvas, 1 native */

static void player_use_presentation(PlayerApp *app, SDL_Window *window,
                                    bool native) {
    int want = native ? 1 : 0;
    if (!s_switch_presentation || s_presentation == want) return;
    if (native) {
        int w = 0;
        int h = 0;
        if (SDL_GetWindowSize(window, &w, &h) && w > 0 && h > 0) {
            app->window_width = w;
            app->window_height = h;
        }
        app->logical_ui = false;
    } else {
        /* player_fit_ui_canvas sets the sizes before the frame is drawn. */
        app->logical_ui = true;
    }
    s_presentation = want;
}

static void player_render_active_ui(PlayerUiNk *next_ui, PlayerApp *app,
                                    SDL_Renderer *renderer, SDL_Window *window,
                                    UiInput *input) {
    PlayerUiAction action;
    bool next = player_ui_nk_view_active(next_ui, app);
    player_use_presentation(app, window, next);
    /* nk_ui sizes itself from the output's pixels; the canvas scale of the
       screens below must not apply to it. */
    if (next) (void)SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    if (next && player_ui_nk_render(next_ui, app, renderer, window, &action)) {
        player_ui_nk_apply_action(app, &action);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    } else if (next_ui == NULL) {
        /* --ui=legacy, the regression build only. */
        player_use_presentation(app, window, false);
        player_fit_ui_canvas(app, renderer);
        ui_render_frame(renderer, app, input);
#endif
    } else {
        /* The launcher draws every view (player_ui_nk_draws_view); a frame it
           could not draw is cleared, never handed to another renderer. */
        (void)input;
        (void)SDL_SetRenderDrawColor(renderer, 11u, 15u, 20u, 255u);
        (void)SDL_RenderClear(renderer);
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
    PLAYER_UI_TEST_ACTION_WAIT_ART_DELAY_ACTIVE = -20261008,
    PLAYER_UI_TEST_ACTION_WAIT_PACKAGE_STATUS = -20261009
};

static unsigned s_ui_test_package_status_thread_create_attempts;
static const PlayerUiNk *s_ui_test_next_ui;
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
        { "confirm_remove_tools", VIEW_CONFIRM_REMOVE_TOOLS },
        { "build_cache", VIEW_BUILD_CACHE },
        { "psp_fonts", VIEW_PSP_FONTS }, { "updates", VIEW_UPDATES },
        { "confirm_factory_reset", VIEW_CONFIRM_FACTORY_RESET }
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

static bool player_ui_test_parse_package_status(
    const char *text, NkRuntimePackageStatus *out_status) {
    if (!text || !out_status) return false;
    if (strcmp(text, "OK") == 0) {
        *out_status = NK_RUNTIME_PACKAGE_OK;
    } else if (strcmp(text, "MISSING") == 0) {
        *out_status = NK_RUNTIME_PACKAGE_MISSING;
    } else if (strcmp(text, "INCOMPATIBLE") == 0) {
        *out_status = NK_RUNTIME_PACKAGE_INCOMPATIBLE;
    } else if (strcmp(text, "STALE") == 0) {
        *out_status = NK_RUNTIME_PACKAGE_STALE;
    } else if (strcmp(text, "UNKNOWN") == 0) {
        *out_status = NK_RUNTIME_PACKAGE_UNKNOWN;
    } else {
        return false;
    }
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

/* The scripted pointer position, so a scripted click lands where it moved. */
static float s_ui_test_mouse_x;
static float s_ui_test_mouse_y;

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
    if (strncmp(token, "WAIT_PACKAGE_STATUS=", 20) == 0) {
        char *comma = strchr(token + 20, ',');
        NkRuntimePackageStatus target = NK_RUNTIME_PACKAGE_UNKNOWN;
        uint64_t timeout = 0;
        if (!comma) return -1;
        *comma = '\0';
        if (!player_ui_test_parse_package_status(token + 20, &target) ||
            !player_ui_test_parse_positive_u64(comma + 1, &timeout) ||
            timeout > UINT32_MAX) return -1;
        event->type = SDL_EVENT_USER;
        event->user.code = PLAYER_UI_TEST_ACTION_WAIT_PACKAGE_STATUS;
        event->user.windowID = (Uint32)target;
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
        s_ui_test_mouse_x = (float)x;
        s_ui_test_mouse_y = (float)y;
        return 1;
    }
    if (strcmp(token, "MOUSE_DOWN") == 0 || strcmp(token, "MOUSE_UP") == 0) {
        /* As SDL reports a real click: at the pointer, with its button state. */
        event->type = strcmp(token, "MOUSE_DOWN") == 0
            ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event->button.button = SDL_BUTTON_LEFT;
        event->button.down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event->button.clicks = 1u;
        event->button.x = s_ui_test_mouse_x;
        event->button.y = s_ui_test_mouse_y;
        return 1;
    }
    if (strncmp(token, "KEY_", 4) == 0) {
        /* As SDL reports a key: its key code and, on a US layout, its position. */
        static const struct { const char *name; SDL_Keycode key; SDL_Scancode scancode; } keys[] = {
            { "ESCAPE", SDLK_ESCAPE, SDL_SCANCODE_ESCAPE },
            { "RETURN", SDLK_RETURN, SDL_SCANCODE_RETURN }, { "TAB", SDLK_TAB, SDL_SCANCODE_TAB },
            { "SPACE", SDLK_SPACE, SDL_SCANCODE_SPACE }, { "LEFT", SDLK_LEFT, SDL_SCANCODE_LEFT },
            { "RIGHT", SDLK_RIGHT, SDL_SCANCODE_RIGHT }, { "UP", SDLK_UP, SDL_SCANCODE_UP },
            { "DOWN", SDLK_DOWN, SDL_SCANCODE_DOWN }, { "HOME", SDLK_HOME, SDL_SCANCODE_HOME },
            { "END", SDLK_END, SDL_SCANCODE_END }, { "PAGEUP", SDLK_PAGEUP, SDL_SCANCODE_PAGEUP },
            { "PAGEDOWN", SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN }, { "C", SDLK_C, SDL_SCANCODE_C },
            { "S", SDLK_S, SDL_SCANCODE_S }, { "O", SDLK_O, SDL_SCANCODE_O },
            { "X", SDLK_X, SDL_SCANCODE_X }, { "F1", SDLK_F1, SDL_SCANCODE_F1 },
            { "F12", SDLK_F12, SDL_SCANCODE_F12 }
        };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (strcmp(token + 4, keys[i].name) == 0) {
                event->type = SDL_EVENT_KEY_DOWN;
                event->key.key = keys[i].key;
                event->key.scancode = keys[i].scancode;
                event->key.down = true;
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
            { "START", SDL_GAMEPAD_BUTTON_START },
            { "LEFT_SHOULDER", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
            { "RIGHT_SHOULDER", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER }
        };
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            if (strcmp(token + 4, buttons[i].name) == 0) {
                event->type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                event->gbutton.button = buttons[i].button;
                event->gbutton.down = true;
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
    case VIEW_BUILD_CACHE: return "build_cache";
    case VIEW_PSP_FONTS: return "psp_fonts";
    case VIEW_ONLINE_SETTINGS: return "online_settings";
    case VIEW_UPDATES: return "updates";
    case VIEW_CONFIRM_FACTORY_RESET: return "confirm_factory_reset";
    case VIEW_DEVELOPER_LOG: return "developer_log";
    default: return "unknown";
    }
}

static const char *player_ui_test_wizard_step_name(WizardStep step) {
    switch (step) {
    case WIZARD_STEP_WELCOME: return "welcome";
    case WIZARD_STEP_SELECT_GAME: return "select_game";
    case WIZARD_STEP_INSPECT_VERIFY: return "inspect_verify";
    case WIZARD_STEP_SYSTEM_FONTS: return "system_fonts";
    case WIZARD_STEP_ONLINE_OPTIONS: return "online_options";
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
    /* The selected title's primary action as the next UI's model holds it
     * (spaces become underscores): -1 and "none" with the existing UI. */
    int next_primary = -1;
    char next_primary_label[64] = "none";
    char next_detail[256] = "none";
    char next_preflight[640] = "none";
    char next_preflight_code[64] = "none";
    char next_preflight_detail[640] = "none";
    char next_preflight_action[24] = "none";
    int next_package = -1;
    char next_expect[256] = "none";
    {
        const PlayerUiTitle *model = selected != NULL
            ? player_ui_nk_test_title(s_ui_test_next_ui, (size_t)app->selected_game_index)
            : NULL;
        if (model != NULL && model->primary_action_label != NULL) {
            next_primary = (int)model->primary_action;
            snprintf(next_primary_label, sizeof(next_primary_label), "%s%s",
                     model->primary_action_enabled ? "" : "disabled:",
                     model->primary_action_label);
            snprintf(next_detail, sizeof(next_detail), "%s",
                     model->library_status_detail != NULL && model->library_status_detail[0]
                         ? model->library_status_detail : "none");
            snprintf(next_preflight, sizeof(next_preflight), "%s",
                     model->build_preflight_message != NULL &&
                             model->build_preflight_message[0]
                         ? model->build_preflight_message : "none");
            if (model->build_preflight_folder != NULL &&
                model->build_preflight_folder[0]) {
                snprintf(next_preflight_action, sizeof(next_preflight_action), "OPEN_FOLDER");
            }
            next_package = (int)model->package_state;
            snprintf(next_expect, sizeof(next_expect), "%s",
                     model->compatibility_expect != NULL && model->compatibility_expect[0]
                         ? model->compatibility_expect : "none");
            for (char *q = next_primary_label; *q; q++) if (*q == ' ') *q = '_';
            for (char *q = next_detail; *q; q++) if (*q == ' ') *q = '_';
            for (char *q = next_preflight; *q; q++) if (*q == ' ') *q = '_';
            for (char *q = next_expect; *q; q++) if (*q == ' ') *q = '_';
        } else if (selected != NULL && s_ui_test_next_ui == NULL) {
            const PlayerPreflightCheck *blocker =
                player_app_package_build_preflight_blocker(
                    app, app->selected_game_index);
            if (blocker) {
                snprintf(next_preflight, sizeof(next_preflight), "%s",
                         player_preflight_plain_message(blocker));
                snprintf(next_preflight_code, sizeof(next_preflight_code), "%s",
                         blocker->code[0] ? blocker->code : "unknown");
                snprintf(next_preflight_detail, sizeof(next_preflight_detail), "%.639s",
                         blocker->message[0] ? blocker->message : "unknown");
                if (app->package_build_preflight.action_folder[0]) {
                    snprintf(next_preflight_action,
                             sizeof(next_preflight_action), "OPEN_FOLDER");
                }
                for (char *q = next_preflight; *q; q++) if (*q == ' ') *q = '_';
                for (char *q = next_preflight_detail; *q; q++) if (*q == ' ') *q = '_';
            }
        }
    }
    /* The error card's words as single tokens (spaces become underscores). */
    char error_title[sizeof(app->last_error.title)];
    char error_badge[64];
    char error_retry[32];
    const char *retry_label = player_error_retry_label(&app->last_error);
    snprintf(error_title, sizeof(error_title), "%s",
             app->last_error.title[0] ? app->last_error.title : "none");
    snprintf(error_badge, sizeof(error_badge), "%s", player_error_badge(&app->last_error));
    snprintf(error_retry, sizeof(error_retry), "%s", retry_label ? retry_label : "none");
    for (char *q = error_title; *q; q++) if (*q == ' ') *q = '_';
    for (char *q = error_badge; *q; q++) if (*q == ' ') *q = '_';
    for (char *q = error_retry; *q; q++) if (*q == ' ') *q = '_';
    const bool bad_disc_notice =
        (app->active_view == VIEW_SETUP_WIZARD &&
         strcmp(app->wizard.status_message, k_invalid_disc_message) == 0) ||
        (app->active_view == VIEW_ERROR &&
         strcmp(app->last_error.message, k_invalid_disc_message) == 0);
    char incompatible_reason[256];
    snprintf(incompatible_reason, sizeof(incompatible_reason), "%s",
             ui_test_last_incompatible_reason()[0] ? ui_test_last_incompatible_reason() : "none");
    for (char *q = incompatible_reason; *q; q++) if (*q == ' ') *q = '_';
    /* The build badge as a single token (spaces become underscores), so the
     * frame record stays parseable. "none" outside the build screen. */
    char build_badge[64];
    if (app->active_view == VIEW_BUILDING_PACKAGE) {
        snprintf(build_badge, sizeof(build_badge), "%s", ui_test_build_badge());
        for (char *q = build_badge; *q; q++) {
            if (*q == ' ') *q = '_';
        }
    } else {
        snprintf(build_badge, sizeof(build_badge), "none");
    }
    /* The build bar's fill in thousandths, -1 for the sweep or off the build screen. */
    int build_bar_permille = -1;
    if (app->active_view == VIEW_BUILDING_PACKAGE && ui_test_build_bar_fraction() >= 0.0f) {
        build_bar_permille = (int)(ui_test_build_bar_fraction() * 1000.0f + 0.5f);
    }
    printf("[PLAYER_UI_TEST] frame=%d ticks_ms=%llu view=%s "
           "selected=%d selected_disc=%s selected_title_id=%s "
           "focus=%d focus_count=%d settings_two_col=%d wizard_step=%s "
           "font_confirmed=%d extracting=%d extraction_percent=%d extraction_cancel=%d error=%s "
           "bad_disc_notice=%d "
           "picker=%d package_building=%d package_cancelled=%d profile_fallback=%d "
           "controller_capturing=%d controller_conflicts=%d calibrating=%d "
           "error_text_complete=%d error_text_lines=%d "
           "error_details_complete=%d error_details_lines=%d error_details_available=%d "
           "select_binding=%d start_binding=%d circle_binding=%d profile_save_notice=%d "
           "input_scope=%s input_titles=%d input_notice=%d "
           "input_device=%s start_key=%d key_note=%d "
           "selected_experimental=%d selected_prepared=%d selected_staged=%d "
           "selected_runtime=%d selected_package_status=%d "
           "selected_package_check_failed=%d package_check_failed_badge=%d games=%d "
           "package_status_thread_attempts=%u "
           "titles_pending=%d titles_failed=%d titles_unclaimed=%d "
           "prereq_items=%zu prereq_bytes=%llu game_running=%d build_stage=%d "
           "font=%s font_reason=%s bitmap_text_draws=%u build_badge=%s build_bar_permille=%d "
           "build_notice=%d "
           "badge=%d,%d,%d,%d running=%d pixels=%016llx "
           "render_ns=%llu package_validations=%llu online_mode=%d developer_mode=%d "
           "dev_log_lines=%zu next_primary=%d next_primary_label=%s next_detail=%s "
           "next_preflight=%s next_preflight_code=%s next_preflight_detail=%s "
           "next_preflight_action=%s "
           "incompatible_reason=%s next_at_root=%d next_route=%d "
           "error_title=%s error_badge=%s error_retry=%s error_copied=%d "
           "next_package=%d next_expect=%s launch_pending=%d next_focus=%u "
           "outdated=%d outdated_scanned=%d rebuild_prompt=%d rebuild_banner=%d "
           "rebuild_reminder=%d rebuild_queue=%d rebuild_queue_active=%d "
           "rebuild_queue_paused=%d prompt_version=%s\n",
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
           bad_disc_notice ? 1 : 0,
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
           app->input_settings.device == INPUT_DEVICE_KEYBOARD ? "keyboard" : "controller",
           app->input_settings.profile.psp_keys[INPUT_CONTROL_BTN_START].primary,
           app->input_settings.key_note[0] ? 1 : 0,
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
           ui_test_bitmap_text_draws(), build_badge, build_bar_permille,
           app->active_view == VIEW_BUILDING_PACKAGE && ui_test_build_notice_drawn() ? 1 : 0,
           badge_valid ? (int)badge.x : -1, badge_valid ? (int)badge.y : -1,
           badge_valid ? (int)badge.w : 0, badge_valid ? (int)badge.h : 0,
           running ? 1 : 0,
           (unsigned long long)player_ui_test_frame_hash(renderer),
           (unsigned long long)render_elapsed_ns,
           (unsigned long long)validation_calls,
           app->settings.online_mode ? 1 : 0, app->settings.developer_mode ? 1 : 0,
           player_dev_log_count(app), next_primary, next_primary_label, next_detail,
           next_preflight, next_preflight_code, next_preflight_detail,
           next_preflight_action,
           incompatible_reason,
           s_ui_test_next_ui != NULL ? (player_ui_nk_at_library_root(s_ui_test_next_ui) ? 1 : 0) : -1,
           s_ui_test_next_ui != NULL ? player_ui_nk_test_route(s_ui_test_next_ui) : -1,
           error_title, error_badge, error_retry,
           app->last_error.details_copied ? 1 : 0,
           next_package, next_expect, app->launch_pending ? 1 : 0,
           s_ui_test_next_ui != NULL ? player_ui_nk_test_focus(s_ui_test_next_ui) : 0u,
           player_app_outdated_count(app), app->outdated.scanned ? 1 : 0,
           player_app_rebuild_prompt_visible(app) ? 1 : 0,
           player_app_rebuild_banner_visible(app) ? 1 : 0,
           player_app_rebuild_reminder_visible(app) ? 1 : 0,
           app->rebuild_queue.count, app->rebuild_queue.active ? 1 : 0,
           app->rebuild_queue.paused ? 1 : 0,
           app->settings.rebuild_prompt_version[0] ? app->settings.rebuild_prompt_version
                                                   : "none");

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

/* Set once the launcher is closing. Package validation polls it while it
   hashes, so closing the window during the startup package check does not
   wait for every check to finish (launcher review B5). */
static SDL_AtomicInt s_player_closing;

static bool player_closing(void *user) {
    (void)user;
    return SDL_GetAtomicInt(&s_player_closing) != 0;
}

/* Which games an older Nakagawa built (player_app_outdated_scan_*). The first
   scan of a session hashes this player's own source tree (about 20 MB), so it
   runs here; later scans only read each package.json. One at a time, polled
   from the loop; the hashing stops when the launcher closes. */
typedef struct {
    SDL_Thread *thread;
    SDL_AtomicInt done;
    PlayerOutdatedScan scan;
} PlayerOutdatedScanJob;

static int SDLCALL player_outdated_scan_thread_main(void *userdata) {
    PlayerOutdatedScanJob *job = (PlayerOutdatedScanJob *)userdata;
    if (!job) return 1;
    player_outdated_scan_run(&job->scan);
    SDL_SetAtomicInt(&job->done, 1);
    return 0;
}

/* Applies a finished scan and starts the next one asked for. closing waits
   for a running scan and applies nothing. */
static PlayerOutdatedScanJob *player_outdated_scan_poll(PlayerApp *app,
                                                        PlayerOutdatedScanJob *job,
                                                        bool closing) {
    if (job) {
        if (!closing && !SDL_GetAtomicInt(&job->done)) return job;
        if (job->thread) SDL_WaitThread(job->thread, NULL);
        if (!closing) player_app_outdated_scan_finish(app, &job->scan, NK_PLAYER_VERSION);
        free(job);
        job = NULL;
    }
    if (closing || !app->outdated.scan_requested) return NULL;
    job = (PlayerOutdatedScanJob *)calloc(1, sizeof(*job));
    if (!job) return NULL;
    if (!player_app_outdated_scan_begin(app, &job->scan)) {
        free(job);
        return NULL;
    }
    job->thread = SDL_CreateThread(player_outdated_scan_thread_main,
                                   "nakagawa-package-age", job);
    if (!job->thread) {
        /* Bounded work: without a worker it runs once here. */
        player_outdated_scan_run(&job->scan);
        SDL_SetAtomicInt(&job->done, 1);
    }
    return job;
}

static int SDLCALL player_package_status_thread_main(void *userdata) {
    PlayerPackageStatusJob *job = (PlayerPackageStatusJob *)userdata;
    if (!job) return 1;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    const char *delay_text = getenv("NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS");
    if (delay_text && delay_text[0]) {
        char *end = NULL;
        unsigned long delay = strtoul(delay_text, &end, 10);
        /* A slow check, standing in for hashing a large package: it stops
           when the launcher closes, as the hashing does. */
        if (end && *end == '\0' && delay <= 5000) {
            for (unsigned long waited = 0; waited < delay && !player_closing(NULL);
                 waited += 10u) {
                SDL_Delay(10);
            }
        }
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

    if (job->runtime_available) {
        char data_reason[1024] = "";
        NkLaunchDataRootStatus data_status = nk_launch_game_data_root_status(
            root, &job->game, NULL, 0, data_reason, sizeof(data_reason));
        if (data_status != NK_LAUNCH_DATA_ROOT_READY &&
            data_status != NK_LAUNCH_DATA_ROOT_NOT_REQUIRED) {
            job->runtime_available = false;
            if (data_reason[0]) {
                snprintf(job->reason, sizeof(job->reason), "%s", data_reason);
            }
        }
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

/* Picks the next queued title to check: the selected one, then the titles the
   launcher shows, then one a build or staging asked for. A title off screen
   waits until it is shown: a full check hashes the disc's executable and
   modules, and checking every title of a large library at every start read
   every disc. Without the launcher (the regression build's old renderer) any
   queued title goes, in library order. */
static int player_package_status_pick(const PlayerApp *app, const PlayerUiNk *next_ui,
                                      const bool requested[MAX_LIBRARY_GAMES],
                                      const bool force_requested[MAX_LIBRARY_GAMES]) {
    int selected = app->selected_game_index;
    bool shown_only = player_ui_nk_view_active(next_ui, app);
    if (selected >= 0 && selected < app->game_count && requested[selected]) {
        return selected;
    }
    if (shown_only) {
        int shown[PLAYER_UI_SHOWN_TITLES_MAX];
        size_t shown_count = player_ui_nk_shown_titles(next_ui, shown,
                                                       PLAYER_UI_SHOWN_TITLES_MAX);
        for (size_t i = 0; i < shown_count; i++) {
            if (shown[i] >= 0 && shown[i] < app->game_count && requested[shown[i]]) {
                return shown[i];
            }
        }
    }
    for (int i = 0; i < app->game_count; i++) {
        if (requested[i] && (!shown_only || force_requested[i])) return i;
    }
    return -1;
}

static bool player_package_status_start_next(
    PlayerApp *app, const PlayerUiNk *next_ui, PlayerPackageStatusJob **job_slot,
    bool requested[MAX_LIBRARY_GAMES], bool force_requested[MAX_LIBRARY_GAMES],
    const char build_check_disc_id[MAX_DISC_ID_LEN],
    uint64_t build_check_session_generation,
    uint64_t build_check_cache_generation,
    bool *build_check_pending) {
    if (!app || !job_slot || *job_slot) return false;
    (void)build_check_cache_generation;
    int index = player_package_status_pick(app, next_ui, requested, force_requested);
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
    /* The same rule as player_package_status_queue_game: a title with a status keeps it while a
       periodic re-check runs, so its card does not blink back to CHECKING on every rescan. A title
       with no status yet, a check the user asked for and a forced check (after a build or a
       staging) still show the check in flight. */
    if (!same_game || !cached->status_valid || job->force) {
        player_app_runtime_package_cache_mark_pending(app, index);
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    printf("[PLAYER_UI_TEST] package_check index=%d disc=%s\n", index, job->game.disc_id);
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
        if (job->status == NK_RUNTIME_PACKAGE_OK && job->runtime_available) {
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
        player_app_runtime_package_cache_set_reason(app, current_index, job->reason);
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
        /* A Rebuild all build ends in the queue: the user stays where they
           are, and a failed check leaves the old package, which still plays. */
        bool queued_build = player_app_rebuild_queue_owns_build(app);
        *build_check_pending = false;
        *build_check_cache_generation = 0;
        /* Keep the completed session identity as a one-shot latch. The build
           state remains complete after its view changes, and the main loop
           must not queue the same post-build validation again. */
        if (queued_build && !(job->status == NK_RUNTIME_PACKAGE_OK && job->runtime_available)) {
            player_app_rebuild_queue_build_checked(app, false);
            if (app->active_view == VIEW_BUILDING_PACKAGE) {
                player_app_set_view(app, VIEW_LIBRARY);
            }
        } else if (job->status == NK_RUNTIME_PACKAGE_OK && job->runtime_available) {
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
                player_app_runtime_package_cache_set_reason(app, current_index,
                                                            job->reason);
            }
            if (queued_build) {
                player_app_rebuild_queue_build_checked(app, true);
                if (app->active_view == VIEW_BUILDING_PACKAGE) {
                    player_app_set_view(app, VIEW_LIBRARY);
                }
            } else {
                player_app_set_view(app, PLAYER_VIEW_READY_LIBRARY);
            }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
            /* runtime= is what makes the title's card offer PLAY. */
            printf("[PLAYER_UI_TEST] post_build_transition view=ready_library "
                   "disc=%s found=%d prepared=%d status=%d runtime=%d\n",
                   job->game.disc_id, current_index >= 0,
                   current_index >= 0
                       ? app->games[current_index].is_prepared : 0,
                   current_index >= 0
                       ? (int)app->games[current_index].status : -1,
                   current_index >= 0
                       ? (int)player_app_cached_game_has_runtime(
                             app, &app->games[current_index]) : 0);
#endif
        } else {
            player_app_set_package_revalidation_error(
                app, job->reason, app->build_session.log_file_path);
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

static bool copy_bounded_text(char *destination, size_t destination_size,
                              const char *source) {
    if (!destination || destination_size == 0 || !source) return false;
    size_t length = strlen(source);
    if (length >= destination_size) return false;
    memcpy(destination, source, length + 1);
    return true;
}

typedef struct {
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    PlayerStagePlan plan;
    bool cancel_requested;
    bool finished;
    bool completion_handled;
    NkResult result;
    int percent;
    size_t files_extracted;
    size_t total_files;
    char current_file[4096];
    char error_message[1024];
    char prepared_root[NK_MAX_PATH];
    PlayerStageTitleOutcome outcome;
    PlayerStageSummary summary;
} PlayerStagingJob;

#define PLAYER_STAGE_PROFILE_UNAVAILABLE \
    "[STAGE_PROFILE_UNAVAILABLE] This game's title settings could not be read, " \
    "so its files were not set up. Add the disc again from the library."

/* Record a finished staging run on the inspected game. */
static bool player_take_staged_title(PlayerApp *app, const char *prepared_root,
                                     const PlayerStageSummary *summary) {
    if (!copy_bounded_text(app->inspecting_game.prepared_root,
                           sizeof(app->inspecting_game.prepared_root),
                           prepared_root)) return false;
    app->inspecting_game.assets_staged = true;
    app->inspecting_game.extracted_asset_count = summary->extracted_asset_count;
    app->inspecting_game.extracted_audio_count = summary->extracted_audio_count;
    app->inspecting_game.extracted_visual_count = summary->extracted_visual_count;
    app->inspecting_game.extracted_layout_count = summary->extracted_layout_count;
    app->inspecting_game.is_prepared = false;
    app->inspecting_game.status = NK_STATUS_SUPPORTED_PREPARATION;
    copy_bounded_text(app->wizard.staging_root, sizeof(app->wizard.staging_root),
                      prepared_root);
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

    char error_message[sizeof(job->error_message)];
    char prepared_root[sizeof(job->prepared_root)];
    PlayerStageSummary summary;
    PlayerStageTitleOutcome outcome = PLAYER_STAGE_TITLE_STAGED;
    NkResult result = player_stage_title(&job->plan.request, &callbacks, &summary,
                                         &outcome, prepared_root,
                                         sizeof(prepared_root), error_message,
                                         sizeof(error_message));
    SDL_LockMutex(job->mutex);
    job->result = result;
    job->summary = summary;
    job->outcome = outcome;
    snprintf(job->prepared_root, sizeof(job->prepared_root), "%s", prepared_root);
    snprintf(job->error_message, sizeof(job->error_message), "%s", error_message);
    job->finished = true;
    SDL_UnlockMutex(job->mutex);
    staging_push_event(job, PLAYER_STAGING_EVENT_COMPLETE);
    return result == NK_OK ? 0 : 1;
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
    if (!player_app_build_stage_plan(&app->inspecting_game, &job->plan)) {
        free(job);
        player_app_wizard_finish_extraction(app, NK_ERROR_UNSUPPORTED_TITLE,
                                            PLAYER_STAGE_PROFILE_UNAVAILABLE);
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
    char error_message[sizeof(job->error_message)];
    snprintf(error_message, sizeof(error_message), "%s", job->error_message);
    bool finished = job->finished;
    SDL_UnlockMutex(job->mutex);
    if (!finished) return;
    if (job->thread) {
        SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
    }
    if (result == NK_OK &&
        !player_take_staged_title(app, job->prepared_root, &job->summary)) {
        result = NK_ERROR_IO;
        snprintf(error_message, sizeof(error_message),
                 "The game's folder path is too long for the library record.");
    }
    /* Package status is refreshed by the package worker after this result is
       returned to the UI thread. */
    if (result == NK_OK && !player_app_register_staged_game(app)) {
        result = NK_ERROR_IO;
        snprintf(error_message, sizeof(error_message),
                 "The game's files are in place, but the game could not be saved to the "
                 "library. Check that Nakagawa's data folder is writable, then add the disc again.");
    }
    player_app_wizard_finish_extraction(app, result,
                                        error_message[0] ? error_message : NULL);
    job->completion_handled = true;
}

/* A PSN package's conversion into its disc image (#295 phase 2), on a worker
   like ADD TO LIBRARY staging: progress and completion come back as SDL user
   events (data1 is the job), and the wizard's cancel stops it. A cancel or a
   failure removes the partial file. */
typedef struct {
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    PlayerPsnPackage package;
    bool cancel_requested;
    bool finished;
    bool completion_handled;
    bool ok;
    int percent;
    uint64_t done_bytes;
    uint64_t total_bytes;
    char code[48];
    char message[512];
} PlayerPsnJob;

static void psn_job_push_event(PlayerPsnJob *job, int code) {
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_USER;
    event.user.code = code;
    event.user.data1 = job;
    SDL_PushEvent(&event);
}

static bool psn_job_cancelled(void *userdata) {
    PlayerPsnJob *job = (PlayerPsnJob *)userdata;
    if (!job || !job->mutex) return true;
    SDL_LockMutex(job->mutex);
    bool cancelled = job->cancel_requested;
    SDL_UnlockMutex(job->mutex);
    return cancelled;
}

static void psn_job_progress(uint64_t done_bytes, uint64_t total_bytes, void *userdata) {
    PlayerPsnJob *job = (PlayerPsnJob *)userdata;
    int percent = total_bytes ? (int)((done_bytes * 100u) / total_bytes) : 0;
    bool changed;
    if (!job || !job->mutex) return;
    SDL_LockMutex(job->mutex);
    changed = job->percent != percent;
    job->percent = percent;
    job->done_bytes = done_bytes;
    job->total_bytes = total_bytes;
    SDL_UnlockMutex(job->mutex);
    if (changed) psn_job_push_event(job, PLAYER_STAGING_EVENT_PROGRESS);
}

static int SDLCALL psn_job_thread_main(void *userdata) {
    PlayerPsnJob *job = (PlayerPsnJob *)userdata;
    if (!job) return -1;
    PlayerPsnConvertCallbacks callbacks = { psn_job_progress, psn_job_cancelled, job };
    char code[sizeof(job->code)];
    char message[sizeof(job->message)];
    /* A compressed disc image is expanded on the same worker; the package is
       the job's own until it finishes, so its iso_path is filled in here. */
    bool ok = job->package.disc_format != NK_DISC_FORMAT_NONE
        ? player_disc_image_expand(&job->package, &callbacks, code, sizeof(code),
                                   message, sizeof(message))
        : player_psn_package_convert(&job->package, &callbacks, code, sizeof(code),
                                     message, sizeof(message));
    SDL_LockMutex(job->mutex);
    job->ok = ok;
    snprintf(job->code, sizeof(job->code), "%s", code);
    snprintf(job->message, sizeof(job->message), "%s", message);
    job->finished = true;
    SDL_UnlockMutex(job->mutex);
    psn_job_push_event(job, PLAYER_STAGING_EVENT_COMPLETE);
    return ok ? 0 : 1;
}

static void request_psn_cancel(PlayerPsnJob *job) {
    if (!job || !job->mutex) return;
    SDL_LockMutex(job->mutex);
    job->cancel_requested = true;
    SDL_UnlockMutex(job->mutex);
}

static bool psn_job_finished(PlayerPsnJob *job) {
    if (!job || !job->mutex || job->completion_handled) return false;
    SDL_LockMutex(job->mutex);
    bool finished = job->finished;
    SDL_UnlockMutex(job->mutex);
    return finished;
}

static void start_psn_job(PlayerApp *app, PlayerPsnJob **job_slot) {
    if (!app || !job_slot) return;
    if (*job_slot) {
        if (!(*job_slot)->completion_handled) return;
        SDL_DestroyMutex((*job_slot)->mutex);
        free(*job_slot);
        *job_slot = NULL;
    }
    PlayerPsnJob *job = (PlayerPsnJob *)calloc(1, sizeof(*job));
    if (job) {
        job->package = app->psn_package;
        job->mutex = SDL_CreateMutex();
    }
    if (job && job->mutex) {
        job->thread = SDL_CreateThread(psn_job_thread_main, "nakagawa-psn-convert", job);
    }
    if (!job || !job->mutex || !job->thread) {
        if (job && job->mutex) SDL_DestroyMutex(job->mutex);
        free(job);
        bool disc_image = app->psn_package.disc_format != NK_DISC_FORMAT_NONE;
        player_app_psn_conversion_finish(app);
        player_show_picked_refusal(app, "INTERNAL_ERROR",
                                   disc_image ? "Disc Image Not Expanded" : "Package Not Converted",
                                   disc_image ? "Nakagawa could not start expanding the disc image. Try again."
                                              : "Nakagawa could not start converting the package. Try again.");
        return;
    }
    *job_slot = job;
}

static void sync_psn_progress(PlayerApp *app, PlayerPsnJob *job) {
    if (!app || !job || !job->mutex) return;
    SDL_LockMutex(job->mutex);
    uint64_t done = job->done_bytes;
    uint64_t total = job->total_bytes;
    SDL_UnlockMutex(job->mutex);
    player_app_psn_conversion_progress(app, done, total);
}

static void finish_psn_job(PlayerApp *app, PlayerPsnJob *job) {
    if (!psn_job_finished(job)) return;
    if (job->thread) {
        SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
    }
    job->completion_handled = true;
    player_app_psn_conversion_finish(app);
    if (job->ok && job->package.disc_format != NK_DISC_FORMAT_NONE) {
        printf("[PLAYER] Disc image %s expanded into %s\n",
               nk_disc_format_extension(job->package.disc_format), job->package.disc_id);
        player_take_picked_disc_from(app, job->package.iso_path, job->package.pbp_path);
    } else if (job->ok) {
        printf("[PLAYER] PSN package %s converted into its disc image\n", job->package.disc_id);
        player_take_picked_disc(app, job->package.iso_path);
    } else if (strcmp(job->code, "CANCELLED") == 0) {
        if (app->active_view == VIEW_SETUP_WIZARD) {
            snprintf(app->wizard.status_message, sizeof(app->wizard.status_message), "%.200s",
                     job->message);
        }
    } else if (job->package.disc_format != NK_DISC_FORMAT_NONE) {
        player_show_picked_refusal(app, job->code, player_disc_refusal_title(job->code),
                                   job->message);
    } else {
        player_show_picked_refusal(app, job->code, "Package Not Converted", job->message);
    }
}

static void destroy_psn_job(PlayerPsnJob **job_slot) {
    if (!job_slot || !*job_slot) return;
    PlayerPsnJob *job = *job_slot;
    request_psn_cancel(job);
    if (job->thread) {
        SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
    }
    if (job->mutex) SDL_DestroyMutex(job->mutex);
    free(job);
    *job_slot = NULL;
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

/* Console progress for the synchronous routes: one line per whole-percent
 * step, so a long disc shows it is moving without flooding the log. */
static void staging_console_progress(const char *current_path, int percent,
                                     size_t files_extracted, size_t total_files,
                                     void *userdata) {
    (void)current_path;
    int *last_percent = (int *)userdata;
    if (!last_percent || percent <= *last_percent) return;
    *last_percent = percent;
    printf("[PLAYER] STAGING_PROGRESS percent=%d files=%zu/%zu\n", percent,
           files_extracted, total_files);
    fflush(stdout);
}

/* Console progress for a PSN package given as --iso: one line per ten percent. */
static void disc_console_progress(uint64_t done_bytes, uint64_t total_bytes, void *userdata) {
    int *last_percent = (int *)userdata;
    int percent = total_bytes ? (int)((done_bytes * 100u) / total_bytes) : 0;
    if (!last_percent || percent / 10 <= *last_percent / 10) return;
    *last_percent = percent;
    printf("[PLAYER] DISC_EXPAND_PROGRESS percent=%d\n", percent);
    fflush(stdout);
}

static void psn_console_progress(uint64_t done_bytes, uint64_t total_bytes, void *userdata) {
    int *last_percent = (int *)userdata;
    int percent = total_bytes ? (int)((done_bytes * 100u) / total_bytes) : 0;
    if (!last_percent || percent / 10 <= *last_percent / 10) return;
    *last_percent = percent;
    printf("[PLAYER] PSN_CONVERT_PROGRESS percent=%d\n", percent);
    fflush(stdout);
}

/* Stage the inspected game synchronously through the same transaction the
 * wizard's worker runs, then save it to the library the same way. Used by
 * `--stage-only` and by `--launch-now` when the game's files are not in place
 * yet. Returns 0 on success or the process exit code to report. */
static int player_stage_inspected_title(PlayerApp *app) {
    if (!app || !app->inspecting_game.iso_path[0] ||
        !app->inspecting_game.disc_id[0]) {
        fprintf(stderr, "[PLAYER] Staging needs a supported --iso=<path>.\n");
        return 2;
    }
    PlayerStagePlan *plan = (PlayerStagePlan *)calloc(1, sizeof(*plan));
    if (!plan) {
        fprintf(stderr, "[PLAYER] Not enough memory to set up this game's files.\n");
        return 3;
    }
    if (!player_app_build_stage_plan(&app->inspecting_game, plan)) {
        free(plan);
        player_app_wizard_finish_extraction(app, NK_ERROR_UNSUPPORTED_TITLE,
                                            PLAYER_STAGE_PROFILE_UNAVAILABLE);
        fprintf(stderr, "[PLAYER] %s\n", PLAYER_STAGE_PROFILE_UNAVAILABLE);
        printf("[PLAYER] STAGING_RESULT status=FAIL disc_id=%s\n",
               app->inspecting_game.disc_id);
        return 3;
    }

    printf("[PLAYER] Setting up the game's files for %s. A large disc can take a few minutes.\n",
           app->inspecting_game.disc_id);
    fflush(stdout);
    int last_percent = -1;
    PlayerStageCallbacks callbacks = { NULL, staging_console_progress, &last_percent };
    PlayerStageSummary summary;
    PlayerStageTitleOutcome outcome = PLAYER_STAGE_TITLE_STAGED;
    char prepared_root[NK_MAX_PATH];
    char message[1024];
    NkResult result = player_stage_title(&plan->request, &callbacks, &summary,
                                         &outcome, prepared_root, sizeof(prepared_root),
                                         message, sizeof(message));
    free(plan);
    if (result != NK_OK) {
        player_app_wizard_finish_extraction(app, result, message);
        fprintf(stderr, "[PLAYER] %s\n", message);
        printf("[PLAYER] STAGING_RESULT status=FAIL disc_id=%s result=%d\n",
               app->inspecting_game.disc_id, (int)result);
        /* 9: another run holds this title's staging, or a file takes its folder
           name; 4: staging itself failed. */
        return result == NK_ERROR_ALREADY_EXISTS ? 9 : 4;
    }
    if (!player_take_staged_title(app, prepared_root, &summary)) {
        fprintf(stderr, "[PLAYER] The game's folder path is too long for the library record.\n");
        return 6;
    }

    char data_reason[1024] = "";
    NkLaunchDataRootStatus data_status = player_app_game_data_root_status(
        app, &app->inspecting_game, NULL, 0, data_reason, sizeof(data_reason));
    bool data_ready = data_status == NK_LAUNCH_DATA_ROOT_READY ||
                      data_status == NK_LAUNCH_DATA_ROOT_NOT_REQUIRED;
    app->inspecting_game.is_prepared =
        data_ready &&
        player_app_validate_runtime_package(app, &app->inspecting_game,
                                            NULL, NULL, 0) == NK_RUNTIME_PACKAGE_OK;
    app->inspecting_game.status = app->inspecting_game.is_prepared
        ? NK_STATUS_PREPARED : NK_STATUS_SUPPORTED_PREPARATION;

    if (!player_app_register_staged_game(app)) {
        player_app_wizard_finish_extraction(
            app, NK_ERROR_IO,
            "The game's files are in place, but the game could not be saved to the library.");
        fprintf(stderr, "[PLAYER] The game's files are in place, but the game could not be "
                "saved to the library. Check that Nakagawa's data folder is writable, then "
                "run this again; the files will be reused.\n");
        return 7;
    }
    printf("[PLAYER] STAGED_ROOT %s\n", prepared_root);
    if (!data_ready) {
        fprintf(stderr, "[PLAYER] %s\n", data_reason[0] ? data_reason :
                "This game's data folder could not be checked.");
        printf("[PLAYER] STAGING_RESULT status=INCOMPLETE view=PLAYER_VIEW_READY_LIBRARY "
               "disc_id=%s reused=%d assets=%u audio=%u visual=%u layout=%u "
               "runtime=not-ready data_root=missing\n",
               app->inspecting_game.disc_id, outcome == PLAYER_STAGE_TITLE_REUSED,
               (unsigned)summary.extracted_asset_count,
               (unsigned)summary.extracted_audio_count,
               (unsigned)summary.extracted_visual_count,
               (unsigned)summary.extracted_layout_count);
        return 8;
    }
    player_app_wizard_finish_extraction(app, NK_OK, NULL);
    printf("[PLAYER] STAGING_RESULT status=PASS view=PLAYER_VIEW_READY_LIBRARY "
           "disc_id=%s reused=%d assets=%u audio=%u visual=%u layout=%u runtime=%s\n",
           app->inspecting_game.disc_id, outcome == PLAYER_STAGE_TITLE_REUSED,
           (unsigned)summary.extracted_asset_count,
           (unsigned)summary.extracted_audio_count,
           (unsigned)summary.extracted_visual_count,
           (unsigned)summary.extracted_layout_count,
           app->inspecting_game.is_prepared ? "ready" : "not-ready");
    return 0;
}

/* --launch-now stands in for PLAY NOW, so a disc whose files are not in place
 * yet is set up first through the same staging transaction. A disc already in
 * the library keeps its staged files: re-adding it merges the earlier record,
 * and a complete staged tree is reused rather than extracted again. */
static int player_stage_for_launch_now(PlayerApp *app) {
    int existing = player_app_find_game_by_disc_id(app, app->inspecting_game.disc_id);
    if (existing >= 0) {
        (void)player_merge_readded_game(&app->games[existing], &app->inspecting_game);
    }
    NkLaunchDataRootStatus status = player_app_game_data_root_status(
        app, &app->inspecting_game, NULL, 0, NULL, 0);
    if (status != NK_LAUNCH_DATA_ROOT_MISSING) return 0;
    return player_stage_inspected_title(app);
}

/* Bound on one headless --launch-index run. A guest that reaches its own exit
   ends the run sooner; this only caps a run that would otherwise wait forever. */
#define NK_HEADLESS_LAUNCH_TIMEOUT_MS 120000
#define NK_HEADLESS_ACCEPTANCE_TIMEOUT_MS 900000
#define NK_HEADLESS_ACCEPTANCE_MAX_PRESENTED_FRAMES 1000000u
#define NK_HEADLESS_ACCEPTANCE_MAX_TIMEOUT_MS 3600000u

static bool player_parse_positive_u32(const char *text, uint32_t maximum,
                                      uint32_t *out_value) {
    if (!text || !text[0] || !out_value) return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || !end || *end || value == 0ul || value > maximum) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

/* PLAY's package check and process start, on a worker so the launcher keeps
 * drawing (#1298). Checking a retail package hashes every artifact it lists,
 * several hundred MB, and the launch used to do that up to three times on the
 * UI thread: the window stopped responding for about 20 s, Windows replaced it
 * with a ghost, and the runtime's foreground request was refused. The app is
 * touched only by player_app_launch_begin and _finish, on this thread. */
typedef struct {
    SDL_Thread *thread;
    SDL_AtomicInt done;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    uint32_t test_slow_start_ms;
#endif
    PlayerLaunchWork work;
} PlayerLaunchJob;

static void player_launch_job_run(PlayerLaunchJob *job) {
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    /* A synthetic slow start: the launch takes this long before its checks. */
    if (job->test_slow_start_ms > 0u) SDL_Delay(job->test_slow_start_ms);
#endif
    player_launch_work_run(&job->work);
    SDL_SetAtomicInt(&job->done, 1);
}

static int SDLCALL player_launch_thread_main(void *data) {
    player_launch_job_run((PlayerLaunchJob *)data);
    return 0;
}

static PlayerLaunchJob *player_launch_job_start(PlayerApp *app, int game_index) {
    if (!app || app->launch_pending || app->is_game_running ||
        game_index < 0 || game_index >= app->game_count) {
        return NULL;
    }
    PlayerLaunchJob *job = (PlayerLaunchJob *)calloc(1, sizeof(*job));
    if (!job) {
        (void)player_app_launch_game(app, game_index);
        return NULL;
    }
    if (!player_app_launch_begin(app, game_index, &job->work)) {
        free(job);
        return NULL;
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    {
        const char *slow = getenv("NK_UI_TEST_SLOW_START_MS");
        uint32_t slow_ms = 0u;
        if (slow && player_parse_positive_u32(slow, 60000u, &slow_ms)) {
            job->test_slow_start_ms = slow_ms;
        }
    }
#endif
    app->launch_pending = true;
    snprintf(app->launch_pending_disc_id, sizeof(app->launch_pending_disc_id), "%s",
             job->work.game.disc_id);
    job->thread = SDL_CreateThread(player_launch_thread_main, "nakagawa-launch", job);
    if (!job->thread) {
        /* No worker: run the launch here, as the launcher always did. */
        fprintf(stderr, "[PLAYER] Launch worker could not start (%s); launching inline.\n",
                SDL_GetError());
        player_launch_job_run(job);
    }
    return job;
}

/* Hands a finished launch to the app. Returns the job while it still runs and
 * NULL once it is handed back; `wait` blocks until then (shutdown only). */
static PlayerLaunchJob *player_launch_job_poll(PlayerApp *app, PlayerLaunchJob *job,
                                               bool wait) {
    if (!job) return NULL;
    if (!wait && SDL_GetAtomicInt(&job->done) == 0) return job;
    if (job->thread) SDL_WaitThread(job->thread, NULL);
    app->launch_pending = false;
    app->launch_pending_disc_id[0] = '\0';
    (void)player_app_launch_finish(app, &job->work);
    free(job);
    return NULL;
}

/* PLAY for one library entry from the command line, shared by --launch-index
 * and `--launch-now --headless-launch`: the launcher's own player_app_launch_game,
 * then a wait for the child. A headless run is bounded by the timeout above. */
static int player_run_command_line_launch(PlayerApp *app, int game_index,
                                          const char *route) {
    if (!player_app_launch_game(app, game_index)) {
        const char *reason = app->launch_session.last_error[0]
            ? app->launch_session.last_error
            : app->last_error.boundary_text[0] ? app->last_error.boundary_text
                                               : app->last_error.message;
        fprintf(stderr, "[PLAYER] %s failed: %s\n", route, reason);
        return 4;
    }
    printf("[PLAYER] Launch argv contains --gui: %s\n",
           app->launch_session.argv_has_gui ? "yes" : "no");
    if (!app->launch_session.argv_has_gui && !app->launch_headless) {
        /* A windowed launch that lost its --gui request is a failure. */
        player_app_stop_game(app);
        return 5;
    }
    if (app->headless_accept_presented_frames > 0u) {
        uint32_t timeout_ms = app->headless_launch_timeout_ms > 0u
            ? app->headless_launch_timeout_ms : NK_HEADLESS_ACCEPTANCE_TIMEOUT_MS;
        int child_code = nk_launch_wait(&app->launch_session, (int)timeout_ms);
        app->is_game_running = false;
        if (child_code < 0) {
            fprintf(stderr,
                    "[PLAYER] HEADLESS_ACCEPTANCE status=FAIL reason=timeout timeout_ms=%u\n",
                    timeout_ms);
            player_app_stop_game(app);
            return 7;
        }
        if (child_code == NK_LAUNCH_EXIT_ACCEPTANCE_REACHED) {
            printf("[PLAYER] HEADLESS_ACCEPTANCE status=PASS presented_frames=%u child_exit=%d\n",
                   app->headless_accept_presented_frames, child_code);
            return 0;
        }
        if (child_code == NK_LAUNCH_EXIT_ACCEPTANCE_PRESENTER_UNAVAILABLE) {
            fprintf(stderr,
                    "[PLAYER] HEADLESS_ACCEPTANCE status=FAIL reason=presenter_unavailable child_exit=%d\n",
                    child_code);
            return 8;
        }
        fprintf(stderr,
                "[PLAYER] HEADLESS_ACCEPTANCE status=FAIL reason=checkpoint_not_reached child_exit=%d presented_frames=%u\n",
                child_code, app->headless_accept_presented_frames);
        return 8;
    }
    if (!app->launch_session.argv_has_gui) {
        /* Headless launch: the same player-owned session, spawned without a
           window so a host with no display can still run it. The bound is
           the whole check -- a run that has not ended inside it is reported
           as a timeout with a distinct status, never as a launch. */
        int headless_code = nk_launch_wait(&app->launch_session,
                                           NK_HEADLESS_LAUNCH_TIMEOUT_MS);
        app->is_game_running = false;
        if (headless_code < 0) {
            fprintf(stderr,
                    "[PLAYER] Headless launch did not finish within %d ms; stopping the child.\n",
                    NK_HEADLESS_LAUNCH_TIMEOUT_MS);
            player_app_stop_game(app);
            return 7;
        }
        printf("[PLAYER] Headless launch child exited with code %d.\n", headless_code);
        return headless_code;
    }
    int child_code = nk_launch_wait(&app->launch_session, -1);
    app->is_game_running = false;
    printf("[PLAYER] %s child exited with code %d.\n", route, child_code);
    return child_code < 0 ? 6 : child_code;
}

/* --user-data-root=<folder> makes this process keep its library, title
 * manifests, packages and staged games in <folder>, the same meaning as
 * nk_cli's --user-data-root, and its settings, cache, logs and saves in the
 * folder's config, cache, logs and saves folders (nk_platform_set_app_data_dir_
 * override). It must apply before anything resolves a per-user folder. */
static bool player_apply_user_data_root(int argc, char *argv[]) {
    static const char prefix[] = "--user-data-root=";
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], prefix, sizeof(prefix) - 1u) != 0) continue;
        const char *requested = argv[i] + sizeof(prefix) - 1u;
        char absolute[NK_MAX_PATH];
        if (!requested[0] ||
            !nk_platform_mkdir_p_private(requested) ||
            !nk_platform_absolute_path(requested, absolute, sizeof(absolute)) ||
            strlen(absolute) + 64u >= NK_MAX_PATH ||
            !nk_platform_set_app_data_dir_override(absolute)) {
            fprintf(stderr,
                    "[PLAYER] The data folder given with --user-data-root could not be "
                    "used: '%s'. Choose a short folder path you can write to and try again.\n",
                    requested);
            return false;
        }
    }
    return true;
}

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
            PLAYER_DATA_DIR_UNAVAILABLE_TEXT " and retry.");
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
    app->prerequisite_session_generation = app->build_session.session_generation;
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

/* After a factory reset: a fresh copy of this player, started on its own so
 * that it outlives this process (nk_platform_spawn_process ties a child to
 * its parent's job). It finds no user data and opens on a first run. False
 * where this program's own path is unknown, or the start failed. */
static bool player_start_fresh_copy(void) {
    char exe[NK_MAX_PATH];
    if (!nk_platform_get_executable_path(exe, sizeof(exe))) return false;
    const char *args[] = { exe, NULL };
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) return false;
    (void)SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    (void)SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    /* A launcher started from a terminal keeps writing there, and one started
       from the desktop finds no streams and opens player-output.txt itself. */
    (void)SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                                SDL_PROCESS_STDIO_INHERITED);
    (void)SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                                SDL_PROCESS_STDIO_INHERITED);
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) return false;
    SDL_DestroyProcess(process);
    return true;
}

#if defined(_WIN32) || defined(_WIN64)
/* The Windows player is a GUI-subsystem program (-mwindows, CMake WIN32_EXECUTABLE), so a
 * double-click starts with no console window. A process that already has standard handles, one
 * started from cmd, PowerShell or Git Bash, keeps them. Without handles, the streams go to the
 * parent terminal's console when there is one, and otherwise to player-output.txt in the logs
 * folder under the player's data folder. Call after the data root is applied. */
static void player_setup_standard_streams(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != NULL && out != INVALID_HANDLE_VALUE) return;

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        SetConsoleOutputCP(CP_UTF8);
        (void)freopen("CONOUT$", "w", stdout);
        (void)freopen("CONOUT$", "w", stderr);
        return;
    }

    char dir[NK_MAX_PATH];
    char path[NK_MAX_PATH];
    if (!nk_platform_get_path(NK_PATH_LOGS, dir, sizeof(dir)) || !nk_platform_mkdir_p(dir) ||
        snprintf(path, sizeof(path), "%s%cplayer-output.txt", dir,
                 nk_platform_path_separator()) >= (int)sizeof(path)) {
        return;
    }
    /* The streams themselves are reopened on the file. A GUI process started
       without standard handles has stdout and stderr bound to no descriptor
       (_fileno returns -2), so pointing descriptors 1 and 2 at the file with
       _dup2 left both streams writing nowhere and the file empty. Each run
       starts the file afresh; both streams then append, so their lines
       interleave in order instead of overwriting each other. */
    WCHAR wpath[NK_MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath,
                            (int)(sizeof(wpath) / sizeof(wpath[0]))) <= 0) {
        return;
    }
    FILE *fresh = _wfopen(wpath, L"wb");
    if (!fresh) return;
    fclose(fresh);
    if (!_wfreopen(wpath, L"ab", stdout)) return;
    if (!_wfreopen(wpath, L"ab", stderr)) return;
    /* Code that writes to the process's standard handles rather than the CRT
       streams reaches the same file. */
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
}
#endif

/* The game-found test views' checks: the real preflight for a disc with a
   readable PARAM.SFO and a plain or an encrypted EBOOT.BIN. */
static void player_test_view_preflight(PlayerApp *app, bool encrypted) {
    NkIsoExecutableReport executables;
    memset(&executables, 0, sizeof(executables));
    executables.eboot.present = true;
    executables.eboot.size_bytes = 4096u;
    executables.eboot.kind = encrypted ? NK_ISO_EXEC_PSP_ENCRYPTED : NK_ISO_EXEC_MIPS_ELF32;
    if (!encrypted) {
        executables.selected = NK_ISO_EXEC_SELECTION_EBOOT;
        snprintf(executables.selected_path, sizeof(executables.selected_path), "EBOOT.BIN");
    }
    player_app_build_compatibility_preflight(app, true, true, &executables);
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

    if (!player_apply_user_data_root(argc, argv)) return 2;
    /* Compressed disc images: CHD is read through the vendored libchdr. */
    nk_disc_chd_register();
#if defined(_WIN32) || defined(_WIN64)
    player_setup_standard_streams();
    /* The Windows CRT has no line buffering: _IOLBF is full buffering, so a file or pipe got
       nothing until 4 KiB had piled up or the player exited, and player-output.txt stayed empty
       while the player ran (#1303). Unbuffered, each printf reaches it as one write. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif
    player_report_legacy_data_root();

    /* Static storage: a full library (NK_MAX_GAMES titles) makes the app state
       tens of megabytes, far more than a thread's stack. */
    static PlayerApp app;
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
    bool ui_test_waiting_for_package_status = false;
    NkRuntimePackageStatus ui_test_wait_package_status_target =
        NK_RUNTIME_PACKAGE_UNKNOWN;
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
    float capture_render_scale = 1.0f;
    /* Demo fixtures are opt-in. Defaulting them on meant a normal first
       launch with an empty library populated TEST00005 and, through
       player_app_add_game, PERSISTED it: a new user's first sight of the
       program was a game they do not have, written into their real library
       file, instead of the documented empty-library prompt. */
    bool populate_sample = false;
    bool force_empty = false;
    /* The nk_ui launcher is the only launcher. The regression-test build keeps
       the old renderer behind --ui=legacy for the tests that still pin it;
       the shipped player has no way to reach it. */
    bool ui_next_requested = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nakagawa_player [--user-data-root=<folder>] "
                   "[--iso=<path> [--stage|--stage-only|--launch-now]] "
                   "[--runtime-root=<path>] [--view=<name>] [--screenshot=<bmp>] "
                   "[--width=W --height=H [--render-scale=S]] "
                   "[--launch-index=N] [--headless-launch] "
                   "[--headless-accept-presented-frames=N] "
                   "[--headless-timeout-ms=N] "
                   "[--experimental-60hz]\n");
            return 0;
        } else if (strcmp(argv[i], "--experimental-60hz") == 0) {
            player_app_set_experimental_60hz_session(&app, true);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        } else if (strcmp(argv[i], "--ui=next") == 0) {
            ui_next_requested = true;
        } else if (strcmp(argv[i], "--ui=legacy") == 0) {
            ui_next_requested = false;
#endif
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
        } else if (strncmp(argv[i], "--user-data-root=", 17) == 0) {
            /* Applied by player_apply_user_data_root before the library loaded. */
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
        } else if (strncmp(argv[i], "--render-scale=", 15) == 0) {
            char *end = NULL;
            float value = strtof(argv[i] + 15, &end);
            if (end == argv[i] + 15 || *end != '\0' ||
                !(value >= PLAYER_UI_MIN_SCALE && value <= PLAYER_UI_MAX_SCALE)) {
                fprintf(stderr, "[PLAYER] --render-scale must be a number from 0.25 to 8.\n");
                return 2;
            }
            capture_render_scale = value;
        } else if (strcmp(argv[i], "--demo") == 0) {
            populate_sample = true;
        } else if (strcmp(argv[i], "--headless-launch") == 0) {
            app.launch_headless = true;
        } else if (strncmp(argv[i], "--headless-accept-presented-frames=", 35) == 0) {
            if (!player_parse_positive_u32(
                    argv[i] + 35, NK_HEADLESS_ACCEPTANCE_MAX_PRESENTED_FRAMES,
                    &app.headless_accept_presented_frames)) {
                fprintf(stderr,
                        "[PLAYER] Invalid --headless-accept-presented-frames value.\n");
                return 2;
            }
        } else if (strncmp(argv[i], "--headless-timeout-ms=", 22) == 0) {
            if (!player_parse_positive_u32(argv[i] + 22,
                                           NK_HEADLESS_ACCEPTANCE_MAX_TIMEOUT_MS,
                                           &app.headless_launch_timeout_ms)) {
                fprintf(stderr, "[PLAYER] Invalid --headless-timeout-ms value.\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--empty") == 0) {
            force_empty = true;
        } else if (strcmp(argv[i], "--wizard") == 0) {
            force_empty = true;
            test_view = "wizard";
        }
    }

    if ((app.headless_accept_presented_frames > 0u &&
         (!app.launch_headless || (!launch_index_requested && !launch_now))) ||
        (app.headless_launch_timeout_ms > 0u &&
         app.headless_accept_presented_frames == 0u)) {
        fprintf(stderr,
                "[PLAYER] Headless acceptance requires --headless-launch and a launch route; "
                "--headless-timeout-ms requires an acceptance frame count.\n");
        return 2;
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
            if (report[0]) {
                snprintf(app.title_manifest_report,
                         sizeof(app.title_manifest_report), "%s", report);
            }
            if (loaded > 0) printf("[PLAYER] Loaded %d title manifest(s) from %s\n", loaded, manifest_dir);
            if (report[0]) fprintf(stderr, "[PLAYER] Title manifests not loaded:\n%s", report);
        }
    }
    /* An experimental entry saved with no selected executable recovers the
       one its profile binds, without a hand edit of library.json (#1276). */
    for (int i = 0; i < app.game_count; i++) {
        (void)player_app_adopt_profile_executable(&app, i);
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

    /* A PSN package given as --iso converts once, here, and its disc image is
       then inspected like any --iso (#295 phase 2). */
    char converted_iso_path[NK_MAX_PATH];
    if (initial_iso_path && player_path_is_pbp_package(initial_iso_path)) {
        PlayerPsnPackage package;
        char code[48];
        char message[512];
        bool ok = player_psn_package_inspect(initial_iso_path, &package, code, sizeof(code),
                                             message, sizeof(message));
        if (ok && !package.iso_ready) {
            int last_percent = -1;
            PlayerPsnConvertCallbacks hooks = { psn_console_progress, NULL, &last_percent };
            printf("[PLAYER] Converting PSN package %s into its disc image...\n",
                   package.disc_id);
            ok = player_psn_package_convert(&package, &hooks, code, sizeof(code), message,
                                            sizeof(message));
        }
        if (!ok) {
            fprintf(stderr, "[PLAYER] %s: %s\n", code, message);
            if (stage_only || app.launch_headless) return 1;
            populate_sample = false;
            player_app_set_error(&app, code, "Package Not Supported", message,
                                 "Try Another File", VIEW_LIBRARY);
            initial_iso_path = NULL;
        } else {
            snprintf(converted_iso_path, sizeof(converted_iso_path), "%s", package.iso_path);
            initial_iso_path = converted_iso_path;
        }
    }

    /* A compressed disc image given as --iso expands once, here, and the
       expanded .iso is then inspected like any --iso. */
    char expanded_iso_path[NK_MAX_PATH];
    const char *initial_source_image = NULL;
    if (initial_iso_path && nk_disc_image_format(initial_iso_path) != NK_DISC_FORMAT_NONE) {
        PlayerPsnPackage package;
        char code[48];
        char message[512];
        bool ok = player_disc_image_inspect(initial_iso_path, &package, code, sizeof(code),
                                            message, sizeof(message));
        if (ok && !package.iso_ready) {
            int last_percent = -1;
            PlayerPsnConvertCallbacks hooks = { disc_console_progress, NULL, &last_percent };
            printf("[PLAYER] Expanding disc image %s into a plain disc image...\n",
                   nk_disc_format_extension(package.disc_format));
            ok = player_disc_image_expand(&package, &hooks, code, sizeof(code), message,
                                          sizeof(message));
        }
        if (!ok) {
            fprintf(stderr, "[PLAYER] %s: %s\n", code, message);
            if (stage_only || app.launch_headless) return 1;
            populate_sample = false;
            player_app_set_error(&app, code, player_disc_refusal_title(code), message,
                                 "Try Another File", VIEW_LIBRARY);
            initial_iso_path = NULL;
        } else {
            snprintf(expanded_iso_path, sizeof(expanded_iso_path), "%s", package.iso_path);
            initial_source_image = initial_iso_path;
            initial_iso_path = expanded_iso_path;
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
            if (!player_populate_inspected_game(&app, initial_iso_path, initial_source_image,
                                                &res, profile_error, sizeof(profile_error))) {
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
                fprintf(stderr, "[PLAYER] Disc %s is not in Nakagawa's title list yet, so "
                        "its files cannot be set up. Nothing was changed.\n",
                        app.inspecting_game.disc_id);
                return 3;
            }

            if (res.is_supported && stage_initial_iso) {
                /* --stage enters the same wizard state used by the file picker,
                   while --stage-only runs the same staging transaction
                   synchronously for the command line and CI. */
                player_app_start_setup_wizard(&app);
                app.wizard.iso_selected = true;
                app.wizard.step = WIZARD_STEP_INSPECT_VERIFY;
                player_app_build_compatibility_preflight(&app, res.success,
                                                          res.param_sfo_parsed,
                                                          &res.executables);
                if (stage_only) {
                    return player_stage_inspected_title(&app);
                }
                player_app_wizard_begin_extraction(&app);
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
            int staging_exit = add_before_launch ? player_stage_for_launch_now(&app) : 0;
            bool stored = add_before_launch && staging_exit == 0 &&
                          player_app_add_game(&app, &app.inspecting_game);

            if (launch_now && app.inspecting_game.is_experimental) {
                const char *message =
                    "Launch now is unavailable for experimental titles. Generic ISO-to-Play "
                    "support is in the works. This title was not added; review its "
                    "compatibility checks before adding it to the library.";
                fprintf(stderr, "[PLAYER] EXPERIMENTAL_LAUNCH_UNAVAILABLE: %s\n", message);
                player_app_set_error(&app, "EXPERIMENTAL_LAUNCH_UNAVAILABLE",
                                     "Experimental Title Cannot Launch Yet", message,
                                     "Review Compatibility", VIEW_EXPERIMENTAL_TITLE);
            } else if (add_before_launch && staging_exit != 0) {
                /* The staging transaction already explained itself on stderr. */
                if (app.launch_headless) return staging_exit;
                player_app_set_error(&app, "GAME_FILES_NOT_READY", "Game Files Not Set Up",
                                     app.wizard.extraction_error[0]
                                         ? app.wizard.extraction_error
                                         : "This game's files could not be set up.",
                                     "Return to Library", VIEW_LIBRARY);
            } else if (add_before_launch && !stored) {
                fprintf(stderr, "[PLAYER] Could not store %s in the library.\n",
                        app.inspecting_game.disc_id);
                if (app.launch_headless) return 2;
                player_app_set_error(&app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                     "The title could not be stored. The library may be full, "
                                     "or the user data directory is not writable.",
                                     "Return to Library", VIEW_LIBRARY);
            } else if (should_store) {
                player_app_set_view(&app, app.inspecting_game.is_experimental
                    ? VIEW_EXPERIMENTAL_TITLE : VIEW_SUPPORTED_TITLE);
                if (stored) {
                    printf("[PLAYER] Launching %s now...\n", app.inspecting_game.disc_id);
                    /* nk_library_add_or_update updates an existing record in
                       place rather than appending it, so in a library holding
                       several games the last entry is a DIFFERENT title
                       whenever this disc was already known. Find the entry by
                       its inspected disc ID. */
                    int game_idx = player_app_find_game_by_disc_id(&app, app.inspecting_game.disc_id);
                    if (game_idx < 0) {
                        fprintf(stderr, "[PLAYER] Disc %s is not in the library after adding it.\n",
                                app.inspecting_game.disc_id);
                        if (app.launch_headless) return 4;
                        player_app_set_error(&app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                             "The title could not be found in the library after "
                                             "adding it. Add the disc again from the library.",
                                             "Return to Library", VIEW_LIBRARY);
                    } else if (app.launch_headless) {
                        /* --launch-now stands in for pressing PLAY NOW: the
                           same launch function and the same bounded wait as
                           --launch-index. */
                        return player_run_command_line_launch(&app, game_idx, "--launch-now");
                    } else if (player_app_launch_game(&app, game_idx)) {
                        app.launch_time_ms = SDL_GetTicks();
                        /* The disc is in the library now, so the card's
                           ADD TO LIBRARY would be stale: show the library,
                           as the wizard's LAUNCH GAME NOW does. */
                        player_app_set_view(&app, VIEW_LIBRARY);
                    } else {
                        fprintf(stderr, "[PLAYER] --launch-now could not start %s: %s\n",
                                app.inspecting_game.disc_id,
                                app.launch_session.last_error[0]
                                    ? app.launch_session.last_error
                                    : app.last_error.message);
                    }
                }
            } else {
                player_app_set_view(&app, VIEW_UNSUPPORTED_TITLE);
            }
            }
        } else {
            fprintf(stderr, "[PLAYER] Failed to inspect ISO: %s\n", initial_iso_path);
            if (stage_only || app.launch_headless) return 1;
            const bool named_boundary = res.boundary_code[0] != '\0';
            const char *message = res.error_message[0]
                ? res.error_message
                : "The selected file is not a valid ISO9660 disc image.";
            player_app_set_error(
                &app,
                named_boundary ? res.boundary_code : "ISO_CORRUPT",
                named_boundary ? player_disc_refusal_title(res.boundary_code)
                               : "Not a PSP Disc Image",
                named_boundary ? message : k_invalid_disc_message,
                "Try Another File", VIEW_LIBRARY);
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
        } else if (strcmp(test_view, "build-preflight-encrypted") == 0 ||
                   strcmp(test_view, "build-preflight-missing-manifest") == 0) {
            /* #1286: an imported title whose current build inputs are refused
               must reach the library title page with BUILD PACKAGE gated. */
            app.game_count = 0;
            app.selected_game_index = -1;
            GameRecord game;
            memset(&game, 0, sizeof(game));
            snprintf(game.disc_id, sizeof(game.disc_id), "TEST00009");
            snprintf(game.title_name, sizeof(game.title_name),
                     "Synthetic Build Preflight Fixture");
            snprintf(game.disc_version, sizeof(game.disc_version), "1.00");
            snprintf(game.title_id, sizeof(game.title_id),
                     "experimental-test00009");
            game.status = NK_STATUS_IDENTIFIED;
            game.is_experimental = true;
            if (strcmp(test_view, "build-preflight-encrypted") == 0) {
                game.executable_eboot_kind = NK_ISO_EXEC_PSP_ENCRYPTED;
                game.executable_selection = NK_ISO_EXEC_SELECTION_NONE;
            } else {
                snprintf(game.selected_executable,
                         sizeof(game.selected_executable), "EBOOT.BIN");
                game.executable_eboot_kind = NK_ISO_EXEC_MIPS_ELF32;
                game.executable_selection = NK_ISO_EXEC_SELECTION_EBOOT;
            }
            app.games[0] = game;
            app.game_count = 1;
            app.selected_game_index = 0;
            PlayerRuntimePackageCacheEntry *package =
                &app.runtime_package_cache[0];
            memset(package, 0, sizeof(*package));
            snprintf(package->disc_id, sizeof(package->disc_id), "%s",
                     game.disc_id);
            snprintf(package->title_id, sizeof(package->title_id), "%s",
                     game.title_id);
            snprintf(package->selected_executable,
                     sizeof(package->selected_executable), "%s",
                     game.selected_executable);
            package->status_valid = true;
            package->status = NK_RUNTIME_PACKAGE_MISSING;
            player_app_set_view(&app, VIEW_LIBRARY);
        } else if (strcmp(test_view, "build-ready-experimental") == 0) {
            /* #1276: an experimental entry imported while its EBOOT.BIN was
               encrypted has no selected executable; its build has just bound
               EBOOT.BIN in the profile and passed. In memory only. */
            app.game_count = 0;
            app.selected_game_index = -1;
            GameRecord experimental;
            memset(&experimental, 0, sizeof(experimental));
            snprintf(experimental.disc_id, sizeof(experimental.disc_id), "TEST00009");
            snprintf(experimental.title_name, sizeof(experimental.title_name),
                     "Nakagawa Synthetic Experimental Fixture");
            snprintf(experimental.disc_version, sizeof(experimental.disc_version), "1.00");
            snprintf(experimental.title_id, sizeof(experimental.title_id),
                     "experimental-test00009");
            experimental.status = NK_STATUS_IDENTIFIED;
            experimental.is_experimental = true;
            app.games[0] = experimental;
            app.game_count = 1;
            app.selected_game_index = 0;
            player_app_set_view(&app, VIEW_BUILDING_PACKAGE);
            package_builder_init_session(&app.build_session, experimental.disc_id,
                                         experimental.title_name);
            app.build_session.is_complete = true;
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
            snprintf(app.inspecting_game.title_id, sizeof(app.inspecting_game.title_id),
                     "synthetic-allegrex-v1");
            player_test_view_preflight(&app, false);
            player_app_set_view(&app, VIEW_SUPPORTED_TITLE);
        } else if (strcmp(test_view, "experimental") == 0 ||
                   strcmp(test_view, "experimental-blocked") == 0) {
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "TEST00007");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name),
                     "Nakagawa Synthetic Experimental Fixture");
            snprintf(app.inspecting_game.title_id, sizeof(app.inspecting_game.title_id),
                     "synthetic-ui-test-v1");
            snprintf(app.inspecting_game.selected_executable,
                     sizeof(app.inspecting_game.selected_executable), "EBOOT.BIN");
            app.inspecting_game.status = NK_STATUS_IDENTIFIED;
            app.inspecting_game.is_experimental = true;
            player_test_view_preflight(&app, strcmp(test_view, "experimental-blocked") == 0);
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
        } else if (strcmp(test_view, "wizard-options") == 0) {
            player_app_start_setup_wizard(&app);
            app.wizard.step = WIZARD_STEP_ONLINE_OPTIONS;
        } else if (strcmp(test_view, "online-settings") == 0) {
            player_app_set_view(&app, VIEW_ONLINE_SETTINGS);
        } else if (strcmp(test_view, "build-cache") == 0) {
            player_app_set_view(&app, VIEW_BUILD_CACHE);
        } else if (strcmp(test_view, "updates") == 0) {
            player_app_set_view(&app, VIEW_UPDATES);
        } else if (strcmp(test_view, "factory-reset") == 0) {
            player_app_set_view(&app, VIEW_CONFIRM_FACTORY_RESET);
        } else if (strcmp(test_view, "psp-fonts") == 0) {
            player_app_refresh_font_status(&app);
            player_app_set_view(&app, VIEW_PSP_FONTS);
        } else if (strcmp(test_view, "developer-log") == 0) {
            app.settings.developer_mode = true;
            player_dev_log_add(&app, "test", "synthetic developer log line");
            (void)player_app_open_developer_log(&app);
        } else if (strcmp(test_view, "wizard5") == 0) {
            player_app_start_setup_wizard(&app);
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "UCUS98701");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name), "Hot Shots Tennis: Get a Grip");
            app.wizard.step = WIZARD_STEP_READY_LAUNCH;
        } else if (strcmp(test_view, "build-failed") == 0) {
            /* A build session that has ended in failure (#1277). */
            player_app_set_view(&app, VIEW_BUILDING_PACKAGE);
            package_builder_init_session(&app.build_session, "TEST80001",
                                         "Synthetic Package Fixture");
            app.build_session.is_failed = true;
            app.build_session.current_stage = PACKAGE_BUILD_STAGE_FAILED;
            snprintf(app.build_session.current_stage_name,
                     sizeof(app.build_session.current_stage_name), "compile");
            snprintf(app.build_session.failure_boundary,
                     sizeof(app.build_session.failure_boundary),
                     "Synthetic compile failure.");
        } else if (strcmp(test_view, "building") == 0 || strcmp(test_view, "building_package") == 0 ||
                   strcmp(test_view, "building-parts") == 0) {
            player_app_set_view(&app, VIEW_BUILDING_PACKAGE);
            const char *disc = (app.selected_game_index >= 0 && app.selected_game_index < app.game_count)
                ? app.games[app.selected_game_index].disc_id : "TEST80001";
            const char *title = (app.selected_game_index >= 0 && app.selected_game_index < app.game_count)
                ? app.games[app.selected_game_index].title_name : "Synthetic Package Fixture";
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
            if (strcmp(test_view, "building-parts") == 0) {
                /* The compile stage's part count, through the same parser and state
                   machine as a line nk_cli writes. */
                static const char parts_line[] =
                    "{\"stage\":\"compile\",\"status\":\"RUNNING\","
                    "\"message\":\"Compiling part 13 of 40\",\"completed\":12,\"total\":40,"
                    "\"notice\":\"Building uses most of your PC's power for a few minutes, "
                    "so other apps may feel slower until it finishes.\"}";
                PackageProgressEvent parts_event;
                if (package_builder_parse_progress_line(parts_line, sizeof(parts_line) - 1u,
                                                        &parts_event)) {
                    package_builder_apply_event(&app.build_session, &parts_event);
                }
            }
        }
    }

#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (ui_test_iso_path && app.selected_game_index >= 0 &&
        app.selected_game_index < app.game_count) {
        snprintf(app.games[app.selected_game_index].iso_path,
                 sizeof(app.games[app.selected_game_index].iso_path), "%s",
                 ui_test_iso_path);
    }
    const char *bind_test_eboot = getenv("NK_UI_TEST_BIND_EBOOT");
    if (ui_test_mode && ui_test_iso_path && bind_test_eboot &&
        strcmp(bind_test_eboot, "1") == 0 &&
        app.selected_game_index >= 0 && app.selected_game_index < app.game_count) {
        GameRecord *game = &app.games[app.selected_game_index];
        snprintf(game->selected_executable, sizeof(game->selected_executable),
                 "EBOOT.BIN");
        game->executable_eboot_kind = NK_ISO_EXEC_MIPS_ELF32;
        game->executable_selection = NK_ISO_EXEC_SELECTION_EBOOT;
    }
    if (ui_test_iso_path) {
        for (int i = 0; i < app.game_count; i++) {
            if (strcmp(app.games[i].disc_id, "TEST00006") == 0) {
                snprintf(app.games[i].iso_path, sizeof(app.games[i].iso_path),
                         "%s", ui_test_iso_path);
            }
        }
    }
    if (ui_test_fake_game_running) {
        /* "1": some game runs. A disc ID names the running game, whose window
           is up; "starting:<disc ID>" is that game before its window opens. */
        const char *fake = getenv("NK_UI_TEST_GAME_RUNNING");
        bool starting = strncmp(fake, "starting:", 9) == 0;
        const char *disc = starting ? fake + 9 : fake;
        app.is_game_running = true;
        if (strcmp(disc, "1") != 0) {
            snprintf(app.launch_session.disc_id, sizeof(app.launch_session.disc_id),
                     "%s", disc);
            /* A launch carries the disc's title, as nk_launch_prepare_session
               copies it from the library record. */
            int running_index = player_app_find_game_by_disc_id(&app, disc);
            if (running_index >= 0) {
                snprintf(app.launch_session.game_title,
                         sizeof(app.launch_session.game_title), "%s",
                         app.games[running_index].title_name);
            }
            app.child_window_ready = !starting;
        }
    }
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
        } else if (strcmp(ui_test_error_code, "RUNTIME_SEMANTIC_BOUNDARY") == 0 ||
                   strcmp(ui_test_error_code, "RUNTIME_PREMATURE_EXIT") == 0 ||
                   strcmp(ui_test_error_code, "RUNTIME_ERROR_EXIT") == 0) {
            /* The cards a finished game session shows (#1282), from a synthetic
               stop of the first library game. */
            const char *disc = app.game_count > 0 ? app.games[0].disc_id : "";
            static const char stop_event[] =
                "BOOT_EVENT phase=stop reason=semantic-boundary "
                "boundary=unsupported-import-scenetadhocinit issue=308";
            player_dev_log_add(&app, "runtime", "%s", stop_event);
            /* The run's output, kept as logs/game-<disc>-last.txt as a real
               stop keeps it (#1303); the report reads its boot events there. */
            char logs_dir[NK_MAX_PATH];
            char run_log[NK_MAX_PATH + 64] = "";
            if (disc[0] && nk_platform_get_path(NK_PATH_LOGS, logs_dir, sizeof(logs_dir)) &&
                snprintf(run_log, sizeof(run_log), "%s%cgame-%s-last.txt", logs_dir,
                         nk_platform_path_separator(), disc) < (int)sizeof(run_log)) {
                FILE *log = nk_fopen_utf8(run_log, "wb");
                if (!log || fprintf(log, "synthetic runtime output\n%s\n", stop_event) < 0) {
                    run_log[0] = '\0';
                }
                if (log && fclose(log) != 0) run_log[0] = '\0';
            }
            player_app_set_game_stop_error(&app, ui_test_error_code,
                                           "unsupported-import-scenetadhocinit", 308u,
                                           strcmp(ui_test_error_code, "RUNTIME_ERROR_EXIT") == 0
                                               ? 3 : 9,
                                           strcmp(ui_test_error_code,
                                                  "RUNTIME_PREMATURE_EXIT") == 0 ? 120u : 9000u,
                                           disc, run_log[0] ? run_log : NULL);
        } else if (strcmp(ui_test_error_code, "PACKAGE_BUILD_REFUSED") == 0) {
            /* A preflight refusal (#1286): the build never started. */
            const char *disc = app.game_count > 0 ? app.games[0].disc_id : "TEST80001";
            package_builder_init_session(&app.build_session, disc, "Synthetic Package Fixture");
            player_app_set_build_error(&app, "preflight",
                                       "ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and "
                                       "built-in decryption stopped: KEY_REQUIRED.",
                                       NULL);
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
        return player_run_command_line_launch(&app, launch_index, "--launch-index");
    }

    nk_title_manifest_set_cancel_check(player_closing, NULL);

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
#if !defined(_WIN32) && !defined(_WIN64)
    player_set_window_icon(window);
#endif
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
    bool launcher_handoff_attempted = false;
    PlayerLaunchJob *launch_job = NULL;
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
    PlayerUiNk *next_ui = NULL;
    if (ui_next_requested) {
        char ui_error[256] = "";
        next_ui = player_ui_nk_create(renderer, window,
                                      ui_error, sizeof(ui_error));
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        s_ui_test_next_ui = next_ui;
#endif
        if (next_ui == NULL) {
            /* There is no other launcher to fall back to: say why, once, and
               stop. A build without the launcher's support needs building
               again; a missing file needs installing again. */
            char message[1024];
            player_launcher_unavailable_message(ui_error,
                                                !player_ui_nk_built_with_launcher(),
                                                message, sizeof(message));
            fprintf(stderr, "[PLAYER] LAUNCHER_UNAVAILABLE: %s\n", message);
            if (interactive_window) {
                SDL_HideWindow(window);
                (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                               "Nakagawa Recomp can't start", message, NULL);
            }
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 4;
        }
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (next_ui != NULL && ui_test_mode) {
        puts("[PLAYER_UI_TEST] UI_NEXT_ACTIVE");
    }
    if (ui_test_mode && getenv("NK_UI_TEST_REPORT_COMPATIBILITY") != NULL) {
        player_ui_nk_test_report_compatibility();
    }
    if (ui_test_mode && getenv("NK_UI_TEST_REPORT_VIEWS") != NULL) {
        /* Every PlayerView, VIEW_LIBRARY through the last, VIEW_CONFIRM_FACTORY_RESET. */
        for (int view = (int)VIEW_LIBRARY; view <= (int)VIEW_CONFIRM_FACTORY_RESET; ++view) {
            printf("[PLAYER_UI_TEST] view_coverage value=%d name=%s nk=%d\n", view,
                   player_ui_test_view_name((PlayerView)view),
                   player_ui_nk_draws_view(view) ? 1 : 0);
        }
    }
#endif
    s_switch_presentation = interactive_window && next_ui != NULL;
    s_ui_canvas_from_window = interactive_window;
    /* A capture lays out --width x --height units, as before, and draws each
       as --render-scale pixels; a window at the scale player_ui_canvas_fit
       picks for it shows the same picture. */
    s_ui_capture_canvas.width = override_w;
    s_ui_capture_canvas.height = override_h;
    s_ui_capture_canvas.scale = capture_render_scale;
    int capture_pixel_width = (int)(override_w * capture_render_scale + 0.5f);
    int capture_pixel_height = (int)(override_h * capture_render_scale + 0.5f);
    if (interactive_window && next_ui == NULL) {
        app.logical_ui = true;
        player_fit_ui_canvas(&app, renderer);
    } else if (interactive_window) {
        int actual_width = startup_window.width;
        int actual_height = startup_window.height;
        if (!SDL_GetWindowSize(window, &actual_width, &actual_height)) {
            actual_width = startup_window.width;
            actual_height = startup_window.height;
        }
        app.window_width = actual_width > 0 ? actual_width : PLAYER_UI_DESIGN_WIDTH;
        app.window_height = actual_height > 0 ? actual_height : PLAYER_UI_DESIGN_HEIGHT;
    }

    UiInput input;
    memset(&input, 0, sizeof(input));

#ifdef NK_PLAYER_UI_REGRESSION_TEST
    SDL_Texture *ui_test_target = NULL;
    if (ui_test_mode) {
        ui_test_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET,
                                           capture_pixel_width,
                                           capture_pixel_height);
        if (!ui_test_target || !SDL_SetRenderTarget(renderer, ui_test_target)) {
            fprintf(stderr, "[PLAYER_UI_TEST] Could not create SDL render target: %s\n",
                    SDL_GetError());
            if (ui_test_target) SDL_DestroyTexture(ui_test_target);
            player_ui_nk_destroy(next_ui);
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
        SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                                capture_pixel_width, capture_pixel_height);
        if (target) {
            SDL_SetRenderTarget(renderer, target);
        }
        player_render_active_ui(next_ui, &app, renderer, window, &input);
        if (ui_capture_screenshot(renderer, screenshot_path)) {
            printf("[PLAYER] Screenshot saved to %s (%dx%d pixels, %dx%d layout)\n", screenshot_path,
                   capture_pixel_width, capture_pixel_height,
                   app.window_width, app.window_height);
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

        player_ui_nk_destroy(next_ui);
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
                /* A pad present at start switches the prompt glyphs to it. */
                player_ui_nk_note_gamepad(next_ui, ids[0], true);
            }
        }
        SDL_free(ids);
    }
    PlayerStagingJob *staging_job = NULL;
    PlayerPsnJob *psn_job = NULL;
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
        &app, next_ui, &package_status_job, package_status_requested,
        package_status_force_requested, build_check_disc_id,
        build_check_session_generation, build_check_cache_generation,
        &build_check_pending);

    /* A Rebuild all queue the last session left resumes, and a scan says which
       games an older Nakagawa built (the prompt follows it once per update). */
    (void)player_app_rebuild_queue_load(&app);
    app.outdated.scan_requested = true;
    PlayerOutdatedScanJob *outdated_scan_job = NULL;
    app.enable_focus_handoff = interactive_window;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (getenv("NK_UI_TEST_CHECK_AT_START") != NULL) app.settings.check_updates_at_start = true;
#endif
    /* The one check at start, only when the user turned it on (off by default). */
    if (app.settings.check_updates_at_start) {
        app.update.from_start = true;
        player_update_start(&app, PLAYER_UPDATE_JOB_CHECK);
    }
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
    player_render_active_ui(next_ui, &app, renderer, window, &input);
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
        player_update_poll(&app);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        {
            static PlayerUpdatePhase reported = PLAYER_UPDATE_IDLE;
            if (ui_test_mode && app.update.phase != reported) {
                reported = app.update.phase;
                printf("[PLAYER_UI_TEST] update=%s error=%s notice=%d newest=%s zip=%s "
                       "install=%s\n",
                       player_update_phase_name(app.update.phase),
                       app.update.error.code[0] ? app.update.error.code : "none",
                       app.update.notice ? 1 : 0,
                       app.update.check.newest.tag[0] ? app.update.check.newest.tag : "none",
                       app.update.zip_path[0] ? app.update.zip_path : "none",
                       app.update.install_dir[0] ? app.update.install_dir : "none");
                fflush(stdout);
            }
        }
#endif
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
            if (ui_test_waiting_for_package_status) {
                const GameRecord *game = app.selected_game_index >= 0 &&
                    app.selected_game_index < app.game_count
                    ? &app.games[app.selected_game_index] : NULL;
                bool ready = game &&
                    !player_app_runtime_package_check_pending(&app, game) &&
                    !player_app_runtime_package_check_failed(&app, game) &&
                    player_app_cached_runtime_package_status(&app, game) ==
                        ui_test_wait_package_status_target;
                if (ready) {
                    printf("[PLAYER_UI_TEST] wait_package_status result=PASS\n");
                    ui_test_waiting_for_package_status = false;
                } else if (ui_test_now >= ui_test_wait_deadline) {
                    fprintf(stderr,
                            "[PLAYER_UI_TEST] wait_package_status result=FAIL target=%d\n",
                            (int)ui_test_wait_package_status_target);
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
                           scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_PACKAGE_STATUS) {
                    ui_test_wait_package_status_target =
                        (NkRuntimePackageStatus)scripted_event.user.windowID;
                    ui_test_wait_deadline = ui_test_now + scripted_event.user.timestamp;
                    ui_test_waiting_for_package_status = true;
                    printf("[PLAYER_UI_TEST] wait_package_status target=%u started\n",
                           scripted_event.user.windowID);
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
                    } else if (scripted_event.type == SDL_EVENT_KEY_DOWN ||
                               scripted_event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                        /* A scripted press is a whole tap: the release follows in
                           the same frame, as a person's key or button does. The
                           next UI's input model ignores a second press of a key it
                           still holds. */
                        SDL_Event release = scripted_event;
                        if (release.type == SDL_EVENT_KEY_DOWN) {
                            release.type = SDL_EVENT_KEY_UP;
                            release.key.down = false;
                        } else {
                            release.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
                            release.gbutton.down = false;
                        }
                        if (!SDL_PushEvent(&release)) {
                            ui_test_failed = true;
                            running = false;
                            fprintf(stderr, "[PLAYER_UI_TEST] Could not queue synthetic SDL "
                                            "release event.\n");
                        }
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
                /* A pad arriving or leaving updates the player's state first: the
                   launcher also takes these events for its prompts, and a consumed
                   event left Settings and the wizard saying "keyboard and mouse"
                   with a controller attached. */
                if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
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
                    if (gamepad && event.gdevice.which == SDL_GetGamepadID(gamepad)) {
                        player_ui_nk_note_gamepad(next_ui, event.gdevice.which, true);
                    }
                } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                    player_ui_nk_note_gamepad(next_ui, event.gdevice.which, false);
                    if (gamepad && event.gdevice.which == SDL_GetGamepadID(gamepad)) {
                        SDL_CloseGamepad(gamepad);
                        gamepad = NULL;
                        app.settings.controller_name[0] = '\0';
                        app.settings.controller_connected = false;
                        printf("[PLAYER] Gamepad disconnected\n");
                    }
                }
                if (!player_dispatch_ui_event(&app, &input, next_ui, window, &running,
                                              &close_request_handled_in_batch,
                                              &event)) {
                    switch (event.type) {
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
                        } else if (psn_job && event.user.data1 == psn_job) {
                            if (event.user.code == PLAYER_STAGING_EVENT_PROGRESS) {
                                sync_psn_progress(&app, psn_job);
                            } else if (event.user.code == PLAYER_STAGING_EVENT_COMPLETE) {
                                finish_psn_job(&app, psn_job);
                            }
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
        if (app.request_font_folder_picker) {
#ifdef NK_PLAYER_UI_REGRESSION_TEST
            if (!ui_test_mode) {
#endif
            app.request_font_folder_picker = false;
            trigger_font_folder_picker(window, &app);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
            }
#endif
        }
        if (app.request_open_folder) {
            app.request_open_folder = false;
#if defined(_WIN32) || defined(_WIN64)
            if (app.requested_open_path[0]) {
                (void)ShellExecuteA(NULL, "open", app.requested_open_path,
                                    NULL, NULL, SW_SHOWNORMAL);
            }
#else
            if (app.requested_open_path[0]) {
                char url[NK_MAX_PATH * 3u];
                if (!player_ui_file_url(app.requested_open_path, url, sizeof(url)) ||
                    !SDL_OpenURL(url)) {
                    fprintf(stderr, "[PLAYER] The folder could not be opened: %s\n",
                            SDL_GetError());
                    player_app_set_folder_open_error(&app, app.requested_open_path,
                                                     SDL_GetError());
                }
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
        /* A picked PSN package converts on its own worker; the wizard's cancel
           reaches it the same way. A completion whose event was lost is
           finished here. */
        if (player_app_psn_conversion_take_request(&app)) {
            start_psn_job(&app, &psn_job);
        }
        if (psn_job && app.psn_conversion_active &&
            player_app_wizard_cancel_requested(&app)) {
            request_psn_cancel(psn_job);
        }
        if (psn_job_finished(psn_job)) {
            finish_psn_job(&app, psn_job);
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
        bool build_check_session_already_handled =
            build_check_session_generation != 0 &&
            build_check_session_generation ==
                app.build_session.session_generation &&
            build_check_disc_id[0] && app.build_session.disc_id[0] &&
            strcmp(build_check_disc_id, app.build_session.disc_id) == 0;

        /* Poll every loop so the child keeps progressing when another view is
           visible. Completion is keyed to the build session's disc, not the
           current library selection. */
        player_app_poll_package_build(&app, SDL_GetTicks());
        if (app.build_session.is_complete && !build_check_pending &&
            !build_check_session_already_handled) {
            int build_game_index = player_app_find_game_by_disc_id(
                &app, app.build_session.disc_id);
            if (build_game_index >= 0 && build_game_index < app.game_count) {
                /* The build may have bound the executable this entry was
                   saved without; the check validates against that one. */
                (void)player_app_adopt_profile_executable(&app, build_game_index);
                snprintf(build_check_disc_id, sizeof(build_check_disc_id),
                         "%s", app.build_session.disc_id);
                build_check_session_generation =
                    app.build_session.session_generation;
                build_check_cache_generation =
                    app.runtime_package_cache_generation;
                build_check_pending = true;
                player_package_status_queue_game(
                    &app, build_game_index, true,
                    package_status_requested,
                    package_status_force_requested);
            } else {
                snprintf(build_check_disc_id, sizeof(build_check_disc_id),
                         "%s", app.build_session.disc_id);
                build_check_session_generation =
                    app.build_session.session_generation;
                build_check_cache_generation = 0;
                if (player_app_rebuild_queue_owns_build(&app)) {
                    /* Its title left the library during the rebuild. */
                    player_app_rebuild_queue_build_checked(&app, false);
                } else {
                    player_app_set_build_error(
                        &app, "package",
                        "Package completed without its library title.",
                        app.build_session.log_file_path);
                }
            }
        } else {
            /* Once per session, like the completion branch above (#1277). */
            (void)player_app_apply_build_outcome(&app);
        }
        /* Rebuild all: the next title starts once nothing else is building,
           and which games an older Nakagawa built is read again when asked. */
        player_app_rebuild_queue_step(&app);
        outdated_scan_job = player_outdated_scan_poll(&app, outdated_scan_job, false);

        (void)player_package_status_start_next(
            &app, next_ui, &package_status_job, package_status_requested,
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

        /* A launch the worker finished becomes the running game, or its card. */
        launch_job = player_launch_job_poll(&app, launch_job, false);

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
        /* The launcher stays open behind the game (#1298): minimizing it
           unasked surprised users. When the game ends, it comes back to the
           front, and a later launch gets its own handoff. */
        if (interactive_window && launcher_handoff_attempted &&
            !app.is_game_running) {
            SDL_RaiseWindow(window);
            launcher_handoff_attempted = false;
        }
        /* The game reports its window through the boot-event marker; the
           launcher, still the foreground process because it kept drawing,
           then hands that window the foreground once. The runtime asks for it
           as well, with the permission AllowSetForegroundWindow gave its
           process at spawn. Missing evidence keeps the launcher in front. */
        bool child_window_ready = app.is_game_running &&
            app.launch_session.boot_event_file_path[0] &&
            player_app_child_window_ready(&app);
        if (player_app_should_attempt_window_handoff(
                interactive_window, app.is_game_running,
                launcher_handoff_attempted, child_window_ready)) {
            launcher_handoff_attempted = true;
            bool fronted = nk_platform_bring_process_window_to_front(
                &app.launch_session.process);
#ifdef _WIN32
            if (!fronted) {
                fprintf(stderr, "[PLAYER] The game window could not be brought to the front; "
                                "it stays where Windows put it.\n");
            }
#else
            /* No POSIX backend raises another process's window: X11 and Wayland
               leave focus to the window manager, which gives the new game
               window focus itself. The raise always reports false here, and
               the stall path below minimizes instead of raising. */
            (void)fronted;
#endif
        }
        /* A stalled game keeps running, so its window would otherwise cover
           the launcher and hide the only actionable notice. Raise the player
           once when the notice first appears. When the notice is dismissed or
           a frame resumes, return focus to the game; on platforms without a
           process-window raise, minimize only after that explicit notice
           transition so the previous game window can be seen again. */
        if (app.stalled_game_notice_raise_requested) {
            app.stalled_game_notice_raise_requested = false;
            if (interactive_window) SDL_RaiseWindow(window);
        }
        if (app.stalled_game_restore_game_front_requested) {
            app.stalled_game_restore_game_front_requested = false;
            if (interactive_window && app.is_game_running &&
                !nk_platform_bring_process_window_to_front(
                    &app.launch_session.process)) {
                SDL_MinimizeWindow(window);
            }
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
        player_render_active_ui(next_ui, &app, renderer, window, &input);
        /* Consume renderer requests in the same frame that created them. A
           later event batch may change the selected title before the next
           loop iteration, which must not redirect this retry. */
        if (app.request_launch) {
            app.request_launch = false;
            if (!launch_job) {
                player_note_launcher_display(&app, window);
                launch_job = player_launch_job_start(&app, app.request_launch_index);
            }
        }
        if (app.request_factory_reset) {
            app.request_factory_reset = false;
            /* A finished staging or PSN job stays allocated until the next one. */
            if (launch_job || package_status_job || s_update_job ||
                (staging_job && !staging_job->completion_handled) ||
                (psn_job && !psn_job->completion_handled)) {
                /* A worker may still have a file in the user data open. */
                player_app_set_error(&app, "FACTORY_RESET_BUSY", "Factory Reset Has to Wait",
                                     "Nakagawa Recomp is still checking or setting up a "
                                     "game. Wait a moment, then reset again. Nothing was "
                                     "moved.", "Return to Settings", VIEW_SETTINGS);
            } else {
                char stamp[32];
                time_t now = time(NULL);
                struct tm *local = localtime(&now);
                if (!local || strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local) == 0) {
                    snprintf(stamp, sizeof(stamp), "%lld", (long long)now);
                }
                (void)player_app_factory_reset(&app, stamp);
            }
        }
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

    /* Closing: a package check still hashing stops at its next read, so the
       window does not hang until every startup check is done. */
    SDL_SetAtomicInt(&s_player_closing, 1);
    outdated_scan_job = player_outdated_scan_poll(&app, outdated_scan_job, true);

    /* A launch still on its worker finishes first, so its game is stopped. */
    launch_job = player_launch_job_poll(&app, launch_job, true);
    if (app.is_game_running) {
        player_app_stop_game(&app);
    }

    /* After a factory reset nothing is saved: the settings would bring the
       config folder back. */
    if (interactive_window && !app.factory_reset_done) {
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

    player_update_shutdown();
    destroy_staging_job(&staging_job);
    destroy_psn_job(&psn_job);
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
    player_ui_nk_destroy(next_ui);
    ui_font_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (app.factory_reset_done) {
        bool start_copy = interactive_window;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        if (ui_test_mode) {
            puts("[PLAYER_UI_TEST] factory_reset restart=skipped");
            start_copy = false;
        }
#endif
        if (start_copy && !player_start_fresh_copy()) {
            fprintf(stderr, "[PLAYER] Factory reset is done, but a new copy of the player could "
                            "not be started (%s). Start it again to set it up.\n", SDL_GetError());
            (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Factory reset is done",
                                           "Start Nakagawa Recomp again to set it up.", NULL);
        }
    }
    SDL_Quit();
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (ui_test_mode && ui_test_failed) return 1;
#endif
    return 0;
}
