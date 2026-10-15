// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// LLE Phase 1 (PR 2): COP0 register definitions, kernel/user mode checks,
// exception entry, eret, and runtime transfer metadata (flow_kind/flow_target).
//
// This header deliberately uses only a forward-declared `struct CpuState` so
// src/rt/recomp.h can include it after the full CpuState definition without
// a circular include. The implementation lives in src/rt/cpu_lle.c.

#ifndef SR_CPU_LLE_H
#define SR_CPU_LLE_H

#include <stdint.h>

struct CpuState;

/* COP0 register indices (spec 3.2). sel != 0 encodings are unsupported in
 * Phase 1 and raise a reserved-instruction exception (never a silent zero). */
#ifndef SR_CP0_STATUS
#define SR_CP0_STATUS 12u
#endif
enum {
    SR_CP0_BADVADDR = 8u,
    SR_CP0_COUNT    = 9u,
    SR_CP0_COMPARE  = 11u,
    SR_CP0_CAUSE    = 13u,
    SR_CP0_EPC      = 14u,
    SR_CP0_PRID     = 15u,
    SR_CP0_CONFIG   = 16u
};

/* Exception codes (spec 3.3). */
enum {
    SR_EXC_INT = 0u,
    SR_EXC_ADEL = 4u,
    SR_EXC_ADES = 5u,
    SR_EXC_SYS = 8u,
    SR_EXC_BP = 9u,
    SR_EXC_RI = 10u,
    SR_EXC_CPU = 11u,
    SR_EXC_OV = 12u
};

/* Runtime transfer metadata (spec 3.3). These are dispatcher control signals,
 * not guest-visible architectural state; every dispatcher boundary must
 * consume and clear them via sr_cpu_clear_flow(). */
enum {
    SR_FLOW_NONE = 0u,
    SR_FLOW_EXCEPTION = 1u,
    SR_FLOW_ERET = 2u,
    SR_FLOW_INTERRUPT = 3u,
    SR_FLOW_FATAL = 4u
};

/* Status bit positions follow the generic MIPS32 layout. The exact PSP
 * enable/mask map is a hardware-oracle question (spec 6.3, Status row), so
 * these are configurable Phase 1 defaults, not retail claims. */
enum {
    SR_STATUS_IE  = 0x00000001u,
    SR_STATUS_EXL = 0x00000002u,
    SR_STATUS_ERL = 0x00000004u,
    SR_STATUS_KSU_MASK = 0x00000018u,
    SR_STATUS_KSU_USER = 0x00000010u,
    SR_STATUS_IM_MASK = 0x0000FF00u,
    SR_STATUS_BEV = 0x00400000u
};

/* Guest-writable Status bits (spec 3.2). Reserved bits are preserved, never
 * cleared or set by an mtc0. Status.ERL is unsupported in Phase 1 (ErrorEPC
 * register 30 is not modeled) and excluded from the writable mask. */
#define SR_STATUS_WRITABLE_MASK \
    (SR_STATUS_IE | SR_STATUS_EXL | \
     SR_STATUS_KSU_MASK | SR_STATUS_IM_MASK | SR_STATUS_BEV)

/* Cause field layout (generic MIPS32 positions; PSP specifics per spec 6.3). */
enum {
    SR_CAUSE_EXCCODE_MASK = 0x0000007Cu,
    SR_CAUSE_IP_MASK = 0x0000FF00u,
    SR_CAUSE_IP_SW_MASK = 0x00000300u,
    SR_CAUSE_CE_MASK = 0x60000000u,
    SR_CAUSE_BD = 0x80000000u
};

/* Exact ERET encoding (spec 3.4: recognize exactly, never by mask). */
#define SR_OPCODE_ERET 0x42000018u

int sr_cp0_mfc0(
    struct CpuState *s,
    unsigned rt,
    unsigned rd,
    unsigned sel,
    uint32_t instr_pc);

int sr_cp0_mtc0(
    struct CpuState *s,
    unsigned rt,
    unsigned rd,
    unsigned sel,
    uint32_t instr_pc);

int sr_cpu_raise_exception(
    struct CpuState *s,
    unsigned exception_code,
    uint32_t fault_pc,
    uint32_t exception_pc,
    uint32_t badvaddr,
    unsigned in_delay_slot,
    unsigned coprocessor);

/* Data-access address check for loads and stores (spec 3.3). Returns 0 when the
 * access may proceed, or the exception code (SR_EXC_ADEL for a load, SR_EXC_ADES
 * for a store) that hardware would raise instead. Callers consult this only
 * while sr_cpu_lle_enabled(); see the implementation for what is measured and
 * what is still architectural. */
unsigned sr_cpu_data_access_fault(
    const struct CpuState *s,
    uint32_t address,
    unsigned width,
    int is_store);

/* Raise the address error sr_cpu_data_access_fault() reported. Resolves EPC and
 * Cause.BD from the caller's delay context and records the effective address in
 * BadVAddr, the same way a COP0 fault does. */
int sr_cpu_raise_data_fault(
    struct CpuState *s,
    unsigned exception_code,
    uint32_t address,
    uint32_t instr_pc);

/* One-call guard for generated code. Returns 1 when an address error was raised
 * and the access must be abandoned (the caller leaves the native body at once),
 * 0 when the access may proceed. `in_delay` installs the branch's delay context
 * around the raise so EPC and Cause.BD stay exact, the same way a delay-slot
 * COP0 fault does; `branch_pc` is read only then. */
int sr_cpu_guard_access(
    struct CpuState *s,
    uint32_t address,
    unsigned width,
    int is_store,
    uint32_t instr_pc,
    uint32_t branch_pc,
    unsigned in_delay);

int sr_cpu_eret(struct CpuState *s, uint32_t instr_pc);

/* Phase 1 stub: no INTC/timer model yet (PR 5/PR 6 own device time and Cause.IP
 * synchronization). Always reports no pending interrupt. */
int sr_cpu_poll(struct CpuState *s, uint32_t resume_pc);

int sr_cpu_in_kernel(const struct CpuState *s);
void sr_cpu_clear_flow(struct CpuState *s);

/* Synthetic LLE configuration (spec 3.3/3.5). Default off: raw guest SYSCALL
 * keeps the existing HLE sr_raw_syscall path and the interpreter keeps its
 * fail-closed contract, so default production behavior is unchanged. */
void sr_cpu_lle_set_enabled(int enabled);
int sr_cpu_lle_enabled(void);

/* Exception vector configuration. Defaults are synthetic fixture values (the
 * general default 0x80000180 is a fixture contract, not a PSP hardware claim;
 * spec 3.3). Vector selection honors Status.BEV. */
void sr_cpu_lle_set_vectors(
    uint32_t general,
    uint32_t interrupt,
    uint32_t bootstrap);
void sr_cpu_lle_get_vectors(
    uint32_t *general,
    uint32_t *interrupt,
    uint32_t *bootstrap);

/* Read-only COP0 identities. Defaults are zero: configuration values, never
 * guessed retail values (spec 3.2). */
void sr_cpu_lle_set_prid(uint32_t prid);
void sr_cpu_lle_set_config(uint32_t config);

/* Restore every LLE knob above to its default (for selftests). */
void sr_cpu_lle_reset_config(void);

#endif
