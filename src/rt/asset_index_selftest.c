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

/* ---- Guest-visible directory enumeration -------------------------------
 *
 * These run against the same host-neutral merge entry point the Windows HLE
 * uses for sceIoDopen, so the merge policy, the variant rule, the ABSENT vs
 * EMPTY distinction and the name-recovery rule are all covered without a
 * Windows host or private game data. */

static int dir_index_of(const SrVfsDirList *list, const char *name) {
    for (size_t i = 0; i < list->count; i++)
        if (strcmp(list->entries[i].name, name) == 0) return (int)i;
    return -1;
}

static int vfs_dir_tests(void) {
    SrAssetIndex ix;
    SrVfsDirList list;

    /* A tree whose KEYS are folded but whose host paths keep real casing, plus
     * a second archive variant and a localized-only child. */
    sr_asset_index_init(&ix);
    if (!sr_asset_index_add_sized(&ix, "data/chara/model/body.gim",
                                  "root/Data/Chara/Model/Body.gim", -1, 100u) ||
        !sr_asset_index_add_sized(&ix, "data/chara/model/body.gim",
                                  "root/Data/Chara/Model/Body.gim", 2, 100u) ||
        !sr_asset_index_add_sized(&ix, "data/chara/model/head.gim",
                                  "root/Data/Chara/Model/Head.gim", -1, 200u) ||
        !sr_asset_index_add_sized(&ix, "data/chara/parameter/parts.txt",
                                  "root/Data/Chara/Parameter/Parts.txt", -1, 300u) ||
        !sr_asset_index_add_sized(&ix, "data/lang/only_fr.bin",
                                  "root/Data/Lang/Only_FR.bin", 2, 400u) ||
        !sr_asset_index_finalize(&ix)) {
        sr_asset_index_destroy(&ix);
        return fail("VFS fixture index construction failed");
    }

    /* Direct children only, with on-host casing recovered from the host path
     * rather than reported out of the folded key. */
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(&ix, "data/chara/model", -1, &list) != 1 ||
        !list.exists || list.count != 2u) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS direct-child enumeration wrong");
    }
    sr_vfs_dirlist_sort(&list);
    if (dir_index_of(&list, "Body.gim") < 0 || dir_index_of(&list, "Head.gim") < 0) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS enumeration folded the on-host name casing");
    }
    if (list.entries[dir_index_of(&list, "Body.gim")].size != 100u ||
        list.entries[dir_index_of(&list, "Body.gim")].is_dir != 0) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS file metadata wrong");
    }
    sr_vfs_dirlist_destroy(&list);

    /* A grandchild contributes its DIRECTORY component, not a recursive walk,
     * and that component also keeps its host casing. */
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(&ix, "data/chara", -1, &list) != 1 ||
        list.count != 2u ||
        dir_index_of(&list, "Model") < 0 || dir_index_of(&list, "Parameter") < 0 ||
        !list.entries[dir_index_of(&list, "Model")].is_dir ||
        list.entries[dir_index_of(&list, "Model")].size != 0u) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS grandchild collapse wrong");
    }
    sr_vfs_dirlist_destroy(&list);

    /* Trailing/leading slashes and the root are all the same namespace. */
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(&ix, "/data/chara/", -1, &list) != 1 || list.count != 2u) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS slash normalization wrong");
    }
    sr_vfs_dirlist_destroy(&list);
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(&ix, "", -1, &list) != 1 ||
        list.count != 1u || dir_index_of(&list, "Data") < 0) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS root enumeration wrong");
    }
    sr_vfs_dirlist_destroy(&list);

    /* A qualified request selects EXACTLY its variant -- the same rule the
     * lookup uses -- so enumeration never offers a name the open would refuse. */
    if (sr_asset_index_variant_selected(-1, 2) || !sr_asset_index_variant_selected(2, 2) ||
        sr_asset_index_variant_selected(3, 2) || !sr_asset_index_variant_selected(3, -2) ||
        !sr_asset_index_variant_selected(-1, -1)) {
        sr_asset_index_destroy(&ix);
        return fail("VFS variant selection rule disagrees with lookup");
    }
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(&ix, "data/chara/model", 2, &list) != 1 ||
        list.count != 1u || dir_index_of(&list, "Body.gim") < 0) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS qualified-variant enumeration wrong");
    }
    sr_vfs_dirlist_destroy(&list);

    /* EXISTS and EMPTY are not the same answer as ABSENT: every child of this
     * directory belongs to another variant, so it exists with zero entries. */
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(&ix, "data/lang", 1, &list) != 1 ||
        !list.exists || list.count != 0u) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS variant-emptied directory did not stay EXISTING");
    }
    sr_vfs_dirlist_destroy(&list);

    /* A directory no source has is absent, and says so distinctly. */
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(&ix, "data/nope", -1, &list) != 1 ||
        list.exists || list.count != 0u) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS absent directory reported as existing");
    }
    sr_vfs_dirlist_destroy(&list);

    /* Overlay precedence: the first source to contribute a name owns its
     * spelling and metadata, and a later directory sighting only promotes
     * is_dir.  Guest-visible casing therefore does not depend on which
     * sources happen to be present alongside it. */
    sr_vfs_dirlist_init(&list);
    if (!sr_vfs_dirlist_merge(&list, "Body.gim", 0, 999u) ||
        sr_asset_index_list_dir(&ix, "data/chara/model", -1, &list) != 1 ||
        list.count != 2u ||
        strcmp(list.entries[dir_index_of(&list, "Body.gim")].name, "Body.gim") != 0 ||
        list.entries[dir_index_of(&list, "Body.gim")].size != 999u) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS overlay precedence not first-source-wins");
    }
    sr_vfs_dirlist_destroy(&list);
    sr_vfs_dirlist_init(&list);
    if (!sr_vfs_dirlist_merge(&list, "Model", 0, 7u) ||
        sr_asset_index_list_dir(&ix, "data/chara", -1, &list) != 1 ||
        list.count != 2u || !list.entries[dir_index_of(&list, "Model")].is_dir) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS directory promotion across sources wrong");
    }
    sr_vfs_dirlist_destroy(&list);

    /* Case-insensitive duplicates collapse to one guest-visible entry. */
    sr_vfs_dirlist_init(&list);
    if (!sr_vfs_dirlist_merge(&list, "Body.gim", 0, 1u) ||
        !sr_vfs_dirlist_merge(&list, "BODY.GIM", 0, 2u) ||
        !sr_vfs_dirlist_merge(&list, "body.gim", 0, 3u) ||
        list.count != 1u || strcmp(list.entries[0].name, "Body.gim") != 0 ||
        list.entries[0].size != 1u) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS case-insensitive collapse wrong");
    }
    sr_vfs_dirlist_destroy(&list);

    /* Fail-closed inputs. */
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(NULL, "data", -1, &list) != -1 ||
        sr_asset_index_list_dir(&ix, "data", -1, NULL) != -1) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS list_dir did not fail closed on NULL");
    }
    {
        char overlong[1024];
        memset(overlong, 'a', sizeof(overlong) - 1u);
        overlong[sizeof(overlong) - 1u] = '\0';
        if (sr_asset_index_list_dir(&ix, overlong, -1, &list) != -1) {
            sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
            return fail("VFS list_dir accepted an unrepresentable key");
        }
    }
    sr_vfs_dirlist_destroy(&list);
    sr_asset_index_destroy(&ix);

    /* An empty index enumerates as an absent directory, never as an error and
     * never as a phantom listing. */
    sr_asset_index_init(&ix);
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(&ix, "data", -1, &list) != 1 ||
        list.exists || list.count != 0u) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS empty index did not enumerate as absent");
    }
    sr_vfs_dirlist_destroy(&list);
    sr_asset_index_destroy(&ix);

    /* One child the guest namespace cannot represent is skipped and counted;
     * the rest of the directory still enumerates. */
    sr_asset_index_init(&ix);
    {
        char big_key[600];
        char big_host[600];
        size_t n = 300u;
        memcpy(big_key, "data/skip/", 10u);
        memset(big_key + 10, 'n', n);
        big_key[10 + n] = '\0';
        memcpy(big_host, "root/skip/", 10u);
        memset(big_host + 10, 'n', n);
        big_host[10 + n] = '\0';
        if (!sr_asset_index_add_sized(&ix, "data/skip/ok.bin", "root/skip/OK.bin", -1, 5u) ||
            !sr_asset_index_add_sized(&ix, big_key, big_host, -1, 6u) ||
            !sr_asset_index_add_sized(&ix, "data/skip/huge.bin", "root/skip/Huge.bin", -1,
                                      0x100000000ull) ||
            !sr_asset_index_finalize(&ix)) {
            sr_asset_index_destroy(&ix);
            return fail("VFS skip fixture construction failed");
        }
    }
    sr_vfs_dirlist_init(&list);
    if (sr_asset_index_list_dir(&ix, "data/skip", -1, &list) != 1 ||
        !list.exists || list.count != 1u || list.skipped != 2u ||
        dir_index_of(&list, "OK.bin") < 0) {
        sr_vfs_dirlist_destroy(&list); sr_asset_index_destroy(&ix);
        return fail("VFS unrepresentable child destroyed a valid directory");
    }
    sr_vfs_dirlist_destroy(&list);
    sr_asset_index_destroy(&ix);
    return 0;
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

    if (vfs_dir_tests() != 0) return 1;
    sr_asset_index_destroy(&short_index);
    sr_asset_index_destroy(&long_index);
    puts("asset index selftest: OK");
    return 0;
}
