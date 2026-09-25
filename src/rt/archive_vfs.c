/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "archive_vfs.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int archive_lower(int value) {
    if (value >= 'A' && value <= 'Z') return value + ('a' - 'A');
    return value;
}

int sr_archive_variant_from_name(const char *name, int *variant_out) {
    if (!name || !variant_out) return 0;
    const char *base = strrchr(name, '.');
    if (!base || base[1] == '\0' ||
        !((base[1] == 'x' || base[1] == 'X') &&
          (base[2] == 'b' || base[2] == 'B'))) return 0;
    const char *suffix = base + 1;
    if (!(suffix[0] == 'x' || suffix[0] == 'X') ||
        !(suffix[1] == 'b' || suffix[1] == 'B')) return 0;
    suffix += 2;
    if (*suffix == '\0') {
        *variant_out = -1;
        return 1;
    }
    uint64_t value = 0;
    while (*suffix) {
        if (*suffix < '0' || *suffix > '9') return 0;
        uint64_t digit = (uint64_t)(*suffix - '0');
        if (value > ((uint64_t)INT_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        suffix++;
    }
    *variant_out = (int)value;
    return 1;
}

static int archive_normalize_key(const char *input, char *out, size_t capacity,
                                 int allow_empty) {
    if (!input || !out || capacity == 0u) return 0;
    const char *p = input;
    while (*p == '/' || *p == '\\') p++;
    size_t out_pos = 0;
    size_t component_start = 0;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            if (out_pos == component_start) return 0;
            if (out_pos - component_start == 1u && out[component_start] == '.') return 0;
            if (out_pos - component_start == 2u && out[component_start] == '.' &&
                out[component_start + 1u] == '.') return 0;
            if (out_pos + 1u >= capacity) return 0;
            out[out_pos++] = '/';
            component_start = out_pos;
            while (*p == '/' || *p == '\\') p++;
            continue;
        }
        unsigned char value = (unsigned char)*p++;
        if (value < 0x20u || value == 0x7fu || value == ':' || value == '*' ||
            value == '?' || value == '"' || value == '<' || value == '>' ||
            value == '|') return 0;
        if (out_pos + 1u >= capacity) return 0;
        out[out_pos++] = (char)archive_lower(value);
    }
    if (out_pos == component_start) {
        if (allow_empty) {
            out[0] = '\0';
            return 1;
        }
        return 0;
    }
    if (out_pos - component_start == 1u && out[component_start] == '.') return 0;
    if (out_pos - component_start == 2u && out[component_start] == '.' &&
        out[component_start + 1u] == '.') return 0;
    out[out_pos] = '\0';
    return 1;
}

static int archive_normalize_directory(const char *input, char *out, size_t capacity) {
    if (!input || !out || capacity == 0u) return 0;
    const char *start = input;
    while (*start == '/' || *start == '\\') start++;
    const char *end = start + strlen(start);
    while (end > start && (end[-1] == '/' || end[-1] == '\\')) end--;
    if (end == start) {
        out[0] = '\0';
        return 1;
    }
    size_t length = (size_t)(end - start);
    if (length >= capacity) return 0;
    char temporary[NK_XB_MAX_NAME_BYTES + 1u];
    if (length >= sizeof(temporary)) return 0;
    memcpy(temporary, start, length);
    temporary[length] = '\0';
    return archive_normalize_key(temporary, out, capacity, 0);
}

static int archive_index_cmp(const void *left, const void *right) {
    const SrArchiveIndexEntry *a = (const SrArchiveIndexEntry *)left;
    const SrArchiveIndexEntry *b = (const SrArchiveIndexEntry *)right;
    int result = strcmp(a->key, b->key);
    if (result) return result;
    if (a->variant != b->variant) return a->variant < b->variant ? -1 : 1;
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    return 0;
}

static size_t archive_index_lower_bound(const SrArchiveVfs *vfs, const char *key) {
    if (!vfs || !key) return 0;
    size_t low = 0;
    size_t high = vfs->index_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (strcmp(vfs->index[middle].key, key) < 0) low = middle + 1u;
        else high = middle;
    }
    return low;
}

static int archive_index_reserve(SrArchiveVfs *vfs, size_t wanted) {
    if (!vfs || wanted <= vfs->index_capacity) return 1;
    size_t next = vfs->index_capacity ? vfs->index_capacity : 256u;
    while (next < wanted) {
        if (next > SIZE_MAX / 2u) return 0;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*vfs->index)) return 0;
    SrArchiveIndexEntry *grown = (SrArchiveIndexEntry *)realloc(
        vfs->index, next * sizeof(*grown));
    if (!grown) return 0;
    memset(grown + vfs->index_capacity, 0,
           (next - vfs->index_capacity) * sizeof(*grown));
    vfs->index = grown;
    vfs->index_capacity = next;
    return 1;
}

static int archive_mount_reserve(SrArchiveVfs *vfs, size_t wanted) {
    if (!vfs || wanted <= vfs->mount_capacity) return 1;
    size_t next = vfs->mount_capacity ? vfs->mount_capacity : 4u;
    while (next < wanted) {
        if (next > SIZE_MAX / 2u) return 0;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*vfs->mounts)) return 0;
    NkXbArchive *grown = (NkXbArchive *)realloc(vfs->mounts, next * sizeof(*grown));
    if (!grown) return 0;
    memset(grown + vfs->mount_capacity, 0,
           (next - vfs->mount_capacity) * sizeof(*grown));
    vfs->mounts = grown;
    vfs->mount_capacity = next;
    return 1;
}

static char *archive_strdup_lower(const char *value) {
    char normalized[NK_XB_MAX_NAME_BYTES + 1u];
    if (!archive_normalize_key(value, normalized, sizeof(normalized), 0)) return NULL;
    size_t length = strlen(normalized);
    char *copy = (char *)malloc(length + 1u);
    if (!copy) return NULL;
    memcpy(copy, normalized, length + 1u);
    return copy;
}

static char *archive_strdup(const char *value) {
    if (!value) return NULL;
    size_t length = strlen(value);
    if (length == SIZE_MAX) return NULL;
    char *copy = (char *)malloc(length + 1u);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1u);
    return copy;
}

static void archive_index_rollback(SrArchiveVfs *vfs, size_t first) {
    if (!vfs) return;
    while (vfs->index_count > first) {
        SrArchiveIndexEntry *entry = &vfs->index[--vfs->index_count];
        free(entry->key);
        free(entry->path);
        memset(entry, 0, sizeof(*entry));
    }
}

void sr_archive_vfs_init(SrArchiveVfs *vfs) {
    if (!vfs) return;
    memset(vfs, 0, sizeof(*vfs));
    vfs->max_cache_bytes = SR_ARCHIVE_VFS_DEFAULT_CACHE_BYTES;
    vfs->max_cache_entries = SR_ARCHIVE_VFS_DEFAULT_CACHE_ENTRIES;
}

void sr_archive_vfs_destroy(SrArchiveVfs *vfs) {
    if (!vfs) return;
    for (size_t i = 0; i < vfs->mount_count; i++) nk_xb_close(&vfs->mounts[i]);
    free(vfs->mounts);
    for (size_t i = 0; i < vfs->index_count; i++) {
        free(vfs->index[i].key);
        free(vfs->index[i].path);
    }
    free(vfs->index);
    for (size_t i = 0; i < vfs->cache_count; i++) free(vfs->cache[i].data);
    free(vfs->cache);
    sr_archive_vfs_init(vfs);
}

int sr_archive_vfs_configure(SrArchiveVfs *vfs, size_t max_cache_bytes,
                             size_t max_cache_entries) {
    if (!vfs || vfs->mount_count != 0u || vfs->cache_count != 0u) return 0;
    vfs->max_cache_bytes = max_cache_bytes;
    vfs->max_cache_entries = max_cache_entries;
    return 1;
}

static NkResult archive_adopt(SrArchiveVfs *vfs, NkXbArchive *archive, int variant) {
    if (!vfs || !archive || archive->entry_count == 0u || variant < -1) {
        return NK_ERROR_INVALID_XB;
    }
    if (archive->entry_count > SIZE_MAX - vfs->index_count ||
        !archive_mount_reserve(vfs, vfs->mount_count + 1u) ||
        !archive_index_reserve(vfs, vfs->index_count + archive->entry_count)) {
        return NK_ERROR_OUT_OF_MEMORY;
    }
    size_t first = vfs->index_count;
    for (size_t i = 0; i < archive->entry_count; i++) {
        const NkXbEntry *source = &archive->entries[i];
        if (source->expanded_size > UINT32_MAX) {
            archive_index_rollback(vfs, first);
            return NK_ERROR_INVALID_XB;
        }
        SrArchiveIndexEntry *target = &vfs->index[vfs->index_count];
        target->key = archive_strdup_lower(source->path);
        target->path = archive_strdup(source->path);
        if (!target->key || !target->path) {
            free(target->key);
            free(target->path);
            target->key = NULL;
            target->path = NULL;
            archive_index_rollback(vfs, first);
            return NK_ERROR_OUT_OF_MEMORY;
        }
        target->mount_index = vfs->mount_count;
        target->entry_index = source->index;
        target->size = source->expanded_size;
        target->variant = variant;
        target->order = vfs->index_count;
        vfs->index_count++;
    }
    vfs->mounts[vfs->mount_count++] = *archive;
    memset(archive, 0, sizeof(*archive));
    qsort(vfs->index, vfs->index_count, sizeof(*vfs->index), archive_index_cmp);
    return NK_OK;
}

NkResult sr_archive_vfs_mount_file(SrArchiveVfs *vfs, const char *path,
                                   bool big_endian, int variant,
                                   const NkXbLimits *limits) {
    if (!vfs || !path || !path[0]) return NK_ERROR_INVALID_XB;
    int actual_variant = variant;
    if (actual_variant == SR_ARCHIVE_VARIANT_AUTO) {
        if (!sr_archive_variant_from_name(path, &actual_variant)) return NK_ERROR_INVALID_XB;
    }
    if (actual_variant < -1) return NK_ERROR_INVALID_XB;
    NkXbArchive archive;
    NkResult result = nk_xb_open_file(path, big_endian, limits, &archive,
                                      NULL, 0);
    if (result != NK_OK) return result;
    result = archive_adopt(vfs, &archive, actual_variant);
    if (result != NK_OK) nk_xb_close(&archive);
    return result;
}

NkResult sr_archive_vfs_mount_memory(SrArchiveVfs *vfs, const void *data,
                                     size_t data_size, const char *source_name,
                                     bool big_endian, int variant,
                                     const NkXbLimits *limits) {
    if (!vfs || !data || data_size == 0u || variant < -1) return NK_ERROR_INVALID_XB;
    NkXbArchive archive;
    NkResult result = nk_xb_open_memory(data, data_size, source_name, big_endian,
                                        limits, &archive, NULL, 0);
    if (result != NK_OK) return result;
    result = archive_adopt(vfs, &archive, variant);
    if (result != NK_OK) nk_xb_close(&archive);
    return result;
}

static int archive_file_from_index(const SrArchiveVfs *vfs,
                                   const SrArchiveIndexEntry *index,
                                   SrArchiveFile *file_out) {
    if (!vfs || !index || !file_out || index->mount_index >= vfs->mount_count) return 0;
    const NkXbArchive *mount = &vfs->mounts[index->mount_index];
    if (index->entry_index >= mount->entry_count) return 0;
    file_out->mount_index = index->mount_index;
    file_out->entry_index = index->entry_index;
    file_out->size = index->size;
    file_out->variant = index->variant;
    return 1;
}

int sr_archive_vfs_lookup(const SrArchiveVfs *vfs, const char *key,
                          int wanted_variant, SrArchiveFile *file_out) {
    if (!vfs || !key || !file_out) return 0;
    memset(file_out, 0, sizeof(*file_out));
    char normalized[NK_XB_MAX_NAME_BYTES + 1u];
    if (!archive_normalize_key(key, normalized, sizeof(normalized), 0)) return 0;
    int wanted = wanted_variant == SR_ARCHIVE_VARIANT_AUTO ? -2 : wanted_variant;
    if (wanted < -2) return 0;
    const SrArchiveIndexEntry *chosen = NULL;
    size_t first = archive_index_lower_bound(vfs, normalized);
    for (size_t i = first; i < vfs->index_count; i++) {
        const SrArchiveIndexEntry *candidate = &vfs->index[i];
        if (strcmp(candidate->key, normalized) != 0) break;
        if (wanted >= 0) {
            if (candidate->variant == wanted) {
                chosen = candidate;
                break;
            }
            continue;
        }
        if (!chosen || candidate->variant == -1 ||
            (chosen->variant != -1 && candidate->variant < chosen->variant)) {
            chosen = candidate;
        }
    }
    return chosen && archive_file_from_index(vfs, chosen, file_out);
}

static uint64_t archive_next_use(SrArchiveVfs *vfs) {
    if (vfs->use_clock == UINT64_MAX) {
        for (size_t i = 0; i < vfs->cache_count; i++) vfs->cache[i].last_used = 0;
        vfs->use_clock = 0;
    }
    return ++vfs->use_clock;
}

static SrArchiveCacheEntry *archive_cache_find(SrArchiveVfs *vfs,
                                               size_t mount_index,
                                               size_t entry_index) {
    for (size_t i = 0; i < vfs->cache_count; i++) {
        SrArchiveCacheEntry *entry = &vfs->cache[i];
        if (entry->mount_index == mount_index && entry->entry_index == entry_index) {
            entry->last_used = archive_next_use(vfs);
            return entry;
        }
    }
    return NULL;
}

static void archive_cache_remove(SrArchiveVfs *vfs, size_t index) {
    if (!vfs || index >= vfs->cache_count) return;
    free(vfs->cache[index].data);
    vfs->cache_bytes -= vfs->cache[index].size;
    if (index + 1u < vfs->cache_count) {
        memmove(&vfs->cache[index], &vfs->cache[index + 1u],
                (vfs->cache_count - index - 1u) * sizeof(vfs->cache[0]));
    }
    vfs->cache_count--;
    memset(&vfs->cache[vfs->cache_count], 0, sizeof(vfs->cache[0]));
}

static int archive_cache_reserve(SrArchiveVfs *vfs, size_t wanted) {
    if (wanted <= vfs->cache_capacity) return 1;
    size_t next = vfs->cache_capacity ? vfs->cache_capacity : 8u;
    while (next < wanted) {
        if (next > SIZE_MAX / 2u) return 0;
        next *= 2u;
    }
    if (next > vfs->max_cache_entries) next = vfs->max_cache_entries;
    if (next < wanted || next > SIZE_MAX / sizeof(*vfs->cache)) return 0;
    SrArchiveCacheEntry *grown = (SrArchiveCacheEntry *)realloc(
        vfs->cache, next * sizeof(*grown));
    if (!grown) return 0;
    memset(grown + vfs->cache_capacity, 0,
           (next - vfs->cache_capacity) * sizeof(*grown));
    vfs->cache = grown;
    vfs->cache_capacity = next;
    return 1;
}

static void archive_cache_insert(SrArchiveVfs *vfs, size_t mount_index,
                                 size_t entry_index, uint8_t *data, size_t size) {
    if (!vfs || !data || size == 0u || vfs->max_cache_entries == 0u ||
        size > vfs->max_cache_bytes) {
        free(data);
        return;
    }
    for (size_t i = 0; i < vfs->cache_count;) {
        if (vfs->cache[i].mount_index == mount_index &&
            vfs->cache[i].entry_index == entry_index) {
            archive_cache_remove(vfs, i);
        } else {
            i++;
        }
    }
    while (vfs->cache_count > 0u &&
           (vfs->cache_count >= vfs->max_cache_entries ||
            size > vfs->max_cache_bytes - vfs->cache_bytes)) {
        size_t oldest = 0;
        for (size_t i = 1; i < vfs->cache_count; i++) {
            if (vfs->cache[i].last_used < vfs->cache[oldest].last_used) oldest = i;
        }
        archive_cache_remove(vfs, oldest);
    }
    if (!archive_cache_reserve(vfs, vfs->cache_count + 1u)) {
        free(data);
        return;
    }
    SrArchiveCacheEntry *entry = &vfs->cache[vfs->cache_count++];
    entry->mount_index = mount_index;
    entry->entry_index = entry_index;
    entry->data = data;
    entry->size = size;
    entry->last_used = archive_next_use(vfs);
    vfs->cache_bytes += size;
}

NkResult sr_archive_vfs_read(const SrArchiveVfs *vfs, const SrArchiveFile *file,
                             uint64_t offset, void *output, size_t output_capacity,
                             size_t *output_size) {
    if (output_size) *output_size = 0;
    if (!vfs || !file || file->mount_index >= vfs->mount_count) return NK_ERROR_INVALID_XB;
    const NkXbArchive *mount = &vfs->mounts[file->mount_index];
    if (file->entry_index >= mount->entry_count) return NK_ERROR_INVALID_XB;
    const NkXbEntry *entry = &mount->entries[file->entry_index];
    if (entry->expanded_size != file->size || file->size > SIZE_MAX) return NK_ERROR_INVALID_XB;
    uint64_t available = file->size > offset ? file->size - offset : 0;
    size_t amount = output_capacity;
    if ((uint64_t)amount > available) amount = (size_t)available;
    if (amount == 0u) return NK_OK;
    if (!output) return NK_ERROR_INVALID_XB;

    SrArchiveVfs *mutable_vfs = (SrArchiveVfs *)vfs;
    SrArchiveCacheEntry *cached = archive_cache_find(mutable_vfs, file->mount_index,
                                                     file->entry_index);
    if (cached) {
        if (offset > cached->size || amount > cached->size - (size_t)offset)
            return NK_ERROR_INVALID_XB;
        memcpy(output, cached->data + (size_t)offset, amount);
        if (output_size) *output_size = amount;
        return NK_OK;
    }

    if (entry->compression == NK_XB_COMPRESSION_NONE) {
        if (entry->offset > mount->data_size ||
            entry->expanded_size > mount->data_size - (size_t)entry->offset ||
            offset > entry->expanded_size ||
            amount > entry->expanded_size - (size_t)offset) return NK_ERROR_INVALID_XB;
        memcpy(output, mount->data + (size_t)entry->offset + (size_t)offset, amount);
        if (output_size) *output_size = amount;
        return NK_OK;
    }

    size_t expanded = (size_t)entry->expanded_size;
    uint8_t *decoded = (uint8_t *)malloc(expanded);
    if (!decoded) return NK_ERROR_OUT_OF_MEMORY;
    NkResult result = nk_xb_read_entry(mount, file->entry_index, decoded, expanded,
                                       NULL, NULL, 0);
    if (result != NK_OK) {
        free(decoded);
        return result;
    }
    if (offset > expanded || amount > expanded - (size_t)offset) {
        free(decoded);
        return NK_ERROR_INVALID_XB;
    }
    memcpy(output, decoded + (size_t)offset, amount);
    if (output_size) *output_size = amount;
    archive_cache_insert(mutable_vfs, file->mount_index, file->entry_index,
                         decoded, expanded);
    return NK_OK;
}

static size_t archive_directory_depth(const char *key) {
    if (!key || !key[0]) return 0;
    size_t depth = 1;
    for (const char *p = key; *p; p++) {
        if (*p == '/') depth++;
    }
    return depth;
}

static void archive_original_child(const char *path, size_t depth,
                                   const char *folded, size_t folded_length,
                                   char *out, size_t capacity) {
    out[0] = '\0';
    const char *p = path;
    for (size_t i = 0; i <= depth && p; i++) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t length = (size_t)(p - start);
        if (i == depth) {
            if (length > 0u && length < capacity) {
                memcpy(out, start, length);
                out[length] = '\0';
                return;
            }
            break;
        }
        if (*p == '/') p++;
    }
    if (folded_length > 0u && folded_length < capacity) {
        memcpy(out, folded, folded_length);
        out[folded_length] = '\0';
    }
}

int sr_archive_vfs_list_dir(const SrArchiveVfs *vfs, const char *dir_key,
                            int wanted_variant, SrVfsDirList *list) {
    if (!vfs || !list) return -1;
    char normalized[NK_XB_MAX_NAME_BYTES + 1u];
    if (!archive_normalize_directory(dir_key, normalized, sizeof(normalized))) return -1;
    int wanted = wanted_variant == SR_ARCHIVE_VARIANT_AUTO ? -2 : wanted_variant;
    if (wanted < -2) return -1;
    char prefix[NK_XB_MAX_NAME_BYTES + 2u];
    size_t prefix_length = 0;
    if (normalized[0]) {
        size_t length = strlen(normalized);
        if (length + 2u > sizeof(prefix)) return -1;
        memcpy(prefix, normalized, length);
        prefix[length] = '/';
        prefix[length + 1u] = '\0';
        prefix_length = length + 1u;
    } else {
        prefix[0] = '\0';
    }
    size_t first = archive_index_lower_bound(vfs, prefix);
    for (size_t i = first; i < vfs->index_count; i++) {
        const SrArchiveIndexEntry *entry = &vfs->index[i];
        if (prefix_length && strncmp(entry->key, prefix, prefix_length) != 0) break;
        const char *tail = entry->key + prefix_length;
        if (!tail[0]) continue;
        list->exists = 1;
        if (wanted >= 0 && entry->variant != wanted) continue;
        const char *slash = strchr(tail, '/');
        int is_dir = slash != NULL;
        size_t folded_length = is_dir ? (size_t)(slash - tail) : strlen(tail);
        if (!is_dir && entry->size > UINT32_MAX) {
            list->skipped++;
            continue;
        }
        char child[SR_VFS_NAME_MAX];
        archive_original_child(entry->path, archive_directory_depth(normalized),
                               tail, folded_length, child, sizeof(child));
        if (!child[0]) {
            list->skipped++;
            continue;
        }
        if (!sr_vfs_dirlist_merge(list, child, is_dir, is_dir ? 0u : entry->size))
            return -1;
    }
    return 1;
}

size_t sr_archive_vfs_mount_count(const SrArchiveVfs *vfs) {
    return vfs ? vfs->mount_count : 0u;
}

size_t sr_archive_vfs_entry_count(const SrArchiveVfs *vfs) {
    return vfs ? vfs->index_count : 0u;
}

size_t sr_archive_vfs_cache_bytes(const SrArchiveVfs *vfs) {
    return vfs ? vfs->cache_bytes : 0u;
}

size_t sr_archive_vfs_cache_entry_count(const SrArchiveVfs *vfs) {
    return vfs ? vfs->cache_count : 0u;
}
