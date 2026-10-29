/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Source-owned microprograms for the instruction-trace differential (#312).

   Each fixture is a run of raw Allegrex words that the insn-trace probe executes one
   instruction at a time on a PSP (probe_insn_trace.c) and that tools/psp_oracle/
   insn_trace.py recompiles at the same address with tools/codegen.py; both sides write a
   v2 trace (tools/TRACE_FORMAT.md) and the strict comparator reports the first record
   that differs. The words are written out with their assembly so the fixture needs no
   assembler; tools/test_insn_trace.py decodes every word back against its comment.

   Contract shared by both sides (tools/psp_oracle/insn_trace.py reads this file):
   - a fixture starts at its base address and ends when execution reaches base + 4 *
     count (the end address; the word there is not executed);
   - registers start as r[i] = 0x01010101 * i, except r4 = NT_DATA_BASE (the data
     pointer), r29 = NT_DATA_BASE + 0x800, and r26, r27, r28 = 0; hi = NT_HI_INIT,
     lo = NT_LO_INIT;
   - the NT_DATA_BYTES bytes at NT_DATA_BASE start zero and are the only memory a
     fixture reads or writes;
   - no word names r26, r27 or r28 (the probe's trampoline holds its state pointer in
     r28), no word is a coprocessor, trap, syscall or likely-in-delay-slot form, and no
     delay slot holds a control transfer. */

#ifndef NT_INSN_TRACE_FIXTURES_H
#define NT_INSN_TRACE_FIXTURES_H

#include <stdint.h>

#define NT_DATA_BASE 0x09f00000u
#define NT_DATA_BYTES 0x1000u
#define NT_HI_INIT 0x0badf00du
#define NT_LO_INIT 0x13579bdfu

/* alu-branch-ls: a five-pass loop of ALU, shift, compare, multiply and every load/store
   width, closed by bne with a delay slot, then a signed load, a variable shift, nor and
   an unsigned divide. */
static const uint32_t nt_alu_branch_ls_words[] = {
    0x24080005u, /* addiu $8, $0, 5 */
    0x3c091234u, /* lui $9, 0x1234 */
    0x35295678u, /* ori $9, $9, 0x5678 */
    0x00005021u, /* addu $10, $0, $0 */
    0x01495021u, /* addu $10, $10, $9 */
    0xac8a0000u, /* sw $10, 0($4) */
    0x8c8b0000u, /* lw $11, 0($4) */
    0xa08b0004u, /* sb $11, 4($4) */
    0x908c0004u, /* lbu $12, 4($4) */
    0xa48c0006u, /* sh $12, 6($4) */
    0x848d0006u, /* lh $13, 6($4) */
    0x000b70c0u, /* sll $14, $11, 3 */
    0x00097903u, /* sra $15, $9, 4 */
    0x01cf8026u, /* xor $16, $14, $15 */
    0x01ee882au, /* slt $17, $15, $14 */
    0x01ee902bu, /* sltu $18, $15, $14 */
    0x01690018u, /* mult $11, $9 */
    0x00009810u, /* mfhi $19 */
    0x0000a012u, /* mflo $20 */
    0x24840008u, /* addiu $4, $4, 8 */
    0x2508ffffu, /* addiu $8, $8, -1 */
    0x1500ffeeu, /* bne $8, $0, -18 */
    0x0211a823u, /* subu $21, $16, $17 */
    0x8096fffcu, /* lb $22, -4($4) */
    0x0109b806u, /* srlv $23, $9, $8 */
    0x012ac027u, /* nor $24, $9, $10 */
    0x012b001bu, /* divu $9, $11 */
    0x0000c810u, /* mfhi $25 */
    0x00001012u, /* mflo $2 */
};

/* delay-slots: a delay slot that changes the branch's own operand, beql not taken (its
   delay slot annulled), bnel taken, bal and bltzal into subroutines that return with jr,
   bgez reading its operand before the delay slot overwrites it, bltzl taken, blez taken
   with a store in its delay slot, bgtz not taken, and an unconditional b. */
static const uint32_t nt_delay_slots_words[] = {
    0x24080001u, /* addiu $8, $0, 1 */
    0x11000018u, /* beq $8, $0, 24 */
    0x25080001u, /* addiu $8, $8, 1 */
    0x51000016u, /* beql $8, $0, 22 */
    0x25080064u, /* addiu $8, $8, 100 */
    0x55000002u, /* bnel $8, $0, 2 */
    0x25090007u, /* addiu $9, $8, 7 */
    0x252903e8u, /* addiu $9, $9, 1000 */
    0x04110014u, /* bgezal $0, 20 (bal) */
    0x240a0003u, /* addiu $10, $0, 3 */
    0x05010002u, /* bgez $8, 2 */
    0x2408fffbu, /* addiu $8, $0, -5 */
    0x240b0063u, /* addiu $11, $0, 99 */
    0x05020002u, /* bltzl $8, 2 */
    0x240c000cu, /* addiu $12, $0, 12 */
    0x240d000du, /* addiu $13, $0, 13 */
    0x19000002u, /* blez $8, 2 */
    0xac880000u, /* sw $8, 0($4) */
    0x240e000eu, /* addiu $14, $0, 14 */
    0x8c8f0000u, /* lw $15, 0($4) */
    0x1de00005u, /* bgtz $15, 5 */
    0x24100010u, /* addiu $16, $0, 16 */
    0x05f00009u, /* bltzal $15, 9 */
    0x24110011u, /* addiu $17, $0, 17 */
    0x1000000au, /* beq $0, $0, 10 (b) */
    0x24120012u, /* addiu $18, $0, 18 */
    0x24130013u, /* addiu $19, $0, 19 */
    0x10000007u, /* beq $0, $0, 7 (b) */
    0x00000000u, /* sll $0, $0, 0 (nop) */
    0x01495821u, /* addu $11, $10, $9 */
    0x03e00008u, /* jr $31 */
    0x256c0001u, /* addiu $12, $11, 1 */
    0x27f40000u, /* addiu $20, $31, 0 */
    0x03e00008u, /* jr $31 */
    0x24150015u, /* addiu $21, $0, 21 */
};

typedef struct NtFixture {
    const char *id;
    uint32_t base;
    uint32_t count;
    const uint32_t *words;
} NtFixture;

#define NT_FIXTURE_COUNT 2u

static const NtFixture nt_fixtures[NT_FIXTURE_COUNT] = {
    {"alu-branch-ls", 0x08900000u, 29u, nt_alu_branch_ls_words},
    {"delay-slots", 0x08910000u, 35u, nt_delay_slots_words},
};

#endif
