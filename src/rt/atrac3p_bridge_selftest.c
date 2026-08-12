// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// atrac3p_bridge_selftest.c — regression suite for the PSP ATRAC3+ HLE
// decode bridge (src/rt/atrac3p_bridge.c, PR-B).
//
// Public, source-owned tests (no game inputs, no copyrighted fixtures):
//   - create() validation (channel set {1,2,3,4,6,7,8}, block_align > 0,
//     NULL out),
//   - decode() NULL-argument rejection and the exact-frame contract
//     (frame_size must equal block_align; anything else is a contract
//     violation, not a decodable frame),
//   - the deterministic terminator frame (0x60) decodes through the bridge
//     with the nb_samples = ATRAC3P_FRAME_SAMPLES PSP contract and
//     all-zero PCM,
//   - the transform-path canary MONO unit frame decodes deterministically
//     through the bridge (reaches reconstruct_frame(); broken transform
//     configs crash here, mirroring the PR-A canary),
//   - determinism: byte-identical PCM across instances, after bridge reset,
//     and after destroy/recreate (the HLE seek/loop-rewind primitive),
//   - destroy(NULL) no-op.
//
// Exit code 0 = all checks passed.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atrac3p_bridge.h"
#include "vfpu_tables.h"

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
 * (CH_UNIT_TERMINATOR) -> 0b01100000 = 0x60. Decodes to 2048 all-zero
 * samples per channel. */
static const uint8_t TERMINATOR_FRAME[] = { 0x60 };

/* Deterministic MONO channel-unit frame; exercises the IMDCT/IPQF transform
 * pipeline (see atrac3p_selftest.c for the full rationale). */
static const uint8_t MONO_UNIT_FRAME[] = { 0x16, 0x0c, 0xf6, 0x86, 0x87, 0x35, 0xd3, 0x1c, 0xb8 };

static int pcm_sha256_hex(const int16_t *pcm, size_t samples, char out[65])
{
    uint8_t digest[32];
    char *p = out;
    size_t i;

    sr_vfpu_sha256((const uint8_t *)pcm, samples * sizeof(int16_t), digest);
    for (i = 0; i < 32; i++)
        p += sprintf(p, "%02x", digest[i]);
    *p = '\0';
    return 0;
}

static void test_create_validation(void)
{
    Atrac3pBridge *b = NULL;

    CHECK(atrac3p_bridge_create(0, 1536, &b) < 0, "reject 0 channels");
    CHECK(atrac3p_bridge_create(5, 1536, &b) < 0, "reject 5 channels");
    CHECK(atrac3p_bridge_create(9, 1536, &b) < 0, "reject 9 channels");
    CHECK(atrac3p_bridge_create(2, 0, &b) < 0, "reject block_align 0");
    CHECK(atrac3p_bridge_create(2, -1, &b) < 0, "reject negative block_align");
    CHECK(atrac3p_bridge_create(2, 1536, NULL) < 0, "reject NULL out");
    CHECK(b == NULL, "no instance left behind on failure");
    /* valid configs across the whole accepted channel set */
    {
        static const int chans[] = { 1, 2, 3, 4, 6, 7, 8 };
        for (size_t i = 0; i < sizeof(chans) / sizeof(chans[0]); i++) {
            Atrac3pBridge *ok = NULL;
            CHECK(atrac3p_bridge_create(chans[i], 1536, &ok) == 0,
                  "create valid channel count");
            atrac3p_bridge_destroy(ok);
        }
    }
}

static void test_decode_contract(void)
{
    Atrac3pBridge *b = NULL;
    int16_t pcm[ATRAC3P_MAX_CHANNELS * ATRAC3P_FRAME_SAMPLES];
    int samples = -1;

    CHECK(atrac3p_bridge_create(2, 1536, &b) == 0, "create stereo bridge");
    if (!b) return;

    /* NULL argument rejection: every required pointer is validated before any
     * decoder state is touched. */
    CHECK(atrac3p_bridge_decode(NULL, TERMINATOR_FRAME, 1, pcm, &samples) < 0,
          "reject NULL bridge");
    CHECK(atrac3p_bridge_decode(b, NULL, 1, pcm, &samples) < 0,
          "reject NULL frame");
    CHECK(atrac3p_bridge_decode(b, TERMINATOR_FRAME, 1, NULL, &samples) < 0,
          "reject NULL pcm_out");
    CHECK(atrac3p_bridge_decode(b, TERMINATOR_FRAME, 1, pcm, NULL) < 0,
          "reject NULL samples_out");

    /* Exact-frame contract: the HLE feeds one blockAlign-sized frame. A
     * different size is a caller contract violation and must be rejected
     * with samples_out left at 0, not partially decoded. */
    samples = -1;
    CHECK(atrac3p_bridge_decode(b, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm, &samples) < 0,
          "reject frame_size != block_align (too small)");
    CHECK(samples == 0, "samples_out zeroed on contract violation");

    atrac3p_bridge_destroy(b);
}

static void test_terminator_decode(void)
{
    Atrac3pBridge *b1 = NULL, *b2 = NULL;
    int16_t pcm1[2 * ATRAC3P_FRAME_SAMPLES];
    int16_t pcm2[2 * ATRAC3P_FRAME_SAMPLES];
    char sha1[65], sha2[65];
    int ret, samples = -1, i;
    const size_t n = 2 * (size_t)ATRAC3P_FRAME_SAMPLES;

    /* The terminator canary uses block_align 1 in the PR-A suite; here the
     * bridge enforces the exact-frame contract, so the frame must fill the
     * configured block_align. Create with block_align 1 (a legal PSP-adjacent
     * config for a single-byte frame) and stereo output. */
    CHECK(atrac3p_bridge_create(2, 1, &b1) == 0, "create h1 (terminator)");
    CHECK(atrac3p_bridge_create(2, 1, &b2) == 0, "create h2 (terminator)");
    if (!b1 || !b2) return;

    ret = atrac3p_bridge_decode(b1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm1, &samples);
    CHECK(ret == 0, "terminator decode succeeds through bridge");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "bridge nb_samples = 2048");
    for (i = 0; i < (int)n; i++)
        CHECK(pcm1[i] == 0, "terminator PCM all-zero through bridge");

    /* determinism across instances */
    CHECK(atrac3p_bridge_decode(b2, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm2, &samples) == 0, "h2 terminator decode");
    pcm_sha256_hex(pcm1, n, sha1);
    pcm_sha256_hex(pcm2, n, sha2);
    CHECK(strcmp(sha1, sha2) == 0, "deterministic PCM across bridge instances");

    /* reset restores post-create state (HLE seek / loop-rewind primitive) */
    CHECK(atrac3p_bridge_reset(b1) == 0, "bridge reset");
    CHECK(atrac3p_bridge_decode(b1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm1, &samples) == 0, "decode after reset");
    pcm_sha256_hex(pcm1, n, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "PCM after bridge reset identical");

    /* destroy/recreate produces the same output (fresh-instance determinism) */
    atrac3p_bridge_destroy(b1);
    b1 = NULL;
    CHECK(atrac3p_bridge_create(2, 1, &b1) == 0, "recreate bridge");
    CHECK(atrac3p_bridge_decode(b1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm1, &samples) == 0, "decode after recreate");
    pcm_sha256_hex(pcm1, n, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "PCM after recreate identical");

    atrac3p_bridge_destroy(b1);
    atrac3p_bridge_destroy(b2);
}

static void test_imdct_path_decode(void)
{
    Atrac3pBridge *b1 = NULL, *b2 = NULL;
    int16_t pcm1[ATRAC3P_FRAME_SAMPLES];
    int16_t pcm2[ATRAC3P_FRAME_SAMPLES];
    char sha1[65], sha2[65];
    int ret, samples = -1;

    /* The bridge enforces the exact-frame contract (frame_size == block_align),
     * so the canary is configured with block_align == its own size; the decoder
     * behaviour is identical to the PR-A canary (which used a wider align). */
    CHECK(atrac3p_bridge_create(1, (int)sizeof(MONO_UNIT_FRAME), &b1) == 0,
          "create bridge h1");
    CHECK(atrac3p_bridge_create(1, (int)sizeof(MONO_UNIT_FRAME), &b2) == 0,
          "create bridge h2");
    if (!b1 || !b2) return;

    ret = atrac3p_bridge_decode(b1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                                pcm1, &samples);
    CHECK(ret == 0, "mono unit decode through bridge");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "mono unit bridge nb_samples");

    CHECK(atrac3p_bridge_decode(b2, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                                pcm2, &samples) == 0, "h2 mono unit decode");
    pcm_sha256_hex(pcm1, ATRAC3P_FRAME_SAMPLES, sha1);
    pcm_sha256_hex(pcm2, ATRAC3P_FRAME_SAMPLES, sha2);
    CHECK(strcmp(sha1, sha2) == 0, "mono unit PCM deterministic across bridges");

    CHECK(atrac3p_bridge_reset(b1) == 0, "reset bridge h1");
    CHECK(atrac3p_bridge_decode(b1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                                pcm1, &samples) == 0, "mono unit after reset");
    pcm_sha256_hex(pcm1, ATRAC3P_FRAME_SAMPLES, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "mono unit PCM deterministic after reset");

    atrac3p_bridge_destroy(b1);
    atrac3p_bridge_destroy(b2);
}

int main(void)
{
    test_create_validation();
    test_decode_contract();
    test_terminator_decode();
    test_imdct_path_decode();

    atrac3p_bridge_destroy(NULL);   /* no-op */

    if (g_failures == 0)
        fprintf(stderr, "ATRAC3P-BRIDGE-SELFTEST: all %d checks passed\n", g_checks);
    else
        fprintf(stderr, "ATRAC3P-BRIDGE-SELFTEST: %d/%d checks FAILED\n",
                g_failures, g_checks);
    return g_failures ? 1 : 0;
}
