/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nk_types.h"
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_launch.h"
#include "generated/nk_title_catalog.h"
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

int main(void) {
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

    printf("[NATIVE_TEST] ALL CORE NATIVE C TESTS PASSED SUCCESSFULLY!\n");
    nk_title_catalog_snapshot_release(&found_ov);
    nk_title_catalog_snapshot_release(&found_ov_by_id);
    nk_title_catalog_snapshot_release(&t_synth1);
    nk_title_catalog_snapshot_release(&t_p5);
    nk_title_catalog_snapshot_release(&t_synth2);
    nk_title_catalog_snapshot_release(&t_disp);
    return 0;
}
