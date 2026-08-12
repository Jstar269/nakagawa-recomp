// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * sr_coro — platform-agnostic cooperative coroutine (user-space thread) primitive.
 *
 * The recompiled game runs as straight C, so to suspend a guest thread mid-call-stack the
 * scheduler (src/rt/sched.c) runs each guest thread on its own coroutine and switches between
 * them cooperatively. This header is the ONLY threading primitive sched.c depends on; the
 * host-specific implementation lives entirely in sr_coro.c:
 *
 *   - Windows: Win32 fibers (ConvertThreadToFiber / CreateFiberEx / SwitchToFiber). Fibers
 *     reserve a large stack but commit it lazily, which the deep guest→native call chains need.
 *   - POSIX (Linux / Steam Deck): ucontext_t + swapcontext over an mmap'd, lazily-committed
 *     stack (MAP_NORESERVE), matching the fibers' lazy-commit behaviour.
 *
 * Semantics are identical on both: exactly one coroutine runs per OS thread at a time; a switch
 * suspends the caller and resumes the target; control returns to the caller when something
 * switches back to it. This is cooperative (no preemption) — the scheduler decides when to
 * switch. Switching to the currently running coroutine is a defined no-op (never a host
 * fiber/context self-switch). A coroutine body function must never return; if one does, the
 * backend reports a fatal invariant violation on stderr and parks the dead coroutine by
 * transferring control back to the main coroutine (every later resume bounces straight back).
 */

#ifndef SR_CORO_H
#define SR_CORO_H

#include <stddef.h>

typedef struct SrCoro SrCoro;
typedef void (*SrCoroFn)(void *arg);

/* Adopt the current OS thread as the "main" coroutine (the scheduler side), before creating or
 * switching to any coroutine. Adoption is idempotent per OS thread: repeated calls return the
 * original handle without allocating or changing the current/main identity. Returns NULL when
 * the first adoption fails. */
SrCoro *sr_coro_main(void);

/* Create a suspended coroutine with `stack_size` bytes of stack. It runs fn(arg) the first time
 * it is switched to. Returns NULL if the stack/context could not be allocated. */
SrCoro *sr_coro_create(SrCoroFn fn, void *arg, size_t stack_size);

/* Suspend the currently running coroutine and resume `to`. Returns in the caller's context once
 * some other coroutine switches back to it. */
void sr_coro_switch(SrCoro *to);

/* Destroy a coroutine and free its stack. Must NOT be the currently running coroutine. Safe on
 * NULL. Never destroy the main coroutine. */
void sr_coro_destroy(SrCoro *c);

/* The coroutine currently executing on this OS thread (the main coroutine between switches on
 * the scheduler side). */
SrCoro *sr_coro_current(void);

/* ---- test-only lifecycle instrumentation ---------------------------------------------
 *
 * Compiled ONLY when SR_CORO_LIFECYCLE_TEST is defined, which no production target does --
 * hle-thread-selftest is the sole consumer. Without the macro this file declares nothing
 * extra, sr_coro.c contains no counters or hooks, and the emitted code is byte-for-byte the
 * ordinary implementation.
 *
 * Why this exists. The lifecycle rules that keep the HLE thread selftest safe -- adopt the
 * main coroutine exactly once, never adopt from inside a child, always park on that one
 * established identity, never self-switch, destroy each coroutine exactly once and never
 * while it is running -- were previously defended by scanning the selftest source for
 * forbidden call shapes. Source scanning is evadable (line splicing, macro aliases,
 * indirect aliases, reordered guards) and, worse, the textual presence of a guard does not
 * prove any control dependence on it. These counters are recorded by the real
 * implementation at the moment each operation happens, so a violation is observed rather
 * than inferred, whatever the source looks like.
 *
 * The instrumentation is also bounded, not merely observational. The historical failure
 * mode was a loop that re-adopted the main coroutine on every iteration and consumed tens
 * of gigabytes of host RAM before anything noticed. Under this macro the adoption count and
 * the suppressed-self-switch count are hard-capped and exceeding either aborts immediately,
 * so a reintroduced defect fails in milliseconds instead of exhausting the machine. */
#ifdef SR_CORO_LIFECYCLE_TEST

/* Hard caps. Both are far above any legitimate value (a healthy run adopts once and
 * self-switches never) and far below anything that costs real memory or time. */
#define SR_CORO_LC_MAX_ADOPTIONS    4u
#define SR_CORO_LC_MAX_SELF_SWITCH  1024u
#define SR_CORO_LC_MAX_TRACKED      256

typedef struct SrCoroLifecycle {
    unsigned long adoptions;          /* successful sr_coro_main() calls */
    unsigned long creates;            /* successful sr_coro_create() calls */
    unsigned long destroys;           /* sr_coro_destroy() calls that really destroyed */
    unsigned long live;               /* creates - destroys */
    unsigned long switches;           /* switches that actually transferred control */
    unsigned long self_switch_noops;  /* sr_coro_switch(current): suppressed */
    unsigned long null_switch_noops;  /* sr_coro_switch(NULL): suppressed */
    unsigned long adopt_while_child;  /* adoption attempted while a child coroutine ran */
    unsigned long identity_changes;   /* the adopted main identity changed after the first */
    unsigned long destroy_while_running;
    unsigned long double_destroys;
    unsigned long destroy_of_main;
    unsigned long child_to_main;      /* switches out of a child, targeting the adopted main */
    unsigned long child_to_other;     /* switches out of a child, targeting anything else */
    unsigned long address_reuses;     /* a freed coroutine's address was handed out again */
    unsigned long clean_incarnations; /* retired incarnations that were destroyed exactly once */
    unsigned long bad_incarnations;   /* retired incarnations that were not */
    unsigned long alias_live;         /* create returned a still-live address (impossible) */
    unsigned long tracked_overflow;   /* registry exhausted; counters below are incomplete */
    const void *main_coro;            /* identity established by the first adoption */
    const void *last_switch_from;
    const void *last_switch_to;
} SrCoroLifecycle;

/* Copy the current counters. Safe to call at any time from the instrumented thread. */
void sr_coro_lifecycle_snapshot(SrCoroLifecycle *out);

/* How many switches have targeted exactly `c`. Identity is by pointer, so this cannot be
 * satisfied by a look-alike target. */
unsigned long sr_coro_lifecycle_switches_to(const SrCoro *c);

/* 1 when every coroutine ever created has been destroyed exactly once and none was
 * destroyed while running; 0 otherwise. Writes a human-readable reason to `why` when it
 * returns 0 (pass NULL to skip). */
int sr_coro_lifecycle_all_destroyed_once(const char **why);

#endif /* SR_CORO_LIFECYCLE_TEST */

#endif /* SR_CORO_H */
