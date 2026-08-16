// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * White-box unit tests for the cooperative scheduler's decision logic and guest-thread
 * lifecycle invariants. Standalone host executable, no game inputs required:
 *
 *   mingw32-make GAME_NAME=hst GAME_ELF=eboot.elf GAME_BASE=0 GAME_ENTRY=0 sched-selftest
 *
 * The harness #includes sched.c so the TCB table, pick_next(), and the rotation cursor
 * are directly inspectable; sr_coro.c is linked normally (its public API is under test).
 *
 * The assertions encode the PSP scheduling MODEL, not the current implementation:
 *   - strict priority: a READY thread never wins while a READY thread with a numerically
 *     lower priority value exists (no anti-starvation override -- hardware has none);
 *   - equal-priority peers make deterministic round-robin progress;
 *   - DORMANT, sleeping, and object/delay-blocked threads are never selected;
 *   - identical scheduler state produces identical decisions;
 *   - a thread entry that RETURNS is terminated through the PSP implicit-return path:
 *     positive exit status is recorded unchanged, signed-negative returns normalize to
 *     ILLEGAL_ARGUMENT, WaitThreadEnd joiners are released, and the TCB becomes DORMANT;
 *   - sr_coro: switching to the current coroutine is a defined no-op, and a coroutine
 *     whose body returns parks by transferring to the main coroutine (no fiber
 *     self-switch, no busy spin);
 *   - role UIDs (root / worker / launcher) are captured dynamically at create time;
 *   - a stack request that does not fit the arena FAILS the create (no silent clamp).
 */

#include "sched.c"   /* white-box: statics (s_tcb, s_ntcb, s_last_pick, ...) visible */

#include <stdlib.h>

/* ---- stubs for runtime symbols sched.c references ---------------------------------- */

uint8_t *g_mem;
static uint8_t *g_mem_base;

uint32_t g_sr_debug = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
int g_sr_heap_watch = 0;
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
void sr_oor(uint32_t a, uint32_t v, int store) { (void)a; (void)v; (void)store; }
uint32_t sr_get_ge_status(void) { return 0; }
uint32_t g_frame_prims = 0;

int gui_on(void) { return 0; }
void gui_pump(void) {}
static uint32_t g_test_vblank_handler;
static unsigned g_test_handler_calls;
static int g_test_handler_raise_ge;
static CpuState g_test_handler_seen;
uint32_t sr_vblank_handler(void) { return g_test_vblank_handler; }
uint32_t sr_vblank_arg(void) { return 0; }
int sr_vblank_dispatch_registered(void) { return 0; }
static unsigned g_test_vblank_delivered;
void sr_vblank_tick(void) { g_test_vblank_delivered++; }
void sr_callback_unregister_owner(uint32_t thread_uid) { (void)thread_uid; }

uint64_t sr_perf_now_ns(void) { return 0; }
void sr_perf_guest_begin(void) {}
void sr_perf_guest_end(void) {}
void sr_perf_guest_idle_wait(uint64_t started_ns) { (void)started_ns; }
void sr_perf_vblank(void) {}

/* sr_newlib_malloc / g_hle_depth are referenced by sched.c but live in the full
 * runtime (recomp.c). The selftest does not exercise the real allocator, so we
 * provide a tiny static arena stub and a plain global here. */
int g_hle_depth = 0;
static uint8_t s_selftest_arena[1 << 20];
static size_t s_selftest_arena_off;
uint32_t sr_newlib_malloc(uint32_t size, uint32_t guest_ra) {
    (void)guest_ra;
    size = (size + 15u) & ~(uint32_t)15u;
    if (s_selftest_arena_off + size > sizeof(s_selftest_arena)) return 0u;
    uint32_t p = (uint32_t)(uintptr_t)(s_selftest_arena + s_selftest_arena_off);
    s_selftest_arena_off += size;
    return p;
}

static uint32_t s_test_uid_next = 0x110u;
uint32_t sr_alloc_uid(void) { return s_test_uid_next++; }

/* dispatch() stand-in: run the configured C body as "the guest thread". */
typedef void (*TestBody)(CpuState *);
static TestBody g_test_body;
void dispatch(CpuState *s, uint32_t target) {
    if (target == g_test_vblank_handler && target != 0u) {
        g_test_handler_calls++;
        memcpy(&g_test_handler_seen, s, sizeof(g_test_handler_seen));
        if (g_test_handler_raise_ge) {
            sched_raise_interrupt(SCHED_INTR_GE);
            g_test_handler_raise_ge = 0;
        }
        /* Prove that a handler's register mutations are discarded with its frame. */
        s->r[16] = 0xdeadbeefu;
        s->pc = 0xfeedfaceu;
    } else if (g_test_body) {
        g_test_body(s);
    }
}

/* ---- tiny check framework ---------------------------------------------------------- */

static int g_checks, g_fails;
static void expect(int cond, const char *what) {
    g_checks++;
    if (!cond) {
        g_fails++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

/* ---- helpers ----------------------------------------------------------------------- */

static CpuState g_cpu_store;

/* Full state reset so every test starts from an identical scheduler world. */
static void reset_sched(void) {
    memset(g_mem_base, 0, 0x0c000000u);
    memset(s_tcb, 0, sizeof(s_tcb));
    memset(s_libc_threads, 0, sizeof(s_libc_threads));
    s_ntcb = 0;
    s_cur = -1;
    s_last_pick = -1;
    s_root_seen = 0;
    g_root_uid = 0x110u;
    g_worker_uid = 0x114u;
    g_launcher_uid = 0x111u;
    g_master_reent = 0x002cf338u;
    s_stack_top = 0x09f00000u;
    stack_ranges_reset();
    s_vtime_us = 0;
    s_tick = 0;
    s_interrupts_enabled = 1;
    s_pending_interrupts = 0;
    s_servicing_interrupts = 0;
    s_vbl_event_period_rem = 0;
    s_vbl_next_us = 0;
    s_vblank_q_us = -1;
    s_last_vblank_ns = 0;
    g_test_vblank_delivered = 0;
    g_test_vblank_handler = 0;
    g_test_handler_calls = 0;
    g_test_handler_raise_ge = 0;
    memset(&g_test_handler_seen, 0, sizeof(g_test_handler_seen));
    s_test_uid_next = 0x110u;
    g_test_body = NULL;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));
    s_cpu = &g_cpu_store;
    s_pace_on = 0;   /* turbo: no host-clock sleeps if a vblank path ever fires */
    s_host_ns_fn = NULL;
    s_clock_epoch_ns = 0;
    s_vbl_next_ns = 0;
    s_vbl_period_rem = 0;
    s_vtime_period_rem = 0;
}

/* ---- controlled host monotonic clock ------------------------------------------------
 * Paced mode anchors guest virtual time to the host monotonic clock, so asserting a
 * paced timing contract against the real clock would be a race. sched.c routes every
 * host-time read through its host_now_ns() seam; these tests install a source they
 * move explicitly, so nothing but the test advances host time. */
static uint64_t g_test_host_ns;
static uint64_t test_host_ns(void) { return g_test_host_ns; }
static void set_host_us(uint64_t us) { g_test_host_ns = us * 1000ull; }

/* Install the controlled source and put every host-anchored timing variable at a
 * known common origin. `paced` selects the production profile (host-anchored) or
 * turbo (SR_NOVBPACE=1, virtual clock advanced explicitly). */
static void begin_clock_fixture(int paced, uint64_t host_us_now) {
    s_host_ns_fn = test_host_ns;
    set_host_us(host_us_now);
    s_pace_on = paced ? 1 : 0;
    s_clock_epoch_ns = 0;
    s_vbl_next_ns = 0;
    s_vbl_period_rem = 0;
    s_vtime_period_rem = 0;
    s_vbl_event_period_rem = 0;
    s_vbl_next_us = 0;
    s_vtime_us = 0;
    s_pending_interrupts = 0;
    s_vblank_q_us = 16000;
    s_last_vblank_ns = g_test_host_ns;
}

/* Silence both VBLANK paths so a timing test can isolate one contract. */
static void disable_vblank_sources(void) {
    s_vbl_next_us = (uint64_t)-1;
    s_vblank_q_us = 0x7fffffff;
}

/* Fabricate a bare TCB for pure pick_next() decision tests (no stack, no coroutine). */
static int mk(uint32_t uid, int state, int pri) {
    TCB *t = &s_tcb[s_ntcb];
    memset(t, 0, sizeof(*t));
    t->uid = uid;
    t->state = state;
    t->priority = pri;
    t->wake = (uint64_t)-1;
    return s_ntcb++;
}

static int index_of_uid(uint32_t uid) {
    for (int i = 0; i < s_ntcb; i++)
        if (s_tcb[i].uid == uid) return i;
    return -1;
}

/* Mirror of sched_run's resume block: run one slice of thread s_tcb[idx] until it
 * yields, blocks, or exits. */
static void run_one_slice(int idx) {
    TCB *t = &s_tcb[idx];
    s_cur = idx;
    t->state = TH_RUNNING;
    memcpy(s_cpu, &t->saved, sizeof(CpuState));
    if (t->k0_init) {
        s_cpu->r[26] = t->k0_init;
        t->saved.r[26] = t->k0_init;
    }
    atomic_store_explicit(&sr_timeslice, TIMESLICE, memory_order_relaxed);
    if (!t->started) {
        t->started = 1;
        t->coro = sr_coro_create(coro_body, t, (size_t)4 << 20);
    }
    sr_coro_switch(t->coro);
    s_cur = -1;
}

/* ---- pick_next() decision tests ---------------------------------------------------- */

static void test_single_ready(void) {
    reset_sched();
    int a = mk(0x200, TH_READY, 32);
    for (int i = 0; i < 5; i++)
        expect(pick_next() == a, "single READY thread is always selected");
}

static void test_priority_wins_and_never_inverts(void) {
    reset_sched();
    int hi = mk(0x200, TH_READY, 20);   /* lower number = higher priority */
    int lo = mk(0x201, TH_READY, 50);
    (void)lo;
    /* Scenario of the removed anti-starvation bug: the same high-priority thread keeps
     * winning while a lower-priority thread stays READY. Strict priority: the
     * lower-priority thread must NEVER be selected, no matter how many decisions pass. */
    for (int i = 0; i < 1000; i++)
        expect(pick_next() == hi,
               "lower-priority READY thread never preempts a higher-priority READY thread");
}

static void test_equal_priority_round_robin(void) {
    reset_sched();
    int a = mk(0x200, TH_READY, 30);
    int b = mk(0x201, TH_READY, 30);
    int first = pick_next();
    int second = pick_next();
    int third = pick_next();
    int fourth = pick_next();
    expect(first != second, "equal-priority peers alternate");
    expect(first == third && second == fourth, "two-peer rotation has period 2");
    expect((first == a && second == b) || (first == b && second == a),
           "both equal-priority peers are selected");
}

static void test_three_equal_priority_rotation(void) {
    reset_sched();
    int a = mk(0x200, TH_READY, 40);
    int b = mk(0x201, TH_READY, 40);
    int c = mk(0x202, TH_READY, 40);
    int seen[3] = {0, 0, 0};
    int seq[6];
    for (int i = 0; i < 6; i++) {
        seq[i] = pick_next();
        if (seq[i] == a) seen[0]++;
        if (seq[i] == b) seen[1]++;
        if (seq[i] == c) seen[2]++;
    }
    expect(seen[0] == 2 && seen[1] == 2 && seen[2] == 2,
           "three equal-priority peers each run twice in six decisions");
    expect(seq[0] == seq[3] && seq[1] == seq[4] && seq[2] == seq[5],
           "three-peer rotation has period 3");
}

static void test_mixed_priorities(void) {
    reset_sched();
    int mid  = mk(0x200, TH_READY, 40);
    int p1   = mk(0x201, TH_READY, 20);
    int p2   = mk(0x202, TH_READY, 20);
    int low  = mk(0x203, TH_READY, 90);
    int blk  = mk(0x204, TH_WAIT_OBJ, 20);   /* best priority but blocked */
    for (int i = 0; i < 100; i++) {
        int got = pick_next();
        expect(got == p1 || got == p2,
               "only READY threads at the best priority are ever selected");
        expect(got != mid && got != low && got != blk,
               "worse-priority and blocked threads are never selected");
    }
}

static void test_sleeping_excluded(void) {
    reset_sched();
    int sleeper = mk(0x200, TH_WAIT_OBJ, 10);
    s_tcb[sleeper].sleeping = 1;
    s_tcb[sleeper].wait_obj = 0x200;   /* sceKernelSleepThread marker: waits on own uid */
    int other = mk(0x201, TH_READY, 60);
    for (int i = 0; i < 10; i++)
        expect(pick_next() == other, "sleeping thread is excluded despite best priority");
    sched_thread_wakeup(0x200);
    expect(s_tcb[sleeper].state == TH_READY, "wakeup readies the sleeping thread");
    expect(pick_next() == sleeper, "woken thread wins on priority");
}

static void test_blocked_excluded_until_wake(void) {
    reset_sched();
    int waiter = mk(0x200, TH_WAIT_OBJ, 10);
    s_tcb[waiter].wait_obj = 0xABCu;
    int other = mk(0x201, TH_READY, 60);
    for (int i = 0; i < 10; i++)
        expect(pick_next() == other, "object-blocked thread is excluded despite best priority");
    sched_wake(0xABCu);
    expect(pick_next() == waiter, "sched_wake readies the blocked thread and it wins on priority");
}

static void test_dormant_excluded(void) {
    reset_sched();
    int dead = mk(0x200, TH_DORMANT, 1);
    (void)dead;
    int live = mk(0x201, TH_READY, 99);
    expect(pick_next() == live, "DORMANT thread is never selected even at best priority");
    s_tcb[live].state = TH_DORMANT;
    expect(pick_next() == -1, "no runnable threads yields -1");
}

static void test_current_becomes_nonrunnable(void) {
    reset_sched();
    int a = mk(0x200, TH_READY, 30);
    int b = mk(0x201, TH_READY, 30);
    int first = pick_next();
    s_tcb[first].state = TH_WAIT_OBJ;      /* previous winner blocks */
    int second = pick_next();
    expect(second == (first == a ? b : a), "selection moves off a thread that blocked");
    s_tcb[second].state = TH_WAIT_OBJ;
    expect(pick_next() == -1, "all blocked yields -1");
}

static void test_timed_wait_promotion(void) {
    reset_sched();
    int d = mk(0x200, TH_WAIT_DELAY, 30);
    s_tcb[d].wake = 100;
    s_vtime_us = 50;
    expect(pick_next() == -1, "delay not yet expired: nothing runnable");
    s_vtime_us = 100;
    expect(pick_next() == d, "expired delay promotes the thread to READY and selects it");
    expect(s_tcb[d].state == TH_RUNNING || s_tcb[d].state == TH_READY,
           "promoted thread is runnable");
}

static void test_determinism(void) {
    reset_sched();
    mk(0x200, TH_READY, 30);
    mk(0x201, TH_READY, 30);
    mk(0x202, TH_READY, 45);
    mk(0x203, TH_WAIT_OBJ, 10);
    int seq1[8], seq2[8];
    s_last_pick = -1;
    for (int i = 0; i < 8; i++) seq1[i] = pick_next();
    s_last_pick = -1;                       /* identical state: cursor reset, TCBs untouched */
    for (int i = 0; i < 8; i++) seq2[i] = pick_next();
    int same = 1;
    for (int i = 0; i < 8; i++) same &= (seq1[i] == seq2[i]);
    expect(same, "identical scheduler state produces identical decision sequences");
}

/* ---- lifecycle tests (real coroutines) --------------------------------------------- */

static void body_returns_42(CpuState *s) {
    s->r[2] = 42;   /* v0: the entry's return value */
}

static void body_returns_thread_error(CpuState *s) {
    s->r[2] = 0x800201acu;   /* signed-negative entry return observed on PSP */
}

static void body_returns_wait_timeout(CpuState *s) {
    s->r[2] = 0x800201a8u;   /* second PSP kernel-error boundary control */
}

static int32_t g_explicit_exit_status;
static void body_explicit_exit(CpuState *s) {
    (void)s;
    sched_exit_current(g_explicit_exit_status);
}

static void test_entry_return_is_thread_exit(void) {
    reset_sched();
    uint32_t uid = sched_create_thread(0x1000u, 32, 0x2000u);
    expect(uid != 0, "create succeeds");
    int j = mk(0x300, TH_WAIT_OBJ, 40);
    s_tcb[j].wait_obj = uid;               /* a sceKernelWaitThreadEnd joiner */
    sched_start_thread(uid, 0, 0);
    g_test_body = body_returns_42;
    int idx = index_of_uid(uid);
    expect(idx >= 0, "created thread present");
    run_one_slice(idx);
    expect(s_tcb[idx].state == TH_DORMANT, "entry return leaves the thread DORMANT");
    expect(s_tcb[idx].exit_status == 42,
           "entry return records v0 as the exit status (implicit sceKernelExitThread)");
    expect(s_tcb[j].state == TH_READY, "entry return releases WaitThreadEnd joiners");
    expect(sched_thread_exit_status(uid) == 42, "GetThreadExitStatus sees the return value");
}

static void test_negative_entry_return_normalizes_to_illegal_argument(void) {
    reset_sched();
    uint32_t uid = sched_create_thread(0x1001u, 32, 0x2000u);
    expect(uid != 0, "negative-return thread creates");
    sched_start_thread(uid, 0, 0);
    g_test_body = body_returns_thread_error;
    int idx = index_of_uid(uid);
    expect(idx >= 0, "negative-return thread is present");
    run_one_slice(idx);
    expect(s_tcb[idx].state == TH_DORMANT,
           "negative entry return leaves the thread DORMANT");
    expect((uint32_t)s_tcb[idx].exit_status == 0x800200d2u,
           "negative entry return latches ILLEGAL_ARGUMENT");
    expect(sched_thread_exit_status(uid) == 0x800200d2u,
           "GetThreadExitStatus exposes the normalized negative return");
}

static void test_wait_timeout_entry_return_normalizes_to_illegal_argument(void) {
    reset_sched();
    uint32_t uid = sched_create_thread(0x1003u, 32, 0x2000u);
    expect(uid != 0, "wait-timeout-return thread creates");
    expect(sched_start_thread(uid, 0, 0) == 0, "wait-timeout-return thread starts");
    g_test_body = body_returns_wait_timeout;
    int idx = index_of_uid(uid);
    expect(idx >= 0, "wait-timeout-return thread is present");
    run_one_slice(idx);
    expect(s_tcb[idx].state == TH_DORMANT,
           "wait-timeout entry return leaves the thread DORMANT");
    expect((uint32_t)s_tcb[idx].exit_status == 0x800200d2u,
           "wait-timeout entry return latches ILLEGAL_ARGUMENT");
    expect(sched_thread_exit_status(uid) == 0x800200d2u,
           "GetThreadExitStatus exposes the normalized wait-timeout return");
}

static void test_explicit_exit_status(uint32_t supplied, uint32_t expected) {
    reset_sched();
    uint32_t uid = sched_create_thread(0x1002u, 32, 0x2000u);
    expect(uid != 0, "explicit-status thread creates");
    int j = mk(0x301, TH_WAIT_OBJ, 40);
    s_tcb[j].wait_obj = uid;
    s_tcb[j].join_target = uid;
    s_tcb[j].join_waiting = 1;
    expect(sched_start_thread(uid, 0, 0) == 0, "explicit-status thread starts");
    g_explicit_exit_status = (int32_t)supplied;
    g_test_body = body_explicit_exit;
    int idx = index_of_uid(uid);
    expect(idx >= 0, "explicit-status thread is present");
    run_one_slice(idx);
    expect(s_tcb[idx].state == TH_DORMANT,
           "explicit ExitThread leaves the thread DORMANT");
    expect((uint32_t)s_tcb[idx].exit_status == expected,
           "explicit ExitThread applies the signed-negative rule or preserves a positive status");
    expect(s_tcb[j].state == TH_READY && s_tcb[j].join_result == expected,
           "explicit ExitThread releases WaitThreadEnd with the latched status");
    expect(sched_thread_exit_status(uid) == expected,
           "GetThreadExitStatus exposes the explicit status observed by the scheduler");
}

static void test_terminate_thread_excluded_after(void) {
    reset_sched();
    uint32_t uid = sched_create_thread(0x1000u, 5, 0x2000u);
    sched_start_thread(uid, 0, 0);
    int idx = index_of_uid(uid);
    int other = mk(0x300, TH_READY, 90);
    expect(pick_next() == idx, "started thread wins on priority");
    sched_terminate_thread(uid);
    expect(s_tcb[idx].state == TH_DORMANT, "terminated thread is DORMANT");
    s_last_pick = -1;
    expect(pick_next() == other, "terminated thread is never selected again");
    expect((uint32_t)s_tcb[idx].exit_status == 0x800201acu,
           "terminated thread reports SCE_KERNEL_ERROR_THREAD_TERMINATED");
}

static void test_delete_and_terminate_delete_contract(void) {
    reset_sched();
    uint32_t target_uid = sched_create_thread(0x2400u, 40, 0x2000u);
    TCB *target = tcb_by_uid(target_uid);
    expect(target_uid != 0 && target != NULL, "lifecycle contract target creates");
    uint32_t target_stack = target ? target->stack_base : 0u;
    expect(sched_start_thread(target_uid, 0, 0) == 0,
           "lifecycle contract target starts once");
    expect(sched_start_thread(target_uid, 0, 0) == 0x800201a4u,
           "duplicate StartThread is rejected with NOT_DORMANT");
    expect(sched_start_thread(0, 0, 0) == 0x80020197u,
           "null UID StartThread is rejected with ILLEGAL_THID");
    if (target) target->state = TH_DORMANT;

    int waiter = mk(0x241u, TH_WAIT_OBJ, 41);
    s_tcb[waiter].wait_obj = target_uid;
    s_tcb[waiter].join_target = target_uid;
    s_tcb[waiter].join_waiting = 1;
    expect(sched_terminate_thread(target_uid) == 0,
           "TerminateDelete accepts a valid dormant target before deletion");
    expect(sched_delete_thread(target_uid) == 0,
           "dormant target deletes successfully");
    expect(sched_thread_exit_status(target_uid) == 0x80020198u &&
           sched_start_thread(target_uid, 0, 0) == 0x80020198u &&
           sched_thread_wakeup(target_uid) == 0x80020198u,
           "deleted target rejects status/start/wakeup operations");
    expect(s_tcb[waiter].state == TH_READY && s_tcb[waiter].join_result_valid &&
           s_tcb[waiter].join_result == 0x800201acu,
           "direct DeleteThread releases a waiting joiner with THREAD_TERMINATED");

    uint32_t replacement_uid = sched_create_thread(0x2401u, 40, 0x2000u);
    TCB *replacement = tcb_by_uid(replacement_uid);
    expect(replacement_uid != 0 && replacement && replacement->stack_base == target_stack,
           "DeleteThread returns its stack range to the allocator");
    if (replacement) expect(sched_delete_thread(replacement_uid) == 0,
                            "replacement object deletes after range reuse");
}

/* ---- role-UID capture tests -------------------------------------------------------- */

static void test_role_uid_capture(void) {
    reset_sched();
    uint32_t root = sched_create_thread(0x00001000u, 32, 0x1000u);
    expect(root != 0 && g_root_uid == root, "first created thread is captured as root");
    uint32_t launcher = sched_create_thread(0x0029a174u, 32, 0x1000u);
    expect(launcher != 0 && g_launcher_uid == launcher,
           "launcher entry 0x0029a174 captures the launcher uid");
    TCB *lt = tcb_by_uid(launcher);
    expect(lt && g_master_reent == lt->k0_init + 0x10u,
           "launcher registration captures the master reent");
    /* Seed a recognizable master-reent word, then create a worker: workers clone the
     * master reent; root/launcher must not. */
    MEM_W32(g_master_reent + 0u, 0xABCD1234u);
    uint32_t worker = sched_create_thread(0x000468c8u, 36, 0x1000u);
    expect(worker != 0 && g_worker_uid == worker,
           "worker entry 0x000468c8 captures the worker uid");
    TCB *wt = tcb_by_uid(worker);
    expect(wt && MEM_R32(wt->k0_init + 0x10u) == 0xABCD1234u,
           "worker reent is cloned from the master reent");
    expect(wt && MEM_R32(wt->k0_init + 0x10u + 0x37cu) == worker,
           "worker uid survives the reent clone at state_ptr+0x37c");
    expect(sched_root_uid() == root && sched_launcher_uid() == launcher &&
           sched_worker_uid() == worker, "role accessors report the captured uids");
}

/* ---- stack-arena exhaustion tests -------------------------------------------------- */

static void test_stack_exhaustion_fails_create(void) {
    reset_sched();
    int n0 = s_ntcb;
    uint32_t top0 = s_stack_top;
    /* 256 MiB cannot fit under s_stack_top (arena floor 0x05000000): the create must
     * FAIL, not silently grant a clamped smaller stack. */
    uint32_t uid = sched_create_thread(0x2000u, 32, 0x10000000u);
    expect(uid == 0, "oversized stack request fails the create");
    expect(s_ntcb == n0, "failed create leaks no TCB slot");
    expect(s_stack_top == top0, "failed create consumes no stack arena");
    uint32_t ok = sched_create_thread(0x2000u, 32, 0x4000u);
    expect(ok != 0, "a fitting request still succeeds after a failed one");
    expect(s_stack_top < top0, "successful create consumes arena");
}

/* ---- sr_coro primitive tests ------------------------------------------------------- */

static int g_dead_body_ran;
static void dead_body(void *arg) {
    (void)arg;
    g_dead_body_ran++;
    /* returns: the trampoline must park this coroutine, not self-switch or spin */
}

static void test_coro_self_switch_and_park(void) {
    SrCoro *self = sr_coro_current();
    expect(self != NULL, "main coroutine adopted");
    sr_coro_switch(self);   /* must be a defined no-op, not a fiber self-switch */
    expect(1, "self-switch returned control to the caller");

    fprintf(stderr, "(expected: one 'sr_coro: FATAL: coroutine body returned' line follows)\n");
    g_dead_body_ran = 0;
    SrCoro *c = sr_coro_create(dead_body, NULL, (size_t)1 << 20);
    expect(c != NULL, "coroutine created");
    sr_coro_switch(c);      /* body runs, returns; trampoline parks and bounces back */
    expect(g_dead_body_ran == 1, "returned body ran exactly once");
    sr_coro_switch(c);      /* resuming the dead coroutine must bounce straight back */
    expect(g_dead_body_ran == 1, "resuming a dead coroutine does not re-run the body");
    sr_coro_destroy(c);
}

static void test_libc_thread_relocation(void) {
    reset_sched();
    
    // Seed guest RAM in the 0x0030a040..0x0030a0bf range with sentinel values
    uint32_t base = 0x0030a040u;
    for (int i = 0; i < 16; i++) {
        MEM_W32(base + i * 8, 0x11111111u * (i + 1));
        MEM_W32(base + i * 8 + 4, 0x22222222u * (i + 1));
    }

    // 1. Create threads (which triggers registration)
    // The first thread created is captured as root
    uint32_t root_uid = sched_create_thread(0x00001000u, 32, 0x1000u);
    expect(root_uid != 0, "root create succeeds");

    // The second thread is launcher
    uint32_t launcher_uid = sched_create_thread(0x0029a174u, 32, 0x1000u);
    expect(launcher_uid != 0, "launcher create succeeds");

    // The third thread is worker
    uint32_t worker_uid = sched_create_thread(0x000468c8u, 36, 0x1000u);
    expect(worker_uid != 0, "worker create succeeds");

    // Proves: Host libc-thread registration does not modify 0x0030a040..0x0030a0bf
    for (int i = 0; i < 16; i++) {
        expect(MEM_R32(base + i * 8) == 0x11111111u * (i + 1), "metadata range field0 unmodified by registration");
        expect(MEM_R32(base + i * 8 + 4) == 0x22222222u * (i + 1), "metadata range field1 unmodified by registration");
    }

    // Proves: Root/launcher/worker registration works under drifted UIDs, and multiple thread registrations do not consume guest metadata slots
    int registered_count = 0;
    for (int i = 0; i < MAXTHREADS; i++) {
        if (s_libc_threads[i].in_use) {
            registered_count++;
            uint32_t uid = s_libc_threads[i].uid;
            expect(uid == root_uid || uid == launcher_uid || uid == worker_uid, "registered thread has valid UID");
        }
    }
    expect(registered_count == 3, "three threads registered in host-owned memory");

    // Proves: Duplicate registration is deterministic
    TCB *wt = tcb_by_uid(worker_uid);
    expect(wt != NULL, "worker TCB found");
    register_libc_thread(wt->k0_init, wt->k0_init + 0x10, wt->uid);
    int new_registered_count = 0;
    for (int i = 0; i < MAXTHREADS; i++) {
        if (s_libc_threads[i].in_use) {
            new_registered_count++;
        }
    }
    expect(new_registered_count == 3, "duplicate registration does not create new slot");

    // Proves: Host registry records are cleaned/reused correctly (unregistration)
    unregister_libc_thread(wt->k0_init);
    int post_unreg_count = 0;
    for (int i = 0; i < MAXTHREADS; i++) {
        if (s_libc_threads[i].in_use) {
            post_unreg_count++;
        }
    }
    expect(post_unreg_count == 2, "unregistration decrements registered count");

    // Proves: Host libc-thread unregistration does not modify that guest region
    for (int i = 0; i < 16; i++) {
        expect(MEM_R32(base + i * 8) == 0x11111111u * (i + 1), "metadata range field0 unmodified by unregistration");
        expect(MEM_R32(base + i * 8 + 4) == 0x22222222u * (i + 1), "metadata range field1 unmodified by unregistration");
    }

    // Proves: Table exhaustion returns -1 (not abort) and the table stays at MAXTHREADS.
    // Fill the remaining slots until we get a -1 return, then attempt 5 more.
    // Exactly 2 slots remain free (root + launcher used 2, worker was unregistered above, so
    // it left 1 free, but we re-register at root+launcher+worker=3 total, then unregister
    // worker=2 left.  Actually we have MAXTHREADS-2 free after root+launcher). The exact
    // accounting doesn't matter: we fill until -1 then confirm table stays at MAXTHREADS.
    int over_count = 0;
    for (int i = 0; i < MAXTHREADS + 5; i++) {
        int rc = register_libc_thread(0xF0000000u + i * 0x1000u, 0xF0000010u + i * 0x1000u, 0x9999u);
        if (rc < 0) over_count++;
    }
    expect(over_count >= 1, "table exhaustion returns -1 at least once");
    int full_count = 0;
    for (int i = 0; i < MAXTHREADS; i++) {
        if (s_libc_threads[i].in_use) full_count++;
    }
    expect(full_count == MAXTHREADS, "libc threads array is exactly full at MAXTHREADS after overflow");
}

static struct {
    uint32_t entry;
    uint32_t common_arg;
    uint32_t notify_arg;
    int notify_count;
    int pending;
    uint32_t owner;
    int executed;       /* dispatch count, not just a flag -- proves same-pass re-dispatch */
    int used;            /* mirrors hle.c's s_callbacks[i].used; cleared on auto-delete */
    uint32_t ret_v0;      /* $v0 the stand-in dispatch body "returns" */
    int renotify_once;    /* if set, marks itself pending again the first time it runs
                            * (simulates a callback that calls NotifyCallback on itself) */
    int wake_owner_once;  /* if set, calls sched_thread_wakeup(owner) the first time it
                            * runs (simulates a wakeup delivered while the sleeping thread
                            * is dispatching callbacks -- state is not TH_WAIT_OBJ yet, so
                            * the wakeup banks into t->wakeups instead of taking the fast
                            * path). Exercises the banked-wakeup re-check in sleep_cb. */
} s_test_callbacks[2];

int sr_thread_has_pending_callbacks(uint32_t thread_uid) {
    for (int i = 0; i < 2; i++) {
        if (s_test_callbacks[i].used && s_test_callbacks[i].pending && s_test_callbacks[i].owner == thread_uid) {
            return 1;
        }
    }
    return 0;
}

/* Captures what a dispatched callback body actually observed, since sr_callback_dispatch_one
 * (recomp.h, shared with production hle.c) restores the interrupted thread's full context
 * once the callback returns -- inspecting the CpuState afterward can no longer show what
 * the callback itself saw. */
static struct {
    int captured;
    uint32_t a0, a1, a2, ra, pc;
} s_last_cb_seen;

/* Set by sr_thread_dispatch_callbacks right before each dispatch so capture_dispatch_fn can
 * look up that slot's configured $v0 / self-renotify behavior. Single-threaded/synchronous,
 * so there is never more than one dispatch in flight. */
static int s_dispatching_index = -1;

static void capture_dispatch_fn(CpuState *s, uint32_t target) {
    (void)target;
    s_last_cb_seen.captured = 1;
    s_last_cb_seen.a0 = s->r[4];
    s_last_cb_seen.a1 = s->r[5];
    s_last_cb_seen.a2 = s->r[6];
    s_last_cb_seen.ra = s->r[31];
    s_last_cb_seen.pc = s->pc;
    if (s_dispatching_index >= 0) {
        s->r[2] = s_test_callbacks[s_dispatching_index].ret_v0;
        if (s_test_callbacks[s_dispatching_index].renotify_once) {
            s_test_callbacks[s_dispatching_index].renotify_once = 0;
            s_test_callbacks[s_dispatching_index].pending = 1;
        }
        if (s_test_callbacks[s_dispatching_index].wake_owner_once) {
            s_test_callbacks[s_dispatching_index].wake_owner_once = 0;
            sched_thread_wakeup(s_test_callbacks[s_dispatching_index].owner);
        }
    }
}

/* Mirrors hle.c's sr_thread_dispatch_callbacks: re-scans the table until a full pass
 * dispatches nothing new (so self/earlier-slot re-notification is picked up in the same
 * pump, not deferred), and auto-deletes ("used = 0") any callback that returns non-zero,
 * per the PSP kernel rule. Same call-frame helper (sr_callback_dispatch_one) the production
 * dispatcher uses, so the ABI order and register-preservation contract asserted elsewhere
 * are the ones guest callbacks really see -- not a hand-copied re-implementation. */
int sr_thread_dispatch_callbacks(void) {
    uint32_t thread_uid = sched_current_uid();
    int total_dispatched = 0;
    for (;;) {
        int dispatched_any = 0;
        for (int i = 0; i < 2; i++) {
            if (!(s_test_callbacks[i].used && s_test_callbacks[i].pending && s_test_callbacks[i].owner == thread_uid)) {
                continue;
            }
            dispatched_any = 1;
            total_dispatched++;
            s_test_callbacks[i].pending = 0;
            s_test_callbacks[i].executed++;
            s_dispatching_index = i;
            uint32_t ret = sr_callback_dispatch_one(s_cpu, s_test_callbacks[i].entry,
                                     s_test_callbacks[i].notify_count,
                                     s_test_callbacks[i].notify_arg,
                                     s_test_callbacks[i].common_arg,
                                     capture_dispatch_fn);
            s_dispatching_index = -1;
            if (ret != 0) {
                s_test_callbacks[i].used = 0;
            }
        }
        if (!dispatched_any) break;
    }
    return total_dispatched;
}

static void cb_sleep_test_body(CpuState *s) {
    (void)s;
    sched_thread_sleep_cb();
}

static void reset_libc_threads(void) {
    for (int i = 0; i < MAXTHREADS; i++) {
        s_libc_threads[i].in_use = 0;
    }
}

/* Direct regression guard on the production argument-packing helper (recomp.h),
 * independent of the scheduler plumbing above. */
static void test_callback_abi_packing(void) {
    CpuState cpu;
    memset(&cpu, 0xff, sizeof(cpu));
    sr_callback_pack_args(&cpu, 3, 0x12345678u, 0x9abcdef0u);
    expect(cpu.r[4] == 3u, "pack_args: $a0 is notify_count");
    expect(cpu.r[5] == 0x12345678u, "pack_args: $a1 is notify_arg");
    expect(cpu.r[6] == 0x9abcdef0u, "pack_args: $a2 is common_arg");
}

static struct {
    int invoked;
    CpuState seen;
    uint32_t target_arg;
} s_ctx_test_dispatch;

/* Stand-in "guest callback body": records the full context it was invoked with, then
 * clobbers a representative sample of callee-saved/HI-LO/FPU/VFPU state and returns a
 * nonzero $v0 -- exactly what a (possibly buggy) real callback might do -- so the test
 * below can prove sr_callback_dispatch_one restores the interrupted thread's context
 * regardless of what the callback did. */
static void context_check_dispatch_fn(CpuState *s, uint32_t target) {
    s_ctx_test_dispatch.invoked = 1;
    s_ctx_test_dispatch.seen = *s;
    s_ctx_test_dispatch.target_arg = target;
    s->r[2] = 1u;             /* $v0 return value */
    s->r[16] = 0xdeadbeefu;   /* $s0 */
    s->hi = 0u; s->lo = 0u;
    s->f[0] = 0.0f;
    s->vi[0] = 0u;
}

/* Production-facing regression for sr_callback_dispatch_one (recomp.h) -- the exact
 * function hle.c's sr_thread_dispatch_callbacks() calls, not a re-implementation. Proves
 * the fix for the "callback dispatch zeroes the entire CpuState" bug: a callback must run
 * as a nested call on the owning thread's LIVE register file (only args/$ra/$pc set), and
 * the interrupted context must be fully restored once the callback returns. */
static void test_callback_dispatch_one_preserves_context(void) {
    CpuState cpu;
    for (int i = 0; i < 32; i++) cpu.r[i] = 0x1000u + (uint32_t)i;
    cpu.hi = 0x2001u; cpu.lo = 0x2002u;
    cpu.pc = 0x3000u;
    for (int i = 0; i < 32; i++) cpu.fi[i] = 0x4000u + (uint32_t)i;
    cpu.fcr31 = 0x5001u; cpu.fpcond = 0x5002u;
    for (int i = 0; i < 128; i++) cpu.vi[i] = 0x6000u + (uint32_t)i;
    for (int i = 0; i < 16; i++) cpu.vfpuCtrl[i] = 0x7000u + (uint32_t)i;
    cpu.status = 0x8001u; cpu.next_pc = 0x8002u; cpu.in_delay_slot = 0x8003u;

    CpuState pre = cpu;
    memset(&s_ctx_test_dispatch, 0, sizeof(s_ctx_test_dispatch));

    sr_callback_dispatch_one(&cpu, 0x08900000u, 7, 0xaabbccddu, 0x11223344u,
                             context_check_dispatch_fn);

    expect(s_ctx_test_dispatch.invoked, "dispatch_fn was invoked");
    expect(s_ctx_test_dispatch.target_arg == 0x08900000u, "dispatch_fn received the callback entry as target");

    /* What the callback body saw while running: args packed per the PSP ABI, $ra is the
     * return sentinel, $pc is the entry -- and every non-argument register is exactly
     * what the interrupted thread had, proving the call frame is NOT zeroed. */
    CpuState *seen = &s_ctx_test_dispatch.seen;
    expect(seen->r[4] == 7u, "callback saw $a0 = notify_count");
    expect(seen->r[5] == 0xaabbccddu, "callback saw $a1 = notify_arg");
    expect(seen->r[6] == 0x11223344u, "callback saw $a2 = common_arg");
    expect(seen->r[31] == 0u, "callback saw $ra = 0 (return sentinel)");
    expect(seen->pc == 0x08900000u, "callback saw $pc = entry");
    expect(seen->r[28] == pre.r[28], "callback inherited the interrupted thread's live $gp ($28)");
    expect(seen->r[26] == pre.r[26], "callback saw the interrupted thread's live k0 ($26)");
    expect(seen->r[16] == pre.r[16], "callback saw the interrupted thread's live $s0 ($16)");
    expect(seen->r[23] == pre.r[23], "callback saw the interrupted thread's live $s7 ($23)");
    expect(seen->r[30] == pre.r[30], "callback saw the interrupted thread's live $fp/$s8 ($30)");
    expect(seen->hi == pre.hi && seen->lo == pre.lo, "callback saw the interrupted thread's live HI/LO");
    expect(seen->fi[0] == pre.fi[0], "callback saw the interrupted thread's live FPU register file");
    expect(seen->fcr31 == pre.fcr31, "callback saw the interrupted thread's live FCR31");
    expect(seen->vi[0] == pre.vi[0], "callback saw the interrupted thread's live VFPU register file");
    expect(seen->vfpuCtrl[0] == pre.vfpuCtrl[0], "callback saw the interrupted thread's live VFPU control state");

    /* Once the callback returns, the interrupted thread's full context is restored --
     * including the return value ($v0), $s0, HI/LO, FPU and VFPU state the callback body
     * (intentionally, in this test) clobbered. */
    expect(memcmp(&cpu, &pre, sizeof(CpuState)) == 0,
           "cpu context after sr_callback_dispatch_one matches the pre-call snapshot exactly");
}

static void test_callbacks_behavioral(void) {
    reset_libc_threads();
    memset(s_test_callbacks, 0, sizeof(s_test_callbacks));
    s_test_callbacks[0].entry = 0x8888u;
    s_test_callbacks[0].common_arg = 0xaaaa1111u;
    
    s_test_uid_next = 0x200u;
    uint32_t tid = sched_create_thread(0x1000u, 16, 0x10000u);
    TCB *t = tcb_by_uid(tid);
    expect(t != NULL, "created callback sleep test thread");
    
    sched_start_thread(tid, 0, 0);
    g_test_body = cb_sleep_test_body;
    int idx = index_of_uid(tid);
    run_one_slice(idx);
    
    expect(t->state == TH_WAIT_OBJ, "thread is blocked on SleepCB");
    expect(t->sleeping == 1, "thread is marked sleeping");
    expect(t->is_cb_wait == 1, "thread has is_cb_wait set while in SleepCB");

    /* Snapshot of the thread's context before any callback ever ran, so we can prove
     * sr_callback_dispatch_one restores it exactly once the callback returns. */
    CpuState pre_cb_saved = t->saved;

    s_test_callbacks[0].owner = tid;
    s_test_callbacks[0].used = 1;
    s_test_callbacks[0].pending = 1;
    s_test_callbacks[0].notify_arg = 0xbbbb2222u;
    s_test_callbacks[0].notify_count = 1;
    memset(&s_last_cb_seen, 0, sizeof(s_last_cb_seen));

    sched_wake_callbacks(tid);
    expect(t->state == TH_READY, "thread is READY after wake_callbacks");
    expect(t->wake == s_vtime_us, "thread wake timer reset to current virtual time");

    run_one_slice(idx);
    expect(s_test_callbacks[0].executed == 1, "callback function executed");
    expect(t->state == TH_WAIT_OBJ, "thread goes back to sleep after callback dispatch");

    /* What the callback body itself observed while running (captured inside
     * capture_dispatch_fn, before sr_callback_dispatch_one restores the context). */
    expect(s_last_cb_seen.captured, "callback dispatch_fn was invoked");
    expect(s_last_cb_seen.a0 == 1u, "callback saw $a0 = notify_count");
    expect(s_last_cb_seen.a1 == 0xbbbb2222u, "callback saw $a1 = notify_arg");
    expect(s_last_cb_seen.a2 == 0xaaaa1111u, "callback saw $a2 = common_arg");
    expect(s_last_cb_seen.ra == 0u, "callback saw $ra = 0 (return sentinel)");
    expect(s_last_cb_seen.pc == s_test_callbacks[0].entry, "callback saw $pc = entry");

    /* Once the callback returns, the interrupted thread's context is restored exactly --
     * the notify/common args do NOT leak into the resumed thread's register state. */
    expect(memcmp(&t->saved, &pre_cb_saved, sizeof(CpuState)) == 0,
           "thread context after callback dispatch matches pre-dispatch snapshot exactly");

    sched_thread_wakeup(tid);
    expect(t->state == TH_READY, "thread is ready after wakeup");
    
    run_one_slice(idx);
    expect(t->state == TH_DORMANT, "thread finished cleanly");
    
    sched_terminate_thread(tid);
}

static void test_callback_renotifies_itself_dispatched_same_pass(void) {
    reset_libc_threads();
    memset(s_test_callbacks, 0, sizeof(s_test_callbacks));
    s_test_callbacks[0].entry = 0x9999u;
    s_test_callbacks[0].renotify_once = 1;

    s_test_uid_next = 0x400u;
    uint32_t tid = sched_create_thread(0x1000u, 16, 0x10000u);
    TCB *t = tcb_by_uid(tid);
    expect(t != NULL, "created renotify test thread");

    sched_start_thread(tid, 0, 0);
    g_test_body = cb_sleep_test_body;
    int idx = index_of_uid(tid);
    run_one_slice(idx);
    expect(t->state == TH_WAIT_OBJ, "thread is blocked on SleepCB");

    s_test_callbacks[0].owner = tid;
    s_test_callbacks[0].used = 1;
    s_test_callbacks[0].pending = 1;

    sched_wake_callbacks(tid);
    run_one_slice(idx);

    /* A single external wake pumped BOTH the original notification and the callback's
     * own self-renotification, within the same sr_thread_dispatch_callbacks() pass loop --
     * proving re-notification during dispatch isn't deferred to a later external check. */
    expect(s_test_callbacks[0].executed == 2,
           "self-renotifying callback is dispatched twice within one dispatch pump");
    expect(t->state == TH_WAIT_OBJ, "thread goes back to sleep once no callback remains pending");

    sched_thread_wakeup(tid);
    run_one_slice(idx);
    expect(t->state == TH_DORMANT, "thread finished cleanly");
    sched_terminate_thread(tid);
}

static void test_callback_auto_deletes_on_nonzero_return(void) {
    reset_libc_threads();
    memset(s_test_callbacks, 0, sizeof(s_test_callbacks));
    s_test_callbacks[0].entry = 0xaaaa0u;
    s_test_callbacks[0].ret_v0 = 1u;   /* nonzero return -> auto-delete */
    s_test_callbacks[1].entry = 0xbbbb0u;
    s_test_callbacks[1].ret_v0 = 0u;   /* zero return -> stays registered */

    s_test_uid_next = 0x500u;
    uint32_t tid = sched_create_thread(0x1000u, 16, 0x10000u);
    TCB *t = tcb_by_uid(tid);
    expect(t != NULL, "created auto-delete test thread");

    sched_start_thread(tid, 0, 0);
    g_test_body = cb_sleep_test_body;
    int idx = index_of_uid(tid);
    run_one_slice(idx);
    expect(t->state == TH_WAIT_OBJ, "thread is blocked on SleepCB");

    s_test_callbacks[0].owner = tid;
    s_test_callbacks[0].used = 1;
    s_test_callbacks[0].pending = 1;
    s_test_callbacks[1].owner = tid;
    s_test_callbacks[1].used = 1;
    s_test_callbacks[1].pending = 1;

    sched_wake_callbacks(tid);
    run_one_slice(idx);

    expect(s_test_callbacks[0].executed == 1, "nonzero-return callback ran once");
    expect(s_test_callbacks[0].used == 0,
           "callback that returned non-zero is auto-deleted (used cleared)");
    expect(s_test_callbacks[1].executed == 1, "zero-return callback ran once");
    expect(s_test_callbacks[1].used == 1,
           "callback that returned zero stays registered (not deleted)");

    sched_thread_wakeup(tid);
    run_one_slice(idx);
    expect(t->state == TH_DORMANT, "thread finished cleanly");
    sched_terminate_thread(tid);
}

/* The internal callback pump returns an invocation count so wait implementations can
 * tell whether work ran. sceKernelCheckCallback itself deliberately collapses this to
 * the PSP hardware's Boolean 0/1 ABI. Exercise the internal count directly: two pending
 * callbacks, one self-renotifying once, must report 3 total dispatches, then 0. */
static void test_dispatch_callbacks_returns_total_count(void) {
    reset_sched();
    memset(s_test_callbacks, 0, sizeof(s_test_callbacks));
    int idx = mk(0x600u, TH_RUNNING, 20);
    s_cur = idx;

    s_test_callbacks[0].entry = 0x1111u;
    s_test_callbacks[0].owner = s_tcb[idx].uid;
    s_test_callbacks[0].used = 1;
    s_test_callbacks[0].pending = 1;
    s_test_callbacks[0].renotify_once = 1;   /* dispatched twice within the pump */

    s_test_callbacks[1].entry = 0x2222u;
    s_test_callbacks[1].owner = s_tcb[idx].uid;
    s_test_callbacks[1].used = 1;
    s_test_callbacks[1].pending = 1;

    int total = sr_thread_dispatch_callbacks();
    expect(total == 3,
           "dispatch pump returns the TOTAL invocation count (cb0 fires twice via "
           "self-renotify, cb1 once), not a 0/1 boolean");
    expect(sr_thread_dispatch_callbacks() == 0, "a pump with nothing pending returns 0");

    s_cur = -1;
}

/* Regression: a wakeup delivered while a SleepCB thread is dispatching callbacks
 * must not be lost. During dispatch the thread has sleeping=1 but state!=TH_WAIT_OBJ,
 * so sched_thread_wakeup banks it (wakeups++) rather than readying the thread. The
 * SleepCB loop must consume that banked wakeup on its next iteration instead of
 * blocking on it forever. */
static void test_sleepcb_consumes_wakeup_banked_during_dispatch(void) {
    reset_libc_threads();
    memset(s_test_callbacks, 0, sizeof(s_test_callbacks));
    s_test_callbacks[0].entry = 0x9999u;

    s_test_uid_next = 0x200u;
    uint32_t tid = sched_create_thread(0x1000u, 16, 0x10000u);
    TCB *t = tcb_by_uid(tid);
    expect(t != NULL, "created banked-wakeup test thread");

    sched_start_thread(tid, 0, 0);
    g_test_body = cb_sleep_test_body;
    int idx = index_of_uid(tid);
    run_one_slice(idx);
    expect(t->state == TH_WAIT_OBJ, "thread blocked on SleepCB before any callback");

    /* A callback that wakes its own owner while being dispatched. */
    s_test_callbacks[0].owner = tid;
    s_test_callbacks[0].used = 1;
    s_test_callbacks[0].pending = 1;
    s_test_callbacks[0].notify_count = 1;
    s_test_callbacks[0].wake_owner_once = 1;

    sched_wake_callbacks(tid);
    expect(t->state == TH_READY, "thread READY to run its pending callback");

    run_one_slice(idx);
    expect(s_test_callbacks[0].executed == 1, "callback ran");
    /* The wakeup the callback banked mid-dispatch must have been consumed, so the
     * thread ran to completion in this slice rather than re-blocking on SleepCB. */
    expect(t->wakeups == 0, "banked wakeup was consumed, not stranded");
    expect(t->state == TH_DORMANT,
           "thread finished after consuming the banked wakeup (not stuck in SleepCB)");

    sched_terminate_thread(tid);
}

static void non_cb_sleep_test_body(CpuState *s) {
    (void)s;
    sched_thread_sleep();
}

static void test_wake_callbacks_no_op_on_non_cb_wait(void) {
    reset_libc_threads();
    s_test_uid_next = 0x300u;
    uint32_t tid = sched_create_thread(0x2000u, 16, 0x10000u);
    TCB *t = tcb_by_uid(tid);
    expect(t != NULL, "created non-callback sleep test thread");
    
    sched_start_thread(tid, 0, 0);
    g_test_body = non_cb_sleep_test_body;
    int idx = index_of_uid(tid);
    run_one_slice(idx);
    
    expect(t->state == TH_WAIT_OBJ, "thread is blocked on normal Sleep");
    expect(t->is_cb_wait == 0, "thread does not have is_cb_wait set");
    
    sched_wake_callbacks(tid);
    expect(t->state == TH_WAIT_OBJ, "thread remains blocked since it is not in CB-wait");
    
    sched_thread_wakeup(tid);
    expect(t->state == TH_READY, "thread is ready after normal wakeup");
    
    run_one_slice(idx);
    expect(t->state == TH_DORMANT, "thread finished cleanly");
    
    sched_terminate_thread(tid);
}

/* PSP clock reads are observations of scheduler time, not scheduler turns.  In
 * particular, polling a clock from a busy loop must not manufacture elapsed
 * guest time just because the host made another HLE call. */
static void test_clock_reads_are_observational(void) {
    reset_sched();
    s_pace_on = 0;                 /* deterministic/turbo fixture, no host clock */
    s_vtime_us = 424242u;
    uint64_t before = s_vtime_us;
    for (int i = 0; i < 32; i++) {
        (void)sched_vtime_us();
        sched_vtime_refresh();
    }
    expect(s_vtime_us == before,
           "repeated scheduler clock reads do not advance guest time");

    s_pending_interrupts = 0;
    s_vbl_next_us = 0;
    s_vtime_us = UINT64_MAX - 2u;
    expect(sched_vtime_deadline_after(3u) == UINT64_MAX,
           "scheduler deadline arithmetic saturates instead of wrapping");
    s_vtime_us = UINT64_MAX;
    scheduler_latch_due_events();
    expect((s_pending_interrupts & SCHED_INTR_VBLANK) != 0u &&
           s_vbl_next_us == UINT64_MAX,
           "monotonic timeline saturates and coalesces a long overdue VBLANK");
}

/* Interrupt suspension freezes dispatch, not the monotonic timeline.  Missed
 * VBLANKs coalesce into one source bit; an unrelated pending source remains
 * latched until its own handler is implemented.  Restoring the outer token
 * services the eligible work and leaves the interrupted low-priority thread
 * ready for the normal post-interrupt selection. */
static void test_pending_interrupts_progress_and_resume(void) {
    reset_sched();
    s_pace_on = 0;
    int low = mk(0x210u, TH_RUNNING, 40);
    int waiter = mk(0x211u, TH_WAIT_OBJ, 10);
    s_tcb[waiter].wait_obj = VBLANK_WAIT_OBJ;
    s_tcb[waiter].wake = (uint64_t)-1;
    s_cur = low;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));

    uint32_t outer = sched_suspend_interrupts();
    expect(outer == 1u, "outer SuspendIntr returns the enabled token");
    uint64_t before = s_vtime_us;
    sr_yield(&g_cpu_store);
    expect(s_vtime_us > before,
           "scheduler time advances while interrupts are disabled");
    expect((sched_pending_interrupts() & SCHED_INTR_VBLANK) != 0u,
           "elapsed VBLANK becomes pending while interrupts are disabled");
    expect(g_test_vblank_delivered == 0u,
           "disabled interrupts defer VBLANK delivery");
    expect(s_tcb[waiter].state == TH_WAIT_OBJ,
           "a VBLANK waiter remains blocked until delivery");

    uint32_t inner = sched_suspend_interrupts();
    expect(inner == 0u, "nested SuspendIntr returns the disabled token");
    sched_raise_interrupt(SCHED_INTR_GE);
    sched_resume_interrupts(inner);
    expect(!sched_interrupts_enabled(),
           "restoring token 0 keeps interrupts disabled");
    expect((sched_pending_interrupts() & SCHED_INTR_GE) != 0u,
           "an unrelated pending source survives a disabled restore");
    expect(g_test_vblank_delivered == 0u,
           "token-0 restore does not deliver pending work");

    sched_resume_interrupts(0xDEADBEEFu);
    expect(!sched_interrupts_enabled(),
           "an invalid resume token cannot re-enable suspended interrupts");

    sched_resume_interrupts(outer);
    expect(sched_interrupts_enabled(),
           "restoring the outer token re-enables interrupts");
    expect(g_test_vblank_delivered == 1u,
           "resume delivers one coalesced pending VBLANK");
    expect((sched_pending_interrupts() & SCHED_INTR_VBLANK) == 0u,
           "delivered VBLANK is removed from the pending set");
    expect((sched_pending_interrupts() & SCHED_INTR_GE) != 0u,
           "unhandled interrupt source remains pending after VBLANK service");
    expect(s_tcb[waiter].state == TH_READY,
           "resume delivery readies the VBLANK waiter");
    expect(s_tcb[low].state == TH_READY,
           "resume requests a post-interrupt reschedule for a higher-priority waiter");

    sched_resume_interrupts(inner);
    expect(!sched_interrupts_enabled(),
           "a later inner token can disable interrupts again");
    expect(g_test_vblank_delivered == 1u,
           "re-disabling after service does not redeliver the same VBLANK");
}

/* The VBLANK entry is a nested interrupt frame.  The handler receives only the
 * hardware-owned entry state, while the interrupted CpuState is restored even
 * when the handler mutates registers or raises another source. */
static void test_interrupt_frame_is_restored(void) {
    reset_sched();
    s_pace_on = 0;
    s_vbl_next_us = UINT64_MAX;
    int running = mk(0x220u, TH_RUNNING, 20);
    s_cur = running;
    g_test_vblank_handler = 0x00001234u;
    g_test_handler_raise_ge = 1;
    g_cpu_store.pc = 0x11112222u;
    g_cpu_store.r[16] = 0x33334444u;
    g_cpu_store.r[28] = 0x55556666u;
    g_cpu_store.r[29] = 0x77778888u;
    g_cpu_store.r[31] = 0x9999aaaau;
    g_cpu_store.vfpuCtrl[0] = 0x12345678u;
    CpuState interrupted;
    memcpy(&interrupted, &g_cpu_store, sizeof(interrupted));

    sched_raise_interrupt(SCHED_INTR_VBLANK);
    sched_resume_interrupts(1u);
    expect(g_test_handler_calls == 1u,
           "eligible VBLANK invokes the registered handler once");
    expect(g_test_handler_seen.pc == g_test_vblank_handler &&
           g_test_handler_seen.r[16] == interrupted.r[16] &&
           g_test_handler_seen.r[29] == 0x09df0000u &&
           g_test_handler_seen.r[31] == 0u,
           "handler sees the preserved interrupted frame plus kernel entry state");
    expect(memcmp(&g_cpu_store, &interrupted, sizeof(interrupted)) == 0,
           "handler register mutations do not leak into the interrupted thread");
    expect((sched_pending_interrupts() & SCHED_INTR_GE) != 0u,
           "a source raised by the handler remains pending for its own service path");
}

/* #70 slice A -- paced mode owns its clock, and the owner is the host.
 *
 * With vblank pacing ON (the production profile) guest virtual time is a SAMPLE of the
 * host monotonic clock: s_vtime_us may catch up to host time, never overtake it. The
 * yield path's "nobody else is runnable" branch used to advance the virtual clock to
 * the soonest future waiter deadline in BOTH modes. In paced mode that manufactures
 * guest time out of nothing -- a busy-waiting thread with a distant sleeper beside it
 * pushes s_vtime_us (and every delay, timed wait and VBLANK deadline derived from it)
 * ahead of real time, which is the guest-runs-fast half of the measured #70 drift.
 *
 * Fixture: host monotonic time is pinned well below a future waiter's deadline, the
 * waiter exists, and the scheduler is driven repeatedly. The asserted invariant is
 * ownership, not a rate: virtual time never exceeds the sampled host time merely
 * because a waiter would like it to. */
static void test_paced_vtime_is_host_anchored(void) {
    reset_sched();
    begin_clock_fixture(1, 0u);
    disable_vblank_sources();     /* slice A is about the clock, not VBLANK production */

    int spinner = mk(0x230u, TH_RUNNING, 40);
    int sleeper = mk(0x231u, TH_WAIT_DELAY, 40);
    s_tcb[sleeper].wake = 5000000u;          /* 5 s of guest time in the future */
    s_cur = spinner;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));

    set_host_us(1000u);                      /* host has elapsed 1 ms, nothing like 5 s */
    int outran = 0;
    for (int i = 0; i < 8; i++) {
        s_tcb[spinner].state = TH_RUNNING;   /* the spinner keeps being re-scheduled */
        s_cur = spinner;
        sr_yield(&g_cpu_store);
        if (s_vtime_us > 1000u) outran = 1;
    }
    expect(!outran,
           "paced guest virtual time never advances beyond the sampled host clock");
    expect(s_vtime_us == 1000u,
           "paced guest virtual time tracks the sampled host clock exactly");
    expect(s_tcb[sleeper].state == TH_WAIT_DELAY,
           "a future timed waiter is not woken early by manufactured guest time");

    /* Host time advancing is what moves the guest clock -- and it still reaches the
     * deadline, so the waiter is not starved, just no longer early. */
    set_host_us(5000000u);
    s_tcb[spinner].state = TH_RUNNING;
    s_cur = spinner;
    sr_yield(&g_cpu_store);
    expect(s_vtime_us == 5000000u,
           "paced guest virtual time follows the host clock forward");
    (void)pick_next();
    expect(s_tcb[sleeper].state == TH_READY,
           "the timed waiter becomes runnable once host time reaches its deadline");

    /* Turbo (SR_NOVBPACE=1) has no host anchor: jumping the virtual clock over an idle
     * wait is its architecturally intended behaviour and must survive the fix. */
    reset_sched();
    begin_clock_fixture(0, 0u);
    disable_vblank_sources();
    int tspin = mk(0x232u, TH_RUNNING, 40);
    int tsleep = mk(0x233u, TH_WAIT_DELAY, 40);
    s_tcb[tsleep].wake = 5000000u;
    s_cur = tspin;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));
    sr_yield(&g_cpu_store);
    expect(s_vtime_us >= 5000000u,
           "turbo mode still jumps virtual time over an idle wait");
    s_host_ns_fn = NULL;
}

/* Independent restatement of the PSP display rate. 59.94 Hz is exactly 60000/1001 Hz,
 * so the k-th VBLANK deadline after an origin delivery is floor(k * 1001000 / 60) us.
 * Derived from the rate itself rather than from sched.c's carry loop, so a drift in
 * that loop shows up here as a mismatch instead of agreeing with itself. */
static uint64_t rational_vblanks_through(uint64_t host_us_now) {
    uint64_t n = 0;
    for (uint64_t k = 0; (k * 1001000ull) / 60ull <= host_us_now; k++) n++;
    return n;
}

/* #70 slice B -- exactly one VBLANK authority in paced mode.
 *
 * Paced mode had two independent producers of a guest VBLANK:
 *
 *   1. the scheduler's exact rational source -- s_vbl_next_us, advanced in
 *      60000/1001 Hz steps by scheduler_latch_due_events();
 *   2. sr_vblank_quantum_due(), a host-wall-clock watchdog that raised
 *      SCHED_INTR_VBLANK directly once SR_VBLANK_Q_US (default 16000 us) had passed
 *      since the last DELIVERY -- without advancing the source deadline.
 *
 * The two boundaries are 683 us apart, so producer 2 fires first every frame and
 * producer 1 then fires anyway: two guest VBLANKs per 16.683 ms period, i.e. structural
 * over-delivery, and vcount/latch/waiter wakeups landing at a cadence the display
 * timeline never agreed to.
 *
 * The fixture walks the host clock across both boundaries with a VBLANK waiter parked,
 * then sweeps 200 ms and compares the delivered sequence against the rate restated
 * independently above. */
static void test_paced_vblank_has_one_authority(void) {
    reset_sched();
    begin_clock_fixture(1, 0u);
    uint64_t vbl0 = s_vbl_count;

    int spinner = mk(0x240u, TH_RUNNING, 40);
    int waiter  = mk(0x241u, TH_WAIT_OBJ, 40);
    s_tcb[waiter].wait_obj = VBLANK_WAIT_OBJ;
    s_tcb[waiter].wake = (uint64_t)-1;
    s_cur = spinner;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));

    /* Host t = 0: the source deadline sits on the origin, so the first yield
     * establishes the phase and resets the watchdog's reference point. */
    sr_yield(&g_cpu_store);
    expect(s_vbl_count - vbl0 == 1u, "the rational source delivers the origin VBLANK");
    expect(s_vbl_next_us == 16683u,
           "the source deadline advances one exact rational period");
    s_tcb[waiter].state = TH_WAIT_OBJ;      /* re-park for the boundary comparison */
    s_tcb[waiter].wait_obj = VBLANK_WAIT_OBJ;
    s_tcb[waiter].wake = (uint64_t)-1;

    /* Host t = 16.000 ms: the safety quantum is due, the rational deadline is not. */
    set_host_us(16000u);
    s_tcb[spinner].state = TH_RUNNING; s_cur = spinner;
    expect(sr_vblank_quantum_due(),
           "the host safety quantum is due at the 16.000 ms boundary");
    uint64_t late0 = s_vblank_late_service_yields;
    sr_yield(&g_cpu_store);
    expect(s_vblank_late_service_yields > late0,
           "a still-due paced quantum is counted as late service, not raised as a source");
    expect(s_vbl_count - vbl0 == 1u,
           "the safety quantum does not produce a VBLANK ahead of the rational deadline");
    expect(g_test_vblank_delivered == 1u,
           "GetVcount does not tick at the safety boundary");
    expect(s_tcb[waiter].state == TH_WAIT_OBJ,
           "a VBLANK waiter is not woken at the safety boundary");
    expect(s_vbl_next_us == 16683u,
           "the source deadline is unchanged by the safety quantum");

    /* Host t = 16.683 ms: the rational boundary. Exactly one more event. */
    set_host_us(16683u);
    s_tcb[spinner].state = TH_RUNNING; s_cur = spinner;
    sr_yield(&g_cpu_store);
    expect(s_vbl_count - vbl0 == 2u,
           "the rational deadline delivers exactly one VBLANK");
    expect(g_test_vblank_delivered == 2u, "GetVcount ticks once per delivered VBLANK");
    expect(s_tcb[waiter].state == TH_READY,
           "the VBLANK waiter wakes on the rational boundary");
    expect(s_vbl_next_us == 33366u,
           "the source deadline advances to the next exact rational period");

    /* Sweep 200 ms in 1 ms host steps and compare the delivered sequence with the
     * 60000/1001 schedule restated independently of sched.c. */
    reset_sched();
    begin_clock_fixture(1, 0u);
    vbl0 = s_vbl_count;
    spinner = mk(0x242u, TH_RUNNING, 40);
    s_cur = spinner;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));
    int sequence_ok = 1;
    for (uint64_t t_us = 0; t_us <= 200000u; t_us += 1000u) {
        set_host_us(t_us);
        s_tcb[spinner].state = TH_RUNNING; s_cur = spinner;
        sr_yield(&g_cpu_store);
        if (s_vbl_count - vbl0 != rational_vblanks_through(t_us)) sequence_ok = 0;
    }
    expect(sequence_ok,
           "paced VBLANK delivery follows the 60000/1001 schedule at every step");
    expect(s_vbl_count - vbl0 == 12u,
           "200 ms of host time delivers exactly 12 paced VBLANKs (origin + 11)");

    /* Coalescing is intentional and survives: a host jump across many periods
     * advances the source timeline past all of them but delivers ONE event -- the
     * pending set is a bit, not a queue. */
    reset_sched();
    begin_clock_fixture(1, 0u);
    vbl0 = s_vbl_count;
    spinner = mk(0x243u, TH_RUNNING, 40);
    s_cur = spinner;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));
    sr_yield(&g_cpu_store);                 /* origin delivery */
    set_host_us(1000000u);                  /* one host second later, no service between */
    s_tcb[spinner].state = TH_RUNNING; s_cur = spinner;
    sr_yield(&g_cpu_store);
    expect(s_vbl_count - vbl0 == 2u,
           "a multi-period host jump coalesces into one delivered VBLANK");
    expect(s_vbl_next_us > 1000000u && s_vbl_next_us <= 1016683u,
           "coalescing still advances the source timeline past every missed period");
    s_host_ns_fn = NULL;
}

/* #70 slice C -- an expired timed wait enters strict-priority scheduling.
 *
 * PSP scheduling is strict-priority preemptive, and a timed wait whose deadline has
 * passed is a RUNNABLE thread. Expiry was only ever noticed inside pick_next(), which
 * runs on the scheduler coroutine; sched_preempt() -- the check that actually takes the
 * CPU away from a running thread at an eligible boundary -- scanned only threads
 * already marked TH_READY. A priority-16 thread whose delay came due therefore did not
 * displace a priority-40 runner: it waited for that runner to yield or block, which for
 * a busy-wait loop can be an unbounded amount of real time. Waking late is the same
 * defect class as the clock drift in slices A and B, one level up.
 *
 * "Immediate" here means the next eligible scheduler/interrupt boundary, not
 * asynchronous mid-instruction switching, and interrupt-disabled / dispatch-disabled
 * deferral still holds. Both are asserted below. */
static void test_expired_timed_wait_enters_strict_priority(void) {
    reset_sched();
    begin_clock_fixture(1, 0u);
    disable_vblank_sources();

    int runner = mk(0x250u, TH_RUNNING, 40);
    int waiter = mk(0x251u, TH_WAIT_DELAY, 16);
    s_tcb[waiter].wake = 1000u;
    s_cur = runner;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));

    /* Before the deadline the boundary changes nothing. */
    set_host_us(500u);
    sched_resume_interrupts(1u);
    expect(s_tcb[waiter].state == TH_WAIT_DELAY,
           "a timed wait that is not yet due is not promoted");
    expect(s_tcb[runner].state == TH_RUNNING,
           "the running thread keeps the CPU before the waiter's deadline");

    /* The deadline comes due; the next eligible boundary hands the CPU over. */
    set_host_us(1000u);
    sched_resume_interrupts(1u);
    expect(s_tcb[waiter].state == TH_READY,
           "an expired timed wait becomes runnable at an eligible scheduler boundary");
    expect(s_tcb[runner].state == TH_READY,
           "the lower-priority runner is preempted rather than left RUNNING");
    expect(pick_next() == waiter,
           "the expired higher-priority waiter wins strict-priority selection");

    /* Deferral: an ineligible boundary promotes nothing and switches nothing, and the
     * work happens at the next eligible one instead. */
    reset_sched();
    begin_clock_fixture(1, 2000u);
    disable_vblank_sources();
    runner = mk(0x252u, TH_RUNNING, 40);
    waiter = mk(0x253u, TH_WAIT_DELAY, 16);
    s_tcb[waiter].wake = 1000u;
    s_vtime_us = 2000u;                       /* already past the deadline */
    s_cur = runner;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));

    (void)sched_suspend_interrupts();
    sched_preempt();
    expect(s_tcb[waiter].state == TH_WAIT_DELAY,
           "an interrupt-disabled context defers the expired waiter");
    expect(s_tcb[runner].state == TH_RUNNING,
           "an interrupt-disabled context does not switch away from the runner");

    s_interrupts_enabled = 1;
    (void)sched_suspend_dispatch();
    sched_preempt();
    expect(s_tcb[waiter].state == TH_WAIT_DELAY,
           "a dispatch-disabled context defers the expired waiter");
    expect(s_tcb[runner].state == TH_RUNNING,
           "a dispatch-disabled context does not switch away from the runner");

    (void)sched_resume_dispatch(1u);          /* the next eligible boundary */
    expect(s_tcb[waiter].state == TH_READY,
           "restoring dispatch runs the deferred promotion");
    expect(s_tcb[runner].state == TH_READY,
           "restoring dispatch runs the deferred preemption");

    /* Strict priority is not weakened in the other direction: an expired waiter that is
     * numerically weaker, or equal, does not take the CPU from the running thread. */
    reset_sched();
    begin_clock_fixture(1, 2000u);
    disable_vblank_sources();
    runner = mk(0x254u, TH_RUNNING, 16);
    int weaker = mk(0x255u, TH_WAIT_DELAY, 40);
    int equal  = mk(0x256u, TH_WAIT_DELAY, 16);
    s_tcb[weaker].wake = 1000u;
    s_tcb[equal].wake = 1000u;
    s_vtime_us = 2000u;
    s_cur = runner;
    memset(&g_cpu_store, 0, sizeof(g_cpu_store));
    sched_preempt();
    expect(s_tcb[runner].state == TH_RUNNING,
           "an expired weaker- or equal-priority waiter does not preempt the runner");
    expect(s_tcb[weaker].state == TH_READY && s_tcb[equal].state == TH_READY,
           "both expired waiters are still promoted to runnable");
    s_host_ns_fn = NULL;
}

/* ---- main -------------------------------------------------------------------------- */

int main(void) {
    g_mem_base = (uint8_t *)calloc(1, 0x0c000000u);
    if (!g_mem_base) {
        fprintf(stderr, "sched_selftest: cannot allocate guest arena\n");
        return 2;
    }
    g_mem = g_mem_base + 0x08000000u;
    s_cpu = &g_cpu_store;

    SrCoro *m1 = sr_coro_main();
    expect(m1 != NULL, "initial sr_coro_main adoption");
    /* sched_init calls sr_coro_main again on the SAME thread: the documented
     * ERROR_ALREADY_FIBER path (Windows) / repeat adoption (POSIX) must succeed. */
    sched_init(&g_cpu_store);
    expect(s_sched_coro != NULL, "repeat sr_coro_main adoption (ERROR_ALREADY_FIBER path)");

    test_single_ready();
    test_priority_wins_and_never_inverts();
    test_equal_priority_round_robin();
    test_three_equal_priority_rotation();
    test_mixed_priorities();
    test_sleeping_excluded();
    test_blocked_excluded_until_wake();
    test_dormant_excluded();
    test_current_becomes_nonrunnable();
    test_timed_wait_promotion();
    test_determinism();
    test_entry_return_is_thread_exit();
    test_negative_entry_return_normalizes_to_illegal_argument();
    test_wait_timeout_entry_return_normalizes_to_illegal_argument();
    test_explicit_exit_status(0x800201acu, 0x800200d2u);
    test_explicit_exit_status(0x800201a8u, 0x800200d2u);
    test_explicit_exit_status((uint32_t)-17, 0x800200d2u);
    test_explicit_exit_status(0x78u, 0x78u);
    test_terminate_thread_excluded_after();
    test_delete_and_terminate_delete_contract();
    test_role_uid_capture();
    test_stack_exhaustion_fails_create();
    test_libc_thread_relocation();
    test_coro_self_switch_and_park();
    test_callback_abi_packing();
    test_callback_dispatch_one_preserves_context();
    test_callbacks_behavioral();
    test_callback_renotifies_itself_dispatched_same_pass();
    test_callback_auto_deletes_on_nonzero_return();
    test_dispatch_callbacks_returns_total_count();
    test_sleepcb_consumes_wakeup_banked_during_dispatch();
    test_wake_callbacks_no_op_on_non_cb_wait();
    test_clock_reads_are_observational();
    test_pending_interrupts_progress_and_resume();
    test_interrupt_frame_is_restored();
    test_paced_vtime_is_host_anchored();
    test_paced_vblank_has_one_authority();
    test_expired_timed_wait_enters_strict_priority();

    fprintf(stderr, "sched_selftest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
