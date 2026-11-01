// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Kernel time: system time, alarms, VTimers and the libc clock functions. Extracted from hle.c
 * on 2026-10-31 (#307). */

#include "hle_internal.h"

/* sceKernelSetAlarm(SceUInt clock, SceKernelAlarmHandler handler, void *common) returns an
 * alarm UID; once `clock` microseconds of guest time pass, the handler runs in interrupt
 * context with `common` as its argument, and a non-zero return value re-arms it for that
 * many microseconds (public PSPSDK pspthreadman.h prototypes).  The timer model and the
 * unknown-id code live with the scheduler (sched_alarm_set / sched_alarm_cancel).  A NULL
 * handler is refused with the project's invalid-guest-address code
 * (SCE_KERNEL_ERROR_ILLEGAL_ADDR); the console's own code for it is not measured.  A zero
 * clock is accepted and fires at the next interrupt-service point; the console's answer to
 * it is not measured either. */
static uint32_t h_SetAlarm(CpuState *s) {
    (void)s;
    if (A1 == 0u) return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    return sched_alarm_set(A0, A1, A2);
}
static uint32_t h_CancelAlarm(CpuState *s) {
    (void)s;
    return sched_alarm_cancel(A0);
}

static uint32_t h_GetSystemTimeLow(CpuState *s) {
    (void)s;
    static uint32_t last_log = 0;
    uint32_t t = (uint32_t)now_usec();
    if (hle_log_on() && t - last_log > 1000000) {
        fprintf(stderr, "HLE: GetSystemTimeLow %u\n", t);
        last_log = t;
    }
    return t;
}
static uint32_t h_GetSystemTimeWide(CpuState *s) {
    uint64_t t = now_usec();
    s->r[3] = (uint32_t)(t >> 32);
    return (uint32_t)t;
}
#define VTIMER_BAD_ID 0x800201beu     /* deleted UID: measured except GetVTimerBase */
#define VTIMER_NO_SLOT 0x80020190u    /* UNMEASURED: no free slot, past the measured 64 */

/* Guest time now. The refresh before the read is the one the timed waits make, so a running
 * timer keeps pace with the guest clock even when the scheduler is paced to the host clock. */
static uint64_t vtimer_now_us(void) {
    sched_vtime_refresh();
    return now_usec();
}

/* Guest time at the most recent Start while running, else zero. Zero once stopped is measured
 * (PSP-HLE-VTIMER-002); the value while running is a system-clock value on the PSP-3000, and
 * base = system time at Start is consistent with that run, not measured exactly. */
static uint64_t vtimer_base_us(const VTimer *t) {
    return t->running ? t->started_us : 0u;
}

static uint32_t h_CreateVTimer(CpuState *s) {
    /* a0=name, a1=SceKernelVTimerOptParam * or NULL. The kernel ignores the option struct: a
     * PSP creates the same timer for a NULL struct and for declared sizes -1, 0, 1, 4, 7, 8, 12,
     * 4096 and 0x7FFFFFFF (pspautotests tests/threads/vtimers/create.expected, recorded on a
     * PSP, at 2ed7c9c5). Only a readable struct is measured, so an unreadable one stops. */
    if (A1 && !sr_guest_span_readable(A1, 4u))
        return hle_unmodelled_stop("sceKernelCreateVTimer",
                                   "option struct at 0x%08x is unreadable, which is not measured", A1);
    VTimer *t = NULL;
    for (int i = 0; i < VTIMER_MAX; i++) {
        if (!s_vtimers[i].used) { t = &s_vtimers[i]; break; }
    }
    if (!t) return VTIMER_NO_SLOT;
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->uid = sr_alloc_uid();
    if (A0) (void)guest_cstr(A0, t->name, sizeof(t->name));
    return t->uid;
}

static uint32_t h_StartVTimer(CpuState *s) {
    /* a0=uid. Returns 1 while the timer already runs (documented), 0 when it starts. */
    VTimer *t = vtimer_lookup(A0);
    if (!t) return VTIMER_BAD_ID;
    if (t->running) return 1u;
    t->started_us = vtimer_now_us();
    t->running = 1;
    return 0;
}

static uint32_t h_StopVTimer(CpuState *s) {
    /* a0=uid. Returns 1 if the timer was running, 0 if it was not started. */
    VTimer *t = vtimer_lookup(A0);
    if (!t) return VTIMER_BAD_ID;
    if (!t->running) return 0;
    t->accum_us = vtimer_elapsed_at(t, vtimer_now_us());
    t->running = 0;
    return 1;
}

static uint32_t h_DeleteVTimer(CpuState *s) {
    /* a0=uid. The UID is never reissued (sr_alloc_uid), so a deleted UID stays unknown. */
    VTimer *t = vtimer_lookup(A0);
    if (!t) return VTIMER_BAD_ID;
    memset(t, 0, sizeof *t);
    return 0;
}

/* ---- VTimer handlers --------------------------------------------------------------------
 *
 * Measured on a PSP by pspautotests tests/threads/vtimers/{sethandler,cancelhandler}.expected
 * (2ed7c9c5, recorded on a PSP):
 *   - SetVTimerHandler(uid, SceKernelSysClock *time, handler, common) and the Wide form (the
 *     time as a 64-bit value) store the schedule, the handler and common, and return 0. A NULL
 *     handler clears the handler and leaves the schedule and common as they were.
 *   - uid 0 returns 0x800201BF, an unknown or deleted uid 0x800201BE.
 *   - The handler runs once the timer's own time reaches the schedule, also when the schedule
 *     is already past at Set (it then runs at once, and again for each return still behind).
 *     It gets (uid, SceKernelSysClock *scheduled, SceKernelSysClock *actual, common); the Wide
 *     form's handler gets the same pointers. A non-zero return is the delay to the next call,
 *     counted from the scheduled time; zero clears the handler and keeps the schedule.
 *   - CancelVTimerHandler clears the handler and keeps the schedule and common. From inside the
 *     timer's own handler it returns 0x800201BF for that timer and 0x800201BE for uid 0.
 * The handler runs as a timer interrupt (sched_set_vtimer_hooks), on a nested guest frame like
 * the GE callbacks. Not measured, so the run stops by name: a NULL time pointer (the test
 * notes it crashes) and a SetVTimerHandler from inside the timer's own handler. */
#define VTIMER_ZERO_ID 0x800201bfu

static uint32_t vtimer_set_handler(uint32_t uid, uint64_t time, uint32_t handler, uint32_t common,
                                   const char *api) {
    if (uid == 0u) return VTIMER_ZERO_ID;
    VTimer *t = vtimer_lookup(uid);
    if (!t) return VTIMER_BAD_ID;
    if (s_vtimer_in_handler == uid)
        return hle_unmodelled_stop(api, "from the timer's own handler is not measured");
    t->handler = handler;
    if (handler) {
        t->schedule = time;
        t->common = common;
    }
    return 0;
}

static uint32_t h_SetVTimerHandler(CpuState *s) {
    /* a0=uid, a1=SceKernelSysClock *time, a2=handler, a3=common. */
    if (!sr_guest_span_readable(A1, 8u))
        return hle_unmodelled_stop("sceKernelSetVTimerHandler",
                                   "with an unreadable time pointer 0x%08x is not measured", A1);
    uint64_t time = (uint64_t)MEM_R32(A1) | ((uint64_t)MEM_R32(A1 + 4u) << 32);
    return vtimer_set_handler(A0, time, A2, A3, "sceKernelSetVTimerHandler");
}

static uint32_t h_SetVTimerHandlerWide(CpuState *s) {
    /* a0=uid, a2/a3=aligned 64-bit time (a1 is skipped), t0=handler, t1=common. */
    return vtimer_set_handler(A0, ((uint64_t)A3 << 32) | A2, stack_arg(s, 0), stack_arg(s, 1),
                              "sceKernelSetVTimerHandlerWide");
}

static uint32_t h_CancelVTimerHandler(CpuState *s) {
    /* a0=uid. */
    if (s_vtimer_in_handler) {
        if (A0 == 0u) return VTIMER_BAD_ID;
        if (A0 == s_vtimer_in_handler) return VTIMER_ZERO_ID;
    }
    if (A0 == 0u) return VTIMER_ZERO_ID;
    VTimer *t = vtimer_lookup(A0);
    if (!t) return VTIMER_BAD_ID;
    t->handler = 0u;
    return 0;
}

static uint32_t h_GetVTimerTime(CpuState *s) {
    /* a0=uid, a1=SceKernelSysClock * (low word, then high word). The pointer is checked before
     * the UID, as ReferFplStatus does. */
    /* Runtime safety choice: PSP-3000 hangs on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), so no game relies on this value. */
    if (!A1 || !sr_guest_span_writable(A1, 8u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    VTimer *t = vtimer_lookup(A0);
    if (!t) return VTIMER_BAD_ID;
    uint64_t value = vtimer_elapsed_at(t, vtimer_now_us());
    MEM_W32(A1, (uint32_t)value);
    MEM_W32(A1 + 4u, (uint32_t)(value >> 32));
    return 0;
}

/* ---- VTimer time, Wide getter and the two setters ------------------------------------------
 * Measured on a PSP by pspautotests tests/threads/vtimers/gettime.expected and settime.expected
 * (rev 2ed7c9c5, recorded on a PSP):
 *   - GetVTimerTimeWide(uid) returns the same elapsed time GetVTimerTime writes, as a 64-bit
 *     value in $v0/$v1 (0 for a new timer, about 5000 after a 5 ms run, unchanged once stopped);
 *     uid 0, 0xDEADBEEF and a deleted uid return -1 (0xFFFFFFFF_FFFFFFFF), not an error code.
 *   - SetVTimerTimeWide(uid, time) makes `time` the timer's time and returns the time it held
 *     before, 64-bit in $v0/$v1: on a new timer 0, then 1 after setting 1, 0x1234567890123 after
 *     setting that, and -1 after setting -1 (the full 64 bits round-trip). Unknown, zero and
 *     deleted uids return -1 and change nothing. ReferVTimerStatus then reports current 0 after
 *     setting 0 and non-zero after any other value, while active and base stay as they were.
 *   - SetVTimerTime(uid, SceKernelSysClock *time) swaps: it returns 0, makes *time the timer's
 *     time, and writes the time it held before back into *time. uid 0, 0xDEADBEEF and a deleted
 *     uid return 0x800201BE and leave *time as it was.
 *   - Set while running with a handler due at the new time (settime.expected "While scheduled":
 *     handler scheduled at 20000, timer set to 20000 1 ms after Start) does not call the handler
 *     inside the Set; it runs after the next 1 ms delay, and its return of 100000 moves the
 *     schedule to 120000. Here the handler deadline is derived from the timer's time on every
 *     scheduler pass (vtimer_handler_deadline), so a Set takes effect at the next pass.
 * A running timer keeps running from the new time: its open interval restarts now. That the PSP
 * counts on from the set value while running is what "While scheduled" shows (the handler due
 * at 20000 runs once the timer had been set to 20000 and run on); the exact microseconds are not
 * measured. A NULL or unwritable SetVTimerTime pointer is not measured (the test notes the NULL
 * case crashes), so the run stops by name. */
static uint64_t vtimer_swap_time(VTimer *t, uint64_t value) {
    uint64_t now = vtimer_now_us();
    uint64_t previous = vtimer_elapsed_at(t, now);
    t->accum_us = value;
    if (t->running) t->started_us = now;
    return previous;
}

static uint32_t h_GetVTimerTimeWide(CpuState *s) {
    /* a0=uid. */
    VTimer *t = vtimer_lookup(A0);
    if (!t) { s->r[3] = 0xffffffffu; return 0xffffffffu; }
    uint64_t value = vtimer_elapsed_at(t, vtimer_now_us());
    s->r[3] = (uint32_t)(value >> 32);
    return (uint32_t)value;
}

static uint32_t h_SetVTimerTimeWide(CpuState *s) {
    /* a0=uid, a2/a3=aligned 64-bit time (a1 is skipped, as in SetVTimerHandlerWide). */
    VTimer *t = vtimer_lookup(A0);
    if (!t) { s->r[3] = 0xffffffffu; return 0xffffffffu; }
    uint64_t previous = vtimer_swap_time(t, ((uint64_t)A3 << 32) | A2);
    s->r[3] = (uint32_t)(previous >> 32);
    return (uint32_t)previous;
}

static uint32_t h_SetVTimerTime(CpuState *s) {
    /* a0=uid, a1=SceKernelSysClock * (low word, then high word), read and written. */
    VTimer *t = vtimer_lookup(A0);
    if (!t) return VTIMER_BAD_ID;
    if (!A1 || !sr_guest_span_writable(A1, 8u))
        return hle_unmodelled_stop("sceKernelSetVTimerTime",
                                   "with an unwritable time pointer 0x%08x is not measured", A1);
    uint64_t value = (uint64_t)MEM_R32(A1) | ((uint64_t)MEM_R32(A1 + 4u) << 32);
    uint64_t previous = vtimer_swap_time(t, value);
    MEM_W32(A1, (uint32_t)previous);
    MEM_W32(A1 + 4u, (uint32_t)(previous >> 32));
    return 0;
}

static uint32_t h_GetVTimerBase(CpuState *s) {
    /* a0=uid, a1=SceKernelSysClock *. Returns the same base as ReferVTimerStatus: measured equal
     * while running and 0 once stopped (PSP-HLE-VTIMER-002). */
    /* Runtime safety choice: PSP-3000 hangs on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), so no game relies on this value. */
    if (!A1 || !sr_guest_span_writable(A1, 8u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    VTimer *t = vtimer_lookup(A0);
    if (!t) return VTIMER_BAD_ID;
    uint64_t base = vtimer_base_us(t);
    MEM_W32(A1, (uint32_t)base);
    MEM_W32(A1 + 4u, (uint32_t)(base >> 32));
    return 0;
}

static uint32_t h_ReferVTimerStatus(CpuState *s) {
    /* a0=uid, a1=info. The caller's size word selects the write: 0 writes nothing, otherwise
     * min(size, 0x48) bytes. That is the rule the sema, event-flag and mailbox status calls
     * share (PSP-KERNEL-STATUS-001); it is applied to VTimer without a VTimer-specific cite. */
    /* Runtime safety choice: PSP-3000 hangs on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), so no game relies on this value. */
    if (!A1 || !sr_guest_span_readable(A1, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    VTimer *t = vtimer_lookup(A0);
    if (!t) return VTIMER_BAD_ID;
    uint32_t input_size = MEM_R32(A1);
    if (input_size == 0u) return 0;
    uint32_t write_len = input_size < VTIMER_INFO_SIZE ? input_size : VTIMER_INFO_SIZE;
    if (!sr_guest_span_writable(A1, write_len)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint64_t base = vtimer_base_us(t);
    uint64_t current = vtimer_elapsed_at(t, vtimer_now_us());
    SceKernelVTimerInfo info;
    memset(&info, 0, sizeof info);
    info.size = VTIMER_INFO_SIZE;
    memcpy(info.name, t->name, sizeof info.name);
    info.active = t->running ? 1u : 0u;
    info.base[0] = (uint32_t)base;
    info.base[1] = (uint32_t)(base >> 32);
    info.current[0] = (uint32_t)current;
    info.current[1] = (uint32_t)(current >> 32);
    info.schedule[0] = (uint32_t)t->schedule;
    info.schedule[1] = (uint32_t)(t->schedule >> 32);
    info.handler = t->handler;
    info.common = t->common;
    for (uint32_t i = 0; i < write_len; i++)
        MEM_W8(A1 + i, ((const uint8_t *)&info)[i]);
    return 0;
}
static uint32_t h_GetSystemTime(CpuState *s) {
    (void)s;
    return rtc_write_u64(A0, now_usec()) ? 0u : RTC_ILLEGAL_ADDR;
}
/* sceKernelSysClock2USec receives the low/high word pair of the same
 * microsecond clock exposed by GetSystemTime and writes that 64-bit value as
 * low/high output words. Read the complete input before writing either output
 * so overlapping guest spans remain deterministic. */
static uint32_t h_SysClock2USec(CpuState *s) {
    (void)s;
    if (!A0 || !sr_guest_span_readable(A0, 8u) ||
        !A1 || !A2 || !sr_guest_span_writable(A1, 4u) ||
        !sr_guest_span_writable(A2, 4u))
        return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t low = MEM_R32(A0);
    uint32_t high = MEM_R32(A0 + 4u);
    MEM_W32(A1, low);
    MEM_W32(A2, high);
    return 0;
}
/* sceKernelSysClock2USecWide(SceInt64 clock, unsigned *low, unsigned *high): the 64-bit
 * clock is passed by value, so under the o32 convention its low word is $a0 and its high
 * word is $a1, with the two output pointers in $a2 and $a3 (PSPSDK pspthreadman.h). It
 * uses the same microsecond clock representation as h_SysClock2USec, so the split is the
 * identity on the value. Both output spans are validated before either word is written. */
static uint32_t h_SysClock2USecWide(CpuState *s) {
    (void)s;
    if (!A2 || !A3 || !sr_guest_span_writable(A2, 4u) || !sr_guest_span_writable(A3, 4u))
        return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    MEM_W32(A2, A0);
    MEM_W32(A3, A1);
    return 0;
}
/* The runtime models SceKernelSysClock as a microsecond count split into low/high
 * words, so converting an unsigned microsecond duration is an identity with a
 * zero high word. Validate the complete guest span before either write. The input
 * arrives in $a0, not in guest memory, so unlike h_SysClock2USec there is no input
 * span that could overlap the output and nothing to read before writing. */
static uint32_t h_USec2SysClock(CpuState *s) {
    (void)s;
    if (!A1 || !sr_guest_span_writable(A1, 8u))
        return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    MEM_W32(A1, A0);
    MEM_W32(A1 + 4u, 0u);
    return 0u;
}
/* A 64-bit Allegrex/O32 result is returned as low $v0 and high $v1. */
static uint32_t h_USec2SysClockWide(CpuState *s) {
    s->r[3] = 0u;
    return A0;
}
static uint32_t h_LibcTime(CpuState *s) {
    (void)s;
    if (A0 && !sr_guest_span_writable(A0, 4u)) return RTC_ILLEGAL_ADDR;
    uint64_t tick = rtc_now_tick();
    uint64_t seconds = tick >= RTC_UNIX_EPOCH_TICK
        ? (tick - RTC_UNIX_EPOCH_TICK) / 1000000u : 0u;
    uint32_t value = (uint32_t)seconds;
    if (A0) MEM_W32(A0, value);
    return value;
}
/* The PSP libc clock surface uses the same one-million-tick-per-second unit as
 * sceKernelGetSystemTime; it is an elapsed guest CPU-time value, not Unix time. */
static uint32_t h_LibcClock(CpuState *s) { (void)s; return (uint32_t)now_usec(); }
/* Fill a struct timeval {sec, usec} and struct timezone {minuteswest,dsttime}.
 * Both spans are preflighted before any write. The timezone record carries the
 * system profile's settings in the BSD shape (minutes west of UTC, standard
 * offset only, and the daylight flag); what the PSP itself writes there is
 * unmeasured. */
static uint32_t h_LibcGettimeofday(CpuState *s) {
    (void)s;
    if (A0 && !sr_guest_span_writable(A0, 8u)) return RTC_ILLEGAL_ADDR;
    if (A1 && !sr_guest_span_writable(A1, 8u)) return RTC_ILLEGAL_ADDR;
    uint64_t tick = rtc_now_tick();
    uint64_t unix_usec = tick >= RTC_UNIX_EPOCH_TICK ? tick - RTC_UNIX_EPOCH_TICK : 0u;
    if (A0) {
        MEM_W32(A0, (uint32_t)(unix_usec / 1000000u));
        MEM_W32(A0 + 4u, (uint32_t)(unix_usec % 1000000u));
    }
    if (A1) {
        int32_t zone;
        uint32_t daylight;
        (void)psp_profile_local_offset_minutes(&zone, &daylight);
        MEM_W32(A1, 0u - (uint32_t)zone);
        MEM_W32(A1 + 4u, daylight);
    }
    return 0;
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_kernel_import_sweep_handlers(). */
void hle_register_kernel_import_sweep_kernel_time_1(void) {
    sr_hle_register(0x6652b8ca, "sceKernelSetAlarm", h_SetAlarm);
    sr_hle_register(0xba6b92e2, "sceKernelSysClock2USec", h_SysClock2USec);
    sr_hle_register(0x110dec9au, "sceKernelUSec2SysClock", h_USec2SysClock);
    sr_hle_register(0x7e65b999, "sceKernelCancelAlarm", h_CancelAlarm);
}

/* From hle_register_kernel_import_sweep_handlers(). */
void hle_register_kernel_import_sweep_kernel_time_2(void) {
    sr_hle_register(0x034a921fu, "sceKernelGetVTimerTime", h_GetVTimerTime);
}

/* From hle_register_time_handlers(). */
void hle_register_time_kernel_time_1(void) {
    sr_hle_register(0x369ed59d, "sceKernelGetSystemTimeLow", h_GetSystemTimeLow);
    sr_hle_register(0x82bc5777, "sceKernelGetSystemTimeWide", h_GetSystemTimeWide);
    sr_hle_register(0xdb738f35, "sceKernelGetSystemTime", h_GetSystemTime);
}

/* From hle_register_time_handlers(). */
void hle_register_time_kernel_time_2(void) {
    sr_hle_register(0x91e4f6a7, "sceKernelLibcClock", h_LibcClock);
    sr_hle_register(0x27cc57f0, "sceKernelLibcTime", h_LibcTime);
    sr_hle_register(0x71ec4271, "sceKernelLibcGettimeofday", h_LibcGettimeofday);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_kernel_time_1(void) {
    sr_hle_register(0xe1619d7cu, "sceKernelSysClock2USecWide", h_SysClock2USecWide);
    sr_hle_register(0xc8cd158cu, "sceKernelUSec2SysClockWide", h_USec2SysClockWide);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_kernel_time_2(void) {
    sr_hle_register(0x20fff560u, "sceKernelCreateVTimer", h_CreateVTimer);
    sr_hle_register(0xc68d9437u, "sceKernelStartVTimer", h_StartVTimer);
    sr_hle_register(0xd0aeee87u, "sceKernelStopVTimer", h_StopVTimer);
    sr_hle_register(0x328f9e52u, "sceKernelDeleteVTimer", h_DeleteVTimer);
    sr_hle_register(0xb3a59970u, "sceKernelGetVTimerBase", h_GetVTimerBase);
    sr_hle_register(0x5f32beaau, "sceKernelReferVTimerStatus", h_ReferVTimerStatus);
    /* VTimer time: the Wide getter and both setters (pspautotests-recorded; see their header). */
    sr_hle_register(0xc0b3ffd2u, "sceKernelGetVTimerTimeWide", h_GetVTimerTimeWide);
    sr_hle_register(0x542ad630u, "sceKernelSetVTimerTime", h_SetVTimerTime);
    sr_hle_register(0xfb6425c3u, "sceKernelSetVTimerTimeWide", h_SetVTimerTimeWide);
    /* VTimer handler family (issue #325). */
    sr_hle_register(0xd8b299aeu, "sceKernelSetVTimerHandler", h_SetVTimerHandler);
    sr_hle_register(0x53b00e9au, "sceKernelSetVTimerHandlerWide", h_SetVTimerHandlerWide);
    sr_hle_register(0xd2d615efu, "sceKernelCancelVTimerHandler", h_CancelVTimerHandler);
}
