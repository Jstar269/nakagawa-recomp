// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// fbcap.c - presenter-neutral frame capture service (issue #57)
//
// The single owner of capture state and of the P6 PPM publisher. Presenters only supply
// the pixels they presented (see fbcap.h for the arm/service/resolve protocol); they never
// hold capture state of their own. Host-only C: no SDL, Vulkan or guest-memory dependency,
// so fbcap-selftest drives it directly with synthetic frames.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "fbcap.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

enum {
    CAP_IDLE = 0,  /* nothing armed */
    CAP_ARMED,     /* armed; the presenter will service the next presented frame */
    CAP_RECORDED,  /* GPU readback recorded in a submitted present; waiting on its fence */
    CAP_DONE,      /* file published */
    CAP_FAILED     /* attempted and failed */
};

static int s_state = CAP_IDLE;
static int s_result;                 /* 1 published, 0 nothing attempted, -1 failed */
static char s_path[1024];
static sr_cap_source s_src = SR_CAP_SRC_NONE;
static uint32_t s_w, s_h;

/* Create the parent directory of `path` (mkdir -p on the directory component only). FBSNAP
 * publishes under build/snapshots/, which no build step creates. An existing directory is
 * success; an existing non-directory leaves the later file open to fail cleanly. */
static int cap_ensure_parent_dir(const char *path) {
    char dir[1024];
    size_t n = strlen(path);
    if (!n || n >= sizeof dir) return 0;
    memcpy(dir, path, n + 1);
    while (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[--n] = '\0';
    char *sep = NULL;
    for (char *q = dir; *q; q++)
        if (*q == '/' || *q == '\\') sep = q;
    if (!sep) return 1;               /* bare file name: current directory exists */
    *sep = '\0';
    if (sep == dir) return 1;         /* "/file" or "\\file": the root exists */
    for (char *q = dir; *q; q++) {
        if (*q != '/' && *q != '\\') continue;
        char save = q[1];
        q[1] = '\0';
#ifdef _WIN32
        if (_mkdir(dir) != 0 && errno != EEXIST) { q[1] = save; return 0; }
#else
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) { q[1] = save; return 0; }
#endif
        q[1] = save;
    }
#ifdef _WIN32
    if (_mkdir(dir) != 0 && errno != EEXIST) return 0;
#else
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) return 0;
#endif
    return 1;
}

/* The single terminal transition out of the armed/recorded states. Every failure funnels
 * through here, so sr_capture_result() can never report a stale or invented outcome. */
static void cap_finish(int ok, const char *why) {
    if (s_state != CAP_ARMED && s_state != CAP_RECORDED) return;
    if (ok) {
        s_state = CAP_DONE;
        s_result = 1;
    } else {
        s_state = CAP_FAILED;
        s_result = -1;
        fprintf(stderr, "fbcap: present capture failed: %s\n", why ? why : "unknown reason");
    }
}

/* Publish w x h pixels as a P6 PPM whose name ends in .ppm: the header matches the
 * extension and exactly w*h*3 colour bytes follow, in R,G,B order whatever the source byte
 * order. Publication is atomic: a unique temp sibling is written completely and then renamed
 * over the destination, so a reader never observes a half-written file. */
static int cap_write_file(const uint8_t *px, uint32_t row_bytes, sr_cap_order order) {
    if (!cap_ensure_parent_dir(s_path)) return 0;
    static unsigned long s_tmp_seq;
    char tmp[1024 + 64];
    if (snprintf(tmp, sizeof tmp, "%s.tmp%lu", s_path, ++s_tmp_seq) >= (int)sizeof tmp)
        return 0;
    unsigned char *rgb = (unsigned char *)malloc((size_t)s_w * 3u);
    if (!rgb) return 0;
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(rgb); return 0; }
    int ok = fprintf(f, "P6\n%u %u\n255\n", s_w, s_h) > 0;
    /* Byte offsets of R, G, B inside one pixel for the source byte order. */
    const int ir = order == SR_CAP_ORDER_RGBX ? 0 : 2;
    const int ib = order == SR_CAP_ORDER_RGBX ? 2 : 0;
    for (uint32_t y = 0; ok && y < s_h; y++) {
        const uint8_t *row = px + (size_t)y * row_bytes;
        for (uint32_t x = 0; x < s_w; x++) {
            const uint8_t *p = row + (size_t)x * 4u;
            rgb[x * 3u + 0u] = p[ir];
            rgb[x * 3u + 1u] = p[1];
            rgb[x * 3u + 2u] = p[ib];
        }
        ok = fwrite(rgb, 1, (size_t)s_w * 3u, f) == (size_t)s_w * 3u;
    }
    if (ok && fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    free(rgb);
    if (!ok) { remove(tmp); return 0; }
#ifdef _WIN32
    if (!MoveFileExA(tmp, s_path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp);
        fprintf(stderr, "fbcap: publish failed (MoveFileExA: %lu)\n", (unsigned long)GetLastError());
        return 0;
    }
#else
    if (rename(tmp, s_path) != 0) { remove(tmp); return 0; }
#endif
    return 1;
}

/* Validate the pixel description, publish the capture, then resolve it. The frame size is
 * checked in 64-bit arithmetic and bounded like the readback buffer (at most UINT32_MAX
 * source bytes), so no size computation below can wrap. */
static int cap_publish(const void *px, uint32_t row_bytes, sr_cap_order order) {
    if (!px || s_w == 0u || s_h == 0u || (uint64_t)row_bytes < (uint64_t)s_w * 4u ||
        (uint64_t)row_bytes * s_h > UINT32_MAX ||
        (order != SR_CAP_ORDER_BGRX && order != SR_CAP_ORDER_RGBX)) {
        cap_finish(0, "invalid pixel description");
        return -1;
    }
    if (!cap_write_file((const uint8_t *)px, row_bytes, order)) {
        cap_finish(0, "write or publish failed");
        return -1;
    }
    cap_finish(1, NULL);
    return 1;
}

int sr_capture_arm(const char *path) {
    /* A rejected request is a new capture attempt, not permission to reuse the last
     * completed result. In-flight state is preserved; only the reportable outcome clears. */
    s_result = 0;
    if (s_state == CAP_ARMED || s_state == CAP_RECORDED) return 0;
    if (!path || !path[0] || strlen(path) >= sizeof s_path) return 0;
    strcpy(s_path, path);
    s_src = SR_CAP_SRC_NONE;
    s_w = s_h = 0;
    s_state = CAP_ARMED;
    return 1;
}

int sr_capture_result(void) { return s_result; }

void sr_capture_cancel(void) {
    if (s_state != CAP_ARMED) return;
    s_state = CAP_IDLE;
    s_path[0] = '\0';
    s_src = SR_CAP_SRC_NONE;
    s_w = s_h = 0;
    s_result = 0;
}

void sr_capture_fail(const char *why) {
    if (s_state != CAP_ARMED && s_state != CAP_RECORDED) return;
    cap_finish(0, why);
}

const char *sr_capture_source_label(void) {
    switch (s_src) {
    case SR_CAP_SRC_CPU: return "cpu-framebuffer";
    case SR_CAP_SRC_GPU: return "gpu-render-target";
    default:             return "";
    }
}

int sr_capture_is_armed(void) { return s_state == CAP_ARMED; }

int sr_capture_is_recorded(void) { return s_state == CAP_RECORDED; }

int sr_capture_serve_host(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                          sr_cap_order order) {
    if (s_state != CAP_ARMED) return 0;
    s_src = SR_CAP_SRC_CPU;
    s_w = w;
    s_h = h;
    return cap_publish(px, row_bytes, order);
}

void sr_capture_mark_recorded(sr_cap_source src, uint32_t w, uint32_t h) {
    if (s_state != CAP_ARMED) return;
    s_src = src;
    s_w = w;
    s_h = h;
    s_state = CAP_RECORDED;
}

int sr_capture_publish_recorded(const void *px, uint32_t row_bytes, sr_cap_order order) {
    if (s_state != CAP_RECORDED) return 0;
    return cap_publish(px, row_bytes, order);
}
