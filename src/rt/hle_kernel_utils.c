// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Kernel utilities: memset, memcpy and strlen, the std handles, kernel printf, and GPI/GPO.
 * Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* 0x00061e74 is the game's newlib FILE write callback used by the module-processing log
 * stream. It is a valid MIPS function entry (jr ra; move v0,zero), but lies in the four-byte
 * gap between codegen's f_00061e4c and f_00061e7c discoveries and therefore misses sr_lookup.
 * The actual callback ABI is write(cookie, buffer, byte_count). Treat the diagnostic stream as
 * a sink and report the complete byte count; returning zero makes __sfvwrite_r retry/stall.
 * sr_syscall applies the standard caller-saved/HI/LO DEADBEEF poison after this returns. */
static uint32_t h_ModuleStreamWrite(CpuState *s) {
    uint32_t buf = s->r[4];
    if (sr_inrange(buf)) {
        char temp[512];
        int i;
        for (i = 0; i < 511; i++) {
            if (!sr_inrange(buf + i)) break;
            char c = (char)MEM_R8(buf + i);
            if (c == '\0') { temp[i] = '\0'; break; }
            temp[i] = c;
        }
        temp[i] = '\0';
        fprintf(stderr, "MODULE_LOG: %s\n", temp);
        fflush(stderr);
    }
    return s->r[6];
}
static uint32_t h_Memset(CpuState *s) {
    /* a0=dst, a1=byte_val, a2=size */
    uint32_t dst = A0, val = A1 & 0xff, n = A2;
    /* A CPU fill: a span through a swizzled VRAM mirror lands where the
     * same scalar stores would (sr_guest_fill, #1049). */
    if (n && sr_guest_fill(dst, (uint8_t)val, n)) {
        if (g_sr_heap_watch) sr_heap_note_bulk_write(dst, n, 0u);
    } else if (n) {
        sr_oor(dst, val, 1);
    }
    return dst;
}
static uint32_t h_Strlen(CpuState *s) {
    (void)s;
    if (!A0) {
        sr_oor(A0, 0u, 0);
        return 0u;
    }
    uint32_t readable = sr_guest_span_prefix(A0, 0xFFFFFFFFu);
    if (!readable) {
        sr_oor(A0, 0u, 0);
        return 0u;
    }
    for (uint32_t i = 0; i < readable; i++) {
        if (sr_r8(A0 + i) == 0u) return i;
    }
    sr_oor(A0, 0u, 0);
    return 0u;
}
static uint32_t h_Memcpy(CpuState *s) {
    /* a0=dst, a1=src, a2=size */
    uint32_t dst = A0, src = A1, n = A2;
    /* A CPU copy with memmove semantics; spans through a swizzled VRAM
     * mirror are copied in measured-contiguous pieces (sr_guest_memmove, #1049). */
    if (n && sr_guest_memmove(dst, src, n)) {
        if (g_sr_heap_watch) sr_heap_note_bulk_write(dst, n, 0u);
    } else if (n) {
        sr_oor(dst, 0u, 1);
        sr_oor(src, 0u, 0);
    }
    return dst;
}
/* PSP's three std-fd imports intentionally alias the stdout handle (1).  The
 * descriptor table still reserves 0/1/2 with distinct standard identities so
 * ordinary file allocation and I/O dispatch never infer identity from a number. */
static uint32_t h_StdFd(CpuState *s) { (void)s; return 1; }

static uint32_t h_KernelPrintf(CpuState *s) {
    char fmt_check[1024];   /* the whole format must be readable, NUL-terminated guest text */
    if (!guest_cstr(A0, fmt_check, sizeof(fmt_check)))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    /* printf formats its variadic words ($5 onward) and returns the formatted
     * length. The PSP debug console is the host log here; the guest sees only the
     * return value. */
    char line[1024];
    int written = sr_guest_format_host(s, A0, 5u, line, (int)sizeof(line));
    fprintf(stderr, "GAMELOG: %s\n", line);
    return (uint32_t)written;
}

/* General-purpose I/O pins (TD-24 batch 4). sceKernelSetGPO stores the
 * output latch; sceKernelGetGPI reads the input pins, which are always 0 on
 * retail PSP (see the previous registration comment). The two are independent
 * hardware: storing a GPO value must never surface from GetGPI, and the
 * regression pins exactly that separation. Whether firmware validates or
 * masks the stored output bits is UNMEASURED: the value is retained
 * verbatim. */
static uint32_t s_gpo_latch = 0u;
static uint32_t s_gpi_pins = 0u;
static uint32_t h_SetGPO(CpuState *s) { s_gpo_latch = A0; return 0; }
static uint32_t h_GetGPI(CpuState *s) { (void)s; return s_gpi_pins; }
#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only view of the retained GPO latch and synthetic GPI pins. Adds no production behaviour. */
uint32_t sr_hle_test_gpo_value(void) { return s_gpo_latch; }
void sr_hle_test_gpo_reset(void) { s_gpo_latch = 0u; }
uint32_t sr_hle_test_gpi_value(void) { return s_gpi_pins; }
void sr_hle_test_gpi_set(uint32_t val) { s_gpi_pins = val; }
void sr_hle_test_gpi_reset(void) { s_gpi_pins = 0u; }
#endif
void hle_register_gpi_gpo_handlers(void) {
    sr_hle_register(0x6ad345d7, "sceKernelSetGPO", h_SetGPO);
    /* GPI reads hardware general-purpose input pins; always 0 on retail PSP. */
    sr_hle_register(0x37fb5c42, "sceKernelGetGPI", h_GetGPI);
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_bulk_memory_handlers(). */
void hle_register_bulk_memory_kernel_utils(void) {
    sr_hle_register(0xa089eca4, "sceKernelMemset", h_Memset);
    sr_hle_register(0x10f3bb61, "memset", h_Memset);
    sr_hle_register(0x52df196c, "strlen", h_Strlen);
    sr_hle_register(0x1839852a, "sceKernelMemcpy", h_Memcpy);
}

#ifndef SR_MPEG_MEDIA_SELFTEST
#ifndef SR_HLE_THREAD_SELFTEST
/* From sr_hle_init(). */
void hle_register_init_kernel_utils_1(void) {
    sr_hle_register(0x00061e74u, "newlibModuleStreamWrite", h_ModuleStreamWrite);
}
#endif
#endif

#ifndef SR_MPEG_MEDIA_SELFTEST
#ifndef SR_HLE_THREAD_SELFTEST
/* From sr_hle_init(). */
void hle_register_init_kernel_utils_2(void) {
    sr_hle_register(0x13a5abef, "sceKernelPrintf", h_KernelPrintf);
    sr_hle_register(0xcab439df, "printf", h_KernelPrintf);
    sr_hle_register(0x172d316e, "sceKernelStdin", h_StdFd);
    sr_hle_register(0xa6bab2e9, "sceKernelStdout", h_StdFd);
    sr_hle_register(0xf78ba90a, "sceKernelStderr", h_StdFd);
}
#endif
#endif
