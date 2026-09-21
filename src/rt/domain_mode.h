// SPDX-License-Identifier: GPL-2.0-or-later
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
//    was requested (a counter the selftest asserts). Dual execution is PR 8.
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

#endif
