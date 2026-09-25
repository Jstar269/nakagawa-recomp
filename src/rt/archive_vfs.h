/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef SR_ARCHIVE_VFS_H
#define SR_ARCHIVE_VFS_H

#include "asset_index.h"
#include "nk_xb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_ARCHIVE_VFS_DEFAULT_CACHE_BYTES (8u * 1024u * 1024u)
#define SR_ARCHIVE_VFS_DEFAULT_CACHE_ENTRIES 32u
#define SR_ARCHIVE_VARIANT_AUTO (-2147483647 - 1)

typedef struct {
    size_t mount_index;
    size_t entry_index;
    uint64_t size;
    int variant;
} SrArchiveFile;

typedef struct {
    size_t mount_index;
    size_t entry_index;
    uint8_t *data;
    size_t size;
    uint64_t last_used;
} SrArchiveCacheEntry;

typedef struct {
    char *key;
    char *path;
    size_t mount_index;
    size_t entry_index;
    uint64_t size;
    int variant;
    size_t order;
} SrArchiveIndexEntry;

typedef struct {
    NkXbArchive *mounts;
    size_t mount_count;
    size_t mount_capacity;
    SrArchiveIndexEntry *index;
    size_t index_count;
    size_t index_capacity;
    size_t index_sort_count;
    int index_dirty;
    SrArchiveCacheEntry *cache;
    size_t cache_count;
    size_t cache_capacity;
    size_t cache_bytes;
    size_t max_cache_bytes;
    size_t max_cache_entries;
    uint64_t use_clock;
} SrArchiveVfs;

void sr_archive_vfs_init(SrArchiveVfs *vfs);
void sr_archive_vfs_destroy(SrArchiveVfs *vfs);
int sr_archive_vfs_configure(SrArchiveVfs *vfs, size_t max_cache_bytes,
                             size_t max_cache_entries);
int sr_archive_variant_from_name(const char *name, int *variant_out);

NkResult sr_archive_vfs_mount_file(SrArchiveVfs *vfs, const char *path,
                                   bool big_endian, int variant,
                                   const NkXbLimits *limits);
NkResult sr_archive_vfs_mount_memory(SrArchiveVfs *vfs, const void *data,
                                     size_t data_size, const char *source_name,
                                     bool big_endian, int variant,
                                     const NkXbLimits *limits);
int sr_archive_vfs_finalize(SrArchiveVfs *vfs);

int sr_archive_vfs_lookup(const SrArchiveVfs *vfs, const char *key,
                          int wanted_variant, SrArchiveFile *file_out);
NkResult sr_archive_vfs_read(const SrArchiveVfs *vfs, const SrArchiveFile *file,
                             uint64_t offset, void *output, size_t output_capacity,
                             size_t *output_size);
int sr_archive_vfs_list_dir(const SrArchiveVfs *vfs, const char *dir_key,
                            int wanted_variant, SrVfsDirList *list);

size_t sr_archive_vfs_mount_count(const SrArchiveVfs *vfs);
size_t sr_archive_vfs_entry_count(const SrArchiveVfs *vfs);
size_t sr_archive_vfs_index_sort_count(const SrArchiveVfs *vfs);
size_t sr_archive_vfs_cache_bytes(const SrArchiveVfs *vfs);
size_t sr_archive_vfs_cache_entry_count(const SrArchiveVfs *vfs);

#ifdef __cplusplus
}
#endif

#endif
