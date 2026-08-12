/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored minimal stand-in for FFmpeg n4.4 libavutil/thread.h
 * (PR-A).
 *
 * Upstream thread.h wraps pthreads (and Win32 threads) to provide
 * ff_thread_once/AVOnce for single-time static table initialization (VLCs,
 * FFT cosine tables). The imported subset uses AVOnce/ff_thread_once in
 * libavcodec/atrac3plusdec.c, libavcodec/fft_template.c and
 * libavcodec/fft_init_table.c only.
 *
 * The standalone decoder is single-threaded by contract (the PSP HLE path is
 * single-threaded; distinct decoder instances never share an AVOnce). This
 * implementation is therefore a plain flag guard, NOT a mutex-protected once
 * primitive. Concurrent first use of two fresh decoder instances is not
 * supported and is documented in PROVENANCE.md.
 */

#ifndef AT3P_LIBAVUTIL_THREAD_H
#define AT3P_LIBAVUTIL_THREAD_H

typedef struct AVOnce {
    void (*func)(void);
    int done;
} AVOnce;

#define AV_ONCE_INIT { NULL, 0 }

static inline void ff_thread_once(AVOnce *once, void (*func)(void))
{
    if (!once->done) {
        once->func = func;
        func();
        once->done = 1;
    }
}

#endif /* AT3P_LIBAVUTIL_THREAD_H */
