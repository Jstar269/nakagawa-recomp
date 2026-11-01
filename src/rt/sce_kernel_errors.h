// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* The one place the runtime names a SCE_KERNEL_ERROR_* status (#1531). The scheduler and every
 * runtime HLE file include it, so a value is spelled once. Before, sched.c, hle_internal.h, hle.c
 * and the hle_kernel_*.c files each redefined the names they used, three of them with #ifndef
 * guards that depended on include order. The values are unchanged. Where a comment says "measured"
 * the value comes from a PSP run or a PSPAutotests oracle cell; the rest are the names and values
 * of the public PSPSDK pspkerror.h. */
#ifndef NK_SCE_KERNEL_ERRORS_H
#define NK_SCE_KERNEL_ERRORS_H

/* ---- memory and general ---- */
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR               0x80000103u
#define SCE_KERNEL_ERROR_INVALID_MODE               0x80000107u
/* PSP's public pspkerror.h names ILLEGAL_ADDR as 0x800200D3 for kernel APIs. Keep it distinct
 * from the subsystem-specific 0x80000103 memory error used by several HLE objects. */
#define SCE_KERNEL_ERROR_KERNEL_ILLEGAL_ADDR        0x800200d3u
#define SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT           0x800200d2u
#define SCE_KERNEL_ERROR_NO_MEMORY                  0x80020190u

/* ---- interrupt manager (PSPSDK pspkerror.h names for its refusals) ---- */
#define SCE_KERNEL_ERROR_ILLEGAL_CONTEXT            0x80020064u
#define SCE_KERNEL_ERROR_ILLEGAL_INTRCODE           0x80020065u
#define SCE_KERNEL_ERROR_FOUND_HANDLER              0x80020067u
#define SCE_KERNEL_ERROR_NOTFOUND_HANDLER           0x80020068u

/* ---- modules. Defined in the public PSPSDK pspkerror.h; the lifecycle mapping that uses the
 * state errors is source-backed and remains unmeasured on hardware. ---- */
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE             0x8002012eu
#define SCE_KERNEL_ERROR_ALREADY_STARTED            0x80020133u
#define SCE_KERNEL_ERROR_NOT_STARTED                0x80020134u
#define SCE_KERNEL_ERROR_ALREADY_STOPPED            0x80020135u
#define SCE_KERNEL_ERROR_CAN_NOT_STOP               0x80020136u
#define SCE_KERNEL_ERROR_ALREADY_STOPPING           0x8002013fu
#define SCE_KERNEL_ERROR_MODULE_MGR_BUSY            0x80020143u
#define SCE_KERNEL_ERROR_CANNOT_GET_MODULE_INFO     0x8002014au

/* ---- threads. The suspend/resume statuses are the measured "Threads / Status codes" cells in
 * docs/HARDWARE_ORACLE.md (double suspend, resume of a thread that is not suspended); the
 * symbolic names are local labels. ---- */
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR               0x80020191u
#define SCE_KERNEL_ERROR_ILLEGAL_PRIORITY           0x80020193u
#define SCE_KERNEL_ERROR_ILLEGAL_THID               0x80020197u
#define SCE_KERNEL_ERROR_UNKNOWN_THID               0x80020198u
#define SCE_KERNEL_ERROR_DORMANT                    0x800201a2u
#define SCE_KERNEL_ERROR_SUSPEND                    0x800201a3u
#define SCE_KERNEL_ERROR_NOT_DORMANT                0x800201a4u
#define SCE_KERNEL_ERROR_NOT_SUSPEND                0x800201a5u
#define SCE_KERNEL_ERROR_THREAD_TERMINATED          0x800201acu

/* ---- unknown kernel-object ids ---- */
#define SCE_KERNEL_ERROR_UNKNOWN_SEMID              0x80020199u
#define SCE_KERNEL_ERROR_UNKNOWN_EVFID              0x8002019au
#define SCE_KERNEL_ERROR_UNKNOWN_MBXID              0x8002019bu
#define SCE_KERNEL_ERROR_UNKNOWN_VPLID              0x8002019cu
#define SCE_KERNEL_ERROR_UNKNOWN_FPLID              0x8002019du
#define SCE_KERNEL_ERROR_UNKNOWN_MPPID              0x8002019eu

/* ---- waits. A blocking call made with CPU interrupts disabled or thread dispatch disabled
 * returns CAN_NOT_WAIT, per PSPAutotests tests/intr/waits.expected. The handlers check
 * sched_wait_permitted() individually, each at the point its own oracle cells put it -- never
 * as a shared pre-handler gate, because the same oracle shows the precedence is per-API:
 * ILLEGAL_MODE (L72) and ILLEGAL_THID (L204) both beat it, while sceKernelWaitSema's bad-object
 * error (L54/L55) does not. See docs/PSP_INTR_WAITS_MATRIX.md.
 *
 * The semaphore codes were measured by PSPAutotests tests/threads/semaphores/wait.expected, a
 * NORMAL-context oracle, so it covers exactly the column tests/intr/waits.expected does not:
 *   UNKNOWN_SEMID  sceKernelWaitSema(0, 1, NULL)          -> 80020199   (L21)
 *                  sceKernelWaitSema(0xDEADBEEF, 1, NULL) -> 80020199   (L23)
 *                  a deleted semaphore id                 -> 80020199   (L25)
 *   ILLEGAL_COUNT  need 100 against maxCount 1            -> 800201BD   (L3, L11)
 *                  need 0                                 -> 800201BD   (L13)
 *                  need -1                                -> 800201BD   (L5, L15)
 * WAIT_TIMEOUT is the same file's L9/L17 result for a wait that was legal, entered, and
 * expired. */
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT               0x800201a7u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT               0x800201a8u
#define SCE_KERNEL_ERROR_WAIT_CANCEL                0x800201a9u
#define SCE_KERNEL_ERROR_WAIT_RELEASE               0x800201aau
#define SCE_KERNEL_ERROR_WAIT_DELETE                0x800201b5u

/* ---- mailboxes, message pipes, semaphore counts and sizes ---- */
#define SCE_KERNEL_ERROR_MBX_EMPTY                  0x800201b2u
#define SCE_KERNEL_ERROR_MPP_FULL                   0x800201b3u
#define SCE_KERNEL_ERROR_MPP_EMPTY                  0x800201b4u
#define SCE_KERNEL_ERROR_ILLEGAL_SIZE               0x800201bcu
#define SCE_KERNEL_ERROR_ILLEGAL_COUNT              0x800201bdu

/* ---- mutexes and lightweight mutexes ---- */
#define SCE_KERNEL_ERROR_UNKNOWN_MUTEXID            0x800201c3u
#define SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN        0x800201c4u
#define SCE_KERNEL_ERROR_LWMUTEX_FAILED_TO_OWN      0x800201c4u
#define SCE_KERNEL_ERROR_MUTEX_NOT_OWNED            0x800201c5u
#define SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW        0x800201c6u
#define SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW     0x800201c7u
#define SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED 0x800201c8u
#define SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND          0x800201cau
#define SCE_KERNEL_ERROR_LWMUTEX_LOCKED             0x800201cbu
#define SCE_KERNEL_ERROR_LWMUTEX_UNLOCK_UNDERFLOW   0x800201ccu
#define SCE_KERNEL_ERROR_LWMUTEX_RECURSIVE_NOT_ALLOWED 0x800201cfu

#endif /* NK_SCE_KERNEL_ERRORS_H */
