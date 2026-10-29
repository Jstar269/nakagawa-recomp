// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Real-PSP instruction trace for the PSP-3000 oracle, one PRX and one launch:
//   CASE=insn-trace (PSP-TRACE-001, #312): executes each source-owned microprogram of
//     insn_trace_fixtures.h one instruction at a time on the Allegrex and records the
//     complete traced register state (r1..r25, r29..r31, hi, lo) and any store after every
//     instruction, so tools/psp_oracle/insn_trace.py can write a v2 PSP_HARDWARE trace and
//     compare it with Nakagawa's trace of the same words.
//
// How a step runs. The probe keeps the guest state in memory. An ordinary instruction is
// copied into the slot of a trampoline (insn_trace_tramp.S) that loads the guest registers,
// executes it on the CPU and stores them back. A conditional branch is placed in the slot
// with its offset rewritten to the trampoline's taken label and a nop in its delay slot, so
// the CPU decides taken or not; the probe then writes the link register for the linking
// forms (pc + 8), runs the delay-slot instruction as its own step unless a likely branch
// was not taken, and continues at the target or at pc + 8. j/jal take their target from the
// word and jr/jalr from the register, read before the link write and the delay slot. A
// store's address is the base register before the step plus the offset, and its value is
// read back after the step. A word outside these forms stops the fixture with an
// UNSUPPORTED end record; no step is guessed.
//
// Memory: the fixtures' data block is the 4 KiB of user partition 2 at NT_DATA_BASE,
// allocated at that address and zeroed; a fixture never touches anything else. The probe's
// own trampoline buffer is a static array, written back and invalidated in the caches
// before every step.
//
// Record stream and log: the same writer, round trip and module lifecycle as
// probe_hle_issues.c (copied byte for byte): NAKAGAWA_PSP_META, a NAKAGAWA_PSP_STEP per
// fixture, per fixture a `<id>-init` record, one `<id>-sNNNN` record per instruction and a
// `<id>-end` record, then an `insn-trace-done` terminal record whose out0 counts the
// records, the host0 round trip and a final NAKAGAWA_PSP_COMPLETE line.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspsysmem.h>
#include <psputils.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define NT_CASE_TRACE 119

#if PSP_ORACLE_CASE == NT_CASE_TRACE
#define HLE_CAMPAIGN_ID "insn-trace"
#define HLE_TEST_ID "PSP-TRACE-001"
#define HLE_LOG "host0:/insn_trace_log.txt"
#else
#error probe_insn_trace.c has no case for this PSP_ORACLE_CASE
#endif
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

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
#if PSP_ORACLE_CASE == NT_CASE_TRACE

#include "insn_trace_fixtures.h"

#define NT_MAX_STEPS 512u
#define NT_TRAMP_WORDS 160u
#define NT_END_REACHED 0u
#define NT_END_STEP_CAP 1u
#define NT_END_UNSUPPORTED 2u
#define NT_END_SETUP 3u

/* The trampoline's view of the guest (insn_trace_tramp.S documents the layout). */
typedef struct NtState {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t flag;
    uint32_t host[12];
} NtState;

extern const uint32_t nt_tramp_begin[];
extern const uint32_t nt_tramp_slot[];
extern const uint32_t nt_tramp_taken[];
extern const uint32_t nt_tramp_end[];

static uint32_t s_nt_tramp[NT_TRAMP_WORDS] __attribute__((aligned(64)));
static uint32_t s_nt_tramp_words;
static uint32_t s_nt_slot;   /* word index of the slot in s_nt_tramp */
static uint32_t s_nt_taken;  /* word index of the taken label */
static NtState s_nt;
static SceUID s_nt_block = -1;

static int nt_prepare_tramp(void)
{
    const uint32_t words = (uint32_t)(nt_tramp_end - nt_tramp_begin);
    if (words > NT_TRAMP_WORDS) {
        return 0;
    }
    memcpy(s_nt_tramp, nt_tramp_begin, words * 4u);
    s_nt_tramp_words = words;
    s_nt_slot = (uint32_t)(nt_tramp_slot - nt_tramp_begin);
    s_nt_taken = (uint32_t)(nt_tramp_taken - nt_tramp_begin);
    return 1;
}

/* Patch the slot pair and run the trampoline once on s_nt. */
static void nt_run(uint32_t first, uint32_t second)
{
    s_nt_tramp[s_nt_slot] = first;
    s_nt_tramp[s_nt_slot + 1u] = second;
    sceKernelDcacheWritebackRange(s_nt_tramp, s_nt_tramp_words * 4u);
    sceKernelIcacheInvalidateRange(s_nt_tramp, s_nt_tramp_words * 4u);
    void (*tramp)(NtState *) = (void (*)(NtState *))(uintptr_t)s_nt_tramp;
    tramp(&s_nt);
}

static uint32_t nt_fnv(const uint32_t *words, uint32_t count)
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < count; i++) {
        for (unsigned b = 0; b < 4u; b++) {
            h ^= (words[i] >> (8u * b)) & 0xffu;
            h *= 0x01000193u;
        }
    }
    return h;
}

/* The traced registers in record order: r1..r25, r29, r30, r31, hi, lo. */
static uint32_t nt_regs(uint32_t *out)
{
    uint32_t n = 0;
    for (unsigned i = 1; i <= 25u; i++) out[n++] = s_nt.r[i];
    out[n++] = s_nt.r[29];
    out[n++] = s_nt.r[30];
    out[n++] = s_nt.r[31];
    out[n++] = s_nt.hi;
    out[n++] = s_nt.lo;
    return n;
}

static void nt_init_state(void)
{
    memset(&s_nt, 0, sizeof(s_nt));
    for (unsigned i = 1; i < 32u; i++) s_nt.r[i] = 0x01010101u * i;
    s_nt.r[4] = NT_DATA_BASE;
    s_nt.r[29] = NT_DATA_BASE + 0x800u;
    s_nt.r[26] = s_nt.r[27] = s_nt.r[28] = 0u;
    s_nt.hi = NT_HI_INIT;
    s_nt.lo = NT_LO_INIT;
}

static int nt_is_cond_branch(uint32_t w)
{
    const uint32_t op = w >> 26, rt = (w >> 16) & 31u;
    if (op == 4u || op == 5u || op == 6u || op == 7u || (op >= 20u && op <= 23u)) return 1;
    return op == 1u && (rt <= 3u || (rt >= 0x10u && rt <= 0x13u));
}

static int nt_is_likely(uint32_t w)
{
    const uint32_t op = w >> 26, rt = (w >> 16) & 31u;
    return (op >= 20u && op <= 23u) || (op == 1u && (rt & 2u));
}

static int nt_is_control(uint32_t w)
{
    const uint32_t op = w >> 26, fn = w & 63u;
    return nt_is_cond_branch(w) || op == 2u || op == 3u || (op == 0u && (fn == 8u || fn == 9u)) ||
           op == 0x11u || op == 0x12u;
}

/* Ordinary words the trampoline may run: SPECIAL ALU/shift/mult/div/moves (not jr, jalr,
   syscall, break, sync), the immediate ALU group, lui, the byte/half/word loads and
   stores, and the SPECIAL3/Allegrex extension groups. Anything else is unsupported. */
static int nt_is_plain(uint32_t w)
{
    const uint32_t op = w >> 26, fn = w & 63u;
    if (op == 0u) return fn != 8u && fn != 9u && fn != 0x0cu && fn != 0x0du && fn != 0x0fu;
    if (op >= 8u && op <= 15u) return 1;
    if (op == 0x20u || op == 0x21u || op == 0x23u || op == 0x24u || op == 0x25u) return 1;
    if (op == 0x28u || op == 0x29u || op == 0x2bu) return 1;
    return op == 0x1cu || op == 0x1fu;
}

/* Run one ordinary word at pc and record it. */
static void nt_step_plain(const char *id, uint32_t step, uint32_t pc, uint32_t w)
{
    const uint32_t op = w >> 26;
    uint32_t mkind = 0, maddr = 0, mval = 0;
    if (op == 0x28u || op == 0x29u || op == 0x2bu) {
        maddr = s_nt.r[(w >> 21) & 31u] + (uint32_t)(int32_t)(int16_t)(w & 0xffffu);
        mkind = op == 0x28u ? 8u : op == 0x29u ? 16u : 32u;
    }
    nt_run(w, 0u);
    if (mkind == 8u) mval = *(volatile uint8_t *)(uintptr_t)maddr;
    else if (mkind == 16u) mval = *(volatile uint16_t *)(uintptr_t)maddr;
    else if (mkind == 32u) mval = *(volatile uint32_t *)(uintptr_t)maddr;
    static char name[48];
    uint32_t out[35];
    out[0] = pc;
    out[1] = w;
    uint32_t n = 2u + nt_regs(out + 2);
    out[n++] = mkind;
    out[n++] = maddr;
    out[n++] = mval;
    snprintf(name, sizeof(name), "%s-s%04u", id, (unsigned int)step);
    hle_record(name, "PASS", 0, out, n);
}

/* A control word at pc: decide, write the link, record it (no store), then the delay slot. */
static void nt_record_control(const char *id, uint32_t step, uint32_t pc, uint32_t w)
{
    static char name[48];
    uint32_t out[35];
    out[0] = pc;
    out[1] = w;
    uint32_t n = 2u + nt_regs(out + 2);
    out[n++] = 0u;
    out[n++] = 0u;
    out[n++] = 0u;
    snprintf(name, sizeof(name), "%s-s%04u", id, (unsigned int)step);
    hle_record(name, "PASS", 0, out, n);
}

static void nt_trace(const NtFixture *fx)
{
    static char name[48];
    hle_step(fx->id);
    nt_init_state();
    memset((void *)(uintptr_t)NT_DATA_BASE, 0, NT_DATA_BYTES);
    sceKernelDcacheWritebackInvalidateRange((void *)(uintptr_t)NT_DATA_BASE, NT_DATA_BYTES);
    uint32_t init[34];
    init[0] = fx->base;
    init[1] = fx->count;
    init[2] = nt_fnv(fx->words, fx->count);
    init[3] = NT_DATA_BASE;
    const uint32_t ni = 4u + nt_regs(init + 4);
    snprintf(name, sizeof(name), "%s-init", fx->id);
    hle_record(name, "PASS", 0, init, ni);

    const uint32_t end = fx->base + 4u * fx->count;
    uint32_t pc = fx->base;
    uint32_t step = 0;
    uint32_t status = NT_END_STEP_CAP;
    uint32_t last = 0;
    while (step < NT_MAX_STEPS) {
        if (pc == end) {
            status = NT_END_REACHED;
            break;
        }
        if (pc < fx->base || pc > end || (pc & 3u)) {
            status = NT_END_UNSUPPORTED;
            break;
        }
        const uint32_t w = fx->words[(pc - fx->base) / 4u];
        last = w;
        if (nt_is_plain(w)) {
            nt_step_plain(fx->id, step++, pc, w);
            pc += 4u;
            continue;
        }
        if (!nt_is_control(w) || (w >> 26) == 0x11u || (w >> 26) == 0x12u || pc + 4u >= end) {
            status = NT_END_UNSUPPORTED;
            break;
        }
        const uint32_t dw = fx->words[(pc + 4u - fx->base) / 4u];
        if (!nt_is_plain(dw)) {
            last = dw;
            status = NT_END_UNSUPPORTED;
            break;
        }
        const uint32_t op = w >> 26, fn = w & 63u;
        uint32_t target = 0;
        int execute_delay = 1;
        if (nt_is_cond_branch(w)) {
            NtState pre = s_nt;
            const int32_t off = (int32_t)s_nt_taken - (int32_t)(s_nt_slot + 1u);
            nt_run((w & 0xffff0000u) | ((uint32_t)off & 0xffffu), 0u);
            const int taken = s_nt.flag != 0u;
            s_nt = pre;
            if (op == 1u && (((w >> 16) & 31u) & 0x10u)) s_nt.r[31] = pc + 8u;
            target = taken ? pc + 4u + ((uint32_t)(int32_t)(int16_t)(w & 0xffffu) << 2) : pc + 8u;
            execute_delay = taken || !nt_is_likely(w);
        } else if (op == 2u || op == 3u) {
            target = ((pc + 4u) & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
            if (op == 3u) s_nt.r[31] = pc + 8u;
        } else if (op == 0u && (fn == 8u || fn == 9u)) {
            target = s_nt.r[(w >> 21) & 31u];
            const uint32_t rd = (w >> 11) & 31u;
            if (fn == 9u && rd != 0u) s_nt.r[rd] = pc + 8u;
        }
        nt_record_control(fx->id, step++, pc, w);
        if (execute_delay && step < NT_MAX_STEPS) {
            nt_step_plain(fx->id, step++, pc + 4u, dw);
        }
        pc = target;
    }
    const uint32_t tail[4] = {step, pc, status, last};
    snprintf(name, sizeof(name), "%s-end", fx->id);
    hle_record(name, status == NT_END_REACHED ? "PASS" : "FAIL", status, tail, 4);
}

static int hle_run(void)
{
    s_nt_block = sceKernelAllocPartitionMemory(2, "nt-data", PSP_SMEM_Addr, NT_DATA_BYTES,
                                               (void *)(uintptr_t)NT_DATA_BASE);
    const uint32_t head = s_nt_block >= 0
        ? (uint32_t)(uintptr_t)sceKernelGetBlockHeadAddr(s_nt_block) : 0u;
    if (s_nt_block < 0 || head != NT_DATA_BASE || !nt_prepare_tramp()) {
        const uint32_t out[3] = {(uint32_t)s_nt_block, head, s_nt_tramp_words};
        hle_record("insn-trace-setup", "ERROR", NT_END_SETUP, out, 3);
        if (s_nt_block >= 0) sceKernelFreePartitionMemory(s_nt_block);
        return 0;
    }
    for (uint32_t f = 0; f < NT_FIXTURE_COUNT; f++) {
        nt_trace(&nt_fixtures[f]);
    }
    sceKernelFreePartitionMemory(s_nt_block);
    s_nt_block = -1;
    return 1;
}

#endif /* NT_CASE_TRACE */

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
