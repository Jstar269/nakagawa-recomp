// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspdmac.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <psputils.h>
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
    }
}

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
#define DMAC_BASELINE_USER_END 0x0a000000u
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

    /* This allocation is an observational safety gate.  If partition 2 can
       allocate at or above the assumed end, no invalid DMA call is issued. */
    const SceUID tail_block = sceKernelAllocPartitionMemory(
        2, "oracle-dmac-tail-check", PSP_SMEM_Addr, 0x100,
        (void *)DMAC_BASELINE_USER_END);
    if (tail_block >= 0) {
        sceKernelFreePartitionMemory(tail_block);
        sceKernelFreePartitionMemory(block);
        emit_dmac_invalid_setup(emulated, "SKIP", 0, setup_mask, 0);
        return;
    }
    setup_mask |= 4u;

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
        (uint32_t)tail_block,
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

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const int emulated = emulator_present();
    char line[320];

    /* uint32_t is `unsigned long` in the PSP newlib ABI, so %x must be fed an
       explicitly-converted unsigned int or psp-gcc warns under -Wformat. */
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_META schema=1 source=%s model=unknown firmware=unknown "
             "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
             "source_commit=0000000000000000000000000000000000000000 fixture=%s\n",
             emulated ? "ppsspp" : "psp", FIXTURE_BUILD_ID);
    emit(emulated, line);

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
