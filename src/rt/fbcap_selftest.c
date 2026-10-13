// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// fbcap_selftest.c - selftest for the presenter-neutral frame capture service (fbcap.c).
//
// Drives the public API with synthetic frames and checks exactly what is published:
//   - a host-served capture (the offscreen and GDI presenters) publishes an exact P6 PPM:
//     header, size, R,G,B channel order and every pixel byte;
//   - a read-back capture (the sdl3vk presenter) publishes the same bytes for the same frame,
//     so a capture means the same thing whichever presenter showed the frame;
//   - a skipped present (cancel) and a refused re-arm never publish or report stale results;
//   - a failed publication or present resolves as failed (-1), never as success;
//   - the publisher creates the parent directory it needs;
//   - SR_FBSNAP_WINDOWS alone selects the FBSNAP slot with every present captured
//     (fbcap_policy.c), while an explicit SR_FBSNAP keeps its meaning.
// No game data, no GPU, no window: exit 0 = every check passed.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "fbcap.h"
#include "fbcap_policy.h" /* declares sr_fbcap_path with size_t: after <stdio.h> */

/* Scratch root for the files this test writes: the checkout's build/ by default, or
 * the BUILD_ROOT the Makefile passes as -DSR_SELFTEST_BUILD_ROOT, so a scratch run never
 * touches the checkout. */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#define PSP_W 480u
#define PSP_H 272u

static int s_failures;

#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "fbcap_selftest FAIL: %s\n", (msg)); \
            s_failures++;                                        \
        }                                                        \
    } while (0)

static char s_dir[512];
static uint32_t s_frame[PSP_W * PSP_H];               /* host frame: 0x00RRGGBB words */
static unsigned char s_frame_rgb[PSP_W * PSP_H * 3];  /* the P6 body it must publish */

/* Path of a named file inside the selftest output directory. */
static void out_path(char *buf, size_t n, const char *name) {
    snprintf(buf, n, "%s/%s", s_dir, name);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void make_dir(const char *p) {
#ifdef _WIN32
    (void)_mkdir(p);
#else
    (void)mkdir(p, 0777);
#endif
}

static void remove_dir(const char *p) {
#ifdef _WIN32
    (void)_rmdir(p);
#else
    (void)rmdir(p);
#endif
}

/* Fill the synthetic host frame, the layout gui.c's converted s_px uses, and the RGB
 * stream a correct publisher writes for it. Every channel varies on both axes. */
static void make_frame(void) {
    for (uint32_t y = 0; y < PSP_H; y++)
        for (uint32_t x = 0; x < PSP_W; x++) {
            uint32_t r = (x * 255u) / PSP_W;
            uint32_t g = (y * 255u) / PSP_H;
            uint32_t b = ((x + y) * 255u) / (PSP_W + PSP_H);
            size_t i = (size_t)y * PSP_W + x;
            s_frame[i] = (r << 16) | (g << 8) | b;
            s_frame_rgb[i * 3 + 0] = (unsigned char)r;
            s_frame_rgb[i * 3 + 1] = (unsigned char)g;
            s_frame_rgb[i * 3 + 2] = (unsigned char)b;
        }
}

/* Read a whole file and compare it with the expected P6 header and RGB body, byte for
 * byte, including that nothing trails the body. */
static int ppm_matches(const char *path, uint32_t w, uint32_t h, const unsigned char *rgb) {
    char header[64];
    int n = snprintf(header, sizeof header, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    size_t body = (size_t)w * h * 3u;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *got = (unsigned char *)malloc((size_t)n + body + 1u);
    if (!got) { fclose(f); return 0; }
    size_t len = fread(got, 1, (size_t)n + body + 1u, f);
    fclose(f);
    int ok = len == (size_t)n + body && memcmp(got, header, (size_t)n) == 0 &&
             memcmp(got + n, rgb, body) == 0;
    free(got);
    return ok;
}

/* Both files exist and hold identical bytes. */
static int files_identical(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    int ok = fa && fb;
    while (ok) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb) ok = 0;
        if (ca == EOF || cb == EOF) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return ok;
}

/* Invalid requests are refused and arm nothing. */
static void test_refused_requests(void) {
    char long_path[1100];
    memset(long_path, 'a', sizeof long_path - 1u);
    long_path[sizeof long_path - 1u] = '\0';
    CHECK(sr_capture_arm(NULL) == 0, "refuse: NULL path");
    CHECK(sr_capture_arm("") == 0, "refuse: empty path");
    CHECK(sr_capture_arm(long_path) == 0, "refuse: path longer than the capture slot");
    CHECK(!sr_capture_is_armed(), "refuse: nothing armed after refused requests");
    CHECK(sr_capture_result() == 0, "refuse: refused requests report nothing attempted");
}

/* A host-served capture (offscreen/GDI) publishes exactly the presented frame. */
static void test_host_capture(void) {
    char path[600];
    out_path(path, sizeof path, "host.ppm");
    remove(path);
    /* Pixel (479,0) is r=254 g=0 b=162: pins the R,G,B order of the published body. */
    CHECK(s_frame_rgb[479 * 3 + 0] == 254 && s_frame_rgb[479 * 3 + 1] == 0 &&
          s_frame_rgb[479 * 3 + 2] == 162, "fixture: channel-order sentinel");
    CHECK(sr_capture_arm(path) == 1, "host: arm accepted");
    CHECK(sr_capture_is_armed(), "host: armed state visible to presenters");
    CHECK(sr_capture_result() == 0, "host: nothing attempted before service");
    CHECK(strcmp(sr_capture_source_label(), "") == 0, "host: no source before service");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "host: serve publishes");
    CHECK(sr_capture_result() == 1, "host: result is published");
    CHECK(strcmp(sr_capture_source_label(), "cpu-framebuffer") == 0, "host: source label");
    CHECK(ppm_matches(path, PSP_W, PSP_H, s_frame_rgb), "host: PPM header and pixels exact");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 0,
          "host: a present with nothing armed publishes nothing");
}

/* The sdl3vk presenter reads its B8G8R8A8 framebuffer image back instead of serving the
 * host frame. For the same frame the published file must be byte-identical to the
 * host-served one, so offscreen and windowed captures can be compared directly. */
static void test_readback_matches_host_capture(void) {
    char host[600], readback[600];
    out_path(host, sizeof host, "same_host.ppm");
    out_path(readback, sizeof readback, "same_readback.ppm");
    remove(host);
    remove(readback);
    CHECK(sr_capture_arm(host) == 1, "same: host arm");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "same: host publish");
    CHECK(sr_capture_arm(readback) == 1, "same: readback arm");
    sr_capture_mark_recorded(SR_CAP_SRC_CPU, PSP_W, PSP_H);
    CHECK(sr_capture_is_recorded() && !sr_capture_is_armed(), "same: recorded, not armed");
    CHECK(sr_capture_publish_recorded(s_frame, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "same: readback publish");
    CHECK(strcmp(sr_capture_source_label(), "cpu-framebuffer") == 0, "same: readback label");
    CHECK(files_identical(host, readback), "same: host-served and read-back files identical");
}

/* A GE render target is read back as R8G8B8A8 with a padded row pitch: channels publish
 * in order and the row padding never reaches the file. */
static void test_gpu_readback(void) {
    enum { GW = 8, GH = 4, GROW = GW * 4 + 8 };
    static unsigned char buf[GROW * GH];
    static unsigned char expect[GW * GH * 3];
    char path[600];
    out_path(path, sizeof path, "gpu.ppm");
    remove(path);
    memset(buf, 0xAB, sizeof buf);
    for (unsigned y = 0; y < GH; y++)
        for (unsigned x = 0; x < GW; x++) {
            unsigned char *p = buf + y * GROW + x * 4u;
            p[0] = (unsigned char)(x * 10u);       /* R */
            p[1] = (unsigned char)(y * 20u);       /* G */
            p[2] = (unsigned char)((x + y) * 7u);  /* B */
            p[3] = 0xEEu;
            size_t i = ((size_t)y * GW + x) * 3u;
            expect[i + 0] = p[0];
            expect[i + 1] = p[1];
            expect[i + 2] = p[2];
        }
    CHECK(sr_capture_arm(path) == 1, "gpu: arm accepted");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, GW, GH);
    CHECK(sr_capture_arm(path) == 0, "gpu: a re-arm while the readback is pending is refused");
    sr_capture_cancel();
    CHECK(sr_capture_is_recorded(), "gpu: cancel never drops a recorded readback");
    CHECK(sr_capture_serve_host(buf, GW, GH, GROW, SR_CAP_ORDER_RGBX) == 0,
          "gpu: a host present never services a recorded readback");
    CHECK(sr_capture_publish_recorded(buf, GROW, SR_CAP_ORDER_RGBX) == 1, "gpu: publish");
    CHECK(sr_capture_result() == 1, "gpu: result is published");
    CHECK(strcmp(sr_capture_source_label(), "gpu-render-target") == 0, "gpu: source label");
    CHECK(!sr_capture_is_recorded(), "gpu: publish resolves the recorded state");
    CHECK(ppm_matches(path, GW, GH, expect), "gpu: RGB order, pitch and pixels exact");
}

/* A skipped present cancels the arm: nothing is published for it, a later present cannot
 * service it, and the next arm publishes only its own frame. */
static void test_skipped_present_cancels(void) {
    char skipped[600], next[600];
    out_path(skipped, sizeof skipped, "skipped.ppm");
    out_path(next, sizeof next, "after_skip.ppm");
    remove(skipped);
    remove(next);
    CHECK(sr_capture_arm(skipped) == 1, "skip: arm accepted");
    sr_capture_cancel();
    CHECK(sr_capture_result() == 0, "skip: cancel resolves as nothing attempted");
    CHECK(!sr_capture_is_armed(), "skip: cancel clears the armed state");
    CHECK(strcmp(sr_capture_source_label(), "") == 0, "skip: no source for a cancelled arm");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 0,
          "skip: a later present must not service the cancelled arm");
    CHECK(!file_exists(skipped), "skip: no file for the cancelled arm");
    CHECK(sr_capture_arm(next) == 1, "skip: a fresh arm after cancel");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "skip: fresh arm publishes");
    CHECK(file_exists(next) && !file_exists(skipped), "skip: only the fresh path is written");
}

/* A refused re-arm keeps the pending capture and clears only the reportable result. */
static void test_refused_rearm_keeps_pending(void) {
    char first[600], second[600];
    out_path(first, sizeof first, "first.ppm");
    out_path(second, sizeof second, "second.ppm");
    remove(first);
    remove(second);
    CHECK(sr_capture_arm(first) == 1, "rearm: first arm accepted");
    CHECK(sr_capture_arm(second) == 0, "rearm: second arm refused while pending");
    CHECK(sr_capture_result() == 0, "rearm: refused arm exposes no stale result");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "rearm: pending capture still serviced");
    CHECK(file_exists(first) && !file_exists(second), "rearm: the pending path is written");
}

/* A failed publication resolves as failed (-1), never as success, and does not wedge the
 * service: the next arm works. */
static void test_failed_publication(void) {
    char blocked[600], next[600];
    out_path(blocked, sizeof blocked, "blocked.ppm");
    out_path(next, sizeof next, "next.ppm");
    make_dir(blocked);                     /* a directory cannot be replaced by the file */
    remove(next);
    CHECK(sr_capture_arm(blocked) == 1, "fail: arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == -1,
          "fail: an unwritable destination is attempted-and-failed");
    CHECK(sr_capture_result() == -1, "fail: result is -1, never a fabricated success");
    CHECK(sr_capture_arm(next) == 1, "fail: the service is not wedged after a failure");
    CHECK(sr_capture_result() == 0, "fail: a new arm clears the previous failure");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "fail: the next capture publishes");
    CHECK(ppm_matches(next, PSP_W, PSP_H, s_frame_rgb), "fail: the next file is exact");
}

/* Failures before publication (a failed present, an invalid pixel description) resolve
 * as failed and publish nothing. */
static void test_explicit_failures(void) {
    char present_failed[600], bad_pitch[600];
    out_path(present_failed, sizeof present_failed, "present_failed.ppm");
    out_path(bad_pitch, sizeof bad_pitch, "bad_pitch.ppm");
    remove(present_failed);
    remove(bad_pitch);
    CHECK(sr_capture_arm(present_failed) == 1, "explicit: arm accepted");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, 8, 4);
    sr_capture_fail("synthetic present failure");
    CHECK(sr_capture_result() == -1, "explicit: fail resolves a recorded capture as -1");
    CHECK(!sr_capture_is_recorded(), "explicit: fail clears the recorded state");
    CHECK(sr_capture_publish_recorded(s_frame, PSP_W * 4u, SR_CAP_ORDER_RGBX) == 0,
          "explicit: a failed capture cannot be published later");
    CHECK(sr_capture_result() == -1 && !file_exists(present_failed),
          "explicit: the failure stands and no file exists");

    CHECK(sr_capture_arm(bad_pitch) == 1, "explicit: second arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u - 1u, SR_CAP_ORDER_BGRX) == -1,
          "explicit: a row pitch shorter than the pixels is refused");
    CHECK(sr_capture_result() == -1 && !file_exists(bad_pitch),
          "explicit: invalid pixels publish nothing");
}

/* The publisher creates the parent directories it needs (FBSNAP writes under
 * build/snapshots/, which no build step creates). */
static void test_parent_directory(void) {
    char path[600], sub[600], nested[600];
    out_path(path, sizeof path, "nested/sub/frame_v1.ppm");
    out_path(sub, sizeof sub, "nested/sub");
    out_path(nested, sizeof nested, "nested");
    remove(path);
    remove_dir(sub);
    remove_dir(nested);
    CHECK(!file_exists(path), "parent: no file left from an earlier run");
    CHECK(sr_capture_arm(path) == 1, "parent: arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "parent: publish creates nested/sub");
    CHECK(ppm_matches(path, PSP_W, PSP_H, s_frame_rgb), "parent: file exact in the new dir");
}

/* An absolute destination publishes too: on Windows its first prefix is a drive root such as
 * "C:/", which mkdir reports as an error (EACCES) even though it exists. */
static void test_absolute_path(void) {
    char cwd[512] = "", base[1100], path[1200], sub[1200], top[1200];
    int absolute = s_dir[0] == '/' || s_dir[0] == '\\' || (s_dir[0] && s_dir[1] == ':');
    if (!absolute) {
#ifdef _WIN32
        CHECK(_getcwd(cwd, (int)sizeof cwd) != NULL, "absolute: current directory");
#else
        CHECK(getcwd(cwd, sizeof cwd) != NULL, "absolute: current directory");
#endif
    }
    int n = absolute ? snprintf(base, sizeof base, "%s", s_dir)
                     : snprintf(base, sizeof base, "%s/%s", cwd, s_dir);
    CHECK(n > 0 && (size_t)n < sizeof base, "absolute: base path fits");
    n = snprintf(top, sizeof top, "%s/abs", base);
    CHECK(n > 0 && (size_t)n < sizeof top, "absolute: directory path fits");
    n = snprintf(sub, sizeof sub, "%s/abs/sub", base);
    CHECK(n > 0 && (size_t)n < sizeof sub, "absolute: subdirectory path fits");
    n = snprintf(path, sizeof path, "%s/abs/sub/frame_v2.ppm", base);
    CHECK(n > 0 && (size_t)n < sizeof path, "absolute: file path fits");
    remove(path);
    remove_dir(sub);
    remove_dir(top);
    CHECK(sr_capture_arm(path) == 1, "absolute: arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "absolute: publish creates the missing directories under an absolute root");
    CHECK(ppm_matches(path, PSP_W, PSP_H, s_frame_rgb), "absolute: file exact");
}

/* SR_FBSNAP_WINDOWS is self-sufficient: with SR_FBSNAP unset, configured windows select the
 * FBSNAP slot and capture every present inside them. An explicit SR_FBSNAP keeps its
 * meaning, and SR_FBDUMP keeps priority over windows. */
static void test_windows_imply_every_present(void) {
    CHECK(sr_fbcap_snap_every(NULL, 0) == 0, "policy: nothing configured is off");
    CHECK(sr_fbcap_snap_every("", 0) == 0, "policy: an empty switch without windows is off");
    CHECK(sr_fbcap_snap_every(NULL, 1) == 1, "policy: windows alone capture every present");
    CHECK(sr_fbcap_snap_every("", 1) == 1, "policy: an empty switch counts as unset");
    CHECK(sr_fbcap_snap_every("0", 1) == 0, "policy: an explicit SR_FBSNAP=0 stays off");
    CHECK(sr_fbcap_snap_every("-2", 1) == 0, "policy: a negative cadence is off");
    CHECK(sr_fbcap_snap_every("every", 1) == 0, "policy: a non-numeric cadence is off");
    CHECK(sr_fbcap_snap_every("3", 0) == 3, "policy: SR_FBSNAP=3 alone keeps its cadence");
    CHECK(sr_fbcap_snap_every("3", 1) == 3, "policy: windows keep an explicit cadence");
    CHECK(sr_fbcap_snap_every("99999999999", 0) == INT_MAX, "policy: a huge cadence saturates");
    CHECK(sr_fbcap_owner(0, sr_fbcap_snap_every(NULL, 1) > 0) == SR_FBCAP_FBSNAP,
          "policy: windows without SR_FBSNAP own the FBSNAP slot");
    CHECK(sr_fbcap_owner(0, sr_fbcap_snap_every("0", 1) > 0) == SR_FBCAP_NONE,
          "policy: SR_FBSNAP=0 with windows owns nothing");
    CHECK(sr_fbcap_owner(1, sr_fbcap_snap_every(NULL, 1) > 0) == SR_FBCAP_FBDUMP,
          "policy: SR_FBDUMP keeps priority over windows");
}

int main(int argc, char **argv) {
    snprintf(s_dir, sizeof s_dir, "%s", argc > 1 ? argv[1] : SR_SELFTEST_BUILD_ROOT "/fbcap_selftest");
    make_dir(s_dir);
    make_frame();
    test_refused_requests();
    test_host_capture();
    test_readback_matches_host_capture();
    test_gpu_readback();
    test_skipped_present_cancels();
    test_refused_rearm_keeps_pending();
    test_failed_publication();
    test_explicit_failures();
    test_parent_directory();
    test_absolute_path();
    test_windows_imply_every_present();
    if (s_failures) {
        fprintf(stderr, "fbcap_selftest: %d check(s) failed\n", s_failures);
        return 1;
    }
    puts("fbcap selftest: OK");
    return 0;
}
