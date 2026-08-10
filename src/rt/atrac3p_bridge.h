// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * atrac3p_bridge.h — Nakagawa-authored PSP ATRAC3+ decode bridge (PR-B).
 *
 * Owns one standalone ATRAC3+ decoder instance (src/rt/atrac3p/, PR-A) and
 * exposes the host-side decode contract the HLE ring model (src/rt/hle.c,
 * #283) feeds: one blockAlign-sized frame in, interleaved s16 PCM out with
 * the nb_samples=2048 PSP frame contract.
 *
 * This file is Nakagawa-authored and is not a PPSSPP code translation.
 * It is the thin integration layer over the project's own FFmpeg n4.4
 * import; PPSSPP was consulted only as historical/comparative provenance
 * for the #286 decision record. The decoder itself is byte-exact FFmpeg
 * n4.4 (LGPL-2.1-or-later, see src/rt/atrac3p/PROVENANCE.md).
 *
 * Failure contract (no fake PCM): every decode either succeeds and reports
 * ATRAC3P_FRAME_SAMPLES samples, or returns a negative AVERROR_* code with
 * *samples_out left at 0. A caller can never mistake a failed frame for a
 * complete one, and no silent-success path exists.
 *
 * Thread-safety: distinct instances are independent; a single instance is
 * single-threaded by contract (same as the PR-A API).
 */

#ifndef SR_ATRAC3P_BRIDGE_H
#define SR_ATRAC3P_BRIDGE_H

#include <stdint.h>

#include "atrac3p/atrac3p_api.h"

/* Opaque bridge instance. NULL is never a valid instance. */
typedef struct Atrac3pBridge Atrac3pBridge;

/*
 * Create a decoder instance for the PSP stream configuration.
 *
 *   channels    - 1, 2, 3, 4, 6, 7 or 8 (PSP ATRAC3+ streams)
 *   block_align - positive frame size in bytes (RIFF fmt blockAlign)
 *   out         - receives the instance, or NULL on failure
 *
 * Returns 0 on success, a negative AVERROR_* code otherwise.
 */
int atrac3p_bridge_create(int channels, int block_align, Atrac3pBridge **out);

/*
 * Decode one frame to interleaved signed-16 PCM.
 *
 *   frame       - exactly block_align bytes of ATRAC3+ data
 *   frame_size  - must equal the block_align passed at create time
 *   pcm_out     - capacity channels * ATRAC3P_FRAME_SAMPLES int16 values
 *   samples_out - ATRAC3P_FRAME_SAMPLES on success, 0 on failure
 *
 * Returns 0 on success or a negative AVERROR_* code on failure. Output is
 * deterministic (bit-exact decoder).
 */
int atrac3p_bridge_decode(Atrac3pBridge *b, const uint8_t *frame, int frame_size,
                          int16_t *pcm_out, int *samples_out);

/*
 * Reset all decoder history to the post-create state. Used by the HLE for
 * PSP seek (sceAtracResetPlayPosition) and loop rewind so cross-frame
 * decoder state cannot leak across a jump. Returns 0 on success.
 */
int atrac3p_bridge_reset(Atrac3pBridge *b);

/*
 * Release the instance. Safe to call with NULL.
 */
void atrac3p_bridge_destroy(Atrac3pBridge *b);

#endif /* SR_ATRAC3P_BRIDGE_H */
