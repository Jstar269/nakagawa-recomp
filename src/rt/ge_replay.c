// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "ge_capture.h"
#include "ge_shared.h"
#include "recomp.h"
#include "gpu_sdl3vk/ge_gpu.h"
#include "gpu_sdl3vk/sdl3vk.h"

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standalone support required by recomp.h's checked memory accessors. The replay owns a
 * private guest arena and deliberately does not link generated game code or HLE state. */
uint8_t *g_mem;
uint32_t g_sr_debug;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count;
int g_sr_metadata_watch;
int g_sr_heap_watch;
int g_hle_depth;
CpuState *s_cpu;

/* Store-context / last-writer diagnostic state referenced by ge.c. The replay links the
 * production GE object but not debug.c, so it supplies the same inert definitions that
 * gpu_coherence_selftest.c does. Without these the target does not link at all. */
int g_sr_last_writer_enabled;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset;
unsigned g_sr_store_context_mem_words;

void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
void sr_add_mem_watch(uint32_t start, uint32_t end, const char *label) {
    (void)start; (void)end; (void)label;
}
int sr_find_last_writer(uint32_t addr, uint32_t width,
                        uint32_t *write_addr, uint32_t *write_width,
                        uint32_t *value, uint32_t *pc) {
    (void)addr; (void)width; (void)write_addr; (void)write_width;
    (void)value; (void)pc;
    return 0;
}

void sr_oor(uint32_t addr, uint32_t value, int store) {
    fprintf(stderr, "ge_replay: out-of-range %s addr=0x%08x value=0x%08x\n",
            store ? "write" : "read", addr, value);
}
void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)addr; (void)width; (void)pc;
}
uint32_t sched_current_uid(void) { return 0; }
uint32_t sr_get_ge_status(void) { return 0; }

static uint32_t fb_addr(const GeState *state) {
    uint32_t addr = state->fbp;
    if ((addr & 0x0F000000u) == 0x04000000u) return addr;
    return 0x04000000u | (addr & 0x001FFFFFu);
}

static void unpack(uint32_t raw, uint32_t fmt, unsigned char rgb[3]) {
    uint32_t r, g, b;
    switch (fmt & 3u) {
        case 0: r = raw & 31u; g = (raw >> 5) & 63u; b = (raw >> 11) & 31u;
                r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); break;
        case 1: r = raw & 31u; g = (raw >> 5) & 31u; b = (raw >> 10) & 31u;
                r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2); break;
        case 2: r = (raw & 15u) * 17u; g = ((raw >> 4) & 15u) * 17u; b = ((raw >> 8) & 15u) * 17u; break;
        default: r = raw & 255u; g = (raw >> 8) & 255u; b = (raw >> 16) & 255u; break;
    }
    rgb[0] = (unsigned char)r; rgb[1] = (unsigned char)g; rgb[2] = (unsigned char)b;
}

static int write_ppm(const char *path, const GeState *state) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    uint32_t base = fb_addr(state);
    uint32_t stride = state->fbw ? state->fbw : 512u;
    uint32_t bpp = (state->fbfmt & 3u) == 3u ? 4u : 2u;
    int ok = fprintf(file, "P6\n480 272\n255\n") > 0;
    for (uint32_t y = 0; ok && y < 272; y++) {
        for (uint32_t x = 0; x < 480; x++) {
            uint32_t addr = base + (y * stride + x) * bpp;
            uint32_t raw = bpp == 4 ? sr_r32(addr) : sr_r16(addr);
            unsigned char rgb[3];
            unpack(raw, state->fbfmt, rgb);
            if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) { ok = 0; break; }
        }
    }
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static void usage(const char *exe) {
    fprintf(stderr, "usage: %s <fixture.ngef> [--backend software|vulkan] [--repeat N] [--output frame.ppm]\n", exe);
}

typedef enum ReplayWallPhase {
    REPLAY_WALL_RESET = 0,
    REPLAY_WALL_FIXTURE_APPLY,
    REPLAY_WALL_GE_RESTORE,
    REPLAY_WALL_GE_LISTS,
    REPLAY_WALL_LOOP_OTHER,
    REPLAY_WALL_MATERIALIZE,
    REPLAY_WALL_PHASE_COUNT,
} ReplayWallPhase;

typedef struct ReplayWallPhaseStats {
    uint64_t calls;
    uint64_t ns;
} ReplayWallPhaseStats;

static int env_enabled(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static uint64_t wall_phase_begin(int enabled) {
    return enabled ? SDL_GetTicksNS() : 0;
}

static void wall_phase_end(ReplayWallPhaseStats *phase, int enabled, uint64_t started) {
    if (!enabled) return;
    phase->calls++;
    phase->ns += SDL_GetTicksNS() - started;
}

static uint64_t primitive_profile_estimate_ns(uint64_t raw_ns, uint64_t samples,
                                              uint64_t eligible) {
    if (!raw_ns || !samples || !eligible) return 0;
    long double scaled = (long double)raw_ns * (long double)eligible / (long double)samples;
    if (scaled >= (long double)UINT64_MAX) return UINT64_MAX;
    return (uint64_t)scaled;
}

/* The empty control is a separate interval, so subtracting it from a phase or
 * sampled-total interval would double-count work that is not inside that
 * interval.  Keep an explicit adjusted value for reports, equal to the raw
 * estimate until a future design puts the control machinery inside the timed
 * interval. */
static uint64_t primitive_profile_adjusted_estimate_ns(uint64_t raw_ns, uint64_t samples,
                                                       uint64_t eligible) {
    return primitive_profile_estimate_ns(raw_ns, samples, eligible);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    const char *fixture_path = argv[1];
    const char *backend = "software";
    const char *output = "ge_replay.ppm";
    unsigned long repeat = 1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) backend = argv[++i];
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) repeat = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) output = argv[++i];
        else { usage(argv[0]); return 2; }
    }
    int vulkan = strcmp(backend, "vulkan") == 0;
    if ((!vulkan && strcmp(backend, "software") != 0) || repeat == 0 || repeat > 1000000ul) {
        usage(argv[0]); return 2;
    }

    GeCaptureFixture fixture;
    if (!ge_capture_load(fixture_path, &fixture)) {
        fprintf(stderr, "ge_replay: invalid fixture %s\n", fixture_path);
        return 2;
    }
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) { ge_capture_free(&fixture); return 2; }
    g_mem = arena + 0x08000000u;
    sr_perf_init();

    if (vulkan) {
        if (!sdl3vk_init("Nakagawa GE replay") || !gegpu_init()) {
            fprintf(stderr, "ge_replay: Vulkan initialization failed\n");
            ge_capture_free(&fixture); free(arena); return 2;
        }
        gegpu_replay_stats_reset();
    }
    ge_cpu_profile_reset();

    int wall_profile = env_enabled("SR_GE_REPLAY_WALL_PROFILE") ||
                       env_enabled("SR_GPU_CPU_PROFILE");
    ReplayWallPhaseStats wall_stats[REPLAY_WALL_PHASE_COUNT] = {{0}};
    uint64_t prior_assigned_ns = 0;
    uint64_t started = SDL_GetTicksNS();
    for (unsigned long i = 0; i < repeat; i++) {
        uint64_t frame_started = wall_phase_begin(wall_profile);
        uint64_t phase_started = wall_phase_begin(wall_profile);
        if (vulkan && !gegpu_replay_reset()) return 3;
        wall_phase_end(&wall_stats[REPLAY_WALL_RESET], wall_profile, phase_started);
        phase_started = wall_phase_begin(wall_profile);
        if (!ge_capture_apply(&fixture, ge_state_ptr(), ge_zbuf_ptr())) return 3;
        wall_phase_end(&wall_stats[REPLAY_WALL_FIXTURE_APPLY], wall_profile, phase_started);
        phase_started = wall_phase_begin(wall_profile);
        ge_replay_restore(&fixture.initial_state, fixture.initial_zbuf);
        wall_phase_end(&wall_stats[REPLAY_WALL_GE_RESTORE], wall_profile, phase_started);
        phase_started = wall_phase_begin(wall_profile);
        for (uint32_t list = 0; list < fixture.list_count; list++) {
            if (ge_run_list(fixture.list_addrs[list], 0) != 0) {
                fprintf(stderr, "ge_replay: list %u did not terminate at iteration %lu\n", list, i);
                return 3;
            }
        }
        wall_phase_end(&wall_stats[REPLAY_WALL_GE_LISTS], wall_profile, phase_started);
        if (wall_profile) {
            uint64_t frame_ns = SDL_GetTicksNS() - frame_started;
            uint64_t assigned_ns = 0;
            for (unsigned phase = REPLAY_WALL_RESET; phase <= REPLAY_WALL_GE_LISTS; phase++)
                assigned_ns += wall_stats[phase].ns;
            uint64_t frame_assigned_ns = assigned_ns - prior_assigned_ns;
            prior_assigned_ns = assigned_ns;
            wall_stats[REPLAY_WALL_LOOP_OTHER].calls++;
            wall_stats[REPLAY_WALL_LOOP_OTHER].ns +=
                frame_ns > frame_assigned_ns ? frame_ns - frame_assigned_ns : 0;
        }
    }
    uint64_t materialize_started = wall_phase_begin(wall_profile);
    if (vulkan && !gegpu_capture_materialize()) return 3;
    wall_phase_end(&wall_stats[REPLAY_WALL_MATERIALIZE], wall_profile, materialize_started);
    uint64_t elapsed = SDL_GetTicksNS() - started;
    if (!write_ppm(output, ge_state_ptr())) {
        fprintf(stderr, "ge_replay: could not write %s\n", output);
        return 3;
    }
    GeGpuReplayStats stats;
    memset(&stats, 0, sizeof(stats));
    if (vulkan) gegpu_replay_stats_get(&stats);
    GeGpuCpuProfileStats cpu_stats;
    memset(&cpu_stats, 0, sizeof(cpu_stats));
    if (vulkan) gegpu_cpu_profile_stats_get(&cpu_stats);
    GeCpuProfileStats ge_cpu_stats;
    memset(&ge_cpu_stats, 0, sizeof(ge_cpu_stats));
    ge_cpu_profile_get(&ge_cpu_stats);
    static const char *boundary_names[GEGPU_BOUNDARY_COUNT] = {
        "render_snapshot", "texture_upload", "target_upload", "depth_upload",
        "readback", "present", "lifetime", "other",
    };
    printf("GE_REPLAY backend=%s repeats=%lu wall_ms=%.3f ms_per_frame=%.6f frame=%u lists=%u pages=%u "
           "fbp=0x%08x fbw=%u fbfmt=%u "
           "queue_submits=%llu render_submits=%llu render_waits=%llu render_wait_ms=%.3f "
           "snapshot_requests=%llu snapshot_copies=%llu snapshot_submits=%llu "
           "snapshot_waits=%llu snapshot_wait_ms=%.3f mixed_submits=%llu mixed_waits=%llu mixed_wait_ms=%.3f "
           "batches=%llu draws=%llu upload_ring_reservations=%llu upload_ring_bytes=%llu "
           "upload_ring_wraps=%llu upload_ring_fallbacks=%llu upload_ring_high_water=%llu output=%s\n",
           backend, repeat, (double)elapsed / 1000000.0,
           (double)elapsed / 1000000.0 / (double)repeat,
           fixture.frame, fixture.list_count, fixture.page_count,
           ge_state_ptr()->fbp, ge_state_ptr()->fbw, ge_state_ptr()->fbfmt,
           stats.queue_submits, stats.render_submits, stats.render_waits,
           (double)stats.render_wait_ns / 1000000.0,
           stats.snapshot_requests, stats.snapshot_copies, stats.snapshot_submits,
           stats.snapshot_waits, (double)stats.snapshot_wait_ns / 1000000.0,
           stats.mixed_submits,
           stats.mixed_waits, (double)stats.mixed_wait_ns / 1000000.0,
           stats.batches, stats.draws,
           stats.upload_ring_reservations, stats.upload_ring_bytes,
           stats.upload_ring_wraps, stats.upload_ring_fallbacks,
           stats.upload_ring_high_water, output);
    fflush(stdout);
    if (vulkan) {
        for (unsigned i = 0; i < GEGPU_BOUNDARY_COUNT; i++) {
            const GeGpuReplayBoundaryStats *b = &stats.boundary[i];
            printf("GE_REPLAY_BOUNDARY reason=%s submits=%llu submit_ms=%.3f waits=%llu wait_ms=%.3f total_ms=%.3f\n",
                   boundary_names[i], b->submits, (double)b->submit_ns / 1000000.0,
                   b->waits, (double)b->wait_ns / 1000000.0,
                   (double)(b->submit_ns + b->wait_ns) / 1000000.0);
        }
        fflush(stdout);
    }
    if (vulkan && cpu_stats.enabled) {
        static const char *phase_names[GEGPU_CPU_PHASE_COUNT] = {
            "state_prep", "state_key", "pipeline_lookup", "pipeline_create",
            "descriptor_alloc", "descriptor_update", "bind_record", "object_lookup",
            "texture_decode", "texture_shadow", "vertex_prep", "snapshot_target", "snapshot_decision",
            "snapshot_region", "snapshot_metadata", "command_record", "memcpy",
            "heap",
        };
        for (unsigned i = 0; i < GEGPU_CPU_PHASE_COUNT; i++) {
            const GeGpuCpuPhaseStats *p = &cpu_stats.phase[i];
            printf("GE_REPLAY_CPU phase=%s calls=%llu ns=%llu ms=%.6f\n",
                   phase_names[i], p->calls, p->ns, (double)p->ns / 1000000.0);
        }
        printf("GE_REPLAY_CPU_COUNTS state_key_builds=%llu state_cache_hits=%llu state_cache_misses=%llu "
               "pipeline_hits=%llu pipeline_misses=%llu pipeline_creations=%llu "
               "descriptor_allocations=%llu descriptor_updates=%llu pipeline_binds=%llu "
               "pipeline_bind_redundant=%llu descriptor_binds=%llu descriptor_bind_redundant=%llu "
               "texture_hits=%llu texture_misses=%llu texture_shadow_checks=%llu "
               "texture_shadow_hits=%llu texture_shadow_bytes=%llu "
               "snapshot_requests=%llu snapshot_copies=%llu "
               "vertex_bytes=%llu memcpy_bytes=%llu "
               "target_calls=%llu target_fast_hits=%llu target_acquires=%llu "
               "ensure_room_calls=%llu ensure_room_flushes=%llu "
               "append_calls=%llu append_compare_calls=%llu append_merges=%llu\n",
               cpu_stats.state_key_builds, cpu_stats.state_cache_hits, cpu_stats.state_cache_misses,
               cpu_stats.pipeline_hits, cpu_stats.pipeline_misses, cpu_stats.pipeline_creations,
               cpu_stats.descriptor_allocations, cpu_stats.descriptor_updates,
               cpu_stats.pipeline_binds, cpu_stats.pipeline_bind_redundant,
               cpu_stats.descriptor_binds, cpu_stats.descriptor_bind_redundant,
               cpu_stats.texture_hits, cpu_stats.texture_misses,
               cpu_stats.texture_shadow_checks, cpu_stats.texture_shadow_hits,
               cpu_stats.texture_shadow_bytes,
               cpu_stats.snapshot_requests, cpu_stats.snapshot_copies,
               cpu_stats.vertex_bytes, cpu_stats.memcpy_bytes,
               cpu_stats.target_calls, cpu_stats.target_fast_hits, cpu_stats.target_acquires,
               cpu_stats.ensure_room_calls, cpu_stats.ensure_room_flushes,
               cpu_stats.append_calls, cpu_stats.append_compare_calls,
               cpu_stats.append_merges);
        for (unsigned i = 0; i < GEGPU_CPU_PHASE_COUNT; i++) {
            const GeGpuCpuPhaseStats *p = &cpu_stats.hook_phase[i];
            printf("GE_REPLAY_HOOK_CPU phase=%s calls=%llu ns=%llu ms=%.6f\n",
                   phase_names[i], p->calls, p->ns, (double)p->ns / 1000000.0);
        }
        printf("GE_REPLAY_HOOK_COUNTS calls=%llu submit_ns=%llu wait_ns=%llu\n",
               cpu_stats.hook_calls, cpu_stats.hook_submit_ns, cpu_stats.hook_wait_ns);
        fflush(stdout);
    }
    if (ge_cpu_stats.enabled) {
        static const char *phase_names[GE_CPU_PHASE_COUNT] = {
            "list_total", "command_dispatch", "primitive", "gpu_hook", "block_transfer", "clut_load", "flush",
        };
        for (unsigned i = 0; i < GE_CPU_PHASE_COUNT; i++) {
            const GeCpuPhaseStats *p = &ge_cpu_stats.phase[i];
            printf("GE_REPLAY_GE_CPU phase=%s calls=%llu ns=%llu ms=%.6f\n",
                   phase_names[i], (unsigned long long)p->calls,
                   (unsigned long long)p->ns, (double)p->ns / 1000000.0);
        }
        printf("GE_REPLAY_GE_CPU_COUNTS commands=%llu primitive_commands=%llu primitive_vertices=%llu "
               "vertex_decode_uses=%llu strip_vertices_reused=%llu "
               "prim_type0=%llu prim_type1=%llu prim_type2=%llu "
               "prim_type3=%llu prim_type4=%llu prim_type5=%llu prim_type6=%llu prim_type7=%llu "
               "block_transfers=%llu clut_loads=%llu flushes=%llu\n",
               (unsigned long long)ge_cpu_stats.commands,
               (unsigned long long)ge_cpu_stats.primitive_commands,
               (unsigned long long)ge_cpu_stats.primitive_vertices,
               (unsigned long long)ge_cpu_stats.vertex_decode_uses,
               (unsigned long long)ge_cpu_stats.strip_vertices_reused,
               (unsigned long long)ge_cpu_stats.primitive_type[0],
               (unsigned long long)ge_cpu_stats.primitive_type[1],
               (unsigned long long)ge_cpu_stats.primitive_type[2],
               (unsigned long long)ge_cpu_stats.primitive_type[3],
               (unsigned long long)ge_cpu_stats.primitive_type[4],
               (unsigned long long)ge_cpu_stats.primitive_type[5],
               (unsigned long long)ge_cpu_stats.primitive_type[6],
               (unsigned long long)ge_cpu_stats.primitive_type[7],
               (unsigned long long)ge_cpu_stats.block_transfers,
               (unsigned long long)ge_cpu_stats.clut_loads,
               (unsigned long long)ge_cpu_stats.flushes);
        fflush(stdout);
    }
    if (ge_cpu_stats.primitive_profile_enabled) {
        static const char *phase_names[GE_PRIM_PROFILE_PHASE_COUNT] = {
            "vertex_fetch_decode", "transform", "lighting",
            "clipping_acceptance", "primitive_assembly",
        };
        printf("GE_REPLAY_GE_PRIM_PROFILE_CONFIG stride=%u timer_pair_ns=%llu\n",
               ge_cpu_stats.primitive_profile_stride,
               (unsigned long long)ge_cpu_stats.primitive_profile_timer_pair_ns);
        for (unsigned i = 0; i < GE_PRIM_PROFILE_PHASE_COUNT; i++) {
            const GeCpuPhaseStats *p = &ge_cpu_stats.primitive_profile_phase[i];
            uint64_t eligible = ge_cpu_stats.primitive_profile_eligible[i];
            uint64_t estimate = primitive_profile_estimate_ns(p->ns, p->calls, eligible);
            printf("GE_REPLAY_GE_PRIM_PROFILE phase=%s calls=%llu ns=%llu ms=%.6f "
                   "eligible=%llu estimated_ns=%llu estimated_ms=%.6f\n",
                   phase_names[i], (unsigned long long)p->calls,
                   (unsigned long long)p->ns, (double)p->ns / 1000000.0,
                   (unsigned long long)eligible, (unsigned long long)estimate,
                   (double)estimate / 1000000.0);
        }
        if (ge_cpu_stats.primitive_profile_calibration_enabled) {
            const GeCpuPhaseStats *control = &ge_cpu_stats.primitive_profile_empty_control;
            const GeCpuPhaseStats *total = &ge_cpu_stats.primitive_profile_sampled_total;
            uint64_t total_eligible = ge_cpu_stats.primitive_profile_triangle_candidates;
            uint64_t total_estimate = primitive_profile_estimate_ns(total->ns, total->calls,
                                                                    total_eligible);
            uint64_t total_adjusted = primitive_profile_adjusted_estimate_ns(
                total->ns, total->calls, total_eligible);
            printf("GE_REPLAY_GE_PRIM_PROFILE_CALIBRATION enabled=1 adjustment=none\n");
            printf("GE_REPLAY_GE_PRIM_PROFILE_CONTROL calls=%llu ns=%llu ms=%.6f per_call_ns=%llu\n",
                   (unsigned long long)control->calls, (unsigned long long)control->ns,
                   (double)control->ns / 1000000.0,
                   (unsigned long long)(control->calls ? control->ns / control->calls : 0));
            printf("GE_REPLAY_GE_PRIM_PROFILE_TOTAL calls=%llu ns=%llu ms=%.6f "
                   "eligible=%llu estimated_ns=%llu estimated_ms=%.6f\n",
                   (unsigned long long)total->calls, (unsigned long long)total->ns,
                   (double)total->ns / 1000000.0, (unsigned long long)total_eligible,
                   (unsigned long long)total_estimate, (double)total_estimate / 1000000.0);
            printf("GE_REPLAY_GE_PRIM_PROFILE_TOTAL_ADJUSTED estimated_ns=%llu estimated_ms=%.6f\n",
                   (unsigned long long)total_adjusted, (double)total_adjusted / 1000000.0);
            for (unsigned i = 0; i < GE_PRIM_PROFILE_PHASE_COUNT; i++) {
                const GeCpuPhaseStats *p = &ge_cpu_stats.primitive_profile_phase[i];
                uint64_t eligible = ge_cpu_stats.primitive_profile_eligible[i];
                uint64_t adjusted = primitive_profile_adjusted_estimate_ns(
                    p->ns, p->calls, eligible);
                printf("GE_REPLAY_GE_PRIM_PROFILE_ADJUSTED phase=%s estimated_ns=%llu estimated_ms=%.6f\n",
                       phase_names[i], (unsigned long long)adjusted,
                       (double)adjusted / 1000000.0);
            }
        }
        printf("GE_REPLAY_GE_PRIM_PROFILE_COUNTS vertices=%llu transform_vertices=%llu "
               "triangle_candidates=%llu\n",
               (unsigned long long)ge_cpu_stats.primitive_profile_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_triangle_candidates);
        uint64_t primitive_commands = 0, submitted_primitives = 0;
        for (unsigned i = 0; i < 8; i++) {
            primitive_commands += ge_cpu_stats.primitive_profile_commands[i];
            submitted_primitives += ge_cpu_stats.primitive_profile_submitted[i];
        }
        printf("GE_REPLAY_GE_PRIM_PROFILE_POPULATION commands=%llu submitted=%llu "
               "vertex_references=%llu triangle_vertex_references=%llu "
               "non_triangle_vertex_references=%llu vertex_uses=%llu "
               "triangle_vertex_uses=%llu non_triangle_vertex_uses=%llu "
               "through_vertex_uses=%llu transform_vertex_uses=%llu "
               "actual_decoded_vertices=%llu actual_transformed_vertices=%llu "
               "actual_through_vertices=%llu strip_cache_commands=%llu strip_cache_hits=%llu "
               "through_triangle_candidates=%llu transform_triangle_candidates=%llu "
               "transform_triangles_drawn=%llu transform_triangles_clipped=%llu "
               "transform_triangles_rejected=%llu non_triangle_primitives=%llu "
               "vertex_rejects=%llu patch_commands=%llu patch_control_vertices=%llu\n",
               (unsigned long long)primitive_commands,
               (unsigned long long)submitted_primitives,
               (unsigned long long)ge_cpu_stats.primitive_profile_vertex_references,
               (unsigned long long)ge_cpu_stats.primitive_profile_triangle_vertex_references,
               (unsigned long long)ge_cpu_stats.primitive_profile_non_triangle_vertex_references,
               (unsigned long long)ge_cpu_stats.primitive_profile_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_triangle_vertex_uses,
               (unsigned long long)ge_cpu_stats.primitive_profile_non_triangle_vertex_uses,
               (unsigned long long)ge_cpu_stats.primitive_profile_through_vertex_uses,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_vertex_uses,
               (unsigned long long)ge_cpu_stats.primitive_profile_actual_decoded_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_actual_transformed_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_actual_through_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_strip_cache_commands,
               (unsigned long long)ge_cpu_stats.primitive_profile_strip_cache_hits,
               (unsigned long long)ge_cpu_stats.primitive_profile_through_triangle_candidates,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_triangle_candidates,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_triangles_drawn,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_triangles_clipped,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_triangles_rejected,
               (unsigned long long)ge_cpu_stats.primitive_profile_non_triangle_primitives,
               (unsigned long long)ge_cpu_stats.primitive_profile_vertex_rejects,
               (unsigned long long)ge_cpu_stats.primitive_profile_patch_commands,
               (unsigned long long)ge_cpu_stats.primitive_profile_patch_control_vertices);
        printf("GE_REPLAY_GE_PRIM_PROFILE_TYPES commands_type0=%llu commands_type1=%llu "
               "commands_type2=%llu commands_type3=%llu commands_type4=%llu "
               "commands_type5=%llu commands_type6=%llu commands_type7=%llu "
               "submitted_type0=%llu submitted_type1=%llu submitted_type2=%llu "
               "submitted_type3=%llu submitted_type4=%llu submitted_type5=%llu "
               "submitted_type6=%llu submitted_type7=%llu\n",
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[0],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[1],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[2],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[3],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[4],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[5],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[6],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[7],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[0],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[1],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[2],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[3],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[4],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[5],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[6],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[7]);
        fflush(stdout);
    }
    if (vulkan && cpu_stats.enabled && ge_cpu_stats.enabled) {
        uint64_t renderer_ns = 0;
        for (unsigned i = 0; i < GEGPU_CPU_PHASE_COUNT; i++)
            renderer_ns += cpu_stats.hook_phase[i].ns;
        uint64_t list_ns = ge_cpu_stats.phase[GE_CPU_LIST_TOTAL].ns;
        uint64_t command_ns = ge_cpu_stats.phase[GE_CPU_COMMAND_DISPATCH].ns;
        uint64_t primitive_ns = ge_cpu_stats.phase[GE_CPU_PRIMITIVE].ns;
        uint64_t gpu_hook_ns = ge_cpu_stats.phase[GE_CPU_GPU_HOOK].ns;
        uint64_t block_ns = ge_cpu_stats.phase[GE_CPU_BLOCK_TRANSFER].ns;
        uint64_t clut_ns = ge_cpu_stats.phase[GE_CPU_CLUT_LOAD].ns;
        uint64_t flush_ns = ge_cpu_stats.phase[GE_CPU_FLUSH].ns;
        uint64_t top_assigned = command_ns + primitive_ns + block_ns + clut_ns + flush_ns;
        uint64_t hook_assigned = renderer_ns + cpu_stats.hook_submit_ns + cpu_stats.hook_wait_ns;
        printf("GE_REPLAY_HIERARCHY list_ns=%llu command_ns=%llu primitive_ns=%llu "
               "primitive_frontend_ns=%llu gpu_hook_ns=%llu block_ns=%llu clut_ns=%llu "
               "flush_ns=%llu list_residual_ns=%llu hook_renderer_ns=%llu hook_submit_ns=%llu "
               "hook_wait_ns=%llu hook_residual_ns=%llu\n",
               (unsigned long long)list_ns, (unsigned long long)command_ns,
               (unsigned long long)primitive_ns,
               (unsigned long long)(primitive_ns > gpu_hook_ns ? primitive_ns - gpu_hook_ns : 0),
               (unsigned long long)gpu_hook_ns, (unsigned long long)block_ns,
               (unsigned long long)clut_ns, (unsigned long long)flush_ns,
               (unsigned long long)(list_ns > top_assigned ? list_ns - top_assigned : 0),
               (unsigned long long)renderer_ns,
               (unsigned long long)cpu_stats.hook_submit_ns,
               (unsigned long long)cpu_stats.hook_wait_ns,
               (unsigned long long)(gpu_hook_ns > hook_assigned ? gpu_hook_ns - hook_assigned : 0));
        fflush(stdout);
    }
    if (wall_profile) {
        static const char *phase_names[REPLAY_WALL_PHASE_COUNT] = {
            "replay_reset", "fixture_apply", "ge_restore", "ge_lists", "loop_other", "materialize",
        };
        static const char *class_names[REPLAY_WALL_PHASE_COUNT] = {
            "HARNESS-ONLY", "HARNESS-ONLY", "HARNESS-ONLY", "PRODUCTION-RELEVANT",
            "HARNESS-ONLY", "PRODUCTION-RELEVANT",
        };
        for (unsigned i = 0; i < REPLAY_WALL_PHASE_COUNT; i++) {
            const ReplayWallPhaseStats *p = &wall_stats[i];
            printf("GE_REPLAY_WALL_PHASE phase=%s classification=%s calls=%llu ns=%llu ms=%.6f\n",
                   phase_names[i], class_names[i], (unsigned long long)p->calls,
                   (unsigned long long)p->ns, (double)p->ns / 1000000.0);
        }
        fflush(stdout);
    }

    if (vulkan) { gegpu_shutdown(); sdl3vk_shutdown(); }
    ge_capture_free(&fixture);
    free(arena);
    return 0;
}
