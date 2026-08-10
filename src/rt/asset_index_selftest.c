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
    if (sr_asset_index_finalize(&empty) != 0) return fail("empty index accepted");
    sr_asset_index_destroy(&empty);
    sr_asset_index_destroy(&short_index);
    sr_asset_index_destroy(&long_index);
    puts("asset index selftest: OK");
    return 0;
}
