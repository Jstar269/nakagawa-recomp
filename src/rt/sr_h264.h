// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#ifndef SR_H264_H
#define SR_H264_H

#include <stdint.h>

#define SR_H264_UNAVAILABLE_BOUNDARY "H.264 backend unavailable; in the works (#283)"

typedef enum {
    SR_H264_PIXEL_NONE = 0,
    SR_H264_PIXEL_NATIVE_NV12 = 1,
    SR_H264_PIXEL_RGBA8888 = 2,
    SR_H264_PIXEL_5650 = 3,
    SR_H264_PIXEL_5551 = 4,
    SR_H264_PIXEL_4444 = 5,
    SR_H264_PIXEL_8888 = 6,
    SR_H264_PIXEL_NATIVE_YUV420P = 7
} SrH264PixelFormat;

typedef enum {
    SR_H264_TARGET_GUEST = 0,
    SR_H264_TARGET_HOST = 1
} SrH264TargetKind;

typedef struct {
    SrH264TargetKind kind;
    uint32_t guest_buffer;
    int frame_width;
    int pixel_mode;
    /* A host target is host_height rows of host_stride bytes; a backend writes at most
     * host_width pixels into each of at most host_height rows. */
    uint8_t *host_buffer;
    int host_width;
    int host_height;
    int host_stride;
} SrH264FrameTarget;

typedef struct {
    int width;
    int height;
    int stride;
    SrH264PixelFormat native_format;
    SrH264PixelFormat delivered_format;
    /* Optional backend-reported output-frame PTS in normalized 90 kHz units. This describes
     * decoded-frame timing, not proof that the source access unit carried a PTS; consumers that
     * need source provenance must retain the producer's AU metadata. */
    int64_t pts;
    int has_pts;
} SrH264FrameInfo;

typedef struct {
    /* One complete H.264 access unit in Annex-B form, beginning with a 3- or 4-byte start code. */
    const uint8_t *data;
    uint32_t size;
    int64_t pts;       /* optional source PTS normalized by the host-neutral producer to 90 kHz */
    int has_pts;       /* exactly 0 or 1; zero means the AU's presentation time is unknown */
} SrH264AccessUnit;

typedef struct SrH264Backend {
    const char *name;
    int (*create)(void);
    void (*destroy)(int id);
    int (*reset)(int id);
    int (*submit_au)(int id, const SrH264AccessUnit *au);
    /* eos != 0 closes backend input; the caller must submit every AU before the first EOS call.
     * After EOS, submit_au rejects further input until reset. */
    int (*frame)(int id, int eos, const SrH264FrameTarget *target,
                 SrH264FrameInfo *info);
} SrH264Backend;

int sr_h264_select_backend(const char *name);
const char *sr_h264_selected_backend(void);
int sr_h264_backend_available(void);
const char *sr_h264_backend_unavailable_reason(void);

int sr_h264_create(void);
void sr_h264_destroy(int id);
int sr_h264_reset(int id);
/* Legacy sceMpeg PS input stays in the common project-authored demux layer. */
int sr_h264_feed(int id, const uint8_t *data, uint32_t len);
int sr_h264_submit_au(int id, const uint8_t *au, uint32_t len);
int sr_h264_submit_au_timed(int id, const uint8_t *au, uint32_t len,
                            int64_t pts, int has_pts);
int sr_h264_frame_ex(int id, int eos, const SrH264FrameTarget *target,
                     SrH264FrameInfo *info);
int sr_h264_drain(int id, const SrH264FrameTarget *target,
                  SrH264FrameInfo *info);
int sr_h264_frame(int id, int eos, uint32_t buffer, int frameWidth, int pixelMode);
int sr_h264_frame_host(int id, int eos, uint8_t *dst, int maxW, int maxH, int strideBytes);
int sr_h264_au_take(int id, int eos, uint64_t *psConsumed, int64_t *pts);
int64_t sr_h264_first_audio_pts(int id);

#endif
