// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* FFmpeg (libavcodec) backend of the sr_h264 seam (sr_h264.h).
 *
 * Built only when SR_H264_FFMPEG is defined (Make: H264_FFMPEG=1, the default off Windows). The
 * decoder is the pinned FFmpeg n4.4 H.264 subset vendored under src/rt/h264_ffmpeg_sub/
 * (LGPL-2.1-or-later, statically linked, no system FFmpeg); this file sees it only through
 * nk_h264_api.h. Without the define this file compiles to nothing and h264_null.c keeps failing
 * closed.
 *
 * The backend receives complete access units from the common PSMF producer. submit_au() queues
 * one validated Annex-B unit with its normalized timestamp; frame() decodes until one picture
 * comes out and converts it into the target: a guest video buffer in a PSP pixel format, or a
 * host RGBA8888 buffer. eos != 0 drains the decoder after all submitted units.
 * A picture that was decoded but could not be written is reported as -1 without poisoning the
 * decoder; only decoder-level failures mark the instance failed.
 */

#if defined(SR_H264_FFMPEG)

#include "recomp.h"
#include "sr_h264.h"
#include "h264_ffmpeg_sub/nk_h264_api.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FF_MAX_DECODERS   4
#define FF_MAX_ES_BYTES   (64u * 1024u * 1024u)
#define FF_MAX_UNITS      (1u << 20)
#define FF_MOVIE_ROWS     272   /* PSP movie buffers are 272 rows tall */

typedef struct {
    uint32_t off;        /* byte offset of the unit in es[] */
    uint32_t len;        /* byte length of one complete access unit */
    int64_t pts;         /* normalized 90 kHz presentation time, if present */
    int has_pts;
} FfUnit;

typedef struct {
    int used;
    int failed;
    int drain_sent;      /* NULL packet sent: the decoder is draining its delayed pictures */
    NkH264Decoder *dec;
    /* The common PSMF producer supplies complete access units to the backend. */
    uint8_t *es;
    uint32_t es_len, es_cap;
    FfUnit *unit;
    uint32_t n_unit, cap_unit;
    uint32_t next_send;  /* first unit not yet given to the decoder */
} FfDec;

static FfDec s_ff[FF_MAX_DECODERS];

static int ff_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_MPEGLOG") ? 1 : 0;
    return on;
}

static FfDec *ff_get(int id) {
    if (id < 0 || id >= FF_MAX_DECODERS || !s_ff[id].used) return NULL;
    return &s_ff[id];
}

/* ---- growable storage ------------------------------------------------------------------- */

static int ff_grow(uint8_t **buf, uint32_t *cap, uint32_t need, uint32_t limit) {
    if (need > limit) return 0;
    if (need <= *cap) return 1;
    uint64_t want = *cap ? *cap : 65536u;
    while (want < need) want *= 2u;
    if (want > limit) want = limit;
    uint8_t *p = (uint8_t *)realloc(*buf, (size_t)want);
    if (!p) return 0;
    *buf = p;
    *cap = (uint32_t)want;
    return 1;
}

static int ff_append_au(FfDec *d, const SrH264AccessUnit *au) {
    if (!d || !au || !au->data || !au->size ||
        (au->has_pts != 0 && au->has_pts != 1) ||
        d->n_unit >= FF_MAX_UNITS || au->size > FF_MAX_ES_BYTES - d->es_len ||
        !ff_grow(&d->es, &d->es_cap, d->es_len + au->size, FF_MAX_ES_BYTES)) return 0;
    if (d->n_unit == d->cap_unit) {
        if (d->cap_unit >= FF_MAX_UNITS) return 0;
        uint32_t cap = d->cap_unit ? d->cap_unit * 2u : 256u;
        if (cap > FF_MAX_UNITS) cap = FF_MAX_UNITS;
        FfUnit *u = (FfUnit *)realloc(d->unit, (size_t)cap * sizeof(FfUnit));
        if (!u) return 0;
        d->unit = u;
        d->cap_unit = cap;
    }
    uint32_t start = d->es_len;
    memcpy(d->es + start, au->data, au->size);
    d->es_len += au->size;
    FfUnit *u = &d->unit[d->n_unit++];
    u->off = start;
    u->len = au->size;
    u->pts = au->pts;
    u->has_pts = au->has_pts;
    return 1;
}

/* Drop decoded units from the front once they are the larger part of a big buffer. */
static void ff_es_compact(FfDec *d) {
    uint32_t done = d->next_send;
    if (done == 0 || done >= d->n_unit || done < d->n_unit / 2u || d->es_len < (1u << 20)) return;
    uint32_t shift = d->unit[done].off;
    if (shift > d->es_len) return;
    memmove(d->es, d->es + shift, d->es_len - shift);
    d->es_len -= shift;
    uint32_t keep = d->n_unit - done;
    memmove(d->unit, d->unit + done, (size_t)keep * sizeof(FfUnit));
    for (uint32_t i = 0; i < keep; i++) d->unit[i].off -= shift;
    d->n_unit = keep;
    d->next_send -= done;
}

/* ---- vendored decoder ----------------------------------------------------- */

static void ff_close(FfDec *d) {
    nk_h264_close(d->dec);
    free(d->es);
    free(d->unit);
    memset(d, 0, sizeof(*d));
}

static int ff_open(FfDec *d) {
    d->dec = nk_h264_open(ff_log());
    if (!d->dec) {
        fprintf(stderr, "h264-ffmpeg: could not open the vendored H.264 decoder\n");
        return 0;
    }
    return 1;
}

/* Give the next submitted access unit to the decoder. 1 = sent, 0 = decoder full (receive first),
 * -1 = decoder failure. A unit the decoder rejects as invalid data is consumed and logged: the
 * stream continues, as on hardware, but nothing is reported as decoded for it. */
static int ff_send_next(FfDec *d) {
    FfUnit *u = &d->unit[d->next_send];
    if (u->off > d->es_len || u->len > d->es_len - u->off || u->len == 0) return -1;
    int r = nk_h264_send(d->dec, d->es + u->off, u->len, u->pts, u->has_pts);
    if (r == NK_H264_AGAIN) return 0;
    if (r == NK_H264_ERROR) return -1;
    if (r == NK_H264_INVALID_DATA && ff_log())
        fprintf(stderr, "h264-ffmpeg: unit %u rejected as invalid data\n", d->next_send);
    d->next_send++;
    ff_es_compact(d);
    return 1;
}

static uint8_t ff_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* BT.601 YCbCr -> RGB, limited (studio) range for H.264 movies, full range for JPEG-range
 * pixel formats. Integer coefficients in 8.8 fixed point. */
static void ff_rgb(int y, int cb, int cr, int full, uint8_t *r, uint8_t *g, uint8_t *b) {
    int du = cb - 128, dv = cr - 128;
    if (full) {
        *r = ff_clamp(y + ((359 * dv + 128) >> 8));
        *g = ff_clamp(y - ((88 * du + 183 * dv + 128) >> 8));
        *b = ff_clamp(y + ((454 * du + 128) >> 8));
    } else {
        int c = 298 * (y - 16);
        *r = ff_clamp((c + 409 * dv + 128) >> 8);
        *g = ff_clamp((c - 100 * du - 208 * dv + 128) >> 8);
        *b = ff_clamp((c + 516 * du + 128) >> 8);
    }
}

/* Chroma access for the 4:2:0 layouts libavcodec produces for PSP movies. */
typedef struct {
    const uint8_t *y, *u, *v;
    int y_stride, u_stride, v_stride;
    int interleaved;      /* NV12: u points at Cb, v = u + 1, step 2 */
    int full_range;
    SrH264PixelFormat native;
} FfPlanes;

static int ff_planes(const NkH264Picture *f, FfPlanes *pl) {
    memset(pl, 0, sizeof(*pl));
    switch (f->format) {
        case NK_H264_FMT_YUV420P:
            pl->y = f->data[0]; pl->u = f->data[1]; pl->v = f->data[2];
            pl->y_stride = f->linesize[0]; pl->u_stride = f->linesize[1]; pl->v_stride = f->linesize[2];
            pl->native = SR_H264_PIXEL_NATIVE_YUV420P;
            break;
        case NK_H264_FMT_NV12:
            pl->y = f->data[0]; pl->u = f->data[1]; pl->v = f->data[1] + 1;
            pl->y_stride = f->linesize[0]; pl->u_stride = pl->v_stride = f->linesize[1];
            pl->interleaved = 1;
            pl->native = SR_H264_PIXEL_NATIVE_NV12;
            break;
        default:
            return 0;
    }
    pl->full_range = f->full_range;
    return pl->y && pl->u && pl->v && pl->y_stride > 0 && pl->u_stride > 0 && pl->v_stride > 0;
}

static void ff_pixel(const FfPlanes *pl, int x, int y, uint8_t *r, uint8_t *g, uint8_t *b) {
    int cx = x >> 1, cy = y >> 1;
    int luma = pl->y[(size_t)y * (size_t)pl->y_stride + (size_t)x];
    int cb, cr;
    if (pl->interleaved) {
        const uint8_t *uv = pl->u + (size_t)cy * (size_t)pl->u_stride + (size_t)cx * 2u;
        cb = uv[0]; cr = uv[1];
    } else {
        cb = pl->u[(size_t)cy * (size_t)pl->u_stride + (size_t)cx];
        cr = pl->v[(size_t)cy * (size_t)pl->v_stride + (size_t)cx];
    }
    ff_rgb(luma, cb, cr, pl->full_range, r, g, b);
}

static SrH264PixelFormat ff_guest_format(int mode) {
    switch (mode) {
        case 0: return SR_H264_PIXEL_5650;
        case 1: return SR_H264_PIXEL_5551;
        case 2: return SR_H264_PIXEL_4444;
        case 3: return SR_H264_PIXEL_8888;
        default: return SR_H264_PIXEL_NONE;
    }
}

/* Write the picture into a guest video buffer (stride frame_width pixels). Every destination
 * row is checked before the first write, so a refused conversion leaves guest memory intact. */
static int ff_to_guest(const NkH264Picture *f, const FfPlanes *pl, const SrH264FrameTarget *t,
                       SrH264FrameInfo *info) {
    int w = f->width < t->frame_width ? f->width : t->frame_width;
    int h = f->height < FF_MOVIE_ROWS ? f->height : FF_MOVIE_ROWS;
    if (w <= 0 || h <= 0) return 0;
    uint64_t bpp = t->pixel_mode == 3 ? 4u : 2u;
    uint64_t row_bytes = (uint64_t)(uint32_t)w * bpp;
    uint64_t pitch = (uint64_t)(uint32_t)t->frame_width * bpp;
    uint64_t total_bytes = (uint64_t)(uint32_t)(h - 1) * pitch + row_bytes;
    if (row_bytes == 0 || row_bytes > UINT32_MAX || pitch == 0 || pitch > UINT32_MAX ||
        total_bytes > UINT32_MAX) return 0;
    for (int y = 0; y < h; y++) {
        uint64_t addr = (uint64_t)t->guest_buffer + (uint64_t)(uint32_t)y * pitch;
        if (addr + row_bytes > UINT64_C(0x100000000) ||
            !sr_guest_span_writable((uint32_t)addr, (uint32_t)row_bytes)) return 0;
    }
    /* Preserve pending GE color/depth data before the CPU writes, then publish the exact
     * committed span after conversion. sr_gpu_vram_dirty is only the post-write notification. */
    sr_guest_vram_write_prepare_bytes(t->guest_buffer, (uint32_t)total_bytes);
    uint8_t *base = (uint8_t *)SR_HOST(t->guest_buffer);
    for (int y = 0; y < h; y++) {
        uint8_t *row = base + (size_t)y * (size_t)pitch;
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            ff_pixel(pl, x, y, &r, &g, &b);
            uint16_t px16;
            switch (t->pixel_mode) {
                case 0: px16 = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)); break;
                case 1: px16 = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000); break;
                case 2: px16 = (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xF000); break;
                default:
                    row[(size_t)x * 4u + 0] = r;
                    row[(size_t)x * 4u + 1] = g;
                    row[(size_t)x * 4u + 2] = b;
                    row[(size_t)x * 4u + 3] = 0xFF;
                    continue;
            }
            memcpy(row + (size_t)x * 2u, &px16, sizeof px16);
        }
    }
    uint64_t full_bytes = (uint64_t)(uint32_t)h * pitch;
    if (w == t->frame_width && full_bytes <= UINT32_MAX) {
        sr_guest_vram_write_commit_bytes(t->guest_buffer, (uint32_t)full_bytes);
    } else {
        for (int y = 0; y < h; y++)
            sr_guest_vram_write_commit_bytes(
                t->guest_buffer + (uint32_t)((uint64_t)(uint32_t)y * pitch),
                (uint32_t)row_bytes);
    }
    if (info) {
        info->width = w;
        info->height = h;
        info->stride = (int)pitch;
        info->delivered_format = ff_guest_format(t->pixel_mode);
    }
    return 1;
}

static int ff_to_host(const NkH264Picture *f, const FfPlanes *pl, const SrH264FrameTarget *t,
                      SrH264FrameInfo *info) {
    int w = f->width < t->host_width ? f->width : t->host_width;
    int h = f->height < FF_MOVIE_ROWS ? f->height : FF_MOVIE_ROWS;
    if (w <= 0 || h <= 0) return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *row = t->host_buffer + (size_t)y * (size_t)t->host_stride;
        for (int x = 0; x < w; x++) {
            ff_pixel(pl, x, y, &row[x * 4 + 0], &row[x * 4 + 1], &row[x * 4 + 2]);
            row[x * 4 + 3] = 0xFF;
        }
    }
    if (info) {
        info->width = w;
        info->height = h;
        info->stride = t->host_stride;
        info->delivered_format = SR_H264_PIXEL_RGBA8888;
    }
    return 1;
}

/* Deliver a decoded picture. 1 = written, 2 = decoded but not writable. */
static int ff_deliver(const NkH264Picture *pic, const SrH264FrameTarget *t, SrH264FrameInfo *info) {
    FfPlanes pl;
    int ok = 0;
    if (pic->width > 0 && pic->height > 0 && ff_planes(pic, &pl)) {
        ok = t->kind == SR_H264_TARGET_HOST ? ff_to_host(pic, &pl, t, info)
                                            : ff_to_guest(pic, &pl, t, info);
        if (ok && info) {
            info->native_format = pl.native;
            /* The decoder reports the decoded frame PTS (the source PTS of the access unit it
             * came from), never a best-effort timestamp. */
            info->has_pts = pic->has_pts;
            info->pts = pic->has_pts ? pic->pts : -1;
        }
    } else if (ff_log()) {
        fprintf(stderr, "h264-ffmpeg: unsupported decoded format %d (%dx%d)\n",
                pic->format, pic->width, pic->height);
    }
    return ok ? 1 : 2;
}

static int ff_pull(FfDec *d, int eos, const SrH264FrameTarget *t, SrH264FrameInfo *info) {
    if (d->failed || !d->dec) return -1;
    for (int guard = 0; guard < 4096; guard++) {
        NkH264Picture pic;
        int r = nk_h264_receive(d->dec, &pic);
        if (r == NK_H264_OK) return ff_deliver(&pic, t, info) == 1 ? 1 : -1;
        if (r != NK_H264_AGAIN && r != NK_H264_EOF) { d->failed = 1; return -1; }
        if (r == NK_H264_EOF) return 0;                /* fully drained */
        if (d->next_send < d->n_unit) {
            int s = ff_send_next(d);
            if (s < 0) { d->failed = 1; return -1; }
            if (s == 0) return 0;                      /* full and nothing to receive */
        } else if (eos && !d->drain_sent) {
            d->drain_sent = 1;
            if (nk_h264_send_eos(d->dec) < 0) { d->failed = 1; return -1; }
        } else {
            return 0;                                  /* waiting for more input */
        }
    }
    return 0;
}

/* ---- seam entry points ------------------------------------------------------------------ */

static int ff_create(void) {
    if (getenv("SR_NOH264")) return -1;
    for (int i = 0; i < FF_MAX_DECODERS; i++) {
        if (s_ff[i].used) continue;
        FfDec *d = &s_ff[i];
        memset(d, 0, sizeof(*d));
        d->used = 1;
        if (!ff_open(d)) { ff_close(d); return -1; }
        if (ff_log()) fprintf(stderr, "h264-ffmpeg: decoder %d open (%s)\n", i, nk_h264_ident());
        return i;
    }
    return -1;
}

static void ff_destroy(int id) {
    FfDec *d = ff_get(id);
    if (d) ff_close(d);
}

static int ff_reset(int id) {
    FfDec *d = ff_get(id);
    if (!d) return -1;
    ff_close(d);
    d->used = 1;
    if (!ff_open(d)) { ff_close(d); return -1; }
    return 0;
}

static int ff_submit_au(int id, const SrH264AccessUnit *au) {
    FfDec *d = ff_get(id);
    if (!d || !au || !au->data || !au->size ||
        (au->has_pts != 0 && au->has_pts != 1) || d->failed || d->drain_sent) return -1;
    int start_code = (au->size >= 3 && au->data[0] == 0 && au->data[1] == 0 && au->data[2] == 1) ||
                     (au->size >= 4 && au->data[0] == 0 && au->data[1] == 0 &&
                      au->data[2] == 0 && au->data[3] == 1);
    if (!start_code) { d->failed = 1; return -1; }
    if (!ff_append_au(d, au)) { d->failed = 1; return -1; }
    return 0;
}

static int ff_frame(int id, int eos, const SrH264FrameTarget *t, SrH264FrameInfo *info) {
    FfDec *d = ff_get(id);
    if (!d || !t) return -1;
    if (info) { memset(info, 0, sizeof(*info)); info->pts = -1; }
    if (t->kind == SR_H264_TARGET_GUEST) {
        if (!t->guest_buffer || t->frame_width <= 0 || t->pixel_mode < 0 || t->pixel_mode > 3)
            return -1;
    } else if (t->kind == SR_H264_TARGET_HOST) {
        if (!t->host_buffer || t->host_width <= 0 || t->host_width > INT_MAX / 4 ||
            t->host_stride < t->host_width * 4)
            return -1;
    } else {
        return -1;
    }
    return ff_pull(d, eos, t, info);
}

const SrH264Backend sr_h264_ffmpeg_backend = {
    "ffmpeg",
    ff_create,
    ff_destroy,
    ff_reset,
    ff_submit_au,
    ff_frame
};

#endif /* SR_H264_FFMPEG */
