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
#include <errno.h>
#include <math.h>
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

/* ---- SR_AUDIODUMP: debug-only capture of the device mix (off by default) ----------------
 *
 * With SR_AUDIODUMP=<path.wav> in the environment, every buffer SDL is about to submit to the
 * device -- the sum of all nine channel streams after the master gain -- is also written as
 * 16-bit PCM WAV at the device's own rate and channel count. That is the sound the host plays,
 * so it is the artefact an audit needs for silence, clipping, discontinuities and sample rate.
 * It is observation only: the callback never writes back into the buffer. It runs on SDL's
 * audio thread and does file I/O there, so a dump changes host pacing; never compare timing
 * with it enabled. The WAV header is patched once a second and at close, so an abnormal exit
 * still leaves a readable file up to its last patch. */
typedef struct {
    uint64_t frames;         /* sample frames written */
    uint64_t silent_frames;  /* frames where every channel was exactly zero */
    uint64_t clipped;        /* samples beyond full scale, before the 16-bit clamp */
    uint32_t peak;           /* largest magnitude written, after the clamp */
} SrAudioDumpStats;

#define SR_AUDIO_DUMP_CHUNK 1024u
#define SR_AUDIO_DUMP_MAX_DATA 0xFFFFFF00u   /* stop before the RIFF size field overflows */

static FILE *s_dump_file = NULL;
static char s_dump_path[512];
static uint32_t s_dump_rate = 0, s_dump_channels = 0;
static uint64_t s_dump_data_bytes = 0, s_dump_last_patch = 0;
static SrAudioDumpStats s_dump_stats;
static int s_dump_write_failed = 0;    /* the first short write ends the dump */

static void sr_put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void sr_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Canonical 44-byte PCM WAV header for 16-bit samples. Pure, so the selftest checks it. */
static void sr_audio_wav_header(uint8_t out[44], uint32_t rate, uint32_t channels, uint32_t data_bytes) {
    memcpy(out + 0, "RIFF", 4);
    sr_put_le32(out + 4, 36u + data_bytes);
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    sr_put_le32(out + 16, 16u);
    sr_put_le16(out + 20, 1u);                              /* PCM */
    sr_put_le16(out + 22, (uint16_t)channels);
    sr_put_le32(out + 24, rate);
    sr_put_le32(out + 28, rate * channels * 2u);            /* byte rate */
    sr_put_le16(out + 32, (uint16_t)(channels * 2u));       /* block align */
    sr_put_le16(out + 34, 16u);
    memcpy(out + 36, "data", 4);
    sr_put_le32(out + 40, data_bytes);
}

/* Float device mix (nominal full scale +-1.0) to 16-bit, rounding half away from zero. A sample
 * beyond the 16-bit range is counted as clipped and clamped; NaN reads as silence. Pure. */
static int16_t sr_audio_dump_sample(float x, uint64_t *clipped) {
    float scaled = x * 32768.0f;
    if (isnan(scaled)) return 0;
    if (scaled > 32767.0f) { (*clipped)++; return 32767; }
    if (scaled < -32768.0f) { (*clipped)++; return -32768; }
    return (int16_t)(int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

/* The first short write ends the dump: the header keeps its last good patch, later blocks are
 * dropped, and the close line says so. One message, not one per buffer. */
static void sr_audio_dump_write_failed(const char *what) {
    if (s_dump_write_failed) return;
    s_dump_write_failed = 1;
    fprintf(stderr, "AUDIODUMP: %s write to %s failed (%s); the dump stops here\n",
            what, s_dump_path, strerror(errno));
}

static void sr_audio_dump_patch_header(void) {
    uint8_t hdr[44];
    sr_audio_wav_header(hdr, s_dump_rate, s_dump_channels, (uint32_t)s_dump_data_bytes);
    if (fseek(s_dump_file, 0, SEEK_SET) != 0 || fwrite(hdr, 1, sizeof(hdr), s_dump_file) != sizeof(hdr)) {
        sr_audio_dump_write_failed("header");
    }
    if (fseek(s_dump_file, 0, SEEK_END) != 0) sr_audio_dump_write_failed("seek");
    fflush(s_dump_file);
    s_dump_last_patch = s_dump_data_bytes;
}

/* Convert and write `samples` interleaved float values (a whole number of frames). Returns the
 * number of samples written; fewer than `samples` only after a failed write. */
static size_t sr_audio_dump_block(const float *in, size_t samples) {
    int16_t out[SR_AUDIO_DUMP_CHUNK];
    size_t chunk = SR_AUDIO_DUMP_CHUNK - (SR_AUDIO_DUMP_CHUNK % s_dump_channels);
    size_t done = 0;
    for (; done < samples;) {
        size_t k = samples - done < chunk ? samples - done : chunk;
        for (size_t i = 0; i < k; i++) {
            int16_t v = sr_audio_dump_sample(in[done + i], &s_dump_stats.clipped);
            out[i] = v;
            uint32_t a = v < 0 ? (uint32_t)(-(int32_t)v) : (uint32_t)v;
            if (a > s_dump_stats.peak) s_dump_stats.peak = a;
        }
        if (fwrite(out, sizeof(int16_t), k, s_dump_file) != k) {
            sr_audio_dump_write_failed("data");
            return done;
        }
        for (size_t f = 0; f < k; f += s_dump_channels) {
            int any = 0;
            for (uint32_t c = 0; c < s_dump_channels; c++) any |= out[f + c] != 0;
            if (!any) s_dump_stats.silent_frames++;
        }
        s_dump_stats.frames += k / s_dump_channels;
        done += k;
    }
    return done;
}

/* Runs on SDL's audio thread. Thread contract: s_dump_file, s_dump_rate, s_dump_channels and
 * s_dump_path are written on the main thread before SDL_SetAudioPostmixCallback registers this
 * callback and after it is removed; SDL's registration and removal are the synchronization
 * points, so this callback never runs concurrently with those writes. s_dump_data_bytes,
 * s_dump_last_patch, s_dump_stats and s_dump_write_failed are written only here while the
 * callback is registered, and the close path reads them only after removing it. */
static void sr_audio_dump_postmix(void *userdata, const SDL_AudioSpec *spec, float *buffer, int buflen) {
    (void)userdata;
    if (!s_dump_file || !buffer || !spec || buflen < (int)sizeof(float)) return;
    /* A device whose format changed mid-run keeps the header it was opened with. */
    if ((uint32_t)spec->channels != s_dump_channels || (uint32_t)spec->freq != s_dump_rate) return;
    size_t samples = (size_t)buflen / sizeof(float);
    samples -= samples % s_dump_channels;
    if (!samples || s_dump_write_failed) return;
    if (s_dump_data_bytes + samples * 2u > SR_AUDIO_DUMP_MAX_DATA) return;
    s_dump_data_bytes += sr_audio_dump_block(buffer, samples) * 2u;
    if (s_dump_write_failed) return;
    if (s_dump_data_bytes - s_dump_last_patch >= (uint64_t)s_dump_rate * s_dump_channels * 2u)
        sr_audio_dump_patch_header();
}

static void sr_audio_dump_start(SDL_AudioDeviceID dev) {
    const char *path = getenv("SR_AUDIODUMP");
    if (!path || !*path || s_dump_file || !dev) return;
    SDL_AudioSpec spec;
    if (!SDL_GetAudioDeviceFormat(dev, &spec, NULL) || spec.channels <= 0 || spec.freq <= 0) {
        fprintf(stderr, "AUDIODUMP: device format unavailable (%s); no dump written\n", SDL_GetError());
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "AUDIODUMP: cannot open %s; no dump written\n", path);
        return;
    }
    snprintf(s_dump_path, sizeof(s_dump_path), "%s", path);
    s_dump_file = f;
    s_dump_rate = (uint32_t)spec.freq;
    s_dump_channels = (uint32_t)spec.channels;
    s_dump_data_bytes = s_dump_last_patch = 0;
    s_dump_write_failed = 0;
    memset(&s_dump_stats, 0, sizeof(s_dump_stats));
    sr_audio_dump_patch_header();
    if (s_dump_write_failed) {
        fclose(s_dump_file);
        s_dump_file = NULL;
        return;
    }
    if (!SDL_SetAudioPostmixCallback(dev, sr_audio_dump_postmix, NULL)) {
        fprintf(stderr, "AUDIODUMP: postmix hook refused (%s); no dump written\n", SDL_GetError());
        fclose(s_dump_file);
        s_dump_file = NULL;
        return;
    }
    fprintf(stderr, "AUDIODUMP: writing device mix to %s (%uHz, %u ch, s16le)\n",
            s_dump_path, s_dump_rate, s_dump_channels);
}

static void sr_audio_dump_stop(SDL_AudioDeviceID dev) {
    if (!s_dump_file) return;
    if (dev) SDL_SetAudioPostmixCallback(dev, NULL, NULL);
    sr_audio_dump_patch_header();
    fclose(s_dump_file);
    s_dump_file = NULL;
    fprintf(stderr, "AUDIODUMP: %s rate=%u channels=%u frames=%llu seconds=%.3f silent_frames=%llu "
                    "clipped_samples=%llu peak=%u\n",
            s_dump_path, s_dump_rate, s_dump_channels, (unsigned long long)s_dump_stats.frames,
            s_dump_rate ? (double)s_dump_stats.frames / (double)s_dump_rate : 0.0,
            (unsigned long long)s_dump_stats.silent_frames, (unsigned long long)s_dump_stats.clipped,
            (unsigned)s_dump_stats.peak);
    if (s_dump_write_failed) {
        fprintf(stderr, "AUDIODUMP: the dump ended early on a failed write; the header covers %llu data bytes\n",
                (unsigned long long)s_dump_last_patch);
    }
    fflush(stderr);
}

/* The SR_AUDIODUMP close for the end-of-run capture point, which exits with _Exit and so skips
 * atexit. It prints no stats line and never reads SR_AUDIOSTAT; once the dump is closed it is a
 * no-op. Without SR_AUDIODUMP no dump is open, so it does nothing. */
void sr_audio_dump_finish(void) {
    sr_audio_dump_stop(s_device_id);
}

static void sr_audio_cleanup(void) {
    if (s_audio_state == AUDIO_STATE_ACTIVE) {
        sr_audio_dump_stop(s_device_id);
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
    sr_audio_dump_start(s_device_id);
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
    /* The dump is closed after the stats line, so the WAV header is finalised. The hle.c _Exit path
     * reaches this only with SR_AUDIOSTAT set and always calls sr_audio_dump_finish, so whichever
     * runs second is a no-op. A no-op without SR_AUDIODUMP. */
    sr_audio_dump_stop(s_device_id);
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

    /* The dummy driver drains bound streams in real time on its own thread, so exact
     * queue depths are only observable while the device is paused; otherwise a loaded
     * host can consume frames between a push and the assertion that counts them. */
    assert(SDL_PauseAudioDevice(s_device_id));

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

    assert(SDL_ResumeAudioDevice(s_device_id));

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

static void set_test_env(const char *key, const char *value) {
#if defined(_WIN32) || defined(_WIN64)
    char buf[600];
    snprintf(buf, sizeof(buf), "%s=%s", key, value);
    _putenv(buf);
#else
    if (value && *value) setenv(key, value, 1);
    else unsetenv(key);
#endif
}

static uint32_t test_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t test_le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* SR_AUDIODUMP conversion: the 16-bit mapping, the clip count and NaN handling, pure. */
static void test_dump_conversion(void) {
    uint64_t clipped = 0;
    assert(sr_audio_dump_sample(0.0f, &clipped) == 0);
    assert(sr_audio_dump_sample(0.5f, &clipped) == 16384);
    assert(sr_audio_dump_sample(-0.5f, &clipped) == -16384);
    assert(sr_audio_dump_sample(0.25f / 32768.0f, &clipped) == 0);        /* rounds to nearest */
    assert(sr_audio_dump_sample(0.75f / 32768.0f, &clipped) == 1);
    assert(clipped == 0);
    assert(sr_audio_dump_sample(1.0f, &clipped) == 32767);                /* 32768 is out of range */
    assert(clipped == 1);
    assert(sr_audio_dump_sample(-1.0f, &clipped) == -32768);              /* exactly representable */
    assert(clipped == 1);
    assert(sr_audio_dump_sample(1.5f, &clipped) == 32767);
    assert(sr_audio_dump_sample(-2.0f, &clipped) == -32768);
    assert(clipped == 3);
    uint32_t nan_bits = 0x7fc00000u;
    float nan_value;
    memcpy(&nan_value, &nan_bits, sizeof(nan_value));
    assert(sr_audio_dump_sample(nan_value, &clipped) == 0);
    assert(clipped == 3);
    printf("test_dump_conversion: PASS\n");
}

/* SR_AUDIODUMP header: the canonical 44-byte PCM layout the Python audit reads. */
static void test_dump_wav_header(void) {
    uint8_t h[44];
    sr_audio_wav_header(h, 44100u, 2u, 1000u);
    assert(memcmp(h + 0, "RIFF", 4) == 0);
    assert(memcmp(h + 8, "WAVE", 4) == 0);
    assert(memcmp(h + 12, "fmt ", 4) == 0);
    assert(memcmp(h + 36, "data", 4) == 0);
    assert(test_le32(h + 4) == 1036u);       /* 36 + data */
    assert(test_le32(h + 16) == 16u);
    assert(test_le16(h + 20) == 1u);         /* PCM */
    assert(test_le16(h + 22) == 2u);
    assert(test_le32(h + 24) == 44100u);
    assert(test_le32(h + 28) == 176400u);    /* 44100 * 2 * 2 */
    assert(test_le16(h + 32) == 4u);
    assert(test_le16(h + 34) == 16u);
    assert(test_le32(h + 40) == 1000u);
    printf("test_dump_wav_header: PASS\n");
}

/* SR_AUDIODUMP is off by default: an unset variable writes nothing and opens no file. */
static void test_dump_off_by_default(void) {
    test_reset();
    set_test_env("SR_AUDIODUMP", "");
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0);
    assert(s_dump_file == NULL);
    test_reset();
    printf("test_dump_off_by_default: PASS\n");
}

/* With SR_AUDIODUMP set, the device mix is written as a valid WAV at the device rate, the
 * header matches the data, and a pushed tone appears in it. */
static void test_dump_writes_wav(void) {
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/sr_audio_dump_selftest.wav", dir);
    remove(path);
    set_test_env("SR_AUDIODUMP", path);
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0);
    assert(s_dump_file != NULL);

    int16_t tone[2048 * 2];
    for (int i = 0; i < 2048; i++) {
        int16_t sample = (int16_t)(sin(2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * 8000.0);
        tone[i * 2] = sample;
        tone[i * 2 + 1] = sample;
    }
    sr_audio_push(0, tone, 2048, 0x8000, 0x8000);

    Uint64 deadline = SDL_GetTicks() + 5000;
    while ((sr_audio_queued(0) != 0 || s_dump_stats.frames < 4096) && SDL_GetTicks() < deadline)
        SDL_Delay(5);
    test_reset();   /* sr_audio_cleanup stops the dump and patches the header */

    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t h[44];
    assert(size >= 44 && fread(h, 1, sizeof(h), f) == sizeof(h));
    assert(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 36, "data", 4) == 0);
    assert(test_le32(h + 4) == (uint32_t)size - 8u);
    assert(test_le16(h + 22) == 2u);
    assert(test_le32(h + 24) == 44100u);
    uint32_t data_bytes = test_le32(h + 40);
    assert(data_bytes == (uint32_t)size - 44u);
    assert(data_bytes >= 4096u * 4u);
    int nonzero = 0;
    int16_t s;
    while (fread(&s, sizeof(s), 1, f) == 1)
        if (s != 0) nonzero++;
    fclose(f);
    assert(nonzero > 0);
    remove(path);
    set_test_env("SR_AUDIODUMP", "");
    printf("test_dump_writes_wav: PASS\n");
}

/* The _Exit path closes the dump through sr_audio_dump_finish and never calls the stats gate. The
 * finaliser does not read SR_AUDIOSTAT, so this holds whatever the host environment sets. The
 * close must finalise the header to every byte written, and a second close must change nothing. */
static void test_dump_finish_without_stats(void) {
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/sr_audio_dump_finish_selftest.wav", dir);
    remove(path);
    set_test_env("SR_AUDIODUMP", path);
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0);
    assert(s_dump_file != NULL);

    int16_t tone[2048 * 2];
    for (int i = 0; i < 2048; i++) {
        int16_t sample = (int16_t)(sin(2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * 8000.0);
        tone[i * 2] = sample;
        tone[i * 2 + 1] = sample;
    }
    sr_audio_push(0, tone, 2048, 0x8000, 0x8000);

    Uint64 deadline = SDL_GetTicks() + 5000;
    while ((sr_audio_queued(0) != 0 || s_dump_stats.frames < 4096) && SDL_GetTicks() < deadline)
        SDL_Delay(5);
    assert(s_dump_stats.frames >= 4096);

    /* The postmix hook is removed inside the close, so the byte count is stable after it. The
     * checks below hold whether or not a once-a-second header patch ran before the close. */
    sr_audio_dump_finish();
    assert(s_dump_file == NULL);
    uint64_t written = s_dump_data_bytes;
    assert(written >= 4096u * 4u);

    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t h[44];
    assert(size >= 44 && fread(h, 1, sizeof(h), f) == sizeof(h));
    fclose(f);
    assert(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 36, "data", 4) == 0);
    assert((uint64_t)size == 44u + written);
    assert(test_le32(h + 4) == (uint32_t)size - 8u);
    assert(test_le32(h + 40) == (uint32_t)written);
    assert(test_le32(h + 40) == (uint32_t)size - 44u);

    sr_audio_dump_finish();   /* a second close is a no-op */
    uint8_t again[44];
    f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    assert(ftell(f) == size);
    fseek(f, 0, SEEK_SET);
    assert(fread(again, 1, sizeof(again), f) == sizeof(again));
    fclose(f);
    assert(memcmp(again, h, sizeof(h)) == 0);

    test_reset();
    remove(path);
    set_test_env("SR_AUDIODUMP", "");
    printf("test_dump_finish_without_stats: PASS\n");
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
    test_dump_conversion();
    test_dump_wav_header();
    test_dump_off_by_default();
    test_dump_writes_wav();
    test_dump_finish_without_stats();
    test_reset();
    printf("ALL AUDIO HOST TESTS PASSED\n");
    return 0;
}
#endif
