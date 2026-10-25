/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"

#include "player_state.h"
#include "player_ui_art.h"
#include "player_ui_sound.h"
#include "iso_reader.h"
#include "nk_platform.h"
#include "nk_ui_input.h"
#include "nk_ui_theme.h"
#include "nk_ui_widgets.h"
#include "backend/nk_ui_sdlr.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_UI_READY 0x2DD4BFFFu
#define PLAYER_UI_WARNING 0xDBB024FFu
#define PLAYER_UI_INFO 0x6CB6FFFFu
#define PLAYER_UI_TITLE_CAP MAX_LIBRARY_GAMES
#define PLAYER_UI_PREREQ_CAP PACKAGE_BUILDER_MAX_PREREQUISITES
/* A blocking check's reason (up to its 2048-byte message) and the next step. */
#define PLAYER_UI_BLOCKED_REASON_MAX 2304

/* Build identity for the About page, injected by the Makefile and CMake:
   SOURCE_DATE_EPOCH when the build sets it, else the source commit's time. */
#ifndef NK_PLAYER_BUILD_EPOCH
#define NK_PLAYER_BUILD_EPOCH (-1LL)
#endif
#ifndef NK_PLAYER_BUILD_COMMIT
#define NK_PLAYER_BUILD_COMMIT ""
#endif

typedef struct {
    const char *key;
    const char *word;
    const char *detail;
} PlayerUiCompatibilityRecord;

/* These labels mirror the current public compatibility table. Lookup is data
   driven so the screen never assigns a compatibility claim from layout code. */
static const PlayerUiCompatibilityRecord s_compatibility_records[] = {
    {"display-smoke-v1", "Works", "Source-owned display fixture."},
    {"synthetic-allegrex-v1", "Works", "Source-owned runtime fixture."},
    {"pspdev-phase5-v1", "Boots to a named boundary",
     "Player launch stops at the unbuilt package boundary."},
    {"synthetic-title2-v1", "Works", "Source-owned runtime fixture."},
    {"showcase-scene-v1", "Partially works", "Showcase profile-zero acceptance remains open."},
    {"showcase-breakout-v1", "Partially works", "Showcase profile-zero acceptance remains open."},
    {"UCUS98701", "Boots to a named boundary", "The public route stops at a named boundary."},
    {"UCUS-98701", "Boots to a named boundary", "The public route stops at a named boundary."},
    {"PAC-MAN Championship Edition", "Partially works",
     "The documented route reaches the title screen and plays a stage."}
};

struct PlayerUiNk {
    NkUi *ui;
    NkUiSdlr *backend;
    NkUiTtf *ttf;
    NkUiTextBackend text;
    NkUiSdlInput input;
    NkUiRouter *router;
    NkUiViewport viewport;
    size_t library_page;
    PlayerUiTitle titles[PLAYER_UI_TITLE_CAP];
    char status_text[PLAYER_UI_TITLE_CAP][256];
    char library_page_label[64];
    char flow_title[NK_MAX_TITLE_LEN + NK_MAX_DISC_ID_LEN + 16];
    PlayerUiPrereqItem prereq_items[PLAYER_UI_PREREQ_CAP];
    char version_line[64];
    char wizard_detail[160];
    char wizard_blocked[PLAYER_UI_BLOCKED_REASON_MAX];
    char wizard_resolution[96];
    char wizard_controller[160];
    char wizard_summary[224];
    unsigned flow_screen;   /* which flow screen drew last; 0 outside them */
    NkUiId pre_flow_focus;  /* the library focus to give back on leaving */
    NkUiPromptImages prompt_images;  /* Kenney glyphs; empty ones keep the geometry */
    PlayerUiArt *art;                /* disc ICON0 / PIC1, decoded at run time */
    PlayerUiSounds *sounds;          /* optional launcher sounds (ui_sounds) */
    int pending_sound;               /* a PlayerUiSound for this frame, or -1 */
    NkUiId sound_focus;              /* focus after the last frame, for the move sound */
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    bool icon_reported[PLAYER_UI_TITLE_CAP];
#endif
    bool ready;
};

/* The player's art reader: ICON0.PNG and PIC1.PNG from the user's disc image,
   through the ISO reader's own size and header checks. */
static void player_ui_read_disc_art(const char *iso_path, unsigned wanted,
                                    uint8_t **icon, size_t *icon_size,
                                    uint8_t **picture, size_t *picture_size) {
    NkIsoImageData icon_data;
    NkIsoImageData picture_data;
    unsigned iso_wanted = 0u;
    if ((wanted & PLAYER_UI_ART_ICON) != 0u) iso_wanted |= NK_ISO_ART_ICON;
    if ((wanted & PLAYER_UI_ART_PICTURE) != 0u) iso_wanted |= NK_ISO_ART_PICTURE;
    nk_iso_read_game_art(iso_path, iso_wanted, &icon_data, &picture_data);
    *icon = icon_data.status == NK_ICON_OK ? icon_data.data : NULL;
    *icon_size = icon_data.status == NK_ICON_OK ? icon_data.size : 0u;
    if (icon_data.status != NK_ICON_OK) free(icon_data.data);
    *picture = picture_data.status == NK_ICON_OK ? picture_data.data : NULL;
    *picture_size = picture_data.status == NK_ICON_OK ? picture_data.size : 0u;
    if (picture_data.status != NK_ICON_OK) free(picture_data.data);
}

static void player_ui_set_error(char *out, size_t out_size, const char *message) {
    if (out == NULL || out_size == 0u) return;
    snprintf(out, out_size, "%s", message != NULL ? message : "UI initialization failed");
}

static bool player_ui_file_exists(const char *path) {
    FILE *file;
    if (path == NULL || path[0] == '\0') return false;
    file = fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static bool player_ui_join_path(const char *root, const char *relative,
                                char *out, size_t out_size) {
    size_t root_length;
    int written;
    if (root == NULL || relative == NULL || out == NULL || out_size == 0u) {
        return false;
    }
    root_length = strlen(root);
    written = snprintf(out, out_size, "%s%s%s", root,
                       root_length != 0u &&
                           (root[root_length - 1u] == '/' ||
                            root[root_length - 1u] == '\\') ? "" :
                           (nk_platform_path_separator() == '\\' ? "\\" : "/"),
                       relative);
    return written > 0 && (size_t)written < out_size;
}

static bool player_ui_add_font(NkUiTtf *ttf, int font, const char *base_path,
                               const char *relative_path) {
    char path[2u * NK_MAX_PATH];
    if (base_path != NULL &&
        player_ui_join_path(base_path, relative_path, path, sizeof(path)) &&
        player_ui_file_exists(path) && nk_ui_ttf_add_font(ttf, font, path)) {
        return true;
    }
    if (player_ui_file_exists(relative_path) &&
        nk_ui_ttf_add_font(ttf, font, relative_path)) {
        return true;
    }
    return false;
}

/* The design system's eight font slots (nk_ui_theme.h). Every face falls back
   to Geist for Latin it lacks and to Noto Sans JP for Japanese titles. */
static bool player_ui_register_fonts(NkUiTtf *ttf, const char *base_path,
                                     char *error, size_t error_size) {
    for (int font = 0; font < NK_UI_FONT_COUNT; ++font) {
        if (!player_ui_add_font(ttf, font, base_path, nk_ui_font_path(font))) {
            player_ui_set_error(error, error_size,
                                "The next UI could not load the bundled UI fonts.");
            return false;
        }
    }
    for (int font = 0; font < NK_UI_FONT_COUNT; ++font) {
        if (font == NK_UI_FONT_CJK) continue;
        if ((font != NK_UI_FONT_BODY &&
             !nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_BODY)) ||
            !nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_CJK)) {
            player_ui_set_error(error, error_size,
                                "The next UI could not configure the font fallback chain.");
            return false;
        }
    }
    return true;
}

PlayerUiNk *player_ui_nk_create(SDL_Renderer *renderer, SDL_Window *window,
                                char *error, size_t error_size) {
    PlayerUiNk *player;
    const char *base_path;
    NkUiConfig config;
    (void)window;
    if (renderer == NULL) {
        player_ui_set_error(error, error_size,
                            "The next UI could not access the player renderer.");
        return NULL;
    }
    if (!nk_ui_sdlr_available() || !nk_ui_ttf_available()) {
        player_ui_set_error(error, error_size,
                            "The next UI needs the SDL_Renderer and SDL3_ttf build dependencies.");
        return NULL;
    }
    player = (PlayerUiNk *)calloc(1u, sizeof(*player));
    if (player == NULL) {
        player_ui_set_error(error, error_size,
                            "The next UI could not allocate its renderer state.");
        return NULL;
    }
    player->router = nk_ui_router_create();
    if (player->router == NULL) {
        player_ui_set_error(error, error_size,
                            "The next UI could not allocate its screen router.");
        player_ui_nk_destroy(player);
        return NULL;
    }
    player->ttf = nk_ui_ttf_create(NULL, 0u);
    if (player->ttf == NULL) {
        player_ui_set_error(error, error_size, nk_ui_ttf_last_error());
        player_ui_nk_destroy(player);
        return NULL;
    }
    base_path = SDL_GetBasePath();
    if (!player_ui_register_fonts(player->ttf, base_path, error, error_size)) {
        player_ui_nk_destroy(player);
        return NULL;
    }
    player->text = nk_ui_ttf_backend(player->ttf);
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &player->text;
    player->ui = nk_ui_create(&config);
    if (player->ui == NULL) {
        player_ui_set_error(error, error_size,
                            "The next UI could not create its layout core.");
        player_ui_nk_destroy(player);
        return NULL;
    }
    player->backend = nk_ui_sdlr_create(renderer, &player->text);
    if (player->backend == NULL) {
        player_ui_set_error(error, error_size,
                            "The next UI could not create its SDL_Renderer backend.");
        player_ui_nk_destroy(player);
        return NULL;
    }
    /* Glyphs, art and sounds are extras: a failure leaves the geometric
       prompts, the placeholders and silence. */
    (void)nk_ui_prompt_images_load(&player->prompt_images);
    nk_ui_set_prompt_images(player->ui, &player->prompt_images);
    player->art = player_ui_art_create(player_ui_read_disc_art);
    player->sounds = player_ui_sounds_create();
    player->pending_sound = -1;
    nk_ui_sdl_input_init(&player->input);
    player_ui_version_line(NK_PLAYER_VERSION, (long long)NK_PLAYER_BUILD_EPOCH,
                           player->version_line, sizeof(player->version_line));
    player->ready = true;
    if (error != NULL && error_size > 0u) error[0] = '\0';
    return player;
}

void player_ui_nk_destroy(PlayerUiNk *player) {
    if (player == NULL) return;
    player_ui_sounds_destroy(player->sounds);
    player_ui_art_destroy(player->art);
    nk_ui_sdlr_destroy(player->backend);
    nk_ui_destroy(player->ui);
    nk_ui_prompt_images_free(&player->prompt_images);
    nk_ui_ttf_destroy(player->ttf);
    nk_ui_router_destroy(player->router);
    free(player);
}

/* The views the next UI draws: the library (router screens), and the setup
   wizard, build, build tool download and error screens a first run passes
   through. The rest (settings, inspector, game found) stay with ui_renderer.c. */
static bool player_ui_nk_flow_view(PlayerView view) {
    return view == VIEW_SETUP_WIZARD || view == VIEW_BUILDING_PACKAGE ||
           view == VIEW_PREREQ_CONSENT ||
           view == VIEW_PREREQ_PROGRESS || view == VIEW_ERROR;
}

bool player_ui_nk_view_active(const PlayerUiNk *player, const PlayerApp *app) {
    return player != NULL && player->ready && app != NULL &&
           (app->active_view == VIEW_LIBRARY ||
            app->active_view == PLAYER_VIEW_READY_LIBRARY ||
            player_ui_nk_flow_view(app->active_view));
}

static void player_ui_format_size(char *out, size_t out_size, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_size, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_size, "%llu bytes", (unsigned long long)bytes);
    }
}

static const char *player_ui_resolution_word(int scale) {
    switch (scale) {
        case 1: return "1x native (480x272)";
        case 2: return "2x (960x544)";
        case 3: return "3x (1440x816)";
        case 4: return "4x";
        case 8: return "8x (3840x2176)";
        default: return "Custom";
    }
}

/* The setup wizard's model, from the same wizard state the existing renderer
   reads. */
static PlayerUiAction player_ui_render_wizard(PlayerUiNk *player, PlayerApp *app,
                                              NkUiScale scale) {
    const NkUiTheme *theme = nk_ui_theme(player->ui);
    const GameRecord *game = &app->inspecting_game;
    PlayerUiWizardModel model;
    char size[48];
    memset(&model, 0, sizeof(model));
    model.step = (int)app->wizard.step;
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    model.disc_id = game->disc_id;
    model.title_name = game->title_name;
    model.online_mode = app->settings.online_mode;
    model.developer_mode = app->settings.developer_mode;
    switch (app->wizard.step) {
        case WIZARD_STEP_SELECT_GAME:
            model.has_iso = game->iso_path[0] != '\0';
            model.iso_selected = app->wizard.iso_selected;
            model.iso_path = game->iso_path;
            player_ui_format_size(size, sizeof(size), game->iso_size_bytes);
            snprintf(player->wizard_detail, sizeof(player->wizard_detail),
                     "Size: %s \xC2\xB7 Disc ID: %s", size,
                     game->disc_id[0] != '\0' ? game->disc_id : "Inspected");
            model.iso_detail = player->wizard_detail;
            model.status_message = app->wizard.status_message;
            break;
        case WIZARD_STEP_INSPECT_VERIFY:
            model.extracting = app->wizard.is_extracting;
            model.extraction_complete = app->wizard.extraction_complete;
            model.extraction_failed = app->wizard.extraction_failed;
            model.experimental = game->is_experimental;
            model.supported = !game->is_experimental &&
                              (app->wizard.extraction_complete ||
                               game->status == NK_STATUS_VERIFIED);
            model.files_extracted = app->wizard.files_extracted;
            model.total_files = app->wizard.total_files;
            model.extraction_percent = app->wizard.extraction_percent;
            model.current_file = app->wizard.extraction_current_file;
            model.extraction_error = player_plain_reason(app->wizard.extraction_error);
            if (game->is_experimental) {
                const PlayerPreflightCheck *blocking =
                    player_preflight_blocking_check(&app->wizard.preflight);
                if (blocking != NULL) {
                    snprintf(player->wizard_blocked, sizeof(player->wizard_blocked),
                             "%s Then open the disc again. The compatibility list on the "
                             "project page shows which games work.",
                             player_preflight_plain_message(blocking));
                    model.blocked_reason = player->wizard_blocked;
                }
            }
            /* Plain names and reasons; the codes stay in the developer log (#1280).
               The experimental row is what the card's badge and note already say. */
            for (size_t i = 0u; i < app->wizard.preflight.count &&
                                model.check_count < PLAYER_UI_WIZARD_CHECKS; ++i) {
                const PlayerPreflightCheck *check = &app->wizard.preflight.checks[i];
                if (strcmp(check->code, "EXPERIMENTAL") == 0) continue;
                PlayerUiCheckRow *row = &model.checks[model.check_count++];
                row->code = player_preflight_label(check);
                row->message = player_preflight_plain_message(check);
                row->status = player_preflight_status_word(check);
                switch (check->status) {
                    case PREFLIGHT_OK: row->rgba = theme->ready; break;
                    case PREFLIGHT_MISSING:
                    case PREFLIGHT_STALE:
                    case PREFLIGHT_IN_PROGRESS: row->rgba = theme->warning; break;
                    case PREFLIGHT_INCOMPATIBLE:
                    case PREFLIGHT_UNSUPPORTED:
                    case PREFLIGHT_INVALID: row->rgba = theme->error; break;
                    default: row->rgba = theme->dim; break;
                }
            }
            break;
        case WIZARD_STEP_SYSTEM_FONTS:
            if (app->wizard.font_slot_detail[0][0] == '\0') player_app_refresh_font_status(app);
            snprintf(player->wizard_resolution, sizeof(player->wizard_resolution),
                     "Internal resolution: %s",
                     player_ui_resolution_word(app->settings.resolution_scale));
            snprintf(player->wizard_controller, sizeof(player->wizard_controller),
                     "Controller: %s",
                     app->settings.controller_connected ? app->settings.controller_name
                                                        : "keyboard and mouse");
            model.resolution_line = player->wizard_resolution;
            model.controller_line = player->wizard_controller;
            for (int item = 0; item < NK_FONT_LATIN_INDEX_COUNT + 2 &&
                               (size_t)item < PLAYER_UI_WIZARD_FONT_LINES; ++item) {
                model.font_lines[model.font_line_count++] = item == 0
                    ? app->wizard.font_slot_detail[NK_FONT_SLOT_JAPANESE]
                    : item == NK_FONT_LATIN_INDEX_COUNT + 1
                        ? app->wizard.font_slot_detail[NK_FONT_SLOT_KOREAN]
                        : app->wizard.font_latin_index_detail[item - 1];
            }
            model.font_message = app->wizard.font_message;
            break;
        case WIZARD_STEP_READY_LAUNCH:
            model.runtime_ready = game->is_prepared;
            snprintf(player->wizard_summary, sizeof(player->wizard_summary),
                     "Resolution %s \xC2\xB7 %s \xC2\xB7 Sound %d%% \xC2\xB7 Package %s",
                     player_ui_resolution_word(app->settings.resolution_scale),
                     app->settings.fullscreen ? "Fullscreen" : "Windowed",
                     app->settings.master_volume, game->is_prepared ? "ready" : "not built yet");
            model.summary_line = player->wizard_summary;
            break;
        case WIZARD_STEP_WELCOME:
        default:
            break;
    }
    return player_ui_screen_wizard(player->ui, &model);
}

static PlayerUiAction player_ui_render_flow(PlayerUiNk *player, PlayerApp *app,
                                            NkUiScale scale, double now_s) {
    PlayerUiAction none = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u};
    if (app->active_view == VIEW_SETUP_WIZARD) {
        return player_ui_render_wizard(player, app, scale);
    }
    if (app->active_view == VIEW_BUILDING_PACKAGE) {
        const PackageBuildSession *build = &app->build_session;
        PlayerUiBuildModel model;
        int count = build->output_line_count;
        int first = count > 4 ? count - 4 : 0;
        memset(&model, 0, sizeof(model));
        if (build->title_name[0] != '\0') {
            snprintf(player->flow_title, sizeof(player->flow_title), "%s (%s)",
                     build->title_name, build->disc_id);
        } else {
            snprintf(player->flow_title, sizeof(player->flow_title), "Package build (%s)",
                     build->disc_id);
        }
        model.title_line = player->flow_title;
        model.built = build->is_complete && !build->is_failed;
        switch (build->current_stage) {
            case PACKAGE_BUILD_STAGE_EXTRACT: model.stage = 1; break;
            case PACKAGE_BUILD_STAGE_COMPILE: model.stage = 2; break;
            case PACKAGE_BUILD_STAGE_PACKAGE: model.stage = 3; break;
            case PACKAGE_BUILD_STAGE_COMPLETE: model.stage = 4; break;
            case PACKAGE_BUILD_STAGE_PREFLIGHT:
            default: model.stage = 0; break;
        }
        model.message = build->current_message;
        model.elapsed_ms = build->elapsed_ms;
        for (int i = first; i < count && model.line_count < 4u; ++i) {
            model.lines[model.line_count++] = package_builder_get_output_line(build, i);
        }
        model.now_s = app->settings.reduce_motion ? 0.0 : now_s;
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        return player_ui_screen_build(player->ui, &model);
    }
    if (app->active_view == VIEW_PREREQ_CONSENT || app->active_view == VIEW_PREREQ_PROGRESS) {
        const PlayerPrerequisiteState *pre = &app->prerequisites;
        PlayerUiPrereqModel model;
        size_t count = pre->items.count < PLAYER_UI_PREREQ_CAP ? pre->items.count
                                                                : PLAYER_UI_PREREQ_CAP;
        memset(&model, 0, sizeof(model));
        for (size_t i = 0u; i < count; ++i) {
            const PackagePrerequisite *item = &pre->items.items[i];
            player->prereq_items[i].name = item->name;
            player->prereq_items[i].version = item->version;
            player->prereq_items[i].host = item->host;
            player->prereq_items[i].license = item->license;
            player->prereq_items[i].bytes = (unsigned long long)item->size_bytes;
        }
        model.in_progress = app->active_view == VIEW_PREREQ_PROGRESS;
        model.items = player->prereq_items;
        model.item_count = count;
        model.total_bytes = (unsigned long long)pre->total_bytes;
        model.current_item = pre->current_item[0] != '\0' ? pre->current_item
            : (pre->phase == PLAYER_PREREQ_BOOTSTRAP ? "cpython-embed-amd64" : NULL);
        model.item_received_bytes = (unsigned long long)pre->item_received_bytes;
        model.item_total_bytes = (unsigned long long)pre->item_total_bytes;
        model.total_received_bytes = (unsigned long long)pre->total_received_bytes;
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        return player_ui_screen_prereq(player->ui, &model);
    }
    if (app->active_view == VIEW_ERROR) {
        const ErrorState *error = &app->last_error;
        bool launch_readiness = player_error_leads_with_message(error);
        PlayerUiErrorModel model;
        memset(&model, 0, sizeof(model));
        model.badge = player_error_badge(error);
        model.title = error->title;
        /* The failed stage is in the copied details, not on the card (#1286). */
        model.failed_stage = NULL;
        model.retry_label = player_error_retry_label(error);
        model.copy_available = true;
        model.copied = error->details_copied;
        /* Plain errors lead with the sentence and show the technical boundary
           as details. */
        model.message = launch_readiness ? error->message
            : (error->boundary_text[0] != '\0' ? error->boundary_text : error->message);
        model.details = error->details[0] != '\0' ? error->details
            : (launch_readiness ? error->boundary_text : "");
        model.log_path = error->log_file_path;
        model.recovery_label = error->recovery_action_label;
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        return player_ui_screen_error(player->ui, &model);
    }
    return none;
}

/* The flow screens open with focus on their main action. The view changes after
   the frame that asked for it, so without this the first frame of a new screen
   would still focus the control that opened it, and a press on that frame would
   be lost. Leaving the flow gives the library back the focus it had. */
static void player_ui_flow_focus(PlayerUiNk *player, const PlayerApp *app) {
    unsigned screen = 0u;
    NkUiId focus = 0u;
    if (player_ui_nk_flow_view(app->active_view)) {
        screen = (unsigned)app->active_view * 64u + 1u;
        switch (app->active_view) {
            case VIEW_SETUP_WIZARD:
                screen += (unsigned)app->wizard.step * 2u + (app->wizard.is_extracting ? 1u : 0u);
                focus = app->wizard.step == WIZARD_STEP_INSPECT_VERIFY &&
                        app->wizard.is_extracting
                    ? PLAYER_UI_ID_WIZARD_CANCEL : PLAYER_UI_ID_WIZARD_PRIMARY;
                break;
            case VIEW_BUILDING_PACKAGE: focus = PLAYER_UI_ID_BUILD_ACTION; break;
            case VIEW_PREREQ_CONSENT: focus = PLAYER_UI_ID_PREREQ_ACCEPT; break;
            case VIEW_PREREQ_PROGRESS: focus = PLAYER_UI_ID_PREREQ_CANCEL; break;
            case VIEW_ERROR: focus = PLAYER_UI_ID_ERROR_RECOVER; break;
            default: break;
        }
    }
    if (screen == player->flow_screen) return;
    if (player->flow_screen == 0u) {
        player->pre_flow_focus = nk_ui_focused(player->ui);
    }
    if (screen != 0u) {
        nk_ui_focus(player->ui, focus);
    } else if (player->pre_flow_focus != 0u) {
        nk_ui_focus(player->ui, player->pre_flow_focus);
    }
    player->flow_screen = screen;
}

#ifdef NK_PLAYER_UI_REGRESSION_TEST
const PlayerUiTitle *player_ui_nk_test_title(const PlayerUiNk *player, size_t index) {
    return player != NULL && index < PLAYER_UI_TITLE_CAP ? &player->titles[index] : NULL;
}

int player_ui_nk_test_route(const PlayerUiNk *player) {
    return player != NULL ? (int)nk_ui_router_current(player->router) : 0;
}
#endif

bool player_ui_nk_at_library_root(const PlayerUiNk *player) {
    return player != NULL && player->ready &&
           nk_ui_router_current(player->router) == PLAYER_UI_ROUTE_LIBRARY &&
           nk_ui_router_depth(player->router) == 1u;
}

bool player_ui_nk_handle_event(PlayerUiNk *player, const SDL_Event *event) {
    NkUiEvent translated[8];
    size_t count;
    double now_s;
    if (player == NULL || !player->ready || event == NULL) return false;
    now_s = (double)SDL_GetTicksNS() / 1000000000.0;
    count = nk_ui_sdl_input_translate(&player->input, event, now_s,
                                     translated,
                                     sizeof(translated) / sizeof(translated[0]));
    for (size_t i = 0u; i < count; ++i) {
        if (translated[i].type == NK_UI_EV_BACK && translated[i].down) {
            player->pending_sound = PLAYER_UI_SOUND_BACK;
        } else if (translated[i].type == NK_UI_EV_ACTIVATE && translated[i].down &&
                   !translated[i].repeat && player->pending_sound < 0) {
            player->pending_sound = PLAYER_UI_SOUND_CONFIRM;
        }
        if (translated[i].type == NK_UI_EV_BACK) {
            nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
            (void)nk_ui_router_handle_event(player->router, &translated[i]);
        } else {
            nk_ui_push_event(player->ui, &translated[i]);
        }
    }
    return true;
}

void player_ui_nk_note_gamepad(PlayerUiNk *player, uint32_t which, bool connected) {
    SDL_Event event;
    if (player == NULL || !player->ready) return;
    memset(&event, 0, sizeof(event));
    event.type = connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
    event.gdevice.which = (SDL_JoystickID)which;
    (void)player_ui_nk_handle_event(player, &event);
}

static const char *player_ui_compatibility_word(const GameRecord *game,
                                               const char **detail) {
    if (detail != NULL) *detail = "";
    if (game == NULL) return "Untested";
    for (size_t i = 0u;
         i < sizeof(s_compatibility_records) / sizeof(s_compatibility_records[0]);
         ++i) {
        if ((game->title_id[0] != '\0' &&
             strcmp(game->title_id, s_compatibility_records[i].key) == 0) ||
            (game->disc_id[0] != '\0' &&
             strcmp(game->disc_id, s_compatibility_records[i].key) == 0) ||
            (game->title_name[0] != '\0' &&
             strcmp(game->title_name, s_compatibility_records[i].key) == 0)) {
            if (detail != NULL) *detail = s_compatibility_records[i].detail;
            return s_compatibility_records[i].word;
        }
    }
    if (game->is_experimental) {
        if (game->status == NK_STATUS_BOOTS || game->status == NK_STATUS_PLAYABLE) {
            return "Experimental";
        }
        return "Untested";
    }
    switch (game->status) {
        case NK_STATUS_BOOTS: return "Boots to a named boundary";
        case NK_STATUS_PLAYABLE: return "Partially works";
        case NK_STATUS_UNIDENTIFIED:
        case NK_STATUS_IDENTIFIED: return "Untested";
        case NK_STATUS_SUPPORTED_PREPARATION:
        case NK_STATUS_PREPARED:
        case NK_STATUS_VERIFIED:
        default: return "Not started";
    }
}

static bool player_ui_has_seen_boot(const GameRecord *game) {
    return game != NULL && (game->status == NK_STATUS_BOOTS ||
                            game->status == NK_STATUS_PLAYABLE);
}

static const char *player_ui_library_status_word(const GameRecord *game,
                                                 bool package_ready) {
    if (package_ready) return "Ready";
    if (game != NULL && game->is_experimental) {
        return player_ui_has_seen_boot(game) ? "Experimental" : "Untested";
    }
    return "Needs setup";
}

/* Status colours follow the design system: Ready teal, experimental and
   untested info blue (with their own shapes), needs setup warning gold. */
static uint32_t player_ui_library_status_color(const GameRecord *game,
                                              bool package_ready) {
    if (package_ready) return PLAYER_UI_READY;
    if (game != NULL && game->is_experimental) return PLAYER_UI_INFO;
    return PLAYER_UI_WARNING;
}

/* The project's own showcase demos carry project-drawn pictures. */
static unsigned player_ui_art_kind(const GameRecord *game) {
    if (game == NULL) return NK_UI_ART_NONE;
    if (strcmp(game->title_id, "showcase-scene-v1") == 0) return NK_UI_ART_SHOWCASE_SCENE;
    if (strcmp(game->title_id, "showcase-breakout-v1") == 0) return NK_UI_ART_SHOWCASE_BREAKOUT;
    return NK_UI_ART_NONE;
}

static const char *player_ui_library_status_marker(const GameRecord *game,
                                                   bool package_ready) {
    if (package_ready) return "●";
    if (game != NULL && game->is_experimental) {
        return player_ui_has_seen_boot(game) ? "◆" : "?";
    }
    return "▲";
}

static PlayerUiPrimaryAction player_ui_fill_title(PlayerUiNk *player,
                                                   const PlayerApp *app,
                                                   int index,
                                                   PlayerUiTitle *out) {
    const GameRecord *game;
    NkRuntimePackageStatus package_status;
    const char *reason;
    bool package_ready;
    bool checking;
    bool check_failed;
    PlayerUiPrimaryAction primary = PLAYER_UI_PRIMARY_NONE;
    if (player == NULL || app == NULL || out == NULL || index < 0 ||
        index >= app->game_count || index >= (int)PLAYER_UI_TITLE_CAP) {
        return primary;
    }
    game = &app->games[index];
    package_status = player_app_cached_runtime_package_status(app, game);
    package_ready = player_app_cached_game_has_runtime(app, game);
    checking = player_app_runtime_package_check_pending(app, game);
    check_failed = player_app_runtime_package_check_failed(app, game);
    reason = player_app_runtime_package_reason(app, game);
    player_library_status_text(package_ready, checking, check_failed,
                               game->assets_staged,
                               game->extracted_asset_count, reason,
                               player->status_text[index],
                               sizeof(player->status_text[index]));
    out->title = game->title_name;
    out->disc_id = game->disc_id;
    out->library_status = player_ui_library_status_word(game, package_ready);
    out->library_status_detail = package_ready || checking
        ? "" : player->status_text[index];
    out->compatibility_word = player_ui_compatibility_word(game,
                                                           &out->compatibility_detail);
    out->input_note =
        game->executable_eboot_kind == NK_ISO_EXEC_PSP_ENCRYPTED &&
        game->executable_selection == NK_ISO_EXEC_SELECTION_NONE
            ? "Nakagawa does not decrypt games; it uses decrypted files from your own copy."
            : "";
    out->status_rgba = player_ui_library_status_color(game, package_ready);
    out->status_marker = player_ui_library_status_marker(game, package_ready);
    out->art_kind = player_ui_art_kind(game);
    out->badge = out->art_kind != NK_UI_ART_NONE ? "Showcase demo" : "Your disc";
    out->icon = NULL;
    out->picture = NULL;
    if (out->art_kind == NK_UI_ART_NONE) {
        player_ui_art_get(player->art, game->iso_path, false, &out->icon, NULL);
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (out->icon != NULL && !player->icon_reported[index]) {
        player->icon_reported[index] = true;
        printf("[PLAYER_UI_TEST] next_ui_disc_icon index=%d width=%u height=%u\n", index,
               (unsigned)out->icon->width, (unsigned)out->icon->height);
    }
#endif
    out->save_folder_enabled = game->disc_id[0] != '\0';
    out->primary_action_enabled = false;
    out->primary_action = PLAYER_UI_PRIMARY_NONE;
    if (app->is_game_running) {
        out->primary_action_label = "STOP GAME";
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_STOP_GAME;
    } else if (package_ready) {
        out->primary_action_label = "PLAY";
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_PLAY;
    } else if (checking || package_status == NK_RUNTIME_PACKAGE_UNKNOWN) {
        out->primary_action_label = "CHECKING PACKAGE";
    } else if (check_failed) {
        out->primary_action_label = "RETRY PACKAGE CHECK";
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_RETRY_PACKAGE_CHECK;
    } else if (package_status == NK_RUNTIME_PACKAGE_STALE) {
        out->primary_action_label = "REBUILD PACKAGE";
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_BUILD;
    } else if (package_status == NK_RUNTIME_PACKAGE_MISSING) {
        out->primary_action_label = "BUILD PACKAGE";
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_BUILD;
    } else if (package_status == NK_RUNTIME_PACKAGE_INCOMPATIBLE) {
        out->primary_action_label = "REBUILD PACKAGE";
        out->primary_action_enabled = true;
        primary = PLAYER_UI_PRIMARY_REBUILD;
        (void)player_incompatible_reason_line(reason, player->status_text[index],
                                              sizeof(player->status_text[index]));
    } else if (game->assets_staged) {
        out->primary_action_label = "RUNTIME REQUIRED";
    } else {
        out->primary_action_label = "PREPARATION UNAVAILABLE";
    }
    out->primary_action = primary;
    return primary;
}

static void player_ui_fill_titles(PlayerUiNk *player, const PlayerApp *app,
                                  size_t count) {
    for (size_t i = 0u; i < count; ++i) {
        (void)player_ui_fill_title(player, app, (int)i, &player->titles[i]);
    }
}


static size_t player_ui_focused_title_index(NkUiId focused, size_t count) {
    uint32_t value = focused;
    if (value < PLAYER_UI_ID_LIBRARY_ITEM_BASE) return count;
    value -= PLAYER_UI_ID_LIBRARY_ITEM_BASE;
    return value < count ? (size_t)value : count;
}

bool player_ui_nk_render(PlayerUiNk *player, PlayerApp *app,
                         SDL_Renderer *renderer, SDL_Window *window,
                         PlayerUiAction *out_action) {
    int pixel_w = 0;
    int pixel_h = 0;
    int window_w = 0;
    int window_h = 0;
    float display_scale;
    float point_to_pixel;
    double now_s;
    NkUiScale scale;
    NkUiEvent repeats[8];
    size_t repeat_count;
    const NkUiDrawList *draw_list;
    PlayerUiAction action = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u};
    PlayerUiRoute route;
    if (out_action != NULL) {
        out_action->kind = PLAYER_UI_ACTION_NONE;
        out_action->primary = PLAYER_UI_PRIMARY_NONE;
        out_action->title_index = 0u;
    }
    if (player == NULL || !player->ready || app == NULL || renderer == NULL ||
        window == NULL) {
        return false;
    }
    if (!SDL_GetRenderOutputSize(renderer, &pixel_w, &pixel_h) ||
        pixel_w <= 0 || pixel_h <= 0) {
        return false;
    }
    (void)SDL_GetWindowSize(window, &window_w, &window_h);
    display_scale = SDL_GetWindowDisplayScale(window);
    if (!(display_scale > 0.0f)) display_scale = 1.0f;
    player->viewport.pixel_w = pixel_w;
    player->viewport.pixel_h = pixel_h;
    player->viewport.display_scale = display_scale;
    player->viewport.ui_scale_pct = 0;
    player->viewport.text_size_pct = 100;
    scale = nk_ui_scale_for_viewport(&player->viewport);
    {
        /* Prompts follow the last input device ("Match input"). */
        NkUiPrefs prefs;
        memset(&prefs, 0, sizeof(prefs));
        prefs.prompt_style = nk_ui_input_prompt_style(&player->input.input);
        nk_ui_set_prefs(player->ui, &prefs);
    }
    point_to_pixel = window_w > 0 ? (float)pixel_w / (float)window_w
                                  : display_scale;
    nk_ui_sdl_input_set_event_scale(&player->input,
                                    point_to_pixel / scale.density);
    nk_ui_set_viewport(player->ui, &player->viewport);
    nk_ui_sdlr_set_scale(player->backend, scale.density);
    now_s = (double)SDL_GetTicksNS() / 1000000000.0;
    repeat_count = nk_ui_input_poll(&player->input.input, now_s, repeats,
                                    sizeof(repeats) / sizeof(repeats[0]));
    for (size_t i = 0u; i < repeat_count; ++i) {
        nk_ui_push_event(player->ui, &repeats[i]);
    }
    player_ui_art_pump(player->art);
    /* Sounds follow the setting, and stay closed while a game runs so its own
       audio is left alone. */
    player_ui_sounds_set_enabled(player->sounds,
                                 app->settings.ui_sounds && !app->is_game_running);
    nk_ui_begin(player->ui, now_s);
    {
        NkUiId restore = nk_ui_router_take_focus_restore(player->router);
        if (restore != 0u) nk_ui_focus(player->ui, restore);
    }
    route = nk_ui_router_current(player->router);
    player_ui_flow_focus(player, app);
    if (player_ui_nk_flow_view(app->active_view)) {
        action = player_ui_render_flow(player, app, scale, now_s);
        if (action.kind != PLAYER_UI_ACTION_NONE && out_action != NULL) {
            *out_action = action;
        }
    } else if (route == PLAYER_UI_ROUTE_LIBRARY) {
        size_t count = app->game_count > 0 ? (size_t)app->game_count : 0u;
        size_t columns = 1u;
        size_t page_size = player_ui_library_page_size(scale.logical_w,
                                                       scale.logical_h, &columns);
        size_t slots;
        if (count > PLAYER_UI_TITLE_CAP) count = PLAYER_UI_TITLE_CAP;
        /* The grid ends with the add card, so it has one slot more than titles. */
        slots = count > 0u ? count + 1u : 0u;
        player_ui_fill_titles(player, app, count);
        {
            size_t focused_index = player_ui_focused_title_index(
                nk_ui_focused(player->ui), count);
            if (focused_index < count) {
                app->selected_game_index = (int)focused_index;
                player->library_page = focused_index / page_size;
            }
        }
        if (nk_ui_focused(player->ui) == PLAYER_UI_ID_LIBRARY_ADD && slots > 0u) {
            player->library_page = (slots - 1u) / page_size;
        }
        if (slots == 0u) player->library_page = 0u;
        else if (player->library_page > (slots - 1u) / page_size) {
            player->library_page = (slots - 1u) / page_size;
        }
        PlayerUiLibraryModel model;
        memset(&model, 0, sizeof(model));
        model.titles = player->titles;
        model.title_count = count;
        model.page_index = player->library_page;
        model.page_size = page_size;
        model.columns = columns;
        model.selected_index = app->selected_game_index >= 0
            ? (size_t)app->selected_game_index : 0u;
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        if (slots > page_size) {
            size_t page_count = (slots + page_size - 1u) / page_size;
            snprintf(player->library_page_label,
                     sizeof(player->library_page_label), "Page %zu of %zu",
                     player->library_page + 1u, page_count);
            model.page_label = player->library_page_label;
        } else {
            player->library_page_label[0] = '\0';
            model.page_label = player->library_page_label;
        }
        action = player_ui_screen_library(player->ui, &model);
        if (action.kind == PLAYER_UI_ACTION_OPEN_TITLE && action.title_index < count) {
            app->selected_game_index = (int)action.title_index;
            nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
            (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_TITLE);
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_PAGE_PREVIOUS) {
            if (player->library_page > 0u) --player->library_page;
            action.kind = PLAYER_UI_ACTION_NONE;
        } else if (action.kind == PLAYER_UI_ACTION_PAGE_NEXT) {
            if ((player->library_page + 1u) * page_size < slots) {
                ++player->library_page;
            }
            action.kind = PLAYER_UI_ACTION_NONE;
        }
    } else if (route == PLAYER_UI_ROUTE_TITLE) {
        int index = app->selected_game_index;
        if (index < 0 || index >= app->game_count ||
            index >= (int)PLAYER_UI_TITLE_CAP) {
            nk_ui_router_replace(player->router, PLAYER_UI_ROUTE_LIBRARY);
        } else {
            PlayerUiTitleModel model;
            player_ui_fill_titles(player, app, (size_t)app->game_count >
                                  PLAYER_UI_TITLE_CAP ? PLAYER_UI_TITLE_CAP :
                                  (size_t)app->game_count);
            if (player->titles[index].art_kind == NK_UI_ART_NONE) {
                player_ui_art_get(player->art, app->games[index].iso_path, true,
                                  &player->titles[index].icon,
                                  &player->titles[index].picture);
            }
            memset(&model, 0, sizeof(model));
            model.title = &player->titles[index];
            model.title_index = (size_t)index;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            action = player_ui_screen_title(player->ui, &model);
            if (action.kind == PLAYER_UI_ACTION_BACK) {
                nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_PRIMARY ||
                       action.kind == PLAYER_UI_ACTION_OPEN_SAVE_FOLDER) {
                if (out_action != NULL) *out_action = action;
            }
        }
    } else if (route == PLAYER_UI_ROUTE_ABOUT) {
        PlayerUiAboutModel model;
        memset(&model, 0, sizeof(model));
        model.version_line = player->version_line;
        model.build_commit = NK_PLAYER_BUILD_COMMIT;
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        action = player_ui_screen_about(player->ui, &model);
    }
    /* The top-bar sections switch the root screen. */
    if (action.kind == PLAYER_UI_ACTION_OPEN_ABOUT) {
        /* About opens over the screen it was chosen from, so Back returns
           there with the ABOUT tab focused. */
        nk_ui_router_set_focus(player->router, PLAYER_UI_ID_TAB_ABOUT);
        (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_ABOUT);
        nk_ui_focus(player->ui, PLAYER_UI_ID_TAB_ABOUT);
        action.kind = PLAYER_UI_ACTION_NONE;
    } else if (action.kind == PLAYER_UI_ACTION_OPEN_LIBRARY) {
        (void)nk_ui_router_replace(player->router, PLAYER_UI_ROUTE_LIBRARY);
        nk_ui_focus(player->ui, PLAYER_UI_ID_TAB_LIBRARY);
        action.kind = PLAYER_UI_ACTION_NONE;
    }
    nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
    draw_list = nk_ui_end(player->ui);
    if (draw_list == NULL) return false;
    {
        NkUiId focus_now = nk_ui_focused(player->ui);
        if (player->pending_sound < 0 && player->sound_focus != 0u && focus_now != 0u &&
            focus_now != player->sound_focus) {
            player->pending_sound = PLAYER_UI_SOUND_MOVE;
        }
        player->sound_focus = focus_now;
        if (player->pending_sound >= 0) {
            player_ui_sounds_play(player->sounds, (PlayerUiSound)player->pending_sound);
            player->pending_sound = -1;
        }
    }
    (void)SDL_SetRenderDrawColor(renderer, 11u, 15u, 20u, 255u);
    (void)SDL_RenderClear(renderer);
    if (!nk_ui_sdlr_render(player->backend, draw_list)) return false;
    SDL_RenderPresent(renderer);
    if (out_action != NULL && action.kind != PLAYER_UI_ACTION_NONE) {
        *out_action = action;
    }
    return true;
}
