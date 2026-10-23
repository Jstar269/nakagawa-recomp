/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_SHOWCASE_TONE_H
#define NAKAGAWA_SHOWCASE_TONE_H

#include <pspaudio.h>
#include <stdint.h>

/* Keep the synthetic note queued across the scheduler boundary exercised by
 * issue #1171's pause smoke; the production runtime still uses its guest sample
 * count and never extends a real title's audio. */
#ifndef SHOWCASE_AUDIO_FRAMES
#define SHOWCASE_AUDIO_FRAMES 512
#endif

static int showcase_audio_channel = -1;
static int16_t showcase_audio_buffer[SHOWCASE_AUDIO_FRAMES * 2];

static inline void showcase_audio_init(void) {
    if (showcase_audio_channel < 0) {
        showcase_audio_channel = sceAudioChReserve(-1, SHOWCASE_AUDIO_FRAMES,
                                                   PSP_AUDIO_FORMAT_STEREO);
    }
}

static inline void showcase_tone(int frequency) {
    const int half_period = 22050 / (frequency > 0 ? frequency : 440);
    int16_t sample = 7000;
    int16_t phase = 0;
    if (showcase_audio_channel < 0) return;
    for (int i = 0; i < SHOWCASE_AUDIO_FRAMES; i++) {
        if (++phase >= half_period) {
            phase = 0;
            sample = (int16_t)-sample;
        }
        showcase_audio_buffer[i * 2] = sample;
        showcase_audio_buffer[i * 2 + 1] = sample;
    }
    sceAudioOutputPannedBlocking(showcase_audio_channel, PSP_AUDIO_VOLUME_MAX,
                                 PSP_AUDIO_VOLUME_MAX, showcase_audio_buffer);
}

#endif
