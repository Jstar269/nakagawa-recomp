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

typedef struct {
    char name[256];
    int is_dir;
    uint64_t size;
} SrVfsDirEntry;

static inline int sr_vfs_ascii_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static inline int sr_vfs_strcasecmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    while (*s1 && *s2) {
        int c1 = sr_vfs_ascii_tolower((unsigned char)*s1);
        int c2 = sr_vfs_ascii_tolower((unsigned char)*s2);
        if (c1 != c2) return c1 < c2 ? -1 : 1;
        s1++;
        s2++;
    }
    int c1 = sr_vfs_ascii_tolower((unsigned char)*s1);
    int c2 = sr_vfs_ascii_tolower((unsigned char)*s2);
    return c1 < c2 ? -1 : (c1 > c2 ? 1 : 0);
}

static inline int sr_vfs_dir_entry_cmp(const void *a, const void *b) {
    const SrVfsDirEntry *ea = (const SrVfsDirEntry *)a;
    const SrVfsDirEntry *eb = (const SrVfsDirEntry *)b;
    int r = sr_vfs_strcasecmp(ea->name, eb->name);
    if (r != 0) return r;
    return strcmp(ea->name, eb->name);
}

/* Enumerate the direct children of a normalized directory key from a finalized SrAssetIndex.
 * - `dir_key`: normalized directory key, lowercase with forward slashes (e.g. "data/chara/model/body").
 *   Leading and trailing slashes may be present or absent. An empty string or NULL represents the root.
 * - `wanted_variant`: archive variant filter:
 *     >= 0: only include entries matching this variant or unqualified (-1).
 *     < 0:  include all entries across variants.
 * - `entries_out`: points to a malloc'd array of SrVfsDirEntry upon success, which caller frees.
 * - `count_out`: receives the number of entries in `entries_out`.
 * Returns:
 *   1 on success (matching directory found and direct children enumerated, count_out >= 0).
 *   0 if the directory does not exist in the index (no entries match the prefix).
 *  -1 on memory allocation error or overlong path component (> 255 bytes).
 */
static inline int sr_asset_index_list_dir(const SrAssetIndex *index,
                                          const char *dir_key,
                                          int wanted_variant,
                                          SrVfsDirEntry **entries_out,
                                          size_t *count_out) {
    if (!index || !entries_out || !count_out) return -1;
    *entries_out = NULL;
    *count_out = 0;
    if (!index->entries || index->count == 0) return 0;

    char prefix[512];
    size_t prefix_len = 0;
    if (dir_key && dir_key[0] != '\0') {
        const char *p = dir_key;
        while (*p == '/') p++;
        size_t len = strlen(p);
        while (len > 0 && p[len - 1] == '/') len--;
        if (len > 0) {
            if (len >= sizeof(prefix) - 2u) return -1;
            memcpy(prefix, p, len);
            prefix[len] = '/';
            prefix[len + 1] = '\0';
            prefix_len = len + 1;
        }
    }

    size_t first = sr_asset_index_lower_bound(index, prefix_len > 0 ? prefix : "");
    if (prefix_len > 0) {
        if (first >= index->count || strncmp(index->entries[first].key, prefix, prefix_len) != 0) {
            return 0; /* Directory does not exist in the index */
        }
    } else if (index->count == 0) {
        return 0;
    }

    size_t cap = 64;
    size_t count = 0;
    SrVfsDirEntry *entries = (SrVfsDirEntry *)malloc(cap * sizeof(SrVfsDirEntry));
    if (!entries) return -1;

    for (size_t i = first; i < index->count; i++) {
        const char *key = index->entries[i].key;
        if (prefix_len > 0 && strncmp(key, prefix, prefix_len) != 0) break;

        /* Filter by variant if requested */
        if (wanted_variant >= 0) {
            int evar = index->entries[i].variant;
            if (evar != -1 && evar != wanted_variant) continue;
        }

        const char *tail = key + prefix_len;
        if (*tail == '\0') continue;

        const char *slash = strchr(tail, '/');
        char child[256];
        int is_dir = 0;
        uint64_t size = 0;
        if (slash != NULL) {
            size_t clen = (size_t)(slash - tail);
            if (clen >= sizeof(child)) {
                free(entries);
                return -1; /* Name component exceeds buffer, fail closed */
            }
            memcpy(child, tail, clen);
            child[clen] = '\0';
            is_dir = 1;
            size = 0;
        } else {
            size_t clen = strlen(tail);
            if (clen >= sizeof(child)) {
                free(entries);
                return -1; /* Name component exceeds buffer, fail closed */
            }
            memcpy(child, tail, clen);
            child[clen] = '\0';
            is_dir = 0;
            size = index->entries[i].size;
        }

        /* Check for duplicate of the previously added child (index is sorted by key) */
        if (count > 0 && sr_vfs_strcasecmp(entries[count - 1].name, child) == 0) {
            if (is_dir) entries[count - 1].is_dir = 1;
            continue;
        }

        if (count == cap) {
            if (cap > SIZE_MAX / (2u * sizeof(SrVfsDirEntry))) {
                free(entries);
                return -1;
            }
            size_t next_cap = cap * 2u;
            SrVfsDirEntry *grown = (SrVfsDirEntry *)realloc(entries, next_cap * sizeof(SrVfsDirEntry));
            if (!grown) {
                free(entries);
                return -1;
            }
            entries = grown;
            cap = next_cap;
        }

        memcpy(entries[count].name, child, strlen(child) + 1u);
        entries[count].is_dir = is_dir;
        entries[count].size = size;
        count++;
    }

    if (count == 0 && prefix_len > 0) {
        /* All matching entries were filtered out by variant */
        free(entries);
        return 0;
    }

    *entries_out = entries;
    *count_out = count;
    return 1;
}

#endif /* SR_ASSET_INDEX_H */
