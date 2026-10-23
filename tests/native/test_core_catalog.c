/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Feature-test macro first: the per-run cache root uses POSIX setenv, mkdir,
 * open, fdopendir, and getpid on non-Windows hosts. */
#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "nk_types.h"
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_font.h"
#include "nk_launch.h"
#include "nk_platform.h"
#include "generated/nk_title_catalog.h"
#include "native_test_isolation.h"

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

static void test_nk_library_init_and_null_guards(void) {
    printf("[NATIVE_TEST] Testing nk_library init and NULL guards...\n");

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
}

static void test_nk_library_add_update_remove_bounds(void) {
    printf("[NATIVE_TEST] Testing nk_library add, update, remove, and bounds...\n");

    NkLibrary lib;
    nk_library_init(&lib);

    NkGameEntry g1, g2;
    seed_game_entry(&g1, "TEST00001", "Game One");
    seed_game_entry(&g2, "TEST00002", "Game Two");

    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_count(&lib) == 1);

    assert(nk_library_add_or_update(&lib, &g2) == NK_OK);
    assert(nk_library_count(&lib) == 2);

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
    assert(nk_library_count(&lib) == 2);

    p1 = nk_library_find_by_disc_id(&lib, "TEST00001");
    assert(p1 != NULL);
    assert(strcmp(p1->title_name, "Game One Updated") == 0);
    assert(p1->status == NK_STATUS_PLAYABLE);

    /* Removal */
    assert(nk_library_remove(&lib, "NONEXISTENT") == NK_ERROR_FILE_NOT_FOUND);
    assert(nk_library_count(&lib) == 2);

    assert(nk_library_remove(&lib, "TEST00001") == NK_OK);
    assert(nk_library_count(&lib) == 1);
    assert(nk_library_find_by_disc_id(&lib, "TEST00001") == NULL);

    const NkGameEntry *rem = nk_library_get(&lib, 0);
    assert(rem != NULL);
    assert(strcmp(rem->disc_id, "TEST00002") == 0);

    assert(nk_library_remove(&lib, "TEST00002") == NK_OK);
    assert(nk_library_count(&lib) == 0);

    /* Capacity bounds */
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
}

static void test_nk_library_save_load_escaping_and_samples(void) {
    printf("[NATIVE_TEST] Testing nk_library save, load, escaping, and sample filtering...\n");

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
    g2.is_sample = true; /* Samples must NOT be written to disk */

    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_add_or_update(&lib, &g2) == NK_OK);
    assert(nk_library_count(&lib) == 2);

    assert(nk_library_save(&lib, file_path) == NK_OK);
    assert(nk_platform_file_exists(file_path));

    NkLibrary loaded;
    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, file_path) == NK_OK);

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
}

static void test_nk_library_corrupt_load_and_backup_recovery(void) {
    printf("[NATIVE_TEST] Testing nk_library corrupt load handling and backup recovery...\n");

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
    seed_game_entry(&g1, "TEST00001", "Valid Game V1");
    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);

    assert(nk_library_save(&lib, primary_path) == NK_OK);
    assert(nk_platform_file_exists(primary_path));

    snprintf(g1.title_name, sizeof(g1.title_name), "Valid Game V2");
    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_save(&lib, primary_path) == NK_OK);
    assert(nk_platform_file_exists(primary_path));
    assert(nk_platform_file_exists(bak_path));

    /* Corrupt primary file */
    FILE *f = fopen(primary_path, "wb");
    assert(f != NULL);
    fputs("{ \"schema_version\": 1, \"games\": [ corrupt_json ", f);
    fclose(f);

    NkLibrary loaded;
    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, primary_path) == NK_OK);
    assert(nk_library_count(&loaded) == 1);

    const NkGameEntry *lg = nk_library_find_by_disc_id(&loaded, "TEST00001");
    assert(lg != NULL);
    assert(strcmp(lg->title_name, "Valid Game V1") == 0);

    /* Corrupt both primary and backup */
    f = fopen(primary_path, "wb");
    assert(f != NULL);
    fputs("{ \"schema_version\": 999 }", f);
    fclose(f);

    f = fopen(bak_path, "wb");
    assert(f != NULL);
    fputs("NOT_JSON", f);
    fclose(f);

    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, primary_path) != NK_OK);
    assert(nk_library_count(&loaded) == 0);

    /* Primary missing but backup valid */
    remove(primary_path);
    assert(nk_library_save(&lib, bak_path) == NK_OK);
    remove(primary_path);

    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, primary_path) == NK_OK);
    assert(nk_library_count(&loaded) == 1);
}

static void test_nk_font_classify_verdict(void) {
    printf("[NATIVE_TEST] Testing nk_font_classify_verdict...\n");
    char reason[128];
    PgfVerdict verdict;

    /* 1. NULL verdict pointer */
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(NULL, reason, sizeof(reason)) == -1);
    assert(strcmp(reason, "the reader refused the file") == 0);

    /* 2. Non-zero refusal status */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_MAGIC;
    verdict.has_latin = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == -1);
    assert(strcmp(reason, "the reader refused the file") == 0);

    /* 3. Ambiguous verdict with both has_hangul and has_kana */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    verdict.has_hangul = 1;
    verdict.has_kana = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == -1);
    assert(strcmp(reason, "covers both Hangul and kana, so it names no slot") == 0);

    /* 4. Korean slot classification (has_hangul = 1) */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    verdict.has_hangul = 1;
    verdict.has_latin = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == NK_FONT_SLOT_KOREAN);

    /* 5. Japanese slot classification (has_kana = 1) */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    verdict.has_kana = 1;
    verdict.has_latin = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == NK_FONT_SLOT_JAPANESE);

    /* 6. Latin slot classification (has_latin = 1) */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    verdict.has_latin = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == NK_FONT_SLOT_LATIN);

    /* 7. No probe flags set */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == -1);
    assert(strcmp(reason, "covers none of the slot probe characters") == 0);

    /* 8. Null / zero-length reason buffer edge cases */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    assert(nk_font_classify_verdict(&verdict, NULL, 0) == -1);
    assert(nk_font_classify_verdict(&verdict, reason, 0) == -1);

    /* 9. Truncated reason buffer */
    char small_buf[5];
    assert(nk_font_classify_verdict(&verdict, small_buf, sizeof(small_buf)) == -1);
    assert(strcmp(small_buf, "cove") == 0);
}

int main(void) {
    native_test_create_root("core-catalog");
    native_test_isolate_user_data_roots();
    native_test_assert_cache_root_isolated();
    native_test_assert_config_root_isolated();

    test_nk_library_init_and_null_guards();
    test_nk_library_add_update_remove_bounds();
    test_nk_library_save_load_escaping_and_samples();
    test_nk_library_corrupt_load_and_backup_recovery();
    test_nk_font_classify_verdict();

    /* Catalog SIZE is deliberately not asserted here. It is a literal that every
       new public title has to come back and edit, and it never caught anything:
       tools/title_catalog_codegen.py --verify and the codegen drift test already
       prove the catalog matches assets/titles/ exactly. What the native side can
       usefully check is that the table it compiled is internally coherent. */
    printf("[NATIVE_TEST] Verifying public title catalog integrity...\n");
    assert(nk_title_catalog_count > 0);
    for (int i = 0; i < nk_title_catalog_count; i++) {
        const NkTitleEntry *e = &nk_title_catalog_entries[i];
        assert(e->id != NULL && e->id[0] != '\0');
        assert(e->game_name != NULL && e->game_name[0] != '\0');
        assert(e->display_name != NULL && e->display_name[0] != '\0');
        assert(e->primary_disc_id != NULL && e->primary_disc_id[0] != '\0');
        /* Every disc id resolves, and resolves to THIS entry. Two entries sharing
           a disc id used to be reachable: unassigned synthetic titles all shared a
           "TEST00000" sentinel, so lookup silently returned whichever came first. */
        NkTitleEntrySnapshot by_disc = {0};
        NkTitleEntrySnapshot by_id = {0};
        assert(nk_title_catalog_find_by_disc_id(e->primary_disc_id, &by_disc));
        assert(nk_title_catalog_find_by_id(e->id, &by_id));
        assert(strcmp(by_disc.entry.id, e->id) == 0);
        assert(strcmp(by_id.entry.id, e->id) == 0);
        nk_title_catalog_snapshot_release(&by_disc);
        nk_title_catalog_snapshot_release(&by_id);
    }

    printf("[NATIVE_TEST] Verifying public source-owned title lookups...\n");
    NkTitleEntrySnapshot t_synth1 = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00001", &t_synth1));
    assert(strcmp(t_synth1.entry.id, "synthetic-allegrex-v1") == 0);
    assert(t_synth1.entry.kind == NK_TITLE_KIND_SYNTHETIC);

    NkTitleEntrySnapshot t_p5 = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00005", &t_p5));
    assert(strcmp(t_p5.entry.id, "pspdev-phase5-v1") == 0);
    assert(t_p5.entry.kind == NK_TITLE_KIND_SYNTHETIC);

    NkTitleEntrySnapshot t_synth2 = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00002", &t_synth2));
    assert(strcmp(t_synth2.entry.id, "synthetic-title2-v1") == 0);

    /* The display fixture is the one public title built under the layout
       src/core/nk_launch.c can actually resolve, so its addresses are load
       bearing for the launch path, not just for the catalog. */
    NkTitleEntrySnapshot t_disp = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00006", &t_disp));
    assert(strcmp(t_disp.entry.id, "display-smoke-v1") == 0);
    assert(t_disp.entry.kind == NK_TITLE_KIND_SYNTHETIC);
    assert(strcmp(t_disp.entry.game_name, "display-smoke") == 0);
    assert(t_disp.entry.executable_base == 0x08810000u);
    assert(t_disp.entry.executable_entry == 0x08810000u);

    /* Verify normalization (hyphens/spaces) */
    NkTitleEntrySnapshot normalized = {0};
    assert(nk_title_catalog_find_by_disc_id("test-00001", &normalized));
    assert(strcmp(normalized.entry.id, t_synth1.entry.id) == 0);
    nk_title_catalog_snapshot_release(&normalized);
    assert(nk_title_catalog_find_by_disc_id("  TEST_00005  ", &normalized));
    assert(strcmp(normalized.entry.id, t_p5.entry.id) == 0);
    nk_title_catalog_snapshot_release(&normalized);

    printf("[NATIVE_TEST] Verifying retail IDs are NOT present in public catalog...\n");
    NkTitleEntrySnapshot missing = {0};
    assert(!nk_title_catalog_find_by_disc_id("UCUS98701", &missing));
    assert(!nk_title_catalog_find_by_disc_id("UCES01402", &missing));
    assert(!nk_title_catalog_find_by_disc_id("ULUS99999", &missing));
    assert(!nk_title_catalog_find_by_id("hst-ucus98701-v1", &missing));

    printf("[NATIVE_TEST] Verifying in-memory private overlay registration...\n");
    NkTitleEntry private_overlay;
    memset(&private_overlay, 0, sizeof(private_overlay));
    private_overlay.id = "private-local-test-v1";
    private_overlay.display_name = "Local Acceptance Private Title";
    private_overlay.kind = NK_TITLE_KIND_RETAIL;
    private_overlay.primary_disc_id = "UCUS98701";

    /* Before registration: NULL */
    assert(!nk_title_catalog_find_by_disc_id("UCUS98701", &missing));

    /* Register overlay */
    nk_title_catalog_register_overlay(&private_overlay);
    NkTitleEntrySnapshot found_ov = {0};
    NkTitleEntrySnapshot found_ov_by_id = {0};
    assert(nk_title_catalog_find_by_disc_id("UCUS-98701", &found_ov));
    assert(strcmp(found_ov.entry.id, "private-local-test-v1") == 0);
    assert(nk_title_catalog_find_by_id("private-local-test-v1",
                                       &found_ov_by_id));
    assert(strcmp(found_ov_by_id.entry.id, found_ov.entry.id) == 0);

    /* Clear overlay */
    nk_title_catalog_clear_overlay();
    assert(strcmp(found_ov.entry.display_name,
                  "Local Acceptance Private Title") == 0);
    assert(!nk_title_catalog_find_by_disc_id("UCUS98701", &missing));
    assert(!nk_title_catalog_find_by_id("private-local-test-v1", &missing));

    printf("[NATIVE_TEST] Verifying library in-memory operations...\n");
    NkLibrary lib;
    nk_library_init(&lib);
    assert(nk_library_count(&lib) == 0);

    NkGameEntry g1;
    memset(&g1, 0, sizeof(g1));
    snprintf(g1.disc_id, sizeof(g1.disc_id), "TEST00001");
    snprintf(g1.title_name, sizeof(g1.title_name), "%s", t_synth1.entry.display_name);
    snprintf(g1.title_id, sizeof(g1.title_id), "%s", t_synth1.entry.id);
    g1.status = NK_STATUS_VERIFIED;
    g1.is_prepared = true;

    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_count(&lib) == 1);
    assert(nk_library_find_by_disc_id(&lib, "TEST00001") != NULL);

    assert(nk_library_remove(&lib, "TEST00001") == NK_OK);
    assert(nk_library_count(&lib) == 0);

    printf("[NATIVE_TEST] Verifying structured data directory routing...\n");
    char path_buf[512];
    assert(nk_platform_get_path(NK_PATH_CONFIG, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_DATA, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_CACHE, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_LOGS, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_SAVES, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    printf("[NATIVE_TEST] Verifying Unicode directory & file handling...\n");
    char cache_dir[512];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));

    /* Test 1: Japanese characters (日本語) */
    char u_jp[600];
    snprintf(u_jp, sizeof(u_jp), "%s%c%s", cache_dir, nk_platform_path_separator(), "テスト_日本語_ディレクトリ");
    assert(nk_platform_mkdir_p(u_jp));
    assert(nk_platform_dir_exists(u_jp));

    /* Test 2: Latin accents and spaces */
    char u_latin[600];
    snprintf(u_latin, sizeof(u_latin), "%s%c%s", cache_dir, nk_platform_path_separator(), "Carpeta con Espacios y Acentos áéíóú");
    assert(nk_platform_mkdir_p(u_latin));
    assert(nk_platform_dir_exists(u_latin));

    /* Test 3: Emoji directory */
    char u_emoji[600];
    snprintf(u_emoji, sizeof(u_emoji), "%s%c%s", cache_dir, nk_platform_path_separator(), "🎮_PSP_Game_Directory");
    assert(nk_platform_mkdir_p(u_emoji));
    assert(nk_platform_dir_exists(u_emoji));

    /* Test 4: File existence in Unicode directory */
    char u_file[700];
    snprintf(u_file, sizeof(u_file), "%s%c%s", u_jp, nk_platform_path_separator(), "テスト_file.txt");
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wfile[1024];
    MultiByteToWideChar(CP_UTF8, 0, u_file, -1, wfile, 1024);
    FILE *f_uni = _wfopen(wfile, L"wb");
#else
    FILE *f_uni = fopen(u_file, "wb");
#endif
    assert(f_uni != NULL);
    const char test_data[] = "Unicode content check 12345";
    fwrite(test_data, 1, sizeof(test_data), f_uni);
    fclose(f_uni);

    assert(nk_platform_file_exists(u_file));
    assert(nk_platform_get_file_size(u_file) == (int64_t)sizeof(test_data));

    printf("[NATIVE_TEST] Verifying nk_platform_get_file_size edge cases...\n");
    /* NULL path returns -1 */
    assert(nk_platform_get_file_size(NULL) == -1);

    /* Non-existent path returns -1 */
    char missing_file_path[700];
    snprintf(missing_file_path, sizeof(missing_file_path), "%s%c%s", cache_dir, nk_platform_path_separator(), "non_existent_file_12345.dat");
    assert(nk_platform_get_file_size(missing_file_path) == -1);

    /* Zero-byte file returns 0 */
    char zero_file_path[700];
    snprintf(zero_file_path, sizeof(zero_file_path), "%s%c%s", cache_dir, nk_platform_path_separator(), "zero_byte_file.dat");
    FILE *f_zero = nk_fopen_utf8(zero_file_path, "wb");
    assert(f_zero != NULL);
    fclose(f_zero);
    assert(nk_platform_get_file_size(zero_file_path) == 0);

    printf("[NATIVE_TEST] ALL CORE NATIVE C TESTS PASSED SUCCESSFULLY!\n");
    nk_title_catalog_snapshot_release(&found_ov);
    nk_title_catalog_snapshot_release(&found_ov_by_id);
    nk_title_catalog_snapshot_release(&t_synth1);
    nk_title_catalog_snapshot_release(&t_p5);
    nk_title_catalog_snapshot_release(&t_synth2);
    nk_title_catalog_snapshot_release(&t_disp);
    native_test_assert_real_config_untouched();
    return 0;
}
