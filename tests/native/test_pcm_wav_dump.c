/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The selftest WAV writer (tests/native/pcm_wav_dump.c over the vendored
   dr_wav): synthetic PCM written and read back frame for frame, the RIFF
   header fields, the refused arguments and names, and NK_PCM_WAV_DIR
   handling. Writes only beneath the BUILD_ROOT the Makefile passes. */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "pcm_wav_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/dr_libs/dr_wav.h"

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

static int g_failures = 0;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);       \
        }                                                                  \
    } while (0)

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value != NULL ? value : "");
#else
    if (value != NULL) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    n = ftell(f);
    fclose(f);
    return n;
}

static void test_round_trip(void) {
    static const char path[] = SR_SELFTEST_BUILD_ROOT "/test_pcm_wav_dump_stereo.wav";
    enum { FRAMES = 300, CHANNELS = 2 };
    int16_t pcm[FRAMES * CHANNELS];
    int16_t back[FRAMES * CHANNELS + 2];
    unsigned char head[12];
    drwav wav;
    FILE *f;
    size_t i;
    /* A deterministic ramp that crosses both int16 limits. */
    for (i = 0; i < (size_t)(FRAMES * CHANNELS); ++i) {
        pcm[i] = (int16_t)((int32_t)(i * 221u % 65536u) - 32768);
    }
    remove(path);
    CHECK(nk_pcm_wav_write(path, pcm, FRAMES, CHANNELS, 44100u) == 1, "stereo write");
    /* The 44-byte canonical header plus 2 bytes per sample. */
    CHECK(file_size(path) == 44L + FRAMES * CHANNELS * 2L, "file size");
    memset(head, 0, sizeof(head));
    f = fopen(path, "rb");
    CHECK(f != NULL && fread(head, 1, sizeof(head), f) == sizeof(head), "read header");
    if (f != NULL) fclose(f);
    CHECK(memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0, "RIFF and WAVE tags");
    if (!drwav_init_file(&wav, path, NULL)) {
        CHECK(0, "reopen");
        return;
    }
    CHECK(wav.channels == CHANNELS && wav.sampleRate == 44100u && wav.bitsPerSample == 16u &&
              wav.translatedFormatTag == DR_WAVE_FORMAT_PCM && wav.totalPCMFrameCount == FRAMES,
          "header fields");
    memset(back, 0x5a, sizeof(back));
    CHECK(drwav_read_pcm_frames_s16(&wav, FRAMES + 1u, back) == FRAMES, "frame count");
    CHECK(memcmp(back, pcm, sizeof(pcm)) == 0, "samples identical");
    drwav_uninit(&wav);
    remove(path);
}

static void test_empty_mono(void) {
    static const char path[] = SR_SELFTEST_BUILD_ROOT "/test_pcm_wav_dump_empty.wav";
    drwav wav;
    remove(path);
    CHECK(nk_pcm_wav_write(path, NULL, 0u, 1u, 32000u) == 1, "zero-frame write");
    if (!drwav_init_file(&wav, path, NULL)) {
        CHECK(0, "reopen empty");
        return;
    }
    CHECK(wav.channels == 1u && wav.sampleRate == 32000u && wav.totalPCMFrameCount == 0u,
          "empty header fields");
    drwav_uninit(&wav);
    remove(path);
}

static void test_refusals(void) {
    static const char path[] = SR_SELFTEST_BUILD_ROOT "/test_pcm_wav_dump_refused.wav";
    int16_t one[9] = {0};
    remove(path);
    CHECK(nk_pcm_wav_write(NULL, one, 1u, 1u, 44100u) == 0, "NULL path");
    CHECK(nk_pcm_wav_write("", one, 1u, 1u, 44100u) == 0, "empty path");
    CHECK(nk_pcm_wav_write(path, NULL, 1u, 1u, 44100u) == 0, "NULL pcm with frames");
    CHECK(nk_pcm_wav_write(path, one, 1u, 0u, 44100u) == 0, "zero channels");
    CHECK(nk_pcm_wav_write(path, one, 1u, 9u, 44100u) == 0, "nine channels");
    CHECK(nk_pcm_wav_write(path, one, 1u, 1u, 0u) == 0, "zero rate");
    CHECK(nk_pcm_wav_write(path, one, 1u, 1u, 384001u) == 0, "rate above 384000");
    /* 0x7FFFFFEA mono frames need 0xFFFFFFD4 data bytes: past the 32-bit RIFF
       size once the 44-byte header is added. Refused before any write. */
    CHECK(nk_pcm_wav_write(path, one, (size_t)0x7FFFFFEAu, 1u, 44100u) == 0,
          "data chunk past 4 GiB");
    CHECK(file_size(path) == -1, "no file left by refusals");
}

static void test_dump_env(void) {
    static const char path[] = SR_SELFTEST_BUILD_ROOT "/nk_pcm_wav_dump_probe.wav";
    int16_t pcm[4] = {1, -1, 32767, -32768};
    remove(path);
    set_env(NK_PCM_WAV_DIR_ENV, NULL);
    CHECK(nk_pcm_wav_dump("nk_pcm_wav_dump_probe", pcm, 4u, 1u, 44100u) == 0, "unset: not asked");
    CHECK(file_size(path) == -1, "unset: nothing written");
    set_env(NK_PCM_WAV_DIR_ENV, SR_SELFTEST_BUILD_ROOT);
    CHECK(nk_pcm_wav_dump("nk_pcm_wav_dump_probe", pcm, 4u, 1u, 44100u) == 1, "set: written");
    CHECK(file_size(path) == 44L + 8L, "set: file size");
    CHECK(nk_pcm_wav_dump("../escape", pcm, 4u, 1u, 44100u) == -1, "path separator refused");
    CHECK(nk_pcm_wav_dump("a.b", pcm, 4u, 1u, 44100u) == -1, "dot refused");
    CHECK(nk_pcm_wav_dump("", pcm, 4u, 1u, 44100u) == -1, "empty name refused");
    CHECK(nk_pcm_wav_dump(NULL, pcm, 4u, 1u, 44100u) == -1, "NULL name refused");
    CHECK(nk_pcm_wav_dump("x0123456789012345678901234567890123456789012345678901234567890123",
                          pcm, 4u, 1u, 44100u) == -1,
          "65-character name refused");
    set_env(NK_PCM_WAV_DIR_ENV, NULL);
    remove(path);
}

int main(void) {
    test_round_trip();
    test_empty_mono();
    test_refusals();
    test_dump_env();
    if (g_failures != 0) {
        fprintf(stderr, "test_pcm_wav_dump: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_pcm_wav_dump: OK\n");
    return 0;
}
