// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-12.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

/* Plain PSP mutexes (sceKernelCreateMutex / LockMutex / ... family).
 *
 * This is a dedicated typed object model, deliberately separate from the
 * semaphore/event-flag `Sync` slab in hle.c.  Plain Mutex used to be routed
 * there (Create -> h_CreateSema, everything else -> h_ok) under a
 * "single-threaded recompiler" premise that is false: the runtime schedules
 * real PSP threads cooperatively (src/rt/sched.c), and a plain mutex is not a
 * counting semaphore -- it has an owner thread, a recursion count, FIFO /
 * priority waiter ordering, and a different error space.
 *
 * The behaviour implemented in mutex.c is pinned to the PSPAutotests
 * tests/threads/mutex/ expectations (create, delete, lock, try, unlock, unlock2,
 * cancel, refer, priority, mutex) and tests/intr/waits.expected for the
 * interrupt/dispatch-context cells, corroborated by PPSSPP
 * Core/HLE/sceKernelMutex.cpp.  See the contract table at the top of mutex.c.
 */

#ifndef SR_RT_MUTEX_H
#define SR_RT_MUTEX_H

#include <stdint.h>

/* ---- error codes (PSP ThreadManForUser; PPSSPP Core/HLE/ErrorCodes.h) ---- */
#define SCE_KERNEL_ERROR_ERROR                      0x80020001u
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR               0x80020191u
#define SCE_KERNEL_ERROR_NO_MEMORY                   0x80020190u
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT               0x800201a7u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT               0x800201a8u
#define SCE_KERNEL_ERROR_WAIT_CANCEL                0x800201a9u
#define SCE_KERNEL_ERROR_WAIT_DELETE                0x800201b5u
#define SCE_KERNEL_ERROR_ILLEGAL_COUNT              0x800201bdu
#define SCE_KERNEL_ERROR_MUTEX_NOT_FOUND            0x800201c3u
#define SCE_KERNEL_ERROR_MUTEX_LOCKED               0x800201c4u
#define SCE_KERNEL_ERROR_MUTEX_UNLOCKED             0x800201c5u
#define SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW        0x800201c6u
#define SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW     0x800201c7u
#define SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED 0x800201c8u

/* ---- attributes (pspthreadman.h / PSPAutotests tests/threads/mutex) ---- */
#define PSP_MUTEX_ATTR_FIFO            0x000u
#define PSP_MUTEX_ATTR_PRIORITY        0x100u
#define PSP_MUTEX_ATTR_ALLOW_RECURSIVE 0x200u
/* create.expected accepts attrs 0x1, 0x100, 0x200, 0x800, 0xb00 and 0xbff and
 * rejects 0x400, 0xc00 and every value with a bit above 0x800.  The hardware
 * mask is therefore 0xBFF (0x800 | 0x3FF): bit 10 is reserved. */
#define PSP_MUTEX_ATTR_KNOWN_MASK      0xBFFu

/* ---- SceKernelMutexInfo (pspthreadman.h) ---------------------------------
 *   +0x00 SceSize  size             (always reported as 0x38)
 *   +0x04 char     name[32]
 *   +0x24 SceUInt  attr
 *   +0x28 int      initCount
 *   +0x2C int      currentCount     (live lock level)
 *   +0x30 SceUID   lockThread       (owning thread uid; 0 while unlocked)
 *   +0x34 int      numWaitThreads
 *
 * size is read first; when it is 0 the kernel writes nothing and returns 0,
 * otherwise it writes the full 0x38-byte record (refer.expected). */
#define SR_MUTEX_INFO_SIZE      0x38u
#define SR_MUTEX_INFO_NAME_OFF  0x04u
#define SR_MUTEX_INFO_NAME_LEN  32
#define SR_MUTEX_INFO_ATTR_OFF  0x24u
#define SR_MUTEX_INFO_INIT_OFF  0x28u
#define SR_MUTEX_INFO_CUR_OFF   0x2cu
#define SR_MUTEX_INFO_OWNER_OFF 0x30u
#define SR_MUTEX_INFO_WAIT_OFF  0x34u

/* ---- HLE entry points (thin glue lives in hle.c) -------------------------
 * Each reads its arguments from guest memory / registers exactly as the
 * production import stubs pass them, and returns the $v0 value. */

/* sceKernelCreateMutex(name, attr, initialCount, optionsPtr). */
uint32_t sr_mutex_create(uint32_t name, uint32_t attr, int32_t initial, uint32_t options);

/* sceKernelDeleteMutex(uid). Wakes waiters with WAIT_DELETE. */
uint32_t sr_mutex_delete(uint32_t uid);

/* sceKernelLockMutex[CB](uid, count, timeoutPtr). `cb` selects the
 * callback-aware wait. Both share the measured context-first entry contract. */
uint32_t sr_mutex_lock(uint32_t uid, int32_t count, uint32_t timeout_ptr, int cb);

/* sceKernelTryLockMutex(uid, count). Never blocks; no context gate. */
uint32_t sr_mutex_try_lock(uint32_t uid, int32_t count);

/* sceKernelUnlockMutex(uid, count). */
uint32_t sr_mutex_unlock(uint32_t uid, int32_t count);

/* sceKernelCancelMutex(uid, newCount, numWaitThreadsPtr). */
uint32_t sr_mutex_cancel(uint32_t uid, int32_t count, uint32_t num_wait_ptr);

/* sceKernelReferMutexStatus(uid, infoPtr). */
uint32_t sr_mutex_refer_status(uint32_t uid, uint32_t info_ptr);

/* Thread-teardown hook, called from the scheduler (sched.c) on thread exit,
 * termination and deletion. Releases every mutex the thread owns (handing each
 * to its next waiter) and removes it from every waiter list. */
void sr_mutex_thread_end(uint32_t thread_uid);

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-only: return the whole object table to the empty state and expose a
 * read-only view of one live object so the executable suite can assert
 * guest-visible state without a second implementation. */
void sr_mutex_test_reset(void);
int sr_mutex_test_state(uint32_t uid, int32_t *level_out, uint32_t *owner_out,
                        int32_t *initial_out, uint32_t *attr_out,
                        int *waiters_out);
#endif /* SR_HLE_THREAD_SELFTEST */

#endif /* SR_RT_MUTEX_H */
