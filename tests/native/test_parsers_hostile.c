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
#include "nk_json.h"
#include "nk_library.h"
#include "nk_platform.h"
#include "native_test_isolation.h"

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

static void put_both32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
    p[4] = (uint8_t)(value >> 24);
    p[5] = (uint8_t)(value >> 16);
    p[6] = (uint8_t)(value >> 8);
    p[7] = (uint8_t)value;
}

static void write_dir_record(uint8_t *sec, size_t offset, const char *name,
                             size_t name_len, uint32_t lba, uint32_t size,
                             bool directory) {
    size_t record_len = 33 + name_len;
    if ((record_len & 1u) != 0) record_len++;
    memset(sec + offset, 0, record_len);
    sec[offset] = (uint8_t)record_len;
    put_both32(sec + offset + 2, lba);
    put_both32(sec + offset + 10, size);
    sec[offset + 25] = directory ? 0x02 : 0x00;
    sec[offset + 32] = (uint8_t)name_len;
    memcpy(sec + offset + 33, name, name_len);
}


static void test_nk_json_validate_utf8_edge_cases(void) {
    printf("[HOSTILE_TEST] Testing nk_json_validate_utf8 boundary and edge cases...\n"); fflush(stdout);

    /* 1. Empty string & valid ASCII */
    assert(nk_json_validate_utf8((const uint8_t *)"", 0) == true);
    assert(nk_json_validate_utf8((const uint8_t *)"hello world", 11) == true);
    {
        uint8_t ascii_all[128];
        for (int i = 0; i <= 0x7F; i++) ascii_all[i] = (uint8_t)i;
        assert(nk_json_validate_utf8(ascii_all, sizeof(ascii_all)) == true);
    }

    /* 2. Valid multi-byte sequences */
    /* 2-byte: U+0080 to U+07FF (e.g. 0xC2 0x80, 0xDF 0xBF) */
    {
        static const uint8_t valid_2byte[] = { 0xC2, 0x80, 0xDF, 0xBF };
        assert(nk_json_validate_utf8(valid_2byte, sizeof(valid_2byte)) == true);
    }
    /* 3-byte: U+0800 to U+FFFF except surrogates */
    {
        static const uint8_t valid_3byte[] = {
            0xE0, 0xA0, 0x80,  /* U+0800 min 3-byte */
            0xED, 0x9F, 0xBF,  /* U+D7FF max before surrogates */
            0xEE, 0x80, 0x80,  /* U+E000 min after surrogates */
            0xEF, 0xBF, 0xBF   /* U+FFFF max 3-byte */
        };
        assert(nk_json_validate_utf8(valid_3byte, sizeof(valid_3byte)) == true);
    }
    /* 4-byte: U+10000 to U+10FFFF */
    {
        static const uint8_t valid_4byte[] = {
            0xF0, 0x90, 0x80, 0x80,  /* U+10000 min 4-byte */
            0xF4, 0x8F, 0xBF, 0xBF   /* U+10FFFF max valid Unicode */
        };
        assert(nk_json_validate_utf8(valid_4byte, sizeof(valid_4byte)) == true);
    }

    /* 3. Overlong sequences (must be rejected) */
    /* 2-byte overlong: lead bytes 0xC0, 0xC1 encoding U+0000..U+007F */
    {
        static const uint8_t overlong_c0[] = { 0xC0, 0xAF };
        static const uint8_t overlong_c1[] = { 0xC1, 0xBF };
        assert(nk_json_validate_utf8(overlong_c0, sizeof(overlong_c0)) == false);
        assert(nk_json_validate_utf8(overlong_c1, sizeof(overlong_c1)) == false);
    }
    /* 3-byte overlong: 0xE0 with c1 < 0xA0 */
    {
        static const uint8_t overlong_e0[] = { 0xE0, 0x80, 0x80 };
        static const uint8_t overlong_e0_2[] = { 0xE0, 0x9F, 0xBF };
        assert(nk_json_validate_utf8(overlong_e0, sizeof(overlong_e0)) == false);
        assert(nk_json_validate_utf8(overlong_e0_2, sizeof(overlong_e0_2)) == false);
    }
    /* 4-byte overlong: 0xF0 with c1 < 0x90 */
    {
        static const uint8_t overlong_f0[] = { 0xF0, 0x80, 0x80, 0x80 };
        static const uint8_t overlong_f0_2[] = { 0xF0, 0x8F, 0xBF, 0xBF };
        assert(nk_json_validate_utf8(overlong_f0, sizeof(overlong_f0)) == false);
        assert(nk_json_validate_utf8(overlong_f0_2, sizeof(overlong_f0_2)) == false);
    }

    /* 4. UTF-16 Surrogate halves U+D800..U+DFFF (0xED 0xA0 0x80 .. 0xED 0xBF 0xBF) */
    {
        static const uint8_t surrogate_min[] = { 0xED, 0xA0, 0x80 }; /* U+D800 */
        static const uint8_t surrogate_max[] = { 0xED, 0xBF, 0xBF }; /* U+DFFF */
        assert(nk_json_validate_utf8(surrogate_min, sizeof(surrogate_min)) == false);
        assert(nk_json_validate_utf8(surrogate_max, sizeof(surrogate_max)) == false);
    }

    /* 5. Values out of Unicode range (> U+10FFFF) */
    /* Lead byte 0xF4 with continuation > 0x8F */
    {
        static const uint8_t above_10ffff_f4[] = { 0xF4, 0x90, 0x80, 0x80 };
        assert(nk_json_validate_utf8(above_10ffff_f4, sizeof(above_10ffff_f4)) == false);
    }
    /* Lead bytes 0xF5..0xF7 (4-byte encodings > U+10FFFF) */
    {
        static const uint8_t lead_f5[] = { 0xF5, 0x80, 0x80, 0x80 };
        static const uint8_t lead_f6[] = { 0xF6, 0x80, 0x80, 0x80 };
        static const uint8_t lead_f7[] = { 0xF7, 0x80, 0x80, 0x80 };
        assert(nk_json_validate_utf8(lead_f5, sizeof(lead_f5)) == false);
        assert(nk_json_validate_utf8(lead_f6, sizeof(lead_f6)) == false);
        assert(nk_json_validate_utf8(lead_f7, sizeof(lead_f7)) == false);
    }
    /* Invalid lead bytes 0xF8..0xFF */
    {
        for (int c = 0xF8; c <= 0xFF; c++) {
            uint8_t bad_lead = (uint8_t)c;
            assert(nk_json_validate_utf8(&bad_lead, 1) == false);
        }
    }

    /* 6. Truncated sequences at end of input */
    {
        static const uint8_t trunc_2_a[] = { 0xC2 };
        static const uint8_t trunc_2_b[] = { 0xDF };
        static const uint8_t trunc_3_1_a[] = { 0xE0 };
        static const uint8_t trunc_3_2_a[] = { 0xE0, 0xA0 };
        static const uint8_t trunc_3_1_b[] = { 0xE1 };
        static const uint8_t trunc_3_2_b[] = { 0xE1, 0x80 };
        static const uint8_t trunc_3_1_c[] = { 0xED };
        static const uint8_t trunc_3_2_c[] = { 0xED, 0x80 };
        static const uint8_t trunc_3_1_d[] = { 0xEF };
        static const uint8_t trunc_3_2_d[] = { 0xEF, 0xBF };
        static const uint8_t trunc_4_1_a[] = { 0xF0 };
        static const uint8_t trunc_4_2_a[] = { 0xF0, 0x90 };
        static const uint8_t trunc_4_3_a[] = { 0xF0, 0x90, 0x80 };
        static const uint8_t trunc_4_1_b[] = { 0xF1 };
        static const uint8_t trunc_4_2_b[] = { 0xF1, 0x80 };
        static const uint8_t trunc_4_3_b[] = { 0xF1, 0x80, 0x80 };
        static const uint8_t trunc_4_1_c[] = { 0xF4 };
        static const uint8_t trunc_4_2_c[] = { 0xF4, 0x80 };
        static const uint8_t trunc_4_3_c[] = { 0xF4, 0x80, 0x80 };

        assert(nk_json_validate_utf8(trunc_2_a, sizeof(trunc_2_a)) == false);
        assert(nk_json_validate_utf8(trunc_2_b, sizeof(trunc_2_b)) == false);
        assert(nk_json_validate_utf8(trunc_3_1_a, sizeof(trunc_3_1_a)) == false);
        assert(nk_json_validate_utf8(trunc_3_2_a, sizeof(trunc_3_2_a)) == false);
        assert(nk_json_validate_utf8(trunc_3_1_b, sizeof(trunc_3_1_b)) == false);
        assert(nk_json_validate_utf8(trunc_3_2_b, sizeof(trunc_3_2_b)) == false);
        assert(nk_json_validate_utf8(trunc_3_1_c, sizeof(trunc_3_1_c)) == false);
        assert(nk_json_validate_utf8(trunc_3_2_c, sizeof(trunc_3_2_c)) == false);
        assert(nk_json_validate_utf8(trunc_3_1_d, sizeof(trunc_3_1_d)) == false);
        assert(nk_json_validate_utf8(trunc_3_2_d, sizeof(trunc_3_2_d)) == false);
        assert(nk_json_validate_utf8(trunc_4_1_a, sizeof(trunc_4_1_a)) == false);
        assert(nk_json_validate_utf8(trunc_4_2_a, sizeof(trunc_4_2_a)) == false);
        assert(nk_json_validate_utf8(trunc_4_3_a, sizeof(trunc_4_3_a)) == false);
        assert(nk_json_validate_utf8(trunc_4_1_b, sizeof(trunc_4_1_b)) == false);
        assert(nk_json_validate_utf8(trunc_4_2_b, sizeof(trunc_4_2_b)) == false);
        assert(nk_json_validate_utf8(trunc_4_3_b, sizeof(trunc_4_3_b)) == false);
        assert(nk_json_validate_utf8(trunc_4_1_c, sizeof(trunc_4_1_c)) == false);
        assert(nk_json_validate_utf8(trunc_4_2_c, sizeof(trunc_4_2_c)) == false);
        assert(nk_json_validate_utf8(trunc_4_3_c, sizeof(trunc_4_3_c)) == false);
    }

    /* 7. Invalid continuation bytes (not 0x80..0xBF) */
    {
        static const uint8_t bad_cont_2_low[] = { 0xC2, 0x7F };
        static const uint8_t bad_cont_2_high[] = { 0xC2, 0xC0 };
        static const uint8_t bad_cont_3_1[] = { 0xE0, 0xA0, 0x20 };
        static const uint8_t bad_cont_3_2[] = { 0xE1, 0x20, 0x80 };
        static const uint8_t bad_cont_3_2_high[] = { 0xE1, 0xC0, 0x80 };
        static const uint8_t bad_cont_3_3_high[] = { 0xE1, 0x80, 0xC0 };
        static const uint8_t bad_cont_4_1[] = { 0xF0, 0x90, 0x80, 0xC0 };
        static const uint8_t bad_cont_4_2[] = { 0xF0, 0x90, 0xC0, 0x80 };
        static const uint8_t bad_cont_4_3[] = { 0xF0, 0xC0, 0x80, 0x80 };
        static const uint8_t bad_cont_4_f1_c1_low[] = { 0xF1, 0x7F, 0x80, 0x80 };
        static const uint8_t bad_cont_4_f1_c1_high[] = { 0xF1, 0xC0, 0x80, 0x80 };
        static const uint8_t bad_cont_4_f1_c2_low[] = { 0xF1, 0x80, 0x7F, 0x80 };
        static const uint8_t bad_cont_4_f1_c2_high[] = { 0xF1, 0x80, 0xC0, 0x80 };
        static const uint8_t bad_cont_4_f1_c3_low[] = { 0xF1, 0x80, 0x80, 0x7F };
        static const uint8_t bad_cont_4_f1_c3_high[] = { 0xF1, 0x80, 0x80, 0xC0 };

        assert(nk_json_validate_utf8(bad_cont_2_low, sizeof(bad_cont_2_low)) == false);
        assert(nk_json_validate_utf8(bad_cont_2_high, sizeof(bad_cont_2_high)) == false);
        assert(nk_json_validate_utf8(bad_cont_3_1, sizeof(bad_cont_3_1)) == false);
        assert(nk_json_validate_utf8(bad_cont_3_2, sizeof(bad_cont_3_2)) == false);
        assert(nk_json_validate_utf8(bad_cont_3_2_high, sizeof(bad_cont_3_2_high)) == false);
        assert(nk_json_validate_utf8(bad_cont_3_3_high, sizeof(bad_cont_3_3_high)) == false);
        assert(nk_json_validate_utf8(bad_cont_4_1, sizeof(bad_cont_4_1)) == false);
        assert(nk_json_validate_utf8(bad_cont_4_2, sizeof(bad_cont_4_2)) == false);
        assert(nk_json_validate_utf8(bad_cont_4_3, sizeof(bad_cont_4_3)) == false);
        assert(nk_json_validate_utf8(bad_cont_4_f1_c1_low, sizeof(bad_cont_4_f1_c1_low)) == false);
        assert(nk_json_validate_utf8(bad_cont_4_f1_c1_high, sizeof(bad_cont_4_f1_c1_high)) == false);
        assert(nk_json_validate_utf8(bad_cont_4_f1_c2_low, sizeof(bad_cont_4_f1_c2_low)) == false);
        assert(nk_json_validate_utf8(bad_cont_4_f1_c2_high, sizeof(bad_cont_4_f1_c2_high)) == false);
        assert(nk_json_validate_utf8(bad_cont_4_f1_c3_low, sizeof(bad_cont_4_f1_c3_low)) == false);
        assert(nk_json_validate_utf8(bad_cont_4_f1_c3_high, sizeof(bad_cont_4_f1_c3_high)) == false);
    }

    /* 8. Stray continuation bytes (0x80..0xBF as lead byte) */
    {
        for (int c = 0x80; c <= 0xBF; c++) {
            uint8_t stray = (uint8_t)c;
            assert(nk_json_validate_utf8(&stray, 1) == false);
        }
    }

    /* 9. Detailed 3-byte and 4-byte scalar boundaries (0xE0, 0xED, 0xF0, 0xF1..0xF3, 0xF4) */
    {
        /* 0xED boundary: U+D000 (0xED 0x80 0x80) is valid */
        static const uint8_t ed_d000[] = { 0xED, 0x80, 0x80 };
        /* 0xED c1 < 0x80 is invalid */
        static const uint8_t ed_bad_c1_low[] = { 0xED, 0x7F, 0x80 };
        /* 0xED c1 = 0xC0 is not a continuation byte and invalid (surrogates: section 4) */
        static const uint8_t ed_bad_c1_high[] = { 0xED, 0xC0, 0x80 };

        /* 0xF1..0xF3 range: U+40000 (0xF1 0x80 0x80 0x80), U+90000 (0xF2 0x90 0x80 0x80), U+FFFFF (0xF3 0xBF 0xBF 0xBF) */
        static const uint8_t f1_valid[] = { 0xF1, 0x80, 0x80, 0x80 };
        static const uint8_t f2_valid[] = { 0xF2, 0x90, 0x80, 0x80 };
        static const uint8_t f3_valid[] = { 0xF3, 0xBF, 0xBF, 0xBF };

        /* 0xF4 boundary: U+100000 (0xF4 0x80 0x80 0x80) min 0xF4 is valid */
        static const uint8_t f4_u100000[] = { 0xF4, 0x80, 0x80, 0x80 };
        /* 0xF4 c1 < 0x80 is invalid */
        static const uint8_t f4_bad_c1_low[] = { 0xF4, 0x7F, 0x80, 0x80 };
        /* 0xF4 c1 = 0xC0 is not a continuation byte and invalid (above U+10FFFF: section 5) */
        static const uint8_t f4_bad_c1_high[] = { 0xF4, 0xC0, 0x80, 0x80 };

        assert(nk_json_validate_utf8(ed_d000, sizeof(ed_d000)) == true);
        assert(nk_json_validate_utf8(ed_bad_c1_low, sizeof(ed_bad_c1_low)) == false);
        assert(nk_json_validate_utf8(ed_bad_c1_high, sizeof(ed_bad_c1_high)) == false);

        assert(nk_json_validate_utf8(f1_valid, sizeof(f1_valid)) == true);
        assert(nk_json_validate_utf8(f2_valid, sizeof(f2_valid)) == true);
        assert(nk_json_validate_utf8(f3_valid, sizeof(f3_valid)) == true);

        assert(nk_json_validate_utf8(f4_u100000, sizeof(f4_u100000)) == true);
        assert(nk_json_validate_utf8(f4_bad_c1_low, sizeof(f4_bad_c1_low)) == false);
        assert(nk_json_validate_utf8(f4_bad_c1_high, sizeof(f4_bad_c1_high)) == false);
    }

    /* 10. Embedded NUL bytes, mixed sequences, and positional invalid sequence checks */
    {
        /* Embedded NUL byte in multi-byte string is valid UTF-8 ASCII byte */
        static const uint8_t embedded_nul[] = { 'h', 'e', 'l', 'l', 'o', 0x00, 'w', 'o', 'r', 'l', 'd' };
        assert(nk_json_validate_utf8(embedded_nul, sizeof(embedded_nul)) == true);

        /* Concatenation of valid 1, 2, 3, and 4-byte UTF-8 sequences */
        static const uint8_t mixed_valid[] = {
            'A',                       /* 1-byte */
            0xC2, 0xA0,                /* 2-byte: U+00A0 */
            0xE2, 0x82, 0xAC,          /* 3-byte: U+20AC Euro symbol */
            0xF0, 0x9F, 0x98, 0x80     /* 4-byte: U+1F600 Emoji */
        };
        assert(nk_json_validate_utf8(mixed_valid, sizeof(mixed_valid)) == true);

        /* Invalid byte at start */
        static const uint8_t bad_at_start[] = { 0xFF, 'h', 'e', 'l', 'l', 'o' };
        assert(nk_json_validate_utf8(bad_at_start, sizeof(bad_at_start)) == false);

        /* Invalid byte in middle */
        static const uint8_t bad_in_middle[] = { 'h', 'e', 0x80, 'l', 'o' };
        assert(nk_json_validate_utf8(bad_in_middle, sizeof(bad_in_middle)) == false);

        /* Invalid byte at end */
        static const uint8_t bad_at_end[] = { 'h', 'e', 'l', 'l', 'o', 0xFE };
        assert(nk_json_validate_utf8(bad_at_end, sizeof(bad_at_end)) == false);
    }

    printf("[HOSTILE_TEST] nk_json_validate_utf8 edge cases PASSED!\n"); fflush(stdout);
}

static void test_hostile_library_json(const char *test_dir) {
    char fpath[512];
    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */

    printf("[HOSTILE_TEST] Testing library.json parser resilience...\n");
    fflush(stdout);

    /* 1. Missing schema_version */
    printf("[HOSTILE_TEST] Subtest 1: missing schema_version\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cbad_schema_missing.json", test_dir, nk_platform_path_separator());
    const char json_no_schema[] = "{\n  \"games\": []\n}";
    write_test_file(fpath, json_no_schema, strlen(json_no_schema));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);

    /* 2. Unsupported schema_version: a newer one is refused by name (a later player
       wrote it), an impossible one as malformed. */
    printf("[HOSTILE_TEST] Subtest 2: unsupported schema_version\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cbad_schema_future.json", test_dir, nk_platform_path_separator());
    const char json_future_schema[] = "{\n  \"schema_version\": 999,\n  \"games\": []\n}";
    write_test_file(fpath, json_future_schema, strlen(json_future_schema));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_NEWER_SCHEMA);
    assert(nk_library_count(&lib) == 0 && lib.newer_schema_version == 999);
    const char json_zero_schema[] = "{\n  \"schema_version\": 0,\n  \"games\": []\n}";
    write_test_file(fpath, json_zero_schema, strlen(json_zero_schema));
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

    /* 5b. Truncated games array EOF without closing bracket (regression test for ASan heap-buffer-overflow) */
    printf("[HOSTILE_TEST] Subtest 5b: truncated games array EOF\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%ctruncated_array_eof.json", test_dir, nk_platform_path_separator());
    const char json_truncated_eof[] = "{\n  \"schema_version\": 1,\n  \"games\": [";
    write_test_file(fpath, json_truncated_eof, strlen(json_truncated_eof));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);
    assert(nk_library_count(&lib) == 0);

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

    static NkLibrary loaded_lib;  /* too large for the stack (NK_MAX_GAMES) */
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

    static NkLibrary recovered_lib;  /* too large for the stack (NK_MAX_GAMES) */
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
    static NkLibrary double_corrupt_lib;  /* too large for the stack (NK_MAX_GAMES) */
    assert(nk_library_load(&double_corrupt_lib, double_corrupt_path) == NK_ERROR_GENERIC);
    assert(nk_library_count(&double_corrupt_lib) == 0);

    /* 11. Zero-byte library file */
    printf("[HOSTILE_TEST] Subtest 11: zero-byte library file\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%czero_byte_lib.json", test_dir, nk_platform_path_separator());
    write_test_file(fpath, "", 0);
    assert(nk_library_load(&lib, fpath) == NK_ERROR_IO);

    /* 12. Oversized library file (over NK_LIBRARY_MAX_BYTES) */
    printf("[HOSTILE_TEST] Subtest 12: oversized library file\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%coversized_lib.json", test_dir, nk_platform_path_separator());
    size_t big_sz = (size_t)NK_LIBRARY_MAX_BYTES + 16u;
    char *big_buf = (char *)malloc(big_sz);
    assert(big_buf != NULL);
    memset(big_buf, ' ', big_sz);
    write_test_file(fpath, big_buf, big_sz);
    free(big_buf);
    assert(nk_library_load(&lib, fpath) == NK_ERROR_IO);

    /* 13. Overlong boot_executable path */
    printf("[HOSTILE_TEST] Subtest 13: overlong boot_executable path\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%coverlong_boot_exec.json", test_dir, nk_platform_path_separator());
    char overlong_exec[300];
    memset(overlong_exec, 'E', 256);
    overlong_exec[256] = '\0';
    char overlong_json[2048];
    snprintf(overlong_json, sizeof(overlong_json),
        "{\n  \"schema_version\": 1,\n  \"games\": [\n    {\n      \"disc_id\": \"TEST00001\",\n      \"boot_executable\": \"%s\"\n    }\n  ]\n}",
        overlong_exec);
    write_test_file(fpath, overlong_json, strlen(overlong_json));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_INVALID_EXECUTABLE);

    /* 14. Out-of-bounds enum values */
    printf("[HOSTILE_TEST] Subtest 14: out-of-bounds enum values\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%coob_enum.json", test_dir, nk_platform_path_separator());
    const char json_oob_kind[] =
        "{\n  \"schema_version\": 1,\n  \"games\": [\n    {\n      \"disc_id\": \"TEST00001\",\n      \"executable_eboot_kind\": 999\n    }\n  ]\n}";
    write_test_file(fpath, json_oob_kind, strlen(json_oob_kind));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);

    /* 15. Missing closing brackets at EOF */
    printf("[HOSTILE_TEST] Subtest 15: missing closing array bracket ]\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cmissing_closing_array.json", test_dir, nk_platform_path_separator());
    const char json_no_closing_array[] = "{\n  \"schema_version\": 1,\n  \"games\": [\n    {\n      \"disc_id\": \"TEST00001\"\n    }\n}";
    write_test_file(fpath, json_no_closing_array, strlen(json_no_closing_array));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);

    printf("[HOSTILE_TEST] Subtest 16: missing closing root object bracket }\n"); fflush(stdout);
    snprintf(fpath, sizeof(fpath), "%s%cmissing_closing_obj.json", test_dir, nk_platform_path_separator());
    const char json_no_closing_obj[] = "{\n  \"schema_version\": 1,\n  \"games\": [\n    {\n      \"disc_id\": \"TEST00001\"\n    }\n  ]";
    write_test_file(fpath, json_no_closing_obj, strlen(json_no_closing_obj));
    assert(nk_library_load(&lib, fpath) == NK_ERROR_GENERIC);

    printf("[HOSTILE_TEST] Library JSON tests PASSED!\n");
    fflush(stdout);
}

/* Writes one library record whose title_name is the raw JSON string body
 * `title_body` (escapes included, quotes excluded) and loads it. */
static NkResult load_library_with_title_body(const char *fpath, const char *title_body,
                                             NkLibrary *lib) {
    char json[4096];
    snprintf(json, sizeof(json),
             "{\n  \"schema_version\": 1,\n  \"games\": [\n"
             "    {\"disc_id\": \"TEST00011\", \"title_name\": \"%s\", \"disc_version\": \"1.00\"}\n"
             "  ]\n}\n",
             title_body);
    write_test_file(fpath, json, strlen(json));
    return nk_library_load(lib, fpath);
}

static void test_library_json_string_escapes(const char *test_dir) {
    char fpath[512];
    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */

    printf("[HOSTILE_TEST] Testing library.json string escapes...\n"); fflush(stdout);

    /* 1. Every RFC 8259 escape decodes to its exact UTF-8 bytes. Metadata is
     *    synthetic: the trademark sign, a surrogate pair and the short escapes. */
    struct { const char *body; const char *expect; } good[] = {
        { "Synthetic \\u2122 fixture", "Synthetic \xE2\x84\xA2 fixture" },
        { "Pair \\ud83c\\udfae end", "Pair \xF0\x9F\x8E\xAE end" },
        { "\\\"\\\\\\/\\b\\f\\n\\r\\t", "\"\\/\b\f\n\r\t" },
        { "\\u00e9t\\u00E9", "\xC3\xA9t\xC3\xA9" },
        { "\\u0041\\u007f", "A\x7f" },
        /* A NUL escape is a NUL byte: the C-string value ends there. */
        { "a\\u0000b", "a" },
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        snprintf(fpath, sizeof(fpath), "%s%cescape_ok_%zu.json", test_dir,
                 nk_platform_path_separator(), i);
        nk_library_init(&lib);
        assert(load_library_with_title_body(fpath, good[i].body, &lib) == NK_OK);
        assert(nk_library_count(&lib) == 1);
        const NkGameEntry *e = nk_library_get(&lib, 0);
        assert(e != NULL);
        assert(strcmp(e->title_name, good[i].expect) == 0);
    }

    /* 2. Malformed escapes and raw control characters fail the whole file with
     *    no entry and no invented bytes. Each case gets a fresh file name, so no
     *    .bak recovery copy can mask the failure. */
    const char *bad[] = {
        "bad \\x41 escape",
        "bad \\u12G4 hex",
        "bad \\u12",
        "lone \\ud83c high surrogate",
        "lone \\udfae low surrogate",
        "pair \\ud83c\\u0041 not a low surrogate",
        "raw \t tab",
        "raw \x01 control",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        snprintf(fpath, sizeof(fpath), "%s%cescape_bad_%zu.json", test_dir,
                 nk_platform_path_separator(), i);
        nk_library_init(&lib);
        assert(load_library_with_title_body(fpath, bad[i], &lib) == NK_ERROR_GENERIC);
        assert(nk_library_count(&lib) == 0);
    }

    /* 3. A decoded code point that does not fit the field is dropped whole.
     *    The field keeps only complete characters, never a split sequence. */
    char long_body[NK_MAX_TITLE_LEN + 64];
    size_t fill = sizeof(((NkGameEntry *)0)->title_name) - 2;
    memset(long_body, 'A', fill);
    memcpy(long_body + fill, "\\u2122", 6);
    long_body[fill + 6] = '\0';
    snprintf(fpath, sizeof(fpath), "%s%cescape_truncated.json", test_dir,
             nk_platform_path_separator());
    nk_library_init(&lib);
    assert(load_library_with_title_body(fpath, long_body, &lib) == NK_OK);
    assert(nk_library_count(&lib) == 1);
    const NkGameEntry *long_entry = nk_library_get(&lib, 0);
    assert(strlen(long_entry->title_name) == fill);
    for (size_t i = 0; i < fill; i++) assert(long_entry->title_name[i] == 'A');

    /* 4. The shared JSON reader decodes the same escapes. */
    const char json_text[] = "{\"k\": \"\\u2122\\ud83c\\udfae\"}";
    char parse_err[128] = "";
    NkJsonNode *root = nk_json_parse(json_text, strlen(json_text), parse_err, sizeof(parse_err));
    assert(root != NULL);
    const char *value = nk_json_get_string(nk_json_obj_get(root, "k"));
    assert(value != NULL);
    assert(strlen(value) == 7);
    assert(memcmp(value, "\xE2\x84\xA2\xF0\x9F\x8E\xAE", 7) == 0);
    nk_json_free(root);

    printf("[HOSTILE_TEST] Library string escape tests PASSED!\n"); fflush(stdout);
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
    uint8_t dup_iso[20 * 2048];
    memset(dup_iso, 0, sizeof(dup_iso));
    uint8_t *pvd_dup = &dup_iso[16 * 2048];
    pvd_dup[0] = 0x01;
    memcpy(&pvd_dup[1], "CD001", 5);
    memcpy(&pvd_dup[40], "DUP_SFO_VOL                     ", 32);
    pvd_dup[158] = 17; pvd_dup[159] = 0; pvd_dup[160] = 0; pvd_dup[161] = 0;
    pvd_dup[162] = 0; pvd_dup[163] = 0; pvd_dup[164] = 0; pvd_dup[165] = 0;
    pvd_dup[166] = 0x00; pvd_dup[167] = 0x08; pvd_dup[168] = 0; pvd_dup[169] = 0;
    pvd_dup[170] = 0; pvd_dup[171] = 0; pvd_dup[172] = 0; pvd_dup[173] = 0x08;
    write_dir_record(pvd_dup, 156, "\0", 1, 17, 2048, true);
    write_dir_record(&dup_iso[17 * 2048], 0, "\0", 1, 17, 2048, true);
    write_dir_record(&dup_iso[17 * 2048], 34, "\1", 1, 17, 2048, true);
    write_dir_record(&dup_iso[17 * 2048], 68, "PSP_GAME", 8, 18, 2048, true);
    write_dir_record(&dup_iso[18 * 2048], 0, "\0", 1, 18, 2048, true);
    write_dir_record(&dup_iso[18 * 2048], 34, "\1", 1, 17, 2048, true);
    write_dir_record(&dup_iso[18 * 2048], 68, "PARAM.SFO;1", 11, 19, 100, false);

    uint8_t *sfo = &dup_iso[19 * 2048];
    sfo[0] = 0x00; sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
    sfo[4] = 0x01; sfo[5] = 0x01;
    sfo[8] = 52; sfo[9] = 0; sfo[10] = 0; sfo[11] = 0;
    sfo[12] = 68; sfo[13] = 0; sfo[14] = 0; sfo[15] = 0;
    sfo[16] = 2; sfo[17] = 0; sfo[18] = 0; sfo[19] = 0;

    sfo[20] = 0; sfo[21] = 0;
    sfo[22] = 0x04; sfo[23] = 0x02;
    sfo[24] = 10; sfo[25] = 0; sfo[26] = 0; sfo[27] = 0;
    sfo[28] = 16; sfo[29] = 0; sfo[30] = 0; sfo[31] = 0;
    sfo[32] = 0; sfo[33] = 0; sfo[34] = 0; sfo[35] = 0;

    sfo[36] = 8; sfo[37] = 0;
    sfo[38] = 0x04; sfo[39] = 0x02;
    sfo[40] = 10; sfo[41] = 0; sfo[42] = 0; sfo[43] = 0;
    sfo[44] = 16; sfo[45] = 0; sfo[46] = 0; sfo[47] = 0;
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
    assert(strcmp(meta.disc_id, "UNKNOWN") == 0);
    assert(strcmp(meta.title_name, "Unknown PSP Title") == 0);
    assert(meta.param_sfo_parsed == false);
    assert(meta.is_supported == false);
    assert(meta.status != NK_STATUS_VERIFIED);
    assert(meta.matched_title_id[0] == '\0');

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
    esfo[22] = 0x04;            /* format 0x0204:     */
    esfo[23] = 0x02;            /* NUL-terminated UTF-8 */
    esfo[24] = 10;              /* data length        */
    esfo[28] = 10;              /* data max length    */
    esfo[32] = 0;               /* data offset 0      */
    memcpy(&esfo[36], "DISC_ID", 7);
    memcpy(&esfo[44], "TEST00001", 9);

    write_test_file(fpath_embed, embed_iso, sizeof(embed_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_embed, &meta) == NK_OK);
    assert(strcmp(meta.disc_id, "UNKNOWN") == 0);
    assert(strcmp(meta.title_name, "Unknown PSP Title") == 0);
    assert(meta.param_sfo_parsed == false);
    assert(meta.is_supported == false);
    assert(meta.status != NK_STATUS_VERIFIED);
    assert(meta.matched_title_id[0] == '\0');


    /* 11. An identity field declaring a non-string parameter format.
     *
     * The parser ignored the format field at entry offset +2 and decoded the
     * bytes as text whatever it said. tools/nk_core/iso_inspect.py does not:
     * for an integer format it yields the decoded number, and for an
     * unrecognised one an empty string. A disc whose DISC_ID declared a
     * non-string format could therefore be matched to a catalog title by the
     * native reader and not by the canonical one -- the exact divergence the
     * two parsers exist to prevent. Identity is decoded only from UTF-8.
     *
     * The image is otherwise the directory-reachable, well-formed one used
     * above, so nothing but the format field can explain a rejection. */
    printf("[HOSTILE_TEST] Subtest 11: non-string SFO parameter format\n"); fflush(stdout);
    char fpath_fmt[512];
    snprintf(fpath_fmt, sizeof(fpath_fmt), "%s%csfo_int_discid.iso", test_dir, nk_platform_path_separator());

    static uint8_t fmt_iso[20 * 2048];
    memset(fmt_iso, 0, sizeof(fmt_iso));
    uint8_t *fpvd = &fmt_iso[16 * 2048];
    fpvd[0] = 0x01;
    memcpy(&fpvd[1], "CD001", 5);
    memcpy(&fpvd[40], "FMT_PROBE_VOL                   ", 32);
    fpvd[158] = 17; fpvd[159] = 0; fpvd[160] = 0; fpvd[161] = 0;
    fpvd[162] = 0; fpvd[163] = 0; fpvd[164] = 0; fpvd[165] = 0;
    fpvd[166] = 0x00; fpvd[167] = 0x08; fpvd[168] = 0; fpvd[169] = 0;
    fpvd[170] = 0; fpvd[171] = 0; fpvd[172] = 0; fpvd[173] = 0x08;
    write_dir_record(fpvd, 156, "\0", 1, 17, 2048, true);
    write_dir_record(&fmt_iso[17 * 2048], 0, "\0", 1, 17, 2048, true);
    write_dir_record(&fmt_iso[17 * 2048], 34, "\1", 1, 17, 2048, true);
    write_dir_record(&fmt_iso[17 * 2048], 68, "PSP_GAME", 8, 18, 2048, true);
    write_dir_record(&fmt_iso[18 * 2048], 0, "\0", 1, 18, 2048, true);
    write_dir_record(&fmt_iso[18 * 2048], 34, "\1", 1, 17, 2048, true);
    write_dir_record(&fmt_iso[18 * 2048], 68, "PARAM.SFO;1", 11, 19, 100, false);

    uint8_t *fsfo = &fmt_iso[19 * 2048];
    fsfo[0] = 0x00; fsfo[1] = 'P'; fsfo[2] = 'S'; fsfo[3] = 'F';
    fsfo[4] = 0x01; fsfo[5] = 0x01;
    fsfo[8] = 36; fsfo[9] = 0; fsfo[10] = 0; fsfo[11] = 0;
    fsfo[12] = 44; fsfo[13] = 0; fsfo[14] = 0; fsfo[15] = 0;
    fsfo[16] = 1; fsfo[17] = 0; fsfo[18] = 0; fsfo[19] = 0;
    fsfo[20] = 0; fsfo[21] = 0;
    fsfo[22] = 0x04; fsfo[23] = 0x04;
    fsfo[24] = 10; fsfo[25] = 0; fsfo[26] = 0; fsfo[27] = 0;
    fsfo[28] = 10; fsfo[29] = 0; fsfo[30] = 0; fsfo[31] = 0;
    fsfo[32] = 0; fsfo[33] = 0; fsfo[34] = 0; fsfo[35] = 0;
    memcpy(&fsfo[36], "DISC_ID", 7);
    memcpy(&fsfo[44], "TEST00001", 9);

    write_test_file(fpath_fmt, fmt_iso, sizeof(fmt_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_fmt, &meta) == NK_ERROR_INVALID_ISO);
    assert(meta.is_supported == false);
    assert(meta.matched_title_id[0] == '\0');

    /* The same image with the string format parses and identifies. Without
     * this the subtest above could pass for some unrelated reason. */
    fsfo[22] = 0x04; fsfo[23] = 0x02;   /* format 0x0204 */
    write_test_file(fpath_fmt, fmt_iso, sizeof(fmt_iso));
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(fpath_fmt, &meta) == NK_OK);
    assert(strcmp(meta.disc_id, "TEST00001") == 0);

    printf("[HOSTILE_TEST] ISO parser tests PASSED!\n");
}

static void write_le16_test(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void write_le32_test(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void write_both_endian_test(uint8_t *dst, uint32_t value) {
    write_le32_test(dst, value);
    dst[4] = (uint8_t)(value >> 24);
    dst[5] = (uint8_t)(value >> 16);
    dst[6] = (uint8_t)(value >> 8);
    dst[7] = (uint8_t)value;
}

static size_t append_iso_record_test(uint8_t *directory, size_t offset,
                                     const uint8_t *name, uint8_t name_size,
                                     uint32_t lba, uint32_t size, bool is_dir) {
    size_t record_size = 33u + name_size;
    if ((record_size & 1u) != 0) record_size++;
    uint8_t *record = directory + offset;
    memset(record, 0, record_size);
    record[0] = (uint8_t)record_size;
    write_both_endian_test(record + 2, lba);
    write_both_endian_test(record + 10, size);
    record[25] = is_dir ? 2u : 0u;
    record[28] = 1u;
    record[31] = 1u;
    record[32] = name_size;
    memcpy(record + 33, name, name_size);
    return offset + record_size;
}

static void build_executable_iso_test(const char *path,
                                      const void *eboot, size_t eboot_size,
                                      const void *boot, size_t boot_size) {
    const size_t sector_size = 2048;
    const size_t image_size = 22u * sector_size;
    uint8_t *image = (uint8_t *)calloc(image_size, 1);
    assert(image != NULL);
    assert(eboot_size <= sector_size && boot_size <= sector_size);

    uint8_t *pvd = image + 16u * sector_size;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    write_both_endian_test(pvd + 158, 17);
    write_both_endian_test(pvd + 166, (uint32_t)sector_size);
    {
        const uint8_t dot[] = { 0 };
        append_iso_record_test(pvd + 156, 0, dot, 1, 17,
                               (uint32_t)sector_size, true);
    }

    uint8_t *root = image + 17u * sector_size;
    uint8_t *game = image + 18u * sector_size;
    uint8_t *sysdir = image + 19u * sector_size;
    const uint8_t dot[] = { 0 }, dotdot[] = { 1 };
    size_t used = 0;
    used = append_iso_record_test(root, used, dot, 1, 17, (uint32_t)sector_size, true);
    used = append_iso_record_test(root, used, dotdot, 1, 17, (uint32_t)sector_size, true);
    used = append_iso_record_test(root, used, (const uint8_t *)"PSP_GAME", 8,
                                  18, (uint32_t)sector_size, true);
    (void)used;

    used = 0;
    used = append_iso_record_test(game, used, dot, 1, 18, (uint32_t)sector_size, true);
    used = append_iso_record_test(game, used, dotdot, 1, 17, (uint32_t)sector_size, true);
    used = append_iso_record_test(game, used, (const uint8_t *)"SYSDIR", 6,
                                  19, (uint32_t)sector_size, true);
    (void)used;

    used = 0;
    used = append_iso_record_test(sysdir, used, dot, 1, 19, (uint32_t)sector_size, true);
    used = append_iso_record_test(sysdir, used, dotdot, 1, 18, (uint32_t)sector_size, true);
    used = append_iso_record_test(sysdir, used, (const uint8_t *)"EBOOT.BIN;1", 11,
                                  20, (uint32_t)eboot_size, false);
    append_iso_record_test(sysdir, used, (const uint8_t *)"BOOT.BIN;1", 10,
                           21, (uint32_t)boot_size, false);
    if (eboot_size) memcpy(image + 20u * sector_size, eboot, eboot_size);
    if (boot_size) memcpy(image + 21u * sector_size, boot, boot_size);
    write_test_file(path, image, image_size);
    free(image);
}

/* Build a two-entry UTF-8 PARAM.SFO (DISC_ID then TITLE) the way the shared
   parser reads it: header, entry table, key table, padded data table. */
static size_t build_sfo_test(uint8_t *dst, size_t cap, const char *disc_id,
                             const char *title) {
    const size_t key_table_start = 52;
    const size_t data_table_start = 68;
    const size_t title_value_offset = 12;
    size_t disc_len = strlen(disc_id);
    size_t title_len = strlen(title);
    size_t total = data_table_start + title_value_offset +
                   ((title_len + 1u + 3u) & ~(size_t)3u);
    if (total > cap) return 0;
    memset(dst, 0, total);
    dst[0] = 0x00; dst[1] = 'P'; dst[2] = 'S'; dst[3] = 'F';
    dst[4] = 0x01; dst[5] = 0x01;
    write_le32_test(dst + 8, (uint32_t)key_table_start);
    write_le32_test(dst + 12, (uint32_t)data_table_start);
    write_le32_test(dst + 16, 2);

    write_le16_test(dst + 20, 0);
    write_le16_test(dst + 22, 0x0204);
    write_le32_test(dst + 24, (uint32_t)(disc_len + 1));
    write_le32_test(dst + 28, (uint32_t)(disc_len + 1));
    write_le32_test(dst + 32, 0);

    write_le16_test(dst + 36, 8);
    write_le16_test(dst + 38, 0x0204);
    write_le32_test(dst + 40, (uint32_t)(title_len + 1));
    write_le32_test(dst + 44, (uint32_t)(title_len + 1));
    write_le32_test(dst + 48, (uint32_t)title_value_offset);

    memcpy(dst + key_table_start, "DISC_ID\0", 8);
    memcpy(dst + key_table_start + 8, "TITLE\0", 6);
    memcpy(dst + data_table_start, disc_id, disc_len);
    memcpy(dst + data_table_start + title_value_offset, title, title_len);
    return total;
}

static void build_pbp_header_test(uint8_t header[40], const uint32_t *offsets) {
    memset(header, 0, 40);
    header[0] = 0x00; header[1] = 'P'; header[2] = 'B'; header[3] = 'P';
    write_le32_test(header + 4, 0x00010000u);
    for (uint32_t index = 0; index < 8; index++) {
        write_le32_test(header + 8 + index * 4, offsets[index]);
    }
}

static void write_pbp_test(const char *path, const uint8_t *header,
                           size_t header_len, const uint8_t *body,
                           size_t body_size) {
    size_t total = header_len + body_size;
    uint8_t *buffer = (uint8_t *)calloc(total + 1u, 1);
    assert(buffer != NULL);
    if (header_len) memcpy(buffer, header, header_len);
    if (body_size) memcpy(buffer + header_len, body, body_size);
    write_test_file(path, buffer, total);
    free(buffer);
}

/* A PlayStation Store package must be refused at a named boundary with the
   sentence it carries, instead of being reported as a disc with no volume
   descriptor. Every hostile variant still has to fail closed. */
static void test_hostile_pbp_package(const char *test_dir) {
    char path[600];
    uint8_t header[40];
    uint8_t sfo[512];
    uint32_t offsets[8];
    NkIsoMetadata meta;
    size_t sfo_size;
    int index;

    printf("[HOSTILE_TEST] Testing PBP package identification...\n"); fflush(stdout);

    printf("[HOSTILE_TEST] PBP subtest 1: well-formed package names its title\n"); fflush(stdout);
    snprintf(path, sizeof(path), "%s%cpbp-package.iso", test_dir, nk_platform_path_separator());
    sfo_size = build_sfo_test(sfo, sizeof(sfo), "TEST00424", "Hostile PBP Title");
    assert(sfo_size > 0);
    offsets[0] = 40;
    for (index = 1; index < 8; index++) offsets[index] = (uint32_t)(40 + sfo_size);
    build_pbp_header_test(header, offsets);
    write_pbp_test(path, header, sizeof(header), sfo, sfo_size);
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(path, &meta) == NK_ERROR_INVALID_ISO);
    assert(strcmp(meta.boundary_code, "PBP_PACKAGE_UNSUPPORTED") == 0);
    assert(strstr(meta.error_message, "Hostile PBP Title") != NULL);
    assert(strstr(meta.error_message, "TEST00424") != NULL);
    assert(strstr(meta.error_message, "disc image") != NULL);
    assert(strchr(meta.error_message, '#') == NULL);
    assert(strcmp(meta.disc_id, "TEST00424") == 0);

    printf("[HOSTILE_TEST] PBP subtest 2: package without a TITLE reports the ID only\n"); fflush(stdout);
    snprintf(path, sizeof(path), "%s%cpbp-no-title.iso", test_dir, nk_platform_path_separator());
    sfo_size = build_sfo_test(sfo, sizeof(sfo), "TEST00424", "");
    assert(sfo_size > 0);
    offsets[0] = 40;
    for (index = 1; index < 8; index++) offsets[index] = (uint32_t)(40 + sfo_size);
    build_pbp_header_test(header, offsets);
    write_pbp_test(path, header, sizeof(header), sfo, sfo_size);
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(path, &meta) == NK_ERROR_INVALID_ISO);
    assert(strcmp(meta.boundary_code, "PBP_PACKAGE_UNSUPPORTED") == 0);
    assert(strstr(meta.error_message, "ID: TEST00424") != NULL);
    assert(strstr(meta.error_message, "Title:") == NULL);

    printf("[HOSTILE_TEST] PBP subtest 3: header shorter than 40 bytes\n"); fflush(stdout);
    snprintf(path, sizeof(path), "%s%cpbp-short-header.iso", test_dir, nk_platform_path_separator());
    {
        static const uint8_t short_header[8] = { 0x00, 'P', 'B', 'P', 0x00, 0x00, 0x01, 0x00 };
        write_pbp_test(path, short_header, sizeof(short_header), NULL, 0);
    }
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(path, &meta) == NK_ERROR_INVALID_ISO);
    assert(strcmp(meta.boundary_code, "PBP_HEADER_TRUNCATED") == 0);

    printf("[HOSTILE_TEST] PBP subtest 4: descending section offsets\n"); fflush(stdout);
    snprintf(path, sizeof(path), "%s%cpbp-descending.iso", test_dir, nk_platform_path_separator());
    sfo_size = build_sfo_test(sfo, sizeof(sfo), "TEST00424", "Descending");
    assert(sfo_size > 0);
    offsets[0] = 40; offsets[1] = 16;
    for (index = 2; index < 8; index++) offsets[index] = (uint32_t)(40 + sfo_size);
    build_pbp_header_test(header, offsets);
    write_pbp_test(path, header, sizeof(header), sfo, sfo_size);
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(path, &meta) == NK_ERROR_INVALID_ISO);
    assert(strcmp(meta.boundary_code, "PBP_OFFSETS_INVALID") == 0);

    printf("[HOSTILE_TEST] PBP subtest 5: section table pointing past the file\n"); fflush(stdout);
    snprintf(path, sizeof(path), "%s%cpbp-past-eof.iso", test_dir, nk_platform_path_separator());
    sfo_size = build_sfo_test(sfo, sizeof(sfo), "TEST00424", "Past EOF");
    assert(sfo_size > 0);
    for (index = 0; index < 7; index++) {
        offsets[index] = index == 0 ? 40u : (uint32_t)(40 + sfo_size);
    }
    offsets[7] = (uint32_t)(40 + sfo_size + 4096u);
    build_pbp_header_test(header, offsets);
    write_pbp_test(path, header, sizeof(header), sfo, sfo_size);
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(path, &meta) == NK_ERROR_INVALID_ISO);
    assert(strcmp(meta.boundary_code, "PBP_OFFSETS_INVALID") == 0);

    printf("[HOSTILE_TEST] PBP subtest 6: empty PARAM.SFO section\n"); fflush(stdout);
    snprintf(path, sizeof(path), "%s%cpbp-empty-sfo.iso", test_dir, nk_platform_path_separator());
    for (index = 0; index < 8; index++) offsets[index] = 40;
    build_pbp_header_test(header, offsets);
    write_pbp_test(path, header, sizeof(header), NULL, 0);
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(path, &meta) == NK_ERROR_INVALID_ISO);
    assert(strcmp(meta.boundary_code, "PBP_SFO_INVALID") == 0);

    printf("[HOSTILE_TEST] PBP subtest 7: PARAM.SFO with the wrong magic\n"); fflush(stdout);
    snprintf(path, sizeof(path), "%s%cpbp-bad-magic.iso", test_dir, nk_platform_path_separator());
    memset(sfo, 0, 32);
    sfo[1] = 'X';
    offsets[0] = 40;
    for (index = 1; index < 8; index++) offsets[index] = 40 + 32;
    build_pbp_header_test(header, offsets);
    write_pbp_test(path, header, sizeof(header), sfo, 32);
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(path, &meta) == NK_ERROR_INVALID_ISO);
    assert(strcmp(meta.boundary_code, "PBP_SFO_INVALID") == 0);

    printf("[HOSTILE_TEST] PBP subtest 8: PARAM.SFO without a usable DISC_ID\n"); fflush(stdout);
    snprintf(path, sizeof(path), "%s%cpbp-no-disc-id.iso", test_dir, nk_platform_path_separator());
    sfo_size = build_sfo_test(sfo, sizeof(sfo), "", "No Disc ID");
    assert(sfo_size > 0);
    offsets[0] = 40;
    for (index = 1; index < 8; index++) offsets[index] = (uint32_t)(40 + sfo_size);
    build_pbp_header_test(header, offsets);
    write_pbp_test(path, header, sizeof(header), sfo, sfo_size);
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(path, &meta) == NK_ERROR_INVALID_ISO);
    assert(strcmp(meta.boundary_code, "PBP_SFO_INVALID") == 0);

    printf("[HOSTILE_TEST] PBP subtest 9: a small non-PBP file keeps the ordinary error\n"); fflush(stdout);
    snprintf(path, sizeof(path), "%s%cpbp-not-a-package.iso", test_dir, nk_platform_path_separator());
    write_test_file(path, "short data", 10);
    memset(&meta, 0, sizeof(meta));
    assert(nk_iso_inspect(path, &meta) == NK_ERROR_INVALID_ISO);
    assert(meta.boundary_code[0] == '\0');
    assert(strstr(meta.error_message, "too small") != NULL);

    printf("[HOSTILE_TEST] PBP package tests PASSED!\n");
}

static void make_plain_mips_elf_test(uint8_t elf[88]) {
    memset(elf, 0, 88);
    memcpy(elf, "\x7f" "ELF", 4);
    elf[4] = 1; /* ELF32 */
    elf[5] = 1; /* little-endian */
    elf[6] = 1;
    write_le16_test(elf + 16, 2); /* ET_EXEC */
    write_le16_test(elf + 18, 8); /* EM_MIPS */
    write_le32_test(elf + 20, 1);
    write_le32_test(elf + 24, 0x08800000u);
    write_le32_test(elf + 28, 52);
    write_le16_test(elf + 40, 52);
    write_le16_test(elf + 42, 32);
    write_le16_test(elf + 44, 1);
    write_le32_test(elf + 52, 1); /* PT_LOAD */
    write_le32_test(elf + 56, 84);
    write_le32_test(elf + 60, 0x08800000u);
    write_le32_test(elf + 64, 0x08800000u);
    write_le32_test(elf + 68, 4);
    write_le32_test(elf + 72, 4);
    write_le32_test(elf + 76, 5); /* PF_R | PF_X */
    write_le32_test(elf + 80, 4);
    elf[84] = 0x34; elf[85] = 0x12; elf[86] = 0x00; elf[87] = 0x00;
}

static NkIsoExecutableReport classify_executable_fixture(
    const char *test_dir, const char *name,
    const void *eboot, size_t eboot_size, const void *boot, size_t boot_size) {
    char path[600];
    NkIsoExecutableReport report;
    snprintf(path, sizeof(path), "%s%c%s.iso", test_dir,
             nk_platform_path_separator(), name);
    build_executable_iso_test(path, eboot, eboot_size, boot, boot_size);
    assert(nk_iso_classify_executables(path, &report) == NK_OK);
    return report;
}

static void test_iso_executable_classification(const char *test_dir) {
    uint8_t elf[88], encrypted[0x80] = { 0 }, wrapper[32] = { 0 };
    uint8_t pbp[32] = { 0 }, zeros[64] = { 0 }, unknown[16] = { 1 };
    uint8_t hostile_elf[88] = { 0 }, truncated_psp[16] = { 0 };
    make_plain_mips_elf_test(elf);
    memcpy(encrypted, "~PSP", 4);
    encrypted[0x27] = 1;
    write_le32_test(encrypted + 0x54, 0x1000);
    memcpy(wrapper, "~SCE", 4);
    memcpy(pbp, "\0PBP", 4);
    memcpy(hostile_elf, "\x7f" "ELF", 4);
    hostile_elf[4] = 1; hostile_elf[5] = 1; hostile_elf[6] = 1;
    write_le16_test(hostile_elf + 16, 2);
    write_le16_test(hostile_elf + 18, 8);
    write_le32_test(hostile_elf + 20, 1);
    write_le32_test(hostile_elf + 28, UINT32_MAX - 3u);
    write_le16_test(hostile_elf + 40, 52);
    write_le16_test(hostile_elf + 42, 32);
    write_le16_test(hostile_elf + 44, 2);
    memcpy(truncated_psp, "~PSP", 4);

    printf("[HOSTILE_TEST] Testing ISO executable classification and BOOT fallback...\n");
    NkIsoExecutableReport report = classify_executable_fixture(
        test_dir, "exec_plain_elf", elf, sizeof(elf), NULL, 0);
    assert(report.eboot.kind == NK_ISO_EXEC_MIPS_ELF32);
    assert(report.selected == NK_ISO_EXEC_SELECTION_EBOOT);
    assert(strcmp(report.selected_path, "EBOOT.BIN") == 0);

    report = classify_executable_fixture(test_dir, "exec_boot_fallback",
                                         encrypted, sizeof(encrypted), elf, sizeof(elf));
    assert(report.eboot.kind == NK_ISO_EXEC_PSP_ENCRYPTED);
    assert(report.boot.kind == NK_ISO_EXEC_MIPS_ELF32);
    assert(report.selected == NK_ISO_EXEC_SELECTION_BOOT && report.boot_fallback);
    assert(strcmp(report.selected_path, "BOOT.BIN") == 0);

    report = classify_executable_fixture(test_dir, "exec_encrypted",
                                         encrypted, sizeof(encrypted), NULL, 0);
    assert(report.eboot.kind == NK_ISO_EXEC_PSP_ENCRYPTED);
    assert(report.selected == NK_ISO_EXEC_SELECTION_NONE);

    report = classify_executable_fixture(test_dir, "exec_sce_wrapper",
                                         wrapper, sizeof(wrapper), NULL, 0);
    assert(report.eboot.kind == NK_ISO_EXEC_SCE_WRAPPER);
    report = classify_executable_fixture(test_dir, "exec_empty",
                                         NULL, 0, NULL, 0);
    assert(report.eboot.kind == NK_ISO_EXEC_EMPTY_OR_ZERO);
    report = classify_executable_fixture(test_dir, "exec_zero_filled",
                                         zeros, sizeof(zeros), NULL, 0);
    assert(report.eboot.kind == NK_ISO_EXEC_EMPTY_OR_ZERO);
    report = classify_executable_fixture(test_dir, "exec_pbp",
                                         pbp, sizeof(pbp), NULL, 0);
    assert(report.eboot.kind == NK_ISO_EXEC_PBP);
    report = classify_executable_fixture(test_dir, "exec_unknown",
                                         unknown, sizeof(unknown), NULL, 0);
    assert(report.eboot.kind == NK_ISO_EXEC_UNKNOWN);
    report = classify_executable_fixture(test_dir, "exec_hostile_elf",
                                         hostile_elf, sizeof(hostile_elf), NULL, 0);
    assert(report.eboot.kind == NK_ISO_EXEC_UNKNOWN);
    report = classify_executable_fixture(test_dir, "exec_truncated_psp",
                                         truncated_psp, sizeof(truncated_psp), NULL, 0);
    assert(report.eboot.kind == NK_ISO_EXEC_UNKNOWN);
}

typedef struct {
    char paths[32][256];
    size_t count;
    size_t stop_after;
} TestWalkCtx;

static bool test_module_walk_cb(const char *member_path, const NkIsoDirEntry *entry, void *userdata) {
    TestWalkCtx *ctx = (TestWalkCtx *)userdata;
    (void)entry;
    if (ctx->count < 32) {
        snprintf(ctx->paths[ctx->count], sizeof(ctx->paths[0]), "%s", member_path);
    }
    ctx->count++;
    if (ctx->stop_after > 0 && ctx->count >= ctx->stop_after) {
        return false;
    }
    return true;
}

/* Every walk fixture uses the same geometry. The deepest record (the depth
 * case, LBA 27) fits inside these 28 sectors. */
#define WALK_SECTOR_SIZE 2048u
#define WALK_TOTAL_SECTORS 28u
#define WALK_SECTOR(image, lba) ((image) + (size_t)(lba) * WALK_SECTOR_SIZE)

/* Each case builds a fresh, zeroed image holding only the PVD and an empty
 * root. The case then adds exactly the records it tests, so no record from an
 * earlier case can leak into a later one. */
static uint8_t *walk_image_new(void) {
    uint8_t *image = (uint8_t *)calloc(WALK_TOTAL_SECTORS, WALK_SECTOR_SIZE);
    assert(image != NULL);
    uint8_t *pvd = WALK_SECTOR(image, 16);
    pvd[0] = 0x01;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 0x01;
    write_both_endian_test(pvd + 158, 17);
    write_both_endian_test(pvd + 166, WALK_SECTOR_SIZE);
    write_dir_record(pvd, 156, "\0", 1, 17, WALK_SECTOR_SIZE, true);
    return image;
}

/* Writes the "." and ".." records every ISO directory starts with. */
static size_t walk_dot_records(uint8_t *dir, uint32_t self_lba, uint32_t parent_lba) {
    size_t off = append_iso_record_test(dir, 0, (const uint8_t *)"\0", 1,
                                        self_lba, WALK_SECTOR_SIZE, true);
    return append_iso_record_test(dir, off, (const uint8_t *)"\1", 1,
                                  parent_lba, WALK_SECTOR_SIZE, true);
}

/* Writes the image to its own file, opens it, and runs the module walk. */
static NkIsoModuleWalkStatus walk_image(const char *test_dir, const char *name,
                                        const uint8_t *image, TestWalkCtx *ctx) {
    char path[600];
    snprintf(path, sizeof(path), "%s%c%s", test_dir,
             nk_platform_path_separator(), name);
    write_test_file(path, image, WALK_TOTAL_SECTORS * WALK_SECTOR_SIZE);
    NkIsoReader *reader = nk_iso_reader_open(path);
    assert(reader != NULL);
    NkIsoModuleWalkStatus status =
        nk_iso_reader_walk_module_tree(reader, test_module_walk_cb, ctx);
    nk_iso_reader_close(reader);
    return status;
}

static void test_iso_walk_module_tree(const char *test_dir) {
    printf("[HOSTILE_TEST] Testing nk_iso_reader_walk_module_tree...\n"); fflush(stdout);

    TestWalkCtx ctx;
    NkIsoModuleWalkStatus status;
    uint8_t *image;
    size_t off;

    /* 1. NULL reader or NULL callback is refused. The image has no module
     * roots, so an unguarded NULL callback would return OK, not the
     * argument error. */
    printf("[HOSTILE_TEST] Walk subtest 1: invalid arguments\n"); fflush(stdout);
    image = walk_image_new();
    walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    {
        char path[600];
        snprintf(path, sizeof(path), "%s%cwalk_args.iso", test_dir,
                 nk_platform_path_separator());
        write_test_file(path, image, WALK_TOTAL_SECTORS * WALK_SECTOR_SIZE);
        NkIsoReader *reader = nk_iso_reader_open(path);
        assert(reader != NULL);
        memset(&ctx, 0, sizeof(ctx));
        status = nk_iso_reader_walk_module_tree(NULL, test_module_walk_cb, &ctx);
        assert(status == NK_ISO_MODULE_WALK_INVALID_ARGUMENT);
        status = nk_iso_reader_walk_module_tree(reader, NULL, NULL);
        assert(status == NK_ISO_MODULE_WALK_INVALID_ARGUMENT);
        nk_iso_reader_close(reader);
    }
    free(image);

    /* 2. A callback that returns false ends the walk at once. The same
     * two-file fixture walked without a stop reports both files, so the
     * stopped run shows the stop itself, not an empty tree. */
    printf("[HOSTILE_TEST] Walk subtest 2: callback stops walk\n"); fflush(stdout);
    image = walk_image_new();
    off = walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                           18, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 18), 18, 17);
    append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"SYSDIR", 6,
                           19, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 19), 19, 18);
    off = append_iso_record_test(WALK_SECTOR(image, 19), off, (const uint8_t *)"A.PRX;1", 7,
                                 20, 100, false);
    append_iso_record_test(WALK_SECTOR(image, 19), off, (const uint8_t *)"B.PRX;1", 7,
                           21, 100, false);
    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_callback.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_OK);
    assert(ctx.count == 2);
    memset(&ctx, 0, sizeof(ctx));
    ctx.stop_after = 1;
    status = walk_image(test_dir, "walk_callback.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_CALLBACK_STOPPED);
    assert(ctx.count == 1);
    assert(strcmp(ctx.paths[0], "PSP_GAME/SYSDIR/A.PRX") == 0);
    free(image);

    /* 3. No PSP_GAME record means there is no module tree. That is OK with no
     * members, because both module roots are optional. */
    printf("[HOSTILE_TEST] Walk subtest 3: missing PSP_GAME directory\n"); fflush(stdout);
    image = walk_image_new();
    walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_no_psp_game.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_OK);
    assert(ctx.count == 0);
    free(image);

    /* 4. PSP_GAME as a regular file is not a module tree: OK with no members.
     * The game sector stays zero, so the walk could reach nothing else even if
     * the file were misread as a directory. */
    printf("[HOSTILE_TEST] Walk subtest 4: PSP_GAME is a file\n"); fflush(stdout);
    image = walk_image_new();
    off = walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                           18, 100, false);
    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_game_file.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_OK);
    assert(ctx.count == 0);
    free(image);

    /* 5. Two PSP_GAME records in the root -> INVALID_TREE, before any member is
     * reported. The second record is a directory too, so only the duplicate
     * check can reject this image. */
    printf("[HOSTILE_TEST] Walk subtest 5: duplicate PSP_GAME directory\n"); fflush(stdout);
    image = walk_image_new();
    off = walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    off = append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                                 18, WALK_SECTOR_SIZE, true);
    append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                           18, WALK_SECTOR_SIZE, true);
    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_dup_psp_game.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_INVALID_TREE);
    assert(ctx.count == 0);
    free(image);

    /* 6. Two SYSDIR records in PSP_GAME -> INVALID_TREE, before any member is
     * reported. The sysdir holds a walkable file, so an unguarded duplicate
     * would walk it and report a member instead. */
    printf("[HOSTILE_TEST] Walk subtest 6: duplicate SYSDIR in PSP_GAME\n"); fflush(stdout);
    image = walk_image_new();
    off = walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                           18, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 18), 18, 17);
    off = append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"SYSDIR", 6,
                                 19, WALK_SECTOR_SIZE, true);
    append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"SYSDIR", 6,
                           19, WALK_SECTOR_SIZE, true);
    walk_dot_records(WALK_SECTOR(image, 19), 19, 18);
    append_iso_record_test(WALK_SECTOR(image, 19), 68, (const uint8_t *)"X.PRX;1", 7,
                           20, 100, false);
    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_dup_sysdir.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_INVALID_TREE);
    assert(ctx.count == 0);
    free(image);

    /* 7. Recursion depth. Directories below USRDIR are walked while their
     * depth is under NK_ISO_MODULE_TREE_MAX_DEPTH (4). D4 is the last level
     * walked, so AT_LIMIT.PRX inside it is reported. D5 is one level past the
     * limit, so DEEPMOD.PRX inside it is not. The reported file proves the
     * recursion works; the missing one proves the bound. */
    printf("[HOSTILE_TEST] Walk subtest 7: recursion depth limit\n"); fflush(stdout);
    image = walk_image_new();
    off = walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                           18, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 18), 18, 17);
    append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"USRDIR", 6,
                           20, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 20), 20, 18);
    append_iso_record_test(WALK_SECTOR(image, 20), off, (const uint8_t *)"D1", 2,
                           21, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 21), 21, 20);
    append_iso_record_test(WALK_SECTOR(image, 21), off, (const uint8_t *)"D2", 2,
                           22, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 22), 22, 21);
    append_iso_record_test(WALK_SECTOR(image, 22), off, (const uint8_t *)"D3", 2,
                           23, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 23), 23, 22);
    append_iso_record_test(WALK_SECTOR(image, 23), off, (const uint8_t *)"D4", 2,
                           24, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 24), 24, 23);
    off = append_iso_record_test(WALK_SECTOR(image, 24), off, (const uint8_t *)"D5", 2,
                                 25, WALK_SECTOR_SIZE, true);
    append_iso_record_test(WALK_SECTOR(image, 24), off, (const uint8_t *)"AT_LIMIT.PRX;1", 14,
                           26, 100, false);
    walk_dot_records(WALK_SECTOR(image, 25), 25, 24);
    append_iso_record_test(WALK_SECTOR(image, 25), 68, (const uint8_t *)"DEEPMOD.PRX;1", 13,
                           27, 100, false);
    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_depth.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_OK);
    assert(ctx.count == 1);
    assert(strcmp(ctx.paths[0], "PSP_GAME/USRDIR/D1/D2/D3/D4/AT_LIMIT.PRX") == 0);
    free(image);

    /* 8. Full walk: visits both SYSDIR and USRDIR modules */
    printf("[HOSTILE_TEST] Walk subtest 8: full walk SYSDIR and USRDIR\n"); fflush(stdout);
    image = walk_image_new();
    off = walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                           18, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 18), 18, 17);
    off = append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"SYSDIR", 6,
                                 19, WALK_SECTOR_SIZE, true);
    append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"USRDIR", 6,
                           20, WALK_SECTOR_SIZE, true);
    walk_dot_records(WALK_SECTOR(image, 19), 19, 18);
    append_iso_record_test(WALK_SECTOR(image, 19), 68, (const uint8_t *)"EBOOT.BIN;1", 11,
                           21, 100, false);
    walk_dot_records(WALK_SECTOR(image, 20), 20, 18);
    append_iso_record_test(WALK_SECTOR(image, 20), 68, (const uint8_t *)"MODULE.PRX;1", 12,
                           22, 100, false);
    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_full.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_OK);
    assert(ctx.count == 2);
    assert(strcmp(ctx.paths[0], "PSP_GAME/SYSDIR/EBOOT.BIN") == 0);
    assert(strcmp(ctx.paths[1], "PSP_GAME/USRDIR/MODULE.PRX") == 0);
    free(image);

    /* 9. KMODULE subtree skip (case-insensitive) */
    printf("[HOSTILE_TEST] Walk subtest 9: KMODULE directory subtree skipping\n"); fflush(stdout);
    image = walk_image_new();
    off = walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                           18, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 18), 18, 17);
    off = append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"SYSDIR", 6,
                                 19, WALK_SECTOR_SIZE, true);
    append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"USRDIR", 6,
                           20, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 19), 19, 18);
    off = append_iso_record_test(WALK_SECTOR(image, 19), off, (const uint8_t *)"KMODULE", 7,
                                 21, WALK_SECTOR_SIZE, true);
    append_iso_record_test(WALK_SECTOR(image, 19), off, (const uint8_t *)"SYS.PRX;1", 9,
                           22, 100, false);
    walk_dot_records(WALK_SECTOR(image, 21), 21, 19);
    append_iso_record_test(WALK_SECTOR(image, 21), 68, (const uint8_t *)"SKIPPED.PRX;1", 13,
                           23, 100, false);
    off = walk_dot_records(WALK_SECTOR(image, 20), 20, 18);
    off = append_iso_record_test(WALK_SECTOR(image, 20), off, (const uint8_t *)"kmodule", 7,
                                 24, WALK_SECTOR_SIZE, true);
    append_iso_record_test(WALK_SECTOR(image, 20), off, (const uint8_t *)"USR.PRX;1", 9,
                           25, 100, false);
    walk_dot_records(WALK_SECTOR(image, 24), 24, 20);
    append_iso_record_test(WALK_SECTOR(image, 24), 68, (const uint8_t *)"SKIPPED2.PRX;1", 14,
                           26, 100, false);
    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_kmodule.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_OK);
    assert(ctx.count == 2);
    assert(strcmp(ctx.paths[0], "PSP_GAME/SYSDIR/SYS.PRX") == 0);
    assert(strcmp(ctx.paths[1], "PSP_GAME/USRDIR/USR.PRX") == 0);
    free(image);

    /* 10. Excess directory count limit */
    printf("[HOSTILE_TEST] Walk subtest 10: directory limit\n"); fflush(stdout);
    {
        const size_t total_sec = 60;
        uint8_t *big_image = (uint8_t *)calloc(total_sec, WALK_SECTOR_SIZE);
        assert(big_image != NULL);
        uint8_t *pvd = big_image + 16 * WALK_SECTOR_SIZE;
        pvd[0] = 0x01;
        memcpy(pvd + 1, "CD001", 5);
        pvd[6] = 0x01;
        write_both_endian_test(pvd + 158, 17);
        write_both_endian_test(pvd + 166, WALK_SECTOR_SIZE);
        write_dir_record(pvd, 156, "\0", 1, 17, WALK_SECTOR_SIZE, true);

        off = walk_dot_records(big_image + 17 * WALK_SECTOR_SIZE, 17, 17);
        append_iso_record_test(big_image + 17 * WALK_SECTOR_SIZE, off,
                               (const uint8_t *)"PSP_GAME", 8, 18, WALK_SECTOR_SIZE, true);
        off = walk_dot_records(big_image + 18 * WALK_SECTOR_SIZE, 18, 17);
        append_iso_record_test(big_image + 18 * WALK_SECTOR_SIZE, off,
                               (const uint8_t *)"USRDIR", 6, 19, WALK_SECTOR_SIZE, true);

        off = walk_dot_records(big_image + 19 * WALK_SECTOR_SIZE, 19, 18);
        for (uint32_t d = 0; d < 32; d++) {
            char dname[8];
            snprintf(dname, sizeof(dname), "D%02u", d);
            off = append_iso_record_test(big_image + 19 * WALK_SECTOR_SIZE, off,
                                           (const uint8_t *)dname, (uint8_t)strlen(dname),
                                           20 + d, WALK_SECTOR_SIZE, true);
        }

        for (uint32_t d = 0; d < 32; d++) {
            uint32_t lba = 20 + d;
            off = walk_dot_records(big_image + lba * WALK_SECTOR_SIZE, lba, 19);
            for (uint32_t s = 0; s < 33; s++) {
                char sname[8];
                snprintf(sname, sizeof(sname), "S%02u", s);
                off = append_iso_record_test(big_image + lba * WALK_SECTOR_SIZE, off,
                                               (const uint8_t *)sname, (uint8_t)strlen(sname),
                                               52, WALK_SECTOR_SIZE, true);
            }
        }
        walk_dot_records(big_image + 52 * WALK_SECTOR_SIZE, 52, 20);

        char path[600];
        snprintf(path, sizeof(path), "%s%cwalk_dir_limit.iso", test_dir,
                 nk_platform_path_separator());
        write_test_file(path, big_image, total_sec * WALK_SECTOR_SIZE);
        NkIsoReader *reader = nk_iso_reader_open(path);
        assert(reader != NULL);
        memset(&ctx, 0, sizeof(ctx));
        status = nk_iso_reader_walk_module_tree(reader, test_module_walk_cb, &ctx);
        assert(status == NK_ISO_MODULE_WALK_DIRECTORY_LIMIT);
        nk_iso_reader_close(reader);
        free(big_image);
    }

    /* 11. Path length limit */
    printf("[HOSTILE_TEST] Walk subtest 11: overlong path lookup limit\n"); fflush(stdout);
    image = walk_image_new();
    off = walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                           18, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 18), 18, 17);
    append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"USRDIR", 6,
                           19, WALK_SECTOR_SIZE, true);

    char long_d1[241], long_d2[241], long_d3[241], long_d4[241], long_file[241];
    memset(long_d1, 'A', 240); long_d1[240] = '\0';
    memset(long_d2, 'B', 240); long_d2[240] = '\0';
    memset(long_d3, 'C', 240); long_d3[240] = '\0';
    memset(long_d4, 'D', 240); long_d4[240] = '\0';
    memset(long_file, 'F', 230);
    memcpy(long_file + 230, ".PRX;1", 6);
    long_file[236] = '\0';

    off = walk_dot_records(WALK_SECTOR(image, 19), 19, 18);
    append_iso_record_test(WALK_SECTOR(image, 19), off, (const uint8_t *)long_d1, 240,
                           20, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 20), 20, 19);
    append_iso_record_test(WALK_SECTOR(image, 20), off, (const uint8_t *)long_d2, 240,
                           21, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 21), 21, 20);
    append_iso_record_test(WALK_SECTOR(image, 21), off, (const uint8_t *)long_d3, 240,
                           22, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 22), 22, 21);
    append_iso_record_test(WALK_SECTOR(image, 22), off, (const uint8_t *)long_d4, 240,
                           23, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 23), 23, 22);
    append_iso_record_test(WALK_SECTOR(image, 23), off, (const uint8_t *)long_file, 236,
                           24, 100, false);

    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_path_limit.iso", image, &ctx);
     assert(status == NK_ISO_MODULE_WALK_INVALID_TREE);
    free(image);

    /* 12. Invalid tree structure */
    printf("[HOSTILE_TEST] Walk subtest 12: invalid tree structure\n"); fflush(stdout);
    image = walk_image_new();
    off = walk_dot_records(WALK_SECTOR(image, 17), 17, 17);
    append_iso_record_test(WALK_SECTOR(image, 17), off, (const uint8_t *)"PSP_GAME", 8,
                           18, WALK_SECTOR_SIZE, true);
    off = walk_dot_records(WALK_SECTOR(image, 18), 18, 17);
    append_iso_record_test(WALK_SECTOR(image, 18), off, (const uint8_t *)"SYSDIR", 6,
                           19, 100, false);
    memset(&ctx, 0, sizeof(ctx));
    status = walk_image(test_dir, "walk_invalid_tree.iso", image, &ctx);
    assert(status == NK_ISO_MODULE_WALK_INVALID_TREE);
    free(image);

    printf("[HOSTILE_TEST] nk_iso_reader_walk_module_tree tests PASSED!\n");
    fflush(stdout);
}


#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>

/* Invalid UTF-8 must never be "repaired" into U+FFFD and then resolve to a
 * different file. A decoy named U+FFFD.iso sits next to the requested path; a
 * lenient conversion would find it and report it as a malformed ISO. */
static void test_invalid_utf8_path_is_refused(const char *test_dir) {
    printf("[HOSTILE_TEST] Testing invalid UTF-8 path refusal...\n");
    fflush(stdout);

    WCHAR wdir[1024];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, test_dir, -1,
                               wdir, (int)(sizeof(wdir) / sizeof(wdir[0]))) > 0);
    WCHAR wdecoy[1100];
    _snwprintf(wdecoy, sizeof(wdecoy) / sizeof(wdecoy[0]), L"%ls\\\xFFFD.iso", wdir);
    wdecoy[(sizeof(wdecoy) / sizeof(wdecoy[0])) - 1] = L'\0';
    FILE *decoy = _wfopen(wdecoy, L"wb");
    assert(decoy != NULL);
    fputs("not an iso", decoy);
    fclose(decoy);

    char bad_path[600];
    snprintf(bad_path, sizeof(bad_path), "%s\\\xff.iso", test_dir);

    assert(!nk_platform_file_exists(bad_path));
    NkIsoMetadata meta;
    NkResult res = nk_iso_inspect(bad_path, &meta);
    assert(res == NK_ERROR_FILE_NOT_FOUND);

    assert(DeleteFileW(wdecoy));

    const char library_json[] =
        "{\"schema_version\":1,\"games\":[{\"disc_id\":\"TEST00001\"}]}";
    const WCHAR *suffixes[] = {L"", L".tmp", L".bak"};
    WCHAR library_decoys[3][1100];
    for (int i = 0; i < 3; i++) {
        int written = _snwprintf(library_decoys[i], 1100,
                                L"%ls%lc\xFFFD.json%ls", wdir, (wint_t)nk_platform_path_separator(), suffixes[i]);
        assert(written > 0 && written < 1100);
        decoy = _wfopen(library_decoys[i], L"wb");
        assert(decoy != NULL);
        assert(fwrite(library_json, 1, sizeof(library_json) - 1, decoy) == sizeof(library_json) - 1);
        assert(fclose(decoy) == 0);
    }

    char valid_path[600];
    snprintf(valid_path, sizeof(valid_path), "%s%c\xef\xbf\xbd.json", test_dir, nk_platform_path_separator());
    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    assert(nk_library_load(&lib, valid_path) == NK_OK);
    assert(nk_library_count(&lib) == 1);
    snprintf(bad_path, sizeof(bad_path), "%s%c\xff.json", test_dir, nk_platform_path_separator());
    assert(nk_library_load(&lib, bad_path) == NK_OK);
    assert(nk_library_count(&lib) == 0);
    assert(nk_library_save(&lib, bad_path) == NK_ERROR_IO);
    assert(nk_library_save(&lib, NULL) == NK_ERROR_IO);

    for (int i = 0; i < 3; i++) {
        char contents[sizeof(library_json)];
        decoy = _wfopen(library_decoys[i], L"rb");
        assert(decoy != NULL);
        size_t count = fread(contents, 1, sizeof(contents), decoy);
        assert(count == sizeof(library_json) - 1);
        assert(memcmp(contents, library_json, count) == 0);
        assert(fclose(decoy) == 0);
        assert(DeleteFileW(library_decoys[i]));
    }
    printf("[HOSTILE_TEST] Invalid UTF-8 ISO and library paths refused.\n");
}
#endif

/* A staging ISO: PSP_GAME/SYSDIR/EBOOT.BIN plus PSP_GAME/USRDIR/DATA holding one
 * member (a file, or a directory holding X.BIN). Optional unstaged siblings sit beside
 * EBOOT.BIN in SYSDIR and beside DATA in USRDIR. Each case writes its own image. */
static void build_stage_iso_test(const char *path, const char *member, bool member_is_dir,
                                 const char *sysdir_sibling, const char *usrdir_sibling) {
    const size_t sector_size = 2048;
    const size_t image_size = 25u * sector_size;
    uint8_t *image = (uint8_t *)calloc(image_size, 1);
    assert(image != NULL);

    uint8_t *pvd = image + 16u * sector_size;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    write_both_endian_test(pvd + 158, 17);
    write_both_endian_test(pvd + 166, (uint32_t)sector_size);

    const uint8_t dot[] = { 0 }, dotdot[] = { 1 };
    const uint32_t dir = (uint32_t)sector_size;
    uint8_t *root = image + 17u * sector_size;
    uint8_t *game = image + 18u * sector_size;
    uint8_t *sysdir = image + 19u * sector_size;
    uint8_t *usrdir = image + 21u * sector_size;
    uint8_t *data = image + 22u * sector_size;
    size_t used = 0;
    used = append_iso_record_test(root, used, dot, 1, 17, dir, true);
    used = append_iso_record_test(root, used, dotdot, 1, 17, dir, true);
    append_iso_record_test(root, used, (const uint8_t *)"PSP_GAME", 8, 18, dir, true);

    used = 0;
    used = append_iso_record_test(game, used, dot, 1, 18, dir, true);
    used = append_iso_record_test(game, used, dotdot, 1, 17, dir, true);
    used = append_iso_record_test(game, used, (const uint8_t *)"SYSDIR", 6, 19, dir, true);
    append_iso_record_test(game, used, (const uint8_t *)"USRDIR", 6, 21, dir, true);

    used = 0;
    used = append_iso_record_test(sysdir, used, dot, 1, 19, dir, true);
    used = append_iso_record_test(sysdir, used, dotdot, 1, 18, dir, true);
    used = append_iso_record_test(sysdir, used, (const uint8_t *)"EBOOT.BIN;1", 11, 20, 5, false);
    if (sysdir_sibling)
        append_iso_record_test(sysdir, used, (const uint8_t *)sysdir_sibling,
                               (uint8_t)strlen(sysdir_sibling), 23, 7, false);
    memcpy(image + 20u * sector_size, "EBOOT", 5);

    used = 0;
    used = append_iso_record_test(usrdir, used, dot, 1, 21, dir, true);
    used = append_iso_record_test(usrdir, used, dotdot, 1, 18, dir, true);
    used = append_iso_record_test(usrdir, used, (const uint8_t *)"DATA", 4, 22, dir, true);
    if (usrdir_sibling)
        append_iso_record_test(usrdir, used, (const uint8_t *)usrdir_sibling,
                               (uint8_t)strlen(usrdir_sibling), 23, 7, false);

    used = 0;
    used = append_iso_record_test(data, used, dot, 1, 22, dir, true);
    used = append_iso_record_test(data, used, dotdot, 1, 21, dir, true);
    if (member_is_dir) {
        uint8_t *child = image + 24u * sector_size;
        append_iso_record_test(data, used, (const uint8_t *)member, (uint8_t)strlen(member),
                               24, dir, true);
        used = 0;
        used = append_iso_record_test(child, used, dot, 1, 24, dir, true);
        used = append_iso_record_test(child, used, dotdot, 1, 22, dir, true);
        append_iso_record_test(child, used, (const uint8_t *)"X.BIN;1", 7, 23, 7, false);
    } else {
        append_iso_record_test(data, used, (const uint8_t *)member, (uint8_t)strlen(member),
                               23, 7, false);
    }
    memcpy(image + 23u * sector_size, "payload", 7);
    write_test_file(path, image, image_size);
    free(image);
}

/* Staged disc members become host entries under their disc names, so a member whose base name
 * is a reserved Windows device name is refused (the portability rule tools/extract_xb.py
 * applies; on Windows 11 build 26300 "NUL", "NUL." and "NUL " even swallow their bytes). The
 * refusal happens while
 * the disc is counted, before anything is written, on every host. Records the stager never
 * writes do not refuse the disc. */
static void stage_one(const char *test_dir, const char *tag, const char *member, bool is_dir,
                      const char *sysdir_sibling, const char *usrdir_sibling,
                      const char *const *roots, NkResult expected, char *host_root,
                      size_t host_root_size) {
    const char sep = nk_platform_path_separator();
    char iso_path[700];
    snprintf(iso_path, sizeof(iso_path), "%s%cstage_%s.iso", test_dir, sep, tag);
    snprintf(host_root, host_root_size, "%s%cstage_%s", test_dir, sep, tag);
    build_stage_iso_test(iso_path, member, is_dir, sysdir_sibling, usrdir_sibling);
    NkResult result = nk_iso_extract_game(iso_path, host_root, roots, 1, NULL, NULL);
    if (result != expected) {
        fprintf(stderr, "[HOSTILE_TEST] staging case %s (%s) gave %d, expected %d\n",
                tag, member, (int)result, (int)expected);
    }
    assert(result == expected);
    remove(iso_path);
}

static void test_iso_stage_refuses_device_names(const char *test_dir) {
    static const struct {
        const char *member;
        bool is_dir;
    } refused[] = {
        { "NUL.BIN;1", false },
        { "com1;1", false },        /* any case, no extension */
        { "CON.;1", false },        /* ISO9660 spelling of an extensionless file */
        { "LPT0.TXT;1", false },
        { "CONIN$.DAT;1", false },
        { "NUL ;1", false },        /* Win32 drops the trailing space: still NUL */
        { "NUL .BIN;1", false },    /* and the spaces before the extension */
        { "AUX", true },            /* a directory, not only a file */
    };
    const char sep = nk_platform_path_separator();
    const char *roots[] = { "DATA" };
    char host_root[700], probe[800], tag[32];
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        snprintf(tag, sizeof(tag), "device_%u", (unsigned)i);
        stage_one(test_dir, tag, refused[i].member, refused[i].is_dir, NULL, NULL, roots,
                  NK_ERROR_INVALID_ISO, host_root, sizeof(host_root));
        /* Refused while counting: the staging root was never created. */
        assert(!nk_platform_dir_exists(host_root));
    }

    /* Controls: names that only resemble a device stage normally. */
    static const char *const allowed[] = { "NULL.BIN;1", "CONSOLE.BIN;1", "COM10.BIN;1" };
    static const char *const allowed_host[] = { "NULL.BIN", "CONSOLE.BIN", "COM10.BIN" };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        snprintf(tag, sizeof(tag), "allowed_%u", (unsigned)i);
        stage_one(test_dir, tag, allowed[i], false, NULL, NULL, roots, NK_OK, host_root,
                  sizeof(host_root));
        snprintf(probe, sizeof(probe), "%s%cEBOOT.BIN", host_root, sep);
        assert(nk_platform_get_file_size(probe) == 5);
        snprintf(probe, sizeof(probe), "%s%cDATA%c%s", host_root, sep, sep, allowed_host[i]);
        assert(nk_platform_get_file_size(probe) == 7);
    }

    /* Device-named records the stager never writes do not refuse the disc. */
    stage_one(test_dir, "sibling", "OK.BIN;1", false, "CON;1", "NUL.BIN;1", roots, NK_OK,
              host_root, sizeof(host_root));
    snprintf(probe, sizeof(probe), "%s%cDATA%cOK.BIN", host_root, sep, sep);
    assert(nk_platform_get_file_size(probe) == 7);

    /* Under the "." root a member that Win32 would store as EBOOT.BIN (trailing dot or space)
     * is refused while counting, so the staged EBOOT.BIN is neither overwritten nor written. */
    static const char *const eboot_aliases[] = { "EBOOT.BIN.;1", "eboot.bin ;1", "EBOOT.BIN;1" };
    const char *dot_root[] = { "." };
    for (size_t i = 0; i < sizeof(eboot_aliases) / sizeof(eboot_aliases[0]); i++) {
        snprintf(tag, sizeof(tag), "eboot_alias_%u", (unsigned)i);
        stage_one(test_dir, tag, "OK.BIN;1", false, NULL, eboot_aliases[i], dot_root,
                  NK_ERROR_INVALID_ISO, host_root, sizeof(host_root));
        assert(!nk_platform_dir_exists(host_root));
    }
    /* The guard is scoped to the top level of ".": below it EBOOT.BIN is an ordinary member. */
    stage_one(test_dir, "eboot_nested", "EBOOT.BIN;1", false, NULL, NULL, dot_root, NK_OK,
              host_root, sizeof(host_root));
    snprintf(probe, sizeof(probe), "%s%cEBOOT.BIN", host_root, sep);
    assert(nk_platform_get_file_size(probe) == 5);
    snprintf(probe, sizeof(probe), "%s%cDATA%cEBOOT.BIN", host_root, sep, sep);
    assert(nk_platform_get_file_size(probe) == 7);

    /* A configured content root named after a device is refused before the disc is read. */
    const char *device_root[] = { "AUX" };
    stage_one(test_dir, "device_root", "OK.BIN;1", false, NULL, NULL, device_root,
              NK_ERROR_INVALID_ISO, host_root, sizeof(host_root));
    assert(!nk_platform_dir_exists(host_root));
    printf("[HOSTILE_TEST] ISO staging refuses Windows device member names.\n");
}

/* One file of the USRDIR/DATA directory of a staging ISO. Every file has its own extent. */
typedef struct {
    const char *name;   /* ISO identifier, for example "A;1.BIN" */
    const char *bytes;  /* content, one byte per character */
} StageDataFile;

/* A staging ISO: PSP_GAME/SYSDIR/EBOOT.BIN plus PSP_GAME/USRDIR/DATA holding the given files.
 * File i is written at LBA 23 + i, so byte-identical files never share an extent. */
static void build_stage_data_iso_test(const char *path, const StageDataFile *files, size_t count) {
    const size_t sector_size = 2048;
    const size_t image_size = (23u + count + 1u) * sector_size;
    uint8_t *image = (uint8_t *)calloc(image_size, 1);
    assert(image != NULL);

    uint8_t *pvd = image + 16u * sector_size;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    write_both_endian_test(pvd + 158, 17);
    write_both_endian_test(pvd + 166, (uint32_t)sector_size);

    const uint8_t dot[] = { 0 }, dotdot[] = { 1 };
    const uint32_t dir = (uint32_t)sector_size;
    uint8_t *root = image + 17u * sector_size;
    uint8_t *game = image + 18u * sector_size;
    uint8_t *sysdir = image + 19u * sector_size;
    uint8_t *usrdir = image + 21u * sector_size;
    uint8_t *data = image + 22u * sector_size;
    size_t used = 0;
    used = append_iso_record_test(root, used, dot, 1, 17, dir, true);
    used = append_iso_record_test(root, used, dotdot, 1, 17, dir, true);
    append_iso_record_test(root, used, (const uint8_t *)"PSP_GAME", 8, 18, dir, true);

    used = 0;
    used = append_iso_record_test(game, used, dot, 1, 18, dir, true);
    used = append_iso_record_test(game, used, dotdot, 1, 17, dir, true);
    used = append_iso_record_test(game, used, (const uint8_t *)"SYSDIR", 6, 19, dir, true);
    append_iso_record_test(game, used, (const uint8_t *)"USRDIR", 6, 21, dir, true);

    used = 0;
    used = append_iso_record_test(sysdir, used, dot, 1, 19, dir, true);
    used = append_iso_record_test(sysdir, used, dotdot, 1, 18, dir, true);
    append_iso_record_test(sysdir, used, (const uint8_t *)"EBOOT.BIN;1", 11, 20, 5, false);
    memcpy(image + 20u * sector_size, "EBOOT", 5);

    used = 0;
    used = append_iso_record_test(usrdir, used, dot, 1, 21, dir, true);
    used = append_iso_record_test(usrdir, used, dotdot, 1, 18, dir, true);
    append_iso_record_test(usrdir, used, (const uint8_t *)"DATA", 4, 22, dir, true);

    used = 0;
    used = append_iso_record_test(data, used, dot, 1, 22, dir, true);
    used = append_iso_record_test(data, used, dotdot, 1, 21, dir, true);
    for (size_t i = 0; i < count; i++) {
        uint32_t lba = (uint32_t)(23u + i);
        size_t length = strlen(files[i].bytes);
        used = append_iso_record_test(data, used, (const uint8_t *)files[i].name,
                                      (uint8_t)strlen(files[i].name), lba, (uint32_t)length,
                                      false);
        memcpy(image + (size_t)lba * sector_size, files[i].bytes, length);
    }
    write_test_file(path, image, image_size);
    free(image);
}

typedef struct {
    size_t total_files;
} StageProgress;

/* Records the file count the stager planned: the EBOOT.BIN file plus each member it writes. */
static bool stage_progress_record(const char *relative_path, uint64_t bytes_complete,
                                  uint64_t bytes_total, size_t files_complete,
                                  size_t total_files, void *userdata) {
    (void)relative_path;
    (void)bytes_complete;
    (void)bytes_total;
    (void)files_complete;
    ((StageProgress *)userdata)->total_files = total_files;
    return true;
}

/* True when the host file at path holds exactly the given bytes. */
static bool host_file_holds(const char *path, const char *bytes) {
    size_t length = strlen(bytes);
    char buf[64];
    if (length > sizeof(buf) || nk_platform_get_file_size(path) != (int64_t)length) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return got == length && memcmp(buf, bytes, length) == 0;
}

/* Stages a build_stage_data_iso_test image under host_root. The planned file count comes back
 * through total_files when it is not NULL. */
static void stage_data_files(const char *test_dir, const char *tag, const StageDataFile *files,
                             size_t count, const char *const *roots, NkResult expected,
                             char *host_root, size_t host_root_size, size_t *total_files) {
    const char sep = nk_platform_path_separator();
    char iso_path[700];
    snprintf(iso_path, sizeof(iso_path), "%s%cstage_%s.iso", test_dir, sep, tag);
    snprintf(host_root, host_root_size, "%s%cstage_%s", test_dir, sep, tag);
    build_stage_data_iso_test(iso_path, files, count);
    StageProgress progress;
    memset(&progress, 0, sizeof(progress));
    NkResult result = nk_iso_extract_game(iso_path, host_root, roots, 1,
                                          stage_progress_record, &progress);
    if (result != expected) {
        fprintf(stderr, "[HOSTILE_TEST] staging case %s gave %d, expected %d\n",
                tag, (int)result, (int)expected);
    }
    assert(result == expected);
    if (total_files) *total_files = progress.total_files;
    remove(iso_path);
}

/* A version is a ';' and digits at the very end of an identifier. A ';' inside a name is part of
 * the name, so "A;1.BIN" and "A;2.BIN" are two files, not two versions of "A". */
static void test_iso_stage_keeps_semicolon_names(const char *test_dir) {
    const char sep = nk_platform_path_separator();
    static const StageDataFile files[] = {
        { "A;1.BIN", "first-bytes" },
        { "A;2.BIN", "second-bytes" },
        { "FOO.BIN;1", "plain" },
    };
    static const char *const host_names[] = { "A;1.BIN", "A;2.BIN", "FOO.BIN" };
    const char *roots[] = { "DATA" };
    char iso_path[700], host_root[700], probe[800];
    snprintf(iso_path, sizeof(iso_path), "%s%csemicolon.iso", test_dir, sep);
    build_stage_data_iso_test(iso_path, files, sizeof(files) / sizeof(files[0]));

    /* Lookup resolves each name to its own extent, not to a sibling that shares its stem. */
    NkIsoReader *reader = nk_iso_reader_open(iso_path);
    assert(reader != NULL);
    static const char *const wanted[] = {
        "PSP_GAME/USRDIR/DATA/A;1.BIN", "PSP_GAME/USRDIR/DATA/A;2.BIN",
    };
    for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        uint32_t lba = 0, size = 0;
        bool is_dir = true;
        int found = nk_iso_reader_lookup(reader, wanted[i], &lba, &size, &is_dir);
        assert(found == 0);
        assert(!is_dir);
        assert(size == strlen(files[i].bytes));
        char buf[64] = { 0 };
        int got = nk_iso_reader_read(reader, lba, 0, buf, size);
        assert(got == (int)size);
        assert(memcmp(buf, files[i].bytes, size) == 0);
    }
    /* A different version of an existing name is not on the disc. */
    uint32_t lba = 0, size = 0;
    bool is_dir = false;
    int missing = nk_iso_reader_lookup(reader, "PSP_GAME/USRDIR/DATA/A;3.BIN", &lba, &size,
                                       &is_dir);
    assert(missing != 0);
    nk_iso_reader_close(reader);

    /* Staging writes each name to its own host file, with its own bytes. */
    snprintf(host_root, sizeof(host_root), "%s%cstage_semicolon", test_dir, sep);
    StageProgress progress;
    memset(&progress, 0, sizeof(progress));
    NkResult result = nk_iso_extract_game(iso_path, host_root, roots, 1,
                                          stage_progress_record, &progress);
    assert(result == NK_OK);
    assert(progress.total_files == 4);  /* EBOOT.BIN plus the three members */
    for (size_t i = 0; i < sizeof(host_names) / sizeof(host_names[0]); i++) {
        snprintf(probe, sizeof(probe), "%s%cDATA%c%s", host_root, sep, sep, host_names[i]);
        assert(host_file_holds(probe, files[i].bytes));
    }
    remove(iso_path);
}

/* Members of one directory whose host names collide. When their bytes differ, staging would
 * replace one file with the other, so the disc is refused and nothing is written. When they are
 * byte-identical, the duplicate is staged once, through the first member. */
static void test_iso_stage_collisions_by_content(const char *test_dir) {
    const char sep = nk_platform_path_separator();
    static const struct {
        const char *first;
        const char *second;
        const char *kept_host_name;
    } pairs[] = {
        { "FOO;1", "FOO.;1", "FOO" },    /* trailing dot */
        { "FOO;1", "FOO ;1", "FOO" },    /* trailing space */
        { "foo;1", "FOO;1", "foo" },     /* letter case */
        { "FOO;1", "FOO;2", "FOO" },     /* two versions of one member */
    };
    const char *roots[] = { "DATA" };
    char host_root[700], probe[800], tag[40];

    /* Differing content at the same size: refused, nothing written. */
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        StageDataFile files[2] = { { pairs[i].first, "aaaaaa" }, { pairs[i].second, "bbbbbb" } };
        snprintf(tag, sizeof(tag), "differ_%u", (unsigned)i);
        stage_data_files(test_dir, tag, files, 2, roots, NK_ERROR_INVALID_ISO, host_root,
                         sizeof(host_root), NULL);
        assert(!nk_platform_dir_exists(host_root));
    }
    /* Differing content at different sizes: refused, nothing written. */
    {
        StageDataFile files[2] = { { "FOO;1", "aaaa" }, { "foo;1", "bbbbbb" } };
        stage_data_files(test_dir, "differ_size", files, 2, roots, NK_ERROR_INVALID_ISO,
                         host_root, sizeof(host_root), NULL);
        assert(!nk_platform_dir_exists(host_root));
    }

    /* Byte-identical content at different extents: staged once, one host file. */
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        StageDataFile files[2] = { { pairs[i].first, "same-bytes" },
                                   { pairs[i].second, "same-bytes" } };
        size_t total_files = 0;
        snprintf(tag, sizeof(tag), "same_%u", (unsigned)i);
        stage_data_files(test_dir, tag, files, 2, roots, NK_OK, host_root, sizeof(host_root),
                         &total_files);
        assert(total_files == 2);  /* EBOOT.BIN plus one copy of the member */
        snprintf(probe, sizeof(probe), "%s%cDATA%c%s", host_root, sep, sep, pairs[i].kept_host_name);
        assert(host_file_holds(probe, "same-bytes"));
    }

    /* Control: names that share a prefix do not normalize alike and stage normally. */
    StageDataFile control[2] = { { "FOO;1", "one" }, { "FOOD;1", "two" } };
    size_t control_total = 0;
    stage_data_files(test_dir, "collide_control", control, 2, roots, NK_OK, host_root,
                     sizeof(host_root), &control_total);
    assert(control_total == 3);
    snprintf(probe, sizeof(probe), "%s%cDATA%cFOO", host_root, sep, sep);
    assert(host_file_holds(probe, "one"));
    snprintf(probe, sizeof(probe), "%s%cDATA%cFOOD", host_root, sep, sep);
    assert(host_file_holds(probe, "two"));

    /* A configured content root whose first component normalizes to the staged EBOOT.BIN would
     * create a directory where the executable is written, so it is refused before any directory
     * is created. */
    static const char *const eboot_roots[] = { "EBOOT.BIN", "eboot.bin ." };
    for (size_t i = 0; i < sizeof(eboot_roots) / sizeof(eboot_roots[0]); i++) {
        const char *root[] = { eboot_roots[i] };
        StageDataFile ok = { "OK.BIN;1", "ok" };
        snprintf(tag, sizeof(tag), "eboot_root_%u", (unsigned)i);
        stage_data_files(test_dir, tag, &ok, 1, root, NK_ERROR_INVALID_ISO, host_root,
                         sizeof(host_root), NULL);
        assert(!nk_platform_dir_exists(host_root));
    }
    printf("[HOSTILE_TEST] ISO staging keeps ';' names and resolves host name collisions by content.\n");
}

static void test_nk_json_utf8_encode(void) {
    printf("[HOSTILE_TEST] Testing nk_json_utf8_encode edge cases and boundaries...\n");
    fflush(stdout);

    char out[4];
    size_t len;

    /* 1. 1-byte ASCII range (0x00 .. 0x7F) with buffer sentinel verification */
    /* U+0000 NUL */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x00, out);
    assert(len == 1);
    assert(out[0] == 0x00);
    assert((uint8_t)out[1] == 0xCC && (uint8_t)out[2] == 0xCC && (uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+0041 'A' */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x41, out);
    assert(len == 1);
    assert(out[0] == 'A');
    assert((uint8_t)out[1] == 0xCC && (uint8_t)out[2] == 0xCC && (uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+007F DEL (max 1-byte) */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x7F, out);
    assert(len == 1);
    assert(out[0] == 0x7F);
    assert((uint8_t)out[1] == 0xCC && (uint8_t)out[2] == 0xCC && (uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* 2. 2-byte range (0x80 .. 0x7FF) with buffer sentinel verification */
    /* U+0080 (min 2-byte) */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x80, out);
    assert(len == 2);
    assert((uint8_t)out[0] == 0xC2 && (uint8_t)out[1] == 0x80);
    assert((uint8_t)out[2] == 0xCC && (uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+00A9 '©' */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x00A9, out);
    assert(len == 2);
    assert((uint8_t)out[0] == 0xC2 && (uint8_t)out[1] == 0xA9);
    assert((uint8_t)out[2] == 0xCC && (uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+03B1 'α' */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x03B1, out);
    assert(len == 2);
    assert((uint8_t)out[0] == 0xCE && (uint8_t)out[1] == 0xB1);
    assert((uint8_t)out[2] == 0xCC && (uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+07FF (max 2-byte) */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x07FF, out);
    assert(len == 2);
    assert((uint8_t)out[0] == 0xDF && (uint8_t)out[1] == 0xBF);
    assert((uint8_t)out[2] == 0xCC && (uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* 3. 3-byte range (0x800 .. 0xFFFF) with buffer sentinel verification */
    /* U+0800 (min 3-byte) */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x0800, out);
    assert(len == 3);
    assert((uint8_t)out[0] == 0xE0 && (uint8_t)out[1] == 0xA0 && (uint8_t)out[2] == 0x80);
    assert((uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+20AC '€' */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x20AC, out);
    assert(len == 3);
    assert((uint8_t)out[0] == 0xE2 && (uint8_t)out[1] == 0x82 && (uint8_t)out[2] == 0xAC);
    assert((uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+65E5 '日' */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x65E5, out);
    assert(len == 3);
    assert((uint8_t)out[0] == 0xE6 && (uint8_t)out[1] == 0x97 && (uint8_t)out[2] == 0xA5);
    assert((uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+D7FF (last scalar before surrogate area) */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0xD7FF, out);
    assert(len == 3);
    assert((uint8_t)out[0] == 0xED && (uint8_t)out[1] == 0x9F && (uint8_t)out[2] == 0xBF);
    assert((uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+E000 (min scalar after surrogate gap) */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0xE000, out);
    assert(len == 3);
    assert((uint8_t)out[0] == 0xEE && (uint8_t)out[1] == 0x80 && (uint8_t)out[2] == 0x80);
    assert((uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+FFFF (max 3-byte / BMP) */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0xFFFF, out);
    assert(len == 3);
    assert((uint8_t)out[0] == 0xEF && (uint8_t)out[1] == 0xBF && (uint8_t)out[2] == 0xBF);
    assert((uint8_t)out[3] == 0xCC);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* 4. 4-byte range (0x10000 .. 0x10FFFF) */
    /* U+10000 (min 4-byte) */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x10000, out);
    assert(len == 4);
    assert((uint8_t)out[0] == 0xF0 && (uint8_t)out[1] == 0x90 &&
           (uint8_t)out[2] == 0x80 && (uint8_t)out[3] == 0x80);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+1F600 '😀' */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x1F600, out);
    assert(len == 4);
    assert((uint8_t)out[0] == 0xF0 && (uint8_t)out[1] == 0x9F &&
           (uint8_t)out[2] == 0x98 && (uint8_t)out[3] == 0x80);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    /* U+10FFFF (max Unicode scalar value) */
    memset(out, 0xCC, sizeof(out));
    len = nk_json_utf8_encode(0x10FFFF, out);
    assert(len == 4);
    assert((uint8_t)out[0] == 0xF4 && (uint8_t)out[1] == 0x8F &&
           (uint8_t)out[2] == 0xBF && (uint8_t)out[3] == 0xBF);
    assert(nk_json_validate_utf8((const uint8_t *)out, len));

    printf("[HOSTILE_TEST] nk_json_utf8_encode tests PASSED!\n");
    fflush(stdout);
}

static void test_nk_json_scalar_extraction_and_queries(void) {
    printf("[HOSTILE_TEST] Testing nk_json get_* scalar extraction and query helpers...\n");
    fflush(stdout);

    /* 1. NULL node safety for all query and extraction functions */
    {
        bool b_out = true;
        double d_out = 1.0;
        int64_t i64_out = 1;
        uint32_t u32_out = 1;

        assert(nk_json_get_type(NULL) == NK_JSON_NULL);
        assert(nk_json_is_null(NULL) == false);
        assert(nk_json_is_bool(NULL) == false);
        assert(nk_json_is_number(NULL) == false);
        assert(nk_json_is_string(NULL) == false);
        assert(nk_json_is_array(NULL) == false);
        assert(nk_json_is_object(NULL) == false);

        assert(nk_json_get_string(NULL) == NULL);
        assert(nk_json_get_bool(NULL, &b_out) == false);
        assert(b_out == true); /* output unchanged */
        assert(nk_json_get_number(NULL, &d_out) == false);
        assert(d_out == 1.0);
        assert(nk_json_get_int64(NULL, &i64_out) == false);
        assert(i64_out == 1);
        assert(nk_json_get_uint32(NULL, &u32_out) == false);
        assert(u32_out == 1);

        assert(nk_json_array_count(NULL) == 0);
        assert(nk_json_array_get(NULL, 0) == NULL);
        assert(nk_json_obj_get(NULL, "key") == NULL);
        assert(nk_json_object_get(NULL, "key") == NULL);
    }

    /* 2. Parse a comprehensive JSON tree containing all types and numeric ranges */
    {
        const char json[] =
            "{\n"
            "  \"null_key\": null,\n"
            "  \"bool_true\": true,\n"
            "  \"bool_false\": false,\n"
            "  \"pos_int\": 42,\n"
            "  \"zero_int\": 0,\n"
            "  \"neg_int\": -100,\n"
            "  \"u32_max\": 4294967295,\n"
            "  \"u32_overflow\": 4294967296,\n"
            "  \"i64_val\": 9223372036854775807,\n"
            "  \"float_val\": 3.14159,\n"
            "  \"str_val\": \"nakagawa\",\n"
            "  \"arr_val\": [ 123, \"element\", false ],\n"
            "  \"obj_val\": { \"inner_bool\": true }\n"
            "}";

        char err[256] = "";
        NkJsonNode *root = nk_json_parse(json, strlen(json), err, sizeof(err));
        assert(root != NULL);
        assert(nk_json_is_object(root));

        /* --- NULL Node --- */
        const NkJsonNode *null_node = nk_json_obj_get(root, "null_key");
        assert(null_node != NULL);
        assert(nk_json_get_type(null_node) == NK_JSON_NULL);
        assert(nk_json_is_null(null_node) == true);
        assert(nk_json_is_bool(null_node) == false);
        assert(nk_json_is_number(null_node) == false);
        assert(nk_json_is_string(null_node) == false);
        assert(nk_json_is_array(null_node) == false);
        assert(nk_json_is_object(null_node) == false);
        assert(nk_json_get_string(null_node) == NULL);

        /* --- Boolean Nodes --- */
        const NkJsonNode *bt_node = nk_json_obj_get(root, "bool_true");
        assert(bt_node != NULL);
        assert(nk_json_get_type(bt_node) == NK_JSON_BOOL);
        assert(nk_json_is_bool(bt_node) == true);
        assert(nk_json_is_null(bt_node) == false);

        bool b_val = false;
        assert(nk_json_get_bool(bt_node, &b_val) == true);
        assert(b_val == true);
        /* Test NULL out_val pointer */
        assert(nk_json_get_bool(bt_node, NULL) == true);

        const NkJsonNode *bf_node = nk_json_obj_get(root, "bool_false");
        assert(bf_node != NULL);
        assert(nk_json_is_bool(bf_node) == true);
        assert(nk_json_get_bool(bf_node, &b_val) == true);
        assert(b_val == false);

        /* Boolean node mismatch extractions */
        double d_dummy = 0;
        int64_t i64_dummy = 0;
        uint32_t u32_dummy = 0;
        assert(nk_json_get_string(bt_node) == NULL);
        assert(nk_json_get_number(bt_node, &d_dummy) == false);
        assert(nk_json_get_int64(bt_node, &i64_dummy) == false);
        assert(nk_json_get_uint32(bt_node, &u32_dummy) == false);

        /* --- Positive Integer (42) --- */
        const NkJsonNode *pos_node = nk_json_obj_get(root, "pos_int");
        assert(pos_node != NULL);
        assert(nk_json_get_type(pos_node) == NK_JSON_NUMBER);
        assert(nk_json_is_number(pos_node) == true);

        double d_val = 0;
        assert(nk_json_get_number(pos_node, &d_val) == true);
        assert(d_val == 42.0);
        assert(nk_json_get_number(pos_node, NULL) == true);

        int64_t i64_val = 0;
        assert(nk_json_get_int64(pos_node, &i64_val) == true);
        assert(i64_val == 42);
        assert(nk_json_get_int64(pos_node, NULL) == true);

        uint32_t u32_val = 0;
        assert(nk_json_get_uint32(pos_node, &u32_val) == true);
        assert(u32_val == 42);
        assert(nk_json_get_uint32(pos_node, NULL) == true);

        /* Number node mismatch extractions */
        assert(nk_json_get_bool(pos_node, &b_val) == false);
        assert(nk_json_get_string(pos_node) == NULL);

        /* --- Zero Integer (0) --- */
        const NkJsonNode *zero_node = nk_json_obj_get(root, "zero_int");
        assert(zero_node != NULL);
        assert(nk_json_get_number(zero_node, &d_val) == true && d_val == 0.0);
        assert(nk_json_get_int64(zero_node, &i64_val) == true && i64_val == 0);
        assert(nk_json_get_uint32(zero_node, &u32_val) == true && u32_val == 0);

        /* --- Negative Integer (-100) --- */
        const NkJsonNode *neg_node = nk_json_obj_get(root, "neg_int");
        assert(neg_node != NULL);
        assert(nk_json_get_number(neg_node, &d_val) == true && d_val == -100.0);
        assert(nk_json_get_int64(neg_node, &i64_val) == true && i64_val == -100);
        /* Negative int must fail uint32 extraction */
        u32_val = 999;
        assert(nk_json_get_uint32(neg_node, &u32_val) == false);
        assert(u32_val == 999); /* unchanged */

        /* --- UINT32_MAX (4294967295) --- */
        const NkJsonNode *u32_max_node = nk_json_obj_get(root, "u32_max");
        assert(u32_max_node != NULL);
        assert(nk_json_get_uint32(u32_max_node, &u32_val) == true);
        assert(u32_val == 4294967295U);

        /* --- UINT32 Overflow (4294967296) --- */
        const NkJsonNode *u32_of_node = nk_json_obj_get(root, "u32_overflow");
        assert(u32_of_node != NULL);
        assert(nk_json_get_int64(u32_of_node, &i64_val) == true && i64_val == 4294967296LL);
        assert(nk_json_get_uint32(u32_of_node, &u32_val) == false);

        /* --- INT64 Value --- */
        const NkJsonNode *i64_node = nk_json_obj_get(root, "i64_val");
        assert(i64_node != NULL);
        assert(nk_json_get_int64(i64_node, &i64_val) == true && i64_val == 9223372036854775807LL);
        assert(nk_json_get_uint32(i64_node, &u32_val) == false);

        /* --- Floating Point (3.14159) --- */
        const NkJsonNode *float_node = nk_json_obj_get(root, "float_val");
        assert(float_node != NULL);
        assert(nk_json_get_number(float_node, &d_val) == true);
        assert(d_val > 3.1415 && d_val < 3.1417);
        /* Floating point number must fail int64 and uint32 extractions */
        assert(nk_json_get_int64(float_node, &i64_val) == false);
        assert(nk_json_get_uint32(float_node, &u32_val) == false);

        /* --- String Node --- */
        const NkJsonNode *str_node = nk_json_obj_get(root, "str_val");
        assert(str_node != NULL);
        assert(nk_json_get_type(str_node) == NK_JSON_STRING);
        assert(nk_json_is_string(str_node) == true);
        const char *s_val = nk_json_get_string(str_node);
        assert(s_val != NULL && strcmp(s_val, "nakagawa") == 0);

        /* String node mismatch extractions */
        assert(nk_json_get_bool(str_node, &b_val) == false);
        assert(nk_json_get_number(str_node, &d_val) == false);
        assert(nk_json_get_int64(str_node, &i64_val) == false);
        assert(nk_json_get_uint32(str_node, &u32_val) == false);

        /* --- Array Node --- */
        const NkJsonNode *arr_node = nk_json_obj_get(root, "arr_val");
        assert(arr_node != NULL);
        assert(nk_json_get_type(arr_node) == NK_JSON_ARRAY);
        assert(nk_json_is_array(arr_node) == true);
        assert(nk_json_array_count(arr_node) == 3);

        const NkJsonNode *a0 = nk_json_array_get(arr_node, 0);
        assert(a0 != NULL && nk_json_get_int64(a0, &i64_val) == true && i64_val == 123);

        const NkJsonNode *a1 = nk_json_array_get(arr_node, 1);
        assert(a1 != NULL && nk_json_is_string(a1) == true && strcmp(nk_json_get_string(a1), "element") == 0);

        const NkJsonNode *a2 = nk_json_array_get(arr_node, 2);
        assert(a2 != NULL && nk_json_get_bool(a2, &b_val) == true && b_val == false);

        /* Array out of bounds */
        assert(nk_json_array_get(arr_node, 3) == NULL);
        assert(nk_json_array_get(arr_node, 100) == NULL);

        /* Non-array array calls */
        assert(nk_json_array_count(str_node) == 0);
        assert(nk_json_array_get(str_node, 0) == NULL);

        /* --- Object Node & Alias --- */
        const NkJsonNode *obj_node = nk_json_obj_get(root, "obj_val");
        assert(obj_node != NULL);
        assert(nk_json_get_type(obj_node) == NK_JSON_OBJECT);
        assert(nk_json_is_object(obj_node) == true);

        const NkJsonNode *inner = nk_json_obj_get(obj_node, "inner_bool");
        assert(inner != NULL && nk_json_get_bool(inner, &b_val) == true && b_val == true);

        /* Test nk_json_object_get compatibility alias */
        const NkJsonNode *inner_alias = nk_json_object_get(obj_node, "inner_bool");
        assert(inner_alias == inner);

        /* Nonexistent key and NULL key */
        assert(nk_json_obj_get(obj_node, "nonexistent") == NULL);
        assert(nk_json_obj_get(obj_node, NULL) == NULL);
        assert(nk_json_object_get(obj_node, "nonexistent") == NULL);

        /* Object lookup on non-object node */
        assert(nk_json_obj_get(str_node, "key") == NULL);

        nk_json_free(root);
    }

    printf("[HOSTILE_TEST] nk_json scalar extraction and query tests PASSED!\n");
    fflush(stdout);
}


int main(void) {
    test_nk_json_utf8_encode();
    test_nk_json_scalar_extraction_and_queries();
    native_test_create_root("parsers-hostile");
    native_test_isolate_user_data_roots();
    native_test_assert_cache_root_isolated();
    native_test_assert_config_root_isolated();

    char test_dir[512];
    assert(nk_platform_get_path(NK_PATH_CACHE, test_dir, sizeof(test_dir)));
    char hostile_dir[600];
    snprintf(hostile_dir, sizeof(hostile_dir), "%s%chostile_tests", test_dir, nk_platform_path_separator());
    assert(nk_platform_mkdir_p(hostile_dir));

    test_nk_json_validate_utf8_edge_cases();
    test_hostile_library_json(hostile_dir);
    test_library_json_string_escapes(hostile_dir);
    test_hostile_iso_parser(hostile_dir);
    test_hostile_pbp_package(hostile_dir);
    test_iso_executable_classification(hostile_dir);
    test_iso_walk_module_tree(hostile_dir);
    test_iso_stage_refuses_device_names(hostile_dir);
    test_iso_stage_keeps_semicolon_names(hostile_dir);
    test_iso_stage_collisions_by_content(hostile_dir);
#if defined(_WIN32) || defined(_WIN64)
    test_invalid_utf8_path_is_refused(hostile_dir);
#endif

    printf("[HOSTILE_TEST] ALL HOSTILE/MALFORMED PARSER TESTS COMPLETED SUCCESSFULLY!\n");
    native_test_assert_real_config_untouched();
    return 0;
}
