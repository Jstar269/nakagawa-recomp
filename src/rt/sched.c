// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.

/* *
 * PSP threads are priority-scheduled and a busy-waiting thread is preempted by its timeslice
 * so a sibling can run. The recompiled code is straight C, so to suspend a thread mid-call-
 * stack we run each guest thread on its own Windows fiber and switch between them. All threads
 * share one CpuState; on a switch its contents are saved into the outgoing thread's control
 * block and the incoming thread's are loaded, so the single register file follows whichever
 * thread is running. SR_YIELD (emitted by codegen at function entry and loop back-edges) burns
 * the timeslice and switches when it reaches zero, giving preemption without a real timer.
 *
 * This is a simplified model: highest-priority ready thread runs; equal priority round-robins;
 * sceKernelDelayThread blocks until enough yields have elapsed. It is enough to interleave the
 * boot threads the way the game's startup expects, not a cycle-accurate kernel.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "sr_coro.h"     /* portable cooperative-coroutine primitive (replaces Win32 fibers) */
#include "perf.h"

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_error.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

int     sr_sched_on = 0;
atomic_int_least32_t sr_timeslice = 0;
/* See recomp.h for the contract. Sticky, relaxed, payload-free. */
atomic_int_least32_t sr_service_request = 0;

#define TIMESLICE 1000           /* yields per thread run before preemption (smaller = more frequent vblank delivery, fixes UMD-init spin) */
#define MAXTHREADS 128

enum { TH_DORMANT = 0, TH_READY, TH_RUNNING, TH_WAIT_DELAY, TH_WAIT_OBJ };
enum { PSP_THREAD_RUNNING = 1, PSP_THREAD_READY = 2, PSP_THREAD_WAITING = 4, PSP_THREAD_STOPPED = 16 };
enum { PSP_WAIT_NONE = 0, PSP_WAIT_SLEEP = 1, PSP_WAIT_DELAY = 2, PSP_WAIT_OBJECT = 3 };

#define SCE_KERNEL_ERROR_ILLEGAL_THID      0x80020197u
#define SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT  0x800200d2u
#define SCE_KERNEL_ERROR_UNKNOWN_THID      0x80020198u
#define SCE_KERNEL_ERROR_DORMANT           0x800201a2u
#define SCE_KERNEL_ERROR_NOT_DORMANT       0x800201a4u
#define SCE_KERNEL_ERROR_THREAD_TERMINATED 0x800201acu
#define SCE_KERNEL_ERROR_WAIT_DELETE       0x800201b5u

/* PSP hardware treats a signed-negative status as an error-shaped non-delete
 * exit and latches SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT.  The boundary probe
 * measured both ThreadMan errors (0x800201a8/0x800201ac) and ordinary -17;
 * positive values propagate.  Delete/self-unload paths do not call this seam. */
static int sched_status_is_negative(int32_t status) {
    return status < 0;
}

#define SR_STACK_ARENA_FLOOR 0x05000000u
#define SR_STACK_ARENA_CEIL  0x09f00000u
#define SR_STACK_RANGE_MAX   (MAXTHREADS + 1)

typedef struct {
    uint32_t base;
    uint32_t size;
} StackRange;

typedef struct {
    uint32_t uid;
    int      state;
    int      priority;
    uint32_t entry, arglen, argp;
    SrCoro  *coro;               /* this thread's coroutine (sr_coro); NULL until first started */
    int      started;            /* coroutine has begun running its body */
    uint64_t wake;               /* scheduler tick to wake at (TH_WAIT_DELAY) */
    uint32_t wait_obj;           /* object uid this thread waits on (TH_WAIT_OBJ) */
    int      wakeups;            /* pending sceKernelWakeupThread count (sleep/wakeup semantics) */
    int      sleeping;           /* 1 while blocked in sceKernelSleepThread[CB] */
    int32_t  exit_status;        /* value passed to sceKernelExitThread */
    uint64_t vbl_seen;           /* s_vbl_count this thread last consumed (vblank latch) */
    uint32_t sp_init, k0_init;   /* initial sp/k0 (to re-seed registers on a restart) */
    CpuState saved;              /* register file while not running */
    jmp_buf  unwind_jmp;         /* unwind point for clean fiber exit */
    int      has_unwind_jmp;     /* 1 when jump buffer is valid */
    int      hle_depth;          /* HLE execution depth when suspended */
    int      is_cb_wait;         /* 1 when thread is in callback-aware wait */
    int      deleted;             /* kernel object has been removed; slot may be recycled */
    int      resources_released;  /* libc/reent/callback ownership released exactly once */
    int      stack_released;      /* guest stack reservation returned exactly once */
    int      join_waiting;        /* current syscall is waiting for join_target */
    int      join_result_valid;  /* target ended while this waiter was blocked */
    uint32_t join_target;
    uint32_t join_result;
    uint32_t stack_base;          /* user-visible stack range, excluding synthetic TLS */
    uint32_t stack_size;          /* aligned user-visible stack bytes */
    uint32_t stack_reservation;   /* user stack plus synthetic TLS reservation */
} TCB;

static TCB      s_tcb[MAXTHREADS];

typedef struct {
    uint32_t uid;
    uint32_t k0;
    uint32_t state_ptr;
    int in_use;
} HostLibcThreadRecord;

static HostLibcThreadRecord s_libc_threads[MAXTHREADS];

static int      s_ntcb = 0;
static int      s_cur = -1;      /* index of running thread, -1 = scheduler */
static int      s_last_pick = -1;/* rotation cursor (file-scope so the selftest can reset it) */
/* CPU-wide PSP interrupt state.  Because scheduling is suppressed while this is
 * false, an interrupt-disabled state cannot migrate to another guest thread. */
static int      s_interrupts_enabled = 1;
static int      s_dispatch_enabled = 1;
/* Interrupt state is a gate on delivery, not a gate on the scheduler clock.  A
 * source bit remains latched until the eligible handler consumes it. */
static uint32_t s_pending_interrupts;
static int      s_servicing_interrupts;
static SrCoro  *s_sched_coro = NULL;
CpuState *s_cpu = NULL;
static uint64_t s_tick = 0;
static uint32_t s_gp = 0x002d0000;        /* module global pointer, inherited by created threads */
static uint32_t s_stack_top = 0x09f00000;  /* sibling thread stacks grow down from here */
static StackRange s_stack_free[SR_STACK_RANGE_MAX];
static int s_stack_free_count;
static int s_stack_allocator_ready;

/* Dynamic role-UID capture. Populated by sched_create_thread: the ROOT thread is the
 * first thread ever created (sched_run's module_start thread); the WORKER is the thread
 * created with entry 0x000468c8u (main_RunGameLoop); the LAUNCHER is the boot thread at
 * entry 0x0029a174u. Defaults match the historical UID assignment (0x110 / 0x114 / 0x111)
 * so that pre-init code paths and one-off diagnostics continue to behave. They are
 * read-only to the rest of the runtime via sched_root_uid() / sched_worker_uid() /
 * sched_launcher_uid(); UID allocation has drifted between runs before (worker 0x115 ->
 * 0x114), so behavioral checks must use these accessors, never literal UIDs. */
uint32_t g_root_uid     = 0x110u;
uint32_t g_worker_uid   = 0x114u;
uint32_t g_launcher_uid = 0x111u;
static int s_root_seen  = 0;      /* first-created-thread latch (file-scope for the selftest) */

/* Defined below, after the virtual-time service and VBLANK frame are declared. */
static void scheduler_progress_time(void);
static void scheduler_latch_due_events(void);
static void scheduler_service_pending(void);
static void scheduler_add_time(uint64_t delta);
static uint64_t scheduler_deadline_after(uint64_t delta);

static TCB *tcb_by_uid(uint32_t uid) {
    for (int i = 0; i < s_ntcb; i++)
        if (!s_tcb[i].deleted && s_tcb[i].uid == uid) return &s_tcb[i];
    return NULL;
}

static TCB *tcb_by_entry(uint32_t entry) {
    for (int i = 0; i < s_ntcb; i++)
        if (!s_tcb[i].deleted && s_tcb[i].entry == entry) return &s_tcb[i];
    return NULL;
}

static uint32_t resolve_thread_uid(uint32_t uid) {
    return uid ? uid : (s_cur >= 0 ? s_tcb[s_cur].uid : 0);
}

/* Guest stacks live in a descending arena, but deletion must return the exact
 * range rather than permanently consuming the high-water mark.  A small
 * first-fit range list is sufficient here: there are at most MAXTHREADS live
 * kernel objects, and every release coalesces adjacent ranges before another
 * allocation can split them.  The allocator owns only the synthetic PSP stack
 * plus TLS reservation; the host coroutine stack is released by sr_coro_destroy. */
static void stack_ranges_update_top(void) {
    uint32_t top = SR_STACK_ARENA_FLOOR;
    for (int i = 0; i < s_stack_free_count; i++) {
        uint32_t end = s_stack_free[i].base + s_stack_free[i].size;
        if (end > top) top = end;
    }
    s_stack_top = top;
}

static void stack_ranges_reset(void) {
    s_stack_free_count = 1;
    s_stack_free[0].base = SR_STACK_ARENA_FLOOR;
    s_stack_free[0].size = SR_STACK_ARENA_CEIL - SR_STACK_ARENA_FLOOR;
    s_stack_allocator_ready = 1;
    stack_ranges_update_top();
}

static int stack_range_alloc(uint32_t size, uint32_t *base_out) {
    if (!s_stack_allocator_ready) stack_ranges_reset();
    if (!base_out || size == 0u) return 0;
    for (int i = 0; i < s_stack_free_count; i++) {
        StackRange *range = &s_stack_free[i];
        if (range->size < size) continue;
        uint32_t base = range->base + range->size - size;
        range->size -= size;
        if (range->size == 0u) {
            for (int j = i + 1; j < s_stack_free_count; j++)
                s_stack_free[j - 1] = s_stack_free[j];
            s_stack_free_count--;
        }
        *base_out = base;
        stack_ranges_update_top();
        return 1;
    }
    return 0;
}

static void stack_range_release(uint32_t base, uint32_t size) {
    if (!s_stack_allocator_ready || base == 0u || size == 0u) return;
    if (base < SR_STACK_ARENA_FLOOR || base > SR_STACK_ARENA_CEIL ||
        size > SR_STACK_ARENA_CEIL - base) {
        fprintf(stderr, "FATAL: refusing to release invalid guest stack range [0x%08x,0x%08x)\n",
                base, base + size);
        abort();
    }
    int pos = 0;
    while (pos < s_stack_free_count && s_stack_free[pos].base < base) pos++;
    if (s_stack_free_count >= SR_STACK_RANGE_MAX) {
        fprintf(stderr, "FATAL: guest stack range table exhausted while releasing [0x%08x,0x%08x)\n",
                base, base + size);
        abort();
    }
    for (int j = s_stack_free_count; j > pos; j--)
        s_stack_free[j] = s_stack_free[j - 1];
    s_stack_free[pos] = (StackRange){base, size};
    s_stack_free_count++;

    if (pos > 0) {
        StackRange *prev = &s_stack_free[pos - 1];
        StackRange *cur = &s_stack_free[pos];
        if (prev->base + prev->size == cur->base) {
            prev->size += cur->size;
            for (int j = pos + 1; j < s_stack_free_count; j++)
                s_stack_free[j - 1] = s_stack_free[j];
            s_stack_free_count--;
            pos--;
        }
    }
    if (pos + 1 < s_stack_free_count) {
        StackRange *cur = &s_stack_free[pos];
        StackRange *next = &s_stack_free[pos + 1];
        if (cur->base + cur->size == next->base) {
            cur->size += next->size;
            for (int j = pos + 2; j < s_stack_free_count; j++)
                s_stack_free[j - 1] = s_stack_free[j];
            s_stack_free_count--;
        }
    }
    stack_ranges_update_top();
}

/* sched_init: one-shot initializer for the cooperative scheduler.
 *
 * SEMANTICS: This function is called ONCE at process startup, before any guest
 * threads are created.  Calling it a second time after live coroutines/TCBs
 * exist would:
 *   - discard coroutine pointers without destroying them (fiber/stack leak);
 *   - zero the TCB table while coroutines reference it (use-after-free);
 *   - lose all role-UID captures (g_root/worker/launcher_uid) and libc records.
 *
 * It is NOT a safe runtime reset.  Tests that need a clean scheduler world
 * must use the white-box reset_sched() helper in sched_selftest.c, which
 * is NOT compiled into the production binary.
 *
 * What sched_init resets: TCB array, host libc thread table, scheduling
 *   counters (s_ntcb, s_cur, s_last_pick, s_tick), s_root_seen, interrupt
 *   state, and the sr_timeslice.
 * What sched_init intentionally does NOT reset (they must persist after init
 *   or are owned by the caller):
 *   - g_root/worker/launcher_uid (populated dynamically at create time);
 *   - g_master_reent (set during launcher registration);
 *   - none of the stack ranges survive this one-shot initialization; later
 *     deletion returns ranges through the allocator below;
 *   - s_gp (global pointer seeded by the driver from the module header);
 *   - virtual-time and pacing state (populated lazily on first vblank);
 *   - sr_sched_on (set here, never reset after init). */
void sched_init(CpuState *cpu) {
    s_cpu = cpu;
    if (cpu->r[28] != 0u) s_gp = cpu->r[28];           /* the driver seeded gp from the module's # init */
    s_sched_coro = sr_coro_main();
    sr_sched_on = 1;
    s_interrupts_enabled = 1;
    s_dispatch_enabled = 1;
    s_pending_interrupts = 0;
    s_servicing_interrupts = 0;
    atomic_store_explicit(&sr_timeslice, TIMESLICE, memory_order_relaxed);

    memset(s_tcb, 0, sizeof(s_tcb));
    memset(s_libc_threads, 0, sizeof(s_libc_threads));
    s_ntcb = 0;
    s_cur = -1;
    s_last_pick = -1;
    s_root_seen = 0;
    s_tick = 0;
    stack_ranges_reset();
}

uint32_t sched_current_uid(void) { return s_cur >= 0 ? s_tcb[s_cur].uid : 0; }
uint32_t sched_root_uid(void)     { return g_root_uid; }
uint32_t sched_worker_uid(void)    { return g_worker_uid; }
uint32_t sched_launcher_uid(void) { return g_launcher_uid; }
uint32_t sr_thread_k0(void)        { return s_cur >= 0 ? s_tcb[s_cur].k0_init : 0; }

uint32_t sched_suspend_interrupts(void) {
    uint32_t previous = s_interrupts_enabled ? 1u : 0u;
    s_interrupts_enabled = 0;
    return previous;
}

void sched_resume_interrupts(uint32_t state) {
    /* SuspendIntr returns exactly the prior I-bit (0 or 1).  Do not treat an
     * arbitrary guest value as an enable request: the hardware probe leaves
     * the CPU disabled for an invalid token such as 0xDEADBEEF, and ignoring
     * that malformed restore avoids manufacturing an interrupt transition. */
    if (state > 1u) return;
    s_interrupts_enabled = state ? 1 : 0;
    if (!s_interrupts_enabled || s_servicing_interrupts) return;

    /* Resume is a scheduler boundary: host time is sampled, elapsed source
     * events are latched, eligible handlers run in priority order, and a
     * higher-priority waiter may preempt the interrupted thread. */
    scheduler_progress_time();
    scheduler_latch_due_events();
    scheduler_service_pending();
    sched_preempt();
}

int sched_interrupts_enabled(void) {
    return s_interrupts_enabled;
}

uint32_t sched_suspend_dispatch(void) {
    if (!s_interrupts_enabled) return 0x80020066u; /* SCE_KERNEL_ERROR_CPUDI */
    uint32_t previous = s_dispatch_enabled ? 1u : 0u;
    s_dispatch_enabled = 0;
    return previous;
}

uint32_t sched_resume_dispatch(uint32_t state) {
    if (!s_interrupts_enabled) return 0x80020066u; /* SCE_KERNEL_ERROR_CPUDI */
    s_dispatch_enabled = state ? 1 : 0;
    if (s_dispatch_enabled) {
        sched_preempt();
    }
    return 0u;
}

int sched_dispatch_enabled(void) {
    return s_dispatch_enabled;
}

/* Is the CPU in a state where a thread is allowed to enter a genuine wait?
 *
 * PSPAutotests tests/intr/waits.expected records that a blocking ThreadMan call
 * returns SCE_KERNEL_ERROR_CAN_NOT_WAIT (0x800201a7) both with CPU interrupts
 * disabled and with thread dispatch disabled -- two independent states that the
 * same oracle proves are NOT aliases of each other (sceIoWaitAsyncCB L278/L279
 * and sceAudioOutputBlocking L230/L231 differ between the two columns).
 *
 * This is deliberately a STATE QUERY and nothing more. It carries no policy: it
 * does not know which APIs block, and it must never be lifted into a universal
 * pre-handler gate. waits.expected rules that out directly -- sceKernelWaitEventFlag
 * with mode 0xFF returns ILLEGAL_MODE ahead of the context error (L72/L73), and
 * sceKernelWaitThreadEnd(0) returns ILLEGAL_THID ahead of it (L204/L205). Error
 * precedence is per-API, so each handler asks this question at its own point,
 * after its own validation and only once it has established that this particular
 * invocation would genuinely block. */
int sched_wait_permitted(void) {
    return s_interrupts_enabled && s_dispatch_enabled;
}

/* Does the running thread already hold a banked sceKernelWakeupThread count?
 * sceKernelSleepThread[CB] consumes one instead of blocking, so a sleep that will
 * be satisfied from the wakeup count is not a genuine wait and is not subject to
 * the context restriction. Pure read: the count is consumed by sched_thread_sleep,
 * never here. */
int sched_current_has_pending_wakeup(void) {
    if (s_cur < 0) return 0;
    return s_tcb[s_cur].wakeups > 0;
}

/* Non-consuming counterpart to sched_take_current_join_result(). A handler that is
 * about to REJECT a join must not take the banked result on its way out, so the
 * "would this block?" question has to be answerable without side effects. */
int sched_current_join_result_pending(uint32_t uid) {
    if (s_cur < 0) return 0;
    TCB *t = &s_tcb[s_cur];
    return t->join_result_valid && t->join_target == uid;
}

void sched_raise_interrupt(uint32_t source) {
    s_pending_interrupts |= source;
}

uint32_t sched_pending_interrupts(void) {
    return s_pending_interrupts;
}

/* Accessor for the HLE callback dispatcher: the callback code in hle.c (a separate
 * translation unit) needs to mutate CpuState in interrupt context, so we expose the
 * live CpuState pointer. $gp is no longer exposed separately: callbacks inherit it
 * from the interrupted thread's context rather than a callback-global value. */
CpuState *sr_cpu_for_callbacks(void) { return s_cpu; }

/* If `addr` points at a short NUL-terminated printable string in guest RAM, copy it
 * into `out` and return 1; otherwise return 0 and leave `out` untouched.  A stalled
 * RUNNING thread is almost always looping on a name/path/id lookup, and its argument
 * registers are the only handle on WHICH name -- the pointers themselves cannot be
 * resolved once the process is gone.  Deliberately conservative: bounded length,
 * printable ASCII only, and span-validated, so a register holding an integer or a
 * struct pointer is simply not reported rather than dumping arbitrary memory. */
static int sched_guest_cstr(uint32_t addr, char *out, size_t out_sz) {
    if (addr == 0u || out_sz < 2u) return 0;
    if (!sr_guest_span_readable(addr, 1u)) return 0;
    const unsigned char *p = (const unsigned char *)SR_HOST(addr);
    size_t max = out_sz - 1u;
    for (size_t i = 0; i < max; i++) {
        if (!sr_guest_span_readable(addr + (uint32_t)i, 1u)) return 0;
        unsigned char c = p[i];
        if (c == 0u) { out[i] = 0; return i > 0u; }   /* empty string is not informative */
        if (c < 0x20u || c > 0x7eu) return 0;
        out[i] = (char)c;
    }
    return 0;   /* no terminator within the window: not a short string */
}

/* Fallback for a register that points into guest RAM but does not hold a clean
 * short string: show the leading bytes so a truncated, non-ASCII, or structured
 * buffer is still identifiable instead of vanishing from the dump. */
static int sched_guest_hexdump(uint32_t addr, char *out, size_t out_sz) {
    enum { N = 16 };
    if (addr == 0u || out_sz < (size_t)(N * 3 + 1)) return 0;
    if (!sr_guest_span_readable(addr, (uint32_t)N)) return 0;
    const unsigned char *p = (const unsigned char *)SR_HOST(addr);
    for (int i = 0; i < N; i++) snprintf(out + i * 3, 4, "%02x ", p[i]);
    out[N * 3 - 1] = 0;
    return 1;
}

/* Threads blocked in sceDisplayWaitVblankStart wait on this object; deliver_vblank readies
 * them, so the render loop draws exactly once per delivered vblank instead of spinning. */
#define VBLANK_WAIT_OBJ 0x56424c4bu   /* "VBLK" */
/* sceCtrl's blocking reads park on CTRL_WAIT_OBJ (shared via recomp.h), so a thread dump
 * can name the wait instead of printing a bare cookie. */

/* Diagnostic: dump every thread's state, entry, saved PC, and what it waits on. Reveals a thread
 * blocked on a sema/event that is never signalled (a likely scene-transition gate). */
void sched_dump_threads(void) {
    static const char *st[] = { "DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ" };
    fprintf(stderr, "--- threads (%d) cur=%d tick=%llu ---\n", s_ntcb, s_cur, (unsigned long long)s_tick);
    if (s_cpu && s_cur >= 0) {
        fprintf(stderr,
                "  live uid=0x%x pc=0x%08x ra=0x%08x sp=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x\n",
                s_tcb[s_cur].uid, s_cpu->pc, s_cpu->r[31], s_cpu->r[29],
                s_cpu->r[4], s_cpu->r[5], s_cpu->r[6], s_cpu->r[7]);
        fprintf(stderr,
                "  live s0=0x%08x s1=0x%08x s2=0x%08x s3=0x%08x s4=0x%08x s5=0x%08x s6=0x%08x s7=0x%08x\n",
                s_cpu->r[16], s_cpu->r[17], s_cpu->r[18], s_cpu->r[19],
                s_cpu->r[20], s_cpu->r[21], s_cpu->r[22], s_cpu->r[23]);
        /* Resolve whichever of those registers actually point at strings. */
        {
            static const struct { const char *name; int reg; } kRegs[] = {
                { "a0", 4 }, { "a1", 5 }, { "a2", 6 }, { "a3", 7 },
                { "s0", 16 }, { "s1", 17 }, { "s2", 18 }, { "s3", 19 },
                { "s4", 20 }, { "s5", 21 }, { "s6", 22 }, { "s7", 23 },
            };
            char buf[128];
            int printed = 0;
            for (size_t i = 0; i < sizeof kRegs / sizeof kRegs[0]; i++) {
                uint32_t v = s_cpu->r[kRegs[i].reg];
                if (sched_guest_cstr(v, buf, sizeof buf)) {
                    if (!printed) { fprintf(stderr, "  live strings:\n"); printed = 1; }
                    fprintf(stderr, "    %s=0x%08x \"%s\"\n", kRegs[i].name, v, buf);
                } else if (sched_guest_hexdump(v, buf, sizeof buf)) {
                    if (!printed) { fprintf(stderr, "  live strings:\n"); printed = 1; }
                    fprintf(stderr, "    %s=0x%08x [%s]\n", kRegs[i].name, v, buf);
                }
            }
            (void)printed;
        }
    }
    for (int i = 0; i < s_ntcb; i++) {
        TCB *t = &s_tcb[i];
        /* State alone does not say WHY a thread is parked, and "wait_obj" is a stale
         * field on a thread that is not actually in an object wait -- reading it as a
         * live wait reason has already misdirected one investigation. Report the
         * distinguishing flags, and print the deadline as a remaining duration
         * (INF for an untimed wait) so a wait that will never expire is obvious. */
        const char *why = "-";
        if (t->state == TH_WAIT_OBJ) {
            if (t->join_waiting)      why = "thread-end";
            else if (t->sleeping)     why = "sleep";
            else if (t->wait_obj == CTRL_WAIT_OBJ) why = "ctrl";
            else if (t->wait_obj == VBLANK_WAIT_OBJ) why = "vblank";
            else                      why = "object";
        } else if (t->state == TH_WAIT_DELAY) {
            why = "delay";
        }
        char deadline[32];
        uint64_t now = sched_vtime_us();
        if (t->state != TH_WAIT_OBJ && t->state != TH_WAIT_DELAY)
            snprintf(deadline, sizeof deadline, "-");
        else if (t->wake == (uint64_t)-1)
            snprintf(deadline, sizeof deadline, "INF");
        else if (t->wake > now)
            snprintf(deadline, sizeof deadline, "%lluus",
                     (unsigned long long)(t->wake - now));
        else
            snprintf(deadline, sizeof deadline, "DUE");
        fprintf(stderr,
                "  uid=0x%x entry=0x%08x pc=0x%08x %-10s pri=%d wait_obj=0x%x wake=%llu"
                " why=%s in=%s cb=%d wakeups=%d join=0x%x\n",
                t->uid, t->entry, t->saved.pc, st[t->state < 5 ? t->state : 0], t->priority,
                t->wait_obj, (unsigned long long)t->wake,
                why, deadline, t->is_cb_wait, t->wakeups,
                t->join_waiting ? t->join_target : 0u);
    }
}

/* Run the game's VBLANK interrupt handler (a guest function) on a dedicated interrupt context.
 * It usually calls sceKernelWakeupThread, readying the game thread. Called by the scheduler
 * when no thread is runnable (i.e. once per simulated frame). */
uint32_t sr_vblank_handler(void);
uint32_t sr_vblank_arg(void);
static uint64_t s_vbl_count = 0;     /* vblanks delivered so far (latch reference) */

/* The vblank is an interrupt: it can fire WHILE a thread runs (from the yield path, or while a
 * host call like a vsynced swapchain present blocks). A thread that then calls
 * sceDisplayWaitVblankStart must not sleep a whole extra period for the NEXT one -- that
 * hard-quantizes any frame whose work+present crosses the period to 30/20 fps. Latch it
 * instead: if a vblank was delivered since this thread last consumed one, return immediately
 * (consume the pending vblank); only block when none is pending. */
void sched_wait_vblank(void) {
    if (s_cur >= 0) {
        TCB *t = &s_tcb[s_cur];
        if (t->vbl_seen != s_vbl_count) { t->vbl_seen = s_vbl_count; return; }
        sched_block_on(VBLANK_WAIT_OBJ);
        t->vbl_seen = s_vbl_count;
        return;
    }
    sched_block_on(VBLANK_WAIT_OBJ);
}

/* ---- virtual time ------------------------------------------------------------------------
 * The scheduler keeps a microsecond clock (s_vtime_us) that all timed waits compare against.
 * With vblank pacing ON (default) it tracks SDL's monotonic clock, so sceKernelDelayThread and
 * timed sema/event waits elapse in REAL time -- the same timebase the paced vblanks run on.
 * (They used to be counted in scheduler "ticks" -- one tick per yield -- an elastic unit
 * that passed in microseconds while threads were busy and was jumped over when idle. Game
 * speed then depended on incidental scheduling: menus pacing via DelayThread ran 2x, and
 * mission logic threads ran a random number of iterations per frame.)
 * With SR_NOVBPACE=1 (turbo) it advances 1/59.94 s per delivered vblank and jumps over idle
 * delay waits, so everything runs as fast as the host allows. */
static uint64_t s_vtime_us = 0;
static int s_pace_on = -1;
static uint64_t s_clock_epoch_ns;
static uint64_t s_vbl_next_ns;       /* absolute SDL monotonic deadline */
static uint32_t s_vbl_period_rem;    /* rational-period remainder, denominator 60000 */
static uint32_t s_vtime_period_rem;  /* turbo-mode microsecond remainder, denominator 60000 */
static uint64_t s_vbl_next_us = 0;   /* guest-time deadline for the next VBLANK source event */
static uint32_t s_vbl_event_period_rem; /* 59.94 Hz event period carry, denominator 60000 */

/* Host monotonic-clock seam.  Every host-time read in this file goes through
 * host_now_ns() so the paced-mode contract (guest virtual time is a SAMPLE of
 * host monotonic time, never a jump ahead of it) can be asserted deterministically
 * by the white-box scheduler selftest, which #includes this file and installs a
 * controlled source.  Deliberately file-static with no exported setter: nothing
 * outside this translation unit can redirect the runtime clock, and the production
 * path is the NULL branch (a plain SDL_GetTicksNS call). */
static uint64_t (*s_host_ns_fn)(void) = NULL;
static uint64_t host_now_ns(void) {
    return s_host_ns_fn ? s_host_ns_fn() : SDL_GetTicksNS();
}

/* #70 -- the NEXT SERVICE DEADLINE, published for the host service-request advisory.
 *
 * WHICH deadline this is matters, and the two candidates are not interchangeable:
 *
 *   s_vbl_next_ns is the PRESENTATION pacing deadline consumed by vblank_pace(). That
 *     function runs only from sched_run()'s idle branch, i.e. only when no guest thread
 *     is runnable. The measured #70 route is ~98% guest CPU with idle_ms ~0.3, so under
 *     exactly the condition this mission addresses s_vbl_next_ns is not advanced at all
 *     and sits arbitrarily far in the past. Deriving a wake time from it would degenerate
 *     to a fixed-floor poll with no relationship to the display cadence. It is therefore
 *     NOT safe to publish as the service deadline.
 *
 *   s_vbl_next_us IS the rational source deadline: the single VBLANK producer advances it
 *     (and only it) through scheduler_advance_vblank_deadlines(), in exact 60000/1001
 *     steps, and it is expressed in the same host-anchored microseconds as host_us().
 *     s_clock_epoch_ns converts it back to an absolute host-monotonic instant.
 *
 * So the advisory is tied to a scheduler-owned deadline and invents no second cadence.
 * The published value is written by the runtime thread only, read by the advisory worker
 * only, and carries no other state -- one relaxed 64-bit atomic is the entire interface.
 * The advisory may act on a stale value with no correctness consequence: waking early
 * merely costs one extra authoritative host sample that finds nothing due, and waking
 * late costs nothing at all, because the runtime -- not the timer -- decides how many
 * rational periods elapsed. */
static atomic_uint_least64_t s_service_deadline_ns;

static void publish_service_deadline(void) {
    uint64_t ns;
    if (s_vbl_next_us == UINT64_MAX ||
        s_vbl_next_us > (UINT64_MAX - s_clock_epoch_ns) / 1000u)
        ns = UINT64_MAX;                       /* saturated timeline: never due again */
    else
        ns = s_clock_epoch_ns + s_vbl_next_us * 1000u;
    atomic_store_explicit(&s_service_deadline_ns, ns, memory_order_relaxed);
}

static void pace_setup(void) {
    if (s_pace_on >= 0) return;
    s_pace_on = getenv("SR_NOVBPACE") ? 0 : 1;
    fprintf(stderr, "pace_setup: s_pace_on = %d (SR_NOVBPACE = %s)\n", s_pace_on, getenv("SR_NOVBPACE"));
    fflush(stderr);
    s_clock_epoch_ns = host_now_ns();
    s_vbl_next_ns = s_clock_epoch_ns;
    s_vbl_period_rem = 0;
    s_vtime_period_rem = 0;
    s_vbl_next_us = 0;
    s_vbl_event_period_rem = 0;
    publish_service_deadline();
}

/* True when the scheduler paces vblanks to real time (the default). gui_present uses this to
 * skip its own legacy 60 Hz sleep: two independent pacers stack and push frames past the
 * vblank period (the second one then costs a whole extra frame). */
int sched_vbl_paced(void) { pace_setup(); return s_pace_on > 0; }

static uint64_t host_us(void) {
    return (host_now_ns() - s_clock_epoch_ns) / 1000u;
}

/* Observe host time without manufacturing guest time in deterministic mode.  All
 * turbo-mode advancement happens at explicit scheduler/event boundaries below. */
static void vtime_refresh(void) {
    pace_setup();
    if (s_pace_on) {
        uint64_t t = host_us();
        if (t > s_vtime_us) s_vtime_us = t;
    }
    scheduler_latch_due_events();
}

/* Advance guest time at a scheduler boundary, then latch every elapsed source
 * event.  The VBLANK source is intentionally coalescing: a long interrupt
 * suspension advances all deadlines but leaves one pending bit for delivery. */
static void scheduler_progress_time(void) {
    pace_setup();
    if (s_pace_on) {
        uint64_t t = host_us();
        if (t > s_vtime_us) s_vtime_us = t;
    } else {
        scheduler_add_time(10000u); /* deterministic scheduler quantum, never a clock-read side effect */
    }
    scheduler_latch_due_events();
}

static void scheduler_add_time(uint64_t delta) {
    if (delta > UINT64_MAX - s_vtime_us) s_vtime_us = UINT64_MAX;
    else s_vtime_us += delta;
}

static uint64_t scheduler_deadline_after(uint64_t delta) {
    return delta > UINT64_MAX - s_vtime_us ? UINT64_MAX : s_vtime_us + delta;
}

/* Return the exact rational 59.94-Hz guest-time increment for `count` source
 * periods, preserving the carry phase used by the one-period path.  The 128-bit
 * intermediate is available in the supported GCC/Clang builds and keeps a
 * long host pause from turning the monotonic clock into an O(number-of-frames)
 * catch-up loop. */
static __uint128_t scheduler_vblank_delta(uint64_t count, uint32_t rem,
                                          uint32_t *new_rem) {
    __uint128_t phase = (__uint128_t)rem + (__uint128_t)count * 20000u;
    if (new_rem) *new_rem = (uint32_t)(phase % 60000u);
    return (__uint128_t)count * 16683u + phase / 60000u;
}

static void scheduler_advance_vblank_deadlines(uint64_t count) {
    if (!count || s_vbl_next_us == UINT64_MAX) return;
    uint32_t new_rem;
    __uint128_t delta = scheduler_vblank_delta(count, s_vbl_event_period_rem, &new_rem);
    if (delta >= (__uint128_t)(UINT64_MAX - s_vbl_next_us))
        s_vbl_next_us = UINT64_MAX;
    else
        s_vbl_next_us += (uint64_t)delta;
    s_vbl_event_period_rem = new_rem;
    /* The single producer just moved its deadline: republish it for the advisory.
     * This is the ONLY place the deadline can move forward, so the published value
     * cannot describe a cadence the source did not agree to. */
    publish_service_deadline();
}

static void scheduler_latch_due_events(void) {
    while (s_vtime_us >= s_vbl_next_us && s_vbl_next_us != UINT64_MAX) {
        sched_raise_interrupt(SCHED_INTR_VBLANK);
        uint64_t distance = s_vtime_us - s_vbl_next_us;
        const __uint128_t period_numerator = (__uint128_t)1001000000u;
        __uint128_t numerator = ((__uint128_t)distance + 1u) * 60000u;
        uint64_t count = (uint64_t)((numerator + period_numerator - 1u) /
                                    period_numerator);
        if (!count) count = 1u;
        /* The ceiling above ignores the carry phase by less than one period.
         * Correct that exact candidate, without an arbitrary catch-up cap. */
        while (count < UINT64_MAX &&
               scheduler_vblank_delta(count, s_vbl_event_period_rem, NULL) <= distance)
            count++;
        while (count > 1u &&
               scheduler_vblank_delta(count - 1u, s_vbl_event_period_rem, NULL) > distance)
            count--;
        scheduler_advance_vblank_deadlines(count);
    }
}

/* Charge a sync wait-cycle's worth of virtual time to a thread waiting in a real HLE handler.
 * Real PSP syscalls cost ~1-50 us of dispatch each; charging this lets clock-driven HLE wait
 * (timed delaythread, sema/ef waits, UMD callbacks) elapse correctly even when the calling
 * recomp thread is between SR_YIELD points. In paced mode this just refreshes from the monotonic
 * clock (no-op, sub-microsecond); in turbo mode it advances the virtual clock so timers
 * fire on the next yield. */
void sr_hle_advance_time(uint32_t us) {
    pace_setup();
    if (s_pace_on) {
        /* real-time mode: nothing to do, hle handlers will sleep_until_us() on demand */
        (void)us;
    } else {
        scheduler_add_time(us);
        scheduler_latch_due_events();
    }
}

/* Public: refresh the virtual clock right before an HLE handler runs. In paced mode this is
 * a cheap monotonic-clock read; in turbo mode it would let turbo callers accelerate virtual time, but the
 * sched.c sr_yield path already handles that, so a no-op is fine. */
void sr_hle_refresh(void) {
    pace_setup();
    if (s_pace_on) {
        uint64_t t = host_us();
        if (t > s_vtime_us) s_vtime_us = t;
    }
    scheduler_latch_due_events();
}

/* Sleep (host) until the virtual clock reaches target_us. Real-time mode only. */
static void sleep_until_us(uint64_t target_us) {
    for (;;) {
        uint64_t now = host_us();
        if (now >= target_us) break;
        SDL_DelayPrecise((target_us - now) * 1000u);
    }
    vtime_refresh();
}

/* Pace vblank delivery to the PSP's real ~59.94 Hz. Without this, vblanks fire whenever
 * the scheduler goes idle, so game speed becomes "however fast the GE renders": apps that
 * flip once per vblank were rescued by gui_present's 60 Hz sleep, but apps that wait 2
 * vblanks per flip (30 fps games, some menus) ran at double speed once the GPU rasterizer
 * made the GE fast. Pacing the vblank itself makes every wait ratio correct.
 * SR_NOVBPACE=1 disables (turbo / old behaviour). */
static void vblank_pace(void) {
    pace_setup();
    if (!s_pace_on) {
        scheduler_add_time(16683u);
        s_vtime_period_rem += 20000u;
        if (s_vtime_period_rem >= 60000u) {
            scheduler_add_time(1u);
            s_vtime_period_rem -= 60000u;
        }
        return;
    }
    uint64_t now = host_now_ns();
    if (s_vbl_next_ns > now) {
        uint64_t wait_started = sr_perf_now_ns();
        SDL_DelayPrecise(s_vbl_next_ns - now);
        sr_perf_guest_idle_wait(wait_started);
        now = host_now_ns();
    }
    /* 59.94 Hz is 60000/1001 Hz. Carry the fractional nanoseconds so the
     * accumulated deadline has no floating-point or per-frame rounding drift. */
    do {
        s_vbl_next_ns += 16683333u;
        s_vbl_period_rem += 20000u;
        if (s_vbl_period_rem >= 60000u) {
            s_vbl_next_ns++;
            s_vbl_period_rem -= 60000u;
        }
        /* Skip missed presentation slots while retaining the rational phase/carry. */
    } while (now > s_vbl_next_ns);
    vtime_refresh();
}

/* Host-clock-anchored vblank watchdog. Recomp emits SR_YIELD only at function entries and
 * loop back-edges, so the worker's busy-wait on 0x310a034 fires sr_yield() extremely sparsely
 * (once every few host seconds). That causes the vblank source latch for engine_Init's callback
 * chain (cb#2) to never flip in time. We track host wall-time since the last vblank delivery
 * and compare it against s_vblank_q_us inside sr_yield.
 *
 * What that comparison DOES depends on the profile, and the split is the #70 slice B contract:
 *
 *   turbo (SR_NOVBPACE=1): there is no host-anchored virtual clock, so the quantum latches an
 *     out-of-band VBLANK source. This is turbo's only escape from a guest loop that never
 *     reaches an explicit advancement point, and it is retained deliberately.
 *
 *   paced (default): the quantum produces NOTHING. scheduler_progress_time() at the top of the
 *     same sr_yield already sampled the host clock and scheduler_latch_due_events() already
 *     latched every elapsed rational deadline from that sample, so the scheduler's rational
 *     60000/1001 source is the single producer. Raising here as well used to insert a second
 *     guest VBLANK per period at the ~16.000 ms quantum boundary, 683 us ahead of the
 *     ~16.683 ms rational one. The quantum is now read only as a diagnostic.
 *
 * Either way the pending source is serviced at the normal eligible-delivery phase, without
 * bypassing interrupt-disable state; pacing on the host-clock deadline still happens inside
 * vblank_pace() when pace_mode=1. */
static int s_vblank_q_us = -1;          /* pacing quantum us; <0 lazy-init from env */
static uint64_t s_last_vblank_ns;       /* when deliver_vblank() last ran (host clock) */
/* Paced-mode diagnostic only, and a RATE rather than an event count: it counts sr_yield calls
 * at which the host quantum had elapsed since the last delivery even though the rational source
 * had already been latched from the same host sample. That can only mean VBLANK SERVICE is
 * behind (interrupts suspended, or service re-entered), never that production is, so a long
 * suspension increments it once per yield for its whole duration. Never used to create an
 * event; see the #70 slice B note in sr_yield. */
static uint64_t s_vblank_late_service_yields;
static void vblank_pace_quantum_init(void) {
    if (s_vblank_q_us >= 0) return;
    const char *e = getenv("SR_VBLANK_Q_US");
    /* Default 16000us (16 ms = paced vblank rate): keeps PSP pacing intact for normal code paths
     * while still firing approximately once per vblank cycle. Tunable downward via env if boot-path
     * vblank-callback chain needs higher density. */
    s_vblank_q_us = e ? atoi(e) : 16000;
    s_last_vblank_ns = host_now_ns();
}
/* Returns 1 if the host wall-clock has crossed s_vblank_q_us since the last delivery. This is
 * the raw observation only; what sr_yield does with it differs by profile (see the block above
 * -- an out-of-band source in turbo, a diagnostic in paced mode).
 * Exported (recomp.h) so the SR_YIELD macro in sched.c-side callers can poll it on every
 * emit without going through sr_yield()'s slice countdown. */
int sr_vblank_quantum_due(void) {
    pace_setup();
    vblank_pace_quantum_init();
    uint64_t since_us = (host_now_ns() - s_last_vblank_ns) / 1000u;
    return since_us >= (uint64_t)s_vblank_q_us;
}
static void vblank_clock_reset(void) { s_last_vblank_ns = host_now_ns(); }

/* Microseconds of virtual time until the next vblank is due (0 when overdue). */
static uint64_t vblank_due_us(void) {
    pace_setup();
    if (!s_pace_on) return 0;
    uint64_t now = host_now_ns();
    if (now >= s_vbl_next_ns) return 0;
    return (s_vbl_next_ns - now) / 1000u;
}

static void deliver_vblank(void) {
    /* Reset the host-clock quantum so the OOB source check in sr_yield won't double-fire. */
    vblank_pace_quantum_init();
    vblank_clock_reset();
    s_vbl_count++;
    sr_perf_vblank();

    /* Increment guest-side counters (OpenGrip: 0x0031101c=FrameCounter, 0x0031105c=VSyncCounter) */
    MEM_W32(0x0031105cu, MEM_R32(0x0031105cu) + 1);
    MEM_W32(0x0031101cu, MEM_R32(0x0031101cu) + 1);

    uint32_t h = sr_vblank_handler();
    static unsigned long long vb = 0;
    extern uint32_t g_frame_prims;
    if (getenv("SR_VBLOG") && (++vb % 1) == 0) {
        fprintf(stderr, "vblank #%llu (handler=0x%x) prims=%u\n", vb, h, g_frame_prims);
    }
    g_frame_prims = 0;

    sched_wake(VBLANK_WAIT_OBJ);
    if (getenv("SR_PCSAMPLE")) {
        static unsigned long n = 0;
        if ((n++ % 30) == 0) fprintf(stderr, "PCSAMPLE frame=%lu interrupted_pc=0x%08x ra=0x%08x\n",
                                     n, s_cpu->pc, s_cpu->r[31]);
    }
    CpuState save;
    memcpy(&save, s_cpu, sizeof(CpuState));
    /* An interrupt frame is a nested call on the interrupted register file,
     * not a zeroed synthetic process. Set only the ABI fields owned by the
     * handler and restore the complete frame after it returns. */
    memcpy(s_cpu, &save, sizeof(CpuState));
    s_cpu->r[29] = 0x09df0000;          /* dedicated interrupt stack (below thread stacks) */
    s_cpu->r[28] = s_gp;
    s_cpu->r[4] = sr_vblank_arg();       /* a0 = registered arg */
    s_cpu->r[31] = 0;
    s_cpu->vfpuCtrl[0] = 0xe4; s_cpu->vfpuCtrl[1] = 0xe4;
    s_cpu->pc = h;
    int save_cur = s_cur; s_cur = -1;    /* interrupt context: SR_YIELD must not switch */
    if (h && getenv("SR_CBSNAP")) {
        /* Legacy single-slot VBLANK pre-snapshot. The entry here is the InterruptManager
         * sub-interrupt handler (sub-int 30), not a s_callbacks[] slot. */
        fprintf(stderr, "CBSNAP: legacy-pre entry=0x%08x a0=0x%08x (cpu pc=0x%08x sp=0x%08x)\n",
                h, s_cpu->r[4], save.pc, save.r[29]);
        fprintf(stderr, "  insn[entry  ]=0x%08x insn[entry+4]=0x%08x insn[entry+8]=0x%08x\n",
                MEM_R32(h), MEM_R32(h + 4), MEM_R32(h + 8));
        fprintf(stderr, "  0x310a034=0x%08x 0x002cf6b4=0x%08x libc_main=0x%08x\n",
                MEM_R32(0x310a034u), MEM_R32(0x002cf6b4u), MEM_R32(0x0030a040u));
        fflush(stderr);
    }
    if (h) dispatch(s_cpu, h);
    if (h && getenv("SR_CBSNAP")) {
        fprintf(stderr, "CBSNAP: legacy-post entry=0x%08x v0=0x%08x a0=0x%08x sp=0x%08x ra=0x%08x\n",
                h, s_cpu->r[2], s_cpu->r[4], s_cpu->r[29], s_cpu->r[31]);
        fprintf(stderr, "  0x310a034=0x%08x 0x002cf6b4=0x%08x\n",
                MEM_R32(0x310a034u), MEM_R32(0x002cf6b4u));
        fflush(stderr);
    }
    extern int sr_vblank_dispatch_registered(void);
    /* Deliver every active callback slot in stable slot/registration order.  A callback
     * may delete itself or another slot; the HLE walker snapshots each slot immediately
     * before dispatch, so later slots observe that change on this same VBLANK. */
    sr_vblank_dispatch_registered();

    s_cur = save_cur;
    memcpy(s_cpu, &save, sizeof(CpuState));
    extern void sr_vblank_tick(void);
    sr_vblank_tick();

    /* Worker relaunch trampoline: if the worker thread went DORMANT (e.g. after
     * the game loop returned), restart it on the next vblank so it re-enters
     * f_000468c8 and submits the next frame. This is the runtime surrogate for
     * the real PSP's GE list-complete callback that re-arms the game loop. */
    {
        static int relaunch_disabled = -1;
        if (relaunch_disabled < 0) relaunch_disabled = getenv("SR_NO_RELAUNCH") ? 1 : 0;
        if (!relaunch_disabled) {
            uint32_t wuid = g_worker_uid;
            TCB *w = tcb_by_uid(wuid);
            if (w && w->state == TH_DORMANT) {
                /* A dormant worker means main_RunGameLoop returned for this frame; on real
                 * PSP the GE list-complete callback re-arms it. The previous gate required
                 * the 0x331b80 frame-ready counter to be exactly 0, but that counter's
                 * polarity is owned by the guest's finish callback (ge.c GE_FINISH_CB ->
                 * func 0x599c) and is not reliably reset here -- so a dormant worker could
                 * be held forever, presenting only frame 0. Re-arm whenever the worker is
                 * dormant; the relaunch is the surrogate for the GE callback. */
                uint32_t ge_ctr = MEM_R32(0x00331b80u);
                fprintf(stderr, "RELAUNCH: worker 0x%x dormant (counter=%u), restarting to entry 0x%08x\n", wuid, ge_ctr, w->entry);
                sched_start_thread(wuid, 0, 0);
            }
        }
    }
}

/* Service only sources whose delivery semantics are implemented. Unknown
 * source bits deliberately remain pending rather than being silently dropped;
 * adding a source handler later cannot lose an event raised today. */
static void scheduler_service_pending(void) {
    if (!s_interrupts_enabled || s_servicing_interrupts) return;
    s_servicing_interrupts = 1;
    while (s_interrupts_enabled && (s_pending_interrupts & SCHED_INTR_VBLANK)) {
        s_pending_interrupts &= ~SCHED_INTR_VBLANK;
        deliver_vblank();
    }
    s_servicing_interrupts = 0;
}

void sr_sched_request_service(void) {
    atomic_store_explicit(&sr_service_request, 1, memory_order_relaxed);
}

/* #70 -- the service-only scheduler phase.
 *
 * This runs on the SAME runtime/guest host thread as the generated code that reached the
 * safe boundary, so every scheduler variable it touches is still single-threaded. It is
 * NOT sr_yield(): it answers only "is host-timed scheduler work due?", never "has this
 * thread run long enough to rotate?". Concretely it must not, and does not:
 *
 *   - reset or otherwise write sr_timeslice (the macro's one decrement per boundary is
 *     the whole quantum accounting);
 *   - rotate equal-priority peers, or let a lower-priority peer run;
 *   - assign CpuState.pc, or advance s_tick / the yield-path diagnostics;
 *   - fabricate a VBLANK, increment s_vbl_count, or move s_vbl_next_us -- only the
 *     rational source below scheduler_latch_due_events() may do that.
 *
 * The one control-flow effect it may have is the existing strict-priority rule: if the
 * authoritative host sample promotes a numerically STRONGER thread, sched_preempt() takes
 * the CPU away exactly as it already does from sched_resume_interrupts(). The call
 * sequence below is deliberately identical to that function's, so there is one eligible-
 * boundary shape in the runtime rather than two:
 *
 *     host sample -> s_vtime_us -> rational latch -> pending -> eligible delivery -> preempt
 *
 * Eligibility is tested BEFORE the request is consumed. An ineligible boundary therefore
 * DEFERS the request rather than losing it -- the sticky flag survives, and the next
 * eligible boundary (or sched_resume_interrupts, which does this same work) services it.
 * Both tests are plain loads of runtime-thread-owned ints, so a long interrupt-suspended
 * or recursive-service region costs one branch per safe boundary and reads no host clock.
 * That recursion test is also what stops a request raised from inside a VBLANK handler
 * from re-entering delivery: scheduler_service_pending()/deliver_vblank() run with
 * s_servicing_interrupts set. */
void sr_sched_service_only(void) {
    if (!s_interrupts_enabled || s_servicing_interrupts) return;
    atomic_store_explicit(&sr_service_request, 0, memory_order_relaxed);
    scheduler_progress_time();
    scheduler_latch_due_events();
    scheduler_service_pending();
    sched_preempt();
}

/* ---- host service-request advisory ---------------------------------------------------
 *
 * The sticky flag above is inert without a truthful producer: nothing in a CPU-bound
 * guest sets it. This worker is that producer, and it is a WAKEUP HINT, never a VBLANK
 * source. Its entire authority is one relaxed store of 1 into sr_service_request. It
 * reads only SDL_GetTicksNS() and two atomics; it never touches CpuState, s_cur, any TCB,
 * s_vtime_us, s_vbl_next_us, s_vbl_next_ns, s_vbl_count, s_pending_interrupts,
 * sched_raise_interrupt(), deliver_vblank(), any coroutine, guest memory, or callback
 * state. It deliberately does NOT go through host_now_ns(): that seam's function pointer
 * is runtime-thread-owned test state, and the worker must not read it.
 *
 * Because the count of events is decided entirely by the runtime thread re-sampling the
 * authoritative clock and advancing every elapsed rational deadline, this worker's timing
 * cannot change how many VBLANKs exist:
 *   - waking EARLY costs one extra authoritative sample that finds nothing due;
 *   - waking LATE (host scheduling jitter, a suspend/resume, a stalled runtime) collapses
 *     into one sticky request, and the runtime then advances all elapsed periods itself.
 *
 * Turbo (SR_NOVBPACE=1) has no host-anchored virtual clock, so the advisory is not started
 * at all there and the flag is never set: the safe-boundary fast path stays a single
 * predicted-not-taken load and turbo semantics are unchanged. SR_NOSERVICEHINT=1 disables
 * the worker in paced mode too, so the same binary can be run with and without it. */
#define SR_SERVICE_HINT_FLOOR_NS   500000ull   /* never spin: every iteration sleeps */
#define SR_SERVICE_HINT_MAX_NS    4000000ull   /* bounds shutdown latency and stale-deadline drift */

static SDL_Thread *s_service_hint_thread = NULL;
static atomic_int_least32_t s_service_hint_stop;
static int s_service_hint_disabled = -1;

static int SDLCALL service_hint_worker(void *unused) {
    (void)unused;
    while (!atomic_load_explicit(&s_service_hint_stop, memory_order_relaxed)) {
        uint64_t now = SDL_GetTicksNS();
        uint64_t deadline = atomic_load_explicit(&s_service_deadline_ns, memory_order_relaxed);
        uint64_t wait;
        if (now >= deadline) {
            sr_sched_request_service();
            wait = SR_SERVICE_HINT_FLOOR_NS;
        } else {
            wait = deadline - now;
            if (wait > SR_SERVICE_HINT_MAX_NS) wait = SR_SERVICE_HINT_MAX_NS;
            if (wait < SR_SERVICE_HINT_FLOOR_NS) wait = SR_SERVICE_HINT_FLOOR_NS;
        }
        SDL_DelayNS(wait);
    }
    return 0;
}

/* Created exactly once, by sched_run() before the first guest thread is resumed. The
 * s_service_hint_thread guard makes a repeated start a no-op, so no reset/re-init path
 * can produce a duplicate worker. Failure to create is not fatal: the runtime simply
 * keeps the pre-#70 timeslice-only service cadence. */
static void service_hint_start(void) {
    if (s_service_hint_thread) return;
    if (!sched_vbl_paced()) return;                  /* turbo: no host anchor, no advisory */
    if (s_service_hint_disabled < 0)
        s_service_hint_disabled = getenv("SR_NOSERVICEHINT") ? 1 : 0;
    if (s_service_hint_disabled) return;
    atomic_store_explicit(&s_service_hint_stop, 0, memory_order_relaxed);
    publish_service_deadline();                      /* never let the worker read an unset deadline */
    s_service_hint_thread = SDL_CreateThread(service_hint_worker, "sr-service-hint", NULL);
    if (!s_service_hint_thread)
        fprintf(stderr, "sched: service-request advisory unavailable (%s); "
                        "falling back to timeslice-only service\n", SDL_GetError());
}

/* Stop and JOIN before sched_run() returns, i.e. before the driver runs any teardown and
 * long before sdl3vk_shutdown()/SDL_Quit(). After the join no further request can be
 * raised, so the flag is cleared here and the runtime is left exactly as it would be if
 * the advisory had never existed. */
static void service_hint_stop(void) {
    if (!s_service_hint_thread) return;
    atomic_store_explicit(&s_service_hint_stop, 1, memory_order_relaxed);
    SDL_WaitThread(s_service_hint_thread, NULL);
    s_service_hint_thread = NULL;
    atomic_store_explicit(&sr_service_request, 0, memory_order_relaxed);
}

static void coro_body(void *param) {
    TCB *t = (TCB *)param;
    for (;;) {
        /* Entry into the thread body: set up args and the standard return address (0). */
        s_cpu->r[4] = t->arglen;
        s_cpu->r[5] = t->argp;
        s_cpu->r[31] = 0;
        if (t->uid == g_worker_uid) fprintf(stderr, "DISPATCH uid=0x%x entry=0x%08x\n", t->uid, t->entry);
        t->has_unwind_jmp = 1;
        if (setjmp(t->unwind_jmp) == 0) {
            dispatch(s_cpu, t->entry);    /* runs until the thread returns or exits */
        } else {
            /* longjmp path: sched_unwind_current() was called from recomp.c */
            if (t->uid == g_worker_uid) fprintf(stderr, "FIBER_UNWIND: uid=0x%x cleanly unwound\n", t->uid);
        }
        t->has_unwind_jmp = 0;
        if (t->uid == g_worker_uid) fprintf(stderr, "DISPATCH uid=0x%x returned pc=0x%08x\n", t->uid, s_cpu->pc);
        /* The entry returned (or was longjmp-unwound) without calling sceKernelExitThread.
         * sched_exit_current applies the measured non-delete ThreadMan exit rule: a positive
         * thread-body return is recorded unchanged, while a signed-negative return is latched
         * as ILLEGAL_ARGUMENT. It then releases sceKernelWaitThreadEnd
         * joiners, unregisters the libc thread state, and marks the TCB
         * DORMANT, and switches to the scheduler. (The old tail only flipped the state
         * flag: joiners blocked on this uid were stranded forever and exit_status kept the
         * NOT_DORMANT sentinel.) If this parked coroutine is ever resumed again, the loop
         * re-enters the body -- but a restart recreates the coroutine, so that resume path
         * is defensive only. */
        if (s_cur >= 0 && &s_tcb[s_cur] == t) {
            sched_exit_current((int32_t)s_cpu->r[2]);
        } else {
            fprintf(stderr, "coro_body: uid=0x%x returned outside its own schedule slot "
                    "(s_cur=%d) -- parking DORMANT without exit bookkeeping\n", t->uid, s_cur);
            t->state = TH_DORMANT;
            sr_coro_switch(s_sched_coro);
        }
    }
}

uint32_t g_master_reent = 0x002cf338u; // fallback to default global reent

/* Host-side registry of live guest threads, keyed by k0, holding (k0, state_ptr) pairs in
 * host memory instead of guest RAM. This avoids colliding with the guest's module/EH-metadata
 * registry at 0x0030a040. */
extern uint32_t sr_newlib_malloc(uint32_t size, uint32_t guest_ra);
extern void sr_callback_unregister_owner(uint32_t thread_uid);

/* guest_reent_register: insert (uid, state_ptr) into the guest per-thread reent/state hash
 * rooted at 0x0030aa88.  Used by the host to pre-populate the table for threads that do not
 * call the original f_00011710 themselves.  The translated f_00011710 returns -1 on
 * duplicate UID; this host helper silently overwrites (idempotent refresh on restart). */
static void guest_reent_register(uint32_t uid, uint32_t state_ptr) {
    uint32_t bucket = uid % 32;
    /* Per-thread reent/state hash root at 0x0030aa88 (BSS static).
     * Layout: { next(+0x00), state_ptr[32](+0x04..+0x80), uid[32](+0x84..+0x100) }
     * Bucket = uid % 32 (signed-safe for positive PSP thread UIDs). */
    uint32_t node_addr = 0x0030aa88u;

    if (getenv("SR_REENT_TRACE")) {
        fprintf(stderr, "REENT_TRACE guest_register: uid=0x%x state_ptr=0x%x bucket=%u\n",
                uid, state_ptr, bucket);
    }

    for (;;) {
        uint32_t key = MEM_R32(node_addr + 0x84u + bucket * 4u);

        if (key == 0u || key == uid) {
            /* Free slot or same-UID refresh: write key and state_ptr. */
            MEM_W32(node_addr + 0x84u + bucket * 4u, uid);
            MEM_W32(node_addr + 0x04u + bucket * 4u, state_ptr);
            return;
        }

        /* Slot occupied by a different UID (hash collision): follow the chain. */
        uint32_t next = MEM_R32(node_addr);
        if (next == 0u) {
            /* End of chain; allocate a new node (0x104 bytes to match f_00011710's
             * f_00010738 allocation). */
            next = sr_newlib_malloc(260, 0);
            if (next == 0u) {
                fprintf(stderr, "FATAL: guest_reent_register: sr_newlib_malloc failed to allocate hash node\n");
                abort();
            }
            for (uint32_t offset = 0; offset < 260; offset += 4) {
                MEM_W32(next + offset, 0u);
            }
            MEM_W32(node_addr, next);
        }
        node_addr = next;
    }
}

/* guest_reent_unregister: remove the entry for (uid derived from state_ptr+0x37c) from
 * the per-thread reent/state hash at 0x0030aa88.  Zeroes both the uid key and state_ptr
 * slots so f_00011600 no longer finds this thread. */
static void guest_reent_unregister(uint32_t state_ptr) {
    if (!sr_inrange(state_ptr)) return;
    uint32_t uid = MEM_R32(state_ptr + 0x37cu);
    if (uid == 0) return;

    uint32_t bucket = uid % 32;
    uint32_t node_addr = 0x0030aa88u;

    if (getenv("SR_REENT_TRACE")) {
        fprintf(stderr, "REENT_TRACE guest_unregister: uid=0x%x state_ptr=0x%x bucket=%u\n",
                uid, state_ptr, bucket);
    }

    while (node_addr != 0u) {
        uint32_t key = MEM_R32(node_addr + 0x84u + bucket * 4u);
        if (key == uid) {
            MEM_W32(node_addr + 0x84u + bucket * 4u, 0u);
            MEM_W32(node_addr + 0x04u + bucket * 4u, 0u);
            return;
        }
        node_addr = MEM_R32(node_addr);
    }
}

static void init_guest_reent(uint32_t state_ptr, uint32_t uid) {
    /* Copy master thread's reent structure to initialize the new thread's reent.
     * This inherits the initialized allocator context. The g_root_uid and
     * g_launcher_uid threads keep their own independently-initialized reent:
     * root has the CRT-provided master reent; launcher initializes its own via
     * f_000118a0 in its guest entry (f_0029a174). */
    if (uid != g_root_uid && uid != g_launcher_uid) {
        if (g_master_reent != 0u && sr_inrange(g_master_reent) && sr_inrange(g_master_reent + 1024u) &&
            (MEM_R32(g_master_reent) != 0u || MEM_R32(g_master_reent + 4u) != 0u)) {
            for (uint32_t offset = 0; offset < 1024; offset += 4) {
                MEM_W32(state_ptr + offset, MEM_R32(g_master_reent + offset));
            }
            /* +0x148 is a self-pointer field within the reent structure (points to
             * a sub-structure at +0x14c). The copied value from the master reent
             * still points into the master's buffer; rewrite it to this thread's
             * equivalent offset. All other pointer fields in the 0x400-byte copy
             * that are relative-to-reent are similarly remapped by the guest's own
             * initializer (f_000118a0) when the thread entry runs. */
            MEM_W32(state_ptr + 0x148u, state_ptr + 0x14cu);
        } else {
            fprintf(stderr, "WARNING: init_guest_reent: g_master_reent=0x%08x unmapped or "
                    "zero -- leaving new thread uid=0x%x reent at zero\n",
                    g_master_reent, uid);
        }
    }
    /* Write the thread UID to state_ptr + 0x37c after the copy resolves.
     * f_00011600 (per-thread reent lookup) reads this field to identify the current
     * thread UID before walking the 0x0030aa88 hash table. */
    MEM_W32(state_ptr + 0x37cu, uid);
    if (getenv("SR_REENT_TRACE")) {
        fprintf(stderr, "REENT_TRACE init_guest_reent: uid=0x%x state_ptr=0x%x uid_at_37c=0x%x\n",
                uid, state_ptr, MEM_R32(state_ptr + 0x37cu));
    }
}

/* register_libc_thread: record (k0, state_ptr, uid) in the host-owned s_libc_threads table
 * and initialize the thread's reent structure (init_guest_reent), then pre-register the thread
 * in the guest per-thread reent/state hash at 0x0030aa88 for all threads except the launcher
 * (the launcher's guest entry f_0029a174 calls f_00011710 itself and must not see a pre-existing
 * entry -- f_00011710 returns -1 on duplicate and would cause the launcher to exit early).
 *
 * Returns 0 on success, -1 if the host table is full (structurally impossible in production
 * because s_libc_threads has MAXTHREADS slots and there can never be more live records than
 * live TCBs; the -1 path exists so the selftest can exercise exhaustion without aborting). */
static int register_libc_thread(uint32_t k0, uint32_t state_ptr, uint32_t uid) {
    int slot = -1;
    for (int i = 0; i < MAXTHREADS; i++) {
        if (s_libc_threads[i].in_use && s_libc_threads[i].k0 == k0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAXTHREADS; i++) {
            if (!s_libc_threads[i].in_use) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            fprintf(stderr, "ERROR: Host libc thread table full (MAXTHREADS=%d), could not register thread uid=0x%x\n", MAXTHREADS, uid);
            return -1;
        }
        s_libc_threads[slot].uid = uid;
        s_libc_threads[slot].k0 = k0;
        s_libc_threads[slot].state_ptr = state_ptr;
        s_libc_threads[slot].in_use = 1;
    } else {
        s_libc_threads[slot].uid = uid;
        s_libc_threads[slot].state_ptr = state_ptr;
    }

    if (uid == g_launcher_uid) {
        g_master_reent = state_ptr;
    }

    init_guest_reent(state_ptr, uid);

    /* Pre-register in the guest per-thread reent/state hash (0x0030aa88) for all
     * threads except the launcher, which calls f_00011710 (the original registration
     * function) from its own guest entry f_0029a174 and must find an empty slot. */
    if (uid != g_launcher_uid) {
        guest_reent_register(uid, state_ptr);
    }

    if (getenv("SR_THLOG")) {
        fprintf(stderr, "DEBUG: Registered thread uid=0x%x in host libc table slot %d: k0=0x%x state_ptr=0x%x\n",
                uid, slot, k0, state_ptr);
    }
    if (getenv("SR_REENT_TRACE")) {
        uint32_t bucket = (uid == 0u) ? 0u : (uid % 32u);
        fprintf(stderr, "REENT_TRACE host_register: uid=0x%x k0=0x%x state_ptr=0x%x bucket=%u slot=%d\n",
                uid, k0, state_ptr, bucket, slot);
    }
    return 0;
}

static TCB *tcb_by_uid(uint32_t uid);
static TCB *tcb_by_entry(uint32_t entry);
static uint32_t sched_create_thread_finish(TCB *t, uint32_t entry, int priority, uint32_t stack_size);

static void unregister_libc_thread(uint32_t k0) {
    for (int i = 0; i < MAXTHREADS; i++) {
        if (s_libc_threads[i].in_use && s_libc_threads[i].k0 == k0) {
            uint32_t state_ptr = s_libc_threads[i].state_ptr;
            uint32_t uid = s_libc_threads[i].uid;
            if (getenv("SR_REENT_TRACE")) {
                uint32_t bucket = (uid == 0u) ? 0u : (uid % 32u);
                fprintf(stderr, "REENT_TRACE host_unregister: uid=0x%x k0=0x%x state_ptr=0x%x bucket=%u slot=%d\n",
                        uid, k0, state_ptr, bucket, i);
            }
            guest_reent_unregister(state_ptr);
            s_libc_threads[i].uid = 0;
            s_libc_threads[i].k0 = 0;
            s_libc_threads[i].state_ptr = 0;
            s_libc_threads[i].in_use = 0;
            if (getenv("SR_THLOG")) {
                fprintf(stderr, "DEBUG: Unregistered thread k0=0x%x from host libc table slot %d\n", k0, i);
            }
            return;
        }
    }
}

/* Thread-owned host resources have one owner and one release point.  Exit and
 * termination release the libc/reent/callback state, while DeleteThread also
 * returns the guest stack range.  The flags live on the TCB so a repeated
 * lifecycle call is an observable no-op instead of a second teardown. */
static void sched_release_thread_resources(TCB *t) {
    if (!t || t->resources_released) return;
    if (t->k0_init) unregister_libc_thread(t->k0_init);
    sr_callback_unregister_owner(t->uid);
    t->resources_released = 1;
}

static void sched_release_thread_stack(TCB *t) {
    if (!t || t->stack_released) return;
    stack_range_release(t->stack_base, t->stack_reservation);
    t->stack_released = 1;
}

uint32_t sched_create_thread(uint32_t entry, int priority, uint32_t stack_size) {
    if (entry == 0x000468c8u && !getenv("SR_NO_THREAD_REUSE")) {
        TCB *existing = tcb_by_entry(entry);
        if (existing) {
            static int n_reuse_log = 0;
            if (n_reuse_log < 16 || getenv("SR_THLOG")) {
                fprintf(stderr,
                        "sched_create_thread: reusing uid=0x%x for entry=0x%08x state=%d started=%d\n",
                        existing->uid, entry, existing->state, existing->started);
                n_reuse_log++;
            }
            return existing->uid;
        }
    }
    if (s_ntcb >= MAXTHREADS) {
        /* TCB-slot reclaim: terminated (DORMANT) threads never released their slot,
         * so a long session that repeatedly starts/exits short-lived threads (callback
         * service threads, transient game state workers) exhausted MAXTHREADS=128.
         * Walk the table for a DORMANT slot, recycle its TCB (its fiber was already
         * freed by sched_exit_current / sched_terminate_thread), and reuse it
         * instead of declining the create. */
        int reused = -1;
        for (int i = 0; i < s_ntcb; i++) {
            if (s_tcb[i].deleted && s_tcb[i].state == TH_DORMANT) {
                /* Confirm no thread is still blocked on this uid (would otherwise
                 * strand a WaitThreadEnd waiter forever). Cheap scan because this is
                 * a rare path under pressure. */
                int someone_waits = 0;
                for (int j = 0; j < s_ntcb; j++) {
                    if (j == i) continue;
                    if ((s_tcb[j].state == TH_WAIT_OBJ) && s_tcb[j].wait_obj == s_tcb[i].uid) {
                        someone_waits = 1; break;
                    }
                }
                if (someone_waits) continue;
                reused = i;
                break;
            }
        }
        if (reused < 0) {
            fprintf(stderr, "sched_create_thread: MAXTHREADS(%d) exhausted (entry=0x%08x)\n", MAXTHREADS, entry);
            return 0;
        }
        /* Free the stale coroutine (defensive: a test fixture may have left it attached).
         * We re-seed the whole TCB below, so this is the only side effect that has to survive
         * the memset. */
        if (s_tcb[reused].coro) { sr_coro_destroy(s_tcb[reused].coro); s_tcb[reused].coro = NULL; }
        TCB *t = &s_tcb[reused];
        memset(t, 0, sizeof(*t));
        t->uid = sr_alloc_uid();
        t->state = TH_DORMANT;
        t->priority = priority;
        t->entry = entry;
        t->started = 0;
        t->coro = NULL;
        return sched_create_thread_finish(t, entry, priority, stack_size);
    }
    if (getenv("SR_THLOG")) fprintf(stderr, "create thread #%d entry=0x%08x pri=%d stack=%u\n", s_ntcb, entry, priority, stack_size);
    TCB *t = &s_tcb[s_ntcb++];
    memset(t, 0, sizeof(*t));
    t->uid = sr_alloc_uid();
    t->state = TH_DORMANT;
    t->priority = priority;
    t->entry = entry;
    t->started = 0;
    t->coro = NULL;
    return sched_create_thread_finish(t, entry, priority, stack_size);
}

/* Continuation of sched_create_thread: stack/UID seeding + libc/reent registration.
 * Split out from the main function so the MAXTHREADS-reclaim path can share it
 * without a goto-forward-over-init. Returns the new thread's uid. */
static uint32_t sched_create_thread_finish(TCB *t, uint32_t entry, int priority, uint32_t stack_size) {
    (void)priority;
    /* Stack fit check FIRST: a create whose stack cannot be carved out of the descending
     * arena (s_stack_top, floor 0x05000000 above VRAM/eDRAM) must FAIL, before any role
     * capture or registration side effect. Real PSP sceKernelCreateThread returns
     * SCE_KERNEL_ERROR_NO_MEMORY when the stack cannot be allocated; silently handing the
     * thread a smaller stack than requested (the old behavior clamped, e.g. 64 KiB
     * requested -> 4 KiB granted) guarantees a later, far-harder-to-diagnose stack
     * overflow into foreign allocations. The 0 return maps to NO_MEMORY in h_CreateThread.
     *
     * Stack ranges are returned only by DeleteThread/ExitDeleteThread after the
     * thread is no longer runnable.  Interleaved live stacks remain untouched;
     * the range allocator coalesces only exact adjacent free extents. */
    uint32_t sz = stack_size ? stack_size : 0x40000;
    const uint32_t tls_size = 0x800u;
    if (sz > UINT32_MAX - 0xffu) {
        fprintf(stderr, "sched_create_thread: stack_size=0x%08x overflows alignment\n", sz);
        t->state = TH_DORMANT;
        t->entry = 0;
        t->started = 0;
        if (t == &s_tcb[s_ntcb - 1]) s_ntcb--;
        return 0;
    }
    uint32_t user_sz = (sz + 0xffu) & ~0xffu;
    if (user_sz > UINT32_MAX - tls_size) {
        fprintf(stderr, "sched_create_thread: stack_size=0x%08x overflows TLS reservation\n", sz);
        t->state = TH_DORMANT;
        t->entry = 0;
        t->started = 0;
        if (t == &s_tcb[s_ntcb - 1]) s_ntcb--;
        return 0;
    }
    uint32_t reservation = user_sz + tls_size;
    uint32_t user_base = 0;
    if (!stack_range_alloc(reservation, &user_base)) {
        fprintf(stderr, "sched_create_thread: stack_size=0x%08x exceeds available "
                "free guest stack ranges below s_stack_top=0x%08x (entry=0x%08x) -- failing create "
                "(maps to SCE_KERNEL_ERROR_NO_MEMORY)\n",
                sz, s_stack_top, entry);
        t->state = TH_DORMANT;
        t->entry = 0;
        t->started = 0;
        if (t == &s_tcb[s_ntcb - 1]) s_ntcb--;   /* tail slot: fully release it */
        return 0;
    }
    /* Capture the role UIDs dynamically. The root is the first thread ever created
     * (sched_run's module_start thread); the worker is the thread whose entry is
     * main_RunGameLoop (0x000468c8u); the launcher is the boot thread at 0x0029a174u.
     * UID allocation has drifted once already (worker moved from 0x115 to 0x114) and
     * silently broke every hardcoded check in hle.c/recomp.c — recording the actual
     * assigned UIDs here lets those checks use sched_root_uid()/sched_worker_uid()/
     * sched_launcher_uid() instead of literals. */
    if (!s_root_seen) {
        s_root_seen = 1;
        g_root_uid = t->uid;
        if (getenv("SR_THLOG")) fprintf(stderr, "ROOT_UID_CAPTURE: root uid=0x%x\n", t->uid);
    }
    if (entry == 0x000468c8u) {
        g_worker_uid = t->uid;
        if (getenv("SR_THLOG")) fprintf(stderr, "WORKER_UID_CAPTURE: worker uid=0x%x\n", t->uid);
    } else if (entry == 0x0029a174u) {
        g_launcher_uid = t->uid;
        if (getenv("SR_THLOG")) fprintf(stderr, "LAUNCHER_UID_CAPTURE: launcher uid=0x%x\n", t->uid);
    }
    /* Priority-inversion guard: HST's launcher (entry 0x0029a174) runs an unconditional
     * recompiled `j L_0029a27c` loop in module_start that calls f_0000ef40 (modtable walk)
     * with thousands of SR_YIELD escapes per second. The launcher thread is normally the
     * highest-priority user thread (32 < worker pri ~38-40), so the scheduler always
     * schedules it, starving the worker thread (entry 0x468c8 / main_RunGameLoop)
     * that needs to drive SetFrameBuf / WaitVblank. Once the launcher uid is known, demote
     * it below any probable worker priority so worker can run. Disable with
     * SR_NO_LAUNCHER_DEMOTE=1 (back to original behaviour). */
    if (!getenv("SR_NO_LAUNCHER_DEMOTE") && entry == 0x0029a174u) {
        const int demoted_priority = 50;
        t->priority = demoted_priority;
        if (getenv("SR_THLOG")) fprintf(stderr, "LAUNCHER_DEMOTE: launcher uid=0x%x -> priority=%d\n",
                                        t->uid, demoted_priority);
    }
    /* Give the thread a stack, the module gp, and a per-thread k0 (r26) area. The PSP kernel
     * sets k0 to a small per-thread control region near the top of the thread stack, and the
     * game's thread bodies use it as a base pointer (e.g. sw r21,4(k0)); leaving it 0 faults.
     * (The entry thread's saved state is overwritten with the driver's seed in sched_run.)
     * sz/user_sz/tls_size were validated by the fit check at the top of this function.
     * The requested PSP stack is entirely user-accessible.  Keep the runtime's
     * synthetic k0/newlib TLS block in a separate reservation above it; placing
     * the 0x800-byte TLS block inside an explicitly requested 0x800-byte stack
     * left small worker threads with SP below their own allocation. */
    uint32_t k0 = user_base + user_sz;
    t->stack_base = user_base;
    t->stack_size = user_sz;
    t->stack_reservation = reservation;
    t->stack_released = 0;
    t->resources_released = 0;
    t->k0_init = k0;
    t->sp_init = (k0 - 0x10) & ~0xFu;              /* sp grows down below the k0 region */
    t->saved.r[26] = t->k0_init;
    t->saved.r[29] = t->sp_init;
    t->saved.r[28] = s_cpu && s_cpu->r[28] ? s_cpu->r[28] : s_gp;
    t->saved.pc = entry;                         /* BUG1 fix: must seed pc=entry; otherwise dispatch target=entry sees s->pc=0 */
    t->saved.vfpuCtrl[0] = 0xe4; t->saved.vfpuCtrl[1] = 0xe4; t->saved.vfpuCtrl[2] = 0;

    /* Seed the thread's TLS region (k0 + 4) to point to its own private reentrancy
     * structure space (allocated within the k0 region at k0 + 0x10) to prevent cross-thread
     * stack corruption. We zero-initialize the reentrancy structure to clear stack garbage
     * and set up the stdin/stdout/stderr pointers mimicking Newlib's __reent_init. */
    uint32_t state_ptr = k0 + 0x10;
    memset(SR_HOST(state_ptr), 0, 0x380);
    MEM_W32(state_ptr + 0x04, state_ptr + 0x268);
    MEM_W32(state_ptr + 0x08, state_ptr + 0x2c4);
    MEM_W32(state_ptr + 0x0c, state_ptr + 0x320);
    MEM_W32(k0 + 4, state_ptr);
    /* Seed the thread UID into the kernel thread info structure at state_ptr + 0x37c.
     * f_00011600 (per-thread reent lookup) reads MEM[state_ptr + 0x37c] to get the
     * current thread's UID, then walks the per-thread reent/state hash at 0x0030aa88
     * to find the matching state pointer.  Without this seed the UID read returns
     * garbage and every lookup fails. */
    MEM_W32(state_ptr + 0x37cu, t->uid);

    fprintf(stderr, "DEBUG: sched_create_thread uid=0x%x entry=0x%x k0=0x%x state_ptr=0x%x val=0x%x\n",
            t->uid, entry, k0, state_ptr, MEM_R32(k0 + 4));

    /* Write stack bounds and state_ptr for exception handling */
    MEM_W32(k0 + 0, user_base);
    MEM_W32(k0 + 8, state_ptr);

    /* Register in the host-owned s_libc_threads table and (for non-launcher threads)
     * pre-populate the guest per-thread reent/state hash at 0x0030aa88 so that
     * f_00011600 can locate this thread's reent before the thread's own entry runs.
     * NOTE: 0x0030a040 is the separate module/EH-metadata registry (not touched here). */
    register_libc_thread(k0, state_ptr, t->uid);

    /* Re-seed the thread UID into the kernel thread info structure at state_ptr + 0x37c.
     * register_libc_thread() did a 1KB copy from the master reent into state_ptr for any
     * non-root, non-launcher UID, which CLOBBERED our earlier seed above. The libc reent
     * lookup (f_00011600) reads MEM[state_ptr + 0x37c] for the UID, so we must
     * write it again AFTER the copy. */
    MEM_W32(state_ptr + 0x37cu, t->uid);

    /* Phase-3 integrity check: confirm both the kernel thread-info UID slot (f_00011600
     * reads this) and the hash-bucket return slot actually landed in the guest hash table. */
    {
        static uint32_t s_noisy_set[64] = {0};
        int idx = (t->uid >> 4) & 63;
        uint32_t bit = 1u << (t->uid & 31);
        if (!(s_noisy_set[idx] & bit)) {
            s_noisy_set[idx] |= bit;
            uint32_t bucket = t->uid % 32;
            uint32_t node_addr = 0x0030aa88u;
            uint32_t found_ptr = 0;
            while (node_addr != 0u) {
                uint32_t key = MEM_R32(node_addr + 0x84u + bucket * 4u);
                if (key == t->uid) {
                    found_ptr = MEM_R32(node_addr + 0x04u + bucket * 4u);
                    break;
                }
                node_addr = MEM_R32(node_addr);
            }
            if (t->uid != g_launcher_uid && found_ptr != state_ptr) {
                fprintf(stderr, "THREAD_SEED_MISMATCH: uid=0x%x found_ptr=0x%08x expected=0x%08x\n",
                        t->uid, found_ptr, state_ptr);
            } else {
                fprintf(stderr, "THREAD_SEED_OK: uid=0x%x state=0x%08x in guest hash\n", t->uid, state_ptr);
            }
        }
    }

    return t->uid;
}

uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) {
    if (uid == 0) return SCE_KERNEL_ERROR_ILLEGAL_THID;
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return SCE_KERNEL_ERROR_UNKNOWN_THID;
    if (t->state != TH_DORMANT)
        return SCE_KERNEL_ERROR_NOT_DORMANT;
    if (getenv("SR_THLOG")) fprintf(stderr, "start thread uid=0x%x entry=0x%08x pri=%d arglen=%u%s\n",
                                    t->uid, t->entry, t->priority, arglen, t->started ? " (restart)" : "");
    /* PSP semantics: starting a DORMANT thread that ran before restarts it from its entry.
     * The old fiber is parked wherever the thread last gave up the CPU -- for a thread that
     * exited via sceKernelExitThread that is deep inside the exit syscall, so resuming it
     * would fall through past the exit into garbage (this stranded the BGM streamer thread
     * and with it the mission scene-switch fade). Throw the old fiber away and re-seed the
     * register file so the scheduler re-enters the body fresh. */
    if (t->started && t->state == TH_DORMANT) {
        if (t->coro) { sr_coro_destroy(t->coro); t->coro = NULL; }
        t->started = 0;
        t->wakeups = 0; t->sleeping = 0; t->wait_obj = 0; t->wake = 0;
        memset(&t->saved, 0, sizeof(t->saved));
        t->saved.r[26] = t->k0_init;
        t->saved.r[29] = t->sp_init;
        t->saved.r[28] = s_gp;
        t->saved.vfpuCtrl[0] = 0xe4; t->saved.vfpuCtrl[1] = 0xe4; t->saved.vfpuCtrl[2] = 0;
        t->saved.pc = t->entry;
    }
    /* sceKernelStartThread copies the caller-supplied argument block onto the new
     * thread's stack.  Passing argp through verbatim leaves a worker pointing at
     * the creator's live stack; by the time the worker is scheduled that stack
     * may contain unrelated return addresses or locals.  HST's character-loader
     * exposed exactly that lifetime bug: its four-byte resource-manager argument
     * had become a text/code address before the loader thread dereferenced it.
     *
     * Keep the copy below the thread's normal initial SP and start the entry below
     * the copy, so an ordinary downward-growing prologue cannot overwrite it.
     * Sixteen-byte rounding preserves the PSP ABI stack alignment. */
    t->arglen = arglen;
    t->argp = 0;
    t->exit_status = (int32_t)0x800201a4u; /* SCE_KERNEL_ERROR_NOT_DORMANT */
    t->saved.r[29] = t->sp_init;
    if (arglen != 0u) {
        uint32_t rounded = (arglen + 15u) & ~15u;
        uint32_t stack_base = MEM_R32(t->k0_init + 0u);
        uint32_t src_phys = SR_PHYS(argp);
        uint32_t dst = t->sp_init - rounded;
        uint32_t dst_phys = SR_PHYS(dst);
        int size_ok = arglen <= 0x0c000000u && rounded >= arglen && rounded <= t->sp_init;
        int src_ok = size_ok && src_phys < 0x0c000000u && arglen <= 0x0c000000u - src_phys;
        int dst_ok = size_ok && dst >= stack_base && dst_phys < 0x0c000000u &&
                     rounded <= 0x0c000000u - dst_phys;
        if (argp != 0u && src_ok && dst_ok) {
            /* memmove also handles the uncommon case where a restarted thread
             * passes an argument block already resident on its own stack. */
            memmove(SR_HOST(dst), SR_HOST(argp), arglen);
            if (rounded > arglen)
                memset(SR_HOST(dst + arglen), 0, rounded - arglen);
            t->argp = dst;
            t->saved.r[29] = dst;
            if (getenv("SR_THLOG") || getenv("SR_ARGLOG"))
                fprintf(stderr,
                        "THREAD_ARG_COPY: uid=0x%x len=%u src=0x%08x dst=0x%08x sp=0x%08x\n",
                        t->uid, arglen, argp, dst, t->saved.r[29]);
        } else {
            fprintf(stderr,
                    "sched_start_thread: invalid argument block uid=0x%x len=%u "
                    "src=0x%08x stack=[0x%08x,0x%08x)\n",
                    t->uid, arglen, argp, stack_base, t->sp_init);
            t->arglen = 0;
        }
    }
    if (register_libc_thread(t->k0_init, t->k0_init + 0x10, t->uid) != 0) {
        t->state = TH_DORMANT;
        return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */
    }
    t->resources_released = 0;
    t->join_waiting = 0;
    t->join_result_valid = 0;
    t->join_target = 0;
    t->join_result = 0;
    t->state = TH_READY;
    return 0;
}

/* A timed wait whose deadline has passed -- an elapsed sceKernelDelayThread, or a timed
 * sema/event wait that timed out -- describes a thread the kernel owes the CPU to, not a
 * blocked one. Promoting it is therefore part of EVERY scheduling decision, not a private
 * step of thread selection: sched_preempt() has to see it too, or an expired thread with a
 * numerically stronger priority sits behind a running weaker one until that thread happens
 * to yield or block (#70 slice C). Idempotent, and deliberately state-only: it decides
 * nothing about who runs next, it only restores the truth about who is runnable. */
static void sched_promote_expired_waits(void) {
    for (int i = 0; i < s_ntcb; i++)
        if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
            s_vtime_us >= s_tcb[i].wake)
            s_tcb[i].state = TH_READY;   /* delay expired, or a timed wait timed out */
}

/* Pick the highest-priority runnable thread (lowest PSP priority number). Wakes delayed
 * threads whose deadline has passed.
 *
 * PSP scheduling is STRICT priority: a READY thread never runs while a READY thread with a
 * numerically lower priority exists. There is no aging/anti-starvation on hardware -- a
 * busy higher-priority thread legitimately starves lower-priority threads. (An earlier
 * "anti-starvation" rotation here forced the first OTHER ready thread -- of any priority --
 * every third decision, which let a demoted priority-50 launcher preempt the priority-3x
 * worker; that violated the model and is gone. Do not reintroduce it: if a route livelocks
 * on a busy-wait that hardware would satisfy, fix the subsystem that fails to produce the
 * awaited state.)
 *
 * A timed wait whose deadline has passed is a RUNNABLE thread, so promoting it is part
 * of every scheduling decision -- not just this one. See sched_promote_expired_waits().
 *
 * Equal-priority peers round-robin deterministically: the scan starts one slot after the
 * previous winner, so among READY threads at the best priority the next one in cyclic slot
 * order wins. Selection depends only on TCB states and the rotation cursor -- identical
 * state yields an identical decision. Returns an index or -1 if nothing is runnable. */
static int pick_next(void) {
    sched_promote_expired_waits();
    int best_pri = 0;
    int have_ready = 0;
    for (int i = 0; i < s_ntcb; i++) {
        if (s_tcb[i].state != TH_READY) continue;
        if (!have_ready || s_tcb[i].priority < best_pri) best_pri = s_tcb[i].priority;
        have_ready = 1;
    }
    if (!have_ready) return -1;
    int start = (s_last_pick >= 0) ? (s_last_pick + 1) % s_ntcb : 0;
    for (int step = 0; step < s_ntcb; step++) {
        int i = (start + step) % s_ntcb;
        if (s_tcb[i].state == TH_READY && s_tcb[i].priority == best_pri) {
            s_last_pick = i;
            return i;
        }
    }
    return -1;   /* unreachable: have_ready guarantees a match above */
}

/* Save the running thread's registers, return to the scheduler, which selects and resumes the
 * next thread. Called from a thread fiber. */
static void switch_to_scheduler(void) {
    sr_coro_switch(s_sched_coro);
}

/* Phase 2.B: SR_SPINLOG watchdog. Detects every-yield-at-same-PC starvation and
 * dumps the trapped thread's registers so we can identify which guest loop is
 * holding the scheduler. Default OFF (env-gated). Threshold N from SR_SPIN_N,
 * defaults to 200000 yields. Fires once per (uid,pc) pair to avoid trace flood. */
static int s_spin_on = -1;
static int s_spin_thr = 200000;

static int guest_ptr_readable(uint32_t p) {
    uint32_t phys = p & 0x1fffffffu;
    /* Diagnostic pointers are expected to reference guest objects/strings, never
     * the ELF header or first code page. Reject the null-page offsets too so a
     * null object plus a field offset cannot masquerade as a readable pointer. */
    return phys >= 0x1000u && phys < 0x0c000000u;
}

static void boot_diag_string(const char *label, uint32_t p) {
    char text[81];
    int i = 0;
    if (!guest_ptr_readable(p)) {
        fprintf(stderr, " %s=<invalid:0x%08x>", label, p);
        return;
    }
    for (; i < 80; i++) {
        unsigned char c = MEM_R8(p + (uint32_t)i);
        if (c == 0) break;
        text[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    text[i] = '\0';
    fprintf(stderr, " %s@0x%08x=\"%s\"", label, p, text);
}

/* SR_BOOT_DIAG: bounded probes for the current resource-table boot path. Unlike
 * SR_SPINLOG, this intentionally recognizes the alternating list-search -> strcmp
 * call cycle. It observes guest state only; it never changes control flow or RAM. */
static void boot_diag(CpuState *s) {
    static int enabled = -1;
    static unsigned list_hits, strcmp_hits, table_hits, parse_hits, parse_samples;
    static unsigned text_lookup_hits;
    static unsigned render_hits, render_entry_hits, render_finish_hits, render_hook_hits;
    if (enabled < 0) {
        const char *e = getenv("SR_BOOT_DIAG");
        enabled = e && strcmp(e, "0") != 0;
        if (enabled) fprintf(stderr, "BOOT_DIAG enabled (read-only, bounded)\n");
    }
    if (!enabled || s_cur < 0 || s_tcb[s_cur].uid != g_worker_uid) return;

    if (s->pc == 0x001039d8u && guest_ptr_readable(s->r[5]) &&
        MEM_R8(s->r[5]) == 'l' && MEM_R8(s->r[5] + 1u) == 'i' &&
        MEM_R8(s->r[5] + 2u) == '_' && text_lookup_hits++ < 12u) {
        uint32_t container = s->r[4];
        uint32_t base = guest_ptr_readable(container + 4u) ? MEM_R32(container + 4u) : 0u;
        uint32_t count = guest_ptr_readable(container + 8u) ? MEM_R32(container + 8u) : 0u;
        fprintf(stderr,
                "BOOT_DIAG text-lookup hit=%u tick=%llu container=0x%08x base=0x%08x count=%u ra=0x%08x",
                text_lookup_hits, (unsigned long long)s_tick, container, base, count, s->r[31]);
        boot_diag_string("query", s->r[5]);
        fprintf(stderr, "\n");
        uint32_t sample_count = count < 8u ? count : 8u;
        for (uint32_t i = 0; i < sample_count; i++) {
            uint32_t slot = base + i * 4u;
            uint32_t item = guest_ptr_readable(slot) ? MEM_R32(slot) : 0u;
            uint32_t name_slot = item + 0xc0u;
            uint32_t name = guest_ptr_readable(name_slot) ? MEM_R32(name_slot) : 0u;
            fprintf(stderr, "  item[%u] slot=0x%08x object=0x%08x nameptr=0x%08x",
                    i, slot, item, name);
            boot_diag_string("name", name);
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    } else if ((s->pc == 0x0008250cu || s->pc == 0x00082530u) && render_hook_hits++ < 24u) {
        fprintf(stderr,
                "BOOT_DIAG render-hook hit=%u tick=%llu pc=0x%08x hook0=0x%08x hook1=0x%08x ra=0x%08x\n",
                render_hook_hits, (unsigned long long)s_tick, s->pc,
                MEM_R32(0x0033316cu), MEM_R32(0x00333170u), s->r[31]);
    } else if (s->pc == 0x0003dfd0u && render_entry_hits++ < 8u) {
        uint32_t owner = s->r[5];
        uint32_t kind = guest_ptr_readable(owner + 0x14u) ? MEM_R32(owner + 0x14u) : 0u;
        uint32_t table_entry = 0x002bbe74u + kind * 4u;
        fprintf(stderr,
                "BOOT_DIAG render-entry hit=%u tick=%llu owner=0x%08x head=0x%08x tail=0x%08x kind=%u count=%u finalize=0x%08x\n",
                render_entry_hits, (unsigned long long)s_tick, owner,
                guest_ptr_readable(owner + 4u) ? MEM_R32(owner + 4u) : 0u,
                guest_ptr_readable(owner + 12u) ? MEM_R32(owner + 12u) : 0u,
                kind,
                guest_ptr_readable(owner + 0x24u) ? MEM_R32(owner + 0x24u) : 0u,
                guest_ptr_readable(table_entry) ? MEM_R32(table_entry) : 0u);
    } else if (s->pc == 0x0003d828u && render_finish_hits++ < 8u) {
        uint32_t owner = s->r[4];
        uint32_t kind = guest_ptr_readable(owner + 0x14u) ? MEM_R32(owner + 0x14u) : 0u;
        uint32_t table_entry = 0x002bbe74u + kind * 4u;
        fprintf(stderr,
                "BOOT_DIAG render-finalize hit=%u tick=%llu owner=0x%08x head=0x%08x tail=0x%08x kind=%u count=%u target=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x\n",
                render_finish_hits, (unsigned long long)s_tick, owner,
                guest_ptr_readable(owner + 4u) ? MEM_R32(owner + 4u) : 0u,
                guest_ptr_readable(owner + 12u) ? MEM_R32(owner + 12u) : 0u,
                kind,
                guest_ptr_readable(owner + 0x24u) ? MEM_R32(owner + 0x24u) : 0u,
                guest_ptr_readable(table_entry) ? MEM_R32(table_entry) : 0u,
                s->r[5], s->r[6], s->r[7]);
    } else if (s->pc == 0x0003e050u && render_hits++ < 32u) {
        uint32_t node = s->r[16];
        fprintf(stderr,
                "BOOT_DIAG render-list hit=%u tick=%llu node=0x%08x next=0x%08x command=0x%08x callback=0x%08x owner=0x%08x\n",
                render_hits, (unsigned long long)s_tick, node,
                guest_ptr_readable(node + 4u) ? MEM_R32(node + 4u) : 0,
                guest_ptr_readable(node + 8u) ? MEM_R32(node + 8u) : 0,
                guest_ptr_readable(node + 12u) ? MEM_R32(node + 12u) : 0,
                s->r[18]);
    } else if (s->pc == 0x0019668cu && list_hits++ < 24u) {
        uint32_t node = s->r[16];
        uint32_t ctx = s->r[21];
        uint32_t query = guest_ptr_readable(ctx) ? MEM_R32(ctx) : 0;
        uint32_t key = guest_ptr_readable(node + 0x0cu) ? MEM_R32(node + 0x0cu) : 0;
        fprintf(stderr, "BOOT_DIAG list hit=%u tick=%llu node=0x%08x next=0x%08x prev=0x%08x key=0x%08x ctx=0x%08x query=0x%08x",
                list_hits, (unsigned long long)s_tick, node,
                guest_ptr_readable(node) ? MEM_R32(node) : 0,
                guest_ptr_readable(node + 4u) ? MEM_R32(node + 4u) : 0,
                key, ctx, query);
        boot_diag_string("query", query);
        boot_diag_string("key", key);
        fprintf(stderr, "\n");
    } else if (s->pc == 0x00014934u && strcmp_hits++ < 24u) {
        fprintf(stderr, "BOOT_DIAG strcmp hit=%u tick=%llu", strcmp_hits,
                (unsigned long long)s_tick);
        boot_diag_string("a0", s->r[4]);
        boot_diag_string("a1", s->r[5]);
        fprintf(stderr, "\n");
    } else if (s->pc == 0x0019357cu && table_hits++ < 24u) {
        uint32_t desc = s->r[16];
        fprintf(stderr, "BOOT_DIAG table hit=%u tick=%llu desc=0x%08x count=%u base=0x%08x index=%u offset=0x%08x\n",
                table_hits, (unsigned long long)s_tick, desc,
                guest_ptr_readable(desc) ? MEM_R32(desc) : 0,
                guest_ptr_readable(desc + 4u) ? MEM_R32(desc + 4u) : 0,
                s->r[18], s->r[17]);
    } else if (s->pc == 0x00015fb4u) {
        /* f_00015fb4 is newlib's re-entrant strtol: a0=reent, a1=input,
         * a2=endptr, a3=base.  Sample the early calls and then sparsely sample
         * long-running parsing so a repeated caller remains diagnosable. */
        parse_hits++;
        if (parse_hits <= 16u || ((parse_hits & 0x7ffu) == 0u && parse_samples < 32u)) {
            parse_samples++;
            fprintf(stderr, "BOOT_DIAG parse hit=%u tick=%llu ra=0x%08x reent=0x%08x",
                    parse_hits, (unsigned long long)s_tick, s->r[31], s->r[4]);
            boot_diag_string("input", s->r[5]);
            fprintf(stderr, " endptr=0x%08x base=%u\n", s->r[6], s->r[7]);
        }
    }
    if (render_hits == 32u || list_hits == 24u || strcmp_hits == 24u || table_hits == 24u || parse_hits == 16u)
        fflush(stderr);
}

void sr_boot_probe(CpuState *s, uint32_t guest_pc) {
    static int enabled = -1;
    static unsigned walker_hits, finalize_hits, queue_entry_hits;
    static unsigned text_table_hits, named_table_hits;
    static uint64_t strtol_scan_hits;
    static uint64_t queue_iter_hits;
    if (enabled < 0) {
        const char *e = getenv("SR_BOOT_DIAG");
        enabled = e && strcmp(e, "0") != 0;
    }
    if (!enabled || !s) return;

    if (guest_pc == 0x001039d8u && text_table_hits < 24u) {
        uint32_t container = s->r[4], query = s->r[5];
        int layout_constructor_lookup = s->r[31] == 0x0021af28u;
        int list_item_lookup = guest_ptr_readable(query) && MEM_R8(query) == 'l' &&
            MEM_R8(query + 1u) == 'i' && MEM_R8(query + 2u) == '_';
        if (layout_constructor_lookup || list_item_lookup) {
            unsigned hit = ++text_table_hits;
            uint32_t base = guest_ptr_readable(container + 4u) ? MEM_R32(container + 4u) : 0u;
            uint32_t count = guest_ptr_readable(container + 8u) ? MEM_R32(container + 8u) : 0u;
            fprintf(stderr,
                    "BOOT_DIAG text-table-enter hit=%u container=0x%08x base=0x%08x count=%u limit=%d ra=0x%08x",
                    hit, container, base, count, (int32_t)s->r[6], s->r[31]);
            boot_diag_string("query", query);
            fprintf(stderr, "\n");
            uint32_t samples = count < 8u ? count : 8u;
            for (uint32_t i = 0; i < samples; i++) {
                uint32_t slot = base + i * 4u;
                uint32_t item = guest_ptr_readable(slot) ? MEM_R32(slot) : 0u;
                uint32_t name = guest_ptr_readable(item + 0xc0u) ? MEM_R32(item + 0xc0u) : 0u;
                fprintf(stderr, "  item[%u] object=0x%08x nameptr=0x%08x", i, item, name);
                boot_diag_string("name", name);
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    } else if (guest_pc == 0x001026b8u && named_table_hits < 24u) {
        uint32_t container = s->r[4], query = s->r[5];
        if (guest_ptr_readable(query) && MEM_R8(query) == 'l' &&
            MEM_R8(query + 1u) == 'i' && MEM_R8(query + 2u) == '_') {
            unsigned hit = ++named_table_hits;
            uint32_t base = guest_ptr_readable(container + 0x10u) ? MEM_R32(container + 0x10u) : 0u;
            uint32_t count = guest_ptr_readable(container + 0x14u) ? MEM_R32(container + 0x14u) : 0u;
            fprintf(stderr,
                    "BOOT_DIAG named-table-enter hit=%u container=0x%08x base=0x%08x count=%u ra=0x%08x",
                    hit, container, base, count, s->r[31]);
            boot_diag_string("query", query);
            fprintf(stderr, "\n");
            uint32_t samples = count < 8u ? count : 8u;
            for (uint32_t i = 0; i < samples; i++) {
                uint32_t slot = base + i * 4u;
                uint32_t item = guest_ptr_readable(slot) ? MEM_R32(slot) : 0u;
                uint32_t name = guest_ptr_readable(item + 0x10u) ? MEM_R32(item + 0x10u) : 0u;
                fprintf(stderr, "  item[%u] object=0x%08x nameptr=0x%08x", i, item, name);
                boot_diag_string("name", name);
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    } else if (guest_pc == 0x000705b0u) {
        unsigned hit = ++queue_entry_hits;
        if (hit <= 24u) {
            uint32_t queue = s->r[4];
            uint32_t table = MEM_R32(0x00331c50u);
            uint32_t slot = table + (queue << 4);
            fprintf(stderr,
                    "BOOT_DIAG queue-enter hit=%u queue=%u table=0x%08x slot=0x%08x count=%u index=%u capacity=%u ra=0x%08x\n",
                    hit, queue, table, slot,
                    guest_ptr_readable(slot + 4u) ? MEM_R32(slot + 4u) : 0u,
                    guest_ptr_readable(slot + 12u) ? MEM_R32(slot + 12u) : 0u,
                    MEM_R32(0x00331c54u), s->r[31]);
            fflush(stderr);
        }
    } else if (guest_pc == 0x000705e4u) {
        uint64_t hit = ++queue_iter_hits;
        uint32_t slot = s->r[16];
        uint32_t count = guest_ptr_readable(slot + 4u) ? MEM_R32(slot + 4u) : 0u;
        if (hit <= 16u || (hit & (hit - 1u)) == 0u) {
            fprintf(stderr,
                    "BOOT_DIAG queue-drain hit=%llu slot=0x%08x count=%u index=%u capacity=%u ra=0x%08x\n",
                    (unsigned long long)hit, slot, count,
                    guest_ptr_readable(slot + 12u) ? MEM_R32(slot + 12u) : 0u,
                    MEM_R32(0x00331c54u), s->r[31]);
            fflush(stderr);
        }
    } else if (guest_pc == 0x000160e8u) {
        uint64_t hit = ++strtol_scan_hits;
        /* First few iterations establish the token, then powers of two show
         * unbounded growth without flooding the log. r15 is the original input
         * and r9 is newlib strtol's current cursor in f_00015fb4. */
        if (hit <= 16u || (hit & (hit - 1u)) == 0u) {
            uint32_t start = s->r[15], cursor = s->r[9];
            fprintf(stderr,
                    "BOOT_DIAG strtol-scan hit=%llu start=0x%08x cursor=0x%08x delta=%u char=0x%02x ra=0x%08x\n",
                    (unsigned long long)hit, start, cursor, cursor - start,
                    MEM_R8(cursor), s->r[31]);
            fflush(stderr);
        }
    } else if (guest_pc == 0x0003dfd0u && walker_hits++ < 16u) {
        uint32_t owner = s->r[4], list = s->r[5];
        fprintf(stderr,
                "BOOT_DIAG command-walk hit=%u owner=0x%08x list=0x%08x mode=%u head=0x%08x tail=0x%08x count=%u owner_list=0x%08x ra=0x%08x\n",
                walker_hits, owner, list,
                guest_ptr_readable(list + 0x18u) ? MEM_R32(list + 0x18u) : 0u,
                guest_ptr_readable(list + 4u) ? MEM_R32(list + 4u) : 0u,
                guest_ptr_readable(list + 12u) ? MEM_R32(list + 12u) : 0u,
                guest_ptr_readable(list + 0x24u) ? MEM_R32(list + 0x24u) : 0u,
                guest_ptr_readable(owner + 0xe0u) ? MEM_R32(owner + 0xe0u) : 0u,
                s->r[31]);
    } else if (guest_pc == 0x0003d828u && finalize_hits++ < 16u) {
        uint32_t list = s->r[4];
        uint32_t kind = guest_ptr_readable(list + 0x14u) ? MEM_R32(list + 0x14u) : 0u;
        uint32_t table_entry = 0x002cbe74u + kind * 4u;
        fprintf(stderr,
                "BOOT_DIAG command-finalize hit=%u list=0x%08x kind=%u head=0x%08x tail=0x%08x count=%u target=0x%08x\n",
                finalize_hits, list, kind,
                guest_ptr_readable(list + 4u) ? MEM_R32(list + 4u) : 0u,
                guest_ptr_readable(list + 12u) ? MEM_R32(list + 12u) : 0u,
                guest_ptr_readable(list + 0x24u) ? MEM_R32(list + 0x24u) : 0u,
                guest_ptr_readable(table_entry) ? MEM_R32(table_entry) : 0u);
    }
}

static void spin_check(CpuState *s) {
    if (s_spin_on < 0) {
        const char *e = getenv("SR_SPINLOG");
        s_spin_on = e ? 1 : 0;
        const char *t = getenv("SR_SPIN_N");
        if (t) { int v = atoi(t); if (v > 0) s_spin_thr = v; }
    }
    if (!s_spin_on || s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    /* Per-thread streak tracking. The previous single global prev_uid/prev_pc reset the
     * streak on every context switch, so any workload where a sibling thread stays READY
     * (the normal boot state: the launcher never blocks) capped the observable streak at
     * the anti-starvation rotation length (~3 quanta) and the diagnostic could never
     * latch -- the "SR_SPINLOG did not fire" gap noted in ISSUES.md. Track one streak per
     * TCB slot instead, and re-fire every s_spin_thr crossings (max 8 dumps per (uid,pc))
     * so a long-running loop yields PROGRESSION samples (s0/s1 deltas across dumps show
     * whether the loop index advances), not a single ambiguous shot. */
    static struct { uint32_t pc; unsigned long long streak; } s_streak[MAXTHREADS];
    static struct { uint32_t uid, pc; int fires; } fired[128]; static int n_fired = 0;
    /* The saved-PC may diverge from `s->pc` (live PC): when sr_yield returns via the
     * !other path, t->saved is NOT re-saved, so the live `s->pc` is the authoritative
     * "where the thread last yielded from" for bypass routing. */
    uint32_t cur_pc = s->pc ? s->pc : t->saved.pc;
    if (s_streak[s_cur].pc == cur_pc) s_streak[s_cur].streak++;
    else { s_streak[s_cur].pc = cur_pc; s_streak[s_cur].streak = 1; }
    if (s_streak[s_cur].streak < (unsigned long long)s_spin_thr) return;
    s_streak[s_cur].streak = 0;               /* rearm: next dump after s_spin_thr more */
    int slot = -1;
    for (int i = 0; i < n_fired; i++) if (fired[i].uid == t->uid && fired[i].pc == cur_pc) { slot = i; break; }
    if (slot < 0) {
        if (n_fired >= 128) return;
        slot = n_fired++;
        fired[slot].uid = t->uid; fired[slot].pc = cur_pc; fired[slot].fires = 0;
    }
    if (fired[slot].fires >= 8) return;
    fired[slot].fires++;
    fprintf(stderr, "SPIN[%d]: uid=0x%x pc=0x%08x ra=0x%08x after %u yields at same PC\n",
            fired[slot].fires, t->uid, cur_pc, s->r[31], s_spin_thr);
    fprintf(stderr, "  v0=0x%08x a0=0x%08x a1=0x%08x s0=0x%08x s1=0x%08x s2=0x%08x k0=0x%08x sp=0x%08x\n",
            s->r[2], s->r[4], s->r[5], s->r[16], s->r[17], s->r[18],
            s->r[26], s->r[29]);
    fflush(stderr);
}

static void audio_trace(CpuState *s) {
    if (s_cur < 0) return;
    if (s->pc == 0x000872ccu) {
        fprintf(stderr, "DEBUG PLAYSTREAM: uid=0x%x a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x ra=0x%08x\n",
                s_tcb[s_cur].uid, s->r[4], s->r[5], s->r[6], s->r[7], s->r[31]);
        fflush(stderr);
    }
}

/* Phase 2.C: SR_T111PC trace. Logs each yield of the LAUNCHER thread (historically uid
 * 0x111 -- the trace keeps its original name) while its PC differs from the last recorded
 * PC; capped at SR_T111PC_MAX (default 256) entries so a long post-worker loop does not
 * flood the trace. Cleared on capture so we can dump a fresh window per bring-up cycle.
 * The uid is resolved via g_launcher_uid so allocation drift cannot silence the trace. */
static int s_t111_on = -1;
static int s_t111_max = 256;
typedef struct { uint32_t pc, ra; uint64_t tick; } T111Rec;
static T111Rec s_t111[256];
static int s_t111_n = 0;
static uint32_t s_t111_last_pc = 0;
static int s_t111_latched = 0;
static void t111_trace(CpuState *s) {
    if (s_t111_on < 0) {
        const char *e = getenv("SR_T111PC");
        s_t111_on = e ? 1 : 0;
        const char *m = getenv("SR_T111PC_MAX");
        if (m) { int v = atoi(m); if (v > 0 && v <= 256) s_t111_max = v; }
        fprintf(stderr, "T111: trace init on=%d max=%d\n", s_t111_on, s_t111_max);
    }
    if (!s_t111_on || s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    if (t->uid != g_launcher_uid) return;
    uint32_t pc = s->pc ? s->pc : t->saved.pc;
    uint32_t ra = s->r[31] ? s->r[31] : t->saved.r[31];
    if (pc == s_t111_last_pc && s_t111_n > 0) return;
    s_t111_last_pc = pc;
    if (s_t111_n >= s_t111_max) return;
    s_t111[s_t111_n].pc = pc;
    s_t111[s_t111_n].ra = ra;
    s_t111[s_t111_n].tick = s_tick;
    s_t111_n++;
    if (s_t111_n == s_t111_max && !s_t111_latched) {
        s_t111_latched = 1;
        fprintf(stderr, "T111: latch reached (%d entries). Recent PCs:\n", s_t111_max);
        for (int i = 0; i < s_t111_n; i++)
            fprintf(stderr, "  tick=%llu pc=0x%08x ra=0x%08x\n",
                    (unsigned long long)s_t111[i].tick, s_t111[i].pc, s_t111[i].ra);
        fflush(stderr);
    }
}
void sr_t111_dump(void) {
    fprintf(stderr, "T111: dump (%d entries)\n", s_t111_n);
    for (int i = 0; i < s_t111_n; i++)
        fprintf(stderr, "  tick=%llu pc=0x%08x ra=0x%08x\n",
                (unsigned long long)s_t111[i].tick, s_t111[i].pc, s_t111[i].ra);
    fflush(stderr);
}

void sr_yield(CpuState *s) {
    if (s->r[28] != 0u) {
        s_gp = s->r[28];
    }
    /* SR_COPYSPIN: bounded register dumps at the plane-copy / cache-flush emulator
     * back-edges (f_00025a18 / f_00025a74).  Sits ABOVE the interrupt-suspension
     * early return on purpose: a hang with interrupts suspended silences every
     * diagnostic below that return, so this one must not depend on it. */
    static int s_copyspin = -1;
    if (s_copyspin < 0) s_copyspin = getenv("SR_COPYSPIN") ? 1 : 0;
    if (s_copyspin) {
        static int copyspin_n = 0;
        static uint64_t copyspin_seen = 0;
        if (s->pc == 0x00025a50u || s->pc == 0x00025a5cu ||
            s->pc == 0x00025abcu || s->pc == 0x00025ac8u) {
            copyspin_seen++;
            if (copyspin_n < 12 && (copyspin_seen & (copyspin_seen - 1)) == 0) { /* powers of two: 1,2,4,... */
                copyspin_n++;
                fprintf(stderr,
                    "COPYSPIN[%d]: pc=0x%08x backedge_hits=%llu intr=%d tick=%llu\n"
                    "  cols_left(r5)=0x%08x width(r6)=0x%08x rows_left(r7)=0x%08x stride(r8)=0x%08x\n"
                    "  src(r9)=0x%08x dst(r4)=0x%08x rowbase(r11)=0x%08x\n"
                    "  s0(r16)=0x%08x s3(r19)=0x%08x s4(r20)=0x%08x s5(r21)=0x%08x s6(r22)=0x%08x ra=0x%08x\n",
                    copyspin_n, s->pc, (unsigned long long)copyspin_seen, s_interrupts_enabled,
                    (unsigned long long)s_tick,
                    s->r[5], s->r[6], s->r[7], s->r[8],
                    s->r[9], s->r[4], s->r[11],
                    s->r[16], s->r[19], s->r[20], s->r[21], s->r[22], s->r[31]);
                fflush(stderr);
            }
        }
    }
    /* Time and source latches advance at every scheduler boundary, even when
     * the CPU's interrupt-enable bit currently suppresses delivery. */
    scheduler_progress_time();
    /* sceKernelCpuSuspendIntr is also a scheduler lock on a real single-core
     * PSP.  Defer both fiber switches and vblank interrupts until ResumeIntr
     * restores the saved state.  A suspension that never resumes freezes the
     * cooperative scheduler and silences every yield-path diagnostic below this
     * return, so surface a long-lived one exactly once per stuck episode. */
    static uint64_t s_suspended_yields = 0;
    if (!s_interrupts_enabled) {
        if (++s_suspended_yields == 200000ull) {
            fprintf(stderr,
                "sr_yield: 200k consecutive yields with interrupts suspended "
                "(uid=0x%x pc=0x%08x ra=0x%08x) -- suspension appears stuck\n",
                s_cur >= 0 ? s_tcb[s_cur].uid : 0, s->pc, s->r[31]);
            if (s->pc == 0x00010c70u) {
                /* _malloc_r bin-chain walk (node = MEM[node+0xc] until sentinel in v1).
                 * Dump the chain so a poisoned bin is visible in the log. */
                uint32_t sentinel = s->r[3], node = s->r[16];
                fprintf(stderr, "  malloc bin walk: sentinel=0x%08x cursor=0x%08x "
                        "binhead[0x2cf6d4]=0x%08x binalt[0x2cf6dc]=0x%08x req(r17)=0x%08x\n",
                        sentinel, node, MEM_R32(0x002cf6d4u), MEM_R32(0x002cf6dcu), s->r[17]);
                for (int i = 0; i < 12 && node != 0u; i++) {
                    fprintf(stderr, "    node[%d]=0x%08x size(+4)=0x%08x fwd(+8)=0x%08x bck(+0xc)=0x%08x\n",
                            i, node, MEM_R32(node + 4u), MEM_R32(node + 8u), MEM_R32(node + 0xcu));
                    node = MEM_R32(node + 0xcu);
                    if (node == sentinel) { fprintf(stderr, "    (reached sentinel)\n"); break; }
                }
            }
            fflush(stderr);
        }
        atomic_store_explicit(&sr_timeslice, TIMESLICE, memory_order_relaxed);
        return;
    }
    s_suspended_yields = 0;
    /* Phase B1.spin (audio dead-loop break) was removed: after stripping the
     * VBLANK forward in recomp.c and the relaunch hacks in deliver_vblank,
     * the recomp's f_0004ea98 stub and its downstream goto-spin are now a real
     * bug to fix at the source. Forcing s->pc from inside sr_yield is unsafe
     * (mutates guest control flow mid-recomp) — replace f_0004ea98 with a
     * native handler in hle.c that writes MEM[0x33b230]=1 and returns r3=1. */

    static int s_yieldlog = -1;
    if (s_yieldlog < 0) s_yieldlog = getenv("SR_YIELDLOG") ? 1 : 0;
    if (s_cur >= 0 && s_tcb[s_cur].uid == g_worker_uid && s_yieldlog) {
        static int yield_count = 0;
        if (yield_count < 200) {
            uint32_t k0 = s->r[26];
            fprintf(stderr, "YIELD uid=0x%x pc=0x%08x ra=0x%08x tick=%llu k0=0x%08x k0+4=0x%08x\n",
                    g_worker_uid, s->pc, s->r[31], s_tick, k0, MEM_R32(k0 + 4));
            yield_count++;
        } else if ((yield_count % 5000) == 0) {
            fprintf(stderr, "PCSAMPLE uid=0x%x pc=0x%08x ra=0x%08x tick=%llu\n",
                    g_worker_uid, s->pc, s->r[31], s_tick);
        }
        yield_count++;
    }
    /* HEAPSPIN diagnostic (SR_HEAPSPIN): one-shot dump of the libc malloc free-list
     * control words when the worker is spinning in the heap allocator.
     * f_00010738 (malloc) reads MEM[0x2cf6d4] (free-list head) / MEM[0x2cf6dc]; if
     * these are zero the allocator can't satisfy a request and loops. Dump once. */
    static int s_heapspin = -1;
    if (s_heapspin < 0) s_heapspin = getenv("SR_HEAPSPIN") ? 1 : 0;
    if (s_cur >= 0 && s_tcb[s_cur].uid == g_worker_uid && s_heapspin) {
        static int workerspin_dumped = 0;
        if (s->pc == 0x000115e8u || s->pc == 0x0000d62cu || s->pc == 0x00000c5cu) {
            fprintf(stderr, "HEAPSPIN: pc=0x%08x tick=%llu ra=0x%08x\n", s->pc, (unsigned long long)s_tick, s->r[31]);
            fprintf(stderr, "  a0(r4)=0x%08x a1(r5)=0x%08x (req size) a2(r6)=0x%08x\n", s->r[4], s->r[5], s->r[6]);
            fprintf(stderr, "  s0(r16)=0x%08x s2(r18)=0x%08x s3(r19)=0x%08x\n", s->r[16], s->r[18], s->r[19]);
            fprintf(stderr, "  MEM[0x2cf6d4] freelist_head=0x%08x\n", MEM_R32(0x002cf6d4u));
            fprintf(stderr, "  MEM[0x2cf6dc] freelist_alt =0x%08x\n", MEM_R32(0x002cf6dcu));
            {
                uint32_t head = MEM_R32(0x002cf6d4u);
                fprintf(stderr, "  Freelist blocks: ");
                uint32_t curr = head;
                for (int k = 0; k < 10 && curr != 0 && curr >= 0x0030b000u && curr < 0x0164b000u; k++) {
                    uint32_t prev = MEM_R32(curr + 0x0u);
                    uint32_t size = MEM_R32(curr + 0x4u);
                    uint32_t next = MEM_R32(curr + 0x8u);
                    uint32_t prev_self = MEM_R32(curr + 0xcu);
                    fprintf(stderr, "[0x%08x: prev=0x%08x size=0x%x next=0x%08x prev_self=0x%08x] ", curr, prev, size, next, prev_self);
                    if (next == head || next == 0 || next == curr) break;
                    curr = next;
                }
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "  MEM[0x2cf6b8] heap_ctx     =0x%08x\n", MEM_R32(0x002cf6b8u));
            fprintf(stderr, "  MEM[0x2cf6c0]              =0x%08x\n", MEM_R32(0x002cf6c0u));
            fprintf(stderr, "  MEM[0x2cf6c8]              =0x%08x\n", MEM_R32(0x002cf6c8u));
            fprintf(stderr, "  UserSbrk block uid 0x112: MEM[0x30b000]=0x%08x (freelist node), MEM[0x30aa84]=0x%08x (heap_bump_ptr seed)\n",
                    MEM_R32(0x0030b000u), MEM_R32(0x0030aa84u));
            fflush(stderr);
        }
        /* Phase A followup: capture worker state at the post-VFS spin PC 0x48c18
         * once, so we can see what game init step got stuck after the heap wedge
         * cleared. Default off; gate on env var so the verbose dump doesn't
         * race the heap probe. */
        if (!workerspin_dumped && s->pc == 0x00048c18u) {
            workerspin_dumped = 1;
            fprintf(stderr, "WORKERSPIN: pc=0x48c18 ra=0x%08x tick=%llu\n", s->r[31], (unsigned long long)s_tick);
            fprintf(stderr, "  a0(r4)=0x%08x a1(r5)=0x%08x a2(r6)=0x%08x a3(r7)=0x%08x\n",
                    s->r[4], s->r[5], s->r[6], s->r[7]);
            fprintf(stderr, "  s0(r16)=0x%08x s1(r17)=0x%08x s2(r18)=0x%08x s3(r19)=0x%08x "
                    "s4(r20)=0x%08x s5(r21)=0x%08x s6(r22)=0x%08x s7(r23)=0x%08x\n",
                    s->r[16], s->r[17], s->r[18], s->r[19],
                    s->r[20], s->r[21], s->r[22], s->r[23]);
            fprintf(stderr, "  t0(r8)=0x%08x t1(r9)=0x%08x t2(r10)=0x%08x v0(r2)=0x%08x\n",
                    s->r[8], s->r[9], s->r[10], s->r[2]);
            fprintf(stderr, "  libc_main_id[0x0030a040]=0x%08x  heap_bump[0x0030b000]=0x%08x  "
                    "audio_gate[0x0030ab8c]=0x%08x\n",
                    MEM_R32(0x0030a040u), MEM_R32(0x0030b000u), MEM_R32(0x0030ab8cu));
            fflush(stderr);
        }
        /* AUDIOWEDGE diagnostic was removed alongside the audio spin hack —
         * once f_0004ea98 is replaced with a native handler in hle.c the
         * wedge becomes reproducible and we no longer need the per-yield
         * memory-of-record dump. */
    }
    /* Previously an SR_WORKER_RELAUNCH block lived here that force-mocked
     * the worker thread into DORMANT after the first GE submit so a relaunch
     * hack could restart it each vblank. Removed: it's a hack; the real
     * path is for the worker's recompiled main_RunGameLoop to round-trip
     * through engine_YieldFrame / sceDisplayWaitVblankStart natively, in
     * which case the per-vblank stub avail no longer matters. */

    /* I2: extra yield snapshot for the worker after umd.ufl is parsed. Activated by
     *   SR_POSTUMD env (default off). Captures a0..a2 + RA + libc_main_id + last_alloc
     *   so we can see which guest function landed at each yield after the manifest
     *   decode. Bounded at 256 entries so we don't fill the trace. */
    if (s_cur >= 0 && s_tcb[s_cur].uid == g_worker_uid) {
        static int s_postumd = -1;
        if (s_postumd < 0) { const char *e = getenv("SR_POSTUMD"); s_postumd = (e && strcmp(e, "0") != 0) ? 1 : 0; }
        if (s_postumd) {
            static int post_yield_count = 0;
            if (post_yield_count < 256) {
                fprintf(stderr, "POSTUMD-YIELD uid=0x%x pc=0x%08x ra=0x%08x tick=%llu a0=0x%08x a1=0x%08x a2=0x%08x libc_main_id[0x0030a040]=0x%08x last_alloc=0x%08x\n",
                        g_worker_uid, s->pc, s->r[31], s_tick, s->r[4], s->r[5], s->r[6],
                        MEM_R32(0x0030a040u), MEM_R32(0x0030b000u));
                fflush(stderr);
                post_yield_count++;
            }
        }
    }
    atomic_store_explicit(&sr_timeslice, TIMESLICE, memory_order_relaxed);
    if (s_cur < 0) {
        scheduler_service_pending();
        return;                            /* not in a thread */
    }
    s_tick++;
    if ((s_tick & 0xff) == 0) vtime_refresh(); /* observation only; time already progressed above */
    audio_trace(s);
    boot_diag(s);
    spin_check(s);
    t111_trace(s);
    /* Pump messages if we are spinning/loading and not calling gui_present. */
    if (gui_on() && (s_tick & 0x7f) == 0) {
        extern void gui_pump(void);
        gui_pump();
    }
    /* The PSP VBLANK is an interrupt source: time crossing its deadline latches
     * the bit, and only the eligible-delivery phase runs the handler. */
    scheduler_latch_due_events();
    /* The SR_YIELD macro no longer calls sr_vblank_quantum_due() (perf: it was invoking
     * SDL_GetTicksNS on 99.9% of all yield points). Instead, check it here inside
     * sr_yield() which fires once per TIMESLICE (1000 yields). This preserves the
     * safety net for sparse yield cadences while eliminating millions of clock queries.
     *
     * #70 slice B -- VBLANK production has exactly one owner. The quantum is a
     * host-wall-clock watchdog on the interval since the last DELIVERY; it never
     * advanced s_vbl_next_us, so in paced mode raising the source here inserted an
     * extra guest VBLANK at the ~16.000 ms quantum boundary 683 us ahead of the
     * ~16.683 ms rational one, and the rational deadline then fired anyway: two
     * events per period, with vcount, the wait latch and waiter wakeups all landing
     * off the display timeline. In paced mode the watchdog has nothing left to add
     * either -- scheduler_progress_time() at the top of this same function already
     * sampled the host clock and scheduler_latch_due_events() just latched every
     * elapsed rational deadline from it. A quantum still due at this point therefore
     * means DELIVERY is behind (interrupts suspended, or service re-entered), which
     * is worth counting but must not manufacture an event; scheduler_service_pending()
     * immediately below is the thing that catches up.
     *
     * Turbo (SR_NOVBPACE=1) keeps the raise: it has no host-anchored virtual clock,
     * so a guest loop that never reaches an explicit advancement point has no other
     * VBLANK source at all. */
    if (sr_vblank_quantum_due()) {
        if (s_pace_on) s_vblank_late_service_yields++;
        else sched_raise_interrupt(SCHED_INTR_VBLANK);
    }
    scheduler_service_pending();
    TCB *t = &s_tcb[s_cur];
    /* Only switch if someone else could run; otherwise keep going (avoids pointless churn). */
    int other = 0;
    for (int i = 0; i < s_ntcb; i++)
        if (i != s_cur && (s_tcb[i].state == TH_READY ||
            ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
             s_vtime_us >= s_tcb[i].wake))) { other = 1; break; }
    /* If no other thread is runnable AND nothing is sleeping on a small timer, TURBO mode
     * advances virtual time so PSP timers (UMD-Ready, callback-drive, etc.) fire. uid 0x115
     * spinning in critically-fast SuspendIntr/ResumeIntr would otherwise burn CPU without
     * ever waking the UMD-callback waker. Cap each spur by the lowest wake-time across
     * waiters so we never skip past a scheduled event.
     *
     * #70 slice A -- clock ownership. This spur is a TURBO-mode construct and must not run
     * in paced mode. With pacing on, guest virtual time is a SAMPLE of the host monotonic
     * clock (scheduler_progress_time() above already took it); jumping s_vtime_us to a
     * waiter's deadline puts guest time AHEAD of host time, and since every delay, timed
     * wait and the rational VBLANK deadline are expressed in that same clock, the whole
     * guest timeline then runs fast by however much this branch manufactured. A busy-wait
     * loop beside one sleeper (exactly the HST boot/worker shape) hits this branch
     * continuously, which is the guest-runs-fast half of the measured #70 drift.
     *
     * Paced mode needs no spur: real time reaches the waiter's deadline on its own, and
     * scheduler_progress_time() at the top of every sr_yield samples it. */
    if (!other) {
        /* Phase 2.1-follow v3: BOUND vtime advancement tightly to real wake deadlines.
         * Recomp-emitted SR_YIELD on every backward branch (codegen.py lines 832/837)
         * means tight recomp loops (e.g. cache-flush emulator f_00025a18 yielding once
         * per 2-byte pair, ~130k yields for a 262 KB flush) traverse this !other branch
         * many times.
         *
         * Unconditional 1 ms-per-yield vtime advance + immediate deliver_vblank on
         * crossing s_vbl_next_us was catastrophic: each yield that crossed the
         * boundary triggered vblank_pace() -> a 16 ms host delay inside the worker coroutine
         * stack. Accumulated ~10s of Sleep; watchdog (600 vblanks no frame) fired
         * before the cache emulator returned. The user saw "black screen" -- the
         * SDL window showed only the initial frame 1 (8 black-sprite overdraw).
         *
         * Fix: drive vtime only forward enough to wake any imminent, finite-deadline
         * waiter (so timer-driven threads still get promoted by pick_next). Skip direct
         * VBLANK delivery here -- cadence is preserved by:
         *   - scheduler_progress_time() at the top of every sr_yield, which in turbo
         *     charges the deterministic quantum and in paced mode samples the host clock
         *   - scheduler_latch_due_events(), which raises the source for every elapsed
         *     rational deadline and coalesces the missed ones into the pending bit
         *   - the eligible-delivery phase (scheduler_service_pending) invoking
         *     deliver_vblank
         *   - the sched_run idle loop driving the vblank chain when nothing is runnable
         *   - in turbo only, sr_vblank_quantum_due() latching an out-of-band source
         * With no imminent wait, adv stays at 0 -- the recomp-fast-loop Sleep
         * cascade is broken, the worker exits f_00025a18 in ms, frame 2 presents.
         *
         * Important: wake==(uint64_t)-1 means "wait on object" (infinite), not a
         * finite timed wait -- must skip it or the initial wait check below would
         * set adv=(uint64_t)-1 and create an invalid deadline.
         */
        if (!s_pace_on) {
            uint64_t adv = 0;
            for (int i = 0; i < s_ntcb; i++) {
                if (i == s_cur) continue;
                if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
                    s_tcb[i].wake != (uint64_t)-1 && s_tcb[i].wake > s_vtime_us) {
                    uint64_t delta = s_tcb[i].wake - s_vtime_us;
                    if (adv == 0 || delta < adv) adv = delta;
                }
            }
            scheduler_add_time(adv);
        }
        scheduler_latch_due_events();
        scheduler_service_pending();
        /* Diagnostic: spin watchdog. */
        static unsigned long long spun = 0;
        static int dumps = 0;
        if (++spun % 100000ull == 2001 && dumps < 6) {
            dumps++;
            static const char *stn[] = {"DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ"};
            int ready = 0, delay = 0, dormant = 0, run = 0, waitobj = 0;
            for (int i = 0; i < s_ntcb; i++) {
                switch (s_tcb[i].state) {
                    case TH_READY:      ready++;   break;
                    case TH_WAIT_DELAY: delay++;   break;
                    case TH_WAIT_OBJ:   waitobj++; break;
                    case TH_DORMANT:    dormant++; break;
                    default:            run++;     break;  /* TH_RUNNING only */
                }
            }
            /* running counts only TH_RUNNING. wait_obj threads are broken out
             * separately -- folding them into "running" (as this once did) makes a
             * fully-blocked scheduler look busy and reads as a false stall. */
            fprintf(stderr, "sched: spin on uid 0x%x at pc=0x%08x ra=0x%08x; threads=%d ready=%d delay=%d wait_obj=%d dormant=%d running=%d vbl_late_service_yields=%llu\n",
                    t->uid, s->pc, s->r[31], s_ntcb, ready, delay, waitobj, dormant, run,
                    (unsigned long long)s_vblank_late_service_yields);
            for (int i = 0; i < s_ntcb; i++)
                fprintf(stderr, "  uid 0x%x entry 0x%08x %-10s prio %d pc=0x%08x ra=0x%08x s3=0x%08x v0=0x%08x wait_obj=0x%x wakeups=%d\n",
                        s_tcb[i].uid, s_tcb[i].entry, stn[s_tcb[i].state < 5 ? s_tcb[i].state : 0],
                        s_tcb[i].priority, s_tcb[i].saved.pc, s_tcb[i].saved.r[31],
                        s_tcb[i].saved.r[19], s_tcb[i].saved.r[2],
                        s_tcb[i].wait_obj, s_tcb[i].wakeups);
            /* If spinning in hash area (0x1b5xx-0x1b8xx), dump table diagnostics. */
            if (s->pc == 0x0006ea40u) {
                fprintf(stderr, "  0x6ea40 loop diag: r2=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x r16=0x%08x r17=0x%08x\n",
                        s->r[2], s->r[3], s->r[4], s->r[5], s->r[16], s->r[17]);
            }
            if (s->pc == 0x00014dacu) {
                static int strspin_cnt = 0;
                strspin_cnt++;
                if (strspin_cnt <= 3) {
                    fprintf(stderr, "  0x14dac strcmp-spin #%d: r4=0x%08x r5=0x%08x r6=%u r16=%u r18=0x%08x ra=0x%08x\n",
                            strspin_cnt, s->r[4], s->r[5], s->r[6], s->r[16], s->r[18], s->r[31]);
                    fprintf(stderr, "    mem[r4]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                            MEM_R8(s->r[4]+0), MEM_R8(s->r[4]+1), MEM_R8(s->r[4]+2), MEM_R8(s->r[4]+3),
                            MEM_R8(s->r[4]+4), MEM_R8(s->r[4]+5), MEM_R8(s->r[4]+6), MEM_R8(s->r[4]+7));
                    fprintf(stderr, "    mem[r5]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                            MEM_R8(s->r[5]+0), MEM_R8(s->r[5]+1), MEM_R8(s->r[5]+2), MEM_R8(s->r[5]+3),
                            MEM_R8(s->r[5]+4), MEM_R8(s->r[5]+5), MEM_R8(s->r[5]+6), MEM_R8(s->r[5]+7));
                }
            }
            if (s->pc == 0x0000095cu) {
                fprintf(stderr, "  0x0095c walker diag: r4=0x%08x r5=0x%08x r16=0x%08x r17=0x%08x r18=0x%08x r19=0x%08x r20=0x%08x r2=0x%08x\n",
                        s->r[4], s->r[5], s->r[16], s->r[17], s->r[18], s->r[19], s->r[20], s->r[2]);
            }
            if (s->pc >= 0x0001b500u && s->pc <= 0x0001b800u) {
                fprintf(stderr, "  hash diag: r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x r16=0x%08x r17=0x%08x r18=0x%08x r19=0x%08x\n",
                        s->r[4], s->r[5], s->r[6], s->r[7], s->r[16], s->r[17], s->r[18], s->r[19]);
                /* Try to dump the table struct pointed to by r17 */
                uint32_t struct_ptr = s->r[17];
                if (struct_ptr >= 0x00001000u && struct_ptr < 0x0c000000u) {
                    uint32_t tbl_base = MEM_R32(struct_ptr);
                    uint32_t tbl_count = MEM_R32(struct_ptr + 4);
                    uint32_t probe_idx = s->r[7];
                    fprintf(stderr, "  hash diag: struct=0x%08x tbl_base=0x%08x count=%u probe_idx=%u\n",
                            struct_ptr, tbl_base, tbl_count, probe_idx);
                    if (tbl_base >= 0x00001000u && tbl_base < 0x0c000000u && tbl_count > 0 && tbl_count < 200000u) {
                        int empty = 0, occupied = 0;
                        uint32_t scan_limit = tbl_count < 50000 ? tbl_count : 50000;
                        for (uint32_t i = 0; i < scan_limit; i++) {
                            uint32_t key = MEM_R32(tbl_base + i * 8);
                            if (key == 0xFFFFFFFFu) empty++;
                            else occupied++;
                        }
                        fprintf(stderr, "  hash diag: empty=%d occupied=%d (scanned %u of %u)\n",
                                empty, occupied, scan_limit, tbl_count);
                        for (int i = 0; i < 4 && i < (int)scan_limit; i++)
                            fprintf(stderr, "  slot[%d]: key=0x%08x val=0x%08x\n", i,
                                    MEM_R32(tbl_base + i*8), MEM_R32(tbl_base + i*8 + 4));
                        if (probe_idx < scan_limit) {
                            uint32_t end = probe_idx + 8 < scan_limit ? probe_idx + 8 : scan_limit;
                            fprintf(stderr, "  probe area [%u..%u]:\n", probe_idx, end - 1);
                            for (uint32_t i = probe_idx; i < end; i++)
                                fprintf(stderr, "  slot[%u]: key=0x%08x val=0x%08x\n", i,
                                        MEM_R32(tbl_base + i*8), MEM_R32(tbl_base + i*8 + 4));
                        }
                    } else {
                        fprintf(stderr, "  hash diag: invalid tbl_base or count\n");
                    }
                } else {
                    fprintf(stderr, "  hash diag: invalid struct_ptr 0x%08x\n", struct_ptr);
                }
            }
            /* If spinning in memcpy (0x11090-0x1119c), dump memcpy diagnostics. */
            if (s->pc >= 0x00011090u && s->pc <= 0x0001119cu) {
                fprintf(stderr, "  memcpy diag: dest=0x%08x src=0x%08x size=%d counter=%u ra=0x%08x\n",
                        s->r[4], s->r[5], (int32_t)s->r[6], s->r[8], s->r[31]);
                if (s->r[31] == 0x00048378u) {
                    fprintf(stderr, "  memcpy from ge-loop: [0x310fec]=0x%08x [0x310ff0]=0x%08x\n",
                            MEM_R32(0x310fecu), MEM_R32(0x310ff0u));
                }
            }
            /* If spinning in display-list loop (0x48360-0x483a0 in f_00048258), dump the
             * display-list struct at 0x310fe0-0x310ff0 so we can see the loop count/size. */
            if (s->pc >= 0x00048360u && s->pc <= 0x000483a0u) {
                fprintf(stderr, "  ge-loop diag: r16(counter)=%u r17(dest)=0x%08x r18(src)=0x%08x\n",
                        s->r[16], s->r[17], s->r[18]);
                fprintf(stderr, "  ge-loop struct: [0x310fe0]=0x%08x [0x310fe4]=0x%08x [0x310fe8]=0x%08x [0x310fec](entry_size)=%u [0x310ff0](count)=%u\n",
                        MEM_R32(0x310fe0u), MEM_R32(0x310fe4u), MEM_R32(0x310fe8u),
                        MEM_R32(0x310fecu), MEM_R32(0x310ff0u));
                fprintf(stderr, "  ge-loop mem: dest_area[0]=0x%08x src_area[0]=0x%08x src_area[1]=0x%08x\n",
                        s->r[17] < 0x0c000000u ? MEM_R32(s->r[17]) : 0xDEADBEEF,
                        s->r[18] < 0x0c000000u ? MEM_R32(s->r[18]) : 0xDEADBEEF,
                        s->r[18] < 0x0bfffffcu ? MEM_R32(s->r[18] + 4) : 0xDEADBEEF);
            }
            fflush(stderr);
        }
        return;
    }
    if (!s_dispatch_enabled) return;
    memcpy(&t->saved, s, sizeof(CpuState));
    if (t->state == TH_RUNNING) t->state = TH_READY;
    switch_to_scheduler();
    /* resumed later: our registers were restored into *s by the scheduler before SwitchToFiber */
}

/* PSP scheduling is strict-priority preemptive: the moment a higher-priority thread becomes
 * ready (e.g. sceKernelStartThread starts one), it runs instead of the current thread. Without
 * this, a low-priority boot thread that starts a high-priority worker and busy-waits on its
 * output would never let the worker run. Call after any op that readies a thread.
 *
 * #70 slice C: a thread also becomes ready when its own deadline passes, with nobody calling
 * anything. That expiry used to be noticed only inside pick_next(), which runs on the
 * scheduler coroutine, so the scan below -- the check that actually takes the CPU away --
 * could not see it and a stronger-priority thread whose delay came due waited for the weaker
 * runner to yield or block. Promote first, then apply the unchanged strict-priority rule.
 *
 * This stays a boundary-triggered check, not continuous preemption: the caller decides when
 * a scheduler/interrupt boundary is reached, and the interrupt-disabled / dispatch-disabled
 * gate above still defers the whole thing to the next eligible one. */
void sched_preempt(void) {
    if (s_cur < 0 || !s_interrupts_enabled || !s_dispatch_enabled) return;
    sched_promote_expired_waits();
    TCB *cur = &s_tcb[s_cur];
    int best = -1;
    for (int i = 0; i < s_ntcb; i++) {
        if (i == s_cur) continue;
        if (s_tcb[i].state == TH_READY && (best < 0 || s_tcb[i].priority < s_tcb[best].priority))
            best = i;
    }
    if (best >= 0 && s_tcb[best].priority < cur->priority) {   /* strictly higher priority ready */
        memcpy(&cur->saved, s_cpu, sizeof(CpuState));
        cur->state = TH_READY;
        switch_to_scheduler();
    }
}

void sched_delay_current(uint32_t usec) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    uint64_t duration = usec ? usec : 1u;
    vtime_refresh();
    uint64_t wake = scheduler_deadline_after(duration);
    if (getenv("SR_DELAYLOG"))
        fprintf(stderr, "DELAY uid=0x%x entry=0x%08x usec=%u (%.1fs) wake=%llu\n", t->uid, t->entry, usec, usec / 1e6, (unsigned long long)wake);
    if (usec > 2000000u && getenv("SR_DELAYLOG"))   /* > 2s: catch a bogus huge delay */
        fprintf(stderr, "BIG DELAY uid=0x%x entry=0x%08x usec=%u (%.1fs)\n", t->uid, t->entry, usec, usec / 1e6);
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_DELAY;
    /* Real microseconds of virtual time; any positive delay yields at least once. */
    t->wake = wake;
    switch_to_scheduler();
}

void sched_block_on(uint32_t obj) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    if (getenv("SR_BLOCKLOG")) fprintf(stderr, "BLOCK: uid 0x%x on obj 0x%x (pc=0x%x ra=0x%x)\n", t->uid, obj, s_cpu->pc, s_cpu->r[31]);
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_OBJ;
    t->wait_obj = obj;
    t->wake = (uint64_t)-1;     /* infinite: only sched_wake releases it */
    switch_to_scheduler();
}

/* Block on obj, but also wake after usec of virtual time (a timed sema/event wait). Returns 1
 * if it timed out (the deadline passed), 0 if it was woken by a signal. */
int sched_block_on_timeout(uint32_t obj, uint32_t usec) {
    if (s_cur < 0) return 1;
    TCB *t = &s_tcb[s_cur];
    vtime_refresh();
    uint64_t deadline = scheduler_deadline_after(usec ? usec : 1u);
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_OBJ;
    t->wait_obj = obj;
    t->wake = deadline;
    switch_to_scheduler();
    vtime_refresh();
    return s_vtime_us >= deadline;   /* resumed: timed out if the deadline has passed */
}

/* WaitThreadEnd uses the same scheduler object-wait primitive as semaphores
 * and event flags.  Marking a waiter explicitly lets a target that is deleted
 * before the waiter resumes deliver its exit result without keeping the kernel
 * object queryable after deletion. */
void sched_set_current_join_target(uint32_t uid) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    t->join_target = uid;
    t->join_waiting = 1;
    t->join_result_valid = 0;
}

void sched_clear_current_join_target(void) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    t->join_waiting = 0;
    t->join_target = 0;
    t->join_result_valid = 0;
}

int sched_take_current_join_result(uint32_t uid, uint32_t *result_out) {
    if (s_cur < 0 || !result_out) return 0;
    TCB *t = &s_tcb[s_cur];
    if (!t->join_result_valid || t->join_target != uid) return 0;
    *result_out = t->join_result;
    t->join_result_valid = 0;
    t->join_target = 0;
    t->join_waiting = 0;
    return 1;
}

static void sched_wake_thread_joiners(uint32_t uid, uint32_t result) {
    for (int i = 0; i < s_ntcb; i++) {
        TCB *waiter = &s_tcb[i];
        if (waiter->deleted || waiter->state != TH_WAIT_OBJ ||
            !waiter->join_waiting || waiter->join_target != uid)
            continue;
        waiter->join_result = result;
        waiter->join_result_valid = 1;
        waiter->join_waiting = 0;
        waiter->state = TH_READY;
        waiter->wait_obj = 0;
        waiter->wake = 0;
        waiter->is_cb_wait = 0;
    }
}

void sched_wake(uint32_t obj) {
    for (int i = 0; i < s_ntcb; i++)
        if (!s_tcb[i].deleted && s_tcb[i].state == TH_WAIT_OBJ && s_tcb[i].wait_obj == obj)
            s_tcb[i].state = TH_READY;
}

uint64_t sched_vtime_us(void) {
    return s_vtime_us;
}

uint64_t sched_vtime_deadline_after(uint64_t delta) {
    return scheduler_deadline_after(delta);
}

void sched_vtime_refresh(void) {
    vtime_refresh();
}

/* The display controller has 286 horizontal sync positions per 59.94-Hz frame.
 * Keep its phase in the scheduler's microsecond domain instead of incrementing a
 * counter when sceDisplayGetCurrentHcount happens to be called.  Multiplication
 * is widened before the rational conversion so a long-running guest cannot wrap
 * the intermediate or make the display clock query-dependent. */
#define SCHED_DISPLAY_HCOUNT_PER_FRAME 286u
#define SCHED_DISPLAY_FRAME_NUMERATOR 1001000ull /* 1001/60000 s, expressed with denominator 60 */
#define SCHED_DISPLAY_VBLANK_WINDOW_US 1500u

static uint64_t scheduler_display_hcount_total(void) {
    __uint128_t numerator = (__uint128_t)s_vtime_us * 60u *
                            SCHED_DISPLAY_HCOUNT_PER_FRAME;
    __uint128_t total = numerator / SCHED_DISPLAY_FRAME_NUMERATOR;
    return total > UINT64_MAX ? UINT64_MAX : (uint64_t)total;
}

uint32_t sched_display_current_hcount(void) {
    return (uint32_t)(scheduler_display_hcount_total() %
                      SCHED_DISPLAY_HCOUNT_PER_FRAME);
}

uint32_t sched_display_accumulated_hcount(void) {
    return (uint32_t)scheduler_display_hcount_total();
}

int sched_display_is_vblank(void) {
    __uint128_t phase = ((__uint128_t)s_vtime_us * 60u) %
                        SCHED_DISPLAY_FRAME_NUMERATOR;
    const uint64_t window = (uint64_t)SCHED_DISPLAY_VBLANK_WINDOW_US * 60u;
    return phase >= SCHED_DISPLAY_FRAME_NUMERATOR - window;
}

void sched_set_current_cb_wait(int cb_wait) {
    if (s_cur >= 0) {
        s_tcb[s_cur].is_cb_wait = cb_wait;
    }
}

void sched_wake_callbacks(uint32_t thread_uid) {
    TCB *t = tcb_by_uid(thread_uid);
    if (t && (t->state == TH_WAIT_OBJ || t->state == TH_WAIT_DELAY) && t->is_cb_wait) {
        t->state = TH_READY;
        t->wake = s_vtime_us;
    }
}

/* sceKernelSleepThread[CB]: PSP wakeup-count semantics. If a wakeup is already pending, consume it
 * and return without blocking; otherwise block until sceKernelWakeupThread targets this thread.
 * This is distinct from sceKernelDelayThread (a timed sleep) -- conflating the two left the main
 * thread sleeping ~forever on a poisoned (0xDEADBEEF) delay argument. */
void sched_thread_sleep(void) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    fprintf(stderr, "DEBUG_SLEEP: thread=0x%x wakeups=%d state=%d\n", t->uid, t->wakeups, t->state);
    if (t->wakeups > 0) { t->wakeups--; return; }   /* pending wakeup: don't block */
    t->sleeping = 1;
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_OBJ;
    t->wait_obj = t->uid;       /* sleep marker: woken only by sched_thread_wakeup(uid) */
    t->wake = (uint64_t)-1;
    switch_to_scheduler();
}

void sched_thread_sleep_cb(void) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    if (getenv("SR_WAKELOG")) {
        fprintf(stderr, "DEBUG_SLEEP_CB: thread=0x%x wakeups=%d state=%d\n", t->uid, t->wakeups, t->state);
    }
    if (t->wakeups > 0) {
        t->wakeups--;
        return;
    }
    t->sleeping = 1;
    while (t->sleeping) {
        extern int sr_thread_has_pending_callbacks(uint32_t);
        extern int sr_thread_dispatch_callbacks(void);
        /* A wakeup delivered while this thread was dispatching callbacks (below)
         * cannot take the sched_thread_wakeup fast path -- state is not yet
         * TH_WAIT_OBJ during dispatch -- so it banks into t->wakeups. Consume a
         * banked wakeup here instead of blocking on it, matching the entry check
         * and sceKernelSleepThreadCB's wakeup-count semantics; otherwise the
         * pending wakeup is stranded and the thread sleeps until the next one. */
        if (t->wakeups > 0) {
            t->wakeups--;
            t->sleeping = 0;
            break;
        }
        if (sr_thread_has_pending_callbacks(t->uid)) {
            sr_thread_dispatch_callbacks();
            continue;
        }
        memcpy(&t->saved, s_cpu, sizeof(CpuState));
        t->state = TH_WAIT_OBJ;
        t->wait_obj = t->uid;
        t->wake = (uint64_t)-1;
        t->is_cb_wait = 1;
        switch_to_scheduler();
        t->is_cb_wait = 0;
    }
}

/* sceKernelWakeupThread(uid): wake a sleeping thread, or bank a pending wakeup if it is not
 * currently asleep (so a wakeup issued before the sleep is not lost). */
uint32_t sched_thread_wakeup(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return SCE_KERNEL_ERROR_UNKNOWN_THID;
    {
        fprintf(stderr, "DEBUG_WAKEUP: target=0x%x sleeping=%d state=%d wait_obj=0x%x wakeups=%d\n",
                uid, t->sleeping, t->state, t->wait_obj, t->wakeups);
        if (t->sleeping && t->state == TH_WAIT_OBJ && t->wait_obj == uid) {
            t->sleeping = 0;
            t->state = TH_READY;
            t->wait_obj = 0;
            t->wake = 0;
        } else {
            t->wakeups++;
        }
    }
    return 0;
}

/* sceKernelCancelWakeupThread(uid): returns the number of pending wakeups and clears them.
 * Passing uid 0 targets the current thread; ACX does this once per frame before sleeping. */
int sched_thread_cancel_wakeup(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return -1;
    int old = t->wakeups;
    t->wakeups = 0;
    return old;
}

static uint32_t psp_thread_status(const TCB *t) {
    switch (t->state) {
        case TH_RUNNING: return PSP_THREAD_RUNNING;
        case TH_READY: return PSP_THREAD_READY;
        case TH_WAIT_DELAY:
        case TH_WAIT_OBJ: return PSP_THREAD_WAITING;
        case TH_DORMANT:
        default: return PSP_THREAD_STOPPED;
    }
}

static uint32_t psp_wait_type(const TCB *t) {
    if (t->state == TH_WAIT_DELAY) return PSP_WAIT_DELAY;
    if (t->state == TH_WAIT_OBJ && t->sleeping && t->wait_obj == t->uid) return PSP_WAIT_SLEEP;
    if (t->state == TH_WAIT_OBJ) return PSP_WAIT_OBJECT;
    return PSP_WAIT_NONE;
}

int sched_thread_run_status(uint32_t uid, SrThreadRunStatus *out) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->size = 0x2c;
    out->status = psp_thread_status(t);
    out->currentPriority = (uint32_t)t->priority;
    out->waitType = psp_wait_type(t);
    /* waitId is only meaningful while the thread is actually waiting. t->wait_obj is
     * not cleared when a thread is resumed (e.g. sched_thread_wakeup sets TH_READY
     * without zeroing it), so reading it unconditionally would leak a stale object id
     * into a RUNNING/READY thread's status. PSP reports 0 when the thread is not
     * waiting; gate it on waitType to stay consistent with that and with waitType. */
    out->waitId = (out->waitType == PSP_WAIT_NONE)  ? 0u
                : (t->state == TH_WAIT_DELAY)       ? uid
                                                    : t->wait_obj;
    out->wakeupCount = (uint32_t)t->wakeups;
    out->runClocksLow = (uint32_t)s_tick;
    out->runClocksHigh = (uint32_t)(s_tick >> 32);
    return 0;
}

uint32_t sched_thread_exit_status(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return SCE_KERNEL_ERROR_UNKNOWN_THID;
    if (t->state != TH_DORMANT) return SCE_KERNEL_ERROR_NOT_DORMANT;
    if (!t->started) return SCE_KERNEL_ERROR_DORMANT;
    return (uint32_t)t->exit_status;
}

static void sched_exit_current_impl(int32_t status, int delete_object) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    uint32_t uid = t->uid;
    t->exit_status = status;
    sched_release_thread_resources(t);
    if (getenv("SR_SYSLOG")) fprintf(stderr, "thr 0x%x EXIT (entry 0x%08x)\n", uid, s_tcb[s_cur].entry);
    /* TCB/fiber leak fix: when a thread exits, free its fiber. The fiber was allocated
     * by CreateFiberEx in sched_run; without DeleteFiber we leaked one fiber handle
     * (and its reserved 64 MB virtual address space) per terminated thread over the
     * entire game session. The fiber cannot be deleted from inside its own body, so we
     * mark the thread DORMANT here and the scheduler loop will DeleteFiber/NULL
     * before the next time it would have switched away OR before the fiber is reused.
     *
     * SwitchToFiber pulls us out of the live fiber context cleanly; the scheduler
     * resumes on its main fiber and observes the DORMANT state, then deletes the
     * fiber (see sched_run's relaunch path -- it already deletes on restart -- and
     * the reaper loop added below). */
    t->state = TH_DORMANT;
    t->sleeping = 0;
    t->wait_obj = 0;
    t->wake = 0;
    t->wakeups = 0;
    t->join_waiting = 0;
    t->join_result_valid = 0;
    if (delete_object) {
        t->deleted = 1;
        t->entry = 0;
        sched_release_thread_stack(t);
    }
    sched_wake_thread_joiners(uid, (uint32_t)status);
    sched_wake(uid);             /* release threads in sceKernelWaitThreadEnd on this thread */
    switch_to_scheduler();
    /* after switch_to_scheduler returns, we are running on this fiber again because the
     * scheduler relaunch could have reused us (sched_start_thread's DORMANT restart path).
     * That's safe -- it deleted the old fiber and made a new one in its place, but in
     * the intermediate window this fiber was "live-but-DORMANT". Returning from the
     * dispatch() body in fiber_proc is the clean-exit voice of a thread -- it falls
     * into the for(;;) loop's body which SwitchToFiber back to the scheduler anyway. */
}

void sched_exit_current(int32_t status) {
    if (sched_status_is_negative(status))
        status = (int32_t)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
    sched_exit_current_impl(status, 0);
}

void sched_exit_current_unchecked(int32_t status) {
    sched_exit_current_impl(status, 0);
}

void sched_exit_current_delete(int32_t status) {
    sched_exit_current_impl(status, 1);
}

/* Clean coroutine unwind: longjmp back to coro_body's setjmp point.
 * Used by recomp.c exit handlers to avoid corrupt spin loops. */
void sched_unwind_current(void) {
    if (s_cur >= 0) {
        TCB *t = &s_tcb[s_cur];
        if (t->has_unwind_jmp) {
            longjmp(t->unwind_jmp, 1);
        }
    }
    /* Fallback: no jump buffer — just return normally */
}

int sched_current_priority(void) { return s_cur >= 0 ? s_tcb[s_cur].priority : 32; }

/* sceKernelChangeThreadPriority: uid 0 = current thread. */
void sched_set_priority(uint32_t uid, int priority) {
    if (uid == 0 && s_cur >= 0) uid = s_tcb[s_cur].uid;
    TCB *t = tcb_by_uid(uid);
    if (t) t->priority = priority;
}

/* Termination and deletion are separate scheduler operations.  The HLE
 * TerminateDeleteThread handler composes them so a target is first reported as
 * terminated to existing joiners, then disappears as a kernel object. */
uint32_t sched_terminate_thread(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return SCE_KERNEL_ERROR_UNKNOWN_THID;
    if (s_cur >= 0 && s_tcb[s_cur].uid == uid) {
        return SCE_KERNEL_ERROR_ILLEGAL_THID;
    }
    sched_release_thread_resources(t);
    if (getenv("SR_SYSLOG")) fprintf(stderr, "thr 0x%x TERMINATED (entry 0x%08x)\n", uid, t->entry);
    /* A target stopped by another thread is no longer running, so its host
     * coroutine can be destroyed immediately. The guest stack remains owned by
     * the dormant object until the corresponding DeleteThread operation. */
    if (t->coro) { sr_coro_destroy(t->coro); t->coro = NULL; }
    t->state = TH_DORMANT;
    t->exit_status = (int32_t)SCE_KERNEL_ERROR_THREAD_TERMINATED;
    t->sleeping = 0; t->wait_obj = 0; t->wake = 0;
    t->join_waiting = 0; t->join_result_valid = 0;
    sched_wake_thread_joiners(uid, SCE_KERNEL_ERROR_THREAD_TERMINATED);
    sched_wake(uid);
    return 0;
}

/* sceKernelDeleteThread removes a dormant object and returns its guest stack
 * range.  A deleted UID never resolves through tcb_by_uid, so every later
 * status/wait/start/wakeup operation gets UNKNOWN_THID. */
uint32_t sched_delete_thread(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return SCE_KERNEL_ERROR_UNKNOWN_THID;
    if (s_cur >= 0 && &s_tcb[s_cur] == t) return SCE_KERNEL_ERROR_NOT_DORMANT;
    if (t->state != TH_DORMANT) return SCE_KERNEL_ERROR_NOT_DORMANT;
    /* PSP reports the terminated status to waiters even when a dormant object
     * is deleted directly; a prior TerminateDeleteThread already delivered the
     * same result and leaves those waiters' latched value untouched. */
    sched_wake_thread_joiners(uid, SCE_KERNEL_ERROR_THREAD_TERMINATED);
    sched_release_thread_resources(t);
    if (t->coro) { sr_coro_destroy(t->coro); t->coro = NULL; }
    sched_release_thread_stack(t);
    t->started = 0;
    t->entry = 0;
    t->arglen = 0;
    t->argp = 0;
    t->exit_status = (int32_t)SCE_KERNEL_ERROR_DORMANT;
    t->sleeping = 0;
    t->wait_obj = 0;
    t->wake = 0;
    t->wakeups = 0;
    t->is_cb_wait = 0;
    t->join_waiting = 0;
    t->join_result_valid = 0;
    t->deleted = 1;
    return 0;
}

int sched_is_dormant(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    return t && t->state == TH_DORMANT;
}

/* The scheduler loop. Runs on the main (converted) fiber. Creates the entry thread, then keeps
 * resuming the highest-priority ready thread until none remain runnable. */
void sched_run(uint32_t entry, uint32_t arglen, uint32_t argp) {
    /* Start the host service-request advisory before the first guest thread is resumed,
     * so no guest code can reach a safe boundary while the producer is still absent. Its
     * first act is sched_vbl_paced(), which resolves the profile through pace_setup();
     * turbo declines the worker outright. The process may still exit from inside a guest
     * thread (an unimplemented import), in which case the worker is torn down with the
     * process -- safe, because everything it touches is process-lifetime static storage. */
    service_hint_start();
    uint32_t uid = sched_create_thread(entry, 32, 0);
    TCB *t0 = tcb_by_uid(uid);
    /* The entry (module_start) keeps the driver-seeded state -- real sp, gp, and module args
     * -- rather than the synthetic thread stack. */
    memcpy(&t0->saved, s_cpu, sizeof(CpuState));
    arglen = s_cpu->r[4]; argp = s_cpu->r[5];
    /* Historical "libc_main_thid" seed -- REMOVED. Static analysis and read-watchpoint
     * instrumentation confirmed that guest code does not read 0x0030a040 as a thread-id scalar,
     * but rather treats the region starting at 0x0030a040 as a module/EH-metadata registry.
     * The legacy seed write stomped slot 0 of the registry, which is now exclusively guest-owned. */
    /* Seed the PSP kernel wait queue head at 0x30aa88 to 0 (end of list).
     * Per-thread state_ptr entries are seeded in sched_create_thread (+4 slot only).
     * The uid slot (+0x84) is intentionally NOT pre-seeded: the game's f_00011710
     * (kernel thread-table registration) writes it; pre-seeding causes f_00011710 to
     * return -1 ("already registered") which aborts the launcher before the game loop.
     * After f_00011710 runs, f_00011600 (libc main-thread check) can read both fields. */
    MEM_W32(0x0030aa88u, 0u);                          /* next pointer: end of list (head init) */
    fprintf(stderr, "DEBUG: wait queue head init: [0x30aa88]=0x%x\n", MEM_R32(0x30aa88u));
    sched_start_thread(uid, arglen, argp);

    static const char *stn[] = {"DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ"};
    unsigned long long iters = 0;
    for (;;) {
        if (getenv("SCHED_DUMP") && (++iters % 400000) == 0) {
            fprintf(stderr, "--- sched dump (tick=%llu) ---\n", (unsigned long long)s_tick);
            for (int i = 0; i < s_ntcb; i++)
                fprintf(stderr, "  uid 0x%x entry 0x%08x %s prio %d wait_obj 0x%x\n",
                        s_tcb[i].uid, s_tcb[i].entry, stn[s_tcb[i].state], s_tcb[i].priority, s_tcb[i].wait_obj);
        }
        int idx = pick_next();
        if (idx < 0) {
            /* No thread is ready. If a timed wait expires before the next vblank is due,
             * sleep precisely to it (sub-frame delays keep their real duration); otherwise
             * advance the display source timeline and service its eligible pending interrupt.
             * Vblank delivery can't starve: the source is latched whenever due. */
            uint64_t soonest = (uint64_t)-1;
            for (int i = 0; i < s_ntcb; i++)
                if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
                    s_tcb[i].wake < soonest) soonest = s_tcb[i].wake;
            scheduler_progress_time();
            if (s_pace_on && soonest != (uint64_t)-1 && soonest > s_vtime_us &&
                soonest - s_vtime_us < vblank_due_us())
                { sleep_until_us(soonest); scheduler_progress_time(); }
            else {
                vblank_pace();
                scheduler_latch_due_events();
            }
            scheduler_service_pending();
            idx = pick_next();
        }
        if (idx < 0) {
            /* Still nothing. Stop only when nothing is even waiting on a deadline. */
            uint64_t soonest = (uint64_t)-1;
            for (int i = 0; i < s_ntcb; i++)
                if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
                    s_tcb[i].wake < soonest) soonest = s_tcb[i].wake;
            if (soonest == (uint64_t)-1) {
                fprintf(stderr, "SCHED: no runnable threads left (deadlock/infinite wait). Dumping thread states:\n");
                sched_dump_threads();
                if (s_t111_on && s_t111_n) sr_t111_dump();
                break;   /* truly nothing runnable (all infinite waits) */
            }
            if (!s_pace_on) {                     /* turbo: jump the clock over the wait */
                if (soonest > s_vtime_us) s_vtime_us = soonest;
                idx = pick_next();
                if (idx < 0) {
                    fprintf(stderr, "SCHED: no runnable threads left after time jump. Dumping thread states:\n");
                    sched_dump_threads();
                    break;
                }
            } else {
                continue;   /* paced: keep delivering vblanks; real time reaches the deadline */
            }
        }
        TCB *t = &s_tcb[idx];
        s_cur = idx;
        t->state = TH_RUNNING;
        memcpy(s_cpu, &t->saved, sizeof(CpuState));   /* load this thread's registers */
        /* FRONTIER: r26/k0 is the PSP per-thread kernel-context pointer; libc's _getmodreent
         * reads it via the recompiled f_0000fe3c. The codegen treats r26 as caller-saved
         * scratch, so it may end as 0xDEADBEEF or 0 in the saved state. Real PSP thread
         * bodies don't use r26 as scratch -- it's preserved per-thread. Restore it on every
         * resume so the libc main-thread check never sees r26=0. */
        if (t->k0_init) {
            s_cpu->r[26] = t->k0_init;
            t->saved.r[26] = t->k0_init;
        }
        atomic_store_explicit(&sr_timeslice, TIMESLICE, memory_order_relaxed);          /* a fresh slice for this run (the counter is global) */
        if (!t->started) {
            t->started = 1;
            /* Guest calls become native C calls, so a deep guest call chain needs a deep host
             * stack. Reserve a large fiber stack (committed on demand) to match.
             *
             * NOTE: CreateFiberEx reserves the requested commit/ReservationSize lazily, but a
             * 1.5 GB total reservation footprint (64MB * ~96 threads after several
             * launch/relaunch cycles) can return ERROR_NOT_ENOUGH_MEMORY on hosts whose
             * page file is constrained. We must NULL-check the return before calling
             * SwitchToFiber -- a NULL fiber would deref garbage and tear the process down.
             * On failure we surface a clean error and abort (there's no game-state left to
             * save -- the scheduler loop has no way to recover a thread that never started). */
            t->coro = sr_coro_create(coro_body, t, (size_t)64 << 20);
            if (!t->coro) {
                fprintf(stderr, "sched_run: sr_coro_create failed for uid=0x%x entry=0x%08x "
                        "-- cannot continue\n",
                        t->uid, t->entry);
                fflush(stderr);
                abort();
            }
        }
        extern int g_hle_depth;
        g_hle_depth = t->hle_depth;
        sr_perf_guest_begin();
        sr_coro_switch(t->coro);           /* run until it yields/blocks/exits */
        sr_perf_guest_end();
        t->hle_depth = g_hle_depth;
        g_hle_depth = 0;
        /* A coroutine cannot destroy itself from inside sched_exit_current;
         * reap it as soon as control is back on the scheduler coroutine. This
         * covers both ordinary ExitThread and ExitDeleteThread. */
        if (t->state == TH_DORMANT && t->coro) {
            sr_coro_destroy(t->coro);
            t->coro = NULL;
        }
        s_cur = -1;
        s_tick++;
    }
    /* Every exit from the loop above lands here. Join the advisory worker before the
     * driver begins teardown; after this point nothing can raise a service request. */
    service_hint_stop();
}
