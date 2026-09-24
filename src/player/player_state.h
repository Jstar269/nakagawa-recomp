/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_PLAYER_STATE_H
#define NAKAGAWA_PLAYER_STATE_H

#include "nk_types.h"
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_launch.h"
#include "input_settings.h"
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
    VIEW_EXPERIMENTAL_TITLE,
    VIEW_UNSUPPORTED_TITLE,
    VIEW_PREPARING,
    VIEW_SETTINGS,
    VIEW_ERROR,
    VIEW_SETUP_WIZARD,
    /* Dedicated post-staging library state. It renders the normal library
       card, but lets tests and the event loop distinguish a newly completed
       setup transaction from an ordinary library visit. */
    PLAYER_VIEW_READY_LIBRARY,
    VIEW_CONTROLLER_SETTINGS
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
    bool reduce_motion;   /* freeze pulses/sweeps for motion sensitivity */
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

typedef enum {
    WIZARD_STEP_WELCOME = 0,
    WIZARD_STEP_SELECT_GAME,
    WIZARD_STEP_INSPECT_VERIFY,
    WIZARD_STEP_SYSTEM_FONTS,
    WIZARD_STEP_READY_LAUNCH
} WizardStep;

typedef enum {
    PREFLIGHT_OK = 0,
    PREFLIGHT_MISSING,
    PREFLIGHT_INCOMPATIBLE,
    PREFLIGHT_STALE,
    PREFLIGHT_UNSUPPORTED,
    PREFLIGHT_IN_PROGRESS,
    PREFLIGHT_INVALID
} PlayerPreflightStatus;

typedef struct {
    char code[32];
    PlayerPreflightStatus status;
    char message[2048];
    unsigned int issue_numbers[2];
    size_t issue_count;
} PlayerPreflightCheck;

typedef struct {
    PlayerPreflightCheck checks[6];
    size_t count;
} PlayerCompatibilityPreflight;

typedef struct {
    WizardStep step;
    bool iso_selected;
    bool font_confirmed;
    char status_message[256];
    int extraction_percent;
    int files_extracted;
    int total_files;
    bool is_extracting;
    bool extraction_complete;
    bool extraction_failed;
    bool extraction_requested;
    bool extraction_cancel_requested;
    NkResult extraction_result;
    char extraction_current_file[MAX_PATH_LEN];
    char extraction_error[256];
    char staging_root[MAX_PATH_LEN];
    PlayerCompatibilityPreflight preflight;
} SetupWizardState;

typedef struct {
    PlayerView active_view;
    GameRecord games[MAX_LIBRARY_GAMES];
    int game_count;
    int selected_game_index;
    GameRecord inspecting_game;
    PreparationState prep_state;
    PlayerSettings settings;
    ErrorState last_error;
    SetupWizardState wizard;

    /* Native core state */
    NkLibrary library;
    NkLaunchSession launch_session;
    /* Optional repository/install root for runtime resolution. Empty means the
       launcher's normal current-directory default. Keeping it on the app makes
       demo and test launches use the same root as PLAY NOW. */
    char runtime_root[MAX_PATH_LEN];
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

    /* Controller settings and live input monitor (#357) */
    InputSettingsState input_settings;
    bool host_buttons_live[NK_HOST_BUTTON_COUNT];
    int16_t host_axes_live[NK_HOST_AXIS_COUNT];

    /* Settings persistence */
    char settings_path[MAX_PATH_LEN];
    char settings_notice[128];

    /* Window & layout metrics */
    int window_width;
    int window_height;
    float dpi_scale;
    bool should_quit;
} PlayerApp;

/* State management API */
void player_app_init(PlayerApp *app);
bool player_app_add_game(PlayerApp *app, const GameRecord *game);
bool player_app_remove_game(PlayerApp *app, int game_index);
void player_app_set_view(PlayerApp *app, PlayerView view);
void player_app_set_error(PlayerApp *app, const char *code, const char *title, const char *msg, const char *recovery_label, PlayerView return_view);
void player_app_populate_sample_games(PlayerApp *app);
void player_app_sync_library(PlayerApp *app);
void player_app_set_runtime_root(PlayerApp *app, const char *root);
NkRuntimePackageStatus player_app_validate_runtime_package(
    const PlayerApp *app,
    const GameRecord *game,
    NkRuntimePackageInfo *out_info,
    char *reason,
    size_t reason_size
);

#define NK_PLAYER_SETTINGS_SCHEMA_VERSION 1

/* Settings mutations. All values are validated and clamped; invalid inputs
 * are ignored so a stray click or keypress can never corrupt launch config.
 * Settings are persisted to a versioned JSON file (settings.json) in the
 * per-user config directory and applied to the child runtime where supported
 * via nk_launch_prepare_session. */
void player_app_settings_init_default(PlayerSettings *settings);
NkResult player_app_load_settings(PlayerApp *app, const char *file_path);
NkResult player_app_save_settings(const PlayerApp *app, const char *file_path);

void player_app_set_resolution_scale(PlayerApp *app, int scale);
void player_app_cycle_resolution_scale(PlayerApp *app, int direction);
void player_app_set_fps_cap(PlayerApp *app, int cap);
void player_app_cycle_fps_cap(PlayerApp *app, int direction);
void player_app_toggle_fullscreen(PlayerApp *app);
void player_app_toggle_vsync(PlayerApp *app);
void player_app_toggle_reduce_motion(PlayerApp *app);
void player_app_adjust_volume(PlayerApp *app, int delta);
void player_app_move_focus(PlayerApp *app, int delta, int focus_count);

/* Index of the library entry carrying this disc ID, or -1 if there is none.
   nk_library_add_or_update updates an existing record IN PLACE, so the entry
   just written is not necessarily the last one; callers that need the record
   they have just added must look it up rather than assume it was appended. */
int player_app_find_game_by_disc_id(const PlayerApp *app, const char *disc_id);

/* Move the library selection by `delta` entries, clamped to the library. */
void player_app_move_selection(PlayerApp *app, int delta);

/* How many library cards fit in the current window, at least one. */
int player_app_visible_library_cards(const PlayerApp *app);

/* How many keyboard/gamepad focus stops the current view offers, at least
 * one. Pure state logic (no SDL): the renderer draws its buttons in this
 * exact order, so the event loop can clamp focus_index and tests can pin
 * the contract without opening a window. */
int player_app_focus_count(const PlayerApp *app);

/* Process launch integration */
bool player_app_launch_game(PlayerApp *app, int game_index);
void player_app_stop_game(PlayerApp *app);

/* Commit the inspected, successfully staged title to the persistent library
 * and enter PLAYER_VIEW_READY_LIBRARY. Runtime readiness remains separate:
 * assets_staged may be true while is_prepared is false. */
bool player_app_register_staged_game(PlayerApp *app);

/* Setup Wizard API */
void player_app_start_setup_wizard(PlayerApp *app);
void player_app_wizard_next(PlayerApp *app);
void player_app_wizard_back(PlayerApp *app);
void player_app_wizard_cancel(PlayerApp *app);
void player_app_wizard_reset_extraction(PlayerApp *app);
bool player_app_wizard_take_extraction_request(PlayerApp *app);
void player_app_wizard_request_cancel(PlayerApp *app);
bool player_app_wizard_cancel_requested(const PlayerApp *app);
void player_app_wizard_set_extraction_progress(PlayerApp *app, int percent,
                                               int files_extracted, int total_files,
                                               const char *current_file);
void player_app_wizard_finish_extraction(PlayerApp *app, NkResult result,
                                         const char *error_message);
void player_app_build_compatibility_preflight(
    PlayerApp *app, bool disc_readable, bool param_sfo_parsed,
    const NkIsoExecutableReport *executables);

#endif /* NAKAGAWA_PLAYER_STATE_H */
