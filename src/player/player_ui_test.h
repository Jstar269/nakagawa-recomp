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
 */

#ifndef NK_PLAYER_UI_TEST_H
#define NK_PLAYER_UI_TEST_H

#ifdef NK_PLAYER_UI_REGRESSION_TEST

#include "player_state.h"
#include "ui/player_ui_nk.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

#endif /* NK_PLAYER_UI_REGRESSION_TEST */

#endif /* NK_PLAYER_UI_TEST_H */
