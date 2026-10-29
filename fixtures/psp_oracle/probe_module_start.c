// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Module-start thread cell for the PSP-3000 oracle, one PRX and one launch:
//   CASE=module-start (PSP-MODULE-START-001): once a module's `module_start` returns,
//     does the kernel delete the thread that ran it, and does that free the thread's
//     stack in the user partition? A recompiled-game runtime keeps that thread DORMANT
//     with its 256 KB stack, so a game that asks for nearly all free memory would fail by
//     just that amount. This probe reads the answer off the console.
//
// The PRX has no main thread. Its `module_start` is the probe's own (crt0's, which would
// create a "user_main" thread, is replaced at link time; see "module_start" below) and does
// only this, with no libc call:
//   1. sceKernelGetThreadId and the full sceKernelReferThreadStatus of itself;
//   2. sceKernelTotalFreeMemSize and sceKernelMaxFreeMemSize;
//   3. creates and starts one helper thread (stack 0x4000) at a priority numerically
//      above, that is lower than, its own, so the helper cannot run before module_start
//      returns (it never blocks, so nothing else lets a lower priority thread in);
//   4. reads the two memory sizes again, and returns 0.
// The helper then, with module_start's thread gone or not:
//   1. sleeps 50 ms (sceKernelDelayThread);
//   2. reads sceKernelReferThreadStatus of module_start's thread id (return code and the
//      raw words), the two memory sizes again, and sceKernelGetThreadmanIdList for
//      SCE_KERNEL_TMID_Thread (count, and whether either thread id is in the list);
//   3. only then initialises libc, as crt0 does for a main thread, and writes everything
//      it and module_start recorded, so no file I/O precedes a measurement;
//   4. ends the way the other standalone probes do: the host0 round trip, the final
//      NAKAGAWA_PSP_COMPLETE line, then it parks until module_stop, which wakes it, waits
//      for it to run the CRT's de-initialisation and exit, and deletes it. The helper is
//      therefore the one remaining thread the module owns at the teardown snapshot, as the
//      main thread is for every other probe.
//
// Record stream and log: the same writer and host0 round trip as probe_hle_issues.c,
// whose text is repeated here byte for byte up to hle_finish (tools/test_psp_oracle.py pins
// the two copies; the module lifecycle below is this probe's own). NAKAGAWA_PSP_META, a
// NAKAGAWA_PSP_STEP before each group of records, one NAKAGAWA_PSP_TEST per measurement, a
// `module-start-done` terminal record whose out0 counts the measurements, the host0 round
// trip and a final NAKAGAWA_PSP_COMPLETE line. Words are raw: no value is compared with an
// expected one and every record is PASS. A sceKernelReferThreadStatus out buffer is filled
// with HLE_SENTINEL before the call, so a refused call leaves a visible untouched buffer.

#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_MODULE_START", 0, 1, 0);
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define MS_CASE_MODULE_START 124

#if PSP_ORACLE_CASE == MS_CASE_MODULE_START
#define HLE_CAMPAIGN_ID "module-start"
#define HLE_TEST_ID "PSP-MODULE-START-001"
#define HLE_LOG "host0:/module_start_log.txt"
#else
#error probe_module_start.c has no case for this PSP_ORACLE_CASE
#endif
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

#define HLE_SENTINEL 0xA5A5A5A5u

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

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == MS_CASE_MODULE_START

/* sceKernelReferThreadStatus out buffer: 26 words, 104 bytes. */
#define MS_INFO_WORDS 26u
#define MS_HELPER_STACK 0x4000
#define MS_HELPER_PRIO_STEP 8
#define MS_HELPER_PRIO_MAX 0x77
#define MS_HELPER_PRIO_FALLBACK 0x40
#define MS_HELPER_DELAY_US 50000u
#define MS_ID_LIST_MAX 128u

_Static_assert(sizeof(SceKernelThreadInfo) == MS_INFO_WORDS * 4u,
               "SceKernelThreadInfo is 26 words");
#define MS_INFO_CURRENT_PRIORITY_WORD 16u
_Static_assert(offsetof(SceKernelThreadInfo, currentPriority) ==
                   MS_INFO_CURRENT_PRIORITY_WORD * 4u,
               "currentPriority is word 16");

/* What module_start recorded, for the helper to write once its own measurements are taken. */
static volatile SceUID s_ms_tid = -1;
static volatile int s_ms_refer_rc;
static uint32_t s_ms_info[MS_INFO_WORDS];
static volatile uint32_t s_ms_mem_pre[2];
static volatile int s_ms_create_rc;
static volatile int s_ms_start_rc;
static volatile int s_ms_helper_prio;
static volatile uint32_t s_ms_mem_created[2];

static volatile SceUID s_ms_helper = -1;
static volatile int s_ms_stop_requested;

static void ms_refer(SceUID tid, int *rc, uint32_t *words)
{
    SceKernelThreadInfo info;
    uint32_t *raw = (uint32_t *)&info;
    for (unsigned i = 0; i < MS_INFO_WORDS; i++) {
        raw[i] = HLE_SENTINEL;
    }
    info.size = (SceSize)sizeof(info);
    *rc = sceKernelReferThreadStatus(tid, &info);
    for (unsigned i = 0; i < MS_INFO_WORDS; i++) {
        words[i] = raw[i];
    }
}

static int ms_helper_thread(SceSize args, void *argp);

/* This probe's own module_start. crt0_prx.o defines a module_start too (it creates the
   "user_main" thread), so this definition carries the assembler name __wrap_module_start
   and the build links with --wrap=module_start and -e __wrap_module_start (the Makefile
   case-block): the export table's reference to `module_start` and the ELF entry point both
   land here, and crt0's copy, with its main() call, is never reached. */
int module_start(SceSize args, void *argp) __asm__("__wrap_module_start");

int module_start(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    s_ms_tid = sceKernelGetThreadId();
    int refer_rc = -1;
    ms_refer(s_ms_tid, &refer_rc, s_ms_info);
    s_ms_refer_rc = refer_rc;
    s_ms_mem_pre[0] = (uint32_t)sceKernelTotalFreeMemSize();
    s_ms_mem_pre[1] = (uint32_t)sceKernelMaxFreeMemSize();

    int prio = MS_HELPER_PRIO_FALLBACK;
    if (refer_rc >= 0) {
        prio = (int)s_ms_info[MS_INFO_CURRENT_PRIORITY_WORD] + MS_HELPER_PRIO_STEP;
        if (prio > MS_HELPER_PRIO_MAX) {
            prio = MS_HELPER_PRIO_MAX;
        }
    }
    s_ms_helper_prio = prio;
    const SceUID helper = sceKernelCreateThread("ms_helper", ms_helper_thread, prio,
                                                MS_HELPER_STACK, THREAD_ATTR_USER, NULL);
    s_ms_create_rc = helper;
    s_ms_start_rc = -1;
    if (helper >= 0) {
        s_ms_helper = helper;
        s_ms_start_rc = sceKernelStartThread(helper, 0, NULL);
    }
    s_ms_mem_created[0] = (uint32_t)sceKernelTotalFreeMemSize();
    s_ms_mem_created[1] = (uint32_t)sceKernelMaxFreeMemSize();
    return 0;
}

/* crt0_prx.o still references main (its _main calls it), and crti/crtbegin supply _init and
   _fini, so the object must link; nothing calls main. */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

extern void _init(void);
extern void _fini(void);
extern void __libcglue_init(int argc, char *argv[]);
extern void __libcglue_deinit(void);
#define MS_STOP_TIMEOUT_US 1000000u

int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    const SceUID helper = s_ms_helper;
    if (helper < 0) {
        return 1;
    }
    s_ms_stop_requested = 1;
    (void)sceKernelWakeupThread(helper);
    SceUInt timeout = MS_STOP_TIMEOUT_US;
    if (sceKernelWaitThreadEnd(helper, &timeout) < 0) {
        return 1;
    }
    if (sceKernelDeleteThread(helper) < 0) {
        return 1;
    }
    s_ms_helper = -1;
    return 0;
}

static void ms_park_until_stop(void)
{
    while (!s_ms_stop_requested) {
        sceKernelSleepThread();
    }
    _fini();
    __libcglue_deinit();
    sceKernelExitThread(0);
}

/* One refer-status record: out0 = the call's return code, out1.. = the 26 raw words. */
static void ms_emit_refer(const char *case_id, uint32_t result, int rc, const uint32_t *words)
{
    uint32_t out[1 + MS_INFO_WORDS];
    out[0] = (uint32_t)rc;
    for (unsigned i = 0; i < MS_INFO_WORDS; i++) {
        out[1 + i] = words[i];
    }
    hle_record(case_id, "PASS", result, out, 1u + MS_INFO_WORDS);
}

static int ms_helper_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    static char *no_argv[1] = {NULL};

    sceKernelDelayThread(MS_HELPER_DELAY_US);

    /* The helper's measurements, taken before any libc or file call. */
    uint32_t after_words[MS_INFO_WORDS];
    int after_rc = -1;
    ms_refer(s_ms_tid, &after_rc, after_words);
    uint32_t mem_after[2];
    mem_after[0] = (uint32_t)sceKernelTotalFreeMemSize();
    mem_after[1] = (uint32_t)sceKernelMaxFreeMemSize();
    SceUID ids[MS_ID_LIST_MAX];
    for (unsigned i = 0; i < MS_ID_LIST_MAX; i++) {
        ids[i] = 0;
    }
    int id_count = -1;
    const int id_rc = sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids,
                                                  (int)MS_ID_LIST_MAX, &id_count);
    uint32_t id_stored = 0;
    uint32_t old_in_list = 0;
    uint32_t helper_in_list = 0;
    if (id_rc >= 0 && id_count > 0) {
        id_stored = (uint32_t)id_count < MS_ID_LIST_MAX ? (uint32_t)id_count : MS_ID_LIST_MAX;
        for (uint32_t i = 0; i < id_stored; i++) {
            old_in_list |= (ids[i] == s_ms_tid) ? 1u : 0u;
            helper_in_list |= (ids[i] == s_ms_helper) ? 1u : 0u;
        }
    }
    const SceUID self_tid = sceKernelGetThreadId();
    uint32_t self_words[MS_INFO_WORDS];
    int self_rc = -1;
    ms_refer(self_tid, &self_rc, self_words);

    /* Now the CRT's start-up for a thread that is not a main thread, as crt0's _main does
       before main(): libc, then the constructors. The log and its round trip need libc. */
    __libcglue_init(0, no_argv);
    _init();
    hle_begin();

    hle_step("module-start-thread");
    ms_emit_refer("ms-self", (uint32_t)s_ms_tid, s_ms_refer_rc, s_ms_info);
    {
        uint32_t out[2] = {s_ms_mem_pre[0], s_ms_mem_pre[1]};
        hle_record("ms-mem-pre", "PASS", 0, out, 2);
    }
    {
        uint32_t out[3] = {(uint32_t)s_ms_create_rc, (uint32_t)s_ms_helper_prio,
                           (uint32_t)MS_HELPER_STACK};
        hle_record("ms-helper-create", "PASS", (uint32_t)s_ms_start_rc, out, 3);
    }
    {
        uint32_t out[2] = {s_ms_mem_created[0], s_ms_mem_created[1]};
        hle_record("ms-mem-created", "PASS", 0, out, 2);
    }

    hle_step("helper-after-return");
    ms_emit_refer("ms-refer-after", (uint32_t)after_rc, after_rc, after_words);
    hle_record("ms-mem-after", "PASS", 0, mem_after, 2);
    {
        uint32_t out[6] = {(uint32_t)id_count, id_stored, old_in_list, helper_in_list,
                           (uint32_t)s_ms_tid, (uint32_t)s_ms_helper};
        hle_record("ms-idlist", "PASS", (uint32_t)id_rc, out, 6);
    }
    ms_emit_refer("ms-helper-self", (uint32_t)self_tid, self_rc, self_words);

    hle_finish(1);
    ms_park_until_stop();
    return 0;
}

#endif /* MS_CASE_MODULE_START */
