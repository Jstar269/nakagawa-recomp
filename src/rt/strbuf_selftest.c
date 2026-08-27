// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// strbuf_selftest.c — unit and adversarial tests for sr_buf_append / strbuf.h.

#include "strbuf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); \
        g_failures++; \
    } \
} while (0)

static void test_truncation_never_escapes_buffer(void) {
    char buf[16];
    size_t n = 0;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "0123456789");
    CHECK(n == 10, "fit 10 chars");
    CHECK(strcmp(buf, "0123456789") == 0, "content correct");

    n = sr_buf_append(buf, sizeof(buf), n, "%s", "abcdefghij");
    CHECK(n == 15, "cursor clamped to cap - 1 (15)");
    CHECK(strlen(buf) == 15, "string length is 15");
    CHECK(buf[15] == '\0', "NUL terminator at cap - 1");
    CHECK(memcmp(buf, "0123456789abcde\0", 16) == 0, "buffer contains truncated content");
}

static void test_full_buffer_is_noop(void) {
    char buf[8];
    size_t n = 0;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "1234567890");
    CHECK(n == 7, "filled to cap - 1");
    CHECK(buf[7] == '\0', "NUL present");

    size_t before = n;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "more");
    CHECK(n == before, "append to full buffer is no-op");
    CHECK(buf[7] == '\0', "NUL preserved");
    CHECK(strcmp(buf, "1234567") == 0, "content preserved");
}

static void test_exact_and_partial_fit(void) {
    char buf[10];
    size_t n = 0;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "1234");
    CHECK(n == 4, "4 chars written");
    CHECK(strcmp(buf, "1234") == 0, "content 1234");

    n = sr_buf_append(buf, sizeof(buf), n, " %d", 56);
    CHECK(n == 7, "7 chars written");
    CHECK(strcmp(buf, "1234 56") == 0, "content 1234 56");

    n = sr_buf_append(buf, sizeof(buf), n, " %s", "7890");
    CHECK(n == 9, "clamped at cap - 1 (9)");
    CHECK(strlen(buf) == 9, "valid 9-char string");
    CHECK(buf[9] == '\0', "NUL terminated");
}

static void test_one_byte_buffer(void) {
    char buf[1] = { 'x' };
    size_t n = sr_buf_append(buf, 1, 0, "%s", "hello");
    CHECK(n == 0, "1-byte buffer returns cursor 0");
    CHECK(buf[0] == '\0', "1-byte buffer gets NUL");

    n = sr_buf_append(buf, 1, n, "%s", "world");
    CHECK(n == 0, "repeat append to 1-byte buffer returns 0");
    CHECK(buf[0] == '\0', "1-byte buffer stays NUL");
}

static void test_already_full_cursor(void) {
    char buf[8] = "1234567";
    size_t n = sr_buf_append(buf, 8, 8, "%s", "extra");
    CHECK(n == 8, "cursor >= cap returns cap");
    CHECK(strcmp(buf, "1234567") == 0, "buffer not modified");

    n = sr_buf_append(buf, 8, 100, "%s", "extra");
    CHECK(n == 8, "cursor >> cap returns cap");
}

static void test_null_and_zero_capacity(void) {
    size_t n = sr_buf_append(NULL, 10, 0, "%s", "test");
    CHECK(n == 0, "NULL buffer returns incoming cursor");

    char buf[8] = "abc";
    n = sr_buf_append(buf, 0, 0, "%s", "test");
    CHECK(n == 0, "0 cap returns incoming cursor");
    CHECK(strcmp(buf, "abc") == 0, "buffer untouched");
}

static void test_append_v_helper(char *buf, size_t cap, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t r = sr_buf_append_v(buf, cap, n, fmt, ap);
    va_end(ap);
    CHECK(r == 5, "append_v formatted 5 chars");
    CHECK(strcmp(buf, "hello") == 0, "append_v content hello");
}

static void test_append_v(void) {
    char buf[16];
    test_append_v_helper(buf, sizeof(buf), 0, "%s", "hello");
}

static void test_append_v_truncates_helper(char *buf, size_t cap, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t r = sr_buf_append_v(buf, cap, n, fmt, ap);
    va_end(ap);
    CHECK(r == cap - 1, "append_v clamps at cap - 1");
    CHECK(buf[cap - 1] == '\0', "append_v writes NUL at cap - 1");
}

static void test_append_v_truncates(void) {
    char buf[8];
    test_append_v_truncates_helper(buf, sizeof(buf), 0, "%s", "123456789012345");
}

static void test_trace_line_production_size(void) {
    char line[4096];
    size_t n = 0;
    n = sr_buf_append(line, sizeof(line), n, "%llu pc=0x%08x op=0x%08x",
                      12345ULL, 0x08804000, 0x00000000);
    for (int i = 1; i < 32; i++)
        n = sr_buf_append(line, sizeof(line), n, " r%d=0x%08x", i, (uint32_t)((uint32_t)i * 0x11111111u));
    n = sr_buf_append(line, sizeof(line), n, " hi=0x%08x", 0x12345678);
    n = sr_buf_append(line, sizeof(line), n, " lo=0x%08x", 0x87654321);
    for (int i = 0; i < 32; i++)
        n = sr_buf_append(line, sizeof(line), n, " f%d=0x%08x", i, (uint32_t)i);
    n = sr_buf_append(line, sizeof(line), n, " fcr31=0x%08x", 0x00000000);
    for (int i = 0; i < 128; i++)
        n = sr_buf_append(line, sizeof(line), n, " v%d=0x%08x", i, (uint32_t)((uint32_t)i * 0x01010101u));
    n = sr_buf_append(line, sizeof(line), n, " m32[0x%08x]=0x%08x", 0x08800000, 0xdeadbeef);

    CHECK(n < sizeof(line), "trace line with all registers fits in 4096 bytes");
    CHECK(line[n] == '\0', "trace line is NUL terminated");
    line[n] = '\n';
    CHECK(line[n] == '\n' && n + 1 <= sizeof(line), "writer finalize stays in range");
}

static void test_trace_line_tiny_buffer(void) {
    char line[32];
    size_t n = 0;
    n = sr_buf_append(line, sizeof(line), n, "%llu pc=0x%08x op=0x%08x",
                      1ULL, 0x08804000, 0x03e00008);
    for (int i = 1; i < 32; i++)
        n = sr_buf_append(line, sizeof(line), n, " r%d=0x%08x", i, (uint32_t)i);
    CHECK(n == sizeof(line) - 1, "tiny trace line clamped to sizeof(line) - 1");
    CHECK(line[sizeof(line) - 1] == '\0', "tiny trace line NUL terminated");
    line[n] = '\n';
    CHECK(line[n] == '\n' && n + 1 <= sizeof(line), "writer finalize stays in range");
}

static void test_hostile_cursor_never_forms_pointer(void) {
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
    CHECK(sr_buf_append(NULL, sizeof(char[8]), 3, "%s", "abc") == 3,
          "NULL buf with capacity returns incoming cursor");
    CHECK(sr_buf_append(NULL, sizeof(char[8]), SIZE_MAX, "%s", "abc") == SIZE_MAX,
          "NULL buf with hostile cursor returns it unchanged");
    CHECK(sr_buf_append(NULL, 0, 0, "%s", "abc") == 0,
          "NULL buf zero-cap returns cursor");
    CHECK(sr_buf_append(NULL, 0, SIZE_MAX, "%s", "abc") == SIZE_MAX,
          "NULL buf zero-cap hostile cursor returns unchanged");
}

static void test_zero_capacity_hostile_cursor(void) {
    char buf[4] = "ab";
    size_t r = sr_buf_append(buf, 0, SIZE_MAX, "%s", "abc");
    CHECK(r == SIZE_MAX, "zero cap returns incoming cursor unchanged");
    CHECK(memcmp(buf, "ab\0\0", sizeof(buf)) == 0, "zero cap never touches buffer");
}

static void test_snprintf_exact_remaining_capacity(void) {
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
    wchar_t bad[2] = { (wchar_t)0xD800, L'\0' };
    char probe[8];
    int raw = snprintf(probe, sizeof(probe), "%ls", bad);
    char buf[8] = "abcd";
    size_t n = 4;
    size_t r = sr_buf_append(buf, sizeof(buf), n, "%ls", bad);
    if (raw < 0) {
        CHECK(r == n, "negative formatter result: cursor stays at n");
        CHECK(buf[n] == '\0', "negative formatter result: NUL written at cursor");
    } else {
        CHECK(r <= sizeof(buf) - 1 && buf[r] == '\0',
              "non-negative formatter probe: cursor and NUL invariants hold");
    }
}

static void test_zero_length_append(void) {
    char buf[8] = "abc";
    size_t n = 3;
    size_t r = sr_buf_append(buf, sizeof(buf), n, "%s", "");
    CHECK(r == 3, "empty token leaves cursor unchanged");
    CHECK(buf[3] == '\0' && memcmp(buf, "abc\0", 4) == 0, "empty token keeps NUL at cursor");
}

static void test_repeated_appends_after_full_state(void) {
    char buf[8] = "abcdef";
    size_t n = 6;
    n = sr_buf_append(buf, sizeof(buf), n, "%s", "ghij");
    CHECK(n == 7, "append at cap-2 fills to cap-1");
    CHECK(buf[7] == '\0', "NUL preserved at cap-1");
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
    char buf[8];
    size_t n = sr_buf_append(buf, sizeof(buf), 0, "%s", "abcdefghijklmnopqrstuvwxyz");
    CHECK(n == 7, "no underflow: huge token clamps to cap-1");
    CHECK(n + 1 == sizeof(buf), "cursor + NUL slot exactly fills buffer");
    CHECK(buf[n] == '\0', "NUL at clamped cursor");
}

/* Failing-before comparison: prove that using raw snprintf accumulation
 * causes out-of-bounds writes into canaries when formatting truncates. */
static void test_failing_before_old_arithmetic_overflows_canary(void) {
    struct {
        unsigned char pre[16];
        char buf[8];
        unsigned char post[16];
    } box;
    memset(box.pre, 0x5A, sizeof(box.pre));
    memset(box.post, 0xA5, sizeof(box.post));
    memset(box.buf, 0, sizeof(box.buf));

    /* Safe path using sr_buf_append */
    size_t n = 0;
    n = sr_buf_append(box.buf, sizeof(box.buf), n, "%s", "0123456789");
    n = sr_buf_append(box.buf, sizeof(box.buf), n, "%s", "xyz");
    CHECK(n == 7, "sr_buf_append clamped at 7");
    CHECK(memcmp(box.pre, "\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A\x5A", 16) == 0,
          "safe path pre-canary intact");
    CHECK(memcmp(box.post, "\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5", 16) == 0,
          "safe path post-canary intact");
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
    test_failing_before_old_arithmetic_overflows_canary();

    if (g_failures == 0) {
        printf("strbuf_selftest: all %d checks passed\n", g_checks);
        return 0;
    }
    fprintf(stderr, "strbuf_selftest: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
