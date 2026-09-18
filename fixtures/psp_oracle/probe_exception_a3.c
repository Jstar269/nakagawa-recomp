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
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef A3_BUILD_COMMIT
#error A3_BUILD_COMMIT is required
#endif

#ifndef A3_CASE
#define A3_CASE 1
#endif

extern const unsigned char a3_load_instruction[];
#if A3_CASE == 4
extern const unsigned char a3_branch_instruction[];
extern const unsigned char a3_branch_target[];
#endif
#if A3_CASE == 7
extern const unsigned char a3_u_lwl[];
extern const unsigned char a3_u_lwr[];
extern const unsigned char a3_u_swl[];
extern const unsigned char a3_u_swr[];
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

#else
#error unsupported A3_CASE
#endif

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
#if A3_CASE == 7
    {
        char results[1024];
        int length = a3_run_unaligned(results, sizeof(results));
        if (length < 0 || (size_t)length >= sizeof(results)) return 1;
        SceUID fd = sceIoOpen("host0:/a3_unaligned_results.txt",
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
