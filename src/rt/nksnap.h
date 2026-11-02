// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * nksnap v1: one guest call's starting state (issue #1842).
 *
 * A snapshot holds the full CpuState, the entry PC and the guest memory blocks the call
 * can touch. src/rt/snapshot_runner.c runs it through the translated code and through the
 * reference interpreter in src/ref and compares the two results. Snapshots hold guest
 * memory, so they stay private like traces (AGENTS.md section 5).
 *
 * Layout, all integers little-endian:
 *   8 bytes   magic "NKSNAP01"
 *   u32       SR_CPUSTATE_ABI_VERSION
 *   u32       sizeof(CpuState)
 *   N bytes   the raw CpuState
 *   u32       entry PC
 *   u32       block count
 *   per block u32 guest address, u32 size, size bytes
 *
 * tools/nksnap.py reads and writes the same format with the same refusal names, and
 * tools/test_snapshot_format.py pins both.
 */
#ifndef NKSNAP_H
#define NKSNAP_H

#include "recomp.h"

#include <stddef.h>
#include <stdint.h>

#define NKSNAP_MAGIC "NKSNAP01"
#define NKSNAP_MAGIC_LEN 8u

/* Refusal reasons, in the order the loader checks them. */
typedef enum {
    NKSNAP_OK = 0,
    NKSNAP_BAD_MAGIC,           /* "bad-magic" */
    NKSNAP_ABI_VERSION,         /* "abi-version": not SR_CPUSTATE_ABI_VERSION */
    NKSNAP_CPUSTATE_SIZE,       /* "cpustate-size": not sizeof(CpuState) */
    NKSNAP_TRUNCATED,           /* "truncated": a field or block runs past the end */
    NKSNAP_BLOCK_SIZE,          /* "block-size": zero, or address + size past 4 GiB */
    NKSNAP_BLOCK_OVERLAP,       /* "block-overlap" */
    NKSNAP_BLOCK_OUTSIDE_GUEST, /* "block-outside-guest": not inside one modelled region */
    NKSNAP_TRAILING_BYTES,      /* "trailing-bytes": data after the last block */
    NKSNAP_IO_ERROR,            /* "io-error": the file could not be read or written */
    NKSNAP_NO_MEMORY,           /* "out-of-memory" */
} NkSnapStatus;

typedef struct {
    uint32_t addr;
    uint32_t size;
    const uint8_t *bytes;
} NkSnapBlock;

typedef struct {
    CpuState cpu;
    uint32_t entry_pc;
    uint32_t block_count;
    NkSnapBlock *blocks;   /* in file order; bytes point into storage */
    uint8_t *storage;      /* owned copy of the file */
} NkSnapshot;

const char *nksnap_status_name(NkSnapStatus status);

/* The guest region (scratchpad, VRAM or main RAM) a block lies wholly inside, by name,
 * or NULL when it is outside every region the runtime's arena models. Addresses are the
 * unmirrored physical form: a capture normalises mirrors before writing. */
const char *nksnap_guest_region(uint32_t addr, uint32_t size);

/* Parse a snapshot from memory. On success *out owns a copy of the bytes and must be
 * released with nksnap_free. On refusal *out is empty and detail (when non-NULL) names
 * the offending field or block. */
NkSnapStatus nksnap_parse(const uint8_t *data, size_t len, NkSnapshot *out,
                          char *detail, size_t detail_cap);
NkSnapStatus nksnap_load_file(const char *path, NkSnapshot *out,
                              char *detail, size_t detail_cap);
void nksnap_free(NkSnapshot *snapshot);

/* Write a snapshot, refusing the same block forms the loader refuses. */
NkSnapStatus nksnap_write_file(const char *path, const CpuState *cpu, uint32_t entry_pc,
                               const NkSnapBlock *blocks, uint32_t block_count,
                               char *detail, size_t detail_cap);

#endif /* NKSNAP_H */
