// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// videorec_selftest.c - selftest for the run recorder (src/rt/videorec.c, #1273).
//
// Drives the recorder the way the runtime does (presented frames, serviced VBLANKs at guest
// times, sceAudioOutput hand-offs) with synthetic pictures and sound, then reads the files back:
//   - one video frame per VBLANK from the first presented picture on: new pictures, repeats when
//     the guest did not flip, and skipped VCOUNT steps written as gap frames in their place;
//   - the WAV holds exactly floor(frames * 44100 * 1001 / 60000) samples;
//   - sync: an impulse handed over at a frame's VBLANK lands on that frame's first sample, one
//     handed over 8 ms later lands 352 samples later, and a channel's next buffer follows its
//     previous one with no gap;
//   - channel volume (0x8000 unity), the sum of two channels, and clipping;
//   - a GE-order (RGBX) picture is written in the video's BGRX order;
//   - a picture of another size is refused and reported, never written;
//   - the recorder never writes the pixels or samples it is handed;
//   - with FFmpeg on PATH, the FFV1 encode and the mux finish with status PASS (SKIP otherwise);
//   - recording only on the deterministic clock; a named encoder, FFmpeg or codec that cannot
//     run refuses the recording (never raw frames in its place); free space under 4 GiB
//     refuses it and under 1 GiB stops it; a used folder keeps no earlier PASS; and an encoder
//     that fails, a mux that fails or crashes, audio far ahead and audio that never reached
//     audio.wav each FAIL the recording with its reason (a fake FFmpeg: this binary);
//   - a fatal stop (_Exit, no atexit) after sr_videorec_finish_at leaves PASS naming the stop,
//     and one without it leaves INCOMPLETE, never PASS (a child process: this binary).
// No game data, no GPU, no window: exit 0 = every check passed.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* The selftest seams of videorec.h; the Makefile defines this too. */
#ifndef SR_VIDEOREC_SELFTEST
#define SR_VIDEOREC_SELFTEST 1
#endif
#include "videorec.h"

/* Scratch files go beneath the BUILD_ROOT the Makefile passes as -DSR_SELFTEST_BUILD_ROOT, so a
 * scratch run never writes the checkout's build/ tree. */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#define W 64u
#define H 36u

static int s_failures;
#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "videorec_selftest FAIL: %s (line %d)\n", \
                    (msg), __LINE__);                                 \
            s_failures++;                                             \
        }                                                             \
    } while (0)

/* Guest time of VBLANK v on an exact 60000/1001 grid, in microseconds. */
static uint64_t vtime(uint32_t v) { return (uint64_t)v * 1001000000u / 60000u; }

static void fill(uint32_t *px, uint32_t value) {
    for (uint32_t i = 0; i < W * H; i++) px[i] = value;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int16_t *read_wav(const char *path, uint32_t *samples) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t h[44];
    if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) || memcmp(h + 36, "data", 4)) {
        fclose(f);
        return NULL;
    }
    uint32_t bytes = (uint32_t)h[40] | ((uint32_t)h[41] << 8) | ((uint32_t)h[42] << 16) |
                     ((uint32_t)h[43] << 24);
    *samples = bytes / 4u;
    int16_t *s = (int16_t *)malloc(bytes ? bytes : 4u);
    uint8_t *raw = (uint8_t *)malloc(bytes ? bytes : 4u);
    if (!s || !raw || fread(raw, 1, bytes, f) != bytes) {
        free(s);
        free(raw);
        fclose(f);
        return NULL;
    }
    for (uint32_t i = 0; i < bytes / 2u; i++)
        s[i] = (int16_t)((uint16_t)raw[i * 2u] | ((uint16_t)raw[i * 2u + 1u] << 8));
    free(raw);
    fclose(f);
    return s;
}

static void path_in(char *out, size_t n, const char *dir, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

static int record_says(const char *dir, const char *status, const char *reason_part);

static void make_dir(const char *d) {
#ifdef _WIN32
    (void)_mkdir(d);
#else
    (void)mkdir(d, 0755);
#endif
}

/* The main route: 40 VBLANKs from vcount 100, flips every other VBLANK, one skipped VCOUNT. */
static void test_timeline(const char *root) {
    char dir[600], p[700];
    snprintf(dir, sizeof dir, "%s/raw", root);
    CHECK(sr_videorec_test_start(dir, "raw"), "raw recording starts");

    static uint32_t px[W * H], copy[W * H];
    static int16_t buf[1024 * 2], buf_copy[1024 * 2];
    for (int i = 0; i < 1024; i++) { buf[i * 2] = 1000; buf[i * 2 + 1] = -1000; }
    memcpy(buf_copy, buf, sizeof buf);

    /* Sound before the first picture is not part of the recording. */
    sr_videorec_audio(0, buf, 1024, 0x8000, 0x8000, vtime(99));
    sr_videorec_vblank(99, vtime(99)); /* no picture yet: nothing starts */

    uint32_t v = 100;
    uint32_t written_value[64];
    int written = 0;
    uint32_t last_value = 0;
    for (int step = 0; step < 40; step++, v++) {
        if (step == 20) v++; /* the host skipped a VCOUNT step: a gap frame stands in */
        if (step % 2 == 0) {
            last_value = 0x00010000u * (uint32_t)step + 0x40u;
            fill(px, last_value);
            memcpy(copy, px, sizeof px);
            sr_videorec_guest_flip();
            sr_videorec_frame(px, W, H, W * 4u, SR_CAP_ORDER_BGRX);
            CHECK(memcmp(copy, px, sizeof px) == 0, "the recorder leaves the presented pixels alone");
        }
        if (step == 20) {
            uint32_t prev = written_value[written - 1];
            written_value[written++] = prev; /* the gap repeats the picture before it */
        }
        sr_videorec_vblank(v, vtime(v));
        written_value[written++] = last_value;
        if (step == 10) {
            /* A 1024-sample buffer on channel 1 at frame 10's VBLANK, an impulse at its start,
             * and a one-sample impulse 8 ms later on channel 2. */
            static int16_t imp[1024 * 2];
            imp[0] = imp[1] = 20000;
            sr_videorec_audio(1, imp, 1024, 0x8000, 0x8000, vtime(v));
            int16_t imp2[2] = { -12000, 12000 };
            sr_videorec_audio(2, imp2, 1, 0x4000, 0x8000, vtime(v) + 8000u);
            /* Channel 1's next buffer, handed over while the first still plays, follows it. */
            int16_t next[2] = { 7000, 7000 };
            sr_videorec_audio(1, next, 1, 0x8000, 0x8000, vtime(v) + 5000u);
            /* Channel 5 runs dry: its next buffer, handed over 12 ms on, starts then. */
            int16_t one[2] = { 100, 100 };
            sr_videorec_audio(5, one, 1, 0x8000, 0x8000, vtime(v) + 1000u);
            sr_videorec_audio(5, one, 1, 0x8000, 0x8000, vtime(v) + 12000u);
        }
        if (step == 30) {
            /* Two channels summing past full scale clip. */
            int16_t loud[2] = { 30000, -30000 };
            sr_videorec_audio(3, loud, 1, 0x8000, 0x8000, vtime(v));
            sr_videorec_audio(4, loud, 1, 0x8000, 0x8000, vtime(v));
        }
    }
    CHECK(memcmp(buf, buf_copy, sizeof buf) == 0, "the recorder leaves the guest samples alone");

    /* A picture of another size is refused, not written. */
    static uint32_t big[W * 2 * H];
    sr_videorec_frame(big, W * 2u, H, W * 8u, SR_CAP_ORDER_BGRX);
    SrVideoRecStats st;
    sr_videorec_test_stats(&st);
    CHECK(st.refused_frames == 1, "a picture of another size is refused");

    CHECK(sr_videorec_finish() == 0, "a recording with a refused picture reports FAIL");
    sr_videorec_test_stats(&st);
    CHECK(st.first_vcount == 100, "the recording starts at the first presented picture");
    CHECK(st.frames == 41, "40 serviced VBLANKs plus one gap give 41 frames");
    CHECK(st.new_frames == 20, "20 flips are 20 new frames");
    CHECK(st.guest_flips == 20, "the guest's 20 flips are counted");
    CHECK(st.repeat_frames == 20, "VBLANKs without a flip repeat the picture");
    CHECK(st.gap_frames == 1, "the skipped VCOUNT step is one gap frame");
    CHECK(st.late_vblanks == 0, "an exact guest-time grid has no late VBLANKs");
    CHECK(st.width == W && st.height == H, "the first picture fixes the size");

    path_in(p, sizeof p, dir, "video.bgr0");
    CHECK(file_size(p) == (long)(41u * W * H * 4u), "video.bgr0 holds exactly 41 frames");
    FILE *f = fopen(p, "rb");
    if (f) {
        static uint32_t got[W * H];
        for (int k = 0; k < 41; k++) {
            if (fread(got, 4, W * H, f) != W * H) { CHECK(0, "frame read"); break; }
            CHECK(got[0] == written_value[k] && got[W * H - 1] == written_value[k],
                  "each frame holds the picture shown at its VBLANK");
        }
        fclose(f);
    }

    uint32_t n = 0;
    path_in(p, sizeof p, dir, "audio.wav");
    int16_t *a = read_wav(p, &n);
    CHECK(a != NULL, "audio.wav reads back");
    CHECK(n == (uint32_t)sr_videorec_frame_sample(41), "audio length equals the video's");
    CHECK(n == 30165u, "41 frames are 30165 samples at 44100 Hz");
    if (a) {
        uint64_t s10 = sr_videorec_frame_sample(10);
        CHECK(a[s10 * 2] == 20000 && a[s10 * 2 + 1] == 20000,
              "an impulse at frame 10's VBLANK is frame 10's first sample");
        CHECK(a[(s10 - 1) * 2] == 0, "nothing before it");
        CHECK(a[(s10 + 1) * 2] == 0, "the impulse buffer's second sample is silent");
        CHECK(a[(s10 + 1024) * 2] == 7000, "the channel's next buffer follows on without a gap");
        CHECK(a[(s10 + 1023) * 2] == 0 && a[(s10 + 1025) * 2] == 0, "and only there");
        CHECK(a[(s10 + 44) * 2] == 100 && a[(s10 + 45) * 2] == 0, "1 ms is 44 samples");
        CHECK(a[(s10 + 529) * 2] == 100, "a channel that ran dry restarts when handed over");
        CHECK(a[(s10 + 352) * 2] == -6000 && a[(s10 + 352) * 2 + 1] == 12000,
              "8 ms after the VBLANK is 352 samples later, at the channel's volume");
        /* Step 30 is frame 31: the gap frame at step 20 keeps its place on the timeline. */
        uint64_t s31 = sr_videorec_frame_sample(31);
        CHECK(a[s31 * 2] == 32767 && a[s31 * 2 + 1] == -32768, "the sum of two channels clips");
        free(a);
    }

    path_in(p, sizeof p, dir, "record.json");
    CHECK(file_size(p) > 0, "record.json is written");
    path_in(p, sizeof p, dir, "frames.csv");
    f = fopen(p, "r");
    int rows = 0, gaps = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            rows++;
            if (strstr(line, ",gap")) gaps++;
        }
        fclose(f);
    }
    CHECK(rows == 42 && gaps == 1, "frames.csv has a header and one row per frame");
    path_in(p, sizeof p, dir, "logic.srt");
    f = fopen(p, "r");
    char srt[256] = { 0 };
    if (f) {
        size_t n = fread(srt, 1, sizeof srt - 1, f);
        srt[n] = 0;
        fclose(f);
    }
    /* 41 frames: one partial cue, frames 0..41, with the 20 flips. */
    CHECK(strstr(srt, "1\n00:00:00,000 --> 00:00:00,684\ngame 29.2 fps (20 frames)") != NULL,
          "logic.srt carries the game's frame rate for the recorded stretch");
}

/* A GE render target (RGBX) is written in BGRX order, and a clean recording reports PASS. */
static void test_order_and_pass(const char *root) {
    char dir[600], p[700];
    snprintf(dir, sizeof dir, "%s/order", root);
    CHECK(sr_videorec_test_start(dir, "raw"), "second recording starts");
    static uint8_t rgbx[W * H * 4];
    for (uint32_t i = 0; i < W * H; i++) {
        rgbx[i * 4] = 0x11; rgbx[i * 4 + 1] = 0x22; rgbx[i * 4 + 2] = 0x33; rgbx[i * 4 + 3] = 0xff;
    }
    sr_videorec_frame(rgbx, W, H, W * 4u, SR_CAP_ORDER_RGBX);
    for (uint32_t v = 7; v < 12; v++) sr_videorec_vblank(v, vtime(v));
    /* A late host: two VBLANKs serviced at nearly the same guest instant. */
    sr_videorec_vblank(12, vtime(11) + 100u);
    CHECK(sr_videorec_finish() == 1, "a clean recording reports PASS");
    SrVideoRecStats st;
    sr_videorec_test_stats(&st);
    CHECK(st.frames == 6 && st.late_vblanks == 1, "a VBLANK serviced late is counted");
    path_in(p, sizeof p, dir, "video.bgr0");
    FILE *f = fopen(p, "rb");
    uint8_t px[4] = { 0, 0, 0, 0 };
    if (f) {
        CHECK(fread(px, 1, 4, f) == 4, "first pixel reads");
        fclose(f);
    }
    CHECK(px[0] == 0x33 && px[1] == 0x22 && px[2] == 0x11, "RGBX is written as BGRX");
    uint32_t n = 0;
    path_in(p, sizeof p, dir, "audio.wav");
    int16_t *a = read_wav(p, &n);
    CHECK(a && n == (uint32_t)sr_videorec_frame_sample(6), "a silent run still has full-length audio");
    free(a);
}

/* SR_VIDEOREC_START: nothing before the start VBLANK, and the picture presented in the period
 * before it is the first frame. */
static void test_start(const char *root) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/start", root);
    CHECK(sr_videorec_test_start_at(dir, "raw", 50u), "a recording with a start VBLANK starts");
    static uint32_t px[W * H];
    int16_t s[2] = { 1, 1 };
    for (uint32_t v = 40; v < 60; v++) {
        CHECK(sr_videorec_wants_frames() == (v >= 50u),
              "pictures are wanted once the VBLANK before the start was serviced");
        fill(px, v);
        sr_videorec_frame(px, W, H, W * 4u, SR_CAP_ORDER_BGRX);
        sr_videorec_audio(0, s, 1, 0x8000, 0x8000, vtime(v) + 10u);
        sr_videorec_vblank(v, vtime(v));
    }
    CHECK(sr_videorec_finish() == 1, "a started recording reports PASS");
    SrVideoRecStats st;
    sr_videorec_test_stats(&st);
    CHECK(st.first_vcount == 50 && st.frames == 10, "the recording holds VBLANKs 50..59");
    char p[700];
    path_in(p, sizeof p, dir, "video.bgr0");
    FILE *f = fopen(p, "rb");
    uint32_t first = 0;
    if (f) {
        CHECK(fread(&first, 4, 1, f) == 1, "first pixel reads");
        fclose(f);
    }
    CHECK(first == 50u, "the first frame is the picture presented just before VBLANK 50");
}

/* With FFmpeg on PATH: the FFV1 encode and the mux. */
static void test_ffmpeg(const char *root) {
    char dir[600], p[700];
    snprintf(dir, sizeof dir, "%s/ffv1", root);
    if (!sr_videorec_test_start(dir, "ffv1")) {
        CHECK(record_says(dir, "REFUSED", "is not in any absolute PATH entry"),
              "without FFmpeg an encoded recording is refused, not written raw");
        printf("videorec_selftest: SKIP ffv1 (FFmpeg is not on PATH; the recording was refused)\n");
        return;
    }
    static uint32_t px[W * H];
    int16_t tone[735 * 2];
    for (int i = 0; i < 735; i++) tone[i * 2] = tone[i * 2 + 1] = (int16_t)((i % 50) * 400 - 10000);
    for (uint32_t v = 1; v <= 30; v++) {
        fill(px, 0x00102030u + v);
        sr_videorec_frame(px, W, H, W * 4u, SR_CAP_ORDER_BGRX);
        sr_videorec_vblank(v, vtime(v));
        sr_videorec_audio(0, tone, 735, 0x8000, 0x8000, vtime(v));
    }
    int ok = sr_videorec_finish();
    CHECK(ok == 1, "the FFV1 recording reports PASS");
    path_in(p, sizeof p, dir, "recording.mkv");
    CHECK(file_size(p) > 0, "recording.mkv is muxed");
    printf("videorec_selftest: ffv1 encode and mux PASS\n");
}

/* ---- failure paths (#2080 review) ------------------------------------------------------------
 *
 * This binary is also the fake FFmpeg the failure tests start through SR_VIDEOREC_FFMPEG: run
 * with "-hide_banner" first (as the recorder starts FFmpeg), it acts as the encoder (input "-")
 * or the muxer (input files) as NK_VIDEOREC_FAKE_FFMPEG says, for example "enc=ok;mux=crash":
 *   enc=ok       read every frame and store the raw bytes in the output file
 *   enc=slow:MS  the same, sleeping MS milliseconds after each frame (a stalled encoder)
 *   enc=fail     exit 3 without reading     enc=dead  exit 0 without reading
 *   mux=ok       copy the first input to the output     mux=fail  exit 1
 *   mux=crash    die as a crash does: on Windows exit code 0xC0000005 (negative as an int)
 * display_smoke/generate.py starts the recorder with enc=slow to prove that a stalled encoder
 * leaves the guest timeline as it was. */


static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static const char *fake_mode(const char *role, char *out, size_t n) {
    const char *spec = getenv("NK_VIDEOREC_FAKE_FFMPEG");
    out[0] = '\0';
    if (!spec) return out;
    size_t rl = strlen(role);
    for (const char *p = spec; *p;) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > rl && strncmp(p, role, rl) == 0 && p[rl] == '=') {
            size_t m = len - rl - 1u < n - 1u ? len - rl - 1u : n - 1u;
            memcpy(out, p + rl + 1u, m);
            out[m] = '\0';
            return out;
        }
        if (!end) break;
        p = end + 1;
    }
    return out;
}

static const char *strip_file_url(const char *p) { return strncmp(p, "file:", 5) == 0 ? p + 5 : p; }

static int fake_ffmpeg(int argc, char **argv) {
    const char *out = strip_file_url(argv[argc - 1]);
    const char *first_input = NULL;
    unsigned w = 0, h = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && !first_input) first_input = argv[i + 1];
        if (strcmp(argv[i], "-video_size") == 0) sscanf(argv[i + 1], "%ux%u", &w, &h);
    }
    char mode[64];
    int encoder = first_input && strcmp(first_input, "-") == 0;
    fake_mode(encoder ? "enc" : "mux", mode, sizeof mode);
    if (strcmp(mode, "fail") == 0) return encoder ? 3 : 1;
    if (strcmp(mode, "dead") == 0) return 0;
    if (strcmp(mode, "crash") == 0) {
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 0xC0000005u);
#endif
        abort();
    }
    unsigned slow_ms = strncmp(mode, "slow:", 5) == 0 ? (unsigned)strtoul(mode + 5, NULL, 10) : 0u;
    if (strcmp(mode, "ok") != 0 && !slow_ms) {
        fprintf(stderr, "fake ffmpeg: no mode for %s in NK_VIDEOREC_FAKE_FFMPEG\n",
                encoder ? "enc" : "mux");
        return 2;
    }
    FILE *in = encoder ? stdin : fopen(strip_file_url(first_input), "rb");
    FILE *o = fopen(out, "wb");
    if (!in || !o) return 4;
#ifdef _WIN32
    if (encoder) (void)_setmode(_fileno(stdin), _O_BINARY);
#endif
    size_t frame = encoder && w && h ? (size_t)w * h * 4u : (size_t)1u << 16;
    uint8_t *buf = (uint8_t *)malloc(frame);
    if (!buf) return 4;
    for (;;) {
        size_t got = fread(buf, 1, frame, in);
        if (got && fwrite(buf, 1, got, o) != got) return 5;
        if (got < frame) break;
        if (slow_ms) sleep_ms(slow_ms);
    }
    free(buf);
    if (!encoder) fclose(in);
    return fclose(o) == 0 ? 0 : 5;
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    (void)_putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static int self_path(char *out, size_t n) {
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)n);
    return len > 0 && len < n;
#else
    ssize_t len = readlink("/proc/self/exe", out, n - 1u);
    if (len <= 0) return 0;
    out[len] = '\0';
    return 1;
#endif
}

/* record.json's "status" and "reason" ("" when the file or the key is missing). */
static void read_record(const char *dir, char *status, size_t sn, char *reason, size_t rn) {
    char p[700], text[8192];
    status[0] = reason[0] = '\0';
    path_in(p, sizeof p, dir, "record.json");
    FILE *f = fopen(p, "rb");
    if (!f) return;
    size_t got = fread(text, 1, sizeof text - 1u, f);
    fclose(f);
    text[got] = '\0';
    const char *keys[2] = { "\"status\": \"", "\"reason\": \"" };
    char *outs[2] = { status, reason };
    size_t ns[2] = { sn, rn };
    for (int k = 0; k < 2; k++) {
        const char *at = strstr(text, keys[k]);
        if (!at) continue;
        at += strlen(keys[k]);
        size_t i = 0;
        while (at[i] && at[i] != '"' && i + 1u < ns[k]) { outs[k][i] = at[i]; i++; }
        outs[k][i] = '\0';
    }
}

static int record_says(const char *dir, const char *status, const char *reason_part) {
    char st[32], why[1200];
    read_record(dir, st, sizeof st, why, sizeof why);
    int ok = strcmp(st, status) == 0 && (!reason_part || strstr(why, reason_part) != NULL);
    if (!ok)
        fprintf(stderr, "videorec_selftest: %s/record.json says %s \"%s\"; expected %s \"%s\"\n",
                dir, st, why, status, reason_part ? reason_part : "");
    return ok;
}

/* Ten frames and their audio, then the finish. */
static int record_ten(void) {
    static uint32_t px[W * H];
    int16_t tone[735 * 2];
    for (int i = 0; i < 735; i++) tone[i * 2] = tone[i * 2 + 1] = (int16_t)(i * 7);
    for (uint32_t v = 1; v <= 10; v++) {
        fill(px, 0x00203040u + v);
        sr_videorec_frame(px, W, H, W * 4u, SR_CAP_ORDER_BGRX);
        sr_videorec_vblank(v, vtime(v));
        sr_videorec_audio(0, tone, 735, 0x8000, 0x8000, vtime(v));
    }
    return sr_videorec_finish();
}

/* Recording only on the deterministic clock; a named encoder or FFmpeg that cannot run, or a
 * full disk, refuses the recording with the reason and never falls back to raw frames. */
static void test_refusals(const char *root) {
    char dir[600], p[700];
    snprintf(dir, sizeof dir, "%s/refuse", root);
    set_env("SR_VIDEOREC_DIR", dir);
    set_env("SR_VIDEOREC_CODEC", "raw");
    set_env("SR_VIDEOREC_FFMPEG", NULL);
    set_env("SR_DETERMINISTIC_CLOCK", NULL);
    CHECK(!sr_videorec_test_start_from_env(), "no recording without SR_DETERMINISTIC_CLOCK=1");
    CHECK(record_says(dir, "REFUSED", "SR_DETERMINISTIC_CLOCK=1"),
          "the refusal names the deterministic clock");
    set_env("SR_DETERMINISTIC_CLOCK", "0");
    CHECK(!sr_videorec_test_start_from_env(), "SR_DETERMINISTIC_CLOCK=0 refuses too");
    set_env("SR_DETERMINISTIC_CLOCK", "1");
    CHECK(sr_videorec_test_start_from_env(), "the deterministic clock records");
    CHECK(record_says(dir, "INCOMPLETE", NULL), "a started recording is INCOMPLETE until finished");
    CHECK(record_ten() == 1 && record_says(dir, "PASS", NULL), "and finishes PASS");
    {
        char p[700], text[4096] = { 0 };
        path_in(p, sizeof p, dir, "record.json");
        FILE *f = fopen(p, "rb");
        if (f) { size_t got = fread(text, 1, sizeof text - 1u, f); text[got] = '\0'; fclose(f); }
        CHECK(strstr(text, "\"muxed\": null") != NULL,
              "a raw recording has no mux step, so a PASS never says muxed: false");
    }

    set_env("SR_VIDEOREC_CODEC", "ffv1");
    set_env("SR_VIDEOREC_FFMPEG", "ffmpeg");
    CHECK(!sr_videorec_test_start_from_env(), "a relative SR_VIDEOREC_FFMPEG refuses");
    CHECK(record_says(dir, "REFUSED", "is not an absolute path"), "and says so");
    path_in(p, sizeof p, dir, "video.bgr0");
    CHECK(file_size(p) < 0, "no raw frames are written in its place");
#ifdef _WIN32
    set_env("SR_VIDEOREC_FFMPEG", "C:/videorec-no-such-dir/ffmpeg.exe");
#else
    set_env("SR_VIDEOREC_FFMPEG", "/videorec-no-such-dir/ffmpeg");
#endif
    CHECK(!sr_videorec_test_start_from_env(), "a missing SR_VIDEOREC_FFMPEG refuses");
    CHECK(record_says(dir, "REFUSED", "is not an executable file"), "and says so");
    set_env("SR_VIDEOREC_FFMPEG", NULL);
    const char *saved = getenv("PATH");
    char *path_copy = saved ? strdup(saved) : NULL;
#ifdef _WIN32
    set_env("PATH", "C:/videorec-no-such-dir");
#else
    set_env("PATH", "/videorec-no-such-dir");
#endif
    CHECK(!sr_videorec_test_start_from_env(), "no FFmpeg on PATH refuses an encoded recording");
    CHECK(record_says(dir, "REFUSED", "is not in any absolute PATH entry"), "and says so");
    set_env("PATH", path_copy);
    free(path_copy);
    set_env("SR_VIDEOREC_CODEC", "h265");
    CHECK(!sr_videorec_test_start_from_env(), "an unknown codec refuses");
    CHECK(record_says(dir, "REFUSED", "is not ffv1, x264rgb, nvenc or raw"), "and says so");
    sr_videorec_test_reset();
    set_env("SR_VIDEOREC_DIR", NULL);
    set_env("SR_VIDEOREC_CODEC", NULL);
    set_env("SR_DETERMINISTIC_CLOCK", NULL);
}

/* Free space: under 4 GiB refuses the start, under 1 GiB during the run stops it as FAIL. */
static void test_disk_space(const char *root) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/disk", root);
    /* The environment route reads the free space at its first call, after the injected value. */
    set_env("SR_VIDEOREC_DIR", dir);
    set_env("SR_VIDEOREC_CODEC", "raw");
    set_env("SR_DETERMINISTIC_CLOCK", "1");
    sr_videorec_test_reset();
    sr_videorec_test_set_free_bytes(((uint64_t)4u << 30) - 1u);
    CHECK(!sr_videorec_active(), "under 4 GiB free refuses the recording");
    CHECK(record_says(dir, "REFUSED", "a recording needs at least 4096 MiB"), "and says so");
    set_env("SR_VIDEOREC_DIR", NULL);
    set_env("SR_VIDEOREC_CODEC", NULL);
    set_env("SR_DETERMINISTIC_CLOCK", NULL);

    CHECK(sr_videorec_test_start(dir, "raw"), "a recording starts with space");
    static uint32_t px[W * H];
    for (uint32_t v = 1; v <= 130; v++) {
        if (v == 70) sr_videorec_test_set_free_bytes(((uint64_t)1u << 30) - 1u);
        fill(px, v);
        sr_videorec_frame(px, W, H, W * 4u, SR_CAP_ORDER_BGRX);
        sr_videorec_vblank(v, vtime(v));
    }
    CHECK(sr_videorec_finish() == 0, "the disk filling up fails the recording");
    CHECK(record_says(dir, "FAIL", "under 1 GiB free"), "and says why");
    SrVideoRecStats st;
    sr_videorec_test_stats(&st);
    CHECK(st.frames == 120, "no frame is written after the check that found the disk full");
}

/* A folder an earlier run left: its PASS and its recording are gone before this run writes. */
static void test_stale_folder(const char *root) {
    char dir[600], p[700];
    snprintf(dir, sizeof dir, "%s/stale", root);
    make_dir(dir);
    path_in(p, sizeof p, dir, "record.json");
    FILE *f = fopen(p, "w");
    if (f) { fputs("{\"status\": \"PASS\"}\n", f); fclose(f); }
    path_in(p, sizeof p, dir, "recording.mkv");
    f = fopen(p, "wb");
    if (f) { fputs("old", f); fclose(f); }
    CHECK(sr_videorec_test_start(dir, "raw"), "a recording starts in a used folder");
    CHECK(file_size(p) < 0, "the earlier run's recording.mkv is removed");
    CHECK(record_says(dir, "INCOMPLETE", "ended before the recording was finished"),
          "record.json says INCOMPLETE until the finish");
    sr_videorec_test_reset();
    CHECK(record_says(dir, "INCOMPLETE", NULL), "a run that never finishes stays INCOMPLETE");
}

/* Encoder and mux failures through a fake FFmpeg: every one is FAIL, never PASS. */
static void test_encoder_failures(const char *root, const char *self) {
    static const struct {
        const char *name, *spec, *reason;
        int pass;
    } cases[] = {
        { "enc-fail", "enc=fail;mux=ok", "encoder", 0 },
        { "enc-dead", "enc=dead;mux=ok", NULL, 0 },
        { "mux-fail", "enc=ok;mux=fail", "muxing audio and video failed", 0 },
        { "mux-crash", "enc=ok;mux=crash", "muxing audio and video failed", 0 },
        { "both-ok", "enc=ok;mux=ok", NULL, 1 },
    };
    set_env("SR_VIDEOREC_FFMPEG", self);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char dir[600], p[700];
        snprintf(dir, sizeof dir, "%s/%s", root, cases[i].name);
        set_env("NK_VIDEOREC_FAKE_FFMPEG", cases[i].spec);
        CHECK(sr_videorec_test_start(dir, "ffv1"), cases[i].name);
        int ok = record_ten();
        CHECK(ok == cases[i].pass, cases[i].name);
        CHECK(record_says(dir, cases[i].pass ? "PASS" : "FAIL", cases[i].reason), cases[i].name);
        path_in(p, sizeof p, dir, "record.json");
        FILE *f = fopen(p, "rb");
        char text[4096] = { 0 };
        if (f) { size_t got = fread(text, 1, sizeof text - 1u, f); text[got] = '\0'; fclose(f); }
        int muxed = strstr(text, "\"muxed\": true") != NULL;
        CHECK(muxed == cases[i].pass, "PASS exactly when the recording was muxed");
        if (cases[i].pass) {
            path_in(p, sizeof p, dir, "recording.mkv");
            CHECK(file_size(p) == (long)(10u * W * H * 4u), "the fake mux holds every frame");
        }
    }
    set_env("SR_VIDEOREC_FFMPEG", NULL);
    set_env("NK_VIDEOREC_FAKE_FFMPEG", NULL);
}

/* Audio that runs more than the ring ahead, and samples that never reach audio.wav, FAIL. */
static void test_audio_failures(const char *root) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/audio-overflow", root);
    CHECK(sr_videorec_test_start(dir, "raw"), "overflow recording starts");
    static uint32_t px[W * H];
    static int16_t block[4096 * 2];
    fill(px, 1);
    sr_videorec_frame(px, W, H, W * 4u, SR_CAP_ORDER_BGRX);
    sr_videorec_vblank(1, vtime(1));
    for (int i = 0; i < 300; i++) sr_videorec_audio(0, block, 4096, 0x8000, 0x8000, vtime(1));
    sr_videorec_vblank(2, vtime(2));
    CHECK(sr_videorec_finish() == 0, "audio far ahead of the picture fails");
    CHECK(record_says(dir, "FAIL", "audio ran more than 23 s ahead"), "and says why");

    snprintf(dir, sizeof dir, "%s/audio-lost", root);
    CHECK(sr_videorec_test_start(dir, "raw"), "lost-audio recording starts");
    sr_videorec_test_fail_audio_writes(1);
    CHECK(record_ten() == 0, "audio that never reached audio.wav fails");
    CHECK(record_says(dir, "FAIL", "audio.wav is shorter than the video"), "and says why");
}

/* A fatal stop ends the process with _Exit, which runs no atexit handler: hle.c's unregistered
 * NID, the watchdog, the frame-dump exit and the other stops in src/rt/hle*.c. Each calls
 * sr_videorec_finish_at first (tools/test_dispatch_fatal_policy.py requires that of every such
 * stop). A child records five VBLANKs and stops with _Exit(7): with the finish, record.json is PASS
 * and names the stop; without it, the recording stays INCOMPLETE, never PASS. */
#define FATAL_STOP_WHY "a fatal stop ended the run"

static int exit_child(const char *dir, int finish) {
    if (!sr_videorec_test_start(dir, "raw")) return 3;
    static uint32_t px[W * H];
    int16_t tone[735 * 2];
    for (int i = 0; i < 735; i++) tone[i * 2] = tone[i * 2 + 1] = (int16_t)(i * 5);
    for (uint32_t v = 1; v <= 5; v++) {
        fill(px, 0x00102030u + v);
        sr_videorec_frame(px, W, H, W * 4u, SR_CAP_ORDER_BGRX);
        sr_videorec_vblank(v, vtime(v));
        sr_videorec_audio(0, tone, 735, 0x8000, 0x8000, vtime(v));
    }
    if (finish) (void)sr_videorec_finish_at(FATAL_STOP_WHY);
    fflush(stdout);
    fflush(stderr);
    _Exit(7);
}

/* The child's exit code, or -1 when it did not exit normally. */
static int run_exit_child(const char *self, const char *dir, const char *mode) {
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    return (int)_spawnl(_P_WAIT, self, self, "--exit-child", dir, mode, NULL);
#else
    (void)self;
    pid_t pid = fork();
    if (pid == 0) _exit(exit_child(dir, strcmp(mode, "finish") == 0));
    int status = 0;
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static void test_fatal_stop(const char *root, const char *self) {
    char dir[600], p[700], text[8192] = { 0 };
    sr_videorec_test_reset();
    snprintf(dir, sizeof dir, "%s/fatal-finish", root);
    CHECK(run_exit_child(self, dir, "finish") == 7, "the finishing child stops with _Exit(7)");
    CHECK(record_says(dir, "PASS", NULL), "a stop that finishes first leaves PASS");
    path_in(p, sizeof p, dir, "record.json");
    FILE *f = fopen(p, "rb");
    if (f) { size_t got = fread(text, 1, sizeof text - 1u, f); text[got] = '\0'; fclose(f); }
    CHECK(strstr(text, "\"run_end\": \"" FATAL_STOP_WHY "\"") != NULL, "and names the stop");
    CHECK(strstr(text, "\"frames\": 5") != NULL, "with the five frames recorded before it");
    snprintf(dir, sizeof dir, "%s/fatal-skip", root);
    CHECK(run_exit_child(self, dir, "skip") == 7, "the other child stops with _Exit(7)");
    CHECK(record_says(dir, "INCOMPLETE", NULL), "a stop that skips the finish never leaves PASS");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-hide_banner") == 0) return fake_ffmpeg(argc, argv);
    if (argc == 4 && strcmp(argv[1], "--exit-child") == 0)
        return exit_child(argv[2], strcmp(argv[3], "finish") == 0);
    const char *root = argc > 1 ? argv[1] : SR_SELFTEST_BUILD_ROOT "/videorec_selftest";
    make_dir(root);
    test_timeline(root);
    test_order_and_pass(root);
    test_start(root);
    test_ffmpeg(root);
    test_refusals(root);
    test_disk_space(root);
    test_stale_folder(root);
    test_audio_failures(root);
    char self[1024];
    CHECK(self_path(self, sizeof self), "the selftest finds its own path for the fake FFmpeg");
    test_encoder_failures(root, self);
    test_fatal_stop(root, self);
    sr_videorec_test_reset();
    if (s_failures) {
        fprintf(stderr, "videorec_selftest: %d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("videorec_selftest: PASS\n");
    return 0;
}
