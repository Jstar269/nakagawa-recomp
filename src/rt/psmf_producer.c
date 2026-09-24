// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Project-authored PSMF producer boundary.  This layer deliberately stops at
 * compressed access units: decoder success is never inferred from demux success.
 * The source callback is the only filesystem boundary, so ISO, loose VFS, and
 * synthetic memory sources share exactly the same parser and queue semantics.
 *
 * Access-unit formation follows the PSP container contract, not the MPEG-PS
 * packet layout:
 *
 *   video (stream ids 0xE0-0xEF)
 *     The payloads are one continuous annex-B elementary stream and every
 *     picture is introduced by an access-unit delimiter NAL (type 9).  An AU is
 *     the bytes from one delimiter up to (not including) the next, so a PES
 *     packet may hold the tail of one picture and the head of the next.  A
 *     parameter set that precedes the first delimiter belongs to the first AU.
 *
 *   audio (private stream 1, 0xBD)
 *     Each payload is prefixed by the PSP audio sub-header -- one sub-stream id
 *     byte followed by three (four when the sub-stream id is 0xB0-0xBF) header
 *     bytes -- and the remainder is a continuous ATRAC stream of
 *     self-delimiting frames: an 8-byte frame header whose second and third
 *     bytes encode the total frame size, followed by the ATRAC data.  Frames
 *     straddle PES boundaries, so an AU is one complete frame.
 *
 * Presentation times come from the PES packet that contained the AU's *first*
 * byte, which is frequently an earlier packet than the one that closes the AU
 * (and most PSMF video packets carry no PTS at all).  Every append therefore
 * records the byte range it covers together with its PTS, and an AU is emitted
 * with the mark covering its head.  An AU whose packet carried no PTS is
 * reported with has_pts = 0 so the consumer extrapolates from the codec frame
 * step instead of being handed a fabricated time. */

#include "psmf_producer.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PSMF_HEADER_BYTES 2048u
#define PSMF_MAGIC_0 'P'
#define PSMF_MAGIC_1 'S'
#define PSMF_MAGIC_2 'M'
#define PSMF_MAGIC_3 'F'

/* Access-unit delimiter NAL unit type inside the H.264 elementary stream. */
#define H264_NAL_AUD 9u
/* PSP audio frame header signature and its size field encoding.  A frame is
 * ((code1 & 3) << 8 | code2 * 8) + PSMF_AUDIO_SIZE_BASE bytes in total,
 * PSMF_AUDIO_HDR_BYTES of which are the header.  The two constants must stay
 * distinct: using the header length as the size base truncates every frame by
 * eight bytes and desynchronizes the whole stream. */
#define PSMF_AUDIO_HDR_0 0x0Fu
#define PSMF_AUDIO_HDR_1 0xD0u
#define PSMF_AUDIO_HDR_BYTES 8u
#define PSMF_AUDIO_SIZE_BASE 0x10u
#define PSMF_AUDIO_MIN_FRAME 0x10u
#define PSMF_AUDIO_MAX_FRAME 0x0B08u
/* Bounded resync window when the audio stream does not start on a frame header
 * (chapter transitions can leave a few bytes behind). */
#define PSMF_AUDIO_RESYNC_MAX 0x2000u
/* Concurrent PES packets whose presentation times are still needed because the
 * access unit they belong to has not been emitted yet. */
#define SR_PSMF_MAX_MARKS 64
#define PSMF_COMPACT_BYTES 65536u

typedef struct {
    SrPsmfAu entries[SR_PSMF_QUEUE_DEPTH];
    uint32_t head, tail, count;
} AuQueue;

/* One appended PES payload: the byte range it occupies in the track buffer and
 * the presentation time it declared (which may be absent). */
typedef struct {
    uint32_t start, end;
    int has_pts, has_dts;
    int64_t pts, dts;
    uint8_t stream_id;
} PtsMark;

typedef struct {
    uint8_t *buf;                    /* allocation; live bytes are [head, tail) */
    uint32_t cap, head, tail;
    uint32_t video_scan_pos;         /* next absolute candidate in buf */
    int video_have_aud;              /* a first AUD is buffered, awaiting the next */
    PtsMark mark[SR_PSMF_MAX_MARKS];
    int nmarks;
    uint8_t stream_id;
} Track;

#define SR_PSMF_MAX_STREAMS 128u

typedef struct {
    uint8_t stream_id;      /* Video: PES sid (0xE0..0xEF). Audio: e[0] (0xBD, 0xB0..0xBF, 0xF0..0xFF) */
    uint8_t sub_stream_id;  /* Audio: sub-stream id (e.g. 0x00, 0x01, or 0xB0..0xBF) */
    uint8_t valid;
} SrPsmfStreamEntry;

struct SrPsmfProducer {
    SrPsmfSource source;
    uint64_t stream_start, stream_end, cursor;
    int64_t presentation_base;
    uint32_t packet_count;
    int eof, failed;
    Track track[2];
    AuQueue queue[2];
    SrPsmfProducerStats stats;
    uint32_t video_stream_count;
    uint32_t audio_stream_count;
    uint32_t selected_video_stream;
    uint32_t selected_audio_stream;
    int single_stream_compat;
    SrPsmfStreamEntry video_stream_entry[SR_PSMF_MAX_STREAMS];
    SrPsmfStreamEntry audio_stream_entry[SR_PSMF_MAX_STREAMS];
};

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) return 0;
    if (out) *out = a + b;
    return 1;
}

static int source_read(SrPsmfProducer *p, uint64_t off, void *dst,
                       uint32_t want, uint32_t *got) {
    if (!p || !p->source.read || !dst || !got || want == 0 || off >= p->source.size) {
        if (got) *got = 0;
        return off >= (p ? p->source.size : 0) ? SR_PSMF_SOURCE_EOF : SR_PSMF_SOURCE_ERROR;
    }
    uint64_t remain = p->source.size - off;
    uint32_t cap = remain < want ? (uint32_t)remain : want;
    int rc = p->source.read(p->source.opaque, off, dst, cap, got);
    if (rc == SR_PSMF_SOURCE_ERROR || *got > cap) {
        p->stats.source_failures++;
        return SR_PSMF_SOURCE_ERROR;
    }
    p->stats.bytes_read += *got;
    if (rc == SR_PSMF_SOURCE_EOF && *got == 0) return SR_PSMF_SOURCE_EOF;
    if (*got == 0) return SR_PSMF_SOURCE_EOF;
    return SR_PSMF_SOURCE_DATA;
}

static int read_exact(SrPsmfProducer *p, uint64_t off, void *dst, uint32_t n) {
    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0;
    while (done < n) {
        uint32_t got = 0;
        int rc = source_read(p, off + done, out + done, n - done, &got);
        if (rc != SR_PSMF_SOURCE_DATA || got == 0) return 0;
        done += got;
    }
    return 1;
}

static int queue_push(AuQueue *q, SrPsmfAu *au) {
    if (!q || !au || q->count >= SR_PSMF_QUEUE_DEPTH) return 0;
    q->entries[q->tail] = *au;
    memset(au, 0, sizeof(*au));
    q->tail = (q->tail + 1u) % SR_PSMF_QUEUE_DEPTH;
    q->count++;
    return 1;
}

static int queue_pop(AuQueue *q, SrPsmfAu *out) {
    if (!q || !out || q->count == 0) return 0;
    *out = q->entries[q->head];
    memset(&q->entries[q->head], 0, sizeof(q->entries[q->head]));
    q->head = (q->head + 1u) % SR_PSMF_QUEUE_DEPTH;
    q->count--;
    return 1;
}

void sr_psmf_au_release(SrPsmfAu *au) {
    if (!au) return;
    free(au->data);
    memset(au, 0, sizeof(*au));
}

static void queue_clear(AuQueue *q) {
    if (!q) return;
    while (q->count) {
        SrPsmfAu au;
        queue_pop(q, &au);
        sr_psmf_au_release(&au);
    }
    q->head = q->tail = 0;
}

/* ---- per-track elementary byte accumulator ------------------------------------------- */

static uint8_t *track_ptr(Track *t) { return t->buf + t->head; }
static uint32_t track_len(const Track *t) { return t->tail - t->head; }

/* Drop fully consumed history: marks that end at or before the head can never be
 * the mark covering a future access unit. */
static void track_drop_marks(Track *t) {
    int keep = 0;
    for (int i = 0; i < t->nmarks; i++) {
        if (t->mark[i].end <= t->head) continue;
        if (keep != i) t->mark[keep] = t->mark[i];
        keep++;
    }
    t->nmarks = keep;
}

static void track_compact(Track *t) {
    if (t->head == 0) return;
    uint32_t live = track_len(t);
    if (live) memmove(t->buf, t->buf + t->head, live);
    t->video_scan_pos = t->video_scan_pos > t->head
                            ? t->video_scan_pos - t->head : 0;
    for (int i = 0; i < t->nmarks; i++) {
        t->mark[i].start = t->mark[i].start > t->head ? t->mark[i].start - t->head : 0;
        t->mark[i].end -= t->head;
    }
    t->head = 0;
    t->tail = live;
}

static void track_consume(Track *t, uint32_t n) {
    if (n > track_len(t)) n = track_len(t);
    t->head += n;
    if (t->video_scan_pos < t->head) t->video_scan_pos = t->head;
    if (t->head == t->tail) t->video_have_aud = 0;
    track_drop_marks(t);
    if (t->head >= PSMF_COMPACT_BYTES && t->head >= track_len(t)) track_compact(t);
}

static int track_mark_push(Track *t, uint32_t start, uint32_t end,
                           int has_pts, int has_dts, int64_t pts, int64_t dts,
                           uint8_t stream_id) {
    track_drop_marks(t);
    if (t->nmarks == SR_PSMF_MAX_MARKS) {
        /* Nothing may be discarded while it still covers a buffered byte, so the
         * two oldest marks coalesce into the older one's presentation time.  Only
         * an access unit spanning more than SR_PSMF_MAX_MARKS packets can reach
         * this, and the head-covering mark always keeps a real PES time. */
        t->mark[0].end = t->mark[1].end;
        memmove(&t->mark[1], &t->mark[2], (size_t)(t->nmarks - 2) * sizeof(PtsMark));
        t->nmarks--;
    }
    PtsMark *m = &t->mark[t->nmarks++];
    m->start = start;
    m->end = end;
    m->has_pts = has_pts;
    m->has_dts = has_dts;
    m->pts = pts;
    m->dts = dts;
    m->stream_id = stream_id;
    return 1;
}

static int track_reserve(Track *t, uint32_t need) {
    if ((uint64_t)track_len(t) + need > SR_PSMF_MAX_AU_BYTES) return 0;
    if ((uint64_t)t->tail + need <= t->cap) return 1;
    track_compact(t);
    if ((uint64_t)t->tail + need <= t->cap) return 1;
    uint32_t want = t->cap ? t->cap : 65536u;
    while ((uint64_t)want < (uint64_t)t->tail + need) {
        if (want > SR_PSMF_MAX_AU_BYTES / 2u) { want = SR_PSMF_MAX_AU_BYTES + 4096u; break; }
        want *= 2u;
    }
    uint8_t *p = (uint8_t *)realloc(t->buf, (size_t)want);
    if (!p) return 0;
    t->buf = p;
    t->cap = want;
    return 1;
}

static int track_append(Track *t, const uint8_t *p, uint32_t n, uint8_t stream_id,
                        int has_pts, int has_dts, int64_t pts, int64_t dts) {
    if (n == 0) return 1;
    if (!p || !track_reserve(t, n)) return 0;
    t->stream_id = stream_id;
    track_mark_push(t, t->tail, t->tail + n, has_pts, has_dts, pts, dts, stream_id);
    memcpy(t->buf + t->tail, p, n);
    t->tail += n;
    return 1;
}

/* Presentation time of the PES packet that contained the current head byte. */
static const PtsMark *track_head_mark(Track *t) {
    for (int i = 0; i < t->nmarks; i++)
        if (t->mark[i].end > t->head) return &t->mark[i];
    return NULL;
}

/* Index of the next access-unit delimiter start code at or after `from`, or -1.
 * Annex-B permits both 00 00 01 and a leading-zero 00 00 00 01; matching the
 * 00 00 01 suffix accepts both forms and retains the leading zero in the first AU. */
static int find_aud(const uint8_t *b, uint32_t len, uint32_t from,
                    uint64_t *scan_candidates) {
    if (from >= len) return -1;
    for (uint32_t i = from; i + 3u < len; i++) {
        (*scan_candidates)++;
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && (b[i + 3] & 0x1Fu) == H264_NAL_AUD)
            return (int)i;
    }
    return -1;
}

/* A start code may straddle appends. The last three positions cannot form a
 * four-byte candidate yet, so resume there when more bytes arrive. Once a
 * delimiter is found, skip its four bytes just as the original second search
 * did. Positions are absolute within buf and move with track compaction. */
static int find_next_video_aud(SrPsmfProducer *p, Track *t) {
    int found = find_aud(t->buf, t->tail, t->video_scan_pos,
                         &p->stats.video_aud_scan_candidates);
    if (found >= 0) {
        t->video_scan_pos = (uint32_t)found + 4u;
        return found;
    }
    uint32_t resume = t->tail >= 3u ? t->tail - 3u : t->head;
    if (resume < t->head) resume = t->head;
    if (t->video_scan_pos < resume) t->video_scan_pos = resume;
    return -1;
}

/* One complete PSP audio frame at the head of the accumulator.
 * Returns 1 with *au_len set, 0 when more bytes are needed, 2 when unrelated
 * bytes have to be skipped, -1 on a frame header whose size field is outside the
 * field's own domain (the caller fails closed rather than guessing a boundary). */
static int audio_frame_at_head(Track *t, uint32_t *skip, uint32_t *au_len) {
    const uint8_t *b = track_ptr(t);
    uint32_t len = track_len(t);
    *skip = 0;
    *au_len = 0;
    if (len < PSMF_AUDIO_HDR_BYTES) return 0;
    if (b[0] != PSMF_AUDIO_HDR_0 || b[1] != PSMF_AUDIO_HDR_1) {
        uint32_t limit = len < PSMF_AUDIO_RESYNC_MAX ? len : PSMF_AUDIO_RESYNC_MAX;
        for (uint32_t i = 1; i + PSMF_AUDIO_HDR_BYTES <= limit; i++) {
            if (b[i] == PSMF_AUDIO_HDR_0 && b[i + 1] == PSMF_AUDIO_HDR_1) {
                *skip = i;
                goto have_header;
            }
        }
        *skip = limit;
        return 2;
    }
have_header:;
    const uint8_t *h = b + *skip;
    uint32_t size = (uint32_t)(((h[2] & 0x03u) << 8) | ((uint32_t)h[3] * 8u)) + PSMF_AUDIO_SIZE_BASE;
    if (size < PSMF_AUDIO_MIN_FRAME || size > PSMF_AUDIO_MAX_FRAME) return -1;
    if (len - *skip < size) return 0;
    *au_len = size;
    return 1;
}

static int emit_au(SrPsmfProducer *p, SrPsmfAuKind kind, uint32_t len) {
    Track *t = &p->track[kind];
    AuQueue *q = &p->queue[kind];
    const PtsMark *m = track_head_mark(t);
    SrPsmfAu au;
    memset(&au, 0, sizeof(au));
    au.kind = kind;
    au.stream_id = m ? m->stream_id : t->stream_id;
    au.has_pts = (uint8_t)(m ? m->has_pts : 0);
    au.has_dts = (uint8_t)(m ? m->has_dts : 0);
    au.raw_pts = m ? m->pts : 0;
    au.raw_dts = m && m->has_dts ? m->dts : au.raw_pts;
    au.pts = au.raw_pts - p->presentation_base;
    au.dts = au.raw_dts - p->presentation_base;
    if (len > SR_PSMF_MAX_AU_BYTES || !(au.data = (uint8_t *)malloc(len))) {
        p->stats.parser_failures++;
        p->failed = 1;
        return -1;
    }
    memcpy(au.data, track_ptr(t), len);
    au.size = len;
    /* queue_push clears the caller's record, so the statistics are taken from
     * locals rather than from the record after it has been handed over. */
    int has_pts = au.has_pts != 0;
    int64_t normalized_pts = au.pts;
    if (!queue_push(q, &au)) {           /* caller checked backpressure before calling */
        sr_psmf_au_release(&au);
        return 0;
    }
    if (has_pts) {
        if (!p->stats.have_pts) { p->stats.first_pts = normalized_pts; p->stats.have_pts = 1; }
        p->stats.last_pts = normalized_pts;
    } else {
        p->stats.aus_without_pts++;
    }

    if (kind == SR_PSMF_AU_VIDEO) p->stats.video_aus++;
    else p->stats.audio_aus++;
    track_consume(t, len);
    return 1;
}

/* Emit every complete access unit the queue has room for.  Bytes that belong to
 * an incomplete AU stay buffered, so backpressure never drops data. */
static int extract_track(SrPsmfProducer *p, SrPsmfAuKind kind) {
    Track *t = &p->track[kind];
    AuQueue *q = &p->queue[kind];
    int emitted = 0;
    for (;;) {
        if (q->count >= SR_PSMF_QUEUE_DEPTH) return emitted;
        uint32_t au_len = 0;
        if (kind == SR_PSMF_AU_VIDEO) {
            if (!t->video_have_aud) {
                if (find_next_video_aud(p, t) < 0) return emitted;
                t->video_have_aud = 1;
            }
            int a1 = find_next_video_aud(p, t);
            if (a1 < 0) return emitted;             /* picture still open */
            uint32_t rel = (uint32_t)a1 - t->head;
            au_len = (t->buf[a1 - 1] == 0) ? rel - 1u : rel;
        } else {
            uint32_t skip = 0, len = 0;
            int rc = audio_frame_at_head(t, &skip, &len);
            if (rc == 0) return emitted;
            if (rc == -1) { p->stats.parser_failures++; p->failed = 1; return emitted; }
            if (rc == 2) {                          /* unclassifiable bytes: resync */
                p->stats.audio_resync_bytes += skip;
                track_consume(t, skip);
                continue;
            }
            if (skip) {
                p->stats.audio_resync_bytes += skip;
                track_consume(t, skip);
            }
            au_len = len;
        }
        if (au_len == 0 || au_len > track_len(t)) return emitted;
        int r = emit_au(p, kind, au_len);
        if (r <= 0) return emitted;
        emitted++;
    }
}

/* Emit the trailing access unit at end of stream: a final picture has no closing
 * delimiter.  A partial audio frame is dropped because it is not decodable. */
static int drain_track(SrPsmfProducer *p, SrPsmfAuKind kind) {
    Track *t = &p->track[kind];
    AuQueue *q = &p->queue[kind];
    if (kind != SR_PSMF_AU_VIDEO || track_len(t) == 0 || q->count >= SR_PSMF_QUEUE_DEPTH) return 0;
    return emit_au(p, kind, track_len(t)) > 0 ? 1 : 0;
}

/* ---- MPEG program stream ------------------------------------------------------------- */

static int parse_pts(const uint8_t *p, int64_t *out) {
    /* MPEG PTS marker layout: 0010/0011, 3, 15, 15 bits with marker bits.  The
     * 4-bit prefix is deliberately not enforced: PTS_DTS_flags already states which
     * fields follow, and no PSP-muxed evidence yet shows every retail stream sets it
     * exactly (#288).  Marker bits are enforced. */
    if (!p || !out || (p[0] & 1u) == 0 || (p[2] & 1u) == 0 || (p[4] & 1u) == 0)
        return 0;
    *out = ((int64_t)((p[0] >> 1) & 7u) << 30) |
           ((int64_t)p[1] << 22) |
           ((int64_t)((p[2] >> 1) & 0x7fu) << 15) |
           ((int64_t)p[3] << 7) |
           ((p[4] >> 1) & 0x7fu);
    return 1;
}

/* Private stream 1 payload prefix: one sub-stream id byte plus three header bytes
 * (four when the sub-stream id selects the 0xB0-0xBF audio range). */
static uint32_t ps1_header_bytes(uint8_t sub_stream_id) {
    return (sub_stream_id >= 0xB0u && sub_stream_id <= 0xBFu) ? 5u : 4u;
}

static int audio_substream_matches(uint8_t entry_stream_id, uint8_t entry_substream_id, uint8_t pkt_sub_id) {
    if (pkt_sub_id == entry_substream_id) return 1;
    if (pkt_sub_id == entry_stream_id) return 1;
    if ((entry_stream_id & 0xF0u) == 0xB0u && pkt_sub_id == (entry_stream_id & 0x0Fu)) return 1;
    if ((entry_substream_id & 0xF0u) == 0xB0u && pkt_sub_id == (entry_substream_id & 0x0Fu)) return 1;
    if (entry_substream_id < 0x10u && pkt_sub_id == (0xB0u | entry_substream_id)) return 1;
    return 0;
}

static int is_selected_video_stream(const SrPsmfProducer *p, uint8_t sid) {
    if (p->single_stream_compat) {
        return (p->selected_video_stream == 0);
    }
    if (p->selected_video_stream >= p->video_stream_count) return 0;
    return (sid == p->video_stream_entry[p->selected_video_stream].stream_id);
}

static int is_selected_audio_stream(const SrPsmfProducer *p, uint8_t sub_id) {
    if (p->selected_audio_stream == SR_PSMF_STREAM_NONE) return 0;
    if (p->single_stream_compat) {
        return (p->selected_audio_stream == 0);
    }
    if (p->selected_audio_stream >= p->audio_stream_count) return 0;
    const SrPsmfStreamEntry *entry = &p->audio_stream_entry[p->selected_audio_stream];
    return audio_substream_matches(entry->stream_id, entry->sub_stream_id, sub_id);
}

static int parse_one(SrPsmfProducer *p) {
    uint8_t h[32];
    if (p->cursor >= p->stream_end) { p->eof = 1; return 0; }
    uint64_t left = p->stream_end - p->cursor;
    if (left < 4 || !read_exact(p, p->cursor, h, 4)) return -1;
    if (h[0] != 0 || h[1] != 0 || h[2] != 1) return -1;
    uint8_t sid = h[3];
    if (sid == 0xB9u) {                 /* program_end_code terminates the program */
        p->cursor += 4u;
        p->eof = 1;
        return 0;
    }
    if (sid == 0xBAu) {
        uint32_t total;
        if (left < 12 || !read_exact(p, p->cursor + 4, h + 4, 8)) return -1;
        if ((h[4] & 0xc0u) == 0x40u) {
            if (left < 14 || !read_exact(p, p->cursor + 12, h + 12, 2)) return -1;
            total = 14u + (h[13] & 7u);
        } else if ((h[4] & 0xF0u) == 0x20u) {
            total = 12u;
        } else {
            return -1;
        }
        if (left < total) return -1;
        p->cursor += total;
        p->stats.packs++;
        return 1;
    }
    if (sid == 0xBBu || sid == 0xBEu || sid == 0xBFu ||
        sid == 0xF0u || sid == 0xF1u || sid == 0xFFu) {
        if (left < 6 || !read_exact(p, p->cursor + 4, h + 4, 2)) return -1;
        uint32_t len = be16(h + 4);
        uint64_t total;
        if (!add_u64(6u, len, &total) || total > left) return -1;
        p->cursor += total;
        if (sid == 0xBBu) p->stats.packs++;
        return 1;
    }
    if ((sid >= 0xE0u && sid <= 0xEFu) || sid == 0xBDu) {
        int is_video = (sid >= 0xE0u && sid <= 0xEFu);
        SrPsmfAuKind kind = is_video ? SR_PSMF_AU_VIDEO : SR_PSMF_AU_AUDIO;
        if (left < 9 || !read_exact(p, p->cursor + 4, h + 4, 5)) return -1;
        uint32_t len = be16(h + 4);
        /* h[4..5] PES length, h[6] flags1 (its top two bits are the mandatory '10'
         * MPEG-2 marker), h[7] flags2 (PTS_DTS_flags are its top two bits),
         * h[8] header_data_length.  The marker byte is not a timestamp byte: it is
         * set on every packet of the format, so reading the flags out of it reports a
         * presentation time on packets that carry none and loses the DTS of every
         * packet that has one.  Each byte is asserted where it belongs. */
        uint8_t flags1 = h[6], flags2 = h[7], hdr_len = h[8];
        if ((flags1 & 0xc0u) != 0x80u) return -1;   /* not MPEG-2 PES: fail closed */
        unsigned pts_dts = (unsigned)((flags2 >> 6) & 0x3u);
        if (pts_dts == 1u) return -1;               /* '01' is reserved */
        uint64_t total;
        if (len < 3u || (uint32_t)3u + hdr_len > len ||
            !add_u64(6u, len, &total) || total > left) return -1;
        uint32_t payload_len = len - 3u - hdr_len;
        uint64_t payload_off = p->cursor + 9u + hdr_len;
        if (payload_len > SR_PSMF_MAX_AU_BYTES) return -1;
        uint8_t *payload = payload_len ? (uint8_t *)malloc(payload_len) : NULL;
        if (payload_len && !payload) return -1;
        if (payload_len && !read_exact(p, payload_off, payload, payload_len)) {
            free(payload); return -1;
        }
        /* PTS_DTS_flags is the packet's own statement about which timestamps follow,
         * and a packet that declares none is left with none: the consumer extrapolates
         * from the codec frame step rather than being handed an invented time.  A packet
         * whose flags promise a field its declared header cannot hold is contradictory,
         * so it fails closed instead of reading payload bytes as a time. */
        int has_pts = pts_dts == 2u || pts_dts == 3u;
        int has_dts = pts_dts == 3u;
        if (hdr_len == 0u && pts_dts != 0u) { free(payload); return -1; }
        if (has_pts && hdr_len < 5u) { free(payload); return -1; }
        if (has_dts && hdr_len < 10u) { free(payload); return -1; }
        int64_t pts = 0, dts = 0;
        uint32_t optional = p->cursor + 9u;
        if (has_pts && !read_exact(p, optional, h + 9, 5)) { free(payload); return -1; }
        if (has_pts && !parse_pts(h + 9, &pts)) { free(payload); return -1; }
        if (has_dts && !read_exact(p, optional + 5u, h + 14, 5)) { free(payload); return -1; }
        if (has_dts && !parse_pts(h + 14, &dts)) { free(payload); return -1; }
        p->cursor += total;
        p->stats.pes_packets++;
        if (is_video) p->stats.video_pes++; else p->stats.audio_pes++;
        if (has_dts) p->stats.pes_with_dts++;

        uint32_t skip = 0;
        uint8_t actual_id = sid;
        if (!is_video) {
            if (payload_len < 4u) { free(payload); return -1; }
            actual_id = payload[0];
            skip = ps1_header_bytes(actual_id);
            if (skip > payload_len) { free(payload); return -1; }
        }

        int selected = is_video ? is_selected_video_stream(p, sid)
                                : is_selected_audio_stream(p, actual_id);
        if (!selected) {
            free(payload);
            return 1;
        }

        int ok = track_append(&p->track[kind], payload + skip, payload_len - skip, actual_id,
                              has_pts, has_dts, pts, dts);
        free(payload);
        if (!ok) { p->stats.parser_failures++; p->failed = 1; return -1; }
        return 1;
    }
    return -1; /* unknown start-code stream is rejected rather than skipped blindly */
}

SrPsmfProducer *sr_psmf_producer_open(const SrPsmfSource *source,
                                      int64_t presentation_base) {
    if (!source || !source->read || source->size < PSMF_HEADER_BYTES) return NULL;
    if (presentation_base < -(INT64_C(1) << 33)) return NULL;
    uint8_t header[PSMF_HEADER_BYTES];
    SrPsmfProducer *p = (SrPsmfProducer *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->source = *source;
    p->presentation_base = presentation_base;
    if (!read_exact(p, 0, header, sizeof(header)) ||
        header[0] != PSMF_MAGIC_0 || header[1] != PSMF_MAGIC_1 ||
        header[2] != PSMF_MAGIC_2 || header[3] != PSMF_MAGIC_3) {
        free(p); return NULL;
    }
    uint64_t start = be32(header + 8), size = be32(header + 12), end;
    if (start < PSMF_HEADER_BYTES || size == 0 || !add_u64(start, size, &end) ||
        end > source->size) { free(p); return NULL; }
    p->stream_start = start;
    p->stream_end = end;
    p->cursor = start;

    uint32_t streams = be16(header + 0x80);
    if (streams > 128u || 0x82u + streams * 16u > PSMF_HEADER_BYTES) {
        free(p); return NULL;
    }
    if (streams == 0) {
        p->single_stream_compat = 1;
        p->video_stream_count = 1;
        p->audio_stream_count = 1;
        p->selected_video_stream = 0;
        p->selected_audio_stream = 0;
    } else {
        for (uint32_t i = 0; i < streams; i++) {
            const uint8_t *e = header + 0x82u + i * 16u;
            if ((e[0] & 0xE0u) == 0xE0u) {
                if (p->video_stream_count < SR_PSMF_MAX_STREAMS) {
                    uint8_t vid_id = e[0];
                    if (e[0] == 0xE0u && (e[1] & 0x0Fu)) {
                        vid_id = 0xE0u | (e[1] & 0x0Fu);
                    }
                    p->video_stream_entry[p->video_stream_count].stream_id = vid_id;
                    p->video_stream_entry[p->video_stream_count].sub_stream_id = e[1];
                    p->video_stream_entry[p->video_stream_count].valid = 1;
                    p->video_stream_count++;
                }
            } else if ((e[0] & 0xF0u) == 0xB0u || (e[0] & 0xF0u) == 0xF0u || e[0] == 0xBDu) {
                if (p->audio_stream_count < SR_PSMF_MAX_STREAMS) {
                    uint8_t sub_id = (e[0] == 0xBDu) ? e[1] : (e[1] != 0 ? e[1] : e[0]);
                    p->audio_stream_entry[p->audio_stream_count].stream_id = e[0];
                    p->audio_stream_entry[p->audio_stream_count].sub_stream_id = sub_id;
                    p->audio_stream_entry[p->audio_stream_count].valid = 1;
                    p->audio_stream_count++;
                }
            }
        }
        if (p->video_stream_count == 0) {
            free(p); return NULL;
        }
        p->selected_video_stream = 0;
        p->selected_audio_stream = p->audio_stream_count > 0 ? 0 : SR_PSMF_STREAM_NONE;
    }

    return p;
}

void sr_psmf_producer_close(SrPsmfProducer *p) {
    if (!p) return;
    queue_clear(&p->queue[SR_PSMF_AU_VIDEO]);
    queue_clear(&p->queue[SR_PSMF_AU_AUDIO]);
    for (int i = 0; i < 2; i++) free(p->track[i].buf);
    free(p);
}

void sr_psmf_producer_reset(SrPsmfProducer *p) {
    if (!p) return;
    queue_clear(&p->queue[SR_PSMF_AU_VIDEO]);
    queue_clear(&p->queue[SR_PSMF_AU_AUDIO]);
    for (int i = 0; i < 2; i++) {
        p->track[i].head = p->track[i].tail = 0;
        p->track[i].nmarks = 0;
        p->track[i].video_scan_pos = 0;
        p->track[i].video_have_aud = 0;
    }
    p->cursor = p->stream_start; p->eof = p->failed = 0;
    memset(&p->stats, 0, sizeof(p->stats));
}

int sr_psmf_producer_pump(SrPsmfProducer *p, uint32_t max_packets) {
    if (!p || p->failed || p->eof || max_packets == 0) return 0;
    int produced = 0;
    for (uint32_t i = 0; i < max_packets; i++) {
        /* Emit first so the accumulators never hold more than one packet's worth
         * while a queue is full: backpressure stops parsing instead of dropping. */
        produced += extract_track(p, SR_PSMF_AU_VIDEO);
        produced += extract_track(p, SR_PSMF_AU_AUDIO);
        if (p->failed) break;
        if (p->queue[SR_PSMF_AU_VIDEO].count >= SR_PSMF_QUEUE_DEPTH ||
            p->queue[SR_PSMF_AU_AUDIO].count >= SR_PSMF_QUEUE_DEPTH) break;
        int rc = parse_one(p);
        if (rc < 0) {
            /* Record where the stream stopped being parseable: the acceptance runs
             * need the offset, not just a failure count. */
            p->stats.parser_failures++;
            p->stats.fail_offset = p->cursor;
            p->failed = 1;
            break;
        }
        if (rc == 0) break;
    }
    produced += extract_track(p, SR_PSMF_AU_VIDEO);
    produced += extract_track(p, SR_PSMF_AU_AUDIO);
    if (p->eof) {
        produced += drain_track(p, SR_PSMF_AU_VIDEO);
        produced += drain_track(p, SR_PSMF_AU_AUDIO);
    }
    p->stats.video_depth = p->queue[SR_PSMF_AU_VIDEO].count;
    p->stats.audio_depth = p->queue[SR_PSMF_AU_AUDIO].count;
    p->stats.video_buffered = track_len(&p->track[SR_PSMF_AU_VIDEO]);
    p->stats.audio_buffered = track_len(&p->track[SR_PSMF_AU_AUDIO]);
    return produced;
}

int sr_psmf_producer_pop(SrPsmfProducer *p, SrPsmfAuKind kind, SrPsmfAu *out) {
    if (!p || !out || kind > SR_PSMF_AU_AUDIO) return 0;
    int rc = queue_pop(&p->queue[kind], out);
    p->stats.video_depth = p->queue[SR_PSMF_AU_VIDEO].count;
    p->stats.audio_depth = p->queue[SR_PSMF_AU_AUDIO].count;
    return rc;
}

int sr_psmf_producer_eof(const SrPsmfProducer *p) { return p ? p->eof : 0; }

void sr_psmf_producer_stats(const SrPsmfProducer *p, SrPsmfProducerStats *out) {
    if (!out) return;
    if (!p) { memset(out, 0, sizeof(*out)); return; }
    *out = p->stats;
    out->eof = p->eof;
    out->failed = p->failed;
}

int sr_psmf_producer_select_streams(SrPsmfProducer *p, uint32_t vs, uint32_t as) {
    if (!p) return 0;
    if (vs >= p->video_stream_count) return 0;
    if (as != SR_PSMF_STREAM_NONE) {
        if (p->audio_stream_count == 0 || as >= p->audio_stream_count) return 0;
    }
    p->selected_video_stream = vs;
    p->selected_audio_stream = as;
    sr_psmf_producer_reset(p);
    return 1;
}

uint32_t sr_psmf_producer_video_streams(const SrPsmfProducer *p) {
    return p ? p->video_stream_count : 0;
}

uint32_t sr_psmf_producer_audio_streams(const SrPsmfProducer *p) {
    return p ? p->audio_stream_count : 0;
}

uint32_t sr_psmf_producer_selected_video_stream(const SrPsmfProducer *p) {
    return p ? p->selected_video_stream : 0;
}

uint32_t sr_psmf_producer_selected_audio_stream(const SrPsmfProducer *p) {
    return p ? p->selected_audio_stream : SR_PSMF_STREAM_NONE;
}
