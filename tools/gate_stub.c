// SPDX-License-Identifier: GPL-2.0-or-later
// Headless link stub for the codegen_gate / microtest driver.
//
// The microtest driver (driver.c) and recomp.c reference scheduler, GUI, GE, HLE,
// VFPU, debug, and perf symbols that live in separate translation units. On a
// host without SDL3/Vulkan or Windows headers (e.g. the Linux CI runner compiling
// only the headless runtime subset) those translation units cannot be compiled, so
// this TU provides just enough definitions to satisfy the linker.
//
// NONE of these alter an instruction-under-test result on the microtest path:
//   - The Allegrex integer/FPU test bodies never call HLE, scheduler, GE, GUI,
//     audio, VFPU fallback, or debug-watch functions. They run straight-line C
//     that terminates at the explicit `syscall` (funct 0x0C) in exit_stub().
//   - sr_sched_on stays 0, so the SR_YIELD macro in generated code is a cheap
//     no-op that never calls sr_yield, consults sr_timeslice, or enters the
//     safe-boundary service path.
//   - If a supposedly dead stub is ever reached, it returns a safe value
//     (0 / false / no-op) rather than crashing or silently skipping CPU state.
//   - sr_syscall is the only stub reached on the intended exit path: the explicit
//     syscall at the end of exit_stub(). It marks the HLE boundary and returns 0.
//
// This is NOT a silent workaround: it does not mask or skip any instruction under
// test. It only provides dead symbols so the host executable links headlessly.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
#include <atomic>
typedef std::atomic_int_least32_t atomic_int_least32_t;
#else
#include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t *g_mem;
#define SR_PHYS(a)  ((a) & 0x1FFFFFFF)
#define SR_HOST(a)  (g_mem + (int32_t)(SR_PHYS(a) - 0x08000000))
static uint32_t mem_r32(uint32_t a) {
    uint32_t v;
    memcpy(&v, SR_HOST(a), 4);
    return v;
}

/* Minimal CpuState forward-declaration sufficient for the dead stubs below.
 * The gate never calls sr_hle_call; this TU is only linked, not executed on
 * the microtest path except for the explicit syscall exit. */
typedef struct CpuState CpuState;

CpuState *s_cpu = NULL;

/* --- Scheduler stubs (sched.c / sr_coro.c) --- */
int     sr_sched_on = 0;
atomic_int_least32_t sr_timeslice = 0;
void    sr_yield(CpuState *s) { (void)s; }
/* SR_YIELD's safe-boundary service hook (#70). Same reasoning as sr_yield above:
 * sr_sched_on is 0, so neither branch of the macro is ever taken. */
atomic_int_least32_t sr_service_request = 0;
void    sr_sched_request_service(void) {}
void    sr_sched_service_only(void) {}
uint32_t sched_current_uid(void) { return 0u; }
uint32_t sched_root_uid(void)     { return 0x110u; }
uint32_t sched_worker_uid(void)   { return 0x114u; }
uint32_t sched_launcher_uid(void) { return 0x111u; }
void    sched_init(CpuState *cpu) { (void)cpu; }
uint32_t sched_terminate_thread(uint32_t uid) { (void)uid; return 0; }
void    sched_exit_current(int32_t status) { (void)status; }
void    sched_exit_current_delete(int32_t status) { (void)status; }
uint32_t sched_suspend_interrupts(void) { return 1u; }
void    sched_resume_interrupts(uint32_t state) { (void)state; }
int     sched_interrupts_enabled(void) { return 1; }
void    sched_delay_current(uint32_t usec) { (void)usec; }
void    sched_preempt(void) {}
void    sched_block_on(uint32_t obj) { (void)obj; }
void    sched_wait_vblank(void) {}
int     sched_block_on_timeout(uint32_t obj, uint32_t usec) { (void)obj; (void)usec; return 0; }
void    sched_wake(uint32_t obj) { (void)obj; }
void    sched_thread_sleep(void) {}
uint32_t sched_thread_wakeup(uint32_t uid) { (void)uid; return 0; }
void    sched_set_priority(uint32_t uid, int priority) { (void)uid; (void)priority; }
int     sched_thread_cancel_wakeup(uint32_t uid) { (void)uid; return 0; }
int     sched_thread_run_status(uint32_t uid, void *out) { (void)uid; (void)out; return 0; }
uint32_t sched_thread_exit_status(uint32_t uid) { (void)uid; return 0u; }
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) { (void)uid; (void)arglen; (void)argp; return 0; }
uint32_t sched_delete_thread(uint32_t uid) { (void)uid; return 0; }
void    sched_set_current_join_target(uint32_t uid) { (void)uid; }
void    sched_clear_current_join_target(void) {}
int     sched_take_current_join_result(uint32_t uid, uint32_t *result_out) { (void)uid; (void)result_out; return 0; }
int     sched_current_priority(void) { return 32; }
int     sched_is_dormant(uint32_t uid) { (void)uid; return 1; }
void    sched_run(uint32_t entry, uint32_t arglen, uint32_t argp) { (void)entry; (void)arglen; (void)argp; }
CpuState *sr_cpu_for_callbacks(void) { return NULL; }
void    sr_boot_probe(CpuState *s, uint32_t guest_pc) { (void)s; (void)guest_pc; }
int     sr_vblank_quantum_due(void) { return 0; }
void    sr_hle_advance_time(uint32_t us) { (void)us; }


/* --- Coroutine stubs (sr_coro.c) --- */
typedef struct SrCoro SrCoro;
SrCoro *sr_coro_main(void) { return NULL; }
SrCoro *sr_coro_create(void (*fn)(void *), void *arg, size_t stack_size) { (void)fn; (void)arg; (void)stack_size; return NULL; }
void    sr_coro_switch(SrCoro *to) { (void)to; }
void    sr_coro_destroy(SrCoro *c) { (void)c; }
SrCoro *sr_coro_current(void) { return NULL; }

/* --- HLE stubs (hle.c) --- */
void sr_trace_close(void);
uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    (void)s;
    (void)nid;
    sr_trace_close();

    const char *ppm_path = getenv("SR_PPM_DUMP");
    if (ppm_path) {
        uint32_t fb_addr = 0x09000000;
        FILE *pf = fopen(ppm_path, "wb");
        if (pf) {
            fprintf(pf, "P6\n64 64\n255\n");
            for (int i = 0; i < 64 * 64; i++) {
                uint32_t pixel = mem_r32(fb_addr + i * 4);
                uint8_t r = pixel & 0xFF;
                uint8_t g = (pixel >> 8) & 0xFF;
                uint8_t b = (pixel >> 16) & 0xFF;
                fputc(r, pf);
                fputc(g, pf);
                fputc(b, pf);
            }
            fclose(pf);
            fprintf(stderr, "[driver/gate] Framebuffer PPM dumped to %s\n", ppm_path);
        }
    }

    fflush(stdout);
    fflush(stderr);
    _Exit(0);
}

uint32_t sr_last_nid = 0;
void     sr_hle_register(uint32_t nid, const char *name, void *fn) { (void)nid; (void)name; (void)fn; }
void     sr_hle_init(void) {}
uint32_t sr_hle_resolve_late_import(uint32_t nid) { (void)nid; return 0u; }
uint32_t sr_alloc_uid(void) { return 0u; }
int      sr_hle_register_late_import(uint32_t nid, uint32_t target) { (void)nid; (void)target; return 0; }

/* --- GE stubs (ge.c / gpu_sdl3vk) --- */
uint32_t sr_get_ge_status(void) { return 0u; }
uint32_t ge_run_list(uint32_t addr, int resume) { (void)addr; (void)resume; return 0u; }
uint32_t ge_framebuffer(void) { return 0u; }
uint32_t g_ge_stall_addr = 0;
typedef struct GeGpuHooks GeGpuHooks;
GeGpuHooks *ge_hooks = NULL;
int ge_loaded = 0;

/* --- GUI stubs (gui.c) --- */
void     gui_init(const char *title) { (void)title; }
int      gui_on(void) { return 0; }
void     gui_pump(void) {}
uint32_t gui_buttons(void) { return 0u; }
void     gui_consume_button_pulses(void) {}
void     gui_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = 128; if (ly) *ly = 128; }
int      gui_pad_present(void) { return 0; }
void     gui_present(uint32_t fbaddr, int fmt, uint32_t stride) { (void)fbaddr; (void)fmt; (void)stride; }

/* --- Perf stubs (perf.c) --- */
void     sr_perf_init(void) {}

/* --- Debug / watch stubs (debug.c) --- */
uint32_t g_sr_debug = 0;
int      g_sr_metadata_watch = 0;
#define SR_MAX_MEM_WATCHES 16
typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t value;
    const char *label;
    int match_value;
} SrMemWatch;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int      g_sr_mem_watch_count = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int      g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_limit = 0;
unsigned g_sr_store_context_count = 0;
int      g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;
int      g_sr_last_writer_enabled = 0;
void     sr_debug_init_watches(void) {}
int      sr_check_mem_watch(uint32_t addr, uint32_t val, int write, uint32_t pc) {
    (void)addr; (void)val; (void)write; (void)pc;
    return 0;
}
void     sr_last_writer_reset(void) {}
void     sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
int      sr_find_last_writer(uint32_t addr, uint32_t width,
                             uint32_t *write_addr, uint32_t *write_width,
                             uint32_t *value, uint32_t *pc) {
    (void)addr; (void)width; (void)write_addr; (void)write_width; (void)value; (void)pc;
    return 0;
}

/* --- VFPU interp stub (vfpu_interp.c) --- */
#define SR_VFPU_OTHER   0
#define SR_VFPU_COMPUTE 1
#define SR_VFPU_STATE   2
int      sr_vfpu_interp(CpuState *s, uint32_t op) { (void)s; (void)op; return SR_VFPU_OTHER; }



/* --- SDL timer stubs (sched.c references) --- */
uint64_t SDL_GetTicksNS(void) { return 0u; }
void     SDL_DelayPrecise(uint64_t ns) { (void)ns; }

/* --- HLE / OSK stubs (dead on microtest path) --- */
void     sr_syscall_wrapper(uint32_t nid) { (void)nid; }

#ifdef SR_GATE_BUILD
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {
    if (code == 0x210cu) {
        sr_syscall(s, 0u);
        return;
    }
    fprintf(stderr, "Fatal error: unsupported raw MIPS syscall 0x%x in gate build at pc=0x%08x\n", code, pc);
    abort();
}
#endif

/* --- generated-entry fallback stub --- */
void     f_00304290(void *s) { (void)s; }

/* --- GE / vblank stubs (dead on microtest path) --- */
uint32_t g_frame_prims = 0u;
uint32_t sr_vblank_handler(void) { return 0u; }
int      sr_vblank_dispatch_registered(void) { return 0; }
void     sr_vblank_tick(void) {}
void     sr_vblank_nop(void) {}

#ifdef __cplusplus
}
#endif
