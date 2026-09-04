// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
// Minimal MIPS assembly trampoline for PSP thread entry register capture.
// PSPDEV_BUILD = NOT RUN without successful psp-gcc compilation and
// inspection of assembler output/relocations/symbol transfer/snapshot offsets.
// REGISTER_CAPTURE_BUILD_VALIDATION = REQUIRED_BEFORE_HARDWARE.
// No claim of PIC_SAFE / ABI_PROVEN / COMPILES without PSPDEV evidence.
//
// Captures a0-a3, t0-t3, gp, sp, ra, hi/lo at the first instruction before
// any C prologue. sp is captured BEFORE allocating a frame via $t8; gp/ra
// are captured directly; hi/lo are read via mfhi/mflo immediately after the
// GPR stores. Unavoidable clobber: $t8, $t9 and $at are used as temporaries
// for address formation and canary store; $t0/$t1 are overwritten only AFTER
// their original values have been stored, so the snapshot retains the entry
// values for a0-a3/t0-t3/gp/sp/ra. Relocation-safe address formation uses
// explicit %hi/%lo via lui/ori (PSPSDK convention) rather than pseudo la
// before capture where possible; where `la` remains it is documented as
// using $at.
//
// If the toolchain cannot guarantee pre-prologue capture, the case is
// recorded as NOT_MEASURABLE_WITH_CURRENT_HARNESS rather than fabricating.

#include <pspimport.s>

    .set noat
    .set noreorder
    .text
    .globl thread_entry_shim
    .ent thread_entry_shim
thread_entry_shim:
    // Capture sp into $t8 before any stack allocation – must be first
    move $t8, $sp

    // Explicit relocation-safe load of g_thread_snapshot address: lui/ori via %hi/%lo
    // This avoids pseudo-instruction ordering ambiguity before capture.
    lui $t9, %hi(g_thread_snapshot)
    ori $t9, $t9, %lo(g_thread_snapshot)

    sw $a0, 0($t9)
    sw $a1, 4($t9)
    sw $a2, 8($t9)
    sw $a3, 12($t9)
    sw $t0, 16($t9)
    sw $t1, 20($t9)
    sw $t2, 24($t9)
    sw $t3, 28($t9)
    sw $gp, 32($t9)
    sw $t8, 36($t9)      // original sp
    sw $ra, 40($t9)
    mfhi $t0
    mflo $t1
    sw $t0, 44($t9)
    sw $t1, 48($t9)
    li $t0, 0xA5A5A5A5
    sw $t0, 52($t9)

    // Mark valid – explicit lui/ori again for g_snapshot_valid
    lui $t9, %hi(g_snapshot_valid)
    ori $t9, $t9, %lo(g_snapshot_valid)
    li $t0, 1
    sw $t0, 0($t9)

    // Tail-call to C handler; a0/a1 still hold original argSize/argp per PSP ABI.
    // No frame allocated, no GP setup, no jal clobber of ra beyond captured value.
    j thread_entry_c
    nop
    .end thread_entry_shim
    // Build validation: snapshot struct offsets must match C side.
    // C side has _Static_assert(offsetof(ThreadEntrySnapshot, a0)==0 etc.)
    // If drift, compilation fails. Hardware remains prohibited until PSPDEV_BUILD succeeds.
