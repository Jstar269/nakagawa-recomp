// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Video: sceMpeg and scePsmf (the PSMF player). Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

#define PSMF_STATUS_INIT 1u
#define PSMF_STATUS_STANDBY 2u
#define PSMF_STATUS_PLAYING 4u
#define PSMF_STATUS_FINISHED 0x200u
#define PSMF_ERR_STATUS 0x80616001u
#define PSMF_ERR_STREAM 0x80616003u
#define PSMF_ERR_BUFSIZE 0x80616005u
#define PSMF_ERR_CONFIG 0x80616006u
#define PSMF_ERR_PARAM 0x80616008u
#define PSMF_ERR_NO_DATA 0x8061600cu
/* Kernel-class errors the reference player returns from the video getter; they are not
 * scePsmfPlayer codes (PPSSPP Core/HLE/ErrorCodes.h). */
#define PSMF_ERR_INVALID_POINTER 0x80000103u
#define PSMF_ERR_INVALID_VALUE 0x800001feu
#define PSMF_ERR_PRIV_REQUIRED 0x80000023u
/* scePsmfPlayerStart playMode values (PSMF_PLAYER_MODE_*). */
#define PSMF_PLAY_MODE_PAUSE 3u
#define PSMF_PLAY_MODE_FORWARD 4
#define PSMF_PLAY_MODE_REWIND 5
/* Pictures the reference player always reports as "not yet" before the first frame.  Guest
 * middleware is written against this: several titles depend on the first calls failing. */
#define PSMF_WARMUP_FRAMES 3
/* PsmfPlayerData (scePsmfPlayerStart's second argument): six words. */
#define PSMF_PLAYER_DATA_BYTES 24u
/* scePsmfPlayerSetTempBuf refuses a buffer smaller than this. */
#define PSMF_TEMP_BUF_MIN_BYTES 0x10000u
/* PsmfPlayerData codec fields: AVC video and ATRAC3+ audio. */
#define PSMF_VIDEO_CODEC_AVC 0xeu
#define PSMF_AUDIO_CODEC_ATRAC3PLUS 0xfu
/* playerVersion of a PSMF without entry points (the BASIC player). */
#define PSMF_PLAYER_VERSION_BASIC 1u
/* How long each call keeps the calling thread, in microseconds. */
#define PSMF_SET_PSMF_DELAY_US 3100
#define PSMF_STOP_DELAY_US 3000
#define PSMF_VIDEO_DATA_DELAY_US 3000
#define PSMF_AUDIO_RETRY_DELAY_US 10000
#define PSMF_AUDIO_DATA_DELAY_US 30000
#define PSMF_CREATE_DELETE_DELAY_US 20000
/* scePsmfPlayerCreate: the smallest player buffer it accepts, and its thread-priority range. */
#define PSMF_PLAYER_BUFFER_MIN_BYTES 0x00285800u
#define PSMF_PLAYER_PRIORITY_MIN 0x10u
#define PSMF_PLAYER_PRIORITY_LIMIT 0x6eu
SrPsmfPlayer s_psmf_players[4];
/* scePsmfPlayer activity counters. The no-frame watchdog reports the sceMpeg counters,
 * which are a different library; without these there is no evidence for or against the
 * player path being involved in a no-new-frame stretch. File-local: no other translation
 * unit consumes them (the watchdog that prints them lives in this file). */
unsigned long s_psmf_calls = 0, s_psmf_getvideo = 0, s_psmf_getaudio = 0;
static SrPsmfPlayer *psmf_slot(uint32_t guest, int create) {
    SrPsmfPlayer *freeSlot = NULL;
    for (size_t i = 0; i < sizeof(s_psmf_players) / sizeof(s_psmf_players[0]); i++) {
        if (s_psmf_players[i].used && s_psmf_players[i].guest == guest) return &s_psmf_players[i];
        if (!s_psmf_players[i].used && !freeSlot) freeSlot = &s_psmf_players[i];
    }
    if (create && guest && freeSlot) { memset(freeSlot, 0, sizeof(*freeSlot)); freeSlot->used=1; freeSlot->guest=guest; freeSlot->h264=-1; return freeSlot; }
    return NULL;
}
/* A ScePsmfPlayer is one guest word: Create stores the player's handle in it and Delete clears
 * it, and every other call names the player by that word's value, not by where the word is.  A
 * title may copy the word into its own thread structures and pass the copy, which on a PSP names
 * the same player.  The handle stored is the address Create was given, as PPSSPP
 * Core/HLE/scePsmf.cpp (3f40ddc8) does; what the PSP library stores there is unmeasured, and no
 * title is known to read it as anything but a handle. */
static SrPsmfPlayer *psmf_find(uint32_t guest, int create) {
    s_psmf_calls++;   /* every scePsmfPlayer handler resolves its control block here */
    if (create) return psmf_slot(guest, create);
    if (!guest || !sr_guest_span_readable(guest, 4u)) return NULL;
    uint32_t handle = MEM_R32(guest);
    return handle ? psmf_slot(handle, 0) : NULL;
}
static void psmf_flush(SrPsmfPlayer *p) {
    if (!p) return;
    for (unsigned t = 0; t < PSMF_TRACKS; t++) for (unsigned stage = 0; stage < PSMF_Q_STAGES; stage++) {
        SrPsmfQueue *q = &p->q[t][stage];
        for (unsigned i = 0; i < PSMF_Q_DEPTH; i++) { free(q->slot[i].hostData); q->slot[i].hostData = NULL; }
        memset(q, 0, sizeof(*q));
    }
}
static int psmf_iso_read(void *opaque, uint64_t offset, void *dst, uint32_t capacity, uint32_t *got) {
    SrPsmfPlayer *p = (SrPsmfPlayer *)opaque;
    if (!p || !dst || !got || offset >= p->source.size || offset > UINT32_MAX) { if (got) *got = 0; return SR_PSMF_SOURCE_EOF; }
    uint32_t remain = p->source.size - (uint32_t)offset;
    uint32_t want = capacity < remain ? capacity : remain;
    int n = iso_read(p->source.lba, (uint32_t)offset, dst, want);
    if (n < 0) { *got = 0; return SR_PSMF_SOURCE_ERROR; }
    *got = (uint32_t)n;
    return n ? SR_PSMF_SOURCE_DATA : SR_PSMF_SOURCE_EOF;
}
/* PSMF presentation times are six-byte big-endian values (the reference reader loads
 * buf[0]..buf[5] the same way).  The five-byte form silently reports 351 instead of 90000
 * for this title's movies, which then skews every AU timestamp the producer normalizes. */
static int64_t psmf_be_timestamp(const uint8_t *p) {
    return ((int64_t)p[0] << 36) | ((int64_t)p[1] << 32) | ((int64_t)p[2] << 24) |
           ((int64_t)p[3] << 16) | ((int64_t)p[4] << 8) | (int64_t)p[5];
}
static int psmf_log_on(void) { static int v = -1; if (v < 0) v = getenv("SR_MPEGLOG") ? 1 : 0; return v; }
void psmf_av_exit_report(void) { psmf_av_final("exit"); }

/* Bytes of a level-0 texture: bufw texels per row, h rows, at the PSM's texel size.  0 for
 * the DXT formats, whose block layout is not a picture a movie decoder writes. */
uint32_t psmf_av_texture_bytes(uint32_t bufw, uint32_t h, uint32_t fmt) {
    static const uint8_t bits[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    uint32_t texels;
    if (fmt > 7u || !sr_size_mul_ok(bufw, h, &texels) || texels > 0x08000000u) return 0;
    return (uint32_t)(((uint64_t)texels * bits[fmt] + 7u) / 8u);
}

#ifdef SR_MPEG_MEDIA_SELFTEST
/* Issue #279: the media fixture asserts the exact published counter line -- the same bytes
 * a run writes on stderr -- without scraping a shared stream.  psmf_log_summary() records
 * the last line it printed here and counts them, so a test can tell a fresh line from a
 * stale one.  Test-only; the production build carries neither the buffer nor the count. */
#define PSMF_SUMMARY_LINE_CAPACITY 4096u
#define PSMF_REJECTION_LINE_CAPACITY 512u
static char s_psmf_last_summary[PSMF_SUMMARY_LINE_CAPACITY];
static uint32_t s_psmf_summary_count;
static char s_psmf_last_rejection[PSMF_REJECTION_LINE_CAPACITY];
const char *sr_hle_test_psmf_last_summary(void) { return s_psmf_last_summary; }
uint32_t sr_hle_test_psmf_summary_count(void) { return s_psmf_summary_count; }
#else
#define PSMF_SUMMARY_LINE_CAPACITY 4096u
#endif

/* The bounded SR_MPEGLOG counter line, printed as one summary.  Every clock on the line
 * names the stage it is measured at: vpts/apts are pipeline clocks -- the last picture
 * submitted to the decoder and the last audio block staged for delivery -- while vts/ats
 * are the two presentation points a guest actually receives (the displaypts written into
 * the getter's video struct, and the pts of the audio block copied to the guest).  avgap
 * is therefore the A/V separation measured at comparable presentation points, and vlead
 * the video pipeline lead (submitted minus delivered, in ticks) -- both on the same
 * 90 kHz timeline, which is what separates queue/interleave distance from synchronization
 * error in a run.  -1 means the pair has not been measured yet.  vlost is the number of
 * presentation times the ring was forced to discard; the submit gate in psmf_video_pump()
 * keeps it at zero, so vlost > 0 on a run means that rule was violated. */
static int psmf_format_summary(char *line, size_t line_size, unsigned vbl,
                               const SrPsmfProducerStats *st,
                               const SrPsmfPlayer *p) {
    int64_t avgap = (p->displayPtsValid && p->audioDisplayPtsValid)
                        ? p->displayPts - p->audioDisplayPts : -1;
    int64_t vlead = (p->videoClockValid && p->displayPtsValid)
                        ? p->videoClock - p->displayPts : -1;
    return snprintf(line, line_size, "PSMF producer vb=%u bytes=%llu packs=%llu pes=%llu video_pes=%llu audio_pes=%llu"
                    " video_aus=%llu audio_aus=%llu no_pts=%llu dts_pes=%llu resync=%llu qv=%u qa=%u eof=%d failed=%d"
                    " vsub=%llu vdec=%llu vout=%llu vhold=%llu veos=%llu vrej=%llu"
                    " asub=%llu adec=%llu aout=%llu ahold=%llu aeos=%llu arej=%llu"
                    " arejf=%u aupm=%u"
                    " fail_at=%llu frames=%u audio_blocks=%u verr=%u aerr=%u"
                    " vpts=%lld apts=%lld vts=%lld ats=%lld avgap=%lld vlead=%lld vclk=%d drained=%d vlost=%llu\n",
                    vbl,
                    (unsigned long long)st->bytes_read, (unsigned long long)st->packs,
                    (unsigned long long)st->pes_packets, (unsigned long long)st->video_pes,
                    (unsigned long long)st->audio_pes, (unsigned long long)st->video_aus,
                    (unsigned long long)st->audio_aus, (unsigned long long)st->aus_without_pts,
                    (unsigned long long)st->pes_with_dts,
                    (unsigned long long)st->audio_resync_bytes,
                    p->q[PSMF_TRACK_VIDEO][PSMF_Q_AU].count,
                    p->q[PSMF_TRACK_AUDIO][PSMF_Q_AU].count, st->eof, st->failed,
                    (unsigned long long)st->video_submitted,
                    (unsigned long long)st->video_decoded,
                    (unsigned long long)st->video_delivered,
                    (unsigned long long)st->video_warmup_held,
                    (unsigned long long)st->video_eos_drained,
                    (unsigned long long)st->video_rejected,
                    (unsigned long long)st->audio_submitted,
                    (unsigned long long)st->audio_decoded,
                    (unsigned long long)st->audio_delivered,
                    (unsigned long long)st->audio_warmup_held,
                    (unsigned long long)st->audio_eos_drained,
                    (unsigned long long)st->audio_rejected,
                    p->audioFormatRejects, p->audioUpmixBlocks,
                    (unsigned long long)st->fail_offset, p->videoFramesOut, p->audioBlocksOut,
                    p->videoErrors, p->audioErrors,
                    (long long)p->videoClock, (long long)p->audioClock,
                    (long long)p->displayPts, (long long)p->audioDisplayPts,
                    (long long)avgap, (long long)vlead,
                    p->videoClockValid, p->videoDrained,
                    (unsigned long long)p->auPtsLost);
}

static int psmf_summary_fits(int formatted, size_t capacity) {
    return formatted >= 0 && (size_t)formatted < capacity;
}

#ifdef SR_MPEG_MEDIA_SELFTEST
int sr_hle_test_psmf_summary_rejects_truncation(void) {
    SrPsmfProducerStats st;
    SrPsmfPlayer p;
    char tiny[16];
    memset(&st, 0, sizeof(st));
    memset(&p, 0, sizeof(p));
    int formatted = psmf_format_summary(tiny, sizeof(tiny), 0u, &st, &p);
    return !psmf_summary_fits(formatted, sizeof(tiny));
}
#endif

static void psmf_log_summary(SrPsmfPlayer *p) {
    if (!p || !psmf_log_on()) return;
    SrPsmfProducerStats st; sr_psmf_producer_stats(p->producer, &st);
    /* Host-stack line buffer: runs on native host thread stack (1 MB on Win32, 2+ MB on POSIX),
     * not guest MIPS stack. Shallow leaf call; reentrant across concurrent player instances
     * without static TLS overhead or per-player struct bloat when SR_MPEGLOG is off. */
    char line[PSMF_SUMMARY_LINE_CAPACITY];
    int formatted = psmf_format_summary(line, sizeof(line), sr_audio_vbl(), &st, p);
    if (!psmf_summary_fits(formatted, sizeof(line))) {
        fprintf(stderr, "PSMF_COUNTER_SUMMARY_TRUNCATED: required=%d capacity=%zu\n",
                formatted, sizeof(line));
        return;
    }
    fputs(line, stderr);
#ifdef SR_MPEG_MEDIA_SELFTEST
    memcpy(s_psmf_last_summary, line, (size_t)formatted + 1u);
    s_psmf_summary_count++;
#endif
}

/* Sampling for the counter line: the first few produce calls, then a regular interval,
 * plus one line per 16 delivered audio blocks so the presentation-boundary gap is
 * observable *through* a run instead of only at its start. */
static void psmf_log_tick(SrPsmfPlayer *p) {
    if (!p || !psmf_log_on()) return;
    int due = p->logTickCalls++ < 8u || (p->logTickCalls & 0xffu) == 0;
    if (p->audioBlocksOut / 16u != p->logTickBlocks / 16u) {
        p->logTickBlocks = p->audioBlocksOut;
        due = 1;
    }
    if (due) psmf_log_summary(p);
}

#ifdef SR_MPEG_MEDIA_SELFTEST
void sr_hle_test_psmf_log_tick(uint32_t guest) {
    SrPsmfPlayer *p = psmf_find(guest, 1);
    if (p) psmf_log_tick(p);
}

void sr_hle_test_psmf_log_release(uint32_t guest) {
    SrPsmfPlayer *p = psmf_slot(guest, 0);
    if (!p) return;
    psmf_flush(p);
    if (p->producer) sr_psmf_producer_close(p->producer);
    memset(p, 0, sizeof(*p));
    p->h264 = -1;
}
#endif

/* A rejected stream is a product boundary, not a debug event.  The producer fails closed
 * and never reaches EOF, so without a named boundary the consumer only sees the picture
 * freeze with nothing in the log: the one thing this runtime must not do.  The message is
 * always on (SR_MPEGLOG is for counters, not for the reason playback stopped), once per
 * stream, and it states the guest-visible consequence instead of hiding it.  The same edge
 * goes to the flight recorder so a bring-up or library sweep can classify the run from a
 * structured record rather than by scraping text. */
static void psmf_name_rejection(SrPsmfPlayer *p) {
    if (!p || !p->producer || p->rejectNamed) return;
    if (!sr_psmf_producer_failed(p->producer)) return;
    p->rejectNamed = 1;
    const char *reason = sr_psmf_producer_fail_reason(p->producer);
    SrPsmfProducerStats st; sr_psmf_producer_stats(p->producer, &st);
#ifdef SR_MPEG_MEDIA_SELFTEST
    int formatted = snprintf(s_psmf_last_rejection, sizeof(s_psmf_last_rejection),
            "PSMF_CONTRACT: scePsmfPlayer: stream rejected by the demuxer: %s "
            "at source offset %llu; no further access unit is decoded and the player keeps "
            "its current status; in the works (#286)\n",
            reason ? reason : "unspecified", (unsigned long long)st.fail_offset);
    if (formatted >= 0 && (size_t)formatted < sizeof(s_psmf_last_rejection))
        fputs(s_psmf_last_rejection, stderr);
    else {
        s_psmf_last_rejection[0] = '\0';
        fprintf(stderr, "PSMF_CONTRACT: scePsmfPlayer: stream rejected by the demuxer\n");
    }
#else
    fprintf(stderr, "PSMF_CONTRACT: scePsmfPlayer: stream rejected by the demuxer: %s "
            "at source offset %llu; no further access unit is decoded and the player keeps "
            "its current status; in the works (#286)\n",
            reason ? reason : "unspecified", (unsigned long long)st.fail_offset);
#endif
    SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_MEDIA, SR_FLIGHT_KIND_MEDIA_STREAM_REJECTED,
                           (uint32_t)st.fail_offset,
                           (uint32_t)(st.fail_offset >> 32), 0u, 0u);
}

/* Move access units from the producer into the player's per-track queues.  The producer's
 * own bounded queue is the only place work is throttled: an AU is never dropped here, a full
 * player queue simply leaves the next access unit upstream until the decoder drains it. */
static void psmf_produce(SrPsmfPlayer *p) {
    if (!p || !p->producer) return;
    sr_psmf_producer_pump(p->producer, 16);
    psmf_name_rejection(p);
    for (unsigned track = 0; track < PSMF_TRACKS; track++) {
        SrPsmfQueue *q = &p->q[track][PSMF_Q_AU];
        while (q->count < PSMF_Q_DEPTH) {
            SrPsmfAu au;
            if (!sr_psmf_producer_pop(p->producer, (SrPsmfAuKind)track, &au)) break;
            SrPsmfQueueSlot *slot = &q->slot[q->tail];
            slot->state = 1;
            slot->flags = (uint8_t)(au.has_pts ? 1u : 0u);   /* presentation time known? */
            slot->bytes = au.size;
            slot->pts = au.pts;
            slot->dts = au.dts;
            slot->hostData = au.data; au.data = NULL;
            q->tail = (q->tail + 1u) % PSMF_Q_DEPTH; q->count++;
            sr_psmf_au_release(&au);
        }
    }
    psmf_log_tick(p);
}

#ifdef SR_MPEG_MEDIA_SELFTEST
/* Drive the production PSMF producer-to-player queue boundary without a decoder or retail
 * media.  A zero expected_reason means valid input; otherwise the failed parser reason and
 * the exact named PSMF_CONTRACT refusal must both be observed. */
int sr_hle_test_psmf_stream_contract(const uint8_t *data, uint32_t size,
                                     uint32_t fragment_bytes,
                                     const char *expected_reason,
                                     uint32_t minimum_video_aus) {
    if ((!data && size) || !fragment_bytes) return 0;
    SrPsmfPlayer player;
    memset(&player, 0, sizeof(player));
    player.h264 = -1;
    player.producer = sr_psmf_producer_open_stream(0);
    if (!player.producer) return 0;
    s_psmf_last_rejection[0] = '\0';

    int feed_ok = 1;
    for (uint32_t at = 0; at < size;) {
        uint32_t amount = size - at;
        if (amount > fragment_bytes) amount = fragment_bytes;
        if (sr_psmf_producer_feed_stream(player.producer, data + at, amount, 0) != 0) {
            feed_ok = 0;
            break;
        }
        at += amount;
        psmf_produce(&player);
    }
    if (feed_ok && !sr_psmf_producer_failed(player.producer) &&
        sr_psmf_producer_feed_stream(player.producer, NULL, 0, 1) != 0)
        feed_ok = 0;
    if (feed_ok) psmf_produce(&player);

    int failed = sr_psmf_producer_failed(player.producer);
    int eof = sr_psmf_producer_eof(player.producer);
    const char *reason = sr_psmf_producer_fail_reason(player.producer);
    uint32_t video_aus = player.q[PSMF_TRACK_VIDEO][PSMF_Q_AU].count;
    int passed;
    if (expected_reason) {
        passed = feed_ok && failed && !eof && player.rejectNamed && video_aus == 0 && reason &&
                 strcmp(reason, expected_reason) == 0 &&
                 strncmp(s_psmf_last_rejection, "PSMF_CONTRACT:",
                         sizeof("PSMF_CONTRACT:") - 1u) == 0 &&
                 strstr(s_psmf_last_rejection, expected_reason) != NULL;
    } else {
        passed = feed_ok && !failed && eof && !player.rejectNamed &&
                 video_aus >= minimum_video_aus && s_psmf_last_rejection[0] == '\0';
    }
    psmf_flush(&player);
    sr_psmf_producer_close(player.producer);
    return passed;
}
#endif

/* ---- decoder integration ------------------------------------------------------------ */

/* Release every decoder-owned object and reset the presentation clocks.  Called on start,
 * stop, loop restart, and teardown so no decoded output survives a stream discontinuity. */
static void psmf_media_reset(SrPsmfPlayer *p) {
    if (!p) return;
    if (p->h264 >= 0) {
        if (sr_h264_reset(p->h264) < 0) {
            sr_h264_destroy(p->h264);
            p->h264 = -1;
        }
    }
    p->h264Unavailable = 0;
    atrac3p_bridge_destroy(p->atrac);
    p->atrac = NULL;
    p->atracChannels = p->atracAlign = 0;
    free(p->audioPcm);
    p->audioPcm = NULL;
    p->audioPcmSampleCount = p->audioPcmValid = 0;
    p->audioPcmPts = 0; p->audioPcmPtsValid = 0;
    p->audioDisplayPts = 0; p->audioDisplayPtsValid = 0;
    p->videoClock = p->audioClock = 0;
    p->videoClockValid = p->audioClockValid = 0;
    p->auPtsHead = p->auPtsCount = 0; p->auPtsLost = 0;
    p->displayPts = 0; p->displayPtsValid = 0; p->videoDrained = 0;
    p->videoFramesOut = p->audioBlocksOut = p->videoErrors = p->audioErrors = 0;
    p->audioUpmixBlocks = p->audioFormatRejects = 0;
    /* Every caller resets or replaces the producer in the same breath, so the named
     * boundary goes with it: a fresh stream may be rejected, and may say so once. */
    p->rejectNamed = 0;
}

/* Give back everything a player acquired -- queued access units, the producer, the H.264
 * decoder instance and the audio decoder -- and free its slot.  Issue #279: Delete used to
 * reset the decoder and then forget its id, so every movie kept one of a backend's few
 * decoder instances (four for Media Foundation and FFmpeg) and the fifth movie of a session
 * got none. */
void psmf_player_release(SrPsmfPlayer *p) {
    if (!p) return;
    psmf_flush(p);
    if (p->producer) { sr_psmf_producer_close(p->producer); p->producer = NULL; }
    if (p->h264 >= 0) { sr_h264_destroy(p->h264); p->h264 = -1; }
    psmf_media_reset(p);
    memset(p, 0, sizeof(*p));
    p->h264 = -1;
}

/* Presentation time of one submitted picture.  A packet that carried a PTS sets the clock;
 * a packet that did not advances it by exactly one frame, and until the first PTS arrives the
 * picture's time stays unknown (-1) rather than invented. */
static void psmf_video_clock_push(SrPsmfPlayer *p, int has_pts, int64_t pts) {
    int64_t value = sr_psmf_pts_advance(&p->videoClock, &p->videoClockValid,
                                        has_pts, pts, PSMF_VIDEO_PTS_STEP);
    if ((uint32_t)p->auPtsCount < PSMF_OUT_PTS_CAPACITY) {
        p->auPts[(p->auPtsHead + p->auPtsCount) % PSMF_OUT_PTS_CAPACITY] = value;
        p->auPtsCount++;
    } else {
        p->auPts[p->auPtsHead] = value;
        p->auPtsHead = (p->auPtsHead + 1) % PSMF_OUT_PTS_CAPACITY;
        p->auPtsLost++;
    }
}

static uint32_t psmf_video_submit_limit(int producer_eof) {
    return producer_eof ? PSMF_OUT_PTS_CAPACITY : PSMF_OUT_PTS_RING;
}

/* Feed queued compressed video access units into the H.264 backend, bounded by the
 * presentation ring: a picture may only be submitted while the ring still has a slot for
 * its presentation time.  Without that bound the submit path (up to PSMF_Q_DEPTH pictures
 * per getter call) outruns the getter (one picture per call), the oldest pending entries
 * are overwritten, and the displaypts handed to the guest skips ahead of the picture
 * actually displayed -- a vpts/vts divergence with no stream-side explanation (#279).
 * With it, normal submitted-minus-delivered lead is bounded by PSMF_OUT_PTS_RING. At EOF,
 * the bounded producer/player tail may use the extra PSMF_OUT_PTS_EOS_TAIL entries so all
 * compressed input reaches the backend before EOS closes it. Every submitted picture's time
 * survives to its own getter call, and a guest that stops pulling throttles normal demux. */
static void psmf_video_pump(SrPsmfPlayer *p) {
    if (!p) return;
    SrPsmfQueue *q = &p->q[PSMF_TRACK_VIDEO][PSMF_Q_AU];
    uint32_t submit_limit = psmf_video_submit_limit(
        p->producer && sr_psmf_producer_eof(p->producer));
    while (q->count && (uint32_t)p->auPtsCount < submit_limit) {
        SrPsmfQueueSlot *slot = &q->slot[q->head];
        if (p->h264 < 0 && !p->h264Unavailable) {
            p->h264 = sr_h264_create();
            if (p->h264 < 0) {
                p->h264Unavailable = 1;
                fprintf(stderr, "%s\n", sr_h264_backend_unavailable_reason());
            }
        }
        if (p->h264 >= 0) {
            if (slot->hostData && slot->bytes) {
                sr_psmf_producer_media_stage(p->producer, SR_PSMF_AU_VIDEO,
                                             SR_PSMF_MEDIA_SUBMITTED);
                if (sr_h264_submit_au_timed(p->h264, (const uint8_t *)slot->hostData,
                                            slot->bytes, slot->pts,
                                            (slot->flags & 1u) != 0) < 0) {
                    p->videoErrors++;
                } else {
                    psmf_video_clock_push(p, (slot->flags & 1u) != 0, slot->pts);
                }
            } else {
                psmf_video_clock_push(p, (slot->flags & 1u) != 0, slot->pts);
            }
        }
        free(slot->hostData);
        slot->hostData = NULL;
        memset(slot, 0, sizeof(*slot));
        q->head = (q->head + 1u) % PSMF_Q_DEPTH; q->count--;
    }
}

/* Parser EOF can precede submission of producer-owned or player-owned tail AUs.  A backend's
 * first EOS call closes the decoder input; allow it only after both queues and the producer's
 * trailing video bytes have been handed to the backend. */
static int psmf_video_eos_gate(int producer_eof, uint32_t player_queue_count,
                               uint32_t producer_queue_depth, uint32_t producer_buffered) {
    return producer_eof && player_queue_count == 0u &&
           producer_queue_depth == 0u && producer_buffered == 0u;
}

static int psmf_video_eos_ready(SrPsmfPlayer *p) {
    if (!p || !p->producer) return 0;
    SrPsmfProducerStats st;
    sr_psmf_producer_stats(p->producer, &st);
    return psmf_video_eos_gate(sr_psmf_producer_eof(p->producer),
                               p->q[PSMF_TRACK_VIDEO][PSMF_Q_AU].count,
                               st.video_depth, st.video_buffered);
}

#ifdef SR_MPEG_MEDIA_SELFTEST
int sr_hle_test_psmf_video_eos_gate(int producer_eof, uint32_t player_queue_count,
                                    uint32_t producer_queue_depth,
                                    uint32_t producer_buffered) {
    return psmf_video_eos_gate(producer_eof, player_queue_count,
                               producer_queue_depth, producer_buffered);
}
uint32_t sr_hle_test_psmf_video_submit_limit(int producer_eof) {
    return psmf_video_submit_limit(producer_eof);
}
#endif

/* One decoded picture straight into the guest display buffer.  1 = written, 0 = nothing
 * ready yet, -1 = decoder failure. */
static int psmf_video_take(SrPsmfPlayer *p, uint32_t displaybuf, int bufw,
                           int pixelMode, int64_t *pts_out) {
    if (!p || p->h264 < 0) return 0;
    int eos = psmf_video_eos_ready(p);
    SrH264FrameTarget target;
    SrH264FrameInfo info;
    memset(&target, 0, sizeof(target));
    target.kind = SR_H264_TARGET_GUEST;
    target.guest_buffer = displaybuf;
    target.frame_width = bufw;
    target.pixel_mode = pixelMode;
    int r = eos ? sr_h264_drain(p->h264, &target, &info) :
                  sr_h264_frame_ex(p->h264, 0, &target, &info);
    if (r < 0) { p->videoErrors++; return -1; }
    if (r == 0) { if (eos) p->videoDrained = 1; return 0; }
    sr_psmf_producer_media_stage(p->producer, SR_PSMF_AU_VIDEO,
                                 SR_PSMF_MEDIA_DECODED);
    if (info.width <= 0 || info.height <= 0 || info.stride <= 0 ||
        info.delivered_format < SR_H264_PIXEL_5650 ||
        info.delivered_format > SR_H264_PIXEL_8888) {
        sr_psmf_producer_media_stage(p->producer, SR_PSMF_AU_VIDEO,
                                     SR_PSMF_MEDIA_REJECTED);
        p->videoErrors++;
        return -1;
    }
    int64_t value = -1;
    if (p->auPtsCount) {
        value = p->auPts[p->auPtsHead];
        p->auPtsHead = (p->auPtsHead + 1) % PSMF_OUT_PTS_CAPACITY;
        p->auPtsCount--;
    }
    if (value < 0) value = p->displayPts + PSMF_VIDEO_PTS_STEP;
    p->displayPts = value;
    p->displayPtsValid = 1;
    *pts_out = value;
    sr_psmf_producer_media_stage(p->producer, SR_PSMF_AU_VIDEO,
                                 eos ? SR_PSMF_MEDIA_EOS_DRAINED :
                                       SR_PSMF_MEDIA_DELIVERED);
    {
        /* Presentation-boundary measurement: remember where this picture now sits in guest
         * memory so the display present that shows it can be matched (diagnostics only). */
        SrGuestRectSpan span;
        if (bufw > 0 && sr_guest_rect_geometry(displaybuf, 0u, 0u, (uint32_t)bufw,
                                               (uint32_t)info.width, (uint32_t)info.height,
                                               info.delivered_format == SR_H264_PIXEL_8888 ? 4u : 2u,
                                               &span))
            psmf_av_delivered(PSMF_AV_VIDEO, span.first, span.total_bytes, value, 1);
        else
            psmf_av_delivered(PSMF_AV_VIDEO, displaybuf, 0u, value, 1);
    }
    p->videoFramesOut++;
    return 1;
}

/* Decode queued ATRAC access units until one guest block is staged.  Every frame comes from
 * a real decoder success; a malformed frame, a wrong declared channel count, or a decoder
 * error leaves the block unstaged (the getter then reports no data, never silence). */
static int psmf_audio_fill(SrPsmfPlayer *p) {
    if (!p) return 0;
    if (p->audioPcmValid) return 1;
    SrPsmfQueue *q = &p->q[PSMF_TRACK_AUDIO][PSMF_Q_AU];
    while (q->count && !p->audioPcmValid) {
        SrPsmfQueueSlot *slot = &q->slot[q->head];
        const uint8_t *frame = (const uint8_t *)slot->hostData;
        if (frame && slot->bytes > 8u && frame[0] == 0x0fu && frame[1] == 0xd0u) {
            uint32_t frame_size = (uint32_t)(((frame[2] & 3u) << 8) |
                                             ((uint32_t)frame[3] * 8u)) + 0x10u;
            if (frame_size == slot->bytes) {
                uint32_t data = frame_size - 8u;
                int channels = (int)p->audioChannels;
                if (channels < 1 || channels > (int)PSMF_AUDIO_MAX_CHANNELS) {
                    p->audioFormatRejects++;
                } else {
                    if (!p->atrac || p->atracAlign != (int)data || p->atracChannels != channels) {
                        atrac3p_bridge_destroy(p->atrac);
                        p->atrac = NULL;
                        if (atrac3p_bridge_create(channels, (int)data, &p->atrac) == 0) {
                            p->atracChannels = channels; p->atracAlign = (int)data;
                        } else {
                            p->atrac = NULL;
                        }
                    }
                    if (p->atrac && !p->audioPcm)
                        p->audioPcm = (int16_t *)malloc((size_t)PSMF_AUDIO_MAX_CHANNELS *
                                                        PSMF_AUDIO_SAMPLES * sizeof(int16_t));
                    if (p->atrac && p->audioPcm) {
                        int samples = 0;
                        sr_psmf_producer_media_stage(p->producer, SR_PSMF_AU_AUDIO,
                                                     SR_PSMF_MEDIA_SUBMITTED);
                        int rc = atrac3p_bridge_decode(p->atrac, frame + 8, (int)data,
                                                       p->audioPcm, &samples);
                        if (rc == 0) {
                            sr_psmf_producer_media_stage(p->producer, SR_PSMF_AU_AUDIO,
                                                         SR_PSMF_MEDIA_DECODED);
                            p->audioPcmSampleCount = 0;
                            if (samples != PSMF_AUDIO_SAMPLES) {
                                p->audioFormatRejects++;
                                sr_psmf_producer_media_stage(p->producer, SR_PSMF_AU_AUDIO,
                                                             SR_PSMF_MEDIA_REJECTED);
                            } else if (channels == 2) {
                                p->audioPcmSampleCount = samples;
                            } else if (channels == 1) {
                                /* The PSP block is stereo; a mono stream is duplicated into both
                                 * channels (what the reference player's resampler does).  The
                                 * samples are still real decoded output, not filler. */
                                for (int i = samples - 1; i >= 0; i--) {
                                    p->audioPcm[i * 2] = p->audioPcm[i];
                                    p->audioPcm[i * 2 + 1] = p->audioPcm[i];
                                }
                                p->audioPcmSampleCount = samples;
                                p->audioUpmixBlocks++;
                            } else {
                                p->audioFormatRejects++;
                                p->audioPcmSampleCount = 0;
                                sr_psmf_producer_media_stage(p->producer, SR_PSMF_AU_AUDIO,
                                                             SR_PSMF_MEDIA_REJECTED);
                            }
                            if (p->audioPcmSampleCount == PSMF_AUDIO_SAMPLES) {
                                sr_psmf_pts_advance(&p->audioClock, &p->audioClockValid,
                                                    (slot->flags & 1u) != 0, slot->pts,
                                                    PSMF_AUDIO_PTS_STEP);
                                p->audioPcmPtsValid = p->audioClockValid;
                                p->audioPcmPts = p->audioClock;
                                p->audioPcmValid = 1;
                            }
                        } else {
                            p->audioErrors++;
                        }
                    }
                }
            } else {
                p->audioFormatRejects++;      /* header size disagrees with the access unit */
            }
        } else {
            p->audioFormatRejects++;
        }
        free(slot->hostData);
        slot->hostData = NULL;
        memset(slot, 0, sizeof(*slot));
        q->head = (q->head + 1u) % PSMF_Q_DEPTH; q->count--;
    }
    return p->audioPcmValid;
}

/* The stream has given everything it has and nothing decoded is left pending. */
static int psmf_reached_end(SrPsmfPlayer *p) {
    if (!p || !p->producer || !sr_psmf_producer_eof(p->producer)) return 0;
    if (!psmf_video_eos_ready(p) || p->q[PSMF_TRACK_AUDIO][PSMF_Q_AU].count) return 0;
    SrPsmfProducerStats st;
    sr_psmf_producer_stats(p->producer, &st);
    if (st.audio_depth) return 0;
    if (p->audioPcmValid) return 0;
    /* Without a video backend the picture can never drain, so end is reported as soon as the
     * stream is consumed -- the same policy the sceMpeg path uses with a blank video output. */
    if (p->h264 >= 0 && !p->videoDrained) return 0;
    return 1;
}
static uint32_t psmf_be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t psmf_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
/* Copy the selected streams' declared geometry out of the header table.  The reference
 * player resolves width/height/channels from the *chosen* stream entry, not from the first
 * entry in the table, so a movie whose first video stream is not the one Start() asks for
 * still reports the right size. */
static void psmf_select_streams(SrPsmfPlayer *p, uint32_t videoNum, uint32_t audioNum) {
    if (!p) return;
    if (videoNum < p->videoStreams && videoNum < 128u) {
        p->videoWidth = (uint32_t)p->videoStreamDim[videoNum][0] * 16u;
        p->videoHeight = (uint32_t)p->videoStreamDim[videoNum][1] * 16u;
    }
    if (audioNum < p->audioStreams && audioNum < 128u) {
        p->audioChannels = p->audioStreamChannels[audioNum];
    } else {
        p->audioChannels = 0;
    }
}
static int psmf_parse_header(SrPsmfPlayer *p, const uint8_t *h, uint32_t n) {
    if (!p || !h || n < 0x82 || psmf_be32(h) != 0x50534d46u) return 0;
    uint32_t streams = psmf_be16(h + 0x80); if (streams > 128 || 0x82u + streams * 16u > n) return 0;
    p->streamOffset=psmf_be32(h+8); p->streamSize=psmf_be32(h+12);
    p->videoStreams=p->audioStreams=0; p->playerVersion=0;
    memset(p->videoStreamDim,0,sizeof(p->videoStreamDim)); memset(p->audioStreamChannels,0,sizeof(p->audioStreamChannels));
    for (uint32_t i=0;i<streams;i++) { const uint8_t *e=h+0x82u+i*16u;
        if ((e[0]&0xe0u)==0xe0u) {
            if (p->videoStreams<128u) { p->videoStreamDim[p->videoStreams][0]=e[12]; p->videoStreamDim[p->videoStreams][1]=e[13]; }
            p->videoStreams++;
            if(!psmf_be32(e+4)||!psmf_be32(e+8))p->playerVersion=1;
        } else if ((e[0]&0xf0u)==0xb0u || (e[0]&0xf0u)==0xf0u || e[0]==0xbdu) {
            if (p->audioStreams<128u) p->audioStreamChannels[p->audioStreams]=e[14];
            p->audioStreams++;
        } }
    if (!p->videoStreams) return 0;
    psmf_select_streams(p, 0, 0);
    /* 0x54 and 0x5a are the header's first and last presentation timestamps, both six-byte
     * fields read like the PES clock; the stream's total duration is their difference. */
    p->presentationBase = psmf_be_timestamp(h + 0x54);
    p->durationPts = psmf_be_timestamp(h + 0x5a) - p->presentationBase;
    p->currentPts = 0;
    return 1;
}
static uint32_t h_PsmfCreate(CpuState *s) {
    /* Parameters are checked before a slot is claimed, so a refused create keeps nothing;
     * creating over a live control block gives the old player's decoder and producer back
     * and starts a fresh player (issue #279). */
    s_psmf_calls++;
    SrPsmfPlayer *p = psmf_slot(A0, 0);
    if (!A0 || !A1 || !sr_guest_span_readable(A1, 12u)) return PSMF_ERR_PARAM;
    uint32_t b = MEM_R32(A1), sz = MEM_R32(A1 + 4), pri = MEM_R32(A1 + 8);
    if (!b || sz < PSMF_PLAYER_BUFFER_MIN_BYTES) {
        MEM_W32(A0, 0);
        return PSMF_ERR_BUFSIZE;
    }
    if (pri < PSMF_PLAYER_PRIORITY_MIN || pri >= PSMF_PLAYER_PRIORITY_LIMIT) {
        MEM_W32(A0, 0);
        return PSMF_ERR_PARAM;
    }
    psmf_player_release(p);
    p = psmf_slot(A0, 1);
    if (!p) return PSMF_ERR_PARAM;
    p->buffer = b;
    p->bufferSize = sz;
    p->priority = pri;
    p->status = PSMF_STATUS_INIT;
    p->pixelMode = 3;
    p->loopStatus = 1;
    p->videoCodec = PSMF_VIDEO_CODEC_AVC;
    p->audioCodec = PSMF_AUDIO_CODEC_ATRAC3PLUS;
    MEM_W32(A0, A0);
    sched_delay_current(PSMF_CREATE_DELETE_DELAY_US);
    return 0;
}
static uint32_t h_PsmfDelete(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p) return PSMF_ERR_STATUS;
    psmf_player_release(p);
    psmf_av_final("player_delete");
    MEM_W32(A0, 0);
    sched_delay_current(PSMF_CREATE_DELETE_DELAY_US);
    return 0;
}
static uint32_t h_PsmfSetTempBuf(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status != PSMF_STATUS_INIT) return PSMF_ERR_STATUS;
    if (!A1 || A2 < PSMF_TEMP_BUF_MIN_BYTES) return PSMF_ERR_PARAM;
    p->tempBuf = A1;
    p->tempSize = A2;
    return 0;
}
/* scePsmfPlayerSetPsmf and scePsmfPlayerSetPsmfCB open the same file the same way and differ only in
 * whether the caller's callbacks may run during the call (PPSSPP Core/HLE/scePsmf.cpp 3f40ddc8,
 * _PsmfPlayerSetPsmfOffset).  Neither services callbacks here yet: the 3,100 us wait is a plain delay
 * for both. */
static uint32_t psmf_set_psmf(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status != PSMF_STATUS_INIT) return PSMF_ERR_STATUS;
    if (!A1) return PSMF_ERR_PARAM;
    char path[512];
    if (!guest_cstr(A1, path, sizeof(path))) return PSMF_ERR_PARAM;
    uint32_t lba = 0, sz = 0;
    uint8_t h[2048];
    if (iso_lookup(path, &lba, &sz) != 0 || sz < sizeof(h) ||
        iso_read(lba, 0, h, sizeof(h)) != (int)sizeof(h) ||
        !psmf_parse_header(p, h, sizeof(h)))
        return PSMF_ERR_PARAM;
    if (p->producer) sr_psmf_producer_close(p->producer);
    p->producer = NULL;
    p->source.lba = lba;
    p->source.size = sz;
    SrPsmfSource source = {psmf_iso_read, p, sz};
    p->producer = sr_psmf_producer_open(&source, p->presentationBase);
    if (!p->producer) return PSMF_ERR_PARAM;
    strncpy(p->path, path, sizeof(p->path) - 1);
    p->fileLba = lba;
    p->fileSize = sz;
    p->readOffset = p->streamOffset;
    p->status = PSMF_STATUS_STANDBY;
    p->stoppedAfterStart = 0;
    psmf_flush(p);
    psmf_media_reset(p);
    sched_delay_current(PSMF_SET_PSMF_DELAY_US);
    return 0;
}
static uint32_t h_PsmfSetPsmf(CpuState *s){return psmf_set_psmf(s);}
static uint32_t h_PsmfSetPsmfCB(CpuState *s){return psmf_set_psmf(s);}
static uint32_t h_PsmfConfig(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p) return PSMF_ERR_STATUS;
    if (A1 == 0) {  /* loop */
        if (A2 > 1) return PSMF_ERR_PARAM;
        p->loopStatus = A2;
        return 0;
    }
    if (A1 == 1) {  /* pixel mode; -1 keeps the default, 3 */
        if ((int32_t)A2 < -1 || A2 > 3) return PSMF_ERR_PARAM;
        p->pixelMode = (A2 == (uint32_t)-1) ? 3 : A2;
        return 0;
    }
    return PSMF_ERR_CONFIG;
}
/* scePsmfPlayerStart(player, PsmfPlayerData *data, int initPts): initPts is the third
 * argument (a2). t0 is the fifth EABI argument, which this call never sets. */
static uint32_t h_PsmfStart(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status == PSMF_STATUS_INIT) return PSMF_ERR_STATUS;
    if (!A1 || !sr_guest_span_readable(A1, PSMF_PLAYER_DATA_BYTES)) return PSMF_ERR_PARAM;
    uint32_t d = A1, vc = MEM_R32(d), vs = MEM_R32(d + 4), ac = MEM_R32(d + 8), as = MEM_R32(d + 12);
    int32_t mode = (int32_t)MEM_R32(d + 16), speed = (int32_t)MEM_R32(d + 20), pts = (int32_t)A2;
    if (mode < 0 || mode > PSMF_PLAY_MODE_REWIND || vs >= p->videoStreams ||
        (p->audioStreams ? (as >= p->audioStreams) : (as > 0 && as != (uint32_t)-1)))
        return PSMF_ERR_CONFIG;
    if (vc && vc != PSMF_VIDEO_CODEC_AVC) return PSMF_ERR_STREAM;
    /* An audio codec of 1 is the other value the reference player accepts. */
    if (p->audioStreams && ac != 1 && ac != PSMF_AUDIO_CODEC_ATRAC3PLUS) return PSMF_ERR_STREAM;
    if (!p->audioStreams && ac != 0) return PSMF_ERR_STREAM;
    if (p->playerVersion == PSMF_PLAYER_VERSION_BASIC && pts != 0) return PSMF_ERR_PARAM;
    p->videoCodec = vc;
    p->videoStreamNum = vs;
    p->audioCodec = ac;
    p->audioStreamNum = as;
    p->playMode = (uint32_t)mode;
    p->playSpeed = (uint32_t)speed;
    p->currentPts = pts;
    p->warmup = 0;
    p->breakRequested = 0;
    psmf_select_streams(p, vs, as);
    psmf_flush(p);
    if (p->producer) {
        if (!sr_psmf_producer_select_streams(p->producer, vs,
                                             p->audioStreams ? as : SR_PSMF_STREAM_NONE))
            return PSMF_ERR_CONFIG;
    }
    psmf_media_reset(p);
    p->status = PSMF_STATUS_PLAYING;
    return 0;
}
/* scePsmfPlayerChangePlayMode(player, playMode, playSpeed): checks and their order follow PPSSPP
 * Core/HLE/scePsmf.cpp (3f40ddc8).  A player that is not playing yet answers INVALID_STATUS, a mode outside
 * PLAY..REWIND (0..5) INVALID_CONFIG, and FORWARD or REWIND on a PSMF without entry points (BASIC player)
 * INVALID_STREAM; only FORWARD and REWIND take the new speed.  The mode itself is what the getters read:
 * PAUSE stops the audio getter, as scePsmfPlayerStart's mode does. */
static uint32_t h_PsmfChangePlayMode(CpuState *s){
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status < PSMF_STATUS_PLAYING) return PSMF_ERR_STATUS;
    int32_t mode = (int32_t)A1;
    if (mode < 0 || mode > 5) return PSMF_ERR_CONFIG;
    if (mode == PSMF_PLAY_MODE_FORWARD || mode == PSMF_PLAY_MODE_REWIND) {
        if (p->playerVersion == PSMF_PLAYER_VERSION_BASIC) return PSMF_ERR_STREAM;
        p->playSpeed = A2;
    }
    if (psmf_log_on())
        fprintf(stderr, "PSMF_PLAY_MODE: player=0x%08x mode %u -> %d speed=%d\n", A0, p->playMode, mode,
                (int32_t)A2);
    p->playMode = (uint32_t)mode;
    return 0;
}
static uint32_t h_PsmfStop(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status < PSMF_STATUS_PLAYING) return PSMF_ERR_STATUS;
    p->status = PSMF_STATUS_STANDBY;
    p->stoppedAfterStart = 1;   /* psmf_media_reset below clears displayPtsValid; GetCurrentPts still knows */
    p->breakRequested = 0;
    psmf_flush(p);
    if (p->producer) sr_psmf_producer_reset(p->producer);
    psmf_media_reset(p);
    sched_delay_current(PSMF_STOP_DELAY_US);
    return 0;
}
static uint32_t h_PsmfBreak(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p) return PSMF_ERR_STATUS;
    p->breakRequested = 1;
    psmf_flush(p);
    return 0;
}
static uint32_t h_PsmfRelease(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status < PSMF_STATUS_STANDBY) return PSMF_ERR_STATUS;
    p->status = PSMF_STATUS_INIT;
    p->stoppedAfterStart = 0;
    p->fileLba = p->fileSize = p->streamOffset = p->streamSize = 0;
    psmf_flush(p);
    if (p->producer) {
        sr_psmf_producer_close(p->producer);
        p->producer = NULL;
    }
    psmf_media_reset(p);
    return 0;
}
static uint32_t h_PsmfStatus(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    return p ? p->status : PSMF_ERR_STATUS;
}
/* scePsmfPlayerGetCurrentPts(player, u32 *pts): the displaypts of the last picture the video getter
 * delivered.  pspautotests video/psmfplayer/getcurrentpts.expected (recorded on a PSP, rev
 * f573fe8c): a player still INIT or deleted is 0x80616001; a player in STANDBY, or playing before
 * its first picture, is 0x8061600c with the output untouched; once a picture was delivered it is 0
 * and writes ONE 32-bit word, the low half of the 64-bit sentinel the test passed in (a first
 * picture at 0 leaves "1337133700000000"), and a finished player reports its last picture (low
 * word 0x8a90f = 567567 there).  The status check comes before the pointer check.  A NULL or
 * unwritable pointer once a picture exists (the hardware run crashes there), and a player that
 * scePsmfPlayerStop returned to STANDBY after playing, are not measured and stop the run by name. */
static uint32_t h_PsmfGetCurrentPts(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status < PSMF_STATUS_STANDBY) return PSMF_ERR_STATUS;
    /* scePsmfPlayerStop returns a played player to STANDBY and resets the media state, so the
     * stop is keyed on that Stop, not on displayPtsValid, which the reset clears. */
    if (p->status == PSMF_STATUS_STANDBY && p->stoppedAfterStart)
        return hle_unmodelled_stop("scePsmfPlayerGetCurrentPts", "on a player stopped after playing is not measured");
    if (!p->displayPtsValid) return PSMF_ERR_NO_DATA;
    if (!A1 || !sr_guest_span_writable(A1, 4u))
        return hle_unmodelled_stop("scePsmfPlayerGetCurrentPts", "with an unwritable output 0x%08x is not measured", A1);
    MEM_W32(A1, (uint32_t)p->displayPts);
    return 0;
}
/* scePsmfPlayerGetPsmfInfo(player, PsmfInfo *info, u32 *width, u32 *height).  PsmfInfo is five
 * words: lastFrameTs, numVideoStreams, numAudioStreams, numPcmStreams, playerVersion.  Checks and
 * their order follow PPSSPP Core/HLE/scePsmf.cpp (3f40ddc8): an unusable player or info address is
 * refused with ILLEGAL_ADDR before the player is resolved (the PSP itself faults there), then an
 * unknown player or one whose PSMF is not set yet with INVALID_STATUS.  lastFrameTs is the header's
 * total duration less one picture, because the first picture is at 0: pspautotests
 * video/psmfplayer/getpsmfinfo.expected records 567567 for its 570570-tick test.pmf.  The stream
 * table classification (psmf_parse_header) has no LPCM class, so numPcmStreams is 0, which is also
 * what that hardware run reports for both of its files.  The width/height outputs exist only in the
 * 0x03090510 libpsmfplayer build (note_psmfplayer_lib_version): that build writes the video
 * stream's size (stream 0 until Start selects one) to each usable pointer, and every other build
 * keeps the two-argument contract.  LocoRoco 2 sizes its movie from them (#279). */
static uint32_t h_PsmfGetPsmfInfo(CpuState *s) {
    if (!A0 || !sr_guest_span_readable(A0, 4u) || !A1 || !sr_guest_span_writable(A1, 20u))
        return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status < PSMF_STATUS_STANDBY) return PSMF_ERR_STATUS;
    MEM_W32(A1 + 0u, (uint32_t)(p->durationPts - PSMF_VIDEO_PTS_STEP));
    MEM_W32(A1 + 4u, p->videoStreams);
    MEM_W32(A1 + 8u, p->audioStreams);
    MEM_W32(A1 + 12u, 0u);
    MEM_W32(A1 + 16u, p->playerVersion);
    if (s_psmfplayer_lib_version == 0x03090510u) {
        if (A2 && sr_guest_span_writable(A2, 4u)) MEM_W32(A2, p->videoWidth);
        if (A3 && sr_guest_span_writable(A3, 4u)) MEM_W32(A3, p->videoHeight);
    }
    return 0;
}
static uint32_t h_PsmfUpdate(CpuState *s) {
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status < PSMF_STATUS_PLAYING) return PSMF_ERR_STATUS;
    psmf_produce(p);
    psmf_video_pump(p);
    if (p->status == PSMF_STATUS_PLAYING && psmf_reached_end(p)) {
        if (p->loopStatus == 0u) {  /* loop: start the stream again */
            psmf_flush(p);
            if (p->producer) sr_psmf_producer_reset(p->producer);
            psmf_media_reset(p);
        } else {
            p->status = PSMF_STATUS_FINISHED;
            psmf_log_summary(p);
        }
    }
    return 0;
}
/* scePsmfPlayerGetVideoData(player, ScePsmfPlayerVideoData *d): d->frameWidth is the
 * caller's stride in pixels (0 means 512, rounded down to even) and d->displaybuf is the
 * guest buffer the decoded picture is written into; the call fills d->displaypts and returns
 * 0 only when a decoder really produced a frame.  Field layout and error behaviour follow the
 * public PSP player contract. Its initial NO_DATA gate precedes decode, so no decoded picture
 * enters the warm-up-held disposition on this path. */
static uint32_t h_PsmfGetVideo(CpuState *s) {
    s_psmf_getvideo++;
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status < PSMF_STATUS_PLAYING) return PSMF_ERR_STATUS;
    if (!A1 || !sr_guest_span_readable(A1, 12u)) return PSMF_ERR_INVALID_POINTER;
    int32_t bufw = (int32_t)MEM_R32(A1);
    uint32_t displaybuf = MEM_R32(A1 + 4);
    if (bufw < 0) return PSMF_ERR_PRIV_REQUIRED;
    if (bufw != 0 && (uint32_t)bufw < p->videoWidth) return PSMF_ERR_INVALID_VALUE;
    if (p->warmup < PSMF_WARMUP_FRAMES) {
        p->warmup++;
        return PSMF_ERR_NO_DATA;
    }
    p->warmup = PSMF_WARMUP_FRAMES;
    if (!displaybuf || !sr_guest_span_writable_any_view(displaybuf, 4u))
        return PSMF_ERR_INVALID_POINTER;
    psmf_produce(p);
    psmf_video_pump(p);
    int64_t pts = 0;
    int r = psmf_video_take(p, displaybuf, bufw == 0 ? 512 : (int)((uint32_t)bufw & ~1u),
                            (int)p->pixelMode, &pts);
    if (r <= 0) return PSMF_ERR_NO_DATA;
    sched_delay_current(PSMF_VIDEO_DATA_DELAY_US);
    MEM_W32(A1 + 4, displaybuf);
    MEM_W32(A1 + 8, (uint32_t)pts);
    return 0;
}
/* scePsmfPlayerGetAudioData(player, void *buf): buf receives one decoded ATRAC3+ frame as
 * 2048 stereo s16 samples (the size scePsmfPlayerGetAudioOutSize reports).  Returns 0 only
 * for a real decode; a frame that cannot be decoded yields NO_MORE_DATA, never silence. */
static uint32_t h_PsmfGetAudio(CpuState *s) {
    s_psmf_getaudio++;
    SrPsmfPlayer *p = psmf_find(A0, 0);
    if (!p || p->status < PSMF_STATUS_PLAYING) return PSMF_ERR_STATUS;
    if (!A1 || !sr_guest_span_writable(A1, PSMF_AUDIO_BYTES)) return PSMF_ERR_INVALID_POINTER;
    /* Audio is never handed out ahead of the first pictures the player calls
     * "still warming up", and a paused player produces nothing.  A track that has genuinely run
     * dry reports no-more-data; the reference player's trailing zero fill sits behind an
     * unassigned duration field and is not observable behaviour, so no silence is invented. */
    if (p->warmup < PSMF_WARMUP_FRAMES || p->playMode == PSMF_PLAY_MODE_PAUSE) return PSMF_ERR_NO_DATA;
    psmf_produce(p);
    psmf_video_pump(p);
    if (!psmf_audio_fill(p)) {
        sched_delay_current(PSMF_AUDIO_RETRY_DELAY_US);
        return PSMF_ERR_NO_DATA;
    }
    const uint8_t *src = (const uint8_t *)p->audioPcm;
    for (uint32_t i = 0; i < PSMF_AUDIO_BYTES; i++) MEM_W8(A1 + i, src[i]);
    sr_psmf_producer_media_stage(p->producer, SR_PSMF_AU_AUDIO,
                                 SR_PSMF_MEDIA_DELIVERED);
    psmf_av_delivered(PSMF_AV_AUDIO, A1, PSMF_AUDIO_BYTES, p->audioPcmPts, p->audioPcmPtsValid);
    p->audioDisplayPts = p->audioPcmPts;
    p->audioDisplayPtsValid = p->audioPcmPtsValid;
    p->audioPcmValid = 0;
    p->audioPcmSampleCount = 0;
    p->audioBlocksOut++;
    sched_delay_current(PSMF_AUDIO_DATA_DELAY_US);
    return 0;
}
/* scePsmfPlayerGetAudioOutSize: one GetAudioData frame, PSMF_AUDIO_BYTES (8192). */
static uint32_t h_PsmfAudioOutSize(CpuState *s) {
    return psmf_find(A0, 0) ? PSMF_AUDIO_BYTES : PSMF_ERR_STATUS;
}
/* ---- sceMpeg (PSMF video): faithful port in src/rt/mpeg.c (from PPSSPP) ----
 * These thin wrappers marshal MIPS args to the ported mpeg_* functions. The port implements the
 * real PSMF analysis, ring-buffer accounting, handle/context creation, stream registration, and
 * AU getters with timestamp progression + end-of-stream, so the movie playback loop runs and
 * completes exactly as on PPSSPP. AVC video goes through the backend-neutral decoder seam
 * (sr_h264.h): the project-authored PSMF producer forms the access units, and the registry in
 * h264_null.c picks the build's decoder, Media Foundation (h264_mf.c) on Windows or the vendored
 * FFmpeg H.264 subset (h264_ffmpeg.c, the H264_FFMPEG=1 default elsewhere). A build with
 * neither fails closed. Nothing here depends on which backend decodes. sceMpegAtracDecode
 * still decodes no audio samples; it only advances the audio clock. */
uint32_t mpeg_init(void);
uint32_t mpeg_finish(void);
uint32_t mpeg_ringbuffer_query_mem_size(uint32_t packets);
uint32_t mpeg_ringbuffer_query_pack_num(uint32_t mem_size);
uint32_t mpeg_avc_query_ycbcr_size(uint32_t mpegAddr, uint32_t mode, uint32_t width, uint32_t height, uint32_t resultAddr);
uint32_t mpeg_avc_init_ycbcr(uint32_t mpegAddr, uint32_t mode, uint32_t width, uint32_t height, uint32_t buf);
uint32_t mpeg_avc_decode_mode(uint32_t mpegAddr, uint32_t modeAddr);
uint32_t mpeg_avc_decode_ycbcr(uint32_t mpegAddr, uint32_t auAddr, uint32_t buf, uint32_t initAddr);
uint32_t mpeg_avc_decode_stop_ycbcr(uint32_t mpegAddr, uint32_t buf, uint32_t statusAddr);
uint32_t mpeg_avc_copy_ycbcr(uint32_t mpegAddr, uint32_t dst, uint32_t src);
uint32_t mpeg_avc_csc(uint32_t mpegAddr, uint32_t buf, uint32_t rangeAddr, uint32_t frameWidth, uint32_t dest);
uint32_t mpeg_query_pcm_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr);
uint32_t mpeg_get_pcm_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr);
uint32_t mpeg_change_get_au_mode(uint32_t mpegAddr, uint32_t sid, uint32_t mode);
uint32_t mpeg_ringbuffer_construct(uint32_t ring, uint32_t numPackets, uint32_t data, uint32_t size, uint32_t cbAddr, uint32_t cbArg);
uint32_t mpeg_create(uint32_t mpegAddr, uint32_t dataPtr, uint32_t size, uint32_t ringAddr, uint32_t frameWidth, uint32_t mode, uint32_t ddrTop);
uint32_t mpeg_delete(uint32_t mpegAddr);
uint32_t mpeg_query_stream_offset(uint32_t mpegAddr, uint32_t bufferAddr, uint32_t offsetAddr);
uint32_t mpeg_query_stream_size(uint32_t bufferAddr, uint32_t sizeAddr);
uint32_t mpeg_regist_stream(uint32_t mpegAddr, uint32_t streamType, uint32_t streamNum);
uint32_t mpeg_unregist_stream(uint32_t mpegAddr, uint32_t sid);
uint32_t mpeg_ringbuffer_available_size(uint32_t ring);
uint32_t mpeg_ringbuffer_put(CpuState *s, uint32_t ring, uint32_t numPackets, uint32_t available);
uint32_t mpeg_get_avc_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr);
uint32_t mpeg_get_atrac_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr);
uint32_t mpeg_avc_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t initAddr);
uint32_t mpeg_atrac_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t bufferAddr, uint32_t init);
uint32_t mpeg_avc_decode_stop(uint32_t mpegAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t statusAddr);
uint32_t mpeg_avc_decode_detail(uint32_t mpegAddr, uint32_t detailAddr);
uint32_t mpeg_malloc_avc_es_buf(uint32_t mpegAddr);
uint32_t mpeg_free_avc_es_buf(uint32_t mpegAddr, uint32_t esBuf);
uint32_t mpeg_init_au(uint32_t mpegAddr, uint32_t esBuffer, uint32_t auAddr);
uint32_t mpeg_query_atrac_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr);
uint32_t mpeg_query_userdata_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr);
uint32_t mpeg_get_userdata_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t resultAddr);
#ifndef SR_HLE_THREAD_SELFTEST
uint32_t mpeg_flush_all_stream(uint32_t mpegAddr);
uint32_t mpeg_flush_stream(uint32_t mpegAddr, uint32_t sid);
uint32_t mpeg_avc_decode_flush(uint32_t mpegAddr);
#endif


static uint32_t h_MpegInit(CpuState *s) { (void)s; return mpeg_init(); }
static uint32_t h_MpegMallocAvcEsBuf(CpuState *s) { return mpeg_malloc_avc_es_buf(A0); }
static uint32_t h_MpegFreeAvcEsBuf(CpuState *s) { return mpeg_free_avc_es_buf(A0, A1); }
static uint32_t h_MpegInitAu(CpuState *s) { return mpeg_init_au(A0, A1, A2); }
static uint32_t h_MpegQueryAtracEsSize(CpuState *s) { return mpeg_query_atrac_es_size(A0, A1, A2); }
static uint32_t h_MpegFinish(CpuState *s) { (void)s; return mpeg_finish(); }
/* sceMpegQueryMemSize() takes no args and RETURNS the context size in v0 (PPSSPP MpegRequiredMem:
 * 0x10000 for lib version >= 0x0105, which ACX uses). The earlier wrapper wrote it to a pointer and
 * returned 0, so the game allocated a 0-byte mpeg buffer and sceMpegCreate failed with NO_MEMORY. */
static uint32_t h_MpegQueryMemSize(CpuState *s) { (void)s; return 0x10000u; }
static uint32_t h_MpegCreate(CpuState *s) {
    uint32_t r = mpeg_create(A0, A1, A2, A3, stack_arg(s, 0), stack_arg(s, 1), stack_arg(s, 2));
    if (getenv("SR_MPEGLOG")) fprintf(stderr, "MpegCreate mpegAddr=0x%x data=0x%x size=0x%x ring=0x%x fw=%u -> 0x%x\n",
        A0, A1, A2, A3, stack_arg(s,0), r);
    return r;
}
static uint32_t h_MpegDelete(CpuState *s) { return mpeg_delete(A0); }
static uint32_t h_MpegRingbufferQueryMemSize(CpuState *s) { return mpeg_ringbuffer_query_mem_size(A0); }
static uint32_t h_MpegRingbufferQueryPackNum(CpuState *s) { return mpeg_ringbuffer_query_pack_num(A0); }
static uint32_t h_MpegAvcQueryYCbCrSize(CpuState *s) { return mpeg_avc_query_ycbcr_size(A0, A1, A2, A3, stack_arg(s, 0)); }
static uint32_t h_MpegAvcInitYCbCr(CpuState *s) { return mpeg_avc_init_ycbcr(A0, A1, A2, A3, stack_arg(s, 0)); }
static uint32_t h_MpegAvcDecodeMode(CpuState *s) { return mpeg_avc_decode_mode(A0, A1); }
/* Same decode latency (and guaranteed yield) as sceMpegAvcDecode. */
static uint32_t h_MpegAvcDecodeYCbCr(CpuState *s) {
    uint32_t r = mpeg_avc_decode_ycbcr(A0, A1, A2, A3);
    sched_delay_current(5400);
    return r;
}
static uint32_t h_MpegAvcDecodeStopYCbCr(CpuState *s) { return mpeg_avc_decode_stop_ycbcr(A0, A1, A2); }
static uint32_t h_MpegAvcCopyYCbCr(CpuState *s) { return mpeg_avc_copy_ycbcr(A0, A1, A2); }
static uint32_t h_MpegAvcCsc(CpuState *s) { return mpeg_avc_csc(A0, A1, A2, A3, stack_arg(s, 0)); }
static uint32_t h_MpegQueryPcmEsSize(CpuState *s) { return mpeg_query_pcm_es_size(A0, A1, A2); }
static uint32_t h_MpegGetPcmAu(CpuState *s) { return mpeg_get_pcm_au(A0, A1, A2, A3); }
static uint32_t h_MpegQueryUserdataEsSize(CpuState *s) { return mpeg_query_userdata_es_size(A0, A1, A2); }
static uint32_t h_MpegGetUserdataAu(CpuState *s) { return mpeg_get_userdata_au(A0, A1, A2, A3); }
static uint32_t h_MpegChangeGetAuMode(CpuState *s) { return mpeg_change_get_au_mode(A0, A1, A2); }
static uint32_t h_MpegRingbufferConstruct(CpuState *s) { return mpeg_ringbuffer_construct(A0, A1, A2, A3, stack_arg(s, 0), stack_arg(s, 1)); }
static uint32_t h_MpegRingbufferAvailable(CpuState *s) { return mpeg_ringbuffer_available_size(A0); }
static uint32_t h_MpegRingbufferPut(CpuState *s) { return mpeg_ringbuffer_put(s, A0, A1, A2); }
static uint32_t h_MpegRegistStream(CpuState *s) { return mpeg_regist_stream(A0, A1, A2); }
static uint32_t h_MpegUnRegistStream(CpuState *s) { return mpeg_unregist_stream(A0, A1); }
static uint32_t h_MpegQueryStreamOffset(CpuState *s) { return mpeg_query_stream_offset(A0, A1, A2); }
static uint32_t h_MpegQueryStreamSize(CpuState *s) { return mpeg_query_stream_size(A0, A1); }
/* PPSSPP returns these via hleDelayResult so the playback thread yields (it does not busy-poll the
 * ring). Mirror that: delay the calling thread a frame-ish, longer when there is no data yet, so the
 * feeder/display threads run and the movie paces instead of hanging the scheduler. */
static uint32_t h_MpegGetAvcAu(CpuState *s) {
    uint32_t r = mpeg_get_avc_au(A0, A1, A2, A3);
    sched_delay_current(r == 0x80618001u ? 8000u : 3000u);
    return r;
}
static uint32_t h_MpegGetAtracAu(CpuState *s) {
    uint32_t r = mpeg_get_atrac_au(A0, A1, A2, A3);
    sched_delay_current(r == 0x80618001u ? 8000u : 3000u);
    return r;
}
/* PPSSPP charges real decode latency (sceMpeg.cpp: avcDecodeDelayMs=5400, atracDecodeDelayMs=3000,
 * passed to hleDelayResult in microseconds). Besides pacing, the delay is a guaranteed yield. */
static uint32_t h_MpegAvcDecode(CpuState *s) {
    uint32_t r = mpeg_avc_decode(A0, A1, A2, A3, stack_arg(s, 0));
    sched_delay_current(5400);
    return r;
}
static uint32_t h_MpegAtracDecode(CpuState *s) {
    uint32_t r = mpeg_atrac_decode(A0, A1, A2, A3);
    sched_delay_current(3000);
    return r;
}
static uint32_t h_MpegAvcDecodeStop(CpuState *s) { return mpeg_avc_decode_stop(A0, A1, A2, A3); }
static uint32_t h_MpegAvcDecodeDetail(CpuState *s) { return mpeg_avc_decode_detail(A0, A1); }
/* sceMpegRingbufferDestruct(ring): drain a constructed ring buffer. The ring
 * layout is owned by src/rt/mpeg.c (RB_* field offsets); mpeg.c is not linked
 * into the executable HLE harness, so the harness-visible handler carries the
 * counter reset here against the same offsets. Only the queue accounting is
 * reset -- packetsRead/packetsWritePos/packetsAvail to zero, which is exactly
 * "reset counters, clear queued packets": the packet capacity, data span and
 * fill callback stay constructed, and the host movie progress (fedPackets)
 * is untouched. Whether firmware also detaches the ring or zeroes the
 * descriptor is UNMEASURED here. A null or partially-mapped descriptor is
 * refused with the same bad-ring code mpeg_ringbuffer_construct uses. */
#define MPEG_RB_BYTES 48u
#define MPEG_RB_PACKETS_READ 4u
#define MPEG_RB_PACKETS_WRITE_POS 8u
#define MPEG_RB_PACKETS_AVAIL 12u
static uint32_t h_MpegRingbufferDestruct(CpuState *s) {
    uint32_t ring = A0;
    if (!ring || !sr_guest_span_writable(ring, MPEG_RB_BYTES)) return 0x80020003u;
    MEM_W32(ring + MPEG_RB_PACKETS_READ, 0u);
    MEM_W32(ring + MPEG_RB_PACKETS_WRITE_POS, 0u);
    MEM_W32(ring + MPEG_RB_PACKETS_AVAIL, 0u);
    return 0;
}

/* sceMpegFlushAllStream(mpeg): flush and reset stream packets for an MPEG stream.
 * Public behaviour reference: PSPSDK pspmpeg.h and PPSSPP Core/HLE/sceMpeg.cpp.
 * Resets the ring buffer counters (packetsRead, packetsWritePos, packetsAvail)
 * and marks streams for reset. Errors reuse neighbouring bad-ring/address codes. */
static uint32_t h_MpegFlushAllStream(CpuState *s) {
    uint32_t mpegAddr = A0;
    if (!mpegAddr || !sr_guest_span_readable(mpegAddr, 4u)) return 0x80020003u;
#ifndef SR_HLE_THREAD_SELFTEST
    mpeg_flush_all_stream(mpegAddr);
#endif
    uint32_t h = MEM_R32(mpegAddr);
    if (h && sr_guest_span_readable(h, 24u)) {
        uint32_t ring = MEM_R32(h + 16u);
        if (ring && sr_guest_span_writable(ring, MPEG_RB_BYTES)) {
            MEM_W32(ring + MPEG_RB_PACKETS_READ, 0u);
            MEM_W32(ring + MPEG_RB_PACKETS_WRITE_POS, 0u);
            MEM_W32(ring + MPEG_RB_PACKETS_AVAIL, 0u);
        }
    }
    return 0;
}

/* sceMpegFlushStream(mpeg, stream): validates the context and the stream id (the value
 * sceMpegRegistStream returned) and returns 0, following PPSSPP (caa91b54, Core/HLE/sceMpeg.cpp),
 * whose handler does no flush.  What a PSP discards is unmeasured; the stream and ring state stay
 * as they are (sceMpegFlushAllStream clears them).  The descriptor check matches
 * sceMpegFlushAllStream.  In the executable HLE harness mpeg.c is not linked, so the handler only
 * checks the descriptor there. */
static uint32_t h_MpegFlushStream(CpuState *s) {
    uint32_t mpegAddr = A0;
    if (!mpegAddr || !sr_guest_span_readable(mpegAddr, 4u)) return 0x80020003u;
#ifndef SR_HLE_THREAD_SELFTEST
    return mpeg_flush_stream(mpegAddr, A1);
#else
    return 0;
#endif
}

/* sceMpegAvcDecodeFlush(mpeg): flush queued AVC decoding frames/packets.
 * Public behaviour reference: PSPSDK pspmpeg.h and PPSSPP Core/HLE/sceMpeg.cpp.
 * Resets the ring packet counters and any pending AVC decode timestamps. */
static uint32_t h_MpegAvcDecodeFlush(CpuState *s) {
    uint32_t mpegAddr = A0;
    if (!mpegAddr || !sr_guest_span_readable(mpegAddr, 4u)) return 0x80020003u;
#ifndef SR_HLE_THREAD_SELFTEST
    mpeg_avc_decode_flush(mpegAddr);
#endif
    uint32_t h = MEM_R32(mpegAddr);
    if (h && sr_guest_span_readable(h, 24u)) {
        uint32_t ring = MEM_R32(h + 16u);
        if (ring && sr_guest_span_writable(ring, MPEG_RB_BYTES)) {
            MEM_W32(ring + MPEG_RB_PACKETS_READ, 0u);
            MEM_W32(ring + MPEG_RB_PACKETS_WRITE_POS, 0u);
            MEM_W32(ring + MPEG_RB_PACKETS_AVAIL, 0u);
        }
    }
    return 0;
}
void hle_register_mpeg_shared_handlers(void) {
    /* MPEG flush/destruct family: shared between selftest harness and production. */
    sr_hle_register(0x13407f13, "sceMpegRingbufferDestruct", h_MpegRingbufferDestruct);
    sr_hle_register(0x707b7629, "sceMpegFlushAllStream", h_MpegFlushAllStream);
    sr_hle_register(0x500f0429, "sceMpegFlushStream", h_MpegFlushStream);
    sr_hle_register(0x4571cc64, "sceMpegAvcDecodeFlush", h_MpegAvcDecodeFlush);
}

/* These YCbCr handlers are registered by the focused media fixture and by the full registry,
 * so the fixture reaches the same production argument marshalling through sr_syscall(). */
/* One definition for both registry branches, so the media fixture reaches mpeg_avc_decode (the
 * movie frame writer behind the VRAM mirrors, #1049) through the production handler. */
void hle_register_mpeg_avc_decode_handler(void) {
    sr_hle_register(0x0e3c2e9d, "sceMpegAvcDecode", h_MpegAvcDecode);
    sr_hle_register(0x0f6c18d7, "sceMpegAvcDecodeDetail", h_MpegAvcDecodeDetail);
}

void hle_register_mpeg_userdata_handlers(void) {
    sr_hle_register(0xc45c99cc, "sceMpegQueryUserdataEsSize", h_MpegQueryUserdataEsSize);
    sr_hle_register(0x01977054, "sceMpegGetUserdataAu", h_MpegGetUserdataAu);
}
void hle_register_mpeg_ycbcr_handlers(void) {
    sr_hle_register(0x211a057c, "sceMpegAvcQueryYCbCrSize", h_MpegAvcQueryYCbCrSize);
    sr_hle_register(0x67179b1b, "sceMpegAvcInitYCbCr", h_MpegAvcInitYCbCr);
    sr_hle_register(0xa11c7026, "sceMpegAvcDecodeMode", h_MpegAvcDecodeMode);
    sr_hle_register(0xf0eb1125, "sceMpegAvcDecodeYCbCr", h_MpegAvcDecodeYCbCr);
    sr_hle_register(0xf2930c9c, "sceMpegAvcDecodeStopYCbCr", h_MpegAvcDecodeStopYCbCr);
    sr_hle_register(0x0558b075, "sceMpegAvcCopyYCbCr", h_MpegAvcCopyYCbCr);
    sr_hle_register(0x31bd0272, "sceMpegAvcCsc", h_MpegAvcCsc);
}

void hle_register_psmf_player_handlers(void) {
    sr_hle_register(0x1078c008, "scePsmfPlayerStop", h_PsmfStop);
    sr_hle_register(0x1e57a8e7, "scePsmfPlayerConfigPlayer", h_PsmfConfig);
    sr_hle_register(0x235d8787, "scePsmfPlayerCreate", h_PsmfCreate);
    sr_hle_register(0x2beb1569, "scePsmfPlayerBreak", h_PsmfBreak);
    sr_hle_register(0x2d0e4e0a, "scePsmfPlayerSetTempBuf", h_PsmfSetTempBuf);
    sr_hle_register(0x3ea82a4b, "scePsmfPlayerGetAudioOutSize", h_PsmfAudioOutSize);
    sr_hle_register(0x46f61f8b, "scePsmfPlayerGetVideoData", h_PsmfGetVideo);
    sr_hle_register(0x3d6d25a9, "scePsmfPlayerSetPsmf", h_PsmfSetPsmf);
    sr_hle_register(0x58b83577, "scePsmfPlayerSetPsmfCB", h_PsmfSetPsmfCB);
    sr_hle_register(0xa3d81169, "scePsmfPlayerChangePlayMode", h_PsmfChangePlayMode);
    sr_hle_register(0x95a84ee5, "scePsmfPlayerStart", h_PsmfStart);
    sr_hle_register(0x9b71a274, "scePsmfPlayerDelete", h_PsmfDelete);
    sr_hle_register(0xa0b8ca55, "scePsmfPlayerUpdate", h_PsmfUpdate);
    sr_hle_register(0xb9848a74, "scePsmfPlayerGetAudioData", h_PsmfGetAudio);
    sr_hle_register(0xdf089680, "scePsmfPlayerGetPsmfInfo", h_PsmfGetPsmfInfo);
    sr_hle_register(0xe792cd94, "scePsmfPlayerReleasePsmf", h_PsmfRelease);
    sr_hle_register(0xf8ef08a6, "scePsmfPlayerGetCurrentStatus", h_PsmfStatus);
    sr_hle_register(0x3ed62233, "scePsmfPlayerGetCurrentPts", h_PsmfGetCurrentPts);
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

#ifndef SR_HLE_THREAD_SELFTEST
/* Two runs of hle_register_init_mpeg() the media fixture also registers, so its end-of-feed
 * tests reach the production handlers through sr_syscall (#286). */
static void hle_register_mpeg_stream_offset_handler(void) {
    sr_hle_register(0x21ff80e4, "sceMpegQueryStreamOffset", h_MpegQueryStreamOffset);
}
static void hle_register_mpeg_ring_handlers(void) {
    sr_hle_register(0xb240a59e, "sceMpegRingbufferPut", h_MpegRingbufferPut);
    sr_hle_register(0xb5f6dc87, "sceMpegRingbufferAvailableSize", h_MpegRingbufferAvailable);
    sr_hle_register(0xfe246728, "sceMpegGetAvcAu", h_MpegGetAvcAu);
}
#endif
#ifdef SR_MPEG_MEDIA_SELFTEST
void sr_hle_test_register_mpeg_ring_handlers(void) {
    hle_register_mpeg_stream_offset_handler();
    hle_register_mpeg_ring_handlers();
}
#endif

#ifndef SR_MPEG_MEDIA_SELFTEST
#ifndef SR_HLE_THREAD_SELFTEST
/* From sr_hle_init(). */
void hle_register_init_mpeg(void) {
    sr_hle_register(0x682a619b, "sceMpegInit", h_MpegInit);
    sr_hle_register(0x874624d6, "sceMpegFinish", h_MpegFinish);
    sr_hle_register(0xc132e22f, "sceMpegQueryMemSize", h_MpegQueryMemSize);
    sr_hle_register(0xd8c5f121, "sceMpegCreate", h_MpegCreate);
    sr_hle_register(0x606a4649, "sceMpegDelete", h_MpegDelete);
    sr_hle_register(0x42560f23, "sceMpegRegistStream", h_MpegRegistStream);
    sr_hle_register(0x591a4aa2, "sceMpegUnRegistStream", h_MpegUnRegistStream);
    hle_register_mpeg_stream_offset_handler();
    sr_hle_register(0x611e9e11, "sceMpegQueryStreamSize", h_MpegQueryStreamSize);
    sr_hle_register(0xd7a29f46, "sceMpegRingbufferQueryMemSize", h_MpegRingbufferQueryMemSize);
    sr_hle_register(0x769bebb6, "sceMpegRingbufferQueryPackNum", h_MpegRingbufferQueryPackNum);
    hle_register_mpeg_ycbcr_handlers();
    sr_hle_register(0xc02cf6b5, "sceMpegQueryPcmEsSize", h_MpegQueryPcmEsSize);
    sr_hle_register(0x8c1e027d, "sceMpegGetPcmAu", h_MpegGetPcmAu);
    sr_hle_register(0x9dcfb7ea, "sceMpegChangeGetAuMode", h_MpegChangeGetAuMode);
    sr_hle_register(0x37295ed8, "sceMpegRingbufferConstruct", h_MpegRingbufferConstruct);
    /* sceMpeg ringbuffer destruct and flush: shared with the executable harness
     * through hle_register_mpeg_shared_handlers(). */
    hle_register_mpeg_shared_handlers();
    hle_register_mpeg_ring_handlers();
    sr_hle_register(0xe1ce83a7, "sceMpegGetAtracAu", h_MpegGetAtracAu);
    hle_register_mpeg_avc_decode_handler();
    hle_register_mpeg_userdata_handlers();
    sr_hle_register(0x800c44df, "sceMpegAtracDecode", h_MpegAtracDecode);
    sr_hle_register(0x740fccd1, "sceMpegAvcDecodeStop", h_MpegAvcDecodeStop);
    sr_hle_register(0xa780cf7e, "sceMpegMallocAvcEsBuf", h_MpegMallocAvcEsBuf);
    sr_hle_register(0xceb870b1, "sceMpegFreeAvcEsBuf", h_MpegFreeAvcEsBuf);
    sr_hle_register(0x167afd9e, "sceMpegInitAu", h_MpegInitAu);
    sr_hle_register(0xf8dcb679, "sceMpegQueryAtracEsSize", h_MpegQueryAtracEsSize);
}
#endif
#endif

/* PSMF player state and the A/V measurement (#279), moved from hle.c (#307) so the media
 * self-test links this file without the rest of HLE. */
uint32_t s_psmfplayer_lib_version;
#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_psmfplayer_lib_version(void) { return s_psmfplayer_lib_version; }
#endif
#ifdef SR_MPEG_MEDIA_SELFTEST
void sr_hle_test_set_psmfplayer_lib_version(uint32_t version) { s_psmfplayer_lib_version = version; }
#endif

/* scePsmfPlayer model: the guest drives the real player, the host services the media
 * boundary underneath it.  The chain is
 *
 *   guest ISO path -> bounded PSMF producer (aud-delimited pictures, ATRAC frames)
 *                  -> per-track compressed-AU queues
 *                  -> H.264 backend / ATRAC3+ bridge
 *                  -> guest display buffer / PCM block
 *
 * The getters return success only when a decoder actually produced output: a compressed
 * access unit is never misreported as a decoded PSP frame or PCM block, a picture whose
 * PES packet carried no timestamp is extrapolated from the codec frame step (the reference
 * player does the same), and a missing decoder leaves the movie visibly short rather than
 * fabricating frames. */
#define PSMF_STATUS_NONE 0u
#define PSMF_ERR_ALREADY_INIT 0x80618005u
#define PSMF_AV_WINDOW_US 5000000ll          /* about 300 vblanks, like AUDIOSTAT_LEAD */
#define PSMF_AV_TIME_LIMIT_US (1ll << 40)    /* keeps every x9 sum far inside int64 */
#define PSMF_AV_PTS_LIMIT ((int64_t)1 << 48)
#define PSMF_AV_DISPLAY_ROWS 272u
#define PSMF_AV_LAT_LIMIT_US ((int64_t)1 << 32)

static struct {
    PsmfAvStream s[2];
    uint64_t seq;
    int64_t window_us, win_start_us;
    int win_started, exit_hooked;
    unsigned windows, paired;
    int64_t av_first, av_last, av_min, av_max, av_sum;   /* x9 */
} s_psmf_av;

#ifdef SR_MPEG_MEDIA_SELFTEST
static int s_psmf_av_test_enable = -1;
int s_psmf_av_test_clock_set, s_psmf_av_test_queue_set;
static int64_t s_psmf_av_test_clock;
static char s_psmf_av_last_window[512], s_psmf_av_last_final[2048];
static unsigned s_psmf_av_window_lines, s_psmf_av_final_lines;
#endif

static int psmf_av_on(void) {
#ifdef SR_MPEG_MEDIA_SELFTEST
    if (s_psmf_av_test_enable >= 0) return s_psmf_av_test_enable;
#endif
    static int v = -1;
    if (v < 0) v = (getenv("SR_MPEGAV") || getenv("SR_MPEGLOG")) ? 1 : 0;
    return v;
}

int64_t psmf_av_now_us(void) {
#ifdef SR_MPEG_MEDIA_SELFTEST
    if (s_psmf_av_test_clock_set) return s_psmf_av_test_clock;
#endif
    uint64_t t = sched_vtime_us();
    return t > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)t;
}

static void psmf_av_clear(void) {
    int64_t window = s_psmf_av.window_us;
    int hooked = s_psmf_av.exit_hooked;
    memset(&s_psmf_av, 0, sizeof(s_psmf_av));
    s_psmf_av.window_us = window;
    s_psmf_av.exit_hooked = hooked;
}

static void psmf_av_window_close(int64_t end_us) {
    PsmfAvStream *v = &s_psmf_av.s[PSMF_AV_VIDEO], *a = &s_psmf_av.s[PSMF_AV_AUDIO];
    if (!v->win_n && !a->win_n) return;
    int64_t vm = v->win_n ? v->win_sum_x9 / (int64_t)v->win_n : 0;
    int64_t am = a->win_n ? a->win_sum_x9 / (int64_t)a->win_n : 0;
    int64_t av = vm - am;
    int paired = v->win_n && a->win_n;
    s_psmf_av.windows++;
    if (paired) {
        if (!s_psmf_av.paired) s_psmf_av.av_first = s_psmf_av.av_min = s_psmf_av.av_max = av;
        s_psmf_av.av_last = av;
        if (av < s_psmf_av.av_min) s_psmf_av.av_min = av;
        if (av > s_psmf_av.av_max) s_psmf_av.av_max = av;
        s_psmf_av.av_sum += av;
        s_psmf_av.paired++;
    }
    char vo[32], ao[32], avs[32], line[512];
    psmf_av_fmt(vo, sizeof(vo), v->win_n != 0, vm);
    psmf_av_fmt(ao, sizeof(ao), a->win_n != 0, am);
    psmf_av_fmt(avs, sizeof(avs), paired, av);
    int n = snprintf(line, sizeof(line),
                     "PSMF_AV_PRESENT: win=%u end_us=%lld vn=%llu an=%llu voff_us=%s aoff_us=%s av_us=%s\n",
                     s_psmf_av.windows, (long long)end_us, (unsigned long long)v->win_n,
                     (unsigned long long)a->win_n, vo, ao, avs);
    if (n > 0 && (size_t)n < sizeof(line)) fputs(line, stderr);
#ifdef SR_MPEG_MEDIA_SELFTEST
    if (n > 0 && (size_t)n < sizeof(line)) {
        memcpy(s_psmf_av_last_window, line, (size_t)n + 1u);
        s_psmf_av_window_lines++;
    }
#endif
    v->win_sum_x9 = a->win_sum_x9 = 0;
    v->win_n = a->win_n = 0;
}

void psmf_av_window_tick(int64_t now_us) {
    int64_t span = s_psmf_av.window_us ? s_psmf_av.window_us : PSMF_AV_WINDOW_US;
    if (!s_psmf_av.win_started) {
        s_psmf_av.win_started = 1;
        s_psmf_av.win_start_us = now_us;
        return;
    }
    if (now_us - s_psmf_av.win_start_us >= span) {
        psmf_av_window_close(now_us);
        s_psmf_av.win_start_us = now_us;
    }
}

void psmf_av_final(const char *reason);
extern void (*sr_ge_texture_sample_hook)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                         uint32_t, uint32_t);
static void psmf_av_texture_sampled(uint32_t tex_addr, uint32_t tex_bufw, uint32_t tex_h,
                                    uint32_t tex_fmt, uint32_t fb_addr, uint32_t fb_stride,
                                    uint32_t fb_fmt);

PsmfAvEntry *psmf_av_delivered(int stream, uint32_t first, uint32_t bytes, int64_t pts, int pts_valid) {
    if (!psmf_av_on()) return NULL;
    if (!s_psmf_av.exit_hooked) { s_psmf_av.exit_hooked = 1; atexit(psmf_av_exit_report); }
    if (stream == PSMF_AV_VIDEO) sr_ge_texture_sample_hook = psmf_av_texture_sampled;
    PsmfAvStream *st = &s_psmf_av.s[stream];
    st->delivered++;
    uint32_t lo, hi;
    if (!psmf_av_range(first, bytes, &lo, &hi)) { st->nospan++; return NULL; }
    PsmfAvEntry *slot = NULL, *oldest = NULL;
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (!e->live) { if (!slot) slot = e; continue; }
        if (!e->composited && e->lo < hi && lo < e->hi) {
            if (e->content) {
                e->lo = e->hi = 0;   /* overwritten where it was written: followed by its bytes only */
            } else {
                e->live = 0; st->superseded++; if (!slot) slot = e; continue;
            }
        }
        if (!oldest || e->seq < oldest->seq) oldest = e;
    }
    if (!slot) { oldest->live = 0; st->evicted++; slot = oldest; }
    slot->seq = ++s_psmf_av.seq;
    slot->lo = lo;
    slot->hi = hi;
    slot->pts = pts;
    slot->pts_valid = pts_valid;
    slot->delivered_us = psmf_av_now_us();
    slot->live = 1;
    slot->composited = 0;
    slot->fb_lo = slot->fb_hi = 0;
    slot->content = 0;
    slot->content_bytes = 0;
    return slot;
}

/* A textured draw (GE_PRIM) samples [tex_addr, +level-0 bytes) into the render target at
 * fb_addr (fb_stride pixels, fb_fmt): a waiting picture it overlaps is composited there. */
static void psmf_av_texture_sampled(uint32_t tex_addr, uint32_t tex_bufw, uint32_t tex_h,
                                    uint32_t tex_fmt, uint32_t fb_addr, uint32_t fb_stride,
                                    uint32_t fb_fmt) {
    if (!psmf_av_on()) return;
    PsmfAvStream *st = &s_psmf_av.s[PSMF_AV_VIDEO];
    uint32_t tlo, thi, row, fbytes, flo, fhi;
    uint32_t tbytes = psmf_av_texture_bytes(tex_bufw, tex_h, tex_fmt);
    if (!tbytes || !psmf_av_range(tex_addr, tbytes, &tlo, &thi) || fb_fmt > 3u ||
        !sr_size_mul_ok(fb_stride, fb_fmt == 3u ? 4u : 2u, &row) ||
        !sr_size_mul_ok(row, PSMF_AV_DISPLAY_ROWS, &fbytes) ||
        !psmf_av_range(fb_addr, fbytes, &flo, &fhi))
        return;
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (!e->live || e->composited || !(e->lo < thi && tlo < e->hi)) continue;
        e->composited = 1;
        e->fb_lo = flo;
        e->fb_hi = fhi;
        st->composited++;
    }
}

/* One delivery meets its presentation/play time.  play_ok = 0 means the time itself was not
 * measured (no host audio queue). */
void psmf_av_match(PsmfAvStream *st, PsmfAvEntry *e, int64_t play_us, int play_ok) {
    e->live = 0;
    if (!e->pts_valid || !play_ok || e->pts < 0 || e->pts >= PSMF_AV_PTS_LIMIT ||
        play_us < 0 || play_us >= PSMF_AV_TIME_LIMIT_US) {
        st->unmeasured++;
        return;
    }
    st->matched++;
    int64_t x9 = play_us * 9 - e->pts * 100;
    st->win_sum_x9 += x9;
    st->win_n++;
    int64_t lat = play_us - e->delivered_us;
    if (lat < 0) lat = 0;
    if (lat >= PSMF_AV_LAT_LIMIT_US) lat = PSMF_AV_LAT_LIMIT_US - 1;
    st->lat_sum_us += (uint64_t)lat;
    if (lat > st->lat_max_us) st->lat_max_us = lat;
}

/* A framebuffer became the visible one (sceDisplaySetFrameBuf sync=0, or the VBLANK latch). */
void psmf_av_video_presented(uint32_t addr, int32_t stride, int32_t fmt) {
    if (!psmf_av_on()) return;
    PsmfAvStream *st = &s_psmf_av.s[PSMF_AV_VIDEO];
    st->boundary_calls++;
    uint32_t row, bytes, lo, hi;
    if (!addr || stride <= 0 || fmt < 0 || fmt > 3 ||
        !sr_size_mul_ok((uint32_t)stride, fmt == 3 ? 4u : 2u, &row) ||
        !sr_size_mul_ok(row, PSMF_AV_DISPLAY_ROWS, &bytes) ||
        !psmf_av_range(addr, bytes, &lo, &hi))
        return;
    int hit = 0;
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (!e->live) continue;
        int own = e->lo < hi && lo < e->hi;
        int drawn = e->composited && e->fb_lo < hi && lo < e->fb_hi;
        if (!own && !drawn) continue;
        int64_t now = psmf_av_now_us();
        if (!hit) psmf_av_window_tick(now);
        hit = 1;
        if (!own) st->composited_matched++;
        psmf_av_match(st, e, now, 1);
    }
    if (hit) st->boundary_hits++;
}

/* An audio buffer reaches an sceAudioOutput* call.  rate_hz is the guest-side rate used to
 * place a block that begins part-way into the submitted buffer. */
void psmf_av_audio_submit(int ch, uint32_t buf, uint32_t bytes, uint32_t bytes_per_frame,
                                 uint32_t rate_hz) {
    if (!psmf_av_on()) return;
    PsmfAvStream *st = &s_psmf_av.s[PSMF_AV_AUDIO];
    st->boundary_calls++;
    uint32_t lo, hi;
    if (!bytes_per_frame || !rate_hz || !psmf_av_range(buf, bytes, &lo, &hi)) return;
    int any = 0;
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (e->live && e->lo < hi && lo < e->hi) { any = 1; break; }
    }
    if (!any) {
        psmf_av_audio_by_content(st, ch, buf, bytes, bytes_per_frame, rate_hz);
        return;
    }
    int queued = psmf_av_queue_frames(ch);          /* lead ahead of this buffer, read before the push */
    int64_t now = psmf_av_now_us();
    int64_t lead_us = queued >= 0 ? (int64_t)queued * 1000000 / 44100 : 0;
    psmf_av_window_tick(now);
    for (unsigned i = 0; i < PSMF_AV_PENDING; i++) {
        PsmfAvEntry *e = &st->pend[i];
        if (!e->live || !(e->lo < hi && lo < e->hi)) continue;
        int64_t frames = ((int64_t)e->lo - (int64_t)lo) / (int64_t)bytes_per_frame;
        int64_t play = now + lead_us + frames * 1000000 / (int64_t)rate_hz;
        psmf_av_match(st, e, play, queued >= 0);
    }
    st->boundary_hits++;
}

void psmf_av_final(const char *reason) {
    if (!psmf_av_on()) return;
    PsmfAvStream *v = &s_psmf_av.s[PSMF_AV_VIDEO], *a = &s_psmf_av.s[PSMF_AV_AUDIO];
    if (!v->delivered && !a->delivered) return;
    psmf_av_window_close(psmf_av_now_us());
    uint64_t pend[2] = {0, 0};
    for (int s = 0; s < 2; s++)
        for (unsigned i = 0; i < PSMF_AV_PENDING; i++)
            if (s_psmf_av.s[s].pend[i].live) pend[s]++;
    char vlm[32], vlx[32], alm[32], alx[32], mn[32], mx[32], mean[32], first[32], last[32], drift[32];
    psmf_av_fmt(vlm, sizeof(vlm), v->matched != 0, v->matched ? (int64_t)(v->lat_sum_us / v->matched) * 9 : 0);
    psmf_av_fmt(vlx, sizeof(vlx), v->matched != 0, v->lat_max_us * 9);
    psmf_av_fmt(alm, sizeof(alm), a->matched != 0, a->matched ? (int64_t)(a->lat_sum_us / a->matched) * 9 : 0);
    psmf_av_fmt(alx, sizeof(alx), a->matched != 0, a->lat_max_us * 9);
    int have = s_psmf_av.paired != 0;
    psmf_av_fmt(mn, sizeof(mn), have, s_psmf_av.av_min);
    psmf_av_fmt(mx, sizeof(mx), have, s_psmf_av.av_max);
    psmf_av_fmt(mean, sizeof(mean), have, have ? s_psmf_av.av_sum / (int64_t)s_psmf_av.paired : 0);
    psmf_av_fmt(first, sizeof(first), have, s_psmf_av.av_first);
    psmf_av_fmt(last, sizeof(last), have, s_psmf_av.av_last);
    psmf_av_fmt(drift, sizeof(drift), s_psmf_av.paired >= 2, s_psmf_av.av_last - s_psmf_av.av_first);
    char line[2048];
    int n = snprintf(line, sizeof(line),
        "PSMF_AV_PRESENT_FINAL: reason=%s windows=%u paired=%u"
        " vdel=%llu vmatch=%llu vunmeas=%llu vsuper=%llu vevict=%llu vnospan=%llu vpend=%llu vbnd=%llu vhit=%llu"
        " vcomp=%llu vcompmatch=%llu"
        " adel=%llu amatch=%llu aunmeas=%llu asuper=%llu aevict=%llu anospan=%llu apend=%llu abnd=%llu ahit=%llu"
        " acontent=%llu"
        " vlat_mean_us=%s vlat_max_us=%s alat_mean_us=%s alat_max_us=%s"
        " av_min_us=%s av_max_us=%s av_mean_us=%s av_first_us=%s av_last_us=%s drift_us=%s\n",
        reason, s_psmf_av.windows, s_psmf_av.paired,
        (unsigned long long)v->delivered, (unsigned long long)v->matched, (unsigned long long)v->unmeasured,
        (unsigned long long)v->superseded, (unsigned long long)v->evicted, (unsigned long long)v->nospan,
        (unsigned long long)pend[0], (unsigned long long)v->boundary_calls, (unsigned long long)v->boundary_hits,
        (unsigned long long)v->composited, (unsigned long long)v->composited_matched,
        (unsigned long long)a->delivered, (unsigned long long)a->matched, (unsigned long long)a->unmeasured,
        (unsigned long long)a->superseded, (unsigned long long)a->evicted, (unsigned long long)a->nospan,
        (unsigned long long)pend[1], (unsigned long long)a->boundary_calls, (unsigned long long)a->boundary_hits,
        (unsigned long long)a->content_matched,
        vlm, vlx, alm, alx, mn, mx, mean, first, last, drift);
    if (n > 0 && (size_t)n < sizeof(line)) {
        fputs(line, stderr);
#ifdef SR_MPEG_MEDIA_SELFTEST
        memcpy(s_psmf_av_last_final, line, (size_t)n + 1u);
        s_psmf_av_final_lines++;
#endif
    }
    psmf_av_clear();
}

/* Entry points for the sceMpeg path in mpeg.c (a separate translation unit in production):
 * sceMpegAvcDecode/Csc pictures and sceMpegAtracDecode blocks.  A guest PSMF player copies each
 * sceMpegAtracDecode block into its own output buffer before the game plays it, so a block is
 * also followed by its bytes. */
int sr_psmf_av_active(void) { return psmf_av_on(); }
void sr_psmf_av_note_video_delivery(uint32_t first, uint32_t bytes, int64_t pts, int pts_valid) {
    (void)psmf_av_delivered(PSMF_AV_VIDEO, first, bytes, pts, pts_valid);
}
void sr_psmf_av_note_audio_delivery(uint32_t first, uint32_t bytes, int64_t pts, int pts_valid) {
    PsmfAvEntry *e = psmf_av_delivered(PSMF_AV_AUDIO, first, bytes, pts, pts_valid);
    if (e) {
        e->content = psmf_av_content(first, bytes);
        e->content_bytes = e->content ? bytes : 0u;
    }
}

#ifdef SR_MPEG_MEDIA_SELFTEST
/* Test-only controls and read-back for the presentation-boundary measurement.  The clock and
 * the host queue lead are injected because the fixture has no running scheduler and no host
 * mixer; everything between the guest call and the recorded offset is production code. */
void sr_hle_test_psmf_av_reset(void) {
    memset(&s_psmf_av, 0, sizeof(s_psmf_av));
    s_psmf_av_test_enable = -1;
    s_psmf_av_test_clock_set = s_psmf_av_test_queue_set = 0;
    s_psmf_av_last_window[0] = s_psmf_av_last_final[0] = '\0';
    s_psmf_av_window_lines = s_psmf_av_final_lines = 0;
}
void sr_hle_test_psmf_av_enable(int on) { s_psmf_av_test_enable = on; }
void sr_hle_test_psmf_av_set_window_us(int64_t us) { s_psmf_av.window_us = us; }
void sr_hle_test_psmf_av_set_clock_us(int64_t us) { s_psmf_av_test_clock = us; s_psmf_av_test_clock_set = 1; }
void sr_hle_test_psmf_av_set_queue_frames(int frames) { s_psmf_av_test_queue = frames; s_psmf_av_test_queue_set = 1; }
const char *sr_hle_test_psmf_av_last_window(void) { return s_psmf_av_last_window; }
const char *sr_hle_test_psmf_av_last_final(void) { return s_psmf_av_last_final; }
unsigned sr_hle_test_psmf_av_window_lines(void) { return s_psmf_av_window_lines; }
unsigned sr_hle_test_psmf_av_final_lines(void) { return s_psmf_av_final_lines; }
/* Live counters before the final report clears them. */
int64_t sr_hle_test_psmf_av_counter(int audio, const char *key) {
    PsmfAvStream *st = &s_psmf_av.s[audio ? PSMF_AV_AUDIO : PSMF_AV_VIDEO];
    if (!strcmp(key, "delivered")) return (int64_t)st->delivered;
    if (!strcmp(key, "matched")) return (int64_t)st->matched;
    if (!strcmp(key, "unmeasured")) return (int64_t)st->unmeasured;
    if (!strcmp(key, "superseded")) return (int64_t)st->superseded;
    if (!strcmp(key, "evicted")) return (int64_t)st->evicted;
    if (!strcmp(key, "nospan")) return (int64_t)st->nospan;
    if (!strcmp(key, "content_matched")) return (int64_t)st->content_matched;
    if (!strcmp(key, "boundary")) return (int64_t)st->boundary_calls;
    if (!strcmp(key, "hits")) return (int64_t)st->boundary_hits;
    if (!strcmp(key, "composited")) return (int64_t)st->composited;
    if (!strcmp(key, "composited_matched")) return (int64_t)st->composited_matched;
    if (!strcmp(key, "pending")) {
        int64_t n = 0;
        for (unsigned i = 0; i < PSMF_AV_PENDING; i++) if (st->pend[i].live) n++;
        return n;
    }
    return -1;
}
void sr_hle_test_psmf_av_report(void) { psmf_av_final("test"); }
#endif

/* The audio side of the A/V measurement, moved from hle_audio.c (#307). */
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
