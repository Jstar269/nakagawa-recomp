// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "gpu_sdl3vk/ge_gpu.h"
#include "gpu_sdl3vk/sdl3vk.h"
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>

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

int main(void) {
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;
    sr_perf_init();

    if (!sdl3vk_init("Nakagawa GPU coherence selftest") || !gegpu_init()) {
        fprintf(stderr, "gpu coherence selftest: SKIP (Vulkan initialization unavailable)\n");
        sdl3vk_shutdown();
        free(arena);
        return 77;
    }

    int ok;
#ifdef SR_GPU_SNAPSHOT_SYNC_SELFTEST
    ok = gegpu_snapshot_sync_selftest();
#else
    ok = gegpu_coherence_selftest();
#endif
    gegpu_shutdown();
    sdl3vk_shutdown();
    free(arena);
    if (!ok) return 1;
#ifdef SR_GPU_SNAPSHOT_SYNC_SELFTEST
    puts("gpu snapshot sync selftest: OK");
#else
    puts("gpu coherence selftest: OK");
#endif
    return 0;
}
