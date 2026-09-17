// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// PSP-A2: one user-mode BREAK placed in the delay slot of an always-taken
// branch. PSPLink reports the resulting exception frame, which records EPC and
// the Cause BD bit for a delay-slot exception. The probe raises exactly one
// exception and never returns from it.

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>

PSP_MODULE_INFO("PSP_A2_EXC_PROBE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef A2_BUILD_COMMIT
#error A2_BUILD_COMMIT is required
#endif

extern const unsigned char a2_branch_instruction[];
extern const unsigned char a2_break_instruction[];
extern const unsigned char a2_branch_target[];

__attribute__((noinline)) static void a2_raise_break_in_delay_slot(void) {
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $9, 0xbbbbbbbb\n\t"
        ".globl a2_branch_instruction\n"
        "a2_branch_instruction:\n\t"
        "beq $0, $0, a2_branch_target\n"
        ".globl a2_break_instruction\n"
        "a2_break_instruction:\n\t"
        ".word 0x0016968d\n\t"
        "li $10, 0x0badc0de\n\t"
        "li $11, 0x0badc0de\n"
        ".globl a2_branch_target\n"
        "a2_branch_target:\n\t"
        "li $12, 0x5a5a5a5a\n\t"
        ".set pop\n\t"
        : : : "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "$12", "memory");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char header[512];
    int length = snprintf(header, sizeof(header),
        "probe_id=PSP-A2 run_id=PSP-A2-01 build_commit=%s\n"
        "branch_address=0x%08x break_address=0x%08x target_address=0x%08x "
        "instruction=0x0016968d code20=0x05a5a\n"
        "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa t1=bbbbbbbb\n"
        "t2/t3=0badc0de only if the fall-through path ran; t4=5a5a5a5a only if the target ran\n",
        A2_BUILD_COMMIT, (unsigned int)(uintptr_t)a2_branch_instruction,
        (unsigned int)(uintptr_t)a2_break_instruction, (unsigned int)(uintptr_t)a2_branch_target);
    if (length < 0 || (size_t)length >= sizeof(header)) return 1;
    SceUID fd = sceIoOpen("host0:/a2_header.txt",
                         PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    if (fd < 0) return 2;
    int written = sceIoWrite(fd, header, (SceSize)length);
    int closed = sceIoClose(fd);
    if (written != length || closed < 0) return 3;
    a2_raise_break_in_delay_slot();
    return 4;
}
