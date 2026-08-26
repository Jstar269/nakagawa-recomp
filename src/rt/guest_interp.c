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

/* Every control-transfer encoding this file recognises, plus every control
 * encoding it deliberately does NOT implement.
 *
 * Both sets matter. The first drives the branch machinery below; the second
 * keeps an unimplemented transfer OUT of the straight-line executor, where its
 * bit fields would otherwise be decoded as some unrelated arithmetic form. A
 * control instruction is never "an opcode we happen not to know" -- misreading
 * one silently rewrites the program's shape. */
static int is_control_opcode(uint32_t opcode) {
    const uint32_t primary = opcode >> 26;
    if (primary == 0u) {
        const uint32_t funct = opcode & 0x3fu;
        return funct == 0x08u || funct == 0x09u;   /* jr, jalr */
    }
    switch (primary) {
    case 0x01u:                                     /* REGIMM b*z / b*zal */
    case 0x02u: case 0x03u:                         /* j, jal */
    case 0x04u: case 0x05u: case 0x06u: case 0x07u: /* beq, bne, blez, bgtz */
    case 0x14u: case 0x15u: case 0x16u: case 0x17u: /* the *l likely forms */
        return 1;
    case 0x11u: case 0x12u:                         /* the COP1/COP2 branch group */
        return ((opcode >> 21) & 31u) == 8u;
    default:
        return 0;
    }
}

/* Bounded guest memory access.
 *
 * Alignment is checked before the bounds test and both are checked before any
 * architectural effect, so a rejected access leaves CpuState and guest memory
 * exactly as they were. Sub-word widths use the same rule as the word forms
 * that already existed here: this layer is strictly more fail-closed than the
 * MEM_* accessors the generated code uses, which absorb an out-of-range access
 * through sr_oor() instead of stopping. */
static int aligned_for(uint32_t address, unsigned width) {
    return (address & (width - 1u)) == 0u;
}

/* Execute one non-control instruction.
 *
 * `store_address`/`store_size` report the instruction's guest store so the
 * caller can hand it to sr_end() -- the same (address, size) pair the generated
 * code passes -- keeping both execution lanes' instruction traces in the one
 * canonical format. */
static SrGuestInterpResult execute_noncontrol(
    CpuState *s,
    uint32_t pc,
    uint32_t opcode,
    uint32_t *store_address,
    int *store_size,
    SrGuestInterpFault *fault) {
    const uint32_t primary = opcode >> 26;
    const uint32_t rs = (opcode >> 21) & 31u;
    const uint32_t rt = (opcode >> 16) & 31u;
    const uint32_t rd = (opcode >> 11) & 31u;
    const uint32_t shift = (opcode >> 6) & 31u;

    *store_address = 0u;
    *store_size = 0;

    /* A transfer reaching the straight-line executor -- a branch in a delay
     * slot, or a form this file does not implement -- is rejected as itself,
     * never decoded as arithmetic. */
    if (is_control_opcode(opcode)) {
        set_fault(fault, pc, opcode, pc, 1);
        return SR_GUEST_INTERP_UNSUPPORTED;
    }

    if (primary == 0x00u) { /* SPECIAL */
        const uint32_t funct = opcode & 0x3fu;
        switch (funct) {
        case 0x00u: write_gpr(s, rd, read_gpr(s, rt) << shift); break;   /* sll */
        case 0x02u: write_gpr(s, rd, read_gpr(s, rt) >> shift); break;   /* srl */
        case 0x03u: /* sra */
            write_gpr(s, rd, (uint32_t)(sr_u32_as_s32(read_gpr(s, rt)) >> shift));
            break;
        case 0x10u: write_gpr(s, rd, s->hi); break;                      /* mfhi */
        case 0x12u: write_gpr(s, rd, s->lo); break;                      /* mflo */
        case 0x18u: { /* mult */
            const int64_t product = (int64_t)sr_u32_as_s32(read_gpr(s, rs)) *
                                    (int64_t)sr_u32_as_s32(read_gpr(s, rt));
            s->lo = (uint32_t)((uint64_t)product & 0xffffffffu);
            s->hi = (uint32_t)((uint64_t)product >> 32);
            break;
        }
        case 0x19u: { /* multu */
            const uint64_t product = (uint64_t)read_gpr(s, rs) * (uint64_t)read_gpr(s, rt);
            s->lo = (uint32_t)(product & 0xffffffffu);
            s->hi = (uint32_t)(product >> 32);
            break;
        }
        case 0x21u: write_gpr(s, rd, read_gpr(s, rs) + read_gpr(s, rt)); break;  /* addu */
        case 0x23u: write_gpr(s, rd, read_gpr(s, rs) - read_gpr(s, rt)); break;  /* subu */
        case 0x24u: write_gpr(s, rd, read_gpr(s, rs) & read_gpr(s, rt)); break;  /* and */
        case 0x25u: write_gpr(s, rd, read_gpr(s, rs) | read_gpr(s, rt)); break;  /* or */
        case 0x26u: write_gpr(s, rd, read_gpr(s, rs) ^ read_gpr(s, rt)); break;  /* xor */
        case 0x2au: /* slt */
            write_gpr(s, rd,
                      sr_u32_as_s32(read_gpr(s, rs)) < sr_u32_as_s32(read_gpr(s, rt))
                          ? 1u : 0u);
            break;
        case 0x2bu: /* sltu */
            write_gpr(s, rd, read_gpr(s, rs) < read_gpr(s, rt) ? 1u : 0u);
            break;
        default:
            set_fault(fault, pc, opcode, pc, 1);
            return SR_GUEST_INTERP_UNSUPPORTED;
        }
        s->r[0] = 0u;
        return SR_GUEST_INTERP_AOT_HANDOFF;
    }

    if (primary == 0x11u) { /* COP1 -- scalar FPU */
        const uint32_t fmt = rs;
        const uint32_t ft = rt;
        const uint32_t fs = rd;
        const uint32_t fd = shift;
        const uint32_t funct = opcode & 0x3fu;
        if (fmt == 0x00u) {                       /* mfc1 rt, fs */
            write_gpr(s, ft, s->fi[fs]);
        } else if (fmt == 0x04u) {                /* mtc1 rt, fs */
            s->fi[fs] = read_gpr(s, ft);
        } else if (fmt == 0x10u && funct == 0x00u) {   /* add.s */
            s->f[fd] = sr_fpu_add_s(s->f[fs], s->f[ft], s->fcr31);
        } else if (fmt == 0x10u && funct == 0x02u) {   /* mul.s */
            /* Classify the inf*0 case from RAW bits, exactly as the generated
             * code does: a floating precheck would itself be a guest-sensitive
             * comparison outside the scoped FP window (see fp_convert.h). */
            const uint32_t a_bits = s->fi[fs];
            const uint32_t b_bits = s->fi[ft];
            if (((a_bits & 0x7fffffffu) == 0x7f800000u && (b_bits & 0x7fffffffu) == 0u) ||
                ((b_bits & 0x7fffffffu) == 0x7f800000u && (a_bits & 0x7fffffffu) == 0u)) {
                s->fi[fd] = 0x7fc00000u;
            } else {
                s->f[fd] = sr_fpu_mul_s(s->f[fs], s->f[ft], s->fcr31);
            }
        } else if (fmt == 0x10u && funct == 0x24u) {   /* cvt.w.s */
            s->fi[fd] = sr_fpu_to_word(s->f[fs], funct, s->fcr31);
        } else {
            set_fault(fault, pc, opcode, pc, 1);
            return SR_GUEST_INTERP_UNSUPPORTED;
        }
        s->r[0] = 0u;
        return SR_GUEST_INTERP_AOT_HANDOFF;
    }

    /* Immediate ALU. */
    if (primary == 0x09u) {        /* addiu rt, rs, immediate */
        write_gpr(s, rt, read_gpr(s, rs) + sign_extend_16(opcode));
        s->r[0] = 0u;
        return SR_GUEST_INTERP_AOT_HANDOFF;
    }
    if (primary == 0x0du) {        /* ori rt, rs, immediate (zero extended) */
        write_gpr(s, rt, read_gpr(s, rs) | (opcode & 0xffffu));
        s->r[0] = 0u;
        return SR_GUEST_INTERP_AOT_HANDOFF;
    }
    if (primary == 0x0fu) {        /* lui rt, immediate */
        write_gpr(s, rt, (opcode & 0xffffu) << 16);
        s->r[0] = 0u;
        return SR_GUEST_INTERP_AOT_HANDOFF;
    }

    /* Loads and stores. Width, signedness and the FPU register file are all
     * selected here so one bounds/alignment contract covers every form. */
    {
        unsigned width = 0u;
        int is_store = 0;
        switch (primary) {
        case 0x20u: case 0x24u: case 0x28u: width = 1u; is_store = primary == 0x28u; break;
        case 0x21u: case 0x25u: case 0x29u: width = 2u; is_store = primary == 0x29u; break;
        case 0x23u: case 0x2bu: case 0x31u: case 0x39u:
            width = 4u;
            is_store = primary == 0x2bu || primary == 0x39u;
            break;
        default: break;
        }
        if (width != 0u) {
            const uint32_t address = read_gpr(s, rs) + sign_extend_16(opcode);
            if (!aligned_for(address, width)) {
                set_fault(fault, pc, opcode, address, 1);
                return SR_GUEST_INTERP_MISALIGNED_DATA;
            }
            if (is_store) {
                if (!sr_guest_span_writable(address, width)) {
                    set_fault(fault, pc, opcode, address, 1);
                    return SR_GUEST_INTERP_MEMORY_FAULT;
                }
                switch (primary) {
                case 0x28u: MEM_W8_PC(address, read_gpr(s, rt), pc); break;
                case 0x29u: MEM_W16_PC(address, read_gpr(s, rt), pc); break;
                case 0x2bu: MEM_W32_PC(address, read_gpr(s, rt), pc); break;
                default:    MEM_W32_PC(address, s->fi[rt], pc); break;  /* swc1 */
                }
                *store_address = address;
                *store_size = (int)width;
            } else {
                if (!sr_guest_span_readable(address, width)) {
                    set_fault(fault, pc, opcode, address, 1);
                    return SR_GUEST_INTERP_MEMORY_FAULT;
                }
                switch (primary) {
                case 0x20u: /* lb -- sign extended */
                    write_gpr(s, rt, (uint32_t)(int32_t)(int8_t)MEM_R8(address));
                    break;
                case 0x24u: /* lbu */
                    write_gpr(s, rt, MEM_R8(address));
                    break;
                case 0x21u: /* lh -- sign extended */
                    write_gpr(s, rt, (uint32_t)(int32_t)(int16_t)MEM_R16(address));
                    break;
                case 0x25u: /* lhu */
                    write_gpr(s, rt, MEM_R16(address));
                    break;
                case 0x23u: /* lw */
                    write_gpr(s, rt, MEM_R32(address));
                    break;
                default:    /* lwc1 */
                    s->fi[rt] = MEM_R32(address);
                    break;
                }
            }
            s->r[0] = 0u;
            return SR_GUEST_INTERP_AOT_HANDOFF;
        }
    }

    set_fault(fault, pc, opcode, pc, 1);
    return SR_GUEST_INTERP_UNSUPPORTED;
}

/* One decoded control transfer.
 *
 * `taken` is resolved from the register file AS IT STANDS AT THE BRANCH, before
 * the link write and before the delay slot runs. `target` is likewise read at
 * the branch: a delay slot that overwrites a jr/jalr target register must not
 * change where control goes. Both are architectural requirements, and both are
 * only expressible by decoding the transfer before its slot -- which is why
 * this is a separate step rather than part of the executor above. */
typedef struct SrGuestInterpTransfer {
    uint32_t target;
    int link_register;   /* -1 when the form does not link */
    int taken;
} SrGuestInterpTransfer;

typedef enum SrGuestInterpCtl {
    SR_CTL_NONE     = 0,   /* straight-line instruction */
    SR_CTL_TRANSFER = 1,   /* decoded, with or without the branch taken */
    SR_CTL_REJECT   = 2,   /* a control form this file does not implement */
} SrGuestInterpCtl;

static SrGuestInterpCtl classify_control(
    const CpuState *s,
    uint32_t pc,
    uint32_t opcode,
    SrGuestInterpTransfer *out) {
    const uint32_t primary = opcode >> 26;
    const uint32_t rs = (opcode >> 21) & 31u;
    const uint32_t rt = (opcode >> 16) & 31u;
    const uint32_t rd = (opcode >> 11) & 31u;

    out->target = 0u;
    out->link_register = -1;
    out->taken = 0;

    if (!is_control_opcode(opcode)) {
        return SR_CTL_NONE;
    }

    if (primary == 0x00u) {
        const uint32_t funct = opcode & 0x3fu;
        if (funct == 0x08u && (opcode & 0x001fffc0u) == 0u) {  /* jr rs */
            out->target = read_gpr(s, rs);
            out->taken = 1;
            return SR_CTL_TRANSFER;
        }
        if (funct == 0x09u && (opcode & 0x001f07c0u) == 0u) {  /* jalr rd, rs */
            out->target = read_gpr(s, rs);
            /* rd == 0 links nowhere; the generated code omits the write for the
             * same encoding, so the two lanes agree by construction. */
            out->link_register = rd == 0u ? -1 : (int)rd;
            out->taken = 1;
            return SR_CTL_TRANSFER;
        }
        return SR_CTL_REJECT;
    }

    if (primary == 0x02u || primary == 0x03u) {  /* j / jal */
        out->target = ((pc + 4u) & 0xf0000000u) | ((opcode & 0x03ffffffu) << 2);
        out->link_register = primary == 0x03u ? 31 : -1;
        out->taken = 1;
        return SR_CTL_TRANSFER;
    }

    if (primary == 0x04u || primary == 0x05u) {  /* beq / bne */
        const uint32_t left = read_gpr(s, rs);
        const uint32_t right = read_gpr(s, rt);
        out->target = pc + 4u + (sign_extend_16(opcode) << 2);
        out->taken = primary == 0x04u ? (left == right) : (left != right);
        return SR_CTL_TRANSFER;
    }

    return SR_CTL_REJECT;
}

SrGuestInterpResult sr_guest_interp_run(
    CpuState *s,
    uint32_t entry,
    SrGuestInterpFault *fault) {
    uint32_t pc = entry;
    unsigned long long instruction_count = 0u;
    const int log_dispatch = getenv("SR_DISPLOG") != NULL;
    set_fault(fault, entry, 0u, entry, 0);

    if (log_dispatch) {
        fprintf(stderr,
                "GUEST_INTERP_ENTER entry=0x%08x caller_pc=0x%08x ra=0x%08x\n",
                entry, s->pc, s->r[31]);
    }

    for (;;) {
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

        SrGuestInterpTransfer transfer;
        const SrGuestInterpCtl control = classify_control(s, pc, opcode, &transfer);
        if (control == SR_CTL_REJECT) {
            set_fault(fault, pc, opcode, pc, 1);
            return SR_GUEST_INTERP_UNSUPPORTED;
        }

        if (control == SR_CTL_TRANSFER) {
            /* The branch owns its delay slot: the AOT tier is NOT reconsulted at
             * pc+4, and a handoff is permitted only once the slot has completed.
             * A control transfer inside the slot is rejected by the executor
             * before any effect is applied. */
            if (transfer.taken && (transfer.target & 3u) != 0u) {
                set_fault(fault, pc, opcode, transfer.target, 1);
                return SR_GUEST_INTERP_MISALIGNED_PC;
            }
            if (pc > UINT32_MAX - 8u) {
                set_fault(fault, pc, opcode, pc, 1);
                return SR_GUEST_INTERP_FETCH_BOUNDARY;
            }

            uint32_t delay_opcode = 0u;
            SrGuestInterpResult delay_fetch =
                fetch_instruction(pc + 4u, &delay_opcode, fault);
            if (delay_fetch < 0) {
                return delay_fetch;
            }

            /* Emission order matches the generated code exactly -- the branch
             * reports (with its link write) before its delay slot -- so the two
             * lanes produce byte-comparable instruction traces. */
            sr_begin(s, pc, opcode);
            if (transfer.link_register >= 0) {
                write_gpr(s, (uint32_t)transfer.link_register, pc + 8u);
                s->r[0] = 0u;
            }
            sr_end(s, 0u, 0);

            uint32_t delay_store_address = 0u;
            int delay_store_size = 0;
            sr_begin(s, pc + 4u, delay_opcode);
            SrGuestInterpResult delay_result = execute_noncontrol(
                s, pc + 4u, delay_opcode, &delay_store_address, &delay_store_size, fault);
            if (delay_result < 0) {
                return delay_result;
            }
            sr_end(s, delay_store_address, delay_store_size);

            instruction_count += 2u;
            pc = transfer.taken ? transfer.target : pc + 8u;
            s->pc = pc;
            continue;
        }

        uint32_t store_address = 0u;
        int store_size = 0;
        sr_begin(s, pc, opcode);
        SrGuestInterpResult result = execute_noncontrol(
            s, pc, opcode, &store_address, &store_size, fault);
        if (result < 0) {
            return result;
        }
        sr_end(s, store_address, store_size);
        instruction_count++;
        pc += 4u;
        s->pc = pc;
    }
}

const char *sr_guest_interp_result_name(SrGuestInterpResult result) {
    switch (result) {
    case SR_GUEST_INTERP_AOT_HANDOFF: return "aot-handoff";
    case SR_GUEST_INTERP_NOT_EXECUTABLE: return "not-executable";
    case SR_GUEST_INTERP_MISALIGNED_PC: return "misaligned-pc";
    case SR_GUEST_INTERP_FETCH_BOUNDARY: return "fetch-boundary";
    case SR_GUEST_INTERP_UNSUPPORTED: return "unsupported-opcode";
    case SR_GUEST_INTERP_MEMORY_FAULT: return "memory-fault";
    case SR_GUEST_INTERP_MISALIGNED_DATA: return "misaligned-data";
    default: return "unknown";
    }
}
