// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* *
 * This is a faithful port of PPSSPP's sceMpeg control flow: PSMF header analysis, ring-buffer
 * packet accounting, MPEG handle/context creation, stream registration, and the access-unit
 * getters with timestamp progression and end-of-stream detection. PPSSPP's MediaEngine is not
 * ported: the media-engine timestamp is modelled directly (video advances by videoTimestampStep
 * per decoded frame and ends at the stream's last timestamp), so a game's movie-playback loop
 * runs and *completes* identically. AVC pictures are decoded through the backend-neutral
 * sr_h264.h seam by whichever decoder the build selected; sceMpegAvcDecode clears the
 * destination until the first picture arrives, and without a usable decoder it fails closed
 * with the backend-unavailable error. The registered ATRAC3+ stream is demultiplexed from the
 * same ring packets and decoded by the project's ATRAC3+ bridge, one frame per sceMpegAtracDecode
 * (see "ATRAC3+ access units" below); the audio AU clock stays the media-engine model.
 *
 * This file is GPL-2.0-or-later because it is derived from PPSSPP; that licence covers this
 * runtime source only and says nothing about the user's game.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "recomp.h"
#include "sr_h264.h"
#include "psmf_producer.h"
#include "atrac3p_bridge.h"
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
#define MPEG_ATRAC_ES_OUTPUT_SIZE 8192u   /* 2048 stereo s16 samples per sceMpegAtracDecode */
#define MPEG_PCM_ES_SIZE   320
#define MPEG_PCM_OUTPUT_SIZE 320

/* Public MPEG constants and return families are recorded in PSPSDK's src/mpeg/pspmpeg.h.  The
 * mode-1 YCbCr buffer is measured (the YCbCr section below); the other modes remain a #302
 * physical-PSP hardware-oracle gap. */
#define MPEG_MEMSIZE_0105 0x10000u
#define MPEG_MAX_DIMENSION 4096u
#define MPEG_AU_MODE_DECODE 0u
#define MPEG_AU_MODE_SKIP   1u

#define SCE_MPEG_ERROR_INVALID_VALUE 0x806101FEu   /* MEASURED (PSP-MPEG-001); was 0x806100FE */
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
#define MPEG_ATRAC_FIFO_DEPTH 64u   /* demuxed ATRAC3+ frames waiting for GetAtracAu (~3 s) */
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
    int h264DemuxFailed, h264DemuxErrorReported;
    int64_t videoPts, audioPts;  /* media-engine timestamp model */
    int videoEnd, audioEnd;
    uint32_t totalPackets;       /* streamSize/packetSize: whole-movie packet count */
    uint32_t fedPackets;         /* cumulative packets the game has put into the ring */
    uint32_t headerAddr;         /* guest address of the PSMF header passed to QueryStreamOffset */
    int h264;                    /* host H.264 backend id (-1 = none) */
    int h264Init, h264Frames;
    int avcFrameStatus;         /* 1 when the last AVC decode call produced a picture */
    int defaultFrameWidth, pixelMode;
    uint32_t streamWidth, streamHeight;
    int ycbcrWant;   /* pictures requested through sceMpegAvcDecodeYCbCr */
    uint32_t auPacketsDone;   /* ring packets released by real access units */
    int64_t lastAuPts;        /* presentation time of the last real access unit (-1 before any) */
    int esBuffers[2];           /* MPEG_DATA_ES_BUFFERS: allocated-flag per ES buffer */
    /* stream map: small fixed table sid -> (type,num,needsReset,auMode) */
    struct { int used, type, num, needsReset, auMode; uint32_t sid; } streams[8];
    /* ATRAC3+ access units: an audio-only demux of the fed ring bytes, its frames waiting in a
     * FIFO, the frame GetAtracAu holds for the next AtracDecode, and the decoder. */
    int audioStreamNum;          /* the registered ATRAC stream (its PS1 sub-stream id) */
    SrPsmfProducer *audioDemux;
    int audioDemuxFailed, audioDemuxEos;
    uint32_t audioFedPackets;    /* ring packets fed to the demux since it (re)started */
    struct { uint8_t *data; uint32_t bytes; } audioFifo[MPEG_ATRAC_FIFO_DEPTH];
    uint32_t audioFifoHead, audioFifoCount;
    uint8_t *audioHeld;
    uint32_t audioHeldBytes;
    Atrac3pBridge *atrac;
    int atracAlign, atracChannels;
    int16_t *audioPcm;
    uint64_t audioAusOut, audioDecoded, audioRejected, audioSkipped;
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
 * the reserved region. The rest of the marshalling is zeroed state, inherited
 * $gp, $ra = 0, 0xe4 VFPU prefixes and a full caller restore. */
/* ---- ATRAC3+ access units (sceMpegGetAtracAu / sceMpegAtracDecode) ----
 * Registering an ATRAC stream opens an audio-only streaming PSMF producer over the ring packets
 * the game feeds: RingbufferPut hands it the same bytes as the H.264 demux.  Its frames (one
 * ATRAC3+ frame per access unit: an 8-byte 0x0FD0 header, then the frame) wait in a bounded FIFO.
 * GetAtracAu holds the next one for the following AtracDecode, which decodes it with the
 * project's ATRAC3+ bridge into the guest's 8192-byte block (2048 stereo s16 samples; a mono
 * stream is duplicated into both channels, as the host player does).  With no frame ready
 * GetAtracAu reports NO_DATA, and AtracDecode without a held frame, or with a frame the decoder
 * refuses, returns an error: no block is filled with silence that no decoder produced.  The AU
 * clock is the media-engine model as before (firstTimestamp + 4180 per decoded block). */
void sr_psmf_av_note_audio_delivery(uint32_t first, uint32_t bytes, int64_t pts, int pts_valid);
static uint32_t mpeg_contract_error(const char *api, const char *boundary, uint32_t error);

static void mpeg_audio_report(Mpeg *ctx, const char *what) {
    static int reported;
    if (reported) return;
    reported = 1;
    const char *why = ctx && ctx->audioDemux ? sr_psmf_producer_fail_reason(ctx->audioDemux) : NULL;
    fprintf(stderr, "MPEG_ATRAC: %s%s%s\n", what, why ? ": " : "", why ? why : "");
}

static void mpeg_audio_fifo_clear(Mpeg *ctx) {
    while (ctx->audioFifoCount) {
        free(ctx->audioFifo[ctx->audioFifoHead].data);
        ctx->audioFifo[ctx->audioFifoHead].data = NULL;
        ctx->audioFifoHead = (ctx->audioFifoHead + 1u) % MPEG_ATRAC_FIFO_DEPTH;
        ctx->audioFifoCount--;
    }
    ctx->audioFifoHead = 0;
    free(ctx->audioHeld);
    ctx->audioHeld = NULL;
    ctx->audioHeldBytes = 0;
}

static void mpeg_audio_close(Mpeg *ctx) {
    if (!ctx) return;
    mpeg_audio_fifo_clear(ctx);
    sr_psmf_producer_close(ctx->audioDemux);
    ctx->audioDemux = NULL;
    atrac3p_bridge_destroy(ctx->atrac);
    ctx->atrac = NULL;
    ctx->atracAlign = ctx->atracChannels = 0;
    free(ctx->audioPcm);
    ctx->audioPcm = NULL;
    ctx->audioDemuxFailed = ctx->audioDemuxEos = 0;
    ctx->audioFedPackets = 0;
}

/* The registered stream's frames carry PS1 sub-stream id 0x00 + its number. */
static void mpeg_audio_open(Mpeg *ctx, int streamNum) {
    mpeg_audio_close(ctx);
    ctx->audioStreamNum = streamNum;
    ctx->audioDemux = sr_psmf_producer_open_stream(0);
    if (streamNum < 0 || streamNum > 0x0f || !ctx->audioDemux ||
        !sr_psmf_producer_select_streams(ctx->audioDemux, SR_PSMF_STREAM_NONE, 0)) {
        ctx->audioDemuxFailed = 1;
        mpeg_audio_report(ctx, "the registered ATRAC stream cannot be demultiplexed");
    }
}

/* The game refills the ring from another position (sceMpegFlushAllStream). */
static void mpeg_audio_restart(Mpeg *ctx) {
    if (!ctx || !ctx->audioDemux) return;
    mpeg_audio_fifo_clear(ctx);
    sr_psmf_producer_reset(ctx->audioDemux);
    if (ctx->atrac) (void)atrac3p_bridge_reset(ctx->atrac);
    ctx->audioDemuxFailed = sr_psmf_producer_failed(ctx->audioDemux);
    ctx->audioDemuxEos = 0;
    ctx->audioFedPackets = 0;
}

/* Parse what the demux holds and move the registered stream's frames into the FIFO until it is
 * full; the demux keeps the remaining bytes until GetAtracAu makes room.  Each round parses a
 * bounded number of elements and stops once the parser makes no progress. */
static void mpeg_audio_pump(Mpeg *ctx) {
    if (!ctx->audioDemux || ctx->audioDemuxFailed) return;
    while (ctx->audioFifoCount < MPEG_ATRAC_FIFO_DEPTH) {
        SrPsmfAu au;
        if (sr_psmf_producer_pop(ctx->audioDemux, SR_PSMF_AU_AUDIO, &au)) {
            if (au.stream_id != (uint8_t)ctx->audioStreamNum || !au.data || !au.size) {
                ctx->audioSkipped++;
                sr_psmf_au_release(&au);
                continue;
            }
            uint8_t *copy = (uint8_t *)malloc(au.size);
            if (!copy) {
                sr_psmf_au_release(&au);
                ctx->audioDemuxFailed = 1;
                mpeg_audio_report(ctx, "no host memory for an ATRAC frame");
                return;
            }
            memcpy(copy, au.data, au.size);
            uint32_t tail = (ctx->audioFifoHead + ctx->audioFifoCount) % MPEG_ATRAC_FIFO_DEPTH;
            ctx->audioFifo[tail].data = copy;
            ctx->audioFifo[tail].bytes = au.size;
            ctx->audioFifoCount++;
            ctx->audioAusOut++;
            sr_psmf_au_release(&au);
            continue;
        }
        uint64_t before = sr_psmf_producer_stream_cursor(ctx->audioDemux);
        int produced = sr_psmf_producer_pump(ctx->audioDemux, 64u);
        if (sr_psmf_producer_failed(ctx->audioDemux)) {
            ctx->audioDemuxFailed = 1;
            mpeg_audio_report(ctx, "the ATRAC stream could not be demultiplexed");
            return;
        }
        if (produced <= 0 && sr_psmf_producer_stream_cursor(ctx->audioDemux) == before) return;
    }
}

static void mpeg_audio_feed(Mpeg *ctx, const uint8_t *bytes, uint32_t len) {
    if (!ctx->audioDemux || ctx->audioDemuxFailed || ctx->audioDemuxEos) return;
    if (sr_psmf_producer_feed_stream(ctx->audioDemux, bytes, len, 0) < 0) {
        ctx->audioDemuxFailed = 1;
        mpeg_audio_report(ctx, "the ATRAC stream demux refused the fed packets");
        return;
    }
    ctx->audioFedPackets += len / MPEG_PACKET_SIZE;
    if (ctx->totalPackets && ctx->audioFedPackets >= ctx->totalPackets) {
        /* The whole movie has been fed since the demux (re)started: it may close its last frame. */
        ctx->audioDemuxEos = 1;
        if (sr_psmf_producer_feed_stream(ctx->audioDemux, NULL, 0, 1) < 0) {
            ctx->audioDemuxFailed = 1;
            mpeg_audio_report(ctx, "the ATRAC stream ended inside an incomplete packet");
            return;
        }
    }
    mpeg_audio_pump(ctx);
}

/* Decode one held frame into ctx->audioPcm as 2048 stereo samples.  Returns 0 or the error the
 * caller reports. */
static uint32_t mpeg_audio_decode_frame(Mpeg *ctx, const uint8_t *frame, uint32_t bytes, uint32_t init) {
    if (bytes <= 8u || frame[0] != 0x0fu || frame[1] != 0xd0u)
        return mpeg_contract_error("sceMpegAtracDecode", "the access unit is not an ATRAC3+ frame",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    uint32_t frame_size = (uint32_t)(((frame[2] & 3u) << 8) | ((uint32_t)frame[3] * 8u)) + 0x10u;
    if (frame_size != bytes)
        return mpeg_contract_error("sceMpegAtracDecode", "the frame header disagrees with the access unit",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    /* Bytes 2-3 are the low half of the ATRAC3+ codec word (sample-rate code, channel id, frame
     * size; PSMF movies carry 0x285C, 44.1 kHz stereo), so the decoder is set up from the frame
     * itself.  A guest PSMF player never hands sceMpeg the PSMF header, so the stream table is
     * not available here. */
    int channels = (frame[2] >> 2) & 7;
    if (channels < 1 || channels > 2)
        return mpeg_contract_error("sceMpegAtracDecode", "the frame header names no mono or stereo layout",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    int data = (int)(frame_size - 8u);
    if (!ctx->atrac || ctx->atracAlign != data || ctx->atracChannels != channels) {
        atrac3p_bridge_destroy(ctx->atrac);
        ctx->atrac = NULL;
        if (atrac3p_bridge_create(channels, data, &ctx->atrac) != 0) {
            ctx->atrac = NULL;
            return mpeg_contract_error("sceMpegAtracDecode", "no ATRAC3+ decoder for this frame size",
                                       SCE_MPEG_ERROR_INVALID_VALUE);
        }
        ctx->atracAlign = data;
        ctx->atracChannels = channels;
    } else if (init) {
        (void)atrac3p_bridge_reset(ctx->atrac);
    }
    if (!ctx->audioPcm &&
        !(ctx->audioPcm = (int16_t *)malloc(MPEG_ATRAC_ES_OUTPUT_SIZE)))
        return mpeg_contract_error("sceMpegAtracDecode", "no host memory for the decoded block",
                                   SCE_MPEG_ERROR_NO_MEMORY);
    int samples = 0;
    if (atrac3p_bridge_decode(ctx->atrac, frame + 8, data, ctx->audioPcm, &samples) != 0 ||
        samples != (int)(MPEG_ATRAC_ES_OUTPUT_SIZE / 4u)) {
        ctx->audioRejected++;
        return mpeg_contract_error("sceMpegAtracDecode", "the ATRAC3+ frame did not decode to one block",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    }
    if (channels == 1)
        for (int i = samples - 1; i >= 0; i--) {
            ctx->audioPcm[i * 2] = ctx->audioPcm[i];
            ctx->audioPcm[i * 2 + 1] = ctx->audioPcm[i];
        }
    return 0;
}

static uint32_t call_guest3(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t frame_sp = 0;
    int frame = -1;
    if (!s || !fn) return 0;
    if (!sr_nested_frame_acquire(sched_current_uid(), &frame_sp, &frame)) {
        fprintf(stderr, "MPEG_CALL_GUEST: fn=0x%08" PRIx32 " refused -- no nested guest-call frame "
                        "available (cur_uid=0x%" PRIx32 ")\n", fn, sched_current_uid());
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
    s->pc = fn;   /* the callback's first pc is its entry */
    atomic_store_explicit(&sr_timeslice, 20000, memory_order_relaxed);
    SrAotReturnBoundaryScope boundary_scope;
    sr_aot_return_boundary_scope_begin(&boundary_scope);
    dispatch(s, fn);
    uint32_t ret = s->r[2];
    int boundary_scope_failed = sr_aot_return_boundary_scope_end(
        s, &boundary_scope, "MPEG guest callback");
    uint32_t callback_flow_kind = s->flow_kind;
    uint32_t callback_flow_target = s->flow_target;
    memcpy(s, &save, sizeof(CpuState));
    if (boundary_scope_failed || callback_flow_kind == SR_FLOW_FATAL) {
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = callback_flow_target;
    }
    /* Returning to the interrupted context ends any ll..sc window it had open. */
    sr_cpu_link_clear(s);
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

/* The presentation time the guest holds for an access unit (the inverse of au_write_pts);
 * -1 when the AU carries none. */
static int64_t au_read_pts(uint32_t auAddr) {
    uint64_t v = ((uint64_t)MEM_R32(auAddr) << 32) | MEM_R32(auAddr + 4u);
    return (int64_t)v;
}

/* Presentation-boundary measurement (hle.c, SR_MPEGAV): a decoded picture now sits in guest
 * memory at [first, first + bytes) with this 90 kHz presentation time. */
void sr_psmf_av_note_video_delivery(uint32_t first, uint32_t bytes, int64_t pts, int pts_valid);
int sr_psmf_av_active(void);   /* SR_MPEGAV: the picture hashes below are kept only for it */

static uint32_t h264_demux_error(Mpeg *ctx, uint32_t auAddr) {
    if (ctx && !ctx->h264DemuxErrorReported) {
        ctx->h264DemuxErrorReported = 1;
        fprintf(stderr, "H264_CONTRACT: sceMpegGetAvcAu: program-stream demux rejected input\n");
    }
    au_write_pts(auAddr, 0, -1);
    au_write_pts(auAddr, 8, -1);
    return SCE_MPEG_ERROR_NO_DATA;
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
    for (int i = 0; i < 8; i++) if (s_mpeg[i].used) {
        ycbcr_reset_mpeg(s_mpeg[i].guestAddr);
        mpeg_audio_close(&s_mpeg[i]);
    }
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
    mpeg_audio_close(ctx);
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
    else if (streamType == MPEG_ATRAC_STREAM || streamType == MPEG_AUDIO_STREAM) {
        ctx->atracRegistered = 1;
        mpeg_audio_open(ctx, (int)streamNum);
    }
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
 * 1-based ES-buffer index (non-zero) the game treats as a valid handle, or 0 if none is free.
 * A game reads 0 as a failed allocation and tears the movie down. */
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
        if (ctx->streams[i].type == MPEG_ATRAC_STREAM || ctx->streams[i].type == MPEG_AUDIO_STREAM)
            mpeg_audio_close(ctx);
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
            fprintf(stderr, "mpeg: ring fill callback 0x%08" PRIx32 " is not recompiled code "
                    "(ring=0x%08" PRIx32 " data=0x%08" PRIx32 " cbArg=0x%08" PRIx32 " mpeg=0x%08" PRIx32 ") -- runtime-loaded code?\n",
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
            !sr_guest_span_readable_current(dst, gotBytes))
            return 0;

#if defined(SR_SDL3VK) || defined(SR_MPEG_MEDIA_SELFTEST)
        if (ctx && got) {
            if (!ctx->h264Init) { ctx->h264Init = 1; ctx->h264 = sr_h264_create(); }
            if (ctx->h264 >= 0 &&
                sr_h264_feed(ctx->h264, (const uint8_t *)SR_HOST(dst), gotBytes) < 0)
                ctx->h264DemuxFailed = 1;
        }
#endif
        if (ctx && got) mpeg_audio_feed(ctx, (const uint8_t *)SR_HOST(dst), gotBytes);

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
            fprintf(stderr, "MpegRingbufferPut ring=0x%" PRIx32 " want=%" PRIu32 "/%" PRIu32 " cb=0x%" PRIx32 " -> %" PRIu32 " avail=%" PRIu32 "/%" PRIu32 "\n",
                    ring, numPackets, available, cb, addedTotal, rb_get(ring, RB_packetsAvail), total);
    }
    return addedTotal;
}

static int mpeg_stream_index(const Mpeg *ctx, uint32_t sid);
/* sceMpegChangeGetAuMode(..., MPEG_AU_MODE_SKIP): the stream's AU getters hand out the clock and
 * consume no stream data until the mode returns to decode (PPSSPP's streamIgnoreMap, c9e2afb6).
 * UNMEASURED on this project's PSP; a guest PSMF player switches its streams this way when it
 * resets its decoders. */
static int mpeg_stream_skipped(const Mpeg *ctx, uint32_t sid) {
    int index = mpeg_stream_index(ctx, sid);
    return index >= 0 && ctx->streams[index].auMode == (int)MPEG_AU_MODE_SKIP;
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
    if (ctx->h264DemuxFailed) return h264_demux_error(ctx, auAddr);
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
    if (mpeg_stream_skipped(ctx, sid)) {
        int64_t clock = ctx->videoPts + ctx->firstTimestamp;
        au_write_pts(auAddr, 0, clock);
        au_write_pts(auAddr, 8, clock - videoTimestampStep);
        MEM_W32(auAddr + 16, (uint32_t)num);
        if (attrAddr) MEM_W32(attrAddr, 1);
        return 0;
    }
    uint32_t release = 1;   /* timestamp model: one packet per access unit */
#if defined(SR_SDL3VK) || defined(SR_MPEG_MEDIA_SELFTEST)
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
        if (r < 0) {
            ctx->h264DemuxFailed = 1;
            return h264_demux_error(ctx, auAddr);
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
            fprintf(stderr, "MpegGetAvcAu #%d pts=%lld avail=%" PRIu32 " end=%d\n",
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
    if (!auAddr || (auAddr & 3u) || !sr_guest_span_writable(auAddr, 24u))
        return mpeg_contract_error("sceMpegGetAtracAu", "invalid AU pointer", (uint32_t)-1);
    int needsReset = 0, num = 0;
    au_stream(mpegAddr, sid, &needsReset, &num);
    /* PSP/PPSSPP's libmpeg clock uses the PSMF presentation origin for ATRAC AUs.  In
     * particular, PPSSPP's sceMpeg implementation documents audioFirstTimestamp as 90000,
     * matching the PSMF first timestamp and the first AVC AU.  A raw private-stream PES PTS is
     * not the player clock origin: using it here made the audio clock title-dependent and moved
     * it ahead of the first video AU, which can make libpsmfplayer reject the video as too early.
     * Keep the demuxer's firstAudioPts for diagnostics, but do not substitute it for the public
     * sceMpeg AU time base. */
#if defined(SR_SDL3VK) || defined(SR_MPEG_MEDIA_SELFTEST)
    /* A context that never saw the PSMF header (a guest PSMF player parses it with scePsmf) takes
     * its presentation origin from the stream's first video picture, as its first sceMpegGetAvcAu
     * would.  Without it the ATRAC units handed out before that carried a clock from 0: title X's
     * guest libpsmfplayer drops audio frames timed more than one block before the movie's start,
     * so its audio began late and every picture was then dropped as too slow. */
    if (!ctx->firstTimestamp && ctx->h264Init && ctx->h264 >= 0) {
        int64_t first = -1;
        if (sr_h264_au_peek_pts(ctx->h264, &first) && first > 0) ctx->firstTimestamp = first;
    }
#endif
    int64_t pts = ctx->audioPts + ctx->firstTimestamp;
    if (mpeg_stream_skipped(ctx, sid)) {
        au_write_pts(auAddr, 0, pts);
        au_write_pts(auAddr, 8, pts);
        MEM_W32(auAddr + 16, (uint32_t)num);
        MEM_W32(auAddr + 20, MPEG_ATRAC_ES_SIZE);
        if (attrAddr) MEM_W32(attrAddr, 0);
        return 0;
    }
    if (!ctx->audioHeld) {
        mpeg_audio_pump(ctx);
        if (ctx->audioFifoCount) {
            ctx->audioHeld = ctx->audioFifo[ctx->audioFifoHead].data;
            ctx->audioHeldBytes = ctx->audioFifo[ctx->audioFifoHead].bytes;
            ctx->audioFifo[ctx->audioFifoHead].data = NULL;
            ctx->audioFifoHead = (ctx->audioFifoHead + 1u) % MPEG_ATRAC_FIFO_DEPTH;
            ctx->audioFifoCount--;
        }
    }
    if (!ctx->audioHeld) {
        /* No frame is demultiplexed yet, the stream has ended, or there is no ATRAC stream to
         * demultiplex: PPSSPP's no-audio-data result, the clock as the pts and no decode time. */
        au_write_pts(auAddr, 0, pts);
        au_write_pts(auAddr, 8, -1);
        if (attrAddr) MEM_W32(attrAddr, 0);
        if (!ctx->audioDemux || ctx->audioDemuxFailed)
            mpeg_audio_report(ctx, "sceMpegGetAtracAu has no demultiplexed ATRAC stream; reporting no data");
        return SCE_MPEG_ERROR_NO_DATA;
    }
    au_write_pts(auAddr, 0, pts);
    au_write_pts(auAddr, 8, pts);
    MEM_W32(auAddr + 20, MPEG_ATRAC_ES_SIZE);
    if (attrAddr) MEM_W32(attrAddr, 0);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 16 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegGetAtracAu #%d pts=%lld frame=%" PRIu32 " queued=%" PRIu32 "\n",
                    n, (long long)pts, ctx->audioHeldBytes, ctx->audioFifoCount);
    }
    return 0;
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
    if (!ptr || !bytes || !sr_guest_span_writable_any_view(ptr, bytes)) return 0;
    /* sr_guest_fill prepares and commits each piece it writes, and splits the span
     * where a swizzled VRAM mirror stops being host-contiguous (#1049). */
    return sr_guest_fill(ptr, 0u, bytes);
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
    if (!buffer || (buffer & 3u) ||
        (height && (!bytes || !sr_guest_span_writable_any_view(buffer, bytes))))
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
    }
    MEM_W32(initAddr, gotFrame > 0 ? 1u : 0u);
    ctx->avcFrameStatus = gotFrame > 0;
    if (gotFrame > 0) {
        int64_t auPts = au_read_pts(auAddr);
        sr_psmf_av_note_video_delivery(buffer, bytes, auPts, auPts >= 0);
    }
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcDecode #%d buf=0x%" PRIx32 " fw=%" PRIu32 " pts=%lld dec=%d frames=%d\n",
                    n, buffer, width, (long long)ctx->videoPts, gotFrame, ctx->h264Frames);
    }
    return gotFrame > 0 ? 0 : SCE_MPEG_ERROR_NO_DATA;
}
/* sceMpegAvcDecodeDetail(mpeg, detail), MEASURED on a PSP-3000 (6.61, 2026-10-31, PSP-MPEG-001,
 * after one picture): 13 words -- 0, 1 (the pictures decoded so far, as PPSSPP has it; only one
 * picture was measured), the frame width and height (the PSMF header's macroblock counts times 16,
 * 480 x 224 there), four zero crop words, 1 (the last decode produced a picture), 0, a guest address
 * the probe could not identify (written 0 here), 0, 0.  PPSSPP (c9e2afb6) writes nine words with a
 * decode result of 1 in word 0. */
uint32_t mpeg_avc_decode_detail(uint32_t mpegAddr, uint32_t detailAddr) {
    if (!detailAddr || (detailAddr & 3u) || !sr_guest_span_writable(detailAddr, 52u))
        return mpeg_contract_error("sceMpegAvcDecodeDetail", "invalid detail pointer", (uint32_t)-1);
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    MEM_W32(detailAddr + 0u, 0u);
    MEM_W32(detailAddr + 4u, (uint32_t)ctx->h264Frames);
    MEM_W32(detailAddr + 8u, ctx->streamWidth);
    MEM_W32(detailAddr + 12u, ctx->streamHeight);
    for (uint32_t off = 16u; off < 32u; off += 4u) MEM_W32(detailAddr + off, 0u);
    MEM_W32(detailAddr + 32u, ctx->avcFrameStatus ? 1u : 0u);
    for (uint32_t off = 36u; off < 52u; off += 4u) MEM_W32(detailAddr + off, 0u);
    return 0;
}

/* ---- YCbCr decode path (sceMpegAvc*YCbCr / sceMpegAvcCsc) ----
 * MEASURED on a PSP-3000 (firmware 6.61; PSP-MPEG-001, 2026-10-31: mode 1, a 480x224 stream in a
 * 480x272 allocation; every sample of the dumped picture matched a reference decode).  The YCbCr
 * buffer is guest data.  Its 128-byte header holds, once a picture is decoded, the picture height
 * / 16 in word 0 and its width / 16 in word 1, and in words 4..11 eight plane pointers into the
 * buffer; sceMpegAvcInitYCbCr writes only the header (words 0..3 zero) and leaves the planes as
 * they are.  The planes hold the picture as 16-pixel column strips stored one after another: luma
 * plane (s & 1) + 2 * (y & 1) holds the lines of parity y & 1 of the strips of parity s & 1, 16
 * bytes per line and h / 2 lines per strip; chroma planes 4..7 do the same at half height with 8
 * chroma samples per strip line, Cb and Cr byte-interleaved.  A decode writes only the picture's
 * share of each plane.  sceMpegAvcCsc reads the planes through the header pointers (exchanging
 * two pointers exchanges the output) and converts with sr_h264_psp_rgb.  Only mode 1 is mapped,
 * the strip length from the decoded height rests on this one geometry, and an odd number of
 * strips is not measured.  The host keeps only the sceMpegAvcInitYCbCr geometry of each buffer (a
 * bound on the decode) and the presentation time of each decoded picture (the A/V measurement),
 * keyed by a hash of its plane bytes: a guest PSMF player converts another buffer it filled from
 * the decoded one, so the picture's bytes, not the buffer, name it.  Neither gates
 * sceMpegAvcCsc. */
#define YCBCR_HEADER_BYTES 128u
#define YCBCR_ALIGNMENT 16u
#define YCBCR_SLOT_COUNT 16
#define YCBCR_MODE1_MAX_W 480u
#define YCBCR_MODE1_MAX_H 272u
#define YCBCR_SCRATCH_W 512u   /* wide enough to see a picture past the mode-1 limit */
#define YCBCR_LUMA_PLANE_MAX ((YCBCR_MODE1_MAX_W / 32u) * 8u * YCBCR_MODE1_MAX_H)
#define YCBCR_CHROMA_PLANE_MAX ((YCBCR_MODE1_MAX_W / 32u) * 4u * YCBCR_MODE1_MAX_H)
/* MEASURED (PSP-MPEG-001): sceMpegAvcCsc's refusal of a zeroed header, of a range outside the
 * picture and of a destination narrower than the range. */
#define MPEG_ERROR_CSC_REFUSED 0x800001FEu

typedef struct {
    uint32_t mpeg, buf, width, height;   /* the sceMpegAvcInitYCbCr geometry; 0 when unknown */
} YcbcrBuf;
static YcbcrBuf s_ycbcr[YCBCR_SLOT_COUNT];
typedef struct {
    uint32_t mpeg;
    uint64_t bytes;          /* FNV-1a of the picture's planes; 0 = free */
    int64_t pts;             /* its access unit's presentation time, -1 when it carried none */
} YcbcrPicture;
static YcbcrPicture s_ycbcr_pictures[YCBCR_SLOT_COUNT];
static unsigned s_ycbcr_picture_next;
/* The decoder's picture, and the guest planes while they are assembled or read. */
static uint8_t s_ycbcr_y[YCBCR_SCRATCH_W * YCBCR_MODE1_MAX_H];
static uint8_t s_ycbcr_cb[(YCBCR_SCRATCH_W / 2u) * (YCBCR_MODE1_MAX_H / 2u)];
static uint8_t s_ycbcr_cr[(YCBCR_SCRATCH_W / 2u) * (YCBCR_MODE1_MAX_H / 2u)];
static uint8_t s_ycbcr_planes[4u * YCBCR_LUMA_PLANE_MAX + 4u * YCBCR_CHROMA_PLANE_MAX];

/* What a header describes: the plane pointers and the decoded geometry of words 0 and 1. */
typedef struct {
    uint32_t plane[8];
    uint32_t width, height;     /* multiples of 16; 0 before the first decode */
} YcbcrLayout;

static YcbcrBuf *ycbcr_find(uint32_t mpegAddr, uint32_t buf) {
    for (int i = 0; i < YCBCR_SLOT_COUNT; i++)
        if (buf && s_ycbcr[i].buf == buf && s_ycbcr[i].mpeg == mpegAddr) return &s_ycbcr[i];
    return NULL;
}

static void ycbcr_clear_slot(YcbcrBuf *b) {
    if (b) memset(b, 0, sizeof(*b));
}

static void ycbcr_reset_mpeg(uint32_t mpegAddr) {
    for (int i = 0; i < YCBCR_SLOT_COUNT; i++) {
        if (s_ycbcr[i].mpeg == mpegAddr) ycbcr_clear_slot(&s_ycbcr[i]);
        if (s_ycbcr_pictures[i].mpeg == mpegAddr) memset(&s_ycbcr_pictures[i], 0, sizeof(s_ycbcr_pictures[i]));
    }
}

/* Every decoded picture is an entry of its own; the oldest entry gives way to a new one. */
static void ycbcr_picture_set(uint32_t mpegAddr, uint64_t bytes, int64_t pts) {
    if (!bytes) return;
    YcbcrPicture *p = &s_ycbcr_pictures[s_ycbcr_picture_next++ % YCBCR_SLOT_COUNT];
    p->mpeg = mpegAddr;
    p->bytes = bytes;
    p->pts = pts;
}

/* The time of the decoded picture with these bytes, or -1 when there is none, or when identical
 * pictures with different times are recorded (a still scene): which one is shown is unknown. */
static int64_t ycbcr_picture_pts(uint32_t mpegAddr, uint64_t bytes) {
    int64_t pts = -1;
    int found = 0;
    for (int i = 0; i < YCBCR_SLOT_COUNT; i++) {
        const YcbcrPicture *p = &s_ycbcr_pictures[i];
        if (!bytes || p->bytes != bytes || p->mpeg != mpegAddr) continue;
        if (found && p->pts != pts) return -1;
        pts = p->pts;
        found = 1;
    }
    return pts;
}

/* The slot of (mpeg, buf), taking a free one when there is none. */
static YcbcrBuf *ycbcr_slot(uint32_t mpegAddr, uint32_t buf) {
    YcbcrBuf *b = ycbcr_find(mpegAddr, buf);
    if (b) return b;
    for (int i = 0; i < YCBCR_SLOT_COUNT; i++)
        if (!s_ycbcr[i].buf) {
            b = &s_ycbcr[i];
            b->mpeg = mpegAddr;
            b->buf = buf;
            return b;
        }
    return NULL;
}

static int ycbcr_header_read(uint32_t buf, YcbcrLayout *l) {
    memset(l, 0, sizeof(*l));
    if (!buf || (buf & 3u) || !sr_guest_span_readable(buf, YCBCR_HEADER_BYTES)) return 0;
    uint32_t rows = MEM_R32(buf), cols = MEM_R32(buf + 4u);
    if (rows <= MPEG_MAX_DIMENSION / 16u && cols <= MPEG_MAX_DIMENSION / 16u) {
        l->height = rows * 16u;
        l->width = cols * 16u;
    }
    for (uint32_t i = 0; i < 8u; i++) l->plane[i] = MEM_R32(buf + 16u + 4u * i);
    return 1;
}

/* Bytes plane i (luma 0..3, chroma 4..7) holds of a width x height picture. */
static uint32_t ycbcr_plane_bytes(int i, uint32_t width, uint32_t height) {
    uint32_t strips = width / 16u;
    uint32_t mine = (strips + 1u - (uint32_t)(i & 1)) / 2u;
    return mine * (i < 4 ? 8u : 4u) * height;
}

static uint8_t *ycbcr_scratch_plane(int i) {
    return s_ycbcr_planes + (i < 4 ? (uint32_t)i * YCBCR_LUMA_PLANE_MAX
                                   : 4u * YCBCR_LUMA_PLANE_MAX + (uint32_t)(i - 4) * YCBCR_CHROMA_PLANE_MAX);
}

/* Plane and byte offset of luma sample (x, y) and of chroma pair (cx, cy) (Cb, then Cr). */
static uint32_t ycbcr_luma_at(uint32_t x, uint32_t y, uint32_t height, int *plane) {
    uint32_t s = x >> 4;
    *plane = (int)((s & 1u) + 2u * (y & 1u));
    return (s >> 1) * 8u * height + (y >> 1) * 16u + (x & 15u);
}
static uint32_t ycbcr_chroma_at(uint32_t cx, uint32_t cy, uint32_t height, int *plane) {
    uint32_t s = cx >> 3;
    *plane = 4 + (int)((s & 1u) + 2u * (cy & 1u));
    return (s >> 1) * 4u * height + (cy >> 1) * 16u + (cx & 7u) * 2u;
}

/* Copy the picture's share of every plane between guest memory and the scratch planes. */
static int ycbcr_planes_in(const YcbcrLayout *l, uint32_t width, uint32_t height) {
    for (int i = 0; i < 8; i++) {
        uint32_t n = ycbcr_plane_bytes(i, width, height);
        if (n && (!l->plane[i] || !sr_guest_copy_in(ycbcr_scratch_plane(i), l->plane[i], n))) return 0;
    }
    return 1;
}
static int ycbcr_planes_writable(const YcbcrLayout *l, uint32_t width, uint32_t height) {
    for (int i = 0; i < 8; i++) {
        uint32_t n = ycbcr_plane_bytes(i, width, height);
        if (n && (!l->plane[i] || !sr_guest_span_writable_any_view(l->plane[i], n))) return 0;
    }
    return 1;
}
static void ycbcr_planes_out(const YcbcrLayout *l, uint32_t width, uint32_t height) {
    for (int i = 0; i < 8; i++) {
        uint32_t n = ycbcr_plane_bytes(i, width, height);
        if (n) sr_guest_copy_out(l->plane[i], ycbcr_scratch_plane(i), n);
    }
}

/* FNV-1a of the picture's share of the scratch planes (never 0). */
static uint64_t ycbcr_planes_hash(uint32_t width, uint32_t height) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 8; i++) {
        const uint8_t *p = ycbcr_scratch_plane(i);
        for (uint32_t n = ycbcr_plane_bytes(i, width, height), k = 0; k < n; k++)
            h = (h ^ p[k]) * 1099511628211ull;
    }
    return h ? h : 1u;
}

static int ycbcr_mode1_geometry(uint32_t width, uint32_t height) {
    return width && height && !(width & 15u) && !(height & 15u) &&
           width <= YCBCR_MODE1_MAX_W && height <= YCBCR_MODE1_MAX_H;
}

static int ycbcr_pointer_valid(uint32_t addr, uint32_t size, uint32_t alignment) {
    return addr && !(addr & (alignment - 1u)) && sr_guest_span_writable(addr, size);
}

/* MEASURED (PSP-MPEG-001): mode 1 needs a width and height that are multiples of 16 within
 * 480x272 and gives 128 + 1.5 w h; mode 2 gives 128 whatever the size; mode 3 gives 1.5 w h with
 * no header (15x16 gives 360, 0x0 gives 0).  Every other mode, and mode 1 outside its limits, is
 * refused with SCE_MPEG_ERROR_INVALID_VALUE and a size of 0.  The PSMF stream's own geometry is
 * not checked.  Mode 3 wider than 480 does not follow 1.5 w h (496 and 512 both give the 512-wide
 * size) and is not modeled. */
uint32_t mpeg_avc_query_ycbcr_size(uint32_t mpegAddr, uint32_t mode, uint32_t width,
                                   uint32_t height, uint32_t resultAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!resultAddr || (resultAddr & 3u) || !sr_guest_span_writable(resultAddr, 4u))
        return mpeg_contract_error("sceMpegAvcQueryYCbCrSize", "unaligned or invalid result pointer", SCE_MPEG_ERROR_INVALID_VALUE);
    uint64_t size;
    if (mode == 1u && ycbcr_mode1_geometry(width, height)) {
        size = (uint64_t)width * height * 3u / 2u + YCBCR_HEADER_BYTES;
    } else if (mode == 2u) {
        size = YCBCR_HEADER_BYTES;
    } else if (mode == 3u) {
        if (width > YCBCR_MODE1_MAX_W || height > MPEG_MAX_DIMENSION) {
            MEM_W32(resultAddr, 0);
            return mpeg_contract_error("sceMpegAvcQueryYCbCrSize", "mode 3 sizes wider than 480 are not modeled",
                                       SCE_MPEG_ERROR_INVALID_VALUE);
        }
        size = (uint64_t)width * height * 3u / 2u;
    } else {
        MEM_W32(resultAddr, 0);
        return SCE_MPEG_ERROR_INVALID_VALUE;
    }
    MEM_W32(resultAddr, (uint32_t)size);
    return 0;
}

/* Mode 1 only: its header is MEASURED (above); a buffer of Wa x Ha gets the pointers, in memory
 * order, P0 = buf + 128, P2, P4 (luma, Wa Ha / 4 bytes each), P6 (chroma, Wa Ha / 8), P1, P3, P5
 * (luma), P7 (chroma).  Modes 2 and 3, and the PSP's own Init limits, are not measured; the
 * query's mode-1 limits are applied. */
uint32_t mpeg_avc_init_ycbcr(uint32_t mpegAddr, uint32_t mode, uint32_t width, uint32_t height,
                             uint32_t buf) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (getenv("SR_MPEGLOG"))
        fprintf(stderr, "MpegAvcInitYCbCr mode=%" PRIu32 " %" PRIu32 "x%" PRIu32 " buf=0x%" PRIx32 "\n",
                mode, width, height, buf);
    if (mode != 1u)
        return mpeg_contract_error("sceMpegAvcInitYCbCr", "only the mode-1 YCbCr layout is measured",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    if (!ycbcr_mode1_geometry(width, height)) return SCE_MPEG_ERROR_INVALID_VALUE;
    if ((width / 16u) & 1u)
        return mpeg_contract_error("sceMpegAvcInitYCbCr", "the layout of an odd number of 16-pixel strips is not measured",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    uint32_t size = width * height * 3u / 2u + YCBCR_HEADER_BYTES;
    if (!ycbcr_pointer_valid(buf, size, YCBCR_ALIGNMENT))
        return mpeg_contract_error("sceMpegAvcInitYCbCr", "unaligned or truncated guest allocation", SCE_MPEG_ERROR_INVALID_VALUE);
    for (int i = 0; i < YCBCR_SLOT_COUNT; i++)
        if (s_ycbcr[i].buf == buf && s_ycbcr[i].mpeg != mpegAddr) ycbcr_clear_slot(&s_ycbcr[i]);
    YcbcrBuf *b = ycbcr_slot(mpegAddr, buf);
    if (!b)
        return mpeg_contract_error("sceMpegAvcInitYCbCr", "no host slot for another YCbCr buffer", SCE_MPEG_ERROR_NO_MEMORY);
    b->width = width;
    b->height = height;
    const uint32_t q = width * height / 4u, e = width * height / 8u;
    uint32_t p[8];
    p[0] = buf + YCBCR_HEADER_BYTES;
    p[2] = p[0] + q;
    p[4] = p[2] + q;
    p[6] = p[4] + e;
    p[1] = p[6] + e;
    p[3] = p[1] + q;
    p[5] = p[3] + q;
    p[7] = p[5] + e;
    for (uint32_t off = 0; off < YCBCR_HEADER_BYTES; off += 4u)
        MEM_W32(buf + off, off >= 16u && off < 48u ? p[(off - 16u) / 4u] : 0u);
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

/* Write the decoded picture (pw x ph samples in the scratch planes) through buf's header. */
static uint32_t ycbcr_store_picture(uint32_t mpegAddr, uint32_t buf, const YcbcrLayout *l, int pw, int ph,
                                    uint64_t *bytes) {
    if (pw <= 0 || ph <= 0 || (uint32_t)pw > YCBCR_MODE1_MAX_W || (uint32_t)ph > YCBCR_MODE1_MAX_H)
        return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "a picture larger than 480x272 is beyond the measured layout",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    const uint32_t w = ((uint32_t)pw + 15u) & ~15u, h = ((uint32_t)ph + 15u) & ~15u;
    if ((w / 16u) & 1u)
        return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "the layout of an odd number of 16-pixel strips is not measured",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    const YcbcrBuf *slot = ycbcr_find(mpegAddr, buf);
    if (slot && slot->width && (w / 32u) * h > (slot->width / 32u) * slot->height)
        return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "the picture is larger than the YCbCr allocation",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    if (!ycbcr_planes_writable(l, w, h) || !ycbcr_planes_in(l, w, h))
        return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "the header's plane pointers do not reach guest memory",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    int plane;
    for (uint32_t y = 0; y < (uint32_t)ph; y++)
        for (uint32_t x = 0; x < (uint32_t)pw; x++) {
            uint32_t off = ycbcr_luma_at(x, y, h, &plane);
            ycbcr_scratch_plane(plane)[off] = s_ycbcr_y[y * YCBCR_SCRATCH_W + x];
        }
    for (uint32_t cy = 0; cy < ((uint32_t)ph + 1u) / 2u; cy++)
        for (uint32_t cx = 0; cx < ((uint32_t)pw + 1u) / 2u; cx++) {
            uint32_t off = ycbcr_chroma_at(cx, cy, h, &plane);
            ycbcr_scratch_plane(plane)[off] = s_ycbcr_cb[cy * (YCBCR_SCRATCH_W / 2u) + cx];
            ycbcr_scratch_plane(plane)[off + 1u] = s_ycbcr_cr[cy * (YCBCR_SCRATCH_W / 2u) + cx];
        }
    ycbcr_planes_out(l, w, h);
    MEM_W32(buf, h / 16u);
    MEM_W32(buf + 4u, w / 16u);
    *bytes = sr_psmf_av_active() ? ycbcr_planes_hash(w, h) : 0u;
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
    YcbcrLayout layout;
    if (!ycbcr_header_read(buf, &layout))
        return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "unaligned or unreadable YCbCr header", SCE_MPEG_ERROR_INVALID_VALUE);
    g_mpeg_avcdec++;
    if (!ctx->h264Init || ctx->h264 < 0) {
        MEM_W32(initAddr, 0u);
        return h264_backend_error("sceMpegAvcDecodeYCbCr");
    }
    ctx->videoPts += videoTimestampStep;
    ctx->ycbcrWant++;
    int got = 0;
    SrH264FrameInfo info = {0};
    int eos = ctx->totalPackets && ctx->fedPackets >= ctx->totalPackets;
#ifdef SR_SDL3VK
    while (ctx->h264Frames < ctx->ycbcrWant) {
        SrH264FrameTarget target;
        memset(&target, 0, sizeof(target));
        target.kind = SR_H264_TARGET_HOST_YUV;
        target.host_buffer = s_ycbcr_y;
        target.host_width = (int)YCBCR_SCRATCH_W;
        target.host_height = (int)YCBCR_MODE1_MAX_H;
        target.host_stride = (int)YCBCR_SCRATCH_W;
        target.host_cb = s_ycbcr_cb;
        target.host_cr = s_ycbcr_cr;
        target.host_c_stride = (int)(YCBCR_SCRATCH_W / 2u);
        int r = sr_h264_frame_ex(ctx->h264, eos, &target, &info);
        if (r > 0) {
            got = r;
            ctx->h264Frames++;
        } else if (r < 0) {
            MEM_W32(initAddr, 0u);
            return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "H.264 backend rejected the access unit", SCE_MPEG_ERROR_NO_DATA);
        } else {
            break;
        }
    }
#endif
    ctx->avcFrameStatus = got > 0;
    if (got <= 0) {
        MEM_W32(initAddr, 0u);
        if (eos)
            return mpeg_contract_error("sceMpegAvcDecodeYCbCr", "no decoded picture at end of stream", SCE_MPEG_ERROR_NO_DATA);
        return 0;
    }
    uint64_t bytes = 0;
    uint32_t stored = ycbcr_store_picture(mpegAddr, buf, &layout, info.width, info.height, &bytes);
    if (stored) {
        MEM_W32(initAddr, 0u);
        return stored;
    }
    ycbcr_picture_set(mpegAddr, bytes, au_read_pts(auAddr));
    MEM_W32(initAddr, 1u);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcDecodeYCbCr #%d au=0x%" PRIx32 " ycbcr=0x%" PRIx32 " dec=%d frames=%d pts=%lld\n",
                    n, auAddr, buf, got, ctx->h264Frames, (long long)ctx->videoPts);
    }
    return 0;
}

/* sceMpegAvcDecodeStopYCbCr(mpeg, ycbcr*, status*): like sceMpegAvcDecodeYCbCr it takes the
 * address of the buffer pointer, and it hands over the picture the decoder still holds.
 * MEASURED (PSP-MPEG-001): given the buffer itself the call never returned; given the address
 * of the buffer pointer after the last access unit it returned 0 with status 1, and the
 * sceMpegAvcDecodeStop that followed returned status 0.  The PSP decoder runs one access unit
 * behind (the first sceMpegAvcDecodeYCbCr gives no picture), so it holds one.  Here a picture
 * the decoder can still give for an access unit the game took is written through the buffer's
 * header with status 1; otherwise the status is 0 and nothing is written.  As in
 * sceMpegAvcDecodeYCbCr the decoder is drained only once the whole stream has been fed: a
 * decoder that holds its last picture until it is drained (Media Foundation) gives none before
 * that, and what the PSP does after a stop in mid-stream is not measured. */
uint32_t mpeg_avc_decode_stop_ycbcr(uint32_t mpegAddr, uint32_t bufPtr, uint32_t statusAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!statusAddr || (statusAddr & 3u) || !sr_guest_span_writable(statusAddr, 4u) ||
        !bufPtr || (bufPtr & 3u) || !sr_guest_span_readable(bufPtr, 4u))
        return mpeg_contract_error("sceMpegAvcDecodeStopYCbCr", "invalid buffer or status pointer", (uint32_t)-1);
    uint32_t buf = MEM_R32(bufPtr);
    YcbcrLayout layout;
    if (!ycbcr_header_read(buf, &layout))
        return mpeg_contract_error("sceMpegAvcDecodeStopYCbCr", "unaligned or unreadable YCbCr header", SCE_MPEG_ERROR_INVALID_VALUE);
    int got = 0;
    SrH264FrameInfo info = {0};
#ifdef SR_SDL3VK
    if (ctx->h264Init && ctx->h264 >= 0) {
        SrH264FrameTarget target;
        memset(&target, 0, sizeof(target));
        target.kind = SR_H264_TARGET_HOST_YUV;
        target.host_buffer = s_ycbcr_y;
        target.host_width = (int)YCBCR_SCRATCH_W;
        target.host_height = (int)YCBCR_MODE1_MAX_H;
        target.host_stride = (int)YCBCR_SCRATCH_W;
        target.host_cb = s_ycbcr_cb;
        target.host_cr = s_ycbcr_cr;
        target.host_c_stride = (int)(YCBCR_SCRATCH_W / 2u);
        int eos = ctx->totalPackets && ctx->fedPackets >= ctx->totalPackets;
        if (sr_h264_frame_ex(ctx->h264, eos, &target, &info) > 0) {
            got = 1;
            ctx->h264Frames++;
            ctx->ycbcrWant = ctx->h264Frames;
        }
    }
#endif
    if (got) {
        uint64_t bytes = 0;
        uint32_t stored = ycbcr_store_picture(mpegAddr, buf, &layout, info.width, info.height, &bytes);
        if (stored) {
            MEM_W32(statusAddr, 0u);
            return stored;
        }
        ycbcr_picture_set(mpegAddr, bytes, -1);
    }
    MEM_W32(statusAddr, got ? 1u : 0u);
    return 0;
}

static int ycbcr_ranges_overlap(uint32_t a, uint32_t asize, uint32_t b, uint32_t bsize) {
    uint64_t a0 = a, a1 = a0 + asize, b0 = b, b1 = b0 + bsize;
    return asize && bsize && a0 < b1 && b0 < a1;
}

/* sceMpegAvcCopyYCbCr(mpeg, dst, src) is not measured: the decoded share of every plane and
 * words 0 and 1 are copied through the two headers. */
uint32_t mpeg_avc_copy_ycbcr(uint32_t mpegAddr, uint32_t dst, uint32_t src) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!dst || !src || (dst & (YCBCR_ALIGNMENT - 1u)) || (src & (YCBCR_ALIGNMENT - 1u)))
        return mpeg_contract_error("sceMpegAvcCopyYCbCr", "unaligned source or destination", SCE_MPEG_ERROR_INVALID_VALUE);
    YcbcrLayout from, to;
    if (!ycbcr_header_read(src, &from) || !ycbcr_header_read(dst, &to))
        return mpeg_contract_error("sceMpegAvcCopyYCbCr", "unreadable YCbCr header", SCE_MPEG_ERROR_INVALID_VALUE);
    const uint32_t w = from.width, h = from.height;
    if (!w || !h || w > YCBCR_MODE1_MAX_W || h > YCBCR_MODE1_MAX_H || ((w / 16u) & 1u))
        return mpeg_contract_error("sceMpegAvcCopyYCbCr", "the source holds no decoded picture", SCE_MPEG_ERROR_INVALID_VALUE);
    if (dst == src) return 0;
    const YcbcrBuf *toInit = ycbcr_find(mpegAddr, dst);
    if (toInit && toInit->width && (w / 32u) * h > (toInit->width / 32u) * toInit->height)
        return mpeg_contract_error("sceMpegAvcCopyYCbCr", "the picture is larger than the destination allocation",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            if (ycbcr_ranges_overlap(from.plane[i], ycbcr_plane_bytes(i, w, h),
                                     to.plane[j], ycbcr_plane_bytes(j, w, h)) ||
                ycbcr_ranges_overlap(dst, YCBCR_HEADER_BYTES, from.plane[i], ycbcr_plane_bytes(i, w, h)))
                return mpeg_contract_error("sceMpegAvcCopyYCbCr", "overlapping planes are not modeled", SCE_MPEG_ERROR_INVALID_VALUE);
    if (!ycbcr_planes_writable(&to, w, h) || !ycbcr_planes_in(&from, w, h))
        return mpeg_contract_error("sceMpegAvcCopyYCbCr", "the header's plane pointers do not reach guest memory",
                                   SCE_MPEG_ERROR_INVALID_VALUE);
    ycbcr_planes_out(&to, w, h);
    MEM_W32(dst, h / 16u);
    MEM_W32(dst + 4u, w / 16u);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcCopyYCbCr #%d dst=0x%" PRIx32 " src=0x%" PRIx32 " %ux%u\n", n, dst, src,
                    (unsigned)w, (unsigned)h);
    }
    return 0;
}

/* sceMpegAvcCsc(mpeg, ycbcr, range*, frameWidth, dest): range is {x, y, w, h} in pixels.
 * MEASURED (PSP-MPEG-001, picture 480x224): an odd x or w is refused with
 * SCE_MPEG_ERROR_INVALID_VALUE; a negative origin, a range past the picture (w 496, h 240), a
 * frame width narrower than the range (256) or a zeroed header with MPEG_ERROR_CSC_REFUSED; an
 * empty range succeeds.  The range {16, 16, 64, 64} wrote the destination from its first pixel
 * with source columns 16..79 and rows 0..63: x selects the source columns, and y had no effect in
 * that one case, so rows are taken from the top.  A y + h past the picture is refused like a
 * wide range, which is not measured.  The 16-bit pixel modes are not measured. */
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
    YcbcrLayout l;
    if (!ycbcr_header_read(buf, &l))
        return mpeg_contract_error("sceMpegAvcCsc", "unaligned or unreadable YCbCr header", SCE_MPEG_ERROR_INVALID_VALUE);
    if (!l.width || !l.height) return MPEG_ERROR_CSC_REFUSED;
    if (l.width > YCBCR_MODE1_MAX_W || l.height > YCBCR_MODE1_MAX_H || ((l.width / 16u) & 1u))
        return mpeg_contract_error("sceMpegAvcCsc", "the header describes a picture beyond the measured layout",
                                   MPEG_ERROR_CSC_REFUSED);
    const int64_t x = (int32_t)MEM_R32(rangeAddr), y = (int32_t)MEM_R32(rangeAddr + 4u);
    const int64_t w = (int32_t)MEM_R32(rangeAddr + 8u), h = (int32_t)MEM_R32(rangeAddr + 12u);
    if ((x & 1) || (w & 1)) return SCE_MPEG_ERROR_INVALID_VALUE;
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > (int64_t)l.width || y + h > (int64_t)l.height)
        return MPEG_ERROR_CSC_REFUSED;
    if (w == 0 || h == 0) return 0;
    if ((uint64_t)w > frameWidth) return MPEG_ERROR_CSC_REFUSED;
    if (!ycbcr_planes_in(&l, l.width, l.height))
        return mpeg_contract_error("sceMpegAvcCsc", "the header's plane pointers do not reach guest memory",
                                   MPEG_ERROR_CSC_REFUSED);
    uint32_t bpp = ctx->pixelMode == 3 ? 4u : 2u;
    SrGuestRectSpan span;
    /* The destination may be a swizzled VRAM mirror view (#1049), so each row is built in a
     * host scratch row and written with sr_guest_copy_out(), which translates it piece by
     * piece and prepares/commits the VRAM span of every piece. */
    if (!sr_guest_rect_geometry(dest, 0u, 0u, frameWidth, (uint32_t)w, (uint32_t)h, bpp, &span) ||
        !sr_guest_span_writable_any_view(span.first, span.total_bytes))
        return mpeg_contract_error("sceMpegAvcCsc", "destination rectangle is out of bounds", SCE_MPEG_ERROR_INVALID_VALUE);
    uint8_t *out = (uint8_t *)malloc(span.row_bytes);
    if (!out)
        return mpeg_contract_error("sceMpegAvcCsc", "no scratch row for the destination", SCE_MPEG_ERROR_INVALID_VALUE);
    for (uint32_t row = 0; row < (uint32_t)h; row++) {
        uint32_t rowAddr = span.first + row * span.row_pitch;
#ifdef SR_MPEG_CSC_NO_WRITE_MUTANT
        (void)out;
        (void)rowAddr;
#else
        for (uint32_t col = 0; col < (uint32_t)w; col++) {
            const uint32_t sx = (uint32_t)x + col, sy = row;
            int lp, cp;
            uint32_t lo = ycbcr_luma_at(sx, sy, l.height, &lp);
            uint32_t co = ycbcr_chroma_at(sx >> 1, sy >> 1, l.height, &cp);
            const uint8_t *chroma = ycbcr_scratch_plane(cp) + co;
            uint8_t r, g, bl;
            sr_h264_psp_rgb(ycbcr_scratch_plane(lp)[lo], chroma[0], chroma[1], &r, &g, &bl);
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
                default:   /* 8888: R, G, B and alpha 0 */
                    out[col * 4u + 0] = r;
                    out[col * 4u + 1] = g;
                    out[col * 4u + 2] = bl;
                    out[col * 4u + 3] = 0u;
                    break;
            }
        }
        sr_guest_copy_out(rowAddr, out, span.row_bytes);
#endif
    }
    free(out);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcCsc #%d ycbcr=0x%" PRIx32 " dest=0x%" PRIx32 " fw=%" PRIu32 " range=%d,%d %dx%d\n",
                    n, buf, dest, frameWidth, (int)x, (int)y, (int)w, (int)h);
    }
    const int64_t pts = sr_psmf_av_active() ? ycbcr_picture_pts(mpegAddr, ycbcr_planes_hash(l.width, l.height)) : -1;
    sr_psmf_av_note_video_delivery(span.first, span.total_bytes, pts, pts >= 0);
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
    ctx->streams[index].auMode = (int)mode;
    return 0;
}

/* sceMpegAtracDecode(mpeg, au, buffer, init): decode the frame the last GetAtracAu held into the
 * 8192-byte block at buffer; init (nonzero on a stream's first call) resets the decoder. */
uint32_t mpeg_atrac_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t bufferAddr, uint32_t init) {
    (void)auAddr;
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!bufferAddr || (bufferAddr & 3u) ||
        !sr_guest_span_writable_any_view(bufferAddr, MPEG_ATRAC_ES_OUTPUT_SIZE))
        return mpeg_contract_error("sceMpegAtracDecode", "invalid output buffer", (uint32_t)-1);
    if (!ctx->audioHeld)
        return mpeg_contract_error("sceMpegAtracDecode", "no ATRAC access unit is held for decoding",
                                   SCE_MPEG_ERROR_NO_DATA);
    uint8_t *frame = ctx->audioHeld;
    uint32_t bytes = ctx->audioHeldBytes;
    ctx->audioHeld = NULL;
    ctx->audioHeldBytes = 0;
    uint32_t refused = mpeg_audio_decode_frame(ctx, frame, bytes, init);
    free(frame);
    if (refused) return refused;
    int64_t pts = ctx->audioPts + ctx->firstTimestamp;
    sr_guest_copy_out(bufferAddr, ctx->audioPcm, MPEG_ATRAC_ES_OUTPUT_SIZE);
    sr_psmf_av_note_audio_delivery(bufferAddr, MPEG_ATRAC_ES_OUTPUT_SIZE, pts, 1);
    ctx->audioPts += audioTimestampStep;
    ctx->audioDecoded++;
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 16 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAtracDecode #%d pts=%lld audioPts=%lld decoded=%llu\n", n,
                    (long long)pts, (long long)ctx->audioPts, (unsigned long long)ctx->audioDecoded);
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
    /* The game refills the ring from a new position: the audio demux starts over with it. */
    mpeg_audio_restart(ctx);
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
