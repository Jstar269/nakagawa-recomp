// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * sr_h264 — portable H.264 (AVC) video-decode backend seam for sceMpeg (src/rt/mpeg.c).
 *
 * mpeg.c demuxes the PSMF program stream and drives one of these backends to turn the AVC
 * elementary stream into PSP video-buffer frames. The backend is selected at build time:
 *
 *   - Windows: Media Foundation (src/rt/h264_mf.c) — msmpeg2vdec.dll, ships with Windows.
 *   - Other platforms: a null backend (src/rt/h264_null.c) that produces no frames, so the
 *     sceMpeg timestamp model still runs the movie to completion with blank video.
 *   - Future: a libavcodec backend (src/rt/h264_ffmpeg.c) drops in behind this same API to give
 *     real frames on Linux/Steam Deck; select it in the Makefile without touching mpeg.c.
 *
 * All backends implement exactly this ABI, so mpeg.c is decoder-agnostic.
 */

#ifndef SR_H264_H
#define SR_H264_H

#include <stdint.h>

/* Create a decoder instance. Returns a non-negative id, or -1 if no decoder is available
 * (e.g. SR_NOH264 set, MFT missing, or the null backend). */
int  sr_h264_create(void);

/* Destroy a decoder instance created by sr_h264_create(). Safe on an invalid id. */
void sr_h264_destroy(int id);

/* Feed `len` bytes of MPEG-PS packet data (demux happens inside the backend). */
void sr_h264_feed(int id, const uint8_t *data, uint32_t len);

/* Try to produce the next decoded frame into guest video buffer at `buffer` (guest address),
 * `frameWidth` pixels stride, in the PSP pixel format `pixelMode` (0=5650,1=5551,2=4444,3=8888).
 * `eos` != 0 once the whole movie has been fed (drain the last frames). Returns 1 if a frame was
 * written, 0 if none is available yet, -1 on failure. */
int  sr_h264_frame(int id, int eos, uint32_t buffer, int frameWidth, int pixelMode);

#endif /* SR_H264_H */
