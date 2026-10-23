/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"

#include "player_state.h"
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
    char version_line[64];
    bool ready;
};

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
    nk_ui_sdl_input_init(&player->input);
    player_ui_version_line(NK_PLAYER_VERSION, (long long)NK_PLAYER_BUILD_EPOCH,
                           player->version_line, sizeof(player->version_line));
    player->ready = true;
    if (error != NULL && error_size > 0u) error[0] = '\0';
    return player;
}

void player_ui_nk_destroy(PlayerUiNk *player) {
    if (player == NULL) return;
    nk_ui_sdlr_destroy(player->backend);
    nk_ui_destroy(player->ui);
    nk_ui_ttf_destroy(player->ttf);
    nk_ui_router_destroy(player->router);
    free(player);
}

bool player_ui_nk_view_active(const PlayerUiNk *player, const PlayerApp *app) {
    return player != NULL && player->ready && app != NULL &&
           (app->active_view == VIEW_LIBRARY ||
            app->active_view == PLAYER_VIEW_READY_LIBRARY);
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
        if (translated[i].type == NK_UI_EV_BACK &&
            nk_ui_router_current(player->router) == PLAYER_UI_ROUTE_ABOUT) {
            /* About is a top-level section: Back returns to the Library. */
            (void)nk_ui_router_replace(player->router, PLAYER_UI_ROUTE_LIBRARY);
            nk_ui_router_set_focus(player->router, PLAYER_UI_ID_TAB_ABOUT);
        } else if (translated[i].type == NK_UI_EV_BACK) {
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
        out->primary_action_label = "PACKAGE INCOMPATIBLE";
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
    nk_ui_begin(player->ui, now_s);
    {
        NkUiId restore = nk_ui_router_take_focus_restore(player->router);
        if (restore != 0u) nk_ui_focus(player->ui, restore);
    }
    route = nk_ui_router_current(player->router);
    if (route == PLAYER_UI_ROUTE_LIBRARY) {
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
        (void)nk_ui_router_replace(player->router, PLAYER_UI_ROUTE_ABOUT);
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
    (void)SDL_SetRenderDrawColor(renderer, 11u, 15u, 20u, 255u);
    (void)SDL_RenderClear(renderer);
    if (!nk_ui_sdlr_render(player->backend, draw_list)) return false;
    SDL_RenderPresent(renderer);
    if (out_action != NULL && action.kind != PLAYER_UI_ACTION_NONE) {
        *out_action = action;
    }
    return true;
}
