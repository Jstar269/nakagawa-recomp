// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * mp3_bridge.h — Nakagawa-authored frame-level MPEG audio decode bridge.
 *
 * Wraps the vendored minimp3 decoder (src/rt/minimp3/, CC0, byte-identical at
 * a pinned upstream commit; see its PROVENANCE.md) with the contract the
 * sceMp3 HLE needs: parse a 4-byte frame header, and decode exactly one frame
 * from a host byte window into interleaved stereo s16. The caller owns all
 * guest-memory traffic; this file never sees a guest address.
 *
 * Failure contract (no fake PCM): a decode either produces the frame's sample
 * count, or reports why it produced none, with *samples_out left at 0 and the
 * output buffer untouched.
 *
 * Thread-safety: distinct instances are independent; a single instance is
 * single-threaded by contract.
 */

#ifndef SR_MP3_BRIDGE_H
#define SR_MP3_BRIDGE_H

#include <stdint.h>

/* Largest frame the bridge decodes: 1152 samples per channel (MPEG-1 Layer
 * II/III). Output is always two interleaved channels. */
#define MP3_BRIDGE_MAX_FRAME_SAMPLES 1152
#define MP3_BRIDGE_MAX_PCM_VALUES (MP3_BRIDGE_MAX_FRAME_SAMPLES * 2)

/* Fields of one MPEG audio frame header. version_bits and layer_bits are the
 * raw header fields (version 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5; layer 1 =
 * Layer III, 2 = Layer II, 3 = Layer I), because guest-visible sceMp3 results
 * report them that way. */
typedef struct Mp3BridgeHeader {
    int version_bits;
    int layer_bits;
    int bitrate_kbps;     /* > 0; free-format headers are rejected */
    int sample_rate;      /* Hz */
    int channels;         /* 1 (mono) or 2 */
    int samples_per_frame;
    int frame_bytes;      /* including the header and any padding slot */
} Mp3BridgeHeader;

/* Parse the 4 header bytes at hdr. Returns 0 and fills *out for a valid,
 * fixed-bitrate header; returns -1 (out untouched) otherwise. */
int mp3_bridge_parse_header(const uint8_t hdr[4], Mp3BridgeHeader *out);

typedef struct Mp3Bridge Mp3Bridge;

enum {
    MP3_BRIDGE_OK = 0,         /* one frame decoded: *samples_out > 0 */
    MP3_BRIDGE_NEED_MORE = 1,  /* a frame starts in the window but is incomplete */
    MP3_BRIDGE_NO_FRAME = 2,   /* no frame sync in the window */
    MP3_BRIDGE_BAD_FRAME = 3,  /* a complete frame was consumed but produced no PCM
                                  (for example missing bit-reservoir bytes after a seek) */
    MP3_BRIDGE_ERROR = -1      /* bad arguments */
};

/* Create a decoder. Returns 0 and *out on success, -1 on allocation failure. */
int mp3_bridge_create(Mp3Bridge **out);

/*
 * Decode the first complete frame in data[0..len).
 *
 *   pcm_out     - capacity MP3_BRIDGE_MAX_PCM_VALUES; receives samples_out
 *                 interleaved L/R pairs (a mono frame is written to both)
 *   consumed    - bytes of the window the caller may discard: any bytes before
 *                 the frame sync plus, for OK and BAD_FRAME, the frame itself
 *   samples_out - samples per channel (0 unless MP3_BRIDGE_OK)
 *   hdr_out     - optional; the decoded frame's header for OK and BAD_FRAME
 *
 * Returns one of the MP3_BRIDGE_* codes above.
 */
int mp3_bridge_decode(Mp3Bridge *b, const uint8_t *data, int len, int16_t *pcm_out,
                      int *consumed, int *samples_out, Mp3BridgeHeader *hdr_out);

/* Drop all decoder history (overlap, synthesis state and bit reservoir), as
 * after a seek. */
void mp3_bridge_reset(Mp3Bridge *b);

/* Release the instance. Safe to call with NULL. */
void mp3_bridge_destroy(Mp3Bridge *b);

#endif /* SR_MP3_BRIDGE_H */
