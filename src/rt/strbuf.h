// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// strbuf.h — checked-append formatting for fixed-size buffers.
//
// snprintf() returns the number of characters that WOULD have been written,
// not the number actually written.  Cursor accumulation of the form
//
//     n += snprintf(buf + n, sizeof(buf) - n, ...);
//
// therefore lets a truncated format advance the cursor beyond the buffer:
// the next iteration forms `buf + n` as an out-of-range pointer and
// `sizeof(buf) - n` underflows to a huge size_t, which is a stack overflow
// in disguise.  sr_buf_append() is the checked replacement used by every
// cursor-accumulation site in this tree.
//
// Contract:
//   - `cap` >= 1, `n` <= `cap`; `n == cap` is the "full" state and is a no-op.
//   - The return value is the new cursor.  When `n < cap` the return is
//     strictly `< cap`, so the caller can never form `buf + cursor` or
//     `cap - cursor` out of range, and `cap - cursor` never underflows.
//   - Every append leaves the buffer NUL-terminated within [0, cap - 1].
//   - A token that does not fit is truncated to the available space; the
//     buffer remains a valid NUL-terminated string and the cursor stops at
//     the NUL (the last usable byte, cap - 1).
//
// The implementation is additionally robust to hostile inputs and never
// depends on every caller forever passing honest state:
//   - `n > cap` (including `SIZE_MAX`, corrupted or adversarial cursors) is
//     treated as the full state: no write, no pointer formed, returns `cap`.
//   - `buf == NULL` or `cap == 0` is a documented no-op: no dereference, no
//     write, returns the incoming cursor unchanged.
//   - `buf + n` is only ever formed when `n < cap`, so no pointer arithmetic
//     leaves [buf, buf + cap] and `cap - n - 1` can never underflow.
// Callers that finalize with `buf[n] = '\0'` / `buf[n] = '\n'` must pass a
// real buffer with `cap >= 1`; the helper itself is a pure no-op otherwise.

#ifndef SR_STRBUF_H
#define SR_STRBUF_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Append one formatted token at cursor `n` of `buf` (capacity `cap`).
 * Returns the new cursor (see contract above).  Never writes past
 * buf[cap - 1] and never forms an out-of-range pointer. */
size_t sr_buf_append(char *buf, size_t cap, size_t n, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/* va_list variant of sr_buf_append (same contract). */
size_t sr_buf_append_v(char *buf, size_t cap, size_t n, const char *fmt, va_list ap)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 0)))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif /* SR_STRBUF_H */
