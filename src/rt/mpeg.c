// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

/* *
 * This is a faithful port of PPSSPP's sceMpeg control flow: PSMF header analysis, ring-buffer
 * packet accounting, MPEG handle/context creation, stream registration, and the access-unit
 * getters with timestamp progression and end-of-stream detection. The one part that cannot be
 * ported without ffmpeg is the actual AVC/ATRAC sample decode (the PPSSPP MediaEngine); here the
 * media-engine timestamp is modelled directly (video advances by videoTimestampStep per decoded
 * frame and ends at the stream's last timestamp), so a game's movie-playback loop runs and
 * *completes* identically — the decoded video frame is left blank.
 *
 * Because the project links the recompiled game (GPLv2+ via this port), this binds to GPLv2+.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "recomp.h"
#include "sr_h264.h"
/* Outside the SR_SDL3VK guard on purpose: call_guest3() is unconditional, so a
 * declaration that only exists in the SDL3 build is an implicit-declaration
 * error in every other one (portable-core caught exactly that). */
#include "nested_frames.h" /* per-owner/per-depth frames for nested guest calls */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- constants (PPSSPP sceMpeg.cpp/.h) ---- */
#define PSMF_MAGIC                 0x464D5350u
#define PSMF_STREAM_VERSION_OFFSET 0x4
#define PSMF_STREAM_OFFSET_OFFSET  0x8
#define PSMF_STREAM_SIZE_OFFSET    0xC
#define PSMF_FIRST_TIMESTAMP_OFFSET 0x54
#define PSMF_LAST_TIMESTAMP_OFFSET  0x5A
#define PSMF_AVC_WIDTH_OFFSET       142u
#define PSMF_AVC_HEIGHT_OFFSET      143u

#define MPEG_AVC_STREAM   0
#define MPEG_ATRAC_STREAM 1
#define MPEG_PCM_STREAM   2
#define MPEG_AUDIO_STREAM 15
#define MPEG_DATA_STREAM  16

#define MPEG_AVC_ES_SIZE   2048
#define MPEG_ATRAC_ES_SIZE 2112
#define MPEG_PCM_ES_SIZE   320
#define MPEG_PCM_OUTPUT_SIZE 320

/* Public MPEG constants and return families are recorded in PSPSDK's src/mpeg/pspmpeg.h. The
 * YCbCr ABI details not stated there remain an explicit #302 physical-PSP hardware-oracle gap. */
#define MPEG_MEMSIZE_0105 0x10000u
#define MPEG_MAX_DIMENSION 4096u
#define MPEG_AU_MODE_DECODE 0u
#define MPEG_AU_MODE_SKIP   1u

#define SCE_MPEG_ERROR_INVALID_VALUE 0x806100FEu
#define SCE_MPEG_ERROR_BAD_VERSION   0x806100A0u
#define SCE_MPEG_ERROR_NO_DATA       0x80618001u
#define SCE_MPEG_ERROR_NO_MEMORY     0x80610022u

static const int videoTimestampStep = 3003;   /* mpegTimestampPerSecond / 29.97 */
static const int audioTimestampStep = 4180;   /* 2048 samples / 44100 Hz */

/* ---- ring-buffer field offsets (SceMpegRingBuffer, all 32-bit) ---- */
enum {
    RB_packets = 0, RB_packetsRead = 4, RB_packetsWritePos = 8, RB_packetsAvail = 12,
    RB_packetSize = 16, RB_data = 20, RB_callback_addr = 24, RB_callback_args = 28,
    RB_dataUpperBound = 32, RB_semaID = 36, RB_mpeg = 40, RB_gp = 44,
};
#define RB_BYTES                 (RB_gp + 4u)
#define MPEG_PACKET_SIZE         2048u
#define MPEG_RING_BYTES_PER_PKT  (104u + MPEG_PACKET_SIZE)
static uint32_t rb_get(uint32_t ring, int f) { return MEM_R32(ring + (uint32_t)f); }
static void rb_set(uint32_t ring, int f, uint32_t v) { MEM_W32(ring + (uint32_t)f, v); }

static int u32_mul_checked(uint32_t a, uint32_t b, uint32_t *out) {
    uint64_t value = (uint64_t)a * b;
    if (value > UINT32_MAX) return 0;
    if (out) *out = (uint32_t)value;
    return 1;
}

static int u32_add_checked(uint32_t a, uint32_t b, uint32_t *out) {
    if (a > UINT32_MAX - b) return 0;
    if (out) *out = a + b;
    return 1;
}

typedef struct {
    uint32_t packets, packetsRead, writePos, avail, packetSize;
    uint32_t data, dataUpper, callback, callbackArgs, mpeg;
} RingState;

/* Read and validate every ring invariant before a caller can invoke the fill callback or
 * publish metadata.  The callback writes packet bytes, and the H.264 bridge reads them, so the
 * complete data span must be valid for both operations before either side is touched. */
static int rb_read_valid(uint32_t ring, RingState *out, int writable) {
    if (!ring || !out || !(writable ? sr_guest_span_writable(ring, RB_BYTES)
                                   : sr_guest_span_readable(ring, RB_BYTES)))
        return 0;
    RingState r = {
        rb_get(ring, RB_packets), rb_get(ring, RB_packetsRead),
        rb_get(ring, RB_packetsWritePos), rb_get(ring, RB_packetsAvail),
        rb_get(ring, RB_packetSize), rb_get(ring, RB_data),
        rb_get(ring, RB_dataUpperBound), rb_get(ring, RB_callback_addr),
        rb_get(ring, RB_callback_args), rb_get(ring, RB_mpeg),
    };
    uint32_t dataBytes, expectedUpper;
    if (r.packets == 0 || r.packetSize != MPEG_PACKET_SIZE ||
        r.avail > r.packets || r.writePos >= r.packets ||
        !u32_mul_checked(r.packets, r.packetSize, &dataBytes) ||
        !u32_add_checked(r.data, dataBytes, &expectedUpper) ||
        expectedUpper != r.dataUpper ||
        !sr_guest_span_writable(r.data, dataBytes))
        return 0;
    *out = r;
    return 1;
}

/* ---- host-side MPEG context, keyed by guest mpeg handle ---- */
typedef struct {
    int used;
    uint32_t handle;            /* the value stored at *mpegAddr (dataPtr+0x30) */
    uint32_t guestAddr;         /* descriptor address used by the HLE entry points */
    uint32_t ringAddr;
    uint32_t magic, rawVersion; int version;
    uint32_t offset, streamSize;
    int64_t firstTimestamp, lastTimestamp;
    int avcRegistered, atracRegistered;
    int isAnalyzed;
    int64_t videoPts, audioPts;  /* media-engine timestamp model */
    int videoEnd, audioEnd;
    uint32_t totalPackets;       /* streamSize/packetSize: whole-movie packet count */
    uint32_t fedPackets;         /* cumulative packets the game has put into the ring */
    uint32_t headerAddr;         /* guest address of the PSMF header passed to QueryStreamOffset */
    int h264;                    /* host H.264 backend id (-1 = none) */
    int h264Init, h264Frames;
    int defaultFrameWidth, pixelMode;
    uint32_t streamWidth, streamHeight;
    int ycbcrWant;   /* pictures requested through sceMpegAvcDecodeYCbCr */
    uint32_t auPacketsDone;   /* ring packets released by real access units */
    int64_t lastAuPts;        /* presentation time of the last real access unit (-1 before any) */
    int esBuffers[2];           /* MPEG_DATA_ES_BUFFERS: allocated-flag per ES buffer */
    /* stream map: small fixed table sid -> (type,num,needsReset,auMode) */
    struct { int used, type, num, needsReset, auMode; uint32_t sid; } streams[8];
} Mpeg;

static Mpeg s_mpeg[8];
static int s_mpegInit = 0;
static uint32_t s_streamIdGen = 1;
static void ycbcr_reset_mpeg(uint32_t mpegAddr);

static Mpeg *mpeg_find(uint32_t mpegAddr) {
    if (!mpegAddr || !sr_guest_span_readable(mpegAddr, 4)) return 0;
    uint32_t h = MEM_R32(mpegAddr);
    for (int i = 0; i < 8; i++) if (s_mpeg[i].used && s_mpeg[i].handle == h) return &s_mpeg[i];
    return 0;
}

/* The MPEG ring-refill callback is a nested host->guest call and shares the
 * frame policy with hle.c's GE marshallers: a per-owner, per-depth frame out of
 * the reserved region rather than the fixed 0x09df8000 this used to hard-code.
 * Everything else about the marshalling -- zeroed state, inherited $gp, $ra = 0,
 * 0xe4 VFPU prefixes, full caller restore -- is unchanged. */
static uint32_t call_guest3(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t frame_sp = 0;
    int frame = -1;
    if (!s || !fn) return 0;
    if (!sr_nested_frame_acquire(sched_current_uid(), &frame_sp, &frame)) {
        fprintf(stderr, "MPEG_CALL_GUEST: fn=0x%08x refused -- no nested guest-call frame "
                        "available (cur_uid=0x%x)\n", fn, sched_current_uid());
        return 0;
    }
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    int32_t save_slice = atomic_load_explicit(&sr_timeslice, memory_order_relaxed);
    memset(s, 0, sizeof(CpuState));
    s->r[4] = a0;
    s->r[5] = a1;
    s->r[6] = a2;
    s->r[28] = save.r[28];
    s->r[29] = frame_sp;
    s->r[31] = 0;
    s->vfpuCtrl[0] = 0xe4; s->vfpuCtrl[1] = 0xe4;
    s->pc = fn;   /* dispatch() treats pc == 0 as a lost thread and halts it */
    atomic_store_explicit(&sr_timeslice, 20000, memory_order_relaxed);
    dispatch(s, fn);
    uint32_t ret = s->r[2];
    memcpy(s, &save, sizeof(CpuState));
    atomic_store_explicit(&sr_timeslice, save_slice, memory_order_relaxed);
    (void)sr_nested_frame_release(frame);
    return ret;
}

/* big-endian 32 read from guest memory (PSMF header is big-endian) */
static uint32_t be32(uint32_t a) {
    return ((uint32_t)MEM_R8(a) << 24) | ((uint32_t)MEM_R8(a+1) << 16) | ((uint32_t)MEM_R8(a+2) << 8) | MEM_R8(a+3);
}
/* PSMF 6-byte timestamp (big-endian, 33-bit-ish packed as PPSSPP getMpegTimeStamp) */
static int64_t mpeg_ts(uint32_t a) {
    return ((int64_t)MEM_R8(a) << 32) | ((int64_t)MEM_R8(a+1) << 24) | ((int64_t)MEM_R8(a+2) << 16) |
           ((int64_t)MEM_R8(a+3) << 8) | (int64_t)MEM_R8(a+4);
}

static uint32_t mpeg_contract_error(const char *api, const char *boundary, uint32_t error) {
    fprintf(stderr, "MPEG_CONTRACT: %s: %s; in the works (#302)\n", api, boundary);
    return error;
}

static uint32_t h264_backend_error(const char *api) {
    fprintf(stderr, "H264_CONTRACT: %s: %s\n", api,
            sr_h264_backend_unavailable_reason());
    return SCE_MPEG_ERROR_NO_DATA;
}

static int getMpegVersion(uint32_t raw) {
    switch (raw) {
        case 0x32313030: return 0; case 0x33313030: return 1;
        case 0x34313030: return 2; case 0x35313030: return 3; default: return -1;
    }
}

/* AnalyzeMpeg: parse the PSMF header at bufferAddr into the context. */
static void analyze(uint32_t buffer, Mpeg *ctx) {
    ctx->magic = be32(buffer) == 0 ? 0 : 0; /* placeholder to avoid warning */
    /* magic is stored little-endian in memory but compared as PSMF_MAGIC ('PSMF' LE) */
    ctx->magic = MEM_R32(buffer);
    ctx->rawVersion = MEM_R32(buffer + PSMF_STREAM_VERSION_OFFSET);
    ctx->version = getMpegVersion(ctx->rawVersion);
    ctx->offset = be32(buffer + PSMF_STREAM_OFFSET_OFFSET);
    ctx->streamSize = be32(buffer + PSMF_STREAM_SIZE_OFFSET);
    ctx->firstTimestamp = mpeg_ts(buffer + PSMF_FIRST_TIMESTAMP_OFFSET);
    ctx->lastTimestamp = mpeg_ts(buffer + PSMF_LAST_TIMESTAMP_OFFSET);
    ctx->videoPts = 0; ctx->audioPts = 0; ctx->videoEnd = 0; ctx->audioEnd = 0;
    ctx->streamWidth = 0; ctx->streamHeight = 0;
    if (sr_guest_span_readable(buffer, PSMF_AVC_HEIGHT_OFFSET + 1u)) {
        uint32_t macroblocksWide = MEM_R8(buffer + PSMF_AVC_WIDTH_OFFSET);
        uint32_t macroblocksHigh = MEM_R8(buffer + PSMF_AVC_HEIGHT_OFFSET);
        if (!(macroblocksWide && macroblocksHigh &&
              u32_mul_checked(macroblocksWide, 16u, &ctx->streamWidth) &&
              u32_mul_checked(macroblocksHigh, 16u, &ctx->streamHeight) &&
              ctx->streamWidth <= MPEG_MAX_DIMENSION && ctx->streamHeight <= MPEG_MAX_DIMENSION))
            ctx->streamWidth = ctx->streamHeight = 0;
    }
    ctx->fedPackets = 0;
    /* Whole-movie packet count from the PSMF stream size; the movie ends when the game has fed
     * this many packets into the ring and they have been consumed (real EOF, not a header
     * timestamp which only covers the first segment). */
    ctx->totalPackets = ctx->streamSize / 2048u;
}

/* ---- SceMpegAu (auAddr): pts/dts stored as 32-bit-word-swapped s64 ---- */
static void au_write_pts(uint32_t auAddr, int off, int64_t v) {
    /* PPSSPP write(): pts = (lo<<32)|hi, then store. i.e. swap the two 32-bit halves. */
    uint32_t lo = (uint32_t)v, hi = (uint32_t)((uint64_t)v >> 32);
    MEM_W32(auAddr + (uint32_t)off, hi);          /* swapped: high word first */
    MEM_W32(auAddr + (uint32_t)off + 4, lo);
}

/* ================= exported handlers (called from hle.c) ================= */
/* Each returns the v0 value; out-params are written to guest memory directly. */

uint32_t mpeg_init(void) { s_mpegInit = 1; return 0; }
uint32_t mpeg_finish(void) {
#ifdef SR_SDL3VK
    for (int i = 0; i < 8; i++) if (s_mpeg[i].used && s_mpeg[i].h264 >= 0) {
        sr_h264_destroy(s_mpeg[i].h264);
        s_mpeg[i].h264 = -1;
    }
#endif
    for (int i = 0; i < 8; i++) if (s_mpeg[i].used) ycbcr_reset_mpeg(s_mpeg[i].guestAddr);
    memset(s_mpeg, 0, sizeof(s_mpeg));
    s_mpegInit = 0;
    return 0;
}

uint32_t mpeg_query_mem_size(uint32_t outAddr) {
    if (outAddr) MEM_W32(outAddr, MPEG_MEMSIZE_0105);
    return 0;
}
uint32_t mpeg_ringbuffer_query_mem_size(uint32_t packets) {
    uint32_t bytes;
    return u32_mul_checked(packets, MPEG_RING_BYTES_PER_PKT, &bytes) ? bytes : UINT32_MAX;
}

/* Inverse of mpeg_ringbuffer_query_mem_size: how many whole packets a buffer of
 * `mem_size` bytes holds. */
uint32_t mpeg_ringbuffer_query_pack_num(uint32_t mem_size) {
    return mem_size / MPEG_RING_BYTES_PER_PKT;
}

uint32_t mpeg_ringbuffer_construct(uint32_t ring, uint32_t numPackets, uint32_t data, uint32_t size,
                                   uint32_t cbAddr, uint32_t cbArg) {
    uint32_t dataBytes, requiredBytes, dataUpper;
    if (!ring || !sr_guest_span_writable(ring, RB_BYTES)) return 0x80020003u;
    if ((int32_t)size < 0) return SCE_MPEG_ERROR_NO_MEMORY;
    if (!u32_mul_checked(numPackets, MPEG_PACKET_SIZE, &dataBytes) ||
        !u32_mul_checked(numPackets, MPEG_RING_BYTES_PER_PKT, &requiredBytes) ||
        !u32_add_checked(data, dataBytes, &dataUpper) ||
        (dataBytes && !sr_guest_span_writable(data, dataBytes)) ||
        requiredBytes > size)
        return SCE_MPEG_ERROR_NO_MEMORY;
    rb_set(ring, RB_packets, numPackets);
    rb_set(ring, RB_packetsRead, 0);
    rb_set(ring, RB_packetsWritePos, 0);
    rb_set(ring, RB_packetsAvail, 0);
    rb_set(ring, RB_packetSize, 2048);
    rb_set(ring, RB_data, data);
    rb_set(ring, RB_callback_addr, cbAddr);
    rb_set(ring, RB_callback_args, cbArg);
    rb_set(ring, RB_dataUpperBound, dataUpper);
    rb_set(ring, RB_mpeg, 0);
    return 0;
}

uint32_t mpeg_create(uint32_t mpegAddr, uint32_t dataPtr, uint32_t size, uint32_t ringAddr,
                     uint32_t frameWidth, uint32_t mode, uint32_t ddrTop) {
    (void)mode; (void)ddrTop;
    if (!mpegAddr) return (uint32_t)-1;
    if (size < MPEG_MEMSIZE_0105) return SCE_MPEG_ERROR_NO_MEMORY;

    if (ringAddr) {
        RingState ring;
        if (!rb_read_valid(ringAddr, &ring, 1)) return SCE_MPEG_ERROR_INVALID_VALUE;
        rb_set(ringAddr, RB_packetsAvail,
               ring.packets - (ring.dataUpper - ring.data) / ring.packetSize);
        rb_set(ringAddr, RB_mpeg, mpegAddr);
    }

    /* Generate handle = dataPtr + 0x30, write it at *mpegAddr, and lay down the fake struct. */
    uint32_t h = dataPtr + 0x30;
    MEM_W32(mpegAddr, h);
    const char *lib = "LIBMPEG\0"; for (int i = 0; i < 8; i++) MEM_W8(h + (uint32_t)i, (uint8_t)lib[i]);
    const char *v = "001\0"; for (int i = 0; i < 4; i++) MEM_W8(h + 8 + (uint32_t)i, (uint8_t)v[i]);
    MEM_W32(h + 12, (uint32_t)-1);
    if (ringAddr) { MEM_W32(h + 16, ringAddr); MEM_W32(h + 20, rb_get(ringAddr, RB_dataUpperBound)); }

    Mpeg *ctx = 0; for (int i = 0; i < 8; i++) if (!s_mpeg[i].used) { ctx = &s_mpeg[i]; break; }
    if (!ctx) return SCE_MPEG_ERROR_NO_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->used = 1; ctx->handle = h; ctx->guestAddr = mpegAddr; ctx->ringAddr = ringAddr;
    ctx->defaultFrameWidth = (int)frameWidth; ctx->pixelMode = 3; ctx->isAnalyzed = 0;
    ctx->lastAuPts = -1;
    ctx->h264 = -1;
    return 0;
}

uint32_t mpeg_delete(uint32_t mpegAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
#ifdef SR_SDL3VK
    if (ctx->h264 >= 0) { sr_h264_destroy(ctx->h264); ctx->h264 = -1; }
#endif
    ycbcr_reset_mpeg(ctx->guestAddr);
    ctx->used = 0;
    return 0;
}

uint32_t mpeg_query_stream_offset(uint32_t mpegAddr, uint32_t bufferAddr, uint32_t offsetAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx || !bufferAddr || !offsetAddr) return (uint32_t)-1;
    analyze(bufferAddr, ctx);
    ctx->isAnalyzed = 1;
    ctx->headerAddr = bufferAddr;
    if (ctx->magic != PSMF_MAGIC) { MEM_W32(offsetAddr, 0); return SCE_MPEG_ERROR_INVALID_VALUE; }
    if (ctx->version < 0) { MEM_W32(offsetAddr, 0); return SCE_MPEG_ERROR_BAD_VERSION; }
    if ((ctx->offset & 2047) != 0 || ctx->offset == 0) { MEM_W32(offsetAddr, 0); return SCE_MPEG_ERROR_INVALID_VALUE; }
    MEM_W32(offsetAddr, ctx->offset);
    return 0;
}

uint32_t mpeg_query_stream_size(uint32_t bufferAddr, uint32_t sizeAddr) {
    if (!bufferAddr || !sizeAddr) return (uint32_t)-1;
    Mpeg tmp; memset(&tmp, 0, sizeof(tmp));
    analyze(bufferAddr, &tmp);
    if (tmp.magic != PSMF_MAGIC) { MEM_W32(sizeAddr, 0); return SCE_MPEG_ERROR_INVALID_VALUE; }
    if ((tmp.offset & 2047) != 0) { MEM_W32(sizeAddr, 0); return SCE_MPEG_ERROR_INVALID_VALUE; }
    MEM_W32(sizeAddr, tmp.streamSize);
    return 0;
}

uint32_t mpeg_regist_stream(uint32_t mpegAddr, uint32_t streamType, uint32_t streamNum) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (streamType == MPEG_AVC_STREAM) ctx->avcRegistered = 1;
    else if (streamType == MPEG_ATRAC_STREAM || streamType == MPEG_AUDIO_STREAM) ctx->atracRegistered = 1;
    uint32_t sid = s_streamIdGen++;
    for (int i = 0; i < 8; i++) if (!ctx->streams[i].used) {
        ctx->streams[i].used = 1; ctx->streams[i].type = (int)streamType;
        ctx->streams[i].num = (int)streamNum; ctx->streams[i].sid = sid; ctx->streams[i].needsReset = 1;
        ctx->streams[i].auMode = MPEG_AU_MODE_DECODE;
        break;
    }
    return sid;
}
/* sceMpegMallocAvcEsBuf: PPSSPP keeps a couple of flags rather than really allocating; returns a
 * 1-based ES-buffer index (non-zero) the game treats as a valid handle, or 0 if none free. The
 * earlier h_ok returned 0, so the game read the movie's ES-buffer alloc as failed and tore down. */
uint32_t mpeg_malloc_avc_es_buf(uint32_t mpegAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    for (int i = 0; i < 2; i++) if (!ctx->esBuffers[i]) { ctx->esBuffers[i] = 1; return (uint32_t)(i + 1); }
    return 0;
}
uint32_t mpeg_free_avc_es_buf(uint32_t mpegAddr, uint32_t esBuf) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (esBuf >= 1 && esBuf <= 2) ctx->esBuffers[esBuf - 1] = 0;
    return 0;
}
/* sceMpegInitAu(mpeg, esBuffer, auAddr): initialise the SceMpegAu at auAddr. AVC buffers get
 * esSize=2048/dts=0; the others (Atrac) get esSize=2112/dts=-1. esBuffer is zeroed (PPSSPP abuses
 * it for the stream id later). pts/dts are stored 32-bit-word-swapped (au_write_pts). */
/* sceMpegQueryAtracEsSize(mpeg, esSizeAddr, outSizeAddr): ES packet size 2112, decoded output
 * size 8192 (PPSSPP MPEG_ATRAC_ES_SIZE / MPEG_ATRAC_ES_OUTPUT_SIZE). */
uint32_t mpeg_query_atrac_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (esSizeAddr) MEM_W32(esSizeAddr, MPEG_ATRAC_ES_SIZE);
    if (outSizeAddr) MEM_W32(outSizeAddr, 8192);
    return 0;
}
uint32_t mpeg_init_au(uint32_t mpegAddr, uint32_t esBuffer, uint32_t auAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    int isAvc = (esBuffer >= 1 && esBuffer <= 2 && ctx->esBuffers[esBuffer - 1]);
    au_write_pts(auAddr, 0, isAvc ? 0 : 0);                 /* pts = 0 */
    au_write_pts(auAddr, 8, isAvc ? 0 : -1);                /* dts: AVC 0, Atrac UNKNOWN(-1) */
    MEM_W32(auAddr + 16, 0);                                /* esBuffer */
    MEM_W32(auAddr + 20, isAvc ? MPEG_AVC_ES_SIZE : MPEG_ATRAC_ES_SIZE);
    return 0;
}
uint32_t mpeg_unregist_stream(uint32_t mpegAddr, uint32_t sid) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (ctx) for (int i = 0; i < 8; i++) if (ctx->streams[i].used && ctx->streams[i].sid == sid) {
        ctx->streams[i].used = 0;
        ctx->streams[i].auMode = MPEG_AU_MODE_DECODE;
    }
    return 0;
}

uint32_t mpeg_ringbuffer_available_size(uint32_t ring) {
    RingState state;
    if (!rb_read_valid(ring, &state, 0)) return 0;
    return state.packets - state.avail;
}

unsigned long g_mpeg_put = 0, g_mpeg_getavc = 0, g_mpeg_avcdec = 0, g_mpeg_nodata = 0;
/* RingbufferPut(ring, numPackets, available): call the game's fill callback, then feed the bytes
 * it wrote into PPSSPP's MediaEngine bridge. */
uint32_t mpeg_ringbuffer_put(CpuState *s, uint32_t ring, uint32_t numPackets, uint32_t available) {
    g_mpeg_put++;
    RingState state;
    if (!rb_read_valid(ring, &state, 1) || numPackets == 0 || available == 0) return 0;
    uint32_t avail = state.avail;
    uint32_t total = state.packets;
    uint32_t mpegAddr = state.mpeg;
    Mpeg *ctx = mpegAddr ? mpeg_find(mpegAddr) : 0;
    uint32_t addWanted = numPackets;
    if (addWanted > available) addWanted = available;
    if (addWanted > total - avail) addWanted = total - avail;
    if (addWanted == 0) return 0;

    /* The metadata counters are guest-visible state.  Reject an unrepresentable request before
     * the callback can write any packet bytes, rather than publishing a wrapped counter later. */
    uint32_t checked;
    if (!u32_add_checked(state.packetsRead, addWanted, &checked) ||
        (ctx && !u32_add_checked(ctx->fedPackets, addWanted, &checked))) return 0;

    uint32_t cb = state.callback;
    uint32_t cbArg = state.callbackArgs;
    uint32_t packetSize = state.packetSize;
    if (cb && !sr_lookup(cb)) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "mpeg: ring fill callback 0x%08x is not recompiled code "
                    "(ring=0x%08x data=0x%08x cbArg=0x%08x mpeg=0x%08x) -- runtime-loaded code?\n",
                    cb, ring, rb_get(ring, RB_data), cbArg, mpegAddr);
        }
    }

    uint32_t addedTotal = 0;
    uint32_t writePos = state.writePos;
    while (addedTotal < addWanted) {
        uint32_t chunk = addWanted - addedTotal;
        if (total && chunk > total - writePos) chunk = total - writePos;
        if (chunk == 0) break;

        uint32_t byteOffset, chunkBytes, dst;
        if (!u32_mul_checked(writePos, packetSize, &byteOffset) ||
            !u32_mul_checked(chunk, packetSize, &chunkBytes) ||
            !u32_add_checked(state.data, byteOffset, &dst) ||
            !sr_guest_span_writable(dst, chunkBytes))
            return 0;
        uint32_t got = cb ? call_guest3(s, cb, dst, chunk, cbArg) : chunk;
        if ((int32_t)got < 0) {
            if (addedTotal == 0) return got;
            break;
        }
        if (got > chunk) got = chunk;
        if (got == 0) break;

        uint32_t gotBytes;
        if (!u32_mul_checked(got, packetSize, &gotBytes) ||
            !sr_guest_span_readable(dst, gotBytes))
            return 0;

#ifdef SR_SDL3VK
        if (ctx && got) {
            if (!ctx->h264Init) { ctx->h264Init = 1; ctx->h264 = sr_h264_create(); }
            if (ctx->h264 >= 0)
                sr_h264_feed(ctx->h264, (const uint8_t *)SR_HOST(dst), gotBytes);
        }
#endif

        addedTotal += got;
        writePos = (got == total - writePos) ? 0 : writePos + got;
        if (got < chunk) break;
    }

    if (addedTotal) {
        rb_set(ring, RB_packetsAvail, avail + addedTotal);
        rb_set(ring, RB_packetsRead, state.packetsRead + addedTotal);
        rb_set(ring, RB_packetsWritePos, writePos);
        if (ctx) ctx->fedPackets += addedTotal;
    }
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || addedTotal != addWanted)
            fprintf(stderr, "MpegRingbufferPut ring=0x%x want=%u/%u cb=0x%x -> %u avail=%u/%u\n",
                    ring, numPackets, available, cb, addedTotal, rb_get(ring, RB_packetsAvail), total);
    }
    return addedTotal;
}

static Mpeg *au_stream(uint32_t mpegAddr, uint32_t sid, int *needsReset, int *num) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return 0;
    for (int i = 0; i < 8; i++) if (ctx->streams[i].used && ctx->streams[i].sid == sid) {
        if (needsReset) *needsReset = ctx->streams[i].needsReset;
        if (num) *num = ctx->streams[i].num;
        ctx->streams[i].needsReset = 0;
        return ctx;
    }
    return ctx;   /* stream not found still returns ctx; caller checks */
}

uint32_t mpeg_get_avc_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr) {
    g_mpeg_getavc++;
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    uint32_t ring = ctx->ringAddr;
    if (!ring) return (uint32_t)-1;
    /* Real end-of-stream: the game has fed the whole movie (fedPackets >= totalPackets) and the
     * ring has drained. This mirrors PPSSPP's mediaengine->IsVideoEnd() without ffmpeg -- the movie
     * runs for the full file then ends, instead of stopping at the (segment-only) header timestamp. */
    if (ctx->totalPackets && ctx->fedPackets >= ctx->totalPackets && rb_get(ring, RB_packetsAvail) == 0)
        ctx->videoEnd = 1;
    if (rb_get(ring, RB_packetsRead) == 0 || rb_get(ring, RB_packetsAvail) == 0) {
        g_mpeg_nodata++;
        au_write_pts(auAddr, 0, -1); au_write_pts(auAddr, 8, -1);
        return SCE_MPEG_ERROR_NO_DATA;
    }
    int needsReset = 0, num = 0;
    au_stream(mpegAddr, sid, &needsReset, &num);
    uint32_t release = 1;   /* timestamp model: one packet per access unit */
#ifdef SR_SDL3VK
    /* With a decoder, an access unit is one real picture of the demuxed stream: hand it out only
     * once it has been fed completely, and release exactly the ring packets it consumed. */
    if (ctx->h264Init && ctx->h264 >= 0) {
        int eos = ctx->totalPackets && ctx->fedPackets >= ctx->totalPackets;
        uint64_t psConsumed = 0;
        int64_t auPts = -1;
        int r = sr_h264_au_take(ctx->h264, eos, &psConsumed, &auPts);
        if (r == 0) {
            if (eos) ctx->videoEnd = 1;
            g_mpeg_nodata++;
            au_write_pts(auAddr, 0, -1); au_write_pts(auAddr, 8, -1);
            return SCE_MPEG_ERROR_NO_DATA;
        }
        if (r > 0) {
            uint64_t done = psConsumed / MPEG_PACKET_SIZE;
            release = done > ctx->auPacketsDone ? (uint32_t)(done - ctx->auPacketsDone) : 0u;
            ctx->auPacketsDone += release;
            /* The stream's own presentation time: PSMF puts a PTS on only some pictures, so the
             * others follow the previous one by one frame. A context whose header was never
             * analysed takes its first timestamp from the first picture, which keeps the audio
             * clock (firstTimestamp + decoded audio) on the same time base. */
            if (auPts >= 0) ctx->lastAuPts = auPts;
            else if (ctx->lastAuPts >= 0) ctx->lastAuPts += videoTimestampStep;
            else ctx->lastAuPts = ctx->firstTimestamp;
            if (!ctx->firstTimestamp && auPts >= 0) ctx->firstTimestamp = auPts;
            int64_t realPts = ctx->lastAuPts;
            au_write_pts(auAddr, 0, realPts);
            au_write_pts(auAddr, 8, realPts - videoTimestampStep);
            MEM_W32(auAddr + 16, (uint32_t)num);
            uint32_t av = rb_get(ring, RB_packetsAvail);
            rb_set(ring, RB_packetsAvail, av > release ? av - release : 0u);
            if (attrAddr) MEM_W32(attrAddr, 1);
            if (ctx->videoEnd) return SCE_MPEG_ERROR_NO_DATA;
            return 0;
        }
    }
#endif
    int64_t pts = ctx->videoPts + ctx->firstTimestamp;
    au_write_pts(auAddr, 0, pts);
    au_write_pts(auAddr, 8, pts - videoTimestampStep);
    MEM_W32(auAddr + 16, (uint32_t)num);            /* esBuffer abused as stream num */
    uint32_t avail = rb_get(ring, RB_packetsAvail);
    rb_set(ring, RB_packetsAvail, avail > release ? avail - release : 0u);
    if (attrAddr) MEM_W32(attrAddr, 1);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegGetAvcAu #%d pts=%lld avail=%u end=%d\n",
                    n, (long long)pts, rb_get(ring, RB_packetsAvail), ctx->videoEnd);
    }
    if (ctx->videoEnd) return SCE_MPEG_ERROR_NO_DATA;
    return 0;
}

uint32_t mpeg_get_atrac_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    uint32_t ring = ctx->ringAddr;
    if (!ring) return (uint32_t)-1;
    int needsReset = 0, num = 0;
    au_stream(mpegAddr, sid, &needsReset, &num);
    /* PSP/PPSSPP's libmpeg clock uses the PSMF presentation origin for ATRAC AUs.  In
     * particular, PPSSPP's sceMpeg implementation documents audioFirstTimestamp as 90000,
     * matching the PSMF first timestamp and the first AVC AU.  A raw private-stream PES PTS is
     * not the player clock origin: using it here made the audio clock title-dependent and moved
     * it ahead of the first video AU, which can make libpsmfplayer reject the video as too early.
     * Keep the demuxer's firstAudioPts for diagnostics, but do not substitute it for the public
     * sceMpeg AU time base. */
    int64_t pts = ctx->audioPts + ctx->firstTimestamp;
    au_write_pts(auAddr, 0, pts);
    au_write_pts(auAddr, 8, pts);
    MEM_W32(auAddr + 20, MPEG_ATRAC_ES_SIZE);
    if (attrAddr) MEM_W32(attrAddr, 0);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 16 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegGetAtracAu #%d pts=%lld\n", n, (long long)pts);
    }
    return 0;   /* audio AU available; the audio ring drains with the video at EOF */
}

static int video_geometry(const Mpeg *ctx, uint32_t requestedWidth,
                          uint32_t *outWidth, uint32_t *outHeight) {
    if (!ctx || !outWidth || !outHeight) return 0;
    uint32_t width = requestedWidth ? requestedWidth : (uint32_t)ctx->defaultFrameWidth;
    uint32_t height = ctx->streamHeight;
    if (!width || !height || width > MPEG_MAX_DIMENSION || height > MPEG_MAX_DIMENSION ||
        (width & 15u) || (height & 15u)) return 0;
    *outWidth = width;
    *outHeight = height;
    return 1;
}

static uint32_t video_buffer_bytes(uint32_t width, uint32_t height, int pixelMode) {
    if (!width || !height || width > MPEG_MAX_DIMENSION || height > MPEG_MAX_DIMENSION ||
        (pixelMode < 0 || pixelMode > 3)) return 0;
    uint32_t bpp = pixelMode == 3 ? 4u : 2u;
    uint32_t pixels, bytes;
    if (!u32_mul_checked(width, height, &pixels) || !u32_mul_checked(pixels, bpp, &bytes)) return 0;
    return bytes;
}

static int clear_video_buffer(uint32_t ptr, uint32_t width, uint32_t height, int pixelMode) {
    uint32_t bytes = video_buffer_bytes(width, height, pixelMode);
    if (!ptr || !bytes || !sr_guest_span_writable(ptr, bytes)) return 0;
    for (uint32_t i = 0; i < bytes; i++) MEM_W8(ptr + i, 0);
    return 1;
}

/* AvcDecode(mpeg, auAddr, frameWidth, bufferAddr, initAddr): decode one AVC frame into
 * *bufferAddr. The output geometry comes from the guest stride and the analysed stream. */
uint32_t mpeg_avc_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t initAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!sr_guest_span_readable(auAddr, 24u) || !sr_guest_span_readable(bufferAddr, 4u) ||
        !sr_guest_span_writable(initAddr, 4u) || (bufferAddr & 3u) || (initAddr & 3u))
        return mpeg_contract_error("sceMpegAvcDecode", "invalid AU or output pointer", (uint32_t)-1);
    uint32_t width, height;
    if (!video_geometry(ctx, frameWidth, &width, &height))
        return mpeg_contract_error("sceMpegAvcDecode", "missing or unsupported stream geometry", SCE_MPEG_ERROR_INVALID_VALUE);
    uint32_t buffer = MEM_R32(bufferAddr);
    uint32_t bytes = video_buffer_bytes(width, height, ctx->pixelMode);
    if (!buffer || (buffer & 3u) || (height && (!bytes || !sr_guest_span_writable(buffer, bytes))))
        return mpeg_contract_error("sceMpegAvcDecode", "invalid destination span", SCE_MPEG_ERROR_INVALID_VALUE);

    g_mpeg_avcdec++;
    if (!ctx->h264Init || ctx->h264 < 0) {
        MEM_W32(initAddr, 0u);
        return h264_backend_error("sceMpegAvcDecode");
    }
    ctx->videoPts += videoTimestampStep;
    int gotFrame = 0;
#ifdef SR_SDL3VK
    {
        int eos = ctx->totalPackets && ctx->fedPackets >= ctx->totalPackets;
        SrH264FrameTarget target;
        SrH264FrameInfo info;
        memset(&target, 0, sizeof(target));
        target.kind = SR_H264_TARGET_GUEST;
        target.guest_buffer = buffer;
        target.frame_width = (int)width;
        target.pixel_mode = ctx->pixelMode;
        gotFrame = sr_h264_frame_ex(ctx->h264, eos, &target, &info);
        if (gotFrame > 0) ctx->h264Frames++;
    }
#endif
    if (gotFrame <= 0 && !ctx->h264Frames) {
        if (!height) {
            MEM_W32(initAddr, 0u);
            return mpeg_contract_error("sceMpegAvcDecode", "stream height is unavailable for a blank frame", SCE_MPEG_ERROR_NO_DATA);
        }
        if (!clear_video_buffer(buffer, width, height, ctx->pixelMode))
            return mpeg_contract_error("sceMpegAvcDecode", "destination changed during validation", SCE_MPEG_ERROR_INVALID_VALUE);
        extern void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
        sr_gpu_vram_dirty(buffer, bytes);
    }
    MEM_W32(initAddr, gotFrame > 0 ? 1u : 0u);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcDecode #%d buf=0x%x fw=%u pts=%lld dec=%d frames=%d\n",
                    n, buffer, width, (long long)ctx->videoPts, gotFrame, ctx->h264Frames);
    }
    return gotFrame > 0 ? 0 : SCE_MPEG_ERROR_NO_DATA;
}
/* ---- YCbCr decode path (sceMpegAvc*YCbCr / sceMpegAvcCsc) ----
 * The guest-visible YCbCr allocation is a deterministic, zero-filled contract. The decoded
 * picture is retained separately as RGBA so the existing H.264 backend can stay unchanged; every
 * operation rechecks the allocation address, geometry, guest fingerprint, and decoded-picture
 * state before using that hidden store. Unsupported firmware-visible details fail closed and
 * name #302 instead of being inferred from a title's dimensions. */
#define YCBCR_HEADER_BYTES 128u
#define YCBCR_ALIGNMENT 16u
#define YCBCR_SLOT_COUNT 16

typedef struct {
    uint32_t mpeg, buf, width, height, size;
    uint64_t guest_tag;
    uint8_t *rgba;
    int initialized, valid;
} YcbcrBuf;
static YcbcrBuf s_ycbcr[YCBCR_SLOT_COUNT];

static int ycbcr_mode_valid(uint32_t mode) {
    return mode == UINT32_MAX || mode <= 3u;
}

static int ycbcr_size_for(uint32_t width, uint32_t height, uint32_t *size) {
    if (!size || !width || !height || width > MPEG_MAX_DIMENSION || height > MPEG_MAX_DIMENSION ||
        (width & 15u) || (height & 15u)) return 0;
    uint32_t pixels, bytes;
    if (!u32_mul_checked(width / 2u, height / 2u, &pixels) ||
        !u32_mul_checked(pixels, 6u, &bytes) || !u32_add_checked(bytes, YCBCR_HEADER_BYTES, size))
        return 0;
    return 1;
}

static YcbcrBuf *ycbcr_find(uint32_t mpegAddr, uint32_t buf) {
    for (int i = 0; i < YCBCR_SLOT_COUNT; i++)
        if (buf && s_ycbcr[i].buf == buf && s_ycbcr[i].mpeg == mpegAddr) return &s_ycbcr[i];
    return NULL;
}

static void ycbcr_clear_slot(YcbcrBuf *b) {
    if (!b) return;
    free(b->rgba);
    memset(b, 0, sizeof(*b));
}

static void ycbcr_reset_mpeg(uint32_t mpegAddr) {
    for (int i = 0; i < YCBCR_SLOT_COUNT; i++)
        if (s_ycbcr[i].mpeg == mpegAddr) ycbcr_clear_slot(&s_ycbcr[i]);
}

static uint64_t ycbcr_fingerprint(uint32_t buf, uint32_t size) {
    const uint8_t *p = (const uint8_t *)SR_HOST(buf);
    uint64_t tag = UINT64_C(1469598103934665603);
    for (uint32_t i = 0; i < size; i++) {
        tag ^= p[i];
        tag *= UINT64_C(1099511628211);
    }
    return tag;
}

static int ycbcr_state_usable(const YcbcrBuf *b, int require_picture) {
    uint32_t expected;
    if (!b || !b->initialized || !b->rgba || !b->buf ||
        !ycbcr_size_for(b->width, b->height, &expected) || expected != b->size ||
        !sr_guest_span_readable(b->buf, b->size) || !sr_guest_span_writable(b->buf, b->size) ||
        ycbcr_fingerprint(b->buf, b->size) != b->guest_tag) return 0;
    return !require_picture || b->valid;
}

static YcbcrBuf *ycbcr_slot_for_init(uint32_t mpegAddr, uint32_t buf, uint32_t width,
                                     uint32_t height, uint32_t size) {
    YcbcrBuf *b = ycbcr_find(mpegAddr, buf);
    if (!b) {
        for (int i = 0; i < YCBCR_SLOT_COUNT; i++) if (!s_ycbcr[i].buf) {
            b = &s_ycbcr[i];
            break;
        }
    }
    if (!b) return NULL;
    uint64_t rgbaBytes64 = (uint64_t)width * height * 4u;
    if (rgbaBytes64 == 0 || rgbaBytes64 > (uint64_t)SIZE_MAX) return NULL;
    uint8_t *rgba = (uint8_t *)calloc((size_t)rgbaBytes64, 1u);
    if (!rgba) return NULL;
    for (int i = 0; i < YCBCR_SLOT_COUNT; i++)
        if (s_ycbcr[i].buf == buf && s_ycbcr[i].mpeg != mpegAddr) ycbcr_clear_slot(&s_ycbcr[i]);
    ycbcr_clear_slot(b);
    b->mpeg = mpegAddr; b->buf = buf; b->width = width; b->height = height; b->size = size;
    b->rgba = rgba; b->initialized = 1; b->valid = 0;
    memset(SR_HOST(buf), 0, size);
    b->guest_tag = ycbcr_fingerprint(buf, size);
    return b;
}

static int ycbcr_pointer_valid(uint32_t addr, uint32_t size, uint32_t alignment) {
    return addr && !(addr & (alignment - 1u)) && sr_guest_span_writable(addr, size);
}

/* The public PSPSDK MPEG surface uses 4:2:0 YCbCr allocations with a 128-byte header. The
 * dimensions and mode are supplied by the guest; the only hardware-independent contract exposed
 * here is that layout. Firmware values outside it remain an explicit #302 limitation. */
uint32_t mpeg_avc_query_ycbcr_size(uint32_t mpegAddr, uint32_t mode, uint32_t width,
                                   uint32_t height, uint32_t resultAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!resultAddr || (resultAddr & 3u) || !sr_guest_span_writable(resultAddr, 4u))
        return mpeg_contract_error("sceMpegAvcQueryYCbCrSize", "unaligned or invalid result pointer", SCE_MPEG_ERROR_INVALID_VALUE);
    uint32_t size;
    if (!ycbcr_mode_valid(mode) || !ycbcr_size_for(width, height, &size)) {
        MEM_W32(resultAddr, 0);
        return mpeg_contract_error("sceMpegAvcQueryYCbCrSize", "unsupported mode or geometry", SCE_MPEG_ERROR_INVALID_VALUE);
    }
    if (ctx->streamWidth && (width != ctx->streamWidth || height != ctx->streamHeight)) {
        MEM_W32(resultAddr, 0);
        return mpeg_contract_error("sceMpegAvcQueryYCbCrSize", "geometry disagrees with the PSMF stream", SCE_MPEG_ERROR_INVALID_VALUE);
    }
    MEM_W32(resultAddr, size);
    return 0;
}

uint32_t mpeg_avc_init_ycbcr(uint32_t mpegAddr, uint32_t mode, uint32_t width, uint32_t height,
                             uint32_t buf) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    uint32_t size;
    if (!ycbcr_mode_valid(mode) || !ycbcr_size_for(width, height, &size))
        return mpeg_contract_error("sceMpegAvcInitYCbCr", "unsupported mode or geometry", SCE_MPEG_ERROR_INVALID_VALUE);
    if (ctx->streamWidth && (width != ctx->streamWidth || height != ctx->streamHeight))
        return mpeg_contract_error("sceMpegAvcInitYCbCr", "geometry disagrees with the PSMF stream", SCE_MPEG_ERROR_INVALID_VALUE);
    if (!ycbcr_pointer_valid(buf, size, YCBCR_ALIGNMENT))
        return mpeg_contract_error("sceMpegAvcInitYCbCr", "unaligned or truncated guest allocation", SCE_MPEG_ERROR_INVALID_VALUE);
    if (!ycbcr_slot_for_init(mpegAddr, buf, width, height, size))
        return mpeg_contract_error("sceMpegAvcInitYCbCr", "host YCbCr state allocation failed", SCE_MPEG_ERROR_NO_MEMORY);
    return 0;
}

/* sceMpegAvcDecodeMode(mpeg, mode*): mode[1] is the output pixel format (0..3). */
uint32_t mpeg_avc_decode_mode(uint32_t mpegAddr, uint32_t modeAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!modeAddr || (modeAddr & 3u) || !sr_guest_span_readable(modeAddr, 8u))
        return mpeg_contract_error("sceMpegAvcDecodeMode", "unaligned or truncated mode pointer", SCE_MPEG_ERROR_INVALID_VALUE);
    uint32_t pm = MEM_R32(modeAddr + 4u);
    if (pm > 3u)
        return mpeg_contract_error("sceMpegAvcDecodeMode", "unsupported pixel mode", SCE_MPEG_ERROR_INVALID_VALUE);
    ctx->pixelMode = (int)pm;
    return 0;
}

/* sceMpegAvcDecodeYCbCr(mpeg, au, ycbcr*, init*): the buffer argument points at the YCbCr
 * allocation address, and init is the guest-visible picture-ready result. */
uint32_t mpeg_avc_decode_ycbcr(uint32_t mpegAddr, uint32_t auAddr, uint32_t bufPtr, uint32_t initAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!sr_guest_span_readable(auAddr, 24u) || !sr_guest_span_readable(bufPtr, 4u) ||
        !sr_guest_span_writable(initAddr, 4u) || (bufPtr & 3u) || (initAddr & 3u))
        return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "invalid AU, buffer, or init pointer", (uint32_t)-1);
    uint32_t buf = MEM_R32(bufPtr);
    YcbcrBuf *b = ycbcr_find(mpegAddr, buf);
    if (!b || !ycbcr_state_usable(b, 0))
        return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "missing, stale, or modified guest YCbCr state", SCE_MPEG_ERROR_INVALID_VALUE);
    g_mpeg_avcdec++;
    if (!ctx->h264Init || ctx->h264 < 0) {
        b->valid = 0;
        MEM_W32(initAddr, 0u);
        return h264_backend_error("sceMpegAvcDecodeYCbCr");
    }
    ctx->videoPts += videoTimestampStep;
    ctx->ycbcrWant++;
    int got = 0;
    int eos = ctx->totalPackets && ctx->fedPackets >= ctx->totalPackets;
#ifdef SR_SDL3VK
    while (ctx->h264Frames < ctx->ycbcrWant) {
        SrH264FrameTarget target;
        SrH264FrameInfo info;
        memset(&target, 0, sizeof(target));
        target.kind = SR_H264_TARGET_HOST;
        target.host_buffer = b->rgba;
        target.host_width = (int)b->width;
        target.host_stride = (int)b->width * 4;
        int r = sr_h264_frame_ex(ctx->h264, eos, &target, &info);
        if (r > 0) {
            got = r;
            b->valid = 1;
            ctx->h264Frames++;
        } else if (r < 0) {
            b->valid = 0;
            MEM_W32(initAddr, 0u);
            return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "H.264 backend rejected the access unit", SCE_MPEG_ERROR_NO_DATA);
        } else {
            break;
        }
    }
#endif
    if (got <= 0) {
        b->valid = 0;
        MEM_W32(initAddr, 0u);
        if (eos)
            return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "no decoded picture at end of stream", SCE_MPEG_ERROR_NO_DATA);
        return 0;
    }
    MEM_W32(initAddr, 1u);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcDecodeYCbCr #%d au=0x%x ycbcr=0x%x dec=%d frames=%d pts=%lld\n",
                    n, auAddr, buf, got, ctx->h264Frames, (long long)ctx->videoPts);
    }
    return 0;
}

uint32_t mpeg_avc_decode_stop_ycbcr(uint32_t mpegAddr, uint32_t buf, uint32_t statusAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!statusAddr || (statusAddr & 3u) || !sr_guest_span_writable(statusAddr, 4u))
        return mpeg_contract_error("sceMpegAvcDecodeStopYCbCr", "invalid status pointer", (uint32_t)-1);
    YcbcrBuf *b = ycbcr_find(mpegAddr, buf);
    if (!b || !ycbcr_state_usable(b, 0))
        return mpeg_contract_error("sceMpegAvcDecodeStopYCbCr", "missing, stale, or modified guest YCbCr state", SCE_MPEG_ERROR_INVALID_VALUE);
    b->valid = 0;
    memset(b->rgba, 0, (size_t)b->width * b->height * 4u);
    memset(SR_HOST(b->buf), 0, b->size);
    b->guest_tag = ycbcr_fingerprint(b->buf, b->size);
    MEM_W32(statusAddr, 0u);
    return 0;
}

static int ycbcr_ranges_overlap(uint32_t a, uint32_t b, uint32_t size) {
    uint64_t a0 = a, a1 = a0 + size, b0 = b, b1 = b0 + size;
    return a0 < b1 && b0 < a1;
}

uint32_t mpeg_avc_copy_ycbcr(uint32_t mpegAddr, uint32_t dst, uint32_t src) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!dst || !src || (dst & (YCBCR_ALIGNMENT - 1u)) || (src & (YCBCR_ALIGNMENT - 1u)))
        return mpeg_contract_error("sceMpegAvcCopyYCbCr", "unaligned source or destination", SCE_MPEG_ERROR_INVALID_VALUE);
    YcbcrBuf *from = ycbcr_find(mpegAddr, src);
    YcbcrBuf *to = ycbcr_find(mpegAddr, dst);
    if (!from || !to || !ycbcr_state_usable(from, 1) || !ycbcr_state_usable(to, 0))
        return mpeg_contract_error("sceMpegAvcCopyYCbCr", "missing, stale, or undecoded guest YCbCr state", SCE_MPEG_ERROR_INVALID_VALUE);
    if (from->width != to->width || from->height != to->height || from->size != to->size)
        return mpeg_contract_error("sceMpegAvcCopyYCbCr", "source and destination geometry differ", SCE_MPEG_ERROR_INVALID_VALUE);
    if (dst == src) return 0;
    if (ycbcr_ranges_overlap(dst, src, from->size))
        return mpeg_contract_error("sceMpegAvcCopyYCbCr", "overlapping allocations are not modeled", SCE_MPEG_ERROR_INVALID_VALUE);
    memcpy(SR_HOST(dst), SR_HOST(src), from->size);
    memcpy(to->rgba, from->rgba, (size_t)from->width * from->height * 4u);
    to->valid = from->valid;
    to->guest_tag = from->guest_tag;
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcCopyYCbCr #%d dst=0x%x src=0x%x size=%u\n", n, dst, src, from->size);
    }
    return 0;
}

/* sceMpegAvcCsc(mpeg, ycbcr, range*, frameWidth, dest): range is {x, y, w, h} in pixels. */
uint32_t mpeg_avc_csc(uint32_t mpegAddr, uint32_t buf, uint32_t rangeAddr, uint32_t frameWidth,
                      uint32_t dest) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!rangeAddr || (rangeAddr & 3u) || !sr_guest_span_readable(rangeAddr, 16u) ||
        !dest || (dest & 3u))
        return mpeg_contract_error("sceMpegAvcCsc", "unaligned or truncated range/destination pointer", SCE_MPEG_ERROR_INVALID_VALUE);
    if (frameWidth == 0) frameWidth = (uint32_t)ctx->defaultFrameWidth;
    if (!frameWidth || frameWidth > MPEG_MAX_DIMENSION || ctx->pixelMode < 0 || ctx->pixelMode > 3)
        return mpeg_contract_error("sceMpegAvcCsc", "invalid stride or pixel mode", SCE_MPEG_ERROR_INVALID_VALUE);
    YcbcrBuf *b = ycbcr_find(mpegAddr, buf);
    if (!b || !ycbcr_state_usable(b, 1))
        return mpeg_contract_error("sceMpegAvcCsc", "missing, stale, or undecoded guest YCbCr state", SCE_MPEG_ERROR_INVALID_VALUE);
    int32_t x = (int32_t)MEM_R32(rangeAddr);
    int32_t y = (int32_t)MEM_R32(rangeAddr + 4u);
    int32_t w = (int32_t)MEM_R32(rangeAddr + 8u);
    int32_t h = (int32_t)MEM_R32(rangeAddr + 12u);
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || (uint32_t)x >= b->width || (uint32_t)y >= b->height)
        return mpeg_contract_error("sceMpegAvcCsc", "negative or empty source range", SCE_MPEG_ERROR_INVALID_VALUE);
    /* Firmware clipping is not measured; clip positive source overflow and reject invalid origins. */
    if ((uint32_t)w > b->width - (uint32_t)x) w = (int32_t)(b->width - (uint32_t)x);
    if ((uint32_t)h > b->height - (uint32_t)y) h = (int32_t)(b->height - (uint32_t)y);
    if ((uint32_t)x >= frameWidth)
        return mpeg_contract_error("sceMpegAvcCsc", "range starts beyond destination stride", SCE_MPEG_ERROR_INVALID_VALUE);
    if ((uint32_t)w > frameWidth - (uint32_t)x)
        return mpeg_contract_error("sceMpegAvcCsc", "range exceeds destination stride", SCE_MPEG_ERROR_INVALID_VALUE);
    uint32_t bpp = ctx->pixelMode == 3 ? 4u : 2u;
    SrGuestRectSpan span;
    if (!sr_guest_rect_writable(dest, (uint32_t)x, (uint32_t)y, frameWidth,
                                (uint32_t)w, (uint32_t)h, bpp, &span))
        return mpeg_contract_error("sceMpegAvcCsc", "destination rectangle is out of bounds", SCE_MPEG_ERROR_INVALID_VALUE);
    for (uint32_t row = 0; row < (uint32_t)h; row++) {
        const uint8_t *px = b->rgba + ((size_t)((uint32_t)y + row) * b->width + (uint32_t)x) * 4u;
        uint32_t rowAddr = span.first + row * span.row_pitch;
        uint8_t *out = (uint8_t *)SR_HOST(rowAddr);
        for (uint32_t col = 0; col < (uint32_t)w; col++, px += 4u) {
            unsigned r = px[0], g = px[1], bl = px[2];
            uint16_t v16;
            switch (ctx->pixelMode) {
                case 0:
                    v16 = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((bl >> 3) << 11));
                    memcpy(out + col * 2u, &v16, 2u);
                    break;
                case 1:
                    v16 = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((bl >> 3) << 10) | 0x8000u);
                    memcpy(out + col * 2u, &v16, 2u);
                    break;
                case 2:
                    v16 = (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((bl >> 4) << 8) | 0xF000u);
                    memcpy(out + col * 2u, &v16, 2u);
                    break;
                default:
                    memcpy(out + col * 4u, px, 4u);
                    break;
            }
        }
        extern void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
        sr_gpu_vram_dirty(rowAddr, span.row_bytes);
    }
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcCsc #%d ycbcr=0x%x dest=0x%x fw=%u range=%u,%u %ux%u\n",
                    n, buf, dest, frameWidth, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
    }
    return 0;
}

static int mpeg_stream_index(const Mpeg *ctx, uint32_t sid) {
    if (!ctx) return -1;
    for (int i = 0; i < 8; i++) if (ctx->streams[i].used && ctx->streams[i].sid == sid) return i;
    return -1;
}

/* LPCM ES and output sizes are public constants; PCM access-unit production remains unsupported. */
uint32_t mpeg_query_pcm_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr) {
    if (!mpeg_find(mpegAddr)) return (uint32_t)-1;
    if (!esSizeAddr || !outSizeAddr || (esSizeAddr & 3u) || (outSizeAddr & 3u) ||
        !sr_guest_span_writable(esSizeAddr, 4u) || !sr_guest_span_writable(outSizeAddr, 4u))
        return mpeg_contract_error("sceMpegQueryPcmEsSize", "unaligned or truncated size pointer", SCE_MPEG_ERROR_INVALID_VALUE);
    MEM_W32(esSizeAddr, MPEG_PCM_ES_SIZE);
    MEM_W32(outSizeAddr, MPEG_PCM_OUTPUT_SIZE);
    return 0;
}

uint32_t mpeg_get_pcm_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!auAddr || (auAddr & 3u) || !sr_guest_span_writable(auAddr, 24u) ||
        !attrAddr || (attrAddr & 3u) || !sr_guest_span_writable(attrAddr, 4u))
        return mpeg_contract_error("sceMpegGetPcmAu", "unaligned or truncated AU pointer", SCE_MPEG_ERROR_INVALID_VALUE);
    int index = mpeg_stream_index(ctx, sid);
    if (index < 0 || ctx->streams[index].type != MPEG_PCM_STREAM)
        return mpeg_contract_error("sceMpegGetPcmAu", "unknown or non-PCM stream", SCE_MPEG_ERROR_INVALID_VALUE);
    au_write_pts(auAddr, 0, -1);
    au_write_pts(auAddr, 8, -1);
    MEM_W32(auAddr + 16u, 0u);
    MEM_W32(auAddr + 20u, MPEG_PCM_ES_SIZE);
    MEM_W32(attrAddr, 0u);
    return mpeg_contract_error("sceMpegGetPcmAu", "PCM decode is not implemented", SCE_MPEG_ERROR_NO_DATA);
}

/* sceMpegChangeGetAuMode(mpeg, stream, mode): decode mode is retained; skip mode has no measured
 * queue/timestamp contract and therefore fails closed. */
uint32_t mpeg_change_get_au_mode(uint32_t mpegAddr, uint32_t sid, uint32_t mode) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (mode != MPEG_AU_MODE_DECODE && mode != MPEG_AU_MODE_SKIP)
        return mpeg_contract_error("sceMpegChangeGetAuMode", "unsupported AU mode", SCE_MPEG_ERROR_INVALID_VALUE);
    int index = mpeg_stream_index(ctx, sid);
    if (index < 0)
        return mpeg_contract_error("sceMpegChangeGetAuMode", "unknown stream", SCE_MPEG_ERROR_INVALID_VALUE);
    if (mode == MPEG_AU_MODE_SKIP)
        return mpeg_contract_error("sceMpegChangeGetAuMode", "skip-mode queue and timestamp semantics are unmeasured", (uint32_t)-1);
    ctx->streams[index].auMode = MPEG_AU_MODE_DECODE;
    return 0;
}

uint32_t mpeg_atrac_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t bufferAddr, uint32_t init) {
    (void)auAddr; (void)bufferAddr; (void)init;
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    ctx->audioPts += audioTimestampStep;
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 16 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAtracDecode #%d audioPts=%lld\n", n, (long long)ctx->audioPts);
    }
    return 0;
}
uint32_t mpeg_avc_decode_stop(uint32_t mpegAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t statusAddr) {
    (void)frameWidth; (void)bufferAddr;
    if (statusAddr) MEM_W32(statusAddr, 0);   /* no frames left */
    (void)mpegAddr; return 0;
}

/* sceMpegFlushAllStream(mpeg): reset stream analysis, clear queued packets,
 * and mark streams as needing reset. Public behaviour reference: PSPSDK
 * pspmpeg.h and PPSSPP Core/HLE/sceMpeg.cpp. */
uint32_t mpeg_flush_all_stream(uint32_t mpegAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    ctx->isAnalyzed = 0;
    for (int i = 0; i < 8; i++) {
        if (ctx->streams[i].used) ctx->streams[i].needsReset = 1;
    }
    if (ctx->ringAddr && sr_guest_span_writable(ctx->ringAddr, RB_BYTES)) {
        rb_set(ctx->ringAddr, RB_packetsRead, 0);
        rb_set(ctx->ringAddr, RB_packetsWritePos, 0);
        rb_set(ctx->ringAddr, RB_packetsAvail, 0);
    }
    return 0;
}

/* sceMpegAvcDecodeFlush(mpeg): clear queued video decoding state and reset
 * the video timestamp to stream start. Public behaviour reference: PSPSDK
 * pspmpeg.h and PPSSPP Core/HLE/sceMpeg.cpp. */
uint32_t mpeg_avc_decode_flush(uint32_t mpegAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    ctx->videoPts = ctx->firstTimestamp;
    ctx->videoEnd = 0;
    return 0;
}
