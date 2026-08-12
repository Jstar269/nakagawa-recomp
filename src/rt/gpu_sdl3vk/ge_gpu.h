// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

#ifndef GE_GPU_H
#define GE_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the Vulkan objects (shares the sdl3vk device; sdl3vk_init() must have
 * succeeded) and register the capture hooks with ge.c. Returns 1 on success; on
 * failure nothing is registered and the software rasterizer runs unchanged. */
int  gegpu_init(void);

/* GE sync points ("listend", "loadclut"); the backend decides what each requires.
 * Safe to call when idle/uninitialized. */
void gegpu_flush(const char *reason);

/* Present the framebuffer at fbaddr straight from its GPU image. Returns 1 when shown,
 * 0 when the user closed the window, -1 when the address is not GPU-resident
 * (CPU-written movie frames etc.) — the caller should convert guest VRAM instead. */
int  gegpu_present(unsigned int fbaddr, int fmt, unsigned int stride);
int  gegpu_capture_materialize(void);
int  gegpu_replay_reset(void);

/* Complete guest-framebuffer geometry used by the explicit snapshot boundary. The
 * visible extent is separate from stride because PSP display buffers commonly have
 * padding columns that are not part of the published image. */
typedef struct GeGpuFbDescriptor {
    uint32_t addr;
    uint32_t format;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
} GeGpuFbDescriptor;

typedef struct GeGpuFbSpan {
    uint32_t base;
    uint32_t bytes_per_pixel;
    uint32_t row_pitch;
    uint32_t total_bytes;
    int in_vram;
    uint32_t vram_offset;
} GeGpuFbSpan;

#define GEGPU_FB_MAX_STRIDE 1024u
#define GEGPU_FB_MAX_HEIGHT 1024u
#define GEGPU_VRAM_BYTES    0x00200000u

int gegpu_validate_guest_fb_descriptor(const GeGpuFbDescriptor *desc,
                                       GeGpuFbSpan *out_span, const char **why);

typedef enum GeGpuSyncResult {
    GEGPU_SYNC_FAILED = -1,
    GEGPU_SYNC_OK = 1,
    GEGPU_SYNC_NO_TARGET = 2,
} GeGpuSyncResult;

/* Materialize a live GPU target into guest memory, or prove that no target owns the
 * validated span. This is target-scoped and does not make ordinary presentation wait. */
int gegpu_sync_guest_fb(const GeGpuFbDescriptor *desc);

typedef enum GeGpuReplayBoundaryKind {
    GEGPU_BOUNDARY_RENDER_SNAPSHOT = 0,
    GEGPU_BOUNDARY_TEXTURE_UPLOAD,
    GEGPU_BOUNDARY_TARGET_UPLOAD,
    GEGPU_BOUNDARY_DEPTH_UPLOAD,
    GEGPU_BOUNDARY_READBACK,
    GEGPU_BOUNDARY_PRESENT,
    GEGPU_BOUNDARY_LIFETIME,
    GEGPU_BOUNDARY_OTHER,
    GEGPU_BOUNDARY_COUNT,
} GeGpuReplayBoundaryKind;

typedef struct GeGpuReplayBoundaryStats {
    unsigned long long submits;
    unsigned long long submit_ns;
    unsigned long long waits;
    unsigned long long wait_ns;
} GeGpuReplayBoundaryStats;

typedef enum GeGpuCpuPhase {
    GEGPU_CPU_STATE_PREP = 0,
    GEGPU_CPU_STATE_KEY,
    GEGPU_CPU_PIPELINE_LOOKUP,
    GEGPU_CPU_PIPELINE_CREATE,
    GEGPU_CPU_DESCRIPTOR_ALLOC,
    GEGPU_CPU_DESCRIPTOR_UPDATE,
    GEGPU_CPU_BIND_RECORD,
    GEGPU_CPU_OBJECT_LOOKUP,
    GEGPU_CPU_TEXTURE_DECODE,
    GEGPU_CPU_TEXTURE_SHADOW,
    GEGPU_CPU_VERTEX_PREP,
    GEGPU_CPU_SNAPSHOT_TARGET,
    GEGPU_CPU_SNAPSHOT_DECISION,
    GEGPU_CPU_SNAPSHOT_REGION,
    GEGPU_CPU_SNAPSHOT_METADATA,
    GEGPU_CPU_COMMAND_RECORD,
    GEGPU_CPU_MEMCPY,
    GEGPU_CPU_HEAP,
    GEGPU_CPU_PHASE_COUNT,
} GeGpuCpuPhase;

typedef struct GeGpuCpuPhaseStats {
    unsigned long long calls;
    unsigned long long ns;
} GeGpuCpuPhaseStats;

typedef struct GeGpuCpuProfileStats {
    int enabled;
    GeGpuCpuPhaseStats phase[GEGPU_CPU_PHASE_COUNT];
    GeGpuCpuPhaseStats hook_phase[GEGPU_CPU_PHASE_COUNT];
    unsigned long long hook_calls;
    unsigned long long hook_submit_ns;
    unsigned long long hook_wait_ns;
    unsigned long long state_key_builds;
    unsigned long long state_cache_hits;
    unsigned long long state_cache_misses;
    unsigned long long pipeline_hits;
    unsigned long long pipeline_misses;
    unsigned long long pipeline_creations;
    unsigned long long descriptor_allocations;
    unsigned long long descriptor_updates;
    unsigned long long pipeline_binds;
    unsigned long long pipeline_bind_redundant;
    unsigned long long descriptor_binds;
    unsigned long long descriptor_bind_redundant;
    unsigned long long texture_hits;
    unsigned long long texture_misses;
    unsigned long long texture_shadow_checks;
    unsigned long long texture_shadow_hits;
    unsigned long long texture_shadow_bytes;
    unsigned long long snapshot_requests;
    unsigned long long snapshot_copies;
    unsigned long long vertex_bytes;
    unsigned long long memcpy_bytes;
    unsigned long long target_calls;
    unsigned long long target_fast_hits;
    unsigned long long target_acquires;
    unsigned long long ensure_room_calls;
    unsigned long long ensure_room_flushes;
    unsigned long long append_calls;
    unsigned long long append_compare_calls;
    unsigned long long append_merges;
} GeGpuCpuProfileStats;

typedef struct GeGpuReplayStats {
    unsigned long long queue_submits;
    unsigned long long render_submits;
    unsigned long long render_waits;
    unsigned long long render_wait_ns;
    unsigned long long snapshot_requests;
    unsigned long long snapshot_copies;
    unsigned long long snapshot_submits;
    unsigned long long snapshot_waits;
    unsigned long long snapshot_wait_ns;
    unsigned long long mixed_waits;
    unsigned long long mixed_wait_ns;
    unsigned long long mixed_submits;
    unsigned long long batches;
    unsigned long long draws;
    unsigned long long upload_ring_reservations;
    unsigned long long upload_ring_bytes;
    unsigned long long upload_ring_wraps;
    unsigned long long upload_ring_fallbacks;
    unsigned long long upload_ring_high_water;
    GeGpuReplayBoundaryStats boundary[GEGPU_BOUNDARY_COUNT];
} GeGpuReplayStats;

void gegpu_replay_stats_reset(void);
void gegpu_replay_stats_get(GeGpuReplayStats *out);
void gegpu_cpu_profile_stats_get(GeGpuCpuProfileStats *out);

#if defined(SR_GPU_COHERENCE_SELFTEST) || defined(SR_GPU_SNAPSHOT_SYNC_SELFTEST)
int gegpu_coherence_selftest(void);
int gegpu_snapshot_sync_selftest(void);
#endif

void gegpu_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
