/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_SHARED_HELPERS_H
#define NK_SHARED_HELPERS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Shared file-open primitive also declared by nk_platform.h for platform callers. */
FILE *nk_fopen_utf8(const char *path, const char *mode);

typedef enum NkSharedPathJoinMode {
    NK_SHARED_PATH_JOIN_ALWAYS,
    NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR,
    NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY
} NkSharedPathJoinMode;

enum {
    NK_SHARED_PATH_TRIM_LEFT_SEPARATORS = 1u << 0,
    NK_SHARED_PATH_NORMALIZE_FORWARD_SLASHES = 1u << 1,
    NK_SHARED_PATH_NORMALIZE_BOTH_SLASHES = 1u << 2,
    NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT = 1u << 3,
    NK_SHARED_PATH_REQUIRE_FIT = 1u << 4,
    NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH = 1u << 5
};

/* On ordinary overflow, like snprintf, writes a truncated NUL-terminated prefix.
 * REQUIRE_FIT leaves out untouched when the result does not fit. */
bool nk_shared_path_join(char *out, size_t out_capacity,
                         const char *left, const char *right, char separator,
                         NkSharedPathJoinMode mode, unsigned int flags);

/* Allocates the complete joined path, checking size arithmetic before allocation. */
char *nk_shared_path_join_alloc(const char *left, const char *right, char separator,
                                NkSharedPathJoinMode mode, unsigned int flags);

typedef enum NkSharedTextCopyMode {
    NK_SHARED_TEXT_COPY_TRUNCATE,
    NK_SHARED_TEXT_COPY_REQUIRE_FIT
} NkSharedTextCopyMode;

/* Truncating copies preserve strncpy padding; strict copies leave out untouched on failure. */
bool nk_shared_text_copy(char *out, size_t out_capacity, const char *text,
                         NkSharedTextCopyMode mode);

void nk_shared_format_size(char *out, size_t out_capacity, uint64_t bytes);

/* Writes the same formatted line to stderr_stream and, when path is nonempty, to an
 * append-binary event file. The caller supplies one timestamp for both records. */
void nk_shared_boot_event_vwrite(FILE *stderr_stream, uint64_t timestamp_ns,
                                 const char *format,
                                 va_list args);

/* Formats the player's process/sequence-specific event pathname. Failure clears out. */
int nk_shared_boot_event_format_path(char *out, size_t out_capacity,
                                    const char *cache_dir, char separator,
                                    unsigned long process_id, unsigned int sequence);

#endif
