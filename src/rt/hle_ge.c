// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* The graphics engine's kernel interface: sceGe display lists, callbacks and EDRAM. Extracted
 * from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* Bounded provenance trace for issue #143.  The missing post-match/menu panels
 * reproduce in both renderers, so the next useful split is whether the guest
 * submitted a display list at all.  Keep this separate from SR_GEDUMP: that
 * stream is intentionally broad and too noisy for a 35k-vblank route.
 *
 * SR_GE_ENQUEUE_TRACE=1 enables the trace.  Optional
 * SR_GE_ENQUEUE_TRACE_WINDOWS=a-b[,c-d...] restricts output to up to eight
 * inclusive vblank ranges.  An invalid, non-empty window string fails closed
 * (no trace records) instead of accidentally flooding stderr. */
#define GE_ENQUEUE_TRACE_MAX_WINDOWS 8

static int ge_enqueue_trace_in_window(void) {
    static int initialized;
    static int configured;
    static int valid;
    static uint32_t lo[GE_ENQUEUE_TRACE_MAX_WINDOWS];
    static uint32_t hi[GE_ENQUEUE_TRACE_MAX_WINDOWS];
    static int count;

    if (!ge_enqueue_trace_enabled()) return 0;
    if (!initialized) {
        initialized = 1;
        const char *windows = getenv("SR_GE_ENQUEUE_TRACE_WINDOWS");
        configured = windows != NULL && windows[0] != '\0';
        valid = !configured;
        if (configured) {
            const char *p = windows;
            valid = 1;
            while (*p && count < GE_ENQUEUE_TRACE_MAX_WINDOWS) {
                char *end = NULL;
                unsigned long first = strtoul(p, &end, 10);
                if (end == p || *end != '-' || first > UINT32_MAX) { valid = 0; break; }
                p = end + 1;
                unsigned long last = strtoul(p, &end, 10);
                if (end == p || last > UINT32_MAX || first > last) { valid = 0; break; }
                lo[count] = (uint32_t)first;
                hi[count] = (uint32_t)last;
                count++;
                p = end;
                if (*p == ',') p++;
                else if (*p != '\0') { valid = 0; break; }
            }
            if (*p != '\0' || count == 0) valid = 0;
            if (!valid) {
                fprintf(stderr,
                        "GE_ENQUEUE_TRACE: invalid SR_GE_ENQUEUE_TRACE_WINDOWS='%s' -- trace disabled\n",
                        windows);
            } else {
                for (int i = 0; i < count; i++)
                    fprintf(stderr, "GE_ENQUEUE_TRACE_WINDOW[%d]=%u-%u\n", i, lo[i], hi[i]);
            }
        }
    }
    if (!valid) return 0;
    if (!configured) return 1;
    for (int i = 0; i < count; i++)
        if (s_vcount >= lo[i] && s_vcount <= hi[i]) return 1;
    return 0;
}

static void ge_enqueue_trace_emit(CpuState *s, const char *op,
                                  uint32_t list_id, uint32_t list,
                                  uint32_t stall, uint32_t cbid) {
    if (!ge_enqueue_trace_in_window()) return;
    uint32_t uid = sched_current_uid();
    GeEnqueueTraceHle *prev_hle = ge_enqueue_trace_hle_slot(uid);
    GeEnqueueTraceCallback *prev_cb = ge_enqueue_trace_callback_slot(uid);
    fprintf(stderr,
            "GE_ENQUEUE_TRACE frame=%u op=%s thread=0x%x call_pc=0x%08x ra=0x%08x "
            "list_id=0x%08x list=0x%08x stall=0x%08x cbid=0x%08x "
            "prev_hle=%s/0x%08x@0x%08x,ra=0x%08x,frame=%u,seq=%llu "
            "prev_cb=0x%08x@0x%08x,frame=%u,seq=%llu\n",
            s_vcount, op, uid, ge_enqueue_trace_call_pc(s), s->r[31],
            list_id, list, stall, cbid,
            prev_hle->seq && prev_hle->name ? prev_hle->name : "none",
            prev_hle->seq ? prev_hle->nid : 0u,
            prev_hle->seq ? prev_hle->call_pc : 0u,
            prev_hle->seq ? prev_hle->ra : 0u,
            prev_hle->seq ? prev_hle->frame : 0u,
            (unsigned long long)prev_hle->seq,
            prev_cb->seq ? prev_cb->callback_uid : 0u,
            prev_cb->seq ? prev_cb->entry : 0u,
            prev_cb->seq ? prev_cb->frame : 0u,
            (unsigned long long)prev_cb->seq);
}

static void ge_enqueue_trace_result(CpuState *s, const char *op,
                                    uint32_t list_id, const char *outcome,
                                    uint32_t next_pc, int list_completed) {
    if (!ge_enqueue_trace_in_window()) return;
    fprintf(stderr,
            "GE_ENQUEUE_RESULT frame=%u op=%s thread=0x%x call_pc=0x%08x "
            "list_id=0x%08x outcome=%s next_pc=0x%08x "
            "complete=%d sig=0x%08lx prims=%lu "
            "through=%lu/%lu/%lu transform=%lu/%lu/%lu "
            "writes=%lu nonblack=%lu clearpx=%lu\n",
            s_vcount, op, sched_current_uid(), ge_enqueue_trace_call_pc(s),
            list_id, outcome, next_pc, list_completed,
            list_completed ? g_ge_list_sig : 0ul,
            list_completed ? g_ge_prim_count : 0ul,
            list_completed ? g_ge_list_through_cmds : 0ul,
            list_completed ? g_ge_list_through_vertices : 0ul,
            list_completed ? g_ge_list_through_sprites : 0ul,
            list_completed ? g_ge_list_transform_cmds : 0ul,
            list_completed ? g_ge_list_transform_vertices : 0ul,
            list_completed ? g_ge_list_transform_sprites : 0ul,
            list_completed ? g_list_writes : 0ul,
            list_completed ? g_list_nonblack : 0ul,
            list_completed ? g_list_clearpx : 0ul);
}

/* GE list-control values.  Every value below is PSP-measured on a PSP-3000 (6.61): the
 * acceptance-eligible 2026-10-15 re-run of the revised ge-break-continue probe (commit
 * c642039a, PSP-GE-CONTROL-001).  Each comment names the list state the cell measured;
 * a state the probe did not reach is marked UNMEASURED where the value is applied. */
#define GE_LIST_SYNC_STALLED 3u     /* sceGeListSync(qid, 1) on a list stalled at its start address, read after a BUSY dequeue; PSP-GE-CONTROL-001, PSP-measured 2026-10-15 (eligible re-run, probe c642039a). A list stalled partway through is UNMEASURED and reads the same value */
#define GE_LIST_SYNC_PAUSED 4u      /* sceGeListSync(qid, 1) on a list sceGeBreak(0) paused mid-draw; PSP-GE-CONTROL-001, PSP-measured 2026-10-15 (eligible re-run, probe c642039a). A list paused out of a stall, the only pause the runtime produces, is UNMEASURED and reads the same value */
#define GE_DRAW_SYNC_STALLED 3u     /* sceGeDrawSync(1) with one list stalled at its start address, read after a BUSY dequeue; PSP-GE-CONTROL-001, PSP-measured 2026-10-15 (eligible re-run, probe c642039a). A list stalled partway through is UNMEASURED */
#define GE_SYNC_DRAWING 2u          /* sceGeDrawSync(1) and sceGeListSync(qid, 1) on a list the GE is drawing, until its finish interrupt has been handled, then 0; PSP-3000 CASE=ge-timing C3, measured 2026-10-25 (issue #1296). Used by the opt-in SR_GE_TIMING model */
#define GE_DRAW_SYNC_PAUSED 2u      /* sceGeDrawSync(1) with a list sceGeBreak(0) paused mid-draw; PSP-GE-CONTROL-001, PSP-measured 2026-10-15 (eligible re-run, probe c642039a). A list paused out of a stall is UNMEASURED */
#define GE_ERROR_NO_ACTIVE_LIST 0x80000020u /* sceGeBreak(0) with no active list; PSP-GE-CONTROL-001, PSP-measured 2026-10-15 (eligible re-run, probe c642039a) */
#define GE_ERROR_INVALID_BREAK_MODE 0x80000107u /* sceGeBreak(-1), parameter untouched; PSP-GE-CONTROL-001, PSP-measured 2026-10-15 (eligible re-run, probe c642039a) */
#define GE_ERROR_INVALID_LIST_ID 0x80000100u /* sceGeListSync(qid, 1) on a list a sceGeBreak(1) reset removed; PSP-GE-CONTROL-001, PSP-measured 2026-10-15 (eligible re-run, probe c642039a) */
#define GE_ERROR_LIST_BUSY 0x80000021u /* sceGeListDeQueue on a stalled list; PSP-GE-CONTROL-001, PSP-measured 2026-10-15 (eligible re-run, probe c642039a). On a completed list too: PSP-GE-SYNC-001 ge-list-sync-completed-poll, captured */
#define GE_ERROR_INVALID_MODE 0x80000107u /* sceGeListSync(qid, 2), sceGeDrawSync(2) and sceGeDrawSync(-1); PSP-GE-SYNC-001 ge-sync-invalid-mode, captured 2026-10-31 */
#define GE_ERROR_OUT_OF_MEMORY 0x80000022u /* sceGeListEnQueue with all 64 lists in use; uofw src/kd/ge/ge.c _sceGeListEnQueue, UNMEASURED */

/* Park the calling thread until the next GE list transition. Returns 1 to re-read the
 * list table; 0 with *result set when the wait ended without one (a released or
 * deleted wait) or cannot happen. A blocking sync that has no waitable guest thread,
 * or runs with interrupts or dispatch disabled, can never be completed by a producer
 * thread; the PSP result there is UNMEASURED, so the run stops by name rather than
 * report it complete. */
static int ge_sync_block(const char *api, uint32_t *result) {
    if (!sched_guest_thread_running() || !sched_wait_permitted()) {
        *result = hle_unmodelled_stop(
            api, "blocking wait outside a waitable guest thread has no measured result");
        return 0;
    }
    sched_block_on(GE_SYNC_WAIT_OBJ);
    uint32_t woken = 0;
    if (sched_take_wake_result(&woken)) {
        *result = woken;
        return 0;
    }
    return 1;
}
static int s_ge_free_ready;                      /* the FIFO has been built */
uint64_t s_ge_list_order;

/* Queue ids: GE_LIST_ID_BASE with the slot in bits 6-11. The driver's ids are the list's
 * address XOR a per-boot mask (uofw sceGeInit); these keep their 0x35 prefix and name the
 * slot the same way, so ids repeat when slots do. */
#define GE_LIST_ID_BASE 0x35000000u
#define GE_LIST_ID_SLOT_SHIFT 6u

static uint32_t ge_list_id(int slot) { return GE_LIST_ID_BASE | ((uint32_t)slot << GE_LIST_ID_SLOT_SHIFT); }

/* The slot an id names, or -1 for an id no enqueue can return: 0x0badf00d and -1 read
 * 0x80000100 in both sync forms (PSP-GE-SYNC-001 ge-list-sync-unknown-poll and -wait). */
static int ge_list_slot(uint32_t id) {
    if ((id & ~((uint32_t)(GE_LIST_MAX - 1) << GE_LIST_ID_SLOT_SHIFT)) != GE_LIST_ID_BASE) return -1;
    return (int)((id >> GE_LIST_ID_SLOT_SHIFT) & (GE_LIST_MAX - 1u));
}

/* Every slot free, in slot order: the driver's queue at boot and after sceGeBreak(1). */
void ge_free_rebuild(void) {
    for (int i = 0; i < GE_LIST_MAX; i++) s_ge_free_slots[i] = (uint8_t)i;
    s_ge_free_head = 0;
    s_ge_free_count = GE_LIST_MAX;
    s_ge_free_ready = 1;
}

static int ge_free_pop(void) {
    if (!s_ge_free_ready) ge_free_rebuild();
    if (s_ge_free_count == 0u) return -1;
    int slot = s_ge_free_slots[s_ge_free_head];
    s_ge_free_head = (s_ge_free_head + 1u) % GE_LIST_MAX;
    s_ge_free_count--;
    return slot;
}

/* The active queue's head: the oldest list that is queued, stalled or paused, else -1. A list
 * that is drawing in the timing model has run its commands and does not hold the queue. */
static int ge_queue_head(void) {
    int head = -1;
    for (int i = 0; i < GE_LIST_MAX; i++) {
        int st = s_ge_lists[i].status;
        if ((st == GE_LIST_QUEUED || st == GE_LIST_STALLED || st == GE_LIST_PAUSED) &&
            (head < 0 || s_ge_lists[i].order < s_ge_lists[head].order))
            head = i;
    }
    return head;
}
uint64_t s_ge_busy_until_ns;       /* the modelled GE is busy until this guest time */
static uint64_t s_ge_timing_seq;          /* completion order between equal times */

static uint64_t ge_timing_now_ns(void) {
    sched_vtime_refresh();
    return sched_vtime_us() * 1000u;
}

/* Put `clk` GE clocks of work on the timeline, at the current bus clock. */
static void ge_timing_add_clk(double clk) {
    uint32_t mhz = sr_power_bus_clock_mhz();
    /* scePowerSetClockFrequency retains its request verbatim; a zero bus clock, which no PSP
     * runs at, keeps the boot clock rather than dividing by zero. */
    if (mhz == 0u) mhz = SR_POWER_BOOT_BUS_MHZ;
    uint64_t now = ge_timing_now_ns();
    if (s_ge_busy_until_ns < now) s_ge_busy_until_ns = now;
    if (clk > 0.0) s_ge_busy_until_ns += (uint64_t)(clk * 1000.0 / (double)mhz + 0.5);  /* nearest ns */
}

/* Charge the chunk the last ge_run_list call executed. */
static void ge_timing_charge_run(void) {
    ge_timing_add_clk(ge_timing_run_clk());
}

/* The GE ran the list in `slot` to its END.  By default it completes now and its finish
 * callback runs in this call, as it always has; with the model on it draws until the GE
 * timeline reaches the end of its work. */
static void ge_list_reached_end(CpuState *s, int slot) {
    GeListInfo *l = &s_ge_lists[slot];
    l->executed = 1;
    l->finish_id = ge_last_finish_id();
    if (ge_timing_on()) {
        l->status = GE_LIST_DRAWING;
        l->done_us = (s_ge_busy_until_ns + 999u) / 1000u;
        l->finish_seq = ++s_ge_timing_seq;
        s_ge_timing_drawing++;
        if (ge_log_on())
            fprintf(stderr, "GE_TIMING: list_id=0x%08x drawing until %llu us\n",
                    l->uid, (unsigned long long)l->done_us);
        return;
    }
    ge_list_completed(slot);
    ge_finish_callback(s, l->cbid, l->finish_id, l->cbarg);
}

/* The latest completion time still ahead of the guest clock among drawing lists (`slot` < 0:
 * every list), else 0. */
static uint64_t ge_timing_drawing_until(int slot) {
    uint64_t now = ge_timing_now_ns() / 1000u;
    uint64_t until = 0;
    for (int i = 0; i < GE_LIST_MAX; i++) {
        if (slot >= 0 && i != slot) continue;
        if (s_ge_lists[i].status == GE_LIST_DRAWING && s_ge_lists[i].done_us > now &&
            s_ge_lists[i].done_us > until)
            until = s_ge_lists[i].done_us;
    }
    return until;
}

/* Delay the calling thread until no list (`slot` < 0) or the list in `slot` is still drawing.
 * A caller with no thread (interrupt context) cannot be delayed and returns at once. */
static void ge_timing_wait(int slot) {
    uint64_t until;
    while (sched_current_uid() != 0u && (until = ge_timing_drawing_until(slot)) != 0u)
        sched_delay_current(until - sched_vtime_us());
}

static uint32_t h_GeGetCmd(CpuState *s) {
    (void)s;
    if (A0 > 0xffu) return 0x800001feu; /* SCE_KERNEL_ERROR_INVALID_VALUE */
    return ge_get_cmd(A0);
}

/* Stall addresses that arrived after the GE already consumed the list. Counted
 * for the selftest and for SR_GELOG triage: a ring-buffer guest that flushes more
 * chunks than the list has commands is normal traffic, not an error. */
static unsigned s_ge_late_stall_ignored;

/* Start the list in `slot`: it becomes the head and the GE runs it until it stalls or reaches
 * its END. stall == list is an empty ring: running it now would read uninitialised eDRAM, so it
 * stays stalled for sceGeListUpdateStallAddr to drive. A stall address of 0 runs it to its END. */
static void ge_list_start(CpuState *s, int slot) {
    GeListInfo *l = &s_ge_lists[slot];
    l->status = GE_LIST_STALLED;
    l->busy = 1;
    if (ge_timing_on()) ge_timing_add_clk(ge_timing_list_clk());
    if (l->stall_addr != 0 && l->stall_addr == l->start_pc) {
        ge_enqueue_trace_result(s, "enqueue", l->uid, "deferred", l->start_pc, 0);
        if (ge_log_on())
            fprintf(stderr, "GE_ENQ: list_id=0x%08x list=0x%08x stall=0x%08x cbid=%u -> DEFERRED (stall==list)\n",
                    l->uid, l->start_pc, l->stall_addr, l->cbid);
        return;
    }
    g_ge_stall_addr = l->stall_addr;
    uint32_t next_pc = ge_run_list(l->start_pc, 0);
    g_ge_stall_addr = 0;
    if (ge_timing_on()) ge_timing_charge_run();

    ge_enqueue_trace_result(s, "enqueue", l->uid, next_pc == 0 ? "done" : "stalled", next_pc, next_pc == 0);
    if (ge_log_on())
        fprintf(stderr, "GE_ENQ: list_id=0x%08x list=0x%08x stall=0x%08x cbid=%u -> next_pc=0x%08x %s\n",
                l->uid, l->start_pc, l->stall_addr, l->cbid, next_pc, next_pc == 0 ? "(DONE)" : "(stalled)");
    if (next_pc == 0) {
        SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_LIST_FINISH,
                               l->uid, l->start_pc, l->stall_addr, next_pc);
        ge_list_reached_end(s, slot);
    } else {
        /* Stalled short of its END: the guest advances it with sceGeListUpdateStallAddr. Do NOT
         * drain here: that runs past the game's write head into uninitialised eDRAM. */
        l->current_pc = next_pc;
    }
}

/* Run the queue: while the head is a list that has not started, start it. A head that stalls or
 * is paused holds the lists behind it, as the GE runs one list at a time (uofw
 * _sceGeFinishInterrupt starts the next active list once one completes). */
static void ge_queue_run(CpuState *s) {
    for (;;) {
        int head = ge_queue_head();
        if (head < 0 || s_ge_lists[head].status != GE_LIST_QUEUED) return;
        ge_list_start(s, head);
    }
}

static uint32_t h_GeListEnQueue(CpuState *s) {
    uint32_t list = A0;
    uint32_t stall = A1;
    uint32_t cbid = A2;
    uint32_t cbarg = A3;
    int slot = ge_free_pop();
    uint32_t list_id = slot >= 0 ? ge_list_id(slot) : GE_ERROR_OUT_OF_MEMORY;
    SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_LIST_ENQUEUE,
                           list, stall, list_id, cbid);

    ge_enqueue_trace_emit(s, "enqueue", list_id, list, stall, cbid);
    if (slot < 0) {
        if (ge_log_on())
            fprintf(stderr, "GE_ENQ: list=0x%08x refused: all %d lists in use\n", list, GE_LIST_MAX);
        return GE_ERROR_OUT_OF_MEMORY;
    }

    GeListInfo *l = &s_ge_lists[slot];
    memset(l, 0, sizeof(*l));
    l->uid = list_id;
    l->start_pc = list;
    l->current_pc = list;
    l->stall_addr = stall;
    l->cbid = cbid;
    l->cbarg = cbarg;
    l->order = ++s_ge_list_order;
    s_ge_list_done_bits &= ~ge_slot_bit(slot);

    if (ge_queue_head() >= 0) {
        /* Another list holds the GE: this one waits its turn. */
        l->status = GE_LIST_QUEUED;
        ge_enqueue_trace_result(s, "enqueue", list_id, "queued", list, 0);
        if (ge_log_on())
            fprintf(stderr, "GE_ENQ: list_id=0x%08x list=0x%08x stall=0x%08x cbid=%u -> QUEUED\n",
                    list_id, list, stall, cbid);
        return list_id;
    }
    ge_list_start(s, slot);
    ge_queue_run(s);
    return list_id;
}

/* With an idle queue, the host runs an enqueued list synchronously, so head
 * and tail insertion have the same observable result. If a stalled list is
 * outstanding, queue order would matter and this runtime cannot honor head
 * insertion yet. */
static uint32_t h_GeListEnQueueHead(CpuState *s) {
    for (int i = 0; i < GE_LIST_MAX; i++) {
        if (s_ge_lists[i].status == GE_LIST_STALLED || s_ge_lists[i].status == GE_LIST_PAUSED) {
            return hle_unmodelled_stop("sceGeListEnQueueHead",
                                       "queue ordering is not supported yet");
        }
    }
    return h_GeListEnQueue(s);
}

static uint32_t h_GeListUpdateStallAddr(CpuState *s) {
    uint32_t list_id = A0;
    uint32_t new_stall = A1;

    /* A list the GE already ran to its END is found too: the stall address that follows its
     * consumption is a defined no-op, not an unknown list. */
    int slot = ge_list_slot(list_id);
    if (slot >= 0 && s_ge_lists[slot].status == GE_LIST_FREE) slot = -1;

    ge_enqueue_trace_emit(s, "update_stall", list_id,
                          slot >= 0 ? s_ge_lists[slot].start_pc : 0u,
                          new_stall,
                          slot >= 0 ? s_ge_lists[slot].cbid : 0u);
    SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_LIST_STALL_UPDATE,
                           list_id, slot >= 0 ? s_ge_lists[slot].start_pc : 0u,
                           new_stall, slot >= 0 ? s_ge_lists[slot].stall_addr : 0u);

    if (slot == -1) {
        ge_enqueue_trace_result(s, "update_stall", list_id, "not_found", 0u, 0);
        if (ge_log_on())
            fprintf(stderr, "GE_UPDATE_STALL: list_id=0x%08x NOT FOUND or not stalled\n", list_id);
        return 0;
    }

    if (s_ge_lists[slot].executed) {
        /* The GE already ran this list to its END, so the list is consumed. A stall
         * address that arrives after that is a no-op on hardware: running the list
         * again would execute the same frame once per chunk the guest flushed, which
         * is what a GU ring buffer that issues one sceGeListUpdateStallAddr per chunk
         * does to every chunk after the one carrying the END. */
        s_ge_late_stall_ignored++;
        ge_enqueue_trace_result(s, "update_stall", list_id, "already_executed", 0u, 0);
        if (ge_log_on())
            fprintf(stderr, "GE_UPDATE_STALL: list_id=0x%08x already executed; stall=0x%08x ignored\n",
                    list_id, new_stall);
        return 0;
    }

    if (s_ge_lists[slot].status == GE_LIST_PAUSED || s_ge_lists[slot].status == GE_LIST_QUEUED) {
        /* A paused list keeps the new stall address until it is continued, a queued one
         * until its turn comes. */
        s_ge_lists[slot].stall_addr = new_stall;
        ge_enqueue_trace_result(s, "update_stall", list_id,
                                s_ge_lists[slot].status == GE_LIST_PAUSED ? "paused" : "queued",
                                s_ge_lists[slot].current_pc, 0);
        if (ge_log_on())
            fprintf(stderr, "GE_UPDATE_STALL: list_id=0x%08x is %s; updated stall=0x%08x without running it\n",
                    list_id, s_ge_lists[slot].status == GE_LIST_PAUSED ? "paused" : "queued", new_stall);
        return 0;
    }

    if (ge_log_on())
        fprintf(stderr, "GE_UPDATE_STALL: list_id=0x%08x stall=0x%08x cur_pc=0x%08x -> new_stall=0x%08x\n",
                list_id, s_ge_lists[slot].stall_addr, s_ge_lists[slot].current_pc, new_stall);

    s_ge_lists[slot].stall_addr = new_stall;
    g_ge_stall_addr = new_stall;

    uint32_t next_pc = ge_run_list(s_ge_lists[slot].current_pc, 1); // 1 = resume
    g_ge_stall_addr = 0;
    if (ge_timing_on()) ge_timing_charge_run();

    ge_enqueue_trace_result(s, "update_stall", list_id,
                            next_pc == 0 ? "done" : "stalled",
                            next_pc, next_pc == 0);

    if (ge_log_on())
        fprintf(stderr, "GE_UPDATE_STALL: ge_run_list returned 0x%08x %s\n",
                next_pc, next_pc == 0 ? "(COMPLETED)" : "(stalled)");

    if (next_pc == 0) {
        SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_LIST_FINISH,
                               list_id, s_ge_lists[slot].start_pc,
                               s_ge_lists[slot].stall_addr, next_pc);
        if (ge_log_on())
            fprintf(stderr, "GE_UPDATE_STALL: list DONE, firing finish callback cbid=%u\n", s_ge_lists[slot].cbid);
        ge_list_reached_end(s, slot);
        ge_queue_run(s);
    } else {
        s_ge_lists[slot].current_pc = next_pc;
    }

    return 0;
}

/* Selftest accessor: how many stall addresses arrived for a list the GE had
 * already run to its END. */
unsigned sr_hle_test_ge_late_stall_ignored(void) { return s_ge_late_stall_ignored; }

/* sceGeListSync(qid, syncType). An id that names no slot is 0x80000100 in both forms, and a
 * sync type other than 0 or 1 is 0x80000107 (PSP-GE-SYNC-001 ge-list-sync-unknown-poll,
 * ge-list-sync-unknown-wait, ge-sync-invalid-mode; the id is checked first, as in uofw).
 *   1, the peek: the list's state. Completed 0, queued behind the head 1 (uofw, UNMEASURED),
 *      drawing in the timing model 2 (CASE=ge-timing C3), stalled 3 and paused 4
 *      (PSP-GE-CONTROL-001), and 0x80000100 once the slot holds no list: after a
 *      sceGeBreak(1) reset (PSP-GE-CONTROL-001, PSP-GE-SYNC-001 ge-list-sync-cancelled-poll)
 *      or a sceGeDrawSync(0) (uofw, UNMEASURED). Known divergence: the PSP also
 *      read some lists as 0x80000100 within 1 ms of their completion, with no
 *      sceGeDrawSync(0) in between, and others as 0 (ge-list-sync-after-reenqueue,
 *      ge-list-sync-stale-1..3); the driver's list lifecycle has no transition that frees
 *      a slot there, so this keeps 0 until a sceGeDrawSync(0).
 *   0, the wait: returns 0 once the slot's completion bit is set, as the driver waits on the
 *      list's event flag bit (see GE_LIST_FREE above). A caller with no guest thread to park
 *      stops the run by name instead. */
static uint32_t h_GeListSync(CpuState *s) {
    uint32_t qid = A0;
    uint32_t syncType = A1;
    int slot = ge_list_slot(qid);
    SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_LIST_SYNC,
                           qid, slot >= 0 ? s_ge_lists[slot].start_pc : 0u,
                           slot >= 0 ? s_ge_lists[slot].stall_addr : 0u, syncType);
    if (slot < 0) {
        if (ge_log_on())
            fprintf(stderr, "GE_SYNC: qid=0x%08x names no list slot\n", qid);
        return GE_ERROR_INVALID_LIST_ID;
    }
    GeListInfo *l = &s_ge_lists[slot];
    if (ge_log_on())
        fprintf(stderr, "GE_SYNC: qid=0x%08x syncType=%u status=%d (cur_pc=0x%08x)\n",
                qid, syncType, l->status, l->current_pc);
    if (syncType == 0u) {
        for (;;) {
            if (l->status == GE_LIST_DRAWING) {
                /* Opt-in timing model: delay until the modelled completion has been delivered.
                 * A caller that cannot be delayed (interrupt context), or whose completion
                 * interrupt cannot be delivered (interrupts suspended), would otherwise be told
                 * the list is done before its finish callback ran; that has no measured result,
                 * so the run stops by name. */
                ge_timing_wait(slot);
                if (l->status == GE_LIST_DRAWING)
                    return hle_unmodelled_stop(
                        "sceGeListSync",
                        "blocking wait for a drawing list whose completion cannot be delivered here "
                        "has no measured result");
                continue;
            }
            if (s_ge_list_done_bits & ge_slot_bit(slot)) break;
            /* probe.c records that a blocking sync on a paused list did not return on PSP-3000;
             * a stalled or queued list returns once a producer thread lets it complete. */
            uint32_t result = 0;
            if (!ge_sync_block("sceGeListSync", &result)) return result;
        }
        ge_transition_trace_check("listsync", s_vcount);
        return 0;
    }
    if (syncType != 1u) return GE_ERROR_INVALID_MODE;
    switch (l->status) {
    case GE_LIST_QUEUED: return 1u; /* uofw SCE_GE_LIST_QUEUED */
    case GE_LIST_STALLED: return GE_LIST_SYNC_STALLED;
    case GE_LIST_PAUSED: return GE_LIST_SYNC_PAUSED;
    case GE_LIST_DRAWING: return GE_SYNC_DRAWING;
    case GE_LIST_COMPLETED:
        ge_transition_trace_check("listsync", s_vcount);
        return 0;
    default: return GE_ERROR_INVALID_LIST_ID;
    }
}

static uint32_t h_GeUnsetCallback(CpuState *s) {
    uint32_t cbid = A0;
    if (cbid < (uint32_t)(sizeof(s_ge_cb) / sizeof(s_ge_cb[0]))) {
        s_ge_cb[cbid].used = 0;
    }
    if (getenv("SR_GELOG")) {
        fprintf(stderr, "sceGeUnsetCallback: cbid=%u\n", cbid);
    }
    return 0;
}

static uint32_t h_GeEdramGetSize(CpuState *s) {
    (void)s;
    return 0x00200000u;
}

/* sceGeEdramSetAddrTranslation(int width) (PSPSDK pspge.h): width 0 leaves the translation width
 * unset, and 512, 1024, 2048 and 4096 set it. The return is the previous width when one was set,
 * Captured on the PSP-3000 oracle (2026-10-10, hle-ge-edram; CAPTURED, not acceptance-eligible, see
 * tools/psp_oracle/manifest.json): every call sets the given width and
 * returns the width that was set before it, width 0 included, so there is no pure query; the boot
 * value is 1024. The retained width is reported back, but this runtime does not apply address
 * translation to GE memory accesses, so the setting changes no rendering. The error code for an
 * unsupported width is not documented: 0x80000107 (the GE invalid-mode code measured on PSP-3000
 * for sceGeBreak) is a project choice, UNMEASURED here. */
#define GE_EDRAM_WIDTH_BOOT 1024u
static uint32_t s_ge_edram_width = GE_EDRAM_WIDTH_BOOT;
static uint32_t h_GeEdramSetAddrTranslation(CpuState *s) {
    uint32_t width = A0;
    uint32_t previous = s_ge_edram_width;
    if (width != 0u && width != 512u && width != 1024u && width != 2048u && width != 4096u) return 0x80000107u;
    s_ge_edram_width = width;
    return previous;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only reset to the measured boot width. */
void sr_hle_test_ge_edram_reset(void) { s_ge_edram_width = GE_EDRAM_WIDTH_BOOT; }
#endif
static uint32_t h_GeDrawSync(CpuState *s) {
    /* sceGeDrawSync(mode): wait for (mode 0) or peek at (mode 1) the end of every queued GE
     * list. Any other mode is 0x80000107 (PSP-GE-SYNC-001 ge-sync-invalid-mode: 2 and -1).
     * The peek reads the active queue's head, as the driver does (uofw sceGeDrawSync): 0 with
     * no active list, 2 for a paused head and 3 for a stalled one (PSP-GE-CONTROL-001), 2
     * while the timing model has a list drawing (CASE=ge-timing C3). The wait parks the caller
     * until no list is active (#1374): a stalled list advances only when a guest thread calls
     * sceGeListUpdateStallAddr, so the waiter blocks through the scheduler and lets that
     * producer run. It returns 0 with every completed list's slot freed, so their peeks then
     * read 0x80000100 (uofw, UNMEASURED). Known divergence:
     * after a sceGeBreak(1) reset of a stalled list the PSP's wait returned in one run and did
     * not return in another (ge-draw-sync-after-reset-wait); with no list active this returns. */
    uint32_t mode = A0;
    if (mode > 1u) return GE_ERROR_INVALID_MODE;
    int recorded = 0;
    for (;;) {
        /* Opt-in timing model: wait for every drawing list first. A list a producer released
         * while this call waited can be drawing on the next pass. */
        if (s_ge_timing_drawing) {
            if (mode == 1u) return GE_SYNC_DRAWING;
            ge_timing_wait(-1);
            /* As sceGeListSync: a completion this caller cannot wait for is never reported. */
            if (s_ge_timing_drawing)
                return hle_unmodelled_stop(
                    "sceGeDrawSync",
                    "blocking wait for drawing lists whose completion cannot be delivered here "
                    "has no measured result");
        }
        int head = ge_queue_head();
        if (!recorded) {
            SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_DRAW_SYNC,
                                   mode, (uint32_t)(head >= 0), 0u, 0u);
            recorded = 1;
        }
        if (mode == 1u) {
            if (head < 0) return 0u;
            return s_ge_lists[head].status == GE_LIST_PAUSED ? GE_DRAW_SYNC_PAUSED : GE_DRAW_SYNC_STALLED;
        }
        if (head < 0) break;
        uint32_t result = 0;
        if (!ge_sync_block("sceGeDrawSync", &result)) return result;
    }
    for (int i = 0; i < GE_LIST_MAX; i++) {
        if (s_ge_lists[i].status == GE_LIST_COMPLETED) {
            s_ge_lists[i].status = GE_LIST_FREE;
            s_ge_lists[i].busy = 0;
        }
    }
    ge_transition_trace_check("drawsync", s_vcount);
    return 0;
}

static uint32_t h_GeBreak(CpuState *s) {
    uint32_t mode = A0;
    uint32_t pparam = A1;

    if (mode > 1u) {
        /* 0x80000107 is measured for mode -1 (GE_ERROR_INVALID_BREAK_MODE); the same code is
         * applied to any mode above 1, which is UNMEASURED. */
        return GE_ERROR_INVALID_BREAK_MODE;
    }
    if (pparam != 0u && (!sr_guest_span_readable(pparam, 16u) || (pparam & 3u) != 0u)) {
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }

    int active_slot = -1;
    for (int i = 0; i < GE_LIST_MAX; i++) {
        if (s_ge_lists[i].status == GE_LIST_STALLED) {
            active_slot = i;
            break;
        }
    }

    if (mode == 0u) {
        /* Mode 0: pause current drawing list at command boundary */
        if (active_slot == -1) {
            return GE_ERROR_NO_ACTIVE_LIST;
        }
        s_ge_lists[active_slot].status = GE_LIST_PAUSED;
        SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_LIST_SYNC,
                               s_ge_lists[active_slot].uid, s_ge_lists[active_slot].start_pc,
                               s_ge_lists[active_slot].stall_addr, mode);
        if (ge_log_on())
            fprintf(stderr, "GE_BREAK: paused list_id=0x%08x cur_pc=0x%08x\n",
                    s_ge_lists[active_slot].uid, s_ge_lists[active_slot].current_pc);
        /* PSP-measured (PSP-GE-CONTROL-001, eligible 2026-10-15 re-run): a mode 0 break of a list
         * running mid-draw returned that list's queue id. Breaking a stalled list, the only active
         * list the runtime has, is UNMEASURED and returns its queue id by the same rule. Known
         * divergence: a list enqueued with no stall address runs to completion inside
         * sceGeListEnQueue, so on the measured sequence the runtime has no active list and returns
         * 0x80000020, and the following ListSync and DrawSync peeks read 0 where the PSP read the
         * queue id, 4 and 2. */
        return s_ge_lists[active_slot].uid;
    } else {
        /* Mode 1: reset every queue and return 0. PSP-measured (PSP-GE-CONTROL-001, eligible
         * 2026-10-15 re-run): a reset with one list stalled at its stall address returned 0,
         * not that list's queue id. A reset with a paused list is UNMEASURED and returns 0 by
         * the same rule. */
        /* The driver frees every slot and rebuilds its free list; it sets no list's event flag
         * bit, so a blocking sync on a removed list stays parked (PSP-GE-SYNC-001
         * ge-list-sync-cancelled-wait: no return). */
        for (int i = 0; i < GE_LIST_MAX; i++) {
            if (ge_log_on() && s_ge_lists[i].status != GE_LIST_FREE &&
                s_ge_lists[i].status != GE_LIST_COMPLETED)
                fprintf(stderr, "GE_BREAK: cancelled queue list_id=0x%08x\n", s_ge_lists[i].uid);
            s_ge_lists[i].status = GE_LIST_FREE;
            s_ge_lists[i].busy = 0;
        }
        s_ge_timing_drawing = 0;
        ge_free_rebuild();
        ge_sync_wake();
        return 0u;
    }
}

static uint32_t h_GeContinue(CpuState *s) {
    int paused_slot = -1;
    for (int i = 0; i < GE_LIST_MAX; i++) {
        if (s_ge_lists[i].status == GE_LIST_PAUSED) {
            paused_slot = i;
            break;
        }
    }
    if (paused_slot == -1) {
        /* PSP-measured (PSP-GE-CONTROL-001, eligible 2026-10-15 re-run): continue with no paused list returns 0. */
        return 0;
    }

    s_ge_lists[paused_slot].status = GE_LIST_STALLED;
    if (ge_log_on())
        fprintf(stderr, "GE_CONTINUE: resuming list_id=0x%08x cur_pc=0x%08x stall=0x%08x\n",
                s_ge_lists[paused_slot].uid, s_ge_lists[paused_slot].current_pc,
                s_ge_lists[paused_slot].stall_addr);

    if (s_ge_lists[paused_slot].stall_addr == 0 ||
        s_ge_lists[paused_slot].current_pc != s_ge_lists[paused_slot].stall_addr) {
        g_ge_stall_addr = s_ge_lists[paused_slot].stall_addr;
        uint32_t next_pc = ge_run_list(s_ge_lists[paused_slot].current_pc, 1); /* 1 = resume */
        g_ge_stall_addr = 0;
        if (ge_timing_on()) ge_timing_charge_run();

        if (next_pc == 0) {
            SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_LIST_FINISH,
                                   s_ge_lists[paused_slot].uid, s_ge_lists[paused_slot].start_pc,
                                   s_ge_lists[paused_slot].stall_addr, next_pc);
            ge_list_reached_end(s, paused_slot);
            ge_queue_run(s);
        } else {
            s_ge_lists[paused_slot].current_pc = next_pc;
            s_ge_lists[paused_slot].status = GE_LIST_STALLED;
        }
    }

    return 0;
}

/* sceGeListDeQueue(qid): an id that names no list is 0x80000100 (PSP-GE-SYNC-001
 * ge-list-dequeue-unknown). A list that has started is BUSY (0x80000021) and stays where it
 * is, whether stalled (PSP-GE-CONTROL-001: the next peeks still read 3) or completed and not
 * yet freed by a sceGeDrawSync(0) (PSP-GE-SYNC-001 ge-list-sync-completed-poll). A queued
 * list that has not started leaves the queue, its slot is freed and its waiters return 0
 * (uofw sceGeListDeQueue, UNMEASURED). */
static uint32_t h_GeListDeQueue(CpuState *s) {
    uint32_t qid = A0;
    int slot = ge_list_slot(qid);
    if (slot < 0 || s_ge_lists[slot].status == GE_LIST_FREE) return GE_ERROR_INVALID_LIST_ID;
    if (s_ge_lists[slot].busy) return GE_ERROR_LIST_BUSY;
    s_ge_lists[slot].status = GE_LIST_FREE;
    ge_free_push(slot);
    s_ge_list_done_bits |= ge_slot_bit(slot);
    ge_sync_wake();
    return 0;
}

static uint32_t h_GeEdramGetAddr(CpuState *s) { (void)s; return 0x04000000; }
static uint32_t h_GeSetCallback(CpuState *s) {
    uint32_t info = A0;
    if (!info || !sr_guest_span_readable(info, 16u))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(s_ge_cb) / sizeof(s_ge_cb[0])); i++) {
        if (!s_ge_cb[i].used) {
            s_ge_cb[i].used = 1;
            s_ge_cb[i].signal_func = MEM_R32(info + 0);
            s_ge_cb[i].signal_arg  = MEM_R32(info + 4);
            s_ge_cb[i].finish_func = MEM_R32(info + 8);
            s_ge_cb[i].finish_arg  = MEM_R32(info + 12);
            if (ge_log_on())
                fprintf(stderr, "GE_SET_CB: cbid=%u sig=0x%08x/0x%08x fin=0x%08x/0x%08x\n",
                        i, s_ge_cb[i].signal_func, s_ge_cb[i].signal_arg,
                        s_ge_cb[i].finish_func, s_ge_cb[i].finish_arg);
            return i;
        }
    }
    return 0xffffffffu;
}

void hle_register_ge_handlers(void) {
    /* sceGe_user */
    sr_hle_register(0xab49e76a, "sceGeListEnQueue", h_GeListEnQueue);
    sr_hle_register(0xb287bd61, "sceGeDrawSync", h_GeDrawSync);
    sr_hle_register(0xe47e40e4, "sceGeEdramGetAddr", h_GeEdramGetAddr);
    sr_hle_register(0xa4fc06a4, "sceGeSetCallback", h_GeSetCallback);
    sr_hle_register(0x03444eb4, "sceGeListSync", h_GeListSync);
    sr_hle_register(0x05db22ce, "sceGeUnsetCallback", h_GeUnsetCallback);
    sr_hle_register(0x1f6752ad, "sceGeEdramGetSize", h_GeEdramGetSize);
    sr_hle_register(0xb77905ea, "sceGeEdramSetAddrTranslation", h_GeEdramSetAddrTranslation);
    sr_hle_register(0xe0d68148, "sceGeListUpdateStallAddr", h_GeListUpdateStallAddr);
    sr_hle_register(0xdc93cfef, "sceGeGetCmd", h_GeGetCmd);
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_kernel_import_sweep_handlers(). */
void hle_register_kernel_import_sweep_ge(void) {
    sr_hle_register(0x1c0d95a6, "sceGeListEnQueueHead", h_GeListEnQueueHead);
    sr_hle_register(0x4c06e472, "sceGeContinue", h_GeContinue);
    sr_hle_register(0xb448ec0d, "sceGeBreak", h_GeBreak);
    sr_hle_register(0x5fb86ab0, "sceGeListDeQueue", h_GeListDeQueue);
    /* sceGeSaveContext / sceGeRestoreContext are refused, not faked. The modeled GE state
     * (GeState in ge_shared.h, 3452 bytes) carries 2048 bytes of CLUT RAM plus matrix banks
     * and derived fields, so it cannot fit a 2048-byte PspGeContext, and the 256-entry
     * command register file alone does not capture matrices or CLUT contents. A restore
     * therefore cannot reinstate what a save would need, so both refuse with the GE
     * controlled-refusal code and write nothing to the guest buffer. */
    sr_hle_register_unsupported(0x438a385au, "sceGeSaveContext", 0x80020002u);
    sr_hle_register_unsupported(0x0bf608fbu, "sceGeRestoreContext", 0x80020002u);
}
