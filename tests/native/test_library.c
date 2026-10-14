/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_test_isolation.h"
#include "nk_library.h"
#include "nk_platform.h"
#include "nk_types.h"

static void seed_game_entry(NkGameEntry *entry, const char *disc_id, const char *title) {
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->disc_id, sizeof(entry->disc_id), "%s", disc_id);
    snprintf(entry->title_name, sizeof(entry->title_name), "%s", title);
    snprintf(entry->disc_version, sizeof(entry->disc_version), "1.00");
    snprintf(entry->title_id, sizeof(entry->title_id), "%s-id", disc_id);
    snprintf(entry->iso_path, sizeof(entry->iso_path), "games/%s.iso", disc_id);
    snprintf(entry->boot_executable, sizeof(entry->boot_executable), "EBOOT.BIN");
    entry->status = NK_STATUS_IDENTIFIED;
    entry->iso_size_bytes = 1048576;
}

static void test_library_init_and_null_guards(void) {
    printf("[LIBRARY_TEST] Subtest 1: init and NULL guards...\n");

    NkLibrary lib;
    memset(&lib, 0xFF, sizeof(lib));
    nk_library_init(&lib);

    assert(lib.count == 0);
    assert(lib.library_path[0] == '\0');
    assert(nk_library_count(&lib) == 0);
    assert(nk_library_count(NULL) == 0);

    assert(nk_library_get(&lib, 0) == NULL);
    assert(nk_library_get(&lib, -1) == NULL);
    assert(nk_library_get(NULL, 0) == NULL);

    assert(nk_library_find_by_disc_id(&lib, "TEST00001") == NULL);
    assert(nk_library_find_by_disc_id(&lib, "") == NULL);
    assert(nk_library_find_by_disc_id(&lib, NULL) == NULL);
    assert(nk_library_find_by_disc_id(NULL, "TEST00001") == NULL);

    NkGameEntry entry;
    seed_game_entry(&entry, "TEST00001", "Test Title");

    assert(nk_library_add_or_update(NULL, &entry) == NK_ERROR_GENERIC);
    assert(nk_library_add_or_update(&lib, NULL) == NK_ERROR_GENERIC);

    NkGameEntry empty_id;
    memset(&empty_id, 0, sizeof(empty_id));
    assert(nk_library_add_or_update(&lib, &empty_id) == NK_ERROR_GENERIC);

    assert(nk_library_remove(NULL, "TEST00001") == NK_ERROR_GENERIC);
    assert(nk_library_remove(&lib, NULL) == NK_ERROR_GENERIC);
    assert(nk_library_remove(&lib, "") == NK_ERROR_GENERIC);

    assert(nk_library_save(NULL, "path.json") == NK_ERROR_GENERIC);
    assert(nk_library_load(NULL, "path.json") == NK_ERROR_GENERIC);

    nk_library_init(NULL); /* Should not crash */

    printf("[LIBRARY_TEST] Subtest 1 PASSED!\n");
}

static void test_library_add_update_remove(void) {
    printf("[LIBRARY_TEST] Subtest 2: add, in-place update, capacity limit, and remove...\n");

    NkLibrary lib;
    nk_library_init(&lib);

    NkGameEntry g1, g2;
    seed_game_entry(&g1, "TEST00001", "Game One");
    seed_game_entry(&g2, "TEST00002", "Game Two");

    /* Add entries */
    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_count(&lib) == 1);

    assert(nk_library_add_or_update(&lib, &g2) == NK_OK);
    assert(nk_library_count(&lib) == 2);

    /* Verify getter and lookup */
    const NkGameEntry *p1 = nk_library_find_by_disc_id(&lib, "TEST00001");
    assert(p1 != NULL);
    assert(strcmp(p1->title_name, "Game One") == 0);

    const NkGameEntry *idx0 = nk_library_get(&lib, 0);
    assert(idx0 == p1);

    const NkGameEntry *p2 = nk_library_find_by_disc_id(&lib, "TEST00002");
    assert(p2 != NULL);
    assert(strcmp(p2->title_name, "Game Two") == 0);

    /* In-place update */
    NkGameEntry g1_updated = g1;
    snprintf(g1_updated.title_name, sizeof(g1_updated.title_name), "Game One Updated");
    g1_updated.status = NK_STATUS_PLAYABLE;

    assert(nk_library_add_or_update(&lib, &g1_updated) == NK_OK);
    assert(nk_library_count(&lib) == 2); /* Count unchanged */

    p1 = nk_library_find_by_disc_id(&lib, "TEST00001");
    assert(p1 != NULL);
    assert(strcmp(p1->title_name, "Game One Updated") == 0);
    assert(p1->status == NK_STATUS_PLAYABLE);

    /* Test removal */
    assert(nk_library_remove(&lib, "NONEXISTENT") == NK_ERROR_FILE_NOT_FOUND);
    assert(nk_library_count(&lib) == 2);

    assert(nk_library_remove(&lib, "TEST00001") == NK_OK);
    assert(nk_library_count(&lib) == 1);
    assert(nk_library_find_by_disc_id(&lib, "TEST00001") == NULL);

    /* Remaining entry shifted to index 0 */
    const NkGameEntry *rem = nk_library_get(&lib, 0);
    assert(rem != NULL);
    assert(strcmp(rem->disc_id, "TEST00002") == 0);

    assert(nk_library_remove(&lib, "TEST00002") == NK_OK);
    assert(nk_library_count(&lib) == 0);

    /* Capacity limit */
    for (int i = 0; i < NK_MAX_GAMES; i++) {
        char disc_id[NK_MAX_DISC_ID_LEN];
        snprintf(disc_id, sizeof(disc_id), "FULL%05d", i);
        NkGameEntry entry;
        seed_game_entry(&entry, disc_id, "Filler Game");
        assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    }
    assert(nk_library_count(&lib) == NK_MAX_GAMES);

    NkGameEntry overflow;
    seed_game_entry(&overflow, "OVERFLOW01", "Overflow Game");
    assert(nk_library_add_or_update(&lib, &overflow) == NK_ERROR_OUT_OF_MEMORY);
    assert(nk_library_count(&lib) == NK_MAX_GAMES);

    printf("[LIBRARY_TEST] Subtest 2 PASSED!\n");
}

static void test_library_save_and_load_roundtrip(void) {
    printf("[LIBRARY_TEST] Subtest 3: save and load roundtrip with escaping...\n");

    const char *test_root = native_test_get_root();
    char file_path[NATIVE_TEST_PATH_MAX];
    snprintf(file_path, sizeof(file_path), "%s%clibrary_test.json",
             test_root, nk_platform_path_separator());

    NkLibrary lib;
    nk_library_init(&lib);

    NkGameEntry g1, g2;
    seed_game_entry(&g1, "TEST00001", "Title with \"quotes\" & \n newline & \t tab");
    snprintf(g1.last_played, sizeof(g1.last_played), "2026-03-30 12:34:56");
    g1.is_experimental = true;
    g1.executable_eboot_kind = 1;
    g1.executable_boot_kind = 2;
    g1.executable_selection = 1;
    g1.executable_boot_fallback = true;
    g1.is_prepared = true;
    g1.assets_staged = true;
    g1.extracted_asset_count = 10;
    g1.extracted_audio_count = 5;
    g1.extracted_visual_count = 3;
    g1.extracted_layout_count = 2;

    seed_game_entry(&g2, "TEST00002", "Sample Game");
    g2.is_sample = true; /* Should NOT be saved to disk */

    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_add_or_update(&lib, &g2) == NK_OK);
    assert(nk_library_count(&lib) == 2);

    /* Save library */
    assert(nk_library_save(&lib, file_path) == NK_OK);
    assert(nk_platform_file_exists(file_path));

    /* Load into fresh library */
    NkLibrary loaded;
    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, file_path) == NK_OK);

    /* Sample game was skipped during save, so loaded count is 1 */
    assert(nk_library_count(&loaded) == 1);
    assert(strcmp(loaded.library_path, file_path) == 0);

    const NkGameEntry *lg1 = nk_library_find_by_disc_id(&loaded, "TEST00001");
    assert(lg1 != NULL);
    assert(strcmp(lg1->title_name, g1.title_name) == 0);
    assert(strcmp(lg1->disc_version, g1.disc_version) == 0);
    assert(strcmp(lg1->iso_path, g1.iso_path) == 0);
    assert(strcmp(lg1->boot_executable, g1.boot_executable) == 0);
    assert(strcmp(lg1->last_played, g1.last_played) == 0);
    assert(lg1->is_experimental == g1.is_experimental);
    assert(lg1->executable_eboot_kind == g1.executable_eboot_kind);
    assert(lg1->executable_boot_kind == g1.executable_boot_kind);
    assert(lg1->executable_selection == g1.executable_selection);
    assert(lg1->executable_boot_fallback == g1.executable_boot_fallback);
    assert(lg1->is_prepared == g1.is_prepared);
    assert(lg1->assets_staged == g1.assets_staged);
    assert(lg1->extracted_asset_count == g1.extracted_asset_count);
    assert(lg1->extracted_audio_count == g1.extracted_audio_count);
    assert(lg1->extracted_visual_count == g1.extracted_visual_count);
    assert(lg1->extracted_layout_count == g1.extracted_layout_count);

    assert(nk_library_find_by_disc_id(&loaded, "TEST00002") == NULL);

    printf("[LIBRARY_TEST] Subtest 3 PASSED!\n");
}

static void test_library_backup_recovery_and_corrupt_handling(void) {
    printf("[LIBRARY_TEST] Subtest 4: backup recovery and corrupt load handling...\n");

    const char *test_root = native_test_get_root();
    char primary_path[NATIVE_TEST_PATH_MAX];
    char bak_path[NATIVE_TEST_PATH_MAX];

    snprintf(primary_path, sizeof(primary_path), "%s%crecovery_lib.json",
             test_root, nk_platform_path_separator());
    snprintf(bak_path, sizeof(bak_path), "%s%crecovery_lib.json.bak",
             test_root, nk_platform_path_separator());

    NkLibrary lib;
    nk_library_init(&lib);

    NkGameEntry g1;
    seed_game_entry(&g1, "TEST00001", "Valid Game");
    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);

    /* First save: primary file created */
    assert(nk_library_save(&lib, primary_path) == NK_OK);
    assert(nk_platform_file_exists(primary_path));

    /* Update entry and save again: primary updated, .bak created from primary */
    snprintf(g1.title_name, sizeof(g1.title_name), "Valid Game V2");
    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_save(&lib, primary_path) == NK_OK);
    assert(nk_platform_file_exists(primary_path));
    assert(nk_platform_file_exists(bak_path));

    /* Corrupt the primary file */
    FILE *f = fopen(primary_path, "wb");
    assert(f != NULL);
    fputs("{ \"schema_version\": 1, \"games\": [ corrupt_json ", f);
    fclose(f);

    /* Load should detect corruption in primary and recover from .bak */
    NkLibrary loaded;
    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, primary_path) == NK_OK);
    assert(nk_library_count(&loaded) == 1);

    const NkGameEntry *lg = nk_library_find_by_disc_id(&loaded, "TEST00001");
    assert(lg != NULL);
    /* .bak contains V1 title_name */
    assert(strcmp(lg->title_name, "Valid Game") == 0);

    /* Corrupt both primary and backup */
    f = fopen(primary_path, "wb");
    assert(f != NULL);
    fputs("{ \"schema_version\": 999 }", f); /* Invalid schema version */
    fclose(f);

    f = fopen(bak_path, "wb");
    assert(f != NULL);
    fputs("NOT_JSON", f);
    fclose(f);

    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, primary_path) != NK_OK);
    assert(nk_library_count(&loaded) == 0); /* Leaves empty library on failure */

    /* Primary missing but backup valid */
    remove(primary_path);
    assert(nk_library_save(&lib, bak_path) == NK_OK); /* Write valid JSON into bak_path */
    remove(primary_path); /* Ensure primary doesn't exist */

    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, primary_path) == NK_OK);
    assert(nk_library_count(&loaded) == 1);

    printf("[LIBRARY_TEST] Subtest 4 PASSED!\n");
}

int main(void) {
    native_test_create_root("library-test");
    native_test_isolate_user_data_roots();
    native_test_assert_cache_root_isolated();
    native_test_assert_config_root_isolated();

    printf("=================================================================\n");
    printf("Starting Nakagawa Recomp NkLibrary Unit Tests\n");
    printf("=================================================================\n");

    test_library_init_and_null_guards();
    test_library_add_update_remove();
    test_library_save_and_load_roundtrip();
    test_library_backup_recovery_and_corrupt_handling();

    printf("=================================================================\n");
    printf("ALL NK_LIBRARY TESTS PASSED SUCCESSFULLY!\n");
    printf("=================================================================\n");

    native_test_assert_real_config_untouched();
    return 0;
}
