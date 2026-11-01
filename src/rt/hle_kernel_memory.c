// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Kernel memory: partitions, memory blocks, and fixed and variable pools (FPL, VPL, TLSPL).
 * Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

HleMemoryBlock s_memory_blocks[HLE_MAX_MEMORY_BLOCKS];

static uint32_t h_SetCompiledSdkVersion(CpuState *s) { return sr_sdkver_set(&g_sdk_version, A0); }
/* sceKernelDevkitVersion (0x3fc9ae6a) returns the devkit version word of the modelled firmware. The
 * value is the single SR_PSP_FIRMWARE_DEVKIT_VERSION in sdkver.h. PSP-3000 6.61 oracle 2026-10-15,
 * record PSP-SYSTEM-001 (case model-profile), measured 0x06060110. */
static uint32_t h_KernelDevkitVersion(CpuState *s) { (void)s; return SR_PSP_FIRMWARE_DEVKIT_VERSION; }
#define HLE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE 0x800200d8u /* SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE */
#define HLE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED 0x800200d9u /* SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED */
#define HLE_KERNEL_ERROR_ILLEGAL_PARTITION 0x800200d6u /* SCE_KERNEL_ERROR_ILLEGAL_PARTITION */
#define HLE_KERNEL_ERROR_ERROR 0x80020001u          /* SCE_KERNEL_ERROR_ERROR */
#define HLE_KERNEL_ERROR_ILLEGAL_ARGUMENT 0x800200d2u /* SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT */

/* SysMemUserForUser */
/* Partition 5 is the 4 MiB volatile region [0x08400000, 0x08800000), the memory
 * sceKernelVolatileMemLock hands out (#1383). It has its own allocator, measured on a PSP by
 * pspautotests tests/sysmem/volatile.expected at f573fe8c9122a8ff5db4f030d74557bed644b4e0:
 * Low (0) blocks start at 0x08400000 and High (1) blocks end at 0x08800000 (a 0x100-byte block
 * at 0x087fff00); sizes round up to 0x100 bytes and the whole 4 MiB can be allocated (0x3fff00
 * beside a 0x100 block, and no more); an Addr (2) block lies exactly at its address inside the
 * region (0x08500000), anything else is MEMBLOCK_ALLOC_FAILED, as is size 0 or 0xFFFFFFFF; the
 * aligned types (3 Low, 4 High) take the alignment in the address argument, which must be a
 * power of two (0, 3, 6 and 0xFFFFFFFF are ILLEGAL_ALIGNMENT_SIZE), and a High block aligned to
 * 0x200, 0x400 or 0x800 ends below the top on that boundary; any other type is
 * ILLEGAL_MEMBLOCKTYPE, which wins over a NULL name and a zero size. The allocations do not
 * depend on the volatile lock: pspautotests allocates partition 5 with the lock free and creates
 * pools there while holding it. */
#define VOLATILE_PART_ID   5u
#define VOLATILE_PART_BASE 0x08400000u
#define VOLATILE_PART_END  0x08800000u
#define HLE_KERNEL_ERROR_ILLEGAL_ALIGNMENT_SIZE 0x800200e4u /* SCE_KERNEL_ERROR_ILLEGAL_ALIGNMENT_SIZE */

/* 1 when [addr, addr + size) lies in the region and overlaps no live partition-5 block. */
static int volatile_part_range_free(uint64_t addr, uint64_t size) {
    if (addr < VOLATILE_PART_BASE || addr + size > VOLATILE_PART_END) return 0;
    for (int i = 0; i < s_nblocks; i++) {
        const Block *b = &s_blocks[i];
        if (b->uid && b->part5 && addr < (uint64_t)b->addr + b->slot_size && b->addr < addr + size)
            return 0;
    }
    return 1;
}

/* A partition-5 block of `size` bytes (type 0-4, `arg` the address or alignment): its UID, or
 * the measured refusal. A Low block's best start is the region's base or the end of a live
 * block, a High block's best end the region's top or the start of one, each moved to the
 * alignment; the lowest (or highest) of those that is free wins. */
static uint32_t volatile_part_alloc(uint32_t type, uint32_t size, uint32_t arg) {
    if (size == 0u || size > UINT32_MAX - 0xffu) return HLE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
    uint64_t slot_size = (size + 0xffu) & ~0xffu, align = 0x100u;
    if (type == 3u || type == 4u) {
        if (arg == 0u || (arg & (arg - 1u)) != 0u) return HLE_KERNEL_ERROR_ILLEGAL_ALIGNMENT_SIZE;
        if (arg > align) align = arg;
    }
    int slot = -1;
    for (int i = 0; i < s_nblocks; i++)
        if (s_blocks[i].uid == 0u) { slot = i; break; }
    if (slot < 0) {
        if (s_nblocks >= HLE_MAX_PARTITION_BLOCKS) return HLE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
        slot = s_nblocks;
    }
    int found = 0;
    uint64_t addr = 0u;
    if (type == 2u) {
        addr = arg;
        found = volatile_part_range_free(addr, slot_size);
    } else {
        int high = type == 1u || type == 4u;
        for (int i = -1; i < s_nblocks; i++) {
            const Block *b = i >= 0 ? &s_blocks[i] : NULL;
            if (b && (!b->uid || !b->part5)) continue;
            uint64_t candidate;
            if (high) {
                uint64_t end = b ? b->addr : VOLATILE_PART_END;
                if (end < VOLATILE_PART_BASE + slot_size) continue;
                candidate = (end - slot_size) & ~(align - 1u);
            } else {
                uint64_t start = b ? (uint64_t)b->addr + b->slot_size : VOLATILE_PART_BASE;
                candidate = (start + align - 1u) & ~(align - 1u);
            }
            if (!volatile_part_range_free(candidate, slot_size)) continue;
            if (!found || (high ? candidate > addr : candidate < addr)) addr = candidate;
            found = 1;
        }
    }
    if (!found) return HLE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
    if (slot == s_nblocks) s_nblocks++;
    uint32_t uid = sr_alloc_uid();
    s_blocks[slot] = (Block){ uid, (uint32_t)addr, size, (uint32_t)slot_size, 0xFFFFFFFFu, 0u, 0, 1 };
    return uid;
}

/* sceKernelAllocPartitionMemory(partition, name, type, size, addr): the 5th
 * argument arrives in t0 (r8) under MIPS EABI (see stack_arg), not at sp+16.
 * Type dispatch today:
 *   0 Low        -> alloc_block first-fit/bump (the PSP_SMEM_Low contract).
 *   1 High       -> NOT implemented top-down: falls through to the Low path
 *                   and allocates bottom-up exactly like type 0. This fake is
 *                   pre-existing behaviour, documented here, and unchanged by
 *                   the fixed-address work (no live reservation can sit above
 *                   s_heap, so leaving High as Low cannot hand out reserved
 *                   space; see sr_alloc_block_at).
 *   2 Addr       -> sr_alloc_block_at: reserve exactly [addr, addr+size), fail
 *                   with the 0xFFFFFFFFu sentinel when overlapped/out of range
 *                   (the exact firmware error for this case is UNMEASURED, so
 *                   the existing allocator sentinel is propagated exactly as
 *                   the current OOM path already does).
 *   3 LowAligned / 4 HighAligned -> fall through to the Low path, ignoring the
 *                   alignment argument (pre-existing behaviour, unchanged).
 * Partitions (pspautotests tests/sysmem/partition.expected at f573fe8c, recorded on a PSP): 2,
 * 5 and 6 allocate; 1, 3 and 4 answer ILLEGAL_PARTITION (0x800200D6) and every other number
 * ILLEGAL_ARGUMENT (0x800200D2), and a type past 4 ILLEGAL_MEMBLOCKTYPE. Partition 5 is the
 * volatile region (volatile_part_alloc), where a NULL name answers 0x80020001 as measured; 2 and 6
 * are the one user partition this runtime models (where a PSP places a partition-6 block is
 * UNMEASURED), which still takes a NULL name as no name although the PSP refuses it there too
 * (partition.expected "NULL name"; existing callers rely on it, #341). */
static uint32_t h_AllocPartitionMemory(CpuState *s) {
    /* a0=partition, a1=name, a2=type, a3=size, t0(r8)=addr. Returns a block UID. */
    char name[64];
    uint32_t partition = A0;
    if (partition == 1u || partition == 3u || partition == 4u) return HLE_KERNEL_ERROR_ILLEGAL_PARTITION;
    if (partition != 2u && partition != VOLATILE_PART_ID && partition != 6u)
        return HLE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
    if (A2 > 4u) return HLE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE;
    if (!A1 && partition == VOLATILE_PART_ID) return HLE_KERNEL_ERROR_ERROR;
    if (A1) {
        if (!guest_cstr(A1, name, sizeof(name)))
            return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    } else {
        name[0] = '\0';
    }
    uint32_t type = A2;
    uint32_t size = A3;
    if (partition == VOLATILE_PART_ID) return volatile_part_alloc(type, size, stack_arg(s, 0));
    uint32_t uid = (type == 2u)
        ? sr_alloc_block_at(stack_arg(s, 0), size, name)
        : alloc_block(size ? size : 16);
    fprintf(stderr, "  -> uid=0x%x addr=0x%08x (heap_bump_now=0x%08x)\n",
            uid, sr_last_alloc_addr, s_heap);
    /* A block that cannot be placed is MEMBLOCK_ALLOC_FAILED: an Addr block at 0x08400000,
     * outside the user partition, measured on a PSP-3000 6.61 by PSP-KERNEL-SEM-001
     * vmem-alloc-free and vmem-alloc-held (2026-10-31), as partition.expected records for
     * placements and sizes the partition cannot hold. */
    return uid == 0xFFFFFFFFu ? HLE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED : uid;
}
static uint32_t h_GetBlockHeadAddr(CpuState *s) { return block_addr(A0); }
static uint32_t h_FreePartitionMemory(CpuState *s) {
    return free_block(A0);
}

static HleMemoryBlock *memory_block_find(uint32_t uid) {
    if (uid == 0u) return NULL;
    for (int i = 0; i < HLE_MAX_MEMORY_BLOCKS; i++)
        if (s_memory_blocks[i].uid == uid) return &s_memory_blocks[i];
    return NULL;
}

/* sceKernelAllocMemoryBlock(name, type, size, SceKernelMemoryBlockAllocOption *opt): a block of
 * the main user partition. Measured on a PSP by pspautotests tests/sysmem/memblock.expected
 * (2ed7c9c5): type 0 and 1 allocate; -1, 2..5 and every larger type return 0x800200D8; size 0 and
 * 0xFFFFFFFF return 0x800200D9; an option block whose size word is anything but 4 (-1, 0..3, 5,
 * 0x11 and larger) returns 0x800200D2, and 4 allocates; a NULL name returns 0x80020001, while an
 * empty name and one longer than the 31-byte name field allocate. uOFW src/kd/sysmem/memblock.c
 * (MIT) gives the same order: the name and option pointers, then the option size, then the type,
 * then sceKernelAllocPartitionMemory(2, name, type, size, 0), whose type 0 is Low and 1 is High.
 * Type 0 is the partition's Low path; type 1 (High) comes from the top of the arena the thread
 * stacks share (alloc_block_high). Which code an unmapped name or option pointer returns is not
 * measured; the pointer check returns uOFW's 0x800200D3. */
static uint32_t h_AllocMemoryBlock(CpuState *s) {
    (void)s;
    uint32_t kind = A1;
    uint32_t size = A2;
    char name[64];
    if (A0 && !guest_cstr(A0, name, sizeof(name)))
        return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (A3 && !sr_guest_span_readable(A3, 4u))
        return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (A3 && MEM_R32(A3) != 4u) return HLE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
    if (kind >= 2u) return HLE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE;
    if (!A0) return HLE_KERNEL_ERROR_ERROR;
    if (size == 0u || size > UINT32_MAX - 0xffu)
        return HLE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
    if (kind == 1u) {
        if (!partition_high_modelled())
            return hle_unmodelled_stop("sceKernelAllocMemoryBlock",
                                       "type 1 in a partition based below the runtime stacks is not modelled");
        int high_slot = -1;
        for (int i = 0; i < HLE_MAX_MEMORY_BLOCKS; i++) {
            if (s_memory_blocks[i].uid == 0u) { high_slot = i; break; }
        }
        if (high_slot < 0) return HLE_KERNEL_ERROR_NO_MEMORY;
        uint32_t high_backing = alloc_block_high(size);
        if (high_backing == 0xffffffffu) return HLE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
        uint32_t high_uid = sr_alloc_uid();
        s_memory_blocks[high_slot].uid = high_uid;
        s_memory_blocks[high_slot].backing_uid = high_backing;
        return high_uid;
    }

    int slot = -1;
    for (int i = 0; i < HLE_MAX_MEMORY_BLOCKS; i++) {
        if (s_memory_blocks[i].uid == 0u) { slot = i; break; }
    }
    if (slot < 0) return HLE_KERNEL_ERROR_NO_MEMORY;
    uint32_t backing_uid = alloc_block(size);
    if (backing_uid == 0xffffffffu) return HLE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
    uint32_t uid = sr_alloc_uid();
    s_memory_blocks[slot].uid = uid;
    s_memory_blocks[slot].backing_uid = backing_uid;
    return uid;
}

static uint32_t h_GetMemoryBlockAddr(CpuState *s) {
    (void)s;
    HleMemoryBlock *block = memory_block_find(A0);
    if (!block) return HLE_KERNEL_ERROR_UNKNOWN_UID;
    if (!A1 || !sr_guest_span_writable(A1, 4u))
        return HLE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t addr = block_addr(block->backing_uid);
    if (addr == 0u) return HLE_KERNEL_ERROR_UNKNOWN_UID;
    MEM_W32(A1, addr);
    return 0;
}

static uint32_t h_FreeMemoryBlock(CpuState *s) {
    (void)s;
    HleMemoryBlock *block = memory_block_find(A0);
    if (!block) return HLE_KERNEL_ERROR_UNKNOWN_UID;
    uint32_t result = free_block(block->backing_uid);
    if (result == 0u) {
        block->uid = 0u;
        block->backing_uid = 0u;
    }
    return result;
}

static uint32_t partition_free(void) {
    user_partition_init();
    uint32_t top = user_partition_top();
    uint32_t f = s_heap < top ? top - s_heap : 0u;
    for (int i = 0; i < s_nfree_blocks; i++) {
        f += s_free_blocks[i].size;
    }
    return f;
}
/* A small accounting tail (the kernel keeps block headers per allocation) so the figure is not
 * the exact arithmetic free; PPSSPP reports e.g. 0x1a0b00 with several blocks live. */
static uint32_t h_TotalFreeMemSize(CpuState *s) {
    (void)s;
    uint32_t f = partition_free();
    return f > (uint32_t)s_nblocks*0x100u ? f - (uint32_t)s_nblocks*0x100u : f;
}
/* sceKernelMaxFreeMemSize: returns the largest contiguous free block across both the unallocated
 * space (from current s_heap to s_part_top) and the host-side free list blocks. */
static uint32_t h_MaxFreeMemSize(CpuState *s) {
    (void)s;
    user_partition_init();
    uint32_t top = user_partition_top();
    uint32_t max_free = s_heap < top ? top - s_heap : 0u;
    for (int i = 0; i < s_nfree_blocks; i++) {
        if (s_free_blocks[i].size > max_free) {
            max_free = s_free_blocks[i].size;
        }
    }
    return max_free;
}
/* PSP-measured 2026-10-15 (PSP-3000, firmware 6.61, probe on e45e667b; docs/HARDWARE_ORACLE.md
 * 2026-10-15 entry, PR #1003): an unknown or deleted FPL id returns SCE_KERNEL_ERROR_UNKNOWN_FPLID,
 * and a refer after delete returns it and writes nothing. */
/* UNMEASURED: create-time block failure and a blocking allocate outside a thread keep the code
 * they returned before the 2026-10-15 measurement; neither path was probed. */
#define FPL_UNMEASURED_NO_SPACE 0x800200d9u

/* A woken waiter that loses the freed block (or wakes for another reason, e.g. a
 * callback) must stay queued, at the head so FIFO order holds; FreeFpl dequeues
 * before it wakes, so without this the thread would block with no one left to
 * wake it. Idempotent: never adds a second entry. */
static void fpl_requeue_waiter_head(FplPool *p, uint32_t thread_uid) {
    for (int i = 0; i < p->nwaiters; i++)
        if (p->waiters[i].thread_uid == thread_uid) return;
    if (p->nwaiters >= FPL_MAX_WAITERS) return;
    for (int j = p->nwaiters; j > 0; j--) p->waiters[j] = p->waiters[j - 1];
    p->waiters[0].thread_uid = thread_uid;
    p->nwaiters++;
}

void fpl_remove_waiter(FplPool *p, uint32_t thread_uid) {
    for (int i = 0; i < p->nwaiters; i++) {
        if (p->waiters[i].thread_uid == thread_uid) {
            for (int j = i; j + 1 < p->nwaiters; j++) {
                p->waiters[j] = p->waiters[j + 1];
            }
            p->nwaiters--;
            break;
        }
    }
}

static uint32_t h_CreateFpl(CpuState *s) {
    /* a0=name, a1=partition, a2=attr, a3=blockSize. 5th arg (numBlocks) is in t0 (r8). */
    char name[32] = {0};
    if (A0) (void)guest_cstr(A0, name, sizeof(name));
    uint32_t bsize = A3;
    uint32_t nblocks = stack_arg(s, 0);
    if (bsize == 0) bsize = 16;
    if (nblocks == 0) nblocks = 1;
    uint32_t total = bsize * nblocks;
    uint32_t block_uid = alloc_block(total ? total : 16);
    if (block_uid == 0xFFFFFFFFu) return FPL_UNMEASURED_NO_SPACE;
    uint32_t pool = block_addr(block_uid);
    uint32_t uid = 0;
    for (int i = 0; i < FPL_MAX; i++) {
        if (!s_fpls[i].used) {
            uid = (uint32_t)(i + 0x500);
            FplPool *p = &s_fpls[i];
            if (p->free_blocks) free(p->free_blocks);
            if (p->allocated) free(p->allocated);
            memset(p, 0, sizeof *p);
            p->base = pool;
            p->cur = pool;
            p->end = pool + total;
            p->bsize = bsize;
            p->used = 1;
            p->block_uid = block_uid;
            p->attr = A2;
            memcpy(p->name, name, sizeof(p->name));
            p->nblocks = (int)nblocks;
            p->free_blocks = (uint32_t *)calloc(nblocks, sizeof(uint32_t));
            p->allocated = (uint8_t *)calloc(nblocks, sizeof(uint8_t));
            break;
        }
    }
    if (uid == 0) {
        free_block(block_uid);
        return FPL_UNMEASURED_NO_SPACE;
    }
    fprintf(stderr, "sceKernelCreateFpl: uid=0x%x pool_uid=0x%x base=0x%08x bsize=%u nblocks=%u total=%u\n", uid, block_uid, pool, bsize, nblocks, total);
    return uid;
}

static uint32_t fpl_try_allocate_internal(FplPool *p, uint32_t *addr_out) {
    if (p->nfree > 0) {
        uint32_t addr = p->free_blocks[--p->nfree];
        uint32_t bidx = (addr - p->base) / p->bsize;
        if (p->allocated && (int)bidx < p->nblocks) p->allocated[bidx] = 1;
        *addr_out = addr;
        return 0;
    }
    if (p->cur + p->bsize <= p->end) {
        uint32_t addr = p->cur;
        p->cur += p->bsize;
        uint32_t bidx = (addr - p->base) / p->bsize;
        if (p->allocated && (int)bidx < p->nblocks) p->allocated[bidx] = 1;
        *addr_out = addr;
        return 0;
    }
    return HLE_KERNEL_ERROR_NO_MEMORY; /* PSP-measured 2026-10-15: exhausted TryAllocateFpl */
}

static uint32_t h_TryAllocateFpl(CpuState *s) {
    /* a0=fplUid, a1=dataPtrOut. Returns 0 on success. */
    uint32_t uid = A0, out = A1;
    FplPool *p = fpl_lookup(uid);
    if (!p) { fprintf(stderr, "sceKernelTryAllocateFpl: bad uid=0x%x\n", uid); return SCE_KERNEL_ERROR_UNKNOWN_FPLID; }
    if (out && !sr_guest_span_writable(out, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t addr = 0;
    uint32_t err = fpl_try_allocate_internal(p, &addr);
    if (err) { fprintf(stderr, "sceKernelTryAllocateFpl: pool 0x%x exhausted\n", uid); return err; }
    if (out) MEM_W32(out, addr);
    fprintf(stderr, "sceKernelTryAllocateFpl: uid=0x%x -> 0x%08x (cur=0x%08x)\n", uid, addr, p->cur);
    return 0;
}

/* sceKernelAllocateFpl / ...CB are the BLOCKING allocate forms, so unlike
 * sceKernelTryAllocateFpl they are subject to the interrupt/dispatch context rule.
 * waits.expected puts the context decision ahead of the FPL object lookup: a bad
 * fpl id answers CAN_NOT_WAIT rather than the bad-id error, and so does a valid
 * pool that could have satisfied the request immediately. One leading check
 * covers both cases before any lookup or mutation. */
static uint32_t allocate_fpl_common(CpuState *s, int is_cb) {
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;

    uint32_t uid = A0, out = A1, toptr = A2;
    FplPool *p = fpl_lookup(uid);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_FPLID;

    if (out && !sr_guest_span_writable(out, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    uint64_t end_time = 0;
    int has_timeout = 0;
    if (toptr) {
        if (!sr_guest_span_writable(toptr, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        uint32_t usec = MEM_R32(toptr);
        if (usec == 0) {
            uint32_t addr = 0;
            if (p->nwaiters == 0 && fpl_try_allocate_internal(p, &addr) == 0) {
                if (out) MEM_W32(out, addr);
                return 0;
            }
            return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        }
        sched_vtime_refresh();
        end_time = sched_vtime_deadline_after((uint64_t)usec);
        has_timeout = 1;
    }

    uint32_t addr = 0;
    if (p->nwaiters == 0 && fpl_try_allocate_internal(p, &addr) == 0) {
        if (out) MEM_W32(out, addr);
        return 0;
    }

    uint32_t cur_thid = sched_current_uid();
    if (cur_thid == 0) {
        /* Direct invocation outside scheduler thread fiber cannot block */
        return FPL_UNMEASURED_NO_SPACE;
    }

    if (p->nwaiters >= FPL_MAX_WAITERS) return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */
    p->waiters[p->nwaiters++].thread_uid = cur_thid;

    for (;;) {
        if (is_cb && sr_thread_has_pending_callbacks(cur_thid)) {
            sr_thread_dispatch_callbacks();
            p = fpl_lookup(uid);
            if (!p) return SCE_KERNEL_ERROR_WAIT_DELETE;
            sched_vtime_refresh();
            continue;
        }

        /* PSP-B3-01: FPL waits report waitType 7 */
        sched_set_current_wait_kind(7);

        if (has_timeout) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= end_time) {
                p = fpl_lookup(uid);
                if (p) fpl_remove_waiter(p, cur_thid);
                MEM_W32(toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
            uint32_t remaining = (uint32_t)(end_time - now);
            if (is_cb) sched_set_current_cb_wait(1);
            int timed_out = sched_block_on_timeout(uid, remaining);
            if (is_cb) sched_set_current_cb_wait(0);

            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) return woken;

            p = fpl_lookup(uid);
            if (!p) return SCE_KERNEL_ERROR_WAIT_DELETE;

            if (fpl_try_allocate_internal(p, &addr) == 0) {
                if (out) MEM_W32(out, addr);
                sched_vtime_refresh();
                now = sched_vtime_us();
                uint32_t rem = (now < end_time) ? (uint32_t)(end_time - now) : 0u;
                MEM_W32(toptr, rem);
                return 0;
            }

            if (timed_out) {
                fpl_remove_waiter(p, cur_thid);
                MEM_W32(toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
            fpl_requeue_waiter_head(p, cur_thid);
        } else {
            if (is_cb) sched_set_current_cb_wait(1);
            sched_block_on(uid);
            if (is_cb) sched_set_current_cb_wait(0);

            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) return woken;

            p = fpl_lookup(uid);
            if (!p) return SCE_KERNEL_ERROR_WAIT_DELETE;

            if (fpl_try_allocate_internal(p, &addr) == 0) {
                if (out) MEM_W32(out, addr);
                return 0;
            }
            fpl_requeue_waiter_head(p, cur_thid);
        }
    }
}

static uint32_t h_AllocateFpl(CpuState *s) {
    return allocate_fpl_common(s, 0);
}

static uint32_t h_AllocateFplCB(CpuState *s) {
    return allocate_fpl_common(s, 1);
}

static uint32_t h_DeleteFpl(CpuState *s) {
    uint32_t uid = A0;
    uint32_t idx = uid - 0x500;
    if (idx >= FPL_MAX || !s_fpls[idx].used) return 0;
    FplPool *p = &s_fpls[idx];
    uint32_t block_uid = p->block_uid;
    int waiters = sched_count_waiters(uid);
    if (waiters) {
        sched_wake_with_result(uid, SCE_KERNEL_ERROR_WAIT_DELETE);
        sched_preempt();
    }
    if (p->free_blocks) free(p->free_blocks);
    if (p->allocated) free(p->allocated);
    memset(p, 0, sizeof *p);
    if (block_uid) {
        free_block(block_uid);
    }
    return 0;
}

static uint32_t h_FreeFpl(CpuState *s) {
    uint32_t uid = A0, addr = A1;
    FplPool *p = fpl_lookup(uid);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_FPLID;
    if (addr < p->base || addr + p->bsize > p->end || ((addr - p->base) % p->bsize) != 0) {
        /* Exact bad-address error code for FreeFpl is UNMEASURED; using SCE_KERNEL_ERROR_ILLEGAL_ADDR (0x80000103u) as defined in hle.c. */
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    uint32_t bidx = (addr - p->base) / p->bsize;
    if (p->allocated && (int)bidx < p->nblocks && !p->allocated[bidx]) {
        /* Double-free or block not currently in use; exact error code is UNMEASURED, returning SCE_KERNEL_ERROR_ILLEGAL_ADDR */
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    }
    if (p->allocated && (int)bidx < p->nblocks) p->allocated[bidx] = 0;
    if (p->free_blocks && p->nfree < p->nblocks) {
        p->free_blocks[p->nfree++] = addr;
    }
    /* Waiter order: semaphores in hle.c do not handle attr for priority/FIFO queuing;
     * per task instruction, default to FIFO order and document.
     * Wake one waiter. */
    if (p->nwaiters > 0) {
        uint32_t next_thread = p->waiters[0].thread_uid;
        for (int j = 0; j + 1 < p->nwaiters; j++) {
            p->waiters[j] = p->waiters[j + 1];
        }
        p->nwaiters--;
        sched_wake_one_object_waiter(uid, next_thread);
        sched_preempt();
    }
    return 0;
}

/* sceKernelReferFplStatus(fplid, SceKernelFplInfo *info). The 56-byte struct follows the public
 * PSPSDK SceKernelFplInfo field order: size(0), name[32](4), attr(36), blockSize(40),
 * numBlocks(44), freeBlocks(48), numWaitThreads(52). The write rule is the one measured for the
 * sema, event-flag and mailbox status calls (PSP-KERNEL-STATUS-001): a caller size of 0 writes
 * nothing and succeeds; otherwise min(size, 56) bytes of a struct whose size word is 56 reach
 * guest memory. The 56-byte size word was measured on the PSP-3000 oracle (2026-10-10,
 * hle-kernel-status: sceKernelReferFplStatus reports 0x38).
 * freeBlocks is the free-list length plus the blocks not yet handed out from the bump region. */
typedef struct {
    uint32_t size;
    char     name[32];
    uint32_t attr;
    int32_t  blockSize;
    int32_t  numBlocks;
    int32_t  freeBlocks;
    int32_t  numWaitThreads;
} SceKernelFplInfo;

static uint32_t h_ReferFplStatus(CpuState *s) {
    uint32_t uid = A0;
    uint32_t info_addr = A1;
    if (!info_addr || !sr_guest_span_readable(info_addr, 4u))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    FplPool *p = fpl_lookup(uid);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_FPLID;
    uint32_t input_size = MEM_R32(info_addr);
    if (input_size == 0) return 0;
    uint32_t write_len = input_size < (uint32_t)sizeof(SceKernelFplInfo)
                         ? input_size : (uint32_t)sizeof(SceKernelFplInfo);
    if (!sr_guest_span_writable(info_addr, write_len))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    SceKernelFplInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(SceKernelFplInfo);
    memcpy(info.name, p->name, sizeof(info.name));
    info.attr = p->attr;
    info.blockSize = (int32_t)p->bsize;
    info.numBlocks = p->nblocks;
    info.freeBlocks = p->nfree + (int32_t)((p->end - p->cur) / p->bsize);
    info.numWaitThreads = p->nwaiters;
    for (uint32_t i = 0; i < write_len; i++)
        MEM_W8(info_addr + i, ((const uint8_t *)&info)[i]);
    return 0;
}
#define VPL_ALIGN 16u
/* PSP-measured (docs/HARDWARE_ORACLE.md kernel-object error table, PSP-B1-01): an unknown VPL id
 * returns SCE_KERNEL_ERROR_UNKNOWN_VPLID. */
/* UNMEASURED: no VPL exhausted code is recorded; this is the pre-measurement value, not the FPL
 * code. */
#define VPL_EXHAUSTED 0x800200d9u

/* Grow a pool array to hold `need` entries. A host that cannot provide them stops the run
 * by name: refusing the guest would report a full pool that is not full. */
static void *vpl_reserve(void *items, int *cap, int need, size_t item_size) {
    if (need <= *cap) return items;
    int grown = *cap ? *cap : 16;
    while (grown < need) grown *= 2;
    void *next = realloc(items, (size_t)grown * item_size);
    if (!next) {
        fprintf(stderr, "VPL: host allocation of %d entries failed\n", grown);
        sr_flight_fatal(SR_FLIGHT_KIND_FATAL_HOST, 0u, (uint32_t)grown, (uint32_t)item_size);
        abort();
    }
    *cap = grown;
    return next;
}

void vpl_release(VplPool *p) {
    free(p->free);
    free(p->allocs);
    memset(p, 0, sizeof *p);
}

static void vpl_insert_free(VplPool *p, uint32_t addr, uint32_t size) {
    int i = 0;
    while (i < p->nfree && p->free[i].addr < addr) i++;
    p->free = vpl_reserve(p->free, &p->free_cap, p->nfree + 1, sizeof *p->free);
    memmove(&p->free[i + 1], &p->free[i], (size_t)(p->nfree - i) * sizeof *p->free);
    p->free[i] = (VplSpan){addr, size};
    p->nfree++;
    /* Coalesce all touching neighbors. */
    for (i = 0; i + 1 < p->nfree;) {
        if (p->free[i].addr + p->free[i].size == p->free[i + 1].addr) {
            p->free[i].size += p->free[i + 1].size;
            for (int j = i + 1; j + 1 < p->nfree; ++j) p->free[j] = p->free[j + 1];
            --p->nfree;
        } else ++i;
    }
}

/* Same contract as fpl_requeue_waiter_head(). */
static void vpl_requeue_waiter_head(VplPool *p, uint32_t thread_uid, uint32_t size) {
    for (int i = 0; i < p->nwaiters; i++)
        if (p->waiters[i].thread_uid == thread_uid) return;
    if (p->nwaiters >= VPL_MAX_WAITERS) return;
    for (int j = p->nwaiters; j > 0; j--) p->waiters[j] = p->waiters[j - 1];
    p->waiters[0] = (VplWaiter){thread_uid, size};
    p->nwaiters++;
}

void vpl_remove_waiter(VplPool *p, uint32_t thread_uid) {
    for (int i = 0; i < p->nwaiters; i++) {
        if (p->waiters[i].thread_uid == thread_uid) {
            for (int j = i; j + 1 < p->nwaiters; j++) {
                p->waiters[j] = p->waiters[j + 1];
            }
            p->nwaiters--;
            break;
        }
    }
}

static uint32_t h_CreateVpl(CpuState *s) {
    uint32_t size = A3;
    if (size == 0) return VPL_EXHAUSTED; /* exact zero-size result is UNMEASURED */
    uint32_t block_uid = alloc_block(size);
    if (block_uid == 0xFFFFFFFFu) return VPL_EXHAUSTED;
    uint32_t base = block_addr(block_uid);
    for (int i = 0; i < VPL_MAX; ++i) {
        if (!s_vpls[i].used) {
            VplPool *p = &s_vpls[i];
            vpl_release(p);
            p->base = base; p->size = size; p->block_uid = block_uid; p->used = 1;
            p->attr = A2;
            vpl_insert_free(p, base, size);
            return (uint32_t)(0x600 + i);
        }
    }
    (void)free_block(block_uid);
    return VPL_EXHAUSTED;
}

static uint32_t vpl_try_allocate_internal(VplPool *p, uint32_t request, uint32_t *addr_out) {
    if (request == 0) return VPL_EXHAUSTED; /* exact zero-size result is UNMEASURED */
    if (request > p->size) return VPL_EXHAUSTED; /* also keeps the rounding below from wrapping */
    uint32_t need = (request + (VPL_ALIGN - 1u)) & ~(VPL_ALIGN - 1u);
    for (int i = 0; i < p->nfree; ++i) {
        if (p->free[i].size >= need) {
            uint32_t addr = p->free[i].addr;
            p->allocs = vpl_reserve(p->allocs, &p->allocs_cap, p->nallocs + 1, sizeof *p->allocs);
            p->allocs[p->nallocs++] = (VplAlloc){addr, need};
            p->free[i].addr += need; p->free[i].size -= need;
            if (!p->free[i].size) {
                for (int j = i; j + 1 < p->nfree; ++j) p->free[j] = p->free[j + 1];
                --p->nfree;
            }
            *addr_out = addr;
            return 0;
        }
    }
    return VPL_EXHAUSTED;
}

static uint32_t h_TryAllocateVpl(CpuState *s) {
    VplPool *p = vpl_lookup(A0);
    uint32_t request = A1, out = A2;
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_VPLID;
    if (request == 0) return VPL_EXHAUSTED; /* exact zero-size result is UNMEASURED */
    if (out && !sr_guest_span_writable(out, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    uint32_t addr = 0;
    uint32_t err = vpl_try_allocate_internal(p, request, &addr);
    if (err) return err;
    if (out) MEM_W32(out, addr);
    return 0;
}

static uint32_t allocate_vpl_common(CpuState *s, int is_cb) {
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;

    uint32_t uid = A0, request = A1, out = A2, toptr = A3;
    VplPool *p = vpl_lookup(uid);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_VPLID;
    if (request == 0) return VPL_EXHAUSTED;
    if (out && !sr_guest_span_writable(out, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    uint64_t end_time = 0;
    int has_timeout = 0;
    if (toptr) {
        if (!sr_guest_span_writable(toptr, 4u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        uint32_t usec = MEM_R32(toptr);
        if (usec == 0) {
            uint32_t addr = 0;
            if (p->nwaiters == 0 && vpl_try_allocate_internal(p, request, &addr) == 0) {
                if (out) MEM_W32(out, addr);
                return 0;
            }
            return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        }
        sched_vtime_refresh();
        end_time = sched_vtime_deadline_after((uint64_t)usec);
        has_timeout = 1;
    }

    uint32_t addr = 0;
    if (p->nwaiters == 0 && vpl_try_allocate_internal(p, request, &addr) == 0) {
        if (out) MEM_W32(out, addr);
        return 0;
    }

    uint32_t cur_thid = sched_current_uid();
    if (cur_thid == 0) {
        return VPL_EXHAUSTED;
    }

    if (p->nwaiters >= VPL_MAX_WAITERS) return 0x80020190u; /* SCE_KERNEL_ERROR_NO_MEMORY */
    p->waiters[p->nwaiters++] = (VplWaiter){cur_thid, request};

    for (;;) {
        if (is_cb && sr_thread_has_pending_callbacks(cur_thid)) {
            sr_thread_dispatch_callbacks();
            p = vpl_lookup(uid);
            if (!p) return SCE_KERNEL_ERROR_WAIT_DELETE;
            sched_vtime_refresh();
            continue;
        }

        /* PSP waitType 6 = VPL */
        sched_set_current_wait_kind(6);

        if (has_timeout) {
            sched_vtime_refresh();
            uint64_t now = sched_vtime_us();
            if (now >= end_time) {
                p = vpl_lookup(uid);
                if (p) vpl_remove_waiter(p, cur_thid);
                MEM_W32(toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
            uint32_t remaining = (uint32_t)(end_time - now);
            if (is_cb) sched_set_current_cb_wait(1);
            int timed_out = sched_block_on_timeout(uid, remaining);
            if (is_cb) sched_set_current_cb_wait(0);

            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) return woken;

            p = vpl_lookup(uid);
            if (!p) return SCE_KERNEL_ERROR_WAIT_DELETE;

            if (vpl_try_allocate_internal(p, request, &addr) == 0) {
                if (out) MEM_W32(out, addr);
                sched_vtime_refresh();
                now = sched_vtime_us();
                uint32_t rem = (now < end_time) ? (uint32_t)(end_time - now) : 0u;
                MEM_W32(toptr, rem);
                return 0;
            }

            if (timed_out) {
                vpl_remove_waiter(p, cur_thid);
                MEM_W32(toptr, 0u);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }

            vpl_requeue_waiter_head(p, cur_thid, request);
        } else {
            if (is_cb) sched_set_current_cb_wait(1);
            sched_block_on(uid);
            if (is_cb) sched_set_current_cb_wait(0);

            uint32_t woken = 0;
            if (sched_take_wake_result(&woken)) return woken;

            p = vpl_lookup(uid);
            if (!p) return SCE_KERNEL_ERROR_WAIT_DELETE;

            if (vpl_try_allocate_internal(p, request, &addr) == 0) {
                if (out) MEM_W32(out, addr);
                return 0;
            }

            vpl_requeue_waiter_head(p, cur_thid, request);
        }
    }
}

static uint32_t h_AllocateVpl(CpuState *s) {
    return allocate_vpl_common(s, 0);
}

static uint32_t h_AllocateVplCB(CpuState *s) {
    return allocate_vpl_common(s, 1);
}

static uint32_t h_FreeVpl(CpuState *s) {
    VplPool *p = vpl_lookup(A0);
    uint32_t addr = A1;
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_VPLID;
    for (int i = 0; i < p->nallocs; ++i) if (p->allocs[i].addr == addr) {
        vpl_insert_free(p, addr, p->allocs[i].size);
        p->allocs[i] = p->allocs[--p->nallocs];
        /* Waiter order: semaphores in hle.c do not handle attr for priority/FIFO queuing;
         * per task instruction, default to FIFO order and document.
         * Wake one waiter. */
        if (p->nwaiters > 0) {
            uint32_t next_thread = p->waiters[0].thread_uid;
            for (int j = 0; j + 1 < p->nwaiters; j++) {
                p->waiters[j] = p->waiters[j + 1];
            }
            p->nwaiters--;
            sched_wake_one_object_waiter(A0, next_thread);
            sched_preempt();
        }
        return 0;
    }
    return SCE_KERNEL_ERROR_UNKNOWN_VPLID;
}

static uint32_t h_DeleteVpl(CpuState *s) {
    VplPool *p = vpl_lookup(A0);
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_VPLID;
    uint32_t block_uid = p->block_uid;
    uint32_t uid = A0;
    int waiters = sched_count_waiters(uid);
    if (waiters) {
        sched_wake_with_result(uid, SCE_KERNEL_ERROR_WAIT_DELETE);
        sched_preempt();
    }
    vpl_release(p);
    return free_block(block_uid); /* exactly once; unknown/double delete frees nothing */
}

static uint32_t h_ReferVplStatus(CpuState *s) {
    VplPool *p = vpl_lookup(A0);
    uint32_t info = A1;
    if (!p) return SCE_KERNEL_ERROR_UNKNOWN_VPLID;
    if (!info || !sr_guest_span_writable(info, 52u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    /* SceKernelVplInfo: size, name[32], attr, poolSize, freeSize, numWaitThreads. */
    MEM_W32(info + 0, 52u);
    MEM_W32(info + 36, p->attr);
    MEM_W32(info + 40, p->size);
    uint32_t free_size = 0;
    for (int i = 0; i < p->nfree; ++i) free_size += p->free[i].size;
    MEM_W32(info + 44, free_size);
    MEM_W32(info + 48, (uint32_t)p->nwaiters);
    return 0;
}
#define TLSPL_BAD_ID 0x800200d3u
#define TLSPL_NO_MEMORY 0x80020190u
#define TLSPL_BUSY 0x800201a8u
#define TLSPL_ILLEGAL_SIZE 0x800201bcu
#define TLSPL_ILLEGAL_ATTR 0x80020191u
#define TLSPL_USER_PARTITION 2u
#define TLSPL_ATTR_WAIT_PRIORITY 0x0100u
#define TLSPL_ATTR_MEM_BOTTOM 0x4000u
#define TLSPL_MAX_ALIGNMENT 256u

TlsplPool s_tlspls[TLSPL_MAX];

static TlsplPool *tlspl_find(uint32_t uid) {
    for (int i = 0; i < TLSPL_MAX; ++i)
        if (s_tlspls[i].used && s_tlspls[i].uid == uid) return &s_tlspls[i];
    return NULL;
}

void tlspl_release_thread(TlsplPool *p, uint32_t thread_uid) {
    if (!p || !p->used || !thread_uid) return;
    for (uint32_t i = 0; i < p->num_blocks; ++i) {
        if (p->thread_uids[i] == thread_uid) {
            p->thread_uids[i] = 0u;
            if (p->free_blocks < p->num_blocks) ++p->free_blocks;
            return;
        }
    }
}

static uint32_t h_CreateTlspl(CpuState *s) {
    /* a0=name, a1=partition, a2=attr, a3=blockSize, t0=numBlocks, t1=options. */
    char name[32] = {0};
    uint32_t block_size = A3;
    uint32_t num_blocks = stack_arg(s, 0);
    uint32_t options = stack_arg(s, 1);
    uint32_t alignment = 4u;
    if (A0 && !guest_cstr(A0, name, sizeof(name))) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (A1 != TLSPL_USER_PARTITION) {
        fprintf(stderr, "TLSPL_PARTITION_UNMODELED: sceKernelCreateTlspl partition=%u; in the works (#339)\n", A1);
        return TLSPL_BAD_ID;
    }
    if (A2 & ~(TLSPL_ATTR_WAIT_PRIORITY | TLSPL_ATTR_MEM_BOTTOM)) {
        fprintf(stderr, "TLSPL_ATTRIBUTES_UNMODELED: sceKernelCreateTlspl attr=0x%08x; in the works (#339)\n", A2);
        return TLSPL_ILLEGAL_ATTR;
    }
    if (A2 & TLSPL_ATTR_MEM_BOTTOM) {
        fprintf(stderr, "TLSPL_MEMORY_BOTTOM_UNMODELED: sceKernelCreateTlspl (0x8daff657); in the works (#339)\n");
        return TLSPL_ILLEGAL_ATTR;
    }
    if (!block_size || !num_blocks) return TLSPL_ILLEGAL_SIZE;

    if (options) {
        if (!sr_guest_span_readable(options, 8u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        if (MEM_R32(options) < 8u) return TLSPL_ILLEGAL_SIZE;
        alignment = MEM_R32(options + 4u);
        if (!alignment) alignment = 4u;
    }
    if (alignment < 4u || alignment > TLSPL_MAX_ALIGNMENT ||
        (alignment & (alignment - 1u)) != 0u) {
        fprintf(stderr, "TLSPL_ALIGNMENT_UNMODELED: sceKernelCreateTlspl alignment=%u; in the works (#339)\n", alignment);
        return TLSPL_ILLEGAL_SIZE;
    }

    uint64_t stride64 = ((uint64_t)block_size + alignment - 1u) & ~((uint64_t)alignment - 1u);
    uint64_t total64 = stride64 * (uint64_t)num_blocks;
    /* alloc_block rounds to a 256-byte slot using uint32 arithmetic. */
    if (!stride64 || total64 > UINT32_MAX - 0xffu) return TLSPL_ILLEGAL_SIZE;

    TlsplPool *p = NULL;
    for (int i = 0; i < TLSPL_MAX; ++i) {
        if (!s_tlspls[i].used) { p = &s_tlspls[i]; break; }
    }
    if (!p) return TLSPL_NO_MEMORY;

    uint32_t total_size = (uint32_t)total64;
    uint32_t block_uid = alloc_block(total_size);
    if (block_uid == 0xFFFFFFFFu) return TLSPL_NO_MEMORY;
    uint32_t *thread_uids = (uint32_t *)calloc((size_t)num_blocks, sizeof(*thread_uids));
    if (!thread_uids) {
        (void)free_block(block_uid);
        return TLSPL_NO_MEMORY;
    }

    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->uid = sr_alloc_uid();
    memcpy(p->name, name, sizeof(p->name));
    p->attr = A2;
    p->block_uid = block_uid;
    p->base = block_addr(block_uid);
    p->block_size = block_size;
    p->stride = (uint32_t)stride64;
    p->num_blocks = num_blocks;
    p->total_size = total_size;
    p->free_blocks = num_blocks;
    p->thread_uids = thread_uids;
    (void)A1; /* The runtime currently exposes one user partition. */
    return p->uid;
}

static uint32_t h_DeleteTlspl(CpuState *s) {
    TlsplPool *p = tlspl_find(A0);
    if (!p) return TLSPL_BAD_ID;
    if (p->free_blocks != p->num_blocks) {
        uint32_t held_blocks = p->num_blocks - p->free_blocks;
        fprintf(stderr,
                "TLSPL_DELETE_WITH_HELD_BLOCKS: pool 0x%x has %u/%u blocks held; in the works (#339)\n",
                p->uid, held_blocks, p->num_blocks);
        return TLSPL_BUSY;
    }
    uint32_t block_uid = p->block_uid;
    free(p->thread_uids);
    memset(p, 0, sizeof(*p));
    return free_block(block_uid);
}

static uint32_t h_FreeTlspl(CpuState *s) {
    TlsplPool *p = tlspl_find(A0);
    if (!p) return TLSPL_BAD_ID;
    uint32_t thread_uid = sched_current_uid();
    if (!thread_uid) return TLSPL_BAD_ID;
    for (uint32_t i = 0; i < p->num_blocks; ++i) {
        if (p->thread_uids[i] == thread_uid) {
            tlspl_release_thread(p, thread_uid);
            return 0;
        }
    }
    return TLSPL_BAD_ID;
}

static uint32_t h_GetTlsAddr(CpuState *s) {
    TlsplPool *p = tlspl_find(A0);
    if (!p) return 0u;
    uint32_t thread_uid = sched_current_uid();
    if (!thread_uid) {
        fprintf(stderr, "TLSPL_CURRENT_THREAD_UNAVAILABLE: sceKernelGetTlsAddr (0xfa835cde); in the works (#339)\n");
        return 0u;
    }

    for (uint32_t i = 0; i < p->num_blocks; ++i) {
        if (p->thread_uids[i] == thread_uid) {
            uint64_t addr = (uint64_t)p->base + (uint64_t)i * p->stride;
            return addr <= UINT32_MAX ? (uint32_t)addr : 0u;
        }
    }
    for (uint32_t i = 0; i < p->num_blocks; ++i) {
        if (p->thread_uids[i]) continue;
        uint64_t addr64 = (uint64_t)p->base + (uint64_t)i * p->stride;
        if (addr64 > UINT32_MAX || !sr_guest_span_writable((uint32_t)addr64, p->block_size)) {
            fprintf(stderr, "TLSPL_MEMORY_UNAVAILABLE: sceKernelGetTlsAddr (0xfa835cde); in the works (#339)\n");
            return 0u;
        }
        uint32_t addr = (uint32_t)addr64;
        for (uint32_t j = 0; j < p->block_size; ++j) MEM_W8(addr + j, 0u);
        p->thread_uids[i] = thread_uid;
        if (p->free_blocks) --p->free_blocks;
        return addr;
    }
    fprintf(stderr, "TLSPL_POOL_EXHAUSTED: sceKernelGetTlsAddr (0xfa835cde) returned NULL; blocking allocation is in the works (#339)\n");
    return 0u;
}

static uint32_t h_ReferTlsplStatus(CpuState *s) {
    TlsplPool *p = tlspl_find(A0);
    uint32_t info = A1;
    if (!p) return TLSPL_BAD_ID;
    if (!info || !sr_guest_span_writable(info, 56u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    /* SceKernelTlsplInfo: size, name[32], attr, blockSize, numBlocks, freeBlocks, numWaitThreads. */
    MEM_W32(info + 0u, 56u);
    for (uint32_t i = 0; i < sizeof(p->name); ++i) MEM_W8(info + 4u + i, (uint8_t)p->name[i]);
    MEM_W32(info + 36u, p->attr);
    MEM_W32(info + 40u, p->block_size);
    MEM_W32(info + 44u, p->num_blocks);
    MEM_W32(info + 48u, p->free_blocks);
    MEM_W32(info + 52u, 0u);
    return 0;
}

/* The lock sceKernelVolatileMemLock and TryLock take and sceKernelVolatileMemUnlock releases:
 * one lock over the 4 MiB volatile region at 0x08400000 (#1383).
 *
 * Measured on PSP-3000 6.6.1: Lock on a free lock returns 0 and writes the address 0x08400000 and
 * the size 4 MiB; Unlock with nothing locked returns 0x800201ae (the raw value; the PSPSDK names
 * it SCE_KERNEL_ERROR_SEMA_OVF); Unlock while the lock is held returns 0 and releases it
 * (2026-10-21, probe CASE=fake-success, #1135). While the lock is held, TryLock returns
 * 0x802b0200 and writes neither output (2026-10-22, PR #1153, #1190).
 *
 * PSPAutotests tests/intr/waits.expected (hardware-generated; src/rt/intr_conformance.h cites each
 * line): Lock returns CAN_NOT_WAIT with interrupts or dispatch disabled, whether the lock is free
 * or held (L290-L293), and ILLEGAL_CONTEXT from an interrupt handler while it is free (L412), so
 * both contexts are checked before the lock state.
 *
 * Contract (#1383): Lock while the lock is held blocks through the scheduler's wait-invocation
 * substrate and waiters queue in FIFO order. Unlock hands the lock to the first queued waiter
 * whose wait is still live, writes that waiter's address and size outputs and completes its wait
 * with 0.
 *
 * Project choices, UNMEASURED on hardware:
 *  - Lock and TryLock accept only type 0: types 1, 2 and -1 answer INVALID_MODE (0x80000107) and
 *    write neither output, Lock as TryLock, while the lock is free (measured on a PSP-3000 6.61 by
 *    PSP-KERNEL-SEM-001 vmem-trylock-type-* and vmem-lock-type-1, 2026-10-31, probe 0c3cf0da). The
 *    type is checked first; its order against the other checks is UNMEASURED. Unlock's argument is
 *    not checked: Unlock keeps its measured route.
 *  - The lock has no owning thread: Unlock from any thread releases it, as the semaphore overflow
 *    code above suggests (an inference). Terminating or deleting the thread that took the lock
 *    therefore leaves it held. A waiting thread that is terminated, deleted or released by
 *    sceKernelReleaseWaitThread leaves the queue through its wait invocation's detach hook and is
 *    never handed the lock.
 *  - Output spans are validated before anything is written or queued (ILLEGAL_ADDR for an
 *    unwritable non-NULL span); NULL outputs are tolerated. CAN_NOT_WAIT and ILLEGAL_CONTEXT write
 *    no output. A blocked Lock latches no ReferThreadStatus wait type.
 *
 * The region is partition 5 of the guest-memory model (volatile_part_alloc), with its own
 * allocator. The lock does not reserve it against that allocator: pspautotests
 * tests/sysmem/volatile.expected (f573fe8c, recorded on a PSP) allocates partition-5 blocks with
 * the lock free and creates pools there while holding it, so neither waits on the other. */
#define VOLATILE_MEM_BASE      0x08400000u
#define VOLATILE_MEM_SIZE      0x00400000u
#define VOLATILE_MEM_WAIT_OBJ  0x564d454du   /* "VMEM": the scheduler object a blocked Lock waits on */
#define VOLATILE_MEM_HELD      0x802b0200u   /* TryLock while held (measured) */
#define VOLATILE_MEM_NOT_HELD  0x800201aeu   /* Unlock with nothing held (measured) */

typedef struct {
    SrWaitHandle invocation;
    uint32_t addr_out, size_out;
} VolatileMemWaiter;

int s_volatile_locked = 0;
static VolatileMemWaiter *s_volatile_waiters;    /* FIFO: index 0 is the oldest waiter */
static size_t s_volatile_wait_count, s_volatile_wait_cap;

#define VOLATILE_MEM_INVALID_MODE 0x80000107u   /* SCE_KERNEL_ERROR_INVALID_MODE (measured) */

/* Both complete non-NULL output spans are checked before either one is written. */
static int volatile_mem_outputs_writable(uint32_t addr_out, uint32_t size_out) {
    return (!addr_out || sr_guest_span_writable(addr_out, 4u)) &&
           (!size_out || sr_guest_span_writable(size_out, 4u));
}

static void volatile_mem_take(uint32_t addr_out, uint32_t size_out) {
    s_volatile_locked = 1;
    if (addr_out) MEM_W32(addr_out, VOLATILE_MEM_BASE);
    if (size_out) MEM_W32(size_out, VOLATILE_MEM_SIZE);
}

static void volatile_mem_remove_waiter(size_t i) {
    memmove(&s_volatile_waiters[i], &s_volatile_waiters[i + 1u],
            (s_volatile_wait_count - i - 1u) * sizeof(*s_volatile_waiters));
    s_volatile_wait_count--;
}

static int volatile_mem_enqueue(SrWaitHandle invocation, uint32_t addr_out, uint32_t size_out) {
    if (s_volatile_wait_count == s_volatile_wait_cap) {
        size_t next = s_volatile_wait_cap ? s_volatile_wait_cap * 2u : 4u;
        if (next > SIZE_MAX / sizeof(*s_volatile_waiters)) return 0;
        VolatileMemWaiter *grown =
            (VolatileMemWaiter *)realloc(s_volatile_waiters, next * sizeof(*grown));
        if (!grown) return 0;
        s_volatile_waiters = grown;
        s_volatile_wait_cap = next;
    }
    VolatileMemWaiter *w = &s_volatile_waiters[s_volatile_wait_count++];
    w->invocation = invocation;
    w->addr_out = addr_out;
    w->size_out = size_out;
    return 1;
}

/* Wait-invocation detach hook: a waiter that finished, or whose thread was terminated, deleted
 * or released, leaves the queue. The lock state does not change, so nobody is notified. */
static int volatile_mem_detach(SrWaitHandle invocation, uint32_t object) {
    (void)object;
    for (size_t i = 0; i < s_volatile_wait_count; i++) {
        if (s_volatile_waiters[i].invocation == invocation) {
            volatile_mem_remove_waiter(i);
            break;
        }
    }
    return 0;
}

/* Hand the free lock to the oldest waiter whose wait is still live. Its output spans are
 * validated again before the grant; a waiter whose spans fail gets the ILLEGAL_ADDR Lock gives at
 * entry and the next waiter is tried. Returns 1 when a waiter now holds the lock. */
static int volatile_mem_grant_next(void) {
    while (s_volatile_wait_count > 0u) {
        VolatileMemWaiter w = s_volatile_waiters[0];
        volatile_mem_remove_waiter(0);
        if (!volatile_mem_outputs_writable(w.addr_out, w.size_out)) {
            (void)sched_wait_complete(w.invocation, SCE_KERNEL_ERROR_ILLEGAL_ADDR);
            continue;
        }
        if (!sched_wait_complete(w.invocation, 0u)) continue;  /* released, terminated, deleted */
        volatile_mem_take(w.addr_out, w.size_out);
        return 1;
    }
    return 0;
}

static uint32_t h_VolatileMemLock(CpuState *s) {
    if (A0 != 0u) return VOLATILE_MEM_INVALID_MODE;
    if (sched_is_intr_context()) return SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;   /* L412 */
    if (!sched_wait_permitted()) return SCE_KERNEL_ERROR_CAN_NOT_WAIT;      /* L290-L293 */
    if (!volatile_mem_outputs_writable(A1, A2)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (!s_volatile_locked) {
        volatile_mem_take(A1, A2);
        return 0;
    }

    SrWaitHandle invocation = sched_wait_begin(VOLATILE_MEM_WAIT_OBJ, 0u, 0, volatile_mem_detach);
    if (!invocation) return HLE_KERNEL_ERROR_NO_MEMORY;
    if (!volatile_mem_enqueue(invocation, A1, A2)) {
        (void)sched_wait_finish(invocation);
        return HLE_KERNEL_ERROR_NO_MEMORY;
    }
    /* Only Unlock's grant (0), a span refusal or a release, termination or deletion completes
     * this invocation; any other wake finds no result and waits again. */
    uint32_t result;
    while (!sched_wait_take_result(invocation, &result))
        (void)sched_wait_block(invocation, 0u, 0);
    if (sched_wait_finish(invocation)) sched_preempt();
    return result;
}

static uint32_t h_VolatileMemTryLock(CpuState *s) {
    if (A0 != 0u) return VOLATILE_MEM_INVALID_MODE;
    if (s_volatile_locked) return VOLATILE_MEM_HELD;
    if (!volatile_mem_outputs_writable(A1, A2)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    volatile_mem_take(A1, A2);
    return 0;
}

static uint32_t h_VolatileMemUnlock(CpuState *s) {
    (void)s;
    if (!s_volatile_locked) return VOLATILE_MEM_NOT_HELD;
    s_volatile_locked = 0;
    if (volatile_mem_grant_next()) sched_preempt();   /* a stronger new holder runs now */
    return 0;
}

/* Test-only: forget the lock and its queue. Waits still parked on it belong to fixture threads
 * the caller is discarding. */
void sr_hle_test_volatile_mem_reset(void) {
    s_volatile_locked = 0;
    free(s_volatile_waiters);
    s_volatile_waiters = NULL;
    s_volatile_wait_count = 0u;
    s_volatile_wait_cap = 0u;
}

/* sceKernelDevkitVersion (0x3fc9ae6a): one definition reached by both sr_hle_init() branches, so the
 * executable harness dispatches the production mapping (the production-only list did not reach it).
 * PSP-3000 6.61 oracle 2026-10-15, record PSP-SYSTEM-001 (case model-profile). */
void hle_register_devkit_version_handler(void) {
    sr_hle_register(0x3fc9ae6a, "sceKernelDevkitVersion", h_KernelDevkitVersion);
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_kernel_import_sweep_handlers(). */
void hle_register_kernel_import_sweep_kernel_memory(void) {
    sr_hle_register(0x50f61d8a, "sceKernelFreeMemoryBlock", h_FreeMemoryBlock);
    sr_hle_register(0xdb83a952, "sceKernelGetMemoryBlockAddr", h_GetMemoryBlockAddr);
    sr_hle_register(0xfe707fdf, "sceKernelAllocMemoryBlock", h_AllocMemoryBlock);
    sr_hle_register(0x342061e5, "sceKernelSetCompiledSdkVersion370", h_SetCompiledSdkVersion);
    sr_hle_register(0x315ad3a0u, "sceKernelSetCompiledSdkVersion380_390", h_SetCompiledSdkVersion);
}

/* From hle_register_partition_savedata_handlers(). */
void hle_register_partition_savedata_kernel_memory(void) {
    sr_hle_register(0x237dbd4f, "sceKernelAllocPartitionMemory", h_AllocPartitionMemory);
    sr_hle_register(0x9d9a5ba1, "sceKernelGetBlockHeadAddr", h_GetBlockHeadAddr);
    sr_hle_register(0xb6d61d02, "sceKernelFreePartitionMemory", h_FreePartitionMemory);
    sr_hle_register(0xf919f628, "sceKernelTotalFreeMemSize", h_TotalFreeMemSize);
    sr_hle_register(0xa291f107, "sceKernelMaxFreeMemSize", h_MaxFreeMemSize);
}

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_kernel_memory_1(void) {
    sr_hle_register(0x8daff657, "sceKernelCreateTlspl", h_CreateTlspl);
    sr_hle_register(0x32bf938e, "sceKernelDeleteTlspl", h_DeleteTlspl);
    sr_hle_register(0x4a719fb2, "sceKernelFreeTlspl", h_FreeTlspl);
    sr_hle_register(0x721067f3, "sceKernelReferTlsplStatus", h_ReferTlsplStatus);
    sr_hle_register(0xfa835cde, "sceKernelGetTlsAddr", h_GetTlsAddr);
    sr_hle_register(0xc07bb470, "sceKernelCreateFpl", h_CreateFpl);
    sr_hle_register(0xd979e9bf, "sceKernelAllocateFpl", h_AllocateFpl);
    sr_hle_register(0xe7282cb6, "sceKernelAllocateFplCB", h_AllocateFplCB);
    sr_hle_register(0xf6414a71, "sceKernelFreeFpl", h_FreeFpl);
    /* Not a matrix NID -- waits.cpp never probes a Try form. It is here so the
     * selftest can pin, through production dispatch, that splitting the blocking
     * Allocate forms off this handler left it alone. Registered by this same
     * helper in both builds, so the production mapping is unchanged. */
    sr_hle_register(0x623ae665, "sceKernelTryAllocateFpl", h_TryAllocateFpl);
    /* Same reason, plus the selftest must hand every pool slot back: s_fpls[] holds
     * FPL_MAX=16 and the conformance matrix already uses all 16, so a test that
     * leaked one would starve the matrix rather than fail on its own assertion. */
    sr_hle_register(0xed1410e0, "sceKernelDeleteFpl", h_DeleteFpl);
    sr_hle_register(0xd8199e4c, "sceKernelReferFplStatus", h_ReferFplStatus);
    /* VPL set (PSP_INTR_WAITS_MATRIX.md PR-G coverage rows; blocking AllocateVpl
     * forms registered with fiber wait queues). */
    sr_hle_register(0x56c039b5, "sceKernelCreateVpl", h_CreateVpl);
    sr_hle_register(0x89b3d48c, "sceKernelDeleteVpl", h_DeleteVpl);
    sr_hle_register(0xaf36d708, "sceKernelTryAllocateVpl", h_TryAllocateVpl);
    sr_hle_register(0xbed27435, "sceKernelAllocateVpl", h_AllocateVpl);
    sr_hle_register(0xec0a693f, "sceKernelAllocateVplCB", h_AllocateVplCB);
    sr_hle_register(0xb736e9ff, "sceKernelFreeVpl", h_FreeVpl);
    sr_hle_register(0x39810265, "sceKernelReferVplStatus", h_ReferVplStatus);
}

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_kernel_memory_2(void) {
    sr_hle_register(0x3e0271d3, "sceKernelVolatileMemLock", h_VolatileMemLock);
    sr_hle_register(0xa14f40b2, "sceKernelVolatileMemTryLock", h_VolatileMemTryLock);
    sr_hle_register(0xa569e425, "sceKernelVolatileMemUnlock", h_VolatileMemUnlock);
}

#ifndef SR_MPEG_MEDIA_SELFTEST
#ifndef SR_HLE_THREAD_SELFTEST
/* From sr_hle_init(). */
void hle_register_init_kernel_memory_1(void) {
    sr_hle_register(0x7591c7db, "sceKernelSetCompiledSdkVersion", h_SetCompiledSdkVersion);
    sr_hle_register(0x35669d4c, "sceKernelSetCompiledSdkVersion600_602", h_SetCompiledSdkVersion);
    sr_hle_register(0x91de343c, "sceKernelSetCompiledSdkVersion500_505", h_SetCompiledSdkVersion);
    sr_hle_register(0xebd5c3e6, "sceKernelSetCompiledSdkVersion395", h_SetCompiledSdkVersion);
    sr_hle_register(0x358ca1bb, "sceKernelSetCompiledSdkVersion606", h_SetCompiledSdkVersion);
    sr_hle_register(0xf77d77cb, "sceKernelSetCompilerVersion", h_SetCompiledSdkVersion);
}
#endif
#endif

#ifndef SR_MPEG_MEDIA_SELFTEST
#ifndef SR_HLE_THREAD_SELFTEST
/* From sr_hle_init(). */
void hle_register_init_kernel_memory_2(void) {
    sr_hle_register(0x1b4217bc, "sceKernelSetCompiledSdkVersion603_605", h_SetCompiledSdkVersion);
}
#endif
#endif
