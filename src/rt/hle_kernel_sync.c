// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Synchronisation: semaphores, event flags, mutexes, lightweight mutexes, message pipes and
 * mailboxes. Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* Semaphore error codes measured by PSPAutotests
 * tests/threads/semaphores/wait.expected, which is a NORMAL-context oracle and
 * therefore covers exactly the column tests/intr/waits.expected does not:
 *
 *   UNKNOWN_SEMID  sceKernelWaitSema(0, 1, NULL)          -> 80020199   (L21)
 *                  sceKernelWaitSema(0xDEADBEEF, 1, NULL) -> 80020199   (L23)
 *                  a deleted semaphore id                 -> 80020199   (L25)
 *   ILLEGAL_COUNT  need 100 against maxCount 1            -> 800201BD   (L3, L11)
 *                  need 0                                 -> 800201BD   (L13)
 *                  need -1                                -> 800201BD   (L5, L15)
 *
 * WAIT_TIMEOUT is the same file's L9/L17 result for a wait that was legal,
 * entered, and expired. */
#define SCE_KERNEL_ERROR_UNKNOWN_SEMID 0x80020199u
#define SCE_KERNEL_ERROR_ILLEGAL_COUNT 0x800201bdu
#ifndef SCE_KERNEL_ERROR_WAIT_CANCEL
#define SCE_KERNEL_ERROR_WAIT_CANCEL   0x800201a9u
#endif
#define SCE_KERNEL_ERROR_UNKNOWN_MPPID 0x8002019eu
#define SCE_KERNEL_ERROR_MPP_FULL      0x800201b3u
#define SCE_KERNEL_ERROR_MPP_EMPTY     0x800201b4u
#define SCE_KERNEL_ERROR_ILLEGAL_SIZE  0x800201bcu
/* Message-pipe waiter attributes are independent for send and receive waits. */
#define PSP_MPP_ATTR_SEND_PRIORITY     0x0100u
#define PSP_MPP_ATTR_RECEIVE_PRIORITY  0x1000u

static int msg_pipe_add_waiter(MsgPipe *p, SrWaitHandle invocation, int is_send,
                               uint32_t size, uint32_t wait_mode, uint32_t buffer,
                               uint32_t resultp, uint32_t progress, uint32_t toptr,
                               uint64_t deadline) {
    if (p->nwaiters == p->wait_cap) {
        int next;
        if (p->wait_cap) {
            if (p->wait_cap > INT_MAX / 2) return 0;
            next = p->wait_cap * 2;
        } else {
            next = 4;
        }
        if ((size_t)next > SIZE_MAX / sizeof(*p->waiters)) return 0;
        MsgPipeWaiter *grown = (MsgPipeWaiter *)realloc(
            p->waiters, (size_t)next * sizeof(*grown));
        if (!grown) return 0;
        p->waiters = grown;
        p->wait_cap = next;
    }
    p->waiters[p->nwaiters++] = (MsgPipeWaiter){invocation, is_send, size, wait_mode, buffer, resultp, progress, toptr, deadline};
    return 1;
}

static void msg_pipe_remove_waiter(MsgPipe *p, SrWaitHandle invocation) {
    for (int i = 0; i < p->nwaiters; i++) {
        if (p->waiters[i].invocation == invocation) {
            for (int j = i; j + 1 < p->nwaiters; j++) p->waiters[j] = p->waiters[j + 1];
            p->nwaiters--;
            return;
        }
    }
}

static int msg_pipe_select_waiter_available(const MsgPipe *p, int is_send, int eligible_only,
                                            uint32_t available, SrWaitHandle *invocation_out) {
    int priority = (p->attr & (is_send ? PSP_MPP_ATTR_SEND_PRIORITY :
                                         PSP_MPP_ATTR_RECEIVE_PRIORITY)) != 0u;
    int best = -1;
    int best_priority = INT_MAX;
    for (int i = 0; i < p->nwaiters; i++) {
        const MsgPipeWaiter *waiter = &p->waiters[i];
        SrWaitState state;
        uint32_t owner;
        if (waiter->is_send != is_send ||
            !sched_wait_state(waiter->invocation, &state, &owner) ||
            state != SR_WAIT_BLOCKED) continue;
        if (eligible_only && (waiter->wait_mode == 0u ? available < waiter->size :
                                                       available == 0u)) continue;
        SrThreadRunStatus status;
        if (sched_thread_run_status(owner, &status) != 0 ||
            status.waitType == 0u || status.waitId != p->uid) continue;
        if (!priority) {
            best = i;
            break;
        }
        if (best < 0 || (int)status.currentPriority < best_priority) {
            best = i;
            best_priority = (int)status.currentPriority;
        }
    }
    if (best < 0) return 0;
    *invocation_out = p->waiters[best].invocation;
    return 1;
}

static int msg_pipe_select_waiter(const MsgPipe *p, int is_send, int eligible_only,
                                  SrWaitHandle *invocation_out) {
    uint32_t available = is_send ? p->capacity - p->count - p->reserved : p->count;
    return msg_pipe_select_waiter_available(p, is_send, eligible_only, available, invocation_out);
}

static int msg_pipe_wake_one_waiter(MsgPipe *p, int is_send) {
    SrWaitHandle invocation;
    return msg_pipe_select_waiter(p, is_send, 1, &invocation) &&
           sched_wait_notify(invocation);
}

static void msg_pipe_wake_waiter(MsgPipe *p, int is_send) {
    if (msg_pipe_wake_one_waiter(p, is_send)) sched_preempt();
}

static void msg_pipe_wake_after_transfer(MsgPipe *p, int is_send) {
    if (is_send) {
        if (p->count) msg_pipe_wake_waiter(p, 0);
    } else {
        if (p->count < p->capacity) msg_pipe_wake_waiter(p, 1);
        if (p->count) msg_pipe_wake_waiter(p, 0);
    }
}

/* Every finish/abandon uses this hook, including transfer errors and blocked
 * owner teardown. Reconsider actual requests; being blocked says nothing about
 * a smaller successor's eligibility. This hook never preempts: teardown may
 * still be inside a composite TerminateDeleteThread operation. */
static int msg_pipe_detach_invocation(SrWaitHandle invocation, uint32_t object) {
    MsgPipe *p = msg_pipe_find(object);
    if (!p) return 0;
    if (p->reservation == invocation) {
        p->count += p->reserved;
        p->reserved = 0u;
        p->reservation = 0;
    }
    msg_pipe_remove_waiter(p, invocation);
    SrWaitHandle successor;
    int notified = 0;
    if (msg_pipe_select_waiter(p, 0, 1, &successor))
        notified |= sched_wait_notify(successor);
    if (msg_pipe_select_waiter(p, 1, 1, &successor))
        notified |= sched_wait_notify(successor);
    return notified;
}

static int msg_pipe_trace_call(uint32_t calls) {
    return getenv("SR_MSGLOG") && (calls <= 16u || (calls & (calls - 1u)) == 0u);
}

static uint32_t h_CreateMsgPipe(CpuState *s) {
    /* a0=name, a1=partition, a2=attr, a3=bufferSize, t0=option. */
    (void)A1;
    /* Issue #178: validate against the documented PSP-resource model BEFORE any
     * host allocation. Sizes above MSG_PIPE_MAX_CAPACITY are
     * outside the modeled range. They fail before malloc, before a UID is
     * handed out, and before a slot is reserved, so a rejected create leaves
     * no observable state behind. */
    /* A NULL name is rejected first with NO_MEMORY, matching PPSSPP's
     * sceKernelCreateMsgPipe (emulator consensus, not measured here). A non-NULL
     * name that is unmapped or unterminated fails closed with ILLEGAL_ADDR
     * (project choice) instead of silently becoming "". */
    if (!A0) return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */
    if (A3 > MSG_PIPE_MAX_CAPACITY) return SCE_KERNEL_ERROR_ILLEGAL_SIZE;
    char name[32];
    if (!guest_cstr(A0, name, sizeof(name))) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    /* A3 <= MSG_PIPE_MAX_CAPACITY (1 MiB), so the allocation is bounded and
     * size_t conversion cannot truncate. */
    size_t capacity = (size_t)A3;
    for (int i = 0; i < MSG_PIPE_MAX; i++) {
        MsgPipe *p = &s_msg_pipes[i];
        if (p->used) continue;
        uint8_t *data = capacity ? (uint8_t *)malloc(capacity) : NULL;
        if (capacity && !data) return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */
        memset(p, 0, sizeof(*p));
        p->used = 1;
        p->uid = sr_alloc_uid();
        p->attr = A2;
        p->capacity = A3;
        p->data = data;
        memcpy(p->name, name, sizeof(p->name));
        if (hle_log_on() || getenv("SR_MSGLOG"))
            fprintf(stderr, "HLE: CreateMsgPipe uid=0x%x name='%s' attr=0x%x size=%u\n",
                    p->uid, p->name, p->attr, p->capacity);
        return p->uid;
    }
    return 0x80020190u;
}

static uint32_t h_DeleteMsgPipe(CpuState *s) {
    MsgPipe *p = msg_pipe_find(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_MPPID;
    if (getenv("SR_MSGLOG"))
        fprintf(stderr, "HLE: DeleteMsgPipe uid=0x%x name='%s' queued=%u send=%u receive=%u\n",
                p->uid, p->name, p->count, p->send_calls, p->receive_calls);
    free(p->data);
    free(p->waiters);
    memset(p, 0, sizeof(*p));
    sched_wait_cancel_object(A0, SCE_KERNEL_ERROR_WAIT_DELETE);
    sched_preempt();
    return 0;
}

/* Zero-buffer pipes transfer bytes directly between opposite wait queues.
 * Each blocked invocation keeps its own progress, including partial FULL waits. */
static MsgPipeWaiter *msg_pipe_waiter_by_handle(MsgPipe *p, SrWaitHandle handle) {
    for (int i = 0; i < p->nwaiters; i++)
        if (p->waiters[i].invocation == handle) return &p->waiters[i];
    return NULL;
}
static int msg_pipe_direct(MsgPipe *p, int is_send, uint32_t buffer, uint32_t size,
                           uint32_t *progress, int *woke) {
    while (*progress < size) {
        SrWaitHandle other_handle;
        if (!msg_pipe_select_waiter_available(p, !is_send, p->capacity != 0u,
                                              size - *progress, &other_handle)) break;
        MsgPipeWaiter *other = msg_pipe_waiter_by_handle(p, other_handle);
        if (!other || other->progress >= other->size) return 0;
        uint32_t amount = size - *progress;
        if (amount > other->size - other->progress) amount = other->size - other->progress;
        uint32_t source = is_send ? buffer + *progress : other->buffer + other->progress;
        uint32_t dest = is_send ? other->buffer + other->progress : buffer + *progress;
        if (!sr_guest_span_readable(source, amount) || !sr_guest_span_writable(dest, amount)) {
            /* The caller's span was checked before rendezvous; an invalid
             * opposite waiter must fail on its own, not fail this caller. */
            *woke |= sched_wait_complete(other_handle, SCE_KERNEL_ERROR_ILLEGAL_ADDR);
            continue;
        }
        for (uint32_t i = 0; i < amount; i++) MEM_W8(dest + i, MEM_R8(source + i));
        *progress += amount;
        other->progress += amount;
        if (other->resultp) MEM_W32(other->resultp, other->progress);
        if (other->progress == other->size || other->wait_mode == 1u) {
            if (other->toptr) {
                sched_vtime_refresh();
                uint64_t now = sched_vtime_us();
                MEM_W32(other->toptr, now < other->deadline ? (uint32_t)(other->deadline - now) : 0u);
            }
            *woke |= sched_wait_complete(other_handle, 0u);
        }
    }
    return 1;
}
static uint32_t msg_pipe_rendezvous(CpuState *s, MsgPipe *p, int is_send, int is_cb, int poll) {
    uint32_t uid = A0, buffer = A1, size = A2, mode = A3;
    uint32_t resultp = stack_arg(s, 0), toptr = poll ? 0u : stack_arg(s, 1);
    if ((is_send && !sr_guest_span_readable(buffer, size)) ||
        (!is_send && !sr_guest_span_writable(buffer, size))) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (toptr && !sr_guest_span_writable(toptr, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (!poll && !sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    uint32_t progress = 0u;
    int woke = 0;
    if (!msg_pipe_direct(p, is_send, buffer, size, &progress, &woke)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (progress == size || (mode == 1u && progress)) {
        if (resultp) MEM_W32(resultp, progress);
        if (woke) sched_preempt();
        return 0u;
    }
    if (poll) {
        if (resultp && mode == 1u) MEM_W32(resultp, progress);
        if (woke) sched_preempt();
        return is_send ? SCE_KERNEL_ERROR_MPP_FULL : SCE_KERNEL_ERROR_MPP_EMPTY;
    }
    uint64_t deadline = 0u;
    if (toptr) {
        uint32_t timeout = MEM_R32(toptr);
        if (!timeout) { if (woke) sched_preempt(); return SCE_KERNEL_ERROR_WAIT_TIMEOUT; }
        sched_vtime_refresh();
        deadline = sched_vtime_deadline_after(timeout);
    }
    SrWaitHandle handle = sched_wait_begin(uid, deadline, is_cb, msg_pipe_detach_invocation);
    if (!handle) { if (woke) sched_preempt(); return 0x80020190u; }
    if (!msg_pipe_add_waiter(p, handle, is_send, size, mode, buffer, resultp, progress, toptr, deadline)) {
        (void)sched_wait_finish(handle);
        if (woke) sched_preempt();
        return 0x80020190u;
    }
    if (resultp) MEM_W32(resultp, progress);
    uint32_t result = 0u;
    for (;;) {
        if (sched_wait_take_result(handle, &result)) break;
        if (is_cb && sr_thread_has_pending_callbacks(sched_current_uid())) {
            sched_wait_callback(handle, 1);
            sr_thread_dispatch_callbacks();
            sched_wait_callback(handle, 0);
            if (sched_wait_take_result(handle, &result)) break;
            p = msg_pipe_find(uid);
            if (!p) { result = SCE_KERNEL_ERROR_WAIT_DELETE; break; }
        }
        MsgPipeWaiter *self = msg_pipe_waiter_by_handle(p, handle);
        if (!self) { result = SCE_KERNEL_ERROR_WAIT_DELETE; break; }
        progress = self->progress;
        if (!msg_pipe_direct(p, is_send, buffer, size, &progress, &woke)) {
            result = SCE_KERNEL_ERROR_ILLEGAL_ADDR; break;
        }
        self = msg_pipe_waiter_by_handle(p, handle);
        if (!self) { result = SCE_KERNEL_ERROR_WAIT_DELETE; break; }
        self->progress = progress;
        if (resultp) MEM_W32(resultp, progress);
        if (progress == size || (mode == 1u && progress)) { result = 0u; break; }
        uint32_t remaining = 0u;
        if (toptr) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= deadline) { MEM_W32(toptr, 0u); result = SCE_KERNEL_ERROR_WAIT_TIMEOUT; break; }
            remaining = (uint32_t)(deadline - now);
        }
        int timed_out = sched_wait_block(handle, remaining, toptr != 0u);
        if (sched_wait_take_result(handle, &result)) break;
        p = msg_pipe_find(uid);
        if (!p) { result = SCE_KERNEL_ERROR_WAIT_DELETE; break; }
        if (timed_out) { if (toptr) MEM_W32(toptr, 0u); result = SCE_KERNEL_ERROR_WAIT_TIMEOUT; break; }
    }
    if (toptr && result != SCE_KERNEL_ERROR_WAIT_TIMEOUT) {
        sched_vtime_refresh();
        uint64_t now = sched_vtime_us();
        MEM_W32(toptr, now < deadline ? (uint32_t)(deadline - now) : 0u);
    }
    if (sched_wait_finish(handle)) woke = 1;
    if (woke) sched_preempt();
    return result;
}

static uint32_t msg_pipe_reserve_receiver(MsgPipe *p, uint32_t buffer, uint32_t amount) {
    /* Reserve a direct handoff until the receiver actually resumes.  The
     * bytes occupy pipe capacity and can be restored if its wait departs. */
    SrWaitHandle receiver;
    uint32_t direct = 0u;
    if (!p->count && !p->reserved &&
        msg_pipe_select_waiter_available(p, 0, 1, amount, &receiver)) {
        MsgPipeWaiter *waiter = msg_pipe_waiter_by_handle(p, receiver);
        uint32_t n = waiter->size < amount ? waiter->size : amount;
        if (sr_guest_span_writable(waiter->buffer, n)) {
            for (uint32_t i = 0; i < n; i++) {
                uint8_t byte = MEM_R8(buffer + i);
                MEM_W8(waiter->buffer + i, byte);
                p->data[p->write_pos] = byte;
                p->write_pos = (p->write_pos + 1u) % p->capacity;
            }
            p->reservation = receiver;
            p->reserved = direct = n;
            (void)sched_wait_notify(receiver);
        }
    }
    return direct;
}

static uint32_t h_TrySendMsgPipe(CpuState *s) {
    /* a0=uid, a1=source, a2=size, a3=waitMode, t0=resultSize. */
    uint32_t resultp = stack_arg(s, 0);
    /* Issue #178: preflight the complete writable resultSize span BEFORE any
     * write, so an invalid result pointer is rejected with no side effect
     * instead of being silently no-op'd mid-send.  Historical semantics are
     * preserved: resultSize is zeroed before the pipe lookup, but only once
     * its span has been validated. */
    if (resultp && !sr_guest_span_writable(resultp, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t saved_result = resultp ? MEM_R32(resultp) : 0u;
    if (resultp) MEM_W32(resultp, 0);
    MsgPipe *p = msg_pipe_find(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_MPPID;
    /* PSP-B3-01 (psp-hw-20260917): wait mode 2 is ILLEGAL_MODE (0x80020195). */
    if (A3 > 1u) return 0x80020195u;
    if ((int32_t)A2 < 0) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (A2 == 0 || (p->capacity && A2 > p->capacity)) return SCE_KERNEL_ERROR_ILLEGAL_SIZE;
    if (!p->capacity) {
        uint32_t rc = msg_pipe_rendezvous(s, p, 1, 0, 1);
        if (rc == SCE_KERNEL_ERROR_MPP_FULL && A3 == 0u && resultp)
            MEM_W32(resultp, saved_result);
        return rc;
    }
    p->send_calls++;
    uint32_t free_bytes = p->capacity - p->count - p->reserved;
    uint32_t amount = A2;
    if (amount > free_bytes) {
        if (A3 == 0 || free_bytes == 0) {
            if (msg_pipe_trace_call(p->send_calls))
                fprintf(stderr, "MSGPIPE: send uid=0x%x thread=0x%x call=%u requested=%u queued=%u -> FULL\n",
                        p->uid, sched_current_uid(), p->send_calls, A2, p->count);
            /* PSP-B3-01 (psp-hw-20260917): an all-or-nothing send without room
             * answers MPP_FULL without writing the result. */
            if (resultp) MEM_W32(resultp, saved_result);
            return SCE_KERNEL_ERROR_MPP_FULL;
        }
        amount = free_bytes;
    }
    /* Issue #178: preflight the complete source span for the ACTUAL transfer
     * amount before mutating FIFO indices/count.  Per-byte MEM_R8 reads are
     * individually bounds-safe, but a partial/invalid span would otherwise
     * advance write_pos/count while fabricating zero bytes for the invalid
     * tail.  Reject the whole operation instead. */
    if (!sr_guest_span_readable(A1, amount)) {
        if (msg_pipe_trace_call(p->send_calls))
            fprintf(stderr, "MSGPIPE: send uid=0x%x thread=0x%x call=%u requested=%u -> ILLEGAL_ADDR (src 0x%08x len %u)\n",
                    p->uid, sched_current_uid(), p->send_calls, A2, A1, amount);
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    uint32_t direct = msg_pipe_reserve_receiver(p, A1, amount);
    for (uint32_t i = direct; i < amount; i++) {
        p->data[p->write_pos] = MEM_R8(A1 + i);
        p->write_pos = (p->write_pos + 1u) % p->capacity;
    }
    p->count += amount - direct;
    if (resultp) MEM_W32(resultp, amount);
    if (msg_pipe_trace_call(p->send_calls))
        fprintf(stderr, "MSGPIPE: send uid=0x%x thread=0x%x call=%u requested=%u transferred=%u queued=%u\n",
                p->uid, sched_current_uid(), p->send_calls, A2, amount, p->count);
    msg_pipe_wake_after_transfer(p, 1);
    if (direct) sched_preempt();
    return 0;
}

static uint32_t h_TryReceiveMsgPipe(CpuState *s) {
    /* a0=uid, a1=destination, a2=size, a3=waitMode, t0=resultSize. */
    uint32_t resultp = stack_arg(s, 0);
    /* Issue #178: preflight the complete writable resultSize span BEFORE any
     * write (see h_TrySendMsgPipe).  Historical semantics are preserved:
     * resultSize is zeroed before the pipe lookup, but only once its span has
     * been validated. */
    if (resultp && !sr_guest_span_writable(resultp, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (resultp) MEM_W32(resultp, 0);
    MsgPipe *p = msg_pipe_find(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_MPPID;
    /* PSP-B3-01 (psp-hw-20260917): wait mode 2 is ILLEGAL_MODE (0x80020195). */
    if (A3 > 1u) return 0x80020195u;
    if ((int32_t)A2 < 0) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (A2 == 0 || (p->capacity && A2 > p->capacity)) return SCE_KERNEL_ERROR_ILLEGAL_SIZE;
    if (!p->capacity) return msg_pipe_rendezvous(s, p, 0, 0, 1);
    p->receive_calls++;
    uint32_t amount = A2;
    uint32_t available = p->reserved ? 0u : p->count;
    if (amount > available) {
        if (A3 == 0 || available == 0) {
            if (msg_pipe_trace_call(p->receive_calls))
                fprintf(stderr, "MSGPIPE: receive uid=0x%x thread=0x%x call=%u requested=%u queued=%u -> EMPTY\n",
                        p->uid, sched_current_uid(), p->receive_calls, A2, p->count);
            return SCE_KERNEL_ERROR_MPP_EMPTY;
        }
        amount = available;
    }
    /* Issue #178: preflight the complete destination span for the ACTUAL
     * transfer amount before mutating FIFO indices/count, so an invalid
     * destination can never leave the pipe partially drained. */
    if (!sr_guest_span_writable(A1, amount)) {
        if (msg_pipe_trace_call(p->receive_calls))
            fprintf(stderr, "MSGPIPE: receive uid=0x%x thread=0x%x call=%u requested=%u -> ILLEGAL_ADDR (dst 0x%08x len %u)\n",
                    p->uid, sched_current_uid(), p->receive_calls, A2, A1, amount);
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    for (uint32_t i = 0; i < amount; i++) {
        MEM_W8(A1 + i, p->data[p->read_pos]);
        p->read_pos = (p->read_pos + 1) % p->capacity;
    }
    p->count -= amount;
    if (resultp) MEM_W32(resultp, amount);
    if (msg_pipe_trace_call(p->receive_calls))
        fprintf(stderr, "MSGPIPE: receive uid=0x%x thread=0x%x call=%u requested=%u transferred=%u queued=%u\n",
                p->uid, sched_current_uid(), p->receive_calls, A2, amount, p->count);
    msg_pipe_wake_after_transfer(p, 0);
    return 0;
}

static int msg_pipe_transfer_ready(const MsgPipe *p, int is_send, uint32_t size,
                                  uint32_t wait_mode, uint32_t *amount_out) {
    uint32_t available = is_send ? p->capacity - p->count - p->reserved :
                         (p->reserved ? 0u : p->count);
    if (wait_mode == 0u) {
        if (available < size) return 0;
        *amount_out = size;
        return 1;
    }
    if (available == 0u) return 0;
    *amount_out = available < size ? available : size;
    return 1;
}

static uint32_t msg_pipe_transfer(MsgPipe *p, uint32_t buffer, uint32_t amount,
                                 uint32_t resultp, int is_send, int *direct_wake) {
    if (is_send) {
        if (!sr_guest_span_readable(buffer, amount)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        uint32_t direct = msg_pipe_reserve_receiver(p, buffer, amount);
        for (uint32_t i = direct; i < amount; i++) {
            p->data[p->write_pos] = MEM_R8(buffer + i);
            p->write_pos = (p->write_pos + 1u) % p->capacity;
        }
        p->count += amount - direct;
        if (resultp) MEM_W32(resultp, amount);
        if (direct) *direct_wake = 1;
        return 0u;
    } else {
        if (!sr_guest_span_writable(buffer, amount)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        for (uint32_t i = 0; i < amount; i++) {
            MEM_W8(buffer + i, p->data[p->read_pos]);
            p->read_pos = (p->read_pos + 1u) % p->capacity;
        }
        p->count -= amount;
    }
    if (resultp) MEM_W32(resultp, amount);
    return 0;
}

static uint32_t msg_pipe_blocking_common(CpuState *s, int is_send, int is_cb) {
    /* Public PSPSDK identifies arg6 as timeout and leaves arg5 named unk2.
     * Preserve the current result-size model; arg5 hardware qualification is
     * separate from semantic wait lifetime. */
    uint32_t resultp = stack_arg(s, 0);
    uint32_t toptr = stack_arg(s, 1);
    if (resultp && !sr_guest_span_writable(resultp, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (resultp) MEM_W32(resultp, 0u);
    MsgPipe *p = msg_pipe_find(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_MPPID;
    if (toptr && !sr_guest_span_writable(toptr, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (A3 > 1u) return 0x80020195u; /* PSP-B3-01: ILLEGAL_MODE. */
    if ((int32_t)A2 < 0) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (A2 == 0u || (p->capacity && A2 > p->capacity)) return SCE_KERNEL_ERROR_ILLEGAL_SIZE;
    if (!p->capacity) return msg_pipe_rendezvous(s, p, is_send, is_cb, 0);
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;

    uint32_t uid = A0;
    uint32_t buffer = A1;
    uint32_t size = A2;
    uint32_t wait_mode = A3;
    uint32_t amount = 0u;
    int direct_wake = 0;
    if (is_send) p->send_calls++;
    else p->receive_calls++;

    if (msg_pipe_transfer_ready(p, is_send, size, wait_mode, &amount)) {
        uint32_t transfer_rc = msg_pipe_transfer(p, buffer, amount, resultp, is_send, &direct_wake);
        if (transfer_rc != 0u) return transfer_rc;
        msg_pipe_wake_after_transfer(p, is_send);
        if (direct_wake) sched_preempt();
        return 0u;
    }

    uint64_t end_time = 0u;
    int has_timeout = 0;
    if (toptr) {
        uint32_t timeout_us = MEM_R32(toptr);
        if (timeout_us == 0u) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        sched_vtime_refresh();
        end_time = sched_vtime_deadline_after((uint64_t)timeout_us);
        has_timeout = 1;
    }

    uint32_t cur_thid = sched_current_uid();
    if (!cur_thid) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    SrWaitHandle waiter_scope = sched_wait_begin(uid, end_time, is_cb,
                                                 msg_pipe_detach_invocation);
    if (!waiter_scope) return 0x80020190u;
    if (!msg_pipe_add_waiter(p, waiter_scope, is_send, size, wait_mode, buffer, resultp, 0u, toptr, end_time)) {
        if (sched_wait_finish(waiter_scope)) sched_preempt();
        return 0x80020190u;
    }

    uint32_t result;
    for (;;) {
        if (sched_wait_take_result(waiter_scope, &result)) break;
        if (is_cb && sr_thread_has_pending_callbacks(cur_thid)) {
            /* The preceding block has detached and captured its result before
             * recursive HLE can install an inner active scheduler wait. */
            sched_wait_callback(waiter_scope, 1);
            sr_thread_dispatch_callbacks();
            sched_wait_callback(waiter_scope, 0);
            /* Consume the parent's retained outcome before reconstructing an
             * object error; an unrelated child cannot own this result. */
            if (sched_wait_take_result(waiter_scope, &result)) break;
            p = msg_pipe_find(uid);
            if (!p) { result = SCE_KERNEL_ERROR_WAIT_DELETE; break; }
            continue;
        }

        uint32_t remaining = 0u;
        if (has_timeout) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= end_time) {
                MEM_W32(toptr, 0u);
                result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
                break;
            }
            remaining = (uint32_t)(end_time - now);
        }

        /* Callback code may have completed a transfer while this invocation
         * was running, when the scheduler could not wake it. Recheck the pipe
         * before marking this scope parked so that this call cannot miss that
         * state change and sleep forever without another transfer edge. */
        if (msg_pipe_transfer_ready(p, is_send, size, wait_mode, &amount)) {
            uint32_t transfer_rc = msg_pipe_transfer(p, buffer, amount, resultp, is_send, &direct_wake);
            if (transfer_rc != 0u) {
                result = transfer_rc;
                break;
            }
            if (toptr) {
                sched_vtime_refresh();
                uint64_t now = sched_vtime_us();
                uint32_t remaining = now < end_time ? (uint32_t)(end_time - now) : 0u;
                MEM_W32(toptr, remaining);
            }
            result = 0u;
            break;
        }

        int timed_out = sched_wait_block(waiter_scope, remaining, has_timeout);
        if (sched_wait_take_result(waiter_scope, &result)) break;
        p = msg_pipe_find(uid);
        if (!p) { result = SCE_KERNEL_ERROR_WAIT_DELETE; break; }

        if (!is_send && p->reservation == waiter_scope) {
            amount = p->reserved;
            if (!sr_guest_span_writable(buffer, amount)) {
                result = SCE_KERNEL_ERROR_ILLEGAL_ADDR;
                break;
            }
            for (uint32_t i = 0; i < amount; i++)
                MEM_W8(buffer + i, p->data[(p->read_pos + i) % p->capacity]);
            p->read_pos = (p->read_pos + amount) % p->capacity;
            p->reserved = 0u;
            p->reservation = 0;
            if (resultp) MEM_W32(resultp, amount);
            if (toptr) {
                sched_vtime_refresh();
                uint64_t now = sched_vtime_us();
                MEM_W32(toptr, now < end_time ? (uint32_t)(end_time - now) : 0u);
            }
            result = 0u;
            break;
        }
        if (msg_pipe_transfer_ready(p, is_send, size, wait_mode, &amount)) {
            uint32_t transfer_rc = msg_pipe_transfer(p, buffer, amount, resultp, is_send, &direct_wake);
            if (transfer_rc != 0u) {
                result = transfer_rc;
                break;
            }
            if (toptr) {
                sched_vtime_refresh();
                uint64_t now = sched_vtime_us();
                uint32_t remaining = now < end_time ? (uint32_t)(end_time - now) : 0u;
                MEM_W32(toptr, remaining);
            }
            result = 0u;
            break;
        }

        if (timed_out) {
            if (toptr) MEM_W32(toptr, 0u);
            result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            break;
        }
    }
    /* A cancelled or deleted timed wait reports its remaining timeout, as the
     * public PSPAutotests threads/msgpipe cancel and delete cases show
     * (reference output, not measured here). */
    if (has_timeout && (result == SCE_KERNEL_ERROR_WAIT_CANCEL ||
                        result == SCE_KERNEL_ERROR_WAIT_DELETE)) {
        sched_vtime_refresh();
        uint64_t now = sched_vtime_us();
        MEM_W32(toptr, now < end_time ? (uint32_t)(end_time - now) : 0u);
    }
    /* All departures detach/reconsider through the same object hook. Only the
     * normal syscall boundary may preempt; owner abandonment never does here. */
    if (sched_wait_finish(waiter_scope) || direct_wake) sched_preempt();
    return result;
}

static uint32_t h_SendMsgPipe(CpuState *s) {
    return msg_pipe_blocking_common(s, 1, 0);
}

static uint32_t h_SendMsgPipeCB(CpuState *s) {
    return msg_pipe_blocking_common(s, 1, 1);
}

static uint32_t h_ReceiveMsgPipe(CpuState *s) {
    return msg_pipe_blocking_common(s, 0, 0);
}

static uint32_t h_ReceiveMsgPipeCB(CpuState *s) {
    return msg_pipe_blocking_common(s, 0, 1);
}

/* A waiter counts for Cancel and Refer only while it is its owner's active
 * wait (blocked, or readied to retry and not yet run), so a thread counts at
 * most once. An entry stays in p->waiters until its owner finishes, so one
 * already released with a result (TERMINAL) is no longer a waiter. A
 * callback-aware wait whose owner is running callbacks is paused, not
 * waiting: it neither counts nor is cancelled, and resumes waiting when the
 * callbacks return (the PPSSPP pause model; not measured here). A readied
 * waiter has not transferred yet, whereas the PSP serves a waiter directly
 * (docs/HARDWARE_ORACLE.md, PSP-B3-01), so a Cancel or Delete between the
 * wake and the retry still releases it without its bytes. */
static int msg_pipe_waiter_waiting(const MsgPipeWaiter *waiter) {
    SrWaitState state;
    uint32_t owner;
    return sched_wait_state(waiter->invocation, &state, &owner) &&
           (state == SR_WAIT_BLOCKED || state == SR_WAIT_NOTIFIED);
}

static void msg_pipe_count_waiters(const MsgPipe *p, uint32_t *send_out,
                                   uint32_t *receive_out) {
    uint32_t send = 0u, receive = 0u;
    for (int i = 0; i < p->nwaiters; i++) {
        if (!msg_pipe_waiter_waiting(&p->waiters[i])) continue;
        if (p->waiters[i].is_send) send++;
        else receive++;
    }
    *send_out = send;
    *receive_out = receive;
}

static uint32_t h_CancelMsgPipe(CpuState *s) {
    /* a0=uid, a1=numSendWaitThreads out, a2=numReceiveWaitThreads out (issue #1381).
     * Both output pointers may be NULL; a non-NULL one that is not writable
     * fails closed with ILLEGAL_ADDR before any waiter or buffer changes
     * (project choice, as in h_CancelReceiveMbx). Every counted sender and
     * receiver waiter is released with WAIT_CANCEL. The counts, the emptied
     * buffer and the remaining timeout a cancelled timed waiter reports follow
     * the public PSPAutotests threads/msgpipe cancel case and PPSSPP
     * (reference output and emulator consensus, not measured here). */
    MsgPipe *p = msg_pipe_find(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_MPPID;
    uint32_t sendp = A1;
    uint32_t receivep = A2;
    if ((sendp && !sr_guest_span_writable(sendp, 4u)) ||
        (receivep && !sr_guest_span_writable(receivep, 4u)))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t senders, receivers;
    msg_pipe_count_waiters(p, &senders, &receivers);
    if (sendp) MEM_W32(sendp, senders);
    if (receivep) MEM_W32(receivep, receivers);
    p->read_pos = 0u;
    p->write_pos = 0u;
    p->count = 0u;
    p->reserved = 0u;
    p->reservation = 0;
    if (getenv("SR_MSGLOG"))
        fprintf(stderr, "HLE: CancelMsgPipe uid=0x%x name='%s' send=%u receive=%u\n",
                p->uid, p->name, senders, receivers);
    /* No waiter: no wake result to latch. Completing an invocation records its
     * result without detaching it, so p->waiters is stable during the walk. */
    if (senders || receivers) {
        for (int i = 0; i < p->nwaiters; i++)
            if (msg_pipe_waiter_waiting(&p->waiters[i]))
                (void)sched_wait_complete(p->waiters[i].invocation,
                                          SCE_KERNEL_ERROR_WAIT_CANCEL);
        sched_preempt();
    }
    return 0;
}

/* SceKernelMppInfo (PSPSDK pspthreadman.h) is 56 bytes: size(0), name[32](4),
 * attr(36), bufSize(40), freeSize(44), numSendWaitThreads(48),
 * numReceiveWaitThreads(52). */
typedef struct {
    uint32_t size;
    char     name[32];
    uint32_t attr;
    int32_t  bufSize;
    int32_t  freeSize;
    int32_t  numSendWaitThreads;
    int32_t  numReceiveWaitThreads;
} SceKernelMppInfo;

static uint32_t h_ReferMsgPipeStatus(CpuState *s) {
    /* a0=uid, a1=info (issue #1381). Same size-field contract as
     * h_ReferSemaStatus: a zero size writes nothing, otherwise
     * min(size, sizeof) bytes are written after the whole span is checked. */
    uint32_t info_addr = A1;
    if (!info_addr || !sr_guest_span_readable(info_addr, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    MsgPipe *p = msg_pipe_find(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_MPPID;
    uint32_t input_size = MEM_R32(info_addr);
    if (input_size == 0u) return 0;
    uint32_t write_len = input_size < (uint32_t)sizeof(SceKernelMppInfo)
                         ? input_size : (uint32_t)sizeof(SceKernelMppInfo);
    if (!sr_guest_span_writable(info_addr, write_len))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t senders, receivers;
    msg_pipe_count_waiters(p, &senders, &receivers);
    SceKernelMppInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(SceKernelMppInfo);
    /* Copy only up to the terminator: create keeps the bytes after it as they
     * were on the host stack, and they must not reach the guest. */
    for (size_t i = 0; i + 1u < sizeof(info.name) && p->name[i]; i++)
        info.name[i] = p->name[i];
    info.attr = p->attr;
    info.bufSize = (int32_t)p->capacity;
    info.freeSize = (int32_t)(p->capacity - p->count - p->reserved);
    info.numSendWaitThreads = (int32_t)senders;
    info.numReceiveWaitThreads = (int32_t)receivers;
    for (uint32_t i = 0; i < write_len; i++)
        MEM_W8(info_addr + i, ((const uint8_t *)&info)[i]);
    return 0;
}

/* Message-pipe NID registrations.  Shared by the production registry and the
 * executable HLE selftest so the test exercises the exact production mapping
 * rather than a duplicate registration list. */
void hle_register_msgpipe_handlers(void) {
    sr_hle_register(0x7c0dc2a0, "sceKernelCreateMsgPipe", h_CreateMsgPipe);
    sr_hle_register(0xf0b7da1c, "sceKernelDeleteMsgPipe", h_DeleteMsgPipe);
    sr_hle_register(0x884c9f90, "sceKernelTrySendMsgPipe", h_TrySendMsgPipe);
    sr_hle_register(0xdf52098f, "sceKernelTryReceiveMsgPipe", h_TryReceiveMsgPipe);
    sr_hle_register(0x876dbfad, "sceKernelSendMsgPipe", h_SendMsgPipe);
    sr_hle_register(0x7c41f2c2, "sceKernelSendMsgPipeCB", h_SendMsgPipeCB);
    sr_hle_register(0x74829b76, "sceKernelReceiveMsgPipe", h_ReceiveMsgPipe);
    sr_hle_register(0xfbfa697d, "sceKernelReceiveMsgPipeCB", h_ReceiveMsgPipeCB);
    sr_hle_register(0x349b864d, "sceKernelCancelMsgPipe", h_CancelMsgPipe);
    sr_hle_register(0x33be4024, "sceKernelReferMsgPipeStatus", h_ReferMsgPipeStatus);
}
#define SCE_KERNEL_ERROR_UNKNOWN_MBXID 0x8002019bu
#define SCE_KERNEL_ERROR_MBX_EMPTY     0x800201b2u
#define PSP_MBX_ATTR_PRIORITY          0x400u

/* Rewrite guest-visible circular next links so first->...->last->first matches
 * host delivery order (HARDWARE_ORACLE.md: FIFO mailboxes link packets circularly). */
static void mbx_relink(Mbx *m) {
    if (m->nmsgs <= 0) return;
    for (int i = 0; i < m->nmsgs; i++)
        MEM_W32(m->msgs[i] + 0, m->msgs[(i + 1) % m->nmsgs]);
}

void mbx_remove_waiter(Mbx *m, uint32_t thid) {
    for (int i = 0; i < m->nwaiters; i++) {
        if (m->waiters[i] == thid) {
            for (int j = i; j + 1 < m->nwaiters; j++) m->waiters[j] = m->waiters[j + 1];
            m->nwaiters--;
            break;
        }
    }
}

/* Attr 0x400 delivers by ascending msgPriority, FIFO among equals; otherwise append. */
static void mbx_enqueue(Mbx *m, uint32_t msg) {
    if (m->nmsgs >= MBX_MAX_MSGS) return;
    int pos = m->nmsgs;
    if (m->attr & PSP_MBX_ATTR_PRIORITY) {
        int32_t pri = (int32_t)MEM_R32(msg + 4);
        for (int i = 0; i < m->nmsgs; i++) {
            if (pri < (int32_t)MEM_R32(m->msgs[i] + 4)) { pos = i; break; }
        }
    }
    for (int j = m->nmsgs; j > pos; j--) m->msgs[j] = m->msgs[j - 1];
    m->msgs[pos] = msg;
    m->nmsgs++;
    mbx_relink(m);
}

static uint32_t mbx_dequeue(Mbx *m) {
    if (m->nmsgs <= 0) return 0;
    uint32_t msg = m->msgs[0];
    for (int i = 0; i + 1 < m->nmsgs; i++) m->msgs[i] = m->msgs[i + 1];
    m->nmsgs--;
    mbx_relink(m);
    return msg;
}

static uint32_t h_CreateMbx(CpuState *s) {
    /* a0=name, a1=attr, a2=option (PSPSDK SceKernelMbxOptParam, unused). */
    (void)A2;
    char name[32];
    name[0] = '\0';
    if (A0) {
        if (!guest_cstr(A0, name, sizeof(name))) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    for (int i = 0; i < MBX_MAX; i++) {
        Mbx *m = &s_mbx[i];
        if (m->used) continue;
        memset(m, 0, sizeof *m);
        m->used = 1;
        m->uid = sr_alloc_uid();
        m->attr = A1;
        memcpy(m->name, name, sizeof m->name);
        return m->uid;
    }
    return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */
}

static uint32_t h_DeleteMbx(CpuState *s) {
    Mbx *m = mbx_find(A0);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MBXID;
    int waiters = m->nwaiters;
    m->used = 0;
    if (waiters) {
        sched_wake_with_result(A0, SCE_KERNEL_ERROR_WAIT_DELETE);
        sched_preempt();
    }
    return 0;
}

static uint32_t h_SendMbx(CpuState *s) {
    Mbx *m = mbx_find(A0);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MBXID;
    uint32_t msg = A1;
    /* Validate the whole accessed packet span: the circular relink writes
     * next@0 and priority mailboxes read msgPriority@4. */
    if (!msg || !sr_guest_span_writable(msg, 8u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (m->nmsgs >= MBX_MAX_MSGS) return 0x80020190u;
    mbx_enqueue(m, msg);
    if (m->nwaiters > 0) {
        uint32_t thid = m->waiters[0];
        for (int j = 0; j + 1 < m->nwaiters; j++) m->waiters[j] = m->waiters[j + 1];
        m->nwaiters--;
        sched_wake_one_object_waiter(A0, thid);
        sched_preempt();
    }
    return 0;
}

static uint32_t h_PollMbx(CpuState *s) {
    Mbx *m = mbx_find(A0);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MBXID;
    uint32_t pmsg = A1;
    if (!pmsg || !sr_guest_span_writable(pmsg, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (m->nmsgs <= 0) return SCE_KERNEL_ERROR_MBX_EMPTY;
    MEM_W32(pmsg, mbx_dequeue(m));
    return 0;
}

static uint32_t receive_mbx_common(CpuState *s, int is_cb) {
    uint32_t uid = A0, pmsg = A1, toptr = A2;
    Mbx *m = mbx_find(uid);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MBXID;
    if (!pmsg || !sr_guest_span_writable(pmsg, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    if (m->nmsgs > 0) {
        MEM_W32(pmsg, mbx_dequeue(m));
        return 0;
    }

    uint64_t end_time = 0;
    int has_timeout = 0;
    if (toptr) {
        if (!sr_guest_span_writable(toptr, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        uint32_t usec = MEM_R32(toptr);
        if (usec == 0) {
            MEM_W32(toptr, 0u);
            return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        }
        sched_vtime_refresh();
        end_time = sched_vtime_deadline_after((uint64_t)usec);
        has_timeout = 1;
    }

    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;

    uint32_t cur_thid = sched_current_uid();
    if (cur_thid == 0) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    if (m->nwaiters >= MBX_MAX_WAITERS) return 0x80020190u;
    m->waiters[m->nwaiters++] = cur_thid;

    for (;;) {
        if (is_cb && sr_thread_has_pending_callbacks(cur_thid)) {
            sr_thread_dispatch_callbacks();
            m = mbx_find(uid);
            if (!m) return SCE_KERNEL_ERROR_WAIT_DELETE;
            sched_vtime_refresh();
            continue;
        }

        sched_set_current_wait_kind(5); /* PSP waitType 5 = mailbox */

        if (has_timeout) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= end_time) {
                m = mbx_find(uid);
                if (m) mbx_remove_waiter(m, cur_thid);
                MEM_W32(toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
            uint32_t remaining = (uint32_t)(end_time - now);
            if (is_cb) sched_set_current_cb_wait(1);
            int timed_out = sched_block_on_timeout(uid, remaining);
            if (is_cb) sched_set_current_cb_wait(0);

            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) {
                m = mbx_find(uid);
                if (m) mbx_remove_waiter(m, cur_thid);
                if (woken == SCE_KERNEL_ERROR_WAIT_DELETE) {
                    /* PSP-measured 2026-10-15 (PSP-3000, 6.61): a timed waiter woken by DeleteMbx
                     * rewrites its timeout word to the remaining time (499950 of 500000 us). Other
                     * wake results are not measured for this word and leave it unchanged. */
                    sched_vtime_refresh();
                    uint64_t now_us = sched_vtime_us();
                    MEM_W32(toptr, now_us < end_time ? (uint32_t)(end_time - now_us) : 0u);
                }
                return woken;
            }

            m = mbx_find(uid);
            if (!m) return SCE_KERNEL_ERROR_WAIT_DELETE;

            if (m->nmsgs > 0) {
                mbx_remove_waiter(m, cur_thid);
                MEM_W32(pmsg, mbx_dequeue(m));
                sched_vtime_refresh();
                now = sched_vtime_us();
                uint32_t rem = (now < end_time) ? (uint32_t)(end_time - now) : 0u;
                MEM_W32(toptr, rem);
                return 0;
            }

            if (timed_out) {
                mbx_remove_waiter(m, cur_thid);
                MEM_W32(toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
            /* Spurious wake: requeue at head so FIFO order is preserved. */
            mbx_remove_waiter(m, cur_thid);
            if (m->nwaiters < MBX_MAX_WAITERS) {
                for (int j = m->nwaiters; j > 0; j--) m->waiters[j] = m->waiters[j - 1];
                m->waiters[0] = cur_thid;
                m->nwaiters++;
            }
        } else {
            if (is_cb) sched_set_current_cb_wait(1);
            sched_block_on(uid);
            if (is_cb) sched_set_current_cb_wait(0);

            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) {
                m = mbx_find(uid);
                if (m) mbx_remove_waiter(m, cur_thid);
                return woken;
            }

            m = mbx_find(uid);
            if (!m) return SCE_KERNEL_ERROR_WAIT_DELETE;

            if (m->nmsgs > 0) {
                mbx_remove_waiter(m, cur_thid);
                MEM_W32(pmsg, mbx_dequeue(m));
                return 0;
            }

            mbx_remove_waiter(m, cur_thid);
            if (m->nwaiters < MBX_MAX_WAITERS) {
                for (int j = m->nwaiters; j > 0; j--) m->waiters[j] = m->waiters[j - 1];
                m->waiters[0] = cur_thid;
                m->nwaiters++;
            }
        }
    }
}

static uint32_t h_ReceiveMbx(CpuState *s) { return receive_mbx_common(s, 0); }
static uint32_t h_ReceiveMbxCB(CpuState *s) { return receive_mbx_common(s, 1); }

static uint32_t h_CancelReceiveMbx(CpuState *s) {
    Mbx *m = mbx_find(A0);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MBXID;
    uint32_t nump = A1;
    if (nump && !sr_guest_span_writable(nump, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    int waiters = m->nwaiters;
    m->nwaiters = 0;
    if (nump) MEM_W32(nump, (uint32_t)waiters);
    if (waiters) {
        sched_wake_with_result(A0, SCE_KERNEL_ERROR_WAIT_CANCEL);
        sched_preempt();
    }
    return 0;
}

/* sceKernelReferMbxStatus: SceKernelMbxInfo is 52 bytes — size(0), name[32](4),
 * attr(36), numWaitThreads(40), numMessages(44), firstMessage(48). */
static uint32_t h_ReferMbxStatus(CpuState *s) {
    Mbx *m = mbx_find(A0);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MBXID;
    uint32_t info = A1;
    if (!info || !sr_guest_span_writable(info, 52u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    MEM_W32(info + 0, 52u);
    for (int i = 0; i < 32; i++) MEM_W8(info + 4 + (uint32_t)i, 0);
    for (int i = 0; i < 31 && m->name[i]; i++) MEM_W8(info + 4 + (uint32_t)i, (uint8_t)m->name[i]);
    MEM_W32(info + 36, m->attr);
    MEM_W32(info + 40, (uint32_t)m->nwaiters);
    MEM_W32(info + 44, (uint32_t)m->nmsgs);
    MEM_W32(info + 48, m->nmsgs ? m->msgs[0] : 0u);
    return 0;
}

/* Shared by production and the executable HLE selftest so both dispatch the
 * same NID→handler mapping (issue #339). */
void hle_register_mbx_handlers(void) {
    sr_hle_register(0x8125221d, "sceKernelCreateMbx", h_CreateMbx);
    sr_hle_register(0x86255ada, "sceKernelDeleteMbx", h_DeleteMbx);
    sr_hle_register(0xe9b3061e, "sceKernelSendMbx", h_SendMbx);
    sr_hle_register(0x18260574, "sceKernelReceiveMbx", h_ReceiveMbx);
    sr_hle_register(0xf3986382, "sceKernelReceiveMbxCB", h_ReceiveMbxCB);
    sr_hle_register(0x0d81716a, "sceKernelPollMbx", h_PollMbx);
    sr_hle_register(0x87d4dd36, "sceKernelCancelReceiveMbx", h_CancelReceiveMbx);
    sr_hle_register(0xa8e8c846, "sceKernelReferMbxStatus", h_ReferMbxStatus);
}
static Sync *sync_find(uint32_t uid, int kind) {
    for (int i = 0; i < 128; i++)
        if (s_sync[i].used && s_sync[i].uid == uid && s_sync[i].kind == kind) return &s_sync[i];
    return NULL;
}
/* The slot is cleared, so no field of a previous object (of any kind) survives
 * into the new one. */
static Sync *sync_new(int kind) {
    for (int i = 0; i < 128; i++)
        if (!s_sync[i].used) {
            memset(&s_sync[i], 0, sizeof(s_sync[i]));
            s_sync[i].used = 1;
            s_sync[i].kind = kind;
            s_sync[i].uid = sr_alloc_uid();
            return &s_sync[i];
        }
    return NULL;
}

static uint32_t h_CreateSema(CpuState *s) {
    /* a0=name, a1=attr, a2=initCount, a3=maxCount.
     * PSP-B1-01 (psp-hw-20260917): attr bits outside the documented set
     * (anything beyond 0x3FF) reject with 80020191. initCount > maxCount and
     * maxCount == 0 are accepted on hardware, so no validation is added for
     * either. */
    if (A1 & ~0x3ffu) return 0x80020191u;
    Sync *m = sync_new(SYNC_KIND_SEMA); if (!m) return 0x80020000;
    m->count = (int)A2; m->maxc = (int)A3;
    /* PSP-B2-01 (psp-hw-20260917): CancelSema(-1) resets the count to the
     * create-time initial count, not to zero. */
    m->initc = (int)A2;
    /* Create-time name and attr for ReferSemaStatus (an unreadable name
     * pointer yields an empty name, never a failed create). */
    m->attr = A1;
    guest_cstr(A0, m->name, sizeof(m->name));
    if (hle_log_on())
        fprintf(stderr, "HLE: CreateSema uid=0x%x init=%d max=%d (from uid=0x%x)\n", m->uid, (int)A2, (int)A3, sched_current_uid());
    return m->uid;
}

static uint32_t h_DeleteSema(CpuState *s) {
    /* PSP-B1-01 (psp-hw-20260917): unknown or deleted semaphore id is
     * UNKNOWN_SEMID. */
    Sync *m = sync_find(A0, SYNC_KIND_SEMA); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    /* PSP-B2-01 (psp-hw-20260917): deleting a semaphore with a waiter succeeds
     * and the waiter leaves with WAIT_DELETE (0x800201B5). With no waiter there
     * is nothing to wake and no wake result to latch (a stale result would be
     * misattributed to the next waiter). */
    int waiters = sched_count_waiters(A0);
    m->used = 0;
    if (waiters) {
        sched_wake_with_result(A0, 0x800201b5u);
        sched_preempt();
    }
    return 0;
}
/* Entry contract shared by sceKernelWaitSema and sceKernelWaitSemaCB.
 *
 * The order below is not a style choice; each step is placed where a measured
 * oracle cell puts it, and moving any one of them breaks a cell:
 *
 *  1. CONTEXT.  tests/intr/waits.expected probes this API with a bad id and with
 *     an invalid count, and gets CAN_NOT_WAIT for BOTH with interrupts disabled
 *     (L54, L56 / CB L62, L64) and with dispatch disabled (L55, L57 / CB L63,
 *     L65).  In normal context those same two calls answer 80020199 and
 *     800201BD (wait.expected L21/L23, L3/L13), so the context decision must
 *     come ahead of the object lookup AND ahead of count validation.  Being
 *     ahead of the lookup necessarily also puts it ahead of the availability
 *     test -- there is no object to test availability on yet.  That last step is
 *     forced by the measured cells rather than measured directly: no fixture
 *     calls WaitSema on an immediately-available count with a context disabled.
 *     PPSSPP takes the same order, as corroboration only.
 *
 *     This deliberately reverses the earlier "only once it would genuinely
 *     block" placement, which was chosen when the bad-object cells were still
 *     pinned as known deviations.
 *
 *  2. OBJECT.  Unknown or already-deleted id -> UNKNOWN_SEMID, replacing the
 *     generic 0x80020000 seam (wait.expected L21/L23/L25).
 *
 *  3. COUNT.  need <= 0 or need > maxCount -> ILLEGAL_COUNT, immediately.  need
 *     == maxCount is legal (L1: need 1 against max 1 succeeds).
 *
 *     `need` is read as a signed int precisely so a negative request stays
 *     negative here.  Comparing A1 unsigned would turn -1 into 4294967295 and
 *     route it through the same path as any oversized request; it would reach
 *     ILLEGAL_COUNT by luck, and the old `m->count -= need` would have ADDED to
 *     the semaphore.  wait.expected L5/L6 and L15/L16 pin both halves: the call
 *     fails, and the following ReferSemaStatus still reports cur=0.
 *
 *  4. AVAILABILITY / BLOCKING, in the callers.
 *
 * Step 3 running before any mutation is what makes the rejection total: no
 * decrement, no waiter, no timeout word written, and for the CB form no clock
 * sample and no callback dispatch.  wait.expected L11 and L13 show the supplied
 * timeout still holding its original 500ms after an oversized and a zero
 * request, so a rejection may not touch *toptr either.
 *
 * The combined case -- unknown id AND invalid count in the same call -- is NOT
 * pinned by any cited hardware cell.  Object-before-count is what the public
 * reference set supports; treat that single ordering as corroborated, not
 * measured.  See docs/PSP_INTR_WAITS_MATRIX.md. */
static uint32_t sema_wait_entry(const char *who, uint32_t uid, int need, Sync **out) {
    uint32_t err = 0;
    Sync *m = NULL;
    if (!sched_wait_permitted()) {
        err = SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    } else if ((m = sync_find(uid, SYNC_KIND_SEMA)) == NULL) {
        err = SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    } else if (need <= 0 || need > m->maxc) {
        err = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    }
    if (err) {
        if (hle_log_on())
            fprintf(stderr, "HLE: %s uid=0x%x need=%d rejected 0x%08x (from 0x%x)\n",
                    who, uid, need, err, sched_current_uid());
        return err;
    }
    *out = m;
    return 0;
}

static uint32_t h_WaitSema(CpuState *s) {
    /* a0=semaid, a1=signal, a2=timeout ptr (0=infinite, else *a2 = microseconds). */
    uint32_t uid = A0; int need = (int)A1; uint32_t toptr = A2;
    Sync *m = NULL;
    uint32_t err = sema_wait_entry("WaitSema", uid, need, &m);
    if (err) return err;
    if (hle_log_on())
        fprintf(stderr, "HLE: WaitSema uid=0x%x count=%d need=%d (from 0x%x)\n", uid, m->count, need, sched_current_uid());
    /* The deadline is fixed by the first block; a wake that finds the count taken
     * by another thread re-waits only for what is left, and a released waiter that
     * resumes after the deadline times out instead of starting a fresh full wait. */
    uint64_t wait_end = 0;
    int wait_end_set = 0;
    while (m->count < need) {
        /* PSP-B3-01 (psp-hw-20260917): ReferThreadStatus reports sema waits as
         * waitType 3 with waitId = the semaphore UID. */
        sched_set_current_wait_kind(3);
        if (toptr) {
            uint32_t usec = MEM_R32(toptr);
            if (!wait_end_set) {
                sched_vtime_refresh();
                wait_end = sched_vtime_deadline_after((uint64_t)usec);
                wait_end_set = 1;
            } else {
                sched_vtime_refresh();
                uint64_t now = sched_vtime_us();
                usec = now >= wait_end ? 0u : (uint32_t)(wait_end - now);
                if (usec == 0u) {
                    MEM_W32(toptr, 0u);
                    return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
                }
            }
            if (sched_block_on_timeout(uid, usec)) {
                /* PSP-B2-01 (psp-hw-20260917): an expired timed wait writes the
                 * remaining time (0 at the deadline) to *timeout and answers
                 * WAIT_TIMEOUT. */
                MEM_W32(toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
        } else {
            sched_block_on(uid);
        }
        /* PSP-B2-01 (psp-hw-20260917): a cancelled/deleted/released wait answers
         * its wake result instead of resuming into a satisfied count. */
        {
            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) return woken;
        }
        m = sync_find(uid, SYNC_KIND_SEMA); if (!m) return 0x80020000;
    }
    m->count -= need;
    return 0;
}
static uint32_t h_WaitSemaCB(CpuState *s) {
    uint32_t uid = A0; int need = (int)A1; uint32_t toptr = A2;
    Sync *m = NULL;
    /* Ahead of sched_vtime_refresh() and the callback loop: a rejected call must
     * not sample the clock, run callbacks or write a remaining-timeout word. */
    uint32_t err = sema_wait_entry("WaitSemaCB", uid, need, &m);
    if (err) return err;
    if (hle_log_on())
        fprintf(stderr, "HLE: WaitSemaCB uid=0x%x count=%d need=%d (from 0x%x)\n", uid, m->count, need, sched_current_uid());

    sched_vtime_refresh();
    uint64_t start = sched_vtime_us();
    uint64_t end = start + (toptr ? MEM_R32(toptr) : 0);

    while (m->count < need) {
        if (sr_thread_has_pending_callbacks(sched_current_uid())) {
            sr_thread_dispatch_callbacks();
            m = sync_find(uid, SYNC_KIND_SEMA); if (!m) return 0x80020000;
            sched_vtime_refresh();
            continue;
        }
        if (toptr) {
            sched_vtime_refresh();
            if (sched_vtime_us() >= end) {
                /* PSP-B2-01 (psp-hw-20260917): the expired wait reports its
                 * remaining timeout (0), it does not leave the caller's word. */
                MEM_W32(toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
            uint32_t remaining = (uint32_t)(end - sched_vtime_us());
            MEM_W32(toptr, remaining);
            sched_set_current_cb_wait(1);
            /* PSP-B3-01 (psp-hw-20260917): sema CB waits report waitType 3. */
            sched_set_current_wait_kind(3);
            int timed_out = sched_block_on_timeout(uid, remaining);
            sched_set_current_cb_wait(0);
            if (timed_out) {
                /* PSP-B2-01 (psp-hw-20260917): same remaining-0 contract after
                 * a real timed block. */
                MEM_W32(toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
        } else {
            sched_set_current_cb_wait(1);
            sched_set_current_wait_kind(3);
            sched_block_on(uid);
            sched_set_current_cb_wait(0);
        }
        /* PSP-B2-01 (psp-hw-20260917): a cancelled/deleted/released wait answers
         * its wake result instead of resuming into a satisfied count. */
        {
            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) return woken;
        }
        m = sync_find(uid, SYNC_KIND_SEMA); if (!m) return 0x80020000;
        sched_vtime_refresh();
    }
    m->count -= need;
    return 0;
}
static uint32_t h_SignalSema(CpuState *s) {
    /* PSP-B1-01 (psp-hw-20260917): unknown or deleted semaphore id is
     * UNKNOWN_SEMID, replacing the generic 0x80020000 seam. */
    Sync *m = sync_find(A0, SYNC_KIND_SEMA); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    int signals = (int)A1;
    int64_t new_count = (int64_t)m->count + signals;
    /* PSP-B1-01 (psp-hw-20260917): an overflow past maxCount rejects with
     * 800201AE and leaves the count unchanged; it does not cap. */
    if (new_count > m->maxc) return 0x800201aeu;
    m->count = (int32_t)(uint32_t)new_count;
    sched_wake(A0);
    sched_preempt();    /* a woken higher-priority waiter runs immediately */
    return 0;
}
/* Unchanged behavior: the two literals below are now the same named constants the
 * blocking forms use, so the family cannot drift to two different spellings of
 * one error. sceKernelPollSema has no context gate because it never blocks. */
static uint32_t h_PollSema(CpuState *s) {
    Sync *m = sync_find(A0, SYNC_KIND_SEMA); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    int need = (int)A1;
    /* PSP-B1-01 (psp-hw-20260917): count validation answers ILLEGAL_COUNT even
     * on an empty semaphore; SEMA_ZERO is only for a valid count it cannot
     * satisfy. */
    if (need <= 0 || need > m->maxc) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (m->count < need) return 0x800201adu;   /* SCE_KERNEL_ERROR_SEMA_ZERO */
    m->count -= need;
    return 0;
}
/* sceKernelReferSemaStatus(semaid, SceKernelSemaInfo *info). The 56-byte struct is
 * size(0), name[32](4), attr(36), initCount(40), currentCount(44), maxCount(48),
 * numWaitThreads(52). Follows h_ReferMutexStatus: the caller's size word at info+0 bounds
 * the write. Size 0 writes nothing and succeeds; otherwise only min(size, 56) bytes reach
 * guest memory, copied from a locally built struct whose size field is 56. */
typedef struct {
    uint32_t size;
    char     name[32];
    uint32_t attr;
    int32_t  initCount;
    int32_t  currentCount;
    int32_t  maxCount;
    int32_t  numWaitThreads;
} SceKernelSemaInfo;

static uint32_t h_ReferSemaStatus(CpuState *s) {
    uint32_t uid = A0;
    uint32_t info_addr = A1;
    if (!info_addr || !sr_guest_span_readable(info_addr, 4))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    Sync *m = sync_find(uid, SYNC_KIND_SEMA);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    uint32_t input_size = MEM_R32(info_addr);
    if (input_size == 0) return 0;
    uint32_t write_len = input_size < (uint32_t)sizeof(SceKernelSemaInfo)
                         ? input_size : (uint32_t)sizeof(SceKernelSemaInfo);
    if (!sr_guest_span_writable(info_addr, write_len))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    SceKernelSemaInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(SceKernelSemaInfo);
    memcpy(info.name, m->name, sizeof(info.name));
    info.attr = m->attr;
    info.initCount = m->initc;
    info.currentCount = m->count;
    info.maxCount = m->maxc;
    info.numWaitThreads = sched_count_waiters(uid);
    for (uint32_t i = 0; i < write_len; i++)
        MEM_W8(info_addr + i, ((const uint8_t *)&info)[i]);
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Read-only semaphore probe for the executable HLE selftest (issue #43).
 *
 * The count-validation regressions must assert that a REJECTED wait mutated
 * nothing, and no registered NID can observe the count without changing it:
 * sceKernelPollSema decrements on success and sceKernelReferSemaStatus is not
 * registered. This reads the real production Sync entry and is compiled only
 * into the test executable. It adds no field and changes no layout. */
int sr_hle_test_sema_state(uint32_t uid, int *count_out, int *max_out) {
    Sync *m = sync_find(uid, SYNC_KIND_SEMA);
    if (!m) return 0;
    if (count_out) *count_out = m->count;
    if (max_out)   *max_out   = m->maxc;
    return 1;
}
#endif /* SR_HLE_THREAD_SELFTEST */

/* ---------------------------------------------------------------------------
 * Lightweight mutexes.
 *
 * These were previously registered as no-ops (Create -> h_CreateSema, every
 * other entry -> h_ok) under a comment justifying it as "single-threaded
 * recompiler".  That premise is false: this runtime schedules real PSP threads
 * cooperatively (src/rt/sched.c), and a retained boot trace shows two distinct
 * guest threads locking the same LwMutex, so the no-op registration silently
 * removed mutual exclusion between them.
 *
 * The old registration was also wrong on the ABI.  sceKernelCreateLwMutex takes
 * (workarea, name, attr, initialCount, opt), so routing it to h_CreateSema --
 * which reads (name, attr, initCount, maxCount) -- shifted every argument by
 * one register: the semaphore was created with count=attr and max=initialCount.
 * It returned the uid where the API returns 0 on success, and it never wrote
 * the caller's workarea at all.
 *
 * The workarea is the whole point of an LwMutex: it lives in guest memory and
 * the guest reads lockLevel/lockThread/uid out of it directly.  Layout is fixed
 * by pspthreadman.h (SceLwMutexWorkarea):
 *
 *     +0x00 int   lockLevel        recursion count; 0 == unlocked
 *     +0x04 SceUID lockThread      owning thread uid; 0 when unlocked
 *     +0x08 int   attr
 *     +0x0c int   numWaitThreads
 *     +0x10 SceUID uid             kernel object id
 *     +0x14 int   pad[3]
 *
 * Error returns for the abusive cases are the physical-PSP measurements of
 * runs PSP-B1-01 and PSP-B2-01 (campaign psp-hw-20260917, PSP-3000 / 6.61):
 *   - TryLock that cannot own (another owner, a self-held non-recursive
 *     mutex, or count 0): 0x800201C4.
 *   - Timed Lock of a self-held non-recursive mutex: 0x800201CF at once,
 *     without waiting or consuming the timeout.
 *   - Unlock by a non-owner, or of an unlocked mutex: 0x800201CC, state
 *     untouched. Unlock count 0: 0x800201BD.
 *   - Delete writes 0xFFFFFFFF to lockThread and uid; a second Delete
 *     returns 0x800201CA.
 *   - Create of a non-recursive mutex with initialCount > 1: 0x800201BD.
 * Cases those runs did not measure (a negative count, an owner unlocking more
 * than it holds, Lock with count 0) keep their previous behaviour.
 * ------------------------------------------------------------------------- */
#define LWMUTEX_WORKAREA_SIZE 0x20u
#define LWMUTEX_LOCK_LEVEL    0x00u
#define LWMUTEX_LOCK_THREAD   0x04u
#define LWMUTEX_ATTR          0x08u
#define LWMUTEX_NUM_WAIT      0x0cu
#define LWMUTEX_UID           0x10u
#define LWMUTEX_ATTR_RECURSIVE 0x0200u
#define LWMUTEX_DELETED_WORD   0xffffffffu
#define SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND           0x800201cau
#define SCE_KERNEL_ERROR_LWMUTEX_FAILED_TO_OWN       0x800201c4u
#define SCE_KERNEL_ERROR_LWMUTEX_LOCKED              0x800201cbu
#define SCE_KERNEL_ERROR_LWMUTEX_UNLOCK_UNDERFLOW    0x800201ccu
#define SCE_KERNEL_ERROR_LWMUTEX_RECURSIVE_NOT_ALLOWED 0x800201cfu

/* PSP-B2-01 (psp-hw-20260917): an unlock to level 0 with a waiter hands
 * ownership directly to the waiter (lockThread = waiter, level = the waiter's
 * count) before the waiter runs. The waiter's requested count lives on its own
 * host stack while blocked, so a small FIFO queue carries it to the unlocker. */
#define LWWAIT_MAX 64
typedef struct { uint32_t muid, thread; int count; int active; } LwWait;
static LwWait s_lwwait[LWWAIT_MAX];
static void lww_enqueue(uint32_t muid, uint32_t thread, int count) {
    for (int i = 0; i < LWWAIT_MAX; i++)
        if (!s_lwwait[i].active) {
            s_lwwait[i] = (LwWait){muid, thread, count, 1};
            return;
        }
}
static void lww_remove_thread(uint32_t thread) {
    for (int i = 0; i < LWWAIT_MAX; i++)
        if (s_lwwait[i].active && s_lwwait[i].thread == thread) s_lwwait[i].active = 0;
}
static void lww_remove_mutex(uint32_t muid) {
    for (int i = 0; i < LWWAIT_MAX; i++)
        if (s_lwwait[i].active && s_lwwait[i].muid == muid) s_lwwait[i].active = 0;
}
static int lww_pop_first(uint32_t muid, uint32_t *thread_out, int *count_out) {
    for (int i = 0; i < LWWAIT_MAX; i++)
        if (s_lwwait[i].active && s_lwwait[i].muid == muid) {
            *thread_out = s_lwwait[i].thread;
            *count_out = s_lwwait[i].count;
            s_lwwait[i].active = 0;
            return 1;
        }
    return 0;
}

/* The guest hands us a pointer; validate the entire struct, not just its first
 * word, before any access.  A partially-mapped workarea must be refused. */
static int lwmutex_workarea_ok(uint32_t wa) {
    return wa && sr_guest_span_writable(wa, LWMUTEX_WORKAREA_SIZE);
}

static uint32_t h_CreateLwMutex(CpuState *s) {
    /* a0=workarea, a1=name, a2=attr, a3=initialCount, [sp+16]=opt. */
    uint32_t wa = A0, attr = A2;
    int initial = (int)A3;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;   /* ILLEGAL_ADDR */
    /* PSP-B1-01: a non-recursive mutex cannot start with more than one lock. */
    if (!(attr & LWMUTEX_ATTR_RECURSIVE) && initial > 1) return 0x800201bdu;
    Sync *m = sync_new(SYNC_KIND_LWMUTEX); if (!m) return 0x80020000;
    m->workarea = wa;
    m->attr = attr;
    m->initc = initial;
    if (A1) guest_cstr(A1, m->name, sizeof(m->name));
    else m->name[0] = '\0';
    MEM_W32(wa + LWMUTEX_LOCK_LEVEL, (uint32_t)initial);
    MEM_W32(wa + LWMUTEX_LOCK_THREAD, initial > 0 ? sched_current_uid() : 0u);
    MEM_W32(wa + LWMUTEX_ATTR, attr);
    MEM_W32(wa + LWMUTEX_NUM_WAIT, 0u);
    MEM_W32(wa + LWMUTEX_UID, m->uid);
    if (hle_log_on())
        fprintf(stderr, "HLE: CreateLwMutex wa=0x%08x uid=0x%x attr=0x%x initial=%d (from uid=0x%x)\n",
                wa, m->uid, attr, initial, sched_current_uid());
    return 0;   /* the API returns 0 on success; the uid goes in the workarea */
}

static uint32_t h_DeleteLwMutex(CpuState *s) {
    uint32_t wa = A0;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;
    uint32_t uid = MEM_R32(wa + LWMUTEX_UID);
    Sync *m = sync_find(uid, SYNC_KIND_LWMUTEX);
    /* PSP-B1-01: deleting an already-deleted workarea is NOT_FOUND. */
    if (!m) return SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND;
    m->used = 0;
    /* Anything still blocked here would wait forever otherwise. */
    lww_remove_mutex(uid);
    sched_wake(uid);
    /* PSP-B1-01: delete (even while held) leaves level 0 and poisons the
     * owner and uid words with 0xFFFFFFFF; attr is kept. */
    MEM_W32(wa + LWMUTEX_LOCK_LEVEL, 0u);
    MEM_W32(wa + LWMUTEX_LOCK_THREAD, LWMUTEX_DELETED_WORD);
    MEM_W32(wa + LWMUTEX_UID, LWMUTEX_DELETED_WORD);
    return 0;
}

/* SceKernelLwMutexInfo (64 bytes): public reference pspthreadman.h and PPSSPP
 * Core/HLE/sceKernelMutex.cpp (NativeLwMutex). */
typedef struct {
    uint32_t size;
    char name[32];
    uint32_t attr;
    uint32_t uid;
    uint32_t workarea;
    int32_t initialCount;
    int32_t currentCount;
    int32_t lockThread;
    int32_t numWaitThreads;
} SceKernelLwMutexInfo;

static uint32_t lwmutex_refer_status(Sync *m, uint32_t info_addr) {
    if (!info_addr || !sr_guest_span_readable(info_addr, 4))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t input_size = MEM_R32(info_addr);
    if (input_size == 0) return 0;
    uint32_t write_len = input_size < (uint32_t)sizeof(SceKernelLwMutexInfo) ? input_size : (uint32_t)sizeof(SceKernelLwMutexInfo);
    if (!sr_guest_span_writable(info_addr, write_len))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    SceKernelLwMutexInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(SceKernelLwMutexInfo);
    memcpy(info.name, m->name, sizeof(info.name));
    info.attr = m->attr;
    info.uid = m->uid;
    info.workarea = m->workarea;
    info.initialCount = m->initc;
    if (m->workarea && sr_guest_span_readable(m->workarea, LWMUTEX_WORKAREA_SIZE)) {
        info.currentCount = (int32_t)MEM_R32(m->workarea + LWMUTEX_LOCK_LEVEL);
        uint32_t th = MEM_R32(m->workarea + LWMUTEX_LOCK_THREAD);
        info.lockThread = th == 0 ? -1 : (int32_t)th;
        info.numWaitThreads = (int32_t)MEM_R32(m->workarea + LWMUTEX_NUM_WAIT);
    } else {
        info.currentCount = 0;
        info.lockThread = -1;
        info.numWaitThreads = 0;
    }
    for (uint32_t i = 0; i < write_len; i++) {
        MEM_W8(info_addr + i, ((const uint8_t *)&info)[i]);
    }
    return 0;
}

/* sceKernelReferLwMutexStatus(workarea, info): query lightweight mutex status by workarea. */
static uint32_t h_ReferLwMutexStatus(CpuState *s) {
    uint32_t wa = A0;
    uint32_t info_addr = A1;
    if (!lwmutex_workarea_ok(wa)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t uid = MEM_R32(wa + LWMUTEX_UID);
    Sync *m = sync_find(uid, SYNC_KIND_LWMUTEX);
    if (!m) return SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND;
    return lwmutex_refer_status(m, info_addr);
}

/* sceKernelReferLwMutexStatusByID(uid, info): query lightweight mutex status by UID. */
static uint32_t h_ReferLwMutexStatusByID(CpuState *s) {
    uint32_t uid = A0;
    uint32_t info_addr = A1;
    Sync *m = sync_find(uid, SYNC_KIND_LWMUTEX);
    if (!m) return SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND;
    return lwmutex_refer_status(m, info_addr);
}

/* Shared by Lock/TryLock/LockCB. `blocking` selects whether contention waits. */
enum {
    LWMUTEX_TAKEN = 0,
    LWMUTEX_BUSY,            /* contended and not blocking (TryLock) */
    LWMUTEX_NOT_RECURSIVE,   /* self-held without the RECURSIVE attribute */
    LWMUTEX_GONE,            /* deleted before or while waiting */
    LWMUTEX_TIMEOUT,         /* contended timed wait expired */
};
static int lwmutex_acquire_timeout(uint32_t wa, int count, int blocking, uint32_t toptr);
static int lwmutex_acquire(uint32_t wa, int count, int blocking) {
    return lwmutex_acquire_timeout(wa, count, blocking, 0u);
}
static int lwmutex_acquire_timeout(uint32_t wa, int count, int blocking, uint32_t toptr) {
    uint32_t cur = sched_current_uid();
    uint64_t end = 0;
    int has_timeout = 0;
    if (blocking && toptr) {
        if (!sr_guest_span_writable(toptr, 4)) return LWMUTEX_GONE;
        uint32_t usec = MEM_R32(toptr);
        if (usec == 0) return LWMUTEX_TIMEOUT;
        sched_vtime_refresh();
        end = sched_vtime_deadline_after((uint64_t)usec);
        has_timeout = 1;
    }
    for (;;) {
        if (!sync_find(MEM_R32(wa + LWMUTEX_UID), SYNC_KIND_LWMUTEX)) return LWMUTEX_GONE;
        int level = (int)MEM_R32(wa + LWMUTEX_LOCK_LEVEL);
        uint32_t owner = MEM_R32(wa + LWMUTEX_LOCK_THREAD);
        if (level == 0) {
            MEM_W32(wa + LWMUTEX_LOCK_LEVEL, (uint32_t)count);
            MEM_W32(wa + LWMUTEX_LOCK_THREAD, cur);
            return LWMUTEX_TAKEN;
        }
        if (owner == cur) {
            /* PSP-B1-01: a self-held non-recursive mutex fails at once, for
             * TryLock and for a timed Lock alike. */
            if (!(MEM_R32(wa + LWMUTEX_ATTR) & LWMUTEX_ATTR_RECURSIVE))
                return LWMUTEX_NOT_RECURSIVE;
            MEM_W32(wa + LWMUTEX_LOCK_LEVEL, (uint32_t)(level + count));
            return LWMUTEX_TAKEN;
        }
        if (!blocking) return LWMUTEX_BUSY;
        /* Contention accounting. Under the previous no-op registration every one
         * of these was a silently unserialised critical section, so the count is
         * the direct measure of what the no-op was costing. Bounded logging: the
         * first few, then sampled, so a hot mutex cannot distort the run it is
         * measuring. */
        {
            static unsigned long blocks = 0;
            if (++blocks <= 8u || (blocks % 1000u) == 0u)
                fprintf(stderr, "LWMUTEX_CONTENDED n=%lu wa=0x%08x owner=0x%x waiter=0x%x level=%d\n",
                        blocks, wa, owner, cur, level);
        }
        uint32_t uid = MEM_R32(wa + LWMUTEX_UID);
        MEM_W32(wa + LWMUTEX_NUM_WAIT, MEM_R32(wa + LWMUTEX_NUM_WAIT) + 1u);
        lww_enqueue(uid, cur, count);
        /* PSP-B3-01 (psp-hw-20260917): LwMutex waits report waitType 13 with
         * waitId = the workarea's uid. */
        sched_set_current_wait_kind(13);
        int timed_out = 0;
        if (has_timeout) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= end) {
                timed_out = 1;
            } else {
                uint32_t remaining = (uint32_t)(end - now);
                MEM_W32(toptr, remaining);
                timed_out = sched_block_on_timeout(uid, remaining);
            }
        } else {
            sched_block_on(uid);
        }
        uint32_t waiters = MEM_R32(wa + LWMUTEX_NUM_WAIT);
        if (waiters) MEM_W32(wa + LWMUTEX_NUM_WAIT, waiters - 1u);
        if (!sync_find(uid, SYNC_KIND_LWMUTEX)) { lww_remove_thread(cur); return LWMUTEX_GONE; }
        /* PSP-B2-01 (psp-hw-20260917): the unlocker hands ownership directly to
         * the waiter, so a woken waiter that now owns the mutex takes it. */
        if (MEM_R32(wa + LWMUTEX_LOCK_THREAD) == cur) {
            if (has_timeout) {
                sched_vtime_refresh();
                uint64_t now = sched_vtime_us();
                MEM_W32(toptr, now < end ? (uint32_t)(end - now) : 0u);
            }
            return LWMUTEX_TAKEN;
        }
        if (timed_out || (has_timeout && ({ sched_vtime_refresh(); sched_vtime_us() >= end; }))) {
            /* PSP-B2-01 (psp-hw-20260917): a contended timed lock answers
             * WAIT_TIMEOUT (0x800201A8) and reports the remaining timeout (0). */
            lww_remove_thread(cur);
            if (toptr) MEM_W32(toptr, 0u);
            return LWMUTEX_TIMEOUT;
        }
    }
}

static uint32_t h_lwmutex_lock_common(CpuState *s) {
    /* a0=workarea, a1=lockCount, a2=timeout ptr. */
    uint32_t wa = A0;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;
    int count = (int)A1; if (count <= 0) return 0x800200d2u;  /* ILLEGAL_ARGUMENT */
    uint32_t toptr = A2;
    if (toptr && !sr_guest_span_writable(toptr, 4)) return 0x80000103u;
    int rc = (toptr ? lwmutex_acquire_timeout(wa, count, 1, toptr)
                    : lwmutex_acquire(wa, count, 1));
    switch (rc) {
    case LWMUTEX_TAKEN: return 0u;
    case LWMUTEX_NOT_RECURSIVE: return SCE_KERNEL_ERROR_LWMUTEX_RECURSIVE_NOT_ALLOWED;
    case LWMUTEX_TIMEOUT: return 0x800201a8u;
    default: return 0x800201b5u;   /* WAIT_DELETE */
    }
}

static uint32_t h_LockLwMutex(CpuState *s) {
    return h_lwmutex_lock_common(s);
}

/* Callback-variant waits remain unreviewed until the callback wait probe has
 * measured them. Keep a distinct registered owner so the measured blocking
 * lock cells cannot implicitly graduate the CB imports. */
static uint32_t h_LockLwMutexCB(CpuState *s) {
    return h_lwmutex_lock_common(s);
}

static uint32_t h_TryLockLwMutex(CpuState *s) {
    uint32_t wa = A0;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;
    /* PSP-B1-01 / PSP-B2-01: count 0, a deleted mutex, another owner and a
     * self-held non-recursive mutex all report FAILED_TO_OWN. */
    int count = (int)A1; if (count <= 0) return SCE_KERNEL_ERROR_LWMUTEX_FAILED_TO_OWN;
    return lwmutex_acquire(wa, count, 0) == LWMUTEX_TAKEN
        ? 0u : SCE_KERNEL_ERROR_LWMUTEX_FAILED_TO_OWN;
}

/* The 6.00+ export reports a lock it cannot take as LOCKED (0x800201CB): SDK-built code relies on
 * it (the retail libpsmfplayer treats only 0x800201CB from sceKernelTryLockLwMutex_600 as
 * contention and otherwise proceeds as the owner). The original export keeps the hardware-measured
 * 0x800201C4 above. */
static uint32_t h_TryLockLwMutex600(CpuState *s) {
    uint32_t r = h_TryLockLwMutex(s);
    return r == SCE_KERNEL_ERROR_LWMUTEX_FAILED_TO_OWN ? SCE_KERNEL_ERROR_LWMUTEX_LOCKED : r;
}

static uint32_t h_UnlockLwMutex(CpuState *s) {
    /* a0=workarea, a1=unlockCount. */
    uint32_t wa = A0;
    if (!lwmutex_workarea_ok(wa)) return 0x80000103u;
    int count = (int)A1;
    if (count == 0) return 0x800201bdu;   /* PSP-B1-01: ILLEGAL_COUNT */
    if (count < 0) return 0x800200d2u;    /* unmeasured; previous behaviour */
    int level = (int)MEM_R32(wa + LWMUTEX_LOCK_LEVEL);
    uint32_t owner = MEM_R32(wa + LWMUTEX_LOCK_THREAD);
    uint32_t cur = sched_current_uid();
    if (level <= 0 || owner != cur) {
        /* PSP-B1-01 (unlocked) and PSP-B2-01 (non-owner): UNLOCK_UNDERFLOW,
         * leaving the real owner's bookkeeping untouched. */
        if (hle_log_on())
            fprintf(stderr, "HLE: UnlockLwMutex wa=0x%08x from uid=0x%x but level=%d owner=0x%x\n",
                    wa, cur, level, owner);
        return SCE_KERNEL_ERROR_LWMUTEX_UNLOCK_UNDERFLOW;
    }
    level -= count;
    if (level < 0) level = 0;
    MEM_W32(wa + LWMUTEX_LOCK_LEVEL, (uint32_t)level);
    if (level == 0) {
        uint32_t muid = MEM_R32(wa + LWMUTEX_UID);
        /* PSP-B2-01 (psp-hw-20260917): ownership passes directly to the waiter
         * before it runs; only then does the waiter become runnable. */
        uint32_t next_thread = 0;
        int next_count = 0;
        if (lww_pop_first(muid, &next_thread, &next_count)) {
            MEM_W32(wa + LWMUTEX_LOCK_LEVEL, (uint32_t)next_count);
            MEM_W32(wa + LWMUTEX_LOCK_THREAD, next_thread);
            sched_wake_one_object_waiter(muid, next_thread);
            sched_preempt();
        } else {
            MEM_W32(wa + LWMUTEX_LOCK_THREAD, 0u);
            sched_wake(muid);
            sched_preempt();
        }
    }
    return 0;
}
#ifndef SCE_KERNEL_ERROR_WAIT_CANCEL
#define SCE_KERNEL_ERROR_WAIT_CANCEL   0x800201a9u
#endif

static uint32_t h_CancelSema(CpuState *s) {
    /* a0=semaid, a1=newCount, a2=numWaitThreads out (stack_arg 0).
     * PSP-B2-01 (psp-hw-20260917): newCount -1 resets the count to the initial
     * count, n <= max sets it, n > max rejects with ILLEGAL_COUNT (and numWait
     * stays untouched). Wake waiters with WAIT_CANCEL and report their number;
     * a NULL numWait is allowed. */
    Sync *m = sync_find(A0, SYNC_KIND_SEMA); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_SEMID;
    int new_count = (int32_t)A1;
    if (new_count > m->maxc) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (hle_log_on())
        fprintf(stderr, "HLE: CancelSema uid=0x%x newCount=%d (from 0x%x)\n", A0, new_count, sched_current_uid());
    int waiters = sched_count_waiters(A0);
    /* PSP-B2-01 (psp-hw-20260917): -1 restores the create-time initial count. */
    m->count = (new_count == -1) ? m->initc : new_count;
    uint32_t nump = stack_arg(s, 0);
    if (nump) MEM_W32(nump, (uint32_t)waiters);
    /* No waiter: no wake result to latch (a stale result would be
     * misattributed to the next waiter). */
    if (waiters) {
        sched_wake_with_result(A0, SCE_KERNEL_ERROR_WAIT_CANCEL);
        sched_preempt();
    }
    return 0;
}

static void evf_cancel_outputs(uint32_t uid, uint32_t pattern);

static uint32_t h_CancelEventFlag(CpuState *s) {
    /* a0=evid, a1=newPattern, a2=numWaitThreads out.
     * PSP-B2-01 (psp-hw-20260917): Cancel sets the pattern, reports the waiter
     * count and wakes every waiter with WAIT_CANCEL. */
    Sync *m = sync_find(A0, SYNC_KIND_EVENT_FLAG); if (!m) return 0x8002019au;   /* UNKNOWN_EVFID */
    int waiters = sched_count_waiters(A0);
    m->pattern = A1;
    uint32_t nump = A2;
    if (nump && sr_guest_span_writable(nump, 4u)) MEM_W32(nump, (uint32_t)waiters);
    if (waiters) {
        evf_cancel_outputs(A0, m->pattern);
        sched_wake_with_result(A0, SCE_KERNEL_ERROR_WAIT_CANCEL);
        sched_preempt();
    }
    return 0;
}
#define PSP_MUTEX_ATTR_PRIORITY        0x0100u
#define PSP_MUTEX_ATTR_ALLOW_RECURSIVE 0x0200u
#define PSP_MUTEX_LEGAL_ATTR_MASK      0x0BFFu

#ifndef SCE_KERNEL_ERROR_WAIT_CANCEL
#define SCE_KERNEL_ERROR_WAIT_CANCEL                0x800201A9u
#endif
#define SCE_KERNEL_ERROR_NO_MEMORY                  0x80020190u
#define SCE_KERNEL_ERROR_UNKNOWN_MUTEXID            0x800201C3u
#define SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN        0x800201C4u
#define SCE_KERNEL_ERROR_MUTEX_NOT_OWNED            0x800201C5u
#define SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW        0x800201C6u
#define SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW      0x800201C7u
#define SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED 0x800201C8u

typedef struct {
    uint32_t size;
    char     name[32];
    uint32_t attr;
    int      initCount;
    int      currentCount;
    int32_t  lockThread;
    int      numWaitThreads;
} SceKernelMutexInfo;
size_t  s_mutex_cap = 0;

static Mutex *mutex_alloc(void) {
    if (s_mutex_len >= s_mutex_cap) {
        size_t next;
        if (s_mutex_cap) {
            if (s_mutex_cap > SIZE_MAX / 2u) return NULL;
            next = s_mutex_cap * 2u;
        } else {
            next = 64u;
        }
        if (next > SIZE_MAX / sizeof(*s_mutexes)) return NULL;
        Mutex **grown = (Mutex **)realloc(s_mutexes, next * sizeof(*grown));
        if (!grown) return NULL;
        for (size_t i = s_mutex_cap; i < next; i++) grown[i] = NULL;
        s_mutexes = grown;
        s_mutex_cap = next;
    }
    Mutex *m = (Mutex *)calloc(1, sizeof(Mutex));
    if (!m) return NULL;
    m->uid = sr_alloc_uid();
    s_mutexes[s_mutex_len++] = m;
    return m;
}

static void mutex_free(Mutex *m) {
    if (!m) return;
    for (size_t i = 0; i < s_mutex_len; i++) {
        if (s_mutexes[i] == m) {
            s_mutexes[i] = s_mutexes[s_mutex_len - 1];
            s_mutexes[s_mutex_len - 1] = NULL;
            s_mutex_len--;
            break;
        }
    }
    if (m->waiters) free(m->waiters);
    free(m);
}

static int mutex_enqueue_waiter(Mutex *m, uint32_t thread_uid,
                                 SrWaitHandle invocation, int requested_count) {
    if (m->wait_count >= m->wait_cap) {
        int next;
        if (m->wait_cap) {
            if (m->wait_cap > INT_MAX / 2) return 0;
            next = m->wait_cap * 2;
        } else {
            next = 4;
        }
        if ((size_t)next > SIZE_MAX / sizeof(*m->waiters)) return 0;
        MutexWaiter *grown = (MutexWaiter *)realloc(m->waiters, (size_t)next * sizeof(*grown));
        if (!grown) return 0;
        m->waiters = grown;
        m->wait_cap = next;
    }
    m->waiters[m->wait_count].thread_uid = thread_uid;
    m->waiters[m->wait_count].invocation = invocation;
    m->waiters[m->wait_count].requested_count = requested_count;
    m->wait_count++;
    m->num_wait_threads++;
    return 1;
}

static void mutex_remove_waiter(Mutex *m, SrWaitHandle invocation) {
    for (int i = 0; i < m->wait_count; i++) {
        if (m->waiters[i].invocation == invocation) {
            for (int j = i; j + 1 < m->wait_count; j++) {
                m->waiters[j] = m->waiters[j + 1];
            }
            m->wait_count--;
            m->num_wait_threads--;
            break;
        }
    }
}

static int mutex_select_and_dequeue_waiter(Mutex *m, MutexWaiter *out) {
    int best_idx = -1, best_pri = INT_MAX;
    for (int i = 0; i < m->wait_count; i++) {
        SrWaitState state;
        uint32_t owner;
        SrThreadRunStatus rs;
        if (!sched_wait_state(m->waiters[i].invocation, &state, &owner) ||
            state == SR_WAIT_TERMINAL || owner != m->waiters[i].thread_uid ||
            sched_thread_run_status(owner, &rs) != 0) continue;
        if (best_idx < 0 || ((m->attr & PSP_MUTEX_ATTR_PRIORITY) &&
                            (int)rs.currentPriority < best_pri)) {
            best_idx = i;
            best_pri = (int)rs.currentPriority;
        }
    }
    if (best_idx < 0) return 0;
    *out = m->waiters[best_idx];
    for (int j = best_idx; j + 1 < m->wait_count; j++) {
        m->waiters[j] = m->waiters[j + 1];
    }
    m->wait_count--;
    m->num_wait_threads--;
    return 1;
}

int mutex_grant_next(Mutex *m) {
    MutexWaiter next;
    if (!mutex_select_and_dequeue_waiter(m, &next)) {
        m->lock_thread = 0u;
        m->current_count = 0;
        return 0;
    }
    m->lock_thread = next.thread_uid;
    m->current_count = next.requested_count;
    (void)sched_wait_complete(next.invocation, 0u);
    /* A callback child on this same mutex may now acquire recursively, but it
     * must add its own count (or reject recursion). Notification is not a grant. */
    for (int i = 0; i < m->wait_count; i++)
        if (m->waiters[i].thread_uid == next.thread_uid)
            (void)sched_wait_notify(m->waiters[i].invocation);
    return 1;
}

static int mutex_detach_invocation(SrWaitHandle invocation, uint32_t uid) {
    Mutex *m = mutex_find(uid);
    if (!m) return 0;
    mutex_remove_waiter(m, invocation);
    return m->current_count == 0 ? mutex_grant_next(m) : 0;
}

static uint32_t h_CreateMutex(CpuState *s) {
    uint32_t name_addr = A0;
    uint32_t attr = A1;
    int init_count = (int)A2;

    if (!name_addr) return 0x80020001u;
    char name_buf[32];
    memset(name_buf, 0, sizeof(name_buf));
    size_t len = 0;
    while (len < 31) {
        uint32_t addr;
        if (!sr_size_add_ok(name_addr, (uint32_t)len, &addr) || !sr_inrange(addr)) {
            return 0x80020001u;
        }
        char c = (char)MEM_R8(addr);
        if (c == '\0') break;
        name_buf[len++] = c;
    }
    name_buf[len] = '\0';

    if ((attr & ~PSP_MUTEX_LEGAL_ATTR_MASK) != 0 || (attr & 0x0400u) != 0) {
        return 0x80020191u; /* SCE_KERNEL_ERROR_ILLEGAL_ATTR */
    }

    if (!(attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE)) {
        if (init_count < 0 || init_count > 1) return 0x800201BDu;
    } else {
        if (init_count < 0) return 0x800201BDu;
    }

    Mutex *m = mutex_alloc();
    if (!m) return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */

    memcpy(m->name, name_buf, sizeof(name_buf));
    m->attr = attr;
    m->init_count = init_count;
    m->current_count = init_count;
    if (init_count > 0) {
        m->lock_thread = sched_current_uid();
    } else {
        m->lock_thread = 0;
    }
    return m->uid;
}

static uint32_t h_DeleteMutex(CpuState *s) {
    uint32_t uid = A0;
    Mutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;

    sched_wait_cancel_object(uid, SCE_KERNEL_ERROR_WAIT_DELETE);
    mutex_free(m);
    return 0;
}

static uint32_t h_LockMutex_impl(CpuState *s, int is_cb) {
    uint32_t uid = A0;
    int count = (int)A1;
    uint32_t toptr = A2;

    if (sched_is_intr_context()) return 0x80020064u; /* SCE_KERNEL_ERROR_ILLEGAL_CONTEXT */
    if (!sched_wait_permitted()) return 0x800201A7u; /* SCE_KERNEL_ERROR_CAN_NOT_WAIT */

    Mutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;

    if (count <= 0) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (!(m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) && count > 1)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    uint32_t cur_thid = sched_current_uid();

    if (m->current_count > 0 && m->lock_thread == cur_thid) {
        if (!(m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE)) {
            return SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED;
        }
        if ((uint64_t)m->current_count + (uint64_t)count > (uint64_t)INT_MAX) {
            return SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW;
        }
        m->current_count += count;
        return 0;
    }

    if (m->current_count == 0) {
        m->current_count = count;
        m->lock_thread = cur_thid;
        return 0;
    }

    uint64_t end_time = 0;
    int has_timeout = 0;
    if (toptr) {
        if (!sr_guest_span_writable(toptr, 4)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        uint32_t usec = MEM_R32(toptr);
        if (usec == 0) {
            return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        }
        sched_vtime_refresh();
        end_time = sched_vtime_deadline_after((uint64_t)usec);
        has_timeout = 1;
    }

    SrWaitHandle invocation = sched_wait_begin(uid, end_time, is_cb,
                                                mutex_detach_invocation);
    if (!invocation) return SCE_KERNEL_ERROR_NO_MEMORY;
    if (!mutex_enqueue_waiter(m, cur_thid, invocation, count)) {
        (void)sched_wait_finish(invocation);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    uint32_t result;
    for (;;) {
        /* Only this invocation's grant completes this queued request. */
        if (sched_wait_take_result(invocation, &result)) break;
        if (is_cb && sr_thread_has_pending_callbacks(cur_thid)) {
            sched_wait_callback(invocation, 1);
            sr_thread_dispatch_callbacks();
            sched_wait_callback(invocation, 0);
            if (sched_wait_take_result(invocation, &result)) break;
        }
        m = mutex_find(uid);
        if (!m) { result = SCE_KERNEL_ERROR_WAIT_DELETE; break; }
        /* A parked parent may have established thread ownership. That is not
         * this child's grant: apply the ordinary recursive-lock policy anew. */
        if (m->current_count > 0 && m->lock_thread == cur_thid) {
            if (!(m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE))
                result = SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED;
            else if ((uint64_t)m->current_count + (uint64_t)count > INT_MAX)
                result = SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW;
            else { m->current_count += count; result = 0u; }
            break;
        }
        uint32_t remaining = 0u;
        if (has_timeout) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= end_time) { result = SCE_KERNEL_ERROR_WAIT_TIMEOUT; break; }
            remaining = (uint32_t)(end_time - now);
        }
        int expired = sched_wait_block(invocation, remaining, has_timeout);
        if (sched_wait_take_result(invocation, &result)) break;
        if (expired) { result = SCE_KERNEL_ERROR_WAIT_TIMEOUT; break; }
    }
    if (has_timeout && (result == 0u || result == SCE_KERNEL_ERROR_WAIT_TIMEOUT)) {
        sched_vtime_refresh();
        uint64_t now = sched_vtime_us();
        MEM_W32(toptr, now < end_time ? (uint32_t)(end_time - now) : 0u);
    }
    if (sched_wait_finish(invocation)) sched_preempt();
    return result;
}

static uint32_t h_LockMutex(CpuState *s) {
    return h_LockMutex_impl(s, 0);
}

static uint32_t h_LockMutexCB(CpuState *s) {
    return h_LockMutex_impl(s, 1);
}

static uint32_t h_TryLockMutex(CpuState *s) {
    uint32_t uid = A0;
    int count = (int)A1;

    if (sched_is_intr_context()) return 0x80020064u; /* SCE_KERNEL_ERROR_ILLEGAL_CONTEXT */

    Mutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;

    if (count <= 0) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (!(m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) && count > 1)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    uint32_t cur_thid = sched_current_uid();
    if (m->current_count == 0) {
        m->current_count = count;
        m->lock_thread = cur_thid;
        return 0;
    }

    if (m->lock_thread == cur_thid) {
        if (!(m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE)) {
            return SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED;
        }
        if ((uint64_t)m->current_count + (uint64_t)count > (uint64_t)INT_MAX) {
            return SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW;
        }
        m->current_count += count;
        return 0;
    }

    return SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN;
}

static uint32_t h_UnlockMutex(CpuState *s) {
    uint32_t uid = A0;
    int count = (int)A1;

    /* Interrupt context is gated ahead of the object and owner checks, as Lock, LockCB and TryLock
     * are. PSP-measured 2026-10-15 (PSP-3000, 6.61; acceptance-eligible mutex-interrupt-context run on
     * probe ec5dd563): in 20 of 20 VBLANK sub-interrupt trials, an unlock of a mutex the handler does
     * not own returned ILLEGAL_CONTEXT (0x80020064), not MUTEX_NOT_OWNED. Unlock with an unknown
     * UID or a count of 0 or less from interrupt context is UNMEASURED; it takes the same gate,
     * following the order measured for LockMutex. */
    if (sched_is_intr_context()) return SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;

    Mutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;

    if (count <= 0) return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (!(m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) && count != 1)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    uint32_t cur_thid = sched_current_uid();
    if (m->current_count == 0 || m->lock_thread != cur_thid)
        return SCE_KERNEL_ERROR_MUTEX_NOT_OWNED;

    if (count > m->current_count)
        return SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW;

    m->current_count -= count;
    if (m->current_count > 0) {
        return 0;
    }

    if (mutex_grant_next(m)) sched_preempt();
    return 0;
}

static uint32_t h_CancelMutex(CpuState *s) {
    uint32_t uid = A0;
    int new_count = (int)A1;
    uint32_t num_wait_ptr = A2;

    Mutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;

    if (new_count < 0 || new_count > 1) {
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    }

    if (num_wait_ptr && !sr_guest_span_writable(num_wait_ptr, 4))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (num_wait_ptr) MEM_W32(num_wait_ptr, (uint32_t)m->num_wait_threads);

    m->cancel_seq++;
    if (new_count == 1) {
        m->current_count = 1;
        m->lock_thread = sched_current_uid();
    } else {
        m->current_count = 0;
        m->lock_thread = 0;
    }

    m->wait_count = 0;
    m->num_wait_threads = 0;

    sched_wait_cancel_object(uid, SCE_KERNEL_ERROR_WAIT_CANCEL);
    return 0;
}

static uint32_t h_ReferMutexStatus(CpuState *s) {
    uint32_t uid = A0;
    uint32_t info_addr = A1;

    if (!info_addr || !sr_guest_span_readable(info_addr, 4))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    Mutex *m = mutex_find(uid);
    if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;

    uint32_t input_size = MEM_R32(info_addr);
    if (input_size == 0) {
        return 0;
    }

    uint32_t write_len = input_size < (uint32_t)sizeof(SceKernelMutexInfo) ? input_size : (uint32_t)sizeof(SceKernelMutexInfo);
    if (!sr_guest_span_writable(info_addr, write_len))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    SceKernelMutexInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(SceKernelMutexInfo);
    memcpy(info.name, m->name, sizeof(info.name));
    info.attr = m->attr;
    info.initCount = m->init_count;
    info.currentCount = m->current_count;
    info.lockThread = (int32_t)m->lock_thread;
    info.numWaitThreads = m->num_wait_threads;

    for (uint32_t i = 0; i < write_len; i++) {
        MEM_W8(info_addr + i, ((const uint8_t *)&info)[i]);
    }
    return 0;
}

static uint32_t h_CreateEventFlag(CpuState *s) {
    /* a0=name, a1=attr, a2=initPattern, a3=opt. */
    /* PSPAutotests threads/events/create.expected records the null-name and
     * attribute cases. The option is ignored, as in the recorded cases. */
    if (!A0) return 0x80020001u;
    if ((A1 & 0x100u) || A1 >= 0x300u) return 0x80020191u;
    /* Unreadable guest names fail closed; the PSP result for that case is
     * unmeasured. Long valid names are truncated to 31 bytes. */
    char name[32] = {0};
    for (uint32_t i = 0; i < 31u; i++) {
        uint32_t addr;
        if (!sr_size_add_ok(A0, i, &addr) || !sr_guest_span_readable(addr, 1u))
            return 0x80020001u;
        name[i] = (char)MEM_R8(addr);
        if (!name[i]) break;
    }
    Sync *m = sync_new(SYNC_KIND_EVENT_FLAG); if (!m) return 0x80020000;
    memcpy(m->name, name, sizeof(name));
    m->pattern = A2;
    m->attr = A1;
    m->init_pattern = A2;
    return m->uid;
}
/* PSP-B1-01: unknown or deleted event-flag ids are UNKNOWN_EVFID. PSPAutotests
 * tests/threads/events gives Clear (clear.expected L15-L19), Wait
 * (wait.expected L35) and Refer (refer.expected L23-L25) the same code, and a
 * UID of another kind in the shared table answers it too (see sync_find). */
#define SCE_KERNEL_ERROR_UNKNOWN_EVFID 0x8002019au
static Sync *evf_find(uint32_t uid) { return sync_find(uid, SYNC_KIND_EVENT_FLAG); }
static uint64_t s_evf_wait_order;

static int evf_waiter_live(const EvfWaiter *w) {
    SrThreadRunStatus rs;
    /* PSP-B3-01 (psp-hw-20260917): an event-flag wait is waitType 4. */
    return w->active && sched_thread_run_status(w->thread, &rs) == 0 &&
           rs.waitType == 4u && rs.waitId == w->evf;
}
static void evf_waiter_end(void) {
    uint32_t self = sched_current_uid();
    for (int i = 0; i < EVF_WAITER_MAX; i++)
        if (s_evf_waiters[i].active && s_evf_waiters[i].thread == self) s_evf_waiters[i].active = 0;
}
static void evf_waiter_begin(uint32_t evf, uint32_t outp, uint32_t bits, uint32_t mode, uint32_t toptr, uint64_t deadline) {
    evf_waiter_end();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < EVF_WAITER_MAX; i++)
            if (!s_evf_waiters[i].active) {
                s_evf_waiters[i] = (EvfWaiter){evf, sched_current_uid(), outp, bits, mode, toptr, ++s_evf_wait_order, deadline, 1};
                return;
            }
        for (int i = 0; i < EVF_WAITER_MAX; i++)
            if (!evf_waiter_live(&s_evf_waiters[i])) s_evf_waiters[i].active = 0;
    }
    fprintf(stderr, "HLE: event-flag waiter table full; uid=0x%x outBits=0x%08x not recorded\n",
            evf, outp);
}

/* Snapshot outputs at wake, before another thread can alter the flag or the
 * released waiter can be delayed by scheduling. */
static void evf_waiter_outputs(const EvfWaiter *w, uint32_t pattern) {
    if (w->outp && sr_guest_span_writable(w->outp, 4u)) MEM_W32(w->outp, pattern);
    if (w->toptr && sr_guest_span_writable(w->toptr, 4u)) {
        sched_vtime_refresh();
        uint64_t now = sched_vtime_us();
        uint64_t remaining = now < w->deadline ? w->deadline - now : 0u;
        MEM_W32(w->toptr, (uint32_t)remaining);
    }
}
static void evf_cancel_outputs(uint32_t uid, uint32_t pattern) {
    for (int i = 0; i < EVF_WAITER_MAX; i++) {
        EvfWaiter *w = &s_evf_waiters[i];
        if (w->evf == uid && evf_waiter_live(w)) evf_waiter_outputs(w, pattern);
    }
}

static uint32_t h_DeleteEventFlag(CpuState *s) {
    uint32_t uid = A0;
    Sync *m = evf_find(uid); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
    uint32_t pattern = m->pattern;
    m->used = 0;
    /* PSPAutotests tests/threads/events: the delete returns 0 and a thread
     * blocked on the flag leaves its wait with WAIT_DELETE (0x800201B5), the
     * plain form (wait.expected L31) and the CB form (delete.expected L5-L7)
     * alike. wait.expected L31 also shows outBits written: 00000000, over
     * 0xDEADBEEF, from a flag whose pattern is 0. Writing the pattern at
     * deletion follows the measured Cancel rule (PSP-B2-01: the canceller's
     * pattern) and PPSSPP; a nonzero pattern at deletion is unmeasured. */
    for (int i = 0; i < EVF_WAITER_MAX; i++) {
        EvfWaiter *w = &s_evf_waiters[i];
        if (!w->active || w->evf != uid) continue;
        if (evf_waiter_live(w)) evf_waiter_outputs(w, pattern);
        w->active = 0;
    }
    /* No waiter: no wake result to latch (see h_DeleteSema). */
    if (sched_count_waiters(uid)) {
        sched_wake_with_result(uid, SCE_KERNEL_ERROR_WAIT_DELETE);
        sched_preempt();   /* delete.expected L6: a stronger waiter runs at once */
    }
    return 0;
}
static uint32_t h_SetEventFlag(CpuState *s) {
    Sync *m = evf_find(A0); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
    m->pattern |= A1;
    /* PSPAutotests set.expected: each waiter observes the pattern left by
     * preceding waiters, not a pattern rechecked later in scheduler order. */
    uint64_t after = 0;
    int woke = 0;
    for (;;) {
        EvfWaiter *next = NULL;
        for (int i = 0; i < EVF_WAITER_MAX; i++) {
            EvfWaiter *w = &s_evf_waiters[i];
            if (w->evf == A0 && evf_waiter_live(w) && w->order > after &&
                (!next || w->order < next->order)) next = w;
        }
        if (!next) break;
        after = next->order;
        if (!sr_evf_matches(m->pattern, next->bits, next->mode)) continue;
        evf_waiter_outputs(next, m->pattern);
        m->pattern = sr_evf_consume(m->pattern, next->bits, next->mode);
        woke |= sched_wake_one_object_waiter_with_result(next->thread, 0u);
    }
    if (woke) sched_preempt();
    return 0;
}
static uint32_t h_ClearEventFlag(CpuState *s) {
    /* sceKernelClearEventFlag(evfid, bits): A1 is the mask of bits to KEEP
     * (currentPattern &= bits), matching PSP/PPSSPP. It is NOT a mask of bits
     * to remove — inverting A1 here inverts the contract. */
    Sync *m = evf_find(A0); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
    m->pattern = sr_evf_clear_pattern(m->pattern, A1); return 0;
}
static uint32_t h_WaitEventFlag(CpuState *s) {
    /* a0=uid, a1=bits, a2=mode, a3=outBits, [sp+16]=timeout. */
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3, toptr = stack_arg(s, 0);
    uint32_t rc = sr_evf_check_wait_args(bits, mode);
    if (rc) return rc;
    Sync *m = evf_find(uid); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
    if (hle_log_on())
        fprintf(stderr, "HLE: WaitEventFlag uid=0x%x bits=0x%x pattern=0x%x (from 0x%x)\n", uid, bits, m->pattern, sched_current_uid());
    /* Third in line, and the ordering is the whole point. sr_evf_check_wait_args()
     * above already returned ILLEGAL_MODE for mode 0xFF, which hardware confirms beats
     * the context error (L72/L73) -- that cell must keep returning 0x80020195. The
     * object lookup keeps its precedence too, and an already-satisfied pattern is
     * consumed normally. Only a pattern that does not match yet is a genuine wait.
     * A rejected call writes no outBits and consumes no pattern. (L74/L75) */
    if (!sr_evf_matches(m->pattern, bits, mode) && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    /* PSP-B2-01 (psp-hw-20260917): a second waiter on a single-wait flag
     * (attr without 0x200) is rejected at once with 0x800201B0, leaving its
     * timeout and outBits untouched. */
    if (!sr_evf_matches(m->pattern, bits, mode) && !(m->attr & 0x200u) &&
        sched_count_waiters(uid) > 0)
        return 0x800201b0u;
    uint64_t wait_end = 0;      /* fixed by the first block; see h_WaitSema */
    int wait_end_set = 0;
    while (!sr_evf_matches(m->pattern, bits, mode)) {
        /* PSP-B3-01 (psp-hw-20260917): evf waits report waitType 4. */
        sched_set_current_wait_kind(4);
        if (toptr) {
            uint32_t usec = MEM_R32(toptr);
            if (!wait_end_set) {
                sched_vtime_refresh();
                wait_end = sched_vtime_deadline_after((uint64_t)usec);
                wait_end_set = 1;
            } else {
                sched_vtime_refresh();
                uint64_t now = sched_vtime_us();
                usec = now >= wait_end ? 0u : (uint32_t)(wait_end - now);
                if (usec == 0u) {
                    MEM_W32(toptr, 0u);
                    if (outp) MEM_W32(outp, m->pattern);
                    return 0x800201A8;
                }
            }
            evf_waiter_begin(uid, outp, bits, mode, toptr, wait_end);
            int timed_out = sched_block_on_timeout(uid, usec);
            evf_waiter_end();
            if (timed_out) {
                /* PSP-B2-01 (psp-hw-20260917): an expired wait writes the
                 * remaining timeout (0) and reports the current pattern. */
                MEM_W32(toptr, 0u);
                m = evf_find(uid);
                if (outp && m) MEM_W32(outp, m->pattern);
                return 0x800201A8;
            }
        } else {
            evf_waiter_begin(uid, outp, bits, mode, toptr, wait_end);
            sched_block_on(uid);
            evf_waiter_end();
        }
        /* PSP-B2-01 (psp-hw-20260917): a cancelled wait answers WAIT_CANCEL
         * (0x800201A9) with outBits = the canceller's new pattern. */
        {
            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) {
                return woken;
            }
        }
        /* Gone without a wake result: deleted while this thread sat READY after
         * another wake. The flag existed when the wait began, so the wait ends
         * as a deleted one (WAIT_DELETE above), not with the generic seam. */
        m = evf_find(uid); if (!m) return SCE_KERNEL_ERROR_WAIT_DELETE;
    }
    if (outp) MEM_W32(outp, m->pattern);         /* outBits = pre-consume pattern */
    m->pattern = sr_evf_consume(m->pattern, bits, mode);
    return 0;
}
static uint32_t h_WaitEventFlagCB(CpuState *s) {
    /* a0=uid, a1=bits, a2=mode, a3=outBits, [sp+16]=timeout. */
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3, toptr = stack_arg(s, 0);
    uint32_t rc = sr_evf_check_wait_args(bits, mode);
    if (rc) return rc;
    Sync *m = evf_find(uid); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
    if (hle_log_on())
        fprintf(stderr, "HLE: WaitEventFlagCB uid=0x%x bits=0x%x pattern=0x%x (from 0x%x)\n", uid, bits, m->pattern, sched_current_uid());
    /* Same ordering as the non-CB form, and ahead of the clock sample and callback
     * loop so a rejected call leaves both untouched. ILLEGAL_MODE still wins (L82/L83);
     * an already-set pattern still returns 0. (L84/L85) */
    if (!sr_evf_matches(m->pattern, bits, mode) && !sched_wait_permitted())
        return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
    /* PSP-B2-01 (psp-hw-20260917): same single-wait rejection as the non-CB
     * form; a rejected call touches neither the timeout nor outBits. */
    if (!sr_evf_matches(m->pattern, bits, mode) && !(m->attr & 0x200u) &&
        sched_count_waiters(uid) > 0)
        return 0x800201b0u;

    /* A CB wait services pending callbacks even when it initially matches.
     * A callback may change or delete the flag, so recheck before waiting. */
    if (sr_thread_has_pending_callbacks(sched_current_uid())) {
        sr_thread_dispatch_callbacks();
        m = evf_find(uid); if (!m) return SCE_KERNEL_ERROR_WAIT_DELETE;
        if (!sr_evf_matches(m->pattern, bits, mode) && !sched_wait_permitted())
            return SCE_KERNEL_ERROR_CAN_NOT_WAIT;
        if (!sr_evf_matches(m->pattern, bits, mode) && !(m->attr & 0x200u) &&
            sched_count_waiters(uid) > 0)
            return 0x800201b0u;
    }
    sched_vtime_refresh();
    uint64_t start = sched_vtime_us();
    uint64_t end = start + (toptr ? MEM_R32(toptr) : 0);

    while (!sr_evf_matches(m->pattern, bits, mode)) {
        if (sr_thread_has_pending_callbacks(sched_current_uid())) {
            sr_thread_dispatch_callbacks();
            /* Deleted while this waiter ran its callbacks: the wait ends as a
             * deleted one, as PPSSPP ends it (unmeasured on hardware). */
            m = evf_find(uid); if (!m) return SCE_KERNEL_ERROR_WAIT_DELETE;
            sched_vtime_refresh();
            continue;
        }
        if (toptr) {
            sched_vtime_refresh();
            if (sched_vtime_us() >= end) {
                /* PSP-B2-01 (psp-hw-20260917): the expired wait reports its
                 * remaining timeout (0), it does not leave the caller's word. */
                MEM_W32(toptr, 0u);
                if (outp) MEM_W32(outp, m->pattern);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
            uint32_t remaining = (uint32_t)(end - sched_vtime_us());
            MEM_W32(toptr, remaining);
            sched_set_current_cb_wait(1);
            /* PSP-B3-01 (psp-hw-20260917): evf CB waits report waitType 4. */
            sched_set_current_wait_kind(4);
            evf_waiter_begin(uid, outp, bits, mode, toptr, end);
            int timed_out = sched_block_on_timeout(uid, remaining);
            evf_waiter_end();
            sched_set_current_cb_wait(0);
            if (timed_out) {
                /* PSP-B2-01 (psp-hw-20260917): same remaining-0 contract after
                 * a real timed block. */
                MEM_W32(toptr, 0u);
                m = evf_find(uid);
                if (outp && m) MEM_W32(outp, m->pattern);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
        } else {
            sched_set_current_cb_wait(1);
            sched_set_current_wait_kind(4);
            evf_waiter_begin(uid, outp, bits, mode, toptr, end);
            sched_block_on(uid);
            evf_waiter_end();
            sched_set_current_cb_wait(0);
        }
        /* PSP-B2-01 (psp-hw-20260917): a cancelled wait answers WAIT_CANCEL
         * (0x800201A9) with outBits = the canceller's new pattern. */
        {
            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) {
                return woken;
            }
        }
        m = evf_find(uid); if (!m) return SCE_KERNEL_ERROR_WAIT_DELETE;   /* as in h_WaitEventFlag */
        sched_vtime_refresh();
    }
    if (outp) MEM_W32(outp, m->pattern);
    m->pattern = sr_evf_consume(m->pattern, bits, mode);
    return 0;
}
static uint32_t h_PollEventFlag(CpuState *s) {
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3;
    uint32_t rc = sr_evf_check_poll_args(bits, mode);
    if (rc) return rc;
    Sync *m = evf_find(uid); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
    if (!sr_evf_matches(m->pattern, bits, mode)) {
        if (outp) MEM_W32(outp, m->pattern);
        return SR_EVF_ERR_COND;
    }
    if (outp) MEM_W32(outp, m->pattern);         /* outBits = pre-consume pattern */
    m->pattern = sr_evf_consume(m->pattern, bits, mode);
    return 0;
}
/* PSPAutotests refer.expected: size zero writes nothing; every nonzero
 * size, including 1, returns the complete 52-byte structure. */
static uint32_t h_ReferEventFlagStatus(CpuState *s) {
    Sync *m = evf_find(A0); if (!m) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
    uint32_t info = A1;
    if (!info || !sr_guest_span_readable(info, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (!MEM_R32(info)) return 0;
    if (!sr_guest_span_writable(info, 52u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    MEM_W32(info, 52u);
    for (uint32_t i = 0; i < 32u; i++) MEM_W8(info + 4u + i, (uint8_t)m->name[i]);
    MEM_W32(info + 36u, m->attr);
    MEM_W32(info + 40u, m->init_pattern);
    MEM_W32(info + 44u, m->pattern);
    MEM_W32(info + 48u, (uint32_t)sched_count_waiters(A0));
    return 0;
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_cancel_release_handlers(). */
void hle_register_cancel_release_kernel_sync(void) {
    sr_hle_register(0x8ffdf9a2, "sceKernelCancelSema", h_CancelSema);
    sr_hle_register(0xcd203292, "sceKernelCancelEventFlag", h_CancelEventFlag);
}

/* From hle_register_selftest_oracle_handlers(). */
void hle_register_selftest_oracle_kernel_sync(void) {
    sr_hle_register(0xd6da4ba1, "sceKernelCreateSema", h_CreateSema);
    sr_hle_register(0x28b6489c, "sceKernelDeleteSema", h_DeleteSema);
    sr_hle_register(0x3f53e640, "sceKernelSignalSema", h_SignalSema);
    sr_hle_register(0x58b1f937, "sceKernelPollSema", h_PollSema);
    sr_hle_register(0xbc6febc5, "sceKernelReferSemaStatus", h_ReferSemaStatus);
}

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_kernel_sync_1(void) {
    sr_hle_register(0x4e3a1105, "sceKernelWaitSema", h_WaitSema);
    sr_hle_register(0x6d212bac, "sceKernelWaitSemaCB", h_WaitSemaCB);
    sr_hle_register(0x55c20a00, "sceKernelCreateEventFlag", h_CreateEventFlag);
    sr_hle_register(0xef9e4c70, "sceKernelDeleteEventFlag", h_DeleteEventFlag);
    sr_hle_register(0x812346e4, "sceKernelClearEventFlag", h_ClearEventFlag);
    sr_hle_register(0x30fd48f0, "sceKernelPollEventFlag", h_PollEventFlag);
    sr_hle_register(0xa66b0120, "sceKernelReferEventFlagStatus", h_ReferEventFlagStatus);
    sr_hle_register(0x60107536, "sceKernelDeleteLwMutex", h_DeleteLwMutex);
    sr_hle_register(0x7cff8cf3, "_sceKernelLockLwMutex", h_LockLwMutex);
    sr_hle_register(0x31327f19, "_sceKernelLockLwMutexCB", h_LockLwMutexCB);
    sr_hle_register(0xdc692ee3, "sceKernelTryLockLwMutex", h_TryLockLwMutex);
    sr_hle_register(0x37431849, "sceKernelTryLockLwMutex_600", h_TryLockLwMutex600);
    sr_hle_register(0x71040d5c, "_sceKernelTryLockLwMutex", h_TryLockLwMutex);
    sr_hle_register(0x15b6446b, "sceKernelUnlockLwMutex", h_UnlockLwMutex);
    sr_hle_register(0xbeed3a47, "_sceKernelUnlockLwMutex", h_UnlockLwMutex);
    sr_hle_register(0x1fb15a32, "sceKernelSetEventFlag", h_SetEventFlag);
    sr_hle_register(0x402fcf22, "sceKernelWaitEventFlag", h_WaitEventFlag);
    sr_hle_register(0x328c546a, "sceKernelWaitEventFlagCB", h_WaitEventFlagCB);
}

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_kernel_sync_2(void) {
    sr_hle_register(0xb7d098c6, "sceKernelCreateMutex", h_CreateMutex);
    sr_hle_register(0xf8170fbe, "sceKernelDeleteMutex", h_DeleteMutex);
    sr_hle_register(0xb011b11f, "sceKernelLockMutex", h_LockMutex);
    sr_hle_register(0x5bf4dd27, "sceKernelLockMutexCB", h_LockMutexCB);
    sr_hle_register(0x0ddcd2c9, "sceKernelTryLockMutex", h_TryLockMutex);
    sr_hle_register(0x6b30100f, "sceKernelUnlockMutex", h_UnlockMutex);
    sr_hle_register(0x87d9223c, "sceKernelCancelMutex", h_CancelMutex);
    sr_hle_register(0xa9c2cb9a, "sceKernelReferMutexStatus", h_ReferMutexStatus);
    sr_hle_register(0x19cff145, "sceKernelCreateLwMutex", h_CreateLwMutex);
    sr_hle_register(0xbea46419, "sceKernelLockLwMutex", h_LockLwMutex);
    sr_hle_register(0x1fc64e09, "sceKernelLockLwMutexCB", h_LockLwMutexCB);
    sr_hle_register(0xc1734599, "sceKernelReferLwMutexStatus", h_ReferLwMutexStatus);
    sr_hle_register(0x4c145944, "sceKernelReferLwMutexStatusByID", h_ReferLwMutexStatusByID);
}
