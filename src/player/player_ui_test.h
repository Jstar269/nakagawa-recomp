/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * The scripted-event harness behind the player's UI regression tests
 * (tests/native/test_player_ui.py). A test runs the real player with
 * NK_UI_TEST_* environment settings: player_ui_test_next_event() turns its
 * event script into SDL events and harness actions, and
 * player_ui_test_report_frame() prints one [PLAYER_UI_TEST] record per frame
 * for the test to check. Only the UI-test player build defines
 * NK_PLAYER_UI_REGRESSION_TEST; the shipped player compiles none of this.
 *
 * This header is also the one seam between the harness and main.c. main.c
 * calls the player_ui_test_* hooks below without any #ifdef: the UI-test build
 * gets the real ones (player_ui_test.c), the shipped build gets empty inline
 * ones that the compiler drops, and PLAYER_UI_TEST_BUILD says which build it
 * is for the few places where the two builds differ.
 */

#ifndef NK_PLAYER_UI_TEST_H
#define NK_PLAYER_UI_TEST_H

#include "package_builder.h"
#include "player_state.h"
#include "ui/player_ui_nk.h"
#include "update_check.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef NK_PLAYER_UI_REGRESSION_TEST
#define PLAYER_UI_TEST_BUILD 1
#else
#define PLAYER_UI_TEST_BUILD 0
#endif

/* A launch worker's synthetic slow start (NK_UI_TEST_SLOW_START_MS; always 0 in the shipped player). */
typedef struct PlayerUiTestLaunch {
    uint32_t slow_start_ms;
} PlayerUiTestLaunch;

#ifdef NK_PLAYER_UI_REGRESSION_TEST

/* Harness actions, carried in SDL_EVENT_USER's code. */
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

#define UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURES 8

/* Package-status worker thread starts the script asked to fail, by attempt number. */
extern unsigned g_ui_test_package_status_thread_create_attempts;
extern unsigned g_ui_test_fail_package_status_thread_create_on_attempts[
    UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURES];
extern unsigned g_ui_test_fail_package_status_thread_create_count;
/* The titles whose package-status thread start fails, and how often each has. */
#define UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURE_TITLES 8
extern unsigned g_ui_test_fail_package_status_thread_create_for_counts[
    UI_TEST_MAX_PACKAGE_STATUS_THREAD_CREATE_FAILURE_TITLES];
/* The next UI, when it draws the library, so a frame record can read its model. */
extern const PlayerUiNk *g_ui_test_next_ui;
/* A WAIT_PACKAGE_FAILURE step in progress: which title, and how many failures. */
extern bool g_ui_test_waiting_for_package_failure;
extern char g_ui_test_wait_package_failure_disc[MAX_DISC_ID_LEN];
extern unsigned g_ui_test_wait_package_failure_target;

bool player_ui_test_parse_positive_u64(const char *text, uint64_t *value);
bool player_ui_test_parse_fail_attempts(const char *text);
bool player_ui_test_parse_fail_titles(const char *text);
bool player_ui_test_fail_thread_create_for_game(const GameRecord *game);
int player_ui_test_fail_title_index(const char *disc_id);
/* The next scripted event: 1 with *event filled, 0 at the end, -1 on a bad step. */
int player_ui_test_next_event(const char *script, size_t *cursor, SDL_Event *event);
bool player_ui_test_copy_file(const char *source, const char *destination);
bool player_ui_test_start_synthetic_build(PlayerApp *app, unsigned game_index);
const char *player_ui_test_view_name(PlayerView view);
void player_ui_test_report_frame(int frame_number, const PlayerApp *app,
                                 SDL_Renderer *renderer, bool running,
                                 uint64_t render_elapsed_ns,
                                 uint64_t validation_calls,
                                 const char *invalid_disc_message);

/* One run's harness state: what main() used to keep in ui_test_* locals. */
typedef struct PlayerUiTest {
    const char *events;
    const char *screenshot_path;
    const char *error_code;
    const char *iso_path;
    const char *art_mount_source;
    bool wait_background;
    bool mode;               /* a --ui-test-* option was given */
    bool failed;
    bool quit_queued;
    size_t event_cursor;
    int frame;
    bool waiting_for_time;
    bool waiting_for_view;
    bool waiting_for_package_status;
    NkRuntimePackageStatus wait_package_status_target;
    bool waiting_for_art_attempt;
    bool waiting_for_art_delay;
    uint64_t wait_art_delay_deadline;
    unsigned wait_art_attempt_target;
    uint64_t wait_deadline;
    uint32_t wait_view_mask;
    bool fake_game_running;
    char *drop_data;         /* a scripted file drop's data, until its event is handled */
    SDL_Texture *render_target;
    uint64_t first_frame_started_ns;
    uint64_t first_frame_validation_count;
    uint64_t frame_started_ns;
    uint64_t frame_validation_count;
} PlayerUiTest;

/* The package-status worker's catalog-reload stress state (--ui-test-catalog-reload-*). */
typedef struct PlayerUiTestCatalog {
    SDL_Semaphore *start;
    SDL_Semaphore *done;
    unsigned repetitions;
    unsigned status_checks;
    bool status_mismatch;
} PlayerUiTestCatalog;

void player_ui_test_init(PlayerUiTest *t);
/* Takes one command-line option when it is a --ui-test-* one. */
bool player_ui_test_parse_arg(PlayerUiTest *t, const char *arg);
/* After the command line: checks the environment settings and pairs. */
void player_ui_test_check_environment(PlayerUiTest *t);
static inline bool player_ui_test_active(const PlayerUiTest *t) { return t->mode; }
static inline bool player_ui_test_failed(const PlayerUiTest *t) { return t->failed; }
static inline bool player_ui_test_game_faked(const PlayerUiTest *t) {
    return t->mode && t->fake_game_running;
}
/* Puts a synthetic state onto a loaded app: card images, a fake running game, a
   synthetic error. */
void player_ui_test_prepare_app(PlayerUiTest *t, PlayerApp *app);
void player_ui_test_apply_motion(const PlayerUiTest *t, PlayerApp *app);
void player_ui_test_apply_update_settings(PlayerApp *app);
void player_ui_test_set_next_ui(const PlayerUiNk *next_ui);
void player_ui_test_report_startup(const PlayerUiTest *t, const PlayerUiNk *next_ui);
/* The scripted run draws into its own target; false when that cannot be made. */
bool player_ui_test_create_target(PlayerUiTest *t, SDL_Renderer *renderer, int width,
                                  int height);
void player_ui_test_destroy_target(PlayerUiTest *t, SDL_Renderer *renderer);
void player_ui_test_begin_first_frame(PlayerUiTest *t);
void player_ui_test_end_first_frame(PlayerUiTest *t, const PlayerApp *app,
                                    SDL_Renderer *renderer, const char *invalid_disc_message);
void player_ui_test_report_update_phase(const PlayerUiTest *t, const PlayerApp *app);
/* One frame's scripted step: the waits, the assertions and the next event. */
void player_ui_test_step_script(PlayerUiTest *t, PlayerApp *app, SDL_Renderer *renderer,
                                bool *running, bool background_pending);
void player_ui_test_event_handled(PlayerUiTest *t, const SDL_Event *event);
void player_ui_test_begin_frame(PlayerUiTest *t);
void player_ui_test_end_frame(PlayerUiTest *t, const PlayerApp *app, SDL_Renderer *renderer,
                              bool running, const char *invalid_disc_message);
/* The launcher's factory reset restarts a new copy; a scripted run says it skipped it. */
bool player_ui_test_skip_factory_restart(const PlayerUiTest *t);

/* One automatic package rescan skipped after a scripted cache invalidation. */
bool player_ui_test_take_cache_rescan_deferral(void);
bool player_ui_test_drop_worker_event(void);

/* The package-status worker. */
void player_ui_test_delay_package_status_worker(bool (*closing)(void *user));
void player_ui_test_catalog_begin(PlayerUiTestCatalog *c);
void player_ui_test_catalog_run_worker(PlayerUiTestCatalog *c, const GameRecord *game,
                                       NkRuntimePackageStatus status, const char *root);
void player_ui_test_catalog_release(PlayerUiTestCatalog *c);
void player_ui_test_catalog_report(PlayerUiTestCatalog *c, NkRuntimePackageStatus status);
void player_ui_test_catalog_drive(PlayerUiTestCatalog *c);
bool player_ui_test_fail_package_check_thread(int index, const GameRecord *game);
void player_ui_test_report_post_build_validation(int status, const char *reason,
                                                 uint64_t job_session_generation,
                                                 uint64_t current_session_generation);
void player_ui_test_report_post_build_transition(PlayerApp *app, const char *disc_id,
                                                 int current_index);
void player_ui_test_hold_disc_check(void);
/* The raw NK_UI_TEST_SLOW_START_MS setting, or NULL. */
const char *player_ui_test_slow_start_text(void);
void player_ui_test_slow_start(const PlayerUiTestLaunch *launch);

/* Hooks where the UI-test build reports or answers instead of acting. */
void player_ui_test_report_quit_dialog(const SDL_MessageBoxData *dialog);
bool player_ui_test_messagebox_fails(void);
void player_ui_test_report_open_url(const char *url);
void player_ui_test_report_copied_details(bool copied, const char *report);
PackageHttpOpen player_ui_test_update_transport(void);
bool player_ui_test_update_signing_key(NkMinisignKey *out, bool *handled);

#else /* shipped player: the hooks do nothing */

typedef struct PlayerUiTest { char unused; } PlayerUiTest;
typedef struct PlayerUiTestCatalog { char unused; } PlayerUiTestCatalog;

static inline void player_ui_test_init(PlayerUiTest *t) { (void)t; }
static inline bool player_ui_test_parse_arg(PlayerUiTest *t, const char *arg) {
    (void)t; (void)arg;
    return false;
}
static inline void player_ui_test_check_environment(PlayerUiTest *t) { (void)t; }
static inline bool player_ui_test_active(const PlayerUiTest *t) { (void)t; return false; }
static inline bool player_ui_test_failed(const PlayerUiTest *t) { (void)t; return false; }
static inline bool player_ui_test_game_faked(const PlayerUiTest *t) { (void)t; return false; }
static inline void player_ui_test_prepare_app(PlayerUiTest *t, PlayerApp *app) {
    (void)t; (void)app;
}
static inline void player_ui_test_apply_motion(const PlayerUiTest *t, PlayerApp *app) {
    (void)t; (void)app;
}
static inline void player_ui_test_apply_update_settings(PlayerApp *app) { (void)app; }
static inline void player_ui_test_set_next_ui(const PlayerUiNk *next_ui) { (void)next_ui; }
static inline void player_ui_test_report_startup(const PlayerUiTest *t,
                                                 const PlayerUiNk *next_ui) {
    (void)t; (void)next_ui;
}
static inline bool player_ui_test_create_target(PlayerUiTest *t, SDL_Renderer *renderer,
                                                int width, int height) {
    (void)t; (void)renderer; (void)width; (void)height;
    return true;
}
static inline void player_ui_test_destroy_target(PlayerUiTest *t, SDL_Renderer *renderer) {
    (void)t; (void)renderer;
}
static inline void player_ui_test_begin_first_frame(PlayerUiTest *t) { (void)t; }
static inline void player_ui_test_end_first_frame(PlayerUiTest *t, const PlayerApp *app,
                                                  SDL_Renderer *renderer,
                                                  const char *invalid_disc_message) {
    (void)t; (void)app; (void)renderer; (void)invalid_disc_message;
}
static inline void player_ui_test_report_update_phase(const PlayerUiTest *t,
                                                      const PlayerApp *app) {
    (void)t; (void)app;
}
static inline void player_ui_test_step_script(PlayerUiTest *t, PlayerApp *app,
                                              SDL_Renderer *renderer, bool *running,
                                              bool background_pending) {
    (void)t; (void)app; (void)renderer; (void)running; (void)background_pending;
}
static inline void player_ui_test_event_handled(PlayerUiTest *t, const SDL_Event *event) {
    (void)t; (void)event;
}
static inline void player_ui_test_begin_frame(PlayerUiTest *t) { (void)t; }
static inline void player_ui_test_end_frame(PlayerUiTest *t, const PlayerApp *app,
                                            SDL_Renderer *renderer, bool running,
                                            const char *invalid_disc_message) {
    (void)t; (void)app; (void)renderer; (void)running; (void)invalid_disc_message;
}
static inline bool player_ui_test_skip_factory_restart(const PlayerUiTest *t) {
    (void)t;
    return false;
}
static inline bool player_ui_test_take_cache_rescan_deferral(void) { return false; }
static inline bool player_ui_test_drop_worker_event(void) { return false; }
static inline void player_ui_test_delay_package_status_worker(bool (*closing)(void *user)) {
    (void)closing;
}
static inline void player_ui_test_catalog_begin(PlayerUiTestCatalog *c) { (void)c; }
static inline void player_ui_test_catalog_run_worker(PlayerUiTestCatalog *c,
                                                     const GameRecord *game,
                                                     NkRuntimePackageStatus status,
                                                     const char *root) {
    (void)c; (void)game; (void)status; (void)root;
}
static inline void player_ui_test_catalog_release(PlayerUiTestCatalog *c) { (void)c; }
static inline void player_ui_test_catalog_report(PlayerUiTestCatalog *c,
                                                 NkRuntimePackageStatus status) {
    (void)c; (void)status;
}
static inline void player_ui_test_catalog_drive(PlayerUiTestCatalog *c) { (void)c; }
static inline bool player_ui_test_fail_package_check_thread(int index,
                                                            const GameRecord *game) {
    (void)index; (void)game;
    return false;
}
static inline void player_ui_test_report_post_build_validation(
    int status, const char *reason, uint64_t job_session_generation,
    uint64_t current_session_generation) {
    (void)status; (void)reason; (void)job_session_generation;
    (void)current_session_generation;
}
static inline void player_ui_test_report_post_build_transition(PlayerApp *app,
                                                               const char *disc_id,
                                                               int current_index) {
    (void)app; (void)disc_id; (void)current_index;
}
static inline void player_ui_test_hold_disc_check(void) {}
static inline const char *player_ui_test_slow_start_text(void) { return NULL; }
static inline void player_ui_test_slow_start(const PlayerUiTestLaunch *launch) {
    (void)launch;
}
static inline void player_ui_test_report_quit_dialog(const SDL_MessageBoxData *dialog) {
    (void)dialog;
}
static inline bool player_ui_test_messagebox_fails(void) { return false; }
static inline void player_ui_test_report_open_url(const char *url) { (void)url; }
static inline void player_ui_test_report_copied_details(bool copied, const char *report) {
    (void)copied; (void)report;
}
static inline PackageHttpOpen player_ui_test_update_transport(void) { return NULL; }
static inline bool player_ui_test_update_signing_key(NkMinisignKey *out, bool *handled) {
    (void)out;
    *handled = false;
    return false;
}

#endif /* NK_PLAYER_UI_REGRESSION_TEST */

#endif /* NK_PLAYER_UI_TEST_H */
