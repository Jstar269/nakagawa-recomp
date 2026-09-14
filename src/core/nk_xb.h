/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_XB_H
#define NK_XB_H

#include "nk_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The archive format stores the signature as these four bytes, independent of
 * the byte order used by the integer fields that follow it. */
#define NK_XB_SIGNATURE_0 0x78u
#define NK_XB_SIGNATURE_1 0x65u
#define NK_XB_SIGNATURE_2 0x00u
#define NK_XB_SIGNATURE_3 0x01u

#define NK_XB_MAX_ARCHIVE_BYTES (512u * 1024u * 1024u)
#define NK_XB_MAX_FILES 100000u
#define NK_XB_MAX_NAME_BYTES 255u
#define NK_XB_MAX_STRING_TABLE_BYTES (16u * 1024u * 1024u)
#define NK_XB_MAX_ENTRY_BYTES (256u * 1024u * 1024u)
#define NK_XB_MAX_TOTAL_EXPANDED_BYTES (512u * 1024u * 1024u)
#define NK_XB_MAX_HUFFMAN_DEPTH 16u
#define NK_XB_MAX_HUFFMAN_CODES 4096u

typedef enum {
    NK_XB_COMPRESSION_DEFLATE = 0,
    NK_XB_COMPRESSION_HUFFMAN = 1,
    NK_XB_COMPRESSION_LZS = 2,
    NK_XB_COMPRESSION_NONE = 3
} NkXbCompression;

typedef struct {
    size_t max_archive_bytes;
    size_t max_files;
    size_t max_name_bytes;
    size_t max_string_table_bytes;
    size_t max_entry_bytes;
    size_t max_total_expanded_bytes;
    unsigned max_huffman_depth;
    unsigned max_huffman_codes;
} NkXbLimits;

typedef struct {
    size_t index;
    char path[NK_XB_MAX_NAME_BYTES + 1];
    uint64_t offset;
    uint64_t expanded_size;
    NkXbCompression compression;
    uint64_t stored_size;
    uint64_t span_size;
} NkXbEntry;

typedef struct {
    const uint8_t *data;
    size_t data_size;
    bool owns_data;
    NkXbEntry *entries;
    size_t entry_count;
    size_t data_start;
    char source_name[NK_MAX_PATH];
    NkXbLimits limits;
    bool big_endian;
} NkXbArchive;

/* Return the strict default resource budget used by the native reader. */
NkXbLimits nk_xb_default_limits(void);

/* Open an archive from a file or from a copied in-memory buffer. The parser
 * validates every FST row, string-table name, data span, and compressed header
 * before returning NK_OK. */
NkResult nk_xb_open_file(const char *path, bool big_endian,
                         const NkXbLimits *limits, NkXbArchive *out_archive,
                         char *error_message, size_t error_message_size);
NkResult nk_xb_open_memory(const void *data, size_t data_size,
                           const char *source_name, bool big_endian,
                           const NkXbLimits *limits, NkXbArchive *out_archive,
                           char *error_message, size_t error_message_size);
void nk_xb_close(NkXbArchive *archive);

/* Read one already-validated entry into a caller-owned buffer. The destination
 * must be large enough for entry->expanded_size; no output allocation is
 * performed by this API. */
NkResult nk_xb_read_entry(const NkXbArchive *archive, size_t entry_index,
                          void *output, size_t output_capacity,
                          size_t *output_size, char *error_message,
                          size_t error_message_size);

typedef bool (*NkXbProgressCallback)(const NkXbEntry *entry,
                                     size_t completed_entries,
                                     size_t total_entries,
                                     void *userdata);

/* Decode every member in one archive into destination_root. Member paths are
 * normalized by the parser and are never re-derived by the writer. The caller
 * owns transaction-level staging; this function never follows archive paths
 * outside destination_root. */
NkResult nk_xb_unpack(const char *archive_path, const char *destination_root,
                      NkXbProgressCallback progress, void *userdata,
                      char *error_message, size_t error_message_size);

#ifdef __cplusplus
}
#endif

#endif /* NK_XB_H */
