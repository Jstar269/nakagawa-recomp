// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#ifndef NAKAGAWA_GUEST_INTERP_H
#define NAKAGAWA_GUEST_INTERP_H

#include <stdint.h>

#include "recomp.h"

typedef enum SrGuestInterpResult {
    SR_GUEST_INTERP_AOT_HANDOFF       = 1,
    SR_GUEST_INTERP_NOT_EXECUTABLE    = -1,
    SR_GUEST_INTERP_MISALIGNED_PC     = -2,
    SR_GUEST_INTERP_FETCH_BOUNDARY    = -3,
    SR_GUEST_INTERP_UNSUPPORTED       = -4,
    SR_GUEST_INTERP_MEMORY_FAULT      = -5,
    SR_GUEST_INTERP_MISALIGNED_DATA   = -6,
} SrGuestInterpResult;

typedef struct SrGuestInterpFault {
    uint32_t pc;
    uint32_t opcode;
    uint32_t address;
    int opcode_valid;
} SrGuestInterpFault;

/* Execute from entry until control reaches a registered AOT destination or a
 * fail-closed boundary. There is deliberately no instruction-count escape hatch:
 * a guest loop is guest behavior, not permission to fabricate completion.
 *
 * IMPLEMENTED INSTRUCTION SET
 * ---------------------------
 * Every form below exists because a cell in fixtures/cosim executes it and its
 * result is compared against real codegen output; nothing here was added to
 * inflate coverage, and tools/test_cosim_fixture.py enforces that in both
 * directions. Anything else -- including every control-transfer form not listed
 * -- fails closed as SR_GUEST_INTERP_UNSUPPORTED rather than being decoded as
 * some unrelated arithmetic form.
 *
 *   ALU        sll srl sra addu subu and or xor slt sltu / addiu ori lui
 *   HI/LO      mult multu mfhi mflo
 *   memory     lb lbu lh lhu lw / sb sh sw / lwc1 swc1
 *   control    beq bne j jal jr jalr
 *   scalar FPU mfc1 mtc1 add.s mul.s cvt.w.s   (via the src/rt/fp_convert.h
 *              helpers the generated code also calls, so guest FCR31 selects
 *              the result in both execution lanes)
 *
 * While SR_INSTRUCTION_TRACE is compiled in, execution emits the canonical
 * per-instruction trace (tools/TRACE_FORMAT.md) through the same sr_begin/sr_end
 * hooks the generated code uses, in the same branch-before-delay-slot order. */
SrGuestInterpResult sr_guest_interp_run(
    CpuState *s,
    uint32_t entry,
    SrGuestInterpFault *fault);

const char *sr_guest_interp_result_name(SrGuestInterpResult result);

#endif
