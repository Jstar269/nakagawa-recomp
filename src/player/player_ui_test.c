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
        { "confirm_factory_reset", VIEW_CONFIRM_FACTORY_RESET },
        { "accessibility_settings", VIEW_ACCESSIBILITY_SETTINGS },
        { "ace_settings", VIEW_ACE_SETTINGS }
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
    if (strcmp(token, "MOUSE_DOWN") == 0 || strcmp(token, "MOUSE_UP") == 0 ||
        strcmp(token, "MOUSE_X1_DOWN") == 0 || strcmp(token, "MOUSE_X1_UP") == 0) {
        /* As SDL reports a real click: at the pointer, with its button state. */
        event->type = strcmp(token, "MOUSE_DOWN") == 0 || strcmp(token, "MOUSE_X1_DOWN") == 0
            ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event->button.button = strncmp(token, "MOUSE_X1", 8) == 0 ? SDL_BUTTON_X1
                                                                  : SDL_BUTTON_LEFT;
        event->button.down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event->button.clicks = 1u;
        event->button.x = s_ui_test_mouse_x;
        event->button.y = s_ui_test_mouse_y;
        return 1;
    }
    if (strncmp(token, "TEXT=", 5) == 0) {
        /* Typed text, as SDL reports it while text input is on. */
        static char text[64];
        (void)snprintf(text, sizeof(text), "%s", token + 5);
        event->type = SDL_EVENT_TEXT_INPUT;
        event->text.text = text;
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
            { "BACKSPACE", SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE },
            { "D", SDLK_D, SDL_SCANCODE_D }, { "SLASH", SDLK_SLASH, SDL_SCANCODE_SLASH },
            { "F12", SDLK_F12, SDL_SCANCODE_F12 }, { "W", SDLK_W, SDL_SCANCODE_W }
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
            { "WEST", SDL_GAMEPAD_BUTTON_WEST },
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
    case VIEW_ACCESSIBILITY_SETTINGS: return "accessibility_settings";
    case VIEW_ACE_SETTINGS: return "ace_settings";
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
    /* The selected title's program input from its last build check (#308). */
    const PlayerInputReport *selected_input = selected != NULL
        ? player_app_package_build_input(app, app->selected_game_index) : NULL;
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
    bool backdrop_shown = false;
    int backdrop_index = g_ui_test_next_ui != NULL
        ? player_ui_nk_test_backdrop(g_ui_test_next_ui, &backdrop_shown) : -1;
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
           "rebuild_queue_paused=%d prompt_version=%s "
           "input_class=%s input_files=%zu next_input_class=%s next_input_files=%zu "
           "backdrop=%d backdrop_shown=%d disc_check=%d\n",
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
           app->input_settings.note[0] ? 1 : 0,
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
           g_ui_test_next_ui != NULL ? player_ui_nk_test_focus(g_ui_test_next_ui) : 0u,
           player_app_outdated_count(app), app->outdated.scanned ? 1 : 0,
           player_app_rebuild_prompt_visible(app) ? 1 : 0,
           player_app_rebuild_banner_visible(app) ? 1 : 0,
           player_app_rebuild_reminder_visible(app) ? 1 : 0,
           app->rebuild_queue.count, app->rebuild_queue.active ? 1 : 0,
           app->rebuild_queue.paused ? 1 : 0,
           app->settings.rebuild_prompt_version[0] ? app->settings.rebuild_prompt_version
                                                   : "none",
           player_input_class_name(app->wizard.preflight.input.input_class),
           app->wizard.preflight.input.files_needed,
           selected_input ? player_input_class_name(selected_input->input_class) : "none",
           selected_input ? selected_input->files_needed : (size_t)0,
           backdrop_index, backdrop_shown ? 1 : 0, app->disc_check.running ? 1 : 0);

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

/* ---- The seam main.c calls (player_ui_test.h). Each moved here from main.c unchanged. ---- */

#include "nk_platform.h"
#include "nk_title_manifest.h"
#include "update_check.h"

/* The cache-epoch build regression deliberately defers the automatic rescan
   for one invalidation so an old post-build latch cannot be completed by a
   background job before the restarted synthetic build owns the queue. */
static bool s_ui_test_defer_cache_rescan_once;
static bool s_ui_test_drop_worker_event;
static const char *s_ui_test_catalog_reload_path;
static unsigned s_ui_test_catalog_reload_count;
static unsigned s_ui_test_catalog_reload_completed;
static bool s_ui_test_catalog_reload_started;
static bool s_ui_test_catalog_reload_failed;

void player_ui_test_init(PlayerUiTest *t) {
    memset(t, 0, sizeof(*t));
    t->events = "";
    t->wait_package_status_target = NK_RUNTIME_PACKAGE_UNKNOWN;
    t->wait_view_mask = UINT32_C(1) << VIEW_LIBRARY;
}

bool player_ui_test_parse_arg(PlayerUiTest *t, const char *arg) {
    if (strncmp(arg, "--ui-test-events=", 17) == 0) {
        t->events = arg + 17;
        t->mode = true;
    } else if (strncmp(arg, "--ui-test-screenshot=", 21) == 0) {
        t->screenshot_path = arg + 21;
        t->mode = true;
    } else if (strncmp(arg, "--ui-test-error-code=", 21) == 0) {
        t->error_code = arg + 21;
        t->mode = true;
    } else if (strncmp(arg, "--ui-test-iso-path=", 19) == 0) {
        t->iso_path = arg + 19;
        t->mode = true;
    } else if (strncmp(arg, "--ui-test-art-mount-source=", 27) == 0) {
        t->art_mount_source = arg + 27;
        t->mode = true;
    } else if (strcmp(arg, "--ui-test-wait-background") == 0) {
        t->wait_background = true;
        t->mode = true;
    } else if (strncmp(arg, "--ui-test-catalog-reload-path=", 30) == 0) {
        s_ui_test_catalog_reload_path = arg + 30;
        t->mode = true;
    } else if (strncmp(arg, "--ui-test-catalog-reload-count=", 31) == 0) {
        uint64_t count = 0;
        if (!player_ui_test_parse_positive_u64(arg + 31, &count) || count > 1024) {
            fprintf(stderr, "[PLAYER_UI_TEST] invalid catalog reload count.\n");
            t->failed = true;
        } else {
            s_ui_test_catalog_reload_count = (unsigned)count;
        }
        t->mode = true;
    } else {
        return false;
    }
    return true;
}

void player_ui_test_check_environment(PlayerUiTest *t) {
    {
        const char *fake_running = getenv("NK_UI_TEST_GAME_RUNNING");
        t->fake_game_running = t->mode && fake_running && fake_running[0];
    }
    const char *fail_package_thread_at =
        getenv("NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT");
    if (t->mode && fail_package_thread_at) {
        if (!player_ui_test_parse_fail_attempts(fail_package_thread_at)) {
            fprintf(stderr,
                    "[PLAYER_UI_TEST] invalid package status thread fail attempt.\n");
            t->failed = true;
        }
    }
    const char *fail_package_thread_for =
        getenv("NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_FOR");
    if (t->mode && fail_package_thread_for &&
        !player_ui_test_parse_fail_titles(fail_package_thread_for)) {
        fprintf(stderr,
                "[PLAYER_UI_TEST] invalid package status thread fail title.\n");
        t->failed = true;
    }
    if (t->mode && getenv("NK_UI_TEST_DROP_WORKER_EVENT")) {
        /* Model SDL refusing a worker completion push, so the launcher's
           reclaim path is exercised by the same harness that drives the real
           event loop. */
        s_ui_test_drop_worker_event = true;
        ui_renderer_test_drop_art_events(true);
    }
    if ((s_ui_test_catalog_reload_count > 0) !=
        (s_ui_test_catalog_reload_path &&
         s_ui_test_catalog_reload_path[0])) {
        fprintf(stderr, "[PLAYER_UI_TEST] catalog reload path/count must be paired.\n");
        t->failed = true;
    }
    if (t->mode) setvbuf(stdout, NULL, _IONBF, 0);
}

/* The UI test points a card at its synthetic image, and the card's own library
   record with it: the library tells two images of a disc ID apart by their
   files, so a card and its record must name the same one. */
static void ui_test_set_card_image(PlayerApp *app, int index, const char *iso_path) {
    GameRecord *card = &app->games[index];
    for (int j = 0; j < app->library.count; j++) {
        NkGameEntry *record = &app->library.entries[j];
        if (strcmp(record->disc_id, card->disc_id) == 0 &&
            strcmp(record->iso_path, card->iso_path) == 0) {
            snprintf(record->iso_path, sizeof(record->iso_path), "%s", iso_path);
        }
    }
    snprintf(card->iso_path, sizeof(card->iso_path), "%s", iso_path);
}

void player_ui_test_prepare_app(PlayerUiTest *t, PlayerApp *app) {
    if (t->iso_path && app->selected_game_index >= 0 &&
        app->selected_game_index < app->game_count) {
        ui_test_set_card_image(app, app->selected_game_index, t->iso_path);
    }
    const char *bind_test_eboot = getenv("NK_UI_TEST_BIND_EBOOT");
    if (t->mode && t->iso_path && bind_test_eboot &&
        strcmp(bind_test_eboot, "1") == 0 &&
        app->selected_game_index >= 0 && app->selected_game_index < app->game_count) {
        GameRecord *game = &app->games[app->selected_game_index];
        snprintf(game->selected_executable, sizeof(game->selected_executable),
                 "EBOOT.BIN");
        game->executable_eboot_kind = NK_ISO_EXEC_MIPS_ELF32;
        game->executable_selection = NK_ISO_EXEC_SELECTION_EBOOT;
    }
    if (t->iso_path) {
        for (int i = 0; i < app->game_count; i++) {
            if (strcmp(app->games[i].disc_id, "TEST00006") == 0) {
                ui_test_set_card_image(app, i, t->iso_path);
            }
        }
    }
    if (t->fake_game_running) {
        /* "1": some game runs. A disc ID names the running game, whose window
           is up; "starting:<disc ID>" is that game before its window opens. */
        const char *fake = getenv("NK_UI_TEST_GAME_RUNNING");
        bool starting = strncmp(fake, "starting:", 9) == 0;
        const char *disc = starting ? fake + 9 : fake;
        app->is_game_running = true;
        if (strcmp(disc, "1") != 0) {
            snprintf(app->launch_session.disc_id, sizeof(app->launch_session.disc_id),
                     "%s", disc);
            /* A launch carries the disc's title, as nk_launch_prepare_session
               copies it from the library record. */
            int running_index = player_app_find_game_by_disc_id(app, disc);
            if (running_index >= 0) {
                snprintf(app->launch_session.game_title,
                         sizeof(app->launch_session.game_title), "%s",
                         app->games[running_index].title_name);
            }
            app->child_window_ready = !starting;
        }
    }
    if (t->error_code) {
        if (strcmp(t->error_code, "CLI_NOT_FOUND") == 0) {
            player_app_set_cli_not_found_error(app, "Return to Library",
                                               VIEW_LIBRARY);
        } else if (strcmp(t->error_code, "UI_TEST_LONG_ERROR") == 0) {
            static const char long_error_message[] =
                "Build setup could not continue because a required file was missing. "
                "Check the message, follow the suggested recovery steps, and try again. "
                "If the application files are incomplete, reinstall the app and keep "
                "its folder together. Return to the library after correcting the issue. "
                "This final sentence confirms the full message remains visible beyond "
                "the old four-line limit. Keep every recovery step on this card so the "
                "player can read the complete instruction before leaving this screen.";
            player_app_set_error(app, t->error_code,
                                 "Synthetic Long Error", long_error_message,
                                 "Return to Library", VIEW_LIBRARY);
        } else if (strcmp(t->error_code, "RUNTIME_SEMANTIC_BOUNDARY") == 0 ||
                   strcmp(t->error_code, "RUNTIME_PREMATURE_EXIT") == 0 ||
                   strcmp(t->error_code, "RUNTIME_ERROR_EXIT") == 0) {
            /* The cards a finished game session shows (#1282), from a synthetic
               stop of the first library game. */
            const char *disc = app->game_count > 0 ? app->games[0].disc_id : "";
            static const char stop_event[] =
                "BOOT_EVENT phase=stop reason=semantic-boundary "
                "boundary=unsupported-import-scenetadhocinit issue=308";
            player_dev_log_add(app, "runtime", "%s", stop_event);
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
            player_app_set_game_stop_error(app, t->error_code,
                                           "unsupported-import-scenetadhocinit", 308u,
                                           strcmp(t->error_code, "RUNTIME_ERROR_EXIT") == 0
                                               ? 3 : 9,
                                           strcmp(t->error_code,
                                                  "RUNTIME_PREMATURE_EXIT") == 0 ? 120u : 9000u,
                                           disc, run_log[0] ? run_log : NULL);
        } else if (strcmp(t->error_code, "PACKAGE_BUILD_REFUSED") == 0) {
            /* A preflight refusal (#1286): the build never started. */
            const char *disc = app->game_count > 0 ? app->games[0].disc_id : "TEST80001";
            package_builder_init_session(&app->build_session, disc, "Synthetic Package Fixture");
            player_app_set_build_error(app, "preflight",
                                       "ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and "
                                       "built-in decryption stopped: KEY_REQUIRED.",
                                       NULL);
        } else {
            player_app_set_error(app, t->error_code, t->error_code,
                                 "Synthetic UI regression error state.",
                                 "Return to Library", VIEW_LIBRARY);
        }
    }
}

void player_ui_test_apply_motion(const PlayerUiTest *t, PlayerApp *app) {
    /* Scripted runs cut motion so their frames are steady; NK_UI_TEST_MOTION keeps the
       settings' choice, for the tests of motion itself. */
    if (t->mode && getenv("NK_UI_TEST_MOTION") == NULL) {
        app->settings.reduce_motion_choice = PLAYER_SYSTEM_CHOICE_ON;
    }
}

void player_ui_test_apply_update_settings(PlayerApp *app) {
    if (getenv("NK_UI_TEST_CHECK_AT_START") != NULL) app->settings.check_updates_at_start = true;
}

void player_ui_test_set_next_ui(const PlayerUiNk *next_ui) {
    g_ui_test_next_ui = next_ui;
}

void player_ui_test_report_startup(const PlayerUiTest *t, const PlayerUiNk *next_ui) {
    if (next_ui != NULL && t->mode) {
        puts("[PLAYER_UI_TEST] UI_NEXT_ACTIVE");
    }
    if (t->mode && getenv("NK_UI_TEST_REPORT_COMPATIBILITY") != NULL) {
        player_ui_nk_test_report_compatibility();
    }
    if (t->mode && getenv("NK_UI_TEST_REPORT_VIEWS") != NULL) {
        /* Every PlayerView, VIEW_LIBRARY through the last, VIEW_ACE_SETTINGS. */
        for (int view = (int)VIEW_LIBRARY; view <= (int)VIEW_ACE_SETTINGS; ++view) {
            printf("[PLAYER_UI_TEST] view_coverage value=%d name=%s nk=%d\n", view,
                   player_ui_test_view_name((PlayerView)view),
                   player_ui_nk_draws_view(view) ? 1 : 0);
        }
    }
}

bool player_ui_test_create_target(PlayerUiTest *t, SDL_Renderer *renderer, int width,
                                  int height) {
    t->render_target = NULL;
    if (!t->mode) return true;
    t->render_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_TARGET, width, height);
    if (!t->render_target || !SDL_SetRenderTarget(renderer, t->render_target)) {
        fprintf(stderr, "[PLAYER_UI_TEST] Could not create SDL render target: %s\n",
                SDL_GetError());
        if (t->render_target) SDL_DestroyTexture(t->render_target);
        t->render_target = NULL;
        return false;
    }
    return true;
}

void player_ui_test_destroy_target(PlayerUiTest *t, SDL_Renderer *renderer) {
    if (t->render_target) {
        SDL_SetRenderTarget(renderer, NULL);
        SDL_DestroyTexture(t->render_target);
        t->render_target = NULL;
    }
}

void player_ui_test_begin_first_frame(PlayerUiTest *t) {
    t->first_frame_started_ns = t->mode ? SDL_GetTicksNS() : 0;
    t->first_frame_validation_count = t->mode
        ? player_app_ui_test_validation_calls() : 0;
}

void player_ui_test_end_first_frame(PlayerUiTest *t, const PlayerApp *app,
                                    SDL_Renderer *renderer, const char *invalid_disc_message) {
    t->drop_data = NULL;
    if (t->mode) {
        uint64_t validations = player_app_ui_test_validation_calls();
        validations = validations >= t->first_frame_validation_count
            ? validations - t->first_frame_validation_count : validations;
        player_ui_test_report_frame(
            0, app, renderer, true,
            SDL_GetTicksNS() - t->first_frame_started_ns, validations,
            invalid_disc_message);
    }
}

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

void player_ui_test_report_update_phase(const PlayerUiTest *t, const PlayerApp *app) {
    static PlayerUpdatePhase reported = PLAYER_UPDATE_IDLE;
    if (t->mode && app->update.phase != reported) {
        reported = app->update.phase;
        printf("[PLAYER_UI_TEST] update=%s error=%s notice=%d newest=%s zip=%s "
               "install=%s\n",
               player_update_phase_name(app->update.phase),
               app->update.error.code[0] ? app->update.error.code : "none",
               app->update.notice ? 1 : 0,
               app->update.check.newest.tag[0] ? app->update.check.newest.tag : "none",
               app->update.zip_path[0] ? app->update.zip_path : "none",
               app->update.install_dir[0] ? app->update.install_dir : "none");
        fflush(stdout);
    }
}

void player_ui_test_step_script(PlayerUiTest *t, PlayerApp *app, SDL_Renderer *renderer,
                                bool *running, bool background_pending) {
    if (!t->mode) return;
    uint64_t ui_test_now = SDL_GetTicks();
    bool ui_test_block_script = false;
    if (t->waiting_for_view) {
        uint32_t current_view_mask = (unsigned)app->active_view < 32
            ? UINT32_C(1) << (unsigned)app->active_view : 0;
        if (t->wait_view_mask & current_view_mask) {
            printf("[PLAYER_UI_TEST] wait_view actual=%s result=PASS\n",
                   player_ui_test_view_name(app->active_view));
            t->waiting_for_view = false;
        } else if (ui_test_now >= t->wait_deadline) {
            fprintf(stderr, "[PLAYER_UI_TEST] wait_view result=FAIL actual=%s\n",
                    player_ui_test_view_name(app->active_view));
            t->failed = true;
            t->quit_queued = true;
            *running = false;
        } else {
            ui_test_block_script = true;
        }
    }
    if (t->waiting_for_package_status) {
        const GameRecord *game = app->selected_game_index >= 0 &&
            app->selected_game_index < app->game_count
            ? &app->games[app->selected_game_index] : NULL;
        bool ready = game &&
            !player_app_runtime_package_check_pending(app, game) &&
            !player_app_runtime_package_check_failed(app, game) &&
            player_app_cached_runtime_package_status(app, game) ==
                t->wait_package_status_target;
        if (ready) {
            printf("[PLAYER_UI_TEST] wait_package_status result=PASS\n");
            t->waiting_for_package_status = false;
        } else if (ui_test_now >= t->wait_deadline) {
            fprintf(stderr,
                    "[PLAYER_UI_TEST] wait_package_status result=FAIL target=%d\n",
                    (int)t->wait_package_status_target);
            t->failed = true;
            t->quit_queued = true;
            *running = false;
        } else {
            ui_test_block_script = true;
        }
    }
    if (t->waiting_for_art_attempt) {
        unsigned completed = ui_renderer_test_art_attempt_count();
        if (completed >= t->wait_art_attempt_target) {
            printf("[PLAYER_UI_TEST] wait_art_attempt target=%u result=PASS\n",
                   t->wait_art_attempt_target);
            t->waiting_for_art_attempt = false;
        } else {
            ui_test_block_script = true;
        }
    }
    if (t->waiting_for_art_delay) {
        if (ui_renderer_test_art_delay_active()) {
            printf("[PLAYER_UI_TEST] wait_art_delay_active result=PASS\n");
            t->waiting_for_art_delay = false;
        } else if (ui_test_now >= t->wait_art_delay_deadline) {
            fprintf(stderr,
                    "[PLAYER_UI_TEST] wait_art_delay_active result=FAIL\n");
            t->failed = true;
            t->quit_queued = true;
            t->waiting_for_art_delay = false;
            *running = false;
        } else {
            ui_test_block_script = true;
        }
    }
    if (g_ui_test_waiting_for_package_failure) {
        int failure_index = player_ui_test_fail_title_index(
            g_ui_test_wait_package_failure_disc);
        unsigned completed = failure_index >= 0
            ? g_ui_test_fail_package_status_thread_create_for_counts[
                failure_index]
            : 0;
        if (completed >= g_ui_test_wait_package_failure_target) {
            printf("[PLAYER_UI_TEST] wait_package_failure disc=%s "
                   "target=%u result=PASS\n",
                   g_ui_test_wait_package_failure_disc,
                   g_ui_test_wait_package_failure_target);
            g_ui_test_waiting_for_package_failure = false;
        } else {
            ui_test_block_script = true;
        }
    }
    if (t->waiting_for_time) {
        if (ui_test_now >= t->wait_deadline) {
            printf("[PLAYER_UI_TEST] wait_ms result=PASS\n");
            t->waiting_for_time = false;
        } else {
            ui_test_block_script = true;
        }
    }
    if (!ui_test_block_script && !t->quit_queued && *running) {
    SDL_Event scripted_event;
    int next_event = player_ui_test_next_event(
        t->events, &t->event_cursor, &scripted_event);
    if (next_event > 0) {
        if (scripted_event.type == SDL_EVENT_USER &&
            scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_ART_ATTEMPT) {
            t->wait_art_attempt_target = scripted_event.user.windowID;
            t->waiting_for_art_attempt = true;
            printf("[PLAYER_UI_TEST] wait_art_attempt target=%u started\n",
                   t->wait_art_attempt_target);
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_ART_DELAY_ACTIVE) {
            t->waiting_for_art_delay = true;
            t->wait_art_delay_deadline = ui_test_now + 10000;
            printf("[PLAYER_UI_TEST] wait_art_delay_active started\n");
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_PACKAGE_FAILURE) {
            g_ui_test_waiting_for_package_failure = true;
            printf("[PLAYER_UI_TEST] wait_package_failure disc=%s "
                   "target=%u started\n",
                   g_ui_test_wait_package_failure_disc,
                   g_ui_test_wait_package_failure_target);
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_INVALIDATE_PACKAGE_CACHE) {
            player_app_runtime_package_cache_invalidate(app);
            printf("[PLAYER_UI_TEST] invalidate_package_cache generation=%llu\n",
                   (unsigned long long)app->runtime_package_cache_generation);
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_MOUNT_ART_ISO) {
            bool mounted = player_ui_test_copy_file(
                t->art_mount_source, t->iso_path);
            printf("[PLAYER_UI_TEST] mount_art_iso result=%s\n",
                   mounted ? "PASS" : "FAIL");
            if (!mounted) {
                t->failed = true;
                t->quit_queued = true;
                *running = false;
            }
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_SATURATE_ART_CACHE) {
            if (!ui_renderer_test_saturate_art_cache(renderer, t->iso_path)) {
                t->failed = true;
            }
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_START_SYNTHETIC_BUILD) {
            bool started = player_ui_test_start_synthetic_build(
                app, scripted_event.user.windowID);
            printf("[PLAYER_UI_TEST] synthetic_build result=%s index=%u disc=%s\n",
                   started ? "PASS" : "FAIL",
                   scripted_event.user.windowID,
                   started ? app->games[app->selected_game_index].disc_id : "NONE");
            if (!started) {
                t->failed = true;
                t->quit_queued = true;
                *running = false;
            }
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_INVALIDATE_AND_START_SYNTHETIC_BUILD) {
            player_app_runtime_package_cache_invalidate(app);
            s_ui_test_defer_cache_rescan_once = true;
            bool started = player_ui_test_start_synthetic_build(
                app, scripted_event.user.windowID);
            printf("[PLAYER_UI_TEST] invalidate_cache_and_synthetic_build result=%s index=%u disc=%s generation=%llu build_session_generation=%llu\n",
                   started ? "PASS" : "FAIL",
                   scripted_event.user.windowID,
                   started ? app->games[app->selected_game_index].disc_id : "NONE",
                   (unsigned long long)app->runtime_package_cache_generation,
                   (unsigned long long)app->build_session.session_generation);
            if (!started) {
                t->failed = true;
                t->quit_queued = true;
                *running = false;
            }
        } else if (scripted_event.type == SDL_EVENT_USER &&
            scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_MS) {
            t->wait_deadline = ui_test_now + scripted_event.user.windowID;
            t->waiting_for_time = true;
            printf("[PLAYER_UI_TEST] wait_ms=%u started\n",
                   scripted_event.user.windowID);
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_VIEW) {
            t->wait_view_mask = scripted_event.user.windowID;
            t->wait_deadline = ui_test_now + scripted_event.user.timestamp;
            t->waiting_for_view = true;
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_WAIT_PACKAGE_STATUS) {
            t->wait_package_status_target =
                (NkRuntimePackageStatus)scripted_event.user.windowID;
            t->wait_deadline = ui_test_now + scripted_event.user.timestamp;
            t->waiting_for_package_status = true;
            printf("[PLAYER_UI_TEST] wait_package_status target=%u started\n",
                   scripted_event.user.windowID);
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_ASSERT_CONSENT) {
            bool matches = app->active_view == VIEW_PREREQ_CONSENT &&
                app->prerequisites.items.count == scripted_event.user.windowID &&
                app->prerequisites.total_bytes == scripted_event.user.timestamp;
            printf("[PLAYER_UI_TEST] assert_consent result=%s items=%zu expected_items=%u "
                   "total_bytes=%llu expected_bytes=%llu\n",
                   matches ? "PASS" : "FAIL",
                   app->prerequisites.items.count, scripted_event.user.windowID,
                   (unsigned long long)app->prerequisites.total_bytes,
                   (unsigned long long)scripted_event.user.timestamp);
            if (!matches) {
                t->failed = true;
                t->quit_queued = true;
                *running = false;
            }
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_ASSERT_VIEW) {
            uint32_t current_view_mask = (unsigned)app->active_view < 32
                ? UINT32_C(1) << (unsigned)app->active_view : 0;
            bool matches = (scripted_event.user.windowID & current_view_mask) != 0;
            printf("[PLAYER_UI_TEST] assert_view result=%s actual=%s\n",
                   matches ? "PASS" : "FAIL",
                   player_ui_test_view_name(app->active_view));
            if (!matches) {
                t->failed = true;
                t->quit_queued = true;
                *running = false;
            }
        } else if (scripted_event.type == SDL_EVENT_USER &&
                   scripted_event.user.code == PLAYER_UI_TEST_ACTION_ASSERT_GAME_RUNNING) {
            printf("[PLAYER_UI_TEST] assert_game_running result=%s\n",
                   app->is_game_running ? "PASS" : "FAIL");
            if (!app->is_game_running) {
                t->failed = true;
                t->quit_queued = true;
                *running = false;
            }
        } else {
            if (scripted_event.type == SDL_EVENT_DROP_FILE) {
                t->drop_data = (char *)scripted_event.drop.data;
            }
            if (!SDL_PushEvent(&scripted_event)) {
                if (t->drop_data) {
                    SDL_free(t->drop_data);
                    t->drop_data = NULL;
                }
                t->failed = true;
                *running = false;
                fprintf(stderr, "[PLAYER_UI_TEST] Could not queue synthetic SDL event.\n");
            } else if (scripted_event.type == SDL_EVENT_QUIT) {
                t->quit_queued = true;
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
                    t->failed = true;
                    *running = false;
                    fprintf(stderr, "[PLAYER_UI_TEST] Could not queue synthetic SDL "
                                    "release event.\n");
                }
            }
        }
    } else if (next_event == 0) {
        if (!(t->wait_background &&
              (background_pending))) {
            memset(&scripted_event, 0, sizeof(scripted_event));
            scripted_event.type = SDL_EVENT_QUIT;
            if (!SDL_PushEvent(&scripted_event)) {
                t->failed = true;
                *running = false;
                fprintf(stderr, "[PLAYER_UI_TEST] Could not queue synthetic quit.\n");
            } else {
                t->quit_queued = true;
            }
        }
    } else {
        t->failed = true;
        *running = false;
        fprintf(stderr, "[PLAYER_UI_TEST] Invalid synthetic event script near byte %llu.\n",
                (unsigned long long)t->event_cursor);
    }
    }
}

void player_ui_test_event_handled(PlayerUiTest *t, const SDL_Event *event) {
    if (t->drop_data && event->type == SDL_EVENT_DROP_FILE &&
        event->drop.data == t->drop_data) {
        SDL_free(t->drop_data);
        t->drop_data = NULL;
    }
}

void player_ui_test_begin_frame(PlayerUiTest *t) {
    t->frame_started_ns = t->mode ? SDL_GetTicksNS() : 0;
    t->frame_validation_count = t->mode
        ? player_app_ui_test_validation_calls() : 0;
}

void player_ui_test_end_frame(PlayerUiTest *t, const PlayerApp *app, SDL_Renderer *renderer,
                              bool running, const char *invalid_disc_message) {
    if (t->mode) {
        t->frame++;
        uint64_t validations = player_app_ui_test_validation_calls();
        validations = validations >= t->frame_validation_count
            ? validations - t->frame_validation_count : validations;
        player_ui_test_report_frame(t->frame, app, renderer,
                                   running && !app->should_quit,
                                   SDL_GetTicksNS() - t->frame_started_ns,
                                   validations, invalid_disc_message);
        if (!running || app->should_quit) {
            bool captured = t->screenshot_path &&
                ui_capture_screenshot(renderer, t->screenshot_path);
            printf("[PLAYER_UI_TEST] screenshot=%s width=%d height=%d\n",
                   captured ? "PASS" : "FAIL", app->window_width, app->window_height);
            if (!captured) t->failed = true;
        }
    }
}

bool player_ui_test_skip_factory_restart(const PlayerUiTest *t) {
    if (t->mode) {
        puts("[PLAYER_UI_TEST] factory_reset restart=skipped");
        return true;
    }
    return false;
}

bool player_ui_test_take_cache_rescan_deferral(void) {
    if (s_ui_test_defer_cache_rescan_once) {
        s_ui_test_defer_cache_rescan_once = false;
        return true;
    }
    return false;
}

/* Models SDL refusing the push so the harness can prove the reclaim path.
   The event must not be queued at all: a queued event the reclaim then
   races would free a job the queue still owns. */
bool player_ui_test_drop_worker_event(void) {
    return s_ui_test_drop_worker_event;
}

void player_ui_test_delay_package_status_worker(bool (*closing)(void *user)) {
    const char *delay_text = getenv("NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS");
    if (delay_text && delay_text[0]) {
        char *end = NULL;
        unsigned long delay = strtoul(delay_text, &end, 10);
        /* A slow check, standing in for hashing a large package: it stops
           when the launcher closes, as the hashing does. */
        if (end && *end == '\0' && delay <= 5000) {
            for (unsigned long waited = 0; waited < delay && !closing(NULL);
                 waited += 10u) {
                SDL_Delay(10);
            }
        }
    }
}

void player_ui_test_catalog_begin(PlayerUiTestCatalog *c) {
    if (s_ui_test_catalog_reload_count > 0 &&
        s_ui_test_catalog_reload_path &&
        s_ui_test_catalog_reload_path[0] &&
        !s_ui_test_catalog_reload_started) {
        c->start = SDL_CreateSemaphore(0);
        c->done = SDL_CreateSemaphore(0);
        c->repetitions = s_ui_test_catalog_reload_count;
        if (!c->start || !c->done) {
            if (c->start) {
                SDL_DestroySemaphore(c->start);
                c->start = NULL;
            }
            if (c->done) {
                SDL_DestroySemaphore(c->done);
                c->done = NULL;
            }
        }
    }
}

/* Every exit of the worker reaches this loop, including the warm-cache shortcut,
   so the UI thread's reload hand-off can never wait for a repetition this worker
   would skip. */
void player_ui_test_catalog_run_worker(PlayerUiTestCatalog *c, const GameRecord *game,
                                       NkRuntimePackageStatus status, const char *root) {
    if (c->start && c->done) {
        for (unsigned i = 0; i < c->repetitions; i++) {
            (void)SDL_WaitSemaphore(c->start);
            NkRuntimePackageStatus repeated_status = status;
            if (root) {
                repeated_status = nk_launch_validate_runtime_package(
                    root, game, NULL, NULL, 0);
            }
            if (repeated_status != status) {
                c->status_mismatch = true;
            }
            c->status_checks++;
            (void)SDL_SignalSemaphore(c->done);
        }
    }
}

/* The worker thread could not start: its semaphores are not used. */
void player_ui_test_catalog_release(PlayerUiTestCatalog *c) {
    if (c->start) {
        SDL_DestroySemaphore(c->start);
        c->start = NULL;
    }
    if (c->done) {
        SDL_DestroySemaphore(c->done);
        c->done = NULL;
    }
}

void player_ui_test_catalog_report(PlayerUiTestCatalog *c, NkRuntimePackageStatus status) {
    if (c->repetitions > 0) {
        bool stress_passed = !s_ui_test_catalog_reload_failed &&
            s_ui_test_catalog_reload_completed == c->repetitions &&
            c->status_checks == c->repetitions &&
            !c->status_mismatch;
        printf("[PLAYER_UI_TEST] catalog_reload result=%s reloads=%u "
               "status_checks=%u status=%d\n",
               stress_passed ? "PASS" : "FAIL",
               s_ui_test_catalog_reload_completed,
               c->status_checks, (int)status);
        if (c->start) {
            SDL_DestroySemaphore(c->start);
            c->start = NULL;
        }
        if (c->done) {
            SDL_DestroySemaphore(c->done);
            c->done = NULL;
        }
    }
}

/* The UI thread's half of the stress: each reload of the title catalog pairs with one
   of the worker's validations. */
void player_ui_test_catalog_drive(PlayerUiTestCatalog *c) {
    if (c->start && c->done && !s_ui_test_catalog_reload_started) {
        s_ui_test_catalog_reload_started = true;
        for (unsigned i = 0; i < s_ui_test_catalog_reload_count; i++) {
            (void)SDL_SignalSemaphore(c->start);
            char reload_error[256] = "";
            if (!nk_title_manifest_load_overlay_ext(
                    s_ui_test_catalog_reload_path, true, reload_error,
                    sizeof(reload_error))) {
                s_ui_test_catalog_reload_failed = true;
            } else {
                s_ui_test_catalog_reload_completed++;
            }
            (void)SDL_WaitSemaphore(c->done);
        }
    }
}

bool player_ui_test_fail_package_check_thread(int index, const GameRecord *game) {
    printf("[PLAYER_UI_TEST] package_check index=%d disc=%s\n", index, game->disc_id);
    g_ui_test_package_status_thread_create_attempts++;
    bool fail_thread_create =
        player_ui_test_fail_thread_create_for_game(game);
    for (unsigned failure = 0;
         failure < g_ui_test_fail_package_status_thread_create_count;
         failure++) {
        if (g_ui_test_package_status_thread_create_attempts ==
            g_ui_test_fail_package_status_thread_create_on_attempts[failure]) {
            fail_thread_create = true;
            break;
        }
    }
    if (fail_thread_create) {
        int fail_title_index = player_ui_test_fail_title_index(game->disc_id);
        if (fail_title_index >= 0) {
            g_ui_test_fail_package_status_thread_create_for_counts[
                fail_title_index]++;
        }
    }
    return fail_thread_create;
}

void player_ui_test_report_post_build_validation(int status, const char *reason,
                                                 uint64_t job_session_generation,
                                                 uint64_t current_session_generation) {
    printf("[PLAYER_UI_TEST] post_build_validation status=%d reason=%s "
           "build_session_generation=%llu current_build_session_generation=%llu\n",
           status, reason[0] ? reason : "none",
           (unsigned long long)job_session_generation,
           (unsigned long long)current_session_generation);
}

void player_ui_test_report_post_build_transition(PlayerApp *app, const char *disc_id,
                                                 int current_index) {
    /* runtime= is what makes the title's card offer PLAY. */
    printf("[PLAYER_UI_TEST] post_build_transition view=ready_library "
           "disc=%s found=%d prepared=%d status=%d runtime=%d\n",
           disc_id, current_index >= 0,
           current_index >= 0
               ? app->games[current_index].is_prepared : 0,
           current_index >= 0
               ? (int)app->games[current_index].status : -1,
           current_index >= 0
               ? (int)player_app_cached_game_has_runtime(
                     app, &app->games[current_index]) : 0);
}

/* The UI test holds the worker, to see the library draw frames while a check runs. */
void player_ui_test_hold_disc_check(void) {
    const char *hold = getenv("NK_UI_TEST_DISC_CHECK_HOLD_MS");
    if (hold && hold[0]) SDL_Delay((Uint32)strtoul(hold, NULL, 10));
}

/* The launch worker's synthetic slow start: the launch takes this long before its checks. */
const char *player_ui_test_slow_start_text(void) {
    return getenv("NK_UI_TEST_SLOW_START_MS");
}

void player_ui_test_slow_start(const PlayerUiTestLaunch *launch) {
    if (launch->slow_start_ms > 0u) SDL_Delay(launch->slow_start_ms);
}

/* The headless regression cannot open a modal box, so it reads the dialog
   the host would show from these lines; the message's line break is
   written as \n to keep it on one. */
void player_ui_test_report_quit_dialog(const SDL_MessageBoxData *dialog) {
    printf("[PLAYER_UI_TEST] QUIT_DIALOG title=%s\n", dialog->title);
    printf("[PLAYER_UI_TEST] QUIT_DIALOG message=");
    for (const char *ch = dialog->message; *ch; ++ch) {
        if (*ch == '\n') fputs("\\n", stdout);
        else putchar(*ch);
    }
    putchar('\n');
    for (int i = 0; i < dialog->numbuttons; ++i) {
        const SDL_MessageBoxButtonData *button = &dialog->buttons[i];
        printf("[PLAYER_UI_TEST] QUIT_DIALOG button id=%d return_default=%d "
               "escape_default=%d label=%s\n",
               button->buttonID,
               (button->flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) != 0,
               (button->flags & SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT) != 0,
               button->text);
    }
    fflush(stdout);
}

bool player_ui_test_messagebox_fails(void) {
    return getenv("NK_UI_TEST_MESSAGEBOX_FAIL") != NULL;
}

void player_ui_test_report_open_url(const char *url) {
    printf("[PLAYER_UI_TEST] open_url=%s\n", url);
    fflush(stdout);
}

/* The test reads the report from stdout, one line: newlines become " | ". */
void player_ui_test_report_copied_details(bool copied, const char *report) {
    printf("[PLAYER_UI_TEST] copied_details clipboard=%d report=", copied ? 1 : 0);
    for (const char *c = report; *c; c++) {
        if (*c == '\n') fputs(c[1] ? " | " : "", stdout);
        else putchar(*c);
    }
    putchar('\n');
}

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

PackageHttpOpen player_ui_test_update_transport(void) {
    return player_update_test_open;
}

/* The key update signatures are checked with: a TEST-ONLY key from
   NK_UI_TEST_UPDATE_DIR's update_signing_key.pub when the test wrote one, so its
   synthetic releases can be signed. Without that file *handled stays false and the
   caller uses the built-in key. */
bool player_ui_test_update_signing_key(NkMinisignKey *out, bool *handled) {
    const char *dir = getenv("NK_UI_TEST_UPDATE_DIR");
    char path[NK_MAX_PATH];
    char text[512];
    FILE *file;
    *handled = false;
    if (dir && snprintf(path, sizeof(path), "%s%cupdate_signing_key.pub", dir,
                        nk_platform_path_separator()) < (int)sizeof(path) &&
        (file = nk_fopen_utf8(path, "rb")) != NULL) {
        size_t length = fread(text, 1, sizeof(text), file);
        fclose(file);
        *handled = true;
        return length < sizeof(text) && nk_minisign_parse_key(text, length, out);
    }
    return false;
}

#endif /* NK_PLAYER_UI_REGRESSION_TEST */
