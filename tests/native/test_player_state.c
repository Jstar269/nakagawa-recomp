/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Player library state: entry lookup and honest add results.
 *
 * Two defects lived here:
 *
 *   - nk_library_add_or_update updates an existing record IN PLACE, so the
 *     entry just written is not necessarily the last one. --launch-now took
 *     app.game_count - 1 and could therefore prepare and launch a different
 *     game while the console reported the requested ISO;
 *   - player_app_add_game discarded both the insert and the save result and
 *     returned true unconditionally, so a rejected insert or an unwritable
 *     user-data directory still reported success and the entry vanished on
 *     the next start.
 *
 * These link player_state.c directly. Nothing here touches SDL, and nothing
 * here writes to the user's real library: the failure path returns before the
 * save, which is the whole point of the assertion.
 */

#include "player_state.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void seed_entry(NkGameEntry *entry, const char *disc_id, const char *name) {
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->disc_id, sizeof(entry->disc_id), "%s", disc_id);
    snprintf(entry->title_name, sizeof(entry->title_name), "%s", name);
    snprintf(entry->disc_version, sizeof(entry->disc_version), "1.00");
    entry->status = NK_STATUS_IDENTIFIED;
}

static void write_file(const char *path) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite("fixture", 1, 7, file) == 7);
    assert(fclose(file) == 0);
}

int main(void) {
    /* PlayerApp holds 64 game records twice over; keep it off the stack. */
    PlayerApp *app = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(app != NULL);

    /* Build the library directly rather than through player_app_init, which
       would read (and later write) the real user data directory. */
    nk_library_init(&app->library);

    NkGameEntry entry;
    seed_entry(&entry, "TEST00001", "First");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    seed_entry(&entry, "TEST00002", "Second");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    seed_entry(&entry, "TEST00005", "Third");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    player_app_sync_library(app);
    assert(app->game_count == 3);

    /* 1. An entry that is NOT last is found at its real index.
     *
     * This is the case --launch-now got wrong: re-adding TEST00001 updates it
     * in place at index 0, while game_count - 1 names TEST00005. */
    printf("[PLAYER_STATE_TEST] Subtest 1: lookup by disc ID\n");
    fflush(stdout);
    assert(player_app_find_game_by_disc_id(app, "TEST00001") == 0);
    assert(player_app_find_game_by_disc_id(app, "TEST00002") == 1);
    assert(player_app_find_game_by_disc_id(app, "TEST00005") == 2);
    assert(player_app_find_game_by_disc_id(app, "TEST00001") != app->game_count - 1);

    /* 2. Updating an existing record keeps its index rather than appending. */
    printf("[PLAYER_STATE_TEST] Subtest 2: update is in place\n");
    fflush(stdout);
    seed_entry(&entry, "TEST00001", "First, revisited");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    player_app_sync_library(app);
    assert(app->game_count == 3);
    assert(player_app_find_game_by_disc_id(app, "TEST00001") == 0);
    assert(strcmp(app->games[0].title_name, "First, revisited") == 0);

    /* 3. Unknown and malformed disc IDs report absence, not index 0. */
    printf("[PLAYER_STATE_TEST] Subtest 3: absent disc IDs\n");
    fflush(stdout);
    assert(player_app_find_game_by_disc_id(app, "TEST09999") == -1);
    assert(player_app_find_game_by_disc_id(app, "") == -1);
    assert(player_app_find_game_by_disc_id(app, NULL) == -1);
    assert(player_app_find_game_by_disc_id(NULL, "TEST00001") == -1);

    /* 4. A rejected insert must be reported as a failure.
     *
     * Fill the library to its limit, then add one more. The insert fails, so
     * player_app_add_game returns before ever reaching nk_library_save -- no
     * file is touched by this test. */
    printf("[PLAYER_STATE_TEST] Subtest 4: a rejected insert reports failure\n");
    fflush(stdout);
    nk_library_init(&app->library);
    for (int i = 0; i < NK_MAX_GAMES; i++) {
        char disc_id[NK_MAX_DISC_ID_LEN];
        snprintf(disc_id, sizeof(disc_id), "FULL%05d", i);
        seed_entry(&entry, disc_id, "Filler");
        assert(nk_library_add_or_update(&app->library, &entry) == NK_OK);
    }
    player_app_sync_library(app);
    assert(app->library.count == NK_MAX_GAMES);

    seed_entry(&entry, "OVERFLOW1", "One too many");
    assert(nk_library_add_or_update(&app->library, &entry) == NK_ERROR_OUT_OF_MEMORY);
    assert(player_app_add_game(app, &entry) == false);
    assert(player_app_find_game_by_disc_id(app, "OVERFLOW1") == -1);

    /* 5. A record with no disc ID is refused outright. */
    printf("[PLAYER_STATE_TEST] Subtest 5: a record with no disc ID is refused\n");
    fflush(stdout);
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.title_name, sizeof(entry.title_name), "Nameless");
    assert(player_app_add_game(app, &entry) == false);

    /* 6. Every library entry must be reachable.
     *
     * The strip draws cards left to right on a 280-pixel pitch, so a
     * 1280-wide window shows about four. Everything past those was drawn
     * outside the window, and src/player had no wheel, paging, keyboard or
     * offset handling at all -- so with a full library most games could
     * neither be selected nor launched. */
    printf("[PLAYER_STATE_TEST] Subtest 6: every entry is reachable\n");
    fflush(stdout);

    app->window_width = 1280;
    app->window_height = 720;
    int visible = player_app_visible_library_cards(app);
    assert(visible >= 1);
    assert(visible < NK_MAX_GAMES);   /* otherwise this proves nothing */

    /* The library is already full from subtest 4. */
    assert(app->game_count == NK_MAX_GAMES);
    app->selected_game_index = 0;
    for (int i = 1; i < NK_MAX_GAMES; i++) {
        player_app_move_selection(app, 1);
        assert(app->selected_game_index == i);
    }
    /* Including the ones that never fit on screen at once. */
    assert(app->selected_game_index == NK_MAX_GAMES - 1);
    assert(app->selected_game_index >= visible);

    /* Selection clamps rather than wrapping or running off either end. */
    player_app_move_selection(app, 1);
    assert(app->selected_game_index == NK_MAX_GAMES - 1);
    player_app_move_selection(app, -NK_MAX_GAMES * 2);
    assert(app->selected_game_index == 0);
    player_app_move_selection(app, -1);
    assert(app->selected_game_index == 0);

    /* A window too narrow for even one card still offers one. */
    app->window_width = 100;
    assert(player_app_visible_library_cards(app) == 1);
    app->window_width = 1920;
    assert(player_app_visible_library_cards(app) > visible);

    /* 7. The demo fixture set must contain a title that can actually launch.
     *
     * A fixture library whose every entry resolves to no runtime made the launch
     * path look implemented while it had never once been reached end to end.
     * display-smoke-v1 is the public title whose build layout nk_launch.c can
     * resolve, so it has to be in the set a fresh install shows. This also pins
     * the memory-only contract: populate must leave a populated library alone. */
    printf("[PLAYER_STATE_TEST] Subtest 7: demo fixtures include a launchable title\n");
    fflush(stdout);
    PlayerApp *fresh = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(fresh != NULL);
    nk_library_init(&fresh->library);
    player_app_sync_library(fresh);
    assert(fresh->game_count == 0);

    /* Point the same probe at a disposable fixture root. A real display-smoke
       build is not a prerequisite of native-core-tests, but the entry must be
       marked prepared when the launcher's candidate binary is actually there. */
    char cache_dir[512];
    char fixture_root[700];
    char fixture_dir[800];
    char fixture_exe[900];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
    snprintf(fixture_root, sizeof(fixture_root), "%s%cpr177_player_state_root",
             cache_dir, nk_platform_path_separator());
    snprintf(fixture_dir, sizeof(fixture_dir), "%s%cbuild%cdisplay-smoke-v1",
             fixture_root, nk_platform_path_separator(), nk_platform_path_separator());
    snprintf(fixture_exe, sizeof(fixture_exe), "%s%cdisplay-smoke-v1%s",
             fixture_dir, nk_platform_path_separator(),
#if defined(_WIN32) || defined(_WIN64)
             ".exe"
#else
             ""
#endif
    );
    assert(nk_platform_mkdir_p(fixture_dir));
    write_file(fixture_exe);
    player_app_set_runtime_root(fresh, fixture_root);

    player_app_populate_sample_games(fresh);
    assert(fresh->game_count > 0);
    int disp = player_app_find_game_by_disc_id(fresh, "TEST00006");
    assert(disp >= 0);
    assert(strcmp(fresh->games[disp].title_id, "display-smoke-v1") == 0);
    assert(fresh->games[disp].is_prepared == true);
    assert(fresh->games[disp].status == NK_STATUS_PREPARED);

    int before = fresh->game_count;
    player_app_populate_sample_games(fresh);
    assert(fresh->game_count == before);
    assert(remove(fixture_exe) == 0);
    free(fresh);

    /* 8. A launch started from the player requests a window. */
    printf("[PLAYER_STATE_TEST] Subtest 8: a player launch requests a window\n");
    fflush(stdout);
    PlayerApp *launcher = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(launcher != NULL);
    nk_library_init(&launcher->library);

    NkGameEntry unknown;
    seed_entry(&unknown, "ZZZZ99999", "Not In The Catalog");
    snprintf(unknown.title_id, sizeof(unknown.title_id), "not-a-catalog-title");
    assert(nk_library_add_or_update(&launcher->library, &unknown) == NK_OK);
    player_app_sync_library(launcher);
    assert(launcher->game_count == 1);

    assert(player_app_launch_game(launcher, 0) == false);
    assert(launcher->launch_session.config.gui_mode == true);
    assert(launcher->is_game_running == false);
    free(launcher);

    free(app);
    printf("[PLAYER_STATE_TEST] ALL PLAYER STATE TESTS PASSED!\n");
    return 0;
}
