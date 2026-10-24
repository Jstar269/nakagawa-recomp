// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// HLE issue cells for the PSP-3000 oracle, one PRX and one launch per case:
//   CASE=hle-issues-batch (PSP-HLE-ISSUES-001): #356 model getters and #326 random-unit
//     carry and vscmp NaN/infinity cells;
//   CASE=hle-intr-cells (PSP-HLE-INTR-001): #325 VTimer/alarm handler return values and
//     VBLANK sub-interrupt delivery while interrupts are suspended or a handler holds.
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
#if PSP_ORACLE_CASE == 106
/* hle-issues-batch runs VFPU words on the main thread; without the VFPU attribute the
   first VFPU instruction traps (probe.c's vfpu-compare case, measured). */
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

#if PSP_ORACLE_CASE == HLE_CASE_ISSUES_BATCH
#define HLE_CAMPAIGN_ID "hle-issues-batch"
#define HLE_TEST_ID "PSP-HLE-ISSUES-001"
#define HLE_LOG "host0:/hle_issues_batch_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_INTR_CELLS
#define HLE_CAMPAIGN_ID "hle-intr-cells"
#define HLE_TEST_ID "PSP-HLE-INTR-001"
#define HLE_LOG "host0:/hle_intr_cells_log.txt"
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
