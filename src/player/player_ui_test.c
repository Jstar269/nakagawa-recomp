/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The UI regression tests' scripted-event harness (player_ui_test.h). It moved
 * here from main.c unchanged; only the UI-test player build compiles it. */

#include "player_ui_test.h"

#ifdef NK_PLAYER_UI_REGRESSION_TEST

#include "package_builder.h"
#include "ui_renderer.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned g_ui_test_package_status_thread_create_attempts;
const PlayerUiNk *g_ui_test_next_ui;
unsigned g_ui_test_fail_package_status_thread_create_on_attempts[
    UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURES];
unsigned g_ui_test_fail_package_status_thread_create_count;
static char s_ui_test_fail_package_status_thread_create_for[
    UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURE_TITLES][MAX_DISC_ID_LEN];
unsigned
    g_ui_test_fail_package_status_thread_create_for_counts[
        UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURE_TITLES];
static unsigned s_ui_test_fail_package_status_thread_create_for_count;
bool g_ui_test_waiting_for_package_failure;
char g_ui_test_wait_package_failure_disc[MAX_DISC_ID_LEN];
unsigned g_ui_test_wait_package_failure_target;

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

bool player_ui_test_parse_positive_u64(const char *text, uint64_t *value) {
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

bool player_ui_test_parse_fail_attempts(const char *text) {
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
        g_ui_test_fail_package_status_thread_create_on_attempts[count++] =
            (unsigned)attempt;
        item = next;
    }
    g_ui_test_fail_package_status_thread_create_count = count;
    return count > 0;
}

bool player_ui_test_parse_fail_titles(const char *text) {
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

bool player_ui_test_fail_thread_create_for_game(
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

int player_ui_test_fail_title_index(const char *disc_id) {
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

int player_ui_test_next_event(const char *script, size_t *cursor,
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
        snprintf(g_ui_test_wait_package_failure_disc,
                 sizeof(g_ui_test_wait_package_failure_disc),
                 "%s", token + 21);
        g_ui_test_wait_package_failure_target = (unsigned)target;
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

bool player_ui_test_copy_file(const char *source, const char *destination) {
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

bool player_ui_test_start_synthetic_build(PlayerApp *app,
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

const char *player_ui_test_view_name(PlayerView view) {
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

void player_ui_test_report_frame(int frame_number, const PlayerApp *app,
                                        SDL_Renderer *renderer, bool running,
                                        uint64_t render_elapsed_ns,
                                        uint64_t validation_calls,
                                        const char *invalid_disc_message) {
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
            ? player_ui_nk_test_title(g_ui_test_next_ui, (size_t)app->selected_game_index)
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
        } else if (selected != NULL && g_ui_test_next_ui == NULL) {
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
         strcmp(app->wizard.status_message, invalid_disc_message) == 0) ||
        (app->active_view == VIEW_ERROR &&
         strcmp(app->last_error.message, invalid_disc_message) == 0);
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
           "next_package=%d next_expect=%s launch_pending=%d next_focus=%u\n",
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
           g_ui_test_package_status_thread_create_attempts,
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
           g_ui_test_next_ui != NULL ? (player_ui_nk_at_library_root(g_ui_test_next_ui) ? 1 : 0) : -1,
           g_ui_test_next_ui != NULL ? player_ui_nk_test_route(g_ui_test_next_ui) : -1,
           error_title, error_badge, error_retry,
           app->last_error.details_copied ? 1 : 0,
           next_package, next_expect, app->launch_pending ? 1 : 0,
           g_ui_test_next_ui != NULL ? player_ui_nk_test_focus(g_ui_test_next_ui) : 0u);

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
#endif /* NK_PLAYER_UI_REGRESSION_TEST */
