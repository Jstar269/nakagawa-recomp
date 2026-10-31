// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Cache maintenance and DMA: sceKernelDcache, sceKernelIcache and sceDmac. Extracted from hle.c
 * on 2026-10-31 (#307). */

#include "hle_internal.h"

/* TD-27 opt-in stale translated-code detector (see src/rt/stale_code.h).
 *
 * Guest/host memory is one coherent array, so cache maintenance has no host
 * work and these stay success no-ops -- with one addition: when
 * SR_STALE_DETECT opts in, an invalidate also hash-compares the affected
 * guest bytes against the translation-time records codegen emitted behind
 * --stale-detect, and aborts loudly on the first mismatch (the sr_break
 * SR_BREAK_FATAL / sr_unimplemented fail-loud convention). Disabled, each
 * handler costs one cached flag branch and returns 0 exactly like h_ok. */
static int stale_mem_read(uint32_t addr, uint32_t *word_out, void *ctx) {
    (void)ctx;
    if (!sr_inrange_n(addr, 4u)) {
        return 0;
    }
    *word_out = sr_r32(addr);
    return 1;
}

static uint32_t h_cache_invalidate_all_common(CpuState *s) {
    (void)s;
    if (sr_stale_enabled()) {
        uint32_t bad = 0u, exp = 0u, act = 0u;
        if (sr_stale_check_all(stale_mem_read, NULL, &bad, &exp, &act)) {
            fflush(stderr);
            sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, s->pc, bad, 0u);
            abort();
        }
    }
    return 0;
}

/* Data-cache maintenance is where the guest makes RAM it wrote with the CPU the bytes the
 * GE reads next: the GE reads memory, not the CPU's data cache. Guest stores to RAM carry
 * no per-store renderer notification (only VRAM stores do, through the store helpers), so
 * a renderer that caches textures decoded from RAM learns of CPU-written texels here. A
 * title that draws glyphs into a RAM glyph-cache texture with its own libfont and then
 * writes the cache back depends on it; without it the renderer kept a texture whose
 * sampled content hash missed the new glyphs, and those glyphs were never drawn.
 * PPSSPP's sceKernelDcache* handlers invalidate its texture cache the same way (behaviour
 * reference only). A VRAM range is left alone: its stores were already reported one by
 * one, and a second post-write notice would merge stale CPU bytes over newer GPU pixels
 * of a render target. Return values are unchanged (unmeasured on PSP). */
#define SR_DCACHE_RAM_BYTES 0x04000000u   /* main RAM, 0x08000000-0x0bffffff */

static void dcache_range_notify_gpu(uint32_t addr, uint32_t size) {
    if (addr == 0u || size == 0u || size > 0x7fffffffu || size - 1u > 0xffffffffu - addr)
        return;
    if (sr_vram_cpu_span_touches(addr, size)) return;
    sr_gpu_vram_dirty(addr, size);
}

static void dcache_all_notify_gpu(void) {
    sr_gpu_vram_dirty(SR_RAM_BASE, SR_DCACHE_RAM_BYTES);
}

/* Keep the HLE ownership boundary aligned with the hardware cells. These
 * wrappers currently share the same coherent-memory implementation; the
 * writeback-invalidate and I-cache NIDs remain unreviewed until their own
 * cells are measured. */
static uint32_t h_DcacheWritebackAll(CpuState *s) {
    dcache_all_notify_gpu();
    return h_cache_invalidate_all_common(s);
}

static uint32_t h_DcacheWritebackInvalidateAll(CpuState *s) {
    dcache_all_notify_gpu();
    return h_cache_invalidate_all_common(s);
}

/* Instruction-cache maintenance is where the guest announces code it wrote itself. A code
 * overlay it read into memory is bound to its translation here, and one whose bytes are gone
 * is unbound (sr_module_code_sync_overlays), so the native tier runs the code that is in
 * memory, and a call into code no translation covers still misses and stops by name. Under
 * SR_STALE_DETECT the translated images are checked as well, as at data-cache maintenance. */
static uint32_t h_CacheInvalidateAllOther(CpuState *s) {
    (void)sr_module_code_sync_overlays(0u, 0xffffffffu);
    return h_cache_invalidate_all_common(s);
}

static uint32_t h_cache_invalidate_range_common(CpuState *s) {
    /* PSP range signature: a0 = address, a1 = length. */
    if (sr_stale_enabled()) {
        uint32_t bad = 0u, exp = 0u, act = 0u;
        if (sr_stale_check_range(A0, A1, stale_mem_read, NULL, &bad, &exp, &act)) {
            fflush(stderr);
            sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, s->pc, bad, A0);
            abort();
        }
    }
    return 0;
}

static uint32_t h_DcacheRangeMaintenance(CpuState *s) {
    dcache_range_notify_gpu(A0, A1);
    return h_cache_invalidate_range_common(s);
}

/* Unmeasured on PSP, so kept apart from the measured range handler above. */
static uint32_t h_DcacheWritebackInvalidateRange(CpuState *s) {
    dcache_range_notify_gpu(A0, A1);
    return h_cache_invalidate_range_common(s);
}

static uint32_t h_CacheInvalidateRangeOther(CpuState *s) {
    /* a0 = address, a1 = length; a range running past the top is clipped there. */
    uint32_t hi = A1 > 0xffffffffu - A0 ? 0xffffffffu : A0 + A1;
    (void)sr_module_code_sync_overlays(A0, hi);
    return h_cache_invalidate_range_common(s);
}

void hle_register_icache_handlers(void) {
    sr_hle_register(0x920f104au, "sceKernelIcacheInvalidateAll", h_CacheInvalidateAllOther);
    sr_hle_register(0xd8779ac6u, "sceKernelIcacheClearAll", h_CacheInvalidateAllOther);
    sr_hle_register(0xc2df770eu, "sceKernelIcacheInvalidateRange", h_CacheInvalidateRangeOther);
}

/* cache / misc UtilsForUser: no host cache work, but Dcache maintenance tells the
 * renderer which RAM the guest rewrote (dcache_range_notify_gpu above). Both the
 * production registry and the executable harness register through this helper. */
void hle_register_dcache_handlers(void) {
    sr_hle_register(0x79d1c3fa, "sceKernelDcacheWritebackAll", h_DcacheWritebackAll);
    /* Guest and host share one coherent byte array; beyond the renderer notice, cache
     * maintenance has no host-side work, but the syscall and success result are real. */
    sr_hle_register(0xb435dec5, "sceKernelDcacheWritebackInvalidateAll", h_DcacheWritebackInvalidateAll);
    sr_hle_register(0x3ee30821, "sceKernelDcacheWritebackRange", h_DcacheRangeMaintenance);
    /* Range invalidates are routine data-cache maintenance around DMA (the guest
     * libpsmfplayer invalidates its read buffer before sceDmacMemcpy), so they are
     * real successes on this coherent memory like the writebacks above. */
    sr_hle_register(0xbfa98062u, "sceKernelDcacheInvalidateRange", h_DcacheRangeMaintenance);
    sr_hle_register(0x34b9fa9eu, "sceKernelDcacheWritebackInvalidateRange", h_DcacheWritebackInvalidateRange);
}

void sr_stale_note_cache_op(uint32_t addr) {
    /* `cache`-op hook emitted by codegen behind --stale-detect. Single-word
     * invalidate; same gate and same abort-on-stale contract as above. */
    if (!sr_stale_enabled()) {
        return;
    }
    {
        uint32_t bad = 0u, exp = 0u, act = 0u;
        if (sr_stale_check_range(addr, 4u, stale_mem_read, NULL, &bad, &exp, &act)) {
            fflush(stderr);
            sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, 0u, bad, addr);
            abort();
        }
    }
}
/* ---- sceDmacMemcpy / sceDmacTryMemcpy ---------------------------------------
 *
 * Evidence/implementation boundary for the PSP-3000 / 6.61-ARK route:
 *
 *   HARDWARE_MEASURED:
 *   - a zero request returns 0x80000104 (illegal size);
 *   - a NULL pointer returns 0x80000103 before guest/GPU side effects, and
 *     both checks come before BUSY and the wait: a pending transfer does not
 *     change either result, and a blocking call returns them at once
 *     (PSP-DMAC-002 b2, 8/8 per cell);
 *   - fully valid RAM/VRAM spans copy their complete requested sizes through
 *     0x100000 bytes in the sequential size matrix;
 *   - a one-byte tail past an allocator-proven valid prefix returns success
 *     after copying exactly that prefix, for both APIs and both directions;
 *   - same-pointer and forward/backward overlapping copies are memmove-correct;
 *   - both APIs block their caller for the whole transfer; a Try made while
 *     another thread's transfer is pending returns BUSY (0x80000021) and a
 *     blocking call made then waits for it and returns 0 (PSP-DMAC-001);
 *   - PSP-DMAC-002 (three acceptance-eligible runs, boots 61-63, 2026-10-29):
 *     - when source and destination share their address modulo 16, the bytes
 *       before the first 16-byte destination boundary (head) and the bytes after
 *       the last whole 16-byte block (tail) move apart from the body; a body of
 *       at most 0x100 bytes does not use the channel at all: Try returns 0 and a
 *       blocking call does not wait, even while another transfer is pending
 *       (aligned 0x10..0x10f all pass, 0x110 and larger refused);
 *     - every other transfer uses the channel; a Try refused with BUSY still
 *       leaves its head and tail bytes copied (and nothing else), and one whose
 *       source and destination differ modulo 16 changes nothing (c4 cells);
 *     - a channel transfer keeps its caller blocked for its duration, measured
 *       per source/destination memory (main RAM or VRAM) and size from 0x200 to
 *       0x100000 bytes (k_dmac_us_x100 below, the mean per call of 16
 *       back-to-back calls over the runs on boots 61-63), and longer when source
 *       and destination differ modulo 16: per byte +0.003895 us at 4-byte,
 *       +0.015319 us at 2-byte and +0.038116 us at 1-byte relative alignment,
 *       none at 8-byte (main RAM to main RAM, 0x10000 bytes);
 *     - callers queued behind a pending transfer are served in arrival order,
 *       whatever their priorities (b3, 16/16 in both orders).
 *
 * RUNTIME_IMPLEMENTED:
 *   - fully valid spans are copied at their requested size (there is no
 *     API-wide 0xC000 ceiling);
 *   - the measured one-byte arena-end tail is copied only through its valid
 *     prefix, while larger or ambiguous overruns remain fail-closed;
 *   - both registered NIDs share one copy path;
 *   - the head/body/tail split and the 0x100-byte channel bypass above;
 *   - a channel transfer opens the scheduler's channel window for the
 *     measured duration (dmac_duration_us) and its caller stays blocked until
 *     the window ends; another thread's Try then returns BUSY after copying
 *     the head and tail, and its blocking call queues (arrival order), then
 *     performs its own transfer.
 *
 * UNMEASURED (explicit runtime choices, not PSP facts):
 *   - sizes from 0x110 to 0x1ff take the 0x200-0x400 segment of the timing
 *     table extended downwards, and sizes above 0x100000 the 0x80000-0x100000
 *     segment extended upwards;
 *   - the relative-alignment surcharges were measured main RAM to main RAM
 *     and are applied to every memory pair; a destination that is not 16-byte
 *     aligned while the source shares its phase measured 3-6 % slower, which
 *     is not modelled;
 *   - the bypass path, a transfer slowed by a concurrent caller (a polling
 *     Try stretched a 1 MiB VRAM copy from about 4.5 to 5.3 ms) and the
 *     syscall overhead carry no guest time;
 *   - a body of at most 0x100 bytes whose source and destination differ
 *     modulo 16 uses the channel (only 0x108 bytes was measured, refused);
 *   - callers that cannot block (interrupt context, suspended interrupts or
 *     dispatch, no guest thread) keep the synchronous copy and open no window;
 *   - span validation runs before BUSY and the wait.
 *
 * The size-before-address ordering below is a runtime ordering; the combined
 * size-zero-plus-invalid-pointer case was not part of the probe.
 * Guest RAM/VRAM share the runtime's unified host allocation, and this target
 * does not currently translate guest self-modifying code or maintain a separate
 * instruction-cache/dispatch-table invalidation layer. DMA therefore exposes
 * no additional code-invalidation side effect here; that boundary belongs to a
 * future dynamic-code correctness issue rather than this copy contract.
 */
#define SCE_DMAC_ERROR_ILLEGAL_ADDR 0x80000103u
#define SCE_DMAC_ERROR_ILLEGAL_SIZE 0x80000104u
#define SCE_DMAC_ERROR_BUSY 0x80000021u
/* The channel's owner and every blocking caller queued behind it park on this
 * object until the scheduler ends the window or hands them the channel. */
#define DMAC_TRANSFER_WAIT_OBJ 0x444d4143u /* "DMAC" */
/* The largest same-phase body the measured bypass covers (PSP-DMAC-002). */
#define DMAC_BYPASS_BODY_MAX 0x100u

/* PSP-DMAC-002 timing table: hundredths of a microsecond per call for a channel
 * transfer with source and destination aligned alike, by pair (main RAM to main RAM,
 * main RAM to VRAM, VRAM to main RAM, VRAM to VRAM) and size. */
#define DMAC_TIMING_POINTS 12
static const uint32_t k_dmac_sizes[DMAC_TIMING_POINTS] = {
    0x200u, 0x400u, 0x800u, 0x1000u, 0x2000u, 0x4000u,
    0x8000u, 0x10000u, 0x20000u, 0x40000u, 0x80000u, 0x100000u};
static const uint32_t k_dmac_us_x100[4][DMAC_TIMING_POINTS] = {
    {5491u, 5781u, 7053u, 8971u, 11878u, 18556u, 31691u, 58304u, 112766u, 222508u, 437641u, 870594u},
    {5372u, 5746u, 6506u, 7596u, 9772u, 13027u, 19875u, 34806u, 65497u, 126729u, 247419u, 493398u},
    {5444u, 5510u, 5953u, 6785u, 8756u, 10702u, 17841u, 26683u, 48875u, 93408u, 180838u, 361456u},
    {5259u, 5271u, 5638u, 6698u, 7934u, 10819u, 15897u, 26842u, 56997u, 118627u, 226912u, 447733u},
};
/* Per-byte surcharge, in millionths of a microsecond, by relative alignment class:
 * index 0 = 8- or 16-byte, 1 = 4-byte, 2 = 2-byte, 3 = 1-byte. */
static const uint32_t k_dmac_misalign_ppm[4] = {0u, 3895u, 15319u, 38116u};

static int dmac_is_vram(uint32_t addr) {
    return sr_vram_cpu_aperture(addr);
}

/* Interpolated duration, in hundredths of a microsecond, of one table row at size n. */
static uint64_t dmac_row_x100(const uint32_t *row, uint32_t n) {
    int hi = 1;
    while (hi < DMAC_TIMING_POINTS - 1 && k_dmac_sizes[hi] < n) hi++;
    const int lo = hi - 1;
    const int64_t x0 = k_dmac_sizes[lo], x1 = k_dmac_sizes[hi];
    const int64_t y0 = row[lo], y1 = row[hi];
    int64_t y = y0 + ((int64_t)n - x0) * (y1 - y0) / (x1 - x0);
    return y > 0 ? (uint64_t)y : 0u;
}

/* Guest microseconds a channel transfer of n bytes keeps its caller blocked. */
static uint32_t dmac_duration_us(uint32_t dst, uint32_t src, uint32_t n) {
    const int pair = (dmac_is_vram(src) ? 2 : 0) + (dmac_is_vram(dst) ? 1 : 0);
    uint64_t x100 = dmac_row_x100(k_dmac_us_x100[pair], n);
    const uint32_t rel = (src - dst) & 15u;
    const int cls = (rel & 7u) == 0u ? 0 : (rel & 3u) == 0u ? 1 : (rel & 1u) == 0u ? 2 : 3;
    x100 += (uint64_t)n * k_dmac_misalign_ppm[cls] / 10000u;
    const uint64_t us = (x100 + 50u) / 100u;
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static uint32_t dmac_transfer(CpuState *s, int try_only);

static uint32_t h_DmacMemcpy(CpuState *s) {
    return dmac_transfer(s, 0);
}

static uint32_t h_DmacTryMemcpy(CpuState *s) {
    return dmac_transfer(s, 1);
}

/* Only a guest thread on its own coroutine, with interrupts and dispatch
 * enabled, can genuinely block; any other caller keeps the synchronous copy,
 * so no caller that copies today starts waiting. */
static int dmac_caller_can_wait(void) {
    return sched_guest_thread_running() && sched_wait_permitted();
}

static void dmac_copy(uint32_t dst, uint32_t src, uint32_t n) {
    if (n == 0u) return;
    sr_guest_vram_write_prepare_bytes(dst, n);
    memmove(SR_HOST(dst), SR_HOST(src), n);
    sr_guest_vram_write_commit_bytes(dst, n);
    if (g_sr_heap_watch) sr_heap_note_bulk_write(dst, n, 0u);
}

static uint32_t dmac_transfer(CpuState *s, int try_only) {
    /* a0=dst, a1=src, a2=size. A real DMA copy in guest memory. */
    uint32_t dst = A0, src = A1, n = A2;

    /* Validate size and null pointers before touching guest memory; both come
     * before BUSY and the wait (measured). */
    if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;
    if (dst == 0u || src == 0u) return SCE_DMAC_ERROR_ILLEGAL_ADDR;

    /* The physical PSP completed the one-byte invalid-tail case through the
     * valid prefix.  Compute both prefixes with overflow-safe arena arithmetic
     * and only admit that measured one-byte shape.  Larger tails, wrapped
     * requests, and zero-length prefixes fail atomically until a broader
     * hardware control establishes their semantics. */
    const uint32_t src_prefix = sr_guest_span_prefix(src, n);
    const uint32_t dst_prefix = sr_guest_span_prefix(dst, n);
    uint32_t effective = src_prefix < dst_prefix ? src_prefix : dst_prefix;
    if (effective == 0u ||
        (src_prefix != n && n - src_prefix != 1u) ||
        (dst_prefix != n && n - dst_prefix != 1u)) {
        if (dst_prefix != n) sr_oor(dst, 0u, 1);
        if (src_prefix != n) sr_oor(src, 0u, 0);
        return SCE_DMAC_ERROR_ILLEGAL_ADDR;
    }
    if (!sr_guest_span_readable_current(src, effective) ||
        !sr_guest_span_writable(dst, effective)) {
        if (!sr_guest_span_writable(dst, effective)) sr_oor(dst, 0u, 1);
        if (!sr_guest_span_readable(src, effective)) sr_oor(src, 0u, 0);
        return SCE_DMAC_ERROR_ILLEGAL_ADDR;
    }

    /* Same phase modulo 16: a head up to the first 16-byte destination boundary, a
     * body of whole 16-byte blocks and a tail move apart; otherwise the whole span is
     * the body. */
    const int same_phase = ((src ^ dst) & 15u) == 0u;
    uint32_t head = 0u, tail = 0u;
    if (same_phase) {
        head = (0u - dst) & 15u;
        if (head > effective) head = effective;
        tail = (effective - head) & 15u;
    }
    const uint32_t body = effective - head - tail;

    /* A short same-phase body bypasses the channel: no BUSY, no wait, no window.
     * memmove, not memcpy: hardware showed both overlap directions landing
     * correctly, and dst == src must leave the buffer intact. */
    if (same_phase && body <= DMAC_BYPASS_BODY_MAX) {
        dmac_copy(dst, src, effective);
        return 0;
    }

    /* Another thread holds the channel: Try copies the head and tail and reports BUSY;
     * the blocking form queues behind it through the scheduler (arrival order). */
    const int can_wait = dmac_caller_can_wait();
    uint32_t holder = 0u;
    if (sched_transfer_pending(DMAC_TRANSFER_WAIT_OBJ, &holder) &&
        !sched_transfer_granted(DMAC_TRANSFER_WAIT_OBJ)) {
        if (try_only) {
            dmac_copy(dst, src, head);
            dmac_copy(dst + effective - tail, src + effective - tail, tail);
            return SCE_DMAC_ERROR_BUSY;
        }
        if (can_wait && sched_transfer_enqueue(DMAC_TRANSFER_WAIT_OBJ)) {
            while (!sched_transfer_granted(DMAC_TRANSFER_WAIT_OBJ))
                sched_block_on(DMAC_TRANSFER_WAIT_OBJ);
        }
    }

    dmac_copy(dst, src, effective);

    /* This call's own transfer: the caller stays blocked in the call until the
     * scheduler ends its window, after the measured duration. */
    if (can_wait &&
        sched_transfer_begin(DMAC_TRANSFER_WAIT_OBJ, dmac_duration_us(dst, src, body))) {
        const uint32_t self = sched_current_uid();
        uint32_t owner = 0u;
        while (sched_transfer_pending(DMAC_TRANSFER_WAIT_OBJ, &owner) && owner == self &&
               !sched_transfer_granted(DMAC_TRANSFER_WAIT_OBJ))
            sched_block_on(DMAC_TRANSFER_WAIT_OBJ);
    }
    return 0;
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_bulk_memory_handlers(). */
void hle_register_bulk_memory_cache_dma(void) {
    sr_hle_register(0x617f3fe6, "sceDmacMemcpy", h_DmacMemcpy);
    /* Both DMAC copy NIDs register here rather than in the general table below,
     * so the executable regression enters the same registration the game does
     * instead of a test-only mapping. */
    sr_hle_register(0xd97f94d8, "sceDmacTryMemcpy", h_DmacTryMemcpy);
}
