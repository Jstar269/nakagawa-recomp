// SPDX-License-Identifier: GPL-3.0-or-later
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

typedef enum SrPerfInterpReason {
    SR_PERF_INTERP_DISPATCH_MISS = 0,
    SR_PERF_INTERP_STALE_BLOCK,
    SR_PERF_INTERP_AOT_HANDOFF,
    SR_PERF_INTERP_CALL_RETURN,
    SR_PERF_INTERP_REASON_COUNT,
} SrPerfInterpReason;

typedef enum SrPerfSchedState {
    SR_PERF_SCHED_RUNNING = 0,
    SR_PERF_SCHED_RUNNABLE,
    SR_PERF_SCHED_BLOCKED,
    SR_PERF_SCHED_IDLE,
    SR_PERF_SCHED_STATE_COUNT,
} SrPerfSchedState;

typedef enum SrPerfVfpuFamily {
    SR_PERF_VFPU_LOAD = 0,
    SR_PERF_VFPU_STORE,
    SR_PERF_VFPU_ARITHMETIC,
    SR_PERF_VFPU_PREFIX,
    SR_PERF_VFPU_OTHER,
    SR_PERF_VFPU_FAMILY_COUNT,
} SrPerfVfpuFamily;

typedef enum SrPerfStorageSource {
    SR_PERF_STORAGE_ISO = 0,
    SR_PERF_STORAGE_VFS,
    SR_PERF_STORAGE_SOURCE_COUNT,
} SrPerfStorageSource;

/* Runtime phase, for display-source service-cadence attribution.  A vblank source
 * period that elapses between two eligible service points is not lost guest time --
 * guest VCOUNT counts every elapsed period -- but the guest then observes fewer
 * VBLANK episodes than display periods, because the interrupt is coalesced into a
 * single pending bit.  Naming the phase that was executing when the period came
 * due is the only way to tell a long guest stretch from a host wait.  The tag is a
 * plain global written only at the cheap seams that already exist (AOT/interp
 * entry, syscall entry, GE list execution, a host sleep, the scheduler idle path,
 * the presenter), never per instruction. */
typedef enum SrRtPhase {
    SR_RT_PHASE_OTHER = 0,   /* host work with no more specific owner */
    SR_RT_PHASE_AOT,         /* recompiled guest code */
    SR_RT_PHASE_INTERP,      /* the guest interpreter */
    SR_RT_PHASE_SYSCALL,     /* inside an HLE syscall handler (see sr_rt_nid) */
    SR_RT_PHASE_GE,          /* GE list execution (ge_run_list) */
    SR_RT_PHASE_HOST_WAIT,   /* inside a host sleep (SDL_DelayPrecise) */
    SR_RT_PHASE_SCHED,       /* the scheduler loop, between guest frames */
    SR_RT_PHASE_PRESENT,     /* inside the presenter */
    SR_RT_PHASE_COUNT
} SrRtPhase;

extern int sr_rt_phase;      /* current phase; write directly, read only when attributing */
extern uint32_t sr_rt_nid;   /* NID of the syscall in progress, valid in SR_RT_PHASE_SYSCALL */
extern const char *const sr_rt_phase_name[SR_RT_PHASE_COUNT];
/* Live guest PC accessor, installed by the scheduler (NULL when there is none). */
extern uint32_t (*sr_rt_pc_fn)(void);
extern uint32_t (*sr_rt_uid_fn)(void);

extern int sr_perf_aot_active;
extern int sr_perf_enabled;

void     sr_perf_init(void);
void     sr_perf_shutdown(void);
uint64_t sr_perf_now_ns_impl(void);
#define sr_perf_now_ns() (sr_perf_enabled ? sr_perf_now_ns_impl() : 0)
void     sr_perf_guest_begin(void);
void     sr_perf_guest_end(void);
void     sr_perf_guest_idle_wait(uint64_t started_ns);
void     sr_perf_vblank(void);
/* One latch of the display source: `gap_us` is host-time microseconds since the
 * previous latch, `periods` how many source periods elapsed, `masked` non-zero
 * when those periods elapsed with the CPU interrupt bit CLEAR (a gap the guest
 * chose not to be told about, credited once at resume by the measured rule).
 * A gap over one display period is attributed here because the phase that owned
 * the CPU is unknowable afterwards. */
void     sr_perf_vblank_latch(uint64_t gap_us, uint32_t periods, int masked);
/* A period can be latched and still never reach the guest: a saturated source
 * timeline cannot name the periods it crossed, and the queued episodes that were
 * already owed are destroyed with it.  Those losses are counted where they happen
 * (sr_perf_vblank_collapse, attributed to the phase that destroyed them) while the
 * collapsed remainder is counted as unnamed periods.  sr_perf_vblank_coalesced
 * counts the one coalesced episode a masked window owes, and
 * sr_perf_vblank_service counts what a service point actually handed to the guest,
 * so the report can tell a latched-and-dropped period from a merely late one. */
void     sr_perf_vblank_collapse(uint32_t owed, uint32_t periods);
void     sr_perf_vblank_coalesced(void);
void     sr_perf_vblank_service(uint32_t delivered);
void     sr_perf_phase_report(int force);
void     sr_perf_ge_submit(SrPerfGeReason reason);
void     sr_perf_ge_wait(uint64_t started_ns, SrPerfGeReason reason);
void     sr_perf_ge_event(SrPerfGeEvent event, uint64_t count);
void     sr_perf_present_submit(void);
void     sr_perf_present_wait(uint64_t started_ns);
void     sr_perf_present_done(uint64_t started_ns, int result);
void     sr_perf_present_skip(void);
void     sr_perf_aot_begin(uint32_t pc);
void     sr_perf_aot_end(void);
void     sr_perf_aot_instruction(uint32_t pc, uint32_t opcode);
void     sr_perf_interp_set_reason(SrPerfInterpReason reason);
void     sr_perf_interp_begin(uint32_t entry_pc);
void     sr_perf_interp_instruction(void);
void     sr_perf_interp_end(uint32_t exit_pc, int result);
void     sr_perf_vfpu(uint32_t opcode, uint64_t started_ns, int result);
void     sr_perf_sched_state(SrPerfSchedState state, uint32_t uid);
void     sr_perf_sched_switch(uint32_t from_uid, uint32_t to_uid);
void     sr_perf_ge_cpu(uint64_t started_ns);
void     sr_perf_ge_cpu_phase(uint64_t transform_ns, uint64_t raster_ns);
void     sr_perf_vulkan_submit(uint64_t started_ns);
void     sr_perf_vulkan_wait(uint64_t started_ns, int readback);
void     sr_perf_vulkan_pipeline(uint64_t started_ns);
void     sr_perf_vulkan_readback(uint32_t bytes);
void     sr_perf_texture_cache(int hit);
void     sr_perf_texture_decode(uint64_t started_ns, uint32_t bytes);
void     sr_perf_storage_read(SrPerfStorageSource source, uint32_t bytes,
                              uint64_t started_ns, int success);
void     sr_perf_h264(uint64_t started_ns, int result);
void     sr_perf_atrac(uint64_t started_ns, int result);
void     sr_perf_audio_mix(uint64_t started_ns);
void     sr_perf_audio_output(uint64_t started_ns, uint32_t frames);

/* In-game HUD metrics query (reuses SR_PERF counters, zero overhead when inactive) */
void     sr_perf_enable_counters(void);
void     sr_perf_get_hud_metrics(double *out_fps, double *out_frame_ms, double *out_vblank_hz);

#ifdef __cplusplus
}
#endif

#endif
