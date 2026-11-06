// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// videorec.h - video recording of a run: every VBLANK's presented picture plus the game's own
// mixed audio, on one guest timeline (#1273, capture keys).
//
// Off unless SR_VIDEOREC_DIR names a folder, and then only on the deterministic clock
// (SR_DETERMINISTIC_CLOCK=1): without it the recorder refuses, says why on stderr and in
// record.json, and records nothing. Recording does cost the guest thread host time (the GPU
// readback, a frame copy, and writes to the encoder that block when it falls behind). On the
// paced clock that host time would make the guest skip VBLANKs; on the deterministic clock guest
// time follows the guest's own work, so the guest timeline is the same with or without the
// recording, only slower in wall-clock time. Then:
//   - the presenter hands the recorder each frame it shows, at its output resolution
//     (sr_videorec_frame, or sr_videorec_frame_pending for a GPU readback still in flight);
//   - every serviced VBLANK writes one video frame: the newest presented picture, or the
//     previous one again when the guest did not flip (constant 60000/1001 frames per second);
//   - every sceAudioOutput* hand-off is mixed into one stereo 44100 Hz track at the guest time
//     it was made, each channel continuing where its last buffer ended, as the PSP plays it.
// Frame k of the video and audio sample k * 44100 * 1001 / 60000 are the same guest instant, so
// the two cannot drift apart however fast or slow the host runs. The recorder writes no guest
// memory.
//
// Files in SR_VIDEOREC_DIR: video.mkv (FFV1 through FFmpeg, see SR_VIDEOREC_CODEC) or
// video.bgr0 (SR_VIDEOREC_CODEC=raw), audio.wav, frames.csv (one row per video frame),
// logic.srt (the game's frame rate per second), record.json (the summary) and, when FFmpeg
// muxed them, recording.mkv. The picture carries no host readout or overlay. A start removes
// what an earlier run left in the folder and writes record.json as INCOMPLETE; only the finish
// replaces it with PASS or FAIL, so a run that ended without one never shows PASS. A named
// encoder that cannot run (no FFmpeg, a bad SR_VIDEOREC_FFMPEG or codec) refuses the
// recording; it never falls back to raw frames. Under 4 GiB free on the folder's drive refuses
// it, and under 1 GiB during the run stops it as FAIL.
#ifndef SR_VIDEOREC_H
#define SR_VIDEOREC_H

#include <stddef.h>
#include <stdint.h>

#include "fbcap.h" /* sr_cap_order */

#define SR_VIDEOREC_AUDIO_RATE 44100u
#define SR_VIDEOREC_FPS_NUM 60000u
#define SR_VIDEOREC_FPS_DEN 1001u
#define SR_VIDEOREC_CHANNELS 16

/* The runtime's main() passes the scheduler's sched_clock_deterministic. A program that sets
 * none (one that links the presenter without a scheduler) never records: the start refuses. */
void sr_videorec_set_clock(int (*deterministic)(void));

/* 1 when recording was requested and the recorder is (or will be) writing. */
int sr_videorec_active(void);

/* 1 when a presented picture may be recorded: recording is on and SR_VIDEOREC_START (a VBLANK
 * number) is reached or next. A presenter skips its record work while this is 0. */
int sr_videorec_wants_frames(void);

/* The output size SR_VIDEOREC_SIZE asks for (WxH). Returns 0 and leaves *w, *h alone when no
 * size was given: the presenter then records at its own output size. */
int sr_videorec_requested_size(uint32_t *w, uint32_t *h);

/* A presenter showed this picture (w x h, row_bytes apart, in order). The recorder keeps a copy;
 * the next VBLANK writes it. The first frame fixes the video size: a later frame of another
 * size is refused, counted and reported, and the previous picture is written in its place. */
void sr_videorec_frame(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                       sr_cap_order order);

/* How a presenter made the picture it hands over (#2138). record.json names the filter of the
 * frames it wrote ("filter", or "mixed" when they differ) and counts each ("filter_frames").
 * Each names the scaling that was carried out, not the setting that asked for it: NONE is a
 * picture at its own size; NEAREST is one nearest-neighbour scale; SHARP is a nearest prescale
 * to a whole multiple, then one linear scale; LINEAR is one linear scale; FSR1 is the
 * presentation upscaler (SR_PRESENT_UPSCALE=fsr1). */
typedef enum {
    SR_VIDEOREC_FILTER_NONE = 0,
    SR_VIDEOREC_FILTER_NEAREST,
    SR_VIDEOREC_FILTER_SHARP,
    SR_VIDEOREC_FILTER_LINEAR,
    SR_VIDEOREC_FILTER_FSR1,
    SR_VIDEOREC_FILTER_COUNT
} SrVideoRecFilter;

/* The name record.json gives a filter ("none", "nearest", "sharp", "linear", "fsr1"), or NULL
 * for a value that is not one. */
const char *sr_videorec_filter_name(int filter);

/* sr_videorec_frame for a picture made with `filter`; sr_videorec_frame is this with NONE. A
 * value that is not an SrVideoRecFilter fails the recording. */
void sr_videorec_frame_filtered(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                                sr_cap_order order, int filter);

/* The guest flipped to a new frame (sceDisplaySetFrameBuf). Counted per written frame in
 * frames.csv and per second in logic.srt, the game's own frame rate beside a picture that carries
 * no host readout. */
void sr_videorec_guest_flip(void);

/* A presenter whose readback is still on the GPU registers how to finish it: the callback waits
 * for it and calls sr_videorec_frame (returns 1), or reports that it could not (0). The recorder
 * calls it before it writes the next VBLANK's frame, and the presenter calls
 * sr_videorec_resolve_pending before it reuses the readback. */
typedef int (*SrVideoRecResolver)(void);
void sr_videorec_frame_pending(SrVideoRecResolver resolver);
void sr_videorec_resolve_pending(void);

/* One serviced VBLANK at guest time guest_us: write this VBLANK's frame and the audio before it.
 * VCOUNT steps the guest skipped (a late host) are written as repeats and counted. */
void sr_videorec_vblank(uint32_t vcount, uint64_t guest_us);

/* A guest audio hand-off of nframes stereo frames on channel ch at volumes volL, volR (0x8000 is
 * unity), made at guest time guest_us. */
void sr_videorec_audio(int ch, const int16_t *lr, int nframes, int volL, int volR,
                       uint64_t guest_us);

/* Close the files and the encoder, mux, and write record.json. Safe to call more than once;
 * also registered with atexit. Returns 1 when the recording is complete and consistent.
 * sr_videorec_finish_at names how the run ended (record.json "run_end"): every route that
 * leaves with _Exit calls it, since _Exit skips atexit. sr_videorec_finish_status also records
 * the process exit status the route is about to return (record.json "exit_status"): every HLE
 * stop, the driver's own ends and sceKernelExitGame call it. The atexit fallback records
 * "exit_status": null and says that an exit() named no end, so a run that left through one of
 * the runtime's exit(1) paths never reads as a clean exit. */
int sr_videorec_finish(void);
int sr_videorec_finish_at(const char *run_end);
int sr_videorec_finish_status(const char *run_end, int exit_status);

/* Audio sample index of video frame k: floor(k * 44100 * 1001 / 60000). */
uint64_t sr_videorec_frame_sample(uint64_t k);

#ifdef SR_VIDEOREC_SELFTEST
/* Selftest seams: start a recording into dir with the given encoder ("raw" writes no process)
 * without reading the environment, and read the counters back. */
int sr_videorec_test_start(const char *dir, const char *codec);
int sr_videorec_test_start_at(const char *dir, const char *codec, uint32_t start_vcount);
typedef struct {
    uint64_t frames, new_frames, guest_flips, repeat_frames, gap_frames, late_vblanks, refused_frames;
    uint64_t audio_samples, audio_overflow, audio_trimmed;
    uint32_t width, height, first_vcount;
    int failed;
    uint64_t filter_frames[SR_VIDEOREC_FILTER_COUNT]; /* written frames, by their picture's filter */
} SrVideoRecStats;
void sr_videorec_test_stats(SrVideoRecStats *out);
void sr_videorec_test_reset(void);
/* Start from the environment, as the runtime does (SR_VIDEOREC_DIR, SR_DETERMINISTIC_CLOCK...). */
int sr_videorec_test_start_from_env(void);
/* The free space the recorder sees (the reset sets 1 TiB), and failing audio.wav writes. */
void sr_videorec_test_set_free_bytes(uint64_t bytes);
void sr_videorec_test_fail_audio_writes(int on);
/* Make the clear's move aside of one output file (by name, e.g. "audio.wav") fail; NULL clears. */
void sr_videorec_test_fail_clear_at(const char *name);
/* Create one output name (e.g. "audio.wav") right after the clear, as another program could;
 * NULL clears. */
void sr_videorec_test_plant_after_clear(const char *name);
#endif

#endif /* SR_VIDEOREC_H */
