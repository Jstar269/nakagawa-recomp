// SPDX-License-Identifier: GPL-2.0-or-later
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
void sr_psmf_producer_stats(const SrPsmfProducer *producer,
                            SrPsmfProducerStats *out);

#endif
