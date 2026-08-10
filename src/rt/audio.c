// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)

/* *
 * The PSP mixes up to 8 sceAudio output channels in hardware; each guest channel that calls
 * sceAudioOutput*Blocking pushes its buffer here. Channels keep their own write cursor over a
 * shared int32 accumulator ring, so concurrently-playing channels overlap (sum) instead of
 * interleaving in time. A pull-model SDL3 audio-stream callback clamps accumulated samples to
 * s16 and hands them to the audio device on demand; if the game outruns real time the push
 * clamps (drops), if it falls behind the callback emits silence. This favours "keeps running,
 * sounds right enough" over hi-fi sync — the scheduler's virtual-time pacing of the *Blocking
 * calls stays the authority on game speed.
 *
 * Portability: this backend is pure SDL3 (SDL_AudioStream) with no Windows waveOut/winmm or any
 * host-specific API, so it builds and runs identically on Windows and Linux/Steam Deck. SDL owns
 * its own audio thread and invokes audio_cb() when the device needs more samples, so the push
 * side never blocks on the device.
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define RING_FRAMES  (1 << 16)            /* 64k frames ≈ 1.5 s of headroom */
#define RING_MASK    (RING_FRAMES - 1)
#define CB_CHUNK     1024                 /* frames produced per inner callback pass */

static int32_t  s_accL[RING_FRAMES], s_accR[RING_FRAMES];
static uint64_t s_play = 0;               /* device (read) cursor, frames */
/* Eight regular PSP channels plus the independent sceAudioOutput2 channel. */
static uint64_t s_chw[9];                 /* per-channel write cursors, frames */
static SDL_Mutex     *s_lock = NULL;
static SDL_AudioStream *s_stream = NULL;
static int s_inited = 0, s_ok = 0;

/* ---- SR_AUDIOSTAT: aggregate end-to-end output telemetry (investigation) ----
 * The existing SR_AUDIOLOG trace is power-of-two decayed, so it samples calls
 * 1,2,4,...,512 and can miss every nonzero push in a run. These are totals, so
 * "the host never received a nonzero sample" is a statement the evidence can
 * actually support. Counters only; no I/O on the audio thread. */
static unsigned long s_st_push_calls[9], s_st_push_frames[9], s_st_push_nonzero[9];
static int           s_st_push_peak[9];
static unsigned long s_st_cb_calls, s_st_cb_frames, s_st_cb_nonzero, s_st_cb_silent;
static int           s_st_cb_peak;
static unsigned long s_st_cb_short;            /* PutAudioStreamData failures */
/* The only two places the push side can lose guest audio: a clamp that drops the
 * tail of a buffer the ring cannot hold, and a snap that abandons a write cursor
 * the playhead has already passed. Both must read zero for "the chain is
 * lossless" to be a measurement rather than an inference. */
static unsigned long s_st_clamps[9], s_st_dropped[9], s_st_snap[9];
static unsigned long s_win_frames, s_win_nonzero;
static unsigned long s_win_dump_frames, s_win_dump_nonzero;
static int s_audiostat_on = -1;
static int audiostat_on(void) {
    if (s_audiostat_on < 0) s_audiostat_on = getenv("SR_AUDIOSTAT") != NULL;
    return s_audiostat_on;
}

static int16_t clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }
static int32_t add_sat32(int32_t a, int32_t b) {
    int64_t v = (int64_t)a + b;
    return v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : (int32_t)v;
}

/* SDL pull callback (runs on SDL's audio thread): produce `additional` bytes of interleaved
 * stereo s16 from the accumulator ring, zeroing consumed slots and advancing the playhead. */
static void SDLCALL audio_cb(void *ud, SDL_AudioStream *stream, int additional, int total) {
    (void)ud; (void)total;
    int stat = audiostat_on();
    if (stat) s_st_cb_calls++;
    if (additional <= 0) return;
    int frames = additional / 4;                       /* 4 bytes/frame (2ch * s16) */
    int16_t buf[CB_CHUNK * 2];
    while (frames > 0) {
        int chunk = frames > CB_CHUNK ? CB_CHUNK : frames;
        SDL_LockMutex(s_lock);
        for (int i = 0; i < chunk; i++) {
            uint32_t idx = (uint32_t)((s_play + (uint64_t)i) & RING_MASK);
            buf[i * 2 + 0] = clamp16(s_accL[idx]);
            buf[i * 2 + 1] = clamp16(s_accR[idx]);
            s_accL[idx] = s_accR[idx] = 0;
        }
        s_play += (uint64_t)chunk;
        if (stat) {
            unsigned long nz = 0;
            int pk = 0;
            for (int i = 0; i < chunk; i++) {
                int l = buf[i * 2 + 0], r = buf[i * 2 + 1];
                if (l | r) nz++;
                if (l < 0) l = -l;
                if (r < 0) r = -r;
                if (l > pk) pk = l;
                if (r > pk) pk = r;
            }
            /* Windowed duty cycle: totals alone cannot separate "BGM is gapped"
             * from "BGM had not started yet during boot". One line per ~5 s. */
            s_win_frames += (unsigned long)chunk;
            s_win_nonzero += nz;
            if (s_win_frames >= 44100u * 5u) {
                s_win_dump_frames = s_win_frames;
                s_win_dump_nonzero = s_win_nonzero;
                s_win_frames = s_win_nonzero = 0;
            }
            s_st_cb_frames += (unsigned long)chunk;
            s_st_cb_nonzero += nz;
            s_st_cb_silent += (unsigned long)chunk - nz;
            if (pk > s_st_cb_peak) s_st_cb_peak = pk;
        }
        SDL_UnlockMutex(s_lock);
        if (stat && s_win_dump_frames) {
            extern uint32_t sr_audio_vbl(void);
            unsigned long pushed = 0;
            for (int c = 0; c < 9; c++) pushed += s_st_push_frames[c];
            fprintf(stderr, "AUDIOSTAT_WIN: vbl=%u frames=%lu nonzero=%lu duty=%lu%% pushed_total=%lu\n",
                    sr_audio_vbl(), s_win_dump_frames, s_win_dump_nonzero,
                    s_win_dump_nonzero * 100u / s_win_dump_frames, pushed);
            s_win_dump_frames = s_win_dump_nonzero = 0;
        }
        /* Never hold the mixer lock while handing bytes back to SDL. The stream can allocate
         * or wake its device thread here, and serializing that work with eight guest producers
         * caused long priority inversions under small callback periods. */
        if (!SDL_PutAudioStreamData(stream, buf, chunk * 4) && stat) s_st_cb_short++;
        frames -= chunk;
    }
}

int sr_audio_init(void) {
    if (s_inited) return s_ok;
    s_inited = 1;
    if (getenv("SR_NOAUDIO")) return s_ok = 0;

    /* Audio is independent of the presenter's video/gamepad subsystems, so init it on its own;
     * this works whether or not the SDL3 window was created (GUI, --nogui, or GDI fallback). */
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "audio: SDL_InitSubSystem(AUDIO) failed: %s (silent run)\n", SDL_GetError());
        return s_ok = 0;
    }
    s_lock = SDL_CreateMutex();
    if (!s_lock) {
        fprintf(stderr, "audio: SDL_CreateMutex failed: %s (silent run)\n", SDL_GetError());
        return s_ok = 0;
    }
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;              /* native-endian signed 16-bit */
    spec.channels = 2;
    spec.freq = 44100;
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!s_stream) {
        fprintf(stderr, "audio: SDL_OpenAudioDeviceStream failed: %s (silent run)\n", SDL_GetError());
        SDL_DestroyMutex(s_lock); s_lock = NULL;
        return s_ok = 0;
    }
    /* Streams open paused; start the device pulling. */
    SDL_ResumeAudioStreamDevice(s_stream);
    fprintf(stderr, "audio: SDL3 audio stream 44100 Hz stereo s16 open\n");
    return s_ok = 1;
}

/* Mix nframes of interleaved stereo s16 into the channel's slice of the ring.
 * volL/volR are 0..0x8000 (PSP panned-output volumes). */
void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR) {
    if (!sr_audio_init() || nframes <= 0) return;
    if (ch < 0) ch = 0;
    if (ch > 8) ch = 8;
    int trace = getenv("SR_AUDIOLOG") != NULL;
    int stat = audiostat_on();
    int peak = 0;
    if (trace || stat) {
        for (int i = 0; i < nframes * 2; i++) {
            int v = lr[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
    }
    SDL_LockMutex(s_lock);
    /* Counters live under the mixer lock: several guest audio threads push
     * concurrently, and a diagnostic that loses increments is worse than none. */
    if (stat) {
        s_st_push_calls[ch]++;
        s_st_push_frames[ch] += (unsigned long)nframes;
        if (peak) s_st_push_nonzero[ch]++;
        if (peak > s_st_push_peak[ch]) s_st_push_peak[ch] = peak;
    }
    uint64_t w = s_chw[ch];
    if (w < s_play) { w = s_play; if (stat) s_st_snap[ch]++; }  /* channel fell behind: snap to now */
    if (w + (uint64_t)nframes > s_play + RING_FRAMES) {     /* too far ahead: clamp (drop tail) */
        if (stat) {
            s_st_dropped[ch] += (unsigned long)nframes - (unsigned long)(s_play + RING_FRAMES - w);
            s_st_clamps[ch]++;
        }
        nframes = (int)(s_play + RING_FRAMES - w);
    }
    for (int i = 0; i < nframes; i++) {
        uint32_t idx = (uint32_t)((w + (uint64_t)i) & RING_MASK);
        s_accL[idx] = add_sat32(s_accL[idx], ((int32_t)lr[i * 2 + 0] * volL) >> 15);
        s_accR[idx] = add_sat32(s_accR[idx], ((int32_t)lr[i * 2 + 1] * volR) >> 15);
    }
    s_chw[ch] = w + (uint64_t)(nframes > 0 ? nframes : 0);
    int queued = s_chw[ch] > s_play ? (int)(s_chw[ch] - s_play) : 0;
    SDL_UnlockMutex(s_lock);
    if (trace) {
        extern uint32_t sr_audio_vbl(void);
        static uint32_t pushes[9];
        uint32_t call = ++pushes[ch];
        if (call <= 16u || (call & (call - 1u)) == 0u)
            fprintf(stderr,
                    "AUDIO_PUSH: ch=%d call=%u vbl=%u frames=%d vol=%d/%d peak=%d queued=%d\n",
                    ch, call, sr_audio_vbl(), nframes, volL, volR, peak, queued);
    }
}

/* SR_AUDIOSTAT dump: end-to-end totals for one run. Called from the clean-exit paths. */
void sr_audio_dump_stats(void) {
    if (!audiostat_on()) return;
    fprintf(stderr, "--- audio chain totals ---\n");
    fprintf(stderr, "AUDIOSTAT_DEV: inited=%d ok=%d stream=%p\n", s_inited, s_ok, (void *)s_stream);
    for (int ch = 0; ch < 9; ch++) {
        if (!s_st_push_calls[ch]) continue;
        fprintf(stderr,
                "AUDIOSTAT_PUSH: ch=%d calls=%lu nonzero_calls=%lu frames=%lu peak=%d clamps=%lu dropped=%lu snaps=%lu\n",
                ch, s_st_push_calls[ch], s_st_push_nonzero[ch],
                s_st_push_frames[ch], s_st_push_peak[ch],
                s_st_clamps[ch], s_st_dropped[ch], s_st_snap[ch]);
    }
    fprintf(stderr,
            "AUDIOSTAT_CB: calls=%lu frames=%lu nonzero_frames=%lu silent_frames=%lu peak=%d put_fail=%lu play=%llu\n",
            s_st_cb_calls, s_st_cb_frames, s_st_cb_nonzero, s_st_cb_silent,
            s_st_cb_peak, s_st_cb_short, (unsigned long long)s_play);
    fflush(stderr);
}

/* Frames this channel has queued ahead of the playhead (-1: no host audio). The blocking
 * output calls pace against this, like real hardware, so drift self-corrects instead of
 * accumulating (an open-loop sleep of the buffer duration always runs slightly slow). */
int sr_audio_queued(int ch) {
    if (!s_ok) return -1;
    if (ch < 0) ch = 0;
    if (ch > 8) ch = 8;
    SDL_LockMutex(s_lock);
    uint64_t w = s_chw[ch], p = s_play;
    SDL_UnlockMutex(s_lock);
    return w > p ? (int)(w - p) : 0;
}
