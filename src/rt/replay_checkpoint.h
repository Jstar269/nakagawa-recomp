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
 * checkpoint it reached, and tools/nkstate.py reads the same lines:
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
#include <wchar.h>

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

/* ---- recording and replay (SR_RECORD / SR_REPLAY) --------------------------------------
 *
 * A recording is a run's own checkpoints plus every host answer the guest received that
 * guest state does not determine. A replay is a fresh run of the same executable that is
 * served those answers and checks each checkpoint as it is reached. Both need
 * SR_DETERMINISTIC_CLOCK=1: with it the guest clock and the RTC are functions of guest time,
 * so the on-screen-keyboard text is the only host answer left to record. The replay is live
 * from the moment the recording is used up, so phase 1 of #2146 loads a state this way.
 *
 * SR_RECORD=<file> writes, each line flushed as it is written:
 *
 *   NKREC v=1 every=<n> clock=deterministic exe=<64 hex> disc=<id|-> abi=<n> cpustate=<n>
 *   NKANSW v=1 call=osk request=<k> pending=<p> result=<ok|cancel|abandoned|open> units=<hex|->
 *   NKCKPT v=1 frame=<f> ...                  (every SR_REPLAY_CHECKPOINT_EVERY vblanks)
 *   NKREC_END frame=<last vblank>             (at a normal exit)
 *
 * exe= is the SHA-256 over the executable's size and the SHA-256 of each 1 MiB block of it.
 * A keyboard request is answered after `pending` polls that reported it still open; ok
 * carries the text as 8 hex digits per wchar_t unit. A request still open at a checkpoint
 * is written as `open` so far, and a later line for it supersedes that one. A run ended by
 * _Exit has no NKREC_END line and replays to its last checkpoint.
 *
 * SR_REPLAY=<file> checks the header first (refusals use the nkstate names:
 * executable-identity, disc-id, abi-version, cpustate-size, clock-not-deterministic, and
 * malformed for a line it cannot read) and then writes exactly one verdict to stderr:
 *
 *   REPLAY_REFUSED reason=<name> line=<n>             exit 1, before the guest runs
 *   REPLAY_MISMATCH frame=<f> index=<i> kind=<k> digest=<name>
 *                                                     exit 1, at the first differing checkpoint
 *   REPLAY_MISSING_ANSWER call=osk request=<k> frame=<f> detail=<why>
 *                                                     exit 1: the guest asked for an answer
 *                                                     the recording does not hold; nothing
 *                                                     is invented
 *   REPLAY_COMPLETE frame=<f> checkpoints=<n> answers=<k>
 *                                                     the run continues live
 *   REPLAY_NOT_RUN reached=<f> end=<f> checkpoints=<i>/<n>
 *                                                     at exit, when the run stopped first;
 *                                                     never a pass
 *
 * Input from the pad is not recorded yet: a replay of a run that read live pad input stops
 * at a REPLAY_MISMATCH. SR_PADSCRIPT and SR_OSK_SCRIPT runs replay when they are given the
 * same script. */

#define SR_REPLAY_OSK_UNITS 256 /* the HLE keyboard's field bound, terminator included */

/* The keyboard's entry points, sr_replay_osk_poll and sr_replay_osk_abandon, are declared in
 * osk_text_entry.h, which the keyboard includes without the guest-memory headers. */

/* Implemented in sched.c: visit every live guest thread, read-only. state is the scheduler's
 * TH_* value; cpu is the thread's saved registers, or the live ones for the running thread. */
typedef void (*SrSchedThreadVisit)(void *ctx, uint32_t uid, int state, int priority,
                                   uint32_t wait_obj, const CpuState *cpu);
void sched_for_each_thread_cpu(SrSchedThreadVisit visit, void *ctx);

#endif /* REPLAY_CHECKPOINT_H */
