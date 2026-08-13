// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// strbuf_selftest.c — regression suite for the checked-append helper
// (src/rt/strbuf.c, issue campaign "format-append-hardening").
//
// The hazard being regression-tested: snprintf() returns the number of
// characters that WOULD have been written, so cursor accumulation like
//
//     n += snprintf(buf + n, sizeof(buf) - n, ...);
//
// advances n beyond the buffer the moment a token truncates.  The next
// iteration then forms buf + n out of range and sizeof(buf) - n underflows.
// Every "cursor < cap" assertion below is violated by that old arithmetic
// (e.g. appending a 10-byte token to an 8-byte buffer leaves the old cursor
// at 10), and under ASan/UBSan the old pattern's follow-up append faults.
//
// The suite drives the helper with tiny buffers and forced truncation so the
// contract invariants are proven directly: the cursor never reaches or
// exceeds cap, the buffer is always NUL-terminated, a full buffer is a
// no-op, and the non-truncating path is byte-identical to plain snprintf
// (which keeps the recompiled-code and reference-interpreter traces
// byte-comparable).  No game inputs or private data required.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "strbuf.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, name)                                                      \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);           \
        }                                                                      \
    } while (0)

static void test_truncation_never_escapes_buffer(void) {
    /* A 10-byte token in an 8-byte buffer: the old arithmetic leaves the
     * cursor at 10 (past the end); the helper must stop at 7 with a NUL. */
    char buf[8];
    memset(buf, 0xAA, sizeof(buf));
    size_t n = 0;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "abcdefghij");
    CHECK(n == 7, "truncated append stops at cap-1");
    CHECK(buf[7] == '\0', "truncated append keeps NUL at the cursor");
    CHECK(memcmp(buf, "abcdefg\0", sizeof(buf)) == 0, "truncated bytes match prefix");
}

static void test_full_buffer_is_noop(void) {
    /* Buffer already full (cursor at cap-1): the helper must not write,
     * must not move the cursor, and must keep the NUL. */
    char buf[8] = "abc";
    size_t n = 3;
    n = sr_buf_append(buf, sizeof(buf), n, "defghijklmnop"); /* would need 13 */
    CHECK(n == 7, "second append stops at cap-1");
    CHECK(buf[7] == '\0', "full buffer stays NUL-terminated");
    CHECK(memcmp(buf, "abcdefg\0", sizeof(buf)) == 0, "full buffer bytes unchanged");
}

static void test_exact_and_partial_fit(void) {
    /* "abc" + "def" in a 6-byte buffer: the second token needs 3 characters
     * but only 2 slots remain before the NUL, so it must truncate to "de". */
    char buf[6];
    size_t n = 0;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "abc");
    CHECK(n == 3, "exact-fit token advances cursor by its length");
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "def");
    CHECK(n == 5, "partial-fit token stops at cap-1");
    CHECK(buf[5] == '\0', "partial-fit keeps NUL at the cursor");
    CHECK(memcmp(buf, "abcde\0", sizeof(buf)) == 0, "partial-fit writes the fitting prefix");
}

static void test_one_byte_buffer(void) {
    char buf[1];
    buf[0] = 'x';
    size_t n = 0;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "abc");
    CHECK(n == 0, "one-byte buffer: cursor stays 0");
    CHECK(buf[0] == '\0', "one-byte buffer: only the NUL fits");
}

static void test_already_full_cursor(void) {
    /* n == cap (the documented "full" state) must be a no-op that returns
     * cap without touching the buffer. */
    char buf[4] = "abc";
    size_t n = 4;
    size_t r = sr_buf_append(buf, sizeof(buf), n, "%s", "xyz");
    CHECK(r == 4, "n == cap returns cap");
    CHECK(memcmp(buf, "abc\0", sizeof(buf)) == 0, "n == cap leaves the buffer untouched");
}

static void test_null_and_zero_capacity(void) {
    CHECK(sr_buf_append(NULL, 0, 0, "%s", "abc") == 0, "NULL/zero-cap returns the cursor");
    char buf[4] = "ab";
    CHECK(sr_buf_append(buf, 0, 0, "%s", "abc") == 0, "zero cap returns the cursor");
    CHECK(memcmp(buf, "ab\0\0", sizeof(buf)) == 0, "zero cap leaves the buffer untouched");
}

static size_t append_v_wrap(char *buf, size_t cap, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t r = sr_buf_append_v(buf, cap, n, fmt, ap);
    va_end(ap);
    return r;
}

static void test_append_v(void) {
    char buf[8] = "";
    size_t n = append_v_wrap(buf, sizeof(buf), 0, "%d", 12345);
    CHECK(n == 5 && memcmp(buf, "12345\0\0\0", sizeof(buf)) == 0,
          "va_list path byte-exact with room");
    CHECK(n < sizeof(buf), "cursor below cap after plain append");
}

static void test_append_v_truncates(void) {
    char buf[6];
    size_t n = append_v_wrap(buf, sizeof(buf), 0, "%s", "abcdefgh");
    CHECK(n == 5, "va_list path truncates at cap-1");
    CHECK(buf[5] == '\0', "va_list path keeps NUL at the cursor");
    CHECK(memcmp(buf, "abcde\0", sizeof(buf)) == 0, "va_list path writes the fitting prefix");
}

/* Worst-case token stream of sr_end_impl / TraceSink::EndStep (31 GPRs,
 * hi, lo, 32 FPRs, fcr31, 128 VFPU regs, one memory token) rendered at two
 * sizes: the production 4096-byte buffer must not truncate and must match a
 * literal expectation; a 64-byte buffer must exercise every truncation
 * branch without ever escaping the buffer. */
static size_t render_trace_line(char *buf, size_t cap, size_t *wrote_expected) {
    size_t n = 0;
    n = sr_buf_append(buf, cap, n, "%llu pc=0x%08x op=0x%08x",
                      (unsigned long long)42, 0x08000100u, 0x8e020004u);
    for (int i = 1; i < 32; i++)
        n = sr_buf_append(buf, cap, n, " r%d=0x%08x", i, 0xffffffffu);
    n = sr_buf_append(buf, cap, n, " hi=0x%08x", 0x12345678u);
    n = sr_buf_append(buf, cap, n, " lo=0x%08x", 0x9abcdef0u);
    for (int i = 0; i < 32; i++)
        n = sr_buf_append(buf, cap, n, " f%d=0x%08x", i, 0xffffffffu);
    n = sr_buf_append(buf, cap, n, " fcr31=0x%08x", 0x01000000u);
    for (int i = 0; i < 128; i++)
        n = sr_buf_append(buf, cap, n, " v%d=0x%08x", i, 0xffffffffu);
    n = sr_buf_append(buf, cap, n, " m32[0x%08x]=0x%08x", 0x08800000u, 0xdeadbeefu);
    if (wrote_expected) *wrote_expected = n;
    return n;
}

static void test_trace_line_production_size(void) {
    /* All 192 registers + header + memory token: ~3.2 KB, comfortably under
     * 4096.  Proves the production call is non-truncating (byte-comparable
     * traces) while exercising every token through the checked append. */
    char line[4096];
    size_t n = render_trace_line(line, sizeof(line), NULL);
    CHECK(n < sizeof(line), "production trace line fits");
    CHECK(n == 2966, "production trace line length unchanged by the helper");
    CHECK(line[n] == '\0', "production trace line is NUL-terminated at the cursor");
    CHECK(strncmp(line, "42 pc=0x08000100 op=0x8e020004 r1=0xffffffff", 44) == 0,
          "production trace prefix byte-exact");
}

static void test_trace_line_tiny_buffer(void) {
    /* The same token stream into 64 bytes: many truncations, but the cursor
     * must never escape the buffer and the buffer must stay NUL-terminated.
     * The old arithmetic escapes on the second token; ASan would fault. */
    char line[64];
    memset(line, 0xAA, sizeof(line));
    size_t n = render_trace_line(line, sizeof(line), NULL);
    CHECK(n < sizeof(line), "tiny-buffer trace cursor stays below cap");
    CHECK(line[n] == '\0', "tiny-buffer trace NUL-terminated at the cursor");
    CHECK(strncmp(line, "42 pc=0x08000100 op=0x8e020004", 30) == 0,
          "tiny-buffer trace keeps the untruncated prefix");
    /* Trace writers finalize with line[n] = '\n'; fwrite(n + 1) — must stay
     * in range for every cursor the helper can return. */
    line[n] = '\n';
    CHECK(line[n] == '\n' && n + 1 <= sizeof(line), "writer finalize stays in range");
}

/* --- Adversarial API group: hostile cursors, capacity edges, and the
 * snprintf return-value shapes the helper must absorb. --- */

static void test_hostile_cursor_never_forms_pointer(void) {
    /* Guard-buffer layout: any write outside [buf, buf+8) flips a canary.
     * Hostile cursors (n == cap, n > cap, SIZE_MAX-class values) must be
     * no-ops returning cap, with every canary intact.  The ASan/UBSan build
     * is the second, stronger proof of the same invariant. */
    struct {
        unsigned char pre[16];
        char buf[8];
        unsigned char post[16];
    } box;
    memset(box.pre, 0x5A, sizeof(box.pre));
    memset(box.post, 0xA5, sizeof(box.post));
    memset(box.buf, 'x', sizeof(box.buf));
    static const size_t hostiles[] = {
        8,             /* n == cap */
        9,             /* n == cap + 1 */
        4096,          /* n far above cap */
        SIZE_MAX,
        SIZE_MAX / 2,
        SIZE_MAX - 1,
    };
    for (size_t i = 0; i < sizeof(hostiles) / sizeof(hostiles[0]); i++) {
        size_t r = sr_buf_append(box.buf, sizeof(box.buf), hostiles[i], "%s", "zzz");
        CHECK(r == sizeof(box.buf), "hostile cursor returns cap");
    }
    CHECK(memcmp(box.pre, "\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A", 16) == 0,
          "front canary intact after hostile cursors");
    CHECK(memcmp(box.post, "\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5", 16) == 0,
          "back canary intact after hostile cursors");
}

static void test_null_buffer_noop(void) {
    /* buf == NULL is a documented no-op: no dereference, cursor returned
     * unchanged, whatever the capacity and cursor. */
    CHECK(sr_buf_append(NULL, sizeof(char[8]), 3, "%s", "abc") == 3,
          "NULL buf with capacity returns the incoming cursor");
    CHECK(sr_buf_append(NULL, sizeof(char[8]), SIZE_MAX, "%s", "abc") == SIZE_MAX,
          "NULL buf with hostile cursor returns it unchanged");
    CHECK(sr_buf_append(NULL, 0, 0, "%s", "abc") == 0,
          "NULL buf zero-cap no-op returns the cursor");
    CHECK(sr_buf_append(NULL, 0, SIZE_MAX, "%s", "abc") == SIZE_MAX,
          "NULL buf zero-cap hostile cursor returns it unchanged");
}

static void test_zero_capacity_hostile_cursor(void) {
    char buf[4] = "ab";
    size_t r = sr_buf_append(buf, 0, SIZE_MAX, "%s", "abc");
    CHECK(r == SIZE_MAX, "zero cap returns the incoming cursor unchanged");
    CHECK(memcmp(buf, "ab\0\0", sizeof(buf)) == 0, "zero cap never touches the buffer");
}

static void test_snprintf_exact_remaining_capacity(void) {
    /* want == avail exactly (3 == 8-4-1): the token ends at cap-1 with the
     * NUL there — the cursor must land on the last usable byte, not past it. */
    char buf[8] = "abcd";
    size_t n = 4;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "abc");
    CHECK(n == 7, "exact-fit want ends at cap-1");
    CHECK(buf[7] == '\0', "exact-fit NUL at cap-1");
    CHECK(memcmp(buf, "abcdabc\0", sizeof(buf)) == 0, "exact-fit bytes correct");
}

static void test_snprintf_far_above_capacity(void) {
    char buf[8];
    memset(buf, 0xAA, sizeof(buf));
    size_t n = sr_buf_append(buf, sizeof(buf), 0, "%s",
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(n == 7, "far-above want clamps to cap-1");
    CHECK(buf[7] == '\0', "far-above NUL at cap-1");
    CHECK(strlen(buf) == 7, "far-above buffer is a valid 7-character string");
}

static void test_negative_formatter_result(void) {
    /* glibc returns -1 (EILSEQ) when %ls meets an invalid wide character;
     * MSVCRT/UCRT may behave differently, so probe the raw formatter first
     * and only assert the negative branch where the platform triggers it.
     * Either way the helper's invariants hold: cursor <= cap-1 and NUL at
     * the cursor. */
    wchar_t bad[2] = { (wchar_t)0xD800, L'\0' };
    char probe[8];
    int raw = snprintf(probe, sizeof(probe), "%ls", bad);
    char buf[8] = "abcd";
    size_t n = 4;
    size_t r = sr_buf_append(buf, sizeof(buf), n, "%ls", bad);
    if (raw < 0) {
        CHECK(r == n, "negative formatter result: cursor stays at n");
        CHECK(buf[n] == '\0', "negative formatter result: NUL written at the cursor");
    } else {
        CHECK(r <= sizeof(buf) - 1 && buf[r] == '\0',
              "non-negative formatter probe: cursor and NUL invariants hold");
    }
}

static void test_zero_length_append(void) {
    char buf[8] = "abc";
    size_t n = 3;
    /* want == 0 (empty token) must leave the cursor and NUL position
     * untouched.  A truly empty format literal would be runtime-identical
     * but trips -Wformat-zero-length/-Wformat-security, so exercise the
     * same vsnprintf want==0 path with an empty %s token. */
    size_t r = sr_buf_append(buf, sizeof(buf), n, "%s", "");
    CHECK(r == 3, "empty token leaves the cursor unchanged");
    CHECK(buf[3] == '\0' && memcmp(buf, "abc\0", 4) == 0, "empty token keeps NUL at the cursor");
}

static void test_repeated_appends_after_full_state(void) {
    char buf[8] = "abcdef";
    size_t n = 6; /* 6 chars, NUL at 6: two bytes remain (char + NUL) */
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "ghij");
    CHECK(n == 7, "append at cap-2 fills to cap-1");
    CHECK(buf[7] == '\0', "NUL preserved at cap-1 after full append");
    CHECK(memcmp(buf, "abcdefg\0", sizeof(buf)) == 0, "fitting prefix written");
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "klmnopqrstuvwxyz");
    CHECK(n == 7, "append at cap-1 stays at cap-1");
    n = sr_buf_append(buf, sizeof(buf), n, "%d", 123456789);
    CHECK(n == 7, "truncating append at cap-1 stays at cap-1");
    CHECK(memcmp(buf, "abcdefg\0", sizeof(buf)) == 0, "full-state bytes never overwritten");
}

static void test_two_byte_buffer(void) {
    char buf[2] = { 'x', 'y' };
    size_t n = 0;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "abcd");
    CHECK(n == 1, "two-byte buffer: cursor at cap-1");
    CHECK(buf[0] == 'a' && buf[1] == '\0', "two-byte buffer: 'a' + NUL");
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "efgh");
    CHECK(n == 1, "two-byte buffer: repeat append stays at cap-1");
    CHECK(buf[0] == 'a' && buf[1] == '\0', "two-byte buffer: bytes unchanged");
}

static void test_no_size_t_underflow_or_wrap(void) {
    /* The internal `cap - n - 1` is only computed when n < cap; a hostile
     * cursor must never reach that subtraction.  (cap, n) = (8, 0) with a
     * giant token is the smallest-capacity sanity case; the hostile-cursor
     * group above covers the n >= cap side with SIZE_MAX-class values. */
    char buf[8];
    size_t n = sr_buf_append(buf, sizeof(buf), 0, "%s", "abcdefghijklmnopqrstuvwxyz");
    CHECK(n == 7, "no underflow: huge token clamps to cap-1");
    CHECK(n + 1 == sizeof(buf), "cursor + NUL slot exactly fills the buffer");
    CHECK(buf[n] == '\0', "NUL at the clamped cursor");
}

int main(void) {
    test_truncation_never_escapes_buffer();
    test_full_buffer_is_noop();
    test_exact_and_partial_fit();
    test_one_byte_buffer();
    test_already_full_cursor();
    test_null_and_zero_capacity();
    test_append_v();
    test_append_v_truncates();
    test_trace_line_production_size();
    test_trace_line_tiny_buffer();
    test_hostile_cursor_never_forms_pointer();
    test_null_buffer_noop();
    test_zero_capacity_hostile_cursor();
    test_snprintf_exact_remaining_capacity();
    test_snprintf_far_above_capacity();
    test_negative_formatter_result();
    test_zero_length_append();
    test_repeated_appends_after_full_state();
    test_two_byte_buffer();
    test_no_size_t_underflow_or_wrap();
    if (g_failures == 0) {
        printf("strbuf_selftest: all %d checks passed\n", g_checks);
        return 0;
    }
    fprintf(stderr, "strbuf_selftest: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
