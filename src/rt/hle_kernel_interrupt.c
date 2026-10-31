// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Interrupts: CPU interrupt suspend and resume, and VBLANK sub-interrupt handlers. Extracted
 * from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

static int hle_debug_thread_log_on(void) {
    static int enabled = -1;
    return hle_env_switch(&enabled, "SR_DEBUG_THREAD_LOG");
}

/* Kernel_Library: interrupt suspend/resume. Suspend returns the prior state (use 1); resume
 * is void. The game uses a lock counter at 0x30aa80 (nesting depth) and 0x30aa84 (saved state).
 * FUN_00011578 (AllocLock) calls SuspendIntr and increments 0x30aa80; on first nest, saves
 * the prior interrupt state to 0x30aa84. FUN_000115b4 (AllocUnlock) decrements 0x30aa80 and
 * on reaching zero, calls ResumeIntr. The spinlock at 0x10974 polls 0x30aa80 via bne. */
static uint32_t h_CpuSuspendIntr(CpuState *s) {
    (void)s;
    if (hle_syslog_on()) {
        fprintf(stderr, "HLE: sceKernelCpuSuspendIntr called\n");
    }
    if (hle_debug_thread_log_on()) {
        /* Role-resolved rather than UID-literal (the historical literals drifted and
         * left these diagnostics permanently silent). A build with no worker/launcher
         * binding has no role to report, so both branches stay quiet. */
        if (sched_current_is_worker() && sr_title_config_diagnostics_enabled()) {
            fprintf(stderr, "DEBUG: CpuSuspendIntr worker=0x%x: r16 (s0)=0x%x, "
                    "r26 (k0)=0x%x, k0+4=0x%x, ra=0x%x, sp=0x%x\n  Stack:",
                    sched_current_uid(), s->r[16], s->r[26],
                    s->r[26] ? MEM_R32(s->r[26] + 4) : 0, s->r[31], s->r[29]);
            for (int i = 0; i < 20; i++)
                fprintf(stderr, " +%d:0x%x", i * 4, MEM_R32(s->r[29] + i * 4));
            fprintf(stderr, "\n");
        }
        if (sched_current_is_launcher() && sr_title_config_diagnostics_enabled()) {
            fprintf(stderr, "DEBUG: CpuSuspendIntr launcher=0x%x: r16 (s0)=0x%x, r17 (s1)=0x%x, r26 (k0)=0x%x, ra=0x%x\n",
                    sched_current_uid(), s->r[16], s->r[17], s->r[26], s->r[31]);
        }
    }
    return sched_suspend_interrupts();
}

static uint32_t h_CpuResumeIntr(CpuState *s) {
    if (hle_syslog_on()) {
        fprintf(stderr, "HLE: sceKernelCpuResumeIntr called\n");
    }
    if (hle_debug_thread_log_on()) {
        /* Role-resolved (see h_CpuSuspendIntr). */
        if (sched_current_is_worker()) {
            fprintf(stderr, "DEBUG: CpuResumeIntr worker=0x%x: r2 (v0)=0x%x, r6 (a2)=0x%x, r16 (s0)=0x%x, r26 (k0)=0x%x, ra=0x%x, sp=0x%x\n  Stack:",
                    sched_current_uid(), s->r[2], s->r[6], s->r[16], s->r[26], s->r[31], s->r[29]);
            for (int i = 0; i < 20; i++) {
                fprintf(stderr, " +%d:0x%x", i * 4, MEM_R32(s->r[29] + i * 4));
            }
            fprintf(stderr, "\n");
        }
        if (sched_current_is_launcher() && sr_title_config_diagnostics_enabled()) {
            fprintf(stderr, "DEBUG: CpuResumeIntr launcher=0x%x: r2 (v0)=0x%x, "
                    "r16 (s0)=0x%x, r17 (s1)=0x%x, r26 (k0)=0x%x, ra=0x%x\n",
                    sched_current_uid(), s->r[2], s->r[16], s->r[17], s->r[26], s->r[31]);
        }
    }
    sched_resume_interrupts(A0);
    /* sched_resume_interrupts performs the eligible pending-source delivery
     * and the normal post-interrupt priority check. */
    return 0;
}
static uint32_t h_CpuResumeIntrWithSync(CpuState *s) {
    return h_CpuResumeIntr(s);
}
/* sceKernelIsCpuIntrSuspended(flag) asks whether the SUPPLIED saved-state token
 * came from an already-suspended CPU.  It is a pure predicate on the argument and
 * must not consult the live interrupt state: PSPAutotests tests/intr/suspended.expected
 * prints the same four results with interrupts enabled and with them suspended
 * (0 -> 1, 1 -> 0, 2 -> 0, 0xDEADBEEF -> 0), and the real-PSP capture on issue #88
 * (PSP-3001 / 6.61-ARK) reports those same values.  A token of 0 is what
 * sceKernelCpuSuspendIntr returns when interrupts were ALREADY off, so 0 -> "was
 * suspended" -> 1; any other token means they had been enabled. */
static uint32_t h_CpuIsIntrSuspended(CpuState *s) {
    return A0 == 0u ? 1u : 0u;
}
static uint32_t h_CpuIsIntrEnable(CpuState *s) {
    (void)s;
    return sched_interrupts_enabled() ? 1u : 0u;
}

/* VBLANK (PSP_VBLANK_INT = 30) sub-interrupt handlers the game registers; the scheduler
 * delivers every enabled one once per frame (they typically wake a sleeping game thread).
 * The interrupt has its own handler, arg and enable state per sub-interrupt number 0..15
 * (PSPSDK pspintrman.h). A title may use several at once: a game registers its frame
 * counter on sub-interrupt 0 and its video player another on 1, then releases 1 when the
 * video closes; with a single shared slot that registration replaced the game's handler
 * and the release removed it, so the frame counter stopped and the game waited for it
 * forever. A slot is registered while its handler word is nonzero: the PSP does not count
 * a NULL-handler Register as a registration (see the codes below) (#1287). */
#define SR_VBLANK_SUBINTR_COUNT 16u
/* PSPSDK pspkerror.h names for the interrupt manager's refusals. */
#define SCE_KERNEL_ERROR_ILLEGAL_INTRCODE 0x80020065u
#define SCE_KERNEL_ERROR_FOUND_HANDLER    0x80020067u
#define SCE_KERNEL_ERROR_NOTFOUND_HANDLER 0x80020068u
typedef struct VblankSubIntr {
    uint32_t handler, arg;
    int enabled;
} VblankSubIntr;
static VblankSubIntr g_vbl[SR_VBLANK_SUBINTR_COUNT];

/* Sub-interrupt calls on interrupt lines other than VBLANK (30), measured on a PSP by
 * pspautotests tests/intr/registersub.expected and releasesub.expected at
 * f573fe8c9122a8ff5db4f030d74557bed644b4e0 (one Register, then one Release, for sub-interrupt
 * 0 of every line from -2 to 69). Line 25 (the GE interrupt) also accepts both; every other
 * line refuses with ILLEGAL_INTRCODE or NOTFOUND_HANDLER, and the two calls disagree on lines
 * 4, 5, 6 and 21. Bit n of a mask is set when line n answers NOTFOUND_HANDLER; a clear bit
 * below SUBINTR_LINE_COUNT is ILLEGAL_INTRCODE. Lines from 67 up (67, 68 and 69 measured)
 * and negative lines answer ILLEGAL_INTRCODE: the PSP has interrupt lines 0 to 66. */
#define SUBINTR_LINE_COUNT 67u
#define SUBINTR_LINE_GE 25u
static const uint64_t s_subintr_register_notfound_lines =
    0x0000000000006b2full |      /* 0-3, 5, 8, 9, 11, 13, 14 */
    (0x7ull << 27) |             /* 27-29 */
    (0xfull << 32) |             /* 32-35 */
    (0x1fffull << 37) |          /* 37-49 */
    (0x1full << 51) |            /* 51-55 */
    (0x3ull << 62);              /* 62, 63 */
static const uint64_t s_subintr_release_notfound_lines =
    0x0000000000006b7full |      /* 0-6, 8, 9, 11, 13, 14 */
    (1ull << 21) | (0x7ull << 27) | (0xfull << 32) | (0x1fffull << 37) | (0x1full << 51) |
    (0x3ull << 62);
/* Lines 64 and 66 are past the 64-bit masks; both answer NOTFOUND_HANDLER to both calls,
 * and line 65 answers ILLEGAL_INTRCODE. */
static uint32_t subintr_other_line_result(uint32_t intno, uint64_t notfound_lines) {
    if (intno >= SUBINTR_LINE_COUNT) return SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
    if (intno >= 64u)
        return intno == 65u ? SCE_KERNEL_ERROR_ILLEGAL_INTRCODE : SCE_KERNEL_ERROR_NOTFOUND_HANDLER;
    return (notfound_lines >> intno) & 1u ? SCE_KERNEL_ERROR_NOTFOUND_HANDLER
                                          : SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
}
/* VBLANK sub-interrupt numbers 16 to 31 are the system's. A user Register answers
 * FOUND_HANDLER on 18-20 and 24-26 and ILLEGAL_INTRCODE on the rest, a Release answers
 * NOTFOUND_HANDLER on all of them, and numbers from 32 up answer ILLEGAL_INTRCODE to both
 * (pspautotests registersub/releasesub at f573fe8c, sub-interrupts -2 to 69 of line 30). */
#define VBLANK_SUBINTR_SYSTEM_END 32u
static int vblank_subintr_system_held(uint32_t no) {
    return (no >= 18u && no <= 20u) || (no >= 24u && no <= 26u);
}

static uint32_t h_RegisterSubIntr(CpuState *s) {
    /* a0=intno, a1=no, a2=handler, a3=arg. The handler is later called as handler(no, arg)
     * (PSPSDK pspintrman.h and its vsync sample: `void on_vblank(int sub, void *data)`), so
     * the sub-interrupt number is kept with the handler and its arg. */
    if (A0 == SUBINTR_LINE_GE)
        return hle_unmodelled_stop("sceKernelRegisterSubIntrHandler",
                                   "on the GE interrupt (line 25, sub-interrupt %u) is not "
                                   "delivered by this runtime", A1);
    if (A0 != 30u) return subintr_other_line_result(A0, s_subintr_register_notfound_lines);
    if (A1 >= SR_VBLANK_SUBINTR_COUNT) {
        return A1 < VBLANK_SUBINTR_SYSTEM_END && vblank_subintr_system_held(A1)
                   ? SCE_KERNEL_ERROR_FOUND_HANDLER : SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
    }
    VblankSubIntr *v = &g_vbl[A1];
    /* Measured on a PSP (pspautotests registersub, "handlers"): a NULL handler answers 0 and
     * registers nothing, so a later non-NULL Register still succeeds; a Register over a
     * non-NULL handler answers FOUND_HANDLER and keeps the first one, whatever the new handler
     * is. Titles re-register the same handler before almost every frame (#1287); the slot is
     * then unchanged, so the line below is written only when a Register takes the slot, or
     * always under SR_HLELOG. Register leaves the enable state alone: Enable before Register
     * delivers the handler once it is registered (enablesub "With handler after"). */
    if (v->handler) {
        if (hle_log_on())
            fprintf(stderr, "HLE: VBLANK sub-interrupt %u already holds 0x%08x\n", A1, v->handler);
        return SCE_KERNEL_ERROR_FOUND_HANDLER;
    }
    if (!A2) return 0;
    fprintf(stderr, "HLE: registering VBLANK handler 0x%08x no %u arg 0x%08x\n", A2, A1, A3);
    v->handler = A2; v->arg = A3;
    return 0;
}
/* Enable/Disable on a line other than VBLANK: line 70 answers ILLEGAL_INTRCODE (enablesub at
 * f573fe8c), as every line past 66 does for Register and Release. The lines 0 to 66 other than
 * 30 have no delivery state here and their answers are not measured, so the run stops by name. */
static uint32_t subintr_toggle_other_line(const char *api, uint32_t intno, uint32_t no) {
    if (intno >= SUBINTR_LINE_COUNT) return SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
    return hle_unmodelled_stop(api, "on interrupt line %u (sub-interrupt %u) is not measured",
                               intno, no);
}
/* The VBLANK sub-interrupt numbers Enable and Disable accept: 0 to 15. Number 70 answers
 * ILLEGAL_INTRCODE (measured); the system's numbers 16 to 31 are not measured. */
static uint32_t vblank_subintr_toggle_check(const char *api, uint32_t no) {
    if (no < SR_VBLANK_SUBINTR_COUNT) return 0u;
    if (no >= VBLANK_SUBINTR_SYSTEM_END) return SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
    return hle_unmodelled_stop(api, "on VBLANK sub-interrupt %u (a system number) is not "
                               "measured", no);
}
/* sceKernelEnableSubIntr(intno, no) and sceKernelDisableSubIntr(intno, no): start or stop
 * delivery of one VBLANK (intno 30) sub-interrupt without unregistering its handler, so a
 * later Enable resumes the same handler. Measured on a PSP (pspautotests enablesub at
 * f573fe8c): both answer 0 for a registered number, a second time, and for a number with no
 * handler, and Enable on a number with no handler delivers nothing until one is registered. */
static uint32_t h_EnableSubIntr(CpuState *s) {
    if (A0 != 30u) return subintr_toggle_other_line("sceKernelEnableSubIntr", A0, A1);
    uint32_t rc = vblank_subintr_toggle_check("sceKernelEnableSubIntr", A1);
    if (rc != 0u) return rc;
    g_vbl[A1].enabled = 1;
    return 0;
}
static uint32_t h_DisableSubIntr(CpuState *s) {
    if (A0 != 30u) return subintr_toggle_other_line("sceKernelDisableSubIntr", A0, A1);
    uint32_t rc = vblank_subintr_toggle_check("sceKernelDisableSubIntr", A1);
    if (rc != 0u) return rc;
    g_vbl[A1].enabled = 0;
    return 0;
}
/* sceKernelReleaseSubIntrHandler(intno, no): unregister one VBLANK sub-interrupt handler and
 * stop its delivery; the others keep running. Measured on a PSP (pspautotests releasesub and
 * enablesub at f573fe8c): a registered number answers 0, a number with no handler (never
 * registered, released already, or registered with a NULL handler) answers NOTFOUND_HANDLER,
 * and Release clears the enable state too, so a handler registered again after a Release of
 * an enabled number is not delivered until a fresh Enable ("Released while enabled"). */
static uint32_t h_ReleaseSubIntr(CpuState *s) {
    if (A0 == SUBINTR_LINE_GE)
        return hle_unmodelled_stop("sceKernelReleaseSubIntrHandler",
                                   "on the GE interrupt (line 25, sub-interrupt %u) is not "
                                   "delivered by this runtime", A1);
    if (A0 != 30u) return subintr_other_line_result(A0, s_subintr_release_notfound_lines);
    if (A1 >= SR_VBLANK_SUBINTR_COUNT)
        return A1 < VBLANK_SUBINTR_SYSTEM_END ? SCE_KERNEL_ERROR_NOTFOUND_HANDLER
                                              : SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
    if (!g_vbl[A1].handler) return SCE_KERNEL_ERROR_NOTFOUND_HANDLER;
    memset(&g_vbl[A1], 0, sizeof(g_vbl[A1]));
    return 0;
}
/* The scheduler's view of VBLANK sub-interrupt `no`: 1 with its handler and arg when it is
 * registered and enabled, 0 when it is not, -1 past the last sub-interrupt number. */
int sr_vblank_subintr(uint32_t no, uint32_t *handler, uint32_t *arg) {
    if (no >= SR_VBLANK_SUBINTR_COUNT) return -1;
    if (!g_vbl[no].enabled || !g_vbl[no].handler) return 0;
    *handler = g_vbl[no].handler;
    *arg = g_vbl[no].arg;
    return 1;
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_time_handlers(). */
void hle_register_time_kernel_interrupt(void) {
    sr_hle_register(0x092968f4, "sceKernelCpuSuspendIntr", h_CpuSuspendIntr);
    sr_hle_register(0x5f10d406, "sceKernelCpuResumeIntr", h_CpuResumeIntr);
    sr_hle_register(0x3b84732d, "sceKernelCpuResumeIntrWithSync", h_CpuResumeIntrWithSync);
    sr_hle_register(0x47a0b729, "sceKernelIsCpuIntrSuspended", h_CpuIsIntrSuspended);
    sr_hle_register(0xb55249d2, "sceKernelIsCpuIntrEnable", h_CpuIsIntrEnable);
}

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_kernel_interrupt(void) {
    sr_hle_register(0xca04a2b9, "sceKernelRegisterSubIntrHandler", h_RegisterSubIntr);
    sr_hle_register(0xfb8e22ec, "sceKernelEnableSubIntr", h_EnableSubIntr);
    /* TD-24 batch 2 lifecycle pair, MOVED verbatim out of sr_hle_init()'s
     * production branch like the lines above: one definition reached by both
     * builds, so the executable harness pins the production mapping and no
     * handler behavior changes on either side. */
    sr_hle_register(0xd61e6961, "sceKernelReleaseSubIntrHandler", h_ReleaseSubIntr);
    sr_hle_register(0x8a389411, "sceKernelDisableSubIntr", h_DisableSubIntr);
}
