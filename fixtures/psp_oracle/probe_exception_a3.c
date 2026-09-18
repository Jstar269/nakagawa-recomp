// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// PSP-A3: one user-mode data access per run. PSPLink reports the resulting
// exception frame (ExcCode, EPC, BadVAddr) for the faulting cases. The faulting
// probes raise exactly one exception and never return from it, so each case is
// its own run. Case 7 (lwl/lwr/swl/swr) is the negative control: it must
// COMPLETE NORMALLY and write its findings to a results file, not raise.
//
// Select the case with -DA3_CASE=n:
//   1 (default) - load from a kernel-segment address. Measured as PSP-A3-01.
//   4           - misaligned lw in the delay slot of an always-taken branch
//                 (PSP-A3-04). Establishes Cause.BD and whether EPC names the
//                 branch.
//   5           - halfword load (lh at an odd address) from mapped user RAM
//                 (PSP-A3-05, load half).
//   6           - halfword store (sh at an odd address) to mapped user RAM
//                 (PSP-A3-05, store half).
//   7           - lwl/lwr/swl/swr across alignment boundaries (PSP-A3-06).
//                 Must not fault; returns and writes a results file.
//   8           - VFPU single load (lv.s at base+2) from an owned 16-byte-aligned
//                 buffer (PSP-A3-08). Faults iff the 4-byte rule applies.
//   9           - VFPU quad load (lv.q at base+4) from the same buffer
//                 (PSP-A3-09, +4 variant). Faults iff quads need 16-byte alignment.
//   10          - VFPU quad load (lv.q at base+8), the PSP-A3-09 +8 variant that
//                 separates an 8-byte rule from a 16-byte rule.
//   11          - VFPU quad store (sv.q at base+4) to the same buffer
//                 (PSP-A3-10). Store half of the quad-alignment question.
//   12          - VFPU negative control (PSP-A3-11): aligned lv.q/sv.q plus
//                 lvl.q/lvr.q at a 4-byte-aligned unaligned-to-16 address.
//                 Must not fault; returns and writes a results file.
//   13          - VFPU single store (sv.s at base+2) from an owned 16-byte-aligned
//                 buffer (PSP-A3-12). Faults with AdES iff the 4-byte rule
//                 applies to stores.
//   14          - VFPU quad store (sv.q at base+8), the PSP-A3-13 +8 variant that
//                 confirms the 16-byte store rule at +8 (compare with case 11).
//   15          - VFPU quad load (lv.q at base+4) in the delay slot of an
//                 always-taken branch (PSP-A3-14, delay-slot pattern copied from
//                 case 4). Establishes Cause.BD and whether EPC names the branch.
//   16          - VFPU left merge at an odd address (lvl.q at base+1, PSP-A3-15).
//                 Returns and writes a results file if it completes; an
//                 exception frame is itself the fault observation.
//
// Cases 8-12 need the VFPU unit, so their builds set the main thread attribute
// to THREAD_ATTR_USER | THREAD_ATTR_VFPU. Without it the access would trap
// with CpU (ExcCode 11) instead of measuring the alignment rule, which is why
// every VFPU header names the CpU-vs-AdEL/AdES check explicitly. The low two
// bits of a VFPU memory offset belong to the register encoding, so a +2
// effective address cannot be written as an immediate: the faulting VFPU cases
// put the full effective address in $t5 and use offset 0.
//
// Cases 2 and 3 (-DA3_CASE=2/3, misaligned word load/store) are owned by open
// PR #244 and intentionally not duplicated here; the CASE_IDs below skip
// 29/30 so a future merge cannot collide. Case 1's assembly is kept
// byte-identical so the existing A3-01 result stays valid.

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_A3_EXC_PROBE", 0, 1, 0);
#if A3_CASE >= 8
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
#else
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
#endif
PSP_HEAP_SIZE_KB(64);

#ifndef A3_BUILD_COMMIT
#error A3_BUILD_COMMIT is required
#endif

#ifndef A3_CASE
#define A3_CASE 1
#endif

extern const unsigned char a3_load_instruction[];
#if A3_CASE == 4 || A3_CASE == 15
extern const unsigned char a3_branch_instruction[];
extern const unsigned char a3_branch_target[];
#endif
#if A3_CASE == 7
extern const unsigned char a3_u_lwl[];
extern const unsigned char a3_u_lwr[];
extern const unsigned char a3_u_swl[];
extern const unsigned char a3_u_swr[];
#endif
#if A3_CASE == 12
extern const unsigned char a3_v_lvq[];
extern const unsigned char a3_v_svq[];
extern const unsigned char a3_v_lvl[];
extern const unsigned char a3_v_lvr[];
#endif
#if A3_CASE == 16
extern const unsigned char a3_v_lvl_odd[];
#endif

#if A3_CASE == 1

__attribute__((noinline)) static void a3_raise_address_error(void) {
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "lui $13, 0x8800\n\t"
        "ori $13, $13, 0x0010\n\t"
        "li $14, 0x0badc0de\n"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "lw $14, 4($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : : : "$4", "$5", "$6", "$7", "$8", "$13", "$14", "$15", "memory");
}

#elif A3_CASE == 2 || A3_CASE == 3

/* A mapped, writable, 16-byte-aligned buffer in the probe's own data. The fault
 * under test must come from the low address bits, not from the page being
 * absent, so the base is deliberately a real object this module owns. */
static volatile uint32_t a3_aligned_buffer[4] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_aligned_buffer;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
#if A3_CASE == 2
        "lw $14, 2($13)\n\t"
#else
        "sw $14, 2($13)\n\t"
#endif
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$14", "$15", "memory");
}

#elif A3_CASE == 4

/* A mapped, writable, 16-byte-aligned buffer in the probe's own data. The fault
 * under test must come from the low address bits, not from the page being
 * absent, so the base is deliberately a real object this module owns. */
static volatile uint32_t a3_aligned_buffer[4] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_aligned_buffer;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n\t"
        ".globl a3_branch_instruction\n"
        "a3_branch_instruction:\n\t"
        "beq $0, $0, a3_branch_target\n\t"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "lw $14, 2($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        "li $11, 0x0badc0de\n\t"
        ".globl a3_branch_target\n"
        "a3_branch_target:\n\t"
        "li $12, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$11", "$12", "$14", "$15", "memory");
}

#elif A3_CASE == 5

/* Halfword load at an odd address (PSP-A3-05, load half). */
static volatile uint32_t a3_aligned_buffer[4] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_aligned_buffer;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n\t"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "lh $14, 1($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$14", "$15", "memory");
}

#elif A3_CASE == 6

/* Halfword store at an odd address (PSP-A3-05, store half). */
static volatile uint32_t a3_aligned_buffer[4] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_aligned_buffer;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n\t"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "sh $14, 1($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$14", "$15", "memory");
}

#elif A3_CASE == 7

/* Negative control (PSP-A3-06): lwl/lwr/swl/swr across alignment boundaries
 * must not fault. This case RETURNS and writes a results file; it never
 * raises. Buffers are owned, aligned, mapped and writable, so any fault would
 * be attributable to the unaligned op itself. */
static uint8_t a3_src[12] __attribute__((aligned(4))) = {
    0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u,
    0x99u, 0xaau, 0xbbu, 0xccu,
};
static uint8_t a3_dst[12] __attribute__((aligned(4))) = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
};

__attribute__((noinline)) static uint32_t a3_unaligned_load_pair(
    const uint8_t *base, uint32_t *lwl_only_out, uint32_t *lwr_only_out) {
    uint32_t paired = 0u;
    uint32_t lwlo = 0xffffffffu;
    uint32_t lwro = 0u;
    register const uint8_t *p __asm__("$8") = base;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        ".globl a3_u_lwl\n"
        "a3_u_lwl:\n\t"
        "lwl %0, 1(%3)\n\t"
        ".globl a3_u_lwr\n"
        "a3_u_lwr:\n\t"
        "lwr %0, 4(%3)\n\t"
        ".set pop\n\t"
        : "+r"(paired) : "r"(p), "m"(*base), "r"(p) : "memory");
    __asm__ volatile(
        ".set noreorder\n\t"
        "lwl %0, 2(%1)\n\t"
        ".set reorder\n\t"
        : "+r"(lwlo) : "r"(base) : "memory");
    __asm__ volatile(
        ".set noreorder\n\t"
        "lwr %0, 2(%1)\n\t"
        ".set reorder\n\t"
        : "+r"(lwro) : "r"(base) : "memory");
    if (lwl_only_out) *lwl_only_out = lwlo;
    if (lwr_only_out) *lwr_only_out = lwro;
    return paired;
}

__attribute__((noinline)) static void a3_unaligned_store_pair(
    uint8_t *base, uint32_t value) {
    register uint8_t *p __asm__("$8") = base;
    register uint32_t v __asm__("$9") = value;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        ".globl a3_u_swl\n"
        "a3_u_swl:\n\t"
        "swl %1, 1(%0)\n\t"
        ".globl a3_u_swr\n"
        "a3_u_swr:\n\t"
        "swr %1, 4(%0)\n\t"
        ".set pop\n\t"
        : "+r"(p) : "r"(v) : "memory");
}

__attribute__((noinline)) static int a3_run_unaligned(char *out, size_t out_size) {
    uint32_t lwl_only = 0u, lwr_only = 0u;
    /* Unaligned load pair: bytes src[1..4] as one little-endian word. */
    uint32_t loaded = a3_unaligned_load_pair(a3_src, &lwl_only, &lwr_only);
    /* Unaligned store pair: write that word to dst[1..4]. */
    a3_unaligned_store_pair(a3_dst, loaded);
    /* Single swl/swr at another odd offset as a second no-fault witness. */
    {
        register uint8_t *p __asm__("$8") = a3_dst + 6;
        register uint32_t v __asm__("$9") = 0x0badc0deu;
        __asm__ volatile(
            ".set noreorder\n\t"
            "swl %1, 1(%0)\n\t"
            "swr %1, 4(%0)\n\t"
            ".set reorder\n\t"
            : "+r"(p) : "r"(v) : "memory");
    }
    return snprintf(out, out_size,
        "probe_id=PSP-A3 run_id=PSP-A3-06 build_commit=%s\n"
        "src_base=0x%08x dst_base=0x%08x\n"
        "src_bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "lwl_addr=0x%08x lwr_addr=0x%08x swl_addr=0x%08x swr_addr=0x%08x\n"
        "loaded_pair=0x%08x lwl_only_at_+2=0x%08x lwr_only_at_+2=0x%08x\n"
        "dst_bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "completed_normally=1 fault=0\n"
        "expectation_under_test=lwl/lwr/swl/swr cross alignment boundaries without faulting\n",
        A3_BUILD_COMMIT,
        (unsigned int)(uintptr_t)a3_src, (unsigned int)(uintptr_t)a3_dst,
        a3_src[0], a3_src[1], a3_src[2], a3_src[3],
        a3_src[4], a3_src[5], a3_src[6], a3_src[7],
        a3_src[8], a3_src[9], a3_src[10], a3_src[11],
        (unsigned int)(uintptr_t)a3_u_lwl, (unsigned int)(uintptr_t)a3_u_lwr,
        (unsigned int)(uintptr_t)a3_u_swl, (unsigned int)(uintptr_t)a3_u_swr,
        (unsigned int)loaded, (unsigned int)lwl_only, (unsigned int)lwr_only,
        a3_dst[0], a3_dst[1], a3_dst[2], a3_dst[3],
        a3_dst[4], a3_dst[5], a3_dst[6], a3_dst[7],
        a3_dst[8], a3_dst[9], a3_dst[10], a3_dst[11]);
}

#elif A3_CASE == 8

/* PSP-A3-08: VFPU single load at base+2. The effective address is held in $t5
 * with offset 0 because the low offset bits are register-encoding bits for
 * lv.s (assembling `lv.s S000, 2($t5)` silently encodes S002 at offset 0). */
static volatile uint32_t a3_vfpu_buffer[8] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_vfpu_buffer + 2u;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "lv.s S000, 0($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$14", "$15", "memory");
}

#elif A3_CASE == 9 || A3_CASE == 10

/* PSP-A3-09: VFPU quad load at base+4 (case 9) or base+8 (case 10) of an owned
 * 16-byte-aligned buffer. Together they separate a 4-byte rule (neither
 * faults), an 8-byte rule (only +4 faults) and a 16-byte rule (both fault). */
static volatile uint32_t a3_vfpu_buffer[8] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
#if A3_CASE == 9
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_vfpu_buffer + 4u;
#else
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_vfpu_buffer + 8u;
#endif
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "lv.q C000, 0($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$14", "$15", "memory");
}

#elif A3_CASE == 11

/* PSP-A3-10: VFPU quad store at base+4. The stored lanes are whatever C000
 * holds; the value is irrelevant because a fault precedes any commit, and a
 * clean return (exit 4, no frame) is itself the no-fault observation. */
static volatile uint32_t a3_vfpu_buffer[8] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_vfpu_buffer + 4u;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "sv.q C000, 0($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$14", "$15", "memory");
}

#elif A3_CASE == 12

/* Negative control (PSP-A3-11): aligned lv.q/sv.q plus lvl.q/lvr.q at a
 * 4-byte-aligned unaligned-to-16 address must complete normally. Returns and
 * writes a results file; never raises. The left/right destinations are
 * pre-filled from a sentinel quad so the reported words are deterministic. */
static uint32_t a3_v_src[8] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u,
};
static uint32_t a3_v_fill[4] __attribute__((aligned(16))) = {
    0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu, 0xddddddddu,
};
static uint32_t a3_v_dst[4] __attribute__((aligned(16))) = { 0u, 0u, 0u, 0u };
static uint32_t a3_v_dstL[4] __attribute__((aligned(16))) = { 0u, 0u, 0u, 0u };
static uint32_t a3_v_dstR[4] __attribute__((aligned(16))) = { 0u, 0u, 0u, 0u };

__attribute__((noinline)) static void a3_run_vfpu_aligned(void) {
    register uint32_t *ps __asm__("$8") = a3_v_src;
    register uint32_t *pd __asm__("$9") = a3_v_dst;
    register uint32_t *pl __asm__("$10") = a3_v_dstL;
    register uint32_t *pr __asm__("$11") = a3_v_dstR;
    register uint32_t *pf __asm__("$12") = a3_v_fill;
    register uint32_t *pu __asm__("$13") = a3_v_src + 1;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        ".globl a3_v_lvq\n"
        "a3_v_lvq:\n\t"
        "lv.q C000, 0($8)\n\t"
        ".globl a3_v_svq\n"
        "a3_v_svq:\n\t"
        "sv.q C000, 0($9)\n\t"
        "lv.q C010, 0($12)\n\t"
        ".globl a3_v_lvl\n"
        "a3_v_lvl:\n\t"
        "lvl.q C010, 0($13)\n\t"
        "sv.q C010, 0($10)\n\t"
        "lv.q C020, 0($12)\n\t"
        ".globl a3_v_lvr\n"
        "a3_v_lvr:\n\t"
        "lvr.q C020, 0($13)\n\t"
        "sv.q C020, 0($11)\n\t"
        ".set pop\n\t"
        : "+r"(ps), "+r"(pd), "+r"(pl), "+r"(pr), "+r"(pf), "+r"(pu)
        : : "memory");
}

__attribute__((noinline)) static int a3_write_vfpu_results(char *out, size_t out_size) {
    a3_run_vfpu_aligned();
    return snprintf(out, out_size,
        "probe_id=PSP-A3 run_id=PSP-A3-11 build_commit=%s\n"
        "src_base=0x%08x fill_base=0x%08x dst_base=0x%08x dstL_base=0x%08x dstR_base=0x%08x unaligned_addr=0x%08x\n"
        "src_words=%08x %08x %08x %08x\n"
        "fill_words=%08x %08x %08x %08x\n"
        "lvq_addr=0x%08x svq_addr=0x%08x lvl_addr=0x%08x lvr_addr=0x%08x\n"
        "dst_words=%08x %08x %08x %08x\n"
        "dstL_words=%08x %08x %08x %08x\n"
        "dstR_words=%08x %08x %08x %08x\n"
        "completed_normally=1 fault=0\n"
        "expectation_under_test=aligned lv.q/sv.q and 4-byte-aligned lvl.q/lvr.q complete without faulting\n",
        A3_BUILD_COMMIT,
        (unsigned int)(uintptr_t)a3_v_src, (unsigned int)(uintptr_t)a3_v_fill,
        (unsigned int)(uintptr_t)a3_v_dst, (unsigned int)(uintptr_t)a3_v_dstL,
        (unsigned int)(uintptr_t)a3_v_dstR, (unsigned int)(uintptr_t)(a3_v_src + 1),
        a3_v_src[0], a3_v_src[1], a3_v_src[2], a3_v_src[3],
        a3_v_fill[0], a3_v_fill[1], a3_v_fill[2], a3_v_fill[3],
        (unsigned int)(uintptr_t)a3_v_lvq, (unsigned int)(uintptr_t)a3_v_svq,
        (unsigned int)(uintptr_t)a3_v_lvl, (unsigned int)(uintptr_t)a3_v_lvr,
        a3_v_dst[0], a3_v_dst[1], a3_v_dst[2], a3_v_dst[3],
        a3_v_dstL[0], a3_v_dstL[1], a3_v_dstL[2], a3_v_dstL[3],
        a3_v_dstR[0], a3_v_dstR[1], a3_v_dstR[2], a3_v_dstR[3]);
}

#elif A3_CASE == 13

/* PSP-A3-12: VFPU single store at base+2. Store half of the single-alignment
 * question: lv.s at +2 (case 8) already proved loads need 4-byte alignment.
 * The effective address is held in $t5 with offset 0 for the same
 * register-encoding reason as case 8. */
static volatile uint32_t a3_vfpu_buffer[8] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_vfpu_buffer + 2u;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "sv.s S000, 0($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$14", "$15", "memory");
}

#elif A3_CASE == 14

/* PSP-A3-13: VFPU quad store at base+8. Confirms the 16-byte store rule at +8:
 * case 11 (sv.q at +4) already faults; an 8-byte rule would let +8 through
 * while a 16-byte rule faults here too. */
static volatile uint32_t a3_vfpu_buffer[8] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_vfpu_buffer + 8u;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "sv.q C000, 0($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$14", "$15", "memory");
}

#elif A3_CASE == 15

/* PSP-A3-14: VFPU quad load at base+4 in the delay slot of an always-taken
 * branch. Delay-slot pattern copied from case 4 (PSP-A3-04): establishes
 * whether Cause.BD is set and EPC names the branch, as for scalar words. */
static volatile uint32_t a3_vfpu_buffer[8] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u,
};

__attribute__((noinline)) static void a3_raise_address_error(void) {
    register uint32_t base __asm__("$13") = (uint32_t)(uintptr_t)a3_vfpu_buffer + 4u;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $14, 0x0badc0de\n\t"
        ".globl a3_branch_instruction\n"
        "a3_branch_instruction:\n\t"
        "beq $0, $0, a3_branch_target\n\t"
        ".globl a3_load_instruction\n"
        "a3_load_instruction:\n\t"
        "lv.q C000, 0($13)\n\t"
        "li $15, 0x5a5a5a5a\n\t"
        "li $11, 0x0badc0de\n\t"
        ".globl a3_branch_target\n"
        "a3_branch_target:\n\t"
        "li $12, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$11", "$12", "$14", "$15", "memory");
}

#elif A3_CASE == 16

/* PSP-A3-15: VFPU left merge at an odd address (lvl.q at base+1). Returns and
 * writes a results file if it completes; the PSPLink exception frame is
 * itself the fault observation. The destination is pre-filled from a sentinel
 * quad so the reported words are deterministic either way. Offset 0 holds the
 * full effective address in $t5, as in cases 8..11, because the low offset
 * bits encode the register. */
static uint32_t a3_v_src[8] __attribute__((aligned(16))) = {
    0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u,
};
static uint32_t a3_v_fill[4] __attribute__((aligned(16))) = {
    0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu, 0xddddddddu,
};
static uint32_t a3_v_dstOdd[4] __attribute__((aligned(16))) = { 0u, 0u, 0u, 0u };

__attribute__((noinline)) static void a3_run_vfpu_odd(void) {
    register uint32_t *pd __asm__("$10") = a3_v_dstOdd;
    register uint32_t *pf __asm__("$12") = a3_v_fill;
    register uint8_t *pu __asm__("$13") =
        (uint8_t *)(uintptr_t)a3_v_src + 1u;
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "lv.q C010, 0($12)\n\t"
        ".globl a3_v_lvl_odd\n"
        "a3_v_lvl_odd:\n\t"
        "lvl.q C010, 0($13)\n\t"
        "sv.q C010, 0($10)\n\t"
        ".set pop\n\t"
        : "+r"(pd), "+r"(pf), "+r"(pu)
        : : "memory");
}

__attribute__((noinline)) static int a3_write_vfpu_odd_results(char *out, size_t out_size) {
    a3_run_vfpu_odd();
    return snprintf(out, out_size,
        "probe_id=PSP-A3 run_id=PSP-A3-15 build_commit=%s\n"
        "src_base=0x%08x fill_base=0x%08x dst_base=0x%08x odd_addr=0x%08x\n"
        "src_words=%08x %08x %08x %08x\n"
        "fill_words=%08x %08x %08x %08x\n"
        "lvl_addr=0x%08x\n"
        "dst_words=%08x %08x %08x %08x\n"
        "completed_normally=1 fault=0\n"
        "expectation_under_test=lvl.q at an odd address completes without faulting (left/right bypass)\n",
        A3_BUILD_COMMIT,
        (unsigned int)(uintptr_t)a3_v_src, (unsigned int)(uintptr_t)a3_v_fill,
        (unsigned int)(uintptr_t)a3_v_dstOdd,
        (unsigned int)((uintptr_t)a3_v_src + 1u),
        a3_v_src[0], a3_v_src[1], a3_v_src[2], a3_v_src[3],
        a3_v_fill[0], a3_v_fill[1], a3_v_fill[2], a3_v_fill[3],
        (unsigned int)(uintptr_t)a3_v_lvl_odd,
        a3_v_dstOdd[0], a3_v_dstOdd[1], a3_v_dstOdd[2], a3_v_dstOdd[3]);
}

#else
#error unsupported A3_CASE
#endif

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
#if A3_CASE == 7 || A3_CASE == 12 || A3_CASE == 16
    {
        char results[1536];
#if A3_CASE == 7
        int length = a3_run_unaligned(results, sizeof(results));
#elif A3_CASE == 12
        int length = a3_write_vfpu_results(results, sizeof(results));
#else
        int length = a3_write_vfpu_odd_results(results, sizeof(results));
#endif
        if (length < 0 || (size_t)length >= sizeof(results)) return 1;
        SceUID fd = sceIoOpen(
#if A3_CASE == 7
                              "host0:/a3_unaligned_results.txt",
#elif A3_CASE == 12
                              "host0:/a3_vfpu_aligned_results.txt",
#else
                              "host0:/a3_vfpu_lvl_odd_results.txt",
#endif
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
        if (fd < 0) return 2;
        int written = sceIoWrite(fd, results, (SceSize)length);
        int closed = sceIoClose(fd);
        if (written != length || closed < 0) return 3;
        return 0;
    }
#else
    char header[768];
#if A3_CASE == 1
    int length = snprintf(header, sizeof(header),
        "probe_id=PSP-A3 run_id=PSP-A3-01 build_commit=%s\n"
        "load_address=0x%08x instruction=lw $t6,4($t5) t5=0x88000010 effective=0x88000014\n"
        "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa t6=0badc0de(before)\n"
        "t7=5a5a5a5a only if execution continued past the load\n",
        A3_BUILD_COMMIT, (unsigned int)(uintptr_t)a3_load_instruction);
#elif A3_CASE == 2 || A3_CASE == 3
    {
        const unsigned int base = (unsigned int)(uintptr_t)a3_aligned_buffer;
        int length = snprintf(header, sizeof(header),
            "probe_id=PSP-A3 run_id=%s build_commit=%s\n"
            "access_address=0x%08x instruction=%s t5=0x%08x effective=0x%08x\n"
            "base_is_16byte_aligned=%u mapped_and_writable=yes\n"
            "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa t6=0badc0de(before)\n"
            "t7=5a5a5a5a only if execution continued past the access\n"
            "expectation_under_test=%s\n",
#if A3_CASE == 2
            "PSP-A3-02", A3_BUILD_COMMIT,
            (unsigned int)(uintptr_t)a3_load_instruction, "lw $t6,2($t5)",
            base, base + 2u, (unsigned int)((base & 15u) == 0u),
            "misaligned load raises AdEL (ExcCode 4), BadVAddr = effective address"
#else
            "PSP-A3-03", A3_BUILD_COMMIT,
            (unsigned int)(uintptr_t)a3_load_instruction, "sw $t6,2($t5)",
            base, base + 2u, (unsigned int)((base & 15u) == 0u),
            "misaligned store raises AdES (ExcCode 5), BadVAddr = effective address"
#endif
            );
        if (length < 0 || (size_t)length >= sizeof(header)) return 1;
        /* O_EXCL on purpose: a rerun must not silently overwrite a previous
         * run's header, because that is the record the measurement is cited from. */
        SceUID fd = sceIoOpen(
#if A3_CASE == 2
                             "host0:/a3_mload_header.txt",
#else
                             "host0:/a3_mstore_header.txt",
#endif
                             PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
        if (fd < 0) return 2;
        int written = sceIoWrite(fd, header, (SceSize)length);
        int closed = sceIoClose(fd);
        if (written != length || closed < 0) return 3;
        a3_raise_address_error();
        return 4;
    }
#elif A3_CASE == 4
    {
        const unsigned int base = (unsigned int)(uintptr_t)a3_aligned_buffer;
        int length = snprintf(header, sizeof(header),
            "probe_id=PSP-A3 run_id=PSP-A3-04 build_commit=%s\n"
            "branch_address=0x%08x load_address=0x%08x target_address=0x%08x "
            "instruction=lw $t6,2($t5) in delay slot of beq $0,$0,target t5=0x%08x effective=0x%08x\n"
            "base_is_16byte_aligned=%u mapped_and_writable=yes\n"
            "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa t6=0badc0de(before)\n"
            "t7=5a5a5a5a only if execution continued past the access\n"
            "expectation_under_test=misaligned load in delay slot sets BD, EPC = branch address\n",
            A3_BUILD_COMMIT,
            (unsigned int)(uintptr_t)a3_branch_instruction,
            (unsigned int)(uintptr_t)a3_load_instruction,
            (unsigned int)(uintptr_t)a3_branch_target,
            base, base + 2u, (unsigned int)((base & 15u) == 0u));
        if (length < 0 || (size_t)length >= sizeof(header)) return 1;
        SceUID fd = sceIoOpen("host0:/a3_delayslot_header.txt",
                             PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
        if (fd < 0) return 2;
        int written = sceIoWrite(fd, header, (SceSize)length);
        int closed = sceIoClose(fd);
        if (written != length || closed < 0) return 3;
        a3_raise_address_error();
        return 4;
    }
#elif A3_CASE == 5
    {
        const unsigned int base = (unsigned int)(uintptr_t)a3_aligned_buffer;
        int length = snprintf(header, sizeof(header),
            "probe_id=PSP-A3 run_id=PSP-A3-05 build_commit=%s variant=lh\n"
            "access_address=0x%08x instruction=lh $t6,1($t5) t5=0x%08x effective=0x%08x\n"
            "base_is_16byte_aligned=%u mapped_and_writable=yes\n"
            "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa t6=0badc0de(before)\n"
            "t7=5a5a5a5a only if execution continued past the access\n"
            "expectation_under_test=halfword load at odd address raises AdEL (ExcCode 4), BadVAddr = effective address\n",
            A3_BUILD_COMMIT,
            (unsigned int)(uintptr_t)a3_load_instruction,
            base, base + 1u, (unsigned int)((base & 15u) == 0u));
        if (length < 0 || (size_t)length >= sizeof(header)) return 1;
        SceUID fd = sceIoOpen("host0:/a3_half_load_header.txt",
                             PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
        if (fd < 0) return 2;
        int written = sceIoWrite(fd, header, (SceSize)length);
        int closed = sceIoClose(fd);
        if (written != length || closed < 0) return 3;
        a3_raise_address_error();
        return 4;
    }
#elif A3_CASE == 6
    {
        const unsigned int base = (unsigned int)(uintptr_t)a3_aligned_buffer;
        int length = snprintf(header, sizeof(header),
            "probe_id=PSP-A3 run_id=PSP-A3-05 build_commit=%s variant=sh\n"
            "access_address=0x%08x instruction=sh $t6,1($t5) t5=0x%08x effective=0x%08x\n"
            "base_is_16byte_aligned=%u mapped_and_writable=yes\n"
            "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa t6=0badc0de(before)\n"
            "t7=5a5a5a5a only if execution continued past the access\n"
            "expectation_under_test=halfword store at odd address raises AdES (ExcCode 5), BadVAddr = effective address\n",
            A3_BUILD_COMMIT,
            (unsigned int)(uintptr_t)a3_load_instruction,
            base, base + 1u, (unsigned int)((base & 15u) == 0u));
        if (length < 0 || (size_t)length >= sizeof(header)) return 1;
        SceUID fd = sceIoOpen("host0:/a3_half_store_header.txt",
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
        if (fd < 0) return 2;
        int written = sceIoWrite(fd, header, (SceSize)length);
        int closed = sceIoClose(fd);
        if (written != length || closed < 0) return 3;
        a3_raise_address_error();
        return 4;
    }
#elif A3_CASE == 8 || A3_CASE == 9 || A3_CASE == 10 || A3_CASE == 11 || A3_CASE == 13 || A3_CASE == 14
    {
        const unsigned int base = (unsigned int)(uintptr_t)a3_vfpu_buffer;
#if A3_CASE == 8 || A3_CASE == 13
        const unsigned int eff = base + 2u;
#elif A3_CASE == 10 || A3_CASE == 14
        const unsigned int eff = base + 8u;
#else
        const unsigned int eff = base + 4u;
#endif
        int length = snprintf(header, sizeof(header),
            "probe_id=PSP-A3 run_id=%s build_commit=%s variant=%s\n"
            "access_address=0x%08x instruction=%s t5=0x%08x effective=0x%08x\n"
            "base_is_16byte_aligned=%u mapped_and_writable=yes vfpu_thread_attr=yes\n"
            "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa\n"
            "t7=5a5a5a5a only if execution continued past the access\n"
            "expectation_under_test=%s\n",
#if A3_CASE == 8
            "PSP-A3-08", A3_BUILD_COMMIT, "lv.s+2",
            (unsigned int)(uintptr_t)a3_load_instruction, "lv.s S000,0($t5)",
            eff, eff, (unsigned int)((base & 15u) == 0u),
            "single VFPU load at base+2 raises AdEL (ExcCode 4) iff the 4-byte rule applies; CpU (ExcCode 11) means the thread lacks the VFPU attribute, not an alignment result"
#elif A3_CASE == 9
            "PSP-A3-09", A3_BUILD_COMMIT, "lv.q+4",
            (unsigned int)(uintptr_t)a3_load_instruction, "lv.q C000,0($t5)",
            eff, eff, (unsigned int)((base & 15u) == 0u),
            "quad VFPU load at a 4-byte-aligned address faults iff quads require 16-byte alignment; CpU (ExcCode 11) means the thread lacks the VFPU attribute"
#elif A3_CASE == 10
            "PSP-A3-09", A3_BUILD_COMMIT, "lv.q+8",
            (unsigned int)(uintptr_t)a3_load_instruction, "lv.q C000,0($t5)",
            eff, eff, (unsigned int)((base & 15u) == 0u),
            "quad VFPU load at an 8-byte-aligned address separates an 8-byte rule (no fault) from a 16-byte rule (fault); compare with the +4 variant"
#elif A3_CASE == 11
            "PSP-A3-10", A3_BUILD_COMMIT, "sv.q+4",
            (unsigned int)(uintptr_t)a3_load_instruction, "sv.q C000,0($t5)",
            eff, eff, (unsigned int)((base & 15u) == 0u),
            "quad VFPU store at a 4-byte-aligned address faults with AdES (ExcCode 5) iff quads require 16-byte alignment; CpU (ExcCode 11) means the thread lacks the VFPU attribute"
#elif A3_CASE == 13
            "PSP-A3-12", A3_BUILD_COMMIT, "sv.s+2",
            (unsigned int)(uintptr_t)a3_load_instruction, "sv.s S000,0($t5)",
            eff, eff, (unsigned int)((base & 15u) == 0u),
            "single VFPU store at base+2 raises AdES (ExcCode 5) iff the 4-byte rule applies to stores; CpU (ExcCode 11) means the thread lacks the VFPU attribute, not an alignment result"
#else
            "PSP-A3-13", A3_BUILD_COMMIT, "sv.q+8",
            (unsigned int)(uintptr_t)a3_load_instruction, "sv.q C000,0($t5)",
            eff, eff, (unsigned int)((base & 15u) == 0u),
            "quad VFPU store at an 8-byte-aligned address faults with AdES (ExcCode 5) iff quads require 16-byte alignment; compare with the +4 variant (PSP-A3-10)"
#endif
            );
        if (length < 0 || (size_t)length >= sizeof(header)) return 1;
        /* O_EXCL on purpose: a rerun must not silently overwrite a previous
         * run's header, because that is the record the measurement is cited from. */
        SceUID fd = sceIoOpen(
#if A3_CASE == 8
                              "host0:/a3_vfpu_lvs_header.txt",
#elif A3_CASE == 9
                              "host0:/a3_vfpu_lvq4_header.txt",
#elif A3_CASE == 10
                              "host0:/a3_vfpu_lvq8_header.txt",
#elif A3_CASE == 11
                              "host0:/a3_vfpu_svq4_header.txt",
#elif A3_CASE == 13
                              "host0:/a3_vfpu_svs_header.txt",
#else
                              "host0:/a3_vfpu_svq8_header.txt",
#endif
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
        if (fd < 0) return 2;
        int written = sceIoWrite(fd, header, (SceSize)length);
        int closed = sceIoClose(fd);
        if (written != length || closed < 0) return 3;
        a3_raise_address_error();
        return 4;
    }
#elif A3_CASE == 15
    {
        const unsigned int base = (unsigned int)(uintptr_t)a3_vfpu_buffer;
        int length = snprintf(header, sizeof(header),
            "probe_id=PSP-A3 run_id=PSP-A3-14 build_commit=%s\n"
            "branch_address=0x%08x load_address=0x%08x target_address=0x%08x "
            "instruction=lv.q C000,0($t5) in delay slot of beq $0,$0,target t5=0x%08x effective=0x%08x\n"
            "base_is_16byte_aligned=%u mapped_and_writable=yes vfpu_thread_attr=yes\n"
            "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa\n"
            "t7=5a5a5a5a only if execution continued past the access\n"
            "expectation_under_test=misaligned VFPU quad load in delay slot sets BD, EPC = branch address\n",
            A3_BUILD_COMMIT,
            (unsigned int)(uintptr_t)a3_branch_instruction,
            (unsigned int)(uintptr_t)a3_load_instruction,
            (unsigned int)(uintptr_t)a3_branch_target,
            base + 4u, base + 4u, (unsigned int)((base & 15u) == 0u));
        if (length < 0 || (size_t)length >= sizeof(header)) return 1;
        SceUID fd = sceIoOpen("host0:/a3_vfpu_lvq_delay_header.txt",
                             PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
        if (fd < 0) return 2;
        int written = sceIoWrite(fd, header, (SceSize)length);
        int closed = sceIoClose(fd);
        if (written != length || closed < 0) return 3;
        a3_raise_address_error();
        return 4;
    }
#endif
#if A3_CASE == 1
    if (length < 0 || (size_t)length >= sizeof(header)) return 1;
    SceUID fd = sceIoOpen("host0:/a3_header.txt",
                         PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    if (fd < 0) return 2;
    int written = sceIoWrite(fd, header, (SceSize)length);
    int closed = sceIoClose(fd);
    if (written != length || closed < 0) return 3;
    a3_raise_address_error();
    return 4;
#endif
#endif
}
