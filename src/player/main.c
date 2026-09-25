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
    snprintf(game->disc_id, sizeof(game->disc_id), "%s", result->disc_id);
    snprintf(game->title_name, sizeof(game->title_name), "%s", result->title_name);
    snprintf(game->disc_version, sizeof(game->disc_version), "%s", result->disc_version);
    snprintf(game->iso_path, sizeof(game->iso_path), "%s", iso_path);
    snprintf(game->title_id, sizeof(game->title_id), "%s", result->matched_title_id);
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

enum {
    PLAYER_STAGING_EVENT_PROGRESS = 1,
    PLAYER_STAGING_EVENT_COMPLETE = 2
};

typedef struct {
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    char iso_path[NK_MAX_PATH];
    char staging_root[4096];
    char final_root[4096];
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
#if defined(_WIN32) || defined(_WIN64)
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (!base || !base[0]) base = getenv("USERPROFILE");
    if (!base || !base[0]) return false;
    games_written = snprintf(games_root, sizeof(games_root), "%s%cNakagawa%cgames",
                             base, nk_platform_path_separator(), nk_platform_path_separator());
#else
    char app_data[NK_MAX_PATH];
    if (!nk_platform_get_app_data_dir(app_data, sizeof(app_data))) return false;
    games_written = snprintf(games_root, sizeof(games_root), "%s%cgames",
                                 app_data, nk_platform_path_separator());
#endif
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

static bool staged_payload_is_complete(const char *root) {
    if (!root || !root[0]) return false;
    char eboot_path[4096];
    char xbdata_path[4096];
    int eboot_written = snprintf(eboot_path, sizeof(eboot_path), "%s%cEBOOT.BIN",
                                 root, nk_platform_path_separator());
    int xbdata_written = snprintf(xbdata_path, sizeof(xbdata_path), "%s%cxbdata",
                                  root, nk_platform_path_separator());
    return eboot_written > 0 && xbdata_written > 0 &&
           (size_t)eboot_written < sizeof(eboot_path) &&
           (size_t)xbdata_written < sizeof(xbdata_path) &&
           nk_platform_file_exists(eboot_path) && nk_platform_dir_exists(xbdata_path);
}

/* A completed promotion can outlive the library write if the user-data
 * filesystem is full or temporarily unavailable. Reuse only the exact root
 * derived from the inspected disc ID and only when the transaction's two
 * required payload roots are present; never replace it with a fresh tree. */
static bool adopt_existing_staged_root(PlayerApp *app, const char *final_root) {
    if (!app || !final_root || !staged_payload_is_complete(final_root) ||
        !copy_bounded_text(app->inspecting_game.prepared_root,
                           sizeof(app->inspecting_game.prepared_root),
                           final_root)) return false;
    app->inspecting_game.assets_staged = true;
    app->inspecting_game.is_prepared = player_app_validate_runtime_package(
        app, &app->inspecting_game, NULL, NULL, 0) == NK_RUNTIME_PACKAGE_OK;
    app->inspecting_game.status = app->inspecting_game.is_prepared
        ? NK_STATUS_PREPARED : NK_STATUS_SUPPORTED_PREPARATION;
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
        bool complete = staged_payload_is_complete(job->final_root);
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
        /* Disc staging and runtime preparation are separate claims. A locally
           available recompiled runtime may make this entry launch-ready; a
           staged retail source without that runtime remains actionable but
           fail-closed when PLAY/LAUNCH PREPARED is activated. */
        app->inspecting_game.is_prepared = player_app_validate_runtime_package(
            app, &app->inspecting_game, NULL, NULL, 0) == NK_RUNTIME_PACKAGE_OK;
        app->inspecting_game.status = app->inspecting_game.is_prepared
            ? NK_STATUS_PREPARED : NK_STATUS_SUPPORTED_PREPARATION;
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
        app->inspecting_game.iso_path, staging_root, NULL, &summary,
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
    const char *executable_directory = SDL_GetBasePath();
    if (executable_directory) {
        player_app_discover_showcase(&app, executable_directory);
    }

    const char *screenshot_path = NULL;
    const char *test_view = NULL;
    const char *manifest_overlay_path = NULL;
    const char *initial_iso_path = NULL;
    const char *runtime_root_path = NULL;
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
                   "[--runtime-root=<path>] [--view=<name>] [--screenshot=<bmp>]\n");
            return 0;
        } else if (strncmp(argv[i], "--screenshot=", 13) == 0) {
            screenshot_path = argv[i] + 13;
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
        } else if (strcmp(argv[i], "--empty") == 0) {
            force_empty = true;
        } else if (strcmp(argv[i], "--wizard") == 0) {
            force_empty = true;
            test_view = "wizard";
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
    if (launch_index_requested && !force_empty) {
        /* The one-shot driver is a deliberate demo/test entry point. It still
           uses the normal library loader first, so an existing user library is
           never overwritten by the fixture population. */
        populate_sample = true;
    }
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
               are useful library entries. Images without parsed PARAM.SFO
               remain refused on the unsupported view. */
            bool should_store = res.is_supported || app.inspecting_game.is_experimental;
            bool stored = should_store && player_app_add_game(&app, &app.inspecting_game);

            if (should_store && !stored) {
                fprintf(stderr, "[PLAYER] Could not store %s in the library.\n",
                        app.inspecting_game.disc_id);
                player_app_set_error(&app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                     "The title could not be stored. The library may be full, "
                                     "or the user data directory is not writable.",
                                     "Return to Library", VIEW_LIBRARY);
            } else if (stored) {
                player_app_set_view(&app, app.inspecting_game.is_experimental
                    ? VIEW_EXPERIMENTAL_TITLE : VIEW_SUPPORTED_TITLE);
                if (launch_now && !app.inspecting_game.is_experimental) {
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
        } else if (strcmp(test_view, "ready-library") == 0 ||
                   strcmp(test_view, "ready") == 0) {
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
            player_app_set_view(&app, PLAYER_VIEW_READY_LIBRARY);
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
            snprintf(app.inspecting_game.disc_id, sizeof(app.inspecting_game.disc_id), "UCUS98701");
            snprintf(app.inspecting_game.title_name, sizeof(app.inspecting_game.title_name), "Hot Shots Tennis: Get a Grip");
            snprintf(app.inspecting_game.iso_path, sizeof(app.inspecting_game.iso_path), "selected/Hot Shots Tennis.iso");
            app.inspecting_game.status = NK_STATUS_VERIFIED;
            app.wizard.iso_selected = true;
            app.wizard.step = WIZARD_STEP_INSPECT_VERIFY;
            app.wizard.is_extracting = true;
            app.wizard.extraction_percent = 67;
            app.wizard.files_extracted = 7;
            app.wizard.total_files = 12;
            snprintf(app.wizard.extraction_current_file,
                     sizeof(app.wizard.extraction_current_file),
                     "xbdata/ui/menus/title_menu.xb");
            snprintf(app.wizard.status_message, sizeof(app.wizard.status_message),
                     "Unpacking clean-room XB assets...");
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

    app.window_width = override_w;
    app.window_height = override_h;

    /* Mechanical launch driver for the real player path. This deliberately
       stops before SDL initialization: the child runtime owns the game window,
       while the parent waits for its real exit status. A test can set
       SR_BOOT_EVENT_FILE to receive the child's window/first-frame milestones. */
    if (launch_index_requested) {
        if (launch_index < 0 || launch_index >= app.game_count) {
            fprintf(stderr, "[PLAYER] Invalid --launch-index=%d for library count %d.\n",
                    launch_index, app.game_count);
            return 2;
        }
        app.selected_game_index = launch_index;
        if (player_app_validate_runtime_package(
                &app, &app.games[launch_index], NULL, NULL, 0) !=
            NK_RUNTIME_PACKAGE_OK) {
            fprintf(stderr, "[PLAYER] Launch index %d has no prepared runtime; PLAY NOW is unavailable.\n",
                    launch_index);
            return 3;
        }
        printf("[PLAYER] Launch index %d: PLAY NOW available for %s (%s).\n",
               launch_index, app.games[launch_index].disc_id,
               app.games[launch_index].title_name);
        if (!player_app_launch_game(&app, launch_index)) {
            fprintf(stderr, "[PLAYER] --launch-index failed: %s\n",
                    app.launch_session.last_error);
            return 4;
        }
        printf("[PLAYER] Launch argv contains --gui: %s\n",
               app.launch_session.argv_has_gui ? "yes" : "no");
        if (!app.launch_session.argv_has_gui) {
            player_app_stop_game(&app);
            return 5;
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
    /* Launcher stays usable when thrown any resize: refuse tiny windows
     * that would collapse every card below its minimum. */
    SDL_SetWindowMinimumSize(window, 640, 560);
    /* Record real pixel density for crisp type; 1.0 on standard displays. */
    app.dpi_scale = SDL_GetWindowPixelDensity(window);
    if (app.dpi_scale < 1.0f) app.dpi_scale = 1.0f;

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

        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        ui_font_shutdown();
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

    bool running = true;
    uint64_t last_tick = SDL_GetTicks();
    /* The first frame is rendered before the event wait. A bounded wait keeps
       process-exit monitoring alive while the user is idle; staging progress
       and normal input still wake the loop immediately. */
    ui_render_frame(renderer, &app, &input);
    while (running && !app.should_quit) {
        uint64_t now_tick = SDL_GetTicks();
        uint32_t delta_ms = (uint32_t)(now_tick >= last_tick ? (now_tick - last_tick) : 0);
        last_tick = now_tick;
        if (app.active_view == VIEW_CONTROLLER_SETTINGS && input_settings_is_capturing(&app.input_settings)) {
            input_settings_update_capture(&app.input_settings, delta_ms > 0 ? delta_ms : 1);
        }

        input.mouse_clicked = false;
        input.activate_pressed = false;
        SDL_Event event;
        bool event_available = SDL_WaitEventTimeout(&event, 50);
        if (event_available) {
            do {
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
                        if (app.active_view == VIEW_SETUP_WIZARD) {
                            player_app_wizard_back(&app);
                        } else if (app.active_view == VIEW_CONTROLLER_SETTINGS) {
                            if (input_settings_is_capturing(&app.input_settings)) {
                                input_settings_cancel_capture(&app.input_settings);
                            } else if (input_settings_is_calibrating(&app.input_settings)) {
                                input_settings_cancel_calibration(&app.input_settings);
                            } else {
                                player_app_set_view(&app, VIEW_SETTINGS);
                            }
                        } else if (app.active_view == VIEW_BUILDING_PACKAGE) {
                            player_app_cancel_package_build(&app);
                            player_app_set_view(&app, VIEW_LIBRARY);
                        } else if (!player_view_is_library(app.active_view)) {
                            player_app_set_view(&app, VIEW_LIBRARY);
                        } else {
                            running = false;
                        }
                    } else if (event.key.key == SDLK_O && !app.wizard.is_extracting) {
                        /* Trigger file picker */
                        trigger_file_picker(window, &app);
                    } else if (event.key.key == SDLK_S && app.active_view != VIEW_INSPECTING) {
                        /* Settings shortcut (topbar button is mouse-only). */
                        if (app.active_view == VIEW_SETTINGS) {
                            player_app_set_view(&app, VIEW_LIBRARY);
                        } else {
                            player_app_set_view(&app, VIEW_SETTINGS);
                        }
                    } else if (event.key.key == SDLK_TAB) {
                        int count = ui_focus_count(&app);
                        if (event.key.mod & SDL_KMOD_SHIFT) {
                            player_app_move_focus(&app, -1, count);
                        } else {
                            player_app_move_focus(&app, 1, count);
                        }
                    } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER ||
                               event.key.key == SDLK_SPACE) {
                        /* Held-key auto-repeat must not re-fire actions: the
                         * first PLAY press swaps the button to STOP, so a
                         * repeat would instantly stop the just-started game. */
                        if (!event.key.repeat) {
                            input.activate_pressed = true;
                        }
                    } else if (player_view_is_library(app.active_view)) {
                        /* Keyboard selection across the whole library, not
                           just the cards that happen to fit on screen. */
                        if (event.key.key == SDLK_LEFT) {
                            player_app_move_selection(&app, -1);
                        } else if (event.key.key == SDLK_RIGHT) {
                            player_app_move_selection(&app, 1);
                        } else if (event.key.key == SDLK_UP) {
                            player_app_move_focus(&app, -1, ui_focus_count(&app));
                        } else if (event.key.key == SDLK_DOWN) {
                            player_app_move_focus(&app, 1, ui_focus_count(&app));
                        } else if (event.key.key == SDLK_HOME) {
                            app.selected_game_index = app.game_count > 0 ? 0 : -1;
                        } else if (event.key.key == SDLK_END) {
                            app.selected_game_index = app.game_count - 1;
                        } else if (event.key.key == SDLK_PAGEUP) {
                            player_app_move_selection(&app, -player_app_visible_library_cards(&app));
                        } else if (event.key.key == SDLK_PAGEDOWN) {
                            player_app_move_selection(&app, player_app_visible_library_cards(&app));
                        }
                    } else {
                        /* Dialog views: arrows move focus so every button is
                         * reachable without a mouse. */
                        if (event.key.key == SDLK_LEFT || event.key.key == SDLK_UP) {
                            player_app_move_focus(&app, -1, ui_focus_count(&app));
                        } else if (event.key.key == SDLK_RIGHT || event.key.key == SDLK_DOWN) {
                            player_app_move_focus(&app, 1, ui_focus_count(&app));
                        }
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    if (player_view_is_library(app.active_view) && event.wheel.y != 0.0f) {
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
                case SDL_EVENT_USER:
                    if (event.user.data1 == staging_job) {
                        if (event.user.code == PLAYER_STAGING_EVENT_PROGRESS) {
                            sync_staging_progress(&app, staging_job);
                        } else if (event.user.code == PLAYER_STAGING_EVENT_COMPLETE) {
                            sync_staging_progress(&app, staging_job);
                            finish_staging_job(&app, staging_job);
                        }
                    }
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    if (app.active_view == VIEW_CONTROLLER_SETTINGS && input_settings_is_capturing(&app.input_settings)) {
                        if ((event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                             event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
                            event.gaxis.value > 16000) {
                            NkBindingSource src;
                            src.type = NK_BINDING_HOST_TRIGGER;
                            src.index = event.gaxis.axis;
                            input_settings_feed_capture_source(&app.input_settings, src);
                        }
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (app.active_view == VIEW_CONTROLLER_SETTINGS && input_settings_is_capturing(&app.input_settings)) {
                        NkBindingSource src;
                        src.type = NK_BINDING_HOST_BUTTON;
                        src.index = event.gbutton.button;
                        input_settings_feed_capture_source(&app.input_settings, src);
                        break;
                    }
                    if (player_view_is_library(app.active_view)) {
                        switch (event.gbutton.button) {
                            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                                player_app_move_selection(&app, -1);
                                break;
                            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                                player_app_move_selection(&app, 1);
                                break;
                            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                                player_app_move_focus(&app, -1, ui_focus_count(&app));
                                break;
                            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                                player_app_move_focus(&app, 1, ui_focus_count(&app));
                                break;
                            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                                player_app_move_selection(&app, -player_app_visible_library_cards(&app));
                                break;
                            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                                player_app_move_selection(&app, player_app_visible_library_cards(&app));
                                break;
                            case SDL_GAMEPAD_BUTTON_SOUTH:
                                input.activate_pressed = true;
                                break;
                            case SDL_GAMEPAD_BUTTON_START:
                                player_app_set_view(&app, VIEW_SETTINGS);
                                break;
                            default:
                                break;
                        }
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                        if (app.active_view == VIEW_SETUP_WIZARD) {
                            player_app_wizard_back(&app);
                        } else if (app.active_view == VIEW_CONTROLLER_SETTINGS) {
                            if (input_settings_is_calibrating(&app.input_settings)) {
                                input_settings_cancel_calibration(&app.input_settings);
                            } else {
                                player_app_set_view(&app, VIEW_SETTINGS);
                            }
                        } else if (app.active_view == VIEW_BUILDING_PACKAGE) {
                            player_app_cancel_package_build(&app);
                            player_app_set_view(&app, VIEW_LIBRARY);
                        } else {
                            player_app_set_view(&app, VIEW_LIBRARY);
                        }
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                        input.activate_pressed = true;
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT ||
                               event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                        player_app_move_focus(&app, -1, ui_focus_count(&app));
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ||
                               event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                        player_app_move_focus(&app, 1, ui_focus_count(&app));
                    } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                        if (app.active_view == VIEW_SETTINGS || app.active_view == VIEW_CONTROLLER_SETTINGS) {
                            player_app_set_view(&app, VIEW_LIBRARY);
                        } else {
                            player_app_set_view(&app, VIEW_SETTINGS);
                        }
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
            } while (SDL_PollEvent(&event));
        }

        /* A renderer control asked for the host file dialog. The renderer has
           no window handle and must stay free of platform dialog calls, so the
           request is serviced here. */
        if (app.request_file_picker) {
            app.request_file_picker = false;
            trigger_file_picker(window, &app);
        }

        /* Step 3 requests one worker; progress and completion return through
           SDL user events, so the UI thread never polls a job or performs ISO
           I/O. */
        if (player_app_wizard_take_extraction_request(&app)) {
            start_staging_job(&app, &staging_job);
        }
        if (staging_job && app.wizard.is_extracting &&
            player_app_wizard_cancel_requested(&app)) {
            request_staging_cancel(staging_job);
        }

        /* Clamp keyboard/gamepad focus before rendering so activation can
         * never target a control the current view no longer draws. */
        player_app_move_focus(&app, 0, ui_focus_count(&app));

        /* Monitor background package build session */
        if (app.active_view == VIEW_BUILDING_PACKAGE) {
            package_builder_poll(&app.build_session, SDL_GetTicks());
            if (app.build_session.is_complete) {
                const GameRecord *game = (app.selected_game_index >= 0 && app.selected_game_index < app.game_count)
                    ? &app.games[app.selected_game_index] : NULL;
                char reason[512] = "";
                NkRuntimePackageStatus status = player_app_validate_runtime_package(&app, game, NULL, reason, sizeof(reason));
                if (status == NK_RUNTIME_PACKAGE_OK) {
                    if (game) {
                        app.games[app.selected_game_index].is_prepared = true;
                        app.games[app.selected_game_index].status = NK_STATUS_PREPARED;
                        nk_library_add_or_update(&app.library, &app.games[app.selected_game_index]);
                        player_app_sync_library(&app);
                    }
                    player_app_set_view(&app, PLAYER_VIEW_READY_LIBRARY);
                } else {
                    player_app_set_build_error(&app, "package",
                                               reason[0] ? reason : "Package re-validation failed after build completed.",
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

        ui_render_frame(renderer, &app, &input);
    }

    if (app.is_game_running) {
        player_app_stop_game(&app);
    }

    destroy_staging_job(&staging_job);

    if (gamepad) {
        SDL_CloseGamepad(gamepad);
        gamepad = NULL;
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    ui_font_shutdown();
    SDL_Quit();
    return 0;
}
