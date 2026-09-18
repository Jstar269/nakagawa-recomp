// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// LLE Phase 1 (PR 2): COP0 access, mode checks, exception entry, and eret.
//
// Return contract for every helper below: 0 means "no transfer, keep
// executing"; any negative value means flow metadata was set and the caller
// (generated code or the interpreter) must leave the native body at once and
// let the guest dispatcher consume flow_kind/flow_target. Helpers never
// return into a native caller past a guest transfer.

#include "cpu_lle.h"

#include "recomp.h"

/* Synthetic defaults (spec 3.3/3.7): fixture contracts, not PSP claims. */
#define SR_LLE_VECTOR_GENERAL_DEFAULT   0x80000180u
#define SR_LLE_VECTOR_INTERRUPT_DEFAULT 0x80000180u
#define SR_LLE_VECTOR_BOOTSTRAP_DEFAULT 0xBFC00200u

static uint32_t s_vector_general = SR_LLE_VECTOR_GENERAL_DEFAULT;
static uint32_t s_vector_interrupt = SR_LLE_VECTOR_INTERRUPT_DEFAULT;
static uint32_t s_vector_bootstrap = SR_LLE_VECTOR_BOOTSTRAP_DEFAULT;
static uint32_t s_prid_value;
static uint32_t s_config_value;
static int s_lle_enabled;

void sr_cpu_lle_set_enabled(int enabled) {
    s_lle_enabled = enabled != 0;
}

int sr_cpu_lle_enabled(void) {
    return s_lle_enabled;
}

void sr_cpu_lle_set_vectors(
    uint32_t general,
    uint32_t interrupt,
    uint32_t bootstrap) {
    s_vector_general = general;
    s_vector_interrupt = interrupt;
    s_vector_bootstrap = bootstrap;
}

void sr_cpu_lle_get_vectors(
    uint32_t *general,
    uint32_t *interrupt,
    uint32_t *bootstrap) {
    if (general) *general = s_vector_general;
    if (interrupt) *interrupt = s_vector_interrupt;
    if (bootstrap) *bootstrap = s_vector_bootstrap;
}

void sr_cpu_lle_set_prid(uint32_t prid) {
    s_prid_value = prid;
}

void sr_cpu_lle_set_config(uint32_t config) {
    s_config_value = config;
}

void sr_cpu_lle_reset_config(void) {
    s_vector_general = SR_LLE_VECTOR_GENERAL_DEFAULT;
    s_vector_interrupt = SR_LLE_VECTOR_INTERRUPT_DEFAULT;
    s_vector_bootstrap = SR_LLE_VECTOR_BOOTSTRAP_DEFAULT;
    s_prid_value = 0u;
    s_config_value = 0u;
    s_lle_enabled = 0;
}

int sr_cpu_in_kernel(const CpuState *s) {
    uint32_t status = s->cop0[SR_CP0_STATUS];
    /* EXL/ERL pin kernel mode regardless of KSU (spec 3.2). */
    if (status & (SR_STATUS_EXL | SR_STATUS_ERL)) {
        return 1;
    }
    return (status & SR_STATUS_KSU_MASK) == 0u;
}

void sr_cpu_clear_flow(CpuState *s) {
    s->flow_kind = SR_FLOW_NONE;
    s->flow_target = 0u;
}

int sr_cpu_poll(CpuState *s, uint32_t resume_pc) {
    /* No device model yet: INTC pending bits, timer advancement, and Cause.IP
     * synchronization land in PR 5; safe-point delivery lands in PR 6. A stub
     * that fabricated an interrupt here would invent timer behavior the timer
     * oracle questions (spec 6.3) leave BLOCKED, so report none. */
    (void)s;
    (void)resume_pc;
    return 0;
}

/* Only sel 0 registers modeled in Phase 1 read or write architectural state.
 * Anything else is an unsupported encoding (spec 3.2: never answer zero). */
static int sr_cp0_rd_supported(unsigned rd) {
    switch (rd) {
    case SR_CP0_BADVADDR:
    case SR_CP0_COUNT:
    case SR_CP0_COMPARE:
    case SR_CP0_STATUS:
    case SR_CP0_CAUSE:
    case SR_CP0_EPC:
    case SR_CP0_PRID:
    case SR_CP0_CONFIG:
        return 1;
    default:
        return 0;
    }
}

/* Delay context for COP0 faults comes from the caller's in_delay_slot/next_pc
 * fields: straight-line callers clear in_delay_slot first (generated code
 * emits the clear; the interpreter maintains it around delay slots), and
 * delay-slot callers set in_delay_slot=1 with next_pc holding the branch PC. */
static void sr_cp0_delay_context(
    const CpuState *s,
    uint32_t instr_pc,
    uint32_t *fault_pc,
    uint32_t *exception_pc,
    unsigned *in_delay_slot) {
    *fault_pc = instr_pc;
    if (s->in_delay_slot != 0u) {
        *exception_pc = s->next_pc;
        *in_delay_slot = 1u;
    } else {
        *exception_pc = instr_pc;
        *in_delay_slot = 0u;
    }
}

/* Data-access address checks (spec 3.3).
 *
 * MEASURED (run PSP-A3-01, campaign psp-hw-20260917, PSP-3000/6.61): a
 * user-mode `lw` from a kernel-segment address raises AdEL with EPC at the load
 * instruction, BadVAddr holding the effective address, and the destination
 * register unchanged. See docs/HARDWARE_ORACLE.md.
 *
 * The default accessors in recomp.h cannot express that result: SR_PHYS() masks
 * every guest address with 0x1FFFFFFF, so a kernel-segment address aliases onto
 * the same host byte as its user-segment counterpart and the access silently
 * succeeds. This helper is the LLE replacement for that decision. It is
 * consulted only while sr_cpu_lle_enabled(), so default builds keep the masking
 * behaviour and stay byte-identical.
 *
 * MEASURED (runs PSP-A3-02 and PSP-A3-03, same campaign): a misaligned user-mode
 * access to a mapped, writable, 16-byte-aligned buffer raises AdEL for a load
 * (Cause 0x10000010, ExcCode 4) and AdES for a store (Cause 0x10000014,
 * ExcCode 5). In both, EPC is the faulting access itself, BadVAddr is the
 * effective address INCLUDING the misaligned low bits (base+2, not the aligned
 * base), the destination register is unchanged, and execution does not continue
 * past the access. These replace what was previously labelled synthetic here.
 *
 * (A3-02/03 left the delay-slot and halfword cases synthetic; A3-04 and A3-05
 * below have since measured both, so that caveat is gone rather than kept.)
 *
 * MEASURED (runs PSP-A3-04 and PSP-A3-05, campaign psp-hw-20260917,
 * PSP-3000/6.61, user-mode PRX probes via PSPLink; fixtures
 * `exception-a3-delayslot`, `exception-a3-half-load`, `exception-a3-half-store`
 * in fixtures/psp_oracle/probe_exception_a3.c, cells in
 * docs/HARDWARE_ORACLE.md "CPU exception delay-slot and width cells"):
 * a misaligned `lw` in the delay slot of an always-taken branch raises AdEL
 * with Cause 0x90000010 (ExcCode 4, BD 1), EPC at the branch address, and
 * BadVAddr holding the effective address including the misaligned low bits;
 * `lh`/`sh` at an odd address of an owned aligned buffer raise AdEL/AdES
 * (Cause 0x10000010/0x10000014, BD 0) with BadVAddr = the effective address,
 * so the rule is width-relative. The destination register is unchanged on a
 * faulting load and execution does not continue past the access. The
 * delay-slot bookkeeping in sr_cpu_guard_access() (BD set, EPC = branch) is
 * thereby measured rather than architectural reasoning.
 *
 * MEASURED (run PSP-A3-06, same campaign; fixture `exception-a3-unaligned`,
 * a negative control that returns and writes a results file): lwl/lwr/swl/swr
 * across alignment boundaries complete normally with the PPSSPP-derived
 * little-endian merge semantics this file's sr_lwl/sr_lwr/sr_swl/sr_swr
 * implement (loaded 0x88776655, lwl-only 0x332211ff, lwr-only 0x00004433 for
 * the probed sequence). They are deliberately never alignment-guarded here,
 * in tools/codegen.py LLE_ACCESS, or in the interpreter width table.
 *
 * MEASURED (runs PSP-A3-08, PSP-A3-09 (+4 and +8 variants), PSP-A3-10 and
 * PSP-A3-11, same campaign and console; fixtures `exception-a3-vfpu-lvs`,
 * `exception-a3-vfpu-lvq4`, `exception-a3-vfpu-lvq8`, `exception-a3-vfpu-svq4`
 * and `exception-a3-vfpu-aligned`, which are probe_exception_a3.c built with
 * -DA3_CASE=8, =9, =10, =11 and =12; cells in docs/HARDWARE_ORACLE.md "VFPU
 * load/store alignment cells"): the VFPU group follows the same width-relative
 * rule with its own widths. `lv.s` at base+2 raises AdEL (Cause 0x10000010,
 * EPC = the access at 0x088043B8, BadVAddr = 0x08821C92, the effective address
 * with low bits kept), so singles need 4-byte alignment. `lv.q` at base+4 AND
 * at base+8 both raise AdEL (EPC = the access, BadVAddr = the effective
 * address in both), so quads need 16-byte alignment, not 4- or 8-byte.
 * `sv.q` at base+4 raises AdES (Cause 0x10000014, EPC = the store at
 * 0x088A1EB8, BadVAddr = the effective address 0x088BF794), so quad stores
 * match. All bases are 16-byte-aligned owned buffers; no run reported CpU
 * (ExcCode 11), which confirms the THREAD_ATTR_VFPU main-thread attribute
 * held, and Status reads 0x40088613 (the historical 0x00088613 plus CU2).
 * The negative control (aligned lv.q/sv.q round-trip plus lvl.q/lvr.q at the
 * 4-byte-aligned unaligned-to-16 address src+4) completes normally and writes
 * host0:/a3_vfpu_aligned_results.txt: dst = the src quad bit-for-bit, and the
 * left/right merges land in the C010/C020 lanes without faulting.
 *
 * The width-relative check below already expresses that rule: width 4 covers
 * lv.s/sv.s and width 16 covers lv.q/sv.q, while lvl/lvr/svl/svr bypass with
 * width 0 exactly like lwl/lwr/swl/swr. Both CPU tiers now pass those VFPU
 * widths: tools/codegen.py emits the VFPU memory forms from vfpu_effect()
 * through the same sr_cpu_guard_access() call as scalars (only when LLE CPU
 * mode is on), sr_vfpu_interp() checks before the access, and the interpreter
 * width table carries the VFPU rows at the same point relative to the access
 * as scalar loads/stores.
 *
 * STILL SYNTHETIC: sv.s misalignment (never probed; singles are measured only
 * for the lv.s +2 load cell), sv.q at +8, any VFPU access in a delay slot,
 * and lvl/lvr/svl/svr at odd (non-4-byte-aligned) addresses. */
unsigned sr_cpu_data_access_fault(
    const CpuState *s,
    uint32_t address,
    unsigned width,
    int is_store) {
    const unsigned code = is_store ? (unsigned)SR_EXC_ADES : (unsigned)SR_EXC_ADEL;

    if (!s || width == 0u) {
        return 0u;
    }
    /* Measured (PSP-A3-02 load, PSP-A3-03 store, PSP-A3-04 word in a delay
     * slot, PSP-A3-05 halfword at odd address, PSP-A3-08 VFPU single at +2,
     * PSP-A3-09 VFPU quad at +4/+8, PSP-A3-10 VFPU quad store at +4): a
     * misaligned access raises AdEL for loads and AdES for
     * stores, with BadVAddr = the effective address. Width-relative: an odd
     * address faults for halfwords and words, while base+2 faults only for
     * words; base+4 and base+8 fault only for width 16 (VFPU quads).
     * lwl/lwr/swl/swr and lvl/lvr/svl/svr never reach this check (PSP-A3-06,
     * PSP-A3-11) and are passed width 0 by their callers. */
    if (width > 1u && (address & (width - 1u)) != 0u) {
        return code;
    }
    /* Measured (PSP-A3-01): kuseg ends at 0x7FFFFFFF; a user-mode access at or
     * above 0x80000000 is an address error, never a masked alias into RAM. */
    if (address >= 0x80000000u && !sr_cpu_in_kernel(s)) {
        return code;
    }
    return 0u;
}

int sr_cpu_raise_data_fault(
    CpuState *s,
    unsigned exception_code,
    uint32_t address,
    uint32_t instr_pc) {
    uint32_t fault_pc;
    uint32_t exception_pc;
    unsigned in_delay_slot;

    if (!s) {
        return -2;
    }
    sr_cp0_delay_context(s, instr_pc, &fault_pc, &exception_pc, &in_delay_slot);
    return sr_cpu_raise_exception(
        s, exception_code, fault_pc, exception_pc, address, in_delay_slot, 0u);
}

int sr_cpu_guard_access(
    CpuState *s,
    uint32_t address,
    unsigned width,
    int is_store,
    uint32_t instr_pc,
    uint32_t branch_pc,
    unsigned in_delay) {
    unsigned code;

    if (!s) {
        return 0;
    }
    code = sr_cpu_data_access_fault(s, address, width, is_store);
    if (code == 0u) {
        return 0;
    }
    if (in_delay != 0u) {
        uint32_t prev_next_pc = s->next_pc;
        uint32_t prev_in_delay = s->in_delay_slot;
        s->in_delay_slot = 1u;
        s->next_pc = branch_pc;
        (void)sr_cpu_raise_data_fault(s, code, address, instr_pc);
        s->in_delay_slot = prev_in_delay;
        s->next_pc = prev_next_pc;
    } else {
        (void)sr_cpu_raise_data_fault(s, code, address, instr_pc);
    }
    return 1;
}

int sr_cpu_raise_exception(
    CpuState *s,
    unsigned exception_code,
    uint32_t fault_pc,
    uint32_t exception_pc,
    uint32_t badvaddr,
    unsigned in_delay_slot,
    unsigned coprocessor) {
    uint32_t cause;
    uint32_t status;
    uint32_t vector;

    if (!s) {
        return -2;
    }
    exception_code &= 0x1Fu;
    in_delay_slot = in_delay_slot != 0u;

    uint32_t pre_status = s->cop0[SR_CP0_STATUS];
    int nested = (pre_status & SR_STATUS_EXL) != 0;

    /* Delay-slot faults report the branch address in EPC (spec 3.3).
     * MIPS32 Architecture For Programmers, Vol. III: The MIPS32 Privileged
     * Resource Architecture (Section 5.1 / 6.1): when Status.EXL is already 1
     * at exception entry (nested exception), EPC and Cause.BD are preserved,
     * while Cause.ExcCode is still updated to the new exception. */
    if (!nested) {
        s->cop0[SR_CP0_EPC] = in_delay_slot ? exception_pc : fault_pc;
    }

    cause = s->cop0[SR_CP0_CAUSE];
    if (!nested) {
        cause &= (uint32_t)~(SR_CAUSE_BD | SR_CAUSE_CE_MASK | SR_CAUSE_EXCCODE_MASK);
        if (in_delay_slot) {
            cause |= SR_CAUSE_BD;
        }
    } else {
        cause &= (uint32_t)~(SR_CAUSE_CE_MASK | SR_CAUSE_EXCCODE_MASK);
    }
    cause |= (uint32_t)((exception_code << 2) & SR_CAUSE_EXCCODE_MASK);
    if (exception_code == SR_EXC_CPU) {
        cause |= (uint32_t)(((coprocessor & 3u) << 28) & SR_CAUSE_CE_MASK);
    }
    s->cop0[SR_CP0_CAUSE] = cause;

    if (exception_code == SR_EXC_ADEL || exception_code == SR_EXC_ADES) {
        s->cop0[SR_CP0_BADVADDR] = badvaddr;
    }

    s->cop0[SR_CP0_STATUS] |= SR_STATUS_EXL;
    status = s->cop0[SR_CP0_STATUS];

    /* BEV selects the bootstrap vector; hardware interrupts use the interrupt
     * vector; everything else uses the general vector (spec 3.3). */
    if (status & SR_STATUS_BEV) {
        vector = s_vector_bootstrap;
    } else if (exception_code == SR_EXC_INT) {
        vector = s_vector_interrupt;
    } else {
        vector = s_vector_general;
    }

    /* Vectors must be owned, aligned guest spans (spec 3.3). Readable-byte
     * validation is deferred until the dispatcher selects the interpreter tier. */
    if ((vector & 3u) != 0u || !sr_exec_span_owns_fetch(vector)) {
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = vector;
        return -2;
    }

    s->pc = vector;
    s->flow_kind = SR_FLOW_EXCEPTION;
    s->flow_target = vector;
    return -1;
}

int sr_cp0_mfc0(
    CpuState *s,
    unsigned rt,
    unsigned rd,
    unsigned sel,
    uint32_t instr_pc) {
    uint32_t fault_pc;
    uint32_t exception_pc;
    unsigned in_delay_slot;
    uint32_t value;

    if (!s || rt >= 32u || rd >= 32u) {
        return -2;
    }
    sr_cp0_delay_context(s, instr_pc, &fault_pc, &exception_pc, &in_delay_slot);
    if (!sr_cpu_in_kernel(s)) {
        return sr_cpu_raise_exception(
            s, SR_EXC_CPU, fault_pc, exception_pc, 0u, in_delay_slot, 0u);
    }
    if (sel != 0u || !sr_cp0_rd_supported(rd)) {
        return sr_cpu_raise_exception(
            s, SR_EXC_RI, fault_pc, exception_pc, 0u, in_delay_slot, 0u);
    }
    switch (rd) {
    case SR_CP0_PRID:
        value = s_prid_value;
        break;
    case SR_CP0_CONFIG:
        value = s_config_value;
        break;
    default:
        value = s->cop0[rd];
        break;
    }
    /* $zero discards the move (spec 3.2). */
    if (rt != 0u) {
        s->r[rt] = value;
        s->r[0] = 0u;
    }
    return 0;
}

int sr_cp0_mtc0(
    CpuState *s,
    unsigned rt,
    unsigned rd,
    unsigned sel,
    uint32_t instr_pc) {
    uint32_t fault_pc;
    uint32_t exception_pc;
    unsigned in_delay_slot;
    uint32_t value;

    if (!s || rt >= 32u || rd >= 32u) {
        return -2;
    }
    sr_cp0_delay_context(s, instr_pc, &fault_pc, &exception_pc, &in_delay_slot);
    if (!sr_cpu_in_kernel(s)) {
        return sr_cpu_raise_exception(
            s, SR_EXC_CPU, fault_pc, exception_pc, 0u, in_delay_slot, 0u);
    }
    if (sel != 0u || !sr_cp0_rd_supported(rd)) {
        return sr_cpu_raise_exception(
            s, SR_EXC_RI, fault_pc, exception_pc, 0u, in_delay_slot, 0u);
    }
    /* $zero reads as zero (spec 3.2). */
    value = rt == 0u ? 0u : s->r[rt];
    switch (rd) {
    case SR_CP0_STATUS:
        /* Reserved bits are preserved (spec 3.2). */
        s->cop0[SR_CP0_STATUS] =
            (s->cop0[SR_CP0_STATUS] & (uint32_t)~SR_STATUS_WRITABLE_MASK) |
            (value & SR_STATUS_WRITABLE_MASK);
        break;
    case SR_CP0_CAUSE:
        /* Hardware pending bits are INTC-owned; guest writes can only set the
         * software pair (spec 3.2). */
        s->cop0[SR_CP0_CAUSE] =
            (s->cop0[SR_CP0_CAUSE] & (uint32_t)~SR_CAUSE_IP_SW_MASK) |
            (value & SR_CAUSE_IP_SW_MASK);
        break;
    case SR_CP0_COUNT:
    case SR_CP0_COMPARE:
        /* Stored plainly: PR 5 (spec 6.2) connects these to the virtual-time
         * timer model. No frequency is fabricated here. */
        s->cop0[rd] = value;
        break;
    case SR_CP0_PRID:
    case SR_CP0_CONFIG:
        /* Read-only identities (spec 3.2). */
        return sr_cpu_raise_exception(
            s, SR_EXC_RI, fault_pc, exception_pc, 0u, in_delay_slot, 0u);
    default:
        s->cop0[rd] = value;
        break;
    }
    return 0;
}

int sr_cpu_eret(CpuState *s, uint32_t instr_pc) {
    uint32_t target;

    (void)instr_pc;
    if (!s) {
        return -2;
    }
    /* eret requires kernel/exception context; a user-mode eret traps with a
     * coprocessor exception carrying the faulting PC (spec 3.3). */
    if (!sr_cpu_in_kernel(s)) {
        uint32_t fault_pc;
        uint32_t exception_pc;
        unsigned in_delay_slot;
        sr_cp0_delay_context(s, instr_pc, &fault_pc, &exception_pc, &in_delay_slot);
        return sr_cpu_raise_exception(
            s, SR_EXC_CPU, fault_pc, exception_pc, 0u, in_delay_slot, 0u);
    }
    target = s->cop0[SR_CP0_EPC];
    /* Fail closed on an invalid target rather than jumping blind (spec 3.3).
     * Executable authority is validated here; readable-byte validation is
     * deferred until the dispatcher selects the interpreter tier. */
    if ((target & 3u) != 0u || !sr_exec_span_owns_fetch(target)) {
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = target;
        return -2;
    }
    s->cop0[SR_CP0_STATUS] &= (uint32_t)~SR_STATUS_EXL;
    s->pc = target;
    s->flow_kind = SR_FLOW_ERET;
    s->flow_target = target;
    return -1;
}
