// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// PSP-A3: one user-mode data read from a kernel-segment address. PSPLink
// reports the resulting exception frame (ExcCode, EPC, BadVAddr). The probe
// raises exactly one exception and never returns from it.

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

extern const unsigned char a3_load_instruction[];

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

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char header[512];
    int length = snprintf(header, sizeof(header),
        "probe_id=PSP-A3 run_id=PSP-A3-01 build_commit=%s\n"
        "load_address=0x%08x instruction=lw $t6,4($t5) t5=0x88000010 effective=0x88000014\n"
        "a0=11111111 a1=22222222 a2=33333333 a3=44444444 t0=aaaaaaaa t6=0badc0de(before)\n"
        "t7=5a5a5a5a only if execution continued past the load\n",
        A3_BUILD_COMMIT, (unsigned int)(uintptr_t)a3_load_instruction);
    if (length < 0 || (size_t)length >= sizeof(header)) return 1;
    SceUID fd = sceIoOpen("host0:/a3_header.txt",
                         PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    if (fd < 0) return 2;
    int written = sceIoWrite(fd, header, (SceSize)length);
    int closed = sceIoClose(fd);
    if (written != length || closed < 0) return 3;
    a3_raise_address_error();
    return 4;
}
