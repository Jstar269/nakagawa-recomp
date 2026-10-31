// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* The UMD drive: sceUmd. Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* Boot-path setup calls. These return success (or the value the reference run returns) so the boot
 * proceeds down the same branch. They do not yet model the subsystem behind them; the import
 * sequence vs the reference run is what validates that the returned value drives the same path. */
static uint32_t h_UmdCheckMedium(CpuState *s) { (void)s; return 1; }      /* medium present */
/* Registered entry for sceUmdRegisterUMDCallBack. When UMD becomes ready the kernel fires
 * this callback (a0=unknown, a1=drive_stat), which in this game calls sceKernelWakeupThread
 * on the launcher (0x111) to unstick it from sceKernelSleepThread. */
static uint32_t s_umd_cb_uid = 0;

/* Drive state sceUmdGetDriveStat reports. Measured on a PSP-3000, firmware 6.61, 2026-10-24,
 * with a game disc inserted: 0x32 at program entry (PSPSDK pspumd.h: PRESENT 0x02 | INITED 0x10 |
 * READY 0x20); sceUmdActivate, sceUmdWaitDriveStat for READY, sceUmdReplaceProhibit and
 * sceUmdReplacePermit each return 0; after sceUmdDeactivate the state is 0x12 (READY cleared).
 * The waits below read this state, so a wait for READY after a deactivate blocks until
 * sceUmdActivate signals the drive ready again. */
enum {
    UMD_STAT_PRESENT = 0x02u, UMD_STAT_INITED = 0x10u, UMD_STAT_READY = 0x20u,
    UMD_STAT_ACTIVE = 0x32u,       /* PRESENT | INITED | READY */
    UMD_STAT_DEACTIVATED = 0x12u   /* PRESENT | INITED */
};
static uint32_t s_umd_drive_stat = UMD_STAT_ACTIVE;
#ifdef SR_HLE_THREAD_SELFTEST
void sr_hle_test_umd_reset(void) { s_umd_drive_stat = UMD_STAT_ACTIVE; }
#endif

static uint32_t h_UmdDriveStat(CpuState *s) { (void)s; return s_umd_drive_stat; }

/* The virtual ISO-backed drive serves one disc that never fails, so sceUmdGetErrorStat
 * reports no drive error while that disc is present, activated or not. The error stat of a
 * deactivated drive is UNMEASURED; other drive error states remain outside this
 * compatibility contract. */
static uint32_t h_UmdGetErrorStat(CpuState *s) {
    uint32_t drive_stat = h_UmdDriveStat(s);
    return (drive_stat & UMD_STAT_PRESENT) ? 0u : 0x80010086u;
}

/* sceUmdReplaceProhibit() / sceUmdReplacePermit(): whether the user may swap the disc.
 * The runtime serves one ISO and never replaces it, so either call only records the
 * permission and returns 0, changed or not. Public behaviour reference: PPSSPP
 * Core/HLE/sceUmd.cpp. Firmware error cases (for example before sceUmdActivate) are
 * UNMEASURED; a PSP oracle case is listed for the maintainer. */
static uint32_t s_umd_replace_permitted = 0u;
static uint32_t h_UmdReplaceProhibit(CpuState *s) { (void)s; s_umd_replace_permitted = 0u; return 0; }
static uint32_t h_UmdReplacePermit(CpuState *s) { (void)s; s_umd_replace_permitted = 1u; return 0; }
#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_umd_replace_permitted(void) { return s_umd_replace_permitted; }
#endif

/* sceUmdGetDiscInfo(SceUmdDiscInfo *info): info->size (u32) must be 8, then info->type
 * (u32) receives PSP_UMD_TYPE_GAME (0x10), the only medium this runtime serves.
 * A size other than 8 or an unusable pointer returns SCE_ERROR_ERRNO_EINVAL
 * (0x80010016). Public behaviour reference: PSPSDK pspumd.h (SceUmdDiscInfo,
 * PSP_UMD_TYPE_GAME) and PPSSPP Core/HLE/sceUmd.cpp. Measured on a PSP-3000, firmware 6.61,
 * 2026-10-24, with a game disc: size 8 returns 0 and type 0x10; size 0 and size 12 return
 * 0x80010016. */
static uint32_t h_UmdGetDiscInfo(CpuState *s) {
    (void)s;
    uint32_t info = A0;
    if (!info || !sr_guest_span_writable(info, 8u)) return 0x80010016u;
    if (sr_r32(info) != 8u) return 0x80010016u;
    sr_w32(info + 4u, 0x10u);
    return 0;
}


/* Signal that the UMD drive is ready. Wake only threads waiting on the UMD object and
 * notify the registered callback if one exists. */
static void sr_umd_signal_ready(void) {
    s_umd_drive_stat = UMD_STAT_ACTIVE;
    sched_wake(0x554D44u);
    if (s_umd_cb_uid) {
        /* The callback argument follows PPSSPP's model (sceUmd.cpp __KernelUmdActivate), not
         * a measurement: a module that never set a compiled SDK version receives
         * PRESENT|READY (0x22), a newer one also INITED (0x32). A title's loader that waits
         * for exactly 0x22 needs the first form. */
        uint32_t callback_stat = UMD_STAT_PRESENT | UMD_STAT_READY;
        if (g_sdk_version != 0u) callback_stat |= UMD_STAT_INITED;
        (void)sr_callback_notify(s_umd_cb_uid, callback_stat);
    }
}

/* Shared body for sceUmdWaitDriveStat / ...WithTimer. A request whose bits all
 * fall inside the current drive state (see s_umd_drive_stat) is satisfied
 * immediately; otherwise the caller waits on the UMD object until sceUmdActivate
 * signals the drive ready or the timeout passes, and the state is read again:
 * still unsatisfied reports SCE_KERNEL_ERROR_WAIT_TIMEOUT. */
static uint32_t umd_wait_drive_stat(uint32_t want, uint32_t timeout_us) {
    if ((s_umd_drive_stat & want) == want) return 0;
    (void)sched_block_on_timeout(0x554D44u, timeout_us);
    return (s_umd_drive_stat & want) == want ? 0u : 0x800201A8u;
}

static uint32_t h_UmdWaitDriveStat(CpuState *s) {
    (void)s;
    return umd_wait_drive_stat(A0, 50000u);
}

/* sceUmdWaitDriveStatWithTimer(stat, timeout_us): same as the untimed wait but the
 * caller supplies the timeout in $a1. Previously this shared h_UmdWaitDriveStat and
 * silently ignored that argument. */
static uint32_t h_UmdWaitDriveStatWithTimer(CpuState *s) {
    (void)s;
    return umd_wait_drive_stat(A0, A1 ? A1 : 50000u);
}

static uint32_t h_UmdWaitDriveStatCB(CpuState *s) {
    uint32_t self = sched_current_uid();
    if (sr_thread_has_pending_callbacks(self))
        sr_thread_dispatch_callbacks();

    if ((s_umd_drive_stat & A0) == A0) return 0;

    uint32_t timeout = A1 ? A1 : 8000u;
    /* PSP hardware rounds very small UMD wait timeouts upward. */
    if (timeout <= 4u) timeout = 15u;
    else if (timeout <= 215u) timeout = 250u;

    sched_set_current_cb_wait(1);
    int timed_out = sched_block_on_timeout(0x554D44u, timeout);
    sched_set_current_cb_wait(0);

    if (sr_thread_has_pending_callbacks(self))
        sr_thread_dispatch_callbacks();

    return timed_out ? 0x800201A8u : 0u;
}

/* sceUmdActivate: real PSP spins up the laser motor; do a small blocking delay before reporting
 * success so the caller sees a settle (matches "Active" transition in the real driver). After
 * the settle, fire the UMD-ready signal so any waiting thread and the registered callback fire. */
static uint32_t h_UmdActivate(CpuState *s) {
    (void)s;
    /* 30 ms virtual delay keeps the loader's stat-read interleaving realistic. */
    sched_delay_current(30000);
    sr_umd_signal_ready();
    return 0;
}

/* sceUmdDeactivate(unit, drive): stop the drive. The measured state afterwards is 0x12 (see
 * s_umd_drive_stat); the return value of a deactivate is UNMEASURED and reported as 0, like
 * sceUmdActivate. The arguments are not checked, as sceUmdActivate does not check them. */
static uint32_t h_UmdDeactivate(CpuState *s) {
    (void)s;
    s_umd_drive_stat = UMD_STAT_DEACTIVATED;
    /* The callback receives the deactivated state 0x12, as PPSSPP's __KernelUmdDeactivate
     * notifies (its PSP_UMD_READY is this file's INITED bit, 0x10). The callback argument
     * itself is not measured; the drive state is. */
    if (s_umd_cb_uid)
        (void)sr_callback_notify(s_umd_cb_uid, s_umd_drive_stat);
    return 0;
}

/* sceUmdRegisterUMDCallBack: register a callback that fires when UMD drive state changes.
 * Registration only records the callback; it does not signal an event or alter any
 * thread's pending wakeup count. */
static uint32_t h_UmdRegisterUMDCallBack(CpuState *s) {
    if (!sr_callback_is_valid(A0))
        return 0x80010016u;
    s_umd_cb_uid = A0;
    if (getenv("SR_CBLOG"))
        fprintf(stderr, "CBLOG: UMD callback registered uid=0x%x\n", s_umd_cb_uid);
    return 0;
}

/* PSPSDK pspumd.h documents only a negative result for an unknown callback ID.
 * Match the kernel callback delete path's UNKNOWN_UID result until hardware
 * measurements establish sceUmdUnRegisterUMDCallBack's exact error code. */
static uint32_t h_UmdUnRegisterUMDCallBack(CpuState *s) {
    (void)s;
    if (!s_umd_cb_uid || s_umd_cb_uid != A0) return 0x800201a1u;
    s_umd_cb_uid = 0u;
    return 0u;
}

void hle_register_umd_callback_handlers(void) {
    sr_hle_register(0xc6183d47u, "sceUmdActivate", h_UmdActivate);
    sr_hle_register(0xe83742bau, "sceUmdDeactivate", h_UmdDeactivate);
    sr_hle_register(0x6b4a146cu, "sceUmdGetDriveStat", h_UmdDriveStat);
    sr_hle_register(0xaee7404du, "sceUmdRegisterUMDCallBack", h_UmdRegisterUMDCallBack);
    sr_hle_register(0xbd2bde07u, "sceUmdUnRegisterUMDCallBack", h_UmdUnRegisterUMDCallBack);
    sr_hle_register(0x87533940u, "sceUmdReplaceProhibit", h_UmdReplaceProhibit);
    sr_hle_register(0xcbe9f02au, "sceUmdReplacePermit", h_UmdReplacePermit);
    sr_hle_register(0x340b7686u, "sceUmdGetDiscInfo", h_UmdGetDiscInfo);
    /* Shared with the executable harness so it dispatches the production mapping (#341). */
    sr_hle_register(0x46ebb729u, "sceUmdCheckMedium", h_UmdCheckMedium);
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_umd(void) {
    sr_hle_register(0x8ef08fce, "sceUmdWaitDriveStat", h_UmdWaitDriveStat);
    sr_hle_register(0x56202973, "sceUmdWaitDriveStatWithTimer", h_UmdWaitDriveStatWithTimer);
    sr_hle_register(0x4a9e5e29, "sceUmdWaitDriveStatCB", h_UmdWaitDriveStatCB);
}

#ifndef SR_MPEG_MEDIA_SELFTEST
#ifndef SR_HLE_THREAD_SELFTEST
/* From sr_hle_init(). */
void hle_register_init_umd(void) {
    sr_hle_register_unsupported(0x6af9b50a, "sceUmdCancelWaitDriveStat", 0x80010086u); /* SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED */
    sr_hle_register(0x20628e6f, "sceUmdGetErrorStat", h_UmdGetErrorStat);
}
#endif
#endif
