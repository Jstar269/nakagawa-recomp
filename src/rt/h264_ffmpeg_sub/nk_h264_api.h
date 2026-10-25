// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Narrow C API over the vendored FFmpeg n4.4 H.264 decoder subset (this directory).
 *
 * The runtime backend (src/rt/h264_ffmpeg.c) sees only this header: no FFmpeg type, macro or
 * header leaks out of the subset. The decoder is the unmodified libavcodec H.264 decoder driven
 * through its send/receive packet interface with one thread, low-delay output and generic C DSP
 * code. Access units are Annex-B byte streams. Nothing here touches guest memory.
 */
#ifndef NK_H264_API_H
#define NK_H264_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NkH264Decoder NkH264Decoder;

enum {
    NK_H264_OK = 0,            /* operation done / picture produced */
    NK_H264_AGAIN = 1,         /* send: pictures must be received first; receive: more input needed */
    NK_H264_EOF = 2,           /* receive: the decoder is fully drained */
    NK_H264_INVALID_DATA = 3,  /* send: the unit was consumed but is not decodable */
    NK_H264_ERROR = -1         /* decoder-level failure; close the instance */
};

enum {
    NK_H264_FMT_OTHER = 0,     /* a picture layout the runtime does not convert */
    NK_H264_FMT_YUV420P = 1,   /* planar Y, Cb, Cr */
    NK_H264_FMT_NV12 = 2       /* planar Y, interleaved Cb/Cr */
};

typedef struct {
    const uint8_t *data[3];    /* Y, Cb, Cr (NV12: data[1] is the interleaved plane, data[2] unused) */
    int linesize[3];
    int width;
    int height;
    int format;                /* NK_H264_FMT_* */
    int full_range;            /* 1 for JPEG-range samples, 0 for studio range */
    int64_t pts;               /* output-frame PTS in 90 kHz units when has_pts */
    int has_pts;
} NkH264Picture;

/* verbose != 0 lets libavcodec warnings reach stderr; otherwise only errors. NULL on failure. */
NkH264Decoder *nk_h264_open(int verbose);
void nk_h264_close(NkH264Decoder *dec);

/* Give one complete Annex-B access unit to the decoder. */
int nk_h264_send(NkH264Decoder *dec, const uint8_t *data, uint32_t size, int64_t pts, int has_pts);
/* Close the input: the decoder drains its delayed pictures. */
int nk_h264_send_eos(NkH264Decoder *dec);
/* Fetch the next picture. The planes stay valid until the next receive or close. */
int nk_h264_receive(NkH264Decoder *dec, NkH264Picture *pic);

const char *nk_h264_ident(void);

#ifdef __cplusplus
}
#endif

#endif /* NK_H264_API_H */
