// SPDX-License-Identifier: GPL-2.0-or-later
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
/* One MPEG-PS PES packet; has_pts == 0 writes no optional header at all (the
 * form PSMF uses for continuation packets). */
static void add_pes(Buf *b, uint8_t sid, int has_pts, int64_t pts,
                    const uint8_t *payload, uint32_t payload_len) {
    uint32_t opt = has_pts ? 5u : 0u;
    uint32_t len = 3u + opt + payload_len;
    uint8_t hdr[9];
    hdr[0]=0; hdr[1]=0; hdr[2]=1; hdr[3]=sid;
    hdr[4]=(uint8_t)(len>>8); hdr[5]=(uint8_t)len;
    hdr[6]= has_pts ? 0x80 : 0x00;
    hdr[7]=0x00;
    hdr[8]=(uint8_t)opt;
    raw(b, hdr, 9);
    if (has_pts) { uint8_t p[5]; put_pts(p, pts, 0x21); raw(b, p, 5); }
    raw(b, payload, payload_len);
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
    b.cap = 65536;
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

int main(void) {
    test_access_unit_formation();
    test_backpressure_keeps_order();
    test_lifecycle_and_source_failure();
    test_malformed();
    test_rejects_bad_containers();
    printf("psmf_producer_selftest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
