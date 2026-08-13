// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// strbuf.c — checked-append formatting for fixed-size buffers (see strbuf.h).

#include "strbuf.h"

#include <stdio.h>

size_t sr_buf_append_v(char *buf, size_t cap, size_t n, const char *fmt, va_list ap) {
    /* Full or unusable target: never form buf + n past the buffer end. */
    if (!buf || cap == 0) return n;
    if (n >= cap) return cap;
    int want = vsnprintf(buf + n, cap - n, fmt, ap);
    if (want < 0) {
        /* Encoding error: truncate at the cursor and stay NUL-terminated. */
        buf[n] = '\0';
        return n;
    }
    /* vsnprintf wrote min(want, cap - n - 1) characters plus a NUL.  The
     * new cursor is the last written character, never the "would have been"
     * count, so a truncated token cannot advance the cursor past the NUL. */
    size_t avail = cap - n - 1;
    size_t wrote = (size_t)want < avail ? (size_t)want : avail;
    return n + wrote;
}

size_t sr_buf_append(char *buf, size_t cap, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t out = sr_buf_append_v(buf, cap, n, fmt, ap);
    va_end(ap);
    return out;
}
