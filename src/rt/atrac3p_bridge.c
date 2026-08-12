// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * atrac3p_bridge.c — thin Nakagawa-authored decode bridge over the PR-A
 * ATRAC3+ decoder. No PPSSPP code. All checks are source-owned and
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
#include "libavutil/error.h"   /* AVERROR(EINVAL/ENOMEM) */

struct Atrac3pBridge {
    Atrac3pHandle *dec;   /* owned decoder instance */
    int channels;         /* validated PSP channel count */
    int block_align;      /* validated frame size */
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
    /* The PR-A API returns the consumed frame size on success; the bridge
     * normalizes that to 0 (see header contract) so callers can test
     * `ret < 0` for failure and rely on *samples_out for output. */
    int ret = atrac3p_decode(b->dec, frame, frame_size, pcm_out, samples_out);
    if (ret < 0) return ret;
    if (*samples_out <= 0) return AVERROR(EINVAL);   /* no PCM, never a success */
    return 0;
}

int atrac3p_bridge_reset(Atrac3pBridge *b) {
    if (!b) return AVERROR(EINVAL);
    return atrac3p_reset(b->dec);
}

void atrac3p_bridge_destroy(Atrac3pBridge *b) {
    if (!b) return;
    atrac3p_destroy(b->dec);
    free(b);
}
