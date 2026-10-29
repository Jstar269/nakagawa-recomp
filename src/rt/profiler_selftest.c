// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

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

/* Live profile view (#314). The history ring keeps the newest SR_PERF_HISTORY_LEN closed
 * intervals, ages from the newest, and drops the oldest when full. */
static void test_live_history_ring(void) {
    static SrPerfHistory history;
    SrPerfLiveInterval interval;
    sr_perf_history_reset(&history);
    CHECK(sr_perf_history_at(&history, 0) == NULL, "an empty ring has no newest interval");
    memset(&interval, 0, sizeof(interval));
    for (unsigned i = 1; i <= 3; i++) {
        interval.wall_ns = i;
        sr_perf_history_push(&history, &interval);
    }
    CHECK(history.count == 3, "three pushes hold three intervals (count=%u)", history.count);
    CHECK(sr_perf_history_at(&history, 0) && sr_perf_history_at(&history, 0)->wall_ns == 3,
          "age 0 is the newest push");
    CHECK(sr_perf_history_at(&history, 2) && sr_perf_history_at(&history, 2)->wall_ns == 1,
          "age 2 is the oldest of three");
    CHECK(sr_perf_history_at(&history, 3) == NULL, "no interval past the oldest held");
    for (unsigned i = 4; i <= SR_PERF_HISTORY_LEN + 5u; i++) {
        interval.wall_ns = i;
        sr_perf_history_push(&history, &interval);
    }
    CHECK(history.count == SR_PERF_HISTORY_LEN, "a full ring holds %u (count=%u)",
          SR_PERF_HISTORY_LEN, history.count);
    CHECK(sr_perf_history_at(&history, 0)->wall_ns == SR_PERF_HISTORY_LEN + 5u,
          "the newest survives a wrap");
    CHECK(sr_perf_history_at(&history, SR_PERF_HISTORY_LEN - 1u)->wall_ns == 6u,
          "the oldest held after a wrap is the first not dropped (wall_ns=%llu)",
          (unsigned long long)sr_perf_history_at(&history, SR_PERF_HISTORY_LEN - 1u)->wall_ns);
    CHECK(sr_perf_history_at(&history, SR_PERF_HISTORY_LEN) == NULL, "no age past the ring");
    history.head = SR_PERF_HISTORY_LEN;
    CHECK(sr_perf_history_at(&history, 0) == NULL, "a corrupt head reads nothing");
    sr_perf_history_push(&history, &interval);
    CHECK(history.head == SR_PERF_HISTORY_LEN, "a corrupt head is not written through");
    sr_perf_history_push(NULL, &interval);
    CHECK(sr_perf_history_at(NULL, 0) == NULL, "a NULL ring is empty");
}

/* The rows are the PERF line's definitions: shares of the interval's wall time for the
 * guest/host split and residency, shares of the recorded thread states for the scheduler. */
static void test_live_rows(void) {
    SrPerfLiveInterval iv;
    SrPerfLiveRow rows[SR_PERF_LIVE_ROWS];
    memset(&iv, 0, sizeof(iv));
    CHECK(sr_perf_live_rows(&iv, rows, SR_PERF_LIVE_ROWS) == 0, "an empty interval has no rows");
    iv.wall_ns = 1000000000ull;
    iv.cpu_ns = 600000000ull;
    iv.ge_wait_ns = 100000000ull;
    iv.present_ns = 50000000ull;
    iv.idle_ns = 250000000ull;
    iv.presents = 60;
    iv.new_frames = 30;
    iv.aot_ns = 550000000ull;
    iv.interp_ns = 25000000ull;
    iv.aot_to_interp = 12;
    iv.interp_to_aot = 11;
    iv.sched_ns[SR_PERF_SCHED_RUNNING] = 300;
    iv.sched_ns[SR_PERF_SCHED_RUNNABLE] = 100;
    iv.sched_ns[SR_PERF_SCHED_BLOCKED] = 500;
    iv.sched_ns[SR_PERF_SCHED_IDLE] = 100;
    iv.ge_cpu_ns = 3100000ull;
    iv.gpu_draws = 812;
    iv.vk_submit_ns = 400000ull;
    iv.vk_wait_ns = 5200000ull;
    iv.vk_readback_ns = 100000ull;
    iv.audio_underruns = 2;
    CHECK(sr_perf_live_rows(&iv, rows, SR_PERF_LIVE_ROWS - 1u) == 0, "a short row buffer is refused");
    CHECK(sr_perf_live_rows(&iv, rows, SR_PERF_LIVE_ROWS) == SR_PERF_LIVE_ROWS, "eight rows");
    static const char *const want[SR_PERF_LIVE_ROWS][2] = {
        {"Frame", "30.0 game fps  16.7 ms per present"},
        {"Guest/host", "cpu 60%  ge wait 10%  present 5%  idle 25%"},
        {"Residency", "aot 55%  interpreter 3%"},
        {"Transitions", "12 to interpreter  11 to aot"},
        {"Scheduler", "run 30%  ready 10%  blocked 50%  idle 10%"},
        {"GE CPU", "3.1 ms  812 gpu draws"},
        {"Vulkan", "submit 0.4 wait 5.2 pipeline 0.0 readback 0.1 ms"},
        {"Audio", "2 underruns"},
    };
    for (unsigned i = 0; i < SR_PERF_LIVE_ROWS; i++) {
        CHECK(strcmp(rows[i].label, want[i][0]) == 0 && strcmp(rows[i].value, want[i][1]) == 0,
              "row %u was '%s' '%s' (expected '%s' '%s')", i, rows[i].label, rows[i].value,
              want[i][0], want[i][1]);
    }
    memset(iv.sched_ns, 0, sizeof(iv.sched_ns));
    (void)sr_perf_live_rows(&iv, rows, SR_PERF_LIVE_ROWS);
    CHECK(strcmp(rows[4].value, "no thread states recorded") == 0,
          "no thread states reads as such, not as 0%% everywhere");
}

/* Each closed telemetry interval lands in the history the live view reads, carrying the
 * interval's counters, and the transition hot spots are the summary's top_pcs. */
static void test_live_history_follows_the_report(void) {
    SrPerfTopPc top[4];
    sr_perf_init();
    unsigned before = sr_perf_history()->count;
    sr_perf_test_set_report_sink(capture_report_line);
    s_report_count = 0;
    sr_perf_audio_queue(3, 0u, 1000000u, 1);
    sr_perf_audio_queue(5, 0u, 1000000u, 1);
    for (int i = 0; i < 3; i++) {
        sr_perf_aot_begin(0x08804000u);
        sr_perf_interp_set_reason(SR_PERF_INTERP_STALE_BLOCK);
        sr_perf_interp_begin(0x08804120u);
        sr_perf_interp_end(0x08804130u, 0);
        sr_perf_aot_end();
    }
    sr_perf_aot_begin(0x08805000u);
    sr_perf_interp_begin(0x08805200u);
    sr_perf_interp_end(0x08805210u, 0);
    sr_perf_aot_end();
    sr_perf_test_force_report();
    sr_perf_test_set_report_sink(NULL);
    const SrPerfLiveInterval *newest = sr_perf_history_at(sr_perf_history(), 0);
    CHECK(sr_perf_history()->count == before + 1u, "one report pushed one interval (%u -> %u)",
          before, sr_perf_history()->count);
    CHECK(newest && newest->wall_ns > 0 && newest->audio_underruns == 2 &&
              newest->aot_to_interp == 4,
          "the newest interval carries the report's counters (underruns=%llu aot_to_interp=%llu)",
          newest ? (unsigned long long)newest->audio_underruns : 0ull,
          newest ? (unsigned long long)newest->aot_to_interp : 0ull);
    unsigned n = sr_perf_top_transitions(0, top, 4);
    CHECK(n == 2 && top[0].pc == 0x08804120u && top[0].count == 3 &&
              strcmp(top[0].reason, "stale-block") == 0 && top[1].pc == 0x08805200u &&
              strcmp(top[1].reason, "dispatch-miss") == 0,
          "top AOT->interpreter PCs, most frequent first (n=%u)", n);
    CHECK(sr_perf_top_transitions(0, top, 1) == 1, "the top list stops at the caller's max");
    CHECK(sr_perf_top_transitions(2, top, 4) == 0, "an unknown direction has no entries");
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

/* New game frames versus presents (#1296). Hot Shots Tennis hands each finished frame to the
 * window twice (the same buffer immediately and for the next frame), so presents ran about twice
 * its real frame rate and the HUD's FPS read 35-39 where the game drew 18. Here two buffers
 * alternate, each presented twice, a new frame every 3 VBLANKs: 7 presents, 4 new frames over
 * 3 intervals of 3 VBLANKs. The forced report closes a 2 s interval. The next interval re-presents
 * the last buffer (no new frame), then flips 5 VBLANKs after the previous new frame. */
static void test_new_frames_are_distinct_buffers(void) {
    sr_perf_init();
    static const uint32_t buffers[] = { 0x04000000u, 0x04000000u, 0x04044000u, 0x04044000u,
                                        0x04000000u, 0x04000000u, 0x04044000u };
    static const uint32_t vcounts[] = { 10u, 10u, 13u, 13u, 16u, 16u, 19u };
    for (unsigned i = 0; i < sizeof buffers / sizeof buffers[0]; i++) {
        sr_perf_present_done(0, 1);
        sr_perf_present_frame(buffers[i], vcounts[i]);
    }
    sr_perf_test_force_report();
    double presents = 0.0, frame_ms = 0.0, vblank_hz = 0.0, game_fps = 0.0, per_frame = 0.0;
    sr_perf_get_hud_metrics(&presents, &frame_ms, &vblank_hz);
    sr_perf_get_hud_frames(&game_fps, &per_frame);
    CHECK(presents == 3.5, "presents per second %.3f (expected 3.5: 7 presents in 2 s)", presents);
    CHECK(game_fps == 2.0, "game frames per second %.3f (expected 2.0: 4 new frames in 2 s)",
          game_fps);
    CHECK(per_frame == 3.0, "VBLANKs per new frame %.3f (expected 3.0)", per_frame);

    sr_perf_present_done(0, 1);
    sr_perf_present_frame(0x04044000u, 22u);
    sr_perf_present_done(0, 1);
    sr_perf_present_frame(0x04000000u, 24u);
    sr_perf_test_force_report();
    sr_perf_get_hud_frames(&game_fps, &per_frame);
    CHECK(game_fps == 0.5, "game frames per second %.3f in the second interval (expected 0.5)",
          game_fps);
    CHECK(per_frame == 5.0, "VBLANKs per new frame %.3f in the second interval (expected 5.0)",
          per_frame);
}

/* Execution share (perf.h): always on, so it is driven here with SR_PERF unset. */
static void spin_ns(uint64_t ns) {
    uint64_t until = sr_exec_now_ns() + ns;
    while (sr_exec_now_ns() < until) {
    }
}

static unsigned long long json_u64(const char *json, const char *key) {
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\": ", key);
    const char *at = strstr(json, needle);
    return at ? strtoull(at + strlen(needle), NULL, 10) : ~0ull;
}

static void test_exec_share(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/nk_exec_share_selftest_%ld.json", tmp, (long)sr_exec_now_ns());
#ifdef _WIN32
    _putenv_s("SR_EXEC_SHARE_JSON", path);
#else
    setenv("SR_EXEC_SHARE_JSON", path, 1);
#endif
    sr_exec_slice_begin(SR_EXEC_TIER_NONE);
    int outer = sr_exec_enter(SR_EXEC_TIER_AOT);
    CHECK(outer == SR_EXEC_TIER_NONE && sr_exec_tier == SR_EXEC_TIER_AOT, "enter AOT from none");
    int same = sr_exec_enter(SR_EXEC_TIER_AOT);   /* AOT to AOT: no switch */
    sr_exec_leave(same);
    CHECK(sr_exec_tier == SR_EXEC_TIER_AOT, "an AOT-to-AOT dispatch keeps the AOT tier");
    spin_ns(2000000u);
    uint64_t idle_started = sr_exec_now_ns();
    spin_ns(20000000u);
    sr_exec_idle(idle_started);                   /* 20 ms of host wait: not guest work */

    sr_perf_interp_set_reason(SR_PERF_INTERP_STALE_BLOCK);
    int from_aot = sr_exec_enter(SR_EXEC_TIER_INTERP);
    sr_exec_interp_entry(0x00001000u);
    sr_exec_interp_instructions += 5u;
    spin_ns(1000000u);
    int nested = sr_exec_enter(SR_EXEC_TIER_AOT);   /* a linked call from the floor */
    spin_ns(10000000u);
    sr_exec_leave(nested);
    CHECK(sr_exec_tier == SR_EXEC_TIER_INTERP, "leaving a nested AOT call restores the floor");
    sr_exec_interp_exit(1, 0);
    sr_exec_leave(from_aot);
    CHECK(sr_exec_tier == SR_EXEC_TIER_AOT, "leaving the floor restores AOT");

    for (unsigned i = 0; i < 2u; i++) {           /* two dispatch misses, one target twice */
        int prev = sr_exec_enter(SR_EXEC_TIER_INTERP);
        sr_exec_interp_entry(i ? 0x00002000u : 0x00001000u);
        sr_exec_interp_exit(1, 1);
        sr_exec_leave(prev);
    }
    sr_exec_leave(outer);
    int saved = sr_exec_slice_end();
    CHECK(saved == SR_EXEC_TIER_NONE, "slice ends with the tier it began with");

    sr_exec_share_report();                       /* not armed: writes nothing */
    FILE *probe = fopen(path, "r");
    CHECK(probe == NULL, "an unarmed report writes no file");
    if (probe) fclose(probe);
    sr_exec_share_arm();
    sr_exec_share_report();
    char json[2048] = {0};
    FILE *in = fopen(path, "r");
    CHECK(in != NULL, "SR_EXEC_SHARE_JSON file written");
    if (!in) return;
    size_t got = fread(json, 1, sizeof json - 1u, in);
    fclose(in);
    remove(path);
    json[got] = '\0';
    CHECK(strstr(json, "\"schema\": \"nakagawa-exec-share-v1\"") != NULL, "schema: %s", json);
    CHECK(json_u64(json, "interp_entries") == 3u, "interp_entries: %s", json);
    CHECK(json_u64(json, "stale_block") == 1u && json_u64(json, "dispatch_miss") == 2u,
          "entries by reason: %s", json);
    CHECK(json_u64(json, "aot_handoff") == 1u && json_u64(json, "call_return") == 2u &&
          json_u64(json, "other") == 0u, "exits: %s", json);
    CHECK(json_u64(json, "interp_entry_points") == 2u, "distinct entry points: %s", json);
    CHECK(json_u64(json, "miss_targets") == 2u, "distinct miss targets: %s", json);
    CHECK(json_u64(json, "interp_instructions") == 5u, "interp instructions: %s", json);
    unsigned long long aot_ns = json_u64(json, "aot_ns");
    unsigned long long interp_ns = json_u64(json, "interp_ns");
    unsigned long long idle_ns = json_u64(json, "idle_ns");
    /* Spins: AOT 2 ms + a 10 ms nested call, the floor 1 ms, idle 20 ms. The bounds leave
     * room for a busy host while still telling which tier each span went to. */
    CHECK(interp_ns >= 1000000u && interp_ns < 9000000u,
          "floor time excludes its nested AOT call: interp_ns=%llu", interp_ns);
    CHECK(aot_ns >= 12000000u && aot_ns < 25000000u,
          "AOT time includes the nested call and excludes the idle wait: aot_ns=%llu", aot_ns);
    CHECK(idle_ns >= 20000000u, "idle_ns=%llu", idle_ns);
    const char *share = strstr(json, "\"interp_time_share\": ");
    double value = share ? strtod(share + strlen("\"interp_time_share\": "), NULL) : -1.0;
    CHECK(value > 0.0 && value < 0.5, "interp_time_share=%f", value);
}

int main(void) {
    test_exec_share();
    test_telemetry_lines_are_emitted_whole();
    test_audio_queue_line();
    test_live_history_ring();
    test_live_rows();
    test_live_history_follows_the_report();
    test_guest_symbol_labels();
    test_new_frames_are_distinct_buffers();
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
