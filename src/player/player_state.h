/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_PLAYER_STATE_H
#define NAKAGAWA_PLAYER_STATE_H

#include "nk_types.h"
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_launch.h"
#include "input_settings.h"
#include "package_builder.h"
#include "generated/nk_title_catalog.h"

#include <stdbool.h>
#include <stdint.h>

#define MAX_LIBRARY_GAMES NK_MAX_GAMES
#define MAX_TITLE_LEN NK_MAX_TITLE_LEN
#define MAX_PATH_LEN NK_MAX_PATH
#define MAX_DISC_ID_LEN NK_MAX_DISC_ID_LEN
#define MAX_SHOWCASE_GAMES 8

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
    VIEW_CONTROLLER_SETTINGS,
    VIEW_BUILDING_PACKAGE,
    VIEW_PREREQ_CONSENT,
    VIEW_PREREQ_PROGRESS,
    VIEW_PREREQ_ABOUT,
    VIEW_CONFIRM_REMOVE_TOOLS
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

/* Requested client-area geometry in SDL screen/window coordinate units. */
typedef struct {
    int x;
    int y;
    int width;
    int height;
} PlayerWindowRect;

/* Window decoration sizes in the same coordinate units as PlayerWindowRect. */
typedef struct {
    int top;
    int left;
    int bottom;
    int right;
} PlayerWindowFrame;

typedef enum {
    PLAYER_CLOSE_QUIT = 0,
    PLAYER_CLOSE_CONFIRM_REQUIRED
} PlayerCloseDecision;

typedef struct {
    bool status_valid;
    bool identity_valid;
    bool validation_pending;
    bool validation_failed;
    bool explicit_retry_pending;
    bool runtime_available;
    char disc_id[MAX_DISC_ID_LEN];
    char title_id[64];
    char selected_executable[MAX_PATH_LEN];
    char package_identity[65];
    NkRuntimePackageStatus status;
    uint64_t last_checked_ms;
} PlayerRuntimePackageCacheEntry;

/* A worker-create failure has no validator result to keep in the cache. Keep
 * that unresolved state in a bounded, title-keyed side record so a successful
 * validation of another title can invalidate the cache without making this
 * title look like MISSING. The key includes the selected executable because a
 * title may legitimately switch between EBOOT.BIN and a fallback. */
typedef struct {
    bool valid;
    char disc_id[MAX_DISC_ID_LEN];
    char title_id[64];
    char selected_executable[MAX_PATH_LEN];
    uint32_t failure_count;
    uint64_t retry_after_ms;
} PlayerRuntimePackageWorkerFailure;

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
    int resolution_scale; /* 1 = Native 480x272, 2 = 2x Vita, 3 = 3x 720p, 4 = 4x 1080p */
    bool fullscreen;
    bool launcher_fullscreen;
    bool vsync;
    int fps_cap;          /* 30, 60, 0 = uncapped */
    int master_volume;    /* 0..100 */
    bool reduce_motion;   /* freeze pulses/sweeps for motion sensitivity */
    bool launcher_window_maximized;
    bool launcher_window_position_valid;
    int launcher_window_x;
    int launcher_window_y;
    int launcher_window_width;
    int launcher_window_height;
    char controller_name[64];
    bool controller_connected;
    /* Save locations are not a setting: nk_launch_prepare_session resolves a
       writable per-disc Memory Stick root at launch (SR_MEMSTICK). The settings
       screen shows that real root instead of a configurable dead field. */
} PlayerSettings;

typedef enum {
    PLAYER_PREREQ_IDLE = 0,
    PLAYER_PREREQ_CONSENT,
    PLAYER_PREREQ_BOOTSTRAP,
    PLAYER_PREREQ_DOWNLOAD,
    PLAYER_PREREQ_INSTALLED,
    PLAYER_PREREQ_FAILED,
    PLAYER_PREREQ_CANCELLED
} PlayerPrerequisitePhase;

typedef struct {
    PlayerPrerequisitePhase phase;
    PackagePrerequisiteList items;
    int game_index;
    int item_count;
    uint64_t total_bytes;
    uint64_t total_received_bytes;
    uint64_t item_received_bytes;
    uint64_t item_total_bytes;
    char current_item[96];
    char error_code[48];
    char error_message[512];
    bool cancel_requested;
    bool bootstrap_python;
    bool resume_build_pending;
} PlayerPrerequisiteState;

/* Room for a diagnostic that names up to three searched paths plus
 * surrounding guidance (CLI_NOT_FOUND is the longest today). */
#define PLAYER_ERROR_DETAILS_MAX (MAX_PATH_LEN * 3 + 512)

typedef struct {
    char error_code[32];
    char title[128];
    char message[512];
    char details[PLAYER_ERROR_DETAILS_MAX];
    char recovery_action_label[64];
    PlayerView return_view;
    char failed_stage[64];
    char boundary_text[512];
    char log_file_path[MAX_PATH_LEN];
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

/* One slot per code the player's builder can emit (DISC_SFO, EXECUTABLE,
 * EXPERIMENTAL, GUEST_MODULES, RUNTIME_PACKAGE, SYSTEM_FONTS, AUDIO_OUTPUT: 7)
 * plus headroom, so adding a check never silently displaces another. */
#define PLAYER_PREFLIGHT_MAX_CHECKS 10

typedef struct {
    PlayerPreflightCheck checks[PLAYER_PREFLIGHT_MAX_CHECKS];
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
    PlayerRuntimePackageCacheEntry runtime_package_cache[MAX_LIBRARY_GAMES];
    PlayerRuntimePackageWorkerFailure runtime_package_worker_failures[MAX_LIBRARY_GAMES];
    uint64_t runtime_package_cache_generation;
    int game_count;
    int selected_game_index;
    GameRecord inspecting_game;
    PreparationState prep_state;
    PlayerSettings settings;
    ErrorState last_error;
    SetupWizardState wizard;
    PackageBuildSession build_session;
    PlayerPrerequisiteState prerequisites;
    PackageBootstrapSession bootstrap_session;
    bool prerequisite_job_started;
    bool prerequisite_fetcher_started;
    bool prerequisite_bootstrap_ready;
    bool prerequisite_cancel_sent;
    bool request_open_license_folder;
    char requested_open_path[NK_MAX_PATH];

    /* Native core state */
    NkLibrary library;
    NkLaunchSession launch_session;
    /* Optional repository/install root for runtime resolution. Empty means the
       launcher's normal current-directory default. Keeping it on the app makes
       demo and test launches use the same root as PLAY NOW. */
    char runtime_root[MAX_PATH_LEN];
    /* Directory holding the player executable; the package builder finds
       tools/nk_cli.py relative to it. Empty when unknown. */
    char install_root[MAX_PATH_LEN];
    /* Read-only package root beside the player executable. Bundled showcase
       records are transient and never enter the user's persisted library. */
    char showcase_root[MAX_PATH_LEN];
    GameRecord showcase_games[MAX_SHOWCASE_GAMES];
    int showcase_count;
    bool is_game_running;
    uint64_t launch_time_ms;
    /* Launch the child without a window. PLAY NOW always requests the window;
       this is the launcher's own headless default, reached only when a caller
       asks for it explicitly, so a host with no display can still prove the
       launch path end to end. */
    bool launch_headless;

    /* Set by the renderer when a control asks for the host file dialog. The
       renderer has no SDL_Window and must stay free of platform dialog calls,
       so it raises this and the event loop in main.c consumes it. */
    bool request_file_picker;
    /* The library card requests a package-status worker retry through this
       flag; worker creation and queue ownership stay on the main event loop. */
    bool request_package_status_retry;

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
    /* Why the last launch could not hand the runtime the mapping this disc
     * asked for, or an empty string. Drawn on the library card so a disc that
     * silently launched on the global mapping is never a mystery. */
    char input_profile_notice[192];

    /* Settings persistence */
    char settings_path[MAX_PATH_LEN];
    char settings_notice[128];

    /* Window & layout metrics */
    int window_width;
    int window_height;
    float dpi_scale;
    bool logical_ui;
    bool enable_focus_handoff;
    bool child_window_ready;
    bool close_confirmation_pending;
    char boot_event_file_path[MAX_PATH_LEN];
    bool should_quit;
} PlayerApp;

/* State management API */
void player_app_init(PlayerApp *app);
bool player_app_add_game(PlayerApp *app, const GameRecord *game);
/* Re-adding a disc that is already in the library re-inspects it, which yields a
 * record with no staging state. When `incoming` is the same disc as `existing`
 * (disc ID, version and image size), carry over the staged-asset state, the
 * prepared root and last-played time so re-adding never discards a completed
 * extraction. Returns true when anything was carried over. */
bool player_merge_readded_game(const GameRecord *existing, GameRecord *incoming);
bool player_app_remove_game(PlayerApp *app, int game_index);
void player_app_set_view(PlayerApp *app, PlayerView view);
void player_app_set_error(PlayerApp *app, const char *code, const char *title, const char *msg, const char *recovery_label, PlayerView return_view);
void player_app_set_cli_not_found_error(PlayerApp *app,
                                        const char *recovery_label,
                                        PlayerView return_view);
void player_app_populate_sample_games(PlayerApp *app);
void player_app_sync_library(PlayerApp *app);
bool player_app_discover_showcase(PlayerApp *app, const char *executable_directory);

/* Maximum SDL3_ttf load candidates from player_app_ttf_library_candidates. */
#define PLAYER_APP_TTF_MAX_CANDIDATES 4

/* Ordered SDL3_ttf library candidates for a player running from exe_dir: the
 * library beside the executable first, then the platform loader's default
 * bare-name search (PATH on Windows). Pure: no filesystem or loader calls. */
int player_app_ttf_library_candidates(const char *exe_dir,
                                      char out[][MAX_PATH_LEN], int max_out);
/* True when a disc directory entry name is safe to write under the private
 * per-title folder (the tooling's FILENAME_RE and WINDOWS_RESERVED rules). */
bool player_module_name_is_safe(const char *name);
bool player_game_is_showcase(const GameRecord *game);
void player_app_set_runtime_root(PlayerApp *app, const char *root);
NkRuntimePackageStatus player_app_validate_runtime_package(
    const PlayerApp *app,
    const GameRecord *game,
    NkRuntimePackageInfo *out_info,
    char *reason,
    size_t reason_size
);
bool player_app_game_has_runtime(const PlayerApp *app, const GameRecord *game);
NkRuntimePackageStatus player_app_cached_runtime_package_status(
    const PlayerApp *app, const GameRecord *game);
bool player_app_cached_game_has_runtime(const PlayerApp *app,
                                        const GameRecord *game);
bool player_app_runtime_package_check_pending(const PlayerApp *app,
                                               const GameRecord *game);
bool player_app_runtime_package_check_failed(const PlayerApp *app,
                                              const GameRecord *game);
bool player_app_runtime_package_worker_start_failed(const PlayerApp *app,
                                                     const GameRecord *game);
bool player_app_runtime_package_worker_start_failed_retry_due(
    const PlayerApp *app, const GameRecord *game, uint64_t now_ms);
void player_app_runtime_package_worker_start_failed_record(
    PlayerApp *app, const GameRecord *game, uint64_t now_ms);
void player_app_runtime_package_worker_start_failed_clear(
    PlayerApp *app, const GameRecord *game);
void player_app_runtime_package_cache_mark_explicit_retry(PlayerApp *app,
                                                          int game_index);
void player_app_runtime_package_cache_mark_failed(PlayerApp *app,
                                                  int game_index,
                                                  uint64_t now_ms);
void player_app_runtime_package_cache_invalidate(PlayerApp *app);
void player_app_runtime_package_cache_mark_pending(PlayerApp *app, int game_index);
void player_app_runtime_package_cache_store(
    PlayerApp *app, int game_index, const GameRecord *game,
    bool identity_valid, const char *package_identity,
    NkRuntimePackageStatus status, bool runtime_available,
    uint64_t checked_ms);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
uint64_t player_app_ui_test_validation_calls(void);
#endif

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
void player_app_toggle_launcher_fullscreen(PlayerApp *app);
PlayerCloseDecision player_app_close_decision(const PlayerApp *app,
                                              bool user_confirmed);
/* SDL may report one host close action with more than one close event type.
 * Claim the first such event in a drained event batch only. */
bool player_app_close_request_batch_claim(bool *close_request_handled);
/* If the host confirmation dialog cannot be shown, keep the running game
 * alive and require a second explicit close request before forcing quit. */
void player_app_note_close_confirmation_failure(PlayerApp *app);
bool player_app_take_close_confirmation_fallback(PlayerApp *app);
bool player_app_boot_event_is_window_ready(const char *line);
bool player_app_child_window_ready(PlayerApp *app);
/* Fit a requested client rectangle into a display. `usable` is the display's
 * usable bounds and `frame` the window decoration, so the client area is
 * `usable` reduced by `frame`; `out` receives a CLIENT rectangle that fits
 * inside that area, ready for SDL_SetWindowSize/SDL_SetWindowPosition and for
 * the launcher's persisted launcher_window_* geometry. */
bool player_window_fit_to_display(PlayerWindowRect requested,
                                  PlayerWindowRect usable,
                                  PlayerWindowFrame frame,
                                  bool requested_position_valid,
                                  PlayerWindowRect *out);
void player_app_toggle_vsync(PlayerApp *app);
void player_app_toggle_reduce_motion(PlayerApp *app);
void player_app_adjust_volume(PlayerApp *app, int delta);
void player_app_move_focus(PlayerApp *app, int delta, int focus_count);

/* Index of the library entry carrying this disc ID, or -1 if there is none.
   nk_library_add_or_update updates an existing record IN PLACE, so the entry
   just written is not necessarily the last one; callers that need the record
   they have just added must look it up rather than assume it was appended. */
int player_app_find_game_by_disc_id(const PlayerApp *app, const char *disc_id);

/* Disc ID of the selected library card, or NULL when no card is selected. The
   per-title controller mapping choice (#520) needs one to name. */
const char *player_app_selected_disc_id(const PlayerApp *app);

/* Move the library selection by `delta` entries, clamped to the library. */
void player_app_move_selection(PlayerApp *app, int delta);

/* How many library cards fit in the current window, at least one. */
int player_app_visible_library_cards(const PlayerApp *app);

/* Whether the settings renderer has enough client width and height for its
 * two-column layout. This is pure state logic so the threshold is tested
 * independently of SDL and the renderer cannot drift from the layout contract. */
bool player_settings_uses_two_columns(int window_width, int window_height);

/* Horizontal offset, from the card's left edge, where the settings right
 * column begins. It is never left of the end of the launcher fullscreen
 * control plus a gutter, so the columns cannot overlap at any card width the
 * two-column layout accepts. */
float player_settings_second_column_offset(float card_width);

/* Whether the event loop should attempt the launcher-to-game focus handoff on
 * this frame. Pure state logic so the once-per-launch contract is tested
 * without SDL: a failed SDL_MinimizeWindow keeps the launcher visible, and
 * retrying it every frame would rewrite the launcher settings every frame. */
bool player_app_should_attempt_window_handoff(bool interactive_window,
                                              bool game_running,
                                              bool handoff_attempted,
                                              bool child_window_ready);

/* The boot-event marker pathname the next interactive launch will use, without
 * consuming the sequence. Pure naming so tests can build a filesystem seam at
 * the exact path production will choose instead of assuming a sequence. */
void player_app_next_boot_event_path(char *out, size_t out_size);

/* How many keyboard/gamepad focus stops the current view offers, at least
 * one. Pure state logic (no SDL): the renderer draws its buttons in this
 * exact order, so the event loop can clamp focus_index and tests can pin
 * the contract without opening a window. */
int player_app_focus_count(const PlayerApp *app);

/* Process launch integration */
bool player_app_launch_game(PlayerApp *app, int game_index);
void player_app_stop_game(PlayerApp *app);

/* Copy the launch-relevant player settings into a prepared session's runtime
 * configuration (resolution, frame cap, vsync, fullscreen, master volume).
 * reduce_motion is launcher-UI state and deliberately not copied. Kept as a
 * named seam so tests can pin the settings -> session mapping without
 * spawning a child. */
void player_app_apply_settings_to_session(const PlayerSettings *settings,
                                          NkRuntimeConfig *config);

/**
 * Copy the effective host input mapping for `game` into the prepared session's
 * runtime configuration as the NK_INPUT_PROFILE path the child will load.
 *
 * A disc with its own per-title mapping gets that mapping written to
 * <config>/input_profiles/<disc_id>.json and pointed at; every other disc is
 * handed the global profile path. Two discs in one session therefore use
 * different mappings without anyone editing a global file (#520). A mapping that
 * cannot be resolved is reported in `input_profile_notice` and the session keeps
 * the profile path it already had, rather than launching with a silent surprise.
 */
NkResult player_app_apply_input_profile_to_session(PlayerApp *app, const GameRecord *game);

/* Advance the running-game session state machine one tick at `now_ms`.
 *
 * Returns true when this tick observed the child's exit and finalized the
 * session. A child that exits before 500 ms of wall time is reported through
 * the structured RUNTIME_PREMATURE_EXIT error; a later non-zero exit becomes
 * RUNTIME_ERROR_EXIT. Either way the session's process handles are released
 * and is_game_running is false before returning, so a subsequent launch
 * starts from clean state. Pure state logic (no SDL): the caller supplies the
 * clock so tests can drive classification deterministically. */
bool player_app_monitor_game_session(PlayerApp *app, uint64_t now_ms);

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

/* Package build actions */
bool player_app_start_package_build(PlayerApp *app, int game_index);
void player_app_cancel_package_build(PlayerApp *app);
/* Per-build consent and progress state. Consent is never persisted. */
bool player_app_prereq_begin(PlayerApp *app, int game_index,
                             bool python_missing, bool toolchain_missing);
void player_app_prereq_accept(PlayerApp *app, bool bootstrap_python);
void player_app_prereq_update_progress(PlayerApp *app, const char *item,
                                       uint64_t item_received,
                                       uint64_t item_total,
                                       uint64_t total_received,
                                       uint64_t total_bytes);
void player_app_prereq_complete(PlayerApp *app);
void player_app_prereq_fail(PlayerApp *app, const char *code,
                            const char *message);
void player_app_prereq_retry(PlayerApp *app);
void player_app_prereq_cancel(PlayerApp *app);
void player_app_prereq_finish_cancel(PlayerApp *app);
bool player_app_prereq_take_resume(PlayerApp *app);
bool player_app_open_prerequisite_about(PlayerApp *app);
void player_app_remove_prerequisites(PlayerApp *app);
void player_app_set_build_error(
    PlayerApp *app,
    const char *failed_stage,
    const char *boundary_text,
    const char *log_file_path
);

#endif /* NAKAGAWA_PLAYER_STATE_H */
