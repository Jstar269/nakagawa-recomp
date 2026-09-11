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
    printf("[NATIVE_TEST] Verifying public title catalog count...\n");
    assert(nk_title_catalog_count == 3);

    printf("[NATIVE_TEST] Verifying public source-owned title lookups...\n");
    const NkTitleEntry *t_synth1 = nk_title_catalog_find_by_disc_id("TEST00001");
    assert(t_synth1 != NULL);
    assert(strcmp(t_synth1->id, "synthetic-allegrex-v1") == 0);
    assert(t_synth1->kind == NK_TITLE_KIND_SYNTHETIC);

    const NkTitleEntry *t_p5 = nk_title_catalog_find_by_disc_id("TEST00005");
    assert(t_p5 != NULL);
    assert(strcmp(t_p5->id, "pspdev-phase5-v1") == 0);
    assert(t_p5->kind == NK_TITLE_KIND_SYNTHETIC);

    const NkTitleEntry *t_synth2 = nk_title_catalog_find_by_disc_id("TEST00002");
    assert(t_synth2 != NULL);
    assert(strcmp(t_synth2->id, "synthetic-title2-v1") == 0);

    /* Verify normalization (hyphens/spaces) */
    assert(nk_title_catalog_find_by_disc_id("test-00001") == t_synth1);
    assert(nk_title_catalog_find_by_disc_id("  TEST_00005  ") == t_p5);

    printf("[NATIVE_TEST] Verifying retail IDs are NOT present in public catalog...\n");
    assert(nk_title_catalog_find_by_disc_id("UCUS98701") == NULL);
    assert(nk_title_catalog_find_by_disc_id("UCES01402") == NULL);
    assert(nk_title_catalog_find_by_disc_id("ULUS99999") == NULL);
    assert(nk_title_catalog_find_by_id("hst-ucus98701-v1") == NULL);

    printf("[NATIVE_TEST] Verifying in-memory private overlay registration...\n");
    NkTitleEntry private_overlay;
    memset(&private_overlay, 0, sizeof(private_overlay));
    private_overlay.id = "private-local-test-v1";
    private_overlay.display_name = "Local Acceptance Private Title";
    private_overlay.kind = NK_TITLE_KIND_RETAIL;
    private_overlay.primary_disc_id = "UCUS98701";

    /* Before registration: NULL */
    assert(nk_title_catalog_find_by_disc_id("UCUS98701") == NULL);

    /* Register overlay */
    nk_title_catalog_register_overlay(&private_overlay);
    const NkTitleEntry *found_ov = nk_title_catalog_find_by_disc_id("UCUS-98701");
    assert(found_ov != NULL);
    assert(strcmp(found_ov->id, "private-local-test-v1") == 0);
    assert(nk_title_catalog_find_by_id("private-local-test-v1") == found_ov);

    /* Clear overlay */
    nk_title_catalog_clear_overlay();
    assert(nk_title_catalog_find_by_disc_id("UCUS98701") == NULL);
    assert(nk_title_catalog_find_by_id("private-local-test-v1") == NULL);

    printf("[NATIVE_TEST] Verifying library in-memory operations...\n");
    NkLibrary lib;
    nk_library_init(&lib);
    assert(nk_library_count(&lib) == 0);

    NkGameEntry g1;
    memset(&g1, 0, sizeof(g1));
    snprintf(g1.disc_id, sizeof(g1.disc_id), "TEST00001");
    snprintf(g1.title_name, sizeof(g1.title_name), "%s", t_synth1->display_name);
    snprintf(g1.title_id, sizeof(g1.title_id), "%s", t_synth1->id);
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
    return 0;
}
