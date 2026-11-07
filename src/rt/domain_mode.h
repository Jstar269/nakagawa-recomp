// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// LLE Phase 1 (PR 3): per-domain HLE/LLE coexistence and the import-call seam.
//
// Spec section 4 owns this table: every domain defaults to HLE, and generated
// import stubs route through sr_import_call() instead of calling sr_syscall()
// directly. The HLE arm of the seam is exactly the existing sr_syscall()
// handler path (caller-saved poisoning and unknown-NID fatal policy intact);
// the LLE arm dispatches to a registered guest export and fails closed when
// none exists. Raw guest SYSCALL is a different boundary (spec 3.4/4):
// HLE/default CPU mode keeps sr_raw_syscall, LLE CPU mode raises SR_EXC_SYS,
// and this seam never converts an arbitrary raw syscall into an HLE import.
//
// PR 3 preflight resolutions (maintainer-directed; recorded here):
//  - NID-to-domain mapping is a static library/module-name table below
//    (ThreadMan* -> THREADMAN, IoFileMgr* -> IO, sceGe_user -> GE,
//    sceAudio/sceSasCore/sceAtrac3plus -> AUDIO, scePsmf*/sceMpeg/
//    sceVideocodec -> MEDIA, InterruptManager -> INTC, sceSysTimer/
//    SysTimerForKernel -> TIMER). The spec enum has no OTHER domain, so none
//    is added: libraries outside the table are HLE-only and never take the
//    LLE guest path (strict LLE on an unmapped NID fails closed).
//  - Guest-export registration is sr_import_register_export() plus lookup.
//    Re-registering the same address is idempotent; registering a different
//    address for one NID fails closed and keeps the original. Generated code
//    registers nothing in this PR; tests register directly.
//  - COSIM routing executes the HLE lane and records that a cosim comparison
//    was requested (a counter the selftest asserts). Dual execution is PR 8:
//    see "COSIM dual run" below (#2148).
//  - Configuration is C API only: sr_domain_mode_set/get (with the
//    sr_domain_set/get_mode spelling as an alias), sr_domain_bind_nid(), and
//    sr_domain_reset_defaults(). No environment variable or CLI option.
//  - The mode table becomes immutable after guest start via sr_domain_lock();
//    sr_domain_reset_defaults() is the test-only unlock. Top-level consumption
//    of SR_FLOW_FATAL left by the seam is PR 6 scheduler integration; PR 3
//    proves the signal is set and propagated, never silently cleared.
//
// This header uses only a forward-declared `struct CpuState` (like cpu_lle.h)
// so recomp.h can include it without a circular include.

#ifndef SR_DOMAIN_MODE_H
#define SR_DOMAIN_MODE_H

#include <stdint.h>

struct CpuState;

/* Spec section 4: the domain table, in spec order. */
typedef enum SrDomain {
    SR_DOMAIN_CPU = 0,
    SR_DOMAIN_BUS = 1,
    SR_DOMAIN_INTC = 2,
    SR_DOMAIN_TIMER = 3,
    SR_DOMAIN_THREADMAN = 4,
    SR_DOMAIN_IO = 5,
    SR_DOMAIN_GE = 6,
    SR_DOMAIN_AUDIO = 7,
    SR_DOMAIN_MEDIA = 8,
    SR_DOMAIN_COUNT = 9
} SrDomain;

/* Spec section 4: every domain defaults to SR_MODE_HLE. */
typedef enum SrDomainMode {
    SR_MODE_HLE = 0,
    SR_MODE_LLE = 1,
    SR_MODE_COSIM = 2,
    SR_MODE_LLE_FALLBACK_HLE = 3
} SrDomainMode;

/* Mode table. Returns 0 on success, negative when the domain/mode is invalid
 * or the table is locked (fail closed; the previous mode is kept). */
int sr_domain_mode_set(SrDomain domain, SrDomainMode mode);
SrDomainMode sr_domain_mode_get(SrDomain domain);

/* Preflight spelling aliases for the two accessors above. */
static inline int sr_domain_set_mode(SrDomain domain, SrDomainMode mode) {
    return sr_domain_mode_set(domain, mode);
}
static inline SrDomainMode sr_domain_get_mode(SrDomain domain) {
    return sr_domain_mode_get(domain);
}

/* Lock the table against further set/bind/register (guest start). Reset
 * restores HLE everywhere and clears bindings, exports, counters, and the
 * lock (selftests only; production never calls it after guest start). */
void sr_domain_lock(void);
int sr_domain_locked(void);
void sr_domain_reset_defaults(void);

/* Static library/module-name -> domain mapping (see the table above).
 * Returns SR_DOMAIN_COUNT for NULL/empty/unmapped names (HLE-only). */
SrDomain sr_domain_for_library(const char *library);

/* NID -> domain binding used by sr_import_call(). Unbound NIDs are HLE-only:
 * they take the HLE lane in every mode except strict LLE, where they fail
 * closed. Returns 0 on success, negative on invalid domain, lock, or a
 * conflicting rebind (the original binding is kept). */
int sr_domain_bind_nid(SrDomain domain, uint32_t nid);
SrDomain sr_domain_lookup_nid(uint32_t nid);

/* Guest-export registry used by the LLE arm. The address is a guest address,
 * never a host pointer. Returns 0 on success (idempotent for the same
 * address), negative for a zero/misaligned address, a zero NID, a full table,
 * a lock, or a conflicting re-registration (the original is kept). */
int sr_import_register_export(uint32_t nid, uint32_t guest_addr);
int sr_import_lookup_export(uint32_t nid, uint32_t *guest_addr_out);

/* Import seam (spec section 4). Return contract:
 *   0  the import was handled (HLE executed, COSIM recorded plus HLE,
 *      explicit fallback logged plus HLE, or the LLE guest export ran via
 *      dispatch_call_try). The HLE arms return the handler value cast to
 *      int; generated stubs key off s->flow_kind, not this value, because
 *      legitimate HLE errors are nonzero.
 *   <0 fail closed: flow_kind is SR_FLOW_FATAL with flow_target naming the
 *      NID, and a diagnostic names the NID, stub PC, and reason. Generated
 *      stubs return to the native caller without clearing flow so the
 *      existing PR 2 propagation unwinds instead of resuming as if the
 *      import had succeeded. A NULL state fails closed without touching it.
 * Unknown NIDs in the HLE arms reach sr_syscall() untouched, so its fatal
 * policy is unchanged. */
int sr_import_call(struct CpuState *s, uint32_t nid, uint32_t stub_pc);

/* Transition accounting for tests and diagnostics. */
uint32_t sr_domain_fallback_count(void);
uint32_t sr_domain_cosim_request_count(void);
uint32_t sr_domain_last_fallback_nid(void);
uint32_t sr_domain_last_cosim_nid(void);

/* COSIM dual run (LLE Phase 1 PR 8, #2148).
 *
 * A NID bound to a domain in SR_MODE_COSIM whose guest export is registered
 * (sr_import_register_export: the registry is the pair table) runs twice from
 * the same input state:
 *   1. The guest lane runs the export through dispatch_call_try. Any import
 *      it reaches through the seam is refused and unwinds the lane, so no
 *      host-side effect (kernel objects, files, devices) can happen twice.
 *   2. Guest memory and CpuState go back to the input state, and the HLE lane
 *      runs exactly as SR_MODE_HLE does. Its result is what the guest keeps:
 *      a difference never changes what the guest sees.
 * The two results are compared: v0 and the callee-saved GPRs (s0-s7, gp, sp,
 * fp), then every byte of scratchpad, eDRAM and RAM, except the stack
 * below the caller's $sp (dead after either lane returns; its size is
 * sr_domain_cosim_set_dead_stack). v1, caller-saved registers and FPU results
 * are not compared: the HLE lane leaves v1 as the caller had it and poisons
 * the caller-saved set. A float-returning pair is therefore not compared by
 * this slice. The eDRAM window is VRAM: the snapshot first reads back what the
 * renderer holds, and each rolled-back VRAM page is committed to it.
 *
 * Every call ends as MATCH, DIFF or NOT_COMPARED (never a silent match):
 *   SR_COSIM_DIFF: nid, stub PC, a0-a3, both v0 values, the first differing
 *     register or byte (address, guest value, HLE value) and the count of
 *     differing bytes;
 *   SR_COSIM_NOT_COMPARED: nid and reason (no guest export, the guest lane
 *     reached an import, dispatch rejected the export, a cosim call nested in
 *     another, no guest arena, no snapshot memory, the HLE lane did not end
 *     normally).
 * MATCH is logged once per NID; DIFF and NOT_COMPARED every time. A match
 * proves only the compared call with those inputs. The snapshot is taken
 * only in COSIM mode, which nothing enables by default. */
typedef enum SrCosimOutcome {
    SR_COSIM_OUTCOME_NONE = 0,
    SR_COSIM_OUTCOME_MATCH = 1,
    SR_COSIM_OUTCOME_DIFF = 2,
    SR_COSIM_OUTCOME_NOT_COMPARED = 3
} SrCosimOutcome;

typedef enum SrCosimReason {
    SR_COSIM_REASON_NONE = 0,
    SR_COSIM_REASON_UNPAIRED = 1,        /* no registered guest export */
    SR_COSIM_REASON_NESTED_IMPORT = 2,   /* guest lane reached an import */
    SR_COSIM_REASON_GUEST_REJECTED = 3,  /* dispatch refused the export */
    SR_COSIM_REASON_REENTRY = 4,         /* cosim call inside another's lane */
    SR_COSIM_REASON_NO_ARENA = 5,        /* guest memory not initialised */
    SR_COSIM_REASON_NO_MEMORY = 6,       /* snapshot allocation failed */
    SR_COSIM_REASON_HLE_FLOW = 7         /* HLE lane left a pending flow */
} SrCosimReason;

/* Where the first difference was found. */
typedef enum SrCosimDiffKind {
    SR_COSIM_DIFF_NONE = 0,
    SR_COSIM_DIFF_V0 = 1,
    SR_COSIM_DIFF_REGISTER = 2,  /* a callee-saved GPR: diff_reg names it */
    SR_COSIM_DIFF_MEMORY = 3
} SrCosimDiffKind;

typedef struct SrCosimResult {
    SrCosimOutcome outcome;
    SrCosimReason reason;
    uint32_t nid;
    uint32_t stub_pc;
    uint32_t args[4];
    uint32_t guest_v0;
    uint32_t hle_v0;
    SrCosimDiffKind first_kind;
    uint32_t diff_reg;           /* GPR number for SR_COSIM_DIFF_REGISTER */
    uint32_t guest_reg;
    uint32_t hle_reg;
    uint32_t first_addr;         /* guest physical address of the first byte */
    uint8_t guest_byte;
    uint8_t hle_byte;
    uint32_t diff_bytes;         /* differing bytes outside the dead stack */
    uint32_t nested_nid;         /* SR_COSIM_REASON_NESTED_IMPORT only */
} SrCosimResult;

uint32_t sr_domain_cosim_match_count(void);
uint32_t sr_domain_cosim_diff_count(void);
uint32_t sr_domain_cosim_not_compared_count(void);
/* The last completed cosim call (outcome NONE before the first one). A cosim
 * call refused as REENTRY is counted and logged but does not replace it. */
void sr_domain_cosim_last_result(SrCosimResult *out);
/* Bytes below the caller's $sp left out of the comparison (default 64 KiB).
 * Both lanes may leave dead frames there; nothing above $sp is excluded. */
void sr_domain_cosim_set_dead_stack(uint32_t bytes);
/* Free the snapshot buffers (they are kept between calls once allocated). */
void sr_domain_cosim_release(void);

#endif
