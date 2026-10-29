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
    const char *category;
    /* The player tier when a player's own run says more (or less) than the
       category's tier; NONE takes player_ui_tier_for_category(category). */
    PlayerUiTier tier;
    const char *expect;
    const char *known_issues;
} PlayerUiCompatibilityRecord;

/* What a player can expect from each title in the public compatibility table,
   in plain words, and its player tier (#1297). The category is the table's own
   word, kept for the developer view: it describes the project's test route,
   not this PC. Lookup is data driven so the screen never assigns a
   compatibility claim from layout code. */
static const PlayerUiCompatibilityRecord s_compatibility_records[] = {
    {"display-smoke-v1", "Works", PLAYER_UI_TIER_NONE,
     "Draws the project's moving test pattern. There is nothing to play.", ""},
    {"synthetic-allegrex-v1", "Works", PLAYER_UI_TIER_NONE,
     "Runs the project's test program, then closes.", ""},
    {"pspdev-phase5-v1", "Partially works", PLAYER_UI_TIER_WONT_BOOT,
     "A small test program from the Nakagawa project. It doesn't start from the player yet.",
     ""},
    {"synthetic-title2-v1", "Works", PLAYER_UI_TIER_NONE,
     "Runs the project's test program, then closes.", ""},
    {"showcase-scene-v1", "Partially works", PLAYER_UI_TIER_PLAYABLE,
     "Shows a lit, rotating cube that answers the stick and buttons, with sound.", ""},
    {"showcase-breakout-v1", "Partially works", PLAYER_UI_TIER_PLAYABLE,
     "Plays a full game of Breakout with sound and keeps your high score.", ""},
    /* Playable: the owner played full matches and Story Mode on 2026-10-24
       (#1297). */
    {"UCUS98701", "Boots to a named boundary", PLAYER_UI_TIER_PLAYABLE,
     "Plays full matches and Story Mode.",
     "Known issues: a few late sound wakes while loading, and some movies show black."},
    {"UCUS-98701", "Boots to a named boundary", PLAYER_UI_TIER_PLAYABLE,
     "Plays full matches and Story Mode.",
     "Known issues: a few late sound wakes while loading, and some movies show black."},
    {"PAC-MAN Championship Edition", "Partially works", PLAYER_UI_TIER_NONE,
     "Reaches the title screen and plays a stage. The rest of the game has not been tried yet.",
     ""}
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
    char disc_reason[PLAYER_UI_BLOCKED_REASON_MAX];
    char disc_profile[512];
    char settings_controller[160];
    char settings_saves[NK_MAX_PATH];
    char settings_tools[PLAYER_UI_SETTINGS_TOOLS][192];
    /* The factory reset check's folders, looked up when the check opens. */
    char reset_folders[PLAYER_UI_RESET_FOLDERS][NK_MAX_PATH];
    size_t reset_folder_count;
    bool reset_folders_ready;
    char update_folder[NK_MAX_PATH];
    char update_notice[96];
    char pad_bindings[PLAYER_UI_CONTROLLER_BUTTONS][128];
    InputBindingGlyph pad_glyphs[PLAYER_UI_CONTROLLER_BUTTONS];
    char pad_scope[96];
    char pad_status[400];
    char pad_deadzone[48];
    char pad_trigger[48];
    char pad_live[PLAYER_UI_CONTROLLER_LIVE_LINES][96];
    PlayerView last_view;   /* the view drawn last, for the focus a Settings page returns to */
    unsigned press_screen;  /* the screen a pointer press can still finish on */
    unsigned flow_screen;   /* which flow screen drew last; 0 outside them */
    NkUiId pre_flow_focus;  /* the library focus to give back on leaving */
    int pre_flow_game_count; /* titles in the library when the flow opened */
    /* The title page's PLAY or BUILD PACKAGE went off while it had focus
       (STARTING..., a running game): focus returns to it when it comes back
       on, unless the user has moved since. title_primary_fallback is where
       focus went meanwhile. */
    bool title_primary_refocus;
    NkUiId title_primary_fallback;
    NkUiPromptImages prompt_images;  /* Kenney glyphs; empty ones keep the geometry */
    PlayerUiArt *art;                /* disc ICON0 / PIC1, decoded at run time */
    PlayerUiSounds *sounds;          /* optional launcher sounds (ui_sounds) */
    int pending_sound;               /* a PlayerUiSound for this frame, or -1 */
    NkUiId sound_focus;              /* focus after the last frame, for the move sound */
    /* Remove from library (#1304): the title the dialog is for, what it lists,
       and the saves box, which every dialog opens unticked. */
    PlayerRemovalPlan remove_plan;
    PlayerUiRemoveItem remove_items[PLAYER_UI_REMOVE_ITEM_CAP];
    size_t remove_item_count;
    int remove_index;
    char remove_disc[NK_MAX_DISC_ID_LEN];
    bool remove_delete_saves;
    bool remove_focus_pending;
    /* STOP GAME asks first: the title page the question was asked from. */
    int stop_index;
    char stop_disc[NK_MAX_DISC_ID_LEN];
    bool stop_focus_pending;
    /* After a removal the library focuses the tile that took its place. */
    bool library_focus_pending;
    size_t library_focus_index;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    bool icon_reported[PLAYER_UI_TITLE_CAP];
    char status_reported[PLAYER_UI_TITLE_CAP][32]; /* the status word last printed */
    int disc_reported_view;  /* the game-found card whose rows were printed, or -1 */
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

/* The launcher's display rule (the profile folder as "~"), applied to a
   string the page will shorten itself; nk_ui applies it again when it draws. */
static void player_ui_display_in_place(char *text, size_t size) {
    char shown[NK_MAX_PATH];
    if (text == NULL || size == 0u || strlen(text) >= sizeof(shown)) return;
    (void)player_display_text(text, shown, sizeof(shown));
    snprintf(text, size, "%s", shown);
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
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    /* Stands in for a ZIP extracted without its fonts folder. */
    if (getenv("NK_UI_TEST_HIDE_UI_FONTS") != NULL) {
        player_ui_set_error(error, error_size,
                            "its fonts (assets\\fonts\\ui) are missing or damaged");
        return false;
    }
#endif
    for (int font = 0; font < NK_UI_FONT_COUNT; ++font) {
        if (!player_ui_add_font(ttf, font, base_path, nk_ui_font_path(font))) {
            player_ui_set_error(error, error_size,
                                "its fonts (assets\\fonts\\ui) are missing or damaged");
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

bool player_ui_nk_built_with_launcher(void) {
    return nk_ui_sdlr_available() && nk_ui_ttf_available();
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
    /* The launcher's display rules, as the old renderer applied them to every
       string it drew: the profile folder shows as "~". */
    config.display_text = player_display_text;
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
    player->last_view = VIEW_LIBRARY;
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    player->disc_reported_view = -1;
#endif
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

static bool player_ui_disc_view(PlayerView view) {
    return view == VIEW_INSPECTING || view == VIEW_SUPPORTED_TITLE ||
           view == VIEW_EXPERIMENTAL_TITLE || view == VIEW_UNSUPPORTED_TITLE;
}

static bool player_ui_settings_view(PlayerView view) {
    return view == VIEW_SETTINGS || view == VIEW_ONLINE_SETTINGS ||
           view == VIEW_PREREQ_ABOUT || view == VIEW_CONFIRM_REMOVE_TOOLS ||
           view == VIEW_BUILD_CACHE || view == VIEW_PSP_FONTS || view == VIEW_UPDATES ||
           view == VIEW_CONFIRM_FACTORY_RESET;
}

/* The views the next UI draws: the library (router screens), the setup wizard,
   build, build tool download and error screens a first run passes through, the
   card a disc gets when it is added, Settings with the controller settings, and
   the developer log. */
static bool player_ui_nk_flow_view(PlayerView view) {
    return view == VIEW_SETUP_WIZARD || view == VIEW_BUILDING_PACKAGE ||
           view == VIEW_PREPARING ||
           view == VIEW_PREREQ_CONSENT ||
           view == VIEW_PREREQ_PROGRESS || view == VIEW_ERROR ||
           player_ui_disc_view(view) || player_ui_settings_view(view) ||
           view == VIEW_CONTROLLER_SETTINGS || view == VIEW_DEVELOPER_LOG;
}

bool player_ui_nk_draws_view(int view_value) {
    PlayerView view = (PlayerView)view_value;
    return view == VIEW_LIBRARY || view == PLAYER_VIEW_READY_LIBRARY ||
           player_ui_nk_flow_view(view);
}

bool player_ui_nk_view_active(const PlayerUiNk *player, const PlayerApp *app) {
    return player != NULL && player->ready && app != NULL &&
           player_ui_nk_draws_view(app->active_view);
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

/* A check's status word colour: only a check that keeps the disc out of the
   library is an error; anything else still to do is a warning (#1302). */
static uint32_t player_ui_check_rgba(const NkUiTheme *theme, const PlayerPreflightCheck *check) {
    if (check->status == PREFLIGHT_OK) return theme->ready;
    return player_preflight_check_blocks(check) ? theme->error : theme->warning;
}

static void player_ui_check_row(PlayerUiCheckRow *row, const NkUiTheme *theme,
                                const PlayerPreflightCheck *check) {
    row->code = player_preflight_label(check);
    row->message = player_preflight_plain_message(check);
    row->status = player_preflight_status_word(check);
    row->rgba = player_ui_check_rgba(theme, check);
}

/* The state's outcome kind as the screens' one; the two lists are in the same order. */
static PlayerUiNoteKind player_ui_note_kind(PlayerNoteKind kind) {
    switch (kind) {
        case PLAYER_NOTE_DONE: return PLAYER_UI_NOTE_DONE;
        case PLAYER_NOTE_WARNING: return PLAYER_UI_NOTE_WARNING;
        case PLAYER_NOTE_ERROR: return PLAYER_UI_NOTE_ERROR;
        case PLAYER_NOTE_NEUTRAL:
        default: return PLAYER_UI_NOTE_NEUTRAL;
    }
}

/* The setup wizard's model, from the same wizard state the existing renderer
   reads. */
static bool player_ui_unrecorded_experimental(const GameRecord *game);

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
            /* A compressed image shows the file the user picked. */
            model.iso_path = game->source_image_path[0] ? game->source_image_path
                                                        : game->iso_path;
            player_ui_format_size(size, sizeof(size), game->iso_size_bytes);
            snprintf(player->wizard_detail, sizeof(player->wizard_detail),
                     "Size: %s \xC2\xB7 Disc ID: %s", size,
                     game->disc_id[0] != '\0' ? game->disc_id : "Inspected");
            model.iso_detail = player->wizard_detail;
            model.status_message = app->wizard.status_message;
            break;
        case WIZARD_STEP_INSPECT_VERIFY:
            model.extracting = app->wizard.is_extracting;
            model.converting_package = app->psn_conversion_active;
            model.extraction_complete = app->wizard.extraction_complete;
            model.extraction_failed = app->wizard.extraction_failed;
            model.experimental = game->is_experimental;
            model.recorded = game->is_experimental && !player_ui_unrecorded_experimental(game);
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
                player_ui_check_row(&model.checks[model.check_count++], theme, check);
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
            model.font_message_kind = player_ui_note_kind(app->wizard.font_message_kind);
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

/* The game-found card's model: the inspected disc and its checks. The check
   that keeps a disc out is the card's reason, not one of its rows. */
static PlayerUiAction player_ui_render_disc(PlayerUiNk *player, PlayerApp *app,
                                            NkUiScale scale, double now_s) {
    const NkUiTheme *theme = nk_ui_theme(player->ui);
    const GameRecord *game = &app->inspecting_game;
    const PlayerCompatibilityPreflight *preflight = &app->wizard.preflight;
    const PlayerPreflightCheck *blocking = player_preflight_blocking_check(preflight);
    PlayerUiDiscModel model;
    memset(&model, 0, sizeof(model));
    switch (app->active_view) {
        case VIEW_INSPECTING: model.kind = PLAYER_UI_DISC_CHECKING; break;
        case VIEW_SUPPORTED_TITLE: model.kind = PLAYER_UI_DISC_SUPPORTED; break;
        case VIEW_EXPERIMENTAL_TITLE:
            /* A disc on the compatibility list is found, not untested, as its
               title page's PLAYABLE says. */
            model.kind = blocking != NULL ? PLAYER_UI_DISC_BLOCKED
                : player_ui_unrecorded_experimental(game) ? PLAYER_UI_DISC_UNTESTED
                : PLAYER_UI_DISC_SUPPORTED;
            break;
        case VIEW_UNSUPPORTED_TITLE:
        default: model.kind = PLAYER_UI_DISC_UNSUPPORTED; break;
    }
    model.title_name = game->title_name;
    model.disc_id = game->disc_id;
    model.iso_path = game->source_image_path[0] ? game->source_image_path : game->iso_path;
    if (model.kind == PLAYER_UI_DISC_BLOCKED) {
        snprintf(player->disc_reason, sizeof(player->disc_reason),
                 "%s Then open the disc again.", player_preflight_plain_message(blocking));
        model.reason = player->disc_reason;
    }
    if ((model.kind == PLAYER_UI_DISC_SUPPORTED || model.kind == PLAYER_UI_DISC_UNSUPPORTED) &&
        player_app_title_profile_refusal_for_disc(app, game->disc_id, player->disc_profile,
                                                  sizeof(player->disc_profile))) {
        model.profile_details = player->disc_profile;
        if (model.kind == PLAYER_UI_DISC_SUPPORTED) {
            model.profile_note = "A title profile on this PC couldn't be used, so Nakagawa uses "
                                 "its own entry for this game.";
        }
    }
    if (model.kind != PLAYER_UI_DISC_CHECKING) {
        for (size_t i = 0u; i < preflight->count && model.check_count < PLAYER_UI_DISC_CHECKS;
             ++i) {
            const PlayerPreflightCheck *check = &preflight->checks[i];
            /* The badge says whether the game was tested, and a disc read is
               only worth a row when it went wrong. */
            if (check == blocking || strcmp(check->code, "EXPERIMENTAL") == 0 ||
                (strcmp(check->code, "DISC_SFO") == 0 && check->status == PREFLIGHT_OK)) {
                continue;
            }
            player_ui_check_row(&model.checks[model.check_count++], theme, check);
        }
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (player->disc_reported_view != (int)app->active_view) {
        player->disc_reported_view = (int)app->active_view;
        printf("[PLAYER_UI_TEST] next_ui_disc kind=%d rows=%zu add=%d\n", (int)model.kind,
               model.check_count,
               model.kind == PLAYER_UI_DISC_SUPPORTED || model.kind == PLAYER_UI_DISC_UNTESTED);
        for (size_t i = 0u; i < model.check_count; ++i) {
            printf("[PLAYER_UI_TEST] next_ui_disc_row status=%s tone=%s label=%s message=%s\n",
                   model.checks[i].status,
                   model.checks[i].rgba == theme->error ? "error"
                       : model.checks[i].rgba == theme->ready ? "ready" : "warning",
                   model.checks[i].code, model.checks[i].message);
        }
        fflush(stdout);
    }
#endif
    model.now_s = app->settings.reduce_motion ? 0.0 : now_s;
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    return player_ui_screen_disc(player->ui, &model);
}

/* PlayerUiUpdateStep follows PlayerUpdatePhase value for value. */
typedef char player_ui_update_step_order[
    (PLAYER_UI_UPDATE_CHECKING == (int)PLAYER_UPDATE_CHECKING &&
     PLAYER_UI_UPDATE_DOWNLOADED == (int)PLAYER_UPDATE_DOWNLOADED &&
     PLAYER_UI_UPDATE_FAILED == (int)PLAYER_UPDATE_FAILED) ? 1 : -1];

/* The Settings pages' model, from the settings the existing renderer shows. */
static PlayerUiAction player_ui_render_settings(PlayerUiNk *player, PlayerApp *app,
                                                NkUiScale scale) {
    PlayerUiSettingsModel model;
    memset(&model, 0, sizeof(model));
    switch (app->active_view) {
        case VIEW_ONLINE_SETTINGS: model.page = PLAYER_UI_SETTINGS_PAGE_ONLINE; break;
        case VIEW_PREREQ_ABOUT: model.page = PLAYER_UI_SETTINGS_PAGE_NOTICES; break;
        case VIEW_CONFIRM_REMOVE_TOOLS: model.page = PLAYER_UI_SETTINGS_PAGE_REMOVE_TOOLS; break;
        case VIEW_BUILD_CACHE: model.page = PLAYER_UI_SETTINGS_PAGE_BUILD_CACHE; break;
        case VIEW_UPDATES: model.page = PLAYER_UI_SETTINGS_PAGE_UPDATES; break;
        case VIEW_CONFIRM_FACTORY_RESET:
            model.page = PLAYER_UI_SETTINGS_PAGE_FACTORY_RESET;
            if (!player->reset_folders_ready) {
                player->reset_folder_count = player_app_factory_reset_folders(
                    app, player->reset_folders, PLAYER_UI_RESET_FOLDERS);
                for (size_t i = 0; i < player->reset_folder_count; ++i) {
                    player_ui_display_in_place(player->reset_folders[i], NK_MAX_PATH);
                }
                player->reset_folders_ready = true;
            }
            for (size_t i = 0; i < player->reset_folder_count; ++i) {
                model.reset_folders[model.reset_folder_count++] = player->reset_folders[i];
            }
            break;
        case VIEW_PSP_FONTS:
            model.page = PLAYER_UI_SETTINGS_PAGE_FONTS;
            if (app->wizard.font_slot_detail[0][0] == '\0') player_app_refresh_font_status(app);
            for (int item = 0; item < NK_FONT_LATIN_INDEX_COUNT + 2 && item < 18; ++item) {
                model.font_lines[model.font_line_count++] = item == 0
                    ? app->wizard.font_slot_detail[NK_FONT_SLOT_JAPANESE]
                    : item == NK_FONT_LATIN_INDEX_COUNT + 1
                        ? app->wizard.font_slot_detail[NK_FONT_SLOT_KOREAN]
                        : app->wizard.font_latin_index_detail[item - 1];
            }
            model.font_message = app->wizard.font_message;
            model.font_message_kind = player_ui_note_kind(app->wizard.font_message_kind);
            break;
        case VIEW_SETTINGS:
        default: model.page = PLAYER_UI_SETTINGS_PAGE_MAIN; break;
    }
    model.resolution = player_ui_resolution_word(app->settings.resolution_scale);
    model.pacing = app->settings.fps_cap < 0 ? "PSP timing" : "No extra cap";
    model.scaling = player_present_scaling_label(app->settings.present_scaling);
    model.vsync = app->settings.vsync;
    model.fullscreen = app->settings.fullscreen;
    model.volume = app->settings.master_volume;
    model.ui_sounds = app->settings.ui_sounds;
    model.reduce_motion = app->settings.reduce_motion;
    model.launcher_fullscreen = app->settings.launcher_fullscreen;
    snprintf(player->settings_controller, sizeof(player->settings_controller), "%s",
             app->settings.controller_connected
                 ? (app->settings.controller_name[0] != '\0' ? app->settings.controller_name
                                                             : "controller connected")
                 : "keyboard and mouse");
    model.controller_line = player->settings_controller;
    /* nk_platform_get_path creates the folder, and it cannot change while the
       player runs, so it is looked up once. */
    if (player->settings_saves[0] == '\0' &&
        !nk_platform_get_path(NK_PATH_SAVES, player->settings_saves,
                              sizeof(player->settings_saves))) {
        player->settings_saves[0] = '\0';
    }
    model.saves_folder = player->settings_saves;
    model.online_mode = app->settings.online_mode;
    model.online_news = app->settings.online_news;
    model.online_reports = app->settings.online_reports;
    model.developer_mode = app->settings.developer_mode;
    model.share_system_libraries = app->settings.share_system_libraries;
    /* The build tools and cache page says what its clear button did, as the
       existing renderer's page does. */
    if (model.page == PLAYER_UI_SETTINGS_PAGE_BUILD_CACHE && app->settings_notice[0] != '\0') {
        model.notice = app->settings_notice;
    }
    for (size_t i = 0u; i < app->prerequisites.items.count &&
                        model.tool_count < PLAYER_UI_SETTINGS_TOOLS; ++i) {
        const PackagePrerequisite *item = &app->prerequisites.items.items[i];
        char *line = player->settings_tools[model.tool_count];
        if (!item->installed) continue;
        snprintf(line, sizeof(player->settings_tools[0]), "%.64s %.32s \xC2\xB7 %.80s", item->name,
                 item->version,
                 strcmp(item->license, "NOASSERTION") == 0 ? "licence review in the works"
                                                            : item->license);
        model.tools[model.tool_count++] = line;
    }
    if (model.page == PLAYER_UI_SETTINGS_PAGE_UPDATES) {
        const PlayerUpdateState *update = &app->update;
        const NkUpdateRelease *newest = &update->check.newest;
        bool checked = update->phase >= PLAYER_UPDATE_CURRENT &&
                       (update->phase != PLAYER_UPDATE_FAILED ||
                        update->failed_step != PLAYER_UPDATE_CHECKING);
        model.update_step = (PlayerUiUpdateStep)update->phase;
        model.update_failed_step = (PlayerUiUpdateStep)update->failed_step;
        model.check_updates_at_start = app->settings.check_updates_at_start;
        model.current_version = NK_PLAYER_VERSION;
        if (checked) {
            model.update_version = newest->version;
            model.update_prerelease = newest->prerelease;
            model.update_published = newest->published;
            model.update_notes = newest->notes;
            model.update_page_url = newest->page_url;
            model.update_has_zip = newest->has_zip;
#if defined(_WIN32) || defined(_WIN64)
            model.update_can_download = newest->has_zip && update->check.newer;
#endif
            model.update_zip_name = newest->zip_name;
            model.update_zip_bytes = newest->zip_size;
        }
        model.update_received_bytes = update->received_bytes;
        /* The folder the last step left something in: the new version, or the
           downloaded ZIP's folder. */
        player->update_folder[0] = '\0';
        if (update->phase == PLAYER_UPDATE_INSTALLED) {
            snprintf(player->update_folder, sizeof(player->update_folder), "%s",
                     update->install_dir);
        } else if (update->zip_path[0] != '\0') {
            size_t cut = strlen(update->zip_path);
            while (cut > 0u && update->zip_path[cut - 1u] != '/' &&
                   update->zip_path[cut - 1u] != '\\') {
                --cut;
            }
            snprintf(player->update_folder, sizeof(player->update_folder), "%.*s",
                     (int)(cut > 0u ? cut - 1u : 0u), update->zip_path);
        }
        /* The page shortens a long folder from the middle; the profile folder
           becomes "~" first, so a cut never keeps part of the user name. */
        player_ui_display_in_place(player->update_folder, sizeof(player->update_folder));
        model.update_folder = player->update_folder;
        model.update_error_title = update->error.title;
        model.update_error_message = update->error.message;
    }
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    return player_ui_screen_settings(player->ui, &model);
}

/* A share of the full axis range, as a whole percent. */
static int player_ui_axis_percent(int value) {
    return (value * 100) / 32767;
}

/* Whose names a controller's pills carry, from the glyph set it is shown in. */
static InputPadNames player_ui_pad_names(NkUiPromptStyle style) {
    switch (style) {
    case NK_UI_PROMPT_LETTERS_A_SOUTH: return INPUT_PAD_NAMES_A_SOUTH;
    case NK_UI_PROMPT_LETTERS_A_EAST: return INPUT_PAD_NAMES_A_EAST;
    case NK_UI_PROMPT_SHAPES: return INPUT_PAD_NAMES_SHAPES;
    default: return INPUT_PAD_NAMES_NEUTRAL;
    }
}

/* A binding's glyph as the prompt glyph that draws it (#1299): face buttons, the
   shoulders, Start and the D-pad as the prompt bar draws them, the arrow keys as the
   arrow cluster, and every other key or pad input as a keycap or pill with its legend. */
static PlayerUiBindingGlyph player_ui_binding_glyph(const InputBindingGlyph *glyph) {
    PlayerUiBindingGlyph out;
    memset(&out, 0, sizeof(out));
    out.shown = true;
    switch (glyph->kind) {
    case INPUT_GLYPH_FACE_SOUTH: out.action = NK_UI_PROMPT_ACT_CONFIRM; break;
    case INPUT_GLYPH_FACE_EAST: out.action = NK_UI_PROMPT_ACT_BACK; break;
    case INPUT_GLYPH_FACE_WEST: out.action = NK_UI_PROMPT_ACT_OPTIONS; break;
    case INPUT_GLYPH_FACE_NORTH: out.action = NK_UI_PROMPT_ACT_DETAILS; break;
    case INPUT_GLYPH_SHOULDER_LEFT: out.action = NK_UI_PROMPT_ACT_TAB_PREV; break;
    case INPUT_GLYPH_SHOULDER_RIGHT: out.action = NK_UI_PROMPT_ACT_TAB_NEXT; break;
    case INPUT_GLYPH_PAD_START: out.action = NK_UI_PROMPT_ACT_MENU; break;
    case INPUT_GLYPH_DPAD:
    case INPUT_GLYPH_ARROW_KEY: out.action = NK_UI_PROMPT_ACT_MOVE; break;
    case INPUT_GLYPH_PAD_LEGEND:
        out.action = NK_UI_PROMPT_ACT_TAB_PREV;   /* the shoulder's pill */
        out.label = glyph->legend;
        break;
    case INPUT_GLYPH_KEY:
        out.action = NK_UI_PROMPT_ACT_CONFIRM;    /* a keycap */
        out.label = glyph->legend;
        break;
    case INPUT_GLYPH_NONE:
    default: out.shown = false; break;
    }
    return out;
}

/* The controller settings model, from the same input_settings state the
   existing renderer reads. */
static PlayerUiAction player_ui_render_controller(PlayerUiNk *player, PlayerApp *app,
                                                  NkUiScale scale) {
    const InputSettingsState *input = &app->input_settings;
    const int16_t *axes = app->host_axes_live;
    PlayerUiControllerModel model;
    memset(&model, 0, sizeof(model));
    model.keyboard = input->device == INPUT_DEVICE_KEYBOARD;
    model.capturing = input->capturing ? input->capture_control : -1;
    /* Keys as keycaps; the controller in the set of the pad the user would pick up,
       even while the keyboard drives the page. */
    model.glyph_style = model.keyboard ? NK_UI_PROMPT_KEYCAPS
                                       : nk_ui_input_pad_prompt_style(&player->input.input);
    for (size_t i = 0u; i < PLAYER_UI_CONTROLLER_BUTTONS; ++i) {
        model.names[i] = input_settings_control_name((int)i);
        if (model.keyboard) {
            /* A key used twice is refused when it is pressed, so keys never conflict. */
            input_settings_format_key_binding(input, (int)i, player->pad_bindings[i],
                                              sizeof(player->pad_bindings[i]));
            input_settings_key_glyph(input, (int)i, &player->pad_glyphs[i]);
        } else {
            input_settings_format_binding(input, (int)i, player->pad_bindings[i],
                                          sizeof(player->pad_bindings[i]));
            model.conflicted[i] = input_settings_is_control_conflicted(input, (int)i);
            input_settings_pad_glyph(input, (int)i, player_ui_pad_names(model.glyph_style),
                                     &player->pad_glyphs[i]);
        }
        model.bindings[i] = player->pad_bindings[i];
        model.glyphs[i] = player_ui_binding_glyph(&player->pad_glyphs[i]);
    }
    model.scope_title = input_settings_is_title_scope(input);
    model.offer_scope = player_app_selected_disc_id(app) != NULL;
    if (model.scope_title) {
        snprintf(player->pad_scope, sizeof(player->pad_scope), "%s only",
                 input_settings_scope_label(input));
    } else {
        snprintf(player->pad_scope, sizeof(player->pad_scope), "global mapping");
    }
    model.scope = player->pad_scope;
    if (model.keyboard && input->key_note[0] != '\0') {
        model.status = input->key_note;
        model.status_error = true;
    } else if (!model.keyboard && input_settings_has_conflicts(input)) {
        model.status = input_settings_get_conflict_summary(input);
        model.status_error = true;
    } else if (input->has_load_diagnostic) {
        snprintf(player->pad_status, sizeof(player->pad_status), "%s", input->load_diagnostic);
        model.status = player->pad_status;
    } else if (input->has_save_diagnostic) {
        snprintf(player->pad_status, sizeof(player->pad_status), "%s", input->save_diagnostic);
        model.status = player->pad_status;
    } else if (model.capturing >= 0) {
        snprintf(player->pad_status, sizeof(player->pad_status),
                 model.keyboard ? "Press the key for %s. %d s left; Esc stops."
                                : "Press the controller button or trigger for %s. %d s left; "
                                  "Esc stops.",
                 input_settings_control_name(model.capturing),
                 (input_settings_get_capture_remaining_ms(input) + 999) / 1000);
        model.status = player->pad_status;
    } else if (model.keyboard) {
        model.status = "Choose a button, then press the key to use for it.";
    } else if (app->settings.controller_connected) {
        snprintf(player->pad_status, sizeof(player->pad_status),
                 "Connected: %s. Choose a button to change it.",
                 app->settings.controller_name[0] != '\0' ? app->settings.controller_name
                                                          : "controller");
        model.status = player->pad_status;
    } else {
        model.status = "No controller connected. The keyboard works, and these are the "
                       "controller defaults.";
    }
    snprintf(player->pad_deadzone, sizeof(player->pad_deadzone), "Dead zone %d%%",
             player_ui_axis_percent(input->profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner));
    snprintf(player->pad_trigger, sizeof(player->pad_trigger), "Triggers at %d%%",
             player_ui_axis_percent(input->profile.trigger_threshold));
    model.deadzone = player->pad_deadzone;
    model.trigger = player->pad_trigger;
    switch (input_settings_is_calibrating(input) ? input_settings_get_calibration_stage(input)
                                                 : CALIBRATION_STAGE_INACTIVE) {
        case CALIBRATION_STAGE_REST: {
            int ms = input->calib.elapsed_ms > 1000 ? 1000 : input->calib.elapsed_ms;
            model.page = PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST;
            model.calibration_fraction = ms > 0 ? (float)ms / 1000.0f : 0.0f;
            snprintf(player->pad_live[0], sizeof(player->pad_live[0]),
                     "Stick at rest   %6d, %6d", input->calib.sampled_rest_x,
                     input->calib.sampled_rest_y);
            snprintf(player->pad_live[1], sizeof(player->pad_live[1]),
                     "Triggers at rest L %d  R %d", input->calib.sampled_rest_lt,
                     input->calib.sampled_rest_rt);
            model.live_count = 2u;
            break;
        }
        case CALIBRATION_STAGE_EXTREMES:
            model.page = PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_EXTREMES;
            snprintf(player->pad_live[0], sizeof(player->pad_live[0]),
                     "Stick X  %6d to %6d  now %6d", input->calib.sampled_min_x,
                     input->calib.sampled_max_x, axes[NK_HOST_AXIS_LEFTX]);
            snprintf(player->pad_live[1], sizeof(player->pad_live[1]),
                     "Stick Y  %6d to %6d  now %6d", input->calib.sampled_min_y,
                     input->calib.sampled_max_y, axes[NK_HOST_AXIS_LEFTY]);
            snprintf(player->pad_live[2], sizeof(player->pad_live[2]),
                     "Left trigger   %6d to %6d", input->calib.sampled_rest_lt,
                     input->calib.sampled_max_lt);
            snprintf(player->pad_live[3], sizeof(player->pad_live[3]),
                     "Right trigger  %6d to %6d", input->calib.sampled_rest_rt,
                     input->calib.sampled_max_rt);
            model.live_count = 4u;
            break;
        case CALIBRATION_STAGE_RESULT:
            model.page = PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_RESULT;
            snprintf(player->pad_live[0], sizeof(player->pad_live[0]),
                     "Stick X  rest %6d  %6d to %6d", input->calib.result_rest_x,
                     input->calib.result_min_x, input->calib.result_max_x);
            snprintf(player->pad_live[1], sizeof(player->pad_live[1]),
                     "Stick Y  rest %6d  %6d to %6d", input->calib.result_rest_y,
                     input->calib.result_min_y, input->calib.result_max_y);
            snprintf(player->pad_live[2], sizeof(player->pad_live[2]),
                     "Triggers rest %6d  full %6d", input->calib.result_trigger_rest,
                     input->calib.result_trigger_extreme);
            model.live_count = 3u;
            break;
        default: {
            uint8_t psp_x = 128u;
            uint8_t psp_y = 128u;
            size_t used;
            int pressed = 0;
            nk_input_profile_eval_analog(&input->profile, axes, &psp_x, &psp_y);
            snprintf(player->pad_live[0], sizeof(player->pad_live[0]), "Stick  %6d, %6d",
                     axes[NK_HOST_AXIS_LEFTX], axes[NK_HOST_AXIS_LEFTY]);
            snprintf(player->pad_live[1], sizeof(player->pad_live[1]), "PSP    %3u, %3u  %s",
                     (unsigned)psp_x, (unsigned)psp_y,
                     psp_x != 128u || psp_y != 128u ? "moving" : "in dead zone");
            snprintf(player->pad_live[2], sizeof(player->pad_live[2]), "Triggers L %d  R %d",
                     axes[NK_HOST_AXIS_LEFT_TRIGGER], axes[NK_HOST_AXIS_RIGHT_TRIGGER]);
            used = (size_t)snprintf(player->pad_live[3], sizeof(player->pad_live[3]),
                                    "Pressed ");
            for (int b = 0; b < NK_HOST_BUTTON_COUNT && pressed < 4 &&
                            used < sizeof(player->pad_live[3]); ++b) {
                if (!app->host_buttons_live[b]) continue;
                used += (size_t)snprintf(player->pad_live[3] + used,
                                         sizeof(player->pad_live[3]) - used, "%s%s",
                                         pressed > 0 ? ", " : "",
                                         nk_host_button_name((NkHostGamepadButton)b));
                ++pressed;
            }
            if (pressed == 0) {
                snprintf(player->pad_live[3] + used, sizeof(player->pad_live[3]) - used,
                         "nothing");
            }
            model.live_count = 4u;
            break;
        }
    }
    for (size_t i = 0u; i < model.live_count; ++i) model.live[i] = player->pad_live[i];
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    return player_ui_screen_controller(player->ui, &model);
}

/* The developer log panel's model: the log lines player_state.c keeps. */
static PlayerUiAction player_ui_render_developer_log(PlayerUiNk *player, PlayerApp *app,
                                                     NkUiScale scale) {
    PlayerUiDeveloperLogModel model;
    size_t count = player_dev_log_count(app);
    memset(&model, 0, sizeof(model));
    if (count > PLAYER_UI_DEVELOPER_LOG_LINES) count = PLAYER_UI_DEVELOPER_LOG_LINES;
    for (size_t i = 0u; i < count; ++i) model.lines[i] = player_dev_log_line(app, i);
    model.line_count = count;
    model.notice = app->dev_log.notice;
    model.logical_width = scale.logical_w;
    model.logical_height = scale.logical_h;
    return player_ui_screen_developer_log(player->ui, &model);
}

static PlayerUiAction player_ui_render_flow(PlayerUiNk *player, PlayerApp *app,
                                            NkUiScale scale, double now_s) {
    PlayerUiAction none = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u};
    if (app->active_view == VIEW_SETUP_WIZARD) {
        return player_ui_render_wizard(player, app, scale);
    }
    if (app->active_view == VIEW_CONTROLLER_SETTINGS) {
        return player_ui_render_controller(player, app, scale);
    }
    if (app->active_view == VIEW_DEVELOPER_LOG) {
        return player_ui_render_developer_log(player, app, scale);
    }
    if (player_ui_disc_view(app->active_view)) {
        return player_ui_render_disc(player, app, scale, now_s);
    }
    if (player_ui_settings_view(app->active_view)) {
        return player_ui_render_settings(player, app, scale);
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
        model.fraction = package_builder_compile_fraction(build);
        model.notice = package_builder_compile_notice(build);
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
    if (app->active_view == VIEW_PREPARING) {
        /* No preparation pipeline is connected in this build: the card says
           so plainly, with the way back, and shows no fake progress. */
        PlayerUiErrorModel model;
        memset(&model, 0, sizeof(model));
        model.badge = "Not available";
        model.title = "Preparation Unavailable";
        model.message = "No preparation pipeline is connected in this build, so no "
                        "preparation work is running.";
        model.recovery_label = "Return to Library";
        model.logical_width = scale.logical_w;
        model.logical_height = scale.logical_h;
        return player_ui_screen_error(player->ui, &model);
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
/* The flow screen the app is on: its view, plus the sub-screen some views
   step through (the wizard's step and copy, a finished build, a calibration
   step, an update step). 0 off the flow views. */
static unsigned player_ui_flow_screen(const PlayerApp *app) {
    unsigned screen;
    if (!player_ui_nk_flow_view(app->active_view)) return 0u;
    screen = (unsigned)app->active_view * 64u + 1u;
    switch (app->active_view) {
        case VIEW_SETUP_WIZARD:
            screen += (unsigned)app->wizard.step * 2u + (app->wizard.is_extracting ? 1u : 0u);
            break;
        case VIEW_BUILDING_PACKAGE:
            if (app->build_session.is_complete && !app->build_session.is_failed) screen += 1u;
            break;
        case VIEW_CONTROLLER_SETTINGS:
            screen += (unsigned)(input_settings_is_calibrating(&app->input_settings)
                                     ? input_settings_get_calibration_stage(&app->input_settings)
                                     : CALIBRATION_STAGE_INACTIVE);
            break;
        case VIEW_UPDATES: screen += (unsigned)app->update.phase; break;
        default: break;
    }
    return screen;
}

static void player_ui_flow_focus(PlayerUiNk *player, const PlayerApp *app) {
    unsigned screen = player_ui_flow_screen(app);
    NkUiId focus = 0u;
    if (player_ui_nk_flow_view(app->active_view)) {
        switch (app->active_view) {
            case VIEW_SETUP_WIZARD:
                focus = app->wizard.step == WIZARD_STEP_INSPECT_VERIFY &&
                        app->wizard.is_extracting
                    ? PLAYER_UI_ID_WIZARD_CANCEL : PLAYER_UI_ID_WIZARD_PRIMARY;
                break;
            case VIEW_BUILDING_PACKAGE:
                /* CANCEL BUILD is never the default focus, so a stray Enter or
                   South cannot stop a build; a move reaches it. A finished
                   build focuses BACK TO LIBRARY. */
                if (app->build_session.is_complete && !app->build_session.is_failed) {
                    focus = PLAYER_UI_ID_BUILD_ACTION;
                }
                break;
            case VIEW_PREREQ_CONSENT: focus = PLAYER_UI_ID_PREREQ_ACCEPT; break;
            case VIEW_PREREQ_PROGRESS: focus = PLAYER_UI_ID_PREREQ_CANCEL; break;
            case VIEW_ERROR:
            case VIEW_PREPARING: focus = PLAYER_UI_ID_ERROR_RECOVER; break;
            case VIEW_INSPECTING:
            case VIEW_SUPPORTED_TITLE:
            case VIEW_EXPERIMENTAL_TITLE:
            case VIEW_UNSUPPORTED_TITLE: focus = PLAYER_UI_ID_DISC_PRIMARY; break;
            case VIEW_SETTINGS:
                /* Back from one of its pages, focus the button that opened it. */
                focus = player->last_view == VIEW_ONLINE_SETTINGS ? PLAYER_UI_ID_SETTINGS_ONLINE
                    : player->last_view == VIEW_PREREQ_ABOUT ? PLAYER_UI_ID_SETTINGS_NOTICES
                    : player->last_view == VIEW_BUILD_CACHE ||
                            player->last_view == VIEW_CONFIRM_REMOVE_TOOLS
                        ? PLAYER_UI_ID_SETTINGS_BUILD_CACHE
                    : player->last_view == VIEW_PSP_FONTS ? PLAYER_UI_ID_SETTINGS_FONTS
                    : player->last_view == VIEW_UPDATES ? PLAYER_UI_ID_SETTINGS_UPDATES
                    : player->last_view == VIEW_CONFIRM_FACTORY_RESET
                        ? PLAYER_UI_ID_SETTINGS_FACTORY_RESET
                    : player->last_view == VIEW_CONTROLLER_SETTINGS
                        ? PLAYER_UI_ID_SETTINGS_CONTROLLER
                        : PLAYER_UI_ID_SETTINGS_RESOLUTION;
                break;
            case VIEW_CONTROLLER_SETTINGS: {
                /* Each calibration step is a screen of its own; the mapping
                   opens on the first button, or on Calibrate after a step. */
                GuidedCalibrationStage stage =
                    input_settings_is_calibrating(&app->input_settings)
                        ? input_settings_get_calibration_stage(&app->input_settings)
                        : CALIBRATION_STAGE_INACTIVE;
                focus = stage == CALIBRATION_STAGE_REST ? PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL
                    : stage != CALIBRATION_STAGE_INACTIVE
                        ? PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY
                    : player->flow_screen > screen && player->flow_screen < screen + 4u
                        ? PLAYER_UI_ID_CONTROLLER_CALIBRATE
                        : PLAYER_UI_ID_CONTROLLER_BUTTON(0u);
                break;
            }
            case VIEW_DEVELOPER_LOG: focus = PLAYER_UI_ID_DEVELOPER_LOG_CLOSE; break;
            case VIEW_PSP_FONTS: focus = PLAYER_UI_ID_SETTINGS_IMPORT_FONTS; break;
            case VIEW_UPDATES:
                /* Each step's result is a screen of its own, focused on what comes
                   next. CANCEL is the download's only control; a finished check
                   with nothing newer, a failure or a release without a Windows ZIP
                   focuses CHECK FOR UPDATES. */
                switch (app->update.phase) {
                    case PLAYER_UPDATE_AVAILABLE:
                        focus = app->update.check.newest.has_zip
                            ? PLAYER_UI_ID_UPDATE_DOWNLOAD : PLAYER_UI_ID_UPDATE_PAGE;
#if !defined(_WIN32) && !defined(_WIN64)
                        focus = PLAYER_UI_ID_UPDATE_PAGE;
#endif
                        break;
                    case PLAYER_UPDATE_DOWNLOADING: focus = PLAYER_UI_ID_UPDATE_CANCEL; break;
                    case PLAYER_UPDATE_DOWNLOADED: focus = PLAYER_UI_ID_UPDATE_INSTALL; break;
                    case PLAYER_UPDATE_INSTALLED: focus = PLAYER_UI_ID_UPDATE_FOLDER; break;
                    case PLAYER_UPDATE_CHECKING:
                    case PLAYER_UPDATE_INSTALLING: focus = PLAYER_UI_ID_SETTINGS_BACK; break;
                    default: focus = PLAYER_UI_ID_UPDATE_CHECK; break;
                }
                break;
            case VIEW_ONLINE_SETTINGS: focus = PLAYER_UI_ID_SETTINGS_ONLINE_MODE; break;
            case VIEW_PREREQ_ABOUT:
            case VIEW_CONFIRM_REMOVE_TOOLS:
            /* The safe answer, Keep everything, has the focus. */
            case VIEW_CONFIRM_FACTORY_RESET: focus = PLAYER_UI_ID_SETTINGS_BACK; break;
            case VIEW_BUILD_CACHE:
                /* Back from the removal check, focus the button that opened it. */
                focus = player->last_view == VIEW_CONFIRM_REMOVE_TOOLS
                    ? PLAYER_UI_ID_SETTINGS_REMOVE_TOOLS : PLAYER_UI_ID_SETTINGS_SHARE_LIBRARIES;
                break;
            default: break;
        }
    }
    player->last_view = app->active_view;
    if (app->active_view != VIEW_CONFIRM_FACTORY_RESET) player->reset_folders_ready = false;
    if (screen == player->flow_screen) return;
    if (player->flow_screen == 0u) {
        player->pre_flow_focus = nk_ui_focused(player->ui);
        player->pre_flow_game_count = app->game_count;
    }
    if (screen != 0u) {
        nk_ui_focus(player->ui, focus);
    } else if (app->game_count > player->pre_flow_game_count &&
               app->selected_game_index >= 0 &&
               app->selected_game_index < app->game_count) {
        /* A flow that added a game (ADD TO LIBRARY) ends on its card. */
        player->library_focus_pending = true;
        player->library_focus_index = (size_t)app->selected_game_index;
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

unsigned player_ui_nk_test_focus(const PlayerUiNk *player) {
    return player != NULL && player->ui != NULL ? (unsigned)nk_ui_focused(player->ui) : 0u;
}
#endif

/* Opens the Remove from library dialog for a title, listing what Nakagawa
   built for it as it is on disk now. */
static bool player_ui_open_remove(PlayerUiNk *player, const PlayerApp *app, int index) {
    static const char *const labels[PLAYER_REMOVAL_KIND_COUNT] = {
        "Built package", "Build cache", "Disc files Nakagawa copied"
    };
    if (!player_app_plan_game_removal(app, index, &player->remove_plan)) return false;
    player->remove_item_count = 0u;
    for (int kind = 0; kind < PLAYER_REMOVAL_KIND_COUNT; ++kind) {
        if (!player->remove_plan.kind_present[kind] ||
            player->remove_item_count >= PLAYER_UI_REMOVE_ITEM_CAP) {
            continue;
        }
        player->remove_items[player->remove_item_count].label = labels[kind];
        player->remove_items[player->remove_item_count].bytes =
            player->remove_plan.kind_bytes[kind];
        ++player->remove_item_count;
    }
    player->remove_index = index;
    snprintf(player->remove_disc, sizeof(player->remove_disc), "%s",
             app->games[index].disc_id);
    player->remove_delete_saves = false;
    player->remove_focus_pending = true;
    return true;
}

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

static PlayerUiTier player_ui_record_tier(const PlayerUiCompatibilityRecord *record) {
    return record->tier != PLAYER_UI_TIER_NONE ? record->tier
                                               : player_ui_tier_for_category(record->category);
}

/* The title's compatibility record: its player tier, what to expect and its
   known issues, in plain words. Returns the docs/COMPATIBILITY.md category for
   the developer view. A title with no record gets a sentence for its general
   state and the tier its category maps to. */
static const char *player_ui_compatibility_words(const GameRecord *game, const char **expect,
                                                 const char **known_issues,
                                                 PlayerUiTier *record_tier) {
    *expect = "No one has tried this game in Nakagawa yet.";
    *known_issues = "";
    *record_tier = PLAYER_UI_TIER_NONE;
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
            *expect = s_compatibility_records[i].expect;
            *known_issues = s_compatibility_records[i].known_issues;
            *record_tier = player_ui_record_tier(&s_compatibility_records[i]);
            return s_compatibility_records[i].category;
        }
    }
    if (game->is_experimental) {
        if (game->status == NK_STATUS_BOOTS || game->status == NK_STATUS_PLAYABLE) {
            *expect = "This game has started in Nakagawa, but no one has checked how "
                      "far it plays. It may stop early.";
            return "Experimental";
        }
        *expect = "No one has tried this game in Nakagawa yet. It may not start.";
        return "Untested";
    }
    switch (game->status) {
        case NK_STATUS_BOOTS:
            *expect = "Starts, then stops at a part Nakagawa does not support yet.";
            return "Boots to a named boundary";
        case NK_STATUS_PLAYABLE:
            *expect = "Plays, but some parts may not work yet.";
            return "Partially works";
        case NK_STATUS_UNIDENTIFIED:
        case NK_STATUS_IDENTIFIED: return "Untested";
        case NK_STATUS_SUPPORTED_PREPARATION:
        case NK_STATUS_PREPARED:
        case NK_STATUS_VERIFIED:
        default:
            *expect = "This game does not run in Nakagawa yet.";
            return "Not started";
    }
}

static const char *player_ui_compatibility(const GameRecord *game, const char **expect,
                                           const char **known_issues, PlayerUiTier *tier) {
    PlayerUiTier record_tier;
    const char *category =
        player_ui_compatibility_words(game, expect, known_issues, &record_tier);
    *tier = record_tier != PLAYER_UI_TIER_NONE ? record_tier
                                               : player_ui_tier_for_category(category);
    return category;
}

#ifdef NK_PLAYER_UI_REGRESSION_TEST
void player_ui_nk_test_report_compatibility(void) {
    for (size_t i = 0u;
         i < sizeof(s_compatibility_records) / sizeof(s_compatibility_records[0]);
         ++i) {
        const PlayerUiCompatibilityRecord *record = &s_compatibility_records[i];
        const char *tier = player_ui_tier_word(player_ui_record_tier(record));
        const char *category_tier =
            player_ui_tier_word(player_ui_tier_for_category(record->category));
        char key[96];
        snprintf(key, sizeof(key), "%s", record->key);
        for (char *q = key; *q; q++) if (*q == ' ') *q = '_';
        printf("[PLAYER_UI_TEST] compatibility key=%s category_tier=%s tier=%s\n", key,
               category_tier ? category_tier : "none", tier ? tier : "none");
    }
}
#endif

static bool player_ui_has_seen_boot(const GameRecord *game) {
    return game != NULL && (game->status == NK_STATUS_BOOTS ||
                            game->status == NK_STATUS_PLAYABLE);
}

/* An experimental entry nobody has recorded a result for. A disc with a
   compatibility record (a catalogued game whose title settings are local,
   such as Hot Shots Tennis) is not called Untested beside its PLAYABLE. */
static bool player_ui_unrecorded_experimental(const GameRecord *game) {
    const char *expect;
    const char *known_issues;
    PlayerUiTier record_tier;
    if (game == NULL || !game->is_experimental) return false;
    (void)player_ui_compatibility_words(game, &expect, &known_issues, &record_tier);
    return record_tier == PLAYER_UI_TIER_NONE;
}

static const char *player_ui_library_status_word(const GameRecord *game,
                                                 bool package_ready) {
    if (package_ready) return "Ready";
    if (player_ui_unrecorded_experimental(game)) {
        return player_ui_has_seen_boot(game) ? "Experimental" : "Untested";
    }
    return "Needs setup";
}

/* Status colours follow the design system: Ready teal, experimental and
   untested info blue (with their own shapes), needs setup warning gold. */
static uint32_t player_ui_library_status_color(const GameRecord *game,
                                              bool package_ready) {
    if (package_ready) return PLAYER_UI_READY;
    if (player_ui_unrecorded_experimental(game)) return PLAYER_UI_INFO;
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
    if (player_ui_unrecorded_experimental(game)) {
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
    bool starting;
    bool running;
    const PlayerPreflightCheck *build_blocker;
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
    /* This title is the game being started or the one that runs now. A game
       whose window is not up yet is still starting. */
    starting = game->disc_id[0] != '\0' &&
        ((app->launch_pending &&
          strcmp(app->launch_pending_disc_id, game->disc_id) == 0) ||
         (app->is_game_running && !app->child_window_ready &&
          strcmp(app->launch_session.disc_id, game->disc_id) == 0));
    running = !starting && game->disc_id[0] != '\0' && app->is_game_running &&
              strcmp(app->launch_session.disc_id, game->disc_id) == 0;
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
    /* The package card's text follows the package, not the primary action
       (#1297): a ready package stays ready while its game runs. */
    if (starting) {
        out->package_state = PLAYER_UI_PACKAGE_STARTING;
    } else if (running) {
        out->package_state = PLAYER_UI_PACKAGE_RUNNING;
    } else if (app->build_session.is_building &&
               strcmp(app->build_session.disc_id, game->disc_id) == 0) {
        out->package_state = PLAYER_UI_PACKAGE_BUILDING;
    } else if (package_ready) {
        out->package_state = PLAYER_UI_PACKAGE_READY;
    } else if (checking || package_status == NK_RUNTIME_PACKAGE_UNKNOWN) {
        out->package_state = PLAYER_UI_PACKAGE_CHECKING;
    } else if (check_failed) {
        out->package_state = PLAYER_UI_PACKAGE_CHECK_FAILED;
    } else {
        out->package_state = PLAYER_UI_PACKAGE_MISSING;
    }
    build_blocker = (package_ready || checking || check_failed || starting || running)
        ? NULL : player_app_package_build_preflight_blocker(app, index);
    out->build_preflight_message = build_blocker
        ? player_preflight_plain_message(build_blocker) : "";
    out->build_preflight_folder = build_blocker
        ? app->package_build_preflight.action_folder : "";
    out->compatibility_category = player_ui_compatibility(
        game, &out->compatibility_expect, &out->compatibility_known_issues,
        &out->compatibility_tier);
    if (!app->settings.developer_mode) out->compatibility_category = NULL;
    /* The disc's executable is encrypted: the player decrypts it from the
       disc (#295). The title screen shows this only while the package still
       needs it. */
    out->input_note =
        game->executable_eboot_kind == NK_ISO_EXEC_PSP_ENCRYPTED &&
        game->executable_selection == NK_ISO_EXEC_SELECTION_NONE
            ? "Nakagawa decrypts this game's program from your own disc, on this PC."
            : "";
    out->status_rgba = player_ui_library_status_color(game, package_ready);
    out->status_marker = player_ui_library_status_marker(game, package_ready);
    if (starting) {
        out->library_status = "Starting";
        out->status_rgba = PLAYER_UI_INFO;
        out->status_marker = "◆";
    } else if (out->package_state == PLAYER_UI_PACKAGE_CHECKING &&
               !player_ui_unrecorded_experimental(game)) {
        /* Launcher review B4: until the background check has looked at the
           package, the card does not know it needs setup. */
        out->library_status = "Checking";
        out->status_rgba = PLAYER_UI_INFO;
        out->status_marker = "◆";
    } else if (running) {
        out->library_status = "Running";
        out->status_rgba = PLAYER_UI_READY;
        out->status_marker = "●";
    }
    out->art_kind = player_ui_art_kind(game);
    /* The showcase packages beside the player are the demos under TEST THIS
       PC. The --demo fixtures are the project's test programs, not a disc the
       user added. */
    out->is_demo = game->is_showcase_package;
    out->badge = out->is_demo || out->art_kind != NK_UI_ART_NONE ? "Showcase demo"
               : game->is_sample ? "Test program" : "Your disc";
    out->icon = NULL;
    out->picture = NULL;
    if (out->art_kind == NK_UI_ART_NONE) {
        player_ui_art_get(player->art, game->iso_path, false, &out->icon, NULL);
    }
#ifdef NK_PLAYER_UI_REGRESSION_TEST
    if (strcmp(player->status_reported[index], out->library_status) != 0) {
        snprintf(player->status_reported[index], sizeof(player->status_reported[index]), "%s",
                 out->library_status);
        printf("[PLAYER_UI_TEST] next_ui_library_status index=%d status=%s\n", index,
               out->library_status);
    }
    if (out->icon != NULL && !player->icon_reported[index]) {
        player->icon_reported[index] = true;
        printf("[PLAYER_UI_TEST] next_ui_disc_icon index=%d width=%u height=%u\n", index,
               (unsigned)out->icon->width, (unsigned)out->icon->height);
    }
#endif
    out->save_folder_enabled = game->disc_id[0] != '\0';
    out->can_remove = player_app_removal_blocker(app, index) == NULL;
    out->primary_action_enabled = false;
    out->primary_action = PLAYER_UI_PRIMARY_NONE;
    if (app->launch_pending) {
        /* The launch worker hands back STOP GAME or an error card. */
        out->primary_action_label = "STARTING...";
    } else if (app->is_game_running) {
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
        if (!build_blocker) {
            out->primary_action_enabled = true;
            primary = PLAYER_UI_PRIMARY_BUILD;
        }
    } else if (package_status == NK_RUNTIME_PACKAGE_MISSING) {
        out->primary_action_label = "BUILD PACKAGE";
        if (!build_blocker) {
            out->primary_action_enabled = true;
            primary = PLAYER_UI_PRIMARY_BUILD;
        }
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
    /* The launcher's screens are laid out for at least its design size
       (1280 x 720); a smaller window, down to the 960 x 540 minimum, draws the
       same layout smaller instead of pushing buttons off the window. */
    player->viewport.min_logical_w = (float)PLAYER_UI_DESIGN_WIDTH;
    player->viewport.min_logical_h = (float)PLAYER_UI_DESIGN_HEIGHT;
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
    {
        /* The owner's first run: one click on COPY FILES AND CONTINUE swapped
           in the copy screen, whose CANCEL sits where the button was, and the
           rest of the click cancelled the copy. A click that began on another
           screen activates nothing on this one. */
        unsigned screen = player_ui_flow_screen(app) * 16u +
                          (unsigned)nk_ui_router_current(player->router);
        if (screen != player->press_screen) {
            nk_ui_cancel_press(player->ui);
            player->press_screen = screen;
        }
    }
    nk_ui_begin(player->ui, now_s);
    {
        NkUiId restore = nk_ui_router_take_focus_restore(player->router);
        if (restore != 0u) nk_ui_focus(player->ui, restore);
    }
    route = nk_ui_router_current(player->router);
    player_ui_flow_focus(player, app);
    if (player_ui_nk_flow_view(app->active_view)) {
        action = player_ui_render_flow(player, app, scale, now_s);
        if ((player_ui_settings_view(app->active_view) ||
             app->active_view == VIEW_CONTROLLER_SETTINGS) &&
            (action.kind == PLAYER_UI_ACTION_OPEN_LIBRARY ||
             action.kind == PLAYER_UI_ACTION_OPEN_ABOUT ||
             action.kind == PLAYER_UI_ACTION_OPEN_SETTINGS)) {
            /* A top-bar section closes Settings and opens that screen; the
               SETTINGS tab and Start close it, as Done does. */
            if (action.kind == PLAYER_UI_ACTION_OPEN_ABOUT) {
                if (route != PLAYER_UI_ROUTE_ABOUT) {
                    nk_ui_router_set_focus(player->router, PLAYER_UI_ID_TAB_ABOUT);
                    (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_ABOUT);
                }
                player->pre_flow_focus = PLAYER_UI_ID_TAB_ABOUT;
            } else if (action.kind == PLAYER_UI_ACTION_OPEN_LIBRARY) {
                (void)nk_ui_router_replace(player->router, PLAYER_UI_ROUTE_LIBRARY);
                player->pre_flow_focus = PLAYER_UI_ID_TAB_LIBRARY;
            }
            if (app->active_view == VIEW_CONTROLLER_SETTINGS) {
                action.kind = PLAYER_UI_ACTION_CONTROLLER;
                action.primary = (PlayerUiPrimaryAction)PLAYER_UI_CONTROLLER_CLOSE;
            } else {
                action.kind = PLAYER_UI_ACTION_SETTINGS;
                action.primary = (PlayerUiPrimaryAction)PLAYER_UI_SETTINGS_DONE;
            }
        }
        if (action.kind != PLAYER_UI_ACTION_NONE && out_action != NULL) {
            *out_action = action;
        }
    } else if (route == PLAYER_UI_ROUTE_LIBRARY) {
        size_t count = app->game_count > 0 ? (size_t)app->game_count : 0u;
        size_t columns = 1u;
        size_t page_size;
        size_t demo_count;
        size_t games;
        size_t slots;
        if (count > PLAYER_UI_TITLE_CAP) count = PLAYER_UI_TITLE_CAP;
        player_ui_fill_titles(player, app, count);
        /* The grid holds the user's games and ends with the add card, so it has
           one slot more than games; the demos sit under it, under TEST THIS PC. */
        demo_count = player_ui_library_demo_count(player->titles, count);
        games = count - demo_count;
        page_size = player_ui_library_page_size_with_demos(scale.logical_w, scale.logical_h,
                                                           demo_count, &columns);
        slots = games > 0u ? games + 1u : 0u;
        {
            size_t focused_index = player_ui_focused_title_index(
                nk_ui_focused(player->ui), count);
            if (focused_index < count) {
                size_t position = player_ui_library_grid_position(player->titles, count,
                                                                  focused_index);
                app->selected_game_index = (int)focused_index;
                if (position < count) player->library_page = position / page_size;
            }
        }
        if (nk_ui_focused(player->ui) == PLAYER_UI_ID_LIBRARY_ADD && slots > 0u) {
            player->library_page = (slots - 1u) / page_size;
        }
        if (player->library_focus_pending) {
            size_t position = player_ui_library_grid_position(
                player->titles, count, player->library_focus_index);
            player->library_focus_pending = false;
            if (position < count) {
                nk_ui_focus(player->ui, PLAYER_UI_ID_LIBRARY_ITEM(player->library_focus_index));
                app->selected_game_index = (int)player->library_focus_index;
                player->library_page = position / page_size;
            } else if (games > 0u) {
                nk_ui_focus(player->ui, PLAYER_UI_ID_LIBRARY_ADD);
                player->library_page = (slots - 1u) / page_size;
            } else {
                nk_ui_focus(player->ui, PLAYER_UI_ID_LIBRARY_EMPTY_ADD);
            }
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
        if (app->update.notice) {
            snprintf(player->update_notice, sizeof(player->update_notice),
                     "Version %.40s is out", app->update.check.newest.version);
            model.update_notice = player->update_notice;
        }
        action = player_ui_screen_library(player->ui, &model);
        if (action.kind == PLAYER_UI_ACTION_OPEN_TITLE && action.title_index < count) {
            app->selected_game_index = (int)action.title_index;
            nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
            (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_TITLE);
            player->title_primary_refocus = false;
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
            const GameRecord *game = &app->games[index];
            if (!app->launch_pending && !app->is_game_running &&
                !player_app_cached_game_has_runtime(app, game) &&
                !player_app_runtime_package_check_pending(app, game) &&
                !player_app_runtime_package_check_failed(app, game)) {
                (void)player_app_build_package_preflight(app, index, false);
            }
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
            model.stalled_game_notice_visible =
                player->titles[index].package_state == PLAYER_UI_PACKAGE_RUNNING &&
                app->stalled_game_notice_visible;
            model.stalled_game_dark_picture = app->stalled_game_dark_picture;
            model.now_s = app->settings.reduce_motion ? 0.0 : now_s;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            {
                /* Keep PLAY or BUILD PACKAGE focused across a game's start
                   and exit: while it is off (STARTING..., STOP GAME is not
                   offered as a default) focus waits on the fallback, and
                   comes back when it is on again unless the user moved. */
                const PlayerUiTitle *shown = &player->titles[index];
                bool primary_default = shown->primary_action_enabled &&
                    (shown->primary_action == PLAYER_UI_PRIMARY_PLAY ||
                     shown->primary_action == PLAYER_UI_PRIMARY_BUILD);
                NkUiId focused = nk_ui_focused(player->ui);
                if (!primary_default) {
                    if (focused == PLAYER_UI_ID_TITLE_PRIMARY) {
                        player->title_primary_refocus = true;
                        player->title_primary_fallback = 0u;
                    } else if (player->title_primary_refocus &&
                               player->title_primary_fallback == 0u) {
                        player->title_primary_fallback = focused;
                    } else if (player->title_primary_refocus &&
                               focused != player->title_primary_fallback) {
                        player->title_primary_refocus = false;
                    }
                } else if (player->title_primary_refocus) {
                    /* No fallback recorded yet means the action was off for a
                       single frame: focus is where the fallback put it. */
                    if (player->title_primary_fallback == 0u ||
                        focused == player->title_primary_fallback ||
                        focused == PLAYER_UI_ID_TITLE_PRIMARY) {
                        nk_ui_focus(player->ui, PLAYER_UI_ID_TITLE_PRIMARY);
                    }
                    player->title_primary_refocus = false;
                }
            }
            action = player_ui_screen_title(player->ui, &model);
            if (action.kind == PLAYER_UI_ACTION_BACK) {
                nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_REMOVE_TITLE) {
                if (player_ui_open_remove(player, app, index)) {
                    nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                    (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_REMOVE);
                }
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_PRIMARY &&
                       action.primary == PLAYER_UI_PRIMARY_STOP_GAME) {
                /* Stopping loses what was not saved, so it asks first, as
                   quitting with a game running does. */
                player->stop_index = index;
                snprintf(player->stop_disc, sizeof(player->stop_disc), "%s",
                         app->games[index].disc_id);
                player->stop_focus_pending = true;
                nk_ui_router_set_focus(player->router, nk_ui_focused(player->ui));
                (void)nk_ui_router_push(player->router, PLAYER_UI_ROUTE_STOP);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_PRIMARY ||
                       action.kind == PLAYER_UI_ACTION_OPEN_SAVE_FOLDER) {
                if (out_action != NULL) *out_action = action;
            }
        }
    } else if (route == PLAYER_UI_ROUTE_STOP) {
        int index = player->stop_index;
        /* The question goes when there is nothing left to stop: the game
           ended by itself, or its title left the library. */
        if (!app->is_game_running || index < 0 || index >= app->game_count ||
            strcmp(app->games[index].disc_id, player->stop_disc) != 0) {
            (void)nk_ui_router_pop(player->router);
        } else {
            PlayerUiStopModel model;
            if (player->stop_focus_pending) {
                nk_ui_focus(player->ui, PLAYER_UI_ID_STOP_KEEP);
                player->stop_focus_pending = false;
            }
            memset(&model, 0, sizeof(model));
            /* STOP GAME stops the game that runs, which may not be this page's. */
            model.title = app->launch_session.game_title[0] != '\0'
                ? app->launch_session.game_title : app->games[index].title_name;
            model.title_index = (size_t)index;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            action = player_ui_screen_stop(player->ui, &model);
            if (action.kind == PLAYER_UI_ACTION_BACK) {
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_PRIMARY) {
                if (out_action != NULL) *out_action = action;
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            }
        }
    } else if (route == PLAYER_UI_ROUTE_REMOVE) {
        int index = player->remove_index;
        /* The dialog closes if its title left the library or can no longer
           be removed (a build or a launch started under it). */
        if (index < 0 || index >= app->game_count ||
            strcmp(app->games[index].disc_id, player->remove_disc) != 0 ||
            player_app_removal_blocker(app, index) != NULL) {
            (void)nk_ui_router_pop(player->router);
        } else {
            PlayerUiRemoveModel model;
            if (player->remove_focus_pending) {
                nk_ui_focus(player->ui, PLAYER_UI_ID_REMOVE_CANCEL);
                player->remove_focus_pending = false;
            }
            memset(&model, 0, sizeof(model));
            model.title = app->games[index].title_name;
            model.disc_id = app->games[index].disc_id;
            model.title_index = (size_t)index;
            model.items = player->remove_items;
            model.item_count = player->remove_item_count;
            model.has_saves = player->remove_plan.has_saves;
            model.saves_bytes = player->remove_plan.saves_bytes;
            model.delete_saves = player->remove_delete_saves;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            action = player_ui_screen_remove(player->ui, &model);
            if (action.kind == PLAYER_UI_ACTION_BACK) {
                (void)nk_ui_router_pop(player->router);
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_REMOVE_TOGGLE_SAVES) {
                player->remove_delete_saves = !player->remove_delete_saves;
                action.kind = PLAYER_UI_ACTION_NONE;
            } else if (action.kind == PLAYER_UI_ACTION_REMOVE_CONFIRM) {
                /* The removal runs after this frame; the library then
                   focuses the tile that takes this one's place. */
                if (out_action != NULL) *out_action = action;
                while (nk_ui_router_depth(player->router) > 1u) {
                    (void)nk_ui_router_pop(player->router);
                }
                player->library_focus_pending = true;
                player->library_focus_index = (size_t)index;
                action.kind = PLAYER_UI_ACTION_NONE;
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
