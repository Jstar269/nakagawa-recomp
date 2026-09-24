// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Source-owned selftest for the bounded PSMF producer.  Every byte here is
 * synthesised by this file: no retail media, no extracted capture.  The fixture
 * exercises the PSP access-unit contract (aud-delimited pictures, ATRAC frame
 * records, private-stream-1 sub-header) including the cases that matter most:
 * one picture spanning several PES packets, an access unit whose starting packet
 * carries no PTS, frames straddling PES boundaries, and malformed input. */

#include "psmf_producer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(x, text) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL: %s\n", text); } } while (0)

typedef struct { uint8_t *data; uint32_t size; uint32_t max_chunk; int fail; } MemSource;
static int mem_read(void *opaque, uint64_t off, void *dst, uint32_t cap, uint32_t *got) {
    MemSource *m = (MemSource *)opaque;
    if (!m || !dst || !got || off > m->size) return SR_PSMF_SOURCE_ERROR;
    if (m->fail) return SR_PSMF_SOURCE_ERROR;
    if (off == m->size) { *got = 0; return SR_PSMF_SOURCE_EOF; }
    uint32_t n = m->size - (uint32_t)off;
    if (n > cap) n = cap;
    if (m->max_chunk && n > m->max_chunk) n = m->max_chunk;
    memcpy(dst, m->data + (uint32_t)off, n);
    *got = n;
    return SR_PSMF_SOURCE_DATA;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static void put_pts(uint8_t *p, int64_t v, uint8_t prefix) {
    uint64_t x = (uint64_t)v;
    p[0] = (uint8_t)(prefix | (((x >> 30) & 7u) << 1) | 1u);
    p[1] = (uint8_t)(x >> 22);
    p[2] = (uint8_t)(((x >> 15) & 0x7fu) << 1 | 1u);
    p[3] = (uint8_t)(x >> 7);
    p[4] = (uint8_t)(((x & 0x7fu) << 1) | 1u);
}

typedef struct { uint8_t *d; uint32_t at, cap; int overflow; } Buf;
static void raw(Buf *b, const uint8_t *p, uint32_t n) {
    if (b->at + n > b->cap) { b->overflow = 1; return; }
    memcpy(b->d + b->at, p, n);
    b->at += n;
}
static void bytes(Buf *b, uint8_t v, uint32_t n) {
    if (b->at + n > b->cap) { b->overflow = 1; return; }
    memset(b->d + b->at, v, n);
    b->at += n;
}
static void add_pack(Buf *b) {
    static const uint8_t pack[14] = {0,0,1,0xba,0x44,0,4,0,4,1,0,0,1,0};
    raw(b, pack, sizeof(pack));
}
/* PTS_DTS_flags, as the packet's second flags byte carries them. */
#define PES_TS_NONE     0
#define PES_TS_RESERVED 1
#define PES_TS_PTS      2
#define PES_TS_BOTH     3

static void check_pes_wellformed(const uint8_t *s, uint32_t at, int pts_dts, uint32_t hdr_len,
                                 int64_t pts, int64_t dts, uint32_t payload_len);
static int64_t read_pts(const uint8_t *p);

/* One MPEG-PS PES packet written field by field.
 *
 * The two flags bytes are independent inputs on purpose.  The first carries the
 * mandatory '10' MPEG-2 marker, measured to be set on all 10,323 media packets of a
 * PSP movie -- including the 9,521 video packets whose optional-header length is zero
 * -- so a fixture that wrote 0x00 there would exercise a form the format never uses.
 * The second carries PTS_DTS_flags, which is the packet's only statement about which
 * timestamps follow.  Folding both facts into one byte, as an earlier version of this
 * fixture did, teaches the parser a layout no stream uses and hides the mistake behind
 * green tests.
 *
 * `hdr_len` is the optional-header length the packet declares; the remainder after the
 * timestamp fields is 0xFF stuffing, the standard's filler.  The retail movie pads the
 * same lengths with a PES extension instead, which is equivalent here: the payload
 * offset comes from the declared length, never from the flags. */
static void add_pes_ex(Buf *b, uint8_t sid, int pts_dts, uint32_t hdr_len,
                       int64_t pts, int64_t dts, int marker_valid,
                       const uint8_t *payload, uint32_t payload_len) {
    uint32_t len = 3u + hdr_len + payload_len;
    uint8_t hdr[9];
    hdr[0]=0; hdr[1]=0; hdr[2]=1; hdr[3]=sid;
    hdr[4]=(uint8_t)(len>>8); hdr[5]=(uint8_t)len;
    hdr[6]=(uint8_t)(marker_valid ? 0x80u : 0x00u);
    hdr[7]=(uint8_t)((unsigned)pts_dts << 6);
    hdr[8]=(uint8_t)hdr_len;
    uint32_t hdr_at = b->at;
    raw(b, hdr, 9);
    uint32_t need = 0;
    if (pts_dts == PES_TS_PTS || pts_dts == PES_TS_BOTH) need += 5u;
    if (pts_dts == PES_TS_BOTH) need += 5u;
    uint32_t written = 0;
    if (need && hdr_len < need) {
        /* Deliberately contradictory: the flags promise a timestamp the declared header
         * cannot hold.  Nothing is written for it, so the packet is malformed in exactly
         * the way the flags claim -- a packet that also carried stray field bytes would
         * fail later for an unrelated reason and make this case prove nothing. */
        need = 0;
    } else {
        if (pts_dts == PES_TS_PTS || pts_dts == PES_TS_BOTH) {
            uint8_t p[5]; put_pts(p, pts, 0x21); raw(b, p, 5); written += 5u;
        }
        if (pts_dts == PES_TS_BOTH) {
            uint8_t p[5]; put_pts(p, dts, 0x31); raw(b, p, 5); written += 5u;
        }
    }
    if (hdr_len > written) bytes(b, 0xffu, hdr_len - written);
    /* Validated after the optional header is present, so the check reads the bytes the
     * parser will read rather than the payload that follows them.  A packet this fixture
     * malformed on purpose is exempt: the check asserts the well-formed form. */
    if (!b->overflow && marker_valid && hdr_len >= written)
        check_pes_wellformed(b->d + hdr_at, hdr_at, pts_dts, hdr_len, pts, dts, payload_len);
    raw(b, payload, payload_len);
}
/* One PES packet carrying a PTS, or none at all. */
static void add_pes(Buf *b, uint8_t sid, int has_pts, int64_t pts,
                    const uint8_t *payload, uint32_t payload_len) {
    if (has_pts) add_pes_ex(b, sid, PES_TS_PTS, 5u, pts, 0, 1, payload, payload_len);
    else         add_pes_ex(b, sid, PES_TS_NONE, 0u, 0, 0, 1, payload, payload_len);
}

/* ---- fixture self-validation ------------------------------------------------------------
 * The fixture checks its own packets before a test reads them.  A PES header is
 * small enough to get wrong silently -- an earlier version of this file wrote the
 * flags byte without its '10' marker and the failure surfaced as a demux symptom
 * rather than as a fixture bug -- so the packet walk below asserts the start-code
 * prefix, the stream id, the marker bits, the optional-header length and that the
 * declared length accounts for exactly the bytes the builder wrote. */
static void check_pes_wellformed(const uint8_t *s, uint32_t at, int pts_dts, uint32_t hdr_len,
                                 int64_t pts, int64_t dts, uint32_t payload_len) {
    CHECK(s[0] == 0u && s[1] == 0u && s[2] == 1u, "PES start-code prefix is 00 00 01");
    CHECK(s[3] == 0xe0u || s[3] == 0xbdu, "PES stream id is video or private stream 1");
    uint32_t declared = ((uint32_t)s[4] << 8) | s[5];
    CHECK(declared == 3u + hdr_len + payload_len,
          "declared PES length accounts for exactly the bytes written");
    /* The two flags bytes are asserted independently.  The first is the mandatory MPEG-2
     * marker, the second is the packet's own statement about its timestamps; a fixture
     * that wrote only one of the two cannot pass both assertions. */
    CHECK((s[6] & 0xc0u) == 0x80u, "first flags byte carries the mandatory '10' marker bits");
    CHECK(((s[7] >> 6) & 0x3u) == (uint32_t)pts_dts,
          "second flags byte states the PTS/DTS combination the builder wrote");
    CHECK(s[8] == hdr_len, "the declared optional-header length is what the builder wrote");
    /* A timestamp field is present when the flags promise one and the declared header can
     * hold it; a packet where the two disagree is the malformed form a test drives on
     * purpose, and no field assertion applies to it. */
    uint32_t need = (pts_dts == PES_TS_PTS || pts_dts == PES_TS_BOTH ? 5u : 0u) +
                    (pts_dts == PES_TS_BOTH ? 5u : 0u);
    int fields_present = need > 0u && hdr_len >= need;
    int has_pts = fields_present && (pts_dts == PES_TS_PTS || pts_dts == PES_TS_BOTH);
    int has_dts = fields_present && pts_dts == PES_TS_BOTH;
    if (has_pts) {
        CHECK((s[9] & 0xf0u) == 0x20u, "PTS field carries the '0010' prefix");
        CHECK((s[9] & 1u) != 0u && (s[11] & 1u) != 0u && (s[13] & 1u) != 0u,
              "PTS field carries all three marker bits");
        CHECK(read_pts(s + 9) == pts, "the PTS field encodes the value the builder declared");
    }
    if (has_dts) {
        CHECK((s[14] & 0xf0u) == 0x30u, "DTS field carries the '0011' prefix");
        CHECK((s[14] & 1u) != 0u && (s[16] & 1u) != 0u && (s[18] & 1u) != 0u,
              "DTS field carries all three marker bits");
        CHECK(read_pts(s + 14) == dts, "the DTS field encodes the value the builder declared");
    }
    (void)at;
}
/* Decode a timestamp field back out of the bytes the builder wrote, so the fixture checks
 * the value it declared and not only the shape of the field. */
static int64_t read_pts(const uint8_t *p) {
    return ((int64_t)((p[0] >> 1) & 7u) << 30) | ((int64_t)p[1] << 22) |
           ((int64_t)((p[2] >> 1) & 0x7fu) << 15) | ((int64_t)p[3] << 7) |
           ((int64_t)((p[4] >> 1) & 0x7fu));
}
/* Private stream 1 payload: sub-stream id byte + three sub-header bytes + ES. */
static void add_audio_pes(Buf *b, int has_pts, int64_t pts, const uint8_t *es, uint32_t es_len) {
    uint8_t prefix[4] = {0x00, 0x00, 0x00, 0x00};
    uint32_t len = 4u + es_len;
    uint8_t *payload = (uint8_t *)malloc(len);
    if (!payload) return;
    memcpy(payload, prefix, 4);
    memcpy(payload + 4, es, es_len);
    add_pes(b, 0xbd, has_pts, pts, payload, len);
    free(payload);
}
/* One PSP ATRAC frame: an 8-byte header whose size field is
 * ((code1 & 3) << 8 | code2 * 8) + 0x10 total bytes, then that many minus the
 * header in frame data. */
static void audio_frame(Buf *b, uint8_t code1, uint8_t code2, uint8_t fill) {
    uint8_t h[8] = {0x0f, 0xd0, code1, code2, 0, 0, 0, 0};
    uint32_t total = (uint32_t)(((code1 & 0x03u) << 8) | ((uint32_t)code2 * 8u)) + 0x10u;
    raw(b, h, 8);
    bytes(b, fill, total - 8u);
}

static const uint8_t k_picture1_body[3] = {0x11, 0x11, 0x11};
static const uint8_t k_picture2_body[5] = {0x22, 0x22, 0x22, 0x22, 0x22};
static const uint8_t k_picture3_body[2] = {0x33, 0x33};
static const uint8_t k_aud[4] = {0, 0, 1, 0x09};
static const uint8_t k_sps[6] = {0, 0, 1, 0x67, 0x4d, 0x40};

/* Build a 4-picture annex-B stream split over three video PES packets, plus two
 * ATRAC frames spread over two audio PES packets. */
static uint8_t *fixture(uint32_t *size_out, int variant) {
    Buf b;
    b.cap = variant == 7 ? 131072u : 65536u;
    b.d = (uint8_t *)calloc(1, b.cap);
    b.at = 2048;
    b.overflow = 0;
    if (!b.d) return NULL;
    b.d[0]='P'; b.d[1]='S'; b.d[2]='M'; b.d[3]='F';
    be32(b.d + 8, 2048);

    if (variant == 4) {                     /* many pictures: queue backpressure */
        for (int i = 0; i < 12; i++) {
            add_pack(&b);
            add_pes(&b, 0xe0, 0, 0, k_aud, 4);
            add_pes(&b, 0xe0, 0, 0, k_picture1_body, sizeof(k_picture1_body));
        }
        be32(b.d + 12, b.at - 2048);
        *size_out = b.at;
        CHECK(!b.overflow, "fixture stream fits its buffer");
        return b.d;
    }

    if (variant == 5) {                     /* many tiny video PES, no AUD */
        uint8_t payload[16];
        memset(payload, 0x55, sizeof(payload));
        for (int i = 0; i < 1024; i++)
            add_pes(&b, 0xe0, 0, 0, payload, sizeof(payload));
        be32(b.d + 12, b.at - 2048);
        *size_out = b.at;
        CHECK(!b.overflow, "no-AUD fixture stream fits its buffer");
        return b.d;
    }

    if (variant == 6) {                     /* both AUDs cross PES cuts */
        static const uint8_t first[] = {0x67, 0x11, 0, 0};
        static const uint8_t second[] = {1, 9, 0x21, 0x22, 0};
        static const uint8_t third[] = {0, 1, 9, 0x33};
        add_pes(&b, 0xe0, 1, 90000, first, sizeof(first));
        add_pes(&b, 0xe0, 0, 0, second, sizeof(second));
        add_pes(&b, 0xe0, 0, 0, third, sizeof(third));
        be32(b.d + 12, b.at - 2048);
        *size_out = b.at;
        CHECK(!b.overflow, "split-AUD fixture stream fits its buffer");
        return b.d;
    }

    if (variant == 7) {                     /* emit enough AUs to compact */
        uint8_t payload[128];
        memset(payload, 0x55, sizeof(payload));
        memcpy(payload, k_aud, sizeof(k_aud));
        for (int i = 0; i < 600; i++)
            add_pes(&b, 0xe0, 0, 0, payload, sizeof(payload));
        be32(b.d + 12, b.at - 2048);
        *size_out = b.at;
        CHECK(!b.overflow, "compaction fixture stream fits its buffer");
        return b.d;
    }

    /* ---- video elementary stream: sps, (aud, body) x 4 ---- */
    Buf es = { NULL, 0, 0, 0 };
    uint8_t esbuf[512];
    es.d = esbuf; es.cap = sizeof(esbuf);
    raw(&es, k_sps, sizeof(k_sps));
    raw(&es, k_aud, 4); raw(&es, k_picture1_body, sizeof(k_picture1_body));
    raw(&es, k_aud, 4); raw(&es, k_picture2_body, sizeof(k_picture2_body));
    raw(&es, k_aud, 4); raw(&es, k_picture3_body, sizeof(k_picture3_body));
    raw(&es, k_aud, 4); raw(&es, k_picture1_body, sizeof(k_picture1_body));
    CHECK(!es.overflow, "fixture video ES fits its buffer");

    /* ---- audio elementary stream: two complete frames plus a truncated tail ---- */
    uint8_t abuf[512];
    Buf aes = { abuf, 0, sizeof(abuf), 0 };
    audio_frame(&aes, 0x28, 6, 0xa5);      /* 64 bytes total */
    audio_frame(&aes, 0x28, 7, 0x5a);      /* 72 bytes total */
    CHECK(!aes.overflow, "fixture audio ES fits its buffer");

    add_pack(&b);
    /* video packet 1: sps + first aud + part of picture 1 (PTS 90000) */
    add_pes(&b, 0xe0, 1, 90000, es.d, 6 + 4 + 2);
    /* video packet 2: rest of picture 1, aud + picture 2, no PTS at all */
    add_pes(&b, 0xe0, 0, 0, es.d + 12, 2 + 4 + 5);
    add_pack(&b);
    /* video packet 3: aud + picture 3 + aud + picture 4 (PTS 99009) */
    add_pes(&b, 0xe0, 1, 99009, es.d + 23, (uint32_t)(es.at - 23));
    /* audio packet 1: first frame only, PTS 85069 */
    add_audio_pes(&b, 1, 85069, aes.d, 64);
    /* audio packet 2: second frame plus a truncated third frame, no PTS */
    add_pack(&b);
    add_audio_pes(&b, 0, 0, aes.d + 64, aes.at - 64);

    if (variant == 1) {                     /* impossible declared PES length */
        uint8_t bad[6] = {0, 0, 1, 0xe0, 0xff, 0xff};
        raw(&b, bad, sizeof(bad));
    } else if (variant == 2) {              /* unsupported stream id */
        uint8_t bad[6] = {0, 0, 1, 0x01, 0x00, 0x00};
        raw(&b, bad, sizeof(bad));
    } else if (variant == 3) {              /* audio payload shorter than the sub-header */
        uint8_t bad[1] = {0x00};
        add_pes(&b, 0xbd, 0, 0, bad, 1);
    }
    be32(b.d + 12, b.at - 2048);
    *size_out = b.at;
    CHECK(!b.overflow, "fixture stream fits its buffer");
    return b.d;
}

/* Drain one track, appending each access-unit size from *count onwards. */
static int pop_into(SrPsmfProducer *p, SrPsmfAuKind kind, uint32_t sizes[], int *count, int max) {
    SrPsmfAu au;
    int n = 0;
    while (sr_psmf_producer_pop(p, kind, &au)) {
        if (sizes && *count + n < max) sizes[*count + n] = au.size;
        n++;
        sr_psmf_au_release(&au);
    }
    *count += n;
    return n;
}

static void test_access_unit_formation(void) {
    uint32_t size = 0; uint8_t *bytes = fixture(&size, 0);
    MemSource mem = {bytes, size, 7, 0};      /* fragmented reads: nothing assumed whole */
    SrPsmfSource source = {mem_read, &mem, size};
    SrPsmfProducer *p = sr_psmf_producer_open(&source, 90000);
    CHECK(p != NULL, "synthetic PSMF opens");
    if (!p) { free(bytes); return; }
    for (int i = 0; i < 64 && !sr_psmf_producer_eof(p); i++) sr_psmf_producer_pump(p, 1);
    SrPsmfProducerStats st; sr_psmf_producer_stats(p, &st);
    if (getenv("SR_PSMF_SELFTEST_DEBUG"))
        fprintf(stderr, "DEBUG pes=%llu vpes=%llu apes=%llu vau=%llu aau=%llu pf=%llu sf=%llu failed=%d eof=%d bytes=%llu clk=%llu resync=%llu\n",
                (unsigned long long)st.pes_packets, (unsigned long long)st.video_pes,
                (unsigned long long)st.audio_pes, (unsigned long long)st.video_aus,
                (unsigned long long)st.audio_aus, (unsigned long long)st.parser_failures,
                (unsigned long long)st.source_failures, st.failed, st.eof,
                (unsigned long long)st.bytes_read, (unsigned long long)st.aus_without_pts,
                (unsigned long long)st.audio_resync_bytes);
    if (getenv("SR_PSMF_SELFTEST_DEBUG"))
        fprintf(stderr, "DEBUG pts have=%d first=%lld last=%lld\n", st.have_pts,
                (long long)st.first_pts, (long long)st.last_pts);
    CHECK(st.packs == 3, "every pack boundary parsed");
    CHECK(st.pes_packets == 5 && st.video_pes == 3 && st.audio_pes == 2, "PES packets split by stream");
    CHECK(st.video_aus == 4, "one video AU per access-unit delimiter");
    CHECK(st.audio_aus == 2, "one audio AU per ATRAC frame");
    CHECK(st.bytes_read > 2048, "fragmented source was consumed");
    CHECK(st.failed == 0, "well-formed fixture never fails closed");
    CHECK(st.eof, "EOF is reported after drain");

    SrPsmfAu au;
    CHECK(sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au), "video queue delivers");
    CHECK(au.size == 6 + 4 + 3, "picture 1 spans two PES packets and keeps the leading sps");
    CHECK(au.data && au.data[0] == 0 && au.data[1] == 0 && au.data[2] == 1 && au.data[3] == 0x67,
          "picture 1 begins with the pre-delimiter parameter set");
    CHECK(au.has_pts && au.raw_pts == 90000 && au.pts == 0, "picture 1 keeps its raw and normalized PTS");
    sr_psmf_au_release(&au);

    CHECK(sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au), "second picture queued");
    CHECK(au.size == 4 + 5, "picture 2 is delimited by its own access-unit delimiter");
    CHECK(!au.has_pts, "picture 2 carries no PTS (consumer extrapolates)");
    sr_psmf_au_release(&au);

    CHECK(sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au), "third picture queued");
    CHECK(au.size == 4 + 2, "picture 3 sized from its delimiter");
    sr_psmf_au_release(&au);
    CHECK(sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au), "trailing picture is closed at EOF");
    CHECK(au.has_pts && au.raw_pts == 99009 && au.pts == 99009 - 90000, "trailing picture keeps its packet PTS");
    CHECK(au.size == 4 + 3 && au.data && au.data[4] == 0x11, "trailing picture payload survives");
    sr_psmf_au_release(&au);

    CHECK(sr_psmf_producer_pop(p, SR_PSMF_AU_AUDIO, &au), "first ATRAC frame queued");
    if (getenv("SR_PSMF_SELFTEST_DEBUG") && au.data)
        fprintf(stderr, "DEBUG audio AU0 size=%u has_pts=%u raw=%lld first=%02x %02x %02x %02x\n",
                au.size, (unsigned)au.has_pts, (long long)au.raw_pts, au.data[0], au.data[1],
                au.size > 2 ? au.data[2] : 0, au.size > 3 ? au.data[3] : 0);
    CHECK(au.size == 64 && au.has_pts && au.raw_pts == 85069, "ATRAC frame size and PTS come from the frame header");
    CHECK(au.data && au.data[0] == 0x0f && au.data[1] == 0xd0, "ATRAC frame keeps its frame header");
    sr_psmf_au_release(&au);
    CHECK(sr_psmf_producer_pop(p, SR_PSMF_AU_AUDIO, &au), "second ATRAC frame straddled a PES boundary");
    CHECK(au.size == 72 && !au.has_pts, "straddling frame is formed with no PTS on its starting packet");
    CHECK(au.data && au.data[8] == 0x5a, "straddling frame body survives the PES cut");
    sr_psmf_au_release(&au);
    CHECK(sr_psmf_producer_pop(p, SR_PSMF_AU_AUDIO, &au) == 0, "truncated trailing frame is not emitted");
    /* Picture 2's starting packet has no PTS, picture 3's delimiter begins in that same
     * packet, and the second ATRAC frame starts in the PTS-less audio packet. */
    CHECK(st.aus_without_pts == 3, "PTS-less access units are counted, not invented");
    CHECK(st.have_pts && st.first_pts == 0 && st.last_pts == 9009,
          "normalized PTS range is reported against the PSMF presentation base");
    sr_psmf_producer_close(p);
    free(bytes);
}

static void test_backpressure_keeps_order(void) {
    uint32_t size = 0; uint8_t *bytes = fixture(&size, 4);
    MemSource mem = {bytes, size, 0, 0};
    SrPsmfSource source = {mem_read, &mem, size};
    SrPsmfProducer *p = sr_psmf_producer_open(&source, 90000);
    CHECK(p != NULL, "backpressure fixture opens");
    if (p) {
        uint32_t sizes[64];
        int seen = 0;
        for (int i = 0; i < 400 && !sr_psmf_producer_eof(p); i++) {
            sr_psmf_producer_pump(p, 1);      /* small pump: queues fill and must throttle */
            pop_into(p, SR_PSMF_AU_VIDEO, sizes, &seen, 64);
            SrPsmfProducerStats st; sr_psmf_producer_stats(p, &st);
            CHECK(st.video_depth <= SR_PSMF_QUEUE_DEPTH, "producer queue never exceeds its depth");
        }
        pop_into(p, SR_PSMF_AU_VIDEO, sizes, &seen, 64);
        CHECK(seen == 12, "all 12 pictures survive backpressure");
        int ordered = seen == 12;
        for (int i = 0; ordered && i < seen; i++) if (sizes[i] != 7) ordered = 0;
        CHECK(ordered, "picture sizes stay stable across backpressure");
        sr_psmf_producer_close(p);
    }
    free(bytes);
}

static void test_no_aud_scan_work_bound(void) {
    uint32_t size = 0;
    uint8_t *bytes = fixture(&size, 5);
    CHECK(bytes != NULL, "many-small-PES fixture allocates");
    if (!bytes) return;
    MemSource mem = {bytes, size, 0, 0};
    SrPsmfSource source = {mem_read, &mem, size};
    SrPsmfProducer *p = sr_psmf_producer_open(&source, 0);
    CHECK(p != NULL, "many-small-PES fixture opens");
    if (p) {
        for (int i = 0; i < 128 && !sr_psmf_producer_eof(p); i++)
            sr_psmf_producer_pump(p, 32);
        SrPsmfProducerStats st;
        sr_psmf_producer_stats(p, &st);
        CHECK(st.eof && !st.failed && st.video_pes == 1024,
              "every no-AUD PES reaches EOF without parser failure");
        CHECK(st.video_aud_scan_candidates <= 2u * 1024u * 16u,
              "AUD candidate work stays linear in appended video bytes");
        SrPsmfAu au;
        memset(&au, 0, sizeof(au));
        int got = sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au);
        CHECK(got,
              "EOF drains the single no-AUD video access unit");
        CHECK(got && au.size == 1024u * 16u,
              "EOF preserves every no-AUD video byte");
        if (got) sr_psmf_au_release(&au);
        CHECK(!sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au),
              "no-AUD stream emits exactly once");
        sr_psmf_producer_close(p);
    }
    free(bytes);
}

static void test_aud_split_across_pes(void) {
    uint32_t size = 0;
    uint8_t *bytes = fixture(&size, 6);
    CHECK(bytes != NULL, "split-AUD fixture allocates");
    if (!bytes) return;
    MemSource mem = {bytes, size, 2, 0};
    SrPsmfSource source = {mem_read, &mem, size};
    SrPsmfProducer *p = sr_psmf_producer_open(&source, 90000);
    CHECK(p != NULL, "split-AUD fixture opens");
    if (p) {
        for (int i = 0; i < 32 && !sr_psmf_producer_eof(p); i++)
            sr_psmf_producer_pump(p, 1);
        SrPsmfProducerStats st;
        sr_psmf_producer_stats(p, &st);
        CHECK(st.eof && !st.failed && st.video_pes == 3 && st.video_aus == 2,
              "two split AUDs form exactly two video access units");
        SrPsmfAu au;
        memset(&au, 0, sizeof(au));
        int got = sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au);
        CHECK(got && au.size == 8u && au.has_pts && au.raw_pts == 90000,
              "first split-AUD unit preserves prefix and first PES time");
        if (got) sr_psmf_au_release(&au);
        got = sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au);
        CHECK(got && au.size == 5u && au.data &&
              memcmp(au.data, k_aud, sizeof(k_aud)) == 0 && !au.has_pts,
              "trailing split-AUD unit preserves its delimiter and missing PTS");
        if (got) sr_psmf_au_release(&au);
        CHECK(!sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au),
              "split-AUD stream emits no duplicate unit");
        sr_psmf_producer_close(p);
    }
    free(bytes);
}

static void test_aud_scan_survives_compaction(void) {
    uint32_t size = 0;
    uint8_t *bytes = fixture(&size, 7);
    CHECK(bytes != NULL, "compaction fixture allocates");
    if (!bytes) return;
    MemSource mem = {bytes, size, 0, 0};
    SrPsmfSource source = {mem_read, &mem, size};
    SrPsmfProducer *p = sr_psmf_producer_open(&source, 0);
    CHECK(p != NULL, "compaction fixture opens");
    if (p) {
        int seen = 0;
        for (int i = 0; i < 1024 && !sr_psmf_producer_eof(p); i++) {
            sr_psmf_producer_pump(p, 1);
            SrPsmfAu au;
            while (sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au)) {
                CHECK(au.size == 128u, "compacted scan preserves AU boundaries");
                seen++;
                sr_psmf_au_release(&au);
            }
        }
        SrPsmfProducerStats st;
        sr_psmf_producer_stats(p, &st);
        CHECK(st.eof && !st.failed && st.video_aus == 600 && seen == 600,
              "all pictures survive a live-buffer compaction");
        CHECK(st.video_aud_scan_candidates <= 2u * 600u * 128u,
              "compacted video scan remains linear");
        sr_psmf_producer_close(p);
    }
    free(bytes);
}

static void test_no_aud_at_buffer_limit(void) {
    uint8_t payload[65000];
    memset(payload, 0x55, sizeof(payload));
    for (uint32_t extra = 0; extra <= 1; extra++) {
        uint32_t payload_total = SR_PSMF_MAX_AU_BYTES + extra;
        Buf b;
        b.cap = payload_total + 2048u + 8192u;
        b.d = (uint8_t *)calloc(1, b.cap);
        CHECK(b.d != NULL, "near-limit no-AUD fixture allocates");
        if (!b.d) continue;
        b.at = 2048u;
        b.overflow = 0;
        b.d[0] = 'P'; b.d[1] = 'S'; b.d[2] = 'M'; b.d[3] = 'F';
        be32(b.d + 8, 2048u);
        uint32_t remaining = payload_total;
        while (remaining) {
            uint32_t n = remaining < sizeof(payload) ? remaining : sizeof(payload);
            add_pes(&b, 0xe0, 0, 0, payload, n);
            remaining -= n;
        }
        CHECK(!b.overflow, "near-limit no-AUD fixture fits its buffer");
        be32(b.d + 12, b.at - 2048u);
        MemSource mem = {b.d, b.at, 0, 0};
        SrPsmfSource source = {mem_read, &mem, b.at};
        SrPsmfProducer *p = sr_psmf_producer_open(&source, 0);
        CHECK(p != NULL, "near-limit no-AUD fixture opens");
        if (p) {
            for (int i = 0; i < 16; i++) {
                SrPsmfProducerStats st;
                sr_psmf_producer_stats(p, &st);
                if (st.eof || st.failed) break;
                sr_psmf_producer_pump(p, 16);
            }
            SrPsmfProducerStats st;
            sr_psmf_producer_stats(p, &st);
            CHECK(st.video_aud_scan_candidates <= 2u * payload_total,
                  "near-limit AUD scan work remains linear");
            SrPsmfAu au;
            memset(&au, 0, sizeof(au));
            int got = sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au);
            if (extra == 0) {
                CHECK(st.eof && !st.failed && got && au.size == payload_total,
                      "exact buffer limit emits one intact AU at EOF");
            } else {
                CHECK(st.failed && st.parser_failures && !got,
                      "one byte beyond the buffer limit fails closed");
            }
            if (got) sr_psmf_au_release(&au);
            sr_psmf_producer_close(p);
        }
        free(b.d);
    }
}

static void test_lifecycle_and_source_failure(void) {
    uint32_t size = 0; uint8_t *bytes = fixture(&size, 0);
    MemSource mem = {bytes, size, 0, 0};
    SrPsmfSource source = {mem_read, &mem, size};
    SrPsmfProducer *p = sr_psmf_producer_open(&source, 90000);
    CHECK(p != NULL, "lifecycle fixture opens");
    if (p) {
        sr_psmf_producer_pump(p, 6);
        SrPsmfAu au;
        if (sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au)) sr_psmf_au_release(&au);
        else CHECK(0, "pre-reset AU was produced");
        sr_psmf_producer_reset(p);
        SrPsmfProducerStats st; sr_psmf_producer_stats(p, &st);
        CHECK(st.pes_packets == 0 && st.video_depth == 0 && st.video_aus == 0,
              "reset clears queues, accumulators, and counters");
        memset(&au, 0, sizeof(au));
        CHECK(!sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au), "reset exposes no stale AU");
        CHECK(!sr_psmf_producer_pop(p, SR_PSMF_AU_AUDIO, &au), "reset exposes no stale audio AU");
        mem.fail = 1;
        sr_psmf_producer_pump(p, 20);
        sr_psmf_producer_stats(p, &st);
        CHECK(st.source_failures > 0 || st.parser_failures > 0, "source failure is observable");
        sr_psmf_producer_close(p);
    }
    free(bytes);
}

static void test_malformed(void) {
    struct { int variant; const char *text; } cases[] = {
        {1, "impossible declared PES length fails closed"},
        {2, "unsupported stream id fails closed"},
        {3, "audio payload shorter than the sub-header fails closed"},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint32_t size = 0;
        uint8_t *bytes = fixture(&size, cases[c].variant);
        MemSource mem = {bytes, size, 3, 0};
        SrPsmfSource source = {mem_read, &mem, size};
        SrPsmfProducer *p = sr_psmf_producer_open(&source, 90000);
        CHECK(p != NULL, "malformed fixture header still opens");
        if (p) {
            for (int i = 0; i < 200 && !sr_psmf_producer_eof(p); i++) sr_psmf_producer_pump(p, 4);
            SrPsmfProducerStats st; sr_psmf_producer_stats(p, &st);
            CHECK(st.failed && st.parser_failures > 0, cases[c].text);
            SrPsmfAu au;
            while (sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au)) sr_psmf_au_release(&au);
            while (sr_psmf_producer_pop(p, SR_PSMF_AU_AUDIO, &au)) sr_psmf_au_release(&au);
            sr_psmf_producer_close(p);
        }
        free(bytes);
    }
}

static void test_rejects_bad_containers(void) {
    uint8_t small[16];
    memset(small, 0, sizeof(small));
    MemSource mem = {small, sizeof(small), 0, 0};
    SrPsmfSource source = {mem_read, &mem, sizeof(small)};
    CHECK(sr_psmf_producer_open(&source, 90000) == NULL, "a header shorter than 2048 bytes is rejected");

    uint32_t size = 0;
    uint8_t *bytes = fixture(&size, 0);
    be32(bytes + 12, 0xFFFFFFFFu);            /* declared stream extent past the source */
    MemSource mem2 = {bytes, size, 0, 0};
    SrPsmfSource source2 = {mem_read, &mem2, size};
    CHECK(sr_psmf_producer_open(&source2, 90000) == NULL, "stream extent past EOF is rejected");
    bytes[0] = 'X';
    CHECK(sr_psmf_producer_open(&source2, 90000) == NULL, "bad magic is rejected");
    free(bytes);
}

/* One picture in one PES packet, with a caller-chosen timestamp shape. */
static uint8_t *ts_fixture(uint32_t *size_out, int pts_dts, uint32_t hdr_len,
                           int marker_valid, int64_t pts, int64_t dts) {
    Buf b;
    b.cap = 4096;
    b.d = (uint8_t *)calloc(1, b.cap);
    b.at = 2048;
    b.overflow = 0;
    if (!b.d) return NULL;
    b.d[0]='P'; b.d[1]='S'; b.d[2]='M'; b.d[3]='F';
    be32(b.d + 8, 2048);
    add_pack(&b);
    uint8_t es[4 + sizeof(k_picture1_body)];
    memcpy(es, k_aud, 4);
    memcpy(es + 4, k_picture1_body, sizeof(k_picture1_body));
    add_pes_ex(&b, 0xe0, pts_dts, hdr_len, pts, dts, marker_valid, es, sizeof(es));
    be32(b.d + 12, b.at - 2048);
    *size_out = b.at;
    CHECK(!b.overflow, "timestamp fixture fits its buffer");
    return b.d;
}

/* Byte 6 is the marker byte and byte 7 is PTS_DTS_flags.  Every combination the
 * format defines is exercised here, and the malformed ones fail closed.
 *
 * This is the coverage whose absence let a parser read the flags out of the marker
 * byte and still pass the rest of the suite: doing so reports a time on the 9,521
 * timestamp-less packets of the retail movie, cannot report the DTS of the 173 that
 * have one, and would read payload as a time on any packet whose flags declare none
 * while its optional header is non-empty. */
static void test_timestamp_flags(void) {
    struct TsCase {
        int pts_dts; uint32_t hdr_len; int marker_valid;
        int64_t pts, dts;
        int want_pts, want_dts, want_fail;
        const char *text;
    } cases[] = {
        { PES_TS_NONE, 0u, 1, 0, 0, 0, 0, 0,
          "PTS_DTS_flags 00: a packet with no timestamps reports none" },
        { PES_TS_PTS, 5u, 1, 90000, 0, 1, 0, 0,
          "PTS_DTS_flags 10: a PTS-only packet keeps its time" },
        { PES_TS_BOTH, 10u, 1, 99009, 96006, 1, 1, 0,
          "PTS_DTS_flags 11: a PTS+DTS packet keeps both times" },
        { PES_TS_PTS, 8u, 1, 93003, 0, 1, 0, 0,
          "a PTS followed by header filler keeps its time and its payload offset" },
        { PES_TS_BOTH, 13u, 1, 96006, 93003, 1, 1, 0,
          "a PTS+DTS pair followed by header filler keeps both times" },
        { PES_TS_RESERVED, 0u, 1, 0, 0, 0, 0, 1,
          "PTS_DTS_flags 01 is reserved and fails closed" },
        { PES_TS_PTS, 0u, 1, 90000, 0, 0, 0, 1,
          "a PTS claimed with no optional header fails closed" },
        { PES_TS_NONE, 0u, 0, 0, 0, 0, 0, 1,
          "a packet without the mandatory '10' marker fails closed" },
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint32_t size = 0;
        uint8_t *bytes = ts_fixture(&size, cases[c].pts_dts, cases[c].hdr_len,
                                    cases[c].marker_valid, cases[c].pts, cases[c].dts);
        if (!bytes) { CHECK(0, "timestamp fixture allocated"); continue; }
        MemSource mem = {bytes, size, 5, 0};       /* fragmented reads: nothing whole */
        SrPsmfSource source = {mem_read, &mem, size};
        SrPsmfProducer *p = sr_psmf_producer_open(&source, 90000);
        CHECK(p != NULL, "timestamp fixture opens");
        if (p) {
            for (int i = 0; i < 64 && !sr_psmf_producer_eof(p); i++) sr_psmf_producer_pump(p, 1);
            SrPsmfProducerStats st; sr_psmf_producer_stats(p, &st);
            SrPsmfAu au;
            int got = 0;
            memset(&au, 0, sizeof(au));
            if (cases[c].want_fail) {
                CHECK(st.failed && st.parser_failures > 0, cases[c].text);
                CHECK(!sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au),
                      "a rejected packet yields no access unit");
            } else {
                CHECK(!st.failed, cases[c].text);
                got = sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au);
                CHECK(got, cases[c].text);
                if (got) {
                    CHECK(au.has_pts == (uint8_t)cases[c].want_pts,
                          "PTS presence follows the flags byte, not the marker byte");
                    CHECK(au.has_dts == (uint8_t)cases[c].want_dts,
                          "DTS presence follows the flags byte, not the marker byte");
                    if (cases[c].want_pts)
                        CHECK(au.raw_pts == cases[c].pts, "the access unit carries the packet's PTS");
                    if (cases[c].want_dts)
                        CHECK(au.raw_dts == cases[c].dts, "the access unit carries the packet's DTS");
                    CHECK(st.pes_with_dts == (uint64_t)cases[c].want_dts,
                          "the decode-time counter reports what the flags declared, not what the marker byte implies");
                    CHECK(au.size == 4u + 3u && au.data && au.data[4] == 0x11,
                          "the picture payload begins after the declared optional header");
                }
            }
            if (got) sr_psmf_au_release(&au);
            sr_psmf_producer_close(p);
        }
        free(bytes);
    }
}

/* ISO/IEC 13818-1 offsets: 0-3 pack_start_code; 4-13 MPEG-2 pack_header; 14-22 PES_header; 23-29 payload. */
static const uint8_t corpus_pack_zero[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0A, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-13 MPEG-2 pack_header; 14-16 pack_stuffing; 17-25 PES_header; 26-32 payload. */
static const uint8_t corpus_pack_stuffed[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x03,
    0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0A, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-17 system_header start; 18-19 length; 20-33 system_header; 34-42 padding_stream; 43-51 PES_header; 52-58 payload. */
static const uint8_t corpus_system_padding[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xBB, 0x00, 0x0E,
    0x80, 0x04, 0x00, 0x21, 0xFF, 0xFF, 0xE0, 0x08,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0xBE, 0x00, 0x03, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0A, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-22 private_stream_1 PES_header; 23-27 PTS; 28-47 private_stream_1 payload. */
static const uint8_t corpus_private_audio[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xBD, 0x00, 0x1C, 0x80, 0x80, 0x05,
    0x21, 0x00, 0x05, 0xBF, 0x21,
    0x00, 0x00, 0x00, 0x00,
    0x0F, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8
};

/* ISO/IEC 13818-1 offsets: 0-3 PES_start_code_prefix; 4-5 PES_packet_length; 6 marker; 7 PTS_DTS_flags; 8 header_data_length; 9-13 PTS; 14-20 payload. */
static const uint8_t corpus_video_pts[] = {
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0F, 0x80, 0x80, 0x05,
    0x21, 0x00, 0x05, 0xBF, 0x21,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-3 PES_start_code_prefix; 4-5 PES_packet_length; 6 marker; 7 PTS_DTS_flags; 8 header_data_length; 9-13 PTS; 14-18 DTS; 19-25 payload. */
static const uint8_t corpus_video_pts_dts[] = {
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x14, 0x80, 0xC0, 0x0A,
    0x31, 0x00, 0x05, 0xBF, 0x21,
    0x11, 0x00, 0x05, 0xEE, 0x0D,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-3 PES_start_code_prefix; 4-5 PES_packet_length; 6 marker; 7 PTS_DTS_flags=00; 8 header_data_length; 9-11 optional bytes; 12-18 payload. */
static const uint8_t corpus_video_no_pts_optional[] = {
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0D, 0x80, 0x00, 0x03,
    0xDE, 0xAD, 0xBE,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-3 PES_start_code_prefix; 4-5 PES_packet_length; 6 marker; 7 no timestamps; 8 no optional header; 9-15 three-byte Annex-B start code and AUD. */
static const uint8_t corpus_annexb_three[] = {
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0A, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-22 PES_header with PTS_DTS_flags=01; 23-29 payload. */
static const uint8_t corpus_video_reserved[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0A, 0x80, 0x40, 0x00,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-22 PES_header; 23-27 PTS with its first marker bit clear; 28-34 payload. */
static const uint8_t corpus_pts_marker_first[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0F, 0x80, 0x80, 0x05,
    0x20, 0x00, 0x05, 0xBF, 0x21,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-22 PES_header; 23-27 PTS with its middle marker bit clear; 28-34 payload. */
static const uint8_t corpus_pts_marker_middle[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0F, 0x80, 0x80, 0x05,
    0x21, 0x00, 0x04, 0xBF, 0x21,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-22 PES_header; 23-27 PTS with its last marker bit clear; 28-34 payload. */
static const uint8_t corpus_pts_marker_last[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0F, 0x80, 0x80, 0x05,
    0x21, 0x00, 0x05, 0xBF, 0x20,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-22 PES_header with flags1 missing the mandatory '10'; 23-29 payload. */
static const uint8_t corpus_bad_pes_marker[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0A, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-22 PES_header; 23-29 payload; 30-33 program_end_code; 34-49 trailing PES packet. */
static const uint8_t corpus_program_end[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0A, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11,
    0x00, 0x00, 0x01, 0xB9,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0A, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0xEE, 0xEE, 0xEE
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-22 PES_header; PES_packet_length declares 32 body bytes but the table ends at offset 22. */
static const uint8_t corpus_truncated_pes[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x20, 0x80, 0x00, 0x00
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-22 PES_header; header_data_length=5 exceeds the three-byte PES body. */
static const uint8_t corpus_header_length_past_end[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x03, 0x80, 0x00, 0x05
};

/* ISO/IEC 13818-1 offsets: 0-13 pack_header; 14-17 bad start-code prefix; 18-29 deliberately present payload. */
static const uint8_t corpus_bad_start_prefix[] = {
    0x00, 0x00, 0x01, 0xBA,
    0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x02, 0xE0, 0x00, 0x0A, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0xEE, 0xEE, 0xEE
};

/* ISO/IEC 13818-1 offsets: 0-3 PES_start_code_prefix; 4-5 PES_packet_length; 6 marker; 7 no timestamps; 8 no optional header; 9-16 four-byte Annex-B start code and AUD. */
static const uint8_t corpus_annexb_four[] = {
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0B, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x09, 0x11, 0x11, 0x11
};

/* ISO/IEC 13818-1 offsets: 0-3 PES_start_code_prefix; 4-5 PES_packet_length; 6 marker; 7 no timestamps; 8 no optional header; 9-16 valid trailing packet. */
static const uint8_t corpus_follow_video[] = {
    0x00, 0x00, 0x01, 0xE0, 0x00, 0x0A, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0xEE, 0xEE, 0xEE
};

#define CORPUS_NO_POS 0xFFFFFFFFu

enum CorpusOutcome {
    CORPUS_ACCEPT,
    CORPUS_FAILURE,
    CORPUS_PROGRAM_END
};

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
    const uint8_t *tail;
    uint32_t tail_size;
    int tail_in_stream;
    enum CorpusOutcome outcome;
    SrPsmfAuKind kind;
    uint32_t packs, pes, video_pes, audio_pes;
    uint32_t pes_at;
    uint16_t pes_length;
    uint8_t pes_sid, pes_flags1, pes_flags2, pes_header_length;
    uint32_t pts_at, dts_at;
    uint8_t pts_prefix, dts_prefix, pts_markers, dts_markers;
    int64_t pts, dts;
    int has_pts, has_dts;
    uint32_t data_at, data_size;
    uint8_t stream_id;
    uint32_t fail_at, read_limit;
} CorpusCase;

static void check_literal_pes(const CorpusCase *c) {
    if (c->pes_at == CORPUS_NO_POS) return;
    uint32_t at = c->pes_at;
    CHECK(at + 9u <= c->size, "literal PES header is inside its byte table");
    if (at + 9u > c->size) return;
    const uint8_t *s = c->data + at;
    CHECK(s[0] == 0x00u && s[1] == 0x00u && s[2] == 0x01u,
          "literal PES start-code prefix is 00 00 01");
    CHECK(s[3] == c->pes_sid, "literal PES stream id is at byte 3");
    CHECK((uint16_t)(((uint16_t)s[4] << 8) | s[5]) == c->pes_length,
          "literal PES length is at bytes 4-5");
    CHECK(s[6] == c->pes_flags1, "literal mandatory marker byte is byte 6");
    CHECK(s[7] == c->pes_flags2, "literal PTS_DTS_flags byte is byte 7");
    CHECK(s[8] == c->pes_header_length, "literal header_data_length is byte 8");
    if (c->pts_at != CORPUS_NO_POS) {
        CHECK(c->pts_at + 5u <= c->size, "literal PTS is inside its byte table");
        if (c->pts_at + 5u <= c->size) {
            const uint8_t *p = c->data + c->pts_at;
            uint8_t markers = (uint8_t)((p[0] & 1u) |
                                        ((p[2] & 1u) << 1) |
                                        ((p[4] & 1u) << 2));
            CHECK((p[0] & 0xF0u) == c->pts_prefix,
                  "literal PTS prefix is checked independently");
            /* ISO/IEC 13818-1 2.4.3.7: PTS alone is prefixed '0010'; with a DTS it is '0011'. */
            if (c->outcome == CORPUS_ACCEPT)
                CHECK((p[0] & 0xF0u) == ((c->pes_flags2 >> 6) == 3u ? 0x30u : 0x20u),
                      "accepted literal PTS prefix matches its PTS_DTS_flags");
            CHECK(markers == c->pts_markers,
                  "literal PTS marker bits are pinned by the table");
            CHECK(read_pts(p) == c->pts, "literal PTS encodes its declared value");
        }
    }
    if (c->dts_at != CORPUS_NO_POS) {
        CHECK(c->dts_at + 5u <= c->size, "literal DTS is inside its byte table");
        if (c->dts_at + 5u <= c->size) {
            const uint8_t *p = c->data + c->dts_at;
            uint8_t markers = (uint8_t)((p[0] & 1u) |
                                        ((p[2] & 1u) << 1) |
                                        ((p[4] & 1u) << 2));
            CHECK((p[0] & 0xF0u) == c->dts_prefix,
                  "literal DTS prefix is checked independently");
            if (c->outcome == CORPUS_ACCEPT)
                CHECK((p[0] & 0xF0u) == 0x10u, "accepted literal DTS prefix is '0001'");
            CHECK(markers == c->dts_markers,
                  "literal DTS marker bits are pinned by the table");
            CHECK(read_pts(p) == c->dts, "literal DTS encodes its declared value");
        }
    }
}

static uint8_t *corpus_wrap(const CorpusCase *c, uint32_t *source_size, uint32_t *stream_size) {
    uint32_t in_stream_tail = c->tail_in_stream ? c->tail_size : 0u;
    *stream_size = c->size + in_stream_tail;
    *source_size = 2048u + c->size + c->tail_size;
    uint8_t *data = (uint8_t *)calloc(1, *source_size);
    if (!data) return NULL;
    memcpy(data + 2048u, c->data, c->size);
    if (c->tail_size) memcpy(data + 2048u + c->size, c->tail, c->tail_size);
    data[0] = 'P'; data[1] = 'S'; data[2] = 'M'; data[3] = 'F';
    be32(data + 8, 2048u);
    be32(data + 12, *stream_size);
    return data;
}

static void run_corpus_case(const CorpusCase *c) {
    check_literal_pes(c);
    uint32_t source_size = 0, stream_size = 0;
    uint8_t *data = corpus_wrap(c, &source_size, &stream_size);
    CHECK(data != NULL, "corpus source allocated");
    if (!data) return;
    MemSource mem = {data, source_size, 3u, 0};
    SrPsmfSource source = {mem_read, &mem, source_size};
    SrPsmfProducer *p = sr_psmf_producer_open(&source, 90000);
    CHECK(p != NULL, c->name);
    if (!p) { free(data); return; }
    for (int i = 0; i < 128 && !sr_psmf_producer_eof(p); i++)
        sr_psmf_producer_pump(p, 1);
    SrPsmfProducerStats st;
    sr_psmf_producer_stats(p, &st);
    CHECK(st.packs == c->packs, c->name);
    CHECK(st.pes_packets == c->pes, c->name);
    CHECK(st.video_pes == c->video_pes, c->name);
    CHECK(st.audio_pes == c->audio_pes, c->name);
    if (c->read_limit != CORPUS_NO_POS)
        CHECK(st.bytes_read <= 2048u + c->read_limit, c->name);

    SrPsmfAu au;
    memset(&au, 0, sizeof(au));
    if (c->outcome == CORPUS_FAILURE) {
        CHECK(st.failed && !st.eof, c->name);
        CHECK(st.parser_failures == 1 && st.source_failures == 0, c->name);
        CHECK(st.fail_offset == 2048u + c->fail_at, c->name);
        CHECK(st.video_aus == 0 && st.audio_aus == 0, c->name);
        int got_video = sr_psmf_producer_pop(p, SR_PSMF_AU_VIDEO, &au);
        CHECK(!got_video, "a failed packet yields no following video access unit");
        if (got_video) sr_psmf_au_release(&au);
        memset(&au, 0, sizeof(au));
        int got_audio = sr_psmf_producer_pop(p, SR_PSMF_AU_AUDIO, &au);
        CHECK(!got_audio, "a failed packet yields no following audio access unit");
        if (got_audio) sr_psmf_au_release(&au);
    } else {
        CHECK(!st.failed && st.eof, c->name);
        CHECK(st.parser_failures == 0 && st.source_failures == 0, c->name);
        int got = sr_psmf_producer_pop(p, c->kind, &au);
        CHECK(got, c->name);
        if (got) {
            CHECK(au.kind == c->kind, c->name);
            CHECK(au.stream_id == c->stream_id, c->name);
            CHECK(au.has_pts == (uint8_t)c->has_pts, c->name);
            CHECK(au.has_dts == (uint8_t)c->has_dts, c->name);
            CHECK(au.size == c->data_size, c->name);
            if (au.size == c->data_size && c->data_at != CORPUS_NO_POS)
                CHECK(memcmp(au.data, c->data + c->data_at, c->data_size) == 0,
                      "accepted payload begins at the literal table's declared offset");
            if (c->has_pts) {
                CHECK(au.raw_pts == c->pts, c->name);
                CHECK(au.pts == c->pts - 90000, c->name);
            }
            if (c->has_dts) {
                CHECK(au.raw_dts == c->dts, c->name);
                CHECK(au.dts == c->dts - 90000, c->name);
            }
            sr_psmf_au_release(&au);
        }
        CHECK(!sr_psmf_producer_pop(p, c->kind, &au),
              "an accepted corpus case yields exactly one access unit");
    }
    sr_psmf_producer_close(p);
    free(data);
}

static void test_conformance_corpus(void) {
    static const CorpusCase cases[] = {
        { .name = "pack stuffing length 0", .data = corpus_pack_zero, .size = sizeof(corpus_pack_zero),
          .outcome = CORPUS_ACCEPT, .kind = SR_PSMF_AU_VIDEO, .packs = 1, .pes = 1,
          .video_pes = 1, .pes_at = 14, .pes_length = 0x000A, .pes_sid = 0xE0,
          .pes_flags1 = 0x80, .pes_flags2 = 0x00, .pes_header_length = 0,
          .pts_at = CORPUS_NO_POS, .dts_at = CORPUS_NO_POS, .data_at = 23, .data_size = 7,
          .stream_id = 0xE0, .read_limit = CORPUS_NO_POS },
        { .name = "pack stuffing length 3", .data = corpus_pack_stuffed, .size = sizeof(corpus_pack_stuffed),
          .outcome = CORPUS_ACCEPT, .kind = SR_PSMF_AU_VIDEO, .packs = 1, .pes = 1,
          .video_pes = 1, .pes_at = 17, .pes_length = 0x000A, .pes_sid = 0xE0,
          .pes_flags1 = 0x80, .pes_flags2 = 0x00, .pes_header_length = 0,
          .pts_at = CORPUS_NO_POS, .dts_at = CORPUS_NO_POS, .data_at = 26, .data_size = 7,
          .stream_id = 0xE0, .read_limit = CORPUS_NO_POS },
        { .name = "system header and padding are skipped", .data = corpus_system_padding, .size = sizeof(corpus_system_padding),
          .outcome = CORPUS_ACCEPT, .kind = SR_PSMF_AU_VIDEO, .packs = 2, .pes = 1,
          .video_pes = 1, .pes_at = 43, .pes_length = 0x000A, .pes_sid = 0xE0,
          .pes_flags1 = 0x80, .pes_flags2 = 0x00, .pes_header_length = 0,
          .pts_at = CORPUS_NO_POS, .dts_at = CORPUS_NO_POS, .data_at = 52, .data_size = 7,
          .stream_id = 0xE0, .read_limit = CORPUS_NO_POS },
        { .name = "private stream 1 audio", .data = corpus_private_audio, .size = sizeof(corpus_private_audio),
          .outcome = CORPUS_ACCEPT, .kind = SR_PSMF_AU_AUDIO, .packs = 1, .pes = 1,
          .audio_pes = 1, .pes_at = 14, .pes_length = 0x001C, .pes_sid = 0xBD,
          .pes_flags1 = 0x80, .pes_flags2 = 0x80, .pes_header_length = 5,
          .pts_at = 23, .dts_at = CORPUS_NO_POS, .pts_prefix = 0x20, .pts_markers = 0x07,
          .pts = 90000, .has_pts = 1, .data_at = 32, .data_size = 16, .stream_id = 0x00,
          .read_limit = CORPUS_NO_POS },
        { .name = "PTS only", .data = corpus_video_pts, .size = sizeof(corpus_video_pts),
          .outcome = CORPUS_ACCEPT, .kind = SR_PSMF_AU_VIDEO, .pes = 1, .video_pes = 1,
          .pes_at = 0, .pes_length = 0x000F, .pes_sid = 0xE0, .pes_flags1 = 0x80,
          .pes_flags2 = 0x80, .pes_header_length = 5, .pts_at = 9, .dts_at = CORPUS_NO_POS,
          .pts_prefix = 0x20, .pts_markers = 0x07, .pts = 90000, .has_pts = 1,
          .data_at = 14, .data_size = 7, .stream_id = 0xE0, .read_limit = CORPUS_NO_POS },
        { .name = "PTS and DTS", .data = corpus_video_pts_dts, .size = sizeof(corpus_video_pts_dts),
          .outcome = CORPUS_ACCEPT, .kind = SR_PSMF_AU_VIDEO, .pes = 1, .video_pes = 1,
          .pes_at = 0, .pes_length = 0x0014, .pes_sid = 0xE0, .pes_flags1 = 0x80,
          .pes_flags2 = 0xC0, .pes_header_length = 10, .pts_at = 9, .dts_at = 14,
          .pts_prefix = 0x30, .dts_prefix = 0x10, .pts_markers = 0x07, .dts_markers = 0x07,
          .pts = 90000, .dts = 96006, .has_pts = 1, .has_dts = 1,
          .data_at = 19, .data_size = 7, .stream_id = 0xE0, .read_limit = CORPUS_NO_POS },
        { .name = "no PTS with optional header", .data = corpus_video_no_pts_optional, .size = sizeof(corpus_video_no_pts_optional),
          .outcome = CORPUS_ACCEPT, .kind = SR_PSMF_AU_VIDEO, .pes = 1, .video_pes = 1,
          .pes_at = 0, .pes_length = 0x000D, .pes_sid = 0xE0, .pes_flags1 = 0x80,
          .pes_flags2 = 0x00, .pes_header_length = 3, .pts_at = CORPUS_NO_POS,
          .dts_at = CORPUS_NO_POS, .data_at = 12, .data_size = 7, .stream_id = 0xE0,
          .read_limit = CORPUS_NO_POS },
        { .name = "three-byte Annex-B start code", .data = corpus_annexb_three, .size = sizeof(corpus_annexb_three),
          .outcome = CORPUS_ACCEPT, .kind = SR_PSMF_AU_VIDEO, .pes = 1, .video_pes = 1,
          .pes_at = 0, .pes_length = 0x000A, .pes_sid = 0xE0, .pes_flags1 = 0x80,
          .pes_flags2 = 0x00, .pes_header_length = 0, .pts_at = CORPUS_NO_POS,
          .dts_at = CORPUS_NO_POS, .data_at = 9, .data_size = 7, .stream_id = 0xE0,
          .read_limit = CORPUS_NO_POS },
        { .name = "program end stops before trailing packet", .data = corpus_program_end, .size = sizeof(corpus_program_end),
          .outcome = CORPUS_PROGRAM_END, .kind = SR_PSMF_AU_VIDEO, .packs = 1, .pes = 1,
          .video_pes = 1, .pes_at = 14, .pes_length = 0x000A, .pes_sid = 0xE0,
          .pes_flags1 = 0x80, .pes_flags2 = 0x00, .pes_header_length = 0,
          .pts_at = CORPUS_NO_POS, .dts_at = CORPUS_NO_POS, .data_at = 23, .data_size = 7,
          .stream_id = 0xE0, .read_limit = 34 },
        { .name = "four-byte Annex-B start code", .data = corpus_annexb_four, .size = sizeof(corpus_annexb_four),
          .outcome = CORPUS_ACCEPT, .kind = SR_PSMF_AU_VIDEO, .pes = 1, .video_pes = 1,
          .pes_at = 0, .pes_length = 0x000B, .pes_sid = 0xE0, .pes_flags1 = 0x80,
          .pes_flags2 = 0x00, .pes_header_length = 0, .pts_at = CORPUS_NO_POS,
          .dts_at = CORPUS_NO_POS, .data_at = 9, .data_size = 8, .stream_id = 0xE0,
          .read_limit = CORPUS_NO_POS },
        { .name = "reserved PTS_DTS_flags 01", .data = corpus_video_reserved, .size = sizeof(corpus_video_reserved),
          .tail = corpus_follow_video, .tail_size = sizeof(corpus_follow_video), .tail_in_stream = 1,
          .outcome = CORPUS_FAILURE, .packs = 1, .pes = 0, .pes_at = 14, .pes_length = 0x000A,
          .pes_sid = 0xE0, .pes_flags1 = 0x80, .pes_flags2 = 0x40, .pes_header_length = 0,
          .pts_at = CORPUS_NO_POS, .dts_at = CORPUS_NO_POS, .fail_at = 14,
          .read_limit = sizeof(corpus_video_reserved) },
        { .name = "PTS first marker bit", .data = corpus_pts_marker_first, .size = sizeof(corpus_pts_marker_first),
          .tail = corpus_follow_video, .tail_size = sizeof(corpus_follow_video), .tail_in_stream = 1,
          .outcome = CORPUS_FAILURE, .packs = 1, .pes = 0, .pes_at = 14, .pes_length = 0x000F,
          .pes_sid = 0xE0, .pes_flags1 = 0x80, .pes_flags2 = 0x80, .pes_header_length = 5,
          .pts_at = 23, .dts_at = CORPUS_NO_POS, .pts_prefix = 0x20, .pts_markers = 0x06, .pts = 90000,
          .fail_at = 14, .read_limit = sizeof(corpus_pts_marker_first) },
        { .name = "PTS middle marker bit", .data = corpus_pts_marker_middle, .size = sizeof(corpus_pts_marker_middle),
          .tail = corpus_follow_video, .tail_size = sizeof(corpus_follow_video), .tail_in_stream = 1,
          .outcome = CORPUS_FAILURE, .packs = 1, .pes = 0, .pes_at = 14, .pes_length = 0x000F,
          .pes_sid = 0xE0, .pes_flags1 = 0x80, .pes_flags2 = 0x80, .pes_header_length = 5,
          .pts_at = 23, .dts_at = CORPUS_NO_POS, .pts_prefix = 0x20, .pts_markers = 0x05, .pts = 90000,
          .fail_at = 14, .read_limit = sizeof(corpus_pts_marker_middle) },
        { .name = "PTS last marker bit", .data = corpus_pts_marker_last, .size = sizeof(corpus_pts_marker_last),
          .tail = corpus_follow_video, .tail_size = sizeof(corpus_follow_video), .tail_in_stream = 1,
          .outcome = CORPUS_FAILURE, .packs = 1, .pes = 0, .pes_at = 14, .pes_length = 0x000F,
          .pes_sid = 0xE0, .pes_flags1 = 0x80, .pes_flags2 = 0x80, .pes_header_length = 5,
          .pts_at = 23, .dts_at = CORPUS_NO_POS, .pts_prefix = 0x20, .pts_markers = 0x03, .pts = 90000,
          .fail_at = 14, .read_limit = sizeof(corpus_pts_marker_last) },
        { .name = "PES marker byte is not 10", .data = corpus_bad_pes_marker, .size = sizeof(corpus_bad_pes_marker),
          .tail = corpus_follow_video, .tail_size = sizeof(corpus_follow_video), .tail_in_stream = 1,
          .outcome = CORPUS_FAILURE, .packs = 1, .pes = 0, .pes_at = 14, .pes_length = 0x000A,
          .pes_sid = 0xE0, .pes_flags1 = 0x00, .pes_flags2 = 0x00, .pes_header_length = 0,
          .pts_at = CORPUS_NO_POS, .dts_at = CORPUS_NO_POS, .fail_at = 14,
          .read_limit = sizeof(corpus_bad_pes_marker) },
        { .name = "truncated PES packet length", .data = corpus_truncated_pes, .size = sizeof(corpus_truncated_pes),
          .tail = corpus_follow_video, .tail_size = sizeof(corpus_follow_video),
          .outcome = CORPUS_FAILURE, .packs = 1, .pes = 0, .pes_at = 14, .pes_length = 0x0020,
          .pes_sid = 0xE0, .pes_flags1 = 0x80, .pes_flags2 = 0x00, .pes_header_length = 0,
          .pts_at = CORPUS_NO_POS, .dts_at = CORPUS_NO_POS, .fail_at = 14,
          .read_limit = sizeof(corpus_truncated_pes) },
        { .name = "header_data_length past packet", .data = corpus_header_length_past_end, .size = sizeof(corpus_header_length_past_end),
          .tail = corpus_follow_video, .tail_size = sizeof(corpus_follow_video), .tail_in_stream = 1,
          .outcome = CORPUS_FAILURE, .packs = 1, .pes = 0, .pes_at = 14, .pes_length = 0x0003,
          .pes_sid = 0xE0, .pes_flags1 = 0x80, .pes_flags2 = 0x00, .pes_header_length = 5,
          .pts_at = CORPUS_NO_POS, .dts_at = CORPUS_NO_POS, .fail_at = 14,
          .read_limit = sizeof(corpus_header_length_past_end) },
        { .name = "bad start-code prefix", .data = corpus_bad_start_prefix, .size = sizeof(corpus_bad_start_prefix),
          .tail = corpus_follow_video, .tail_size = sizeof(corpus_follow_video), .tail_in_stream = 1,
          .outcome = CORPUS_FAILURE, .packs = 1, .pes = 0, .pes_at = CORPUS_NO_POS,
          .pts_at = CORPUS_NO_POS, .dts_at = CORPUS_NO_POS, .fail_at = 14,
          .read_limit = sizeof(corpus_bad_start_prefix) }
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_corpus_case(&cases[i]);
}

int main(void) {
    test_access_unit_formation();
    test_backpressure_keeps_order();
    test_no_aud_scan_work_bound();
    test_aud_split_across_pes();
    test_aud_scan_survives_compaction();
    test_no_aud_at_buffer_limit();
    test_lifecycle_and_source_failure();
    test_malformed();
    test_rejects_bad_containers();
    test_timestamp_flags();
    test_conformance_corpus();
    printf("psmf_producer_selftest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
