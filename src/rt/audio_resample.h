/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#ifndef SR_AUDIO_RESAMPLE_H
#define SR_AUDIO_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#define SR_AUDIO_RESAMPLER_OUTPUT_RATE 44100u

/* A streaming stereo signed-16-bit converter. The state starts as {0}; init
 * releases anything an earlier init allocated, so it may be called again on the
 * same state. reset keeps the rate-specific filter table allocated while
 * clearing phase, history, and queued output. */
typedef struct SrAudioResampler {
    uint32_t input_rate;
    uint64_t input_frames;
    uint64_t output_phase;
    uint64_t next_input_whole;
    uint64_t next_input_remainder;
    uint32_t history_count;
    size_t history_capacity;
    int32_t *coefficients_q30;
    int16_t *history;
    int16_t *pending_output;
    size_t pending_capacity;
    size_t pending_head;
    size_t pending_frames;
} SrAudioResampler;

int sr_audio_resampler_init(SrAudioResampler *state, uint32_t input_rate);
void sr_audio_resampler_reset(SrAudioResampler *state);
void sr_audio_resampler_destroy(SrAudioResampler *state);
int sr_audio_resampler_push(SrAudioResampler *state, const int16_t *stereo,
                             size_t frames);
size_t sr_audio_resampler_pull(SrAudioResampler *state, int16_t *stereo,
                               size_t frame_capacity);

#endif
