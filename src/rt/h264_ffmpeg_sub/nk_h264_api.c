// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Narrow C API over the vendored FFmpeg n4.4 H.264 decoder subset; see nk_h264_api.h. */

#include "nk_h264_api.h"

#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/pixfmt.h"
#include "libavutil/rational.h"

#include <stdlib.h>
#include <string.h>

struct NkH264Decoder {
    AVCodecContext *ctx;
    AVFrame *pic;
    AVPacket *pkt;
};

NkH264Decoder *nk_h264_open(int verbose) {
    static int log_level_set;
    if (!log_level_set) {            /* keep libavcodec's own chatter out of the runtime log */
        log_level_set = 1;
        av_log_set_level(verbose ? AV_LOG_WARNING : AV_LOG_ERROR);
    }
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return NULL;
    NkH264Decoder *dec = (NkH264Decoder *)calloc(1, sizeof(*dec));
    if (!dec) return NULL;
    dec->ctx = avcodec_alloc_context3(codec);
    dec->pic = av_frame_alloc();
    dec->pkt = av_packet_alloc();
    if (!dec->ctx || !dec->pic || !dec->pkt) { nk_h264_close(dec); return NULL; }
    dec->ctx->thread_count = 1;                 /* deterministic, low memory, no frame delay */
    dec->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dec->ctx->pkt_timebase = (AVRational){1, 90000};
    if (avcodec_open2(dec->ctx, codec, NULL) < 0) { nk_h264_close(dec); return NULL; }
    return dec;
}

void nk_h264_close(NkH264Decoder *dec) {
    if (!dec) return;
    avcodec_free_context(&dec->ctx);
    av_frame_free(&dec->pic);
    av_packet_free(&dec->pkt);
    free(dec);
}

static int nk_h264_map_error(int r) {
    if (r == 0) return NK_H264_OK;
    if (r == AVERROR(EAGAIN)) return NK_H264_AGAIN;
    if (r == AVERROR_EOF) return NK_H264_EOF;
    if (r == AVERROR_INVALIDDATA) return NK_H264_INVALID_DATA;
    return NK_H264_ERROR;
}

int nk_h264_send(NkH264Decoder *dec, const uint8_t *data, uint32_t size, int64_t pts, int has_pts) {
    if (!dec || !data || size == 0 || size > INT32_MAX) return NK_H264_ERROR;
    av_packet_unref(dec->pkt);
    if (av_new_packet(dec->pkt, (int)size) < 0) return NK_H264_ERROR;   /* zeroed input padding included */
    memcpy(dec->pkt->data, data, size);
    dec->pkt->pts = has_pts ? pts : AV_NOPTS_VALUE;
    int r = avcodec_send_packet(dec->ctx, dec->pkt);
    av_packet_unref(dec->pkt);
    return nk_h264_map_error(r);
}

int nk_h264_send_eos(NkH264Decoder *dec) {
    if (!dec) return NK_H264_ERROR;
    return nk_h264_map_error(avcodec_send_packet(dec->ctx, NULL));
}

int nk_h264_receive(NkH264Decoder *dec, NkH264Picture *pic) {
    if (!dec || !pic) return NK_H264_ERROR;
    av_frame_unref(dec->pic);
    int r = nk_h264_map_error(avcodec_receive_frame(dec->ctx, dec->pic));
    if (r != NK_H264_OK) return r;
    const AVFrame *f = dec->pic;
    memset(pic, 0, sizeof(*pic));
    pic->width = f->width;
    pic->height = f->height;
    for (int i = 0; i < 3; i++) {
        pic->data[i] = f->data[i];
        pic->linesize[i] = f->linesize[i];
    }
    switch (f->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            pic->format = NK_H264_FMT_YUV420P;
            break;
        case AV_PIX_FMT_NV12:
            pic->format = NK_H264_FMT_NV12;
            break;
        default:
            pic->format = NK_H264_FMT_OTHER;
            break;
    }
    pic->full_range = f->format == AV_PIX_FMT_YUVJ420P || f->color_range == AVCOL_RANGE_JPEG;
    /* pkt_timebase is 1/90000 and nk_h264_send supplies AV_NOPTS_VALUE for a unit without a
     * source PTS: report the decoded frame PTS, never best_effort_timestamp. */
    pic->has_pts = f->pts != AV_NOPTS_VALUE;
    pic->pts = pic->has_pts ? f->pts : -1;
    return NK_H264_OK;
}

const char *nk_h264_ident(void) {
    return LIBAVCODEC_IDENT;
}
