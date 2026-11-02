/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Single-step trampoline for the insn-trace probe (probe_insn_trace.c, #312).

   void nt_tramp(NtState *st): saves the caller's callee-saved registers, sp, ra, gp,
   f0-f3, fcr31 and VFPU S000-S003; loads the selected guest state; executes the two
   words at nt_tramp_slot; stores the selected state back with flag 0; restores the
   caller and returns. Conditional branches use nt_tramp_taken to set flag and skip the
   stores; the probe keeps its copy of the state for branches.

   NtState layout (bytes): r[32] at 0, hi 128, lo 132, flag 136, f[4] 140,
   fcr31 156, v[5] 160, host[12] 180, host_f[4] 228, host_fcr31 244,
   host_v[5] 248. S000-S003 map to v0/v1/v2/v3; S200 maps to v32. */

    .set noreorder
    .set noat
    .text
    .align 4
    .global nt_tramp_begin
    .global nt_tramp_slot
    .global nt_tramp_taken
    .global nt_tramp_end
nt_tramp_begin:
    sw    $16, 180($4)
    sw    $17, 184($4)
    sw    $18, 188($4)
    sw    $19, 192($4)
    sw    $20, 196($4)
    sw    $21, 200($4)
    sw    $22, 204($4)
    sw    $23, 208($4)
    sw    $30, 212($4)
    sw    $29, 216($4)
    sw    $31, 220($4)
    sw    $28, 224($4)
    swc1  $f0, 228($4)
    swc1  $f1, 232($4)
    swc1  $f2, 236($4)
    swc1  $f3, 240($4)
    cfc1  $1, $31
    sw    $1, 244($4)
    sv.s  S000, 248($4)
    sv.s  S001, 252($4)
    sv.s  S002, 256($4)
    sv.s  S003, 260($4)
    sv.s  S200, 264($4)
    move  $28, $4
    lw    $1, 128($28)
    mthi  $1
    lw    $1, 132($28)
    mtlo  $1
    lw    $1, 140($28)
    mtc1  $1, $f0
    lw    $1, 144($28)
    mtc1  $1, $f1
    lw    $1, 148($28)
    mtc1  $1, $f2
    lw    $1, 152($28)
    mtc1  $1, $f3
    lw    $1, 156($28)
    ctc1  $1, $31
    lv.s  S000, 160($28)
    lv.s  S001, 164($28)
    lv.s  S002, 168($28)
    lv.s  S003, 172($28)
    lv.s  S200, 176($28)
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
    swc1  $f0, 140($28)
    swc1  $f1, 144($28)
    swc1  $f2, 148($28)
    swc1  $f3, 152($28)
    cfc1  $1, $31
    sw    $1, 156($28)
    sv.s  S000, 160($28)
    sv.s  S001, 164($28)
    sv.s  S002, 168($28)
    sv.s  S003, 172($28)
    sv.s  S200, 176($28)
    b     nt_tramp_restore
    sw    $0, 136($28)
nt_tramp_taken:
    sw    $28, 136($28)
nt_tramp_restore:
    move  $4, $28
    lwc1  $f0, 228($4)
    lwc1  $f1, 232($4)
    lwc1  $f2, 236($4)
    lwc1  $f3, 240($4)
    lw    $1, 244($4)
    ctc1  $1, $31
    lv.s  S000, 248($4)
    lv.s  S001, 252($4)
    lv.s  S002, 256($4)
    lv.s  S003, 260($4)
    lv.s  S200, 264($4)
    lw    $16, 180($4)
    lw    $17, 184($4)
    lw    $18, 188($4)
    lw    $19, 192($4)
    lw    $20, 196($4)
    lw    $21, 200($4)
    lw    $22, 204($4)
    lw    $23, 208($4)
    lw    $30, 212($4)
    lw    $29, 216($4)
    lw    $31, 220($4)
    lw    $28, 224($4)
    jr    $31
    nop
nt_tramp_end:
