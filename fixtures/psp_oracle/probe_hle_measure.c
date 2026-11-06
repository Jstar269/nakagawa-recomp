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
// Eight single-launch cases share this file: CASE=hle-vtimer-unit
// (PSP-HLE-VTIMER-002), hle-vtimer-errors (PSP-HLE-VTIMER-003),
// hle-kernel-pool-bounds (PSP-HLE-KERNEL-STATUS-002), hle-power-float-v0
// (PSP-HLE-POWER-002), hle-null-out-pointers (PSP-HLE-NULL-OUT-001),
// hle-audio-src (PSP-HLE-AUDIO-SRC-001), hle-timer-order
// (PSP-HLE-TIMER-ORDER-001) and hle-savedata-getsize
// (PSP-HLE-SAVEDATA-GETSIZE-001), plus hle-utility-none (PSP-HLE-UTILITY-NONE-001), which
// imports no InitStart call, and hle-utility-dialogs (PSP-HLE-UTILITY-DIALOG-001), which opens
// the savedata, gamedata install, netconf and game sharing dialogs in game context and never
// answers them. Cases 97-101 have recorded PSP-3000 runs from
// 2026-10-15: the VTimer and power cases were measured; pool-bounds was captured
// only in its earlier revision, and the revised form has not run; null-out was
// captured incomplete. Case 104 was measured on 2026-11-02 (docs/HARDWARE_ORACLE.md).
// Their pass rule is machinery only: a record is PASS when its calls completed and were
// recorded in order, and SKIP only when the object a cell needs (a timer, pool or
// block UID) was not obtained. A fixed-pool delete after a refused create is PASS
// with out0 0 and issues no delete, because the refusal is what that create measures.
// No returned value is compared with an expected PSP value, here, in the host checker
// or in the parser; hle-savedata-getsize's check that its save is absent is a precondition
// of its measurement, not a PSP semantic. hle-null-out-pointers passes NULL as a mandatory out pointer,
// which may fault in kernel mode, so it is scheduled last in a session.
//
// Record stream (tools/psp_oracle/protocol.py): NAKAGAWA_PSP_META, then one
// NAKAGAWA_PSP_STEP before each risky call, one NAKAGAWA_PSP_TEST per
// measurement, a `<case>-done` terminal record whose out0 counts the
// measurements, and a final NAKAGAWA_PSP_COMPLETE line. Every line is appended to
// the case's host0 log durably (open, append, close). A bounded wait never waits
// longer than its stated bound, and a family records SKIP rather than omitting a
// cell when its prerequisite did not hold. After the last record the probe runs the
// host0 round trip that probe.c's teardown runs (hle_host0_roundtrip). COMPLETE
// reports FAIL when that round trip fails, and the runner verifies the file it leaves.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <psphprm.h>
#include <pspctrl.h>
#include <psputility.h>
#include <pspge.h>
#include <pspdisplay.h>
#include <pspsuspend.h>
#include <pspstdio.h>
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
#define HLE_CASE_VTIMER_UNIT 97
#define HLE_CASE_VTIMER_ERRORS 98
#define HLE_CASE_KERNEL_POOL_BOUNDS 99
#define HLE_CASE_POWER_FLOAT_V0 100
#define HLE_CASE_NULL_OUT_POINTERS 101
#define HLE_CASE_AUDIO_SRC 102
#define HLE_CASE_TIMER_ORDER 103
#define HLE_CASE_SAVEDATA_GETSIZE 104
#define HLE_CASE_UTILITY_NONE 140
#define HLE_CASE_STD_HANDLES 156
#define HLE_CASE_AUDIO_REST 159
#define HLE_CASE_UTILITY_DIALOGS 161

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
#elif PSP_ORACLE_CASE == HLE_CASE_VTIMER_UNIT
#define HLE_CAMPAIGN_ID "hle-vtimer-unit"
#define HLE_TEST_ID "PSP-HLE-VTIMER-002"
#define HLE_LOG "host0:/hle_vtimer_unit_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VTIMER_ERRORS
#define HLE_CAMPAIGN_ID "hle-vtimer-errors"
#define HLE_TEST_ID "PSP-HLE-VTIMER-003"
#define HLE_LOG "host0:/hle_vtimer_errors_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_KERNEL_POOL_BOUNDS
#define HLE_CAMPAIGN_ID "hle-kernel-pool-bounds"
#define HLE_TEST_ID "PSP-HLE-KERNEL-STATUS-002"
#define HLE_LOG "host0:/hle_kernel_pool_bounds_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_POWER_FLOAT_V0
#define HLE_CAMPAIGN_ID "hle-power-float-v0"
#define HLE_TEST_ID "PSP-HLE-POWER-002"
#define HLE_LOG "host0:/hle_power_float_v0_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_NULL_OUT_POINTERS
#define HLE_CAMPAIGN_ID "hle-null-out-pointers"
#define HLE_TEST_ID "PSP-HLE-NULL-OUT-001"
#define HLE_LOG "host0:/hle_null_out_pointers_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_AUDIO_SRC
#define HLE_CAMPAIGN_ID "hle-audio-src"
#define HLE_TEST_ID "PSP-HLE-AUDIO-SRC-001"
#define HLE_LOG "host0:/hle_audio_src_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_TIMER_ORDER
#define HLE_CAMPAIGN_ID "hle-timer-order"
#define HLE_TEST_ID "PSP-HLE-TIMER-ORDER-001"
#define HLE_LOG "host0:/hle_timer_order_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_SAVEDATA_GETSIZE
#define HLE_CAMPAIGN_ID "hle-savedata-getsize"
#define HLE_TEST_ID "PSP-HLE-SAVEDATA-GETSIZE-001"
#define HLE_LOG "host0:/hle_savedata_getsize_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_UTILITY_NONE
#define HLE_CAMPAIGN_ID "hle-utility-none"
#define HLE_TEST_ID "PSP-HLE-UTILITY-NONE-001"
#define HLE_LOG "host0:/hle_utility_none_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_STD_HANDLES
#define HLE_CAMPAIGN_ID "hle-std-handles"
#define HLE_TEST_ID "PSP-HLE-STDIO-001"
#define HLE_LOG "host0:/hle_std_handles_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_AUDIO_REST
#define HLE_CAMPAIGN_ID "hle-audio-rest"
#define HLE_TEST_ID "PSP-HLE-AUDIO-REST-001"
#define HLE_LOG "host0:/hle_audio_rest_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_UTILITY_DIALOGS
#define HLE_CAMPAIGN_ID "hle-utility-dialogs"
#define HLE_TEST_ID "PSP-HLE-UTILITY-DIALOG-001"
#define HLE_LOG "host0:/hle_utility_dialogs_log.txt"
#else
#error probe_hle_measure.c has no family for this PSP_ORACLE_CASE
#endif
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

#define HLE_SENTINEL 0xA5A5A5A5u
/* The same value as text, for the assembly wrappers (HLE_PFV_WRAPPER). */
#define HLE_SENTINEL_HEX "0xA5A5A5A5"
#define HLE_MAX_WORDS 24u
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
    static char line[1536];
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

/* The host0 round trip probe.c's teardown performs (PROBE_HOST0_ROUNDTRIP_PATH and
   probe_host0_roundtrip there): 64 bytes, byte i = (0x5A ^ (i * 0x25 + (i >> 3))) & 0xFF,
   written with truncate, then read back and compared. The runner's
   _verify_host0_roundtrip reads the file after unload, so every family leaves it.
   probe.c is a separate translation unit, so the contract is repeated here and
   tools/test_psp_oracle.py pins both copies. */
#define HLE_ROUNDTRIP_PATH "host0:/nakagawa_transport_write.bin"
#define HLE_ROUNDTRIP_BYTES 64u

static int hle_host0_roundtrip(void)
{
    uint8_t expected[HLE_ROUNDTRIP_BYTES];
    uint8_t observed[HLE_ROUNDTRIP_BYTES];
    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = (uint8_t)(0x5Au ^ (i * 0x25u + (i >> 3)));
    }
    memset(observed, 0, sizeof(observed));
    SceUID fd = sceIoOpen(HLE_ROUNDTRIP_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return 0;
    }
    const int written = sceIoWrite(fd, expected, (SceSize)sizeof(expected));
    const int write_close = sceIoClose(fd);
    if (written != (int)sizeof(expected) || write_close < 0) {
        return 0;
    }
    fd = sceIoOpen(HLE_ROUNDTRIP_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    const int read = sceIoRead(fd, observed, (SceSize)sizeof(observed));
    const int read_close = sceIoClose(fd);
    return read == (int)sizeof(observed) && read_close >= 0 &&
           memcmp(expected, observed, sizeof(expected)) == 0;
}

/* Shared end of run, after every measurement and the GE width restore. The round trip
   is host I/O only, so it cannot change a cell already recorded. As in probe.c's
   teardown, a failed round trip makes the completion status FAIL. */
static void hle_finish(int ok)
{
    const uint32_t count = s_records;
    hle_emit(HLE_DONE_ID, "PASS", 0, &count, 1);
    const int roundtrip = hle_host0_roundtrip();
    static char line[96];
    snprintf(line, sizeof(line), "NAKAGAWA_PSP_COMPLETE schema=1 status=%s\n",
             (ok && !s_log_failed && roundtrip) ? "PASS" : "FAIL");
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

#define HLE_STRING_BYTES 256u
#define HLE_PRIVATE_STRINGS_PATH "host0:/hle_sysparam_private_strings.txt"
#define HLE_PRIVATE_HEADER_BYTES 64u

/* Private string bytes go to their own host0 file, never into a TEST record. The file
   holds the console owner's nickname and no tool reads it, so it must not outlive the
   case (#1072 M4): the probe reads it back, compares it with what it wrote, and then
   removes it whatever the comparison found. A failed write, read-back, comparison or
   removal marks the log failed, so COMPLETE reports FAIL when the bytes did not round
   trip or the file may still be on the host. */
static void hle_private_nickname_write(const uint8_t *bytes, uint32_t length)
{
    static uint8_t written[HLE_PRIVATE_HEADER_BYTES + HLE_STRING_BYTES];
    static uint8_t observed[sizeof(written) + 1u];
    const int header = snprintf((char *)written, HLE_PRIVATE_HEADER_BYTES,
                                "NICKNAME_BYTES=%u\n", (unsigned int)length);
    if (header <= 0 || header >= (int)HLE_PRIVATE_HEADER_BYTES || length > HLE_STRING_BYTES) {
        s_log_failed = 1;
        return;
    }
    memcpy(written + header, bytes, length);
    const SceSize total = (SceSize)header + (SceSize)length;
    SceUID fd = sceIoOpen(HLE_PRIVATE_STRINGS_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        s_log_failed = 1;
        return;
    }
    const int wrote = sceIoWrite(fd, written, total);
    const int write_close = sceIoClose(fd);
    int verified = wrote == (int)total && write_close >= 0;
    memset(observed, 0, sizeof(observed));
    fd = sceIoOpen(HLE_PRIVATE_STRINGS_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        verified = 0;
    } else {
        const int read = sceIoRead(fd, observed, (SceSize)sizeof(observed));
        const int read_close = sceIoClose(fd);
        verified = verified && read == (int)total && read_close >= 0 &&
                   memcmp(written, observed, total) == 0;
    }
    if (sceIoRemove(HLE_PRIVATE_STRINGS_PATH) < 0 || !verified) {
        s_log_failed = 1;
    }
    memset(written, 0, sizeof(written));
    memset(observed, 0, sizeof(observed));
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
#if PSP_ORACLE_CASE == HLE_CASE_VTIMER_UNIT

/* The VTimer time unit (PSP-HLE-VTIMER-002). One timer is created and started, two
   raw clock readings are taken either side of one bounded 20 ms delay, the base and
   the Refer base field are read while running, then the timer is stopped, the base
   and Refer base field are read again, and the timer is deleted. Every 64-bit clock
   is recorded as its low word then its high word. sceKernelGetSystemTimeWide returns
   the 64-bit value in $v0:$v1, so its result word is the low word. The pass rule is
   machinery only: a cell is PASS when its calls completed and were recorded, and SKIP
   when the timer UID it needs was not obtained. */

#define HLE_VTU_INFO_BYTES ((uint32_t)sizeof(SceKernelVTimerInfo))
#define HLE_VTU_INFO_WORDS 18u
_Static_assert(HLE_VTU_INFO_BYTES == HLE_VTU_INFO_WORDS * sizeof(uint32_t),
               "SceKernelVTimerInfo must fill the 18-word probe buffer");

static void hle_vtu_clock(const char *case_id, uint32_t result, const SceKernelSysClock *clock)
{
    const uint32_t out[2] = {clock->low, clock->hi};
    hle_record(case_id, "PASS", result, out, 2);
}

static SceUID hle_vtu_create(void)
{
    hle_step("vtimer-unit-create");
    const SceUID uid = sceKernelCreateVTimer("hlevtu", NULL);
    const uint32_t created = (uid >= 0) ? 1u : 0u;
    hle_record("vtimer-unit-create", "PASS", (uint32_t)uid, &created, 1);
    return uid;
}

/* Start (`stop` = 0) or stop (`stop` = 1) the timer; out0 is 0. */
static void hle_vtu_control(const char *case_id, SceUID uid, int stop)
{
    if (uid < 0) {
        hle_skip(case_id, 1);
        return;
    }
    const uint32_t zero = 0;
    hle_step(case_id);
    const int rc = stop ? sceKernelStopVTimer(uid) : sceKernelStartVTimer(uid);
    hle_record(case_id, "PASS", (uint32_t)rc, &zero, 1);
}

static void hle_vtu_time(const char *case_id, SceUID uid)
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
    hle_vtu_clock(case_id, (uint32_t)rc, &clock);
}

static void hle_vtu_systime(const char *case_id)
{
    hle_step(case_id);
    const SceInt64 now = sceKernelGetSystemTimeWide();
    SceKernelSysClock clock;
    clock.low = (SceUInt32)((uint64_t)now & 0xFFFFFFFFu);
    clock.hi = (SceUInt32)((uint64_t)now >> 32);
    hle_vtu_clock(case_id, clock.low, &clock);
}

static void hle_vtu_delay(const char *case_id)
{
    const uint32_t requested = HLE_DELAY_US;
    hle_step(case_id);
    const int rc = sceKernelDelayThread(HLE_DELAY_US);
    hle_record(case_id, "PASS", (uint32_t)rc, &requested, 1);
}

static void hle_vtu_base(const char *case_id, SceUID uid)
{
    if (uid < 0) {
        hle_skip(case_id, 2);
        return;
    }
    SceKernelSysClock base;
    base.low = HLE_SENTINEL;
    base.hi = HLE_SENTINEL;
    hle_step(case_id);
    const int rc = sceKernelGetVTimerBase(uid, &base);
    hle_vtu_clock(case_id, (uint32_t)rc, &base);
}

/* ReferVTimerStatus into an 18-word buffer; the record carries only the base field
   (SceKernelVTimerInfo.base). The buffer is prefilled with the sentinel. */
static void hle_vtu_refer_base(const char *case_id, SceUID uid)
{
    if (uid < 0) {
        hle_skip(case_id, 2);
        return;
    }
    uint32_t buf[HLE_VTU_INFO_WORDS];
    hle_prefill(buf, HLE_VTU_INFO_WORDS, HLE_VTU_INFO_BYTES);
    hle_step(case_id);
    const int rc = sceKernelReferVTimerStatus(uid, (SceKernelVTimerInfo *)buf);
    const SceKernelVTimerInfo *info = (const SceKernelVTimerInfo *)buf;
    hle_vtu_clock(case_id, (uint32_t)rc, &info->base);
}

static void hle_vtu_delete(SceUID uid)
{
    if (uid < 0) {
        hle_skip("vtimer-unit-delete", 1);
        return;
    }
    const uint32_t zero = 0;
    hle_step("vtimer-unit-delete");
    const int rc = sceKernelDeleteVTimer(uid);
    hle_record("vtimer-unit-delete", "PASS", (uint32_t)rc, &zero, 1);
}

static int hle_run(void)
{
    const SceUID vt = hle_vtu_create();
    hle_vtu_control("vtimer-unit-start", vt, 0);
    hle_vtu_time("vtimer-unit-time-a", vt);
    hle_vtu_systime("vtimer-unit-systime-a");
    hle_vtu_delay("vtimer-unit-delay");
    hle_vtu_time("vtimer-unit-time-b", vt);
    hle_vtu_systime("vtimer-unit-systime-b");
    hle_vtu_base("vtimer-unit-base-running", vt);
    hle_vtu_refer_base("vtimer-unit-refer-running", vt);
    hle_vtu_control("vtimer-unit-stop", vt, 1);
    hle_vtu_base("vtimer-unit-base-stopped", vt);
    hle_vtu_refer_base("vtimer-unit-refer-stopped", vt);
    hle_vtu_delete(vt);
    return 1;
}

#endif /* HLE_CASE_VTIMER_UNIT */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VTIMER_ERRORS

/* VTimer cells that pass a UID already deleted in this run, and create up to
   HLE_VTE_NOSLOT_CAP timers (PSP-HLE-VTIMER-003). The stale UID is deleted by a cleanup
   cell after its last use, whatever the delete cell returned, so a delete that failed
   cannot leave that timer running for the cells after it. The NULL that CreateVTimer
   takes as its option pointer is an optional argument. The pass rule is machinery only:
   a cell is PASS when its calls completed and were recorded, and SKIP when the UID it
   needs was not obtained. */

#define HLE_VTE_NOSLOT_CAP 64u
#define HLE_VTE_NAME "hlevte"

static SceUID hle_vte_create(const char *case_id)
{
    hle_step(case_id);
    const SceUID uid = sceKernelCreateVTimer(HLE_VTE_NAME, NULL);
    const uint32_t created = (uid >= 0) ? 1u : 0u;
    hle_record(case_id, "PASS", (uint32_t)uid, &created, 1);
    return uid;
}

static void hle_vte_control(const char *case_id, SceUID uid, int stop)
{
    if (uid < 0) {
        hle_skip(case_id, 1);
        return;
    }
    const uint32_t zero = 0;
    hle_step(case_id);
    const int rc = stop ? sceKernelStopVTimer(uid) : sceKernelStartVTimer(uid);
    hle_record(case_id, "PASS", (uint32_t)rc, &zero, 1);
}

static void hle_vte_delete(const char *case_id, SceUID uid)
{
    if (uid < 0) {
        hle_skip(case_id, 1);
        return;
    }
    const uint32_t zero = 0;
    hle_step(case_id);
    const int rc = sceKernelDeleteVTimer(uid);
    hle_record(case_id, "PASS", (uint32_t)rc, &zero, 1);
}

/* GetVTimerTime with a valid out pointer records the clock that came back. */
static void hle_vte_time_valid(const char *case_id, SceUID uid)
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

/* Deletes the stale UID after its last use, whatever the delete cell returned. The
   record holds only the return code, so it carries no out words (count 0). */
static void hle_vte_cleanup_stale(SceUID uid)
{
    hle_step("vtimer-err-cleanup-stale");
    const int rc = sceKernelDeleteVTimer(uid);
    hle_record("vtimer-err-cleanup-stale", "PASS", (uint32_t)rc, NULL, 0);
}

/* Up to HLE_VTE_NOSLOT_CAP creates, stopping at the first negative code. The create
   cell's result is the first refusal code (0 when none came within the cap); out0
   counts the timers created and out1 records whether a refusal occurred. The cleanup
   cell's result is the first delete return; out0 is the count created and out1 the
   number of delete calls issued. Returns 1 once both cells are recorded. */
static int hle_vte_noslot(void)
{
    SceUID slots[HLE_VTE_NOSLOT_CAP];
    uint32_t created = 0;
    uint32_t refusal = 0;
    uint32_t refused = 0;
    hle_step("vtimer-err-noslot-create");
    for (uint32_t i = 0; i < HLE_VTE_NOSLOT_CAP; i++) {
        const SceUID uid = sceKernelCreateVTimer(HLE_VTE_NAME, NULL);
        if (uid < 0) {
            refusal = (uint32_t)uid;
            refused = 1;
            break;
        }
        slots[created] = uid;
        created++;
    }
    const uint32_t create_out[2] = {created, refused};
    hle_record("vtimer-err-noslot-create", "PASS", refusal, create_out, 2);

    uint32_t issued = 0;
    uint32_t first = 0;
    hle_step("vtimer-err-noslot-cleanup");
    for (uint32_t i = 0; i < created; i++) {
        const int rc = sceKernelDeleteVTimer(slots[i]);
        if (issued == 0) {
            first = (uint32_t)rc;
        }
        issued++;
    }
    const uint32_t cleanup_out[2] = {created, issued};
    hle_record("vtimer-err-noslot-cleanup", "PASS", first, cleanup_out, 2);
    return 1;
}

static int hle_run(void)
{
    /* A timer created and deleted here gives the stale UID for the cells below. */
    const SceUID stale = hle_vte_create("vtimer-err-create-stale");
    hle_vte_delete("vtimer-err-delete-stale", stale);
    hle_vte_control("vtimer-err-start-deleted", stale, 0);
    hle_vte_control("vtimer-err-stop-deleted", stale, 1);
    hle_vte_time_valid("vtimer-err-time-deleted", stale);
    hle_vte_cleanup_stale(stale);

    const SceUID live = hle_vte_create("vtimer-err-create-live");
    hle_vte_delete("vtimer-err-delete-live", live);

    return hle_vte_noslot();
}

#endif /* HLE_CASE_VTIMER_ERRORS */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_KERNEL_POOL_BOUNDS

/* Zero-size and maximum-size fixed pools, and a small variable pool exhausted by
   TryAllocateVpl (PSP-HLE-KERNEL-STATUS-002). The pool creates pass a NULL option
   pointer, the optional argument. Every fixed pool created is deleted; a fixed-pool
   create that is refused leaves no pool, and its delete cell records that as a PASS with
   out0 0 instead of a SKIP, so a refusal, which is what these creates measure, keeps the
   envelope eligible. The VPL blocks are freed, bounded at HLE_KSN_VPL_CAP allocations,
   before the VPL is deleted. The pass rule is machinery only: completion and cleanup. */

#define HLE_KSN_PART 2
#define HLE_KSN_VPL_POOL 0x1000u
#define HLE_KSN_VPL_REQUEST 0x100u
#define HLE_KSN_VPL_CAP 64u

static SceUID hle_ksn_fpl_create(const char *case_id, uint32_t block_size, uint32_t blocks)
{
    hle_step(case_id);
    const SceUID uid = sceKernelCreateFpl("hlefplk", HLE_KSN_PART, 0, block_size, blocks, NULL);
    const uint32_t created = (uid >= 0) ? 1u : 0u;
    hle_record(case_id, "PASS", (uint32_t)uid, &created, 1);
    return uid;
}

/* Deletes the pool its create returned. out0 is 1 when a delete was issued, and the result
   is its return. With no pool (the create was refused) the cell issues no call and records
   PASS with result 0 and out0 0: there is no object to delete. */
static void hle_ksn_fpl_delete(const char *case_id, SceUID uid)
{
    if (uid < 0) {
        const uint32_t no_object = 0;
        hle_record(case_id, "PASS", 0u, &no_object, 1);
        return;
    }
    const uint32_t issued = 1;
    hle_step(case_id);
    const int rc = sceKernelDeleteFpl(uid);
    hle_record(case_id, "PASS", (uint32_t)rc, &issued, 1);
}

static SceUID hle_ksn_vpl_create(void)
{
    hle_step("vpl-create");
    const SceUID uid = sceKernelCreateVpl("hlevpl", HLE_KSN_PART, 0, HLE_KSN_VPL_POOL, NULL);
    const uint32_t created = (uid >= 0) ? 1u : 0u;
    hle_record("vpl-create", "PASS", (uint32_t)uid, &created, 1);
    return uid;
}

/* TryAllocateVpl until a negative code, at most HLE_KSN_VPL_CAP times. The result is
   the first refusal code (0 when none came within the cap); out0 counts the blocks
   taken and out1 is 1 when a refusal was seen. Returns the number of blocks taken. */
static uint32_t hle_ksn_vpl_alloc(SceUID vpl, void **blocks)
{
    if (vpl < 0) {
        hle_skip("vpl-try-alloc-refusal", 2);
        return 0;
    }
    uint32_t taken = 0;
    uint32_t refusal = 0;
    uint32_t refused = 0;
    void *block = NULL;
    hle_step("vpl-try-alloc-refusal");
    for (uint32_t i = 0; i < HLE_KSN_VPL_CAP; i++) {
        const int rc = sceKernelTryAllocateVpl(vpl, HLE_KSN_VPL_REQUEST, &block);
        if (rc < 0) {
            refusal = (uint32_t)rc;
            refused = 1;
            break;
        }
        blocks[taken] = block;
        taken++;
    }
    const uint32_t out[2] = {taken, refused};
    hle_record("vpl-try-alloc-refusal", "PASS", refusal, out, 2);
    return taken;
}

/* Frees the `taken` blocks. The result is the first free return; out0 counts the blocks
   taken and out1 the free calls issued, which must match for a full cleanup. */
static void hle_ksn_vpl_free(SceUID vpl, void **blocks, uint32_t taken)
{
    if (vpl < 0) {
        hle_skip("vpl-free-all", 2);
        return;
    }
    uint32_t issued = 0;
    uint32_t first = 0;
    hle_step("vpl-free-all");
    for (uint32_t i = 0; i < taken; i++) {
        const int rc = sceKernelFreeVpl(vpl, blocks[i]);
        if (issued == 0) {
            first = (uint32_t)rc;
        }
        issued++;
    }
    const uint32_t out[2] = {taken, issued};
    hle_record("vpl-free-all", "PASS", first, out, 2);
}

static void hle_ksn_vpl_delete(SceUID vpl)
{
    if (vpl < 0) {
        hle_skip("vpl-delete", 1);
        return;
    }
    const uint32_t zero = 0;
    hle_step("vpl-delete");
    const int rc = sceKernelDeleteVpl(vpl);
    hle_record("vpl-delete", "PASS", (uint32_t)rc, &zero, 1);
}

static int hle_run(void)
{
    const SceUID fpl_zero = hle_ksn_fpl_create("fpl-create-size-0x00", 0u, 1u);
    hle_ksn_fpl_delete("fpl-delete-size-0x00", fpl_zero);
    const SceUID fpl_max = hle_ksn_fpl_create("fpl-create-size-0x7fffffff", 0x7fffffffu, 1u);
    hle_ksn_fpl_delete("fpl-delete-size-0x7fffffff", fpl_max);

    const SceUID vpl = hle_ksn_vpl_create();
    void *blocks[HLE_KSN_VPL_CAP] = {NULL};
    const uint32_t taken = hle_ksn_vpl_alloc(vpl, blocks);
    hle_ksn_vpl_free(vpl, blocks, taken);
    hle_ksn_vpl_delete(vpl);
    return 1;
}

#endif /* HLE_CASE_KERNEL_POOL_BOUNDS */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_NULL_OUT_POINTERS

/* NULL as a mandatory out pointer (PSP-HLE-NULL-OUT-001). Four calls pass NULL where
   pspthreadman.h makes the pointer a mandatory output: sceKernelReferSystemStatus,
   sceKernelGetVTimerTime, sceKernelGetVTimerBase and sceKernelReferVTimerStatus. The
   kernel's user-pointer check only tests the sign bit, so a NULL may fault in kernel
   mode and hang the PSP, and a fault loses every later cell in the launch. This case
   is scheduled last in a session, after every other case. It creates one live VTimer
   for the three VTimer calls, deletes it at the end, and records only return codes
   (count 0). The pass rule is machinery only: a cell is PASS when its call returned and
   was recorded, and SKIP when the timer UID it needs was not obtained. */

static SceUID hle_null_vtimer_create(void)
{
    hle_step("null-vtimer-create");
    const SceUID uid = sceKernelCreateVTimer("hlenul", NULL);
    hle_record("null-vtimer-create", "PASS", (uint32_t)uid, NULL, 0);
    return uid;
}

static void hle_null_sys_status(void)
{
    hle_step("null-sys-status");
    const int rc = sceKernelReferSystemStatus(NULL);
    hle_record("null-sys-status", "PASS", (uint32_t)rc, NULL, 0);
}

static void hle_null_time(SceUID uid)
{
    if (uid < 0) {
        hle_skip("null-vtimer-time", 0);
        return;
    }
    hle_step("null-vtimer-time");
    const int rc = sceKernelGetVTimerTime(uid, NULL);
    hle_record("null-vtimer-time", "PASS", (uint32_t)rc, NULL, 0);
}

static void hle_null_base(SceUID uid)
{
    if (uid < 0) {
        hle_skip("null-vtimer-base", 0);
        return;
    }
    hle_step("null-vtimer-base");
    const int rc = sceKernelGetVTimerBase(uid, NULL);
    hle_record("null-vtimer-base", "PASS", (uint32_t)rc, NULL, 0);
}

static void hle_null_refer(SceUID uid)
{
    if (uid < 0) {
        hle_skip("null-vtimer-refer", 0);
        return;
    }
    hle_step("null-vtimer-refer");
    const int rc = sceKernelReferVTimerStatus(uid, NULL);
    hle_record("null-vtimer-refer", "PASS", (uint32_t)rc, NULL, 0);
}

static void hle_null_delete(SceUID uid)
{
    if (uid < 0) {
        hle_skip("null-vtimer-delete", 0);
        return;
    }
    hle_step("null-vtimer-delete");
    const int rc = sceKernelDeleteVTimer(uid);
    hle_record("null-vtimer-delete", "PASS", (uint32_t)rc, NULL, 0);
}

static int hle_run(void)
{
    const SceUID vt = hle_null_vtimer_create();
    hle_null_sys_status();
    hle_null_time(vt);
    hle_null_base(vt);
    hle_null_refer(vt);
    hle_null_delete(vt);
    return 1;
}

#endif /* HLE_CASE_NULL_OUT_POINTERS */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_POWER_FLOAT_V0

/* Raw registers of the three scePower Float getters (PSP-HLE-POWER-002). A getter's
   integer return register is $v0 and its float return register is $f0, so no C call
   reads both. Each wrapper below is a few instructions of assembly: it saves $ra and
   its output pointer in a 32-byte frame, presets $f0 to HLE_SENTINEL, makes one `jal`
   to its import, stores $v0 and the raw bits of $f0 to the output words, and returns.
   A getter that leaves $f0 untouched therefore records the sentinel bits in out0. It
   touches no other register the caller can observe. The record carries $v0 as result
   and the $f0 bits as out0. The code is placed in .text whatever section is current.
   hle_power_imports.S supplies the NIDs; that block also declares the other five
   getters, which this family does not call. */

#define HLE_PFV_WRAPPER(wrapper, target)                                      \
    __asm__(                                                                  \
        ".pushsection .text\n\t"                                              \
        ".set push\n\t"                                                       \
        ".set noreorder\n\t"                                                  \
        ".globl " #wrapper "\n\t"                                             \
        ".ent " #wrapper "\n"                                                 \
        #wrapper ":\n\t"                                                      \
        "addiu $sp, $sp, -32\n\t"                                             \
        "sw $ra, 28($sp)\n\t"                                                 \
        "sw $a0, 24($sp)\n\t"                                                 \
        "li $t1, " HLE_SENTINEL_HEX "\n\t"                                    \
        "mtc1 $t1, $f0\n\t"                                                   \
        "jal " #target "\n\t"                                                 \
        "nop\n\t"                                                             \
        "lw $t0, 24($sp)\n\t"                                                 \
        "mfc1 $t1, $f0\n\t"                                                   \
        "sw $v0, 0($t0)\n\t"                                                  \
        "sw $t1, 4($t0)\n\t"                                                  \
        "lw $ra, 28($sp)\n\t"                                                 \
        "addiu $sp, $sp, 32\n\t"                                              \
        "jr $ra\n\t"                                                          \
        "nop\n\t"                                                             \
        ".end " #wrapper "\n\t"                                               \
        ".set pop\n\t"                                                        \
        ".popsection\n")

HLE_PFV_WRAPPER(hle_pfv_raw_pll, scePowerGetPllClockFrequencyFloat);
HLE_PFV_WRAPPER(hle_pfv_raw_cpu, scePowerGetCpuClockFrequencyFloat);
HLE_PFV_WRAPPER(hle_pfv_raw_bus, scePowerGetBusClockFrequencyFloat);

void hle_pfv_raw_pll(uint32_t *out);
void hle_pfv_raw_cpu(uint32_t *out);
void hle_pfv_raw_bus(uint32_t *out);

static void hle_pfv_cell(const char *case_id, void (*raw)(uint32_t *out))
{
    uint32_t words[2] = {HLE_SENTINEL, HLE_SENTINEL};
    hle_step(case_id);
    raw(words);
    const uint32_t v0 = words[0];
    const uint32_t f0 = words[1];
    hle_record(case_id, "PASS", v0, &f0, 1);
}

static int hle_run(void)
{
    hle_pfv_cell("pll-clock-float-v0", hle_pfv_raw_pll);
    hle_pfv_cell("cpu-clock-float-v0", hle_pfv_raw_cpu);
    hle_pfv_cell("bus-clock-float-v0", hle_pfv_raw_bus);
    return 1;
}

#endif /* HLE_CASE_POWER_FLOAT_V0 */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_AUDIO_SRC

/* SRC and Output2 return words are observations only. The only output buffers
   are this probe-owned, aligned, all-zero stereo block. SRC has no rest-sample
   query in the project's public NID corpus, so the blocking call itself is the
   drain boundary. Every acquired reservation is released immediately when a
   shape probe unexpectedly succeeds and is retried once during final cleanup
   if an earlier release did not return success. */
#define HLE_ASRC_SAMPLES 1024u
#define HLE_ASRC_FREQUENCY 44100u
#define HLE_ASRC_CHANNELS 2u
#define HLE_ASRC_OUTPUT2_SAMPLES 512u
#define HLE_ASRC_NOT_CALLED 0xffffffffu

static int16_t s_hle_asrc_silence[HLE_ASRC_SAMPLES * HLE_ASRC_CHANNELS]
    __attribute__((aligned(64)));
static int s_hle_asrc_src_reserved;
static int s_hle_asrc_output2_reserved;

static int hle_asrc_release_src(const char *case_id)
{
    hle_step(case_id);
    const int rc = sceAudioSRCChRelease();
    hle_record(case_id, "PASS", (uint32_t)rc, NULL, 0);
    if (rc >= 0) {
        s_hle_asrc_src_reserved = 0;
    }
    return rc;
}

static int hle_asrc_release_output2(const char *case_id)
{
    hle_step(case_id);
    const int rc = sceAudioOutput2Release();
    hle_record(case_id, "PASS", (uint32_t)rc, NULL, 0);
    if (rc >= 0) {
        s_hle_asrc_output2_reserved = 0;
    }
    return rc;
}

static void hle_asrc_invalid_reserve(const char *case_id, uint32_t samples,
                                     uint32_t frequency, uint32_t channels)
{
    if (s_hle_asrc_src_reserved) {
        hle_skip(case_id, 4);
        return;
    }
    hle_step(case_id);
    const int rc = sceAudioSRCChReserve((int)samples, (int)frequency, (int)channels);
    uint32_t release_rc = HLE_ASRC_NOT_CALLED;
    if (rc >= 0) {
        s_hle_asrc_src_reserved = 1;
        char step[72];
        snprintf(step, sizeof(step), "%s-release", case_id);
        hle_step(step);
        release_rc = (uint32_t)sceAudioSRCChRelease();
        if ((int32_t)release_rc >= 0) {
            s_hle_asrc_src_reserved = 0;
        }
    }
    const uint32_t out[4] = {samples, frequency, channels, release_rc};
    hle_record(case_id, "PASS", (uint32_t)rc, out, 4);
}

static void hle_asrc_output2_then_src(void)
{
    if (s_hle_asrc_src_reserved || s_hle_asrc_output2_reserved) {
        hle_skip("out2-then-src-reserve", 3);
        return;
    }
    hle_step("out2-then-src-output2-reserve");
    const int output2_rc = sceAudioOutput2Reserve((int)HLE_ASRC_OUTPUT2_SAMPLES);
    int src_rc = (int)HLE_ASRC_NOT_CALLED;
    int src_release_rc = (int)HLE_ASRC_NOT_CALLED;
    int output2_release_rc = (int)HLE_ASRC_NOT_CALLED;
    if (output2_rc >= 0) {
        s_hle_asrc_output2_reserved = 1;
        hle_step("out2-then-src-src-reserve");
        src_rc = sceAudioSRCChReserve((int)HLE_ASRC_SAMPLES,
                                      (int)HLE_ASRC_FREQUENCY,
                                      (int)HLE_ASRC_CHANNELS);
        if (src_rc >= 0) {
            s_hle_asrc_src_reserved = 1;
            hle_step("out2-then-src-src-release");
            src_release_rc = sceAudioSRCChRelease();
            if (src_release_rc >= 0) {
                s_hle_asrc_src_reserved = 0;
            }
        }
        hle_step("out2-then-src-output2-release");
        output2_release_rc = sceAudioOutput2Release();
        if (output2_release_rc >= 0) {
            s_hle_asrc_output2_reserved = 0;
        }
    }
    const uint32_t out[3] = {(uint32_t)output2_rc, (uint32_t)src_release_rc,
                             (uint32_t)output2_release_rc};
    hle_record("out2-then-src-reserve", output2_rc < 0 ? "SKIP" : "PASS",
               (uint32_t)src_rc, out, 3);
}

static void hle_asrc_src_then_output2(void)
{
    if (s_hle_asrc_src_reserved || s_hle_asrc_output2_reserved) {
        hle_skip("src-then-out2-reserve", 3);
        return;
    }
    hle_step("src-then-out2-src-reserve");
    const int src_rc = sceAudioSRCChReserve((int)HLE_ASRC_SAMPLES,
                                            (int)HLE_ASRC_FREQUENCY,
                                            (int)HLE_ASRC_CHANNELS);
    int output2_rc = (int)HLE_ASRC_NOT_CALLED;
    int src_release_rc = (int)HLE_ASRC_NOT_CALLED;
    int output2_release_rc = (int)HLE_ASRC_NOT_CALLED;
    if (src_rc >= 0) {
        s_hle_asrc_src_reserved = 1;
        hle_step("src-then-out2-output2-reserve");
        output2_rc = sceAudioOutput2Reserve((int)HLE_ASRC_OUTPUT2_SAMPLES);
        if (output2_rc >= 0) {
            s_hle_asrc_output2_reserved = 1;
        }
        hle_step("src-then-out2-src-release");
        src_release_rc = sceAudioSRCChRelease();
        if (src_release_rc >= 0) {
            s_hle_asrc_src_reserved = 0;
        }
        if (output2_rc >= 0) {
            hle_step("src-then-out2-output2-release");
            output2_release_rc = sceAudioOutput2Release();
            if (output2_release_rc >= 0) {
                s_hle_asrc_output2_reserved = 0;
            }
        }
    }
    const uint32_t out[3] = {(uint32_t)src_rc, (uint32_t)src_release_rc,
                             (uint32_t)output2_release_rc};
    hle_record("src-then-out2-reserve", src_rc < 0 ? "SKIP" : "PASS",
               (uint32_t)output2_rc, out, 3);
}

static int hle_run(void)
{
    memset(s_hle_asrc_silence, 0, sizeof(s_hle_asrc_silence));

    hle_asrc_release_src("src-release-unreserved");
    const uint32_t unreserved_out[2] = {HLE_ASRC_SAMPLES,
                                        (uint32_t)sizeof(s_hle_asrc_silence)};
    hle_step("src-output-unreserved");
    const int unreserved_output =
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, s_hle_asrc_silence);
    hle_record("src-output-unreserved", "PASS", (uint32_t)unreserved_output,
               unreserved_out, 2);

    hle_step("src-reserve-valid");
    const int reserve_rc = sceAudioSRCChReserve((int)HLE_ASRC_SAMPLES,
                                                (int)HLE_ASRC_FREQUENCY,
                                                (int)HLE_ASRC_CHANNELS);
    if (reserve_rc >= 0) {
        s_hle_asrc_src_reserved = 1;
    }
    const uint32_t reserve_out[3] = {HLE_ASRC_SAMPLES, HLE_ASRC_FREQUENCY,
                                     HLE_ASRC_CHANNELS};
    hle_record("src-reserve-valid", "PASS", (uint32_t)reserve_rc, reserve_out, 3);

    if (reserve_rc < 0) {
        hle_skip("src-reserve-already-held", 3);
        hle_skip("src-output-blocking-0", 2);
        hle_skip("src-release-after-output", 0);
    } else {
        hle_step("src-reserve-already-held");
        const int second_rc = sceAudioSRCChReserve((int)HLE_ASRC_SAMPLES,
                                                   (int)HLE_ASRC_FREQUENCY,
                                                   (int)HLE_ASRC_CHANNELS);
        hle_record("src-reserve-already-held", "PASS", (uint32_t)second_rc,
                   reserve_out, 3);
        const uint32_t block_out[2] = {HLE_ASRC_SAMPLES,
                                       (uint32_t)sizeof(s_hle_asrc_silence)};
        hle_step("src-output-blocking-0");
        const int output_rc =
            sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, s_hle_asrc_silence);
        hle_record("src-output-blocking-0", "PASS", (uint32_t)output_rc,
                   block_out, 2);
        hle_asrc_release_src("src-release-after-output");
    }
    hle_asrc_release_src("src-release-again");

    hle_asrc_invalid_reserve("src-reserve-invalid-samples-zero", 0u,
                             HLE_ASRC_FREQUENCY, HLE_ASRC_CHANNELS);
    hle_asrc_invalid_reserve("src-reserve-invalid-samples-65", 65u,
                             HLE_ASRC_FREQUENCY, HLE_ASRC_CHANNELS);
    hle_asrc_invalid_reserve("src-reserve-invalid-samples-4096", 4096u,
                             HLE_ASRC_FREQUENCY, HLE_ASRC_CHANNELS);
    hle_asrc_invalid_reserve("src-reserve-invalid-frequency", HLE_ASRC_SAMPLES,
                             48000u, HLE_ASRC_CHANNELS);
    hle_asrc_invalid_reserve("src-reserve-invalid-channels-1", HLE_ASRC_SAMPLES,
                             HLE_ASRC_FREQUENCY, 1u);
    hle_asrc_invalid_reserve("src-reserve-invalid-channels-4", HLE_ASRC_SAMPLES,
                             HLE_ASRC_FREQUENCY, 4u);
    hle_asrc_output2_then_src();
    hle_asrc_src_then_output2();

    /* Nothing left reserved is the expected end state, not a skipped cell: the record passes
       with the NOT_CALLED word, so a clean run stays acceptance-eligible (the 2026-10-15 run on
       22b027f6 recorded these two as SKIP, which made a complete capture ineligible). */
    int cleanup_ok = 1;
    if (s_hle_asrc_src_reserved) {
        cleanup_ok &= hle_asrc_release_src("src-final-cleanup") >= 0;
    } else {
        hle_record("src-final-cleanup", "PASS", HLE_ASRC_NOT_CALLED, NULL, 0);
    }
    if (s_hle_asrc_output2_reserved) {
        cleanup_ok &= hle_asrc_release_output2("out2-final-cleanup") >= 0;
    } else {
        hle_record("out2-final-cleanup", "PASS", HLE_ASRC_NOT_CALLED, NULL, 0);
    }
    return cleanup_ok && !s_hle_asrc_src_reserved &&
           !s_hle_asrc_output2_reserved;
}

#endif /* HLE_CASE_AUDIO_SRC */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_TIMER_ORDER

/* Timer ordering only: callback sequence and the system-time words sampled
   inside each callback are observations, not deadlines or timing claims. The
   callback common words and VTimer target are probe-owned storage. Each wait is
   one finite DelayThread duration, with no unbounded polling loop. */
#define HLE_TIMER_WAIT_LIMIT_US 250000u
#define HLE_TIMER_EQUAL_DELAY_US 50000u
#define HLE_TIMER_LONG_DELAY_US 100000u
#define HLE_TIMER_SHORT_DELAY_US 30000u
#define HLE_TIMER_VTIMER_DELAY_US 60000u
#define HLE_TIMER_ZERO_WAIT_US 100000u
#define HLE_TIMER_NO_TIME 0xffffffffu

struct hle_timer_order_capture {
    volatile uint32_t sequence;
    volatile uint32_t low;
    volatile uint32_t high;
};

static volatile uint32_t s_hle_timer_order_sequence;
static struct hle_timer_order_capture s_hle_alarm_same_a;
static struct hle_timer_order_capture s_hle_alarm_same_b;
static struct hle_timer_order_capture s_hle_alarm_long;
static struct hle_timer_order_capture s_hle_alarm_short;
static struct hle_timer_order_capture s_hle_vtimer_event;
static struct hle_timer_order_capture s_hle_vtimer_alarm_event;
static struct hle_timer_order_capture s_hle_alarm_zero;
static SceKernelSysClock s_hle_vtimer_due;
static struct SceKernelVTimerOptParam s_hle_vtimer_options = {
    sizeof(struct SceKernelVTimerOptParam)
};

static void hle_timer_order_capture(struct hle_timer_order_capture *capture)
{
    const uint32_t sequence = ++s_hle_timer_order_sequence;
    const uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();
    capture->low = (uint32_t)now;
    capture->high = (uint32_t)(now >> 32);
    capture->sequence = sequence;
}

static SceUInt hle_alarm_same_a(void *common)
{
    hle_timer_order_capture((struct hle_timer_order_capture *)common);
    return 0;
}

static SceUInt hle_alarm_same_b(void *common)
{
    hle_timer_order_capture((struct hle_timer_order_capture *)common);
    return 0;
}

static SceUInt hle_alarm_long(void *common)
{
    hle_timer_order_capture((struct hle_timer_order_capture *)common);
    return 0;
}

static SceUInt hle_alarm_short(void *common)
{
    hle_timer_order_capture((struct hle_timer_order_capture *)common);
    return 0;
}

static SceUInt hle_alarm_vtimer_equal(void *common)
{
    hle_timer_order_capture((struct hle_timer_order_capture *)common);
    return 0;
}

static SceUInt hle_alarm_zero(void *common)
{
    hle_timer_order_capture((struct hle_timer_order_capture *)common);
    return 0;
}

static SceUInt hle_vtimer_order_handler(SceUID uid, SceKernelSysClock *current,
                                        SceKernelSysClock *scheduled, void *common)
{
    (void)uid;
    (void)current;
    (void)scheduled;
    hle_timer_order_capture((struct hle_timer_order_capture *)common);
    return 0;
}

static void hle_timer_order_wait(const char *step, uint32_t timeout_us)
{
    hle_step(step);
    sceKernelDelayThread(timeout_us);
}

static void hle_timer_order_event(const char *case_id,
                                  const struct hle_timer_order_capture *capture,
                                  int scheduled)
{
    if (!scheduled) {
        hle_skip(case_id, 2);
        return;
    }
    const uint32_t out[2] = {
        capture->sequence != 0u ? capture->low : HLE_TIMER_NO_TIME,
        capture->sequence != 0u ? capture->high : HLE_TIMER_NO_TIME,
    };
    hle_record(case_id, "PASS", capture->sequence, out, 2);
}

static uint32_t hle_timer_order_cancel(SceUID uid, const char *step)
{
    if (uid < 0) {
        return HLE_TIMER_NO_TIME;
    }
    hle_step(step);
    return (uint32_t)sceKernelCancelAlarm(uid);
}

static int hle_run(void)
{
    hle_step("timer-order-set-same-a");
    const SceUID alarm_same_a = sceKernelSetAlarm(HLE_TIMER_EQUAL_DELAY_US,
                                                   hle_alarm_same_a,
                                                   &s_hle_alarm_same_a);
    hle_step("timer-order-set-same-b");
    const SceUID alarm_same_b = sceKernelSetAlarm(HLE_TIMER_EQUAL_DELAY_US,
                                                   hle_alarm_same_b,
                                                   &s_hle_alarm_same_b);
    const uint32_t same_set_out[1] = {(uint32_t)alarm_same_b};
    hle_record("alarm-same-delay-set", "PASS", (uint32_t)alarm_same_a,
               same_set_out, 1);
    hle_timer_order_wait("timer-order-wait-same", HLE_TIMER_WAIT_LIMIT_US);
    hle_timer_order_event("alarm-same-delay-a", &s_hle_alarm_same_a,
                          alarm_same_a >= 0);
    hle_timer_order_event("alarm-same-delay-b", &s_hle_alarm_same_b,
                          alarm_same_b >= 0);

    hle_step("timer-order-set-long");
    const SceUID alarm_long = sceKernelSetAlarm(HLE_TIMER_LONG_DELAY_US,
                                                 hle_alarm_long, &s_hle_alarm_long);
    hle_step("timer-order-set-short");
    const SceUID alarm_short = sceKernelSetAlarm(HLE_TIMER_SHORT_DELAY_US,
                                                  hle_alarm_short, &s_hle_alarm_short);
    const uint32_t order_set_out[1] = {(uint32_t)alarm_short};
    hle_record("alarm-long-then-short-set", "PASS", (uint32_t)alarm_long,
               order_set_out, 1);
    hle_timer_order_wait("timer-order-wait-long-short", HLE_TIMER_WAIT_LIMIT_US);
    hle_timer_order_event("alarm-long-event", &s_hle_alarm_long, alarm_long >= 0);
    hle_timer_order_event("alarm-short-event", &s_hle_alarm_short, alarm_short >= 0);

    hle_step("timer-order-vtimer-create");
    const SceUID vtimer = sceKernelCreateVTimer("hleord", &s_hle_vtimer_options);
    const uint32_t vtimer_created = vtimer >= 0 ? 1u : 0u;
    hle_record("timer-order-vtimer-create", "PASS", (uint32_t)vtimer,
               &vtimer_created, 1);
    int vtimer_handler_rc = -1;
    int vtimer_start_rc = -1;
    SceUID vtimer_alarm = (SceUID)-1;
    if (vtimer >= 0) {
        s_hle_vtimer_due.low = HLE_TIMER_VTIMER_DELAY_US;
        s_hle_vtimer_due.hi = 0;
        hle_step("timer-order-vtimer-handler");
        vtimer_handler_rc = sceKernelSetVTimerHandler(
            vtimer, &s_hle_vtimer_due, hle_vtimer_order_handler, &s_hle_vtimer_event);
        hle_step("timer-order-vtimer-start");
        vtimer_start_rc = sceKernelStartVTimer(vtimer);
        if (vtimer_handler_rc >= 0 && vtimer_start_rc >= 0) {
            hle_step("timer-order-vtimer-alarm");
            vtimer_alarm = sceKernelSetAlarm(HLE_TIMER_VTIMER_DELAY_US,
                                               hle_alarm_vtimer_equal,
                                               &s_hle_vtimer_alarm_event);
        }
    }
    const uint32_t vtimer_setup_out[1] = {(uint32_t)vtimer_start_rc};
    hle_record("timer-order-vtimer-handler", vtimer < 0 ? "SKIP" : "PASS",
               (uint32_t)vtimer_handler_rc, vtimer_setup_out, 1);
    const uint32_t vtimer_alarm_out[1] = {HLE_TIMER_VTIMER_DELAY_US};
    hle_record("timer-order-vtimer-alarm-set",
               vtimer_handler_rc < 0 || vtimer_start_rc < 0 ? "SKIP" : "PASS",
               (uint32_t)vtimer_alarm, vtimer_alarm_out, 1);
    hle_timer_order_wait("timer-order-wait-vtimer-alarm", HLE_TIMER_WAIT_LIMIT_US);
    hle_timer_order_event("timer-order-vtimer-event", &s_hle_vtimer_event,
                          vtimer_handler_rc >= 0 && vtimer_start_rc >= 0);
    hle_timer_order_event("timer-order-vtimer-alarm-event",
                          &s_hle_vtimer_alarm_event, vtimer_alarm >= 0);

    hle_step("timer-order-alarm-zero");
    const SceUID alarm_zero = sceKernelSetAlarm(0u, hle_alarm_zero,
                                                 &s_hle_alarm_zero);
    hle_timer_order_wait("timer-order-wait-zero", HLE_TIMER_ZERO_WAIT_US);
    const uint32_t zero_ran = s_hle_alarm_zero.sequence != 0u ? 1u : 0u;
    hle_record("alarm-zero-set", "PASS", (uint32_t)alarm_zero, &zero_ran, 1);
    hle_timer_order_event("alarm-zero-event", &s_hle_alarm_zero, alarm_zero >= 0);

    const uint32_t same_cleanup[2] = {
        hle_timer_order_cancel(alarm_same_a, "timer-order-cancel-same-a"),
        hle_timer_order_cancel(alarm_same_b, "timer-order-cancel-same-b"),
    };
    hle_record("alarm-same-delay-cleanup", "PASS", 0u, same_cleanup, 2);
    const uint32_t order_cleanup[2] = {
        hle_timer_order_cancel(alarm_long, "timer-order-cancel-long"),
        hle_timer_order_cancel(alarm_short, "timer-order-cancel-short"),
    };
    hle_record("alarm-long-short-cleanup", "PASS", 0u, order_cleanup, 2);
    uint32_t vtimer_cleanup[3] = {
        hle_timer_order_cancel(vtimer_alarm, "timer-order-cancel-vtimer-alarm"),
        HLE_TIMER_NO_TIME,
        HLE_TIMER_NO_TIME,
    };
    int cleanup_ok = 1;
    if (vtimer >= 0) {
        hle_step("timer-order-vtimer-stop");
        vtimer_cleanup[1] = (uint32_t)sceKernelStopVTimer(vtimer);
        hle_step("timer-order-vtimer-delete");
        vtimer_cleanup[2] = (uint32_t)sceKernelDeleteVTimer(vtimer);
        cleanup_ok = (int32_t)vtimer_cleanup[2] >= 0;
    }
    hle_record("timer-order-vtimer-cleanup", "PASS", 0u, vtimer_cleanup, 3);
    const uint32_t zero_cleanup[1] = {
        hle_timer_order_cancel(alarm_zero, "timer-order-cancel-zero"),
    };
    hle_record("alarm-zero-cleanup", "PASS", 0u, zero_cleanup, 1);
    return cleanup_ok;
}

#endif /* HLE_CASE_TIMER_ORDER */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_SAVEDATA_GETSIZE

/* #308, PR #1898: what savedata mode 22 (GETSIZE) answers when the named save does not exist,
   and whether it still fills in the free and shortfall fields. GETSIZE changes nothing on
   the Memory Stick: the save name is one no game owns, and the probe makes and deletes no
   save. No dialog shows and no button is pressed. Every cell here measures a save that does
   not exist, so the save directory must be absent before and after each dialog: a stat that
   does not return HLE_SD_SAVE_ABSENT makes COMPLETE report FAIL, and the host contract
   (validate_hle_savedata_getsize_absent) refuses the stream.

   The dialog must run in game context (standalone PSPLink after a soft reset). Launched under
   the XMB, InitStart returned 0 and the status stayed at INIT with an SceUtilityInit thread
   left behind (two launches, 2026-10-31), so volatile-lock-before records whether the memory
   the utility needs is free when the first dialog starts.

   Each dialog runs as in a game's frame loop, at most HLE_SD_POLL_VBLANKS polls one vblank
   apart: GetStatus; Update(1) when RUNNING; ShutdownStart once FINISHED; done when NONE
   follows an accepted ShutdownStart. A dialog not done by the bound is recorded with the
   TIMEOUT outcome and makes COMPLETE report FAIL. Dialog record words (result is InitStart's
   return):
     out0 the common result word (block + 0x1c, preset to HLE_SENTINEL) after the dialog
     out1 the GetStatus values, one nibble per change, the first change in the highest
          nibble (0-4 as returned, 0xE for any other value; the first eight changes)
     out2 the number of changes, out3 the number of GetStatus polls
     out4 the outcome: 0 NONE after an accepted ShutdownStart, 1 TIMEOUT, 2 InitStart
          refused (one GetStatus sample, then no Update or ShutdownStart)
     out5 the ShutdownStart return (HLE_SENTINEL when not called)
     out6 the last GetStatus return, out7 the number of Update calls
     out8 the bind word (block + 0x34, preset to HLE_SENTINEL)
     out9-out23 the 15 sizeInfo words: numSecure, numNormal, the secure and normal list
          pointers, sectorSize, freeSectors, freeKB, freeString[8], neededKB, neededString[8],
          overwriteKB, overwriteString[8]. All but the four inputs are preset to HLE_SENTINEL,
          so a field the utility leaves alone shows, and a NULL sizeInfo leaves all 15 at
          HLE_SENTINEL. A list pointer that still holds the probe's value reads as 1, so no
          probe address enters the record. */
#define HLE_SD_PARAM_BYTES 0x600u
#define HLE_SD_MODE_GETSIZE 22u
#define HLE_SD_STATUS_NONE 0
#define HLE_SD_STATUS_RUNNING 2
#define HLE_SD_STATUS_FINISHED 3
#define HLE_SD_POLL_VBLANKS 1200u
#define HLE_SD_SIZE_INFO_WORDS 15u
#define HLE_SD_SIZE_ENTRY_BYTES 24u
#define HLE_SD_SECURE_FITS_BYTES 0x2000ull
#define HLE_SD_SECURE_TOO_BIG_BYTES 0x1000000000ull
#define HLE_SD_DRIVER_WORDS 9u
#define HLE_SD_DIALOG_WORDS 24u
#define HLE_SD_SEQ_NIBBLES 8u
#define HLE_SD_OUTCOME_COMPLETE 0u
#define HLE_SD_OUTCOME_TIMEOUT 1u
#define HLE_SD_OUTCOME_INIT_REFUSED 2u
#define HLE_SD_SAVE_ABSENT 0x80010002u /* sceIoGetstat: no such file or directory */
#define HLE_SD_GAME_NAME "NKPROBE01"
#define HLE_SD_SAVE_NAME "GETSIZE"
#define HLE_SD_SAVE_DIR "ms0:/PSP/SAVEDATA/" HLE_SD_GAME_NAME HLE_SD_SAVE_NAME
_Static_assert(HLE_SD_DIALOG_WORDS == HLE_SD_DRIVER_WORDS + HLE_SD_SIZE_INFO_WORDS,
               "a dialog record is the driver words and the sizeInfo block");
_Static_assert(HLE_SD_DIALOG_WORDS <= HLE_MAX_WORDS, "a dialog record fits the writer");

static uint8_t s_hle_sd_param[HLE_SD_PARAM_BYTES] __attribute__((aligned(64)));
static uint32_t s_hle_sd_size_info[HLE_SD_SIZE_INFO_WORDS] __attribute__((aligned(64)));
static uint8_t s_hle_sd_entry[HLE_SD_SIZE_ENTRY_BYTES] __attribute__((aligned(64)));
static uint32_t s_hle_sd_timeouts;
static uint32_t s_hle_sd_not_absent;

static void hle_sd_put32(uint32_t offset, uint32_t value)
{
    memcpy(s_hle_sd_param + offset, &value, sizeof(value));
}

static uint32_t hle_sd_get32(uint32_t offset)
{
    uint32_t value;
    memcpy(&value, s_hle_sd_param + offset, sizeof(value));
    return value;
}

/* A NUL-terminated text into a zeroed field; every text here is shorter than its field. */
static void hle_sd_put_text(uint32_t offset, const char *text)
{
    memcpy(s_hle_sd_param + offset, text, strlen(text) + 1u);
}

/* The block one GETSIZE reads, at the offsets src/rt/savedata.c reads (its SDP_* enum). A
   field not set here is zero: no data buffer, no key. A non-zero `secure_bytes` lists one
   secure file of that size in sizeInfo; it is only a number the utility adds up, so 64 GiB
   writes nothing. With `with_size_info` 0 the pointer stays NULL. */
static void hle_sd_param(int with_size_info, uint64_t secure_bytes)
{
    memset(s_hle_sd_param, 0, sizeof(s_hle_sd_param));
    memset(s_hle_sd_entry, 0, sizeof(s_hle_sd_entry));
    hle_sd_put32(0x00u, HLE_SD_PARAM_BYTES); /* common size */
    hle_sd_put32(0x04u, 1u);                 /* language: English */
    hle_sd_put32(0x08u, 1u);                 /* buttonSwap: cross accepts */
    hle_sd_put32(0x0cu, 17u);                /* graphics thread priority */
    hle_sd_put32(0x10u, 19u);                /* access thread priority */
    hle_sd_put32(0x14u, 18u);                /* font thread priority */
    hle_sd_put32(0x18u, 16u);                /* sound thread priority */
    hle_sd_put32(0x1cu, HLE_SENTINEL);       /* common result: an unwritten word shows */
    hle_sd_put32(0x30u, HLE_SD_MODE_GETSIZE);
    hle_sd_put32(0x34u, HLE_SENTINEL);       /* bind: an unwritten word shows */
    hle_sd_put_text(0x3cu, HLE_SD_GAME_NAME);
    hle_sd_put_text(0x4cu, HLE_SD_SAVE_NAME);
    for (uint32_t i = 0; i < HLE_SD_SIZE_INFO_WORDS; i++) {
        s_hle_sd_size_info[i] = HLE_SENTINEL;
    }
    if (with_size_info) {
        s_hle_sd_size_info[0] = secure_bytes ? 1u : 0u;
        s_hle_sd_size_info[1] = 0u;
        s_hle_sd_size_info[2] = secure_bytes ? (uint32_t)(uintptr_t)s_hle_sd_entry : 0u;
        s_hle_sd_size_info[3] = 0u;
        if (secure_bytes) {
            memcpy(s_hle_sd_entry, &secure_bytes, sizeof(secure_bytes));
            memcpy(s_hle_sd_entry + 8u, "DATA.BIN", sizeof("DATA.BIN"));
        }
        hle_sd_put32(0x5fcu, (uint32_t)(uintptr_t)s_hle_sd_size_info);
    }
}

static uint32_t hle_sd_nibble(int status)
{
    return (status >= 0 && status <= 4) ? (uint32_t)status : 0xEu;
}

/* One dialog on the block hle_sd_param built; out1-out7 of the record. */
static int hle_sd_run(const char *step, uint32_t *words)
{
    int accepted = 0;
    int finish_shutdown = 0;
    words[5] = HLE_SENTINEL;
    hle_step(step);
    const int init = sceUtilitySavedataInitStart((SceUtilitySavedataParam *)(void *)s_hle_sd_param);
    uint32_t outcome = init >= 0 ? HLE_SD_OUTCOME_TIMEOUT : HLE_SD_OUTCOME_INIT_REFUSED;
    const uint32_t bound = init >= 0 ? HLE_SD_POLL_VBLANKS : 1u;
    int last = 0;
    for (uint32_t i = 0; i < bound; i++) {
        const int status = sceUtilitySavedataGetStatus();
        words[3]++;
        words[6] = (uint32_t)status;
        if (i == 0 || status != last) {
            if (words[2] < HLE_SD_SEQ_NIBBLES) {
                words[1] = (words[1] << 4) | hle_sd_nibble(status);
            }
            words[2]++;
            last = status;
        }
        if (init < 0) {
            break;
        }
        if (status == HLE_SD_STATUS_RUNNING) {
            sceUtilitySavedataUpdate(1);
            words[7]++;
        } else if (status == HLE_SD_STATUS_FINISHED && !finish_shutdown) {
            const int rc = sceUtilitySavedataShutdownStart();
            words[5] = (uint32_t)rc;
            finish_shutdown = 1;
            accepted = rc >= 0;
        } else if (status == HLE_SD_STATUS_NONE && accepted) {
            outcome = HLE_SD_OUTCOME_COMPLETE;
            break;
        }
        (void)sceDisplayWaitVblankStart();
    }
    if (outcome == HLE_SD_OUTCOME_TIMEOUT) {
        s_hle_sd_timeouts++;
    }
    words[4] = outcome;
    return init;
}

static void hle_sd_dialog(const char *case_id, int with_size_info, uint64_t secure_bytes)
{
    uint32_t out[HLE_SD_DIALOG_WORDS];
    memset(out, 0, sizeof(out));
    hle_sd_param(with_size_info, secure_bytes);
    const int init = hle_sd_run(case_id, out);
    out[0] = hle_sd_get32(0x1cu);
    out[8] = hle_sd_get32(0x34u);
    for (uint32_t i = 0; i < HLE_SD_SIZE_INFO_WORDS; i++) {
        out[HLE_SD_DRIVER_WORDS + i] = s_hle_sd_size_info[i];
    }
    if (secure_bytes && s_hle_sd_size_info[2] == (uint32_t)(uintptr_t)s_hle_sd_entry) {
        out[HLE_SD_DRIVER_WORDS + 2] = 1u;
    }
    hle_record(case_id, "PASS", (uint32_t)init, out, HLE_SD_DIALOG_WORDS);
}

/* The save's directory: the result is sceIoGetstat's return. Any other than
   HLE_SD_SAVE_ABSENT voids the run, whichever cell saw it. */
static void hle_sd_absent(const char *case_id)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    hle_step(case_id);
    const int rc = sceIoGetstat(HLE_SD_SAVE_DIR, &st);
    if ((uint32_t)rc != HLE_SD_SAVE_ABSENT) {
        s_hle_sd_not_absent++;
    }
    const uint32_t out[1] = {(uint32_t)st.st_mode};
    hle_record(case_id, "PASS", (uint32_t)rc, out, 1);
}

/* TryLock of volatile memory, released at once when it succeeds. out0 is the Unlock return,
   or HLE_SENTINEL when the lock was not obtained; out1 the reported size. */
static void hle_sd_volatile_lock(const char *case_id)
{
    void *ptr = NULL;
    int size = 0;
    hle_step(case_id);
    const int rc = sceKernelVolatileMemTryLock(0, &ptr, &size);
    const uint32_t out[2] = {
        rc == 0 ? (uint32_t)sceKernelVolatileMemUnlock(0) : HLE_SENTINEL,
        (uint32_t)size,
    };
    hle_record(case_id, "PASS", (uint32_t)rc, out, 2);
}

static int hle_run(void)
{
    hle_sd_absent("save-absent-before");
    hle_sd_volatile_lock("volatile-lock-before");
    hle_sd_dialog("getsize-missing-secure", 1, HLE_SD_SECURE_FITS_BYTES);
    hle_sd_absent("save-absent-after-secure");
    hle_sd_dialog("getsize-missing-empty", 1, 0u);
    hle_sd_absent("save-absent-after-empty");
    hle_sd_dialog("getsize-missing-shortfall", 1, HLE_SD_SECURE_TOO_BIG_BYTES);
    hle_sd_absent("save-absent-after-shortfall");
    /* Last: a NULL out pointer can fault in kernel mode, and the cells above are on host0. */
    hle_sd_dialog("getsize-missing-null", 0, 0u);
    hle_sd_absent("save-absent-after-null");
    return s_hle_sd_timeouts == 0u && s_hle_sd_not_absent == 0u;
}

#endif /* HLE_CASE_SAVEDATA_GETSIZE */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_UTILITY_NONE

/* #1386: what each utility dialog's Update, ShutdownStart and GetStatus answer when no dialog
   of any type has been started (status NONE), and what a second ShutdownStart answers. The
   build imports no InitStart call at all (hle_utility_imports.S declares only GetStatus,
   Update and ShutdownStart for this case and the Makefile pins the set), so no dialog can open
   and nothing is drawn, installed, sent or saved. Savedata and the message dialog come first:
   pspautotests measured them (utility/dialog/status.expected), so they show whether the
   context answers like a game. Netconf, gamedata install and game sharing are the unmeasured
   types. Each row's words: GetStatus, Update(1), GetStatus, ShutdownStart, GetStatus,
   ShutdownStart again, GetStatus. The imports are named nkUtil* (hle_utility_imports.S):
   PSPSDK's headers declare some of these calls as returning void, but each returns a status
   word, which is what this case records. */

typedef struct HleUtilNone {
    const char *case_id;
    int (*get_status)(void);
    int (*update)(int);
    int (*shutdown)(void);
} HleUtilNone;

int nkUtilSavedataGetStatus(void);
int nkUtilSavedataUpdate(int);
int nkUtilSavedataShutdownStart(void);
int nkUtilMsgDialogGetStatus(void);
int nkUtilMsgDialogUpdate(int);
int nkUtilMsgDialogShutdownStart(void);
int nkUtilNetconfGetStatus(void);
int nkUtilNetconfUpdate(int);
int nkUtilNetconfShutdownStart(void);
int nkUtilGamedataInstallGetStatus(void);
int nkUtilGamedataInstallUpdate(int);
int nkUtilGamedataInstallShutdownStart(void);
int nkUtilGameSharingGetStatus(void);
int nkUtilGameSharingUpdate(int);
int nkUtilGameSharingShutdownStart(void);

static const HleUtilNone k_hle_util_none[] = {
    {"utility-none-savedata", nkUtilSavedataGetStatus, nkUtilSavedataUpdate,
     nkUtilSavedataShutdownStart},
    {"utility-none-msgdialog", nkUtilMsgDialogGetStatus, nkUtilMsgDialogUpdate,
     nkUtilMsgDialogShutdownStart},
    {"utility-none-netconf", nkUtilNetconfGetStatus, nkUtilNetconfUpdate,
     nkUtilNetconfShutdownStart},
    {"utility-none-gamedata", nkUtilGamedataInstallGetStatus,
     nkUtilGamedataInstallUpdate, nkUtilGamedataInstallShutdownStart},
    {"utility-none-sharing", nkUtilGameSharingGetStatus, nkUtilGameSharingUpdate,
     nkUtilGameSharingShutdownStart},
};

static void hle_util_none(const HleUtilNone *d)
{
    uint32_t out[7];
    hle_step(d->case_id);
    out[0] = (uint32_t)d->get_status();
    out[1] = (uint32_t)d->update(1);
    out[2] = (uint32_t)d->get_status();
    out[3] = (uint32_t)d->shutdown();
    out[4] = (uint32_t)d->get_status();
    out[5] = (uint32_t)d->shutdown();
    out[6] = (uint32_t)d->get_status();
    hle_record(d->case_id, "PASS", out[0], out, 7);
}

static int hle_run(void)
{
    for (unsigned i = 0; i < sizeof(k_hle_util_none) / sizeof(k_hle_util_none[0]); i++) {
        hle_util_none(&k_hle_util_none[i]);
    }
    return 1;
}

#endif /* HLE_CASE_UTILITY_NONE */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_STD_HANDLES

/* #341 (W-HLE-2, part 1): the three standard handles in the probe's launch context, a
   one-byte write to Stdout and to Stderr, and a write to Stdout after closing it. The runtime
   answers 1 for all three handles today (h_StdFd); this case measures what the console does.

   Cells, in order:
   - stdio-handles (3 words): sceKernelStdin, sceKernelStdout, sceKernelStderr. result: Stdout.
   - stdio-write-stdout, stdio-write-stderr (2 words): the handle written to and, again, the
     handle read after the write. result: what sceIoWrite(handle, "x", 1) returned.
   - stdio-close-stdout (3 words): the write to the closed handle, sceKernelStdout read after
     the close, and the handle that was closed. result: what sceIoClose returned.

   The family log goes to host0 only, so closing Stdout cannot cut a record short, and the
   close comes last so every earlier cell runs on an open handle. Under PSPLink the handles are
   its tty; the close lasts until the next boot, and every launch here gets a fresh one.

   Not run: sceIoRead on Stdin. On PSPLink's tty a one-byte read waits for input that nothing
   sends, and a thread blocked in a device read when the runner stops the module risks a hang
   that needs a power cycle. That cell needs a design that cannot block (#341). */

static const char s_hle_std_byte[1] = {'x'};

static int hle_run(void)
{
    uint32_t handles[3];
    hle_step("stdio-handles");
    handles[0] = (uint32_t)sceKernelStdin();
    handles[1] = (uint32_t)sceKernelStdout();
    handles[2] = (uint32_t)sceKernelStderr();
    hle_record("stdio-handles", "PASS", handles[1], handles, 3);

    uint32_t out_wrote[2];
    hle_step("stdio-write-stdout");
    const int out_rc = sceIoWrite((SceUID)handles[1], s_hle_std_byte, 1);
    out_wrote[0] = handles[1];
    hle_step("stdio-stdout-reread");
    out_wrote[1] = (uint32_t)sceKernelStdout();
    hle_record("stdio-write-stdout", "PASS", (uint32_t)out_rc, out_wrote, 2);

    uint32_t err_wrote[2];
    hle_step("stdio-write-stderr");
    const int err_rc = sceIoWrite((SceUID)handles[2], s_hle_std_byte, 1);
    err_wrote[0] = handles[2];
    hle_step("stdio-stderr-reread");
    err_wrote[1] = (uint32_t)sceKernelStderr();
    hle_record("stdio-write-stderr", "PASS", (uint32_t)err_rc, err_wrote, 2);

    uint32_t closed[3];
    hle_step("stdio-close-stdout");
    const int close_rc = sceIoClose((SceUID)handles[1]);
    hle_step("stdio-write-closed");
    closed[0] = (uint32_t)sceIoWrite((SceUID)handles[1], s_hle_std_byte, 1);
    hle_step("stdio-handle-after-close");
    closed[1] = (uint32_t)sceKernelStdout();
    closed[2] = handles[1];
    hle_record("stdio-close-stdout", "PASS", (uint32_t)close_rc, closed, 3);
    return 1;
}

#endif /* HLE_CASE_STD_HANDLES */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_AUDIO_REST

/* #279 (W-MEDIA-2): how much a regular audio channel still holds when sceAudioOutputBlocking
   returns, in steady state. The runtime's release of a blocking output (#67) lets a caller run
   two blocks ahead; this case measures what the console does.

   For each channel size F of 1024, 2048 and 4096 frames, one stereo channel is reserved with
   sceAudioChReserve (channel -1, F frames, PSP_AUDIO_FORMAT_STEREO) and fed HLE_AREST_BLOCKS
   blocking outputs of the same F-frame buffer at volume 0x8000. Right after each return the probe reads
   sceAudioGetChannelRestLen and sceAudioGetChannelRestLength and the system time. Nothing is
   written to host0 between the outputs (a host0 write can take longer than a block and would
   starve the channel): the values are kept in arrays and the records follow the run. After the
   last output the probe polls sceAudioGetChannelRestLen every 1 ms, at most HLE_AREST_DRAIN_POLLS
   times, until it reads 0, then releases the channel.

   The buffer is a project-written ramp of amplitude 64 (of 32767), quiet on the speaker; no
   game data.

   Cells, per F (`audio-rest-<F>-...`):
   - `-reserve` (3 words): the channel, F, the format. result: the channel.
   - `-bNN`, NN = 00..31 (5 words): the time the call took (us), the time since the previous
     return (us; for b00 the time since the reserve), RestLen and RestLength after the return,
     the channel. result: what sceAudioOutputBlocking returned.
   - `-drain` (4 words): the polls taken, RestLen at the last poll, the time from the last return
     to that poll (us), what sceAudioChRelease returned. result: the release's return.
   A reserve that fails gives SKIP for that F's other cells; nothing else is called. */

#define HLE_AREST_BLOCKS 32u
#define HLE_AREST_MAX_FRAMES 4096u
#define HLE_AREST_AMPLITUDE 64
#define HLE_AREST_DRAIN_POLLS 500u
#define HLE_AREST_DRAIN_POLL_US 1000u

/* Every record name, literal, in record order: per F, the reserve, b00..b31 and the drain. */
static const char *const k_hle_arest_names[3][HLE_AREST_BLOCKS + 2u] = {
    {"audio-rest-1024-reserve", "audio-rest-1024-b00", "audio-rest-1024-b01",
     "audio-rest-1024-b02", "audio-rest-1024-b03", "audio-rest-1024-b04", "audio-rest-1024-b05",
     "audio-rest-1024-b06", "audio-rest-1024-b07", "audio-rest-1024-b08", "audio-rest-1024-b09",
     "audio-rest-1024-b10", "audio-rest-1024-b11", "audio-rest-1024-b12", "audio-rest-1024-b13",
     "audio-rest-1024-b14", "audio-rest-1024-b15", "audio-rest-1024-b16", "audio-rest-1024-b17",
     "audio-rest-1024-b18", "audio-rest-1024-b19", "audio-rest-1024-b20", "audio-rest-1024-b21",
     "audio-rest-1024-b22", "audio-rest-1024-b23", "audio-rest-1024-b24", "audio-rest-1024-b25",
     "audio-rest-1024-b26", "audio-rest-1024-b27", "audio-rest-1024-b28", "audio-rest-1024-b29",
     "audio-rest-1024-b30", "audio-rest-1024-b31", "audio-rest-1024-drain"},
    {"audio-rest-2048-reserve", "audio-rest-2048-b00", "audio-rest-2048-b01",
     "audio-rest-2048-b02", "audio-rest-2048-b03", "audio-rest-2048-b04", "audio-rest-2048-b05",
     "audio-rest-2048-b06", "audio-rest-2048-b07", "audio-rest-2048-b08", "audio-rest-2048-b09",
     "audio-rest-2048-b10", "audio-rest-2048-b11", "audio-rest-2048-b12", "audio-rest-2048-b13",
     "audio-rest-2048-b14", "audio-rest-2048-b15", "audio-rest-2048-b16", "audio-rest-2048-b17",
     "audio-rest-2048-b18", "audio-rest-2048-b19", "audio-rest-2048-b20", "audio-rest-2048-b21",
     "audio-rest-2048-b22", "audio-rest-2048-b23", "audio-rest-2048-b24", "audio-rest-2048-b25",
     "audio-rest-2048-b26", "audio-rest-2048-b27", "audio-rest-2048-b28", "audio-rest-2048-b29",
     "audio-rest-2048-b30", "audio-rest-2048-b31", "audio-rest-2048-drain"},
    {"audio-rest-4096-reserve", "audio-rest-4096-b00", "audio-rest-4096-b01",
     "audio-rest-4096-b02", "audio-rest-4096-b03", "audio-rest-4096-b04", "audio-rest-4096-b05",
     "audio-rest-4096-b06", "audio-rest-4096-b07", "audio-rest-4096-b08", "audio-rest-4096-b09",
     "audio-rest-4096-b10", "audio-rest-4096-b11", "audio-rest-4096-b12", "audio-rest-4096-b13",
     "audio-rest-4096-b14", "audio-rest-4096-b15", "audio-rest-4096-b16", "audio-rest-4096-b17",
     "audio-rest-4096-b18", "audio-rest-4096-b19", "audio-rest-4096-b20", "audio-rest-4096-b21",
     "audio-rest-4096-b22", "audio-rest-4096-b23", "audio-rest-4096-b24", "audio-rest-4096-b25",
     "audio-rest-4096-b26", "audio-rest-4096-b27", "audio-rest-4096-b28", "audio-rest-4096-b29",
     "audio-rest-4096-b30", "audio-rest-4096-b31", "audio-rest-4096-drain"},
};

static int16_t s_hle_arest_ramp[HLE_AREST_MAX_FRAMES * 2u] __attribute__((aligned(64)));
static uint32_t s_hle_arest_rc[HLE_AREST_BLOCKS];
static uint32_t s_hle_arest_call_us[HLE_AREST_BLOCKS];
static uint32_t s_hle_arest_gap_us[HLE_AREST_BLOCKS];
static uint32_t s_hle_arest_len[HLE_AREST_BLOCKS];
static uint32_t s_hle_arest_length[HLE_AREST_BLOCKS];

/* Written through the uncached mirror (address | 0x40000000), so the audio DMA reads it
   without a cache write-back. */
static void hle_arest_fill(void)
{
    volatile int16_t *ramp =
        (volatile int16_t *)((uintptr_t)s_hle_arest_ramp | 0x40000000u);
    for (uint32_t i = 0; i < HLE_AREST_MAX_FRAMES; i++) {
        const int16_t v = (int16_t)((int)(i % 128u) - HLE_AREST_AMPLITUDE);
        ramp[2u * i] = v;
        ramp[2u * i + 1u] = v;
    }
}

static void hle_arest_size(uint32_t idx, uint32_t frames)
{
    const char *const *names = k_hle_arest_names[idx];
    static char step[48];
    snprintf(step, sizeof(step), "audio-rest-%u-run", (unsigned int)frames);
    hle_step(step);
    const int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, (int)frames, PSP_AUDIO_FORMAT_STEREO);
    uint32_t prev = sceKernelGetSystemTimeLow();
    const uint32_t reserve[3] = {(uint32_t)ch, frames, (uint32_t)PSP_AUDIO_FORMAT_STEREO};
    if (ch >= 0) {
        for (uint32_t b = 0; b < HLE_AREST_BLOCKS; b++) {
            const uint32_t start = sceKernelGetSystemTimeLow();
            const int rc = sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, s_hle_arest_ramp);
            const uint32_t end = sceKernelGetSystemTimeLow();
            s_hle_arest_len[b] = (uint32_t)sceAudioGetChannelRestLen(ch);
            s_hle_arest_length[b] = (uint32_t)sceAudioGetChannelRestLength(ch);
            s_hle_arest_rc[b] = (uint32_t)rc;
            s_hle_arest_call_us[b] = end - start;
            s_hle_arest_gap_us[b] = end - prev;
            prev = end;
        }
    }
    uint32_t polls = 0;
    int rest = 0;
    uint32_t drain_us = 0;
    int release_rc = 0;
    if (ch >= 0) {
        rest = sceAudioGetChannelRestLen(ch);
        for (; rest > 0 && polls < HLE_AREST_DRAIN_POLLS; polls++) {
            sceKernelDelayThread(HLE_AREST_DRAIN_POLL_US);
            rest = sceAudioGetChannelRestLen(ch);
        }
        drain_us = sceKernelGetSystemTimeLow() - prev;
        release_rc = sceAudioChRelease(ch);
    }

    hle_record(names[0], ch < 0 ? "FAIL" : "PASS", (uint32_t)ch, reserve, 3);
    for (uint32_t b = 0; b < HLE_AREST_BLOCKS; b++) {
        if (ch < 0) {
            hle_skip(names[1u + b], 5);
            continue;
        }
        const uint32_t out[5] = {s_hle_arest_call_us[b], s_hle_arest_gap_us[b], s_hle_arest_len[b],
                                 s_hle_arest_length[b], (uint32_t)ch};
        hle_record(names[1u + b], "PASS", s_hle_arest_rc[b], out, 5);
    }
    if (ch < 0) {
        hle_skip(names[1u + HLE_AREST_BLOCKS], 4);
        return;
    }
    const uint32_t drain[4] = {polls, (uint32_t)rest, drain_us, (uint32_t)release_rc};
    hle_record(names[1u + HLE_AREST_BLOCKS], "PASS", (uint32_t)release_rc, drain, 4);
}

static int hle_run(void)
{
    hle_arest_fill();
    hle_arest_size(0u, 1024u);
    hle_arest_size(1u, 2048u);
    hle_arest_size(2u, 4096u);
    return 1;
}

#endif /* HLE_CASE_AUDIO_REST */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_UTILITY_DIALOGS

/* #1386 (W-HLE-1): utility dialog status timing after an InitStart, the wrong-state answers of
   netconf, the gamedata install dialog and game sharing, and the common result word a cancel
   leaves. The runtime model is utility_slot_settle_locked and the dialog state machine in
   src/rt/hle_utility.c: start-up and shutdown complete on the first vblank after the call, and
   netconf, gamedata and sharing take INVALID_STATUS (0x80110001) at NONE and during SHUTDOWN.
   This case measures what the console does. It must run in game context (standalone PSPLink
   after a soft reset): under the XMB a dialog stays at INIT (PSP-HLE-UTILITY-NONE-001 note).

   Safety rules, all kept in the code below:
   - Order: savedata (mode 22 GETSIZE, which writes nothing), the gamedata install dialog, then
     netconf, then game sharing last, so a hang in a radio dialog cannot lose an earlier record.
     Every record is written as soon as its cell ends.
   - Nothing presses a button. A dialog only ever gets GetStatus, Update(1) and ShutdownStart,
     and the matrix calls below. A run still RUNNING after HLE_UD_CANCEL_POLLS polls is
     cancelled with ShutdownStart; one still at INIT after HLE_UD_INIT_CANCEL_POLLS polls gets one
     ShutdownStart too. Every wait is a vblank-counted bound.
   - No network is joined, created or scanned for, and no setting or profile is written: netconf
     runs action 0 (connect to an access point) with hotspot 0 and wifisp 0 and is never answered,
     so it can only be cancelled. The only net calls are the module load/unload pair and each net
     library's Init and Term (the Makefile pins every net library to exactly that pair). No apctl
     connect, ad hoc connect, create or join, scan or registry call is made or linked.
   - Game sharing shares HLE_UD_GS_DATA_BYTES of project-written pattern bytes in memory mode
     (no file, no game data) under a synthetic ad hoc product code, and is cancelled like netconf.
   - The gamedata install dialog (mode 0, no prompt) reads disc0:/PSP_GAME/INSDIR and writes under
     HLE_UD_GD_DIR. Its cells run only when sceUmdCheckMedium reports no disc (so nothing on a
     disc can be copied) and the directory is absent (Dopen fails and Getstat answers
     0x80010002), so the directory cannot be anyone else's. After the gamedata cells the probe
     removes each regular file in that directory, removes the directory, and checks that a Dopen
     of it fails; nothing else on ms0: is touched. The savedata runs use the GETSIZE block of case
     104 (save NKPROBE01GETSIZE, which no game owns): the utility reads the Memory Stick's free
     space and finds no save; it writes nothing.
   - A durable step marker precedes each call that could hang, except inside the per-vblank poll
     loops (a host0 write there would hide vblanks) and inside the INIT and SHUTDOWN bursts (the
     state would move on while the marker is written); those are named by the marker before the
     loop or burst. A ShutdownStart issued by a poll loop gets its own marker first, and its
     vblank is read after the marker, so the marker cannot lengthen a measured shutdown.
   - Each dialog is back at NONE before the next starts. A run that is not back at NONE by its
     bound is recorded as stuck, makes COMPLETE report FAIL, and every later dialog cell (and net
     setup) is SKIP with reason HLE_UD_SKIP_STUCK; teardown and cleanup still run.

   Left out: every message dialog run. The message dialog refuses ShutdownStart during INIT and
   RUNNING (pspautotests utility/dialog/status.expected, as hle_utility.c cites it), so once open it
   leaves RUNNING only through a button press or its Abort call, neither of which this case may
   make; an opened message dialog would hold the slot and stop every later cell. The message
   dialog's three INIT/SHUTDOWN timing runs and its wrong-state row are therefore not measured.
   The only message dialog call is the "InitStart while another dialog is active" cell, made with
   a block whose size word is 0, so no message dialog can open: the busy check comes before the
   size check (status.expected), so a busy slot still answers INVALID_STATUS, while a size check
   that came first would answer 0x80110004. An accepted one is recorded as stuck.

   Records, in order (a SKIP record keeps its word count, all words 0, and carries the reason in
   result: 1 a UMD is present, 2 the gamedata directory exists, 3 sceUmdCheckMedium failed, 4 the
   directory's Getstat did not answer 0x80010002, 5 an earlier dialog is stuck, 6 the net setup
   failed, 7 the gamedata cells did not run):
   - savedata-timing-r1..r3, then <t>-timing for t = gamedata, netconf, sharing (24 words; result
     InitStart's return). One run: InitStart, then each poll one vblank apart (GetStatus;
     Update(1) while RUNNING; ShutdownStart once at FINISHED or at a cancel bound) until NONE.
       out0 InitStart's return
       out1 outcome: 0 NONE after an accepted ShutdownStart, 1 stuck at the bound, 2 InitStart
            refused (one GetStatus sample), 3 at NONE with no accepted ShutdownStart (after the
            dialog was active, or for HLE_UD_CANCEL_POLLS polls), 4 GetStatus answered a negative value (the loop stops there)
       out2 polls, out3 Update(1) calls
       out4 the first poll-loop ShutdownStart's return, out5 its vblank, out6 the status it was
            issued in (all three HLE_SENTINEL when none was issued), out7 ShutdownStart calls
       out8 the common result word (block + 0x1c, preset HLE_SENTINEL) after the run
       out9 the type's own output word, preset HLE_SENTINEL except netconf's: savedata bind
            (+0x34), gamedata progress (+0x55c), netconf hotspot_connected (+0x3c, preset 0 as an
            output a caller clears), sharing result (+0x4c)
       out10 the last GetStatus value, out11 the last poll's vblank, out12 status changes
       out13-out23 the first 11 changes, each (vblank << 8) | status (0-4, 0xFF for any other
            value), unused slots HLE_SENTINEL. A vblank is sceDisplayGetVcount() minus its value
            just before InitStart; the first poll follows InitStart without a wait.
   - <t>-state-<s> for t = gamedata, netconf, sharing and s = init, running, finished, shutdown
     (21 words; result the entry InitStart's return). Each state uses a fresh dialog. init: the
     calls follow InitStart at once. running: polls (no Update) until RUNNING. finished: polls,
     Update(1) while RUNNING, until FINISHED or HLE_UD_CANCEL_POLLS RUNNING polls (then not
     reached). shutdown: polls until RUNNING (or FINISHED), then the entry ShutdownStart. When
     the status read just before the calls is the state, the calls run back to back: Update(1),
     a second InitStart of the same type (its own block), a message dialog InitStart (size-0
     block), ShutdownStart, ShutdownStart again, each followed by GetStatus. Then the dialog is
     driven to NONE as in a timing run.
       out0 the entry InitStart's return
       out1 1 the state was reached and the calls ran, 0 not reached (out4-out13 HLE_SENTINEL),
            2 InitStart refused
       out2 the vblank of the poll that found the state (for shutdown, the poll that found
            RUNNING or FINISHED before the entry ShutdownStart; HLE_SENTINEL when not reached)
       out3 GetStatus just before the calls
       out4 Update(1), out5 GetStatus
       out6 the second InitStart, out7 GetStatus
       out8 the message dialog InitStart, out9 GetStatus (of this type)
       out10 ShutdownStart, out11 GetStatus
       out12 ShutdownStart again, out13 GetStatus
       out14 the entry ShutdownStart (shutdown only; HLE_SENTINEL otherwise)
       out15 the drive-out outcome (as a timing run's out1), out16 its ShutdownStart calls
       out17 the last poll's vblank, out18 the last GetStatus value
       out19 the common result word of the entry block, out20 that of the second block (both
             preset HLE_SENTINEL), after the drive-out
     The common result word after a cancel during RUNNING (#1386 item 4) is a timing run's out8
     when its out6 is 2, and the running cell's out19 when its ShutdownStart was accepted.
   - gamedata-precheck (5 words; result the reason, 0 to run): sceUmdCheckMedium, the Dopen of
     HLE_UD_GD_DIR, its Dclose (HLE_SENTINEL when not opened), the directory's Getstat, the reason.
   - gamedata-cleanup (23 words; result the final Dopen's return; FAIL when that Dopen
     succeeds): the Dopen, the entries read (not . and ..), the regular files kept for removal
     (at most HLE_UD_CLEAN_FILES), the entries not removed, the Dclose, the Rmdir, the final Dopen,
     then the HLE_UD_CLEAN_FILES remove returns and HLE_UD_CLEAN_FILES file sizes (low word),
     HLE_SENTINEL where unused. SKIP with reason 7 when the gamedata cells did not run.
   - netconf-setup and sharing-setup (5 words; result 0, or the failing call's return): the
     returns of LoadNetModule of COMMON, LoadNetModule of INET (netconf) or ADHOC (sharing),
     NetInit, then for netconf InetInit and ApctlInit, for sharing AdhocInit and AdhocctlInit
     (with the synthetic product code HLE_UD_PRODUCT). A failure stops the setup (later words HLE_SENTINEL) and the dialog cells are
     SKIP with reason 6.
   - netconf-teardown and sharing-teardown (5 words; result the number of setup calls undone):
     the Term calls and unloads in reverse order (ApctlTerm, InetTerm, NetTerm, unload INET,
     unload COMMON; AdhocctlTerm, AdhocTerm, NetTerm, unload ADHOC, unload COMMON), each made
     only when its setup call succeeded (HLE_SENTINEL otherwise). They run on every path that
     ran a setup. */

#include <pspumd.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>

#define HLE_UD_STATUS_NONE 0
#define HLE_UD_STATUS_INIT 1
#define HLE_UD_STATUS_RUNNING 2
#define HLE_UD_STATUS_FINISHED 3
#define HLE_UD_STATUS_SHUTDOWN 4
#define HLE_UD_POLL_LIMIT 600u
#define HLE_UD_REACH_POLLS 300u
#define HLE_UD_CANCEL_POLLS 60u
#define HLE_UD_INIT_CANCEL_POLLS 300u
#define HLE_UD_RUN_SHUTDOWNS 2u
#define HLE_UD_TIMING_WORDS 24u
#define HLE_UD_TIMING_HEAD 13u
#define HLE_UD_CHANGE_SLOTS 11u
#define HLE_UD_STATE_WORDS 21u
#define HLE_UD_PRECHECK_WORDS 5u
#define HLE_UD_CLEANUP_WORDS 23u
#define HLE_UD_CLEAN_FILES 8u
#define HLE_UD_CLEAN_HEAD 7u
#define HLE_UD_NAME_BYTES 64u
#define HLE_UD_DIR_READS 40u
#define HLE_UD_NET_WORDS 5u
#define HLE_UD_NET_STEPS 5u
#define HLE_UD_OUT_NONE 0u
#define HLE_UD_OUT_STUCK 1u
#define HLE_UD_OUT_REFUSED 2u
#define HLE_UD_OUT_SELF_NONE 3u
#define HLE_UD_OUT_STATUS_ERROR 4u
#define HLE_UD_SKIP_UMD_PRESENT 1u
#define HLE_UD_SKIP_DIR_EXISTS 2u
#define HLE_UD_SKIP_UMD_UNKNOWN 3u
#define HLE_UD_SKIP_DIR_UNCLEAR 4u
#define HLE_UD_SKIP_STUCK 5u
#define HLE_UD_SKIP_NET_SETUP 6u
#define HLE_UD_SKIP_NOT_RUN 7u
#define HLE_UD_PATH_ABSENT 0x80010002u /* sceIoGetstat: no such file or directory */
/* Block sizes the runtime accepts (utility_param_size_ok lists in hle_utility.c) and the PSPSDK
   and PPSSPP layouts give: savedata 0x600, netconf 0x44 (pspUtilityNetconfData), the gamedata
   install block 0x598 (PPSSPP SceUtilityGamedataInstallParam), sharing 0x64
   (pspUtilityGameSharingParams). The message dialog block is the 0x2c4-byte V3 size, all zero. */
#define HLE_UD_SD_BYTES 0x600u
#define HLE_UD_NC_BYTES 0x44u
#define HLE_UD_GD_BYTES 0x598u
#define HLE_UD_GS_BYTES 0x64u
#define HLE_UD_MSG_BYTES 0x2c4u
#define HLE_UD_SD_SIZE_INFO_WORDS 15u
#define HLE_UD_GS_DATA_BYTES 256u
#define HLE_UD_SD_GAME_NAME "NKPROBE01"
#define HLE_UD_SD_SAVE_NAME "GETSIZE"
#define HLE_UD_GD_GAME_NAME "NKORACLE0"
#define HLE_UD_GD_DATA_NAME "GDI1"
#define HLE_UD_GD_DIR "ms0:/PSP/SAVEDATA/" HLE_UD_GD_GAME_NAME HLE_UD_GD_DATA_NAME
#define HLE_UD_GS_NAME "NKORACL"
#define HLE_UD_PRODUCT "NKOR00000"
_Static_assert(HLE_UD_TIMING_HEAD + HLE_UD_CHANGE_SLOTS == HLE_UD_TIMING_WORDS,
               "a timing record is its head and its change slots");
_Static_assert(HLE_UD_CLEAN_HEAD + 2u * HLE_UD_CLEAN_FILES == HLE_UD_CLEANUP_WORDS,
               "a cleanup record is its head, the removes and the sizes");
_Static_assert(HLE_UD_TIMING_WORDS <= HLE_MAX_WORDS, "a timing record fits the writer");

int nkUtilSavedataInitStart(void *param);
int nkUtilSavedataGetStatus(void);
int nkUtilSavedataUpdate(int speed);
int nkUtilSavedataShutdownStart(void);
int nkUtilMsgDialogInitStart(void *param);
int nkUtilNetconfInitStart(void *param);
int nkUtilNetconfGetStatus(void);
int nkUtilNetconfUpdate(int speed);
int nkUtilNetconfShutdownStart(void);
int nkUtilGamedataInstallInitStart(void *param);
int nkUtilGamedataInstallGetStatus(void);
int nkUtilGamedataInstallUpdate(int speed);
int nkUtilGamedataInstallShutdownStart(void);
int nkUtilGameSharingInitStart(void *param);
int nkUtilGameSharingGetStatus(void);
int nkUtilGameSharingUpdate(int speed);
int nkUtilGameSharingShutdownStart(void);

typedef struct HleUdType {
    const char *tag;
    int (*init)(void *param);
    int (*get_status)(void);
    int (*update)(int speed);
    int (*shutdown)(void);
    void (*fill)(uint8_t *block);
    uint8_t *first;
    uint8_t *second;
    uint32_t output_offset;
} HleUdType;

typedef struct HleUdRun {
    const HleUdType *t;
    uint32_t v0;
    int init_rc;
    int last;
    uint32_t polls;
    uint32_t updates;
    uint32_t changes;
    uint32_t change[HLE_UD_CHANGE_SLOTS];
    uint32_t sd_rc;
    uint32_t sd_at;
    uint32_t sd_status;
    uint32_t sd_calls;
    uint32_t end_at;
    uint32_t outcome;
    int accepted;
    int active;
    int finish_called;
    int init_cancel_called;
} HleUdRun;

static uint8_t s_ud_sd[HLE_UD_SD_BYTES] __attribute__((aligned(64)));
static uint32_t s_ud_sd_size_info[HLE_UD_SD_SIZE_INFO_WORDS] __attribute__((aligned(64)));
static uint8_t s_ud_nc[2][HLE_UD_NC_BYTES] __attribute__((aligned(64)));
static uint8_t s_ud_nc_adhoc[16] __attribute__((aligned(64)));
static uint8_t s_ud_gd[2][HLE_UD_GD_BYTES] __attribute__((aligned(64)));
static uint8_t s_ud_gs[2][HLE_UD_GS_BYTES] __attribute__((aligned(64)));
static uint8_t s_ud_gs_data[HLE_UD_GS_DATA_BYTES] __attribute__((aligned(64)));
static uint8_t s_ud_msg[HLE_UD_MSG_BYTES] __attribute__((aligned(64)));
static struct productStruct s_ud_product;
static SceIoDirent s_ud_dirent;
static char s_ud_names[HLE_UD_CLEAN_FILES][HLE_UD_NAME_BYTES];
static char s_ud_path[sizeof(HLE_UD_GD_DIR) + 1u + HLE_UD_NAME_BYTES];
_Static_assert(sizeof(s_ud_path) >= sizeof(HLE_UD_GD_DIR) + HLE_UD_NAME_BYTES,
               "a file path is the directory, a slash, a name of at most 63 bytes and a NUL");
static uint32_t s_ud_stuck;
static uint32_t s_ud_fail;

static void hle_ud_put32(uint8_t *block, uint32_t offset, uint32_t value)
{
    memcpy(block + offset, &value, sizeof(value));
}

static uint32_t hle_ud_get32(const uint8_t *block, uint32_t offset)
{
    uint32_t value;
    memcpy(&value, block + offset, sizeof(value));
    return value;
}

/* A NUL-terminated text into a zeroed field; every text here is shorter than its field. */
static void hle_ud_put_text(uint8_t *block, uint32_t offset, const char *text)
{
    memcpy(block + offset, text, strlen(text) + 1u);
}

/* The common header (pspUtilityDialogCommon), filled as case 104 fills it. */
static void hle_ud_common(uint8_t *block, uint32_t bytes)
{
    memset(block, 0, bytes);
    hle_ud_put32(block, 0x00u, bytes);        /* size */
    hle_ud_put32(block, 0x04u, 1u);           /* language: English */
    hle_ud_put32(block, 0x08u, 1u);           /* buttonSwap: cross accepts */
    hle_ud_put32(block, 0x0cu, 17u);          /* graphics thread priority */
    hle_ud_put32(block, 0x10u, 19u);          /* access thread priority */
    hle_ud_put32(block, 0x14u, 18u);          /* font thread priority */
    hle_ud_put32(block, 0x18u, 16u);          /* sound thread priority */
    hle_ud_put32(block, 0x1cu, HLE_SENTINEL); /* common result: an unwritten word shows */
}

/* Savedata GETSIZE for a save no game owns, with an empty sizeInfo (no files listed). */
static void hle_ud_fill_savedata(uint8_t *block)
{
    hle_ud_common(block, HLE_UD_SD_BYTES);
    hle_ud_put32(block, 0x30u, 22u);          /* mode 22: GETSIZE */
    hle_ud_put32(block, 0x34u, HLE_SENTINEL); /* bind */
    hle_ud_put_text(block, 0x3cu, HLE_UD_SD_GAME_NAME);
    hle_ud_put_text(block, 0x4cu, HLE_UD_SD_SAVE_NAME);
    for (uint32_t i = 0; i < HLE_UD_SD_SIZE_INFO_WORDS; i++) {
        s_ud_sd_size_info[i] = i < 4u ? 0u : HLE_SENTINEL;
    }
    hle_ud_put32(block, 0x5fcu, (uint32_t)(uintptr_t)s_ud_sd_size_info);
}

/* Netconf action 0 (connect to an access point), no ad hoc group, hotspot 0, wifisp 0. */
static void hle_ud_fill_netconf(uint8_t *block)
{
    hle_ud_common(block, HLE_UD_NC_BYTES);
    memset(s_ud_nc_adhoc, 0, sizeof(s_ud_nc_adhoc));
    hle_ud_put32(block, 0x30u, (uint32_t)PSP_NETCONF_ACTION_CONNECTAP);
    hle_ud_put32(block, 0x34u, (uint32_t)(uintptr_t)s_ud_nc_adhoc);
    hle_ud_put32(block, 0x38u, 0u);           /* hotspot: off */
    hle_ud_put32(block, 0x3cu, 0u);           /* hotspot_connected: an output */
    hle_ud_put32(block, 0x40u, 0u);           /* wifisp: off */
}

/* The gamedata install block, mode 0 (no prompt), into the probe-owned directory. */
static void hle_ud_fill_gamedata(uint8_t *block)
{
    hle_ud_common(block, HLE_UD_GD_BYTES);
    hle_ud_put32(block, 0x30u, 0u);
    hle_ud_put_text(block, 0x34u, HLE_UD_GD_GAME_NAME);
    hle_ud_put_text(block, 0x44u, HLE_UD_GD_DATA_NAME);
    hle_ud_put_text(block, 0x58u, "Nakagawa oracle probe");
    hle_ud_put_text(block, 0xd8u, "Probe data");
    hle_ud_put_text(block, 0x158u, "Synthetic probe install");
    block[0x558u] = 1u;                       /* parental level */
    hle_ud_put32(block, 0x55cu, HLE_SENTINEL); /* progress */
    hle_ud_put32(block, 0x560u, HLE_SENTINEL);
    hle_ud_put32(block, 0x564u, HLE_SENTINEL);
}

/* Game sharing, single send, the probe's pattern bytes in memory. */
static void hle_ud_fill_sharing(uint8_t *block)
{
    hle_ud_common(block, HLE_UD_GS_BYTES);
    for (uint32_t i = 0; i < HLE_UD_GS_DATA_BYTES; i++) {
        s_ud_gs_data[i] = (uint8_t)(i * 0x3Bu + 0x11u);
    }
    hle_ud_put_text(block, 0x38u, HLE_UD_GS_NAME);
    hle_ud_put32(block, 0x4cu, HLE_SENTINEL); /* result */
    hle_ud_put32(block, 0x54u, (uint32_t)PSP_UTILITY_GAMESHARING_MODE_SINGLE);
    hle_ud_put32(block, 0x58u, (uint32_t)PSP_UTILITY_GAMESHARING_DATA_TYPE_MEMORY);
    hle_ud_put32(block, 0x5cu, (uint32_t)(uintptr_t)s_ud_gs_data);
    hle_ud_put32(block, 0x60u, HLE_UD_GS_DATA_BYTES);
}

static const HleUdType k_ud_savedata = {
    "savedata", nkUtilSavedataInitStart, nkUtilSavedataGetStatus, nkUtilSavedataUpdate,
    nkUtilSavedataShutdownStart, hle_ud_fill_savedata, s_ud_sd, s_ud_sd, 0x34u,
};
static const HleUdType k_ud_gamedata = {
    "gamedata", nkUtilGamedataInstallInitStart, nkUtilGamedataInstallGetStatus,
    nkUtilGamedataInstallUpdate, nkUtilGamedataInstallShutdownStart, hle_ud_fill_gamedata,
    s_ud_gd[0], s_ud_gd[1], 0x55cu,
};
static const HleUdType k_ud_netconf = {
    "netconf", nkUtilNetconfInitStart, nkUtilNetconfGetStatus, nkUtilNetconfUpdate,
    nkUtilNetconfShutdownStart, hle_ud_fill_netconf, s_ud_nc[0], s_ud_nc[1], 0x3cu,
};
static const HleUdType k_ud_sharing = {
    "sharing", nkUtilGameSharingInitStart, nkUtilGameSharingGetStatus, nkUtilGameSharingUpdate,
    nkUtilGameSharingShutdownStart, hle_ud_fill_sharing, s_ud_gs[0], s_ud_gs[1], 0x4cu,
};

static void hle_ud_skip(const char *case_id, uint32_t reason, uint32_t count)
{
    uint32_t zeros[HLE_MAX_WORDS];
    memset(zeros, 0, sizeof(zeros));
    hle_record(case_id, "SKIP", reason, zeros, count);
}

/* A step marker named for the dialog type, "<type>-<what>". */
static void hle_ud_step(const HleUdType *t, const char *what)
{
    static char step[48];
    snprintf(step, sizeof(step), "%s-%s", t->tag, what);
    hle_step(step);
}

static uint32_t hle_ud_code(int status)
{
    return (status >= HLE_UD_STATUS_NONE && status <= HLE_UD_STATUS_SHUTDOWN) ? (uint32_t)status
                                                                               : 0xFFu;
}

static void hle_ud_begin(HleUdRun *r, const HleUdType *t)
{
    memset(r, 0, sizeof(*r));
    r->t = t;
    r->sd_rc = HLE_SENTINEL;
    r->sd_at = HLE_SENTINEL;
    r->sd_status = HLE_SENTINEL;
    r->outcome = HLE_UD_OUT_STUCK;
    for (uint32_t i = 0; i < HLE_UD_CHANGE_SLOTS; i++) {
        r->change[i] = HLE_SENTINEL;
    }
}

/* One poll: the vblank and GetStatus, kept in memory (nothing is written to host0 here). */
static int hle_ud_poll(HleUdRun *r)
{
    const uint32_t at = sceDisplayGetVcount() - r->v0;
    const int status = r->t->get_status();
    if (r->polls == 0u || status != r->last) {
        if (r->changes < HLE_UD_CHANGE_SLOTS) {
            r->change[r->changes] = (at << 8) | hle_ud_code(status);
        }
        r->changes++;
    }
    r->polls++;
    r->last = status;
    r->end_at = at;
    if (status >= HLE_UD_STATUS_INIT && status <= HLE_UD_STATUS_SHUTDOWN) {
        r->active = 1;
    }
    return status;
}

/* A ShutdownStart from a poll loop: its marker, then the vblank, then the call. */
static void hle_ud_shutdown(HleUdRun *r, int status)
{
    hle_ud_step(r->t, "shutdown");
    const uint32_t at = sceDisplayGetVcount() - r->v0;
    const int rc = r->t->shutdown();
    if (r->sd_calls == 0u) {
        r->sd_rc = (uint32_t)rc;
        r->sd_at = at;
        r->sd_status = (uint32_t)status;
    }
    r->sd_calls++;
    if (rc >= 0) {
        r->accepted = 1;
    }
}

/* Drive the dialog to NONE, at most HLE_UD_POLL_LIMIT polls one vblank apart. */
static void hle_ud_drive(HleUdRun *r)
{
    uint32_t running = 0;
    uint32_t initial = 0;
    uint32_t idle = 0;
    r->outcome = HLE_UD_OUT_STUCK;
    for (uint32_t i = 0; i < HLE_UD_POLL_LIMIT; i++) {
        const int status = hle_ud_poll(r);
        if (status < 0) {
            r->outcome = HLE_UD_OUT_STATUS_ERROR;
            return;
        }
        if (status == HLE_UD_STATUS_NONE) {
            idle++;
            if (r->accepted) {
                r->outcome = HLE_UD_OUT_NONE;
                return;
            }
            if (r->active || idle >= HLE_UD_CANCEL_POLLS) {
                r->outcome = HLE_UD_OUT_SELF_NONE;
                return;
            }
        } else if (status == HLE_UD_STATUS_RUNNING) {
            running++;
            if (!r->accepted && r->sd_calls < HLE_UD_RUN_SHUTDOWNS &&
                running >= HLE_UD_CANCEL_POLLS * (r->sd_calls + 1u)) {
                hle_ud_shutdown(r, status);
            } else {
                (void)r->t->update(1);
                r->updates++;
            }
        } else if (status == HLE_UD_STATUS_FINISHED) {
            if (!r->accepted && !r->finish_called) {
                r->finish_called = 1;
                hle_ud_shutdown(r, status);
            }
        } else if (status == HLE_UD_STATUS_INIT) {
            initial++;
            if (!r->accepted && !r->init_cancel_called && initial >= HLE_UD_INIT_CANCEL_POLLS) {
                r->init_cancel_called = 1;
                hle_ud_shutdown(r, status);
            }
        }
        (void)sceDisplayWaitVblankStart();
    }
}

/* Poll until the dialog is in `want`, at most HLE_UD_REACH_POLLS polls; returns the status the
   last poll read. Update(1) is called only on the way to FINISHED, and a dialog still RUNNING
   after HLE_UD_CANCEL_POLLS polls has not reached FINISHED. A status past `want` ends the
   search. */
static int hle_ud_reach(HleUdRun *r, int want)
{
    uint32_t running = 0;
    int status = -1;
    for (uint32_t i = 0; i < HLE_UD_REACH_POLLS; i++) {
        status = hle_ud_poll(r);
        if (status == want || status < 0) {
            return status;
        }
        if (status == HLE_UD_STATUS_NONE) {
            if (r->active) {
                return status;
            }
        } else if (status > want) {
            return status;
        } else if (status == HLE_UD_STATUS_RUNNING) {
            running++;
            if (running >= HLE_UD_CANCEL_POLLS) {
                return status;
            }
            (void)r->t->update(1);
            r->updates++;
        }
        (void)sceDisplayWaitVblankStart();
    }
    return status;
}

static uint32_t hle_ud_stuck_reason(uint32_t reason)
{
    return reason ? reason : (s_ud_stuck ? HLE_UD_SKIP_STUCK : 0u);
}

static void hle_ud_settle(const HleUdRun *r)
{
    if (r->outcome == HLE_UD_OUT_STUCK || r->outcome == HLE_UD_OUT_STATUS_ERROR) {
        s_ud_stuck++;
        s_ud_fail++;
    }
}

/* Item 1 and 3: one dialog from InitStart back to NONE, with each status change's vblank. */
static void hle_ud_timing(const char *case_id, const HleUdType *t, uint32_t reason)
{
    reason = hle_ud_stuck_reason(reason);
    if (reason) {
        hle_ud_skip(case_id, reason, HLE_UD_TIMING_WORDS);
        return;
    }
    HleUdRun r;
    uint32_t out[HLE_UD_TIMING_WORDS];
    hle_ud_begin(&r, t);
    t->fill(t->first);
    hle_step(case_id);
    r.v0 = sceDisplayGetVcount();
    r.init_rc = t->init(t->first);
    if (r.init_rc < 0) {
        (void)hle_ud_poll(&r);
        r.outcome = HLE_UD_OUT_REFUSED;
    } else {
        hle_ud_drive(&r);
    }
    hle_ud_settle(&r);
    out[0] = (uint32_t)r.init_rc;
    out[1] = r.outcome;
    out[2] = r.polls;
    out[3] = r.updates;
    out[4] = r.sd_rc;
    out[5] = r.sd_at;
    out[6] = r.sd_status;
    out[7] = r.sd_calls;
    out[8] = hle_ud_get32(t->first, 0x1cu);
    out[9] = hle_ud_get32(t->first, t->output_offset);
    out[10] = (uint32_t)r.last;
    out[11] = r.end_at;
    out[12] = r.changes;
    for (uint32_t i = 0; i < HLE_UD_CHANGE_SLOTS; i++) {
        out[HLE_UD_TIMING_HEAD + i] = r.change[i];
    }
    hle_record(case_id, "PASS", (uint32_t)r.init_rc, out, HLE_UD_TIMING_WORDS);
}

/* Item 2: the wrong-state calls in one state, on a fresh dialog, then the drive back to NONE. */
static void hle_ud_state(const char *case_id, const HleUdType *t, int state, uint32_t reason)
{
    reason = hle_ud_stuck_reason(reason);
    if (reason) {
        hle_ud_skip(case_id, reason, HLE_UD_STATE_WORDS);
        return;
    }
    HleUdRun r;
    uint32_t out[HLE_UD_STATE_WORDS];
    for (uint32_t i = 0; i < HLE_UD_STATE_WORDS; i++) {
        out[i] = HLE_SENTINEL;
    }
    hle_ud_begin(&r, t);
    t->fill(t->first);
    t->fill(t->second);
    memset(s_ud_msg, 0, sizeof(s_ud_msg));
    hle_step(case_id);
    r.v0 = sceDisplayGetVcount();
    r.init_rc = t->init(t->first);
    out[0] = (uint32_t)r.init_rc;
    if (r.init_rc < 0) {
        out[1] = 2u;
        (void)hle_ud_poll(&r);
        r.outcome = HLE_UD_OUT_REFUSED;
    } else {
        const int want = state == HLE_UD_STATUS_SHUTDOWN ? HLE_UD_STATUS_RUNNING : state;
        int status = hle_ud_reach(&r, want);
        int entry_accepted = 0;
        if (state == HLE_UD_STATUS_SHUTDOWN &&
            (status == HLE_UD_STATUS_RUNNING || status == HLE_UD_STATUS_FINISHED)) {
            hle_ud_step(t, "entry-shutdown");
            const int entry = t->shutdown();
            out[14] = (uint32_t)entry;
            entry_accepted = entry >= 0;
            status = t->get_status();
        } else if ((state == HLE_UD_STATUS_RUNNING || state == HLE_UD_STATUS_FINISHED) &&
                   status == state) {
            /* Both states wait for the caller, so the marker cannot move them on. */
            hle_ud_step(t, "calls");
            status = t->get_status();
        }
        out[3] = (uint32_t)status;
        r.accepted = entry_accepted;
        if (status == state) {
            out[1] = 1u;
            out[2] = r.end_at;
            out[4] = (uint32_t)t->update(1);
            out[5] = (uint32_t)t->get_status();
            out[6] = (uint32_t)t->init(t->second);
            out[7] = (uint32_t)t->get_status();
            out[8] = (uint32_t)nkUtilMsgDialogInitStart(s_ud_msg);
            out[9] = (uint32_t)t->get_status();
            out[10] = (uint32_t)t->shutdown();
            out[11] = (uint32_t)t->get_status();
            out[12] = (uint32_t)t->shutdown();
            out[13] = (uint32_t)t->get_status();
            if ((int)out[6] >= 0) {
                r.accepted = 0;
            }
            if ((int)out[10] >= 0 || (int)out[12] >= 0) {
                r.accepted = 1;
            }
            if ((int)out[8] >= 0) {
                s_ud_stuck++;
                s_ud_fail++;
            }
        } else {
            out[1] = 0u;
        }
        hle_ud_step(t, "drive");
        hle_ud_drive(&r);
    }
    hle_ud_settle(&r);
    out[15] = r.outcome;
    out[16] = r.sd_calls;
    out[17] = r.end_at;
    out[18] = (uint32_t)r.last;
    out[19] = hle_ud_get32(t->first, 0x1cu);
    out[20] = hle_ud_get32(t->second, 0x1cu);
    hle_record(case_id, "PASS", (uint32_t)r.init_rc, out, HLE_UD_STATE_WORDS);
}

static void hle_ud_states(const char *init_id, const char *running_id, const char *finished_id,
                          const char *shutdown_id, const HleUdType *t, uint32_t reason)
{
    hle_ud_state(init_id, t, HLE_UD_STATUS_INIT, reason);
    hle_ud_state(running_id, t, HLE_UD_STATUS_RUNNING, reason);
    hle_ud_state(finished_id, t, HLE_UD_STATUS_FINISHED, reason);
    hle_ud_state(shutdown_id, t, HLE_UD_STATUS_SHUTDOWN, reason);
}

/* The gamedata cells run only with no disc in the drive and no directory of that name. */
static uint32_t hle_ud_gamedata_precheck(void)
{
    SceIoStat st;
    uint32_t out[HLE_UD_PRECHECK_WORDS];
    memset(&st, 0, sizeof(st));
    hle_step("gamedata-precheck-umd");
    const int umd = sceUmdCheckMedium();
    hle_step("gamedata-precheck-dopen");
    const SceUID dfd = sceIoDopen(HLE_UD_GD_DIR);
    out[2] = HLE_SENTINEL;
    if (dfd >= 0) {
        hle_step("gamedata-precheck-dclose");
        out[2] = (uint32_t)sceIoDclose(dfd);
    }
    hle_step("gamedata-precheck-stat");
    const int stat = sceIoGetstat(HLE_UD_GD_DIR, &st);
    uint32_t reason = 0u;
    if (s_ud_stuck) {
        reason = HLE_UD_SKIP_STUCK;
    } else if (umd < 0) {
        reason = HLE_UD_SKIP_UMD_UNKNOWN;
    } else if (umd != 0) {
        reason = HLE_UD_SKIP_UMD_PRESENT;
    } else if (dfd >= 0) {
        reason = HLE_UD_SKIP_DIR_EXISTS;
    } else if ((uint32_t)stat != HLE_UD_PATH_ABSENT) {
        reason = HLE_UD_SKIP_DIR_UNCLEAR;
    }
    out[0] = (uint32_t)umd;
    out[1] = (uint32_t)dfd;
    out[3] = (uint32_t)stat;
    out[4] = reason;
    hle_record("gamedata-precheck", "PASS", reason, out, HLE_UD_PRECHECK_WORDS);
    return reason;
}

/* s_ud_path = HLE_UD_GD_DIR "/" name. A kept name is shorter than HLE_UD_NAME_BYTES (the
   collection checks it), and the copy stops at HLE_UD_NAME_BYTES - 1 bytes in any case, so the
   path always fits: sizeof(HLE_UD_GD_DIR) - 1 + 1 + 63 + 1 <= sizeof(s_ud_path). Built with
   memcpy rather than snprintf, so nothing can be cut short. */
static void hle_ud_file_path(const char *name)
{
    const uint32_t dir = (uint32_t)sizeof(HLE_UD_GD_DIR) - 1u;
    uint32_t len = 0;
    for (; len + 1u < HLE_UD_NAME_BYTES && name[len] != '\0'; len++) {
    }
    memcpy(s_ud_path, HLE_UD_GD_DIR, dir);
    s_ud_path[dir] = '/';
    memcpy(s_ud_path + dir + 1u, name, len);
    s_ud_path[dir + 1u + len] = '\0';
}

/* Remove what the dialog left in the probe-owned directory, then the directory itself. */
static void hle_ud_gamedata_cleanup(uint32_t reason)
{
    if (reason) {
        hle_ud_skip("gamedata-cleanup", HLE_UD_SKIP_NOT_RUN, HLE_UD_CLEANUP_WORDS);
        return;
    }
    uint32_t out[HLE_UD_CLEANUP_WORDS];
    uint32_t sizes[HLE_UD_CLEAN_FILES];
    uint32_t seen = 0;
    uint32_t files = 0;
    uint32_t left = 0;
    memset(sizes, 0, sizeof(sizes));
    for (uint32_t i = 0; i < HLE_UD_CLEANUP_WORDS; i++) {
        out[i] = HLE_SENTINEL;
    }
    hle_step("gamedata-cleanup-dopen");
    const SceUID dfd = sceIoDopen(HLE_UD_GD_DIR);
    out[0] = (uint32_t)dfd;
    if (dfd >= 0) {
        for (uint32_t i = 0; i < HLE_UD_DIR_READS; i++) {
            memset(&s_ud_dirent, 0, sizeof(s_ud_dirent));
            hle_step("gamedata-cleanup-dread");
            if (sceIoDread(dfd, &s_ud_dirent) <= 0) {
                break;
            }
            const char *name = s_ud_dirent.d_name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                continue;
            }
            seen++;
            uint32_t len = 0;
            for (; len < HLE_UD_NAME_BYTES && name[len] != '\0'; len++) {
            }
            if (FIO_S_ISREG(s_ud_dirent.d_stat.st_mode) && files < HLE_UD_CLEAN_FILES &&
                len > 0u && len < HLE_UD_NAME_BYTES) {
                memcpy(s_ud_names[files], name, len + 1u);
                sizes[files] = (uint32_t)s_ud_dirent.d_stat.st_size;
                files++;
            } else {
                left++;
            }
        }
        hle_step("gamedata-cleanup-dclose");
        out[4] = (uint32_t)sceIoDclose(dfd);
        for (uint32_t f = 0; f < files; f++) {
            hle_ud_file_path(s_ud_names[f]);
            hle_step("gamedata-cleanup-remove");
            out[HLE_UD_CLEAN_HEAD + f] = (uint32_t)sceIoRemove(s_ud_path);
            out[HLE_UD_CLEAN_HEAD + HLE_UD_CLEAN_FILES + f] = sizes[f];
        }
        hle_step("gamedata-cleanup-rmdir");
        out[5] = (uint32_t)sceIoRmdir(HLE_UD_GD_DIR);
    }
    hle_step("gamedata-cleanup-verify");
    const SceUID again = sceIoDopen(HLE_UD_GD_DIR);
    out[6] = (uint32_t)again;
    if (again >= 0) {
        hle_step("gamedata-cleanup-verify-dclose");
        (void)sceIoDclose(again);
        s_ud_fail++;
    }
    out[1] = seen;
    out[2] = files;
    out[3] = left;
    hle_record("gamedata-cleanup", again < 0 ? "PASS" : "FAIL", (uint32_t)again, out,
               HLE_UD_CLEANUP_WORDS);
}

static void hle_ud_gamedata(void)
{
    const uint32_t reason = hle_ud_gamedata_precheck();
    hle_ud_timing("gamedata-timing", &k_ud_gamedata, reason);
    hle_ud_states("gamedata-state-init", "gamedata-state-running", "gamedata-state-finished",
                  "gamedata-state-shutdown", &k_ud_gamedata, reason);
    hle_ud_gamedata_cleanup(reason);
}

/* Netconf's net setup; *stage counts the calls that succeeded, in order. */
static uint32_t hle_ud_netconf_setup(uint32_t *stage)
{
    uint32_t out[HLE_UD_NET_WORDS];
    if (s_ud_stuck) {
        hle_ud_skip("netconf-setup", HLE_UD_SKIP_STUCK, HLE_UD_NET_WORDS);
        return HLE_UD_SKIP_STUCK;
    }
    for (uint32_t i = 0; i < HLE_UD_NET_WORDS; i++) {
        out[i] = HLE_SENTINEL;
    }
    hle_step("netconf-load-common");
    out[0] = (uint32_t)sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if ((int)out[0] >= 0) {
        *stage = 1u;
        hle_step("netconf-load-inet");
        out[1] = (uint32_t)sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    }
    if (*stage == 1u && (int)out[1] >= 0) {
        *stage = 2u;
        hle_step("netconf-net-init");
        out[2] = (uint32_t)sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    }
    if (*stage == 2u && (int)out[2] >= 0) {
        *stage = 3u;
        hle_step("netconf-inet-init");
        out[3] = (uint32_t)sceNetInetInit();
    }
    if (*stage == 3u && (int)out[3] >= 0) {
        *stage = 4u;
        hle_step("netconf-apctl-init");
        out[4] = (uint32_t)sceNetApctlInit(0x8000, 48);
    }
    if (*stage == 4u && (int)out[4] >= 0) {
        *stage = HLE_UD_NET_STEPS;
    }
    const uint32_t result = *stage == HLE_UD_NET_STEPS ? 0u : out[*stage];
    hle_record("netconf-setup", "PASS", result, out, HLE_UD_NET_WORDS);
    return *stage == HLE_UD_NET_STEPS ? 0u : HLE_UD_SKIP_NET_SETUP;
}

static void hle_ud_netconf_teardown(uint32_t stage, uint32_t skipped)
{
    uint32_t out[HLE_UD_NET_WORDS];
    if (skipped) {
        hle_ud_skip("netconf-teardown", skipped, HLE_UD_NET_WORDS);
        return;
    }
    for (uint32_t i = 0; i < HLE_UD_NET_WORDS; i++) {
        out[i] = HLE_SENTINEL;
    }
    if (stage >= 5u) {
        hle_step("netconf-apctl-term");
        out[0] = (uint32_t)sceNetApctlTerm();
    }
    if (stage >= 4u) {
        hle_step("netconf-inet-term");
        out[1] = (uint32_t)sceNetInetTerm();
    }
    if (stage >= 3u) {
        hle_step("netconf-net-term");
        out[2] = (uint32_t)sceNetTerm();
    }
    if (stage >= 2u) {
        hle_step("netconf-unload-inet");
        out[3] = (uint32_t)sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    }
    if (stage >= 1u) {
        hle_step("netconf-unload-common");
        out[4] = (uint32_t)sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    }
    hle_record("netconf-teardown", "PASS", stage, out, HLE_UD_NET_WORDS);
}

static void hle_ud_netconf(void)
{
    uint32_t stage = 0;
    const uint32_t reason = hle_ud_netconf_setup(&stage);
    hle_ud_timing("netconf-timing", &k_ud_netconf, reason);
    hle_ud_states("netconf-state-init", "netconf-state-running", "netconf-state-finished",
                  "netconf-state-shutdown", &k_ud_netconf, reason);
    hle_ud_netconf_teardown(stage, reason == HLE_UD_SKIP_STUCK ? reason : 0u);
}

/* Game sharing's ad hoc setup: init calls only, no group is created or joined here. */
static uint32_t hle_ud_sharing_setup(uint32_t *stage)
{
    uint32_t out[HLE_UD_NET_WORDS];
    if (s_ud_stuck) {
        hle_ud_skip("sharing-setup", HLE_UD_SKIP_STUCK, HLE_UD_NET_WORDS);
        return HLE_UD_SKIP_STUCK;
    }
    for (uint32_t i = 0; i < HLE_UD_NET_WORDS; i++) {
        out[i] = HLE_SENTINEL;
    }
    memset(&s_ud_product, 0, sizeof(s_ud_product));
    memcpy(s_ud_product.product, HLE_UD_PRODUCT, sizeof(s_ud_product.product));
    hle_step("sharing-load-common");
    out[0] = (uint32_t)sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if ((int)out[0] >= 0) {
        *stage = 1u;
        hle_step("sharing-load-adhoc");
        out[1] = (uint32_t)sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC);
    }
    if (*stage == 1u && (int)out[1] >= 0) {
        *stage = 2u;
        hle_step("sharing-net-init");
        out[2] = (uint32_t)sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    }
    if (*stage == 2u && (int)out[2] >= 0) {
        *stage = 3u;
        hle_step("sharing-adhoc-init");
        out[3] = (uint32_t)sceNetAdhocInit();
    }
    if (*stage == 3u && (int)out[3] >= 0) {
        *stage = 4u;
        hle_step("sharing-adhocctl-init");
        out[4] = (uint32_t)sceNetAdhocctlInit(0x2000, 0x20, &s_ud_product);
    }
    if (*stage == 4u && (int)out[4] >= 0) {
        *stage = HLE_UD_NET_STEPS;
    }
    const uint32_t result = *stage == HLE_UD_NET_STEPS ? 0u : out[*stage];
    hle_record("sharing-setup", "PASS", result, out, HLE_UD_NET_WORDS);
    return *stage == HLE_UD_NET_STEPS ? 0u : HLE_UD_SKIP_NET_SETUP;
}

static void hle_ud_sharing_teardown(uint32_t stage, uint32_t skipped)
{
    uint32_t out[HLE_UD_NET_WORDS];
    if (skipped) {
        hle_ud_skip("sharing-teardown", skipped, HLE_UD_NET_WORDS);
        return;
    }
    for (uint32_t i = 0; i < HLE_UD_NET_WORDS; i++) {
        out[i] = HLE_SENTINEL;
    }
    if (stage >= 5u) {
        hle_step("sharing-adhocctl-term");
        out[0] = (uint32_t)sceNetAdhocctlTerm();
    }
    if (stage >= 4u) {
        hle_step("sharing-adhoc-term");
        out[1] = (uint32_t)sceNetAdhocTerm();
    }
    if (stage >= 3u) {
        hle_step("sharing-net-term");
        out[2] = (uint32_t)sceNetTerm();
    }
    if (stage >= 2u) {
        hle_step("sharing-unload-adhoc");
        out[3] = (uint32_t)sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
    }
    if (stage >= 1u) {
        hle_step("sharing-unload-common");
        out[4] = (uint32_t)sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    }
    hle_record("sharing-teardown", "PASS", stage, out, HLE_UD_NET_WORDS);
}

static void hle_ud_sharing(void)
{
    uint32_t stage = 0;
    const uint32_t reason = hle_ud_sharing_setup(&stage);
    hle_ud_timing("sharing-timing", &k_ud_sharing, reason);
    hle_ud_states("sharing-state-init", "sharing-state-running", "sharing-state-finished",
                  "sharing-state-shutdown", &k_ud_sharing, reason);
    hle_ud_sharing_teardown(stage, reason == HLE_UD_SKIP_STUCK ? reason : 0u);
}

static int hle_run(void)
{
    hle_ud_timing("savedata-timing-r1", &k_ud_savedata, 0u);
    hle_ud_timing("savedata-timing-r2", &k_ud_savedata, 0u);
    hle_ud_timing("savedata-timing-r3", &k_ud_savedata, 0u);
    hle_ud_gamedata();
    hle_ud_netconf();
    hle_ud_sharing();
    return s_ud_fail == 0u;
}

#endif /* HLE_CASE_UTILITY_DIALOGS */

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
