// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the psp-recomp authors

#define _POSIX_C_SOURCE 200809L

#include "recomp.h"
#include "perf.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_checks;
static int s_failures;

#define CHECK(cond, ...) do {                                                   \
    s_checks++;                                                                 \
    if (!(cond)) {                                                              \
        s_failures++;                                                           \
        fprintf(stderr, "FAIL: ");                                             \
        fprintf(stderr, __VA_ARGS__);                                           \
        fputc('\n', stderr);                                                    \
    }                                                                           \
} while (0)

/* Telemetry emission unit.  A PERF line is written from the guest thread, and guest time is
 * host time, so how many stdio calls a line costs is guest-visible latency: on the MinGW C
 * runtime a formatted fprintf() to the unbuffered stderr stopped the flagship for ~25-30 ms
 * per line (~55 ms every second under SR_PERF), which made a two-VBLANK-gated title run at 2.26
 * VBLANKs per frame instead of 2.02.  The contract pinned here is that each telemetry line
 * reaches the emitter as one finished, newline-terminated string, so the stream sees one write
 * per line.  The production report path is driven through the test sink; with the old direct
 * fprintf(stderr, ...) the sink is never reached and the line count below is 0. */
static char s_report_lines[4][2048];
static size_t s_report_lens[4];
static int s_report_count;

static void capture_report_line(const char *line, size_t len) {
    if (s_report_count < 4 && len < sizeof s_report_lines[0]) {
        memcpy(s_report_lines[s_report_count], line, len);
        s_report_lines[s_report_count][len] = '\0';
        s_report_lens[s_report_count] = len;
    }
    s_report_count++;
}

static int whole_line(int i, const char *prefix) {
    if (i >= s_report_count || i >= 4) return 0;
    const char *l = s_report_lines[i];
    size_t n = s_report_lens[i];
    return n > strlen(prefix) && strncmp(l, prefix, strlen(prefix)) == 0 &&
           n == strlen(l) && l[n - 1u] == '\n' && strchr(l, '\n') == l + n - 1u;
}

static void test_telemetry_lines_are_emitted_whole(void) {
#ifdef _WIN32
    putenv("SR_PERF=1");
#else
    setenv("SR_PERF", "1", 1);
#endif
    sr_perf_init();
    sr_perf_test_set_report_sink(capture_report_line);
    s_report_count = 0;
    sr_perf_ge_frontend_profile_config(8, 1);
    sr_perf_ge_frontend_profile(SR_PERF_GE_FRONTEND_COMMAND_DISPATCH, 240, 3, 3);
    sr_perf_ge_frontend_profile(SR_PERF_GE_FRONTEND_DRAW_SETUP, 100, 2, 8);
    sr_perf_test_force_report();
    sr_perf_test_set_report_sink(NULL);
    CHECK(s_report_count == 3,
          "one telemetry interval emitted %d line units (expected PERF, PERF_ATTRIB, and GE frontend)",
          s_report_count);
    CHECK(whole_line(0, "PERF vblank_total="),
          "the PERF line was not delivered as one whole newline-terminated string");
    CHECK(whole_line(0, "PERF vblank_total=") && strstr(s_report_lines[0], " target30=") != NULL,
          "the PERF line lost its final field");
    CHECK(whole_line(1, "PERF_ATTRIB aot_ms="),
          "the PERF_ATTRIB line was not delivered as one whole newline-terminated string");
    CHECK(whole_line(1, "PERF_ATTRIB aot_ms=") && strstr(s_report_lines[1], " output_ms=") != NULL,
          "the PERF_ATTRIB line lost its final field");
    CHECK(whole_line(2, "PERF_GE_FRONTEND stride=8 calibration=1"),
          "the GE frontend profile was not delivered as one whole line");
    CHECK(whole_line(2, "PERF_GE_FRONTEND stride=8 calibration=1") &&
              strstr(s_report_lines[2],
                     "draw_setup_sample_ns=100 draw_setup_samples=2 draw_setup_eligible=8") != NULL,
          "the GE frontend profile lost sampled timing or eligibility counts");
}

int main(void) {
    test_telemetry_lines_are_emitted_whole();
    sr_profile_test_reset();

    /* HST and other zero-based PSP images can legitimately execute PC zero. It must be a
     * normal key, not the profiler table's empty-slot sentinel. */
    sr_profile_block(0);
    sr_profile_block(0);
    CHECK(sr_profile_test_block_count(0) == 2,
          "PC zero count=%" PRIu64 " (expected 2)", sr_profile_test_block_count(0));
    CHECK(sr_profile_test_lookup_drops() == 0,
          "unexpected lookup drop after PC-zero insert");

    sr_profile_block(4);
    CHECK(sr_profile_test_block_count(4) == 1,
          "ordinary PC count=%" PRIu64 " (expected 1)", sr_profile_test_block_count(4));

    /* The hash table deliberately bounds a lookup to 64 probes. Multiples of the table size
     * have identical low hash bits, so this fills one probe window deterministically. */
    for (uint32_t i = 1; i < 64; i++) {
        sr_profile_block(i * 131072u);
    }
    CHECK(sr_profile_test_lookup_drops() == 0,
          "lookup dropped before the 64-probe window was full");

    sr_profile_block(64u * 131072u);
    CHECK(sr_profile_test_lookup_drops() == 1,
          "saturated lookup drops=%" PRIu64 " (expected 1)",
          sr_profile_test_lookup_drops());
    CHECK(sr_profile_test_block_count(64u * 131072u) == 0,
          "dropped key unexpectedly appeared in the table");
    CHECK(sr_profile_test_block_count(0) == 2,
          "collision pressure corrupted the PC-zero entry");

    printf("profiler selftest: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
