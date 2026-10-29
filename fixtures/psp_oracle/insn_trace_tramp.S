/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Single-step trampoline for the insn-trace probe (probe_insn_trace.c, #312).

   void nt_tramp(NtState *st): saves the caller's callee-saved registers, sp, ra and gp
   in st->host, points gp at st, loads every traced guest register (r1..r25, r29..r31,
   hi and lo) from st, executes the two words at nt_tramp_slot, stores the registers
   back with flag 0, restores the caller and returns. The probe copies the template to
   a buffer and patches the slot: an ordinary instruction and a nop, or a conditional
   branch whose offset is rewritten to nt_tramp_taken and a nop, in which case a taken
   branch skips the stores and sets flag to a nonzero value (the probe keeps its own copy
   of the state for branches). k0, k1 and gp are never loaded: the fixtures do not use
   them, and gp holds st while guest registers are live.

   NtState layout (bytes): r[32] at 0, hi 128, lo 132, flag 136, host[12] at 140. */

    .set noreorder
    .set noat
    .text
    .align 4
    .global nt_tramp_begin
    .global nt_tramp_slot
    .global nt_tramp_taken
    .global nt_tramp_end
nt_tramp_begin:
    sw    $16, 140($4)
    sw    $17, 144($4)
    sw    $18, 148($4)
    sw    $19, 152($4)
    sw    $20, 156($4)
    sw    $21, 160($4)
    sw    $22, 164($4)
    sw    $23, 168($4)
    sw    $30, 172($4)
    sw    $29, 176($4)
    sw    $31, 180($4)
    sw    $28, 184($4)
    move  $28, $4
    lw    $1, 128($28)
    mthi  $1
    lw    $1, 132($28)
    mtlo  $1
    lw    $1, 4($28)
    lw    $2, 8($28)
    lw    $3, 12($28)
    lw    $4, 16($28)
    lw    $5, 20($28)
    lw    $6, 24($28)
    lw    $7, 28($28)
    lw    $8, 32($28)
    lw    $9, 36($28)
    lw    $10, 40($28)
    lw    $11, 44($28)
    lw    $12, 48($28)
    lw    $13, 52($28)
    lw    $14, 56($28)
    lw    $15, 60($28)
    lw    $16, 64($28)
    lw    $17, 68($28)
    lw    $18, 72($28)
    lw    $19, 76($28)
    lw    $20, 80($28)
    lw    $21, 84($28)
    lw    $22, 88($28)
    lw    $23, 92($28)
    lw    $24, 96($28)
    lw    $25, 100($28)
    lw    $29, 116($28)
    lw    $30, 120($28)
    lw    $31, 124($28)
nt_tramp_slot:
    nop
    nop
    sw    $1, 4($28)
    sw    $2, 8($28)
    sw    $3, 12($28)
    sw    $4, 16($28)
    sw    $5, 20($28)
    sw    $6, 24($28)
    sw    $7, 28($28)
    sw    $8, 32($28)
    sw    $9, 36($28)
    sw    $10, 40($28)
    sw    $11, 44($28)
    sw    $12, 48($28)
    sw    $13, 52($28)
    sw    $14, 56($28)
    sw    $15, 60($28)
    sw    $16, 64($28)
    sw    $17, 68($28)
    sw    $18, 72($28)
    sw    $19, 76($28)
    sw    $20, 80($28)
    sw    $21, 84($28)
    sw    $22, 88($28)
    sw    $23, 92($28)
    sw    $24, 96($28)
    sw    $25, 100($28)
    sw    $29, 116($28)
    sw    $30, 120($28)
    sw    $31, 124($28)
    mfhi  $1
    sw    $1, 128($28)
    mflo  $1
    sw    $1, 132($28)
    b     nt_tramp_restore
    sw    $0, 136($28)
nt_tramp_taken:
    sw    $28, 136($28)
nt_tramp_restore:
    move  $4, $28
    lw    $16, 140($4)
    lw    $17, 144($4)
    lw    $18, 148($4)
    lw    $19, 152($4)
    lw    $20, 156($4)
    lw    $21, 160($4)
    lw    $22, 164($4)
    lw    $23, 168($4)
    lw    $30, 172($4)
    lw    $29, 176($4)
    lw    $31, 180($4)
    lw    $28, 184($4)
    jr    $31
    nop
nt_tramp_end:
