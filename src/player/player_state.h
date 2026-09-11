/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_PLAYER_STATE_H
#define NAKAGAWA_PLAYER_STATE_H

#include "nk_types.h"
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_launch.h"
#include "generated/nk_title_catalog.h"

#include <stdbool.h>
#include <stdint.h>

#define MAX_LIBRARY_GAMES NK_MAX_GAMES
#define MAX_TITLE_LEN NK_MAX_TITLE_LEN
#define MAX_PATH_LEN NK_MAX_PATH
#define MAX_DISC_ID_LEN NK_MAX_DISC_ID_LEN

typedef enum {
    VIEW_LIBRARY = 0,
    VIEW_INSPECTING,
    VIEW_SUPPORTED_TITLE,
    VIEW_UNSUPPORTED_TITLE,
    VIEW_PREPARING,
    VIEW_SETTINGS,
    VIEW_ERROR
} PlayerView;

typedef enum {
    STATUS_UNIDENTIFIED = NK_STATUS_UNIDENTIFIED,
    STATUS_IDENTIFIED = NK_STATUS_IDENTIFIED,
    STATUS_SUPPORTED_PREPARATION = NK_STATUS_SUPPORTED_PREPARATION,
    STATUS_PREPARED = NK_STATUS_PREPARED,
    STATUS_BOOTS = NK_STATUS_BOOTS,
    STATUS_PLAYABLE = NK_STATUS_PLAYABLE,
    STATUS_VERIFIED = NK_STATUS_VERIFIED
} GameSupportStatus;

typedef enum {
    STAGE_IDLE = 0,
    STAGE_INSPECTING_ISO,
    STAGE_EXTRACTING_CONTAINERS,
    STAGE_DECRYPTING_MODULES,
    STAGE_VALIDATING_ELFS,
    STAGE_PREPARING_VFS,
    STAGE_READY,
    STAGE_FAILED,
    STAGE_CANCELLED
} PlayerPrepStage;

typedef NkGameEntry GameRecord;

typedef struct {
    PlayerPrepStage stage;
    char operation[64];
    char current_item[128];
    int completed_items;
    int total_items;
    float percentage;
    int elapsed_ms;
    char status_message[256];
    bool cancellable;
} PreparationState;

typedef struct {
    int resolution_scale; /* 1 = Native 480x272, 2 = 2x Vita, 3 = 3x 720p, 4 = 4x 1080p, 8 = 4K */
    bool fullscreen;
    bool vsync;
    int fps_cap;          /* 30, 60, 0 = uncapped */
    int master_volume;    /* 0..100 */
    char controller_name[64];
    bool controller_connected;
    char save_directory[MAX_PATH_LEN];
} PlayerSettings;

typedef struct {
    char error_code[32];
    char title[128];
    char message[512];
    char recovery_action_label[64];
    PlayerView return_view;
} ErrorState;

typedef struct {
    PlayerView active_view;
    GameRecord games[MAX_LIBRARY_GAMES];
    int game_count;
    int selected_game_index;
    GameRecord inspecting_game;
    PreparationState prep_state;
    PlayerSettings settings;
    ErrorState last_error;

    /* Native core state */
    NkLibrary library;
    NkLaunchSession launch_session;
    bool is_game_running;
    uint64_t launch_time_ms;

    /* Set by the renderer when a control asks for the host file dialog. The
       renderer has no SDL_Window and must stay free of platform dialog calls,
       so it raises this and the event loop in main.c consumes it. */
    bool request_file_picker;

    /* Index of the leftmost visible card in the library strip. The strip
       lays cards out horizontally and a 1280-wide window fits about four, so
       without this every entry past the fourth was drawn outside the window
       with no way to reach it. */
    int library_scroll_index;

    /* Navigation & Focus */
    int focus_index; /* current focused UI element index for gamepad/keyboard */
    int active_tab;   /* for settings: 0=Display, 1=Audio, 2=Controller, 3=Logs */

    /* Window & layout metrics */
    int window_width;
    int window_height;
    float dpi_scale;
    bool should_quit;
} PlayerApp;

/* State management API */
void player_app_init(PlayerApp *app);
bool player_app_add_game(PlayerApp *app, const GameRecord *game);
void player_app_set_view(PlayerApp *app, PlayerView view);
void player_app_set_error(PlayerApp *app, const char *code, const char *title, const char *msg, const char *recovery_label, PlayerView return_view);
void player_app_populate_sample_games(PlayerApp *app);
void player_app_sync_library(PlayerApp *app);

/* Index of the library entry carrying this disc ID, or -1 if there is none.
   nk_library_add_or_update updates an existing record IN PLACE, so the entry
   just written is not necessarily the last one; callers that need the record
   they have just added must look it up rather than assume it was appended. */
int player_app_find_game_by_disc_id(const PlayerApp *app, const char *disc_id);

/* Move the library selection by `delta` entries, clamped to the library. */
void player_app_move_selection(PlayerApp *app, int delta);

/* How many library cards fit in the current window, at least one. */
int player_app_visible_library_cards(const PlayerApp *app);

/* Process launch integration */
bool player_app_launch_game(PlayerApp *app, int game_index);
void player_app_stop_game(PlayerApp *app);

#endif /* NAKAGAWA_PLAYER_STATE_H */
