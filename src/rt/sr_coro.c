// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Platform backends for the sr_coro cooperative-coroutine primitive (see sr_coro.h). The
 * scheduler (src/rt/sched.c) is written against this API alone and contains no fiber/ucontext
 * calls, so the same scheduler source builds on Windows and POSIX.
 *
 * The currently-running coroutine is tracked per OS thread in s_current. sr_coro_switch() sets
 * s_current to the target BEFORE switching, so on both backends the freshly-resumed coroutine
 * (and the first-run trampoline) can read sr_coro_current() to find itself.
 */

#include "sr_coro.h"
#include <stdlib.h>
#include <stdio.h>

/* ---- test-only lifecycle instrumentation (SR_CORO_LIFECYCLE_TEST) ---------------------
 *
 * See sr_coro.h for why this exists. Everything here is behind the macro; ordinary builds
 * compile none of it and the hook macros below expand to nothing.
 *
 * The bookkeeping deliberately never dereferences a SrCoro. struct SrCoro is defined
 * separately by each backend below, and a destroyed coroutine's memory is freed, so the
 * registry identifies coroutines by pointer value alone. That is also what makes
 * double-destroy detection safe: the second destroy is recognised and suppressed before
 * anything touches the freed object. */
#ifdef SR_CORO_LIFECYCLE_TEST

typedef enum { LC_FREE = 0, LC_LIVE, LC_DESTROYED } LcSlotState;

typedef struct LcSlot {
    const void   *coro;
    LcSlotState   state;
    int           is_main;
    unsigned long switches_to;
    unsigned long destroy_calls;
} LcSlot;

static LcSlot          s_lc_slot[SR_CORO_LC_MAX_TRACKED];
static int             s_lc_nslot;
static SrCoroLifecycle s_lc;

static LcSlot *lc_find(const void *c) {
    for (int i = 0; i < s_lc_nslot; i++)
        if (s_lc_slot[i].coro == c) return &s_lc_slot[i];
    return NULL;
}

static LcSlot *lc_intern(const void *c) {
    LcSlot *s = lc_find(c);
    if (s) return s;
    if (s_lc_nslot >= SR_CORO_LC_MAX_TRACKED) { s_lc.tracked_overflow++; return NULL; }
    s = &s_lc_slot[s_lc_nslot++];
    s->coro = c;
    s->state = LC_FREE;
    s->is_main = 0;
    s->switches_to = 0;
    s->destroy_calls = 0;
    return s;
}

/* Exceeding a cap is not a test failure to be reported later -- the whole point is that the
 * runaway never gets to run. Abort loudly and immediately. */
static void lc_cap(const char *what, unsigned long value, unsigned long cap) {
    fprintf(stderr,
            "sr_coro: LIFECYCLE CAP EXCEEDED: %s reached %lu (cap %lu). "
            "This is a lifecycle defect, not a slow test; aborting before it consumes the host.\n",
            what, value, cap);
    fflush(stderr);
    abort();
}

static void lc_note_adopt(const void *c, const void *current) {
    s_lc.adoptions++;
    if (s_lc.adoptions > (unsigned long)SR_CORO_LC_MAX_ADOPTIONS)
        lc_cap("main-coroutine adoptions", s_lc.adoptions, SR_CORO_LC_MAX_ADOPTIONS);

    /* Adoption is a one-shot initialisation operation (see sr_coro.h). Doing it from inside
     * a child coroutine means the scheduler identity is being redefined by code that is
     * itself running on top of the old one. */
    if (current) {
        LcSlot *cs = lc_find(current);
        if (!cs || !cs->is_main) s_lc.adopt_while_child++;
    }

    if (!s_lc.main_coro) s_lc.main_coro = c;
    else if (s_lc.main_coro != c) s_lc.identity_changes++;

    LcSlot *s = lc_intern(c);
    if (s) { s->state = LC_LIVE; s->is_main = 1; }
}

/* A coroutine's identity is its *incarnation*, not its address.
 *
 * The registry is keyed by pointer because struct SrCoro is backend-private and a destroyed
 * coroutine's memory is gone. But the allocator legitimately hands the same address back for
 * the next sr_coro_create() once the previous one has been freed, and the tests here do
 * exactly that: each test destroys its coroutines before the next test creates any. Treating
 * the recycled address as the same object made a perfectly balanced run (6 creates, 6
 * destroys, 0 double destroys) report a slot with two destroy calls.
 *
 * So a create onto a DESTROYED slot closes out the previous incarnation -- which must have
 * been destroyed exactly once -- and starts a fresh one at the same address. A create onto a
 * still-LIVE slot is a genuine impossibility and is counted separately. */
static void lc_note_create(const void *c) {
    s_lc.creates++;
    s_lc.live++;
    LcSlot *s = lc_intern(c);
    if (!s) return;
    if (s->state == LC_DESTROYED) {
        if (s->destroy_calls == 1) s_lc.clean_incarnations++;
        else                       s_lc.bad_incarnations++;
        s_lc.address_reuses++;
        s->destroy_calls = 0;
        s->switches_to = 0;
    } else if (s->state == LC_LIVE) {
        s_lc.alias_live++;
    }
    s->state = LC_LIVE;
    s->is_main = 0;
}

/* Returns 1 when the destroy may proceed, 0 when it must be suppressed. */
static int lc_note_destroy(const void *c) {
    LcSlot *s = lc_find(c);
    if (s) s->destroy_calls++;
    if (s && s->state == LC_DESTROYED) {
        s_lc.double_destroys++;
        return 0;                     /* do NOT touch the freed object */
    }
    if (c == (const void *)sr_coro_current()) {
        s_lc.destroy_while_running++;
        return 0;                     /* destroying the running coroutine is never valid */
    }
    if (s && s->is_main) s_lc.destroy_of_main++;
    if (s) s->state = LC_DESTROYED;
    s_lc.destroys++;
    if (s_lc.live) s_lc.live--;
    return 1;
}

static void lc_note_switch(const void *from, const void *to, int performed) {
    if (!to) { s_lc.null_switch_noops++; return; }
    if (!performed) {
        s_lc.self_switch_noops++;
        if (s_lc.self_switch_noops > (unsigned long)SR_CORO_LC_MAX_SELF_SWITCH)
            lc_cap("suppressed self-switches", s_lc.self_switch_noops, SR_CORO_LC_MAX_SELF_SWITCH);
        return;
    }
    s_lc.switches++;
    s_lc.last_switch_from = from;
    s_lc.last_switch_to = to;
    LcSlot *ts = lc_intern(to);
    if (ts) ts->switches_to++;
    /* A child coroutine may only ever transfer control back to the established main. */
    if (from) {
        LcSlot *fs = lc_find(from);
        if (fs && !fs->is_main) {
            if (to == s_lc.main_coro) s_lc.child_to_main++;
            else                      s_lc.child_to_other++;
        }
    }
}

void sr_coro_lifecycle_snapshot(SrCoroLifecycle *out) { if (out) *out = s_lc; }

unsigned long sr_coro_lifecycle_switches_to(const SrCoro *c) {
    LcSlot *s = lc_find((const void *)c);
    return s ? s->switches_to : 0ul;
}

int sr_coro_lifecycle_all_destroyed_once(const char **why) {
    if (s_lc.tracked_overflow) { if (why) *why = "coroutine registry overflowed"; return 0; }
    if (s_lc.double_destroys)  { if (why) *why = "a coroutine was destroyed more than once"; return 0; }
    if (s_lc.destroy_while_running) {
        if (why) *why = "a coroutine was destroyed while it was running";
        return 0;
    }
    if (s_lc.destroy_of_main) { if (why) *why = "the main coroutine was destroyed"; return 0; }
    if (s_lc.alias_live) {
        if (why) *why = "sr_coro_create returned the address of a still-live coroutine";
        return 0;
    }
    if (s_lc.bad_incarnations) {
        if (why) *why = "a reused coroutine address had not been destroyed exactly once";
        return 0;
    }
    /* Every incarnation still resident in the registry must have ended destroyed exactly
     * once; earlier incarnations at the same address were checked as they were retired. */
    unsigned long final_incarnations = 0;
    for (int i = 0; i < s_lc_nslot; i++) {
        const LcSlot *s = &s_lc_slot[i];
        if (s->is_main) continue;
        if (s->state != LC_DESTROYED) {
            if (why) *why = "a created coroutine was never destroyed";
            return 0;
        }
        if (s->destroy_calls != 1) {
            if (why) *why = "a coroutine received a number of destroy calls other than one";
            return 0;
        }
        final_incarnations++;
    }
    /* Closed-out incarnations plus still-resident ones must account for every create. */
    if (s_lc.clean_incarnations + final_incarnations != s_lc.creates) {
        if (why) *why = "the number of completed incarnations does not match the create count";
        return 0;
    }
    if (why) *why = NULL;
    return 1;
}

#define LC_ADOPT(c, cur)          lc_note_adopt((const void *)(c), (const void *)(cur))
#define LC_CREATE(c)              lc_note_create((const void *)(c))
#define LC_DESTROY_OK(c)          lc_note_destroy((const void *)(c))
#define LC_SWITCH(f, t, done)     lc_note_switch((const void *)(f), (const void *)(t), (done))

#else  /* !SR_CORO_LIFECYCLE_TEST -- ordinary builds compile no instrumentation at all */

#define LC_ADOPT(c, cur)          ((void)0)
#define LC_CREATE(c)              ((void)0)
#define LC_DESTROY_OK(c)          1
#define LC_SWITCH(f, t, done)     ((void)0)

#endif /* SR_CORO_LIFECYCLE_TEST */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct SrCoro {
    SrCoroFn fn;
    void    *arg;
    void    *fiber;      /* LPVOID from ConvertThreadToFiber / CreateFiberEx */
    int      is_main;    /* the adopted OS thread: do not DeleteFiber it */
};

static _Thread_local SrCoro *s_current;
static _Thread_local SrCoro *s_main;    /* the adopted scheduler coroutine (park target) */

SrCoro *sr_coro_current(void) { return s_current; }

static void CALLBACK coro_trampoline(void *param) {
    SrCoro *c = (SrCoro *)param;
    c->fn(c->arg);
    /* A body returning violates the sr_coro contract (sched.c's coro_body never returns).
     * SwitchToFiber to the running fiber is documented-undefined and spinning would burn a
     * core, so park by handing control to the main coroutine; if this dead coroutine is
     * ever resumed again, it immediately bounces back. */
    fprintf(stderr, "sr_coro: FATAL: coroutine body returned (coro=%p) -- parking on main\n",
            (void *)c);
    fflush(stderr);
    if (!s_main || s_main == c) abort();   /* no safe transfer target: hard invariant */
    for (;;) sr_coro_switch(s_main);
}

SrCoro *sr_coro_main(void) {
    /* Adoption is idempotent on an OS thread.  Besides making the API safer for
     * callers, this closes the historical failure mode where a loop repeatedly
     * called sr_coro_main(), allocated a fresh wrapper each iteration, and then
     * self-switched forever while exhausting host memory. */
    if (s_main) return s_main;
    SrCoro *c = (SrCoro *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->is_main = 1;
    c->fiber = ConvertThreadToFiber(NULL);
    if (!c->fiber) {
        /* ConvertThreadToFiber fails with ERROR_ALREADY_FIBER when the thread is already a
         * fiber; only that documented condition (cross-checked with IsThreadAFiber) permits
         * adopting the current fiber handle. Any other failure is a real error. */
        if (GetLastError() == ERROR_ALREADY_FIBER && IsThreadAFiber())
            c->fiber = GetCurrentFiber();
        if (!c->fiber) {
            free(c);
            return NULL;
        }
    }
    LC_ADOPT(c, s_current);
    s_current = c;
    s_main = c;
    return c;
}

SrCoro *sr_coro_create(SrCoroFn fn, void *arg, size_t stack_size) {
    SrCoro *c = (SrCoro *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fn = fn;
    c->arg = arg;
    /* Commit a small amount up front; reserve the full stack_size (committed lazily by Windows,
     * matching the deep-call-chain requirement without paying real memory per idle coroutine). */
    c->fiber = CreateFiberEx((SIZE_T)1 << 18, (SIZE_T)stack_size, 0, coro_trampoline, c);
    if (!c->fiber) { free(c); return NULL; }
    LC_CREATE(c);
    return c;
}

void sr_coro_switch(SrCoro *to) {
    /* Self-switch guard: SwitchToFiber to the currently running fiber has documented-
     * unpredictable results. A switch to the current coroutine is a no-op by definition. */
    if (!to || to == s_current) { LC_SWITCH(s_current, to, 0); return; }
    LC_SWITCH(s_current, to, 1);
    s_current = to;
    SwitchToFiber(to->fiber);
}

void sr_coro_destroy(SrCoro *c) {
    if (!c) return;
    if (!LC_DESTROY_OK(c)) return;   /* instrumented builds: refuse an invalid destroy */
    if (c->fiber && !c->is_main) DeleteFiber(c->fiber);
    free(c);
}

#else  /* POSIX (Linux / Steam Deck) */

#include <ucontext.h>
#include <sys/mman.h>

struct SrCoro {
    SrCoroFn  fn;
    void     *arg;
    ucontext_t ctx;
    void     *stack;
    size_t    stack_size;
    int       is_main;
};

static _Thread_local SrCoro *s_current;
static _Thread_local SrCoro *s_main;    /* the adopted scheduler coroutine (park target) */

SrCoro *sr_coro_current(void) { return s_current; }

/* makecontext cannot portably pass a pointer argument, so the trampoline reads the coroutine it
 * is running from s_current (set by sr_coro_switch immediately before the first resume). */
static void coro_trampoline(void) {
    SrCoro *c = s_current;
    c->fn(c->arg);
    /* A body returning violates the sr_coro contract (sched.c's coro_body never returns).
     * Spinning here would burn a host core and falling off the trampoline would resume an
     * invalid context (uc_link is NULL); park by handing control to the main coroutine. */
    fprintf(stderr, "sr_coro: FATAL: coroutine body returned (coro=%p) -- parking on main\n",
            (void *)c);
    fflush(stderr);
    if (!s_main || s_main == c) abort();   /* no safe transfer target: hard invariant */
    for (;;) sr_coro_switch(s_main);
}

SrCoro *sr_coro_main(void) {
    /* Match the Windows backend: adoption is a one-time operation per OS thread
     * and repeated calls return the established scheduler identity. */
    if (s_main) return s_main;
    SrCoro *c = (SrCoro *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->is_main = 1;
    /* The main context is captured on the first swapcontext away from it (sr_coro_switch saves
     * the caller's context into its own ctx), so nothing else is needed here. */
    LC_ADOPT(c, s_current);
    s_current = c;
    s_main = c;
    return c;
}

SrCoro *sr_coro_create(SrCoroFn fn, void *arg, size_t stack_size) {
    SrCoro *c = (SrCoro *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fn = fn;
    c->arg = arg;
    c->stack_size = stack_size;
    /* MAP_NORESERVE gives lazy commit like a Win32 fiber's reserved stack: the full range is
     * addressable but physical pages are only backed as the deep call chain touches them, so a
     * large per-coroutine stack does not cost real memory while the coroutine is idle. */
    c->stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (c->stack == MAP_FAILED) { free(c); return NULL; }
    if (getcontext(&c->ctx) != 0) { munmap(c->stack, stack_size); free(c); return NULL; }
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = stack_size;
    c->ctx.uc_link = NULL;   /* body never returns */
    makecontext(&c->ctx, coro_trampoline, 0);
    LC_CREATE(c);
    return c;
}

void sr_coro_switch(SrCoro *to) {
    /* Self-switch guard: swapping a context with itself is a wasteful no-op at best;
     * a switch to the current coroutine is a no-op by definition. */
    if (!to || to == s_current) { LC_SWITCH(s_current, to, 0); return; }
    SrCoro *from = s_current;
    LC_SWITCH(from, to, 1);
    s_current = to;
    swapcontext(&from->ctx, &to->ctx);
}

void sr_coro_destroy(SrCoro *c) {
    if (!c) return;
    if (!LC_DESTROY_OK(c)) return;   /* instrumented builds: refuse an invalid destroy */
    if (c->stack && c->stack != MAP_FAILED && !c->is_main) munmap(c->stack, c->stack_size);
    free(c);
}

#endif
