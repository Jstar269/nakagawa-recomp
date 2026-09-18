// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// PSP-A3: one user-mode data access that is expected to fault. PSPLink reports
// the resulting exception frame (ExcCode, EPC, BadVAddr). The probe raises
// exactly one exception and never returns from it, so each case is its own run.
//
// Select the case with -DA3_CASE=n:
//   1 (default) - load from a kernel-segment address. Measured as PSP-A3-01.
//   2           - misaligned load  (lw at base+2) from mapped user RAM.
//   3           - misaligned store (sw at base+2) to mapped user RAM.
//
// Cases 2 and 3 exist because src/rt/cpu_lle.c currently raises AdEL/AdES for
// misalignment on the MIPS32 architectural rule alone and labels that SYNTHETIC.
// These runs are what would let it be labelled measured instead. Case 1's
// assembly is kept byte-identical so the existing A3-01 result stays valid.

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>

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

#else

// A mapped, writable, 16-byte-aligned buffer in the probe's own data. The fault
// under test must come from the low address bits, not from the page being
// absent, so the base is deliberately a real object this module owns.
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
#elif A3_CASE == 3
        "sw $14, 2($13)\n\t"
#else
#error unsupported A3_CASE
#endif
        "li $15, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : "+r"(base)
        : : "$4", "$5", "$6", "$7", "$8", "$14", "$15", "memory");
}

#endif

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char header[512];
#if A3_CASE == 1
    int length = snprintf(header, sizeof(header),
        "probe_id=PSP-A3 run_id=PSP-A3-01 build_commit=%s\n"
        "load_address=0x%08x instruction=lw $t6,4($t5) t5=0x88000010 effective=0x88000014\n"
        "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa t6=0badc0de(before)\n"
        "t7=5a5a5a5a only if execution continued past the load\n",
        A3_BUILD_COMMIT, (unsigned int)(uintptr_t)a3_load_instruction);
#else
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
#endif
    if (length < 0 || (size_t)length >= sizeof(header)) return 1;
#if A3_CASE == 1
#define A3_HEADER_PATH "host0:/a3_header.txt"
#elif A3_CASE == 2
#define A3_HEADER_PATH "host0:/a3_mload_header.txt"
#else
#define A3_HEADER_PATH "host0:/a3_mstore_header.txt"
#endif
    // O_EXCL on purpose: a rerun must not silently overwrite a previous run's
    // header, because that is the record the measurement is cited from.
    SceUID fd = sceIoOpen(A3_HEADER_PATH,
                         PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    if (fd < 0) return 2;
    int written = sceIoWrite(fd, header, (SceSize)length);
    int closed = sceIoClose(fd);
    if (written != length || closed < 0) return 3;
    a3_raise_address_error();
    return 4;
}
