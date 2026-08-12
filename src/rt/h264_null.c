// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Portable no-op H.264 backend (see sr_h264.h). Used on platforms without the Windows Media
 * Foundation decoder (h264_mf.c) and until a libavcodec backend is wired in. It produces no
 * frames, so sceMpeg's timestamp model (src/rt/mpeg.c) still advances and completes the movie
 * with blank video rather than failing — the game's movie-playback loop runs identically, just
 * without pixels. Swap this out for h264_ffmpeg.c to get real frames on Linux/Steam Deck.
 *
 * On Windows this file compiles to nothing (h264_mf.c provides the real backend).
 */

#if !defined(_WIN32)

#include "sr_h264.h"

int  sr_h264_create(void) { return -1; }
void sr_h264_destroy(int id) { (void)id; }
void sr_h264_feed(int id, const uint8_t *data, uint32_t len) { (void)id; (void)data; (void)len; }
int  sr_h264_frame(int id, int eos, uint32_t buffer, int frameWidth, int pixelMode) {
    (void)id; (void)eos; (void)buffer; (void)frameWidth; (void)pixelMode;
    return -1;   /* no decoder: mpeg.c clears the frame buffer and advances the timestamp model */
}

#endif /* !_WIN32 */
