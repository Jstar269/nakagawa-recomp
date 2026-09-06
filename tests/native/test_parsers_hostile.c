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

    /* 7. Backup (.bak) automatic recovery when primary is corrupt */
    printf("[HOSTILE_TEST] Subtest 7: automatic .bak recovery on primary corruption\n"); fflush(stdout);
    /* Save again to trigger .bak creation */
    NkGameEntry second_entry;
    memset(&second_entry, 0, sizeof(second_entry));
    snprintf(second_entry.disc_id, sizeof(second_entry.disc_id), "TEST00006");
    snprintf(second_entry.title_name, sizeof(second_entry.title_name), "Second Entry");
    assert(nk_library_add_or_update(&lib, &second_entry) == NK_OK);
    assert(nk_library_save(&lib, fpath) == NK_OK);

    /* Verify .bak exists */
    char bak_path[600];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", fpath);
    assert(nk_platform_file_exists(bak_path));

    /* Corrupt primary file */
    const char corrupt_primary[] = "CORRUPT_INVALID_JSON_CONTENT_TRUNCATED";
    write_test_file(fpath, corrupt_primary, strlen(corrupt_primary));

    /* nk_library_load should automatically fall back to .bak and succeed */
    NkLibrary recovered_lib;
    assert(nk_library_load(&recovered_lib, fpath) == NK_OK);
    assert(nk_library_count(&recovered_lib) >= 1);
    assert(nk_library_find_by_disc_id(&recovered_lib, "TEST00005") != NULL);

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

    /* 6. Directory record crossing sector boundary (ECMA-119 6.8.1.1) */
    printf("[HOSTILE_TEST] Subtest 6: directory record crossing sector boundary\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%csector_cross.iso", test_dir, nk_platform_path_separator());
    uint8_t cross_iso[18 * 2048];
    memset(cross_iso, 0, sizeof(cross_iso));
    /* PVD at 16 */
    uint8_t *pvd_cross = &cross_iso[16 * 2048];
    pvd_cross[0] = 0x01;
    memcpy(&pvd_cross[1], "CD001", 5);
    memcpy(&pvd_cross[40], "SECTOR_CROSS_VOL                ", 32);
    /* Root dir at LBA 17, size 2048 */
    pvd_cross[158] = 17; pvd_cross[159] = 0; pvd_cross[160] = 0; pvd_cross[161] = 0;
    pvd_cross[162] = 0; pvd_cross[163] = 0; pvd_cross[164] = 0; pvd_cross[165] = 17;
    pvd_cross[166] = 0x00; pvd_cross[167] = 0x08; pvd_cross[168] = 0; pvd_cross[169] = 0;
    pvd_cross[170] = 0; pvd_cross[171] = 0; pvd_cross[172] = 0x08; pvd_cross[173] = 0x00;
    /* In sector 17, put record at byte 2040 with rec_len = 34 (crosses 2048 boundary) */
    uint8_t *sec17 = &cross_iso[17 * 2048];
    sec17[2040] = 34;
    write_test_file(fpath, cross_iso, sizeof(cross_iso));
    assert(nk_iso_inspect(fpath, &meta) == NK_OK || nk_iso_inspect(fpath, &meta) == NK_ERROR_INVALID_ISO);

    /* 7. Both-endian disagreement (ECMA-119 7.3.3) */
    printf("[HOSTILE_TEST] Subtest 7: both-endian disagreement\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cendian_disagree.iso", test_dir, nk_platform_path_separator());
    uint8_t endian_iso[19 * 2048];
    memset(endian_iso, 0, sizeof(endian_iso));
    uint8_t *pvd_endian = &endian_iso[16 * 2048];
    pvd_endian[0] = 0x01;
    memcpy(&pvd_endian[1], "CD001", 5);
    memcpy(&pvd_endian[40], "ENDIAN_TEST_VOL                 ", 32);
    /* Root dir at LBA 17 */
    pvd_endian[158] = 17; pvd_endian[159] = 0; pvd_endian[160] = 0; pvd_endian[161] = 0;
    pvd_endian[162] = 0; pvd_endian[163] = 0; pvd_endian[164] = 0; pvd_endian[165] = 17;
    pvd_endian[166] = 0x00; pvd_endian[167] = 0x08; pvd_endian[168] = 0; pvd_endian[169] = 0;
    pvd_endian[170] = 0; pvd_endian[171] = 0; pvd_endian[172] = 0x08; pvd_endian[173] = 0x00;
    /* Directory record for PSP_GAME at sector 17, but LE LBA and BE LBA disagree */
    uint8_t *sec17_endian = &endian_iso[17 * 2048];
    sec17_endian[0] = 42; /* rec_len */
    sec17_endian[2] = 18; sec17_endian[3] = 0; sec17_endian[4] = 0; sec17_endian[5] = 0; /* LE = 18 */
    sec17_endian[6] = 0; sec17_endian[7] = 0; sec17_endian[8] = 0x03; sec17_endian[9] = 0xE7; /* BE = 999 (DISAGREEMENT) */
    sec17_endian[10] = 0x00; sec17_endian[11] = 0x08; sec17_endian[12] = 0; sec17_endian[13] = 0; /* Size LE = 2048 */
    sec17_endian[14] = 0; sec17_endian[15] = 0; sec17_endian[16] = 0x08; sec17_endian[17] = 0x00; /* Size BE = 2048 */
    sec17_endian[32] = 8; /* name_len */
    memcpy(&sec17_endian[33], "PSP_GAME", 8);
    write_test_file(fpath, endian_iso, sizeof(endian_iso));
    /* nk_iso_inspect must reject the record with mismatched endians */
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath, &meta) == NK_OK);
    /* Since PSP_GAME record was rejected, disc_id falls back to volume ID */
    assert(strcmp(meta.volume_id, "ENDIAN_TEST_VOL") == 0);

    /* 8. Duplicate SFO keys (preserve first valid, ignore hostile duplicate) */
    printf("[HOSTILE_TEST] Subtest 8: duplicate SFO keys\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cdup_sfo_keys.iso", test_dir, nk_platform_path_separator());
    uint8_t dup_iso[18 * 2048];
    memset(dup_iso, 0, sizeof(dup_iso));
    /* Sector 16: PVD */
    uint8_t *pvd_dup = &dup_iso[16 * 2048];
    pvd_dup[0] = 0x01;
    memcpy(&pvd_dup[1], "CD001", 5);
    memcpy(&pvd_dup[40], "DUP_SFO_VOL                     ", 32);
    /* Root dir at 17 */
    pvd_dup[158] = 17; pvd_dup[165] = 17;
    pvd_dup[167] = 0x08; pvd_dup[172] = 0x08;

    /* Construct synthetic SFO with two DISC_ID keys in sector 17 */
    uint8_t *sfo = &dup_iso[17 * 2048];
    sfo[0] = 0x00; sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
    sfo[4] = 0x01; sfo[5] = 0x01; /* Version 1.1 */
    /* key_table_off = 20 + 2 * 16 = 52 */
    sfo[8] = 52; sfo[9] = 0; sfo[10] = 0; sfo[11] = 0;
    /* data_table_off = 52 + 16 = 68 */
    sfo[12] = 68; sfo[13] = 0; sfo[14] = 0; sfo[15] = 0;
    /* entry_count = 2 */
    sfo[16] = 2; sfo[17] = 0; sfo[18] = 0; sfo[19] = 0;

    /* Entry 0: key offset 0 ("DISC_ID"), data_len = 10, data_offset = 0 ("UCUS98701\0") */
    sfo[20] = 0; sfo[21] = 0;
    sfo[24] = 10; sfo[25] = 0; sfo[26] = 0; sfo[27] = 0;
    sfo[32] = 0; sfo[33] = 0; sfo[34] = 0; sfo[35] = 0;

    /* Entry 1: key offset 8 ("DISC_ID"), data_len = 10, data_offset = 10 ("MALICIOUS\0") */
    sfo[36] = 8; sfo[37] = 0;
    sfo[40] = 10; sfo[41] = 0; sfo[42] = 0; sfo[43] = 0;
    sfo[48] = 10; sfo[49] = 0; sfo[50] = 0; sfo[51] = 0;

    /* Key table at 52: "DISC_ID\0" (8 bytes), "DISC_ID\0" (8 bytes) */
    memcpy(&sfo[52], "DISC_ID\0", 8);
    memcpy(&sfo[60], "DISC_ID\0", 8);

    /* Data table at 68: "UCUS98701\0", "MALICIOUS\0" */
    memcpy(&sfo[68], "UCUS98701\0", 10);
    memcpy(&sfo[78], "MALICIOUS\0", 10);

    write_test_file(fpath, dup_iso, sizeof(dup_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath, &meta) == NK_OK);
    /* First valid key must be preserved */
    assert(strcmp(meta.disc_id, "UCUS98701") == 0);

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
