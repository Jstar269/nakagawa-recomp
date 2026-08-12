/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored standalone ATRAC3+ decoder API (PR-A).
 *
 * This is the thin C entry-point surface over the imported FFmpeg n4.4
 * decoder subset (libavcodec/atrac3plus*.c). There is no FFmpeg codec
 * framework here: the caller owns an AVCodecContext, fills the minimal
 * config fields, and drives init/decode/flush/close directly.
 *
 * Every decode returns either a success length (the number of frame bytes
 * consumed: min(block_align, buf_size)) or a negative AVERROR_* code. On
 * failure *nb_samples is left at 0, so a caller can never mistake a failed
 * decode for a complete frame.
 *
 * Thread-safety: distinct instances are independent. The imported static
 * table initialization (VLC tables, sine tables) uses a plain once-guard
 * (see libavutil/thread.h); the API is single-threaded by contract.
 */

#ifndef AT3P_API_H
#define AT3P_API_H

#include <stddef.h>
#include <stdint.h>

#include "libavcodec/avcodec.h"

/* Maximum channel count supported by the decoder (upstream
 * set_channel_params accepts 1/2/3/4/6/7/8). */
#define ATRAC3P_MAX_CHANNELS 8

/* Samples per channel per decoded frame. The imported decoder header
 * defines ATRAC3P_FRAME_SAMPLES as ATRAC3P_SUBBAND_SAMPLES *
 * ATRAC3P_SUBBANDS (128 * 16 = 2048); this header must not redefine it. */
#ifndef ATRAC3P_FRAME_SAMPLES
#define ATRAC3P_FRAME_SAMPLES 2048
#endif

/* Number of bytes to reserve after each frame buffer for bitreader
 * over-reads (upstream AV_INPUT_BUFFER_PADDING_SIZE). */
#define ATRAC3P_PADDING_SIZE AV_INPUT_BUFFER_PADDING_SIZE

/* Opaque decoder instance created by atrac3p_create() and destroyed by
 * atrac3p_destroy(). */
typedef struct Atrac3pHandle Atrac3pHandle;

/*
 * Create a decoder instance.
 *
 *   channels    - 1, 2, 3, 4, 6, 7 or 8
 *   block_align - positive frame size in bytes (PSP block alignment)
 *   out         - receives the instance, or NULL on failure
 *
 * Returns 0 on success, a negative AVERROR_* code otherwise.
 */
int atrac3p_create(int channels, int block_align, Atrac3pHandle **out);

/*
 * Decode one frame to interleaved signed-16 PCM.
 *
 *   frame       - exactly h->frame_size bytes of ATRAC3+ data
 *   frame_size  - <= the block_align passed at create time
 *   pcm_out     - capacity channels * ATRAC3P_FRAME_SAMPLES int16 values
 *   samples_out - ATRAC3P_FRAME_SAMPLES on success, 0 on failure
 *
 * Returns the number of frame bytes consumed on success or a negative
 * AVERROR_* code on failure. The decoder is bit-exact
 * (AV_CODEC_FLAG_BITEXACT) and output is deterministic.
 */
int atrac3p_decode(Atrac3pHandle *h, const uint8_t *frame, int frame_size,
                   int16_t *pcm_out, int *samples_out);

/*
 * Reset all decoder history/state to the post-create state. Used when a
 * new stream is set up. Returns 0 on success.
 */
int atrac3p_reset(Atrac3pHandle *h);

/*
 * Alias of atrac3p_reset() exposed for PSP flush semantics. Returns 0 on
 * success.
 */
int atrac3p_flush(Atrac3pHandle *h);

/*
 * Low-level decoder-state reset used by atrac3p_reset()/atrac3p_flush().
 * Not part of the stable handle API.
 */
void atrac3p_flush_context(AVCodecContext *avctx);

/*
 * Release the instance and all decoder resources. Safe to call with NULL.
 */
void atrac3p_destroy(Atrac3pHandle *h);

/*
 * Size of the private decoder context (ATRAC3PContext). The caller must
 * allocate avctx->priv_data of at least this size and zero it before
 * atrac3p_init().
 */
size_t atrac3p_context_size(void);

/*
 * Initialize the decoder. avctx->priv_data must already be a zeroed buffer
 * of at least atrac3p_context_size() bytes. Valid config fields:
 *   avctx->channels      - 1, 2, 3, 4, 6, 7 or 8
 *   avctx->block_align   - nonzero frame size
 *   avctx->flags         - AV_CODEC_FLAG_BITEXACT recommended for
 *                          deterministic output
 * Returns 0 on success, a negative AVERROR_* code otherwise.
 */
int atrac3p_init(AVCodecContext *avctx);

/*
 * Decode one ATRAC3+ frame.
 *
 *   out       - array of ATRAC3P_MAX_CHANNELS plane pointers; each plane
 *               must hold ATRAC3P_FRAME_SAMPLES floats. Only the first
 *               avctx->channels planes are written.
 *   nb_samples - set to ATRAC3P_FRAME_SAMPLES on success, 0 on failure.
 *   buf       - frame bytes; must have at least buf_size +
 *               ATRAC3P_PADDING_SIZE readable bytes (bitreader over-read).
 *
 * Returns the number of frame bytes consumed on success, or a negative
 * AVERROR_* code on failure.
 */
int atrac3p_decode_frame(AVCodecContext *avctx, float *out[ATRAC3P_MAX_CHANNELS],
                         int *nb_samples, const uint8_t *buf, int buf_size);

/*
 * Release all decoder resources. Safe to call once; afterwards only
 * avctx->priv_data itself (caller-owned) remains.
 */
void atrac3p_close(AVCodecContext *avctx);

#endif /* AT3P_API_H */
