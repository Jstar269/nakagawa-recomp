// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>

PSP_MODULE_INFO("PSP_A1_EXC_PROBE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef A1_BUILD_COMMIT
#error A1_BUILD_COMMIT is required
#endif

extern const unsigned char a1_break_instruction[];

__attribute__((noinline)) static void a1_raise_break(void) {
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        "li $4, 0x11111111\n\t"
        "li $5, 0x22222222\n\t"
        "li $6, 0x33333333\n\t"
        "li $7, 0x44444444\n\t"
        "li $8, 0xaaaaaaaa\n\t"
        "li $9, 0xbbbbbbbb\n\t"
        "li $10, 0xcccccccc\n\t"
        "li $11, 0xdddddddd\n\t"
        ".globl a1_break_instruction\n"
        "a1_break_instruction:\n\t"
        ".word 0x0016968d\n\t"
        ".set pop\n\t"
        : : : "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "memory");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char header[512];
    int length = snprintf(header, sizeof(header),
        "probe_id=PSP-A1 run_id=PSP-A1-01 build_commit=%s\n"
        "break_address=0x%08x instruction=0x0016968d code20=0x05a5a\n"
        "a0=11111111 a1=22222222 a2=33333333 a3=44444444 "
        "t0=aaaaaaaa t1=bbbbbbbb t2=cccccccc t3=dddddddd\n",
        A1_BUILD_COMMIT, (unsigned int)(uintptr_t)a1_break_instruction);
    if (length < 0 || (size_t)length >= sizeof(header)) return 1;
    SceUID fd = sceIoOpen("host0:/a1_header.txt",
                         PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    if (fd < 0) return 2;
    int written = sceIoWrite(fd, header, (SceSize)length);
    int closed = sceIoClose(fd);
    if (written != length || closed < 0) return 3;
    a1_raise_break();
    return 4;
}
