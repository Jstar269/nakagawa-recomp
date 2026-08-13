// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the psp-recomp authors

/*
 * Interrupt/dispatch-context conformance harness for issue #88 (verification
 * infrastructure only -- this file changes no HLE handler behavior).
 *
 * ---------------------------------------------------------------------------
 * Evidence
 * ---------------------------------------------------------------------------
 * PSPAutotests `tests/intr/waits.cpp` + `waits.expected` is a hardware-generated
 * matrix: for ~50 blocking/polling APIs it records the exact return value with
 * (a) CPU interrupts disabled, (b) thread dispatch disabled, and (c) the call made
 * from inside a real VBLANK sub-interrupt handler. Every `hw[]` value in the table
 * below is transcribed from that file, and each cell carries the exact line number
 * it came from so the transcription is checkable. `tools/test_intr_waits_matrix.py`
 * re-verifies every value against the checkout when `third_party/ppsspp-src` is
 * present (that tree is Git-ignored, so the check SKIPs without it).
 *
 * The matrix does NOT support a universal pre-handler context gate, which is why
 * this harness deliberately does not introduce one:
 *
 *   - Most cells return CAN_NOT_WAIT (0x800201a7) when interrupts or dispatch are
 *     disabled, and ILLEGAL_CONTEXT (0x80020064) from interrupt context.
 *   - But some APIs validate a PARAMETER first and never reach the gate:
 *     sceKernelWaitEventFlag with mode 0xFF returns ILLEGAL_MODE (L72/L73),
 *     sceCtrlReadBufferPositive with count 256 returns INVALID_SIZE (L220/L221),
 *     sceUmdWaitDriveStat with type 0 returns ERRNO_INVALID_ARGUMENT (L296/L297).
 *   - Some validate the OBJECT first: sceKernelWaitThreadEnd(0) returns
 *     ILLEGAL_THID (L204/L205), every sceIo* bad-fd cell returns BADF (L258...).
 *   - And precedence is itself CONTEXT-DEPENDENT: sceKernelWaitEventFlag mode 0xFF
 *     returns ILLEGAL_MODE with interrupts disabled (L72) but ILLEGAL_CONTEXT from
 *     interrupt context (L338). A single flag on a registry entry cannot express
 *     that; each handler owns its own ordering.
 *
 * ---------------------------------------------------------------------------
 * Why a header
 * ---------------------------------------------------------------------------
 * This is included once by hle_thread_selftest.c, which already links production
 * hle.c, includes production sched.c, and enters handlers through sr_syscall's
 * registered-NID lookup. Reusing that target rather than adding a new one is
 * deliberate: the Makefile comment above RT_SRCS records that hand-copied flag
 * lists are exactly how selftest recipes have silently diverged before.
 *
 * ---------------------------------------------------------------------------
 * Contexts
 * ---------------------------------------------------------------------------
 *   IC_NORMAL   -- ordinary call on a fixture thread. waits.expected has no
 *                  normal-context column, so this is a CONTROL only: it is
 *                  executed and reported, never compared against hardware.
 *   IC_INTR_OFF -- sceKernelCpuSuspendIntr held across the call, entered through
 *                  the production NID. Compared against the "interrupts disabled"
 *                  column.
 *   IC_ICTX     -- the call is made from inside a real VBLANK sub-interrupt
 *                  handler: sceKernelRegisterSubIntrHandler(30, ...) +
 *                  sceKernelEnableSubIntr(30) through the production NIDs, then
 *                  sched_raise_interrupt(SCHED_INTR_VBLANK) and a production
 *                  sceKernelCpuResumeIntr, which drives scheduler_service_pending
 *                  -> deliver_vblank -> dispatch(). That is the same registration
 *                  path waits.cpp uses. Compared against the "Inside interrupt"
 *                  column.
 *
 *   IC_DISP_OFF -- sceKernelSuspendDispatchThread (0x3ad58b8c) held across
 *                  the call, entered through production NID lookup and restored
 *                  via sceKernelResumeDispatchThread (0x27e22ec2) with scheduler
 *                  dispatch-suspension state s_dispatch_enabled. Compared
 *                  against the "Dispatch disabled" column.
 *
 * ---------------------------------------------------------------------------
 * How "would block" and known failures are represented
 * ---------------------------------------------------------------------------
 * On hardware every cell here returns immediately. On current main no handler
 * consults interrupt or dispatch state at all, so a probe frequently enters a
 * real wait instead of returning. A probe therefore runs on its own coroutine
 * standing in for a guest thread; if it does not come back, its TCB is parked in
 * TH_WAIT_* and the outcome is recorded as IC_BLOCKED -- a first-class outcome,
 * not a hang and not a silent pass.
 *
 * Each cell records both the hardware value (hw[]) and the exact value current
 * main produces (base[]). The runner classifies:
 *
 *   actual == hw                      -> CONFORMS
 *   actual == base != hw              -> KNOWN DEVIATION (reported, not a failure)
 *   actual == hw but base != hw       -> FAILURE "promote the baseline"
 *   anything else                     -> FAILURE "regression"
 *
 * A baseline is one exact value, never a wildcard. A future regression produces a
 * third value and fails; a future fix makes actual == hw and fails until the
 * baseline is deliberately promoted to hw in this table. Known failures can
 * therefore never rot into silent passes in either direction.
 *
 * ---------------------------------------------------------------------------
 * Bounded interrupt-context execution
 * ---------------------------------------------------------------------------
 * In interrupt context s_cur is -1, and every scheduler blocking primitive
 * (sched_block_on, sched_block_on_timeout, sched_delay_current, sched_thread_sleep)
 * returns immediately when s_cur < 0. A handler that LOOPS on an unsatisfied wait
 * condition -- h_WaitSema, h_WaitEventFlag, h_WaitThreadEnd, lwmutex_acquire --
 * therefore spins forever rather than blocking. That is a real defect this pass
 * must not paper over, and it is also unobservable from inside the process.
 *
 * The runner bounds it with a measured gate rather than a guess: IC_ICTX is
 * executed for a probe only when that probe's IC_NORMAL leg RETURNED. If the
 * normal-context leg blocked, this probe's wait condition is unsatisfiable, so the
 * interrupt-context call would spin; the cell is recorded IC_NOTRUN with reason
 * "spin-unbounded" instead. The gate is derived at run time from the measurement,
 * so it re-opens by itself once the wait stops being entered. It is conservative
 * (h_DelayThread is gated even though sched_delay_current early-returns), and being
 * conservative here costs coverage, never correctness.
 *
 * The gate reads the NORMAL leg, not the interrupts-disabled leg. It originally read
 * the latter, which was equivalent only while no handler consulted interrupt state:
 * once CAN_NOT_WAIT landed, that leg returned for a reason that does not hold inside
 * an interrupt handler. ic_run_in_interrupt() reaches the handler through a
 * production sceKernelCpuResumeIntr, which restores the prior ENABLED interrupt state
 * before driving scheduler_service_pending(), and never touches dispatch state -- so
 * sched_wait_permitted() is true there and the wait loops are entered exactly as
 * before. Gating on the intr-off leg would therefore have opened the gate for the six
 * looping probes (WaitSema/CB, WaitEventFlag/CB, WaitThreadEnd/CB) and hung the suite.
 * The normal leg measures the underlying condition instead, and is not affected by
 * any context semantics. Retiring these cells needs the interrupt-context work.
 */

#ifndef SR_INTR_CONFORMANCE_H
#define SR_INTR_CONFORMANCE_H

/* Registry membership probe exported by hle.c for this build only. Needed because
 * sr_syscall() calls _Exit(7) on an unregistered NID under the fiber scheduler:
 * an out-of-scope cell has to be detected without being called. */
extern int sr_hle_test_is_registered(uint32_t nid);

/* ---- outcome encoding ---------------------------------------------------- */
/* A 32-bit guest return value is stored verbatim; the sentinels live above it so
 * no real return can ever collide with one. */
#define IC_RET(x)     ((uint64_t)(uint32_t)(x))
#define IC_BLOCKED    0x100000000ull   /* probe entered a real wait and did not return */
#define IC_UNKNOWN    0x200000000ull   /* not covered by waits.expected -- never asserted */
#define IC_NOTRUN     0x300000000ull   /* deliberately not executed; reason recorded */
#define IC_SETUPFAIL  0x400000000ull   /* fixture arrangement failed -- harness limitation */

/* ---- contexts ------------------------------------------------------------ */
enum { IC_NORMAL = 0, IC_INTR_OFF = 1, IC_ICTX = 2, IC_DISP_OFF = 3, IC_NCTX = 4 };

static const char *const kIcCtxName[IC_NCTX] = {
    "normal", "interrupts-disabled", "interrupt-context", "dispatch-disabled"
};

/* ---- error-precedence classification (evidence-derived, per context) ------ */
enum {
    IC_PREC_NA = 0,      /* the call is valid; no competing error to order */
    IC_PREC_CONTEXT,     /* the context restriction wins */
    IC_PREC_PARAM,       /* a parameter error is returned instead of the context error */
    IC_PREC_OBJECT,      /* an object-identity error is returned instead */
    IC_PREC_UNKNOWN
};
static const char *const kIcPrecName[] = {
    "n/a", "context-first", "param-first", "object-first", "unknown"
};

/* ---- blocking classification on hardware --------------------------------- */
enum { IC_HW_IMMEDIATE = 0, IC_HW_WOULD_BLOCK = 1, IC_HW_POLL = 2 };

/* ---- probe groups: each owns its own arrangement ------------------------- */
typedef enum {
    ICG_NONE = 0,   /* no object, no arguments */
    ICG_SEMA,
    ICG_EVF,
    ICG_FPL,
    ICG_MUTEX,
    ICG_LWMUTEX,
    ICG_THREAD,
    ICG_VOLATILE,
    ICG_UMD,
    ICG_CTRL,
    ICG_IO_BADFD
} IcGroup;

/* Source-owned synthetic guest addresses. Chosen clear of the fixtures already in
 * this file (0x00200000 time-domain block) and of the scheduler's guest counters
 * (0x0031101c / 0x0031105c / 0x00331b80). Nothing here comes from the title. */
enum {
    IC_SCRATCH  = 0x00240000u,  /* generic out-parameter word */
    IC_LWWORK   = 0x00240100u,  /* lightweight-mutex work area */
    IC_PADBUF   = 0x00240400u,  /* SceCtrlData[64] = 1024 bytes */
    IC_NAMEBUF  = 0x00240900u,  /* NUL object name */
    IC_IOBUF    = 0x00240a00u
};

/* Guest handler entry the VBLANK sub-interrupt registration points at. The
 * selftest's dispatch() stub stands in for the generated translation of that
 * entry, exactly as it already does for callback entries. */
#define IC_INTR_ENTRY 0x0800c088u

#define NID_IC_CPU_SUSPEND_INTR   0x092968f4u
#define NID_IC_CPU_RESUME_INTR    0x5f10d406u
#define NID_IC_SUSPEND_DISPATCH_THREAD 0x3ad58b8cu
#define NID_IC_RESUME_DISPATCH_THREAD  0x27e22ec2u
#define NID_IC_REGISTER_SUBINTR   0xca04a2b9u
#define NID_IC_ENABLE_SUBINTR     0xfb8e22ecu
#define NID_IC_CREATE_SEMA        0xd6da4ba1u
#define NID_IC_CREATE_EVF         0x55c20a00u
#define NID_IC_SET_EVF            0x1fb15a32u
#define NID_IC_CREATE_FPL         0xc07bb470u
#define NID_IC_CREATE_MUTEX       0xb7d098c6u
#define NID_IC_CREATE_LWMUTEX     0x19cff145u
#define NID_IC_VOLATILE_MEM_LOCK  0x3e0271d3u

#define SCE_KERNEL_ERROR_CAN_NOT_WAIT_HW     0x800201a7u
#define SCE_KERNEL_ERROR_ILLEGAL_CONTEXT_HW  0x80020064u

/* ---- a single matrix cell ------------------------------------------------ */
typedef struct {
    const char *api;        /* PSP API name as called by waits.cpp */
    uint32_t    nid;        /* the registered NID this probe actually dispatches */
    const char *scenario;   /* waits.cpp INTR_DISPATCH_TITLE text, or "" */
    IcGroup     group;
    uint32_t    variant;    /* scenario selector inside the group */

    /* waits.expected line numbers (1-based); 0 == this context is not covered. */
    uint16_t    ev_line[IC_NCTX];
    /* Hardware truth per context. IC_UNKNOWN where waits.expected has no cell. */
    uint64_t    hw[IC_NCTX];
    /* Exact value current main produces. Promote to hw when the semantics land. */
    uint64_t    base[IC_NCTX];

    uint8_t     prec[IC_NCTX];  /* evidence-derived error precedence */
    uint8_t     hw_block;       /* IC_HW_* -- what hardware does absent the gate */

    /* Non-NULL means this probe is deliberately not executed, for a reason that
     * is a property of the harness rather than of the runtime. Trailing field so
     * rows that do execute say nothing about it. */
    const char *skip;
} IcProbe;

/* -------------------------------------------------------------------------
 * The matrix. Every hw[] / hw_dispatch value is transcribed from
 * third_party/ppsspp-src/pspautotests/tests/intr/waits.expected at the cited
 * line. base[] values are MEASURED against this branch's base commit; none of
 * them is a guess, and a wrong one fails the run rather than passing it.
 *
 * 28 base[] entries have been promoted from IC_BLOCKED to CNW, deliberately and
 * only after the run named each one with [PROMOTE BASELINE]: the 24 cells of the
 * genuinely-blocking CAN_NOT_WAIT work (12 probes x intr-off/disp-off), plus the
 * 4 sceKernelWaitSema/CB "Invalid count" cells.
 *
 * Issue #43 promoted 4 more -- sceKernelWaitSema/CB "Bad sema", intr-off and
 * disp-off -- and re-pinned 8 non-hardware cells in the same group. Those last 4
 * "Invalid count" promotions were originally reached by accident: the runtime had
 * no signal-vs-maxCount validation, so the call was merely an unsatisfiable wait
 * that arrived at the same context check as "Valid sema". It now has that
 * validation and the context check now precedes it, so the same cells are reached
 * by the route hardware uses. Same value, different path -- the point of #43.
 * ------------------------------------------------------------------------- */

#define ICB   IC_BLOCKED
#define ICU   IC_UNKNOWN
#define CNW   IC_RET(SCE_KERNEL_ERROR_CAN_NOT_WAIT_HW)
#define ILCTX IC_RET(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT_HW)

static const IcProbe kIcMatrix[] = {
/* ---- group A: no object, no arguments ---------------------------------- */
{ "sceKernelDelayThread", 0xceadeb47u, "", ICG_NONE, 0,
  {0, 2, 317, 3}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceKernelDelayThreadCB", 0x68da9e36u, "", ICG_NONE, 0,
  {0, 6, 318, 7}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceKernelSleepThread", 0x9ace131eu, "", ICG_NONE, 0,
  {0, 18, 321, 19}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceKernelSleepThreadCB", 0x82826f70u, "", ICG_NONE, 0,
  {0, 22, 322, 23}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
/* sceDisplayWaitVblank is the one API in the whole matrix that SUCCEEDS from
 * interrupt context (00000001, L323) instead of returning ILLEGAL_CONTEXT. */
{ "sceDisplayWaitVblank", 0x36cdfadeu, "", ICG_NONE, 0,
  {0, 26, 323, 27}, {ICU, CNW, IC_RET(1), CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_NA, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceDisplayWaitVblankStart", 0x984c27e7u, "", ICG_NONE, 0,
  {0, 34, 325, 35}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},

/* ---- group B: semaphore ------------------------------------------------- */
/* Issue #43 promoted the four "Bad sema" intr-off/disp-off cells to hw: the
 * context decision now runs ahead of the object lookup, which is what L54/L55
 * and L62/L63 measure. The other base[] moves in this group are consequences of
 * the same change, and none of them is a hardware claim:
 *   - normal "Bad sema" is now UNKNOWN_SEMID rather than the 0x80020000 seam,
 *     which is hardware-measured, but by wait.expected L21/L23 -- waits.expected
 *     has no normal column, so this stays a CONTROL here;
 *   - intr-ctx "Bad sema" is UNKNOWN_SEMID for the same reason and remains a
 *     known deviation from ILLEGAL_CONTEXT, which is PR-D's cell to fix;
 *   - normal "Invalid count" was IC_BLOCKED and is now ILLEGAL_COUNT (control);
 *   - intr-ctx "Invalid count" was NOT RUN under the spin-unbounded gate. The
 *     gate reads the normal-context leg, that leg now RETURNS, so the cell is
 *     executed for the first time. It is a known deviation, not a regression. */
{ "sceKernelWaitSema", 0x4e3a1105u, "Bad sema", ICG_SEMA, 0,
  {0, 54, 331, 55}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x80020199u), CNW, IC_RET(0x80020199u), CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelWaitSema", 0x4e3a1105u, "Invalid count", ICG_SEMA, 1,
  {0, 56, 332, 57}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x800201bdu), CNW, IC_RET(0x800201bdu), CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelWaitSema", 0x4e3a1105u, "Valid sema", ICG_SEMA, 2,
  {0, 58, 333, 59}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceKernelWaitSemaCB", 0x6d212bacu, "Bad sema", ICG_SEMA, 0,
  {0, 62, 334, 63}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x80020199u), CNW, IC_RET(0x80020199u), CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelWaitSemaCB", 0x6d212bacu, "Invalid count", ICG_SEMA, 1,
  {0, 64, 335, 65}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x800201bdu), CNW, IC_RET(0x800201bdu), CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelWaitSemaCB", 0x6d212bacu, "Valid sema", ICG_SEMA, 2,
  {0, 66, 336, 67}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},

/* ---- group C: event flag ------------------------------------------------ */
/* "Invalid mode" is the load-bearing row: ILLEGAL_MODE wins over the context
 * restriction with interrupts disabled (L72) but LOSES to it inside an interrupt
 * (L338). This single pair is why a universal pre-handler gate is wrong. */
{ "sceKernelWaitEventFlag", 0x402fcf22u, "Bad flag", ICG_EVF, 0,
  {0, 70, 337, 71}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x80020000u), IC_RET(0x80020000u), IC_RET(0x80020000u), IC_RET(0x80020000u)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelWaitEventFlag", 0x402fcf22u, "Invalid mode", ICG_EVF, 1,
  {0, 72, 338, 73}, {ICU, IC_RET(0x80020195u), ILCTX, IC_RET(0x80020195u)}, {IC_RET(0x80020195u), IC_RET(0x80020195u), IC_RET(0x80020195u), IC_RET(0x80020195u)},
  {IC_PREC_NA, IC_PREC_PARAM, IC_PREC_CONTEXT, IC_PREC_PARAM}, IC_HW_IMMEDIATE},
{ "sceKernelWaitEventFlag", 0x402fcf22u, "Valid flag", ICG_EVF, 2,
  {0, 74, 339, 75}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceKernelWaitEventFlag", 0x402fcf22u, "Already set", ICG_EVF, 3,
  {0, 76, 0, 77}, {ICU, CNW, ICU, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_UNKNOWN, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelWaitEventFlagCB", 0x328c546au, "Bad flag", ICG_EVF, 0,
  {0, 80, 340, 81}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x80020000u), IC_RET(0x80020000u), IC_RET(0x80020000u), IC_RET(0x80020000u)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelWaitEventFlagCB", 0x328c546au, "Invalid mode", ICG_EVF, 1,
  {0, 82, 341, 83}, {ICU, IC_RET(0x80020195u), ILCTX, IC_RET(0x80020195u)}, {IC_RET(0x80020195u), IC_RET(0x80020195u), IC_RET(0x80020195u), IC_RET(0x80020195u)},
  {IC_PREC_NA, IC_PREC_PARAM, IC_PREC_CONTEXT, IC_PREC_PARAM}, IC_HW_IMMEDIATE},
{ "sceKernelWaitEventFlagCB", 0x328c546au, "Valid flag", ICG_EVF, 2,
  {0, 84, 342, 85}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceKernelWaitEventFlagCB", 0x328c546au, "Already set", ICG_EVF, 3,
  {0, 86, 0, 87}, {ICU, CNW, ICU, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_UNKNOWN, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},

/* ---- group D: fixed-size pool ------------------------------------------- */
/* PR-C1 promoted the intr-off and disp-off cells of all four rows: the blocking
 * Allocate forms now answer CAN_NOT_WAIT ahead of the FPL object lookup. The
 * normal cells keep their control values and the intr-ctx cells stay pinned to
 * the pre-existing deviation -- sched_wait_permitted() is true inside a handler,
 * so the new check cannot fire there. Those belong to PR-D. */
{ "sceKernelAllocateFpl", 0xd979e9bfu, "Bad fpl", ICG_FPL, 0,
  {0, 102, 347, 103}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x800200d3u), CNW, IC_RET(0x800200d3u), CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelAllocateFpl", 0xd979e9bfu, "Valid fpl", ICG_FPL, 1,
  {0, 104, 348, 105}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), CNW, IC_RET(0), CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelAllocateFplCB", 0xe7282cb6u, "Bad fpl", ICG_FPL, 0,
  {0, 108, 349, 109}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x800200d3u), CNW, IC_RET(0x800200d3u), CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelAllocateFplCB", 0xe7282cb6u, "Valid fpl", ICG_FPL, 1,
  {0, 110, 350, 111}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), CNW, IC_RET(0), CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},

/* ---- group E: mutex (registered to h_ok on current main) ----------------- */
{ "sceKernelLockMutex", 0xb011b11fu, "Bad mutex", ICG_MUTEX, 0,
  {0, 168, 371, 169}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelLockMutex", 0xb011b11fu, "Bad count", ICG_MUTEX, 1,
  {0, 170, 372, 171}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelLockMutex", 0xb011b11fu, "Valid mutex", ICG_MUTEX, 2,
  {0, 172, 373, 173}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelLockMutexCB", 0x5bf4dd27u, "Bad mutex", ICG_MUTEX, 0,
  {0, 176, 374, 177}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelLockMutexCB", 0x5bf4dd27u, "Bad count", ICG_MUTEX, 1,
  {0, 178, 375, 179}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelLockMutexCB", 0x5bf4dd27u, "Valid mutex", ICG_MUTEX, 2,
  {0, 180, 376, 181}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},

/* ---- group F: lightweight mutex ----------------------------------------- */
{ "sceKernelLockLwMutex", 0xbea46419u, "Bad count", ICG_LWMUTEX, 0,
  {0, 184, 377, 185}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelLockLwMutex", 0xbea46419u, "Valid mutex", ICG_LWMUTEX, 1,
  {0, 186, 378, 187}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelLockLwMutexCB", 0x1fc64e09u, "Bad count", ICG_LWMUTEX, 0,
  {0, 190, 379, 191}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelLockLwMutexCB", 0x1fc64e09u, "Valid mutex", ICG_LWMUTEX, 1,
  {0, 192, 380, 193}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},

/* ---- group G: thread join ----------------------------------------------- */
/* ILLEGAL_THID survives BOTH context restrictions (L204 and L383): the object
 * check genuinely runs first for this API, in every context. */
{ "sceKernelWaitThreadEnd", 0x278c0df5u, "Bad thread", ICG_THREAD, 0,
  {0, 204, 383, 205}, {ICU, IC_RET(0x80020197u), IC_RET(0x80020197u), IC_RET(0x80020197u)}, {IC_RET(0x80020197u), IC_RET(0x80020197u), IC_RET(0x80020197u), IC_RET(0x80020197u)},
  {IC_PREC_NA, IC_PREC_OBJECT, IC_PREC_OBJECT, IC_PREC_OBJECT}, IC_HW_IMMEDIATE},
/* "Not running" does not block on current main: h_wait_thread_status() reports
 * DORMANT (0x800201a2) for a created-but-never-started thread and the handler
 * returns it, so the interrupt-context leg is measurable here. */
{ "sceKernelWaitThreadEnd", 0x278c0df5u, "Not running", ICG_THREAD, 1,
  {0, 206, 384, 207}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x800201a2u), IC_RET(0x800201a2u), IC_RET(0x800201a2u), IC_RET(0x800201a2u)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceKernelWaitThreadEnd", 0x278c0df5u, "Running", ICG_THREAD, 2,
  {0, 208, 385, 209}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceKernelWaitThreadEndCB", 0x840e8133u, "Bad thread", ICG_THREAD, 0,
  {0, 212, 386, 213}, {ICU, IC_RET(0x80020197u), IC_RET(0x80020197u), IC_RET(0x80020197u)}, {IC_RET(0x80020197u), IC_RET(0x80020197u), IC_RET(0x80020197u), IC_RET(0x80020197u)},
  {IC_PREC_NA, IC_PREC_OBJECT, IC_PREC_OBJECT, IC_PREC_OBJECT}, IC_HW_IMMEDIATE},
{ "sceKernelWaitThreadEndCB", 0x840e8133u, "Not running", ICG_THREAD, 1,
  {0, 214, 387, 215}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0x800201a2u), IC_RET(0x800201a2u), IC_RET(0x800201a2u), IC_RET(0x800201a2u)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceKernelWaitThreadEndCB", 0x840e8133u, "Running", ICG_THREAD, 2,
  {0, 216, 388, 217}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},

/* ---- group H: volatile memory ------------------------------------------- */
{ "sceKernelVolatileMemLock", 0x3e0271d3u, "While not locked", ICG_VOLATILE, 0,
  {0, 290, 412, 291}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_IMMEDIATE},
{ "sceKernelVolatileMemLock", 0x3e0271d3u, "While locked", ICG_VOLATILE, 1,
  {0, 292, 0, 293}, {ICU, CNW, ICU, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_UNKNOWN, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},

/* ---- group I: UMD ------------------------------------------------------- */
/* ERRNO_INVALID_ARGUMENT beats both context restrictions (L296, L413): the type
 * mask is validated before anything else. */
{ "sceUmdWaitDriveStat", 0x8ef08fceu, "Invalid type", ICG_UMD, 0,
  {0, 296, 413, 297}, {ICU, IC_RET(0x80010016u), IC_RET(0x80010016u), IC_RET(0x80010016u)}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_PARAM, IC_PREC_PARAM, IC_PREC_PARAM}, IC_HW_IMMEDIATE},
{ "sceUmdWaitDriveStat", 0x8ef08fceu, "Valid type", ICG_UMD, 1,
  {0, 298, 414, 299}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceUmdWaitDriveStatWithTimer", 0x56202973u, "Invalid type", ICG_UMD, 0,
  {0, 302, 415, 303}, {ICU, IC_RET(0x80010016u), IC_RET(0x80010016u), IC_RET(0x80010016u)}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_PARAM, IC_PREC_PARAM, IC_PREC_PARAM}, IC_HW_IMMEDIATE},
{ "sceUmdWaitDriveStatWithTimer", 0x56202973u, "Valid type", ICG_UMD, 1,
  {0, 304, 416, 305}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},
{ "sceUmdWaitDriveStatCB", 0x4a9e5e29u, "Invalid type", ICG_UMD, 0,
  {0, 308, 417, 309}, {ICU, IC_RET(0x80010016u), IC_RET(0x80010016u), IC_RET(0x80010016u)}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_PARAM, IC_PREC_PARAM, IC_PREC_PARAM}, IC_HW_IMMEDIATE},
{ "sceUmdWaitDriveStatCB", 0x4a9e5e29u, "Valid type", ICG_UMD, 1,
  {0, 310, 418, 311}, {ICU, CNW, ILCTX, CNW}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK},

/* ---- group J: controller ------------------------------------------------ */
/* Declared but NOT executed. ctrl_fill_n() reads hle.c's file-static sample ring
 * (s_ctrl_r / s_ctrl_w), which reset_fixture() cannot clear, so the result
 * depends on what ran earlier in the process: the first measurement returned 36
 * samples in normal context and blocked with interrupts disabled, from residue
 * rather than from context. Pinning a baseline on that would pin test order, not
 * behavior. Executing these needs a small test-only ring reset exported from
 * hle.c -- see the prerequisite list on issue #88. */
{ "sceCtrlReadBufferPositive", 0x1f803938u, "Bad count", ICG_CTRL, 0,
  {0, 220, 389, 221}, {ICU, IC_RET(0x80000104u), IC_RET(0x80000104u), IC_RET(0x80000104u)}, {IC_NOTRUN, IC_NOTRUN, IC_NOTRUN, IC_NOTRUN},
  {IC_PREC_NA, IC_PREC_PARAM, IC_PREC_PARAM, IC_PREC_PARAM}, IC_HW_IMMEDIATE,
  "fixture-state: hle.c controller ring is not resettable" },
{ "sceCtrlReadBufferPositive", 0x1f803938u, "Valid", ICG_CTRL, 1,
  {0, 222, 390, 223}, {ICU, CNW, ILCTX, CNW}, {IC_NOTRUN, IC_NOTRUN, IC_NOTRUN, IC_NOTRUN},
  {IC_PREC_NA, IC_PREC_CONTEXT, IC_PREC_CONTEXT, IC_PREC_CONTEXT}, IC_HW_WOULD_BLOCK,
  "fixture-state: hle.c controller ring is not resettable" },

/* ---- group K: IoFileMgr, bad descriptor only ---------------------------- */
/* Only the bad-fd column is exercised: the valid-fd cells need a real host file,
 * which would make the outcome depend on the filesystem rather than on context.
 * BADF wins over both context restrictions in every cell of this group. */
/* Current main answers the bad-fd cells in the errno error space
 * (0x80010009 = ERRNO_BAD_FILE_DESCRIPTOR) rather than ThreadMan's BADF
 * (0x80020323) that hardware returns. Same class of error, different code --
 * an ordinary known deviation, not a context defect. */
{ "sceIoRead", 0x6a638d83u, "Bad file", ICG_IO_BADFD, 0,
  {0, 258, 401, 259}, {ICU, IC_RET(0x80020323u), IC_RET(0x80020323u), IC_RET(0x80020323u)}, {IC_RET(0x80010009u), IC_RET(0x80010009u), IC_RET(0x80010009u), IC_RET(0x80010009u)},
  {IC_PREC_NA, IC_PREC_OBJECT, IC_PREC_OBJECT, IC_PREC_OBJECT}, IC_HW_IMMEDIATE},
{ "sceIoWrite", 0x42ec03acu, "Bad file", ICG_IO_BADFD, 0,
  {0, 264, 403, 265}, {ICU, IC_RET(0x80020323u), IC_RET(0x80020323u), IC_RET(0x80020323u)}, {IC_RET(0x80010009u), IC_RET(0x80010009u), IC_RET(0x80010009u), IC_RET(0x80010009u)},
  {IC_PREC_NA, IC_PREC_OBJECT, IC_PREC_OBJECT, IC_PREC_OBJECT}, IC_HW_IMMEDIATE},
{ "sceIoWaitAsync", 0xe23eec33u, "Bad file", ICG_IO_BADFD, 1,
  {0, 270, 405, 271}, {ICU, IC_RET(0x80020323u), IC_RET(0x80020323u), IC_RET(0x80020323u)}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_OBJECT, IC_PREC_OBJECT, IC_PREC_OBJECT}, IC_HW_IMMEDIATE},
{ "sceIoWaitAsyncCB", 0x35dbd746u, "Bad file", ICG_IO_BADFD, 1,
  {0, 276, 407, 277}, {ICU, IC_RET(0x80020323u), IC_RET(0x80020323u), IC_RET(0x80020323u)}, {IC_RET(0), IC_RET(0), IC_RET(0), IC_RET(0)},
  {IC_PREC_NA, IC_PREC_OBJECT, IC_PREC_OBJECT, IC_PREC_OBJECT}, IC_HW_IMMEDIATE},
};

#define IC_MATRIX_N ((int)(sizeof kIcMatrix / sizeof kIcMatrix[0]))

/* -------------------------------------------------------------------------
 * Runner state
 * ------------------------------------------------------------------------- */
static uint64_t    s_ic_actual[IC_MATRIX_N][IC_NCTX];
static const char *s_ic_reason[IC_MATRIX_N][IC_NCTX];
static int      s_ic_conforms, s_ic_known_dev, s_ic_notrun, s_ic_setupfail;
static int      s_ic_completed_probes;   /* probes whose coroutine returned and parked */

/* Setup NIDs a group needs in addition to the probe's own NID. A cell whose
 * arrangement cannot be built through production dispatch is out of registry
 * scope just as surely as one whose probe NID is missing. */
static int ic_group_reachable(IcGroup g) {
    switch (g) {
    case ICG_SEMA:     return sr_hle_test_is_registered(NID_IC_CREATE_SEMA);
    case ICG_EVF:      return sr_hle_test_is_registered(NID_IC_CREATE_EVF) &&
                              sr_hle_test_is_registered(NID_IC_SET_EVF);
    case ICG_FPL:      return sr_hle_test_is_registered(NID_IC_CREATE_FPL);
    case ICG_MUTEX:    return sr_hle_test_is_registered(NID_IC_CREATE_MUTEX);
    case ICG_LWMUTEX:  return sr_hle_test_is_registered(NID_IC_CREATE_LWMUTEX);
    case ICG_VOLATILE: return sr_hle_test_is_registered(NID_IC_VOLATILE_MEM_LOCK);
    default:           return 1;
    }
}

/* The interrupt-context leg additionally needs the sub-interrupt registration
 * pair, because that is how waits.cpp builds the context and how deliver_vblank
 * finds a handler to dispatch. */
static int ic_interrupt_context_reachable(void) {
    return sr_hle_test_is_registered(NID_IC_REGISTER_SUBINTR) &&
           sr_hle_test_is_registered(NID_IC_ENABLE_SUBINTR) &&
           sr_hle_test_is_registered(NID_IC_CPU_SUSPEND_INTR) &&
           sr_hle_test_is_registered(NID_IC_CPU_RESUME_INTR);
}

/* Cross-probe arrangement handles, published by ic_arrange for ic_invoke. */
static uint32_t s_ic_sema_uid, s_ic_evf_uid, s_ic_fpl_uid, s_ic_mutex_uid;
static uint32_t s_ic_target_thread_uid;

/* How many times a conformance probe should have parked on the scheduler,
 * derived from the recorded outcome table rather than from the park hook itself.
 * A probe parks exactly once per coroutine leg that RETURNED; a leg that blocked
 * never reaches the park, and the interrupt-context leg uses no coroutine. This
 * is an independent derivation, so it still cross-checks the coroutine layer's
 * own park counter in check_coroutine_lifecycle(). */
static int ic_expected_parks(void) {
    int n = 0;
    for (int i = 0; i < IC_MATRIX_N; i++) {
        for (int c = 0; c < IC_NCTX; c++) {
            if (c == IC_ICTX) continue; /* interrupt-context leg uses no coroutine */
            uint64_t v = s_ic_actual[i][c];
            if (v != IC_BLOCKED && v != IC_NOTRUN && v != IC_SETUPFAIL) n++;
        }
    }
    return n;
}

static void ic_fmt_outcome(uint64_t v, char *out, size_t n) {
    if (v == IC_BLOCKED)         snprintf(out, n, "WOULD_BLOCK");
    else if (v == IC_UNKNOWN)    snprintf(out, n, "unknown");
    else if (v == IC_NOTRUN)     snprintf(out, n, "NOT_RUN");
    else if (v == IC_SETUPFAIL)  snprintf(out, n, "SETUP_FAILED");
    else                         snprintf(out, n, "0x%08x", (unsigned)(v & 0xffffffffu));
}

/* -------------------------------------------------------------------------
 * Per-group arrangement. Each group owns its own object creation and argument
 * layout; there is deliberately no single "generic wait call" abstraction,
 * because the APIs do not share one.
 *
 * Every object is created through the production registered NID, so the fixture
 * is production dispatch too, not a hand-built table entry. Returns 0 when the
 * arrangement could not be built (recorded as SETUP_FAILED, never as a pass).
 * ------------------------------------------------------------------------- */
static int ic_arrange(const IcProbe *p, CpuState *cpu) {
    CpuState setup;
    memset(&setup, 0, sizeof setup);
    memset(cpu, 0, sizeof *cpu);

    switch (p->group) {
    case ICG_NONE:
        if (strcmp(p->api, "sceKernelDelayThread") == 0 ||
            strcmp(p->api, "sceKernelDelayThreadCB") == 0)
            cpu->r[4] = 200u;
        return 1;

    case ICG_SEMA: {
        setup.r[4] = IC_NAMEBUF; setup.r[5] = 0; setup.r[6] = 0; setup.r[7] = 1;
        s_ic_sema_uid = sr_syscall(&setup, NID_IC_CREATE_SEMA);   /* init 0, max 1 */
        if (s_ic_sema_uid == 0x80020000u) return 0;
        cpu->r[4] = (p->variant == 0) ? 0u : s_ic_sema_uid;
        cpu->r[5] = (p->variant == 1) ? 9u : 1u;   /* invalid count = 9 vs max 1 */
        cpu->r[6] = 0u;                            /* NULL timeout = wait forever */
        return 1;
    }

    case ICG_EVF: {
        setup.r[4] = IC_NAMEBUF; setup.r[5] = 0; setup.r[6] = 0; setup.r[7] = 0;
        s_ic_evf_uid = sr_syscall(&setup, NID_IC_CREATE_EVF);     /* initial pattern 0 */
        if (s_ic_evf_uid == 0x80020000u) return 0;
        if (p->variant == 3) {                                    /* "Already set" */
            memset(&setup, 0, sizeof setup);
            setup.r[4] = s_ic_evf_uid; setup.r[5] = 1u;
            (void)sr_syscall(&setup, NID_IC_SET_EVF);
        }
        cpu->r[4] = (p->variant == 0) ? 0u : s_ic_evf_uid;
        cpu->r[5] = 1u;                                  /* bits */
        cpu->r[6] = (p->variant == 1) ? 0xFFu : 0u;      /* mode: 0xFF invalid, 0 = WAITAND */
        cpu->r[7] = 0u;                                  /* outBits = NULL */
        cpu->r[8] = 0u;                                  /* timeout ptr = NULL (stack_arg 0) */
        return 1;
    }

    case ICG_FPL: {
        setup.r[4] = IC_NAMEBUF; setup.r[5] = 0; setup.r[6] = 0; setup.r[7] = 0x100u;
        setup.r[8] = 0x10u;                              /* numBlocks via stack_arg(0) */
        s_ic_fpl_uid = sr_syscall(&setup, NID_IC_CREATE_FPL);
        if (s_ic_fpl_uid == 0) return 0;
        cpu->r[4] = (p->variant == 0) ? 0u : s_ic_fpl_uid;
        cpu->r[5] = IC_SCRATCH;                          /* data pointer out */
        cpu->r[6] = 0u;                                  /* NULL timeout */
        return 1;
    }

    case ICG_MUTEX: {
        setup.r[4] = IC_NAMEBUF; setup.r[5] = 0; setup.r[6] = 0; setup.r[7] = 0;
        s_ic_mutex_uid = sr_syscall(&setup, NID_IC_CREATE_MUTEX);
        if (s_ic_mutex_uid == 0x80020000u) return 0;
        cpu->r[4] = (p->variant == 0) ? 0u : s_ic_mutex_uid;
        cpu->r[5] = (p->variant == 1) ? 9u : 1u;
        cpu->r[6] = 0u;
        return 1;
    }

    case ICG_LWMUTEX: {
        setup.r[4] = IC_LWWORK; setup.r[5] = IC_NAMEBUF; setup.r[6] = 0; setup.r[7] = 0;
        if (sr_syscall(&setup, NID_IC_CREATE_LWMUTEX) != 0u) return 0;
        cpu->r[4] = IC_LWWORK;
        cpu->r[5] = (p->variant == 0) ? 9u : 1u;         /* "Bad count" = 9 */
        cpu->r[6] = 0u;
        return 1;
    }

    case ICG_THREAD: {
        /* variant 0 = bad thid (0); 1 = a created-but-never-started thread;
         * 2 = a thread that is READY, i.e. running as far as the join is
         * concerned. Both fixture threads are source-owned TCBs, as everywhere
         * else in this selftest. */
        if (p->variant == 0) { cpu->r[4] = 0u; cpu->r[5] = 0u; return 1; }
        TCB *target = fixture_thread(0x1c0u + p->variant,
                                     p->variant == 1 ? TH_DORMANT : TH_READY, 40);
        if (!target) return 0;
        if (p->variant == 1) target->started = 0;
        else                 target->started = 1;
        s_ic_target_thread_uid = target->uid;
        cpu->r[4] = target->uid;
        cpu->r[5] = 0u;                                  /* NULL timeout */
        return 1;
    }

    case ICG_VOLATILE:
        if (p->variant == 1) {                           /* "While locked" */
            setup.r[4] = 0; setup.r[5] = IC_SCRATCH; setup.r[6] = IC_SCRATCH + 4u;
            (void)sr_syscall(&setup, NID_IC_VOLATILE_MEM_LOCK);
        }
        cpu->r[4] = 0u;
        cpu->r[5] = IC_SCRATCH;
        cpu->r[6] = IC_SCRATCH + 4u;
        return 1;

    case ICG_UMD:
        cpu->r[4] = (p->variant == 0) ? 0x00u : 0x20u;   /* type mask */
        cpu->r[5] = 100u;                                /* timeout (WithTimer / CB) */
        return 1;

    case ICG_CTRL:
        cpu->r[4] = IC_PADBUF;
        cpu->r[5] = (p->variant == 0) ? 256u : 64u;      /* 256 is over the hw limit */
        return 1;

    case ICG_IO_BADFD:
        cpu->r[4] = 63u;                                 /* fd 63 is never open */
        if (p->variant == 0) { cpu->r[5] = IC_IOBUF; cpu->r[6] = 1u; }  /* read/write */
        else                 { cpu->r[5] = IC_SCRATCH; }                /* async result out */
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Probe execution
 * ------------------------------------------------------------------------- */
static const IcProbe *s_ic_probe;
static CpuState       s_ic_probe_cpu;
static uint64_t       s_ic_probe_result;
static int            s_ic_probe_returned;

static void ic_probe_body(void *arg) {
    (void)arg;
    s_ic_probe_result = IC_RET(sr_syscall(&s_ic_probe_cpu, s_ic_probe->nid));
    s_ic_probe_returned = 1;
    s_ic_completed_probes++;
    selftest_park_on_scheduler();
}

/* Run one probe on its own guest-thread coroutine. Returns the outcome; a probe
 * that entered a real wait is reported as IC_BLOCKED rather than deadlocking the
 * suite, because control comes back here via switch_to_scheduler(). */
static uint64_t ic_run_on_thread(const IcProbe *p, int intr_off) {
    TCB *probe = fixture_thread(0x1b0u, TH_RUNNING, 32);
    if (!probe) return IC_SETUPFAIL;
    s_cur = (int)(probe - s_tcb);
    probe->started = 1;

    if (!ic_arrange(p, &s_ic_probe_cpu)) { s_cur = -1; return IC_SETUPFAIL; }

    uint32_t token = 0;
    if (intr_off) {
        CpuState ctl;
        memset(&ctl, 0, sizeof ctl);
        token = sr_syscall(&ctl, NID_IC_CPU_SUSPEND_INTR);
        if (sched_interrupts_enabled()) { s_cur = -1; return IC_SETUPFAIL; }
    }

    s_ic_probe = p;
    s_ic_probe_returned = 0;
    s_ic_probe_result = IC_BLOCKED;
    probe->coro = sr_coro_create(ic_probe_body, NULL, (size_t)1 << 20);
    if (!probe->coro) { s_cur = -1; return IC_SETUPFAIL; }
    sr_coro_switch(probe->coro);

    uint64_t outcome = s_ic_probe_returned ? s_ic_probe_result : IC_BLOCKED;

    /* Recover: the probe may be parked in a wait it will never be released
     * from. Tearing its coroutine down here is what keeps a would-block cell a
     * measurement instead of a hang; reset_fixture() clears the TCB table. */
    sr_coro_destroy(probe->coro);
    probe->coro = NULL;
    s_cur = -1;

    if (intr_off) {
        CpuState ctl;
        memset(&ctl, 0, sizeof ctl);
        ctl.r[4] = token;
        (void)sr_syscall(&ctl, NID_IC_CPU_RESUME_INTR);
    }
    return outcome;
}

static uint64_t ic_run_on_thread_disp(const IcProbe *p) {
    TCB *probe = fixture_thread(0x1b0u, TH_RUNNING, 32);
    if (!probe) return IC_SETUPFAIL;
    s_cur = (int)(probe - s_tcb);
    probe->started = 1;

    if (!ic_arrange(p, &s_ic_probe_cpu)) { s_cur = -1; return IC_SETUPFAIL; }

    CpuState ctl;
    memset(&ctl, 0, sizeof ctl);
    uint32_t token = sr_syscall(&ctl, NID_IC_SUSPEND_DISPATCH_THREAD);
    if (sched_dispatch_enabled()) { s_cur = -1; return IC_SETUPFAIL; }

    s_ic_probe = p;
    s_ic_probe_returned = 0;
    s_ic_probe_result = IC_BLOCKED;
    probe->coro = sr_coro_create(ic_probe_body, NULL, (size_t)1 << 20);
    uint64_t outcome;
    if (!probe->coro) {
        outcome = IC_SETUPFAIL;
    } else {
        sr_coro_switch(probe->coro);
        outcome = s_ic_probe_returned ? s_ic_probe_result : IC_BLOCKED;
        sr_coro_destroy(probe->coro);
        probe->coro = NULL;
    }
    s_cur = -1;

    memset(&ctl, 0, sizeof ctl);
    ctl.r[4] = token;
    (void)sr_syscall(&ctl, NID_IC_RESUME_DISPATCH_THREAD);
    return outcome;
}

/* Interrupt-context leg. The probe runs inside the selftest's dispatch() stub
 * while deliver_vblank() has the CPU, i.e. with s_servicing_interrupts set and
 * s_cur == -1, reached through the production registration + resume path. */
static int      s_ic_in_intr_probe;
static uint64_t s_ic_intr_result;
static int      s_ic_intr_ran;

/* Claim the guest dispatch of the synthetic sub-interrupt handler entry. This is
 * the selftest's stand-in for the generated translation of that entry, exactly as
 * dispatch() already stands in for a callback entry. Everything around it --
 * registration, enable, latch, resume, deliver_vblank -- is production code. */
static int ic_dispatch_intercept(uint32_t target) {
    if (target != IC_INTR_ENTRY) return 0;
    if (s_ic_in_intr_probe && !s_ic_intr_ran) {
        s_ic_intr_ran = 1;
        CpuState icpu = s_ic_probe_cpu;
        s_ic_intr_result = IC_RET(sr_syscall(&icpu, s_ic_probe->nid));
    }
    return 1;
}

static uint64_t ic_run_in_interrupt(const IcProbe *p) {
    if (!ic_arrange(p, &s_ic_probe_cpu)) return IC_SETUPFAIL;

    CpuState ctl;
    memset(&ctl, 0, sizeof ctl);
    ctl.r[4] = 30u;                    /* PSP_VBLANK_INT */
    ctl.r[5] = 1u;                     /* sub-interrupt 1, as waits.cpp uses */
    ctl.r[6] = IC_INTR_ENTRY;
    ctl.r[7] = 0u;
    if (sr_syscall(&ctl, NID_IC_REGISTER_SUBINTR) != 0u) return IC_SETUPFAIL;
    memset(&ctl, 0, sizeof ctl);
    ctl.r[4] = 30u; ctl.r[5] = 1u;
    if (sr_syscall(&ctl, NID_IC_ENABLE_SUBINTR) != 0u) return IC_SETUPFAIL;

    s_ic_probe = p;
    s_ic_intr_ran = 0;
    s_ic_intr_result = IC_SETUPFAIL;
    s_ic_in_intr_probe = 1;

    /* Suspend, latch a VBLANK, then resume: sched_resume_interrupts() runs
     * scheduler_service_pending() -> deliver_vblank() -> dispatch(). */
    memset(&ctl, 0, sizeof ctl);
    uint32_t token = sr_syscall(&ctl, NID_IC_CPU_SUSPEND_INTR);
    sched_raise_interrupt(SCHED_INTR_VBLANK);
    memset(&ctl, 0, sizeof ctl);
    ctl.r[4] = token;
    (void)sr_syscall(&ctl, NID_IC_CPU_RESUME_INTR);

    s_ic_in_intr_probe = 0;
    return s_ic_intr_ran ? s_ic_intr_result : IC_SETUPFAIL;
}

/* -------------------------------------------------------------------------
 * Classification
 * ------------------------------------------------------------------------- */
static void ic_classify(const IcProbe *p, int idx, int ctx, uint64_t actual,
                        const char *reason) {
    char sa[32], sh[32], sb[32], msg[320];
    s_ic_actual[idx][ctx] = actual;
    s_ic_reason[idx][ctx] = reason;
    ic_fmt_outcome(actual, sa, sizeof sa);
    ic_fmt_outcome(p->hw[ctx], sh, sizeof sh);
    ic_fmt_outcome(p->base[ctx], sb, sizeof sb);

    const char *scen = p->scenario[0] ? p->scenario : "(no argument)";

    if (actual == IC_SETUPFAIL) {
        s_ic_setupfail++;
        snprintf(msg, sizeof msg,
                 "[HARNESS LIMIT] %s / %s / %s: fixture could not be arranged (%s)",
                 p->api, scen, kIcCtxName[ctx], reason ? reason : "unknown");
        expect(0, msg);
        return;
    }
    if (actual == IC_NOTRUN) {
        s_ic_notrun++;
        fprintf(stderr,
                "intr-conformance: NOT RUN  %-30s %-18s %-20s reason=%s "
                "(hw=%s, waits.expected:%u)\n",
                p->api, scen, kIcCtxName[ctx], reason ? reason : "unknown", sh,
                (unsigned)p->ev_line[ctx]);
        /* A gated cell must still be declared gated in the table, so a cell that
         * silently starts running (or stops) is caught. registry-scope is the one
         * reason that is a property of the build rather than of the matrix, so it
         * is exempt from the declaration -- widening scope must not require a
         * table edit before the new cells can be measured. */
        if (strcmp(reason ? reason : "", "registry-scope") != 0) {
            snprintf(msg, sizeof msg,
                     "%s / %s / %s: gated cell is declared NOT_RUN in the matrix",
                     p->api, scen, kIcCtxName[ctx]);
            expect(p->base[ctx] == IC_NOTRUN, msg);
        }
        return;
    }
    if (p->hw[ctx] == IC_UNKNOWN) {
        /* Control column: executed and reported, never compared to hardware.
         * It is still pinned to its baseline so a change is visible. */
        snprintf(msg, sizeof msg,
                 "[CONTROL] %s / %s / %s: %s (no hardware cell; baseline %s)",
                 p->api, scen, kIcCtxName[ctx], sa, sb);
        expect(actual == p->base[ctx], msg);
        return;
    }

    if (actual == p->hw[ctx]) {
        if (p->base[ctx] == p->hw[ctx]) {
            s_ic_conforms++;
            snprintf(msg, sizeof msg,
                     "%s / %s / %s == %s (waits.expected:%u, %s)",
                     p->api, scen, kIcCtxName[ctx], sh, (unsigned)p->ev_line[ctx],
                     kIcPrecName[p->prec[ctx]]);
            expect(1, msg);
        } else {
            snprintf(msg, sizeof msg,
                     "[PROMOTE BASELINE] %s / %s / %s now matches hardware %s; "
                     "change base[] from %s to hw in kIcMatrix",
                     p->api, scen, kIcCtxName[ctx], sh, sb);
            expect(0, msg);
        }
        return;
    }

    if (actual == p->base[ctx]) {
        s_ic_known_dev++;
        fprintf(stderr,
                "intr-conformance: KNOWN DEVIATION %-30s %-18s %-20s got=%s want=%s "
                "(waits.expected:%u, %s)\n",
                p->api, scen, kIcCtxName[ctx], sa, sh,
                (unsigned)p->ev_line[ctx], kIcPrecName[p->prec[ctx]]);
        return;
    }

    snprintf(msg, sizeof msg,
             "[REGRESSION] %s / %s / %s: got %s, hardware %s, recorded baseline %s "
             "(waits.expected:%u)",
             p->api, scen, kIcCtxName[ctx], sa, sh, sb, (unsigned)p->ev_line[ctx]);
    expect(0, msg);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
static void test_intr_context_conformance(void) {
    fprintf(stderr,
            "\nintr-conformance: %d probes x 4 executable contexts\n",
            IC_MATRIX_N);

    sr_hle_init();   /* the registry must exist before reachability can be asked */
    const int intr_ctx_ok = ic_interrupt_context_reachable();
    int out_of_scope = 0;
    int disp_off_executed = 0;
    int disp_off_skipped = 0;
    int disp_off_out_of_scope = 0;

    for (int i = 0; i < IC_MATRIX_N; i++) {
        const IcProbe *p = &kIcMatrix[i];

        /* Registry scope. A NID that sr_hle_init() did not register cannot be
         * called at all here, so the cell is reported, never invoked. */
        if (!sr_hle_test_is_registered(p->nid) || !ic_group_reachable(p->group)) {
            out_of_scope++;
            disp_off_out_of_scope++;
            for (int c = 0; c < IC_NCTX; c++)
                ic_classify(p, i, c, IC_NOTRUN, "registry-scope");
            continue;
        }
        /* Harness-side exclusion declared in the table itself. */
        if (p->skip) {
            disp_off_skipped++;
            for (int c = 0; c < IC_NCTX; c++) ic_classify(p, i, c, IC_NOTRUN, p->skip);
            continue;
        }

        /* Normal-context control. */
        reset_fixture(); sr_hle_init();
        s_vbl_next_us = UINT64_MAX;   /* keep VBLANK pacing out of the measurement */
        uint64_t normal = ic_run_on_thread(p, 0);
        ic_classify(p, i, IC_NORMAL, normal, "measured");

        /* Interrupts disabled. */
        reset_fixture(); sr_hle_init();
        s_vbl_next_us = UINT64_MAX;
        uint64_t intr_off = ic_run_on_thread(p, 1);
        ic_classify(p, i, IC_INTR_OFF, intr_off, "measured");

        /* Interrupt context -- gated on the measured NORMAL-context leg having
         * returned. See the header comment: with s_cur == -1 the scheduler's
         * blocking primitives no-op, so a handler that loops on an unsatisfied
         * wait spins instead of blocking, and that spin is unobservable.
         *
         * The normal leg is the right gate because it measures exactly the property
         * that causes the spin -- whether this probe's wait condition is satisfiable
         * -- and it measures it in the one context where no restriction can mask the
         * answer. The intr-disabled leg cannot: once a handler returns CAN_NOT_WAIT
         * it returns for a reason that does NOT hold inside an interrupt, where
         * ic_run_in_interrupt() dispatches with interrupts and dispatch both enabled
         * (sceKernelCpuResumeIntr restores the prior enabled state before
         * scheduler_service_pending runs). Gating on it would open the gate for
         * exactly the probes that still spin. */
        uint64_t ictx;
        const char *ictx_reason = "measured";
        if (!intr_ctx_ok) {
            ictx = IC_NOTRUN; ictx_reason = "registry-scope";
        } else if (normal == IC_BLOCKED || normal == IC_SETUPFAIL ||
                   intr_off == IC_SETUPFAIL) {
            ictx = IC_NOTRUN; ictx_reason = "spin-unbounded";
        } else {
            reset_fixture(); sr_hle_init();
            s_vbl_next_us = UINT64_MAX;
            ictx = ic_run_in_interrupt(p);
        }
        ic_classify(p, i, IC_ICTX, ictx, ictx_reason);

        /* Dispatch disabled. */
        reset_fixture(); sr_hle_init();
        s_vbl_next_us = UINT64_MAX;
        uint64_t disp_off = ic_run_on_thread_disp(p);
        ic_classify(p, i, IC_DISP_OFF, disp_off, "measured");
        disp_off_executed++;
    }

    fprintf(stderr,
            "intr-conformance: %d/%d probes out of registry scope in this build; "
            "interrupt-context substrate %s\n",
            out_of_scope, IC_MATRIX_N,
            intr_ctx_ok ? "available" : "UNAVAILABLE (sub-interrupt NIDs unregistered)");
    fprintf(stderr,
            "intr-conformance: %d/%d dispatch-disabled cells executed via sceKernelSuspendDispatchThread substrate "
            "(%d fixture-skipped, %d registry-out-of-scope, 0 UNTESTABLE)\n",
            disp_off_executed, IC_MATRIX_N, disp_off_skipped, disp_off_out_of_scope);

    fprintf(stderr,
            "intr-conformance: conforms=%d known-deviations=%d not-run=%d "
            "setup-failures=%d\n",
            s_ic_conforms, s_ic_known_dev, s_ic_notrun, s_ic_setupfail);
    expect(s_ic_setupfail == 0,
           "every conformance cell was either executed or explicitly gated; "
           "no cell was lost to a fixture failure");
}

#undef ICB
#undef ICU
#undef CNW
#undef ILCTX

#endif /* SR_INTR_CONFORMANCE_H */
