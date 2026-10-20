/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored thin C wrappers for the standalone ATRAC3+ and plain
 * ATRAC3 decoders (PR-A).
 *
 * This file is NOT imported FFmpeg code. It provides:
 *  - av_log()/av_vlog(): the only libavutil symbols the imported subset
 *    calls that FFmpeg defines in libavutil/log.c (not imported). Format
 *    strings used by the imported code already carry their own line
 *    endings, so messages are written to stderr verbatim.
 *  - the thin C ABI (create / decode-to-s16 / reset / flush / destroy)
 *    that PR-B (PSP HLE integration) will consume. No C++ exceptions
 *    cross this boundary, no PSP HLE state is held here, and no guest
 *    pointers ever reach the decoder.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "atrac3p_api.h"
#include "libavutil/mem.h"
#include "libavutil/error.h"

/* ------------------------------------------------------------------ */
/* av_log()/av_vlog() implementation (see header comment)             */
/* ------------------------------------------------------------------ */

void av_log(void *avcl, int level, const char *fmt, ...)
{
    va_list ap;
    (void)avcl;
    (void)level;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void av_vlog(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    (void)level;
    vfprintf(stderr, fmt, vl);
}

/* ------------------------------------------------------------------ */
/* Thin C ABI                                                          */
/* ------------------------------------------------------------------ */

struct Atrac3pHandle {
    AVCodecContext avctx;
    float *pcm;                          /* channels x 2048 planar float   */
    float *planes[ATRAC3P_MAX_CHANNELS]; /* per-channel pointers into pcm  */
    uint8_t *scratch;                    /* frame_size + padding, zeroed   */
    int frame_size;                      /* validated block_align          */
};

int atrac3p_create(int channels, int block_align, Atrac3pHandle **out)
{
    Atrac3pHandle *h;
    int ch, ok_channels;

    if (!out)
        return AVERROR(EINVAL);

    *out = NULL;

    switch (channels) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 6:
    case 7:
    case 8:
        ok_channels = 1;
        break;
    default:
        ok_channels = 0;
        break;
    }

    if (!ok_channels)
        return AVERROR(EINVAL);

    if (block_align <= 0)
        return AVERROR(EINVAL);

    h = av_mallocz(sizeof(*h));
    if (!h)
        return AVERROR(ENOMEM);

    h->pcm = av_mallocz((size_t)channels * ATRAC3P_FRAME_SAMPLES * sizeof(float));
    if (!h->pcm)
        goto fail;

    h->scratch = av_mallocz((size_t)block_align + ATRAC3P_PADDING_SIZE);
    if (!h->scratch)
        goto fail;

    for (ch = 0; ch < channels; ch++)
        h->planes[ch] = h->pcm + (size_t)ch * ATRAC3P_FRAME_SAMPLES;

    h->avctx.priv_data = av_mallocz(atrac3p_context_size());
    if (!h->avctx.priv_data)
        goto fail;

    h->avctx.channels      = channels;
    h->avctx.block_align   = block_align;
    h->avctx.flags         = AV_CODEC_FLAG_BITEXACT;
    h->avctx.codec_id      = AV_CODEC_ID_ATRAC3P;
    h->frame_size          = block_align;

    if (atrac3p_init(&h->avctx) < 0)
        goto fail;

    *out = h;
    return 0;

fail:
    atrac3p_destroy(h);
    return AVERROR(ENOMEM);
}

/*
 * Decode one frame to interleaved signed-16 PCM.
 *
 *   frame      - exactly h->frame_size bytes of ATRAC3+ data (copied into
 *                the padded scratch buffer before decoding).
 *   pcm_out    - receives channels x ATRAC3P_FRAME_SAMPLES interleaved
 *                samples; the caller must provide capacity
 *                channels * ATRAC3P_FRAME_SAMPLES int16 values.
 *   samples_out- receives ATRAC3P_FRAME_SAMPLES on success, 0 on failure.
 *
 * Returns the number of frame bytes consumed on success (always
 * h->frame_size for well-formed input) or a negative AVERROR_* code.
 *
 * PCM conversion: float [0..1] planes are scaled by 32768, rounded with
 * lrintf() and saturated to int16. Bit-exact DSP (AV_CODEC_FLAG_BITEXACT)
 * is forced at create time so output is deterministic on the supported
 * platform set; the conversion step itself is integer-deterministic.
 */
int atrac3p_decode(Atrac3pHandle *h, const uint8_t *frame, int frame_size,
                   int16_t *pcm_out, int *samples_out)
{
    float *planes[ATRAC3P_MAX_CHANNELS];
    int ret, nb_samples, ch, i;
    int channels;

    /* Validate every argument BEFORE touching h: a NULL handle must be
     * rejected without dereferencing it (the previous ordering read
     * h->avctx.channels up front and segfaulted on NULL). */
    if (!h || !frame || !pcm_out || !samples_out)
        return AVERROR(EINVAL);

    channels = h->avctx.channels;
    *samples_out = 0;

    /* A zero-length "frame" must not decode as a success with 2048 phantom
     * zero samples; the PSP contract always has block_align >= 1. */
    if (frame_size <= 0 || frame_size > h->frame_size)
        return AVERROR(EINVAL);

    memset(h->scratch, 0, (size_t)h->frame_size + ATRAC3P_PADDING_SIZE);
    memcpy(h->scratch, frame, (size_t)frame_size);

    for (ch = 0; ch < channels; ch++)
        planes[ch] = h->planes[ch];

    ret = atrac3p_decode_frame(&h->avctx, planes, &nb_samples,
                               h->scratch, frame_size);
    if (ret < 0)
        return ret;

    for (ch = 0; ch < channels; ch++) {
        const float *p = h->planes[ch];
        for (i = 0; i < ATRAC3P_FRAME_SAMPLES; i++)
            pcm_out[(size_t)i * channels + ch] = av_clip_int16(lrintf(p[i] * 32768.0f));
    }

    *samples_out = nb_samples;
    return ret;
}

/*
 * Reset all decoder history/state to the post-create state. Used by the
 * PSP integration when a new stream is set up. Also exposed as
 * atrac3p_flush(); the two are the same primitive.
 */
int atrac3p_reset(Atrac3pHandle *h)
{
    if (!h)
        return AVERROR(EINVAL);

    atrac3p_flush_context(&h->avctx);
    return 0;
}

int atrac3p_flush(Atrac3pHandle *h)
{
    return atrac3p_reset(h);
}

void atrac3p_destroy(Atrac3pHandle *h)
{
    if (!h)
        return;

    if (h->avctx.priv_data)
        atrac3p_close(&h->avctx);

    av_freep(&h->avctx.priv_data);
    av_freep(&h->pcm);
    av_freep(&h->scratch);
    av_freep(&h);
}

/* ------------------------------------------------------------------ */
/* Plain ATRAC3 (AT3) API                                              */
/* ------------------------------------------------------------------ */

struct Atrac3Handle {
    AVCodecContext avctx;
    float *pcm;
    float *planes[ATRAC3_MAX_CHANNELS];
    uint8_t *scratch;
    uint8_t extradata[14];
    int frame_size;
    int decode_error;
};

static unsigned atrac3_le16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static uint32_t atrac3_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int atrac3_config_valid(int format_tag, int channels, int block_align,
                        const uint8_t *extradata, int extradata_size)
{
    unsigned samples_per_frame, joint0, joint1, frame_factor;

    if (format_tag != 0x0270 || (channels != 1 && channels != 2) ||
        !extradata || extradata_size != 14 || block_align <= 0 ||
        block_align > 4096)
        return 0;
    if (atrac3_le16(extradata) != 1u)
        return 0;
    samples_per_frame = atrac3_le32(extradata + 2);
    if (samples_per_frame != (unsigned)channels * ATRAC3_FRAME_SAMPLES * 2u)
        return 0;
    joint0 = atrac3_le16(extradata + 6);
    joint1 = atrac3_le16(extradata + 8);
    frame_factor = atrac3_le16(extradata + 10);
    if (joint0 > 1u || joint1 != joint0 || frame_factor != 1u ||
        atrac3_le16(extradata + 12) != 0u || (joint0 && (channels & 1)))
        return 0;
    return block_align == 96 * channels * (int)frame_factor ||
           block_align == 152 * channels * (int)frame_factor ||
           block_align == 192 * channels * (int)frame_factor;
}

int atrac3_create(int channels, int block_align, const uint8_t *extradata,
                  Atrac3Handle **out)
{
    Atrac3Handle *h;
    int ch, ret;

    if (!out)
        return AVERROR(EINVAL);
    *out = NULL;
    if (!atrac3_config_valid(0x0270, channels, block_align, extradata, 14))
        return AVERROR(EINVAL);

    h = av_mallocz(sizeof(*h));
    if (!h)
        return AVERROR(ENOMEM);
    h->pcm = av_mallocz((size_t)channels * ATRAC3_FRAME_SAMPLES * sizeof(float));
    h->scratch = av_mallocz((size_t)block_align + ATRAC3P_PADDING_SIZE);
    h->avctx.priv_data = av_mallocz(atrac3_context_size());
    if (!h->pcm || !h->scratch || !h->avctx.priv_data) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    for (ch = 0; ch < channels; ch++)
        h->planes[ch] = h->pcm + (size_t)ch * ATRAC3_FRAME_SAMPLES;
    memcpy(h->extradata, extradata, sizeof(h->extradata));
    h->frame_size = block_align;
    h->avctx.channels = channels;
    h->avctx.block_align = block_align;
    h->avctx.flags = AV_CODEC_FLAG_BITEXACT;
    h->avctx.codec_id = AV_CODEC_ID_ATRAC3;
    h->avctx.extradata = h->extradata;
    h->avctx.extradata_size = (int)sizeof(h->extradata);

    ret = atrac3_init(&h->avctx);
    if (ret < 0)
        goto fail;
    *out = h;
    return 0;

fail:
    atrac3_destroy(h);
    return ret;
}

int atrac3_decode(Atrac3Handle *h, const uint8_t *frame, int frame_size,
                  int16_t *pcm_out, int *samples_out)
{
    int ret, samples, ch, i;

    if (!h || !frame || !pcm_out || !samples_out)
        return AVERROR(EINVAL);
    *samples_out = 0;
    if (frame_size != h->frame_size)
        return AVERROR(EINVAL);
    if (h->decode_error < 0)
        return h->decode_error;

    memset(h->scratch, 0, (size_t)h->frame_size + ATRAC3P_PADDING_SIZE);
    memcpy(h->scratch, frame, (size_t)frame_size);
    ret = atrac3_decode_frame(&h->avctx, h->planes, &samples,
                              h->scratch, frame_size);
    if (ret < 0) {
        h->decode_error = ret;
        return ret;
    }
    if (samples != ATRAC3_FRAME_SAMPLES) {
        h->decode_error = AVERROR_INVALIDDATA;
        return h->decode_error;
    }

    for (ch = 0; ch < h->avctx.channels; ch++) {
        const float *p = h->planes[ch];
        for (i = 0; i < ATRAC3_FRAME_SAMPLES; i++)
            pcm_out[(size_t)i * h->avctx.channels + ch] =
                av_clip_int16(lrintf(p[i] * 32768.0f));
    }
    *samples_out = samples;
    return ret;
}

int atrac3_reset(Atrac3Handle *h)
{
    int ret;
    if (!h)
        return AVERROR(EINVAL);
    atrac3_close(&h->avctx);
    memset(h->avctx.priv_data, 0, atrac3_context_size());
    ret = atrac3_init(&h->avctx);
    if (ret >= 0)
        h->decode_error = 0;
    else
        h->decode_error = ret;
    return ret;
}

void atrac3_destroy(Atrac3Handle *h)
{
    if (!h)
        return;
    if (h->avctx.priv_data)
        atrac3_close(&h->avctx);
    av_freep(&h->avctx.priv_data);
    av_freep(&h->pcm);
    av_freep(&h->scratch);
    av_freep(&h);
}
