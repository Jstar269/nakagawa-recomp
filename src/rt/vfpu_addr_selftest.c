// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Dumps the PRODUCTION VFPU vector-register addressing decode over its entire
 * finite domain, so a source-owned test can compare it against real-PSP
 * measurements and against the recompiler's independent Python implementation.
 *
 * This includes vfpu_interp.c directly and calls its real vreg_idx(); it does
 * not restate the algorithm, so a divergence between this output and
 * tools/codegen.py vreg_indices() is a genuine two-implementation disagreement
 * rather than a test that agrees with itself.
 *
 * Output, one line per (width, encoding), 4 * 128 = 512 lines:
 *
 *     w<width> e<2-hex-encoding> n<lanes> <idx> <idx> ...
 *
 * Built and run by tools/test_vfpu_addressing.py. No game inputs required:
 *
 *     gcc -std=c11 -Isrc/rt -o vfpu_addr_selftest src/rt/vfpu_addr_selftest.c
 */

#include "vfpu_interp.c"

#include <stdio.h>

/* Standalone support required by recomp.h and the rest of vfpu_interp.c. None
 * of it is reachable from vreg_idx(); it exists only so the translation unit
 * links without dragging in the runtime. Same pattern as
 * src/rt/gpu_coherence_selftest.c. No game inputs or private data required. */
uint8_t *g_mem;
CpuState *s_cpu;
int g_hle_depth;
int g_sr_heap_watch;
int g_sr_metadata_watch;
int g_sr_mem_watch_count;
int g_sr_last_writer_enabled;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
unsigned g_sr_mem_watch_context_count;
unsigned g_sr_mem_watch_context_limit;
uint32_t g_sr_mem_watch_context_pc;
int g_sr_mem_watch_context_fpr;
uint32_t g_sr_mem_watch_context_fpr_value;
unsigned g_sr_store_context_count;
unsigned g_sr_store_context_limit;
uint32_t g_sr_store_context_pc;
int g_sr_store_context_mem_gpr;
uint32_t g_sr_store_context_mem_offset;
unsigned g_sr_store_context_mem_words;

void sr_oor(uint32_t addr, uint32_t value, int store) { (void)addr; (void)value; (void)store; }
void sr_note_mem_write(uint32_t a, uint32_t w, uint32_t v, uint32_t pc) {
    (void)a; (void)w; (void)v; (void)pc;
}
void sr_heap_note_write(uint32_t a, uint32_t w, uint32_t v, uint32_t pc) {
    (void)a; (void)w; (void)v; (void)pc;
}
uint32_t sched_current_uid(void) { return 0u; }
uint32_t sr_get_ge_status(void) { return 0u; }
/* The LLE CPU gate is off here, so the VFPU alignment guard (#258) never runs;
 * these keep the translation unit linking without src/rt/cpu_lle.c. */
int sr_cpu_lle_enabled(void) { return 0; }
unsigned sr_cpu_data_access_fault(const CpuState *s, uint32_t address, unsigned width, int is_store) {
    (void)s; (void)address; (void)width; (void)is_store; return 0u;
}
int sr_cpu_raise_data_fault(CpuState *s, unsigned exception_code, uint32_t address, uint32_t instr_pc) {
    (void)s; (void)exception_code; (void)address; (void)instr_pc; return 0;
}
void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix) {
    (void)r; (void)s; (void)idx; (void)n; (void)prefix;
}
void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t dprefix) {
    (void)s; (void)idx; (void)d; (void)n; (void)dprefix;
}
float sr_vfpu_sin(float x) { return x; }
float sr_vfpu_cos(float x) { return x; }
float sr_vfpu_asin(float x) { return x; }
float sr_vfpu_exp2(float x) { return x; }
float sr_vfpu_log2(float x) { return x; }
float sr_vfpu_rcp(float x) { return x; }
float sr_vfpu_rsqrt(float x) { return x; }
float sr_vfpu_sqrt(float x) { return x; }

int main(void) {
    for (int width = 1; width <= 4; width++) {
        for (int enc = 0; enc < 128; enc++) {
            uint8_t idx[4] = {0, 0, 0, 0};
            int lanes = vreg_idx(enc, width, idx);
            printf("w%d e%02x n%d", width, enc, lanes);
            for (int i = 0; i < lanes; i++) printf(" %d", (int)idx[i]);
            printf("\n");
        }
    }
    return 0;
}
