// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* The real-time clock: sceRtc. Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

static int rtc_read_u64(uint32_t addr, uint64_t *out) {
    if (!addr || !out || !sr_guest_span_readable(addr, 8u)) return 0;
    *out = (uint64_t)MEM_R32(addr) | ((uint64_t)MEM_R32(addr + 4u) << 32);
    return 1;
}

typedef struct {
    uint32_t year, month, day, hour, minute, second, microsecond;
} RtcDateTime;

static int rtc_is_leap(uint32_t year) {
    return (year % 4u == 0u) && (year % 100u != 0u || year % 400u == 0u);
}
static uint32_t rtc_days_in_month(uint32_t year, uint32_t month) {
    static const uint8_t days[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < 1u || month > 12u) return 0;
    return days[month - 1u] + (month == 2u && rtc_is_leap(year));
}
static int rtc_read_datetime(uint32_t addr, RtcDateTime *out) {
    if (!addr || !out || !sr_guest_span_readable(addr, 16u)) return -1;
    out->year = MEM_R16(addr + 0u); out->month = MEM_R16(addr + 2u);
    out->day = MEM_R16(addr + 4u); out->hour = MEM_R16(addr + 6u);
    out->minute = MEM_R16(addr + 8u); out->second = MEM_R16(addr + 10u);
    out->microsecond = MEM_R32(addr + 12u);
    if (out->year < 1u || out->year > 9999u) return -1; /* PSP_TIME_INVALID_YEAR */
    if (out->month < 1u || out->month > 12u) return -2;
    if (out->day < 1u || out->day > rtc_days_in_month(out->year, out->month)) return -3;
    if (out->hour > 23u) return -4;
    if (out->minute > 59u) return -5;
    if (out->second > 59u) return -6;
    if (out->microsecond > 999999u) return -7;
    return 0;
}
static int rtc_write_datetime(uint32_t addr, const RtcDateTime *value) {
    if (!addr || !value || !sr_guest_span_writable(addr, 16u)) return 0;
    MEM_W16(addr + 0u, (uint16_t)value->year); MEM_W16(addr + 2u, (uint16_t)value->month);
    MEM_W16(addr + 4u, (uint16_t)value->day); MEM_W16(addr + 6u, (uint16_t)value->hour);
    MEM_W16(addr + 8u, (uint16_t)value->minute); MEM_W16(addr + 10u, (uint16_t)value->second);
    MEM_W32(addr + 12u, value->microsecond);
    return 1;
}
/* PSP RTC ticks are microseconds since 0001-01-01 in the proleptic Gregorian calendar. */
static uint64_t rtc_datetime_to_tick(const RtcDateTime *value) {
    uint64_t y = value->year - 1u;
    uint64_t days = y * 365u + y / 4u - y / 100u + y / 400u;
    for (uint32_t month = 1; month < value->month; month++)
        days += rtc_days_in_month(value->year, month);
    days += value->day - 1u;
    uint64_t seconds = ((days * 24u + value->hour) * 60u + value->minute) * 60u + value->second;
    return seconds * 1000000u + value->microsecond;
}
/* PSP tick->date conversion covers the FULL u64 tick range: PSPAutotests
 * tests/rtc/arithmetic.expected prints years 60267, 38202, 10000 and 26003
 * written through sceRtcSetTick for wrapped/extreme ticks, so there is no
 * year>9999 rejection here -- the fields are written with natural truncation
 * into the pspTime u16/u32 widths, exactly like the firmware. */
static int rtc_tick_to_datetime(uint64_t tick, RtcDateTime *out) {
    if (!out) return -1;
    uint64_t total_seconds = tick / 1000000u;
    uint64_t days = total_seconds / 86400u;
    uint64_t day_seconds = total_seconds % 86400u;
    uint64_t n400 = days / 146097u; days %= 146097u;
    uint64_t n100 = days / 36524u; if (n100 > 3u) n100 = 3u; days -= n100 * 36524u;
    uint64_t n4 = days / 1461u; days %= 1461u;
    uint64_t n1 = days / 365u; if (n1 > 3u) n1 = 3u; days -= n1 * 365u;
    uint64_t year = 1u + n400 * 400u + n100 * 100u + n4 * 4u + n1;
    uint32_t month = 1u;
    while (month <= 12u) {
        uint32_t dim = rtc_days_in_month((uint32_t)year, month);
        if (days < dim) break;
        days -= dim; month++;
    }
    out->year = (uint32_t)year; out->month = month; out->day = (uint32_t)days + 1u;
    out->hour = (uint32_t)(day_seconds / 3600u);
    out->minute = (uint32_t)((day_seconds / 60u) % 60u);
    out->second = (uint32_t)(day_seconds % 60u);
    out->microsecond = (uint32_t)(tick % 1000000u);
    return 0;
}

static int rtc_add_offset(uint64_t tick, int64_t minutes, uint64_t *out) {
    if (!out) return 0;
    int64_t delta = (int64_t)minutes * 60ll * 1000000ll;
    if (delta >= 0) {
        if (tick > UINT64_MAX - (uint64_t)delta) return 0;
        *out = tick + (uint64_t)delta;
    } else {
        uint64_t magnitude = (uint64_t)(-delta);
        if (tick < magnitude) return 0;
        *out = tick - magnitude;
    }
    return 1;
}
/* sceRtcGetCurrentClock[LocalTime] accepts every int32 timezone offset and
 * returns success (PSPAutotests tests/rtc/rtc.expected: 0, +13, +60, -60,
 * -600000, INT_MAX and -INT_MAX all print 00000000).  The offset is applied in
 * well-defined modulo-2^64 arithmetic -- the firmware's own u64 tick shape
 * (arithmetic.expected wraps to years 60267/38202 on negative overrun) -- so
 * an offset that crosses year 1 wraps instead of failing.  ConvertUtcToLocal
 * keeps the checked rtc_add_offset above because its overflow contract is not
 * autotest-verified; only the current-clock path uses the wrap form. */
static void rtc_add_offset_wrap(uint64_t tick, int64_t minutes, uint64_t *out) {
    int64_t delta = (int64_t)minutes * 60ll * 1000000ll;
    *out = tick + (uint64_t)delta;   /* defined modulo 2^64 for both signs */
}
static uint32_t h_RtcGetCurrentTick(CpuState *s) {
    (void)s;
    return rtc_write_u64(A0, rtc_now_tick()) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcGetTick(CpuState *s) {
    (void)s;
    if (!A0 || !A1) return RTC_ILLEGAL_ADDR;
    if (!sr_guest_span_readable(A0, 16u)) return RTC_ILLEGAL_ADDR;
    RtcDateTime value;
    int valid = rtc_read_datetime(A0, &value);
    /* PSPAutotests tests/rtc/convert.expected: "Min year: 800001fe" and
     * "Year overflow: 800001fe" -- an invalid date reports
     * SCE_KERNEL_ERROR_INVALID_VALUE and leaves the output tick untouched
     * (the tick printed after the failure is the previous success). */
    if (valid != 0) return RTC_INVALID_VALUE;
    if (!sr_guest_span_writable(A1, 8u)) return RTC_ILLEGAL_ADDR;
    return rtc_write_u64(A1, rtc_datetime_to_tick(&value)) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcSetTick(CpuState *s) {
    (void)s;
    if (!A0 || !A1) return RTC_ILLEGAL_ADDR;
    uint64_t tick;
    if (!rtc_read_u64(A1, &tick)) return RTC_ILLEGAL_ADDR;
    RtcDateTime value;
    /* Any u64 tick converts; extreme/wrapped values truncate into the pspTime
     * field widths (PSPAutotests arithmetic.expected, checkSetTick). */
    rtc_tick_to_datetime(tick, &value);
    return rtc_write_datetime(A0, &value) ? 0u : RTC_ILLEGAL_ADDR;
}
/* sceRtcGetWin32FileTime(pspTime *in, u64 *out): convert from the PSP Gregorian
 * epoch to Windows' 1601 epoch and 100 ns units.
 *
 * PSPAutotests tests/rtc/convert.expected defines the failure contract:
 *   - "NULL filetime: -1337 (800001fe)"    invalid out pointer -> 0x800001fe, no write
 *   - "Zeroed time: 0 (800001fe)"          year 0             -> 0x800001fe, out := 0
 *   - "1600 January 01: 0 (800001fe)"      pre-1601 tick      -> 0x800001fe, out := 0
 *   - "Arbitrary date/time: 127779156600000010 (00000000)" -- 2005-11-31 is
 *     ACCEPTED (day 31 in a 30-day month) and converts with carry arithmetic,
 *     so this path does NOT run the strict days-in-month validator.  Only the
 *     year bound (1..9999) is checked here; the epoch bound below rejects
 *     everything before 1601-01-01.  On failure the output is written as 0. */
static uint32_t h_RtcGetWin32FileTime(CpuState *s) {
    (void)s;
    if (!A0 || !A1) return RTC_INVALID_VALUE;
    if (!sr_guest_span_readable(A0, 16u)) return RTC_INVALID_VALUE;
    if (!sr_guest_span_writable(A1, 8u)) return RTC_INVALID_VALUE;
    RtcDateTime value;
    value.year = MEM_R16(A0 + 0u); value.month = MEM_R16(A0 + 2u);
    value.day = MEM_R16(A0 + 4u); value.hour = MEM_R16(A0 + 6u);
    value.minute = MEM_R16(A0 + 8u); value.second = MEM_R16(A0 + 10u);
    value.microsecond = MEM_R32(A0 + 12u);
    if (value.year < 1u || value.year > 9999u) {
        MEM_W32(A1, 0u); MEM_W32(A1 + 4u, 0u);
        return RTC_INVALID_VALUE;
    }
    /* Component lower bounds are part of the declared contract (psprtc.h
     * pspRtcCheckValidErrors: PSP_TIME_INVALID_MONTH / _DAY document the
     * ranges), but the measured corpus pins only the year bound for this
     * path, and month 0/13 or day 0 previously reached rtc_datetime_to_tick()
     * unchallenged: `day - 1` underflows to a modular garbage tick and an
     * empty or full month sum can return a fabricated success.  Fail closed
     * with this function's measured invalid-input class (convert.expected
     * "Zeroed time: 0 (800001fe)", output written 0) until firmware behavior
     * for these fields is measured.  The day-in-month UPPER bound stays
     * unchecked on purpose: convert.expected measures 2005-11-31 accepted
     * (a 31st in a 30-day month), so only the lower bound is enforced here. */
    if (value.month < 1u || value.month > 12u || value.day < 1u) {
        MEM_W32(A1, 0u); MEM_W32(A1 + 4u, 0u);
        return RTC_INVALID_VALUE;
    }
    uint64_t tick = rtc_datetime_to_tick(&value);
    if (tick < RTC_FILETIME_EPOCH_TICK) {
        MEM_W32(A1, 0u); MEM_W32(A1 + 4u, 0u);
        return RTC_INVALID_VALUE;
    }
    uint64_t delta = tick - RTC_FILETIME_EPOCH_TICK;
    if (delta > UINT64_MAX / 10u) {
        MEM_W32(A1, 0u); MEM_W32(A1 + 4u, 0u);
        return RTC_INVALID_VALUE;
    }
    return rtc_write_u64(A1, delta * 10u) ? 0u : RTC_INVALID_VALUE;
}
/* A guest access an HLE handler makes where its PSP counterpart has no pointer check and
 * takes a bus error inside the system module. A user program cannot recover from that on the
 * PSP, so the runtime does not turn it into an error code the program could read: the access
 * is recorded as out of range (sr_oor), the fault goes to the flight recorder as a fault
 * exception (MIPS ExcCode 7, data bus error, with the faulting guest address), and the run
 * stops the way an unregistered import stops it, with its own exit status. The executable
 * harness counts the fault and returns instead, so a test can assert it. */
#define HLE_GUEST_FAULT_EXIT 10
#define HLE_GUEST_FAULT_EXC_DBE 7u
#ifdef SR_HLE_THREAD_SELFTEST
static uint32_t s_hle_guest_fault_count, s_hle_guest_fault_addr;
uint32_t sr_hle_test_guest_fault_count(void) { return s_hle_guest_fault_count; }
uint32_t sr_hle_test_guest_fault_addr(void) { return s_hle_guest_fault_addr; }
void sr_hle_test_guest_fault_reset(void) { s_hle_guest_fault_count = 0u; s_hle_guest_fault_addr = 0u; }
#endif
static void hle_guest_fault(CpuState *s, const char *name, uint32_t addr, int store) {
    sr_oor(addr, 0u, store);
    fprintf(stderr, "HLE: guest fault: %s %s guest 0x%08x; the PSP faults inside the module here, "
                    "so the run stops\n", name, store ? "wrote" : "read", addr);
    sr_flight_fault(SR_FLIGHT_KIND_FAULT_EXCEPTION, s->pc, HLE_GUEST_FAULT_EXC_DBE, addr);
#ifdef SR_HLE_THREAD_SELFTEST
    s_hle_guest_fault_count++;
    s_hle_guest_fault_addr = addr;
#else
    if (sr_sched_on) {
        sr_trace_close();
        sr_compat_report();
        sr_perf_shutdown();
        fflush(stderr);
        _Exit(HLE_GUEST_FAULT_EXIT);
    }
    longjmp(g_hle_jmp, 1);
#endif
}

/* sceRtcGetTime64_t(const pspTime *date, u64 *time): the date as seconds since
 * 1970-01-01 00:00:00. Measured on a PSP-3000, firmware 6.61, 2026-10-24:
 *   - valid dates convert and return 0;
 *   - year 1 and any date before 1970 return 0x800001FE;
 *   - month 13, day 32, February 30 and hour 24 are not rejected: they return 0 and
 *     normalise, carrying into the next year, month or day;
 *   - a NULL output pointer returns 0 without a fault;
 *   - a NULL date pointer faults inside the module (it has no check), so it takes the
 *     guest-fault stop above, as does any date span that is not readable.
 * Carrying is modelled as calendar arithmetic: months past 12 move into the following
 * years, and the day, hour, minute, second and microsecond fields add linearly through
 * rtc_datetime_to_tick(). The measurement records the return codes and that these
 * dates normalise; the exact seconds for them are this arithmetic. Year 0, month 0 and
 * day 0 are UNMEASURED and refused with 0x800001FE rather than converted. On a refusal
 * the output is not written (also UNMEASURED); a non-NULL output span that is not
 * writable takes the guest-fault stop, as the module would store through it. */
static uint32_t h_RtcGetTime64_t(CpuState *s) {
    /* NULL is tested on its own: the guest arena backs low physical addresses, so the span
     * check alone would read a date from address 0 where the PSP faults. */
    if (!A0 || !sr_guest_span_readable(A0, 16u)) {
        hle_guest_fault(s, "sceRtcGetTime64_t", A0, 0);
        return 0u;
    }
    RtcDateTime value;
    value.year = MEM_R16(A0 + 0u); value.month = MEM_R16(A0 + 2u);
    value.day = MEM_R16(A0 + 4u); value.hour = MEM_R16(A0 + 6u);
    value.minute = MEM_R16(A0 + 8u); value.second = MEM_R16(A0 + 10u);
    value.microsecond = MEM_R32(A0 + 12u);
    if (value.year < 1u || value.month < 1u || value.day < 1u) return RTC_INVALID_VALUE;
    value.year += (value.month - 1u) / 12u;
    value.month = (value.month - 1u) % 12u + 1u;
    uint64_t tick = rtc_datetime_to_tick(&value);
    if (tick < RTC_UNIX_EPOCH_TICK) return RTC_INVALID_VALUE;
    if (!A1) return 0u;
    if (!sr_guest_span_writable(A1, 8u)) {
        hle_guest_fault(s, "sceRtcGetTime64_t", A1, 1);
        return 0u;
    }
    return rtc_write_u64(A1, (tick - RTC_UNIX_EPOCH_TICK) / 1000000u) ? 0u : RTC_ILLEGAL_ADDR;
}
/* sceRtcGetTime_t(const pspTime *date, u32 *time): the date as seconds since 1970-01-01 00:00:00,
 * the microseconds dropped, written as one 32-bit word. Measured on a PSP by pspautotests
 * tests/rtc/convert.expected (rev 2ed7c9c5, checkRtcGetTime_t): 2012-09-20 07:12:15.000500
 * returns 0 and writes 1348125135, the same seconds sceRtcGetTime64_t gives for that date.
 * Only in-range dates on or after the epoch whose seconds fit the 32-bit word are measured.
 * What the PSP does with a field out of range, a date before 1970, a value past 32 bits, or a
 * NULL or unmapped pointer is not measured for this function (sceRtcGetTime64_t's measured
 * answers are not assumed to carry over), so each of those stops the run by name. */
static uint32_t h_RtcGetTime_t(CpuState *s) {
    if (!A0 || !sr_guest_span_readable(A0, 16u))
        return hle_unmodelled_stop("sceRtcGetTime_t", "with an unreadable date 0x%08x is not measured", A0);
    if (!A1 || !sr_guest_span_writable(A1, 4u))
        return hle_unmodelled_stop("sceRtcGetTime_t", "with an unwritable output 0x%08x is not measured", A1);
    RtcDateTime value;
    if (rtc_read_datetime(A0, &value) != 0)
        return hle_unmodelled_stop("sceRtcGetTime_t", "with a date field out of range is not measured");
    uint64_t tick = rtc_datetime_to_tick(&value);
    if (tick < RTC_UNIX_EPOCH_TICK)
        return hle_unmodelled_stop("sceRtcGetTime_t", "with a date before 1970 is not measured");
    uint64_t seconds = (tick - RTC_UNIX_EPOCH_TICK) / 1000000u;
    if (seconds > UINT32_MAX)
        return hle_unmodelled_stop("sceRtcGetTime_t", "with a date past the 32-bit range is not measured");
    MEM_W32(A1, (uint32_t)seconds);
    return 0;
}
/* sceRtcTickAddYears(u64 *dest, const u64 *src, int years): the source tick moved by whole
 * calendar years, its month, day and time of day kept. Measured on a PSP by pspautotests
 * tests/rtc/arithmetic.expected (rev 2ed7c9c5, checkRtcTickAddYears), which fills dest with
 * 0x1337 before each call:
 *   - 2012-02-29 + 1 year is 2013-02-28 (the leap day clamps), + 4 years is 2016-02-29;
 *   - 1970-01-01 00:00:00.000445 - 1969 is 0001-01-01 (the microseconds kept), - 1968 is
 *     0002-01-01, + 2000 is 3970-01-01, - 20 is 1950-01-01; 1850 to 2300 + 1 move one year;
 *   - a result year below 1 (1970 - 1970 and beyond, 10 - 10) or above 9999 (9998 + 2) still
 *     returns 0 but leaves dest holding 0x1337, unwritten.
 * A source tick past year 9999, and a NULL or unmapped pointer, are not measured, so each stops
 * the run by name. */
static uint32_t h_RtcTickAddYears(CpuState *s) {
    uint64_t tick;
    if (!rtc_read_u64(A1, &tick))
        return hle_unmodelled_stop("sceRtcTickAddYears", "with an unreadable source 0x%08x is not measured", A1);
    if (!A0 || !sr_guest_span_writable(A0, 8u))
        return hle_unmodelled_stop("sceRtcTickAddYears", "with an unwritable destination 0x%08x is not measured", A0);
    RtcDateTime value;
    rtc_tick_to_datetime(tick, &value);
    if (value.year > 9999u)
        return hle_unmodelled_stop("sceRtcTickAddYears", "from a tick past year 9999 is not measured");
    int64_t year = (int64_t)value.year + (int32_t)A2;
    if (year < 1 || year > 9999) return 0;
    value.year = (uint32_t)year;
    if (value.month == 2u && value.day == 29u && !rtc_is_leap(value.year)) value.day = 28u;
    return rtc_write_u64(A0, rtc_datetime_to_tick(&value)) ? 0u : RTC_ILLEGAL_ADDR;
}
/* sceRtcGetTickResolution(void): ticks per second. The RTC tick is one microsecond, the unit every
 * tick conversion above uses (pspautotests rtc tests); uOFW src/kd/rtc/rtc.c (MIT) reconstructs
 * this function, marked as matching the firmware's assembly, as returning 1000000. */
static uint32_t h_RtcGetTickResolution(CpuState *s) { (void)s; return 1000000u; }
/* Fill a pspTime {u16 year,month,day,hour,min,sec; u32 usec}. The explicit
 * current-clock variant accepts a signed PSP timezone offset in minutes. */
static uint32_t h_RtcGetCurrentClock(CpuState *s) {
    (void)s;
    if (!sr_guest_span_writable(A0, 16u)) return RTC_ILLEGAL_ADDR;
    uint64_t local_tick;
    rtc_add_offset_wrap(rtc_now_tick(), (int32_t)A1, &local_tick);
    RtcDateTime value;
    rtc_tick_to_datetime(local_tick, &value);
    return rtc_write_datetime(A0, &value) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcGetCurrentClockLocal(CpuState *s) {
    (void)s;
    if (!sr_guest_span_writable(A0, 16u)) return RTC_ILLEGAL_ADDR;
    uint64_t local_tick;
    rtc_add_offset_wrap(rtc_now_tick(), psp_profile_local_offset_minutes(NULL, NULL), &local_tick);
    RtcDateTime value;
    rtc_tick_to_datetime(local_tick, &value);
    return rtc_write_datetime(A0, &value) ? 0u : RTC_ILLEGAL_ADDR;
}
/* Format and invalid-address result derived from PPSSPP 7fab97c05f9568b17ac79c235b945f568eaf6384.
 * Local offset comes from the PSP profile, as for the other local RTC calls. */
static uint32_t h_RtcFormatRFC2822Local(CpuState *s) {
    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    uint64_t tick, local;
    if (!A0 || !sr_guest_span_writable(A0, 32u) || (A1 & 3u) || !rtc_read_u64(A1, &tick)) return 0xffffffffu;
    int64_t offset = psp_profile_local_offset_minutes(NULL, NULL);
    rtc_add_offset_wrap(tick, offset, &local);
    RtcDateTime date;
    rtc_tick_to_datetime(local, &date);
    uint64_t magnitude = offset < 0 ? (uint64_t)(-offset) : (uint64_t)offset;
    char result[128];
    snprintf(result, sizeof(result), "%s, %02u %s %04u %02u:%02u:%02u %c%02llu%02llu",
             days[(local / 86400000000ull + 1u) % 7u], date.day, months[date.month - 1u], date.year,
             date.hour, date.minute, date.second, offset < 0 ? '-' : '+',
             (unsigned long long)(magnitude / 60u), (unsigned long long)(magnitude % 60u));
    result[31] = 0;
    for (size_t i = 0; i < 32u; i++) { MEM_W8(A0 + (uint32_t)i, result[i]); if (!result[i]) break; }
    return 0;
}

static uint32_t h_RtcConvertUtcToLocal(CpuState *s) {
    (void)s;
    uint64_t in, out;
    if (!A0 || !A1 || !rtc_read_u64(A0, &in) || !sr_guest_span_writable(A1, 8u) ||
        !rtc_add_offset(in, psp_profile_local_offset_minutes(NULL, NULL), &out)) return RTC_ILLEGAL_ADDR;
    return rtc_write_u64(A1, out) ? 0u : RTC_ILLEGAL_ADDR;
}
static uint32_t h_RtcConvertLocalToUtc(CpuState *s) {
    (void)s;
    uint64_t in, out;
    if (!A0 || !A1 || !rtc_read_u64(A0, &in) || !sr_guest_span_writable(A1, 8u) ||
        !rtc_add_offset(in, -psp_profile_local_offset_minutes(NULL, NULL), &out)) return RTC_ILLEGAL_ADDR;
    return rtc_write_u64(A1, out) ? 0u : RTC_ILLEGAL_ADDR;
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_launch_sweep_blocker_handlers(). */
void hle_register_launch_sweep_blocker_rtc(void) {
    sr_hle_register(0x27c4594c, "sceRtcGetTime_t", h_RtcGetTime_t);
    sr_hle_register(0x42842c77, "sceRtcTickAddYears", h_RtcTickAddYears);
    sr_hle_register(0xc41c2853, "sceRtcGetTickResolution", h_RtcGetTickResolution);
}

/* From hle_register_time_handlers(). */
void hle_register_time_rtc(void) {
    sr_hle_register(0x6ff40acc, "sceRtcGetTick", h_RtcGetTick);
    sr_hle_register(0x7ed29e40, "sceRtcSetTick", h_RtcSetTick);
    sr_hle_register(0xcf561893, "sceRtcGetWin32FileTime", h_RtcGetWin32FileTime);
    sr_hle_register(0xe1c93e47, "sceRtcGetTime64_t", h_RtcGetTime64_t);
    sr_hle_register(0x4cfa57b0, "sceRtcGetCurrentClock", h_RtcGetCurrentClock);
    sr_hle_register(0xe7c27d1b, "sceRtcGetCurrentClockLocalTime", h_RtcGetCurrentClockLocal);
    sr_hle_register(0x3f7ad767, "sceRtcGetCurrentTick", h_RtcGetCurrentTick);
    sr_hle_register(0x34885e0d, "sceRtcConvertUtcToLocalTime", h_RtcConvertUtcToLocal);
    sr_hle_register(0x779242a2, "sceRtcConvertLocalTimeToUTC", h_RtcConvertLocalToUtc);
    sr_hle_register(0x7de6711b, "sceRtcFormatRFC2822LocalTime", h_RtcFormatRFC2822Local);
}

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_rtc(void) {
    sr_hle_register_unsupported(0x011f03c1u, "sceRtcGetAccumulativeTime", 0x80020002u);
}
