/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_title_manifest.h"
#include "nk_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char * const CANONICAL_VALID_BASE =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"id\": \"size-boundary-test\",\n"
    "  \"display_name\": \"Size Boundary Test\",\n"
    "  \"kind\": \"synthetic\",\n"
    "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
    "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
    "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
    "  \"hle_profile\": \"standard\",\n"
    "  \"feature_requirements\": [\"allegrex\"],\n"
    "  \"verification_profile\": \"smoke\"\n"
    "}\n";

static void test_manifest_size_boundary(void) {
    printf("[MANIFEST_TEST] Testing 256 KB size boundary (LIMIT_CLASS = %s)...\n", NK_MANIFEST_LIMIT_CLASS);

    size_t base_len = strlen(CANONICAL_VALID_BASE);
    assert(base_len < NK_MANIFEST_MAX_BYTES);

    /* 1. Exactly NK_MANIFEST_MAX_BYTES (padded with whitespace) -> MUST ACCEPT */
    char *max_accepted = (char *)malloc(NK_MANIFEST_MAX_BYTES + 1);
    assert(max_accepted != NULL);
    memcpy(max_accepted, CANONICAL_VALID_BASE, base_len);
    memset(max_accepted + base_len, ' ', NK_MANIFEST_MAX_BYTES - base_len);
    max_accepted[NK_MANIFEST_MAX_BYTES] = '\0';

    char err_buf[512] = {0};
    NkTitleEntry entry;
    bool ok_max = nk_title_manifest_parse_buffer(max_accepted, NK_MANIFEST_MAX_BYTES, false, &entry, err_buf, sizeof(err_buf));
    if (!ok_max) {
        fprintf(stderr, "[MANIFEST_TEST] Max accepted failed: %s\n", err_buf);
    }
    assert(ok_max);
    assert(strcmp(entry.id, "size-boundary-test") == 0);
    free(max_accepted);

    /* 2. Exactly NK_MANIFEST_MAX_BYTES + 1 -> MUST REJECT WITH PRODUCT_SECURITY_POLICY */
    size_t over_len = NK_MANIFEST_MAX_BYTES + 1;
    char *over_limit = (char *)malloc(over_len + 1);
    assert(over_limit != NULL);
    memcpy(over_limit, CANONICAL_VALID_BASE, base_len);
    memset(over_limit + base_len, ' ', over_len - base_len);
    over_limit[over_len] = '\0';

    memset(err_buf, 0, sizeof(err_buf));
    bool ok_over = nk_title_manifest_parse_buffer(over_limit, over_len, false, &entry, err_buf, sizeof(err_buf));
    assert(!ok_over);
    assert(strstr(err_buf, NK_MANIFEST_LIMIT_CLASS) != NULL || strstr(err_buf, "limit") != NULL);
    free(over_limit);

    printf("[MANIFEST_TEST] Size boundary test PASSED! (Exact 256KB accepted, 256KB+1 rejected)\n");
}

static void test_manifest_unicode_and_surrogates(void) {
    printf("[MANIFEST_TEST] Testing Unicode escape decoding, UTF-16 surrogate pairs, and UTF-8 emission...\n");
    char err[512];
    NkTitleEntry entry;

    /* 1. Japanese escaped \u vs UTF-8 */
    const char *jp_escaped =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"unicode-jp-test\",\n"
        "  \"display_name\": \"\\u65e5\\u672c\\u8a9e\\u30bf\\u30a4\\u30c8\\u30eb\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    assert(nk_title_manifest_parse_buffer(jp_escaped, strlen(jp_escaped), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.display_name, "日本語タイトル") == 0);

    /* 2. UTF-16 surrogate pair for supplementary plane codepoint U+1F600 (GRINNING FACE 😀) */
    const char *surrogate_json =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"surrogate-test\",\n"
        "  \"display_name\": \"Emoji \\uD83D\\uDE00 Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    assert(nk_title_manifest_parse_buffer(surrogate_json, strlen(surrogate_json), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.display_name, "Emoji \xF0\x9F\x98\x80 Test") == 0);

    /* 3. Lone high surrogate -> MUST REJECT */
    const char *lone_high =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"lone-high-test\",\n"
        "  \"display_name\": \"Lone \\uD83D High\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(lone_high, strlen(lone_high), false, &entry, err, sizeof(err)));

    /* 4. Lone low surrogate -> MUST REJECT */
    const char *lone_low =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"lone-low-test\",\n"
        "  \"display_name\": \"Lone \\uDE00 Low\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(lone_low, strlen(lone_low), false, &entry, err, sizeof(err)));

    printf("[MANIFEST_TEST] Unicode and surrogate tests PASSED!\n");
}

static void test_manifest_numbers_and_addresses(void) {
    printf("[MANIFEST_TEST] Testing integer validation, float rejection, and address parsing...\n");
    char err[512];
    NkTitleEntry entry;

    /* 1. Fractional number (1.5) for integer field -> MUST REJECT */
    const char *float_entry =
        "{\n"
        "  \"schema_version\": 1.5,\n"
        "  \"id\": \"float-test\",\n"
        "  \"display_name\": \"Float Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(float_entry, strlen(float_entry), false, &entry, err, sizeof(err)));

    /* 2. Number with exponent (1e5) for integer field -> MUST REJECT */
    const char *exp_entry =
        "{\n"
        "  \"schema_version\": 1e0,\n"
        "  \"id\": \"exp-test\",\n"
        "  \"display_name\": \"Exp Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(exp_entry, strlen(exp_entry), false, &entry, err, sizeof(err)));

    /* 3. Valid exact 8-hex address string -> MUST ACCEPT */
    const char *hex_addr =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"hex-addr-test\",\n"
        "  \"display_name\": \"Hex Address Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": \"0x08800000\", \"entry\": \"0x08804000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(nk_title_manifest_parse_buffer(hex_addr, strlen(hex_addr), false, &entry, err, sizeof(err)));
    assert(entry.executable_base == 0x08800000U);
    assert(entry.executable_entry == 0x08804000U);

    /* 4. Overlong address string ("0x088000000" -> 9 hex digits) -> MUST REJECT */
    const char *overlong_hex =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"bad-hex-test\",\n"
        "  \"display_name\": \"Bad Hex Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": \"0x088000000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(overlong_hex, strlen(overlong_hex), false, &entry, err, sizeof(err)));

    /* 5. Short address string ("0x1") -> MUST REJECT */
    const char *short_hex =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"bad-hex-test\",\n"
        "  \"display_name\": \"Bad Hex Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": \"0x1\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(short_hex, strlen(short_hex), false, &entry, err, sizeof(err)));

    printf("[MANIFEST_TEST] Numbers and addresses tests PASSED!\n");
}

static void test_manifest_identifiers_and_disc_ids(void) {
    printf("[MANIFEST_TEST] Testing canonical identifier regex and disc ID validation...\n");
    char err[512];
    NkTitleEntry entry;

    /* 1. Identifier with uppercase -> MUST REJECT (canonical is strictly lowercase) */
    const char *upper_id =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"UPPERCASE-id\",\n"
        "  \"display_name\": \"Upper ID Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(upper_id, strlen(upper_id), false, &entry, err, sizeof(err)));

    /* 2. Identifier starting with symbol -> MUST REJECT */
    const char *sym_id =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"_bad_start\",\n"
        "  \"display_name\": \"Sym ID Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(sym_id, strlen(sym_id), false, &entry, err, sizeof(err)));

    /* 3. Valid lowercase identifier with digits, '.', '_', '-' -> MUST ACCEPT */
    const char *valid_id =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"title.v1_alpha-2\",\n"
        "  \"display_name\": \"Valid ID Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(nk_title_manifest_parse_buffer(valid_id, strlen(valid_id), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.id, "title.v1_alpha-2") == 0);

    /* 4. Retail disc ID with invalid format (lowercase) -> MUST REJECT */
    const char *bad_disc =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"retail-bad-disc\",\n"
        "  \"display_name\": \"Retail Bad Disc\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"ucus98701\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(bad_disc, strlen(bad_disc), false, &entry, err, sizeof(err)));

    /* 5. Retail disc ID with compatible_revisions containing primary disc ID -> MUST REJECT */
    const char *compat_dup_primary =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"retail-compat-dup\",\n"
        "  \"display_name\": \"Retail Compat Dup Primary\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCUS98701\", \"region\": \"NA\", \"revision_policy\": \"explicit-compatible-revisions\", \"compatible_revisions\": [\"UCUS98701\"]},\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(compat_dup_primary, strlen(compat_dup_primary), false, &entry, err, sizeof(err)));

    printf("[MANIFEST_TEST] Identifiers and disc ID tests PASSED!\n");
}

static void test_manifest_mutations(void) {
    printf("[MANIFEST_TEST] Testing schema mutations and hostile edge cases...\n");
    char err[512];
    NkTitleEntry entry;

    /* 1. Missing required field (display_name missing) */
    const char *no_display = "{\"schema_version\": 1, \"id\": \"test\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(no_display, strlen(no_display), false, &entry, err, sizeof(err)));
    assert(strstr(err, "missing required field") != NULL);

    /* 2. Unknown field */
    const char *unknown_field = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"unknown_key\": 123, \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(unknown_field, strlen(unknown_field), false, &entry, err, sizeof(err)));
    assert(strstr(err, "unknown field") != NULL);

    /* 3. Duplicate JSON object key */
    const char *dup_key = "{\"schema_version\": 1, \"id\": \"test\", \"id\": \"test2\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(dup_key, strlen(dup_key), false, &entry, err, sizeof(err)));
    assert(strstr(err, "duplicate JSON object key") != NULL);

    /* 4. Wrong type for schema_version */
    const char *wrong_type = "{\"schema_version\": \"1\", \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(wrong_type, strlen(wrong_type), false, &entry, err, sizeof(err)));

    /* 5. Unsupported schema_version */
    const char *bad_sv = "{\"schema_version\": 2, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(bad_sv, strlen(bad_sv), false, &entry, err, sizeof(err)));

    /* 6. Negative/overflow integer */
    const char *neg_entry = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": -5, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(neg_entry, strlen(neg_entry), false, &entry, err, sizeof(err)));

    /* 7. Malformed hex */
    const char *bad_hex = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": \"0xZZZZ\", \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(bad_hex, strlen(bad_hex), false, &entry, err, sizeof(err)));

    /* 8. Duplicate module binding */
    const char *dup_mod = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [{\"name\": \"a.prx\", \"load_address\": 1, \"required\": true, \"role\": \"guest-prx\"}, {\"name\": \"A.PRX\", \"load_address\": 2, \"required\": true, \"role\": \"guest-prx\"}], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(dup_mod, strlen(dup_mod), false, &entry, err, sizeof(err)));
    assert(strstr(err, "duplicate module name") != NULL);

    /* 9. Duplicate feature requirement */
    const char *dup_feat = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [\"allegrex\", \"allegrex\"], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(dup_feat, strlen(dup_feat), false, &entry, err, sizeof(err)));
    assert(strstr(err, "duplicate feature requirement") != NULL);

    /* 10. Trailing garbage */
    const char *trailing = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"} GARBAGE";
    assert(!nk_title_manifest_parse_buffer(trailing, strlen(trailing), false, &entry, err, sizeof(err)));
    assert(strstr(err, "Trailing garbage") != NULL);

    /* 11. Invalid UTF-8 */
    const char invalid_utf8[] = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"\xFF\xFE\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(invalid_utf8, sizeof(invalid_utf8) - 1, false, &entry, err, sizeof(err)));
    assert(strstr(err, "UTF-8") != NULL);

    /* 12. Excessively nested JSON (> 16 levels) */
    const char *deep_nest = "{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":1}}}}}}}}}}}}}}}}}}";
    assert(!nk_title_manifest_parse_buffer(deep_nest, strlen(deep_nest), false, &entry, err, sizeof(err)));
    assert(strstr(err, "nesting exceeds") != NULL);

    /* 13. Reserved VFPU target range rejection */
    const char *vfpu_target = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\", \"runtime_bindings\": {\"schema_version\": 1, \"dispatch_aliases\": [{\"from\": \"0x40001000\", \"to\": \"0x08801000\"}]}}";
    assert(!nk_title_manifest_parse_buffer(vfpu_target, strlen(vfpu_target), false, &entry, err, sizeof(err)));
    assert(strstr(err, "core VFPU dispatch-target range") != NULL);

    /* 14. Non-retail kind with disc object -> REJECT */
    const char *synth_with_disc = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"disc\": {\"id\": \"UCUS98701\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"}, \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(synth_with_disc, strlen(synth_with_disc), false, &entry, err, sizeof(err)));
    assert(strstr(err, "disc object is forbidden") != NULL);

    /* 15. Retail kind without disc object -> REJECT */
    const char *retail_no_disc = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"retail\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(retail_no_disc, strlen(retail_no_disc), false, &entry, err, sizeof(err)));
    assert(strstr(err, "retail kind requires disc object") != NULL);

    /* 16. Trailing comma in an object -> REJECT
     *
     * Python's json module rejects {"a":1,}. The native parser consumed the
     * comma, returned to the top of its loop and accepted the closing brace,
     * so a manifest could exist that only the native overlay loader would
     * take -- a differential-parser divergence, which is exactly what the
     * two implementations are meant not to have. */
    const char *obj_trailing_comma = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\",}";
    assert(!nk_title_manifest_parse_buffer(obj_trailing_comma, strlen(obj_trailing_comma), false, &entry, err, sizeof(err)));
    assert(strstr(err, "Trailing comma") != NULL);

    /* 17. Trailing comma in an array -> REJECT (same state transition) */
    const char *arr_trailing_comma = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\",]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(arr_trailing_comma, strlen(arr_trailing_comma), false, &entry, err, sizeof(err)));
    assert(strstr(err, "Trailing comma") != NULL);

    printf("[MANIFEST_TEST] Schema mutation tests PASSED!\n");
}

static void test_executable_entry_vs_fallback(void) {
    printf("[MANIFEST_TEST] Testing executable.entry vs runtime_bindings.fallback_entry projection contract...\n");
    char err[512];
    NkTitleEntry entry;

    /* Fixture where executable.entry != runtime_bindings.fallback_entry */
    const char *fixture =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"entry-diff-test\",\n"
        "  \"display_name\": \"Entry Diff Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": \"0x08800000\", \"entry\": \"0x08800000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\",\n"
        "  \"runtime_bindings\": {\"schema_version\": 1, \"fallback_entry\": \"0x08801000\"}\n"
        "}\n";

    assert(nk_title_manifest_parse_buffer(fixture, strlen(fixture), false, &entry, err, sizeof(err)));
    /* Contract: NkTitleEntry.executable_entry must project executable.entry (0x08800000), NOT fallback_entry (0x08801000) */
    assert(entry.executable_entry == 0x08800000U);
    assert(entry.executable_base == 0x08800000U);

    printf("[MANIFEST_TEST] Executable entry contract test PASSED! (executable.entry projected cleanly)\n");
}

static void test_multi_overlay_storage(void) {
    printf("[MANIFEST_TEST] Testing simultaneous multiple overlay storage and memory durability...\n");
    char err[512];
    NkTitleEntry ov1, ov2;

    const char *manifest_alpha =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"overlay-alpha-test\",\n"
        "  \"display_name\": \"Overlay Alpha Title\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCUS99901\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": \"0x08800000\", \"entry\": \"0x08804000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"alpha.prx\", \"load_address\": \"0x08900000\", \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data/alpha\", \"memory_stick_root\": \"ms/alpha\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    const char *manifest_beta =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"overlay-beta-test\",\n"
        "  \"display_name\": \"Overlay Beta Title\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCES99902\", \"region\": \"EU\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": \"0x08a00000\", \"entry\": \"0x08a04000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"beta.prx\", \"load_address\": \"0x08b00000\", \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data/beta\", \"memory_stick_root\": \"ms/beta\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    assert(nk_title_manifest_parse_buffer(manifest_alpha, strlen(manifest_alpha), false, &ov1, err, sizeof(err)));
    assert(nk_title_manifest_parse_buffer(manifest_beta, strlen(manifest_beta), false, &ov2, err, sizeof(err)));

    /* Verify both entries coexist without data mutation */
    assert(strcmp(ov1.id, "overlay-alpha-test") == 0);
    assert(strcmp(ov1.primary_disc_id, "UCUS99901") == 0);
    assert(ov1.executable_entry == 0x08804000U);
    assert(strcmp(ov1.modules[0].name, "alpha.prx") == 0);

    assert(strcmp(ov2.id, "overlay-beta-test") == 0);
    assert(strcmp(ov2.primary_disc_id, "UCES99902") == 0);
    assert(ov2.executable_entry == 0x08a04000U);
    assert(strcmp(ov2.modules[0].name, "beta.prx") == 0);

    printf("[MANIFEST_TEST] Simultaneous multi-overlay storage test PASSED!\n");
}

static void test_overlay_clear_releases_storage(void) {
    printf("[MANIFEST_TEST] Testing that clearing the overlay releases parser storage...\n");
    char err[512];
    NkTitleEntry entry;

    /* Two distinct overlay identities that claim the same disc. */
    const char *overlay_a =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"overlay-clear-a\",\n"
        "  \"display_name\": \"Overlay Clear A\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCUS99911\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": \"0x08804000\", \"entry\": \"0x08808000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"a.prx\", \"load_address\": \"0x08900000\", \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data/a\", \"memory_stick_root\": \"ms/a\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    const char *overlay_b =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"overlay-clear-b\",\n"
        "  \"display_name\": \"Overlay Clear B\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCUS99911\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": \"0x08804000\", \"entry\": \"0x08808000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"b.prx\", \"load_address\": \"0x08900000\", \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data/b\", \"memory_stick_root\": \"ms/b\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    /* Start from a known-empty overlay state regardless of test order. */
    nk_title_catalog_clear_overlay();

    assert(nk_title_manifest_parse_buffer(overlay_a, strlen(overlay_a), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.id, "overlay-clear-a") == 0);

    /* While A is loaded, B claims the same disc and must be refused. This is
     * the check that was still firing after a clear. */
    assert(!nk_title_manifest_parse_buffer(overlay_b, strlen(overlay_b), false, &entry, err, sizeof(err)));
    assert(strstr(err, "overlay-clear-a") != NULL);

    /* Clearing the registry must release the parser's storage too. Before the
     * fix it only reset the catalog's pointer array, so B stayed blocked by an
     * overlay the caller had already cleared and the fixed slot capacity
     * remained consumed. */
    nk_title_catalog_clear_overlay();

    assert(nk_title_manifest_parse_buffer(overlay_b, strlen(overlay_b), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.id, "overlay-clear-b") == 0);

    nk_title_catalog_clear_overlay();
    printf("[MANIFEST_TEST] Overlay clear/storage release test PASSED!\n");
}

static void test_overlay_collision_policy(void) {
    printf("[MANIFEST_TEST] Testing overlay collision policy across all identities...\n");
    char err[512];
    NkTitleEntry entry;

    /* Conflicting private overlay matching public canonical title "synthetic-allegrex-v1" */
    const char *collision_manifest =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"synthetic-allegrex-v1\",\n"
        "  \"display_name\": \"Malicious Public Hijack\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"hijack.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    /* Default (allow_override = false) => MUST REJECT */
    bool ok_default = nk_title_manifest_parse_buffer(
        collision_manifest, strlen(collision_manifest), false, &entry, err, sizeof(err));
    assert(!ok_default);
    assert(strstr(err, "conflicts with public canonical catalog") != NULL);

    /* Override enabled (allow_override = true) => MUST ACCEPT with noisy warning */
    bool ok_override = nk_title_manifest_parse_buffer(
        collision_manifest, strlen(collision_manifest), true, &entry, err, sizeof(err));
    assert(ok_override);
    assert(strcmp(entry.id, "synthetic-allegrex-v1") == 0);

    printf("[MANIFEST_TEST] Overlay collision policy tests PASSED!\n");
}

int main(int argc, char *argv[]) {
    if (argc >= 3 && strcmp(argv[1], "--check") == 0) {
        const char *manifest_path = argv[2];
        char err_buf[512] = {0};
        NkTitleEntry entry;
        FILE *f = fopen(manifest_path, "rb");
        if (!f) {
            printf("REJECT: Could not open file: %s\n", manifest_path);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        if (!buf) {
            printf("REJECT: Out of memory\n");
            fclose(f);
            return 1;
        }
        size_t rd = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        buf[rd] = '\0';

        bool allow_override = (argc >= 4 && strcmp(argv[3], "--allow-override") == 0);
        bool ok = nk_title_manifest_parse_buffer(buf, rd, allow_override, &entry, err_buf, sizeof(err_buf));
        free(buf);

        if (!ok) {
            printf("REJECT: %s\n", err_buf);
            return 1;
        } else {
            printf("ACCEPT: id=%s entry=0x%08x base=0x%08x modules=%d disc_id=%s\n",
                   entry.id, entry.executable_entry, entry.executable_base,
                   entry.module_count, entry.primary_disc_id ? entry.primary_disc_id : "none");
            return 0;
        }
    }

    printf("[MANIFEST_TEST] Starting native manifest parser test suite...\n");
    test_manifest_size_boundary();
    test_manifest_unicode_and_surrogates();
    test_manifest_numbers_and_addresses();
    test_manifest_identifiers_and_disc_ids();
    test_manifest_mutations();
    test_executable_entry_vs_fallback();
    test_multi_overlay_storage();
    test_overlay_collision_policy();
    test_overlay_clear_releases_storage();
    printf("[MANIFEST_TEST] ALL NATIVE MANIFEST PARSER TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
