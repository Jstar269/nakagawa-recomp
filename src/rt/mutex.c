// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-12.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

/* Plain PSP mutex object model (sceKernelCreateMutex / LockMutex / ...).
 *
 * ---------------------------------------------------------------------------
 * Contract (measured)
 * ---------------------------------------------------------------------------
 * PSPAutotests tests/threads/mutex/ expectations are the executable public
 * specification; PPSSPP Core/HLE/sceKernelMutex.cpp corroborates.  Every cell
 * below cites its source.  "n/r" = non-recursive (no ALLOW_RECURSIVE attr).
 *
 *   CreateMutex(name, attr, initial, options):
 *     name == NULL                              -> ERROR 0x80020001 (create.expected)
 *     attr & ~0xBFF                             -> ILLEGAL_ATTR 0x80020191
 *     initial < 0                               -> ILLEGAL_COUNT 0x800201BD
 *     initial > 1 && n/r                        -> ILLEGAL_COUNT
 *     options != 0: validate a readable 4-byte word, otherwise ignore
 *     success -> uid; initial > 0 sets lockLevel=initial, lockThread=cur
 *
 *   DeleteMutex(uid):
 *     NULL / invalid / already-deleted           -> MUTEX_NOT_FOUND 0x800201C3
 *     success -> 0; blocked waiters wake with WAIT_DELETE 0x800201B5
 *
 *   LockMutex[CB](uid, count, timeoutPtr): entry order is measured by
 *   tests/intr/waits.expected (L168-L181) -- context BEFORE object BEFORE count:
 *     !sched_wait_permitted()                   -> CAN_NOT_WAIT 0x800201A7
 *     bad uid                                   -> MUTEX_NOT_FOUND
 *     count <= 0                                -> ILLEGAL_COUNT
 *     count > 1 && n/r                          -> ILLEGAL_COUNT
 *     count + lockLevel < 0 (signed overflow)   -> MUTEX_LOCK_OVERFLOW 0x800201C6
 *     lockThread == cur: recursive -> lockLevel += count, 0
 *                        n/r       -> MUTEX_RECURSIVE_NOT_ALLOWED 0x800201C8
 *     lockLevel == 0                            -> acquire, 0
 *     else -> wait; timeoutPtr written with the REMAINING time (0 on expiry)
 *
 *   TryLockMutex(uid, count): same validation as Lock, but never blocks; a
 *   contended mutex answers MUTEX_LOCKED 0x800201C4 (try.expected).  No
 *   context gate (waits.cpp never probes a Try form).
 *
 *   UnlockMutex(uid, count):
 *     bad uid                                   -> MUTEX_NOT_FOUND
 *     count <= 0                                -> ILLEGAL_COUNT
 *     count > 1 && n/r                          -> ILLEGAL_COUNT
 *     lockLevel == 0 || lockThread != cur       -> MUTEX_UNLOCKED 0x800201C5
 *     lockLevel < count                         -> MUTEX_UNLOCK_UNDERFLOW 0x800201C7
 *     lockLevel -= count; on 0 hand off to the next waiter
 *
 *   CancelMutex(uid, newCount, numWaitThreadsPtr):
 *     bad uid                                   -> MUTEX_NOT_FOUND
 *     newCount > 1 && n/r                       -> ILLEGAL_COUNT
 *     (LOCK_OVERFLOW / ALREADY_LOCKED are tolerated, matching PPSSPP)
 *     numWaitThreadsPtr = waiter count (only on success)
 *     every waiter wakes with WAIT_CANCEL 0x800201A9
 *     newCount <= 0 -> unlock; else lockLevel=newCount, lockThread=cur
 *
 *   ReferMutexStatus(uid, infoPtr):
 *     bad uid                                   -> MUTEX_NOT_FOUND
 *     infoPtr->size == 0                        -> 0, writes nothing
 *     else write the 0x38-byte record           -> 0
 *
 *   Thread teardown (lock.expected "Woke up after other thread exited",
 *   priority.expected B-chain): exit/terminate releases owned mutexes and
 *   hands each to its next waiter; a waiter that dies is removed from the list.
 *
 * ---------------------------------------------------------------------------
 * Unmeasured / unresolved cells (do not fabricate)
 * ---------------------------------------------------------------------------
 *  - Interrupt-context (ILLEGAL_CONTEXT 0x80020064) is PR-D; sched_wait_permitted
 *    cannot see interrupt context, so LockMutex[CB] there behaves as normal
 *    context and stays a pinned known deviation.
 *  - PPSSPP's 25us/250us timeout quantization (__KernelWaitMutex) is NOT
 *    applied; this runtime uses the same unquantized sched_block_on_timeout as
 *    h_WaitSema.  No hardware cell isolates the quanta.
 *  - PSP_MUTEX_ATTR_PRIORITY waiter ORDER is implemented (hand off to the
 *    highest-priority waiter, priority.expected).  Priority INHERITANCE
 *    (raising the owner's effective priority while held) is NOT implemented and
 *    is unmeasured by the public tests.
 *  - ReferMutexStatus writes lockThread = owning uid, and 0 while unlocked
 *    (consistent with the LwMutex workarea here).  PPSSPP writes -1; the
 *    public tests normalise -1 to 0 so neither is disproven.
 */

#include "recomp.h"
#include "mutex.h"

#include <string.h>

/* Callback dispatch lives in hle.c.  mutex.c is always linked beside it in the
 * production runtime and the hle-thread selftest. */
int sr_thread_has_pending_callbacks(uint32_t thread_uid);
int sr_thread_dispatch_callbacks(void);

#define SR_MUTEX_MAX          1024u   /* create.expected: "Create 1024: OK" */
#define SR_MUTEX_MAX_WAITERS  128u    /* == sched.c MAXTHREADS */

enum { MUTEX_WAIT_NONE = 0, MUTEX_WAIT_MUST_WAIT = 1 };

typedef struct {
    uint32_t thread_uid;   /* waiting thread */
    int32_t  count;        /* requested lock count */
    uint32_t toptr;        /* guest timeout word, 0 = infinite */
    uint64_t deadline;     /* absolute vtime deadline (valid iff toptr != 0) */
    uint32_t outcome;      /* 0 = still waiting, else WAIT_CANCEL / WAIT_DELETE */
} MutexWaiter;

typedef struct {
    int      used;
    uint32_t uid;
    char     name[SR_MUTEX_INFO_NAME_LEN];
    uint32_t attr;
    int32_t  initial_count;
    int32_t  lock_level;   /* live recursion count; 0 == unlocked */
    uint32_t lock_thread;  /* owning thread uid; 0 == unlocked */
    MutexWaiter waiters[SR_MUTEX_MAX_WAITERS];
    int      n_waiters;
} SrMutex;

static SrMutex s_mutex[SR_MUTEX_MAX];

static SrMutex *mutex_find(uint32_t uid) {
    for (uint32_t i = 0; i < SR_MUTEX_MAX; i++)
        if (s_mutex[i].used && s_mutex[i].uid == uid) return &s_mutex[i];
    return NULL;
}

static SrMutex *mutex_new(void) {
    for (uint32_t i = 0; i < SR_MUTEX_MAX; i++) {
        if (!s_mutex[i].used) {
            memset(&s_mutex[i], 0, sizeof(s_mutex[i]));
            s_mutex[i].used = 1;
            s_mutex[i].uid = sr_alloc_uid();
            return &s_mutex[i];
        }
    }
    return NULL;
}

static MutexWaiter *mutex_find_waiter(SrMutex *m, uint32_t thread_uid) {
    for (int i = 0; i < m->n_waiters; i++)
        if (m->waiters[i].thread_uid == thread_uid) return &m->waiters[i];
    return NULL;
}

static void mutex_remove_waiter(SrMutex *m, uint32_t thread_uid) {
    for (int i = 0; i < m->n_waiters; i++) {
        if (m->waiters[i].thread_uid == thread_uid) {
            memmove(&m->waiters[i], &m->waiters[i + 1],
                    (size_t)(m->n_waiters - i - 1) * sizeof(m->waiters[0]));
            m->n_waiters--;
            return;
        }
    }
}

/* Copy a NUL-terminated guest string (name), bounded to `max` bytes.  A name
 * is never required beyond being readable: create.expected only distinguishes
 * NULL (error) from any readable name (success). */
static void mutex_read_name(uint32_t addr, char *out, size_t max) {
    if (!addr || !out || max == 0) return;
    out[0] = '\0';
    const uint8_t *p = (const uint8_t *)SR_HOST(addr);
    for (size_t i = 0; i + 1 < max; i++) {
        if (!sr_guest_span_readable(addr + (uint32_t)i, 1u)) { out[i] = '\0'; return; }
        char c = (char)p[i];
        if (c == '\0') { out[i] = '\0'; return; }
        out[i] = c;
        out[i + 1] = '\0';
    }
}

/* Append a waiter with a timeout deadline derived ONCE from *toptr, so a later
 * spurious wake or callback dispatch re-blocks against the same deadline and
 * never restarts the full timeout (scope item 5). */
static int mutex_add_waiter(SrMutex *m, uint32_t self, int32_t count,
                            uint32_t toptr) {
    if (m->n_waiters >= (int)SR_MUTEX_MAX_WAITERS) return 0;
    MutexWaiter *w = &m->waiters[m->n_waiters++];
    w->thread_uid = self;
    w->count = count;
    w->toptr = toptr;
    w->outcome = 0;
    if (toptr) {
        sched_vtime_refresh();
        w->deadline = sched_vtime_deadline_after(MEM_R32(toptr));
    } else {
        w->deadline = 0;
    }
    return 1;
}

/* Index of the next waiter to receive the lock, or -1 when none.  FIFO takes
 * the list head; PRIORITY takes the highest-priority (lowest number) waiter
 * still actually waiting (outcome == 0). */
static int mutex_pick_waiter(SrMutex *m) {
    if (m->n_waiters == 0) return -1;
    if (!(m->attr & PSP_MUTEX_ATTR_PRIORITY)) {
        for (int i = 0; i < m->n_waiters; i++)
            if (m->waiters[i].outcome == 0) return i;
        return -1;
    }
    int best = -1;
    int best_prio = 0x7fffffff;
    for (int i = 0; i < m->n_waiters; i++) {
        if (m->waiters[i].outcome != 0) continue;
        int prio = sched_thread_priority(m->waiters[i].thread_uid);
        if (prio < best_prio) { best = i; best_prio = prio; }
    }
    return best;
}

/* Release the lock: hand it to the next waiter, or clear ownership.  Does NOT
 * preempt -- callers decide (HLE ops preempt; the scheduler teardown path
 * switches to the scheduler itself). */
static void mutex_release(SrMutex *m) {
    int next = mutex_pick_waiter(m);
    if (next >= 0) {
        MutexWaiter *w = &m->waiters[next];
        m->lock_level = w->count;
        m->lock_thread = w->thread_uid;
        memmove(&m->waiters[next], &m->waiters[next + 1],
                (size_t)(m->n_waiters - next - 1) * sizeof(m->waiters[0]));
        m->n_waiters--;
    } else {
        m->lock_level = 0;
        m->lock_thread = 0;
    }
    sched_wake(m->uid);
}

/* Shared count/relock validation.  Returns 0 with the lock taken, the error
 * code, or MUTEX_WAIT_MUST_WAIT when the call must block. */
static uint32_t mutex_lock_try(SrMutex *m, int32_t count) {
    const int recursive = (m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) != 0;
    const uint32_t cur = sched_current_uid();
    if (count <= 0) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (count > 1 && !recursive) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    /* Two positive ints overflowing to negative == overflow (lock.expected). */
    if (count + m->lock_level < 0) return SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW;
    /* Acquire BEFORE the relock test: an unlocked mutex has lock_thread == 0
     * (the "no owner" sentinel), and interrupt context reports the current uid
     * as 0 (s_cur < 0).  Testing lock_thread == cur first would misread that
     * fresh acquire as a non-recursive relock by "thread 0". */
    if (m->lock_level == 0) { m->lock_level = count; m->lock_thread = cur; return 0; }
    if (m->lock_thread == cur) {
        if (recursive) { m->lock_level += count; return 0; }
        return SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED;
    }
    return MUTEX_WAIT_MUST_WAIT;
}

uint32_t sr_mutex_create(uint32_t name, uint32_t attr, int32_t initial,
                         uint32_t options) {
    if (!name) return SCE_KERNEL_ERROR_ERROR;                     /* create.expected */
    if (attr & ~PSP_MUTEX_ATTR_KNOWN_MASK) return SCE_KERNEL_ERROR_ILLEGAL_ATTR;
    if (initial < 0) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (initial > 1 && !(attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE))
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    /* optionsPtr is accepted but ignored beyond a validated 4-byte read,
     * exactly as PPSSPP does (create.expected "Option sizes" all succeed). */
    if (options && !sr_guest_span_readable(options, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ATTR;

    SrMutex *m = mutex_new();
    if (!m) return SCE_KERNEL_ERROR_NO_MEMORY;
    mutex_read_name(name, m->name, sizeof(m->name));
    m->attr = attr;
    m->initial_count = initial;
    if (initial > 0) {
        m->lock_level = initial;
        m->lock_thread = sched_current_uid();   /* create.expected "Positive count" */
    }
    return m->uid;
}

uint32_t sr_mutex_delete(uint32_t uid) {
    SrMutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_MUTEX_NOT_FOUND;
    m->used = 0;
    m->n_waiters = 0;
    /* Waiters resume, find the object gone, and answer WAIT_DELETE
     * (delete.expected / lock.expected "After delete"). */
    sched_wake(uid);
    sched_preempt();
    return 0;
}

uint32_t sr_mutex_lock(uint32_t uid, int32_t count, uint32_t toptr, int cb) {
    /* Context first: waits.expected L168-L181 puts CAN_NOT_WAIT ahead of both
     * the object lookup and count validation for the blocking forms. */
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;

    SrMutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_MUTEX_NOT_FOUND;

    uint32_t r = mutex_lock_try(m, count);
    if (r == 0) return 0;         /* acquired (or recursive relock) */
    if (r != MUTEX_WAIT_MUST_WAIT) return r;

    const uint32_t self = sched_current_uid();
    mutex_add_waiter(m, self, count, toptr);

    for (;;) {
        m = mutex_find(uid);
        if (!m) return SCE_KERNEL_ERROR_WAIT_DELETE;

        if (m->lock_thread == self) {         /* handed the lock */
            mutex_remove_waiter(m, self);
            return 0;
        }

        MutexWaiter *w = mutex_find_waiter(m, self);
        if (w && w->outcome != 0) {           /* canceled while waiting */
            uint32_t outcome = w->outcome;
            mutex_remove_waiter(m, self);
            return outcome;
        }
        /* w == NULL should not happen: handoff removes the target (handled by
         * the lock_thread check above) and cancel/delete leave the entry until
         * this thread consumes it.  Treat it as delete for safety. */
        if (!w) return SCE_KERNEL_ERROR_WAIT_DELETE;

        if (cb && sr_thread_has_pending_callbacks(self)) {
            sr_thread_dispatch_callbacks();
            continue;   /* the deadline is fixed; dispatching never restarts it */
        }

        if (w->toptr) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= w->deadline) {
                mutex_remove_waiter(m, self);
                MEM_W32(w->toptr, 0u);        /* lock.expected "0ms left" */
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
            uint32_t remaining = (uint32_t)(w->deadline - now);
            if (cb) sched_set_current_cb_wait(1);
            int timed_out = sched_block_on_timeout(uid, remaining);
            if (cb) sched_set_current_cb_wait(0);
            if (timed_out) {
                mutex_remove_waiter(m, self);
                MEM_W32(w->toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
        } else {
            if (cb) sched_set_current_cb_wait(1);
            sched_block_on(uid);
            if (cb) sched_set_current_cb_wait(0);
        }
    }
}

uint32_t sr_mutex_try_lock(uint32_t uid, int32_t count) {
    SrMutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_MUTEX_NOT_FOUND;
    uint32_t r = mutex_lock_try(m, count);
    if (r == 0) return 0;
    if (r == MUTEX_WAIT_MUST_WAIT) return SCE_KERNEL_ERROR_MUTEX_LOCKED;
    return r;
}

uint32_t sr_mutex_unlock(uint32_t uid, int32_t count) {
    SrMutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_MUTEX_NOT_FOUND;
    const int recursive = (m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) != 0;
    if (count <= 0) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (count > 1 && !recursive) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (m->lock_level == 0 || m->lock_thread != sched_current_uid())
        return SCE_KERNEL_ERROR_MUTEX_UNLOCKED;
    if (m->lock_level < count)
        return SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW;

    m->lock_level -= count;
    if (m->lock_level == 0) {
        mutex_release(m);
        sched_preempt();    /* a handed-off waiter runs now, like h_SignalSema */
    }
    return 0;
}

uint32_t sr_mutex_cancel(uint32_t uid, int32_t count, uint32_t num_wait_ptr) {
    SrMutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_MUTEX_NOT_FOUND;

    /* Only the count range is fatal: PPSSPP tolerates LOCK_OVERFLOW and
     * ALREADY_LOCKED here and proceeds to (re)acquire. */
    if (count > 0) {
        if (count > 1 && !(m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE))
            return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
        /* count + lock_level < 0 is tolerated (proceeds to acquire). */
    }

    if (num_wait_ptr && sr_guest_span_writable(num_wait_ptr, 4u))
        MEM_W32(num_wait_ptr, (uint32_t)m->n_waiters);

    for (int i = 0; i < m->n_waiters; i++)
        m->waiters[i].outcome = SCE_KERNEL_ERROR_WAIT_CANCEL;

    if (count <= 0) {
        m->lock_level = 0;
        m->lock_thread = 0;
    } else {
        m->lock_level = count;
        m->lock_thread = sched_current_uid();
    }
    sched_wake(uid);
    sched_preempt();
    return 0;
}

uint32_t sr_mutex_refer_status(uint32_t uid, uint32_t info_ptr) {
    SrMutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_MUTEX_NOT_FOUND;
    if (!info_ptr || !sr_guest_span_writable(info_ptr, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ATTR;

    uint32_t size = MEM_R32(info_ptr);
    if (size == 0) return 0;                 /* refer.expected: writes nothing */

    if (!sr_guest_span_writable(info_ptr, SR_MUTEX_INFO_SIZE))
        return SCE_KERNEL_ERROR_ILLEGAL_ATTR;

    MEM_W32(info_ptr + 0x00u, SR_MUTEX_INFO_SIZE);
    for (uint32_t i = 0; i < SR_MUTEX_INFO_NAME_LEN; i += 4u) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4u && i + b < SR_MUTEX_INFO_NAME_LEN; b++)
            word |= (uint32_t)(uint8_t)m->name[i + b] << (b * 8u);
        MEM_W32(info_ptr + SR_MUTEX_INFO_NAME_OFF + i, word);
    }
    MEM_W32(info_ptr + SR_MUTEX_INFO_ATTR_OFF, m->attr);
    MEM_W32(info_ptr + SR_MUTEX_INFO_INIT_OFF, (uint32_t)m->initial_count);
    MEM_W32(info_ptr + SR_MUTEX_INFO_CUR_OFF, (uint32_t)m->lock_level);
    MEM_W32(info_ptr + SR_MUTEX_INFO_OWNER_OFF, m->lock_thread);
    MEM_W32(info_ptr + SR_MUTEX_INFO_WAIT_OFF, (uint32_t)m->n_waiters);
    return 0;
}

void sr_mutex_thread_end(uint32_t thread_uid) {
    for (uint32_t i = 0; i < SR_MUTEX_MAX; i++) {
        SrMutex *m = &s_mutex[i];
        if (!m->used) continue;
        mutex_remove_waiter(m, thread_uid);   /* it was waiting, not owning */
        if (m->lock_thread == thread_uid) {
            /* Release without preempting: the scheduler's teardown path owns
             * the switch away from this thread. */
            mutex_release(m);
        }
    }
}

#ifdef SR_HLE_THREAD_SELFTEST
void sr_mutex_test_reset(void) {
    memset(s_mutex, 0, sizeof(s_mutex));
}

int sr_mutex_test_state(uint32_t uid, int32_t *level_out, uint32_t *owner_out,
                        int32_t *initial_out, uint32_t *attr_out,
                        int *waiters_out) {
    SrMutex *m = mutex_find(uid);
    if (!m) return 0;
    if (level_out)   *level_out   = m->lock_level;
    if (owner_out)   *owner_out   = m->lock_thread;
    if (initial_out) *initial_out = m->initial_count;
    if (attr_out)    *attr_out    = m->attr;
    if (waiters_out) *waiters_out = m->n_waiters;
    return 1;
}
#endif /* SR_HLE_THREAD_SELFTEST */
