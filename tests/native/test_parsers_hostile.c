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

static void fill_dir_chain(uint8_t *sec, size_t target_off) {
    size_t cur = 0;
    while (cur < target_off) {
        size_t step = target_off - cur;
        if (step > 200) step = 200;
        sec[cur] = (uint8_t)step;
        sec[cur + 32] = 1;
        sec[cur + 33] = '_';
        cur += step;
    }
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

    /* 4b. A malformed member must fail the WHOLE file, not just its entry.
     *
     * The parser used to scan to the entry's closing brace and carry on, so a
     * library whose second game had a garbage boolean loaded as NK_OK with that
     * game silently missing. nk_library_load therefore never reached its .bak
     * recovery, and the next save wrote the truncated library back -- making a
     * recoverable corruption permanent. */
    printf("[HOSTILE_TEST] Subtest 4b: malformed entry fails the whole file\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cmalformed_entry.json", test_dir, nk_platform_path_separator());
    const char json_bad_member[] =
        "{\n  \"schema_version\": 1,\n  \"games\": [\n"
        "    {\"disc_id\": \"TEST00001\", \"title_name\": \"first\"},\n"
        "    {\"disc_id\": \"TEST00002\", \"is_prepared\": maybe}\n"
        "  ]\n}";
    write_test_file(fpath, json_bad_member, strlen(json_bad_member));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);
    assert(nk_library_count(&lib) == 0);

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
    NkGameEntry second_entry;
    memset(&second_entry, 0, sizeof(second_entry));
    snprintf(second_entry.disc_id, sizeof(second_entry.disc_id), "TEST00006");
    snprintf(second_entry.title_name, sizeof(second_entry.title_name), "Second Entry");
    assert(nk_library_add_or_update(&lib, &second_entry) == NK_OK);
    assert(nk_library_save(&lib, fpath) == NK_OK);

    char bak_path[600];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", fpath);
    assert(nk_platform_file_exists(bak_path));

    const char corrupt_primary[] = "CORRUPT_INVALID_JSON_CONTENT_TRUNCATED";
    write_test_file(fpath, corrupt_primary, strlen(corrupt_primary));

    NkLibrary recovered_lib;
    assert(nk_library_load(&recovered_lib, fpath) == NK_OK);
    assert(nk_library_count(&recovered_lib) >= 1);
    assert(nk_library_find_by_disc_id(&recovered_lib, "TEST00005") != NULL);

    /* 8. Simulated write failure (uncreatable path) */
    printf("[HOSTILE_TEST] Subtest 8: simulated write failure\n"); fflush(stdout);
    char bad_write_path[600];
    snprintf(bad_write_path, sizeof(bad_write_path), "%s%cbad_dir_nonexistent%cuncreatable.json",
             test_dir, nk_platform_path_separator(), nk_platform_path_separator());
    assert(nk_library_save(&lib, bad_write_path) == NK_ERROR_IO);

    /* 9. Simulated replace failure (target is an existing directory) */
    printf("[HOSTILE_TEST] Subtest 9: simulated replace failure\n"); fflush(stdout);
    char dir_as_target[600];
    snprintf(dir_as_target, sizeof(dir_as_target), "%.500s%ctarget_is_a_dir", test_dir, nk_platform_path_separator());
    assert(nk_platform_mkdir_p(dir_as_target));
    assert(nk_library_save(&lib, dir_as_target) == NK_ERROR_IO);
    /* Verify that dir_as_target.tmp was cleaned up */
    char tmp_check[600];
    snprintf(tmp_check, sizeof(tmp_check), "%.500s.tmp", dir_as_target);
    assert(!nk_platform_file_exists(tmp_check));

    /* 10. Corrupt primary + corrupt backup: fails closed cleanly without crash */
    printf("[HOSTILE_TEST] Subtest 10: corrupt primary + corrupt backup clean reset\n"); fflush(stdout);
    char double_corrupt_path[600];
    snprintf(double_corrupt_path, sizeof(double_corrupt_path), "%.500s%cdouble_corrupt.json", test_dir, nk_platform_path_separator());
    char double_bak_path[600];
    snprintf(double_bak_path, sizeof(double_bak_path), "%.500s.bak", double_corrupt_path);
    write_test_file(double_corrupt_path, "BAD_PRIMARY", 11);
    write_test_file(double_bak_path, "BAD_BACKUP", 10);
    NkLibrary double_corrupt_lib;
    assert(nk_library_load(&double_corrupt_lib, double_corrupt_path) == NK_ERROR_GENERIC);
    assert(nk_library_count(&double_corrupt_lib) == 0);

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
    uint8_t *pvd_cross = &cross_iso[16 * 2048];
    pvd_cross[0] = 0x01;
    memcpy(&pvd_cross[1], "CD001", 5);
    memcpy(&pvd_cross[40], "SECTOR_CROSS_VOL                ", 32);
    pvd_cross[158] = 17; pvd_cross[159] = 0; pvd_cross[160] = 0; pvd_cross[161] = 0;
    pvd_cross[162] = 0; pvd_cross[163] = 0; pvd_cross[164] = 0; pvd_cross[165] = 17;
    pvd_cross[166] = 0x00; pvd_cross[167] = 0x08; pvd_cross[168] = 0; pvd_cross[169] = 0;
    pvd_cross[170] = 0; pvd_cross[171] = 0; pvd_cross[172] = 0x08; pvd_cross[173] = 0x00;
    uint8_t *sec17 = &cross_iso[17 * 2048];
    fill_dir_chain(sec17, 2040);
    sec17[2040] = 34; /* 2040 + 34 = 2074 > 2048 -> crosses boundary! */
    write_test_file(fpath, cross_iso, sizeof(cross_iso));
    /* MUST be rejected with NK_ERROR_INVALID_ISO */
    assert(nk_iso_inspect(fpath, &meta) == NK_ERROR_INVALID_ISO);

    /* 7. Both-endian disagreement (ECMA-119 7.3.3) */
    printf("[HOSTILE_TEST] Subtest 7: both-endian disagreement\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cendian_disagree.iso", test_dir, nk_platform_path_separator());
    uint8_t endian_iso[19 * 2048];
    memset(endian_iso, 0, sizeof(endian_iso));
    uint8_t *pvd_endian = &endian_iso[16 * 2048];
    pvd_endian[0] = 0x01;
    memcpy(&pvd_endian[1], "CD001", 5);
    memcpy(&pvd_endian[40], "ENDIAN_TEST_VOL                 ", 32);
    pvd_endian[158] = 17; pvd_endian[159] = 0; pvd_endian[160] = 0; pvd_endian[161] = 0;
    pvd_endian[162] = 0; pvd_endian[163] = 0; pvd_endian[164] = 0; pvd_endian[165] = 17;
    pvd_endian[166] = 0x00; pvd_endian[167] = 0x08; pvd_endian[168] = 0; pvd_endian[169] = 0;
    pvd_endian[170] = 0; pvd_endian[171] = 0; pvd_endian[172] = 0x08; pvd_endian[173] = 0x00;
    uint8_t *sec17_endian = &endian_iso[17 * 2048];
    sec17_endian[0] = 42; /* rec_len */
    sec17_endian[2] = 18; sec17_endian[3] = 0; sec17_endian[4] = 0; sec17_endian[5] = 0; /* LE = 18 */
    sec17_endian[6] = 0; sec17_endian[7] = 0; sec17_endian[8] = 0x03; sec17_endian[9] = 0xE7; /* BE = 999 (DISAGREEMENT) */
    sec17_endian[10] = 0x00; sec17_endian[11] = 0x08; sec17_endian[12] = 0; sec17_endian[13] = 0;
    sec17_endian[14] = 0; sec17_endian[15] = 0; sec17_endian[16] = 0x08; sec17_endian[17] = 0x00;
    sec17_endian[32] = 8;
    memcpy(&sec17_endian[33], "PSP_GAME", 8);
    write_test_file(fpath, endian_iso, sizeof(endian_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath, &meta) == NK_OK);
    assert(strcmp(meta.volume_id, "ENDIAN_TEST_VOL") == 0);

    /* 8. Duplicate SFO keys */
    printf("[HOSTILE_TEST] Subtest 8a: conflicting duplicate SFO keys (must reject as ambiguous)\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cdup_sfo_conflict.iso", test_dir, nk_platform_path_separator());
    uint8_t dup_iso[18 * 2048];
    memset(dup_iso, 0, sizeof(dup_iso));
    uint8_t *pvd_dup = &dup_iso[16 * 2048];
    pvd_dup[0] = 0x01;
    memcpy(&pvd_dup[1], "CD001", 5);
    memcpy(&pvd_dup[40], "DUP_SFO_VOL                     ", 32);
    pvd_dup[158] = 17; pvd_dup[165] = 17;
    pvd_dup[167] = 0x08; pvd_dup[172] = 0x08;

    uint8_t *sfo = &dup_iso[17 * 2048];
    sfo[0] = 0x00; sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
    sfo[4] = 0x01; sfo[5] = 0x01;
    sfo[8] = 52; sfo[9] = 0; sfo[10] = 0; sfo[11] = 0;
    sfo[12] = 68; sfo[13] = 0; sfo[14] = 0; sfo[15] = 0;
    sfo[16] = 2; sfo[17] = 0; sfo[18] = 0; sfo[19] = 0;

    sfo[20] = 0; sfo[21] = 0;
    sfo[24] = 10; sfo[25] = 0; sfo[26] = 0; sfo[27] = 0;
    sfo[32] = 0; sfo[33] = 0; sfo[34] = 0; sfo[35] = 0;

    sfo[36] = 8; sfo[37] = 0;
    sfo[40] = 10; sfo[41] = 0; sfo[42] = 0; sfo[43] = 0;
    sfo[48] = 10; sfo[49] = 0; sfo[50] = 0; sfo[51] = 0;

    memcpy(&sfo[52], "DISC_ID\0", 8);
    memcpy(&sfo[60], "DISC_ID\0", 8);

    memcpy(&sfo[68], "UCUS98701\0", 10);
    memcpy(&sfo[78], "MALICIOUS\0", 10);

    write_test_file(fpath, dup_iso, sizeof(dup_iso));
    memset(&meta, 0, sizeof(meta));
    /* Conflicting duplicate keys MUST be rejected with NK_ERROR_INVALID_ISO */
    assert(nk_iso_inspect(fpath, &meta) == NK_ERROR_INVALID_ISO);

    printf("[HOSTILE_TEST] Subtest 8b: byte-identical duplicate SFO keys (must accept)\n"); fflush(stdout);
    char fpath_identical[512];
    snprintf(fpath_identical, sizeof(fpath_identical), "%s%cdup_sfo_identical.iso", test_dir, nk_platform_path_separator());
    memcpy(&sfo[78], "UCUS98701\0", 10); /* Same byte-identical value */
    write_test_file(fpath_identical, dup_iso, sizeof(dup_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_identical, &meta) == NK_OK);
    assert(strcmp(meta.disc_id, "UCUS98701") == 0);

    /* 9. ECMA-119 6.8.1.1 Sector Boundary Edge Cases */
    printf("[HOSTILE_TEST] Subtest 9a: valid sector padding (rec_len = 0 at byte 2040)\n"); fflush(stdout);
    char fpath_pad[512];
    snprintf(fpath_pad, sizeof(fpath_pad), "%s%csector_padding.iso", test_dir, nk_platform_path_separator());
    uint8_t pad_iso[19 * 2048];
    memset(pad_iso, 0, sizeof(pad_iso));
    uint8_t *pvd_pad = &pad_iso[16 * 2048];
    pvd_pad[0] = 0x01;
    memcpy(&pvd_pad[1], "CD001", 5);
    memcpy(&pvd_pad[40], "PAD_VOL                         ", 32);
    pvd_pad[158] = 17; pvd_pad[165] = 17;
    pvd_pad[166] = 0x00; pvd_pad[167] = 0x10; /* 4096 bytes (2 sectors: 17 and 18) */
    pvd_pad[172] = 0x10; pvd_pad[173] = 0x00;
    /* In sector 17, fill chain up to 2000, and byte 2000 is 0 (padding) */
    fill_dir_chain(&pad_iso[17 * 2048], 2000);
    pad_iso[17 * 2048 + 2000] = 0; /* padding */
    /* In sector 18 (offset 18*2048), put valid record */
    pad_iso[18 * 2048 + 0] = 34;
    write_test_file(fpath_pad, pad_iso, sizeof(pad_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_pad, &meta) == NK_OK);

    printf("[HOSTILE_TEST] Subtest 9b: record exactly ending at sector edge (rec_len 48 at 2000)\n"); fflush(stdout);
    char fpath_edge[512];
    snprintf(fpath_edge, sizeof(fpath_edge), "%s%csector_edge.iso", test_dir, nk_platform_path_separator());
    fill_dir_chain(&pad_iso[17 * 2048], 2000);
    pad_iso[17 * 2048 + 2000] = 48; /* 2000 + 48 = 2048 (exact edge) */
    write_test_file(fpath_edge, pad_iso, sizeof(pad_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_edge, &meta) == NK_OK);

    printf("[HOSTILE_TEST] Subtest 9c: one-byte-over boundary (rec_len 49 at 2000 -> 2049)\n"); fflush(stdout);
    char fpath_over[512];
    snprintf(fpath_over, sizeof(fpath_over), "%s%csector_over.iso", test_dir, nk_platform_path_separator());
    fill_dir_chain(&pad_iso[17 * 2048], 2000);
    pad_iso[17 * 2048 + 2000] = 49; /* 2000 + 49 = 2049 > 2048 */
    write_test_file(fpath_over, pad_iso, sizeof(pad_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_over, &meta) == NK_ERROR_INVALID_ISO);

    printf("[HOSTILE_TEST] Subtest 9d: absurd record length (rec_len 255 at 2000)\n"); fflush(stdout);
    char fpath_absurd[512];
    snprintf(fpath_absurd, sizeof(fpath_absurd), "%s%csector_absurd.iso", test_dir, nk_platform_path_separator());
    fill_dir_chain(&pad_iso[17 * 2048], 2000);
    pad_iso[17 * 2048 + 2000] = 255;
    write_test_file(fpath_absurd, pad_iso, sizeof(pad_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_absurd, &meta) == NK_ERROR_INVALID_ISO);

    printf("[HOSTILE_TEST] Subtest 9e: truncated next sector (root size 4096 but file 18 sectors)\n"); fflush(stdout);
    char fpath_trunc[512];
    snprintf(fpath_trunc, sizeof(fpath_trunc), "%s%csector_trunc.iso", test_dir, nk_platform_path_separator());
    /* Write only 18 sectors: sector 17 exists, but sector 18 does not! */
    write_test_file(fpath_trunc, pad_iso, 18 * 2048);
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_trunc, &meta) == NK_ERROR_INVALID_ISO);

    /* 10. Raw-scan identity must not satisfy the catalog.
       A valid image with no PARAM.SFO anywhere, but carrying the ASCII bytes of
       a catalog-known disc id in its data. The id may be reported as a guess;
       it must NOT mark the disc supported or verified, because a byte sequence
       occurring somewhere in an image is not evidence of disc identity. */
    char fpath_scan[512];
    snprintf(fpath_scan, sizeof(fpath_scan), "%s%cscan_identity.iso", test_dir, nk_platform_path_separator());
    static uint8_t scan_iso[20 * 2048];
    memset(scan_iso, 0, sizeof(scan_iso));
    uint8_t *spvd = &scan_iso[16 * 2048];
    spvd[0] = 0x01;
    memcpy(&spvd[1], "CD001", 5);
    memcpy(&spvd[40], "SCANPROBE_VOL                   ", 32);
    /* Root extent LBA = 17 (an empty directory sector), size = 2048 */
    spvd[158] = 17; spvd[159] = 0; spvd[160] = 0; spvd[161] = 0;
    spvd[166] = 0x00; spvd[167] = 0x08; spvd[168] = 0; spvd[169] = 0;
    /* Catalog-known disc id present only as loose bytes, with no SFO structure */
    memcpy(&scan_iso[18 * 2048], "TEST00001", 9);
    write_test_file(fpath_scan, scan_iso, sizeof(scan_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_scan, &meta) == NK_OK);
    assert(strcmp(meta.disc_id, "TEST00001") == 0);   /* still reported */
    assert(meta.is_supported == false);               /* but never trusted */
    assert(meta.status != NK_STATUS_VERIFIED);
    assert(meta.matched_title == NULL);

    /* An SFO reached only by scanning raw bytes has no filesystem provenance.
       The earlier case above embedded a bare disc-id string; this one embeds a
       STRUCTURALLY VALID SFO carrying a catalog-known DISC_ID in a sector the
       directory tree never references. Parsing succeeds, so the values may be
       reported -- but nothing about where they were found authorizes trusting
       them, and a crafted image must not be matched to a catalog title. */
    printf("[HOSTILE_TEST] embedded valid SFO with no directory provenance\n"); fflush(stdout);
    char fpath_embed[512];
    snprintf(fpath_embed, sizeof(fpath_embed), "%s%cembedded_sfo.iso", test_dir, nk_platform_path_separator());
    static uint8_t embed_iso[20 * 2048];
    memset(embed_iso, 0, sizeof(embed_iso));
    uint8_t *epvd = &embed_iso[16 * 2048];
    epvd[0] = 0x01;
    memcpy(&epvd[1], "CD001", 5);
    memcpy(&epvd[40], "EMBEDPROBE_VOL                  ", 32);
    /* Root extent is sector 17, an empty directory: PARAM.SFO is NOT reachable. */
    epvd[158] = 17; epvd[166] = 0x00; epvd[167] = 0x08;

    /* A well-formed one-entry SFO parked in unreferenced sector 18. Every
       string is copied without its terminator; the buffer is already zeroed. */
    uint8_t *esfo = &embed_iso[18 * 2048];
    esfo[1] = 'P'; esfo[2] = 'S'; esfo[3] = 'F';
    esfo[4] = 0x01; esfo[5] = 0x01;
    esfo[8] = 36;               /* key table offset   */
    esfo[12] = 44;              /* data table offset  */
    esfo[16] = 1;               /* one entry          */
    esfo[20] = 0; esfo[21] = 0; /* key offset 0       */
    esfo[24] = 10;              /* data length        */
    esfo[28] = 10;              /* data max length    */
    esfo[32] = 0;               /* data offset 0      */
    memcpy(&esfo[36], "DISC_ID", 7);
    memcpy(&esfo[44], "TEST00001", 9);

    write_test_file(fpath_embed, embed_iso, sizeof(embed_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_embed, &meta) == NK_OK);
    assert(strcmp(meta.disc_id, "TEST00001") == 0);   /* parsed, so reported */
    assert(meta.is_supported == false);               /* but never trusted   */
    assert(meta.status != NK_STATUS_VERIFIED);
    assert(meta.matched_title == NULL);


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
