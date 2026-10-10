// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// HLE measurement probes for the PSP-3000 oracle: one PRX per family, one launch
// per family (CASE=hle-kernel-status, hle-vtimer, hle-power-clock, hle-hprm,
// hle-ctrl-latch, hle-sysparam, hle-ge-edram). The only state a family changes
// is the GE eDRAM translation width, which the hle-ge-edram family restores
// before it exits. Every import is user-mode and comes from a per-family import
// block (hle_*_imports.S) or the shared threadman_user_imports.S.
//
// Record stream (tools/psp_oracle/protocol.py): NAKAGAWA_PSP_META, then one
// NAKAGAWA_PSP_STEP before each risky call, one NAKAGAWA_PSP_TEST per
// measurement, a `<case>-done` terminal record whose out0 counts the
// measurements, and a final NAKAGAWA_PSP_COMPLETE line. Every line is appended to
// the case's host0 log durably (open, append, close). A bounded wait never waits
// longer than its stated bound, and a family records SKIP rather than omitting a
// cell when its prerequisite did not hold.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <psppower.h>
#include <psphprm.h>
#include <pspctrl.h>
#include <psputility_sysparam.h>
#include <pspge.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define HLE_CASE_KERNEL_STATUS 90
#define HLE_CASE_VTIMER 91
#define HLE_CASE_POWER_CLOCK 92
#define HLE_CASE_HPRM 93
#define HLE_CASE_CTRL_LATCH 94
#define HLE_CASE_SYSPARAM 95
#define HLE_CASE_GE_EDRAM 96

#if PSP_ORACLE_CASE == HLE_CASE_KERNEL_STATUS
#define HLE_CAMPAIGN_ID "hle-kernel-status"
#define HLE_TEST_ID "PSP-HLE-KERNEL-STATUS-001"
#define HLE_LOG "host0:/hle_kernel_status_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VTIMER
#define HLE_CAMPAIGN_ID "hle-vtimer"
#define HLE_TEST_ID "PSP-HLE-VTIMER-001"
#define HLE_LOG "host0:/hle_vtimer_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_POWER_CLOCK
#define HLE_CAMPAIGN_ID "hle-power-clock"
#define HLE_TEST_ID "PSP-HLE-POWER-001"
#define HLE_LOG "host0:/hle_power_clock_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_HPRM
#define HLE_CAMPAIGN_ID "hle-hprm"
#define HLE_TEST_ID "PSP-HLE-HPRM-001"
#define HLE_LOG "host0:/hle_hprm_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_CTRL_LATCH
#define HLE_CAMPAIGN_ID "hle-ctrl-latch"
#define HLE_TEST_ID "PSP-HLE-CTRL-LATCH-001"
#define HLE_LOG "host0:/hle_ctrl_latch_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_SYSPARAM
#define HLE_CAMPAIGN_ID "hle-sysparam"
#define HLE_TEST_ID "PSP-HLE-SYSPARAM-001"
#define HLE_LOG "host0:/hle_sysparam_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_GE_EDRAM
#define HLE_CAMPAIGN_ID "hle-ge-edram"
#define HLE_TEST_ID "PSP-HLE-GE-EDRAM-001"
#define HLE_LOG "host0:/hle_ge_edram_log.txt"
#else
#error probe_hle_measure.c has no family for this PSP_ORACLE_CASE
#endif
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

#define HLE_SENTINEL 0xA5A5A5A5u
#define HLE_MAX_WORDS 18u
#define HLE_POLL_ITERATIONS 200u
#define HLE_POLL_DELAY_US 10000u
#define HLE_DELAY_US 20000u

static int s_log_failed;
static uint32_t s_records;

/* Durable line writer: open, write everything, close. A later hang or fault
   keeps every line already written. `truncate` starts a fresh log. */
static void hle_write(const char *line, int truncate)
{
    const int flags = PSP_O_WRONLY | PSP_O_CREAT | (truncate ? PSP_O_TRUNC : PSP_O_APPEND);
    const SceUID fd = sceIoOpen(HLE_LOG, flags, 0777);
    if (fd < 0) {
        s_log_failed = 1;
        return;
    }
    const size_t length = strlen(line);
    size_t offset = 0;
    while (offset < length) {
        const int wrote = sceIoWrite(fd, line + offset, (SceSize)(length - offset));
        if (wrote <= 0) {
            s_log_failed = 1;
            break;
        }
        offset += (size_t)wrote;
    }
    if (sceIoClose(fd) < 0) {
        s_log_failed = 1;
    }
}

static void hle_begin(void)
{
    static const char meta[] =
        "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
        "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
        "source_commit=" PROBE_BUILD_COMMIT " fixture=hle-measure\n";
    hle_write(meta, 1);
}

/* Progress marker written durably before a call that could hang or fault. The
   step name is the measurement case about to run, so a hang names its cell. */
static void hle_step(const char *step)
{
    static char line[200];
    snprintf(line, sizeof(line), "NAKAGAWA_PSP_STEP schema=1 case_id=%s step=%s\n",
             HLE_CAMPAIGN_ID, step);
    hle_write(line, 0);
}

static void hle_emit(const char *case_id, const char *status, uint32_t result,
                     const uint32_t *out, uint32_t count)
{
    static char line[700];
    int n = snprintf(line, sizeof(line),
                     "NAKAGAWA_PSP_TEST schema=1 test_id=%s case_id=%s status=%s result=0x%08x",
                     HLE_TEST_ID, case_id, status, (unsigned int)result);
    for (uint32_t i = 0; i < count && n > 0 && n < (int)sizeof(line) - 32; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " out%u=0x%08x", (unsigned int)i,
                      (unsigned int)out[i]);
    }
    if (n > 0 && n < (int)sizeof(line) - 2) {
        line[n] = '\n';
        line[n + 1] = '\0';
        hle_write(line, 0);
    }
}

static void hle_record(const char *case_id, const char *status, uint32_t result,
                       const uint32_t *out, uint32_t count)
{
    s_records++;
    hle_emit(case_id, status, result, out, count);
}

__attribute__((unused)) static void hle_skip(const char *case_id, uint32_t count)
{
    uint32_t zeros[HLE_MAX_WORDS] = {0};
    hle_record(case_id, "SKIP", 0, zeros, count);
}

__attribute__((unused)) static void hle_prefill(uint32_t *buf, uint32_t words, uint32_t size_word)
{
    for (uint32_t i = 0; i < words; i++) {
        buf[i] = HLE_SENTINEL;
    }
    buf[0] = size_word;
}

static void hle_finish(int ok)
{
    const uint32_t count = s_records;
    hle_emit(HLE_DONE_ID, "PASS", 0, &count, 1);
    static char line[96];
    snprintf(line, sizeof(line), "NAKAGAWA_PSP_COMPLETE schema=1 status=%s\n",
             (ok && !s_log_failed) ? "PASS" : "FAIL");
    hle_write(line, 0);
}

/* Module lifecycle, as in probe.c: main parks until module_stop asks it to end,
   then runs the CRT's de-initialisation and exits; module_stop deletes it. */
static volatile SceUID s_hle_main_thread = -1;
static volatile int s_hle_stop_requested;
extern void _fini(void);
extern void __libcglue_deinit(void);
#define HLE_MAIN_STOP_TIMEOUT_US 1000000u

static void hle_park_until_stop(void)
{
    while (!s_hle_stop_requested) {
        sceKernelSleepThread();
    }
    _fini();
    __libcglue_deinit();
    sceKernelExitThread(0);
}

int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    const SceUID main_thread = s_hle_main_thread;
    if (main_thread < 0) {
        return 1;
    }
    s_hle_stop_requested = 1;
    (void)sceKernelWakeupThread(main_thread);
    SceUInt timeout = HLE_MAIN_STOP_TIMEOUT_US;
    if (sceKernelWaitThreadEnd(main_thread, &timeout) < 0) {
        return 1;
    }
    if (sceKernelDeleteThread(main_thread) < 0) {
        return 1;
    }
    s_hle_main_thread = -1;
    return 0;
}

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_KERNEL_STATUS

#define HLE_SYS_WORDS 8u
#define HLE_FPL_WORDS 14u

static void hle_sys_refer(const char *case_id, uint32_t size_word)
{
    uint32_t buf[HLE_SYS_WORDS];
    hle_prefill(buf, HLE_SYS_WORDS, size_word);
    hle_step(case_id);
    const int rc = sceKernelReferSystemStatus((SceKernelSystemStatus *)buf);
    hle_record(case_id, "PASS", (uint32_t)rc, buf, HLE_SYS_WORDS);
}

static void hle_fpl_refer(const char *case_id, SceUID uid, uint32_t size_word)
{
    if (uid < 0) {
        hle_skip(case_id, HLE_FPL_WORDS);
        return;
    }
    uint32_t buf[HLE_FPL_WORDS];
    hle_prefill(buf, HLE_FPL_WORDS, size_word);
    hle_step(case_id);
    const int rc = sceKernelReferFplStatus(uid, (SceKernelFplInfo *)buf);
    hle_record(case_id, "PASS", (uint32_t)rc, buf, HLE_FPL_WORDS);
}

static void hle_fpl_alloc(const char *case_id, SceUID uid, void **slot)
{
    if (uid < 0) {
        hle_skip(case_id, 1);
        return;
    }
    hle_step(case_id);
    const int rc = sceKernelTryAllocateFpl(uid, slot);
    const uint32_t got = (*slot != NULL) ? 1u : 0u;
    hle_record(case_id, "PASS", (uint32_t)rc, &got, 1);
}

static int hle_fpl_free(const char *case_id, SceUID uid, void *block)
{
    if (uid < 0 || block == NULL) {
        hle_skip(case_id, 1);
        return 1;
    }
    hle_step(case_id);
    const int rc = sceKernelFreeFpl(uid, block);
    const uint32_t zero = 0;
    hle_record(case_id, "PASS", (uint32_t)rc, &zero, 1);
    return rc == 0;
}

static int hle_run(void)
{
    int ok = 1;
    hle_sys_refer("sys-status-size-0x1c", 0x1cu);
    hle_sys_refer("sys-status-size-0x20", 0x20u);
    hle_sys_refer("sys-status-size-0x08", 0x08u);
    hle_sys_refer("sys-status-size-0x00", 0x00u);

    /* Probe-owned fixed-size pool in user partition 2: two 64-byte blocks. */
    hle_step("fpl-create");
    const SceUID fpl = sceKernelCreateFpl("hlefpl", 2, 0, 64, 2, NULL);
    const uint32_t created = (fpl >= 0) ? 1u : 0u;
    hle_record("fpl-create", (fpl >= 0) ? "PASS" : "FAIL", (uint32_t)fpl, &created, 1);

    hle_fpl_refer("fpl-refer-full-all-free", fpl, 0x38u);
    void *block_a = NULL;
    void *block_b = NULL;
    void *block_x = NULL;
    hle_fpl_alloc("fpl-try-alloc-a", fpl, &block_a);
    hle_fpl_alloc("fpl-try-alloc-b", fpl, &block_b);
    hle_fpl_alloc("fpl-try-alloc-exhausted", fpl, &block_x);
    hle_fpl_refer("fpl-refer-full-two-held", fpl, 0x38u);
    hle_fpl_refer("fpl-refer-size-0x08-two-held", fpl, 0x08u);
    hle_fpl_refer("fpl-refer-size-0x00-two-held", fpl, 0x00u);
    ok &= hle_fpl_free("fpl-free-a", fpl, block_a);
    hle_fpl_refer("fpl-refer-full-one-free", fpl, 0x38u);
    ok &= hle_fpl_free("fpl-free-b", fpl, block_b);

    hle_step("fpl-delete");
    const int deleted = (fpl >= 0) ? sceKernelDeleteFpl(fpl) : -1;
    const uint32_t zero = 0;
    if (fpl >= 0) {
        hle_record("fpl-delete", "PASS", (uint32_t)deleted, &zero, 1);
        ok &= (deleted == 0);
    } else {
        hle_skip("fpl-delete", 1);
    }
    /* The stale UID shows what a refer on a deleted pool reports. */
    hle_fpl_refer("fpl-refer-full-after-delete", fpl, 0x38u);
    return ok;
}

#endif /* HLE_CASE_KERNEL_STATUS */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VTIMER

#define HLE_VT_WORDS 18u

static void hle_vt_refer(const char *case_id, SceUID uid, uint32_t size_word)
{
    if (uid < 0) {
        hle_skip(case_id, HLE_VT_WORDS);
        return;
    }
    uint32_t buf[HLE_VT_WORDS];
    hle_prefill(buf, HLE_VT_WORDS, size_word);
    hle_step(case_id);
    const int rc = sceKernelReferVTimerStatus(uid, (SceKernelVTimerInfo *)buf);
    hle_record(case_id, "PASS", (uint32_t)rc, buf, HLE_VT_WORDS);
}

static void hle_vt_time(const char *case_id, SceUID uid)
{
    if (uid < 0) {
        hle_skip(case_id, 2);
        return;
    }
    SceKernelSysClock clock;
    clock.low = HLE_SENTINEL;
    clock.hi = HLE_SENTINEL;
    hle_step(case_id);
    const int rc = sceKernelGetVTimerTime(uid, &clock);
    const uint32_t out[2] = {clock.low, clock.hi};
    hle_record(case_id, "PASS", (uint32_t)rc, out, 2);
}

/* Start (`stop` = 0) or stop (`stop` = 1) a VTimer and record the return. */
static void hle_vt_control(const char *case_id, SceUID uid, int stop)
{
    if (uid < 0) {
        hle_skip(case_id, 1);
        return;
    }
    hle_step(case_id);
    const int rc = stop ? sceKernelStopVTimer(uid) : sceKernelStartVTimer(uid);
    const uint32_t zero = 0;
    hle_record(case_id, "PASS", (uint32_t)rc, &zero, 1);
}

static void hle_delay(const char *case_id)
{
    hle_step(case_id);
    const int rc = sceKernelDelayThread(HLE_DELAY_US);
    const uint32_t out = HLE_DELAY_US;
    hle_record(case_id, "PASS", (uint32_t)rc, &out, 1);
}

static int hle_run(void)
{
    int ok = 1;
    hle_step("vtimer-create");
    const SceUID vt = sceKernelCreateVTimer("hlevt", NULL);
    const uint32_t created = (vt >= 0) ? 1u : 0u;
    hle_record("vtimer-create", (vt >= 0) ? "PASS" : "FAIL", (uint32_t)vt, &created, 1);

    hle_vt_refer("vtimer-refer-created", vt, 0x48u);
    hle_vt_control("vtimer-stop-not-started", vt, 1);
    hle_vt_control("vtimer-start", vt, 0);
    hle_vt_time("vtimer-time-running-early", vt);
    hle_vt_refer("vtimer-refer-running-early", vt, 0x48u);
    hle_delay("vtimer-delay-20ms");
    hle_vt_time("vtimer-time-running-20ms", vt);
    hle_vt_control("vtimer-stop", vt, 1);
    hle_vt_time("vtimer-time-stopped", vt);
    hle_vt_refer("vtimer-refer-stopped", vt, 0x48u);
    hle_delay("vtimer-delay-20ms-stopped");
    hle_vt_time("vtimer-time-stopped-later", vt);
    hle_vt_control("vtimer-start-restart", vt, 0);
    hle_vt_refer("vtimer-refer-size-0x08", vt, 0x08u);

    hle_step("vtimer-delete-running");
    const int deleted = (vt >= 0) ? sceKernelDeleteVTimer(vt) : -1;
    const uint32_t zero = 0;
    if (vt >= 0) {
        hle_record("vtimer-delete-running", "PASS", (uint32_t)deleted, &zero, 1);
        ok &= (deleted == 0);
    } else {
        hle_skip("vtimer-delete-running", 1);
    }
    /* The stale UID shows what a refer and a second delete report after delete. */
    hle_vt_refer("vtimer-refer-after-delete", vt, 0x48u);
    if (vt >= 0) {
        hle_step("vtimer-delete-again");
        const int again = sceKernelDeleteVTimer(vt);
        hle_record("vtimer-delete-again", "PASS", (uint32_t)again, &zero, 1);
    } else {
        hle_skip("vtimer-delete-again", 1);
    }
    return ok;
}

#endif /* HLE_CASE_VTIMER */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_POWER_CLOCK

/* Declared here because PSPSDK does not ship the PLL stubs; see
   hle_power_imports.S for the NID source. */
int scePowerGetPllClockFrequencyInt(void);
float scePowerGetPllClockFrequencyFloat(void);

static uint32_t hle_float_bits(float value)
{
    union {
        float f;
        uint32_t u;
    } bits;
    bits.f = value;
    return bits.u;
}

/* Integer part of a clock value; out-of-range or NaN input reads as all ones. */
static uint32_t hle_float_trunc(float value)
{
    if (value > -2147483648.0f && value < 2147483648.0f) {
        return (uint32_t)(int32_t)value;
    }
    return 0xFFFFFFFFu;
}

static void hle_clock_int(const char *case_id, int value)
{
    const uint32_t out = (uint32_t)value;
    hle_record(case_id, "PASS", (uint32_t)value, &out, 1);
}

static void hle_clock_float(const char *case_id, float value)
{
    const uint32_t out = hle_float_trunc(value);
    hle_record(case_id, "PASS", hle_float_bits(value), &out, 1);
}

static int hle_run(void)
{
    hle_step("pll-clock-int");
    hle_clock_int("pll-clock-int", scePowerGetPllClockFrequencyInt());
    hle_step("pll-clock-float");
    hle_clock_float("pll-clock-float", scePowerGetPllClockFrequencyFloat());
    hle_step("cpu-clock-int");
    hle_clock_int("cpu-clock-int", scePowerGetCpuClockFrequencyInt());
    hle_step("cpu-clock-float");
    hle_clock_float("cpu-clock-float", scePowerGetCpuClockFrequencyFloat());
    hle_step("cpu-clock-alias");
    hle_clock_int("cpu-clock-alias", scePowerGetCpuClockFrequency());
    hle_step("bus-clock-int");
    hle_clock_int("bus-clock-int", scePowerGetBusClockFrequencyInt());
    hle_step("bus-clock-float");
    hle_clock_float("bus-clock-float", scePowerGetBusClockFrequencyFloat());
    hle_step("bus-clock-alias");
    hle_clock_int("bus-clock-alias", scePowerGetBusClockFrequency());
    hle_step("cpu-clock-int-repeat");
    hle_clock_int("cpu-clock-int-repeat", scePowerGetCpuClockFrequencyInt());
    return 1;
}

#endif /* HLE_CASE_POWER_CLOCK */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_HPRM

static void hle_flag(const char *case_id, int value)
{
    const uint32_t out = (uint32_t)value;
    hle_record(case_id, "PASS", (uint32_t)value, &out, 1);
}

static int hle_run(void)
{
    hle_step("hprm-remote-first");
    hle_flag("hprm-remote-first", sceHprmIsRemoteExist());
    hle_step("hprm-headphone-first");
    hle_flag("hprm-headphone-first", sceHprmIsHeadphoneExist());
    hle_step("hprm-microphone-first");
    hle_flag("hprm-microphone-first", sceHprmIsMicrophoneExist());
    hle_step("hprm-remote-second");
    hle_flag("hprm-remote-second", sceHprmIsRemoteExist());
    hle_step("hprm-headphone-second");
    hle_flag("hprm-headphone-second", sceHprmIsHeadphoneExist());
    hle_step("hprm-microphone-second");
    hle_flag("hprm-microphone-second", sceHprmIsMicrophoneExist());
    return 1;
}

#endif /* HLE_CASE_HPRM */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_CTRL_LATCH

static void hle_latch_prefill(SceCtrlLatch *latch, uint32_t value)
{
    latch->uiMake = value;
    latch->uiBreak = value;
    latch->uiPress = value;
    latch->uiRelease = value;
}

static void hle_latch_record(const char *case_id, int rc, const SceCtrlLatch *latch)
{
    const uint32_t out[4] = {latch->uiMake, latch->uiBreak, latch->uiPress, latch->uiRelease};
    hle_record(case_id, "PASS", (uint32_t)rc, out, 4);
}

static uint32_t hle_latch_nonzero(const SceCtrlLatch *latch)
{
    return (latch->uiMake | latch->uiBreak | latch->uiPress | latch->uiRelease) ? 1u : 0u;
}

static int hle_run(void)
{
    SceCtrlLatch latch;
    SceCtrlLatch copy;
    hle_latch_prefill(&latch, HLE_SENTINEL);
    hle_step("latch-read-initial");
    int rc = sceCtrlReadLatch(&latch);
    hle_latch_record("latch-read-initial", rc, &latch);

    hle_latch_prefill(&latch, HLE_SENTINEL);
    hle_step("latch-peek-initial");
    rc = sceCtrlPeekLatch(&latch);
    hle_latch_record("latch-peek-initial", rc, &latch);

    /* Bounded poll: at most HLE_POLL_ITERATIONS peeks, each followed by a
       HLE_POLL_DELAY_US wait. Observation only; no input is generated. The
       first non-zero latch triggers the Read/Peek semantics sequence. */
    uint32_t nonzero_count = 0;
    uint32_t or_make = 0;
    uint32_t or_break = 0;
    uint32_t or_press = 0;
    uint32_t or_release = 0;
    int have_semantics = 0;
    uint32_t semantics[5] = {0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < HLE_POLL_ITERATIONS; i++) {
        hle_latch_prefill(&latch, 0);
        hle_step("latch-poll-window");
        rc = sceCtrlPeekLatch(&latch);
        or_make |= latch.uiMake;
        or_break |= latch.uiBreak;
        or_press |= latch.uiPress;
        or_release |= latch.uiRelease;
        if (hle_latch_nonzero(&latch)) {
            nonzero_count++;
            if (!have_semantics) {
                have_semantics = 1;
                semantics[0] = hle_latch_nonzero(&latch);
                hle_latch_prefill(&copy, 0);
                hle_step("latch-semantics-peek-2");
                (void)sceCtrlPeekLatch(&copy);
                semantics[1] = hle_latch_nonzero(&copy);
                hle_latch_prefill(&copy, 0);
                hle_step("latch-semantics-read-1");
                (void)sceCtrlReadLatch(&copy);
                semantics[2] = hle_latch_nonzero(&copy);
                hle_latch_prefill(&copy, 0);
                hle_step("latch-semantics-read-2");
                (void)sceCtrlReadLatch(&copy);
                semantics[3] = hle_latch_nonzero(&copy);
                hle_latch_prefill(&copy, 0);
                hle_step("latch-semantics-peek-3");
                (void)sceCtrlPeekLatch(&copy);
                semantics[4] = hle_latch_nonzero(&copy);
            }
        }
        hle_step("latch-poll-delay");
        (void)sceKernelDelayThread(HLE_POLL_DELAY_US);
    }
    const uint32_t poll[6] = {
        HLE_POLL_ITERATIONS, nonzero_count, or_make, or_break, or_press, or_release,
    };
    hle_record("latch-poll-window", "PASS", (uint32_t)rc, poll, 6);

    hle_latch_prefill(&latch, HLE_SENTINEL);
    hle_step("latch-read-after-poll");
    rc = sceCtrlReadLatch(&latch);
    hle_latch_record("latch-read-after-poll", rc, &latch);

    hle_latch_prefill(&latch, HLE_SENTINEL);
    hle_step("latch-peek-after-poll");
    rc = sceCtrlPeekLatch(&latch);
    hle_latch_record("latch-peek-after-poll", rc, &latch);

    if (have_semantics) {
        hle_record("latch-read-clears-peek-keeps", "PASS", 0, semantics, 5);
    } else {
        hle_skip("latch-read-clears-peek-keeps", 5);
    }
    return 1;
}

#endif /* HLE_CASE_CTRL_LATCH */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_SYSPARAM

/* Private string bytes go to their own host0 file, never into a TEST record. */
static void hle_private_nickname_write(const uint8_t *bytes, uint32_t length)
{
    const SceUID fd = sceIoOpen("host0:/hle_sysparam_private_strings.txt",
                                PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        s_log_failed = 1;
        return;
    }
    static char header[64];
    snprintf(header, sizeof(header), "NICKNAME_BYTES=%u\n", (unsigned int)length);
    (void)sceIoWrite(fd, header, strlen(header));
    if (length > 0) {
        (void)sceIoWrite(fd, bytes, (SceSize)length);
    }
    if (sceIoClose(fd) < 0) {
        s_log_failed = 1;
    }
}

static void hle_int_param(const char *case_id, int id)
{
    uint32_t raw = HLE_SENTINEL;
    int value = 0;
    memcpy(&value, &raw, sizeof(value));
    hle_step(case_id);
    const int rc = sceUtilityGetSystemParamInt(id, &value);
    uint32_t out = 0;
    memcpy(&out, &value, sizeof(out));
    hle_record(case_id, "PASS", (uint32_t)rc, &out, 1);
}

#define HLE_STRING_BYTES 256u

static void hle_string_param(const char *case_id, int id, int length, int keep_private)
{
    static uint8_t buf[HLE_STRING_BYTES];
    memset(buf, 0xA5, sizeof(buf));
    hle_step(case_id);
    const int rc = sceUtilityGetSystemParamString(id, (char *)buf, length);
    uint32_t nul = HLE_STRING_BYTES;
    uint32_t extent = 0;
    for (uint32_t i = 0; i < HLE_STRING_BYTES; i++) {
        if (buf[i] == 0 && nul == HLE_STRING_BYTES) {
            nul = i;
        }
        if (buf[i] != 0xA5) {
            extent = i + 1;
        }
    }
    const uint32_t out[3] = {nul, extent, (uint32_t)length};
    hle_record(case_id, "PASS", (uint32_t)rc, out, 3);
    if (keep_private) {
        hle_private_nickname_write(buf, nul);
    }
}

static int hle_run(void)
{
    hle_int_param("int-id-2", 2);
    hle_int_param("int-id-3", 3);
    hle_int_param("int-id-4", 4);
    hle_int_param("int-id-5", 5);
    hle_int_param("int-id-6", 6);
    hle_int_param("int-id-7", 7);
    hle_int_param("int-id-8", 8);
    hle_int_param("int-id-9", 9);
    hle_int_param("int-unknown-0", 0);
    hle_int_param("int-unknown-10", 10);
    hle_int_param("int-unknown-64", 64);
    hle_string_param("string-id-1-len-0x80", 1, 0x80, 1);
    hle_string_param("string-id-1-len-0x04", 1, 0x04, 0);
    return 1;
}

#endif /* HLE_CASE_SYSPARAM */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_GE_EDRAM

static const int s_widths[4] = {512, 1024, 2048, 4096};
static const char *const s_width_cases[4] = {
    "edram-width-set-512", "edram-width-set-1024",
    "edram-width-set-2048", "edram-width-set-4096",
};

static int hle_run(void)
{
    int ok = 1;
    const uint32_t zero = 0;

    hle_step("edram-size");
    const uint32_t size = sceGeEdramGetSize();
    hle_record("edram-size", "PASS", size, &size, 1);

    hle_step("edram-addr");
    const void *addr = sceGeEdramGetAddr();
    const uint32_t addr_bits = (uint32_t)(uintptr_t)addr;
    hle_record("edram-addr", "PASS", addr_bits, &addr_bits, 1);

    /* PSPSDK documents width 0 as "do not set". The PSP-3000 capture of 2026-10-10 (not
       acceptance-eligible, so CAPTURED only) showed Set(0) setting 0 and returning the
       previous width like every other value: this read already leaves 0 behind, and the
       restore below puts the original back when it is a documented width. */
    hle_step("edram-width-query-initial");
    const int initial = sceGeEdramSetAddrTranslation(0);
    hle_record("edram-width-query-initial", "PASS", (uint32_t)initial, &zero, 1);

    /* A width outside the documented four cannot be set back, so the width cells
       run only when the original width is a documented one. */
    const int restorable = (initial == 512 || initial == 1024 ||
                            initial == 2048 || initial == 4096);
    for (int i = 0; i < 4; i++) {
        if (!restorable) {
            hle_skip(s_width_cases[i], 1);
            continue;
        }
        hle_step(s_width_cases[i]);
        const int previous = sceGeEdramSetAddrTranslation(s_widths[i]);
        hle_step("edram-width-post-query");
        const uint32_t after = (uint32_t)sceGeEdramSetAddrTranslation(0);
        hle_record(s_width_cases[i], "PASS", (uint32_t)previous, &after, 1);
    }

    /* Under the captured behaviour (Set(w) sets w and returns the width it replaced, width 0
       included) a Set(0) reports the current width and undoes it in the same call. The restore
       therefore ends with Set(initial) and verifies it with a second Set(initial), which returns
       the width now set and leaves it in place. The previous revision verified with Set(0): its
       own final read then saw 0, every run ended FAIL, and the console was left at width 0. */
    if (!restorable) {
        hle_skip("edram-width-restore", 1);
    } else {
        hle_step("edram-width-restore");
        const int previous = sceGeEdramSetAddrTranslation(initial);
        hle_step("edram-width-restore-verify");
        const uint32_t after = (uint32_t)sceGeEdramSetAddrTranslation(initial);
        hle_record("edram-width-restore", "PASS", (uint32_t)previous, &after, 1);
        ok &= ((int)after == initial);
    }

    /* The final read is Set(initial) when the width was restorable (returns initial, leaves
       initial) and Set(0) otherwise: the initial read already left 0 and nothing can put a
       non-documented width back; the case's soft reset restores the console. out0 records
       the argument. */
    hle_step("edram-width-query-final");
    const uint32_t final_arg = restorable ? (uint32_t)initial : 0u;
    const int final_width = sceGeEdramSetAddrTranslation((int)final_arg);
    hle_record("edram-width-query-final", "PASS", (uint32_t)final_width, &final_arg, 1);
    ok &= (final_width == (restorable ? initial : 0));
    return ok;
}

#endif /* HLE_CASE_GE_EDRAM */

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    s_hle_main_thread = sceKernelGetThreadId();
    hle_begin();
    const int ok = hle_run();
    hle_finish(ok);
    hle_park_until_stop();
    return 0;
}
