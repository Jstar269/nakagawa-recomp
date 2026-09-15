// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Host-neutral regression for the dynamic extracted-asset index (issue #223). */

#include "asset_index.h"

#include <stdio.h>

typedef struct {
    const char *relative;
    int is_dir;
    size_t size;
} SyntheticNode;

static int fail(const char *what) {
    fprintf(stderr, "asset index selftest: %s\n", what);
    return 1;
}

static char *make_host_path(const char *root, const char *leaf, size_t repeats) {
    size_t root_len = strlen(root), leaf_len = strlen(leaf);
    if (root_len > SIZE_MAX - leaf_len - 2u) return NULL;
    size_t n = root_len + leaf_len + 2u;
    if (repeats > (SIZE_MAX - n) / 2u) return NULL;
    n += repeats * 2u;
    char *path = (char *)malloc(n + 1u);
    if (!path) return NULL;
    size_t at = 0;
    memcpy(path + at, root, root_len); at += root_len;
    for (size_t i = 0; i < repeats; i++) {
        path[at++] = '/';
        path[at++] = 'x';
    }
    path[at++] = '/';
    memcpy(path + at, leaf, leaf_len); at += leaf_len;
    path[at] = '\0';
    return path;
}

static int add_synthetic_tree(SrAssetIndex *index, const char *root,
                              size_t repeats, size_t fail_at,
                              size_t *directories_out) {
    static const SyntheticNode tree[] = {
        {"locale", 1, 0},
        {"locale/common.xb.d", 1, 0},
        {"locale/common.xb.d/data/menu/text/common.to", 0, 17},
        {"locale/common.xb2.d", 1, 0},
        {"locale/common.xb2.d/data/menu/text/common.to", 0, 23},
        {"locale/COMMON.XB2.D", 1, 0},
        {"locale/COMMON.XB2.D/data/menu/text/UPPER.TO", 0, 29},
        {"locale/foo.XB", 1, 0},
        {"locale/foo.XB/other.XB10.D", 1, 0},
        {"locale/foo.XB/other.XB10.D/data/menu/text/NESTED.TO", 0, 31},
    };
    size_t directories = 0;
    for (size_t i = 0; i < sizeof(tree) / sizeof(tree[0]); i++) {
        if (i == fail_at) return 0;
        const SyntheticNode *node = &tree[i];
        if (node->is_dir) { directories++; continue; }
        char *key = NULL;
        char *host = make_host_path(root, node->relative, repeats);
        int variant = -1;
        if (!host || !sr_asset_index_key_from_rel(node->relative, &key, &variant) ||
            !sr_asset_index_add_sized(index, key, host, variant, (uint64_t)node->size)) {
            free(key);
            free(host);
            return 0;
        }
        free(key);
        free(host);
    }
    if (directories_out) *directories_out = directories;
    return 1;
}

static int build_synthetic_index(SrAssetIndex *published, const char *root,
                                 size_t repeats, size_t fail_at,
                                 size_t *directories_out) {
    SrAssetIndex temporary;
    sr_asset_index_init(&temporary);
    if (!add_synthetic_tree(&temporary, root, repeats, fail_at, directories_out) ||
        !sr_asset_index_finalize(&temporary) ||
        !sr_asset_index_publish(published, &temporary)) {
        sr_asset_index_destroy(&temporary);
        return 0;
    }
    return 1;
}

static const SrAssetIndexEntry *find_variant(const SrAssetIndex *index,
                                             const char *key, int wanted) {
    size_t first = sr_asset_index_lower_bound(index, key);
    for (size_t i = first; i < index->count; i++) {
        const SrAssetIndexEntry *entry = &index->entries[i];
        if (strcmp(entry->key, key) != 0) break;
        if (entry->variant == wanted) return entry;
    }
    return NULL;
}

int main(void) {
    SrAssetIndex short_index, long_index;
    sr_asset_index_init(&short_index);
    sr_asset_index_init(&long_index);
    size_t short_dirs = 0, long_dirs = 0;

    if (!build_synthetic_index(&short_index, "short-root", 1u, SIZE_MAX, &short_dirs) ||
        !build_synthetic_index(&long_index, "long-root", 900u, SIZE_MAX, &long_dirs))
        return fail("synthetic tree construction failed");
    if (short_dirs != 6u || long_dirs != short_dirs ||
        short_index.count != 4u || long_index.count != short_index.count)
        return fail("synthetic tree shape changed");

    if (sr_asset_index_finalize(&short_index) == 0 ||
        sr_asset_index_finalize(&long_index) == 0)
        return fail("non-empty index did not finalize");
    if (sr_asset_index_finalize(NULL) != 0) return fail("NULL index finalized");

    const char *key = "data/menu/text/common.to";
    const SrAssetIndexEntry *short_plain = find_variant(&short_index, key, -1);
    const SrAssetIndexEntry *long_plain = find_variant(&long_index, key, -1);
    const SrAssetIndexEntry *short_v2 = find_variant(&short_index, key, 2);
    const SrAssetIndexEntry *long_v2 = find_variant(&long_index, key, 2);
    const SrAssetIndexEntry *short_upper = find_variant(
        &short_index, "data/menu/text/upper.to", 2);
    const SrAssetIndexEntry *long_upper = find_variant(
        &long_index, "data/menu/text/upper.to", 2);
    const SrAssetIndexEntry *short_nested = find_variant(
        &short_index, "data/menu/text/nested.to", 10);
    const SrAssetIndexEntry *long_nested = find_variant(
        &long_index, "data/menu/text/nested.to", 10);
    if (!short_plain || !long_plain || !short_v2 || !long_v2 ||
        !short_upper || !long_upper || !short_nested || !long_nested)
        return fail("synthetic lookup missed a variant");
    if (strcmp(short_plain->key, long_plain->key) != 0 ||
        strcmp(short_v2->key, long_v2->key) != 0 ||
        short_plain->variant != long_plain->variant ||
        short_v2->variant != long_v2->variant ||
        short_plain->size != long_plain->size || short_v2->size != long_v2->size)
        return fail("short and long lookup results differ");
    if (short_upper->size != 29u || long_upper->size != short_upper->size)
        return fail("uppercase archive marker did not normalize");
    if (short_nested->size != 31u || long_nested->size != short_nested->size)
        return fail("nested archive marker did not select the variant");
    if (strlen(long_plain->host) <= 512u || strlen(long_plain->host) <= strlen(short_plain->host))
        return fail("long host path was truncated");
    if (find_variant(&short_index, "missing", -1))
        return fail("missing key unexpectedly matched");
    static const unsigned char invalid_utf8[][4] = {
        {0xc3u, 0x28u, 0u, 0u},
        {0xe0u, 0u, 0u, 0u},
        {0xe0u, 0xa0u, 0u, 0u},
        {0xf0u, 0u, 0u, 0u},
        {0xf0u, 0x90u, 0u, 0u},
        {0xf0u, 0x90u, 0x80u, 0u},
    };
    for (size_t i = 0; i < sizeof(invalid_utf8) / sizeof(invalid_utf8[0]); i++) {
        if (sr_asset_index_valid_utf8((const char *)invalid_utf8[i]))
            return fail("truncated or malformed UTF-8 was accepted");
    }
    size_t before = short_index.count;
    if (sr_asset_index_add(&short_index, NULL, "ignored", -1) != 0 ||
        short_index.count != before)
        return fail("invalid insertion was accepted");

    SrAssetIndex failed_publish;
    sr_asset_index_init(&failed_publish);
    if (build_synthetic_index(&failed_publish, "failed-root", 900u, 3u, NULL) ||
        failed_publish.count != 0u)
        return fail("enumeration failure published a partial index");
    sr_asset_index_destroy(&failed_publish);

    SrAssetIndex overflow;
    sr_asset_index_init(&overflow);
    if (sr_asset_index_reserve(&overflow, SIZE_MAX) != 0 || overflow.count != 0u)
        return fail("allocation-overflow seam did not fail closed");
    sr_asset_index_destroy(&overflow);

    SrAssetIndex unfinalized;
    sr_asset_index_init(&unfinalized);
    if (!sr_asset_index_add(&unfinalized, "unsorted", "host", -1) ||
        sr_asset_index_publish(&failed_publish, &unfinalized) != 0 ||
        sr_asset_index_publish(&unfinalized, &unfinalized) != 0) {
        sr_asset_index_destroy(&unfinalized);
        return fail("unfinalized or aliased publication was accepted");
    }
    sr_asset_index_destroy(&unfinalized);

    SrAssetIndex empty;
    sr_asset_index_init(&empty);
    /* VFS Directory Enumeration Tests */
    SrVfsDirEntry *dir_entries = NULL;
    size_t dir_count = 0;

    /* Test 1: Direct child enumeration of data/menu/text */
    if (sr_asset_index_list_dir(&short_index, "data/menu/text", -1, &dir_entries, &dir_count) != 1 ||
        dir_count != 3) {
        free(dir_entries);
        return fail("VFS list_dir data/menu/text failed count");
    }
    if (strcmp(dir_entries[0].name, "common.to") != 0 || dir_entries[0].is_dir != 0 || dir_entries[0].size != 17 ||
        strcmp(dir_entries[1].name, "nested.to") != 0 || dir_entries[1].is_dir != 0 || dir_entries[1].size != 31 ||
        strcmp(dir_entries[2].name, "upper.to") != 0 || dir_entries[2].is_dir != 0 || dir_entries[2].size != 29) {
        free(dir_entries);
        return fail("VFS list_dir data/menu/text entries mismatch or duplicate not collapsed");
    }
    free(dir_entries); dir_entries = NULL;

    /* Test 2: Trailing slash invariance "data/menu/text/" */
    if (sr_asset_index_list_dir(&short_index, "data/menu/text/", -1, &dir_entries, &dir_count) != 1 ||
        dir_count != 3) {
        free(dir_entries);
        return fail("VFS list_dir trailing slash mismatch");
    }
    free(dir_entries); dir_entries = NULL;

    /* Test 3: Subdirectory level "data/menu" (no recursive grandchildren) */
    if (sr_asset_index_list_dir(&short_index, "data/menu", -1, &dir_entries, &dir_count) != 1 ||
        dir_count != 1 || strcmp(dir_entries[0].name, "text") != 0 || dir_entries[0].is_dir != 1) {
        free(dir_entries);
        return fail("VFS list_dir data/menu grandchildren not collapsed into direct child dir");
    }
    free(dir_entries); dir_entries = NULL;

    /* Test 4: Root level "" */
    if (sr_asset_index_list_dir(&short_index, "", -1, &dir_entries, &dir_count) != 1 ||
        dir_count != 1 || strcmp(dir_entries[0].name, "data") != 0 || dir_entries[0].is_dir != 1) {
        free(dir_entries);
        return fail("VFS list_dir root mismatch");
    }
    free(dir_entries); dir_entries = NULL;

    /* Test 5: Non-existent directory returns 0 */
    if (sr_asset_index_list_dir(&short_index, "data/nonexistent", -1, &dir_entries, &dir_count) != 0 ||
        dir_count != 0) {
        free(dir_entries);
        return fail("VFS list_dir nonexistent succeeded unexpectedly");
    }

    /* Adversarial Test 6: Long filenames (> 63 bytes) sharing prefix */
    SrAssetIndex adv_index;
    sr_asset_index_init(&adv_index);
    const char *long_file_a = "data/long/test_prefix_shared_characters_1234567890_1234567890_1234567890_alpha.bin";
    const char *long_file_b = "data/long/test_prefix_shared_characters_1234567890_1234567890_1234567890_beta.bin";
    if (!sr_asset_index_add_sized(&adv_index, long_file_a, "host_a", -1, 100) ||
        !sr_asset_index_add_sized(&adv_index, long_file_b, "host_b", -1, 200) ||
        !sr_asset_index_finalize(&adv_index)) {
        sr_asset_index_destroy(&adv_index);
        return fail("Failed to build long filename index");
    }
    if (sr_asset_index_list_dir(&adv_index, "data/long", -1, &dir_entries, &dir_count) != 1 ||
        dir_count != 2) {
        free(dir_entries);
        sr_asset_index_destroy(&adv_index);
        return fail("VFS list_dir collapsed two long filenames sharing a prefix");
    }
    if (strcmp(dir_entries[0].name, "test_prefix_shared_characters_1234567890_1234567890_1234567890_alpha.bin") != 0 ||
        strcmp(dir_entries[1].name, "test_prefix_shared_characters_1234567890_1234567890_1234567890_beta.bin") != 0) {
        free(dir_entries);
        sr_asset_index_destroy(&adv_index);
        return fail("VFS list_dir long filenames truncated or corrupted");
    }
    free(dir_entries); dir_entries = NULL;
    sr_asset_index_destroy(&adv_index);

    /* Adversarial Test 7: Variant-specific directory enumeration */
    SrAssetIndex var_index;
    sr_asset_index_init(&var_index);
    if (!sr_asset_index_add_sized(&var_index, "data/var/common.bin", "host_c", -1, 50) ||
        !sr_asset_index_add_sized(&var_index, "data/var/text_en.bin", "host_en", 1, 100) ||
        !sr_asset_index_add_sized(&var_index, "data/var/text_fr.bin", "host_fr", 2, 100) ||
        !sr_asset_index_finalize(&var_index)) {
        sr_asset_index_destroy(&var_index);
        return fail("Failed to build variant test index");
    }
    /* Variant 1: should see common.bin + text_en.bin */
    if (sr_asset_index_list_dir(&var_index, "data/var", 1, &dir_entries, &dir_count) != 1 ||
        dir_count != 2) {
        free(dir_entries);
        sr_asset_index_destroy(&var_index);
        return fail("VFS list_dir variant 1 filter failed count");
    }
    if (strcmp(dir_entries[0].name, "common.bin") != 0 ||
        strcmp(dir_entries[1].name, "text_en.bin") != 0) {
        free(dir_entries);
        sr_asset_index_destroy(&var_index);
        return fail("VFS list_dir variant 1 filter mismatch");
    }
    free(dir_entries); dir_entries = NULL;

    /* Variant 2: should see common.bin + text_fr.bin */
    if (sr_asset_index_list_dir(&var_index, "data/var", 2, &dir_entries, &dir_count) != 1 ||
        dir_count != 2) {
        free(dir_entries);
        sr_asset_index_destroy(&var_index);
        return fail("VFS list_dir variant 2 filter failed count");
    }
    if (strcmp(dir_entries[0].name, "common.bin") != 0 ||
        strcmp(dir_entries[1].name, "text_fr.bin") != 0) {
        free(dir_entries);
        sr_asset_index_destroy(&var_index);
        return fail("VFS list_dir variant 2 filter mismatch");
    }
    free(dir_entries); dir_entries = NULL;

    /* All variants (-1): should see common.bin + text_en.bin + text_fr.bin */
    if (sr_asset_index_list_dir(&var_index, "data/var", -1, &dir_entries, &dir_count) != 1 ||
        dir_count != 3) {
        free(dir_entries);
        sr_asset_index_destroy(&var_index);
        return fail("VFS list_dir all variants filter failed count");
    }
    free(dir_entries); dir_entries = NULL;
    sr_asset_index_destroy(&var_index);

    /* Adversarial Test 8: Case-insensitive duplicate collapse */
    SrAssetIndex case_index;
    sr_asset_index_init(&case_index);
    if (!sr_asset_index_add_sized(&case_index, "data/case/testfile.bin", "host_1", -1, 10) ||
        !sr_asset_index_add_sized(&case_index, "data/case/TestFile.bin", "host_2", -1, 20) ||
        !sr_asset_index_finalize(&case_index)) {
        sr_asset_index_destroy(&case_index);
        return fail("Failed to build case test index");
    }
    if (sr_asset_index_list_dir(&case_index, "data/case", -1, &dir_entries, &dir_count) != 1 ||
        dir_count != 1) {
        free(dir_entries);
        sr_asset_index_destroy(&case_index);
        return fail("VFS list_dir failed to collapse case-insensitive duplicates");
    }
    free(dir_entries); dir_entries = NULL;
    sr_asset_index_destroy(&case_index);

    /* Adversarial Test 9: Fail-closed boundary checks */
    if (sr_asset_index_list_dir(NULL, "data", -1, &dir_entries, &dir_count) != -1 ||
        sr_asset_index_list_dir(&short_index, "data", -1, NULL, &dir_count) != -1 ||
        sr_asset_index_list_dir(&short_index, "data", -1, &dir_entries, NULL) != -1) {
        return fail("VFS list_dir did not fail closed on NULL pointers");
    }
    if (sr_asset_index_list_dir(&empty, "data", -1, &dir_entries, &dir_count) != 0) {
        return fail("VFS list_dir did not return 0 for empty index");
    }

    sr_asset_index_destroy(&empty);
    sr_asset_index_destroy(&short_index);
    sr_asset_index_destroy(&long_index);
    puts("asset index selftest: OK");
    return 0;
}
