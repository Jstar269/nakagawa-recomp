// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include "guest_interp.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct SrExecSpan {
    uint32_t start;
    uint32_t end;
} SrExecSpan;

static SrExecSpan *s_exec_spans;
static size_t s_exec_span_count;
static size_t s_exec_span_capacity;

/* Two distinct qualifications keep guest control flow lawful:
 *
 * 1. EXECUTABLE AUTHORITY -- membership in an analyzer-owned, end-exclusive
 *    span registered below. Codegen derives these spans from the primary image
 *    and every extra module it actually translated. Authority is structural:
 *    alignment and a complete four-byte fetch slot. It deliberately does NOT
 *    require the flat interpreter arena to back the bytes: extra modules are
 *    translated at build time from their own files and are never copied into
 *    guest RAM, so their addresses (e.g. 0x32200000-class load slots) are
 *    owned here while being unreachable through SR_PHYS/SR_HOST.
 * 2. INTERPRETER FETCH BACKING -- sr_guest_span_readable() over the actual
 *    instruction word. Only interpreting bytes needs this; entering an already
 *    translated native body does not.
 *
 * Mapped RAM alone grants neither tier, and native registration alone grants
 * neither tier. */
void sr_exec_span_reset(void) {
    free(s_exec_spans);
    s_exec_spans = NULL;
    s_exec_span_count = 0;
    s_exec_span_capacity = 0;
}

int sr_exec_span_register(uint32_t start, uint32_t end) {
    if ((start & 3u) != 0u || end <= start) {
        return 0;
    }

    for (size_t i = 0; i < s_exec_span_count; i++) {
        if (s_exec_spans[i].start == start && s_exec_spans[i].end == end) {
            return 1;
        }
    }

    if (s_exec_span_count == s_exec_span_capacity) {
        size_t next_capacity = s_exec_span_capacity == 0u ? 4u : s_exec_span_capacity * 2u;
        if (next_capacity < s_exec_span_capacity ||
            next_capacity > SIZE_MAX / sizeof(*s_exec_spans)) {
            return 0;
        }
        SrExecSpan *next = realloc(s_exec_spans, next_capacity * sizeof(*next));
        if (!next) {
            return 0;
        }
        s_exec_spans = next;
        s_exec_span_capacity = next_capacity;
    }

    s_exec_spans[s_exec_span_count].start = start;
    s_exec_spans[s_exec_span_count].end = end;
    s_exec_span_count++;
    return 1;
}

/* Authority tier: the PC is 4-byte aligned and one registered span covers its
 * complete fetch slot. This is the predicate sr_lookup() applies before
 * entering a native body; whether the arena can actually supply those bytes
 * matters only when the interpreter itself must fetch them. */
int sr_exec_span_owns_fetch(uint32_t pc) {
    if ((pc & 3u) != 0u) {
        return 0;
    }
    for (size_t i = 0; i < s_exec_span_count; i++) {
        const SrExecSpan *span = &s_exec_spans[i];
        if (pc >= span->start && pc < span->end && span->end - pc >= 4u) {
            return 1;
        }
    }
    return 0;
}

static void set_fault(
    SrGuestInterpFault *fault,
    uint32_t pc,
    uint32_t opcode,
    uint32_t address,
    int opcode_valid) {
    if (!fault) {
        return;
    }
    fault->pc = pc;
    fault->opcode = opcode;
    fault->address = address;
    fault->opcode_valid = opcode_valid;
}

/* Fetch tier: interpretation needs the actual instruction word, so this is the
 * only place that couples executable authority with arena-backed readability.
 * An owned but unbacked PC fails closed here as a precise memory fault instead
 * of reading fabricated or out-of-arena bytes. */
static SrGuestInterpResult fetch_instruction(
    uint32_t pc,
    uint32_t *opcode,
    SrGuestInterpFault *fault) {
    if ((pc & 3u) != 0u) {
        set_fault(fault, pc, 0u, pc, 0);
        return SR_GUEST_INTERP_MISALIGNED_PC;
    }

    int touches_span_boundary = 0;
    for (size_t i = 0; i < s_exec_span_count; i++) {
        const SrExecSpan *span = &s_exec_spans[i];
        if (pc < span->start || pc > span->end) {
            continue;
        }
        touches_span_boundary = 1;
        if (pc < span->end && span->end - pc >= 4u) {
            if (!sr_guest_span_readable(pc, 4u)) {
                set_fault(fault, pc, 0u, pc, 0);
                return SR_GUEST_INTERP_MEMORY_FAULT;
            }
            *opcode = MEM_R32(pc);
            return SR_GUEST_INTERP_AOT_HANDOFF;
        }
    }

    set_fault(fault, pc, 0u, pc, 0);
    return touches_span_boundary ? SR_GUEST_INTERP_FETCH_BOUNDARY
                                 : SR_GUEST_INTERP_NOT_EXECUTABLE;
}

static uint32_t read_gpr(const CpuState *s, uint32_t index) {
    return index == 0u ? 0u : s->r[index];
}

static void write_gpr(CpuState *s, uint32_t index, uint32_t value) {
    if (index != 0u) {
        s->r[index] = value;
    }
}

static uint32_t sign_extend_16(uint32_t opcode) {
    uint32_t value = opcode & 0xffffu;
    return (value & 0x8000u) != 0u ? value | 0xffff0000u : value;
}

static SrGuestInterpResult execute_noncontrol(
    CpuState *s,
    uint32_t pc,
    uint32_t opcode,
    SrGuestInterpFault *fault) {
    uint32_t primary = opcode >> 26;
    uint32_t rs = (opcode >> 21) & 31u;
    uint32_t rt = (opcode >> 16) & 31u;

    if (primary == 0x09u) { /* ADDIU rt, rs, immediate */
        uint32_t value = read_gpr(s, rs) + sign_extend_16(opcode);
        write_gpr(s, rt, value);
    } else if (primary == 0x0fu) { /* LUI rt, immediate */
        write_gpr(s, rt, (opcode & 0xffffu) << 16);
    } else if (primary == 0x2bu) { /* SW rt, immediate(rs) */
        uint32_t address = read_gpr(s, rs) + sign_extend_16(opcode);
        if ((address & 3u) != 0u) {
            set_fault(fault, pc, opcode, address, 1);
            return SR_GUEST_INTERP_MISALIGNED_DATA;
        }
        if (!sr_guest_span_writable(address, 4u)) {
            set_fault(fault, pc, opcode, address, 1);
            return SR_GUEST_INTERP_MEMORY_FAULT;
        }
        MEM_W32_PC(address, read_gpr(s, rt), pc);
    } else if (primary == 0x23u) { /* LW rt, immediate(rs) */
        uint32_t address = read_gpr(s, rs) + sign_extend_16(opcode);
        if ((address & 3u) != 0u) {
            set_fault(fault, pc, opcode, address, 1);
            return SR_GUEST_INTERP_MISALIGNED_DATA;
        }
        if (!sr_guest_span_readable(address, 4u)) {
            set_fault(fault, pc, opcode, address, 1);
            return SR_GUEST_INTERP_MEMORY_FAULT;
        }
        uint32_t value = MEM_R32(address);
        write_gpr(s, rt, value);
    } else {
        set_fault(fault, pc, opcode, pc, 1);
        return SR_GUEST_INTERP_UNSUPPORTED;
    }

    s->r[0] = 0u;
    return SR_GUEST_INTERP_AOT_HANDOFF;
}

static SrGuestInterpResult sr_guest_interp_run_internal(
    CpuState *s,
    uint32_t entry,
    const SrGuestInterpCallBoundary *boundary,
    SrGuestInterpFault *fault) {
    uint32_t pc = entry;
    unsigned long long instruction_count = 0u;
    const int log_dispatch = getenv("SR_DISPLOG") != NULL;
    set_fault(fault, entry, 0u, entry, 0);

    if (boundary && (boundary->resume_pc & 3u) != 0u) {
        set_fault(fault, boundary->resume_pc, 0u, boundary->resume_pc, 0);
        return SR_GUEST_INTERP_MISALIGNED_PC;
    }

    if (log_dispatch) {
        fprintf(stderr,
                "GUEST_INTERP_ENTER entry=0x%08x caller_pc=0x%08x ra=0x%08x\n",
                entry, s->pc, s->r[31]);
    }

    for (;;) {
        /* A CALL boundary is an explicit execution contract, not an inference from
         * the live $ra.  Check it before AOT lookup/fetch so a continuation that is
         * itself registered is handed back to the still-live native caller rather
         * than entered a second time.  The instruction count guard keeps a malformed
         * self-call from returning before its callee executes any instruction. */
        if (boundary && instruction_count != 0u && pc == boundary->resume_pc) {
            s->pc = pc;
            if (log_dispatch) {
                fprintf(stderr,
                        "GUEST_INTERP_CALL_RETURN pc=0x%08x instructions=%llu\n",
                        pc, instruction_count);
            }
            return SR_GUEST_INTERP_CALL_RETURN;
        }

        if ((pc & 3u) != 0u) {
            set_fault(fault, pc, 0u, pc, 0);
            return SR_GUEST_INTERP_MISALIGNED_PC;
        }

        /* Tier selection precedes byte fetching. sr_lookup() requires complete
         * executable ownership, so a registered native body may be entered even
         * when no arena bytes back the PC: the translation itself embodies those
         * instructions (build-time-translated modules). Only the interpreted tier
         * below needs readable bytes. */
        if (sr_lookup(pc)) {
            s->pc = pc;
            if (log_dispatch) {
                fprintf(stderr,
                        "GUEST_INTERP_AOT_HANDOFF pc=0x%08x instructions=%llu\n",
                        pc, instruction_count);
            }
            dispatch(s, pc);
            return SR_GUEST_INTERP_AOT_HANDOFF;
        }

        uint32_t opcode = 0u;
        SrGuestInterpResult fetch_result = fetch_instruction(pc, &opcode, fault);
        if (fetch_result < 0) {
            return fetch_result;
        }

        /* JR and direct J have one architectural delay slot. The branch owns
         * that slot: do not reevaluate the AOT tier at pc+4, and permit handoff
         * only after the slot completes. A control transfer in the slot remains
         * outside this first slice and is rejected before applying any effect. */
        const int is_jr = (opcode & 0xfc1fffffu) == 0x00000008u;
        const int is_j = (opcode >> 26) == 0x02u;
        if (is_jr || is_j) {
            uint32_t target = is_jr
                ? read_gpr(s, (opcode >> 21) & 31u)
                : (((pc + 4u) & 0xf0000000u) | ((opcode & 0x03ffffffu) << 2));
            if ((target & 3u) != 0u) {
                set_fault(fault, pc, opcode, target, 1);
                return SR_GUEST_INTERP_MISALIGNED_PC;
            }
            if (pc > UINT32_MAX - 4u) {
                set_fault(fault, pc, opcode, pc, 1);
                return SR_GUEST_INTERP_FETCH_BOUNDARY;
            }

            uint32_t delay_opcode = 0u;
            SrGuestInterpResult delay_fetch = fetch_instruction(pc + 4u, &delay_opcode, fault);
            if (delay_fetch < 0) {
                return delay_fetch;
            }
            SrGuestInterpResult delay_result =
                execute_noncontrol(s, pc + 4u, delay_opcode, fault);
            if (delay_result < 0) {
                return delay_result;
            }
            instruction_count += 2u;
            pc = target;
            s->pc = pc;
            continue;
        }

        SrGuestInterpResult result = execute_noncontrol(s, pc, opcode, fault);
        if (result < 0) {
            return result;
        }
        instruction_count++;
        pc += 4u;
        s->pc = pc;
    }
}

SrGuestInterpResult sr_guest_interp_run(
    CpuState *s,
    uint32_t entry,
    SrGuestInterpFault *fault) {
    return sr_guest_interp_run_internal(s, entry, NULL, fault);
}

SrGuestInterpResult sr_guest_interp_run_with_boundary(
    CpuState *s,
    uint32_t entry,
    const SrGuestInterpCallBoundary *boundary,
    SrGuestInterpFault *fault) {
    return sr_guest_interp_run_internal(s, entry, boundary, fault);
}

const char *sr_guest_interp_result_name(SrGuestInterpResult result) {
    switch (result) {
    case SR_GUEST_INTERP_AOT_HANDOFF: return "aot-handoff";
    case SR_GUEST_INTERP_CALL_RETURN: return "call-return";
    case SR_GUEST_INTERP_NOT_EXECUTABLE: return "not-executable";
    case SR_GUEST_INTERP_MISALIGNED_PC: return "misaligned-pc";
    case SR_GUEST_INTERP_FETCH_BOUNDARY: return "fetch-boundary";
    case SR_GUEST_INTERP_UNSUPPORTED: return "unsupported-opcode";
    case SR_GUEST_INTERP_MEMORY_FAULT: return "memory-fault";
    case SR_GUEST_INTERP_MISALIGNED_DATA: return "misaligned-data";
    default: return "unknown";
    }
}
