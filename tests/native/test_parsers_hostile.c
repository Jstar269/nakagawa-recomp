/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nk_types.h"
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_platform.h"

static void write_test_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (size > 0) {
        fwrite(data, 1, size, f);
    }
    fclose(f);
}

static void test_hostile_library_json(const char *test_dir) {
    char fpath[512];
    NkLibrary lib;

    printf("[HOSTILE_TEST] Testing library.json parser resilience...\n");
    fflush(stdout);

    /* 1. Missing schema_version */
    printf("[HOSTILE_TEST] Subtest 1: missing schema_version\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cbad_schema_missing.json", test_dir, nk_platform_path_separator());
    const char json_no_schema[] = "{\n  \"games\": []\n}";
    write_test_file(fpath, json_no_schema, strlen(json_no_schema));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);

    /* 2. Unsupported schema_version */
    printf("[HOSTILE_TEST] Subtest 2: unsupported schema_version\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cbad_schema_future.json", test_dir, nk_platform_path_separator());
    const char json_future_schema[] = "{\n  \"schema_version\": 999,\n  \"games\": []\n}";
    write_test_file(fpath, json_future_schema, strlen(json_future_schema));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);

    /* 3. Unterminated string literal */
    printf("[HOSTILE_TEST] Subtest 3: unterminated string literal\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cunterminated_str.json", test_dir, nk_platform_path_separator());
    const char json_unterminated[] = "{\n  \"schema_version\": 1,\n  \"games\": [\n    {\n      \"disc_id\": \"TEST00001,\n      \"title_name\": \"test\"\n    }\n  ]\n}";
    write_test_file(fpath, json_unterminated, strlen(json_unterminated));
    /* Should fail closed or load 0 entries without crashing */
    assert(nk_library_load(&lib, fpath) == NK_OK || nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);
    assert(nk_library_count(&lib) == 0);

    /* 4. String longer than destination buffer */
    printf("[HOSTILE_TEST] Subtest 4: long string\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%clong_string.json", test_dir, nk_platform_path_separator());
    char long_buf[4096];
    memset(long_buf, 'A', sizeof(long_buf) - 1);
    long_buf[sizeof(long_buf) - 1] = '\0';
    char json_long[8192];
    snprintf(json_long, sizeof(json_long),
        "{\n  \"schema_version\": 1,\n  \"games\": [\n    {\n      \"disc_id\": \"TEST00001\",\n      \"title_name\": \"%s\",\n      \"disc_version\": \"1.00\"\n    }\n  ]\n}",
        long_buf);
    write_test_file(fpath, json_long, strlen(json_long));
    assert(nk_library_load(&lib, fpath) == NK_OK);
    assert(nk_library_count(&lib) == 1);
    const NkGameEntry *e = nk_library_get(&lib, 0);
    assert(e != NULL);
    assert(strcmp(e->disc_id, "TEST00001") == 0);
    assert(strcmp(e->disc_version, "1.00") == 0);
    assert(strlen(e->title_name) < sizeof(e->title_name));

    /* 5. Trailing corrupted garbage */
    printf("[HOSTILE_TEST] Subtest 5: trailing garbage\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%ctrailing_garbage.json", test_dir, nk_platform_path_separator());
    const char json_garbage[] = "{\n  \"schema_version\": 1,\n  \"games\": []\n} CORRUPT_TRAILING_GARBAGE!!!";
    write_test_file(fpath, json_garbage, strlen(json_garbage));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);

    /* 6. Valid library save & roundtrip */
    printf("[HOSTILE_TEST] Subtest 6: valid roundtrip\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cvalid_roundtrip.json", test_dir, nk_platform_path_separator());
    nk_library_init(&lib);
    NkGameEntry valid_entry;
    memset(&valid_entry, 0, sizeof(valid_entry));
    snprintf(valid_entry.disc_id, sizeof(valid_entry.disc_id), "TEST00005");
    snprintf(valid_entry.title_name, sizeof(valid_entry.title_name), "Phase 5 Valid Fixture");
    snprintf(valid_entry.disc_version, sizeof(valid_entry.disc_version), "1.00");
    valid_entry.is_prepared = true;
    valid_entry.status = NK_STATUS_VERIFIED;
    assert(nk_library_add_or_update(&lib, &valid_entry) == NK_OK);
    assert(nk_library_save(&lib, fpath) == NK_OK);

    NkLibrary loaded_lib;
    assert(nk_library_load(&loaded_lib, fpath) == NK_OK);
    assert(nk_library_count(&loaded_lib) == 1);
    assert(nk_library_find_by_disc_id(&loaded_lib, "TEST00005") != NULL);

    printf("[HOSTILE_TEST] Library JSON tests PASSED!\n");
    fflush(stdout);
}

static void test_hostile_iso_parser(const char *test_dir) {
    char fpath[512];
    NkIsoMetadata meta;

    printf("[HOSTILE_TEST] Testing ISO parser resilience...\n");

    /* 1. Zero-byte file */
    snprintf(fpath, sizeof(fpath), "%s%czero_byte.iso", test_dir, nk_platform_path_separator());
    write_test_file(fpath, "", 0);
    assert(nk_iso_inspect(fpath, &meta) == NK_ERROR_INVALID_ISO);

    /* 2. Truncated ISO (< 16 sectors) */
    snprintf(fpath, sizeof(fpath), "%s%ctruncated.iso", test_dir, nk_platform_path_separator());
    uint8_t small_buf[1024];
    memset(small_buf, 0, sizeof(small_buf));
    write_test_file(fpath, small_buf, sizeof(small_buf));
    assert(nk_iso_inspect(fpath, &meta) == NK_ERROR_INVALID_ISO);

    /* 3. Corrupt PVD magic (17 sectors of zeroes) */
    snprintf(fpath, sizeof(fpath), "%s%cbad_magic.iso", test_dir, nk_platform_path_separator());
    uint8_t pvd_zeroes[17 * 2048];
    memset(pvd_zeroes, 0, sizeof(pvd_zeroes));
    write_test_file(fpath, pvd_zeroes, sizeof(pvd_zeroes));
    assert(nk_iso_inspect(fpath, &meta) == NK_ERROR_INVALID_ISO);

    /* 4. Valid PVD but root directory LBA points outside file (LBA 10000) */
    snprintf(fpath, sizeof(fpath), "%s%chostile_offset.iso", test_dir, nk_platform_path_separator());
    uint8_t hostile_iso[18 * 2048];
    memset(hostile_iso, 0, sizeof(hostile_iso));
    /* Sector 16: PVD */
    uint8_t *pvd = &hostile_iso[16 * 2048];
    pvd[0] = 0x01;
    memcpy(&pvd[1], "CD001", 5);
    memcpy(&pvd[40], "TEST_VOL                        ", 32);
    /* Root dir record at pvd[156] */
    /* Root extent LBA = 10000 (exceeds 18 sectors!) */
    pvd[158] = 0x10;
    pvd[159] = 0x27;
    pvd[160] = 0x00;
    pvd[161] = 0x00;
    /* Root size = 2048 */
    pvd[166] = 0x00;
    pvd[167] = 0x08;
    pvd[168] = 0x00;
    pvd[169] = 0x00;
    write_test_file(fpath, hostile_iso, sizeof(hostile_iso));

    /* Must fail closed gracefully and not crash or attempt huge seek */
    assert(nk_iso_inspect(fpath, &meta) == NK_ERROR_INVALID_ISO);

    /* 5. Extraction with hostile depth */
    char out_file[512];
    snprintf(out_file, sizeof(out_file), "%s%cextracted.bin", test_dir, nk_platform_path_separator());
    const char deep_path[] = "A/B/C/D/E/F/G/H/I/J/K/L/M/N/O/P/Q/R/S/T/U/V/W/X/Y/Z";
    assert(nk_iso_extract_file(fpath, deep_path, out_file) == NK_ERROR_INVALID_ISO);

    printf("[HOSTILE_TEST] ISO parser tests PASSED!\n");
}

int main(void) {
    char test_dir[512];
    assert(nk_platform_get_path(NK_PATH_CACHE, test_dir, sizeof(test_dir)));
    char hostile_dir[600];
    snprintf(hostile_dir, sizeof(hostile_dir), "%s%chostile_tests", test_dir, nk_platform_path_separator());
    assert(nk_platform_mkdir_p(hostile_dir));

    test_hostile_library_json(hostile_dir);
    test_hostile_iso_parser(hostile_dir);

    printf("[HOSTILE_TEST] ALL HOSTILE/MALFORMED PARSER TESTS COMPLETED SUCCESSFULLY!\n");
    return 0;
}
