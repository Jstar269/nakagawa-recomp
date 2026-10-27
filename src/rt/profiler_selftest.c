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

/* PERF_AUDIO (#1296): the host-queue depth seen before each push and the silence of each
 * underrun, per channel, so an audio gap can be told apart from a guest that is merely slow.
 * Channel 8 gets a full queue, a nearly drained one, and an underrun after 20 ms of silence;
 * channel 0 gets one push; the other channels are silent and print nothing. */
static void test_audio_queue_line(void) {
    sr_perf_init();
    sr_perf_test_set_report_sink(capture_report_line);
    s_report_count = 0;
    sr_perf_audio_queue(8, 4410u, 0u, 0);
    sr_perf_audio_queue(8, 441u, 0u, 0);
    sr_perf_audio_queue(8, 0u, 20000000u, 1);
    sr_perf_audio_queue(0, 2205u, 0u, 0);
    sr_perf_audio_queue(SR_PERF_AUDIO_CHANNELS, 1u, 1u, 1);
    sr_perf_test_force_report();
    sr_perf_test_set_report_sink(NULL);
    CHECK(s_report_count == 4,
          "one interval with two pushed channels emitted %d line units (expected PERF, "
          "PERF_ATTRIB and two PERF_AUDIO)", s_report_count);
    CHECK(whole_line(2, "PERF_AUDIO ch=0 ") &&
              strcmp(s_report_lines[2], "PERF_AUDIO ch=0 pushes=1 q_min_ms=50.0 q_max_ms=50.0 "
                                        "underruns=0 starved_ms=0.000\n") == 0,
          "channel 0 audio line was '%s'", s_report_count > 2 ? s_report_lines[2] : "");
    CHECK(whole_line(3, "PERF_AUDIO ch=8 ") &&
              strcmp(s_report_lines[3], "PERF_AUDIO ch=8 pushes=3 q_min_ms=0.0 q_max_ms=100.0 "
                                        "underruns=1 starved_ms=20.000\n") == 0,
          "channel 8 audio line was '%s'", s_report_count > 3 ? s_report_lines[3] : "");

    /* The next interval starts empty: no push, no PERF_AUDIO line. */
    s_report_count = 0;
    sr_perf_test_set_report_sink(capture_report_line);
    sr_perf_test_force_report();
    sr_perf_test_set_report_sink(NULL);
    CHECK(s_report_count == 2, "an interval without pushes emitted %d line units (expected 2)",
          s_report_count);
}

/* Guest symbol labels (codegen --symbol-map). The registered table is what the crash report and
 * the dispatch trace name addresses with; without a table every label is empty, so a build with
 * no map prints exactly what it printed before. Address 0 is a real function start on a zero-based
 * image, like the profiler's PC zero above. */
static void expect_label(uint32_t pc, const char *want) {
    char buf[64];
    const char *got = sr_guest_symbol_label(pc, buf, sizeof buf);
    CHECK(strcmp(got, want) == 0, "label for 0x%08" PRIx32 " is \"%s\" (expected \"%s\")",
          pc, got, want);
}

static void test_guest_symbol_labels(void) {
    static const SrGuestSymbol table[] = {
        {0x00000000u, 0x00000010u, "entry_stub"},
        {0x00000100u, 0x00000040u, "ball_update"},
        {0x00000200u, 0x00000000u, "game_loop"},     /* no size: only the start is named */
    };
    expect_label(0x00000100u, "");                    /* nothing registered yet */
    sr_guest_symbols_register(table, 3u);
    expect_label(0x00000000u, "entry_stub");
    expect_label(0x0000000cu, "entry_stub+0xc");
    expect_label(0x00000010u, "");                    /* one past the size */
    expect_label(0x00000100u, "ball_update");
    expect_label(0x0000013cu, "ball_update+0x3c");
    expect_label(0x00000140u, "");
    expect_label(0x00000200u, "game_loop");
    expect_label(0x00000204u, "");
    expect_label(0xfffffffcu, "");
    CHECK(sr_guest_symbol_at(0x00000120u) == &table[1], "0x120 resolves to the wrong entry");
    char tiny[8];
    CHECK(strcmp(sr_guest_symbol_label(0x0000013cu, tiny, sizeof tiny), "ball_up") == 0,
          "a short buffer is not truncated to its size");
    CHECK(sr_guest_symbol_label(0x00000100u, tiny, 0u)[0] == '\0', "a zero-size buffer is written");
    sr_guest_symbols_register(NULL, 0u);
    expect_label(0x00000100u, "");
}

int main(void) {
    test_telemetry_lines_are_emitted_whole();
    test_audio_queue_line();
    test_guest_symbol_labels();
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
