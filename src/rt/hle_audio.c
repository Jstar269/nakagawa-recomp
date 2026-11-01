// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Audio output: sceAudio channels and the sceSas voice mixer. Extracted from hle.c on 2026-10-31
 * (#307). */

#include "hle_internal.h"

#define PSMF_AV_CONTENT_MAX 8192u            /* one sceMpegAtracDecode block */
#ifdef SR_MPEG_MEDIA_SELFTEST
int s_psmf_av_test_queue;
#endif

extern int sr_audio_queued(int ch);
int psmf_av_queue_frames(int ch) {
#ifdef SR_MPEG_MEDIA_SELFTEST
    if (s_psmf_av_test_queue_set) return s_psmf_av_test_queue;
#endif
    return sr_audio_queued(ch);
}

/* FNV-1a of [addr, +bytes), 0 when the bytes are all zero or cannot be read: a silent block is
 * not told apart from any other. */
uint64_t psmf_av_content(uint32_t addr, uint32_t bytes) {
    static uint8_t block[PSMF_AV_CONTENT_MAX];
    if (!bytes || bytes > PSMF_AV_CONTENT_MAX || !sr_guest_copy_in(block, addr, bytes)) return 0;
    uint64_t h = 1469598103934665603ull;
    unsigned any = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        any |= block[i];
        h = (h ^ block[i]) * 1099511628211ull;
    }
    return any ? (h ? h : 1u) : 0;
}

/* A submitted buffer that holds no pending block where it was written may hold one the guest
 * copied there: each block-sized piece of it meets the oldest pending block with the same
 * bytes. */
void psmf_av_audio_by_content(PsmfAvStream *st, int ch, uint32_t buf, uint32_t bytes,
                                     uint32_t bytes_per_frame, uint32_t rate_hz) {
    uint32_t size = 0;
    for (unsigned i = 0; i < PSMF_AV_PENDING && !size; i++)
        if (st->pend[i].live && st->pend[i].content) size = st->pend[i].content_bytes;
    if (!size || bytes < size) return;
    int ticked = 0, queued = -1;
    int64_t now = 0, lead_us = 0;
    for (uint32_t off = 0; bytes - off >= size; off += size) {
        uint64_t h = psmf_av_content(buf + off, size);
        if (!h) continue;
        PsmfAvEntry *best = NULL;
        for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
            PsmfAvEntry *e = &st->pend[i];
            if (e->live && e->content == h && e->content_bytes == size && (!best || e->seq < best->seq))
                best = e;
        }
        if (!best) continue;
        if (!ticked) {
            queued = psmf_av_queue_frames(ch);      /* lead ahead of this buffer, read before the push */
            now = psmf_av_now_us();
            lead_us = queued >= 0 ? (int64_t)queued * 1000000 / 44100 : 0;
            psmf_av_window_tick(now);
            ticked = 1;
        }
        int64_t play = now + lead_us + (int64_t)(off / bytes_per_frame) * 1000000 / (int64_t)rate_hz;
        st->content_matched++;
        psmf_av_match(st, best, play, queued >= 0);
    }
    if (ticked) st->boundary_hits++;
}
/* SR_AUDIOSTAT aggregate counters (investigation telemetry). Declared here because
 * the ATRAC decode path and the vblank exit dump both precede the SAS definitions. */
unsigned long g_sas_calls, g_sas_calls_add, g_sas_pre_nonzero, g_sas_post_nonzero;
unsigned long g_sas_erased;    /* nonzero on entry, silent on exit */
int g_sas_pre_peak, g_sas_post_peak;
unsigned long g_sas_no_voice, g_sas_no_voice_overwrite;
/* Buffer identities: [0]=sceAudioOutput2 submissions per channel, plus the SAS
 * output targets, so the two stages can be shown to be the same memory. */
uint32_t g_sas_bufs[4];
int      g_sas_nbufs;
static void audio_note_sas_buf(uint32_t buf) {
    if (!buf) return;
    for (int i = 0; i < g_sas_nbufs; i++) if (g_sas_bufs[i] == buf) return;
    if (g_sas_nbufs < 4) g_sas_bufs[g_sas_nbufs++] = buf;
}

/* ---- audio / control / display / GE / SAS: functional stubs ----
 * These return success and neutral data so the boot reaches and runs its main loop without an
 * actual audio device or GPU. Real rendering (sceGe display lists) and audio are later work;
 * here the calls must not block forever and must hand back valid-shaped results. */

/* sceAudio: a small channel table. The *Blocking output calls must block until the buffer is
 * consumed by the (virtual) audio hardware; that block is what lets the rest of the game run,
 * so a no-op return makes the audio thread monopolise the CPU. Here it yields the thread for
 * the buffer's duration AND forwards the samples to the host waveOut backend (audio.c). */
extern void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR);
/* Slots 0..7 are the regular hardware channels. Slot 8 models the separate
 * sceAudioOutput2 channel, which has its own reservation and queue. */
static int s_audio_ch[9], s_audio_fmt[9];   /* regular fmt: 0=stereo, 0x10=mono */
static uint32_t s_audio_len[9];
/* sceAudioSRC* shares slot 8 with Output2: the PSP-3000 refused either API's reserve with
 * 0x80268002 while the other held it, in both orders (PSP-HLE-AUDIO-SRC-001). */
static int s_audio_src_owned;
static uint32_t s_audio_src_freq;
static SrAudioResampler s_audio_src_resampler;

/* Public contract vocabulary pinned to PSPSDK src/audio/pspaudio.h at
 * 314b2083f2e1eaf145fc5de342736336fe1f0148 and PSPAutotests
 * tests/audio/sceaudio/{reserve,datalen}.{c,expected} at
 * ea71108f00933712c4662276261b39cd42249b1e. Those sources are corroborative
 * inputs; the executable checks in this tree are host evidence. */
#define SCE_AUDIO_ERROR_NOT_INITIALIZED 0x80260001u
#define SCE_AUDIO_ERROR_OUTPUT_BUSY     0x80260002u
#define SCE_AUDIO_ERROR_INVALID_CH      0x80260003u
#define SCE_AUDIO_ERROR_NOT_FOUND       0x80260005u
#define SCE_AUDIO_ERROR_INVALID_SIZE    0x80260006u
#define SCE_AUDIO_ERROR_INVALID_FORMAT  0x80260007u
#define SCE_AUDIO_ERROR_NOT_RESERVED    0x80260008u
#define SCE_AUDIO_ERROR_INVALID_VOL     0x8026000bu
/* Output2 release while samples are still queued. Measured on PSP-3001, firmware 6.6.1
 * (2026-10-01, issue #311); the raw records stay private. */
#define SCE_AUDIO_ERROR_OUTPUT2_BUSY    0x80268002u
/* sceAudioSRCChReserve argument errors, measured on PSP-3000 firmware 6.6.1 (2026-10-15,
 * PSP-HLE-AUDIO-SRC-001): a sample count of 0 and a channel count of 1 both returned
 * 0x80000104, a channel count of 4 returned 0x80000003. */
#define SCE_AUDIO_ERROR_SRC_INVALID_SIZE     0x80000104u
#define SCE_AUDIO_ERROR_SRC_INVALID_CHANNELS 0x80000003u
#define PSP_AUDIO_FORMAT_STEREO         0u
#define PSP_AUDIO_FORMAT_MONO           0x10u
#define PSP_AUDIO_SAMPLE_MIN            64u
#define PSP_AUDIO_SAMPLE_MAX            65472u

static int audio_regular_sample_count_valid(uint32_t frames) {
    return frames >= PSP_AUDIO_SAMPLE_MIN && frames <= PSP_AUDIO_SAMPLE_MAX &&
           (frames & 63u) == 0u;
}

static uint32_t h_AudioChReserve(CpuState *s) {
    int32_t ch = (int32_t)A0;
    if (ch < 0) {
        ch = -1;
        for (int32_t i = 0; i < 8; i++) {
            if (!s_audio_ch[i]) { ch = i; break; }
        }
        if (ch < 0) return SCE_AUDIO_ERROR_NOT_FOUND;
    }
    if (ch >= 8) return SCE_AUDIO_ERROR_INVALID_CH;
    if (!audio_regular_sample_count_valid(A1)) return SCE_AUDIO_ERROR_INVALID_SIZE;
    if (A2 != PSP_AUDIO_FORMAT_STEREO && A2 != PSP_AUDIO_FORMAT_MONO)
        return SCE_AUDIO_ERROR_INVALID_FORMAT;
    if (s_audio_ch[ch]) return SCE_AUDIO_ERROR_INVALID_CH;
    s_audio_ch[ch] = 1;
    s_audio_len[ch] = A1;
    s_audio_fmt[ch] = (int)A2;
    return (uint32_t)ch;
}
static uint32_t h_AudioChRelease(CpuState *s) { if (A0 < 8) s_audio_ch[A0] = 0; return 0; }
static uint32_t h_AudioSetChannelDataLen(CpuState *s) {
    if (A0 >= 8u) return SCE_AUDIO_ERROR_INVALID_CH;
    if (!s_audio_ch[A0]) return SCE_AUDIO_ERROR_NOT_INITIALIZED;
    if (!audio_regular_sample_count_valid(A1)) return SCE_AUDIO_ERROR_INVALID_SIZE;
    s_audio_len[A0] = A1;
    return 0;
}
/* sceAudioChangeChannelVolume(ch, leftvol, rightvol): retain the requested
 * output levels for a reserved regular channel. Public behaviour reference:
 * PSPSDK src/audio/pspaudio.h (sceAudioChangeChannelVolume) and PPSSPP
 * Core/HLE/sceAudio.cpp, which keep per-channel volumes as channel state.
 * Errors reuse the neighbouring regular-audio codes, and the 0x8000 volume
 * ceiling is the same bound h_AudioOutput2Blocking already enforces. Whether
 * the retained levels multiply the per-call volumes handed to the
 * OutputBlocking family, and the power-on default level, are UNMEASURED here:
 * mixing still consumes the per-call volumes only. */
static uint32_t s_audio_volL[8], s_audio_volR[8];
static uint32_t h_AudioChangeChannelVolume(CpuState *s) {
    if (A0 >= 8u) return SCE_AUDIO_ERROR_INVALID_CH;
    if (!s_audio_ch[A0]) return SCE_AUDIO_ERROR_NOT_INITIALIZED;
    if (A1 > 0x8000u || A2 > 0x8000u) return SCE_AUDIO_ERROR_INVALID_VOL;
    s_audio_volL[A0] = A1;
    s_audio_volR[A0] = A2;
    return 0;
}
/* Read a guest sample buffer, expand mono to stereo, hand to the backend, then block until
 * the host queue is back down to ~one buffer of lead (real sceAudio blocking semantics).
 * Pacing against the queue self-corrects: a late thread returns immediately and catches up,
 * an early one sleeps the difference. The old open-loop sleep of the buffer's duration ran
 * slightly slower than the device every iteration (sleep + decode time), so streams drifted
 * behind (voice lagging subtitles) and underran (crackle). */
extern int sr_audio_queued(int ch);
static uint32_t s_audio_delay_carry[9];

/* Convert a frame delta to microseconds without discarding the 44.1 kHz fractional carry.
 * The old integer division lost up to almost one microsecond on every wake and could also
 * produce a zero-delay scheduler spin for a small queue lead. */
static uint32_t audio_frames_to_us(uint32_t ch, uint32_t frames) {
    uint32_t slot = ch < 9u ? ch : 8u;
    uint64_t scaled = (uint64_t)frames * 1000000u + s_audio_delay_carry[slot];
    uint32_t us = (uint32_t)(scaled / 44100u);
    s_audio_delay_carry[slot] = (uint32_t)(scaled % 44100u);
    return us ? us : 1u;
}

/* SR_AUDIOSTAT: the audio drift the guest actually felt, read where it is felt.
 *
 * sr_audio_queued() is the value the blocking output below paces against -- the audio the
 * host mixer still owes the guest -- so it is also the honest place to measure drift. Counting
 * pushed frames cannot: a guest that submits exactly what it consumes and one that submits
 * twice as much both push frames, and only the queue depth tells them apart. A queue that only
 * grows is drift a consumer hears as lag building over minutes while nothing is dropped; once
 * it reaches the mixer's capacity the push clamps and real guest audio is lost. Neither is
 * visible in the end-of-run totals, so the reading is windowed (LEAD_WINDOW_VBLANK vblanks,
 * about five seconds) and printed as AUDIOSTAT_LEAD, which gives a long run a series instead
 * of a single end figure.
 *
 * The line is backend-neutral on purpose: the two host mixers are separate translation units
 * with separate telemetry, and neither is in every build, but this code is. `queued` is the
 * deepest lead any channel carried in the window and `lead_ms` is that lead in milliseconds
 * of audio; both are -1 when every output in the window had no host queue at all, which is
 * absence of a measurement and must never read as a drift of zero. */
#define LEAD_WINDOW_VBLANK 300u
#define LEAD_FRAMES_PER_MS  (44100u / 1000u)
static uint32_t s_lead_last_vbl;
static unsigned long s_lead_outputs, s_lead_noqueue;
static int s_lead_peak;
static uint32_t s_lead_peak_ch;
static long s_lead_last_ms;

static void audio_lead_note(uint32_t vbl, int ch, int queued) {
    s_lead_outputs++;
    if (queued < 0) {
        s_lead_noqueue++;
    } else if (queued > s_lead_peak) {
        s_lead_peak = queued;
        s_lead_peak_ch = (uint32_t)(ch < 0 ? 0 : ch);
    }
    if (vbl < s_lead_last_vbl) return;                  /* no window across a vcount reset */
    if (vbl - s_lead_last_vbl < LEAD_WINDOW_VBLANK) return;
    s_lead_last_vbl = vbl;
    int measured = s_lead_noqueue < s_lead_outputs;     /* some output saw a real queue */
    s_lead_last_ms = measured ? (long)s_lead_peak / (long)LEAD_FRAMES_PER_MS : -1L;
    fprintf(stderr, "AUDIOSTAT_LEAD: vbl=%u ch=%u outputs=%lu queued=%d lead_ms=%ld\n",
            vbl, s_lead_peak_ch, s_lead_outputs, measured ? s_lead_peak : -1, s_lead_last_ms);
    s_lead_outputs = s_lead_noqueue = 0;
    s_lead_peak = 0;
    s_lead_peak_ch = 0;
}

static void audio_note_lead(int ch, int queued) {
    if (!audio_stat_on()) return;
    audio_lead_note(sr_audio_vbl(), ch, queued);
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only view of the drift window, so the selftest can assert what the
 * AUDIOSTAT_LEAD line would say without capturing stderr: feed a reading, read the
 * window's own accumulator back, and read the milliseconds the line printed. */
void sr_hle_test_audio_lead_reset(void) {
    s_lead_last_vbl = 0;
    s_lead_outputs = s_lead_noqueue = 0;
    s_lead_peak = 0;
    s_lead_peak_ch = 0;
    s_lead_last_ms = -1L;
}
void sr_hle_test_audio_lead_feed(uint32_t vbl, int ch, int queued) {
    audio_lead_note(vbl, ch, queued);
}
unsigned long sr_hle_test_audio_lead_window(int *peak, uint32_t *peak_ch,
                                            unsigned long *noqueue, long *last_ms) {
    *peak = s_lead_peak;
    *peak_ch = s_lead_peak_ch;
    *noqueue = s_lead_noqueue;
    *last_ms = s_lead_last_ms;
    return s_lead_outputs;
}
#endif

/* SR_AUDIOSTAT: buffer identity per channel. Proving the buffer sceAudioOutput2
 * receives is the same one __sceSasCore wrote is what links the two stages; the
 * addresses are guest-side and the game double-buffers, so record a small set. */
uint32_t g_audio_bufs[9][4];
int      g_audio_nbufs[9];
static void audio_note_buf(uint32_t ch, uint32_t buf) {
    if (ch > 8u || !buf) return;
    for (int i = 0; i < g_audio_nbufs[ch]; i++) if (g_audio_bufs[ch][i] == buf) return;
    if (g_audio_nbufs[ch] < 4) g_audio_bufs[ch][g_audio_nbufs[ch]++] = buf;
}

void hle_audio_guest_reset(void) {
    memset(s_audio_ch, 0, sizeof(s_audio_ch));
    memset(s_audio_fmt, 0, sizeof(s_audio_fmt));
    memset(s_audio_len, 0, sizeof(s_audio_len));
    memset(s_audio_volL, 0, sizeof(s_audio_volL));
    memset(s_audio_volR, 0, sizeof(s_audio_volR));
    s_audio_src_owned = 0;
    s_audio_src_freq = 0;
    sr_audio_resampler_destroy(&s_audio_src_resampler);
    memset(s_audio_delay_carry, 0, sizeof(s_audio_delay_carry));
    memset(g_audio_bufs, 0, sizeof(g_audio_bufs));
    memset(g_audio_nbufs, 0, sizeof(g_audio_nbufs));
    s_lead_last_vbl = 0u;
    s_lead_outputs = s_lead_noqueue = 0u;
    s_lead_peak = 0;
    s_lead_peak_ch = 0u;
    s_lead_last_ms = -1L;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Read-only regular-audio state for production-dispatch contract regressions. */
void sr_hle_test_audio_reset(void) { hle_audio_guest_reset(); }

int sr_hle_test_audio_state(uint32_t ch, int *reserved, uint32_t *frames, int *format) {
    if (ch >= 8u) return 0;
    if (reserved) *reserved = s_audio_ch[ch];
    if (frames)   *frames   = s_audio_len[ch];
    if (format)   *format   = s_audio_fmt[ch];
    return 1;
}

/* Read-only view of the retained ChangeChannelVolume levels for the
 * production-dispatch regression below. Test builds only. */
int sr_hle_test_audio_volume(uint32_t ch, uint32_t *left, uint32_t *right) {
    if (ch >= 8u) return 0;
    if (left)  *left  = s_audio_volL[ch];
    if (right) *right = s_audio_volR[ch];
    return 1;
}
#endif

static uint32_t audio_output(CpuState *s, uint32_t ch, uint32_t buf, int voll, int volr) {
    (void)s;
    uint32_t n = ch < 9 ? s_audio_len[ch] : 1024;
    int mono = ch < 9 && s_audio_fmt[ch] == (int)PSP_AUDIO_FORMAT_MONO;
    uint32_t bytes = 0;

    /* Validate the complete source span before telemetry, scalar reads, backend
     * submission, host-queue observation, or scheduler delay. A null buffer is
     * intentionally preserved: public PSPAutotests show it is accepted by the
     * regular blocking API, and this path has never dereferenced it. */
    if (buf && n > 0u &&
        (!sr_size_mul_ok(n, mono ? 2u : 4u, &bytes) ||
         !sr_guest_span_readable(buf, bytes)))
        return 0x80000103u; /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */

    if (audio_stat_on()) audio_note_buf(ch, buf);
    if (buf && n > 0 && n <= 65536) {
        static int16_t lr[65536 * 2];
        for (uint32_t i = 0; i < n; i++) {
            if (mono) { int16_t v = (int16_t)MEM_R16(buf + i * 2); lr[i*2] = v; lr[i*2+1] = v; }
            else { lr[i*2] = (int16_t)MEM_R16(buf + i * 4); lr[i*2+1] = (int16_t)MEM_R16(buf + i * 4 + 2); }
        }
        /* The output counters live here, on the guest-visible sceAudioOutput* hand-off,
         * not inside a host audio backend. A backend that does not instrument itself made
         * audio_output_calls/frames read zero for a run whose music was plainly audible,
         * which is exactly the case the counters exist to describe. */
        psmf_av_audio_submit((int)ch, buf, bytes, mono ? 2u : 4u, 44100u);   /* queue lead read before the push */
        uint64_t out_started = sr_perf_now_ns();
        sr_audio_push((int)ch, lr, (int)n, voll, volr);
        if (out_started) sr_perf_audio_output(out_started, n);
    }
    int q = sr_audio_queued((int)ch);
    if (q < 0 || n == 0) {                 /* no host audio: open-loop pacing as before */
        sched_delay_current(n ? audio_frames_to_us(ch, n) : 1000u);
        return n;
    }
    /* sr_audio_queued() is signed only so the backend can report -1. Once that
     * sentinel is excluded, keep the frame comparison/subtraction unsigned;
     * never narrow the guest-derived frame count into the signed queue domain.
     * This form is defense in depth, not a repaired defect: with the reserve and
     * SetChannelDataLen size contracts above, n can no longer exceed INT_MAX, so
     * no production dispatch distinguishes it and it has no failing-before.
     *
     * Release at a two-period lead, not one (#67). A delayed thread is only woken
     * at a scheduler boundary, and a busy lower-priority runner can hold the CPU
     * until the next one -- as much as a VBLANK period. Released with only one
     * period queued, any wake that late drains the host ring before the next
     * push, and the write cursor snaps past lost audio. Two periods give that
     * boundary latency a whole period of headroom while still pacing against the
     * device's real drain; n is bounded by the reserve/SetChannelDataLen
     * contracts (<= 65536 frames), so 2u * n cannot wrap. */
    while ((q = sr_audio_queued((int)ch)) >= 0 && (uint32_t)q > 2u * n)
        sched_delay_current(audio_frames_to_us(ch, (uint32_t)q - 2u * n));
    audio_note_lead((int)ch, q);
    return n;
}
static uint32_t h_AudioOutputBlocking(CpuState *s) {
    /* sceAudioOutputBlocking(ch, vol, buf) */
    return audio_output(s, A0, A2, (int)(A1 & 0xFFFF), (int)(A1 & 0xFFFF));
}
static uint32_t h_AudioOutputPannedBlocking(CpuState *s) {
    /* sceAudioOutputPannedBlocking(ch, leftvol, rightvol, buf) */
    return audio_output(s, A0, A3, (int)(A1 & 0xFFFF), (int)(A2 & 0xFFFF));
}
static uint32_t h_AudioChangeChannelConfig(CpuState *s) {
    (void)s;
    if (A0 >= 8u) return SCE_AUDIO_ERROR_INVALID_CH;
    if (!s_audio_ch[A0]) return SCE_AUDIO_ERROR_NOT_INITIALIZED;
    if (A1 != PSP_AUDIO_FORMAT_STEREO && A1 != PSP_AUDIO_FORMAT_MONO)
        return SCE_AUDIO_ERROR_INVALID_FORMAT;
    s_audio_fmt[A0] = (int)A1;
    return 0;
}
static uint32_t h_AudioRestLen(CpuState *s) {
    (void)s;
    if (A0 >= 8u) return SCE_AUDIO_ERROR_INVALID_CH;
    int q = sr_audio_queued((int)A0);
    return q > 0 ? (uint32_t)q : 0u;
}

/* Keep the executable audio contract harness on the exact production NID
 * mapping. The Output2 family is registered by hle_register_output2_audio_handlers(),
 * which both the harness and the production branch of sr_hle_init call. */
void hle_register_regular_audio_handlers(void) {
    sr_hle_register(0x5ec81c55, "sceAudioChReserve", h_AudioChReserve);
    sr_hle_register(0x6fc46853, "sceAudioChRelease", h_AudioChRelease);
    sr_hle_register(0x136caf51, "sceAudioOutputBlocking", h_AudioOutputBlocking);
    sr_hle_register(0x13f592bc, "sceAudioOutputPannedBlocking", h_AudioOutputPannedBlocking);
    sr_hle_register(0xe2d56b2d, "sceAudioOutputPanned", h_AudioOutputPannedBlocking);
    sr_hle_register(0x95fd0c2d, "sceAudioChangeChannelConfig", h_AudioChangeChannelConfig);
    sr_hle_register(0xb011922f, "sceAudioGetChannelRestLength", h_AudioRestLen);
    sr_hle_register(0xb7e1d8e7, "sceAudioChangeChannelVolume", h_AudioChangeChannelVolume);
    sr_hle_register(0xcb2e439e, "sceAudioSetChannelDataLen", h_AudioSetChannelDataLen);
}

#define AUDIO_OUTPUT2_CHANNEL 8u

/* Slot 8 reserved through sceAudioOutput2Reserve, as opposed to sceAudioSRCChReserve. Using one
 * API's call on the other's reservation is unmeasured; it is refused as "not reserved". */
static int audio_output2_reserved(void) {
    return s_audio_ch[AUDIO_OUTPUT2_CHANNEL] && !s_audio_src_owned;
}

static uint32_t h_AudioOutput2Reserve(CpuState *s) {
    uint32_t samples = A0;
    /* A slot held through sceAudioSRCChReserve refuses this with 0x80268002 (measured,
     * PSP-HLE-AUDIO-SRC-001). The code for a second Output2 reserve over an Output2 one is
     * unmeasured and keeps the regular-channel busy code. */
    if (s_audio_src_owned) return SCE_AUDIO_ERROR_OUTPUT2_BUSY;
    if (s_audio_ch[AUDIO_OUTPUT2_CHANNEL]) return SCE_AUDIO_ERROR_OUTPUT_BUSY;
    if (samples < 17u || samples > 4111u) return SCE_AUDIO_ERROR_INVALID_SIZE;
    s_audio_ch[AUDIO_OUTPUT2_CHANNEL] = 1;
    s_audio_fmt[AUDIO_OUTPUT2_CHANNEL] = 0;
    s_audio_len[AUDIO_OUTPUT2_CHANNEL] = samples;
    return 0;
}
/* Measured on PSP-3001, firmware 6.6.1 (2026-10-01, issue #311): releasing while samples are
 * still queued returns 0x80268002 and leaves Output2 reserved; draining
 * sceAudioOutput2GetRestSample to 0 first lets the release succeed. The queue is read the
 * same way h_AudioOutput2Rest reads it. */
static uint32_t h_AudioOutput2Release(CpuState *s) {
    (void)s;
    if (!audio_output2_reserved()) return SCE_AUDIO_ERROR_NOT_RESERVED;
    if (sr_audio_queued(AUDIO_OUTPUT2_CHANNEL) > 0) return SCE_AUDIO_ERROR_OUTPUT2_BUSY;
    s_audio_ch[AUDIO_OUTPUT2_CHANNEL] = 0;
    s_audio_len[AUDIO_OUTPUT2_CHANNEL] = 0;
    return 0;
}
static uint32_t h_AudioOutput2ChangeLength(CpuState *s) {
    if (!audio_output2_reserved()) return SCE_AUDIO_ERROR_NOT_RESERVED;
    if (A0 < 17u || A0 > 4111u) return SCE_AUDIO_ERROR_INVALID_SIZE;
    s_audio_len[AUDIO_OUTPUT2_CHANNEL] = A0;
    return 0;
}
static uint32_t h_AudioOutput2Blocking(CpuState *s) {
    if (!audio_output2_reserved()) return SCE_AUDIO_ERROR_NOT_RESERVED;
    if (A0 > 0x8000u) return SCE_AUDIO_ERROR_INVALID_VOL;
    (void)audio_output(s, AUDIO_OUTPUT2_CHANNEL, A1, (int)A0, (int)A0);
    return 0;
}
static uint32_t h_AudioOutput2Rest(CpuState *s) {
    (void)s;
    if (!audio_output2_reserved()) return SCE_AUDIO_ERROR_NOT_RESERVED;
    int queued = sr_audio_queued(AUDIO_OUTPUT2_CHANNEL);
    return queued > 0 ? (uint32_t)queued : 0u;
}

/* sceAudioSRCChReserve / sceAudioSRCOutputBlocking / sceAudioSRCChRelease (issue #339).
 *
 * The SRC channel is the same hardware slot Output2 uses (slot 8 here). Measured on PSP-3000,
 * firmware 6.6.1, 2026-10-15 (PSP-HLE-AUDIO-SRC-001): with nothing reserved Release and
 * OutputBlocking return 0x80260008; Reserve(1024, 44100, 2) returns 0, an identical second
 * Reserve returns 0x80268002, OutputBlocking then returns the sample count, Release returns
 * 0 and a second Release returns 0x80260008; a sample count of 0 and a channel count of 1 return
 * 0x80000104 and a channel count of 4 returns 0x80000003; sample counts 65 and 4096 and a rate
 * of 48000 Hz are accepted; and Output2 and SRC refuse each other's Reserve with 0x80268002 in
 * both orders.
 *
 * Not measured, so held to what is already modelled or refused in the open: the upper
 * sample-count bound (the Output2 range 17..4111 is reused), channel counts other than 1, 2
 * and 4 (refused with the code measured for 4), the volume bound (the Output2 bound 0x8000 is
 * reused), and rates other than 44100 and 48000 (refused with the code measured for an
 * unsupported channel count, and a message). The host mixer runs at 44100 Hz; 48000 Hz SRC
 * blocks pass through a streaming band-limited converter while retaining source-rate pacing. */
static uint32_t h_AudioSRCChReserve(CpuState *s) {
    const uint32_t samples = A0, freq = A1, channels = A2;
    if (s_audio_ch[AUDIO_OUTPUT2_CHANNEL]) return SCE_AUDIO_ERROR_OUTPUT2_BUSY;
    if (samples < 17u || samples > 4111u) return SCE_AUDIO_ERROR_SRC_INVALID_SIZE;
    if (channels == 1u) return SCE_AUDIO_ERROR_SRC_INVALID_SIZE;
    if (channels != 2u) return SCE_AUDIO_ERROR_SRC_INVALID_CHANNELS;
    if (freq != 44100u && freq != 48000u) {
        fprintf(stderr, "AUDIOSRC+ unsupported sample rate %u Hz: only 44100 and 48000 Hz are "
                "modelled, the reservation is refused\n", freq);
        return SCE_AUDIO_ERROR_SRC_INVALID_CHANNELS;
    }
    sr_audio_resampler_destroy(&s_audio_src_resampler);
    if (freq != SR_AUDIO_RESAMPLER_OUTPUT_RATE && !sr_audio_resampler_init(&s_audio_src_resampler, freq))
        return HLE_KERNEL_ERROR_NO_MEMORY;
    s_audio_ch[AUDIO_OUTPUT2_CHANNEL] = 1;
    s_audio_fmt[AUDIO_OUTPUT2_CHANNEL] = 0;
    s_audio_len[AUDIO_OUTPUT2_CHANNEL] = samples;
    s_audio_src_owned = 1;
    s_audio_src_freq = freq;
    return 0;
}
static uint32_t h_AudioSRCChRelease(CpuState *s) {
    (void)s;
    if (!s_audio_src_owned) return SCE_AUDIO_ERROR_NOT_RESERVED;
    s_audio_ch[AUDIO_OUTPUT2_CHANNEL] = 0;
    s_audio_len[AUDIO_OUTPUT2_CHANNEL] = 0;
    s_audio_src_owned = 0;
    s_audio_src_freq = 0;
    sr_audio_resampler_destroy(&s_audio_src_resampler);
    return 0;
}
static uint32_t h_AudioSRCOutputBlocking(CpuState *s) {
    /* sceAudioSRCOutputBlocking(vol, buf): returns the reserved sample count. */
    if (!s_audio_src_owned) return SCE_AUDIO_ERROR_NOT_RESERVED;
    if (A0 > 0x8000u) return SCE_AUDIO_ERROR_INVALID_VOL;
    if (s_audio_src_freq == 44100u)
        return audio_output(s, AUDIO_OUTPUT2_CHANNEL, A1, (int)A0, (int)A0);

    /* Preserve the guest-rate blocking interval while forwarding converted frames to the
     * 44100 Hz mixer. The reserve contract limits n to 4111 stereo frames. */
    const uint32_t n = s_audio_len[AUDIO_OUTPUT2_CHANNEL];
    uint32_t bytes = 0;
    if (A1 && n > 0u &&
        (!sr_size_mul_ok(n, 4u, &bytes) || !sr_guest_span_readable(A1, bytes)))
        return 0x80000103u; /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
    /* Fail closed if a reserved rate ever lacks its converter (Reserve builds one for every
     * accepted rate other than the mixer's own). */
    if (s_audio_src_resampler.input_rate != s_audio_src_freq)
        return SCE_AUDIO_ERROR_SRC_INVALID_CHANNELS;
    if (A1 && n > 0u) {
        /* Shared scratch is safe: the SRC slot has one owner, and nothing between filling
         * these buffers and handing the frames to the mixer can yield to another thread. */
        static int16_t input[4111u * 2u];
        static int16_t converted[4111u * 2u];
        for (uint32_t i = 0; i < n; i++) {
            input[i * 2u] = (int16_t)MEM_R16(A1 + i * 4u);
            input[i * 2u + 1u] = (int16_t)MEM_R16(A1 + i * 4u + 2u);
        }
        if (!sr_audio_resampler_push(&s_audio_src_resampler, input, n))
            return HLE_KERNEL_ERROR_NO_MEMORY;
        psmf_av_audio_submit(AUDIO_OUTPUT2_CHANNEL, A1, bytes, 4u, s_audio_src_freq);
        const size_t converted_frames = sr_audio_resampler_pull(
            &s_audio_src_resampler, converted, 4111u);
        if (converted_frames > 0u) {
            uint64_t out_started = sr_perf_now_ns();
            sr_audio_push(AUDIO_OUTPUT2_CHANNEL, converted, (int)converted_frames,
                          (int)A0, (int)A0);
            if (out_started)
                sr_perf_audio_output(out_started, (uint32_t)converted_frames);
        }
    }
    uint64_t us = (uint64_t)n * 1000000u / s_audio_src_freq;
    sched_delay_current(us ? (uint32_t)us : 1u);
    return n;
}

/* Output2 and SRC registrations shared by the production mapping and the ThreadMan/audio
 * selftest harness, so the selftest drives these exact production handlers through sr_syscall. */
void hle_register_output2_audio_handlers(void) {
    sr_hle_register(0x38553111, "sceAudioSRCChReserve", h_AudioSRCChReserve);
    sr_hle_register(0x5c37c0ae, "sceAudioSRCChRelease", h_AudioSRCChRelease);
    sr_hle_register(0xe0727056, "sceAudioSRCOutputBlocking", h_AudioSRCOutputBlocking);
    sr_hle_register(0x01562ba3, "sceAudioOutput2Reserve", h_AudioOutput2Reserve);
    sr_hle_register(0x2d53f36e, "sceAudioOutput2OutputBlocking", h_AudioOutput2Blocking);
    sr_hle_register(0x43196845, "sceAudioOutput2Release", h_AudioOutput2Release);
    sr_hle_register(0x63f2889c, "sceAudioOutput2ChangeLength", h_AudioOutput2ChangeLength);
    sr_hle_register(0x647cef33, "sceAudioOutput2GetRestSample", h_AudioOutput2Rest);
}

/* ---- sceSasCore: bounded, stateful voice mixer -------------------------------------
 *
 * The real PSP keeps a 3616-byte SceSasCore object in guest memory.  The native
 * implementation keeps the decoded state host-side, but the guest pointer is still a
 * load-bearing handle: every operation validates the same aligned, readable core span
 * and the initialized core identity before touching a voice.  This prevents the old
 * ``A1 & 31`` aliases from turning malformed calls into writes to another voice.
 *
 * The mixer is intentionally modest (VAG ADPCM, mono PCM, deterministic noise and a
 * simple ADSR), but all successful setters retain the state consumed by later queries or
 * mixing.  Unsupported waveform/ATRAC3 operations return a controlled SAS error instead
 * of fabricating success. */
#define SAS_VOICES 32
#define SAS_CORE_BYTES 3616u
#define SAS_GRAIN_MIN 64
#define SAS_GRAIN_MAX 2048
#define SAS_SAMPLE_RATE 44100
#define SAS_VOLUME_MAX 0x1000
#define SAS_PITCH_MIN 1
#define SAS_PITCH_MAX 0x4000
#define SAS_NOISE_FREQ_MAX 0x3f
#define SAS_ENVELOPE_MAX 0x40000000
#define SAS_ADSR_ATTACK  0x1u
#define SAS_ADSR_DECAY   0x2u
#define SAS_ADSR_SUSTAIN 0x4u
#define SAS_ADSR_RELEASE 0x8u
#define SAS_ADSR_ALL     (SAS_ADSR_ATTACK | SAS_ADSR_DECAY | SAS_ADSR_SUSTAIN | SAS_ADSR_RELEASE)

/* PSPSDK's public error values (pspsascore.h).  Keep these in one place so a rejected
 * call is distinguishable from a successful no-op in both production and selftests. */
#define SAS_ERROR_ADDRESS       0x80420005u
#define SAS_ERROR_VOICE_INDEX   0x80420010u
#define SAS_ERROR_NOISE_CLOCK   0x80420011u
#define SAS_ERROR_PITCH_VAL     0x80420012u
#define SAS_ERROR_ADSR_MODE     0x80420013u
#define SAS_ERROR_ADPCM_SIZE    0x80420014u
#define SAS_ERROR_LOOP_MODE     0x80420015u
#define SAS_ERROR_INVALID_STATE 0x80420016u
#define SAS_ERROR_VOLUME_VAL    0x80420018u
#define SAS_ERROR_ADSR_VAL      0x80420019u
#define SAS_ERROR_FX_TYPE       0x80420020u
#define SAS_ERROR_FX_FEEDBACK   0x80420021u
#define SAS_ERROR_FX_DELAY      0x80420022u
#define SAS_ERROR_FX_VOLUME_VAL 0x80420023u
#define SAS_ERROR_NOTINIT       0x80420100u

typedef enum {
    SAS_VOICE_OFF = 0,
    SAS_VOICE_VAG,
    SAS_VOICE_PCM,
    SAS_VOICE_NOISE,
    SAS_VOICE_ATRAC3,
} SasVoiceType;

typedef struct {
    int on;                       /* keyed on and source not exhausted */
    int paused;                   /* playback pause bit */
    SasVoiceType type;            /* explicit source kind; never inferred from vag */
    uint32_t vag, vag_size;       /* VAG stream base and byte size */
    uint32_t pos;                 /* byte offset of the next 16-byte block */
    int loop_requested;           /* guest loop policy; data markers cannot override 0 */
    int loop_start;               /* marked block offset to loop to (-1 = none) */
    int hist1, hist2;             /* ADPCM filter state */
    int pitch;                    /* 0x1000 = native 44.1 kHz */
    int voll, volr;               /* dry L/R, 0..0x1000 */
    int effect_l, effect_r;       /* send L/R, 0..0x1000 */
    int16_t buf[28]; int bufn, bufi;
    uint32_t frac;                /* 12-bit fixed-point resample remainder */
    uint32_t pcm;                 /* mono signed-16 PCM source */
    uint32_t pcm_samples, pcm_pos;
    int pcm_loop_start;
    /* ADSR rates/modes and retained simple-envelope words. */
    int adsr_a, adsr_d, adsr_s, adsr_r;
    int mode_a, mode_d, mode_s, mode_r;
    int sustain_level;            /* 0..0x40000000 */
    uint32_t simple_adsr1, simple_adsr2;
    int env_level;                /* 0..0x40000000 */
    int env_phase;                /* 0=ATTACK, 1=DECAY, 2=SUSTAIN, 3=RELEASE, 4=OFF */
    int noise;                    /* 0..63 (noise clock) */
} SasVoice;

typedef struct {
    int initialized;
    uint32_t core;
    int grain;
    int max_voices;
    int output_mode;              /* 0=interleaved stereo, 1=four planar channels */
    int sample_rate;
    int effect_type;              /* -1=off, 0..8 are the PSP effect types */
    int effect_l, effect_r;
    int effect_delay, effect_feedback;
    int effect_dry, effect_wet;
    uint32_t end_flags;           /* voice end bits (bit N set = voice N off), per-cycle snapshot */
} SasCoreState;

static SasVoice s_sasv[SAS_VOICES];
static SasCoreState s_sas_core;
static int s_sas_max_voices = SAS_VOICES;

void hle_sas_guest_reset(void) {
    memset(&s_sas_core, 0, sizeof(s_sas_core));
    memset(s_sasv, 0, sizeof(s_sasv));
    s_sas_max_voices = SAS_VOICES;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* The selftest creates several independent synthetic cores in one process.  A
 * real guest would keep one initialized core until teardown; this explicit
 * fixture hook resets only the host-side model and never enters production
 * registration or dispatch paths. */
void sr_hle_test_sas_reset(void) { hle_sas_guest_reset(); }
#endif

/* SR_SASLOG: decay-gated trace of KeyOn events and SAS mixer active-voice counts, to determine
 * whether the game ever keys on a SAS voice during the silent-PCM window. */
static int sas_log_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_SASLOG") != NULL;
    return on;
}

static const int vag_f0[5] = { 0, 60, 115,  98, 122 };
static const int vag_f1[5] = { 0,  0, -52, -55, -60 };

/* Decode the next 16-byte VAG block into v->buf. Returns 0 when the stream ends. */
static int sas_vag_block(SasVoice *v) {
    /* Do subtraction before addition: a forged pos near UINT32_MAX must not wrap into
     * the apparently-valid [pos, pos+16) test.  SetVoice already checked the complete
     * source span, but the cursor still needs this per-block guard after every loop. */
    if (v->pos > v->vag_size || v->vag_size - v->pos < 16u) return 0;
    uint32_t a = v->vag + v->pos;
    int hdr = MEM_R8(a), flags = MEM_R8(a + 1);
    if (flags == 7) return 0;                            /* end marker block */
    if (sas_log_on()) {
        /* Bounded per-voice VAG block dumps (two emissions max, matching the
         * docs): the first block (pos 0, first grain) and the block at +0x10,
         * which is the first data block after a legitimate zero prefix. The
         * 16-byte span is guaranteed by the pos+16 guard above; validate the
         * guest span anyway so the diagnostic never reads past mapped memory. */
        static uint32_t dumps[32];
        uint32_t vi = (uint32_t)(v - s_sasv);
        if (dumps[vi] < 2u && sr_guest_span_readable(v->vag + v->pos, 16u)) {
            uint8_t bh[16];
            for (uint32_t k = 0; k < 16; k++) bh[k] = MEM_R8(a + k);
            if (v->pos == 0u && v->bufn == 0) {
                fprintf(stderr, "SAS_VAG_B0: vbl=%u voice=%u vag=0x%08x pos=0x%x size=%u hdr=%02x flags=%02x b0=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        s_vcount, vi, v->vag, v->pos, v->vag_size, hdr, flags,
                        bh[0], bh[1], bh[2], bh[3], bh[4], bh[5], bh[6], bh[7],
                        bh[8], bh[9], bh[10], bh[11], bh[12], bh[13], bh[14], bh[15]);
                dumps[vi]++;
            } else if (v->pos == 0x10u) {
                fprintf(stderr, "SAS_VAG_B16: vbl=%u voice=%u vag=0x%08x pos=0x%x size=%u hdr=%02x flags=%02x b16=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        s_vcount, vi, v->vag, v->pos, v->vag_size, hdr, flags,
                        bh[0], bh[1], bh[2], bh[3], bh[4], bh[5], bh[6], bh[7],
                        bh[8], bh[9], bh[10], bh[11], bh[12], bh[13], bh[14], bh[15]);
                dumps[vi]++;
            }
        }
    }
    int pred = (hdr >> 4) & 0xF, shift = hdr & 0xF;
    if (pred > 4) pred = 0;
    if (flags == 6 && v->loop_requested) v->loop_start = (int)v->pos; /* guest-enabled loop */
    for (int i = 0; i < 28; i++) {
        int byte = MEM_R8(a + 2 + (i >> 1));
        int nib = (i & 1) ? (byte >> 4) : (byte & 0xF);
        int samp = (int)((int16_t)((uint16_t)nib << 12)) >> shift;
        samp += (v->hist1 * vag_f0[pred] + v->hist2 * vag_f1[pred]) >> 6;
        if (samp > 32767) samp = 32767;
        if (samp < -32768) samp = -32768;
        v->buf[i] = (int16_t)samp;

        v->hist2 = v->hist1; v->hist1 = samp;
    }
    v->pos += 16;
    if (flags == 3 && v->loop_requested && v->loop_start >= 0)
        v->pos = (uint32_t)v->loop_start;  /* data marker + guest policy */
    v->bufn = 28; v->bufi = 0;
    return 1;
}
/* Stateful handlers for the public SAS registrations.  The host keeps the
 * decoded voice state here while the guest core pointer remains a validated
 * identity handle. */
static uint32_t sas_core_address_ok(uint32_t core) {
    if (!core || (core & 0x3fu) != 0 || !sr_guest_span_readable(core, SAS_CORE_BYTES))
        return SAS_ERROR_ADDRESS;
    return 0;
}

static uint32_t sas_require_core(uint32_t core) {
    uint32_t e = sas_core_address_ok(core);
    if (e) return e;
    if (!s_sas_core.initialized || core != s_sas_core.core) return SAS_ERROR_NOTINIT;
    return 0;
}

static uint32_t sas_voice_for(uint32_t core, uint32_t raw_voice, SasVoice **out) {
    uint32_t e = sas_require_core(core);
    int32_t voice = (int32_t)raw_voice;
    if (e) return e;
    if (voice < 0 || voice >= s_sas_max_voices) return SAS_ERROR_VOICE_INDEX;
    *out = &s_sasv[voice];
    return 0;
}

static int sas_grain_valid(int32_t grain) {
    return grain >= SAS_GRAIN_MIN && grain <= SAS_GRAIN_MAX && (grain % 64) == 0;
}

static int sas_output_bytes(uint32_t *bytes) {
    uint32_t channels = s_sas_core.output_mode ? 4u : 2u;
    return sr_size_mul_ok((uint32_t)s_sas_core.grain, channels * 2u, bytes);
}

static uint32_t sas_output_validate(uint32_t out, int add) {
    uint32_t bytes = 0;
    if (!out || !sas_output_bytes(&bytes) || !sr_guest_span_writable(out, bytes))
        return SAS_ERROR_ADDRESS;
    if (add && !sr_guest_span_readable(out, bytes)) return SAS_ERROR_ADDRESS;
    return 0;
}

static int16_t sas_output_read(uint32_t out, uint32_t frame, int channel) {
    uint32_t off;
    if (s_sas_core.output_mode) {
        off = (uint32_t)channel * (uint32_t)s_sas_core.grain * 2u + frame * 2u;
    } else {
        if (channel > 1) return 0;
        off = frame * 4u + (uint32_t)channel * 2u;
    }
    return (int16_t)MEM_R16(out + off);
}

static void sas_output_write(uint32_t out, uint32_t frame, int channel, int value) {
    uint32_t off;
    if (s_sas_core.output_mode) {
        off = (uint32_t)channel * (uint32_t)s_sas_core.grain * 2u + frame * 2u;
    } else {
        if (channel > 1) return;
        off = frame * 4u + (uint32_t)channel * 2u;
    }
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    MEM_W16(out + off, (uint16_t)(int16_t)value);
}

static int sas_env_step(int configured, int fallback) {
    return configured > 0 ? configured : fallback;
}

static void sas_advance_envelope(SasVoice *v) {
    if (v->env_phase == 0) {
        int64_t level = (int64_t)v->env_level + sas_env_step(v->adsr_a, 0x1000000);
        if (level >= SAS_ENVELOPE_MAX) { v->env_level = SAS_ENVELOPE_MAX; v->env_phase = 1; }
        else v->env_level = (int)level;
    } else if (v->env_phase == 1) {
        int step = sas_env_step(v->adsr_d, 0x800000);
        if (v->env_level > v->sustain_level) {
            v->env_level -= step;
            if (v->env_level <= v->sustain_level) {
                v->env_level = v->sustain_level; v->env_phase = 2;
            }
        } else {
            v->env_level = v->sustain_level; v->env_phase = 2;
        }
    } else if (v->env_phase == 3) {
        v->env_level -= sas_env_step(v->adsr_r, 0x1000000);
        if (v->env_level <= 0) { v->env_level = 0; v->env_phase = 4; v->on = 0; }
    }
}

static int sas_next_sample(SasVoice *v, int *sample) {
    if (v->type == SAS_VOICE_NOISE) {
        static uint32_t nseed = 0x12345678;
        nseed = nseed * 1103515245u + 12345u;
        *sample = (int)(int16_t)(nseed >> 16);
        return 1;
    }
    if (v->type == SAS_VOICE_PCM) {
        if (v->pcm_pos >= v->pcm_samples) {
            if (v->pcm_loop_start >= 0 && v->pcm_loop_start < (int)v->pcm_samples)
                v->pcm_pos = (uint32_t)v->pcm_loop_start;
            else return 0;
        }
        *sample = (int16_t)MEM_R16(v->pcm + v->pcm_pos * 2u);
        v->pcm_pos++;
        return 1;
    }
    if (v->type != SAS_VOICE_VAG) return 0;
    if (v->bufi >= v->bufn && !sas_vag_block(v)) return 0;
    *sample = v->buf[v->bufi];
    return 1;
}

/* Live end bits: bit N is set when voice N is off. PSPSDK (pspsascore.h) says the end flags
 * are refreshed only AFTER __sceSasCore runs, so the guest reads the snapshot captured at the
 * end of each cycle (see sas_mix_stateful), not this live value. */
static uint32_t sas_live_end_flags(void) {
    uint32_t m = 0;
    for (int i = 0; i < s_sas_max_voices; i++) if (!s_sasv[i].on) m |= 1u << i;
    return m;
}

/* Mix one grain into out (s16 stereo or four planar channels). add=0 overwrites. */
static void sas_mix_stateful(uint32_t out, int add, int left_gain, int right_gain) {
    uint64_t perf_started = sr_perf_now_ns();
    int32_t mixl[2048], mixr[2048], mixsl[2048], mixsr[2048];
    int n = s_sas_core.grain;
    for (int i = 0; i < n; i++) mixl[i] = mixr[i] = mixsl[i] = mixsr[i] = 0;
    int stat = audio_stat_on(), pre_peak = 0;
    if (stat) {
        g_sas_calls++;
        if (add) g_sas_calls_add++;
        audio_note_sas_buf(out);
        int active = 0;
        for (int vi = 0; vi < s_sas_max_voices; vi++) if (s_sasv[vi].on) active++;
        if (!active) g_sas_no_voice++;
        if (!active && !add) g_sas_no_voice_overwrite++;
        for (int i = 0; i < n; i++) {
            int l = sas_output_read(out, (uint32_t)i, 0);
            int r = sas_output_read(out, (uint32_t)i, 1);
            if (l < 0) l = -l;
            if (r < 0) r = -r;
            if (l > pre_peak) pre_peak = l;
            if (r > pre_peak) pre_peak = r;
        }
        if (pre_peak) g_sas_pre_nonzero++;
        if (pre_peak > g_sas_pre_peak) g_sas_pre_peak = pre_peak;
    }
    if (sas_log_on()) {
        static uint32_t call;
        static int last_active = -1;
        call++;
        int active = 0; unsigned busy = 0;
        for (int vi = 0; vi < s_sas_max_voices; vi++) {
            if (s_sasv[vi].on) active++;
            if (s_sasv[vi].on) busy |= 1u << vi;
        }
        if (call <= 64u || (call & (call - 1u)) == 0u || active != last_active) {
            fprintf(stderr, "SAS_MIX: call=%u vbl=%u out=0x%08x add=%d active=%d busy=0x%x grain=%d\n",
                    call, s_vcount, out, add, active, busy, n);
            for (int vi = 0; vi < s_sas_max_voices; vi++) {
                SasVoice *v = &s_sasv[vi];
                if (v->on)
                    fprintf(stderr, "SAS_MIX_V: vbl=%u voice=%d type=%d pos=%u bufn=%d bufi=%d pitch=%d vol=%d/%d env=%d\n",
                            s_vcount, vi, v->type, v->pos, v->bufn, v->bufi, v->pitch, v->voll, v->volr, v->env_level);
            }
        }
        last_active = active;
    }
    for (int vi = 0; vi < s_sas_max_voices; vi++) {
        SasVoice *v = &s_sasv[vi];
        if (!v->on || v->paused) continue;
        for (int i = 0; i < n; i++) {
            int samp = 0;
            if (!sas_next_sample(v, &samp)) { v->on = 0; break; }
            sas_advance_envelope(v);
            int s_env = (int)(((int64_t)samp * v->env_level) >> 30);
            mixl[i] += (s_env * v->voll) >> 12;
            mixr[i] += (s_env * v->volr) >> 12;
            mixsl[i] += (s_env * v->effect_l) >> 12;
            mixsr[i] += (s_env * v->effect_r) >> 12;
            v->frac += (uint32_t)(v->pitch > 0 ? v->pitch : 0x1000);
            while (v->frac >= 0x1000) {
                v->frac -= 0x1000;
                if (v->type == SAS_VOICE_VAG) v->bufi++;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        /* The gain product is 64-bit: twenty full-scale voices already sum past INT32_MAX / 4096,
         * and a wrapped product inverts loud samples instead of saturating them at the output. */
        int l = (int)(((int64_t)mixl[i] * left_gain) >> 12);
        int r = (int)(((int64_t)mixr[i] * right_gain) >> 12);
        int sl = mixsl[i], sr = mixsr[i];
        if (add) {
            l += sas_output_read(out, (uint32_t)i, 0);
            r += sas_output_read(out, (uint32_t)i, 1);
            if (s_sas_core.output_mode) {
                sl += sas_output_read(out, (uint32_t)i, 2);
                sr += sas_output_read(out, (uint32_t)i, 3);
            }
        }
        sas_output_write(out, (uint32_t)i, 0, l);
        sas_output_write(out, (uint32_t)i, 1, r);
        if (s_sas_core.output_mode) {
            sas_output_write(out, (uint32_t)i, 2, sl);
            sas_output_write(out, (uint32_t)i, 3, sr);
        }
    }
    if (stat) {
        int post_peak = 0;
        for (int i = 0; i < n; i++) {
            int l = sas_output_read(out, (uint32_t)i, 0);
            int r = sas_output_read(out, (uint32_t)i, 1);
            if (l < 0) l = -l;
            if (r < 0) r = -r;
            if (l > post_peak) post_peak = l;
            if (r > post_peak) post_peak = r;
        }
        if (post_peak) g_sas_post_nonzero++;
        if (post_peak > g_sas_post_peak) g_sas_post_peak = post_peak;
        if (pre_peak && !post_peak) g_sas_erased++;
    }
    s_sas_core.end_flags = sas_live_end_flags();
    if (perf_started) sr_perf_audio_mix(perf_started);
}

/* An init on a core that is already initialized is a fresh init (measured on PSP-3000, firmware 6.61,
 * PSP-SAS-001, #1595): it returns 0, the new grain and output mode take effect, and every voice reads as
 * ended again. sceSasCore exports no exit call, so a game that stops its mixer for a movie re-inits the same
 * core this way. Initializing a second core leaves the first one's state alone on the PSP; this model keeps
 * one core, so the newest init is the live one. */
static uint32_t h_SasInit(CpuState *s) {
    uint32_t e = sas_core_address_ok(A0);
    int32_t grain = (int32_t)A1, max_voices = (int32_t)A2;
    int32_t output_mode = (int32_t)A3, sample_rate = (int32_t)stack_arg(s, 0);
    if (e) return e;
    if (!sas_grain_valid(grain)) return SAS_ERROR_ADPCM_SIZE;
    if (max_voices < 1 || max_voices > SAS_VOICES) return SAS_ERROR_VOICE_INDEX;
    if (output_mode != 0 && output_mode != 1) return SAS_ERROR_INVALID_STATE;
    if (sample_rate != SAS_SAMPLE_RATE) return SAS_ERROR_INVALID_STATE;
    memset(&s_sas_core, 0, sizeof(s_sas_core));
    s_sas_core.initialized = 1; s_sas_core.core = A0; s_sas_core.grain = grain;
    s_sas_core.max_voices = max_voices; s_sas_core.output_mode = output_mode;
    s_sas_core.sample_rate = sample_rate; s_sas_core.effect_type = -1;
    s_sas_core.effect_l = s_sas_core.effect_r = SAS_VOLUME_MAX;
    s_sas_core.effect_dry = s_sas_core.effect_wet = 1;
    s_sas_max_voices = max_voices;
    memset(s_sasv, 0, sizeof(s_sasv));
    for (int i = 0; i < SAS_VOICES; i++) {
        s_sasv[i].pitch = 0x1000; s_sasv[i].loop_start = -1; s_sasv[i].pcm_loop_start = -1;
        s_sasv[i].sustain_level = SAS_ENVELOPE_MAX; s_sasv[i].env_phase = 4;
    }
    s_sas_core.end_flags = sas_live_end_flags();
    return 0;
}

static uint32_t h_SasSetVoice(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t size = (int32_t)A3, loop = (int32_t)stack_arg(s, 0);
    if (e) return e;
    if (size <= 0 || (((uint32_t)size & 15u) != 0)) return SAS_ERROR_ADPCM_SIZE;
    if (loop != 0 && loop != 1) return SAS_ERROR_LOOP_MODE;
    if (!A2 || !sr_guest_span_readable(A2, (uint32_t)size)) return SAS_ERROR_ADDRESS;
    v->on = 0; v->type = SAS_VOICE_VAG; v->vag = A2; v->vag_size = (uint32_t)size;
    v->pos = 0; v->loop_requested = loop; v->loop_start = -1;
    v->pcm = v->pcm_samples = v->pcm_pos = 0; v->pcm_loop_start = -1;
    v->noise = 0; v->hist1 = v->hist2 = 0; v->bufn = v->bufi = 0; v->frac = 0;
    return 0;
}

static uint32_t h_SasSetVoicePCM(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t samples = (int32_t)A3, loop_start = (int32_t)stack_arg(s, 0);
    uint32_t bytes = 0;
    if (e) return e;
    if (samples <= 0 || !sr_size_mul_ok((uint32_t)samples, 2u, &bytes)) return SAS_ERROR_ADPCM_SIZE;
    if (loop_start < -1 || loop_start >= samples) return SAS_ERROR_LOOP_MODE;
    if (!A2 || !sr_guest_span_readable(A2, bytes)) return SAS_ERROR_ADDRESS;
    v->on = 0; v->type = SAS_VOICE_PCM; v->pcm = A2; v->pcm_samples = (uint32_t)samples;
    v->pcm_pos = 0; v->pcm_loop_start = loop_start; v->vag = v->vag_size = v->pos = 0;
    v->loop_requested = loop_start >= 0; v->loop_start = -1; v->noise = 0; v->bufn = v->bufi = 0;
    return 0;
}

static uint32_t h_SasSetPitch(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t pitch = (int32_t)A2;
    if (e) return e;
    if (pitch < SAS_PITCH_MIN || pitch > SAS_PITCH_MAX) return SAS_ERROR_PITCH_VAL;
    v->pitch = pitch; return 0;
}

static uint32_t h_SasSetVolume(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t l = (int32_t)A2, r = (int32_t)A3;
    int32_t el = (int32_t)stack_arg(s, 0), er = (int32_t)stack_arg(s, 1);
    if (e) return e;
    if (l < 0 || l > SAS_VOLUME_MAX || r < 0 || r > SAS_VOLUME_MAX ||
        el < 0 || el > SAS_VOLUME_MAX || er < 0 || er > SAS_VOLUME_MAX)
        return SAS_ERROR_VOLUME_VAL;
    v->voll = l; v->volr = r; v->effect_l = el; v->effect_r = er; return 0;
}

static uint32_t h_SasSetADSR(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    uint32_t mask = A2;
    int32_t a = (int32_t)A3, d = (int32_t)stack_arg(s, 0);
    int32_t sustain = (int32_t)stack_arg(s, 1), r = (int32_t)stack_arg(s, 2);
    if (e) return e;
    if (mask & ~SAS_ADSR_ALL) return SAS_ERROR_ADSR_VAL;
    if ((mask & SAS_ADSR_ATTACK && a < 0) || (mask & SAS_ADSR_DECAY && d < 0) ||
        (mask & SAS_ADSR_SUSTAIN && sustain < 0) || (mask & SAS_ADSR_RELEASE && r < 0))
        return SAS_ERROR_ADSR_VAL;
    if (mask & SAS_ADSR_ATTACK) v->adsr_a = a;
    if (mask & SAS_ADSR_DECAY) v->adsr_d = d;
    if (mask & SAS_ADSR_SUSTAIN) v->adsr_s = sustain;
    if (mask & SAS_ADSR_RELEASE) v->adsr_r = r;
    return 0;
}

static uint32_t h_SasSetADSRmode(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    uint32_t mask = A2;
    int32_t a = (int32_t)A3, d = (int32_t)stack_arg(s, 0);
    int32_t sustain = (int32_t)stack_arg(s, 1), r = (int32_t)stack_arg(s, 2);
    if (e) return e;
    if (mask & ~SAS_ADSR_ALL) return SAS_ERROR_ADSR_MODE;
    if ((mask & SAS_ADSR_ATTACK && (a < 0 || a > 5)) ||
        (mask & SAS_ADSR_DECAY && (d < 0 || d > 5)) ||
        (mask & SAS_ADSR_SUSTAIN && (sustain < 0 || sustain > 5)) ||
        (mask & SAS_ADSR_RELEASE && (r < 0 || r > 5))) return SAS_ERROR_ADSR_MODE;
    if (mask & SAS_ADSR_ATTACK) v->mode_a = a;
    if (mask & SAS_ADSR_DECAY) v->mode_d = d;
    if (mask & SAS_ADSR_SUSTAIN) v->mode_s = sustain;
    if (mask & SAS_ADSR_RELEASE) v->mode_r = r;
    return 0;
}

static uint32_t h_SasSetSimpleADSR(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    if (e) return e;
    if ((A3 >> 13) & 1u) return SAS_ERROR_ADSR_MODE;
    v->simple_adsr1 = A2; v->simple_adsr2 = A3;
    v->adsr_a = (int)(A2 & 0xffffu); v->adsr_r = (int)(A3 & 0xffffu);
    v->adsr_d = 0; v->adsr_s = SAS_ENVELOPE_MAX;
    return 0;
}

static uint32_t h_SasSetSL(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t level = (int32_t)A2;
    if (e) return e;
    if (level < 0 || level > SAS_ENVELOPE_MAX) return SAS_ERROR_ADSR_VAL;
    v->sustain_level = level; return 0;
}

static uint32_t h_SasSetNoise(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    int32_t freq = (int32_t)A2;
    if (e) return e;
    if (freq < 0 || freq > SAS_NOISE_FREQ_MAX) return SAS_ERROR_NOISE_CLOCK;
    v->on = 0; v->type = SAS_VOICE_NOISE; v->noise = freq;
    v->vag = v->vag_size = v->pos = 0; v->pcm = v->pcm_samples = v->pcm_pos = 0;
    v->pcm_loop_start = -1; v->loop_requested = 0; v->loop_start = -1; v->bufn = v->bufi = 0;
    return 0;
}

/* The flags are the snapshot refreshed by the last __sceSasCore / __sceSasCoreWithMix cycle
 * (all voices off at init), as pspsascore.h specifies. */
static uint32_t h_SasGetEndFlag(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    return s_sas_core.end_flags;
}

static uint32_t h_SasSetKeyOn(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    if (e) return e;
    if (v->paused || v->type == SAS_VOICE_OFF ||
        (v->type == SAS_VOICE_VAG && (!v->vag || v->vag_size < 16u)) ||
        (v->type == SAS_VOICE_PCM && (!v->pcm || !v->pcm_samples))) return SAS_ERROR_INVALID_STATE;
    v->pos = 0; v->pcm_pos = 0; v->hist1 = v->hist2 = 0; v->bufn = v->bufi = 0; v->frac = 0;
    if (!v->voll && !v->volr) { v->voll = SAS_VOLUME_MAX; v->volr = SAS_VOLUME_MAX; }
    v->env_level = 0; v->env_phase = 0; v->on = 1;
    if (sas_log_on()) {
        static uint32_t calls; calls++;
        fprintf(stderr, "SAS_KEYON: call=%u vbl=%u voice=%u type=%d vag=0x%08x size=%u voll=%d volr=%d on=%d\n",
                calls, s_vcount, A1, v->type, v->vag, v->vag_size, v->voll, v->volr, v->on);
    }
    return 0;
}

static uint32_t h_SasSetKeyOff(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    if (e) return e;
    if (v->on) v->env_phase = 3;
    return 0;
}

static uint32_t h_SasGetEnvelopeHeight(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    return e ? e : (uint32_t)v->env_level;
}

static uint32_t h_SasGetAllEnvelopeHeights(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    if (!A1 || !sr_guest_span_writable(A1, SAS_VOICES * 4u)) return SAS_ERROR_ADDRESS;
    for (int i = 0; i < SAS_VOICES; i++) MEM_W32(A1 + (uint32_t)i * 4u, (uint32_t)s_sasv[i].env_level);
    return 0;
}

static uint32_t h_SasGetPauseFlag(CpuState *s) {
    uint32_t e = sas_require_core(A0), m = 0;
    if (e) return e;
    for (int i = 0; i < s_sas_max_voices; i++) if (s_sasv[i].paused) m |= 1u << i;
    return m;
}

static uint32_t h_SasSetPause(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    for (int i = 0; i < s_sas_max_voices; i++)
        if (A1 & (1u << i)) s_sasv[i].paused = A2 != 0;
    return 0;
}

static uint32_t h_SasSetGrain(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t grain = (int32_t)A1;
    if (e) return e;
    if (!sas_grain_valid(grain)) return SAS_ERROR_ADPCM_SIZE;
    s_sas_core.grain = grain; return 0;
}

static uint32_t h_SasGetGrain(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    return e ? e : (uint32_t)s_sas_core.grain;
}

static uint32_t h_SasSetOutputmode(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    if (A1 > 1u) return SAS_ERROR_INVALID_STATE;
    s_sas_core.output_mode = (int)A1; return 0;
}

static uint32_t h_SasGetOutputmode(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    return e ? e : (uint32_t)s_sas_core.output_mode;
}

static uint32_t h_SasRevType(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t type = (int32_t)A1;
    if (e) return e;
    if (type < -1 || type > 8) return SAS_ERROR_FX_TYPE;
    s_sas_core.effect_type = type; return 0;
}

static uint32_t h_SasRevParam(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t delay = (int32_t)A1, feedback = (int32_t)A2;
    if (e) return e;
    if (delay < 0 || delay > 128) return SAS_ERROR_FX_DELAY;
    if (feedback < 0 || feedback > 128) return SAS_ERROR_FX_FEEDBACK;
    s_sas_core.effect_delay = delay; s_sas_core.effect_feedback = feedback; return 0;
}

static uint32_t h_SasRevEVOL(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t l = (int32_t)A1, r = (int32_t)A2;
    if (e) return e;
    if (l < 0 || l > SAS_VOLUME_MAX || r < 0 || r > SAS_VOLUME_MAX) return SAS_ERROR_FX_VOLUME_VAL;
    s_sas_core.effect_l = l; s_sas_core.effect_r = r; return 0;
}

static uint32_t h_SasRevVON(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    s_sas_core.effect_dry = A1 != 0; s_sas_core.effect_wet = A2 != 0; return 0;
}

static uint32_t h_SasCore(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    if (e) return e;
    e = sas_output_validate(A1, 0);
    if (e) return e;
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sas_mix_stateful(A1, 0, SAS_VOLUME_MAX, SAS_VOLUME_MAX); return 0;
}

static uint32_t h_SasCoreWithMix(CpuState *s) {
    uint32_t e = sas_require_core(A0);
    int32_t left_gain = (int32_t)A2, right_gain = (int32_t)A3;
    if (e) return e;
    if (left_gain < 0 || left_gain > SAS_VOLUME_MAX || right_gain < 0 || right_gain > SAS_VOLUME_MAX)
        return SAS_ERROR_VOLUME_VAL;
    e = sas_output_validate(A1, 1);
    if (e) return e;
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sas_mix_stateful(A1, 1, left_gain, right_gain); return 0;
}

/* The public API exposes triangular/square and ATRAC3 voices, but the current runtime has
 * no source-owned implementation for those codecs. Refuse them after validating the core
 * and voice instead of routing them through h_ok and claiming success. */
static uint32_t h_SasUnsupportedVoice(CpuState *s) {
    SasVoice *v; uint32_t e = sas_voice_for(A0, A1, &v);
    (void)v;
    return e ? e : SAS_ERROR_INVALID_STATE;
}

/* sceSasCore: stateful SAS registrations.  This helper is called outside the selftest
 * gate, exactly like the sceAtrac family, so the executable HLE harness dispatches the
 * same registrations the game does instead of carrying a second, drifting test mapping. */
void hle_register_sas_handlers(void) {
    sr_hle_register(0x68a46b95, "__sceSasGetEndFlag", h_SasGetEndFlag);
    sr_hle_register(0xa3589d81, "__sceSasCore", h_SasCore);
    sr_hle_register(0x50a14dfc, "__sceSasCoreWithMix", h_SasCoreWithMix);
    sr_hle_register(0x76f01aca, "__sceSasSetKeyOn", h_SasSetKeyOn);
    sr_hle_register(0xa0cf2fa4, "__sceSasSetKeyOff", h_SasSetKeyOff);
    sr_hle_register(0x42778a9f, "__sceSasInit", h_SasInit);
    sr_hle_register(0x99944089, "__sceSasSetVoice", h_SasSetVoice);
    sr_hle_register(0xad84d37f, "__sceSasSetPitch", h_SasSetPitch);
    sr_hle_register(0x440ca7d8, "__sceSasSetVolume", h_SasSetVolume);
    sr_hle_register(0x019b25eb, "__sceSasSetADSR", h_SasSetADSR);
    sr_hle_register(0x9ec3676a, "__sceSasSetADSRmode", h_SasSetADSRmode);
    sr_hle_register(0x33d4ab37, "__sceSasRevType", h_SasRevType);
    sr_hle_register(0x74ae582a, "__sceSasGetEnvelopeHeight", h_SasGetEnvelopeHeight);
    sr_hle_register(0x267a6dd2, "__sceSasRevParam", h_SasRevParam);
    sr_hle_register(0x2c8e6ab3, "__sceSasGetPauseFlag", h_SasGetPauseFlag);
    sr_hle_register(0x5f9529f6, "__sceSasSetSL", h_SasSetSL);
    sr_hle_register(0x787d04d5, "__sceSasSetPause", h_SasSetPause);
    sr_hle_register(0xb7660a23, "__sceSasSetNoise", h_SasSetNoise);
    sr_hle_register(0xcbcd4f79, "__sceSasSetSimpleADSR", h_SasSetSimpleADSR);
    sr_hle_register(0xd5a229c9, "__sceSasRevEVOL", h_SasRevEVOL);
    sr_hle_register(0xf983b186, "__sceSasRevVON", h_SasRevVON);
    sr_hle_register(0xe175ef66, "__sceSasGetOutputmode", h_SasGetOutputmode);
    sr_hle_register(0xe855bf76, "__sceSasSetOutputmode", h_SasSetOutputmode);
    sr_hle_register(0xd1e0a01e, "__sceSasSetGrain", h_SasSetGrain);
    sr_hle_register(0xbd11b7c2, "__sceSasGetGrain", h_SasGetGrain);
    sr_hle_register(0xd5ebbbcd, "__sceSasSetSteepWave", h_SasUnsupportedVoice);
    sr_hle_register(0xa232cbe6, "__sceSasSetTrianglarWave", h_SasUnsupportedVoice);
    sr_hle_register(0xe1cd9561, "__sceSasSetVoicePCM", h_SasSetVoicePCM);
    sr_hle_register(0x4aa9ead6, "__sceSasSetVoiceATRAC3", h_SasUnsupportedVoice);
    sr_hle_register(0x7497ea85, "__sceSasConcatenateATRAC3", h_SasUnsupportedVoice);
    sr_hle_register(0xf6107f00, "__sceSasUnsetATRAC3", h_SasUnsupportedVoice);
    sr_hle_register(0x07f58c24, "__sceSasGetAllEnvelopeHeights", h_SasGetAllEnvelopeHeights);
}
