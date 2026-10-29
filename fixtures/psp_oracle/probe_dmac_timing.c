// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// sceDmacMemcpy / sceDmacTryMemcpy transfer time and busy-channel cells for the PSP-3000
// oracle, one PRX and one launch:
//   CASE=dma-timing (PSP-DMAC-002, #303): how long a transfer keeps its caller and the
//     channel busy, by size, source/destination memory (main RAM or VRAM) and alignment;
//     what a Try refused with BUSY leaves in its destination; whether BUSY is decided
//     before or after the NULL-pointer and zero-size checks; in which order two blocking
//     callers queued behind a pending transfer are served; how long a lower-priority
//     thread sees the channel busy; and, call by call from 0x10 to 0x10000 bytes, which
//     sizes a Try refuses and a blocking call waits for while another transfer is pending.
//
// Memory: three 0x110000-byte blocks from user partition 2 (A, B, C) and the two 1 MiB
// halves of VRAM (0x04000000 and 0x04100000). The CPU fills and checks every byte through
// the uncached mirrors (address | 0x40000000) and the cached alias of every DMA span is
// written back and invalidated before each call, so a stale cache line can neither feed
// nor hide a transfer. Every destination is reset to a sentinel byte before each call and
// 64 bytes past the end of the span (and any bytes before an offset start) are checked,
// where the block has room. The DMAC calls take the cached addresses, as titles pass them.
//
// Record stream and log: the same writer, round trip and module lifecycle as
// probe_hle_issues.c (copied byte for byte): NAKAGAWA_PSP_META, a NAKAGAWA_PSP_STEP before
// each group of cells, one NAKAGAWA_PSP_TEST per cell, a `dma-timing-done` terminal record
// whose out0 counts the records, the host0 round trip and a final NAKAGAWA_PSP_COMPLETE
// line. Steps are never written inside a timed cell. Every loop is bounded; a worker thread
// that does not end within its bound is recorded and terminated. The probe writes only its
// own RAM blocks, the 2 MiB of VRAM and host0 logs.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspsysmem.h>
#include <psputils.h>
#include <pspdmac.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
PSP_MAIN_THREAD_PARAMS(0x20, 32, THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define DMAC_CASE_TIMING 118

#if PSP_ORACLE_CASE == DMAC_CASE_TIMING
#define HLE_CAMPAIGN_ID "dma-timing"
#define HLE_TEST_ID "PSP-DMAC-002"
#define HLE_LOG "host0:/dmac_timing_log.txt"
#else
#error probe_dmac_timing.c has no case for this PSP_ORACLE_CASE
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
#if PSP_ORACLE_CASE == DMAC_CASE_TIMING

#define DT_API_MEMCPY 0u
#define DT_API_TRY 1u
#define DT_BUSY 0x80000021u
#define DT_SENTINEL 0xa5u
#define DT_GUARD 64u
#define DT_TRIALS 8u
#define DT_BATCH 16u
#define DT_BUSY_TRIALS 16u
#define DT_BLOCK_BYTES 0x00110000u     /* each partition-2 block: 1 MiB + 64 KiB of guard room */
#define DT_VRAM_BYTES 0x00100000u      /* each VRAM half */
#define DT_VRAM_DST 0x04000000u
#define DT_VRAM_SRC 0x04100000u
#define DT_UNCACHED 0x40000000u
#define DT_FIRST_PRIORITY 0x10
#define DT_JOIN_TIMEOUT_US 2000000u    /* a worker that has not ended after 2 s is terminated */
#define DT_POLL_MAX 200000u            /* b4: Try polls before the cell gives up */

static uint32_t s_dt_a, s_dt_b, s_dt_c;    /* cached base addresses of the three RAM blocks */
static SceUID s_dt_blocks[3] = {-1, -1, -1};

typedef struct DtJob {
    uint32_t api, dst, src, n;
    volatile uint32_t entered, returned, result, enter_us, exit_us;
} DtJob;

static DtJob s_dt_jobs[3];

static int dt_call(uint32_t api, uint32_t dst, uint32_t src, uint32_t n)
{
    return api == DT_API_TRY ? sceDmacTryMemcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n)
                             : sceDmacMemcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
}

static int dt_worker(SceSize args, void *argp)
{
    const uint32_t index = args == sizeof(uint32_t) ? *(const uint32_t *)argp : 0u;
    DtJob *job = &s_dt_jobs[index < 3u ? index : 0u];
    job->enter_us = sceKernelGetSystemTimeLow();
    job->entered = 1;
    job->result = (uint32_t)dt_call(job->api, job->dst, job->src, job->n);
    job->exit_us = sceKernelGetSystemTimeLow();
    job->returned = 1;
    return 0;
}

static void dt_job(uint32_t index, uint32_t api, uint32_t dst, uint32_t src, uint32_t n)
{
    DtJob *job = &s_dt_jobs[index];
    job->api = api;
    job->dst = dst;
    job->src = src;
    job->n = n;
    job->entered = 0;
    job->returned = 0;
    job->result = 0;
    job->enter_us = 0;
    job->exit_us = 0;
}

/* Create and start worker `index` at `priority`; a stronger priority than main's runs at
   once and is left blocked inside its DMAC call. Returns the thread UID or the error. */
static SceUID dt_start(uint32_t index, int priority)
{
    const SceUID thread = sceKernelCreateThread("dt-worker", dt_worker, priority, 0x2000,
                                                THREAD_ATTR_USER, NULL);
    if (thread < 0) {
        return thread;
    }
    uint32_t arg = index;
    const int started = sceKernelStartThread(thread, sizeof(arg), &arg);
    if (started < 0) {
        sceKernelDeleteThread(thread);
        return started;
    }
    return thread;
}

/* Bounded join: 1 when the worker ended, 0 when it had to be terminated. */
static int dt_join(SceUID thread)
{
    if (thread < 0) {
        return 1;
    }
    SceUInt timeout = DT_JOIN_TIMEOUT_US;
    if (sceKernelWaitThreadEnd(thread, &timeout) < 0) {
        sceKernelTerminateDeleteThread(thread);
        return 0;
    }
    sceKernelDeleteThread(thread);
    return 1;
}

static uint8_t dt_byte(uint32_t seed, uint32_t offset)
{
    uint32_t x = offset * 0x9e3779b1u + seed * 0x85ebca77u;
    x ^= x >> 15;
    return (uint8_t)(x ^ (x >> 8));
}

static volatile uint8_t *dt_u8(uint32_t cached)
{
    return (volatile uint8_t *)(uintptr_t)(cached | DT_UNCACHED);
}

static uint32_t dt_round4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

/* Fill [base, base + round4(len)) with the seeded pattern, byte i = dt_byte(seed, i). */
static void dt_fill(uint32_t base, uint32_t len, uint32_t seed)
{
    volatile uint32_t *words = (volatile uint32_t *)dt_u8(base);
    const uint32_t count = dt_round4(len) / 4u;
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t i = k * 4u;
        words[k] = (uint32_t)dt_byte(seed, i) | ((uint32_t)dt_byte(seed, i + 1u) << 8) |
                   ((uint32_t)dt_byte(seed, i + 2u) << 16) | ((uint32_t)dt_byte(seed, i + 3u) << 24);
    }
}

static void dt_clear(uint32_t base, uint32_t len)
{
    volatile uint32_t *words = (volatile uint32_t *)dt_u8(base);
    const uint32_t count = dt_round4(len) / 4u;
    for (uint32_t k = 0; k < count; k++) {
        words[k] = 0xa5a5a5a5u;
    }
}

/* Bytes of the destination region to reset and check: the span, the guard after it and
   any offset bytes before it, clipped to the region's capacity. */
static uint32_t dt_extent(uint32_t dst_off, uint32_t n, uint32_t cap)
{
    const uint32_t want = dst_off + n + DT_GUARD;
    return dt_round4(want < cap ? want : cap);
}

/* Mismatched bytes in the copied span (0 = the whole request arrived); bytes changed
   outside it, within the checked extent, are added to *stray. */
static uint32_t dt_check(uint32_t dst, uint32_t dst_off, uint32_t n, uint32_t cap,
                         uint32_t src_off, uint32_t seed, uint32_t *stray)
{
    volatile uint8_t *bytes = dt_u8(dst);
    const uint32_t extent = dt_extent(dst_off, n, cap);
    uint32_t bad = 0;
    uint32_t outside = 0;
    if (((dst_off | src_off | n) & 3u) == 0u) {
        volatile uint32_t *words = (volatile uint32_t *)dt_u8(dst + dst_off);
        for (uint32_t k = 0; k < n / 4u; k++) {
            const uint32_t i = src_off + k * 4u;
            const uint32_t expect = (uint32_t)dt_byte(seed, i) | ((uint32_t)dt_byte(seed, i + 1u) << 8) |
                                    ((uint32_t)dt_byte(seed, i + 2u) << 16) |
                                    ((uint32_t)dt_byte(seed, i + 3u) << 24);
            if (words[k] != expect) {
                bad++;
            }
        }
    } else {
        for (uint32_t j = 0; j < n; j++) {
            if (bytes[dst_off + j] != dt_byte(seed, src_off + j)) {
                bad++;
            }
        }
    }
    for (uint32_t j = 0; j < dst_off; j++) {
        if (bytes[j] != DT_SENTINEL) {
            outside++;
        }
    }
    for (uint32_t j = dst_off + n; j < extent; j++) {
        if (bytes[j] != DT_SENTINEL) {
            outside++;
        }
    }
    *stray += outside;
    return bad;
}

static uint32_t dt_changed(uint32_t dst, uint32_t len)
{
    volatile uint8_t *bytes = dt_u8(dst);
    uint32_t changed = 0;
    for (uint32_t j = 0; j < len; j++) {
        if (bytes[j] != DT_SENTINEL) {
            changed++;
        }
    }
    return changed;
}

static void dt_flush(uint32_t addr, uint32_t len)
{
    sceKernelDcacheWritebackInvalidateRange((void *)(uintptr_t)addr, len);
}

static uint32_t dt_alloc(uint32_t index, const char *name)
{
    const SceUID block = sceKernelAllocPartitionMemory(2, name, PSP_SMEM_Low, DT_BLOCK_BYTES + 64u, NULL);
    if (block < 0) {
        return 0;
    }
    s_dt_blocks[index] = block;
    const uint32_t head = (uint32_t)(uintptr_t)sceKernelGetBlockHeadAddr(block);
    return (head + 63u) & ~63u;
}

static void dt_release(void)
{
    for (unsigned i = 0; i < 3u; i++) {
        if (s_dt_blocks[i] >= 0) {
            sceKernelFreePartitionMemory(s_dt_blocks[i]);
            s_dt_blocks[i] = -1;
        }
    }
}

/* ---- t-* and a-*: one caller, no contention -------------------------------------------
   out0 size, out1 api, out2 trials, out3 full copies, out4 min us, out5 max us, out6 sum us,
   out7 us for DT_BATCH back-to-back calls, out8 batch copy full (1/0), out9 stray bytes,
   out10 nonzero results, out11 src offset, out12 dst offset. */
static void dt_time_cell(const char *case_id, uint32_t src, uint32_t dst, uint32_t dst_cap,
                         uint32_t api, uint32_t n, uint32_t src_off, uint32_t dst_off, uint32_t seed)
{
    uint32_t full = 0, min_us = UINT32_MAX, max_us = 0, sum_us = 0, stray = 0, nonzero = 0;
    uint32_t last = 0;
    const uint32_t extent = dt_extent(dst_off, n, dst_cap);
    dt_fill(src, src_off + n, seed);
    for (uint32_t t = 0; t < DT_TRIALS; t++) {
        dt_clear(dst, extent);
        dt_flush(src + src_off, n);
        dt_flush(dst + dst_off, n);
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        const uint32_t r = (uint32_t)dt_call(api, dst + dst_off, src + src_off, n);
        const uint32_t us = sceKernelGetSystemTimeLow() - t0;
        if (us < min_us) min_us = us;
        if (us > max_us) max_us = us;
        sum_us += us;
        if (r != 0u) nonzero++;
        last = r;
        if (dt_check(dst, dst_off, n, dst_cap, src_off, seed, &stray) == 0u) full++;
    }
    dt_clear(dst, extent);
    dt_flush(src + src_off, n);
    dt_flush(dst + dst_off, n);
    const uint32_t b0 = sceKernelGetSystemTimeLow();
    for (uint32_t k = 0; k < DT_BATCH; k++) {
        if (dt_call(api, dst + dst_off, src + src_off, n) != 0) nonzero++;
    }
    const uint32_t batch_us = sceKernelGetSystemTimeLow() - b0;
    const uint32_t batch_full = dt_check(dst, dst_off, n, dst_cap, src_off, seed, &stray) == 0u;
    const uint32_t out[13] = {n, api, DT_TRIALS, full, min_us, max_us, sum_us, batch_us, batch_full,
                              stray, nonzero, src_off, dst_off};
    const int pass = full == DT_TRIALS && batch_full && stray == 0u && nonzero == 0u;
    hle_record(case_id, pass ? "PASS" : "FAIL", last, out, 13);
}

typedef struct DtPair {
    const char *name;
    uint32_t vram_src, vram_dst;    /* 1 = that side is the VRAM half */
} DtPair;

static const DtPair s_dt_pairs[4] = {{"rr", 0u, 0u}, {"rv", 0u, 1u}, {"vr", 1u, 0u}, {"vv", 1u, 1u}};
static const uint32_t s_dt_sizes[9] = {0x10u, 0x40u, 0x100u, 0x400u, 0x1000u, 0x4000u,
                                       0x10000u, 0x40000u, 0x100000u};
static const uint32_t s_dt_align[11][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 2}, {4, 4},
                                           {8, 8}, {16, 16}, {4, 0}, {0, 4}, {32, 0}};

static void dt_timing_cells(void)
{
    static char id[48];
    hle_step("t-cells");
    for (unsigned p = 0; p < 4u; p++) {
        const DtPair *pair = &s_dt_pairs[p];
        const uint32_t src = pair->vram_src ? DT_VRAM_SRC : s_dt_a;
        const uint32_t dst = pair->vram_dst ? DT_VRAM_DST : s_dt_b;
        const uint32_t cap = pair->vram_dst ? DT_VRAM_BYTES : DT_BLOCK_BYTES;
        for (unsigned api = 0; api < 2u; api++) {
            for (unsigned s = 0; s < 9u; s++) {
                snprintf(id, sizeof(id), "t-%s-%s-%06x", pair->name, api ? "t" : "m",
                         (unsigned int)s_dt_sizes[s]);
                dt_time_cell(id, src, dst, cap, api, s_dt_sizes[s], 0u, 0u, 0x100u + p * 32u + api * 16u + s);
            }
        }
    }
    hle_step("a-cells");
    for (unsigned k = 0; k < 11u; k++) {
        for (unsigned z = 0; z < 2u; z++) {
            const uint32_t n = z ? 0x10000u : 0x1000u;
            snprintf(id, sizeof(id), "a-%02x-%02x-%06x", (unsigned int)s_dt_align[k][0],
                     (unsigned int)s_dt_align[k][1], (unsigned int)n);
            dt_time_cell(id, s_dt_a, s_dt_b, DT_BLOCK_BYTES, DT_API_MEMCPY, n, s_dt_align[k][0],
                         s_dt_align[k][1], 0x200u + k * 2u + z);
        }
    }
}

/* The first transfer of every b-cell: 1 MiB VRAM to VRAM from a thread stronger than main,
   which is left blocked in the call while the channel is busy. */
static SceUID dt_first(uint32_t api)
{
    dt_flush(DT_VRAM_SRC, DT_VRAM_BYTES);
    dt_flush(DT_VRAM_DST, DT_VRAM_BYTES);
    dt_job(0, api, DT_VRAM_DST, DT_VRAM_SRC, DT_VRAM_BYTES);
    return dt_start(0, DT_FIRST_PRIORITY);
}

static uint32_t dt_pending(const DtJob *job)
{
    return job->entered && !job->returned;
}

/* ---- b1: what a Try refused with BUSY leaves in its destination -----------------------
   out0 trials, out1 first pending at the Try, out2 BUSY, out3 trials whose destination
   changed, out4 most changed bytes, out5 Try min us, out6 Try max us, out7 other results,
   out8 first-call results not 0, out9 unjoined workers, out10 setup error. */
static void dt_b1(void)
{
    uint32_t pend = 0, busy = 0, changed_trials = 0, changed_max = 0, min_us = UINT32_MAX, max_us = 0;
    uint32_t other = 0, first_bad = 0, unjoined = 0, setup = 0, last = 0;
    const uint32_t n = 0x10000u;
    dt_fill(s_dt_a, n, 0x301u);
    for (uint32_t t = 0; t < DT_BUSY_TRIALS; t++) {
        dt_clear(s_dt_c, n + DT_GUARD);
        dt_flush(s_dt_a, n);
        dt_flush(s_dt_c, n + DT_GUARD);
        const SceUID first = dt_first(DT_API_MEMCPY);
        if (first < 0) {
            setup = (uint32_t)first;
            break;
        }
        const uint32_t pending = dt_pending(&s_dt_jobs[0]);
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        const uint32_t r = (uint32_t)dt_call(DT_API_TRY, s_dt_c, s_dt_a, n);
        const uint32_t us = sceKernelGetSystemTimeLow() - t0;
        if (!dt_join(first)) unjoined++;
        if (s_dt_jobs[0].result != 0u) first_bad++;
        pend += pending;
        if (r == DT_BUSY) busy++; else other++;
        if (us < min_us) min_us = us;
        if (us > max_us) max_us = us;
        last = r;
        const uint32_t changed = dt_changed(s_dt_c, n + DT_GUARD);
        if (changed) changed_trials++;
        if (changed > changed_max) changed_max = changed;
    }
    const uint32_t out[11] = {DT_BUSY_TRIALS, pend, busy, changed_trials, changed_max, min_us, max_us,
                              other, first_bad, unjoined, setup};
    hle_record("b1-try-dest", setup ? "ERROR" : "PASS", last, out, 11);
}

/* ---- b2: BUSY against the NULL-pointer and zero-size checks ---------------------------
   One call per trial while the first transfer is pending. out0 trials, out1 first pending
   at the call, out2 first result, out3 trials with that result, out4 min us, out5 max us,
   out6 first transfer returned before this call returned, out7 first-call results not 0,
   out8 unjoined workers, out9 setup error, out10 api, out11 size. */
static void dt_b2_cell(const char *case_id, uint32_t api, uint32_t null_dst, uint32_t n)
{
    uint32_t pend = 0, first_result = 0, same = 0, min_us = UINT32_MAX, max_us = 0, after = 0;
    uint32_t first_bad = 0, unjoined = 0, setup = 0;
    for (uint32_t t = 0; t < DT_TRIALS; t++) {
        dt_clear(s_dt_c, 0x100u + DT_GUARD);
        dt_flush(s_dt_c, 0x100u + DT_GUARD);
        const SceUID first = dt_first(DT_API_MEMCPY);
        if (first < 0) {
            setup = (uint32_t)first;
            break;
        }
        const uint32_t pending = dt_pending(&s_dt_jobs[0]);
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        const uint32_t r = (uint32_t)dt_call(api, null_dst ? 0u : s_dt_c, s_dt_a, n);
        const uint32_t us = sceKernelGetSystemTimeLow() - t0;
        const uint32_t first_done = s_dt_jobs[0].returned;
        if (!dt_join(first)) unjoined++;
        if (s_dt_jobs[0].result != 0u) first_bad++;
        pend += pending;
        after += first_done;
        if (t == 0u) first_result = r;
        if (r == first_result) same++;
        if (us < min_us) min_us = us;
        if (us > max_us) max_us = us;
    }
    const uint32_t out[12] = {DT_TRIALS, pend, first_result, same, min_us, max_us, after, first_bad,
                              unjoined, setup, api, n};
    hle_record(case_id, setup ? "ERROR" : "PASS", first_result, out, 12);
}

/* ---- b3: two blocking callers queued behind the first transfer ------------------------
   Q1 starts first, then Q2; each copies 0x40000 bytes main RAM to its own block.
   out0 trials, out1 Q1 priority, out2 Q2 priority, out3 Q1 returned first, out4 Q2
   returned first, out5 same microsecond, out6 Q1 result 0, out7 Q2 result 0, out8 Q1 copy
   full, out9 Q2 copy full, out10 both queued while the first was pending, out11 min us
   from the first transfer's return to the earlier queued return, out12 max of that, out13
   min us between the two queued returns, out14 max of that, out15 unjoined workers, out16
   setup error. */
static void dt_b3_cell(const char *case_id, int q1_priority, int q2_priority)
{
    uint32_t q1_first = 0, q2_first = 0, tie = 0, q1_zero = 0, q2_zero = 0, q1_full = 0, q2_full = 0;
    uint32_t queued = 0, lead_min = UINT32_MAX, lead_max = 0, gap_min = UINT32_MAX, gap_max = 0;
    uint32_t unjoined = 0, setup = 0, stray = 0;
    const uint32_t n = 0x40000u;
    dt_fill(s_dt_a, n, 0x401u);
    for (uint32_t t = 0; t < DT_BUSY_TRIALS; t++) {
        dt_clear(s_dt_b, n + DT_GUARD);
        dt_clear(s_dt_c, n + DT_GUARD);
        dt_flush(s_dt_a, n);
        dt_flush(s_dt_b, n + DT_GUARD);
        dt_flush(s_dt_c, n + DT_GUARD);
        const SceUID first = dt_first(DT_API_MEMCPY);
        if (first < 0) {
            setup = (uint32_t)first;
            break;
        }
        dt_job(1, DT_API_MEMCPY, s_dt_b, s_dt_a, n);
        dt_job(2, DT_API_MEMCPY, s_dt_c, s_dt_a, n);
        const SceUID q1 = dt_start(1, q1_priority);
        const uint32_t pending_q1 = dt_pending(&s_dt_jobs[0]) && dt_pending(&s_dt_jobs[1]);
        const SceUID q2 = q1 >= 0 ? dt_start(2, q2_priority) : q1;
        const uint32_t pending_q2 = dt_pending(&s_dt_jobs[0]) && dt_pending(&s_dt_jobs[2]);
        if (!dt_join(first)) unjoined++;
        if (!dt_join(q1)) unjoined++;
        if (!dt_join(q2)) unjoined++;
        if (q1 < 0 || q2 < 0) {
            setup = (uint32_t)(q1 < 0 ? q1 : q2);
            break;
        }
        queued += pending_q1 && pending_q2;
        const uint32_t e0 = s_dt_jobs[0].exit_us, e1 = s_dt_jobs[1].exit_us, e2 = s_dt_jobs[2].exit_us;
        const uint32_t early = e1 < e2 ? e1 : e2;
        const uint32_t late = e1 < e2 ? e2 : e1;
        if (e1 < e2) q1_first++; else if (e2 < e1) q2_first++; else tie++;
        const uint32_t lead = early - e0, gap = late - early;
        if (lead < lead_min) lead_min = lead;
        if (lead > lead_max) lead_max = lead;
        if (gap < gap_min) gap_min = gap;
        if (gap > gap_max) gap_max = gap;
        if (s_dt_jobs[1].result == 0u) q1_zero++;
        if (s_dt_jobs[2].result == 0u) q2_zero++;
        if (dt_check(s_dt_b, 0u, n, n + DT_GUARD, 0u, 0x401u, &stray) == 0u) q1_full++;
        if (dt_check(s_dt_c, 0u, n, n + DT_GUARD, 0u, 0x401u, &stray) == 0u) q2_full++;
    }
    const uint32_t out[17] = {DT_BUSY_TRIALS, (uint32_t)q1_priority, (uint32_t)q2_priority, q1_first,
                              q2_first, tie, q1_zero, q2_zero, q1_full, q2_full, queued, lead_min,
                              lead_max, gap_min, gap_max, unjoined, setup};
    hle_record(case_id, setup ? "ERROR" : stray ? "FAIL" : "PASS", stray, out, 17);
}

/* ---- b4: how long a weaker thread sees the channel busy -------------------------------
   The first transfer (size n, VRAM to VRAM or main RAM to main RAM) runs from the stronger
   thread; main polls sceDmacTryMemcpy with 16 bytes until it is not BUSY.
   out0 size, out1 trials, out2 first pending at the first poll, out3 min polls, out4 max
   polls, out5 min us from the first call's entry to the first non-BUSY poll, out6 max of
   that, out7 min us of the first call, out8 max of that, out9 last non-BUSY result,
   out10 cells that reached DT_POLL_MAX, out11 first-call results not 0, out12 unjoined
   workers, out13 setup error, out14 1 = VRAM pair. */
static void dt_b4_cell(const char *case_id, uint32_t vram, uint32_t n)
{
    uint32_t pend = 0, polls_min = UINT32_MAX, polls_max = 0, win_min = UINT32_MAX, win_max = 0;
    uint32_t call_min = UINT32_MAX, call_max = 0, last = 0, capped = 0, first_bad = 0;
    uint32_t unjoined = 0, setup = 0;
    const uint32_t src = vram ? DT_VRAM_SRC : s_dt_a;
    const uint32_t dst = vram ? DT_VRAM_DST : s_dt_b;
    for (uint32_t t = 0; t < DT_TRIALS; t++) {
        dt_flush(src, n);
        dt_flush(dst, n);
        dt_flush(s_dt_c, 0x10u);
        dt_job(0, DT_API_MEMCPY, dst, src, n);
        const SceUID first = dt_start(0, DT_FIRST_PRIORITY);
        if (first < 0) {
            setup = (uint32_t)first;
            break;
        }
        pend += dt_pending(&s_dt_jobs[0]);
        uint32_t polls = 0;
        uint32_t r = DT_BUSY;
        for (; polls < DT_POLL_MAX; polls++) {
            r = (uint32_t)dt_call(DT_API_TRY, s_dt_c, s_dt_a + 0x100000u, 0x10u);
            if (r != DT_BUSY) {
                break;
            }
        }
        const uint32_t free_at = sceKernelGetSystemTimeLow();
        if (!dt_join(first)) unjoined++;
        if (polls == DT_POLL_MAX) capped++;
        if (s_dt_jobs[0].result != 0u) first_bad++;
        last = r;
        const uint32_t win = free_at - s_dt_jobs[0].enter_us;
        const uint32_t call = s_dt_jobs[0].exit_us - s_dt_jobs[0].enter_us;
        if (polls < polls_min) polls_min = polls;
        if (polls > polls_max) polls_max = polls;
        if (win < win_min) win_min = win;
        if (win > win_max) win_max = win;
        if (call < call_min) call_min = call;
        if (call > call_max) call_max = call;
    }
    const uint32_t out[15] = {n, DT_TRIALS, pend, polls_min, polls_max, win_min, win_max, call_min,
                              call_max, last, capped, first_bad, unjoined, setup, vram};
    hle_record(case_id, setup ? "ERROR" : capped ? "TIMEOUT" : "PASS", last, out, 15);
}

/* ---- c1 / c2: one call of each size while the first transfer is pending ---------------
   c1 is sceDmacTryMemcpy and c2 sceDmacMemcpy, main RAM A to C. out0 size, out1 trials,
   out2 first pending at the call, out3 BUSY, out4 results 0, out5 other results, out6 min us,
   out7 max us, out8 first transfer returned before this call returned, out9 full copies
   (destination checked after the first transfer ended), out10 trials whose destination
   changed at all, out11 unjoined workers, out12 setup error. */
static void dt_c_cell(const char *case_id, uint32_t api, uint32_t n)
{
    uint32_t pend = 0, busy = 0, zero = 0, other = 0, min_us = UINT32_MAX, max_us = 0, after = 0;
    uint32_t full = 0, changed = 0, unjoined = 0, setup = 0, stray = 0, last = 0;
    const uint32_t seed = 0x500u + n / 16u;
    dt_fill(s_dt_a, n, seed);
    for (uint32_t t = 0; t < DT_TRIALS; t++) {
        dt_clear(s_dt_c, n + DT_GUARD);
        dt_flush(s_dt_a, n);
        dt_flush(s_dt_c, n + DT_GUARD);
        const SceUID first = dt_first(DT_API_MEMCPY);
        if (first < 0) {
            setup = (uint32_t)first;
            break;
        }
        const uint32_t pending = dt_pending(&s_dt_jobs[0]);
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        const uint32_t r = (uint32_t)dt_call(api, s_dt_c, s_dt_a, n);
        const uint32_t us = sceKernelGetSystemTimeLow() - t0;
        const uint32_t first_done = s_dt_jobs[0].returned;
        if (!dt_join(first)) unjoined++;
        pend += pending;
        after += first_done;
        if (r == DT_BUSY) busy++; else if (r == 0u) zero++; else other++;
        if (us < min_us) min_us = us;
        if (us > max_us) max_us = us;
        last = r;
        if (dt_check(s_dt_c, 0u, n, n + DT_GUARD, 0u, seed, &stray) == 0u) full++;
        if (dt_changed(s_dt_c, n + DT_GUARD)) changed++;
    }
    const uint32_t out[13] = {n, DT_TRIALS, pend, busy, zero, other, min_us, max_us, after, full,
                              changed, unjoined, setup};
    hle_record(case_id, setup ? "ERROR" : "PASS", last, out, 13);
}

/* ---- b5: the busy window, polled with a Try that the pending transfer refuses ---------
   As b4, but each poll is a 0x10000-byte Try (main RAM A + 1 MiB is not a source of the
   first transfer), so the window ends at the start of the first poll the channel accepts.
   out0 size, out1 trials, out2 first pending at the first poll, out3 min BUSY polls, out4
   max, out5 min us from the first call's entry to the start of the accepted poll, out6 max
   of that, out7 min us of the first call, out8 max of that, out9 min us from the first
   call's return to the start of the accepted poll (signed, as a 32-bit word), out10 max of
   that, out11 last accepted result, out12 cells that reached DT_POLL_MAX, out13 unjoined
   workers, out14 setup error, out15 1 = VRAM pair. */
static void dt_b5_cell(const char *case_id, uint32_t vram, uint32_t n)
{
    uint32_t pend = 0, polls_min = UINT32_MAX, polls_max = 0, win_min = UINT32_MAX, win_max = 0;
    uint32_t call_min = UINT32_MAX, call_max = 0, last = 0, capped = 0, unjoined = 0, setup = 0;
    int32_t tail_min = INT32_MAX, tail_max = INT32_MIN;
    const uint32_t src = vram ? DT_VRAM_SRC : s_dt_a;
    const uint32_t dst = vram ? DT_VRAM_DST : s_dt_b;
    for (uint32_t t = 0; t < DT_TRIALS; t++) {
        dt_flush(src, n);
        dt_flush(dst, n);
        dt_flush(s_dt_a + 0x100000u, 0x10000u);
        dt_flush(s_dt_c, 0x10000u);
        dt_job(0, DT_API_MEMCPY, dst, src, n);
        const SceUID first = dt_start(0, DT_FIRST_PRIORITY);
        if (first < 0) {
            setup = (uint32_t)first;
            break;
        }
        pend += dt_pending(&s_dt_jobs[0]);
        uint32_t polls = 0;
        uint32_t r = DT_BUSY;
        uint32_t accepted_at = 0;
        for (; polls < DT_POLL_MAX; polls++) {
            accepted_at = sceKernelGetSystemTimeLow();
            r = (uint32_t)dt_call(DT_API_TRY, s_dt_c, s_dt_a + 0x100000u, 0x10000u);
            if (r != DT_BUSY) {
                break;
            }
        }
        if (!dt_join(first)) unjoined++;
        if (polls == DT_POLL_MAX) capped++;
        last = r;
        const uint32_t win = accepted_at - s_dt_jobs[0].enter_us;
        const uint32_t call = s_dt_jobs[0].exit_us - s_dt_jobs[0].enter_us;
        const int32_t tail = (int32_t)(accepted_at - s_dt_jobs[0].exit_us);
        if (polls < polls_min) polls_min = polls;
        if (polls > polls_max) polls_max = polls;
        if (win < win_min) win_min = win;
        if (win > win_max) win_max = win;
        if (call < call_min) call_min = call;
        if (call > call_max) call_max = call;
        if (tail < tail_min) tail_min = tail;
        if (tail > tail_max) tail_max = tail;
    }
    const uint32_t out[16] = {n, DT_TRIALS, pend, polls_min, polls_max, win_min, win_max, call_min,
                              call_max, (uint32_t)tail_min, (uint32_t)tail_max, last, capped,
                              unjoined, setup, vram};
    hle_record(case_id, setup ? "ERROR" : capped ? "TIMEOUT" : "PASS", last, out, 16);
}

/* ---- c4: which bytes a Try changes while another transfer is pending -------------------
   One Try per trial, main RAM A + src_off to C + dst_off, n bytes; the destination region
   [0, dst_off + n + 64) is reset to the sentinel first and read back after the first
   transfer ended. out0 size, out1 src offset, out2 dst offset, out3 trials, out4 BUSY,
   out5 results 0, out6 other results, out7 min changed bytes, out8 max changed bytes, out9
   lowest changed offset relative to C + dst_off (0xffffffff when none; signed when below),
   out10 highest changed offset, out11 changed bytes equal to the source byte for their
   offset, out12 min us, out13 max us, out14 unjoined workers, out15 setup error. */
static void dt_c4_cell(const char *case_id, uint32_t n, uint32_t src_off, uint32_t dst_off)
{
    uint32_t busy = 0, zero = 0, other = 0, ch_min = UINT32_MAX, ch_max = 0, matched = 0;
    uint32_t lo_abs = UINT32_MAX, hi = 0, min_us = UINT32_MAX, max_us = 0, unjoined = 0, setup = 0;
    uint32_t last = 0;
    const uint32_t seed = 0x800u + n + src_off * 3u + dst_off * 5u;
    const uint32_t extent = dt_round4(dst_off + n + DT_GUARD);
    dt_fill(s_dt_a, src_off + n, seed);
    for (uint32_t t = 0; t < DT_TRIALS; t++) {
        dt_clear(s_dt_c, extent);
        dt_flush(s_dt_a, src_off + n);
        dt_flush(s_dt_c, extent);
        const SceUID first = dt_first(DT_API_MEMCPY);
        if (first < 0) {
            setup = (uint32_t)first;
            break;
        }
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        const uint32_t r = (uint32_t)dt_call(DT_API_TRY, s_dt_c + dst_off, s_dt_a + src_off, n);
        const uint32_t us = sceKernelGetSystemTimeLow() - t0;
        if (!dt_join(first)) unjoined++;
        if (r == DT_BUSY) busy++; else if (r == 0u) zero++; else other++;
        if (us < min_us) min_us = us;
        if (us > max_us) max_us = us;
        last = r;
        volatile uint8_t *bytes = dt_u8(s_dt_c);
        uint32_t changed = 0;
        for (uint32_t j = 0; j < extent; j++) {
            if (bytes[j] == DT_SENTINEL) {
                continue;
            }
            changed++;
            const uint32_t rel = j - dst_off;   /* wraps below dst_off: read as signed */
            if (lo_abs == UINT32_MAX || j < lo_abs) lo_abs = j;
            if (j >= dst_off && rel > hi) hi = rel;
            if (j >= dst_off && rel < n && bytes[j] == dt_byte(seed, src_off + rel)) matched++;
        }
        if (changed < ch_min) ch_min = changed;
        if (changed > ch_max) ch_max = changed;
    }
    const uint32_t lo = lo_abs == UINT32_MAX ? UINT32_MAX : lo_abs - dst_off;
    const uint32_t out[16] = {n, src_off, dst_off, DT_TRIALS, busy, zero, other, ch_min, ch_max, lo,
                              hi, matched, min_us, max_us, unjoined, setup};
    hle_record(case_id, setup ? "ERROR" : "PASS", last, out, 16);
}

/* {size, src offset, dst offset} */
static const uint32_t s_dt_c4[16][3] = {
    {0x109u, 0, 0}, {0x10cu, 0, 0}, {0x10fu, 0, 0}, {0x1f1u, 0, 0}, {0x1f4u, 0, 0},
    {0x1f8u, 0, 0}, {0x1fcu, 0, 0}, {0x1ffu, 0, 0}, {0x201u, 0, 0}, {0x20cu, 0, 0},
    {0x10004u, 0, 0}, {0x200u, 1, 0}, {0x200u, 0, 1}, {0x200u, 4, 4}, {0x1fcu, 4, 4},
    {0x108u, 0, 1}};

static const uint32_t s_dt_c_sizes[13] = {0x10u, 0x20u, 0x40u, 0x80u, 0x100u, 0x200u, 0x400u,
                                          0x800u, 0x1000u, 0x2000u, 0x4000u, 0x8000u, 0x10000u};
static const uint32_t s_dt_align2[6][2] = {{2, 0}, {0, 2}, {3, 0}, {8, 0}, {0, 8}, {12, 0}};
/* Between the largest size a pending transfer let through (0x100) and the smallest it
   refused (0x200) in the first runs. */
static const uint32_t s_dt_c3_sizes[11] = {0x101u, 0x104u, 0x108u, 0x110u, 0x120u, 0x140u,
                                           0x180u, 0x1c0u, 0x1f0u, 0x1fcu, 0x1ffu};
/* Sizes between the t-cell points, for the duration table. */
static const uint32_t s_dt_t2_sizes[6] = {0x200u, 0x800u, 0x2000u, 0x8000u, 0x20000u, 0x80000u};

static void dt_followup_cells(void)
{
    static char id[48];
    hle_step("c1");
    for (unsigned s = 0; s < 13u; s++) {
        snprintf(id, sizeof(id), "c1-try-%06x", (unsigned int)s_dt_c_sizes[s]);
        dt_c_cell(id, DT_API_TRY, s_dt_c_sizes[s]);
    }
    hle_step("c2");
    for (unsigned s = 0; s < 13u; s++) {
        snprintf(id, sizeof(id), "c2-memcpy-%06x", (unsigned int)s_dt_c_sizes[s]);
        dt_c_cell(id, DT_API_MEMCPY, s_dt_c_sizes[s]);
    }
    hle_step("b5");
    dt_b5_cell("b5-vv-010000", 1u, 0x10000u);
    dt_b5_cell("b5-vv-040000", 1u, 0x40000u);
    dt_b5_cell("b5-vv-100000", 1u, 0x100000u);
    dt_b5_cell("b5-rr-040000", 0u, 0x40000u);
    dt_b5_cell("b5-rr-0c0000", 0u, 0xc0000u);
    hle_step("c3");
    for (unsigned s = 0; s < 11u; s++) {
        snprintf(id, sizeof(id), "c3-try-%06x", (unsigned int)s_dt_c3_sizes[s]);
        dt_c_cell(id, DT_API_TRY, s_dt_c3_sizes[s]);
    }
    hle_step("t2-cells");
    for (unsigned p = 0; p < 4u; p++) {
        const DtPair *pair = &s_dt_pairs[p];
        const uint32_t src = pair->vram_src ? DT_VRAM_SRC : s_dt_a;
        const uint32_t dst = pair->vram_dst ? DT_VRAM_DST : s_dt_b;
        const uint32_t cap = pair->vram_dst ? DT_VRAM_BYTES : DT_BLOCK_BYTES;
        for (unsigned s = 0; s < 6u; s++) {
            snprintf(id, sizeof(id), "t2-%s-m-%06x", pair->name, (unsigned int)s_dt_t2_sizes[s]);
            dt_time_cell(id, src, dst, cap, DT_API_MEMCPY, s_dt_t2_sizes[s], 0u, 0u, 0x700u + p * 8u + s);
        }
    }
    hle_step("c4");
    for (unsigned k = 0; k < 16u; k++) {
        snprintf(id, sizeof(id), "c4-%06x-%02x-%02x", (unsigned int)s_dt_c4[k][0],
                 (unsigned int)s_dt_c4[k][1], (unsigned int)s_dt_c4[k][2]);
        dt_c4_cell(id, s_dt_c4[k][0], s_dt_c4[k][1], s_dt_c4[k][2]);
    }
    hle_step("a2-cells");
    for (unsigned k = 0; k < 6u; k++) {
        for (unsigned z = 0; z < 2u; z++) {
            const uint32_t n = z ? 0x10000u : 0x1000u;
            snprintf(id, sizeof(id), "a-%02x-%02x-%06x", (unsigned int)s_dt_align2[k][0],
                     (unsigned int)s_dt_align2[k][1], (unsigned int)n);
            dt_time_cell(id, s_dt_a, s_dt_b, DT_BLOCK_BYTES, DT_API_MEMCPY, n, s_dt_align2[k][0],
                         s_dt_align2[k][1], 0x600u + k * 2u + z);
        }
    }
}

static void dt_busy_cells(void)
{
    dt_fill(DT_VRAM_SRC, DT_VRAM_BYTES, 0x300u);
    hle_step("b1");
    dt_b1();
    hle_step("b2");
    dt_fill(s_dt_a, 0x100u, 0x302u);
    dt_b2_cell("b2-try-null", DT_API_TRY, 1u, 0x100u);
    dt_b2_cell("b2-try-zero", DT_API_TRY, 0u, 0u);
    dt_b2_cell("b2-memcpy-null", DT_API_MEMCPY, 1u, 0x100u);
    dt_b2_cell("b2-memcpy-zero", DT_API_MEMCPY, 0u, 0u);
    dt_b2_cell("b2-try-valid", DT_API_TRY, 0u, 0x100u);
    hle_step("b3");
    dt_b3_cell("b3-second-higher", 0x1c, 0x18);
    dt_b3_cell("b3-second-lower", 0x18, 0x1c);
    hle_step("b4");
    dt_b4_cell("b4-vv-010000", 1u, 0x10000u);
    dt_b4_cell("b4-vv-040000", 1u, 0x40000u);
    dt_b4_cell("b4-vv-100000", 1u, 0x100000u);
    dt_b4_cell("b4-rr-010000", 0u, 0x10000u);
    dt_b4_cell("b4-rr-040000", 0u, 0x40000u);
    dt_b4_cell("b4-rr-100000", 0u, 0x100000u);
}

static int hle_run(void)
{
    s_dt_a = dt_alloc(0, "dt-a");
    s_dt_b = dt_alloc(1, "dt-b");
    s_dt_c = dt_alloc(2, "dt-c");
    if (!s_dt_a || !s_dt_b || !s_dt_c) {
        const uint32_t out[3] = {s_dt_a, s_dt_b, s_dt_c};
        hle_record("dt-setup", "ERROR", 0, out, 3);
        dt_release();
        return 0;
    }
    dt_flush(s_dt_a, DT_BLOCK_BYTES);
    dt_flush(s_dt_b, DT_BLOCK_BYTES);
    dt_flush(s_dt_c, DT_BLOCK_BYTES);
    dt_timing_cells();
    dt_busy_cells();
    dt_followup_cells();
    dt_release();
    return 1;
}

#endif /* DMAC_CASE_TIMING */

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
