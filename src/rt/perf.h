// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#ifndef SR_PERF_H
#define SR_PERF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SrPerfGeReason {
    SR_PERF_GE_RENDER_BATCH = 0,
    SR_PERF_GE_SNAPSHOT_COPY,
    SR_PERF_GE_TEXTURE_UPLOAD,
    SR_PERF_GE_TARGET_UPLOAD,
    SR_PERF_GE_DEPTH_UPLOAD,
    SR_PERF_GE_DEPTH_READBACK,
    SR_PERF_GE_TARGET_READBACK_TRANSITION,
    SR_PERF_GE_TRANSFER_BLIT,
    SR_PERF_GE_INIT,
    /* A wait-all covering submissions with more than one reason cannot be split
     * truthfully after the fact. Keep it separate instead of charging the same wall
     * duration in full to every reason represented by the fence set. */
    SR_PERF_GE_MIXED_DRAIN,
    SR_PERF_GE_REASON_COUNT,
} SrPerfGeReason;

typedef enum SrPerfGeEvent {
    SR_PERF_GE_SHBLEND_STATE = 0,
    SR_PERF_GE_SHBLEND_DRAW,
    SR_PERF_GE_SHBLEND_BATCH,
    SR_PERF_GE_SHBLEND_FB16,
    SR_PERF_GE_SHBLEND_DITHER,
    SR_PERF_GE_SHBLEND_ABSDIFF,
    SR_PERF_GE_SHBLEND_DOUBLE_DST_ALPHA,
    SR_PERF_GE_SHBLEND_DOUBLE_SRC_ALPHA_DST,
    SR_PERF_GE_SHBLEND_DUAL_FIX,
    SR_PERF_GE_SNAPSHOT_REQUEST,
    SR_PERF_GE_SNAPSHOT_CACHE_HIT,
    SR_PERF_GE_SNAPSHOT_COPIED,
    SR_PERF_GE_EVENT_COUNT,
} SrPerfGeEvent;

/* Low-overhead, opt-in runtime telemetry. Enable with SR_PERF=1. The reporter
 * emits one aggregate row per second; SR_PERF_CSV optionally names a CSV file. */
void     sr_perf_init(void);
uint64_t sr_perf_now_ns(void);
void     sr_perf_guest_begin(void);
void     sr_perf_guest_end(void);
void     sr_perf_guest_idle_wait(uint64_t started_ns);
void     sr_perf_vblank(void);
void     sr_perf_ge_submit(SrPerfGeReason reason);
void     sr_perf_ge_wait(uint64_t started_ns, SrPerfGeReason reason);
void     sr_perf_ge_event(SrPerfGeEvent event, uint64_t count);
void     sr_perf_present_submit(void);
void     sr_perf_present_wait(uint64_t started_ns);
void     sr_perf_present_done(uint64_t started_ns, int result);
void     sr_perf_present_skip(void);

#ifdef __cplusplus
}
#endif

#endif
