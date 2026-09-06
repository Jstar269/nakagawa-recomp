/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_title_manifest.h"
#include "nk_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_manifest_size_boundary(void) {
    printf("[MANIFEST_TEST] Testing 256 KB size boundary (LIMIT_CLASS = %s)...\n", NK_MANIFEST_LIMIT_CLASS);

    const char *base_valid =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"size-boundary-test\",\n"
        "  \"display_name\": \"Size Boundary Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": \"0x08800000\"},\n"
        "  \"modules\": [{\"name\": \"main.prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\"},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    size_t base_len = strlen(base_valid);
    assert(base_len < NK_MANIFEST_MAX_BYTES);

    /* 1. Exactly NK_MANIFEST_MAX_BYTES (padded with whitespace) -> MUST ACCEPT */
    char *max_accepted = (char *)malloc(NK_MANIFEST_MAX_BYTES + 1);
    assert(max_accepted != NULL);
    memcpy(max_accepted, base_valid, base_len);
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
    memcpy(over_limit, base_valid, base_len);
    memset(over_limit + base_len, ' ', over_len - base_len);
    over_limit[over_len] = '\0';

    memset(err_buf, 0, sizeof(err_buf));
    bool ok_over = nk_title_manifest_parse_buffer(over_limit, over_len, false, &entry, err_buf, sizeof(err_buf));
    assert(!ok_over);
    assert(strstr(err_buf, NK_MANIFEST_LIMIT_CLASS) != NULL || strstr(err_buf, "limit") != NULL);
    free(over_limit);

    printf("[MANIFEST_TEST] Size boundary test PASSED! (Exact 256KB accepted, 256KB+1 rejected)\n");
}

static void test_manifest_mutations(void) {
    printf("[MANIFEST_TEST] Testing schema mutations and hostile edge cases...\n");
    char err[512];
    NkTitleEntry entry;

    /* 1. Missing required field (display_name missing) */
    const char *no_display = "{\"schema_version\": 1, \"id\": \"test\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(no_display, strlen(no_display), false, &entry, err, sizeof(err)));
    assert(strstr(err, "missing required field") != NULL);

    /* 2. Unknown field */
    const char *unknown_field = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"unknown_key\": 123, \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(unknown_field, strlen(unknown_field), false, &entry, err, sizeof(err)));
    assert(strstr(err, "unknown field") != NULL);

    /* 3. Duplicate JSON object key */
    const char *dup_key = "{\"schema_version\": 1, \"id\": \"test\", \"id\": \"test2\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(dup_key, strlen(dup_key), false, &entry, err, sizeof(err)));
    assert(strstr(err, "duplicate JSON object key") != NULL);

    /* 4. Wrong type for schema_version */
    const char *wrong_type = "{\"schema_version\": \"1\", \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(wrong_type, strlen(wrong_type), false, &entry, err, sizeof(err)));

    /* 5. Unsupported schema_version */
    const char *bad_sv = "{\"schema_version\": 2, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(bad_sv, strlen(bad_sv), false, &entry, err, sizeof(err)));

    /* 6. Negative/overflow integer */
    const char *neg_entry = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": -5}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(neg_entry, strlen(neg_entry), false, &entry, err, sizeof(err)));

    /* 7. Malformed hex */
    const char *bad_hex = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": \"0xZZZZ\"}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(bad_hex, strlen(bad_hex), false, &entry, err, sizeof(err)));

    /* 8. Duplicate module binding */
    const char *dup_mod = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [{\"name\": \"a.prx\"}, {\"name\": \"a.prx\"}], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(dup_mod, strlen(dup_mod), false, &entry, err, sizeof(err)));
    assert(strstr(err, "duplicate module binding") != NULL);

    /* 9. Duplicate feature requirement */
    const char *dup_feat = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [\"allegrex\", \"allegrex\"], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(dup_feat, strlen(dup_feat), false, &entry, err, sizeof(err)));
    assert(strstr(err, "duplicate feature requirement") != NULL);

    /* 10. Trailing garbage */
    const char *trailing = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"} GARBAGE";
    assert(!nk_title_manifest_parse_buffer(trailing, strlen(trailing), false, &entry, err, sizeof(err)));
    assert(strstr(err, "Trailing garbage") != NULL);

    /* 11. Invalid UTF-8 */
    const char invalid_utf8[] = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"\xFF\xFE\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(invalid_utf8, sizeof(invalid_utf8) - 1, false, &entry, err, sizeof(err)));
    assert(strstr(err, "UTF-8") != NULL);

    /* 12. Excessively nested JSON (> 16 levels) */
    const char *deep_nest = "{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":1}}}}}}}}}}}}}}}}}}";
    assert(!nk_title_manifest_parse_buffer(deep_nest, strlen(deep_nest), false, &entry, err, sizeof(err)));
    assert(strstr(err, "nesting exceeds") != NULL);

    /* 13. Reserved VFPU target range rejection */
    const char *vfpu_target = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\", \"runtime_bindings\": {\"fallback_entry\": \"0x40001000\"}}";
    assert(!nk_title_manifest_parse_buffer(vfpu_target, strlen(vfpu_target), false, &entry, err, sizeof(err)));
    assert(strstr(err, "core VFPU dispatch-target range") != NULL);

    /* 14. Non-retail kind with disc object -> REJECT */
    const char *synth_with_disc = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"disc\": {\"id\": \"UCUS98701\"}, \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(synth_with_disc, strlen(synth_with_disc), false, &entry, err, sizeof(err)));
    assert(strstr(err, "disc object is forbidden") != NULL);

    /* 15. Retail kind without disc object -> REJECT */
    const char *retail_no_disc = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"retail\", \"executable\": {\"base\": 0, \"entry\": 0}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\"}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(retail_no_disc, strlen(retail_no_disc), false, &entry, err, sizeof(err)));
    assert(strstr(err, "retail kind requires disc object") != NULL);

    printf("[MANIFEST_TEST] Schema mutation tests PASSED!\n");
}

static void test_overlay_collision_policy(void) {
    printf("[MANIFEST_TEST] Testing overlay collision policy (Section 4)...\n");
    char err[512];
    NkTitleEntry entry;

    /* Conflicting private overlay matching public canonical title "synthetic-allegrex-v1" */
    const char *collision_manifest =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"synthetic-allegrex-v1\",\n"
        "  \"display_name\": \"Malicious Public Hijack\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": \"0x08800000\"},\n"
        "  \"modules\": [{\"name\": \"hijack.prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\"},\n"
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
        char *buf = (char *)malloc(sz + 1);
        fread(buf, 1, sz, f);
        fclose(f);
        buf[sz] = '\0';

        bool allow_override = (argc >= 4 && strcmp(argv[3], "--allow-override") == 0);
        bool ok = nk_title_manifest_parse_buffer(buf, (size_t)sz, allow_override, &entry, err_buf, sizeof(err_buf));
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
    test_manifest_mutations();
    test_overlay_collision_policy();
    printf("[MANIFEST_TEST] ALL NATIVE MANIFEST PARSER TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
