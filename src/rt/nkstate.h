// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * nkstate v1: the container for save states (#2146) and replay recordings (#2147).
 *
 * A file binds itself to the build that wrote it and holds one segment per subsystem. Each
 * subsystem owns its segment: it exports a save and a load function and carries its own
 * version. Nothing is best effort:
 *
 *   - a capture that requires a subsystem with no exporter is refused and names it;
 *   - an exporter that is not at a quiet point refuses the capture and says why;
 *   - a load checks every identity field, every segment hash and the file hash before any
 *     loader runs, then checks that each loaded subsystem saves back to the exact bytes it
 *     was given, so a loader that drops a field is caught before the guest runs.
 *
 * A state holds guest memory and a recording holds a private run's input timing, so both
 * stay in the user's private folder like saves (AGENTS.md section 5). docs/DEBUGGING.md
 * describes what each holds.
 *
 * Layout, all integers little-endian:
 *   8 bytes  magic "NKSTATE1"
 *   u32      format version (NKSTATE_FORMAT_VERSION)
 *   u32      kind (1 recording, 2 state)
 *   u32      SR_CPUSTATE_ABI_VERSION
 *   u32      sizeof(CpuState)
 *   32 bytes executable identity (SHA-256 of the generated code)
 *   64 bytes runtime identity (SR_BUILD_ID, the source commit), NUL padded
 *   16 bytes disc ID, NUL padded
 *   u32      flags (bit 0: written under the deterministic clock)
 *   u32      segment count
 *   per segment: 4-byte tag, u32 version, u64 length, 32-byte SHA-256 of the payload
 *   the payloads, in table order
 *   32 bytes SHA-256 of every byte above
 *
 * tools/nkstate.py reads and writes the same format with the same refusal names, and
 * tools/test_nkstate.py pins both to one golden file digest.
 */
#ifndef NKSTATE_H
#define NKSTATE_H

#include "recomp.h"

#include <stddef.h>
#include <stdint.h>

#define NKSTATE_MAGIC "NKSTATE1"
#define NKSTATE_MAGIC_LEN 8u
#define NKSTATE_FORMAT_VERSION 1u
#define NKSTATE_RUNTIME_ID_LEN 64u
#define NKSTATE_DISC_ID_LEN 16u
#define NKSTATE_TAG_LEN 4u
#define NKSTATE_MAX_SEGMENTS 64u
#define NKSTATE_HEADER_SIZE (8u + 4u * 4u + 32u + NKSTATE_RUNTIME_ID_LEN + NKSTATE_DISC_ID_LEN + 4u + 4u)
#define NKSTATE_TABLE_ENTRY_SIZE (4u + 4u + 8u + 32u)

#define NKSTATE_KIND_RECORDING 1u
#define NKSTATE_KIND_STATE 2u

#define NKSTATE_FLAG_DETERMINISTIC_CLOCK 0x1u
#define NKSTATE_FLAGS_KNOWN NKSTATE_FLAG_DETERMINISTIC_CLOCK

/* Refusal reasons. The loader checks them in this order; nkstate_status_name gives the
 * stable name tools/nkstate.py also uses. */
typedef enum {
    NKSTATE_OK = 0,
    NKSTATE_BAD_MAGIC,               /* "bad-magic" */
    NKSTATE_FORMAT_VERSION_MISMATCH, /* "format-version" */
    NKSTATE_KIND_MISMATCH,           /* "kind" */
    NKSTATE_ABI_VERSION,             /* "abi-version": not SR_CPUSTATE_ABI_VERSION */
    NKSTATE_CPUSTATE_SIZE,           /* "cpustate-size": not sizeof(CpuState) */
    NKSTATE_EXECUTABLE_IDENTITY,     /* "executable-identity" */
    NKSTATE_RUNTIME_IDENTITY,        /* "runtime-identity" */
    NKSTATE_DISC_ID,                 /* "disc-id" */
    NKSTATE_FLAGS,                   /* "flags": a bit this build does not know */
    NKSTATE_CLOCK_NOT_DETERMINISTIC, /* "clock-not-deterministic" */
    NKSTATE_TRUNCATED,               /* "truncated" */
    NKSTATE_TOO_MANY_SEGMENTS,       /* "too-many-segments" */
    NKSTATE_BAD_TAG,                 /* "bad-tag": not four printable ASCII bytes */
    NKSTATE_DUPLICATE_SEGMENT,       /* "duplicate-segment" */
    NKSTATE_UNKNOWN_SEGMENT,         /* "unknown-segment": this build has no loader for it */
    NKSTATE_MISSING_SEGMENT,         /* "missing-segment": a subsystem this build requires */
    NKSTATE_SEGMENT_VERSION,         /* "segment-version" */
    NKSTATE_TRAILING_BYTES,          /* "trailing-bytes" */
    NKSTATE_SEGMENT_HASH,            /* "segment-hash": a payload was changed */
    NKSTATE_FILE_HASH,               /* "file-hash": the header or table was changed */
    NKSTATE_SEGMENT_PAYLOAD,         /* "segment-payload": a loader refused its payload */
    NKSTATE_RESTORE_VERIFY,          /* "restore-verify": a loaded subsystem saves back differently */
    NKSTATE_NO_RUNTIME_IDENTITY,     /* "no-runtime-identity": an ad-hoc build writes nothing */
    NKSTATE_MISSING_EXPORTER,        /* "missing-exporter": a required subsystem cannot save */
    NKSTATE_NOT_QUIET,               /* "not-quiet": a subsystem is mid-operation */
    NKSTATE_IO_ERROR,                /* "io-error" */
    NKSTATE_NO_MEMORY,               /* "out-of-memory" */
} NkStateStatus;

const char *nkstate_status_name(NkStateStatus status);

/* What the file is bound to. Both sides of a load fill one in: the file's, and this
 * process's own. Strings are NUL padded to their fixed width. */
typedef struct {
    uint32_t kind;
    uint8_t executable[32];
    char runtime[NKSTATE_RUNTIME_ID_LEN];
    char disc_id[NKSTATE_DISC_ID_LEN];
    uint32_t flags;
} NkStateIdentity;

/* A growable byte buffer the save functions append to. */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} NkStateBuf;

int nkstate_buf_put(NkStateBuf *buf, const void *bytes, size_t len); /* 0 when out of memory */
int nkstate_buf_put_u32(NkStateBuf *buf, uint32_t value);
int nkstate_buf_put_u64(NkStateBuf *buf, uint64_t value);
void nkstate_buf_free(NkStateBuf *buf);

typedef NkStateStatus (*NkStateSaveFn)(void *ctx, NkStateBuf *out, char *detail, size_t detail_cap);
typedef NkStateStatus (*NkStateLoadFn)(void *ctx, const uint8_t *payload, size_t len,
                                       char *detail, size_t detail_cap);

/* One subsystem's segment. tag is four printable ASCII bytes plus a NUL. */
typedef struct {
    char tag[NKSTATE_TAG_LEN + 1u];
    const char *name;
    uint32_t version;
    NkStateSaveFn save;
    NkStateLoadFn load;
    void *ctx;
} NkStateExporter;

/* A subsystem a capture cannot be complete without. */
typedef struct {
    char tag[NKSTATE_TAG_LEN + 1u];
    const char *name;
} NkStateRequirement;

typedef struct {
    char tag[NKSTATE_TAG_LEN + 1u];
    uint32_t version;
    uint64_t length;
    uint8_t sha256[32];
    const uint8_t *payload; /* points into NkStateFile.storage */
} NkStateSegment;

typedef struct {
    NkStateIdentity identity;
    uint32_t abi_version;
    uint32_t cpustate_size;
    uint32_t segment_count;
    NkStateSegment segments[NKSTATE_MAX_SEGMENTS];
    uint8_t *storage; /* owned copy of the file */
} NkStateFile;

/* What a load accepts: this process's identity and the segments it can load. */
typedef struct {
    const NkStateIdentity *self;
    int require_deterministic_clock;
    const NkStateExporter *exporters;
    size_t exporter_count;
} NkStateExpect;

/* Write every required subsystem, then every other exporter, into one file image in out
 * (caller frees with nkstate_buf_free). Refuses before any save runs when a requirement
 * has no exporter with both functions, or when self has no runtime identity. */
NkStateStatus nkstate_capture(const NkStateIdentity *self,
                              const NkStateRequirement *required, size_t required_count,
                              const NkStateExporter *exporters, size_t exporter_count,
                              NkStateBuf *out, char *detail, size_t detail_cap);

/* Parse and check a file image without loading anything. On success *out owns a copy. */
NkStateStatus nkstate_parse(const uint8_t *data, size_t len, const NkStateExpect *expect,
                            NkStateFile *out, char *detail, size_t detail_cap);
void nkstate_file_free(NkStateFile *file);

/* Parse and check the whole file, then run each exporter's load in table order, then
 * check that each one saves back to its segment's exact bytes. Any refusal before the
 * first load leaves the target untouched. A refusal after it (segment-payload,
 * restore-verify) means the target holds a partial state: the caller must not run the
 * guest from it. */
NkStateStatus nkstate_restore(const uint8_t *data, size_t len, const NkStateExpect *expect,
                              char *detail, size_t detail_cap);

NkStateStatus nkstate_write_file(const char *path, const NkStateBuf *image,
                                 char *detail, size_t detail_cap);
NkStateStatus nkstate_read_file(const char *path, NkStateBuf *out, char *detail, size_t detail_cap);

/* Segment helpers for the two subsystems every state holds. */

/* "CPU ": the CpuState of each guest thread, keyed by thread uid. */
typedef struct {
    uint32_t uid;
    CpuState *cpu;
} NkStateThread;

NkStateStatus nkstate_cpu_save(const NkStateThread *threads, uint32_t count, NkStateBuf *out,
                               char *detail, size_t detail_cap);
/* The payload must name exactly the given uids, in the same order. */
NkStateStatus nkstate_cpu_load(const NkStateThread *threads, uint32_t count,
                               const uint8_t *payload, size_t len, char *detail, size_t detail_cap);

/* "MEM ": guest memory regions, each a guest address, size and the host bytes behind it. */
typedef struct {
    uint32_t addr;
    uint32_t size;
    uint8_t *host;
} NkStateMemRegion;

NkStateStatus nkstate_mem_save(const NkStateMemRegion *regions, uint32_t count, NkStateBuf *out,
                               char *detail, size_t detail_cap);
/* The payload must name exactly the given regions (address and size), in the same order. */
NkStateStatus nkstate_mem_load(const NkStateMemRegion *regions, uint32_t count,
                               const uint8_t *payload, size_t len, char *detail, size_t detail_cap);

#endif /* NKSTATE_H */
