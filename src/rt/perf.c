// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* clock_gettime(CLOCK_MONOTONIC) is POSIX: strict -std=c99/c11 builds hide it unless
 * the feature macro is set, and the timer would silently fall back to clock(). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#ifndef PERF_AOT_INSTRUCTIONS
#define PERF_AOT_INSTRUCTIONS 0
#endif

#define SR_PERF_TOP_N 16u
#define SR_PERF_PATH_MAX 4096u
#define SR_PERF_TIER_NONE 0u
#define SR_PERF_TIER_AOT 1u
#define SR_PERF_TIER_INTERP 2u
#define SR_PERF_RESULT_AOT_HANDOFF 1
#define SR_PERF_RESULT_CALL_RETURN 2

#define ADD(field, value) do { \
    s_perf.interval.field += (value); \
    s_perf.total.field += (value); \
} while (0)

#define ADD_ARRAY(field, index, value) do { \
    s_perf.interval.field[(index)] += (value); \
    s_perf.total.field[(index)] += (value); \
} while (0)

typedef struct SrPerfMetrics {
    uint64_t guest_ns;
    uint64_t guest_idle_ns;
    uint64_t ge_wait_ns;
    uint64_t present_ns;
    uint64_t present_wait_ns;
    uint64_t vblanks;
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
    uint64_t aot_ns;
    uint64_t aot_calls;
    uint64_t aot_instructions;
    uint64_t interp_ns;
    uint64_t interp_calls;
    uint64_t interp_instructions;
    uint64_t aot_to_interp;
    uint64_t interp_to_aot;
    uint64_t vfpu_ns;
    uint64_t vfpu_count;
    uint64_t vfpu_interp_count;
    uint64_t vfpu_aot_count;
    uint64_t vfpu_interp_ns;
    uint64_t vfpu_aot_ns;
    uint64_t vfpu_errors;
    uint64_t vfpu_family[SR_PERF_VFPU_FAMILY_COUNT];
    uint64_t sched_ns[SR_PERF_SCHED_STATE_COUNT];
    uint64_t sched_switches;
    uint64_t sched_blocked;
    uint64_t sched_wakes;
    uint64_t ge_cpu_ns;
    uint64_t ge_cpu_calls;
    uint64_t ge_transform_sample_ns;
    uint64_t ge_primitive_ns;
    uint64_t vk_submit_ns;
    uint64_t vk_submits;
    uint64_t vk_wait_ns;
    uint64_t vk_waits;
    uint64_t vk_readback_ns;
    uint64_t vk_readbacks;
    uint64_t vk_readback_bytes;
    uint64_t vk_pipeline_ns;
    uint64_t vk_pipeline_creations;
    uint64_t texture_decode_ns;
    uint64_t texture_decodes;
    uint64_t texture_decode_bytes;
    uint64_t texture_cache_hits;
    uint64_t texture_cache_misses;
    uint64_t storage_ns[SR_PERF_STORAGE_SOURCE_COUNT];
    uint64_t storage_reads[SR_PERF_STORAGE_SOURCE_COUNT];
    uint64_t storage_bytes[SR_PERF_STORAGE_SOURCE_COUNT];
    uint64_t storage_failures[SR_PERF_STORAGE_SOURCE_COUNT];
    uint64_t h264_ns;
    uint64_t h264_calls;
    uint64_t h264_failures;
    uint64_t atrac_ns;
    uint64_t atrac_calls;
    uint64_t atrac_failures;
    uint64_t audio_mix_ns;
    uint64_t audio_mix_calls;
    uint64_t audio_output_ns;
    uint64_t audio_output_calls;
    uint64_t audio_output_frames;
} SrPerfMetrics;

typedef struct SrPerfTransitionEntry {
    uint32_t pc;
    uint32_t reason;
    uint64_t count;
} SrPerfTransitionEntry;

typedef struct SrPerfState {
    int enabled;
    int initialized;
    int shutdown;
    int guest_active;
    unsigned current_tier;
    uint64_t run_start_ns;
    uint64_t interval_start_ns;
    uint64_t attrib_report_ns;   /* last cumulative phase-attribution line, host ns */
    uint64_t guest_start_ns;
    uint64_t total_vblanks;
    uint64_t interval_count;
    SrPerfMetrics interval;
    SrPerfMetrics total;
    FILE *csv;
    char csv_path[SR_PERF_PATH_MAX];
    char json_path[SR_PERF_PATH_MAX];
    int aot_depth;
    int interp_depth;
    int aot_seen;
    uint32_t last_aot_pc;
    uint64_t aot_start_ns;
    uint64_t interp_start_ns;
    SrPerfInterpReason pending_interp_reason;
    int sched_state_valid;
    SrPerfSchedState sched_state;
    uint32_t sched_uid;
    uint64_t sched_start_ns;
    SrPerfTransitionEntry transitions[2][SR_PERF_TOP_N];
} SrPerfState;

static SrPerfState s_perf;
int sr_perf_aot_active;
int sr_perf_enabled;
/* SR_PERF prints 1 Hz telemetry; SR_HUD or the F1 overlay only collect counters for
 * the in-game HUD, so the stderr lines are gated separately. */
static int s_perf_stderr_enabled = 0;
static double s_hud_fps = 0.0;
static double s_hud_frame_ms = 0.0;
static double s_hud_vblank_hz = 0.0;

static int env_on(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static uint64_t raw_now_ns(void) {
#if defined(_WIN32) || defined(_WIN64)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0 && !QueryPerformanceFrequency(&frequency))
        return (uint64_t)clock() * 1000000000ull / (uint64_t)CLOCKS_PER_SEC;
    if (!QueryPerformanceCounter(&counter))
        return (uint64_t)clock() * 1000000000ull / (uint64_t)CLOCKS_PER_SEC;
    return (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000000000ull +
           (uint64_t)((counter.QuadPart % frequency.QuadPart) * 1000000000ll /
                      frequency.QuadPart);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return 0;
#else
    return (uint64_t)clock() * 1000000000ull / (uint64_t)CLOCKS_PER_SEC;
#endif
}

static double ms(uint64_t ns) {
    return (double)ns / 1000000.0;
}

static uint64_t elapsed_ns(uint64_t started_ns) {
    if (!started_ns) return 0;
    uint64_t now = raw_now_ns();
    return now > started_ns ? now - started_ns : 0;
}

static void checkpoint_timers(uint64_t now) {
    if (s_perf.guest_active && now > s_perf.guest_start_ns) {
        ADD(guest_ns, now - s_perf.guest_start_ns);
        s_perf.guest_start_ns = now;
    }
    if (s_perf.current_tier == SR_PERF_TIER_AOT && s_perf.aot_start_ns &&
        now > s_perf.aot_start_ns) {
        ADD(aot_ns, now - s_perf.aot_start_ns);
        s_perf.aot_start_ns = now;
    }
    if (s_perf.current_tier == SR_PERF_TIER_INTERP && s_perf.interp_start_ns &&
        now > s_perf.interp_start_ns) {
        ADD(interp_ns, now - s_perf.interp_start_ns);
        s_perf.interp_start_ns = now;
    }
    if (s_perf.sched_state_valid && s_perf.sched_start_ns && now > s_perf.sched_start_ns &&
        (unsigned)s_perf.sched_state < SR_PERF_SCHED_STATE_COUNT) {
        ADD_ARRAY(sched_ns, (unsigned)s_perf.sched_state, now - s_perf.sched_start_ns);
        s_perf.sched_start_ns = now;
    }
}

static void stop_timers(uint64_t now) {
    if (s_perf.guest_active && now > s_perf.guest_start_ns)
        ADD(guest_ns, now - s_perf.guest_start_ns);
    if (s_perf.current_tier == SR_PERF_TIER_AOT && s_perf.aot_start_ns &&
        now > s_perf.aot_start_ns)
        ADD(aot_ns, now - s_perf.aot_start_ns);
    if (s_perf.current_tier == SR_PERF_TIER_INTERP && s_perf.interp_start_ns &&
        now > s_perf.interp_start_ns)
        ADD(interp_ns, now - s_perf.interp_start_ns);
    if (s_perf.sched_state_valid && s_perf.sched_start_ns && now > s_perf.sched_start_ns &&
        (unsigned)s_perf.sched_state < SR_PERF_SCHED_STATE_COUNT)
        ADD_ARRAY(sched_ns, (unsigned)s_perf.sched_state, now - s_perf.sched_start_ns);
    s_perf.guest_active = 0;
    s_perf.guest_start_ns = 0;
    s_perf.aot_start_ns = 0;
    s_perf.interp_start_ns = 0;
    s_perf.sched_state_valid = 0;
    s_perf.sched_start_ns = 0;
    s_perf.current_tier = SR_PERF_TIER_NONE;
    sr_perf_aot_active = 0;
}

static const char *interp_reason_name(uint32_t reason) {
    switch (reason) {
    case SR_PERF_INTERP_DISPATCH_MISS: return "dispatch-miss";
    case SR_PERF_INTERP_STALE_BLOCK: return "stale-block";
    case SR_PERF_INTERP_AOT_HANDOFF: return "aot-handoff";
    case SR_PERF_INTERP_CALL_RETURN: return "call-return";
    default: return "unknown";
    }
}

static void record_transition(unsigned direction, uint32_t pc, uint32_t reason) {
    if (direction > 1u || (unsigned)reason >= SR_PERF_INTERP_REASON_COUNT) return;
    SrPerfTransitionEntry *table = s_perf.transitions[direction];
    for (unsigned i = 0; i < SR_PERF_TOP_N; i++) {
        if (table[i].count && table[i].pc == pc && table[i].reason == reason) {
            table[i].count++;
            return;
        }
    }
    for (unsigned i = 0; i < SR_PERF_TOP_N; i++) {
        if (!table[i].count) {
            table[i].pc = pc;
            table[i].reason = reason;
            table[i].count = 1;
            return;
        }
    }
    unsigned lowest = 0;
    for (unsigned i = 1; i < SR_PERF_TOP_N; i++)
        if (table[i].count < table[lowest].count) lowest = i;
    table[lowest].pc = pc;
    table[lowest].reason = reason;
    table[lowest].count = 1;
}

static SrPerfVfpuFamily vfpu_family(uint32_t opcode) {
    switch ((opcode >> 26) & 0x3fu) {
    case 0x32u:
    case 0x35u:
    case 0x36u:
        return SR_PERF_VFPU_LOAD;
    case 0x3au:
    case 0x3du:
    case 0x3eu:
        return SR_PERF_VFPU_STORE;
    case 0x34u:
        return SR_PERF_VFPU_ARITHMETIC;
    case 0x12u:
    case 0x37u:
        return SR_PERF_VFPU_PREFIX;
    default:
        return SR_PERF_VFPU_OTHER;
    }
}

static void add_vfpu_family(SrPerfVfpuFamily family) {
    if ((unsigned)family < SR_PERF_VFPU_FAMILY_COUNT)
        ADD_ARRAY(vfpu_family, (unsigned)family, 1);
}

static void switch_to_aot(uint64_t now) {
    if (s_perf.current_tier == SR_PERF_TIER_AOT) return;
    if (s_perf.current_tier == SR_PERF_TIER_INTERP) {
        if (s_perf.interp_start_ns && now > s_perf.interp_start_ns)
            ADD(interp_ns, now - s_perf.interp_start_ns);
        s_perf.interp_start_ns = 0;
    }
    s_perf.current_tier = SR_PERF_TIER_AOT;
    s_perf.aot_start_ns = now;
    sr_perf_aot_active = 1;
}

static void switch_to_interp(uint32_t pc, SrPerfInterpReason reason, uint64_t now,
                              int record) {
    if (s_perf.current_tier == SR_PERF_TIER_INTERP) return;
    if (s_perf.current_tier == SR_PERF_TIER_AOT) {
        if (s_perf.aot_start_ns && now > s_perf.aot_start_ns)
            ADD(aot_ns, now - s_perf.aot_start_ns);
        s_perf.aot_start_ns = 0;
        if (record) {
            record_transition(0u, pc, (uint32_t)reason);
            ADD(aot_to_interp, 1);
        }
    }
    s_perf.current_tier = SR_PERF_TIER_INTERP;
    s_perf.interp_start_ns = now;
    sr_perf_aot_active = 0;
}

static void csv_header(FILE *csv) {
    fputs("vblank_total,wall_ms,fps,frame_ms,vblank_hz,cpu_ms,ge_wait_ms,present_ms,idle_ms,"
          "submits,ge_submits,present_submits,waits,readback_waits,present_skips,present_wait_ms,target30,"
          "ge_submit_render,ge_submit_snapshot,ge_submit_texup,ge_submit_targetup,ge_submit_depthup,ge_submit_depthread,ge_submit_targetread,ge_submit_xfer,ge_submit_init,ge_submit_mixed,"
          "ge_wait_render,ge_wait_snapshot,ge_wait_texup,ge_wait_targetup,ge_wait_depthup,ge_wait_depthread,ge_wait_targetread,ge_wait_xfer,ge_wait_init,ge_wait_mixed,"
          "ge_wait_render_ms,ge_wait_snapshot_ms,ge_wait_texup_ms,ge_wait_targetup_ms,ge_wait_depthup_ms,ge_wait_depthread_ms,ge_wait_targetread_ms,ge_wait_xfer_ms,ge_wait_init_ms,ge_wait_mixed_ms,"
          "shblend_states,shblend_draws,shblend_batches,shblend_fb16,shblend_dither,shblend_absdiff,shblend_double_dst_alpha,shblend_double_src_alpha_dst,shblend_dual_fix,"
          "snapshot_requests,snapshot_cache_hits,snapshot_copies,"
          "aot_ms,aot_calls,aot_instructions,interp_ms,interp_calls,interp_instructions,aot_to_interp,interp_to_aot,"
          "vfpu_ms,vfpu_count,vfpu_interp_count,vfpu_aot_count,vfpu_errors,vfpu_interp_ms,vfpu_aot_ms,"
          "vfpu_load_count,vfpu_store_count,vfpu_arithmetic_count,vfpu_prefix_count,vfpu_other_count,"
          "sched_running_ms,sched_runnable_ms,sched_blocked_ms,sched_idle_ms,context_switches,"
          "ge_cpu_ms,ge_cpu_calls,ge_transform_sample_ms,ge_primitive_ms,"
          "vulkan_submit_ms,vulkan_submits,vulkan_wait_ms,vulkan_waits,vulkan_readback_ms,vulkan_readbacks,vulkan_readback_bytes,vulkan_pipeline_ms,vulkan_pipeline_creations,"
          "texture_decode_ms,texture_decodes,texture_decode_bytes,texture_cache_hits,texture_cache_misses,"
          "iso_read_ms,iso_reads,iso_read_bytes,iso_failures,vfs_read_ms,vfs_reads,vfs_read_bytes,vfs_failures,"
          "h264_decode_ms,h264_decodes,h264_failures,atrac_decode_ms,atrac_decodes,atrac_failures,"
          "audio_mix_ms,audio_mix_calls,audio_output_ms,audio_output_calls,audio_output_frames\n", csv);
}

static void csv_row(FILE *csv, const SrPerfMetrics *m, uint64_t total_vblanks,
                    uint64_t wall_ns) {
    double seconds = (double)wall_ns / 1000000000.0;
    double fps = seconds > 0.0 ? (double)m->presents / seconds : 0.0;
    double vblank_hz = seconds > 0.0 ? (double)m->vblanks / seconds : 0.0;
    double frame_ms = m->presents ? ms(wall_ns) / (double)m->presents : 0.0;
    uint64_t scheduler_ns = wall_ns > m->guest_ns ? wall_ns - m->guest_ns : 0;
    uint64_t idle_ns = scheduler_ns + m->guest_idle_ns;
    uint64_t excluded_ns = m->ge_wait_ns + m->present_ns + m->guest_idle_ns;
    uint64_t cpu_ns = m->guest_ns > excluded_ns ? m->guest_ns - excluded_ns : 0;
    fprintf(csv, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%s",
            (unsigned long long)total_vblanks, ms(wall_ns), fps, frame_ms, vblank_hz,
            ms(cpu_ns), ms(m->ge_wait_ns), ms(m->present_ns), ms(idle_ns),
            (unsigned long long)(m->ge_submits + m->present_submits),
            (unsigned long long)m->ge_submits, (unsigned long long)m->present_submits,
            (unsigned long long)(m->ge_waits + m->present_waits),
            (unsigned long long)m->readback_waits, (unsigned long long)m->present_skips,
            ms(m->present_wait_ns), fps >= 29.5 ? "yes" : "no");
    for (unsigned i = 0; i < SR_PERF_GE_REASON_COUNT; i++)
        fprintf(csv, ",%llu", (unsigned long long)m->ge_reason_submits[i]);
    for (unsigned i = 0; i < SR_PERF_GE_REASON_COUNT; i++)
        fprintf(csv, ",%llu", (unsigned long long)m->ge_reason_waits[i]);
    for (unsigned i = 0; i < SR_PERF_GE_REASON_COUNT; i++)
        fprintf(csv, ",%.3f", ms(m->ge_reason_wait_ns[i]));
    for (unsigned i = 0; i < SR_PERF_GE_EVENT_COUNT; i++)
        fprintf(csv, ",%llu", (unsigned long long)m->ge_events[i]);
    fprintf(csv, ",%.3f,%llu,%llu,%.3f,%llu,%llu,%llu,%llu",
            ms(m->aot_ns), (unsigned long long)m->aot_calls,
            (unsigned long long)m->aot_instructions, ms(m->interp_ns),
            (unsigned long long)m->interp_calls,
            (unsigned long long)m->interp_instructions,
            (unsigned long long)m->aot_to_interp,
            (unsigned long long)m->interp_to_aot);
    fprintf(csv, ",%.3f,%llu,%llu,%llu,%llu,%.3f,%.3f",
            ms(m->vfpu_ns), (unsigned long long)m->vfpu_count,
            (unsigned long long)m->vfpu_interp_count,
            (unsigned long long)m->vfpu_aot_count, (unsigned long long)m->vfpu_errors,
            ms(m->vfpu_interp_ns), ms(m->vfpu_aot_ns));
    for (unsigned i = 0; i < SR_PERF_VFPU_FAMILY_COUNT; i++)
        fprintf(csv, ",%llu", (unsigned long long)m->vfpu_family[i]);
    fprintf(csv, ",%.3f,%.3f,%.3f,%.3f,%llu",
            ms(m->sched_ns[SR_PERF_SCHED_RUNNING]),
            ms(m->sched_ns[SR_PERF_SCHED_RUNNABLE]),
            ms(m->sched_ns[SR_PERF_SCHED_BLOCKED]),
            ms(m->sched_ns[SR_PERF_SCHED_IDLE]),
            (unsigned long long)m->sched_switches);
    fprintf(csv, ",%.3f,%llu,%.3f,%.3f",
            ms(m->ge_cpu_ns), (unsigned long long)m->ge_cpu_calls,
            ms(m->ge_transform_sample_ns), ms(m->ge_primitive_ns));
    fprintf(csv, ",%.3f,%llu,%.3f,%llu,%.3f,%llu,%llu,%.3f,%llu",
            ms(m->vk_submit_ns), (unsigned long long)m->vk_submits,
            ms(m->vk_wait_ns), (unsigned long long)m->vk_waits,
            ms(m->vk_readback_ns), (unsigned long long)m->vk_readbacks,
            (unsigned long long)m->vk_readback_bytes, ms(m->vk_pipeline_ns),
            (unsigned long long)m->vk_pipeline_creations);
    fprintf(csv, ",%.3f,%llu,%llu,%llu,%llu",
            ms(m->texture_decode_ns), (unsigned long long)m->texture_decodes,
            (unsigned long long)m->texture_decode_bytes,
            (unsigned long long)m->texture_cache_hits,
            (unsigned long long)m->texture_cache_misses);
    for (unsigned i = 0; i < SR_PERF_STORAGE_SOURCE_COUNT; i++)
        fprintf(csv, ",%.3f,%llu,%llu,%llu", ms(m->storage_ns[i]),
                (unsigned long long)m->storage_reads[i],
                (unsigned long long)m->storage_bytes[i],
                (unsigned long long)m->storage_failures[i]);
    fprintf(csv, ",%.3f,%llu,%llu,%.3f,%llu,%llu",
            ms(m->h264_ns), (unsigned long long)m->h264_calls,
            (unsigned long long)m->h264_failures, ms(m->atrac_ns),
            (unsigned long long)m->atrac_calls, (unsigned long long)m->atrac_failures);
    fprintf(csv, ",%.3f,%llu,%.3f,%llu,%llu\n", ms(m->audio_mix_ns),
            (unsigned long long)m->audio_mix_calls, ms(m->audio_output_ns),
            (unsigned long long)m->audio_output_calls,
            (unsigned long long)m->audio_output_frames);
}

static int transition_compare(const void *left, const void *right) {
    const SrPerfTransitionEntry *a = (const SrPerfTransitionEntry *)left;
    const SrPerfTransitionEntry *b = (const SrPerfTransitionEntry *)right;
    if (a->count < b->count) return 1;
    if (a->count > b->count) return -1;
    if (a->pc < b->pc) return -1;
    if (a->pc > b->pc) return 1;
    if (a->reason < b->reason) return -1;
    if (a->reason > b->reason) return 1;
    return 0;
}

static void json_top_pcs(FILE *json, unsigned direction) {
    SrPerfTransitionEntry entries[SR_PERF_TOP_N];
    unsigned count = 0;
    for (unsigned i = 0; i < SR_PERF_TOP_N; i++)
        if (s_perf.transitions[direction][i].count)
            entries[count++] = s_perf.transitions[direction][i];
    qsort(entries, count, sizeof(entries[0]), transition_compare);
    fputc('[', json);
    for (unsigned i = 0; i < count; i++) {
        if (i) fputc(',', json);
        fprintf(json, "{\"pc\":\"0x%08x\",\"reason\":\"%s\",\"count\":%llu}",
                entries[i].pc, interp_reason_name(entries[i].reason),
                (unsigned long long)entries[i].count);
    }
    fputc(']', json);
}

static void write_summary(const char *path, uint64_t wall_ns) {
    FILE *json = fopen(path, "w");
    if (!json) {
        fprintf(stderr, "PERF: cannot open JSON summary output\n");
        return;
    }
    const SrPerfMetrics *m = &s_perf.total;
    fprintf(json, "{\n");
    fprintf(json, "  \"schema\":\"nakagawa-perf-v1\",\n");
    fprintf(json, "  \"build\":{\"aot_instruction_hook\":%s},\n",
            PERF_AOT_INSTRUCTIONS ? "true" : "false");
    fprintf(json, "  \"wall_ns\":%llu,\n", (unsigned long long)wall_ns);
    fprintf(json, "  \"interval_count\":%llu,\n", (unsigned long long)s_perf.interval_count);
    fprintf(json, "  \"vblanks\":{\"count\":%llu,\"presents\":%llu,\"present_skips\":%llu},\n",
            (unsigned long long)m->vblanks, (unsigned long long)m->presents,
            (unsigned long long)m->present_skips);
    fprintf(json, "  \"guest\":{\"ns\":%llu,\"idle_ns\":%llu,\"aot_ns\":%llu,\"aot_calls\":%llu,\"aot_instruction_count\":",
            (unsigned long long)m->guest_ns, (unsigned long long)m->guest_idle_ns,
            (unsigned long long)m->aot_ns, (unsigned long long)m->aot_calls);
    if (PERF_AOT_INSTRUCTIONS)
        fprintf(json, "%llu", (unsigned long long)m->aot_instructions);
    else
        fputs("null", json);
    fprintf(json, ",\"interpreter_ns\":%llu,\"interpreter_calls\":%llu,\"interpreter_instructions\":%llu},\n",
            (unsigned long long)m->interp_ns, (unsigned long long)m->interp_calls,
            (unsigned long long)m->interp_instructions);
    fprintf(json, "  \"transitions\":{\"aot_to_interpreter_count\":%llu,\"interpreter_to_aot_count\":%llu,\"top_pcs\":{\"aot_to_interpreter\":",
            (unsigned long long)m->aot_to_interp, (unsigned long long)m->interp_to_aot);
    json_top_pcs(json, 0);
    fputs(",\"interpreter_to_aot\":", json);
    json_top_pcs(json, 1);
    fputs("}},\n", json);
    fprintf(json, "  \"scheduler\":{\"running_ns\":%llu,\"runnable_ns\":%llu,\"blocked_ns\":%llu,\"idle_ns\":%llu,\"context_switches\":%llu,\"blocked_transitions\":%llu,\"wake_transitions\":%llu},\n",
            (unsigned long long)m->sched_ns[SR_PERF_SCHED_RUNNING],
            (unsigned long long)m->sched_ns[SR_PERF_SCHED_RUNNABLE],
            (unsigned long long)m->sched_ns[SR_PERF_SCHED_BLOCKED],
            (unsigned long long)m->sched_ns[SR_PERF_SCHED_IDLE],
            (unsigned long long)m->sched_switches, (unsigned long long)m->sched_blocked,
            (unsigned long long)m->sched_wakes);
    fprintf(json, "  \"vfpu\":{\"ns\":%llu,\"count\":%llu,\"interpreter_count\":%llu,\"aot_instruction_count\":%llu,\"interpreter_ns\":%llu,\"aot_ns\":%llu,\"errors\":%llu,\"families\":{\"load\":%llu,\"store\":%llu,\"arithmetic\":%llu,\"prefix\":%llu,\"other\":%llu}},\n",
            (unsigned long long)m->vfpu_ns, (unsigned long long)m->vfpu_count,
            (unsigned long long)m->vfpu_interp_count,
            (unsigned long long)m->vfpu_aot_count,
            (unsigned long long)m->vfpu_interp_ns, (unsigned long long)m->vfpu_aot_ns,
            (unsigned long long)m->vfpu_errors,
            (unsigned long long)m->vfpu_family[SR_PERF_VFPU_LOAD],
            (unsigned long long)m->vfpu_family[SR_PERF_VFPU_STORE],
            (unsigned long long)m->vfpu_family[SR_PERF_VFPU_ARITHMETIC],
            (unsigned long long)m->vfpu_family[SR_PERF_VFPU_PREFIX],
            (unsigned long long)m->vfpu_family[SR_PERF_VFPU_OTHER]);
    fprintf(json, "  \"ge\":{\"cpu_ns\":%llu,\"cpu_calls\":%llu,\"transform_sample_ns\":%llu,\"primitive_ns\":%llu,\"submits\":%llu,\"waits\":%llu,\"wait_ns\":%llu,\"present_submits\":%llu,\"present_waits\":%llu,\"present_wait_ns\":%llu},\n",
            (unsigned long long)m->ge_cpu_ns, (unsigned long long)m->ge_cpu_calls,
            (unsigned long long)m->ge_transform_sample_ns,
            (unsigned long long)m->ge_primitive_ns, (unsigned long long)m->ge_submits,
            (unsigned long long)m->ge_waits, (unsigned long long)m->ge_wait_ns,
            (unsigned long long)m->present_submits,
            (unsigned long long)m->present_waits,
            (unsigned long long)m->present_wait_ns);
    fprintf(json, "  \"vulkan\":{\"submit_ns\":%llu,\"submits\":%llu,\"wait_ns\":%llu,\"waits\":%llu,\"readback_ns\":%llu,\"readbacks\":%llu,\"readback_bytes\":%llu,\"pipeline_creation_ns\":%llu,\"pipeline_creations\":%llu},\n",
            (unsigned long long)m->vk_submit_ns, (unsigned long long)m->vk_submits,
            (unsigned long long)m->vk_wait_ns, (unsigned long long)m->vk_waits,
            (unsigned long long)m->vk_readback_ns,
            (unsigned long long)m->vk_readbacks,
            (unsigned long long)m->vk_readback_bytes,
            (unsigned long long)m->vk_pipeline_ns,
            (unsigned long long)m->vk_pipeline_creations);
    fprintf(json, "  \"textures\":{\"decode_ns\":%llu,\"decodes\":%llu,\"decoded_bytes\":%llu,\"cache_hits\":%llu,\"cache_misses\":%llu},\n",
            (unsigned long long)m->texture_decode_ns,
            (unsigned long long)m->texture_decodes,
            (unsigned long long)m->texture_decode_bytes,
            (unsigned long long)m->texture_cache_hits,
            (unsigned long long)m->texture_cache_misses);
    fprintf(json, "  \"storage\":{\"iso_read_ns\":%llu,\"iso_reads\":%llu,\"iso_read_bytes\":%llu,\"iso_failures\":%llu,\"vfs_read_ns\":%llu,\"vfs_reads\":%llu,\"vfs_read_bytes\":%llu,\"vfs_failures\":%llu},\n",
            (unsigned long long)m->storage_ns[SR_PERF_STORAGE_ISO],
            (unsigned long long)m->storage_reads[SR_PERF_STORAGE_ISO],
            (unsigned long long)m->storage_bytes[SR_PERF_STORAGE_ISO],
            (unsigned long long)m->storage_failures[SR_PERF_STORAGE_ISO],
            (unsigned long long)m->storage_ns[SR_PERF_STORAGE_VFS],
            (unsigned long long)m->storage_reads[SR_PERF_STORAGE_VFS],
            (unsigned long long)m->storage_bytes[SR_PERF_STORAGE_VFS],
            (unsigned long long)m->storage_failures[SR_PERF_STORAGE_VFS]);
    fprintf(json, "  \"media\":{\"h264_decode_ns\":%llu,\"h264_decodes\":%llu,\"h264_failures\":%llu,\"atrac_decode_ns\":%llu,\"atrac_decodes\":%llu,\"atrac_failures\":%llu},\n",
            (unsigned long long)m->h264_ns, (unsigned long long)m->h264_calls,
            (unsigned long long)m->h264_failures, (unsigned long long)m->atrac_ns,
            (unsigned long long)m->atrac_calls, (unsigned long long)m->atrac_failures);
    fprintf(json, "  \"audio\":{\"mix_ns\":%llu,\"mix_calls\":%llu,\"output_ns\":%llu,\"output_calls\":%llu,\"output_frames\":%llu},\n",
            (unsigned long long)m->audio_mix_ns, (unsigned long long)m->audio_mix_calls,
            (unsigned long long)m->audio_output_ns,
            (unsigned long long)m->audio_output_calls,
            (unsigned long long)m->audio_output_frames);
    fputs("  \"boundaries\":[", json);
    fputs("{\"metric\":\"aot_instruction_count\",\"status\":", json);
    fputs(PERF_AOT_INSTRUCTIONS ? "\"compiled\"" : "\"not_compiled\"", json);
    fputs("},{\"metric\":\"aot_vfpu_time\",\"status\":\"partial\"},{\"metric\":\"ge_primitive_phase\",\"status\":\"sampled\"}]\n", json);
    fputs("}\n", json);
    fclose(json);
}

static void report_if_due(uint64_t now) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    if (!s_perf.interval_start_ns) {
        s_perf.interval_start_ns = now;
        return;
    }
    if (now <= s_perf.interval_start_ns || now - s_perf.interval_start_ns < 1000000000ull)
        return;
    checkpoint_timers(now);
    uint64_t wall_ns = now - s_perf.interval_start_ns;
    SrPerfMetrics m = s_perf.interval;
    uint64_t scheduler_ns = wall_ns > m.guest_ns ? wall_ns - m.guest_ns : 0;
    uint64_t idle_ns = scheduler_ns + m.guest_idle_ns;
    uint64_t excluded_ns = m.ge_wait_ns + m.present_ns + m.guest_idle_ns;
    uint64_t cpu_ns = m.guest_ns > excluded_ns ? m.guest_ns - excluded_ns : 0;
    uint64_t submits = m.ge_submits + m.present_submits;
    uint64_t waits = m.ge_waits + m.present_waits;
    double seconds = (double)wall_ns / 1000000000.0;
    double fps = seconds > 0.0 ? (double)m.presents / seconds : 0.0;
    double vblank_hz = seconds > 0.0 ? (double)m.vblanks / seconds : 0.0;
    double frame_ms = m.presents ? ms(wall_ns) / (double)m.presents : 0.0;
    s_hud_fps = fps;
    s_hud_frame_ms = frame_ms;
    s_hud_vblank_hz = vblank_hz;
    if (s_perf_stderr_enabled) {
    fprintf(stderr,
            "PERF vblank_total=%llu wall_ms=%.3f fps=%.3f frame_ms=%.3f vblank_hz=%.3f "
            "cpu_ms=%.3f ge_wait_ms=%.3f present_ms=%.3f idle_ms=%.3f "
            "submits=%llu ge_submits=%llu present_submits=%llu waits=%llu "
            "readback_waits=%llu present_skips=%llu present_wait_ms=%.3f target30=%s\n",
            (unsigned long long)s_perf.total_vblanks, ms(wall_ns), fps, frame_ms,
            vblank_hz, ms(cpu_ns), ms(m.ge_wait_ns), ms(m.present_ns), ms(idle_ns),
            (unsigned long long)submits, (unsigned long long)m.ge_submits,
            (unsigned long long)m.present_submits, (unsigned long long)waits,
            (unsigned long long)m.readback_waits,
            (unsigned long long)m.present_skips, ms(m.present_wait_ns),
            fps >= 29.5 ? "yes" : "no");
    fprintf(stderr,
            "PERF_ATTRIB aot_ms=%.3f aot_calls=%llu aot_instructions=%llu interp_ms=%.3f interp_calls=%llu interp_instructions=%llu aot_to_interp=%llu interp_to_aot=%llu vfpu_ms=%.3f ge_cpu_ms=%.3f vk_submit_ms=%.3f vk_wait_ms=%.3f texture_decode_ms=%.3f iso_read_ms=%.3f vfs_read_ms=%.3f h264_ms=%.3f atrac_ms=%.3f mix_ms=%.3f output_ms=%.3f\n",
            ms(m.aot_ns), (unsigned long long)m.aot_calls,
            (unsigned long long)m.aot_instructions, ms(m.interp_ns),
            (unsigned long long)m.interp_calls,
            (unsigned long long)m.interp_instructions,
            (unsigned long long)m.aot_to_interp,
            (unsigned long long)m.interp_to_aot, ms(m.vfpu_ns), ms(m.ge_cpu_ns),
            ms(m.vk_submit_ns), ms(m.vk_wait_ns), ms(m.texture_decode_ns),
            ms(m.storage_ns[SR_PERF_STORAGE_ISO]),
            ms(m.storage_ns[SR_PERF_STORAGE_VFS]), ms(m.h264_ns), ms(m.atrac_ns),
            ms(m.audio_mix_ns), ms(m.audio_output_ns));
    }
    if (s_perf.csv) {
        csv_row(s_perf.csv, &m, s_perf.total_vblanks, wall_ns);
        fflush(s_perf.csv);
    }
    fflush(stderr);
    memset(&s_perf.interval, 0, sizeof(s_perf.interval));
    s_perf.interval_start_ns = now;
    s_perf.interval_count++;
}

static void make_summary_path(const char *csv_path, char *out, size_t out_size) {
    const char *slash = strrchr(csv_path, '/');
    const char *dot = strrchr(csv_path, '.');
    size_t length = strlen(csv_path);
    if (length + sizeof(".json") > out_size) {
        if (out_size) out[0] = '\0';
        return;
    }
    if (dot && (!slash || dot > slash)) {
        size_t stem = (size_t)(dot - csv_path);
        memcpy(out, csv_path, stem);
        memcpy(out + stem, ".json", sizeof(".json"));
        return;
    }
    memcpy(out, csv_path, length);
    memcpy(out + length, ".json", sizeof(".json"));
}

void sr_perf_shutdown(void) {
    if (!s_perf.enabled || !s_perf.initialized || s_perf.shutdown) return;
    s_perf.shutdown = 1;
    sr_perf_enabled = 0;
    sr_perf_phase_report(1);
    uint64_t now = raw_now_ns();
    stop_timers(now);
    uint64_t wall_ns = s_perf.run_start_ns && now > s_perf.run_start_ns
                          ? now - s_perf.run_start_ns : 0;
    if (s_perf.json_path[0]) write_summary(s_perf.json_path, wall_ns);
    if (s_perf.csv) {
        uint64_t final_wall_ns = s_perf.interval_start_ns && now > s_perf.interval_start_ns
                                     ? now - s_perf.interval_start_ns : 0;
        if (final_wall_ns)
            csv_row(s_perf.csv, &s_perf.interval, s_perf.total_vblanks, final_wall_ns);
        fflush(s_perf.csv);
        fclose(s_perf.csv);
        s_perf.csv = NULL;
    }
}

static void perf_exit(void) {
    sr_perf_shutdown();
}

void sr_perf_init(void) {
    if (s_perf.initialized) return;
    memset(&s_perf, 0, sizeof(s_perf));
    s_perf.initialized = 1;
    s_perf_stderr_enabled = env_on("SR_PERF");
    s_perf.enabled = s_perf_stderr_enabled || env_on("SR_HUD");
    sr_perf_enabled = s_perf.enabled;
    sr_perf_aot_active = 0;
    if (!s_perf.enabled) return;
    s_perf.run_start_ns = raw_now_ns();
    s_perf.interval_start_ns = s_perf.run_start_ns;
    const char *csv_path = getenv("SR_PERF_CSV");
    if (csv_path && csv_path[0]) {
        snprintf(s_perf.csv_path, sizeof(s_perf.csv_path), "%s", csv_path);
        make_summary_path(s_perf.csv_path, s_perf.json_path, sizeof(s_perf.json_path));
        s_perf.csv = fopen(s_perf.csv_path, "w");
        if (s_perf.csv) {
            csv_header(s_perf.csv);
            fflush(s_perf.csv);
        } else {
            fprintf(stderr, "PERF: cannot open CSV output\n");
        }
    }
    const char *json_path = getenv("SR_PERF_JSON");
    if (json_path && json_path[0])
        snprintf(s_perf.json_path, sizeof(s_perf.json_path), "%s", json_path);
    atexit(perf_exit);
    fprintf(stderr, "PERF enabled: 1 Hz aggregate telemetry%s%s\n",
            s_perf.csv ? " + CSV" : "",
            s_perf.json_path[0] ? " + JSON summary" : "");
}

uint64_t sr_perf_now_ns_impl(void) {
    return s_perf.enabled && !s_perf.shutdown ? raw_now_ns() : 0;
}

void sr_perf_guest_begin(void) {
    if (!s_perf.enabled || s_perf.shutdown || s_perf.guest_active) return;
    s_perf.guest_active = 1;
    s_perf.guest_start_ns = raw_now_ns();
}

void sr_perf_guest_end(void) {
    if (!s_perf.enabled || s_perf.shutdown || !s_perf.guest_active) return;
    uint64_t now = raw_now_ns();
    if (now > s_perf.guest_start_ns) ADD(guest_ns, now - s_perf.guest_start_ns);
    s_perf.guest_active = 0;
    s_perf.guest_start_ns = 0;
    report_if_due(now);
}

void sr_perf_guest_idle_wait(uint64_t started_ns) {
    if (s_perf.enabled && !s_perf.shutdown && s_perf.guest_active && started_ns)
        ADD(guest_idle_ns, elapsed_ns(started_ns));
}

void sr_perf_vblank(void) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    ADD(vblanks, 1);
    s_perf.total_vblanks++;
    report_if_due(raw_now_ns());
}

/* ---- display-source service cadence ------------------------------------------------
 * The display source counts a period per elapsed rational deadline, but delivery
 * happens only at an eligible service point and the pending bit coalesces. A latch
 * whose host-time gap since the previous latch exceeds one display period is
 * therefore a period the guest could not be told about, and attributing it to the
 * phase that was executing when the period came due is what turns "the rate is
 * low" into "this construct holds the CPU".  Counted here, not derived afterwards:
 * after the fact the phase is unknowable. */
#define SR_PERF_DISPLAY_PERIOD_US 16683u   /* 60000/1001 Hz */
#define SR_PERF_PHASE_SAMPLE_N 4u          /* guest-PC/NID samples kept per phase */

typedef struct SrPerfPhaseAttrib {
    uint64_t latches;            /* latches whose gap fit inside one period */
    uint64_t late;               /* latches whose gap exceeded one period */
    uint64_t lost_us;            /* sum of (gap - one period) over those latches */
    uint64_t max_gap_us;         /* worst single gap seen in this phase */
    uint64_t periods_lost;       /* whole source periods the coalesced bit swallowed */
    uint32_t pc[SR_PERF_PHASE_SAMPLE_N];    /* interrupted guest PCs, power-of-two sampled */
    uint32_t nid[SR_PERF_PHASE_SAMPLE_N];   /* NIDs in progress, power-of-two sampled */
    uint32_t uid[SR_PERF_PHASE_SAMPLE_N];   /* running thread uid at the latch */
    uint64_t sample_mask;        /* 2^n - 1: keep every 2^n-th late latch */
} SrPerfPhaseAttrib;

static SrPerfPhaseAttrib s_phase_attrib[SR_RT_PHASE_COUNT];
static uint64_t s_late_total;
static uint64_t s_periods_masked;   /* display periods that elapsed with IE clear */

const char *const sr_rt_phase_name[SR_RT_PHASE_COUNT] = {
    "other", "aot", "interp", "syscall", "ge", "host_wait", "sched", "present",
};

/* Read by the attribution above and written at the cheap phase seams; a plain
 * global so a phase change costs one store and no call. */
int sr_rt_phase = SR_RT_PHASE_OTHER;
uint32_t sr_rt_nid = 0u;
/* Installed by the scheduler, which owns the live CpuState; NULL in a build with
 * no scheduler, where there is no guest PC to report. */
uint32_t (*sr_rt_pc_fn)(void) = NULL;
uint32_t (*sr_rt_uid_fn)(void) = NULL;

void sr_perf_phase_report(int force) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    uint64_t now = raw_now_ns();
    if (!force && now - s_perf.attrib_report_ns < 60000000000ull) return;
    s_perf.attrib_report_ns = now;
    uint64_t total_late = 0, total_lost = 0;
    for (int i = 0; i < SR_RT_PHASE_COUNT; i++) {
        total_late += s_phase_attrib[i].late;
        total_lost += s_phase_attrib[i].lost_us;
    }
    fprintf(stderr, "PERF_ATTRIB vblank_late total=%llu lost_ms=%llu masked_periods=%llu\n",
            (unsigned long long)total_late, (unsigned long long)(total_lost / 1000u),
            (unsigned long long)s_periods_masked);
    for (int i = 0; i < SR_RT_PHASE_COUNT; i++) {
        const SrPerfPhaseAttrib *a = &s_phase_attrib[i];
        if (!a->late) continue;
        fprintf(stderr, "  PERF_ATTRIB_LATE phase=%s n=%llu lost_ms=%llu max_gap_ms=%llu periods=%llu",
                sr_rt_phase_name[i], (unsigned long long)a->late,
                (unsigned long long)(a->lost_us / 1000u),
                (unsigned long long)(a->max_gap_us / 1000u),
                (unsigned long long)a->periods_lost);
        for (unsigned k = 0; k < SR_PERF_PHASE_SAMPLE_N; k++) {
            if (!a->pc[k] && !a->nid[k] && !a->uid[k]) continue;
            fprintf(stderr, " [uid=0x%x pc=0x%08x%s]",
                    a->uid[k], a->pc[k], a->nid[k] ? " nid" : "");
            if (a->nid[k]) fprintf(stderr, "=0x%08x", a->nid[k]);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

void sr_perf_vblank_latch(uint64_t gap_us, uint32_t periods, int masked) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    if (masked) {                 /* the guest's own interrupt mask, not a lost edge */
        s_periods_masked += periods;
        return;
    }
    unsigned phase = (unsigned)sr_rt_phase;
    if (phase >= SR_RT_PHASE_COUNT) phase = SR_RT_PHASE_OTHER;
    SrPerfPhaseAttrib *a = &s_phase_attrib[phase];
    a->latches++;
    if (gap_us <= SR_PERF_DISPLAY_PERIOD_US) return;
    uint64_t lost = gap_us - SR_PERF_DISPLAY_PERIOD_US;
    a->late++;
    a->lost_us += lost;
    if (periods > 1u) a->periods_lost += periods - 1u;
    if (gap_us > a->max_gap_us) a->max_gap_us = gap_us;
    /* Guest PCs and NIDs are functional facts, not private bytes: keep a
     * power-of-two sample so a long run stays readable and bounded. */
    a->sample_mask++;
    if ((a->sample_mask & (a->sample_mask - 1u)) == 0u) {
        unsigned slot = (unsigned)(a->sample_mask >> 1) % SR_PERF_PHASE_SAMPLE_N;
        a->pc[slot] = sr_rt_pc_fn ? sr_rt_pc_fn() : 0u;
        a->uid[slot] = sr_rt_uid_fn ? sr_rt_uid_fn() : 0u;
        a->nid[slot] = (phase == (unsigned)SR_RT_PHASE_SYSCALL) ? sr_rt_nid : 0u;
    }
    s_late_total++;
    sr_perf_phase_report(0);
}

void sr_perf_ge_submit(SrPerfGeReason reason) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    ADD(ge_submits, 1);
    if ((unsigned)reason < SR_PERF_GE_REASON_COUNT)
        ADD_ARRAY(ge_reason_submits, (unsigned)reason, 1);
}

void sr_perf_ge_wait(uint64_t started_ns, SrPerfGeReason reason) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    sr_rt_phase = SR_RT_PHASE_GE;   /* a GE completion wait, not host sleep */
    uint64_t elapsed = elapsed_ns(started_ns);
    ADD(ge_wait_ns, elapsed);
    ADD(ge_waits, 1);
    if ((unsigned)reason < SR_PERF_GE_REASON_COUNT) {
        ADD_ARRAY(ge_reason_waits, (unsigned)reason, 1);
        ADD_ARRAY(ge_reason_wait_ns, (unsigned)reason, elapsed);
    }
    if (reason == SR_PERF_GE_DEPTH_READBACK ||
        reason == SR_PERF_GE_TARGET_READBACK_TRANSITION)
        ADD(readback_waits, 1);
    sr_rt_phase = SR_RT_PHASE_OTHER;
}

void sr_perf_ge_event(SrPerfGeEvent event, uint64_t count) {
    if (s_perf.enabled && !s_perf.shutdown && (unsigned)event < SR_PERF_GE_EVENT_COUNT)
        ADD_ARRAY(ge_events, (unsigned)event, count);
}

void sr_perf_present_submit(void) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    sr_rt_phase = SR_RT_PHASE_PRESENT;
    ADD(present_submits, 1);
}

void sr_perf_present_wait(uint64_t started_ns) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    ADD(present_wait_ns, elapsed_ns(started_ns));
    ADD(present_waits, 1);
}

void sr_perf_present_done(uint64_t started_ns, int result) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    if (started_ns) ADD(present_ns, elapsed_ns(started_ns));
    if (result == 1) ADD(presents, 1);
    sr_rt_phase = SR_RT_PHASE_OTHER;
}

void sr_perf_present_skip(void) {
    if (s_perf.enabled && !s_perf.shutdown) ADD(present_skips, 1);
}

void sr_perf_aot_begin(uint32_t pc) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    uint64_t now = raw_now_ns();
    s_perf.last_aot_pc = pc;
    if (s_perf.interp_depth)
        s_perf.aot_seen = 1;
    if (s_perf.current_tier != SR_PERF_TIER_AOT)
        switch_to_aot(now);
    /* The tag names the INNERMOST active tier, so it is re-asserted on every
     * entry rather than only on the outermost one: a late latch is attributed to
     * the code actually running, not to whatever context last ran a scheduler. */
    sr_rt_phase = SR_RT_PHASE_AOT;
    s_perf.aot_depth++;
    ADD(aot_calls, 1);
}

void sr_perf_aot_end(void) {
    if (!s_perf.enabled || s_perf.shutdown || !s_perf.aot_depth) return;
    uint64_t now = raw_now_ns();
    s_perf.aot_depth--;
    if (s_perf.aot_depth) {
        sr_rt_phase = SR_RT_PHASE_AOT;
        return;
    }
    if (s_perf.current_tier == SR_PERF_TIER_AOT && s_perf.aot_start_ns &&
        now > s_perf.aot_start_ns)
        ADD(aot_ns, now - s_perf.aot_start_ns);
    s_perf.aot_start_ns = 0;
    if (s_perf.interp_depth) {
        s_perf.current_tier = SR_PERF_TIER_INTERP;
        s_perf.interp_start_ns = now;
        sr_perf_aot_active = 0;
        sr_rt_phase = SR_RT_PHASE_INTERP;
    } else {
        s_perf.current_tier = SR_PERF_TIER_NONE;
        sr_perf_aot_active = 0;
        sr_rt_phase = SR_RT_PHASE_OTHER;
    }
}

void sr_perf_aot_instruction(uint32_t pc, uint32_t opcode) {
    if (!s_perf.enabled || s_perf.shutdown ||
        s_perf.current_tier != SR_PERF_TIER_AOT)
        return;
    (void)pc;
    (void)opcode;
    ADD(aot_instructions, 1);
}

void sr_perf_interp_set_reason(SrPerfInterpReason reason) {
    if (s_perf.enabled && !s_perf.shutdown &&
        (unsigned)reason < SR_PERF_INTERP_REASON_COUNT)
        s_perf.pending_interp_reason = reason;
}

void sr_perf_interp_begin(uint32_t entry_pc) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    uint64_t now = raw_now_ns();
    if (!s_perf.interp_depth) {
        s_perf.aot_seen = 0;
        switch_to_interp(entry_pc, s_perf.pending_interp_reason, now, 1);
    }
    sr_rt_phase = SR_RT_PHASE_INTERP;
    s_perf.interp_depth++;
    ADD(interp_calls, 1);
    s_perf.pending_interp_reason = SR_PERF_INTERP_DISPATCH_MISS;
}

void sr_perf_interp_instruction(void) {
    if (s_perf.enabled && !s_perf.shutdown &&
        s_perf.current_tier == SR_PERF_TIER_INTERP)
        ADD(interp_instructions, 1);
}

void sr_perf_interp_end(uint32_t exit_pc, int result) {
    if (!s_perf.enabled || s_perf.shutdown || !s_perf.interp_depth) return;
    uint64_t now = raw_now_ns();
    s_perf.interp_depth--;
    if (s_perf.interp_depth) return;
    if (s_perf.current_tier == SR_PERF_TIER_INTERP && s_perf.interp_start_ns &&
        now > s_perf.interp_start_ns)
        ADD(interp_ns, now - s_perf.interp_start_ns);
    s_perf.interp_start_ns = 0;
    int returned_to_aot = s_perf.aot_depth || s_perf.aot_seen;
    if (returned_to_aot) {
        uint32_t pc = s_perf.aot_seen ? s_perf.last_aot_pc : exit_pc;
        SrPerfInterpReason reason = result == SR_PERF_RESULT_CALL_RETURN
                                        ? SR_PERF_INTERP_CALL_RETURN
                                        : SR_PERF_INTERP_AOT_HANDOFF;
        record_transition(1u, pc, (uint32_t)reason);
        ADD(interp_to_aot, 1);
        s_perf.aot_seen = 0;
    }
    if (s_perf.aot_depth) {
        s_perf.current_tier = SR_PERF_TIER_AOT;
        s_perf.aot_start_ns = now;
        sr_perf_aot_active = 1;
        sr_rt_phase = SR_RT_PHASE_AOT;
    } else {
        s_perf.current_tier = SR_PERF_TIER_NONE;
        sr_perf_aot_active = 0;
        sr_rt_phase = SR_RT_PHASE_OTHER;
    }
}

void sr_perf_vfpu(uint32_t opcode, uint64_t started_ns, int result) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    int aot = s_perf.current_tier == SR_PERF_TIER_AOT;
    ADD(vfpu_count, 1);
    if (aot) ADD(vfpu_aot_count, 1);
    else ADD(vfpu_interp_count, 1);
    if (result == 0) ADD(vfpu_errors, 1);
    add_vfpu_family(vfpu_family(opcode));
    if (started_ns) {
        uint64_t elapsed = elapsed_ns(started_ns);
        ADD(vfpu_ns, elapsed);
        if (aot) ADD(vfpu_aot_ns, elapsed);
        else ADD(vfpu_interp_ns, elapsed);
    }
}

void sr_perf_sched_state(SrPerfSchedState state, uint32_t uid) {
    if (!s_perf.enabled || s_perf.shutdown ||
        (unsigned)state >= SR_PERF_SCHED_STATE_COUNT)
        return;
    uint64_t now = raw_now_ns();
    if (s_perf.sched_state_valid && s_perf.sched_state == state &&
        s_perf.sched_uid == uid)
        return;
    if (s_perf.sched_state_valid && s_perf.sched_start_ns && now > s_perf.sched_start_ns)
        ADD_ARRAY(sched_ns, (unsigned)s_perf.sched_state, now - s_perf.sched_start_ns);
    if (state == SR_PERF_SCHED_BLOCKED) ADD(sched_blocked, 1);
    if (s_perf.sched_state_valid && s_perf.sched_state == SR_PERF_SCHED_BLOCKED &&
        state != SR_PERF_SCHED_BLOCKED)
        ADD(sched_wakes, 1);
    s_perf.sched_state = state;
    s_perf.sched_uid = uid;
    s_perf.sched_start_ns = now;
    s_perf.sched_state_valid = 1;
}

void sr_perf_sched_switch(uint32_t from_uid, uint32_t to_uid) {
    (void)from_uid;
    (void)to_uid;
    if (s_perf.enabled && !s_perf.shutdown) ADD(sched_switches, 1);
}

void sr_perf_ge_cpu(uint64_t started_ns) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    ADD(ge_cpu_ns, elapsed_ns(started_ns));
    ADD(ge_cpu_calls, 1);
}

void sr_perf_ge_cpu_phase(uint64_t transform_ns, uint64_t raster_ns) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    ADD(ge_transform_sample_ns, transform_ns);
    ADD(ge_primitive_ns, raster_ns);
}

void sr_perf_vulkan_submit(uint64_t started_ns) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    ADD(vk_submit_ns, elapsed_ns(started_ns));
    ADD(vk_submits, 1);
}

void sr_perf_vulkan_wait(uint64_t started_ns, int readback) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    uint64_t elapsed = elapsed_ns(started_ns);
    ADD(vk_wait_ns, elapsed);
    ADD(vk_waits, 1);
    if (readback) ADD(vk_readback_ns, elapsed);
}

void sr_perf_vulkan_pipeline(uint64_t started_ns) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    ADD(vk_pipeline_ns, elapsed_ns(started_ns));
    ADD(vk_pipeline_creations, 1);
}

void sr_perf_vulkan_readback(uint32_t bytes) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    ADD(vk_readbacks, 1);
    ADD(vk_readback_bytes, bytes);
}

void sr_perf_texture_cache(int hit) {
    if (!s_perf.enabled || s_perf.shutdown) return;
    if (hit) ADD(texture_cache_hits, 1);
    else ADD(texture_cache_misses, 1);
}

void sr_perf_texture_decode(uint64_t started_ns, uint32_t bytes) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    ADD(texture_decode_ns, elapsed_ns(started_ns));
    ADD(texture_decodes, 1);
    ADD(texture_decode_bytes, bytes);
}

void sr_perf_storage_read(SrPerfStorageSource source, uint32_t bytes,
                          uint64_t started_ns, int success) {
    if (!s_perf.enabled || s_perf.shutdown ||
        (unsigned)source >= SR_PERF_STORAGE_SOURCE_COUNT || !started_ns)
        return;
    ADD_ARRAY(storage_ns, (unsigned)source, elapsed_ns(started_ns));
    ADD_ARRAY(storage_reads, (unsigned)source, 1);
    ADD_ARRAY(storage_bytes, (unsigned)source, bytes);
    if (!success) ADD_ARRAY(storage_failures, (unsigned)source, 1);
}

void sr_perf_h264(uint64_t started_ns, int result) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    ADD(h264_ns, elapsed_ns(started_ns));
    ADD(h264_calls, 1);
    if (result < 0) ADD(h264_failures, 1);
}

void sr_perf_atrac(uint64_t started_ns, int result) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    ADD(atrac_ns, elapsed_ns(started_ns));
    ADD(atrac_calls, 1);
    if (result < 0) ADD(atrac_failures, 1);
}

void sr_perf_audio_mix(uint64_t started_ns) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    ADD(audio_mix_ns, elapsed_ns(started_ns));
    ADD(audio_mix_calls, 1);
}

void sr_perf_audio_output(uint64_t started_ns, uint32_t frames) {
    if (!s_perf.enabled || s_perf.shutdown || !started_ns) return;
    ADD(audio_output_ns, elapsed_ns(started_ns));
    ADD(audio_output_calls, 1);
    ADD(audio_output_frames, frames);
}

void sr_perf_enable_counters(void) {
    if (!s_perf.initialized) {
        s_perf.initialized = 1;
        s_perf.run_start_ns = raw_now_ns();
    }
    s_perf.enabled = 1;
    sr_perf_enabled = 1;
    if (s_perf.interval_start_ns == 0) s_perf.interval_start_ns = raw_now_ns();
}

void sr_perf_get_hud_metrics(double *out_fps, double *out_frame_ms, double *out_vblank_hz) {
    double fps = 0.0, frame_ms = 0.0, vblank_hz = 0.0;
    if (s_hud_fps > 0.0) {
        fps = s_hud_fps;
        frame_ms = s_hud_frame_ms;
        vblank_hz = s_hud_vblank_hz;
    } else {
        /* Before the first 1 Hz interval closes, estimate from the open interval. */
        uint64_t now = raw_now_ns();
        uint64_t elapsed = now > s_perf.interval_start_ns ? now - s_perf.interval_start_ns : 0;
        double seconds = (double)elapsed / 1000000000.0;
        if (s_perf.interval_start_ns && seconds > 0.1) {
            fps = (double)s_perf.interval.presents / seconds;
            vblank_hz = (double)s_perf.interval.vblanks / seconds;
            frame_ms = s_perf.interval.presents
                           ? ms(elapsed) / (double)s_perf.interval.presents : 0.0;
        }
    }
    if (out_fps) *out_fps = fps;
    if (out_frame_ms) *out_frame_ms = frame_ms;
    if (out_vblank_hz) *out_vblank_hz = vblank_hz;
}
