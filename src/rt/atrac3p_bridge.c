// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * atrac3p_bridge.c — thin Nakagawa-authored decode bridges over the PR-A
 * ATRAC3+ and FFmpeg n4.4 plain-AT3 decoders. No PPSSPP code. All checks are source-owned and
 * regression-tested in src/rt/atrac3p_bridge_selftest.c.
 *
 * The bridge owns the decoder instance lifetime and the exact frame contract
 * the HLE needs: blockAlign bytes in, channels*2048 interleaved s16 out, or
 * a negative AVERROR_* with *samples_out = 0 (never fake PCM). The HLE ring
 * model stays in src/rt/hle.c (#283); this file has no guest-memory or
 * CpuState dependency so it is testable standalone.
 */

#include <stdlib.h>
#include <string.h>

#include "atrac3p_bridge.h"
#include "perf.h"
#include "libavutil/error.h"   /* AVERROR(EINVAL/ENOMEM) */

struct Atrac3pBridge {
    Atrac3pHandle *dec;   /* owned decoder instance */
    int channels;         /* validated PSP channel count */
    int block_align;      /* validated frame size */
    int decode_error;     /* latched decoder failure; cleared only by reset */
};

int atrac3p_bridge_create(int channels, int block_align, Atrac3pBridge **out) {
    if (!out) return AVERROR(EINVAL);
    *out = NULL;
    switch (channels) {
    case 1: case 2: case 3: case 4: case 6: case 7: case 8:
        break;
    default:
        return AVERROR(EINVAL);
    }
    if (block_align <= 0) return AVERROR(EINVAL);

    Atrac3pBridge *b = (Atrac3pBridge *)calloc(1, sizeof(*b));
    if (!b) return AVERROR(ENOMEM);
    int ret = atrac3p_create(channels, block_align, &b->dec);
    if (ret < 0) {
        free(b);
        return ret;
    }
    b->channels = channels;
    b->block_align = block_align;
    *out = b;
    return 0;
}

int atrac3p_bridge_decode(Atrac3pBridge *b, const uint8_t *frame, int frame_size,
                          int16_t *pcm_out, int *samples_out) {
    if (!b || !frame || !pcm_out || !samples_out) return AVERROR(EINVAL);
    *samples_out = 0;
    /* The HLE feeds exactly one blockAlign-sized frame; anything else is a
     * caller contract violation, not a decodable frame. */
    if (frame_size != b->block_align) return AVERROR(EINVAL);
    /* A rejected frame may have changed codec history before the decoder
     * discovered malformed data. Keep the stream fail-closed instead of
     * re-entering the same damaged decoder state on every guest retry. */
    if (b->decode_error < 0) return b->decode_error;
    /* The PR-A API returns the consumed frame size on success; the bridge
     * normalizes that to 0 (see header contract) so callers can test
     * `ret < 0` for failure and rely on *samples_out for output. */
    uint64_t perf_started = sr_perf_now_ns();
    int ret = atrac3p_decode(b->dec, frame, frame_size, pcm_out, samples_out);
    if (ret < 0) {
        b->decode_error = ret;
        if (perf_started) sr_perf_atrac(perf_started, ret);
        return ret;
    }
    if (*samples_out <= 0) {
        ret = AVERROR(EINVAL);
        b->decode_error = ret;
        if (perf_started) sr_perf_atrac(perf_started, ret);
        return ret;
    }
    if (perf_started) sr_perf_atrac(perf_started, ret);
    return 0;
}

int atrac3p_bridge_reset(Atrac3pBridge *b) {
    if (!b) return AVERROR(EINVAL);
    int ret = atrac3p_reset(b->dec);
    if (ret >= 0) b->decode_error = 0;
    return ret;
}

void atrac3p_bridge_destroy(Atrac3pBridge *b) {
    if (!b) return;
    atrac3p_destroy(b->dec);
    free(b);
}

struct Atrac3Bridge {
    Atrac3Handle *dec;
    int channels;
    int block_align;
    int decode_error;
};

int atrac3_bridge_create(int channels, int block_align,
                         const uint8_t *fmt_extradata, Atrac3Bridge **out) {
    if (!out) return AVERROR(EINVAL);
    *out = NULL;
    Atrac3Bridge *b = (Atrac3Bridge *)calloc(1, sizeof(*b));
    if (!b) return AVERROR(ENOMEM);
    int ret = atrac3_create(channels, block_align, fmt_extradata, &b->dec);
    if (ret < 0) {
        free(b);
        return ret;
    }
    b->channels = channels;
    b->block_align = block_align;
    *out = b;
    return 0;
}

int atrac3_bridge_decode(Atrac3Bridge *b, const uint8_t *frame, int frame_size,
                         int16_t *pcm_out, int *samples_out) {
    if (!b || !frame || !pcm_out || !samples_out) return AVERROR(EINVAL);
    *samples_out = 0;
    if (frame_size != b->block_align) return AVERROR(EINVAL);
    if (b->decode_error < 0) return b->decode_error;
    uint64_t perf_started = sr_perf_now_ns();
    int ret = atrac3_decode(b->dec, frame, frame_size, pcm_out, samples_out);
    if (ret < 0 || *samples_out != ATRAC3_FRAME_SAMPLES) {
        if (ret >= 0) ret = AVERROR(EINVAL);
        *samples_out = 0;
        b->decode_error = ret;
        if (perf_started) sr_perf_atrac(perf_started, ret);
        return ret;
    }
    if (perf_started) sr_perf_atrac(perf_started, ret);
    return 0;
}

int atrac3_bridge_reset(Atrac3Bridge *b) {
    if (!b) return AVERROR(EINVAL);
    int ret = atrac3_reset(b->dec);
    if (ret >= 0) b->decode_error = 0;
    return ret;
}

void atrac3_bridge_destroy(Atrac3Bridge *b) {
    if (!b) return;
    atrac3_destroy(b->dec);
    free(b);
}
