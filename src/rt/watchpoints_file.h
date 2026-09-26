// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// watchpoints_file.h — bounded parser for the derived `watchpoints.json`
// runtime artifact (issue #188).
//
// The versioned envelope is the canonical persisted watchpoint format; this
// parser is its runtime consumer. It accepts:
//
//   1. the title-neutral versioned envelope:
//        { "format": "nk-watchpoints", "version": 1, ..., "watchpoints": [...] }
//      where format/version MUST match and every other field is ignored;
//   2. the legacy title-branded envelope:
//        { "format": "hst-watchpoints", "version": 1, ..., "watchpoints": [...] }
//      accepted for read-only backward compatibility (retirement policy: legacy
//      read-only; removal is tracked in #369);
//   3. a legacy bare array: [ {"start":N,"end":N,"label":"..."}, ... ].
//      accepted for read-only backward compatibility (retirement policy: legacy
//      read-only; removal is tracked in #369).
//
// Writers must only emit the canonical "nk-watchpoints" format (version 1)
// and never emit the legacy name or bare-array form.
//
// Every watchpoint must satisfy the runtime matching contract
// (guest_addr >= start && guest_addr < end) with 0 <= start < end <= UINT32_MAX,
// a bounded span, a bounded canonical label (charset [A-Za-z0-9_. -], <= 64
// bytes) and no exact (start,end) duplicates. At most SR_MAX_MEM_WATCHES (16)
// entries are accepted; more is an error, never silent truncation.
//
// The parser is intentionally fail-closed: any structural or semantic
// violation rejects the whole file instead of partially applying it.

#ifndef SR_WATCHPOINTS_FILE_H
#define SR_WATCHPOINTS_FILE_H

#include <stddef.h>
#include <stdint.h>

#define SR_WATCHPOINTS_FILE_FORMAT "nk-watchpoints"
#define SR_WATCHPOINTS_LEGACY_FILE_FORMAT "hst-watchpoints"
#define SR_WATCHPOINTS_FILE_VERSION 1
#define SR_WATCHPOINTS_FILE_MAX_BYTES (64u * 1024u)
#define SR_WATCHPOINT_MAX_SPAN (1u << 24)
#define SR_WATCHPOINT_LABEL_MAX 64u /* max label characters (ASCII charset) */
#define SR_WATCHPOINT_LABEL_BUF (SR_WATCHPOINT_LABEL_MAX + 1u) /* + NUL */

typedef struct SrWatchpointEntry {
    uint32_t start;
    uint32_t end;
    char label[SR_WATCHPOINT_LABEL_BUF]; /* NUL-terminated, canonical charset */
} SrWatchpointEntry;

/* Parse watchpoints from a JSON buffer. The input MUST be NUL-terminated
 * (the file reader guarantees this; string literals in tests are too).
 * `out_cap` is the capacity of `out` (pass SR_MAX_MEM_WATCHES from debug.h
 * in production). Returns the number of watchpoints parsed (>= 0), or -1 with
 * a NUL-terminated message in `errbuf` (errbuf_size >= 1) on any error.
 * Never partially fills `out` on failure. */
int sr_parse_watchpoints_buffer(const char *json, SrWatchpointEntry *out, int out_cap,
                                char *errbuf, size_t errbuf_size);

/* Read + parse the artifact at `path`. Same contract as the buffer variant. */
int sr_parse_watchpoints_file(const char *path, SrWatchpointEntry *out, int out_cap,
                              char *errbuf, size_t errbuf_size);

#endif /* SR_WATCHPOINTS_FILE_H */
