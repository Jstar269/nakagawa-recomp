// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * atrac3p_title_accept.c — PRIVATE title acceptance for the ATRAC3+ lane
 * (PR-C, issues #286/#32).
 *
 * Bounded private-route acceptance: reads the lawful private title stream
 * (a RIFF/WAVE ATRAC3+ file, canonically
 * place_game_here/EXTRACTED/PSP_GAME/USRDIR/data/sound/bgm/bgm_title.sgb),
 * parses the container with the same field offsets and bounds discipline as
 * the HLE ring parser (src/rt/hle.c atrac_parse_track), and decodes EVERY
 * frame through the production PR-A decoder via the PR-B bridge
 * (atrac3p_bridge_decode). It asserts:
 *
 *   - every frame decodes (ret == 0 and samples == ATRAC3P_FRAME_SAMPLES);
 *   - the PCM is valid and nonzero (the historical silence-only decoder
 *     blocker is gone: a stream that decodes to all-zero PCM is a FAIL);
 *   - determinism: a fresh decoder instance produces byte-identical output
 *     for the same frames;
 *   - reset lifecycle: atrac3p_bridge_reset returns the decoder to its
 *     post-create state (same output for the same input after reset).
 *
 * Output is aggregate statistics ONLY: codec tag, channels, sample rate,
 * blockAlign, frame count, duration, max |sample|, nonzero-frame ratio,
 * determinism. It never writes PCM, never prints stream bytes, and never
 * computes/stores hashes of the retail content.
 *
 * Exit codes: 0 = PASS (valid, nonzero, deterministic PCM); 77 = SKIP
 * (private input absent — never a pass); 1 = FAIL (silence, undecodable
 * frame, or inconsistent container). This tool is NOT part of CI and its
 * evidence is the user's lawful private title route only.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atrac3p_bridge.h"

/* ---- bounded host-side container parse (same fields as hle.c) ---- */

typedef struct {
    uint32_t riff_end;       /* total file size per the RIFF header */
    uint32_t data_off;       /* byte offset of the 'data' chunk payload */
    uint32_t data_size;      /* 'data' chunk payload size */
    uint32_t block_align;    /* fmt blockAlign = bytes per frame */
    uint32_t channels;       /* fmt channels */
    uint32_t sample_rate;    /* fmt sampleRate (reported only) */
    uint16_t codec_tag;      /* fmt format tag */
    uint32_t fact_samples;   /* 'fact' chunk sample count, or 0 */
    int have_fmt;
    int have_data;
} TitleStream;

static int accept_parse(const uint8_t *buf, size_t size, TitleStream *ts) {
    memset(ts, 0, sizeof(*ts));
    /* RIFF header: "RIFF"<size>"WAVE" */
    if (size < 12u || memcmp(buf, "RIFF", 4u) != 0 || memcmp(buf + 8u, "WAVE", 4u) != 0)
        return 0;
    uint32_t riff_end = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                        ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    if (riff_end < 12u) return 0;
    uint64_t file_end = 8ull + (uint64_t)riff_end;      /* checked arithmetic */
    if (file_end > size) file_end = size;               /* tolerate trailing bytes */

    uint32_t off = 12u;
    while (off <= file_end - 8u) {
        uint32_t id = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
                      ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
        uint32_t sz = (uint32_t)buf[off + 4] | ((uint32_t)buf[off + 5] << 8) |
                      ((uint32_t)buf[off + 6] << 16) | ((uint32_t)buf[off + 7] << 24);
        uint64_t next = (uint64_t)off + 8ull + (uint64_t)sz + (uint64_t)(sz & 1u);
        if (next > file_end) break;                     /* chunk runs past fed bytes */
        if (id == 0x20746d66u /* 'fmt ' */) {
            if (sz < 16u) return 0;
            ts->codec_tag = (uint16_t)(buf[off + 8u] | ((uint32_t)buf[off + 9u] << 8));
            ts->channels = (uint32_t)(buf[off + 10u] | ((uint32_t)buf[off + 11u] << 8));
            ts->sample_rate = (uint32_t)buf[off + 12u] | ((uint32_t)buf[off + 13u] << 8) |
                              ((uint32_t)buf[off + 14u] << 16) | ((uint32_t)buf[off + 15u] << 24);
            ts->block_align = (uint32_t)(buf[off + 20u] | ((uint32_t)buf[off + 21u] << 8));
            if (ts->channels < 1u || ts->channels > 8u) return 0;
            if (ts->block_align < 1u || ts->block_align > 0x10000u) return 0;
            ts->have_fmt = 1;
        } else if (id == 0x74636166u /* 'fact' */) {
            if (sz < 4u) return 0;
            ts->fact_samples = (uint32_t)buf[off + 8u] | ((uint32_t)buf[off + 9u] << 8) |
                               ((uint32_t)buf[off + 10u] << 16) | ((uint32_t)buf[off + 11u] << 24);
        } else if (id == 0x61746164u /* 'data' */) {
            ts->data_off = off + 8u;
            ts->data_size = sz;
            ts->have_data = 1;
        }
        off = (uint32_t)next;
    }
    if (!ts->have_fmt || !ts->have_data) return 0;
    if (ts->fact_samples == 0u) return 0;               /* PSP ATRAC tracks carry a fact chunk */
    return 1;
}

static int is_atrac3plus(uint16_t tag) {
    /* Raw ATRAC3+ tags and WAVE_FORMAT_EXTENSIBLE (the title stream uses
     * 0xFFFE; the real codec is proven by the decode itself, which fails
     * honestly for anything that is not ATRAC3+). */
    return tag == 0x27ffu || tag == 0x2700u || tag == 0x2701u || tag == 0xfffeu;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: atrac3p_title_accept.exe <title-atrac-file>\n");
        return 1;
    }
    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Private input absent: SKIP, never a pass. Exit 77 is the
         * repository's established "unavailable" SKIP code. */
        fprintf(stderr, "ATRAC3P-TITLE: SKIP (private title input not present: %s)\n", path);
        return 77;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (unsigned long)sz > 512u * 1024u * 1024u) {
        fclose(f);
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (file size out of range)\n");
        return 1;
    }
    size_t fsize = (size_t)sz;
    /* Zeroed padding after the last byte for the bitreader over-read. */
    uint8_t *file = (uint8_t *)calloc(1, fsize + ATRAC3P_PADDING_SIZE);
    if (!file) { fclose(f); fprintf(stderr, "ATRAC3P-TITLE: FAIL (oom)\n"); return 1; }
    if (fread(file, 1, fsize, f) != fsize) {
        fclose(f);
        free(file);
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (short read of %s)\n", path);
        return 1;
    }
    fclose(f);

    TitleStream ts;
    if (!accept_parse(file, fsize, &ts)) {
        free(file);
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (not a parseable RIFF/WAVE ATRAC track)\n");
        return 1;
    }
    if (!is_atrac3plus(ts.codec_tag)) {
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (unexpected codec tag 0x%04x)\n", ts.codec_tag);
        free(file);
        return 1;
    }
    if (ts.data_size < ts.block_align) {
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (data chunk smaller than one frame)\n");
        free(file);
        return 1;
    }
    uint32_t frames = ts.data_size / ts.block_align;
    uint32_t fact_frames = (ts.fact_samples + ATRAC3P_FRAME_SAMPLES - 1u) / ATRAC3P_FRAME_SAMPLES;
    if (frames < fact_frames) {
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (truncated: %u frames present, %u declared)\n",
                frames, fact_frames);
        free(file);
        return 1;
    }

    int rc = 1;   /* FAIL until proven */
    Atrac3pBridge *dec = NULL;
    int16_t *pcm = (int16_t *)malloc((size_t)ts.channels * ATRAC3P_FRAME_SAMPLES * sizeof(int16_t));
    if (!pcm) { free(file); return 1; }
    if (atrac3p_bridge_create((int)ts.channels, (int)ts.block_align, &dec) < 0) {
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (decoder create)\n");
        goto out;
    }

    /* Pass 1: decode every frame; verify nonzero PCM and aggregate stats. */
    uint64_t nonzero_frames = 0;
    int64_t max_abs = 0;
    int bad_frame = -1;
    for (uint32_t i = 0u; i < frames; i++) {
        const uint8_t *frame = file + ts.data_off + (size_t)i * ts.block_align;
        int n = 0;
        if (atrac3p_bridge_decode(dec, frame, (int)ts.block_align, pcm, &n) < 0 || n != ATRAC3P_FRAME_SAMPLES) {
            bad_frame = (int)i;
            break;
        }
        int64_t m = 0;
        for (int k = 0; k < n * (int)ts.channels; k++) {
            int64_t a = pcm[k] < 0 ? -(int64_t)pcm[k] : (int64_t)pcm[k];
            if (a > m) m = a;
        }
        if (m > max_abs) max_abs = m;
        if (m > 0) nonzero_frames++;
    }
    if (bad_frame >= 0) {
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (frame %d did not decode)\n", bad_frame);
        goto out;
    }
    if (nonzero_frames == 0u) {
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (all-zero PCM — silence-only decoder regression)\n");
        goto out;
    }

    /* Pass 2: determinism + reset lifecycle on the leading probe frames.
     * ATRAC3+ is a transform codec with cross-frame IMDCT overlap state, so
     * only same-sequence comparisons are meaningful: two fresh instances
     * decoding the identical leading sequence must produce byte-identical
     * output at every step, and a reset must restore the post-create state
     * (the same sequence again decodes identically). */
    uint32_t probe = frames < 32u ? frames : 32u;
    int deterministic = 0, reset_ok = 0;
    /* ref_first[i] stores the reference decode of the i-th leading frame so
     * the post-reset comparison never re-decodes on a mid-sequence context. */
    size_t frame_bytes = (size_t)ts.channels * ATRAC3P_FRAME_SAMPLES * sizeof(int16_t);
    int16_t **ref_first = (int16_t **)calloc(probe ? probe : 1u, sizeof(*ref_first));
    if (ref_first) {
        Atrac3pBridge *ref = NULL, *tst = NULL;
        int16_t *pcm_ref = (int16_t *)malloc(frame_bytes);
        int16_t *pcm_tst = (int16_t *)malloc(frame_bytes);
        if (pcm_ref && pcm_tst &&
            atrac3p_bridge_create((int)ts.channels, (int)ts.block_align, &ref) == 0 &&
            atrac3p_bridge_create((int)ts.channels, (int)ts.block_align, &tst) == 0) {
            int same = 1, ok = 1;
            for (uint32_t i = 0u; i < probe; i++) {
                const uint8_t *frame = file + ts.data_off + (size_t)i * ts.block_align;
                int nr = 0, nt = 0;
                if (atrac3p_bridge_decode(ref, frame, (int)ts.block_align, pcm_ref, &nr) < 0 ||
                    atrac3p_bridge_decode(tst, frame, (int)ts.block_align, pcm_tst, &nt) < 0) {
                    ok = 0;
                    break;
                }
                if (nr != ATRAC3P_FRAME_SAMPLES || nt != nr ||
                    memcmp(pcm_ref, pcm_tst, frame_bytes) != 0) {
                    same = 0;
                }
                ref_first[i] = (int16_t *)malloc(frame_bytes);
                if (!ref_first[i]) { ok = 0; break; }
                memcpy(ref_first[i], pcm_ref, frame_bytes);
            }
            deterministic = same && ok;
            /* Reset: tst returns to post-create state, so decoding the same
             * leading sequence again must reproduce the stored reference.
             * Only run this when pass 1 completed: a short pass leaves the
             * tail of ref_first[] NULL, and the decode below would succeed
             * for those frames, so the memcmp -- not the short-circuit --
             * would be reached with a NULL reference. reset_ok stays 0, which
             * fails the run just below, as an incomplete pass 1 should. */
            int reset_same = ok;
            atrac3p_bridge_reset(tst);
            for (uint32_t i = 0u; ok && i < probe; i++) {
                const uint8_t *frame = file + ts.data_off + (size_t)i * ts.block_align;
                int nt = 0;
                if (atrac3p_bridge_decode(tst, frame, (int)ts.block_align, pcm_tst, &nt) < 0 ||
                    nt != ATRAC3P_FRAME_SAMPLES ||
                    memcmp(pcm_tst, ref_first[i], frame_bytes) != 0) {
                    reset_same = 0;
                    break;
                }
            }
            reset_ok = reset_same;
            atrac3p_bridge_destroy(tst);
            atrac3p_bridge_destroy(ref);
        }
        free(pcm_ref);
        free(pcm_tst);
        for (uint32_t i = 0u; i < probe; i++) free(ref_first[i]);
        free(ref_first);
    }

    if (!deterministic || !reset_ok) {
        fprintf(stderr, "ATRAC3P-TITLE: FAIL (nondeterministic decode or broken reset lifecycle)\n");
        goto out;
    }

    /* Aggregate stats only — no stream bytes, no hashes. */
    printf("ATRAC3P-TITLE: PASS codec=0x%04x ch=%u rate=%u align=%u frames=%u "
           "factFrames=%u duration=%.3fs maxAbs=%lld nonzeroFrames=%llu/%u "
           "deterministic=1 resetLifecycle=1\n",
           ts.codec_tag, ts.channels, ts.sample_rate, ts.block_align, frames,
           fact_frames, (double)frames * ATRAC3P_FRAME_SAMPLES / (double)ts.sample_rate,
           (long long)max_abs, (unsigned long long)nonzero_frames, frames);
    rc = 0;

out:
    atrac3p_bridge_destroy(dec);
    free(pcm);
    free(file);
    return rc;
}
