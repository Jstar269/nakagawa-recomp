// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// atrac3p_selftest.c — regression suite for the standalone ATRAC3+ decoder
// (src/rt/atrac3p/, PR-A).
//
// Public, source-owned tests (no game inputs, no copyrighted fixtures):
//   - create() validation (channel set {1,2,3,4,6,7,8}, block_align > 0,
//     NULL out),
//   - decode() NULL-argument and oversized-frame rejection,
//   - garbage-frame rejection with the *samples_out = 0 contract,
//   - a deterministic positive decode through the PRODUCTION entry point:
//     a frame whose first byte is 0x60 (start bit 0, then the
//     CH_UNIT_TERMINATOR channel-unit id) exits the unit loop immediately,
//     which drives init_get_bits8 + start-bit check + terminator detection +
//     the nb_samples contract + FFMIN(block_align, buf_size) return and
//     yields exactly ATRAC3P_FRAME_SAMPLES all-zero samples per channel
//     (the zeroed output buffer is never written when no unit is decoded),
//   - determinism: the same frame decodes to byte-identical PCM across
//     instances, after reset, and after flush,
//   - destroy(NULL) no-op,
//   - transform-path canary: a deterministic MONO channel-unit frame drives a
//     real unit through decode_residual_spectrum() + reconstruct_frame()
//     (IMDCT/IPQF) and must decode with the nb_samples=2048 contract and
//     byte-identical PCM across instances/reset/flush (fails-before/after the
//     CONFIG_MDCT configuration fix; see PROVENANCE.md).
//
// Private fixture hook (optional, never required for the public suite):
// if the environment variable ATRAC3P_FIXTURE points to a directory
// containing stream.bin (concatenated ATRAC3+ frames, each exactly
// block_align bytes) and meta.txt (lines: channels=N, block_align=N,
// frames=N, pcm_sha256=<64 hex> of the full interleaved s16 PCM produced by
// atrac3p_decode()), the whole stream is decoded and hashed with the same
// SHA-256 used by the VFPU table loader (sr_vfpu_sha256). Mismatch or a
// malformed fixture is a hard failure; when the variable is unset the
// fixture portion prints a SKIP line and is not counted. The fixture must
// be sourced from license-compatible material (e.g. samples.ffmpeg.org
// ATRAC3+ files or PSP rips) and must never be committed to this
// repository.
//
// Exit code 0 = all public checks passed (fixture portion SKIP or passed).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atrac3p/atrac3p_api.h"
#include "vfpu_tables.h"
#include "strbuf.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, name)                                                      \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);           \
        }                                                                      \
    } while (0)

/* Terminator-only frame: start bit 0, then channel-unit id 3
 * (CH_UNIT_TERMINATOR) in MSB-first order -> 0b01100000 = 0x60. */
static const uint8_t TERMINATOR_FRAME[] = { 0x60 };

/* Deterministic MONO channel-unit frame (start bit 0, unit id 0). This exact
 * byte pattern passes the decoder's structural checks for 1 channel and
 * drives a real unit through decode_residual_spectrum() + reconstruct_frame()
 * (i.e. the IMDCT/IPQF transform pipeline). It decodes to deterministic
 * all-zero PCM (no coded spectral data in the garbage). It is the canary for
 * the transform path: without CONFIG_MDCT the imdct_calc/imdct_half function
 * pointers are never assigned (fft_template.c assigns them under
 * #if CONFIG_MDCT) and this decode crashes with a NULL indirect call, so this
 * test fails-before/passes-after that configuration fix. */
static const uint8_t MONO_UNIT_FRAME[] = { 0x16, 0x0c, 0xf6, 0x86, 0x87, 0x35, 0xd3, 0x1c, 0xb8 };

static int pcm_sha256_hex(const int16_t *pcm, size_t samples, char out[65])
{
    uint8_t digest[32];
    size_t n = 0;
    size_t i;

    sr_vfpu_sha256((const uint8_t *)pcm, samples * sizeof(int16_t), digest);
    /* Checked append: 32 x 2 hex digits exactly fill out[65] (64 + NUL).
     * sr_buf_append clamps the cursor to the NUL slot if that ever changed,
     * instead of advancing past the buffer like sprintf accumulation would. */
    for (i = 0; i < 32; i++)
        n = sr_buf_append(out, 65, n, "%02x", digest[i]);
    out[n] = '\0';
    return 0;
}

/* Positive-path check: the terminator-only frame must decode successfully
 * through the production entry with nb_samples = ATRAC3P_FRAME_SAMPLES and
 * byte-identical all-zero PCM. Runs twice plus after reset and flush to
 * prove determinism and state restoration. */
static void test_terminator_decode(void)
{
    Atrac3pHandle *h1 = NULL, *h2 = NULL;
    int16_t pcm1[2 * ATRAC3P_FRAME_SAMPLES];
    int16_t pcm2[2 * ATRAC3P_FRAME_SAMPLES];
    char sha1[65], sha2[65];
    int ret, samples = -1, i;
    const size_t n = 2 * (size_t)ATRAC3P_FRAME_SAMPLES;

    CHECK(atrac3p_create(2, 1536, &h1) == 0, "create h1");
    CHECK(atrac3p_create(2, 1536, &h2) == 0, "create h2");
    if (!h1 || !h2)
        return;

    ret = atrac3p_decode(h1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "terminator frame consumed");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "nb_samples = 2048 on success");
    for (i = 0; i < (int)n; i++)
        CHECK(pcm1[i] == 0, "terminator frame PCM is all-zero");

    /* determinism: a second fresh instance produces identical PCM */
    ret = atrac3p_decode(h2, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm2, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "h2 terminator consumed");
    pcm_sha256_hex(pcm1, n, sha1);
    pcm_sha256_hex(pcm2, n, sha2);
    CHECK(strcmp(sha1, sha2) == 0, "deterministic PCM across instances");

    /* reset restores post-create state */
    CHECK(atrac3p_reset(h1) == 0, "reset h1");
    ret = atrac3p_decode(h1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "decode after reset");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "nb_samples after reset");
    pcm_sha256_hex(pcm1, n, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "PCM after reset identical");

    /* flush is the same primitive */
    CHECK(atrac3p_flush(h1) == 0, "flush h1");
    ret = atrac3p_decode(h1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "decode after flush");
    pcm_sha256_hex(pcm1, n, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "PCM after flush identical");

    atrac3p_destroy(h1);
    atrac3p_destroy(h2);
}

/* Transform-path canary: the MONO unit frame must decode through the
 * production entry point (ret = frame length, samples = ATRAC3P_FRAME_SAMPLES)
 * and produce byte-identical PCM across instances, reset and flush. The
 * decode reaches reconstruct_frame(), so a broken transform configuration
 * (missing CONFIG_MDCT) crashes here instead of passing silently. */
static void test_imdct_path_decode(void)
{
    Atrac3pHandle *h1 = NULL, *h2 = NULL;
    int16_t pcm1[ATRAC3P_FRAME_SAMPLES];
    int16_t pcm2[ATRAC3P_FRAME_SAMPLES];
    char sha1[65], sha2[65];
    int ret, samples = -1;

    CHECK(atrac3p_create(1, 3071, &h1) == 0, "create h1 (mono unit)");
    CHECK(atrac3p_create(1, 3071, &h2) == 0, "create h2 (mono unit)");
    if (!h1 || !h2)
        return;

    ret = atrac3p_decode(h1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(MONO_UNIT_FRAME), "mono unit frame consumed");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "mono unit nb_samples");

    ret = atrac3p_decode(h2, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                         pcm2, &samples);
    CHECK(ret == (int)sizeof(MONO_UNIT_FRAME), "h2 mono unit consumed");
    pcm_sha256_hex(pcm1, ATRAC3P_FRAME_SAMPLES, sha1);
    pcm_sha256_hex(pcm2, ATRAC3P_FRAME_SAMPLES, sha2);
    CHECK(strcmp(sha1, sha2) == 0, "mono unit PCM deterministic across instances");

    CHECK(atrac3p_reset(h1) == 0, "reset h1 (mono unit)");
    ret = atrac3p_decode(h1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(MONO_UNIT_FRAME), "mono unit decode after reset");
    pcm_sha256_hex(pcm1, ATRAC3P_FRAME_SAMPLES, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "mono unit PCM deterministic after reset");

    CHECK(atrac3p_flush(h1) == 0, "flush h1 (mono unit)");
    ret = atrac3p_decode(h1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(MONO_UNIT_FRAME), "mono unit decode after flush");
    pcm_sha256_hex(pcm1, ATRAC3P_FRAME_SAMPLES, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "mono unit PCM deterministic after flush");

    atrac3p_destroy(h1);
    atrac3p_destroy(h2);
}

static void test_create_validation(void)
{
    Atrac3pHandle *h = NULL;

    CHECK(atrac3p_create(0, 1536, &h) < 0, "reject 0 channels");
    CHECK(atrac3p_create(5, 1536, &h) < 0, "reject 5 channels");
    CHECK(atrac3p_create(9, 1536, &h) < 0, "reject 9 channels");
    CHECK(atrac3p_create(2, 0, &h) < 0, "reject block_align 0");
    CHECK(atrac3p_create(2, -1, &h) < 0, "reject negative block_align");
    CHECK(atrac3p_create(2, 1536, NULL) < 0, "reject NULL out");
    CHECK(atrac3p_create(1, 1536, &h) == 0 && h != NULL, "accept 1 channel");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(3, 1536, &h) == 0 && h != NULL, "accept 3 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(4, 1536, &h) == 0 && h != NULL, "accept 4 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(6, 1536, &h) == 0 && h != NULL, "accept 6 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(7, 1536, &h) == 0 && h != NULL, "accept 7 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(8, 1536, &h) == 0 && h != NULL, "accept 8 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
}

static void test_decode_rejection(void)
{
    Atrac3pHandle *h = NULL;
    int16_t pcm[2 * ATRAC3P_FRAME_SAMPLES];
    uint8_t garbage[1536];
    int ret, samples = -1, i;

    CHECK(atrac3p_create(2, 1536, &h) == 0 && h != NULL, "create for rejection");
    if (!h)
        return;

    for (i = 0; i < (int)sizeof(garbage); i++)
        garbage[i] = (uint8_t)(i * 7 + 13);

    ret = atrac3p_decode(h, garbage, sizeof(garbage), pcm, &samples);
    CHECK(ret < 0, "garbage frame rejected");
    CHECK(samples == 0, "nb_samples stays 0 on failure");

    CHECK(atrac3p_decode(h, garbage, (int)sizeof(garbage) + 1, pcm, &samples) < 0,
          "oversized frame rejected");
    CHECK(atrac3p_decode(NULL, garbage, (int)sizeof(garbage), pcm, &samples) < 0,
          "NULL handle rejected");
    CHECK(atrac3p_decode(h, NULL, (int)sizeof(garbage), pcm, &samples) < 0,
          "NULL frame rejected");
    CHECK(atrac3p_decode(h, garbage, (int)sizeof(garbage), NULL, &samples) < 0,
          "NULL pcm rejected");
    CHECK(atrac3p_decode(h, garbage, (int)sizeof(garbage), pcm, NULL) < 0,
          "NULL samples rejected");
    CHECK(atrac3p_reset(NULL) < 0, "NULL reset rejected");
    CHECK(atrac3p_flush(NULL) < 0, "NULL flush rejected");

    /* controlled recovery after failures */
    ret = atrac3p_decode(h, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "decode succeeds after rejections");

    atrac3p_destroy(h);
    atrac3p_destroy(NULL); /* must be a no-op */
}

/* ------------------------------------------------------------------ */
/* Optional private fixture hook                                       */
/* ------------------------------------------------------------------ */

static int run_fixture(const char *dir)
{
    char path[1024];
    char meta[1024];
    char want[65] = { 0 };
    char got[65];
    FILE *f;
    int channels = 0, block_align = 0, frames = 0;
    Atrac3pHandle *h = NULL;
    int16_t *stream = NULL;
    uint8_t *buf = NULL;
    size_t frame_pcm, stream_bytes;
    int frame, ret, samples = 0;
    int rc = 1;

    snprintf(path, sizeof(path), "%s/meta.txt", dir);
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "FAIL: fixture meta.txt not readable: %s\n", path);
        goto out;
    }
    while (fgets(meta, sizeof(meta), f)) {
        if (sscanf(meta, "channels=%d", &channels) == 1) continue;
        if (sscanf(meta, "block_align=%d", &block_align) == 1) continue;
        if (sscanf(meta, "frames=%d", &frames) == 1) continue;
        if (sscanf(meta, "pcm_sha256=%63s", want) == 1) continue;
    }
    fclose(f);

    /* Bound the fixture parameters: the decoder supports at most 8 channels
     * and the stream hash must stay below the 1 Mi frame sanity cap. */
    if (channels < 1 || channels > ATRAC3P_MAX_CHANNELS || block_align < 1 ||
        frames < 1 || frames > (1 << 20) || strlen(want) != 64) {
        fprintf(stderr, "FAIL: fixture meta.txt malformed (channels=%d block_align=%d frames=%d want='%s')\n",
                channels, block_align, frames, want);
        goto out;
    }

    if (atrac3p_create(channels, block_align, &h) != 0 || !h) {
        fprintf(stderr, "FAIL: fixture create\n");
        goto out;
    }

    frame_pcm = (size_t)channels * ATRAC3P_FRAME_SAMPLES;
    stream_bytes = frame_pcm * (size_t)frames * sizeof(int16_t);
    stream = (int16_t *)malloc(stream_bytes);
    buf = (uint8_t *)malloc((size_t)block_align);
    if (!stream || !buf) {
        fprintf(stderr, "FAIL: fixture allocation\n");
        goto out;
    }

    snprintf(path, sizeof(path), "%s/stream.bin", dir);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: fixture stream.bin not readable: %s\n", path);
        goto out;
    }

    for (frame = 0; frame < frames; frame++) {
        if (fread(buf, 1, (size_t)block_align, f) != (size_t)block_align) {
            fprintf(stderr, "FAIL: fixture stream truncated at frame %d\n", frame);
            fclose(f);
            goto out;
        }
        ret = atrac3p_decode(h, buf, block_align, stream + frame * frame_pcm, &samples);
        if (ret != block_align || samples != ATRAC3P_FRAME_SAMPLES) {
            fprintf(stderr, "FAIL: fixture frame %d decode ret=%d samples=%d\n",
                    frame, ret, samples);
            fclose(f);
            goto out;
        }
    }
    fclose(f);

    pcm_sha256_hex(stream, stream_bytes / sizeof(int16_t), got);
    rc = 0;
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: fixture PCM sha256 mismatch\n  want: %s\n  got:  %s\n",
                want, got);
        rc = 1;
    }

out:
    if (h) atrac3p_destroy(h);
    free(stream);
    free(buf);
    return rc;
}

int main(int argc, char **argv)
{
    const char *fixture = getenv("ATRAC3P_FIXTURE");
    (void)argc;
    (void)argv;

    test_create_validation();
    test_decode_rejection();
    test_terminator_decode();
    test_imdct_path_decode();

    if (fixture && fixture[0]) {
        if (run_fixture(fixture) != 0) {
            fprintf(stderr, "ATRAC3P-FIXTURE: FAIL\n");
            g_failures++;
        } else {
            g_checks++;
            printf("ATRAC3P-FIXTURE: PASS\n");
        }
    } else {
        printf("ATRAC3P-FIXTURE: SKIP (set ATRAC3P_FIXTURE=<dir> with stream.bin + meta.txt)\n");
    }

    if (g_failures) {
        fprintf(stderr, "ATRAC3P-SELFTEST: %d of %d checks FAILED\n",
                g_failures, g_checks);
        return 1;
    }
    printf("ATRAC3P-SELFTEST: all %d public checks passed\n", g_checks);
    return 0;
}
