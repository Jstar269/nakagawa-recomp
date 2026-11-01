// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Threads and callbacks: thread lifecycle, waits and wakeups, priorities and callbacks.
 * Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

extern int sr_libc_death_wish;    /* defined with h_ExitGame below */
static int sr_thread_dispatch_callbacks_once(uint32_t thread_uid);
#define HLE_KERNEL_ERROR_ILLEGAL_SIZE 0x800201bcu   /* SCE_KERNEL_ERROR_ILLEGAL_SIZE */

/* AllocateVpl / AllocateVplCB blocking forms integrated with fiber wait queues (PR-G). */

/* ThreadManForUser, backed by the fiber scheduler (src/rt/sched.c). */
static uint32_t h_CreateThread(CpuState *s) {
    /* a0=name, a1=entry, a2=priority, a3=stackSize. Returns a UID bound to the entry.
     * sched_create_thread returns 0 when the TCB table or the stack arena is exhausted;
     * real PSP fails such a create instead of granting a smaller stack. */
    char name[32] = {0};
    if (A0 && !guest_cstr(A0, name, sizeof(name))) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t uid = sched_create_thread_ex(A1, (int)A2, A3, stack_arg(s, 0), name);
    return uid ? uid : 0x80020190u;   /* SCE_KERNEL_ERROR_NO_MEMORY */
}
static uint32_t h_StartThread(CpuState *s) {
    /* a0=thid, a1=arglen, a2=argp. Mark ready, then preempt if it is higher priority -- PSP
     * runs a higher-priority started thread immediately. */
    uint32_t thid = A0;
    uint32_t result = sched_start_thread(thid, A1, A2);
    /* Historical "libc_main_thid" re-seed -- REMOVED. Statically and dynamically proven
     * to be part of the guest module/EH-metadata registry, not a thread ID scalar. */
    if (result == 0) sched_preempt();
    return result;
}
extern void sr_exitsnap_capture(CpuState *s);
extern void sr_exitsnap_dump_latest(const char *tag);
static int s_exit_delete_request;
static uint32_t h_ExitThread(CpuState *s) {
    /* sceKernelExitThread(status): TERMINAL for the calling thread, unconditionally --
     * sched_exit_current() at the bottom records the status, releases WaitThreadEnd
     * joiners, unregisters the libc thread state, marks the TCB DORMANT, and switches
     * away; control never returns to the guest caller. Historical builds carried
     * launcher/worker "survive their own exit" bypasses here (return 0 and keep the
     * caller running); those are GONE -- the earlier comments describing them were stale.
     * Do not reintroduce a survival path: a route that deadlocks with no runnable
     * threads after a legitimate exit has a bug in whatever failed to keep its awaited
     * state alive, not in the exit itself.
     *
     * death_wish: if h_KernelPrintf just saw the libc reentrancy guard fire ("should be
     * called from main thread" / "no reent structure"), the dying thread's libc state is
     * already corrupt and its _exit() path is unwinding -- exit the host process instead
     * of scheduling into a broken world. The root and launcher threads are exempt: their
     * exit is part of normal boot teardown, not a libc failure. The exemption is a ROLE
     * test, not a UID-number test: a build with no launcher binding exempts nothing, so
     * an ordinary thread cannot inherit the exemption by its allocated number. */
    int death_wish = sr_libc_death_wish;
    uint32_t uid = sched_current_uid();
    if (sched_uid_is_root(uid) || sched_uid_is_launcher(uid)) {
        death_wish = 0;
    }
    /* Trace call stack via RA chain */
    fprintf(stderr, "TRACE_EXIT: uid=0x%x pc=0x%08x ra=0x%08x ra2=0x%08x ra3=0x%08x\n",
            uid, s->pc, s->r[31],
            s->r[29] ? MEM_R32(s->r[29] + 0x0c) : 0,
            s->r[29] ? MEM_R32(s->r[29] + 0x10) : 0);
    fflush(stderr);
    /* ExitThread terminates only the caller. Joiners are released by
     * sched_exit_current(); waking an unrelated sleeping thread here fabricates a
     * sceKernelWakeupThread that the guest never issued. In HST that compatibility
     * shortcut let a short-lived character-resource worker wake the launcher, whose
     * teardown then destroyed the singleton beneath a sibling worker (issue #126).
     * Keep the env-gated primary-worker diagnostics independent of guest scheduling. */
    if (sched_uid_is_worker(uid)) {
        sr_exitsnap_capture(s);
        sr_exitsnap_dump_latest("worker-exit-pre");
    }
    fprintf(stderr, "HLE: ExitThread cur_uid=0x%x ra=0x%08x (death_wish=%d)\n",
            uid, s->r[31], death_wish);
    fprintf(stderr,
            "  regs uid=0x%x: pc=0x%08x sp=0x%08x gp=0x%08x k0=0x%08x k0+4=0x%08x k0+0x38c=0x%08x "
            "v0=0x%08x a0=0x%08x t0..t3=0x%08x/0x%08x/0x%08x/0x%08x s0..s3=0x%08x/0x%08x/0x%08x/0x%08x\n",
            uid, s->pc, s->r[29], s->r[28], s->r[26], s->r[26] ? MEM_R32(s->r[26] + 4) : 0, s->r[26] ? MEM_R32(s->r[26] + 0x38c) : 0,
            s->r[2], s->r[4], s->r[8], s->r[9], s->r[10], s->r[11],
            s->r[16], s->r[17], s->r[18], s->r[19]);
    if (death_wish) {
        /* _exit: skip host atexit handlers (libc has already burned -- the death wish
         * is unconditional at this point). stderr flush so the trace survives. */
        fprintf(stderr, "GAMELOG: Guest requested exit (libc guard chain)\n");
        fflush(stderr);
        sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, s->pc, uid, 1u);
        _exit(1);
    }
    int delete_request = s_exit_delete_request;
    s_exit_delete_request = 0;
    if (delete_request)
        sched_exit_current_delete((int32_t)A0);
    else
        sched_exit_current((int32_t)A0);
    return 0;
}
static uint32_t h_ExitDeleteThread(CpuState *s) {
    /* Share the terminal diagnostics and death-wish handling with ExitThread,
     * but select the delete-at-exit lifecycle operation at its final handoff. */
    s_exit_delete_request = 1;
    return h_ExitThread(s);
}
/* sceKernelDelayThread: delays the calling thread by usec microseconds.
 * PSP-3001 6.6.1 hardware measurement (#340): sceKernelDelayThread(0) returns 0
 * without yielding to an equal-priority ready thread and without dispatching
 * pending callbacks. Context check (CAN_NOT_WAIT) still precedes. (L2/L3) */
static uint32_t h_DelayThread(CpuState *s) {
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    if (A0 == 0) return 0;
    sched_delay_current(A0);
    return 0;
}
/* sceKernelDelayThreadCB: delays the calling thread while servicing callbacks.
 * PSP-3001 6.6.1 hardware measurement (#340): sceKernelDelayThreadCB(0) returns 0
 * without yielding to an equal-priority ready thread, and dispatches pending
 * callbacks (exactly once). Context check (CAN_NOT_WAIT) still precedes. (L6/L7) */
static uint32_t h_DelayThreadCBForUsec(uint64_t usec) {
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;  /* L6/L7 */
    uint32_t thread_uid = sched_current_uid();
    if (usec == 0) {
        /* Zero delay stays non-blocking: visit each callback slot present at entry at most
         * once, so a callback that re-notifies itself remains pending for a later pump.
         * CheckCallback is the explicit pump and keeps dispatching until the queue is empty. */
        if (sr_thread_has_pending_callbacks(thread_uid)) {
            (void)sr_thread_dispatch_callbacks_once(thread_uid);
        }
        return 0;
    }
    sched_vtime_refresh();
    uint64_t end_time = sched_vtime_deadline_after(usec);

    while (sched_vtime_us() < end_time) {
        if (sr_thread_has_pending_callbacks(thread_uid)) {
            sr_thread_dispatch_callbacks();
            sched_vtime_refresh();
            continue;
        }
        uint64_t remaining = end_time - sched_vtime_us();
        sched_set_current_cb_wait(1);
        sched_delay_current(remaining);
        sched_set_current_cb_wait(0);
        sched_vtime_refresh();
    }
    return 0;
}
static uint32_t h_DelayThreadCB(CpuState *s) {
    return h_DelayThreadCBForUsec(A0);
}

/* PSP's public pspkerror.h names ILLEGAL_ADDR as 0x800200D3 for kernel APIs.
 * Keep it distinct from the subsystem-specific 0x80000103 memory error used by
 * several HLE objects below. */
#define SCE_KERNEL_ERROR_KERNEL_ILLEGAL_ADDR 0x800200d3u

static uint32_t h_ReadSysClockDelay(uint32_t addr, uint64_t *usec_out) {
    if (!addr || !usec_out || !sr_guest_span_readable(addr, 8u))
        return SCE_KERNEL_ERROR_KERNEL_ILLEGAL_ADDR;
    uint64_t low = MEM_R32(addr);
    uint64_t high = MEM_R32(addr + 4u);
    *usec_out = low | (high << 32);
    return 0;
}

/* Public PSPSDK pspthreadman.h: int sceKernelDelaySysClockThread(SceKernelSysClock *delay)
 * and int sceKernelDelaySysClockThreadCB(SceKernelSysClock *delay). The only argument is
 * the pointer in $a0; the SceKernelSysClock it names (low word, high word) is the delay in
 * microseconds, so $a1 is not an argument of either call. */
static uint32_t h_DelaySysClockThread(CpuState *s) {
    (void)s;
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    uint64_t usec;
    uint32_t result = h_ReadSysClockDelay(A0, &usec);
    if (result != 0u) return result;
    sched_delay_current(usec);
    return 0;
}

static uint32_t h_DelaySysClockThreadCB(CpuState *s) {
    (void)s;
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    uint64_t usec;
    uint32_t result = h_ReadSysClockDelay(A0, &usec);
    if (result != 0u) return result;
    return h_DelayThreadCBForUsec(usec);
}
/* sceKernelChangeThreadPriority(thid, priority).  Validation, the stored value and the
 * scheduling boundary at the call (the PSPAutotests threads/threads/change.expected
 * cells) are sched_set_priority's. */
static uint32_t h_ChangeThreadPriority(CpuState *s) {
    return sched_set_priority(A0, (int)A1);
}

/* sceKernelChangeCurrentThreadAttr: changes the current thread's attribute bits.
 * PSPSDK signature: int sceKernelChangeCurrentThreadAttr(int unknown, SceUInt attr)
 * First argument (A0): mask of bits to clear
 * Second argument (A1): mask of bits to set
 * Returns 0 on success, or an error code.
 * Only the VFPU bit (0x00004000, the common call with set mask PSP_THREAD_ATTR_VFPU)
 * is accepted and recorded; any other bit fails closed with ILLEGAL_ATTR until its
 * semantics are evidenced. The runtime does not gate VFPU use on this bit. */
static uint32_t h_ChangeCurrentThreadAttr(CpuState *s) {
    (void)s;
    return sched_change_current_thread_attr(A0, A1);
}

/* sceKernelSuspendThread(thid) / sceKernelResumeThread(thid) /
 * sceKernelRotateThreadReadyQueue(priority).  Public PSPSDK pspthreadman.h prototypes;
 * semantics and error codes are documented at the sched_*_thread functions in sched.c.
 * Resume applies strict-priority preemption once the thread is runnable again, like
 * WakeupThread. */
static uint32_t h_SuspendThread(CpuState *s) {
    (void)s;
    return sched_suspend_thread(A0);
}
static uint32_t h_ResumeThread(CpuState *s) {
    (void)s;
    uint32_t result = sched_resume_thread(A0);
    if (result == 0) sched_preempt();
    return result;
}
static uint32_t h_RotateThreadReadyQueue(CpuState *s) {
    (void)s;
    return sched_rotate_thread_ready_queue((int)A0);
}

static uint32_t h_TerminateDeleteThread(CpuState *s) {
    uint32_t result = sched_terminate_thread(A0);
    if (result != 0) return result;
    result = sched_delete_thread(A0);
    /* Let queued replacement waiters run only after the composite lifecycle
     * operation has finished deleting its target. */
    sched_preempt();
    return result;
}
static uint32_t h_TerminateThread(CpuState *s) {
    /* DORMANT and invalid/self targets have distinct public PSPSDK error
     * classes; sched_terminate_thread supplies ILLEGAL_THID/UNKNOWN_THID. */
    if (sched_is_dormant(A0)) return 0x800201a2u; /* SCE_KERNEL_ERROR_DORMANT */
    uint32_t result = sched_terminate_thread(A0);
    if (result == 0u) sched_preempt();
    return result;
}
static uint32_t h_DeleteThread(CpuState *s) {
    return sched_delete_thread(A0);
}
static uint32_t h_GetThreadIdSched(CpuState *s) { (void)s; return sched_current_uid(); }
static uint32_t h_GetThreadPriority(CpuState *s) { (void)s; return (uint32_t)sched_current_priority(); }
static uint32_t h_GetThreadExitStatus(CpuState *s) { return sched_thread_exit_status(A0); }
/* sceKernelGetThreadStackFreeSize(thid), thid 0 = the calling thread: the unused part of the
 * selected thread's own stack, from the scheduler's stack range and creation fill
 * (sched_thread_stack_free_size has the public evidence and the unmeasured cases). */
static uint32_t h_GetThreadStackFreeSize(CpuState *s) {
    (void)s;
    return sched_thread_stack_free_size(A0);
}
/* sceKernelCheckThreadStack(void): the calling thread's stack space left below its stack
 * pointer, that is $sp minus the low end of its stack. Measured on a PSP by pspautotests
 * tests/threads/threads/stackfree.expected (rev 2ed7c9c5), threads with a 0x1000-byte stack:
 * a thread that only calls it reads 0xEB0, and one that first puts a 1 KiB array in its frame
 * reads 0xAB0, 0x400 less, with or without the stack fill, so the value follows the stack
 * pointer and not the fill bytes. With the fill, and no 0xFF bytes written in the frame, it is
 * 0x10 above sceKernelGetThreadStackFreeSize (0xEB0 against 0xEA0, 0xAB0 against 0xAA0): the
 * 0x10 bytes at the low end that call does not count (sched_thread_stack_free_size). The main
 * thread's value is above 0x30000.
 * The caller's $sp is the guest's own: the import stub has no frame. A stack pointer outside
 * the thread's stack (an overflow, or code running on another stack) is not measured, nor is
 * a call with no current thread, so both stop the run by name. */
static uint32_t h_CheckThreadStack(CpuState *s) {
    uint32_t base = 0u, size = 0u, sp = s->r[29];
    if (!sched_current_thread_stack_range(&base, &size))
        return hle_unmodelled_stop("sceKernelCheckThreadStack", "with no current thread is not measured");
    if (sp < base || sp - base > size)
        return hle_unmodelled_stop("sceKernelCheckThreadStack",
                                   "with $sp 0x%08x outside the thread's stack 0x%08x+0x%x is not measured",
                                   sp, base, size);
    return sp - base;
}
/* sceKernelSleepThread[CB] blocks only when no wakeup is banked; a pending wakeup is
 * consumed and the call returns without waiting. The context check therefore asks the
 * wakeup count first, so a rejected call decrements nothing, sets no sleep marker, and
 * does not yield -- and a sleep that would have been satisfied immediately keeps
 * working with interrupts or dispatch disabled. (L18/L19, L22/L23) */
static uint32_t h_SleepThread(CpuState *s) {
    (void)s;
    uint32_t uid = sched_current_uid();
    if (hle_wakelog_on()) {
        static int n=0;
        if (n++<8) fprintf(stderr, "SLEEP uid=0x%x\n", uid);
    }
    if (!sched_current_has_pending_wakeup() && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sched_thread_sleep();
    return 0;
}
static uint32_t h_SleepThreadCB(CpuState *s) {
    (void)s;
    uint32_t uid = sched_current_uid();
    if (hle_wakelog_on()) {
        static int n=0;
        if (n++<8) fprintf(stderr, "SLEEP_CB uid=0x%x\n", uid);
    }
    if (!sched_current_has_pending_wakeup() && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    sched_thread_sleep_cb();
    return 0;
}
static uint32_t h_WakeupThread(CpuState *s) {
    if (hle_wakelog_on()) { static int n=0; if (n++<8) fprintf(stderr, "WAKEUP target=0x%x (from 0x%x)\n", A0, sched_current_uid()); }
    uint32_t result = sched_thread_wakeup(A0);
    if (result == 0) sched_preempt();
    return result;
}
static uint32_t h_CancelWakeupThread(CpuState *s) {
    int old = sched_thread_cancel_wakeup(A0);
    if (hle_wakelog_on()) { static int n=0; if (n++<8) fprintf(stderr, "CANCEL_WAKE target=0x%x old=%d (from 0x%x)\n", A0, old, sched_current_uid()); }
    return old < 0 ? 0x800201a0u : (uint32_t)old;
}
static uint32_t h_WaitThreadEnd(CpuState *s) {
    return h_WaitThreadEnd_impl(s, 0);
}
static uint32_t h_WaitThreadEndCB(CpuState *s) {
    return h_WaitThreadEnd_impl(s, 1);
}
static uint32_t h_ReferThreadRunStatus(CpuState *s) {
    uint32_t out = A1;
    if (out) {
        SrThreadRunStatus rs;
        int rc = sched_thread_run_status(A0, &rs);
        if (rc < 0) return 0x800201a0u;
        for (uint32_t i = 0; i < 0x2c; i++) MEM_W8(out + i, 0);
        MEM_W32(out + 0x00, rs.size);
        MEM_W32(out + 0x04, rs.status);
        MEM_W32(out + 0x08, rs.currentPriority);
        MEM_W32(out + 0x0c, rs.waitType);
        MEM_W32(out + 0x10, rs.waitId);
        MEM_W32(out + 0x14, rs.wakeupCount);
        MEM_W32(out + 0x18, rs.runClocksLow);
        MEM_W32(out + 0x1c, rs.runClocksHigh);
        MEM_W32(out + 0x20, rs.intrPreemptCount);
        MEM_W32(out + 0x24, rs.threadPreemptCount);
        MEM_W32(out + 0x28, rs.releaseCount);
        if (hle_wakelog_on()) { static int n=0; if (n++<12) fprintf(stderr,
            "REFER_RUN target=0x%x status=%u pri=%u waitType=%u waitId=0x%x wakeups=%u\n",
            A0, rs.status, rs.currentPriority, rs.waitType, rs.waitId, rs.wakeupCount); }
    }
    return 0;
}
static uint32_t h_ReferThreadStatus(CpuState *s) {
    if (!A1 || !sr_guest_span_writable(A1, (uint32_t)sizeof(SrThreadInfo)))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    SrThreadInfo info;
    if (sched_thread_info(A0, &info) != 0) return 0x80020198u;
    for (uint32_t i = 0; i < sizeof(info.name); i++) MEM_W8(A1 + 4u + i, (uint8_t)info.name[i]);
    MEM_W32(A1 + 0x00u, info.size);
    MEM_W32(A1 + 0x24u, info.attr);
    MEM_W32(A1 + 0x28u, info.status);
    MEM_W32(A1 + 0x2cu, info.entry);
    MEM_W32(A1 + 0x30u, info.stack);
    MEM_W32(A1 + 0x34u, info.stackSize);
    MEM_W32(A1 + 0x38u, info.gpReg);
    MEM_W32(A1 + 0x3cu, info.initPriority);
    MEM_W32(A1 + 0x40u, info.currentPriority);
    MEM_W32(A1 + 0x44u, info.waitType);
    MEM_W32(A1 + 0x48u, info.waitId);
    MEM_W32(A1 + 0x4cu, info.wakeupCount);
    MEM_W32(A1 + 0x50u, info.exitStatus);
    MEM_W32(A1 + 0x54u, info.runClocksLow);
    MEM_W32(A1 + 0x58u, info.runClocksHigh);
    MEM_W32(A1 + 0x5cu, info.intrPreemptCount);
    MEM_W32(A1 + 0x60u, info.threadPreemptCount);
    MEM_W32(A1 + 0x64u, info.releaseCount);
    return 0;
}

/* I3: deepest-frame register snapshot when the worker (uid 0x115) is about to exit. The
 *   trace shows the worker exits via sceKernelExitThread after a
 *   Stop/Unload/Exit chain (run_err_new.log lines 165..180). The shutdown decision
 *   originates from a guest function (likely engine_Shutdown). Capturing r2/v0, r31/ra,
 *   r26/k0 stack, and PC at the moment the worker decides to unwind gives the calling
 *   function (i.e. the engine decision site). SR_EXITSNAP env-gates the trace. */
static int s_exitsnap_armed = -1;
typedef struct { uint32_t pc, ra, v0, k0, k0p4, a0, a1, a2, a3; uint64_t tick; } ExitSnap;
static ExitSnap s_last_worker_exit;
static int      s_last_worker_exit_valid = 0;
static int      s_last_worker_exit_count = 0;
void sr_exitsnap_capture(CpuState *s) {
    if (s_exitsnap_armed < 0) s_exitsnap_armed = getenv("SR_EXITSNAP") ? 1 : 0;
    if (!s_exitsnap_armed) return;
    if (!sched_current_is_worker() && !sched_current_is_launcher()) return;
    s_last_worker_exit.pc = s->pc;
    s_last_worker_exit.ra = s->r[31];
    s_last_worker_exit.v0 = s->r[2];
    s_last_worker_exit.k0 = s->r[26];
    s_last_worker_exit.k0p4 = s->r[26] ? MEM_R32(s->r[26] + 4) : 0;
    s_last_worker_exit.a0 = s->r[4];
    s_last_worker_exit.a1 = s->r[5];
    s_last_worker_exit.a2 = s->r[6];
    s_last_worker_exit.a3 = s->r[7];
    /* Tick is exposed via an extern since s_tick is static in sched.c. */
    s_last_worker_exit.tick = 0;
    s_last_worker_exit_valid = 1;
    s_last_worker_exit_count++;
    if (s_last_worker_exit_count <= 16) {
        fprintf(stderr, "EXITSNAP uid=0x%x pc=0x%08x ra=0x%08x v0=0x%08x k0=0x%08x k0+4=0x%08x a0..a3=%08x %08x %08x %08x\n",
                sched_current_uid(), s_last_worker_exit.pc, s_last_worker_exit.ra, s_last_worker_exit.v0,
                s_last_worker_exit.k0, s_last_worker_exit.k0p4,
                s_last_worker_exit.a0, s_last_worker_exit.a1, s_last_worker_exit.a2, s_last_worker_exit.a3);
        fflush(stderr);
    }
}
void sr_exitsnap_dump_latest(const char *tag) {
    if (!s_exitsnap_armed || !s_last_worker_exit_valid) return;
    fprintf(stderr, "EXITSNAP[%s]: pc=0x%08x ra=0x%08x v0=0x%08x k0=0x%08x k0+4=0x%08x\n",
            tag, s_last_worker_exit.pc, s_last_worker_exit.ra, s_last_worker_exit.v0,
            s_last_worker_exit.k0, s_last_worker_exit.k0p4);
    fflush(stderr);
}

/* Glue for the libc "should be called from main thread" path: when that string fires the
 * guest's libc reentrancy guard has already decided to shut down. The next syscall
 * EXECUTED by the same thread is normally sceKernelExitThread (it traps itself by
 * calling sceKernelExitThread with a poisoned stack), but if we let it run we drop
 * into the scheduler with NO runnable threads and just sit there burning vblanks.
 * This flag tells h_ExitThread to honour the guest's death wish and _exit(1) the host
 * cleanly instead. */
int sr_libc_death_wish = 0;
extern uint32_t sr_callback_table_register(uint32_t name_addr, uint32_t entry,
                                           uint32_t arg, uint32_t *error_out);

static uint32_t h_CreateCallback(CpuState *s) {
    uint32_t error = 0;
    uint32_t uid = sr_callback_table_register(A0, A1, A2, &error);
    if (getenv("SR_CBLOG")) {
        fprintf(stderr,
                "CBLOG: CreateCallback name=0x%08x entry=0x%08x common=0x%08x -> 0x%08x\n",
                A0, A1, A2, uid ? uid : error);
    }
    return uid ? uid : error;
}
static uint32_t h_SuspendDispatchThread(CpuState *s) {
    (void)s;
    return sched_suspend_dispatch();
}
static uint32_t h_ResumeDispatchThread(CpuState *s) {
    return sched_resume_dispatch(A0);
}
size_t s_callbacks_cap;

static int sr_callback_reserve_one(void) {
    if (s_callbacks_len < s_callbacks_cap) return 1;
    size_t next = s_callbacks_cap ? s_callbacks_cap * 2u : 64u;
    if (next < s_callbacks_cap || next > ((size_t)-1) / sizeof(*s_callbacks)) return 0;
    CallbackEntry *grown = (CallbackEntry *)realloc(s_callbacks, next * sizeof(*grown));
    if (!grown) return 0;
    memset(grown + s_callbacks_cap, 0, (next - s_callbacks_cap) * sizeof(*grown));
    s_callbacks = grown;
    s_callbacks_cap = next;
    return 1;
}

uint32_t sr_callback_table_register(uint32_t name_addr, uint32_t entry,
                                    uint32_t arg, uint32_t *error_out) {
    if (error_out) *error_out = 0x80020190u;
    if (!name_addr || !sr_inrange(name_addr)) {
        if (error_out) *error_out = 0x80020001u;
        return 0;
    }
    if (entry && !sr_inrange(entry)) {
        if (error_out) *error_out = 0x800200D3u;
        return 0;
    }
    if (!sr_callback_reserve_one()) return 0;

    CallbackEntry *cb = &s_callbacks[s_callbacks_len++];
    memset(cb, 0, sizeof(*cb));
    cb->uid = sr_alloc_uid();
    cb->entry = entry;
    cb->arg = arg;
    cb->owner_thread_uid = sched_current_uid();
    cb->used = 1;
    for (size_t i = 0; i < sizeof(cb->name) - 1u; i++) {
        uint32_t addr = name_addr + (uint32_t)i;
        if (!sr_inrange(addr)) break;
        cb->name[i] = (char)MEM_R8(addr);
        if (cb->name[i] == '\0') break;
    }
    cb->name[sizeof(cb->name) - 1u] = '\0';
    if (error_out) *error_out = 0;
    return cb->uid;
}

static int sr_thread_dispatch_callbacks_once(uint32_t thread_uid) {
    CpuState *cpu = sr_cpu_for_callbacks();
    if (!cpu) return 0;

    /* This non-blocking caller gets one slot-order pass over the table length captured before
     * guest code runs. Re-read each slot after the preceding callback because dispatch may
     * realloc the table. Re-notifications of visited slots and newly appended slots wait for
     * the next pump. */
    const size_t slot_limit = s_callbacks_len;
    int total_dispatched = 0;
    for (size_t i = 0; i < slot_limit; i++) {
        uint32_t selected_uid = 0u;
        if (i < s_callbacks_len && s_callbacks[i].used &&
            s_callbacks[i].owner_thread_uid == thread_uid && s_callbacks[i].pending) {
            selected_uid = s_callbacks[i].uid;
        }
        if (selected_uid != 0u)
            total_dispatched += sr_thread_dispatch_callback_uid(thread_uid, cpu, selected_uid);
    }
    return total_dispatched;
}
/* h_NotifyCallback: 0xc11ba8c4 — synchronously dispatch a registered callback entry from
 * the calling thread context. The PSP firmware exposes this so the game can manually pump
 * a callback instead of waiting for the vblank IRQ. We honour the uid; the dispatched entry
 * receives the ABI packed by sr_callback_pack_args ($a0 = count, $a1 = notify arg,
 * $a2 = registered common arg). */
static uint32_t h_NotifyCallback(CpuState *s) {
    return sr_callback_notify(A0, A1);
}

/* sceKernelCheckCallback: run any callbacks pending on the calling thread and
 * report whether at least one ran. The kernel's public return is Boolean (1 if a
 * callback was serviced, 0 if none were pending) -- guest code polls it in a loop
 * (e.g. waiting for the HOME-button exit callback) and only branches on
 * zero/nonzero. The internal pump (sr_thread_dispatch_callbacks) still returns a
 * count, which is collapsed to 0/1 here. */
static uint32_t h_CheckCallback(CpuState *s) {
    (void)s;
    if (!sr_thread_has_pending_callbacks(sched_current_uid())) return 0;
    return sr_thread_dispatch_callbacks() > 0 ? 1u : 0u;
}

static uint32_t h_CancelCallback(CpuState *s) {
    int idx = sr_callback_find_in_table(A0);
    if (idx < 0) return 0x800201A1u;
    s_callbacks[idx].pending = 0;
    s_callbacks[idx].notify_count = 0;
    s_callbacks[idx].notify_arg = 0;
    return 0;
}

static uint32_t h_GetCallbackCount(CpuState *s) {
    int idx = sr_callback_find_in_table(A0);
    if (idx < 0) return 0x800201A1u;
    return s_callbacks[idx].notify_count;
}

static uint32_t h_ReferCallbackStatus(CpuState *s) {
    uint32_t uid = A0;
    uint32_t infop = A1;
    int idx = sr_callback_find_in_table(uid);
    if (idx < 0) return 0x800201A1u;
    if (infop && MEM_R32(infop) != 0) {
        CallbackEntry *cb = &s_callbacks[idx];
        for (uint32_t i = 0; i < 56u; i++) MEM_W8(infop + i, 0);
        MEM_W32(infop + 0, 56u);
        for (uint32_t i = 0; i < 32u; i++)
            MEM_W8(infop + 4u + i, (uint8_t)cb->name[i]);
        MEM_W32(infop + 36, cb->owner_thread_uid);
        MEM_W32(infop + 40, cb->entry);
        MEM_W32(infop + 44, cb->arg);
        MEM_W32(infop + 48, cb->notify_count);
        MEM_W32(infop + 52, cb->notify_arg);
    }
    return 0;
}
/* h_CreateNotifyCallback: 0x9f9b46b9 — same as sceKernelCreateCallback but the callback uid
 * is registered for NOTIFY semantics: the game calls sceKernelNotifyCallback (0xc11ba8c4) on
 * it, OR the kernel auto-fires it when triggered programmatically. The PSMF modules and the
 * PSP-LDD-aware HL code paths use this variant to install their ring-fill and stream-event
 * callbacks (sceMpeg uses the "Notify" path for its NEAR/full callbacks; the IdStorage +
 * audio thread models assume one uid per NamedCallback binding). We register against the
 * same s_callbacks[] table as h_CreateCallback, so h_NotifyCallback and the callback-aware
 * wait paths (sr_thread_dispatch_callbacks) surface it the same way; the raw vblank IRQ does
 * NOT dispatch callbacks (sr_vblank_dispatch_registered() is a deliberate no-op — see its
 * comment above). */
static uint32_t h_CreateNotifyCallback(CpuState *s) {
    uint32_t error = 0;
    uint32_t uid = sr_callback_table_register(A0, A1, A2, &error);
    if (getenv("SR_CBLOG")) {
        fprintf(stderr,
                "CBLOG: CreateNotifyCallback name=0x%08x entry=0x%08x common=0x%08x -> 0x%08x\n",
                A0, A1, A2, uid ? uid : error);
    }
    return uid ? uid : error;
}
/* h_DeleteCallback: 0xedba5844 — release a callback uid from the s_callbacks[] table.
 * h_CreateCallback returns these uids (they're real s_callbacks[] slots, not module uid
 * pool). Returning success without freeing would leave a stale entry that fires every
 * vblank against deleted code. */
static uint32_t h_DeleteCallback(CpuState *s) {
    return sr_callback_table_unregister(A0) ? 0u : 0x800201A1u;
}
/* h_DeleteNotifyCallback: 0x0ed48fe2 — release a Notify-flavored callback uid
 * (allocated by h_CreateNotifyCallback). Same backing table; identical semantics. */
static uint32_t h_DeleteNotifyCallback(CpuState *s) {
    return sr_callback_table_unregister(A0) ? 0u : 0x800201A1u;
}

/* ---- CancelSema / CancelEventFlag / ReleaseWaitThread (PSP-B2-01 / PSP-B3-01,
 * campaign psp-hw-20260917).  These wake blocked waiters with an explicit
 * result instead of a satisfaction, which is why h_WaitSema / h_WaitEventFlag
 * consult sched_take_wake_result() right after each sched_block_on(). ---- */

#define SCE_KERNEL_ERROR_WAIT_RELEASE  0x800201aau

static uint32_t h_ReleaseWaitThread(CpuState *s) {
    /* a0=thid. PSP-B3-01 (psp-hw-20260917): a waiter is released with
     * WAIT_RELEASE; the running caller (by uid or 0) answers ILLEGAL_THID and a
     * deleted thread answers UNKNOWN_THID. */
    uint32_t uid = A0;
    uint32_t self = sched_current_uid();
    if (uid == 0u || uid == self) return 0x80020197u;
    if (!sched_wake_one_object_waiter_with_result(uid, SCE_KERNEL_ERROR_WAIT_RELEASE))
        return 0x80020198u;
    sched_preempt();
    return 0;
}

/* ---- sceKernelGetThreadmanIdList(type, readbuf, readbufsize, idcount) ----
 *
 * Signature and list classes: the public PSPSDK (pspthreadman.h, SceKernelIdListType);
 * readbufsize counts SceUIDs. The call writes min(readbufsize, total) uids of the class to
 * readbuf, stores the total in *idcount when idcount is not NULL, and returns the number
 * written (issue #1288). Every argument check runs before any guest write.
 *
 * UNMEASURED on a PSP: the list order (table order here), whether the return of a short
 * buffer is the number written or the total (the PSPSDK leaves it open), and the error
 * codes, which are the PSPSDK pspkerror.h names for each case: ILLEGAL_TYPE for a type
 * outside SceKernelIdListType, ILLEGAL_SIZE for a negative readbufsize, ILLEGAL_ADDR for
 * an unwritable readbuf span or idcount. Thread event handlers are a PSPSDK class this
 * runtime does not track (their registration is not implemented), so that type is a named
 * refusal, never an empty list. */
#define THREADMAN_ERROR_ILLEGAL_TYPE 0x800201bbu   /* SCE_KERNEL_ERROR_ILLEGAL_TYPE */

typedef struct {
    uint32_t readbuf;
    uint32_t cap;      /* uids readbuf holds */
    uint32_t total;    /* uids of the class */
} ThreadmanIdSink;

static void threadman_id_sink(uint32_t uid, void *ctx) {
    ThreadmanIdSink *sink = (ThreadmanIdSink *)ctx;
    if (sink->total < sink->cap) MEM_W32(sink->readbuf + 4u * sink->total, uid);
    sink->total++;
}

/* The classes the HLE object tables own. Returns 1 when `tmid` is one of them. */
static int hle_visit_threadman_ids(int tmid, SrUidVisitor visit, void *ctx) {
    switch (tmid) {
        case SR_TMID_SEMAPHORE:
        case SR_TMID_EVENT_FLAG: {
            const int kind = tmid == SR_TMID_SEMAPHORE ? SYNC_KIND_SEMA : SYNC_KIND_EVENT_FLAG;
            for (int i = 0; i < 128; i++)
                if (s_sync[i].used && s_sync[i].kind == kind) visit(s_sync[i].uid, ctx);
            return 1;
        }
        case SR_TMID_MBOX:
            for (int i = 0; i < MBX_MAX; i++)
                if (s_mbx[i].used) visit(s_mbx[i].uid, ctx);
            return 1;
        case SR_TMID_VPL:   /* vpl_lookup: uid = 0x600 + slot */
            for (uint32_t i = 0; i < VPL_MAX; i++)
                if (s_vpls[i].used) visit(0x600u + i, ctx);
            return 1;
        case SR_TMID_FPL:   /* fpl_lookup: uid = 0x500 + slot */
            for (uint32_t i = 0; i < FPL_MAX; i++)
                if (s_fpls[i].used) visit(0x500u + i, ctx);
            return 1;
        case SR_TMID_MPIPE:
            for (int i = 0; i < MSG_PIPE_MAX; i++)
                if (s_msg_pipes[i].used) visit(s_msg_pipes[i].uid, ctx);
            return 1;
        case SR_TMID_CALLBACK:
            for (size_t i = 0; i < s_callbacks_len; i++)
                if (s_callbacks[i].used) visit(s_callbacks[i].uid, ctx);
            return 1;
        case SR_TMID_VTIMER:
            for (int i = 0; i < VTIMER_MAX; i++)
                if (s_vtimers[i].used) visit(s_vtimers[i].uid, ctx);
            return 1;
        default:
            return 0;
    }
}

static int threadman_id_type_listed(int tmid) {
    return (tmid >= SR_TMID_THREAD && tmid <= SR_TMID_VTIMER) ||
           (tmid >= SR_TMID_SLEEP_THREAD && tmid <= SR_TMID_DORMANT_THREAD);
}

static uint32_t h_GetThreadmanIdList(CpuState *s) {
    const int tmid = (int)(int32_t)A0;
    const uint32_t readbuf = A1;
    const int32_t readbufsize = (int32_t)A2;
    const uint32_t idcount = A3;
    if (!threadman_id_type_listed(tmid)) return THREADMAN_ERROR_ILLEGAL_TYPE;
    if (tmid == SR_TMID_THREAD_EVENT_HANDLER) {
        return hle_unmodelled_stop("sceKernelGetThreadmanIdList",
                                   "type %d (ThreadEventHandler): thread event handlers are "
                                   "not modelled", tmid);
    }
    if (readbufsize < 0) return HLE_KERNEL_ERROR_ILLEGAL_SIZE;
    if (readbufsize > 0) {
        const uint64_t bytes = (uint64_t)(uint32_t)readbufsize * 4u;
        if (!readbuf || bytes > UINT32_MAX || !sr_guest_span_writable(readbuf, (uint32_t)bytes))
            return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    if (idcount && !sr_guest_span_writable(idcount, 4u)) return HLE_KERNEL_ERROR_ILLEGAL_ADDR;

    ThreadmanIdSink sink = { readbuf, (uint32_t)readbufsize, 0u };
    if (!sched_visit_threadman_ids(tmid, threadman_id_sink, &sink) &&
        !hle_visit_threadman_ids(tmid, threadman_id_sink, &sink)) {
        /* A listed PSPSDK class with no owner here: fail closed by name. */
        return hle_unmodelled_stop("sceKernelGetThreadmanIdList",
                                   "type %d has no object table in this runtime", tmid);
    }
    if (idcount) MEM_W32(idcount, sink.total);
    return sink.total < sink.cap ? sink.total : sink.cap;
}

void hle_register_thread_exit_handlers(void) {
    sr_hle_register(0xaa73c935, "sceKernelExitThread", h_ExitThread);
    sr_hle_register(0x809ce29b, "sceKernelExitDeleteThread", h_ExitDeleteThread);
    sr_hle_register(0x278c0df5, "sceKernelWaitThreadEnd", h_WaitThreadEnd);
    sr_hle_register(0x840e8133, "sceKernelWaitThreadEndCB", h_WaitThreadEndCB);
    sr_hle_register(0x17c1684e, "sceKernelReferThreadStatus", h_ReferThreadStatus);
}

/* sceKernelReferThreadProfiler / sceKernelReferGlobalProfiler return a pointer to the
 * profiler registers, or NULL when no profiler data is available. This runtime models no
 * profiler, so the honest answer is always NULL, which is what firmware with profiling off
 * reports. The handler writes nothing; a controlled refusal would instead return a non-NULL
 * error value in $v0 that the title dereferences as a pointer. NULL-ness on retail firmware
 * is not hardware-measured. */
static uint32_t h_ReferProfilerNull(CpuState *s) {
    (void)s;
    return 0u;
}

/* Imports that were absent from the registry and stopped titles on an unknown NID. Each
 * is either a real handler over state the runtime already models (the sysclock conversion
 * pair and sceCtrlGetSamplingMode, the latter registered with the ctrl sampling helper), or a
 * named controlled refusal. A refusal is never fake success: no virtual timer, hold-mode,
 * UMD popup, battery icon, sceNet, sceHttp, or audiocodec state exists to back these calls. ThreadMan and sceDisplay refusals use SCE_KERNEL_ERROR_NOTIMP, the code the
 * kernel refusals above already use; the non-kernel sceImpose, sceNet, sceHttp and
 * sceAudiocodec libraries use SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED, the code the
 * sceReg and sceUmd refusals use. The two NIDs with no confirmed public name stay unregistered. */
/* sceKernelReferSystemStatus(SceKernelSystemStatus *status). PSP-3000, firmware 6.61 (PR #1003 head
 * 3072bd7d, docs/HARDWARE_ORACLE.md 2026-10-15 entry): the call returns 0 and writes min(size, 0x1c)
 * bytes, where size is the caller's first word. The size word reads back 0x1c for non-zero sizes 0x08,
 * 0x1c and 0x20; size 0 writes nothing. Status word 1 read 0 in all three records but is not cited.
 * The idle-clock, comes-out-of-idle and thread-switch counters are written as zero; hardware reports
 * non-zero, time-dependent values (not cited). That zero fill is a known divergence from hardware. The
 * VFPU-switch counter is also written as zero and is not cited. A NULL pointer hangs on the
 * PSP-3000 (PSP-HLE-NULL-OUT-001); ILLEGAL_ADDR is a runtime safety choice, not a measured result.
 * Other unwritable non-NULL pointers remain unmeasured. */
static uint32_t h_ReferSystemStatus(CpuState *s) {
    uint32_t out = A0;
    /* Runtime safety choice: PSP-3000 hangs on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), so no game relies on this value. */
    if (!out || !sr_guest_span_writable(out, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t size = MEM_R32(out);
    uint32_t n = size < 0x1cu ? size : 0x1cu;
    if (n > 4u && !sr_guest_span_writable(out, n)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint8_t image[0x1c];
    memset(image, 0, sizeof image);
    image[0] = 0x1cu; /* size field, little-endian */
    for (uint32_t i = 0; i < n; i++) MEM_W8(out + i, image[i]);
    return 0;
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_cancel_release_handlers(). */
void hle_register_cancel_release_kernel_thread(void) {
    sr_hle_register(0x2c34e053, "sceKernelReleaseWaitThread", h_ReleaseWaitThread);
}

/* From hle_register_kernel_import_sweep_handlers(). */
void hle_register_kernel_import_sweep_kernel_thread_1(void) {
    sr_hle_register(0xea748e31, "sceKernelChangeCurrentThreadAttr", h_ChangeCurrentThreadAttr);
    sr_hle_register(0x912354a7, "sceKernelRotateThreadReadyQueue", h_RotateThreadReadyQueue);
    sr_hle_register(0x75156e8f, "sceKernelResumeThread", h_ResumeThread);
    sr_hle_register(0x9944f31f, "sceKernelSuspendThread", h_SuspendThread);
    sr_hle_register(0x71bc9871, "sceKernelChangeThreadPriority", h_ChangeThreadPriority);
}

/* From hle_register_kernel_import_sweep_handlers(). */
void hle_register_kernel_import_sweep_kernel_thread_2(void) {
    sr_hle_register(0x94416130u, "sceKernelGetThreadmanIdList", h_GetThreadmanIdList);
    sr_hle_register(0x52089ca1, "sceKernelGetThreadStackFreeSize", h_GetThreadStackFreeSize);
}

/* From hle_register_launch_sweep_blocker_handlers(). */
void hle_register_launch_sweep_blocker_kernel_thread(void) {
    sr_hle_register(0xd13bde95, "sceKernelCheckThreadStack", h_CheckThreadStack);
}

/* From hle_register_selftest_oracle_handlers(). */
void hle_register_selftest_oracle_kernel_thread(void) {
    sr_hle_register(0x446d8de6, "sceKernelCreateThread", h_CreateThread);
    sr_hle_register(0xf475845d, "sceKernelStartThread", h_StartThread);
    sr_hle_register(0x293b45b8, "sceKernelGetThreadId", h_GetThreadIdSched);
    sr_hle_register(0x3b183e26, "sceKernelGetThreadExitStatus", h_GetThreadExitStatus);
    sr_hle_register(0x9fa03cd3, "sceKernelDeleteThread", h_DeleteThread);
    sr_hle_register(0x383f7bcc, "sceKernelTerminateDeleteThread", h_TerminateDeleteThread);
    sr_hle_register(0xd59ead2f, "sceKernelWakeupThread", h_WakeupThread);
    sr_hle_register(0x9ace131e, "sceKernelSleepThread", h_SleepThread);
    sr_hle_register(0xe81caf8f, "sceKernelCreateCallback", h_CreateCallback);
    sr_hle_register(0xedba5844, "sceKernelDeleteCallback", h_DeleteCallback);
    sr_hle_register(0xc11ba8c4, "sceKernelNotifyCallback", h_NotifyCallback);
    sr_hle_register(0x349d6d6c, "sceKernelCheckCallback", h_CheckCallback);
    sr_hle_register(0xba4051d6, "sceKernelCancelCallback", h_CancelCallback);
    sr_hle_register(0x2a3d44ff, "sceKernelGetCallbackCount", h_GetCallbackCount);
}

/* From hle_register_time_handlers(). */
void hle_register_time_kernel_thread(void) {
    sr_hle_register(0x3ad58b8c, "sceKernelSuspendDispatchThread", h_SuspendDispatchThread);
    sr_hle_register(0x27e22ec2, "sceKernelResumeDispatchThread", h_ResumeDispatchThread);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_kernel_thread_1(void) {
    sr_hle_register(0x64d4540eu, "sceKernelReferThreadProfiler", h_ReferProfilerNull);
    sr_hle_register(0x8218b4ddu, "sceKernelReferGlobalProfiler", h_ReferProfilerNull);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_kernel_thread_2(void) {
    sr_hle_register(0x627e6f3au, "sceKernelReferSystemStatus", h_ReferSystemStatus);
}

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_kernel_thread(void) {
    sr_hle_register(0xceadeb47, "sceKernelDelayThread", h_DelayThread);
    sr_hle_register(0x68da9e36, "sceKernelDelayThreadCB", h_DelayThreadCB);
    sr_hle_register(0xbd123d9e, "sceKernelDelaySysClockThread", h_DelaySysClockThread);
    sr_hle_register(0x1181e963, "sceKernelDelaySysClockThreadCB", h_DelaySysClockThreadCB);
    sr_hle_register(0x616403ba, "sceKernelTerminateThread", h_TerminateThread);
    sr_hle_register(0x82826f70, "sceKernelSleepThreadCB", h_SleepThreadCB);
}

#ifndef SR_MPEG_MEDIA_SELFTEST
#ifndef SR_HLE_THREAD_SELFTEST
/* From sr_hle_init(). */
void hle_register_init_kernel_thread_1(void) {
    sr_hle_register(0x9f9b46b9, "sceKernelCreateNotifyCallback", h_CreateNotifyCallback);
    sr_hle_register(0x0ed48fe2, "sceKernelDeleteNotifyCallback", h_DeleteNotifyCallback);
    sr_hle_register(0x94aa61ee, "sceKernelGetThreadCurrentPriority", h_GetThreadPriority);
    sr_hle_register(0xfccfad26, "sceKernelCancelWakeupThread", h_CancelWakeupThread);
    sr_hle_register(0xffc36a14, "sceKernelReferThreadRunStatus", h_ReferThreadRunStatus);
}
#endif
#endif

#ifndef SR_MPEG_MEDIA_SELFTEST
#ifndef SR_HLE_THREAD_SELFTEST
/* From sr_hle_init(). */
void hle_register_init_kernel_thread_2(void) {
    sr_hle_register(0x730ed8bc, "sceKernelReferCallbackStatus", h_ReferCallbackStatus);
}
#endif
#endif
