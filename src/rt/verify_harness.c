// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Verify-build harness (issue #1843): runs calls into one module's generated code with the
 * sampled in-play checker (src/rt/verify.c) armed, the way the player would, and leaves the
 * verdicts to the report (SR_VERIFY_REPORT) and the captures (SR_VERIFY_CAPTURE).
 *
 *   verify_harness [--call ADDR:COUNT[:A0[:A1[:A2]]]]... [--sched-slice N] [--frozen-clock]
 *                  <image.bin> <base-hex>
 *
 * Each --call enters the translated function at ADDR (hex) COUNT times (decimal) through
 * production dispatch as a host entry, with the arguments in hex (default: the data block
 * below, 0, 0) and $sp at the top of a stack region. --sched-slice N turns the scheduler
 * flag on with a slice of N yield points; each time the slice expires guest code reaches sr_yield,
 * which here only re-arms the slice, so a call that yields is reported SKIP preempted.
 * --frozen-clock fixes the verifier's clock at zero, to test the rate budget.
 *
 * The harness links the generated module, recomp.c and the dispatch and interpreter units
 * but no scheduler, HLE, GE or host platform. The definitions below stand in for those, as
 * in src/rt/snapshot_runner.c: none can change a compared result silently, and the two
 * calls that would leave guest code, sr_syscall and sr_raw_syscall, report the stop to the
 * verifier and return.
 */

#include "recomp.h"
#include "nested_frames.h"
#include "verify.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sr_register_all(void);   /* generated: executable spans and every native body */

volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_commit(uint32_t addr, uint32_t width) { (void)addr; (void)width; }

uint32_t g_sr_debug = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
int g_sr_metadata_watch = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_count = 0;
unsigned g_sr_store_context_limit = 0;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;
int g_sr_last_writer_enabled = 0;
CpuState *s_cpu = NULL;
int sr_sched_on = 0;
atomic_int_least32_t sr_timeslice;

uint32_t sched_current_uid(void) { return 0u; }
void sched_exit_current(int32_t status) { (void)status; }
void sched_exit_current_delete(int32_t status) { (void)status; }
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) { (void)uid; (void)arglen; (void)argp; return 0; }
uint32_t sched_terminate_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_delete_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_thread_wakeup(uint32_t uid) { (void)uid; return 0; }
void sched_set_current_join_target(uint32_t uid) { (void)uid; }
void sched_clear_current_join_target(void) {}
int sched_take_current_join_result(uint32_t uid, uint32_t *result_out) { (void)uid; (void)result_out; return 0; }
uint32_t sr_get_ge_status(void) { return 0u; }
uint32_t sr_hle_resolve_late_import(uint32_t nid) { (void)nid; return 0u; }
void sr_main_module_register(const SrMainModuleRecord *record) { (void)record; }
/* recomp.c reads the SDL clock for diagnostics only; the Makefile builds this harness's
 * copy of recomp.c with SDL_GetTicksNS renamed to this. */
uint64_t snapshot_runner_ticks_ns(void) { return 0u; }
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    (void)addr; (void)width; (void)val; (void)pc;
}

static unsigned s_yields;
static unsigned long s_slice;

/* A scheduler slice expired inside guest code. The real sr_yield may switch guest threads;
 * here it only counts and re-arms the slice. The verifier was told by SR_YIELD. */
void sr_yield(CpuState *s) {
    (void)s;
    s_yields++;
    atomic_store(&sr_timeslice, (int_least32_t)s_slice);
}

uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    sr_verify_at_stop(s, SR_VERIFY_STOP_IMPORT, nid, 0u);
    return 0u;
}

void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {
    sr_verify_at_stop(s, SR_VERIFY_STOP_SYSCALL, code, pc);
}

static uint64_t frozen_clock(void) { return 0u; }

/* With the scheduler flag on, dispatch measures the host stack. The harness runs on the
 * process's own stack, which is far larger than any call it makes. */
static int main_stack_remaining(size_t *remaining) {
    *remaining = (size_t)1 << 20;
    return 1;
}

#define DATA_ADDR 0x09000000u
#define STACK_TOP 0x09100000u
#define MAX_CALLS 64u

typedef struct {
    uint32_t addr, count, arg[3];
} CallSpec;

static int parse_hex(const char *text, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &end, 16);
    if (errno || !end || end == text || *end != '\0' || v > UINT32_MAX) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int parse_call(const char *text, CallSpec *c) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", text);
    c->arg[0] = DATA_ADDR;
    c->arg[1] = 0u;
    c->arg[2] = 0u;
    char *field[5] = { buf, NULL, NULL, NULL, NULL };
    int n = 1;
    for (char *p = buf; *p && n < 5; p++) {
        if (*p == ':') { *p = '\0'; field[n++] = p + 1; }
    }
    if (n < 2 || !parse_hex(field[0], &c->addr)) return 0;
    char *end = NULL;
    unsigned long long count = strtoull(field[1], &end, 10);
    if (!end || end == field[1] || *end != '\0' || count == 0u || count > UINT32_MAX) return 0;
    c->count = (uint32_t)count;
    for (int i = 2; i < n; i++)
        if (!parse_hex(field[i], &c->arg[i - 2])) return 0;
    return 1;
}

static int usage(void) {
    fprintf(stderr,
            "usage: verify_harness [--call ADDR:COUNT[:A0[:A1[:A2]]]]... [--sched-slice N]\n"
            "                      [--frozen-clock] <image.bin> <base-hex>\n");
    return 2;
}

static uint8_t *read_image(const char *path, uint32_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *data = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n < 0 || (unsigned long long)n > UINT32_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (uint8_t *)malloc(n ? (size_t)n : 1u);
    if (data && n && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len_out = (uint32_t)n;
    return data;
}

int main(int argc, char **argv) {
    CallSpec calls[MAX_CALLS];
    unsigned ncalls = 0;
    unsigned long slice = 0;
    int frozen = 0, positional = 0;
    const char *image_path = NULL;
    uint32_t base = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--call") && i + 1 < argc) {
            if (ncalls >= MAX_CALLS || !parse_call(argv[++i], &calls[ncalls])) return usage();
            ncalls++;
        } else if (!strcmp(a, "--sched-slice") && i + 1 < argc) {
            char *end = NULL;
            slice = strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0' || slice == 0ul) return usage();
        } else if (!strcmp(a, "--frozen-clock")) {
            frozen = 1;
        } else if (a[0] == '-' && a[1] == '-') {
            return usage();
        } else if (positional == 0) {
            image_path = a;
            positional++;
        } else if (positional == 1 && parse_hex(a, &base)) {
            positional++;
        } else {
            return usage();
        }
    }
    if (positional != 2) return usage();

    uint32_t image_len = 0;
    uint8_t *image = read_image(image_path, &image_len);
    if (!image) {
        fprintf(stderr, "verify_harness: cannot read %s\n", image_path);
        return 2;
    }
    sr_mem_init();
    sr_load_segment(base, image, image_len);
    free(image);
    sr_register_all();
    for (uint32_t i = 0; i < 0x40u; i++)
        g_mem[DATA_ADDR - SR_RAM_BASE + i] = (uint8_t)(i * 37u + 11u);

    SrVerifyConfig cfg;
    sr_verify_config_from_env(&cfg);
    sr_verify_configure(&cfg);
    if (frozen) sr_verify_set_clock(frozen_clock);
    if (slice) {
        s_slice = slice;
        sr_sched_on = 1;
        sr_dispatch_host_stack_remaining = main_stack_remaining;
        atomic_store(&sr_timeslice, (int_least32_t)slice);
    }

    static CpuState cpu;
    for (unsigned i = 0; i < ncalls; i++) {
        for (uint32_t n = 0; n < calls[i].count; n++) {
            memset(&cpu, 0, sizeof cpu);
            cpu.r[29] = STACK_TOP;
            cpu.r[31] = SR_HOST_RETURN_PC;
            cpu.r[4] = calls[i].arg[0];
            cpu.r[5] = calls[i].arg[1];
            cpu.r[6] = calls[i].arg[2];
            dispatch_call(&cpu, calls[i].addr, SR_HOST_RETURN_PC);
        }
    }
    fprintf(stderr, "verify_harness: %u call spec(s) done, %u yield(s)\n", ncalls, s_yields);
    return 0;
}
