// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// PSP-B1: bounded kernel-object semantics probe (one launch per boot).
//
// Every cell creates and deletes its own objects, uses only non-blocking or
// short timed calls (at most 20 ms), and writes one line per call to
// host0:/psp_b1_results.txt before running the next call, so a fault still
// leaves every earlier result on the host. Status structures are recorded as
// raw little-endian words with the size field set from the PSPSDK layout; the
// probe does not interpret them. Heavyweight mutexes are left to the Phase-B
// probe, which already measured them.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_B1_KOBJ_PROBE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef B1_BUILD_COMMIT
#error B1_BUILD_COMMIT is required
#endif

#define B1_LOG "host0:/psp_b1_results.txt"
#define USER_PARTITION 2

static SceUID g_log = -1;

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

/* Record `words` raw words of a status buffer after the call that filled it. */
static void words_line(const char *cell, const char *call, int rc, const uint32_t *buf, int words) {
    char line[640];
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

static int refer(const char *cell, const char *call, int (*fn)(SceUID, void *), SceUID uid,
                 uint32_t size) {
    uint32_t buf[32];
    memset(buf, 0, sizeof(buf));
    buf[0] = size;
    int rc = fn(uid, buf);
    words_line(cell, call, rc, buf, (int)(size + 3) / 4);
    return rc;
}

/* Status-call adapters with a common signature for refer(). */
static int refer_sema(SceUID uid, void *b) { return sceKernelReferSemaStatus(uid, b); }
static int refer_evf(SceUID uid, void *b) { return sceKernelReferEventFlagStatus(uid, b); }
static int refer_fpl(SceUID uid, void *b) { return sceKernelReferFplStatus(uid, b); }
static int refer_vpl(SceUID uid, void *b) { return sceKernelReferVplStatus(uid, b); }
static int refer_vt(SceUID uid, void *b) { return sceKernelReferVTimerStatus(uid, b); }

/* Record an invalid-create result and delete the object if it was created anyway. */
static void bad(const char *cell, const char *call, int rc, int (*del)(SceUID)) {
    rc_line(cell, call, rc);
    if (rc > 0) rc_line(cell, "cleanup_delete", del(rc));
}

static int delete_evf(SceUID uid) { return sceKernelDeleteEventFlag(uid); }

/* SDK layout sizes: size + name[32] + N int fields (VTimer adds three 64-bit clocks). */
#define SEMA_INFO_SIZE 56u
#define EVF_INFO_SIZE 52u
#define FPL_INFO_SIZE 56u
#define VPL_INFO_SIZE 52u
#define VTIMER_INFO_SIZE 72u

static int g_cb_calls;
static int g_cb_last_count;
static int g_cb_last_arg2;
static uintptr_t g_cb_last_arg;

static int b1_callback(int count, int arg2, void *arg) {
    g_cb_calls++;
    g_cb_last_count = count;
    g_cb_last_arg2 = arg2;
    g_cb_last_arg = (uintptr_t)arg;
    return 0;
}

static void cell_callback(void) {
    const char *c = "cb";
    SceUID cb = sceKernelCreateCallback("b1cb", b1_callback, (void *)0x1234);
    rc_line(c, "CreateCallback", cb);
    if (cb < 0) return;
    rc_line(c, "GetCallbackCount.initial", sceKernelGetCallbackCount(cb));
    rc_line(c, "NotifyCallback.1", sceKernelNotifyCallback(cb, 0x55));
    rc_line(c, "GetCallbackCount.after1", sceKernelGetCallbackCount(cb));
    rc_line(c, "NotifyCallback.2", sceKernelNotifyCallback(cb, 0x66));
    rc_line(c, "GetCallbackCount.after2", sceKernelGetCallbackCount(cb));
    rc_line(c, "CheckCallback", sceKernelCheckCallback());
    val_line(c, "handler_calls", (uint32_t)g_cb_calls);
    val_line(c, "handler_last_count", (uint32_t)g_cb_last_count);
    val_line(c, "handler_last_arg2", (uint32_t)g_cb_last_arg2);
    val_line(c, "handler_last_arg", (uint32_t)g_cb_last_arg);
    rc_line(c, "GetCallbackCount.afterCheck", sceKernelGetCallbackCount(cb));
    rc_line(c, "CheckCallback.again", sceKernelCheckCallback());
    val_line(c, "handler_calls.again", (uint32_t)g_cb_calls);
    rc_line(c, "DeleteCallback", sceKernelDeleteCallback(cb));
    rc_line(c, "GetCallbackCount.deleted", sceKernelGetCallbackCount(cb));
    rc_line(c, "DeleteCallback.again", sceKernelDeleteCallback(cb));
}

static void cell_sema(void) {
    const char *c = "sema";
    bad(c, "CreateSema.init3_max2", sceKernelCreateSema("b1bad", 0, 3, 2, NULL), sceKernelDeleteSema);
    bad(c, "CreateSema.max0", sceKernelCreateSema("b1bad", 0, 0, 0, NULL), sceKernelDeleteSema);
    bad(c, "CreateSema.attr_ffffffff", sceKernelCreateSema("b1bad", 0xffffffffu, 0, 1, NULL), sceKernelDeleteSema);
    SceUID s = sceKernelCreateSema("b1sema", 0, 1, 2, NULL);
    rc_line(c, "CreateSema.init1_max2", s);
    if (s < 0) return;
    refer(c, "ReferSemaStatus.created", refer_sema, s, SEMA_INFO_SIZE);
    rc_line(c, "PollSema.1", sceKernelPollSema(s, 1));
    rc_line(c, "PollSema.1.empty", sceKernelPollSema(s, 1));
    rc_line(c, "PollSema.0", sceKernelPollSema(s, 0));
    rc_line(c, "PollSema.neg1", sceKernelPollSema(s, -1));
    rc_line(c, "PollSema.3.overmax", sceKernelPollSema(s, 3));
    SceUInt tmo = 10000;
    rc_line(c, "WaitSema.1.empty_10ms", sceKernelWaitSema(s, 1, &tmo));
    val_line(c, "WaitSema.timeout_left", tmo);
    rc_line(c, "SignalSema.1", sceKernelSignalSema(s, 1));
    rc_line(c, "SignalSema.1.second", sceKernelSignalSema(s, 1));
    rc_line(c, "SignalSema.1.overmax", sceKernelSignalSema(s, 1));
    rc_line(c, "SignalSema.0", sceKernelSignalSema(s, 0));
    rc_line(c, "SignalSema.neg1", sceKernelSignalSema(s, -1));
    refer(c, "ReferSemaStatus.full", refer_sema, s, SEMA_INFO_SIZE);
    tmo = 10000;
    rc_line(c, "WaitSema.2.available", sceKernelWaitSema(s, 2, &tmo));
    val_line(c, "WaitSema.available.timeout_left", tmo);
    uint32_t small[4] = {8, 0, 0, 0};
    int rc = sceKernelReferSemaStatus(s, (SceKernelSemaInfo *)small);
    words_line(c, "ReferSemaStatus.size8", rc, small, 4);
    rc_line(c, "DeleteSema", sceKernelDeleteSema(s));
    rc_line(c, "PollSema.deleted", sceKernelPollSema(s, 1));
    rc_line(c, "SignalSema.deleted", sceKernelSignalSema(s, 1));
    rc_line(c, "DeleteSema.again", sceKernelDeleteSema(s));
    refer(c, "ReferSemaStatus.deleted", refer_sema, s, SEMA_INFO_SIZE);
}

static void cell_evf(void) {
    const char *c = "evf";
    bad(c, "CreateEventFlag.attr_ffffffff",
        sceKernelCreateEventFlag("b1bad", (int)0xffffffff, 0, NULL), delete_evf);
    SceUID e = sceKernelCreateEventFlag("b1evf", PSP_EVENT_WAITSINGLE, 0x0f, NULL);
    rc_line(c, "CreateEventFlag.single_init0f", e);
    if (e < 0) return;
    refer(c, "ReferEventFlagStatus.created", refer_evf, e, EVF_INFO_SIZE);
    u32 out = 0xdeadbeef;
    rc_line(c, "PollEventFlag.and_01", sceKernelPollEventFlag(e, 0x01, PSP_EVENT_WAITAND, &out));
    val_line(c, "PollEventFlag.and_01.out", out);
    out = 0xdeadbeef;
    rc_line(c, "PollEventFlag.and_11.unmet", sceKernelPollEventFlag(e, 0x11, PSP_EVENT_WAITAND, &out));
    val_line(c, "PollEventFlag.and_11.out", out);
    out = 0xdeadbeef;
    rc_line(c, "PollEventFlag.or_30.unmet", sceKernelPollEventFlag(e, 0x30, PSP_EVENT_WAITOR, &out));
    val_line(c, "PollEventFlag.or_30.out", out);
    out = 0xdeadbeef;
    rc_line(c, "PollEventFlag.bits0", sceKernelPollEventFlag(e, 0, PSP_EVENT_WAITAND, &out));
    val_line(c, "PollEventFlag.bits0.out", out);
    out = 0xdeadbeef;
    rc_line(c, "PollEventFlag.mode2", sceKernelPollEventFlag(e, 0x01, 2, &out));
    rc_line(c, "PollEventFlag.mode_ff", sceKernelPollEventFlag(e, 0x01, 0xff, &out));
    out = 0xdeadbeef;
    rc_line(c, "PollEventFlag.or_03_clearpat", sceKernelPollEventFlag(e, 0x03, PSP_EVENT_WAITOR | 0x10, &out));
    val_line(c, "PollEventFlag.or_03_clearpat.out", out);
    refer(c, "ReferEventFlagStatus.after_clearpat", refer_evf, e, EVF_INFO_SIZE);
    out = 0xdeadbeef;
    rc_line(c, "PollEventFlag.or_04_clear", sceKernelPollEventFlag(e, 0x04, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &out));
    val_line(c, "PollEventFlag.or_04_clear.out", out);
    refer(c, "ReferEventFlagStatus.after_clear", refer_evf, e, EVF_INFO_SIZE);
    rc_line(c, "PollEventFlag.null_out", sceKernelPollEventFlag(e, 0x01, PSP_EVENT_WAITOR, NULL));
    rc_line(c, "SetEventFlag.f0", sceKernelSetEventFlag(e, 0xf0));
    rc_line(c, "ClearEventFlag.3c", sceKernelClearEventFlag(e, 0x3c));
    refer(c, "ReferEventFlagStatus.after_set_f0_clear_3c", refer_evf, e, EVF_INFO_SIZE);
    rc_line(c, "SetEventFlag.0", sceKernelSetEventFlag(e, 0));
    rc_line(c, "DeleteEventFlag", sceKernelDeleteEventFlag(e));
    rc_line(c, "PollEventFlag.deleted", sceKernelPollEventFlag(e, 0x01, PSP_EVENT_WAITOR, &out));
    rc_line(c, "SetEventFlag.deleted", sceKernelSetEventFlag(e, 1));
    rc_line(c, "DeleteEventFlag.again", sceKernelDeleteEventFlag(e));
}

static void cell_fpl(void) {
    const char *c = "fpl";
    bad(c, "CreateFpl.blocksize0", sceKernelCreateFpl("b1bad", USER_PARTITION, 0, 0, 2, NULL), sceKernelDeleteFpl);
    bad(c, "CreateFpl.blocks0", sceKernelCreateFpl("b1bad", USER_PARTITION, 0, 64, 0, NULL), sceKernelDeleteFpl);
    bad(c, "CreateFpl.partition0", sceKernelCreateFpl("b1bad", 0, 0, 64, 2, NULL), sceKernelDeleteFpl);
    SceUID f = sceKernelCreateFpl("b1fpl", USER_PARTITION, 0, 64, 2, NULL);
    rc_line(c, "CreateFpl.user_64x2", f);
    if (f < 0) return;
    refer(c, "ReferFplStatus.created", refer_fpl, f, FPL_INFO_SIZE);
    void *a = NULL, *b = NULL, *x = NULL;
    rc_line(c, "TryAllocateFpl.a", sceKernelTryAllocateFpl(f, &a));
    val_line(c, "block_a", (uint32_t)(uintptr_t)a);
    rc_line(c, "TryAllocateFpl.b", sceKernelTryAllocateFpl(f, &b));
    val_line(c, "block_b", (uint32_t)(uintptr_t)b);
    x = (void *)0x1;
    rc_line(c, "TryAllocateFpl.empty", sceKernelTryAllocateFpl(f, &x));
    val_line(c, "TryAllocateFpl.empty.out", (uint32_t)(uintptr_t)x);
    unsigned int tmo = 10000;
    x = (void *)0x1;
    rc_line(c, "AllocateFpl.empty_10ms", sceKernelAllocateFpl(f, &x, &tmo));
    val_line(c, "AllocateFpl.timeout_left", tmo);
    refer(c, "ReferFplStatus.full", refer_fpl, f, FPL_INFO_SIZE);
    rc_line(c, "FreeFpl.a", sceKernelFreeFpl(f, a));
    rc_line(c, "FreeFpl.a.again", sceKernelFreeFpl(f, a));
    rc_line(c, "FreeFpl.b_plus_1", sceKernelFreeFpl(f, (char *)b + 1));
    rc_line(c, "FreeFpl.null", sceKernelFreeFpl(f, NULL));
    refer(c, "ReferFplStatus.one_free", refer_fpl, f, FPL_INFO_SIZE);
    rc_line(c, "DeleteFpl.with_block_held", sceKernelDeleteFpl(f));
    rc_line(c, "TryAllocateFpl.deleted", sceKernelTryAllocateFpl(f, &x));
    rc_line(c, "FreeFpl.deleted", sceKernelFreeFpl(f, b));
    rc_line(c, "DeleteFpl.again", sceKernelDeleteFpl(f));
}

static void cell_vpl(void) {
    const char *c = "vpl";
    bad(c, "CreateVpl.size0", sceKernelCreateVpl("b1bad", USER_PARTITION, 0, 0, NULL), sceKernelDeleteVpl);
    bad(c, "CreateVpl.partition0", sceKernelCreateVpl("b1bad", 0, 0, 1024, NULL), sceKernelDeleteVpl);
    SceUID v = sceKernelCreateVpl("b1vpl", USER_PARTITION, 0, 1024, NULL);
    rc_line(c, "CreateVpl.user_1024", v);
    if (v < 0) return;
    refer(c, "ReferVplStatus.created", refer_vpl, v, VPL_INFO_SIZE);
    void *p = NULL, *x = (void *)0x1;
    rc_line(c, "TryAllocateVpl.100", sceKernelTryAllocateVpl(v, 100, &p));
    val_line(c, "block_100", (uint32_t)(uintptr_t)p);
    refer(c, "ReferVplStatus.after100", refer_vpl, v, VPL_INFO_SIZE);
    rc_line(c, "TryAllocateVpl.0", sceKernelTryAllocateVpl(v, 0, &x));
    val_line(c, "TryAllocateVpl.0.out", (uint32_t)(uintptr_t)x);
    x = (void *)0x1;
    rc_line(c, "TryAllocateVpl.2000", sceKernelTryAllocateVpl(v, 2000, &x));
    val_line(c, "TryAllocateVpl.2000.out", (uint32_t)(uintptr_t)x);
    void *blocks[32];
    int got = 0;
    for (; got < 32; got++) {
        blocks[got] = NULL;
        int rc = sceKernelTryAllocateVpl(v, 64, &blocks[got]);
        if (rc < 0) {
            rc_line(c, "TryAllocateVpl.64.fill_stop", rc);
            break;
        }
    }
    val_line(c, "fill_64_count", (uint32_t)got);
    if (got > 0) val_line(c, "fill_first", (uint32_t)(uintptr_t)blocks[0]);
    if (got > 1) val_line(c, "fill_second", (uint32_t)(uintptr_t)blocks[1]);
    refer(c, "ReferVplStatus.filled", refer_vpl, v, VPL_INFO_SIZE);
    unsigned int tmo = 10000;
    x = (void *)0x1;
    rc_line(c, "AllocateVpl.64.full_10ms", sceKernelAllocateVpl(v, 64, &x, &tmo));
    val_line(c, "AllocateVpl.timeout_left", tmo);
    for (int i = 0; i < got; i++) {
        int rc = sceKernelFreeVpl(v, blocks[i]);
        if (rc != 0) rc_line(c, "FreeVpl.fill_block_nonzero", rc);
    }
    rc_line(c, "FreeVpl.fill_block0.again", got > 0 ? sceKernelFreeVpl(v, blocks[0]) : -1);
    rc_line(c, "FreeVpl.interior", sceKernelFreeVpl(v, (char *)p + 4));
    rc_line(c, "FreeVpl.null", sceKernelFreeVpl(v, NULL));
    refer(c, "ReferVplStatus.one_held", refer_vpl, v, VPL_INFO_SIZE);
    rc_line(c, "DeleteVpl.with_block_held", sceKernelDeleteVpl(v));
    rc_line(c, "TryAllocateVpl.deleted", sceKernelTryAllocateVpl(v, 16, &x));
    rc_line(c, "FreeVpl.deleted", sceKernelFreeVpl(v, p));
    rc_line(c, "DeleteVpl.again", sceKernelDeleteVpl(v));
}

static void cell_vtimer(void) {
    const char *c = "vtimer";
    SceUID t = sceKernelCreateVTimer("b1vt", NULL);
    rc_line(c, "CreateVTimer", t);
    if (t < 0) return;
    refer(c, "ReferVTimerStatus.created", refer_vt, t, VTIMER_INFO_SIZE);
    rc_line(c, "StopVTimer.not_started", sceKernelStopVTimer(t));
    rc_line(c, "StartVTimer", sceKernelStartVTimer(t));
    rc_line(c, "StartVTimer.again", sceKernelStartVTimer(t));
    sceKernelDelayThread(20000);
    refer(c, "ReferVTimerStatus.running_20ms", refer_vt, t, VTIMER_INFO_SIZE);
    rc_line(c, "StopVTimer", sceKernelStopVTimer(t));
    rc_line(c, "StopVTimer.again", sceKernelStopVTimer(t));
    refer(c, "ReferVTimerStatus.stopped", refer_vt, t, VTIMER_INFO_SIZE);
    sceKernelDelayThread(20000);
    refer(c, "ReferVTimerStatus.stopped_20ms_later", refer_vt, t, VTIMER_INFO_SIZE);
    rc_line(c, "StartVTimer.restart", sceKernelStartVTimer(t));
    rc_line(c, "DeleteVTimer.running", sceKernelDeleteVTimer(t));
    rc_line(c, "StartVTimer.deleted", sceKernelStartVTimer(t));
    refer(c, "ReferVTimerStatus.deleted", refer_vt, t, VTIMER_INFO_SIZE);
    rc_line(c, "DeleteVTimer.again", sceKernelDeleteVTimer(t));
}

static void wa_line(const char *cell, const char *call, int rc, const SceLwMutexWorkarea *wa) {
    uint32_t w[8];
    memcpy(w, wa, sizeof(w));
    words_line(cell, call, rc, w, 8);
}

static void cell_lwmutex(void) {
    const char *c = "lwmutex";
    SceLwMutexWorkarea wa, wb;
    memset(&wa, 0xa5, sizeof(wa));
    memset(&wb, 0xa5, sizeof(wb));
    int rc = sceKernelCreateLwMutex(&wa, "b1lwr", PSP_LW_MUTEX_ATTR_RECURSIVE, 0, NULL);
    wa_line(c, "CreateLwMutex.recursive_init0", rc, &wa);
    if (rc < 0) return;
    rc = sceKernelTryLockLwMutex(&wa, 1);
    wa_line(c, "TryLockLwMutex.1", rc, &wa);
    rc = sceKernelTryLockLwMutex(&wa, 2);
    wa_line(c, "TryLockLwMutex.recursive_2", rc, &wa);
    rc = sceKernelTryLockLwMutex(&wa, 0);
    wa_line(c, "TryLockLwMutex.0", rc, &wa);
    rc = sceKernelUnlockLwMutex(&wa, 2);
    wa_line(c, "UnlockLwMutex.2", rc, &wa);
    rc = sceKernelUnlockLwMutex(&wa, 1);
    wa_line(c, "UnlockLwMutex.1", rc, &wa);
    rc = sceKernelUnlockLwMutex(&wa, 1);
    wa_line(c, "UnlockLwMutex.underflow", rc, &wa);
    rc = sceKernelUnlockLwMutex(&wa, 0);
    wa_line(c, "UnlockLwMutex.0", rc, &wa);

    rc = sceKernelCreateLwMutex(&wb, "b1lwn", 0, 1, NULL);
    wa_line(c, "CreateLwMutex.nonrecursive_init1", rc, &wb);
    if (rc >= 0) {
        rc = sceKernelTryLockLwMutex(&wb, 1);
        wa_line(c, "TryLockLwMutex.self_held_nonrecursive", rc, &wb);
        unsigned int tmo = 10000;
        rc = sceKernelLockLwMutex(&wb, 1, &tmo);
        wa_line(c, "LockLwMutex.self_held_nonrecursive_10ms", rc, &wb);
        val_line(c, "LockLwMutex.timeout_left", tmo);
        rc = sceKernelDeleteLwMutex(&wb);
        wa_line(c, "DeleteLwMutex.while_held", rc, &wb);
        rc = sceKernelTryLockLwMutex(&wb, 1);
        wa_line(c, "TryLockLwMutex.deleted", rc, &wb);
        rc = sceKernelDeleteLwMutex(&wb);
        wa_line(c, "DeleteLwMutex.again", rc, &wb);
    }
    rc = sceKernelCreateLwMutex(&wb, "b1lwbad", 0, 2, NULL);
    wa_line(c, "CreateLwMutex.nonrecursive_init2", rc, &wb);
    if (rc >= 0) sceKernelDeleteLwMutex(&wb);
    rc = sceKernelDeleteLwMutex(&wa);
    wa_line(c, "DeleteLwMutex.recursive", rc, &wa);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    g_log = sceIoOpen(B1_LOG, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    if (g_log < 0) return 1;
    char header[160];
    snprintf(header, sizeof(header), "probe_id=PSP-B1 run_id=PSP-B1-01 build_commit=%s thread=0x%08x\n",
             B1_BUILD_COMMIT, (unsigned int)sceKernelGetThreadId());
    emit(header);
    cell_callback();
    cell_sema();
    cell_evf();
    cell_fpl();
    cell_vpl();
    cell_vtimer();
    cell_lwmutex();
    emit("probe_end=PSP-B1\n");
    sceIoClose(g_log);
    return 0;
}
