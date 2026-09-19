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

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#ifdef SR_SDL3VK
/* Portable H.264 decode backend seam (sr_h264.h): Media Foundation on Windows (h264_mf.c),
 * a null/blank backend elsewhere (h264_null.c), libavcodec droppable in later. */
#include "sr_h264.h"
#endif
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

#define MPEG_AVC_STREAM   0
#define MPEG_ATRAC_STREAM 1
#define MPEG_PCM_STREAM   2
#define MPEG_AUDIO_STREAM 15
#define MPEG_DATA_STREAM  16

#define MPEG_AVC_ES_SIZE   2048
#define MPEG_ATRAC_ES_SIZE 2112

#define MPEG_MEMSIZE_0105 0x10000u

#define SCE_MPEG_ERROR_INVALID_VALUE 0x806100FEu
#define SCE_MPEG_ERROR_BAD_VERSION   0x806100A0u
#define SCE_MPEG_ERROR_NO_DATA       0x80618001u
#define SCE_MPEG_ERROR_NO_MEMORY     0x80610022u

static const int videoTimestampStep = 3003;   /* mpegTimestampPerSecond / 29.97 */
static const int audioTimestampStep = 4180;   /* 2048 samples / 44100 Hz */
static const int64_t mpegTimestampPerSecond = 90000;

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
    int h264;                    /* SDL3 build: Media Foundation H.264 decoder id (-1 = none) */
    int h264Init, h264Frames;
    int defaultFrameWidth, pixelMode;
    int ycbcrWant;   /* pictures requested through sceMpegAvcDecodeYCbCr */
    uint32_t auPacketsDone;   /* ring packets released by real access units */
    int64_t lastAuPts;        /* presentation time of the last real access unit (-1 before any) */
    int esBuffers[2];           /* MPEG_DATA_ES_BUFFERS: allocated-flag per ES buffer */
    /* stream map: small fixed table sid -> (type,num,needsReset) */
    struct { int used, type, num, needsReset; uint32_t sid; } streams[8];
} Mpeg;

static Mpeg s_mpeg[8];
static int s_mpegInit = 0;
static uint32_t s_streamIdGen = 1;

static Mpeg *mpeg_find(uint32_t mpegAddr) {
    if (!mpegAddr) return 0;
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
    ctx->used = 1; ctx->handle = h; ctx->ringAddr = ringAddr;
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
    if (ctx) for (int i = 0; i < 8; i++) if (ctx->streams[i].used && ctx->streams[i].sid == sid) ctx->streams[i].used = 0;
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

static uint32_t video_buffer_bytes(uint32_t frameWidth, int pixelMode) {
    uint32_t fw = frameWidth ? frameWidth : 512;
    if (fw > 2048) fw = 512;
    return fw * 272u * (pixelMode == 3 ? 4u : 2u);
}

static void clear_video_buffer(uint32_t ptr, uint32_t frameWidth, int pixelMode) {
    if (!ptr) return;
    uint32_t bytes = video_buffer_bytes(frameWidth, pixelMode);
    for (uint32_t i = 0; i < bytes; i++) MEM_W8(ptr + i, 0);
}

/* AvcDecode(mpeg, auAddr, frameWidth, bufferAddr, initAddr): decode one AVC frame into
 * *bufferAddr. The SDL3 build decodes through Media Foundation (h264_mf.c); otherwise the
 * timestamp-only model runs and leaves the frame blank. */
uint32_t mpeg_avc_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t initAddr) {
    (void)auAddr;
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    g_mpeg_avcdec++;
    if (frameWidth == 0 || frameWidth > 2048)
        frameWidth = ctx->defaultFrameWidth ? (uint32_t)ctx->defaultFrameWidth : 512u;
    uint32_t buffer = bufferAddr ? MEM_R32(bufferAddr) : 0;

    ctx->videoPts += videoTimestampStep;
    /* Report "a frame was produced" (1) every decode; the game keeps feeding/decoding until it has
     * read the whole file, then stops on its own. */
    int gotFrame = 0;
#ifdef SR_SDL3VK
    if (ctx->h264Init && ctx->h264 >= 0 && buffer) {
        int eos = ctx->totalPackets && ctx->fedPackets >= ctx->totalPackets;
        gotFrame = sr_h264_frame(ctx->h264, eos, buffer,
                                 (int)frameWidth, ctx->pixelMode);
        if (gotFrame > 0) ctx->h264Frames++;
    }
#endif
    /* No decoder (or it hasn't produced its first frame yet): clear the destination instead of
     * leaving stale contents, which otherwise appears as moving black bands over uninitialised
     * movie frames. Once frames flow, a miss keeps the previous frame (no black flicker). */
    if (gotFrame <= 0 && !ctx->h264Frames) {
        clear_video_buffer(buffer, frameWidth, ctx->pixelMode);
        if (buffer) {
            extern void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
            sr_gpu_vram_dirty(buffer, video_buffer_bytes(frameWidth, ctx->pixelMode));
        }
    }
    if (initAddr) MEM_W32(initAddr, 1);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcDecode #%d buf=0x%x fw=%u pts=%lld dec=%d frames=%d\n",
                    n, buffer, frameWidth, (long long)ctx->videoPts, gotFrame, ctx->h264Frames);
    }
    return 0;
}
/* ---- YCbCr decode path (sceMpegAvc*YCbCr / sceMpegAvcCsc) ----
 * Project-authored boundary for the guest libpsmfplayer, which decodes each access unit into an
 * opaque "YCbCr" buffer, may copy it to another YCbCr buffer, and later colour-converts one into
 * its display buffer (observed in the title's own libpsmfplayer: QueryYCbCrSize -> InitYCbCr ->
 * DecodeYCbCr -> CopyYCbCr -> Csc). The guest never reads the buffer contents, so the host keeps
 * the decoded picture for each buffer address itself: DecodeYCbCr decodes the next picture into
 * that buffer's host store, CopyYCbCr copies a store, and Csc converts a store into the
 * destination in the context's pixel mode. Stores are RGBA8888, 512 x 272. */
#define YCBCR_W 512
#define YCBCR_H 272
typedef struct { uint32_t mpeg, buf; uint8_t *rgba; int valid; } YcbcrBuf;
static YcbcrBuf s_ycbcr[16];

static YcbcrBuf *ycbcr_find(uint32_t mpegAddr, uint32_t buf) {
    for (int i = 0; i < 16; i++)
        if (buf && s_ycbcr[i].buf == buf && s_ycbcr[i].mpeg == mpegAddr) return &s_ycbcr[i];
    return NULL;
}

static YcbcrBuf *ycbcr_slot(uint32_t mpegAddr, uint32_t buf) {
    YcbcrBuf *b = ycbcr_find(mpegAddr, buf);
    if (b || !buf) return b;
    for (int i = 0; i < 16; i++) if (!s_ycbcr[i].buf) { b = &s_ycbcr[i]; break; }
    if (!b) b = &s_ycbcr[0];   /* all in use: recycle the first slot */
    b->mpeg = mpegAddr; b->buf = buf; b->valid = 0;
    if (!b->rgba) b->rgba = (uint8_t *)calloc(YCBCR_W * YCBCR_H, 4);
    return b->rgba ? b : NULL;
}

/* 4:2:0 planar size (Y plane plus two quarter-size chroma planes) plus a 128-byte header. */
uint32_t mpeg_avc_query_ycbcr_size(uint32_t mpegAddr, uint32_t mode, uint32_t width,
                                   uint32_t height, uint32_t resultAddr) {
    (void)mode;
    if (!mpeg_find(mpegAddr)) return (uint32_t)-1;
    if (!width || !height || width > 4096 || height > 4096 || (width & 15) || (height & 15))
        return SCE_MPEG_ERROR_INVALID_VALUE;
    uint32_t size = (width / 2u) * (height / 2u) * 6u + 128u;
    if (resultAddr) MEM_W32(resultAddr, size);
    return 0;
}

uint32_t mpeg_avc_init_ycbcr(uint32_t mpegAddr, uint32_t mode, uint32_t width, uint32_t height,
                             uint32_t buf) {
    (void)mode; (void)width; (void)height;
    if (!mpeg_find(mpegAddr)) return (uint32_t)-1;
    YcbcrBuf *b = ycbcr_slot(mpegAddr, buf);
    if (b) b->valid = 0;
    return 0;
}

/* sceMpegAvcDecodeMode(mpeg, mode*): mode[1] is the output pixel format (0..3). */
uint32_t mpeg_avc_decode_mode(uint32_t mpegAddr, uint32_t modeAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!modeAddr) return SCE_MPEG_ERROR_INVALID_VALUE;
    uint32_t pm = MEM_R32(modeAddr + 4);
    if (pm > 3) return SCE_MPEG_ERROR_INVALID_VALUE;
    ctx->pixelMode = (int)pm;
    return 0;
}

/* sceMpegAvcDecodeYCbCr(mpeg, au, ycbcr*, init*): like sceMpegAvcDecode, the buffer argument
 * points at the YCbCr buffer address. */
uint32_t mpeg_avc_decode_ycbcr(uint32_t mpegAddr, uint32_t auAddr, uint32_t bufPtr, uint32_t initAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    g_mpeg_avcdec++;
    ctx->videoPts += videoTimestampStep;
    uint32_t buf = bufPtr ? MEM_R32(bufPtr) : 0u;
    YcbcrBuf *b = ycbcr_slot(mpegAddr, buf);
    int got = 0;
#ifdef SR_SDL3VK
    if (b && ctx->h264Init && ctx->h264 >= 0) {
        int eos = ctx->totalPackets && ctx->fedPackets >= ctx->totalPackets;
        /* One picture per request: pull until the decoder's output count catches up with the
         * requests (a low-latency decoder can hold pictures back), keeping the newest. */
        ctx->ycbcrWant++;
        while (ctx->h264Frames < ctx->ycbcrWant) {
            int r = sr_h264_frame_host(ctx->h264, eos, b->rgba, YCBCR_W, YCBCR_W * 4);
            if (r <= 0) break;
            got = r; b->valid = 1; ctx->h264Frames++;
        }
    }
#endif
    if (initAddr) MEM_W32(initAddr, 1);   /* a picture is ready in the buffer */
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcDecodeYCbCr #%d au=0x%x ycbcr=0x%x dec=%d frames=%d pts=%lld\n",
                    n, auAddr, buf, got, ctx->h264Frames, (long long)ctx->videoPts);
    }
    return 0;
}

uint32_t mpeg_avc_decode_stop_ycbcr(uint32_t mpegAddr, uint32_t buf, uint32_t statusAddr) {
    (void)buf;
    if (!mpeg_find(mpegAddr)) return (uint32_t)-1;
    if (statusAddr) MEM_W32(statusAddr, 0);   /* no buffered pictures remain */
    return 0;
}

uint32_t mpeg_avc_copy_ycbcr(uint32_t mpegAddr, uint32_t dst, uint32_t src) {
    if (!mpeg_find(mpegAddr)) return (uint32_t)-1;
    YcbcrBuf *from = ycbcr_find(mpegAddr, src);
    YcbcrBuf *to = ycbcr_slot(mpegAddr, dst);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcCopyYCbCr #%d dst=0x%x [dst]=0x%x src=0x%x [src]=0x%x from=%d\n",
                    n, dst, sr_guest_span_readable(dst, 4) ? MEM_R32(dst) : 0u,
                    src, sr_guest_span_readable(src, 4) ? MEM_R32(src) : 0u, from ? from->valid : -1);
    }
    if (from && to && from != to) {
        if (from->valid) memcpy(to->rgba, from->rgba, (size_t)YCBCR_W * YCBCR_H * 4);
        to->valid = from->valid;
    }
    return 0;
}

/* sceMpegAvcCsc(mpeg, ycbcr, range*, frameWidth, dest): range is {x, y, w, h} in pixels. */
uint32_t mpeg_avc_csc(uint32_t mpegAddr, uint32_t buf, uint32_t rangeAddr, uint32_t frameWidth,
                      uint32_t dest) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!rangeAddr || !dest) return SCE_MPEG_ERROR_INVALID_VALUE;
    if (frameWidth == 0 || frameWidth > 2048)
        frameWidth = ctx->defaultFrameWidth ? (uint32_t)ctx->defaultFrameWidth : 512u;
    uint32_t x0 = MEM_R32(rangeAddr), y0 = MEM_R32(rangeAddr + 4);
    uint32_t w = MEM_R32(rangeAddr + 8), h = MEM_R32(rangeAddr + 12);
    if (x0 >= YCBCR_W || y0 >= YCBCR_H) return SCE_MPEG_ERROR_INVALID_VALUE;
    if (w > YCBCR_W - x0) w = YCBCR_W - x0;
    if (h > YCBCR_H - y0) h = YCBCR_H - y0;
    if (w > frameWidth) w = frameWidth;
    uint32_t bpp = ctx->pixelMode == 3 ? 4u : 2u;
    YcbcrBuf *b = ycbcr_find(mpegAddr, buf);
    if (!b || !b->valid) {
        if (!ctx->h264Frames) clear_video_buffer(dest, frameWidth, ctx->pixelMode);
    } else {
        for (uint32_t y = 0; y < h; y++) {
            uint32_t row = dest + ((y0 + y) * frameWidth + x0) * bpp;
            if (!sr_guest_span_writable(row, w * bpp)) break;
            const uint8_t *px = b->rgba + ((size_t)(y0 + y) * YCBCR_W + x0) * 4u;
            uint8_t *out = (uint8_t *)SR_HOST(row);
            for (uint32_t x = 0; x < w; x++, px += 4) {
                unsigned r = px[0], g = px[1], bl = px[2];
                uint16_t v16;
                switch (ctx->pixelMode) {
                    case 0: v16 = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((bl >> 3) << 11));
                            memcpy(out + x * 2u, &v16, 2); break;
                    case 1: v16 = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((bl >> 3) << 10) | 0x8000u);
                            memcpy(out + x * 2u, &v16, 2); break;
                    case 2: v16 = (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((bl >> 4) << 8) | 0xF000u);
                            memcpy(out + x * 2u, &v16, 2); break;
                    default: memcpy(out + x * 4u, px, 4); break;
                }
            }
        }
    }
    extern void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
    sr_gpu_vram_dirty(dest, video_buffer_bytes(frameWidth, ctx->pixelMode));
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcCsc #%d ycbcr=0x%x valid=%d dest=0x%x fw=%u range=%u,%u %ux%u\n",
                    n, buf, b ? b->valid : -1, dest, frameWidth, x0, y0, w, h);
    }
    return 0;
}

/* LPCM audio streams: this model has no PCM decode; report the standard LPCM access-unit size and
 * no data, which the guest player treats as "no PCM stream present". */
uint32_t mpeg_query_pcm_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr) {
    if (!mpeg_find(mpegAddr)) return (uint32_t)-1;
    if (!esSizeAddr || !outSizeAddr) return SCE_MPEG_ERROR_INVALID_VALUE;
    MEM_W32(esSizeAddr, 320u);
    MEM_W32(outSizeAddr, 320u);
    return 0;
}

uint32_t mpeg_get_pcm_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr) {
    (void)sid; (void)auAddr; (void)attrAddr;
    if (!mpeg_find(mpegAddr)) return (uint32_t)-1;
    return SCE_MPEG_ERROR_NO_DATA;
}

/* sceMpegChangeGetAuMode(mpeg, stream, mode): 0 = decode, 1 = skip. Access units are produced
 * the same way in both modes here; skipping only means the guest does not decode them. */
uint32_t mpeg_change_get_au_mode(uint32_t mpegAddr, uint32_t sid, uint32_t mode) {
    (void)sid;
    if (!mpeg_find(mpegAddr)) return (uint32_t)-1;
    if (mode > 1) return SCE_MPEG_ERROR_INVALID_VALUE;
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
