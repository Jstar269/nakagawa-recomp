// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Executable production-HLE regression tests for ThreadMan behavior.
 *
 * Unlike the source-shape checks in tools/test_sched_invariants.py, this host
 * executable links the real hle.c, includes the real scheduler implementation,
 * and enters handlers through sr_syscall's registered-NID lookup. Only
 * unrelated host services are stubbed; the registry, handler, scheduler, and
 * coroutine transitions under test are production code.
 *
 * Coroutine lifecycle safety.
 *
 * This test used to be able to exhaust host RAM. A joiner body parked with
 * `for (;;) sr_coro_switch(sr_coro_main());`, and sr_coro_main() is a one-shot
 * initialisation operation: each call allocates a fresh wrapper and makes it the
 * current coroutine, so the following switch degenerated into a self-switch no-op
 * and the loop span forever, allocating every iteration. Two independent runs
 * reached roughly 21 GB and 25 GB before the host died.
 *
 * That defect is now caught by sr_coro.c's SR_CORO_LIFECYCLE_TEST instrumentation,
 * which counts adoptions, creates, destroys and switches inside the real
 * implementation and hard-caps adoptions and suppressed self-switches. A
 * reintroduced defect therefore aborts in milliseconds regardless of how the
 * offending call is spelled -- line splicing, a macro alias, an indirect alias or
 * a reordered guard all reach the same counted operation. The checks at the end of
 * main() assert the exact expected counters.
 *
 * Building this test without the instrumentation would silently remove that
 * protection, so it is a hard requirement rather than an option.
 */

#ifndef SR_CORO_LIFECYCLE_TEST
#error "hle_thread_selftest requires -DSR_CORO_LIFECYCLE_TEST: the coroutine lifecycle \
instrumentation is this test's protection against the historical RAM runaway."
#endif

#include "ge_shared.h"
#include "gpu_sdl3vk/ge_gpu.h"   /* GeGpuFbDescriptor: header-only, no Vulkan */
#include "sched.c" /* white-box fixture setup and observable TCB state */
#include "iso.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <process.h>

extern void sr_vblank_tick(void);
void sr_ctrl_sample(void);
int sr_route_sig_bytes(void);
int sr_route_test_sample(uint8_t *out);
void sr_display_test_reset(void);

/* Test-build-only white-box view of the no-frame watchdog state exported by
 * hle.c: the observation count and the vblanks-since-flip clock. */
extern void sr_watchdog_test_state(unsigned long *fires, uint32_t *vblanks_since_flip);

/* The selftest deliberately omits the renderer, so this stands in for the
 * production invalidation callback. It records what it was told rather than
 * discarding it: "a rejected DMA request must not dirty a GPU range" is a
 * DMA-path safety requirement, and the only way to assert a notification did
 * NOT happen is to be able to observe that it did. */
static unsigned long s_gpu_dirty_calls;
static uint32_t s_gpu_dirty_addr;
static uint32_t s_gpu_dirty_bytes;

static void selftest_vram_dirty_hook(uint32_t addr, uint32_t bytes) {
    s_gpu_dirty_calls++;
    s_gpu_dirty_addr = addr;
    s_gpu_dirty_bytes = bytes;
}

static const GeGpuHooks s_selftest_gpu_hooks = {
    .vram_dirty = selftest_vram_dirty_hook,
};

static void gpu_dirty_reset(void) {
    s_gpu_dirty_calls = 0;
    s_gpu_dirty_addr = 0;
    s_gpu_dirty_bytes = 0;
    ge_set_gpu_hooks(&s_selftest_gpu_hooks);
}

/* Test-build-only production IoFileMgr entry points and descriptor identity
 * probe exported by hle.c. */
extern uint32_t sr_hle_test_io_open(CpuState *s);
extern uint32_t sr_hle_test_io_read(CpuState *s);
extern uint32_t sr_hle_test_io_write(CpuState *s);
extern uint32_t sr_hle_test_io_lseek(CpuState *s);
extern uint32_t sr_hle_test_io_lseek32(CpuState *s);
extern uint32_t sr_hle_test_io_dopen(CpuState *s);
extern uint32_t sr_hle_test_io_dread(CpuState *s);
extern uint32_t sr_hle_test_io_dclose(CpuState *s);
extern uint32_t sr_hle_test_io_ioctl(CpuState *s);
extern uint32_t sr_hle_test_io_close(CpuState *s);
extern uint32_t sr_hle_test_io_open_async(CpuState *s);
extern uint32_t sr_hle_test_io_close_async(CpuState *s);
extern int sr_hle_test_fd_kind(uint32_t fd);
extern int sr_callback_is_valid(uint32_t uid);

/* Extracted-data census preparation contract hooks (defined in hle.c,
 * selftest-only): preparation state machine, walk/build counters, a guest-start
 * boundary marker, and a reset that restores the pristine UNINITIALIZED route.
 * See the prewarm tests below for the contracts these make observable. */
extern int sr_host_data_prepare(void);
extern size_t sr_host_data_entry_count(void);
extern void sr_hle_test_data_mark_guest_start(void);
extern unsigned long sr_hle_test_data_walk_calls(void);
extern unsigned long sr_hle_test_data_build_attempts(void);
extern unsigned long sr_hle_test_data_builds_after_guest(void);
extern int sr_hle_test_data_state(void);
extern size_t sr_hle_test_data_entry_count(void);
extern void sr_hle_test_data_reset(int pace_ms);

/* Issue #178 white-box message-pipe probes (defined in hle.c, selftest-only). */
typedef struct {
    uint32_t capacity, count, read_pos, write_pos;
} SrMsgPipeState;
extern int sr_hle_test_msgpipe_state(uint32_t uid, SrMsgPipeState *out);
extern uint32_t sr_hle_test_msgpipe_max_capacity(void);
/* Read-only view of a production Sync entry; see hle.c (issue #43). */
extern int sr_hle_test_sema_state(uint32_t uid, int *count_out, int *max_out);
/* Read-only view of the sceDisplaySetFrameBuf outcome accounting (defined in hle.c).
 * Any out-pointer may be NULL. */
extern void sr_display_test_flip_counts(unsigned long *calls, unsigned long *immediate,
                                        unsigned long *latched, unsigned long *rejected,
                                        uint32_t *last_err);
extern void sr_hle_test_sas_reset(void);
extern void sr_hle_test_audio_reset(void);
extern int sr_hle_test_audio_state(uint32_t ch, int *reserved,
                                   uint32_t *frames, int *format);

#define NID_SCE_KERNEL_EXIT_THREAD 0xaa73c935u
#define NID_SCE_KERNEL_SLEEP_THREAD 0x9ace131eu
#define NID_SCE_KERNEL_EXIT_DELETE_THREAD_ORACLE 0x809ce29bu
#define NID_SCE_KERNEL_GET_THREAD_ID 0x293b45b8u
#define NID_SCE_KERNEL_CREATE_MSG_PIPE 0x7c0dc2a0u
#define NID_SCE_KERNEL_DELETE_MSG_PIPE 0xf0b7da1cu
#define NID_SCE_KERNEL_TRY_SEND_MSG_PIPE 0x884c9f90u
#define NID_SCE_KERNEL_TRY_RECEIVE_MSG_PIPE 0xdf52098fu
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR 0x80000103u
#define NID_SCE_KERNEL_GET_SYSTEM_TIME_LOW 0x369ed59du
#define NID_SCE_KERNEL_GET_SYSTEM_TIME_WIDE 0x82bc5777u
#define NID_SCE_KERNEL_GET_SYSTEM_TIME 0xdb738f35u
#define NID_SCE_RTC_GET_CURRENT_TICK 0x3f7ad767u
#define NID_SCE_RTC_GET_CURRENT_CLOCK 0x4cfa57b0u
#define NID_SCE_RTC_GET_CURRENT_CLOCK_LOCAL 0xe7c27d1bu
#define NID_SCE_RTC_GET_TICK 0x6ff40accu
#define NID_SCE_RTC_SET_TICK 0x7ed29e40u
#define NID_SCE_RTC_GET_WIN32_FILETIME 0xcf561893u
#define NID_SCE_KERNEL_DELAY_THREAD 0xceadeb47u
#define NID_DISPLAY_FRAME_PER_SEC 0xdba6c4c4u
#define NID_SCE_KERNEL_LIBC_CLOCK 0x91e4f6a7u
#define NID_SCE_AUDIO_CH_RESERVE 0x5ec81c55u
#define NID_SCE_AUDIO_CH_RELEASE 0x6fc46853u
#define NID_SCE_AUDIO_OUTPUT_BLOCKING 0x136caf51u
#define NID_SCE_AUDIO_SET_DATA_LEN 0xcb2e439eu
#define SCE_AUDIO_ERROR_NOT_INITIALIZED 0x80260001u
#define SCE_AUDIO_ERROR_INVALID_CH 0x80260003u
#define SCE_AUDIO_ERROR_INVALID_SIZE 0x80260006u
#define SCE_AUDIO_ERROR_INVALID_FORMAT 0x80260007u

/* White-box fixture hook defined in hle.c under SR_HLE_THREAD_SELFTEST. */
extern void sr_hle_test_reset_rtc_epoch(void);
#define NID_SCE_KERNEL_LIBC_TIME 0x27cc57f0u
#define NID_SCE_KERNEL_LIBC_GETTIMEOFDAY 0x71ec4271u
#define NID_SCE_KERNEL_CPU_SUSPEND_INTR 0x092968f4u
#define NID_SCE_KERNEL_CPU_RESUME_INTR 0x5f10d406u
#define NID_SCE_KERNEL_CPU_RESUME_INTR_SYNC 0x3b84732du
#define NID_SCE_KERNEL_IS_CPU_INTR_SUSPENDED 0x47a0b729u
#define NID_SCE_KERNEL_IS_CPU_INTR_ENABLE 0xb55249d2u
#define NID_SCE_KERNEL_SUSPEND_DISPATCH_THREAD 0x3ad58b8cu
#define NID_SCE_KERNEL_RESUME_DISPATCH_THREAD  0x27e22ec2u
#define SCE_KERNEL_ERROR_MPP_FULL     0x800201b3u
#define SCE_KERNEL_ERROR_MPP_EMPTY    0x800201b4u
#define SCE_KERNEL_ERROR_ILLEGAL_SIZE 0x800201bcu
#define SCE_KERNEL_ERROR_UNKNOWN_MPPID 0x8002019eu
#define NID_SCE_ATRAC_RELEASE_ID 0x61eb33f5u
#define NID_SCE_ATRAC_SET_DATA 0x0e2a73abu
#define NID_SCE_ATRAC_SET_DATA_AND_GET_ID 0x7a20e7afu
#define NID_SCE_ATRAC_GET_ID 0x780f88d1u
#define NID_SCE_ATRAC_GET_SOUND_SAMPLE 0xa2bba8beu
#define NID_SCE_ATRAC_GET_STREAM_DATA_INFO 0x5d268707u
#define NID_SCE_ATRAC_GET_REMAIN_FRAME 0x9ae849a7u

#define ATRAC_CODEC_AT3PLUS 0x1000u
#define ATRAC_CODEC_AT3 0x1001u
#define ATRAC_ERROR_NO_ATRACID 0x80630003u
#define ATRAC_ERROR_INVALID_CODECTYPE 0x80630004u
#define ATRAC_ERROR_BAD_ATRACID 0x80630005u
#define ATRAC_ERROR_UNKNOWN_FORMAT 0x80630006u
#define ATRAC_ERROR_SIZE_TOO_SMALL 0x80630011u
#define NID_SCE_UTILITY_LOAD_MODULE 0x2a2b3de0u
#define NID_SCE_UTILITY_UNLOAD_MODULE 0xe49bfe92u
#define NID_SCE_UTILITY_LOAD_AV_MODULE 0xc629af26u
#define NID_SCE_UTILITY_UNLOAD_AV_MODULE 0xf7d8d092u
#define SCE_ERROR_MODULE_BAD_ID 0x80111101u
#define SCE_ERROR_MODULE_ALREADY_LOADED 0x80111102u
#define SCE_ERROR_MODULE_NOT_LOADED 0x80111103u
#define SCE_ERROR_AV_MODULE_BAD_ID 0x80110f01u
#define SCE_ERROR_AV_MODULE_ALREADY_LOADED 0x80110f02u
#define SCE_ERROR_AV_MODULE_NOT_LOADED 0x80110f03u
#define SCE_ERROR_AV_LIBRARY_NOT_FOUND 0x8002013cu

uint8_t *g_mem;
static uint8_t *g_mem_base;

uint32_t g_sr_debug;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count;
int g_sr_heap_watch;
int g_sr_metadata_watch;
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
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    (void)addr; (void)width; (void)val; (void)pc;
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
void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)addr; (void)width; (void)pc;
}
static unsigned long s_oor_calls;
void sr_oor(uint32_t addr, uint32_t value, int store) {
    (void)addr; (void)value; (void)store;
    s_oor_calls++;
}

static unsigned long s_audio_push_calls;
static unsigned long s_audio_queue_calls;
static int s_audio_push_frames;
static int16_t s_audio_push_first_l, s_audio_push_first_r;
static int16_t s_audio_push_last_l, s_audio_push_last_r;
static int s_audio_queue_result;
static int s_audio_queue_seq[4];
static int s_audio_queue_seq_len;

void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR) {
    (void)ch; (void)volL; (void)volR;
    s_audio_push_calls++;
    s_audio_push_frames = nframes;
    if (lr && nframes > 0) {
        s_audio_push_first_l = lr[0];
        s_audio_push_first_r = lr[1];
        s_audio_push_last_l = lr[(nframes - 1) * 2];
        s_audio_push_last_r = lr[(nframes - 1) * 2 + 1];
    }
}

int sr_audio_queued(int ch) {
    (void)ch;
    if (s_audio_queue_seq_len > 0) {
        int i = (int)s_audio_queue_calls;
        if (i >= s_audio_queue_seq_len) i = s_audio_queue_seq_len - 1;
        s_audio_queue_calls++;
        return s_audio_queue_seq[i];
    }
    s_audio_queue_calls++;
    return s_audio_queue_result;
}

static void audio_fixture_reset(void) {
    s_oor_calls = 0;
    s_audio_push_calls = 0;
    s_audio_queue_calls = 0;
    s_audio_push_frames = 0;
    s_audio_push_first_l = 0;
    s_audio_push_first_r = 0;
    s_audio_push_last_l = 0;
    s_audio_push_last_r = 0;
    s_audio_queue_result = 0;
    s_audio_queue_seq_len = 0;
}

int gui_on(void) { return 0; }
void gui_pump(void) {}
uint32_t gui_buttons(void) { return 0u; }
void gui_consume_button_pulses(void) {}
void gui_analog(uint8_t *lx, uint8_t *ly) {
    if (lx) *lx = 128;
    if (ly) *ly = 128;
}
int gui_pad_present(void) { return 0; }
void gui_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    (void)fbaddr; (void)fmt; (void)stride;
}
/* The host-neutral HLE selftest omits the Vulkan backend. With no live GPU target,
 * a fully validated descriptor is correctly classified as guest-authoritative.
 *
 * The stub RECORDS what it was asked, because the observer-ownership test asserts
 * on the descriptor the caller constructs -- specifically that no descriptor is
 * handed to the coherence boundary at all before the guest owns the scanout.
 * Whether the boundary itself refuses a genuine mismatch is a different question,
 * owned by gpu-coherence-selftest against the real Vulkan target. */
static unsigned long g_sync_calls;
static GeGpuFbDescriptor g_sync_last;
int gegpu_sync_guest_fb(const GeGpuFbDescriptor *desc) {
    g_sync_calls++;
    if (desc) g_sync_last = *desc;
    return 2; /* GEGPU_SYNC_NO_TARGET */
}
void sr_profile_dump(void) {}
#ifdef SR_PSP_ORACLE_SMOKE
/* The smoke translation retains the production SR_YIELD instrumentation hook,
 * but this focused executable does not link the full profiler object. */
int g_prof_enabled;
void sr_profile_block(uint32_t target_pc) { (void)target_pc; }
#endif
uint64_t SDL_GetTicksNS(void) { return 0; }
int sdl3vk_capture_arm(const char *path) { (void)path; return 0; }
int sdl3vk_capture_result(void) { return 0; }
int sdl3vk_renderer_terminal(void) { return 0; }
const char *sdl3vk_capture_source_label(void) { return ""; }
int sdl3vk_validation_error_count(void) { return 0; }
unsigned long g_mpeg_put;
unsigned long g_mpeg_getavc;
unsigned long g_mpeg_avcdec;
unsigned long g_mpeg_nodata;
uint64_t sr_perf_now_ns(void) { return 0; }
void sr_perf_guest_begin(void) {}
void sr_perf_guest_end(void) {}
void sr_perf_guest_idle_wait(uint64_t started_ns) { (void)started_ns; }
void sr_perf_vblank(void) {}

/* The FD fixture deliberately exercises the writable host-backed branch.  Keep
 * the ISO side absent and deterministic rather than making the selftest depend
 * on a private game image. */
int iso_lookup(const char *guest_path, uint32_t *out_lba, uint32_t *out_size) {
    (void)guest_path; (void)out_lba; (void)out_size;
    return -1;
}
int iso_read(uint32_t lba, uint32_t offset, void *dst, uint32_t bytes) {
    (void)lba; (void)offset; (void)dst; (void)bytes;
    return -1;
}
int iso_list(const char *guest_path, uint32_t index, IsoDirEntry *out) {
    (void)guest_path; (void)index; (void)out;
    return 0;
}

/* recomp.c is not linked here. The #88 conformance matrix registers the pool
 * APIs, which reach hle.c's user_partition_init(); its only external dependency
 * is the loader's high-water mark for the module image.
 *
 * This synthetic world loads no module, so the value is a declared property of
 * the fixture rather than a measurement: 4 MiB, which sits above every guest
 * address this file fabricates (the 0x00200000 time block, the 0x00240000
 * conformance block, and sched.c's 0x0031xxxx / 0x00331b80 counters) and below
 * both the stack arena floor (0x05000000) and the default partition top
 * (0x0A000000). Returning 0 is not an option -- user_partition_init() correctly
 * fail-closes on it rather than placing the partition over the image. */
uint32_t sr_loaded_end(void) { return 0x00400000u; }

int g_hle_depth;
jmp_buf g_hle_jmp;
int sr_hit_hle;
void sr_trace_close(void) {}
static uint8_t s_heap_arena[1u << 20];
static size_t s_heap_arena_off;
uint32_t sr_newlib_malloc(uint32_t size, uint32_t guest_ra) {
    (void)guest_ra;
    size = (size + 15u) & ~15u;
    if (s_heap_arena_off + size > sizeof(s_heap_arena)) return 0u;
    uint32_t p = 0x09000000u + (uint32_t)s_heap_arena_off;
    s_heap_arena_off += size;
    return p;
}

/* The generated dispatcher is replaced by deterministic synthetic guest entries. The worker
 * still reaches the production handler exactly as a generated import stub does: through
 * sr_syscall with the public NID. The callback entry is only a guest address selector; its
 * arguments are captured from the CpuState that sr_callback_dispatch_one prepares. */
#define ORACLE_CALLBACK_ENTRY 0x0800cafeu
#define ORACLE_THREAD_ENTRY   0x0800db00u
#define ORACLE_CALLBACK_NAME  0x08000100u

static int32_t s_exit_argument;
static int s_exit_dispatches;
static uint32_t s_exit_nid = NID_SCE_KERNEL_EXIT_THREAD;
enum { ORACLE_THREAD_ACTION_EXIT = 0, ORACLE_THREAD_ACTION_SLEEP = 1,
       ORACLE_THREAD_ACTION_EXIT_DELETE = 2 };
static int s_oracle_thread_action;
static int s_oracle_mode;
static int s_oracle_callback_calls;
static uint32_t s_oracle_callback_arg1;
static uint32_t s_oracle_callback_arg2;

/* Defined in intr_conformance.h (included below, once the fixture helpers it
 * builds on exist). Returns non-zero when `target` is the synthetic VBLANK
 * sub-interrupt handler entry that the #88 conformance harness registered; in
 * that case it has already run its probe, in real interrupt context. */
static int ic_dispatch_intercept(uint32_t target);
/* Synthetic guest bodies for the nested-guest-call ABI specimen; defined
 * further down, next to the regression that reads what they recorded. */
static int cbabi_dispatch(CpuState *cpu, uint32_t target);

void dispatch(CpuState *cpu, uint32_t target) {
    if (ic_dispatch_intercept(target)) { cpu->r[2] = 0; return; }
    if (cbabi_dispatch(cpu, target)) return;
    if (s_oracle_mode && target == ORACLE_CALLBACK_ENTRY) {
        s_oracle_callback_calls++;
        s_oracle_callback_arg1 = cpu->r[4];
        s_oracle_callback_arg2 = cpu->r[5];
        cpu->r[2] = 0;
        return;
    }
    s_exit_dispatches++;
    if (s_oracle_thread_action == ORACLE_THREAD_ACTION_SLEEP) {
        (void)sr_syscall(cpu, NID_SCE_KERNEL_SLEEP_THREAD);
        return;
    }
    if (s_oracle_thread_action == ORACLE_THREAD_ACTION_EXIT_DELETE) {
        cpu->r[4] = (uint32_t)s_exit_argument;
        (void)sr_syscall(cpu, NID_SCE_KERNEL_EXIT_DELETE_THREAD_ORACLE);
        return;
    }
    cpu->r[4] = (uint32_t)s_exit_argument;
    (void)sr_syscall(cpu, s_exit_nid);
}

static int s_checks;
static int s_failures;
static void expect(int condition, const char *description) {
    s_checks++;
    if (!condition) {
        s_failures++;
        fprintf(stderr, "FAIL: %s\n", description);
    }
}

/* Test-only access to the real PRX parser.  The wrapper is compiled only for this executable;
 * the fixture below still exercises register_prx_exports(), elf_vaddr_to_file(), module-info
 * validation, and the production late-import registry rather than duplicating their logic. */
unsigned sr_hle_test_register_prx_exports(const char *host_path, uint32_t base);

static void fixture_wr16(uint8_t *p, uint16_t value) {
    memcpy(p, &value, sizeof(value));
}

static void fixture_wr32(uint8_t *p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static int write_synthetic_prx(const char *path, int malformed) {
    enum { SIZE = 0x220, PHOFF = 0x34, MODOFF = 0x100, ENTOFF = 0x180, TABLEOFF = 0x1c0 };
    uint8_t image[SIZE];
    memset(image, 0, sizeof(image));

    {
        static const uint8_t magic[8] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0};
        memcpy(image, magic, sizeof(magic));
    }
    fixture_wr16(image + 40, 52);       /* ELF header size */
    fixture_wr32(image + 28, PHOFF);    /* program-header table */
    fixture_wr16(image + 42, 32);       /* sizeof Elf32_Phdr */
    fixture_wr16(image + 44, 1);        /* one PT_LOAD */

    uint8_t *ph = image + PHOFF;
    fixture_wr32(ph + 0, 1);            /* PT_LOAD */
    fixture_wr32(ph + 4, 0x80);         /* file offset */
    fixture_wr32(ph + 8, 0x1000);       /* virtual address */
    fixture_wr32(ph + 12, MODOFF);     /* stripped-PRX module-info file hint */
    fixture_wr32(ph + 16, 0x1a0);      /* file size: 0x80..0x220 is file-backed */
    fixture_wr32(ph + 20, 0x1a0);      /* memory size */
    fixture_wr32(ph + 24, 5);           /* executable/readable */
    fixture_wr32(ph + 28, 4);           /* alignment */

    memcpy(image + MODOFF + 4, "synthetic", 9);
    fixture_wr32(image + MODOFF + 36, 0x1100); /* export-table virtual start */
    fixture_wr32(image + MODOFF + 40, malformed ? 0x21101 : 0x1110);

    uint8_t *entry = image + ENTOFF;
    entry[8] = 4;                       /* words per export entry */
    entry[9] = 0;                       /* variable exports */
    fixture_wr16(entry + 10, 3);        /* three function exports */
    fixture_wr32(entry + 12, 0x1140);   /* NID/target pair table */

    fixture_wr32(image + TABLEOFF + 0, 0x11111111); /* target base + 0 */
    fixture_wr32(image + TABLEOFF + 4, 0x22222222); /* target base + 0x20 */
    fixture_wr32(image + TABLEOFF + 8, 0x33333333); /* target that wraps below */
    fixture_wr32(image + TABLEOFF + 12, 0x00000000);
    fixture_wr32(image + TABLEOFF + 16, 0x00000020);
    fixture_wr32(image + TABLEOFF + 20, 0xd0000000);

    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t written = fwrite(image, 1, sizeof(image), f);
    int close_result = fclose(f);
    return written == sizeof(image) && close_result == 0;
}

static void test_prx_export_relocation_behavior(void) {
    const char *path = "hle_prx_export_fixture.bin";
    const uint32_t base = 0x30000000u;
    const uint32_t nid_zero = 0x11111111u;
    const uint32_t nid_nonzero = 0x22222222u;
    const uint32_t nid_wrap = 0x33333333u;

    int valid_fixture = write_synthetic_prx(path, 0);
    expect(valid_fixture, "synthetic PRX fixture was written");
    if (valid_fixture) {
        unsigned registered = sr_hle_test_register_prx_exports(path, base);
        expect(registered == 2, "loader publishes zero and nonzero exports but rejects wrapping target");
        expect(sr_hle_resolve_late_import(nid_zero) == base,
               "zero-relative export resolves to the loaded base address");
        expect(sr_hle_resolve_late_import(nid_nonzero) == base + 0x20u,
               "nonzero-relative export resolves to base plus target");
        expect(sr_hle_resolve_late_import(nid_wrap) == 0,
               "overflowing export target remains unresolved");
    }
    remove(path);

    int malformed_fixture = write_synthetic_prx(path, 1);
    expect(malformed_fixture, "malformed synthetic PRX fixture was written");
    if (malformed_fixture) {
        expect(sr_hle_test_register_prx_exports(path, base) == 0,
               "malformed module metadata publishes no exports");
    }
    remove(path);
}

static CpuState s_cpu_store;

/* Production-dispatch regression for utility AV module identity and lifecycle.  The
 * authoritative PPSSPP model uses 0x80111101/02/03 for bad, already-loaded, and not-loaded
 * generic utility modules, and requires AVCODEC before ATRAC3+, MPEGBASE, or MP4. This fixture
 * keeps the title's real AV IDs while proving state transitions through sr_syscall. */
static uint32_t utility_module_call(CpuState *cpu, uint32_t nid, uint32_t module) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->r[4] = module;
    return sr_syscall(cpu, nid);
}

/* Production-helper coverage for the guest FD namespace.  The payload/path
 * intentionally match fixtures/nakagawa_minimal_v4: the real PSP fixture's
 * byte-level oracle is `NAKAGAWA_MINIMAL SUM=5050\n`.  This native harness
 * exercises the same Open/Write/Close sequence against the production HLE
 * handlers and asserts the host bytes, while keeping private PSP artifacts out
 * of the repository. */
static void fd_guest_copy(uint32_t address, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) MEM_W8(address + (uint32_t)i, bytes[i]);
}

static void fd_host_path(char *out, size_t capacity, const char *guest) {
    const char root[] = "build/hle_fd_namespace_fs/";
    size_t at = 0;
    if (!out || capacity == 0) return;
    for (size_t i = 0; root[i] && at + 1 < capacity; i++) out[at++] = root[i];
    for (size_t i = 0; guest && guest[i] && at + 1 < capacity; i++) {
        char c = guest[i];
        out[at++] = (c == '/' || c == ':' || c == '\\' || c == ' ') ? '_' : c;
    }
    out[at] = '\0';
}

static int fd_host_bytes_equal(const char *path, const uint8_t *expected, size_t size) {
    FILE *host = fopen(path, "rb");
    if (!host) return 0;
    uint8_t actual[128];
    size_t got = size <= sizeof(actual) ? fread(actual, 1, sizeof(actual), host) : 0;
    int extra = fgetc(host) != EOF;
    fclose(host);
    return got == size && !extra && memcmp(actual, expected, size) == 0;
}

static void fd_set_path(CpuState *cpu, uint32_t path_address, const char *path) {
    memset(cpu, 0, sizeof(*cpu));
    fd_guest_copy(path_address, path, strlen(path) + 1u);
    cpu->r[4] = path_address;
    cpu->r[5] = 0x00000602u; /* PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC */
    cpu->r[6] = 0777u;
}

static void fd_set_write(CpuState *cpu, uint32_t fd, uint32_t source, uint32_t count) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->r[4] = fd;
    cpu->r[5] = source;
    cpu->r[6] = count;
}

static void test_fd_namespace(void) {
    enum {
        FD_KIND_STD = 1,
        FD_KIND_FILE = 2,
        SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR = 0x80020323u,
        SCE_KERNEL_ERROR_TOO_MANY_OPEN_FILES = 0x80020320u,
        SCE_KERNEL_ERROR_INVALID_ARGUMENT = 0x80020324u
    };
    const uint32_t path_addr = 0x09010000u;
    const uint32_t payload_addr = 0x09011000u;
    static const char result_guest[] = "ms0:/NAKAGAWA_MINIMAL_RESULT.TXT";
    static const uint8_t payload[] = "NAKAGAWA_MINIMAL SUM=5050\n";
    char result_host[256];
    CpuState cpu;
    const char *old_root_value = getenv("SR_FSDIR");
    char *old_root = old_root_value ? (char *)malloc(strlen(old_root_value) + 1u) : NULL;
    if (old_root) memcpy(old_root, old_root_value, strlen(old_root_value) + 1u);

    fd_host_path(result_host, sizeof(result_host), result_guest);
    DeleteFileA(result_host);
    CreateDirectoryA("build", NULL);
    CreateDirectoryA("build/hle_fd_namespace_fs", NULL);
    SetEnvironmentVariableA("SR_FSDIR", "build/hle_fd_namespace_fs");

    /* sr_hle_init performs the real runtime descriptor-table initialization. */
    sr_hle_init();
    expect(sr_hle_test_fd_kind(0) == FD_KIND_STD &&
           sr_hle_test_fd_kind(1) == FD_KIND_STD &&
           sr_hle_test_fd_kind(2) == FD_KIND_STD,
           "runtime initialization reserves fd 0/1/2 as standard descriptors");

    fd_set_path(&cpu, path_addr, result_guest);
    uint32_t fd = sr_hle_test_io_open(&cpu);
    expect(fd == 3u, "first ordinary sceIoOpen-style allocation returns fd 3");
    expect(sr_hle_test_fd_kind(fd) == FD_KIND_FILE,
           "ordinary allocation records file identity independently of fd number");

    fd_guest_copy(payload_addr, payload, sizeof(payload) - 1u);
    fd_set_write(&cpu, fd, payload_addr, (uint32_t)(sizeof(payload) - 1u));
    expect(sr_hle_test_io_write(&cpu) == sizeof(payload) - 1u,
           "write through the first ordinary descriptor reports the full payload");
    expect(fd_host_bytes_equal(result_host, payload, sizeof(payload) - 1u),
           "Phase-5 payload is persisted byte-for-byte through the ordinary fd");

    /* Standard descriptor operations */
    fd_set_write(&cpu, 1u, payload_addr, (uint32_t)(sizeof(payload) - 1u));
    expect(sr_hle_test_io_write(&cpu) == sizeof(payload) - 1u,
           "write through stdout's reserved descriptor follows the console path");
    expect(fd_host_bytes_equal(result_host, payload, sizeof(payload) - 1u),
           "stdout write does not alter the ordinary host file");
    fd_set_write(&cpu, 0u, payload_addr, (uint32_t)(sizeof(payload) - 1u));
    expect(sr_hle_test_io_write(&cpu) == sizeof(payload) - 1u,
           "stdin's reserved descriptor remains a distinct standard object");
    fd_set_write(&cpu, 2u, payload_addr, (uint32_t)(sizeof(payload) - 1u));
    expect(sr_hle_test_io_write(&cpu) == sizeof(payload) - 1u,
           "stderr's reserved descriptor follows the standard-stream path");

    /* Standard descriptor operations outside console write preserve baseline behavior */
    expect(sr_hle_test_io_read(&(CpuState){.r = {0, 0, 0, 0, 1u, 0, 0}}) == 0x80010009u,
           "read on a standard descriptor preserves baseline errno 0x80010009");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 1u;
    expect(sr_hle_test_io_lseek32(&cpu) == 0x80010009u,
           "lseek32 on a standard descriptor preserves baseline errno 0x80010009");

    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 1u;
    uint32_t lseek_std = sr_hle_test_io_lseek(&cpu);
    expect(lseek_std == 0x80010009u && cpu.r[3] == 0u,
           "lseek on a standard descriptor preserves baseline errno 0x80010009");

    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 1u;
    expect(sr_hle_test_io_ioctl(&cpu) == 0x80010009u,
           "ioctl on a standard descriptor preserves baseline errno 0x80010009");

    /* Scope-negative: Unsupported ioctl command on valid file fd remains 0x80010086 */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = fd;
    cpu.r[5] = 0x99999999u;
    expect(sr_hle_test_io_ioctl(&cpu) == 0x80010086u,
           "unsupported ioctl command returns 0x80010086");

    /* Scope-negative: Non-existent file open remains driver errno (0x80010002) */
    memset(&cpu, 0, sizeof(cpu));
    fd_set_path(&cpu, path_addr, "ms0:/NON_EXISTENT_FILE_12345.TXT");
    cpu.r[5] = 1u;
    expect(sr_hle_test_io_open(&cpu) == 0x80010002u,
           "open non-existent file returns driver errno 0x80010002");

    /* Out-of-range / Negative descriptors (0xFFFFFFFFu = -1) */
    fd_set_write(&cpu, 0xffffffffu, payload_addr, (uint32_t)(sizeof(payload) - 1u));
    expect(sr_hle_test_io_write(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "write on negative fd reports manager bad-fd");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 0xffffffffu;
    expect(sr_hle_test_io_read(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "read on negative fd reports manager bad-fd");
    cpu.r[4] = 0xffffffffu;
    expect(sr_hle_test_io_lseek32(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "lseek32 on negative fd reports manager bad-fd");
    cpu.r[4] = 0xffffffffu;
    cpu.r[29] = 0x09012000u;
    MEM_W32(cpu.r[29] + 16u, 0u);
    uint32_t lseek_neg = sr_hle_test_io_lseek(&cpu);
    expect(lseek_neg == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR && cpu.r[3] == 0xFFFFFFFFu,
           "lseek on negative fd reports manager bad-fd with high-word error");
    cpu.r[4] = 0xffffffffu;
    expect(sr_hle_test_io_ioctl(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "ioctl on negative fd reports manager bad-fd");
    cpu.r[4] = 0xffffffffu;
    expect(sr_hle_test_io_close(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "close on negative fd reports manager bad-fd");
    cpu.r[4] = 0xffffffffu;
    expect(sr_hle_test_io_close_async(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "close_async on negative fd reports manager bad-fd");

    /* Unknown out-of-table fd (64u) */
    fd_set_write(&cpu, 64u, payload_addr, (uint32_t)(sizeof(payload) - 1u));
    expect(sr_hle_test_io_write(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "write on out-of-table fd 64 reports manager bad-fd");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 64u;
    expect(sr_hle_test_io_read(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "read on out-of-table fd 64 reports manager bad-fd");
    cpu.r[4] = 64u;
    expect(sr_hle_test_io_lseek32(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "lseek32 on out-of-table fd 64 reports manager bad-fd");
    cpu.r[4] = 64u;
    cpu.r[29] = 0x09012000u;
    MEM_W32(cpu.r[29] + 16u, 0u);
    uint32_t lseek_oot = sr_hle_test_io_lseek(&cpu);
    expect(lseek_oot == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR && cpu.r[3] == 0xFFFFFFFFu,
           "lseek on out-of-table fd 64 reports manager bad-fd");
    cpu.r[4] = 64u;
    expect(sr_hle_test_io_ioctl(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "ioctl on out-of-table fd 64 reports manager bad-fd");
    cpu.r[4] = 64u;
    expect(sr_hle_test_io_close(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "close on out-of-table fd 64 reports manager bad-fd");
    cpu.r[4] = 64u;
    expect(sr_hle_test_io_close_async(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "close_async on out-of-table fd 64 reports manager bad-fd");

    /* Directory descriptor passed to file API (Wrong kind) */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 0x100u;
    expect(sr_hle_test_io_write(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "write on directory fd reports manager bad-fd");
    expect(sr_hle_test_io_read(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "read on directory fd reports manager bad-fd");
    expect(sr_hle_test_io_lseek32(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "lseek32 on directory fd reports manager bad-fd");
    cpu.r[29] = 0x09012000u;
    MEM_W32(cpu.r[29] + 16u, 0u);
    uint32_t lseek_dir = sr_hle_test_io_lseek(&cpu);
    expect(lseek_dir == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR && cpu.r[3] == 0xFFFFFFFFu,
           "lseek on directory fd reports manager bad-fd");
    expect(sr_hle_test_io_ioctl(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "ioctl on directory fd reports manager bad-fd");
    expect(sr_hle_test_io_close(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "close on directory fd reports manager bad-fd");

    /* File descriptor passed to directory API (Wrong kind) */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = fd;
    cpu.r[5] = payload_addr;
    expect(sr_hle_test_io_dread(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "dread on file fd reports manager bad-fd");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = fd;
    expect(sr_hle_test_io_dclose(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "dclose on file fd reports manager bad-fd");

    /* Invalid directory descriptors */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 0xffffffffu;
    expect(sr_hle_test_io_dread(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "dread on negative dir fd reports manager bad-fd");
    expect(sr_hle_test_io_dclose(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "dclose on negative dir fd reports manager bad-fd");
    cpu.r[4] = 0x200u;
    expect(sr_hle_test_io_dread(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "dread on out-of-range dir fd reports manager bad-fd");
    expect(sr_hle_test_io_dclose(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "dclose on out-of-range dir fd reports manager bad-fd");
    cpu.r[4] = 0x100u;
    expect(sr_hle_test_io_dread(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "dread on unallocated dir fd reports manager bad-fd");
    expect(sr_hle_test_io_dclose(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "dclose on unallocated dir fd reports manager bad-fd");

    /* Valid directory descriptor operations */
    memset(&cpu, 0, sizeof(cpu));
    fd_set_path(&cpu, path_addr, "disc0:/");
    uint32_t dir_fd = sr_hle_test_io_dopen(&cpu);
    expect(dir_fd == 0x100u, "dopen on disc0:/ succeeds and returns dir fd 0x100");
    /* Scope-negative non-regression: valid dir fd with null dirent pointer (de == 0) preserves baseline 0x80010009 */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = dir_fd;
    cpu.r[5] = 0u;
    expect(sr_hle_test_io_dread(&cpu) == 0x80010009u,
           "dread on valid dir fd with null pointer preserves baseline errno 0x80010009");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = dir_fd;
    expect(sr_hle_test_io_dclose(&cpu) == 0u, "dclose on valid dir fd succeeds");

    /* Whence validation on valid open file */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = fd;
    cpu.r[5] = 0u;     /* offset */
    cpu.r[6] = 3u;     /* invalid whence */
    expect(sr_hle_test_io_lseek32(&cpu) == SCE_KERNEL_ERROR_INVALID_ARGUMENT,
           "lseek32 with whence=3 reports manager invalid-argument");
    cpu.r[6] = 0xffffffffu;  /* invalid whence negative */
    expect(sr_hle_test_io_lseek32(&cpu) == SCE_KERNEL_ERROR_INVALID_ARGUMENT,
           "lseek32 with negative whence reports manager invalid-argument");
    cpu.r[6] = 100u;   /* invalid whence large */
    expect(sr_hle_test_io_lseek32(&cpu) == SCE_KERNEL_ERROR_INVALID_ARGUMENT,
           "lseek32 with whence=100 reports manager invalid-argument");

    /* Valid whence operations on lseek32 */
    cpu.r[5] = 5u;
    cpu.r[6] = 0u;     /* SEEK_SET */
    expect(sr_hle_test_io_lseek32(&cpu) == 5u, "lseek32 SEEK_SET succeeds");
    cpu.r[5] = 3u;
    cpu.r[6] = 1u;     /* SEEK_CUR */
    expect(sr_hle_test_io_lseek32(&cpu) == 8u, "lseek32 SEEK_CUR succeeds");
    cpu.r[5] = 0u;
    cpu.r[6] = 2u;     /* SEEK_END */
    expect(sr_hle_test_io_lseek32(&cpu) == sizeof(payload) - 1u, "lseek32 SEEK_END succeeds");

    /* Whence validation on 64-bit lseek */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = fd;
    cpu.r[6] = 0u;     /* offset low */
    cpu.r[7] = 0u;     /* offset high */
    cpu.r[8] = 3u;     /* whence = 3 */
    uint32_t lseek_w3 = sr_hle_test_io_lseek(&cpu);
    expect(lseek_w3 == SCE_KERNEL_ERROR_INVALID_ARGUMENT && cpu.r[3] == 0xFFFFFFFFu,
           "lseek with whence=3 reports manager invalid-argument with high-word error");
    cpu.r[8] = 0xffffffffu;  /* whence = -1 */
    uint32_t lseek_wn = sr_hle_test_io_lseek(&cpu);
    expect(lseek_wn == SCE_KERNEL_ERROR_INVALID_ARGUMENT && cpu.r[3] == 0xFFFFFFFFu,
           "lseek with negative whence reports manager invalid-argument with high-word error");

    /* Valid whence on 64-bit lseek */
    cpu.r[8] = 0u;     /* SEEK_SET */
    cpu.r[6] = 5u;
    expect(sr_hle_test_io_lseek(&cpu) == 5u && cpu.r[3] == 0u, "lseek SEEK_SET succeeds");
    cpu.r[8] = 1u;     /* SEEK_CUR */
    cpu.r[6] = 3u;
    expect(sr_hle_test_io_lseek(&cpu) == 8u && cpu.r[3] == 0u, "lseek SEEK_CUR succeeds");
    cpu.r[8] = 2u;     /* SEEK_END */
    cpu.r[6] = 0u;
    expect(sr_hle_test_io_lseek(&cpu) == sizeof(payload) - 1u && cpu.r[3] == 0u, "lseek SEEK_END succeeds");

    /* Closed descriptor behavior */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = fd;
    expect(sr_hle_test_io_close(&cpu) == 0u, "closing the ordinary descriptor succeeds");
    fd_set_write(&cpu, fd, payload_addr, (uint32_t)(sizeof(payload) - 1u));
    expect(sr_hle_test_io_write(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "write through a closed ordinary descriptor reports manager bad-fd");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = fd;
    expect(sr_hle_test_io_read(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "read through a closed ordinary descriptor reports manager bad-fd");
    cpu.r[4] = fd;
    expect(sr_hle_test_io_lseek32(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "lseek32 through a closed ordinary descriptor reports manager bad-fd");
    cpu.r[4] = fd;
    cpu.r[29] = 0x09012000u;
    MEM_W32(cpu.r[29] + 16u, 0u);
    uint32_t lseek_closed = sr_hle_test_io_lseek(&cpu);
    expect(lseek_closed == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR && cpu.r[3] == 0xFFFFFFFFu,
           "lseek through a closed descriptor reports manager bad-fd");
    cpu.r[4] = fd;
    expect(sr_hle_test_io_ioctl(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "ioctl through a closed descriptor reports manager bad-fd");
    cpu.r[4] = fd;
    expect(sr_hle_test_io_close(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "closing an already-closed descriptor reports manager bad-fd");
    cpu.r[4] = fd;
    expect(sr_hle_test_io_close_async(&cpu) == SCE_KERNEL_ERROR_BAD_FILE_DESCRIPTOR,
           "close_async on an already-closed descriptor reports manager bad-fd");

    /* FD Reuse */
    fd_set_path(&cpu, path_addr, result_guest);
    uint32_t reused = sr_hle_test_io_open(&cpu);
    expect(reused == 3u, "closing an ordinary descriptor releases fd 3 for reuse");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = reused;
    expect(sr_hle_test_io_close(&cpu) == 0u, "reused ordinary descriptor closes cleanly");

    fd_set_path(&cpu, path_addr, result_guest);
    uint32_t async_fd = sr_hle_test_io_open_async(&cpu);
    expect(async_fd == 3u, "async open also allocates from the ordinary fd namespace");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = async_fd;
    expect(sr_hle_test_io_close_async(&cpu) == 0u,
           "async close releases the ordinary descriptor through shared teardown");
    fd_set_path(&cpu, path_addr, result_guest);
    expect(sr_hle_test_io_open(&cpu) == 3u,
           "a descriptor released by async close is available for reuse");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 3u;
    expect(sr_hle_test_io_close(&cpu) == 0u,
           "the descriptor reused after async close closes cleanly");

    /* Fill every ordinary slot to cover the upper bound and prove std slots can
     * never be reached by allocation, even when the table is exhausted. */
    uint32_t open_fds[61];
    char guest_path[96], host_path[256];
    for (uint32_t i = 0; i < 61u; i++) {
        snprintf(guest_path, sizeof(guest_path), "ms0:/NAKAGAWA_FD_SLOT_%02u.TXT", i);
        fd_set_path(&cpu, path_addr, guest_path);
        open_fds[i] = sr_hle_test_io_open(&cpu);
        expect(open_fds[i] == 3u + i, "ordinary descriptor allocation stays within fd 3..63");
    }
    snprintf(guest_path, sizeof(guest_path), "ms0:/NAKAGAWA_FD_OVERFLOW.TXT");
    fd_set_path(&cpu, path_addr, guest_path);
    expect(sr_hle_test_io_open(&cpu) == SCE_KERNEL_ERROR_TOO_MANY_OPEN_FILES,
           "ordinary allocation reports manager TOO_MANY_OPEN_FILES when fd 3..63 are exhausted");

    /* Release one slot and prove allocation succeeds again immediately */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = open_fds[0];
    expect(sr_hle_test_io_close(&cpu) == 0u, "closing slot 0 frees it for reallocation");
    fd_set_path(&cpu, path_addr, "ms0:/NAKAGAWA_FD_REALLOC.TXT");
    uint32_t realloc_fd = sr_hle_test_io_open(&cpu);
    expect(realloc_fd == 3u, "allocation immediately reuses the freed slot");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = realloc_fd;
    expect(sr_hle_test_io_close(&cpu) == 0u, "reallocated descriptor closes cleanly");
    fd_host_path(host_path, sizeof(host_path), "ms0:/NAKAGAWA_FD_REALLOC.TXT");
    DeleteFileA(host_path);

    for (uint32_t i = 1; i < 61u; i++) {
        memset(&cpu, 0, sizeof(cpu));
        cpu.r[4] = open_fds[i];
        expect(sr_hle_test_io_close(&cpu) == 0u, "each exhausted-table descriptor closes cleanly");
        snprintf(guest_path, sizeof(guest_path), "ms0:/NAKAGAWA_FD_SLOT_%02u.TXT", i);
        fd_host_path(host_path, sizeof(host_path), guest_path);
        DeleteFileA(host_path);
    }
    snprintf(guest_path, sizeof(guest_path), "ms0:/NAKAGAWA_FD_SLOT_00.TXT");
    fd_host_path(host_path, sizeof(host_path), guest_path);
    DeleteFileA(host_path);

    /* Closing a standard descriptor makes it invalid for I/O but does not turn
     * its reserved identity into an ordinary allocation slot. */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 2u;
    expect(sr_hle_test_io_close(&cpu) == 0u, "closing a standard descriptor succeeds");
    expect(sr_hle_test_fd_kind(2u) == FD_KIND_STD,
           "closed standard descriptor retains its reserved identity");
    fd_set_path(&cpu, path_addr, result_guest);
    uint32_t after_std_close = sr_hle_test_io_open(&cpu);
    expect(after_std_close == 3u,
           "ordinary allocation still starts at fd 3 after a standard close");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = after_std_close;
    expect(sr_hle_test_io_close(&cpu) == 0u,
           "ordinary descriptor opened after a standard close closes cleanly");

    DeleteFileA(result_host);
    RemoveDirectoryA("build/hle_fd_namespace_fs");
    if (old_root) SetEnvironmentVariableA("SR_FSDIR", old_root);
    else SetEnvironmentVariableA("SR_FSDIR", NULL);
    free(old_root);
}

static void test_utility_av_module_state(void) {
    CpuState cpu;
    sr_hle_init();

    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_MODULE, 0x2ffu) == SCE_ERROR_MODULE_BAD_ID,
           "utility AV load rejects an invalid module ID below the AV range");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_MODULE, 0x309u) == SCE_ERROR_MODULE_BAD_ID,
           "utility AV load rejects an invalid module ID above the AV range");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_MODULE, 0x2ffu) == SCE_ERROR_MODULE_BAD_ID,
           "utility AV unload rejects an invalid module ID");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_MODULE, 0x302u) == SCE_ERROR_AV_LIBRARY_NOT_FOUND,
           "ATRAC3+ load requires AVCODEC to be loaded first");

    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_MODULE, 0x300u) == 0,
           "AVCODEC loads successfully");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_MODULE, 0x300u) == SCE_ERROR_MODULE_ALREADY_LOADED,
           "duplicate AVCODEC load reports already loaded");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_MODULE, 0x302u) == 0,
           "ATRAC3+ loads after AVCODEC");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_MODULE, 0x308u) == 0,
           "MP4 loads after AVCODEC");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_MODULE, 0x308u) == 0,
           "MP4 unloads after use");

    for (uint32_t module = 0x301u; module <= 0x307u; module++) {
        if (module == 0x302u) continue; /* already loaded above */
        expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_MODULE, module) == 0,
               "each remaining AV module ID loads once");
        expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_MODULE, module) == 0,
               "each remaining AV module ID unloads once");
    }
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_MODULE, 0x302u) == 0,
           "ATRAC3+ unloads after use");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_MODULE, 0x302u) == SCE_ERROR_MODULE_NOT_LOADED,
           "duplicate ATRAC3+ unload reports not loaded");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_MODULE, 0x300u) == 0,
           "AVCODEC unloads after dependent module shutdown");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_MODULE, 0x300u) == SCE_ERROR_MODULE_NOT_LOADED,
           "duplicate AVCODEC unload reports not loaded");

    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_AV_MODULE, 8u) == SCE_ERROR_AV_MODULE_BAD_ID,
           "AV-specific load rejects an out-of-range index with its AV error");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_AV_MODULE, 8u) == SCE_ERROR_AV_MODULE_BAD_ID,
           "AV-specific unload rejects an out-of-range index with its AV error");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_AV_MODULE, 0u) == SCE_ERROR_AV_MODULE_NOT_LOADED,
           "AV-specific unload distinguishes an unloaded module");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_AV_MODULE, 0u) == 0,
           "AV-specific load maps index zero to AVCODEC");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_LOAD_AV_MODULE, 0u) == SCE_ERROR_AV_MODULE_ALREADY_LOADED,
           "AV-specific duplicate load reports its AV error");
    expect(utility_module_call(&cpu, NID_SCE_UTILITY_UNLOAD_AV_MODULE, 0u) == 0,
           "AV-specific unload clears the shared AVCODEC state");
}

/* ---- coroutine park ------------------------------------------------------------------
 *
 * sched.c's coro_body never returns, so a test body must not either: it hands control back
 * to the one scheduler coroutine established at startup and stays parked there.
 *
 * s_sched_coro is that identity, set once by sched_init() -> sr_coro_main(). Reading it is
 * the whole operation; there is deliberately no call that could establish a *new* identity
 * here. The guard below is defence in depth and its textual presence proves nothing -- what
 * proves the invariant is that sr_coro.c counts every adoption and every suppressed
 * self-switch, and check_coroutine_lifecycle() asserts the exact totals. */
static unsigned long s_parks;
static const void   *s_park_target_mismatch;

static void selftest_park_on_scheduler(void) {
    for (;;) {
        SrCoro *target = s_sched_coro;
        SrCoroLifecycle lc;
        sr_coro_lifecycle_snapshot(&lc);
        /* Record rather than merely trust: the park target must be the identity the
         * implementation itself recorded at adoption time. */
        if ((const void *)target != lc.main_coro) s_park_target_mismatch = (const void *)target;
        if (!target || target == sr_coro_current()) {
            fprintf(stderr, "FAIL: park target invalid (target=%p current=%p)\n",
                    (void *)target, (void *)sr_coro_current());
            fflush(stderr);
            abort();
        }
        s_parks++;
        sr_coro_switch(target);
    }
}

static TCB *fixture_thread(uint32_t uid, int state, int priority);

static void reset_fixture(void) {
    memset(g_mem_base, 0, 0x0c000000u);
    sr_display_test_reset();
    memset(s_tcb, 0, sizeof(s_tcb));
    memset(s_libc_threads, 0, sizeof(s_libc_threads));
    memset(&s_cpu_store, 0, sizeof(s_cpu_store));
    s_ntcb = 0;
    s_cur = -1;
    s_last_pick = -1;
    s_root_seen = 0;
    /* Deliberately install a CAPTURED-role world: this suite exercises the HLE paths
     * that only exist once roles are held, and it fabricates matching TCBs for these
     * exact UIDs below. These are fixture assignments, not defaults -- production
     * starts every role at SR_ROLE_UID_NONE and captures it from a configured entry. */
    g_root_uid = 0x110u;
    g_launcher_uid = 0x111u;
    g_worker_uid = 0x114u; /* primary render worker, not the resource worker below */
    g_master_reent = 0x002cf338u;
    s_stack_top = 0x09f00000u;
    stack_ranges_reset();
    s_vtime_us = 0;
    s_tick = 0;
    s_interrupts_enabled = 1;
    s_dispatch_enabled = 1;
    s_pending_interrupts = 0;
    s_servicing_interrupts = 0;
    s_vbl_event_period_rem = 0;
    s_vbl_next_us = 0;
    s_vbl_count = 0;
    s_vblank_q_us = -1;
    s_last_vblank_ns = 0;
    s_heap_arena_off = 0;
    s_exit_dispatches = 0;
    s_oracle_thread_action = ORACLE_THREAD_ACTION_EXIT;
    s_cpu = &s_cpu_store;
    s_pace_on = 0;
    s_host_ns_fn = NULL;   /* deterministic timeline: no host clock in this fixture */
    sr_hle_test_audio_reset();
    audio_fixture_reset();
}

static uint32_t audio_dispatch(CpuState *cpu, uint32_t nid,
                               uint32_t a0, uint32_t a1, uint32_t a2) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->r[4] = a0;
    cpu->r[5] = a1;
    cpu->r[6] = a2;
    return sr_syscall(cpu, nid);
}

/* PRODUCTION_DISPATCH host regression, with CORROBORATIVE_ONLY PSP contract
 * inputs pinned to public sources (not a local hardware claim):
 *
 * - PSPSDK src/audio/pspaudio.h @
 *   314b2083f2e1eaf145fc5de342736336fe1f0148: regular samples are 64..65472
 *   aligned to 64; stereo is 0 and mono is 0x10.
 * - PSPAutotests tests/audio/sceaudio/{reserve,datalen}.{c,expected} @
 *   ea71108f00933712c4662276261b39cd42249b1e records the corresponding
 *   INVALID_SIZE / INVALID_FORMAT / INVALID_CH / NOT_INITIALIZED results.
 *
 * The guest-span case is a host memory-safety contract: a rejected buffer must
 * be atomic before scalar reads, telemetry/backend submission, queue queries,
 * or scheduler-time changes. It is HOST_TESTED, not PSP_HARDWARE evidence. */
static void test_audio_regular_contract_safety(void) {
    enum {
        MONO_EDGE = 0x0bffff80u,  /* exactly 64 mono frames fit in the arena */
        STEREO_BUF = 0x08a00000u, /* 64 stereo frames, well inside the arena */
    };
    CpuState cpu;
    int reserved = -1, format = -1;
    uint32_t frames = 0xffffffffu;

    reset_fixture();
    sr_hle_init();
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) &&
               reserved == 0 && frames == 0u && format == 0,
           "audio fixture starts with an unreserved regular channel");

    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 0xffffffffu, 0u) ==
               SCE_AUDIO_ERROR_INVALID_SIZE,
           "AudioChReserve rejects an unsigned-oversized sample count");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) &&
               reserved == 0 && frames == 0u && format == 0,
           "oversized reservation rejection does not mutate channel state");
    (void)audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RELEASE, 0u, 0u, 0u);

    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 96u, 0u) ==
               SCE_AUDIO_ERROR_INVALID_SIZE,
           "AudioChReserve rejects a sample count not aligned to 64");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) &&
               reserved == 0 && frames == 0u && format == 0,
           "misaligned reservation rejection does not mutate channel state");
    (void)audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RELEASE, 0u, 0u, 0u);

    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 65536u, 0u) ==
               SCE_AUDIO_ERROR_INVALID_SIZE,
           "AudioChReserve rejects the first aligned count above the public maximum");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) &&
               reserved == 0 && frames == 0u && format == 0,
           "above-maximum reservation rejection does not mutate channel state");
    (void)audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RELEASE, 0u, 0u, 0u);

    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 65472u, 0u) == 0u,
           "AudioChReserve accepts the aligned public maximum sample count");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) &&
               reserved == 1 && frames == 65472u && format == 0,
           "maximum-size reservation records the accepted sample count");
    (void)audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RELEASE, 0u, 0u, 0u);
    sr_hle_test_audio_reset();

    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 64u, 1u) ==
               SCE_AUDIO_ERROR_INVALID_FORMAT,
           "AudioChReserve rejects format 1 rather than treating it as mono");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) &&
               reserved == 0 && frames == 0u && format == 0,
           "invalid-format reservation rejection does not mutate channel state");
    (void)audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RELEASE, 0u, 0u, 0u);

    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 64u, 0x10u) == 0u,
           "AudioChReserve accepts the public mono format value 0x10");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) &&
               reserved == 1 && frames == 64u && format == 0x10,
           "mono reservation retains the 0x10 format identity");
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 128u, 0u) ==
               SCE_AUDIO_ERROR_INVALID_CH,
           "AudioChReserve rejects an already-reserved channel");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) &&
               reserved == 1 && frames == 64u && format == 0x10,
           "duplicate reservation rejection preserves length and format");

    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_SET_DATA_LEN, 0u, 96u, 0u) ==
               SCE_AUDIO_ERROR_INVALID_SIZE,
           "AudioSetChannelDataLen rejects a misaligned sample count");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) && frames == 64u,
           "invalid SetChannelDataLen does not mutate the prior length");
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_SET_DATA_LEN, 0u, 65536u, 0u) ==
               SCE_AUDIO_ERROR_INVALID_SIZE,
           "AudioSetChannelDataLen rejects the first aligned count above the maximum");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) && frames == 64u,
           "above-maximum SetChannelDataLen rejection preserves the prior length");
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_SET_DATA_LEN, 0u, 128u, 0u) == 0u,
           "AudioSetChannelDataLen accepts an aligned in-range sample count");
    expect(sr_hle_test_audio_state(0u, &reserved, &frames, &format) && frames == 128u,
           "valid SetChannelDataLen mutates the reserved channel length");
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_SET_DATA_LEN, 0u, 64u, 0u) == 0u,
           "AudioSetChannelDataLen restores the 64-frame mono fixture");
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_SET_DATA_LEN, 1u, 64u, 0u) ==
               SCE_AUDIO_ERROR_NOT_INITIALIZED,
           "AudioSetChannelDataLen rejects an unreserved regular channel");
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_SET_DATA_LEN, 0xffffffffu, 64u, 0u) ==
               SCE_AUDIO_ERROR_INVALID_CH,
           "AudioSetChannelDataLen rejects an invalid channel before mutation");

    for (uint32_t i = 0; i < 64u; i++)
        MEM_W16(MONO_EDGE + i * 2u, (uint16_t)(i + 1u));
    audio_fixture_reset();
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_OUTPUT_BLOCKING,
                          0u, 0x8000u, MONO_EDGE) == 64u,
           "AudioOutputBlocking accepts a whole-span-valid mono edge buffer");
    expect(s_oor_calls == 0u,
           "mono 0x10 reads exactly two bytes per frame without crossing the arena");
    expect(s_audio_push_calls == 1u && s_audio_push_frames == 64,
           "valid mono output reaches the host backend once with 64 frames");
    expect(s_audio_push_first_l == 1 && s_audio_push_first_r == 1 &&
               s_audio_push_last_l == 64 && s_audio_push_last_r == 64,
           "mono 0x10 expands each source sample to equal left/right samples");
    expect(s_audio_queue_calls == 2u,
           "a zero-lead queue is observed once before and once inside the wait");

    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RELEASE, 0u, 0u, 0u) == 0u,
           "mono fixture channel releases before the stereo span case");
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 64u, 0u) == 0u,
           "stereo span fixture reserves a 64-frame channel");
    audio_fixture_reset();
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_OUTPUT_BLOCKING,
                          0u, 0x8000u, MONO_EDGE) == SCE_KERNEL_ERROR_ILLEGAL_ADDR,
           "AudioOutputBlocking rejects a stereo buffer whose whole span is invalid");
    expect(s_oor_calls == 0u,
           "whole-span rejection occurs before any scalar guest sample read");
    expect(s_audio_push_calls == 0u,
           "whole-span rejection occurs before any backend submission");
    expect(s_audio_queue_calls == 0u,
           "whole-span rejection occurs before any host queue query or wait");

    /* Drain path. The blocking output re-reads the host queue while the backend
     * still holds more than one channel period, then returns. Wait duration is
     * deliberately not asserted here: sched_delay_current() is inert without a
     * current scheduler thread, so this fixture can only witness loop shape. */
    sr_hle_test_audio_reset();
    audio_fixture_reset();
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 64u, 0u) == 0u,
           "queue-drain fixture reserves a 64-frame stereo channel");
    for (uint32_t i = 0; i < 64u * 2u; i++)
        MEM_W16(STEREO_BUF + i * 2u, (uint16_t)(i + 1u));
    s_audio_queue_seq[0] = 192;
    s_audio_queue_seq[1] = 128;
    s_audio_queue_seq[2] = 64;
    s_audio_queue_seq_len = 3;
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_OUTPUT_BLOCKING,
                          0u, 0x8000u, STEREO_BUF) == 64u,
           "AudioOutputBlocking returns the channel frame count after draining");
    expect(s_audio_push_calls == 1u && s_audio_push_frames == 64,
           "the drain path submits the buffer once before waiting");
    expect(s_audio_queue_calls == 3u,
           "the drain loop re-reads the host queue until the lead is one period");


    /* The backend reports -1 when it has no queue. That sentinel must end the
     * wait rather than being compared as a frame count. */
    sr_hle_test_audio_reset();
    audio_fixture_reset();
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_CH_RESERVE, 0u, 64u, 0u) == 0u,
           "queue-sentinel fixture reserves a 64-frame stereo channel");
    s_audio_queue_result = -1;
    expect(audio_dispatch(&cpu, NID_SCE_AUDIO_OUTPUT_BLOCKING,
                          0u, 0x8000u, STEREO_BUF) == 64u,
           "a negative queue report ends the wait instead of looping");
    expect(s_audio_queue_calls == 1u,
           "the negative queue sentinel takes the open-loop path after one query");
}

static uint64_t selftest_guest_u64(uint32_t addr) {
    return (uint64_t)MEM_R32(addr) | ((uint64_t)MEM_R32(addr + 4u) << 32);
}

/* Production-dispatch clock regression.  The fixture sets the scheduler's
 * deterministic timeline directly, then enters every API through sr_syscall;
 * repeated observations must agree without changing that timeline. */
static void test_time_domains_are_coherent(void) {
    enum {
        SYS_OUT = 0x00200000u,
        TICK_OUT = 0x00200010u,
        TV_OUT = 0x00200020u,
        TZ_OUT = 0x00200030u,
        DATE_EXPLICIT = 0x00200100u,
        DATE_LOCAL = 0x00200120u,
        DATE_TICK_EXPLICIT = 0x00200140u,
        DATE_TICK_LOCAL = 0x00200150u,
        BAD_U64 = 0x0bfffffcu,
        BAD_DATE = 0x0bfffff4u,
    };
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    s_vtime_us = 1234567u;
    uint64_t before = s_vtime_us;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    cpu.r[4] = SYS_OUT;
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME) == 0u,
           "GetSystemTime dispatch writes the scheduler clock");
    uint64_t system_tick = selftest_guest_u64(SYS_OUT);
    expect(system_tick == before, "GetSystemTime maps directly to scheduler time");

    uint32_t low1 = sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME_LOW);
    uint32_t low2 = sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME_LOW);
    expect(low1 == low2 && low1 == (uint32_t)before,
           "repeated GetSystemTimeLow queries are stable");
    uint32_t wide_lo = sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME_WIDE);
    uint64_t wide = ((uint64_t)cpu.r[3] << 32) | wide_lo;
    expect(wide == before, "GetSystemTimeWide shares the same timeline");
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_LIBC_CLOCK) == (uint32_t)before,
           "libc clock uses scheduler microseconds without a pseudo epoch");

    cpu.r[4] = TICK_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_TICK) == 0u,
           "RTC current tick dispatch succeeds");
    uint64_t rtc1 = selftest_guest_u64(TICK_OUT);
    cpu.r[4] = TICK_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_TICK) == 0u,
           "repeated RTC current tick dispatch succeeds");
    uint64_t rtc2 = selftest_guest_u64(TICK_OUT);
    expect(rtc1 == rtc2, "repeated RTC queries do not advance the guest calendar");

    cpu.r[4] = TV_OUT;
    cpu.r[5] = TZ_OUT;
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_LIBC_GETTIMEOFDAY) == 0u,
           "gettimeofday dispatch succeeds");
    uint32_t tv_sec = MEM_R32(TV_OUT);
    uint32_t tv_usec = MEM_R32(TV_OUT + 4u);
    uint32_t libc_time_out = TV_OUT + 0x20u;
    cpu.r[4] = libc_time_out;
    uint32_t libc_time = sr_syscall(&cpu, NID_SCE_KERNEL_LIBC_TIME);
    expect(libc_time == MEM_R32(libc_time_out) && libc_time == tv_sec,
           "libc time and gettimeofday share the RTC/Unix epoch");
    expect(tv_usec < 1000000u && MEM_R32(TZ_OUT) == 0u && MEM_R32(TZ_OUT + 4u) == 0u,
           "gettimeofday emits a valid deterministic UTC timezone record");

    cpu.r[4] = DATE_EXPLICIT;
    cpu.r[5] = 60u; /* explicit PSP timezone offset in minutes */
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_CLOCK) == 0u,
           "explicit RTC current-clock NID is registered");
    cpu.r[4] = DATE_LOCAL;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_CLOCK_LOCAL) == 0u,
           "local RTC current-clock NID is registered");
    cpu.r[4] = DATE_EXPLICIT; cpu.r[5] = DATE_TICK_EXPLICIT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_TICK) == 0u,
           "explicit current-clock output is a valid RTC datetime");
    cpu.r[4] = DATE_LOCAL; cpu.r[5] = DATE_TICK_LOCAL;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_TICK) == 0u,
           "local current-clock output is a valid RTC datetime");
    expect(selftest_guest_u64(DATE_TICK_EXPLICIT) - selftest_guest_u64(DATE_TICK_LOCAL) == 3600000000ull,
           "explicit RTC offset is applied in checked microseconds");

    expect(s_vtime_us == before,
           "all repeated clock queries leave the scheduler timeline unchanged");

    cpu.r[4] = BAD_U64;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_TICK) == SCE_KERNEL_ERROR_ILLEGAL_ADDR,
           "RTC current tick rejects a complete-span overflow");
    cpu.r[4] = BAD_DATE;
    cpu.r[5] = 0u;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_CLOCK_LOCAL) == SCE_KERNEL_ERROR_ILLEGAL_ADDR,
           "RTC current clock rejects a complete datetime-span overflow");

    /* The host calendar is sampled once for the RTC epoch; changing the
     * deterministic guest time remains the only way these values can move. */
    s_vtime_us += 77u;
    cpu.r[4] = SYS_OUT;
    (void)sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME);
    expect(selftest_guest_u64(SYS_OUT) == before + 77u,
           "clock values advance only after an explicit scheduler-time advance");
    (void)rtc1; (void)rtc2;
}

/* Display clock regression through the production NID registry.  HCOUNT is a
 * scanout observation of the scheduler timeline: query count must not move it,
 * while an explicit guest-time advance must move both current and accumulated
 * positions.  The GE VBLANK bit shares the same rational frame phase, including
 * the interval just beyond the old integer-16667-us rollover. */
static void test_display_clock_reads_are_observational(void) {
    enum {
        NID_DISPLAY_CURRENT_HCOUNT = 0x773dd3a3u,
        NID_DISPLAY_ACCUMULATED_HCOUNT = 0x210eab3au,
        NID_DISPLAY_IS_VBLANK = 0x4d4e10ecu,
    };
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    s_vtime_us = 0;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    cpu.r[4] = 0;
    uint32_t current0 = sr_syscall(&cpu, NID_DISPLAY_CURRENT_HCOUNT);
    uint32_t accumulated0;
    cpu.r[4] = 0;
    accumulated0 = sr_syscall(&cpu, NID_DISPLAY_ACCUMULATED_HCOUNT);
    expect(current0 == 0u && accumulated0 == 0u,
           "display HCOUNT starts at the scheduler timeline origin");

    for (int i = 0; i < 64; i++) {
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_CURRENT_HCOUNT) == current0,
               "repeated current HCOUNT reads do not advance the scanline");
    }

    s_vtime_us = 1000u;
    cpu.r[4] = 0;
    uint32_t current1 = sr_syscall(&cpu, NID_DISPLAY_CURRENT_HCOUNT);
    cpu.r[4] = 0;
    uint32_t accumulated1 = sr_syscall(&cpu, NID_DISPLAY_ACCUMULATED_HCOUNT);
    expect(current1 > current0 && accumulated1 > accumulated0,
           "elapsed scheduler time advances current and accumulated HCOUNT");

    /* 16,670 us is still inside the rational 59.94-Hz frame.  The previous
     * 16,667-us modulo made this read look like the next frame and cleared the
     * VBLANK bit three microseconds early. */
    s_vtime_us = 16670u;
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_IS_VBLANK) == 1u,
           "display VBLANK uses the rational scheduler frame phase");
    s_vtime_us = 16684u;
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_IS_VBLANK) == 0u,
           "display VBLANK clears at the next rational frame");
}

static int s_delay_done;      /* set when the delay guest body returned */
static uint32_t s_delay_ret;  /* return code of sceKernelDelayThread */

/* Guest body for the delay test: enters sceKernelDelayThread through the real
 * NID inside its own coroutine (the production shape), then parks on the
 * scheduler.  The delay's own switch_to_scheduler() is a genuine child->main
 * switch here, so the coroutine-lifecycle invariant (no suppressed
 * self-switches) stays intact. */
static void delay_coro_body(void *arg) {
    (void)arg;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 1000u;   /* 1 ms */
    s_delay_ret = sr_syscall(&cpu, NID_SCE_KERNEL_DELAY_THREAD);
    s_delay_done = 1;
    selftest_park_on_scheduler();
}

/* Production-path clock regression: a controlled scheduler delay advances the
 * unified microsecond timeline.  A RUNNING thread parks through the real
 * sceKernelDelayThread NID, the scheduler charges exactly the delay through its
 * public HLE time-charge API (sr_hle_advance_time, the turbo-mode wait path),
 * and system time, the RTC tick, and the display scan each observe the result
 * in their own domains -- mirroring the PSP-3001/6.61-ARK measurements on
 * issue #80 (system time and RTC advance ~1 us/us over a 1 ms delay; the
 * display counter does not move inside the window). */
static void test_delay_advances_unified_timeline(void) {
    enum { SYS_OUT = 0x00200000u, TICK_OUT = 0x00200010u };
    reset_fixture();
    sr_hle_init();
    sr_hle_test_reset_rtc_epoch();   /* re-anchor the RTC epoch at our timeline */
    s_pace_on = 0;   /* deterministic: no host-clock dependence */
    TCB *self = fixture_thread(0x1d0u, TH_RUNNING, 32);
    s_cur = (int)(self - s_tcb);
    self->started = 1;
    s_vtime_us = 5000u;
    /* Next VBLANK far away: like the hardware window, a 1 ms delay must not
     * move the display counter (59.94 Hz period ~16.7 ms). */
    s_vbl_next_us = 100000u;
    s_vbl_event_period_rem = 0;
    s_delay_done = 0;
    s_delay_ret = 0xFFFFFFFFu;

    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    uint64_t before = s_vtime_us;

    cpu.r[4] = TICK_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_TICK) == 0u,
           "pre-delay RTC tick query succeeds");
    uint64_t tick_before = selftest_guest_u64(TICK_OUT);
    cpu.r[4] = 0;
    uint32_t hc_before = sr_syscall(&cpu, 0x773dd3a3u);   /* sceDisplayGetCurrentHcount */
    cpu.r[4] = 0;
    uint32_t vc_before = sr_syscall(&cpu, 0x9c6eaad7u);   /* sceDisplayGetVcount */

    self->coro = sr_coro_create(delay_coro_body, NULL, (size_t)4 << 20);
    expect(self->coro != NULL, "delay thread coroutine created");
    if (self->coro) sr_coro_switch(self->coro);

    expect(s_delay_done == 0, "guest body is still parked inside the delay syscall");
    expect(self->state == TH_WAIT_DELAY && self->wake == before + 1000u,
           "delay arm uses the unified deadline = vtime + usec");

    sr_hle_advance_time(1000u);   /* elapse exactly the controlled delay */
    expect(sched_vtime_us() == before + 1000u,
           "scheduler time advanced by exactly the controlled delay");

    /* Clock observations while the guest is still parked. */
    cpu.r[4] = SYS_OUT;
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME) == 0u &&
           selftest_guest_u64(SYS_OUT) == before + 1000u,
           "GetSystemTime tracks the elapsing delay exactly");
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME_LOW) ==
               (uint32_t)(before + 1000u),
           "GetSystemTimeLow shares the microsecond timeline");
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_LIBC_CLOCK) == (uint32_t)(before + 1000u),
           "libc clock follows the same scheduler microseconds");
    cpu.r[4] = TICK_OUT;
    (void)sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_TICK);
    expect(selftest_guest_u64(TICK_OUT) - tick_before == 1000u,
           "RTC tick advances 1 us per us of scheduler time (same domain)");

    /* Display domain: same timeline, separate 59.94-Hz/286-line scan scale. */
    cpu.r[4] = 0;
    uint32_t hc_after = sr_syscall(&cpu, 0x773dd3a3u);
    expect(hc_before == 85u && hc_after == 102u,
           "display HCOUNT advances on its rational scan scale (85 -> 102 over 1 ms)");
    cpu.r[4] = 0;
    uint32_t vc_after = sr_syscall(&cpu, 0x9c6eaad7u);
    expect(vc_after == vc_before,
           "VCOUNT is frozen inside the sub-frame window");
    expect(pick_next() == (int)(self - s_tcb),
           "expired delay promotes the parked thread on the next scheduler pick");

    /* Resume the parked guest: the syscall completes and returns 0. */
    s_cur = (int)(self - s_tcb);
    if (self->coro) sr_coro_switch(self->coro);
    expect(s_delay_done == 1 && s_delay_ret == 0u,
           "sceKernelDelayThread returns 0 after the delay elapses");
    if (self->coro) {
        sr_coro_destroy(self->coro);
        self->coro = NULL;
    }
    s_cur = -1;
}

/* sceDisplayGetFramePerSec returns a single-precision float through $f0.  The
 * integer return convention reports success in $v0, so a guest reading the
 * float must see the measured 60000/1001 bits -- never stale/poisoned state
 * (issue #80 display-clock campaign). */
static void test_display_frame_per_sec_float_return(void) {
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.fi[0] = 0xDEADBEEFu;   /* poison the register the handler must overwrite */

    expect(sr_syscall(&cpu, NID_DISPLAY_FRAME_PER_SEC) == 0u,
           "FramePerSec dispatch reports success in $v0");
    expect(cpu.fi[0] == 0x426fc29fu,
           "FramePerSec writes the 60000/1001 float bits (59.9400599f) in $f0");
    cpu.fi[0] = 0u;
    expect(sr_syscall(&cpu, NID_DISPLAY_FRAME_PER_SEC) == 0u &&
           cpu.fi[0] == 0x426fc29fu,
           "repeated FramePerSec reads are stable");
    /* The rate is a constant of the display domain: advancing guest time must
     * not change it, and it must stay coherent with the scan model. */
    s_vtime_us = 16670u;
    cpu.fi[0] = 0u;
    (void)sr_syscall(&cpu, NID_DISPLAY_FRAME_PER_SEC);
    expect(cpu.fi[0] == 0x426fc29fu,
           "FramePerSec stays coherent with the rational frame phase");
}

/* RTC conversion error codes, output-mutation behavior, and full-range tick
 * conversion, all pinned to PSPAutotests tests/rtc (convert.expected,
 * arithmetic.expected, rtc.expected). */
static void test_rtc_conversion_errors_and_full_range(void) {
    enum {
        DATE = 0x00200200u,
        TICK_OUT = 0x00200210u,
        FT_OUT = 0x00200220u,
    };
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    s_vtime_us = 0;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    /* ---- sceRtcGetTick: invalid year -> 0x800001fe, output untouched ---- */
    MEM_W16(DATE + 0u, 0u); MEM_W16(DATE + 2u, 1u); MEM_W16(DATE + 4u, 1u);
    MEM_W16(DATE + 6u, 0u); MEM_W16(DATE + 8u, 0u); MEM_W16(DATE + 10u, 0u);
    MEM_W32(DATE + 12u, 0u);
    MEM_W32(TICK_OUT, 0xDEADBEEFu); MEM_W32(TICK_OUT + 4u, 0xDEADBEEFu);
    cpu.r[4] = DATE; cpu.r[5] = TICK_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_TICK) == 0x800001feu,
           "GetTick year 0 reports INVALID_VALUE");
    expect(selftest_guest_u64(TICK_OUT) == 0xDEADBEEFDEADBEEFull,
           "GetTick failure leaves the output tick untouched");

    MEM_W16(DATE + 0u, 10000u);
    MEM_W32(TICK_OUT, 0xDEADBEEFu); MEM_W32(TICK_OUT + 4u, 0xDEADBEEFu);
    cpu.r[4] = DATE; cpu.r[5] = TICK_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_TICK) == 0x800001feu,
           "GetTick year 10000 reports INVALID_VALUE");
    expect(selftest_guest_u64(TICK_OUT) == 0xDEADBEEFDEADBEEFull,
           "GetTick overflow failure leaves the output untouched");

    /* ---- valid round trip: 2012-09-20 07:12:15.500 ---- */
    MEM_W16(DATE + 0u, 2012u); MEM_W16(DATE + 2u, 9u); MEM_W16(DATE + 4u, 20u);
    MEM_W16(DATE + 6u, 7u); MEM_W16(DATE + 8u, 12u); MEM_W16(DATE + 10u, 15u);
    MEM_W32(DATE + 12u, 500u);
    cpu.r[4] = DATE; cpu.r[5] = TICK_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_TICK) == 0u,
           "GetTick accepts a valid date");
    expect(selftest_guest_u64(TICK_OUT) == 63483721935000500ull,
           "GetTick converts the valid date to the proleptic Gregorian tick");
    cpu.r[4] = DATE; cpu.r[5] = TICK_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_SET_TICK) == 0u,
           "SetTick accepts the converted tick");
    expect(MEM_R16(DATE + 0u) == 2012u && MEM_R16(DATE + 2u) == 9u &&
           MEM_R16(DATE + 4u) == 20u && MEM_R16(DATE + 6u) == 7u &&
           MEM_R16(DATE + 8u) == 12u && MEM_R16(DATE + 10u) == 15u &&
           MEM_R32(DATE + 12u) == 500u,
           "tick/date round trip is exact");

    /* ---- sceRtcSetTick covers the full u64 range (arithmetic.expected) ---- */
    uint64_t wrap_tick = 315537897698999999ull;   /* 9999-12-31 23:59:59.999998 + 1us */
    MEM_W32(TICK_OUT, (uint32_t)wrap_tick); MEM_W32(TICK_OUT + 4u, (uint32_t)(wrap_tick >> 32));
    cpu.r[4] = DATE; cpu.r[5] = TICK_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_SET_TICK) == 0u,
           "SetTick accepts a tick beyond year 9999");
    expect(MEM_R16(DATE + 0u) == 10000u && MEM_R16(DATE + 2u) == 1u &&
           MEM_R16(DATE + 4u) == 1u && MEM_R16(DATE + 6u) == 0u &&
           MEM_R16(DATE + 8u) == 1u && MEM_R16(DATE + 10u) == 38u &&
           MEM_R32(DATE + 12u) == 999999u,
           "SetTick writes the wrapped date with natural field truncation");

    /* ---- sceRtcGetWin32FileTime: 2005-11-31 accepted, 1600 rejected ---- */
    MEM_W16(DATE + 0u, 2005u); MEM_W16(DATE + 2u, 11u); MEM_W16(DATE + 4u, 31u);
    MEM_W16(DATE + 6u, 13u); MEM_W16(DATE + 8u, 1u); MEM_W16(DATE + 10u, 0u);
    MEM_W32(DATE + 12u, 1u);
    MEM_W32(FT_OUT, 0xFFFFFFFFu); MEM_W32(FT_OUT + 4u, 0xFFFFFFFFu);
    cpu.r[4] = DATE; cpu.r[5] = FT_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_WIN32_FILETIME) == 0u,
           "GetWin32FileTime accepts a carry date (2005-11-31)");
    expect(selftest_guest_u64(FT_OUT) == 127779156600000010ull,
           "GetWin32FileTime matches the measured 2005-11-31 conversion");

    MEM_W16(DATE + 0u, 1600u); MEM_W16(DATE + 2u, 1u); MEM_W16(DATE + 4u, 1u);
    MEM_W16(DATE + 6u, 0u); MEM_W16(DATE + 8u, 0u); MEM_W16(DATE + 10u, 0u);
    MEM_W32(DATE + 12u, 0u);
    MEM_W32(FT_OUT, 0xFFFFFFFFu); MEM_W32(FT_OUT + 4u, 0xFFFFFFFFu);
    cpu.r[4] = DATE; cpu.r[5] = FT_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_WIN32_FILETIME) == 0x800001feu,
           "GetWin32FileTime pre-1601 reports INVALID_VALUE");
    expect(selftest_guest_u64(FT_OUT) == 0ull,
           "GetWin32FileTime writes 0 on failure (convert.expected)");

    /* ---- GetCurrentClock accepts the full int32 offset range ---- */
    cpu.r[4] = DATE;
    cpu.r[5] = (uint32_t)(int32_t)-2147483647;   /* -INT_MAX minutes */
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_CLOCK) == 0u,
           "GetCurrentClock returns success for -INT_MAX minutes (rtc.expected)");
    cpu.r[4] = DATE;
    cpu.r[5] = 2147483647u;                       /* +INT_MAX minutes */
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_CLOCK) == 0u,
           "GetCurrentClock returns success for INT_MAX minutes (rtc.expected)");
    cpu.r[4] = DATE;
    cpu.r[5] = (uint32_t)(int32_t)-600000;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_CLOCK) == 0u,
           "GetCurrentClock returns success for -600000 minutes (rtc.expected)");
}

/* #80 bulk-stability regression: 10000 reads of every guest-visible time API
 * at one unchanged emulated timestamp must be side-effect free -- identical
 * values and the scheduler timeline left exactly where it was.  Time moves
 * only through scheduler progression (sr_hle_advance_time, yield/idle
 * boundaries, explicit fixture advances), never through a clock query.  The
 * RTC/Unix epoch is anchored once; every read after that is guest-time
 * arithmetic on the same microsecond timeline. */
static void test_bulk_clock_reads_are_side_effect_free(void) {
    enum {
        TICK_OUT = 0x00200300u,
        TV_OUT = 0x00200310u,
        TZ_OUT = 0x00200320u,
    };
    reset_fixture();
    sr_hle_init();
    sr_hle_test_reset_rtc_epoch();
    s_pace_on = 0;
    s_vtime_us = 424242u;
    uint64_t before = s_vtime_us;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    cpu.r[4] = TICK_OUT;
    expect(sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_TICK) == 0u,
           "RTC current tick anchors the fixture epoch");
    uint64_t rtc0 = selftest_guest_u64(TICK_OUT);
    uint32_t low0 = sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME_LOW);
    uint32_t clock0 = sr_syscall(&cpu, NID_SCE_KERNEL_LIBC_CLOCK);
    cpu.r[4] = TV_OUT;
    cpu.r[5] = TZ_OUT;
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_LIBC_GETTIMEOFDAY) == 0u,
           "gettimeofday baseline read succeeds");
    uint32_t tv_sec0 = MEM_R32(TV_OUT);
    uint32_t tv_usec0 = MEM_R32(TV_OUT + 4u);

    for (int i = 0; i < 10000; i++) {
        cpu.r[4] = TICK_OUT;
        (void)sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_TICK);
        expect(selftest_guest_u64(TICK_OUT) == rtc0,
               "10000 RTC current-tick reads return the identical calendar value");
        expect(sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME_LOW) == low0,
               "10000 GetSystemTimeLow reads are stable");
        expect(sr_syscall(&cpu, NID_SCE_KERNEL_LIBC_CLOCK) == clock0,
               "10000 libc clock reads are stable");
        cpu.r[4] = TV_OUT;
        cpu.r[5] = TZ_OUT;
        (void)sr_syscall(&cpu, NID_SCE_KERNEL_LIBC_GETTIMEOFDAY);
        expect(MEM_R32(TV_OUT) == tv_sec0 && MEM_R32(TV_OUT + 4u) == tv_usec0,
               "10000 gettimeofday reads return the identical Unix time");
    }
    expect(s_vtime_us == before,
           "10000 reads of every clock leave the scheduler timeline untouched");

    /* Relative coherence: one known scheduler advance moves system time and the
     * RTC tick by exactly the same microsecond delta (same domain, same rate). */
    s_vtime_us += 5000u;
    cpu.r[4] = TICK_OUT;
    (void)sr_syscall(&cpu, NID_SCE_RTC_GET_CURRENT_TICK);
    expect(selftest_guest_u64(TICK_OUT) - rtc0 == 5000u,
           "RTC tick advances 1 us per us of scheduler time (same domain)");
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME_LOW) ==
               (uint32_t)(before + 5000u),
           "system time advances by the identical delta");
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_LIBC_CLOCK) == (uint32_t)(before + 5000u),
           "libc clock advances by the identical delta");
}

/* #80 display-domain independence: display progression is owned by display
 * state (the scheduler's rational scan phase plus elapsed display periods),
 * never by the number of queries.  1000 reads of every display clock at one
 * unchanged emulated timestamp must return identical values, leave s_vtime_us
 * untouched, and not deliver a vblank; VCOUNT advances through the source
 * accounting seam as periods elapse, not per delivered vblank. */
static void test_display_queries_do_not_progress_display(void) {
    enum {
        NID_DISPLAY_CURRENT_HCOUNT = 0x773dd3a3u,
        NID_DISPLAY_IS_VBLANK = 0x4d4e10ecu,
        NID_DISPLAY_VCOUNT = 0x9c6eaad7u,
    };
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    s_vtime_us = 5000u;              /* mid-frame: outside the vblank window */
    s_vbl_next_us = 1000000u;        /* next VBLANK source event far away */
    s_vbl_event_period_rem = 0;
    s_vbl_count = 0;
    uint64_t before = s_vtime_us;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    cpu.r[4] = 0;
    uint32_t hc0 = sr_syscall(&cpu, NID_DISPLAY_CURRENT_HCOUNT);
    cpu.r[4] = 0;
    uint32_t vb0 = sr_syscall(&cpu, NID_DISPLAY_IS_VBLANK);
    cpu.r[4] = 0;
    uint32_t vc0 = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);

    for (int i = 0; i < 1000; i++) {
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_CURRENT_HCOUNT) == hc0,
               "1000 current-HCOUNT reads do not advance the scanline");
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_IS_VBLANK) == vb0,
               "1000 vblank-phase reads are stable");
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0,
               "1000 VCOUNT reads do not deliver a vblank");
        cpu.fi[0] = 0u;
        expect(sr_syscall(&cpu, NID_DISPLAY_FRAME_PER_SEC) == 0u &&
               cpu.fi[0] == 0x426fc29fu,
               "1000 FramePerSec reads return the constant float rate");
    }
    expect(s_vtime_us == before,
           "display queries never advance the scheduler timeline");
    expect(s_vbl_count == 0u,
           "no VBLANK source event was latched by the reads");

    /* Display progression: VCOUNT is source-driven, not delivery-driven.  The
     * service tick performs framebuffer/interrupt work but does not move VCOUNT;
     * only the scheduler source latch advances it by the crossed periods. */
    sr_vblank_tick();
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0,
           "a delivered service tick does not advance VCOUNT");

    s_vbl_next_us = s_vtime_us;      /* place the next boundary at the current instant */
    s_vbl_event_period_rem = 0;
    s_vtime_us = 15000u;             /* 10 ms later: still inside the first period */
    scheduler_latch_due_events();
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0 + 1u,
           "crossing one period boundary advances VCOUNT through the source seam");
    cpu.r[4] = 0;
    uint32_t hc1 = sr_syscall(&cpu, NID_DISPLAY_CURRENT_HCOUNT);
    expect(hc1 != hc0,
           "HCOUNT follows the elapsed display phase, not the query count");

    /* The new state is again read-only: further reads change nothing. */
    for (int i = 0; i < 100; i++) {
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0 + 1u,
               "post-progression VCOUNT reads stay stable");
    }
}

/* Guest-visible VCOUNT advances by elapsed display periods at scheduler
 * source-latch boundaries, decoupled from VBLANK service -- it is not a count
 * of delivered/serviced VBLANK episodes and is not described as strictly
 * free-running.  A provisional PSP observation corroborates the service-
 * independence direction, but it does not establish the runtime's exact rate;
 * this checked-in regression is HOST_TESTED source-contract evidence.
 *
 * This is the public failing-before regression for that boundary, exercised
 * through the production scheduler_latch_due_events / scheduler_service_pending
 * path (not HST, not the hardware probe).  For each N the source deadline is
 * advanced across exactly N periods before service; VCOUNT must reflect V+N,
 * while delivered episodes stay at most one. */
static void test_vcount_tracks_elapsed_source_periods(void) {
    enum {
        NID_DISPLAY_VCOUNT = 0x9c6eaad7u,
    };
    const unsigned periods[] = { 1u, 2u, 3u, 10u };
    for (unsigned pi = 0; pi < sizeof(periods) / sizeof(periods[0]); pi++) {
        const unsigned N = periods[pi];
        reset_fixture();
        sr_hle_init();
        s_pace_on = 0;
        s_vbl_next_us = 0;
        s_vbl_event_period_rem = 0;
        s_vbl_count = 0;
        s_interrupts_enabled = 1;
        CpuState cpu;
        memset(&cpu, 0, sizeof(cpu));

        cpu.r[4] = 0;
        uint32_t vc0 = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);

        /* Reach the boundary of N-1 completed periods from the origin, so the
         * latch's exact rational carry math advances exactly N period
         * boundaries before any service. */
        s_vtime_us = (uint64_t)scheduler_vblank_delta(N - 1u, 0u, NULL);
        scheduler_latch_due_events();

        char msg[96];
        snprintf(msg, sizeof msg, "N=%u: VCOUNT advances by the elapsed source periods", N);
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0 + N, msg);
        snprintf(msg, sizeof msg, "N=%u: no delivery before the eligible service phase", N);
        expect(s_vbl_count == 0u, msg);
        expect((s_pending_interrupts & SCHED_INTR_VBLANK) != 0,
               "a burst of periods coalesces into one pending source bit");

        /* Service once: one delivered episode, VCOUNT unchanged. */
        scheduler_service_pending();
        snprintf(msg, sizeof msg, "N=%u: one serviced episode regardless of the burst", N);
        expect(s_vbl_count == 1u, msg);
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0 + N,
               "service does not re-advance guest VCOUNT");
        expect((s_pending_interrupts & SCHED_INTR_VBLANK) == 0u,
               "service clears the coalesced source bit");
    }

    /* Pending bit already set before the latch: the latch must not manufacture
     * a second episode; VCOUNT still advances by the crossed periods. */
    {
        reset_fixture();
        sr_hle_init();
        s_pace_on = 0;
        s_vbl_next_us = 0;
        s_vbl_event_period_rem = 0;
        s_vbl_count = 0;
        s_interrupts_enabled = 1;
        CpuState cpu;
        memset(&cpu, 0, sizeof(cpu));
        cpu.r[4] = 0;
        uint32_t vc0 = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);

        sched_raise_interrupt(SCHED_INTR_VBLANK);  /* already pending */
        s_vtime_us = (uint64_t)scheduler_vblank_delta(2u, 0u, NULL);
        scheduler_latch_due_events();
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0 + 3u,
               "a pre-set pending bit still lets VCOUNT track the crossed periods");
        expect(s_vbl_count == 0u, "a pre-set pending bit adds no delivery before service");
        scheduler_service_pending();
        expect(s_vbl_count == 1u, "a pre-set pending bit still coalesces to one episode");
    }

    /* Multiple deadline-latch calls before service: each latch contributes its
     * own period burst to VCOUNT and the delivery stays one episode. */
    {
        reset_fixture();
        sr_hle_init();
        s_pace_on = 0;
        s_vbl_next_us = 0;
        s_vbl_event_period_rem = 0;
        s_vbl_count = 0;
        s_interrupts_enabled = 1;
        CpuState cpu;
        memset(&cpu, 0, sizeof(cpu));
        cpu.r[4] = 0;
        uint32_t vc0 = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);

        s_vtime_us = (uint64_t)scheduler_vblank_delta(1u, 0u, NULL);  /* 2 periods */
        scheduler_latch_due_events();
        s_vtime_us = (uint64_t)scheduler_vblank_delta(4u, 0u, NULL);  /* 3 more */
        scheduler_latch_due_events();
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0 + 5u,
               "multiple latches accumulate VCOUNT (2 + 3 periods)");
        expect(s_vbl_count == 0u, "no delivery yet across multiple latches");
        expect((s_pending_interrupts & SCHED_INTR_VBLANK) != 0,
               "multiple latches still coalesce into one pending source");
        scheduler_service_pending();
        expect(s_vbl_count == 1u, "one delivery after multiple latches");
        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0 + 5u,
               "service leaves the accumulated VCOUNT alone");
    }
}

/* Guest VCOUNT and VBLANK delivery both freeze while the CPU interrupt bit is
 * clear.
 *
 * This is the second of the two regimes the runtime must represent. Qualified
 * PSP measurements held `sceKernelCpuSuspendIntr` across a long spin and found:
 *
 *   - "display vcount does NOT advance during suspension (vc_during == vc_before)"
 *   - "VBLANK handler calls freeze during CpuSuspendIntr window"
 *   - "system time DOES advance during suspension"
 *   - "exactly ONE pending VBLANK is delivered on CpuResumeIntr"
 *   - "one further VBLANK delivery occurs at the next real boundary after resume"
 *
 * A later probe (display-mask-vcount, PSP-3001 / 6.61-ARK, 12 trials at each of
 * 4 / 16.7 / 30 / 50 ms) took the sample the #88 record was missing -- VCOUNT
 * IMMEDIATELY after CpuResumeIntr -- and measured a credit of exactly one
 * whenever at least one source period had become pending.  The conservative
 * "no increment at resume" policy this test used to assert is therefore
 * retired; see test_vcount_credits_one_deferred_period_on_resume().
 *
 * The sibling test_vcount_tracks_elapsed_source_periods() covers the other
 * regime -- interrupts ENABLED, guest service starved -- where VCOUNT does
 * advance by elapsed periods.  Neither observation may be extrapolated onto the
 * other.  The single model that
 * satisfies both is that guest-visible VCOUNT is maintained by the VBLANK
 * interrupt path: it ticks per elapsed display period whenever interrupts are
 * enabled (independently of whether a guest thread ever services the episode),
 * and stops while they are masked.
 *
 * What this test still owns is the DURING-mask half: VCOUNT must not move while
 * the bit is clear, however many periods elapse.  The resume credit is asserted
 * by the sibling test. */
static void test_vcount_freezes_while_cpu_interrupts_are_masked(void) {
    enum {
        NID_DISPLAY_VCOUNT = 0x9c6eaad7u,
    };
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    s_vtime_us = 1000u;
    s_vbl_next_us = 16683u;        /* one rational 59.94-Hz frame away */
    s_vbl_event_period_rem = 0;
    s_vbl_count = 0;
    s_interrupts_enabled = 0;      /* interrupt-disabled period */
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    cpu.r[4] = 0;
    uint32_t vc0 = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);
    uint32_t sys0 = sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME_LOW);
    uint64_t vbl_next_before_mask = s_vbl_next_us;

    /* Cross the VBLANK source deadline while interrupts are disabled. */
    s_vtime_us = 30000u;
    scheduler_latch_due_events();
    scheduler_service_pending();   /* no-op: delivery is interrupt-gated */

    expect(sr_syscall(&cpu, NID_SCE_KERNEL_GET_SYSTEM_TIME_LOW) == 30000u,
           "system time continued through the interrupt-disabled period (#88: st_during != st_before)");
    /* #88: vc_during == vc_before.  A period boundary was crossed and the source
     * bit was latched, but the interrupt path that maintains guest VCOUNT never
     * ran, so the guest-visible counter is unchanged. */
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0,
           "guest VCOUNT freezes while the CPU interrupt bit is clear");
    expect(s_vbl_count == 0u,
           "VBLANK handler calls freeze while interrupts are disabled");
    expect((s_pending_interrupts & SCHED_INTR_VBLANK) != 0,
           "the VBLANK source stays latched/pending for the eligible phase");

    /* Several more periods elapse, still masked: they must coalesce, not
     * accumulate into VCOUNT, and must not queue N separate deliveries. */
    s_vtime_us = 30000u + 5u * 16683u;
    scheduler_latch_due_events();
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0,
           "further masked periods still do not advance guest VCOUNT");
    expect(s_vbl_count == 0u,
           "no delivery accrues for the additional masked periods");

    /* Re-enable through the production resume boundary.  That boundary samples
     * time before restoring the I-bit, so even a period first discovered here is
     * still classified as masked time.  Exactly one coalesced episode is
     * delivered and no synthetic VCOUNT catch-up is applied. */
    sched_resume_interrupts(1u);
    expect(s_vbl_count == 1u,
           "resume delivers exactly one coalesced VBLANK episode");
    cpu.r[4] = 0;
    /* HARDWARE_MEASURED: six masked periods, one credited increment.  The old
     * expectation here was == vc0, which the display-mask-vcount probe refutes. */
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0 + 1u,
           "resume credits exactly one deferred period, not zero and not six");

    /* SOURCE-PHASE CONTINUITY. Freezing the guest-visible counter must not corrupt
     * the display source underneath it. The deadline was advanced for every
     * masked period, so after resume the next boundary remains in the future and
     * no more than one period away. */
    expect(s_vbl_next_us > vbl_next_before_mask,
           "the display source deadline advanced across the masked window (phase not stalled)");
    /* The deadline must name the NEXT boundary exactly: still in the future (not stale, which
     * would fabricate frames on the next latch) and no more than one period ahead (not
     * over-advanced, which would swallow one). This is checked against absolute time, so it
     * cannot be satisfied by a wrong deadline the way a s_vbl_next_us-relative check could. */
    expect(s_vbl_next_us > s_vtime_us,
           "the masked latch left the source deadline in the future, not stale");
    expect(s_vbl_next_us - s_vtime_us <= 16684u,
           "the masked latch advanced the source deadline by no more than one period");
    uint32_t vc_resume = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);
    uint64_t next_boundary = s_vbl_next_us;

    /* (a) one microsecond short of the next boundary: nothing is due yet. */
    s_vtime_us = next_boundary - 1u;
    scheduler_latch_due_events();
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc_resume,
           "a sub-period step after resume crosses no boundary and does not advance VCOUNT");
    expect((s_pending_interrupts & SCHED_INTR_VBLANK) == 0u,
           "a sub-period step after resume raises no source event");
    scheduler_service_pending();
    expect(s_vbl_count == 1u,
           "a sub-period step after resume delivers nothing further");

    /* (b) exactly at the boundary: exactly one advance and one delivery. */
    s_vtime_us = next_boundary;
    scheduler_latch_due_events();
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc_resume + 1u,
           "the next genuine display-source boundary advances VCOUNT by exactly one");
    expect((s_pending_interrupts & SCHED_INTR_VBLANK) != 0,
           "the next genuine boundary raises exactly one source event");
    scheduler_service_pending();
    expect(s_vbl_count == 2u,
           "the next real boundary after resume delivers exactly one further episode");
    (void)sys0;
}


/* Case-B deferred VCOUNT accounting, and the discovery-time defect behind it.
 *
 * HARDWARE_MEASURED, PSP-3001 / 6.61-ARK, source-owned probe display-mask-vcount
 * (oracle/hardware-results/display-mask-accepted.json), 12 trials per duration:
 *
 *      mask     source periods crossed     VCOUNT credited at resume
 *      4.0 ms            0                          +0
 *     16.7 ms            1                          +1
 *     30.0 ms            1                          +1
 *     50.0 ms            2                          +1
 *
 * No trial at any duration showed an N-period catch-up, and none showed a credit
 * when no period had elapsed.  sceDisplayGetAccumulatedHcount kept advancing at
 * the full display rate throughout every mask, so the display SOURCE never stops
 * -- what stops is the interrupt-gated counter the guest reads.
 *
 * The fourth case is the host defect that made the first three matter.  Source
 * periods are discovered lazily at scheduler boundaries, and were classified by
 * the interrupt bit AT DISCOVERY TIME.  A period that elapsed with interrupts
 * enabled, but that no latch had noticed yet, was therefore re-classified as
 * masked by the next latch under a mask -- typically the one CpuResumeIntr runs
 * before restoring the bit -- and dropped.  Private route measurement found that
 * every dropped period had a boundary predating the mask that later discovered
 * it, while the mask itself was held for a negligible fraction of wall time: the
 * defect is classification at discovery time, not interrupt-mask residency.
 * (The matched before/after rate table for a specific title and route is run
 * evidence and lives with that run, not in this comment.)
 *
 * Every assertion goes through production NID dispatch on a fixture with a real
 * current thread, so none of them can pass vacuously. */
static void test_vcount_credits_one_deferred_period_on_resume(void) {
    enum { NID_DISPLAY_VCOUNT = 0x9c6eaad7u };
    const uint64_t PERIOD = 16683u;
    CpuState cpu;

    /* One masked window crossing `periods` source boundaries; returns the VCOUNT
     * delta observed across the whole suspend/resume pair, and reports the delta
     * seen while still masked through `during`. */
    struct { uint64_t periods; uint32_t expect_credit; const char *what; } cases[] = {
        { 0u, 0u, "no period crosses the mask: resume credits nothing" },
        { 1u, 1u, "one period crosses the mask: resume credits exactly one" },
        { 2u, 1u, "two periods coalesce: resume still credits exactly one" },
        { 5u, 1u, "five periods coalesce: resume still credits exactly one" },
    };

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        reset_fixture();
        sr_hle_init();
        s_pace_on = 0;
        s_vtime_us = 1000u;
        s_vbl_next_us = 1000u + PERIOD;
        s_vbl_event_period_rem = 0;
        s_vbl_count = 0;
        s_interrupts_enabled = 1;
        memset(&cpu, 0, sizeof(cpu));

        cpu.r[4] = 0;
        uint32_t vc0 = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);

        (void)sched_suspend_interrupts();
        expect(sched_interrupts_enabled() == 0, "the mask is actually held");

        s_vtime_us += cases[c].periods * PERIOD;
        scheduler_latch_due_events();

        cpu.r[4] = 0;
        expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == vc0,
               "VCOUNT does not advance while the CPU interrupt bit is clear");

        sched_resume_interrupts(1u);
        cpu.r[4] = 0;
        uint32_t credited = sr_syscall(&cpu, NID_DISPLAY_VCOUNT) - vc0;
        expect(credited == cases[c].expect_credit, cases[c].what);
    }

    /* THE HOST DEFECT. A period elapses with interrupts ENABLED but is not
     * latched yet; the guest then masks and unmasks without any further period
     * crossing.  That period was delivered on hardware and must survive.
     *
     * Before the fix this asserted 0: the pre-restore latch inside
     * sched_resume_interrupts() discovered the period with the bit still clear
     * and dropped it. */
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    s_vtime_us = 1000u;
    s_vbl_next_us = 1000u + PERIOD;
    s_vbl_event_period_rem = 0;
    s_vbl_count = 0;
    s_interrupts_enabled = 1;
    memset(&cpu, 0, sizeof(cpu));

    cpu.r[4] = 0;
    uint32_t base = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);

    /* Three periods elapse with interrupts enabled.  Advance the clock WITHOUT
     * latching, exactly as a stretch of guest execution between two scheduler
     * boundaries does. */
    s_vtime_us += 3u * PERIOD;

    (void)sched_suspend_interrupts();
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == base + 3u,
           "periods that elapsed before the mask are credited when the mask is taken");
    expect(s_vbl_count == 0u,
           "consuming pre-mask periods does not run a guest handler from inside SuspendIntr");

    sched_resume_interrupts(1u);
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == base + 3u,
           "and resume adds no credit of its own when no period elapsed under the mask");

    /* The same shape one more time, now with a genuine masked period on top: the
     * three pre-mask periods and the one masked period must both be counted, and
     * the masked one exactly once. */
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    s_vtime_us = 1000u;
    s_vbl_next_us = 1000u + PERIOD;
    s_vbl_event_period_rem = 0;
    s_vbl_count = 0;
    s_interrupts_enabled = 1;
    memset(&cpu, 0, sizeof(cpu));

    cpu.r[4] = 0;
    base = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);
    s_vtime_us += 3u * PERIOD;
    (void)sched_suspend_interrupts();
    s_vtime_us += 2u * PERIOD;
    scheduler_latch_due_events();
    sched_resume_interrupts(1u);
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == base + 4u,
           "three enabled periods plus two coalesced masked periods credit 3 + 1");

    /* CpuResumeIntr(0) taken from an enabled state is also an enabled->disabled
     * edge -- the #88 token algebra proves the token is literally the saved
     * I-bit -- so it must consume pre-mask periods too. */
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    s_vtime_us = 1000u;
    s_vbl_next_us = 1000u + PERIOD;
    s_vbl_event_period_rem = 0;
    s_vbl_count = 0;
    s_interrupts_enabled = 1;
    memset(&cpu, 0, sizeof(cpu));

    cpu.r[4] = 0;
    base = sr_syscall(&cpu, NID_DISPLAY_VCOUNT);
    s_vtime_us += 2u * PERIOD;
    sched_resume_interrupts(0u);
    expect(sched_interrupts_enabled() == 0, "resume(0) from enabled really masks");
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_VCOUNT) == base + 2u,
           "resume(0) from an enabled state also consumes the pre-mask periods");
}


/* The route observer must not read the scanout before the guest owns it.
 *
 * s_display_active carries compile-time initializers {0x04000000, 512, fmt 3}
 * from process start.  The GE routinely registers a render target at that same
 * address, in whatever pixel format the display list asked for, BEFORE the guest
 * makes its first sceDisplaySetFrameBuf call.  An observer that trusts the
 * initializer then does two wrong things at once: it would decode the buffer with
 * a format the guest never selected, and it hands that placeholder descriptor to
 * gegpu_sync_guest_fb(), whose format cross-check is there to report genuine
 * disagreements.  On a real boot that produced
 *
 *   gegpu: snapshot sync refused: target at 0x04000000 has stride=512 fmt=1
 *          but caller described stride=512 fmt=3
 *
 * which reads exactly like a coherence defect and is not one -- the guest simply
 * had not configured anything yet.  Its first SetFrameBuf then requested format 1,
 * agreeing with the target, and no further refusal occurred all run.
 *
 * The rule this pins: no observation, and no descriptor handed to the coherence
 * boundary, until a COMPLETE guest-provided scanout state has been applied.  A
 * latched (sync=1) request is not enough on its own -- PSP publishes its stride
 * and format immediately but keeps the previous scanout address until the latch
 * lands at VBLANK, so between those two moments s_display_active is a mixture of
 * guest format and placeholder address.
 *
 * The coherence boundary itself is deliberately NOT relaxed; that it still refuses
 * a genuine format or stride mismatch against a live target is asserted separately
 * by gpu-coherence-selftest, which has the real Vulkan target this build lacks. */
static void test_route_observer_waits_for_guest_scanout_state(void) {
    static const uint32_t NID_DISPLAY_SET_FRAME_BUF = 0x289d82feu;
    static const uint32_t VRAM_B = 0x04044000u;
    uint8_t sig[4096];
    CpuState cpu;

    reset_fixture();
    sr_hle_init();
    memset(&cpu, 0, sizeof(cpu));
    expect(sr_route_sig_bytes() > 0 && (size_t)sr_route_sig_bytes() <= sizeof(sig),
           "the default observer grid fits the local signature buffer");

    /* (a) Nothing configured yet.  This is the state every run passes through. */
    g_sync_calls = 0;
    expect(sr_route_test_sample(sig) == 0,
           "the observer declines to sample before the guest configures scanout");
    expect(g_sync_calls == 0,
           "and never hands the coherence boundary a placeholder descriptor");

    /* (b) A latched request publishes stride and format at once, but the scanout
     * address is still the placeholder until VBLANK applies the latch.  The
     * observer must keep declining across that window. */
    cpu.r[4] = VRAM_B; cpu.r[5] = 512; cpu.r[6] = 1; cpu.r[7] = 1;   /* sync = 1 */
    expect(sr_syscall(&cpu, NID_DISPLAY_SET_FRAME_BUF) == 0u,
           "a latched SetFrameBuf that changes format is accepted");
    expect(sr_route_test_sample(sig) == 0,
           "a latched request alone does not make the scanout state observable");
    expect(g_sync_calls == 0,
           "and still nothing reaches the coherence boundary");

    /* (c) VBLANK applies the latch: address, stride and format are now all the
     * guest's.  Observation resumes, and synchronises against exactly those. */
    sr_vblank_tick();
    expect(sr_route_test_sample(sig) == 1,
           "observation resumes once the guest owns the complete scanout state");
    expect(g_sync_calls == 1,
           "the resumed observation synchronises exactly once");
    expect(g_sync_last.addr == VRAM_B,
           "and synchronises the guest's own scanout address");
    expect(g_sync_last.stride == 512u,
           "with the guest's own stride");
    expect(g_sync_last.format == 1u,
           "with the guest's own pixel format (1), not the initializer's 3");
    expect(g_sync_last.width == 480u && g_sync_last.height == 272u,
           "and the full visible extent");
}

/* ---- extracted-data census preparation seam ---------------------------------
 *
 * The cold extracted-data index census must never begin from a guest HLE call.
 * The historical lazy data_init() inside h_IoOpen ran the whole SR_DATAROOT
 * filesystem walk synchronously on the single guest-scheduler OS thread; under
 * host filesystem contention every guest thread/yield/VBLANK starved for
 * seconds and the VCOUNT catch-up afterwards tripped SR_EXIT_AT_VBLANK.
 *
 * Production order under test: sr_host_data_prepare() runs once BEFORE any
 * guest execution (driver.c), reaching a TERMINAL state -- READY (index
 * published), FAILED (attempted census failed), or DISABLED (route not
 * applicable: no operator SR_DATAROOT and this profile declares no expected
 * census). Guest-time lookups consume terminal states only; a non-terminal
 * observation refuses with ONE bounded diagnostic and never enumerates.
 *
 * These tests drive the REAL sr_host_data_prepare / host_data_lookup /
 * data_walk through the real production h_IoOpen entry. */
#define SR_DATA_TEST_STATE_UNINITIALIZED 0
#define SR_DATA_TEST_STATE_INITIALIZING  1
#define SR_DATA_TEST_STATE_READY         2
#define SR_DATA_TEST_STATE_FAILED        3
#define SR_DATA_TEST_STATE_DISABLED      4

static char s_prewarm_root[MAX_PATH];

static void prewarm_env_restore(void) {
    SetEnvironmentVariableA("SR_DATAROOT", NULL);
}

/* Build a fresh source-owned fixture tree <cwd>/build/data_prewarm_<pid> with
 * one servable asset at data/menu/text/common.to. Returns 1 on success. */
static int prewarm_make_fixture(int with_asset) {
    char cwd[MAX_PATH];
    if (!GetCurrentDirectoryA(MAX_PATH, cwd)) return 0;
    CreateDirectoryA("build", NULL);
    snprintf(s_prewarm_root, sizeof s_prewarm_root, "%s\\build\\data_prewarm_%lu",
             cwd, (unsigned long)GetCurrentProcessId());
    /* A stale fixture from an earlier run must not inflate the counts. */
    char clean_root[MAX_PATH];
    snprintf(clean_root, sizeof clean_root, "\\\\?\\%s", s_prewarm_root);
    /* Best-effort removal of a previous run's deepest file only. */
    {
        char old_file[MAX_PATH];
        snprintf(old_file, sizeof old_file, "\\\\?\\%s\\data\\menu\\text\\common.to",
                 s_prewarm_root);
        DeleteFileA(old_file);
    }
    (void)clean_root;
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\data\\menu\\text", s_prewarm_root);
    if (!(CreateDirectoryA(s_prewarm_root, NULL) || GetLastError() == ERROR_ALREADY_EXISTS))
        return 0;
    snprintf(dir, sizeof dir, "%s\\data", s_prewarm_root);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, sizeof dir, "%s\\data\\menu", s_prewarm_root);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, sizeof dir, "%s\\data\\menu\\text", s_prewarm_root);
    CreateDirectoryA(dir, NULL);
    if (with_asset) {
        char file[MAX_PATH];
        snprintf(file, sizeof file, "%s\\common.to", dir);
        FILE *f = fopen(file, "wb");
        if (!f) return 0;
        fputs("common", f);
        fclose(f);
    }
    return 1;
}

static uint32_t prewarm_open_common(CpuState *cpu) {
    static const uint32_t path_addr = 0x09100000u;
    memset(cpu, 0, sizeof *cpu);
    cpu->r[4] = path_addr;
    /* disc0:/<rel> exercises the same device-strip + binary-search shape the
     * extracted-data route serves in production. (The combined PSP_GAME/ +
     * USRDIR/ double-strip has a pre-existing leading-slash quirk documented
     * since PR #108; it is unchanged here and out of scope.) */
    const char *path = "disc0:/data/menu/text/common.to";
    for (unsigned i = 0;; i++) {
        MEM_W8(path_addr + i, (uint8_t)path[i]);
        if (!path[i]) break;
    }
    return sr_hle_test_io_open(cpu);
}

/* 1. The applicable route reaches READY synchronously during preparation; after
 *    the guest-start boundary the production open serves the fixture asset,
 *    and NO census attempt happens after that boundary. */
static void test_extracted_data_prepares_before_guest_and_lookup_never_builds(void) {
    CpuState cpu;
    reset_fixture();
    sr_hle_init();
    expect(prewarm_make_fixture(1), "the source-owned data fixture was created");
    SetEnvironmentVariableA("SR_DATAROOT", s_prewarm_root);
    sr_hle_test_data_reset(0);

    expect(sr_hle_test_data_state() == SR_DATA_TEST_STATE_UNINITIALIZED,
           "a reset route starts UNINITIALIZED");
    int state = sr_host_data_prepare();
    expect(state == SR_DATA_TEST_STATE_READY,
           "an applicable route reaches READY during preparation");
    expect(sr_hle_test_data_entry_count() == 1 && sr_host_data_entry_count() == 1,
           "the published index carries exactly the fixture asset");
    unsigned long walks_before = sr_hle_test_data_walk_calls();
    expect(walks_before >= 4u,
           "the census enumerated the fixture directories exactly during preparation");

    sr_hle_test_data_mark_guest_start();
    uint32_t fd = prewarm_open_common(&cpu);
    expect(fd >= 3u && fd < 64u,
           "after guest start the production open serves the prepared asset");
    expect(sr_hle_test_data_builds_after_guest() == 0,
           "guest time never begins a cold census");
    expect(sr_hle_test_data_build_attempts() == 1,
           "preparation attempted the census exactly once");
    expect(sr_hle_test_data_walk_calls() == walks_before,
           "the served lookup enumerated nothing further");
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = fd;
    expect(sr_hle_test_io_close(&cpu) == 0u, "the served descriptor closes cleanly");

    prewarm_env_restore();
    sr_hle_test_data_reset(0);
}

/* 2. Mutation catcher: a lookup on a NEVER-prepared route after guest start
 *    must refuse without enumerating or building anything. */
static void test_unprepared_route_lookup_fails_closed_without_building(void) {
    CpuState cpu;
    reset_fixture();
    sr_hle_init();
    expect(prewarm_make_fixture(1), "the source-owned data fixture was created");
    SetEnvironmentVariableA("SR_DATAROOT", s_prewarm_root);
    sr_hle_test_data_reset(0);
    sr_hle_test_data_mark_guest_start();

    expect(prewarm_open_common(&cpu) == 0x80010002u,
           "an unprepared applicable route fails the guest open closed");
    expect(sr_hle_test_data_state() == SR_DATA_TEST_STATE_UNINITIALIZED,
           "the refusal did not start a census");
    expect(sr_hle_test_data_walk_calls() == 0u,
           "guest time never enumerates on behalf of an unprepared route");
    expect(sr_hle_test_data_builds_after_guest() == 0,
           "and never records a post-guest build attempt");

    prewarm_env_restore();
    sr_hle_test_data_reset(0);
}

/* 3. Placement proof, not speed proof: even with per-directory pacing inside
 *    the real walk, the whole census completes within preparation -- before
 *    the guest-start boundary is marked. */
static void test_slow_enumeration_completes_before_guest_start(void) {
    CpuState cpu;
    reset_fixture();
    sr_hle_init();
    expect(prewarm_make_fixture(1), "the source-owned data fixture was created");
    SetEnvironmentVariableA("SR_DATAROOT", s_prewarm_root);
    sr_hle_test_data_reset(20 /* ms per directory */);

    expect(sr_host_data_prepare() == SR_DATA_TEST_STATE_READY,
           "a slow census still finishes entirely inside preparation");
    sr_hle_test_data_mark_guest_start();
    expect(sr_hle_test_data_builds_after_guest() == 0,
           "no paced enumeration work leaked past the boundary");

    prewarm_env_restore();
    sr_hle_test_data_reset(0);
    (void)cpu;
}

/* 4. A generic profile without SR_DATAROOT terminates DISABLED with ZERO
 *    filesystem enumeration, and guest-time lookups consume that state in O(1). */
static void test_unapplicable_route_disables_without_scanning(void) {
    CpuState cpu;
    reset_fixture();
    sr_hle_init();
    expect(prewarm_make_fixture(1), "the unused fixture exists but must not be scanned");
    prewarm_env_restore();   /* no operator root configured */
    sr_hle_test_data_reset(0);

    expect(sr_host_data_prepare() == SR_DATA_TEST_STATE_DISABLED,
           "a generic profile without SR_DATAROOT disables the route");
    expect(sr_hle_test_data_state() == SR_DATA_TEST_STATE_DISABLED,
           "DISABLED is terminal");
    expect(sr_hle_test_data_walk_calls() == 0u,
           "a disabled route performs ZERO recursive enumeration");
    expect(sr_hle_test_data_build_attempts() == 1,
           "disabling is itself a single terminal preparation decision");

    sr_hle_test_data_mark_guest_start();
    expect(prewarm_open_common(&cpu) == 0x80010002u,
           "guest lookups fail closed against a disabled route");
    expect(sr_hle_test_data_walk_calls() == 0u,
           "guest lookups never build or spin against a disabled route");

    sr_hle_test_data_reset(0);
}

/* 5. A configured-but-missing root fails ONCE during preparation; repeated
 *    guest-time lookups never rescan. */
static void test_missing_root_fails_once_and_stays_failed(void) {
    CpuState cpu;
    reset_fixture();
    sr_hle_init();
    char missing[MAX_PATH];
    snprintf(missing, sizeof missing, "%s\\build\\data_prewarm_missing_%lu",
             s_prewarm_root, (unsigned long)GetCurrentProcessId());
    SetEnvironmentVariableA("SR_DATAROOT", missing);
    sr_hle_test_data_reset(0);

    expect(sr_host_data_prepare() == SR_DATA_TEST_STATE_FAILED,
           "a missing configured root reaches FAILED during preparation");
    expect(sr_hle_test_data_build_attempts() == 1,
           "exactly one census attempt was made");
    sr_hle_test_data_mark_guest_start();
    for (int i = 0; i < 3; i++) {
        expect(prewarm_open_common(&cpu) == 0x80010002u,
               "repeated guest opens keep failing closed against the failed route");
    }
    expect(sr_hle_test_data_build_attempts() == 1,
           "FAILED is terminal: guest lookups never rescan");

    prewarm_env_restore();
    sr_hle_test_data_reset(0);
}

/* sceDisplaySetFrameBuf flip accounting.
 *
 * A stretch of vblanks with no new presented frame is a NO-NEW-FLIP observation,
 * and it has two opposite causes: the guest stopped asking for flips, or the guest
 * asked and this handler refused. The no-frame watchdog reports only a flip-vcount
 * delta, which cannot tell those apart -- a real investigation had to guess. These
 * counters make the distinction observable, so they are load-bearing diagnostics
 * and must not regress.
 *
 * Contract asserted here, through production NID dispatch:
 *   - an accepted immediate (sync=0) flip counts as `immediate`, not as a rejection;
 *   - a refused request counts as `rejected`, records the PSP error it returned, and
 *     leaves the active scanout state untouched (a refusal must not half-apply);
 *   - every call is counted exactly once, whatever the outcome.
 * Refusal cases used: sync>1 (INVALID_MODE) and a misaligned address (ILLEGAL_ADDR); both
 * are pre-existing behavior, so this test pins the accounting, not the error policy. */
static void test_display_setframebuf_flip_accounting(void) {
    /* These do not fit in an int, so they cannot be enum constants, and
     * SCE_KERNEL_ERROR_ILLEGAL_ADDR already exists as a file-scope macro. */
    static const uint32_t NID_DISPLAY_SET_FRAME_BUF = 0x289d82feu;
    static const uint32_t ERR_INVALID_MODE = 0x80000107u;
    static const uint32_t ERR_ILLEGAL_ADDR = SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    static const uint32_t VRAM_A = 0x04000000u;
    static const uint32_t VRAM_B = 0x04044000u;
    reset_fixture();
    sr_hle_init();
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    unsigned long c0, i0, l0, r0;
    sr_display_test_flip_counts(&c0, &i0, &l0, &r0, NULL);

    /* Accepted immediate flip: stride/format match the latched state, so it presents. */
    cpu.r[4] = VRAM_B; cpu.r[5] = 512; cpu.r[6] = 3; cpu.r[7] = 0;
    uint32_t ok = sr_syscall(&cpu, NID_DISPLAY_SET_FRAME_BUF);
    unsigned long c1, i1, l1, r1;
    sr_display_test_flip_counts(&c1, &i1, &l1, &r1, NULL);
    expect(ok == 0u, "SetFrameBuf accepts a matching immediate flip");
    expect(c1 == c0 + 1u, "SetFrameBuf counts the accepted call");
    expect(i1 == i0 + 1u, "an accepted immediate flip is counted as immediate");
    expect(r1 == r0, "an accepted flip is not counted as a rejection");

    /* Refusal 1: sync out of range. */
    cpu.r[4] = VRAM_A; cpu.r[5] = 512; cpu.r[6] = 3; cpu.r[7] = 2;
    uint32_t bad_sync = sr_syscall(&cpu, NID_DISPLAY_SET_FRAME_BUF);
    unsigned long c2, i2, l2, r2; uint32_t err2;
    sr_display_test_flip_counts(&c2, &i2, &l2, &r2, &err2);
    expect(bad_sync == ERR_INVALID_MODE,
           "SetFrameBuf refuses sync>1 with INVALID_MODE");
    expect(c2 == c1 + 1u, "SetFrameBuf counts a refused call");
    expect(r2 == r1 + 1u, "a refused request is counted as a rejection");
    expect(err2 == ERR_INVALID_MODE,
           "the rejection records the error actually returned");
    expect(i2 == i1 && l2 == l1,
           "a refusal is not also counted as an accepted flip");

    /* Refusal 2: misaligned address, to show the accounting tracks which refusal was
     * last and is not wired to a single error path. */
    cpu.r[4] = VRAM_A + 1u; cpu.r[5] = 512; cpu.r[6] = 3; cpu.r[7] = 0;
    uint32_t bad_addr = sr_syscall(&cpu, NID_DISPLAY_SET_FRAME_BUF);
    unsigned long c3, i3, l3, r3; uint32_t err3;
    sr_display_test_flip_counts(&c3, &i3, &l3, &r3, &err3);
    expect(bad_addr == ERR_ILLEGAL_ADDR,
           "SetFrameBuf refuses a misaligned address with ILLEGAL_ADDR");
    expect(c3 == c2 + 1u && r3 == r2 + 1u, "the misaligned refusal is counted too");
    expect(err3 == ERR_ILLEGAL_ADDR,
           "the recorded error tracks the most recent refusal");
    expect(i3 == i2, "a refused request performed no flip");
}

/* No-frame watchdog observation boundary semantics.
 *
 * The no-frame watchdog reports a NO-NEW-FLIP observation, not a hang verdict:
 * a stretch with no new presented frame is exactly what a legitimately static
 * scene (e.g. a save-confirmation modal waiting for user input) also produces.
 * The contract asserted here is the clock itself, through production
 * sr_vblank_tick:
 *   - at one-period service cadence, entering each new 600-period bucket emits
 *     one observation;
 *   - a multi-period service that crosses one or more bucket boundaries emits
 *     one current-state observation and records the newest bucket, never a
 *     replay burst;
 *   - an accepted immediate flip resets the clock: the next observation needs a
 *     fresh 600-vblank stretch;
 *   - a refused request is not a new frame, so it must not reset the clock.
 */
static void test_watchdog_no_new_frame_observation(void) {
    static const uint32_t NID_DISPLAY_SET_FRAME_BUF = 0x289d82feu;
    static const uint32_t VRAM_B = 0x04044000u;
    reset_fixture();
    sr_hle_init();
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    unsigned long f0;
    uint32_t d0;
    sr_watchdog_test_state(&f0, &d0);

    /* 600 display periods with no flip: exactly one observation fires. */
    for (unsigned i = 0; i < 600u; i++) { sr_display_advance_vcount(1u); sr_vblank_tick(); }
    unsigned long f1;
    uint32_t d1;
    sr_watchdog_test_state(&f1, &d1);
    expect(f1 == f0 + 1ul,
           "no-new-frame observation fires exactly once per 600 vblanks");
    expect(d1 == d0 + 600u,
           "the vblanks-since-flip clock advanced by the elapsed periods");

    /* Another 600 with still no flip: the next boundary fires once more. */
    for (unsigned i = 0; i < 600u; i++) { sr_display_advance_vcount(1u); sr_vblank_tick(); }
    unsigned long f2;
    sr_watchdog_test_state(&f2, NULL);
    expect(f2 == f1 + 1ul,
           "the observation fires again at the next 600-vblank boundary");

    /* An accepted immediate flip is a new frame: it resets the clock, so the
     * next firing needs a fresh 600-vblank stretch. */
    cpu.r[4] = VRAM_B; cpu.r[5] = 512; cpu.r[6] = 3; cpu.r[7] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_SET_FRAME_BUF) == 0u,
           "SetFrameBuf accepts a matching immediate flip (watchdog reset case)");
    uint32_t d_after_flip;
    sr_watchdog_test_state(NULL, &d_after_flip);
    expect(d_after_flip == 0u,
           "an accepted flip resets the vblanks-since-flip clock");
    for (unsigned i = 0; i < 600u; i++) { sr_display_advance_vcount(1u); sr_vblank_tick(); }
    unsigned long f3;
    sr_watchdog_test_state(&f3, NULL);
    expect(f3 == f2 + 1ul,
           "after a reset the next observation needs a fresh 600-vblank stretch");

    /* A refused request is not a new frame: the clock keeps running. */
    cpu.r[4] = VRAM_B; cpu.r[5] = 512; cpu.r[6] = 3; cpu.r[7] = 2; /* sync>1 */
    (void)sr_syscall(&cpu, NID_DISPLAY_SET_FRAME_BUF);
    uint32_t d_after_refusal;
    sr_watchdog_test_state(NULL, &d_after_refusal);
    expect(d_after_refusal == 600u,
           "a refused request does not reset the no-frame clock");
}

/* #77 watchdog boundary semantics under coalesced VCOUNT advance.
 *
 * VCOUNT advances by whole elapsed display periods at the scheduler source
 * latch, while sr_vblank_tick() runs once per *serviced* episode.  A single
 * service can therefore observe the no-flip distance jump across a 600 boundary
 * without ever landing on a multiple of 600.  The contract is a boundary
 * CROSSING, not an exact modulus:
 *   - 598 -> 602 in one service emits exactly one observation;
 *   - further services inside the same bucket emit nothing;
 *   - crossing the next bucket emits exactly one more;
 *   - an accepted flip resets the bucket, so a fresh stretch reports again.
 *
 * This test drives the real scheduler source latch and pending-service path in
 * multi-period jumps. Driving the accounting helper directly, or one period per
 * tick, would fail to prove the production boundary this regression protects. */
static void test_watchdog_fires_on_boundary_crossing_not_exact_multiple(void) {
    static const uint32_t NID_DISPLAY_SET_FRAME_BUF = 0x289d82feu;
    static const uint32_t VRAM_B = 0x04044000u;
    reset_fixture();
    sr_hle_init();
    s_pace_on = 0;
    s_vtime_us = 0;
    s_vbl_next_us = 0;
    s_vbl_event_period_rem = 0;
    s_vbl_count = 0;
    s_interrupts_enabled = 1;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    /* Zero the no-flip clock deterministically through the production path. */
    cpu.r[4] = VRAM_B; cpu.r[5] = 512; cpu.r[6] = 3; cpu.r[7] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_SET_FRAME_BUF) == 0u,
           "watchdog crossing probe: accepted immediate flip zeroes the clock");
    uint32_t d;
    unsigned long f_base;
    sr_watchdog_test_state(&f_base, &d);
    expect(d == 0u, "watchdog crossing probe: no-flip clock starts at zero");

    /* The source latch discovers 598 elapsed periods in one production batch;
     * the single pending episode is then serviced once. */
    s_vtime_us = (uint64_t)scheduler_vblank_delta(597u, 0u, NULL);
    scheduler_latch_due_events();
    expect((s_pending_interrupts & SCHED_INTR_VBLANK) != 0,
           "598-period source batch raises one pending VBLANK");
    scheduler_service_pending();
    unsigned long f;
    sr_watchdog_test_state(&f, &d);
    expect(d == 598u, "no-flip clock reached 598 in one coalesced advance");
    expect(f == f_base, "no observation before the first boundary is crossed");

    /* The next source latch discovers four periods at once.  The serviced
     * distance moves 598 -> 602 without ever equalling a multiple of 600, so an
     * exact-modulus watchdog reports nothing and a crossing watchdog reports
     * exactly once. */
    s_vtime_us = (uint64_t)scheduler_vblank_delta(601u, 0u, NULL);
    scheduler_latch_due_events();
    scheduler_service_pending();
    sr_watchdog_test_state(&f, &d);
    expect(d == 602u, "no-flip clock crossed the 600 boundary to 602");
    expect(f == f_base + 1ul,
           "crossing 600 without landing on it reports exactly once");

    /* A later eight-period source batch stays in the same bucket and must not
     * duplicate the observation. */
    s_vtime_us = (uint64_t)scheduler_vblank_delta(609u, 0u, NULL);
    scheduler_latch_due_events();
    scheduler_service_pending();
    sr_watchdog_test_state(&f, &d);
    expect(d == 610u, "no-flip clock advanced within the same bucket");
    expect(f == f_base + 1ul,
           "additional services inside the same bucket report nothing further");

    /* Cross the next bucket (1200) in another production source batch. */
    s_vtime_us = (uint64_t)scheduler_vblank_delta(1204u, 0u, NULL);
    scheduler_latch_due_events();
    scheduler_service_pending();
    sr_watchdog_test_state(&f, &d);
    expect(d == 1205u, "no-flip clock crossed the second boundary to 1205");
    expect(f == f_base + 2ul,
           "crossing the next bucket reports exactly once more");

    /* A single source batch may cross several buckets after a long host stall.
     * Emit one current-state observation, record the newest bucket, and do not
     * replay one diagnostic per missed threshold. */
    s_vtime_us = (uint64_t)scheduler_vblank_delta(3004u, 0u, NULL);
    scheduler_latch_due_events();
    scheduler_service_pending();
    sr_watchdog_test_state(&f, &d);
    expect(d == 3005u, "no-flip clock crossed several buckets in one source batch");
    expect(f == f_base + 3ul,
           "a multi-bucket source batch emits one current-state observation");

    s_vtime_us = (uint64_t)scheduler_vblank_delta(3009u, 0u, NULL);
    scheduler_latch_due_events();
    scheduler_service_pending();
    sr_watchdog_test_state(&f, &d);
    expect(d == 3010u, "no-flip clock advanced within the newest bucket");
    expect(f == f_base + 3ul,
           "recording the newest crossed bucket prevents a replay on the next service");

    /* An accepted flip resets the bucket: a fresh stretch reports again. */
    cpu.r[4] = VRAM_B; cpu.r[5] = 512; cpu.r[6] = 3; cpu.r[7] = 0;
    expect(sr_syscall(&cpu, NID_DISPLAY_SET_FRAME_BUF) == 0u,
           "watchdog crossing probe: second accepted flip");
    sr_watchdog_test_state(&f, &d);
    expect(d == 0u, "an accepted flip resets the no-flip clock");
    expect(f == f_base + 3ul, "the reset itself reports nothing");
    s_vtime_us = (uint64_t)scheduler_vblank_delta(3611u, 0u, NULL);
    scheduler_latch_due_events();
    scheduler_service_pending();
    sr_watchdog_test_state(&f, &d);
    expect(d == 602u, "fresh stretch reached 602 in one coalesced advance");
    expect(f == f_base + 4ul,
           "after a reset the next crossing reports exactly once");
}

static void test_interrupt_nid_semantics(void) {
    reset_fixture();
    sr_hle_init();
    s_vbl_next_us = UINT64_MAX; /* keep this NID-only probe independent of VBLANK delivery */
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    expect(sr_syscall(&cpu, NID_SCE_KERNEL_IS_CPU_INTR_ENABLE) == 1u,
           "IsCpuIntrEnable reports the initial enabled state");
    uint32_t outer = sr_syscall(&cpu, NID_SCE_KERNEL_CPU_SUSPEND_INTR);
    expect(outer == 1u, "CpuSuspendIntr returns the prior enabled token");
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_IS_CPU_INTR_ENABLE) == 0u,
           "IsCpuIntrEnable reports a suspended CPU");
    uint32_t inner = sr_syscall(&cpu, NID_SCE_KERNEL_CPU_SUSPEND_INTR);
    expect(inner == 0u, "nested CpuSuspendIntr returns token 0");
    cpu.r[4] = inner;
    (void)sr_syscall(&cpu, NID_SCE_KERNEL_CPU_RESUME_INTR_SYNC);
    expect(!sched_interrupts_enabled(), "ResumeIntrWithSync restores token 0");
    cpu.r[4] = 0xDEADBEEFu;
    (void)sr_syscall(&cpu, NID_SCE_KERNEL_CPU_RESUME_INTR);
    expect(!sched_interrupts_enabled(), "invalid ResumeIntr token cannot enable interrupts");
    cpu.r[4] = outer;
    (void)sr_syscall(&cpu, NID_SCE_KERNEL_CPU_RESUME_INTR);
    expect(sched_interrupts_enabled(), "ResumeIntr restores token 1");
}

/* sceKernelIsCpuIntrSuspended is a pure predicate on the saved-state token the
 * caller supplies, not a query of the CPU's current interrupt-enable state.
 *
 * PSPAutotests tests/intr/suspended.expected prints the same four results twice --
 * once with interrupts enabled, once with them suspended -- so the hardware answer
 * provably cannot depend on the current state:
 *
 *     0: 00000001   1: 00000000   2: 00000000   0xDEADBEEF: 00000000
 *
 * The real-PSP capture recorded on issue #88 (PSP-3001 / 6.61-ARK) reports the same
 * four values, and suspended.cpp's own flags(inner)/flags(outer) probes agree: a
 * token of 0 means "was already suspended" (1) and a token of 1 means "was enabled" (0).
 *
 * The previous revision of this suite probed only argument 0 while suspended -- the
 * single cell of the 2x4 matrix where a current-state query and the token predicate
 * happen to agree -- so an implementation that ignored the argument stayed green.
 * Probe the complete matrix through the production NID so that cannot recur. */
static void test_is_cpu_intr_suspended_is_token_predicate(void) {
    reset_fixture();
    sr_hle_init();
    s_vbl_next_us = UINT64_MAX; /* NID-only probe: keep VBLANK delivery out of it */
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    static const struct {
        uint32_t arg;
        uint32_t want;
        const char *label;
    } kCells[] = {
        { 0u,          1u, "0" },
        { 1u,          0u, "1" },
        { 2u,          0u, "2" },
        { 0xDEADBEEFu, 0u, "0xDEADBEEF" },
    };

    for (int suspended = 0; suspended <= 1; suspended++) {
        if (suspended) (void)sr_syscall(&cpu, NID_SCE_KERNEL_CPU_SUSPEND_INTR);
        expect(sched_interrupts_enabled() == !suspended,
               suspended ? "matrix fixture: CPU interrupts are suspended"
                         : "matrix fixture: CPU interrupts are enabled");
        for (size_t i = 0; i < sizeof kCells / sizeof kCells[0]; i++) {
            char msg[128];
            cpu.r[4] = kCells[i].arg;
            uint32_t got = sr_syscall(&cpu, NID_SCE_KERNEL_IS_CPU_INTR_SUSPENDED);
            snprintf(msg, sizeof msg,
                     "IsCpuIntrSuspended(%s) == %u with interrupts %s",
                     kCells[i].label, kCells[i].want,
                     suspended ? "suspended" : "enabled");
            expect(got == kCells[i].want, msg);
        }
    }

    /* Leave the shared interrupt state as this suite found it. */
    cpu.r[4] = 1u;
    (void)sr_syscall(&cpu, NID_SCE_KERNEL_CPU_RESUME_INTR);
}

static int s_worker_ran_prematurely = 0;

static void yield_worker_coro_body(void *arg) {
    (void)arg;
    s_worker_ran_prematurely = 1;
}

static void yield_test_coro_body(void *arg) {
    (void)arg;
    CpuState ctl;
    memset(&ctl, 0, sizeof ctl);
    uint32_t token = sr_syscall(&ctl, NID_SCE_KERNEL_SUSPEND_DISPATCH_THREAD);
    expect(token == 1u && !sched_dispatch_enabled(), "dispatch suspended in coroutine for yield preemption test");

    uint64_t vtime_before = s_vtime_us;
    sr_yield(&ctl);
    expect(s_cur == 0, "sr_yield boundary preserves current thread execution while dispatch is suspended");
    expect(s_worker_ran_prematurely == 0, "higher-priority worker did not run prematurely while dispatch was suspended");
    expect(s_vtime_us >= vtime_before, "scheduler time/interrupts progress normally during sr_yield while dispatch is suspended");
    expect(s_vtime_us < s_tcb[2].wake, "dispatch lock alone does not spuriously snap virtual time to sleeping waiter's deadline");

    ctl.r[4] = token;
    (void)sr_syscall(&ctl, NID_SCE_KERNEL_RESUME_DISPATCH_THREAD);
    expect(s_cur == 1, "ResumeDispatchThread(1) re-enables preemption and switches to higher-priority READY thread");
    expect(s_worker_ran_prematurely == 1, "higher-priority worker ran after ResumeDispatchThread(1)");
}

static void test_dispatch_suspend_resume_nid_semantics(void) {
    reset_fixture();
    sr_hle_init();

    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);

    /* 1. Initial state: dispatch enabled -> SuspendDispatchThread returns token 1 */
    expect(sched_dispatch_enabled() == 1, "dispatch is enabled initially");
    uint32_t token1 = sr_syscall(&cpu, NID_SCE_KERNEL_SUSPEND_DISPATCH_THREAD);
    expect(token1 == 1u, "SuspendDispatchThread returns prior state 1 when enabled");
    expect(sched_dispatch_enabled() == 0, "SuspendDispatchThread disables dispatch");

    /* 2. Nested suspension: SuspendDispatchThread when already suspended returns token 0 */
    uint32_t token2 = sr_syscall(&cpu, NID_SCE_KERNEL_SUSPEND_DISPATCH_THREAD);
    expect(token2 == 0u, "SuspendDispatchThread returns prior state 0 when already suspended");
    expect(sched_dispatch_enabled() == 0, "dispatch remains suspended");

    /* 3. Resume with state=0 keeps dispatch suspended */
    cpu.r[4] = 0u;
    uint32_t res0 = sr_syscall(&cpu, NID_SCE_KERNEL_RESUME_DISPATCH_THREAD);
    expect(res0 == 0u, "ResumeDispatchThread(0) returns 0");
    expect(sched_dispatch_enabled() == 0, "ResumeDispatchThread(0) keeps dispatch suspended");

    /* 4. Resume with state=1 restores dispatch */
    cpu.r[4] = token1;
    uint32_t res1 = sr_syscall(&cpu, NID_SCE_KERNEL_RESUME_DISPATCH_THREAD);
    expect(res1 == 0u, "ResumeDispatchThread(1) returns 0");
    expect(sched_dispatch_enabled() == 1, "ResumeDispatchThread(1) restores dispatch enabled");

    /* 5. CPU Interrupt Error Precedence: when CPU interrupts are suspended,
     * SuspendDispatchThread and ResumeDispatchThread return 0x80020066 (SCE_KERNEL_ERROR_CPUDI)
     * without modifying dispatch-suspension state. */
    uint32_t intr_token = sr_syscall(&cpu, NID_SCE_KERNEL_CPU_SUSPEND_INTR);
    expect(!sched_interrupts_enabled(), "CPU interrupts suspended");

    uint32_t err_suspend = sr_syscall(&cpu, NID_SCE_KERNEL_SUSPEND_DISPATCH_THREAD);
    expect(err_suspend == 0x80020066u, "SuspendDispatchThread returns SCE_KERNEL_ERROR_CPUDI when interrupts disabled");

    cpu.r[4] = 1u;
    uint32_t err_resume = sr_syscall(&cpu, NID_SCE_KERNEL_RESUME_DISPATCH_THREAD);
    expect(err_resume == 0x80020066u, "ResumeDispatchThread returns SCE_KERNEL_ERROR_CPUDI when interrupts disabled");

    /* Restore CPU interrupts */
    cpu.r[4] = intr_token;
    (void)sr_syscall(&cpu, NID_SCE_KERNEL_CPU_RESUME_INTR);
    expect(sched_interrupts_enabled(), "CPU interrupts restored");
    expect(sched_dispatch_enabled() == 1, "dispatch state unaffected by rejected CPUDI calls");

    /* 6. sr_yield() boundary preemption suppression & virtual time preservation under dispatch suspension:
     * Current thread (low priority, prio 40, uid 0x110, s_cur 0) vs READY worker thread (higher priority, prio 20, uid 0x111, s_cur 1)
     * vs separate finite-deadline sleeping waiter (prio 30, uid 0x112, s_cur 2). */
    reset_fixture();
    sr_hle_init();
    s_worker_ran_prematurely = 0;
    TCB *cur_tcb = fixture_thread(0x110u, TH_RUNNING, 40);
    TCB *high_tcb = fixture_thread(0x111u, TH_READY, 20);
    TCB *sleep_tcb = fixture_thread(0x112u, TH_WAIT_DELAY, 30);
    sleep_tcb->wake = s_vtime_us + 100000u;
    s_cur = (int)(cur_tcb - s_tcb);
    cur_tcb->started = 1;
    cur_tcb->coro = sr_coro_create(yield_test_coro_body, NULL, (size_t)1 << 20);
    high_tcb->started = 1;
    high_tcb->coro = sr_coro_create(yield_worker_coro_body, NULL, (size_t)1 << 20);
    expect(cur_tcb->coro != NULL && high_tcb->coro != NULL, "yield test coroutines created");
    if (cur_tcb->coro) {
        sr_coro_switch(cur_tcb->coro);
        if (s_cur != 0 && cur_tcb->coro) {
            sr_coro_switch(cur_tcb->coro);
        }
        sr_coro_destroy(cur_tcb->coro);
        cur_tcb->coro = NULL;
    }
    if (high_tcb->coro) {
        sr_coro_destroy(high_tcb->coro);
        high_tcb->coro = NULL;
    }
}

/* -------------------------------------------------------------------------
 * SCE_KERNEL_ERROR_CAN_NOT_WAIT: what the conformance matrix cannot assert
 * -------------------------------------------------------------------------
 * The matrix in intr_conformance.h pins the RETURN VALUE of each cell against
 * hardware. These checks cover the properties a return value cannot show:
 *
 *   - dispatch-disabled alone is sufficient, with CPU interrupts still enabled,
 *     so the two states are being consulted independently rather than one being
 *     read as a proxy for the other;
 *   - a rejected call leaves the caller RUNNING -- no TH_WAIT_* transition and
 *     no coroutine park behind the returned error;
 *   - a rejected call performs no part of the operation: no wakeup consumed, no
 *     wake deadline armed, no vblank latch taken, no semaphore count decremented,
 *     no event pattern consumed, no join target set;
 *   - validation that hardware puts AHEAD of the context error still runs first;
 *   - an invocation that would NOT have blocked still succeeds while the context
 *     is disabled, which is what keeps the check a wait gate rather than a
 *     blanket syscall gate.
 *
 * The precedence is per-API, and sceKernelWaitSema/CB sit on the other side of
 * it from sceKernelWaitEventFlag and sceKernelWaitThreadEnd: waits.expected
 * L54-L57 and L62-L65 answer CAN_NOT_WAIT for a bad id and for an invalid count,
 * so for that one family the gate is ahead of the object lookup and therefore
 * ahead of the availability test too. Its "would not have blocked still
 * succeeds" leg is asserted as CAN_NOT_WAIT below for exactly that reason; the
 * gate-not-blanket property is carried by the other five families here.
 *
 * Everything enters through sr_syscall's registered-NID lookup, so these are
 * production-dispatch assertions on the real handlers.
 * ------------------------------------------------------------------------- */
#define NID_CNW_DELAY_THREAD        0xceadeb47u
#define NID_CNW_DELAY_THREAD_CB     0x68da9e36u
#define NID_CNW_SLEEP_THREAD_CB     0x82826f70u
#define NID_CNW_WAIT_VBLANK         0x36cdfadeu
#define NID_CNW_WAIT_VBLANK_START   0x984c27e7u
#define NID_CNW_CREATE_SEMA         0xd6da4ba1u
#define NID_CNW_WAIT_SEMA           0x4e3a1105u
#define NID_CNW_WAIT_SEMA_CB        0x6d212bacu
#define NID_CNW_CREATE_EVF          0x55c20a00u
#define NID_CNW_WAIT_EVF            0x402fcf22u
#define NID_CNW_WAIT_EVF_CB         0x328c546au
#define NID_CNW_WAIT_THREAD_END     0x278c0df5u
#define NID_CNW_WAIT_THREAD_END_CB  0x840e8133u

#define CNW_ERR      0x800201a7u   /* SCE_KERNEL_ERROR_CAN_NOT_WAIT */
#define CNW_NAMEBUF  0x00250000u

/* Put the caller on a fixture thread and disable ONE of the two states. When
 * intr_off is 0 the CPU stays interrupt-enabled and only dispatch is suspended,
 * which is the leg that proves the states are independent. */
static TCB *cnw_begin(int intr_off, uint32_t *token_out) {
    reset_fixture();
    sr_hle_init();
    TCB *self = fixture_thread(0x1d0u, TH_RUNNING, 32);
    s_cur = (int)(self - s_tcb);
    self->started = 1;

    CpuState ctl;
    memset(&ctl, 0, sizeof ctl);
    *token_out = sr_syscall(&ctl, intr_off ? NID_SCE_KERNEL_CPU_SUSPEND_INTR
                                           : NID_SCE_KERNEL_SUSPEND_DISPATCH_THREAD);
    return self;
}

static void cnw_end(int intr_off, uint32_t token) {
    CpuState ctl;
    memset(&ctl, 0, sizeof ctl);
    ctl.r[4] = token;
    (void)sr_syscall(&ctl, intr_off ? NID_SCE_KERNEL_CPU_RESUME_INTR
                                    : NID_SCE_KERNEL_RESUME_DISPATCH_THREAD);
    s_cur = -1;
}

static void test_can_not_wait_semantics(void) {
    CpuState cpu;
    uint32_t token;

    /* ---- 1. dispatch-disabled alone rejects, with interrupts still enabled --- */
    for (int i = 0; i < 2; i++) {
        const uint32_t nid = i ? NID_CNW_DELAY_THREAD_CB : NID_CNW_DELAY_THREAD;
        const char *who = i ? "sceKernelDelayThreadCB" : "sceKernelDelayThread";
        TCB *self = cnw_begin(0, &token);
        expect(sched_interrupts_enabled(),
               "dispatch-disabled leg leaves CPU interrupts ENABLED");
        expect(!sched_dispatch_enabled(), "dispatch is suspended");

        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = 200u;
        uint32_t rc = sr_syscall(&cpu, nid);

        char msg[192];
        snprintf(msg, sizeof msg,
                 "%s returns CAN_NOT_WAIT with dispatch disabled and interrupts enabled "
                 "(the two states are consulted independently)", who);
        expect(rc == CNW_ERR, msg);
        snprintf(msg, sizeof msg, "%s: rejected call left the caller RUNNING, not TH_WAIT_DELAY", who);
        expect(self->state == TH_RUNNING, msg);
        snprintf(msg, sizeof msg, "%s: rejected call armed no wake deadline", who);
        expect(self->wake == (uint64_t)-1, msg);
        cnw_end(0, token);
    }

    /* ---- 2. the same calls are rejected with interrupts disabled -------------- */
    {
        TCB *self = cnw_begin(1, &token);
        expect(!sched_interrupts_enabled(), "CPU interrupts are suspended");
        expect(sched_dispatch_enabled(),
               "interrupts-disabled leg leaves dispatch state untouched");
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = 200u;
        expect(sr_syscall(&cpu, NID_CNW_DELAY_THREAD) == CNW_ERR,
               "sceKernelDelayThread returns CAN_NOT_WAIT with interrupts disabled");
        expect(self->state == TH_RUNNING, "interrupts-disabled rejection did not park the caller");
        cnw_end(1, token);
    }

    /* ---- 3. sleep: a banked wakeup is neither consumed nor required ----------- */
    {
        TCB *self = cnw_begin(0, &token);
        self->wakeups = 0;
        memset(&cpu, 0, sizeof cpu);
        expect(sr_syscall(&cpu, NID_SCE_KERNEL_SLEEP_THREAD) == CNW_ERR,
               "sceKernelSleepThread with no banked wakeup returns CAN_NOT_WAIT");
        expect(self->wakeups == 0 && self->sleeping == 0 && self->state == TH_RUNNING,
               "rejected sceKernelSleepThread consumed no wakeup and set no sleep marker");

        /* A sleep that would be satisfied from the wakeup count is not a wait, so
         * it must still succeed while dispatch is suspended -- this is the check
         * that separates a wait gate from a blanket syscall gate. */
        self->wakeups = 1;
        memset(&cpu, 0, sizeof cpu);
        expect(sr_syscall(&cpu, NID_SCE_KERNEL_SLEEP_THREAD) == 0u,
               "sceKernelSleepThread with a banked wakeup still succeeds with dispatch disabled");
        expect(self->wakeups == 0, "the satisfied sleep consumed exactly one banked wakeup");

        self->wakeups = 0;
        memset(&cpu, 0, sizeof cpu);
        expect(sr_syscall(&cpu, NID_CNW_SLEEP_THREAD_CB) == CNW_ERR,
               "sceKernelSleepThreadCB with no banked wakeup returns CAN_NOT_WAIT");
        expect(self->wakeups == 0 && self->sleeping == 0,
               "rejected sceKernelSleepThreadCB mutated no sleep state");
        cnw_end(0, token);
    }

    /* ---- 4. vblank: the latch is not consumed -------------------------------- */
    for (int i = 0; i < 2; i++) {
        const uint32_t nid = i ? NID_CNW_WAIT_VBLANK_START : NID_CNW_WAIT_VBLANK;
        const char *who = i ? "sceDisplayWaitVblankStart" : "sceDisplayWaitVblank";
        TCB *self = cnw_begin(0, &token);
        /* Arrange an UNSEEN vblank: without the gate, sched_wait_vblank() would
         * take this latch and return 0 without blocking at all. */
        s_vbl_count = 7;
        self->vbl_seen = 3;
        memset(&cpu, 0, sizeof cpu);
        uint32_t rc = sr_syscall(&cpu, nid);

        char msg[192];
        snprintf(msg, sizeof msg, "%s returns CAN_NOT_WAIT with dispatch disabled", who);
        expect(rc == CNW_ERR, msg);
        snprintf(msg, sizeof msg, "%s: rejected call did not consume the vblank latch", who);
        expect(self->vbl_seen == 3, msg);
        snprintf(msg, sizeof msg, "%s: rejected call did not block on VBLANK_WAIT_OBJ", who);
        expect(self->state == TH_RUNNING && self->wait_obj == 0, msg);
        cnw_end(0, token);
    }

    /* ---- 5. semaphore: object error keeps precedence, count is untouched ------ */
    for (int i = 0; i < 2; i++) {
        const uint32_t nid = i ? NID_CNW_WAIT_SEMA_CB : NID_CNW_WAIT_SEMA;
        const char *who = i ? "sceKernelWaitSemaCB" : "sceKernelWaitSema";
        char msg[192];

        /* An unsatisfiable wait is rejected. */
        TCB *self = cnw_begin(0, &token);
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = CNW_NAMEBUF; cpu.r[5] = 0; cpu.r[6] = 0; cpu.r[7] = 1;   /* init 0, max 1 */
        uint32_t sema = sr_syscall(&cpu, NID_CNW_CREATE_SEMA);
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = sema; cpu.r[5] = 1u; cpu.r[6] = 0u;
        snprintf(msg, sizeof msg, "%s on an unavailable count returns CAN_NOT_WAIT", who);
        expect(sr_syscall(&cpu, nid) == CNW_ERR, msg);
        snprintf(msg, sizeof msg, "%s: rejected call did not enter a wait", who);
        expect(self->state == TH_RUNNING && self->wait_obj == 0, msg);

        /* The context error now wins over the bad-object error for this family:
         * waits.expected L54/L55 (CB L62/L63) measure CAN_NOT_WAIT for a bad id,
         * where normal context answers UNKNOWN_SEMID (wait.expected L21/L23). */
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = 0u; cpu.r[5] = 1u; cpu.r[6] = 0u;
        snprintf(msg, sizeof msg,
                 "%s on a bad sema returns CAN_NOT_WAIT, ahead of its object error", who);
        expect(sr_syscall(&cpu, nid) == CNW_ERR, msg);

        /* Same for an invalid count: L56/L57 (CB L64/L65). Normal context here is
         * ILLEGAL_COUNT, so this is the gate beating count validation too. */
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = sema; cpu.r[5] = 9u; cpu.r[6] = 0u;   /* 9 against maxCount 1 */
        snprintf(msg, sizeof msg,
                 "%s on an invalid count returns CAN_NOT_WAIT, ahead of ILLEGAL_COUNT", who);
        expect(sr_syscall(&cpu, nid) == CNW_ERR, msg);
        cnw_end(0, token);

        /* A count that IS available is still rejected. This cell is not measured
         * directly -- no fixture calls WaitSema on an available count with a
         * context disabled -- but it is FORCED by the two cells above: a gate
         * ahead of the object lookup cannot also be behind the availability test
         * that needs the object. */
        self = cnw_begin(0, &token);
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = CNW_NAMEBUF; cpu.r[5] = 0; cpu.r[6] = 1u; cpu.r[7] = 1u;  /* init 1, max 1 */
        sema = sr_syscall(&cpu, NID_CNW_CREATE_SEMA);
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = sema; cpu.r[5] = 1u; cpu.r[6] = 0u;
        snprintf(msg, sizeof msg,
                 "%s with an available count is still rejected with dispatch disabled", who);
        expect(sr_syscall(&cpu, nid) == CNW_ERR, msg);
        int cur = -1, mx = -1;
        expect(sr_hle_test_sema_state(sema, &cur, &mx) && cur == 1 && mx == 1,
               "the context-rejected wait left the available count untouched");
        expect(self->state == TH_RUNNING && self->wait_obj == 0,
               "the context-rejected available wait entered no wait");
        cnw_end(0, token);
    }

    /* ---- 6. event flag: ILLEGAL_MODE and the satisfied case both win --------- */
    for (int i = 0; i < 2; i++) {
        const uint32_t nid = i ? NID_CNW_WAIT_EVF_CB : NID_CNW_WAIT_EVF;
        const char *who = i ? "sceKernelWaitEventFlagCB" : "sceKernelWaitEventFlag";
        char msg[192];

        TCB *self = cnw_begin(0, &token);
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = CNW_NAMEBUF; cpu.r[5] = 0; cpu.r[6] = 0; cpu.r[7] = 0;   /* pattern 0 */
        uint32_t evf = sr_syscall(&cpu, NID_CNW_CREATE_EVF);

        /* Unmatched pattern -> genuine wait -> rejected, consuming nothing. */
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = evf; cpu.r[5] = 1u; cpu.r[6] = 0u; cpu.r[7] = 0u; cpu.r[8] = 0u;
        snprintf(msg, sizeof msg, "%s on an unmatched pattern returns CAN_NOT_WAIT", who);
        expect(sr_syscall(&cpu, nid) == CNW_ERR, msg);
        snprintf(msg, sizeof msg, "%s: rejected call did not enter a wait", who);
        expect(self->state == TH_RUNNING && self->wait_obj == 0, msg);

        /* Mode validation runs BEFORE the context decision (waits.expected L72/L73,
         * L82/L83): ILLEGAL_MODE, not CAN_NOT_WAIT. This is the single cell that
         * rules out a universal pre-handler gate, so it is asserted directly. */
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = evf; cpu.r[5] = 1u; cpu.r[6] = 0xFFu; cpu.r[7] = 0u; cpu.r[8] = 0u;
        snprintf(msg, sizeof msg,
                 "%s invalid mode returns ILLEGAL_MODE ahead of the context error", who);
        expect(sr_syscall(&cpu, nid) == 0x80020195u, msg);

        /* Bad object still answers in the object error space. */
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = 0u; cpu.r[5] = 1u; cpu.r[6] = 0u; cpu.r[7] = 0u; cpu.r[8] = 0u;
        snprintf(msg, sizeof msg, "%s bad flag still returns its object error", who);
        expect(sr_syscall(&cpu, nid) == 0x80020000u, msg);
        cnw_end(0, token);

        /* An already-satisfied pattern is not a wait: it succeeds and is consumed. */
        self = cnw_begin(0, &token);
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = CNW_NAMEBUF; cpu.r[5] = 0; cpu.r[6] = 1u; cpu.r[7] = 0;  /* pattern 1 */
        evf = sr_syscall(&cpu, NID_CNW_CREATE_EVF);
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = evf; cpu.r[5] = 1u; cpu.r[6] = 0u; cpu.r[7] = 0u; cpu.r[8] = 0u;
        snprintf(msg, sizeof msg,
                 "%s on an already-set pattern still succeeds with dispatch disabled", who);
        expect(sr_syscall(&cpu, nid) == 0u, msg);
        cnw_end(0, token);
    }

    /* ---- 7. thread join: ILLEGAL_THID and the immediate cases both win ------- */
    for (int i = 0; i < 2; i++) {
        const uint32_t nid = i ? NID_CNW_WAIT_THREAD_END_CB : NID_CNW_WAIT_THREAD_END;
        const char *who = i ? "sceKernelWaitThreadEndCB" : "sceKernelWaitThreadEnd";
        char msg[192];

        TCB *self = cnw_begin(0, &token);
        TCB *running = fixture_thread(0x1d2u, TH_READY, 40);
        running->started = 1;
        TCB *dormant = fixture_thread(0x1d3u, TH_DORMANT, 40);
        dormant->started = 0;

        /* Object validation is ahead of the context error in EVERY context on
         * hardware (waits.expected L204/L205/L383), so thid 0 keeps ILLEGAL_THID. */
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = 0u; cpu.r[5] = 0u;
        snprintf(msg, sizeof msg,
                 "%s(0) returns ILLEGAL_THID ahead of the context error", who);
        expect(sr_syscall(&cpu, nid) == 0x80020197u, msg);

        /* A target that is not running resolves immediately, so it is not a wait
         * and its current answer is preserved. */
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = dormant->uid; cpu.r[5] = 0u;
        snprintf(msg, sizeof msg,
                 "%s on a never-started target still resolves immediately", who);
        expect(sr_syscall(&cpu, nid) == 0x800201a2u, msg);

        /* A running target is a genuine wait. */
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = running->uid; cpu.r[5] = 0u;
        snprintf(msg, sizeof msg, "%s on a running target returns CAN_NOT_WAIT", who);
        expect(sr_syscall(&cpu, nid) == CNW_ERR, msg);
        snprintf(msg, sizeof msg,
                 "%s: rejected join set no join target and did not park the caller", who);
        expect(self->state == TH_RUNNING && self->join_target == 0 &&
               self->join_waiting == 0, msg);
        cnw_end(0, token);
    }

    /* ---- 8. control: with both states enabled nothing is rejected ------------- */
    {
        reset_fixture();
        sr_hle_init();
        TCB *self = fixture_thread(0x1d0u, TH_RUNNING, 32);
        s_cur = (int)(self - s_tcb);
        self->started = 1;
        expect(sched_interrupts_enabled() && sched_dispatch_enabled(),
               "control: both interrupts and dispatch are enabled");
        expect(sched_wait_permitted(), "control: waiting is permitted in normal context");

        self->wakeups = 1;
        CpuState c;
        memset(&c, 0, sizeof c);
        expect(sr_syscall(&c, NID_SCE_KERNEL_SLEEP_THREAD) == 0u,
               "control: a satisfiable sleep still returns 0 in normal context");
        s_cur = -1;
    }
}

/* -------------------------------------------------------------------------
 * sceKernelWaitSema / sceKernelWaitSemaCB signal-count validation (issue #43)
 * -------------------------------------------------------------------------
 * The conformance matrix in intr_conformance.h is built from
 * tests/intr/waits.expected, which has no normal-context column for this family.
 * tests/threads/semaphores/wait.expected IS that column, measured on hardware,
 * and these are its cells -- entered through the registered production NIDs, not
 * through helper calls:
 *
 *   L1        need 1 against max 1, count 1        -> OK, count 1 -> 0
 *   L3/L4     need 100 against max 1               -> 800201BD, cur still 0
 *   L5/L6     need -1                              -> 800201BD, cur still 0
 *   L11       need 100 with a 500ms timeout        -> 800201BD, 500ms LEFT
 *   L13       need 0 with a 500ms timeout          -> 800201BD, 500ms LEFT
 *   L15       need -1 with a 500ms timeout         -> 800201BD, 500ms LEFT
 *   L21/L23   id 0 and id 0xDEADBEEF               -> 80020199
 *   L25       a deleted id                         -> 80020199
 *   L28       need 2 against max 1, off-thread     -> 800201BD, no reschedule
 *
 * The `Sema: OK (...cur=N...)` line that follows each failing call in that file
 * is why every rejection here also asserts the count, and the "500ms left" in
 * L11/L13/L15 is why every rejection with a timeout pointer asserts the word is
 * untouched. L5/L6 is the sharp one: before this change `count -= need` ran with
 * a negative need, ADDED to the semaphore, and returned success.
 *
 * s_sync[128] is never cleared between tests and the conformance matrix runs
 * later out of the same table, so every semaphore created here is deleted.
 * ------------------------------------------------------------------------- */
#define NID_WSV_DELETE_SEMA      0x28b6489cu
#define NID_WSV_SIGNAL_SEMA      0x3f53e640u
#define NID_WSV_CREATE_CALLBACK  0xe81caf8fu
#define NID_WSV_NOTIFY_CALLBACK  0xc11ba8c4u
#define WSV_NAMEBUF              0x00250100u
#define WSV_TIMEOUT_PTR          0x00250200u
#define WSV_TIMEOUT_US           500000u
#define WSV_ILLEGAL_COUNT        0x800201bdu   /* SCE_KERNEL_ERROR_ILLEGAL_COUNT */
#define WSV_UNKNOWN_SEMID        0x80020199u   /* SCE_KERNEL_ERROR_UNKNOWN_SEMID */

static TCB *wsv_begin(void) {
    reset_fixture();
    sr_hle_init();
    TCB *self = fixture_thread(0x1e0u, TH_RUNNING, 32);
    s_cur = (int)(self - s_tcb);
    self->started = 1;
    return self;
}

static uint32_t wsv_create(int init, int max) {
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = WSV_NAMEBUF; cpu.r[5] = 0u;
    cpu.r[6] = (uint32_t)init; cpu.r[7] = (uint32_t)max;
    return sr_syscall(&cpu, NID_CNW_CREATE_SEMA);
}

static void wsv_delete(uint32_t uid) {
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = uid;
    (void)sr_syscall(&cpu, NID_WSV_DELETE_SEMA);
}

static uint32_t wsv_wait(uint32_t nid, uint32_t uid, uint32_t need, uint32_t toptr) {
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = uid; cpu.r[5] = need; cpu.r[6] = toptr;
    return sr_syscall(&cpu, nid);
}

/* Read-only count view; -1 when the object does not exist. */
static int wsv_count(uint32_t uid) {
    int count = -1, max = -1;
    if (!sr_hle_test_sema_state(uid, &count, &max)) return -1;
    (void)max;
    return count;
}

/* One rejected request, checked for every way a rejection could still leak:
 * wrong code, mutated count, enqueued waiter, consumed timeout. */
static void wsv_reject(uint32_t nid, const char *who, TCB *self, uint32_t uid,
                       uint32_t need, int use_timeout, uint32_t want,
                       int count_before, const char *what) {
    char msg[256];
    uint32_t toptr = use_timeout ? WSV_TIMEOUT_PTR : 0u;
    if (use_timeout) MEM_W32(WSV_TIMEOUT_PTR, WSV_TIMEOUT_US);

    uint32_t rc = wsv_wait(nid, uid, need, toptr);
    snprintf(msg, sizeof msg, "%s: %s returns 0x%08x", who, what, want);
    expect(rc == want, msg);

    if (count_before >= 0) {
        snprintf(msg, sizeof msg, "%s: %s left the semaphore count at %d", who, what, count_before);
        expect(wsv_count(uid) == count_before, msg);
    }
    snprintf(msg, sizeof msg, "%s: %s enqueued no waiter and left the caller RUNNING", who, what);
    expect(self->state == TH_RUNNING && self->wait_obj == 0, msg);
    if (use_timeout) {
        snprintf(msg, sizeof msg, "%s: %s did not consume the supplied timeout", who, what);
        expect(MEM_R32(WSV_TIMEOUT_PTR) == WSV_TIMEOUT_US, msg);
    }
}

/* Coroutine for the CB control leg: a VALID sceKernelWaitSemaCB that genuinely
 * blocks, so the "no callback ran" assertion above it is measured against an
 * observable that is known to fire. */
static uint32_t s_wsv_cb_sema;
static uint32_t s_wsv_cb_ret;
static int s_wsv_cb_returned;

static void wsv_cb_coro_body(void *arg) {
    (void)arg;
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = s_wsv_cb_sema; cpu.r[5] = 1u; cpu.r[6] = 0u;
    s_wsv_cb_ret = sr_syscall(&cpu, NID_CNW_WAIT_SEMA_CB);
    s_wsv_cb_returned = 1;
    selftest_park_on_scheduler();
}

static void test_wait_sema_count_validation(void) {
    char msg[256];

    for (int i = 0; i < 2; i++) {
        const uint32_t nid = i ? NID_CNW_WAIT_SEMA_CB : NID_CNW_WAIT_SEMA;
        const char *who = i ? "sceKernelWaitSemaCB" : "sceKernelWaitSema";

        /* ---- 1. boundaries against maxCount 1 with the count available ----- */
        TCB *self = wsv_begin();
        uint32_t sema = wsv_create(1, 1);
        snprintf(msg, sizeof msg, "%s fixture: semaphore starts at count 1, max 1", who);
        expect(wsv_count(sema) == 1, msg);

        wsv_reject(nid, who, self, sema, 0u, 0, WSV_ILLEGAL_COUNT, 1, "need 0");
        wsv_reject(nid, who, self, sema, 0xFFFFFFFFu, 0, WSV_ILLEGAL_COUNT, 1, "need -1");
        wsv_reject(nid, who, self, sema, 2u, 0, WSV_ILLEGAL_COUNT, 1, "need maxCount+1");

        snprintf(msg, sizeof msg, "%s: need 1 with the count available succeeds", who);
        expect(wsv_wait(nid, sema, 1u, 0u) == 0u, msg);
        snprintf(msg, sizeof msg, "%s: the satisfied wait decremented the count to 0", who);
        expect(wsv_count(sema) == 0, msg);
        wsv_delete(sema);

        /* ---- 2. need == maxCount is legal; maxCount+1 is not (max 3) ------- */
        sema = wsv_create(3, 3);
        wsv_reject(nid, who, self, sema, 4u, 0, WSV_ILLEGAL_COUNT, 3, "need 4 against maxCount 3");
        snprintf(msg, sizeof msg, "%s: need == maxCount with enough count succeeds", who);
        expect(wsv_wait(nid, sema, 3u, 0u) == 0u, msg);
        snprintf(msg, sizeof msg, "%s: the maxCount wait decremented by exactly maxCount", who);
        expect(wsv_count(sema) == 0, msg);
        wsv_delete(sema);

        sema = wsv_create(2, 3);
        snprintf(msg, sizeof msg, "%s: a partial take of an available count succeeds", who);
        expect(wsv_wait(nid, sema, 1u, 0u) == 0u, msg);
        snprintf(msg, sizeof msg, "%s: the partial take decremented by exactly 1", who);
        expect(wsv_count(sema) == 1, msg);
        wsv_delete(sema);

        /* ---- 3. a negative request cannot INCREASE the count --------------- */
        /* count 0: the old `m->count -= need` would have made this 1 and
         * returned success. wait.expected L5/L6 measures failure with cur=0. */
        sema = wsv_create(0, 1);
        wsv_reject(nid, who, self, sema, 0xFFFFFFFFu, 0, WSV_ILLEGAL_COUNT, 0,
                   "need -1 against count 0 (the state-corruption case)");
        wsv_delete(sema);

        /* ---- 4. a rejected request does not consume the timeout (L11/13/15) */
        sema = wsv_create(0, 1);
        wsv_reject(nid, who, self, sema, 0u,          1, WSV_ILLEGAL_COUNT, 0, "need 0 with a timeout");
        wsv_reject(nid, who, self, sema, 0xFFFFFFFFu, 1, WSV_ILLEGAL_COUNT, 0, "need -1 with a timeout");
        wsv_reject(nid, who, self, sema, 9u,          1, WSV_ILLEGAL_COUNT, 0, "need 9 against maxCount 1 with a timeout");
        wsv_delete(sema);

        /* ---- 5. unknown, invalid and deleted ids (L21, L23, L25) ----------- */
        wsv_reject(nid, who, self, 0u, 1u, 0, WSV_UNKNOWN_SEMID, -1, "id 0");
        wsv_reject(nid, who, self, 0xDEADBEEFu, 1u, 0, WSV_UNKNOWN_SEMID, -1, "id 0xDEADBEEF");

        sema = wsv_create(1, 1);
        wsv_delete(sema);
        wsv_reject(nid, who, self, sema, 1u, 0, WSV_UNKNOWN_SEMID, -1, "a deleted id");

        /* UNRESOLVED PRECEDENCE. No cited hardware cell combines an unknown id
         * with an invalid count, so this pins the corroborated object-before-
         * count ordering as a regression guard -- it is NOT a hardware claim.
         * See docs/PSP_INTR_WAITS_MATRIX.md. */
        wsv_reject(nid, who, self, 0u, 0u, 1, WSV_UNKNOWN_SEMID, -1,
                   "id 0 with need 0 (unknown id wins; combined order not hardware-measured)");
        s_cur = -1;
    }

    /* ---- 6. a rejected CB wait never reaches callback delivery ------------- */
    {
        TCB *self = wsv_begin();
        s_oracle_mode = 1;
        s_oracle_callback_calls = 0;

        static const char cbname[] = "wsv-cb";
        for (size_t k = 0; k < sizeof cbname; k++)
            MEM_W8(WSV_NAMEBUF + (uint32_t)k, (uint8_t)cbname[k]);

        CpuState cpu;
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = WSV_NAMEBUF; cpu.r[5] = ORACLE_CALLBACK_ENTRY; cpu.r[6] = 0x55u;
        uint32_t cb = sr_syscall(&cpu, NID_WSV_CREATE_CALLBACK);
        expect(cb > 0 && sr_callback_is_valid(cb),
               "WaitSemaCB probe: callback registered through CreateCallback");
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = cb; cpu.r[5] = 1u;
        expect(sr_syscall(&cpu, NID_WSV_NOTIFY_CALLBACK) == 0u,
               "WaitSemaCB probe: callback notified and now pending");
        expect(s_oracle_callback_calls == 0,
               "WaitSemaCB probe: notifying alone dispatched nothing");

        /* count 0 < need 9, so the OLD code entered the callback loop here. */
        uint32_t sema = wsv_create(0, 1);
        expect(wsv_wait(NID_CNW_WAIT_SEMA_CB, sema, 9u, 0u) == WSV_ILLEGAL_COUNT,
               "sceKernelWaitSemaCB rejects an oversized request with a callback pending");
        expect(s_oracle_callback_calls == 0,
               "the rejected CB wait ran no callback-delivery loop");
        expect(self->state == TH_RUNNING && self->wait_obj == 0 && self->is_cb_wait == 0,
               "the rejected CB wait entered no callback wait");
        expect(wsv_count(sema) == 0, "the rejected CB wait left the count at 0");

        /* Control: the same observable DOES fire once the request is valid and
         * genuinely blocks, so the assertions above are not passing on a probe
         * that could never have fired. */
        s_wsv_cb_sema = sema;
        s_wsv_cb_ret = 0xFFFFFFFFu;
        s_wsv_cb_returned = 0;
        self->coro = sr_coro_create(wsv_cb_coro_body, NULL, (size_t)4 << 20);
        expect(self->coro != NULL, "WaitSemaCB control coroutine created");
        if (self->coro) sr_coro_switch(self->coro);

        expect(s_oracle_callback_calls == 1,
               "control: a VALID blocking CB wait did dispatch the pending callback");
        expect(self->state == TH_WAIT_OBJ && self->wait_obj == sema,
               "control: the valid CB wait blocked on the semaphore");
        expect(self->is_cb_wait == 1, "control: the blocked CB wait is marked as a callback wait");
        expect(s_wsv_cb_returned == 0, "control: the valid CB wait has not returned yet");

        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = sema; cpu.r[5] = 1u;
        (void)sr_syscall(&cpu, NID_WSV_SIGNAL_SEMA);
        if (self->state == TH_READY && self->coro) {
            s_cur = (int)(self - s_tcb);
            sr_coro_switch(self->coro);
        }
        expect(s_wsv_cb_returned == 1 && s_wsv_cb_ret == 0u,
               "control: the valid CB wait resumed and succeeded after SignalSema");
        expect(wsv_count(sema) == 0,
               "control: the resumed CB wait decremented exactly its signal count");

        if (self->coro) { sr_coro_destroy(self->coro); self->coro = NULL; }
        wsv_delete(sema);
        s_oracle_mode = 0;
        s_oracle_callback_calls = 0;
        s_cur = -1;
    }
}

/* -------------------------------------------------------------------------
 * #70 slice C: an expired timed WAIT-OBJECT enters strict-priority scheduling
 * -------------------------------------------------------------------------
 * sched_preempt() now promotes a timed wait whose deadline has passed before it
 * applies the strict-priority rule. The scheduler-only regression for that drives
 * a synthetic TH_WAIT_DELAY TCB; this one drives the real TH_WAIT_OBJ path end to
 * end, through the registered production NIDs and the production handler:
 *
 *   sceKernelWaitSema / sceKernelWaitSemaCB
 *     -> h_WaitSema / h_WaitSemaCB (hle.c)
 *       -> sched_block_on_timeout() (sched.c)   TH_WAIT_OBJ, wake = deadline
 *         -> sched_promote_expired_waits() via sched_preempt()
 *
 * A priority-16 thread enters an actual timed wait on a semaphore that is never
 * signalled, the deterministic scheduler clock is advanced to exactly the
 * deadline, and a priority-40 runner then reaches an eligible sched_preempt()
 * boundary.
 *
 * Promoting EARLIER must not change what the guest is told, so the timeout code,
 * the semaphore count and the wait bookkeeping are all asserted on the far side
 * of the resume: an earlier promotion must not turn a timeout into a success,
 * consume a count that was never available, or leave a ghost waiter that a later
 * signal would be absorbed by.
 * ------------------------------------------------------------------------- */
#define SLC_TIMEOUT_PTR   0x00250300u
#define SLC_TIMEOUT_US    1000u
#define SLC_WAIT_TIMEOUT  0x800201a8u   /* SCE_KERNEL_ERROR_WAIT_TIMEOUT */

static uint32_t s_slc_nid;
static uint32_t s_slc_sema;
static uint32_t s_slc_ret;
static int      s_slc_returned;
static uint32_t s_slc_signal_ret;
static int      s_slc_runner_fellthrough;

/* The waiter runs the wait on its own coroutine, exactly as a guest thread does:
 * sched_block_on_timeout() switches away from inside the syscall, and the return
 * value is only observable once the scheduler resumes it. */
static void slc_waiter_body(void *arg) {
    (void)arg;
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = s_slc_sema; cpu.r[5] = 1u; cpu.r[6] = SLC_TIMEOUT_PTR;
    s_slc_ret = sr_syscall(&cpu, s_slc_nid);
    s_slc_returned = 1;
    selftest_park_on_scheduler();
}

/* The priority-40 runner also gets its own coroutine, so the preemption transfer
 * out of sched_preempt() is a genuine child-to-scheduler switch rather than a
 * self-switch on the adopted scheduler coroutine.
 *
 * The body deliberately does nothing after the boundary except record that it got
 * there. When the promotion is in place sched_preempt() does NOT return here -- it
 * transfers to the scheduler, which is the behaviour under test -- so reaching the
 * line below is itself the negative observation. */
static void slc_runner_body(void *arg) {
    (void)arg;
    /* The deterministic clock reaches the waiter's deadline EXACTLY -- due, not
     * overshot -- and this thread then hits an eligible strict-priority boundary. */
    s_vtime_us = SLC_TIMEOUT_US;
    sched_preempt();
    s_slc_runner_fellthrough = 1;
    selftest_park_on_scheduler();
}

static void test_expired_timed_sema_wait_enters_strict_priority(uint32_t nid,
                                                                const char *who) {
    char msg[256];
    reset_fixture();
    sr_hle_init();

    TCB *runner = fixture_thread(0x1f0u, TH_RUNNING, 40);   /* holds the CPU */
    TCB *waiter = fixture_thread(0x1f1u, TH_READY, 16);     /* numerically stronger */
    const int runner_idx = (int)(runner - s_tcb);
    const int waiter_idx = (int)(waiter - s_tcb);
    runner->started = 1;
    s_cur = runner_idx;

    /* count 0 against need 1: the wait must genuinely block, never take a count. */
    uint32_t sema = wsv_create(0, 1);
    snprintf(msg, sizeof msg, "%s slice C: fixture semaphore starts at count 0", who);
    expect(wsv_count(sema) == 0, msg);

    MEM_W32(SLC_TIMEOUT_PTR, SLC_TIMEOUT_US);
    s_slc_nid = nid; s_slc_sema = sema;
    s_slc_ret = 0xFFFFFFFFu; s_slc_returned = 0;

    waiter->started = 1;
    waiter->coro = sr_coro_create(slc_waiter_body, NULL, (size_t)4 << 20);
    snprintf(msg, sizeof msg, "%s slice C: waiter coroutine created", who);
    expect(waiter->coro != NULL, msg);
    if (!waiter->coro) { wsv_delete(sema); s_cur = -1; return; }
    s_cur = waiter_idx;
    waiter->state = TH_RUNNING;
    sr_coro_switch(waiter->coro);

    /* The production handler blocked it on the object with a finite deadline. */
    snprintf(msg, sizeof msg, "%s slice C: the timed wait blocked on the semaphore object", who);
    expect(waiter->state == TH_WAIT_OBJ && waiter->wait_obj == sema, msg);
    snprintf(msg, sizeof msg, "%s slice C: the deadline is the supplied timeout", who);
    expect(waiter->wake == (uint64_t)SLC_TIMEOUT_US, msg);
    snprintf(msg, sizeof msg, "%s slice C: the blocking wait has not returned yet", who);
    expect(s_slc_returned == 0, msg);
    snprintf(msg, sizeof msg, "%s slice C: the blocking wait consumed no count", who);
    expect(wsv_count(sema) == 0, msg);

    /* Hand the CPU to the priority-40 runner, which advances the clock to the
     * deadline and reaches the eligible sched_preempt() boundary from inside its
     * own coroutine. */
    s_slc_runner_fellthrough = 0;
    s_slc_signal_ret = 0xFFFFFFFFu;
    runner->coro = sr_coro_create(slc_runner_body, NULL, (size_t)4 << 20);
    snprintf(msg, sizeof msg, "%s slice C: runner coroutine created", who);
    expect(runner->coro != NULL, msg);
    if (!runner->coro) { wsv_delete(sema); s_cur = -1; return; }
    s_cur = runner_idx;
    runner->state = TH_RUNNING;
    sr_coro_switch(runner->coro);

    snprintf(msg, sizeof msg,
             "%s slice C (A): the expired timed WAIT_OBJ waiter becomes runnable at the boundary", who);
    expect(waiter->state == TH_READY, msg);
    snprintf(msg, sizeof msg,
             "%s slice C (B): the priority-40 runner is preempted by the expired priority-16 waiter", who);
    expect(runner->state == TH_READY, msg);
    snprintf(msg, sizeof msg,
             "%s slice C (B): the expired waiter wins strict-priority selection", who);
    expect(pick_next() == waiter_idx, msg);
    snprintf(msg, sizeof msg,
             "%s slice C (B): the boundary transferred control out of the runner", who);
    expect(s_slc_runner_fellthrough == 0, msg);

    /* Resume it the way sched_run would, and read what the guest is actually told. */
    s_cur = waiter_idx;
    waiter->state = TH_RUNNING;
    sr_coro_switch(waiter->coro);

    snprintf(msg, sizeof msg, "%s slice C: the timed wait returned after promotion", who);
    expect(s_slc_returned == 1, msg);
    snprintf(msg, sizeof msg,
             "%s slice C (C): the guest receives SCE_KERNEL_ERROR_WAIT_TIMEOUT (0x800201a8)", who);
    expect(s_slc_ret == SLC_WAIT_TIMEOUT, msg);
    snprintf(msg, sizeof msg,
             "%s slice C (F): earlier promotion manufactured no success result", who);
    expect(s_slc_ret != 0u, msg);
    snprintf(msg, sizeof msg,
             "%s slice C (D): the timed-out wait left the semaphore count unchanged", who);
    expect(wsv_count(sema) == 0, msg);
    snprintf(msg, sizeof msg,
             "%s slice C (E): the resumed thread is in no wait state", who);
    expect(waiter->state != TH_WAIT_OBJ && waiter->state != TH_WAIT_DELAY, msg);
    snprintf(msg, sizeof msg,
             "%s slice C (E): callback-wait bookkeeping is cleared on the way out", who);
    expect(waiter->is_cb_wait == 0, msg);

    /* The sharp half of (E): the timed-out thread's wait_obj still names the
     * semaphore, so a signal arriving afterwards must raise the count rather than
     * be handed to a ghost waiter. Issued with no current thread, so it cannot be
     * reached by the runner falling through its boundary. */
    waiter->state = TH_DORMANT;         /* it took its result and stopped competing */
    s_cur = -1;
    {
        CpuState cpu;
        memset(&cpu, 0, sizeof cpu);
        cpu.r[4] = sema; cpu.r[5] = 1u;
        s_slc_signal_ret = sr_syscall(&cpu, NID_WSV_SIGNAL_SEMA);
    }
    snprintf(msg, sizeof msg, "%s slice C: the post-timeout signal succeeded", who);
    expect(s_slc_signal_ret == 0u, msg);
    snprintf(msg, sizeof msg,
             "%s slice C (E): a later signal is not absorbed by the timed-out waiter", who);
    expect(wsv_count(sema) == 1, msg);

    if (waiter->coro) { sr_coro_destroy(waiter->coro); waiter->coro = NULL; }
    if (runner->coro) { sr_coro_destroy(runner->coro); runner->coro = NULL; }
    wsv_delete(sema);
    s_cur = -1;
}

static void test_expired_timed_object_waits_enter_strict_priority(void) {
    test_expired_timed_sema_wait_enters_strict_priority(NID_CNW_WAIT_SEMA,
                                                        "sceKernelWaitSema");
    test_expired_timed_sema_wait_enters_strict_priority(NID_CNW_WAIT_SEMA_CB,
                                                        "sceKernelWaitSemaCB");
}

/* -------------------------------------------------------------------------
 * PR-C1: the blocking FPL allocate forms and the context rule
 * -------------------------------------------------------------------------
 * sceKernelAllocateFpl / ...CB used to BE sceKernelTryAllocateFpl -- one handler
 * behind three NIDs. waits.expected puts the context decision ahead of the FPL
 * object lookup for the blocking pair (bad id answers CAN_NOT_WAIT at L102/L103
 * and L108/L109, not the bad-id error), so they had to be split off.
 *
 * The matrix pins the return values of those eight cells. What it cannot show is
 * on this side of the split: that the rejection happens before ANY mutation, that
 * normal context is untouched, and above all that sceKernelTryAllocateFpl did not
 * come along for the ride. That last group is a regression pin on current
 * behavior, not a hardware claim -- waits.cpp never probes a Try form, so there
 * is no oracle cell for it.
 * ------------------------------------------------------------------------- */
#define NID_FPL_CREATE        0xc07bb470u
#define NID_FPL_DELETE        0xed1410e0u
#define NID_FPL_ALLOCATE      0xd979e9bfu
#define NID_FPL_ALLOCATE_CB   0xe7282cb6u
#define NID_FPL_TRY_ALLOCATE  0x623ae665u
#define FPL_BAD_ID_ERR        0x800200d3u
#define FPL_EXHAUSTED_ERR     0x800200d9u
#define FPL_NAMEBUF           0x00240900u
#define FPL_OUTPTR            0x00240940u
#define FPL_SENTINEL          0xfeedfaceu
#define FPL_BSIZE             0x100u
#define FPL_NBLOCKS           0x10

/* Fresh FPL_NBLOCKS x FPL_BSIZE pool. Returns 0 on arrangement failure. */
static uint32_t fpl_make_pool(void) {
    CpuState setup;
    memset(&setup, 0, sizeof setup);
    setup.r[4] = FPL_NAMEBUF; setup.r[5] = 0; setup.r[6] = 0; setup.r[7] = FPL_BSIZE;
    setup.r[8] = (uint32_t)FPL_NBLOCKS;     /* numBlocks, read via stack_arg(0) */
    return sr_syscall(&setup, NID_FPL_CREATE);
}

/* s_fpls[] has FPL_MAX=16 slots, reset_fixture() does not clear it, and the
 * conformance matrix already uses all 16. Every pool this test creates must go
 * back or the matrix starves. */
static void fpl_free_pool(uint32_t uid) {
    CpuState c;
    memset(&c, 0, sizeof c);
    c.r[4] = uid;
    (void)sr_syscall(&c, NID_FPL_DELETE);
}

static uint32_t fpl_call(uint32_t nid, uint32_t uid, uint32_t outptr) {
    CpuState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = uid; cpu.r[5] = outptr; cpu.r[6] = 0u;   /* NULL timeout */
    return sr_syscall(&cpu, nid);
}

static void test_allocate_fpl_context_precedence(void) {
    char msg[160];

    /* ---- 1+2. context beats the object lookup, on BOTH disabled states ------
     * A bad uid would return FPL_BAD_ID_ERR if the lookup ran first. Asserting
     * CAN_NOT_WAIT here is what proves the ordering, not merely that some error
     * came back. Both legs run so dispatch-disabled is not inferred from the
     * interrupt-disabled result. */
    for (int intr_off = 0; intr_off < 2; intr_off++) {
        for (int cb = 0; cb < 2; cb++) {
            const uint32_t nid = cb ? NID_FPL_ALLOCATE_CB : NID_FPL_ALLOCATE;
            const char *who = cb ? "sceKernelAllocateFplCB" : "sceKernelAllocateFpl";
            const char *ctx = intr_off ? "interrupts disabled" : "dispatch disabled";
            uint32_t token;

            (void)cnw_begin(intr_off, &token);
            MEM_W32(FPL_OUTPTR, FPL_SENTINEL);
            uint32_t rc = fpl_call(nid, 0u /* bad uid */, FPL_OUTPTR);
            snprintf(msg, sizeof msg,
                     "%s with %s returns CAN_NOT_WAIT before the bad-uid lookup", who, ctx);
            expect(rc == CNW_ERR, msg);
            snprintf(msg, sizeof msg,
                     "%s rejected with %s writes no output pointer", who, ctx);
            expect(MEM_R32(FPL_OUTPTR) == FPL_SENTINEL, msg);
            cnw_end(intr_off, token);
        }
    }

    /* ---- 3. a VALID pool is rejected without consuming a block --------------
     * Proved by capacity rather than by address: pool bases move between
     * fixtures, but the block COUNT does not. The pool holds FPL_NBLOCKS blocks,
     * so after a rejected allocate all of them must still be handed out and only
     * the one after that may report exhaustion. Had the rejection consumed a
     * block, the final allocation would fail early. */
    for (int intr_off = 0; intr_off < 2; intr_off++) {
        const char *ctx = intr_off ? "interrupts disabled" : "dispatch disabled";
        uint32_t token;

        (void)cnw_begin(intr_off, &token);
        uint32_t uid = fpl_make_pool();
        expect(uid != 0u, "control: FPL pool created for the rejection leg");
        MEM_W32(FPL_OUTPTR, FPL_SENTINEL);
        uint32_t rc = fpl_call(NID_FPL_ALLOCATE, uid, FPL_OUTPTR);
        snprintf(msg, sizeof msg, "AllocateFpl on a VALID pool with %s returns CAN_NOT_WAIT", ctx);
        expect(rc == CNW_ERR, msg);
        snprintf(msg, sizeof msg, "AllocateFpl rejected with %s writes no data pointer", ctx);
        expect(MEM_R32(FPL_OUTPTR) == FPL_SENTINEL, msg);
        cnw_end(intr_off, token);

        int handed_out = 0;
        uint32_t prev = 0u;
        int contiguous = 1;
        for (int i = 0; i < FPL_NBLOCKS; i++) {
            if (fpl_call(NID_FPL_ALLOCATE, uid, FPL_OUTPTR) != 0u) break;
            uint32_t got = MEM_R32(FPL_OUTPTR);
            if (i > 0 && got != prev + FPL_BSIZE) contiguous = 0;
            prev = got;
            handed_out++;
        }
        snprintf(msg, sizeof msg,
                 "AllocateFpl rejected with %s consumed no block: all %d remain", ctx, FPL_NBLOCKS);
        expect(handed_out == FPL_NBLOCKS, msg);
        snprintf(msg, sizeof msg,
                 "AllocateFpl after a %s rejection still walks the pool one block at a time", ctx);
        expect(contiguous, msg);
        snprintf(msg, sizeof msg,
                 "the pool really was exhausted at %d blocks, so the count is a real bound",
                 FPL_NBLOCKS);
        expect(fpl_call(NID_FPL_ALLOCATE, uid, FPL_OUTPTR) == FPL_EXHAUSTED_ERR, msg);
        fpl_free_pool(uid);
    }

    /* ---- 4. normal context is exactly what it was before the split ---------- */
    {
        reset_fixture();
        sr_hle_init();
        TCB *self = fixture_thread(0x1d2u, TH_RUNNING, 32);
        s_cur = (int)(self - s_tcb); self->started = 1;
        expect(sched_wait_permitted(), "control: waiting is permitted in normal context");

        uint32_t uid = fpl_make_pool();
        expect(uid != 0u, "control: FPL pool created in normal context");
        MEM_W32(FPL_OUTPTR, FPL_SENTINEL);
        expect(fpl_call(NID_FPL_ALLOCATE, uid, FPL_OUTPTR) == 0u,
               "normal-context AllocateFpl still returns 0");
        uint32_t first = MEM_R32(FPL_OUTPTR);
        expect(first != FPL_SENTINEL, "normal-context AllocateFpl still writes the data pointer");
        expect(fpl_call(NID_FPL_ALLOCATE_CB, uid, FPL_OUTPTR) == 0u,
               "normal-context AllocateFplCB still returns 0");
        expect(MEM_R32(FPL_OUTPTR) == first + FPL_BSIZE,
               "normal-context AllocateFplCB still advances the cursor by one block");
        expect(fpl_call(NID_FPL_ALLOCATE, 0u, FPL_OUTPTR) == FPL_BAD_ID_ERR,
               "normal-context AllocateFpl still reports the bad-uid error");
        fpl_free_pool(uid);
        s_cur = -1;
    }

    /* ---- 5. sceKernelTryAllocateFpl did NOT change ---------------------------
     * Regression pin on current behavior. The Try form does not block, so the
     * context rule must not reach it: it keeps allocating while interrupts and
     * dispatch are disabled, and keeps its own bad-uid error. No oracle cell
     * covers this -- waits.cpp never probes a Try form. */
    {
        reset_fixture();
        sr_hle_init();
        TCB *self = fixture_thread(0x1d3u, TH_RUNNING, 32);
        s_cur = (int)(self - s_tcb); self->started = 1;
        uint32_t uid = fpl_make_pool();
        expect(uid != 0u, "control: FPL pool created for the TryAllocate pin");
        MEM_W32(FPL_OUTPTR, FPL_SENTINEL);
        expect(fpl_call(NID_FPL_TRY_ALLOCATE, uid, FPL_OUTPTR) == 0u,
               "normal-context TryAllocateFpl still returns 0");
        uint32_t first = MEM_R32(FPL_OUTPTR);
        expect(first != FPL_SENTINEL, "normal-context TryAllocateFpl still writes the data pointer");
        expect(fpl_call(NID_FPL_TRY_ALLOCATE, 0u, FPL_OUTPTR) == FPL_BAD_ID_ERR,
               "normal-context TryAllocateFpl still reports the bad-uid error");
        fpl_free_pool(uid);
        s_cur = -1;
    }
    for (int intr_off = 0; intr_off < 2; intr_off++) {
        const char *ctx = intr_off ? "interrupts disabled" : "dispatch disabled";
        uint32_t token;
        (void)cnw_begin(intr_off, &token);
        uint32_t uid = fpl_make_pool();
        expect(uid != 0u, "control: FPL pool created for the disabled-context Try pin");
        uint32_t base_before;
        MEM_W32(FPL_OUTPTR, FPL_SENTINEL);
        uint32_t rc = fpl_call(NID_FPL_TRY_ALLOCATE, uid, FPL_OUTPTR);
        base_before = MEM_R32(FPL_OUTPTR);
        snprintf(msg, sizeof msg, "TryAllocateFpl still succeeds with %s (not a blocking form)", ctx);
        expect(rc == 0u, msg);
        snprintf(msg, sizeof msg, "TryAllocateFpl still writes its data pointer with %s", ctx);
        expect(base_before != FPL_SENTINEL, msg);
        rc = fpl_call(NID_FPL_TRY_ALLOCATE, uid, FPL_OUTPTR);
        snprintf(msg, sizeof msg, "TryAllocateFpl still advances the cursor with %s", ctx);
        expect(rc == 0u && MEM_R32(FPL_OUTPTR) == base_before + FPL_BSIZE, msg);
        snprintf(msg, sizeof msg, "TryAllocateFpl still reports the bad-uid error with %s", ctx);
        expect(fpl_call(NID_FPL_TRY_ALLOCATE, 0u, FPL_OUTPTR) == FPL_BAD_ID_ERR, msg);
        fpl_free_pool(uid);
        cnw_end(intr_off, token);
    }
}

/* Production-dispatch regression for the low-level ATRAC context ABI.  The
 * calls below enter the same NID registry and sr_syscall path as a generated
 * import stub; the guest buffer is a tiny synthetic RIFF envelope, so no
 * decoder or retail bytes are involved. */
static void test_atrac_context_abi(void) {
    reset_fixture();
    sr_hle_init();

    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));

    cpu.r[4] = 0x1234u;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_ID) == ATRAC_ERROR_INVALID_CODECTYPE,
           "sceAtracGetAtracID rejects an unsupported codec type");

    uint32_t ids[8];
    for (unsigned i = 0; i < 8; i++) {
        cpu.r[4] = ATRAC_CODEC_AT3PLUS;
        ids[i] = sr_syscall(&cpu, NID_SCE_ATRAC_GET_ID);
        expect(ids[i] == i, "sceAtracGetAtracID allocates the next tracked context");
    }
    cpu.r[4] = ATRAC_CODEC_AT3;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_ID) == ATRAC_ERROR_NO_ATRACID,
           "sceAtracGetAtracID reports exhaustion instead of fabricating an ID");

    cpu.r[4] = 0x7fu;
    cpu.r[5] = 0;
    cpu.r[6] = 0;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA) == ATRAC_ERROR_BAD_ATRACID,
           "sceAtracSetData rejects an unallocated context");
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_RELEASE_ID) == ATRAC_ERROR_BAD_ATRACID,
           "sceAtracReleaseAtracID rejects an unallocated context");

    cpu.r[4] = ids[0];
    cpu.r[5] = 0;
    cpu.r[6] = 0;
    uint32_t short_ret = sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA);
    expect(short_ret == ATRAC_ERROR_SIZE_TOO_SMALL,
           "sceAtracSetData rejects a null or short buffer");
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x0bfffffeu;
    cpu.r[6] = 44;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA) == ATRAC_ERROR_SIZE_TOO_SMALL,
           "sceAtracSetData rejects a guest span that crosses the arena boundary");

    /* Minimal RIFF/WAVE/fact envelope: enough to exercise the production
     * sample-count parser without embedding any game-derived bytes. */
    g_mem[0] = 'R'; g_mem[1] = 'I'; g_mem[2] = 'F'; g_mem[3] = 'F';
    /* Deliberately describe a larger track than this supplied prefix: streamed
     * sceAtracSetData calls may provide only the first buffer-sized window. */
    g_mem[4] = 0; g_mem[5] = 0x10; g_mem[6] = 0; g_mem[7] = 0;
    g_mem[8] = 'W'; g_mem[9] = 'A'; g_mem[10] = 'V'; g_mem[11] = 'E';
    g_mem[12] = 'f'; g_mem[13] = 'a'; g_mem[14] = 'c'; g_mem[15] = 't';
    g_mem[16] = 4; g_mem[17] = 0; g_mem[18] = 0; g_mem[19] = 0;
    g_mem[20] = 0; g_mem[21] = 0x10; g_mem[22] = 0; g_mem[23] = 0;
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x08000000u;
    cpu.r[6] = 44;
    uint32_t valid_ret = sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA);
    expect(valid_ret == 0,
           "sceAtracSetData accepts the (id, buffer, size) ABI for a tracked context");

    /* Malformed fact chunks must fail closed and leave a previously configured
     * context untouched.  In particular, a zero-sized chunk must not consume the
     * following sample word, and a wrapped chunk size must not stall the handler. */
    g_mem[16] = 0; g_mem[17] = 0; g_mem[18] = 0; g_mem[19] = 0;
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x08000000u;
    cpu.r[6] = 44;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA) == ATRAC_ERROR_UNKNOWN_FORMAT,
           "sceAtracSetData rejects a fact chunk with no sample payload");
    g_mem[16] = 0xf8; g_mem[17] = 0xff; g_mem[18] = 0xff; g_mem[19] = 0xff;
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x08000000u;
    cpu.r[6] = 44;
    uint32_t wrapped_ret = sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA);
    expect(wrapped_ret == ATRAC_ERROR_UNKNOWN_FORMAT,
           "sceAtracSetData rejects a wrapping chunk size");
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x08000040u;
    cpu.r[6] = 0x08000044u;
    cpu.r[7] = 0x08000048u;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_SOUND_SAMPLE) == 0 &&
           MEM_R32(0x08000040u) == 0x1000u,
           "sceAtracSetData preserves the prior context after malformed input");

    /* An accepted fact-only prefix is honest linear mode: no frame-size or
     * ring metadata is invented, so the stream interface reports nothing
     * writable and remaining frames keep the ALLDATA sentinel. */
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x08000100u;   /* *writePointer */
    cpu.r[6] = 0x08000104u;   /* *writableBytes */
    cpu.r[7] = 0x08000108u;   /* *readOffset */
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_STREAM_DATA_INFO) == 0 &&
           MEM_R32(0x08000100u) == 0x08000000u &&
           MEM_R32(0x08000104u) == 0u &&
           MEM_R32(0x08000108u) == 0u,
           "fact-only prefix: linear mode reports no invented ring contract");
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x0800010cu;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_REMAIN_FRAME) == 0 &&
           MEM_R32(0x0800010cu) == 0xFFFFFFFFu,
           "fact-only prefix: remaining frames stay ALLDATA_IS_ON_MEMORY");

    /* A later SetData carrying a complete header (fmt blockAlign + data
     * offset) must replace the linear mode with the real streaming contract. */
    g_mem[0x100] = 'R'; g_mem[0x101] = 'I'; g_mem[0x102] = 'F'; g_mem[0x103] = 'F';
    g_mem[0x104] = 0; g_mem[0x105] = 0; g_mem[0x106] = 2; g_mem[0x107] = 0;   /* RIFF size 0x20000 */
    g_mem[0x108] = 'W'; g_mem[0x109] = 'A'; g_mem[0x10a] = 'V'; g_mem[0x10b] = 'E';
    g_mem[0x10c] = 'f'; g_mem[0x10d] = 'm'; g_mem[0x10e] = 't'; g_mem[0x10f] = ' ';
    g_mem[0x110] = 16; g_mem[0x111] = 0; g_mem[0x112] = 0; g_mem[0x113] = 0;
    g_mem[0x114] = 1; g_mem[0x115] = 0;                 /* format PCM */
    g_mem[0x116] = 2; g_mem[0x117] = 0;                 /* channels */
    g_mem[0x118] = 0x44; g_mem[0x119] = 0xac; g_mem[0x11a] = 0; g_mem[0x11b] = 0;  /* 44100 */
    g_mem[0x11c] = 0; g_mem[0x11d] = 0; g_mem[0x11e] = 0; g_mem[0x11f] = 0;
    g_mem[0x120] = 0xe8; g_mem[0x121] = 0x02;           /* blockAlign 744 */
    g_mem[0x122] = 16; g_mem[0x123] = 0;                /* bits */
    g_mem[0x124] = 'f'; g_mem[0x125] = 'a'; g_mem[0x126] = 'c'; g_mem[0x127] = 't';
    g_mem[0x128] = 4; g_mem[0x129] = 0; g_mem[0x12a] = 0; g_mem[0x12b] = 0;
    g_mem[0x12c] = 0; g_mem[0x12d] = 0; g_mem[0x12e] = 0x10; g_mem[0x12f] = 0;  /* 0x100000 samples */
    g_mem[0x130] = 'd'; g_mem[0x131] = 'a'; g_mem[0x132] = 't'; g_mem[0x133] = 'a';
    g_mem[0x134] = 0; g_mem[0x135] = 0; g_mem[0x136] = 0x10; g_mem[0x137] = 0;  /* chunk size */
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x08000100u;
    cpu.r[6] = 0x138u;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA) == 0,
           "sceAtracSetData upgrades a linear prefix to a full streaming contract");
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x08000200u;
    cpu.r[6] = 0x08000204u;
    cpu.r[7] = 0x08000208u;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_STREAM_DATA_INFO) == 0 &&
           MEM_R32(0x08000200u) == 0x08000100u &&
           MEM_R32(0x08000204u) == 0x138u &&
           MEM_R32(0x08000208u) == 0x138u,
           "complete header: streaming interface reports writable ring bytes");
    cpu.r[4] = ids[0];
    cpu.r[5] = 0x0800020cu;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_REMAIN_FRAME) == 0 &&
           MEM_R32(0x0800020cu) == 0u,
           "complete header: remaining frames leave the ALLDATA sentinel");

    /* SetDataAndGetID with a malformed track must not hand out a
     * half-configured context: the slot stays available for reuse. */
    cpu.r[4] = ids[7];
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_RELEASE_ID) == 0,
           "sceAtracReleaseAtracID frees id 7 for the malformed-track check");
    g_mem[0x200] = 'R'; g_mem[0x201] = 'I'; g_mem[0x202] = 'F'; g_mem[0x203] = 'F';
    g_mem[0x204] = 0; g_mem[0x205] = 0; g_mem[0x206] = 0; g_mem[0x207] = 0;
    g_mem[0x208] = 'W'; g_mem[0x209] = 'A'; g_mem[0x20a] = 'V'; g_mem[0x20b] = 'E';
    g_mem[0x20c] = 'f'; g_mem[0x20d] = 'a'; g_mem[0x20e] = 'c'; g_mem[0x20f] = 't';
    g_mem[0x210] = 0; g_mem[0x211] = 0; g_mem[0x212] = 0; g_mem[0x213] = 0;  /* sz = 0 */
    cpu.r[4] = 0x08000200u;
    cpu.r[5] = 0x18u;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA_AND_GET_ID) == ATRAC_ERROR_UNKNOWN_FORMAT,
           "sceAtracSetDataAndGetID rejects a malformed track");
    cpu.r[4] = ATRAC_CODEC_AT3PLUS;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_ID) == 7,
           "the rejected SetDataAndGetID slot is reusable, not half-configured");

    /* A chunk whose declared payload runs past the fed bytes must be rejected
     * too: the parser may not read beyond the validated guest span. */
    cpu.r[4] = ids[7];
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_RELEASE_ID) == 0,
           "sceAtracReleaseAtracID frees id 7 for the truncated-payload check");
    g_mem[0x300] = 'R'; g_mem[0x301] = 'I'; g_mem[0x302] = 'F'; g_mem[0x303] = 'F';
    g_mem[0x304] = 0; g_mem[0x305] = 0; g_mem[0x306] = 1; g_mem[0x307] = 0;   /* RIFF size 0x100 */
    g_mem[0x308] = 'W'; g_mem[0x309] = 'A'; g_mem[0x30a] = 'V'; g_mem[0x30b] = 'E';
    g_mem[0x30c] = 'f'; g_mem[0x30d] = 'm'; g_mem[0x30e] = 't'; g_mem[0x30f] = ' ';
    g_mem[0x310] = 16; g_mem[0x311] = 0; g_mem[0x312] = 0; g_mem[0x313] = 0;  /* fmt size */
    g_mem[0x314] = 1; g_mem[0x315] = 0;                 /* format PCM */
    g_mem[0x316] = 2; g_mem[0x317] = 0;                 /* channels */
    g_mem[0x318] = 0x44; g_mem[0x319] = 0xac; g_mem[0x31a] = 0; g_mem[0x31b] = 0;  /* 44100 */
    g_mem[0x31c] = 0; g_mem[0x31d] = 0; g_mem[0x31e] = 0; g_mem[0x31f] = 0;
    g_mem[0x320] = 0xe8; g_mem[0x321] = 0x02;           /* blockAlign 744 */
    g_mem[0x322] = 16; g_mem[0x323] = 0;                /* bits */
    g_mem[0x324] = 'f'; g_mem[0x325] = 'a'; g_mem[0x326] = 'c'; g_mem[0x327] = 't';
    g_mem[0x328] = 4; g_mem[0x329] = 0; g_mem[0x32a] = 0; g_mem[0x32b] = 0;   /* sz = 4, payload cut off */
    cpu.r[4] = 0x08000300u;
    cpu.r[5] = 0x2eu;                                  /* 46 bytes: fact payload truncated */
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA_AND_GET_ID) == ATRAC_ERROR_UNKNOWN_FORMAT,
           "sceAtracSetDataAndGetID rejects a chunk payload past the fed bytes");
    cpu.r[4] = ATRAC_CODEC_AT3PLUS;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_ID) == 7,
           "the truncated-payload rejection also leaves the slot reusable");

    for (unsigned i = 0; i < 8; i++) {
        cpu.r[4] = ids[i];
        expect(sr_syscall(&cpu, NID_SCE_ATRAC_RELEASE_ID) == 0,
               "sceAtracReleaseAtracID releases a tracked context");
    }
}

static TCB *fixture_thread(uint32_t uid, int state, int priority) {
    TCB *thread = &s_tcb[s_ntcb++];
    memset(thread, 0, sizeof(*thread));
    thread->uid = uid;
    thread->state = state;
    thread->priority = priority;
    thread->wake = (uint64_t)-1;
    thread->exit_status = (int32_t)0x800201a4u;
    return thread;
}

/* Issue #88 interrupt/dispatch-context conformance matrix. Included here rather
 * than compiled separately so it reuses this target's production hle.c + sched.c
 * link and its expect()/reset_fixture()/fixture_thread()/park helpers; it must
 * come after those definitions. It adds no production code and changes no
 * handler behavior. */
#include "intr_conformance.h"

static void run_worker(TCB *worker) {
    s_cur = (int)(worker - s_tcb);
    worker->state = TH_RUNNING;
    worker->started = 1;
    memcpy(s_cpu, &worker->saved, sizeof(*s_cpu));
    worker->coro = sr_coro_create(coro_body, worker, (size_t)4 << 20);
    expect(worker->coro != NULL, "resource worker coroutine was created");
    if (worker->coro) sr_coro_switch(worker->coro);
    s_cur = -1;
}

static void test_exit_thread_does_not_wake_launcher(int launcher_wakeups) {
    reset_fixture();

    TCB *launcher = fixture_thread(g_launcher_uid, TH_WAIT_OBJ, 32);
    launcher->sleeping = 1;
    launcher->wait_obj = launcher->uid;
    launcher->wakeups = launcher_wakeups;

    TCB *worker = fixture_thread(0x13au, TH_READY, 40);
    TCB *joiner = fixture_thread(0x13bu, TH_WAIT_OBJ, 41);
    joiner->wait_obj = worker->uid; /* sceKernelWaitThreadEnd(worker) */

    s_exit_argument = -17;
    run_worker(worker);

    SrThreadRunStatus launcher_status;
    SrThreadRunStatus worker_status;
    SrThreadRunStatus joiner_status;
    expect(s_exit_dispatches == 1,
           "synthetic import stub dispatched the registered ExitThread NID once");
    expect(sr_last_nid == NID_SCE_KERNEL_EXIT_THREAD,
           "production dispatcher recorded sceKernelExitThread as the executed NID");
    expect(sched_thread_run_status(worker->uid, &worker_status) == 0,
           "worker status remains queryable after exit");
    expect(worker_status.status == PSP_THREAD_STOPPED,
           "ExitThread leaves the caller dormant/stopped");
    expect(sched_thread_exit_status(worker->uid) == 0x800200d2u,
           "ExitThread normalizes the measured signed-negative status");
    expect(sched_thread_run_status(joiner->uid, &joiner_status) == 0,
           "joiner status remains queryable");
    expect(joiner_status.status == PSP_THREAD_READY,
           "ExitThread releases a thread waiting for the caller to end");
    expect(joiner_status.waitType == PSP_WAIT_NONE && joiner_status.waitId == 0,
           "released joiner no longer reports a stale wait reason or object");
    expect(sched_thread_run_status(launcher->uid, &launcher_status) == 0,
           "launcher status remains queryable");
    expect(launcher_status.status == PSP_THREAD_WAITING &&
           launcher_status.waitType == PSP_WAIT_SLEEP &&
           launcher_status.waitId == launcher->uid,
           "unrelated sleeping launcher stays asleep on its own wait object");
    expect(launcher_status.wakeupCount == (uint32_t)launcher_wakeups,
           "ExitThread preserves the launcher's existing wakeup count exactly");

    if (worker->coro) {
        sr_coro_destroy(worker->coro);
        worker->coro = NULL;
    }
}

static void test_explicit_exit_status_exact(int32_t supplied, uint32_t expected) {
    reset_fixture();
    TCB *worker = fixture_thread(0x13cu, TH_READY, 40);
    s_exit_argument = supplied;
    run_worker(worker);
    expect(s_exit_dispatches == 1,
           "exact-status control dispatches the production ExitThread NID once");
    expect(sr_last_nid == NID_SCE_KERNEL_EXIT_THREAD,
           "exact-status control reaches sceKernelExitThread through sr_syscall");
    SrThreadRunStatus worker_status;
    expect(sched_thread_run_status(worker->uid, &worker_status) == 0 &&
           worker_status.status == PSP_THREAD_STOPPED &&
           worker_status.waitType == PSP_WAIT_NONE && worker_status.waitId == 0,
           "exact-status ExitThread leaves a stopped thread with no wait object");
    expect(sched_thread_exit_status(worker->uid) == expected,
           "exact-status ExitThread exposes the measured latched value");
    if (worker->coro) {
        sr_coro_destroy(worker->coro);
        worker->coro = NULL;
    }
}

#define NID_SCE_KERNEL_EXIT_DELETE_THREAD 0x809ce29bu
#define NID_SCE_KERNEL_CREATE_THREAD     0x446d8de6u
#define NID_SCE_KERNEL_START_THREAD      0xf475845du
#define NID_SCE_KERNEL_DELETE_THREAD     0x9fa03cd3u
#define NID_SCE_KERNEL_GET_EXIT_STATUS   0x3b183e26u
#define NID_SCE_KERNEL_WAKEUP_THREAD     0xd59ead2fu
#define NID_SCE_KERNEL_TERMINATE_DELETE  0x383f7bccu
#define NID_SCE_KERNEL_CREATE_CALLBACK   0xe81caf8fu
#define NID_SCE_KERNEL_DELETE_CALLBACK   0xedba5844u

static int guest_hash_contains_uid(uint32_t uid) {
    uint32_t bucket = uid % 32u;
    for (uint32_t node = 0x0030aa88u; node != 0u; node = MEM_R32(node)) {
        if (MEM_R32(node + 0x84u + bucket * 4u) == uid) return 1;
    }
    return 0;
}

static int host_libc_contains_uid(uint32_t uid) {
    for (int i = 0; i < MAXTHREADS; i++)
        if (s_libc_threads[i].in_use && s_libc_threads[i].uid == uid) return 1;
    return 0;
}

static void test_thread_delete_lifecycle_and_cleanup(void) {
    reset_fixture();
    sr_hle_init();

    TCB *owner = fixture_thread(0x180u, TH_RUNNING, 20);
    s_cur = (int)(owner - s_tcb);

    /* Deleting the current/running object is rejected rather than silently
     * tearing down the caller. */
    s_cpu->r[4] = owner->uid;
    expect(sr_syscall(s_cpu, NID_SCE_KERNEL_DELETE_THREAD) == 0x800201a4u,
           "DeleteThread(current running) returns NOT_DORMANT");
    expect(sched_terminate_thread(owner->uid) == 0x80020197u,
           "TerminateDeleteThread(current) returns ILLEGAL_THID");

    /* Create a real scheduler object so the target owns a stack, libc/reent
     * record, and a callback registered through the production NID path. */
    uint32_t target_uid = sched_create_thread(0x0800db00u, 40, 0x2000u);
    TCB *target = tcb_by_uid(target_uid);
    expect(target_uid != 0 && target != NULL, "lifecycle target object created");
    uint32_t target_stack = target ? target->stack_base : 0u;
    if (target) {
        target->state = TH_READY;
        target->started = 1;
    }

    static const char callback_name[] = "delete-owner";
    for (size_t i = 0; i < sizeof(callback_name); i++)
        MEM_W8(0x08001000u + (uint32_t)i, (uint8_t)callback_name[i]);
    s_cur = target ? (int)(target - s_tcb) : -1;
    s_cpu->r[4] = 0x08001000u;
    s_cpu->r[5] = ORACLE_CALLBACK_ENTRY;
    s_cpu->r[6] = 0x55u;
    uint32_t callback_uid = sr_syscall(s_cpu, NID_SCE_KERNEL_CREATE_CALLBACK);
    expect(callback_uid > 0 && sr_callback_is_valid(callback_uid),
           "target callback is registered through CreateCallback");

    TCB *joiner = fixture_thread(0x181u, TH_WAIT_OBJ, 41);
    joiner->wait_obj = target_uid;
    joiner->join_target = target_uid;
    joiner->join_waiting = 1;
    s_cur = (int)(owner - s_tcb);

    s_cpu->r[4] = target_uid;
    uint32_t terminate_delete = sr_syscall(s_cpu, NID_SCE_KERNEL_TERMINATE_DELETE);
    expect(terminate_delete == 0, "TerminateDeleteThread terminates and removes a target");
    expect(target && target->deleted && target->stack_released && target->resources_released,
           "TerminateDeleteThread marks the object deleted and releases resources once");
    expect(!host_libc_contains_uid(target_uid) && !guest_hash_contains_uid(target_uid),
           "TerminateDeleteThread removes host libc and guest reent ownership");
    expect(!sr_callback_is_valid(callback_uid),
           "TerminateDeleteThread removes callbacks owned by the target");
    expect(joiner->state == TH_READY && joiner->join_result_valid &&
           joiner->join_result == 0x800201acu,
           "TerminateDeleteThread wakes a waiting joiner with THREAD_TERMINATED");

    /* The removed UID is rejected by every public follow-up operation. */
    s_cpu->r[4] = target_uid;
    expect(sr_syscall(s_cpu, NID_SCE_KERNEL_GET_EXIT_STATUS) == 0x80020198u,
           "GetThreadExitStatus rejects a deleted UID");
    expect(sr_syscall(s_cpu, NID_SCE_KERNEL_START_THREAD) == 0x80020198u,
           "StartThread rejects a deleted UID");
    expect(sr_syscall(s_cpu, NID_SCE_KERNEL_WAKEUP_THREAD) == 0x80020198u,
           "WakeupThread rejects a deleted UID");
    expect(sched_is_dormant(target_uid) == 0,
           "deleted UID is not reported as a dormant thread");

    /* The exact freed range is reusable without overlapping the owner's live
     * object.  This is the stack-lifetime side of #16's contract. */
    uint32_t replacement_uid = sched_create_thread(0x0800db01u, 40, 0x2000u);
    TCB *replacement = tcb_by_uid(replacement_uid);
    expect(replacement_uid != 0 && replacement && replacement->stack_base == target_stack,
           "a deleted thread's stack range is reclaimed and reused exactly");
    if (replacement) expect(sched_delete_thread(replacement_uid) == 0,
                            "reclaimed replacement object deletes cleanly");
}

static void test_start_thread_error_semantics(void) {
    reset_fixture();
    sr_hle_init();

    TCB *cur = fixture_thread(0x195u, TH_RUNNING, 40);
    s_cur = (int)(cur - s_tcb);

    /* 1. Null UID -> 0x80020197 (SCE_KERNEL_ERROR_ILLEGAL_THID) */
    s_cpu->r[4] = 0;
    expect(sr_syscall(s_cpu, NID_SCE_KERNEL_START_THREAD) == 0x80020197u,
           "sceKernelStartThread rejects null UID with ILLEGAL_THID");

    /* 2. Invalid/unknown UID -> 0x80020198 (SCE_KERNEL_ERROR_UNKNOWN_THID) */
    s_cpu->r[4] = 0xDEADBEEFu;
    expect(sr_syscall(s_cpu, NID_SCE_KERNEL_START_THREAD) == 0x80020198u,
           "sceKernelStartThread rejects invalid UID with UNKNOWN_THID");

    /* 3. Start current running thread -> 0x800201a4 (SCE_KERNEL_ERROR_NOT_DORMANT) */
    s_cpu->r[4] = cur->uid;
    expect(sr_syscall(s_cpu, NID_SCE_KERNEL_START_THREAD) == 0x800201a4u,
           "sceKernelStartThread rejects current running thread with NOT_DORMANT");

    /* 4. True start-twice sequence: start DORMANT target thread once (0), then start again (NOT_DORMANT) */
    uint32_t target_uid = sched_create_thread(0x0800db10u, 40, 0x2000u);
    TCB *target = tcb_by_uid(target_uid);
    expect(target_uid != 0 && target != NULL, "start-twice target thread created");
    s_cpu->r[4] = target_uid;
    s_cpu->r[5] = 0;
    s_cpu->r[6] = 0;
    expect(sr_syscall(s_cpu, NID_SCE_KERNEL_START_THREAD) == 0,
           "sceKernelStartThread starts dormant target thread cleanly");
    s_cpu->r[4] = target_uid;
    expect(sr_syscall(s_cpu, NID_SCE_KERNEL_START_THREAD) == 0x800201a4u,
           "sceKernelStartThread second start is rejected with NOT_DORMANT");
}

static void test_exit_delete_lifecycle_and_join_result(void) {
    reset_fixture();
    sr_hle_init();

    TCB *worker = fixture_thread(0x190u, TH_READY, 40);
    TCB *joiner = fixture_thread(0x191u, TH_WAIT_OBJ, 41);
    joiner->wait_obj = worker->uid;
    joiner->join_target = worker->uid;
    joiner->join_waiting = 1;
    s_exit_argument = 0x66;
    s_exit_nid = NID_SCE_KERNEL_EXIT_DELETE_THREAD;
    run_worker(worker);
    s_exit_nid = NID_SCE_KERNEL_EXIT_THREAD;

    expect(worker->deleted, "ExitDeleteThread removes the current thread object");
    expect(sched_thread_exit_status(worker->uid) == 0x80020198u,
           "ExitDeleteThread hides the UID from later status queries");
    expect(joiner->state == TH_READY && joiner->join_result_valid &&
           joiner->join_result == 0x66u,
           "ExitDeleteThread preserves the exit result for an existing joiner");
    expect(sched_start_thread(worker->uid, 0, 0) == 0x80020198u &&
           sched_thread_wakeup(worker->uid) == 0x80020198u,
           "ExitDeleteThread rejects later start and wakeup operations");
    if (worker->coro) {
        sr_coro_destroy(worker->coro);
        worker->coro = NULL;
    }
}

#define NID_SCE_KERNEL_WAIT_THREAD_END 0x278c0df5u
#define NID_SCE_KERNEL_WAIT_THREAD_END_CB 0x840e8133u

static void test_wait_thread_end_invalid_targets(void) {
    reset_fixture();
    sr_hle_init();

    TCB *self = fixture_thread(0x130u, TH_RUNNING, 32);
    s_cur = 0;

    /* Target UID == 0: ILLEGAL_THID */
    s_cpu->r[4] = 0u;
    s_cpu->r[5] = 0u;
    uint32_t ret = sr_syscall(s_cpu, NID_SCE_KERNEL_WAIT_THREAD_END);
    expect(ret == 0x80020197u, "WaitThreadEnd(0) returns SCE_KERNEL_ERROR_ILLEGAL_THID");

    /* Target UID == self: ILLEGAL_THID */
    s_cpu->r[4] = self->uid;
    ret = sr_syscall(s_cpu, NID_SCE_KERNEL_WAIT_THREAD_END);
    expect(ret == 0x80020197u, "WaitThreadEnd(self) returns SCE_KERNEL_ERROR_ILLEGAL_THID");

    /* Target UID unknown: UNKNOWN_THID */
    s_cpu->r[4] = 0x9999u;
    ret = sr_syscall(s_cpu, NID_SCE_KERNEL_WAIT_THREAD_END);
    expect(ret == 0x80020198u, "WaitThreadEnd(unknown) returns SCE_KERNEL_ERROR_UNKNOWN_THID");
}

static void test_wait_thread_end_already_ended(void) {
    reset_fixture();
    sr_hle_init();

    TCB *target = fixture_thread(0x140u, TH_DORMANT, 40);
    target->started = 1;
    target->exit_status = 0x42;

    TCB *waiter = fixture_thread(0x141u, TH_RUNNING, 32);
    s_cur = (int)(waiter - s_tcb);

    /* Wait for already-ended target through production sr_syscall */
    s_cpu->r[4] = target->uid;
    s_cpu->r[5] = 0u; /* timeout NULL */
    uint32_t ret = sr_syscall(s_cpu, NID_SCE_KERNEL_WAIT_THREAD_END);
    expect(ret == 0x42u, "WaitThreadEnd on dormant started target returns its exit status (0x42)");
}

static int s_joiner_woken;
static uint32_t s_joiner_ret;

static void joiner_coro_body(void *arg) {
    TCB *target = (TCB *)arg;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = target->uid;
    cpu.r[5] = 0u;
    s_joiner_ret = sr_syscall(&cpu, NID_SCE_KERNEL_WAIT_THREAD_END);
    s_joiner_woken = 1;
    selftest_park_on_scheduler();
}

static void test_wait_thread_end_blocking_and_resume(void) {
    reset_fixture();
    sr_hle_init();

    TCB *worker = fixture_thread(0x160u, TH_READY, 40);
    TCB *joiner = fixture_thread(0x161u, TH_RUNNING, 32);

    s_cur = (int)(joiner - s_tcb);
    joiner->started = 1;
    s_joiner_woken = 0;
    s_joiner_ret = 0xFFFFFFFFu;

    /* Create coroutine for joiner and enter WaitThreadEnd via sr_syscall */
    joiner->coro = sr_coro_create(joiner_coro_body, worker, (size_t)4 << 20);
    expect(joiner->coro != NULL, "joiner coroutine created");
    if (joiner->coro) sr_coro_switch(joiner->coro);

    /* Verify joiner is now blocked waiting for worker */
    expect(joiner->state == TH_WAIT_OBJ, "WaitThreadEnd via sr_syscall placed joiner into TH_WAIT_OBJ");
    expect(joiner->wait_obj == worker->uid, "WaitThreadEnd recorded target UID as wait object");
    expect(s_joiner_woken == 0, "joiner has not resumed yet while worker is running");

    /* Now run worker to execute ExitThread */
    s_exit_argument = 0x55;
    run_worker(worker);

    /* Switch to ready joiner coroutine to complete its resumed wait syscall */
    if (joiner->state == TH_READY && joiner->coro) {
        s_cur = (int)(joiner - s_tcb);
        sr_coro_switch(joiner->coro);
        s_cur = -1;
    }

    /* Verify ExitThread woke joiner and joiner resumed to complete sr_syscall */
    expect(joiner->state == TH_READY || joiner->state == TH_RUNNING, "ExitThread woke joiner thread");
    expect(s_joiner_woken == 1, "joiner resumed execution after target ExitThread");
    expect(s_joiner_ret == 0x55u, "WaitThreadEnd returned target exit status after blocking resume");

    if (joiner->coro) {
        sr_coro_destroy(joiner->coro);
        joiner->coro = NULL;
    }
    if (worker->coro) {
        sr_coro_destroy(worker->coro);
        worker->coro = NULL;
    }
}

static void joiner_cb_coro_body(void *arg) {
    TCB *target = (TCB *)arg;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = target->uid;
    cpu.r[5] = 0u;
    s_joiner_ret = sr_syscall(&cpu, NID_SCE_KERNEL_WAIT_THREAD_END_CB);
    s_joiner_woken = 1;
    selftest_park_on_scheduler();
}

static void test_wait_thread_end_cb_execution(void) {
    reset_fixture();
    sr_hle_init();

    TCB *worker = fixture_thread(0x170u, TH_READY, 40);
    TCB *joiner = fixture_thread(0x171u, TH_RUNNING, 32);

    s_cur = (int)(joiner - s_tcb);
    joiner->started = 1;
    s_joiner_woken = 0;
    s_joiner_ret = 0xFFFFFFFFu;

    /* Enter WaitThreadEndCB via production sr_syscall */
    joiner->coro = sr_coro_create(joiner_cb_coro_body, worker, (size_t)4 << 20);
    expect(joiner->coro != NULL, "joiner CB coroutine created");
    if (joiner->coro) sr_coro_switch(joiner->coro);

    /* Verify joiner is in TH_WAIT_OBJ with callback wait flag set while blocked */
    expect(joiner->state == TH_WAIT_OBJ, "WaitThreadEndCB placed joiner into TH_WAIT_OBJ");
    expect(joiner->wait_obj == worker->uid, "WaitThreadEndCB recorded target UID as wait object");
    expect(joiner->is_cb_wait == 1, "WaitThreadEndCB set is_cb_wait flag while blocked");
    expect(sr_last_nid == NID_SCE_KERNEL_WAIT_THREAD_END_CB, "production dispatcher executed WaitThreadEndCB NID");

    /* Run worker to ExitThread and wake joiner */
    s_exit_argument = 0x77;
    run_worker(worker);

    /* Switch to ready joiner coroutine to complete its resumed CB wait syscall */
    if (joiner->state == TH_READY && joiner->coro) {
        s_cur = (int)(joiner - s_tcb);
        sr_coro_switch(joiner->coro);
        s_cur = -1;
    }

    expect(s_joiner_woken == 1, "WaitThreadEndCB resumed joiner after target exit");
    expect(s_joiner_ret == 0x77u, "WaitThreadEndCB returned target exit status");

    if (joiner->coro) {
        sr_coro_destroy(joiner->coro);
        joiner->coro = NULL;
    }
    if (worker->coro) {
        sr_coro_destroy(worker->coro);
        worker->coro = NULL;
    }
}

/* The display handler is exercised through the production NID registry just
 * like the generated import stub.  This is intentionally small: it protects
 * the PSP current/pending latch split and the scalar/span validation at the
 * HLE edge; host presentation timing is a separate layer. */
#define NID_SCE_DISPLAY_SET_FRAMEBUF 0x289d82feu
#define NID_SCE_DISPLAY_GET_FRAMEBUF 0xeeda2e54u
#define NID_SCE_DMAC_MEMCPY 0x617f3fe6u
#define NID_SCE_DMAC_TRY_MEMCPY 0xd97f94d8u
#define NID_SCE_KERNEL_MEMSET 0xa089eca4u
#define NID_SCE_KERNEL_MEMCPY 0x1839852au
/* PSP DMAC error classes, measured on hardware (see test_dmac_semantics). */
#define SCE_DMAC_ILLEGAL_ADDR 0x80000103u
#define SCE_DMAC_ILLEGAL_SIZE 0x80000104u

static uint32_t display_set(uint32_t addr, int32_t stride, int32_t fmt, uint32_t sync) {
    s_cpu->r[4] = addr;
    s_cpu->r[5] = (uint32_t)stride;
    s_cpu->r[6] = (uint32_t)fmt;
    s_cpu->r[7] = sync;
    return sr_syscall(s_cpu, NID_SCE_DISPLAY_SET_FRAMEBUF);
}

static uint32_t display_get(uint32_t addr_out, uint32_t stride_out,
                            uint32_t fmt_out, uint32_t latched) {
    s_cpu->r[4] = addr_out;
    s_cpu->r[5] = stride_out;
    s_cpu->r[6] = fmt_out;
    s_cpu->r[7] = latched;
    return sr_syscall(s_cpu, NID_SCE_DISPLAY_GET_FRAMEBUF);
}

static uint32_t bulk_call(uint32_t nid, uint32_t dst, uint32_t src_or_value, uint32_t size) {
    s_cpu->r[4] = dst;
    s_cpu->r[5] = src_or_value;
    s_cpu->r[6] = size;
    return sr_syscall(s_cpu, nid);
}

/* ---------------------------------------------------------------------------
 * Generic PSP input contract: sceCtrlReadBufferPositive.
 *
 * PRODUCTION_DISPATCH.  0x1f803938 is registered by
 * hle_register_wait_conformance_handlers(), the single helper that BOTH
 * sr_hle_init() branches call, so this enters the same h_CtrlReadBuffer the game
 * build uses.  The first assertion checks registry membership rather than
 * assuming it: an unregistered NID reaches the unimplemented path in
 * sr_syscall(), which is exactly how a test passes for the wrong reason.
 *
 * PSP contract inputs are CORROBORATIVE_ONLY -- prior art read as source, not a
 * local hardware measurement:
 *
 *  - PPSSPP Core/HLE/sceCtrl.cpp, __CtrlReadBuffer(): a request for more than
 *    NUM_CTRL_BUFFERS (64) buffers returns SCE_KERNEL_ERROR_INVALID_SIZE rather
 *    than being clamped, and the returned value is the number of buffers
 *    actually written -- __CtrlReadSingleBuffer() contributes 0 for a pointer it
 *    cannot write, so a call that stores nothing reports 0.  With nBufs == 0 the
 *    available count is min(fresh, 0) == 0, so it writes nothing and returns 0.
 *  - pspautotests tests/ctrl/ctrl.expected records that the blocking read really
 *    does wait and that the peek form does not.  Only the blocking form is
 *    registered by this runtime, so that is background, not an assertion here.
 *
 * The whole-span requirement does not depend on the PSP contract at all.  The
 * scalar accessors reject an out-of-range store one word at a time, so with no
 * preflight this handler can write PART of the requested history, or -- once
 * buf + i*16 wraps uint32_t -- write into an unrelated but in-range guest
 * address, and still report the full count back to the guest.  That is the
 * defect class #86 closed for sceAudio, reached here through a different API.
 *
 * Deliberately NOT asserted, and left visible rather than quietly changed: when
 * no fresh sample is available this runtime still returns one stale sample
 * ("always give at least the latest") where PPSSPP returns 0.  That sits on the
 * per-frame hot path of every run rather than on an error path, so it is a
 * separate behavioural question needing its own evidence.
 * --------------------------------------------------------------------------- */
#define NID_SCE_CTRL_READ_BUFFER_POSITIVE 0x1f803938u
#define SCE_CTRL_ERROR_INVALID_SIZE 0x80000104u
#define CTRL_SAMPLE_BYTES 16u
#define CTRL_BTN_START 0x0008u

/* Guest scratch well inside RAM, and a base whose 16-byte first sample fits the
 * arena exactly so that a second sample must cross the end. */
#define CTRL_OK_BUF   0x08a00000u
#define CTRL_TAIL_BUF 0x0bfffff0u
/* phys(0xfffffff0) is past the arena, so sample 0 is dropped -- but every later
 * sample address wraps uint32_t down into guest 0x00000000.., which IS in range. */
#define CTRL_WRAP_BUF 0xfffffff0u
#define CTRL_WRAP_LANDING 0x00000000u
#define CTRL_WRAP_LANDING_BYTES 0x400u

void sr_route_reset(void);   /* also declared beside the issue #64 route tests */

static uint32_t ctrl_dispatch(CpuState *cpu, uint32_t buf, uint32_t nbufs) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->r[4] = buf;
    cpu->r[5] = nbufs;
    return sr_syscall(cpu, NID_SCE_CTRL_READ_BUFFER_POSITIVE);
}

/* Deliver n whole vblanks through the production path: sr_vblank_tick() is what
 * latches a controller sample, so input history here is produced the same way a
 * run produces it. */
static void ctrl_tick(unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        s_vtime_us += 16683u;
        sr_display_advance_vcount(1u);
        sr_vblank_tick();
    }
}

static void ctrl_fill_guest(uint32_t addr, uint32_t bytes, uint32_t word) {
    for (uint32_t off = 0; off < bytes; off += 4u) MEM_W32(addr + off, word);
}

static int ctrl_guest_all(uint32_t addr, uint32_t bytes, uint32_t word) {
    for (uint32_t off = 0; off < bytes; off += 4u)
        if (MEM_R32(addr + off) != word) return 0;
    return 1;
}

/* Drop whatever history is pending so each case starts from a known ring. */
static void ctrl_drain(CpuState *cpu) {
    (void)ctrl_dispatch(cpu, CTRL_OK_BUF, 64u);
    ctrl_fill_guest(CTRL_OK_BUF, 64u * CTRL_SAMPLE_BYTES, 0u);
}

static void ctrl_env(const char *noinput, const char *pad,
                     const char *period, const char *width) {
    char buf[64];
    snprintf(buf, sizeof buf, "SR_NOINPUT=%s", noinput);  _putenv(buf);
    snprintf(buf, sizeof buf, "SR_PAD=%s", pad);          _putenv(buf);
    snprintf(buf, sizeof buf, "SR_PADPERIOD=%s", period); _putenv(buf);
    snprintf(buf, sizeof buf, "SR_PADWIDTH=%s", width);   _putenv(buf);
    _putenv("SR_PADSTART=");
    _putenv("SR_PADSCRIPT=");
    _putenv("SR_INLOG=");
}

static void test_ctrl_read_buffer_contract(void) {
    CpuState cpu;

    reset_fixture();
    sr_hle_init();
    sr_route_reset();   /* no route program: the env pulse below is the only input source */

    expect(sr_hle_test_is_registered(NID_SCE_CTRL_READ_BUFFER_POSITIVE),
           "sceCtrlReadBufferPositive is a registered NID in this build");

    /* --- no input -------------------------------------------------------- */
    ctrl_env("1", "", "", "");
    ctrl_drain(&cpu);
    ctrl_tick(8u);
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 8u) == 8u,
           "a drained ring plus eight vblanks yields eight fresh samples");
    {
        int any_button = 0;
        uint32_t prev_ts = 0;
        int monotonic = 1;
        for (unsigned i = 0; i < 8u; i++) {
            uint32_t e = CTRL_OK_BUF + i * CTRL_SAMPLE_BYTES;
            if (MEM_R32(e + 4u) != 0u) any_button = 1;
            if (i && MEM_R32(e) <= prev_ts) monotonic = 0;
            prev_ts = MEM_R32(e);
        }
        expect(!any_button, "SR_NOINPUT reports a neutral pad through production dispatch");
        expect(monotonic, "each delivered sample carries a strictly newer timestamp");
        expect(MEM_R8(CTRL_OK_BUF + 8u) == 128u && MEM_R8(CTRL_OK_BUF + 9u) == 128u,
               "a neutral pad reports both analog axes centred");
    }

    /* --- held button ----------------------------------------------------- */
    ctrl_env("", "0008", "1", "1");
    ctrl_drain(&cpu);
    ctrl_tick(8u);
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 8u) == 8u, "held-button read returns eight samples");
    {
        unsigned set = 0;
        for (unsigned i = 0; i < 8u; i++)
            if (MEM_R32(CTRL_OK_BUF + i * CTRL_SAMPLE_BYTES + 4u) & CTRL_BTN_START) set++;
        expect(set == 8u, "a continuously held button is set in every delivered sample");
    }

    /* --- press transition ------------------------------------------------ *
     * Two frames pressed, two released, so eight consecutive vblanks carry
     * exactly four of each and at least one rising edge whatever the starting
     * phase.  A flat "current state" fill cannot produce that, which is the
     * distinction this case exists to make. */
    ctrl_env("", "0008", "4", "2");
    ctrl_drain(&cpu);
    ctrl_tick(8u);
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 8u) == 8u, "press-transition read returns eight samples");
    {
        unsigned set = 0, rising = 0;
        int prev = -1;
        for (unsigned i = 0; i < 8u; i++) {
            int cur = (MEM_R32(CTRL_OK_BUF + i * CTRL_SAMPLE_BYTES + 4u) & CTRL_BTN_START) ? 1 : 0;
            if (cur) set++;
            if (prev == 0 && cur == 1) rising++;
            prev = cur;
        }
        expect(set == 4u, "a two-on/two-off pulse delivers exactly four pressed samples in eight");
        expect(rising >= 1u, "delivered history contains a real press edge, not a flat state fill");
    }

    /* --- oversized request ------------------------------------------------ */
    ctrl_env("1", "", "", "");
    ctrl_drain(&cpu);
    ctrl_tick(63u);
    ctrl_fill_guest(CTRL_OK_BUF, 64u * CTRL_SAMPLE_BYTES, 0xa5a5a5a5u);
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 65u) == SCE_CTRL_ERROR_INVALID_SIZE,
           "a request for more than the 64-entry ring is rejected, not clamped");
    expect(ctrl_guest_all(CTRL_OK_BUF, 64u * CTRL_SAMPLE_BYTES, 0xa5a5a5a5u),
           "a rejected oversized request writes no guest byte");
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 63u) == 63u,
           "a rejected oversized request leaves the sample ring unconsumed");

    /* --- zero-count request ----------------------------------------------- */
    ctrl_drain(&cpu);
    ctrl_tick(4u);
    ctrl_fill_guest(CTRL_OK_BUF, 4u * CTRL_SAMPLE_BYTES, 0xa5a5a5a5u);
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 0u) == 0u,
           "a zero-buffer request reports zero buffers written");
    expect(ctrl_guest_all(CTRL_OK_BUF, 4u * CTRL_SAMPLE_BYTES, 0xa5a5a5a5u),
           "a zero-buffer request writes no guest byte");

    /* --- span crossing the end of the arena -------------------------------- *
     * The base is writable and its first sample fits exactly; the second cannot.
     * Without a whole-span preflight the handler writes sample 0, silently drops
     * sample 1, and still reports two. */
    ctrl_drain(&cpu);
    ctrl_tick(4u);
    ctrl_fill_guest(CTRL_TAIL_BUF, CTRL_SAMPLE_BYTES, 0xa5a5a5a5u);
    expect(ctrl_dispatch(&cpu, CTRL_TAIL_BUF, 2u) == 0u,
           "a span crossing the end of the guest arena reports nothing written");
    expect(ctrl_guest_all(CTRL_TAIL_BUF, CTRL_SAMPLE_BYTES, 0xa5a5a5a5u),
           "a rejected tail span leaves even its writable leading sample untouched");

    /* --- span whose element addresses wrap uint32_t ------------------------ */
    ctrl_drain(&cpu);
    ctrl_tick(63u);
    ctrl_fill_guest(CTRL_WRAP_LANDING, CTRL_WRAP_LANDING_BYTES, 0x5a5a5a5au);
    expect(ctrl_dispatch(&cpu, CTRL_WRAP_BUF, 64u) == 0u,
           "a base past the arena reports nothing written even when later elements wrap into it");
    expect(ctrl_guest_all(CTRL_WRAP_LANDING, CTRL_WRAP_LANDING_BYTES, 0x5a5a5a5au),
           "wrapped sample addresses do not scribble over unrelated guest memory");
    ctrl_fill_guest(CTRL_WRAP_LANDING, CTRL_WRAP_LANDING_BYTES, 0u);

    ctrl_env("1", "", "", "");
    ctrl_drain(&cpu);
}

static void test_ctrl_sample_timestamp_microsecond_contract(void) {
    CpuState cpu;

    reset_fixture();
    sr_hle_init();
    sr_route_reset();
    ctrl_env("1", "", "", "");

    /* Vector A: SAMPLE OWNERSHIP
     * SceCtrlData.TimeStamp must contain the low 32 bits of the guest microsecond
     * clock at sample creation/latch time. With virtual time at 1,000,000 us,
     * one sample latched and read must report timestamp 1,000,000. */
    ctrl_drain(&cpu);
    s_vtime_us = 1000000u;
    sr_ctrl_sample();
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 1u) == 1u,
           "Vector A: one sample produced yields one sample read");
    expect(MEM_R32(CTRL_OK_BUF) == 1000000u,
           "Vector A: sample timestamp matches guest microsecond clock (1,000,000 us)");

    /* Vector B: EXACT DELTA
     * Two consecutive samples produced at 1,000,000 us and 1,016,683 us (~1 frame
     * interval) must report timestamps reflecting the exact 16,683 us delta. */
    ctrl_drain(&cpu);
    s_vtime_us = 1000000u;
    sr_ctrl_sample();
    s_vtime_us = 1016683u;
    sr_ctrl_sample();
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 2u) == 2u,
           "Vector B: two produced samples yield two samples read");
    {
        uint32_t ts0 = MEM_R32(CTRL_OK_BUF);
        uint32_t ts1 = MEM_R32(CTRL_OK_BUF + CTRL_SAMPLE_BYTES);
        expect(ts0 == 1000000u, "Vector B: first sample timestamp is 1,000,000 us");
        expect(ts1 == 1016683u, "Vector B: second sample timestamp is 1,016,683 us");
        expect(ts1 - ts0 == 16683u, "Vector B: delta between samples is exactly 16,683 us");
    }

    /* Vector C: NOT VCOUNT-DERIVED
     * With VCOUNT held unchanged (no display advance), advance virtual time by
     * 33,366 us (two frames) and produce a second sample. Timestamp must advance
     * by 33,366 us even though VCOUNT did not move. */
    ctrl_drain(&cpu);
    s_vtime_us = 2000000u;
    sr_ctrl_sample();
    s_vtime_us = 2033366u;
    sr_ctrl_sample();
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 2u) == 2u,
           "Vector C: two produced samples with held VCOUNT yield two samples read");
    {
        uint32_t ts0 = MEM_R32(CTRL_OK_BUF);
        uint32_t ts1 = MEM_R32(CTRL_OK_BUF + CTRL_SAMPLE_BYTES);
        expect(ts0 == 2000000u, "Vector C: first sample timestamp is 2,000,000 us");
        expect(ts1 == 2033366u, "Vector C: second sample timestamp is 2,033,366 us");
        expect(ts1 - ts0 == 33366u, "Vector C: timestamp advances with virtual time when VCOUNT is frozen");
    }

    /* Vector D: SAMPLE-TIME, NOT READ-TIME
     * Produce a sample at T1 = 3,000,000 us. Advance virtual time to T2 = 3,500,000 us
     * WITHOUT producing a new sample. Read the stored sample. Its timestamp must
     * remain T1 (3,000,000 us), proving timestamp belongs to sample latch time,
     * not dispatch/read time. */
    ctrl_drain(&cpu);
    s_vtime_us = 3000000u;
    sr_ctrl_sample();
    s_vtime_us = 3500000u;
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 1u) == 1u,
           "Vector D: reading stored sample after time advancement yields one sample");
    expect(MEM_R32(CTRL_OK_BUF) == 3000000u,
           "Vector D: timestamp reflects latch time (3,000,000 us), not read time (3,500,000 us)");

    /* Vector E: LOW-32 WRAP
     * Virtual time wraps 32 bits from 0xFFFFFFF0 us (4,294,967,280 us) to
     * 0x10000000E us (4,294,967,310 us). The stored timestamps must be
     * 0xFFFFFFF0 and 0x0000000E, yielding an unsigned 32-bit delta of 30. */
    ctrl_drain(&cpu);
    s_vtime_us = 0x00000000FFFFFFF0ULL;
    sr_ctrl_sample();
    s_vtime_us = 0x000000010000000EULL;
    sr_ctrl_sample();
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 2u) == 2u,
           "Vector E: two samples across 32-bit wrap yield two samples read");
    {
        uint32_t ts0 = MEM_R32(CTRL_OK_BUF);
        uint32_t ts1 = MEM_R32(CTRL_OK_BUF + CTRL_SAMPLE_BYTES);
        expect(ts0 == 0xFFFFFFF0u, "Vector E: pre-wrap timestamp is 0xFFFFFFF0");
        expect(ts1 == 0x0000000Eu, "Vector E: post-wrap timestamp is 0x0000000E");
        expect(ts1 - ts0 == 30u, "Vector E: unsigned 32-bit delta across wrap is 30 us");
    }

    /* Vector F: ZERO COUNT
     * Produce a sample at T = 4,000,000 us. Advance time to 4,500,000 us.
     * Issue a zero-count read (nbufs = 0). The zero-count read must return 0,
     * write no bytes, and must NOT create or restamp any sample.
     * Advance time to 5,000,000 us and read the sample: its timestamp must
     * still be 4,000,000 us. */
    ctrl_drain(&cpu);
    s_vtime_us = 4000000u;
    sr_ctrl_sample();
    s_vtime_us = 4500000u;
    ctrl_fill_guest(CTRL_OK_BUF, CTRL_SAMPLE_BYTES, 0xa5a5a5a5u);
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 0u) == 0u,
           "Vector F: zero-count read reports 0");
    expect(ctrl_guest_all(CTRL_OK_BUF, CTRL_SAMPLE_BYTES, 0xa5a5a5a5u),
           "Vector F: zero-count read writes no guest bytes");
    s_vtime_us = 5000000u;
    expect(ctrl_dispatch(&cpu, CTRL_OK_BUF, 1u) == 1u,
           "Vector F: subsequent read yields one stored sample");
    expect(MEM_R32(CTRL_OK_BUF) == 4000000u,
           "Vector F: stored sample timestamp remains 4,000,000 us after zero-count read");

    ctrl_drain(&cpu);
}

/* ---------------------------------------------------------------------------
 * Nested guest-call (callback) ABI specimen.
 *
 * WHAT THIS IS.  Every nested guest call this runtime makes goes through the
 * same marshalling policy: ge_call_guest() and ge_call_guest_rv() in hle.c, and
 * call_guest3() in mpeg.c.  Before this regression existed nothing recorded what
 * that policy actually preserves, so any argument about the MPEG callback stack
 * was an argument about unmeasured behaviour.  These cases enter the production
 * marshalling through sr_hle_test_call_guest() -- a call-through to
 * ge_call_guest_rv(), not a copy of it -- and pin every observable.
 *
 * WHAT IT IS NOT.  This is HOST_TESTED: it states what Nakagawa does today.  It
 * is NOT a PSP contract and must never be read as one.  Public PSP ABI/source
 * material is consistent with callbacks using an ordinary guest calling-thread
 * stack under the normal MIPS o32 ABI (where the callee preserves $s0-$s7, $sp
 * and $gp and may freely clobber the argument, result and temporary registers),
 * but the exact PSP callback-stack contract relevant to this runtime is
 * unmeasured here and remains CORROBORATIVE_ONLY / NOT_ESTABLISHED.
 * This runtime instead zeroes the whole CpuState, hands the callee one fixed
 * scratch stack, and restores the caller's entire state afterwards.  Those are
 * different models.  Which one the PSP requires is NOT ESTABLISHED and cannot be
 * settled from source; it needs a hardware probe.  Until then this regression's
 * job is to make the current model impossible to change by accident.
 *
 * The guest bodies below are synthetic and source-owned.  Only the body is
 * synthetic: the state marshalling under test is production code.
 * --------------------------------------------------------------------------- */
extern uint32_t sr_hle_test_call_guest(CpuState *s, uint32_t fn,
                                       uint32_t a0, uint32_t a1, uint32_t a2);
extern uint32_t sr_hle_test_call_guest_stack(void);

#define CBABI_ENTRY        0x0800ab00u  /* records its incoming state, then mutates everything */
#define CBABI_NESTED_ENTRY 0x0800ab40u  /* records, makes one nested call, records again */
#define CBABI_STORE_ENTRY  0x0800ab80u  /* writes one word below $sp and returns */
#define CBABI_NEG_ENTRY    0x0800abc0u  /* returns a value with the sign bit set */
#define CBABI_RETURN_VALUE  0xfeedbac1u
#define CBABI_NESTED_RETURN 0x0000002au
#define CBABI_STORE_RETURN  0x00000007u
#define CBABI_NEG_RETURN    0xffffffffu
/* A word the outer body parks below its own stack pointer, standing in for any
 * guest local a translated function spills to the guest stack. */
#define CBABI_LOCAL_OFFSET 16u
#define CBABI_OUTER_LOCAL  0x0a7e5710u
#define CBABI_INNER_LOCAL  0xdeadbeefu

static CpuState s_cbabi_seen[4];     /* incoming state, per invocation */
static unsigned s_cbabi_calls;
static int s_cbabi_depth;
static int s_cbabi_max_depth;
static uint32_t s_cbabi_outer_sp;
static uint32_t s_cbabi_inner_sp;
static uint32_t s_cbabi_outer_local_after_nested;

/* Mutate every architectural class a real guest body could touch, so anything
 * the caller-side restore misses shows up as a difference after the call. */
static void cbabi_scribble(CpuState *cpu, uint32_t tag) {
    for (int i = 1; i < 32; i++) cpu->r[i] = tag + (uint32_t)i;
    cpu->hi = tag ^ 0x11111111u;
    cpu->lo = tag ^ 0x22222222u;
    for (int i = 0; i < 32; i++) cpu->fi[i] = tag + 0x100u + (uint32_t)i;
    for (int i = 0; i < 128; i++) cpu->vi[i] = tag + 0x200u + (uint32_t)i;
    for (int i = 0; i < 16; i++) cpu->vfpuCtrl[i] = tag + 0x300u + (uint32_t)i;
    cpu->fcr31 = tag ^ 0x33333333u;
    cpu->fpcond = tag & 1u;
    cpu->status = tag ^ 0x44444444u;
}

/* Called from the selftest's dispatch() for the synthetic guest entries above.
 * Returns non-zero when it owned the target. */
static int cbabi_dispatch(CpuState *cpu, uint32_t target) {
    if (target != CBABI_ENTRY && target != CBABI_NESTED_ENTRY &&
        target != CBABI_STORE_ENTRY && target != CBABI_NEG_ENTRY)
        return 0;

    if (s_cbabi_calls < sizeof(s_cbabi_seen) / sizeof(s_cbabi_seen[0]))
        memcpy(&s_cbabi_seen[s_cbabi_calls], cpu, sizeof(CpuState));
    s_cbabi_calls++;
    if (++s_cbabi_depth > s_cbabi_max_depth) s_cbabi_max_depth = s_cbabi_depth;

    if (target == CBABI_STORE_ENTRY) {
        s_cbabi_inner_sp = cpu->r[29];
        /* Exactly what a translated body does with a guest local: store below $sp. */
        MEM_W32(cpu->r[29] - CBABI_LOCAL_OFFSET, CBABI_INNER_LOCAL);
        cbabi_scribble(cpu, 0x77000000u);
        cpu->r[2] = CBABI_STORE_RETURN;
    } else if (target == CBABI_NEG_ENTRY) {
        cpu->r[2] = CBABI_NEG_RETURN;
    } else if (target == CBABI_NESTED_ENTRY) {
        s_cbabi_outer_sp = cpu->r[29];
        MEM_W32(cpu->r[29] - CBABI_LOCAL_OFFSET, CBABI_OUTER_LOCAL);
        /* Re-enter the production marshalling from inside a guest body: the
         * nested-callback shape, driven through the same entry point. */
        uint32_t inner = sr_hle_test_call_guest(cpu, CBABI_STORE_ENTRY, 1u, 2u, 3u);
        s_cbabi_outer_local_after_nested = MEM_R32(cpu->r[29] - CBABI_LOCAL_OFFSET);
        cpu->r[2] = inner == CBABI_STORE_RETURN ? CBABI_NESTED_RETURN : 0xbadbad00u;
    } else {
        cbabi_scribble(cpu, 0x55000000u);
        cpu->r[2] = CBABI_RETURN_VALUE;
    }

    s_cbabi_depth--;
    return 1;
}

static void cbabi_reset(void) {
    memset(s_cbabi_seen, 0, sizeof(s_cbabi_seen));
    s_cbabi_calls = 0;
    s_cbabi_depth = 0;
    s_cbabi_max_depth = 0;
    s_cbabi_outer_sp = 0;
    s_cbabi_inner_sp = 0;
    s_cbabi_outer_local_after_nested = 0;
}

/* Every GPR except the ones the marshalling deliberately populates: the three
 * argument registers, $gp and $sp.  Those four are asserted individually. */
static int cbabi_other_gprs_zero(const CpuState *seen) {
    for (int i = 0; i < 32; i++) {
        if (i == 4 || i == 5 || i == 6 || i == 28 || i == 29) continue;
        if (seen->r[i] != 0u) return 0;
    }
    return 1;
}

static void test_nested_guest_call_abi(void) {
    CpuState caller, before;
    const uint32_t scratch = sr_hle_test_call_guest_stack();
    const uint32_t local_addr = scratch - CBABI_LOCAL_OFFSET;

    reset_fixture();
    sr_hle_init();
    cbabi_reset();

    /* A caller state with every class set to something distinctive, so "restored"
     * is a real claim rather than "was zero and stayed zero". */
    memset(&caller, 0, sizeof(caller));
    cbabi_scribble(&caller, 0x33000000u);
    caller.r[0] = 0u;                     /* $zero is architecturally fixed */
    caller.r[28] = 0x08800000u;           /* $gp */
    caller.r[29] = 0x09c00000u;           /* the caller's own stack, distinct from the scratch one */
    caller.r[31] = 0x08123456u;           /* $ra */
    caller.pc = 0x08001000u;
    memcpy(&before, &caller, sizeof(caller));

    uint32_t rv = sr_hle_test_call_guest(&caller, CBABI_ENTRY,
                                         0xa0a0a0a0u, 0xb1b1b1b1u, 0xc2c2c2c2u);

    expect(s_cbabi_calls == 1u, "the synthetic guest body was entered exactly once");
    expect(rv == CBABI_RETURN_VALUE, "the nested call returns the callee's $v0 to its HLE caller");

    /* ---- what the callee is handed ---------------------------------------- */
    {
        const CpuState *seen = &s_cbabi_seen[0];
        expect(seen->r[4] == 0xa0a0a0a0u && seen->r[5] == 0xb1b1b1b1u &&
                   seen->r[6] == 0xc2c2c2c2u,
               "the three call arguments arrive in $a0/$a1/$a2");
        expect(seen->r[28] == before.r[28], "$gp is inherited from the calling state");
        expect(seen->r[29] == scratch,
               "the callee runs on the fixed scratch stack, not the caller's $sp");
        expect(seen->r[29] != before.r[29],
               "the scratch stack is genuinely a different stack from the caller's");
        expect(seen->r[31] == 0u, "$ra is zero: the callee has no guest return address to jump to");
        expect(seen->pc == CBABI_ENTRY, "$pc names the guest entry being dispatched");
        expect(cbabi_other_gprs_zero(seen),
               "every GPR the call does not populate is zeroed: no caller state leaks in");
        expect(seen->hi == 0u && seen->lo == 0u, "HI/LO are zeroed for the callee");
        expect(seen->fcr31 == 0u && seen->fpcond == 0u && seen->status == 0u,
               "FPU control, FP condition and COP0 status are zeroed for the callee");
        {
            int fpu_clear = 1, vfpu_clear = 1;
            for (int i = 0; i < 32; i++) if (seen->fi[i] != 0u) fpu_clear = 0;
            for (int i = 0; i < 128; i++) if (seen->vi[i] != 0u) vfpu_clear = 0;
            expect(fpu_clear, "all 32 FPU registers are zeroed for the callee");
            expect(vfpu_clear, "all 128 VFPU registers are zeroed for the callee");
        }
        expect(seen->vfpuCtrl[0] == 0xe4u && seen->vfpuCtrl[1] == 0xe4u,
               "the two VFPU prefix control words are seeded to the identity value 0xe4");
        {
            int rest_clear = 1;
            for (int i = 2; i < 16; i++) if (seen->vfpuCtrl[i] != 0u) rest_clear = 0;
            expect(rest_clear, "the remaining VFPU control words are zeroed for the callee");
        }
    }

    /* ---- what the caller gets back ---------------------------------------- *
     * The callee scribbled over every architectural class before returning, so a
     * byte-identical CpuState here is a real restoration claim.  Note what that
     * implies, and what is easy to get wrong: $v0 is restored too, so a callee's
     * result reaches the HLE caller ONLY through the C return value, never
     * through the guest register file. */
    expect(memcmp(&caller, &before, sizeof(CpuState)) == 0,
           "the entire caller CpuState is restored byte-for-byte across the call");
    expect(caller.r[2] == before.r[2],
           "$v0 is restored as well: the callee's result is not left in the caller's registers");

    /* ---- guest memory is NOT part of that restoration ---------------------- */
    cbabi_reset();
    MEM_W32(local_addr, 0u);
    (void)sr_hle_test_call_guest(&caller, CBABI_STORE_ENTRY, 0u, 0u, 0u);
    expect(MEM_R32(local_addr) == CBABI_INNER_LOCAL,
           "guest memory written by the callee persists after the call returns");

    /* ---- repeated calls do not leak state between invocations -------------- */
    cbabi_reset();
    (void)sr_hle_test_call_guest(&caller, CBABI_ENTRY, 1u, 0u, 0u);
    (void)sr_hle_test_call_guest(&caller, CBABI_ENTRY, 2u, 0u, 0u);
    expect(s_cbabi_calls == 2u, "the guest body is entered once per call");
    expect(s_cbabi_seen[1].r[4] == 2u && cbabi_other_gprs_zero(&s_cbabi_seen[1]),
           "the second invocation starts from a freshly zeroed state, not the first one's");

    /* ---- a sign-bit result survives unchanged ------------------------------ *
     * The marshalling returns uint32_t, so a guest error code must arrive with
     * its top bit intact rather than being clamped or reinterpreted. */
    cbabi_reset();
    expect(sr_hle_test_call_guest(&caller, CBABI_NEG_ENTRY, 0u, 0u, 0u) == CBABI_NEG_RETURN,
           "a callback result with the sign bit set is returned verbatim");

    /* ---- a null entry is refused without dispatching ----------------------- */
    cbabi_reset();
    expect(sr_hle_test_call_guest(&caller, 0u, 1u, 2u, 3u) == 0u,
           "a null guest entry returns zero");
    expect(s_cbabi_calls == 0u, "a null guest entry dispatches nothing");

    /* ---- nested call: the measured shared-stack hazard --------------------- *
     * This is the concrete mechanism behind the MPEG scratch-stack question.  The
     * inner call is handed the SAME fixed stack address as the outer one, so a
     * word the outer body parked below its own $sp is overwritten by the inner
     * body before the outer body resumes.  Recorded as a measurement of THIS
     * runtime, not as a claim about the PSP: whether hardware shares a stack
     * across nested guest calls is NOT ESTABLISHED, and no production behaviour
     * is changed on the strength of this test. */
    cbabi_reset();
    memcpy(&caller, &before, sizeof(caller));
    uint32_t nested_rv = sr_hle_test_call_guest(&caller, CBABI_NESTED_ENTRY, 0u, 0u, 0u);
    expect(nested_rv == CBABI_NESTED_RETURN, "a callback may itself perform a nested guest call");
    expect(s_cbabi_calls == 2u && s_cbabi_max_depth == 2,
           "the nested call really re-entered the marshalling two levels deep");
    expect(s_cbabi_outer_sp == s_cbabi_inner_sp && s_cbabi_outer_sp == scratch,
           "outer and inner nested calls are handed the identical scratch stack address");
    expect(s_cbabi_outer_local_after_nested == CBABI_INNER_LOCAL,
           "MEASURED HAZARD: the inner call overwrites the outer body's guest stack local");
    expect(s_cbabi_outer_local_after_nested != CBABI_OUTER_LOCAL,
           "MEASURED HAZARD: the outer body cannot rely on its guest stack across a nested call");
    expect(memcmp(&caller, &before, sizeof(CpuState)) == 0,
           "the outermost caller state is still restored despite the nested re-entry");
}

/* =========================================================================
 * Guest-Authored GE Sentinel (Issue generic sentinel stage)
 *
 * Full production pipeline under test:
 *   source-owned guest command list
 *   -> production HLE / GE submission (sceGeListEnQueue / sr_syscall)
 *   -> production GE command execution (h_GeListEnQueue / ge_run_list)
 *   -> actual render backend (ge.c software rasterizer)
 *   -> framebuffer result (guest VRAM at 0x04000000)
 *   -> deterministic pixel, boundary, anti-symmetry, and region hash assertions
 * ========================================================================= */

#define NID_SCE_GE_EDRAM_GET_ADDR           0xe47e40e4u
#define NID_SCE_GE_EDRAM_GET_SIZE           0x1f6752adu
#define NID_SCE_GE_LIST_ENQUEUE             0xab49e76au
#define NID_SCE_GE_LIST_SYNC                0x03444eb4u
#define NID_SCE_GE_LIST_UPDATE_STALL_ADDR   0xe0d68148u
#define NID_SCE_GE_DRAW_SYNC                0xb287bd61u
#define NID_SCE_GE_SET_CALLBACK             0xa4fc06a4u
#define NID_SCE_GE_UNSET_CALLBACK           0x05db22ceu

static uint64_t ge_sentinel_hash64_region(uint32_t fb_base, uint32_t stride,
                                          uint32_t x0, uint32_t y0,
                                          uint32_t w, uint32_t h) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (uint32_t y = y0; y < y0 + h; y++) {
        for (uint32_t x = x0; x < x0 + w; x++) {
            uint32_t addr = fb_base + (y * stride + x) * 4u;
            /* Canonical Little-Endian byte order: byte 0 (R), byte 1 (G), byte 2 (B), byte 3 (A) */
            for (uint32_t b = 0; b < 4u; b++) {
                uint8_t byte = MEM_R8(addr + b);
                hash ^= (uint64_t)byte;
                hash *= 0x100000001b3ULL;
            }
        }
    }
    return hash;
}

static void test_ge_guest_sentinel(void) {
    CpuState cpu;
    reset_fixture();
    sr_hle_init();

    /* 1. Prove all GE NIDs are explicitly registered in this build */
    expect(sr_hle_test_is_registered(NID_SCE_GE_EDRAM_GET_ADDR),
           "sceGeEdramGetAddr is registered in this build");
    expect(sr_hle_test_is_registered(NID_SCE_GE_EDRAM_GET_SIZE),
           "sceGeEdramGetSize is registered in this build");
    expect(sr_hle_test_is_registered(NID_SCE_GE_LIST_ENQUEUE),
           "sceGeListEnQueue is registered in this build");
    expect(sr_hle_test_is_registered(NID_SCE_GE_LIST_SYNC),
           "sceGeListSync is registered in this build");
    expect(sr_hle_test_is_registered(NID_SCE_GE_LIST_UPDATE_STALL_ADDR),
           "sceGeListUpdateStallAddr is registered in this build");
    expect(sr_hle_test_is_registered(NID_SCE_GE_DRAW_SYNC),
           "sceGeDrawSync is registered in this build");
    expect(sr_hle_test_is_registered(NID_SCE_GE_SET_CALLBACK),
           "sceGeSetCallback is registered in this build");
    expect(sr_hle_test_is_registered(NID_SCE_GE_UNSET_CALLBACK),
           "sceGeUnsetCallback is registered in this build");

    /* 2. eDRAM query contracts */
    memset(&cpu, 0, sizeof(cpu));
    expect(sr_syscall(&cpu, NID_SCE_GE_EDRAM_GET_ADDR) == 0x04000000u,
           "sceGeEdramGetAddr returns canonical eDRAM base 0x04000000");
    memset(&cpu, 0, sizeof(cpu));
    expect(sr_syscall(&cpu, NID_SCE_GE_EDRAM_GET_SIZE) == 0x00200000u,
           "sceGeEdramGetSize returns 2 MiB eDRAM size (0x00200000)");

    /* 3. Setup guest workload layout */
    const uint32_t fb_base = 0x04000000u;
    const uint32_t fb_stride = 512u;
    const uint32_t fb_w = 480u;
    const uint32_t fb_h = 272u;
    const uint32_t tex_base = 0x04100000u; /* in guest VRAM */
    const uint32_t dl_base = 0x08900000u;  /* in guest RAM */
    const uint32_t vtx_base = 0x08904000u; /* in guest RAM */

    /* Colors (RGBA8888 formatted as 0xAABBGGRR in LE memory) */
    const uint32_t bg_color = 0xFF281E14u; /* Dark slate blue background */
    const uint32_t r1_color = 0xFF2233EEu; /* Vibrant red */
    const uint32_t r2_color = 0xFF44DD33u; /* Lime green */
    const uint32_t r4_color = 0xFFDD8822u; /* Orange triangle */

    /* Setup 8x8 asymmetric texture at tex_base */
    for (uint32_t v = 0; v < 8u; v++) {
        for (uint32_t u = 0; u < 8u; u++) {
            uint32_t r = 30u + u * 28u;
            uint32_t g = 20u + v * 30u;
            uint32_t b = 150u + (u ^ v) * 12u;
            uint32_t color = 0xFF000000u | (b << 16) | (g << 8) | r;
            MEM_W32(tex_base + (v * 8u + u) * 4u, color);
        }
    }

    /* Setup vertices in guest memory:
     * vtx_base + 0x000: Clear sprite (0,0) to (480, 272)
     * vtx_base + 0x040: Region 1 rect (20,15) to (60,40) [40x25]
     * vtx_base + 0x080: Region 2 rect (260,140) to (380,220) [120x80]
     * vtx_base + 0x0C0: Region 3 textured rect (380,20) to (444,84) [64x64]
     * vtx_base + 0x100: Region 4 triangle (100,180)-(180,180)-(100,240)
     */
    #define W_FLOAT(addr, val) do { float _f = (val); uint32_t _u; memcpy(&_u, &_f, 4); MEM_W32((addr), _u); } while(0)

    /* Clear sprite */
    MEM_W32(vtx_base + 0x00, bg_color);
    W_FLOAT(vtx_base + 0x04, 0.0f); W_FLOAT(vtx_base + 0x08, 0.0f); W_FLOAT(vtx_base + 0x0C, 0.0f);
    MEM_W32(vtx_base + 0x10, bg_color);
    W_FLOAT(vtx_base + 0x14, 480.0f); W_FLOAT(vtx_base + 0x18, 272.0f); W_FLOAT(vtx_base + 0x1C, 0.0f);

    /* Region 1: ColorVtx */
    MEM_W32(vtx_base + 0x40, r1_color);
    W_FLOAT(vtx_base + 0x44, 20.0f); W_FLOAT(vtx_base + 0x48, 15.0f); W_FLOAT(vtx_base + 0x4C, 0.0f);
    MEM_W32(vtx_base + 0x50, r1_color);
    W_FLOAT(vtx_base + 0x54, 60.0f); W_FLOAT(vtx_base + 0x58, 40.0f); W_FLOAT(vtx_base + 0x5C, 0.0f);

    /* Region 2: ColorVtx */
    MEM_W32(vtx_base + 0x80, r2_color);
    W_FLOAT(vtx_base + 0x84, 260.0f); W_FLOAT(vtx_base + 0x88, 140.0f); W_FLOAT(vtx_base + 0x8C, 0.0f);
    MEM_W32(vtx_base + 0x90, r2_color);
    W_FLOAT(vtx_base + 0x94, 380.0f); W_FLOAT(vtx_base + 0x98, 220.0f); W_FLOAT(vtx_base + 0x9C, 0.0f);

    /* Region 3: TexVtx */
    W_FLOAT(vtx_base + 0xC0, 0.0f); W_FLOAT(vtx_base + 0xC4, 0.0f);
    W_FLOAT(vtx_base + 0xC8, 380.0f); W_FLOAT(vtx_base + 0xCC, 20.0f); W_FLOAT(vtx_base + 0xD0, 0.0f);
    W_FLOAT(vtx_base + 0xD4, 8.0f); W_FLOAT(vtx_base + 0xD8, 8.0f);
    W_FLOAT(vtx_base + 0xDC, 444.0f); W_FLOAT(vtx_base + 0xE0, 84.0f); W_FLOAT(vtx_base + 0xE4, 0.0f);

    /* Region 4: Triangles ColorVtx */
    MEM_W32(vtx_base + 0x100, r4_color);
    W_FLOAT(vtx_base + 0x104, 100.0f); W_FLOAT(vtx_base + 0x108, 180.0f); W_FLOAT(vtx_base + 0x10C, 0.0f);
    MEM_W32(vtx_base + 0x110, r4_color);
    W_FLOAT(vtx_base + 0x114, 180.0f); W_FLOAT(vtx_base + 0x118, 180.0f); W_FLOAT(vtx_base + 0x11C, 0.0f);
    MEM_W32(vtx_base + 0x120, r4_color);
    W_FLOAT(vtx_base + 0x124, 100.0f); W_FLOAT(vtx_base + 0x128, 240.0f); W_FLOAT(vtx_base + 0x12C, 0.0f);
    #undef W_FLOAT

    /* Build display list */
    uint32_t *dl = (uint32_t *)SR_HOST(dl_base);
    int p = 0;
    #define DL_CMD(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)

    DL_CMD(0x10, (vtx_base >> 8) & 0x000F0000u);   /* GE_BASE = high 4 bits of vtx_base (0x00080000) */
    DL_CMD(0x13, 0);                               /* GE_OFFSETADDR = 0 */
    DL_CMD(0x4C, 0);                               /* GE_OFFSETX = 0 */
    DL_CMD(0x4D, 0);                               /* GE_OFFSETY = 0 */
    DL_CMD(0x9C, fb_base & 0x00FFFFFFu);           /* GE_FRAMEBUFPTR */
    DL_CMD(0x9D, fb_stride | ((fb_base & 0xFF000000u) >> 8)); /* GE_FRAMEBUFWIDTH */
    DL_CMD(0xD2, 3);                               /* GE_FRAMEBUFPIXFORMAT = RGBA8888 */
    DL_CMD(0x15, 0);                               /* GE_REGION1 = (0,0) */
    DL_CMD(0x16, ((fb_h - 1) << 10) | (fb_w - 1)); /* GE_REGION2 */
    DL_CMD(0xD4, 0);                               /* GE_SCISSOR1 = (0,0) */
    DL_CMD(0xD5, ((fb_h - 1) << 10) | (fb_w - 1)); /* GE_SCISSOR2 */
    DL_CMD(0x23, 0);                               /* GE_ZTESTENABLE = 0 */
    DL_CMD(0x22, 0);                               /* GE_ALPHATESTENABLE = 0 */
    DL_CMD(0x21, 0);                               /* GE_ALPHABLENDENABLE = 0 */
    DL_CMD(0x1E, 0);                               /* GE_TEXTUREMAPENABLE = 0 */

    /* Step A: Clear screen via clear-mode sprite */
    DL_CMD(0xD3, 0x301);                           /* GE_CLEARMODE = 0x301 (color+alpha clear) */
    DL_CMD(0x12, (7 << 2) | (3 << 7) | (1 << 23)); /* GE_VERTEXTYPE = color + float pos + through */
    DL_CMD(0x01, vtx_base & 0x00FFFFFFu);          /* GE_VADDR */
    DL_CMD(0x04, (6 << 16) | 2);                   /* GE_PRIM = SPRITES (6), count = 2 */
    DL_CMD(0xD3, 0);                               /* GE_CLEARMODE = 0 (disable clear mode) */

    /* Step B: Draw Region 1 (Top-Left solid red rectangle) */
    DL_CMD(0x12, (7 << 2) | (3 << 7) | (1 << 23)); /* GE_VERTEXTYPE */
    DL_CMD(0x01, (vtx_base + 0x040) & 0x00FFFFFFu);/* GE_VADDR */
    DL_CMD(0x04, (6 << 16) | 2);                   /* GE_PRIM = SPRITES, count = 2 */

    /* Step C: Draw Region 2 (Bottom-Right solid green rectangle) */
    DL_CMD(0x12, (7 << 2) | (3 << 7) | (1 << 23)); /* GE_VERTEXTYPE */
    DL_CMD(0x01, (vtx_base + 0x080) & 0x00FFFFFFu);/* GE_VADDR */
    DL_CMD(0x04, (6 << 16) | 2);                   /* GE_PRIM = SPRITES, count = 2 */

    /* Step D: Draw Region 3 (Top-Right textured sprite) */
    DL_CMD(0xA0, tex_base & 0x00FFFFFFu);          /* GE_TEXADDR0 */
    DL_CMD(0xA8, 8 | ((tex_base & 0xFF000000u) >> 8)); /* GE_TEXBUFWIDTH0 = 8 */
    DL_CMD(0xB8, (3 << 8) | 3);                    /* GE_TEXSIZE0 = 8x8 */
    DL_CMD(0xC0, 0);                               /* GE_TEXMAPMODE = UV coordinates */
    DL_CMD(0xC3, 3);                               /* GE_TEXFORMAT = RGBA8888 */
    DL_CMD(0xC6, 0);                               /* GE_TEXFILTER = NEAREST */
    DL_CMD(0xC7, 0);                               /* GE_TEXWRAP = CLAMP */
    DL_CMD(0xC9, (1 << 8) | 3);                    /* GE_TEXFUNC = REPLACE with RGBA */
    DL_CMD(0x1E, 1);                               /* GE_TEXTUREMAPENABLE = 1 */
    DL_CMD(0x12, (3 << 0) | (3 << 7) | (1 << 23)); /* GE_VERTEXTYPE = UV + float pos + through */
    DL_CMD(0x01, (vtx_base + 0x0C0) & 0x00FFFFFFu);/* GE_VADDR */
    DL_CMD(0x04, (6 << 16) | 2);                   /* GE_PRIM = SPRITES, count = 2 */
    DL_CMD(0x1E, 0);                               /* GE_TEXTUREMAPENABLE = 0 */

    /* Step E: Draw Region 4 (Bottom-Left solid triangle) */
    DL_CMD(0x12, (7 << 2) | (3 << 7) | (1 << 23)); /* GE_VERTEXTYPE */
    DL_CMD(0x01, (vtx_base + 0x100) & 0x00FFFFFFu);/* GE_VADDR */
    DL_CMD(0x04, (3 << 16) | 3);                   /* GE_PRIM = TRIANGLES, count = 3 */

    /* Step F: Finish & End */
    DL_CMD(0x0F, 0);                               /* GE_FINISH */
    DL_CMD(0x0C, 0);                               /* GE_END */
    #undef DL_CMD

    /* Poison entire framebuffer with 0x5A */
    memset(SR_HOST(fb_base), 0x5A, fb_stride * fb_h * 4u);

    /* 4. Production Submission via sr_syscall(NID_SCE_GE_LIST_ENQUEUE) */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = dl_base;
    cpu.r[5] = 0;       /* stall = 0 (run to completion) */
    cpu.r[6] = 0;       /* cbid = 0 */
    cpu.r[7] = 0;       /* cbarg = 0 */
    uint32_t qid = sr_syscall(&cpu, NID_SCE_GE_LIST_ENQUEUE);
    expect((qid & 0xFF000000u) == 0x35000000u,
           "sceGeListEnQueue returns valid queue id (0x35xxxxxx)");

    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = qid;
    cpu.r[5] = 0;
    expect(sr_syscall(&cpu, NID_SCE_GE_LIST_SYNC) == 0u,
           "sceGeListSync confirms completed list status");

    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = 0;
    expect(sr_syscall(&cpu, NID_SCE_GE_DRAW_SYNC) == 0u,
           "sceGeDrawSync reports drawing complete");

    /* 5. Pixel & Boundary assertions */
    uint32_t *fb = (uint32_t *)SR_HOST(fb_base);

    /* Check clear background at all 4 corners */
    expect(fb[0 * fb_stride + 0] == bg_color, "framebuffer top-left corner is background color");
    expect(fb[0 * fb_stride + (fb_w - 1)] == bg_color, "framebuffer top-right corner is background color");
    expect(fb[(fb_h - 1) * fb_stride + 0] == bg_color, "framebuffer bottom-left corner is background color");
    expect(fb[(fb_h - 1) * fb_stride + (fb_w - 1)] == bg_color, "framebuffer bottom-right corner is background color");

    /* Region 1: (20,15) to (59,39) */
    expect(fb[15 * fb_stride + 20] == r1_color, "Region 1 top-left pixel matches expected color");
    expect(fb[27 * fb_stride + 35] == r1_color, "Region 1 interior pixel matches expected color");
    expect(fb[39 * fb_stride + 59] == r1_color, "Region 1 bottom-right pixel matches expected color");
    /* Region 1 boundary exclusivity */
    expect(fb[15 * fb_stride + 19] == bg_color, "Region 1 left edge is bounded (background)");
    expect(fb[15 * fb_stride + 60] == bg_color, "Region 1 right edge is bounded (background)");
    expect(fb[14 * fb_stride + 20] == bg_color, "Region 1 top edge is bounded (background)");
    expect(fb[40 * fb_stride + 20] == bg_color, "Region 1 bottom edge is bounded (background)");

    /* Region 2: (260,140) to (379,219) */
    expect(fb[140 * fb_stride + 260] == r2_color, "Region 2 top-left pixel matches expected color");
    expect(fb[180 * fb_stride + 320] == r2_color, "Region 2 interior pixel matches expected color");
    expect(fb[219 * fb_stride + 379] == r2_color, "Region 2 bottom-right pixel matches expected color");
    /* Region 2 boundary exclusivity */
    expect(fb[140 * fb_stride + 259] == bg_color, "Region 2 left edge is bounded (background)");
    expect(fb[140 * fb_stride + 380] == bg_color, "Region 2 right edge is bounded (background)");
    expect(fb[139 * fb_stride + 260] == bg_color, "Region 2 top edge is bounded (background)");
    expect(fb[220 * fb_stride + 260] == bg_color, "Region 2 bottom edge is bounded (background)");

    /* Region 3: Textured (380,20) to (443,83) */
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    expect(fb[(20 + 2) * fb_stride + (380 + 2)] == tex[0 * 8 + 0],
           "Region 3 textured texel (0,0) center matches source texture");
    expect(fb[(20 + 7 * 8 + 2) * fb_stride + (380 + 7 * 8 + 2)] == tex[7 * 8 + 7],
           "Region 3 textured texel (7,7) center matches source texture");
    expect(fb[20 * fb_stride + 379] == bg_color, "Region 3 left edge is bounded (background)");
    expect(fb[20 * fb_stride + 444] == bg_color, "Region 3 right edge is bounded (background)");

    /* Region 4: Triangle (100,180)-(180,180)-(100,240) */
    expect(fb[190 * fb_stride + 110] == r4_color, "Region 4 triangle interior pixel matches color");
    expect(fb[230 * fb_stride + 160] == bg_color, "Region 4 outside hypotenuse is background color");

    /* Anti-symmetry / Anti-mirror checks */
    expect(fb[27 * fb_stride + (fb_w - 1 - 35)] == bg_color,
           "Horizontally mirrored Region 1 is not mutated (anti-mirror pass)");
    expect(fb[(fb_h - 1 - 27) * fb_stride + 35] == bg_color,
           "Vertically mirrored Region 1 is not mutated (anti-mirror pass)");
    expect(fb[150 * fb_stride + (fb_w - 1 - 320)] == bg_color,
           "Horizontally mirrored Region 2 is not mutated (anti-mirror pass)");
    expect(fb[(fb_h - 1 - 150) * fb_stride + 320] == bg_color,
           "Vertically mirrored Region 2 is not mutated (anti-mirror pass)");

    /* 6. Deterministic Canonical Region & Framebuffer Hashes */
    uint64_t r1_hash = ge_sentinel_hash64_region(fb_base, fb_stride, 20, 15, 40, 25);
    uint64_t r2_hash = ge_sentinel_hash64_region(fb_base, fb_stride, 260, 140, 120, 80);
    uint64_t r3_hash = ge_sentinel_hash64_region(fb_base, fb_stride, 380, 20, 64, 64);
    uint64_t r4_hash = ge_sentinel_hash64_region(fb_base, fb_stride, 100, 180, 80, 60);
    uint64_t fb_hash = ge_sentinel_hash64_region(fb_base, fb_stride, 0, 0, fb_w, fb_h);

    expect(r1_hash == 0x0eb6697557c1d045ULL,
           "Region 1 matches canonical 64-bit FNV-1a hash (0x0eb6697557c1d045)");
    expect(r2_hash == 0xbf1aca308f0c0a25ULL,
           "Region 2 matches canonical 64-bit FNV-1a hash (0xbf1aca308f0c0a25)");
    expect(r3_hash == 0x2158c050396fe725ULL,
           "Region 3 matches canonical 64-bit FNV-1a hash (0x2158c050396fe725)");
    expect(r4_hash == 0x04cfd0712d0f88e5ULL,
           "Region 4 triangle matches canonical 64-bit FNV-1a hash (0x04cfd0712d0f88e5)");
    expect(fb_hash == 0x6b749d6fd93580c5ULL,
           "Full 480x272 framebuffer matches canonical 64-bit FNV-1a hash (0x6b749d6fd93580c5)");

    /* 7. Callback registration lifecycle */
    const uint32_t cb_struct = 0x08920000u;
    MEM_W32(cb_struct + 0, 0);  /* signal_func */
    MEM_W32(cb_struct + 4, 0);  /* signal_arg */
    MEM_W32(cb_struct + 8, 0);  /* finish_func */
    MEM_W32(cb_struct + 12, 0); /* finish_arg */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = cb_struct;
    uint32_t cbid = sr_syscall(&cpu, NID_SCE_GE_SET_CALLBACK);
    expect(cbid < 16u, "sceGeSetCallback returns valid callback ID (< 16)");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = cbid;
    expect(sr_syscall(&cpu, NID_SCE_GE_UNSET_CALLBACK) == 0u,
           "sceGeUnsetCallback unregisters callback successfully");

    /* 8. Stalled list and sceGeListUpdateStallAddr test */
    const uint32_t dl2_base = 0x08910000u;
    uint32_t *dl2 = (uint32_t *)SR_HOST(dl2_base);
    int p2 = 0;
    #define DL2_CMD(cmd, val) dl2[p2++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    #define W_FLOAT2(addr, val) do { float _f = (val); uint32_t _u; memcpy(&_u, &_f, 4); MEM_W32((addr), _u); } while(0)

    const uint32_t white_color = 0xFFFFFFFFu;
    const uint32_t purple_color = 0xFF880088u;

    /* Setup clear-to-white vertices at vtx_base + 0x180 */
    MEM_W32(vtx_base + 0x180, white_color);
    W_FLOAT2(vtx_base + 0x184, 0.0f); W_FLOAT2(vtx_base + 0x188, 0.0f); W_FLOAT2(vtx_base + 0x18C, 0.0f);
    MEM_W32(vtx_base + 0x190, white_color);
    W_FLOAT2(vtx_base + 0x194, 480.0f); W_FLOAT2(vtx_base + 0x198, 272.0f); W_FLOAT2(vtx_base + 0x19C, 0.0f);

    /* Setup purple rectangle vertices at vtx_base + 0x140 */
    MEM_W32(vtx_base + 0x140, purple_color);
    W_FLOAT2(vtx_base + 0x144, 50.0f); W_FLOAT2(vtx_base + 0x148, 50.0f); W_FLOAT2(vtx_base + 0x14C, 0.0f);
    MEM_W32(vtx_base + 0x150, purple_color);
    W_FLOAT2(vtx_base + 0x154, 90.0f); W_FLOAT2(vtx_base + 0x158, 90.0f); W_FLOAT2(vtx_base + 0x15C, 0.0f);

    DL2_CMD(0x10, (vtx_base >> 8) & 0x000F0000u);
    DL2_CMD(0x13, 0);
    DL2_CMD(0x4C, 0);
    DL2_CMD(0x4D, 0);
    DL2_CMD(0x9C, fb_base & 0x00FFFFFFu);
    DL2_CMD(0x9D, fb_stride | ((fb_base & 0xFF000000u) >> 8));
    DL2_CMD(0xD2, 3);
    DL2_CMD(0x15, 0);
    DL2_CMD(0x16, ((fb_h - 1) << 10) | (fb_w - 1));
    DL2_CMD(0xD4, 0);
    DL2_CMD(0xD5, ((fb_h - 1) << 10) | (fb_w - 1));
    DL2_CMD(0x23, 0); DL2_CMD(0x22, 0); DL2_CMD(0x21, 0); DL2_CMD(0x1E, 0);

    /* Clear to white */
    DL2_CMD(0xD3, 0x301);
    DL2_CMD(0x12, (7 << 2) | (3 << 7) | (1 << 23));
    DL2_CMD(0x01, (vtx_base + 0x180) & 0x00FFFFFFu);
    DL2_CMD(0x04, (6 << 16) | 2);
    DL2_CMD(0xD3, 0);

    /* Part 1 end (stall here): address dl2_base + p2*4 */
    uint32_t stall_point = dl2_base + (uint32_t)(p2 * 4);

    /* Part 2: Draw purple rectangle */
    DL2_CMD(0x12, (7 << 2) | (3 << 7) | (1 << 23));
    DL2_CMD(0x01, (vtx_base + 0x140) & 0x00FFFFFFu);
    DL2_CMD(0x04, (6 << 16) | 2);
    DL2_CMD(0x0F, 0);
    DL2_CMD(0x0C, 0);
    uint32_t end_point = dl2_base + (uint32_t)(p2 * 4);
    #undef W_FLOAT2
    #undef DL2_CMD

    /* Submit with stall at stall_point */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = dl2_base;
    cpu.r[5] = stall_point;
    cpu.r[6] = 0;
    cpu.r[7] = 0;
    uint32_t qid2 = sr_syscall(&cpu, NID_SCE_GE_LIST_ENQUEUE);
    expect((qid2 & 0xFF000000u) == 0x35000000u,
           "sceGeListEnQueue with stall returns valid queue id");

    /* Stalled list sync with syncType=1 returns 1 (stalled) */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = qid2;
    cpu.r[5] = 1;
    expect(sr_syscall(&cpu, NID_SCE_GE_LIST_SYNC) == 1u,
           "sceGeListSync(syncType=1) reports list is currently stalled");

    /* Framebuffer is cleared to white, but purple rect is NOT drawn yet */
    expect(fb[0 * fb_stride + 0] == white_color, "stalled list executed clear to white");
    expect(fb[60 * fb_stride + 60] == white_color, "purple rect not yet rendered before stall advance");

    /* Advance stall to end_point via sceGeListUpdateStallAddr */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = qid2;
    cpu.r[5] = end_point;
    expect(sr_syscall(&cpu, NID_SCE_GE_LIST_UPDATE_STALL_ADDR) == 0u,
           "sceGeListUpdateStallAddr advances stall address and resumes execution");

    /* Now list is completed */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = qid2;
    cpu.r[5] = 0;
    expect(sr_syscall(&cpu, NID_SCE_GE_LIST_SYNC) == 0u,
           "sceGeListSync confirms completed list after stall advance");

    /* Purple rectangle is now rendered */
    expect(fb[60 * fb_stride + 60] == purple_color,
           "resumed list rendered purple rectangle to completion");
}

static uint32_t ge_transfer_enqueue(uint32_t src, uint32_t src_stride,
                                    uint32_t src_x, uint32_t src_y,
                                    uint32_t dst, uint32_t dst_stride,
                                    uint32_t dst_x, uint32_t dst_y,
                                    uint32_t width, uint32_t height,
                                    uint32_t bytes_per_pixel) {
    const uint32_t list_addr = 0x08940000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    uint32_t p = 0;
#define XFER_CMD(cmd, value) \
    do { dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(value) & 0x00ffffffu); } while (0)
    XFER_CMD(0xb2, src);
    XFER_CMD(0xb3, src_stride | ((src & 0xff000000u) >> 8));
    XFER_CMD(0xb4, dst);
    XFER_CMD(0xb5, dst_stride | ((dst & 0xff000000u) >> 8));
    XFER_CMD(0xeb, (src_y << 10) | src_x);
    XFER_CMD(0xec, (dst_y << 10) | dst_x);
    XFER_CMD(0xee, ((height - 1u) << 10) | (width - 1u));
    XFER_CMD(0xea, bytes_per_pixel == 4u ? 1u : 0u);
    XFER_CMD(0x0f, 0u);
    XFER_CMD(0x0c, 0u);
#undef XFER_CMD

    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = list_addr;
    return sr_syscall(&cpu, NID_SCE_GE_LIST_ENQUEUE);
}

static uint32_t ge_clut_enqueue(uint32_t src, uint32_t blocks) {
    const uint32_t list_addr = 0x08941000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    dl[0] = (0xb0u << 24) | (src & 0x00ffffffu);
    dl[1] = (0xb1u << 24) | ((src & 0xff000000u) >> 8);
    dl[2] = (0xc4u << 24) | (blocks & 0x00ffffffu);
    dl[3] = 0x0f000000u;
    dl[4] = 0x0c000000u;
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = list_addr;
    return sr_syscall(&cpu, NID_SCE_GE_LIST_ENQUEUE);
}

/* Host-memory safety for the production GE block-transfer command path.  The
 * temporary arena deliberately commits one complete maximum-transfer window after
 * the modeled 192 MiB guest arena.  That makes the historical later-row write
 * observable as a canary mutation rather than an access violation: the regression
 * fails before the fix, while the host process remains safe and deterministic. */
static void test_ge_block_transfer_span_atomicity(void) {
    const size_t arena_bytes = 0x0c000000u;
    const size_t extra_bytes = 0x00400000u;
    uint8_t *saved_base = g_mem_base;
    uint8_t *scratch = (uint8_t *)calloc(1, arena_bytes + extra_bytes);
    expect(scratch != NULL, "GE transfer regression allocates a guarded canary arena");
    if (!scratch) return;

    g_mem_base = scratch;
    g_mem = scratch + 0x08000000u;
    reset_fixture();
    sr_hle_init();

    /* Destination first row is valid; row two starts four bytes past the arena. */
    MEM_W32(0x08010000u, 0x11223344u);
    MEM_W32(0x08010020u, 0xa1b2c3d4u);
    MEM_W32(0x0bfff004u, 0x55555555u);
    memcpy(scratch + arena_bytes + 4u, &(uint32_t){0x66666666u}, 4u);
    gpu_dirty_reset();
    uint32_t qid = ge_transfer_enqueue(0x08010000u, 8u, 0u, 0u,
                                       0x0bfff000u, 1024u, 1u, 0u,
                                       1u, 2u, 4u);
    uint32_t outside = 0u;
    memcpy(&outside, scratch + arena_bytes + 4u, 4u);
    expect((qid & 0xff000000u) == 0x35000000u,
           "invalid-destination GE list still completes through production dispatch");
    expect(MEM_R32(0x0bfff004u) == 0x55555555u && outside == 0x66666666u,
           "a later invalid destination row causes no prefix or native-canary write");
    expect(s_gpu_dirty_calls == 0u,
           "a rejected destination rectangle causes no GPU dirty notification");

    /* Mirror the shape: the source tail is outside while both destination rows are valid. */
    MEM_W32(0x0bfff004u, 0x778899aau);
    memcpy(scratch + arena_bytes + 4u, &(uint32_t){0xbbccddefu}, 4u);
    MEM_W32(0x08020000u, 0x13572468u);
    MEM_W32(0x08020020u, 0x24681357u);
    gpu_dirty_reset();
    (void)ge_transfer_enqueue(0x0bfff000u, 1024u, 1u, 0u,
                              0x08020000u, 8u, 0u, 0u,
                              1u, 2u, 4u);
    expect(MEM_R32(0x08020000u) == 0x13572468u &&
               MEM_R32(0x08020020u) == 0x24681357u,
           "a later invalid source row causes no partial destination mutation");
    expect(s_gpu_dirty_calls == 0u,
           "a rejected source rectangle causes no GPU dirty notification");

    /* The register arithmetic itself must not wrap a high base into physical zero. */
    MEM_W32(0x00000000u, 0xdeadc0deu);
    MEM_W32(0x08021000u, 0xabcdef01u);
    gpu_dirty_reset();
    (void)ge_transfer_enqueue(0xfffffff0u, 8u, 4u, 0u,
                              0x08021000u, 8u, 0u, 0u,
                              1u, 1u, 4u);
    expect(MEM_R32(0x08021000u) == 0xabcdef01u && s_gpu_dirty_calls == 0u,
           "overflowing base-plus-origin arithmetic is rejected before any write");

    /* Last byte exactly at the arena end is valid, including on a later row. */
    MEM_W32(0x08030000u, 0x01020304u);
    MEM_W32(0x08030020u, 0x11121314u);
    MEM_W32(0x0bffeffcu, 0u);
    MEM_W32(0x0bfffffcu, 0u);
    gpu_dirty_reset();
    (void)ge_transfer_enqueue(0x08030000u, 8u, 0u, 0u,
                              0x0bffeff0u, 1024u, 3u, 0u,
                              1u, 2u, 4u);
    expect(MEM_R32(0x0bffeffcu) == 0x01020304u &&
               MEM_R32(0x0bfffffcu) == 0x11121314u,
           "a complete transfer ending exactly at the arena boundary succeeds");
    expect(s_gpu_dirty_calls == 2u && s_gpu_dirty_addr == 0x0bfffffcu &&
               s_gpu_dirty_bytes == 4u,
           "a valid two-row transfer reports each completed destination row");

    /* PSP transfers permit width greater than stride; the rows overlap in the
     * bounding span but remain a valid, fully contained memory shape. */
    for (uint32_t i = 0; i < 48u; i++) {
        MEM_W8(0x08040000u + i, (uint8_t)(0x20u + i));
        MEM_W8(0x08050000u + i, 0u);
    }
    gpu_dirty_reset();
    (void)ge_transfer_enqueue(0x08040000u, 8u, 0u, 0u,
                              0x08050000u, 8u, 0u, 0u,
                              16u, 2u, 2u);
    int narrow_pitch_ok = 1;
    for (uint32_t i = 0; i < 48u; i++)
        if (MEM_R8(0x08050000u + i) != (uint8_t)(0x20u + i)) narrow_pitch_ok = 0;
    expect(narrow_pitch_ok, "a valid transfer preserves width-greater-than-stride semantics");

    /* Valid physical aliases and an overlapping row retain the old memmove contract. */
    MEM_W32(0x08060000u, 0xcafebabeu);
    MEM_W32(0x08061000u, 0u);
    (void)ge_transfer_enqueue(0x88060000u, 8u, 0u, 0u,
                              0xa8061000u, 8u, 0u, 0u,
                              1u, 1u, 4u);
    expect(MEM_R32(0x08061000u) == 0xcafebabeu,
           "kseg aliases remain valid for a completely contained transfer");
    for (uint32_t i = 0; i < 5u; i++) MEM_W32(0x08070000u + i * 4u, i + 1u);
    (void)ge_transfer_enqueue(0x08070000u, 8u, 0u, 0u,
                              0x08070000u, 8u, 1u, 0u,
                              4u, 1u, 4u);
    expect(MEM_R32(0x08070004u) == 1u && MEM_R32(0x08070010u) == 4u,
           "valid source/destination overlap keeps row-wise memmove semantics");

    /* The GPU texture fast path is a production helper over the same native-pointer
     * boundary.  A valid first texel with row two outside must return a fully zeroed
     * decode, never read the committed host canary. */
    GeState *state = ge_state_ptr();
    state->tex_addr = 0x0bfffffcu;
    state->tex_bufw = 1u;
    state->tex_fmt = 3u;
    state->tex_w = 1;
    state->tex_h = 2;
    state->tex_swizzle = 0;
    MEM_W32(0x0bfffffcu, 0x01020304u);
    memcpy(scratch + arena_bytes, &(uint32_t){0xaabbccddu}, 4u);
    uint32_t decoded[2] = { 0x55555555u, 0x66666666u };
    ge_decode_tex_rgba(decoded);
    expect(decoded[0] == 0u && decoded[1] == 0u,
           "linear texture decode rejects the complete invalid rectangle before reading");

    /* Tightest reachable overshoot. TEXADDR0 carries no alignment mask, so a guest
     * can place a 16-bpp linear texture at an odd address whose final row ends
     * exactly ONE byte past the arena. Widening the accepted extent by a single
     * byte is invisible to every other assertion in this file, so pin both sides
     * of that boundary through the same production decode helper. */
    state->tex_fmt = 0u;
    state->tex_bufw = 1u;
    state->tex_w = 1;
    state->tex_h = 2;
    state->tex_addr = 0x0bfffffdu;                 /* last row ends at arena_end + 1 */
    memcpy(scratch + arena_bytes, &(uint16_t){0x7fffu}, 2u);
    decoded[0] = 0x55555555u; decoded[1] = 0x66666666u;
    ge_decode_tex_rgba(decoded);
    expect(decoded[0] == 0u && decoded[1] == 0u,
           "a texture rectangle ending one byte past the arena is rejected whole");

    state->tex_addr = 0x0bfffffcu;                 /* last row ends AT arena_end */
    MEM_W16(0x0bfffffcu, 0xffffu);
    MEM_W16(0x0bfffffeu, 0xffffu);
    decoded[0] = 0u; decoded[1] = 0u;
    ge_decode_tex_rgba(decoded);
    expect(decoded[0] != 0u && decoded[1] != 0u,
           "a texture rectangle ending exactly at the arena end still decodes");

    /* LOADCLUT previously updated its internal palette one scalar at a time.  A
     * rejected complete source span must leave both palette bytes and generation
     * unchanged. */
    memset(state->clutram, 0x5au, sizeof(state->clutram));
    uint32_t clut_generation = state->clut_gen;
    MEM_W32(0x0bfffffcu, 0x12345678u);
    (void)ge_clut_enqueue(0x0bfffffcu, 1u);
    int clut_unchanged = state->clut_gen == clut_generation;
    for (uint32_t i = 0; i < sizeof(state->clutram); i++)
        if (state->clutram[i] != 0x5au) clut_unchanged = 0;
    expect(clut_unchanged,
           "LOADCLUT rejects an invalid full source span without partial palette state");

    ge_set_gpu_hooks(NULL);
    g_mem_base = saved_base;
    g_mem = saved_base + 0x08000000u;
    free(scratch);
}


static void test_bulk_guest_span_atomicity(void) {
    reset_fixture();
    sr_hle_init();
    const uint32_t src = 0x0bffff80u;
    const uint32_t dst = 0x0bffffc0u;
    for (uint32_t i = 0; i < 64u; i++) {
        MEM_W8(src + i, (uint8_t)(0x30u + i));
        MEM_W8(dst + i, 0xa5u);
    }
    expect(bulk_call(NID_SCE_KERNEL_MEMCPY, dst, src, 64u) == dst,
           "production memcpy returns the destination on a valid span");
    expect(MEM_R8(dst) == 0x30u && MEM_R8(dst + 63u) == 0x6fu,
           "production memcpy copies the complete valid span");
    for (uint32_t i = 0; i < 16u; i++) MEM_W8(0x0bfffff0u + i, 0x5au);
    expect(bulk_call(NID_SCE_KERNEL_MEMCPY, 0x0bfffff0u, src, 17u) == 0x0bfffff0u,
           "production memcpy reports the destination for a rejected span");
    expect(MEM_R8(0x0bfffff0u) == 0x5au && MEM_R8(0x0bfffff0u + 15u) == 0x5au,
           "rejected memcpy performs no partial guest mutation");
    expect(bulk_call(NID_SCE_KERNEL_MEMSET, dst, 0x7cu, 64u) == dst,
           "production memset returns the destination on a valid span");
    expect(MEM_R8(dst) == 0x7cu && MEM_R8(dst + 63u) == 0x7cu,
           "production memset writes the complete valid span");
    for (uint32_t i = 0; i < 16u; i++) MEM_W8(0x0bfffff0u + i, 0x3cu);
    expect(bulk_call(NID_SCE_KERNEL_MEMSET, 0x0bfffff0u, 0x44u, 17u) == 0x0bfffff0u,
           "production memset reports the destination for a rejected span");
    expect(MEM_R8(0x0bfffff0u) == 0x3cu && MEM_R8(0x0bfffff0u + 15u) == 0x3cu,
           "rejected memset performs no partial guest mutation");
    for (uint32_t i = 0; i < 16u; i++) MEM_W8(0x0bfffff0u + i, 0x2au);
    expect(bulk_call(NID_SCE_DMAC_MEMCPY, 0x0bfffff0u, src, 17u) == SCE_DMAC_ILLEGAL_ADDR,
           "production DMA reports its PSP return value for a rejected span");
    expect(MEM_R8(0x0bfffff0u) == 0x2au && MEM_R8(0x0bfffff0u + 15u) == 0x2au,
           "rejected DMA performs no partial guest mutation");
}

/* ---- sceDmacMemcpy / sceDmacTryMemcpy hardware regressions ------------------
 *
 * Production dispatch: every call below enters through sr_syscall with the real
 * registered NID, so these assert the PSP-visible return value and the
 * PSP-visible memory state of the shipped handlers.
 *
 * The expected values come from repeated PSP-3001 / 6.61-ARK observations;
 * private capture details are intentionally not part of this public-safe tree.
 * The measured large-transfer ceiling is asserted as a prefix copy with an
 * untouched tail. The invalid-truncated-tail case is deliberately labelled as
 * a conservative runtime policy: hardware has not yet established whether the
 * tail is validated before the effective transfer length is applied. */
extern uint32_t sr_hle_test_dmac_effective_max(void);

/* The probe's source pattern: byte i of the source buffer is 0x10 + (i & 0x3F).
 * Reusing the exact fill makes the expected bytes below the same literals the
 * hardware capture reported (0x10 at offset 0, 0x4F at offset 1023, ...). */
static uint8_t dmac_pattern(uint32_t i) { return (uint8_t)(0x10u + (i & 0x3Fu)); }

static void dmac_fill(uint32_t addr, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) MEM_W8(addr + i, dmac_pattern(i));
}

static void dmac_clear(uint32_t addr, uint32_t n, uint8_t value) {
    for (uint32_t i = 0; i < n; i++) MEM_W8(addr + i, value);
}

/* Whole-span comparison, so "copies completely" is proven across every byte
 * rather than at the two or three offsets the hardware probe could sample. */
static int dmac_span_matches(uint32_t addr, uint32_t n, uint32_t pattern_origin) {
    for (uint32_t i = 0; i < n; i++)
        if (MEM_R8(addr + i) != dmac_pattern(pattern_origin + i)) return 0;
    return 1;
}

static int dmac_span_is(uint32_t addr, uint32_t n, uint8_t value) {
    for (uint32_t i = 0; i < n; i++)
        if (MEM_R8(addr + i) != value) return 0;
    return 1;
}

static void test_dmac_hardware_semantics(uint32_t nid, const char *who) {
    reset_fixture();
    sr_hle_init();

    const uint32_t src = 0x08200000u;
    const uint32_t dst = 0x08400000u;

    /* --- proven: illegal size, illegal address, and failure atomicity ------- */

    /* Hardware: valid pointers, size 0 -> 0x80000104 (v4 and v6, 2/2 each). */
    dmac_fill(src, 256u);
    dmac_clear(dst, 256u, 0xa5u);
    gpu_dirty_reset();
    expect(bulk_call(nid, dst, src, 0u) == SCE_DMAC_ILLEGAL_SIZE,
           "PSP: zero size returns the illegal-size error");
    expect(dmac_span_is(dst, 256u, 0xa5u),
           "PSP: a zero-size request modifies no destination byte");
    expect(s_gpu_dirty_calls == 0u,
           "a zero-size request issues no GPU dirty notification");

    /* Hardware: NULL dst or NULL src with size 64 -> 0x80000103 (v4 and v6).
     * Guest address 0 is inside this runtime's flat arena, so without an
     * explicit check it would pass span validation and silently copy. */
    expect(bulk_call(nid, 0u, src, 64u) == SCE_DMAC_ILLEGAL_ADDR,
           "PSP: a NULL destination returns the illegal-address error");
    expect(bulk_call(nid, dst, 0u, 64u) == SCE_DMAC_ILLEGAL_ADDR,
           "PSP: a NULL source returns the illegal-address error");
    expect(dmac_span_is(dst, 256u, 0xa5u),
           "PSP: a NULL-pointer request modifies no destination byte");

    /* Checked span arithmetic. The arena ends at guest physical 0x0c000000, so
     * these requests end exactly one byte past it or wrap uint32_t outright.
     * A base-address-only check or an `addr + size` comparison would accept
     * them; both must be rejected before any byte moves. */
    dmac_clear(0x0bfffff0u, 16u, 0x3bu);
    expect(bulk_call(nid, 0x0bfffff0u, src, 17u) == SCE_DMAC_ILLEGAL_ADDR,
           "PSP class: a destination span ending past the arena is rejected");
    expect(bulk_call(nid, dst, 0x0bfffff0u, 17u) == SCE_DMAC_ILLEGAL_ADDR,
           "PSP class: a source span ending past the arena is rejected");
    expect(bulk_call(nid, 0x0bffff00u, src, 0xFFFFFF00u) == SCE_DMAC_ILLEGAL_ADDR,
           "PSP class: a destination span that wraps uint32_t is rejected");
    expect(bulk_call(nid, dst, 0x0bffff00u, 0xFFFFFF00u) == SCE_DMAC_ILLEGAL_ADDR,
           "PSP class: a source span that wraps uint32_t is rejected");
    expect(dmac_span_is(0x0bfffff0u, 16u, 0x3bu) && dmac_span_is(dst, 256u, 0xa5u),
           "PSP: a rejected span leaves both buffers byte-for-byte unchanged");
    /* Every rejection above ran with the counter still at zero. A GPU dirty
     * notification for a transfer that never happened would invalidate a live
     * texture or framebuffer cache entry for no reason. */
    expect(s_gpu_dirty_calls == 0u,
           "no rejected DMA request issues a GPU dirty notification");

    /* Exactly-to-the-end must still be accepted: the bound is the real arena
     * end, not a conservative margin that would reject legal transfers. */
    dmac_fill(src, 16u);
    expect(bulk_call(nid, 0x0bfffff0u, src, 16u) == 0u,
           "a span ending exactly at the arena end is accepted");
    expect(dmac_span_matches(0x0bfffff0u, 16u, 0u),
           "a span ending exactly at the arena end copies completely");
    /* ...and a successful transfer dirties exactly its own destination range. */
    expect(s_gpu_dirty_calls == 1u && s_gpu_dirty_addr == 0x0bfffff0u &&
               s_gpu_dirty_bytes == 16u,
           "a successful DMA dirties exactly the destination range, once");

    /* --- proven: sizes that hardware measured as complete copies ------------ */

    /* 16385 and 32769 are the sizes that ruled out a 16 KiB / 32 KiB ceiling on
     * hardware; every byte is checked here, not just the sampled endpoints. */
    static const uint32_t full_sizes[] = { 1u, 1024u, 4096u, 16384u, 16385u, 32768u, 32769u };
    for (unsigned i = 0; i < sizeof(full_sizes) / sizeof(full_sizes[0]); i++) {
        const uint32_t n = full_sizes[i];
        dmac_fill(src, n);
        dmac_clear(dst, n + 1u, 0u);
        expect(bulk_call(nid, dst, src, n) == 0u,
               "PSP: a hardware-verified transfer size returns success");
        expect(dmac_span_matches(dst, n, 0u),
               "PSP: a hardware-verified transfer size copies every byte");
        expect(MEM_R8(dst + n) == 0u,
               "a transfer writes nothing past its requested size");
    }

    /* --- coherency matrix: RAM/VRAM directions and aliases ------------------
     * The HLE entry must use the unified SR_HOST mapping for every source and
     * destination class. The real GPU hook decides whether a dirty range is
     * relevant; this production-dispatch fixture records the exact address and
     * byte count it receives so the DMA contract cannot lose alias information
     * before the renderer canonicalizes it. */
    {
        struct DmacDirectionCase {
            uint32_t dst;
            uint32_t src;
            const char *label;
        };
        static const struct DmacDirectionCase cases[] = {
            { 0x08300000u, 0x08280000u, "RAM-to-RAM" },
            { 0x04080000u, 0x08320000u, "RAM-to-VRAM" },
            { 0x08310000u, 0x04070000u, "VRAM-to-RAM" },
            { 0x040a0000u, 0x04090000u, "VRAM-to-VRAM" },
            { 0x440b0000u, 0x08330000u, "aliased VRAM destination" },
            { 0x08340000u, 0x440c0000u, "aliased VRAM source" },
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            const uint32_t n = 64u;
            dmac_fill(cases[i].src, n);
            dmac_clear(cases[i].dst, n + 1u, 0x5cu);
            gpu_dirty_reset();
            expect(bulk_call(nid, cases[i].dst, cases[i].src, n) == 0u,
                   cases[i].label);
            expect(dmac_span_matches(cases[i].dst, n, 0u),
                   "a DMA direction copies through the unified guest mapping");
            expect(s_gpu_dirty_calls == 1u && s_gpu_dirty_addr == cases[i].dst &&
                       s_gpu_dirty_bytes == n,
                   "a DMA direction reports its exact destination range");
        }
        /* The aliased destination must be the same physical bytes as its
         * canonical 0x04xxxxxx address, not a second host allocation. */
        expect(dmac_span_matches(0x040b0000u, 64u, 0u),
               "an aliased VRAM destination is visible through its canonical address");
    }

    /* --- proven: same-pointer and overlap behave like memmove --------------- */

    /* Hardware: dst == src, 16384 bytes -> 0, buffer intact (v4, 2/2). */
    dmac_fill(src, 16384u);
    expect(bulk_call(nid, src, src, 16384u) == 0u,
           "PSP: a same-pointer self copy returns success");
    expect(dmac_span_matches(src, 16384u, 0u),
           "PSP: a same-pointer self copy leaves the buffer unchanged");

    /* Hardware: forward overlap dst = src + 16, 16384 bytes, lands
     * memmove-correct (v4 sampled offsets 16, 128 and 0x3FFF; checked here
     * across the whole span). A naive forward byte loop would smear the first
     * 16 bytes across the destination and fail this. */
    dmac_fill(src, 32768u);
    expect(bulk_call(nid, src + 16u, src, 16384u) == 0u,
           "PSP: a forward-overlapping copy returns success");
    expect(dmac_span_matches(src + 16u, 16384u, 0u),
           "PSP: a forward-overlapping copy is memmove-correct across the span");

    /* Hardware: backward overlap dst = src, source = src + 16 (v4, 2/2). */
    dmac_fill(src, 32768u);
    expect(bulk_call(nid, src, src + 16u, 16384u) == 0u,
           "PSP: a backward-overlapping copy returns success");
    expect(dmac_span_matches(src, 16384u, 16u),
           "PSP: a backward-overlapping copy is memmove-correct across the span");

    /* --- measured: the 0xC000 effective ceiling ----------------------------- */

    /* Independent PSP-3001 / 6.61-ARK runs bracketed the boundary at 0xC000:
     * 0xBFFF and 0xC000 are complete, while larger requests return success,
     * copy the contiguous prefix, and leave the remainder untouched. Assert
     * every byte for both registered NIDs, including the dirty range reported
     * to the renderer. */
    const uint32_t ceiling = sr_hle_test_dmac_effective_max();
    expect(ceiling == 0xC000u, "the measured DMA effective ceiling is 0xC000");
    static const uint32_t ceiling_sizes[] = {
        0xBFFFu, 0xC000u, 0xC001u, 0xD000u, 0xF000u, 0xFFFFu, 0x10000u
    };
    for (unsigned i = 0; i < sizeof(ceiling_sizes) / sizeof(ceiling_sizes[0]); i++) {
        const uint32_t requested = ceiling_sizes[i];
        const uint32_t effective = requested > ceiling ? ceiling : requested;
        dmac_fill(src, requested);
        dmac_clear(dst, requested + 1u, 0xa7u);
        gpu_dirty_reset();
        expect(bulk_call(nid, dst, src, requested) == 0u,
               "a measured-ceiling request returns success");
        expect(dmac_span_matches(dst, effective, 0u),
               "a measured-ceiling request copies the complete effective prefix");
        expect(dmac_span_is(dst + effective, requested - effective, 0xa7u),
               "a measured-ceiling request leaves the truncated tail untouched");
        expect(MEM_R8(dst + requested) == 0xa7u,
               "a measured-ceiling request writes nothing past its request");
        expect(s_gpu_dirty_calls == 1u && s_gpu_dirty_addr == dst &&
                   s_gpu_dirty_bytes == effective,
               "a measured-ceiling request dirties only the effective destination prefix");
    }

    /* Conservative memory-safety policy (not a hardware claim): a request
     * whose effective prefix is in range but whose requested tail crosses the
     * modeled arena is rejected atomically until hardware settles precedence.
     * This prevents a partially validated bulk access from reaching SR_HOST. */
    const uint32_t invalid_tail_dst = 0x0bff1000u;
    const uint32_t invalid_tail_src = 0x08210000u;
    const uint32_t invalid_tail_size = 0x10000u;
    dmac_fill(invalid_tail_src, invalid_tail_size);
    dmac_clear(invalid_tail_dst, ceiling, 0x6du);
    gpu_dirty_reset();
    expect(bulk_call(nid, invalid_tail_dst, invalid_tail_src, invalid_tail_size) ==
               SCE_DMAC_ILLEGAL_ADDR,
           "the conservative policy rejects an invalid requested tail");
    expect(dmac_span_is(invalid_tail_dst, ceiling, 0x6du),
           "an invalid requested tail causes no prefix mutation");
    expect(s_gpu_dirty_calls == 0u,
           "an invalid requested tail causes no GPU dirty notification");

    (void)who;
}

/* Hardware measured sceDmacTryMemcpy blocking for the full transfer and
 * producing the same content as the blocking form at every size it tried, so
 * the whole contract above is asserted against both NIDs. No BUSY result was
 * ever observed in any session, so none is asserted or fabricated. */
static void test_dmac_semantics(void) {
    test_dmac_hardware_semantics(NID_SCE_DMAC_MEMCPY, "sceDmacMemcpy");
    test_dmac_hardware_semantics(NID_SCE_DMAC_TRY_MEMCPY, "sceDmacTryMemcpy");
}

static void test_display_framebuf_latch(void) {
    reset_fixture();
    sr_hle_init();

    const uint32_t out_addr = 0x09000000u;
    const uint32_t out_stride = out_addr + 4u;
    const uint32_t out_fmt = out_addr + 8u;
    expect(display_set(0x04000000u, 256, 1, 0) == 0x80000107u,
           "SetFrameBuf immediate rejects a non-latched first-call stride/format");
    uint32_t ret = display_set(0x04000000u, 512, 3, 0);
    expect(ret == 0u, "SetFrameBuf immediate accepts a normal VRAM buffer");
    ret = display_get(out_addr, out_stride, out_fmt, 0);
    expect(ret == 0u && MEM_R32(out_addr) == 0x04000000u &&
           MEM_R32(out_stride) == 512u && MEM_R32(out_fmt) == 3u,
           "GetFrameBuf immediate returns all active fields");

    expect(display_set(0x04000000u, 512, 1, 0) == 0x80000107u,
           "SetFrameBuf immediate rejects a format change before latching");
    expect(display_set(0x04000000u, 512, 1, 1) == 0u,
           "SetFrameBuf latches a format change");
    expect(display_set(0x04000000u, 512, 1, 0) == 0u,
           "SetFrameBuf immediate accepts the latched format");
    expect(display_set(0x04000000u, 512, 3, 1) == 0u &&
           display_set(0x04000000u, 512, 3, 0) == 0u,
           "SetFrameBuf restores the normal scanout format");

    ret = display_set(0x04088000u, 256, 1, 1);
    expect(ret == 0u, "SetFrameBuf next-frame accepts a valid pending buffer");
    ret = display_get(out_addr, out_stride, out_fmt, 1);
    expect(ret == 0u && MEM_R32(out_addr) == 0x04088000u &&
           MEM_R32(out_stride) == 256u && MEM_R32(out_fmt) == 1u,
           "GetFrameBuf latched returns the complete pending state");
    ret = display_set(0x04100000u, 256, 1, 1);
    expect(ret == 0u, "SetFrameBuf overwrites a pending next-frame request");
    ret = display_get(out_addr, out_stride, out_fmt, 1);
    expect(ret == 0u && MEM_R32(out_addr) == 0x04100000u &&
           MEM_R32(out_stride) == 256u && MEM_R32(out_fmt) == 1u,
           "GetFrameBuf latched returns the most recent pending request");
    ret = display_get(out_addr, out_stride, out_fmt, 0);
    expect(ret == 0u && MEM_R32(out_addr) == 0x04000000u &&
           MEM_R32(out_stride) == 256u && MEM_R32(out_fmt) == 1u,
           "GetFrameBuf immediate keeps the address active before VBLANK");

    sr_vblank_tick();
    ret = display_get(out_addr, out_stride, out_fmt, 0);
    expect(ret == 0u && MEM_R32(out_addr) == 0x04100000u &&
           MEM_R32(out_stride) == 256u && MEM_R32(out_fmt) == 1u,
           "VBLANK applies the most recent pending address to active scanout");

    expect(display_set(0x04088000u, -64, 0, 1) == 0u,
           "SetFrameBuf accepts the PSP negative-stride contract");
    ret = display_get(out_addr, out_stride, out_fmt, 1);
    expect(ret == 0u && (int32_t)MEM_R32(out_stride) == -64 &&
           MEM_R32(out_fmt) == 0u,
           "GetFrameBuf preserves a latched negative stride");
    expect(display_set(0x04088000u, -1, 0, 1) == 0x80000104u,
           "SetFrameBuf rejects an unaligned negative stride");
    expect(display_set(0, 0, 0, 1) == 0u &&
           display_set(0, 0, 0, 0) == 0u,
           "SetFrameBuf supports display-off with zero stride");
    expect(display_set(0, 100, 3, 1) == 0x80000104u,
           "SetFrameBuf rejects a nonzero stride when the display is off");
    expect(display_set(0x44088000u, 512, 3, 1) == 0u &&
           display_set(0x44088000u, 512, 3, 0) == 0u,
           "SetFrameBuf accepts the uncached VRAM alias");

    expect(display_set(0x04000000u, 512, 3, 2) == 0x80000107u,
           "SetFrameBuf rejects an invalid sync mode");
    expect(display_set(0x04000000u, 512, 4, 0) == 0x80000108u,
           "SetFrameBuf rejects an invalid pixel format");
    expect(display_set(0x00100000u, 512, 4, 0) == 0x80000103u,
           "SetFrameBuf reports an invalid address before an invalid format");
    expect(display_set(0x04000000u, 100, 4, 0) == 0x80000104u,
           "SetFrameBuf reports an invalid stride before an invalid format");
    expect(display_set(0x04000000u, 0, 3, 0) == 0x80000104u,
           "SetFrameBuf rejects zero stride for an enabled display");
    expect(display_set(0x04000004u, 512, 3, 0) == 0x80000103u,
           "SetFrameBuf rejects a misaligned address");
    expect(display_set(0x00100000u, 512, 3, 0) == 0x80000103u,
           "SetFrameBuf rejects scratchpad addresses");
    expect(display_get(0x0c000000u, 0, 0, 0) == 0x80000103u,
           "GetFrameBuf rejects an output pointer outside guest memory");
    expect(display_get(0, 0, 0, 2) == 0x80000107u,
           "GetFrameBuf rejects an invalid latch selector");
}

/* ---- message-pipe safety (issue #178) --------------------------------------------------
 *
 * Executable proof of the message-pipe resource/safety contract against the
 * production HLE handlers through the registered-NID sr_syscall path:
 *   - a guest-controlled bufferSize outside the documented PSP-resource model
 *     is rejected BEFORE any host allocation, UID hand-out, or slot use;
 *   - TrySend/TryReceive preflight the complete guest source/destination span
 *     and the resultSize span before mutating FIFO state, so an invalid
 *     pointer can never partially perform a transfer;
 *   - the explicit FIFO invariants (count <= capacity, read_pos < capacity,
 *     write_pos < capacity) hold under send/receive churn.
 * The white-box state probe below is compiled into this executable only. */
static void msgpipe_setup(CpuState *cpu, uint32_t uid, uint32_t buf, uint32_t size,
                          uint32_t wait_mode, uint32_t resultp) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->r[4] = uid;
    cpu->r[5] = buf;
    cpu->r[6] = size;
    cpu->r[7] = wait_mode;
    cpu->r[8] = resultp; /* t0: resultSize */
}

/* ---- issue #32: streamed ATRAC3+ ring wrap keeps logical frame order ----
 *
 * The title BGM is a streamed ATRAC3+ track: the guest hands sceAtracSetData a
 * ring smaller than the file, then repeatedly asks sceAtracGetStreamDataInfo
 * where to write, copies the next file bytes there, and calls
 * sceAtracAddStreamData, while sceAtracDecodeData consumes one frame per call.
 *
 * The first physical lap begins after the RIFF header, but that header is a
 * one-time prefix. Once the read cursor reaches the frame-aligned end, the
 * next lap uses the whole physical buffer (base zero). The title's frame 87
 * is the important boundary: 580 bytes remain at the old physical tail and
 * the next refill begins at offset 580, so one encoded frame is split across
 * the old end and the new write span. A decoder that insists on a single
 * guest-contiguous read either rereads the RIFF prefix or feeds malformed
 * bytes to FFmpeg.
 *
 * This fixture is fully synthetic: the frames are zero-padded ATRAC3+
 * terminator units (0x60), which the production decoder accepts at any
 * blockAlign and decodes to 2048 silent samples. No game data is involved.
 * dataByteOffset (56) is deliberately NOT a multiple of blockAlign (24), and
 * the synthetic buffer leaves a 16-byte residual at the first-lap end. */
extern int sr_hle_test_atrac_ring(uint32_t id, uint32_t *pos, uint32_t *base,
                                  uint32_t *end, uint32_t *frame, uint32_t *valid);

#define AT_ALIGN    24u                          /* blockAlign (bytes/frame) */
#define AT_DATAOFF  56u                          /* 'data' payload offset    */
#define AT_RINGFR   10u                          /* frames the ring holds    */
#define AT_TAIL     16u                          /* residual before first wrap */
#define AT_FILEFR   40u                          /* frames in the whole file */
#define AT_BUFSIZE  (AT_DATAOFF + AT_RINGFR * AT_ALIGN + AT_TAIL)
#define AT_FILESIZE (AT_DATAOFF + AT_FILEFR * AT_ALIGN)
#define AT_RINGBASE 0x08001000u
#define AT_PCMOUT   0x08010000u

/* Byte at `off` of the synthetic file: a terminator unit at every frame start,
 * zero padding elsewhere. */
static uint8_t atring_file_byte(uint32_t off) {
    if (off < AT_DATAOFF) return 0;
    return ((off - AT_DATAOFF) % AT_ALIGN) == 0u ? 0x60u : 0x00u;
}

static void atring_build_header(void) {
    uint8_t *h = &g_mem[AT_RINGBASE - 0x08000000u];
    memset(h, 0, AT_BUFSIZE);
    memcpy(h + 0, "RIFF", 4);
    fixture_wr32(h + 4, AT_FILESIZE - 8u);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    fixture_wr32(h + 16, 16u);
    fixture_wr16(h + 20, 0xFFFEu);               /* WAVE_FORMAT_EXTENSIBLE   */
    fixture_wr16(h + 22, 2u);                    /* channels                 */
    fixture_wr32(h + 24, 44100u);
    fixture_wr32(h + 28, 44100u * 4u);
    fixture_wr16(h + 32, (uint16_t)AT_ALIGN);    /* blockAlign               */
    fixture_wr16(h + 34, 16u);
    memcpy(h + 36, "fact", 4);
    fixture_wr32(h + 40, 4u);
    fixture_wr32(h + 44, AT_FILEFR * 2048u);     /* total samples            */
    memcpy(h + 48, "data", 4);
    fixture_wr32(h + 52, AT_FILESIZE - AT_DATAOFF);
    for (uint32_t off = AT_DATAOFF; off < AT_BUFSIZE; off++)
        h[off] = atring_file_byte(off);         /* initial file prefix      */
}

static void test_atrac_stream_ring_wrap(void) {
    reset_fixture();
    sr_hle_init();

    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    atring_build_header();

    cpu.r[4] = AT_RINGBASE;
    cpu.r[5] = AT_BUFSIZE;
    uint32_t id = sr_syscall(&cpu, NID_SCE_ATRAC_SET_DATA_AND_GET_ID);
    expect(id < 8u, "streamed ATRAC3+ track accepted by sceAtracSetDataAndGetID");
    if (id >= 8u) return;

    uint32_t pos = 0, base = 0, end = 0, frame = 0, valid = 0;
    expect(sr_hle_test_atrac_ring(id, &pos, &base, &end, &frame, &valid) == 1,
           "ring geometry readable for the tracked context");
    expect(frame == AT_ALIGN, "blockAlign parsed from the synthetic fmt chunk");
    expect(base == AT_DATAOFF, "streamed ring starts at the 'data' payload, not at buffer offset 0");
    /* The first lap is frame aligned after its one-time header prefix. */
    expect(end > base && ((end - base) % AT_ALIGN) == 0u,
           "streamed ring spans an exact number of frames");
    expect(pos == AT_DATAOFF + AT_ALIGN,
           "first decode starts after the state-loading frame");

    /* Consume the initial nine complete frames. The sixteenth byte tail is
     * deliberately left queued so the next frame straddles the first wrap. */
    const uint32_t WP = 0x08000200u, WB = 0x08000204u, RO = 0x08000208u;
    const uint32_t DEC = 0x08000210u, FIN = 0x08000214u, REM = 0x08000218u;
    int wrapped = 0, split_refill = 0, decoded_frames = 0, decode_failures = 0;

    for (int i = 0; i < 9; i++) {
        cpu.r[4] = id; cpu.r[5] = AT_PCMOUT; cpu.r[6] = DEC; cpu.r[7] = FIN;
        cpu.r[8] = REM;
        uint32_t ret = sr_syscall(&cpu, 0x6a8c3cd5u);
        if (ret != 0u) decode_failures++;
        else if (MEM_R32(DEC) == 2048u) decoded_frames++;
    }

    uint32_t after_initial_pos = 0, after_initial_base = 0, after_initial_valid = 0;
    expect(sr_hle_test_atrac_ring(id, &after_initial_pos, &after_initial_base,
                                   NULL, NULL, &after_initial_valid) == 1,
           "ring readable after the initial complete frames");
    wrapped = after_initial_base == 0u && after_initial_pos == 0u;
    expect(wrapped, "first lap switches the physical base to zero");
    expect(after_initial_valid == AT_TAIL,
           "the residual tail remains queued across the physical wrap");

    /* The next frame is split: 16 bytes remain at the old tail and eight new
     * bytes are written at physical offset 16. This is production dispatch,
     * not a direct helper call. */
    cpu.r[4] = id; cpu.r[5] = WP; cpu.r[6] = WB; cpu.r[7] = RO;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_STREAM_DATA_INFO) == 0,
           "sceAtracGetStreamDataInfo exposes the split-frame write span");
    uint32_t wp = MEM_R32(WP), wb = MEM_R32(WB), ro = MEM_R32(RO);
    expect(wp == AT_RINGBASE + AT_TAIL && ro == AT_BUFSIZE,
           "split refill uses physical tail offset and logical file end");
    expect(wb >= AT_ALIGN - AT_TAIL,
           "split refill reports enough writable bytes for the next frame");
    for (uint32_t b = 0; b < AT_ALIGN - AT_TAIL; b++)
        MEM_W8(wp + b, atring_file_byte(ro + b));
    cpu.r[4] = id; cpu.r[5] = AT_ALIGN - AT_TAIL; cpu.r[6] = 0;
    expect(sr_syscall(&cpu, 0x7db31251u) == 0,
           "sceAtracAddStreamData accepts the partial frame completion");
    split_refill = 1;

    cpu.r[4] = id; cpu.r[5] = AT_PCMOUT; cpu.r[6] = DEC; cpu.r[7] = FIN;
    cpu.r[8] = REM;
    uint32_t split_ret = sr_syscall(&cpu, 0x6a8c3cd5u);
    if (split_ret != 0u) decode_failures++;
    else if (MEM_R32(DEC) == 2048u) decoded_frames++;

    /* Continue through ordinary post-wrap refills so the queue is exercised
     * after the boundary, not only for one special frame. */
    for (int i = 0; i < 15; i++) {
        cpu.r[4] = id; cpu.r[5] = WP; cpu.r[6] = WB; cpu.r[7] = RO;
        expect(sr_syscall(&cpu, NID_SCE_ATRAC_GET_STREAM_DATA_INFO) == 0,
               "sceAtracGetStreamDataInfo succeeds after the split frame");
        wp = MEM_R32(WP); wb = MEM_R32(WB); ro = MEM_R32(RO);
        expect(wb >= AT_ALIGN && ro < AT_FILESIZE,
               "post-wrap refill exposes one complete frame of writable space");
        for (uint32_t b = 0; b < AT_ALIGN; b++)
            MEM_W8(wp + b, atring_file_byte(ro + b));
        cpu.r[4] = id; cpu.r[5] = AT_ALIGN; cpu.r[6] = 0;
        expect(sr_syscall(&cpu, 0x7db31251u) == 0,
               "sceAtracAddStreamData accepts post-wrap frame data");

        cpu.r[4] = id; cpu.r[5] = AT_PCMOUT; cpu.r[6] = DEC; cpu.r[7] = FIN;
        cpu.r[8] = REM;
        uint32_t ret = sr_syscall(&cpu, 0x6a8c3cd5u);
        if (ret != 0u) decode_failures++;
        else if (MEM_R32(DEC) == 2048u) decoded_frames++;

        uint32_t now = 0;
        (void)sr_hle_test_atrac_ring(id, &now, NULL, NULL, NULL, NULL);
    }

    expect(split_refill == 1, "the fixture exercised a physical split-frame refill");
    expect(decode_failures == 0,
           "no streamed frame is rejected by the decoder across a ring wrap");
    expect(decoded_frames == 25,
           "every sceAtracDecodeData call across the wrap returns a full 2048-sample frame");

    expect(sr_hle_test_atrac_ring(id, &pos, &base, NULL, NULL, NULL) == 1 &&
           ((pos - base) % AT_ALIGN) == 0u,
           "the physical cursor remains frame-aligned after the split boundary");

    cpu.r[4] = id;
    expect(sr_syscall(&cpu, NID_SCE_ATRAC_RELEASE_ID) == 0,
           "streamed context releases cleanly");
}

/* Production-dispatch regression for the BGM/SFX mix junction (#32, #75).
 *
 * The title routes music as: sceAtracDecodeData writes PCM, the game copies it
 * into the SAS output buffer, then __sceSasCoreWithMix adds VAG sound effects on
 * top, and the result goes to sceAudioOutput2OutputBlocking. The load-bearing
 * property of that junction is that WithMix ADDS to whatever the caller already
 * placed in the buffer while plain Core OVERWRITES it. If WithMix ever became an
 * overwrite, every frame of title BGM would be silently erased at the last step
 * before the audio device, with no error anywhere -- exactly the "overwritten"
 * failure mode that is hardest to see in a running route.
 *
 * Everything here is synthetic: a generated PCM ramp stands in for decoded audio
 * and a hand-built 4-block VAG stream stands in for a sound effect. No retail
 * bytes, no decoder, no audio device -- so this runs anywhere the harness does.
 */
#define NID_SAS_INIT           0x42778a9fu
#define NID_SAS_CORE           0xa3589d81u
#define NID_SAS_CORE_WITH_MIX  0x50a14dfcu
#define NID_SAS_SET_VOICE      0x99944089u
#define NID_SAS_SET_VOLUME     0x440ca7d8u
#define NID_SAS_SET_KEY_ON     0x76f01acau
#define NID_SAS_SET_KEY_OFF    0xa0cf2fa4u
#define NID_SAS_GET_END        0x68a46b95u
#define NID_SAS_SET_ADSR       0x019b25ebu
#define NID_SAS_SET_ADSR_MODE  0x9ec3676au
#define NID_SAS_SET_SIMPLE     0xcbcd4f79u
#define NID_SAS_SET_NOISE      0xb7660a23u
#define NID_SAS_REV_TYPE       0x33d4ab37u
#define SAS_ERROR_ADDRESS      0x80420005u
#define SAS_ERROR_VOICE_INDEX  0x80420010u
#define SAS_ERROR_ADSR_MODE    0x80420013u

#define SAS_TEST_GRAIN 64u

/* Interleaved stereo s16 helpers over the guest arena. */
static void sas_write_pcm(uint32_t addr, const int16_t *lr, uint32_t frames) {
    for (uint32_t i = 0; i < frames * 2u; i++)
        MEM_W16(addr + i * 2u, (uint16_t)lr[i]);
}
static void sas_read_pcm(uint32_t addr, int16_t *lr, uint32_t frames) {
    for (uint32_t i = 0; i < frames * 2u; i++)
        lr[i] = (int16_t)MEM_R16(addr + i * 2u);
}
static int16_t sas_clamp16(int32_t v) {
    return v < -32768 ? (int16_t)-32768 : v > 32767 ? (int16_t)32767 : (int16_t)v;
}

static void test_sas_core_mix_preserves_caller_pcm(void) {
    reset_fixture();
    sr_hle_init();

    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[29] = 0x09012000u;

    const uint32_t SAS_OUT = 0x08010000u;   /* SAS output buffer */
    const uint32_t SAS_VAG = 0x08020000u;   /* synthetic VAG stream */
    const uint32_t SAS_CORE = 0x08030000u;   /* aligned SceSasCore span */

    /* __sceSasInit(core, grain, maxVoices, outMode, sampleRate) */
    cpu.r[4] = SAS_CORE; cpu.r[5] = SAS_TEST_GRAIN; cpu.r[6] = 32; cpu.r[7] = 0;
    cpu.r[8] = 44100;
    expect(sr_syscall(&cpu, NID_SAS_INIT) == 0,
           "__sceSasInit accepts the grain/voice configuration");

    /* Stand-in for one grain of decoded BGM: a deterministic ramp with both
     * signs, distinct per channel so an L/R swap would also be caught. */
    int16_t bgm[SAS_TEST_GRAIN * 2], got[SAS_TEST_GRAIN * 2], voice_only[SAS_TEST_GRAIN * 2];
    for (uint32_t i = 0; i < SAS_TEST_GRAIN; i++) {
        bgm[i * 2 + 0] = (int16_t)(1000 + (int)i * 7);
        bgm[i * 2 + 1] = (int16_t)(-800 - (int)i * 5);
    }

    /* --- 1. WithMix with no voice keyed on must not disturb the caller's PCM. --- */
    sas_write_pcm(SAS_OUT, bgm, SAS_TEST_GRAIN);
    cpu.r[4] = SAS_CORE; cpu.r[5] = SAS_OUT; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000;
    expect(sr_syscall(&cpu, NID_SAS_CORE_WITH_MIX) == 0, "__sceSasCoreWithMix returns success");
    sas_read_pcm(SAS_OUT, got, SAS_TEST_GRAIN);
    expect(memcmp(got, bgm, sizeof(bgm)) == 0,
           "__sceSasCoreWithMix with no active voice leaves caller PCM bit-identical");

    /* --- 2. Plain Core is an overwrite: the same buffer must come back silent. --- */
    sas_write_pcm(SAS_OUT, bgm, SAS_TEST_GRAIN);
    cpu.r[4] = SAS_CORE; cpu.r[5] = SAS_OUT;
    expect(sr_syscall(&cpu, NID_SAS_CORE) == 0, "__sceSasCore returns success");
    sas_read_pcm(SAS_OUT, got, SAS_TEST_GRAIN);
    int core_silent = 1;
    for (uint32_t i = 0; i < SAS_TEST_GRAIN * 2u; i++) if (got[i]) { core_silent = 0; break; }
    expect(core_silent,
           "__sceSasCore with no active voice overwrites the buffer with silence");

    /* --- 3. A keyed-on voice must actually contribute samples. ---
     * Four 16-byte SAS_VAG blocks (predictor 0, shift 0, no loop/end flags), which
     * decode to nibble<<12 with no filter history, so the stream is bounded and
     * deterministic without embedding anything game-derived. */
    for (uint32_t b = 0; b < 4u; b++) {
        uint32_t a = SAS_VAG + b * 16u;
        MEM_W8(a, 0u);          /* predictor 0, shift 0 */
        MEM_W8(a + 1u, 0u);     /* no loop-start / loop-end / end marker */
        for (uint32_t k = 0; k < 14u; k++)
            MEM_W8(a + 2u + k, (uint8_t)(0x11u * ((b + k) % 7u + 1u)));
    }
    /* __sceSasSetVoice(core, voice, vagAddr, size, loopmode) -- loopmode is the
     * fifth argument, which stack_arg(0) reads from $t0. */
    cpu.r[4] = SAS_CORE; cpu.r[5] = 0; cpu.r[6] = SAS_VAG; cpu.r[7] = 64u; cpu.r[8] = 0;
    expect(sr_syscall(&cpu, NID_SAS_SET_VOICE) == 0, "__sceSasSetVoice accepts the stream");
    cpu.r[4] = SAS_CORE; cpu.r[5] = 0; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000;
    cpu.r[8] = 0x1000; cpu.r[9] = 0x1000;
    expect(sr_syscall(&cpu, NID_SAS_SET_VOLUME) == 0, "__sceSasSetVolume accepts full volume");
    cpu.r[4] = SAS_CORE; cpu.r[5] = 0;
    expect(sr_syscall(&cpu, NID_SAS_SET_KEY_ON) == 0, "__sceSasSetKeyOn keys the voice on");

    /* Voice-only reference: Core over a silent buffer. */
    memset(got, 0, sizeof(got));
    sas_write_pcm(SAS_OUT, got, SAS_TEST_GRAIN);
    cpu.r[4] = SAS_CORE; cpu.r[5] = SAS_OUT;
    (void)sr_syscall(&cpu, NID_SAS_CORE);
    sas_read_pcm(SAS_OUT, voice_only, SAS_TEST_GRAIN);
    int voice_audible = 0;
    for (uint32_t i = 0; i < SAS_TEST_GRAIN * 2u; i++) if (voice_only[i]) { voice_audible = 1; break; }
    expect(voice_audible, "__sceSasCore mixes a keyed-on SAS_VAG voice to nonzero output");

    /* --- 4. WithMix over BGM must equal the saturating sum of the two. ---
     * KeyOn resets position, filter history, envelope and resample phase, so the
     * voice replays identically and the expected buffer is exact. */
    cpu.r[4] = SAS_CORE; cpu.r[5] = 0;
    (void)sr_syscall(&cpu, NID_SAS_SET_KEY_ON);
    sas_write_pcm(SAS_OUT, bgm, SAS_TEST_GRAIN);
    cpu.r[4] = SAS_CORE; cpu.r[5] = SAS_OUT; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000;
    (void)sr_syscall(&cpu, NID_SAS_CORE_WITH_MIX);
    sas_read_pcm(SAS_OUT, got, SAS_TEST_GRAIN);

    int sum_exact = 1, differs_from_bgm = 0, differs_from_voice = 0;
    for (uint32_t i = 0; i < SAS_TEST_GRAIN * 2u; i++) {
        if (got[i] != sas_clamp16((int32_t)bgm[i] + (int32_t)voice_only[i])) sum_exact = 0;
        if (got[i] != bgm[i]) differs_from_bgm = 1;
        if (got[i] != voice_only[i]) differs_from_voice = 1;
    }
    expect(sum_exact,
           "__sceSasCoreWithMix adds the voice to caller PCM sample-for-sample");
    expect(differs_from_bgm && differs_from_voice,
           "__sceSasCoreWithMix output is neither the caller PCM nor the voice alone");
}

/* Production-dispatch SAS state regressions.  Each case uses only synthetic
 * guest data and starts from a fresh core so one voice cannot mask another. */
static void sas_test_init(CpuState *cpu, uint32_t core, uint32_t output_mode) {
    sr_hle_test_sas_reset();
    memset(cpu, 0, sizeof(*cpu));
    cpu->r[29] = 0x09012000u;
    cpu->r[4] = core; cpu->r[5] = SAS_TEST_GRAIN; cpu->r[6] = 32;
    cpu->r[7] = output_mode; cpu->r[8] = 44100;
    expect(sr_syscall(cpu, NID_SAS_INIT) == 0,
           "__sceSasInit accepts the synthetic state fixture");
}

static void sas_test_vag(uint32_t addr, uint32_t blocks, uint8_t flags) {
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t a = addr + b * 16u;
        MEM_W8(a, 0u); MEM_W8(a + 1u, b + 1u == blocks ? flags : 0u);
        for (uint32_t k = 0; k < 14u; k++) MEM_W8(a + 2u + k, 0x11u);
    }
}

static void test_sas_state_contracts(void) {
    CpuState cpu;
#ifdef CORE
#undef CORE
#endif
#ifdef OUT
#undef OUT
#endif
#ifdef VAG
#undef VAG
#endif
    const uint32_t CORE = 0x08030000u;
    const uint32_t OUT = 0x08010000u;
    const uint32_t VAG = 0x08020000u;

    /* RevType has core/effect arguments, not SetNoise's voice/frequency pair.
     * Configure the voice after the call: the old misroute left noise latched
     * on that voice, so its first sample was no longer the VAG sample. */
    reset_fixture(); sr_hle_init(); sas_test_init(&cpu, CORE, 0);
    sas_test_vag(VAG, 4u, 0u);
    cpu.r[4] = CORE; cpu.r[5] = 3; cpu.r[6] = VAG; cpu.r[7] = 64; cpu.r[8] = 0;
    expect(sr_syscall(&cpu, NID_SAS_SET_VOICE) == 0,
           "RevType fixture accepts the VAG voice");
    cpu.r[4] = CORE; cpu.r[5] = 3; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000;
    cpu.r[8] = 0x1000; cpu.r[9] = 0x1000; (void)sr_syscall(&cpu, NID_SAS_SET_VOLUME);
    cpu.r[4] = CORE; cpu.r[5] = 3; cpu.r[6] = 3; cpu.r[7] = 17;
    expect(sr_syscall(&cpu, NID_SAS_REV_TYPE) == 0,
           "__sceSasRevType updates effect state");
    cpu.r[4] = CORE; cpu.r[5] = 3; (void)sr_syscall(&cpu, NID_SAS_SET_KEY_ON);
    memset(g_mem + (OUT - 0x08000000u), 0, SAS_TEST_GRAIN * 4u);
    cpu.r[4] = CORE; cpu.r[5] = OUT; (void)sr_syscall(&cpu, NID_SAS_CORE);
    expect((int16_t)MEM_R16(OUT) == 64,
           "__sceSasRevType does not mutate the selected voice's source type");

    /* SetNoise is independent of VAG/PCM source state and can be keyed on. */
    reset_fixture(); sr_hle_init(); sas_test_init(&cpu, CORE, 0);
    cpu.r[4] = CORE; cpu.r[5] = 2; cpu.r[6] = 17;
    expect(sr_syscall(&cpu, NID_SAS_SET_NOISE) == 0,
           "__sceSasSetNoise accepts a noise-only voice");
    cpu.r[4] = CORE; cpu.r[5] = 2; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000;
    cpu.r[8] = 0x1000; cpu.r[9] = 0x1000; (void)sr_syscall(&cpu, NID_SAS_SET_VOLUME);
    cpu.r[4] = CORE; cpu.r[5] = 2;
    expect(sr_syscall(&cpu, NID_SAS_SET_KEY_ON) == 0,
           "noise voice keys on without a VAG source");
    memset(g_mem + (OUT - 0x08000000u), 0, SAS_TEST_GRAIN * 4u);
    cpu.r[4] = CORE; cpu.r[5] = OUT; (void)sr_syscall(&cpu, NID_SAS_CORE);
    int noise_audible = 0;
    for (uint32_t i = 0; i < SAS_TEST_GRAIN * 2u; i++)
        if ((int16_t)MEM_R16(OUT + i * 2u) != 0) { noise_audible = 1; break; }
    expect(noise_audible, "noise voice contributes samples through Core");

    /* ADSR rates and curves use independent masks/argument positions.  A high
     * attack rate makes the selected-mode regression directly observable in the
     * first synthetic sample; the former handlers reduced both calls to small
     * voice/envelope aliases and produced silence. */
    reset_fixture(); sr_hle_init(); sas_test_init(&cpu, CORE, 0);
    cpu.r[4] = CORE; cpu.r[5] = 1; cpu.r[6] = 1; cpu.r[7] = 0x10000000u;
    cpu.r[8] = 0; cpu.r[9] = 0; cpu.r[10] = 0;
    expect(sr_syscall(&cpu, NID_SAS_SET_ADSR) == 0,
           "__sceSasSetADSR updates the selected attack rate");
    cpu.r[4] = CORE; cpu.r[5] = 1; cpu.r[6] = 1; cpu.r[7] = 5;
    cpu.r[8] = 5; cpu.r[9] = 5; cpu.r[10] = 5;
    expect(sr_syscall(&cpu, NID_SAS_SET_ADSR_MODE) == 0,
           "__sceSasSetADSRmode updates the selected curve only");
    sas_test_vag(VAG, 4u, 0u);
    cpu.r[4] = CORE; cpu.r[5] = 1; cpu.r[6] = VAG; cpu.r[7] = 64; cpu.r[8] = 0;
    (void)sr_syscall(&cpu, NID_SAS_SET_VOICE);
    cpu.r[4] = CORE; cpu.r[5] = 1; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000;
    cpu.r[8] = 0x1000; cpu.r[9] = 0x1000; (void)sr_syscall(&cpu, NID_SAS_SET_VOLUME);
    cpu.r[4] = CORE; cpu.r[5] = 1; (void)sr_syscall(&cpu, NID_SAS_SET_KEY_ON);
    memset(g_mem + (OUT - 0x08000000u), 0, SAS_TEST_GRAIN * 4u);
    cpu.r[4] = CORE; cpu.r[5] = OUT; (void)sr_syscall(&cpu, NID_SAS_CORE);
    expect((int16_t)MEM_R16(OUT) != 0,
           "ADSRmode does not overwrite the selected voice's rate state");

    reset_fixture(); sr_hle_init(); sas_test_init(&cpu, CORE, 0);
    cpu.r[4] = CORE; cpu.r[5] = 1; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000;
    expect(sr_syscall(&cpu, NID_SAS_SET_SIMPLE) == 0,
           "__sceSasSetSimpleADSR accepts its envelope words");
    cpu.r[4] = CORE; cpu.r[5] = 1; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000u | (1u << 13);
    expect(sr_syscall(&cpu, NID_SAS_SET_SIMPLE) == SAS_ERROR_ADSR_MODE,
           "__sceSasSetSimpleADSR rejects an invalid envelope mode");

    /* Invalid -1 must not alias voice 31.  Mutate the active voice only if the
     * implementation still contains the historical A1 & 31 access. */
    reset_fixture(); sr_hle_init(); sas_test_init(&cpu, CORE, 0);
    sas_test_vag(VAG, 4u, 0u);
    cpu.r[4] = CORE; cpu.r[5] = 31; cpu.r[6] = VAG; cpu.r[7] = 64; cpu.r[8] = 0;
    (void)sr_syscall(&cpu, NID_SAS_SET_VOICE);
    cpu.r[4] = CORE; cpu.r[5] = 31; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000;
    cpu.r[8] = 0x1000; cpu.r[9] = 0x1000; (void)sr_syscall(&cpu, NID_SAS_SET_VOLUME);
    cpu.r[4] = CORE; cpu.r[5] = 31; (void)sr_syscall(&cpu, NID_SAS_SET_KEY_ON);
    cpu.r[4] = CORE; cpu.r[5] = 0xffffffffu; cpu.r[6] = 0; cpu.r[7] = 0;
    cpu.r[8] = 0; cpu.r[9] = 0;
    expect(sr_syscall(&cpu, NID_SAS_SET_VOLUME) == SAS_ERROR_VOICE_INDEX,
           "negative voice index is rejected rather than wrapped");
    memset(g_mem + (OUT - 0x08000000u), 0, SAS_TEST_GRAIN * 4u);
    cpu.r[4] = CORE; cpu.r[5] = OUT; (void)sr_syscall(&cpu, NID_SAS_CORE);
    expect((int16_t)MEM_R16(OUT) != 0,
           "rejecting an invalid voice leaves voice 31 audible");

    /* A multichannel grain needs the complete 4-plane span.  The old stereo
     * mixer wrote only the first 256 bytes and accepted this truncated target. */
    reset_fixture(); sr_hle_init(); sas_test_init(&cpu, CORE, 1);
    const uint32_t BAD_OUT = 0x0bffff00u;
    MEM_W32(OUT, 0xdeadbeefu);
    cpu.r[4] = CORE; cpu.r[5] = BAD_OUT;
    expect(sr_syscall(&cpu, NID_SAS_CORE) == SAS_ERROR_ADDRESS,
           "Core rejects an output pointer without a complete grain span");
    expect(MEM_R32(OUT) == 0xdeadbeefu,
           "rejected output span leaves unrelated guest memory untouched");

    /* Data loop markers cannot override a caller loop=0 request. */
    reset_fixture(); sr_hle_init(); sas_test_init(&cpu, CORE, 0);
    sas_test_vag(VAG, 3u, 7u);
    MEM_W8(VAG + 1u, 6u); MEM_W8(VAG + 16u + 1u, 3u);
    cpu.r[4] = CORE; cpu.r[5] = 5; cpu.r[6] = VAG; cpu.r[7] = 48; cpu.r[8] = 0;
    (void)sr_syscall(&cpu, NID_SAS_SET_VOICE);
    cpu.r[4] = CORE; cpu.r[5] = 5; cpu.r[6] = 0x1000; cpu.r[7] = 0x1000;
    cpu.r[8] = 0x1000; cpu.r[9] = 0x1000; (void)sr_syscall(&cpu, NID_SAS_SET_VOLUME);
    cpu.r[4] = CORE; cpu.r[5] = 5; (void)sr_syscall(&cpu, NID_SAS_SET_KEY_ON);
    memset(g_mem + (OUT - 0x08000000u), 0, SAS_TEST_GRAIN * 4u);
    cpu.r[4] = CORE; cpu.r[5] = OUT; (void)sr_syscall(&cpu, NID_SAS_CORE);
    cpu.r[4] = CORE;
    expect(sr_syscall(&cpu, NID_SAS_GET_END) & (1u << 5),
           "a non-looping VAG voice ends despite loop markers");
}

static void test_msgpipe_safety(void) {
    reset_fixture();
    sr_hle_init();

    CpuState cpu;
    /* Guest buffers live inside the 0x0c000000 arena (g_mem maps 0x08000000). */
    const uint32_t GUEST_BUF  = 0x08010000u;
    const uint32_t GUEST_OUT  = 0x08020000u;
    const uint32_t GUEST_NAME = 0x08030000u;
    const uint32_t GUEST_RES  = 0x08040000u; /* distinct resultSize slot */
    const uint32_t ARENA_END  = 0x0c000000u;
    g_mem[0x010000] = 'p'; g_mem[0x010001] = '0'; g_mem[0x010002] = 0;   /* pipe name */

    uint32_t max_cap = sr_hle_test_msgpipe_max_capacity();
    expect(max_cap > 0u && max_cap <= 0x1000000u,
           "message-pipe capacity ceiling is a sane bounded constant");

    /* --- CreateMsgPipe: resource-model validation before any allocation --- */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = GUEST_NAME;
    cpu.r[7] = 0u; /* bufferSize 0 -> illegal */
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_CREATE_MSG_PIPE) == SCE_KERNEL_ERROR_ILLEGAL_SIZE,
           "CreateMsgPipe rejects bufferSize 0");

    cpu.r[7] = max_cap + 1u; /* first size above the model ceiling */
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_CREATE_MSG_PIPE) == SCE_KERNEL_ERROR_ILLEGAL_SIZE,
           "CreateMsgPipe rejects bufferSize above the capacity ceiling (bounded time, no alloc)");

    cpu.r[7] = 0xFFFFFFFFu; /* maximal hostile request */
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_CREATE_MSG_PIPE) == SCE_KERNEL_ERROR_ILLEGAL_SIZE,
           "CreateMsgPipe rejects a 4 GiB hostile request before allocation");

    /* A rejected create must not consume a UID or slot: a follow-up valid
     * create must succeed and be the FIRST pipe (state probe finds it). */
    cpu.r[7] = 64u;
    uint32_t uid = sr_syscall(&cpu, NID_SCE_KERNEL_CREATE_MSG_PIPE);
    expect(uid != 0u && uid < 0x80000000u, "valid CreateMsgPipe returns a kernel UID");
    SrMsgPipeState st;
    expect(sr_hle_test_msgpipe_state(uid, &st) == 1, "state probe finds the created pipe");
    expect(st.capacity == 64u && st.count == 0u, "pipe starts empty at the requested capacity");

    /* --- TrySend: full source-span preflight, no partial mutation --- */
    for (uint32_t i = 0; i < 8; i++) g_mem[0x010000 + i] = (uint8_t)(0xA0u + i); /* 8-byte source */

    /* Source span crossing the arena end: must be rejected with the FIFO untouched. */
    msgpipe_setup(&cpu, uid, ARENA_END - 3u, 8u, 1u, GUEST_OUT);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_SEND_MSG_PIPE) == SCE_KERNEL_ERROR_ILLEGAL_ADDR,
           "TrySend rejects a source span crossing the arena boundary");
    expect(sr_hle_test_msgpipe_state(uid, &st) == 1 && st.count == 0u && st.write_pos == 0u,
           "rejected send leaves count/write_pos untouched");
    expect(MEM_R32(GUEST_OUT) == 0u,
           "rejected send leaves resultSize zero");

    /* Source entirely outside the arena. */
    msgpipe_setup(&cpu, uid, 0xDEAD0000u, 4u, 1u, GUEST_OUT);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_SEND_MSG_PIPE) == SCE_KERNEL_ERROR_ILLEGAL_ADDR,
           "TrySend rejects a fully out-of-range source");

    /* Invalid resultSize span: must be rejected before any FIFO mutation. */
    msgpipe_setup(&cpu, uid, GUEST_BUF, 8u, 1u, ARENA_END - 1u);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_SEND_MSG_PIPE) == SCE_KERNEL_ERROR_ILLEGAL_ADDR,
           "TrySend rejects an out-of-range resultSize span");
    expect(sr_hle_test_msgpipe_state(uid, &st) == 1 && st.count == 0u,
           "resultSize rejection leaves the FIFO untouched");

    /* Valid send: exact byte transfer + resultSize + state. */
    msgpipe_setup(&cpu, uid, GUEST_BUF, 8u, 0u, GUEST_OUT);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_SEND_MSG_PIPE) == 0u, "valid TrySend succeeds");
    expect(sr_hle_test_msgpipe_state(uid, &st) == 1 && st.count == 8u,
           "send advances count by the transferred amount");
    expect(MEM_R32(GUEST_OUT) == 8u, "valid send reports transferred bytes in resultSize");

    /* --- TryReceive: full destination-span preflight, no partial drain --- */
    for (uint32_t i = 0; i < 8; i++) g_mem[0x020000 + i] = 0x55u; /* output buffer */
    msgpipe_setup(&cpu, uid, ARENA_END - 5u, 8u, 0u, GUEST_OUT);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_RECEIVE_MSG_PIPE) == SCE_KERNEL_ERROR_ILLEGAL_ADDR,
           "TryReceive rejects a destination span crossing the arena boundary");
    expect(sr_hle_test_msgpipe_state(uid, &st) == 1 && st.count == 8u && st.read_pos == 0u,
           "rejected receive leaves the pipe undrained");

    msgpipe_setup(&cpu, uid, GUEST_OUT, 8u, 0u, ARENA_END - 1u);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_RECEIVE_MSG_PIPE) == SCE_KERNEL_ERROR_ILLEGAL_ADDR,
           "TryReceive rejects an out-of-range resultSize span");
    expect(sr_hle_test_msgpipe_state(uid, &st) == 1 && st.count == 8u,
           "resultSize rejection leaves the pipe undrained");

    /* Valid receive: bytes come back in FIFO order.  resultSize uses a
     * distinct slot so the amount write cannot clobber the received payload. */
    msgpipe_setup(&cpu, uid, GUEST_OUT, 8u, 0u, GUEST_RES);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_RECEIVE_MSG_PIPE) == 0u, "valid TryReceive succeeds");
    expect(sr_hle_test_msgpipe_state(uid, &st) == 1 && st.count == 0u,
           "receive drains count to zero");
    expect(MEM_R32(GUEST_RES) == 8u, "valid receive reports transferred bytes");
    int bytes_match = 1;
    for (uint32_t i = 0; i < 8; i++) if (MEM_R8(GUEST_OUT + i) != (uint8_t)(0xA0u + i)) bytes_match = 0;
    expect(bytes_match, "received bytes match the sent FIFO content in order");

    /* --- Boundary behavior: full / empty --- */
    msgpipe_setup(&cpu, uid, GUEST_BUF, 64u, 0u, GUEST_OUT);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_SEND_MSG_PIPE) == 0u,
           "send that exactly fills the pipe succeeds");
    msgpipe_setup(&cpu, uid, GUEST_BUF, 1u, 0u, GUEST_OUT);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_SEND_MSG_PIPE) == SCE_KERNEL_ERROR_MPP_FULL,
           "send beyond a full pipe returns MPP_FULL");
    msgpipe_setup(&cpu, uid, GUEST_OUT, 64u, 0u, GUEST_OUT);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_RECEIVE_MSG_PIPE) == 0u,
           "receive draining the full pipe succeeds");
    msgpipe_setup(&cpu, uid, GUEST_OUT, 1u, 0u, GUEST_OUT);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_RECEIVE_MSG_PIPE) == SCE_KERNEL_ERROR_MPP_EMPTY,
           "receive on an empty pipe returns MPP_EMPTY");

    /* --- Churn: explicit invariants under mixed send/receive --- */
    int invariants_ok = 1;
    uint32_t cap = st.capacity; /* 64 */
    for (uint32_t phase = 0; phase < 200u && invariants_ok; phase++) {
        /* 1..cap: a 0-length request is ILLEGAL_SIZE by contract, so churn
         * stays on legal request sizes to exercise the FULL/EMPTY boundary. */
        uint32_t n = 1u + (phase * 7u + 3u) % cap;
        msgpipe_setup(&cpu, uid, GUEST_BUF, n, 1u, GUEST_OUT);
        uint32_t rc = sr_syscall(&cpu, NID_SCE_KERNEL_TRY_SEND_MSG_PIPE);
        expect(rc == 0u || rc == SCE_KERNEL_ERROR_MPP_FULL,
               "churn send returns success or MPP_FULL");
        msgpipe_setup(&cpu, uid, GUEST_OUT, n, 1u, GUEST_OUT);
        rc = sr_syscall(&cpu, NID_SCE_KERNEL_TRY_RECEIVE_MSG_PIPE);
        expect(rc == 0u || rc == SCE_KERNEL_ERROR_MPP_EMPTY,
               "churn receive returns success or MPP_EMPTY");
        if (!sr_hle_test_msgpipe_state(uid, &st) ||
            st.count > st.capacity || st.read_pos >= st.capacity || st.write_pos >= st.capacity) {
            invariants_ok = 0;
        }
    }
    expect(invariants_ok, "count <= capacity and both positions < capacity under 200-op churn");

    /* --- Delete: pipe no longer usable --- */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = uid;
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_DELETE_MSG_PIPE) == 0u, "DeleteMsgPipe succeeds");
    msgpipe_setup(&cpu, uid, GUEST_BUF, 4u, 1u, GUEST_OUT);
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_TRY_SEND_MSG_PIPE) == SCE_KERNEL_ERROR_UNKNOWN_MPPID,
           "send on a deleted pipe returns UNKNOWN_MPPID");
    expect(sr_hle_test_msgpipe_state(uid, &st) == 0, "state probe reports the deleted pipe gone");

    /* --- Exact ceiling boundary: cap itself is legal (probe-backed) --- */
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = GUEST_NAME;
    cpu.r[7] = max_cap;
    uint32_t big_uid = sr_syscall(&cpu, NID_SCE_KERNEL_CREATE_MSG_PIPE);
    expect(big_uid != 0u && big_uid < 0x80000000u, "capacity exactly at the ceiling is accepted");
    expect(sr_hle_test_msgpipe_state(big_uid, &st) == 1 && st.capacity == max_cap,
           "ceiling-capacity pipe is created with the exact requested size");
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = big_uid;
    expect(sr_syscall(&cpu, NID_SCE_KERNEL_DELETE_MSG_PIPE) == 0u, "ceiling pipe deletes cleanly");
}

/* ---- coroutine lifecycle invariants ---------------------------------------------------
 *
 * These read counters recorded by sr_coro.c itself as each operation happened, so they hold
 * whatever the calling source looks like. This is the primary safety proof; the source-shape
 * checks in tools/test_sched_invariants.py are a secondary diagnostic only. */
static void check_coroutine_lifecycle(void) {
    SrCoroLifecycle lc;
    sr_coro_lifecycle_snapshot(&lc);

    /* Adoption is a one-shot initialisation operation. */
    expect(lc.adoptions == 1,
           "exactly one main-coroutine adoption occurred for the whole run");
    expect(lc.adopt_while_child == 0,
           "no adoption occurred while a child coroutine was executing");
    expect(lc.identity_changes == 0,
           "the adopted scheduler identity never changed after it was established");
    expect(lc.main_coro == (const void *)s_sched_coro,
           "the scheduler coroutine is exactly the identity the implementation adopted");

    /* Parking. */
    expect(lc.self_switch_noops == 0,
           "no switch ever targeted the coroutine that was already running");
    expect(lc.null_switch_noops == 0,
           "no switch was ever issued with a NULL target");
    expect(lc.child_to_other == 0,
           "every switch out of a child coroutine targeted the adopted scheduler");
    /* Two joiner bodies, plus the WaitSemaCB control body from
     * test_wait_sema_count_validation, plus the two waiter bodies from
     * test_expired_timed_object_waits_enter_strict_priority (each parks once, after
     * its timed wait returns the timeout; the two runner bodies in that test park
     * ZERO times, because sched_preempt() transfers control out of them and never
     * returns -- which is exactly what that test asserts), plus one park per
     * conformance probe leg
     * that returned, plus the delay-thread body from
     * test_delay_advances_unified_timeline (its sceKernelDelayThread parks
     * inside the syscall, and the body parks again when the syscall returns).
     * ic_expected_parks() derives its count from the recorded outcome table,
     * not from the park hook, so this stays a genuine cross-check of the
     * coroutine layer rather than a tautology. */
    {
        int expected_parks = 6 + ic_expected_parks();
        char msg[224];
        snprintf(msg, sizeof msg,
                 "every parking body parked exactly once (2 joiners + 1 sema CB body "
                 "+ 1 delay body + 2 slice-C waiters + %d returned conformance legs "
                 "= %d, observed %lu)",
                 ic_expected_parks(), expected_parks, s_parks);
        expect(s_parks == (unsigned long)expected_parks, msg);
    }
    expect(s_park_target_mismatch == NULL,
           "every park targeted the adopted scheduler identity, not a look-alike");
    expect(lc.child_to_main >= s_parks,
           "each park transferred control to the adopted scheduler coroutine");

    /* Creation and destruction. */
    expect(lc.creates > 0, "the run actually created coroutines to observe");
    expect(lc.creates == lc.destroys, "every created coroutine was destroyed");
    expect(lc.live == 0, "no coroutine outlived the run");
    expect(lc.double_destroys == 0, "no coroutine was destroyed more than once");
    expect(lc.destroy_while_running == 0, "no coroutine was destroyed while it was running");
    expect(lc.destroy_of_main == 0, "the adopted main coroutine was never destroyed");
    expect(lc.tracked_overflow == 0, "the lifecycle registry tracked every coroutine");
    expect(lc.alias_live == 0, "sr_coro_create never returned a still-live coroutine address");
    expect(lc.bad_incarnations == 0,
           "every reused coroutine address had been destroyed exactly once first");
    {
        const char *why = NULL;
        int ok = sr_coro_lifecycle_all_destroyed_once(&why);
        expect(ok, why ? why : "each created coroutine was destroyed exactly once");
    }

    fprintf(stderr,
            "hle_thread_selftest: lifecycle adoptions=%lu creates=%lu destroys=%lu live=%lu "
            "switches=%lu child_to_main=%lu child_to_other=%lu self_switch=%lu null_switch=%lu "
            "parks=%lu addr_reuse=%lu incarnations_retired=%lu\n",
            lc.adoptions, lc.creates, lc.destroys, lc.live, lc.switches,
            lc.child_to_main, lc.child_to_other, lc.self_switch_noops,
            lc.null_switch_noops, s_parks, lc.address_reuses, lc.clean_incarnations);
}

/* ---- production-HLE PSP oracle mode -----------------------------------------------
 *
 * This mode deliberately lives in the existing production selftest executable.  It does
 * not synthesize a second emitter or call private handlers directly: each case below sets up
 * a small white-box scheduler fixture, then enters the registered NID through sr_syscall and
 * derives every emitted scalar from the returned state.  The fixture setup is category-2
 * production-helper evidence; the syscall/handler/callback dispatch path is production code.
 */
#define ORACLE_NID_CREATE_THREAD       0x446d8de6u
#define ORACLE_NID_START_THREAD        0xf475845du
#define ORACLE_NID_WAIT_THREAD_END     0x278c0df5u
#define ORACLE_NID_GET_EXIT_STATUS     0x3b183e26u
#define ORACLE_NID_DELETE_THREAD       0x9fa03cd3u
#define ORACLE_NID_TERMINATE_DELETE    0x383f7bccu
#define ORACLE_NID_WAKEUP_THREAD       0xd59ead2fu
#define ORACLE_NID_GET_THREAD_ID       0x293b45b8u
#define ORACLE_NID_CREATE_CALLBACK     0xe81caf8fu
#define ORACLE_NID_DELETE_CALLBACK     0xedba5844u
#define ORACLE_NID_NOTIFY_CALLBACK     0xc11ba8c4u
#define ORACLE_NID_CHECK_CALLBACK      0x349d6d6cu
#define ORACLE_NID_CANCEL_CALLBACK     0xba4051d6u
#define ORACLE_NID_CALLBACK_COUNT      0x2a3d44ffu
#define ORACLE_NID_CREATE_SEMA         0xd6da4ba1u
#define ORACLE_NID_DELETE_SEMA         0x28b6489cu
#define ORACLE_NID_SIGNAL_SEMA         0x3f53e640u
#define ORACLE_NID_POLL_SEMA           0x58b1f937u

enum { ORACLE_UNAVAILABLE = -1 };

typedef struct {
    const char *case_id;
    /* Retained in the command-line contract for Make/runbook compatibility.
     * The digest is taken from the running module below, never from this
     * caller-provided path. */
    const char *artifact;
    const char *source_commit;
    const char *model;
    const char *firmware;
} OracleArgs;

#ifdef SR_PSP_ORACLE_SMOKE
/* Generated from the source-owned PSP oracle ELF by
 * tools/psp_oracle/build_nakagawa_smoke.py.  The adapter enters the translated
 * guest body; it does not calculate or substitute the expected sum. */
extern uint32_t sr_psp_oracle_smoke_sum(CpuState *s, uint32_t count);
#endif

static uint32_t oracle_rotr(uint32_t value, unsigned shift) {
    return (value >> shift) | (value << (32u - shift));
}

static const uint32_t oracle_sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} OracleSha256;

static void oracle_sha_init(OracleSha256 *sha) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(sha->h, initial, sizeof(initial));
    sha->bits = 0;
    sha->used = 0;
}

static void oracle_sha_transform(OracleSha256 *sha, const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) {
        unsigned j = i * 4u;
        w[i] = ((uint32_t)block[j] << 24) | ((uint32_t)block[j + 1u] << 16) |
               ((uint32_t)block[j + 2u] << 8) | (uint32_t)block[j + 3u];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = oracle_rotr(w[i - 15u], 7) ^ oracle_rotr(w[i - 15u], 18) ^
                      (w[i - 15u] >> 3);
        uint32_t s1 = oracle_rotr(w[i - 2u], 17) ^ oracle_rotr(w[i - 2u], 19) ^
                      (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = sha->h[0], b = sha->h[1], c = sha->h[2], d = sha->h[3];
    uint32_t e = sha->h[4], f = sha->h[5], g = sha->h[6], h = sha->h[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t s1 = oracle_rotr(e, 6) ^ oracle_rotr(e, 11) ^ oracle_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + oracle_sha_k[i] + w[i];
        uint32_t s0 = oracle_rotr(a, 2) ^ oracle_rotr(a, 13) ^ oracle_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    sha->h[0] += a; sha->h[1] += b; sha->h[2] += c; sha->h[3] += d;
    sha->h[4] += e; sha->h[5] += f; sha->h[6] += g; sha->h[7] += h;
}

static void oracle_sha_update(OracleSha256 *sha, const uint8_t *data, size_t size) {
    sha->bits += (uint64_t)size * 8u;
    while (size) {
        size_t take = sizeof(sha->block) - sha->used;
        if (take > size) take = size;
        memcpy(sha->block + sha->used, data, take);
        sha->used += take;
        data += take;
        size -= take;
        if (sha->used == sizeof(sha->block)) {
            oracle_sha_transform(sha, sha->block);
            sha->used = 0;
        }
    }
}

static void oracle_sha_final(OracleSha256 *sha, uint8_t digest[32]) {
    size_t used = sha->used;
    sha->block[used++] = 0x80u;
    if (used > 56u) {
        memset(sha->block + used, 0, 64u - used);
        oracle_sha_transform(sha, sha->block);
        used = 0;
    }
    memset(sha->block + used, 0, 56u - used);
    for (unsigned i = 0; i < 8; i++)
        sha->block[56u + i] = (uint8_t)(sha->bits >> (56u - 8u * i));
    oracle_sha_transform(sha, sha->block);
    for (unsigned i = 0; i < 8; i++) {
        digest[i * 4u] = (uint8_t)(sha->h[i] >> 24);
        digest[i * 4u + 1u] = (uint8_t)(sha->h[i] >> 16);
        digest[i * 4u + 2u] = (uint8_t)(sha->h[i] >> 8);
        digest[i * 4u + 3u] = (uint8_t)sha->h[i];
    }
}

static int oracle_sha256_file(const char *path, char out[65]) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    OracleSha256 sha;
    uint8_t buffer[4096];
    uint8_t digest[32];
    oracle_sha_init(&sha);
    for (;;) {
        size_t got = fread(buffer, 1, sizeof(buffer), file);
        if (got) oracle_sha_update(&sha, buffer, got);
        if (got < sizeof(buffer)) {
            if (ferror(file)) { fclose(file); return 0; }
            break;
        }
    }
    if (fclose(file) != 0) return 0;
    oracle_sha_final(&sha, digest);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(digest); i++) {
        out[i * 2u] = hex[digest[i] >> 4];
        out[i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    out[64] = '\0';
    return 1;
}

/* Provenance must identify the executable that actually emitted stdout.  Do
 * not hash an arbitrary --artifact argument: a caller could otherwise pass a
 * valid-looking PRX (or unrelated file) while this host selftest executed a
 * different module. */
static int oracle_running_executable(char *path, size_t capacity) {
    if (!path || capacity == 0 || capacity > (size_t)UINT32_MAX) return 0;
    const DWORD length = GetModuleFileNameA(NULL, path, (DWORD)capacity);
    return length != 0 && length < (DWORD)capacity;
}

static int oracle_field_safe(const char *value) {
    if (!value || !*value) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++)
        if (*p <= 0x20u || *p == '=' || *p == '#') return 0;
    return 1;
}

static const char *oracle_arg(const char *name, int argc, char **argv) {
    for (int i = 2; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

static int oracle_parse_args(int argc, char **argv, OracleArgs *out) {
    if (argc < 3 || strcmp(argv[1], "--psp-oracle") != 0) return 0;
    out->case_id = oracle_arg("--case", argc, argv);
    out->artifact = oracle_arg("--artifact", argc, argv);
    out->source_commit = oracle_arg("--source-commit", argc, argv);
    out->model = oracle_arg("--model", argc, argv);
    out->firmware = oracle_arg("--firmware", argc, argv);
    return oracle_field_safe(out->case_id) && oracle_field_safe(out->artifact) &&
           oracle_field_safe(out->source_commit) && oracle_field_safe(out->model) &&
           oracle_field_safe(out->firmware);
}

static int oracle_runtime_init(void) {
    g_mem_base = (uint8_t *)calloc(1, 0x0c000000u);
    if (!g_mem_base) {
        fprintf(stderr, "hle_thread_selftest: cannot allocate guest arena for PSP oracle\n");
        return 0;
    }
    g_mem = g_mem_base + 0x08000000u;
    s_cpu = &s_cpu_store;
    sched_init(&s_cpu_store);
    return 1;
}

static void oracle_runtime_fini(void) {
    free(g_mem_base);
    g_mem_base = NULL;
    g_mem = NULL;
}

static uint32_t oracle_syscall4(CpuState *cpu, uint32_t nid,
                                uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    cpu->r[4] = a0;
    cpu->r[5] = a1;
    cpu->r[6] = a2;
    cpu->r[7] = a3;
    return sr_syscall(cpu, nid);
}

static int oracle_setup_owner(TCB **out_owner) {
    TCB *owner = fixture_thread(0x130u, TH_RUNNING, 20);
    if (!owner) return 0;
    s_cur = (int)(owner - s_tcb);
    memset(s_cpu, 0, sizeof(*s_cpu));
    *out_owner = owner;
    return 1;
}

#ifdef SR_PSP_ORACLE_SMOKE
static int oracle_smoke_case(uint32_t *out0, uint32_t *out1,
                             uint32_t *out2, uint32_t *out3,
                             uint32_t *raw_result) {
    reset_fixture();
    TCB *owner = NULL;
    if (!oracle_setup_owner(&owner)) return ORACLE_UNAVAILABLE;
    (void)owner;
    /* The generated o32 body preserves the incoming guest stack pointer.  The
     * arithmetic function does not dereference it, but a valid guest value
     * keeps this invocation identical to an ordinary generated entry. */
    s_cpu->r[29] = 0x09f00000u;
    uint32_t sum = sr_psp_oracle_smoke_sum(s_cpu, 100u);
    *raw_result = sum;
    *out0 = (uint32_t)(sum == 5050u);
    *out1 = 0;
    *out2 = 0;
    *out3 = 0;
    return sum == 5050u;
}
#endif

static int oracle_callback_case(uint32_t *out0, uint32_t *out1,
                                uint32_t *out2, uint32_t *out3) {
    reset_fixture();
    sr_hle_init();
    TCB *owner = NULL;
    if (!oracle_setup_owner(&owner)) return ORACLE_UNAVAILABLE;
    (void)owner;
    memcpy(SR_HOST(ORACLE_CALLBACK_NAME), "oracle-callback", sizeof("oracle-callback"));
    s_oracle_mode = 1;
    s_oracle_callback_calls = 0;
    s_oracle_callback_arg1 = 0;
    s_oracle_callback_arg2 = 0;

    CpuState *cpu = s_cpu;
    uint32_t cbid = oracle_syscall4(cpu, ORACLE_NID_CREATE_CALLBACK,
                                    ORACLE_CALLBACK_NAME, ORACLE_CALLBACK_ENTRY, 0x55u, 0);
    if (cbid & 0x80000000u) {
        fprintf(stderr, "psp-oracle callback-notify-check unavailable: CreateCallback returned 0x%08x\n", cbid);
        s_oracle_mode = 0;
        return ORACLE_UNAVAILABLE;
    }
    uint32_t notify_first = oracle_syscall4(cpu, ORACLE_NID_NOTIFY_CALLBACK, cbid, 0x1234u, 0, 0);
    uint32_t count_before = oracle_syscall4(cpu, ORACLE_NID_CALLBACK_COUNT, cbid, 0, 0, 0);
    uint32_t check = oracle_syscall4(cpu, ORACLE_NID_CHECK_CALLBACK, 0, 0, 0, 0);
    uint32_t count_after = oracle_syscall4(cpu, ORACLE_NID_CALLBACK_COUNT, cbid, 0, 0, 0);
    uint32_t notify_second = oracle_syscall4(cpu, ORACLE_NID_NOTIFY_CALLBACK, cbid, 0x5678u, 0, 0);
    uint32_t cancel = oracle_syscall4(cpu, ORACLE_NID_CANCEL_CALLBACK, cbid, 0, 0, 0);
    uint32_t count_cancelled = oracle_syscall4(cpu, ORACLE_NID_CALLBACK_COUNT, cbid, 0, 0, 0);
    uint32_t delete_result = oracle_syscall4(cpu, ORACLE_NID_DELETE_CALLBACK, cbid, 0, 0, 0);

    *out0 = (uint32_t)(notify_first == 0) |
            ((uint32_t)(count_before == 1) << 1) |
            ((uint32_t)(check > 0) << 2) |
            ((uint32_t)(count_after == 0) << 3) |
            ((uint32_t)(notify_second == 0) << 4) |
            ((uint32_t)(cancel == 0) << 5) |
            ((uint32_t)(count_cancelled == 0) << 6) |
            ((uint32_t)(delete_result == 0) << 7) |
            ((uint32_t)(s_oracle_callback_calls == 1) << 8);
    *out0 |= ((uint32_t)(count_cancelled & 0xffu) << 16);
    *out1 = ((s_oracle_callback_arg1 & 0xffffu) << 16) |
            (s_oracle_callback_arg2 & 0xffffu);
    *out2 = cancel;
    *out3 = delete_result;
    s_oracle_mode = 0;
    return notify_first == 0 && count_before == 1 && check > 0 && count_after == 0 &&
           notify_second == 0 && cancel == 0 && count_cancelled == 0 &&
           delete_result == 0 && s_oracle_callback_calls == 1;
}

static int oracle_wait_cancel_case(uint32_t *out0, uint32_t *out1,
                                   uint32_t *out2, uint32_t *out3) {
    reset_fixture();
    sr_hle_init();
    TCB *owner = NULL;
    if (!oracle_setup_owner(&owner)) return ORACLE_UNAVAILABLE;
    (void)owner;
    CpuState *cpu = s_cpu;
    uint32_t semaid = oracle_syscall4(cpu, ORACLE_NID_CREATE_SEMA, 0, 0, 0, 1);
    if (semaid & 0x80000000u) {
        fprintf(stderr, "psp-oracle wait-cancel unavailable: CreateSema returned 0x%08x\n", semaid);
        return ORACLE_UNAVAILABLE;
    }
    uint32_t empty = oracle_syscall4(cpu, ORACLE_NID_POLL_SEMA, semaid, 1, 0, 0);
    uint32_t signal = oracle_syscall4(cpu, ORACLE_NID_SIGNAL_SEMA, semaid, 1, 0, 0);
    uint32_t ready = oracle_syscall4(cpu, ORACLE_NID_POLL_SEMA, semaid, 1, 0, 0);
    uint32_t empty_again = oracle_syscall4(cpu, ORACLE_NID_POLL_SEMA, semaid, 1, 0, 0);
    uint32_t delete_result = oracle_syscall4(cpu, ORACLE_NID_DELETE_SEMA, semaid, 0, 0, 0);
    *out0 = (uint32_t)((int32_t)empty < 0) |
            ((uint32_t)(signal == 0) << 1) |
            ((uint32_t)(ready == 0) << 2) |
            ((uint32_t)((int32_t)empty_again < 0) << 3) |
            ((uint32_t)(delete_result == 0) << 4);
    *out1 = 0;
    *out2 = empty;
    *out3 = empty_again;
    return *out0 == 0x1fu;
}

static int oracle_thread_lifecycle_case(uint32_t *out0, uint32_t *out1,
                                        uint32_t *out2, uint32_t *out3) {
    reset_fixture();
    sr_hle_init();
    TCB *owner = NULL;
    if (!oracle_setup_owner(&owner)) return ORACLE_UNAVAILABLE;
    s_exit_argument = 0x42;
    uint32_t thid = oracle_syscall4(s_cpu, ORACLE_NID_CREATE_THREAD, 0,
                                    ORACLE_THREAD_ENTRY, 32, 0x4000u);
    if (thid & 0x80000000u) {
        fprintf(stderr, "psp-oracle thread-lifecycle unavailable: CreateThread returned 0x%08x\n", thid);
        return ORACLE_UNAVAILABLE;
    }
    uint32_t start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD, thid, 0, 0, 0);
    TCB *worker = tcb_by_uid(thid);
    if (!worker || worker->state != TH_READY) {
        fprintf(stderr, "psp-oracle thread-lifecycle unavailable: StartThread did not produce a READY target\n");
        return ORACLE_UNAVAILABLE;
    }
    run_worker(worker);
    s_cur = (int)(owner - s_tcb);
    owner->state = TH_RUNNING;
    uint32_t wait = oracle_syscall4(s_cpu, ORACLE_NID_WAIT_THREAD_END, thid, 0, 0, 0);
    uint32_t exit_status = oracle_syscall4(s_cpu, ORACLE_NID_GET_EXIT_STATUS, thid, 0, 0, 0);
    uint32_t delete_result = oracle_syscall4(s_cpu, ORACLE_NID_DELETE_THREAD, thid, 0, 0, 0);
    uint32_t post_delete_status = oracle_syscall4(s_cpu, ORACLE_NID_GET_EXIT_STATUS, thid, 0, 0, 0);
    *out0 = (uint32_t)(start == 0) |
            ((uint32_t)(wait == 0x42u) << 1) |
            ((uint32_t)(delete_result == 0) << 2) |
            ((uint32_t)((int32_t)post_delete_status < 0) << 3);
    *out1 = (exit_status & 0xffffu) | (wait << 16);
    *out2 = post_delete_status;
    *out3 = delete_result;
    return start == 0 && (int32_t)wait >= 0 && delete_result == 0 &&
           (int32_t)post_delete_status < 0 && exit_status == 0x42u;
}

static int oracle_thread_delete_case(uint32_t *out0, uint32_t *out1,
                                     uint32_t *out2, uint32_t *out3,
                                     uint32_t *out4, uint32_t *out5,
                                     uint32_t *out6, uint32_t *out7,
                                     uint32_t *out8) {
    reset_fixture();
    sr_hle_init();
    TCB *owner = NULL;
    if (!oracle_setup_owner(&owner)) return ORACLE_UNAVAILABLE;

    uint32_t invalid_delete = oracle_syscall4(s_cpu, ORACLE_NID_DELETE_THREAD,
                                              0x7fffffffu, 0, 0, 0);
    uint32_t current_uid = oracle_syscall4(s_cpu, ORACLE_NID_GET_THREAD_ID, 0, 0, 0, 0);
    uint32_t current_delete = oracle_syscall4(s_cpu, ORACLE_NID_DELETE_THREAD,
                                              current_uid, 0, 0, 0);

    uint32_t term_target_uid = oracle_syscall4(s_cpu, ORACLE_NID_CREATE_THREAD, 0,
                                               ORACLE_THREAD_ENTRY, 48, 0x4000u);
    TCB *term_target = tcb_by_uid(term_target_uid);
    if (!term_target) return ORACLE_UNAVAILABLE;
    uint32_t term_start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD,
                                          term_target_uid, 0, 0, 0);
    s_oracle_thread_action = ORACLE_THREAD_ACTION_SLEEP;
    run_worker(term_target);
    s_oracle_thread_action = ORACLE_THREAD_ACTION_EXIT;

    TCB *term_joiner = fixture_thread(0x1a0u, TH_WAIT_OBJ, 32);
    term_joiner->wait_obj = term_target_uid;
    term_joiner->join_target = term_target_uid;
    term_joiner->join_waiting = 1;
    s_cur = (int)(owner - s_tcb);
    owner->state = TH_RUNNING;
    uint32_t term_delete = oracle_syscall4(s_cpu, ORACLE_NID_TERMINATE_DELETE,
                                           term_target_uid, 0, 0, 0);
    uint32_t term_join_result = term_joiner->join_result;
    uint32_t term_post_status = oracle_syscall4(s_cpu, ORACLE_NID_GET_EXIT_STATUS,
                                                term_target_uid, 0, 0, 0);
    uint32_t term_post_start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD,
                                               term_target_uid, 0, 0, 0);
    uint32_t term_post_wake = oracle_syscall4(s_cpu, ORACLE_NID_WAKEUP_THREAD,
                                              term_target_uid, 0, 0, 0);

    uint32_t exit_target_uid = oracle_syscall4(s_cpu, ORACLE_NID_CREATE_THREAD, 0,
                                               ORACLE_THREAD_ENTRY, 48, 0x4000u);
    TCB *exit_target = tcb_by_uid(exit_target_uid);
    if (!exit_target) return ORACLE_UNAVAILABLE;
    TCB *exit_joiner = fixture_thread(0x1a1u, TH_WAIT_OBJ, 32);
    exit_joiner->wait_obj = exit_target_uid;
    exit_joiner->join_target = exit_target_uid;
    exit_joiner->join_waiting = 1;
    s_exit_argument = 0x66;
    s_oracle_thread_action = ORACLE_THREAD_ACTION_EXIT_DELETE;
    uint32_t exit_start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD,
                                          exit_target_uid, 0, 0, 0);
    run_worker(exit_target);
    s_oracle_thread_action = ORACLE_THREAD_ACTION_EXIT;
    s_cur = (int)(owner - s_tcb);
    owner->state = TH_RUNNING;
    uint32_t exit_join_result = exit_joiner->join_result;
    exit_joiner->state = TH_DORMANT;
    exit_joiner->started = 1;
    exit_joiner->exit_status = (int32_t)exit_join_result;
    uint32_t exit_join_wait = oracle_syscall4(s_cpu, ORACLE_NID_WAIT_THREAD_END,
                                              exit_joiner->uid, 0, 0, 0);
    uint32_t exit_post_status = oracle_syscall4(s_cpu, ORACLE_NID_GET_EXIT_STATUS,
                                                exit_target_uid, 0, 0, 0);

    *out0 = (uint32_t)((int32_t)invalid_delete < 0) |
            ((uint32_t)(current_delete == 0x800201a4u) << 1) |
            ((uint32_t)(term_target_uid != 0 && term_start == 0) << 2) |
            ((uint32_t)(term_delete == 0) << 3) |
            ((uint32_t)(term_join_result == 0x800201acu) << 4) |
            ((uint32_t)(term_joiner->state == TH_READY) << 5) |
            ((uint32_t)(term_post_status == 0x80020198u) << 6) |
            ((uint32_t)(term_post_start == 0x80020198u) << 7) |
            ((uint32_t)(term_post_wake == 0x80020198u) << 8) |
            ((uint32_t)(exit_target_uid != 0 && exit_start == 0) << 9) |
            ((uint32_t)(exit_join_result == 0x66u) << 10) |
            ((uint32_t)(exit_join_wait == 0x66u) << 11) |
            ((uint32_t)(exit_post_status == 0x80020198u) << 12);
    *out1 = invalid_delete;
    *out2 = current_delete;
    *out3 = term_delete;
    *out4 = term_join_result;
    *out5 = term_post_status;
    *out6 = exit_join_result;
    *out7 = exit_post_status;
    *out8 = exit_join_wait;
    return *out0 == 0x1fffu;
}

static int oracle_thread_delete_followup_case(uint32_t *out0, uint32_t *out1,
                                              uint32_t *out2, uint32_t *out3,
                                              uint32_t *out4, uint32_t *out5,
                                              uint32_t *out6, uint32_t *out7,
                                              uint32_t *out8, uint32_t *out9,
                                              uint32_t *out10, uint32_t *out11,
                                              uint32_t *out12, uint32_t *out13,
                                              uint32_t *out14) {
    reset_fixture();
    sr_hle_init();
    TCB *owner = NULL;
    if (!oracle_setup_owner(&owner)) return ORACLE_UNAVAILABLE;

    /* The host oracle uses the production scheduler's real target lifecycle,
       while the joiner body is represented by the same observable state that
       the guest entry has after returning from its inner WaitThreadEnd. */
    uint32_t error_target_uid = oracle_syscall4(s_cpu, ORACLE_NID_CREATE_THREAD, 0,
                                                ORACLE_THREAD_ENTRY, 48, 0x4000u);
    TCB *error_target = tcb_by_uid(error_target_uid);
    if (!error_target) return ORACLE_UNAVAILABLE;
    uint32_t error_start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD,
                                           error_target_uid, 0, 0, 0);
    s_oracle_thread_action = ORACLE_THREAD_ACTION_SLEEP;
    run_worker(error_target);
    s_oracle_thread_action = ORACLE_THREAD_ACTION_EXIT;
    TCB *error_joiner = fixture_thread(0x1a2u, TH_WAIT_OBJ, 32);
    error_joiner->wait_obj = error_target_uid;
    error_joiner->join_target = error_target_uid;
    error_joiner->join_waiting = 1;
    s_cur = (int)(owner - s_tcb);
    owner->state = TH_RUNNING;
    uint32_t error_delete = oracle_syscall4(s_cpu, ORACLE_NID_TERMINATE_DELETE,
                                            error_target_uid, 0, 0, 0);
    uint32_t error_inner = error_joiner->join_result;
    error_joiner->state = TH_DORMANT;
    error_joiner->started = 1;
    /* This control represents the implicit entry-return path.  The explicit
       sibling below drives the production ExitThread NID separately; both
       now converge on the measured signed-negative non-delete rule. */
    error_joiner->exit_status = (int32_t)0x800200d2u;
    error_joiner->join_waiting = 0;
    error_joiner->join_result_valid = 0;
    error_joiner->join_target = 0;
    SrThreadRunStatus error_info;
    memset(&error_info, 0, sizeof(error_info));
    uint32_t error_ref = (uint32_t)sched_thread_run_status(error_joiner->uid, &error_info);
    uint32_t error_outer = oracle_syscall4(s_cpu, ORACLE_NID_WAIT_THREAD_END,
                                           error_joiner->uid, 0, 0, 0);

    uint32_t positive_target_uid = oracle_syscall4(s_cpu, ORACLE_NID_CREATE_THREAD, 0,
                                                   ORACLE_THREAD_ENTRY, 48, 0x4000u);
    TCB *positive_target = tcb_by_uid(positive_target_uid);
    if (!positive_target) return ORACLE_UNAVAILABLE;
    uint32_t positive_start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD,
                                              positive_target_uid, 0, 0, 0);
    s_oracle_thread_action = ORACLE_THREAD_ACTION_SLEEP;
    run_worker(positive_target);
    s_oracle_thread_action = ORACLE_THREAD_ACTION_EXIT;
    TCB *positive_joiner = fixture_thread(0x1a3u, TH_WAIT_OBJ, 32);
    positive_joiner->wait_obj = positive_target_uid;
    positive_joiner->join_target = positive_target_uid;
    positive_joiner->join_waiting = 1;
    s_cur = (int)(owner - s_tcb);
    owner->state = TH_RUNNING;
    uint32_t positive_delete = oracle_syscall4(s_cpu, ORACLE_NID_TERMINATE_DELETE,
                                               positive_target_uid, 0, 0, 0);
    uint32_t positive_inner = positive_joiner->join_result;
    positive_joiner->state = TH_DORMANT;
    positive_joiner->started = 1;
    positive_joiner->exit_status = 0x77;
    positive_joiner->join_waiting = 0;
    positive_joiner->join_result_valid = 0;
    positive_joiner->join_target = 0;
    SrThreadRunStatus positive_info;
    memset(&positive_info, 0, sizeof(positive_info));
    uint32_t positive_ref = (uint32_t)sched_thread_run_status(positive_joiner->uid, &positive_info);
    uint32_t positive_outer = oracle_syscall4(s_cpu, ORACLE_NID_WAIT_THREAD_END,
                                              positive_joiner->uid, 0, 0, 0);

    /* As in the PSP fixture, PASS covers only setup, inner results, and
       successful status queries. The state fields and two outer wait values
       are emitted raw and remain unresolved observations. */
    *out0 = (uint32_t)(error_target_uid != 0 && error_start == 0) |
            ((uint32_t)(error_joiner->state == TH_DORMANT) << 1) |
            ((uint32_t)(error_delete == 0) << 2) |
            ((uint32_t)(error_inner == 0x800201acu) << 3) |
            ((uint32_t)(error_ref == 0) << 4) |
            ((uint32_t)(error_info.status == PSP_THREAD_STOPPED) << 5) |
            ((uint32_t)(error_info.waitType == PSP_WAIT_NONE) << 6) |
            ((uint32_t)(error_info.status == PSP_THREAD_STOPPED &&
                        error_joiner->exit_status == (int32_t)0x800200d2u) << 7) |
            ((uint32_t)(positive_target_uid != 0 && positive_start == 0) << 8) |
            ((uint32_t)(positive_joiner->state == TH_DORMANT) << 9) |
            ((uint32_t)(positive_delete == 0) << 10) |
            ((uint32_t)(positive_inner == 0x800201acu) << 11) |
            ((uint32_t)(positive_ref == 0) << 12) |
            ((uint32_t)(positive_info.status == PSP_THREAD_STOPPED) << 13) |
            ((uint32_t)(positive_info.waitType == PSP_WAIT_NONE) << 14) |
            ((uint32_t)(positive_joiner->exit_status == 0x77) << 15) |
            (1u << 16) |
            (1u << 17) |
            (1u << 18) |
            (1u << 19);
    *out1 = error_inner;
    *out2 = error_outer;
    *out3 = error_ref;
    *out4 = error_info.status;
    *out5 = error_info.waitType;
    *out6 = error_info.waitId;
    *out7 = (uint32_t)error_joiner->exit_status;
    *out8 = positive_inner;
    *out9 = positive_outer;
    *out10 = positive_ref;
    *out11 = positive_info.status;
    *out12 = positive_info.waitType;
    *out13 = positive_info.waitId;
    *out14 = (uint32_t)positive_joiner->exit_status;
    return error_target_uid != 0 && error_start == 0 &&
           error_delete == 0 &&
           error_inner == 0x800201acu && error_ref == 0 &&
           error_outer == 0x800200d2u &&
           error_info.status == PSP_THREAD_STOPPED && error_info.waitType == PSP_WAIT_NONE &&
           error_joiner->exit_status == (int32_t)0x800200d2u &&
           positive_target_uid != 0 && positive_start == 0 &&
           positive_delete == 0 &&
           positive_inner == 0x800201acu && positive_ref == 0 &&
           positive_outer == 0x77u &&
           positive_info.status == PSP_THREAD_STOPPED && positive_info.waitType == PSP_WAIT_NONE &&
           positive_joiner->exit_status == 0x77;
}

/* The explicit sibling drives the production ExitThread NID for the
 * intermediate joiner instead of fabricating its final TCB state.  The target
 * lifecycle and inner THREAD_TERMINATED latch remain the same as the PSP
 * fixture; ReferThreadStatus is represented by the scheduler's white-box
 * status helper because this selftest does not expose a guest info buffer. */
static int oracle_thread_delete_exit_pair_case(uint32_t *out0, uint32_t *out1,
                                               uint32_t *out2, uint32_t *out3,
                                               uint32_t *out4, uint32_t *out5,
                                               uint32_t *out6, uint32_t *out7,
                                               uint32_t *out8, uint32_t *out9,
                                               uint32_t *out10, uint32_t *out11,
                                               uint32_t *out12, uint32_t *out13,
                                               uint32_t *out14, uint32_t *out15,
                                               uint32_t *out16,
                                               uint32_t explicit_error,
                                               uint32_t expected_error,
                                               uint32_t explicit_positive,
                                               uint32_t expected_positive) {
    reset_fixture();
    sr_hle_init();
    TCB *owner = NULL;
    if (!oracle_setup_owner(&owner)) return ORACLE_UNAVAILABLE;

    uint32_t error_target_uid = oracle_syscall4(s_cpu, ORACLE_NID_CREATE_THREAD, 0,
                                                ORACLE_THREAD_ENTRY, 48, 0x4000u);
    TCB *error_target = tcb_by_uid(error_target_uid);
    if (!error_target) return ORACLE_UNAVAILABLE;
    uint32_t error_start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD,
                                           error_target_uid, 0, 0, 0);
    s_oracle_thread_action = ORACLE_THREAD_ACTION_SLEEP;
    run_worker(error_target);

    uint32_t error_joiner_uid = oracle_syscall4(s_cpu, ORACLE_NID_CREATE_THREAD, 0,
                                                ORACLE_THREAD_ENTRY, 16, 0x4000u);
    TCB *error_joiner = tcb_by_uid(error_joiner_uid);
    if (!error_joiner) return ORACLE_UNAVAILABLE;
    uint32_t error_join_start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD,
                                                error_joiner_uid, 0, 0, 0);
    error_joiner->state = TH_WAIT_OBJ;
    error_joiner->wait_obj = error_target_uid;
    error_joiner->join_target = error_target_uid;
    error_joiner->join_waiting = 1;
    s_cur = (int)(owner - s_tcb);
    owner->state = TH_RUNNING;
    uint32_t error_delete = oracle_syscall4(s_cpu, ORACLE_NID_TERMINATE_DELETE,
                                            error_target_uid, 0, 0, 0);
    uint32_t error_inner = error_joiner->join_result;
    s_exit_argument = (int32_t)explicit_error;
    s_oracle_thread_action = ORACLE_THREAD_ACTION_EXIT;
    run_worker(error_joiner);
    s_cur = (int)(owner - s_tcb);
    owner->state = TH_RUNNING;
    uint32_t error_outer = oracle_syscall4(s_cpu, ORACLE_NID_WAIT_THREAD_END,
                                           error_joiner_uid, 0, 0, 0);
    SrThreadRunStatus error_info;
    memset(&error_info, 0, sizeof(error_info));
    uint32_t error_ref = (uint32_t)sched_thread_run_status(error_joiner_uid, &error_info);
    uint32_t error_exit_status = sched_thread_exit_status(error_joiner_uid);
    if (error_joiner->coro) {
        sr_coro_destroy(error_joiner->coro);
        error_joiner->coro = NULL;
    }
    (void)oracle_syscall4(s_cpu, ORACLE_NID_DELETE_THREAD, error_joiner_uid, 0, 0, 0);

    uint32_t positive_target_uid = oracle_syscall4(s_cpu, ORACLE_NID_CREATE_THREAD, 0,
                                                   ORACLE_THREAD_ENTRY, 48, 0x4000u);
    TCB *positive_target = tcb_by_uid(positive_target_uid);
    if (!positive_target) return ORACLE_UNAVAILABLE;
    uint32_t positive_start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD,
                                              positive_target_uid, 0, 0, 0);
    s_oracle_thread_action = ORACLE_THREAD_ACTION_SLEEP;
    run_worker(positive_target);

    uint32_t positive_joiner_uid = oracle_syscall4(s_cpu, ORACLE_NID_CREATE_THREAD, 0,
                                                   ORACLE_THREAD_ENTRY, 16, 0x4000u);
    TCB *positive_joiner = tcb_by_uid(positive_joiner_uid);
    if (!positive_joiner) return ORACLE_UNAVAILABLE;
    uint32_t positive_join_start = oracle_syscall4(s_cpu, ORACLE_NID_START_THREAD,
                                                   positive_joiner_uid, 0, 0, 0);
    positive_joiner->state = TH_WAIT_OBJ;
    positive_joiner->wait_obj = positive_target_uid;
    positive_joiner->join_target = positive_target_uid;
    positive_joiner->join_waiting = 1;
    s_cur = (int)(owner - s_tcb);
    owner->state = TH_RUNNING;
    uint32_t positive_delete = oracle_syscall4(s_cpu, ORACLE_NID_TERMINATE_DELETE,
                                               positive_target_uid, 0, 0, 0);
    uint32_t positive_inner = positive_joiner->join_result;
    s_exit_argument = (int32_t)explicit_positive;
    s_oracle_thread_action = ORACLE_THREAD_ACTION_EXIT;
    run_worker(positive_joiner);
    s_cur = (int)(owner - s_tcb);
    owner->state = TH_RUNNING;
    uint32_t positive_outer = oracle_syscall4(s_cpu, ORACLE_NID_WAIT_THREAD_END,
                                              positive_joiner_uid, 0, 0, 0);
    SrThreadRunStatus positive_info;
    memset(&positive_info, 0, sizeof(positive_info));
    uint32_t positive_ref = (uint32_t)sched_thread_run_status(positive_joiner_uid, &positive_info);
    uint32_t positive_exit_status = sched_thread_exit_status(positive_joiner_uid);
    if (positive_joiner->coro) {
        sr_coro_destroy(positive_joiner->coro);
        positive_joiner->coro = NULL;
    }
    (void)oracle_syscall4(s_cpu, ORACLE_NID_DELETE_THREAD, positive_joiner_uid, 0, 0, 0);

    /* Bits 16..19 mirror the PSP fixture's two semaphore handshakes.  The
     * host model has already constructed the same ordered state, so those
     * controls are deterministic here; bits 20..21 make the explicit outer
     * results visible in the mask as well as in the raw fields. */
    *out0 = (uint32_t)(error_target_uid != 0 && error_start == 0) |
            ((uint32_t)(error_joiner_uid != 0 && error_join_start == 0) << 1) |
            ((uint32_t)(error_delete == 0) << 2) |
            ((uint32_t)(error_inner == 0x800201acu) << 3) |
            ((uint32_t)(error_ref == 0) << 4) |
            ((uint32_t)(error_info.status == PSP_THREAD_STOPPED) << 5) |
            ((uint32_t)(error_info.waitType == PSP_WAIT_NONE) << 6) |
            ((uint32_t)(error_exit_status == expected_error) << 7) |
            ((uint32_t)(positive_target_uid != 0 && positive_start == 0) << 8) |
            ((uint32_t)(positive_joiner_uid != 0 && positive_join_start == 0) << 9) |
            ((uint32_t)(positive_delete == 0) << 10) |
            ((uint32_t)(positive_inner == 0x800201acu) << 11) |
            ((uint32_t)(positive_ref == 0) << 12) |
            ((uint32_t)(positive_info.status == PSP_THREAD_STOPPED) << 13) |
            ((uint32_t)(positive_info.waitType == PSP_WAIT_NONE) << 14) |
            ((uint32_t)(positive_exit_status == expected_positive) << 15) |
            (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19) |
            ((uint32_t)(error_outer == expected_error) << 20) |
            ((uint32_t)(positive_outer == expected_positive) << 21);
    *out1 = error_inner;
    *out2 = error_outer;
    *out3 = error_ref;
    *out4 = error_info.status;
    *out5 = error_info.waitType;
    *out6 = error_info.waitId;
    *out7 = error_exit_status;
    *out8 = positive_inner;
    *out9 = positive_outer;
    *out10 = positive_ref;
    *out11 = positive_info.status;
    *out12 = positive_info.waitType;
    *out13 = positive_info.waitId;
    *out14 = positive_exit_status;
    *out15 = explicit_error;
    *out16 = explicit_positive;
    return error_target_uid != 0 && error_start == 0 &&
           error_joiner_uid != 0 && error_join_start == 0 && error_delete == 0 &&
           error_inner == 0x800201acu && error_outer == expected_error && error_ref == 0 &&
           error_info.status == PSP_THREAD_STOPPED && error_info.waitType == PSP_WAIT_NONE &&
           error_info.waitId == 0 && error_exit_status == expected_error &&
           positive_target_uid != 0 && positive_start == 0 &&
           positive_joiner_uid != 0 && positive_join_start == 0 && positive_delete == 0 &&
           positive_inner == 0x800201acu && positive_outer == expected_positive &&
           positive_ref == 0 && positive_info.status == PSP_THREAD_STOPPED &&
           positive_info.waitType == PSP_WAIT_NONE && positive_info.waitId == 0 &&
           positive_exit_status == expected_positive;
}

static int oracle_thread_delete_explicit_case(uint32_t *out0, uint32_t *out1,
                                              uint32_t *out2, uint32_t *out3,
                                              uint32_t *out4, uint32_t *out5,
                                              uint32_t *out6, uint32_t *out7,
                                              uint32_t *out8, uint32_t *out9,
                                              uint32_t *out10, uint32_t *out11,
                                              uint32_t *out12, uint32_t *out13,
                                              uint32_t *out14) {
    uint32_t ignored_error = 0;
    uint32_t ignored_positive = 0;
    return oracle_thread_delete_exit_pair_case(
        out0, out1, out2, out3, out4, out5, out6, out7, out8, out9,
        out10, out11, out12, out13, out14, &ignored_error, &ignored_positive,
        0x800201acu, 0x800200d2u, 0x78u, 0x78u);
}

static int oracle_thread_delete_boundary_case(uint32_t *out0, uint32_t *out1,
                                              uint32_t *out2, uint32_t *out3,
                                              uint32_t *out4, uint32_t *out5,
                                              uint32_t *out6, uint32_t *out7,
                                              uint32_t *out8, uint32_t *out9,
                                              uint32_t *out10, uint32_t *out11,
                                              uint32_t *out12, uint32_t *out13,
                                              uint32_t *out14, uint32_t *out15,
                                              uint32_t *out16) {
    return oracle_thread_delete_exit_pair_case(
        out0, out1, out2, out3, out4, out5, out6, out7, out8, out9,
        out10, out11, out12, out13, out14, out15, out16,
        0x800201a8u, 0x800200d2u, (uint32_t)-17, 0x800200d2u);
}

static int oracle_emit(const OracleArgs *args, int pass, uint32_t result_value,
                       uint32_t out0, uint32_t out1, uint32_t out2, uint32_t out3,
                       const uint32_t *extra, size_t extra_count) {
    char artifact_path[32768];
    char digest[65];
    if (!oracle_running_executable(artifact_path, sizeof(artifact_path)) ||
        !oracle_sha256_file(artifact_path, digest)) {
        fprintf(stderr, "psp-oracle: cannot hash the running selftest executable\n");
        return 0;
    }
    const int smoke = strcmp(args->case_id, "sum-1-to-100") == 0;
    printf("NAKAGAWA_PSP_META schema=1 source=nakagawa model=%s firmware=%s "
           "binary_sha256=%s source_commit=%s fixture=%s\n",
           args->model, args->firmware, digest, args->source_commit,
           smoke ? "nakagawa-generated-guest-production-runtime-direct-entry"
                 : "nakagawa-hle-thread-selftest-production-dispatch");
    if (smoke) {
        printf("NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SMOKE-001 case_id=%s "
               "status=%s result=0x%08x out0=0x%08x\n",
               args->case_id, pass ? "PASS" : "FAIL", result_value, out0);
    } else if (extra && extra_count == 11u &&
               (strcmp(args->case_id, "thread-delete-followup") == 0 ||
                strcmp(args->case_id, "thread-delete-explicit") == 0)) {
        printf("NAKAGAWA_PSP_TEST schema=1 test_id=PSP-KERNEL-001 case_id=%s "
               "status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x "
               "out3=0x%08x out4=0x%08x out5=0x%08x out6=0x%08x out7=0x%08x "
               "out8=0x%08x out9=0x%08x out10=0x%08x out11=0x%08x "
               "out12=0x%08x out13=0x%08x out14=0x%08x\n",
               args->case_id, pass ? "PASS" : "FAIL", result_value,
               out0, out1, out2, out3, extra[0], extra[1], extra[2], extra[3],
               extra[4], extra[5], extra[6], extra[7], extra[8], extra[9],
               extra[10]);
    } else if (extra && extra_count == 13u &&
               strcmp(args->case_id, "thread-delete-boundary") == 0) {
        printf("NAKAGAWA_PSP_TEST schema=1 test_id=PSP-KERNEL-001 case_id=%s "
               "status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x "
               "out3=0x%08x out4=0x%08x out5=0x%08x out6=0x%08x out7=0x%08x "
               "out8=0x%08x out9=0x%08x out10=0x%08x out11=0x%08x "
               "out12=0x%08x out13=0x%08x out14=0x%08x out15=0x%08x "
               "out16=0x%08x\n",
               args->case_id, pass ? "PASS" : "FAIL", result_value,
               out0, out1, out2, out3, extra[0], extra[1], extra[2], extra[3],
               extra[4], extra[5], extra[6], extra[7], extra[8], extra[9],
               extra[10], extra[11], extra[12]);
    } else if (extra && extra_count == 5u &&
               strcmp(args->case_id, "thread-delete-lifecycle") == 0) {
        printf("NAKAGAWA_PSP_TEST schema=1 test_id=PSP-KERNEL-001 case_id=%s "
               "status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x "
               "out3=0x%08x out4=0x%08x out5=0x%08x out6=0x%08x out7=0x%08x out8=0x%08x\n",
               args->case_id, pass ? "PASS" : "FAIL", result_value,
               out0, out1, out2, out3, extra[0], extra[1], extra[2], extra[3], extra[4]);
    } else {
        printf("NAKAGAWA_PSP_TEST schema=1 test_id=PSP-KERNEL-001 case_id=%s "
               "status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x\n",
               args->case_id, pass ? "PASS" : "FAIL", result_value,
               out0, out1, out2, out3);
    }
    return 1;
}

static int run_psp_oracle(int argc, char **argv) {
    OracleArgs args;
    if (!oracle_parse_args(argc, argv, &args)) {
        fprintf(stderr, "usage: --psp-oracle --case CASE --artifact EXE --source-commit SHA "
                        "--model MODEL --firmware FIRMWARE\n");
        return 2;
    }
    const int smoke = strcmp(args.case_id, "sum-1-to-100") == 0;
    if (!smoke && strcmp(args.case_id, "callback-notify-check") != 0 &&
        strcmp(args.case_id, "wait-cancel") != 0 &&
        strcmp(args.case_id, "thread-lifecycle") != 0 &&
        strcmp(args.case_id, "thread-delete-lifecycle") != 0 &&
        strcmp(args.case_id, "thread-delete-followup") != 0 &&
        strcmp(args.case_id, "thread-delete-explicit") != 0 &&
        strcmp(args.case_id, "thread-delete-boundary") != 0) {
        fprintf(stderr, "psp-oracle: unsupported case %s\n", args.case_id);
        return 2;
    }
#ifndef SR_PSP_ORACLE_SMOKE
    if (smoke) {
        fprintf(stderr, "psp-oracle: smoke case is unavailable in this build; no result record emitted\n");
        return 2;
    }
#endif
    if (!oracle_runtime_init()) return 2;
    uint32_t out0 = 0, out1 = 0, out2 = 0, out3 = 0;
    uint32_t extra[13] = {0};
    uint32_t result_value = 0;
    int result;
    if (smoke) {
#ifdef SR_PSP_ORACLE_SMOKE
        result = oracle_smoke_case(&out0, &out1, &out2, &out3, &result_value);
#else
        result = ORACLE_UNAVAILABLE;
#endif
    } else if (strcmp(args.case_id, "callback-notify-check") == 0) {
        result = oracle_callback_case(&out0, &out1, &out2, &out3);
        result_value = result == ORACLE_UNAVAILABLE ? 0u : (result != 0 ? 1u : 0u);
    } else if (strcmp(args.case_id, "wait-cancel") == 0) {
        result = oracle_wait_cancel_case(&out0, &out1, &out2, &out3);
        result_value = result == ORACLE_UNAVAILABLE ? 0u : (result != 0 ? 1u : 0u);
    } else if (strcmp(args.case_id, "thread-delete-lifecycle") == 0) {
        result = oracle_thread_delete_case(&out0, &out1, &out2, &out3,
                                           &extra[0], &extra[1], &extra[2],
                                           &extra[3], &extra[4]);
        result_value = result == ORACLE_UNAVAILABLE ? 0u : (result != 0 ? 1u : 0u);
    } else if (strcmp(args.case_id, "thread-delete-followup") == 0) {
        result = oracle_thread_delete_followup_case(
            &out0, &out1, &out2, &out3, &extra[0], &extra[1], &extra[2],
            &extra[3], &extra[4], &extra[5], &extra[6], &extra[7], &extra[8],
            &extra[9], &extra[10]);
        result_value = result == ORACLE_UNAVAILABLE ? 0u : (result != 0 ? 1u : 0u);
    } else if (strcmp(args.case_id, "thread-delete-explicit") == 0) {
        result = oracle_thread_delete_explicit_case(
            &out0, &out1, &out2, &out3, &extra[0], &extra[1], &extra[2],
            &extra[3], &extra[4], &extra[5], &extra[6], &extra[7], &extra[8],
            &extra[9], &extra[10]);
        result_value = result == ORACLE_UNAVAILABLE ? 0u : (result != 0 ? 1u : 0u);
    } else if (strcmp(args.case_id, "thread-delete-boundary") == 0) {
        result = oracle_thread_delete_boundary_case(
            &out0, &out1, &out2, &out3, &extra[0], &extra[1], &extra[2],
            &extra[3], &extra[4], &extra[5], &extra[6], &extra[7], &extra[8],
            &extra[9], &extra[10], &extra[11], &extra[12]);
        result_value = result == ORACLE_UNAVAILABLE ? 0u : (result != 0 ? 1u : 0u);
    } else {
        result = oracle_thread_lifecycle_case(&out0, &out1, &out2, &out3);
        result_value = result == ORACLE_UNAVAILABLE ? 0u : (result != 0 ? 1u : 0u);
    }

    int rc = 0;
    if (result == ORACLE_UNAVAILABLE) {
        fprintf(stderr, "psp-oracle: case %s could not be driven; no result record emitted\n",
                args.case_id);
        rc = 3;
    } else if (!oracle_emit(&args, result != 0, result_value, out0, out1, out2, out3,
                             (strcmp(args.case_id, "thread-delete-lifecycle") == 0 ||
                             strcmp(args.case_id, "thread-delete-followup") == 0 ||
                             strcmp(args.case_id, "thread-delete-explicit") == 0 ||
                             strcmp(args.case_id, "thread-delete-boundary") == 0) ? extra : NULL,
                            (strcmp(args.case_id, "thread-delete-followup") == 0 ||
                             strcmp(args.case_id, "thread-delete-explicit") == 0) ? 11u :
                            strcmp(args.case_id, "thread-delete-boundary") == 0 ? 13u :
                            strcmp(args.case_id, "thread-delete-lifecycle") == 0 ? 5u : 0u)) {
        rc = 2;
    }
    oracle_runtime_fini();
    return rc;
}

/* ---- state-qualified acceptance routes (issue #64) --------------------------------
 *
 * These exercise the production route engine in hle.c: the real parser, the real
 * signature matcher and the real step machine. Only the entry is test-specific --
 * observations are handed to sr_route_step() directly rather than sampled from a
 * framebuffer, because this executable deliberately omits the renderer.
 *
 * The defect they pin down is issue #64: an acceptance route that reaches the wrong
 * screen must fail loudly. Under the previous fixed-vblank pad script there was no
 * expression for "the screen the next press assumes", so a run that entered STORY MODE
 * instead of the Exhibition route completed normally and was archived as evidence. Every
 * FAILED expectation below is therefore a case that had no failure mode at all before.
 */
int      sr_route_load(const char *path);
uint32_t sr_route_step(uint32_t vblank, const uint8_t *sig);
int      sr_route_status(void);
int      sr_route_sig_bytes(void);
void     sr_route_reset(void);

enum { RT_OFF = 0, RT_LEGACY, RT_RUNNING, RT_DONE, RT_FAILED };

#define RT_PATH "route_selftest_tmp.pad"

static void rt_write(const char *body) {
    FILE *fp = fopen(RT_PATH, "wb");
    if (!fp) { expect(0, "route selftest could not create its temporary route file"); return; }
    fputs(body, fp);
    fclose(fp);
}

/* A flat signature is enough to separate "screens" here: the matcher under test compares
 * mean absolute difference, and a constant differs from another constant by exactly the
 * gap between them, which makes the tolerance boundary exact rather than approximate. */
static void rt_hex(char *out, int value) {
    int n = sr_route_sig_bytes();
    for (int i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", value & 0xff);
}
static void rt_sig(uint8_t *out, int value) {
    for (int i = 0; i < sr_route_sig_bytes(); i++) out[i] = (uint8_t)value;
}

static void test_route_program_advances_only_on_observed_state(void) {
    char hexA[1024], hexB[1024], body[4096];
    uint8_t sigA[576], sigB[576];

    sr_route_reset();
    rt_hex(hexA, 0x20); rt_hex(hexB, 0x80);
    snprintf(body, sizeof body,
             "# route program\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "CHECKPOINT SINGLE_PLAYER_MENU %s\n"
             "WAIT MAIN_MENU 1000\n"
             "PRESS 4000 16\n"
             "WAIT SINGLE_PLAYER_MENU 1000\n"
             "END\n", hexA, hexB);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "a route program loads");
    expect(sr_route_status() == RT_RUNNING, "a loaded route program is running");
    rt_sig(sigA, 0x20); rt_sig(sigB, 0x80);

    /* No observation: the route may not advance and may not press. Elapsed vblanks alone
     * are exactly what issue #64 showed to be worthless. */
    for (uint32_t v = 0; v < 200; v++)
        expect(sr_route_step(v, NULL) == 0u, "an unobserved WAIT emits no input");
    expect(sr_route_status() == RT_RUNNING, "an unobserved WAIT is still waiting");

    /* Wrong screen observed: still no advance. */
    expect(sr_route_step(200, sigB) == 0u, "the wrong screen does not satisfy a WAIT");

    /* Right screen: the WAIT completes and the following PRESS starts on the same vblank. */
    expect(sr_route_step(201, sigA) == 0x4000u, "the press begins on the vblank the screen is reached");
    for (uint32_t v = 202; v < 217; v++)
        expect(sr_route_step(v, NULL) == 0x4000u, "the press is held for its full width");
    expect(sr_route_step(217, NULL) == 0u, "the press is released after its width");

    expect(sr_route_step(240, sigB) == 0u, "the second WAIT is satisfied by the second screen");
    expect(sr_route_status() == RT_DONE, "END completes the route");
    remove(RT_PATH);
}

static void test_route_wait_timeout_fails_loudly(void) {
    char hexA[1024], hexB[1024], body[4096];
    uint8_t sigB[576];

    sr_route_reset();
    rt_hex(hexA, 0x20); rt_hex(hexB, 0x80);
    snprintf(body, sizeof body,
             "CHECKPOINT MAIN_MENU %s\n"
             "CHECKPOINT SINGLE_PLAYER_MENU %s\n"
             "WAIT MAIN_MENU 100\n"
             "PRESS 4000 16\n"
             "END\n", hexA, hexB);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "the timeout route loads");
    rt_sig(sigB, 0x80);
    for (uint32_t v = 0; v < 100; v++) sr_route_step(v, sigB);
    expect(sr_route_status() == RT_RUNNING, "the WAIT is still pending inside its budget");
    sr_route_step(100, sigB);
    expect(sr_route_status() == RT_FAILED, "a WAIT that times out fails the route");
    expect(sr_route_step(101, sigB) == 0u, "a failed route emits no further input");
    remove(RT_PATH);
}

static void test_route_expect_rejects_the_wrong_screen(void) {
    char hexA[1024], hexB[1024], body[4096];
    uint8_t sigA[576], sigB[576];

    /* The #64 divergence in miniature: the route believes it is on the Main Menu, the
     * guest is one level deeper. The run must stop, not press on. */
    sr_route_reset();
    rt_hex(hexA, 0x20); rt_hex(hexB, 0x80);
    snprintf(body, sizeof body,
             "CHECKPOINT MAIN_MENU %s\n"
             "CHECKPOINT SINGLE_PLAYER_MENU %s\n"
             "EXPECT MAIN_MENU\n"
             "PRESS 4000 16\n"
             "END\n", hexA, hexB);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "the expect route loads");
    rt_sig(sigA, 0x20); rt_sig(sigB, 0x80);
    expect(sr_route_step(10, sigB) == 0u, "a mismatched EXPECT presses nothing");
    expect(sr_route_status() == RT_FAILED, "a mismatched EXPECT fails the route");

    sr_route_reset();
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "the expect route reloads");
    expect(sr_route_step(10, sigA) == 0x4000u, "a matching EXPECT advances to the press");
    expect(sr_route_status() == RT_RUNNING, "a matching EXPECT keeps the route running");
    remove(RT_PATH);
}

static void test_route_expect_without_observation_fails_closed(void) {
    char hexA[1024], body[4096];

    sr_route_reset();
    rt_hex(hexA, 0x20);
    snprintf(body, sizeof body,
             "SAMPLE_EVERY 20\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "EXPECT MAIN_MENU\n"
             "END\n", hexA);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "the unobserved-expect route loads");
    for (uint32_t v = 0; v < 20 * 8 + 60; v++) sr_route_step(v, NULL);
    expect(sr_route_status() == RT_RUNNING, "EXPECT waits for an observation within its budget");
    sr_route_step(20 * 8 + 60, NULL);
    expect(sr_route_status() == RT_FAILED,
           "EXPECT fails closed when no framebuffer observation ever arrives");
    remove(RT_PATH);
}

static void test_route_signature_tolerance_is_enforced(void) {
    char hexA[1024], body[4096];
    uint8_t near_sig[576], far_sig[576];

    sr_route_reset();
    rt_hex(hexA, 0x40);
    snprintf(body, sizeof body,
             "CHECKPOINT MAIN_MENU tol=8 %s\n"
             "WAIT MAIN_MENU 10000\n"
             "END\n", hexA);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "the tolerance route loads");
    rt_sig(near_sig, 0x48);   /* mean absolute difference 8: inside tol=8 */
    rt_sig(far_sig,  0x49);   /* mean absolute difference 9: outside tol=8 */
    sr_route_step(0, far_sig);
    expect(sr_route_status() == RT_RUNNING, "a signature outside tolerance does not match");
    sr_route_step(1, near_sig);
    expect(sr_route_status() == RT_DONE, "a signature at the tolerance boundary matches");
    remove(RT_PATH);
}

/* HST redraws its menus over a club backdrop that is not the same every run, so a
 * whole-frame comparison rejects the right screen. Recording the screen twice makes the
 * bytes the two recordings disagree on drop out of the comparison. */
/* The boot prefix has to repeat an input until a screen arrives. As a fixed table every
 * extra press lands on whatever comes next when the run is faster than the recording --
 * which is how a CROSS meant for the title screen ended up opening a menu (issue #64). */
static void test_route_press_until_stops_when_the_screen_arrives(void) {
    char hexA[1024], hexB[1024], body[4096];
    uint8_t sigA[576], sigB[576];

    sr_route_reset();
    rt_hex(hexA, 0x20); rt_hex(hexB, 0x80);
    snprintf(body, sizeof body,
             "CHECKPOINT TITLE %s\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "PRESS_UNTIL TITLE 0008 8 240 12000\n"
             "PRESS 4000 8\n"
             "END\n", hexA, hexB);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "a PRESS_UNTIL route loads");
    rt_sig(sigA, 0x20); rt_sig(sigB, 0x80);

    expect(sr_route_step(0, NULL) == 0x0008u, "PRESS_UNTIL pulses at the start of each period");
    expect(sr_route_step(7, NULL) == 0x0008u, "the pulse covers its full width");
    expect(sr_route_step(8, NULL) == 0u, "the pulse stops after its width");
    expect(sr_route_step(240, NULL) == 0x0008u, "the pulse repeats one period later");
    expect(sr_route_step(480, sigB) == 0x0008u, "a different screen does not end the repeat");

    /* The screen arrives: the repeat ends and the next step starts in the same vblank,
     * so the START pulse is never issued again. */
    expect(sr_route_step(481, sigA) == 0x4000u, "the next step begins on the vblank the screen arrives");
    expect(sr_route_step(485, NULL) == 0x4000u, "the following press holds for its width");
    expect(sr_route_step(489, NULL) == 0u, "no further pulse is issued once the screen was reached");
    expect(sr_route_status() == RT_DONE, "the route completes after the gated press");
    remove(RT_PATH);
}

/* Some screens accept an input only once the work behind them finishes -- the title draws
 * its CONTINUE option long before it will act on one, and no pixel says which. Repeating
 * until the NEXT screen appears leaves a window where a press can still reach it; repeating
 * only while the current screen is on show ends the input before anything else can. */
static void test_route_press_while_ends_with_its_screen(void) {
    char hexA[1024], hexB[1024], body[4096];
    uint8_t sigA[576], sigB[576];

    sr_route_reset();
    rt_hex(hexA, 0x20); rt_hex(hexB, 0x80);
    snprintf(body, sizeof body,
             "CHECKPOINT TITLE %s\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "PRESS_WHILE TITLE 4000 8 300 12000\n"
             "WAIT MAIN_MENU 5000\n"
             "END\n", hexA, hexB);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "a PRESS_WHILE route loads");
    rt_sig(sigA, 0x20); rt_sig(sigB, 0x80);

    expect(sr_route_step(0, sigA) == 0x4000u, "PRESS_WHILE presses while its screen is up");
    expect(sr_route_step(10, sigA) == 0u, "the press respects its width");
    expect(sr_route_step(300, sigA) == 0x4000u, "the press repeats one period later");
    expect(sr_route_status() == RT_RUNNING, "PRESS_WHILE keeps going while the screen is up");

    /* The screen goes away -- to something that is not the next checkpoint either, which is
     * what a crossfade looks like. The step ends there, long before the menu is live. */
    uint8_t midway[576];
    for (int i = 0; i < sr_route_sig_bytes(); i++) midway[i] = 0x50;
    expect(sr_route_step(600, midway) == 0u, "PRESS_WHILE stops as soon as its screen is gone");
    expect(sr_route_step(900, midway) == 0u, "nothing is pressed while waiting for the next screen");
    sr_route_step(1200, sigB);
    expect(sr_route_status() == RT_DONE, "the following WAIT completes the route");

    /* Entering the step one vblank before its screen is drawn must not skip the input. */
    sr_route_reset();
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "the PRESS_WHILE route reloads");
    expect(sr_route_step(0, midway) == 0x4000u, "the press still fires before its screen appears");
    expect(sr_route_status() == RT_RUNNING, "PRESS_WHILE does not complete before its screen is seen");
    remove(RT_PATH);
}

static void test_route_press_until_timeout_fails_loudly(void) {
    char hexA[1024], hexB[1024], body[4096];
    uint8_t sigB[576];

    sr_route_reset();
    rt_hex(hexA, 0x20); rt_hex(hexB, 0x80);
    snprintf(body, sizeof body,
             "CHECKPOINT TITLE %s\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "PRESS_UNTIL TITLE 0008 8 240 1000\n"
             "END\n", hexA, hexB);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "the PRESS_UNTIL timeout route loads");
    rt_sig(sigB, 0x80);
    for (uint32_t v = 0; v < 1000; v++) sr_route_step(v, sigB);
    expect(sr_route_status() == RT_RUNNING, "PRESS_UNTIL keeps trying inside its budget");
    sr_route_step(1000, sigB);
    expect(sr_route_status() == RT_FAILED, "PRESS_UNTIL that never sees its screen fails the route");

    sr_route_reset();
    rt_write("CHECKPOINT TITLE 00\nPRESS_UNTIL TITLE 0008 8 4 1000\n");
    expect(sr_route_load(RT_PATH) == 0, "a PRESS_UNTIL whose period does not exceed its width is refused");
    remove(RT_PATH);
}

static void test_route_alternate_signatures_mask_variable_content(void) {
    char hexA[1024], hexB[1024], hexC[1024], body[8192];
    uint8_t obs[576];
    int n = sr_route_sig_bytes();

    sr_route_reset();
    /* Two recordings of one screen: the first half is stable, the second half varies. */
    for (int i = 0; i < n; i++) {
        snprintf(hexA + i * 2, 3, "%02x", i < n / 2 ? 0x40 : 0x10);
        snprintf(hexB + i * 2, 3, "%02x", i < n / 2 ? 0x40 : 0xd0);
        snprintf(hexC + i * 2, 3, "%02x", 0x90);
    }
    snprintf(body, sizeof body,
             "TOLERANCE 6\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "CHECKPOINT OTHER_SCREEN %s\n"
             "WAIT MAIN_MENU 10000\n"
             "END\n", hexA, hexB, hexC);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "a checkpoint with two recordings loads");

    /* Stable half matches, variable half is a third value neither recording saw. */
    for (int i = 0; i < n; i++) obs[i] = (uint8_t)(i < n / 2 ? 0x42 : 0x77);
    sr_route_step(0, obs);
    expect(sr_route_status() == RT_DONE,
           "a screen matches on the bytes its recordings agree on, whatever varies elsewhere");

    /* A genuinely different screen must still be rejected on the stable half alone. */
    sr_route_reset();
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 1, "the masked route reloads");
    for (int i = 0; i < n; i++) obs[i] = 0x90;
    sr_route_step(0, obs);
    expect(sr_route_status() == RT_RUNNING,
           "masking the variable bytes must not make a different screen match");

    /* Two recordings that share almost nothing are not one screen, and a route built on
     * them would match anything. Refuse it rather than run a route that cannot fail. */
    sr_route_reset();
    for (int i = 0; i < n; i++) {
        snprintf(hexA + i * 2, 3, "%02x", 0x10);
        snprintf(hexB + i * 2, 3, "%02x", 0xf0);
    }
    snprintf(body, sizeof body,
             "TOLERANCE 6\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "WAIT MAIN_MENU 100\n"
             "END\n", hexA, hexB);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 0,
           "two recordings with nothing in common are refused as one checkpoint");
    expect(sr_route_status() == RT_FAILED, "an indistinguishable checkpoint fails the run");

    /* Masking is what makes this possible, so it is also what could make two checkpoints
     * collapse into each other. A route whose assertions cannot distinguish its own screens
     * cannot fail, which is worse than having no assertions. */
    sr_route_reset();
    for (int i = 0; i < n; i++) {
        int stable = i < n / 4;
        snprintf(hexA + i * 2, 3, "%02x", stable ? 0x40 : 0x10);
        snprintf(hexB + i * 2, 3, "%02x", stable ? 0x40 : 0xd0);
        snprintf(hexC + i * 2, 3, "%02x", stable ? 0x41 : 0x88);
    }
    snprintf(body, sizeof body,
             "TOLERANCE 6\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "CHECKPOINT MAIN_MENU %s\n"
             "CHECKPOINT SINGLE_PLAYER_MENU %s\n"
             "WAIT MAIN_MENU 100\n"
             "END\n", hexA, hexB, hexC);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 0,
           "a checkpoint that also matches another checkpoint's recording is refused");
    remove(RT_PATH);
}

static void test_route_malformed_files_are_refused(void) {
    char hexA[1024], body[4096];

    sr_route_reset();
    rt_hex(hexA, 0x20);
    snprintf(body, sizeof body,
             "CHECKPOINT MAIN_MENU %s\n"
             "WAIT EXHIBITION_SETUP 1000\n"
             "END\n", hexA);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 0, "a route naming an undefined checkpoint is refused");
    expect(sr_route_status() == RT_FAILED, "an unusable route file fails the run");

    sr_route_reset();
    snprintf(body, sizeof body, "CHECKPOINT MAIN_MENU 0011223344\nWAIT MAIN_MENU 100\n");
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 0, "a truncated signature is refused");

    sr_route_reset();
    snprintf(body, sizeof body,
             "CHECKPOINT MAIN_MENU %s\n"
             "SIGGRID 8 8\n"
             "WAIT MAIN_MENU 100\n", hexA);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 0, "SIGGRID after a CHECKPOINT is refused");

    sr_route_reset();
    snprintf(body, sizeof body,
             "CHECKPOINT MAIN_MENU %s\n"
             "WAIT MAIN_MENU 100\n"
             "8600 0x4000 16\n", hexA);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 0, "a file mixing a program with bare frame lines is refused");

    sr_route_reset();
    snprintf(body, sizeof body, "CHECKPOINT MAIN_MENU %s\nSTEP_SIDEWAYS 3\n", hexA);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 0, "an unknown route keyword is refused");

    sr_route_reset();
    snprintf(body, sizeof body, "CHECKPOINT MAIN_MENU %s\nTOLERANCE 4\n", hexA);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 0, "TOLERANCE after a CHECKPOINT is refused");

    sr_route_reset();
    snprintf(body, sizeof body, "CHECKPOINT MAIN_MENU %s\n", hexA);
    rt_write(body);
    expect(sr_route_load(RT_PATH) == 0, "a file of checkpoints with no steps is refused");
    expect(sr_route_status() == RT_FAILED,
           "an unfinished route must not silently fall back to the default pad pulse");
    remove(RT_PATH);
}

static void test_route_legacy_pad_script_is_unchanged(void) {
    sr_route_reset();
    rt_write("1 0x0008 8\n240 0x0008 8\n8600 0x4000 16\n");
    expect(sr_route_load(RT_PATH) == 1, "a bare pad script still loads");
    expect(sr_route_status() == RT_LEGACY,
           "a bare pad script keeps its original absolute-frame behaviour");
    expect(sr_route_step(8600, NULL) == 0u,
           "the program stepper stays inert for a legacy pad script");

    sr_route_reset();
    rt_write("# comment only\n\n");
    expect(sr_route_load(RT_PATH) == 0, "an empty route file loads nothing");
    expect(sr_route_status() == RT_OFF, "an empty route file leaves the pad unscripted");
    remove(RT_PATH);
    sr_route_reset();
}

/* sceKernelExitGame is `void sceKernelExitGame(void)`:
 *   - PSPSDK psp/sdk/include/psploadexec.h declares it with no parameter, and
 *     every psp/sdk/samples call site invokes it with no argument.
 *   - PPSSPP registers NID 0x05572a5f as WrapV_V with the empty argument
 *     signature "" (Core/HLE/sceKernel.cpp).
 * Both are CORROBORATIVE_ONLY inputs; the executable check below is the host
 * evidence. Because the call takes no argument, $a0 at entry holds whatever the
 * caller last left there, so the host process result must not be derived from
 * it. The child mode poisons all four argument registers and the parent asserts
 * the observed process result. */
#define EXITGAME_POISON 0xDEADBEEFu
#define EXITGAME_RETURNED 90   /* the syscall came back; on hardware it cannot */

static int run_exit_game_poisoned(void) {
    CpuState cpu;
    g_mem_base = (uint8_t *)calloc(1, 0x0c000000u);
    if (!g_mem_base) return 2;
    g_mem = g_mem_base + 0x08000000u;
    s_cpu = &s_cpu_store;
    sched_init(&s_cpu_store);
    sr_hle_init();
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = EXITGAME_POISON;   /* $a0 */
    cpu.r[5] = EXITGAME_POISON;   /* $a1 */
    cpu.r[6] = EXITGAME_POISON;   /* $a2 */
    cpu.r[7] = EXITGAME_POISON;   /* $a3 */
    (void)sr_syscall(&cpu, 0x05572a5fu);
    return EXITGAME_RETURNED;
}

static void test_exit_game_ignores_argument_registers(const char *self) {
    intptr_t rc;
    if (!self) {
        expect(0, "exit-game regression knows its own executable path");
        return;
    }
    rc = _spawnl(_P_WAIT, self, self, "--exit-game-poisoned", (const char *)NULL);
    expect(rc != -1, "exit-game child process starts");
    if (rc == -1) return;
    expect(rc != (intptr_t)EXITGAME_RETURNED,
           "dispatching sceKernelExitGame terminates the process instead of returning");
    expect(((unsigned long long)rc & 0xffffffffull) != (unsigned long long)EXITGAME_POISON,
           "sceKernelExitGame does not adopt a poisoned $a0 as the process result");
    expect(rc == 0,
           "a no-argument sceKernelExitGame yields a zero host process result");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--psp-oracle") == 0)
        return run_psp_oracle(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--exit-game-poisoned") == 0)
        return run_exit_game_poisoned();
    g_mem_base = (uint8_t *)calloc(1, 0x0c000000u);
    if (!g_mem_base) {
        fprintf(stderr, "hle_thread_selftest: cannot allocate guest arena\n");
        return 2;
    }
    g_mem = g_mem_base + 0x08000000u;
    s_cpu = &s_cpu_store;

    /* sched_init() performs the single sr_coro_main() adoption. There is deliberately no
     * separate adoption here: a second call would allocate a second wrapper and redefine
     * the current coroutine, which is the exact shape of the historical defect. */
    sched_init(&s_cpu_store);
    expect(s_sched_coro != NULL, "scheduler coroutine was adopted");
    expect(sr_coro_current() == s_sched_coro,
           "the adopted scheduler coroutine is the one currently running");

    test_prx_export_relocation_behavior();
    test_fd_namespace();
    test_utility_av_module_state();
    test_exit_thread_does_not_wake_launcher(0);
    test_exit_thread_does_not_wake_launcher(2);
    test_explicit_exit_status_exact((int32_t)0x800201acu, 0x800200d2u);
    test_explicit_exit_status_exact((int32_t)0x800201a8u, 0x800200d2u);
    test_explicit_exit_status_exact((int32_t)-17, 0x800200d2u);
    test_explicit_exit_status_exact(0x78, 0x78u);
    test_thread_delete_lifecycle_and_cleanup();
    test_start_thread_error_semantics();
    test_exit_delete_lifecycle_and_join_result();
    test_wait_thread_end_invalid_targets();
    test_wait_thread_end_already_ended();
    test_wait_thread_end_blocking_and_resume();
    test_wait_thread_end_cb_execution();
    test_audio_regular_contract_safety();
    test_ctrl_read_buffer_contract();
    test_ctrl_sample_timestamp_microsecond_contract();
    test_nested_guest_call_abi();
    test_ge_guest_sentinel();
    test_ge_block_transfer_span_atomicity();
    test_exit_game_ignores_argument_registers(argc > 0 ? argv[0] : NULL);
    test_bulk_guest_span_atomicity();
    test_dmac_semantics();
    test_display_framebuf_latch();
    test_time_domains_are_coherent();
    test_display_clock_reads_are_observational();
    test_delay_advances_unified_timeline();
    test_display_frame_per_sec_float_return();
    test_rtc_conversion_errors_and_full_range();
    test_bulk_clock_reads_are_side_effect_free();
    test_display_queries_do_not_progress_display();
    test_vcount_tracks_elapsed_source_periods();
    test_vcount_freezes_while_cpu_interrupts_are_masked();
    test_vcount_credits_one_deferred_period_on_resume();
    test_route_observer_waits_for_guest_scanout_state();
    test_extracted_data_prepares_before_guest_and_lookup_never_builds();
    test_unprepared_route_lookup_fails_closed_without_building();
    test_slow_enumeration_completes_before_guest_start();
    test_unapplicable_route_disables_without_scanning();
    test_missing_root_fails_once_and_stays_failed();
    test_display_setframebuf_flip_accounting();
    test_watchdog_no_new_frame_observation();
    test_watchdog_fires_on_boundary_crossing_not_exact_multiple();
    test_interrupt_nid_semantics();
    test_is_cpu_intr_suspended_is_token_predicate();
    test_dispatch_suspend_resume_nid_semantics();
    test_can_not_wait_semantics();
    test_wait_sema_count_validation();
    test_expired_timed_object_waits_enter_strict_priority();
    test_allocate_fpl_context_precedence();
    test_atrac_context_abi();
    test_atrac_stream_ring_wrap();
    test_sas_core_mix_preserves_caller_pcm();
    test_sas_state_contracts();
    test_msgpipe_safety();
    test_intr_context_conformance();

    /* Issue #64. SR_ROUTE_NO_EXIT keeps a deliberately failed route observable: in a real
     * run the same paths terminate the process with status 86 so a wrong reached state can
     * never be archived as a completed route. */
    _putenv("SR_ROUTE_NO_EXIT=1");
    test_route_program_advances_only_on_observed_state();
    test_route_wait_timeout_fails_loudly();
    test_route_expect_rejects_the_wrong_screen();
    test_route_expect_without_observation_fails_closed();
    test_route_signature_tolerance_is_enforced();
    test_route_press_until_stops_when_the_screen_arrives();
    test_route_press_while_ends_with_its_screen();
    test_route_press_until_timeout_fails_loudly();
    test_route_alternate_signatures_mask_variable_content();
    test_route_malformed_files_are_refused();
    test_route_legacy_pad_script_is_unchanged();

    check_coroutine_lifecycle();

    fprintf(stderr, "hle_thread_selftest: %d checks, %d failures\n",
            s_checks, s_failures);
    free(g_mem_base);
    return s_failures ? 1 : 0;
}
