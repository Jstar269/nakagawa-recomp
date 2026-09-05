// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspdmac.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <psputils.h>
#include <pspctrl.h>
#include <psprtc.h>
#include <pspaudio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("NAKAGAWA_PSP_ORACLE", 0, 1, 0);

#define FIXTURE_BUILD_ID "nakagawa-psp-oracle-v1"

#ifndef PSP_ORACLE_CASE
#define PSP_ORACLE_CASE 0
#endif

#define PSP_ORACLE_CASE_SMOKE 0
#define PSP_ORACLE_CASE_CALLBACK 1
#define PSP_ORACLE_CASE_WAIT_CANCEL 2
#define PSP_ORACLE_CASE_THREAD_LIFECYCLE 3
#define PSP_ORACLE_CASE_THREAD_DELETE 4
#define PSP_ORACLE_CASE_THREAD_DELETE_FOLLOWUP 5
#define PSP_ORACLE_CASE_THREAD_DELETE_EXPLICIT 6
#define PSP_ORACLE_CASE_THREAD_DELETE_BOUNDARY 7
#define PSP_ORACLE_CASE_DMAC_CONCURRENCY 8
#define PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST 9
#define PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_SRC 10
#define PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_DST 11
#define PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC 12
#define PSP_ORACLE_CASE_DISPLAY_MASK_VCOUNT 13
#define PSP_ORACLE_CASE_DISPLAY_MASK_DUTY 14
#define PSP_ORACLE_CASE_DISPLAY_GE_MASK 15
#define PSP_ORACLE_CASE_TRANSPORT_WRITE 16
#define PSP_ORACLE_CASE_THREAD_EXIT_DELETE 17
#define PSP_ORACLE_CASE_DMAC_SURVEY 18
#define PSP_ORACLE_CASE_CTRL_CLOCK 19
#define PSP_ORACLE_CASE_FPU_VECTOR 20
#define PSP_ORACLE_CASE_TEARDOWN_TEST 21
#define PSP_ORACLE_CASE_IO_MATRIX 22
#define PSP_ORACLE_CASE_AUDIO_QUERY 23
#define PSP_ORACLE_CASE_CACHE_ALIAS 24

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY
PSP_MAIN_THREAD_PARAMS(0x20, 32, THREAD_ATTR_USER);
#else
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST && \
    PSP_ORACLE_CASE <= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC
/* The default newlib heap claims the largest free partition block. A bounded
   heap leaves the high-address system-memory allocation available to the
   invalid-tail probe without relying on unowned memory. */
PSP_HEAP_SIZE_KB(512);
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SURVEY
/* Same bounded heap as the tails: the default heap demonstrably squats the
   surveyed range (32/32 clean-fails), while the bounded one leaves the free
   pool observable. Survey and tails must share heap geometry or their
   results are incomparable. */
PSP_HEAP_SIZE_KB(512);
#endif

/* PPSSPP exposes a pseudo-device that headless builds use to capture test
   output; see Core/HLE/sceIo.cpp. Real hardware has no such device and the
   devctl simply fails, which is how the probe tells the two apart. The same
   record text is emitted either way, but the `source=` field differs so an
   emulator capture can never be compared as if it were hardware. */
#define EMULATOR_DEVCTL_SEND_OUTPUT 2
#define EMULATOR_DEVCTL_IS_EMULATOR 3

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_SMOKE
/* Keep this arithmetic body as a distinct guest function.  The Nakagawa smoke
   route recompiles this same PSP ELF and executes the generated function; an
   inlined host-side copy would not be an oracle producer. */
__attribute__((noinline)) uint32_t nakagawa_psp_oracle_sum_u32(uint32_t count) {
    uint32_t sum = 0;
    for (uint32_t value = 1; value <= count; ++value) {
        sum += value;
    }
    return sum;
}
#endif

static int emulator_present(void) {
    uint32_t flag = 0;
    if (sceIoDevctl("emulator:", EMULATOR_DEVCTL_IS_EMULATOR, NULL, 0, &flag, sizeof(flag)) < 0) {
        return 0;
    }
    return flag == 1;
}

static void emit(int emulated, const char *text) {
    if (emulated) {
        sceIoDevctl("emulator:", EMULATOR_DEVCTL_SEND_OUTPUT, (void *)text, (int)strlen(text), NULL, 0);
    } else {
        printf("%s", text);
        fflush(stdout);
    }
}

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR
#define PROBE_HOST0_LOG "host0:/fpu_vector_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_IO_MATRIX
#define PROBE_HOST0_LOG "host0:/io_matrix_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY
#define PROBE_HOST0_LOG "host0:/audio_query_log.txt"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_CACHE_ALIAS
#define PROBE_HOST0_LOG "host0:/cache_alias_log.txt"
#endif

#if PSP_ORACLE_CASE != PSP_ORACLE_CASE_SMOKE
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CALLBACK || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_WAIT_CANCEL || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_LIFECYCLE
static void emit_test(int emulated, const char *case_id, int pass,
                      uint32_t result, uint32_t out0, uint32_t out1,
                      uint32_t out2, uint32_t out3) {
    char line[320];
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-KERNEL-001 case_id=%s "
             "status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x\n",
             case_id, pass ? "PASS" : "FAIL", (unsigned int)result,
             (unsigned int)out0, (unsigned int)out1,
             (unsigned int)out2, (unsigned int)out3);
    emit(emulated, line);
}
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_THREAD_DELETE
static void emit_record_extended(int emulated, const char *test_id,
                                 const char *case_id, const char *status,
                                 uint32_t result, const uint32_t *out,
                                 size_t out_count) {
    char line[1024];
    int used = snprintf(line, sizeof(line),
                        "NAKAGAWA_PSP_TEST schema=1 test_id=%s "
                        "case_id=%s status=%s result=0x%08x",
                        test_id, case_id, status, (unsigned int)result);
    for (size_t i = 0; i < out_count && used > 0 && (size_t)used < sizeof(line); i++) {
        int wrote = snprintf(line + used, sizeof(line) - (size_t)used,
                             " out%u=0x%08x", (unsigned int)i,
                             (unsigned int)out[i]);
        if (wrote < 0) break;
        used += wrote;
    }
    if (used > 0 && (size_t)used + 1 < sizeof(line)) {
        line[used++] = '\n';
        line[used] = '\0';
    }
    emit(emulated, line);
#ifdef PROBE_HOST0_LOG
    if (!emulated) {
        SceUID fd = sceIoOpen(PROBE_HOST0_LOG,
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, line, strlen(line));
            sceIoClose(fd);
        }
    }
#endif
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_IO_MATRIX || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_CACHE_ALIAS
/* Deferred record buffer.
   The IO matrix measures IoFileMgr itself and the cache matrix measures
   dcache line residency across cells.  Emitting a record inside the measured
   window would perturb the very state under test: `emit_record_extended`
   opens, writes and closes a `host0:` descriptor, which allocates a fresh
   SceUID from the same IoFileMgr namespace the IO probe samples, and drives a
   blocking USB round trip whose kernel and DMA footprint can evict the
   `s_cache_buf` line whose residency the cache probe samples between cells.

   Records are therefore accumulated as raw values -- no formatting, no
   syscall, no allocation -- and emitted only after every measurement cell has
   run.  Capacity is a compile-time constant checked against the largest cell
   count either probe can emit, so overflow is impossible by construction
   rather than handled at runtime. */
#define DEFERRED_MAX_RECORDS 8
#define DEFERRED_MAX_OUT 6

struct deferred_record {
    const char *case_id; /* string literal; never freed */
    const char *status;  /* "PASS" / "FAIL" literal */
    uint32_t result;
    uint32_t out[DEFERRED_MAX_OUT];
    size_t out_count;
};

static struct deferred_record s_deferred[DEFERRED_MAX_RECORDS];
static size_t s_deferred_count;

static void defer_record(const char *case_id, const char *status,
                         uint32_t result, const uint32_t *out,
                         size_t out_count) {
    struct deferred_record *rec = &s_deferred[s_deferred_count++];
    rec->case_id = case_id;
    rec->status = status;
    rec->result = result;
    for (size_t i = 0; i < out_count; i++) {
        rec->out[i] = out[i];
    }
    rec->out_count = out_count;
}

/* Emit every buffered record in capture order.  Called only after the last
   measurement cell has completed, so no emission touches measured state. */
static void flush_deferred(int emulated, const char *test_id) {
    for (size_t i = 0; i < s_deferred_count; i++) {
        emit_record_extended(emulated, test_id, s_deferred[i].case_id,
                             s_deferred[i].status, s_deferred[i].result,
                             s_deferred[i].out, s_deferred[i].out_count);
    }
}
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_THREAD_DELETE && \
    PSP_ORACLE_CASE <= PSP_ORACLE_CASE_THREAD_DELETE_BOUNDARY
static void emit_test_extended(int emulated, const char *case_id, int pass,
                               uint32_t result, const uint32_t *out,
                               size_t out_count) {
    emit_record_extended(emulated, "PSP-KERNEL-001", case_id,
                         pass ? "PASS" : "FAIL", result, out, out_count);
}
#endif
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CALLBACK
static volatile int s_callback_calls;
static volatile int s_callback_arg1;
static volatile int s_callback_arg2;

static int oracle_callback(int arg1, int arg2, void *common) {
    (void)common;
    s_callback_calls++;
    s_callback_arg1 = arg1;
    s_callback_arg2 = arg2;
    return 0;
}

static int run_callback_case(uint32_t *out0, uint32_t *out1,
                             uint32_t *out2, uint32_t *out3) {
    s_callback_calls = 0;
    s_callback_arg1 = 0;
    s_callback_arg2 = 0;
    const SceUID cbid = sceKernelCreateCallback("oracle-callback", oracle_callback, NULL);
    if (cbid < 0) {
        *out0 = 0;
        *out1 = (uint32_t)cbid;
        return 0;
    }
    const int notify_first = sceKernelNotifyCallback(cbid, 0x1234);
    const int count_before = sceKernelGetCallbackCount(cbid);
    const int check = sceKernelCheckCallback();
    const int count_after = sceKernelGetCallbackCount(cbid);
    const int notify_second = sceKernelNotifyCallback(cbid, 0x5678);
    const int cancel = sceKernelCancelCallback(cbid);
    const int count_cancelled = sceKernelGetCallbackCount(cbid);
    const int delete_result = sceKernelDeleteCallback(cbid);

    /* Normalize control-flow predicates to bits so UIDs do not enter the comparison
       stream.  The raw cancellation/deletion returns remain in out2/out3 so an
       acceptance comparison also checks the PSP error-code contract. */
    *out0 = (uint32_t)(notify_first == 0) |
            ((uint32_t)(count_before == 1) << 1) |
            ((uint32_t)(check > 0) << 2) |
            ((uint32_t)(count_after == 0) << 3) |
            ((uint32_t)(notify_second == 0) << 4) |
            ((uint32_t)(cancel == 0) << 5) |
            ((uint32_t)(count_cancelled == 0) << 6) |
            ((uint32_t)(delete_result == 0) << 7) |
            ((uint32_t)(s_callback_calls == 1) << 8);
    *out0 |= ((uint32_t)(count_cancelled & 0xff) << 16);
    *out1 = ((uint32_t)(s_callback_arg1 & 0xffff) << 16) |
            (uint32_t)(s_callback_arg2 & 0xffff);
    *out2 = (uint32_t)cancel;
    *out3 = (uint32_t)delete_result;
    return notify_first == 0 && count_before == 1 && check > 0 && count_after == 0 &&
           notify_second == 0 && cancel == 0 && count_cancelled == 0 &&
           delete_result == 0 && s_callback_calls == 1;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_WAIT_CANCEL
static uint32_t run_wait_cancel_case(uint32_t *out0, uint32_t *out1,
                                     uint32_t *out2, uint32_t *out3) {
    const SceUID semaid = sceKernelCreateSema("oracle-sema", 0, 0, 1, NULL);
    if (semaid < 0) {
        *out0 = 0;
        *out1 = (uint32_t)semaid;
        return 0;
    }
    const int empty = sceKernelPollSema(semaid, 1);
    const int signal = sceKernelSignalSema(semaid, 1);
    const int ready = sceKernelPollSema(semaid, 1);
    const int empty_again = sceKernelPollSema(semaid, 1);
    const int delete_result = sceKernelDeleteSema(semaid);
    *out0 = (uint32_t)(empty < 0) |
            ((uint32_t)(signal == 0) << 1) |
            ((uint32_t)(ready == 0) << 2) |
            ((uint32_t)(empty_again < 0) << 3) |
            ((uint32_t)(delete_result == 0) << 4);
    *out1 = 0;
    *out2 = (uint32_t)empty;
    *out3 = (uint32_t)empty_again;
    return *out0 == 0x1fu;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_LIFECYCLE
static int oracle_thread_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0x42;
}

static uint32_t run_thread_lifecycle_case(uint32_t *out0, uint32_t *out1,
                                          uint32_t *out2, uint32_t *out3) {
    const SceUID thid = sceKernelCreateThread("oracle-thread", oracle_thread_entry,
                                              0x20, 0x4000, 0, NULL);
    if (thid < 0) {
        *out0 = 0;
        *out1 = (uint32_t)thid;
        return 0;
    }
    const int start = sceKernelStartThread(thid, 0, NULL);
    const int wait = sceKernelWaitThreadEnd(thid, NULL);
    const int exit_status = sceKernelGetThreadExitStatus(thid);
    const int delete_result = sceKernelDeleteThread(thid);
    const int post_delete_status = sceKernelGetThreadExitStatus(thid);
    *out0 = (uint32_t)(start == 0) |
            ((uint32_t)(wait == 0x42) << 1) |
            ((uint32_t)(delete_result == 0) << 2) |
            ((uint32_t)(post_delete_status < 0) << 3);
    *out1 = ((uint32_t)exit_status & 0xffffu) |
            ((uint32_t)wait << 16);
    *out2 = (uint32_t)post_delete_status;
    *out3 = (uint32_t)delete_result;
    return start == 0 && wait >= 0 && delete_result == 0 && post_delete_status < 0 &&
           exit_status == 0x42;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE
static volatile SceUID s_join_target;
static volatile int s_join_result;

static int oracle_sleep_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelSleepThread();
    return 0x55;
}

static int oracle_exit_delete_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelExitDeleteThread(0x66);
    return -1;
}

static int oracle_join_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    s_join_result = sceKernelWaitThreadEnd(s_join_target, NULL);
    return s_join_result;
}

static uint32_t run_thread_delete_case(uint32_t *out0, uint32_t *out1,
                                       uint32_t *out2, uint32_t *out3,
                                       uint32_t *out4, uint32_t *out5,
                                       uint32_t *out6, uint32_t *out7,
                                       uint32_t *out8, uint32_t *out9) {
    const int invalid_delete = sceKernelDeleteThread(0x7fffffff);
    const int current_delete = sceKernelDeleteThread(sceKernelGetThreadId());

    const SceUID term_target = sceKernelCreateThread("oracle-term", oracle_sleep_entry,
                                                     0x30, 0x4000, 0, NULL);
    const SceUID term_joiner = sceKernelCreateThread("oracle-term-join", oracle_join_entry,
                                                     0x20, 0x4000, 0, NULL);
    s_join_target = term_target;
    s_join_result = 0;
    const int term_start = term_target < 0 ? term_target : sceKernelStartThread(term_target, 0, NULL);
    const int term_join_start = term_joiner < 0 ? term_joiner : sceKernelStartThread(term_joiner, 0, NULL);
    if (term_joiner >= 0) sceKernelDelayThread(1000);
    const int term_delete = term_target < 0 ? term_target : sceKernelTerminateDeleteThread(term_target);
    const int term_join_wait = term_joiner < 0 ? term_joiner : sceKernelWaitThreadEnd(term_joiner, NULL);
    const int term_join_result = s_join_result;
    const int term_post_status = term_target < 0 ? term_target : sceKernelGetThreadExitStatus(term_target);
    const int term_post_start = term_target < 0 ? term_target : sceKernelStartThread(term_target, 0, NULL);
    const int term_post_wake = term_target < 0 ? term_target : sceKernelWakeupThread(term_target);
    if (term_joiner >= 0) sceKernelDeleteThread(term_joiner);

    const SceUID exit_target = sceKernelCreateThread("oracle-exit-delete", oracle_exit_delete_entry,
                                                     0x30, 0x4000, 0, NULL);
    const SceUID exit_joiner = sceKernelCreateThread("oracle-exit-join", oracle_join_entry,
                                                     0x20, 0x4000, 0, NULL);
    s_join_target = exit_target;
    s_join_result = 0;
    const int exit_start = exit_target < 0 ? exit_target : sceKernelStartThread(exit_target, 0, NULL);
    const int exit_join_start = exit_joiner < 0 ? exit_joiner : sceKernelStartThread(exit_joiner, 0, NULL);
    if (exit_joiner >= 0) sceKernelDelayThread(1000);
    const int exit_join_wait = exit_joiner < 0 ? exit_joiner : sceKernelWaitThreadEnd(exit_joiner, NULL);
    const int exit_join_result = s_join_result;
    const int exit_post_status = exit_target < 0 ? exit_target : sceKernelGetThreadExitStatus(exit_target);
    if (exit_joiner >= 0) sceKernelDeleteThread(exit_joiner);

    *out0 = (uint32_t)(invalid_delete < 0) |
            ((uint32_t)((uint32_t)current_delete == 0x800201a4u) << 1) |
            ((uint32_t)(term_target >= 0 && term_start == 0 && term_join_start == 0) << 2) |
            ((uint32_t)(term_delete == 0) << 3) |
            ((uint32_t)((uint32_t)term_join_result == 0x800201acu) << 4) |
            ((uint32_t)((uint32_t)term_join_wait == 0x800201acu) << 5) |
            ((uint32_t)((uint32_t)term_post_status == 0x80020198u) << 6) |
            ((uint32_t)((uint32_t)term_post_start == 0x80020198u) << 7) |
            ((uint32_t)((uint32_t)term_post_wake == 0x80020198u) << 8) |
            ((uint32_t)(exit_target >= 0 && exit_start == 0 && exit_join_start == 0) << 9) |
            ((uint32_t)(exit_join_result == 0x66) << 10) |
            ((uint32_t)(exit_join_wait == 0x66) << 11) |
            ((uint32_t)((uint32_t)exit_post_status == 0x80020198u) << 12);
    *out1 = (uint32_t)invalid_delete;
    *out2 = (uint32_t)current_delete;
    *out3 = (uint32_t)term_delete;
    *out4 = (uint32_t)term_join_result;
    *out5 = (uint32_t)term_post_status;
    *out6 = (uint32_t)exit_join_result;
    *out7 = (uint32_t)exit_post_status;
    *out8 = (uint32_t)exit_join_wait;
    /* Diagnostic: bit 5 of out0 compares term_join_wait against
       SCE_KERNEL_ERROR_THREAD_TERMINATED, but the raw value was never
       emitted, so a hardware DIFFERENCE said only "not 0x800201ac".
       Emit it so the PSP's actual second-order join result is readable. */
    *out9 = (uint32_t)term_join_wait;
    return *out0 == 0x1fffu;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_FOLLOWUP || PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_EXPLICIT || PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_BOUNDARY
/* This is the smallest control that separates the two live explanations for
   the second-order wait discrepancy.  Both joiners wait on a target that is
   terminate-deleted and both receive THREAD_TERMINATED from the inner wait.
   The semaphores prove that the inner wait was entered before deletion and
   returned before the outer wait and ReferThreadStatus are sampled.  The
   follow-up case uses implicit entry returns; the explicit sibling calls
   sceKernelExitThread with the same two status shapes. */
static volatile SceUID s_followup_join_target;
static volatile int s_followup_join_result;
static volatile int s_followup_join_exit_mode;
static volatile SceUID s_followup_waiting_sema;
static volatile SceUID s_followup_done_sema;

static int followup_sleep_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelSleepThread();
    return 0x55;
}

static int followup_join_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (s_followup_waiting_sema >= 0) {
        sceKernelSignalSema(s_followup_waiting_sema, 1);
    }
    s_followup_join_result = sceKernelWaitThreadEnd(s_followup_join_target, NULL);
    if (s_followup_done_sema >= 0) {
        sceKernelSignalSema(s_followup_done_sema, 1);
    }
    const int exit_mode = s_followup_join_exit_mode;
    if (exit_mode == 2) {
        (void)sceKernelExitThread((int)0x800201acu);
        return 0;
    }
    if (exit_mode == 3) {
        (void)sceKernelExitThread(0x78);
        return 0;
    }
    if (exit_mode == 4) {
        (void)sceKernelExitThread((int)0x800201a8u);
        return 0;
    }
    if (exit_mode == 5) {
        (void)sceKernelExitThread(-17);
        return 0;
    }
    return exit_mode == 1 ? 0x77 : s_followup_join_result;
}

static int followup_refer_status(SceUID thid, uint32_t *ret,
                                 uint32_t *status, uint32_t *wait_type,
                                 uint32_t *wait_id, uint32_t *exit_status) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    const int result = sceKernelReferThreadStatus(thid, &info);
    *ret = (uint32_t)result;
    *status = (uint32_t)info.status;
    *wait_type = (uint32_t)info.waitType;
    *wait_id = (uint32_t)info.waitId;
    *exit_status = (uint32_t)info.exitStatus;
    return result;
}

static uint32_t run_thread_delete_followup_case(uint32_t *out0, uint32_t *out1,
                                                uint32_t *out2, uint32_t *out3,
                                                uint32_t *out4, uint32_t *out5,
                                                uint32_t *out6, uint32_t *out7,
                                                uint32_t *out8, uint32_t *out9,
                                                uint32_t *out10, uint32_t *out11,
                                                uint32_t *out12, uint32_t *out13,
                                                uint32_t *out14,
                                                uint32_t *out15, uint32_t *out16,
                                                int exit_variant) {
    const int boundary = exit_variant == 2;
    const int explicit_exit = exit_variant != 0;
    const uint32_t expected_error_exit = 0x800200d2u;
    const uint32_t explicit_error = boundary ? 0x800201a8u : 0x800201acu;
    const uint32_t positive_exit_argument = boundary ? 0xfffffffefu :
                                                  (explicit_exit ? 0x78u : 0x77u);
    const uint32_t expected_positive_exit = boundary ? expected_error_exit :
                                                   positive_exit_argument;
    const int error_exit_mode = boundary ? 4 : (explicit_exit ? 2 : 0);
    const int positive_exit_mode = boundary ? 5 : (explicit_exit ? 3 : 1);
    const SceUID error_target = sceKernelCreateThread("oracle-follow-error-target",
                                                       followup_sleep_entry,
                                                       0x30, 0x4000, 0, NULL);
    const SceUID error_joiner = sceKernelCreateThread("oracle-follow-error-joiner",
                                                       followup_join_entry,
                                                       0x10, 0x4000, 0, NULL);
    const SceUID error_waiting_sema = sceKernelCreateSema("oracle-follow-error-waiting",
                                                          0, 0, 1, NULL);
    const SceUID error_done_sema = sceKernelCreateSema("oracle-follow-error-done",
                                                       0, 0, 1, NULL);
    s_followup_join_target = error_target;
    s_followup_join_result = 0;
    s_followup_join_exit_mode = error_exit_mode;
    s_followup_waiting_sema = error_waiting_sema;
    s_followup_done_sema = error_done_sema;
    const int error_sema_ok = error_waiting_sema >= 0 && error_done_sema >= 0;
    const int error_start = !error_sema_ok || error_target < 0
        ? -1 : sceKernelStartThread(error_target, 0, NULL);
    const int error_join_start = !error_sema_ok || error_joiner < 0
        ? -1 : sceKernelStartThread(error_joiner, 0, NULL);
    const int error_waiting = error_join_start < 0
        ? error_join_start : sceKernelWaitSema(error_waiting_sema, 1, NULL);
    const int error_delete = error_waiting < 0
        ? error_waiting : sceKernelTerminateDeleteThread(error_target);
    const int error_done = error_delete < 0 || error_joiner < 0
        ? error_delete : sceKernelWaitSema(error_done_sema, 1, NULL);
    const int error_inner = s_followup_join_result;

    uint32_t error_ref = 0;
    uint32_t error_status = 0;
    uint32_t error_wait_type = 0;
    uint32_t error_wait_id = 0;
    uint32_t error_exit_status = 0;
    const int error_outer = error_done < 0
        ? error_done
        : (error_joiner < 0 ? error_joiner : sceKernelWaitThreadEnd(error_joiner, NULL));
    const int error_ref_result = error_joiner < 0
        ? error_joiner
        : followup_refer_status(error_joiner, &error_ref, &error_status,
                                &error_wait_type, &error_wait_id, &error_exit_status);
    if (error_joiner >= 0) sceKernelDeleteThread(error_joiner);
    if (error_waiting_sema >= 0) sceKernelDeleteSema(error_waiting_sema);
    if (error_done_sema >= 0) sceKernelDeleteSema(error_done_sema);
    s_followup_waiting_sema = -1;
    s_followup_done_sema = -1;

    const SceUID positive_target = sceKernelCreateThread("oracle-follow-positive-target",
                                                          followup_sleep_entry,
                                                          0x30, 0x4000, 0, NULL);
    const SceUID positive_joiner = sceKernelCreateThread("oracle-follow-positive-joiner",
                                                          followup_join_entry,
                                                          0x10, 0x4000, 0, NULL);
    const SceUID positive_waiting_sema = sceKernelCreateSema("oracle-follow-positive-waiting",
                                                              0, 0, 1, NULL);
    const SceUID positive_done_sema = sceKernelCreateSema("oracle-follow-positive-done",
                                                           0, 0, 1, NULL);
    s_followup_join_target = positive_target;
    s_followup_join_result = 0;
    s_followup_join_exit_mode = positive_exit_mode;
    s_followup_waiting_sema = positive_waiting_sema;
    s_followup_done_sema = positive_done_sema;
    const int positive_sema_ok = positive_waiting_sema >= 0 && positive_done_sema >= 0;
    const int positive_start = !positive_sema_ok || positive_target < 0
        ? -1 : sceKernelStartThread(positive_target, 0, NULL);
    const int positive_join_start = !positive_sema_ok || positive_joiner < 0
        ? -1 : sceKernelStartThread(positive_joiner, 0, NULL);
    const int positive_waiting = positive_join_start < 0
        ? positive_join_start : sceKernelWaitSema(positive_waiting_sema, 1, NULL);
    const int positive_delete = positive_waiting < 0
        ? positive_waiting : sceKernelTerminateDeleteThread(positive_target);
    const int positive_done = positive_delete < 0 || positive_joiner < 0
        ? positive_delete : sceKernelWaitSema(positive_done_sema, 1, NULL);
    const int positive_inner = s_followup_join_result;

    uint32_t positive_ref = 0;
    uint32_t positive_status = 0;
    uint32_t positive_wait_type = 0;
    uint32_t positive_wait_id = 0;
    uint32_t positive_exit_status = 0;
    const int positive_outer = positive_done < 0
        ? positive_done
        : (positive_joiner < 0 ? positive_joiner : sceKernelWaitThreadEnd(positive_joiner, NULL));
    const int positive_ref_result = positive_joiner < 0
        ? positive_joiner
        : followup_refer_status(positive_joiner, &positive_ref, &positive_status,
                                &positive_wait_type, &positive_wait_id, &positive_exit_status);
    if (positive_joiner >= 0) sceKernelDeleteThread(positive_joiner);
    if (positive_waiting_sema >= 0) sceKernelDeleteSema(positive_waiting_sema);
    if (positive_done_sema >= 0) sceKernelDeleteSema(positive_done_sema);
    s_followup_waiting_sema = -1;
    s_followup_done_sema = -1;

    /* The mask records setup, semaphore handshakes, inner results, the
       measured negative-return normalization, and status-query observations.
       The raw fields remain in the record so a new firmware/model can expose
       a divergence without losing the observed scalars. */
    *out0 = (uint32_t)(error_target >= 0 && error_start == 0) |
            ((uint32_t)(error_joiner >= 0 && error_join_start == 0) << 1) |
            ((uint32_t)(error_delete == 0) << 2) |
            ((uint32_t)((uint32_t)error_inner == 0x800201acu) << 3) |
            ((uint32_t)(error_ref_result == 0) << 4) |
            ((uint32_t)(error_status == PSP_THREAD_STOPPED) << 5) |
            ((uint32_t)(error_wait_type == 0) << 6) |
             ((uint32_t)((uint32_t)error_exit_status == expected_error_exit) << 7) |
            ((uint32_t)(positive_target >= 0 && positive_start == 0) << 8) |
            ((uint32_t)(positive_joiner >= 0 && positive_join_start == 0) << 9) |
            ((uint32_t)(positive_delete == 0) << 10) |
            ((uint32_t)((uint32_t)positive_inner == 0x800201acu) << 11) |
            ((uint32_t)(positive_ref_result == 0) << 12) |
            ((uint32_t)(positive_status == PSP_THREAD_STOPPED) << 13) |
            ((uint32_t)(positive_wait_type == 0) << 14) |
             ((uint32_t)((uint32_t)positive_exit_status == expected_positive_exit) << 15) |
            ((uint32_t)(error_waiting == 0) << 16) |
            ((uint32_t)(error_done == 0) << 17) |
            ((uint32_t)(positive_waiting == 0) << 18) |
            ((uint32_t)(positive_done == 0) << 19) |
            ((uint32_t)(explicit_exit && (uint32_t)error_outer == expected_error_exit) << 20) |
            ((uint32_t)(explicit_exit && (uint32_t)positive_outer == expected_positive_exit) << 21);
    *out1 = (uint32_t)error_inner;
    *out2 = (uint32_t)error_outer;
    *out3 = (uint32_t)error_ref;
    *out4 = error_status;
    *out5 = error_wait_type;
    *out6 = error_wait_id;
    *out7 = error_exit_status;
    *out8 = (uint32_t)positive_inner;
    *out9 = (uint32_t)positive_outer;
    *out10 = (uint32_t)positive_ref;
    *out11 = positive_status;
    *out12 = positive_wait_type;
    *out13 = positive_wait_id;
    *out14 = positive_exit_status;
    *out15 = boundary ? explicit_error : 0u;
    *out16 = boundary ? positive_exit_argument : 0u;
    return error_target >= 0 && error_start == 0 &&
           error_joiner >= 0 && error_join_start == 0 && error_delete == 0 &&
           error_waiting == 0 && error_done == 0 &&
           (uint32_t)error_inner == 0x800201acu && error_ref_result == 0 &&
           (uint32_t)error_outer == expected_error_exit && error_status == PSP_THREAD_STOPPED &&
           error_wait_type == 0 && error_wait_id == 0 &&
           error_exit_status == expected_error_exit &&
           positive_target >= 0 && positive_start == 0 &&
           positive_joiner >= 0 && positive_join_start == 0 && positive_delete == 0 &&
           positive_waiting == 0 && positive_done == 0 &&
           (uint32_t)positive_inner == 0x800201acu && positive_ref_result == 0 &&
           (uint32_t)positive_outer == expected_positive_exit && positive_status == PSP_THREAD_STOPPED &&
           positive_wait_type == 0 && positive_wait_id == 0 &&
           positive_exit_status == expected_positive_exit;
}
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_DMAC_CONCURRENCY && \
    PSP_ORACLE_CASE <= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC
#define DMAC_API_MEMCPY 0u
#define DMAC_API_TRY_MEMCPY 1u
#define DMAC_MEASURED_PREFIX 0x0000c000u
#define DMAC_REFERENCE_BUSY 0x80000021u

static int dmac_call(uint32_t api, void *dst, const void *src, uint32_t size) {
    return api == DMAC_API_TRY_MEMCPY
        ? sceDmacTryMemcpy(dst, src, size)
        : sceDmacMemcpy(dst, src, size);
}

static uint8_t dmac_pattern(uint32_t offset) {
    return (uint8_t)(0x10u + (offset & 0x3fu));
}

static uint32_t dmac_elapsed_us(uint64_t start, uint64_t end) {
    const uint64_t elapsed = end >= start ? end - start : 0;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY
#define DMAC_CONCURRENCY_BYTES 0x00100000u
#define DMAC_CONCURRENCY_TRIALS 64u
#define DMAC_FIRST_PRIORITY 0x10u
#define DMAC_DEST_SENTINEL 0xa5u
#define DMAC_CONCURRENCY_DST ((uint8_t *)0x04000000u)
#define DMAC_CONCURRENCY_SRC ((uint8_t *)0x04100000u)

static volatile uint32_t s_dmac_first_api;
static volatile uint32_t s_dmac_first_entered;
static volatile uint32_t s_dmac_first_returned;
static volatile uint32_t s_dmac_first_result;
static volatile uint64_t s_dmac_first_enter_us;
static volatile uint64_t s_dmac_first_exit_us;

static int dmac_first_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    s_dmac_first_enter_us = sceKernelGetSystemTimeWide();
    s_dmac_first_entered = 1;
    s_dmac_first_result = (uint32_t)dmac_call(
        s_dmac_first_api, DMAC_CONCURRENCY_DST,
        DMAC_CONCURRENCY_SRC, DMAC_CONCURRENCY_BYTES);
    s_dmac_first_exit_us = sceKernelGetSystemTimeWide();
    s_dmac_first_returned = 1;
    return 0;
}

static uint32_t dmac_contiguous_prefix(void) {
    uint32_t offset = 0;
    while (offset < DMAC_CONCURRENCY_BYTES &&
           DMAC_CONCURRENCY_DST[offset] == dmac_pattern(offset)) {
        ++offset;
    }
    return offset;
}

static uint32_t dmac_non_sentinel_after(uint32_t offset) {
    uint32_t count = 0;
    while (offset < DMAC_CONCURRENCY_BYTES) {
        if (DMAC_CONCURRENCY_DST[offset] != DMAC_DEST_SENTINEL) ++count;
        ++offset;
    }
    return count;
}

static void run_dmac_concurrency_combo(int emulated, uint32_t first_api,
                                       uint32_t second_api,
                                       const char *case_id) {
    uint32_t attempted = 0;
    uint32_t first_entered_count = 0;
    uint32_t first_returned_count = 0;
    uint32_t start_window_count = 0;
    uint32_t timeline_overlap_count = 0;
    uint32_t first_zero_count = 0;
    uint32_t first_other_count = 0;
    uint32_t second_busy_count = 0;
    uint32_t second_zero_count = 0;
    uint32_t second_other_count = 0;
    uint32_t busy_while_first_pending_count = 0;
    uint32_t busy_after_first_return_count = 0;
    uint32_t prefix_min = UINT32_MAX;
    uint32_t prefix_max = 0;
    uint32_t prefix_c000_count = 0;
    uint32_t stray_mutation_count = 0;
    uint32_t first_min_us = UINT32_MAX;
    uint32_t first_max_us = 0;
    uint32_t second_min_us = UINT32_MAX;
    uint32_t second_max_us = 0;
    uint32_t last_first_result = 0;
    uint32_t last_second_result = 0;
    uint32_t setup_error = 0;

    for (uint32_t offset = 0; offset < DMAC_CONCURRENCY_BYTES; ++offset) {
        DMAC_CONCURRENCY_SRC[offset] = dmac_pattern(offset);
    }
    sceKernelDcacheWritebackInvalidateRange(
        DMAC_CONCURRENCY_SRC, DMAC_CONCURRENCY_BYTES);

    for (uint32_t trial = 0; trial < DMAC_CONCURRENCY_TRIALS; ++trial) {
        memset(DMAC_CONCURRENCY_DST, DMAC_DEST_SENTINEL,
               DMAC_CONCURRENCY_BYTES);
        sceKernelDcacheWritebackInvalidateRange(
            DMAC_CONCURRENCY_DST, DMAC_CONCURRENCY_BYTES);

        s_dmac_first_api = first_api;
        s_dmac_first_entered = 0;
        s_dmac_first_returned = 0;
        s_dmac_first_result = 0;
        s_dmac_first_enter_us = 0;
        s_dmac_first_exit_us = 0;

        const SceUID thread = sceKernelCreateThread(
            "oracle-dmac-first", dmac_first_thread, DMAC_FIRST_PRIORITY,
            0x2000, 0, NULL);
        if (thread < 0) {
            setup_error = (uint32_t)thread;
            break;
        }
        const int start_result = sceKernelStartThread(thread, 0, NULL);
        if (start_result < 0) {
            setup_error = (uint32_t)start_result;
            sceKernelDeleteThread(thread);
            break;
        }

        /* A high-priority first caller can return control here only if the
           syscall blocks/yields or has already returned.  Record that state;
           do not infer an in-flight DMA solely from thread scheduling. */
        const uint32_t entered_before_second = s_dmac_first_entered;
        const uint32_t returned_before_second = s_dmac_first_returned;
        if (entered_before_second) ++first_entered_count;
        if (returned_before_second) ++first_returned_count;
        if (entered_before_second && !returned_before_second) {
            ++start_window_count;
        }

        const uint64_t second_enter_us = sceKernelGetSystemTimeWide();
        const uint32_t second_result = (uint32_t)dmac_call(
            second_api, DMAC_CONCURRENCY_DST,
            DMAC_CONCURRENCY_SRC, DMAC_CONCURRENCY_BYTES);
        const uint64_t second_exit_us = sceKernelGetSystemTimeWide();
        (void)sceKernelWaitThreadEnd(thread, NULL);
        sceKernelDeleteThread(thread);
        ++attempted;

        const uint32_t first_result = s_dmac_first_result;
        const uint32_t first_us = dmac_elapsed_us(
            s_dmac_first_enter_us, s_dmac_first_exit_us);
        const uint32_t second_us = dmac_elapsed_us(
            second_enter_us, second_exit_us);
        if (second_enter_us < s_dmac_first_exit_us) ++timeline_overlap_count;
        if (first_result == 0) ++first_zero_count; else ++first_other_count;
        if (second_result == DMAC_REFERENCE_BUSY) {
            ++second_busy_count;
            if (entered_before_second && !returned_before_second) {
                ++busy_while_first_pending_count;
            }
            if (returned_before_second) {
                ++busy_after_first_return_count;
            }
        } else if (second_result == 0) {
            ++second_zero_count;
        } else {
            ++second_other_count;
        }
        if (first_us < first_min_us) first_min_us = first_us;
        if (first_us > first_max_us) first_max_us = first_us;
        if (second_us < second_min_us) second_min_us = second_us;
        if (second_us > second_max_us) second_max_us = second_us;
        last_first_result = first_result;
        last_second_result = second_result;

        sceKernelDcacheInvalidateRange(
            DMAC_CONCURRENCY_DST, DMAC_CONCURRENCY_BYTES);
        const uint32_t prefix = dmac_contiguous_prefix();
        if (prefix < prefix_min) prefix_min = prefix;
        if (prefix > prefix_max) prefix_max = prefix;
        if (prefix == DMAC_MEASURED_PREFIX) ++prefix_c000_count;
        if (dmac_non_sentinel_after(prefix) != 0) ++stray_mutation_count;
    }

    if (attempted == 0) {
        prefix_min = 0;
        first_min_us = 0;
        second_min_us = 0;
    }
    const uint32_t out[] = {
        attempted,
        first_api,
        second_api,
        first_entered_count,
        first_returned_count,
        start_window_count,
        timeline_overlap_count,
        first_zero_count,
        first_other_count,
        second_busy_count,
        second_zero_count,
        second_other_count,
        busy_while_first_pending_count,
        busy_after_first_return_count,
        prefix_min,
        prefix_max,
        prefix_c000_count,
        stray_mutation_count,
        first_min_us,
        first_max_us,
        second_min_us,
        second_max_us,
        DMAC_FIRST_PRIORITY,
        last_first_result,
    };
    emit_record_extended(emulated, "PSP-DMAC-001", case_id,
                         setup_error ? "ERROR" : "PASS",
                         setup_error ? setup_error : last_second_result,
                         out, sizeof(out) / sizeof(out[0]));
}

static void run_dmac_concurrency(int emulated) {
    /* This reproduces the scheduling shape in PSPAutotests' public DMAC test,
       then adds a Try/Try control and a normal-call second caller.  The joint
       scalar state distinguishes BUSY while the first caller remains pending
       from BUSY after that syscall returned. Zero BUSY returns without a
       first-caller window are merely "not measurable with this probe." */
    run_dmac_concurrency_combo(emulated, DMAC_API_MEMCPY,
                               DMAC_API_TRY_MEMCPY,
                               "concurrent-memcpy-try");
    run_dmac_concurrency_combo(emulated, DMAC_API_TRY_MEMCPY,
                               DMAC_API_TRY_MEMCPY,
                               "concurrent-try-try");
    run_dmac_concurrency_combo(emulated, DMAC_API_TRY_MEMCPY,
                               DMAC_API_MEMCPY,
                               "concurrent-try-memcpy");
}
#endif

#if PSP_ORACLE_CASE >= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST && \
    PSP_ORACLE_CASE <= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC
/* With PSP_LARGE_MEMORY=0, the pinned PSPSDK build contract requests the
   24 MiB baseline user partition.  uOFW's public memory map places that
   partition at 0x08800000 with size 0x01800000, ending at 0x0A000000.
   The probe does not touch that boundary until the allocator has reserved the
   entire valid prefix and independently rejected a block beginning at the
   next address. */
/* Allocator-observed top (PSP-3000/6.61-ARK-5.1.0, 512 KiB bounded heap):
   grants through 0x0B700000; failures at 0x0B73F000/0x0B740000/0x0B7FF000/
   0x0B800000 (surveyor + premise runs, same heap geometry). Candidate end
   with sanity 64 KiB below in granted territory. The v3 premise gate
   re-verifies at runtime; if the boundary moved, the SKIP records it. */
#define DMAC_BASELINE_USER_END 0x0b720000u
/* Proven-grantable sanity address for the premise gate (granted in prior
   surveyor runs under the same heap geometry). */
#define DMAC_SANITY_ADDR 0x0b700000u
#define DMAC_BOUNDARY_LEAD 0x00000100u
#define DMAC_BOUNDARY_BLOCK_BASE \
    (DMAC_BASELINE_USER_END - DMAC_MEASURED_PREFIX - DMAC_BOUNDARY_LEAD)
#define DMAC_BOUNDARY_BLOCK_BYTES (DMAC_MEASURED_PREFIX + DMAC_BOUNDARY_LEAD)
#define DMAC_INVALID_REQUEST (DMAC_MEASURED_PREFIX + 1u)
#define DMAC_BOUNDARY_SENTINEL 0xa5u
#define DMAC_BOUNDARY_GUARD 0x6du

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST
#define DMAC_INVALID_API DMAC_API_MEMCPY
#define DMAC_INVALID_DIRECTION 0u
#define DMAC_INVALID_CASE_ID "invalid-tail-memcpy-dst"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_SRC
#define DMAC_INVALID_API DMAC_API_MEMCPY
#define DMAC_INVALID_DIRECTION 1u
#define DMAC_INVALID_CASE_ID "invalid-tail-memcpy-src"
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_DST
#define DMAC_INVALID_API DMAC_API_TRY_MEMCPY
#define DMAC_INVALID_DIRECTION 0u
#define DMAC_INVALID_CASE_ID "invalid-tail-try-dst"
#else
#define DMAC_INVALID_API DMAC_API_TRY_MEMCPY
#define DMAC_INVALID_DIRECTION 1u
#define DMAC_INVALID_CASE_ID "invalid-tail-try-src"
#endif

static uint8_t s_dmac_valid_source[DMAC_INVALID_REQUEST]
    __attribute__((aligned(64)));
static uint8_t s_dmac_valid_destination[DMAC_INVALID_REQUEST + 1u]
    __attribute__((aligned(64)));

static uint32_t dmac_count_pattern(const uint8_t *bytes, uint32_t size) {
    uint32_t count = 0;
    for (uint32_t offset = 0; offset < size; ++offset) {
        if (bytes[offset] == dmac_pattern(offset)) ++count;
    }
    return count;
}

static uint32_t dmac_count_not(const uint8_t *bytes, uint32_t size,
                               uint8_t value) {
    uint32_t count = 0;
    for (uint32_t offset = 0; offset < size; ++offset) {
        if (bytes[offset] != value) ++count;
    }
    return count;
}

static void emit_dmac_invalid_setup(int emulated, const char *status,
                                    uint32_t result, uint32_t setup_mask,
                                    uint32_t tail_allocation_result) {
    const uint32_t out[] = {
        setup_mask,
        DMAC_INVALID_REQUEST,
        DMAC_MEASURED_PREFIX,
        DMAC_INVALID_DIRECTION,
        DMAC_INVALID_API,
        tail_allocation_result,
    };
    emit_record_extended(emulated, "PSP-DMAC-001", DMAC_INVALID_CASE_ID,
                         status, result, out, sizeof(out) / sizeof(out[0]));
}

static void run_dmac_invalid_tail(int emulated) {
    uint32_t setup_mask = 0;
    /* Phase A: clean-premise check with NO blocks held. The candidate end
       must FAIL a fixed-address alloc (it is the allegedly invalid byte),
       while the sanity address (proven grantable) must SUCCEED (the
       fixed-addr mechanism works at all). A grant AT the end means the
       premise is false (SKIP). A held-block-adjacent check is deliberately
       NOT used here: measured 4/4, the allocator grants the exact end of a
       held block while granting nothing there clean (adjacency artifact). */
    const uint32_t free_before = (uint32_t)sceKernelMaxFreeMemSize();
    const SceUID probe_end = sceKernelAllocPartitionMemory(
        2, "oracle-dmac-endprobe", PSP_SMEM_Addr, 0x100,
        (void *)DMAC_BASELINE_USER_END);
    const uint32_t free_after = (uint32_t)sceKernelMaxFreeMemSize();
    if (probe_end >= 0) {
        /* Granted somewhere: only a grant AT the end falsifies the premise.
           An elsewhere-grant means the end itself is not free (proceed).
           The free-pool delta arbitrates heap-squat vs genuinely-free: a
           real grant consumes 0x100 from the pool. */
        uint8_t *const end_head =
            (uint8_t *)sceKernelGetBlockHeadAddr(probe_end);
        const uint32_t granted_at_end =
            (uint32_t)((uintptr_t)end_head == (uintptr_t)DMAC_BASELINE_USER_END);
        sceKernelFreePartitionMemory(probe_end);
        if (granted_at_end) {
            const uint32_t out[] = {0, DMAC_BASELINE_USER_END, free_before,
                                    free_after};
            emit_record_extended(emulated, "PSP-DMAC-001",
                                 DMAC_INVALID_CASE_ID, "SKIP", 1u, out, 4);
            return;
        }
    }
    const uint32_t end_err = (uint32_t)probe_end;
    const SceUID probe_sane = sceKernelAllocPartitionMemory(
        2, "oracle-dmac-sanity", PSP_SMEM_Addr, 0x100,
        (void *)DMAC_SANITY_ADDR);
    if (probe_sane < 0) {
        emit_dmac_invalid_setup(emulated, "SKIP", (uint32_t)probe_sane,
                                setup_mask, 0);
        return;
    }
    sceKernelFreePartitionMemory(probe_sane);
    setup_mask |= 4u;
    const SceUID block = sceKernelAllocPartitionMemory(
        2, "oracle-dmac-boundary", PSP_SMEM_Addr,
        DMAC_BOUNDARY_BLOCK_BYTES, (void *)DMAC_BOUNDARY_BLOCK_BASE);
    if (block < 0) {
        emit_dmac_invalid_setup(emulated, "SKIP", (uint32_t)block,
                                setup_mask, 0);
        return;
    }
    setup_mask |= 1u;
    uint8_t *const block_head = (uint8_t *)sceKernelGetBlockHeadAddr(block);
    if ((uintptr_t)block_head != (uintptr_t)DMAC_BOUNDARY_BLOCK_BASE) {
        sceKernelFreePartitionMemory(block);
        emit_dmac_invalid_setup(emulated, "SKIP", 0, setup_mask, 0);
        return;
    }
    setup_mask |= 2u;

    uint8_t *const boundary_prefix = block_head + DMAC_BOUNDARY_LEAD;
    memset(block_head, DMAC_BOUNDARY_GUARD, DMAC_BOUNDARY_LEAD);
    for (uint32_t offset = 0; offset < DMAC_INVALID_REQUEST; ++offset) {
        s_dmac_valid_source[offset] = dmac_pattern(offset);
    }
    memset(s_dmac_valid_destination, DMAC_BOUNDARY_SENTINEL,
           sizeof(s_dmac_valid_destination));
    if (DMAC_INVALID_DIRECTION == 0u) {
        memset(boundary_prefix, DMAC_BOUNDARY_SENTINEL,
               DMAC_MEASURED_PREFIX);
    } else {
        for (uint32_t offset = 0; offset < DMAC_MEASURED_PREFIX; ++offset) {
            boundary_prefix[offset] = dmac_pattern(offset);
        }
    }
    sceKernelDcacheWritebackInvalidateRange(
        block_head, DMAC_BOUNDARY_BLOCK_BYTES);
    sceKernelDcacheWritebackInvalidateRange(
        s_dmac_valid_source, sizeof(s_dmac_valid_source));
    sceKernelDcacheWritebackInvalidateRange(
        s_dmac_valid_destination, sizeof(s_dmac_valid_destination));

    void *const dst = DMAC_INVALID_DIRECTION == 0u
        ? (void *)boundary_prefix : (void *)s_dmac_valid_destination;
    const void *const src = DMAC_INVALID_DIRECTION == 0u
        ? (const void *)s_dmac_valid_source : (const void *)boundary_prefix;
    const uint64_t start_us = sceKernelGetSystemTimeWide();
    const uint32_t result = (uint32_t)dmac_call(
        DMAC_INVALID_API, dst, src, DMAC_INVALID_REQUEST);
    const uint64_t end_us = sceKernelGetSystemTimeWide();

    uint8_t *const valid_destination = DMAC_INVALID_DIRECTION == 0u
        ? boundary_prefix : s_dmac_valid_destination;
    const uint8_t *const expected_source = DMAC_INVALID_DIRECTION == 0u
        ? s_dmac_valid_source : boundary_prefix;
    sceKernelDcacheInvalidateRange(valid_destination, DMAC_MEASURED_PREFIX);
    sceKernelDcacheInvalidateRange(block_head, DMAC_BOUNDARY_LEAD);
    if (DMAC_INVALID_DIRECTION != 0u) {
        sceKernelDcacheInvalidateRange(
            s_dmac_valid_destination, sizeof(s_dmac_valid_destination));
    }

    const uint32_t prefix_matches = dmac_count_pattern(
        valid_destination, DMAC_MEASURED_PREFIX);
    const uint32_t prefix_non_sentinel = dmac_count_not(
        valid_destination, DMAC_MEASURED_PREFIX, DMAC_BOUNDARY_SENTINEL);
    const uint32_t guard_changed = dmac_count_not(
        block_head, DMAC_BOUNDARY_LEAD, DMAC_BOUNDARY_GUARD);
    const uint32_t valid_tail_changed = DMAC_INVALID_DIRECTION == 0u
        ? UINT32_MAX
        : (uint32_t)(s_dmac_valid_destination[DMAC_MEASURED_PREFIX] !=
                     DMAC_BOUNDARY_SENTINEL);
    const uint32_t post_request_changed = DMAC_INVALID_DIRECTION == 0u
        ? UINT32_MAX
        : (uint32_t)(s_dmac_valid_destination[DMAC_INVALID_REQUEST] !=
                     DMAC_BOUNDARY_SENTINEL);
    const uint32_t source_prefix_matches = dmac_count_pattern(
        expected_source, DMAC_MEASURED_PREFIX);
    const uint32_t out[] = {
        setup_mask,
        DMAC_INVALID_REQUEST,
        DMAC_MEASURED_PREFIX,
        DMAC_INVALID_DIRECTION,
        DMAC_INVALID_API,
        prefix_matches,
        prefix_non_sentinel,
        guard_changed,
        valid_tail_changed,
        post_request_changed,
        source_prefix_matches,
        dmac_elapsed_us(start_us, end_us),
        end_err,
    };
    sceKernelFreePartitionMemory(block);
    emit_record_extended(emulated, "PSP-DMAC-001", DMAC_INVALID_CASE_ID,
                         "PASS", result, out,
                         sizeof(out) / sizeof(out[0]));
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_VCOUNT || \
    PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_DUTY

/* Long-interrupt-mask display accounting.
 *
 * The accepted #88 record established the SHORT-window facts: system time keeps
 * advancing across `sceKernelCpuSuspendIntr`, guest VCOUNT does not, and exactly
 * one VBLANK handler call is taken on resume.  It never sampled VCOUNT
 * immediately after `sceKernelCpuResumeIntr`, so it cannot distinguish
 *
 *   (A) the counter catches up by every period that elapsed under the mask,
 *   (B) exactly one deferred period is credited, or
 *   (C) the elapsed periods are permanently absent from software VCOUNT.
 *
 * These two cases measure that difference directly and never assume a period
 * length: the vblank period is calibrated on the same device in the same run.
 * Every spin is bounded by BOTH an elapsed-system-time test and an iteration
 * cap, so a stopped clock cannot turn a probe into a hang. */

#define MASK_TRIALS      12
#define MASK_SPIN_CAP    40000000u   /* iteration ceiling; never the normal exit */
#define CALIB_FRAMES     60

/* Spin until `want` microseconds of system time have elapsed since `t0`, or the
 * iteration cap trips.  Returns the measured elapsed microseconds.  The volatile
 * sink stops the compiler from discarding the loop. */
static volatile uint32_t s_spin_sink;
static uint32_t spin_us(uint32_t t0, uint32_t want, uint32_t *iters_out) {
    uint32_t i = 0;
    uint32_t now = t0;
    for (; i < MASK_SPIN_CAP; i++) {
        now = sceKernelGetSystemTimeLow();
        if ((uint32_t)(now - t0) >= want) break;
        s_spin_sink = i;
    }
    if (iters_out) *iters_out = i;
    return (uint32_t)(now - t0);
}

/* Measure the device's own vblank period without assuming 60000/1001.  Returns
 * nanoseconds per period; 0 if the display never advanced. */
static uint32_t calibrate_period_ns(uint32_t *vc_frames_out) {
    sceDisplayWaitVblankStart();
    uint32_t st0 = sceKernelGetSystemTimeLow();
    uint32_t vc0 = sceDisplayGetVcount();
    for (int i = 0; i < CALIB_FRAMES; i++) sceDisplayWaitVblankStart();
    uint32_t st1 = sceKernelGetSystemTimeLow();
    uint32_t vc1 = sceDisplayGetVcount();
    uint32_t frames = vc1 - vc0;
    if (vc_frames_out) *vc_frames_out = frames;
    if (!frames) return 0;
    return (uint32_t)(((uint64_t)(uint32_t)(st1 - st0) * 1000ull) / frames);
}

static uint32_t periods_in(uint32_t span_us, uint32_t period_ns) {
    if (!period_ns) return 0;
    return (uint32_t)(((uint64_t)span_us * 1000ull) / period_ns);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_VCOUNT
static const uint32_t k_mask_us[] = { 4000u, 16700u, 30000u, 50000u };
#define MASK_DURATIONS ((int)(sizeof(k_mask_us) / sizeof(k_mask_us[0])))

static void run_display_mask_vcount(int emulated) {
    uint32_t calib_frames = 0;
    const uint32_t period_ns = calibrate_period_ns(&calib_frames);
    const uint32_t cpu_mhz = (uint32_t)scePowerGetCpuClockFrequencyInt();

    for (int d = 0; d < MASK_DURATIONS; d++) {
        const uint32_t want = k_mask_us[d];
        uint32_t out[29];
        uint32_t trials = 0;
        uint32_t span_min = 0xffffffffu, span_max = 0, span_sum = 0;
        uint32_t per_min = 0xffffffffu, per_max = 0, per_sum = 0;
        uint32_t dur_min = 0xffffffffu, dur_max = 0;              /* vcD - vc0 */
        uint32_t imm_min = 0xffffffffu, imm_max = 0, imm_sum = 0;  /* vcI - vc0 */
        uint32_t n_zero = 0, n_one = 0, n_full = 0, n_partial = 0;
        uint32_t nx1_min = 0xffffffffu, nx1_max = 0, n_next_one = 0;
        uint32_t ahd_min = 0xffffffffu, ahd_max = 0;
        uint32_t ahi_min = 0xffffffffu, ahi_max = 0;
        uint32_t hc_min = 0xffffffffu, hc_max = 0;
        uint32_t n_time_moved = 0;

        for (int k = 0; k < MASK_TRIALS; k++) {
            sceDisplayWaitVblankStart();          /* phase-align to a boundary */
            const uint32_t st0 = sceKernelGetSystemTimeLow();
            const uint32_t vc0 = sceDisplayGetVcount();
            const uint32_t ah0 = (uint32_t)sceDisplayGetAccumulatedHcount();

            const int tok = sceKernelCpuSuspendIntr();
            const uint32_t span = spin_us(st0, want, NULL);
            const uint32_t vcD = sceDisplayGetVcount();
            const uint32_t ahD = (uint32_t)sceDisplayGetAccumulatedHcount();
            const uint32_t hcD = (uint32_t)sceDisplayGetCurrentHcount();
            sceKernelCpuResumeIntr(tok);

            /* The single measurement the accepted record is missing. */
            const uint32_t vcI = sceDisplayGetVcount();
            const uint32_t ahI = (uint32_t)sceDisplayGetAccumulatedHcount();

            sceDisplayWaitVblankStart();
            const uint32_t vcN1 = sceDisplayGetVcount();

            const uint32_t periods = periods_in(span, period_ns);
            const uint32_t d_dur = vcD - vc0;
            const uint32_t d_imm = vcI - vc0;
            const uint32_t d_nx1 = vcN1 - vcI;

            trials++;
            if (span) n_time_moved++;
            if (span < span_min) span_min = span;
            if (span > span_max) span_max = span;
            span_sum += span;
            if (periods < per_min) per_min = periods;
            if (periods > per_max) per_max = periods;
            per_sum += periods;
            if (d_dur < dur_min) dur_min = d_dur;
            if (d_dur > dur_max) dur_max = d_dur;
            if (d_imm < imm_min) imm_min = d_imm;
            if (d_imm > imm_max) imm_max = d_imm;
            imm_sum += d_imm;
            if (d_imm == 0u) n_zero++;
            else if (d_imm == 1u) n_one++;
            if (periods >= 2u && d_imm == periods) n_full++;
            else if (d_imm > 1u && periods >= 2u && d_imm < periods) n_partial++;
            if (d_nx1 < nx1_min) nx1_min = d_nx1;
            if (d_nx1 > nx1_max) nx1_max = d_nx1;
            if (d_nx1 == 1u) n_next_one++;
            {
                const uint32_t dahd = ahD - ah0, dahi = ahI - ah0;
                if (dahd < ahd_min) ahd_min = dahd;
                if (dahd > ahd_max) ahd_max = dahd;
                if (dahi < ahi_min) ahi_min = dahi;
                if (dahi > ahi_max) ahi_max = dahi;
            }
            if (hcD < hc_min) hc_min = hcD;
            if (hcD > hc_max) hc_max = hcD;
        }

        out[0]  = trials;
        out[1]  = want;
        out[2]  = span_min;
        out[3]  = span_max;
        out[4]  = per_min;
        out[5]  = per_max;
        out[6]  = dur_min;
        out[7]  = dur_max;
        out[8]  = imm_min;
        out[9]  = imm_max;
        out[10] = n_zero;
        out[11] = n_one;
        out[12] = n_full;
        out[13] = n_partial;
        out[14] = nx1_min;
        out[15] = nx1_max;
        out[16] = ahd_min;
        out[17] = ahd_max;
        out[18] = ahi_min;
        out[19] = ahi_max;
        out[20] = imm_sum;
        out[21] = per_sum;
        out[22] = trials ? span_sum / trials : 0u;
        out[23] = n_time_moved;
        out[24] = hc_min;
        out[25] = hc_max;
        out[26] = period_ns;
        out[27] = cpu_mhz;
        out[28] = n_next_one;

        {
            char case_id[64];
            snprintf(case_id, sizeof(case_id), "display-mask-vcount-%uus", (unsigned int)want);
            /* PASS states only that every trial ran and the masked window really
               elapsed; it makes no claim about which semantic the numbers show. */
            const int ok = (trials == (uint32_t)MASK_TRIALS) &&
                           (n_time_moved == trials) && (period_ns != 0u) &&
                           (calib_frames >= (uint32_t)(CALIB_FRAMES - 2));
            emit_record_extended(emulated, "PSP-DISPLAY-001", case_id,
                                 ok ? "PASS" : "FAIL", period_ns, out, 29);
        }
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_DUTY
/* Sustained duty cycle.  One resume per mask span, many periods per span: the
 * three candidate semantics separate by a factor of ~4 in the accumulated
 * VCOUNT over a fixed wall window, which no single-trial rounding can fake. */
#define DUTY_WINDOW_US 2000000u
#define DUTY_GAP_US       2000u

static const uint32_t k_duty_us[] = { 33000u, 66000u };
#define DUTY_SETTINGS ((int)(sizeof(k_duty_us) / sizeof(k_duty_us[0])))

static void run_display_mask_duty(int emulated) {
    uint32_t calib_frames = 0;
    const uint32_t period_ns = calibrate_period_ns(&calib_frames);

    for (int d = 0; d < DUTY_SETTINGS; d++) {
        const uint32_t want = k_duty_us[d];
        uint32_t out[13];
        uint32_t episodes = 0, masked_sum = 0, unmasked_sum = 0;
        uint32_t span_min = 0xffffffffu, span_max = 0;

        sceDisplayWaitVblankStart();
        const uint32_t wall0 = sceKernelGetSystemTimeLow();
        const uint32_t vc0 = sceDisplayGetVcount();
        const uint32_t ah0 = (uint32_t)sceDisplayGetAccumulatedHcount();

        while ((uint32_t)(sceKernelGetSystemTimeLow() - wall0) < DUTY_WINDOW_US &&
               episodes < 4096u) {
            const int tok = sceKernelCpuSuspendIntr();
            const uint32_t m0 = sceKernelGetSystemTimeLow();
            const uint32_t span = spin_us(m0, want, NULL);
            sceKernelCpuResumeIntr(tok);
            episodes++;
            masked_sum += span;
            if (span < span_min) span_min = span;
            if (span > span_max) span_max = span;
            /* Unmasked servicing gap: long enough for the deferred delivery and
               USB/PSPLink to run, short enough that it carries few periods. */
            const uint32_t g0 = sceKernelGetSystemTimeLow();
            unmasked_sum += spin_us(g0, DUTY_GAP_US, NULL);
        }

        const uint32_t wall = (uint32_t)(sceKernelGetSystemTimeLow() - wall0);
        const uint32_t vcd = sceDisplayGetVcount() - vc0;
        const uint32_t ahd = (uint32_t)sceDisplayGetAccumulatedHcount() - ah0;
        const uint32_t expected = periods_in(wall, period_ns);

        out[0]  = wall;
        out[1]  = want;
        out[2]  = DUTY_GAP_US;
        out[3]  = episodes;
        out[4]  = vcd;
        out[5]  = expected;
        out[6]  = ahd;
        out[7]  = masked_sum;
        out[8]  = unmasked_sum;
        out[9]  = expected ? (uint32_t)(((uint64_t)vcd * 1000ull) / expected) : 0u;
        out[10] = span_min;
        out[11] = span_max;
        out[12] = period_ns;

        {
            char case_id[64];
            snprintf(case_id, sizeof(case_id), "display-mask-duty-%uus", (unsigned int)want);
            const int ok = episodes > 8u && period_ns != 0u && wall >= DUTY_WINDOW_US;
            emit_record_extended(emulated, "PSP-DISPLAY-001", case_id,
                                 ok ? "PASS" : "FAIL", period_ns, out, 13);
        }
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_GE_MASK

/* Does the PSP GE make forward progress while CPU interrupt delivery is masked?
 *
 * The experiment is stall-gated so that "the GE simply finished before the mask
 * opened" cannot produce a false positive: the list is enqueued already stalled
 * at its first word, the destination is proven untouched, the CPU mask is taken,
 * and only then is the stall released.  Any destination change observed before
 * `sceKernelCpuResumeIntr` is therefore work the GE did with CPU interrupts off.
 *
 * The observable is memory the GE itself writes -- a chain of source-owned block
 * transfers into VRAM tiles -- read back exclusively through the UNCACHED VRAM
 * mirror, so a stale CPU cache line can never be misread as "the GE did not
 * progress".  GE completion notification is measured separately, because the
 * finish handler runs in interrupt context and is expected to stay pending.
 *
 * Nothing here touches retail code or data. */

#define GE_TILES        16
#define GE_TILE_W       512u          /* pixels, 4 bytes each                */
#define GE_TILE_H        32u
#define GE_TILE_BYTES   (GE_TILE_W * GE_TILE_H * 4u)
#define GE_SENTINEL     0xA5A5A5A5u
#define GE_PAYLOAD      0x5A5A5A5Au
#define GE_MASK_CAP_US  40000u        /* bounded masked poll window          */
#define GE_SPIN_CAP     20000000u
#define GE_TRIALS       12

/* Uncached mirrors.  VRAM 0x04000000 is the cached view, 0x44000000 the
   uncached one; main RAM 0x08800000 mirrors at 0x48800000. */
#define UNCACHED(p)  ((volatile uint32_t *)((uintptr_t)(p) | 0x40000000u))

/* GE command words: (cmd << 24) | 24-bit payload. */
#define GE_CMD_NOP            0x00
#define GE_CMD_END            0x0C
#define GE_CMD_FINISH         0x0F
#define GE_CMD_TRANSFERSRC    0xB2
#define GE_CMD_TRANSFERSRCW   0xB3
#define GE_CMD_TRANSFERDST    0xB4
#define GE_CMD_TRANSFERDSTW   0xB5
#define GE_CMD_TRANSFERSTART  0xEA
#define GE_CMD_TRANSFERSRCPOS 0xEB
#define GE_CMD_TRANSFERDSTPOS 0xEC
#define GE_CMD_TRANSFERSIZE   0xEE

static uint32_t s_ge_list[GE_TILES * 10 + 8] __attribute__((aligned(64)));
static uint32_t s_ge_src[GE_TILE_W * GE_TILE_H] __attribute__((aligned(64)));
static volatile int s_ge_finish_calls;
static volatile int s_ge_signal_calls;

static void ge_finish_cb(int id, void *arg) { (void)id; (void)arg; s_ge_finish_calls++; }
static void ge_signal_cb(int id, void *arg) { (void)id; (void)arg; s_ge_signal_calls++; }

static uint32_t ge_cmd(uint32_t cmd, uint32_t payload) {
    return (cmd << 24) | (payload & 0x00ffffffu);
}

/* Address is split: low 24 bits in the ADDR command, bits 24..31 in bits 16..23
   of the companion W command, which also carries the stride in pixels. */
static void ge_emit_addr(uint32_t **w, uint32_t addr_cmd, uint32_t w_cmd,
                         uint32_t addr, uint32_t stride_px) {
    *(*w)++ = ge_cmd(addr_cmd, addr & 0x00fffff0u);
    *(*w)++ = ge_cmd(w_cmd, ((addr >> 8) & 0x00ff0000u) | (stride_px & 0x7f8u));
}

static uint32_t ge_build_list(uint32_t src, uint32_t dst_base) {
    uint32_t *w = s_ge_list;
    for (int t = 0; t < GE_TILES; t++) {
        ge_emit_addr(&w, GE_CMD_TRANSFERSRC, GE_CMD_TRANSFERSRCW, src, GE_TILE_W);
        ge_emit_addr(&w, GE_CMD_TRANSFERDST, GE_CMD_TRANSFERDSTW,
                     dst_base + (uint32_t)t * GE_TILE_BYTES, GE_TILE_W);
        *w++ = ge_cmd(GE_CMD_TRANSFERSRCPOS, 0u);
        *w++ = ge_cmd(GE_CMD_TRANSFERDSTPOS, 0u);
        *w++ = ge_cmd(GE_CMD_TRANSFERSIZE, ((GE_TILE_H - 1u) << 10) | (GE_TILE_W - 1u));
        *w++ = ge_cmd(GE_CMD_TRANSFERSTART, 1u);   /* bit0 = 4 bytes per pixel */
    }
    *w++ = ge_cmd(GE_CMD_FINISH, 0u);
    *w++ = ge_cmd(GE_CMD_END, 0u);
    *w++ = ge_cmd(GE_CMD_NOP, 0u);
    *w++ = ge_cmd(GE_CMD_NOP, 0u);
    return (uint32_t)((char *)w - (char *)s_ge_list);
}

/* Paint every destination tile with the sentinel through the uncached mirror and
   confirm it reads back, so a failed pre-fill can never look like GE progress. */
static int ge_prime_dst(uint32_t dst_base) {
    for (int t = 0; t < GE_TILES; t++) {
        volatile uint32_t *p = UNCACHED(dst_base + (uint32_t)t * GE_TILE_BYTES);
        p[0] = GE_SENTINEL;
        p[(GE_TILE_BYTES / 4u) - 1u] = GE_SENTINEL;
    }
    for (int t = 0; t < GE_TILES; t++) {
        volatile uint32_t *p = UNCACHED(dst_base + (uint32_t)t * GE_TILE_BYTES);
        if (p[0] != GE_SENTINEL || p[(GE_TILE_BYTES / 4u) - 1u] != GE_SENTINEL) return 0;
    }
    return 1;
}

/* Number of leading tiles whose first AND last word have left the sentinel. */
static int ge_tiles_done(uint32_t dst_base) {
    int n = 0;
    for (int t = 0; t < GE_TILES; t++) {
        volatile uint32_t *p = UNCACHED(dst_base + (uint32_t)t * GE_TILE_BYTES);
        if (p[0] == GE_SENTINEL || p[(GE_TILE_BYTES / 4u) - 1u] == GE_SENTINEL) break;
        n++;
    }
    return n;
}

/* mode 0 = PRIMARY (release the stall while masked)
   mode 1 = CONTROL A (hold the stall while masked -- must stay untouched)
   mode 2 = CONTROL B (release the stall with interrupts enabled -- must change) */
static void ge_run_case(int emulated, int mode, const char *case_id) {
    const uint32_t vram = (uint32_t)(uintptr_t)sceGeEdramGetAddr();
    const uint32_t dst_base = vram + 0x00100000u;      /* top 1 MiB of eDRAM */
    const uint32_t src = (uint32_t)(uintptr_t)s_ge_src;
    const uint32_t list = (uint32_t)(uintptr_t)s_ge_list;

    PspGeCallbackData cb;
    memset(&cb, 0, sizeof(cb));
    cb.signal_func = ge_signal_cb;
    cb.finish_func = ge_finish_cb;
    const int cbid = sceGeSetCallback(&cb);

    for (uint32_t i = 0; i < GE_TILE_W * GE_TILE_H; i++) s_ge_src[i] = GE_PAYLOAD;

    uint32_t out[24];
    memset(out, 0, sizeof(out));
    uint32_t trials = 0, prefill_ok = 0, pre_clean = 0;
    uint32_t masked_any = 0, masked_all = 0, masked_none = 0;
    uint32_t tiles_masked_min = 0xffffffffu, tiles_masked_max = 0;
    uint32_t first_us_min = 0xffffffffu, first_us_max = 0;
    uint32_t all_us_min = 0xffffffffu, all_us_max = 0;
    uint32_t fin_during = 0, fin_after_resume = 0, fin_after_sync = 0;
    uint32_t release_rc_nonzero = 0, enq_bad = 0;
    uint32_t span_min = 0xffffffffu, span_max = 0;
    uint32_t after_resume_all = 0, after_sync_all = 0;

    const uint32_t list_bytes = ge_build_list(src, dst_base);

    for (int k = 0; k < GE_TRIALS; k++) {
        sceKernelDcacheWritebackAll();
        if (!ge_prime_dst(dst_base)) continue;
        prefill_ok++;
        s_ge_finish_calls = 0;
        s_ge_signal_calls = 0;

        /* Enqueue already stalled at the first word: nothing may execute yet. */
        const int qid = sceGeListEnQueue((void *)list, (void *)list, cbid, NULL);
        if (qid < 0) { enq_bad++; continue; }

        /* Step 4 of the design: the destination is still untouched. */
        const int before = ge_tiles_done(dst_base);
        if (before == 0) pre_clean++;

        uint32_t tiles_masked = 0, t_first = 0, t_all = 0, span = 0;
        int fin_masked = 0;

        if (mode == 2) {
            /* CONTROL B: identical release, interrupts left enabled. */
            const uint32_t t0 = sceKernelGetSystemTimeLow();
            const int rc = sceGeListUpdateStallAddr(qid, (void *)(list + list_bytes));
            if (rc < 0) release_rc_nonzero++;
            uint32_t i = 0;
            for (; i < GE_SPIN_CAP; i++) {
                tiles_masked = (uint32_t)ge_tiles_done(dst_base);
                span = (uint32_t)(sceKernelGetSystemTimeLow() - t0);
                if (tiles_masked && !t_first) t_first = span ? span : 1u;
                if (tiles_masked >= (uint32_t)GE_TILES) { t_all = span ? span : 1u; break; }
                if (span >= GE_MASK_CAP_US) break;
            }
            fin_masked = s_ge_finish_calls;
        } else {
            const int tok = sceKernelCpuSuspendIntr();
            const uint32_t t0 = sceKernelGetSystemTimeLow();
            int rc = 0;
            if (mode == 0)
                rc = sceGeListUpdateStallAddr(qid, (void *)(list + list_bytes));
            uint32_t i = 0;
            for (; i < GE_SPIN_CAP; i++) {
                tiles_masked = (uint32_t)ge_tiles_done(dst_base);
                span = (uint32_t)(sceKernelGetSystemTimeLow() - t0);
                if (tiles_masked && !t_first) t_first = span ? span : 1u;
                if (tiles_masked >= (uint32_t)GE_TILES) { t_all = span ? span : 1u; break; }
                if (span >= GE_MASK_CAP_US) break;
            }
            fin_masked = s_ge_finish_calls;   /* sampled while still masked */
            sceKernelCpuResumeIntr(tok);
            if (rc < 0) release_rc_nonzero++;
        }

        if (fin_masked) fin_during++;
        if (ge_tiles_done(dst_base) >= GE_TILES) after_resume_all++;
        if (s_ge_finish_calls) fin_after_resume++;

        /* Drain: CONTROL A never released the stall, so release it now with
           interrupts enabled and sync, leaving the GE queue clean either way. */
        if (mode == 1) sceGeListUpdateStallAddr(qid, (void *)(list + list_bytes));
        sceGeListSync(qid, 0);
        sceGeDrawSync(0);
        if (s_ge_finish_calls) fin_after_sync++;
        if (ge_tiles_done(dst_base) >= GE_TILES) after_sync_all++;

        trials++;
        if (mode == 1) {
            /* For CONTROL A the interesting count is how many tiles moved while
               the stall was held: it must be zero. */
            if (tiles_masked) masked_any++; else masked_none++;
        } else {
            if (tiles_masked >= (uint32_t)GE_TILES) masked_all++;
            else if (tiles_masked) masked_any++;
            else masked_none++;
        }
        if (tiles_masked < tiles_masked_min) tiles_masked_min = tiles_masked;
        if (tiles_masked > tiles_masked_max) tiles_masked_max = tiles_masked;
        if (t_first) {
            if (t_first < first_us_min) first_us_min = t_first;
            if (t_first > first_us_max) first_us_max = t_first;
        }
        if (t_all) {
            if (t_all < all_us_min) all_us_min = t_all;
            if (t_all > all_us_max) all_us_max = t_all;
        }
        if (span < span_min) span_min = span;
        if (span > span_max) span_max = span;
    }

    sceGeUnsetCallback(cbid);

    out[0]  = trials;
    out[1]  = (uint32_t)mode;
    out[2]  = prefill_ok;
    out[3]  = pre_clean;
    out[4]  = masked_all;
    out[5]  = masked_any;
    out[6]  = masked_none;
    out[7]  = tiles_masked_min == 0xffffffffu ? 0u : tiles_masked_min;
    out[8]  = tiles_masked_max;
    out[9]  = first_us_min == 0xffffffffu ? 0u : first_us_min;
    out[10] = first_us_max;
    out[11] = all_us_min == 0xffffffffu ? 0u : all_us_min;
    out[12] = all_us_max;
    out[13] = fin_during;
    out[14] = fin_after_resume;
    out[15] = fin_after_sync;
    out[16] = after_resume_all;
    out[17] = after_sync_all;
    out[18] = release_rc_nonzero;
    out[19] = enq_bad;
    out[20] = span_min == 0xffffffffu ? 0u : span_min;
    out[21] = span_max;
    out[22] = (uint32_t)GE_TILES;
    out[23] = (uint32_t)scePowerGetCpuClockFrequencyInt();

    /* PASS means the trial machinery ran and the pre-release destination was
       clean every time.  It asserts nothing about which semantic was observed. */
    const int ok = (trials == (uint32_t)GE_TRIALS) &&
                   (prefill_ok == trials) && (pre_clean == trials) && (enq_bad == 0u);
    emit_record_extended(emulated, "PSP-DISPLAY-001", case_id,
                         ok ? "PASS" : "FAIL", (uint32_t)trials, out, 24);
}

static void run_display_ge_mask(int emulated) {
    ge_run_case(emulated, 2, "ge-mask-controlB-enabled-release");
    ge_run_case(emulated, 1, "ge-mask-controlA-stall-held");
    ge_run_case(emulated, 0, "ge-mask-primary-masked-release");
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_TRANSPORT_WRITE
/* Bidirectional host0 file proof: the PSP writes a fixed 64-byte pattern to a
   probe-owned disposable path, reads it back, and reports a checksum plus a
   match flag. The host independently hashes the file it receives. Pattern byte
   i is (0x5A ^ (i * 0x25 + (i >> 3))) & 0xFF. Only this one path is touched. */
#define TRANSPORT_PATH "host0:/nakagawa_transport_write.bin"
#define TRANSPORT_LEN 64u
static uint32_t transport_fnv1a(const uint8_t *data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}
static int run_transport_write_case(int emulated, uint32_t *out) {
    uint8_t pattern[TRANSPORT_LEN];
    uint8_t back[TRANSPORT_LEN];
    for (uint32_t i = 0; i < TRANSPORT_LEN; i++) {
        pattern[i] = (uint8_t)(0x5Au ^ (i * 0x25u + (i >> 3)));
    }
    memset(back, 0, sizeof(back));
    out[0] = transport_fnv1a(pattern, sizeof(pattern));
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    out[4] = (uint32_t)scePowerGetCpuClockFrequencyInt();
    SceUID fd = sceIoOpen(TRANSPORT_PATH,
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return (int)fd;
    }
    const int written = sceIoWrite(fd, pattern, (SceSize)sizeof(pattern));
    out[1] = (uint32_t)(written < 0 ? 0 : written);
    sceIoClose(fd);
    if (written != (int)sizeof(pattern)) {
        return -1;
    }
    fd = sceIoOpen(TRANSPORT_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return (int)fd;
    }
    const int got = sceIoRead(fd, back, (SceSize)sizeof(back));
    out[2] = (uint32_t)(got < 0 ? 0 : got);
    sceIoClose(fd);
    if (got != (int)sizeof(back)) {
        return -1;
    }
    const uint32_t match =
        (memcmp(pattern, back, sizeof(pattern)) == 0) ? 1u : 0u;
    out[3] = match;
    const int pass = (match == 1u) ? 0 : -1;
    emit_record_extended(emulated, "PSP-TRANSPORT-001",
                         "host0-write-readback", match == 1u ? "PASS" : "FAIL",
                         (uint32_t)pass, out, 5);
    return pass;
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_EXIT_DELETE
/* Exit/delete boundary matrix: implicit return vs sceKernelExitThread vs
   sceKernelExitDeleteThread, across positive / zero / small-negative /
   error-shaped statuses. Each cell observes through sceKernelWaitThreadEnd
   AND SceKernelThreadInfo.exitStatus, then probes post-state (delete and
   restart legality). One thread per cell, sequential, immediate exits; NULL
   waits are against threads that exit at once (deterministic sync).
   PASS = harness completed and all raw observations captured; raw API
   outcomes (including errors) are data in result/out*, never converted. */
#define ED_METHOD_RETURN 0u
#define ED_METHOD_EXIT 1u
#define ED_METHOD_EXITDELETE 2u
static uint32_t g_ed_status;
static uint32_t g_ed_method;
static int ed_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (g_ed_method == ED_METHOD_EXIT) {
        sceKernelExitThread((int)g_ed_status);
        return 0x55; /* unreachable */
    }
    if (g_ed_method == ED_METHOD_EXITDELETE) {
        sceKernelExitDeleteThread((int)g_ed_status);
        return 0x55; /* unreachable */
    }
    return (int)g_ed_status;
}
static int run_exit_delete_cell(int emulated, const char *case_id,
                                uint32_t status, uint32_t method) {
    uint32_t out[6] = {0xffffffffu, 0xffffffffu, 0xffffffffu,
                       0xffffffffu, 0xffffffffu, 0xffffffffu};
    g_ed_status = status;
    g_ed_method = method;
    SceUID thid = sceKernelCreateThread(case_id, ed_entry, 32, 0x1000, 0, NULL);
    if (thid < 0) {
        emit_record_extended(emulated, "PSP-THREAD-EXIT-001", case_id, "FAIL",
                             (uint32_t)thid, out, 6);
        return (int)thid;
    }
    out[5] = (uint32_t)sceKernelStartThread(thid, 0, NULL);
    if ((int)out[5] < 0) {
        sceKernelDeleteThread(thid);
        emit_record_extended(emulated, "PSP-THREAD-EXIT-001", case_id, "FAIL",
                             out[5], out, 6);
        return (int)out[5];
    }
    out[0] = (uint32_t)sceKernelWaitThreadEnd(thid, NULL);
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelReferThreadStatus(thid, &info) == 0) {
        out[1] = (uint32_t)info.exitStatus;
        out[2] = (uint32_t)info.status;
    }
    out[3] = (uint32_t)sceKernelDeleteThread(thid);
    out[4] = (uint32_t)sceKernelStartThread(thid, 0, NULL);
    if (method != ED_METHOD_EXITDELETE && (int)out[4] == 0) {
        /* Accepted restart: wait for the rerun, then delete, so the launch
           leaks nothing in-process. */
        sceKernelWaitThreadEnd(thid, NULL);
        sceKernelDeleteThread(thid);
    }
    emit_record_extended(emulated, "PSP-THREAD-EXIT-001", case_id, "PASS",
                         out[0], out, 6);
    return 0;
}
static void run_thread_exit_delete(int emulated) {
    static const struct {
        const char *id;
        uint32_t status;
        uint32_t method;
    } cells[] = {
        {"ED-R77", 0x00000077u, ED_METHOD_RETURN},
        {"ED-R00", 0x00000000u, ED_METHOD_RETURN},
        {"ED-RNEG", 0xffffffefu, ED_METHOD_RETURN},
        {"ED-RERR", 0x800201acu, ED_METHOD_RETURN},
        {"ED-X77", 0x00000077u, ED_METHOD_EXIT},
        {"ED-X00", 0x00000000u, ED_METHOD_EXIT},
        {"ED-XNEG", 0xffffffefu, ED_METHOD_EXIT},
        {"ED-XERR", 0x800201acu, ED_METHOD_EXIT},
        {"ED-D77", 0x00000077u, ED_METHOD_EXITDELETE},
        {"ED-D00", 0x00000000u, ED_METHOD_EXITDELETE},
        {"ED-DNEG", 0xffffffefu, ED_METHOD_EXITDELETE},
        {"ED-DERR", 0x800201acu, ED_METHOD_EXITDELETE},
    };
    for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        run_exit_delete_cell(emulated, cells[i].id, cells[i].status,
                             cells[i].method);
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SURVEY
/* Allocator-observed user-memory boundary survey: redesign input for the
   invalid-tail precedence cells, whose compiled-in end assumption
   (0x0A000000) the firmware rejects by successfully allocating there.
   Scans partition-2 fixed-address allocatability upward, freeing every
   success immediately. Thread-free, main-thread only; failures are ordinary
   error codes, never faults. Nothing is written through the surveyed
   addresses. Emits highest provable base + first failure. */
#define DMAC_SURVEY_STEPS 4u
static const uint32_t dmac_survey_bases[DMAC_SURVEY_STEPS] = {
    0x0b740000u, 0x0b780000u, 0x0b7c0000u, 0x0b7e0000u,
};
static void run_dmac_survey(int emulated) {
    uint32_t top_ok = 0;
    uint32_t first_fail = 0;
    uint32_t first_err = 0;
    uint32_t attempts = 0;
    for (uint32_t i = 0; i < DMAC_SURVEY_STEPS; i++) {
        const uint32_t base = dmac_survey_bases[i];
        attempts++;
        const SceUID got = sceKernelAllocPartitionMemory(
            2, "oracle-dmac-survey", PSP_SMEM_Addr, 0x100,
            (void *)(uintptr_t)base);
        if (got < 0) {
            if (first_fail == 0u) {
                first_fail = base;
                first_err = (uint32_t)got;
            }
            continue;
        }
        if (base > top_ok) {
            top_ok = base;
        }
        sceKernelFreePartitionMemory(got);
    }
    const uint32_t out[] = {
        top_ok, first_fail, first_err, attempts, DMAC_SURVEY_STEPS,
    };
    emit_record_extended(emulated, "PSP-DMAC-001", "allocator-survey", "PASS",
                         top_ok, out, sizeof(out) / sizeof(out[0]));
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CTRL_CLOCK
/* Controller timestamp + clock-domain correlations (main thread only, no
   threads created). Timestamp units/epoch are NOT assumed: every cell pairs
   the controller timestamp against sceKernelGetSystemTimeLow so the host
   derives offset/rate empirically. All deltas are raw u32 wraps (host
   interprets). 8-iteration summaries use min/max only (no float). */
#define CC_ITERS 8
static void run_ctrl_clock(int emulated) {
    SceCtrlData pad;
    /* CC-TS-PAIRS: peek timestamp vs system time, back-to-back. */
    {
        uint32_t ts_prev = 0, sys_prev = 0, ts0 = 0, sys0 = 0;
        uint32_t min_dts = 0xffffffffu, max_dts = 0;
        uint32_t min_dsys = 0xffffffffu, max_dsys = 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < CC_ITERS; i++) {
            memset(&pad, 0, sizeof(pad));
            if (sceCtrlPeekBufferPositive(&pad, 1) < 0) {
                continue;
            }
            const uint32_t sys = sceKernelGetSystemTimeLow();
            if (n == 0) {
                ts0 = pad.TimeStamp;
                sys0 = sys;
            } else {
                /* wrap-safe forward deltas via unsigned arithmetic */
                const uint32_t fwd_ts = pad.TimeStamp - ts_prev;
                const uint32_t fwd_sys = sys - sys_prev;
                if (fwd_ts < min_dts) {
                    min_dts = fwd_ts;
                }
                if (fwd_ts > max_dts) {
                    max_dts = fwd_ts;
                }
                if (fwd_sys < min_dsys) {
                    min_dsys = fwd_sys;
                }
                if (fwd_sys > max_dsys) {
                    max_dsys = fwd_sys;
                }
            }
            ts_prev = pad.TimeStamp;
            sys_prev = sys;
            n++;
        }
        const uint32_t out[] = {n, min_dts, max_dts, min_dsys, max_dsys, ts0,
                                sys0};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "ctrl-ts-pairs",
                             n > 1 ? "PASS" : "FAIL", n, out, 7);
    }
    /* CC-TS-VCOUNT: vcount vs peek timestamp. */
    {
        uint32_t v_prev = 0, ts_prev = 0, v0 = 0, ts00 = 0;
        uint32_t min_dv = 0xffffffffu, max_dv = 0;
        uint32_t min_dts = 0xffffffffu, max_dts = 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < CC_ITERS; i++) {
            const uint32_t v = sceDisplayGetVcount();
            memset(&pad, 0, sizeof(pad));
            if (sceCtrlPeekBufferPositive(&pad, 1) < 0) {
                continue;
            }
            if (n == 0) {
                v0 = v;
                ts00 = pad.TimeStamp;
            } else {
                const uint32_t dv = v - v_prev;
                const uint32_t dts = pad.TimeStamp - ts_prev;
                if (dv < min_dv) {
                    min_dv = dv;
                }
                if (dv > max_dv) {
                    max_dv = dv;
                }
                if (dts < min_dts) {
                    min_dts = dts;
                }
                if (dts > max_dts) {
                    max_dts = dts;
                }
            }
            v_prev = v;
            ts_prev = pad.TimeStamp;
            n++;
        }
        const uint32_t out[] = {n, min_dv, max_dv, min_dts, max_dts, v0, ts00};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "ctrl-ts-vcount",
                             n > 1 ? "PASS" : "FAIL", n, out, 7);
    }
    /* CC-SNAPSHOT: one clock-domain anchor row. */
    {
        const uint32_t sys = sceKernelGetSystemTimeLow();
        const uint32_t v = sceDisplayGetVcount();
        const uint32_t h = sceDisplayGetAccumulatedHcount();
        uint64_t tick = 0;
        const int rtc_rc = sceRtcGetCurrentTick(&tick);
        const uint32_t mhz = (uint32_t)scePowerGetCpuClockFrequencyInt();
        const uint32_t out[] = {sys, v, h, (uint32_t)(tick & 0xffffffffu),
                                (uint32_t)(tick >> 32), mhz,
                                (uint32_t)rtc_rc};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "clock-snapshot",
                             rtc_rc == 0 ? "PASS" : "FAIL", sys, out, 7);
    }
    /* CC-DELAY: 10 ms DelayThread ground truth. */
    {
        const uint32_t before = sceKernelGetSystemTimeLow();
        sceKernelDelayThread(10000);
        const uint32_t after = sceKernelGetSystemTimeLow();
        const uint32_t out[] = {before, after, after - before, 10000u};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "delay-10ms",
                             "PASS", after - before, out, 4);
    }
    /* CC-ZERO: zero-count read behavior. */
    {
        memset(&pad, 0, sizeof(pad));
        const int rc = sceCtrlReadBufferPositive(&pad, 0);
        const uint32_t out[] = {(uint32_t)pad.TimeStamp};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "ctrl-zero-count",
                             "PASS", (uint32_t)rc, out, 1);
    }
    /* CC-PEEK-READ: does a consuming read change the timestamp? */
    {
        SceCtrlData a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        const int prc = sceCtrlPeekBufferPositive(&a, 1);
        const int rrc = sceCtrlReadBufferPositive(&b, 1);
        const uint32_t out[] = {(uint32_t)a.TimeStamp, (uint32_t)b.TimeStamp,
                                (uint32_t)prc, (uint32_t)rrc};
        emit_record_extended(emulated, "PSP-SYSTEM-001", "ctrl-peek-read",
                             (prc >= 0 && rrc >= 0) ? "PASS" : "FAIL",
                             (uint32_t)(b.TimeStamp - a.TimeStamp), out, 4);
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR
/* COP1/FPU result vectors (main thread only, no threads created). Two
   evidence classes, kept distinct in the report: PINNED cells use inline
   asm for the exact opcode (cvt.w.s/cvt.s.w, FCR31 access); COMPILER cells
   use C operators (the compiler selects the sequence — measured as run,
   opcode not pinned). FCR31 exception enables stay OFF throughout (a trap
   would kill the launch); only RM and FS bits are touched, saved/restored
   per cell. All bit patterns travel as u32 (memcpy-punned, volatile).

   MAXIMALLY HARDENED v3:
   1. Boot FCR31 is captured via explicit cfc1 at the entry of main() and
      recorded as diagnostic data, but NEVER restored during execution
      (the boot FCR31 has IEEE trap enables active, 0x0E00).
   2. FCR31 is cleared to 0 (all traps off, flags 0, RM=RN) at the literal
      first instruction of main() and between every single test cell.
   4. Cell index is maintained in volatile memory diagnostic marker
      g_fpu_cell_index before each cell. The unsafe undeclared $s2 register write
      (which clobbered GCC's emulated local variable in v4) is removed;
      the volatile memory store guarantees strict sequence ordering and zero
      register allocation hazards.
      host0:/fpu_vector_log.txt.
   6. Clean ExitGame only once, zero threads created, 16 expected records.
*/
static volatile uint32_t g_fpu_cell_index = 0;

static inline uint32_t fpu_get_fcr31(void) {
    uint32_t v;
    __asm__ volatile("cfc1 %0, $31" : "=r"(v) :: "memory");
    return v;
}

static inline void fpu_set_fcr31(uint32_t v) {
    __asm__ volatile("ctc1 %0, $31" :: "r"(v) : "memory");
}

/* Traps OFF, RM=RN: the only FCR31 state FP arithmetic may run under here.
   The boot FCR31 is never trusted (it carries exception enables 0x0E00). */
static inline void fpu_quiet(void) {
    fpu_set_fcr31(0);
}

/* PINNED: cvt.w.s honors RM. */
static inline int32_t fpu_cvt_w_s(float f) {
    float w;
    __asm__ volatile("cvt.w.s %0, %1" : "=f"(w) : "f"(f) : "memory");
    int32_t r;
    memcpy(&r, &w, 4);
    return r;
}

/* PINNED: cvt.s.w int->float. */
static inline float fpu_cvt_s_w(int32_t i) {
    float in;
    memcpy(&in, &i, 4);
    float f;
    __asm__ volatile("cvt.s.w %0, %1" : "=f"(f) : "f"(in) : "memory");
    return f;
}

static inline uint32_t f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float u32_f32(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void run_fpu_vector(int emulated, uint32_t boot_fcr31) {
    static const uint32_t inputs[12] = {
        0x3fc00000u, /* 1.5 */
        0x40200000u, /* 2.5 */
        0xbfc00000u, /* -1.5 */
        0xc0200000u, /* -2.5 */
        0x3dccccddu, /* 0.1 */
        0x501502f9u, /* 1e10 */
        0xd01502f9u, /* -1e10 */
        0x7fc00001u, /* quiet NaN, payload 1 */
        0x7f800000u, /* +Inf */
        0xff800000u, /* -Inf */
        0x00000000u, /* +0 */
        0x80000000u, /* -0 */
    };

    /* Cell 0: Diagnostic record of inherited boot FCR31. */
    g_fpu_cell_index = 0;
    {
        const uint32_t out[] = {boot_fcr31};
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-boot-fcr31", "PASS", boot_fcr31, out, 1);
    }

    /* PINNED cells 1..4: cvt.w.s under each RM (0 RN, 1 RZ, 2 RP, 3 RM). */
    for (uint32_t rm = 0; rm < 4; rm++) {
        g_fpu_cell_index = 1 + rm;
        fpu_set_fcr31(rm & 3u); /* RM set, all trap enables strictly 0 */
        uint32_t out[12];
        for (uint32_t i = 0; i < 12; i++) {
            out[i] = (uint32_t)fpu_cvt_w_s(u32_f32(inputs[i]));
        }
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet(); /* clear flags, traps remain 0; NEVER restore boot_fcr31 */
        char cid[32];
        snprintf(cid, sizeof(cid), "fpu-cvt-rm%u", (unsigned int)rm);
        emit_record_extended(emulated, "PSP-FPU-001", cid, "PASS", flags, out, 12);
    }

    /* COMPILER cell 5: C-cast float->int (compiler selects trunc sequence). */
    g_fpu_cell_index = 5;
    {
        uint32_t out[12];
        fpu_quiet();
        for (uint32_t i = 0; i < 12; i++) {
            volatile float vf = u32_f32(inputs[i]);
            out[i] = (uint32_t)(int32_t)vf;
        }
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet();
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-ccast-trunc", "PASS", flags, out, 12);
    }

    /* PINNED cell 6: int->float exactness. */
    g_fpu_cell_index = 6;
    {
        static const int32_t ints[6] = {0, 1, -1, 0x7fffffff, (int32_t)0x80000000,
                                        123456789};
        uint32_t out[6];
        fpu_quiet();
        for (uint32_t i = 0; i < 6; i++) {
            out[i] = f32_bits(fpu_cvt_s_w(ints[i]));
        }
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet();
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-cvt-s-w", "PASS", flags, out, 6);
    }

    /* COMPILER cells 7..11: flag/edge arithmetic (volatile C operators). */
    {
        struct {
            const char *id;
            float a;
            float b;
            uint32_t op; /* 0=mul 1=div 2=add */
        } const ops[] = {
            {"fpu-flag-overflow", 1e30f, 1e30f, 0},
            {"fpu-flag-div0", 1.0f, 0.0f, 1},
            {"fpu-flag-invalid", 0.0f, 0.0f, 1},
            {"fpu-flag-underflow", 1e-30f, 1e-30f, 0},
            {"fpu-flag-inexact", 0.1f, 0.2f, 2},
        };
        for (uint32_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
            g_fpu_cell_index = 7 + k;
            fpu_quiet();
            volatile float va = ops[k].a;
            volatile float vb = ops[k].b;
            volatile float vr = 0;
            if (ops[k].op == 0u) {
                vr = va * vb;
            } else if (ops[k].op == 1u) {
                vr = va / vb;
            } else {
                vr = va + vb;
            }
            const uint32_t bits = f32_bits(vr);
            const uint32_t flags = fpu_get_fcr31();
            fpu_quiet(); /* clear flags; NEVER restore boot_fcr31 */
            const uint32_t out[] = {bits, flags};
            emit_record_extended(emulated, "PSP-FPU-001", ops[k].id, "PASS", flags, out, 2);
        }
    }

    /* COMPILER cell 12: FTZ contrast (FS=1 vs FS=0) on the underflow op. */
    g_fpu_cell_index = 12;
    {
        volatile float va = 1e-30f;
        volatile float vb = 1e-30f;
        fpu_quiet();
        volatile float r0 = va * vb;
        const uint32_t b0 = f32_bits(r0);
        const uint32_t f0 = fpu_get_fcr31();
        fpu_set_fcr31(0x01000000u); /* FS=1, all enables strictly 0 */
        volatile float r1 = va * vb;
        const uint32_t b1 = f32_bits(r1);
        const uint32_t f1 = fpu_get_fcr31();
        fpu_quiet(); /* clear flags and FS */
        const uint32_t out[] = {b0, f0, b1, f1};
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-ftz-contrast", "PASS", b0 ^ b1, out, 4);
    }

    /* COMPILER cell 13: signed-zero behaviors. */
    g_fpu_cell_index = 13;
    {
        volatile float pz = 0.0f;
        volatile float nz = u32_f32(0x80000000u);
        volatile float inf = u32_f32(0x7f800000u);
        fpu_quiet();
        const uint32_t out[] = {
            f32_bits(pz + nz), f32_bits(nz + nz), f32_bits(1.0f / pz),
            f32_bits(1.0f / nz), f32_bits(pz * inf), f32_bits(nz * inf),
        };
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet();
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-signed-zero", "PASS", flags, out, 6);
    }

    /* COMPILER cell 14: NaN payload propagation through + and *. */
    g_fpu_cell_index = 14;
    {
        static const uint32_t nans[3] = {0x7fc00001u, 0x7fffffffu, 0xffc00001u};
        uint32_t out[6];
        fpu_quiet();
        for (uint32_t i = 0; i < 3; i++) {
            volatile float vn = u32_f32(nans[i]);
            volatile float vo = 1.0f;
            out[2 * i] = f32_bits(vn + vo);
            out[2 * i + 1] = f32_bits(vn * vo);
        }
        const uint32_t flags = fpu_get_fcr31();
        fpu_quiet();
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-nan-payload", "PASS", flags, out, 6);
    }

    /* Cell 15: Done record confirming all preceding cells executed without trap. */
    g_fpu_cell_index = 15;
    fpu_quiet();
    {
        const uint32_t out[] = {15};
        emit_record_extended(emulated, "PSP-FPU-001", "fpu-done", "PASS", 0, out, 1);
    }
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_TEARDOWN_TEST
/* Teardown-method experiment: does ending main via sceKernelExitDeleteThread
   (instead of returning into sceKernelExitGame) avoid poisoning the boot's
   thread table for the NEXT launch? Main-thread only, no other threads.
   Emits one record, then ExitDeleteThread(0). The DIAGNOSIS is the next
   launch (any binary): loads+passes => teardown clean; startup-crash =>
   same poison. NEVER run anything after this except the diagnostic. */
static void run_teardown_test(int emulated) {
    const uint32_t self = (uint32_t)sceKernelGetThreadId();
    const uint32_t out[] = {self};
    emit_record_extended(emulated, "PSP-TEARDOWN-001", "exitdelete-main",
                         "PASS", self, out, 1);
    sceKernelExitDeleteThread(0);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_IO_MATRIX
/* 6 measurement cells + 1 completion sentinel. */
_Static_assert(DEFERRED_MAX_RECORDS >= 7, "io-matrix needs 7 deferred slots");

static void run_io_matrix(int emulated) {
    const char *test_path = "host0:/test_io_matrix.tmp";
    uint32_t out[6];

    /* Cell 1: io-open-create */
    memset(out, 0xFF, sizeof(out));
    SceUID fd = sceIoOpen(test_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    out[0] = (uint32_t)fd;
    defer_record("io-open-create",
                 fd >= 0 ? "PASS" : "FAIL", (uint32_t)fd, out, 1);

    if (fd >= 0) {
        /* Cell 2: io-write */
        memset(out, 0xFF, sizeof(out));
        uint8_t wbuf[64];
        memset(wbuf, 0x5A, sizeof(wbuf));
        int written = sceIoWrite(fd, wbuf, sizeof(wbuf));
        int close_rc = sceIoClose(fd);
        out[0] = (uint32_t)written;
        out[1] = (uint32_t)close_rc;
        defer_record("io-write",
                     (written == 64 && close_rc == 0) ? "PASS" : "FAIL",
                     (uint32_t)written, out, 2);
    }

    /* Cell 3: io-read-verify */
    memset(out, 0xFF, sizeof(out));
    fd = sceIoOpen(test_path, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        uint8_t rbuf[64];
        memset(rbuf, 0, sizeof(rbuf));
        int nread = sceIoRead(fd, rbuf, sizeof(rbuf));
        int match = 1;
        for (int i = 0; i < 64; i++) {
            if (rbuf[i] != 0x5A) { match = 0; break; }
        }
        out[0] = (uint32_t)nread;
        out[1] = (uint32_t)match;
        defer_record("io-read-verify",
                     (nread == 64 && match) ? "PASS" : "FAIL",
                     (uint32_t)nread, out, 2);

        /* Cell 4: io-lseek */
        memset(out, 0xFF, sizeof(out));
        SceOff s_set = sceIoLseek(fd, 32, PSP_SEEK_SET);
        SceOff s_cur = sceIoLseek(fd, -16, PSP_SEEK_CUR);
        SceOff s_end = sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoClose(fd);
        out[0] = (uint32_t)s_set;
        out[1] = (uint32_t)s_cur;
        out[2] = (uint32_t)s_end;
        defer_record("io-lseek",
                     (s_set == 32 && s_cur == 16 && s_end == 64) ? "PASS" : "FAIL",
                     (uint32_t)s_end, out, 3);
    } else {
        out[0] = (uint32_t)fd;
        defer_record("io-read-verify", "FAIL", (uint32_t)fd, out, 1);
    }

    /* Cell 5: io-append */
    memset(out, 0xFF, sizeof(out));
    fd = sceIoOpen(test_path, PSP_O_WRONLY | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        uint8_t abuf[32];
        memset(abuf, 0xA5, sizeof(abuf));
        int app_written = sceIoWrite(fd, abuf, sizeof(abuf));
        sceIoClose(fd);

        fd = sceIoOpen(test_path, PSP_O_RDONLY, 0777);
        SceOff total_sz = sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoClose(fd);
        out[0] = (uint32_t)app_written;
        out[1] = (uint32_t)total_sz;
        defer_record("io-append",
                     (app_written == 32 && total_sz == 96) ? "PASS" : "FAIL",
                     (uint32_t)total_sz, out, 2);
    } else {
        out[0] = (uint32_t)fd;
        defer_record("io-append", "FAIL", (uint32_t)fd, out, 1);
    }

    /* Cell 6: io-errors & cleanup */
    memset(out, 0xFF, sizeof(out));
    SceUID bad_open = sceIoOpen("host0:/__nonexistent_file_matrix_xyz__.tmp", PSP_O_RDONLY, 0777);
    int bad_read = sceIoRead(-1, out, 4);
    int rem_rc = sceIoRemove(test_path);
    out[0] = (uint32_t)bad_open;
    out[1] = (uint32_t)bad_read;
    out[2] = (uint32_t)rem_rc;
    defer_record("io-errors",
                 (bad_open < 0 && bad_read < 0 && rem_rc == 0) ? "PASS" : "FAIL",
                 (uint32_t)bad_open, out, 3);

    /* Every IoFileMgr measurement is now complete.  The completion sentinel
       carries the number of semantic records actually captured, so a truncated
       transport is detectable against the emitted stream length. */
    const uint32_t done_out[1] = {(uint32_t)s_deferred_count};
    defer_record("io-done", "PASS", 0, done_out, 1);

    flush_deferred(emulated, "PSP-IO-001");
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY
static void run_audio_query(int emulated) {
    uint32_t out[6];

    /* Cell 1: audio-ch-reserve */
    memset(out, 0xFF, sizeof(out));
    int res0 = sceAudioChReserve(0, 512, PSP_AUDIO_FORMAT_STEREO);
    int rest0 = sceAudioGetChannelRestLen(0);
    out[0] = (uint32_t)res0;
    out[1] = (uint32_t)rest0;
    emit_record_extended(emulated, "PSP-AUDIO-001", "audio-ch-reserve",
                         res0 == 0 ? "PASS" : "FAIL", (uint32_t)res0, out, 2);

    /* Cell 2: audio-ch-release */
    memset(out, 0xFF, sizeof(out));
    int rel0 = sceAudioChRelease(0);
    int rel_again = sceAudioChRelease(0);
    out[0] = (uint32_t)rel0;
    out[1] = (uint32_t)rel_again;
    emit_record_extended(emulated, "PSP-AUDIO-001", "audio-ch-release",
                         (rel0 == 0 && rel_again < 0) ? "PASS" : "FAIL", (uint32_t)rel0, out, 2);

    /* Cell 3: audio-out2-query */
    memset(out, 0xFF, sizeof(out));
    int out2_res = sceAudioOutput2Reserve(512);
    int out2_rest = sceAudioOutput2GetRestSample();
    int out2_rel = sceAudioOutput2Release();
    out[0] = (uint32_t)out2_res;
    out[1] = (uint32_t)out2_rest;
    out[2] = (uint32_t)out2_rel;
    emit_record_extended(emulated, "PSP-AUDIO-001", "audio-out2-query",
                         (out2_res == 0 && out2_rel == 0) ? "PASS" : "FAIL",
                         (uint32_t)out2_res, out, 3);

    /* Cell 4: audio-src-reserve */
    memset(out, 0xFF, sizeof(out));
    int src_res = sceAudioSRCChReserve(512, 44100, 2);
    int src_rel = sceAudioSRCChRelease();
    out[0] = (uint32_t)src_res;
    out[1] = (uint32_t)src_rel;
    emit_record_extended(emulated, "PSP-AUDIO-001", "audio-src-reserve",
                         (src_res == 0 && src_rel == 0) ? "PASS" : "FAIL",
                         (uint32_t)src_res, out, 2);

    /* Cell 5: Completion sentinel */
    uint32_t done_out[1] = {4u};
    emit_record_extended(emulated, "PSP-AUDIO-001", "audio-done", "PASS", 0, done_out, 1);
}
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CACHE_ALIAS
/* 4 measurement cells + 1 completion sentinel. */
_Static_assert(DEFERRED_MAX_RECORDS >= 5, "cache-alias needs 5 deferred slots");

static uint32_t s_cache_buf[64] __attribute__((aligned(64)));

static void run_cache_alias(int emulated) {
    uint32_t out[6];
    volatile uint32_t *c_ptr = s_cache_buf;
    volatile uint32_t *u_ptr = (volatile uint32_t *)((uintptr_t)s_cache_buf | 0x40000000u);

    /* Cell 1: cache-alias-init (Uncached write visible after invalidate) */
    memset(out, 0xFF, sizeof(out));
    u_ptr[0] = 0x11223344u;
    sceKernelDcacheInvalidateRange((void *)c_ptr, 64);
    uint32_t c_read1 = c_ptr[0];
    out[0] = u_ptr[0];
    out[1] = c_read1;
    out[2] = (c_read1 == 0x11223344u) ? 1u : 0u;
    defer_record("cache-alias-init",
                 c_read1 == 0x11223344u ? "PASS" : "FAIL", c_read1, out, 3);

    /* Cell 2: cache-writeback-contrast (Cached write vs uncached visibility before/after WB) */
    memset(out, 0xFF, sizeof(out));
    c_ptr[0] = 0x55667788u;
    uint32_t u_before = u_ptr[0];
    sceKernelDcacheWritebackRange((void *)c_ptr, 64);
    uint32_t u_after = u_ptr[0];
    out[0] = u_before;
    out[1] = u_after;
    out[2] = (u_after == 0x55667788u) ? 1u : 0u;
    defer_record("cache-writeback-contrast",
                 u_after == 0x55667788u ? "PASS" : "FAIL", u_after, out, 3);

    /* Cell 3: cache-inval-contrast (Uncached write vs stale cached read before/after inval) */
    memset(out, 0xFF, sizeof(out));
    u_ptr[0] = 0x99AABBCCu;
    uint32_t c_stale = c_ptr[0];
    sceKernelDcacheInvalidateRange((void *)c_ptr, 64);
    uint32_t c_fresh = c_ptr[0];
    out[0] = c_stale;
    out[1] = c_fresh;
    out[2] = (c_fresh == 0x99AABBCCu) ? 1u : 0u;
    defer_record("cache-inval-contrast",
                 c_fresh == 0x99AABBCCu ? "PASS" : "FAIL", c_fresh, out, 3);

    /* Cell 4: cache-wball (Writeback all lines) */
    memset(out, 0xFF, sizeof(out));
    c_ptr[1] = 0xDEADBEEFu;
    sceKernelDcacheWritebackAll();
    uint32_t u_wball = u_ptr[1];
    out[0] = u_wball;
    out[1] = (u_wball == 0xDEADBEEFu) ? 1u : 0u;
    defer_record("cache-wball",
                 u_wball == 0xDEADBEEFu ? "PASS" : "FAIL", u_wball, out, 2);

    /* Every dcache/alias measurement is now complete.  Emission below is the
       first host0 or stdout activity since the probe began, so no line
       residency observed above was perturbed by logging. */
    const uint32_t done_out[1] = {(uint32_t)s_deferred_count};
    defer_record("cache-done", "PASS", 0, done_out, 1);

    flush_deferred(emulated, "PSP-CACHE-001");
}
#endif

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR
    /* Capture boot FCR31 immediately at process entry before ANY other code runs */
    uint32_t boot_fcr31 = 0;
    __asm__ volatile("cfc1 %0, $31" : "=r"(boot_fcr31) :: "memory");
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
#endif
    const int emulated = emulator_present();
    /* Unbuffered stdout so a probe-induced exception stays attributable to
       the exact record instead of losing buffered output. Zero semantic
       effect on emitted records. */
    setvbuf(stdout, NULL, _IONBF, 0);
    char line[320];

    /* uint32_t is `unsigned long` in the PSP newlib ABI, so %x must be fed an
       explicitly-converted unsigned int or psp-gcc warns under -Wformat. */
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_META schema=1 source=%s model=unknown firmware=unknown "
             "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
             "source_commit=0000000000000000000000000000000000000000 fixture=%s\n",
             emulated ? "ppsspp" : "psp", FIXTURE_BUILD_ID);
    emit(emulated, line);
#ifdef PROBE_HOST0_LOG
    if (!emulated) {
        SceUID fd = sceIoOpen(PROBE_HOST0_LOG,
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, line, strlen(line));
            sceIoClose(fd);
        }
    }
#endif

#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_CALLBACK
    uint32_t out0 = 0;
    uint32_t out1 = 0;
    uint32_t out2 = 0;
    uint32_t out3 = 0;
    const int pass = run_callback_case(&out0, &out1, &out2, &out3);
    emit_test(emulated, "callback-notify-check", pass, pass ? 1u : 0u,
              out0, out1, out2, out3);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_WAIT_CANCEL
    uint32_t out0 = 0;
    uint32_t out1 = 0;
    uint32_t out2 = 0;
    uint32_t out3 = 0;
    const int pass = (int)run_wait_cancel_case(&out0, &out1, &out2, &out3);
    emit_test(emulated, "wait-cancel", pass, pass ? 1u : 0u,
              out0, out1, out2, out3);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_LIFECYCLE
    uint32_t out0 = 0;
    uint32_t out1 = 0;
    uint32_t out2 = 0;
    uint32_t out3 = 0;
    const int pass = run_thread_lifecycle_case(&out0, &out1, &out2, &out3);
    emit_test(emulated, "thread-lifecycle", pass, pass ? 1u : 0u,
              out0, out1, out2, out3);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE
    uint32_t out[10] = {0};
    const int pass = (int)run_thread_delete_case(&out[0], &out[1], &out[2], &out[3],
                                                 &out[4], &out[5], &out[6], &out[7], &out[8], &out[9]);
    emit_test_extended(emulated, "thread-delete-lifecycle", pass, pass ? 1u : 0u,
                       out, sizeof(out) / sizeof(out[0]));
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_FOLLOWUP
    uint32_t out[17] = {0};
    const int pass = (int)run_thread_delete_followup_case(
        &out[0], &out[1], &out[2], &out[3], &out[4], &out[5], &out[6], &out[7],
        &out[8], &out[9], &out[10], &out[11], &out[12], &out[13], &out[14],
        &out[15], &out[16], 0);
    emit_test_extended(emulated, "thread-delete-followup", pass, pass ? 1u : 0u,
                       out, 15);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_EXPLICIT
    uint32_t out[17] = {0};
    const int pass = (int)run_thread_delete_followup_case(
        &out[0], &out[1], &out[2], &out[3], &out[4], &out[5], &out[6], &out[7],
        &out[8], &out[9], &out[10], &out[11], &out[12], &out[13], &out[14],
        &out[15], &out[16], 1);
    emit_test_extended(emulated, "thread-delete-explicit", pass, pass ? 1u : 0u,
                       out, 15);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_DELETE_BOUNDARY
    uint32_t out[17] = {0};
    const int pass = (int)run_thread_delete_followup_case(
        &out[0], &out[1], &out[2], &out[3], &out[4], &out[5], &out[6], &out[7],
        &out[8], &out[9], &out[10], &out[11], &out[12], &out[13], &out[14],
        &out[15], &out[16], 2);
    emit_test_extended(emulated, "thread-delete-boundary", pass, pass ? 1u : 0u,
                       out, sizeof(out) / sizeof(out[0]));
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY
    run_dmac_concurrency(emulated);
#elif PSP_ORACLE_CASE >= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST && \
      PSP_ORACLE_CASE <= PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC
    run_dmac_invalid_tail(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_VCOUNT
    run_display_mask_vcount(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_MASK_DUTY
    run_display_mask_duty(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DISPLAY_GE_MASK
    run_display_ge_mask(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_TRANSPORT_WRITE
    uint32_t tout[5] = {0};
    const int tpass = run_transport_write_case(emulated, tout);
    if (tpass != 0) {
        emit_record_extended(emulated, "PSP-TRANSPORT-001",
                             "host0-write-readback", "FAIL",
                             (uint32_t)tpass, tout, 5);
    }
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_THREAD_EXIT_DELETE
    run_thread_exit_delete(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_SURVEY
    run_dmac_survey(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_CTRL_CLOCK
    run_ctrl_clock(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_FPU_VECTOR
    run_fpu_vector(emulated, boot_fcr31);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_TEARDOWN_TEST
    run_teardown_test(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_IO_MATRIX
    run_io_matrix(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_AUDIO_QUERY
    run_audio_query(emulated);
#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_CACHE_ALIAS
    run_cache_alias(emulated);
#else
    const uint32_t sum = nakagawa_psp_oracle_sum_u32(100);
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SMOKE-001 case_id=sum-1-to-100 "
             "status=%s result=0x%08x out0=0x%08x\n",
             sum == 5050 ? "PASS" : "FAIL", (unsigned int)sum, sum == 5050 ? 1u : 0u);
    emit(emulated, line);
#endif

    /* Returning is equivalent to calling sceKernelExitGame() explicitly: the
       PSPSDK CRT emits `jal sceKernelExitGame` in _main once main() returns
       (confirmed with psp-objdump on this fixture). The explicit call was
       dropped only because it was redundant -- it does NOT stop PSPLINK from
       resetting between probes.

       PSPLINK's reset is controlled by `resetonexit` in psplink.ini. With
       resetonexit=1 it calls psplinkStop() then sceKernelLoadExec to reload
       itself, which re-enumerates the USB endpoint on every probe. Set
       resetonexit=0 on the Memory Stick for multi-probe sessions. */
    return 0;
}
