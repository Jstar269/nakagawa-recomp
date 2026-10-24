// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * mp3_bridge.c — Nakagawa-authored frame-level bridge over minimp3.
 *
 * The only translation unit that compiles the vendored decoder
 * (src/rt/minimp3/minimp3.h, CC0, byte-identical; see its PROVENANCE.md).
 * Upstream defaults are kept: int16_t output and the SIMD paths the compiler
 * enables.
 *
 * minimp3's own frame search wants several consecutive frames before it trusts
 * a sync word, and it discards its decoder state when the window it is given
 * ends a few bytes past a frame. A streaming caller cannot promise either, so
 * this bridge finds and sizes the frame itself and hands minimp3 exactly one
 * whole frame, which minimp3 accepts directly and decodes with the bit
 * reservoir it kept from the previous frame.
 */

#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"

#include "mp3_bridge.h"

#include <stdlib.h>
#include <string.h>

struct Mp3Bridge {
    mp3dec_t dec;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
};

/* ISO/IEC 11172-3 and 13818-3 header tables, indexed by the raw fields. */
static const uint16_t k_bitrate_mpeg1[3][15] = {
    /* Layer III */ {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
    /* Layer II  */ {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
    /* Layer I   */ {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
};
static const uint16_t k_bitrate_mpeg2[3][15] = {
    /* Layer III */ {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    /* Layer II  */ {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    /* Layer I   */ {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
};
static const uint16_t k_sample_rate[3] = {44100, 48000, 32000};

int mp3_bridge_parse_header(const uint8_t hdr[4], Mp3BridgeHeader *out) {
    if (!hdr || !out) return -1;
    if (hdr[0] != 0xffu || (hdr[1] & 0xe0u) != 0xe0u) return -1;
    int version = (hdr[1] >> 3) & 3;
    int layer = (hdr[1] >> 1) & 3;
    int rate_index = hdr[2] >> 4;
    int sr_index = (hdr[2] >> 2) & 3;
    int padding = (hdr[2] >> 1) & 1;
    if (version == 1 || layer == 0 || rate_index == 0 || rate_index == 15 || sr_index == 3)
        return -1;

    int kbps = version == 3 ? k_bitrate_mpeg1[layer - 1][rate_index]
                            : k_bitrate_mpeg2[layer - 1][rate_index];
    /* MPEG-2 halves and MPEG-2.5 quarters the MPEG-1 rates. */
    int hz = k_sample_rate[sr_index] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    int samples = layer == 3 ? 384 : (layer == 1 && version != 3) ? 576 : 1152;
    /* Same arithmetic as the decoder (and the standard): slots of 4 bytes for
     * Layer I, 1 byte otherwise. */
    int bytes = samples * kbps * 125 / hz;
    if (layer == 3) bytes = (bytes & ~3) + padding * 4;
    else bytes += padding;

    out->version_bits = version;
    out->layer_bits = layer;
    out->bitrate_kbps = kbps;
    out->sample_rate = hz;
    out->channels = ((hdr[3] >> 6) & 3) == 3 ? 1 : 2;
    out->samples_per_frame = samples;
    out->frame_bytes = bytes;
    return 0;
}

/* Two headers belong to one stream when version, layer and sample rate agree
 * (the fields a decoder cannot change between frames). */
static int same_stream(const Mp3BridgeHeader *a, const Mp3BridgeHeader *b) {
    return a->version_bits == b->version_bits && a->layer_bits == b->layer_bits &&
           a->sample_rate == b->sample_rate;
}

int mp3_bridge_create(Mp3Bridge **out) {
    if (!out) return -1;
    *out = NULL;
    Mp3Bridge *b = (Mp3Bridge *)calloc(1, sizeof(*b));
    if (!b) return -1;
    mp3dec_init(&b->dec);
    *out = b;
    return 0;
}

int mp3_bridge_decode(Mp3Bridge *b, const uint8_t *data, int len, int16_t *pcm_out,
                      int *consumed, int *samples_out, Mp3BridgeHeader *hdr_out) {
    if (consumed) *consumed = 0;
    if (samples_out) *samples_out = 0;
    if (!b || (!data && len) || len < 0 || !pcm_out || !consumed || !samples_out)
        return MP3_BRIDGE_ERROR;

    Mp3BridgeHeader h;
    int off = 0;
    for (; off + 4 <= len; off++) {
        if (data[off] != 0xffu || mp3_bridge_parse_header(data + off, &h) != 0) continue;
        /* A sync word inside junk (an ID3 tag, say) is believed only if the
         * next header, when it is already in the window, continues the stream. */
        int next = off + h.frame_bytes;
        Mp3BridgeHeader nh;
        if (next + 4 <= len &&
            (mp3_bridge_parse_header(data + next, &nh) != 0 || !same_stream(&h, &nh)))
            continue;
        break;
    }
    if (off + 4 > len) {
        /* Keep the last three bytes: they may begin a header. */
        *consumed = len > 3 ? len - 3 : 0;
        return MP3_BRIDGE_NO_FRAME;
    }
    if (h.frame_bytes > len - off) {
        *consumed = off;
        return MP3_BRIDGE_NEED_MORE;
    }

    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    int n = mp3dec_decode_frame(&b->dec, data + off, h.frame_bytes, b->pcm, &info);
    *consumed = off + h.frame_bytes;
    if (hdr_out) *hdr_out = h;
    if (n <= 0 || n > MP3_BRIDGE_MAX_FRAME_SAMPLES || info.frame_bytes != h.frame_bytes)
        return MP3_BRIDGE_BAD_FRAME;

    if (info.channels == 1) {
        for (int i = 0; i < n; i++) {
            pcm_out[2 * i] = b->pcm[i];
            pcm_out[2 * i + 1] = b->pcm[i];
        }
    } else {
        memcpy(pcm_out, b->pcm, (size_t)n * 2u * sizeof(int16_t));
    }
    *samples_out = n;
    return MP3_BRIDGE_OK;
}

void mp3_bridge_reset(Mp3Bridge *b) {
    if (b) memset(&b->dec, 0, sizeof(b->dec));
}

void mp3_bridge_destroy(Mp3Bridge *b) {
    free(b);
}
