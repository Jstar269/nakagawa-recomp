// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Project-authored PSMF producer boundary.  This layer deliberately stops at
 * compressed access units: decoder success is never inferred from demux success.
 * The source callback is the only filesystem boundary, so ISO, loose VFS, and
 * synthetic memory sources share exactly the same parser and queue semantics. */
#ifndef SR_PSMF_PRODUCER_H
#define SR_PSMF_PRODUCER_H

#include <stdint.h>

#define SR_PSMF_SOURCE_ERROR (-1)
#define SR_PSMF_SOURCE_EOF    0
#define SR_PSMF_SOURCE_DATA   1
#define SR_PSMF_MAX_AU_BYTES  (4u * 1024u * 1024u)
#define SR_PSMF_QUEUE_DEPTH   8u

/* Presentation pacing, owned here so the player (src/rt/hle.c) and every source-owned
 * test advance the same clocks with the same numbers.  Both are 90 kHz presentation
 * ticks: 3003 is one 29.97 fps coded picture, 4180 is the integer step for one
 * 2048-sample ATRAC3+ block at 44.1 kHz (2048 * 90000 / 44100 = 4180.95...).  The
 * truncation is why a clock may only extrapolate between anchors: see
 * sr_psmf_pts_advance(). */
#define PSMF_VIDEO_PTS_STEP 3003
#define PSMF_AUDIO_PTS_STEP 4180

/* Shared scePsmfPlayer decode and queue dimensions.  The producer's AU queue
 * depth above is a separate layer from the player's per-stage queues. */
#define PSMF_AUDIO_SAMPLES 2048u
#define PSMF_AUDIO_BYTES (PSMF_AUDIO_SAMPLES * 4u) /* stereo s16 */
#define PSMF_AUDIO_MAX_CHANNELS 8u
#define PSMF_Q_DEPTH 4u

/* How many submitted pictures' presentation times the player (src/rt/hle.c) can hold ahead * of the getter.  Published here because it is the bound that makes the measured pipeline
 * lead meaningful: submitted-minus-delivered can never exceed it, so a vpts-vts separation
 * inside this bound is queue distance by construction, not synchronization error. */
#define PSMF_OUT_PTS_RING 8

typedef int (*SrPsmfReadFn)(void *opaque, uint64_t offset, void *dst,
                            uint32_t capacity, uint32_t *got);

typedef struct {
    SrPsmfReadFn read;
    void *opaque;
    uint64_t size;
} SrPsmfSource;

typedef enum {
    SR_PSMF_AU_VIDEO = 0,
    SR_PSMF_AU_AUDIO = 1,
} SrPsmfAuKind;

typedef enum {
    SR_PSMF_MEDIA_SUBMITTED = 0,
    SR_PSMF_MEDIA_DECODED,
    SR_PSMF_MEDIA_DELIVERED,
    SR_PSMF_MEDIA_WARMUP_HELD,
    SR_PSMF_MEDIA_EOS_DRAINED,
    SR_PSMF_MEDIA_REJECTED,
} SrPsmfMediaStage;

typedef struct {
    SrPsmfAuKind kind;
    uint8_t stream_id;
    uint8_t has_pts;
    uint8_t has_dts;
    uint8_t reserved;
    int64_t pts;       /* normalized to the PSMF presentation base */
    int64_t dts;       /* normalized to the PSMF presentation base */
    int64_t raw_pts;   /* PES clock observation, retained for diagnostics */
    int64_t raw_dts;
    uint8_t *data;
    uint32_t size;
} SrPsmfAu;

typedef struct {
    uint64_t bytes_read;
    uint64_t packs;
    uint64_t pes_packets;
    uint64_t video_pes;
    uint64_t audio_pes;
    uint64_t video_aus;
    uint64_t audio_aus;
    /* Consumer-side media census. delivered excludes outputs returned by the EOS-drain
     * path, and rejected counts decoded output refused by the consumer. warmup_held is
     * the current outstanding count; resolve_warmup_hold() moves it to a terminal bucket. */
    uint64_t video_submitted, video_decoded, video_delivered;
    uint64_t video_warmup_held, video_eos_drained, video_rejected;
    uint64_t audio_submitted, audio_decoded, audio_delivered;
    uint64_t audio_warmup_held, audio_eos_drained, audio_rejected;
    /* Candidate four-byte start positions examined while locating video AUDs. */
    uint64_t video_aud_scan_candidates;
    uint64_t parser_failures;
    uint64_t source_failures;
    /* Access units that carried no PES presentation time; the consumer extrapolates
     * those from the codec frame step.  Also the bytes dropped while resynchronizing
     * the audio frame stream, and the elementary bytes still held incomplete. */
    uint64_t aus_without_pts;
    /* Media PES packets whose PTS_DTS_flags declared a decode time.  This is the count that
     * separates "the stream carries no DTS" from "the parser dropped it": both look
     * identical in every other counter, and only one of them is real.  It is counted per
     * packet, so it can be checked against a census of the stream itself. */
    uint64_t pes_with_dts;
    uint64_t audio_resync_bytes;
    uint64_t fail_offset;      /* source offset of the first unparseable element */
    uint32_t video_depth;
    uint32_t audio_depth;
    uint32_t video_buffered;
    uint32_t audio_buffered;
    int have_pts;
    int eof;
    int failed;
    int64_t first_pts;
    int64_t last_pts;
} SrPsmfProducerStats;

typedef struct SrPsmfProducer SrPsmfProducer;

/* Opens and validates the 2048-byte PSMF header and its bounded stream extent. */
SrPsmfProducer *sr_psmf_producer_open(const SrPsmfSource *source,
                                      int64_t presentation_base);
void sr_psmf_producer_close(SrPsmfProducer *producer);
void sr_psmf_producer_reset(SrPsmfProducer *producer);

/* Parse at most max_packets syntactic PS elements. A zero return means no AU was
 * produced; callers use stats.failed/eof to distinguish terminal outcomes. */
int sr_psmf_producer_pump(SrPsmfProducer *producer, uint32_t max_packets);

/* Consume each AU exactly once. The returned payload remains producer-owned until
 * sr_psmf_au_release is called. */
int sr_psmf_producer_pop(SrPsmfProducer *producer, SrPsmfAuKind kind,
                         SrPsmfAu *out);
void sr_psmf_au_release(SrPsmfAu *au);
int sr_psmf_producer_eof(const SrPsmfProducer *producer);

/* Terminal rejection queries.  A failed stream never resumes, so failed() stays true
 * until sr_psmf_producer_reset().  fail_reason() names the element the parser refused
 * (a compile-time literal owned by the producer, NULL while the stream is accepted, and
 * NULL for a NULL producer): it exists so a consumer can report *why* a stream stopped
 * instead of leaving the freeze unexplained.  The offset of the refused element is
 * stats().fail_offset. */
int sr_psmf_producer_failed(const SrPsmfProducer *producer);
const char *sr_psmf_producer_fail_reason(const SrPsmfProducer *producer);

void sr_psmf_producer_stats(const SrPsmfProducer *producer,
                            SrPsmfProducerStats *out);

/* Record one exact consumer-side stage event. Decoded outputs must eventually be
 * delivered, explicitly held for warm-up, drained at EOS, or rejected. A held output
 * remains in the conservation total until resolve_warmup_hold() classifies it. */
void sr_psmf_producer_media_stage(SrPsmfProducer *producer, SrPsmfAuKind kind,
                                  SrPsmfMediaStage stage);
int sr_psmf_producer_resolve_warmup_hold(SrPsmfProducer *producer,
                                         SrPsmfAuKind kind,
                                         SrPsmfMediaStage disposition);

/* One presentation clock, advanced the way both A/V tracks of the player advance:
 * an access unit that carries a PES timestamp ANCHORS the clock to the stream's own
 * timeline; a unit without one extrapolates by `step` while an anchor already exists;
 * before the first anchor the clock stays unknown (-1) instead of inventing a time.
 *
 * This is the generic rule that makes a measured video/audio PTS separation mean
 * pipeline distance rather than rate error: every anchor re-syncs the clock to the
 * authored timeline, so between two anchors the worst deviation is one step and a
 * truncating step (e.g. PSMF_AUDIO_PTS_STEP = 4180 for 4180.95) cannot accumulate
 * across anchors.  Returns the clock's new value, or -1 while it is still unknown. */
int64_t sr_psmf_pts_advance(int64_t *clock, int *valid, int has_pts,
                            int64_t pts, int64_t step);

#define SR_PSMF_STREAM_NONE ((uint32_t)-1)

/* Select the stream indices to demux.
 * video_stream_num: 0-based index of the video stream in the PSMF stream table.
 * audio_stream_num: 0-based index of the audio stream in the PSMF stream table,
 *                   or SR_PSMF_STREAM_NONE if no audio is requested.
 * Returns 1 on success, 0 on failure (e.g. out of range). */
int sr_psmf_producer_select_streams(SrPsmfProducer *producer,
                                    uint32_t video_stream_num,
                                    uint32_t audio_stream_num);

uint32_t sr_psmf_producer_video_streams(const SrPsmfProducer *producer);
uint32_t sr_psmf_producer_audio_streams(const SrPsmfProducer *producer);
uint32_t sr_psmf_producer_selected_video_stream(const SrPsmfProducer *producer);
uint32_t sr_psmf_producer_selected_audio_stream(const SrPsmfProducer *producer);

#endif
