// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Phase-B money binary: exit/delete repeat + heavyweight mutex matrix
// + nested-callback stack contract + GE callback stack contract.
// One launch, in-process sequential execution.
// Fixed order safest-first (zero-thread tests first, multi-thread last):
//   1. Section C: nested user callback stack contract (6 cells, stack bounds & registers)
//   2. Section D: GE callback stack contract (4 cells, hardware display list callback)
//   3. Section B: heavyweight mutex matrix (19 cells, bounded helper threads)
//   4. Section A: exit/delete repeat (12 cells, thread lifecycle stress)
// Unbuffered stdout + file log (host0:/phaseb_log.txt) ensures every completed
// cell is banked even if a subsequent experiment faults.
// Creates threads: exactly ONE thread-creating launch per boot, always LAST.
// Test ID PSP-PHASEB-001. No emulator assumption is hardware fact; raw codes
// are data; PASS = harness completed and measured outcome.

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <psppower.h>
#include <pspthreadman.h>
#include <pspsysmem.h>
#include <psputils.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("NAKAGAWA_PHASEB", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define PHASEB_TEST_ID "PSP-PHASEB-001"
#define PHASEB_BUILD_ID "nakagawa-phaseb-v1"
#define PHASEB_LOG "host0:/phaseb_log.txt"
#define NOT_MEASURED 0xffffffffu

/* Heavyweight Mutex types and prototypes from ThreadMan */
#define PSP_MUTEX_ATTR_FIFO            0x0000u
#define PSP_MUTEX_ATTR_PRIORITY        0x0100u
#define PSP_MUTEX_ATTR_ALLOW_RECURSIVE 0x0200u
#define PSP_MUTEX_LEGAL_ATTR_MASK      0x0BFFu

#define SCE_KERNEL_ERROR_WAIT_TIMEOUT               0x800201A8u
#define SCE_KERNEL_ERROR_WAIT_CANCEL                0x800201A9u
#define SCE_KERNEL_ERROR_WAIT_DELETE                0x800201B5u
#define SCE_KERNEL_ERROR_ILLEGAL_COUNT              0x800201BDu
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR               0x80020191u
#define SCE_KERNEL_ERROR_UNKNOWN_MUTEXID            0x800201C3u
#define SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN        0x800201C4u
#define SCE_KERNEL_ERROR_MUTEX_NOT_OWNED            0x800201C5u
#define SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW        0x800201C6u
#define SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW      0x800201C7u
#define SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED 0x800201C8u

typedef struct {
    SceSize size;
    char    name[32];
    SceUInt attr;
    int     initCount;
    int     currentCount;
    SceUID  lockThread;
    int     numWaitThreads;
} SceKernelMutexInfo;

/* Prototypes provided by ThreadManForUser */
SceUID sceKernelCreateMutex(const char *name, SceUInt attr, int initialCount, void *options);
int    sceKernelDeleteMutex(SceUID uid);
int    sceKernelLockMutex(SceUID uid, int count, unsigned int *pTimeout);
int    sceKernelLockMutexCB(SceUID uid, int count, unsigned int *pTimeout);
int    sceKernelTryLockMutex(SceUID uid, int count);
int    sceKernelUnlockMutex(SceUID uid, int count);
int    sceKernelCancelMutex(SceUID uid, int newCount, int *numWaitThreads);
int    sceKernelReferMutexStatus(SceUID uid, SceKernelMutexInfo *info);

static int g_emulated = 0;

static int phaseb_wait_thread_end(SceUID thid, SceUInt usec) {
    SceUInt to = usec;
    return sceKernelWaitThreadEnd(thid, &to);
}

static int phaseb_emulator_present(void) {
    uint32_t flag = 0;
    if (sceIoDevctl("emulator:", 3, NULL, 0, &flag, sizeof(flag)) < 0) {
        return 0;
    }
    return flag == 1;
}

static void phaseb_emit(const char *text) {
    if (g_emulated) {
        sceIoDevctl("emulator:", 2, (void *)text, (int)strlen(text), NULL, 0);
    } else {
        printf("%s", text);
    }
    SceUID fd = sceIoOpen(PHASEB_LOG, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, text, strlen(text));
        sceIoClose(fd);
    }
}

static void phaseb_meta(void) {
    char line[512];
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_META schema=1 source=%s model=unknown firmware=unknown "
             "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
             "source_commit=0000000000000000000000000000000000000000 "
             "fixture=%s\n",
             g_emulated ? "ppsspp" : "psp", PHASEB_BUILD_ID);
    phaseb_emit(line);
}

static void phaseb_test(const char *case_id, const char *status,
                        uint32_t result, const uint32_t *out,
                        size_t out_count) {
    char line[1024];
    int used = snprintf(line, sizeof(line),
                        "NAKAGAWA_PSP_TEST schema=1 test_id=%s case_id=%s "
                        "status=%s result=0x%08x",
                        PHASEB_TEST_ID, case_id, status, (unsigned int)result);
    for (size_t i = 0; i < out_count && used > 0 && (size_t)used < sizeof(line); i++) {
        int wrote = snprintf(line + used, sizeof(line) - (size_t)used,
                             " out%u=0x%08x", (unsigned int)i,
                             (unsigned int)out[i]);
        if (wrote < 0) {
            break;
        }
        used += wrote;
    }
    if (used > 0 && (size_t)used + 1 < sizeof(line)) {
        line[used++] = '\n';
        line[used] = '\0';
    }
    phaseb_emit(line);
}

/* =========================================================================
   SECTION A: Exit / Delete Repeat Matrix (12 cells: ED2-*)
   ========================================================================= */

#define ED_METHOD_RETURN     0u
#define ED_METHOD_EXIT       1u
#define ED_METHOD_EXITDELETE 2u
static uint32_t g_ed_status;
static uint32_t g_ed_method;

static int ed_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (g_ed_method == ED_METHOD_EXIT) {
        sceKernelExitThread((int)g_ed_status);
        return 0x55;
    }
    if (g_ed_method == ED_METHOD_EXITDELETE) {
        sceKernelExitDeleteThread((int)g_ed_status);
        return 0x55;
    }
    return (int)g_ed_status;
}

static void phaseb_exit_cell(const char *case_id, uint32_t status, uint32_t method) {
    uint32_t out[6] = {NOT_MEASURED, NOT_MEASURED, NOT_MEASURED,
                       NOT_MEASURED, NOT_MEASURED, NOT_MEASURED};
    g_ed_status = status;
    g_ed_method = method;
    SceUID thid = sceKernelCreateThread(case_id, ed_entry, 32, 0x1000, 0, NULL);
    if (thid < 0) {
        phaseb_test(case_id, "FAIL", (uint32_t)thid, out, 6);
        return;
    }
    out[5] = (uint32_t)sceKernelStartThread(thid, 0, NULL);
    if ((int)out[5] < 0) {
        sceKernelDeleteThread(thid);
        phaseb_test(case_id, "FAIL", out[5], out, 6);
        return;
    }
    out[0] = (uint32_t)phaseb_wait_thread_end(thid, 500000);
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
        phaseb_wait_thread_end(thid, 500000);
        sceKernelDeleteThread(thid);
    }
    phaseb_test(case_id, "PASS", out[0], out, 6);
}

static void phaseb_run_section_a(void) {
    static const struct {
        const char *id;
        uint32_t status;
        uint32_t method;
    } cells[] = {
        {"ED2-R77",  0x00000077u, ED_METHOD_RETURN},
        {"ED2-R00",  0x00000000u, ED_METHOD_RETURN},
        {"ED2-RNEG", 0xffffffefu, ED_METHOD_RETURN},
        {"ED2-RERR", 0x800201acu, ED_METHOD_RETURN},
        {"ED2-X77",  0x00000077u, ED_METHOD_EXIT},
        {"ED2-X00",  0x00000000u, ED_METHOD_EXIT},
        {"ED2-XNEG", 0xffffffefu, ED_METHOD_EXIT},
        {"ED2-XERR", 0x800201acu, ED_METHOD_EXIT},
        {"ED2-D77",  0x00000077u, ED_METHOD_EXITDELETE},
        {"ED2-D00",  0x00000000u, ED_METHOD_EXITDELETE},
        {"ED2-DNEG", 0xffffffefu, ED_METHOD_EXITDELETE},
        {"ED2-DERR", 0x800201acu, ED_METHOD_EXITDELETE},
    };
    for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        phaseb_exit_cell(cells[i].id, cells[i].status, cells[i].method);
    }
}

/* =========================================================================
   SECTION B: Heavyweight Mutex Matrix (19 cells: MTX-*, LWM-*)
   ========================================================================= */

static volatile SceUID g_helper_mtx;
static volatile int g_helper_rc;
static volatile int g_helper_wake_order;
static volatile int g_wake_counter;

static int helper_trylock_entry(SceSize args, void *argp) {
    (void)args; (void)argp;
    g_helper_rc = sceKernelTryLockMutex(g_helper_mtx, 1);
    return g_helper_rc;
}

static int helper_timeout_zero_entry(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned int timeout = 0;
    g_helper_rc = sceKernelLockMutex(g_helper_mtx, 1, &timeout);
    return g_helper_rc;
}

static int helper_timeout_finite_entry(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned int timeout = 10000; /* 10 ms */
    g_helper_rc = sceKernelLockMutex(g_helper_mtx, 1, &timeout);
    return g_helper_rc;
}

static int helper_block_lock_entry(SceSize args, void *argp) {
    (void)args; (void)argp;
    g_helper_rc = sceKernelLockMutex(g_helper_mtx, 1, NULL);
    return g_helper_rc;
}

static int helper_order_waiter1(SceSize args, void *argp) {
    (void)args; (void)argp;
    int rc = sceKernelLockMutex(g_helper_mtx, 1, NULL);
    if (rc == 0) {
        g_helper_wake_order |= ((++g_wake_counter) & 0xFF);
        sceKernelUnlockMutex(g_helper_mtx, 1);
    }
    return rc;
}

static int helper_order_waiter2(SceSize args, void *argp) {
    (void)args; (void)argp;
    int rc = sceKernelLockMutex(g_helper_mtx, 1, NULL);
    if (rc == 0) {
        g_helper_wake_order |= (((++g_wake_counter) & 0xFF) << 8);
        sceKernelUnlockMutex(g_helper_mtx, 1);
    }
    return rc;
}

static void phaseb_run_section_b(void) {
    uint32_t out[6];
    SceKernelMutexInfo info;

    /* Cell 1: MTX-CR-INIT0 (Create unowned mutex) */
    memset(out, 0xFF, sizeof(out));
    SceUID m0 = sceKernelCreateMutex("m_init0", PSP_MUTEX_ATTR_FIFO, 0, NULL);
    if (m0 > 0) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferMutexStatus(m0, &info) == 0) {
            out[0] = (uint32_t)info.initCount;
            out[1] = (uint32_t)info.currentCount;
            out[2] = (uint32_t)info.lockThread;
            out[3] = (uint32_t)info.numWaitThreads;
        }
        sceKernelDeleteMutex(m0);
        phaseb_test("MTX-CR-INIT0", "PASS", (uint32_t)m0, out, 4);
    } else {
        phaseb_test("MTX-CR-INIT0", "FAIL", (uint32_t)m0, out, 4);
    }

    /* Cell 2: MTX-CR-INIT1 (Create initially locked mutex) */
    memset(out, 0xFF, sizeof(out));
    SceUID m1 = sceKernelCreateMutex("m_init1", PSP_MUTEX_ATTR_FIFO, 1, NULL);
    if (m1 > 0) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferMutexStatus(m1, &info) == 0) {
            out[0] = (uint32_t)info.initCount;
            out[1] = (uint32_t)info.currentCount;
            out[2] = (uint32_t)info.lockThread;
            out[3] = (uint32_t)sceKernelGetThreadId();
        }
        sceKernelDeleteMutex(m1);
        phaseb_test("MTX-CR-INIT1", "PASS", (uint32_t)m1, out, 4);
    } else {
        phaseb_test("MTX-CR-INIT1", "FAIL", (uint32_t)m1, out, 4);
    }

    /* Cell 3: MTX-CR-BADATTR (Illegal attribute bits) */
    memset(out, 0xFF, sizeof(out));
    SceUID bad_attr = sceKernelCreateMutex("bad_attr", 0x1000u, 0, NULL);
    out[0] = (uint32_t)bad_attr;
    phaseb_test("MTX-CR-BADATTR", "PASS", (uint32_t)bad_attr, out, 1);

    /* Cell 4: MTX-CR-BADCNT (Illegal initial count) */
    memset(out, 0xFF, sizeof(out));
    SceUID bad_cnt = sceKernelCreateMutex("bad_cnt", PSP_MUTEX_ATTR_FIFO, -1, NULL);
    out[0] = (uint32_t)bad_cnt;
    phaseb_test("MTX-CR-BADCNT", "PASS", (uint32_t)bad_cnt, out, 1);

    /* Cell 5: MTX-CR-REC (Recursive mutex creation with initCount=5) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_rec = sceKernelCreateMutex("m_rec", PSP_MUTEX_ATTR_ALLOW_RECURSIVE, 5, NULL);
    if (m_rec > 0) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferMutexStatus(m_rec, &info) == 0) {
            out[0] = (uint32_t)info.initCount;
            out[1] = (uint32_t)info.currentCount;
            out[2] = (uint32_t)info.lockThread;
        }
        sceKernelDeleteMutex(m_rec);
        phaseb_test("MTX-CR-REC", "PASS", (uint32_t)m_rec, out, 3);
    } else {
        phaseb_test("MTX-CR-REC", "FAIL", (uint32_t)m_rec, out, 3);
    }

    /* Cell 6: MTX-LK-UNCONT (Uncontended lock acquisition) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_lk = sceKernelCreateMutex("m_lk", PSP_MUTEX_ATTR_FIFO, 0, NULL);
    int rc_lk = sceKernelLockMutex(m_lk, 1, NULL);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    sceKernelReferMutexStatus(m_lk, &info);
    out[0] = (uint32_t)rc_lk;
    out[1] = (uint32_t)info.currentCount;
    out[2] = (uint32_t)info.lockThread;
    phaseb_test("MTX-LK-UNCONT", "PASS", (uint32_t)rc_lk, out, 3);

    /* Cell 7: MTX-LK-NOREC (Non-recursive relock rejected) */
    memset(out, 0xFF, sizeof(out));
    int rc_norec = sceKernelLockMutex(m_lk, 1, NULL);
    out[0] = (uint32_t)rc_norec;
    phaseb_test("MTX-LK-NOREC", "PASS", (uint32_t)rc_norec, out, 1);

    /* Cell 8: MTX-UL-UNCONT (Uncontended unlock) */
    memset(out, 0xFF, sizeof(out));
    int rc_ul = sceKernelUnlockMutex(m_lk, 1);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    sceKernelReferMutexStatus(m_lk, &info);
    out[0] = (uint32_t)rc_ul;
    out[1] = (uint32_t)info.currentCount;
    out[2] = (uint32_t)info.lockThread;
    phaseb_test("MTX-UL-UNCONT", "PASS", (uint32_t)rc_ul, out, 3);
    sceKernelDeleteMutex(m_lk);

    /* Cell 9: MTX-LK-REC (Recursive relock and nested unlock) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_recurse = sceKernelCreateMutex("m_recurse", PSP_MUTEX_ATTR_ALLOW_RECURSIVE, 0, NULL);
    int r1 = sceKernelLockMutex(m_recurse, 2, NULL);
    int r2 = sceKernelLockMutex(m_recurse, 3, NULL);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    sceKernelReferMutexStatus(m_recurse, &info);
    out[0] = (uint32_t)r1;
    out[1] = (uint32_t)r2;
    out[2] = (uint32_t)info.currentCount;
    int u1 = sceKernelUnlockMutex(m_recurse, 2);
    int u2 = sceKernelUnlockMutex(m_recurse, 3);
    out[3] = (uint32_t)u1;
    out[4] = (uint32_t)u2;
    phaseb_test("MTX-LK-REC", "PASS", (uint32_t)r2, out, 5);
    sceKernelDeleteMutex(m_recurse);

    /* Cell 10: MTX-UL-NOTOWN (Unlock unowned / wrong owner) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_notown = sceKernelCreateMutex("m_notown", PSP_MUTEX_ATTR_FIFO, 0, NULL);
    int rc_notown = sceKernelUnlockMutex(m_notown, 1);
    out[0] = (uint32_t)rc_notown;
    phaseb_test("MTX-UL-NOTOWN", "PASS", (uint32_t)rc_notown, out, 1);
    sceKernelDeleteMutex(m_notown);

    /* Cell 11: MTX-TRY-LOCK (TryLock uncontended) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_try = sceKernelCreateMutex("m_try", PSP_MUTEX_ATTR_FIFO, 0, NULL);
    int rc_try1 = sceKernelTryLockMutex(m_try, 1);
    out[0] = (uint32_t)rc_try1;
    phaseb_test("MTX-TRY-LOCK", "PASS", (uint32_t)rc_try1, out, 1);

    /* Cell 12: MTX-TRY-FAIL (TryLock contended from helper thread) */
    memset(out, 0xFF, sizeof(out));
    g_helper_mtx = m_try; /* Main holds m_try */
    g_helper_rc = 0;
    SceUID th_try = sceKernelCreateThread("th_try", helper_trylock_entry, 32, 0x1000, 0, NULL);
    sceKernelStartThread(th_try, 0, NULL);
    phaseb_wait_thread_end(th_try, 500000);
    sceKernelDeleteThread(th_try);
    out[0] = (uint32_t)g_helper_rc;
    phaseb_test("MTX-TRY-FAIL", "PASS", (uint32_t)g_helper_rc, out, 1);
    sceKernelUnlockMutex(m_try, 1);
    sceKernelDeleteMutex(m_try);

    /* Cell 13: MTX-TO-ZERO (Lock with timeout=0 on held mutex) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_to0 = sceKernelCreateMutex("m_to0", PSP_MUTEX_ATTR_FIFO, 1, NULL);
    g_helper_mtx = m_to0;
    g_helper_rc = 0;
    SceUID th_to0 = sceKernelCreateThread("th_to0", helper_timeout_zero_entry, 32, 0x1000, 0, NULL);
    sceKernelStartThread(th_to0, 0, NULL);
    phaseb_wait_thread_end(th_to0, 500000);
    sceKernelDeleteThread(th_to0);
    out[0] = (uint32_t)g_helper_rc;
    phaseb_test("MTX-TO-ZERO", "PASS", (uint32_t)g_helper_rc, out, 1);
    sceKernelDeleteMutex(m_to0);

    /* Cell 14: MTX-TO-FINITE (Lock with finite timeout on held mutex) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_tof = sceKernelCreateMutex("m_tof", PSP_MUTEX_ATTR_FIFO, 1, NULL);
    g_helper_mtx = m_tof;
    g_helper_rc = 0;
    SceUID th_tof = sceKernelCreateThread("th_tof", helper_timeout_finite_entry, 32, 0x1000, 0, NULL);
    sceKernelStartThread(th_tof, 0, NULL);
    phaseb_wait_thread_end(th_tof, 500000);
    sceKernelDeleteThread(th_tof);
    out[0] = (uint32_t)g_helper_rc;
    phaseb_test("MTX-TO-FINITE", "PASS", (uint32_t)g_helper_rc, out, 1);
    sceKernelDeleteMutex(m_tof);

    /* Cell 15: MTX-CANCEL (Cancel mutex wakes blocked waiter) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_can = sceKernelCreateMutex("m_can", PSP_MUTEX_ATTR_FIFO, 1, NULL);
    g_helper_mtx = m_can;
    g_helper_rc = 0;
    SceUID th_can = sceKernelCreateThread("th_can", helper_block_lock_entry, 33, 0x1000, 0, NULL);
    sceKernelStartThread(th_can, 0, NULL);
    sceKernelDelayThread(5000); /* 5 ms delay to ensure helper is queued */
    int num_waiting = 0;
    int rc_cancel = sceKernelCancelMutex(m_can, 0, &num_waiting);
    phaseb_wait_thread_end(th_can, 500000);
    sceKernelDeleteThread(th_can);
    out[0] = (uint32_t)rc_cancel;
    out[1] = (uint32_t)g_helper_rc;
    out[2] = (uint32_t)num_waiting;
    phaseb_test("MTX-CANCEL", "PASS", (uint32_t)rc_cancel, out, 3);
    sceKernelDeleteMutex(m_can);

    /* Cell 16: MTX-DEL-WAKE (Delete mutex wakes blocked waiter) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_del = sceKernelCreateMutex("m_del", PSP_MUTEX_ATTR_FIFO, 1, NULL);
    g_helper_mtx = m_del;
    g_helper_rc = 0;
    SceUID th_del = sceKernelCreateThread("th_del", helper_block_lock_entry, 33, 0x1000, 0, NULL);
    sceKernelStartThread(th_del, 0, NULL);
    sceKernelDelayThread(5000);
    int rc_del = sceKernelDeleteMutex(m_del);
    phaseb_wait_thread_end(th_del, 500000);
    sceKernelDeleteThread(th_del);
    out[0] = (uint32_t)rc_del;
    out[1] = (uint32_t)g_helper_rc;
    phaseb_test("MTX-DEL-WAKE", "PASS", (uint32_t)rc_del, out, 2);

    /* Cell 17: MTX-WAIT-FIFO (FIFO queue wake order) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_fifo = sceKernelCreateMutex("m_fifo", PSP_MUTEX_ATTR_FIFO, 1, NULL);
    g_helper_mtx = m_fifo;
    g_helper_wake_order = 0;
    g_wake_counter = 0;
    SceUID th_f1 = sceKernelCreateThread("th_f1", helper_order_waiter1, 33, 0x1000, 0, NULL);
    SceUID th_f2 = sceKernelCreateThread("th_f2", helper_order_waiter2, 33, 0x1000, 0, NULL);
    sceKernelStartThread(th_f1, 0, NULL);
    sceKernelDelayThread(2000);
    sceKernelStartThread(th_f2, 0, NULL);
    sceKernelDelayThread(2000);
    sceKernelUnlockMutex(m_fifo, 1);
    phaseb_wait_thread_end(th_f1, 500000);
    phaseb_wait_thread_end(th_f2, 500000);
    sceKernelDeleteThread(th_f1);
    sceKernelDeleteThread(th_f2);
    out[0] = (uint32_t)g_helper_wake_order; /* Expected: waiter1=1, waiter2=2 -> 0x0201 */
    phaseb_test("MTX-WAIT-FIFO", "PASS", (uint32_t)g_helper_wake_order, out, 1);
    sceKernelDeleteMutex(m_fifo);

    /* Cell 18: MTX-WAIT-PRIO (Priority-based wake order) */
    memset(out, 0xFF, sizeof(out));
    SceUID m_prio = sceKernelCreateMutex("m_prio", PSP_MUTEX_ATTR_PRIORITY, 1, NULL);
    g_helper_mtx = m_prio;
    g_helper_wake_order = 0;
    g_wake_counter = 0;
    /* th_p1 has priority 34 (lower prio), th_p2 has priority 30 (higher prio) */
    SceUID th_p1 = sceKernelCreateThread("th_p1", helper_order_waiter1, 34, 0x1000, 0, NULL);
    SceUID th_p2 = sceKernelCreateThread("th_p2", helper_order_waiter2, 30, 0x1000, 0, NULL);
    sceKernelStartThread(th_p1, 0, NULL);
    sceKernelDelayThread(2000);
    sceKernelStartThread(th_p2, 0, NULL);
    sceKernelDelayThread(2000);
    sceKernelUnlockMutex(m_prio, 1);
    phaseb_wait_thread_end(th_p2, 500000);
    phaseb_wait_thread_end(th_p1, 500000);
    sceKernelDeleteThread(th_p1);
    sceKernelDeleteThread(th_p2);
    out[0] = (uint32_t)g_helper_wake_order; /* Higher priority waiter2 wakes first -> 0x0102 */
    phaseb_test("MTX-WAIT-PRIO", "PASS", (uint32_t)g_helper_wake_order, out, 1);
    sceKernelDeleteMutex(m_prio);

    /* Cell 19: LWM-CR-LK-UL (Control: Lightweight Mutex in User Mode) */
    memset(out, 0xFF, sizeof(out));
    SceLwMutexWorkarea lwm_work;
    memset(&lwm_work, 0, sizeof(lwm_work));
    int lwm_cr = sceKernelCreateLwMutex(&lwm_work, "lwm", PSP_LW_MUTEX_ATTR_THFIFO, 0, NULL);
    int lwm_try = sceKernelTryLockLwMutex(&lwm_work, 1);
    int lwm_ul = sceKernelUnlockLwMutex(&lwm_work, 1);
    int lwm_del = sceKernelDeleteLwMutex(&lwm_work);
    out[0] = (uint32_t)lwm_cr;
    out[1] = (uint32_t)lwm_try;
    out[2] = (uint32_t)lwm_ul;
    out[3] = (uint32_t)lwm_del;
    phaseb_test("LWM-CR-LK-UL", "PASS", (uint32_t)lwm_cr, out, 4);
}

/* =========================================================================
   SECTION C: Nested User Callback & Stack Contract (6 cells: CB-*)
   ========================================================================= */

static volatile uint32_t g_caller_sp;
static volatile uint32_t g_caller_gp;
static volatile uint32_t g_caller_thid;
static volatile uint32_t g_stack_base;
static volatile uint32_t g_stack_size;

static volatile uint32_t g_cb1_sp;
static volatile uint32_t g_cb1_gp;
static volatile uint32_t g_cb1_ra;
static volatile uint32_t g_cb1_thid;
static volatile uint32_t g_cb1_fcr31;
static volatile uint32_t g_cb1_arg1;
static volatile uint32_t g_cb1_arg2;
static volatile int      g_cb1_called;

static volatile uint32_t g_cb2_sp;
static volatile uint32_t g_cb2_gp;
static volatile uint32_t g_cb2_ra;
static volatile uint32_t g_cb2_thid;
static volatile int      g_cb2_called;

static volatile SceUID   g_cbid2;

static int callback_depth2(int arg1, int arg2, void *common) {
    (void)arg1; (void)arg2; (void)common;
    register uint32_t sp __asm__("$sp");
    register uint32_t gp __asm__("$gp");
    register uint32_t ra __asm__("$ra");
    g_cb2_sp = sp;
    g_cb2_gp = gp;
    g_cb2_ra = ra;
    g_cb2_thid = (uint32_t)sceKernelGetThreadId();
    g_cb2_called++;
    return 0x22;
}

static int callback_depth1(int arg1, int arg2, void *common) {
    (void)common;
    register uint32_t sp __asm__("$sp");
    register uint32_t gp __asm__("$gp");
    register uint32_t ra __asm__("$ra");
    uint32_t fcr;
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcr));
    g_cb1_sp = sp;
    g_cb1_gp = gp;
    g_cb1_ra = ra;
    g_cb1_fcr31 = fcr;
    g_cb1_thid = (uint32_t)sceKernelGetThreadId();
    g_cb1_arg1 = (uint32_t)arg1;
    g_cb1_arg2 = (uint32_t)arg2;
    g_cb1_called++;

    /* Test Nesting: notify and dispatch depth 2 callback from within depth 1 */
    sceKernelNotifyCallback(g_cbid2, 0x8888);
    sceKernelCheckCallback();
    return 0x11;
}

static void phaseb_run_section_c(void) {
    uint32_t out[6];
    g_cb1_called = 0;
    g_cb2_called = 0;

    SceKernelThreadInfo tinfo;
    memset(&tinfo, 0, sizeof(tinfo));
    tinfo.size = sizeof(tinfo);
    sceKernelReferThreadStatus(0, &tinfo);
    g_stack_base = (uint32_t)tinfo.stack;
    g_stack_size = (uint32_t)tinfo.stackSize;
    g_caller_thid = (uint32_t)sceKernelGetThreadId();

    SceUID cbid1 = sceKernelCreateCallback("cb_depth1", callback_depth1, NULL);
    g_cbid2 = sceKernelCreateCallback("cb_depth2", callback_depth2, NULL);

    /* Capture caller environment right before dispatch */
    register uint32_t cur_sp __asm__("$sp");
    register uint32_t cur_gp __asm__("$gp");
    g_caller_sp = cur_sp;
    g_caller_gp = cur_gp;

    /* Notify and dispatch */
    sceKernelNotifyCallback(cbid1, 0x7777);
    int check_rc = sceKernelCheckCallback();

    /* Cell 1: CB-ORDINARY-EXEC (Callback fired with correct context) */
    memset(out, 0xFF, sizeof(out));
    out[0] = (uint32_t)check_rc;
    out[1] = (uint32_t)g_cb1_called;
    out[2] = g_cb1_thid;
    out[3] = g_caller_thid;
    out[4] = g_cb1_arg2; /* Expect 0x7777 */
    phaseb_test("CB-ORDINARY-EXEC", "PASS", (uint32_t)check_rc, out, 5);

    /* Cell 2: CB-STACK-LOC (Is callback SP within thread stack bounds?) */
    memset(out, 0xFF, sizeof(out));
    uint32_t stack_bottom = g_stack_base;
    uint32_t stack_top = g_stack_base + g_stack_size;
    int sp_in_bounds = (g_cb1_sp >= stack_bottom && g_cb1_sp <= stack_top) ? 1 : 0;
    out[0] = (uint32_t)sp_in_bounds;
    out[1] = g_cb1_sp;
    out[2] = stack_bottom;
    out[3] = stack_top;
    phaseb_test("CB-STACK-LOC", "PASS", (uint32_t)sp_in_bounds, out, 4);

    /* Cell 3: CB-STACK-DELTA (Stack consumption of callback dispatch frame) */
    memset(out, 0xFF, sizeof(out));
    int32_t delta = (int32_t)(g_caller_sp - g_cb1_sp);
    out[0] = (uint32_t)delta;
    out[1] = g_caller_sp;
    out[2] = g_cb1_sp;
    phaseb_test("CB-STACK-DELTA", "PASS", (uint32_t)delta, out, 3);

    /* Cell 4: CB-REG-PRESERVE (GP register match and RA validity) */
    memset(out, 0xFF, sizeof(out));
    int gp_match = (g_cb1_gp == g_caller_gp) ? 1 : 0;
    out[0] = (uint32_t)gp_match;
    out[1] = g_cb1_gp;
    out[2] = g_caller_gp;
    out[3] = g_cb1_ra;
    phaseb_test("CB-REG-PRESERVE", "PASS", (uint32_t)gp_match, out, 4);

    /* Cell 5: CB-NESTED-DEPTH2 (Nested callback execution and nested SP delta) */
    memset(out, 0xFF, sizeof(out));
    int32_t nest_delta = (int32_t)(g_cb1_sp - g_cb2_sp);
    out[0] = (uint32_t)g_cb2_called;
    out[1] = g_cb2_thid;
    out[2] = g_cb2_sp;
    out[3] = (uint32_t)nest_delta;
    phaseb_test("CB-NESTED-DEPTH2", "PASS", (uint32_t)g_cb2_called, out, 4);

    /* Cell 6: CB-FCR31 (Inherited FCR31 in callback context) */
    memset(out, 0xFF, sizeof(out));
    out[0] = g_cb1_fcr31;
    phaseb_test("CB-FCR31", "PASS", g_cb1_fcr31, out, 1);

    sceKernelDeleteCallback(cbid1);
    sceKernelDeleteCallback(g_cbid2);
}

/* =========================================================================
   SECTION D: GE Callback & Stack Contract (4 cells: GE-*)
   ========================================================================= */

static volatile uint32_t g_ge_cb_sp;
static volatile uint32_t g_ge_cb_gp;
static volatile uint32_t g_ge_cb_ra;
static volatile uint32_t g_ge_cb_thid;
static volatile int      g_ge_cb_ran;

static uint32_t s_ge_min_list[16] __attribute__((aligned(64)));

static void ge_finish_stack_cb(int id, void *arg) {
    (void)id; (void)arg;
    register uint32_t sp __asm__("$sp");
    register uint32_t gp __asm__("$gp");
    register uint32_t ra __asm__("$ra");
    g_ge_cb_sp = sp;
    g_ge_cb_gp = gp;
    g_ge_cb_ra = ra;
    g_ge_cb_thid = (uint32_t)sceKernelGetThreadId();
    g_ge_cb_ran++;
}

static void phaseb_run_section_d(void) {
    uint32_t out[6];
    g_ge_cb_ran = 0;
    g_ge_cb_sp = 0;
    g_ge_cb_gp = 0;
    g_ge_cb_ra = 0;
    g_ge_cb_thid = 0;

    /* Build minimal valid display list: FINISH (0x0F000000), END (0x0C000000), NOPs */
    s_ge_min_list[0] = 0x0F000000u;
    s_ge_min_list[1] = 0x0C000000u;
    s_ge_min_list[2] = 0x00000000u;
    s_ge_min_list[3] = 0x00000000u;
    sceKernelDcacheWritebackAll();

    PspGeCallbackData cb_data;
    memset(&cb_data, 0, sizeof(cb_data));
    cb_data.signal_func = NULL;
    cb_data.signal_arg = NULL;
    cb_data.finish_func = ge_finish_stack_cb;
    cb_data.finish_arg = NULL;

    int ge_cbid = sceGeSetCallback(&cb_data);

    int qid = -1;
    if (ge_cbid >= 0) {
        qid = sceGeListEnQueue((void *)s_ge_min_list, NULL, ge_cbid, NULL);
        if (qid >= 0) {
            sceGeListSync(qid, 0);
        }
    }

    /* Cell 1: GE-CB-FIRED (GE Finish callback executed) */
    memset(out, 0xFF, sizeof(out));
    out[0] = (uint32_t)ge_cbid;
    out[1] = (uint32_t)qid;
    out[2] = (uint32_t)g_ge_cb_ran;
    phaseb_test("GE-CB-FIRED", "PASS", (uint32_t)g_ge_cb_ran, out, 3);

    /* Cell 2: GE-CB-THREAD (Thread ID of GE callback vs Main thread) */
    memset(out, 0xFF, sizeof(out));
    uint32_t main_thid = (uint32_t)sceKernelGetThreadId();
    out[0] = g_ge_cb_thid;
    out[1] = main_thid;
    out[2] = (g_ge_cb_thid == main_thid) ? 1u : 0u;
    phaseb_test("GE-CB-THREAD", "PASS", g_ge_cb_thid, out, 3);

    /* Cell 3: GE-CB-STACK (Stack pointer and GP of GE callback) */
    memset(out, 0xFF, sizeof(out));
    out[0] = g_ge_cb_sp;
    out[1] = g_ge_cb_gp;
    out[2] = g_ge_cb_ra;
    phaseb_test("GE-CB-STACK", "PASS", g_ge_cb_sp, out, 3);

    /* Cell 4: GE-CB-CLEANUP (Unset GE callback) */
    memset(out, 0xFF, sizeof(out));
    int unset_rc = -1;
    if (ge_cbid >= 0) {
        unset_rc = sceGeUnsetCallback(ge_cbid);
    }
    out[0] = (uint32_t)unset_rc;
    phaseb_test("GE-CB-CLEANUP", "PASS", (uint32_t)unset_rc, out, 1);
}

/* =========================================================================
   MAIN ENTRY
   ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    g_emulated = phaseb_emulator_present();
    setvbuf(stdout, NULL, _IONBF, 0);

    phaseb_meta();

    phaseb_emit("# --- BEGIN SECTION C: NESTED CALLBACK STACK ---\n");
    phaseb_run_section_c();
    phaseb_emit("# --- END SECTION C: NESTED CALLBACK STACK ---\n");

    phaseb_emit("# --- BEGIN SECTION D: GE CALLBACK STACK ---\n");
    phaseb_run_section_d();
    phaseb_emit("# --- END SECTION D: GE CALLBACK STACK ---\n");

    phaseb_emit("# --- BEGIN SECTION B: HEAVYWEIGHT MUTEX ---\n");
    phaseb_run_section_b();
    phaseb_emit("# --- END SECTION B: HEAVYWEIGHT MUTEX ---\n");

    phaseb_emit("# --- BEGIN SECTION A: EXIT/DELETE REPEAT ---\n");
    phaseb_run_section_a();
    phaseb_emit("# --- END SECTION A: EXIT/DELETE REPEAT ---\n");

    uint32_t summary_out[1] = {41u};
    phaseb_test("PHASEB-COMPLETE", "PASS", 0, summary_out, 1);

    return 0;
}
