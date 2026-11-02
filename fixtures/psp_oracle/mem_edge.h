// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Memory-edge fixture for the PSP-3000 oracle (#303, #1380): places a test buffer so that its
// last (or first) N bytes sit exactly at the end (or start) of valid memory, so that a call
// given a longer length runs past the edge, and reports in a setup mask whether the placement
// succeeded. A header of static functions so any probe source can include it; it issues no
// output and no file calls of its own, only the partition-memory calls the probes already use
// (sceKernelAllocPartitionMemory, sceKernelGetBlockHeadAddr, sceKernelFreePartitionMemory,
// sceKernelTotalFreeMemSize, sceKernelMaxFreeMemSize), and runs in user mode.
//
// Three places:
//   ME_PLACE_RAM_TAIL   the last N bytes of user RAM: a High page of partition 2 whose end is
//                       the measured top of the partition.
//   ME_PLACE_RAM_HEAD   the first N bytes of user RAM: the page at the measured bottom of the
//                       partition when the allocator hands it out ("owned"); otherwise the
//                       memory there is in use by something else and the buffer is read-only
//                       for the probe (ME_SETUP_OWNED clear).
//   ME_PLACE_VRAM_TAIL  the last N bytes of VRAM (0x041FFFFF). Fixed hardware range, no
//                       allocation; the probe saves the N bytes and restores them.
//
// A call is given me_call_ptr() and a length of N + delta. For a tail placement the pointer is
// the buffer and the range runs delta bytes past the end; for a head placement the pointer is
// delta bytes below the start and the range runs up into the buffer. Which addresses are the
// edges is measured by me_measure_ram(), not assumed: the end of user RAM is the end of the
// highest page the allocator will give out when the page after it is classified like an
// address far outside the partition, and the bottom is the lowest candidate page the allocator
// classifies differently from the kernel page below it.
//
// Every placement checks, and records in the setup mask, that the page right outside the edge
// is refused by the allocator. A failed bit never aborts a probe: the cells that need it are
// emitted as SKIP with the setup mask, so an unmeasured edge is never reported as a result.

#ifndef NK_MEM_EDGE_H
#define NK_MEM_EDGE_H

#include <pspkernel.h>
#include <pspsysmem.h>
#include <stdint.h>
#include <string.h>

#define ME_PAGE 0x1000u
#define ME_PARTITION_USER 2
/* Candidate edge addresses: documented PSP memory map values the measurement classifies. */
#define ME_KERNEL_PAGE_BELOW_USER 0x087FF000u /* a kernel-partition page, below user RAM */
#define ME_USER_BASE_CANDIDATE 0x08800000u
#define ME_RAM_TOP_32M 0x0A000000u
#define ME_RAM_TOP_64M 0x0C000000u /* end of a 64 MiB console's RAM */
#define ME_SCRATCH_LO 0x00010000u
#define ME_SCRATCH_HI 0x00014000u
#define ME_VRAM_LO 0x04000000u
#define ME_VRAM_HI 0x04200000u

/* me_bounds.mask */
#define ME_B_RAM_HI 0x01u  /* top of user RAM measured */
#define ME_B_RAM_LO 0x02u  /* bottom of user RAM measured */
#define ME_B_SCRATCH 0x04u /* scratchpad range accepted by the read-side oracle */
#define ME_B_VRAM 0x08u    /* VRAM range accepted by the read-side oracle */

typedef struct {
    uint32_t ram_lo, ram_hi; /* 0 when not measured; ram_hi is one past the last byte */
    uint32_t scratch_lo, scratch_hi;
    uint32_t vram_lo, vram_hi;
    uint32_t mask;
    /* Raw allocator answers behind ram_lo / ram_hi (0 = the page was handed out). */
    uint32_t hi_head, hi_rc_end, hi_rc_next, hi_rc_far;
    uint32_t lo_head, lo_rc_kernel, lo_rc_base, lo_rc_next;
    uint32_t free_total, free_max;
} me_bounds;

/* me_place.setup_mask */
#define ME_SETUP_BLOCK 0x01u   /* the block (or fixed range) is available */
#define ME_SETUP_GEOMETRY 0x02u /* page-aligned block holding the N bytes */
#define ME_SETUP_AT_EDGE 0x04u /* the buffer's last/first byte is the measured edge */
#define ME_SETUP_OUTSIDE 0x08u /* the page just outside the edge is refused by the allocator */
#define ME_SETUP_OWNED 0x10u   /* the probe owns the N bytes and may write them */
#define ME_SETUP_OK (ME_SETUP_BLOCK | ME_SETUP_GEOMETRY | ME_SETUP_AT_EDGE)

#define ME_PLACE_RAM_TAIL 0
#define ME_PLACE_RAM_HEAD 1
#define ME_PLACE_VRAM_TAIL 2

typedef struct {
    int kind;
    uint8_t *buf;        /* the N bytes at the edge */
    uint32_t n;
    uint8_t *block;      /* owned block holding buf (NULL for VRAM) */
    uint32_t block_size;
    SceUID uid;          /* allocator block id, -1 when none */
    uint32_t setup_mask;
    uint32_t outside_rc; /* allocator answer for the page just outside the edge (0 = handed out) */
    uint8_t saved[256];  /* VRAM: the bytes restored by me_release */
} me_place;

/* An Addr allocation of one page at `address`, freed at once. Returns 0 when the allocator
   handed it out, otherwise the allocator's own error code. */
static uint32_t me_try_page(uint32_t address)
{
    const SceUID uid = sceKernelAllocPartitionMemory(
        ME_PARTITION_USER, "nkme-try", PSP_SMEM_Addr, ME_PAGE, (void *)(uintptr_t)address);
    if (uid < 0) {
        return (uint32_t)uid;
    }
    sceKernelFreePartitionMemory(uid);
    return 0u;
}

/* Measure the two ends of user RAM with the allocator. The top: a High page of partition 2;
   its end is the top when the allocator answers for the page after it exactly as it answers
   for an address far outside the partition (ME_RAM_TOP_64M). The bottom: the base candidate
   is the bottom when the allocator answers for it differently from the kernel page below
   it. Raw answers are kept in the struct so the classification can be re-judged offline. */
static void me_measure_ram(me_bounds *b)
{
    b->scratch_lo = ME_SCRATCH_LO;
    b->scratch_hi = ME_SCRATCH_HI;
    b->vram_lo = ME_VRAM_LO;
    b->vram_hi = ME_VRAM_HI;
    b->free_total = (uint32_t)sceKernelTotalFreeMemSize();
    b->free_max = (uint32_t)sceKernelMaxFreeMemSize();

    const SceUID hi = sceKernelAllocPartitionMemory(
        ME_PARTITION_USER, "nkme-hi", PSP_SMEM_High, ME_PAGE, NULL);
    if (hi >= 0) {
        const uint32_t head = (uint32_t)(uintptr_t)sceKernelGetBlockHeadAddr(hi);
        const uint32_t end = head + ME_PAGE;
        b->hi_head = head;
        sceKernelFreePartitionMemory(hi);
        b->hi_rc_end = me_try_page(end);
        b->hi_rc_next = me_try_page(end + ME_PAGE);
        b->hi_rc_far = me_try_page(ME_RAM_TOP_64M);
        if ((head & (ME_PAGE - 1u)) == 0u && head != 0u && end > head &&
            b->hi_rc_end != 0u && b->hi_rc_end == b->hi_rc_far) {
            b->ram_hi = end;
            b->mask |= ME_B_RAM_HI;
        }
    }

    const SceUID lo = sceKernelAllocPartitionMemory(
        ME_PARTITION_USER, "nkme-lo", PSP_SMEM_Low, ME_PAGE, NULL);
    if (lo >= 0) {
        b->lo_head = (uint32_t)(uintptr_t)sceKernelGetBlockHeadAddr(lo);
        sceKernelFreePartitionMemory(lo);
    }
    b->lo_rc_kernel = me_try_page(ME_KERNEL_PAGE_BELOW_USER);
    b->lo_rc_base = me_try_page(ME_USER_BASE_CANDIDATE);
    b->lo_rc_next = me_try_page(ME_USER_BASE_CANDIDATE + ME_PAGE);
    if (b->lo_rc_base != b->lo_rc_kernel && b->lo_rc_base != b->hi_rc_far) {
        b->ram_lo = ME_USER_BASE_CANDIDATE;
        b->mask |= ME_B_RAM_LO;
    }
}

/* Place N bytes at the end of user RAM. N must be a non-zero multiple of 64 below one page. */
static void me_place_ram_tail(me_place *p, const me_bounds *b, uint32_t n)
{
    memset(p, 0, sizeof(*p));
    p->kind = ME_PLACE_RAM_TAIL;
    p->n = n;
    p->uid = -1;
    p->block_size = ME_PAGE;
    if (n == 0u || n >= ME_PAGE || (n & 63u) != 0u || !(b->mask & ME_B_RAM_HI)) {
        return;
    }
    p->uid = sceKernelAllocPartitionMemory(
        ME_PARTITION_USER, "nkme-tail", PSP_SMEM_High, ME_PAGE, NULL);
    if (p->uid < 0) {
        p->uid = -1;
        return;
    }
    p->setup_mask |= ME_SETUP_BLOCK;
    p->block = (uint8_t *)sceKernelGetBlockHeadAddr(p->uid);
    if (((uintptr_t)p->block & (ME_PAGE - 1u)) == 0u) {
        p->setup_mask |= ME_SETUP_GEOMETRY;
    }
    p->buf = p->block + ME_PAGE - n;
    if ((uint32_t)(uintptr_t)p->block + ME_PAGE == b->ram_hi) {
        p->setup_mask |= ME_SETUP_AT_EDGE | ME_SETUP_OWNED;
    }
    p->outside_rc = me_try_page(b->ram_hi);
    if (p->outside_rc != 0u) {
        p->setup_mask |= ME_SETUP_OUTSIDE;
    }
}

/* Place N bytes at the bottom of user RAM. The allocator may refuse the page (something else
   uses it); the buffer is then still the first N bytes of user RAM but the probe does not own
   them, so ME_SETUP_OWNED stays clear and only read-side cells may use it. */
static void me_place_ram_head(me_place *p, const me_bounds *b, uint32_t n)
{
    memset(p, 0, sizeof(*p));
    p->kind = ME_PLACE_RAM_HEAD;
    p->n = n;
    p->uid = -1;
    p->block_size = ME_PAGE;
    if (n == 0u || n >= ME_PAGE || (n & 63u) != 0u || !(b->mask & ME_B_RAM_LO)) {
        return;
    }
    p->uid = sceKernelAllocPartitionMemory(
        ME_PARTITION_USER, "nkme-head", PSP_SMEM_Addr, ME_PAGE, (void *)(uintptr_t)b->ram_lo);
    p->buf = (uint8_t *)(uintptr_t)b->ram_lo;
    p->setup_mask |= ME_SETUP_GEOMETRY | ME_SETUP_AT_EDGE;
    if (p->uid >= 0) {
        p->block = (uint8_t *)sceKernelGetBlockHeadAddr(p->uid);
        p->setup_mask |= ME_SETUP_BLOCK | ME_SETUP_OWNED;
        if (p->block != p->buf) {
            p->setup_mask &= ~(ME_SETUP_AT_EDGE | ME_SETUP_OWNED);
        }
    } else {
        p->uid = -1;
        /* In use by another module: valid memory, readable, not ours to write. */
        p->setup_mask |= ME_SETUP_BLOCK;
    }
    p->outside_rc = me_try_page(ME_KERNEL_PAGE_BELOW_USER);
    if (p->outside_rc != 0u) {
        p->setup_mask |= ME_SETUP_OUTSIDE;
    }
}

/* Place N bytes at the end of VRAM (N <= 256, a multiple of 64). The caller must have seen
   ME_B_VRAM. The bytes are saved and written back by me_release. */
static void me_place_vram_tail(me_place *p, const me_bounds *b, uint32_t n)
{
    memset(p, 0, sizeof(*p));
    p->kind = ME_PLACE_VRAM_TAIL;
    p->n = n;
    p->uid = -1;
    if (n == 0u || n > sizeof(p->saved) || (n & 63u) != 0u || !(b->mask & ME_B_VRAM)) {
        return;
    }
    p->buf = (uint8_t *)(uintptr_t)(b->vram_hi - n);
    memcpy(p->saved, p->buf, n);
    p->setup_mask = ME_SETUP_BLOCK | ME_SETUP_GEOMETRY | ME_SETUP_AT_EDGE | ME_SETUP_OWNED;
}

/* The pointer to give a call that overruns the edge by `delta` bytes (length n + delta). */
static uint8_t *me_call_ptr(const me_place *p, uint32_t delta)
{
    return p->kind == ME_PLACE_RAM_HEAD ? p->buf - delta : p->buf;
}

/* The bytes of the block outside the N-byte buffer (tail: the bytes below it; head: the bytes
   above it), which a call must not touch: a guard the fixture owns. NULL when not owned. */
static uint8_t *me_guard_ptr(const me_place *p)
{
    if (!(p->setup_mask & ME_SETUP_OWNED) || p->block == NULL) {
        return NULL;
    }
    return p->kind == ME_PLACE_RAM_HEAD ? p->buf + p->n : p->block;
}

static uint32_t me_guard_len(const me_place *p)
{
    return me_guard_ptr(p) == NULL ? 0u : ME_PAGE - p->n;
}

static void me_release(me_place *p)
{
    if (p->kind == ME_PLACE_VRAM_TAIL && p->buf != NULL) {
        memcpy(p->buf, p->saved, p->n);
        sceKernelDcacheWritebackInvalidateRange(p->buf, p->n);
    }
    if (p->uid >= 0) {
        sceKernelFreePartitionMemory(p->uid);
        p->uid = -1;
    }
}

#endif /* NK_MEM_EDGE_H */
