// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Kernel semantics cells for the PSP-3000 oracle, one PRX and one launch:
//   CASE=kernel-semantics (PSP-KERNEL-SEM-001): #1287 VBLANK sub-interrupt delivery order,
//     #1390 controller sampling cells, #1383 volatile-lock types and allocations, #1380
//     exclusive create and 64-bit seeks in a probe-owned Memory Stick directory, and #1382
//     WLAN switch and power state. The console's MAC address and OpenPSID are never read.
//
// These cells live apart from probe_hle_issues.c because they do what that file may not:
// they write and remove a Memory Stick file and directory, allocate partition memory, and
// change and restore the controller sampling mode and cycle.
//
// Record stream and log: the same writer, host0 round trip and module lifecycle as
// probe_hle_issues.c, repeated here byte for byte (tools/test_psp_oracle.py pins the
// copies). NAKAGAWA_PSP_META, a NAKAGAWA_PSP_STEP before each cell, one NAKAGAWA_PSP_TEST
// per measurement, a `kernel-semantics-done` terminal record whose out0 counts the
// measurements, the host0 round trip and a final NAKAGAWA_PSP_COMPLETE line.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspintrman.h>
#include <pspctrl.h>
#include <pspsuspend.h>
#include <pspsysmem.h>
#include <pspwlan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define KS_CASE_KERNEL_SEM 127
#if PSP_ORACLE_CASE != KS_CASE_KERNEL_SEM
#error probe_kernel_semantics.c has no case for this PSP_ORACLE_CASE
#endif
#define HLE_CAMPAIGN_ID "kernel-semantics"
#define HLE_TEST_ID "PSP-KERNEL-SEM-001"
#define HLE_LOG "host0:/kernel_semantics_log.txt"
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

#define HLE_SENTINEL 0xA5A5A5A5u
/* The same value as text, for the assembly wrappers (IC_V0_WRAPPER). */
#define HLE_SENTINEL_HEX "0xA5A5A5A5"

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

/* Shared end of run, after every measurement and its cleanup. The round trip
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
/* Kernel semantics cells for v0.0.2 (PSP-KERNEL-SEM-001), one launch, user mode:
   (a) #1287: four VBLANK sub-interrupt handlers registered at once, in the order 3, 0, 2, 1:
       the order every VBLANK calls them in and the (no, arg) each receives;
   (b) #1390: a blocking controller read with no unread sample; a short read after several
       samples accumulated, and what the next read finds; peek counts; the analog bytes in
       digital and in analog sampling mode; the sample spacing at cycle 5555 and the wait of a
       blocking read there;
   (c) #1383: TryLock and Lock of the volatile memory with types other than 0 while it is free,
       and partition-5 and fixed-address allocations while it is free and while it is held;
   (d) #1380: sceIoOpen with PSP_O_CREAT | PSP_O_EXCL in a probe-owned Memory Stick directory,
       ms0:/NKKSEM00 (created and removed by the probe; every io row is SKIP when it already
       exists): under the missing directory, for a new file, for the existing file; then
       64-bit seeks past 4 GiB on that 16-byte file, with no write past its end;
   (e) #1382: the WLAN switch and power state. The MAC address and the OpenPSID are
       console-bound values and are never read (no sceWlanGetEtherAddr or sceOpenPSID import;
       the build-time gate pins sceWlanDrv to the two state calls).
   Every row is emitted on every run with a fixed word count; a word that does not apply is
   HLE_SENTINEL. Each cell restores what it changed: the sampling mode and cycle, the
   sub-interrupt handlers (disabled and released), the volatile lock (unlocked), every block
   (freed) and the Memory Stick directory (removed). */


/* ---- (a) VBLANK sub-interrupt delivery order --------------------------------------------- */
#define KS_VBL_LOG 48u
#define KS_VBL_SUBS 4u
static const int k_ks_vbl_order[KS_VBL_SUBS] = { 3, 0, 2, 1 };
static volatile uint32_t s_ks_vbl_count;
static volatile uint32_t s_ks_vbl_word[KS_VBL_LOG];
static volatile uint32_t s_ks_vbl_time[KS_VBL_LOG];

static int ks_vbl_handler(int sub, void *arg)
{
    const uint32_t n = s_ks_vbl_count;
    if (n < KS_VBL_LOG) {
        s_ks_vbl_word[n] = ((uint32_t)sub << 16) | ((uint32_t)(uintptr_t)arg & 0xffffu);
        s_ks_vbl_time[n] = sceKernelGetSystemTimeLow();
    }
    s_ks_vbl_count = n + 1u;
    return -1;
}

/* Rows subintr-register (8 words: the four Register codes, then the four Enable codes, in
   registration order), subintr-order (result = calls; 24 words: the first 24 calls as
   sub << 16 | arg), subintr-times (24 words: those calls' times minus the first call's) and
   subintr-release (8 words: the four Disable codes, then the four Release codes). */
static void ks_subintr_order(void)
{
    uint32_t w[24];
    s_ks_vbl_count = 0u;
    for (unsigned i = 0; i < KS_VBL_LOG; i++) {
        s_ks_vbl_word[i] = HLE_SENTINEL;
        s_ks_vbl_time[i] = 0u;
    }
    hle_step("subintr-register");
    for (unsigned i = 0; i < KS_VBL_SUBS; i++) {
        const int no = k_ks_vbl_order[i];
        w[i] = (uint32_t)sceKernelRegisterSubIntrHandler(PSP_VBLANK_INT, no, (void *)ks_vbl_handler,
                                                         (void *)(uintptr_t)(0xa000u + (unsigned)no));
    }
    for (unsigned i = 0; i < KS_VBL_SUBS; i++) {
        w[4u + i] = (uint32_t)sceKernelEnableSubIntr(PSP_VBLANK_INT, k_ks_vbl_order[i]);
    }
    hle_record("subintr-register", "PASS", 0u, w, 8u);
    sceKernelDelayThread(70000);   /* about four VBLANKs */
    for (unsigned i = 0; i < KS_VBL_SUBS; i++) {
        w[i] = (uint32_t)sceKernelDisableSubIntr(PSP_VBLANK_INT, k_ks_vbl_order[i]);
    }
    for (unsigned i = 0; i < KS_VBL_SUBS; i++) {
        w[4u + i] = (uint32_t)sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, k_ks_vbl_order[i]);
    }
    const uint32_t calls = s_ks_vbl_count;
    uint32_t order[24], times[24];
    for (unsigned i = 0; i < 24u; i++) {
        const int have = i < calls && i < KS_VBL_LOG;
        order[i] = have ? s_ks_vbl_word[i] : HLE_SENTINEL;
        times[i] = have ? s_ks_vbl_time[i] - s_ks_vbl_time[0] : HLE_SENTINEL;
    }
    hle_record("subintr-order", "PASS", calls, order, 24u);
    hle_record("subintr-times", "PASS", calls, times, 24u);
    hle_record("subintr-release", "PASS", 0u, w, 8u);
}

/* ---- (b) controller sampling -------------------------------------------------------------- */
static SceCtrlData s_ks_pad[64];
static SceCtrlData s_ks_pad2[64];

static uint32_t ks_analog_word(const SceCtrlData *d)
{
    return (uint32_t)d->Lx | ((uint32_t)d->Ly << 8) | ((uint32_t)d->Rsrv[0] << 16) |
           ((uint32_t)d->Rsrv[1] << 24);
}

/* Row ctrl-empty-read (6 words): three times, a drain read of 64, then a read of 1: its
   result and the time it waited. */
static void ks_ctrl_empty_read(void)
{
    uint32_t w[6];
    hle_step("ctrl-empty-read");
    for (unsigned i = 0; i < 3u; i++) {
        (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        w[2u * i] = (uint32_t)sceCtrlReadBufferPositive(s_ks_pad, 1);
        w[2u * i + 1u] = sceKernelGetSystemTimeLow() - t0;
    }
    hle_record("ctrl-empty-read", "PASS", 0u, w, 6u);
}

/* Rows ctrl-short-read-peek (8 words: the time stamps of the newest eight samples a peek of 64
   reports, oldest first, minus the drain time), ctrl-short-read (3 words: the result of a read
   of 2 after a 100 ms pause, then its two time stamps minus the drain time) and
   ctrl-short-read-next (3 words: the next read of 64's result, how long it waited, and its
   first time stamp minus the drain time). */
static void ks_ctrl_short_read(void)
{
    uint32_t w[8];
    hle_step("ctrl-short-read");
    (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    sceKernelDelayThread(100000);
    const int peeked = sceCtrlPeekBufferPositive(s_ks_pad, 64);
    for (unsigned i = 0; i < 8u; i++) {
        w[i] = peeked == 64 ? s_ks_pad[56u + i].TimeStamp - t0 : HLE_SENTINEL;
    }
    hle_record("ctrl-short-read-peek", "PASS", (uint32_t)peeked, w, 8u);
    const int got = sceCtrlReadBufferPositive(s_ks_pad2, 2);
    w[0] = (uint32_t)got;
    w[1] = got >= 1 ? s_ks_pad2[0].TimeStamp - t0 : HLE_SENTINEL;
    w[2] = got >= 2 ? s_ks_pad2[1].TimeStamp - t0 : HLE_SENTINEL;
    hle_record("ctrl-short-read", "PASS", (uint32_t)got, w, 3u);
    const uint32_t t1 = sceKernelGetSystemTimeLow();
    const int next = sceCtrlReadBufferPositive(s_ks_pad2, 64);
    w[0] = (uint32_t)next;
    w[1] = sceKernelGetSystemTimeLow() - t1;
    w[2] = next >= 1 ? s_ks_pad2[0].TimeStamp - t0 : HLE_SENTINEL;
    hle_record("ctrl-short-read-next", "PASS", (uint32_t)next, w, 3u);
}

/* Row ctrl-peek-count (3 words): peeks of 64, 3 and 1 right after a read. */
static void ks_ctrl_peek_count(void)
{
    uint32_t w[3];
    hle_step("ctrl-peek-count");
    (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
    w[0] = (uint32_t)sceCtrlPeekBufferPositive(s_ks_pad, 64);
    w[1] = (uint32_t)sceCtrlPeekBufferPositive(s_ks_pad, 3);
    w[2] = (uint32_t)sceCtrlPeekBufferPositive(s_ks_pad, 1);
    hle_record("ctrl-peek-count", "PASS", 0u, w, 3u);
}

/* Rows ctrl-mode-digital, ctrl-mode-analog and ctrl-mode-digital-again (9 words: the
   SetSamplingMode result, then the newest eight samples' Lx | Ly << 8 | Rsrv0 << 16 |
   Rsrv1 << 24, 50 ms after the change): digital, analog, then digital again, which tells a
   centred stick from the last analog position kept. The stick is not touched. The mode is
   restored afterwards. */
static void ks_ctrl_modes(void)
{
    uint32_t w[9];
    int mode = -1;
    hle_step("ctrl-mode");
    (void)sceCtrlGetSamplingMode(&mode);
    static const char *const rows[3] = { "ctrl-mode-digital", "ctrl-mode-analog",
                                         "ctrl-mode-digital-again" };
    for (int pass = 0; pass < 3; pass++) {
        w[0] = (uint32_t)sceCtrlSetSamplingMode(pass == 1 ? 1 : 0);
        sceKernelDelayThread(50000);
        const int peeked = sceCtrlPeekBufferPositive(s_ks_pad, 8);
        for (unsigned i = 0; i < 8u; i++) {
            w[1u + i] = peeked == 8 ? ks_analog_word(&s_ks_pad[i]) : HLE_SENTINEL;
        }
        hle_record(rows[pass], "PASS", (uint32_t)mode, w, 9u);
    }
    if (mode == 0 || mode == 1) {
        (void)sceCtrlSetSamplingMode(mode);
    }
}

/* Rows ctrl-cycle-5555 (13 words: the SetSamplingCycle result, then the spacing between the
   first thirteen consecutive samples a read of 64 returns 50 ms after a drain; result = that
   read's result) and ctrl-cycle-wait (3 words: three blocking reads of 1 after a drain at cycle
   5555, the time each waited). The cycle is restored afterwards. */
static void ks_ctrl_cycle(void)
{
    uint32_t w[13];
    int cycle = -1;
    hle_step("ctrl-cycle");
    (void)sceCtrlGetSamplingCycle(&cycle);
    w[0] = (uint32_t)sceCtrlSetSamplingCycle(5555);
    sceKernelDelayThread(20000);
    (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
    sceKernelDelayThread(50000);
    const int got = sceCtrlReadBufferPositive(s_ks_pad, 64);
    for (unsigned i = 0; i < 12u; i++) {
        w[1u + i] = (int)(i + 1u) < got ? s_ks_pad[i + 1u].TimeStamp - s_ks_pad[i].TimeStamp
                                        : HLE_SENTINEL;
    }
    hle_record("ctrl-cycle-5555", "PASS", (uint32_t)got, w, 13u);
    uint32_t waits[3];
    for (unsigned i = 0; i < 3u; i++) {
        (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        (void)sceCtrlReadBufferPositive(s_ks_pad, 1);
        waits[i] = sceKernelGetSystemTimeLow() - t0;
    }
    hle_record("ctrl-cycle-wait", "PASS", 0u, waits, 3u);
    if (cycle >= 0) {
        (void)sceCtrlSetSamplingCycle(cycle);
    }
}

/* ---- (c) volatile memory lock ------------------------------------------------------------- */
#define KS_VMEM_BASE 0x08400000u

/* Rows vmem-trylock-type-<1|2|m1> and vmem-lock-type-1 (4 words: the call's result, the address
   and size outputs (HLE_SENTINEL when unwritten), and the Unlock result when the call locked,
   else HLE_SENTINEL). The lock is free before each. */
static void ks_vmem_type(const char *row, int lock, int type)
{
    uint32_t w[4];
    void *addr = (void *)(uintptr_t)HLE_SENTINEL;
    int size = (int)HLE_SENTINEL;
    hle_step(row);
    const int rc = lock ? sceKernelVolatileMemLock(type, &addr, &size)
                        : sceKernelVolatileMemTryLock(type, &addr, &size);
    w[0] = (uint32_t)rc;
    w[1] = (uint32_t)(uintptr_t)addr;
    w[2] = (uint32_t)size;
    w[3] = rc == 0 ? (uint32_t)sceKernelVolatileMemUnlock(0) : HLE_SENTINEL;
    hle_record(row, "PASS", (uint32_t)rc, w, 4u);
}

/* One allocation cell: words result (uid or code), block head address and free result. */
static void ks_alloc_cell(uint32_t *w, int partition, int type, SceSize size, void *addr)
{
    const SceUID uid = sceKernelAllocPartitionMemory(partition, "nkksem", type, size, addr);
    w[0] = (uint32_t)uid;
    w[1] = uid > 0 ? (uint32_t)(uintptr_t)sceKernelGetBlockHeadAddr(uid) : HLE_SENTINEL;
    w[2] = uid > 0 ? (uint32_t)sceKernelFreePartitionMemory(uid) : HLE_SENTINEL;
}

/* Rows vmem-alloc-free and vmem-alloc-held (6 words: a 4 KiB Low block of partition 5, then a
   4 KiB Addr block of partition 2 at 0x08400000, each as result, head address, free result),
   while the lock is free and while this thread holds it; vmem-alloc-held's result is the
   TryLock result and vmem-unlock records the Unlock. */
static void ks_vmem_alloc(void)
{
    uint32_t w[6];
    hle_step("vmem-alloc-free");
    ks_alloc_cell(&w[0], 5, PSP_SMEM_Low, 0x1000, NULL);
    ks_alloc_cell(&w[3], 2, PSP_SMEM_Addr, 0x1000, (void *)(uintptr_t)KS_VMEM_BASE);
    hle_record("vmem-alloc-free", "PASS", 0u, w, 6u);
    hle_step("vmem-alloc-held");
    void *addr = NULL;
    int size = 0;
    const int locked = sceKernelVolatileMemTryLock(0, &addr, &size);
    if (locked == 0) {
        ks_alloc_cell(&w[0], 5, PSP_SMEM_Low, 0x1000, NULL);
        ks_alloc_cell(&w[3], 2, PSP_SMEM_Addr, 0x1000, (void *)(uintptr_t)KS_VMEM_BASE);
    } else {
        for (unsigned i = 0; i < 6u; i++) w[i] = HLE_SENTINEL;
    }
    hle_record("vmem-alloc-held", "PASS", (uint32_t)locked, w, 6u);
    uint32_t u = locked == 0 ? (uint32_t)sceKernelVolatileMemUnlock(0) : HLE_SENTINEL;
    hle_record("vmem-unlock", "PASS", u, &u, 1u);
}

/* ---- (d) Memory Stick exclusive create and 64-bit seeks ------------------------------------ */
#define KS_DIR "ms0:/NKKSEM00"
#define KS_FILE KS_DIR "/f.bin"
#define KS_IO_ROWS 7u
static const char *const k_ks_io_rows[KS_IO_ROWS] = {
    "io-missing-parent", "io-mkdir", "io-excl-new", "io-excl-existing", "io-seek64",
    "io-seek64-rdwr", "io-cleanup",
};
static const uint32_t k_ks_io_words[KS_IO_ROWS] = { 4u, 1u, 3u, 5u, 16u, 4u, 3u };

static uint32_t ks_close_if_open(SceUID fd)
{
    return fd >= 0 ? (uint32_t)sceIoClose(fd) : HLE_SENTINEL;
}

static void ks_io(void)
{
    uint32_t w[16];
    hle_step("io-precheck");
    const SceUID pre = sceIoDopen(KS_DIR);
    if (pre >= 0) {
        /* The directory exists: it is not the probe's, so nothing below may touch it. */
        (void)sceIoDclose(pre);
        for (unsigned r = 0; r < KS_IO_ROWS; r++) {
            for (unsigned i = 0; i < k_ks_io_words[r]; i++) w[i] = HLE_SENTINEL;
            hle_record(k_ks_io_rows[r], "SKIP", (uint32_t)pre, w, k_ks_io_words[r]);
        }
        return;
    }

    /* io-missing-parent: O_CREAT|O_EXCL and O_CREAT alone under the missing directory, each
       as result and close result. */
    hle_step("io-missing-parent");
    SceUID fd = sceIoOpen(KS_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    w[0] = (uint32_t)fd;
    w[1] = ks_close_if_open(fd);
    fd = sceIoOpen(KS_FILE, PSP_O_RDWR | PSP_O_CREAT, 0777);
    w[2] = (uint32_t)fd;
    w[3] = ks_close_if_open(fd);
    if ((int)w[0] >= 0 || (int)w[2] >= 0) {
        (void)sceIoRemove(KS_FILE);
    }
    hle_record("io-missing-parent", "PASS", 0u, w, 4u);

    hle_step("io-mkdir");
    const int made = sceIoMkdir(KS_DIR, 0777);
    w[0] = (uint32_t)made;
    hle_record("io-mkdir", "PASS", (uint32_t)made, w, 1u);
    if (made < 0) {
        for (unsigned r = 2; r < KS_IO_ROWS; r++) {
            for (unsigned i = 0; i < k_ks_io_words[r]; i++) w[i] = HLE_SENTINEL;
            hle_record(k_ks_io_rows[r], "SKIP", (uint32_t)made, w, k_ks_io_words[r]);
        }
        return;
    }

    /* io-excl-new: O_CREAT|O_EXCL for the new file, the 16-byte write and the close. */
    hle_step("io-excl-new");
    static const char bytes[16] = { 'n', 'a', 'k', 'a', 'g', 'a', 'w', 'a',
                                     '-', 'k', 's', 'e', 'm', '-', '0', '1' };
    fd = sceIoOpen(KS_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    w[0] = (uint32_t)fd;
    w[1] = fd >= 0 ? (uint32_t)sceIoWrite(fd, bytes, sizeof(bytes)) : HLE_SENTINEL;
    w[2] = ks_close_if_open(fd);
    hle_record("io-excl-new", "PASS", (uint32_t)fd, w, 3u);

    /* io-excl-existing: O_CREAT|O_EXCL, O_CREAT|O_EXCL|O_TRUNC and O_EXCL without O_CREAT on
       the existing file (each opened descriptor closed at once), then the file's size words
       from sceIoGetstat (low, high). */
    hle_step("io-excl-existing");
    fd = sceIoOpen(KS_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    w[0] = (uint32_t)fd;
    (void)ks_close_if_open(fd);
    fd = sceIoOpen(KS_FILE, PSP_O_RDWR | PSP_O_CREAT | PSP_O_EXCL | PSP_O_TRUNC, 0777);
    w[1] = (uint32_t)fd;
    (void)ks_close_if_open(fd);
    fd = sceIoOpen(KS_FILE, PSP_O_RDONLY | PSP_O_EXCL, 0777);
    w[2] = (uint32_t)fd;
    (void)ks_close_if_open(fd);
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    const int gs = sceIoGetstat(KS_FILE, &st);
    w[3] = gs >= 0 ? (uint32_t)((uint64_t)st.st_size & 0xffffffffu) : (uint32_t)gs;
    w[4] = gs >= 0 ? (uint32_t)((uint64_t)st.st_size >> 32) : HLE_SENTINEL;
    hle_record("io-excl-existing", "PASS", 0u, w, 5u);

    /* io-seek64 on a read-only descriptor: Lseek SET 0x100000010 (low, high), Lseek CUR 0
       (low, high), Lseek32 CUR 0, a read of 4 there, Lseek END +0x100000000 (low, high),
       Lseek SET 0xFFFFFFFF (low, high), a read of 4 there, Lseek SET 0 (low, high), Lseek32
       SET 0x7FFFFFFF, a read of 4 there, then the close. */
    hle_step("io-seek64");
    fd = sceIoOpen(KS_FILE, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        uint8_t buf[4];
        SceOff r = sceIoLseek(fd, (SceOff)0x100000010LL, PSP_SEEK_SET);
        w[0] = (uint32_t)((uint64_t)r & 0xffffffffu); w[1] = (uint32_t)((uint64_t)r >> 32);
        r = sceIoLseek(fd, 0, PSP_SEEK_CUR);
        w[2] = (uint32_t)((uint64_t)r & 0xffffffffu); w[3] = (uint32_t)((uint64_t)r >> 32);
        w[4] = (uint32_t)sceIoLseek32(fd, 0, PSP_SEEK_CUR);
        w[5] = (uint32_t)sceIoRead(fd, buf, sizeof(buf));
        r = sceIoLseek(fd, (SceOff)0x100000000LL, PSP_SEEK_END);
        w[6] = (uint32_t)((uint64_t)r & 0xffffffffu); w[7] = (uint32_t)((uint64_t)r >> 32);
        r = sceIoLseek(fd, (SceOff)0xFFFFFFFFLL, PSP_SEEK_SET);
        w[8] = (uint32_t)((uint64_t)r & 0xffffffffu); w[9] = (uint32_t)((uint64_t)r >> 32);
        w[10] = (uint32_t)sceIoRead(fd, buf, sizeof(buf));
        r = sceIoLseek(fd, 0, PSP_SEEK_SET);
        w[11] = (uint32_t)((uint64_t)r & 0xffffffffu); w[12] = (uint32_t)((uint64_t)r >> 32);
        w[13] = (uint32_t)sceIoLseek32(fd, 0x7fffffff, PSP_SEEK_SET);
        w[14] = (uint32_t)sceIoRead(fd, buf, sizeof(buf));
        w[15] = (uint32_t)sceIoClose(fd);
        hle_record("io-seek64", "PASS", (uint32_t)fd, w, 16u);
    } else {
        for (unsigned i = 0; i < 16u; i++) w[i] = HLE_SENTINEL;
        hle_record("io-seek64", "FAIL", (uint32_t)fd, w, 16u);
    }

    /* io-seek64-rdwr on a read-write descriptor (nothing is written): Lseek SET 0x100000010
       (low, high), sceIoGetstat size low afterwards, then the close. */
    hle_step("io-seek64-rdwr");
    fd = sceIoOpen(KS_FILE, PSP_O_RDWR, 0777);
    if (fd >= 0) {
        SceOff r = sceIoLseek(fd, (SceOff)0x100000010LL, PSP_SEEK_SET);
        w[0] = (uint32_t)((uint64_t)r & 0xffffffffu); w[1] = (uint32_t)((uint64_t)r >> 32);
        w[2] = (uint32_t)sceIoClose(fd);
        memset(&st, 0, sizeof(st));
        const int gs2 = sceIoGetstat(KS_FILE, &st);
        w[3] = gs2 >= 0 ? (uint32_t)((uint64_t)st.st_size & 0xffffffffu) : (uint32_t)gs2;
        hle_record("io-seek64-rdwr", "PASS", (uint32_t)fd, w, 4u);
    } else {
        for (unsigned i = 0; i < 4u; i++) w[i] = HLE_SENTINEL;
        hle_record("io-seek64-rdwr", "FAIL", (uint32_t)fd, w, 4u);
    }

    /* io-cleanup: remove the file and the directory, then confirm the directory is gone. */
    hle_step("io-cleanup");
    w[0] = (uint32_t)sceIoRemove(KS_FILE);
    w[1] = (uint32_t)sceIoRmdir(KS_DIR);
    const SceUID post = sceIoDopen(KS_DIR);
    w[2] = (uint32_t)post;
    if (post >= 0) {
        (void)sceIoDclose(post);
    }
    hle_record("io-cleanup", "PASS", 0u, w, 3u);
}

/* ---- (e) WLAN state ------------------------------------------------------------------------ */
/* Row wlan-state (2 words): sceWlanGetSwitchState, sceWlanDevIsPowerOn. */
static void ks_wlan(void)
{
    uint32_t w[2];
    hle_step("wlan-state");
    w[0] = (uint32_t)sceWlanGetSwitchState();
    w[1] = (uint32_t)sceWlanDevIsPowerOn();
    hle_record("wlan-state", "PASS", 0u, w, 2u);
}

static int hle_run(void)
{
    ks_subintr_order();
    ks_ctrl_empty_read();
    ks_ctrl_short_read();
    ks_ctrl_peek_count();
    ks_ctrl_modes();
    ks_ctrl_cycle();
    ks_vmem_type("vmem-trylock-type-1", 0, 1);
    ks_vmem_type("vmem-trylock-type-2", 0, 2);
    ks_vmem_type("vmem-trylock-type-m1", 0, -1);
    ks_vmem_type("vmem-lock-type-1", 1, 1);
    ks_vmem_alloc();
    ks_io();
    ks_wlan();
    return 1;
}

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
