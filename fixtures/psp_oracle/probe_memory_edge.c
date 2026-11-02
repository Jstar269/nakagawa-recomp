// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Memory-edge cells for the PSP-3000 oracle, one PRX and one launch:
//   CASE=memory-edge (PSP-MEMEDGE-001): where user-visible memory ends, and what sceIoWrite,
//     sceIoRead, sceDmacMemcpy and sceDmacTryMemcpy do when a buffer's range runs past that
//     edge (#303's invalid-tail and boundary cells; #1380's sceIoRead into a boundary
//     buffer). The placement helper is mem_edge.h, which any probe source can include.
//
// What it measures. The top and bottom of user RAM from the partition allocator's answers
// (edge-ram-hi, edge-ram-lo) and the end of VRAM from what the file calls accept
// (edge-vram), then three placements (the last N = 0x100 bytes of user RAM, the first
// N bytes of user RAM, the last N bytes of VRAM). Each placement is called with length
// N + delta (delta 0, 1, 4 and 0x1000 for the reading side, 0, 1 and 4 for the writing
// side) and every call's outcome is one record: the return code, the bytes of the buffer that
// changed, the bytes of the sink that changed, the guard bytes that changed and the setup
// mask. A record exists only if the call returned; a call that faults or hangs leaves the
// NAKAGAWA_PSP_STEP written before it as the last line, naming the cell.
//
// Safety order. A reading call (sceIoWrite from the buffer, DMA source) only reads the bytes
// past the edge, into a probe-owned host0 file or RAM array. A writing call (sceIoRead into
// the buffer, DMA destination) runs past the edge only after the same API refused the same
// overrun on the reading side with a negative return code (reason 3 otherwise), and only on
// memory the probe owns (reason 2 otherwise), so no write is sent past an edge without proof
// that the API checks ranges. Reading past the top of user RAM runs only when the RAM above
// it exists (reason 4 otherwise). Direct CPU loads and stores past an edge, and any file
// call from the scratchpad (the first launch's did not return), are SKIP rows with their
// reasons: a fault in user mode cannot be recorded by the mechanisms the current probes use.
//
// Record stream and log: the same writer, host0 round trip and module lifecycle as
// probe_hle_issues.c (copied byte for byte, pinned by tools/test_psp_oracle.py): a
// NAKAGAWA_PSP_STEP before each cell, one NAKAGAWA_PSP_TEST per cell (PASS when the call was
// made and recorded, SKIP when it was not run), a `memory-edge-done` terminal record whose
// out0 counts the records, the host0 round trip and a final NAKAGAWA_PSP_COMPLETE line.
// Raw bytes read from beyond an edge go only to a probe-owned host0 file and are never
// inspected or recorded; the records carry counts and return codes.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspsysmem.h>
#include <pspdmac.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mem_edge.h"

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define ME_CASE_EDGE 138
#if PSP_ORACLE_CASE != ME_CASE_EDGE
#error probe_memory_edge.c has no case for this PSP_ORACLE_CASE
#endif
#define HLE_CAMPAIGN_ID "memory-edge"
#define HLE_TEST_ID "PSP-MEMEDGE-001"
#define HLE_LOG "host0:/memory_edge_log.txt"
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
    static char line[1536];
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
/* Memory-edge cells (PSP-MEMEDGE-001). */

#define ME_N 0x100u            /* bytes of valid memory at the edge */
#define ME_BUF_BYTES 0x1140u   /* the sink and source arrays: N plus the largest delta */
#define ME_DST_FILL 0x00u      /* a writing call's buffer before the call */
#define ME_SINK_FILL 0xEEu     /* a reading call's sink before the call (the pattern is odd) */
#define ME_GUARD_FILL 0x5Au
#define ME_WORDS 12u           /* words of every cell record */
#define ME_NOT_RUN 0xA5A5A5A5u /* a measurement that could not run */
#define ME_DATA_FILE "host0:/memory_edge_data.bin"
#define ME_SINK_FILE "host0:/memory_edge_sink.bin"

/* SKIP reasons, word 10 of a SKIP cell. */
#define ME_SKIP_PLACEMENT 1u /* the placement did not succeed (setup mask in word 9) */
#define ME_SKIP_NOT_OWNED 2u /* a writing call needs memory the probe owns */
#define ME_SKIP_UNPROVEN 3u  /* the same API did not refuse the same overrun on the reading side */
#define ME_SKIP_NO_RAM 4u    /* no RAM known to exist above the edge */
#define ME_SKIP_FAULT 5u     /* a fault cannot be recorded in user mode */
#define ME_SKIP_UNBACKED 6u  /* nothing is mapped past the edge */
#define ME_SKIP_NO_FILE 7u   /* the host0 data or sink file could not be opened */

#define ME_API_IO 0u
#define ME_API_MEMCPY 1u
#define ME_API_TRY 2u

static const char *const k_me_places[3] = {"ramtail", "ramhead", "vramtail"};
static const char *const k_me_apis[3] = {"io", "memcpy", "try"};
static const uint32_t k_me_src_deltas[4] = {0u, 1u, 4u, 0x1000u};
static const uint32_t k_me_dst_deltas[3] = {0u, 1u, 4u};

static uint8_t s_me_sink[ME_BUF_BYTES] __attribute__((aligned(64)));
static uint8_t s_me_src[ME_BUF_BYTES] __attribute__((aligned(64)));
/* [place][api][delta index]: the reading call at that delta returned a negative code. */
static uint8_t s_me_refused[3][3][4];
static int s_me_data_ok;

static uint8_t me_pattern(uint32_t i)
{
    return (uint8_t)((i * 7u + 1u) | 1u);
}

static void me_skip(const char *id, uint32_t mask, uint32_t reason)
{
    uint32_t w[ME_WORDS];
    memset(w, 0, sizeof(w));
    w[9] = mask;
    w[10] = reason;
    hle_record(id, "SKIP", 0, w, ME_WORDS);
}

/* Rows edge-ram-hi and edge-ram-lo (8 words each): the allocator's raw answers behind the
   two ends of user RAM. 0 = the page was handed out, otherwise the allocator's code. */
static void me_emit_ram(const me_bounds *b)
{
    uint32_t w[8];
    hle_step("edge-ram-hi");
    w[0] = b->hi_head;
    w[1] = b->ram_hi;
    w[2] = b->hi_rc_end;
    w[3] = b->hi_rc_next;
    w[4] = b->hi_rc_far;
    w[5] = b->free_total;
    w[6] = b->free_max;
    w[7] = b->mask;
    hle_record("edge-ram-hi", "PASS", 0, w, 8);
    hle_step("edge-ram-lo");
    w[0] = b->lo_head;
    w[1] = b->ram_lo;
    w[2] = b->lo_rc_kernel;
    w[3] = b->lo_rc_base;
    w[4] = b->lo_rc_next;
    w[5] = 0;
    w[6] = 0;
    w[7] = b->mask;
    hle_record("edge-ram-lo", "PASS", 0, w, 8);
}

/* A read-side oracle: write `len` bytes from `ptr` to the probe's sink file. The return code
   is the file call's own (the byte count, or an error code). */
static uint32_t me_oracle(const void *ptr, uint32_t len)
{
    const SceUID fd = sceIoOpen(ME_SINK_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return ME_NOT_RUN;
    }
    const int rc = sceIoWrite(fd, ptr, len);
    (void)sceIoClose(fd);
    return (uint32_t)rc;
}

/* Row edge-data (3 words): the data file the sceIoRead cells read, N + the largest delta
   bytes of the pattern: bytes written, bytes expected, close result. */
static void me_make_data(void)
{
    uint32_t w[3] = {0, ME_BUF_BYTES, 0};
    hle_step("edge-data");
    for (uint32_t i = 0; i < ME_BUF_BYTES; i++) {
        s_me_src[i] = me_pattern(i);
    }
    const SceUID fd = sceIoOpen(ME_DATA_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        w[0] = (uint32_t)sceIoWrite(fd, s_me_src, ME_BUF_BYTES);
        w[2] = (uint32_t)sceIoClose(fd);
    } else {
        w[0] = (uint32_t)fd;
        w[2] = ME_NOT_RUN;
    }
    s_me_data_ok = w[0] == ME_BUF_BYTES && w[2] == 0u;
    hle_record("edge-data", "PASS", 0, w, 3);
}

/* Row edge-vram (4 words): what the file calls accept at the end of VRAM, as the read-side
   oracle. 0: the last page of VRAM; 1: its last word; 2: eight bytes across the end (the
   mirror starts there); 3: the bounds mask afterwards. A read of memory the kernel is asked to
   copy into the probe's sink file; nothing is written. Each call has its own step. It runs
   after the user-RAM cells because it is the first file call the probe hands a VRAM pointer. */
static void me_emit_vram(me_bounds *b)
{
    uint32_t w[4];
    hle_step("edge-vram-page");
    w[0] = me_oracle((const void *)(uintptr_t)(b->vram_hi - ME_PAGE), ME_PAGE);
    hle_step("edge-vram-last");
    w[1] = me_oracle((const void *)(uintptr_t)(b->vram_hi - 4u), 4u);
    if (w[0] == ME_PAGE && w[1] == 4u) {
        b->mask |= ME_B_VRAM;
    }
    hle_step("edge-vram-cross");
    w[2] = me_oracle((const void *)(uintptr_t)(b->vram_hi - 4u), 8u);
    w[3] = b->mask;
    hle_record("edge-vram", "PASS", 0, w, 4);
}

/* Row edge-place-<place> (8 words): the setup mask, buffer and block addresses, N, the block
   size, the allocator's answer for the page outside the edge, the guard length and the kind. */
static void me_emit_place(const char *place_name, const me_place *p)
{
    char id[40];
    uint32_t w[8];
    snprintf(id, sizeof(id), "edge-place-%s", place_name);
    hle_step(id);
    w[0] = p->setup_mask;
    w[1] = (uint32_t)(uintptr_t)p->buf;
    w[2] = p->n;
    w[3] = (uint32_t)(uintptr_t)p->block;
    w[4] = p->block_size;
    w[5] = p->outside_rc;
    w[6] = me_guard_len(p);
    w[7] = (uint32_t)p->kind;
    hle_record(id, "PASS", 0, w, 8);
}

static uint32_t me_dma(unsigned api, void *dst, const void *src, uint32_t len)
{
    return (uint32_t)(api == ME_API_TRY ? sceDmacTryMemcpy(dst, src, len)
                                        : sceDmacMemcpy(dst, src, len));
}

/* One call at one placement. Words: 0 return code; 1 returned (1: this record exists);
   2 N; 3 requested length; 4 buffer bytes that changed (a writing call: bytes no longer the
   fill; a reading call: bytes no longer the pattern); 5 sink (file position after, or the
   DMA sink bytes that changed); 6 first changed buffer offset (0xFFFFFFFF none); 7 sink bytes
   that changed on the outside-the-edge side (DMA), or the return code beyond N (file);
   8 guard bytes that changed; 9 setup mask; 10 skip reason (0 here); 11 call pointer.
   `refused` receives 1 when the return code was negative. */
static void me_run_cell(const char *id, me_place *pl, unsigned api, int is_dst, uint32_t delta,
                        uint8_t *refused)
{
    const uint32_t n = pl->n;
    const uint32_t len = n + delta;
    uint8_t *const ptr = me_call_ptr(pl, delta);
    const int owned = (pl->setup_mask & ME_SETUP_OWNED) != 0;
    const void *const cache_base = pl->block ? (const void *)pl->block : (const void *)pl->buf;
    const uint32_t cache_len = pl->block ? pl->block_size : n;
    uint32_t w[ME_WORDS];
    memset(w, 0, sizeof(w));
    hle_step(id);

    if (owned) {
        for (uint32_t i = 0; i < n; i++) {
            pl->buf[i] = is_dst ? (uint8_t)ME_DST_FILL : me_pattern(i);
        }
        if (me_guard_ptr(pl)) {
            memset(me_guard_ptr(pl), ME_GUARD_FILL, me_guard_len(pl));
        }
        sceKernelDcacheWritebackInvalidateRange(cache_base, cache_len);
    }
    SceUID fd = -1;
    if (api == ME_API_IO) {
        fd = is_dst ? sceIoOpen(ME_DATA_FILE, PSP_O_RDONLY, 0)
                    : sceIoOpen(ME_SINK_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd < 0) {
            me_skip(id, pl->setup_mask, ME_SKIP_NO_FILE);
            return;
        }
    } else if (is_dst) {
        for (uint32_t i = 0; i < ME_BUF_BYTES; i++) {
            s_me_src[i] = me_pattern(i);
        }
        sceKernelDcacheWritebackInvalidateRange(s_me_src, ME_BUF_BYTES);
    } else {
        memset(s_me_sink, ME_SINK_FILL, ME_BUF_BYTES);
        sceKernelDcacheWritebackInvalidateRange(s_me_sink, ME_BUF_BYTES);
    }

    uint32_t rc;
    uint32_t pos = 0;
    if (api == ME_API_IO) {
        rc = (uint32_t)(is_dst ? sceIoRead(fd, ptr, len) : sceIoWrite(fd, ptr, len));
        pos = (uint32_t)sceIoLseek32(fd, 0, PSP_SEEK_CUR);
        (void)sceIoClose(fd);
    } else {
        rc = is_dst ? me_dma(api, ptr, s_me_src, len) : me_dma(api, s_me_sink, ptr, len);
        sceKernelDcacheInvalidateRange(s_me_sink, ME_BUF_BYTES);
    }
    if (owned) {
        sceKernelDcacheInvalidateRange(cache_base, cache_len);
    }

    uint32_t changed = 0;
    uint32_t first = 0xFFFFFFFFu;
    if (owned) {
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t expect = is_dst ? (uint8_t)ME_DST_FILL : me_pattern(i);
            if (pl->buf[i] != expect) {
                changed++;
                if (first == 0xFFFFFFFFu) {
                    first = i;
                }
            }
        }
        for (uint32_t i = 0; i < me_guard_len(pl); i++) {
            w[8] += me_guard_ptr(pl)[i] != ME_GUARD_FILL;
        }
    }
    if (api == ME_API_IO) {
        w[5] = pos;
        w[7] = (!is_dst && (int32_t)rc > (int32_t)n) ? rc - n : 0u;
    } else if (!is_dst) {
        /* The bytes outside the edge are the high ones of a tail placement, the low ones
           (the first `delta`) of a head placement. */
        const uint32_t outside_from = pl->kind == ME_PLACE_RAM_HEAD ? 0u : n;
        const uint32_t outside_to = pl->kind == ME_PLACE_RAM_HEAD ? delta : len;
        for (uint32_t i = 0; i < len; i++) {
            const int moved = s_me_sink[i] != ME_SINK_FILL;
            w[5] += moved;
            w[7] += moved && i >= outside_from && i < outside_to;
        }
    }
    w[0] = rc;
    w[1] = 1u;
    w[2] = n;
    w[3] = len;
    w[4] = changed;
    w[6] = first;
    w[9] = pl->setup_mask;
    w[11] = (uint32_t)(uintptr_t)ptr;
    *refused = (int32_t)rc < 0;
    hle_record(id, "PASS", rc, w, ME_WORDS);
}

/* Every cell of one placement, reading side first: three APIs by four deltas, then the
   writing side: three APIs by three deltas. Every cell is a record, run or SKIP. */
static void me_run_place(unsigned place, me_place *pl, const me_bounds *b)
{
    const int usable = (pl->setup_mask & ME_SETUP_OK) == ME_SETUP_OK;
    const int owned = (pl->setup_mask & ME_SETUP_OWNED) != 0;
    const int ram_above = place != ME_PLACE_RAM_TAIL || b->ram_hi + ME_PAGE <= ME_RAM_TOP_64M;
    char id[48];
    uint8_t unused;
    for (unsigned is_dst = 0; is_dst < 2u; is_dst++) {
        const unsigned count = is_dst ? 3u : 4u;
        for (unsigned api = 0; api < 3u; api++) {
            for (unsigned d = 0; d < count; d++) {
                const uint32_t delta = is_dst ? k_me_dst_deltas[d] : k_me_src_deltas[d];
                snprintf(id, sizeof(id), "me-%s-%s-%s-%04x", k_me_places[place],
                         k_me_apis[api], is_dst ? "dst" : "src", (unsigned int)delta);
                if (!usable) {
                    me_skip(id, pl->setup_mask, ME_SKIP_PLACEMENT);
                } else if (is_dst && !owned) {
                    me_skip(id, pl->setup_mask, ME_SKIP_NOT_OWNED);
                } else if (delta > 0u && !ram_above) {
                    me_skip(id, pl->setup_mask, ME_SKIP_NO_RAM);
                } else if (is_dst && delta > 0u && !s_me_refused[place][api][d]) {
                    me_skip(id, pl->setup_mask, ME_SKIP_UNPROVEN);
                } else if (api == ME_API_IO && !s_me_data_ok) {
                    me_skip(id, pl->setup_mask, ME_SKIP_NO_FILE);
                } else {
                    me_run_cell(id, pl, api, (int)is_dst, delta,
                                is_dst ? &unused : &s_me_refused[place][api][d]);
                }
            }
        }
    }
}

static int hle_run(void)
{
    me_bounds b;
    me_place pl;
    memset(&b, 0, sizeof(b));
    me_measure_ram(&b);
    me_emit_ram(&b);
    me_make_data();

    me_place_ram_tail(&pl, &b, ME_N);
    me_emit_place("ramtail", &pl);
    me_run_place(ME_PLACE_RAM_TAIL, &pl, &b);
    me_release(&pl);

    me_place_ram_head(&pl, &b, ME_N);
    me_emit_place("ramhead", &pl);
    me_run_place(ME_PLACE_RAM_HEAD, &pl, &b);
    me_release(&pl);

    me_emit_vram(&b);
    me_place_vram_tail(&pl, &b, ME_N);
    me_emit_place("vramtail", &pl);
    me_run_place(ME_PLACE_VRAM_TAIL, &pl, &b);
    me_release(&pl);

    /* Not run, with the reason in word 10. The first launch's file call from the scratchpad
       (a whole-scratchpad sceIoWrite from 0x00010000) wrote its step and never returned, and
       the console was recovered by the watchdog: the outcome is "did not return normally",
       and repeating it would cost another recovery. */
    me_skip("me-cpu-store-edge", b.mask, ME_SKIP_FAULT);
    me_skip("me-cpu-load-edge", b.mask, ME_SKIP_FAULT);
    me_skip("me-scratch-io", b.mask, ME_SKIP_FAULT);
    me_skip("me-scratch-cross", b.mask, ME_SKIP_UNBACKED);
    return 1;
}

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
