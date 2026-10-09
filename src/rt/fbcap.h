// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// fbcap.h - presenter-neutral frame capture service (issue #57)
//
// One module owns all capture state and the P6 PPM publisher; every presenter publishes
// through it, so a capture means the same thing whichever presenter showed the frame.
//
//   1. The HLE present path arms a capture with sr_capture_arm() BEFORE the present call.
//   2. The presenter that actually presents the frame services the armed capture from the
//      frame it presented:
//        - a host-memory presenter (offscreen sink, GDI window) calls sr_capture_serve_host()
//          with the converted frame after the present succeeded;
//        - a presenter that reads its source back (sdl3vk) records the readback inside the
//          presenting submit, calls sr_capture_mark_recorded() once that submit is queued,
//          and sr_capture_publish_recorded() once the readback is complete and the frame is
//          known to have reached the presentation engine.
//      A presenter whose present fails calls sr_capture_fail().
//   3. Once its present call returns, the arming side calls sr_capture_cancel(): an arm no
//      presenter serviced (output slot skipped, no presenter, refused span) resolves as
//      "nothing attempted" and can never be serviced later by a newer frame.
//
// Published files are P6 PPMs written atomically. A publication failure resolves the capture
// as failed (-1); it never changes whether the frame itself was presented, and no path
// invents a success.

#ifndef NAKAGAWA_RECOMP_FBCAP_H
#define NAKAGAWA_RECOMP_FBCAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte order of one 32-bit pixel as it sits in memory. Both orders carry three colour bytes
 * and one ignored byte. The host frame (0x00RRGGBB words on a little-endian host) and the
 * B8G8R8A8 framebuffer image are SR_CAP_ORDER_BGRX; GE render targets are SR_CAP_ORDER_RGBX. */
typedef enum {
    SR_CAP_ORDER_BGRX = 0,
    SR_CAP_ORDER_RGBX
} sr_cap_order;

/* Presentation source of a serviced capture, reported by sr_capture_source_label(). */
typedef enum {
    SR_CAP_SRC_NONE = 0,
    SR_CAP_SRC_CPU, /* "cpu-framebuffer": the converted guest framebuffer that was presented */
    SR_CAP_SRC_GPU  /* "gpu-render-target": a GE render target presented by the GPU path */
} sr_cap_source;

/* Arm a capture of the next presented frame to the P6 PPM at path. Returns 1 when armed,
 * 0 when refused (path invalid or too long, or a capture is already pending). Any request,
 * accepted or refused, clears the reportable result, so a stale outcome is never mistaken
 * for this request's. */
int sr_capture_arm(const char *path);

/* Outcome of the most recent request: 1 published, 0 nothing attempted (not armed, refused,
 * cancelled, or not yet serviced), -1 attempted and failed. */
int sr_capture_result(void);

/* Resolve an armed-but-unserviced capture as "nothing attempted". No-op otherwise. */
void sr_capture_cancel(void);

/* Resolve an armed or recorded capture as attempted-and-failed. No-op otherwise. */
void sr_capture_fail(const char *why);

/* "cpu-framebuffer" or "gpu-render-target" for the capture serviced since the last arm, or
 * "" when none has been serviced. */
const char *sr_capture_source_label(void);

/* 1 while a capture is armed and waiting for a presenter to service it. */
int sr_capture_is_armed(void);

/* 1 while a recorded readback is waiting to be published. */
int sr_capture_is_recorded(void);

/* Service an armed capture from host pixels that were just presented (CPU source): h rows of
 * w pixels, each row row_bytes long (row_bytes >= 4*w). Returns 1 published, -1 failed, and
 * 0 when no capture was armed (nothing to do). */
int sr_capture_serve_host(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                          sr_cap_order order);

/* Move an armed capture to the recorded state: a w x h readback of the presentation source
 * src is queued in a submitted present. No-op unless a capture is armed. */
void sr_capture_mark_recorded(sr_cap_source src, uint32_t w, uint32_t h);

/* Publish a recorded capture from its completed readback. Returns 1 published, -1 failed,
 * and 0 when no capture was recorded (nothing to do). */
int sr_capture_publish_recorded(const void *px, uint32_t row_bytes, sr_cap_order order);

#ifdef __cplusplus
}
#endif

#endif /* NAKAGAWA_RECOMP_FBCAP_H */
