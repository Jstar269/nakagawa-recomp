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
    uint64_t pause_enters;
    uint64_t pause_held_frames;
    uint64_t pause_stash_failures;
    uint32_t peak_queued_frames[SR_AUDIO_CHANNELS];
} AudioStats;

typedef struct {
    int16_t *samples;
    int frames;
} AudioPauseStash;

typedef struct {
    int16_t *samples;
    uint32_t capacity_frames;
    uint32_t head_frame;
    uint32_t frames;
    int valid;
} AudioShadowQueue;

#define SR_AUDIO_PAUSE_FADE_FRAMES 256

static AudioBackendState s_audio_state = AUDIO_STATE_UNINITIALIZED;
static SDL_AudioDeviceID s_device_id = 0;
static SDL_AudioStream *s_streams[SR_AUDIO_CHANNELS] = {NULL};
static AudioStats s_stats;
static AudioPauseStash s_pause_stash[SR_AUDIO_CHANNELS];
static AudioShadowQueue s_shadow_queues[SR_AUDIO_CHANNELS];
static int16_t s_pause_fade[SR_AUDIO_CHANNELS][SR_AUDIO_PAUSE_FADE_FRAMES * 2];
static int s_pause_active;
static int s_pause_device_fallback;
static int s_pause_device_was_paused;
static int s_pause_failure_reported;
static int s_master_volume_percent = 100;
static int s_reported_unavailable = 0;
static int s_reported_init = 0;
static int s_cleanup_registered = 0;

#ifdef SR_AUDIO_SELFTEST
static int s_test_fail_pause_stash_alloc;
static int s_test_pause_failure_messages;
#endif

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
    uint64_t dropped_frames; /* frames offered and not written: size limit, format change, failed write */
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

/* A block refused whole, past the size limit or in a format the header does not describe, is
 * counted in dropped_frames. The first refusal prints one line; the close line has the total. */
static void sr_audio_dump_refuse(uint64_t frames, const char *why) {
    if (!s_dump_stats.dropped_frames && frames) {
        fprintf(stderr, "AUDIODUMP: %s %s; device blocks from here on are dropped and counted\n",
                s_dump_path, why);
    }
    s_dump_stats.dropped_frames += frames;
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
    size_t samples = (size_t)buflen / sizeof(float);
    /* A device whose format changed mid-run keeps the header it was opened with. */
    if ((uint32_t)spec->channels != s_dump_channels || (uint32_t)spec->freq != s_dump_rate) {
        if (spec->channels > 0) sr_audio_dump_refuse(samples / (size_t)spec->channels, "changed format");
        return;
    }
    samples -= samples % s_dump_channels;
    if (!samples) return;
    if (s_dump_write_failed) {
        s_dump_stats.dropped_frames += samples / s_dump_channels;
        return;
    }
    if (s_dump_data_bytes + samples * 2u > SR_AUDIO_DUMP_MAX_DATA) {
        sr_audio_dump_refuse(samples / s_dump_channels, "reached its size limit");
        return;
    }
    /* A failed write stops inside the block: the frames it did not reach are dropped with the rest. */
    size_t written = sr_audio_dump_block(buffer, samples);
    s_dump_data_bytes += written * 2u;
    s_dump_stats.dropped_frames += (samples - written) / s_dump_channels;
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
                    "clipped_samples=%llu peak=%u dropped_frames=%llu\n",
            s_dump_path, s_dump_rate, s_dump_channels, (unsigned long long)s_dump_stats.frames,
            s_dump_rate ? (double)s_dump_stats.frames / (double)s_dump_rate : 0.0,
            (unsigned long long)s_dump_stats.silent_frames, (unsigned long long)s_dump_stats.clipped,
            (unsigned)s_dump_stats.peak, (unsigned long long)s_dump_stats.dropped_frames);
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
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        free(s_pause_stash[i].samples);
        s_pause_stash[i].samples = NULL;
        s_pause_stash[i].frames = 0;
        free(s_shadow_queues[i].samples);
        memset(&s_shadow_queues[i], 0, sizeof(s_shadow_queues[i]));
    }
    s_pause_active = 0;
    s_pause_device_fallback = 0;
    s_pause_device_was_paused = 0;
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

static int sr_audio_apply_master_volume(int channel_volume) {
    if (s_master_volume_percent >= 100) return channel_volume;
    if (s_master_volume_percent <= 0) return 0;
    return (int)(((int64_t)channel_volume * s_master_volume_percent) / 100);
}

static int sr_audio_shadow_reserve(AudioShadowQueue *queue, uint32_t needed_frames) {
    if (!queue || !queue->valid || needed_frames > SR_AUDIO_MAX_QUEUED_FRAMES) return 0;
    if (needed_frames <= queue->capacity_frames) return 1;
    uint32_t capacity = queue->capacity_frames ? queue->capacity_frames : 1024u;
    while (capacity < needed_frames) {
        if (capacity > SR_AUDIO_MAX_QUEUED_FRAMES / 2u) {
            capacity = SR_AUDIO_MAX_QUEUED_FRAMES;
            break;
        }
        capacity *= 2u;
    }
    int16_t *samples = (int16_t *)malloc((size_t)capacity * 4u);
    if (!samples) return 0;
    for (uint32_t i = 0; i < queue->frames; i++) {
        const uint32_t src = (queue->head_frame + i) % queue->capacity_frames;
        samples[i * 2u] = queue->samples[src * 2u];
        samples[i * 2u + 1u] = queue->samples[src * 2u + 1u];
    }
    free(queue->samples);
    queue->samples = samples;
    queue->capacity_frames = capacity;
    queue->head_frame = 0;
    return 1;
}

static void sr_audio_shadow_sync(int ch, uint32_t queued_frames) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || s_pause_active) return;
    AudioShadowQueue *queue = &s_shadow_queues[ch];
    if (!queued_frames) {
        queue->head_frame = 0;
        queue->frames = 0;
        queue->valid = 1;
        return;
    }
    if (!queue->valid || queued_frames > SR_AUDIO_MAX_QUEUED_FRAMES) {
        queue->valid = 0;
        return;
    }
    if (queue->frames < queued_frames) {
        queue->valid = 0;
        return;
    }
    const uint32_t consumed = queue->frames - queued_frames;
    if (consumed && queue->capacity_frames)
        queue->head_frame = (queue->head_frame + consumed) % queue->capacity_frames;
    queue->frames = queued_frames;
}

static int sr_audio_shadow_append(int ch, const int16_t *samples, int frames) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || !samples || frames < 0) return 0;
    AudioShadowQueue *queue = &s_shadow_queues[ch];
    if (!queue->valid) return 0;
    if ((uint32_t)frames > SR_AUDIO_MAX_QUEUED_FRAMES - queue->frames ||
        !sr_audio_shadow_reserve(queue, queue->frames + (uint32_t)frames)) {
        queue->valid = 0;
        return 0;
    }
    if (!frames) return 1;
    const uint32_t tail = (queue->head_frame + queue->frames) % queue->capacity_frames;
    uint32_t first = queue->capacity_frames - tail;
    if (first > (uint32_t)frames) first = (uint32_t)frames;
    memcpy(queue->samples + tail * 2u, samples, (size_t)first * 4u);
    if ((uint32_t)frames > first)
        memcpy(queue->samples, samples + first * 2u, (size_t)((uint32_t)frames - first) * 4u);
    queue->frames += (uint32_t)frames;
    return 1;
}

static int sr_audio_shadow_copy(int ch, int16_t *out, uint32_t frames) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || !out) return 0;
    const AudioShadowQueue *queue = &s_shadow_queues[ch];
    if (!queue->valid || queue->frames != frames) return 0;
    if (!frames) return 1;
    uint32_t first = queue->capacity_frames - queue->head_frame;
    if (first > frames) first = frames;
    memcpy(out, queue->samples + queue->head_frame * 2u, (size_t)first * 4u);
    if (frames > first)
        memcpy(out + first * 2u, queue->samples, (size_t)(frames - first) * 4u);
    return 1;
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
        s_shadow_queues[i].valid = 1;
    }

    /* Apply master volume in the integer push path. SDL3 applies stream gain
     * when data leaves a stream, which would also scale the temporary pause
     * stash when it is read back. The stream gain remains at its default 1.0. */
    {
        const char *mv = getenv("SR_MASTER_VOLUME");
        const int requested = (mv && mv[0]) ? atoi(mv) : 100;
        const float gain = sr_audio_master_gain(requested);
        s_master_volume_percent = (int)(gain * 100.0f + 0.5f);
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

    volL = sr_audio_apply_master_volume(volL);
    volR = sr_audio_apply_master_volume(volR);

    int queued_bytes = SDL_GetAudioStreamQueued(s_streams[ch]);
    if (queued_bytes < 0) {
        return;
    }
    int cur_queued_frames = queued_bytes / 4;
    sr_audio_shadow_sync(ch, (uint32_t)cur_queued_frames);

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

    const int16_t *submitted = lr;
    int push_ok;
    /* Volume scaling and submission */
    if (volL == 0x8000 && volR == 0x8000) {
        push_ok = SDL_PutAudioStreamData(s_streams[ch], lr, nframes * 4);
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
        submitted = buf;
        push_ok = SDL_PutAudioStreamData(s_streams[ch], buf, nframes * 4);
        if (push_ok) (void)sr_audio_shadow_append(ch, submitted, nframes);
        if (buf != stack_buf) {
            free(buf);
        }
    }
    if (!push_ok) {
        fprintf(stderr, "audio: SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
        return;
    }
    if (volL == 0x8000 && volR == 0x8000)
        (void)sr_audio_shadow_append(ch, submitted, nframes);
    s_stats.total_pushed_frames += (uint64_t)nframes;
    s_stats.channel_pushed_frames[ch] += (uint64_t)nframes;

    const int new_queue_bytes = SDL_GetAudioStreamQueued(s_streams[ch]);
    if (new_queue_bytes >= 0) {
        const int new_q = new_queue_bytes / 4;
        sr_audio_shadow_sync(ch, (uint32_t)new_q);
        if (new_q > (int)s_stats.peak_queued_frames[ch])
            s_stats.peak_queued_frames[ch] = (uint32_t)new_q;
    }
}

static void sr_audio_pause_unlock(const int locked[SR_AUDIO_CHANNELS]) {
    for (int i = SR_AUDIO_CHANNELS - 1; i >= 0; i--)
        if (locked[i]) SDL_UnlockAudioStream(s_streams[i]);
}

static void sr_audio_pause_free_stashes(void) {
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        free(s_pause_stash[i].samples);
        s_pause_stash[i].samples = NULL;
        s_pause_stash[i].frames = 0;
    }
}

static void *sr_audio_pause_alloc(size_t bytes) {
#ifdef SR_AUDIO_SELFTEST
    if (s_test_fail_pause_stash_alloc) {
        s_test_fail_pause_stash_alloc = 0;
        return NULL;
    }
#endif
    return malloc(bytes);
}

static void sr_audio_pause_failure_message(int failure_kind) {
    if (failure_kind == 1) {
        if (s_pause_failure_reported) return;
        s_pause_failure_reported = 1;
        const int written = fprintf(stderr,
            "AUDIO_PAUSE: stash allocation failed; device paused without fade\n");
#ifdef SR_AUDIO_SELFTEST
        if (written > 0) s_test_pause_failure_messages++;
#else
        (void)written;
#endif
    } else if (failure_kind == 2) {
        fprintf(stderr, "AUDIO_PAUSE: queued audio mirror unavailable; device paused without fade\n");
    } else {
        fprintf(stderr, "AUDIO_PAUSE: stream operation failed; device paused without fade (%s)\n",
                SDL_GetError());
    }
    fflush(stderr);
}

static void sr_audio_pause_fallback(uint64_t queued_frames, int failure_kind) {
    s_stats.pause_stash_failures++;
    s_stats.pause_held_frames += queued_frames;
    sr_audio_pause_failure_message(failure_kind);
    s_pause_device_was_paused = SDL_AudioDevicePaused(s_device_id);
    if (!s_pause_device_was_paused && !SDL_PauseAudioDevice(s_device_id)) {
        fprintf(stderr, "AUDIO_PAUSE: could not pause the device for fallback (%s)\n", SDL_GetError());
        fflush(stderr);
    }
    s_pause_device_fallback = 1;
    s_pause_active = 1;
    s_stats.pause_enters++;
}

static void sr_audio_pause_restore_unfaded(const int locked[SR_AUDIO_CHANNELS]) {
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (!locked[i] || !s_pause_stash[i].frames) continue;
        SDL_ClearAudioStream(s_streams[i]);
        if (!SDL_PutAudioStreamData(s_streams[i], s_pause_stash[i].samples,
                                    s_pause_stash[i].frames * 4)) {
            fprintf(stderr, "AUDIO_PAUSE: could not restore channel %d after stream failure (%s)\n",
                    i, SDL_GetError());
            fflush(stderr);
        }
    }
}

static void sr_audio_pause_scale_prefix(const int16_t *source, int16_t *destination,
                                        int frames, int fade_in) {
    for (int frame = 0; frame < frames; frame++) {
        int numerator, denominator;
        if (frames <= 1) {
            numerator = fade_in ? 1 : 0;
            denominator = 1;
        } else {
            denominator = frames - 1;
            numerator = fade_in ? frame : (frames - 1 - frame);
        }
        for (int channel = 0; channel < 2; channel++) {
            destination[frame * 2 + channel] = (int16_t)(
                ((int32_t)source[frame * 2 + channel] * numerator) / denominator);
        }
    }
}

void sr_audio_pause_enter(void) {
    if (s_audio_state != AUDIO_STATE_ACTIVE || s_pause_active) return;

    int locked[SR_AUDIO_CHANNELS] = {0};
    int queued[SR_AUDIO_CHANNELS] = {0};
    uint64_t queued_frames = 0;
    int failure = 0;
    int failure_kind = 0;

    /* Lock every stream before reading any depth, so SDL's device thread cannot
     * consume a later channel while an earlier channel is being stashed. */
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (!s_streams[i] || !SDL_LockAudioStream(s_streams[i])) {
            failure = 1;
            break;
        }
        locked[i] = 1;
    }
    if (failure) {
        sr_audio_pause_unlock(locked);
        sr_audio_pause_fallback(0, 3);
        return;
    }

    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        const int bytes = SDL_GetAudioStreamQueued(s_streams[i]);
        if (bytes < 0 || (bytes & 3) != 0) {
            failure = 1;
            break;
        }
        queued[i] = bytes / 4;
        queued_frames += (uint64_t)queued[i];
        sr_audio_shadow_sync(i, (uint32_t)queued[i]);
        if (!s_shadow_queues[i].valid || s_shadow_queues[i].frames != (uint32_t)queued[i]) {
            failure = 1;
            failure_kind = 2;
            break;
        }
        if (queued[i] > 0) {
            s_pause_stash[i].samples = (int16_t *)sr_audio_pause_alloc((size_t)bytes);
            s_pause_stash[i].frames = queued[i];
            if (!s_pause_stash[i].samples) {
                failure = 1;
                failure_kind = 1;
                break;
            }
            if (!sr_audio_shadow_copy(i, s_pause_stash[i].samples, (uint32_t)queued[i])) {
                failure = 1;
                failure_kind = 2;
                break;
            }
        }
    }
    if (failure) {
        sr_audio_pause_free_stashes();
        sr_audio_pause_unlock(locked);
        sr_audio_pause_fallback(queued_frames, failure_kind);
        return;
    }

    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (!SDL_ClearAudioStream(s_streams[i])) { failure = 1; break; }
    }
    if (!failure) {
        for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
            const int frames = queued[i] < SR_AUDIO_PAUSE_FADE_FRAMES
                ? queued[i] : SR_AUDIO_PAUSE_FADE_FRAMES;
            if (!frames) continue;
            sr_audio_pause_scale_prefix(s_pause_stash[i].samples, s_pause_fade[i], frames, 0);
            if (!SDL_PutAudioStreamData(s_streams[i], s_pause_fade[i], frames * 4)) {
                failure = 1;
                break;
            }
        }
    }

    if (failure) sr_audio_pause_restore_unfaded(locked);
    sr_audio_pause_unlock(locked);
    if (failure) {
        sr_audio_pause_free_stashes();
        sr_audio_pause_fallback(queued_frames, 3);
        return;
    }

    s_pause_active = 1;
    s_pause_device_fallback = 0;
    s_stats.pause_enters++;
    s_stats.pause_held_frames += queued_frames;
}

void sr_audio_pause_leave(void) {
    if (s_audio_state != AUDIO_STATE_ACTIVE || !s_pause_active) return;
    if (s_pause_device_fallback) {
        if (!s_pause_device_was_paused && !SDL_ResumeAudioDevice(s_device_id)) {
            fprintf(stderr, "AUDIO_PAUSE: could not resume fallback device (%s)\n", SDL_GetError());
            fflush(stderr);
        }
        s_pause_device_fallback = 0;
        s_pause_active = 0;
        return;
    }

    int locked[SR_AUDIO_CHANNELS] = {0};
    int failure = 0;
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (!s_streams[i] || !SDL_LockAudioStream(s_streams[i])) {
            failure = 1;
            break;
        }
        locked[i] = 1;
    }
    if (failure) {
        sr_audio_pause_unlock(locked);
        fprintf(stderr, "AUDIO_PAUSE: could not lock streams while restoring audio (%s)\n", SDL_GetError());
        fflush(stderr);
        sr_audio_pause_free_stashes();
        s_pause_active = 0;
        return;
    }

    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        const int frames = s_pause_stash[i].frames;
        const int fade_frames = frames < SR_AUDIO_PAUSE_FADE_FRAMES
            ? frames : SR_AUDIO_PAUSE_FADE_FRAMES;
        if (!SDL_ClearAudioStream(s_streams[i])) {
            failure = 1;
            break;
        }
        if (!frames) continue;
        sr_audio_pause_scale_prefix(s_pause_stash[i].samples, s_pause_fade[i], fade_frames, 1);
        if (!SDL_PutAudioStreamData(s_streams[i], s_pause_fade[i], fade_frames * 4) ||
            (frames > fade_frames && !SDL_PutAudioStreamData(s_streams[i],
                s_pause_stash[i].samples + fade_frames * 2, (frames - fade_frames) * 4))) {
            failure = 1;
            SDL_ClearAudioStream(s_streams[i]);
            if (!SDL_PutAudioStreamData(s_streams[i], s_pause_stash[i].samples, frames * 4))
                fprintf(stderr, "AUDIO_PAUSE: could not restore channel %d after fade failure (%s)\n",
                        i, SDL_GetError());
            break;
        }
    }
    sr_audio_pause_unlock(locked);
    if (failure) {
        fprintf(stderr, "AUDIO_PAUSE: fade-in restore failed (%s)\n", SDL_GetError());
        fflush(stderr);
    }
    sr_audio_pause_free_stashes();
    s_pause_active = 0;
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
    const int frames = bytes / 4;
    sr_audio_shadow_sync(ch, (uint32_t)frames);
    return frames;
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
                    "backpressure=%llu pause_enters=%llu pause_held_frames=%llu pause_stash_failures=%llu "
                    "peak_q=%u queued=%d lead_ms=%ld\n",
            state_str,
            driver ? driver : "none",
            (unsigned long long)s_stats.total_pushed_frames,
            (unsigned long long)s_stats.underrun_events,
            (unsigned long long)s_stats.overrun_events,
            (unsigned long long)s_stats.backpressure_events,
            (unsigned long long)s_stats.pause_enters,
            (unsigned long long)s_stats.pause_held_frames,
            (unsigned long long)s_stats.pause_stash_failures,
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

int sr_audio_pause_script_ready(void) {
    if (s_audio_state == AUDIO_STATE_UNINITIALIZED) return 0;
    if (s_audio_state != AUDIO_STATE_ACTIVE) return 1;
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        const int queued = SDL_GetAudioStreamQueued(s_streams[i]);
        if (queued > 0) return 1;
    }
    return 0;
}

#ifdef SR_AUDIO_SELFTEST
#include <assert.h>
#include <math.h>

static void set_test_env(const char *key, const char *value);

static void test_reset(void) {
    sr_audio_cleanup();
    s_audio_state = AUDIO_STATE_UNINITIALIZED;
    s_reported_unavailable = 0;
    s_reported_init = 0;
    s_pause_failure_reported = 0;
    s_pause_device_fallback = 0;
    s_pause_device_was_paused = 0;
    s_pause_active = 0;
    s_master_volume_percent = 100;
    s_test_fail_pause_stash_alloc = 0;
    s_test_pause_failure_messages = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    set_test_env("SR_MASTER_VOLUME", "");
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
    sr_audio_pause_enter();
    sr_audio_pause_leave();
    assert(!s_pause_active);

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
    sr_audio_pause_enter();
    sr_audio_pause_leave();
    assert(!s_pause_active);

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

/* A block the dump refuses is counted in dropped_frames and never written: past the size limit,
 * in a format the header does not describe, and after a failed write. The writer is driven
 * directly through the postmix callback SDL calls, with no device, so the counts are exact. */
static void test_dump_counts_refused_blocks(void) {
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/sr_audio_dump_drop_selftest.wav", dir);
    remove(path);
    test_reset();
    snprintf(s_dump_path, sizeof(s_dump_path), "%s", path);
    s_dump_file = fopen(path, "wb");
    assert(s_dump_file != NULL);
    s_dump_rate = 44100;
    s_dump_channels = 2;
    s_dump_data_bytes = s_dump_last_patch = 0;
    s_dump_write_failed = 0;
    memset(&s_dump_stats, 0, sizeof(s_dump_stats));
    sr_audio_dump_patch_header();

    SDL_AudioSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = 44100;
    float block[512 * 2];   /* 512 stereo frames: 2048 bytes once written as s16 */
    for (int i = 0; i < 512 * 2; i++) block[i] = 0.25f;
    const int blen = (int)sizeof(block);

    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_stats.frames == 512);
    assert(s_dump_stats.dropped_frames == 0);

    /* The data is at the limit, so the next block would pass it: each whole block is refused. */
    s_dump_data_bytes = SR_AUDIO_DUMP_MAX_DATA - 1024u;
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_stats.dropped_frames == 1024);
    assert(s_dump_data_bytes == SR_AUDIO_DUMP_MAX_DATA - 1024u);
    assert(s_dump_stats.frames == 512);

    /* A device that changed format is refused too: 1024 mono frames in this buffer. */
    spec.channels = 1;
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_stats.dropped_frames == 2048);
    spec.channels = 2;

    /* Reopen read-only so the next write fails for real: that block and every later one is
     * counted, with no per-block message. */
    fclose(s_dump_file);
    s_dump_file = fopen(path, "rb");
    assert(s_dump_file != NULL);
    s_dump_data_bytes = 2048u;
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_write_failed == 1);
    assert(s_dump_stats.dropped_frames == 2560);
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_stats.dropped_frames == 3072);
    assert(s_dump_stats.frames == 512);

    sr_audio_dump_finish();
    assert(s_dump_file == NULL);
    assert(s_dump_stats.dropped_frames == 3072);
    remove(path);
    printf("test_dump_counts_refused_blocks: PASS\n");
}

static void test_pause_stream_readback(const int16_t *input, int frames, int16_t *output) {
    SDL_AudioSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq = SR_AUDIO_SAMPLE_RATE;
    SDL_AudioStream *stream = SDL_CreateAudioStream(&spec, &spec);
    assert(stream != NULL);
    assert(SDL_SetAudioStreamGain(stream, s_streams[0]
        ? SDL_GetAudioStreamGain(s_streams[0]) : 1.0f));
    assert(SDL_PutAudioStreamData(stream, input, frames * 4));
    assert(SDL_GetAudioStreamData(stream, output, frames * 4) == frames * 4);
    SDL_DestroyAudioStream(stream);
}

static void test_pause_queue_hold_and_fades(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));

    int16_t dc0[4096 * 2];
    int16_t dc8[300 * 2];
    for (int i = 0; i < 4096 * 2; i++) dc0[i] = 16000;
    for (int i = 0; i < 300 * 2; i++) dc8[i] = 16000;
    sr_audio_push(0, dc0, 4096, 0x8000, 0x8000);
    sr_audio_push(8, dc8, 300, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == 4096 && sr_audio_queued(8) == 300);

    sr_audio_pause_enter();
    assert(s_pause_active && !s_pause_device_fallback);
    const int held0 = sr_audio_queued(0);
    const int held8 = sr_audio_queued(8);
    if (held0 != SR_AUDIO_PAUSE_FADE_FRAMES || held8 != SR_AUDIO_PAUSE_FADE_FRAMES)
        fprintf(stderr, "AUDIO_PAUSE_TEST: held queue depths ch0=%d ch8=%d\n", held0, held8);
    assert(held0 == SR_AUDIO_PAUSE_FADE_FRAMES);
    assert(held8 == SR_AUDIO_PAUSE_FADE_FRAMES);
    assert(s_stats.pause_enters == 1 && s_stats.pause_held_frames == 4396);

    int16_t fade_out[SR_AUDIO_PAUSE_FADE_FRAMES * 2];
    test_pause_stream_readback(s_pause_fade[0], SR_AUDIO_PAUSE_FADE_FRAMES, fade_out);
    for (int i = 1; i < SR_AUDIO_PAUSE_FADE_FRAMES; i++) {
        assert(fade_out[i * 2] <= fade_out[(i - 1) * 2]);
        assert(fade_out[i * 2 + 1] <= fade_out[(i - 1) * 2 + 1]);
    }
    assert(fade_out[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2] <= 63);
    assert(fade_out[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2 + 1] <= 63);

    sr_audio_pause_leave();
    assert(!s_pause_active);
    assert(sr_audio_queued(0) == 4096 && sr_audio_queued(8) == 300);
    int16_t restored0[SR_AUDIO_PAUSE_FADE_FRAMES * 2];
    int16_t restored8[SR_AUDIO_PAUSE_FADE_FRAMES * 2];
    test_pause_stream_readback(s_pause_fade[0], SR_AUDIO_PAUSE_FADE_FRAMES, restored0);
    test_pause_stream_readback(s_pause_fade[8], SR_AUDIO_PAUSE_FADE_FRAMES, restored8);
    assert(restored0[0] <= 63 && restored0[1] <= 63);
    for (int i = 1; i < SR_AUDIO_PAUSE_FADE_FRAMES; i++) {
        assert(restored0[i * 2] >= restored0[(i - 1) * 2]);
        assert(restored0[i * 2 + 1] >= restored0[(i - 1) * 2 + 1]);
    }
    assert(restored0[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2] == 16000);
    assert(restored0[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2 + 1] == 16000);
    for (int i = SR_AUDIO_PAUSE_FADE_FRAMES; i < 4096; i++) {
        const uint32_t pos = (s_shadow_queues[0].head_frame + (uint32_t)i) % s_shadow_queues[0].capacity_frames;
        assert(s_shadow_queues[0].samples[pos * 2u] == 16000 &&
               s_shadow_queues[0].samples[pos * 2u + 1u] == 16000);
    }
    assert(restored8[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2] == 16000);
    assert(restored8[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2 + 1] == 16000);
    test_reset();
    printf("test_pause_queue_hold_and_fades: PASS\n");
}

static void test_pause_master_gain_applied_once(void) {
    test_reset();
    set_test_env("SR_MASTER_VOLUME", "50");
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));
    int16_t dc[4096 * 2];
    int16_t sample[2];
    for (int i = 0; i < 4096 * 2; i++) dc[i] = 16000;
    sr_audio_push(0, dc, 4096, 0x8000, 0x8000);
    assert(s_shadow_queues[0].samples[0] == 8000 && s_shadow_queues[0].samples[1] == 8000);
    /* Read a copy through the production stream's gain setting. SDL applies
     * stream gain during reads, so an accidental second master gain changes
     * these 8000 samples to 4000. */
    test_pause_stream_readback(s_shadow_queues[0].samples, 4096, (int16_t *)dc);
    sample[0] = dc[0]; sample[1] = dc[1];
    assert(sample[0] == 8000 && sample[1] == 8000);

    sr_audio_pause_enter();
    assert(s_pause_stash[0].samples[0] == 8000 && s_pause_stash[0].samples[1] == 8000);
    sr_audio_pause_leave();
    int16_t restored[SR_AUDIO_PAUSE_FADE_FRAMES * 2];
    test_pause_stream_readback(s_pause_fade[0], SR_AUDIO_PAUSE_FADE_FRAMES, restored);
    assert(restored[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2] == 8000);
    assert(restored[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2 + 1] == 8000);
    assert(s_shadow_queues[0].samples[0] == 8000 && s_shadow_queues[0].samples[1] == 8000);
    test_reset();
    printf("test_pause_master_gain_applied_once: PASS\n");
}

static void test_pause_fade_has_no_dumped_click(void) {
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/sr_audio_pause_selftest.wav", dir);
    remove(path);

    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    int16_t *dc = (int16_t *)malloc(44100u * 2u * sizeof(int16_t));
    assert(dc != NULL);
    for (int i = 0; i < 44100 * 2; i++) dc[i] = 16000;
    sr_audio_push(0, dc, 44100, 0x8000, 0x8000);
    free(dc);
    SDL_Delay(100);  /* establish a steady DC output before starting the captured window */
    set_test_env("SR_AUDIODUMP", path);
    sr_audio_dump_start(s_device_id);
    sr_audio_pause_enter();
    SDL_Delay(200);
    sr_audio_pause_leave();
    SDL_Delay(200);  /* stop on the restored steady signal, before the queue drains */
    sr_audio_dump_stop(s_device_id);
    test_reset();
    set_test_env("SR_AUDIODUMP", "");

    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    uint8_t header[44];
    assert(fread(header, 1, sizeof(header), f) == sizeof(header));
    assert(memcmp(header, "RIFF", 4) == 0 && memcmp(header + 36, "data", 4) == 0);
    int16_t previous = 0, sample = 0;
    int have_previous = 0;
    unsigned samples = 0;
    unsigned max_step = 0;
    while (fread(&sample, sizeof(sample), 1, f) == 1) {
        if (have_previous) {
            unsigned step = (unsigned)abs((int)sample - (int)previous);
            if (step > max_step) max_step = step;
        }
        previous = sample;
        have_previous = 1;
        samples++;
    }
    fclose(f);
    remove(path);
    assert(samples > SR_AUDIO_PAUSE_FADE_FRAMES * 4u);
    assert(max_step <= 64u);
    printf("test_pause_fade_has_no_dumped_click: PASS (max_step=%u)\n", max_step);
}

static void test_pause_stash_failure_is_visible(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    int16_t dc[512 * 2];
    for (int i = 0; i < 512 * 2; i++) dc[i] = 8000;

    for (int failure = 1; failure <= 2; failure++) {
        assert(SDL_PauseAudioDevice(s_device_id));
        sr_audio_push(0, dc, 512, 0x8000, 0x8000);
        assert(SDL_ResumeAudioDevice(s_device_id));
        s_test_fail_pause_stash_alloc = 1;
        sr_audio_pause_enter();
        assert(s_pause_active && s_pause_device_fallback);
        assert(SDL_AudioDevicePaused(s_device_id));
        assert(s_stats.pause_stash_failures == (uint64_t)failure);
        assert(s_test_pause_failure_messages == 1);
        sr_audio_pause_leave();
        assert(!s_pause_active && !SDL_AudioDevicePaused(s_device_id));
    }
    test_reset();
    printf("test_pause_stash_failure_is_visible: PASS\n");
}

int main(int argc, char **argv) {
    sr_perf_init();
    (void)argc;
    (void)argv;
    printf("--- Running audio selftest ---\n");
    test_master_volume_gain();
    test_dummy_mixer_handoff();
    test_pause_queue_hold_and_fades();
    test_pause_master_gain_applied_once();
    test_pause_fade_has_no_dumped_click();
    test_no_device_path();
    test_null_backend_env();
    test_pause_stash_failure_is_visible();
    test_dump_conversion();
    test_dump_wav_header();
    test_dump_off_by_default();
    test_dump_writes_wav();
    test_dump_finish_without_stats();
    test_dump_counts_refused_blocks();
    test_reset();
    printf("ALL AUDIO HOST TESTS PASSED\n");
    return 0;
}
#endif
