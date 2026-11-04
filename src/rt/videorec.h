// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// videorec.h - video recording of a run: every VBLANK's presented picture plus the game's own
// mixed audio, on one guest timeline (#1273, capture keys).
//
// Off unless SR_VIDEOREC_DIR names a folder. Then:
//   - the presenter hands the recorder each frame it shows, at its output resolution
//     (sr_videorec_frame, or sr_videorec_frame_pending for a GPU readback still in flight);
//   - every serviced VBLANK writes one video frame: the newest presented picture, or the
//     previous one again when the guest did not flip (constant 60000/1001 frames per second);
//   - every sceAudioOutput* hand-off is mixed into one stereo 44100 Hz track at the guest time
//     it was made, each channel continuing where its last buffer ended, as the PSP plays it.
// Frame k of the video and audio sample k * 44100 * 1001 / 60000 are the same guest instant, so
// the two cannot drift apart however fast or slow the host runs. The recorder is host-side
// only: it writes no guest memory and changes no guest-visible state or timing.
//
// Files in SR_VIDEOREC_DIR: video.mkv (FFV1 through FFmpeg, see SR_VIDEOREC_CODEC) or
// video.bgr0 (raw frames, when FFmpeg is not used), audio.wav, frames.csv (one row per
// video frame), record.json (the summary) and, when FFmpeg muxed them, recording.mkv.
#ifndef SR_VIDEOREC_H
#define SR_VIDEOREC_H

#include <stddef.h>
#include <stdint.h>

#include "fbcap.h" /* sr_cap_order */

#define SR_VIDEOREC_AUDIO_RATE 44100u
#define SR_VIDEOREC_FPS_NUM 60000u
#define SR_VIDEOREC_FPS_DEN 1001u
#define SR_VIDEOREC_CHANNELS 16

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
 * also registered with atexit. Returns 1 when the recording is complete and consistent. */
int sr_videorec_finish(void);

/* Audio sample index of video frame k: floor(k * 44100 * 1001 / 60000). */
uint64_t sr_videorec_frame_sample(uint64_t k);

#ifdef SR_VIDEOREC_SELFTEST
/* Selftest seams: start a recording into dir with the given encoder ("raw" writes no process)
 * without reading the environment, and read the counters back. */
int sr_videorec_test_start(const char *dir, const char *codec);
int sr_videorec_test_start_at(const char *dir, const char *codec, uint32_t start_vcount);
typedef struct {
    uint64_t frames, new_frames, repeat_frames, gap_frames, late_vblanks, refused_frames;
    uint64_t audio_samples, audio_overflow, audio_trimmed;
    uint32_t width, height, first_vcount;
    int failed;
} SrVideoRecStats;
void sr_videorec_test_stats(SrVideoRecStats *out);
void sr_videorec_test_reset(void);
#endif

#endif /* SR_VIDEOREC_H */
