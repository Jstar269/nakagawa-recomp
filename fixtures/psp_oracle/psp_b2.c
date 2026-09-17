// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// PSP-B2: contended-wait semantics with one bounded helper thread per cell
// (one launch per boot; this is the boot's only thread-creating launch).
//
// The helper runs at one priority step above the main thread, so it runs as
// soon as it is started or woken. Every wait has a timeout of at most 500 ms,
// every helper is joined with a 1 s timeout and terminated if the join fails,
// and every result line is written to host0:/psp_b2_results.txt before the next
// call. Thread and object status buffers are recorded as raw words.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_B2_WAIT_PROBE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef B2_BUILD_COMMIT
#error B2_BUILD_COMMIT is required
#endif

int sceKernelCancelSema(SceUID semaid, int newCount, int *numWaitThreads);
int sceKernelCancelEventFlag(SceUID evid, u32 newPattern, int *numWaitThreads);

#define B2_LOG "host0:/psp_b2_results.txt"
#define USER_PARTITION 2
#define WAIT_US 500000u
#define STEP_US 20000

#define SEMA_INFO_SIZE 56u
#define EVF_INFO_SIZE 52u
#define FPL_INFO_SIZE 56u
#define VPL_INFO_SIZE 52u
#define THREAD_INFO_SIZE 104u

static SceUID g_log = -1;
static SceUID g_obj = -1;
static SceLwMutexWorkarea g_lw;
static void *g_block;
static const char *g_fpl_cell = "fpl_wake";

static void emit(const char *line) {
    if (g_log >= 0) sceIoWrite(g_log, line, strlen(line));
}

static void rc_line(const char *cell, const char *call, int rc) {
    char line[160];
    snprintf(line, sizeof(line), "cell=%s call=%s rc=0x%08x\n", cell, call, (unsigned int)rc);
    emit(line);
}

static void val_line(const char *cell, const char *what, uint32_t value) {
    char line[160];
    snprintf(line, sizeof(line), "cell=%s %s=0x%08x\n", cell, what, (unsigned int)value);
    emit(line);
}

static void words_line(const char *cell, const char *call, int rc, const uint32_t *buf, int words) {
    char line[900];
    int n = snprintf(line, sizeof(line), "cell=%s call=%s rc=0x%08x words=", cell, call, (unsigned int)rc);
    for (int i = 0; i < words && n > 0 && n < (int)sizeof(line) - 12; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%08x", i ? "," : "", (unsigned int)buf[i]);
    }
    if (n > 0 && n < (int)sizeof(line) - 1) {
        line[n++] = '\n';
        line[n] = '\0';
        emit(line);
    }
}

static int refer_sema(SceUID uid, void *b) { return sceKernelReferSemaStatus(uid, b); }
static int refer_evf(SceUID uid, void *b) { return sceKernelReferEventFlagStatus(uid, b); }
static int refer_fpl(SceUID uid, void *b) { return sceKernelReferFplStatus(uid, b); }
static int refer_vpl(SceUID uid, void *b) { return sceKernelReferVplStatus(uid, b); }
static int refer_thread(SceUID uid, void *b) { return sceKernelReferThreadStatus(uid, b); }

static void refer(const char *cell, const char *call, int (*fn)(SceUID, void *), SceUID uid,
                  uint32_t size) {
    uint32_t buf[32];
    memset(buf, 0, sizeof(buf));
    buf[0] = size;
    int rc = fn(uid, buf);
    words_line(cell, call, rc, buf, (int)(size + 3) / 4);
}

static void lw_line(const char *cell, const char *call, int rc) {
    uint32_t w[5];
    memcpy(w, &g_lw, sizeof(w));
    words_line(cell, call, rc, w, 5);
}

enum {
    H_SEMA_SIGNAL_LATE = 1,
    H_SEMA_DELETE_LATE,
    H_SEMA_CANCEL_LATE,
    H_SEMA_WAIT,
    H_EVF_SET_LATE,
    H_EVF_WAIT,
    H_LW_CONTEND,
    H_FPL_WAIT,
    H_VPL_WAIT,
};

static int helper(SceSize args, void *argp) {
    int mode = (args >= sizeof(int) && argp) ? *(int *)argp : 0;
    SceUInt tmo = WAIT_US;
    u32 out = 0xdeadbeef;
    int n = -7;
    int rc;
    void *p = (void *)0x1;
    switch (mode) {
    case H_SEMA_SIGNAL_LATE:
        sceKernelDelayThread(STEP_US);
        rc_line("sema_wake", "helper.SignalSema", sceKernelSignalSema(g_obj, 1));
        break;
    case H_SEMA_DELETE_LATE:
        sceKernelDelayThread(STEP_US);
        rc_line("sema_delete", "helper.DeleteSema", sceKernelDeleteSema(g_obj));
        break;
    case H_SEMA_CANCEL_LATE:
        sceKernelDelayThread(STEP_US);
        rc = sceKernelCancelSema(g_obj, -1, &n);
        rc_line("sema_cancel", "helper.CancelSema.newcount_neg1", rc);
        val_line("sema_cancel", "helper.CancelSema.numWaitThreads", (uint32_t)n);
        break;
    case H_SEMA_WAIT:
        rc = sceKernelWaitSema(g_obj, 1, &tmo);
        rc_line("sema_helper_waits", "helper.WaitSema", rc);
        val_line("sema_helper_waits", "helper.WaitSema.timeout_left", tmo);
        break;
    case H_EVF_SET_LATE:
        sceKernelDelayThread(STEP_US);
        rc_line("evf_wake", "helper.SetEventFlag.03", sceKernelSetEventFlag(g_obj, 0x03));
        break;
    case H_EVF_WAIT:
        rc = sceKernelWaitEventFlag(g_obj, 0x01, PSP_EVENT_WAITOR, &out, &tmo);
        rc_line("evf_second_waiter", "helper.WaitEventFlag", rc);
        val_line("evf_second_waiter", "helper.WaitEventFlag.out", out);
        val_line("evf_second_waiter", "helper.WaitEventFlag.timeout_left", tmo);
        break;
    case H_LW_CONTEND: {
        const char *c = "lw_contend";
        lw_line(c, "helper.TryLockLwMutex.held_by_main", sceKernelTryLockLwMutex(&g_lw, 1));
        lw_line(c, "helper.UnlockLwMutex.not_owner", sceKernelUnlockLwMutex(&g_lw, 1));
        tmo = STEP_US / 2;
        rc = sceKernelLockLwMutex(&g_lw, 1, &tmo);
        lw_line(c, "helper.LockLwMutex.10ms", rc);
        val_line(c, "helper.LockLwMutex.10ms.timeout_left", tmo);
        tmo = WAIT_US;
        rc = sceKernelLockLwMutex(&g_lw, 1, &tmo);
        lw_line(c, "helper.LockLwMutex.500ms", rc);
        val_line(c, "helper.LockLwMutex.500ms.timeout_left", tmo);
        if (rc == 0) lw_line(c, "helper.UnlockLwMutex.after_wake", sceKernelUnlockLwMutex(&g_lw, 1));
        break;
    }
    case H_FPL_WAIT:
        rc = sceKernelAllocateFpl(g_obj, &p, &tmo);
        rc_line(g_fpl_cell, "helper.AllocateFpl", rc);
        val_line(g_fpl_cell, "helper.AllocateFpl.same_block", p == g_block);
        val_line(g_fpl_cell, "helper.AllocateFpl.timeout_left", tmo);
        if (rc == 0) rc_line(g_fpl_cell, "helper.FreeFpl", sceKernelFreeFpl(g_obj, p));
        break;
    case H_VPL_WAIT:
        rc = sceKernelAllocateVpl(g_obj, 100, &p, &tmo);
        rc_line("vpl_wake", "helper.AllocateVpl.100", rc);
        val_line("vpl_wake", "helper.AllocateVpl.block", (uint32_t)(uintptr_t)p);
        val_line("vpl_wake", "helper.AllocateVpl.timeout_left", tmo);
        if (rc == 0) rc_line("vpl_wake", "helper.FreeVpl", sceKernelFreeVpl(g_obj, p));
        break;
    default:
        break;
    }
    return 0;
}

static int g_helper_prio;

static SceUID start_helper(const char *cell, int mode) {
    SceUID uid = sceKernelCreateThread("b2helper", helper, g_helper_prio, 0x2000, THREAD_ATTR_USER, NULL);
    rc_line(cell, "CreateThread.helper", uid);
    if (uid < 0) return uid;
    int rc = sceKernelStartThread(uid, sizeof(mode), &mode);
    rc_line(cell, "StartThread.helper", rc);
    return uid;
}

static void join_helper(const char *cell, SceUID uid) {
    if (uid < 0) return;
    SceUInt tmo = 1000000;
    int rc = sceKernelWaitThreadEnd(uid, &tmo);
    rc_line(cell, "WaitThreadEnd.helper", rc);
    if (rc < 0) {
        rc_line(cell, "TerminateDeleteThread.helper", sceKernelTerminateDeleteThread(uid));
    } else {
        rc_line(cell, "DeleteThread.helper", sceKernelDeleteThread(uid));
    }
}

static void cell_sema_wake(void) {
    const char *c = "sema_wake";
    g_obj = sceKernelCreateSema("b2s1", 0, 0, 1, NULL);
    rc_line(c, "CreateSema", g_obj);
    if (g_obj < 0) return;
    SceUID h = start_helper(c, H_SEMA_SIGNAL_LATE);
    SceUInt tmo = WAIT_US;
    int rc = sceKernelWaitSema(g_obj, 1, &tmo);
    rc_line(c, "main.WaitSema", rc);
    val_line(c, "main.WaitSema.timeout_left", tmo);
    join_helper(c, h);
    refer(c, "ReferSemaStatus.end", refer_sema, g_obj, SEMA_INFO_SIZE);
    rc_line(c, "DeleteSema", sceKernelDeleteSema(g_obj));
}

static void cell_sema_delete(void) {
    const char *c = "sema_delete";
    g_obj = sceKernelCreateSema("b2s2", 0, 0, 1, NULL);
    rc_line(c, "CreateSema", g_obj);
    if (g_obj < 0) return;
    SceUID h = start_helper(c, H_SEMA_DELETE_LATE);
    SceUInt tmo = WAIT_US;
    int rc = sceKernelWaitSema(g_obj, 1, &tmo);
    rc_line(c, "main.WaitSema", rc);
    val_line(c, "main.WaitSema.timeout_left", tmo);
    join_helper(c, h);
    rc_line(c, "DeleteSema.after", sceKernelDeleteSema(g_obj));
}

static void cell_sema_cancel(void) {
    const char *c = "sema_cancel";
    g_obj = sceKernelCreateSema("b2s3", 0, 1, 3, NULL);
    rc_line(c, "CreateSema.init1_max3", g_obj);
    if (g_obj < 0) return;
    SceUInt tmo = WAIT_US;
    SceUID h = start_helper(c, H_SEMA_CANCEL_LATE);
    int rc = sceKernelWaitSema(g_obj, 2, &tmo);
    rc_line(c, "main.WaitSema.2", rc);
    val_line(c, "main.WaitSema.timeout_left", tmo);
    join_helper(c, h);
    refer(c, "ReferSemaStatus.after_cancel_neg1", refer_sema, g_obj, SEMA_INFO_SIZE);
    int n = -7;
    rc = sceKernelCancelSema(g_obj, 3, &n);
    rc_line(c, "CancelSema.newcount3.no_waiters", rc);
    val_line(c, "CancelSema.newcount3.numWaitThreads", (uint32_t)n);
    refer(c, "ReferSemaStatus.after_cancel_3", refer_sema, g_obj, SEMA_INFO_SIZE);
    rc_line(c, "CancelSema.newcount4.overmax", sceKernelCancelSema(g_obj, 4, &n));
    rc_line(c, "CancelSema.null_numwait", sceKernelCancelSema(g_obj, 0, NULL));
    refer(c, "ReferSemaStatus.end", refer_sema, g_obj, SEMA_INFO_SIZE);
    rc_line(c, "DeleteSema", sceKernelDeleteSema(g_obj));
}

static void cell_sema_helper_waits(void) {
    const char *c = "sema_helper_waits";
    g_obj = sceKernelCreateSema("b2s4", 0, 0, 1, NULL);
    rc_line(c, "CreateSema", g_obj);
    if (g_obj < 0) return;
    SceUID h = start_helper(c, H_SEMA_WAIT);
    refer(c, "ReferSemaStatus.helper_waiting", refer_sema, g_obj, SEMA_INFO_SIZE);
    if (h >= 0) refer(c, "ReferThreadStatus.helper_waiting", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "main.SignalSema", sceKernelSignalSema(g_obj, 1));
    rc_line(c, "main.after_signal_marker", 0);
    join_helper(c, h);
    refer(c, "ReferSemaStatus.end", refer_sema, g_obj, SEMA_INFO_SIZE);
    rc_line(c, "DeleteSema", sceKernelDeleteSema(g_obj));
}

static void cell_evf_wake(void) {
    const char *c = "evf_wake";
    g_obj = sceKernelCreateEventFlag("b2e1", PSP_EVENT_WAITSINGLE, 0, NULL);
    rc_line(c, "CreateEventFlag", g_obj);
    if (g_obj < 0) return;
    SceUID h = start_helper(c, H_EVF_SET_LATE);
    SceUInt tmo = WAIT_US;
    u32 out = 0xdeadbeef;
    int rc = sceKernelWaitEventFlag(g_obj, 0x01, PSP_EVENT_WAITAND | PSP_EVENT_WAITCLEAR, &out, &tmo);
    rc_line(c, "main.WaitEventFlag.and_clear", rc);
    val_line(c, "main.WaitEventFlag.out", out);
    val_line(c, "main.WaitEventFlag.timeout_left", tmo);
    join_helper(c, h);
    refer(c, "ReferEventFlagStatus.end", refer_evf, g_obj, EVF_INFO_SIZE);
    rc_line(c, "DeleteEventFlag", sceKernelDeleteEventFlag(g_obj));
}

static void cell_evf_second_waiter(void) {
    const char *c = "evf_second_waiter";
    g_obj = sceKernelCreateEventFlag("b2e2", PSP_EVENT_WAITSINGLE, 0, NULL);
    rc_line(c, "CreateEventFlag.single", g_obj);
    if (g_obj < 0) return;
    SceUID h = start_helper(c, H_EVF_WAIT);
    refer(c, "ReferEventFlagStatus.helper_waiting", refer_evf, g_obj, EVF_INFO_SIZE);
    if (h >= 0) refer(c, "ReferThreadStatus.helper_waiting", refer_thread, h, THREAD_INFO_SIZE);
    SceUInt tmo = STEP_US / 2;
    u32 out = 0xdeadbeef;
    int rc = sceKernelWaitEventFlag(g_obj, 0x02, PSP_EVENT_WAITOR, &out, &tmo);
    rc_line(c, "main.WaitEventFlag.second_waiter", rc);
    val_line(c, "main.WaitEventFlag.second_waiter.out", out);
    val_line(c, "main.WaitEventFlag.second_waiter.timeout_left", tmo);
    int n = -7;
    rc = sceKernelCancelEventFlag(g_obj, 0x10, &n);
    rc_line(c, "main.CancelEventFlag.10", rc);
    val_line(c, "main.CancelEventFlag.numWaitThreads", (uint32_t)n);
    join_helper(c, h);
    refer(c, "ReferEventFlagStatus.end", refer_evf, g_obj, EVF_INFO_SIZE);
    rc_line(c, "DeleteEventFlag", sceKernelDeleteEventFlag(g_obj));
}

static void cell_lw_contend(void) {
    const char *c = "lw_contend";
    memset(&g_lw, 0, sizeof(g_lw));
    int rc = sceKernelCreateLwMutex(&g_lw, "b2lw", 0, 0, NULL);
    lw_line(c, "CreateLwMutex", rc);
    if (rc < 0) return;
    lw_line(c, "main.TryLockLwMutex", sceKernelTryLockLwMutex(&g_lw, 1));
    SceUID h = start_helper(c, H_LW_CONTEND);
    sceKernelDelayThread(STEP_US * 2);
    lw_line(c, "main.workarea_helper_waiting", 0);
    if (h >= 0) refer(c, "ReferThreadStatus.helper_waiting", refer_thread, h, THREAD_INFO_SIZE);
    lw_line(c, "main.UnlockLwMutex", sceKernelUnlockLwMutex(&g_lw, 1));
    join_helper(c, h);
    lw_line(c, "DeleteLwMutex", sceKernelDeleteLwMutex(&g_lw));
}

static void cell_fpl_wake(void) {
    const char *c = "fpl_wake";
    g_fpl_cell = c;
    g_obj = sceKernelCreateFpl("b2f", USER_PARTITION, 0, 64, 1, NULL);
    rc_line(c, "CreateFpl.64x1", g_obj);
    if (g_obj < 0) return;
    g_block = NULL;
    rc_line(c, "main.TryAllocateFpl", sceKernelTryAllocateFpl(g_obj, &g_block));
    SceUID h = start_helper(c, H_FPL_WAIT);
    refer(c, "ReferFplStatus.helper_waiting", refer_fpl, g_obj, FPL_INFO_SIZE);
    if (h >= 0) refer(c, "ReferThreadStatus.helper_waiting", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "main.FreeFpl", sceKernelFreeFpl(g_obj, g_block));
    join_helper(c, h);
    refer(c, "ReferFplStatus.end", refer_fpl, g_obj, FPL_INFO_SIZE);
    rc_line(c, "DeleteFpl", sceKernelDeleteFpl(g_obj));
}

static void cell_fpl_delete_waiting(void) {
    const char *c = "fpl_delete_waiting";
    g_fpl_cell = c;
    g_obj = sceKernelCreateFpl("b2fd", USER_PARTITION, 0, 64, 1, NULL);
    rc_line(c, "CreateFpl.64x1", g_obj);
    if (g_obj < 0) return;
    g_block = NULL;
    rc_line(c, "main.TryAllocateFpl", sceKernelTryAllocateFpl(g_obj, &g_block));
    SceUID h = start_helper(c, H_FPL_WAIT);
    rc_line(c, "main.DeleteFpl.with_waiter", sceKernelDeleteFpl(g_obj));
    join_helper(c, h);
}

static void cell_vpl_wake(void) {
    const char *c = "vpl_wake";
    g_obj = sceKernelCreateVpl("b2v", USER_PARTITION, 0, 256, NULL);
    rc_line(c, "CreateVpl.256", g_obj);
    if (g_obj < 0) return;
    void *a = NULL;
    rc_line(c, "main.TryAllocateVpl.200", sceKernelTryAllocateVpl(g_obj, 200, &a));
    val_line(c, "main.block_200", (uint32_t)(uintptr_t)a);
    SceUID h = start_helper(c, H_VPL_WAIT);
    refer(c, "ReferVplStatus.helper_waiting", refer_vpl, g_obj, VPL_INFO_SIZE);
    rc_line(c, "main.FreeVpl", sceKernelFreeVpl(g_obj, a));
    join_helper(c, h);
    refer(c, "ReferVplStatus.end", refer_vpl, g_obj, VPL_INFO_SIZE);
    rc_line(c, "DeleteVpl", sceKernelDeleteVpl(g_obj));
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    g_log = sceIoOpen(B2_LOG, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    if (g_log < 0) return 1;
    int prio = sceKernelGetThreadCurrentPriority();
    g_helper_prio = prio > 1 ? prio - 1 : prio;
    char header[200];
    snprintf(header, sizeof(header),
             "probe_id=PSP-B2 run_id=PSP-B2-01 build_commit=%s main_thread=0x%08x main_prio=0x%x helper_prio=0x%x\n",
             B2_BUILD_COMMIT, (unsigned int)sceKernelGetThreadId(), (unsigned int)prio,
             (unsigned int)g_helper_prio);
    emit(header);
    cell_sema_wake();
    cell_sema_delete();
    cell_sema_cancel();
    cell_sema_helper_waits();
    cell_evf_wake();
    cell_evf_second_waiter();
    cell_lw_contend();
    cell_fpl_wake();
    cell_fpl_delete_waiting();
    cell_vpl_wake();
    emit("probe_end=PSP-B2\n");
    sceIoClose(g_log);
    return 0;
}
