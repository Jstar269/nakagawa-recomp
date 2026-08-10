// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "perf.h"

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SrPerfState {
    int enabled;
    int guest_active;
    uint64_t interval_start_ns;
    uint64_t guest_start_ns;
    uint64_t guest_ns;
    uint64_t guest_idle_ns;
    uint64_t ge_wait_ns;
    uint64_t present_ns;
    uint64_t present_wait_ns;
    uint64_t vblanks;
    uint64_t total_vblanks;
    uint64_t presents;
    uint64_t ge_submits;
    uint64_t present_submits;
    uint64_t ge_waits;
    uint64_t present_waits;
    uint64_t present_skips;
    uint64_t readback_waits;
    uint64_t ge_reason_submits[SR_PERF_GE_REASON_COUNT];
    uint64_t ge_reason_waits[SR_PERF_GE_REASON_COUNT];
    uint64_t ge_reason_wait_ns[SR_PERF_GE_REASON_COUNT];
    uint64_t ge_events[SR_PERF_GE_EVENT_COUNT];
    FILE *csv;
} SrPerfState;

static SrPerfState s_perf;

static int env_on(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static double ms(uint64_t ns) { return (double)ns / 1000000.0; }

static void report_if_due(uint64_t now) {
    if (!s_perf.enabled || now - s_perf.interval_start_ns < 1000000000ull) return;

    if (s_perf.guest_active) {
        s_perf.guest_ns += now - s_perf.guest_start_ns;
        s_perf.guest_start_ns = now;
    }

    uint64_t wall_ns = now - s_perf.interval_start_ns;
    uint64_t scheduler_ns = wall_ns > s_perf.guest_ns ? wall_ns - s_perf.guest_ns : 0;
    uint64_t idle_ns = scheduler_ns + s_perf.guest_idle_ns;
    uint64_t excluded_ns = s_perf.ge_wait_ns + s_perf.present_ns + s_perf.guest_idle_ns;
    uint64_t cpu_ns = s_perf.guest_ns > excluded_ns ? s_perf.guest_ns - excluded_ns : 0;
    uint64_t submits = s_perf.ge_submits + s_perf.present_submits;
    uint64_t waits = s_perf.ge_waits + s_perf.present_waits;
    double seconds = (double)wall_ns / 1000000000.0;
    double fps = seconds > 0.0 ? (double)s_perf.presents / seconds : 0.0;
    double vblank_hz = seconds > 0.0 ? (double)s_perf.vblanks / seconds : 0.0;
    double frame_ms = s_perf.presents ? ms(wall_ns) / (double)s_perf.presents : 0.0;

    fprintf(stderr,
            "PERF vblank_total=%llu wall_ms=%.3f fps=%.3f frame_ms=%.3f vblank_hz=%.3f "
            "cpu_ms=%.3f ge_wait_ms=%.3f present_ms=%.3f idle_ms=%.3f "
            "submits=%llu ge_submits=%llu present_submits=%llu waits=%llu "
            "readback_waits=%llu present_skips=%llu present_wait_ms=%.3f target30=%s "
            "ge_reason_submit=[render=%llu,snapshot=%llu,texup=%llu,targetup=%llu,depthup=%llu,depthread=%llu,targetread=%llu,xfer=%llu,init=%llu,mixed=%llu] "
            "ge_reason_wait_ms=[render=%.3f,snapshot=%.3f,texup=%.3f,targetup=%.3f,depthup=%.3f,depthread=%.3f,targetread=%.3f,xfer=%.3f,init=%.3f,mixed=%.3f] "
            "shblend=[states=%llu,draws=%llu,batches=%llu,fb16=%llu,dither=%llu,absdiff=%llu,double_dst_alpha=%llu,double_src_alpha_dst=%llu,dual_fix=%llu] "
            "snapshot=[requests=%llu,hits=%llu,copies=%llu]\n",
            (unsigned long long)s_perf.total_vblanks,
            ms(wall_ns), fps, frame_ms, vblank_hz, ms(cpu_ns), ms(s_perf.ge_wait_ns),
            ms(s_perf.present_ns), ms(idle_ns), (unsigned long long)submits,
            (unsigned long long)s_perf.ge_submits,
            (unsigned long long)s_perf.present_submits, (unsigned long long)waits,
            (unsigned long long)s_perf.readback_waits,
            (unsigned long long)s_perf.present_skips, ms(s_perf.present_wait_ns),
            fps >= 29.5 ? "yes" : "no",
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_RENDER_BATCH],
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_SNAPSHOT_COPY],
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_TEXTURE_UPLOAD],
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_TARGET_UPLOAD],
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_DEPTH_UPLOAD],
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_DEPTH_READBACK],
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_TARGET_READBACK_TRANSITION],
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_TRANSFER_BLIT],
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_INIT],
            (unsigned long long)s_perf.ge_reason_submits[SR_PERF_GE_MIXED_DRAIN],
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_RENDER_BATCH]),
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_SNAPSHOT_COPY]),
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_TEXTURE_UPLOAD]),
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_TARGET_UPLOAD]),
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_DEPTH_UPLOAD]),
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_DEPTH_READBACK]),
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_TARGET_READBACK_TRANSITION]),
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_TRANSFER_BLIT]),
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_INIT]),
            ms(s_perf.ge_reason_wait_ns[SR_PERF_GE_MIXED_DRAIN]),
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SHBLEND_STATE],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SHBLEND_DRAW],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SHBLEND_BATCH],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SHBLEND_FB16],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SHBLEND_DITHER],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SHBLEND_ABSDIFF],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SHBLEND_DOUBLE_DST_ALPHA],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SHBLEND_DOUBLE_SRC_ALPHA_DST],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SHBLEND_DUAL_FIX],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SNAPSHOT_REQUEST],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SNAPSHOT_CACHE_HIT],
            (unsigned long long)s_perf.ge_events[SR_PERF_GE_SNAPSHOT_COPIED]);

    if (s_perf.csv) {
        fprintf(s_perf.csv,
                "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%s,"
                "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned long long)s_perf.total_vblanks,
                ms(wall_ns), fps, frame_ms, vblank_hz, ms(cpu_ns), ms(s_perf.ge_wait_ns),
                ms(s_perf.present_ns), ms(idle_ns), (unsigned long long)submits,
                (unsigned long long)s_perf.ge_submits,
                (unsigned long long)s_perf.present_submits, (unsigned long long)waits,
                (unsigned long long)s_perf.readback_waits,
                (unsigned long long)s_perf.present_skips, ms(s_perf.present_wait_ns),
                fps >= 29.5 ? "yes" : "no",
                (unsigned long long)s_perf.ge_reason_submits[0], (unsigned long long)s_perf.ge_reason_submits[1],
                (unsigned long long)s_perf.ge_reason_submits[2], (unsigned long long)s_perf.ge_reason_submits[3],
                (unsigned long long)s_perf.ge_reason_submits[4], (unsigned long long)s_perf.ge_reason_submits[5],
                (unsigned long long)s_perf.ge_reason_submits[6], (unsigned long long)s_perf.ge_reason_submits[7],
                (unsigned long long)s_perf.ge_reason_submits[8], (unsigned long long)s_perf.ge_reason_submits[9],
                (unsigned long long)s_perf.ge_reason_waits[0], (unsigned long long)s_perf.ge_reason_waits[1],
                (unsigned long long)s_perf.ge_reason_waits[2], (unsigned long long)s_perf.ge_reason_waits[3],
                (unsigned long long)s_perf.ge_reason_waits[4], (unsigned long long)s_perf.ge_reason_waits[5],
                (unsigned long long)s_perf.ge_reason_waits[6], (unsigned long long)s_perf.ge_reason_waits[7],
                (unsigned long long)s_perf.ge_reason_waits[8], (unsigned long long)s_perf.ge_reason_waits[9],
                ms(s_perf.ge_reason_wait_ns[0]), ms(s_perf.ge_reason_wait_ns[1]),
                ms(s_perf.ge_reason_wait_ns[2]), ms(s_perf.ge_reason_wait_ns[3]),
                ms(s_perf.ge_reason_wait_ns[4]), ms(s_perf.ge_reason_wait_ns[5]),
                ms(s_perf.ge_reason_wait_ns[6]), ms(s_perf.ge_reason_wait_ns[7]),
                ms(s_perf.ge_reason_wait_ns[8]), ms(s_perf.ge_reason_wait_ns[9]),
                (unsigned long long)s_perf.ge_events[0], (unsigned long long)s_perf.ge_events[1],
                (unsigned long long)s_perf.ge_events[2], (unsigned long long)s_perf.ge_events[3],
                (unsigned long long)s_perf.ge_events[4], (unsigned long long)s_perf.ge_events[5],
                (unsigned long long)s_perf.ge_events[6], (unsigned long long)s_perf.ge_events[7],
                (unsigned long long)s_perf.ge_events[8], (unsigned long long)s_perf.ge_events[9],
                (unsigned long long)s_perf.ge_events[10], (unsigned long long)s_perf.ge_events[11]);
        fflush(s_perf.csv);
    }
    fflush(stderr);

    int guest_active = s_perf.guest_active;
    FILE *csv = s_perf.csv;
    uint64_t total_vblanks = s_perf.total_vblanks;
    memset(&s_perf, 0, sizeof(s_perf));
    s_perf.enabled = 1;
    s_perf.guest_active = guest_active;
    s_perf.guest_start_ns = guest_active ? now : 0;
    s_perf.interval_start_ns = now;
    s_perf.total_vblanks = total_vblanks;
    s_perf.csv = csv;
}

void sr_perf_init(void) {
    memset(&s_perf, 0, sizeof(s_perf));
    s_perf.enabled = env_on("SR_PERF");
    if (!s_perf.enabled) return;
    s_perf.interval_start_ns = SDL_GetTicksNS();
    const char *path = getenv("SR_PERF_CSV");
    if (path && path[0]) {
        s_perf.csv = fopen(path, "w");
        if (s_perf.csv) {
            fputs("vblank_total,wall_ms,fps,frame_ms,vblank_hz,cpu_ms,ge_wait_ms,present_ms,idle_ms,"
                  "submits,ge_submits,present_submits,waits,readback_waits,present_skips,"
                  "present_wait_ms,target30,"
                  "ge_submit_render,ge_submit_snapshot,ge_submit_texup,ge_submit_targetup,ge_submit_depthup,ge_submit_depthread,ge_submit_targetread,ge_submit_xfer,ge_submit_init,ge_submit_mixed,"
                  "ge_wait_render,ge_wait_snapshot,ge_wait_texup,ge_wait_targetup,ge_wait_depthup,ge_wait_depthread,ge_wait_targetread,ge_wait_xfer,ge_wait_init,ge_wait_mixed,"
                  "ge_wait_render_ms,ge_wait_snapshot_ms,ge_wait_texup_ms,ge_wait_targetup_ms,ge_wait_depthup_ms,ge_wait_depthread_ms,ge_wait_targetread_ms,ge_wait_xfer_ms,ge_wait_init_ms,ge_wait_mixed_ms,"
                  "shblend_states,shblend_draws,shblend_batches,shblend_fb16,shblend_dither,shblend_absdiff,shblend_double_dst_alpha,shblend_double_src_alpha_dst,shblend_dual_fix,"
                  "snapshot_requests,snapshot_cache_hits,snapshot_copies\n",
                  s_perf.csv);
            fflush(s_perf.csv);
        } else {
            fprintf(stderr, "PERF: cannot open CSV output %s\n", path);
        }
    }
    fprintf(stderr, "PERF enabled: 1 Hz aggregate telemetry%s\n", s_perf.csv ? " + CSV" : "");
}

uint64_t sr_perf_now_ns(void) {
    return s_perf.enabled ? SDL_GetTicksNS() : 0;
}

void sr_perf_guest_begin(void) {
    if (!s_perf.enabled || s_perf.guest_active) return;
    s_perf.guest_active = 1;
    s_perf.guest_start_ns = SDL_GetTicksNS();
}

void sr_perf_guest_end(void) {
    if (!s_perf.enabled || !s_perf.guest_active) return;
    uint64_t now = SDL_GetTicksNS();
    s_perf.guest_ns += now - s_perf.guest_start_ns;
    s_perf.guest_active = 0;
    report_if_due(now);
}

void sr_perf_guest_idle_wait(uint64_t started_ns) {
    if (!s_perf.enabled || !s_perf.guest_active || !started_ns) return;
    s_perf.guest_idle_ns += SDL_GetTicksNS() - started_ns;
}

void sr_perf_vblank(void) {
    if (!s_perf.enabled) return;
    s_perf.vblanks++;
    s_perf.total_vblanks++;
    report_if_due(SDL_GetTicksNS());
}

void sr_perf_ge_submit(SrPerfGeReason reason) {
    if (!s_perf.enabled) return;
    s_perf.ge_submits++;
    if ((unsigned)reason < SR_PERF_GE_REASON_COUNT) s_perf.ge_reason_submits[reason]++;
}

void sr_perf_ge_wait(uint64_t started_ns, SrPerfGeReason reason) {
    if (!s_perf.enabled || !started_ns) return;
    uint64_t elapsed = SDL_GetTicksNS() - started_ns;
    s_perf.ge_wait_ns += elapsed;
    s_perf.ge_waits++;
    if ((unsigned)reason < SR_PERF_GE_REASON_COUNT) {
        s_perf.ge_reason_waits[reason]++;
        s_perf.ge_reason_wait_ns[reason] += elapsed;
    }
    if (reason == SR_PERF_GE_DEPTH_READBACK ||
        reason == SR_PERF_GE_TARGET_READBACK_TRANSITION)
        s_perf.readback_waits++;
}

void sr_perf_ge_event(SrPerfGeEvent event, uint64_t count) {
    if (s_perf.enabled && (unsigned)event < SR_PERF_GE_EVENT_COUNT)
        s_perf.ge_events[event] += count;
}

void sr_perf_present_submit(void) { if (s_perf.enabled) s_perf.present_submits++; }

void sr_perf_present_wait(uint64_t started_ns) {
    if (!s_perf.enabled || !started_ns) return;
    s_perf.present_wait_ns += SDL_GetTicksNS() - started_ns;
    s_perf.present_waits++;
}

void sr_perf_present_done(uint64_t started_ns, int result) {
    if (!s_perf.enabled || !started_ns) return;
    s_perf.present_ns += SDL_GetTicksNS() - started_ns;
    if (result == 1) s_perf.presents++;
}

void sr_perf_present_skip(void) { if (s_perf.enabled) s_perf.present_skips++; }
