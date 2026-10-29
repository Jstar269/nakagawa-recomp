// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// HLE issue cells for the PSP-3000 oracle, one PRX and one launch per case:
//   CASE=hle-issues-batch (PSP-HLE-ISSUES-001): #356 model getters and #326 random-unit
//     carry and vscmp NaN/infinity cells;
//   CASE=hle-intr-cells (PSP-HLE-INTR-001): #325 VTimer/alarm handler return values and
//     VBLANK sub-interrupt delivery while interrupts are suspended or a handler holds;
//   CASE=vfpu-forms (PSP-VFPU-FORMS-001): #326 raw result words of vh2f, vuc2i, vsocp,
//     vwbn, vrndf1 and vrndf2, register-only;
//   CASE=vfpu-funnel (PSP-VFPU-FUNNEL-001): #326 raw result words of vfad and vavg at
//     .p, .t and .q, register-only;
//   CASE=vfpu-sort (PSP-VFPU-SORT-001): #326 raw result words of vsrt1.q to vsrt4.q,
//     register-only.
// These cases live apart from probe_hle_measure.c because they do what that file's
// families never may: hle-intr-cells registers a VBLANK sub-interrupt handler, sets
// VTimer and alarm handlers and suspends interrupts, and hle-issues-batch calls a kernel
// getter through the ARK kernel bridge. Each case removes what it installs before it
// returns: every VTimer is stopped and deleted, every alarm UID cancelled, the
// sub-interrupt handler disabled and released, and every suspension resumed.
//
// Record stream and log: the same as probe_hle_measure.c, whose writer, round trip and
// module lifecycle are repeated here byte for byte (tools/test_psp_oracle.py pins the
// two copies). NAKAGAWA_PSP_META, a NAKAGAWA_PSP_STEP before each risky cell, one
// NAKAGAWA_PSP_TEST per measurement, a `<case>-done` terminal record whose out0 counts
// the measurements, the host0 round trip, and a final NAKAGAWA_PSP_COMPLETE line. Every
// line is appended to the case's host0 log durably (open, append, close). Steps are
// never written inside a timed cell, so host0 I/O cannot shift what the cell measures.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
#if PSP_ORACLE_CASE == 106 || PSP_ORACLE_CASE == 108 || PSP_ORACLE_CASE == 118 || PSP_ORACLE_CASE == 120 || PSP_ORACLE_CASE == 121
/* hle-issues-batch, vfpu-forms, vfpu-funnel, vfpu-sort and vfpu-forms2 run VFPU words on the main thread;
   without the VFPU attribute the first VFPU instruction traps (probe.c's vfpu-compare case, measured). */
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
#else
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
#endif
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define HLE_CASE_ISSUES_BATCH 106
#define HLE_CASE_INTR_CELLS 107
#define HLE_CASE_VFPU_FORMS 108
#define HLE_CASE_VFPU_FUNNEL 118
#define HLE_CASE_VFPU_SORT 120
#define HLE_CASE_VFPU_FORMS2 121

#if PSP_ORACLE_CASE == HLE_CASE_ISSUES_BATCH
#define HLE_CAMPAIGN_ID "hle-issues-batch"
#define HLE_TEST_ID "PSP-HLE-ISSUES-001"
#define HLE_LOG "host0:/hle_issues_batch_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_INTR_CELLS
#define HLE_CAMPAIGN_ID "hle-intr-cells"
#define HLE_TEST_ID "PSP-HLE-INTR-001"
#define HLE_LOG "host0:/hle_intr_cells_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_FORMS
#define HLE_CAMPAIGN_ID "vfpu-forms"
#define HLE_TEST_ID "PSP-VFPU-FORMS-001"
#define HLE_LOG "host0:/vfpu_forms_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_FUNNEL
#define HLE_CAMPAIGN_ID "vfpu-funnel"
#define HLE_TEST_ID "PSP-VFPU-FUNNEL-001"
#define HLE_LOG "host0:/vfpu_funnel_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_SORT
#define HLE_CAMPAIGN_ID "vfpu-sort"
#define HLE_TEST_ID "PSP-VFPU-SORT-001"
#define HLE_LOG "host0:/vfpu_sort_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_FORMS2
#define HLE_CAMPAIGN_ID "vfpu-forms2"
#define HLE_TEST_ID "PSP-VFPU-FORMS2-001"
#define HLE_LOG "host0:/vfpu_forms2_log.txt"
#else
#error probe_hle_issues.c has no case for this PSP_ORACLE_CASE
#endif
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

#define HLE_SENTINEL 0xA5A5A5A5u
/* The same value as text, for the assembly wrappers (IC_V0_WRAPPER). */
#define HLE_SENTINEL_HEX "0xA5A5A5A5"

static int s_log_failed;
static uint32_t s_records;

/* Durable line writer: open, write everything, close. A later hang or fault
   keeps every line already written. `truncate` starts a fresh log. */
static void hle_write(const char *line, int truncate)
{
    const int flags = PSP_O_WRONLY | PSP_O_CREAT | (truncate ? PSP_O_TRUNC : PSP_O_APPEND);
    const SceUID fd = sceIoOpen(HLE_LOG, flags, 0777);
    if (fd < 0) {
        s_log_failed = 1;
        return;
    }
    const size_t length = strlen(line);
    size_t offset = 0;
    while (offset < length) {
        const int wrote = sceIoWrite(fd, line + offset, (SceSize)(length - offset));
        if (wrote <= 0) {
            s_log_failed = 1;
            break;
        }
        offset += (size_t)wrote;
    }
    if (sceIoClose(fd) < 0) {
        s_log_failed = 1;
    }
}

static void hle_begin(void)
{
    static const char meta[] =
        "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
        "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
        "source_commit=" PROBE_BUILD_COMMIT " fixture=hle-measure\n";
    hle_write(meta, 1);
}

/* Progress marker written durably before a call that could hang or fault. The
   step name is the measurement case about to run, so a hang names its cell. */
static void hle_step(const char *step)
{
    static char line[200];
    snprintf(line, sizeof(line), "NAKAGAWA_PSP_STEP schema=1 case_id=%s step=%s\n",
             HLE_CAMPAIGN_ID, step);
    hle_write(line, 0);
}

static void hle_emit(const char *case_id, const char *status, uint32_t result,
                     const uint32_t *out, uint32_t count)
{
    static char line[700];
    int n = snprintf(line, sizeof(line),
                     "NAKAGAWA_PSP_TEST schema=1 test_id=%s case_id=%s status=%s result=0x%08x",
                     HLE_TEST_ID, case_id, status, (unsigned int)result);
    for (uint32_t i = 0; i < count && n > 0 && n < (int)sizeof(line) - 32; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " out%u=0x%08x", (unsigned int)i,
                      (unsigned int)out[i]);
    }
    if (n > 0 && n < (int)sizeof(line) - 2) {
        line[n] = '\n';
        line[n + 1] = '\0';
        hle_write(line, 0);
    }
}

static void hle_record(const char *case_id, const char *status, uint32_t result,
                       const uint32_t *out, uint32_t count)
{
    s_records++;
    hle_emit(case_id, status, result, out, count);
}

/* The host0 round trip probe.c's teardown performs (PROBE_HOST0_ROUNDTRIP_PATH and
   probe_host0_roundtrip there): 64 bytes, byte i = (0x5A ^ (i * 0x25 + (i >> 3))) & 0xFF,
   written with truncate, then read back and compared. The runner's
   _verify_host0_roundtrip reads the file after unload, so every family leaves it.
   probe.c is a separate translation unit, so the contract is repeated here and
   tools/test_psp_oracle.py pins both copies. */
#define HLE_ROUNDTRIP_PATH "host0:/nakagawa_transport_write.bin"
#define HLE_ROUNDTRIP_BYTES 64u

static int hle_host0_roundtrip(void)
{
    uint8_t expected[HLE_ROUNDTRIP_BYTES];
    uint8_t observed[HLE_ROUNDTRIP_BYTES];
    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = (uint8_t)(0x5Au ^ (i * 0x25u + (i >> 3)));
    }
    memset(observed, 0, sizeof(observed));
    SceUID fd = sceIoOpen(HLE_ROUNDTRIP_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return 0;
    }
    const int written = sceIoWrite(fd, expected, (SceSize)sizeof(expected));
    const int write_close = sceIoClose(fd);
    if (written != (int)sizeof(expected) || write_close < 0) {
        return 0;
    }
    fd = sceIoOpen(HLE_ROUNDTRIP_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    const int read = sceIoRead(fd, observed, (SceSize)sizeof(observed));
    const int read_close = sceIoClose(fd);
    return read == (int)sizeof(observed) && read_close >= 0 &&
           memcmp(expected, observed, sizeof(expected)) == 0;
}

/* Shared end of run, after every measurement and its cleanup. The round trip
   is host I/O only, so it cannot change a cell already recorded. As in probe.c's
   teardown, a failed round trip makes the completion status FAIL. */
static void hle_finish(int ok)
{
    const uint32_t count = s_records;
    hle_emit(HLE_DONE_ID, "PASS", 0, &count, 1);
    const int roundtrip = hle_host0_roundtrip();
    static char line[96];
    snprintf(line, sizeof(line), "NAKAGAWA_PSP_COMPLETE schema=1 status=%s\n",
             (ok && !s_log_failed && roundtrip) ? "PASS" : "FAIL");
    hle_write(line, 0);
}

/* Module lifecycle, as in probe.c: main parks until module_stop asks it to end,
   then runs the CRT's de-initialisation and exits; module_stop deletes it. */
static volatile SceUID s_hle_main_thread = -1;
static volatile int s_hle_stop_requested;
extern void _fini(void);
extern void __libcglue_deinit(void);
#define HLE_MAIN_STOP_TIMEOUT_US 1000000u

static void hle_park_until_stop(void)
{
    while (!s_hle_stop_requested) {
        sceKernelSleepThread();
    }
    _fini();
    __libcglue_deinit();
    sceKernelExitThread(0);
}

int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    const SceUID main_thread = s_hle_main_thread;
    if (main_thread < 0) {
        return 1;
    }
    s_hle_stop_requested = 1;
    (void)sceKernelWakeupThread(main_thread);
    SceUInt timeout = HLE_MAIN_STOP_TIMEOUT_US;
    if (sceKernelWaitThreadEnd(main_thread, &timeout) < 0) {
        return 1;
    }
    if (sceKernelDeleteThread(main_thread) < 0) {
        return 1;
    }
    s_hle_main_thread = -1;
    return 0;
}

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_ISSUES_BATCH

/* The 2026-10-24 NEEDS-PSP issue batch (PSP-HLE-ISSUES-001), one launch:
   - #356: kuKernelGetModel and sceKernelGetModel side by side, raw;
   - #326: the random unit's carry, one vrndi.s step from 2 x 448 written RCX states, and
     vscmp over NaN, infinity, signed-zero and denormal lanes at every width.
   Both #326 sections run twice in the same launch (pass 0 and pass 1) for repeatability.
   Nothing here touches device state outside the VFPU registers of this thread. */

#include <kubridge.h>
#include <systemctrl.h>
#include "hle_vfpu_cells.h"

/* ---- #356: model getters ---------------------------------------------------- */

/* sceKernelGetModel's user export: SysMemUserForUser NID 0x6373995d (src/rt/nid_names.h).
   PSPSDK ships the name only as a kernel prototype, so the stub is written here. On this
   firmware an unresolved user import returns the caller's $v0 unchanged (measured
   2026-10-23), so the call goes through a wrapper that presets $v0 to HLE_SENTINEL, and
   each row also records the stub's two words as the loader left them: a linked user
   import is `jr $ra` followed by a `syscall` in the delay slot, an unlinked one keeps its
   NID-table words. */
__asm__(
    ".set push\n\t"
    ".section .rodata.sceResident, \"a\"\n\t"
    ".word 0\n"
    "__stub_modulestr_ic_SysMemUserForUser:\n\t"
    ".asciz \"SysMemUserForUser\"\n\t"
    ".align 2\n\t"
    ".section .lib.stub, \"a\", @progbits\n\t"
    ".global __stub_module_ic_SysMemUserForUser\n"
    "__stub_module_ic_SysMemUserForUser:\n\t"
    ".word __stub_modulestr_ic_SysMemUserForUser\n\t"
    ".word 0x40010000\n\t"
    ".word 0x5\n\t"
    ".word __executable_start\n\t"
    ".word __executable_start\n\t"
    ".set pop\n\t"
    ".set push\n\t"
    ".set noreorder\n\t"
    ".section .sceStub.text, \"ax\", @progbits\n\t"
    ".globl ic_sceKernelGetModel\n\t"
    ".type ic_sceKernelGetModel, @function\n\t"
    ".ent ic_sceKernelGetModel, 0\n"
    "ic_sceKernelGetModel:\n\t"
    ".word __stub_module_ic_SysMemUserForUser\n\t"
    ".word 0x6373995d\n\t"
    ".end ic_sceKernelGetModel\n\t"
    ".size ic_sceKernelGetModel, .-ic_sceKernelGetModel\n\t"
    ".section .rodata.sceNid, \"a\"\n\t"
    ".word 0x6373995d\n\t"
    ".set pop\n\t"
    ".text\n");

int ic_sceKernelGetModel(void);

/* wrapper(out): presets $v0 to HLE_SENTINEL, calls the import with no arguments and
   stores the returned $v0 to out[0]. Same frame discipline as HLE_PFV_WRAPPER. The preset
   is before the jal, never in its delay slot: `li` of a 32-bit value is two instructions,
   and the second would run after the call (measured: the first build OR'd 0xa5a5 into
   every result). */
#define IC_V0_WRAPPER(wrapper, target)                                        \
    __asm__(                                                                  \
        ".pushsection .text\n\t"                                              \
        ".set push\n\t"                                                       \
        ".set noreorder\n\t"                                                  \
        ".globl " #wrapper "\n\t"                                             \
        ".ent " #wrapper "\n"                                                 \
        #wrapper ":\n\t"                                                      \
        "addiu $sp, $sp, -32\n\t"                                             \
        "sw $ra, 28($sp)\n\t"                                                 \
        "sw $a0, 24($sp)\n\t"                                                 \
        "li $v0, " HLE_SENTINEL_HEX "\n\t"                                    \
        "jal " #target "\n\t"                                                 \
        "nop\n\t"                                                             \
        "lw $t0, 24($sp)\n\t"                                                 \
        "sw $v0, 0($t0)\n\t"                                                  \
        "lw $ra, 28($sp)\n\t"                                                 \
        "addiu $sp, $sp, 32\n\t"                                              \
        "jr $ra\n\t"                                                          \
        "nop\n\t"                                                             \
        ".end " #wrapper "\n\t"                                               \
        ".set pop\n\t"                                                        \
        ".popsection\n")

IC_V0_WRAPPER(ic_v0_sce_get_model, ic_sceKernelGetModel);
IC_V0_WRAPPER(ic_v0_ku_get_model, kuKernelGetModel);
void ic_v0_sce_get_model(uint32_t *out);
void ic_v0_ku_get_model(uint32_t *out);

/* result = the returned $v0 ($v0 preset to HLE_SENTINEL); out0/out1 = the import stub's
   two words after loading; out2 = the firmware word (sceKernelDevkitVersion). */
static void ic_model_row(const char *case_id, void (*call)(uint32_t *), const void *stub)
{
    hle_step(case_id);
    uint32_t v0 = 0;
    call(&v0);
    const volatile uint32_t *s = (const volatile uint32_t *)stub;
    const uint32_t w[3] = {s[0], s[1], (uint32_t)sceKernelDevkitVersion()};
    hle_record(case_id, "PASS", v0, w, 3);
}

/* The kernel getter itself: SysMemForKernel's sceKernelGetModel (6.60 NID 0x07c586a1; the
   PSPSDK's older NID 0x6373995d), from libpspsysmem_kernel_660.a / libpspkernel.a
   .rodata.sceNid, found with ARK's sctrlHENFindFunction and called through kuKernelCall.
   It only reads a kernel global. The call is made only when the address lies in kernel
   RAM. Row model-kernel-get-model-<nid>: result = the getter's return (ret1, preset to
   HLE_SENTINEL); out0 = the address found (0 = not found); out1 = kuKernelCall's own
   return (0xffffffff when not called); out2 = the NID. */
static void ic_model_kernel(void)
{
    static const uint32_t nids[2] = {0x07c586a1u, 0x6373995du};
    char name[48];
    for (unsigned n = 0; n < 2u; n++) {
        snprintf(name, sizeof(name), "model-kernel-get-model-%08x", (unsigned int)nids[n]);
        hle_step(name);
        const uint32_t addr =
            (uint32_t)sctrlHENFindFunction("sceSystemMemoryManager", "SysMemForKernel", nids[n]);
        uint32_t rc = 0xffffffffu;
        KernelCallArg arg;
        memset(&arg, 0, sizeof(arg));
        arg.ret1 = HLE_SENTINEL;
        if (addr >= 0x88000000u && addr < 0x8c000000u) {
            rc = (uint32_t)kuKernelCall((void *)addr, &arg);
        }
        const uint32_t w[3] = {addr, rc, nids[n]};
        hle_record(name, "PASS", arg.ret1, w, 3);
    }
}

static void ic_model(void)
{
    ic_model_row("model-ku-get-model", ic_v0_ku_get_model, (const void *)kuKernelGetModel);
    ic_model_row("model-sce-get-model", ic_v0_sce_get_model, (const void *)ic_sceKernelGetModel);
    ic_model_row("model-ku-get-model-again", ic_v0_ku_get_model, (const void *)kuKernelGetModel);
    ic_model_row("model-sce-get-model-again", ic_v0_sce_get_model, (const void *)ic_sceKernelGetModel);
    ic_model_kernel();
}

/* ---- #326: random unit carry ------------------------------------------------ */

/* The 2026-10-23 rows fit this state layout: RCX0-7 hold four 32-bit components C0-C3,
   Ck = (RCX(k+4) & 0xffff) << 16 | (RCXk & 0xffff), and a carry whose nibble k sits in
   bits 16-19 of RCXk. Every rule but the next carry out of the C3 step is fixed. Each
   cell writes one state with mtvc, reads RCX back (did the write take?), executes one
   vrndi.s S000 and reads RCX again. Nothing else runs between. */
static const uint32_t s_ic_c2[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xfffffffeu, 0xffffffffu, 0x907f6e50u, 0x5a5a5a5au};
static const uint32_t s_ic_c3[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xc0000000u, 0xffffffffu, 0xc4d5e6d8u, 0x89abcdefu};
static const uint32_t s_ic_carry[7] = {0x0u, 0x1u, 0x2u, 0x3u, 0xfu, 0x10u, 0xffffu};
/* Second grid: carries wider than 16 bits (all eight nibbles). */
static const uint32_t s_ic_carry_big[7] = {0x00010000u, 0x40000000u, 0x7fff0000u, 0x80000000u,
                                           0x12345678u, 0xfffffffcu, 0xffffffffu};
#define IC_C0 0x2468ace0u
#define IC_C1 0x13579bdfu

/* The carry is 32 bits: nibble k in bits 16-19 of RCXk, k = 0..7 (a seed writes all eight;
   the 2026-10-23 rows show nibbles in RCX4-7 after seeding 0x80000000 and 0xffffffff). */
static void ic_rcx_image(uint32_t *rcx, const uint32_t c[4], uint32_t carry)
{
    for (unsigned k = 0; k < 8u; k++) {
        const uint32_t half = (c[k & 3u] >> (k < 4u ? 0u : 16u)) & 0xffffu;
        rcx[k] = 0x3f800000u | (((carry >> (4u * k)) & 0xfu) << 16) | half;
    }
}

/* out: C000 lanes 0-3 after (lane 0 = the value; lanes 1-3 keep the sentinel), RCX0-7
   after the write, RCX0-7 after the step (20 words). */
static void ic_rnd_step(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out)
{
    __asm__ volatile(
        "lv.q C000, 0(%[sen])\n\t"
        TN_RCX_WRITE("%[st]")
        "addiu $t0, %[out], 16\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0210000\n\t" /* vrndi.s S000 */
        "sv.q C000, 0(%[out])\n\t"
        "addiu $t0, %[out], 48\n\t"
        TN_RCX_READ("$t0")
        :
        : [st] "r"(rcx_in), [sen] "r"(sentinel), [out] "r"(out)
        : "t0", "t1", "memory");
}

/* Row <stem>-p<pass>-<i>-<j>-<k> (C2 = s_ic_c2[i], C3 = s_ic_c3[j], carry = carries[k];
   C0/C1 = IC_C0/IC_C1; stem rcar = s_ic_carry, rcbig = s_ic_carry_big): result = the
   vrndi.s value (C000 lane 0); out0-7 = RCX0-7 after the write; out8-15 = RCX0-7 after the
   step; out16 = 1 when C000 lanes 1-3 all kept the sentinel. */
static void ic_rnd_carry(unsigned pass, const char *stem, const uint32_t *carries)
{
    static const uint32_t sentinel[4] __attribute__((aligned(16))) = {
        TN_SENTINEL, TN_SENTINEL, TN_SENTINEL, TN_SENTINEL};
    uint32_t rcx[8] __attribute__((aligned(16)));
    uint32_t out[20] __attribute__((aligned(16)));
    char name[48];
    for (unsigned i = 0; i < 8u; i++) {
        snprintf(name, sizeof(name), "%s-p%u-c2-%u", stem, pass, i);
        hle_step(name);
        for (unsigned j = 0; j < 8u; j++) {
            for (unsigned k = 0; k < 7u; k++) {
                const uint32_t c[4] = {IC_C0, IC_C1, s_ic_c2[i], s_ic_c3[j]};
                ic_rcx_image(rcx, c, carries[k]);
                ic_rnd_step(rcx, sentinel, out);
                uint32_t w[17];
                for (unsigned n = 0; n < 16u; n++) {
                    w[n] = out[4u + n];
                }
                w[16] = (out[1] == TN_SENTINEL && out[2] == TN_SENTINEL && out[3] == TN_SENTINEL) ? 1u : 0u;
                snprintf(name, sizeof(name), "%s-p%u-%u-%u-%u", stem, pass, i, j, k);
                hle_record(name, "PASS", out[0], w, 17);
            }
        }
    }
}

/* ---- #326: vscmp ------------------------------------------------------------ */

/* vscmp vd, vs, vt writes the sign of vs - vt per lane. vs = C000 (image 0-3), vt = C100
   (image 16-19), vd = C200 (image 32-35); the same-register form compares C000 with
   itself. Encodings from psp-as (pspdev binutils). */
TN_CELL_FN(ic_vscmp_s, TN_PFX_NONE, "0x6e840008")      /* vscmp.s S200, S000, S100 */
TN_CELL_FN(ic_vscmp_p, TN_PFX_NONE, "0x6e840088")      /* vscmp.p C200, C000, C100 */
TN_CELL_FN(ic_vscmp_t, TN_PFX_NONE, "0x6e848008")      /* vscmp.t C200, C000, C100 */
TN_CELL_FN(ic_vscmp_q, TN_PFX_NONE, "0x6e848088")      /* vscmp.q C200, C000, C100 */
TN_CELL_FN(ic_vscmp_q_same, TN_PFX_NONE, "0x6e808088") /* vscmp.q C200, C000, C000 */

typedef void (*ic_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

static const struct {
    const char *name;
    uint32_t vs[4];
    uint32_t vt[4];
} s_ic_vscmp_sets[6] = {
    {"qnan-inf", {0x7fc00000u, 0x7fc00000u, 0x7f800000u, 0xff800000u},
                 {0x7fc00000u, 0x7f800000u, 0x7fc00000u, 0x7fc00000u}},
    {"nan-kinds", {0x7f800001u, 0xffc00000u, 0x7fffffffu, 0x7f800001u},
                  {0x3f800000u, 0xbf800000u, 0x7fffffffu, 0xff800001u}},
    {"inf-inf", {0x7f800000u, 0xff800000u, 0x7f800000u, 0xff800000u},
                {0x7f800000u, 0xff800000u, 0xff800000u, 0x7f800000u}},
    {"zero-nan", {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u},
                 {0x80000000u, 0x00000000u, 0x7fc00000u, 0xffc00000u}},
    {"denorm-finite", {0x00000001u, 0x80000001u, 0x40400000u, 0x40000000u},
                      {0x00000000u, 0x00000000u, 0x40000000u, 0x40400000u}},
    {"nan-mixed", {0x7f800001u, 0x3f800000u, 0x7fc00000u, 0xff800000u},
                  {0xff800001u, 0x7f800001u, 0xff800000u, 0x7fc00000u}},
};

static const struct {
    const char *name;
    uint32_t word;
    ic_cell_fn fn;
} s_ic_vscmp_forms[5] = {
    {"vscmp.s", 0x6e840008u, ic_vscmp_s}, {"vscmp.p", 0x6e840088u, ic_vscmp_p},
    {"vscmp.t", 0x6e848008u, ic_vscmp_t}, {"vscmp.q", 0x6e848088u, ic_vscmp_q},
    {"vscmp.q-same", 0x6e808088u, ic_vscmp_q_same},
};

static uint32_t s_ic_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_ic_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* Row vscmp-p<pass>-<set>-<form>: result = the instruction word; out0-3 = C200 lanes after
   (lanes past the width keep their background value); out4/out5 = VFPU_CC after the preset
   / after the word; out6-9 = vs lanes; out10-13 = vt lanes; out14 = number of image words
   outside C200 that changed; out15 = the first such index (0xffffffff when none). */
static void ic_vscmp(unsigned pass)
{
    char name[64];
    for (unsigned s = 0; s < 6u; s++) {
        snprintf(name, sizeof(name), "vscmp-p%u-%s", pass, s_ic_vscmp_sets[s].name);
        hle_step(name);
        for (unsigned f = 0; f < 5u; f++) {
            for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
                s_ic_in[i] = 0x3f800000u | (i << 16);
            }
            for (unsigned l = 0; l < 4u; l++) {
                s_ic_in[0u + l] = s_ic_vscmp_sets[s].vs[l];
                s_ic_in[16u + l] = s_ic_vscmp_sets[s].vt[l];
            }
            uint32_t cc[2] = {0u, 0u};
            s_ic_vscmp_forms[f].fn(s_ic_in, s_ic_out, cc);
            uint32_t changed = 0u, first = 0xffffffffu;
            for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
                if ((i < 32u || i > 35u) && s_ic_out[i] != s_ic_in[i]) {
                    if (changed++ == 0u) {
                        first = i;
                    }
                }
            }
            const uint32_t w[16] = {
                s_ic_out[32], s_ic_out[33], s_ic_out[34], s_ic_out[35], cc[0], cc[1],
                s_ic_in[0], s_ic_in[1], s_ic_in[2], s_ic_in[3],
                s_ic_in[16], s_ic_in[17], s_ic_in[18], s_ic_in[19], changed, first,
            };
            snprintf(name, sizeof(name), "vscmp-p%u-%s-%s", pass, s_ic_vscmp_sets[s].name,
                     s_ic_vscmp_forms[f].name);
            hle_record(name, "PASS", s_ic_vscmp_forms[f].word, w, 16);
        }
    }
}

static int hle_run(void)
{
    ic_model();
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        ic_vscmp(pass);
        ic_rnd_carry(pass, "rcar", s_ic_carry);
        ic_rnd_carry(pass, "rcbig", s_ic_carry_big);
    }
    return 1;
}

#endif /* HLE_CASE_ISSUES_BATCH */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_FORMS

/* #326 VFPU forms (PSP-VFPU-FORMS-001), one launch, register-only: the raw result words of
   vh2f.s/.p (half pairs to float), vuc2i.s (bytes to words), vsocp.s/.p, vwbn.s at eight
   exponents, and one vrndf1.s/vrndf2.s draw from written RCX states. Each form runs under no
   S prefix and the two TN_PFX forms, except vwbn (no prefix at every exponent, P1 at 0x7f)
   and the draws (no prefix). Two passes for repeatability. Every word is emitted as a
   word; the encodings were checked against psp-as (pspdev binutils). Nothing outside the
   VFPU registers of this thread is touched. */

#include "hle_vfpu_cells.h"

/* Source C000 (image 0-3, S000 for a single), destination C200 (image 32-35, S200). */
TN_CELL_SET(vf_h2f_s, "0xd0330008")   /* vh2f.s C200.p, S000 */
TN_CELL_SET(vf_h2f_p, "0xd0330088")   /* vh2f.p C200.q, C000.p */
TN_CELL_FN(vf_h2f_alias, TN_PFX_NONE, "0xd0330080") /* vh2f.p C000.q, C000.p (in place) */
TN_CELL_SET(vf_uc2i, "0xd0380008")    /* vuc2i.s C200.q, S000 */
TN_CELL_SET(vf_socp_s, "0xd0450008")  /* vsocp.s C200.p, S000 */
TN_CELL_SET(vf_socp_p, "0xd0450088")  /* vsocp.p C200.q, C000.p */
TN_CELL_FN(vf_wbn_00, TN_PFX_NONE, "0xd3000008") /* vwbn.s S200, S000, 0x00 */
TN_CELL_FN(vf_wbn_01, TN_PFX_NONE, "0xd3010008")
TN_CELL_FN(vf_wbn_7e, TN_PFX_NONE, "0xd37e0008")
TN_CELL_FN(vf_wbn_7f, TN_PFX_NONE, "0xd37f0008")
TN_CELL_FN(vf_wbn_80, TN_PFX_NONE, "0xd3800008")
TN_CELL_FN(vf_wbn_96, TN_PFX_NONE, "0xd3960008")
TN_CELL_FN(vf_wbn_fe, TN_PFX_NONE, "0xd3fe0008")
TN_CELL_FN(vf_wbn_ff, TN_PFX_NONE, "0xd3ff0008")
TN_CELL_FN(vf_wbn_7f_p1, TN_PFX_P1, "0xd37f0008")

typedef void (*vf_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Half pairs, low half first: signed zeros, denormals, the normal limits, 1 and -2,
   infinities, signalling and quiet NaNs, two ordinary values each way. */
static const uint32_t s_vf_halves[12] = {
    0x80000000u, 0x80010001u, 0x020003ffu, 0x3c000400u, 0xc0007bffu, 0xfc007c00u,
    0x7e007c01u, 0xfe007fffu, 0xb7ff3555u, 0x12345640u, 0xfbff8400u, 0xfdff83ffu,
};
/* Byte patterns (two of them float NaN or 1.0 as words). */
static const uint32_t s_vf_bytes[10] = {
    0x00000000u, 0xffffffffu, 0x7f80ff01u, 0x80402010u, 0x01020304u,
    0xfedcba98u, 0x3f800000u, 0x7fc00000u, 0x00000001u, 0x00ff00ffu,
};
/* vsocp operands: signed zeros, the [0, 1] edges, beyond both edges, infinities, NaNs,
   denormals, the neighbours of 1, +-2^-24 and 0.75. */
static const uint32_t s_vf_socp[22] = {
    0x00000000u, 0x80000000u, 0x3e800000u, 0x3f000000u, 0x3f800000u, 0x3fc00000u,
    0x40000000u, 0xbf000000u, 0xbf800000u, 0xc0000000u, 0x7f800000u, 0xff800000u,
    0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x00000001u, 0x80000001u, 0x3f7fffffu,
    0x3f800001u, 0x33800000u, 0xb3800000u, 0x3f400000u,
};
/* vwbn operands: zeros, small and large normals each way, powers of two around 2^23,
   the normal limits, denormals, infinities, NaNs and two arbitrary words. */
static const uint32_t s_vf_wbn[28] = {
    0x00000000u, 0x80000000u, 0x3f800000u, 0x3fc00000u, 0x40000000u, 0x40400000u,
    0x40b00000u, 0x3f000000u, 0x3f400000u, 0x3e800000u, 0x2edbe6ffu, 0x501502f9u,
    0x4b000000u, 0x4b800001u, 0x4e800000u, 0xc0500000u, 0xbe99999au, 0x7f7fffffu,
    0x00800000u, 0x00400000u, 0x00000001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
    0x7f800001u, 0x40490fdbu, 0x12345678u, 0x8abcdef0u,
};

static const struct {
    const char *name;
    vf_cell_fn fn;
    uint32_t word;
} s_vf_wbn_forms[9] = {
    {"00", vf_wbn_00, 0xd3000008u}, {"01", vf_wbn_01, 0xd3010008u},
    {"7e", vf_wbn_7e, 0xd37e0008u}, {"7f", vf_wbn_7f, 0xd37f0008u},
    {"80", vf_wbn_80, 0xd3800008u}, {"96", vf_wbn_96, 0xd3960008u},
    {"fe", vf_wbn_fe, 0xd3fe0008u}, {"ff", vf_wbn_ff, 0xd3ff0008u},
    {"7f-p1", vf_wbn_7f_p1, 0xd37f0008u},
};

static const char *const s_vf_pfx_names[3] = {"none", "p1", "p2"};
static const uint32_t s_vf_pfx_words[3] = {TN_PFXW_NONE, TN_PFXW_P1, TN_PFXW_P2};

static uint32_t s_vf_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_vf_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One cell: background image word i = 0x3f800000 | i << 16, source lanes a/b at image 0/1
   (lanes 2/3 keep the background), then the word. Row <name>: result = the instruction
   word; out0-3 = image dest..dest+3 after (C200 = 32, or C000 = 0 for the in-place form);
   out4/out5 = VFPU_CC after the preset / after the word; out6-9 = image 0-3 before;
   out10 = number of image words outside dest..dest+3 that changed; out11 = the first such
   index (0xffffffff when none); out12 = the vpfxs word issued before it (0 = none). */
static void vf_cell(const char *name, vf_cell_fn fn, uint32_t word, uint32_t pfx, uint32_t a,
                    uint32_t b, unsigned dest)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_vf_in[i] = 0x3f800000u | (i << 16);
    }
    s_vf_in[0] = a;
    s_vf_in[1] = b;
    uint32_t cc[2] = {0u, 0u};
    fn(s_vf_in, s_vf_out, cc);
    uint32_t changed = 0u, first = 0xffffffffu;
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        if ((i < dest || i > dest + 3u) && s_vf_out[i] != s_vf_in[i]) {
            if (changed++ == 0u) {
                first = i;
            }
        }
    }
    const uint32_t w[13] = {
        s_vf_out[dest], s_vf_out[dest + 1u], s_vf_out[dest + 2u], s_vf_out[dest + 3u],
        cc[0], cc[1], s_vf_in[0], s_vf_in[1], s_vf_in[2], s_vf_in[3], changed, first, pfx,
    };
    hle_record(name, "PASS", word, w, 13);
}

/* Rows h2f-p<pass>-s-<i>-<pfx> (vh2f.s of s_vf_halves[i]), h2f-p<pass>-p-<i>-<pfx> (vh2f.p
   of halves 2i and 2i+1) and h2f-p<pass>-alias-<i> (the same pair, in place). */
static void vf_h2f(unsigned pass)
{
    static const vf_cell_fn s_fns[3] = {vf_h2f_s_none, vf_h2f_s_p1, vf_h2f_s_p2};
    static const vf_cell_fn p_fns[3] = {vf_h2f_p_none, vf_h2f_p_p1, vf_h2f_p_p2};
    char name[64];
    snprintf(name, sizeof(name), "h2f-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 12u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "h2f-p%u-s-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, s_fns[f], 0xd0330008u, s_vf_pfx_words[f], s_vf_halves[i],
                    TN_SENTINEL, 32u);
        }
    }
    for (unsigned i = 0; i < 6u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "h2f-p%u-p-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, p_fns[f], 0xd0330088u, s_vf_pfx_words[f], s_vf_halves[2u * i],
                    s_vf_halves[2u * i + 1u], 32u);
        }
        snprintf(name, sizeof(name), "h2f-p%u-alias-%u", pass, i);
        vf_cell(name, vf_h2f_alias, 0xd0330080u, TN_PFXW_NONE, s_vf_halves[2u * i],
                s_vf_halves[2u * i + 1u], 0u);
    }
}

/* Rows uc2i-p<pass>-<i>-<pfx>: vuc2i.s of s_vf_bytes[i]. */
static void vf_uc2i(unsigned pass)
{
    static const vf_cell_fn fns[3] = {vf_uc2i_none, vf_uc2i_p1, vf_uc2i_p2};
    char name[64];
    snprintf(name, sizeof(name), "uc2i-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 10u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "uc2i-p%u-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, fns[f], 0xd0380008u, s_vf_pfx_words[f], s_vf_bytes[i], TN_SENTINEL,
                    32u);
        }
    }
}

/* Rows socp-p<pass>-s-<i>-<pfx> (vsocp.s of s_vf_socp[i]) and socp-p<pass>-p-<i>-<pfx>
   (vsocp.p of operands 2i and 2i+1). */
static void vf_socp(unsigned pass)
{
    static const vf_cell_fn s_fns[3] = {vf_socp_s_none, vf_socp_s_p1, vf_socp_s_p2};
    static const vf_cell_fn p_fns[3] = {vf_socp_p_none, vf_socp_p_p1, vf_socp_p_p2};
    char name[64];
    snprintf(name, sizeof(name), "socp-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 22u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "socp-p%u-s-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, s_fns[f], 0xd0450008u, s_vf_pfx_words[f], s_vf_socp[i], TN_SENTINEL,
                    32u);
        }
    }
    for (unsigned i = 0; i < 11u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "socp-p%u-p-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, p_fns[f], 0xd0450088u, s_vf_pfx_words[f], s_vf_socp[2u * i],
                    s_vf_socp[2u * i + 1u], 32u);
        }
    }
}

/* Rows wbn-p<pass>-<form>-<i>: vwbn.s of s_vf_wbn[i] at the form's exponent. */
static void vf_wbn(unsigned pass)
{
    char name[64];
    for (unsigned f = 0; f < 9u; f++) {
        snprintf(name, sizeof(name), "wbn-p%u-%s", pass, s_vf_wbn_forms[f].name);
        hle_step(name);
        for (unsigned i = 0; i < 28u; i++) {
            snprintf(name, sizeof(name), "wbn-p%u-%s-%u", pass, s_vf_wbn_forms[f].name, i);
            vf_cell(name, s_vf_wbn_forms[f].fn, s_vf_wbn_forms[f].word,
                    f == 8u ? TN_PFXW_P1 : TN_PFXW_NONE, s_vf_wbn[i], TN_SENTINEL, 32u);
        }
    }
}

/* ---- random-unit float draws ------------------------------------------------ */

/* RCX image of four 32-bit components and a 32-bit carry, as ic_rcx_image in the
   hle-issues-batch section: Ck = (RCX(k+4) & 0xffff) << 16 | (RCXk & 0xffff), carry nibble
   k in bits 16-19 of RCXk. */
static void vf_rcx_image(uint32_t *rcx, const uint32_t c[4], uint32_t carry)
{
    for (unsigned k = 0; k < 8u; k++) {
        const uint32_t half = (c[k & 3u] >> (k < 4u ? 0u : 16u)) & 0xffffu;
        rcx[k] = 0x3f800000u | (((carry >> (4u * k)) & 0xfu) << 16) | half;
    }
}

/* out: C000 lanes 0-3 after (lane 0 = the draw), RCX0-7 after the write, RCX0-7 after the
   draw (20 words). */
static void vf_rndf1_step(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out)
{
    __asm__ volatile(
        "lv.q C000, 0(%[sen])\n\t"
        TN_RCX_WRITE("%[st]")
        "addiu $t0, %[out], 16\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0220000\n\t" /* vrndf1.s S000 */
        "sv.q C000, 0(%[out])\n\t"
        "addiu $t0, %[out], 48\n\t"
        TN_RCX_READ("$t0")
        :
        : [st] "r"(rcx_in), [sen] "r"(sentinel), [out] "r"(out)
        : "t0", "t1", "memory");
}

static void vf_rndf2_step(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out)
{
    __asm__ volatile(
        "lv.q C000, 0(%[sen])\n\t"
        TN_RCX_WRITE("%[st]")
        "addiu $t0, %[out], 16\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0230000\n\t" /* vrndf2.s S000 */
        "sv.q C000, 0(%[out])\n\t"
        "addiu $t0, %[out], 48\n\t"
        TN_RCX_READ("$t0")
        :
        : [st] "r"(rcx_in), [sen] "r"(sentinel), [out] "r"(out)
        : "t0", "t1", "memory");
}

static const uint32_t s_vf_c2[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xfffffffeu, 0xffffffffu, 0x907f6e50u, 0x5a5a5a5au};
static const uint32_t s_vf_c3[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xc0000000u, 0xffffffffu, 0xc4d5e6d8u, 0x89abcdefu};
static const uint32_t s_vf_carry[8] = {0x0u, 0x1u, 0x2u, 0x3u, 0xfu, 0x10u, 0xffffu,
                                       0x12345678u};

/* Rows rndf<n>-p<pass>-<k>: k < 16 draws from C0 = 0x2468ace0 + k * 0x11111111,
   C1 = 0x13579bdf ^ (k << 20), C2 = s_vf_c2[k & 7], C3 = s_vf_c3[(3k) & 7],
   carry = s_vf_carry[k & 7]; k = 16 from the thread-entry RCX (0x3f800001, 0x3f800002,
   0x3f800004, 0x3f800008, then 0x3f800000 four times). result = the draw (C000 lane 0);
   out0-7 = RCX0-7 after the write; out8-15 = RCX0-7 after the draw; out16 = 1 when C000
   lanes 1-3 all kept the sentinel. */
static void vf_rndf(unsigned pass, unsigned which)
{
    static const uint32_t sentinel[4] __attribute__((aligned(16))) = {
        TN_SENTINEL, TN_SENTINEL, TN_SENTINEL, TN_SENTINEL};
    uint32_t rcx[8] __attribute__((aligned(16)));
    uint32_t out[20] __attribute__((aligned(16)));
    char name[48];
    snprintf(name, sizeof(name), "rndf%u-p%u", which, pass);
    hle_step(name);
    for (unsigned k = 0; k < 17u; k++) {
        if (k < 16u) {
            const uint32_t c[4] = {0x2468ace0u + k * 0x11111111u, 0x13579bdfu ^ (k << 20),
                                   s_vf_c2[k & 7u], s_vf_c3[(3u * k) & 7u]};
            vf_rcx_image(rcx, c, s_vf_carry[k & 7u]);
        } else {
            for (unsigned r = 0; r < 8u; r++) {
                rcx[r] = r < 4u ? (0x3f800000u | (1u << r)) : 0x3f800000u;
            }
        }
        if (which == 1u) {
            vf_rndf1_step(rcx, sentinel, out);
        } else {
            vf_rndf2_step(rcx, sentinel, out);
        }
        uint32_t w[17];
        for (unsigned n = 0; n < 16u; n++) {
            w[n] = out[4u + n];
        }
        w[16] = (out[1] == TN_SENTINEL && out[2] == TN_SENTINEL && out[3] == TN_SENTINEL) ? 1u : 0u;
        snprintf(name, sizeof(name), "rndf%u-p%u-%u", which, pass, k);
        hle_record(name, "PASS", out[0], w, 17);
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        vf_h2f(pass);
        vf_uc2i(pass);
        vf_socp(pass);
        vf_wbn(pass);
        vf_rndf(pass, 1u);
        vf_rndf(pass, 2u);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_FORMS */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_FUNNEL

/* #326 VFPU funnel add and average (PSP-VFPU-FUNNEL-001), one launch, register-only: the
   raw result word of vfad (sum of the source lanes) and vavg (their average) at .p, .t and
   .q, each over 26 operand sets chosen so that different summation rules (sequential,
   pairwise, exact-then-round) and different divisions (x / n against x * (1 / n)) give
   different bits, and each under no S prefix and the two TN_PFX forms. Two passes for
   repeatability. Every word is emitted as a word. The encodings are VFPU op 0x34, jump
   field 2, op field 6 (vfad) or 7 (vavg), vd S200 (0x08), vs C000, size from bit 7 (.p),
   bit 15 (.t) or both (.q); they sit one op field above the vsocp words of the vfpu-forms
   case (0xd0450008, 0xd0450088), which were checked against psp-as (pspdev binutils).
   Nothing outside the VFPU registers of this thread is touched. */

#include "hle_vfpu_cells.h"

/* Source C000 (image 0-3), destination S200 (image 32). */
TN_CELL_SET(fn_fad_p, "0xd0460088")   /* vfad.p S200, C000.p */
TN_CELL_SET(fn_fad_t, "0xd0468008")   /* vfad.t S200, C000.t */
TN_CELL_SET(fn_fad_q, "0xd0468088")   /* vfad.q S200, C000.q */
TN_CELL_SET(fn_avg_p, "0xd0470088")   /* vavg.p S200, C000.p */
TN_CELL_SET(fn_avg_t, "0xd0478008")   /* vavg.t S200, C000.t */
TN_CELL_SET(fn_avg_q, "0xd0478088")   /* vavg.q S200, C000.q */

typedef void (*fn_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Operand sets, four float32 words each (lanes 0-3 of C000); lanes beyond the size are
   ignored by the instruction but still recorded. */
static const uint32_t s_fn_ops[26][4] = {
    /*  0: 2^24, 1, 1, 1: sequential 2^24, pairwise 2^24+2, exact-then-round 2^24+4 */
    {0x4b800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u},
    /*  1: 1, 1, 1, 2^24: the same values in reverse order */
    {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x4b800000u},
    /*  2: 1, 2^-24, 2^-24, 0: sequential 1, exact 1 + 2^-23 */
    {0x3f800000u, 0x33800000u, 0x33800000u, 0x00000000u},
    /*  3: 2^-24, 2^-24, 1, 0 */
    {0x33800000u, 0x33800000u, 0x3f800000u, 0x00000000u},
    /*  4: 1e30, -1e30, 3, 0: the cancellation comes first */
    {0x7149f2cau, 0xf149f2cau, 0x40400000u, 0x00000000u},
    /*  5: 3, 1e30, -1e30, 0: the 3 is absorbed before the cancellation */
    {0x40400000u, 0x7149f2cau, 0xf149f2cau, 0x00000000u},
    /*  6: 5, 0, 0, 0 */
    {0x40a00000u, 0x00000000u, 0x00000000u, 0x00000000u},
    /*  7: 7, 0, 0, 0 */
    {0x40e00000u, 0x00000000u, 0x00000000u, 0x00000000u},
    /*  8: 1, 0, 0, 0 */
    {0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u},
    /*  9: 100, 0, 0, 0 */
    {0x42c80000u, 0x00000000u, 0x00000000u, 0x00000000u},
    /* 10: 1, 1, 1, 0: vavg.t is 3 / 3 or 3 * (1 / 3) */
    {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x00000000u},
    /* 11: 1, 2, 3, 4 */
    {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u},
    /* 12: -0 four times */
    {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u},
    /* 13: 0, -0, 0, -0 */
    {0x00000000u, 0x80000000u, 0x00000000u, 0x80000000u},
    /* 14: +inf, -inf, 1, 1 */
    {0x7f800000u, 0xff800000u, 0x3f800000u, 0x3f800000u},
    /* 15: +inf, 1, 1, 1 */
    {0x7f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u},
    /* 16: quiet NaN, 1, 1, 1 */
    {0x7fc00000u, 0x3f800000u, 0x3f800000u, 0x3f800000u},
    /* 17: 1, 1, signalling NaN, 1 */
    {0x3f800000u, 0x3f800000u, 0x7f800001u, 0x3f800000u},
    /* 18: negative NaN with a payload, 0, 0, 0 */
    {0xffc00001u, 0x00000000u, 0x00000000u, 0x00000000u},
    /* 19: the smallest denormal twice */
    {0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u},
    /* 20: a large denormal (2^-127) twice */
    {0x00400000u, 0x00400000u, 0x00000000u, 0x00000000u},
    /* 21: FLT_MAX twice (overflow) */
    {0x7f7fffffu, 0x7f7fffffu, 0x00000000u, 0x00000000u},
    /* 22: -FLT_MAX twice */
    {0xff7fffffu, 0xff7fffffu, 0x00000000u, 0x00000000u},
    /* 23: 1 + 2^-23 four times */
    {0x3f800001u, 0x3f800001u, 0x3f800001u, 0x3f800001u},
    /* 24: 1.5, 2.25, -0.125, 1e-8 */
    {0x3fc00000u, 0x40100000u, 0xbe000000u, 0x322bcc77u},
    /* 25: 1e30, 3, -1e30, 0: the 3 sits between the cancelling pair */
    {0x7149f2cau, 0x40400000u, 0xf149f2cau, 0x00000000u},
};

static const char *const s_fn_forms[2] = {"fad", "avg"};
static const char *const s_fn_sizes[3] = {"p", "t", "q"};
static const fn_cell_fn s_fn_cells[2][3][3] = {
    {{fn_fad_p_none, fn_fad_p_p1, fn_fad_p_p2}, {fn_fad_t_none, fn_fad_t_p1, fn_fad_t_p2},
     {fn_fad_q_none, fn_fad_q_p1, fn_fad_q_p2}},
    {{fn_avg_p_none, fn_avg_p_p1, fn_avg_p_p2}, {fn_avg_t_none, fn_avg_t_p1, fn_avg_t_p2},
     {fn_avg_q_none, fn_avg_q_p1, fn_avg_q_p2}},
};
static const uint32_t s_fn_words[2][3] = {
    {0xd0460088u, 0xd0468008u, 0xd0468088u},
    {0xd0470088u, 0xd0478008u, 0xd0478088u},
};
static const char *const s_fn_pfx_names[3] = {"none", "p1", "p2"};
static const uint32_t s_fn_pfx_words[3] = {TN_PFXW_NONE, TN_PFXW_P1, TN_PFXW_P2};

static uint32_t s_fn_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_fn_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One cell: background image word i = 0x3f800000 | i << 16, source lanes at image 0-3, then
   the word. Row <name>: result = the instruction word; out0-3 = image 32-35 after (S200 and
   the three words after it); out4/out5 = VFPU_CC after the preset / after the word;
   out6-9 = image 0-3 before; out10 = number of image words outside 32-35 that changed;
   out11 = the first such index (0xffffffff when none); out12 = the vpfxs word issued
   before it (0 = none). */
static void fn_cell(const char *name, fn_cell_fn fn, uint32_t word, uint32_t pfx,
                    const uint32_t *lanes)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_fn_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 4u; i++) {
        s_fn_in[i] = lanes[i];
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_fn_in, s_fn_out, cc);
    uint32_t changed = 0u, first = 0xffffffffu;
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        if ((i < 32u || i > 35u) && s_fn_out[i] != s_fn_in[i]) {
            if (changed++ == 0u) {
                first = i;
            }
        }
    }
    const uint32_t w[13] = {
        s_fn_out[32], s_fn_out[33], s_fn_out[34], s_fn_out[35], cc[0], cc[1], s_fn_in[0],
        s_fn_in[1], s_fn_in[2], s_fn_in[3], changed, first, pfx,
    };
    hle_record(name, "PASS", word, w, 13);
}

/* Rows fad-p<pass>-<size>-<i>-<pfx> and avg-p<pass>-<size>-<i>-<pfx>: the form at the size
   over operand set i, one step per (form, pass, size) before its cells. */
static void fn_group(unsigned pass, unsigned form, unsigned size)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u-%s", s_fn_forms[form], pass, s_fn_sizes[size]);
    hle_step(name);
    for (unsigned i = 0; i < 26u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "%s-p%u-%s-%u-%s", s_fn_forms[form], pass,
                     s_fn_sizes[size], i, s_fn_pfx_names[f]);
            fn_cell(name, s_fn_cells[form][size][f], s_fn_words[form][size],
                    s_fn_pfx_words[f], s_fn_ops[i]);
        }
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < 2u; form++) {
            for (unsigned size = 0; size < 3u; size++) {
                fn_group(pass, form, size);
            }
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_FUNNEL */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_SORT

/* #326 VFPU sort steps (PSP-VFPU-SORT-001), one launch, register-only: the raw result words
   of vsrt1.q, vsrt2.q, vsrt3.q and vsrt4.q (the min/max compare-exchange of the pairs
   (x, y) and (z, w), (x, w) and (y, z), and so on) over 24 operand sets that put the
   interesting pair in several lane positions, each under no S prefix and the two TN_PFX
   forms. Two passes for repeatability. Only the .q form exists. Every word is emitted as a
   word. The encodings are VFPU op 0x34, jump field 2, op field 0, 1, 8 and 9 (vsrt1, vsrt2,
   vsrt3, vsrt4), vd C200 (0x08), vs C000, size .q (bits 7 and 15). Nothing outside the VFPU
   registers of this thread is touched. */

#include "hle_vfpu_cells.h"

/* Source C000 (image 0-3), destination C200 (image 32-35, all four lanes are results). */
TN_CELL_SET(st_srt1, "0xd0408088")   /* vsrt1.q C200, C000 */
TN_CELL_SET(st_srt2, "0xd0418088")   /* vsrt2.q C200, C000 */
TN_CELL_SET(st_srt3, "0xd0488088")   /* vsrt3.q C200, C000 */
TN_CELL_SET(st_srt4, "0xd0498088")   /* vsrt4.q C200, C000 */

typedef void (*st_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Operand sets, four float32 words each (lanes 0-3 of C000). */
static const uint32_t s_st_ops[24][4] = {
    /*  0: 1, 2, 3, 4 */
    {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u},
    /*  1: 4, 3, 2, 1 */
    {0x40800000u, 0x40400000u, 0x40000000u, 0x3f800000u},
    /*  2: 2, 1, 4, 3 */
    {0x40000000u, 0x3f800000u, 0x40800000u, 0x40400000u},
    /*  3: -1, 1, -2, 2 */
    {0xbf800000u, 0x3f800000u, 0xc0000000u, 0x40000000u},
    /*  4: 5 four times */
    {0x40a00000u, 0x40a00000u, 0x40a00000u, 0x40a00000u},
    /*  5: +0, -0, -0, +0 */
    {0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u},
    /*  6: -0, +0, +0, -0 */
    {0x80000000u, 0x00000000u, 0x00000000u, 0x80000000u},
    /*  7: qNaN, 1, 1, qNaN */
    {0x7fc00000u, 0x3f800000u, 0x3f800000u, 0x7fc00000u},
    /*  8: 1, qNaN, qNaN, 1 */
    {0x3f800000u, 0x7fc00000u, 0x7fc00000u, 0x3f800000u},
    /*  9: -qNaN, 1, 1, -qNaN */
    {0xffc00000u, 0x3f800000u, 0x3f800000u, 0xffc00000u},
    /* 10: signalling NaN, -1, -1, signalling NaN */
    {0x7f800001u, 0xbf800000u, 0xbf800000u, 0x7f800001u},
    /* 11: qNaN, -qNaN, -qNaN, qNaN */
    {0x7fc00000u, 0xffc00000u, 0xffc00000u, 0x7fc00000u},
    /* 12: NaN payloads 0x7fc12345, 0x7fc00001, 0x7fc00001, 0x7fc12345 */
    {0x7fc12345u, 0x7fc00001u, 0x7fc00001u, 0x7fc12345u},
    /* 13: +inf, -inf, -inf, +inf */
    {0x7f800000u, 0xff800000u, 0xff800000u, 0x7f800000u},
    /* 14: +inf, qNaN, qNaN, +inf */
    {0x7f800000u, 0x7fc00000u, 0x7fc00000u, 0x7f800000u},
    /* 15: -inf, 1, 1, -inf */
    {0xff800000u, 0x3f800000u, 0x3f800000u, 0xff800000u},
    /* 16: smallest denormal, 0, 0, smallest denormal */
    {0x00000001u, 0x00000000u, 0x00000000u, 0x00000001u},
    /* 17: -smallest denormal, 0, 0, -smallest denormal */
    {0x80000001u, 0x00000000u, 0x00000000u, 0x80000001u},
    /* 18: +denormal, -denormal, -denormal, +denormal */
    {0x00000001u, 0x80000001u, 0x80000001u, 0x00000001u},
    /* 19: denormal 1, denormal 2, denormal 2, denormal 1 */
    {0x00000001u, 0x00000002u, 0x00000002u, 0x00000001u},
    /* 20: 1, 1 + 2^-23, 1 + 2^-23, 1 */
    {0x3f800000u, 0x3f800001u, 0x3f800001u, 0x3f800000u},
    /* 21: FLT_MAX, +inf, +inf, FLT_MAX */
    {0x7f7fffffu, 0x7f800000u, 0x7f800000u, 0x7f7fffffu},
    /* 22: 1, 1, 0, 0 (ties across the pairs) */
    {0x3f800000u, 0x3f800000u, 0x00000000u, 0x00000000u},
    /* 23: -2, -1, 1, 2 (ascending across zero) */
    {0xc0000000u, 0xbf800000u, 0x3f800000u, 0x40000000u},
};

static const char *const s_st_forms[4] = {"srt1", "srt2", "srt3", "srt4"};
static const st_cell_fn s_st_cells[4][3] = {
    {st_srt1_none, st_srt1_p1, st_srt1_p2}, {st_srt2_none, st_srt2_p1, st_srt2_p2},
    {st_srt3_none, st_srt3_p1, st_srt3_p2}, {st_srt4_none, st_srt4_p1, st_srt4_p2},
};
static const uint32_t s_st_words[4] = {0xd0408088u, 0xd0418088u, 0xd0488088u, 0xd0498088u};
static const char *const s_st_pfx_names[3] = {"none", "p1", "p2"};
static const uint32_t s_st_pfx_words[3] = {TN_PFXW_NONE, TN_PFXW_P1, TN_PFXW_P2};

static uint32_t s_st_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_st_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One cell: background image word i = 0x3f800000 | i << 16, source lanes at image 0-3, then
   the word. Row <name>: result = the instruction word; out0-3 = image 32-35 after (C200);
   out4/out5 = VFPU_CC after the preset / after the word; out6-9 = image 0-3 before;
   out10 = number of image words outside 32-35 that changed; out11 = the first such index
   (0xffffffff when none); out12 = the vpfxs word issued before it (0 = none). */
static void st_cell(const char *name, st_cell_fn fn, uint32_t word, uint32_t pfx,
                    const uint32_t *lanes)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_st_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 4u; i++) {
        s_st_in[i] = lanes[i];
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_st_in, s_st_out, cc);
    uint32_t changed = 0u, first = 0xffffffffu;
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        if ((i < 32u || i > 35u) && s_st_out[i] != s_st_in[i]) {
            if (changed++ == 0u) {
                first = i;
            }
        }
    }
    const uint32_t w[13] = {
        s_st_out[32], s_st_out[33], s_st_out[34], s_st_out[35], cc[0], cc[1], s_st_in[0],
        s_st_in[1], s_st_in[2], s_st_in[3], changed, first, pfx,
    };
    hle_record(name, "PASS", word, w, 13);
}

/* Rows srt<n>-p<pass>-<i>-<pfx>: the form over operand set i, one step per (form, pass)
   before its cells. */
static void st_group(unsigned pass, unsigned form)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", s_st_forms[form], pass);
    hle_step(name);
    for (unsigned i = 0; i < 24u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "%s-p%u-%u-%s", s_st_forms[form], pass, i,
                     s_st_pfx_names[f]);
            st_cell(name, s_st_cells[form][f], s_st_words[form], s_st_pfx_words[f],
                    s_st_ops[i]);
        }
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < 4u; form++) {
            st_group(pass, form);
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_SORT */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_FORMS2

/* #326 VFPU forms, second batch (PSP-VFPU-FORMS2-001), one launch, register-only: the raw
   result words of the forms the runtime does not model yet. vrnds.s (the random seed, each of
   24 seed words from two written RCX states, followed by one vrndi.s draw), vrndi, vrndf1 and
   vrndf2 at .p, .t and .q from eight written RCX states, vf2h.p and vf2h.q over 28 operand
   quads, and vc2i.s over the byte patterns of the vfpu-forms case. vf2h and vc2i run under no
   S prefix and the two TN_PFX forms. Two passes for repeatability. Every word is emitted as a
   word; the encodings were checked against psp-as (pspdev binutils): VFPU op 0x34, jump field
   1, op field 0 (vrnds), 1 (vrndi), 2 (vrndf1), 3 (vrndf2), 18 (vf2h) and 25 (vc2i), size
   bits 7 (.p) and 15 (.t), both for .q. Nothing outside the VFPU registers of this thread is
   touched. */

#include "hle_vfpu_cells.h"

/* Source C000 (image 0-3, S000 for a single), destination C200 (image 32-35, S200). */
TN_CELL_SET(f2_f2h_p, "0xd0320088")   /* vf2h.p S200, C000.p */
TN_CELL_SET(f2_f2h_q, "0xd0328088")   /* vf2h.q C200.p, C000.q */
TN_CELL_SET(f2_c2i, "0xd0390008")     /* vc2i.s C200.q, S000 */

typedef void (*f2_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Operand quads, four float32 words each (lanes 0-3 of C000; the .p form reads lanes 0-1). */
static const uint32_t s_f2_quads[28][4] = {
    /*  0: +0, -0, smallest denormal, -smallest denormal */
    {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u},
    /*  1: largest float denormal, 2^-14, its float neighbour below and above */
    {0x007fffffu, 0x38800000u, 0x387fffffu, 0x38800001u},
    /*  2: -2^-14, 2^-24, 2^-15, 1.5 * 2^-24 */
    {0xb8800000u, 0x33800000u, 0x38000000u, 0x33c00000u},
    /*  3: 0.5 * 2^-24 (tie) and its neighbours, 1.5 * 2^-24 + 1ulp */
    {0x33000000u, 0x33000001u, 0x32ffffffu, 0x33c00001u},
    /*  4: 1.5 * 2^-24 - 1ulp, largest half denormal, -2^-24, -1.5 * 2^-24 */
    {0x33bfffffu, 0x387fc000u, 0xb3800000u, 0xb3c00000u},
    /*  5: 1 + 2^-11 (tie), its neighbours, 1 + 3 * 2^-11 (tie) */
    {0x3f801000u, 0x3f800fffu, 0x3f801001u, 0x3f803000u},
    /*  6: 1 + 3 * 2^-11 neighbours, -(1 + 2^-11), -(1 + 3 * 2^-11) */
    {0x3f802fffu, 0x3f803001u, 0xbf801000u, 0xbf803000u},
    /*  7: 65504, 65519, 65520, 65535 */
    {0x477fe000u, 0x477fef00u, 0x477ff000u, 0x477fff00u},
    /*  8: 65536, 1e6, -65504, -65520 */
    {0x47800000u, 0x49742400u, 0xc77fe000u, 0xc77ff000u},
    /*  9: +inf, -inf, quiet NaN, negative quiet NaN */
    {0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u},
    /* 10: signalling NaN, negative signalling NaN, quiet NaN payload, negative quiet NaN payload */
    {0x7f800001u, 0xff800001u, 0x7fc12345u, 0xffd54321u},
    /* 11: signalling NaN payload, negative one, all-ones payload NaNs */
    {0x7fa5a5a5u, 0xffbfffffu, 0x7fffffffu, 0xffffffffu},
    /* 12: 1, -2, 0.333, 3.14159 */
    {0x3f800000u, 0xc0000000u, 0x3eaa7efau, 0x40490fd0u},
    /* 13: quiet NaN against finite values and -inf in lane 3 */
    {0x7fc00000u, 0x3f800000u, 0x477ff000u, 0xff800000u},
    /* 14: finite lane 0 against a signalling NaN in lane 1 */
    {0x3f800000u, 0x7f800001u, 0x33800000u, 0x477fe000u},
    /* 15: +inf with +0, then 65519 and a tie neighbour */
    {0x7f800000u, 0x00000000u, 0x477fef00u, 0x33000001u},
    /* 16: -inf with -0, -denormal and 1 + 2^-11 */
    {0xff800000u, 0x80000000u, 0x80000001u, 0x3f801000u},
    /* 17: 65519, 65520 (tie), 65504, 65535 */
    {0x477fef00u, 0x477ff000u, 0x477fe000u, 0x477fff00u},
    /* 18: FLT_MAX, -FLT_MAX, FLT_MIN, -FLT_MIN */
    {0x7f7fffffu, 0xff7fffffu, 0x00800000u, 0x80800000u},
    /* 19: 2^-15 neighbours, 2^-24 neighbours */
    {0x38000001u, 0x37ffffffu, 0x33800001u, 0x337fffffu},
    /* 20: 0.75, 2.5, 3 and 3.5 times 2^-24 */
    {0x33400000u, 0x34200000u, 0x34400000u, 0x34600000u},
    /* 21: 1/3, -1/3, 0.1, 100 */
    {0x3eaaaaabu, 0xbeaaaaabu, 0x3dcccccdu, 0x42c80000u},
    /* 22: 2 + 2^-10 (tie) at exponent 1, neighbours, 2 + 3 * 2^-10 */
    {0x40001000u, 0x40000fffu, 0x40001001u, 0x40003000u},
    /* 23: quiet and signalling NaN payload 1 of each sign */
    {0x7fc00001u, 0xffc00001u, 0x7f800002u, 0xff800002u},
    /* 24: -0, +0, quiet NaN, 1 */
    {0x80000000u, 0x00000000u, 0x7fc00000u, 0x3f800000u},
    /* 25: 65536 neighbours below and above, 65520 neighbours */
    {0x477fffffu, 0x47800001u, 0x477ff001u, 0x477fefffu},
    /* 26: arbitrary words */
    {0xc0600000u, 0x12345678u, 0x8abcdef0u, 0x3727c5acu},
    /* 27: denormal, 2^-14 and a normal, -inf */
    {0x00000002u, 0xb8800000u, 0x3f000000u, 0xff800000u},
};

/* vc2i.s operands: the byte patterns of the vfpu-forms case. */
static const uint32_t s_f2_bytes[10] = {
    0x00000000u, 0xffffffffu, 0x7f80ff01u, 0x80402010u, 0x01020304u,
    0xfedcba98u, 0x3f800000u, 0x7fc00000u, 0x00000001u, 0x00ff00ffu,
};

/* vrnds.s seed words. */
static const uint32_t s_f2_seeds[24] = {
    0x00000000u, /* 0 */
    0x00000001u, /* 1 */
    0x80000000u, /* sign bit */
    0xffffffffu, /* all ones */
    0x3f800000u, /* 1.0f */
    0x7fc00000u, /* quiet NaN */
    0x12345678u, /* arbitrary */
    0xdeadbeefu, /* arbitrary */
    0x00000002u, /* walking bit 1 */
    0x00000100u, /* walking bit 8 */
    0x00010000u, /* walking bit 16 */
    0x01000000u, /* walking bit 24 */
    0x40000000u, /* walking bit 30 */
    0x7fffffffu, /* all but the sign */
    0x55555555u, /* alternating */
    0xaaaaaaaau, /* alternating */
    0x0000ffffu, /* low half */
    0xffff0000u, /* high half */
    0x2468ace0u, /* RCX image word */
    0x13579bdfu, /* RCX image word */
    0x907f6e50u, /* RCX image word */
    0xc4d5e6d8u, /* RCX image word */
    0x3f800001u, /* 1.0f + 1ulp */
    0x00000fffu, /* twelve ones */
};

static const char *const s_f2_pfx_names[3] = {"none", "p1", "p2"};
static const uint32_t s_f2_pfx_words[3] = {TN_PFXW_NONE, TN_PFXW_P1, TN_PFXW_P2};

static uint32_t s_f2_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_f2_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One cell: background image word i = 0x3f800000 | i << 16, source lanes at image 0-3, then
   the word. Row <name>: result = the instruction word; out0-3 = image 32-35 after (C200);
   out4/out5 = VFPU_CC after the preset / after the word; out6-9 = image 0-3 before;
   out10 = number of image words outside 32-35 that changed; out11 = the first such index
   (0xffffffff when none); out12 = the vpfxs word issued before it (0 = none). */
static void f2_cell(const char *name, f2_cell_fn fn, uint32_t word, uint32_t pfx,
                    const uint32_t *lanes)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_f2_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 4u; i++) {
        s_f2_in[i] = lanes[i];
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_f2_in, s_f2_out, cc);
    uint32_t changed = 0u, first = 0xffffffffu;
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        if ((i < 32u || i > 35u) && s_f2_out[i] != s_f2_in[i]) {
            if (changed++ == 0u) {
                first = i;
            }
        }
    }
    const uint32_t w[13] = {
        s_f2_out[32], s_f2_out[33], s_f2_out[34], s_f2_out[35], cc[0], cc[1], s_f2_in[0],
        s_f2_in[1], s_f2_in[2], s_f2_in[3], changed, first, pfx,
    };
    hle_record(name, "PASS", word, w, 13);
}

/* Rows f2h-p<pass>-<p|q>-<i>-<pfx>: vf2h.p (lanes 0-1) or vf2h.q (lanes 0-3) of quad i. */
static void f2_f2h(unsigned pass, unsigned wide)
{
    static const f2_cell_fn cells[2][3] = {
        {f2_f2h_p_none, f2_f2h_p_p1, f2_f2h_p_p2}, {f2_f2h_q_none, f2_f2h_q_p1, f2_f2h_q_p2}};
    static const uint32_t words[2] = {0xd0320088u, 0xd0328088u};
    char name[64];
    snprintf(name, sizeof(name), "f2h-p%u-%c", pass, wide ? 'q' : 'p');
    hle_step(name);
    for (unsigned i = 0; i < 28u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "f2h-p%u-%c-%u-%s", pass, wide ? 'q' : 'p', i,
                     s_f2_pfx_names[f]);
            f2_cell(name, cells[wide][f], words[wide], s_f2_pfx_words[f], s_f2_quads[i]);
        }
    }
}

/* Rows c2i-p<pass>-<i>-<pfx>: vc2i.s of s_f2_bytes[i] (lane 0; lane 1 carries the vfpu-forms
   sentinel). */
static void f2_c2i(unsigned pass)
{
    static const f2_cell_fn fns[3] = {f2_c2i_none, f2_c2i_p1, f2_c2i_p2};
    char name[64];
    snprintf(name, sizeof(name), "c2i-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 10u; i++) {
        const uint32_t lanes[4] = {s_f2_bytes[i], TN_SENTINEL, 0x3f820000u, 0x3f830000u};
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "c2i-p%u-%u-%s", pass, i, s_f2_pfx_names[f]);
            f2_cell(name, fns[f], 0xd0390008u, s_f2_pfx_words[f], lanes);
        }
    }
}

/* ---- random-unit cells -------------------------------------------------------- */

/* RCX image of four 32-bit components and a 32-bit carry, as vf_rcx_image in the
   vfpu-forms section: Ck = (RCX(k+4) & 0xffff) << 16 | (RCXk & 0xffff), carry nibble k in
   bits 16-19 of RCXk. */
static void f2_rcx_image(uint32_t *rcx, const uint32_t c[4], uint32_t carry)
{
    for (unsigned k = 0; k < 8u; k++) {
        const uint32_t half = (c[k & 3u] >> (k < 4u ? 0u : 16u)) & 0xffffu;
        rcx[k] = 0x3f800000u | (((carry >> (4u * k)) & 0xfu) << 16) | half;
    }
}

/* The thread-entry RCX image vf_rndf uses for its k = 16 row. */
static void f2_rcx_entry(uint32_t *rcx)
{
    for (unsigned r = 0; r < 8u; r++) {
        rcx[r] = r < 4u ? (0x3f800000u | (1u << r)) : 0x3f800000u;
    }
}

/* One seed cell. out: RCX0-7 read back after the write (0-7), RCX0-7 after vrnds.s (8-15),
   the vrndi.s draw (16), RCX0-7 after the draw (17-24). */
static void f2_rnds_step(const uint32_t *rcx_in, const uint32_t *seed, uint32_t *out)
{
    __asm__ volatile(
        "lv.s S000, 0(%[seed])\n\t"
        TN_RCX_WRITE("%[st]")
        "move $t0, %[out]\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0200000\n\t" /* vrnds.s S000 */
        "addiu $t0, %[out], 32\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0210000\n\t" /* vrndi.s S000 */
        "sv.s S000, 64(%[out])\n\t"
        "addiu $t0, %[out], 68\n\t"
        TN_RCX_READ("$t0")
        :
        : [st] "r"(rcx_in), [seed] "r"(seed), [out] "r"(out)
        : "t0", "t1", "memory");
}

/* One wide draw cell per instruction word. out: C000 lanes 0-3 after the draw (0-3), RCX0-7
   after it (4-11), RCX0-7 after the write (12-19). C000 holds the sentinel before. */
#define F2_DRAW_FN(fname, word)                                                      \
    static void fname(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out) \
    {                                                                                \
        __asm__ volatile(                                                            \
            "lv.q C000, 0(%[sen])\n\t"                                               \
            TN_RCX_WRITE("%[st]")                                                    \
            "addiu $t0, %[out], 48\n\t"                                              \
            TN_RCX_READ("$t0")                                                       \
            ".word " word "\n\t"                                                     \
            "sv.q C000, 0(%[out])\n\t"                                               \
            "addiu $t0, %[out], 16\n\t"                                              \
            TN_RCX_READ("$t0")                                                       \
            :                                                                        \
            : [st] "r"(rcx_in), [sen] "r"(sentinel), [out] "r"(out)                  \
            : "t0", "t1", "memory");                                                 \
    }

F2_DRAW_FN(f2_rndi_p, "0xd0210080")   /* vrndi.p C000.p */
F2_DRAW_FN(f2_rndi_t, "0xd0218000")   /* vrndi.t C000.t */
F2_DRAW_FN(f2_rndi_q, "0xd0218080")   /* vrndi.q C000.q */
F2_DRAW_FN(f2_rndf1_p, "0xd0220080")  /* vrndf1.p C000.p */
F2_DRAW_FN(f2_rndf1_t, "0xd0228000")  /* vrndf1.t C000.t */
F2_DRAW_FN(f2_rndf1_q, "0xd0228080")  /* vrndf1.q C000.q */
F2_DRAW_FN(f2_rndf2_p, "0xd0230080")  /* vrndf2.p C000.p */
F2_DRAW_FN(f2_rndf2_t, "0xd0238000")  /* vrndf2.t C000.t */
F2_DRAW_FN(f2_rndf2_q, "0xd0238080")  /* vrndf2.q C000.q */

typedef void (*f2_draw_fn)(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out);

static const char *const s_f2_draw_stems[3] = {"wrndi", "wrndf1", "wrndf2"};
static const char s_f2_draw_sizes[3] = {'p', 't', 'q'};
static const f2_draw_fn s_f2_draw_fns[3][3] = {
    {f2_rndi_p, f2_rndi_t, f2_rndi_q}, {f2_rndf1_p, f2_rndf1_t, f2_rndf1_q},
    {f2_rndf2_p, f2_rndf2_t, f2_rndf2_q}};
static const uint32_t s_f2_draw_words[3][3] = {
    {0xd0210080u, 0xd0218000u, 0xd0218080u}, {0xd0220080u, 0xd0228000u, 0xd0228080u},
    {0xd0230080u, 0xd0238000u, 0xd0238080u}};

static const uint32_t s_f2_c2[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xfffffffeu, 0xffffffffu, 0x907f6e50u, 0x5a5a5a5au};
static const uint32_t s_f2_c3[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xc0000000u, 0xffffffffu, 0xc4d5e6d8u, 0x89abcdefu};
static const uint32_t s_f2_carry[8] = {0x0u, 0x1u, 0x2u, 0x3u, 0xfu, 0x10u, 0xffffu,
                                       0x12345678u};

/* The RCX state k of the vfpu-forms draws (k < 8): C0 = 0x2468ace0 + k * 0x11111111,
   C1 = 0x13579bdf ^ (k << 20), C2 = s_f2_c2[k], C3 = s_f2_c3[(3k) & 7], carry = s_f2_carry[k]. */
static void f2_rcx_state(unsigned k, uint32_t *rcx)
{
    const uint32_t c[4] = {0x2468ace0u + k * 0x11111111u, 0x13579bdfu ^ (k << 20),
                           s_f2_c2[k & 7u], s_f2_c3[(3u * k) & 7u]};
    f2_rcx_image(rcx, c, s_f2_carry[k & 7u]);
}

/* Rows rnds-p<pass>-<i>-<a|b>: seed i from the entry RCX (a) or from the carry state
   (b: C0 = 0x2468ace0, C1 = 0x13579bdf, C2 = 0x907f6e50, C3 = 0xc4d5e6d8, carry
   0x12345678). result = the seed; out0-7 = RCX0-7 after the write, out8-15 = after vrnds.s,
   out16 = the vrndi.s draw, out17-24 = RCX0-7 after the draw. */
static void f2_rnds(unsigned pass)
{
    static const uint32_t c_b[4] = {0x2468ace0u, 0x13579bdfu, 0x907f6e50u, 0xc4d5e6d8u};
    uint32_t rcx[8] __attribute__((aligned(16)));
    uint32_t seedq[4] __attribute__((aligned(16))) = {0u, 0u, 0u, 0u};
    uint32_t out[25] __attribute__((aligned(16)));
    char name[48];
    snprintf(name, sizeof(name), "rnds-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 24u; i++) {
        for (unsigned st = 0; st < 2u; st++) {
            if (st == 0u) {
                f2_rcx_entry(rcx);
            } else {
                f2_rcx_image(rcx, c_b, 0x12345678u);
            }
            seedq[0] = s_f2_seeds[i];
            f2_rnds_step(rcx, seedq, out);
            snprintf(name, sizeof(name), "rnds-p%u-%u-%c", pass, i, st == 0u ? 'a' : 'b');
            hle_record(name, "PASS", s_f2_seeds[i], out, 25);
        }
    }
}

/* Rows <wrndi|wrndf1|wrndf2>-p<pass>-<p|t|q>-<k>: the draw from RCX state k (0-7).
   result = the instruction word; out0-3 = C000 lanes 0-3 after, out4-11 = RCX0-7 after,
   out12-19 = RCX0-7 after the write. */
static void f2_wide(unsigned pass, unsigned op, unsigned size)
{
    static const uint32_t sentinel[4] __attribute__((aligned(16))) = {
        TN_SENTINEL, TN_SENTINEL, TN_SENTINEL, TN_SENTINEL};
    uint32_t rcx[8] __attribute__((aligned(16)));
    uint32_t out[20] __attribute__((aligned(16)));
    char name[48];
    snprintf(name, sizeof(name), "%s-p%u-%c", s_f2_draw_stems[op], pass, s_f2_draw_sizes[size]);
    hle_step(name);
    for (unsigned k = 0; k < 8u; k++) {
        f2_rcx_state(k, rcx);
        s_f2_draw_fns[op][size](rcx, sentinel, out);
        snprintf(name, sizeof(name), "%s-p%u-%c-%u", s_f2_draw_stems[op], pass,
                 s_f2_draw_sizes[size], k);
        hle_record(name, "PASS", s_f2_draw_words[op][size], out, 20);
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        f2_rnds(pass);
        for (unsigned op = 0; op < 3u; op++) {
            for (unsigned size = 0; size < 3u; size++) {
                f2_wide(pass, op, size);
            }
        }
        f2_f2h(pass, 0u);
        f2_f2h(pass, 1u);
        f2_c2i(pass);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_FORMS2 */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_INTR_CELLS

/* #325 interrupt and timer delivery cells (PSP-HLE-INTR-001), one launch, user mode:
   (a) a VBLANK sub-interrupt left pending while interrupts are suspended
       (sceKernelCpuSuspendIntr) for 20 ms and 50 ms: how many handler entries follow the
       resume, and when;
   (b) re-pending while the handler runs: one entry holds for 25 ms (longer than a frame);
       when do the next entries come;
   (c) VTimer and alarm handlers that return a reschedule interval, then 0: the scheduled,
       actual and system times of every call.
   Every row is sequence numbers and system-time low words relative to a reference time
   recorded in the same row. Waits are bounded; the handler is released and the timers
   deleted before the end. Interrupts are never suspended for longer than 50 ms, and the
   one long handler entry is 25 ms. */

#include <pspintrman.h>

#define IC_VBL_SLOTS 64u
static volatile uint32_t s_ic_vbl_count;
static volatile uint32_t s_ic_vbl_in[IC_VBL_SLOTS];
static volatile uint32_t s_ic_vbl_out[IC_VBL_SLOTS];
static volatile uint32_t s_ic_vbl_hold_us;

static int ic_vbl_handler(int sub, void *arg)
{
    (void)sub;
    (void)arg;
    const uint32_t n = s_ic_vbl_count;
    const uint32_t t = sceKernelGetSystemTimeLow();
    if (n < IC_VBL_SLOTS) {
        s_ic_vbl_in[n] = t;
    }
    const uint32_t hold = s_ic_vbl_hold_us;
    if (hold != 0u) {
        s_ic_vbl_hold_us = 0u;
        while ((uint32_t)(sceKernelGetSystemTimeLow() - t) < hold) {
        }
    }
    if (n < IC_VBL_SLOTS) {
        s_ic_vbl_out[n] = sceKernelGetSystemTimeLow();
    }
    s_ic_vbl_count = n + 1u;
    return -1;
}

static void ic_vbl_reset(void)
{
    s_ic_vbl_count = 0u;
    for (unsigned i = 0; i < IC_VBL_SLOTS; i++) {
        s_ic_vbl_in[i] = 0u;
        s_ic_vbl_out[i] = 0u;
    }
}

static void ic_spin_us(uint32_t us)
{
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    while ((uint32_t)(sceKernelGetSystemTimeLow() - t0) < us) {
    }
}

/* Waits (bounded, 100 ms) for the next handler entry, so a cell starts just after a frame. */
static void ic_vbl_align(void)
{
    const uint32_t c = s_ic_vbl_count;
    for (unsigned i = 0; i < 100u && s_ic_vbl_count == c; i++) {
        sceKernelDelayThread(1000);
    }
}

/* Row <name>: result = number of entries; out0..: entry times (and exit times when
   with_exit) minus ref, entries first..first+n. */
static void ic_vbl_times(const char *name, uint32_t ref, uint32_t first, uint32_t n, int with_exit)
{
    uint32_t w[16];
    uint32_t k = 0;
    for (uint32_t i = first; i < first + n && k < 16u; i++) {
        w[k++] = i < s_ic_vbl_count && i < IC_VBL_SLOTS ? s_ic_vbl_in[i] - ref : 0xffffffffu;
        if (with_exit && k < 16u) {
            w[k++] = i < s_ic_vbl_count && i < IC_VBL_SLOTS ? s_ic_vbl_out[i] - ref : 0xffffffffu;
        }
    }
    hle_record(name, "PASS", s_ic_vbl_count, w, k);
}

/* (a) Row vbl-masked-<ms>: result = entries during the suspension (should be 0); out0 =
   entries before; out1 = entries during; out2 = entries 200 us after the resume; out3 =
   entries 30 ms after the resume; out4 = suspension length (system time); out5 = the
   suspend return word; out6.. = entry times after the resume minus the resume time. */
static void ic_vbl_masked(const char *name, uint32_t us)
{
    hle_step(name);
    ic_vbl_align();
    const uint32_t before = s_ic_vbl_count;
    const unsigned int flags = sceKernelCpuSuspendIntr();
    const uint32_t ts = sceKernelGetSystemTimeLow();
    ic_spin_us(us);
    const uint32_t during = s_ic_vbl_count;
    const uint32_t tr = sceKernelGetSystemTimeLow();
    sceKernelCpuResumeIntr(flags);
    ic_spin_us(200u);
    const uint32_t after200 = s_ic_vbl_count;
    sceKernelDelayThread(30000);
    const uint32_t after30 = s_ic_vbl_count;
    uint32_t w[14] = {before, during, after200, after30, tr - ts, (uint32_t)flags};
    for (unsigned i = 0; i < 8u; i++) {
        const uint32_t e = during + i;
        w[6u + i] = e < after30 && e < IC_VBL_SLOTS ? s_ic_vbl_in[e] - tr : 0xffffffffu;
    }
    hle_record(name, "PASS", during - before, w, 14);
}

/* (c) VTimer and alarm handlers. Each call stores its sequence number, the scheduled and
   actual VTimer times (low words; alarms: 0) and the system time; it returns
   s_ic_ret[n] (0 ends the series). */
#define IC_TCALLS 8u
struct ic_tcall {
    volatile uint32_t sched;
    volatile uint32_t actual;
    volatile uint32_t sys;
};
static struct ic_tcall s_ic_tcall[IC_TCALLS];
static volatile uint32_t s_ic_tcount;
static uint32_t s_ic_ret[IC_TCALLS];

static SceUInt ic_vtimer_handler(SceUID uid, SceKernelSysClock *sched, SceKernelSysClock *actual, void *common)
{
    (void)uid;
    (void)common;
    const uint32_t n = s_ic_tcount;
    if (n < IC_TCALLS) {
        s_ic_tcall[n].sched = sched ? sched->low : 0xffffffffu;
        s_ic_tcall[n].actual = actual ? actual->low : 0xffffffffu;
        s_ic_tcall[n].sys = sceKernelGetSystemTimeLow();
    }
    s_ic_tcount = n + 1u;
    return n < IC_TCALLS ? s_ic_ret[n] : 0u;
}

static SceUInt ic_alarm_handler(void *common)
{
    (void)common;
    const uint32_t n = s_ic_tcount;
    if (n < IC_TCALLS) {
        s_ic_tcall[n].sched = 0u;
        s_ic_tcall[n].actual = 0u;
        s_ic_tcall[n].sys = sceKernelGetSystemTimeLow();
    }
    s_ic_tcount = n + 1u;
    return n < IC_TCALLS ? s_ic_ret[n] : 0u;
}

static void ic_tcalls_reset(const uint32_t *rets)
{
    s_ic_tcount = 0u;
    for (unsigned i = 0; i < IC_TCALLS; i++) {
        s_ic_tcall[i].sched = 0u;
        s_ic_tcall[i].actual = 0u;
        s_ic_tcall[i].sys = 0u;
        s_ic_ret[i] = rets[i];
    }
}

/* Rows <name>-call<i>: result = the call's sequence number; out0 = scheduled VTimer time
   low; out1 = actual VTimer time low; out2 = system time minus ref; out3 = the value that
   call returned. Row <name>: result = calls seen at the end; out0 = calls 150 ms after the
   series ended (a call after a return of 0 shows as a higher count); out1 = the set
   call's return; out2 = ref. */
static void ic_tcalls_rows(const char *name, uint32_t ref, int set_rc, uint32_t calls_at_end)
{
    char row[64];
    const uint32_t n = s_ic_tcount < IC_TCALLS ? s_ic_tcount : IC_TCALLS;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t w[4] = {s_ic_tcall[i].sched, s_ic_tcall[i].actual, s_ic_tcall[i].sys - ref, s_ic_ret[i]};
        snprintf(row, sizeof(row), "%s-call%u", name, (unsigned int)i);
        hle_record(row, "PASS", i + 1u, w, 4);
    }
    const uint32_t w[3] = {s_ic_tcount, (uint32_t)set_rc, ref};
    hle_record(name, "PASS", calls_at_end, w, 3);
}

static void ic_vtimer_series(const char *name, const uint32_t *rets, uint32_t first_us)
{
    hle_step(name);
    ic_tcalls_reset(rets);
    const SceUID vt = sceKernelCreateVTimer("ic_vt", NULL);
    if (vt < 0) {
        hle_record(name, "FAIL", (uint32_t)vt, NULL, 0);
        return;
    }
    (void)sceKernelStartVTimer(vt);
    SceKernelSysClock due;
    due.low = first_us;
    due.hi = 0u;
    const uint32_t ref = sceKernelGetSystemTimeLow();
    const int rc = sceKernelSetVTimerHandler(vt, &due, ic_vtimer_handler, NULL);
    sceKernelDelayThread(250000);
    const uint32_t at_end = s_ic_tcount;
    sceKernelDelayThread(150000);
    const uint32_t later = s_ic_tcount;
    (void)sceKernelStopVTimer(vt);
    (void)sceKernelDeleteVTimer(vt);
    ic_tcalls_rows(name, ref, rc, at_end);
    const uint32_t w[1] = {later};
    char row[64];
    snprintf(row, sizeof(row), "%s-after-zero", name);
    hle_record(row, "PASS", later - at_end, w, 1);
}

static void ic_alarm_series(const char *name, const uint32_t *rets, uint32_t first_us)
{
    hle_step(name);
    ic_tcalls_reset(rets);
    const uint32_t ref = sceKernelGetSystemTimeLow();
    const SceUID alarm = sceKernelSetAlarm(first_us, ic_alarm_handler, NULL);
    sceKernelDelayThread(250000);
    const uint32_t at_end = s_ic_tcount;
    sceKernelDelayThread(150000);
    const uint32_t later = s_ic_tcount;
    if (alarm >= 0) {
        (void)sceKernelCancelAlarm(alarm);
    }
    ic_tcalls_rows(name, ref, alarm, at_end);
    const uint32_t w[1] = {later};
    char row[64];
    snprintf(row, sizeof(row), "%s-after-zero", name);
    hle_record(row, "PASS", later - at_end, w, 1);
}

static int hle_run(void)
{
    /* (c) first: timers need no interrupt handler of ours. */
    static const uint32_t rets_20ms[IC_TCALLS] = {20000u, 20000u, 20000u, 20000u, 20000u, 0u, 0u, 0u};
    static const uint32_t rets_small[IC_TCALLS] = {1u, 1u, 1u, 1u, 0u, 0u, 0u, 0u};
    static const uint32_t rets_mixed[IC_TCALLS] = {5000u, 30000u, 1000u, 0u, 0u, 0u, 0u, 0u};
    ic_vtimer_series("vtimer-ret-20ms", rets_20ms, 10000u);
    ic_vtimer_series("vtimer-ret-1us", rets_small, 10000u);
    ic_vtimer_series("vtimer-ret-mixed", rets_mixed, 10000u);
    ic_alarm_series("alarm-ret-20ms", rets_20ms, 10000u);
    ic_alarm_series("alarm-ret-1us", rets_small, 10000u);

    /* (a)/(b): the VBLANK sub-interrupt. */
    /* Sub-interrupt slot 0 of VBLANK is taken in the XMB context (RegisterSubIntrHandler answered
       0x80020067 there, measured 2026-10-24): take the first free slot of 0..15 and record it. */
    hle_step("vbl-register");
    ic_vbl_reset();
    int slot = -1, reg = -1;
    uint32_t tries[16];
    for (int s = 0; s < 16; s++) {
        tries[s] = 0u;
        if (slot < 0) {
            reg = sceKernelRegisterSubIntrHandler(PSP_VBLANK_INT, s, (void *)ic_vbl_handler, NULL);
            tries[s] = (uint32_t)reg;
            if (reg >= 0) {
                slot = s;
            }
        }
    }
    const int en = slot >= 0 ? sceKernelEnableSubIntr(PSP_VBLANK_INT, slot) : -1;
    {
        /* result = the slot used (0xffffffff: none); out0 = Enable's return; out1-16 = Register's
           return per slot tried (0 = not tried). */
        uint32_t w[17];
        w[0] = (uint32_t)en;
        for (int s = 0; s < 16; s++) {
            w[1 + s] = tries[s];
        }
        hle_record("vbl-register", slot >= 0 && en >= 0 ? "PASS" : "FAIL", (uint32_t)slot, w, 17);
    }
    if (slot >= 0 && en >= 0) {
        hle_step("vbl-free");
        ic_vbl_align();
        ic_vbl_reset();
        const uint32_t ref = sceKernelGetSystemTimeLow();
        sceKernelDelayThread(200000);
        ic_vbl_times("vbl-free", ref, 0u, 12u, 0);

        ic_vbl_masked("vbl-masked-20ms", 20000u);
        ic_vbl_masked("vbl-masked-50ms", 50000u);
        ic_vbl_masked("vbl-masked-20ms-again", 20000u);

        hle_step("vbl-hold-25ms");
        ic_vbl_align();
        ic_vbl_reset();
        const uint32_t ref2 = sceKernelGetSystemTimeLow();
        s_ic_vbl_hold_us = 25000u;
        sceKernelDelayThread(200000);
        ic_vbl_times("vbl-hold-25ms", ref2, 0u, 8u, 1);
    }
    /* A registered handler is released on every path, including the one where Enable
       refused the slot (no cell ran then, so only this record follows vbl-register). */
    if (slot >= 0) {
        hle_step("vbl-release");
        const int dis = sceKernelDisableSubIntr(PSP_VBLANK_INT, slot);
        const int rel = sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, slot);
        const uint32_t w[2] = {(uint32_t)dis, (uint32_t)rel};
        hle_record("vbl-release", "PASS", (uint32_t)rel, w, 2);
    }
    return 1;
}

#endif /* HLE_CASE_INTR_CELLS */

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    s_hle_main_thread = sceKernelGetThreadId();
    hle_begin();
    const int ok = hle_run();
    hle_finish(ok);
    hle_park_until_stop();
    return 0;
}
