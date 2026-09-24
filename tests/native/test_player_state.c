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

static void write_text_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    size_t length = strlen(text);
    assert(fwrite(text, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static const char *const FIXTURE_SHA256 =
    "f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d";

static void write_runtime_package_fixture(const char *user_root,
                                          const char *disc_id,
                                          const char *title_id,
                                          uint32_t abi_version,
                                          const char *executable_relative_path,
                                          const char *input_executable_sha256) {
    char packages[768], package_dir[896], executable[1100], image[1100];
    char package_json[8192], report_json[4096];
    snprintf(packages, sizeof(packages), "%s%cpackages", user_root,
             nk_platform_path_separator());
    snprintf(package_dir, sizeof(package_dir), "%s%c%s", packages,
             nk_platform_path_separator(), disc_id);
    assert(nk_platform_mkdir_p(package_dir));
    snprintf(executable, sizeof(executable), "%s%c%s.exe", package_dir,
             nk_platform_path_separator(), title_id);
    snprintf(image, sizeof(image), "%s%c%s_image.bin", package_dir,
             nk_platform_path_separator(), title_id);
    write_file(executable);
    write_file(image);

    int report_length = snprintf(report_json, sizeof(report_json),
        "{\"format\":\"nakagawa-build-report\",\"schema_version\":1,"
        "\"title_id\":\"%s\",\"runtime_abi\":{\"name\":\"CpuState\",\"version\":%u},"
        "\"input_hashes\":{\"manifest\":{\"sha256\":\"%064d\"},"
        "\"executable\":{\"sha256\":\"%s\"},\"modules\":[],\"psp_header\":null},"
        "\"tools\":{},\"coverage\":{},\"unsupported\":{\"imports\":[],"
        "\"instructions\":[],\"regions\":[]},\"analysis_diagnostics\":[],\"artifacts\":{}}\n",
        title_id, (unsigned)abi_version, 0, input_executable_sha256);
    assert(report_length > 0 && (size_t)report_length < sizeof(report_json));
    char report_path[1100];
    snprintf(report_path, sizeof(report_path), "%s%cbuild-report.json", package_dir,
             nk_platform_path_separator());
    write_text_file(report_path, report_json);

    int package_length = snprintf(package_json, sizeof(package_json),
        "{\"format\":\"nakagawa-aot-package\",\"schema_version\":1,"
        "\"title\":{\"id\":\"%s\",\"display_name\":\"Synthetic fixture\","
        "\"kind\":\"retail\",\"manifest_sha256\":\"%064d\","
        "\"protected_digest\":\"%064d\"},"
        "\"inputs\":{\"manifest\":{\"sha256\":\"%064d\"},"
        "\"executable\":{\"sha256\":\"%s\"},\"modules\":[],\"psp_header\":null},"
        "\"runtime\":{\"abi\":\"CpuState\",\"abi_version\":%u,"
        "\"abi_header_sha256\":\"%064d\",\"run_entry\":\"0x00000000\","
        "\"runtime_contract\":null,\"runtime_bindings\":{},"
        "\"required_runtime_bindings\":[]},"
        "\"executable\":{\"path\":\"%s\",\"sha256\":\"%s\","
        "\"guest_entry\":\"0x00000000\"},\"generated_objects\":[],"
        "\"required_local_assets\":[],\"build_report\":\"build-report.json\"}\n",
        title_id, 0, 0, 0, input_executable_sha256, (unsigned)abi_version,
        0, executable_relative_path, FIXTURE_SHA256);
    assert(package_length > 0 && (size_t)package_length < sizeof(package_json));
    char package_path[1100];
    snprintf(package_path, sizeof(package_path), "%s%cpackage.json", package_dir,
             nk_platform_path_separator());
    write_text_file(package_path, package_json);
}

static void write_experimental_profile_fixture(const char *user_root,
                                               const char *disc_id,
                                               const char *title_id,
                                               const char *selected_executable,
                                               const char *executable_sha256) {
    char experimental[768], profile_dir[896], profile_path[1100];
    char profile_json[4096];
    snprintf(experimental, sizeof(experimental), "%s%cexperimental", user_root,
             nk_platform_path_separator());
    snprintf(profile_dir, sizeof(profile_dir), "%s%c%s", experimental,
             nk_platform_path_separator(), disc_id);
    assert(nk_platform_mkdir_p(profile_dir));
    snprintf(profile_path, sizeof(profile_path), "%s%cprofile.json", profile_dir,
             nk_platform_path_separator());
    int length = snprintf(profile_json, sizeof(profile_json),
        "{\"schema_version\":1,\"manifest\":{\"schema_version\":1,"
        "\"id\":\"%s\",\"game_name\":\"%s\","
        "\"display_name\":\"Synthetic experiment\",\"kind\":\"retail\","
        "\"disc\":{\"id\":\"%s\",\"region\":\"NA\","
        "\"revision_policy\":\"exact-disc-id\"},"
        "\"executable\":{\"base\":0,\"entry\":0,\"bss_metadata_source\":\"none\","
        "\"extra_executable_spans\":[]},\"modules\":[],"
        "\"filesystem\":{\"data_root\":\"data\",\"memory_stick_root\":\"savedata\","
        "\"device_prefixes\":[\"disc0:\",\"ms0:\"]},\"hle_profile\":\"generic\","
        "\"feature_requirements\":[],\"verification_profile\":\"experimental-unverified\"},"
        "\"input_identity\":{\"disc_id\":\"%s\","
        "\"selected_executable\":\"PSP_GAME/SYSDIR/%s\","
        "\"executable_sha256\":\"%s\",\"elf_sha256\":\"%s\"}}\n",
        title_id, title_id, disc_id, disc_id, selected_executable,
        executable_sha256, executable_sha256);
    assert(length > 0 && (size_t)length < sizeof(profile_json));
    write_text_file(profile_path, profile_json);
}

static const PlayerPreflightCheck *find_preflight_check(
    const PlayerCompatibilityPreflight *preflight, const char *code) {
    if (!preflight || !code) return NULL;
    for (size_t i = 0; i < preflight->count; i++) {
        if (strcmp(preflight->checks[i].code, code) == 0) return &preflight->checks[i];
    }
    return NULL;
}

static const char *runtime_package_status_name(NkRuntimePackageStatus status) {
    switch (status) {
        case NK_RUNTIME_PACKAGE_OK: return "OK";
        case NK_RUNTIME_PACKAGE_MISSING: return "MISSING";
        case NK_RUNTIME_PACKAGE_INCOMPATIBLE: return "INCOMPATIBLE";
        case NK_RUNTIME_PACKAGE_STALE: return "STALE";
        default: return "UNKNOWN";
    }
}

int main(int argc, char **argv) {
    if (argc == 7 && strcmp(argv[1], "--validate-package") == 0) {
        char *end = NULL;
        unsigned long experimental = strtoul(argv[5], &end, 10);
        if (!end || *end || experimental > 1) return 2;
        PlayerApp *probe = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(probe != NULL);
        player_app_set_runtime_root(probe, argv[2]);
        GameRecord game;
        memset(&game, 0, sizeof(game));
        snprintf(game.disc_id, sizeof(game.disc_id), "%s", argv[3]);
        snprintf(game.title_id, sizeof(game.title_id), "%s", argv[4]);
        game.is_experimental = experimental != 0;
        snprintf(game.selected_executable, sizeof(game.selected_executable), "%s", argv[6]);
        NkRuntimePackageInfo info;
        char reason[2048];
        NkRuntimePackageStatus status = player_app_validate_runtime_package(
            probe, &game, &info, reason, sizeof(reason));
        printf("PACKAGE_STATUS=%s\nPACKAGE_REASON=%s\n",
               runtime_package_status_name(status), reason);
        free(probe);
        return 0;
    }

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

    /* Point package discovery at a disposable root. A bare executable/image
       pair must not make the fixture launchable; only a matching v1 package
       enables Play. */
    char cache_dir[512];
    char fixture_root[700];
    char fixture_package_dir[900];
    char fixture_package_json[1100];
    char fixture_report[1100];
    char fixture_exe[1100];
    char fixture_image[1100];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
    snprintf(fixture_root, sizeof(fixture_root), "%s%cpr177_player_state_root",
             cache_dir, nk_platform_path_separator());
    snprintf(fixture_package_dir, sizeof(fixture_package_dir), "%s%cpackages%cTEST00006",
             fixture_root, nk_platform_path_separator(), nk_platform_path_separator());
    snprintf(fixture_exe, sizeof(fixture_exe), "%s%cdisplay-smoke-v1%s",
             fixture_package_dir, nk_platform_path_separator(),
#if defined(_WIN32) || defined(_WIN64)
             ".exe"
#else
             ""
#endif
    );
    snprintf(fixture_image, sizeof(fixture_image), "%s%cdisplay-smoke-v1_image.bin",
             fixture_package_dir, nk_platform_path_separator());
    snprintf(fixture_package_json, sizeof(fixture_package_json), "%s%cpackage.json",
             fixture_package_dir, nk_platform_path_separator());
    snprintf(fixture_report, sizeof(fixture_report), "%s%cbuild-report.json",
             fixture_package_dir, nk_platform_path_separator());
    write_runtime_package_fixture(fixture_root, "TEST00006", "display-smoke-v1", 2,
                                  "display-smoke-v1.exe", FIXTURE_SHA256);
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
    assert(remove(fixture_image) == 0);
    assert(remove(fixture_report) == 0);
    assert(remove(fixture_package_json) == 0);
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

    /* 9. Settings mutations validate and clamp: previously the settings
     * screen drew preset buttons whose clicks were discarded, so this is
     * the failing-before contract for the rehaul. */
    printf("[PLAYER_STATE_TEST] Subtest 9: settings mutations validate\n");
    fflush(stdout);
    PlayerApp *settings = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(settings != NULL);
    nk_library_init(&settings->library);
    settings->settings.resolution_scale = 4;
    settings->settings.fps_cap = 60;
    settings->settings.master_volume = 80;
    settings->settings.vsync = true;
    settings->settings.fullscreen = false;

    player_app_set_resolution_scale(settings, 2);
    assert(settings->settings.resolution_scale == 2);
    player_app_set_resolution_scale(settings, 7);
    assert(settings->settings.resolution_scale == 2);
    player_app_set_resolution_scale(settings, 0);
    assert(settings->settings.resolution_scale == 2);
    player_app_cycle_resolution_scale(settings, 1);
    assert(settings->settings.resolution_scale == 4);
    player_app_cycle_resolution_scale(settings, -1);
    assert(settings->settings.resolution_scale == 2);

    player_app_set_fps_cap(settings, 30);
    assert(settings->settings.fps_cap == 30);
    player_app_set_fps_cap(settings, 999);
    assert(settings->settings.fps_cap == 30);
    player_app_cycle_fps_cap(settings, 1);
    assert(settings->settings.fps_cap == 60);
    player_app_cycle_fps_cap(settings, 1);
    assert(settings->settings.fps_cap == 0);
    player_app_cycle_fps_cap(settings, 1);
    assert(settings->settings.fps_cap == 30);

    player_app_toggle_vsync(settings);
    assert(settings->settings.vsync == false);
    player_app_toggle_vsync(settings);
    assert(settings->settings.vsync == true);
    player_app_toggle_fullscreen(settings);
    assert(settings->settings.fullscreen == true);
    player_app_toggle_fullscreen(settings);
    assert(settings->settings.fullscreen == false);
    assert(settings->settings.reduce_motion == false);
    player_app_toggle_reduce_motion(settings);
    assert(settings->settings.reduce_motion == true);
    player_app_toggle_reduce_motion(settings);
    assert(settings->settings.reduce_motion == false);

    player_app_adjust_volume(settings, 5);
    assert(settings->settings.master_volume == 85);
    player_app_adjust_volume(settings, 1000);
    assert(settings->settings.master_volume == 100);
    player_app_adjust_volume(settings, -2000);
    assert(settings->settings.master_volume == 0);
    free(settings);

    /* 10. Focus clamps into range so keyboard/gamepad activation can never
     * target a control the view no longer draws. */
    printf("[PLAYER_STATE_TEST] Subtest 10: focus clamps\n");
    fflush(stdout);
    PlayerApp *focus = (PlayerApp *)calloc(1, sizeof(PlayerApp));
    assert(focus != NULL);
    focus->focus_index = 0;
    player_app_move_focus(focus, 5, 3);
    assert(focus->focus_index == 2);
    player_app_move_focus(focus, -10, 3);
    assert(focus->focus_index == 0);
    player_app_move_focus(focus, 1, 0);
    assert(focus->focus_index == 0);
    player_app_move_focus(NULL, 1, 3);
    free(focus);

    /* 11. Library removal touches only the entry, never the ISO file, and
     * invalid indices are refused before any save. */
    printf("[PLAYER_STATE_TEST] Subtest 11: library removal is entry-only\n");
    fflush(stdout);
    assert(player_app_remove_game(NULL, 0) == false);
    assert(player_app_remove_game(app, -1) == false);
    assert(player_app_remove_game(app, NK_MAX_GAMES + 100) == false);
    {
        PlayerApp *removal = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(removal != NULL);
        nk_library_init(&removal->library);
        /* Redirect persistence at a disposable file: remove saves, and the
         * test must never write the user's real library. */
        char rcache[512];
        assert(nk_platform_get_path(NK_PATH_CACHE, rcache, sizeof(rcache)));
        char rpath_tmp[1024];
        snprintf(rpath_tmp, sizeof(rpath_tmp),
                 "%s%cpr177_rm.json", rcache, nk_platform_path_separator());
        assert(strlen(rpath_tmp) < sizeof(removal->library.library_path));
        strcpy(removal->library.library_path, rpath_tmp);
        remove(removal->library.library_path);
        seed_entry(&entry, "RMV00001", "First");
        assert(nk_library_add_or_update(&removal->library, &entry) == NK_OK);
        seed_entry(&entry, "RMV00002", "Second");
        assert(nk_library_add_or_update(&removal->library, &entry) == NK_OK);
        player_app_sync_library(removal);
        assert(removal->game_count == 2);
        removal->selected_game_index = 1;
        assert(player_app_remove_game(removal, 0) == true);
        assert(removal->game_count == 1);
        assert(strcmp(removal->games[0].disc_id, "RMV00002") == 0);
        assert(player_app_find_game_by_disc_id(removal, "RMV00001") == -1);
        assert(player_app_remove_game(removal, 5) == false);
        remove(removal->library.library_path);
        free(removal);
    }

    /* 12. Focus-stop counts match the buttons each view draws, so the
     * event loop can never park focus on a control that does not exist. */
    printf("[PLAYER_STATE_TEST] Subtest 12: focus stops match drawn buttons\n");
    fflush(stdout);
    {
        PlayerApp *stops = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(stops != NULL);
        nk_library_init(&stops->library);
        stops->window_width = 1280;
        stops->window_height = 720;
        assert(player_app_focus_count(NULL) == 1);

        stops->active_view = VIEW_LIBRARY;
        stops->game_count = 0;
        assert(player_app_focus_count(stops) == 1); /* empty: add only */

        stops->active_view = PLAYER_VIEW_READY_LIBRARY;
        assert(player_app_focus_count(stops) == 1); /* empty ready library */
        stops->active_view = VIEW_LIBRARY;

        seed_entry(&entry, "FCS00001", "Unprepared");
        entry.is_prepared = false;
        stops->games[0] = entry;
        stops->game_count = 1;
        stops->selected_game_index = 0;
        assert(player_app_focus_count(stops) == 2); /* add + remove */

        stops->games[0].is_prepared = true;
        assert(player_app_focus_count(stops) == 2); /* no validated package: add + remove */

        stops->is_game_running = true;
        assert(player_app_focus_count(stops) == 3); /* stop + add + remove */

        /* Overflow adds the two paging stops. */
        stops->is_game_running = false;
        stops->window_width = 640;
        assert(player_app_visible_library_cards(stops) == 2);
        stops->game_count = 1;
        assert(player_app_focus_count(stops) == 2);
        seed_entry(&entry, "FCS00002", "Second");
        stops->games[1] = entry;
        seed_entry(&entry, "FCS00003", "Third");
        stops->games[2] = entry;
        stops->game_count = 3;
        assert(player_app_focus_count(stops) == 4);

        stops->active_view = VIEW_INSPECTING;
        assert(player_app_focus_count(stops) == 1);
        stops->active_view = VIEW_SUPPORTED_TITLE;
        assert(player_app_focus_count(stops) == 2);
        stops->active_view = VIEW_UNSUPPORTED_TITLE;
        assert(player_app_focus_count(stops) == 1);
        stops->active_view = VIEW_PREPARING;
        assert(player_app_focus_count(stops) == 1);
        stops->active_view = VIEW_SETTINGS;
        assert(player_app_focus_count(stops) == 13);
        stops->active_view = VIEW_ERROR;
        assert(player_app_focus_count(stops) == 1);

        stops->active_view = VIEW_SETUP_WIZARD;
        stops->wizard.step = WIZARD_STEP_WELCOME;
        assert(player_app_focus_count(stops) == 2);
        stops->wizard.step = WIZARD_STEP_SELECT_GAME;
        stops->wizard.iso_selected = false;
        assert(player_app_focus_count(stops) == 3);
        stops->wizard.iso_selected = true;
        assert(player_app_focus_count(stops) == 4);
        stops->wizard.step = WIZARD_STEP_INSPECT_VERIFY;
        assert(player_app_focus_count(stops) == 3);
        stops->wizard.step = WIZARD_STEP_SYSTEM_FONTS;
        assert(player_app_focus_count(stops) == 4);
        stops->wizard.step = WIZARD_STEP_READY_LAUNCH;
        assert(player_app_focus_count(stops) == 3);
        free(stops);
    }

    /* 13. Setup Wizard state machine: start, step navigation, and cancellation. */
    printf("[PLAYER_STATE_TEST] Subtest 13: setup wizard transitions\n");
    fflush(stdout);
    {
        PlayerApp *wiz = (PlayerApp *)calloc(1, sizeof(PlayerApp));
        assert(wiz != NULL);
        nk_library_init(&wiz->library);

        player_app_start_setup_wizard(wiz);
        assert(wiz->active_view == VIEW_SETUP_WIZARD);
        assert(wiz->wizard.step == WIZARD_STEP_WELCOME);
        assert(wiz->focus_index == 0);

        player_app_wizard_next(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_SELECT_GAME);

        /* Moving next without selected ISO requests file picker */
        wiz->request_file_picker = false;
        player_app_wizard_next(wiz);
        assert(wiz->request_file_picker == true);
        assert(wiz->wizard.step == WIZARD_STEP_SELECT_GAME);

        /* Simulating ISO inspection */
        snprintf(wiz->inspecting_game.iso_path, sizeof(wiz->inspecting_game.iso_path), "test.iso");
        snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id), "UCUS98701");
        wiz->inspecting_game.status = NK_STATUS_VERIFIED;
        player_app_wizard_next(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_INSPECT_VERIFY);

        /* Step 3 starts an asynchronous extraction request and remains
           visible until the reactive worker completion is delivered. */
        player_app_wizard_next(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_INSPECT_VERIFY);
        assert(wiz->wizard.is_extracting == true);
        assert(player_app_wizard_take_extraction_request(wiz) == true);
        assert(player_app_wizard_take_extraction_request(wiz) == false);
        assert(player_app_focus_count(wiz) == 1);
        player_app_wizard_set_extraction_progress(wiz, 42, 2, 5, "xbdata/menu.xb");
        assert(wiz->wizard.extraction_percent == 42);
        assert(wiz->wizard.files_extracted == 2);
        snprintf(wiz->inspecting_game.prepared_root,
                 sizeof(wiz->inspecting_game.prepared_root), "stage/TEST00001");
        wiz->inspecting_game.assets_staged = true;
        wiz->inspecting_game.extracted_asset_count = 4;
        wiz->inspecting_game.extracted_audio_count = 1;
        wiz->inspecting_game.extracted_visual_count = 1;
        wiz->inspecting_game.extracted_layout_count = 2;
        char cache_dir[512];
        char staged_library_path[700];
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
        snprintf(staged_library_path, sizeof(staged_library_path),
                 "%s%cplayer_state_staged_library.json", cache_dir,
                 nk_platform_path_separator());
        remove(staged_library_path);
        {
            char staged_library_bak[720];
            snprintf(staged_library_bak, sizeof(staged_library_bak), "%s.bak",
                     staged_library_path);
            remove(staged_library_bak);
        }
        assert(strlen(staged_library_path) < sizeof(wiz->library.library_path));
        memcpy(wiz->library.library_path, staged_library_path,
               strlen(staged_library_path) + 1);
        assert(player_app_register_staged_game(wiz) == true);
        assert(wiz->game_count == 1);
        assert(player_app_find_game_by_disc_id(wiz, "UCUS98701") == 0);
        assert(wiz->games[0].extracted_asset_count == 4);
        assert(wiz->active_view == PLAYER_VIEW_READY_LIBRARY);
        NkLibrary persisted;
        assert(nk_library_load(&persisted, staged_library_path) == NK_OK);
        const NkGameEntry *persisted_game = nk_library_find_by_disc_id(&persisted, "UCUS98701");
        assert(persisted_game != NULL);
        assert(persisted_game->assets_staged == true);
        assert(persisted_game->extracted_audio_count == 1);
        player_app_wizard_finish_extraction(wiz, NK_OK, NULL);
        assert(wiz->wizard.step == WIZARD_STEP_READY_LAUNCH);
        assert(wiz->wizard.extraction_complete == true);
        assert(wiz->active_view == PLAYER_VIEW_READY_LIBRARY);

        /* Back navigation */
        player_app_wizard_back(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_SYSTEM_FONTS);
        player_app_wizard_back(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_INSPECT_VERIFY);
        player_app_wizard_back(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_SELECT_GAME);
        player_app_wizard_back(wiz);
        assert(wiz->wizard.step == WIZARD_STEP_WELCOME);
        player_app_wizard_back(wiz);
        assert(wiz->active_view == VIEW_LIBRARY);
        remove(staged_library_path);
        {
            char staged_library_bak[720];
            snprintf(staged_library_bak, sizeof(staged_library_bak), "%s.bak",
                     staged_library_path);
            remove(staged_library_bak);
        }

        /* Cancel directly */
        player_app_start_setup_wizard(wiz);
        assert(wiz->active_view == VIEW_SETUP_WIZARD);
        player_app_wizard_cancel(wiz);
        assert(wiz->active_view == VIEW_LIBRARY);

        /* The compatibility report exposes the selected plaintext BOOT
           fallback and every current pre-launch boundary without touching a
           real title or runtime package. */
        char preflight_root[640], font_dir[720], font_path[800];
        assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));
        snprintf(preflight_root, sizeof(preflight_root), "%s%cplayer-preflight-synthetic",
                 cache_dir, nk_platform_path_separator());
        assert(nk_platform_mkdir_p(preflight_root));
        snprintf(font_dir, sizeof(font_dir), "%s%cfont", preflight_root,
                 nk_platform_path_separator());
        snprintf(font_path, sizeof(font_path), "%s%cjpn0.pgf", font_dir,
                 nk_platform_path_separator());
        const NkTitleEntry *synthetic = nk_title_catalog_find_by_id(
            "synthetic-allegrex-v1");
        assert(synthetic != NULL && synthetic->game_name != NULL &&
               synthetic->primary_disc_id != NULL);
        const char *synthetic_disc_id = synthetic->primary_disc_id;
        char build_dir[760], runtime_exe[900], runtime_image[900];
        char package_dir[900], package_json[1100], package_report[1100];
        snprintf(build_dir, sizeof(build_dir), "%s%cbuild%c%s", preflight_root,
                 nk_platform_path_separator(), nk_platform_path_separator(),
                 synthetic->game_name);
        snprintf(runtime_exe, sizeof(runtime_exe), "%s%c%s.exe", build_dir,
                 nk_platform_path_separator(), synthetic->game_name);
        snprintf(runtime_image, sizeof(runtime_image), "%s%c%s_image.bin", build_dir,
                 nk_platform_path_separator(), synthetic->game_name);
        snprintf(package_dir, sizeof(package_dir), "%s%cpackages%c%s", preflight_root,
                 nk_platform_path_separator(), nk_platform_path_separator(),
                 synthetic_disc_id);
        snprintf(package_json, sizeof(package_json), "%s%cpackage.json", package_dir,
                 nk_platform_path_separator());
        snprintf(package_report, sizeof(package_report), "%s%cbuild-report.json", package_dir,
                 nk_platform_path_separator());
        remove(runtime_exe);
        remove(runtime_image);
        remove(package_json);
        remove(package_report);
        remove(font_path);
        player_app_set_runtime_root(wiz, preflight_root);
        snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id),
                 "%s", synthetic_disc_id);
        snprintf(wiz->inspecting_game.title_id, sizeof(wiz->inspecting_game.title_id),
                 "synthetic-allegrex-v1");
        NkIsoExecutableReport executable_report;
        memset(&executable_report, 0, sizeof(executable_report));
        executable_report.eboot.kind = NK_ISO_EXEC_PSP_ENCRYPTED;
        executable_report.boot.kind = NK_ISO_EXEC_MIPS_ELF32;
        executable_report.selected = NK_ISO_EXEC_SELECTION_BOOT;
        executable_report.boot_fallback = true;
        snprintf(executable_report.selected_path, sizeof(executable_report.selected_path),
                 "BOOT.BIN");
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        assert(wiz->wizard.preflight.count == 5);
        const PlayerPreflightCheck *check = find_preflight_check(
            &wiz->wizard.preflight, "DISC_SFO");
        assert(check && check->status == PREFLIGHT_OK);
        check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
        assert(check && check->status == PREFLIGHT_OK);
        assert(strstr(check->message, "BOOT.BIN selected for analysis") != NULL);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        check = find_preflight_check(&wiz->wizard.preflight, "SYSTEM_FONTS");
        assert(check && check->status == PREFLIGHT_MISSING);
        check = find_preflight_check(&wiz->wizard.preflight, "AUDIO_OUTPUT");
        assert(check && check->status == PREFLIGHT_OK);

        assert(nk_platform_mkdir_p(build_dir));
        write_file(runtime_exe);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        write_file(runtime_image);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        write_runtime_package_fixture(preflight_root, synthetic_disc_id,
                                      "synthetic-allegrex-v1", 2,
                                      "synthetic-allegrex-v1.exe", FIXTURE_SHA256);
        assert(nk_platform_mkdir_p(font_dir));
        write_file(font_path);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_OK);
        check = find_preflight_check(&wiz->wizard.preflight, "SYSTEM_FONTS");
        assert(check && check->status == PREFLIGHT_OK);

        executable_report.selected = NK_ISO_EXEC_SELECTION_NONE;
        executable_report.boot_fallback = false;
        executable_report.boot.kind = NK_ISO_EXEC_UNKNOWN;
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "EXECUTABLE");
        assert(check && check->status == PREFLIGHT_UNSUPPORTED);
        assert(strcmp(check->message,
                      "Encrypted executable. Decryption support is in the works (#295).") == 0);
        assert(check->issue_count == 1 && check->issue_numbers[0] == 295);

        wiz->inspecting_game.is_experimental = true;
        snprintf(wiz->inspecting_game.disc_id, sizeof(wiz->inspecting_game.disc_id),
                 "ULUS99998");
        snprintf(wiz->inspecting_game.selected_executable,
                 sizeof(wiz->inspecting_game.selected_executable), "EBOOT.BIN");
        snprintf(wiz->inspecting_game.title_id, sizeof(wiz->inspecting_game.title_id),
                 "experimental-ulus99998");
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        assert(wiz->wizard.preflight.count == 6);
        check = find_preflight_check(&wiz->wizard.preflight, "EXPERIMENTAL");
        assert(check && check->status == PREFLIGHT_IN_PROGRESS);
        assert(strstr(check->message,
                      "Experimental: this game has not been verified. Compatibility is unknown.") != NULL);
        assert(check->issue_count == 2 && check->issue_numbers[0] == 285 &&
               check->issue_numbers[1] == 308);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_MISSING);
        assert(check->issue_count == 2 && check->issue_numbers[0] == 296 &&
               check->issue_numbers[1] == 297);

        /* Native v1 package checks bind the profile executable hash, the
           player ABI, and a package-contained executable path. */
        char experimental_package_dir[900], experimental_package_json[1100];
        char experimental_report[1100], experimental_exe[1100], experimental_image[1100];
        char experimental_root[900], profile_dir[1000], profile_path[1200];
        snprintf(experimental_root, sizeof(experimental_root), "%s%cpackages%cULUS99998",
                 preflight_root, nk_platform_path_separator(), nk_platform_path_separator());
        snprintf(experimental_package_dir, sizeof(experimental_package_dir), "%s",
                 experimental_root);
        snprintf(experimental_package_json, sizeof(experimental_package_json), "%s%cpackage.json",
                 experimental_package_dir, nk_platform_path_separator());
        snprintf(experimental_report, sizeof(experimental_report), "%s%cbuild-report.json",
                 experimental_package_dir, nk_platform_path_separator());
        snprintf(experimental_exe, sizeof(experimental_exe), "%s%cexperimental-ulus99998.exe",
                 experimental_package_dir, nk_platform_path_separator());
        snprintf(experimental_image, sizeof(experimental_image), "%s%cexperimental-ulus99998_image.bin",
                 experimental_package_dir, nk_platform_path_separator());
        snprintf(profile_dir, sizeof(profile_dir), "%s%cexperimental%cULUS99998",
                 preflight_root, nk_platform_path_separator(), nk_platform_path_separator());
        snprintf(profile_path, sizeof(profile_path), "%s%cprofile.json", profile_dir,
                 nk_platform_path_separator());
        write_experimental_profile_fixture(preflight_root, "ULUS99998",
                                           "experimental-ulus99998", "EBOOT.BIN",
                                           FIXTURE_SHA256);
        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 2,
                                      "experimental-ulus99998.exe", FIXTURE_SHA256);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_OK);

        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 2,
                                      "experimental-ulus99998.exe",
                                      "0000000000000000000000000000000000000000000000000000000000000000");
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_STALE);

        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 99,
                                      "experimental-ulus99998.exe", FIXTURE_SHA256);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_INCOMPATIBLE);

        write_runtime_package_fixture(preflight_root, "ULUS99998",
                                      "experimental-ulus99998", 2,
                                      "../escape.exe", FIXTURE_SHA256);
        player_app_build_compatibility_preflight(wiz, true, true, &executable_report);
        check = find_preflight_check(&wiz->wizard.preflight, "RUNTIME_PACKAGE");
        assert(check && check->status == PREFLIGHT_INCOMPATIBLE);

        remove(experimental_package_json);
        remove(experimental_report);
        remove(experimental_exe);
        remove(experimental_image);
        remove(profile_path);
        remove(package_json);
        remove(package_report);

        memset(&wiz->games[0], 0, sizeof(wiz->games[0]));
        snprintf(wiz->games[0].disc_id, sizeof(wiz->games[0].disc_id), "ULUS99998");
        snprintf(wiz->games[0].title_id, sizeof(wiz->games[0].title_id),
                 "experimental-ulus99998");
        snprintf(wiz->games[0].selected_executable,
                 sizeof(wiz->games[0].selected_executable), "EBOOT.BIN");
        wiz->games[0].is_experimental = true;
        wiz->game_count = 1;
        assert(!player_app_launch_game(wiz, 0));
        assert(strcmp(wiz->last_error.error_code, "RUNTIME_PACKAGE_NOT_READY") == 0);
        assert(strstr(wiz->last_error.message, "build-package") != NULL);
        player_app_set_view(wiz, VIEW_EXPERIMENTAL_TITLE);
        assert(player_app_focus_count(wiz) == 2);

        remove(runtime_exe);
        remove(runtime_image);
        remove(font_path);

        free(wiz);
    }

    free(app);
    printf("[PLAYER_STATE_TEST] ALL PLAYER STATE TESTS PASSED!\n");
    return 0;
}
