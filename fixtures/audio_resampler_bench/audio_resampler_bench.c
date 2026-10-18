/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * Synthetic-input microbenchmark for the streaming audio resampler
 * (sr_audio_resampler_init / push / pull). It uses no game data: the input is a
 * fixed integer triangle-plus-noise signal from a fixed-seed xorshift generator,
 * and the push schedule is a fixed sequence of chunk sizes, so every build
 * converts identical bytes.
 *
 * Usage: audio_resampler_bench [dump-file]
 *   One line per case: the wall time of each timed pass (ns) and an FNV-1a 64
 *   hash of the converted stereo output. Each case runs one untimed warm-up
 *   pass, which defines the reference output, followed by the timed passes.
 *   The optional dump file receives the warm-up output of every case as
 *   little-endian int16 so two builds can be compared with sha256sum.
 * Exit status is nonzero if any timed pass disagrees with the warm-up pass.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_resample.h"
#include "perf.h"

#ifdef _WIN32
#include <windows.h>
static double bench_now_ns(void) {
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1e9 / (double)frequency.QuadPart;
}
#else
#include <time.h>
static double bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

#define BENCH_INPUT_FRAMES 65536u
#define BENCH_TIMED_PASSES 5u
#define BENCH_PUSH_CHUNK_MAX 1021u
#define BENCH_PULL_CHUNK 1024u
#define BENCH_SEED UINT64_C(0x2F6E2B1D4C9A7E35)

/* Input rates cover upsampling (22050, 32000), the identity-ratio path (44100),
 * and downsampling (48000). */
static const uint32_t bench_rates[] = {22050u, 32000u, 44100u, 48000u};

static uint64_t bench_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void bench_make_input(int16_t *stereo, size_t frames) {
    uint64_t rng = BENCH_SEED;
    for (size_t i = 0; i < frames; i++) {
        for (unsigned c = 0; c < 2u; c++) {
            /* Integer triangle wave (period 4096 frames, per-channel rate and
             * offset) plus bounded noise, so taps see the full sample range. */
            uint32_t phase = (uint32_t)((i * (c ? 97u : 61u) + c * 1024u) % 4096u);
            int32_t tri = phase < 2048u ? (int32_t)phase - 1024 : 3071 - (int32_t)phase;
            int32_t noise = (int32_t)(bench_next(&rng) % 4001u) - 2000;
            int32_t value = tri * 24 + noise;
            if (value > INT16_MAX) value = INT16_MAX;
            if (value < INT16_MIN) value = INT16_MIN;
            stereo[i * 2u + c] = (int16_t)value;
        }
    }
}

static size_t bench_chunk_frames(size_t k) {
    return 1u + (k * 7919u) % BENCH_PUSH_CHUNK_MAX;
}

static size_t bench_output_capacity(size_t frames, uint32_t rate) {
    return (size_t)((uint64_t)frames * SR_AUDIO_RESAMPLER_OUTPUT_RATE / rate) + 16u;
}

/* Converts the whole input through the streaming API after a reset, so the
 * coefficient table built by init is reused and excluded from the timed region.
 * Returns the number of output frames, or (size_t)-1 on failure. */
static size_t bench_convert(SrAudioResampler *state, const int16_t *input,
                            size_t frames, int16_t *output, size_t output_capacity) {
    size_t consumed = 0;
    size_t produced = 0;
    size_t chunk_index = 0;

    sr_audio_resampler_reset(state);
    while (consumed < frames) {
        size_t chunk = bench_chunk_frames(chunk_index++);
        if (chunk > frames - consumed) chunk = frames - consumed;
        if (!sr_audio_resampler_push(state, input + consumed * 2u, chunk))
            return (size_t)-1;
        consumed += chunk;
        for (;;) {
            size_t room = output_capacity - produced;
            size_t want = room < BENCH_PULL_CHUNK ? room : BENCH_PULL_CHUNK;
            size_t got;
            if (!want) return (size_t)-1;
            got = sr_audio_resampler_pull(state, output + produced * 2u, want);
            if (!got) break;
            produced += got;
        }
    }
    return produced;
}

static uint64_t bench_hash(const int16_t *samples, size_t count) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < count; i++) {
        uint16_t bits = (uint16_t)samples[i];
        for (unsigned b = 0; b < 2u; b++) {
            hash ^= (uint64_t)((bits >> (8u * b)) & 0xffu);
            hash *= UINT64_C(0x100000001b3);
        }
    }
    return hash;
}

static int bench_dump_samples(FILE *dump, const int16_t *samples, size_t count) {
    unsigned char *bytes;
    size_t i;
    if (!dump) return 1;
    bytes = malloc(count * 2u);
    if (!bytes) return 0;
    for (i = 0; i < count; i++) {
        uint16_t bits = (uint16_t)samples[i];
        bytes[i * 2u] = (unsigned char)(bits & 0xffu);
        bytes[i * 2u + 1u] = (unsigned char)(bits >> 8);
    }
    i = fwrite(bytes, 1u, count * 2u, dump);
    free(bytes);
    return i == count * 2u;
}

int main(int argc, char **argv) {
    const char *dump_path = argc > 1 ? argv[1] : NULL;
    const size_t case_count = sizeof(bench_rates) / sizeof(bench_rates[0]);
    int16_t *input;
    FILE *dump = NULL;
    int failed = 0;

    sr_perf_init();
    input = malloc((size_t)BENCH_INPUT_FRAMES * 2u * sizeof(*input));
    if (!input) return 2;
    bench_make_input(input, BENCH_INPUT_FRAMES);
    if (dump_path) {
        dump = fopen(dump_path, "wb");
        if (!dump) {
            fprintf(stderr, "audio_resampler_bench: cannot open dump file\n");
            free(input);
            return 2;
        }
    }

    for (size_t c = 0; c < case_count && !failed; c++) {
        const uint32_t rate = bench_rates[c];
        const size_t capacity = bench_output_capacity(BENCH_INPUT_FRAMES, rate);
        int16_t *reference = malloc(capacity * 2u * sizeof(*reference));
        int16_t *scratch = malloc(capacity * 2u * sizeof(*scratch));
        SrAudioResampler state;
        size_t warm_frames;
        uint64_t warm_hash;

        memset(&state, 0, sizeof(state));
        if (!reference || !scratch || !sr_audio_resampler_init(&state, rate)) {
            fprintf(stderr, "audio_resampler_bench: setup failed at %lu Hz\n", (unsigned long)rate);
            failed = 1;
            break;
        }
        warm_frames = bench_convert(&state, input, BENCH_INPUT_FRAMES, reference, capacity);
        if (warm_frames == (size_t)-1) {
            fprintf(stderr, "audio_resampler_bench: warm-up conversion failed at %lu Hz\n",
                    (unsigned long)rate);
            failed = 1;
        } else {
            warm_hash = bench_hash(reference, warm_frames * 2u);
            if (!bench_dump_samples(dump, reference, warm_frames * 2u)) {
                fprintf(stderr, "audio_resampler_bench: dump write failed\n");
                failed = 1;
            }
            printf("AUDIO_RESAMPLER_BENCH rate=%lu in_frames=%lu out_frames=%lu hash=%016llx passes_ns=",
                   (unsigned long)rate, (unsigned long)BENCH_INPUT_FRAMES,
                   (unsigned long)warm_frames, (unsigned long long)warm_hash);
            for (unsigned pass = 0; pass < BENCH_TIMED_PASSES && !failed; pass++) {
                double started = bench_now_ns();
                size_t frames = bench_convert(&state, input, BENCH_INPUT_FRAMES, scratch, capacity);
                double elapsed = bench_now_ns() - started;
                if (frames != warm_frames || bench_hash(scratch, frames * 2u) != warm_hash) {
                    fprintf(stderr, "audio_resampler_bench: timed pass %u diverged at %lu Hz\n",
                            pass, (unsigned long)rate);
                    failed = 1;
                    break;
                }
                printf("%s%.0f", pass ? "," : "", elapsed);
            }
            printf("\n");
        }
        sr_audio_resampler_destroy(&state);
        free(reference);
        free(scratch);
    }

    if (dump) {
        if (fclose(dump) != 0) failed = 1;
    }
    free(input);
    return failed ? 1 : 0;
}
