// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Host-neutral dynamic asset index primitives (issue #223).
 *
 * The Windows HLE uses this table for the extracted-XB data cache, while the
 * standalone selftest exercises the same ownership, growth, sorting, and
 * lookup rules without requiring Windows or private game data.  Host paths are
 * opaque UTF-8 strings here; the Windows layer converts them to wide paths at
 * the I/O boundary.
 */

#ifndef SR_ASSET_INDEX_H
#define SR_ASSET_INDEX_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;                 /* normalized guest-relative key */
    char *host;                /* opaque host path (UTF-8) */
    int variant;               /* -1 = unqualified, otherwise archive variant */
    uint64_t size;             /* size captured during successful enumeration */
} SrAssetIndexEntry;

typedef struct {
    SrAssetIndexEntry *entries;
    size_t count;
    size_t capacity;
    int finalized;
} SrAssetIndex;

static inline char *sr_asset_index_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    if (n == SIZE_MAX) return NULL;
    char *copy = (char *)malloc(n + 1u);
    if (!copy) return NULL;
    memcpy(copy, s, n + 1u);
    return copy;
}

/* Reject malformed UTF-8 before a Windows UTF-16 conversion.  Host paths
 * produced by WideCharToMultiByte always satisfy this contract; the check is
 * primarily for environment/guest strings and gives the portable selftest a
 * deterministic invalid-conversion seam. */
static inline int sr_asset_index_valid_utf8(const char *s) {
    if (!s) return 0;
    const unsigned char *p = (const unsigned char *)s;
    size_t remaining = strlen(s);
    while (remaining != 0u) {
        if (*p < 0x80u) { p++; remaining--; continue; }
        if (*p >= 0xc2u && *p <= 0xdfu) {
            if (remaining < 2u) return 0;
            if ((p[1] & 0xc0u) != 0x80u) return 0;
            p += 2; remaining -= 2u; continue;
        }
        if (*p == 0xe0u) {
            if (remaining < 3u) return 0;
            if (p[1] < 0xa0u || p[1] > 0xbfu || (p[2] & 0xc0u) != 0x80u) return 0;
            p += 3; remaining -= 3u; continue;
        }
        if ((*p >= 0xe1u && *p <= 0xecu) || (*p >= 0xeeu && *p <= 0xefu)) {
            if (remaining < 3u) return 0;
            if ((p[1] & 0xc0u) != 0x80u || (p[2] & 0xc0u) != 0x80u) return 0;
            p += 3; remaining -= 3u; continue;
        }
        if (*p == 0xedu) {
            if (remaining < 3u) return 0;
            if (p[1] < 0x80u || p[1] > 0x9fu || (p[2] & 0xc0u) != 0x80u) return 0;
            p += 3; remaining -= 3u; continue;
        }
        if (*p == 0xf0u) {
            if (remaining < 4u) return 0;
            if (p[1] < 0x90u || p[1] > 0xbfu ||
                (p[2] & 0xc0u) != 0x80u || (p[3] & 0xc0u) != 0x80u) return 0;
            p += 4; remaining -= 4u; continue;
        }
        if (*p >= 0xf1u && *p <= 0xf3u) {
            if (remaining < 4u) return 0;
            if ((p[1] & 0xc0u) != 0x80u || (p[2] & 0xc0u) != 0x80u ||
                (p[3] & 0xc0u) != 0x80u) return 0;
            p += 4; remaining -= 4u; continue;
        }
        if (*p == 0xf4u) {
            if (remaining < 4u) return 0;
            if (p[1] < 0x80u || p[1] > 0x8fu ||
                (p[2] & 0xc0u) != 0x80u || (p[3] & 0xc0u) != 0x80u) return 0;
            p += 4; remaining -= 4u; continue;
        }
        return 0;
    }
    return 1;
}

/* Find an ASCII marker without depending on host locale or archive filename
 * casing.  The extractor accepts `.XB`, `.XB2`, etc. case-insensitively and
 * preserves the source spelling in its `.d` directory name. */
static inline const char *sr_asset_index_find_ci(const char *haystack,
                                                 const char *needle) {
    if (!haystack || !needle || !needle[0]) return NULL;
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            if (a != b) break;
            i++;
        }
        if (i == needle_len) return p;
    }
    return NULL;
}

static inline int sr_asset_index_prefix_ci(const char *text,
                                           const char *prefix) {
    return text && prefix && sr_asset_index_find_ci(text, prefix) == text;
}

/* Convert an extracted-tree relative path to the guest lookup key and retain
 * the archive variant.  Keeping this in the portable core makes the synthetic
 * tree selftest exercise the same key contract as the Windows walker. */
static inline int sr_asset_index_key_from_rel(const char *relative,
                                              char **key_out, int *variant_out) {
    if (!relative || !key_out || !variant_out) return 0;
    *key_out = NULL;
    *variant_out = -1;
    const char *key = relative;
    for (const char *scan = relative;;) {
        const char *xb = sr_asset_index_find_ci(scan, ".xb");
        if (!xb) break;
        const char *suffix = xb + 3;
        int variant = -1;
        uint64_t parsed_variant = 0;
        int variant_overflow = 0;
        while (*suffix >= '0' && *suffix <= '9') {
            uint64_t digit = (uint64_t)(*suffix - '0');
            if (parsed_variant > (UINT64_MAX - digit) / 10u)
                variant_overflow = 1;
            else
                parsed_variant = parsed_variant * 10u + digit;
            suffix++;
        }
        if (!variant_overflow && parsed_variant <= (uint64_t)INT_MAX &&
            sr_asset_index_prefix_ci(suffix, ".d/")) {
            if (parsed_variant != 0u || suffix != xb + 3)
                variant = (int)parsed_variant;
            *variant_out = variant;
            key = suffix + 3;
            break;
        }
        scan = xb + 3;
    }
    char *normalized = sr_asset_index_strdup(key);
    if (!normalized) return 0;
    for (char *p = normalized; *p; p++) {
        if (*p == '\\') *p = '/';
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + ('a' - 'A'));
    }
    *key_out = normalized;
    return 1;
}

static inline void sr_asset_index_init(SrAssetIndex *index) {
    if (!index) return;
    index->entries = NULL;
    index->count = 0;
    index->capacity = 0;
    index->finalized = 0;
}

static inline void sr_asset_index_destroy(SrAssetIndex *index) {
    if (!index) return;
    for (size_t i = 0; i < index->count; i++) {
        free(index->entries[i].key);
        free(index->entries[i].host);
    }
    free(index->entries);
    sr_asset_index_init(index);
}

static inline int sr_asset_index_reserve(SrAssetIndex *index, size_t wanted) {
    if (!index) return 0;
    if (wanted <= index->capacity) return 1;
    size_t next = index->capacity ? index->capacity : 1024u;
    while (next < wanted) {
        if (next > SIZE_MAX / 2u) return 0;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*index->entries)) return 0;
    SrAssetIndexEntry *grown = (SrAssetIndexEntry *)realloc(
        index->entries, next * sizeof(*index->entries));
    if (!grown) return 0;
    if (next > index->capacity)
        memset(grown + index->capacity, 0,
               (next - index->capacity) * sizeof(*grown));
    index->entries = grown;
    index->capacity = next;
    return 1;
}

static inline int sr_asset_index_add(SrAssetIndex *index, const char *key,
                                     const char *host, int variant) {
    if (!index || !key || !host || index->count == SIZE_MAX) return 0;
    if (!sr_asset_index_reserve(index, index->count + 1u)) return 0;
    char *key_copy = sr_asset_index_strdup(key);
    char *host_copy = sr_asset_index_strdup(host);
    if (!key_copy || !host_copy) {
        free(key_copy);
        free(host_copy);
        return 0;
    }
    SrAssetIndexEntry *entry = &index->entries[index->count++];
    entry->key = key_copy;
    entry->host = host_copy;
    entry->variant = variant;
    entry->size = 0;
    index->finalized = 0;
    return 1;
}

/* Add an entry while retaining metadata supplied by the successful directory
 * enumeration.  The Windows data-root walker uses WIN32_FIND_DATAW's checked
 * 64-bit size here; its wide read-open probe runs before this host-neutral
 * record is published. */
static inline int sr_asset_index_add_sized(SrAssetIndex *index, const char *key,
                                           const char *host, int variant,
                                           uint64_t size) {
    if (!sr_asset_index_add(index, key, host, variant)) return 0;
    index->entries[index->count - 1u].size = size;
    return 1;
}

static inline int sr_asset_index_entry_cmp(const void *a, const void *b) {
    const SrAssetIndexEntry *aa = (const SrAssetIndexEntry *)a;
    const SrAssetIndexEntry *bb = (const SrAssetIndexEntry *)b;
    int r = strcmp(aa->key, bb->key);
    if (r) return r;
    if (aa->variant != bb->variant) return aa->variant < bb->variant ? -1 : 1;
    return strcmp(aa->host, bb->host);
}

/* A zero-entry index is never a valid extracted-data result. */
static inline int sr_asset_index_finalize(SrAssetIndex *index) {
    if (!index || !index->entries || index->count == 0) return 0;
    qsort(index->entries, index->count, sizeof(*index->entries),
          sr_asset_index_entry_cmp);
    index->finalized = 1;
    return 1;
}

/* Atomically publish a finalized temporary table.  The destination is not
 * touched when the source is empty/unfinalized or aliases it, and ownership
 * moves without a lossy copy of either path string. */
static inline int sr_asset_index_publish(SrAssetIndex *destination,
                                         SrAssetIndex *source) {
    if (!destination || !source || destination == source || !source->entries ||
        source->count == 0 || !source->finalized) return 0;
    sr_asset_index_destroy(destination);
    *destination = *source;
    sr_asset_index_init(source);
    return 1;
}

static inline size_t sr_asset_index_lower_bound(const SrAssetIndex *index,
                                                const char *key) {
    if (!index || !key) return 0;
    size_t lo = 0, hi = index->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (strcmp(index->entries[mid].key, key) < 0) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

/* ------------------------------------------------------------------------
 * Guest-visible directory enumeration.
 *
 * sceIoDopen/sceIoDread expose ONE namespace to the guest.  Two host sources
 * back it: the writable host overlay (SR_FSDIR) and the read-only extracted
 * data index above.  sceIoOpen resolves the overlay first and the index
 * second, so enumeration merges the same two sources in the same precedence
 * order -- a name the guest sees is a name the guest can open, and the
 * metadata reported is the metadata of the object an open would serve.
 *
 * Everything here is host-neutral: the Windows layer converts to and from
 * wide paths at the I/O boundary and contributes overlay children through the
 * same merge entry point the portable selftest uses.
 * ------------------------------------------------------------------------ */

/* Longest guest-visible component.  Matches IsoDirEntry::name so a name that
 * survives enumeration always survives the sceIoDread copy. */
#define SR_VFS_NAME_MAX 256

typedef struct {
    char name[SR_VFS_NAME_MAX];
    int is_dir;
    uint64_t size;
} SrVfsDirEntry;

typedef struct {
    SrVfsDirEntry *entries;
    size_t count;
    size_t capacity;
    /* A directory node was observed in at least one source.  Kept apart from
     * `count` on purpose: a directory that exists but whose children are all
     * filtered out is EMPTY, not ABSENT, and the two must not collapse into
     * one answer at the sceIoDopen boundary. */
    int exists;
    /* Entries a source offered but this namespace cannot represent (a
     * component >= SR_VFS_NAME_MAX).  Skipping one malformed child keeps an
     * otherwise-valid directory enumerable; the count lets the caller report
     * that the listing is not complete. */
    size_t skipped;
} SrVfsDirList;

static inline int sr_vfs_ascii_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

/* ASCII case-insensitive compare.  Guest keys are ASCII-folded by
 * sr_asset_index_normalize_key, so this orders folded names exactly as
 * strcmp orders the keys they came from. */
static inline int sr_vfs_strcasecmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    for (;;) {
        int c1 = sr_vfs_ascii_tolower((unsigned char)*s1);
        int c2 = sr_vfs_ascii_tolower((unsigned char)*s2);
        if (c1 != c2) return c1 < c2 ? -1 : 1;
        if (c1 == 0) return 0;
        s1++;
        s2++;
    }
}

static inline int sr_vfs_dir_entry_cmp(const void *a, const void *b) {
    const SrVfsDirEntry *ea = (const SrVfsDirEntry *)a;
    const SrVfsDirEntry *eb = (const SrVfsDirEntry *)b;
    int r = sr_vfs_strcasecmp(ea->name, eb->name);
    if (r != 0) return r;
    return strcmp(ea->name, eb->name);
}

/* Does `entry_variant` participate in a request for `wanted_variant`?
 *
 * This is the ONE rule shared by enumeration and by lookup.  A qualified
 * request (a localized root the guest named explicitly) selects exactly that
 * archive variant; an unqualified request considers every variant and lets
 * the caller pick.  Enumeration and open must not disagree here, or the guest
 * sees names it cannot open. */
static inline int sr_asset_index_variant_selected(int entry_variant, int wanted_variant) {
    if (wanted_variant < 0) return 1;
    return entry_variant == wanted_variant;
}

static inline void sr_vfs_dirlist_init(SrVfsDirList *list) {
    if (!list) return;
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
    list->exists = 0;
    list->skipped = 0;
}

static inline void sr_vfs_dirlist_destroy(SrVfsDirList *list) {
    if (!list) return;
    free(list->entries);
    sr_vfs_dirlist_init(list);
}

static inline int sr_vfs_dirlist_reserve(SrVfsDirList *list, size_t wanted) {
    if (!list) return 0;
    if (wanted <= list->capacity) return 1;
    size_t next = list->capacity ? list->capacity : 32u;
    while (next < wanted) {
        if (next > SIZE_MAX / 2u) return 0;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*list->entries)) return 0;
    SrVfsDirEntry *grown = (SrVfsDirEntry *)realloc(list->entries,
                                                    next * sizeof(*list->entries));
    if (!grown) return 0;
    list->entries = grown;
    list->capacity = next;
    return 1;
}

/* Contribute one child to the listing.
 *
 * Merge policy, applied in sceIoOpen's own source order (overlay first, index
 * second, each source's own children in its own order):
 *
 *   - the FIRST source to contribute a name owns that name's spelling and its
 *     metadata, because that is the source an open of that name resolves to;
 *   - a later source observing the same name as a DIRECTORY promotes is_dir,
 *     since a directory node is real wherever it appears;
 *   - a later source never overwrites a spelling.  Guest-visible casing is
 *     therefore a function of the sources present, never of the order two
 *     equal-precedence sources happened to be walked in.
 *
 * Returns 1 when the child was merged, 0 on allocation failure.  A name that
 * does not fit the namespace is counted in `skipped` and reported as merged,
 * so one unrepresentable child never destroys an otherwise-valid directory. */
static inline int sr_vfs_dirlist_merge(SrVfsDirList *list, const char *name,
                                       int is_dir, uint64_t size) {
    if (!list || !name) return 0;
    size_t len = strlen(name);
    if (len == 0u || len >= SR_VFS_NAME_MAX) {
        list->skipped++;
        return 1;
    }
    /* The index streams its children in sorted order, so a repeat of the name
     * just merged is the common case and is answered without a scan. */
    if (list->count > 0u &&
        sr_vfs_strcasecmp(list->entries[list->count - 1u].name, name) == 0) {
        if (is_dir) list->entries[list->count - 1u].is_dir = 1;
        return 1;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (sr_vfs_strcasecmp(list->entries[i].name, name) != 0) continue;
        if (is_dir) list->entries[i].is_dir = 1;
        return 1;
    }
    if (!sr_vfs_dirlist_reserve(list, list->count + 1u)) return 0;
    SrVfsDirEntry *slot = &list->entries[list->count];
    memcpy(slot->name, name, len + 1u);
    slot->is_dir = is_dir;
    slot->size = is_dir ? 0u : size;
    list->count++;
    return 1;
}

static inline void sr_vfs_dirlist_sort(SrVfsDirList *list) {
    if (!list || list->count < 2u) return;
    qsort(list->entries, list->count, sizeof(*list->entries), sr_vfs_dir_entry_cmp);
}

/* Normalize a guest directory key into the "dir/" prefix the index is keyed
 * by.  Leading and trailing slashes are optional; NULL or "" is the root and
 * yields an empty prefix.  Returns 1 on success, 0 when the key cannot fit. */
static inline int sr_asset_index_dir_prefix(const char *dir_key, char *out,
                                            size_t cap, size_t *len_out) {
    if (!out || cap == 0u || !len_out) return 0;
    out[0] = '\0';
    *len_out = 0;
    if (!dir_key || dir_key[0] == '\0') return 1;
    const char *p = dir_key;
    while (*p == '/') p++;
    size_t len = strlen(p);
    while (len > 0u && p[len - 1u] == '/') len--;
    if (len == 0u) return 1;
    if (len + 2u > cap) return 0;
    memcpy(out, p, len);
    out[len] = '/';
    out[len + 1u] = '\0';
    *len_out = len + 1u;
    return 1;
}

/* Recover a child's on-host spelling from an indexed entry.
 *
 * Index KEYS are ASCII-folded, so reporting a child straight out of the key
 * would hand the guest a lowercased name for an object whose real name may be
 * mixed case.  The entry's host path preserves the original spelling, and the
 * key's tail and the host path's tail describe the same component sequence, so
 * the child is the component `depth` positions from the end of the host path
 * (depth 1 = last component).  Falls back to the folded key component when the
 * host path cannot supply one, which keeps the listing complete rather than
 * dropping a real entry. */
static inline void sr_asset_index_child_name(const char *host, size_t depth,
                                             const char *folded, size_t folded_len,
                                             char *out, size_t cap) {
    out[0] = '\0';
    if (host && depth > 0u) {
        const char *end = host + strlen(host);
        /* Trailing separators are not part of a component. */
        while (end > host && (end[-1] == '/' || end[-1] == '\\')) end--;
        const char *stop = end;
        size_t seen = 0;
        while (stop > host) {
            const char *begin = stop;
            while (begin > host && begin[-1] != '/' && begin[-1] != '\\') begin--;
            seen++;
            if (seen == depth) {
                size_t clen = (size_t)(stop - begin);
                if (clen > 0u && clen < cap) {
                    memcpy(out, begin, clen);
                    out[clen] = '\0';
                    return;
                }
                break;
            }
            stop = begin;
            while (stop > host && (stop[-1] == '/' || stop[-1] == '\\')) stop--;
        }
    }
    if (folded_len > 0u && folded_len < cap) {
        memcpy(out, folded, folded_len);
        out[folded_len] = '\0';
    }
}

/* Enumerate the direct children of a normalized directory key.
 *
 * `dir_key` is a folded, forward-slashed guest key ("data/chara/model"), with
 * optional leading/trailing slashes; NULL or "" is the root.  `wanted_variant`
 * follows sr_asset_index_variant_selected.  Children are merged into `list`,
 * which the caller initializes and destroys, so an overlay source can be
 * merged into the same listing before or after this call.
 *
 * Returns 1 on success (inspect list->exists for whether the directory is in
 * this source at all) and -1 when the key is unusable or allocation fails.
 * `list->exists` is set from the prefix scan alone and is deliberately
 * independent of the variant filter: a directory whose every child belongs to
 * another archive variant EXISTS and is EMPTY. */
static inline int sr_asset_index_list_dir(const SrAssetIndex *index,
                                          const char *dir_key,
                                          int wanted_variant,
                                          SrVfsDirList *list) {
    if (!index || !list) return -1;
    char prefix[512];
    size_t prefix_len = 0;
    if (!sr_asset_index_dir_prefix(dir_key, prefix, sizeof(prefix), &prefix_len)) return -1;
    if (!index->entries || index->count == 0u) return 1;

    size_t first = sr_asset_index_lower_bound(index, prefix_len > 0u ? prefix : "");
    for (size_t i = first; i < index->count; i++) {
        const SrAssetIndexEntry *entry = &index->entries[i];
        const char *key = entry->key;
        if (prefix_len > 0u && strncmp(key, prefix, prefix_len) != 0) break;
        const char *tail = key + prefix_len;
        if (*tail == '\0') continue;
        /* The prefix range is non-empty, so the directory node is real even if
         * every child below is filtered out by variant. */
        list->exists = 1;
        if (!sr_asset_index_variant_selected(entry->variant, wanted_variant)) continue;

        const char *slash = strchr(tail, '/');
        int is_dir = slash != NULL;
        size_t folded_len = is_dir ? (size_t)(slash - tail) : strlen(tail);
        /* Depth of the child inside the host path: 1 for a file child, one
         * more for each further separator in the tail. */
        size_t depth = 1u;
        for (const char *q = tail + folded_len; *q; q++)
            if (*q == '/') depth++;

        /* The guest's directory-entry size field is 32-bit. An indexed file too
         * large to describe is skipped rather than narrowed: reporting a
         * truncated size would be a lie the guest cannot detect. */
        if (!is_dir && entry->size > 0xFFFFFFFFull) {
            list->skipped++;
            continue;
        }

        char child[SR_VFS_NAME_MAX];
        sr_asset_index_child_name(entry->host, depth, tail, folded_len,
                                  child, sizeof(child));
        if (child[0] == '\0') {
            list->skipped++;
            continue;
        }
        if (!sr_vfs_dirlist_merge(list, child, is_dir, is_dir ? 0u : entry->size))
            return -1;
    }
    return 1;
}

#endif /* SR_ASSET_INDEX_H */
