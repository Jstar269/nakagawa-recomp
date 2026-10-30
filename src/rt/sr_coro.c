// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Platform backends for the sr_coro cooperative-coroutine primitive (see sr_coro.h). The
 * scheduler (src/rt/sched.c) is written against this API alone and contains no fiber/ucontext
 * calls, so the same scheduler source builds on Windows and POSIX.
 *
 * The currently-running coroutine is tracked per OS thread in s_current. sr_coro_switch() sets
 * s_current to the target BEFORE switching, so on both backends the freshly-resumed coroutine
 * (and the first-run trampoline) can read sr_coro_current() to find itself.
 */

#if !defined(_WIN32)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "sr_coro.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

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
    void    *stack_low;  /* AllocationBase for this fiber's reserved stack */
    int      is_main;    /* the adopted OS thread: do not DeleteFiber it */
};

static _Thread_local SrCoro *s_current;
static _Thread_local SrCoro *s_main;    /* the adopted scheduler coroutine (park target) */

SrCoro *sr_coro_current(void) { return s_current; }

int sr_coro_stack_remaining(size_t *remaining_out) {
    volatile unsigned char marker = 0;
    MEMORY_BASIC_INFORMATION region;
    uintptr_t current = (uintptr_t)&marker;
    if (!remaining_out || !s_current) return 0;
    *remaining_out = 0;
    if (!s_current->stack_low) {
        if (VirtualQuery((const void *)&marker, &region, sizeof region) != sizeof region ||
            !region.AllocationBase) {
            return 0;
        }
        s_current->stack_low = region.AllocationBase;
    }
    uintptr_t low = (uintptr_t)s_current->stack_low;
    if (current <= low) return 0;
    *remaining_out = (size_t)(current - low);
    return 1;
}

int sr_coro_stack_top(uintptr_t *top_out) {
    /* SwitchToFiber swaps the TIB's stack fields, so StackBase is the running fiber's top. */
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    if (!top_out || !s_current || !tib || !tib->StackBase) return 0;
    *top_out = (uintptr_t)tib->StackBase;
    return 1;
}

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

void sr_coro_call_on_thread_stack(void (*fn)(void *arg), void *arg) {
    if (fn) fn(arg);   /* fibers: no host runtime checks the stack a call runs on */
}

int sr_coro_thread_stack_calls_switch(void) { return 0; }

#else  /* POSIX (Linux / Steam Deck / Android) */

/* Context-switch mechanism. glibc and musl provide getcontext/makecontext/swapcontext; Android's
 * bionic declares ucontext_t (for signal handlers) but implements none of the three, so Android
 * uses the stack-switch routine below. SR_CORO_BACKEND_ASM selects it on another POSIX host too,
 * which is how a Linux build exercises the Android path (make coro-selftest-asm). */
#if defined(__ANDROID__) || defined(SR_CORO_BACKEND_ASM)
#define SR_CORO_ASM 1
#if !defined(__x86_64__) && !defined(__aarch64__)
#error "sr_coro: the stack-switch backend supports x86-64 (System V ABI) and AArch64 (AAPCS64) only"
#endif
#endif

#if !defined(SR_CORO_ASM)
#include <ucontext.h>
#endif
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#if defined(SR_CORO_ASM)
/* sr_coro_stack_switch(save, load): push the callee-saved state the ABI requires a call to
 * preserve onto the running stack, store the stack pointer in *save, adopt `load` as the stack
 * pointer, pop the state saved there and return into it. The floating-point control state (MXCSR
 * and the x87 control word; FPCR) is part of the saved frame, as it is in glibc's swapcontext, so
 * a rounding mode one coroutine leaves set never leaks into another. A new coroutine's first frame
 * is built by coro_initial_frame() below with the same layout. */
void sr_coro_stack_switch(void **save, void *load) __attribute__((visibility("hidden")));
void sr_coro_call_with_stack(void (*fn)(void *), void *arg, uintptr_t stack_top)
    __attribute__((visibility("hidden")));
/* Room left below the suspended main coroutine's saved stack pointer before a thread-stack
 * call starts: clear of the switch frame (which lies above it) and of any red zone. */
#define SR_CORO_THREAD_CALL_GAP 4096u

#if defined(__x86_64__)
/* Frame, from the saved stack pointer up: MXCSR (4 bytes), x87 control word (2, padded to 4),
 * r15, r14, r13, r12, rbx, rbp, return address. */
#define SR_CORO_FRAME_BYTES 64u
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl sr_coro_stack_switch\n"
    ".hidden sr_coro_stack_switch\n"
    ".type sr_coro_stack_switch,@function\n"
    "sr_coro_stack_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $8, %rsp\n"
    "    stmxcsr (%rsp)\n"
    "    fnstcw 4(%rsp)\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    ldmxcsr (%rsp)\n"
    "    fldcw 4(%rsp)\n"
    "    addq $8, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    ".size sr_coro_stack_switch, .-sr_coro_stack_switch\n"
    /* sr_coro_call_with_stack(fn, arg, stack_top): call fn(arg) with the stack pointer at
     * stack_top (16-byte aligned), then return on the caller's stack; rbp holds the way back. */
    ".p2align 4\n"
    ".globl sr_coro_call_with_stack\n"
    ".hidden sr_coro_call_with_stack\n"
    ".type sr_coro_call_with_stack,@function\n"
    "sr_coro_call_with_stack:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    movq %rdx, %rsp\n"
    "    movq %rdi, %rax\n"
    "    movq %rsi, %rdi\n"
    "    callq *%rax\n"
    "    movq %rbp, %rsp\n"
    "    popq %rbp\n"
    "    ret\n"
    ".size sr_coro_call_with_stack, .-sr_coro_call_with_stack\n");
#else
/* Frame, from the saved stack pointer up: x19..x28, x29 (frame pointer), x30 (return address),
 * d8..d15, FPCR, padding to keep the stack pointer 16-byte aligned. */
#define SR_CORO_FRAME_BYTES 176u
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl sr_coro_stack_switch\n"
    ".hidden sr_coro_stack_switch\n"
    ".type sr_coro_stack_switch,%function\n"
    "sr_coro_stack_switch:\n"
    "    hint #34\n"                      /* BTI c: a no-op unless branch protection is on */
    "    sub sp, sp, #176\n"
    "    stp x19, x20, [sp, #0]\n"
    "    stp x21, x22, [sp, #16]\n"
    "    stp x23, x24, [sp, #32]\n"
    "    stp x25, x26, [sp, #48]\n"
    "    stp x27, x28, [sp, #64]\n"
    "    stp x29, x30, [sp, #80]\n"
    "    stp d8, d9, [sp, #96]\n"
    "    stp d10, d11, [sp, #112]\n"
    "    stp d12, d13, [sp, #128]\n"
    "    stp d14, d15, [sp, #144]\n"
    "    mrs x9, fpcr\n"
    "    str x9, [sp, #160]\n"
    "    mov x9, sp\n"
    "    str x9, [x0]\n"
    "    mov sp, x1\n"
    "    ldr x9, [sp, #160]\n"
    "    msr fpcr, x9\n"
    "    ldp x19, x20, [sp, #0]\n"
    "    ldp x21, x22, [sp, #16]\n"
    "    ldp x23, x24, [sp, #32]\n"
    "    ldp x25, x26, [sp, #48]\n"
    "    ldp x27, x28, [sp, #64]\n"
    "    ldp x29, x30, [sp, #80]\n"
    "    ldp d8, d9, [sp, #96]\n"
    "    ldp d10, d11, [sp, #112]\n"
    "    ldp d12, d13, [sp, #128]\n"
    "    ldp d14, d15, [sp, #144]\n"
    "    add sp, sp, #176\n"
    "    ret\n"
    ".size sr_coro_stack_switch, .-sr_coro_stack_switch\n"
    /* sr_coro_call_with_stack(fn, arg, stack_top): as on x86-64; x29 holds the way back. */
    ".p2align 4\n"
    ".globl sr_coro_call_with_stack\n"
    ".hidden sr_coro_call_with_stack\n"
    ".type sr_coro_call_with_stack,%function\n"
    "sr_coro_call_with_stack:\n"
    "    hint #34\n"
    "    stp x29, x30, [sp, #-16]!\n"
    "    mov x29, sp\n"
    "    mov sp, x2\n"
    "    mov x9, x0\n"
    "    mov x0, x1\n"
    "    blr x9\n"
    "    mov sp, x29\n"
    "    ldp x29, x30, [sp], #16\n"
    "    ret\n"
    ".size sr_coro_call_with_stack, .-sr_coro_call_with_stack\n");
#endif
#endif /* SR_CORO_ASM */

struct SrCoro {
    SrCoroFn  fn;
    void     *arg;
#if defined(SR_CORO_ASM)
    void     *sp;          /* saved stack pointer while suspended */
#else
    ucontext_t ctx;
#endif
    void     *stack;       /* usable low end: the guard band lies directly below it */
    size_t    stack_size;  /* usable bytes */
    void     *map;         /* the whole mapping, guard band included (created coroutines) */
    size_t    map_size;
    int       is_main;
};

static _Thread_local SrCoro *s_current;
static _Thread_local SrCoro *s_main;    /* the adopted scheduler coroutine (park target) */

SrCoro *sr_coro_current(void) { return s_current; }

int sr_coro_stack_remaining(size_t *remaining_out) {
    volatile unsigned char marker = 0;
    uintptr_t current = (uintptr_t)&marker;
    uintptr_t low = 0;
    size_t stack_size = 0;
    if (!remaining_out || !s_current) return 0;
    *remaining_out = 0;
    if (!s_current->is_main) {
        low = (uintptr_t)s_current->stack;
        stack_size = s_current->stack_size;
    } else {
        if (!s_current->stack || s_current->stack_size == 0) {
            /* pthread_getattr_np is a GNU extension (glibc, also musl); a non-GNU
             * libc reports the main stack as unavailable here, which the dispatcher
             * turns into the named host-fiber-stack-bounds-unavailable stop rather
             * than a guess. Guest fibers do not take this path. */
            pthread_attr_t attr;
            void *stack_addr = NULL;
            if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
            int attr_result = pthread_attr_getstack(&attr, &stack_addr, &stack_size);
            pthread_attr_destroy(&attr);
            if (attr_result != 0 || !stack_addr) return 0;
            s_current->stack = stack_addr;
            s_current->stack_size = stack_size;
        }
        low = (uintptr_t)s_current->stack;
        stack_size = s_current->stack_size;
    }
    if (stack_size == 0 || current <= low || current - low > stack_size) return 0;
    *remaining_out = (size_t)(current - low);
    return 1;
}

int sr_coro_stack_top(uintptr_t *top_out) {
    /* The main coroutine's bounds are known once sr_coro_stack_remaining has read them. */
    if (!top_out || !s_current || !s_current->stack || s_current->stack_size == 0) return 0;
    *top_out = (uintptr_t)s_current->stack + s_current->stack_size;
    return 1;
}

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

#if defined(SR_CORO_ASM)
/* Build the frame the first sr_coro_stack_switch into `c` pops: zeroed callee-saved registers
 * (a zero frame pointer ends backtraces), the creating thread's floating-point control state, and
 * coro_trampoline as the return address. The trampoline is entered as if called: on x86-64 the
 * stack pointer is 8 below a 16-byte boundary at entry (a zero return slot sits above the entry
 * address), on AArch64 it is 16-byte aligned. */
static int coro_initial_frame(SrCoro *c) {
    uintptr_t top = ((uintptr_t)c->stack + c->stack_size) & ~(uintptr_t)15u;
    if (top - (uintptr_t)c->stack < SR_CORO_FRAME_BYTES + 16u) return 0;
#if defined(__x86_64__)
    unsigned char *frame = (unsigned char *)(top - 16u - 56u);
    uint32_t mxcsr = 0;
    uint16_t fcw = 0;
    __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr));
    __asm__ __volatile__("fnstcw %0" : "=m"(fcw));
    memset(frame, 0, 72u);
    memcpy(frame, &mxcsr, sizeof mxcsr);
    memcpy(frame + 4, &fcw, sizeof fcw);
    {
        void (*entry)(void) = coro_trampoline;
        memcpy(frame + 56, &entry, sizeof entry);   /* popped by ret; rsp = top - 8 after it */
    }
#else
    unsigned char *frame = (unsigned char *)(top - SR_CORO_FRAME_BYTES);
    uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    memset(frame, 0, SR_CORO_FRAME_BYTES);
    {
        void (*entry)(void) = coro_trampoline;
        memcpy(frame + 88, &entry, sizeof entry);   /* x30: ret branches here; sp = top after it */
    }
    memcpy(frame + 160, &fpcr, sizeof fpcr);
#endif
    c->sp = frame;
    return 1;
}
#endif

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
    /* The guard band (see sr_coro.h) sits below the usable range, page aligned. */
    long page = sysconf(_SC_PAGESIZE);
    size_t guard = SR_CORO_STACK_GUARD_BYTES;
    if (page > 0) guard = (guard + (size_t)page - 1u) / (size_t)page * (size_t)page;
    if (stack_size == 0 || stack_size > SIZE_MAX - guard) { free(c); return NULL; }
    c->map_size = stack_size + guard;
    /* MAP_NORESERVE gives lazy commit like a Win32 fiber's reserved stack: the full range is
     * addressable but physical pages are only backed as the deep call chain touches them, so a
     * large per-coroutine stack does not cost real memory while the coroutine is idle. */
    c->map = mmap(NULL, c->map_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (c->map == MAP_FAILED) { free(c); return NULL; }
    if (mprotect(c->map, guard, PROT_NONE) != 0) {
        munmap(c->map, c->map_size);
        free(c);
        return NULL;
    }
    c->stack = (unsigned char *)c->map + guard;
#if defined(SR_CORO_ASM)
    if (!coro_initial_frame(c)) { munmap(c->map, c->map_size); free(c); return NULL; }
#else
    if (getcontext(&c->ctx) != 0) { munmap(c->map, c->map_size); free(c); return NULL; }
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = stack_size;
    c->ctx.uc_link = NULL;   /* body never returns */
    makecontext(&c->ctx, coro_trampoline, 0);
#endif
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
#if defined(SR_CORO_ASM)
    sr_coro_stack_switch(&from->sp, to->sp);
#else
    swapcontext(&from->ctx, &to->ctx);
#endif
}

void sr_coro_destroy(SrCoro *c) {
    if (!c) return;
    if (!LC_DESTROY_OK(c)) return;   /* instrumented builds: refuse an invalid destroy */
    if (c->map && c->map != MAP_FAILED && !c->is_main) munmap(c->map, c->map_size);
    free(c);
}

void sr_coro_call_on_thread_stack(void (*fn)(void *arg), void *arg) {
    if (!fn) return;
#if defined(SR_CORO_ASM)
    if (s_main && s_current && s_current != s_main && s_main->sp) {
        /* The main coroutine is suspended in sr_coro_stack_switch: its frame lies at and above
         * s_main->sp, and nothing runs below it until it resumes, which cannot happen during
         * fn (fn does not switch). */
        uintptr_t top = ((uintptr_t)s_main->sp - SR_CORO_THREAD_CALL_GAP) & ~(uintptr_t)15u;
        sr_coro_call_with_stack(fn, arg, top);
        return;
    }
#endif
    fn(arg);
}

int sr_coro_thread_stack_calls_switch(void) {
#if defined(SR_CORO_ASM)
    return 1;
#else
    return 0;
#endif
}

#endif
