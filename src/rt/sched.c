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
#include "title_config.h"   /* optional, validated title bindings (roles + counter words) */
#include "nested_frames.h"  /* reserved region for nested host->guest call frames */
#include "flight_recorder.h"

/* Test-only scheduler liveness instrumentation (#290). Declared here, not in recomp.h, so the
 * CPU-state ABI header (and every title's generated-code build profile) stays unchanged. */

#ifdef SR_SCHED_LIVENESS_TEST
#define SR_SCHED_LIVENESS_MAX_OWNERS 256u
#define SR_SCHED_LIVENESS_TRACE_CAP 4096u

enum {
    SR_SCHED_LIVENESS_PICK = 1,
    SR_SCHED_LIVENESS_PREEMPT = 2,
    SR_SCHED_LIVENESS_BLOCK = 3,
    SR_SCHED_LIVENESS_WAKE = 4,
    SR_SCHED_LIVENESS_INTERRUPT = 5
};

typedef struct SrSchedLivenessSnapshot {
    uint64_t decisions;
    uint64_t picks;
    uint64_t preempt_checks;
    uint64_t block_transitions;
    uint64_t wake_transitions;
    uint64_t waiters_readied;
    uint64_t interrupt_transitions;
    uint64_t starvation_violations;
    uint64_t max_starvation_age;
    uint32_t last_owner_uid;
    uint32_t last_selected_uid;
    uint32_t trace_count;
} SrSchedLivenessSnapshot;

typedef struct SrSchedLivenessEvent {
    uint64_t sequence;
    uint32_t reason;
    uint32_t owner_uid;
    uint32_t selected_uid;
    uint32_t object_uid;
} SrSchedLivenessEvent;

void sched_liveness_reset(void);
void sched_liveness_snapshot(SrSchedLivenessSnapshot *out);
int sched_liveness_event(unsigned index, SrSchedLivenessEvent *out);
int sched_liveness_owner_counts(uint32_t uid, uint64_t *decisions,
                                uint64_t *selections, uint64_t *idle);
void sched_liveness_set_starvation_limit(unsigned limit);
#endif

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

int     sr_sched_on = 0;
atomic_int_least32_t sr_timeslice = 0;

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
#define SCE_KERNEL_ERROR_ILLEGAL_CONTEXT   0x80020064u
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR      0x80020191u

/* PSP hardware treats a signed-negative status as an error-shaped non-delete
 * exit and latches SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT.  The boundary probe
 * measured both ThreadMan errors (0x800201a8/0x800201ac) and ordinary -17;
 * positive values propagate.  Delete/self-unload paths do not call this seam. */
static int sched_status_is_negative(int32_t status) {
    return status < 0;
}

#define SR_STACK_ARENA_FLOOR 0x05000000u
/* Dedicated VBLANK interrupt stack reservation (Issue #98 / PORTING.md Section C-6).
 * 64 KiB region [0x09ef0000, 0x09f00000) structurally reserved between the
 * thread-stack arena ceiling and the nested host->guest frame region.
 * The thread-stack arena ceiling is derived from this base, ensuring that
 * thread stack allocation can never hand out or collide with the VBLANK interrupt stack. */
#define SR_VBLANK_STACK_SIZE 0x00010000u
#define SR_VBLANK_STACK_TOP  SR_NESTED_FRAME_BASE
#define SR_VBLANK_STACK_BASE (SR_VBLANK_STACK_TOP - SR_VBLANK_STACK_SIZE)

#define SR_STACK_ARENA_CEIL  SR_VBLANK_STACK_BASE
_Static_assert(SR_STACK_ARENA_CEIL == 0x09ef0000u,
               "deriving the ceiling from the VBLANK reservation must set expected floor");
_Static_assert(SR_STACK_ARENA_FLOOR < SR_STACK_ARENA_CEIL,
               "the thread-stack arena must be non-empty and below the VBLANK reservation");
_Static_assert(SR_STACK_ARENA_CEIL == SR_VBLANK_STACK_BASE,
               "the thread-stack arena must end exactly at the VBLANK reservation base");
_Static_assert(SR_VBLANK_STACK_TOP == SR_NESTED_FRAME_BASE,
               "the VBLANK reservation must end exactly at the nested-frame region base");
#define SR_STACK_RANGE_MAX   (MAXTHREADS + 1)

typedef struct {
    uint32_t base;
    uint32_t size;
} StackRange;

typedef struct {
    uint32_t uid;
    int      state;
    int      priority;
    int      init_priority;
    uint32_t attr;
    char     name[32];
    uint32_t entry, arglen, argp;
    SrCoro  *coro;               /* this thread's coroutine (sr_coro); NULL until first started */
    int      started;            /* coroutine has begun running its body */
    uint64_t wake;               /* scheduler tick to wake at (TH_WAIT_DELAY) */
    uint32_t wait_obj;           /* object uid this thread waits on (TH_WAIT_OBJ) */
    int      wait_kind;           /* PSP waitType while in TH_WAIT_OBJ (3=sema,4=evf,13=lwmutex) */
    int      pending_wait_kind;   /* latched by sched_set_current_wait_kind for the next block */
    uint32_t wake_result;         /* cancel/delete/release code for this waiter */
    int      wake_result_valid;   /* wake_result is pending for this thread */
    int      wakeups;            /* pending sceKernelWakeupThread count (sleep/wakeup semantics) */
    int      sleeping;           /* 1 while blocked in sceKernelSleepThread[CB] */
    int32_t  exit_status;        /* value passed to sceKernelExitThread */
    uint32_t sp_init, k0_init;   /* initial sp/k0 (to re-seed registers on a restart) */
    CpuState saved;              /* register file while not running */
    jmp_buf  unwind_jmp;         /* unwind point for clean fiber exit */
    int      has_unwind_jmp;     /* 1 when jump buffer is valid */
    int      hle_depth;          /* HLE execution depth when suspended */
    int      rt_phase;           /* runtime phase (perf.h) when suspended: attribution
                                  * must follow the RUNNING context, not the last one
                                  * to touch the global, or a blocking syscall's tag
                                  * would be blamed for the code that ran next */
    int      is_cb_wait;         /* 1 when thread is in callback-aware wait */
    int      deleted;             /* kernel object has been removed; slot may be recycled */
    SrWaitHandle active_wait;    /* only the currently attached semantic block */
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

/* Heap-owned semantic records survive nonlocal C-frame abandonment. TCB slots
 * cannot be reused until owner teardown drains these records; numeric handles
 * never recycle, including across thread restarts and fixture resets. */
typedef struct WaitInvocation {
    SrWaitHandle handle;
    TCB *owner;
    uint32_t owner_uid, object;
    uint64_t deadline;
    int callback_enabled, result_valid, thread_join;
    uint32_t result;
    SrWaitState state;
    SrWaitDetach detach;
    struct WaitInvocation *next;
} WaitInvocation;
static WaitInvocation *s_wait_invocations;
static SrWaitHandle s_wait_sequence;
static void sched_wait_abandon_owner(TCB *owner);
static void sched_wait_record_wake(TCB *owner, int terminal, uint32_t result);

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
/* At least one display source period elapsed while the CPU interrupt bit was
 * clear, and its single coalesced delivery has not been credited yet.  This is
 * a flag rather than a count on purpose: the measured resume credit is one
 * whether one period or several elapsed under the mask, so ten masked periods
 * and one masked period are indistinguishable to the guest.  See
 * sched_resume_interrupts(). */
static int      s_vblank_masked_pending;
/* Interrupt state is a gate on delivery, not a gate on the scheduler clock.  A
 * source bit remains latched until the eligible handler consumes it. */
static uint32_t s_pending_interrupts;
static int      s_servicing_interrupts;
static SrCoro  *s_sched_coro = NULL;
CpuState *s_cpu = NULL;
/* Read by tools/mem_debug.py: a live debugger refuses CpuState access unless the
 * attached image reports the ABI it was compiled against. */
const uint32_t sr_cpustate_abi_version __attribute__((used)) = SR_CPUSTATE_ABI_VERSION;
static uint64_t s_tick = 0;
static uint32_t s_gp = 0x002d0000;        /* module global pointer, inherited by created threads */
static uint32_t s_stack_top = SR_STACK_ARENA_CEIL;  /* sibling thread stacks grow down from here */
static StackRange s_stack_free[SR_STACK_RANGE_MAX];
static int s_stack_free_count;
static int s_stack_allocator_ready;

#ifdef SR_SCHED_LIVENESS_TEST
typedef struct {
    uint32_t uid;
    uint64_t decisions;
    uint64_t selections;
    uint64_t idle;
} SrSchedLivenessOwner;

static SrSchedLivenessOwner s_sched_liveness_owners[SR_SCHED_LIVENESS_MAX_OWNERS];
static unsigned s_sched_liveness_owner_count;
static SrSchedLivenessEvent s_sched_liveness_events[SR_SCHED_LIVENESS_TRACE_CAP];
static uint64_t s_sched_liveness_event_total;
static uint64_t s_sched_liveness_starvation_limit = 64u;
static uint64_t s_sched_liveness_starvation_ages[MAXTHREADS];
static uint32_t s_sched_liveness_owner_uid;
static SrSchedLivenessSnapshot s_sched_liveness;

static int sched_liveness_owner_slot(uint32_t uid) {
    for (unsigned i = 0; i < s_sched_liveness_owner_count; i++)
        if (s_sched_liveness_owners[i].uid == uid) return (int)i;
    if (s_sched_liveness_owner_count >= SR_SCHED_LIVENESS_MAX_OWNERS) return -1;
    unsigned slot = s_sched_liveness_owner_count++;
    s_sched_liveness_owners[slot].uid = uid;
    return (int)slot;
}

static void sched_liveness_sync_owner(void) {
    if (s_cur >= 0 && s_cur < s_ntcb && !s_tcb[s_cur].deleted)
        s_sched_liveness_owner_uid = s_tcb[s_cur].uid;
}

static uint32_t sched_liveness_uid_at(int index) {
    return index >= 0 && index < s_ntcb && !s_tcb[index].deleted
               ? s_tcb[index].uid : 0u;
}

static void sched_liveness_observe_pick(int selected) {
    if (selected < 0 || selected >= s_ntcb) return;
    int selected_priority = s_tcb[selected].priority;
    for (int i = 0; i < s_ntcb; i++) {
        if (s_tcb[i].state != TH_READY || i == selected ||
            s_tcb[i].priority >= selected_priority) {
            s_sched_liveness_starvation_ages[i] = 0;
            continue;
        }
        s_sched_liveness_starvation_ages[i]++;
        if (s_sched_liveness_starvation_ages[i] > s_sched_liveness.max_starvation_age)
            s_sched_liveness.max_starvation_age = s_sched_liveness_starvation_ages[i];
        if (s_sched_liveness_starvation_ages[i] > s_sched_liveness_starvation_limit)
            s_sched_liveness.starvation_violations++;
    }
}

static void sched_liveness_note(uint32_t reason, int selected, uint32_t object_uid) {
    sched_liveness_sync_owner();
    uint32_t owner_uid = s_sched_liveness_owner_uid;
    uint32_t selected_uid = sched_liveness_uid_at(selected);
    uint64_t sequence = s_sched_liveness_event_total++;
    unsigned slot = sequence < SR_SCHED_LIVENESS_TRACE_CAP
                        ? (unsigned)sequence
                        : (unsigned)((sequence - SR_SCHED_LIVENESS_TRACE_CAP) %
                                     SR_SCHED_LIVENESS_TRACE_CAP);
    s_sched_liveness_events[slot] = (SrSchedLivenessEvent){
        sequence, reason, owner_uid, selected_uid, object_uid
    };
    s_sched_liveness.last_owner_uid = owner_uid;
    s_sched_liveness.last_selected_uid = selected_uid;

    if (reason == SR_SCHED_LIVENESS_PICK || reason == SR_SCHED_LIVENESS_PREEMPT) {
        int owner_slot = sched_liveness_owner_slot(owner_uid);
        if (owner_slot >= 0) s_sched_liveness_owners[owner_slot].decisions++;
        s_sched_liveness.decisions++;
        if (selected_uid) {
            int selected_slot = sched_liveness_owner_slot(selected_uid);
            if (selected_slot >= 0) s_sched_liveness_owners[selected_slot].selections++;
        } else {
            int owner_slot_idle = sched_liveness_owner_slot(owner_uid);
            if (owner_slot_idle >= 0) s_sched_liveness_owners[owner_slot_idle].idle++;
        }
        if (reason == SR_SCHED_LIVENESS_PICK) {
            s_sched_liveness.picks++;
            sched_liveness_observe_pick(selected);
        } else {
            s_sched_liveness.preempt_checks++;
        }
    } else if (reason == SR_SCHED_LIVENESS_BLOCK) {
        s_sched_liveness.block_transitions++;
    } else if (reason == SR_SCHED_LIVENESS_WAKE) {
        s_sched_liveness.wake_transitions++;
    } else if (reason == SR_SCHED_LIVENESS_INTERRUPT) {
        s_sched_liveness.interrupt_transitions++;
    }
}

static void sched_liveness_record_wake(uint32_t object_uid, uint64_t count) {
    if (!count) return;
    s_sched_liveness.waiters_readied += count;
    sched_liveness_note(SR_SCHED_LIVENESS_WAKE, -1, object_uid);
}

static void sched_liveness_set_owner(uint32_t uid) {
    s_sched_liveness_owner_uid = uid;
}

void sched_liveness_reset(void) {
    memset(s_sched_liveness_owners, 0, sizeof(s_sched_liveness_owners));
    memset(s_sched_liveness_events, 0, sizeof(s_sched_liveness_events));
    memset(s_sched_liveness_starvation_ages, 0, sizeof(s_sched_liveness_starvation_ages));
    memset(&s_sched_liveness, 0, sizeof(s_sched_liveness));
    s_sched_liveness_owner_count = 0;
    s_sched_liveness_event_total = 0;
    s_sched_liveness_owner_uid = 0;
    s_sched_liveness_starvation_limit = 64u;
}

void sched_liveness_snapshot(SrSchedLivenessSnapshot *out) {
    if (!out) return;
    *out = s_sched_liveness;
    uint64_t total = s_sched_liveness_event_total;
    out->trace_count = (uint32_t)(total < SR_SCHED_LIVENESS_TRACE_CAP
                                      ? total : SR_SCHED_LIVENESS_TRACE_CAP);
}

int sched_liveness_event(unsigned index, SrSchedLivenessEvent *out) {
    if (!out || index >= SR_SCHED_LIVENESS_TRACE_CAP) return 0;
    uint64_t total = s_sched_liveness_event_total;
    uint64_t count = total < SR_SCHED_LIVENESS_TRACE_CAP
                         ? total : SR_SCHED_LIVENESS_TRACE_CAP;
    if (index >= count) return 0;
    uint64_t first = total > SR_SCHED_LIVENESS_TRACE_CAP
                         ? total - SR_SCHED_LIVENESS_TRACE_CAP : 0;
    uint64_t physical = (first + index) % SR_SCHED_LIVENESS_TRACE_CAP;
    *out = s_sched_liveness_events[physical];
    return 1;
}

int sched_liveness_owner_counts(uint32_t uid, uint64_t *decisions,
                                uint64_t *selections, uint64_t *idle) {
    for (unsigned i = 0; i < s_sched_liveness_owner_count; i++) {
        if (s_sched_liveness_owners[i].uid != uid) continue;
        if (decisions) *decisions = s_sched_liveness_owners[i].decisions;
        if (selections) *selections = s_sched_liveness_owners[i].selections;
        if (idle) *idle = s_sched_liveness_owners[i].idle;
        return 1;
    }
    return 0;
}

void sched_liveness_set_starvation_limit(unsigned limit) {
    s_sched_liveness_starvation_limit = limit ? limit : 1u;
}

#define SCHED_LIVENESS_NOTE(reason, selected, object_uid) \
    sched_liveness_note((reason), (selected), (object_uid))
#define SCHED_LIVENESS_OWNER(uid) sched_liveness_set_owner(uid)
#else
#define SCHED_LIVENESS_NOTE(reason, selected, object_uid) ((void)0)
#define SCHED_LIVENESS_OWNER(uid) ((void)0)
#endif

/* Dynamic role-UID capture. Populated by sched_create_thread: the ROOT thread is the
 * first thread ever created (sched_run's module_start thread); the WORKER and LAUNCHER
 * are the threads whose entry matches the build's configured worker/launcher binding
 * (see title_config.h). With no title configuration neither role is ever claimed.
 *
 * All three start UNCAPTURED. They previously started at the historical HST allocation
 * (0x110 / 0x114 / 0x111), which made them live comparison values before any capture:
 * UIDs are handed out from 0x110 upward, so an ordinary thread in a build with no
 * launcher binding could be allocated 0x111 and then inherit launcher-only treatment
 * (master-reent seeding, skipped guest reent registration) purely by its number. A role
 * UID is an outcome of allocation, never title configuration, so "absent" is represented
 * structurally instead -- SR_ROLE_UID_NONE, a value sr_alloc_uid() never returns.
 *
 * Read them via sched_root_uid() / sched_worker_uid() / sched_launcher_uid() when the
 * number itself is wanted; ask sched_uid_is_*() when the question is "does this thread
 * hold the role", because those fail closed while the role is uncaptured. */
uint32_t g_root_uid     = SR_ROLE_UID_NONE;
uint32_t g_worker_uid   = SR_ROLE_UID_NONE;
uint32_t g_launcher_uid = SR_ROLE_UID_NONE;
static int s_root_seen  = 0;      /* first-created-thread latch (file-scope for the selftest) */

/* Nested host->guest call frames still held when a thread's resources are
 * released.  This should always be zero: it counts release paths that were
 * missed, not frames that were expected to survive. */
static unsigned s_stranded_nested_frames;

/* Defined below, after the virtual-time service and VBLANK frame are declared. */
static void scheduler_progress_time(void);
static void scheduler_latch_due_events(void);
static void vtime_refresh(void);

/* VBLANK episodes the display source has raised that no eligible service point
 * has delivered yet.  This is the runtime's model of the PSP's IF bit for the
 * VBLANK source, and it is a COUNT, not a flag: the hardware re-asserts an
 * interrupt source for every edge that arrives while the previous one is still
 * pending, so a guest that spends two display periods inside one stretch with
 * no service point is owed two handler episodes, not one.  A single pending bit
 * made the guest observe fewer VBLANK episodes than display periods while guest
 * VCOUNT (which has always advanced by the full elapsed count) said otherwise. */
static uint32_t s_pending_vblanks;

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
static uint32_t sr_rt_current_pc(void);
static uint32_t sr_rt_current_uid(void);

void sched_init(CpuState *cpu) {
    sr_flight_init();
    s_cpu = cpu;
    sr_rt_pc_fn = sr_rt_current_pc;
    sr_rt_uid_fn = sr_rt_current_uid;
    if (cpu->r[28] != 0u) s_gp = cpu->r[28];           /* the driver seeded gp from the module's # init */
    s_sched_coro = sr_coro_main();
    sr_sched_on = 1;
    s_interrupts_enabled = 1;
    s_dispatch_enabled = 1;
    s_pending_interrupts = 0;
    s_servicing_interrupts = 0;
    s_vblank_masked_pending = 0;
    atomic_store_explicit(&sr_timeslice, TIMESLICE, memory_order_relaxed);

    memset(s_tcb, 0, sizeof(s_tcb));
    memset(s_libc_threads, 0, sizeof(s_libc_threads));
    s_ntcb = 0;
    s_cur = -1;
    s_last_pick = -1;
    s_root_seen = 0;
    s_tick = 0;
#ifdef SR_SCHED_LIVENESS_TEST
    sched_liveness_reset();
#endif
    stack_ranges_reset();
}

uint32_t sched_current_uid(void) { return s_cur >= 0 ? s_tcb[s_cur].uid : 0; }
uint32_t sched_root_uid(void)     { return g_root_uid; }
uint32_t sched_worker_uid(void)    { return g_worker_uid; }
uint32_t sched_launcher_uid(void) { return g_launcher_uid; }

int sched_role_uid_captured(uint32_t role_uid) { return role_uid != SR_ROLE_UID_NONE; }

/* The one place a role question is answered. Both halves fail closed:
 *   - an uncaptured role matches no thread at all, so a build with no worker/launcher
 *     binding can never grant role treatment to an ordinary thread by its number;
 *   - UID 0 is PSP's "current thread" / "no current thread" value and is never a real
 *     thread identity, so it can never satisfy a captured-role test either. Without
 *     that guard, sched_current_is_worker() with no current thread would compare 0
 *     against 0 the moment any sentinel scheme used zero for "absent". */
static int role_uid_matches(uint32_t role_uid, uint32_t uid) {
    if (role_uid == SR_ROLE_UID_NONE) return 0;
    if (uid == SR_ROLE_UID_NONE || uid == 0u) return 0;
    return role_uid == uid;
}

int sched_uid_is_root(uint32_t uid)     { return role_uid_matches(g_root_uid, uid); }
int sched_uid_is_worker(uint32_t uid)   { return role_uid_matches(g_worker_uid, uid); }
int sched_uid_is_launcher(uint32_t uid) { return role_uid_matches(g_launcher_uid, uid); }
int sched_current_is_worker(void)       { return sched_uid_is_worker(sched_current_uid()); }
int sched_current_is_launcher(void)     { return sched_uid_is_launcher(sched_current_uid()); }
uint32_t sr_thread_k0(void)        { return s_cur >= 0 ? s_tcb[s_cur].k0_init : 0; }

/* Clearing the interrupt bit is a point on the display timeline, and every
 * period before that point asserted its interrupt with delivery still enabled.
 * Consume them here, while the bit is still set, so they are credited to the
 * regime they actually elapsed in.
 *
 * Without this the periods stay undiscovered until some later latch, and the
 * most frequent later latch is the one sched_resume_interrupts() performs
 * before restoring the bit -- which classifies them as masked and drops them.
 * A title that brackets its allocator with CpuSuspendIntr/CpuResumeIntr
 * thousands of times a second therefore loses most of its VCOUNT to a
 * discovery-time artifact, with the mask itself held for a negligible
 * fraction of wall time.
 *
 * vtime_refresh() rather than scheduler_progress_time(): the latter injects a
 * fixed deterministic quantum in turbo mode, which at this call frequency
 * would run the virtual clock away from the run.  This samples the clock (in
 * paced mode) and latches what is already due, and never services or
 * schedules -- no guest handler may run from inside CpuSuspendIntr. */
/* Entering a masked window.
 *
 * The clamp that used to live here -- "keep exactly one owed episode" -- destroyed
 * periods that had elapsed with the I-bit SET, and that is the wrong half of the
 * level to collapse.  Two different things are being modelled and only one of them
 * is a level:
 *
 *   - Edges that arrive while IE is CLEAR collapse.  HARDWARE_MEASURED
 *     (display-mask-vcount, PSP-3001 / 6.61-ARK, 12/12 trials at 4/16.7/30/50 ms):
 *     however many source periods the window covered, resume produces exactly ONE
 *     coalesced delivery and credits VCOUNT exactly one.  That is
 *     s_vblank_masked_pending, a flag, and it is the rule this file implements.
 *
 *   - Edges that arrived while IE was SET do NOT collapse.  That is the runtime's own
 *     documented model rather than a guess: with the I-bit set the source latch advances
 *     guest VCOUNT by every elapsed period (docs/ARCHITECTURE.md, display domain), so
 *     those periods are counted as owed, and a clamp that destroyed the delivery left the
 *     guest with a VCOUNT claiming periods the VBLANK handler never saw.  It also matches
 *     the qualified record: the coalesced delivery in PSP-DISPLAY-001 was observed across
 *     a MASKED window, so the collapse it measured is the window's, not the backlog's.
 *     The runtime only recognises an interrupt at a service point, so a period that came
 *     due inside a long syscall is still owed when the guest masks -- and the clamp's only
 *     caller is sceKernelCpuSuspendIntr, so a title that masks around every allocation
 *     paid the whole cost.  A/B on the flagship (one build, 480 s runs, machine otherwise
 *     idle): with the clamp 1,337 episodes owed and never delivered over 419 presenting
 *     seconds -- 3.19 Hz -- and a delivered rate of 56.73 Hz against the 59.94 Hz source,
 *     so the two agree to 0.02 Hz and the clamp is the entire residual; without it, three
 *     runs measured 59.86, 59.93 and 59.94 Hz with dropped=0.
 *
 * So the backlog is left intact here and delivered at the next eligible service point,
 * which is the resume.  No guest handler may run from inside sceKernelCpuSuspendIntr, so
 * deferring is the only option -- and it is a latency artifact of the runtime, not a
 * semantic one. */
static void sched_enter_masked(void) {
    vtime_refresh();
    s_vblank_masked_pending = 0;
}

uint32_t sched_suspend_interrupts(void) {
    uint32_t previous = s_interrupts_enabled ? 1u : 0u;
    if (previous) sched_enter_masked();
    s_interrupts_enabled = 0;
    return previous;
}

void sched_resume_interrupts(uint32_t state) {
    /* SuspendIntr returns exactly the prior I-bit (0 or 1).  Do not treat an
     * arbitrary guest value as an enable request: the hardware probe leaves
     * the CPU disabled for an invalid token such as 0xDEADBEEF, and ignoring
     * that malformed restore avoids manufacturing an interrupt transition. */
    if (state > 1u) return;
    if (!state) {
        if (s_interrupts_enabled) sched_enter_masked();
        s_interrupts_enabled = 0;
        return;
    }
    if (s_servicing_interrupts) {
        s_interrupts_enabled = 1;
        return;
    }

    /* Resume is a scheduler boundary: host time is sampled, elapsed source
     * events are latched, eligible handlers run in priority order, and a
     * higher-priority waiter may preempt the interrupted thread.  When this is
     * the disabled->enabled transition, account the elapsed interval before
     * restoring the I-bit.  Otherwise a period first discovered by ResumeIntr
     * would be misclassified as enabled time and spuriously advance VCOUNT even
     * though the complete interval elapsed under the mask. */
    int was_enabled = s_interrupts_enabled;
    if (!was_enabled)
        scheduler_progress_time();
    s_interrupts_enabled = 1;
    if (!was_enabled && s_vblank_masked_pending) {
        /* HARDWARE_MEASURED (PSP-3001 / 6.61-ARK, 12/12 trials at each of
         * 4/16.7/30/50 ms): however many source periods elapsed under the mask,
         * resume credits VCOUNT exactly ONE -- +1 when one period crossed, +1
         * when multiple crossed, and +0 at 4 ms where none did.  Never N, and
         * never zero when a period did become pending. */
        sr_display_advance_vcount(1u);
        /* ... and the measured interrupt-conformance record is one coalesced
         * VBLANK handler delivery on resume, not a VCOUNT credit with no event.
         * Owe that episode explicitly instead of leaving it to the source bit:
         * with a pre-mask backlog still owed, the delivery loop takes its count
         * from the backlog and the bit is consumed by the same batch, so the
         * window's own edge would otherwise never be delivered at all. */
        if (s_pending_vblanks < UINT32_MAX) {
            s_pending_vblanks++;
            sr_perf_vblank_coalesced();
        }
        s_vblank_masked_pending = 0;
    }
    if (was_enabled)
        scheduler_progress_time();
    scheduler_latch_due_events();
    scheduler_service_pending();
    sched_preempt();
}

int sched_interrupts_enabled(void) {
    return s_interrupts_enabled;
}

int sched_is_intr_context(void) {
    return s_cur < 0;
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
    SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_INTERRUPT, -1, source);
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
/* The deadline a thread carries when nothing but a signal can release it.
 * Named so the "infinite waits never expire" rule in
 * sched_promote_expired_waits() is checkable rather than a bare -1. */
#define SCHED_WAIT_FOREVER ((uint64_t)-1)

/* Consecutive idle scheduler iterations that may deliver no VBLANK and ready no
 * thread before the runtime declares no-progress. The unit is scheduler
 * iterations over a source that is supposed to fire once per iteration, so this
 * is a delivered-event contract rather than a wall-clock timeout: a healthy
 * paced idle loop resets it every iteration and can never approach it. */
#define SCHED_IDLE_NO_PROGRESS_LIMIT 64

static uint64_t s_vbl_count = 0;     /* vblanks delivered so far */
/* Guest-time stamp of the most recent delivered VBLANK. The guest-visible vblank
 * interval is measured from this edge (see sched_display_is_vblank), so it has to
 * track real delivery rather than a free-running phase. */
static uint64_t s_vbl_last_us = 0;
/* Host stamp of the previous display-source latch.  The gap between consecutive
 * latches IS the service-point granularity: a gap wider than one display period
 * means periods came due with no eligible point to service them. */
static uint64_t s_vbl_latch_ns;

/* The live guest PC, for service-cadence attribution.  Read only when a period
 * came due late, so it never sits on the instruction path. */
static uint32_t sr_rt_current_pc(void) {
    return s_cpu ? s_cpu->pc : 0u;
}
static uint32_t sr_rt_current_uid(void) {
    return (s_cur >= 0 && s_cur < s_ntcb) ? s_tcb[s_cur].uid : 0u;
}
/* PSP display waits, as measured on PSP-3001/6.61-ARK (probe cases
 * `display-wait-late`, `display-vblank-window`; records PSP-DISPLAY-002 and
 * PSP-DISPLAY-004).
 *
 * There is no missed-edge memory. The scheduler used to carry a per-thread
 * `vbl_seen` latch: if a VBLANK had been delivered since this thread last
 * completed a display wait, the wait returned immediately and consumed the
 * missed edge, on the theory that blocking would hard-quantize a frame whose
 * work crossed the period. Hardware says otherwise, and says it without
 * ambiguity. With 0.25, 0.75, 1.25, 1.75 and 2.5 periods of un-yielded CPU spin
 * since the last edge, sceDisplayWaitVblankStart blocked on all 240 trials,
 * waited exactly the remainder of the period in progress, and advanced VCOUNT by
 * exactly 1 -- never 0, and never 2 even when two whole edges had been missed.
 * A late caller is not owed the edges it slept through. The latch is therefore
 * gone, along with the TCB field that backed it.
 *
 * The two NIDs differ in exactly one place. At every phase OUTSIDE the vblank
 * interval the two calls are indistinguishable (240/240 trials each: blocked,
 * VCOUNT +1, returned 0). Called from INSIDE the interval, sceDisplayWaitVblank
 * returns immediately -- 3..5 us, i.e. syscall overhead -- and returns 1, while
 * sceDisplayWaitVblankStart waits a full period (16.669..16.693 ms) and returns
 * 0. That single cell is the whole difference between them.
 *
 * Neither call consults any other thread's state. Making the block/return
 * decision depend on whether some unrelated thread is TH_READY is not a PSP
 * semantic and is not expressible on hardware; see the D2 record
 * (PSP-DISPLAY-003) quoted above sched_run's idle handling. */
static void sched_vblank_block(void) {
    sched_block_on(VBLANK_WAIT_OBJ);
}

/* sceDisplayWaitVblankStart: block until the NEXT vblank start edge, always. */
void sched_wait_vblank_start(void) {
    sched_vblank_block();
}

/* sceDisplayWaitVblank: identical, except that a caller already inside the
 * vblank interval returns without blocking. Returns 1 in that case (the value
 * hardware returns), 0 when it blocked to the next edge. */
int sched_wait_vblank(void) {
    if (sched_display_is_vblank()) return 1;
    sched_vblank_block();
    return 0;
}

/* Callback-aware display waits return only after the vblank condition was met.
 * A queued callback can ready the thread before the display source does, so the
 * CB path compares the delivered-edge counter and re-enters the same wait until
 * the next start edge is observed. WaitVblank keeps its measured in-window fast
 * return, but services a callback already pending for the caller first. */
int sched_wait_vblank_cb(int wait_start) {
    extern int sr_thread_has_pending_callbacks(uint32_t thread_uid);
    extern int sr_thread_dispatch_callbacks(void);
    if (s_cur < 0 || s_cur >= s_ntcb) {
        /* No current thread means no waiter exists; report it instead of a
         * synthesized "waited and returned 0". Callers map this to CAN_NOT_WAIT. */
        fprintf(stderr, "SCHED: display CB wait without a current thread (s_cur=%d); "
                        "refusing instead of reporting a completed wait\n", s_cur);
        return -1;
    }

    uint32_t uid = s_tcb[s_cur].uid;
    uint64_t target_vblank = s_vbl_count + 1u;
    for (;;) {
        if (sr_thread_has_pending_callbacks(uid))
            sr_thread_dispatch_callbacks();
        if (s_vbl_count >= target_vblank) return 0;

        sched_set_current_cb_wait(1);
        int returned_in_vblank = wait_start ? 0 : sched_wait_vblank();
        if (wait_start) sched_wait_vblank_start();
        sched_set_current_cb_wait(0);
        if (returned_in_vblank) return 1;
    }
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

static void pace_setup(void) {
    if (s_pace_on >= 0) return;
    const char *e = getenv("SR_NOVBPACE");
    if (!e || e[0] == '\0' || strcmp(e, "0") == 0) {
        s_pace_on = 1;
    } else if (strcmp(e, "1") == 0) {
        s_pace_on = 0;
    } else {
        fprintf(stderr, "SR_NOVBPACE: invalid value '%s'; must be unset, empty, '0' (paced), or '1' (turbo)\n", e);
        fflush(stderr);
        exit(1);
    }
    fprintf(stderr, "pace_setup: s_pace_on = %d (SR_NOVBPACE = %s)\n", s_pace_on, e ? e : "<unset>");
    fflush(stderr);
    s_clock_epoch_ns = host_now_ns();
    s_vbl_next_ns = s_clock_epoch_ns;
    s_vbl_period_rem = 0;
    s_vtime_period_rem = 0;
    s_vbl_next_us = 0;
    s_vbl_event_period_rem = 0;
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
 * event.  The VBLANK source is intentionally coalescing on the delivery side: a
 * long suspension advances all deadlines but leaves one pending bit for the
 * interrupt/service path.  Display-period accounting (guest-visible VCOUNT) is
 * separate and tracks the total elapsed periods, not the delivered episodes. */
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
}

static void scheduler_latch_due_events(void) {
    /* The host clock is read only when a period is actually due, so the common
     * "nothing due yet" service point stays a single guest-time comparison. */
    if (!(s_vtime_us >= s_vbl_next_us && s_vbl_next_us != UINT64_MAX)) return;
    uint32_t due = 0;
    uint32_t masked = 0;
    uint64_t latch_ns = host_now_ns();
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
        /* Separate the two concepts the source owns: the elapsed display period
         * count (guest-visible VCOUNT, advanced at source-latch boundaries)
         * advances by `count`, and the serviced VBLANK event is now a COUNT of
         * `count` episodes rather than one coalesced bit.  Both halves advanced
         * by the elapsed period count; the VBLANK side used to stop at one,
         * which is why a title whose frame work crossed a period saw a whole
         * period disappear while VCOUNT kept counting.
         *
         * CPU interrupt masking is a third, distinct state, and it gates this
         * accounting.  Qualified PSP measurements show that system time advances
         * across a CpuSuspendIntr window while guest VCOUNT and VBLANK handler
         * calls remain frozen.  Guest-visible VCOUNT therefore is not modelled as
         * a raw free-running display register: with interrupts enabled it advances
         * by elapsed source periods even when service is starved, and while the
         * CPU interrupt bit is clear it stops.
         *
         * Deadlines still advance below, so the periods that elapse under a mask
         * are consumed rather than replayed: on resume the coalesced pending bit
         * delivers exactly one episode and credits VCOUNT exactly one, which is
         * what the hardware probe measured at every mask length it tested, from
         * a quarter of a display period up to three.  No N-period catch-up is
         * applied, and no increment at all when no period became pending. */
        if (s_interrupts_enabled) {
            sr_display_advance_vcount((uint32_t)count);
            /* count is 64-bit and the tallies are 32-bit: compare against the
             * headroom, because a 64-bit sum can never wrap and a truncating
             * add would. Saturate, never wrap. */
            if (count > (uint64_t)(UINT32_MAX - due)) due = UINT32_MAX;
            else due += (uint32_t)count;
        } else {
            s_vblank_masked_pending = 1;   /* coalesced; credited once at resume */
            if (count > (uint64_t)(UINT32_MAX - masked)) masked = UINT32_MAX;
            else masked += (uint32_t)count;
        }
        scheduler_advance_vblank_deadlines(count);
        /* A source timeline that saturates cannot express the periods it just
         * crossed, and the owed-episode count has to stay a count the runtime can
         * actually deliver: this is the same "a saturated source deadline raises
         * nothing further" boundary the idle path already reports, seen from the
         * latch side.  The elapsed periods collapse into the ONE episode the
         * overflow can name, the queue is emptied, and both facts are counted --
         * a silent queue here would run four billion handler episodes -- before the
         * loop ends because the deadline is now saturated. */
        if (s_vbl_next_us == UINT64_MAX) {
            if (due > 1u) {
                sr_perf_vblank_collapse(s_pending_vblanks, due - 1u);
                due = 1u;
                s_pending_vblanks = 0;
            }
            break;
        }
    }
    if (masked) sr_perf_vblank_latch(0u, masked, 1);
    if (!due) return;
    /* Accumulate across latches: each elapsed period is owed its own episode, and
     * an undelivered episode is never dropped to make room for a later one. */
    s_pending_vblanks = due > UINT32_MAX - s_pending_vblanks
                          ? UINT32_MAX : s_pending_vblanks + due;
    uint64_t gap_us = latch_ns > s_vbl_latch_ns
                        ? (latch_ns - s_vbl_latch_ns) / 1000u : 0u;
    s_vbl_latch_ns = latch_ns;
    sr_perf_vblank_latch(gap_us, due, 0);
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
    sr_rt_phase = SR_RT_PHASE_HOST_WAIT;
    for (;;) {
        uint64_t now = host_us();
        if (now >= target_us) break;
        SDL_DelayPrecise((target_us - now) * 1000u);
    }
    sr_rt_phase = SR_RT_PHASE_SCHED;
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
        sr_rt_phase = SR_RT_PHASE_HOST_WAIT;
        SDL_DelayPrecise(s_vbl_next_ns - now);
        sr_perf_guest_idle_wait(wait_started);
        sr_rt_phase = SR_RT_PHASE_SCHED;
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
    s_vbl_last_us = s_vtime_us;   /* start edge: the vblank interval runs from here */
    sr_perf_vblank();

    /* A delivered VBLANK writes NO guest memory. The guest's own VBLANK sub-interrupt
     * handler (dispatched below) advances its frame/vsync words; the runtime has no
     * evidence that PSP firmware writes title memory at VBLANK (assumed from the
     * documented sub-interrupt model, not measured). An earlier build also added one to
     * a title-named pair of words here, so a guest whose handler maintains the same words
     * saw every VBLANK counted twice and a loop gated on "two VBLANKs elapsed" passed after
     * one. Measured on the flagship title screen against PPSSPP: 55.16 vs 30.0 game
     * frames/s before (1.84x), 27.75 after. */

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
    s_cpu->r[29] = SR_VBLANK_STACK_TOP; /* dedicated interrupt stack (above thread stacks, below nested frames) */
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
        fflush(stderr);
    }
    if (h) dispatch(s_cpu, h);
    if (h && getenv("SR_CBSNAP")) {
        fprintf(stderr, "CBSNAP: legacy-post entry=0x%08x v0=0x%08x a0=0x%08x sp=0x%08x ra=0x%08x\n",
                h, s_cpu->r[2], s_cpu->r[4], s_cpu->r[29], s_cpu->r[31]);
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
            /* No captured worker role means there is nothing to re-arm. Looking the
             * sentinel up would find no TCB anyway; the explicit guard says why. */
            TCB *w = sched_role_uid_captured(wuid) ? tcb_by_uid(wuid) : NULL;
            if (w && w->state == TH_DORMANT) {
                /* A dormant worker means main_RunGameLoop returned for this frame; on real
                 * PSP the GE list-complete callback re-arms it. The previous gate required
                 * the 0x331b80 frame-ready counter to be exactly 0, but that counter's
                 * polarity is owned by the guest's finish callback (ge.c GE_FINISH_CB ->
                 * func 0x599c) and is not reliably reset here -- so a dormant worker could
                 * be held forever, presenting only frame 0. Re-arm whenever the worker is
                 * dormant; the relaunch is the surrogate for the GE callback. */
                fprintf(stderr, "RELAUNCH: worker 0x%x dormant, restarting to entry 0x%08x\n", wuid, w->entry);
                sched_start_thread(wuid, 0, 0);
            }
        }
    }
}

/* Service only sources whose delivery semantics are implemented. Unknown
 * source bits deliberately remain pending rather than being silently dropped;
 * adding a source handler later cannot lose an event raised today. */
/* Can the runtime still produce AND deliver a VBLANK?
 *
 * A thread parked on VBLANK_WAIT_OBJ carries no finite deadline, so no guest
 * timer can wake it -- but the runtime itself is the event source, and
 * deliver_vblank() wakes every VBLANK waiter unconditionally. "No guest timer
 * can wake this" is therefore not the same claim as "nothing can wake this",
 * and the idle path has to test the second one.
 *
 * Both halves of the chain are required:
 *   - production: scheduler_latch_due_events() raises nothing once the source
 *     deadline has saturated, so a saturated deadline means no further edges;
 *   - delivery: scheduler_service_pending() is a no-op while interrupts are
 *     disabled, so a raised source is never turned into a wake either.
 *
 * With either half missing the wait is genuinely unsatisfiable and must be
 * reported, not waited on. Dispatch state is deliberately NOT part of this:
 * suspended dispatch stops thread switching, not interrupt service, and VBLANK
 * delivery continues across it. */
static int vblank_event_producible(void) {
    return s_interrupts_enabled && s_vbl_next_us != UINT64_MAX;
}

/* The scheduler's idle classification, as a pure read of scheduler state.
 *
 * Factored out of sched_run so the policy is directly testable: the loop it used
 * to live in is only reachable from driver.c, which is why the previous version
 * of this decision shipped with no executable coverage at all. */
typedef struct {
    uint64_t soonest;        /* earliest finite wake deadline, else SCHED_WAIT_FOREVER */
    int waiting_on_vblank;   /* a live thread is parked on VBLANK_WAIT_OBJ */
    int vblank_can_wake;     /* ...and the source can still fire AND be serviced */
    int unwakeable;          /* nothing the runtime can do will ready anyone */
} SchedIdleState;

static SchedIdleState sched_classify_idle(void) {
    SchedIdleState st;
    st.soonest = SCHED_WAIT_FOREVER;
    st.waiting_on_vblank = 0;
    for (int i = 0; i < s_ntcb; i++) {
        if (s_tcb[i].state != TH_WAIT_DELAY && s_tcb[i].state != TH_WAIT_OBJ) continue;
        if (s_tcb[i].wake < st.soonest) st.soonest = s_tcb[i].wake;
        if (!s_tcb[i].deleted && s_tcb[i].state == TH_WAIT_OBJ &&
            s_tcb[i].wait_obj == VBLANK_WAIT_OBJ)
            st.waiting_on_vblank = 1;
    }
    /* Object identity alone is not a licence to keep spinning: the source has to
     * still be able to fire and to be serviced. */
    st.vblank_can_wake = st.waiting_on_vblank && vblank_event_producible();
    st.unwakeable = (st.soonest == SCHED_WAIT_FOREVER) && !st.vblank_can_wake;
    return st;
}

/* Deliver every VBLANK episode the display source has raised.
 *
 * The source raises a COUNT (s_pending_vblanks), one per elapsed display period,
 * because a hardware interrupt is recognized for every edge that arrives while
 * the CPU is still busy with the previous one: the IF bit stays set, the handler
 * runs, and it runs again immediately for each further edge.  A single pending
 * flag turned N elapsed periods into one episode, so a guest whose frame work
 * crossed a period silently lost one -- which is what made a vblank-paced title
 * run at 55 Hz with an exactly-correct VCOUNT.
 *
 * Interrupt masking is deliberately NOT replayed here: an episode the guest chose not
 * to service inside a masked window is not owed again, exactly as the hardware loses
 * it (one pending edge, one handler call).  Masking does not, however, discard what
 * the source owed before the window opened -- see sched_enter_masked(). */
static void scheduler_service_pending(void) {
    if (!s_interrupts_enabled || s_servicing_interrupts) return;
    s_servicing_interrupts = 1;
    while (s_interrupts_enabled &&
           ((s_pending_interrupts & SCHED_INTR_VBLANK) || s_pending_vblanks)) {
        s_pending_interrupts &= ~SCHED_INTR_VBLANK;
        /* A bare pending bit with no count is an out-of-band source (turbo mode's
         * quantum): exactly one episode, never zero. */
        uint32_t episodes = s_pending_vblanks ? s_pending_vblanks : 1u;
        s_pending_vblanks = 0;
        for (uint32_t i = 0; i < episodes; i++) {
            /* The handler itself can mask interrupts.  The episodes it never took
             * stay owed -- they elapsed with the I-bit set -- and a batch that ends
             * exactly on the mask point hands the window's own coalesced episode to
             * the next resume. */
            if (!s_interrupts_enabled) {
                s_pending_vblanks = episodes - i - 1u;
                if (!s_pending_vblanks) s_vblank_masked_pending = 1;
                episodes = i;      /* what the guest actually received */
                break;
            }
            deliver_vblank();
        }
        sr_perf_vblank_service(episodes);
    }
    s_servicing_interrupts = 0;
}

/* Turbo (SR_NOVBPACE=1): jump the virtual clock to the VBLANK source boundary
 * and then actually RUN the source.
 *
 * Moving the clock alone is not enough and is the specific shape of a defect
 * worth naming: a thread parked on VBLANK_WAIT_OBJ carries SCHED_WAIT_FOREVER,
 * so no amount of elapsed virtual time can promote it -- only a delivered edge
 * releases it. Latching and servicing here is what turns the time jump into an
 * event. */
/* A thread switch is deliberately NOT an interrupt-recognition point.
 *
 * The obvious place to service the display source is the switch back into a
 * runnable thread: a period that came due inside the previous thread's syscall
 * (`syscall` class in the delivered-rate attribution) could be delivered there
 * instead of at the next recomp yield.  It was built and measured, and it is not
 * shipped: delivering the backlog at a switch boundary rather than at the guest's
 * own yield point moved the flagship from 27.5 to 24.1-26.0 mean fps and raised the
 * per-second vblank spread from sd 0.57 to sd 2.2, because the vblank handler and
 * its callback walker then run mid-frame-switch instead of where the guest expects
 * them.  Recognition points stay where the guest can observe them: a recomp yield,
 * sceKernelCpuResumeIntr, and the scheduler's idle path.
 *
 * With the masked-window clamp gone (sched_enter_masked) the delivered rate is
 * 59.9 Hz without any of that: the `syscall` class is LATENCY, not loss -- the
 * periods it latches are owed and the next service point delivers them, which the
 * PERF_ATTRIB vblank_owed ledger shows as late_owed == late_delivered with
 * dropped=0.  Closing that latency needs a real interrupt-recognition point inside
 * a long guest stretch, which is a codegen change, not a scheduler-boundary one. */

static void sched_turbo_advance_to_vblank(void) {
    if (s_vbl_next_us > s_vtime_us) s_vtime_us = s_vbl_next_us;
    scheduler_latch_due_events();
    scheduler_service_pending();
}

static void coro_body(void *param) {
    TCB *t = (TCB *)param;
    for (;;) {
        /* Entry into the thread body: set up args and the standard return address (0). */
        s_cpu->r[4] = t->arglen;
        s_cpu->r[5] = t->argp;
        s_cpu->r[31] = 0;
        if (sched_uid_is_worker(t->uid)) fprintf(stderr, "DISPATCH uid=0x%x entry=0x%08x\n", t->uid, t->entry);
        t->has_unwind_jmp = 1;
        if (setjmp(t->unwind_jmp) == 0) {
            dispatch(s_cpu, t->entry);    /* runs until the thread returns or exits */
        } else {
            /* longjmp path: sched_unwind_current() was called from recomp.c.
             * The C frames of any in-flight nested host->guest call were
             * discarded by that jump, so their guest frames must be reclaimed
             * here: leaving them held would retire pool slots permanently and
             * make this thread's later nested calls fail closed for no reason. */
            unsigned reclaimed = sr_nested_frame_release_owner(t->uid);
            if (sched_uid_is_worker(t->uid)) fprintf(stderr, "FIBER_UNWIND: uid=0x%x cleanly unwound\n", t->uid);
            if (reclaimed)
                fprintf(stderr,
                        "FIBER_UNWIND: uid=0x%x reclaimed %u nested guest-call frame(s)\n",
                        t->uid, reclaimed);
        }
        t->has_unwind_jmp = 0;
        if (sched_uid_is_worker(t->uid)) fprintf(stderr, "DISPATCH uid=0x%x returned pc=0x%08x\n", t->uid, s_cpu->pc);
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

uint32_t g_master_reent;

/* Host-side registry of live guest threads, keyed by k0, holding (k0, state_ptr) pairs in
 * host memory instead of guest RAM. */
extern uint32_t sr_newlib_malloc(uint32_t size, uint32_t guest_ra);
extern void sr_callback_unregister_owner(uint32_t thread_uid);
extern void sr_hle_release_thread_resources(uint32_t thread_uid);

/* guest_reent_register: insert (uid, state_ptr) into the title-configured guest
 * per-thread reent/state hash for threads that do not call the guest registration
 * routine themselves. The translated routine returns -1 on duplicate UID; this host
 * helper silently overwrites (idempotent refresh on restart). */
static void guest_reent_register(uint32_t uid, uint32_t state_ptr) {
    SrTitleReentBindings bindings;
    if (!sr_title_config_reent_bindings(&bindings)) return;
    uint32_t bucket = uid % 32;
    /* Layout: { next(+0x00), state_ptr[32](+0x04..+0x80), uid[32](+0x84..+0x100) }.
     * Bucket = uid % 32 (signed-safe for positive PSP thread UIDs). */
    uint32_t node_addr = bindings.guest_thread_table_addr;

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
 * the configured per-thread reent/state hash. Zeroes both the uid key and state_ptr
 * slots so f_00011600 no longer finds this thread. */
static void guest_reent_unregister(uint32_t state_ptr) {
    SrTitleReentBindings bindings;
    if (!sr_title_config_reent_bindings(&bindings)) return;
    if (!sr_inrange(state_ptr)) return;
    uint32_t uid = MEM_R32(state_ptr + 0x37cu);
    if (uid == 0) return;

    uint32_t bucket = uid % 32;
    uint32_t node_addr = bindings.guest_thread_table_addr;

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
    SrTitleReentBindings bindings;
    int has_bindings = sr_title_config_reent_bindings(&bindings);
    if (has_bindings && g_master_reent == 0u)
        g_master_reent = bindings.master_reent_addr;
    /* Copy master thread's reent structure to initialize the new thread's reent.
     * This inherits the initialized allocator context. A thread holding the ROOT or
     * LAUNCHER role keeps its own independently-initialized reent: root has the
     * CRT-provided master reent; a launcher initializes its own from its guest entry.
     * Both tests are role tests, not UID-number tests -- with no captured role every
     * thread takes the ordinary inheriting path. */
    if (!sched_uid_is_root(uid) && !sched_uid_is_launcher(uid)) {
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
     * guest per-thread lookup reads this field to identify the current thread UID. */
    MEM_W32(state_ptr + 0x37cu, uid);
    if (getenv("SR_REENT_TRACE")) {
        fprintf(stderr, "REENT_TRACE init_guest_reent: uid=0x%x state_ptr=0x%x uid_at_37c=0x%x\n",
                uid, state_ptr, MEM_R32(state_ptr + 0x37cu));
    }
}

/* register_libc_thread: record (k0, state_ptr, uid) in the host-owned s_libc_threads table
 * and initialize the thread's reent structure (init_guest_reent), then pre-register the thread
 * in the configured guest reent/state hash for all threads except the launcher. The launcher's
 * guest entry calls its own registration routine and must see an empty slot.
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

    /* Only a thread that actually holds the LAUNCHER role seeds the master reent.
     * This was a UID-number test, so in a build with no launcher binding an ordinary
     * thread that happened to be allocated the historical launcher UID took over the
     * master reent for every later thread. */
    if (sched_uid_is_launcher(uid)) {
        g_master_reent = state_ptr;
    }

    init_guest_reent(state_ptr, uid);

    /* Pre-register in the configured guest hash for every thread except a launcher,
     * which calls the guest's own registration function from its entry and must find
     * an empty slot. The accessor makes this compatibility behavior inert otherwise. */
    if (!sched_uid_is_launcher(uid)) {
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
    unsigned stranded;
    if (!t || t->resources_released) return;
    if (t->k0_init) unregister_libc_thread(t->k0_init);
    sr_callback_unregister_owner(t->uid);
    sched_wait_abandon_owner(t);
    sr_hle_release_thread_resources(t->uid);
    /* Last net under this thread's nested host->guest call frames.  Every normal
     * and error return releases its own frame, and the coro_body unwind path
     * reclaims the chain after a longjmp, so a non-zero count here means a
     * release path was missed -- report and COUNT it instead of absorbing it.
     * The counter is what makes the unwind hook testable: without it a removed
     * reclaim in coro_body is invisible, because this net silently repairs it
     * a few instructions later. */
    stranded = sr_nested_frame_release_owner(t->uid);
    s_stranded_nested_frames += stranded;
    if (stranded)
        fprintf(stderr,
                "sched_release_thread_resources: uid=0x%x still held %u nested guest-call "
                "frame(s) at teardown -- reclaimed, but a release path was missed\n",
                t->uid, stranded);
    t->resources_released = 1;
}

static void sched_release_thread_stack(TCB *t) {
    if (!t || t->stack_released) return;
    stack_range_release(t->stack_base, t->stack_reservation);
    t->stack_released = 1;
}

uint32_t sched_create_thread_ex(uint32_t entry, int priority, uint32_t stack_size,
                                uint32_t attr, const char *name) {
    if (sr_title_config_is_worker_entry(entry) && !getenv("SR_NO_THREAD_REUSE")) {
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
        t->init_priority = priority;
        t->attr = attr;
        memset(t->name, 0, sizeof(t->name));
        if (name) strncpy(t->name, name, sizeof(t->name) - 1u);
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
    t->init_priority = priority;
    t->attr = attr;
    memset(t->name, 0, sizeof(t->name));
    if (name) strncpy(t->name, name, sizeof(t->name) - 1u);
    t->entry = entry;
    t->started = 0;
    t->coro = NULL;
    return sched_create_thread_finish(t, entry, priority, stack_size);
}

uint32_t sched_create_thread(uint32_t entry, int priority, uint32_t stack_size) {
    return sched_create_thread_ex(entry, priority, stack_size, 0u, NULL);
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
     * (sched_run's module_start thread); the worker and launcher are the threads whose
     * entry matches the build's configured worker/launcher binding, and neither role is
     * claimed at all when the build has no title configuration.
     * UID allocation has drifted once already (worker moved from 0x115 to 0x114) and
     * silently broke every hardcoded check in hle.c/recomp.c — recording the actual
     * assigned UIDs here lets those checks use sched_root_uid()/sched_worker_uid()/
     * sched_launcher_uid() instead of literals. */
    if (!s_root_seen) {
        s_root_seen = 1;
        g_root_uid = t->uid;
        if (getenv("SR_THLOG")) fprintf(stderr, "ROOT_UID_CAPTURE: root uid=0x%x\n", t->uid);
    }
    if (sr_title_config_is_worker_entry(entry)) {
        g_worker_uid = t->uid;
        if (getenv("SR_THLOG")) fprintf(stderr, "WORKER_UID_CAPTURE: worker uid=0x%x\n", t->uid);
    } else if (sr_title_config_is_launcher_entry(entry)) {
        g_launcher_uid = t->uid;
        if (getenv("SR_THLOG")) fprintf(stderr, "LAUNCHER_UID_CAPTURE: launcher uid=0x%x\n", t->uid);
    }
    /* Priority-inversion guard, applied only to a configured launcher role. A launcher
     * that spins in a module_start loop with thousands of SR_YIELD escapes per second is
     * normally the highest-priority user thread (32 < worker pri ~38-40), so the scheduler
     * always schedules it and starves the worker that drives SetFrameBuf / WaitVblank.
     * Once the launcher uid is known, demote it below any probable worker priority so the
     * worker can run. Disable with SR_NO_LAUNCHER_DEMOTE=1 (back to original behaviour).
     * A build with no configured launcher binding never reaches this. */
    if (!getenv("SR_NO_LAUNCHER_DEMOTE") && sr_title_config_is_launcher_entry(entry)) {
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
    /* Seed the thread UID field consumed by the configured guest reent lookup. */
    MEM_W32(state_ptr + 0x37cu, t->uid);

    fprintf(stderr, "DEBUG: sched_create_thread uid=0x%x entry=0x%x k0=0x%x state_ptr=0x%x val=0x%x\n",
            t->uid, entry, k0, state_ptr, MEM_R32(k0 + 4));

    /* Write stack bounds and state_ptr for exception handling */
    MEM_W32(k0 + 0, user_base);
    MEM_W32(k0 + 8, state_ptr);

    /* Register in the host-owned s_libc_threads table and any configured guest reent
     * hash before the thread's own entry runs. */
    register_libc_thread(k0, state_ptr, t->uid);

    /* Re-seed the thread UID into the kernel thread info structure at state_ptr + 0x37c.
     * register_libc_thread() did a 1KB copy from the master reent into state_ptr for any
     * non-root, non-launcher UID, which CLOBBERED our earlier seed above. The libc reent
     * lookup (f_00011600) reads MEM[state_ptr + 0x37c] for the UID, so we must
     * write it again AFTER the copy. */
    MEM_W32(state_ptr + 0x37cu, t->uid);

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
    for (int i = 0; i < s_ntcb; i++) {
        if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
            /* An infinite wait has no deadline and can only be released by a
             * signal. Without this guard a virtual clock that reached the
             * sentinel would "expire" every untimed wait at once -- a thread
             * parked in a display wait could resume having been delivered no
             * VBLANK at all. The clock must never satisfy a wait nothing
             * signalled. */
            s_tcb[i].wake != SCHED_WAIT_FOREVER &&
            s_vtime_us >= s_tcb[i].wake) {
            sched_wait_record_wake(&s_tcb[i], 0, 0u);
            s_tcb[i].state = TH_READY;   /* delay expired, or a timed wait timed out */
            SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_WAKE, s_tcb[i].uid, s_tcb[i].uid, s_tcb[i].wait_obj, 1u);
#ifdef SR_SCHED_LIVENESS_TEST
            sched_liveness_record_wake(s_tcb[i].wait_obj, 1u);
#endif
        }
    }
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
    if (!have_ready) {
#ifdef SR_SCHED_LIVENESS_TEST
        SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_PICK, -1, 0u);
#endif
        SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_PICK,
                                s_cur >= 0 && s_cur < s_ntcb ? s_tcb[s_cur].uid : 0u, 0u, 0u, 0u);
        return -1;
    }
    int start = (s_last_pick >= 0) ? (s_last_pick + 1) % s_ntcb : 0;
    for (int step = 0; step < s_ntcb; step++) {
        int i = (start + step) % s_ntcb;
        if (s_tcb[i].state == TH_READY && s_tcb[i].priority == best_pri) {
            s_last_pick = i;
            SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_PICK, i, 0u);
            SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_PICK,
                                    s_cur >= 0 && s_cur < s_ntcb ? s_tcb[s_cur].uid : 0u,
                                    s_tcb[i].uid, 0u, 0u);
            return i;
        }
    }
#ifdef SR_SCHED_LIVENESS_TEST
    SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_PICK, -1, 0u);
#endif
    return -1;   /* unreachable: have_ready guarantees a match above */
}

static SrPerfSchedState sched_perf_state(void) {
    if (s_cur < 0 || s_cur >= s_ntcb) return SR_PERF_SCHED_IDLE;
    if (s_tcb[s_cur].state == TH_READY) return SR_PERF_SCHED_RUNNABLE;
    if (s_tcb[s_cur].state == TH_WAIT_DELAY || s_tcb[s_cur].state == TH_WAIT_OBJ)
        return SR_PERF_SCHED_BLOCKED;
    if (s_tcb[s_cur].state == TH_DORMANT) return SR_PERF_SCHED_IDLE;
    return SR_PERF_SCHED_RUNNING;
}

static uint32_t sched_perf_uid(void) {
    return s_cur >= 0 && s_cur < s_ntcb ? s_tcb[s_cur].uid : 0u;
}

/* Save the running thread's registers, return to the scheduler, which selects and resumes the
 * next thread. Called from a thread fiber. */
static void switch_to_scheduler(void) {
#ifdef SR_SCHED_LIVENESS_TEST
    if (s_cur >= 0 && s_cur < s_ntcb)
        SCHED_LIVENESS_OWNER(s_tcb[s_cur].uid);
#endif
    if (!s_sched_coro || sr_coro_current() == s_sched_coro) return;
    if (sr_perf_enabled) sr_perf_sched_state(sched_perf_state(), sched_perf_uid());
    sr_coro_switch(s_sched_coro);
}

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
    if (!sched_uid_is_launcher(t->uid)) return;
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
    if (s_cur >= 0 && sched_uid_is_worker(s_tcb[s_cur].uid) && s_yieldlog) {
        static int yield_count = 0;
        if (yield_count < 200) {
            uint32_t k0 = s->r[26];
            fprintf(stderr, "YIELD uid=0x%x pc=0x%08x ra=0x%08x tick=%llu k0=0x%08x k0+4=0x%08x\n",
                    g_worker_uid, s->pc, s->r[31], (unsigned long long)s_tick,
                    k0, MEM_R32(k0 + 4));
            yield_count++;
        } else if ((yield_count % 5000) == 0) {
            fprintf(stderr, "PCSAMPLE uid=0x%x pc=0x%08x ra=0x%08x tick=%llu\n",
                    g_worker_uid, s->pc, s->r[31], (unsigned long long)s_tick);
        }
        yield_count++;
    }
    atomic_store_explicit(&sr_timeslice, TIMESLICE, memory_order_relaxed);
    if (s_cur < 0) {
        scheduler_service_pending();
        return;                            /* not in a thread */
    }
    s_tick++;
    if ((s_tick & 0xff) == 0) vtime_refresh(); /* observation only; time already progressed above */
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
    if (s_cur < 0 || !s_interrupts_enabled || !s_dispatch_enabled) {
#ifdef SR_SCHED_LIVENESS_TEST
        SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_PREEMPT, s_cur, 0u);
#endif
        return;
    }
    sched_promote_expired_waits();
    TCB *cur = &s_tcb[s_cur];
    int best = -1;
    for (int i = 0; i < s_ntcb; i++) {
        if (i == s_cur) continue;
        if (s_tcb[i].state == TH_READY && (best < 0 || s_tcb[i].priority < s_tcb[best].priority))
            best = i;
    }
    if (best >= 0 && s_tcb[best].priority < cur->priority) {   /* strictly higher priority ready */
#ifdef SR_SCHED_LIVENESS_TEST
        SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_PREEMPT, best, 0u);
#endif
        memcpy(&cur->saved, s_cpu, sizeof(CpuState));
        cur->state = TH_READY;
        switch_to_scheduler();
    }
#ifdef SR_SCHED_LIVENESS_TEST
    else {
        SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_PREEMPT, s_cur, 0u);
    }
#endif
}

void sched_delay_current(uint64_t usec) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    uint64_t duration = usec ? usec : 1u;
    vtime_refresh();
    uint64_t wake = scheduler_deadline_after(duration);
    if (getenv("SR_DELAYLOG"))
        fprintf(stderr, "DELAY uid=0x%x entry=0x%08x usec=%llu (%.1fs) wake=%llu\n",
                t->uid, t->entry, (unsigned long long)usec,
                (double)usec / 1e6, (unsigned long long)wake);
    if (usec > 2000000u && getenv("SR_DELAYLOG"))   /* > 2s: catch a bogus huge delay */
        fprintf(stderr, "BIG DELAY uid=0x%x entry=0x%08x usec=%llu (%.1fs)\n",
                t->uid, t->entry, (unsigned long long)usec, (double)usec / 1e6);
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_BLOCK, s_cur, 0u);
    SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_BLOCK, t->uid, t->uid, 0u, 0u);
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
    SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_BLOCK, s_cur, obj);
    SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_BLOCK, t->uid, t->uid, obj, 0u);
    t->state = TH_WAIT_OBJ;
    t->wait_obj = obj;
    t->wait_kind = t->pending_wait_kind;
    t->pending_wait_kind = 0;
    t->wake_result_valid = 0;
    t->wake = SCHED_WAIT_FOREVER;     /* infinite: only sched_wake releases it */
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
    SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_BLOCK, s_cur, obj);
    SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_BLOCK, t->uid, t->uid, obj, 0u);
    t->state = TH_WAIT_OBJ;
    t->wait_obj = obj;
    t->wait_kind = t->pending_wait_kind;
    t->pending_wait_kind = 0;
    t->wake_result_valid = 0;
    t->wake = deadline;
    switch_to_scheduler();
    vtime_refresh();
    return s_vtime_us >= deadline;   /* resumed: timed out if the deadline has passed */
}

static WaitInvocation *sched_wait_find(SrWaitHandle handle) {
    for (WaitInvocation *w = s_wait_invocations; w; w = w->next)
        if (w->handle == handle) return w;
    return NULL;
}

SrWaitHandle sched_wait_begin(uint32_t object, uint64_t deadline,
                              int callback_enabled, SrWaitDetach detach) {
    if (s_cur < 0 || s_tcb[s_cur].deleted || s_wait_sequence == UINT64_MAX)
        return 0u;
    WaitInvocation *w = (WaitInvocation *)calloc(1, sizeof(*w));
    if (!w) return 0u;
    w->handle = ++s_wait_sequence;
    w->owner = &s_tcb[s_cur];
    w->owner_uid = w->owner->uid;
    w->object = object;
    w->deadline = deadline;
    w->callback_enabled = callback_enabled;
    w->state = SR_WAIT_EXECUTING;
    w->detach = detach;
    w->next = s_wait_invocations;
    s_wait_invocations = w;
    return w->handle;
}

SrWaitHandle sched_wait_begin_join(uint32_t target, uint64_t deadline,
                                    int callback_enabled) {
    SrWaitHandle handle = sched_wait_begin(target, deadline, callback_enabled, NULL);
    WaitInvocation *w = sched_wait_find(handle);
    if (w) w->thread_join = 1;
    return handle;
}

int sched_wait_complete(SrWaitHandle handle, uint32_t result) {
    WaitInvocation *w = sched_wait_find(handle);
    if (!w || w->result_valid || w->state == SR_WAIT_TERMINAL ||
        w->owner->deleted || w->owner->uid != w->owner_uid) return 0;
    w->result = result;
    w->result_valid = 1;
    w->state = SR_WAIT_TERMINAL;
    /* A completed callback parent retains its result without waking a child. */
    if (w->owner->active_wait == handle)
        (void)sched_wake_one_object_waiter(w->object, w->owner_uid);
    return 1;
}

int sched_wait_state(SrWaitHandle handle, SrWaitState *state, uint32_t *owner) {
    WaitInvocation *w = sched_wait_find(handle);
    if (!w || w->owner->deleted || w->owner->uid != w->owner_uid) return 0;
    if (state) *state = w->state;
    if (owner) *owner = w->owner_uid;
    return 1;
}

static void sched_wait_record_wake(TCB *owner, int terminal, uint32_t result) {
    WaitInvocation *w = sched_wait_find(owner->active_wait);
    if (!w || w->owner != owner) return;
    if (terminal && !w->result_valid) {
        w->result = result;
        w->result_valid = 1;
    }
    w->state = w->result_valid ? SR_WAIT_TERMINAL : SR_WAIT_NOTIFIED;
}

int sched_wait_notify(SrWaitHandle handle) {
    WaitInvocation *w = sched_wait_find(handle);
    if (!w || w->state != SR_WAIT_BLOCKED || w->owner->active_wait != handle ||
        w->owner->uid != w->owner_uid || w->owner->deleted) return 0;
    return sched_wake_one_object_waiter(w->object, w->owner_uid);
}

int sched_wait_block(SrWaitHandle handle, uint32_t remaining, int timed) {
    WaitInvocation *w = sched_wait_find(handle);
    if (!w || s_cur < 0 || w->owner != &s_tcb[s_cur] ||
        w->owner->active_wait || w->owner->uid != w->owner_uid) abort();
    if (w->result_valid) return 0;
    TCB *owner = w->owner;
    owner->active_wait = handle;
    owner->is_cb_wait = w->callback_enabled;
    w->state = SR_WAIT_BLOCKED;
    int expired = 0;
    if (timed) expired = sched_block_on_timeout(w->object, remaining);
    else sched_block_on(w->object);
    /* No callbacks execute between block return and outcome capture. The
     * existing thread result slot is transport, not durable invocation storage. */
    uint32_t result = 0;
    if (sched_take_wake_result(&result) && !w->result_valid) {
        w->result = result;
        w->result_valid = 1;
    }
    owner->active_wait = 0u;
    owner->is_cb_wait = 0;
    w->state = w->result_valid ? SR_WAIT_TERMINAL : SR_WAIT_EXECUTING;
    return expired;
}

void sched_wait_callback(SrWaitHandle handle, int active) {
    WaitInvocation *w = sched_wait_find(handle);
    if (!w || s_cur < 0 || w->owner != &s_tcb[s_cur] || w->owner->active_wait)
        abort();
    if (!w->result_valid) w->state = active ? SR_WAIT_CALLBACK : SR_WAIT_EXECUTING;
}

int sched_wait_take_result(SrWaitHandle handle, uint32_t *result) {
    WaitInvocation *w = sched_wait_find(handle);
    if (!w || s_cur < 0 || w->owner != &s_tcb[s_cur] || !w->result_valid) return 0;
    if (result) *result = w->result;
    w->result_valid = 0;
    w->state = SR_WAIT_EXECUTING;
    return 1;
}

int sched_wait_finish(SrWaitHandle handle) {
    WaitInvocation **link = &s_wait_invocations;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (!*link) return 0;
    WaitInvocation *w = *link;
    *link = w->next; /* nonconsumable before calling object policy */
    if (w->owner->active_wait == handle) w->owner->active_wait = 0u;
    int notified = w->detach ? w->detach(handle, w->object) : 0;
    free(w);
    return notified;
}

static void sched_wait_abandon_owner(TCB *owner) {
    /* Invalidate every nesting level before any detach hook selects successors. */
    for (WaitInvocation *w = s_wait_invocations; w; w = w->next)
        if (w->owner == owner) w->state = SR_WAIT_TERMINAL;
    for (;;) {
        WaitInvocation *w = s_wait_invocations;
        while (w && w->owner != owner) w = w->next;
        if (!w) break;
        sched_wait_finish(w->handle);
    }
    owner->active_wait = 0u;
}

void sched_wait_cancel_object(uint32_t object, uint32_t result) {
    for (WaitInvocation *w = s_wait_invocations; w; w = w->next) {
        if (w->object != object) continue;
        if (!w->result_valid) { w->result = result; w->result_valid = 1; }
        w->state = SR_WAIT_TERMINAL;
        /* A callback-parked parent cannot wake its unrelated inner wait. */
        if (w->owner->active_wait == w->handle)
            (void)sched_wake_one_object_waiter(w->object, w->owner_uid);
    }
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
    for (WaitInvocation *w = s_wait_invocations; w; w = w->next) {
        if (!w->thread_join || w->object != uid) continue;
        if (sched_wait_complete(w->handle, result) &&
            w->owner->active_wait == w->handle) {
            TCB *waiter = w->owner;
            waiter->wait_obj = 0u;
            waiter->wake = 0u;
            waiter->wait_kind = waiter->pending_wait_kind = 0;
            waiter->wake_result_valid = waiter->is_cb_wait = 0;
        }
    }
    /* Legacy scheduler fixtures use the thread latch; production joins above
     * have one durable result per invocation, not per TCB. */
    for (int i = 0; i < s_ntcb; i++) {
        TCB *waiter = &s_tcb[i];
        if (waiter->deleted || waiter->state != TH_WAIT_OBJ ||
            !waiter->join_waiting || waiter->join_target != uid)
            continue;
        waiter->join_result = result;
        waiter->join_result_valid = 1;
        waiter->join_waiting = 0;
        waiter->state = TH_READY;
#ifdef SR_SCHED_LIVENESS_TEST
        sched_liveness_record_wake(uid, 1u);
#endif
        waiter->wait_obj = 0;
        waiter->wake = 0;
        waiter->wait_kind = 0;
        waiter->pending_wait_kind = 0;
        waiter->wake_result_valid = 0;
        waiter->is_cb_wait = 0;
    }
}

void sched_wake(uint32_t obj) {
#ifdef SR_SCHED_LIVENESS_TEST
    int readied = 0;
#endif
    for (int i = 0; i < s_ntcb; i++)
        if (!s_tcb[i].deleted && s_tcb[i].state == TH_WAIT_OBJ && s_tcb[i].wait_obj == obj) {
            sched_wait_record_wake(&s_tcb[i], 0, 0u);
            s_tcb[i].state = TH_READY;
            SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_WAKE,
                                    s_cur >= 0 && s_cur < s_ntcb ? s_tcb[s_cur].uid : 0u,
                                    s_tcb[i].uid, obj, 1u);
#ifdef SR_SCHED_LIVENESS_TEST
            readied++;
#endif
        }
#ifdef SR_SCHED_LIVENESS_TEST
    sched_liveness_record_wake(obj, (uint64_t)readied);
#endif
}

/* PSP-B2-01 (psp-hw-20260917): a cancelled waiter leaves its wait with a kernel
 * result instead of the object being satisfied -- WAIT_CANCEL (0x800201A9) from
 * CancelSema/CancelEventFlag/CancelReceiveMbx, WAIT_DELETE (0x800201B5) from
 * deleting the waited object, WAIT_RELEASE (0x800201AA) from ReleaseWaitThread.
 * The waiters themselves poll this after sched_block_on(); an untimed block has
 * no other way to distinguish "woken" from "cancelled". The result is stored
 * on each readied waiter, so several waiters all observe it and no other
 * thread can consume it. */
void sched_wake_with_result(uint32_t obj, uint32_t result) {
#ifdef SR_SCHED_LIVENESS_TEST
    int readied = 0;
#endif
    for (int i = 0; i < s_ntcb; i++) {
        TCB *w = &s_tcb[i];
        if (!w->deleted && w->state == TH_WAIT_OBJ && w->wait_obj == obj) {
            sched_wait_record_wake(w, 1, result);
            w->wake_result = result;
            w->wake_result_valid = 1;
            w->state = TH_READY;
#ifdef SR_SCHED_LIVENESS_TEST
            readied++;
#endif
        }
    }
#ifdef SR_SCHED_LIVENESS_TEST
    sched_liveness_record_wake(obj, (uint64_t)readied);
#endif
}

int sched_take_wake_result(uint32_t *result_out) {
    /* PSP-B2-01 / PSP-B3-01 (psp-hw-20260917): wake results are kernel error
     * codes (0x800201A9/0x800201AA/0x800201B5) whose high bit is set, so they
     * are negative as a signed int. Returning the code directly with a -1
     * sentinel misclassifies every real wake as "none". Report presence
     * separately and hand the code out-of-band. */
    if (s_cur < 0) return 0;
    TCB *self = &s_tcb[s_cur];
    int valid = self->wake_result_valid;
    self->wake_result_valid = 0;
    if (valid && result_out) *result_out = self->wake_result;
    return valid;
}

int sched_wake_one_object_waiter(uint32_t obj, uint32_t thread_uid) {
    TCB *t = tcb_by_uid(thread_uid);
    if (t && !t->deleted && t->state == TH_WAIT_OBJ && t->wait_obj == obj) {
        sched_wait_record_wake(t, 0, 0u);
        t->state = TH_READY;
#ifdef SR_SCHED_LIVENESS_TEST
        sched_liveness_record_wake(obj, 1u);
#endif
        return 1;
    }
    return 0;
}

/* Number of threads currently blocked on obj. PSP-B2-01 (psp-hw-20260917):
 * CancelSema / CancelEventFlag report this count to the caller. */
int sched_count_waiters(uint32_t obj) {
    int n = 0;
    for (int i = 0; i < s_ntcb; i++)
        if (!s_tcb[i].deleted && s_tcb[i].state == TH_WAIT_OBJ && s_tcb[i].wait_obj == obj)
            n++;
    return n;
}

/* PSP-B3-01 (psp-hw-20260917): ReleaseWaitThread readies one blocked thread and
 * hands it WAIT_RELEASE as its wait result. The target must be in an object
 * wait; anything else (running, dormant, deleted, delay) reports no waiter. */
int sched_wake_one_object_waiter_with_result(uint32_t thread_uid, uint32_t result) {
    TCB *t = tcb_by_uid(thread_uid);
    if (t && !t->deleted && t->state == TH_WAIT_OBJ) {
        sched_wait_record_wake(t, 1, result);
        t->wake_result = result;
        t->wake_result_valid = 1;
        t->state = TH_READY;
#ifdef SR_SCHED_LIVENESS_TEST
        sched_liveness_record_wake(t->wait_obj, 1u);
#endif
        return 1;
    }
    return 0;
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
/* Measured vblank interval width, not a guess: 721..734 us (mean 728) over 48
 * trials on PSP-3001/6.61-ARK (record PSP-DISPLAY-004). 729 sits inside the
 * measured band. See sched_display_is_vblank(). */
#define SCHED_DISPLAY_VBLANK_WINDOW_US 729u

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

/* The vblank interval BEGINS at the vblank start edge; it does not end there.
 *
 * Measured on PSP-3001/6.61-ARK (probe case `display-vblank-window`, record
 * PSP-DISPLAY-004): immediately after sceDisplayWaitVblankStart returned,
 * sceDisplayIsVblank() was true on 48 of 48 trials, and stayed true for
 * 721..734 us (mean 729) before falling. In the display's own units the
 * interval was entered at hcount 1 and left at hcount 14, i.e. it occupies the
 * first ~13 of the 286 lines in a frame. The same placement follows
 * independently from PSP-DISPLAY-002: a sceDisplayWaitVblankStart issued while
 * IsVblank() was true waited a FULL period, which is only possible if the caller
 * had just crossed a start edge rather than being about to reach one.
 *
 * This previously tested the LAST 1500 us before the next edge -- the opposite
 * end of the period, and roughly twice too wide. Anchor it to the delivery the
 * scheduler actually made, not to a free-running phase: the guest-visible
 * interval is the window immediately following the VBLANK the runtime
 * delivered, which is what a guest polling IsVblank() after a display wait
 * observes on hardware. Before the first delivery there is no interval to be
 * inside of.
 *
 * Scope, stated rather than implied. Delivery-anchoring makes the measured
 * primary invariant exact -- a caller that just returned from a display wait is
 * inside the interval, which is what the WaitVblank fast return depends on --
 * and it stays exact when the runtime falls behind and the source coalesces,
 * where a free-running phase would report `false` immediately after a late
 * delivery. The cost is the interrupt-masked regime: real scanout keeps running
 * under a CPU interrupt mask (the #88 measurements show AccumulatedHcount
 * free-running through one), so hardware's IsVblank keeps toggling there while
 * this predicate holds still until service resumes. Masks were measured at ~0.1%
 * of wall time with no guest execution inside them, so that regime is left to
 * the interrupt-context work rather than modelled by guessing here. */
int sched_display_is_vblank(void) {
    if (!s_vbl_count) return 0;          /* no edge delivered yet */
    if (s_vtime_us < s_vbl_last_us) return 0;
    return (s_vtime_us - s_vbl_last_us) < (uint64_t)SCHED_DISPLAY_VBLANK_WINDOW_US;
}

void sched_set_current_cb_wait(int cb_wait) {
    if (s_cur >= 0) {
        s_tcb[s_cur].is_cb_wait = cb_wait;
    }
}

void sched_wake_callbacks(uint32_t thread_uid) {
    TCB *t = tcb_by_uid(thread_uid);
    if (t && (t->state == TH_WAIT_OBJ || t->state == TH_WAIT_DELAY) && t->is_cb_wait) {
        sched_wait_record_wake(t, 0, 0u);
        t->state = TH_READY;
        t->wake = s_vtime_us;
#ifdef SR_SCHED_LIVENESS_TEST
        sched_liveness_record_wake(t->wait_obj, 1u);
#endif
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
    t->wake = SCHED_WAIT_FOREVER;
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
        t->wake = SCHED_WAIT_FOREVER;
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
    /* PSP-B3-01 (psp-hw-20260917): a dormant (never-started or terminated)
     * target answers DORMANT (0x800201A2), it does not bank a wakeup. */
    if (t->state == TH_DORMANT) return SCE_KERNEL_ERROR_DORMANT;
    {
        fprintf(stderr, "DEBUG_WAKEUP: target=0x%x sleeping=%d state=%d wait_obj=0x%x wakeups=%d\n",
                uid, t->sleeping, t->state, t->wait_obj, t->wakeups);
#ifdef SR_SCHED_LIVENESS_TEST
        int readied = 0;
#endif
        if (t->sleeping && t->state == TH_WAIT_OBJ && t->wait_obj == uid) {
            t->sleeping = 0;
            t->state = TH_READY;
            t->wait_obj = 0;
            t->wake = 0;
#ifdef SR_SCHED_LIVENESS_TEST
            readied = 1;
#endif
        } else {
            t->wakeups++;
        }
#ifdef SR_SCHED_LIVENESS_TEST
        if (readied)
            sched_liveness_record_wake(uid, 1u);
        else
            SCHED_LIVENESS_NOTE(SR_SCHED_LIVENESS_WAKE, -1, uid);
#endif
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
    /* PSP-B2-01 / PSP-B3-01 (psp-hw-20260917): ReferThreadStatus distinguishes
     * object waits (sema 3, evf 4, lwmutex 13); the kind is latched at block
     * time because the waited UID alone does not identify its type. */
    if (t->state == TH_WAIT_OBJ) return t->wait_kind ? (uint32_t)t->wait_kind : PSP_WAIT_OBJECT;
    return PSP_WAIT_NONE;
}

/* Latch the PSP waitType for the current thread's next object block; the
 * block consumes it, so a later wait of another kind never reports it. */
void sched_set_current_wait_kind(int kind) {
    if (s_cur >= 0) s_tcb[s_cur].pending_wait_kind = kind;
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

int sched_thread_info(uint32_t uid, SrThreadInfo *out) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t || !out) return -1;
    memset(out, 0, sizeof(*out));
    SrThreadRunStatus status;
    if (sched_thread_run_status(uid, &status) != 0) return -1;
    out->size = (uint32_t)sizeof(*out);
    memcpy(out->name, t->name, sizeof(out->name));
    out->attr = t->attr;
    out->status = status.status;
    out->entry = t->entry;
    out->stack = t->stack_base;
    out->stackSize = t->stack_size;
    out->gpReg = s_cur >= 0 && t == &s_tcb[s_cur] && s_cpu
               ? s_cpu->r[28] : t->saved.r[28];
    out->initPriority = (uint32_t)t->init_priority;
    out->currentPriority = status.currentPriority;
    out->waitType = status.waitType;
    out->waitId = status.waitId;
    out->wakeupCount = status.wakeupCount;
    out->exitStatus = (uint32_t)t->exit_status;
    out->runClocksLow = status.runClocksLow;
    out->runClocksHigh = status.runClocksHigh;
    out->intrPreemptCount = status.intrPreemptCount;
    out->threadPreemptCount = status.threadPreemptCount;
    out->releaseCount = status.releaseCount;
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
uint32_t sched_set_priority(uint32_t uid, int priority) {
    if (uid == 0 && s_cur >= 0) uid = s_tcb[s_cur].uid;
    else uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return SCE_KERNEL_ERROR_UNKNOWN_THID;
    /* PSP-B3-01 (psp-hw-20260917): a dormant (never-started or terminated)
     * target answers DORMANT (0x800201A2). */
    if (t->state == TH_DORMANT) return SCE_KERNEL_ERROR_DORMANT;
    t->priority = priority;
    return 0;
}

/* sceKernelChangeCurrentThreadAttr: changes the current thread's attribute bits.
 * clear_mask: bits to clear from the thread's attribute word
 * set_mask: bits to set in the thread's attribute word
 * Returns 0 on success, or an error code.
 * Only PSP_THREAD_ATTR_VFPU (0x00004000) is accepted; any other bit fails closed
 * with ILLEGAL_ATTR (0x80020191, the code measured for an invalid semaphore attr,
 * docs/HARDWARE_ORACLE.md) until hardware evidence covers it. */
uint32_t sched_change_current_thread_attr(uint32_t clear_mask, uint32_t set_mask) {
    if (s_cur < 0) return SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
    TCB *t = &s_tcb[s_cur];
    const uint32_t USER_MODIFIABLE_ATTR = 0x00004000u; /* PSP_THREAD_ATTR_VFPU */

    if ((clear_mask & ~USER_MODIFIABLE_ATTR) != 0 || (set_mask & ~USER_MODIFIABLE_ATTR) != 0)
        return SCE_KERNEL_ERROR_ILLEGAL_ATTR;

    t->attr &= ~clear_mask;
    t->attr |= set_mask;
    return 0;
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
    /* CONFLICT (PSP-B3-01 vs sched_selftest lifecycle contract): hardware
     * reports DORMANT (0x800201A2) for Terminate on a never-started or
     * terminated target, but test_delete_and_terminate_delete_contract pins
     * terminate-then-delete on a synthetic DORMANT target as success (0) so
     * TerminateDelete can proceed. Keep the contract; report HW as conflicting.
     * See FINAL REPORT. */
    sched_release_thread_resources(t);
    if (getenv("SR_SYSLOG")) fprintf(stderr, "thr 0x%x TERMINATED (entry 0x%08x)\n", uid, t->entry);
    /* A target stopped by another thread is no longer running, so its host
     * coroutine can be destroyed immediately. The guest stack remains owned by
     * the dormant object until the corresponding DeleteThread operation. */
    if (t->coro) { sr_coro_destroy(t->coro); t->coro = NULL; }
    t->state = TH_DORMANT;
    t->exit_status = (int32_t)SCE_KERNEL_ERROR_THREAD_TERMINATED;
    t->sleeping = 0; t->wait_obj = 0; t->wake = 0;
    t->wait_kind = 0; t->pending_wait_kind = 0;
    t->wake_result = 0; t->wake_result_valid = 0;
    t->wakeups = 0; t->is_cb_wait = 0;
    t->join_waiting = 0; t->join_target = 0; t->join_result = 0;
    t->join_result_valid = 0;
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
    uint32_t uid = sched_create_thread(entry, 32, 0);
    TCB *t0 = tcb_by_uid(uid);
    /* The entry (module_start) keeps the driver-seeded state -- real sp, gp, and module args
     * -- rather than the synthetic thread stack. */
    memcpy(&t0->saved, s_cpu, sizeof(CpuState));
    arglen = s_cpu->r[4]; argp = s_cpu->r[5];
    /* The HST guest reent hash is initialized only when the validated profile
     * supplies its typed binding. Other titles receive no guest-memory seed. */
    SrTitleReentBindings reent_bindings;
    if (sr_title_config_reent_bindings(&reent_bindings) &&
        sr_inrange(reent_bindings.guest_thread_table_addr) &&
        sr_inrange(reent_bindings.guest_thread_table_addr + 4u)) {
        MEM_W32(reent_bindings.guest_thread_table_addr, 0u);
    }
    sched_start_thread(uid, arglen, argp);

    static const char *stn[] = {"DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ"};
    unsigned long long iters = 0;
    /* Idle no-progress accounting; see the guard in the idle path below. */
    uint64_t idle_vbl_mark = s_vbl_count;
    int idle_no_progress = 0;
    if (sr_perf_enabled) sr_perf_sched_state(SR_PERF_SCHED_IDLE, 0u);
    for (;;) {
        if (getenv("SCHED_DUMP") && (++iters % 400000) == 0) {
            fprintf(stderr, "--- sched dump (tick=%llu) ---\n", (unsigned long long)s_tick);
            for (int i = 0; i < s_ntcb; i++)
                fprintf(stderr, "  uid 0x%x entry 0x%08x %s prio %d wait_obj 0x%x\n",
                        s_tcb[i].uid, s_tcb[i].entry, stn[s_tcb[i].state], s_tcb[i].priority, s_tcb[i].wait_obj);
        }
        int idx = pick_next();
        if (idx < 0) {
            if (sr_perf_enabled) sr_perf_sched_state(SR_PERF_SCHED_IDLE, 0u);
            sr_rt_phase = SR_RT_PHASE_SCHED;
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
            /* Still nothing. Stop only when nothing can be woken at all -- by a
             * deadline, or by the runtime's own VBLANK source. */
            const SchedIdleState idle = sched_classify_idle();
            const uint64_t soonest = idle.soonest;

            if (idle.unwakeable) {
                if (idle.waiting_on_vblank)
                    fprintf(stderr, "SCHED: threads wait on VBLANK but the source cannot fire "
                                    "(interrupts_enabled=%d vbl_next_us=%s). Dumping thread states:\n",
                            s_interrupts_enabled,
                            s_vbl_next_us == UINT64_MAX ? "saturated" : "live");
                else
                    fprintf(stderr, "SCHED: no runnable threads left (deadlock/infinite wait). Dumping thread states:\n");
                sched_dump_threads();
                if (s_t111_on && s_t111_n) sr_t111_dump();
                break;   /* nothing the runtime can still wake */
            }

            /* Bounded final guard. Not a timeout: the unit is delivered VBLANKs.
             * A healthy idle iteration delivers exactly one edge and wakes its
             * waiters, so a run of iterations that delivers none and readies
             * nobody means the source stopped for a reason not enumerated
             * above. Report it with the same dump rather than looping forever. */
            /* A finite deadline is its own progress source.  It may be longer than
             * the VBLANK watchdog window when the display source is masked or
             * saturated, but the scheduler must still advance to and honor it. */
            if (soonest == SCHED_WAIT_FOREVER &&
                s_vbl_count == idle_vbl_mark) {
                if (++idle_no_progress >= SCHED_IDLE_NO_PROGRESS_LIMIT) {
                    fprintf(stderr, "SCHED: %d idle iterations delivered no VBLANK and readied "
                                    "no thread. Dumping thread states:\n",
                            SCHED_IDLE_NO_PROGRESS_LIMIT);
                    sched_dump_threads();
                    if (s_t111_on && s_t111_n) sr_t111_dump();
                    break;
                }
            } else {
                idle_vbl_mark = s_vbl_count;
                idle_no_progress = 0;
            }

            if (!s_pace_on) {                     /* turbo: jump the clock over the wait */
                if (soonest == SCHED_WAIT_FOREVER && idle.vblank_can_wake) {
                    sched_turbo_advance_to_vblank();
                } else if (soonest > s_vtime_us) {
                    s_vtime_us = soonest;
                }
                idx = pick_next();
                if (idx < 0) {
                    fprintf(stderr, "SCHED: no runnable threads left after time jump. Dumping thread states:\n");
                    sched_dump_threads();
                    break;
                }
            } else {
                continue;   /* paced: keep delivering vblanks; real time reaches the deadline */
            }
        } else {
            idle_no_progress = 0;
            idle_vbl_mark = s_vbl_count;
        }
        TCB *t = &s_tcb[idx];
        uint32_t previous_uid = 0;
        if (sr_perf_enabled && s_cur >= 0 && s_cur < s_ntcb)
            previous_uid = s_tcb[s_cur].uid;
        s_cur = idx;
        SCHED_LIVENESS_OWNER(t->uid);
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
        sr_rt_phase = t->rt_phase;
        if (sr_perf_enabled) {
            sr_perf_sched_state(SR_PERF_SCHED_RUNNING, t->uid);
            sr_perf_sched_switch(previous_uid, t->uid);
            sr_perf_guest_begin();
        }
        sr_coro_switch(t->coro);           /* run until it yields/blocks/exits */
        if (sr_perf_enabled) sr_perf_guest_end();
        t->hle_depth = g_hle_depth;
        t->rt_phase = sr_rt_phase;
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
        sr_rt_phase = SR_RT_PHASE_SCHED;
    }
}
