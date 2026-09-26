// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Public-safe SDL3 host audio output backend.
 *
 * Provides a redistributable, public-safe host audio backend using SDL3
 * (already a project dependency). Consumes the runtime PCM/mix contract
 * from sceAudio / SAS without altering guest-visible timing.
 * If no host audio device is available or headless mode is selected,
 * fails closed gracefully with a clear diagnostic and open-loop guest pacing.
 */

#define SDL_MAIN_HANDLED 1

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>

#include "perf.h"

#define SR_AUDIO_CHANNELS 9
#define SR_AUDIO_OUTPUT2_CH 8
#define SR_AUDIO_SAMPLE_RATE 44100
#define SR_AUDIO_MAX_QUEUED_FRAMES (65536 * 4)

typedef enum {
    AUDIO_STATE_UNINITIALIZED = 0,
    AUDIO_STATE_ACTIVE,
    AUDIO_STATE_UNAVAILABLE,
    AUDIO_STATE_NULL
} AudioBackendState;

typedef struct {
    uint64_t total_pushed_frames;
    uint64_t channel_pushed_frames[SR_AUDIO_CHANNELS];
    uint64_t underrun_events;
    uint64_t overrun_events;
    uint64_t backpressure_events;
    uint32_t peak_queued_frames[SR_AUDIO_CHANNELS];
} AudioStats;

static AudioBackendState s_audio_state = AUDIO_STATE_UNINITIALIZED;
static SDL_AudioDeviceID s_device_id = 0;
static SDL_AudioStream *s_streams[SR_AUDIO_CHANNELS] = {NULL};
static AudioStats s_stats;
static int s_reported_unavailable = 0;
static int s_reported_init = 0;
static int s_cleanup_registered = 0;

/* SR_AUDIOSTAT gate, matching the other backend: the queue/drift fields are diagnostics and
 * must not change the default output of a public build. */
static int audio_stat_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_AUDIOSTAT") != NULL;
    return on;
}

static void sr_audio_cleanup(void) {
    if (s_audio_state == AUDIO_STATE_ACTIVE) {
        for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
            if (s_streams[i]) {
                SDL_DestroyAudioStream(s_streams[i]);
                s_streams[i] = NULL;
            }
        }
        if (s_device_id) {
            SDL_CloseAudioDevice(s_device_id);
            s_device_id = 0;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    s_audio_state = AUDIO_STATE_UNINITIALIZED;
}

/* Master-volume gain from SR_MASTER_VOLUME (player Settings > Master Volume,
 * 0..100). Pure and clamped: 0 mutes, 100 is unity. Callers pass 100 when the
 * variable is unset so a direct runtime launch keeps full volume. */
static float sr_audio_master_gain(int percent) {
    if (percent <= 0) return 0.0f;
    if (percent >= 100) return 1.0f;
    return (float)percent / 100.0f;
}

int sr_audio_init(void) {
    if (s_audio_state == AUDIO_STATE_ACTIVE) {
        return 0;
    }
    if (s_audio_state == AUDIO_STATE_UNAVAILABLE || s_audio_state == AUDIO_STATE_NULL) {
        return -1;
    }

    /* Check environment configuration for explicit null/disabled audio */
    const char *env_backend = getenv("SR_AUDIO_BACKEND");
    const char *env_disabled = getenv("SR_AUDIO_DISABLED");
    const char *env_headless = getenv("SR_HEADLESS");
    if ((env_backend && (strcmp(env_backend, "null") == 0 ||
                         strcmp(env_backend, "none") == 0 ||
                         strcmp(env_backend, "0") == 0 ||
                         strcmp(env_backend, "off") == 0)) ||
        (env_disabled && strcmp(env_disabled, "1") == 0) ||
        (env_headless && strcmp(env_headless, "1") == 0 && !env_backend)) {
        s_audio_state = AUDIO_STATE_NULL;
        if (!s_reported_unavailable) {
            fprintf(stderr, "audio: host audio output disabled by environment (null backend)\n");
            s_reported_unavailable = 1;
        }
        return -1;
    }

    if (env_backend && strcmp(env_backend, "dummy") == 0) {
        SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        s_audio_state = AUDIO_STATE_UNAVAILABLE;
        if (!s_reported_unavailable) {
            fprintf(stderr, "audio: host audio output unavailable: %s\n", SDL_GetError());
            s_reported_unavailable = 1;
        }
        return -1;
    }

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq = SR_AUDIO_SAMPLE_RATE;

    s_device_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!s_device_id) {
        s_audio_state = AUDIO_STATE_UNAVAILABLE;
        if (!s_reported_unavailable) {
            fprintf(stderr, "audio: host audio output unavailable (no audio device: %s)\n", SDL_GetError());
            s_reported_unavailable = 1;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        s_streams[i] = SDL_CreateAudioStream(&spec, &spec);
        if (!s_streams[i] || !SDL_BindAudioStream(s_device_id, s_streams[i])) {
            s_audio_state = AUDIO_STATE_UNAVAILABLE;
            if (!s_reported_unavailable) {
                fprintf(stderr, "audio: host audio output stream creation failed: %s\n", SDL_GetError());
                s_reported_unavailable = 1;
            }
            sr_audio_cleanup();
            return -1;
        }
    }

    {
        /* Apply the player's master volume once at backend init; the setting
         * is fixed for the child's lifetime (settings apply at next launch). */
        const char *mv = getenv("SR_MASTER_VOLUME");
        float gain = sr_audio_master_gain((mv && mv[0]) ? atoi(mv) : 100);
        for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
            if (!SDL_SetAudioStreamGain(s_streams[i], gain)) {
                fprintf(stderr, "audio: could not apply master volume: %s\n",
                        SDL_GetError());
            }
        }
    }

    s_audio_state = AUDIO_STATE_ACTIVE;
    if (!s_reported_init) {
        const char *driver = SDL_GetCurrentAudioDriver();
        fprintf(stderr, "audio: initialized SDL3 audio output (driver: %s, %uHz stereo 16-bit)\n",
                driver ? driver : "unknown", SR_AUDIO_SAMPLE_RATE);
        s_reported_init = 1;
    }

    if (!s_cleanup_registered) {
        atexit(sr_audio_cleanup);
        s_cleanup_registered = 1;
    }

    return 0;
}

void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || !lr || nframes <= 0) {
        return;
    }

    if (s_audio_state == AUDIO_STATE_UNINITIALIZED) {
        sr_audio_init();
    }
    if (s_audio_state != AUDIO_STATE_ACTIVE) {
        return;
    }

    int queued_bytes = SDL_GetAudioStreamQueued(s_streams[ch]);
    if (queued_bytes < 0) {
        return;
    }
    int cur_queued_frames = queued_bytes / 4;

    /* Instrumentation: underrun detection (queue had completely dried up after previous activity) */
    if (cur_queued_frames == 0 && s_stats.channel_pushed_frames[ch] > 0) {
        s_stats.underrun_events++;
    }

    /* Instrumentation: backpressure detection (queue has more than two periods of lead) */
    if (cur_queued_frames > 2 * nframes) {
        s_stats.backpressure_events++;
    }

    /* Backpressure ceiling: drop samples if queue has grown excessively to protect memory */
    if (cur_queued_frames >= SR_AUDIO_MAX_QUEUED_FRAMES) {
        s_stats.overrun_events++;
        return;
    }

    /* Volume scaling and submission */
    if (volL == 0x8000 && volR == 0x8000) {
        SDL_PutAudioStreamData(s_streams[ch], lr, nframes * 4);
    } else {
        int16_t stack_buf[1024 * 2];
        int16_t *buf = stack_buf;
        if (nframes > 1024) {
            buf = (int16_t *)malloc((size_t)nframes * 4);
            if (!buf) return;
        }
        for (int i = 0; i < nframes; i++) {
            int32_t l = ((int32_t)lr[i * 2] * volL) >> 15;
            int32_t r = ((int32_t)lr[i * 2 + 1] * volR) >> 15;
            buf[i * 2]     = (int16_t)(l < -32768 ? -32768 : (l > 32767 ? 32767 : l));
            buf[i * 2 + 1] = (int16_t)(r < -32768 ? -32768 : (r > 32767 ? 32767 : r));
        }
        SDL_PutAudioStreamData(s_streams[ch], buf, nframes * 4);
        if (buf != stack_buf) {
            free(buf);
        }
    }
    s_stats.total_pushed_frames += (uint64_t)nframes;
    s_stats.channel_pushed_frames[ch] += (uint64_t)nframes;

    int new_q = SDL_GetAudioStreamQueued(s_streams[ch]) / 4;
    if (new_q > (int)s_stats.peak_queued_frames[ch]) {
        s_stats.peak_queued_frames[ch] = (uint32_t)new_q;
    }
}

int sr_audio_queued(int ch) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS) {
        return -1;
    }
    if (s_audio_state == AUDIO_STATE_UNINITIALIZED) {
        sr_audio_init();
    }
    if (s_audio_state != AUDIO_STATE_ACTIVE) {
        return -1;
    }

    int bytes = SDL_GetAudioStreamQueued(s_streams[ch]);
    if (bytes < 0) {
        return -1;
    }
    return bytes / 4;
}

/* SR_AUDIOSTAT: the value the blocking output paces against, and the drift it is accumulating.
 * `worst` is the largest per-channel sr_audio_queued() in this backend -- the lead the slowest
 * channel carries -- and `lead_ms` is every channel's un-consumed audio in milliseconds.
 * The arithmetic is separated from the device query so it can be checked exactly, with no
 * device and no race against the driver draining the queue as it is read. */
static void audio_lead_from(const int *queued_frames, int channels, int *worst, long *lead_ms) {
    uint64_t lead = 0;
    int q = 0;
    for (int i = 0; i < channels; i++) {
        int frames = queued_frames[i] > 0 ? queued_frames[i] : 0;
        if (frames > q) q = frames;
        lead += (uint64_t)frames;
    }
    *worst = q;
    *lead_ms = (long)(lead / (SR_AUDIO_SAMPLE_RATE / 1000u));
}

static void audio_lead(int *worst, long *lead_ms) {
    /* Without a device there is no queue to be ahead of, so both stay -1 ("not measured")
     * rather than a zero that would read as "no drift". */
    *worst = -1;
    *lead_ms = -1;
    if (s_audio_state != AUDIO_STATE_ACTIVE) return;
    int queued[SR_AUDIO_CHANNELS];
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        int bytes = SDL_GetAudioStreamQueued(s_streams[i]);
        queued[i] = bytes > 0 ? bytes / 4 : 0;
    }
    audio_lead_from(queued, SR_AUDIO_CHANNELS, worst, lead_ms);
}

void sr_audio_dump_stats(void) {
    if (s_audio_state == AUDIO_STATE_UNINITIALIZED) {
        sr_audio_init();
    }
    uint32_t peak = 0;
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (s_stats.peak_queued_frames[i] > peak) {
            peak = s_stats.peak_queued_frames[i];
        }
    }
    const char *driver = (s_audio_state == AUDIO_STATE_ACTIVE) ? SDL_GetCurrentAudioDriver() : NULL;
    const char *state_str = (s_audio_state == AUDIO_STATE_ACTIVE) ? "active" :
                            (s_audio_state == AUDIO_STATE_NULL) ? "null" : "unavailable";
    /* SR_AUDIOSTAT: the value the blocking output paces against, and the drift it is
     * accumulating (see audio_lead). */
    long lead_ms = -1;
    int worst = -1;
    if (audio_stat_on()) audio_lead(&worst, &lead_ms);
    fprintf(stderr, "AUDIOSTAT_HOST: state=%s driver=%s pushed=%llu underruns=%llu overruns=%llu "
                    "backpressure=%llu peak_q=%u queued=%d lead_ms=%ld\n",
            state_str,
            driver ? driver : "none",
            (unsigned long long)s_stats.total_pushed_frames,
            (unsigned long long)s_stats.underrun_events,
            (unsigned long long)s_stats.overrun_events,
            (unsigned long long)s_stats.backpressure_events,
            (unsigned)peak, worst, lead_ms);
    fflush(stderr);
}

int sr_audio_is_active(void) {
    return s_audio_state == AUDIO_STATE_ACTIVE;
}

#ifdef SR_AUDIO_SELFTEST
#include <assert.h>
#include <math.h>

static void test_reset(void) {
    sr_audio_cleanup();
    s_audio_state = AUDIO_STATE_UNINITIALIZED;
    s_reported_unavailable = 0;
    s_reported_init = 0;
    memset(&s_stats, 0, sizeof(s_stats));
}

static void test_dummy_mixer_handoff(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    int rc = sr_audio_init();
    assert(rc == 0);
    assert(s_audio_state == AUDIO_STATE_ACTIVE);

    /* Initial state: 0 frames queued on all channels */
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        assert(sr_audio_queued(i) == 0);
    }

    /* Generate 512 frames of 440Hz test sine tone */
    int16_t tone[512 * 2];
    for (int i = 0; i < 512; i++) {
        int16_t sample = (int16_t)(sin(2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * 16000.0);
        tone[i * 2]     = sample;
        tone[i * 2 + 1] = sample;
    }

    /* Push to channel 0 */
    sr_audio_push(0, tone, 512, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == 512);
    assert(sr_audio_queued(1) == 0); /* Channel isolation */
    assert(sr_audio_queued(8) == 0);

    /* Push to channel 8 (AudioOutput2) with panned volume */
    sr_audio_push(8, tone, 256, 0x4000, 0x8000);
    assert(sr_audio_queued(8) == 256);
    assert(sr_audio_queued(0) == 512);

    /* The dummy driver consumes queued data in real time. Poll against a
     * generous deadline rather than a fixed sleep so a loaded CI host
     * cannot turn scheduling jitter into a failure. */
    Uint64 deadline = SDL_GetTicks() + 5000;
    while ((sr_audio_queued(0) != 0 || sr_audio_queued(8) != 0) &&
           SDL_GetTicks() < deadline) {
        SDL_Delay(5);
    }
    assert(sr_audio_queued(0) == 0);
    assert(sr_audio_queued(8) == 0);

    /* Second push after drain triggers underrun tracking */
    sr_audio_push(0, tone, 128, 0x8000, 0x8000);
    assert(s_stats.underrun_events >= 1);

    /* SR_AUDIOSTAT drift telemetry: the published worst-channel lead is the value the
     * blocking output paces against, and the drift is every channel's un-consumed audio. */
    int worst = -2;
    long lead_ms = -2;
    audio_lead_from((const int[SR_AUDIO_CHANNELS]){0}, SR_AUDIO_CHANNELS, &worst, &lead_ms);
    assert(worst == 0);
    assert(lead_ms == 0);

    const int led[] = { 4410, 0, 0, 2205, 0, 0, 0, 0, 8820 };
    audio_lead_from(led, SR_AUDIO_CHANNELS, &worst, &lead_ms);
    assert(worst == 8820);
    assert(lead_ms == 350);
    assert(worst == led[8]);

    /* A negative reading from the device is not a negative lead. */
    const int odd[] = { -1, -100, 0 };
    audio_lead_from(odd, 3, &worst, &lead_ms);
    assert(worst == 0);
    assert(lead_ms == 0);

    /* With the device draining in real time the exact value races, but the published number
     * is still one of this backend's own sr_audio_queued() readings and never exceeds it. */
    audio_lead(&worst, &lead_ms);
    assert(worst >= 0);
    assert(worst >= sr_audio_queued(0));

    sr_audio_dump_stats();
    printf("test_dummy_mixer_handoff: PASS\n");
}

static void test_no_device_path(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "nonexistent_driver_xyz", SDL_HINT_OVERRIDE);

    int rc = sr_audio_init();
    assert(rc == -1);
    assert(s_audio_state == AUDIO_STATE_UNAVAILABLE);
    assert(sr_audio_queued(0) == -1);

    /* No device means no queue, so drift is not measurable: -1, never 0. */
    int worst = 0;
    long lead_ms = 0;
    audio_lead(&worst, &lead_ms);
    assert(worst == -1);
    assert(lead_ms == -1);

    /* Push must fail-closed safely without crash */
    int16_t dummy[128 * 2] = {0};
    sr_audio_push(0, dummy, 128, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == -1);

    sr_audio_dump_stats();
    printf("test_no_device_path: PASS\n");
}

static void test_null_backend_env(void) {
    test_reset();
#if defined(_WIN32) || defined(_WIN64)
    _putenv("SR_AUDIO_BACKEND=null");
#else
    setenv("SR_AUDIO_BACKEND", "null", 1);
#endif

    int rc = sr_audio_init();
    assert(rc == -1);
    assert(s_audio_state == AUDIO_STATE_NULL);
    assert(sr_audio_queued(0) == -1);

#if defined(_WIN32) || defined(_WIN64)
    _putenv("SR_AUDIO_BACKEND=");
#else
    unsetenv("SR_AUDIO_BACKEND");
#endif
    printf("test_null_backend_env: PASS\n");
}

/* Master-volume gain mapping (SR_MASTER_VOLUME): pure, window-free. */
static void test_master_volume_gain(void) {
    assert(sr_audio_master_gain(0) == 0.0f);
    assert(sr_audio_master_gain(-5) == 0.0f);
    assert(sr_audio_master_gain(100) == 1.0f);
    assert(sr_audio_master_gain(250) == 1.0f);
    assert(sr_audio_master_gain(50) == 0.5f);
    assert(sr_audio_master_gain(80) > 0.7999f && sr_audio_master_gain(80) < 0.8001f);
}

int main(int argc, char **argv) {
    sr_perf_init();
    (void)argc;
    (void)argv;
    printf("--- Running audio selftest ---\n");
    test_master_volume_gain();
    test_dummy_mixer_handoff();
    test_no_device_path();
    test_null_backend_env();
    test_reset();
    printf("ALL AUDIO HOST TESTS PASSED\n");
    return 0;
}
#endif
