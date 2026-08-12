// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Public-safe audio boundary.
 *
 * The optional host mixer is intentionally absent from the public candidate
 * until its provenance and release review are complete.  These entry points
 * keep the generic runtime linkable while making the capability unavailable;
 * they never claim that samples were accepted or played.
 */

#include <stdint.h>
#include <stdio.h>

static int s_reported;

int sr_audio_init(void) {
    if (!s_reported) {
        fprintf(stderr, "audio: public-safe build has no host audio backend\n");
        s_reported = 1;
    }
    return 0;
}

void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR) {
    (void)ch;
    (void)lr;
    (void)nframes;
    (void)volL;
    (void)volR;
    (void)sr_audio_init();
}

void sr_audio_dump_stats(void) { (void)sr_audio_init(); }

/* A negative value is the documented "no host audio" signal to hle.c. */
int sr_audio_queued(int ch) {
    (void)ch;
    (void)sr_audio_init();
    return -1;
}
