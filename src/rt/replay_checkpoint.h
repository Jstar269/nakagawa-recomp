// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Replay checkpoints (#2147): SHA-256 digests of guest-visible state at one frame, so two
 * runs can be compared state by state and the first frame and subsystem where they split
 * can be named. A checkpoint holds digests only, never guest bytes.
 *
 * A checkpoint is a list of named digests in a fixed order:
 *   cpu.<uid>        one thread's CpuState (32-bit words, little-endian)
 *   <prefix>.<addr>  one chunk of a guest memory region, named by its guest address
 *   <name>           any other subsystem's own serialized state (sched, audio, ge, ...)
 *
 * One line of text carries one checkpoint, so a run that stops early still leaves every
 * checkpoint it reached, and tools/nkreplay.py reads the same lines:
 *
 *   NKCKPT v=1 frame=<decimal> n=<count> <name>=<64 hex> ...
 *
 * Names are 1..23 characters of [A-Za-z0-9._-].
 */
#ifndef REPLAY_CHECKPOINT_H
#define REPLAY_CHECKPOINT_H

#include "recomp.h"

#include <stddef.h>
#include <stdint.h>

#define NK_CKPT_NAME_LEN 24u
#define NK_CKPT_MAX_DIGESTS 1024u
#define NK_CKPT_LINE_PREFIX "NKCKPT v=1 "

typedef struct {
    char name[NK_CKPT_NAME_LEN];
    uint8_t sha256[32];
} NkCkptDigest;

typedef struct {
    uint64_t frame;
    uint32_t count;
    int overflow; /* a digest did not fit or had a bad name: the checkpoint is unusable */
    NkCkptDigest digests[NK_CKPT_MAX_DIGESTS];
} NkCheckpoint;

void nk_ckpt_begin(NkCheckpoint *ckpt, uint64_t frame);
/* Each returns 1, or 0 (and sets overflow) when the name is invalid or the list is full. */
int nk_ckpt_add(NkCheckpoint *ckpt, const char *name, const void *bytes, size_t len);
int nk_ckpt_add_cpu(NkCheckpoint *ckpt, uint32_t uid, const CpuState *cpu);
int nk_ckpt_add_chunks(NkCheckpoint *ckpt, const char *prefix, uint32_t guest_addr,
                       const uint8_t *host, size_t size, size_t chunk);

/* Write one line (with its newline) into out; returns the length, or 0 when it does not fit
 * or the checkpoint overflowed. */
size_t nk_ckpt_format(const NkCheckpoint *ckpt, char *out, size_t cap);
/* Parse one line (a trailing newline is allowed). Returns 1, or 0 with a reason in detail. */
int nk_ckpt_parse(const char *line, NkCheckpoint *out, char *detail, size_t detail_cap);

typedef enum {
    NK_CKPT_SAME = 0,
    NK_CKPT_FRAME,  /* the checkpoints are for different frames */
    NK_CKPT_NAMES,  /* the digest lists differ (a thread or region appeared or went) */
    NK_CKPT_DIGEST, /* one named digest differs */
} NkCkptDiff;

typedef struct {
    NkCkptDiff kind;
    uint64_t frame;              /* the expected checkpoint's frame */
    char name[NK_CKPT_NAME_LEN]; /* the first differing digest (NAMES: the expected name, or "") */
} NkCkptMismatch;

/* The first difference between an expected and an actual checkpoint, in the expected
 * checkpoint's digest order. */
NkCkptDiff nk_ckpt_compare(const NkCheckpoint *expected, const NkCheckpoint *actual, NkCkptMismatch *out);

const char *nk_ckpt_diff_name(NkCkptDiff diff);

/* ---- the runtime's checkpoint stream ----------------------------------------------------
 *
 * SR_REPLAY_CHECKPOINTS=<file> writes a checkpoint at the first VBLANK of every
 * SR_REPLAY_CHECKPOINT_EVERY vblanks (default 60). The file starts with one line naming the
 * run's settings,
 *
 *   NKCKPT_RUN v=1 every=<n> clock=<deterministic|host>
 *
 * then one NKCKPT line per checkpoint, flushed as it is written. Each checkpoint holds:
 *   cpu.<uid>       every live guest thread's CpuState (the running thread's live registers)
 *   sched           each thread's uid, state, priority and wait object, and the guest clock
 *   vram.<addr>     VRAM, 1 MiB chunks
 *   ram.<addr>      main RAM, 1 MiB chunks
 * Reading the state changes nothing the guest can see. Off (unset), the VBLANK hook returns
 * at once. Two runs are compared with `python tools/nkstate.py ckpt-diff A B`. */

/* Called by sr_vblank_tick (hle_display.c) once per delivered VBLANK. */
void sr_replay_checkpoint_vblank(uint32_t vcount);

/* Implemented in sched.c: visit every live guest thread, read-only. state is the scheduler's
 * TH_* value; cpu is the thread's saved registers, or the live ones for the running thread. */
typedef void (*SrSchedThreadVisit)(void *ctx, uint32_t uid, int state, int priority,
                                   uint32_t wait_obj, const CpuState *cpu);
void sched_for_each_thread_cpu(SrSchedThreadVisit visit, void *ctx);

#endif /* REPLAY_CHECKPOINT_H */
