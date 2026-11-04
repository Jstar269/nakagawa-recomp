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
//   - with FFmpeg on PATH, the FFV1 encode and the mux finish with status PASS (SKIP otherwise).
// No game data, no GPU, no window: exit 0 = every check passed.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "videorec.h"

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
    CHECK(sr_videorec_test_start(dir, "ffv1"), "ffv1 recording starts");
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
    path_in(p, sizeof p, dir, "video.bgr0");
    if (file_size(p) >= 0) {
        printf("videorec_selftest: SKIP ffv1 (FFmpeg not on PATH; raw fallback used)\n");
        CHECK(ok == 1, "the raw fallback is still a complete recording");
        return;
    }
    CHECK(ok == 1, "the FFV1 recording reports PASS");
    path_in(p, sizeof p, dir, "recording.mkv");
    CHECK(file_size(p) > 0, "recording.mkv is muxed");
    printf("videorec_selftest: ffv1 encode and mux PASS\n");
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "build/videorec_selftest";
    make_dir(root);
    test_timeline(root);
    test_order_and_pass(root);
    test_start(root);
    test_ffmpeg(root);
    sr_videorec_test_reset();
    if (s_failures) {
        fprintf(stderr, "videorec_selftest: %d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("videorec_selftest: PASS\n");
    return 0;
}
