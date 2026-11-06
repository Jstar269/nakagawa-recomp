// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Differential check of an HLE handler against the game's own disc-shipped module (#2148).
//
// Where a title ships a library on its disc and the runtime also has an HLE handler for the same
// NID, a started module's export runs (link_started_export in hle.c, LLE first: AGENTS.md
// section 6). With SR_HLE_DIFF=1 that call also runs the HLE handler, from the same CPU state
// and on a copy of guest memory, and compares the two: the return value and every byte of guest
// memory either one wrote. The guest export's run is the one the game keeps, so a difference
// never changes what the guest sees; it is evidence for fixing the handler or moving the call
// to LLE.
//
// Only NIDs with a pair-table row are compared, and only rows whose handler keeps no host-side
// state: running such a handler beside the export would change later host answers. Every other
// call is reported NOT_COMPARED with its reason and is never counted as a match. A match proves
// only the compared call with those arguments and that memory, nothing about other inputs.
//
// The comparator itself depends only on CpuState, the region list and the two callbacks, so a
// synthetic test can drive it directly, and another seam (the LLE import seam's COSIM mode,
// domain_mode.h) can call it the same way.

#ifndef SR_IMPORT_DIFF_H
#define SR_IMPORT_DIFF_H

#include <stddef.h>
#include <stdint.h>

struct CpuState;

/* Pair-table row flags. */
#define IMPORT_DIFF_PAIR_COMPARE    0x1u  /* the handler keeps no host state for this NID: compare */
#define IMPORT_DIFF_PAIR_HOST_STATE 0x2u  /* the handler keeps host state: never run it beside */
#define IMPORT_DIFF_RET_V1          0x4u  /* the result is 64-bit: compare v1 as well as v0 */
#define IMPORT_DIFF_RET_F0          0x8u  /* the result is a float: compare f0 as well as v0 */

typedef struct ImportDiffPair {
    const char *library;   /* the PSP library the export belongs to, e.g. "sceLibFont" */
    uint32_t nid;
    unsigned flags;
    const char *note;      /* why the row has its flags (the review that set them) */
} ImportDiffPair;

typedef enum ImportDiffVerdict {
    IMPORT_DIFF_MATCH = 0,
    IMPORT_DIFF_DIFFER = 1,
    IMPORT_DIFF_NOT_COMPARED = 2,
} ImportDiffVerdict;

/* One guest memory range the comparison covers, with the host bytes that hold it. */
typedef struct ImportDiffRegion {
    uint32_t guest_base;
    uint32_t size;
    uint8_t *host;
} ImportDiffRegion;

typedef struct ImportDiffResult {
    ImportDiffVerdict verdict;
    uint32_t nid;
    const char *name;        /* the HLE registration's name, or NULL */
    const char *library;     /* the pair row's library, or NULL with no row */
    uint32_t args[8];        /* a0-a3, then t0-t3 (PSP EABI arguments 5-8) */
    uint32_t guest_v0, hle_v0, guest_v1, hle_v1, guest_f0, hle_f0;
    int ret_differs;         /* 1 when a compared return word differs */
    int mem_differs;         /* 1 when a byte of a covered region differs */
    uint32_t first_addr;     /* the lowest differing guest address, when mem_differs */
    uint8_t guest_byte, hle_byte;
    uint32_t differing_bytes;
    const char *reason;      /* NOT_COMPARED: why; otherwise NULL */
} ImportDiffResult;

/* Runs the guest export on the live state: the call the game keeps. */
typedef void (*ImportDiffGuestRun)(struct CpuState *s, void *ctx);
typedef uint32_t (*ImportDiffHleFn)(struct CpuState *s);

/* 1 when SR_HLE_DIFF=1 and no comparison is in flight (a call made from inside one takes the
 * ordinary path). Read once per process; the selftest override below replaces it. */
int import_diff_active(void);

/* The pair row for `nid`, or NULL. */
const ImportDiffPair *import_diff_find_pair(uint32_t nid);

/* Called by sr_syscall on every import: while a comparison is in flight it counts the call,
 * because a nested syscall reaches host state the snapshot does not cover. */
void import_diff_note_syscall(void);

/* The whole check. Always runs the guest export once, on the live state, and leaves the live
 * state as that run left it. `hle` and `hle_refusal` come from the NID's HLE registration (NULL
 * and 0 when there is none). Guest addresses in [dead_lo, dead_hi) are left out of the memory
 * comparison: the stack below the caller's sp, which the export uses for its own frames and
 * which is dead once it returns. Returns the verdict, also stored in *out. */
ImportDiffVerdict import_diff_call(struct CpuState *s, uint32_t nid, const char *name, ImportDiffHleFn hle,
                             uint32_t hle_refusal, ImportDiffGuestRun run_guest, void *ctx,
                             uint32_t dead_lo, uint32_t dead_hi, ImportDiffResult *out);

/* One report line for a result, without a newline:
 *   HLE_DIFF MATCH nid=0x... name=... lib=... args=... v0=...
 *   HLE_DIFF DIFFER nid=... args=... v0 guest=... hle=... first=0x... guest=0x.. hle=0x.. bytes=N
 *   HLE_DIFF NOT_COMPARED nid=... reason=... */
void import_diff_format(const ImportDiffResult *r, char *buf, size_t size);

/* Prints a result to stderr: every DIFFER up to 32 per NID, and the first MATCH and the first
 * NOT_COMPARED of each NID, so a long run stays readable. The counts are kept either way. */
void import_diff_report(const ImportDiffResult *r);

/* Counters since the last reset, for the selftest and the exit summary. */
void import_diff_counts(uint32_t *match, uint32_t *differ, uint32_t *not_compared);

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only hooks: force the switch, replace the pair table and the covered regions, and
 * clear the counters. A NULL table restores the built-in one; NULL regions restore the default
 * guest RAM, VRAM and scratchpad ranges. */
void import_diff_test_set_enabled(int on);
void import_diff_test_set_pairs(const ImportDiffPair *pairs, unsigned count);
void import_diff_test_set_regions(const ImportDiffRegion *regions, unsigned count);
void import_diff_test_reset(void);
#endif

#endif /* SR_IMPORT_DIFF_H */
