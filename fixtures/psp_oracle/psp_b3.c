// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// PSP-B3: message pipes, mailboxes, thread sleep/wakeup/suspend/terminate,
// release-wait, priority changes, alarms and delay timing, in one bounded
// launch (the boot's only launch). No call raises an exception. Every wait has
// a timeout of at most 500 ms, every helper thread is woken by the main thread
// and joined with a 1 s timeout (terminated if the join fails), and the alarm
// handler only updates globals. When the cells finish, the main thread parks
// in sceKernelSleepThread() instead of returning, so the module does not exit
// and PSPLink stays in control of the boot.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_B3_KERNEL_PROBE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef B3_BUILD_COMMIT
#error B3_BUILD_COMMIT is required
#endif

#define B3_LOG "host0:/psp_b3_results.txt"
#define USER_PARTITION 2
#define WAIT_US 500000u
#define STEP_US 20000

#define MPP_INFO_SIZE 56u
#define MBX_INFO_SIZE 52u
#define THREAD_INFO_SIZE 104u

static SceUID g_log = -1;
static SceUID g_obj = -1;
static int g_helper_prio;

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

static int refer_mpp(SceUID uid, void *b) { return sceKernelReferMsgPipeStatus(uid, b); }
static int refer_mbx(SceUID uid, void *b) { return sceKernelReferMbxStatus(uid, b); }
static int refer_thread(SceUID uid, void *b) { return sceKernelReferThreadStatus(uid, b); }

static void refer(const char *cell, const char *call, int (*fn)(SceUID, void *), SceUID uid,
                  uint32_t size) {
    uint32_t buf[32];
    memset(buf, 0, sizeof(buf));
    buf[0] = size;
    int rc = fn(uid, buf);
    words_line(cell, call, rc, buf, (int)(size + 3) / 4);
}

typedef struct {
    SceKernelMsgPacket header;
    uint32_t marker;
} B3Packet;

static B3Packet g_pkt[4];

enum {
    H_MPP_RECEIVE = 1,
    H_MBX_RECEIVE,
    H_SLEEP_TWICE,
    H_WAKEUP_COUNT,
    H_SLEEP_ONCE,
    H_EXIT_QUICK,
    H_SEMA_WAIT,
};

static int helper(SceSize args, void *argp) {
    int mode = (args >= sizeof(int) && argp) ? *(int *)argp : 0;
    unsigned int tmo = WAIT_US;
    int rc;
    switch (mode) {
    case H_MPP_RECEIVE: {
        char buf[16];
        int result = -7;
        memset(buf, 0, sizeof(buf));
        rc = sceKernelReceiveMsgPipe(g_obj, buf, 8, 0, &result, &tmo);
        rc_line("mpp_wake", "helper.ReceiveMsgPipe.8_full", rc);
        val_line("mpp_wake", "helper.ReceiveMsgPipe.result", (uint32_t)result);
        val_line("mpp_wake", "helper.ReceiveMsgPipe.first_word", (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                 | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24));
        val_line("mpp_wake", "helper.ReceiveMsgPipe.timeout_left", tmo);
        break;
    }
    case H_MBX_RECEIVE: {
        void *msg = (void *)0x1;
        SceUInt t = WAIT_US;
        rc = sceKernelReceiveMbx(g_obj, &msg, &t);
        rc_line("mbx_helper", "helper.ReceiveMbx", rc);
        val_line("mbx_helper", "helper.ReceiveMbx.msg", (uint32_t)(uintptr_t)msg);
        val_line("mbx_helper", "helper.ReceiveMbx.timeout_left", t);
        break;
    }
    case H_SLEEP_TWICE:
        rc_line("sleep", "helper.SleepThread.1", sceKernelSleepThread());
        rc_line("sleep", "helper.SleepThread.2", sceKernelSleepThread());
        return 0x1234;
    case H_WAKEUP_COUNT:
        sceKernelDelayThread(STEP_US);
        rc_line("wakeup_count", "helper.CancelWakeupThread.self", sceKernelCancelWakeupThread(0));
        rc_line("wakeup_count", "helper.CancelWakeupThread.self.again", sceKernelCancelWakeupThread(0));
        rc_line("wakeup_count", "helper.SleepThread", sceKernelSleepThread());
        break;
    case H_SLEEP_ONCE:
        rc_line("terminate", "helper.SleepThread.unexpected_return", sceKernelSleepThread());
        break;
    case H_EXIT_QUICK:
        return 0x5678;
    case H_SEMA_WAIT: {
        SceUInt t = WAIT_US;
        rc = sceKernelWaitSema(g_obj, 1, &t);
        rc_line("release_wait", "helper.WaitSema", rc);
        val_line("release_wait", "helper.WaitSema.timeout_left", t);
        break;
    }
    default:
        break;
    }
    return 0;
}

static SceUID create_helper(const char *cell) {
    SceUID uid = sceKernelCreateThread("b3helper", helper, g_helper_prio, 0x2000, THREAD_ATTR_USER, NULL);
    rc_line(cell, "CreateThread.helper", uid);
    return uid;
}

static SceUID start_helper(const char *cell, int mode) {
    SceUID uid = create_helper(cell);
    if (uid < 0) return uid;
    rc_line(cell, "StartThread.helper", sceKernelStartThread(uid, sizeof(mode), &mode));
    return uid;
}

static void join_helper(const char *cell, SceUID uid) {
    if (uid < 0) return;
    SceUInt tmo = 1000000;
    int rc = sceKernelWaitThreadEnd(uid, &tmo);
    rc_line(cell, "WaitThreadEnd.helper", rc);
    if (rc < 0) {
        rc_line(cell, "WakeupThread.cleanup", sceKernelWakeupThread(uid));
        rc_line(cell, "TerminateDeleteThread.helper", sceKernelTerminateDeleteThread(uid));
    } else {
        rc_line(cell, "GetThreadExitStatus.after_end", sceKernelGetThreadExitStatus(uid));
        rc_line(cell, "DeleteThread.helper", sceKernelDeleteThread(uid));
    }
}

static void cell_mpp(void) {
    const char *c = "mpp";
    char out[128];
    char msg[128];
    int result;
    for (int i = 0; i < (int)sizeof(msg); i++) msg[i] = (char)(0x40 + (i & 0x3f));
    SceUID p = sceKernelCreateMsgPipe("b3mpp", USER_PARTITION, 0, (void *)64, NULL);
    rc_line(c, "CreateMsgPipe.64", p);
    if (p < 0) return;
    refer(c, "ReferMsgPipeStatus.created", refer_mpp, p, MPP_INFO_SIZE);
    result = -7;
    rc_line(c, "TrySendMsgPipe.10_full", sceKernelTrySendMsgPipe(p, msg, 10, 0, &result));
    val_line(c, "TrySendMsgPipe.10_full.result", (uint32_t)result);
    refer(c, "ReferMsgPipeStatus.after10", refer_mpp, p, MPP_INFO_SIZE);
    result = -7;
    rc_line(c, "TrySendMsgPipe.60_full", sceKernelTrySendMsgPipe(p, msg + 10, 60, 0, &result));
    val_line(c, "TrySendMsgPipe.60_full.result", (uint32_t)result);
    result = -7;
    rc_line(c, "TrySendMsgPipe.60_asap", sceKernelTrySendMsgPipe(p, msg + 10, 60, 1, &result));
    val_line(c, "TrySendMsgPipe.60_asap.result", (uint32_t)result);
    result = -7;
    rc_line(c, "TrySendMsgPipe.100_oversize", sceKernelTrySendMsgPipe(p, msg, 100, 0, &result));
    val_line(c, "TrySendMsgPipe.100_oversize.result", (uint32_t)result);
    refer(c, "ReferMsgPipeStatus.full", refer_mpp, p, MPP_INFO_SIZE);
    result = -7;
    memset(out, 0, sizeof(out));
    rc_line(c, "TryReceiveMsgPipe.4_full", sceKernelTryReceiveMsgPipe(p, out, 4, 0, &result));
    val_line(c, "TryReceiveMsgPipe.4_full.result", (uint32_t)result);
    val_line(c, "TryReceiveMsgPipe.4_full.first_byte", (uint8_t)out[0]);
    result = -7;
    rc_line(c, "TryReceiveMsgPipe.100_full", sceKernelTryReceiveMsgPipe(p, out, 100, 0, &result));
    val_line(c, "TryReceiveMsgPipe.100_full.result", (uint32_t)result);
    result = -7;
    rc_line(c, "TryReceiveMsgPipe.100_asap", sceKernelTryReceiveMsgPipe(p, out, 100, 1, &result));
    val_line(c, "TryReceiveMsgPipe.100_asap.result", (uint32_t)result);
    val_line(c, "TryReceiveMsgPipe.100_asap.first_byte", (uint8_t)out[0]);
    result = -7;
    rc_line(c, "TryReceiveMsgPipe.empty", sceKernelTryReceiveMsgPipe(p, out, 4, 1, &result));
    val_line(c, "TryReceiveMsgPipe.empty.result", (uint32_t)result);
    unsigned int tmo = STEP_US / 2;
    result = -7;
    rc_line(c, "ReceiveMsgPipe.empty_10ms", sceKernelReceiveMsgPipe(p, out, 4, 0, &result, &tmo));
    val_line(c, "ReceiveMsgPipe.empty_10ms.timeout_left", tmo);
    rc_line(c, "TrySendMsgPipe.size0", sceKernelTrySendMsgPipe(p, msg, 0, 0, &result));
    rc_line(c, "TrySendMsgPipe.waitmode2", sceKernelTrySendMsgPipe(p, msg, 4, 2, &result));
    rc_line(c, "DeleteMsgPipe", sceKernelDeleteMsgPipe(p));
    rc_line(c, "TrySendMsgPipe.deleted", sceKernelTrySendMsgPipe(p, msg, 4, 0, &result));
    rc_line(c, "DeleteMsgPipe.again", sceKernelDeleteMsgPipe(p));

    SceUID z = sceKernelCreateMsgPipe("b3mpp0", USER_PARTITION, 0, (void *)0, NULL);
    rc_line(c, "CreateMsgPipe.unbuffered", z);
    if (z >= 0) {
        refer(c, "ReferMsgPipeStatus.unbuffered", refer_mpp, z, MPP_INFO_SIZE);
        result = -7;
        rc_line(c, "TrySendMsgPipe.unbuffered_no_receiver", sceKernelTrySendMsgPipe(z, msg, 4, 0, &result));
        val_line(c, "TrySendMsgPipe.unbuffered_no_receiver.result", (uint32_t)result);
        rc_line(c, "DeleteMsgPipe.unbuffered", sceKernelDeleteMsgPipe(z));
    }
}

static void cell_mpp_wake(void) {
    const char *c = "mpp_wake";
    char msg[8] = {'W', 'A', 'K', 'E', 1, 2, 3, 4};
    int result = -7;
    g_obj = sceKernelCreateMsgPipe("b3mppw", USER_PARTITION, 0, (void *)64, NULL);
    rc_line(c, "CreateMsgPipe.64", g_obj);
    if (g_obj < 0) return;
    SceUID h = start_helper(c, H_MPP_RECEIVE);
    refer(c, "ReferMsgPipeStatus.helper_waiting", refer_mpp, g_obj, MPP_INFO_SIZE);
    if (h >= 0) refer(c, "ReferThreadStatus.helper_waiting", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "main.TrySendMsgPipe.8", sceKernelTrySendMsgPipe(g_obj, msg, 8, 0, &result));
    val_line(c, "main.TrySendMsgPipe.8.result", (uint32_t)result);
    join_helper(c, h);
    refer(c, "ReferMsgPipeStatus.end", refer_mpp, g_obj, MPP_INFO_SIZE);
    rc_line(c, "DeleteMsgPipe", sceKernelDeleteMsgPipe(g_obj));
}

static void pkt_line(const char *cell, const char *what) {
    for (int i = 0; i < 4; i++) {
        char name[48];
        snprintf(name, sizeof(name), "%s.pkt%d.next", what, i);
        val_line(cell, name, (uint32_t)(uintptr_t)g_pkt[i].header.next);
    }
}

static void mbx_order(const char *cell, SceUInt attr, const char *label) {
    memset(g_pkt, 0, sizeof(g_pkt));
    for (int i = 0; i < 4; i++) {
        g_pkt[i].header.next = (SceKernelMsgPacket *)0x1;
        g_pkt[i].marker = 0x4d420000u | (uint32_t)i;
        val_line(cell, "packet_address", (uint32_t)(uintptr_t)&g_pkt[i]);
    }
    g_pkt[0].header.msgPriority = 5;
    g_pkt[1].header.msgPriority = 1;
    g_pkt[2].header.msgPriority = 3;
    g_pkt[3].header.msgPriority = 1;
    SceUID m = sceKernelCreateMbx(label, attr, NULL);
    rc_line(cell, "CreateMbx", m);
    if (m < 0) return;
    for (int i = 0; i < 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "SendMbx.pkt%d", i);
        rc_line(cell, name, sceKernelSendMbx(m, &g_pkt[i]));
    }
    refer(cell, "ReferMbxStatus.four_queued", refer_mbx, m, MBX_INFO_SIZE);
    pkt_line(cell, "after_send");
    for (int i = 0; i < 5; i++) {
        void *msg = (void *)0x1;
        int rc = sceKernelPollMbx(m, &msg);
        char name[32];
        snprintf(name, sizeof(name), "PollMbx.%d", i);
        rc_line(cell, name, rc);
        snprintf(name, sizeof(name), "PollMbx.%d.marker", i);
        val_line(cell, name, (rc == 0 && msg) ? ((B3Packet *)msg)->marker : (uint32_t)(uintptr_t)msg);
    }
    pkt_line(cell, "after_poll");
    SceUInt tmo = STEP_US / 2;
    void *msg = (void *)0x1;
    rc_line(cell, "ReceiveMbx.empty_10ms", sceKernelReceiveMbx(m, &msg, &tmo));
    val_line(cell, "ReceiveMbx.empty_10ms.timeout_left", tmo);
    rc_line(cell, "DeleteMbx", sceKernelDeleteMbx(m));
    rc_line(cell, "PollMbx.deleted", sceKernelPollMbx(m, &msg));
    rc_line(cell, "DeleteMbx.again", sceKernelDeleteMbx(m));
}

static void cell_mbx(void) {
    SceUID bad = sceKernelCreateMbx("b3bad", 0xffffffffu, NULL);
    rc_line("mbx_attr", "CreateMbx.attr_ffffffff", bad);
    if (bad > 0) rc_line("mbx_attr", "cleanup_delete", sceKernelDeleteMbx(bad));
    mbx_order("mbx_fifo", 0, "b3mbxf");
    mbx_order("mbx_msgpri", 0x400, "b3mbxp");
}

static void cell_mbx_helper(void) {
    const char *c = "mbx_helper";
    memset(g_pkt, 0, sizeof(g_pkt));
    g_pkt[0].marker = 0x4d42aaaau;
    g_obj = sceKernelCreateMbx("b3mbxh", 0, NULL);
    rc_line(c, "CreateMbx", g_obj);
    if (g_obj < 0) return;
    SceUID h = start_helper(c, H_MBX_RECEIVE);
    refer(c, "ReferMbxStatus.helper_waiting", refer_mbx, g_obj, MBX_INFO_SIZE);
    if (h >= 0) refer(c, "ReferThreadStatus.helper_waiting", refer_thread, h, THREAD_INFO_SIZE);
    val_line(c, "packet_address", (uint32_t)(uintptr_t)&g_pkt[0]);
    rc_line(c, "main.SendMbx", sceKernelSendMbx(g_obj, &g_pkt[0]));
    join_helper(c, h);
    refer(c, "ReferMbxStatus.after_handoff", refer_mbx, g_obj, MBX_INFO_SIZE);

    h = start_helper(c, H_MBX_RECEIVE);
    int n = -7;
    rc_line(c, "main.CancelReceiveMbx", sceKernelCancelReceiveMbx(g_obj, &n));
    val_line(c, "main.CancelReceiveMbx.num", (uint32_t)n);
    join_helper(c, h);
    rc_line(c, "DeleteMbx", sceKernelDeleteMbx(g_obj));
}

static void cell_sleep(void) {
    const char *c = "sleep";
    SceUID h = create_helper(c);
    if (h < 0) return;
    rc_line(c, "WakeupThread.dormant", sceKernelWakeupThread(h));
    rc_line(c, "GetThreadExitStatus.dormant_never_run", sceKernelGetThreadExitStatus(h));
    refer(c, "ReferThreadStatus.dormant", refer_thread, h, THREAD_INFO_SIZE);
    int mode = H_SLEEP_TWICE;
    rc_line(c, "StartThread.helper", sceKernelStartThread(h, sizeof(mode), &mode));
    refer(c, "ReferThreadStatus.sleeping", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "GetThreadExitStatus.sleeping", sceKernelGetThreadExitStatus(h));
    rc_line(c, "WakeupThread.1", sceKernelWakeupThread(h));
    rc_line(c, "main.after_wakeup1_marker", 0);
    sceKernelDelayThread(STEP_US);
    refer(c, "ReferThreadStatus.sleeping_again", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "SuspendThread", sceKernelSuspendThread(h));
    refer(c, "ReferThreadStatus.sleeping_suspended", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "SuspendThread.again", sceKernelSuspendThread(h));
    rc_line(c, "ResumeThread", sceKernelResumeThread(h));
    rc_line(c, "ResumeThread.again", sceKernelResumeThread(h));
    refer(c, "ReferThreadStatus.resumed", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "WakeupThread.2", sceKernelWakeupThread(h));
    sceKernelDelayThread(STEP_US);
    join_helper(c, h);
}

static void cell_wakeup_count(void) {
    const char *c = "wakeup_count";
    SceUID h = start_helper(c, H_WAKEUP_COUNT);
    if (h < 0) return;
    rc_line(c, "WakeupThread.while_delayed.1", sceKernelWakeupThread(h));
    rc_line(c, "WakeupThread.while_delayed.2", sceKernelWakeupThread(h));
    refer(c, "ReferThreadStatus.two_pending", refer_thread, h, THREAD_INFO_SIZE);
    sceKernelDelayThread(STEP_US * 2);
    refer(c, "ReferThreadStatus.helper_sleeping", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "CancelWakeupThread.other_zero", sceKernelCancelWakeupThread(h));
    rc_line(c, "WakeupThread.final", sceKernelWakeupThread(h));
    join_helper(c, h);
}

static void cell_terminate(void) {
    const char *c = "terminate";
    SceUID h = start_helper(c, H_SLEEP_ONCE);
    if (h < 0) return;
    refer(c, "ReferThreadStatus.sleeping", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "TerminateThread", sceKernelTerminateThread(h));
    refer(c, "ReferThreadStatus.terminated", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "GetThreadExitStatus.terminated", sceKernelGetThreadExitStatus(h));
    rc_line(c, "TerminateThread.again", sceKernelTerminateThread(h));
    int mode = H_EXIT_QUICK;
    rc_line(c, "StartThread.restart_dormant", sceKernelStartThread(h, sizeof(mode), &mode));
    join_helper(c, h);
}

static void cell_release_wait(void) {
    const char *c = "release_wait";
    g_obj = sceKernelCreateSema("b3rel", 0, 0, 1, NULL);
    rc_line(c, "CreateSema", g_obj);
    if (g_obj < 0) return;
    SceUID h = start_helper(c, H_SEMA_WAIT);
    if (h >= 0) {
        refer(c, "ReferThreadStatus.helper_waiting", refer_thread, h, THREAD_INFO_SIZE);
        rc_line(c, "ReleaseWaitThread", sceKernelReleaseWaitThread(h));
        join_helper(c, h);
        rc_line(c, "ReleaseWaitThread.deleted_thread", sceKernelReleaseWaitThread(h));
    }
    rc_line(c, "ReleaseWaitThread.self_running", sceKernelReleaseWaitThread(sceKernelGetThreadId()));
    rc_line(c, "ReleaseWaitThread.zero", sceKernelReleaseWaitThread(0));
    rc_line(c, "DeleteSema", sceKernelDeleteSema(g_obj));
}

static void cell_priority(void) {
    const char *c = "priority";
    int mode = H_EXIT_QUICK;
    SceUID h = create_helper(c);
    if (h < 0) return;
    rc_line(c, "ChangeThreadPriority.dormant_0x30", sceKernelChangeThreadPriority(h, 0x30));
    refer(c, "ReferThreadStatus.dormant_after_change", refer_thread, h, THREAD_INFO_SIZE);
    rc_line(c, "ChangeThreadPriority.0x00", sceKernelChangeThreadPriority(h, 0x00));
    rc_line(c, "ChangeThreadPriority.0x07", sceKernelChangeThreadPriority(h, 0x07));
    rc_line(c, "ChangeThreadPriority.0x08", sceKernelChangeThreadPriority(h, 0x08));
    rc_line(c, "ChangeThreadPriority.0x77", sceKernelChangeThreadPriority(h, 0x77));
    rc_line(c, "ChangeThreadPriority.0x78", sceKernelChangeThreadPriority(h, 0x78));
    rc_line(c, "ChangeThreadPriority.self_same", sceKernelChangeThreadPriority(0, 0));
    rc_line(c, "GetThreadCurrentPriority.self", sceKernelGetThreadCurrentPriority());
    rc_line(c, "ChangeThreadPriority.restore_helper", sceKernelChangeThreadPriority(h, g_helper_prio));
    rc_line(c, "StartThread.helper", sceKernelStartThread(h, sizeof(mode), &mode));
    join_helper(c, h);
    rc_line(c, "ChangeThreadPriority.deleted", sceKernelChangeThreadPriority(h, 0x20));
}

static volatile int g_alarm_calls;
static volatile uintptr_t g_alarm_common;

static SceUInt alarm_repeat(void *common) {
    g_alarm_calls++;
    g_alarm_common = (uintptr_t)common;
    return g_alarm_calls < 3 ? 5000u : 0u;
}

static void cell_alarm(void) {
    const char *c = "alarm";
    g_alarm_calls = 0;
    SceUID a = sceKernelSetAlarm(10000, alarm_repeat, (void *)0x2468);
    rc_line(c, "SetAlarm.10ms_repeat3", a);
    sceKernelDelayThread(100000);
    val_line(c, "handler_calls_after_100ms", (uint32_t)g_alarm_calls);
    val_line(c, "handler_common", (uint32_t)g_alarm_common);
    if (a >= 0) rc_line(c, "CancelAlarm.after_finished", sceKernelCancelAlarm(a));

    g_alarm_calls = 0;
    SceUID b = sceKernelSetAlarm(1000000, alarm_repeat, (void *)0x1357);
    rc_line(c, "SetAlarm.1s", b);
    if (b >= 0) {
        rc_line(c, "CancelAlarm.pending", sceKernelCancelAlarm(b));
        rc_line(c, "CancelAlarm.again", sceKernelCancelAlarm(b));
    }
    sceKernelDelayThread(20000);
    val_line(c, "handler_calls_after_cancel", (uint32_t)g_alarm_calls);
    rc_line(c, "CancelAlarm.zero", sceKernelCancelAlarm(0));
}

static void cell_delay_timing(void) {
    const char *c = "delay";
    static const int delays[] = {0, 1, 100, 1000, 5000, 16667};
    for (unsigned d = 0; d < sizeof(delays) / sizeof(delays[0]); d++) {
        for (int i = 0; i < 3; i++) {
            SceUInt32 t0 = sceKernelGetSystemTimeLow();
            sceKernelDelayThread(delays[d]);
            SceUInt32 t1 = sceKernelGetSystemTimeLow();
            char name[48];
            snprintf(name, sizeof(name), "DelayThread.%d.elapsed.%d", delays[d], i);
            val_line(c, name, t1 - t0);
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    g_log = sceIoOpen(B3_LOG, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    if (g_log < 0) {
        sceKernelSleepThread();
        return 1;
    }
    int prio = sceKernelGetThreadCurrentPriority();
    g_helper_prio = prio > 1 ? prio - 1 : prio;
    char header[200];
    snprintf(header, sizeof(header),
             "probe_id=PSP-B3 run_id=PSP-B3-01 build_commit=%s main_thread=0x%08x main_prio=0x%x helper_prio=0x%x\n",
             B3_BUILD_COMMIT, (unsigned int)sceKernelGetThreadId(), (unsigned int)prio,
             (unsigned int)g_helper_prio);
    emit(header);
    cell_mpp();
    cell_mpp_wake();
    cell_mbx();
    cell_mbx_helper();
    cell_sleep();
    cell_wakeup_count();
    cell_terminate();
    cell_release_wait();
    cell_priority();
    cell_alarm();
    cell_delay_timing();
    emit("probe_end=PSP-B3 parking_main_thread\n");
    sceIoClose(g_log);
    g_log = -1;
    sceKernelSleepThread();
    return 0;
}
