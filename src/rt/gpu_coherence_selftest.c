// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#define _POSIX_C_SOURCE 200809L   /* setenv under -std=c11 */

#include "ace.h"
#include "gpu_sdl3vk/ge_gpu.h"
#include "gpu_sdl3vk/sdl3vk.h"
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* Standalone support required by recomp.h and the production GE object. The test owns a
 * synthetic guest arena and never consumes game binaries, captures, or private inputs. */
uint8_t *g_mem;
uint32_t g_sr_debug;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count;
int g_sr_metadata_watch;
int g_sr_heap_watch;
int g_hle_depth;
CpuState *s_cpu;
int g_sr_last_writer_enabled;
uint32_t g_sr_store_context_pc;
unsigned g_sr_store_context_count;
unsigned g_sr_store_context_limit;
uint32_t g_sr_mem_watch_context_pc;
unsigned g_sr_mem_watch_context_limit;
unsigned g_sr_mem_watch_context_count;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value;
int g_sr_store_context_mem_gpr;
uint32_t g_sr_store_context_mem_offset;
unsigned g_sr_store_context_mem_words;

void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
void sr_add_mem_watch(uint32_t start, uint32_t end, const char *label) {
    (void)start; (void)end; (void)label;
}
void sr_add_value_watch(uint32_t value, const char *label) {
    (void)value; (void)label;
}
void sr_debug_init_watches(void) {}
void sr_last_writer_reset(void) {}
int sr_find_last_writer(uint32_t addr, uint32_t width,
                        uint32_t *write_addr, uint32_t *write_width,
                        uint32_t *value, uint32_t *pc) {
    (void)addr; (void)width; (void)write_addr; (void)write_width;
    (void)value; (void)pc;
    return 0;
}

void sr_oor(uint32_t addr, uint32_t value, int store) {
    fprintf(stderr, "gpu coherence selftest: out-of-range %s addr=0x%08x value=0x%08x\n",
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

/* Host-mapped buffer memory selection (#1296), pure and device-free so it runs where Vulkan
 * does not. The table is an RTX 3080's: type 3 is visible+coherent but uncached, type 4 adds
 * HOST_CACHED. A readback must take type 4; an upload keeps the first visible+coherent type;
 * a readback with no cached+coherent type keeps the old choice. */
static int host_mem_type_selection_ok(void) {
    const uint32_t vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint32_t rtx3080[6] = {
        0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vis,
        vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | vis,
    };
    const uint32_t cached_noncoherent[3] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, vis, 0u,
    };
    int ok = 1;
    ok &= gegpu_pick_host_mem_type(rtx3080, 6u, 0x3fu, 1) == 4u;
    ok &= gegpu_pick_host_mem_type(rtx3080, 6u, 0x3fu, 0) == 3u;
    ok &= gegpu_pick_host_mem_type(rtx3080, 6u, 0x2fu, 1) == 3u;   /* type 4 not allowed */
    ok &= gegpu_pick_host_mem_type(rtx3080, 6u, 0x07u, 1) == UINT32_MAX;
    ok &= gegpu_pick_host_mem_type(cached_noncoherent, 3u, 0x7u, 1) == 1u;
    if (!ok) fprintf(stderr, "gpu coherence selftest: FAIL host buffer memory type selection\n");
    return ok;
}

static int set_env(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

/* `--scale N` and `--case NAME` set SR_GPU_SCALE and SR_GPU_COHERENCE_CASE before the backend
 * starts. Recipes pass arguments, not a `VAR=value` prefix, which cmd.exe does not accept. */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *name = strcmp(argv[i], "--scale") == 0 ? "SR_GPU_SCALE"
                         : strcmp(argv[i], "--case") == 0  ? "SR_GPU_COHERENCE_CASE" : NULL;
        if (!name || i + 1 >= argc || !set_env(name, argv[++i])) {
            fprintf(stderr, "usage: gpu_coherence_selftest [--scale N] [--case scaled-upload]\n");
            return 2;
        }
    }
    if (!host_mem_type_selection_ok()) return 1;
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;
    sr_perf_init();
#ifdef SR_GPU_ACE_SELFTEST
    /* Ace Mode with 16x texture filtering, read before the device is made (src/rt/ace.h). */
    if (!ace_init_spec("v1;af=16;text=off;readout=off;notice=off;wide=off;smooth=off", NULL)) {
        fprintf(stderr, "gpu ace selftest: FAIL the Ace line was refused\n");
        free(arena);
        return 1;
    }
#endif

    if (!sdl3vk_init("Nakagawa GPU coherence selftest") || !gegpu_init()) {
        fprintf(stderr, "gpu coherence selftest: SKIP (Vulkan initialization unavailable)\n");
        sdl3vk_shutdown();
        free(arena);
        return 77;
    }

    int ok;
#if defined(SR_GPU_ACE_SELFTEST)
    ok = gegpu_ace_af_selftest();
    if (ok == 77) {
        gegpu_shutdown();
        sdl3vk_shutdown();
        free(arena);
        return 77;
    }
#elif defined(SR_GPU_SNAPSHOT_SYNC_SELFTEST)
    ok = gegpu_snapshot_sync_selftest();
#else
    ok = gegpu_coherence_selftest();
#endif
#ifndef SR_GPU_ACE_SELFTEST
    /* Ace Mode off (A2, src/rt/ace.h): every cell above ran with no anisotropic sampler,
     * and the texture pool kept its MAX_TEX sets. */
    {
        GeGpuAceStats ace;
        gegpu_ace_stats(&ace);
        if (ace.af != 0 || ace.af_samplers != 0u || ace.tex_pool_sets == 0u) {
            fprintf(stderr, "gpu coherence selftest: FAIL ace-off af=%d af_samplers=%llu pool=%u\n",
                    ace.af, ace.af_samplers, ace.tex_pool_sets);
            ok = 0;
        } else {
            printf("gpu coherence selftest: PASS ace-off af=0 af_samplers=0 pool=%u\n",
                   ace.tex_pool_sets);
        }
    }
#endif
    gegpu_shutdown();
    sdl3vk_shutdown();
    free(arena);
    if (!ok) return 1;
#if defined(SR_GPU_ACE_SELFTEST)
    puts("gpu ace selftest: OK");
#elif defined(SR_GPU_SNAPSHOT_SYNC_SELFTEST)
    puts("gpu snapshot sync selftest: OK");
#else
    puts("gpu coherence selftest: OK");
#endif
    return 0;
}
